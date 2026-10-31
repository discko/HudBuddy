import argparse
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from service.config import resolve_config


class ConfigTests(unittest.TestCase):
    def test_config_and_cli_precedence(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.json"
            path.write_text(json.dumps({"device_serial": "ab:cd", "poll_interval": 90,
                                        "state_dir": directory}))
            result = resolve_config(argparse.Namespace(config=path, poll_interval=30))
            self.assertEqual(result["device_serial"], "AB:CD")
            self.assertEqual(result["poll_interval"], 30)
            self.assertEqual(result["heartbeat_interval"], 600)
            self.assertEqual(result["state_dir"], directory)

    def test_reject_invalid_config(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.json"
            for data in ([], {"unknown": 1}, {"poll_interval": 0}, {"heartbeat_interval": True},
                         {"device_serial": 123}, {"state_dir": "relative"}, {"hud_cli": None}):
                path.write_text(json.dumps(data))
                with self.subTest(data=data), self.assertRaises(ValueError):
                    resolve_config(argparse.Namespace(config=path))

    def test_installed_agent_has_no_checkout_dependency(self):
        root = Path(__file__).resolve().parents[1]
        spec = importlib.util.spec_from_file_location("installer", root / "scripts/install-bridge.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        base = Path("/Users/test/Library/Application Support/HudBuddy")
        release = base / "bridge/releases/test"
        plist = module.agent_plist(release, base / "config.json", Path("/Users/test/Library/Logs/HudBuddy"))
        self.assertEqual(plist["WorkingDirectory"], str(release))
        self.assertEqual(plist["ProgramArguments"][0], str(release / "venv/bin/python"))
        self.assertNotIn(str(root), str(plist))
        self.assertTrue(plist["RunAtLoad"])
        self.assertTrue(plist["KeepAlive"])
