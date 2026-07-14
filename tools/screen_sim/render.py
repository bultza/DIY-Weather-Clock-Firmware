#!/usr/bin/env python3
"""Bit-for-bit simulation of the clock's two OLED screens (128x64, 1-bit).

Mirrors drawTimeScreen() / drawWeatherScreen() in the firmware exactly (incl. the
clock-screen WiFi meter + Netatmo status mark from drawTopStatusIcons()), using
the real Adafruit GFX fonts (gfx.py) and the real weather icons (../icon_sim).
Produces a labeled PNG montage of several example states.

    python render.py            # -> screens.png
    python render.py --out x.png
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "icon_sim"))

from PIL import Image, ImageDraw

import gfx
from gfx import Display, FreeMonoBold12, FreeMonoBold18
import icons

DEG = "\xf7"  # what the firmware prints as (char)247 on the classic font


# --------------------------------------------------------------------------
# Icon bytes (row-padded, MSB-first) straight from the Dhole sources, exactly
# as weather_icons.h would hold them.
# --------------------------------------------------------------------------
def icon_bytes(name, night):
    img = icons.render(name, night)
    px = img.load()
    w, h = icons.ICON_W, icons.ICON_H
    rb = (w + 7) // 8
    out = []
    for y in range(h):
        for b in range(rb):
            byte = 0
            for bit in range(8):
                x = b * 8 + bit
                if x < w and px[x, y]:
                    byte |= (0x80 >> bit)
            out.append(byte)
    return out, w, h


def icon_for_code(code, night):
    name = icons.CODE_TO_ICON.get(code)
    if name is None:
        return None
    if night and not icons.has_night(name):
        night = False
    return icon_bytes(name, night)


# 8x8 "Netatmo OK" glyph — identical bytes to netatmoOkIcon[] in the firmware.
NETATMO_OK_ICON = [0x18, 0x3C, 0x7E, 0xFF, 0xFF, 0xFF, 0xC3, 0x81]

# 8x8 heart, two frames — identical bytes to heartSmall[]/heartBig[] in the
# firmware. Alternated (lub-dub) as the "firmware update available" indicator.
HEART_SMALL = [0x00, 0x66, 0x7E, 0x7E, 0x3C, 0x18, 0x00, 0x00]
HEART_BIG   = [0x66, 0xFF, 0xFF, 0xFF, 0x7E, 0x3C, 0x18, 0x00]


def draw_update_heart(d, heart):
    """Mirror drawUpdateHeart(): a beating heart, top-left, when an update exists.

    heart: None (nothing), "small" (diastole / rest) or "big" (systole / beat).
    The firmware animates between the two on a lub-dub rhythm; here we render a
    chosen frame since the sim is a single still.
    """
    if not heart:
        return
    d.bitmap(0, 0, HEART_BIG if heart == "big" else HEART_SMALL, 8, 8)


def draw_status_icons(d, wifi_bars, netatmo):
    """Mirror drawTopStatusIcons(): top-right WiFi meter + Netatmo mark.

    wifi_bars: 0-4 (how many of the 4 ascending bars are lit).
    netatmo:   None (disabled / nothing), "ok" (the glyph), or "error" (a '!').
    """
    bw, gap, base_y = 2, 1, 8
    x0 = 128 - (4 * bw + 3 * gap)          # right-aligned = 117
    for i in range(4):
        if i < wifi_bars:
            hgt = 2 * (i + 1)              # 2,4,6,8 px tall
            x = x0 + i * (bw + gap)
            for dx in range(bw):
                for dy in range(hgt):
                    d.pixel(x + dx, base_y - hgt + dy)

    if netatmo:
        nx = x0 - 10                       # 8px icon + 2px gap before the meter
        if netatmo == "ok":
            d.bitmap(nx, 0, NETATMO_OK_ICON, 8, 8)
        elif netatmo == "error":
            d.setFont(None)
            d.setCursor(nx + 2, 0)
            d.print("!")


# --------------------------------------------------------------------------
# Screen 1: the clock face  (drawTimeScreen)
# --------------------------------------------------------------------------
def time_screen(day_name, hh, mm, ss=None, ampm=None,
                temp_line="N/A", date_str="04/06/2026",
                wifi_bars=4, netatmo=None, heart=None):
    """ss/ampm None => not shown. temp_line is the full bottom-left string.
    wifi_bars (0-4) and netatmo (None/"ok"/"error") drive the top status icons.
    heart (None/"small"/"big") draws the update indicator, top-left."""
    d = Display()
    show_seconds = ss is not None
    show_12h = ampm is not None

    # Day name, classic, centered, top
    d.setFont(None)
    w, h = d.getTextBounds(day_name, 0, 0)
    d.setCursor((128 - w) // 2, 0)
    d.print(day_name)

    ampmW = 0
    if show_12h:
        d.setFont(None)
        ampmW, _ = d.getTextBounds(ampm, 0, 0)

    time_str = "%02d:%02d" % (hh, mm)
    timeX = timeY = 0

    if show_seconds:
        sec_str = " :%02d" % ss
        d.setFont(None)
        w2, h2 = d.getTextBounds(sec_str, 0, 0)
        d.setFont(FreeMonoBold18)
        w, h = d.getTextBounds(time_str, 0, 30)
        timeX = (128 - w - w2) // 2
        timeY = 28 + (h // 2)
        d.setCursor(timeX, timeY)
        d.print(time_str)
        d.setFont(None)
        d.setCursor(timeX + w, timeY - h2 + 2)
        d.print(sec_str)
    else:
        d.setFont(FreeMonoBold18)
        w, h = d.getTextBounds(time_str, 0, 30)
        timeX = (128 - w - (ampmW + 12 if show_12h else 0)) // 2
        timeY = 28 + (h // 2)
        d.setCursor(timeX, timeY)
        d.print(time_str)

    if show_12h:
        d.setFont(None)
        d.setCursor(timeX + w + 12, timeY - h)
        d.print(ampm)

    # Bottom-left: temperature + humidity (classic)
    d.setFont(None)
    d.setCursor(0, 57)
    d.print(temp_line)

    # Bottom-right: date (classic)
    w, h = d.getTextBounds(date_str, 0, 0)
    d.setCursor(128 - w, 56)
    d.print(date_str)

    d.hline(0, 52, 128)
    draw_status_icons(d, wifi_bars, netatmo)
    draw_update_heart(d, heart)
    return d


# --------------------------------------------------------------------------
# Screen 2: the weather face  (drawWeatherScreen)
# --------------------------------------------------------------------------
def weather_screen(city, temp_num, unit, cond, bottom, code=None, night=False, heart=None):
    """code None => no icon. temp_num/unit None => N/A state.
    heart (None/"small"/"big") draws the update indicator, top-left."""
    d = Display()
    have_temp = temp_num is not None

    d.setFont(None)
    w, h = d.getTextBounds(city, 0, 0)
    d.setCursor((128 - w) // 2, 0)
    d.print(city)

    icon = icon_for_code(code, night) if (code is not None and have_temp) else None

    if have_temp:
        d.setFont(FreeMonoBold12)
        w, h = d.getTextBounds(temp_num, 0, 30)
        wu, hu = d.getTextBounds(unit, 0, 30)
        total = w + 6 + wu
        if icon:
            data, iw, ih = icon
            GAP = 5
            combined = iw + GAP + total
            startX = (128 - combined) // 2
            d.bitmap(startX, 9, data, iw, ih)
            tempX = startX + iw + GAP
            tempY = 32
        else:
            tempX = (128 - total) // 2
            tempY = 30
        d.setFont(FreeMonoBold12)
        d.setCursor(tempX, tempY)
        d.print(temp_num)
        degX = tempX + w + 3
        degY = max(tempY - 16, 0)
        d.circle(degX, degY, 2)
        d.setCursor(degX + 3, tempY)
        d.print(unit)
        d.setFont(None)
    else:
        d.setFont(FreeMonoBold12)
        w, h = d.getTextBounds("N/A", 0, 30)
        d.setCursor((128 - w) // 2, 30)
        d.print("N/A")
        d.setFont(None)

    # Condition, classic, centered
    d.setFont(None)
    condY = 42 if icon else 38
    w, h = d.getTextBounds(cond, 0, condY)
    d.setCursor((128 - w) // 2, condY)
    d.print(cond)

    # Bottom line, classic
    botY = 57 if icon else 56
    if have_temp:
        w, h = d.getTextBounds(bottom, 0, botY)
        d.setCursor((128 - w) // 2, botY)
        d.print(bottom)
    else:
        w, h = d.getTextBounds(bottom, 0, botY)
        d.setCursor(128 - w, botY)
        d.print(bottom)

    d.hline(0, 52, 128)
    draw_update_heart(d, heart)
    return d


# --------------------------------------------------------------------------
# Montage
# --------------------------------------------------------------------------
SCALE = 4
PAD = 10
LABEL_H = 16
COLS = 2
# OLED-ish look: bright cyan-white pixels on near-black
ON = (180, 240, 255)
OFF = (8, 10, 16)


def to_image(d):
    img = Image.new("RGB", (gfx.WIDTH, gfx.HEIGHT), OFF)
    px = img.load()
    for y in range(gfx.HEIGHT):
        row = d.buf[y]
        for x in range(gfx.WIDTH):
            if row[x]:
                px[x, y] = ON
    return img.resize((gfx.WIDTH * SCALE, gfx.HEIGHT * SCALE), Image.NEAREST)


def montage(items):
    cw = gfx.WIDTH * SCALE + PAD * 2
    ch = gfx.HEIGHT * SCALE + PAD + LABEL_H
    rows = (len(items) + COLS - 1) // COLS
    sheet = Image.new("RGB", (cw * COLS, ch * rows), (32, 33, 38))
    sd = ImageDraw.Draw(sheet)
    for idx, (label, d) in enumerate(items):
        r, c = divmod(idx, COLS)
        ox, oy = c * cw, r * ch
        sub = to_image(d)
        sd.rectangle([ox + PAD - 1, oy + PAD - 1,
                      ox + PAD + sub.width, oy + PAD + sub.height], outline=(70, 72, 80))
        sheet.paste(sub, (ox + PAD, oy + PAD))
        sd.text((ox + PAD, oy + PAD + sub.height + 3), label, fill=(210, 212, 220))
    return sheet


def examples():
    out = []
    # --- Firmware-update heartbeat indicator (top-left), both beat frames ---
    out.append(("Update heart BIG (systole/beat) - clock",
                time_screen("Wednesday", 14, 9,
                            temp_line="+22" + DEG + "C 55%", date_str="04/06/2026",
                            wifi_bars=4, netatmo="ok", heart="big")))
    out.append(("Update heart small (diastole/rest) - clock",
                time_screen("Wednesday", 14, 9,
                            temp_line="+22" + DEG + "C 55%", date_str="04/06/2026",
                            wifi_bars=4, netatmo="ok", heart="small")))
    out.append(("Update heart BIG (beat) - weather",
                weather_screen("Madrid", "22", "C", "Sunny",
                               "H:40% 12km/h 1014hPa", code=113, night=False, heart="big")))
    out.append(("Update heart small (rest) - weather",
                weather_screen("Bilbao", "14", "C", "Light rain",
                               "H:88% 20km/h 1008hPa", code=296, night=False, heart="small")))
    # --- Clock faces (top-right: WiFi meter + Netatmo mark) ---
    out.append(("Clock 24h, WiFi full + Netatmo OK",
                time_screen("Wednesday", 14, 9,
                            temp_line="+22" + DEG + "C 55%", date_str="04/06/2026",
                            wifi_bars=4, netatmo="ok")))
    out.append(("Clock 12h + seconds, Netatmo OK",
                time_screen("Sunday", 9, 7, ss=5, ampm="AM",
                            temp_line="+18" + DEG + "C 60%", date_str="06/04/2026",
                            wifi_bars=3, netatmo="ok")))
    out.append(("Clock 24h + seconds, Netatmo ERROR (!)",
                time_screen("Friday", 23, 41, ss=58,
                            temp_line="+7" + DEG + "C 80%", date_str="12/12/2026",
                            wifi_bars=2, netatmo="error")))
    out.append(("Clock, weak WiFi, Netatmo disabled",
                time_screen("Monday", 6, 30, temp_line="N/A", date_str="01/01/2027",
                            wifi_bars=1, netatmo=None)))
    # --- Weather faces (with icons) ---
    out.append(("Weather: Sunny (113, day)",
                weather_screen("Madrid", "22", "C", "Sunny",
                               "H:40% 12km/h 1014hPa", code=113, night=False)))
    out.append(("Weather: Light rain (296)",
                weather_screen("Bilbao", "14", "C", "Light rain",
                               "H:88% 20km/h 1008hPa", code=296, night=False)))
    out.append(("Weather: Snow at night (338)",
                weather_screen("Oslo", "-3", "C", "Light snow",
                               "H:91% 8km/h 1001hPa", code=338, night=True)))
    out.append(("Weather: Thunder (200)",
                weather_screen("Miami", "84", "F", "Thundery outbreaks",
                               "H:75% 9mph 29.7in", code=200, night=False)))
    out.append(("Weather: Partly cloudy night (116)",
                weather_screen("Tokyo", "19", "C", "Partly cloudy",
                               "H:55% 11km/h 1016hPa", code=116, night=True)))
    # Note: drawWeatherScreen() is only ever called with weather_valid == true and
    # a real temperature, so its internal "N/A" branch is dead code -- not shown.
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "screens.png"))
    args = ap.parse_args()
    sheet = montage(examples())
    sheet.save(args.out)
    print("saved", args.out)


if __name__ == "__main__":
    main()
