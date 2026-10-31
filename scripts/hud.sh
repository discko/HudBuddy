#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
BOARD='esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=custom,PSRAM=opi,CDCOnBoot=cdc,USBMode=hwcdc'
case "${1:-build}" in
  build)
    exec sh scripts/arduino.sh compile --fqbn "$BOARD" \
      --build-property upload.maximum_size=8388608 \
      --build-property "compiler.c.extra_flags=-DLV_CONF_PATH=$ROOT/firmware/hud/lv_conf.h" \
      --build-property "compiler.cpp.extra_flags=-DLV_CONF_PATH=$ROOT/firmware/hud/lv_conf.h" \
      --libraries firmware/libraries \
      --libraries vendor/waveshare-2.16/examples/arduino/libraries \
      --build-path "$ROOT/build/hud" firmware/hud
    ;;
  flash)
    : "${2:?Usage: sh scripts/hud.sh flash /dev/cu.usbmodem101}"
    exec sh scripts/arduino.sh upload --fqbn "$BOARD" --port "$2" \
      --input-dir "$ROOT/build/hud" firmware/hud
    ;;
  *) echo 'Usage: sh scripts/hud.sh build|flash [port]' >&2; exit 2 ;;
esac
