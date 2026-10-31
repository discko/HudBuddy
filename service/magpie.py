"""Read Codex quota from the local Magpie Gateway. GET only; no reset actions."""
from __future__ import annotations

import datetime as dt
import copy
import http.client
import json
import logging
import math
import os
from pathlib import Path
import tempfile
import time
from typing import Any
import urllib.error
import urllib.request

URL = "http://127.0.0.1:3425/v1/magpie/quotas"
MAX_RESPONSE_BYTES = 1024 * 1024
MAX_CARDS = 64
MAX_TIME_MS = 253402300799999  # End of year 9999 UTC.
TEXT_LIMITS = {"account": 512, "plan": 128}
DEFAULT_STATE_PATH = Path(__file__).resolve().parents[1] / ".state/magpie-state.json"
LOG = logging.getLogger("hudbuddy.magpie")


class MagpieError(RuntimeError):
    pass


def empty_quota(*, stale: bool = False) -> dict[str, Any]:
    return {"available": False, "stale": stale, "account": "", "plan": "", "read_at_ms": 0,
            "last_success_ms": None, "monitor_started_ms": None,
            "weekly": None, "five_hour": None, "cards": None}


def iso_ms(value: Any) -> int | None:
    if not isinstance(value, str) or not value:
        return None
    try:
        parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
        if parsed.tzinfo is None:
            return None  # Do not guess a timezone for an incomplete source date.
        delta = parsed.astimezone(dt.timezone.utc) - dt.datetime(1970, 1, 1, tzinfo=dt.timezone.utc)
        milliseconds = delta.days * 86400000 + delta.seconds * 1000 + delta.microseconds // 1000
        return milliseconds if 0 <= milliseconds <= MAX_TIME_MS else None
    except (ValueError, OverflowError, OSError):
        return None


def percent(value: Any) -> int | float | None:
    if type(value) not in (int, float) or not 0 <= value <= 100 or not math.isfinite(value):
        return None
    return value


def quota_text(value: Any, field: str) -> str:
    value = value if isinstance(value, str) else ""
    try:
        if len(value.encode("utf-8")) > TEXT_LIMITS[field]:
            raise MagpieError(f"Magpie {field} exceeds {TEXT_LIMITS[field]} UTF-8 bytes")
    except UnicodeError as error:
        raise MagpieError(f"Magpie {field} is not valid UTF-8") from error
    return value


def normalize_cards(resets: Any) -> dict[str, Any] | None:
    if not isinstance(resets, dict):
        return None
    count = resets.get("count")
    each = resets.get("each")
    if (type(count) is int and count > MAX_CARDS) or (isinstance(each, list) and len(each) > MAX_CARDS):
        raise MagpieError(f"Magpie reset card count exceeds device limit {MAX_CARDS}")
    if type(count) is not int or count < 0:
        return None  # Missing count is unknown; never infer it from valid dates.
    if each is not None and not isinstance(each, list):
        raise MagpieError("Magpie resets.each must be a list")
    if isinstance(each, list) and len(each) > count:
        raise MagpieError("Magpie resets.each has more entries than count")
    if isinstance(each, list):
        dates = [iso_ms(card.get("until")) if isinstance(card, dict) else None for card in each]
    elif count == 1:
        dates = [iso_ms(resets.get("until"))]
    else:
        dates = []
    # Known expiries earliest first, unknown dates last. Keep unknown entries.
    dates.sort(key=lambda date: (date is None, date or 0))
    return {"count": count, "expires_at_ms": dates}


def normalize_quota(envelope: Any) -> dict[str, Any]:
    if not isinstance(envelope, dict) or not isinstance(envelope.get("data"), list):
        raise MagpieError("Magpie returned invalid quota data")
    if any(not isinstance(entry, dict) for entry in envelope["data"]):
        raise MagpieError("Magpie returned an invalid account entry")
    candidates = [entry for entry in envelope["data"] if isinstance(entry, dict)
                  and entry.get("provider") == "codex" and entry.get("kind") == "subscription"]
    current = [entry for entry in candidates if entry.get("last") is True]
    if len(candidates) > 1:
        if len(current) != 1:
            raise MagpieError("Magpie has ambiguous Codex accounts; exactly one last:true is required")
        selected = current[0]
    elif candidates:
        selected = candidates[0]
    else:
        return empty_quota()
    windows = selected.get("windows", [])
    if not isinstance(windows, list):
        raise MagpieError("Magpie windows must be a list")
    weekly = five_hour = None
    for window in windows:
        if not isinstance(window, dict):
            raise MagpieError("Magpie returned an invalid window")
        name = "".join(str(window.get("name", "")).lower().replace("-", " ").split())
        projected = {"remaining": percent(window.get("remaining")), "resets_at_ms": iso_ms(window.get("resetsAt"))}
        if name in {"7days", "7day", "7d", "1week", "weekly"} and weekly is None:
            weekly = projected
        elif name in {"5hours", "5hour", "5h"} and five_hour is None:
            five_hour = projected
    return {"available": True, "stale": False,
            "account": quota_text(selected.get("user"), "account"),
            "plan": quota_text(selected.get("plan"), "plan"),
            "read_at_ms": iso_ms(selected.get("readAt")) or 0,
            "last_success_ms": None, "monitor_started_ms": None,
            "weekly": weekly, "five_hour": five_hour, "cards": normalize_cards(selected.get("resets"))}


def business_value(quota: dict[str, Any] | None) -> dict[str, Any] | None:
    ignored = {"read_at_ms", "last_success_ms", "monitor_started_ms"}
    return {key: value for key, value in quota.items() if key not in ignored} if quota is not None else None


def _time_value(value: Any) -> bool:
    return type(value) is int and 0 <= value <= MAX_TIME_MS


