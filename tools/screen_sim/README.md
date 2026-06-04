# Screen sim

Bit-for-bit simulator of the clock's 128x64 monochrome OLED. It re-implements the
slice of Adafruit_GFX + SSD1306 the firmware uses and mirrors `drawTimeScreen()` /
`drawWeatherScreen()` (including the clock-screen WiFi meter + Netatmo status mark
from `drawTopStatusIcons()`), so the rendered pixels match what the real display
shows. Handy for previewing layout changes without flashing hardware.

It uses the **real Adafruit GFX fonts** and the **real Dhole weather icons**, so
nothing is approximated:

- `gfx.py` — the 1-bit framebuffer + GFX text/draw primitives (classic 5x7 font,
  custom GFX fonts, `getTextBounds`, `drawBitmap`, `drawCircle`, ...).
- `render.py` — the two screen layouts + a labeled montage of example states.

## Requirements

- Python 3 with **Pillow**: `pip install pillow`

## Setup (one-time)

The font and icon source files are **not committed** (they're third-party — see
below), so fetch them first:

```bash
cd tools/screen_sim
bash fetch_fonts.sh            # Adafruit GFX fonts  -> _fonts/      (git-ignored)
bash ../icon_sim/fetch_icons.sh   # Dhole weather icons -> ../icon_sim/_src/ (git-ignored)
```

`render.py` imports the icon code from `../icon_sim`, which reads the `.xbm`
sources in `../icon_sim/_src/` — that's why both fetch scripts are needed.

## Run

```bash
python render.py              # -> screens.png  (montage of clock + weather faces)
python render.py --out x.png  # choose the output path
```

Open `screens.png` to see the simulated screens. Edit the `examples()` list in
`render.py` to add/adjust cases — `time_screen(...)` takes `wifi_bars` (0-4) and
`netatmo` (`None` / `"ok"` / `"error"`) to exercise the status icons.

## Is `_fonts/` committed? No.

`_fonts/` (and `../icon_sim/_src/`) are **git-ignored** and recreated by the fetch
scripts. The fonts are part of [Adafruit-GFX-Library](https://github.com/adafruit/Adafruit-GFX-Library)
(BSD) and the icons are Dhole's (CC BY-SA 4.0); we don't redistribute them here,
we download them on demand — same pattern as `tools/icon_sim`. The simulator
itself (`gfx.py`, `render.py`) is the only thing tracked. Note: `screens.png` is a
generated preview; regenerate it rather than relying on a committed copy.

## Relation to the firmware

This tool is **not** part of the firmware build. The `netatmoOkIcon` bytes and all
layout coordinates are duplicated from the `.ino` by hand; if you change a screen
layout or the status-icon glyph in the firmware, update `render.py` to match.
