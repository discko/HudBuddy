"""Runtime configuration shared by development and installed Bridge."""
import json
import math
from pathlib import Path


def resolve_config(args):
    from service.bridge import DEVICE_SERIAL, DEFAULT_CLI, DEFAULT_COMPATIBILITY, DEFAULT_STATE_PATH
    data = {}
    if args.config:
        data = json.loads(args.config.expanduser().read_text(encoding="utf-8"))
        if not isinstance(data, dict):
            raise ValueError("config must be a JSON object")
    defaults = dict(device_serial=DEVICE_SERIAL, hud_cli=str(DEFAULT_CLI),
                    compatibility=str(DEFAULT_COMPATIBILITY), state_dir=str(DEFAULT_STATE_PATH.parent),
                    poll_interval=60, heartbeat_interval=600)
    unknown = set(data) - set(defaults)
    if unknown:
        raise ValueError(f"Unknown config keys: {', '.join(sorted(unknown))}")
    values = defaults | data
    for key, attr in dict(device_serial="device_serial", hud_cli="cli", compatibility="compatibility",
                          state_dir="state_dir", poll_interval="poll_interval", heartbeat_interval="heartbeat_interval").items():
        value = getattr(args, attr, None)
        if value is not None:
            values[key] = str(value) if isinstance(value, Path) else value
    for key in ("device_serial", "hud_cli", "compatibility", "state_dir"):
        if not isinstance(values[key], str):
            raise ValueError(f"{key} must be a string")
    values["device_serial"] = values["device_serial"].strip().upper()
    for key in ("hud_cli", "compatibility", "state_dir"):
        path = Path(values[key]).expanduser()
        if not path.is_absolute():
            raise ValueError(f"{key} must be an absolute path")
        values[key] = str(path)
    for key in ("poll_interval", "heartbeat_interval"):
        n = values[key]
        if isinstance(n, bool) or not isinstance(n, (int, float)) or not math.isfinite(n) or n < 1:
            raise ValueError(f"{key} must be a finite number >= 1")
    return values
