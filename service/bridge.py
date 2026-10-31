#!/usr/bin/env python3
"""Stream read-only HUD snapshots to HudBuddy over USB JSON Lines."""
from __future__ import annotations

import argparse
import datetime as dt
import json
import logging
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import time
from typing import Any

from service.magpie import DEFAULT_STATE_PATH, MAX_TIME_MS, MagpieClient, MagpieError, business_value, empty_quota

PROTOCOL = 1
MAX_LINE_BYTES = 100 * 1024  # Includes the final LF.
MAX_TASKS = 64
DEVICE_SERIAL_PATH = Path(__file__).resolve().parents[1] / ".state/device-serial"


def configured_serial() -> str:
    value = os.environ.get("HUDBUDDY_DEVICE_SERIAL")
    if value is None:
        try:
            value = DEVICE_SERIAL_PATH.read_text(encoding="utf-8")
        except FileNotFoundError:
            value = ""
    return value.strip().upper()


DEVICE_SERIAL = configured_serial()
DEFAULT_CLI = Path.home() / ".local/bin/hud-cli"
DEFAULT_COMPATIBILITY = Path.home() / ".codex/skills/hud/scripts/check_compatibility.py"
STATUS = {
    "doing": ("进行中", "#a5c4ee", "#2c3c52"),
    "waiting": ("等待中", "#e2c48b", "#443a27"),
}
LOG = logging.getLogger("hudbuddy")


class BridgeError(Exception):
    pass


def encode_line(message: dict[str, Any]) -> bytes:
    line = (json.dumps(message, ensure_ascii=False, separators=(",", ":"), allow_nan=False) + "\n").encode("utf-8")
    if len(line) > MAX_LINE_BYTES:
        raise BridgeError(f"snapshot exceeds {MAX_LINE_BYTES} bytes including LF ({len(line)} bytes); keeping previous snapshot")
    return line


