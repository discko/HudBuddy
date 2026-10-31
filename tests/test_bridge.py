"""Bridge tests use fake HUD subprocesses and fake/PTY serial ports only."""
import copy
import json
import os
from pathlib import Path
import pty
import subprocess
import sys
import tempfile
import termios
import unittest
from unittest.mock import patch

from service import bridge as b
# Device identity fixture must not depend on local pairing.
b.DEVICE_SERIAL = "AA:BB:CC:00:11:22"
from service.usb_events import Presence, USBMonitor


def card(task_id="a", status="doing", summary="说明", **extra):
    return {"task": {"id": task_id, "title": "测试任务", "status": status,
                     "summary": summary, "updated_at": 1793400000000, **extra},
            "primary_thread": {"thread": "must not leak"}}


def task_list(*cards):
    return b.project_tasks({"tasks": list(cards)})


class FakeClient:
    def __init__(self, tasks=None):
        self.tasks = tasks if tasks is not None else task_list(card())
        self.error = None
        self.calls = 0

    def fetch(self):
        self.calls += 1
        if self.error:
            raise self.error
        return copy.deepcopy(self.tasks)


class FakeSerial:
    def __init__(self):
        self.writes = []
        self.incoming = bytearray()
        self.closed = False
        self.fail_write = False

    def write(self, data):
        if self.fail_write:
            raise OSError("unplugged")
        self.writes.append(json.loads(data))
        return len(data)

    def read(self, size):
        chunk = bytes(self.incoming[:size])
        del self.incoming[:size]
        return chunk

    def close(self):
        self.closed = True


class ProjectionTests(unittest.TestCase):
    def test_filter_keeps_hud_order_and_only_task_fields(self):
        tasks = task_list(card("z", "waiting", sort_order=9, items=["secret"]),
                          card("hidden", "todo"), card("a", "doing", sort_order=1),
                          card("done", "done"), card("paused", "paused"))
        self.assertEqual([t["id"] for t in tasks], ["z", "a"])
        self.assertEqual(set(tasks[0]), {"id", "title", "status", "summary", "updated_at",
                                        "status_label", "status_color", "status_background"})
        self.assertEqual((tasks[0]["status_label"], tasks[0]["status_color"], tasks[0]["status_background"]),
                         ("等待中", "#e2c48b", "#443a27"))
        self.assertEqual(tasks[1]["status_color"], "#a5c4ee")

    def test_readable_summary_retains_text_and_code(self):
        original = "# 标题\n- [x] 查看 [文档](https://example.com/path) 与 `字段`\n```python\n`literal` [x](https://code.test)\n```\n嵌套 [文字](https://a.test/a(b)) 和 a_b、2*3\n最后\u0000"
        expected = "标题\n☑ 查看 文档 与 字段\n`literal` [x](https://code.test)\n嵌套 [文字](https://a.test/a(b)) 和 a_b、2*3\n最后"
        self.assertEqual(b.readable_summary(original), expected)

    def test_task_limit_and_malformed_data(self):
        self.assertEqual(len(task_list(*(card(str(i)) for i in range(64)))), 64)
        with self.assertRaisesRegex(b.BridgeError, "limit 64"):
            task_list(*(card(str(i)) for i in range(65)))
        for invalid in ({}, {"tasks": None}, {"tasks": [{}]}, {"tasks": [card(updated_at=True)]}):
            with self.subTest(invalid=invalid), self.assertRaises(b.BridgeError):
                b.project_tasks(invalid)

    def test_utf8_line_limit_includes_lf(self):
        payload = {"summary": ""}
        base = len(b.encode_line(payload))
        payload["summary"] = "x" * (b.MAX_LINE_BYTES - base)
        self.assertEqual(len(b.encode_line(payload)), b.MAX_LINE_BYTES)
        payload["summary"] += "中"
        with self.assertRaisesRegex(b.BridgeError, "including LF"):
            b.encode_line(payload)
        self.assertEqual(b.encode_line({"s": "中\n文"}).count(b"\n"), 1)


