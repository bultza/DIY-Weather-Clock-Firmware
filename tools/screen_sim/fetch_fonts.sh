#!/usr/bin/env bash
# Downloads the Adafruit GFX font sources the simulator needs into _fonts/.
# They are git-ignored (third-party, part of Adafruit-GFX-Library, BSD license) —
# same approach as tools/icon_sim/_src. gfx.py parses them at runtime.
set -e
cd "$(dirname "$0")"
mkdir -p _fonts
base="https://raw.githubusercontent.com/adafruit/Adafruit-GFX-Library/master"

for f in "Fonts/FreeMonoBold18pt7b.h" "Fonts/FreeMonoBold12pt7b.h" "glcdfont.c"; do
  echo "  $(basename "$f")"
  curl -fsS "$base/$f" -o "_fonts/$(basename "$f")"
done
echo "done -> _fonts/"