def clock_fields() -> dict[str, int]:
    offset = dt.datetime.now().astimezone().utcoffset()
    return {"unix_ms": time.time_ns() // 1_000_000, "tz_offset_min": int(offset.total_seconds() // 60) if offset else 0}


def message(kind: str, **fields: Any) -> dict[str, Any]:
    return {"type": kind, "protocol": PROTOCOL, **clock_fields(), **fields}


def readable_summary(text: str) -> str:
    """Remove unambiguous Markdown decoration; keep all textual content.

    Deliberately leaves complex/nested Markdown intact rather than losing words.
    Only simple inline link targets are removed; visible labels/alt text remain.
    """
    text = re.sub(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]", "", text.replace("\r\n", "\n").replace("\r", "\n"))
    lines = []
    fenced = False
    for line in text.split("\n"):
        if re.fullmatch(r"\s*(```+|~~~+)[A-Za-z0-9_+.-]*\s*", line):
            fenced = not fenced
            continue
        if not fenced:
            line = re.sub(r"!?\[([^\[\]\n]*)\]\((?:https?://|mailto:)[^\s()]+\)", r"\1", line)
            line = re.sub(r"(?<!`)`([^`\n]+)`(?!`)", r"\1", line)
            line = re.sub(r"^(\s{0,3})#{1,6}\s+", r"\1", line)
            line = re.sub(r"^(\s*)[-+*]\s+\[ \]\s+", r"\1☐ ", line)
            line = re.sub(r"^(\s*)[-+*]\s+\[[xX]\]\s+", r"\1☑ ", line)
            line = re.sub(r"^(\s*)[-+*]\s+", r"\1• ", line)
            line = re.sub(r"^\s{0,3}> ?", "", line)
        lines.append(line)
    return "\n".join(lines)


def project_tasks(data: Any) -> list[dict[str, Any]]:
    if not isinstance(data, dict) or not isinstance(data.get("tasks"), list):
        raise BridgeError("list_tasks returned invalid tasks; keeping previous snapshot")
    tasks = []
    # list_tasks already applies HUD's canonical ordering. Never re-sort by status/date.
    for card in data["tasks"]:
        if not isinstance(card, dict) or not isinstance(card.get("task"), dict):
            raise BridgeError("list_tasks returned an invalid task card")
        task = card["task"]
        if not isinstance(task.get("status"), str):
            raise BridgeError("list_tasks returned invalid task status")
        if task.get("status") not in STATUS:
            continue
        if any(not isinstance(task.get(key), str) for key in ("id", "title", "summary", "status")):
            raise BridgeError("list_tasks returned invalid task text")
        if not task["id"] or type(task.get("updated_at")) is not int:
            raise BridgeError("list_tasks returned invalid task id/updated_at")
        label, color, background = STATUS[task["status"]]
        tasks.append({"id": task["id"], "title": task["title"], "status": task["status"],
                      "summary": readable_summary(task["summary"]), "updated_at": task["updated_at"],
                      "status_label": label, "status_color": color, "status_background": background})
    if len(tasks) > MAX_TASKS:
        raise BridgeError(f"{len(tasks)} active tasks exceeds device limit {MAX_TASKS}; keeping previous snapshot")
    return tasks


class HudClient:
    def __init__(self, cli: Path, timeout: float = 1.0, compatibility: Path = DEFAULT_COMPATIBILITY):
        self.cli = cli
        self.timeout = timeout
        self.compatibility = compatibility

    def _run_json(self, argv: list[str], *, payload: Any = None, timeout: float | None = None) -> Any:
        try:
            result = subprocess.run(argv, input=json.dumps(payload, ensure_ascii=False) if payload is not None else None,
                                    capture_output=True, text=True, encoding="utf-8", timeout=timeout or self.timeout)
        except (OSError, subprocess.SubprocessError, UnicodeError) as error:
            raise BridgeError(f"HUD CLI failed: {error}") from error
        try:
            envelope = json.loads(result.stdout)
        except (ValueError, TypeError) as error:
            raise BridgeError(f"HUD CLI returned invalid JSON (exit {result.returncode}): {result.stderr.strip()[:240]}") from error
        if result.returncode or not isinstance(envelope, dict) or envelope.get("ok") is not True:
            detail = envelope.get("error", envelope.get("message", "unknown error")) if isinstance(envelope, dict) else "invalid envelope"
            raise BridgeError(f"HUD CLI failed (exit {result.returncode}): {detail}")
        return envelope

    def check_compatibility(self) -> None:
        if not self.cli.is_absolute() or not self.cli.is_file():
            raise BridgeError("--cli must name an existing absolute hud-cli path")
        if not self.compatibility.is_file():
            raise BridgeError(f"HUD compatibility checker missing: {self.compatibility}")
        result = self._run_json([sys.executable, str(self.compatibility), "--cli", str(self.cli)], timeout=35)
        if result.get("update_available"):
            LOG.warning("HUD skill and bundled skill differ: %s", result.get("message"))
        LOG.info("HUD compatibility: CLI %s, API %s", result.get("cli_version"), result.get("api_version"))

    def fetch(self) -> list[dict[str, Any]]:
        envelope = self._run_json([str(self.cli), "request", "--stdin"], payload={"op": "list_tasks", "args": {"filter": "all"}})
        response = envelope.get("response")
        if not isinstance(response, dict) or response.get("op") != "list_tasks":
            raise BridgeError("HUD CLI returned an unexpected response")
        return project_tasks(response.get("data"))


def discover_port() -> str | None:
    if not DEVICE_SERIAL:
        raise BridgeError("Set HUDBUDDY_DEVICE_SERIAL or .state/device-serial before connecting")
    from serial.tools import list_ports
    matches = [p.device for p in list_ports.comports() if p.vid == 0x303A and p.pid == 0x1001
               and (p.serial_number or "").upper() == DEVICE_SERIAL]
    # macOS exposes a tty and cu counterpart; use the outgoing cu endpoint.
    return sorted(matches, key=lambda path: (not path.startswith("/dev/cu."), path))[0] if matches else None


def open_serial(port: str):
    import serial
    import termios

    class QuietSerial(serial.Serial):
        # POSIX pyserial normally asserts both lines when opening. Preserve them.
        def _update_dtr_state(self):
            pass

        def _update_rts_state(self):
            pass

        def _reconfigure_port(self, force_update=False):
            super()._reconfigure_port(force_update)
            attrs = termios.tcgetattr(self.fd)
            attrs[2] &= ~termios.HUPCL  # Closing the descriptor must not hang up DTR.
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)

    return QuietSerial(port=port, baudrate=115200, timeout=0, write_timeout=1,
                       rtscts=False, dsrdtr=False, exclusive=True)