class StateTests(unittest.TestCase):
    def setUp(self):
        self.client = FakeClient()
        self.ports = []
        self.now = 0.0
        self.find_calls = 0
        self.available = True

        def finder():
            self.find_calls += 1
            return "/dev/fake" if self.available else None

        def opener(_):
            serial = FakeSerial()
            self.ports.append(serial)
            return serial

        self.bridge = b.Bridge(self.client, port_finder=finder, serial_opener=opener, clock=lambda: self.now)

    def step(self, now):
        self.now = now
        self.bridge.step()

    def attach(self):
        self.bridge.set_presence(True)
        self.bridge.step()

    def ack(self):
        self.ports[-1].incoming.extend(b.encode_line({"type": "ack", "seq": self.bridge.pending_seq}))
        self.bridge.step()

    def test_offline_has_no_timers_hud_reads_or_discovery(self):
        for now in (0, 60, 600, 100000):
            self.step(now)
        self.assertEqual((self.client.calls, self.find_calls), (0, 0))
        self.assertIsNone(self.bridge.wait_timeout())
        self.assertIsNone(self.bridge.poll_due)
        self.assertIsNone(self.bridge.heartbeat_due)
        self.assertIsNone(self.bridge.retry_due)

    def test_run_blocks_on_usb_event_indefinitely_when_offline(self):
        from unittest.mock import Mock
        monitor = Mock()
        monitor.take_changes.return_value = []
        monitor.event.wait.side_effect = KeyboardInterrupt
        with self.assertRaises(KeyboardInterrupt):
            self.bridge.run(monitor)
        monitor.event.wait.assert_called_once_with(None)
        monitor.start.assert_called_once()
        monitor.close.assert_called_once()
        self.assertEqual((self.client.calls, self.find_calls), (0, 0))

    def test_attach_queries_immediately_and_periods_are_independent(self):
        self.attach()
        self.assertEqual(self.client.calls, 1)
        self.assertEqual([w["type"] for w in self.ports[0].writes], ["heartbeat", "snapshot"])
        self.ack()
        for now in range(60, 600, 60):
            self.step(now)
        self.assertEqual(self.client.calls, 10)
        self.assertEqual(len(self.ports[0].writes), 2)  # No unchanged snapshots/heartbeats.
        self.step(600)
        self.assertEqual(self.client.calls, 11)
        self.assertEqual([w["type"] for w in self.ports[0].writes], ["heartbeat", "snapshot", "heartbeat"])
        self.assertEqual(self.bridge.seq, 1)
        for wire in self.ports[0].writes:
            self.assertIn("unix_ms", wire)
            self.assertIn("tz_offset_min", wire)

    def test_changed_task_pushes_snapshot_without_moving_heartbeat_deadline(self):
        self.attach()
        self.ack()
        self.client.tasks[0]["summary"] = "new"
        self.step(59.9)
        self.assertEqual(self.client.calls, 1)
        self.step(60)
        self.assertEqual(self.bridge.seq, 2)
        self.assertEqual(self.ports[0].writes[-1]["tasks"][0]["summary"], "new")
        self.assertEqual(self.bridge.heartbeat_due, 600)

    def test_detach_stops_timers_and_reconnect_queries_immediately(self):
        self.attach()
        self.ack()
        self.bridge.set_presence(False)
        self.assertTrue(self.ports[0].closed)
        self.step(10000)
        self.assertEqual((self.client.calls, self.find_calls), (1, 1))
        self.assertIsNone(self.bridge.wait_timeout())
        self.assertIsNone(self.bridge.retry_due)
        self.bridge.set_presence(True)
        self.step(10000)
        self.assertEqual((self.client.calls, self.find_calls), (2, 2))
        self.assertEqual(self.ports[1].writes[-1]["seq"], 2)
        self.assertEqual(self.bridge.poll_due, 10060)
        self.assertEqual(self.bridge.heartbeat_due, 10600)

    def test_cli_error_and_overflow_retain_last_good_snapshot(self):
        self.attach()
        self.ack()
        previous = copy.deepcopy(self.bridge.tasks)
        self.client.error = b.BridgeError("HUD storage unavailable")
        with self.assertLogs("hudbuddy", "ERROR"):
            self.step(60)
        self.assertEqual(self.bridge.tasks, previous)
        self.assertEqual(len(self.ports[0].writes), 3)
        self.assertFalse(self.ports[0].writes[-1]["hud_ok"])
        self.assertEqual(self.ports[0].writes[-1]["tasks"], previous)
        self.ack()
        self.client.error = None
        self.client.tasks[0]["summary"] = "长" * b.MAX_LINE_BYTES
        with self.assertLogs("hudbuddy", "ERROR"):
            self.step(120)
        self.assertEqual(self.bridge.tasks, previous)
        self.assertEqual(len(self.ports[0].writes), 3)
        with self.assertLogs("hudbuddy", "DEBUG") as logs:
            self.step(600)
        self.assertIn("sent heartbeat", "\n".join(logs.output))

    def test_initial_cli_error_never_sends_empty_tasks(self):
        self.client.error = b.BridgeError("busy")
        with self.assertLogs("hudbuddy", "ERROR"):
            self.attach()
        self.assertIsNone(self.bridge.tasks)
        self.assertEqual([w["type"] for w in self.ports[0].writes], ["heartbeat"])
        self.client.error = None
        self.client.tasks = []
        self.step(60)
        self.assertEqual(self.ports[0].writes[-1]["tasks"], [])

    def test_reconnect_replays_cached_snapshot_during_cli_error(self):
        self.attach()
        self.ack()
        self.ports[0].fail_write = True
        self.client.error = b.BridgeError("storage")
        with self.assertLogs("hudbuddy"):
            self.step(600)
        self.assertTrue(self.ports[0].closed)
        self.assertIsNone(self.bridge.serial)
        with self.assertLogs("hudbuddy"):
            self.step(600.5)
        self.assertEqual(self.ports[1].writes[-1]["seq"], 2)
        self.assertEqual(self.ports[1].writes[-1]["tasks"], self.bridge.tasks)

    def test_ack_timeout_is_six_seconds_independent_of_poll(self):
        self.attach()
        self.ports[0].incoming.extend(b'{"type":"ack","seq":99}\n')
        self.step(1)
        self.assertEqual(self.bridge.pending_seq, 1)
        self.step(5.99)
        self.assertIsNotNone(self.bridge.serial)
        with self.assertLogs("hudbuddy", "WARNING"):
            self.step(6)
        self.assertIsNone(self.bridge.serial)
        self.assertEqual(self.client.calls, 1)
        self.assertEqual(self.bridge.retry_due, 6.5)
        self.step(6.5)
        self.assertEqual(self.ports[1].writes[-1]["seq"], 2)

    def test_ack_deadline_starts_after_slow_full_write(self):
        self.bridge.serial = FakeSerial()
        self.bridge.tasks = self.client.tasks
        self.bridge.needs_snapshot = True
        original_write = self.bridge.serial.write

        def slow_write(wire):
            self.now += 1
            return original_write(wire)

        self.bridge.serial.write = slow_write
        self.bridge.send_snapshot()
        self.assertEqual(self.bridge.pending_since, 1)
        self.bridge.needs_snapshot = True
        self.bridge.send_snapshot()
        self.assertEqual(self.bridge.pending_since, 1)

    def test_node_appearance_fast_retries_then_low_frequency_only_when_online(self):
        self.available = False
        with self.assertLogs("hudbuddy", "WARNING"):
            self.attach()
            for now in (0.1, 0.5, 1.5, 3.5, 7.5):
                self.step(now)
        self.assertEqual(self.find_calls, 5)
        self.assertEqual(self.bridge.retry_due, 37.5)
        self.assertEqual(self.client.calls, 0)
        self.step(20)
        self.assertEqual(self.find_calls, 5)
        self.available = True
        self.step(37.5)
        self.assertEqual(self.client.calls, 1)
        self.assertEqual(self.bridge.retry_index, 5)  # Open alone does not reset backoff.
        self.ack()
        self.assertEqual(self.bridge.retry_index, 0)
        self.bridge.set_presence(False)
        self.step(100000)
        self.assertEqual(self.find_calls, 6)

    def test_repeated_ack_failures_eventually_use_low_frequency_retry(self):
        self.attach()
        for retry_delay in (0.5, 1, 2, 4, 30, 30):
            self.now = self.bridge.pending_since + 6
            with self.assertLogs("hudbuddy", "WARNING"):
                self.bridge.step()
            self.assertEqual(self.bridge.retry_due, self.now + retry_delay)
            self.step(self.bridge.retry_due)
        self.assertEqual(self.client.calls, 7)

    def test_partial_write_reconnects_and_replays_complete_snapshot(self):
        self.attach()
        self.ack()
        self.client.tasks[0]["summary"] = "change"
        port = self.ports[0]
        original_write = port.write

        def partial_snapshot(wire):
            if json.loads(wire)["type"] == "snapshot":
                return len(wire) // 2
            return original_write(wire)

        port.write = partial_snapshot
        with self.assertLogs("hudbuddy", "WARNING"):
            self.step(60)
        self.assertTrue(port.closed)
        self.step(60.5)
        self.assertEqual(self.ports[1].writes[-1]["tasks"][0]["summary"], "change")

    def test_hello_coalesces_in_flight_snapshot_and_handles_reset(self):
        self.attach()
        port = self.ports[0]
        hello = b'{"type":"hello","protocol":1,"device":"HudBuddy"}\n'
        port.incoming.extend(hello)
        self.step(1)
        self.assertEqual(self.bridge.seq, 1)
        self.ack()
        port.incoming.extend(b'boot log\n{"type":"hello","protocol":1,')
        self.step(2)
        self.assertEqual(self.bridge.seq, 1)
        port.incoming.extend(b'"device":"HudBuddy"}\n')
        self.step(2.1)
        self.assertEqual(self.bridge.seq, 2)
        self.assertEqual(self.client.calls, 1)

    def test_oversized_rx_recovers_at_next_line(self):
        self.attach()
        self.ack()
        port = self.ports[0]
        port.incoming.extend(b"x" * (b.MAX_LINE_BYTES + 20) + b"\n")
        with self.assertLogs("hudbuddy", "WARNING"):
            while port.incoming:
                self.bridge.step()
        self.assertEqual(len(self.bridge.rx), 0)
        self.assertFalse(self.bridge.discard_line)


