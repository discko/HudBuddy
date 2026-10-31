"""Local Magpie reads, normalized protocol, persistence and source isolation."""
import copy
import io
import json
from pathlib import Path
import stat
import tempfile
import unittest
from unittest.mock import Mock, patch

from service import bridge as b
from service import magpie as m


def account(**overrides):
    return {"provider": "codex", "kind": "subscription", "plan": "prolite", "user": "test@example.test",
            "windows": [{"name": "7 days", "used": 34, "remaining": 66, "resetsAt": "2026-11-06T12:30:00Z"}],
            "readAt": "2026-10-31T09:00:00Z", "resets": {"count": 1, "until": "2026-12-01T00:00:00Z",
                                                            "each": [{"until": "2026-12-01T00:00:00Z"}]}, **overrides}


def quota(**overrides):
    value = m.normalize_quota({"data": [account()]})
    value.update(last_success_ms=1793437200000, monitor_started_ms=1793437100000)
    value.update(overrides)
    return value


class FakeOpener:
    def __init__(self, payload=None, error=None):
        self.payload = payload if payload is not None else {"data": [account()]}
        self.error = error
        self.requests = []

    def open(self, request, timeout):
        self.requests.append((request, timeout))
        if self.error:
            raise self.error
        body = self.payload if isinstance(self.payload, bytes) else json.dumps(self.payload).encode()
        return io.BytesIO(body)


class NormalizationTests(unittest.TestCase):
    def test_weekly_only_does_not_infer_five_hour_or_unlimited(self):
        value = quota()
        self.assertEqual(value["weekly"], {"remaining": 66, "resets_at_ms": m.iso_ms("2026-11-06T12:30:00Z")})
        self.assertIsNone(value["five_hour"])
        self.assertEqual((value["available"], value["stale"], value["plan"]), (True, False, "prolite"))

    def test_explicit_five_hour_and_unknown_window(self):
        value = m.normalize_quota({"data": [account(windows=[{"name": "5 hours", "remaining": 12.5},
                                                                   {"name": "monthly", "remaining": 99}])]})
        self.assertEqual(value["five_hour"], {"remaining": 12.5, "resets_at_ms": None})
        self.assertIsNone(value["weekly"])

    def test_percent_values_are_finite_bounded_and_never_guessed_from_used(self):
        for value in (True, -1, 101, float("nan"), float("inf"), "66", 10 ** 1000):
            self.assertIsNone(m.percent(value))
        for value in (0, 100, 12.25):
            self.assertEqual(m.percent(value), value)
        value = m.normalize_quota({"data": [account(windows=[{"name": "7 days", "used": 34}])]})
        self.assertIsNone(value["weekly"]["remaining"])

    def test_dates_require_explicit_timezone_and_supported_range(self):
        self.assertEqual(m.iso_ms("2026-10-31T17:00:00+08:00"), m.iso_ms("2026-10-31T09:00:00Z"))
        self.assertEqual(m.iso_ms("1970-01-01T00:00:00Z"), 0)
        self.assertEqual(m.iso_ms("9999-12-31T23:59:59.999Z"), m.MAX_TIME_MS)
        for value in (None, "", "2026-10-31", "2026-10-31T09:00:00", "1969-12-31T23:59:59Z", "invalid", 123):
            self.assertIsNone(m.iso_ms(value), value)

    def test_cards_sort_earliest_known_keep_unknown_and_preserve_count(self):
        cards = m.normalize_cards({"count": 7, "each": [{"until": "2027-01-01T00:00:00Z"}, {},
                                                             {"until": "2026-11-01T00:00:00Z"}, {"until": "bad"}]})
        self.assertEqual(cards["count"], 7)
        self.assertEqual(cards["expires_at_ms"], [m.iso_ms("2026-11-01T00:00:00Z"),
                                                 m.iso_ms("2027-01-01T00:00:00Z"), None, None])

    def test_missing_count_dates_and_until_fallback(self):
        date = "2026-12-01T00:00:00Z"
        self.assertIsNone(m.normalize_cards({"each": [{"until": date}]}))
        self.assertEqual(m.normalize_cards({"count": 1, "until": date}), {"count": 1, "expires_at_ms": [m.iso_ms(date)]})
        self.assertEqual(m.normalize_cards({"count": 2, "until": date}), {"count": 2, "expires_at_ms": []})
        self.assertEqual(m.normalize_cards({"count": 1, "until": date, "each": []}), {"count": 1, "expires_at_ms": []})
        self.assertEqual(m.normalize_cards({"count": 1}), {"count": 1, "expires_at_ms": [None]})
        self.assertIsNone(m.normalize_cards({"count": True}))

    def test_card_limit_rejects_count_and_each_without_truncation(self):
        self.assertEqual(m.normalize_cards({"count": 64, "each": [{}] * 64})["count"], 64)
        for resets in ({"count": 65}, {"count": 1, "each": [{}] * 65}, {"each": [{}] * 65}):
            with self.assertRaisesRegex(m.MagpieError, "limit 64"):
                m.normalize_cards(resets)
        with self.assertRaisesRegex(m.MagpieError, "more entries than count"):
            m.normalize_cards({"count": 0, "each": [{}]})

    def test_account_and_plan_limits_count_utf8_bytes(self):
        for overrides in ({"user": "中" * 171}, {"plan": "x" * 129}):
            with self.assertRaisesRegex(m.MagpieError, "UTF-8 bytes"):
                m.normalize_quota({"data": [account(**overrides)]})

    def test_select_only_exact_codex_subscription_and_unique_current(self):
        other = account(provider="other", last=True)
        older = account(user="old@example.test")
        current = account(user="current@example.test", last=True)
        value = m.normalize_quota({"data": [other, older, current]})
        self.assertEqual(value["account"], "current@example.test")
        for entries in ([older, account()], [current, account(last=True)]):
            with self.assertRaisesRegex(m.MagpieError, "ambiguous"):
                m.normalize_quota({"data": entries})
        value = m.normalize_quota({"data": [other, account(provider="Codex"), account(kind="api_key")]})
        self.assertEqual(value, m.empty_quota())

    def test_business_comparison_ignores_only_observation_times(self):
        a = quota()
        c = copy.deepcopy(a)
        for key in ("read_at_ms", "last_success_ms", "monitor_started_ms"):
            c[key] += 10000
        self.assertEqual(m.business_value(a), m.business_value(c))
        c["stale"] = True
        self.assertNotEqual(m.business_value(a), m.business_value(c))


