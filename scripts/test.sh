#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
PYTHON=${PYTHON:-.tools/python/bin/python}
"$PYTHON" -m unittest discover -s tests -p 'test_*.py'
TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT HUP INT TERM
for test in power-policy short-press tap-guard quota-policy task-notifications chime-pcm; do
  clang++ -std=c++17 "tests/$test.cpp" -o "$TEST_DIR/$test"
  "$TEST_DIR/$test"
done
clang++ -std=c++17 tests/preview-scene.cpp firmware/libraries/HudBuddyScene/src/DogFrames.cpp -o "$TEST_DIR/scene"
"$TEST_DIR/scene" "$TEST_DIR/scene.ppm"
echo "Service, power, button, gesture guard, quota and scene tests passed."
