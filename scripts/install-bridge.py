#!/usr/bin/env python3
"""Install/update a per-user macOS LaunchAgent. No sudo required."""
import argparse
import datetime
import json
import os
from pathlib import Path
import plistlib
import shutil
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
LABEL = "com.hudbuddy.bridge"
BASE = Path.home() / "Library/Application Support/HudBuddy"
LOGS = Path.home() / "Library/Logs/HudBuddy"
PLIST = Path.home() / "Library/LaunchAgents" / (LABEL + ".plist")
DOMAIN = f"gui/{os.getuid()}"
TARGET = f"{DOMAIN}/{LABEL}"


def run(argv, check=True, capture=False):
    return subprocess.run([str(a) for a in argv], check=check, text=True,
                          capture_output=capture)


def agent_plist(release, config, logs):
    return dict(Label=LABEL, ProgramArguments=[str(release / "venv/bin/python"), "-u", "-m", "service.bridge",
                "--config", str(config), "--log-file", str(logs / "bridge.log")], WorkingDirectory=str(release), RunAtLoad=True,
                KeepAlive=True, ThrottleInterval=30, ProcessType="Background",
                StandardOutPath=str(logs / "stdout.log"), StandardErrorPath=str(logs / "launcher.log"),
                EnvironmentVariables={"PYTHONUNBUFFERED": "1", "PATH": "/usr/bin:/bin:/usr/sbin:/sbin"})


def unload():
    if run(["launchctl", "print", TARGET], check=False, capture=True).returncode == 0:
        run(["launchctl", "bootout", TARGET])


def stop_development_bridge():
    pidfile = ROOT / ".state/bridge.pid"
    if not pidfile.exists():
        return
    pid = int(pidfile.read_text().strip())
    result = run(["ps", "-p", str(pid), "-o", "args="], check=False, capture=True)
    if result.returncode:
        return
    cwd = run(["lsof", "-a", "-p", str(pid), "-d", "cwd", "-Fn"], check=False, capture=True)
    # macOS reports the resolved Python.app binary, not the venv symlink.
    same_project = "n" + str(ROOT) in cwd.stdout.splitlines()
    if not same_project or "-m service.bridge" not in result.stdout:
        raise RuntimeError("Development PID belongs to another command; stop the development Bridge manually")
    os.kill(pid, signal.SIGINT)
    for _ in range(50):
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            pidfile.unlink(missing_ok=True)
            return
        time.sleep(.1)
    raise RuntimeError("Development Bridge did not stop; installation not activated")


def install(args):
    BASE.mkdir(parents=True, exist_ok=True, mode=0o700)
    state = BASE / "state"
    state.mkdir(exist_ok=True, mode=0o700)
    LOGS.mkdir(parents=True, exist_ok=True)
    config = BASE / "config.json"
    if not config.exists():
        serial_file = ROOT / ".state/device-serial"
        serial = args.device_serial or os.environ.get("HUDBUDDY_DEVICE_SERIAL") or (
            serial_file.read_text().strip() if serial_file.exists() else "")
        if not serial:
            raise RuntimeError("First install needs --device-serial or .state/device-serial")
        data = dict(device_serial=serial.upper(), hud_cli=str(Path.home() / ".local/bin/hud-cli"),
                    compatibility=str(Path.home() / ".codex/skills/hud/scripts/check_compatibility.py"),
                    state_dir=str(state), poll_interval=60, heartbeat_interval=600)
        with config.open("x", encoding="utf-8") as f:
            os.chmod(config, 0o600)
            json.dump(data, f, ensure_ascii=False, indent=2)
            f.write("\n")
    elif args.device_serial:
        raise RuntimeError("Existing config preserved; edit config.json to change the paired device")
    release = BASE / "bridge/releases" / datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    release.mkdir(parents=True)
    shutil.copytree(ROOT / "service", release / "service", ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    # Create in its final directory: venv entry points contain absolute paths.
    run([sys.executable, "-m", "venv", release / "venv"])
    python = release / "venv/bin/python"
    run([python, "-m", "pip", "install", "-r", release / "service/requirements.txt"])
    run([python, "-m", "compileall", "-q", release / "service"])
    # Validate config and HUD compatibility before interrupting the working service.
    check = "from argparse import Namespace; from pathlib import Path; from service.config import resolve_config; from service.bridge import HudClient; import sys; c=resolve_config(Namespace(config=Path(sys.argv[1]))); assert c['device_serial']; HudClient(Path(c['hud_cli']),compatibility=Path(c['compatibility'])).check_compatibility()"
    subprocess.run([str(python), "-c", check, str(config)], cwd=release, check=True)
    old_plist = PLIST.read_bytes() if PLIST.exists() else None
    stop_development_bridge()
    unload()
    old_cache = ROOT / ".state/magpie-state.json"
    # Do not overwrite a newer installed cache on subsequent updates.
    if old_cache.exists() and not (state / "magpie-state.json").exists():
        shutil.copy2(old_cache, state / "magpie-state.json")
        (state / "magpie-state.json").chmod(0o600)
    PLIST.parent.mkdir(parents=True, exist_ok=True)
    temp = PLIST.with_suffix(".tmp")
    temp.write_bytes(plistlib.dumps(agent_plist(release, config, LOGS)))
    temp.chmod(0o644)
    temp.replace(PLIST)
    try:
        run(["launchctl", "enable", TARGET])
        run(["launchctl", "bootstrap", DOMAIN, PLIST])
        time.sleep(2)
        status = run(["launchctl", "print", TARGET], capture=True).stdout
        if "state = running" not in status:
            raise RuntimeError("Agent did not stay running; inspect ~/Library/Logs/HudBuddy/bridge.log")
    except Exception:
        unload()
        if old_plist:
            PLIST.write_bytes(old_plist)
            run(["launchctl", "bootstrap", DOMAIN, PLIST], check=False)
        else:
            PLIST.unlink(missing_ok=True)
        raise
    current = BASE / "bridge/current"
    link = current.with_name(".current-next")
    link.unlink(missing_ok=True)
    link.symlink_to(release)
    link.replace(current)
    print(f"Installed {LABEL}\nProgram: {release}\nConfig: {config}\nLogs: {LOGS}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["install", "status", "start", "stop", "restart", "uninstall"])
    parser.add_argument("--device-serial")
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("macOS is required")
    if args.action == "install":
        install(args)
    elif args.action == "status":
        run(["launchctl", "print", TARGET])
    elif args.action == "stop":
        run(["launchctl", "disable", TARGET]); unload()
    elif args.action in ("start", "restart"):
        if args.action == "restart":
            unload()
        run(["launchctl", "enable", TARGET])
        if run(["launchctl", "print", TARGET], check=False, capture=True).returncode:
            run(["launchctl", "bootstrap", DOMAIN, PLIST])
    else:
        unload()
        PLIST.unlink(missing_ok=True)
        # Preserve config, durable state and logs; remove only installed code.
        if (BASE / "bridge").exists():
            shutil.rmtree(BASE / "bridge")
        print("Uninstalled. Configuration, state and logs preserved.")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"Installation/service operation failed: {error}", file=sys.stderr)
        sys.exit(1)
