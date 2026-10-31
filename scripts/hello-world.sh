#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
BOARD='esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,CDCOnBoot=cdc,USBMode=hwcdc'
case "${1:-build}" in
  build)
    exec sh scripts/arduino.sh compile --fqbn "$BOARD" \
      --libraries vendor/waveshare-2.16/examples/arduino/libraries \
      --build-path "$ROOT/build/hello-world-216" \
      vendor/waveshare-2.16/examples/arduino/01_HelloWorld
    ;;
  flash)
    : "${2:?Usage: sh scripts/hello-world.sh flash /dev/cu.usbmodem101}"
    exec sh scripts/arduino.sh upload --fqbn "$BOARD" --port "$2" \
      --input-dir "$ROOT/build/hello-world-216" \
      vendor/waveshare-2.16/examples/arduino/01_HelloWorld
    ;;
  *) echo 'Usage: sh scripts/hello-world.sh build|flash [port]' >&2; exit 2 ;;
esac
