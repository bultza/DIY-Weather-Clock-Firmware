# Weather icon sim

Tool to preview the monochrome weather icons and export them to the firmware.

The icons are the hand-made 1-bit pixel-art set by **Dhole**
([weather-pixel-icons](https://github.com/Dhole/weather-pixel-icons), **CC BY-SA
4.0**), designed natively at 32x32 for monochrome displays — so they stay crisp
on the OLED (unlike downscaled vector/grayscale icons, which threshold badly).

## Use

```bash
pip install pillow
cd tools/icon_sim
bash fetch_icons.sh              # download the .xbm sources into _src/ (git-ignored)
python bitmap_sim.py             # window with all icons (day + night), scaled
python bitmap_sim.py --png x.png # save the grid instead
python bitmap_sim.py --export    # (re)generate ../../weather_icons.h
```

The firmware draws them with
`display.drawBitmap(x, y, weatherIconForCode(code, night), WEATHER_ICON_W, WEATHER_ICON_H, SSD1306_WHITE)`.

## Icon selection

The clock requests the numeric WWO condition code from wttr.in (one-line `%i`)
and maps it through `CODE_TO_ICON` (in `icons.py`, derived from wttr.in's own
`WWOCodeToName`) to one of: `sun, partly, cloud, fog, rain, heavy_rain, sleet,
snow, thunder`. At night (`isNightNow()` in the firmware) the `_moon` variant is
used where one exists (`sun, partly, fog, rain, snow`); the rest reuse the day
icon.

To change which Dhole file maps to each icon, edit `ICON_FILES` / `NIGHT_FILES`
in `icons.py`, re-run `fetch_icons.sh` and `bitmap_sim.py --export`.

## Attribution

The generated `weather_icons.h` and any redistribution must credit Dhole and
keep the CC BY-SA 4.0 license (the icon bitmaps are CC BY-SA; this does not
affect the firmware's own license).