class USBEventTests(unittest.TestCase):
    def setUp(self):
        self.presence = Presence(0x303A, 0x1001, b.DEVICE_SERIAL)

    def test_identity_filter_and_duplicate_notifications(self):
        self.presence.add(1, 0x303A, 0x1001, "other-device")
        self.presence.add(2, 1, 0x1001, b.DEVICE_SERIAL)
        self.assertEqual(self.presence.take_changes(), [])
        self.presence.add(3, 0x303A, 0x1001, b.DEVICE_SERIAL.lower())
        self.presence.add(3, 0x303A, 0x1001, b.DEVICE_SERIAL)
        self.assertEqual(self.presence.take_changes(), [True])
        self.assertFalse(self.presence.event.is_set())
        self.presence.remove(99)
        self.assertEqual(self.presence.take_changes(), [])
        self.presence.remove(3)
        self.assertEqual(self.presence.take_changes(), [False])

    def test_quick_unplug_replug_edges_are_not_collapsed(self):
        self.presence.add(1, 0x303A, 0x1001, b.DEVICE_SERIAL)
        self.presence.remove(1)
        self.presence.add(2, 0x303A, 0x1001, b.DEVICE_SERIAL)
        self.assertEqual(self.presence.take_changes(), [True, False, True])

    def test_distinct_host_legacy_ids_only_emit_physical_state_changes(self):
        for registry_id in (1, 2):
            self.presence.add(registry_id, 0x303A, 0x1001, b.DEVICE_SERIAL)
        self.presence.remove(1)
        self.assertEqual(self.presence.take_changes(), [True])
        self.presence.remove(2)
        self.assertEqual(self.presence.take_changes(), [False])

    def test_termination_uses_registry_id_without_reading_lost_properties(self):
        import ctypes
        from unittest.mock import Mock
        monitor = USBMonitor(0x303A, 0x1001, b.DEVICE_SERIAL)
        monitor.presence.add(42, 0x303A, 0x1001, b.DEVICE_SERIAL)
        monitor.presence.take_changes()

        def registry_id(service, output):
            ctypes.cast(output, ctypes.POINTER(ctypes.c_uint64))[0] = 42
            return 0

        monitor.io = Mock()
        monitor.io.IOIteratorNext.side_effect = [123, 0]
        monitor.io.IORegistryEntryGetRegistryEntryID.side_effect = registry_id
        monitor._property = Mock(side_effect=AssertionError("properties vanished"))
        monitor._drain(9, removed=True)
        self.assertEqual(monitor.take_changes(), [False])
        monitor._property.assert_not_called()
        monitor.io.IOObjectRelease.assert_called_once_with(123)



class ClientTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.cli = self.root / "hud-cli"
        self.response_file = self.root / "response.json"
        self.response_file.write_text(json.dumps({"ok": True, "response": {"op": "list_tasks", "data": {"tasks": [card()]}}}), encoding="utf-8")
        self.cli.write_text(f"#!{sys.executable}\n" + '''import json, pathlib, sys
if sys.argv[1:] == ['info', '--json']:
    print(json.dumps({'ok': True, 'info': {'cli_version': '0.2.0', 'api_version': 3, 'bundled_skill_version': '0.2.2'}}))
elif sys.argv[1:] == ['request', '--stdin']:
    request = json.load(sys.stdin)
    assert request == {'op': 'list_tasks', 'args': {'filter': 'all'}}
    print(pathlib.Path(__file__).with_name('response.json').read_text())
else:
    sys.exit(8)
''', encoding="utf-8")
        self.cli.chmod(0o755)
        self.client = b.HudClient(self.cli, timeout=2)

    def tearDown(self):
        self.temp.cleanup()

    def test_real_subprocess_contract_and_compatibility(self):
        self.client.check_compatibility()
        self.assertEqual(self.client.fetch()[0]["id"], "a")

    def test_invalid_json_and_ok_false_raise(self):
        for content in ("not json", '{"ok":false,"error":{"code":"storage","message":"locked"}}'):
            self.response_file.write_text(content, encoding="utf-8")
            with self.subTest(content=content), self.assertRaises(b.BridgeError):
                self.client.fetch()

    def test_timeout_and_nonzero_even_with_ok_json_raise(self):
        for body in ("import time; time.sleep(2)", "print('{\"ok\":true}'); raise SystemExit(1)"):
            self.cli.write_text(f"#!{sys.executable}\n{body}\n", encoding="utf-8")
            self.client.timeout = 0.05
            with self.subTest(body=body), self.assertRaises(b.BridgeError):
                self.client.fetch()

    def test_once_stdout_does_not_require_serial(self):
        import io
        from types import SimpleNamespace
        from service.magpie import empty_quota
        stdout = SimpleNamespace(buffer=io.BytesIO())
        quota_client = SimpleNamespace(cached_quota=None, fetch=lambda: empty_quota())
        with patch("service.bridge.MagpieClient", return_value=quota_client), patch("service.bridge.sys.stdout", stdout), patch("service.bridge.logging.basicConfig"):
            result = b.main(["--cli", str(self.cli), "--once", "--stdout", "--poll-interval", "60", "--heartbeat-interval", "600"])
        self.assertEqual(result, 0)
        self.assertEqual(stdout.buffer.getvalue().count(b"\n"), 1)
        output = json.loads(stdout.buffer.getvalue())
        self.assertEqual(output["type"], "snapshot")
        self.assertEqual(output["tasks"][0]["id"], "a")
        self.assertFalse(output["quota"]["available"])

    def test_invalid_timer_values_fail_before_hud_requests(self):
        for args in (("--poll-interval", "nan"), ("--heartbeat-interval", "0"), ("--interval", "-1")):
            result = subprocess.run([sys.executable, "-m", "service.bridge", "--cli", str(self.cli), "--once", *args],
                                    capture_output=True, cwd=Path(__file__).resolve().parents[1], timeout=10)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stdout, b"")

    def test_incompatible_cli_stops_before_request(self):
        self.cli.write_text(f"#!{sys.executable}\n" + '''import json, sys
if sys.argv[1:] == ['info', '--json']:
    print(json.dumps({'ok': True, 'info': {'cli_version': '0.2.0', 'api_version': 99, 'bundled_skill_version': '0.2.2'}}))
else:
    raise RuntimeError('Business request must never run')
''', encoding="utf-8")
        with self.assertRaisesRegex(b.BridgeError, "不兼容"):
            self.client.check_compatibility()


