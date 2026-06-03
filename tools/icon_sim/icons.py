"""Weather icons + the wttr.in code -> icon mapping, with day/night variants.

Icons are the hand-made 1-bit pixel-art set by Dhole
(https://github.com/Dhole/weather-pixel-icons, CC BY-SA 4.0), designed natively
at 32x32 for monochrome displays (no downscaling artefacts).

Run fetch_icons.sh to download the .xbm files into _src/ (git-ignored), then
bitmap_sim.py previews them and exports the C header for the firmware.
"""
import os

from PIL import Image, ImageStat

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "_src")

ICON_W = 32
ICON_H = 32

# our icon name -> Dhole file (day)
ICON_FILES = {
    "sun":        "sun",
    "partly":     "cloud_sun",
    "cloud":      "cloud",
    "fog":        "cloud_wind",
    "rain":       "rain1",
    "heavy_rain": "rain2",
    "sleet":      "rain_snow",
    "snow":       "snow",
    "thunder":    "rain_lightning",
}

# night variants (only those the set provides; others reuse the day icon)
NIGHT_FILES = {
    "sun":    "moon",
    "partly": "cloud_moon",
    "fog":    "cloud_wind_moon",
    "rain":   "rain1_moon",
    "snow":   "snow_moon",
}

ICONS = list(ICON_FILES)

# every distinct Dhole file we need (day + night) -> used by fetch_icons.sh
ALL_FILES = sorted(set(ICON_FILES.values()) | set(NIGHT_FILES.values()))


def has_night(name):
    return name in NIGHT_FILES


def render(name, night=False):
    """Return a 1-bit PIL image (ICON_W x ICON_H), lit pixels = icon."""
    fname = NIGHT_FILES[name] if (night and name in NIGHT_FILES) else ICON_FILES[name]
    im = Image.open(os.path.join(SRC, fname + ".xbm")).convert("L")
    # XBM may come as black-on-white; we want the icon lit (white) on black.
    if ImageStat.Stat(im).mean[0] > 127:
        im = Image.eval(im, lambda v: 255 - v)
    return im.point(lambda v: 255 if v >= 128 else 0, mode="1")


# wttr.in WWO condition code -> icon name (derived from wttr.in's WWOCodeToName).
CODE_TO_ICON = {
    113: "sun",
    116: "partly",
    119: "cloud", 122: "cloud",
    143: "fog", 248: "fog", 260: "fog",
    176: "rain", 263: "rain", 266: "rain", 293: "rain", 296: "rain", 353: "rain",
    299: "heavy_rain", 302: "heavy_rain", 305: "heavy_rain", 308: "heavy_rain",
    356: "heavy_rain", 359: "heavy_rain",
    179: "sleet", 182: "sleet", 185: "sleet", 281: "sleet", 284: "sleet",
    311: "sleet", 314: "sleet", 317: "sleet", 350: "sleet", 362: "sleet",
    365: "sleet", 374: "sleet", 377: "sleet",
    227: "snow", 230: "snow", 320: "snow", 323: "snow", 326: "snow", 329: "snow",
    332: "snow", 335: "snow", 338: "snow", 368: "snow", 371: "snow", 395: "snow",
    200: "thunder", 386: "thunder", 389: "thunder", 392: "thunder",
}