class Bridge:
    ACK_TIMEOUT = 6.0
    RETRY_DELAYS = (0.5, 1.0, 2.0, 4.0, 30.0)

    def __init__(self, client: HudClient, port: str | None = None,
                 poll_interval: float = 60, heartbeat_interval: float = 600,
                 port_finder=discover_port, serial_opener=open_serial, clock=time.monotonic,
                 quota_client=None):
        self.client, self.port = client, port
        self.quota_client = quota_client
        self.poll_interval, self.heartbeat_interval = poll_interval, heartbeat_interval
        self.port_finder, self.serial_opener, self.clock = port_finder, serial_opener, clock
        self.serial = None
        self.physical_online = False
        self.poll_due: float | None = None
        self.heartbeat_due: float | None = None
        self.retry_due: float | None = None
        self.retry_index = 0
        self.tasks: list[dict[str, Any]] | None = None
        self.hud_ok = False
        self.quota: dict[str, Any] | None = None
        if quota_client is not None and getattr(quota_client, "cached_quota", None) is not None:
            self.quota = quota_client.failure_quota()
        self.quota_error: str | None = None
        self.seq = 0
        self.pending_seq: int | None = None
        self.pending_since = 0.0
        self.needs_snapshot = False
        self.rx = bytearray()
        self.discard_line = False
        self.last_error: str | None = None
        self.last_connection_error: str | None = None

    def close(self) -> None:
        if self.serial is not None:
            try:
                self.serial.close()
            except OSError:
                pass
        self.serial = None
        self.pending_seq = None
        self.poll_due = self.heartbeat_due = None
        self.rx.clear()
        self.discard_line = False

    def set_presence(self, online: bool) -> None:
        if online == self.physical_online:
            return
        self.physical_online = online
        self.close()
        self.retry_index = 0
        self.retry_due = self.clock() if online else None
        LOG.info("target USB %s; %s", "attached" if online else "removed",
                 "connecting" if online else "HUD, quota and heartbeat timers stopped")

    def snapshot(self, **overrides: Any) -> dict[str, Any]:
        fields: dict[str, Any] = {"hud_ok": self.hud_ok}
        if self.tasks is not None:
            fields["tasks"] = self.tasks
        if self.quota is not None:
            fields["quota"] = self.quota
        fields.update(overrides)
        return message("snapshot", seq=self.seq + 1, **fields)

    def refresh(self) -> bool:
        LOG.debug("checking HUD tasks")
        try:
            tasks = self.client.fetch()
            candidate = self.snapshot(tasks=tasks, hud_ok=False)  # Reserve the longer failed-health flag too.
            if self.quota_client is not None and "quota" not in candidate:
                candidate["quota"] = empty_quota(stale=True)
                candidate["quota"].update(last_success_ms=MAX_TIME_MS, monitor_started_ms=MAX_TIME_MS)
            encode_line(candidate)
        except (BridgeError, ValueError, TypeError) as error:
            detail = str(error)
            if detail != self.last_error:
                LOG.error("HUD refresh failed; retaining previous snapshot: %s", detail)
            self.last_error = detail
            if self.hud_ok:
                self.needs_snapshot = True
            self.hud_ok = False
            return False
        if self.last_error is not None:
            LOG.info("HUD refresh recovered")
        self.last_error = None
        changed = tasks != self.tasks or not self.hud_ok
        self.tasks = tasks
        self.hud_ok = True
        if changed:
            self.needs_snapshot = True
        LOG.debug("HUD checked: %d tasks, changed=%s", len(tasks), changed)
        return changed

    def refresh_quota(self) -> bool:
        if self.quota_client is None:
            return False
        LOG.debug("checking Magpie quota")
        try:
            quota = self.quota_client.fetch()
            encode_line(self.snapshot(quota=quota))
        except (MagpieError, BridgeError, ValueError, TypeError) as error:
            detail = str(error)
            if detail != self.quota_error:
                LOG.error("Magpie refresh failed; retaining previous quota: %s", detail)
            self.quota_error = detail
            quota = dict(self.quota) if self.quota is not None else empty_quota(stale=True)
            for name in ("last_success_ms", "monitor_started_ms"):
                if hasattr(self.quota_client, name):
                    quota[name] = getattr(self.quota_client, name)
            quota["stale"] = True
        else:
            if self.quota_error is not None:
                LOG.info("Magpie refresh recovered")
            self.quota_error = None
        changed = business_value(quota) != business_value(self.quota)
        self.quota = quota
        if changed:
            self.needs_snapshot = True
        LOG.debug("Magpie checked: available=%s stale=%s changed=%s", quota["available"], quota["stale"], changed)
        return changed

    def _send(self, payload: dict[str, Any]) -> None:
        wire = encode_line(payload)
        if self.serial.write(wire) != len(wire):
            raise OSError("incomplete USB write")

    def send_snapshot(self) -> None:
        if self.serial is None or (self.tasks is None and self.quota is None) or not self.needs_snapshot:
            return
        next_seq = self.seq + 1
        self._send(self.snapshot())
        self.seq = next_seq
        if self.pending_seq is None:
            self.pending_since = self.clock()  # Full write completed; device ACK time starts now.
        self.pending_seq = next_seq
        self.needs_snapshot = False
        LOG.info("sent snapshot seq=%d tasks=%s quota=%s", self.seq,
                 len(self.tasks) if self.tasks is not None else "unchanged", self.quota is not None)

    def send_heartbeat(self) -> None:
        fields = {}
        if self.quota_client is not None:
            fields = {"magpie_last_success_ms": self.quota.get("last_success_ms") if self.quota else None,
                      "magpie_monitor_started_ms": self.quota.get("monitor_started_ms") if self.quota else None}
        self._send(message("heartbeat", **fields))
        LOG.debug("sent heartbeat")

    def connect(self) -> bool:
        try:
            port = self.port or self.port_finder()
            if port is None:
                raise OSError("target USB is present; waiting for serial node")
            self.serial = self.serial_opener(port)
            self.needs_snapshot = True
            self.last_connection_error = None
            LOG.info("connected %s", port)
            # Initial heartbeat supplies the clock even when the first HUD read fails.
            # Its LF also discards any partial frame left by a failed prior write.
            self.send_heartbeat()
            now = self.clock()
            self.poll_due = now  # Every successful open immediately re-reads both sources.
            self.heartbeat_due = now + self.heartbeat_interval
            self.retry_due = None
            return True
        except (OSError, BridgeError) as error:
            self.connection_failed(error)
            return False

    def connection_failed(self, error: Exception) -> None:
        self.close()
        detail = str(error)
        if self.physical_online:
            delay = self.RETRY_DELAYS[min(self.retry_index, len(self.RETRY_DELAYS) - 1)]
            self.retry_index += 1
            self.retry_due = self.clock() + delay
            LOG.warning("USB transport failed: %s; target still present, retry in %.1fs", detail, delay)
        else:
            self.retry_due = None
            if detail != self.last_connection_error:
                LOG.warning("USB transport failed: %s; waiting for target USB arrival", detail)
        self.last_connection_error = detail

    def read_device(self) -> None:
        if self.serial is None:
            return
        try:
            chunk = self.serial.read(4096)
            for byte in chunk:
                if self.discard_line:
                    if byte == 10:
                        self.discard_line = False
                    continue
                self.rx.append(byte)
                if len(self.rx) > MAX_LINE_BYTES:
                    self.rx.clear()
                    self.discard_line = byte != 10
                    LOG.warning("discarded oversized USB input line")
                    continue
                if byte != 10:
                    continue
                raw = bytes(self.rx)
                self.rx.clear()
                try:
                    incoming = json.loads(raw)
                except (ValueError, UnicodeError):
                    continue
                if not isinstance(incoming, dict):
                    continue
                if incoming.get("type") == "hello" and incoming.get("protocol") == PROTOCOL and incoming.get("device") == "HudBuddy":
                    # Startup hello often arrives after our initial snapshot. Do not
                    # duplicate an in-flight snapshot; a later hello denotes a reset.
                    if self.pending_seq is None:
                        self.needs_snapshot = True
                elif incoming.get("type") == "ack" and type(incoming.get("seq")) is int and incoming["seq"] == self.pending_seq:
                    LOG.debug("acked seq=%d", self.pending_seq)
                    self.pending_seq = None
                    self.retry_index = 0  # Only an ACK proves successful recovery.
        except (OSError, BridgeError) as error:
            self.connection_failed(error)

    def step(self) -> None:
        """Run due work. Offline has no timers, HUD reads, or port discovery."""
        if not self.physical_online:
            return
        now = self.clock()
        if self.serial is None:
            if self.retry_due is None or now < self.retry_due or not self.connect():
                return
        self.read_device()
        if self.serial is None:
            return
        now = self.clock()
        if self.pending_seq is not None and now - self.pending_since >= self.ACK_TIMEOUT:
            self.connection_failed(OSError(f"snapshot ack timed out (seq={self.pending_seq})"))
            return
        try:
            if self.heartbeat_due is not None and now >= self.heartbeat_due:
                self.send_heartbeat()
                self.heartbeat_due = self.clock() + self.heartbeat_interval
            if self.poll_due is not None and now >= self.poll_due:
                self.refresh()
                self.refresh_quota()
                self.poll_due = self.clock() + self.poll_interval
            self.send_snapshot()
        except (OSError, BridgeError) as error:
            self.connection_failed(error)

    def wait_timeout(self) -> float | None:
        if not self.physical_online:
            return None  # Block on IOKit Event until target Buddy arrives.
        if self.serial is None:
            return max(0, self.retry_due - self.clock()) if self.retry_due is not None else None
        deadlines = [due for due in (self.poll_due, self.heartbeat_due) if due is not None]
        if self.pending_seq is not None:
            deadlines.append(self.pending_since + self.ACK_TIMEOUT)
        return min(0.05, max(0, min(deadlines) - self.clock())) if deadlines else 0.05

    def run(self, monitor=None) -> None:
        from service.usb_events import USBMonitor, USBEventError
        monitor = monitor or USBMonitor(0x303A, 0x1001, DEVICE_SERIAL)
        try:
            monitor.start()
            while True:
                for online in monitor.take_changes():
                    self.set_presence(online)
                self.step()
                monitor.event.wait(self.wait_timeout())
        except USBEventError as error:
            raise BridgeError(str(error)) from error
        finally:
            self.close()
            monitor.close()