class PortTests(unittest.TestCase):
    def test_discovery_matches_vid_pid_and_identity(self):
        from types import SimpleNamespace
        def port(path, serial=b.DEVICE_SERIAL, vid=0x303A, pid=0x1001):
            return SimpleNamespace(device=path, vid=vid, pid=pid, serial_number=serial)
        with patch("serial.tools.list_ports.comports", return_value=[
            port("/dev/cu.wrong", serial="another"), port("/dev/cu.wrongpid", pid=1),
            port("/dev/tty.usbmodem1"), port("/dev/cu.usbmodem1")]):
            self.assertEqual(b.discover_port(), "/dev/cu.usbmodem1")
        with patch("serial.tools.list_ports.comports", return_value=[]):
            self.assertIsNone(b.discover_port())

    def test_open_virtual_port_never_toggles_modem_lines_and_disables_hupcl(self):
        master, slave = pty.openpty()
        try:
            with patch("serial.serialposix.Serial._update_dtr_state") as dtr, patch("serial.serialposix.Serial._update_rts_state") as rts:
                port = b.open_serial(os.ttyname(slave))
                try:
                    self.assertFalse(termios.tcgetattr(port.fd)[2] & termios.HUPCL)
                finally:
                    port.close()
                dtr.assert_not_called()
                rts.assert_not_called()
        finally:
            os.close(master)
            os.close(slave)


if __name__ == "__main__":
    unittest.main()
