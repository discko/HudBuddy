#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
BOARD='esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,CDCOnBoot=cdc,USBMode=hwcdc'
case "${1:-build}" in
  build)
    exec sh scripts/arduino.sh compile --fqbn "$BOARD" \
      --libraries firmware/libraries \
      --libraries vendor/waveshare-2.16/examples/arduino/libraries \
      --build-path "$ROOT/build/standby" \
      firmware/standby
    ;;
  flash)
    : "${2:?Usage: sh scripts/standby.sh flash /dev/cu.usbmodem101}"
    exec sh scripts/arduino.sh upload --fqbn "$BOARD" --port "$2" \
      --input-dir "$ROOT/build/standby" \
      firmware/standby
    ;;
  *) echo 'Usage: sh scripts/standby.sh build|flash [port]' >&2; exit 2 ;;
esac
