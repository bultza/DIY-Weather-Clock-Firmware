#!/usr/bin/env bash
# Downloads the source weather icons (Dhole pixel set, 32x32 XBM, CC BY-SA 4.0)
# into _src/. They are git-ignored; weather_icons.h (generated from them) is what
# the firmware uses. The file list is taken from ALL_FILES in icons.py.
set -e
cd "$(dirname "$0")"
mkdir -p _src
base="https://raw.githubusercontent.com/Dhole/weather-pixel-icons/master/32"

files=$(python -c "import icons; print(' '.join(icons.ALL_FILES))")
for f in $files; do
  echo "  $f.xbm"
  curl -fsS "$base/$f.xbm" -o "_src/$f.xbm"
done
echo "done -> _src/"