def _cached_quota(raw: Any) -> dict[str, Any]:
    """Validate and project only our normalized fields; never forward arbitrary cache data."""
    if not isinstance(raw, dict) or type(raw.get("available")) is not bool:
        raise ValueError("invalid cached quota")
    result = empty_quota()
    result["available"] = raw["available"]
    for name in ("account", "plan"):
        if not isinstance(raw.get(name), str):
            raise ValueError(f"invalid cached {name}")
        result[name] = quota_text(raw[name], name)
    if not _time_value(raw.get("read_at_ms")):
        raise ValueError("invalid cached read_at_ms")
    result["read_at_ms"] = raw["read_at_ms"]
    for name in ("weekly", "five_hour"):
        window = raw.get(name)
        if window is None:
            continue
        if not isinstance(window, dict):
            raise ValueError("invalid cached window")
        remaining, resets_at = window.get("remaining"), window.get("resets_at_ms")
        if ((remaining is not None and percent(remaining) is None)
                or (resets_at is not None and not _time_value(resets_at))):
            raise ValueError("invalid cached window values")
        result[name] = {"remaining": remaining, "resets_at_ms": resets_at}
    cards = raw.get("cards")
    if cards is not None:
        if (not isinstance(cards, dict) or type(cards.get("count")) is not int
                or not 0 <= cards["count"] <= MAX_CARDS or not isinstance(cards.get("expires_at_ms"), list)
                or len(cards["expires_at_ms"]) > cards["count"]
                or any(date is not None and not _time_value(date) for date in cards["expires_at_ms"])):
            raise ValueError("invalid cached cards")
        result["cards"] = {"count": cards["count"], "expires_at_ms": cards["expires_at_ms"]}
    return result


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None  # Fixed loopback endpoint must never redirect to another host.


class MagpieClient:
    def __init__(self, opener=None, state_path: Path | None = None, now_ms=None):
        self.opener = opener or urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
        self.state_path = state_path
        self.now_ms = now_ms or (lambda: time.time_ns() // 1_000_000)
        self.last_success_ms: int | None = None
        self.monitor_started_ms: int | None = None
        self.cached_quota: dict[str, Any] | None = None
        self._load_state()

    def _load_state(self) -> None:
        if self.state_path is None:
            return
        try:
            with self.state_path.open("rb") as handle:
                body = handle.read(MAX_RESPONSE_BYTES + 1)
            if len(body) > MAX_RESPONSE_BYTES:
                raise ValueError("quota cache exceeds size limit")
            raw = json.loads(body)
            if not isinstance(raw, dict) or raw.get("version") != 1:
                raise ValueError("invalid quota cache version")
            last, started = raw.get("last_success_ms"), raw.get("monitor_started_ms")
            if any(value is not None and not _time_value(value) for value in (last, started)):
                raise ValueError("invalid quota cache time")
            quota = _cached_quota(raw["quota"]) if raw.get("quota") is not None else None
            self.last_success_ms, self.monitor_started_ms, self.cached_quota = last, started, quota
            if self.cached_quota is not None:
                self.cached_quota.update(last_success_ms=last, monitor_started_ms=started)
        except FileNotFoundError:
            pass
        except (OSError, ValueError, TypeError, KeyError, MagpieError) as error:
            LOG.error("cannot load Magpie state: %s", error)

    def _save_state(self) -> None:
        if self.state_path is None:
            return
        temp_path = None
        try:
            self.state_path.parent.mkdir(parents=True, exist_ok=True)
            descriptor, name = tempfile.mkstemp(prefix=".magpie-state-", suffix=".tmp", dir=self.state_path.parent)
            temp_path = Path(name)
            with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
                os.fchmod(handle.fileno(), 0o600)
                json.dump({"version": 1, "last_success_ms": self.last_success_ms,
                           "monitor_started_ms": self.monitor_started_ms, "quota": self.cached_quota},
                          handle, ensure_ascii=False, separators=(",", ":"), allow_nan=False)
                handle.flush()
                os.fsync(handle.fileno())
            os.replace(temp_path, self.state_path)
        except (OSError, ValueError) as error:
            LOG.error("cannot persist Magpie state: %s", error)
        finally:
            if temp_path is not None:
                try:
                    temp_path.unlink(missing_ok=True)
                except OSError as error:
                    LOG.error("cannot remove temporary Magpie state: %s", error)

    def failure_quota(self) -> dict[str, Any]:
        quota = copy.deepcopy(self.cached_quota) if self.cached_quota is not None else empty_quota()
        quota.update(stale=True, last_success_ms=self.last_success_ms, monitor_started_ms=self.monitor_started_ms)
        return quota

    def fetch(self) -> dict[str, Any]:
        if self.monitor_started_ms is None:
            self.monitor_started_ms = self.now_ms()
            self._save_state()  # Preserve the first attempt even if Magpie has never replied.
        request = urllib.request.Request(URL, headers={"Accept": "application/json"}, method="GET")
        try:
            with self.opener.open(request, timeout=2) as response:
                body = response.read(MAX_RESPONSE_BYTES + 1)
            if len(body) > MAX_RESPONSE_BYTES:
                raise MagpieError(f"Magpie response exceeds {MAX_RESPONSE_BYTES} bytes")
            envelope = json.loads(body.decode("utf-8"))
        except (OSError, urllib.error.URLError, http.client.HTTPException, ValueError, UnicodeError) as error:
            raise MagpieError(f"Magpie GET failed: {error}") from error
        quota = normalize_quota(envelope)
        self.last_success_ms = self.now_ms()
        quota.update(last_success_ms=self.last_success_ms, monitor_started_ms=self.monitor_started_ms)
        self.cached_quota = copy.deepcopy(quota)
        self._save_state()
        return quota
