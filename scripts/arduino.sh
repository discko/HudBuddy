#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
exec "$ROOT/.tools/bin/arduino-cli" --config-file "$ROOT/.tools/arduino-cli.yaml" "$@"