def main(argv: list[str] | None = None) -> int:
    global DEVICE_SERIAL
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device-serial", default=DEVICE_SERIAL, help="target USB serial; defaults to HUDBUDDY_DEVICE_SERIAL or .state/device-serial")
    parser.add_argument("--port", help="explicit USB serial path; otherwise discover this HudBuddy by VID/PID/serial")
    parser.add_argument("--cli", type=Path, default=DEFAULT_CLI, help="absolute installed hud-cli path")
    parser.add_argument("--poll-interval", "--interval", dest="poll_interval", type=float, default=60, help="HUD/Magpie poll seconds (default: 60; --interval is an alias)")
    parser.add_argument("--heartbeat-interval", type=float, default=600, help="heartbeat seconds (default: 600)")
    parser.add_argument("--debug", action="store_true", help="log HUD checks, heartbeats, and USB registry events")
    parser.add_argument("--once", action="store_true", help="read and validate once without connecting USB")
    parser.add_argument("--stdout", action="store_true", help="emit the one-shot snapshot JSONL (requires --once)")
    args = parser.parse_args(argv)
    DEVICE_SERIAL = args.device_serial.strip().upper()
    if not args.once and not DEVICE_SERIAL:
        parser.error("set --device-serial, HUDBUDDY_DEVICE_SERIAL or .state/device-serial")
    for name in ("poll_interval", "heartbeat_interval"):
        if not math.isfinite(getattr(args, name)) or getattr(args, name) < 1:
            parser.error("--" + name.replace("_", "-") + " must be at least 1 second")
    if args.stdout and not args.once:
        parser.error("--stdout requires --once")
    logging.basicConfig(level=logging.DEBUG if args.debug else logging.INFO, format="%(asctime)s %(levelname)s %(message)s", stream=sys.stderr)
    client = HudClient(args.cli.expanduser(), timeout=1)
    quota_client = MagpieClient(state_path=DEFAULT_STATE_PATH)
    try:
        client.check_compatibility()
        if args.once:
            bridge = Bridge(client, quota_client=quota_client)
            bridge.refresh()
            bridge.refresh_quota()
            wire = encode_line(bridge.snapshot())
            if args.stdout:
                sys.stdout.buffer.write(wire)
            else:
                LOG.info("snapshot valid: tasks=%s quota=%s, %d bytes",
                         len(bridge.tasks) if bridge.tasks is not None else "unavailable", bridge.quota["available"], len(wire))
            if not bridge.hud_ok or bridge.quota_error:
                return 1
        else:
            Bridge(client, port=args.port, poll_interval=args.poll_interval, heartbeat_interval=args.heartbeat_interval,
                   quota_client=quota_client).run()
    except KeyboardInterrupt:
        return 0
    except (BridgeError, ImportError) as error:
        LOG.error("%s", error)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
