#!/bin/sh
# Provision project-local dependencies without changing shell/global Arduino config.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
mkdir -p .tools/bin vendor
if [ ! -x .tools/bin/arduino-cli ]; then
  CLI=$(command -v arduino-cli || true)
  if [ -z "$CLI" ]; then
    echo "Install Arduino CLI 1.5.1, then rerun (macOS: brew install arduino-cli)." >&2
    exit 1
  fi
  ln -s "$CLI" .tools/bin/arduino-cli
fi
python3 - <<'CONFIG'
import json
from pathlib import Path
root=Path.cwd()
config={"directories":{"data":str(root/".tools/arduino-data"),"downloads":str(root/".tools/arduino-downloads"),"user":str(root/".tools/arduino-user")},"board_manager":{"additional_urls":["https://espressif.github.io/arduino-esp32/package_esp32_index.json"]}}
(root/".tools/arduino-cli.yaml").write_text(json.dumps(config,indent=2)+"\n")
CONFIG
VENDOR_REV=225a62bff11b5d0a0b607873860d39485a9a9685
if [ ! -d vendor/waveshare-2.16 ]; then
  git clone https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16.git vendor/waveshare-2.16
  git -C vendor/waveshare-2.16 checkout "$VENDOR_REV"
fi
if [ "$(git -C vendor/waveshare-2.16 rev-parse HEAD)" != "$VENDOR_REV" ]; then
  echo "Vendor checkout differs from pinned revision; inspect it before continuing." >&2
  exit 1
fi
sh scripts/arduino.sh core update-index
sh scripts/arduino.sh core install esp32:esp32@3.3.11
if [ ! -x .tools/python/bin/python ]; then python3 -m venv .tools/python; fi
.tools/python/bin/python -m pip install -r service/requirements.txt -r scripts/requirements-dev.txt
echo "Ready. Build: sh scripts/hud.sh build"