class HTTPAndPersistenceTests(unittest.TestCase):
    def test_fixed_loopback_get_timeout_and_proxy_bypass(self):
        opener = FakeOpener()
        client = m.MagpieClient(opener=opener, now_ms=lambda: 2000)
        value = client.fetch()
        request, timeout = opener.requests[0]
        self.assertEqual((request.full_url, request.method, request.data, timeout), (m.URL, "GET", None, 2))
        self.assertEqual((value["last_success_ms"], value["monitor_started_ms"]), (2000, 2000))
        with patch("service.magpie.urllib.request.build_opener") as build:
            m.MagpieClient()
        self.assertEqual(build.call_args.args[0].proxies, {})
        self.assertIsInstance(build.call_args.args[1], m.NoRedirect)
        self.assertIsNone(m.NoRedirect().redirect_request(None, None, 302, "redirect", {}, "https://remote.invalid/"))

    def test_errors_invalid_schema_and_response_cap(self):
        for opener in (FakeOpener(error=TimeoutError("slow")), FakeOpener(payload=b"not json"),
                       FakeOpener(payload={"data": None}), FakeOpener(payload={"data": [None]}),
                       FakeOpener(payload=b"x" * (m.MAX_RESPONSE_BYTES + 1))):
            with self.subTest(opener=opener), self.assertRaises(m.MagpieError):
                m.MagpieClient(opener=opener).fetch()

    def test_atomic_private_cache_restores_success_after_restart(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "hud/magpie-state.json"
            client = m.MagpieClient(opener=FakeOpener(), state_path=path, now_ms=lambda: 1000)
            good = client.fetch()
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
            restored = m.MagpieClient(opener=FakeOpener(error=OSError("offline")), state_path=path, now_ms=lambda: 2000)
            with self.assertRaises(m.MagpieError):
                restored.fetch()
            fallback = restored.failure_quota()
            self.assertEqual(fallback["weekly"], good["weekly"])
            self.assertEqual((fallback["last_success_ms"], fallback["monitor_started_ms"], fallback["stale"]), (1000, 1000, True))
            self.assertEqual(list(path.parent.glob("*.tmp")), [])

    def test_never_connected_start_time_survives_restart(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            client = m.MagpieClient(opener=FakeOpener(error=OSError("offline")), state_path=path, now_ms=lambda: 1000)
            with self.assertRaises(m.MagpieError):
                client.fetch()
            restored = m.MagpieClient(opener=FakeOpener(error=OSError("offline")), state_path=path, now_ms=lambda: 100000)
            with self.assertRaises(m.MagpieError):
                restored.fetch()
            fallback = restored.failure_quota()
            self.assertEqual(fallback["monitor_started_ms"], 1000)
            self.assertIsNone(fallback["last_success_ms"])
            self.assertFalse(fallback["available"])

    def test_no_account_success_is_still_successful_contact_and_persisted(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            value = m.MagpieClient(opener=FakeOpener(payload={"data": []}), state_path=path, now_ms=lambda: 3000).fetch()
            self.assertEqual((value["available"], value["stale"], value["last_success_ms"]), (False, False, 3000))
            restored = m.MagpieClient(opener=FakeOpener(), state_path=path)
            self.assertEqual(restored.failure_quota()["last_success_ms"], 3000)

    def test_failed_atomic_replace_keeps_prior_valid_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            opener = FakeOpener()
            client = m.MagpieClient(opener=opener, state_path=path, now_ms=lambda: 1000)
            client.fetch()
            original = path.read_bytes()
            opener.payload = {"data": [account(plan="new-plan")]}
            with patch("service.magpie.os.replace", side_effect=OSError("cannot replace")), self.assertLogs("hudbuddy.magpie", "ERROR"):
                self.assertEqual(client.fetch()["plan"], "new-plan")
            self.assertEqual(path.read_bytes(), original)
            self.assertEqual(list(path.parent.glob("*.tmp")), [])

    def test_invalid_cached_card_count_and_text_are_rejected(self):
        for value in (quota(cards={"count": 0, "expires_at_ms": [1000]}), quota(account="x" * 513)):
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "state.json"
                path.write_text(json.dumps({"version": 1, "quota": value,
                                            "last_success_ms": 1000, "monitor_started_ms": 1000}))
                with self.assertLogs("hudbuddy.magpie", "ERROR"):
                    client = m.MagpieClient(opener=FakeOpener(), state_path=path)
                self.assertIsNone(client.cached_quota)
                self.assertIsNone(client.last_success_ms)


class BridgeQuotaTests(unittest.TestCase):
    def setUp(self):
        self.now = 0
        self.hud = Mock()
        self.tasks = b.project_tasks({"tasks": [{"task": {"id": "a", "title": "任务", "summary": "说明",
                                                         "status": "doing", "updated_at": 1000}}]})
        self.hud.fetch.return_value = self.tasks
        self.magpie = Mock(cached_quota=None, spec=["fetch", "cached_quota"])
        self.magpie.fetch.return_value = quota()
        self.port = Mock()
        self.port.read.return_value = b""
        self.writes = []

        def write(wire):
            self.writes.append(json.loads(wire))
            return len(wire)

        self.port.write.side_effect = write
        self.bridge = b.Bridge(self.hud, quota_client=self.magpie, clock=lambda: self.now,
                               port_finder=lambda: "/dev/fake", serial_opener=lambda _: self.port)

    def step(self, now):
        self.now = now
        self.bridge.step()

    def attach(self):
        self.bridge.set_presence(True)
        self.bridge.step()

    def ack(self):
        self.port.read.return_value = b.encode_line({"type": "ack", "seq": self.bridge.pending_seq})
        self.bridge.step()
        self.port.read.return_value = b""

    def test_offline_never_queries_either_source(self):
        for now in (0, 60, 600, 100000):
            self.step(now)
        self.hud.fetch.assert_not_called()
        self.magpie.fetch.assert_not_called()

    def test_initial_hud_failure_omits_tasks_and_does_not_block_quota(self):
        self.hud.fetch.side_effect = b.BridgeError("HUD unavailable")
        with self.assertLogs("hudbuddy", "ERROR"):
            self.attach()
        snapshot = self.writes[-1]
        self.assertNotIn("tasks", snapshot)
        self.assertFalse(snapshot["hud_ok"])
        self.assertTrue(snapshot["quota"]["available"])

    def test_initial_both_failures_send_explicit_unavailable_without_empty_tasks(self):
        self.hud.fetch.side_effect = b.BridgeError("HUD unavailable")
        self.magpie.fetch.side_effect = m.MagpieError("Magpie unavailable")
        with self.assertLogs("hudbuddy", "ERROR"):
            self.attach()
        snapshot = self.writes[-1]
        self.assertNotIn("tasks", snapshot)
        self.assertFalse(snapshot["quota"]["available"])
        self.assertTrue(snapshot["quota"]["stale"])

    def test_quota_failure_retains_value_and_does_not_block_hud_then_recovers(self):
        self.attach()
        self.ack()
        old = copy.deepcopy(self.bridge.quota)
        self.magpie.fetch.side_effect = m.MagpieError("offline")
        self.hud.fetch.return_value = copy.deepcopy(self.tasks)
        self.hud.fetch.return_value[0]["summary"] = "updated task"
        with self.assertLogs("hudbuddy", "ERROR"):
            self.step(60)
        self.assertEqual(self.writes[-1]["tasks"][0]["summary"], "updated task")
        self.assertTrue(self.writes[-1]["quota"]["stale"])
        self.assertEqual(self.writes[-1]["quota"]["weekly"], old["weekly"])
        self.ack()
        count = len(self.writes)
        self.step(120)
        self.assertEqual(len(self.writes), count)
        self.magpie.fetch.side_effect = None
        self.magpie.fetch.return_value = quota(last_success_ms=1793437500000)
        self.step(180)
        self.assertFalse(self.writes[-1]["quota"]["stale"])
        self.assertEqual(len(self.writes), count + 1)

    def test_hud_failure_retains_tasks_while_new_quota_is_sent(self):
        self.attach()
        self.ack()
        self.hud.fetch.side_effect = b.BridgeError("HUD unavailable")
        self.magpie.fetch.return_value = quota(plan="new-plan")
        with self.assertLogs("hudbuddy", "ERROR"):
            self.step(60)
        snapshot = self.writes[-1]
        self.assertEqual(snapshot["tasks"], self.tasks)
        self.assertFalse(snapshot["hud_ok"])
        self.assertEqual(snapshot["quota"]["plan"], "new-plan")

    def test_observation_times_do_not_push_but_heartbeat_carries_latest_contact(self):
        self.attach()
        self.ack()
        count = len(self.writes)
        self.magpie.fetch.return_value = quota(read_at_ms=1793437600000, last_success_ms=1793437600000)
        self.step(60)
        self.assertEqual(len(self.writes), count)
        self.step(600)
        self.assertEqual(self.writes[-1]["type"], "heartbeat")
        self.assertEqual(self.writes[-1]["magpie_last_success_ms"], 1793437600000)
        self.assertEqual(self.writes[-1]["magpie_monitor_started_ms"], 1793437100000)

    def test_oversized_quota_preserves_prior_value_and_usb_connection(self):
        self.attach()
        self.ack()
        self.magpie.fetch.return_value = quota(account="x" * b.MAX_LINE_BYTES)
        with self.assertLogs("hudbuddy", "ERROR"):
            self.step(60)
        self.assertIsNotNone(self.bridge.serial)
        self.assertEqual(self.writes[-1]["quota"]["account"], "test@example.test")
        self.assertTrue(self.writes[-1]["quota"]["stale"])

    def test_restart_uses_persisted_quota_when_both_sources_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            m.MagpieClient(opener=FakeOpener(), state_path=path, now_ms=lambda: 1000).fetch()
            restored = m.MagpieClient(opener=FakeOpener(error=OSError("offline")), state_path=path, now_ms=lambda: 2000)
            self.bridge = b.Bridge(self.hud, quota_client=restored, clock=lambda: self.now,
                                   port_finder=lambda: "/dev/fake", serial_opener=lambda _: self.port)
            self.hud.fetch.side_effect = b.BridgeError("HUD offline")
            with self.assertLogs("hudbuddy", "ERROR"):
                self.attach()
            snapshot = self.writes[-1]
            self.assertNotIn("tasks", snapshot)
            self.assertTrue(snapshot["quota"]["available"])
            self.assertTrue(snapshot["quota"]["stale"])
            self.assertEqual(snapshot["quota"]["last_success_ms"], 1000)


if __name__ == "__main__":
    unittest.main()
