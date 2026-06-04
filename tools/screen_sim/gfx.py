"""A faithful, bit-for-bit reimplementation of the slice of Adafruit_GFX +
SSD1306 (128x64, 1-bit) that the firmware uses to draw its two screens.

It mirrors the real algorithms so the simulated framebuffer matches what the
OLED would show pixel-for-pixel:
  - classic 5x7 font  (glcdfont.c)        -> setFont(NULL)
  - custom GFX fonts  (FreeMonoBold*pt7b) -> setFont(&font)
  - getTextBounds / charBounds            -> used by the firmware for centering
  - drawPixel/drawCircle/drawFastHLine/drawBitmap

References: Adafruit_GFX.cpp (drawChar, charBounds, getTextBounds, write).
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
FONTS = os.path.join(HERE, "_fonts")

WIDTH = 128
HEIGHT = 64


# --------------------------------------------------------------------------
# Font loading
# --------------------------------------------------------------------------
def _load_classic():
    """Parse glcdfont.c -> 256*5 byte table (cp437, column-major, LSB=top)."""
    txt = open(os.path.join(FONTS, "glcdfont.c"), encoding="utf-8").read()
    body = txt[txt.index("{") + 1: txt.rindex("}")]
    bytes_ = [int(b, 16) for b in re.findall(r"0[xX][0-9a-fA-F]+", body)]
    assert len(bytes_) >= 256 * 5, len(bytes_)
    return bytes_[: 256 * 5]


class GFXFont:
    """A parsed custom Adafruit GFX font (FreeMonoBold*pt7b.h)."""

    def __init__(self, path):
        txt = open(path, encoding="utf-8").read()
        # Bitmaps array
        bm = re.search(r"Bitmaps\[\]\s*PROGMEM\s*=\s*\{(.*?)\};", txt, re.S).group(1)
        self.bitmap = [int(b, 16) for b in re.findall(r"0[xX][0-9a-fA-F]+", bm)]
        # Glyphs array: each {bitmapOffset, width, height, xAdvance, xOffset, yOffset}
        gl = re.search(r"Glyphs\[\]\s*PROGMEM\s*=\s*\{(.*?)\};", txt, re.S).group(1)
        self.glyphs = []
        for m in re.finditer(r"\{\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,"
                             r"\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}", gl):
            o, w, h, xa, xo, yo = (int(v) for v in m.groups())
            self.glyphs.append((o, w, h, xa, xo, yo))
        # Font struct: ...Bitmaps, ...Glyphs, first, last, yAdvance };
        # The three trailing numeric literals are first, last, yAdvance.
        fs = re.search(r"GFXfont\s+\w+\s*PROGMEM\s*=\s*\{(.*?)\}\s*;", txt, re.S).group(1)
        nums = re.findall(r"0[xX][0-9a-fA-F]+|\d+", fs)
        first, last, yadv = nums[-3:]
        self.first = int(first, 0)
        self.last = int(last, 0)
        self.yadvance = int(yadv, 0)

    def glyph(self, c):
        if c < self.first or c > self.last:
            return None
        return self.glyphs[c - self.first]


CLASSIC = _load_classic()

# Degree symbol: the firmware prints (char)247 for the degree sign and the author
# verified it shows as '°' on the real build ("your display's charset"). The
# position of the degree glyph differs between versions of Adafruit's classic
# font: in the current CP437-correct font 0xF7 (247) is '≈' and 0xF8 (248) is the
# degree ring. The firmware's toolchain has the degree at 247, so we mirror that
# here by pointing char 247 at the degree-ring glyph the font already carries.
CLASSIC[247 * 5: 247 * 5 + 5] = CLASSIC[248 * 5: 248 * 5 + 5]


# --------------------------------------------------------------------------
# Display: 1-bit framebuffer + the GFX drawing primitives we need
# --------------------------------------------------------------------------
class Display:
    def __init__(self):
        self.buf = [[0] * WIDTH for _ in range(HEIGHT)]
        self.font = None          # None => classic
        self.cx = 0
        self.cy = 0
        self.size = 1
        self.wrap = True

    # --- primitives ---
    def pixel(self, x, y, on=1):
        if 0 <= x < WIDTH and 0 <= y < HEIGHT:
            self.buf[y][x] = on

    def hline(self, x, y, w):
        for i in range(w):
            self.pixel(x + i, y)

    def circle(self, x0, y0, r):
        # Adafruit drawCircle (Bresenham, outline only)
        f = 1 - r
        ddF_x, ddF_y = 1, -2 * r
        x, y = 0, r
        self.pixel(x0, y0 + r); self.pixel(x0, y0 - r)
        self.pixel(x0 + r, y0); self.pixel(x0 - r, y0)
        while x < y:
            if f >= 0:
                y -= 1; ddF_y += 2; f += ddF_y
            x += 1; ddF_x += 2; f += ddF_x
            for sx, sy in ((x, y), (-x, y), (x, -y), (-x, -y),
                           (y, x), (-y, x), (y, -x), (-y, -x)):
                self.pixel(x0 + sx, y0 + sy)

    def bitmap(self, x, y, data, w, h):
        """drawBitmap: 1-bit, MSB-first, row-padded to whole bytes."""
        row_bytes = (w + 7) // 8
        for j in range(h):
            for i in range(w):
                byte = data[j * row_bytes + (i >> 3)]
                if byte & (0x80 >> (i & 7)):
                    self.pixel(x + i, y + j)

    # --- text state ---
    def setFont(self, font):
        self.font = font

    def setCursor(self, x, y):
        self.cx, self.cy = x, y

    # --- char drawing ---
    def _draw_classic(self, c):
        if c == ord("\n"):
            self.cx = 0; self.cy += self.size * 8; return
        if c == ord("\r"):
            return
        for i in range(5):
            line = CLASSIC[c * 5 + i]
            for j in range(8):
                if line & 1:
                    if self.size == 1:
                        self.pixel(self.cx + i, self.cy + j)
                    else:
                        for a in range(self.size):
                            for b in range(self.size):
                                self.pixel(self.cx + i * self.size + a,
                                           self.cy + j * self.size + b)
                line >>= 1
        self.cx += self.size * 6

    def _draw_custom(self, c):
        if c == ord("\n"):
            self.cx = 0; self.cy += self.size * self.font.yadvance; return
        if c == ord("\r"):
            return
        g = self.font.glyph(c)
        if g is None:
            return
        bo, w, h, xa, xo, yo = g
        bits = 0; bit = 0
        for yy in range(h):
            for xx in range(w):
                if (bit & 7) == 0:
                    bits = self.font.bitmap[bo]; bo += 1
                bit += 1
                if bits & 0x80:
                    self.pixel(self.cx + xo + xx, self.cy + yo + yy)
                bits = (bits << 1) & 0xFF
        self.cx += xa * self.size

    def write(self, text):
        for ch in text:
            c = ord(ch)
            if self.font is None:
                self._draw_classic(c)
            else:
                self._draw_custom(c)

    # alias matching the firmware
    def print(self, text):
        self.write(str(text))

    # --- measurement (mirrors charBounds / getTextBounds) ---
    def getTextBounds(self, text, x, y):
        minx, miny, maxx, maxy = WIDTH, HEIGHT, -1, -1
        cx, cy = x, y
        for ch in text:
            c = ord(ch)
            if self.font is None:
                if c == ord("\n"):
                    cx = 0; cy += self.size * 8
                elif c != ord("\r"):
                    if self.wrap and (cx + self.size * 6) > WIDTH:
                        cx = 0; cy += self.size * 8
                    x2 = cx + self.size * 6 - 1
                    y2 = cy + self.size * 8 - 1
                    minx = min(minx, cx); miny = min(miny, cy)
                    maxx = max(maxx, x2); maxy = max(maxy, y2)
                    cx += self.size * 6
            else:
                if c == ord("\n"):
                    cx = 0; cy += self.size * self.font.yadvance
                elif c != ord("\r"):
                    g = self.font.glyph(c)
                    if g is not None:
                        bo, gw, gh, xa, xo, yo = g
                        if self.wrap and (cx + self.size * (xo + gw)) > WIDTH:
                            cx = 0; cy += self.size * self.font.yadvance
                        x1 = cx + self.size * xo
                        y1 = cy + self.size * yo
                        x2 = x1 + self.size * gw - 1
                        y2 = y1 + self.size * gh - 1
                        minx = min(minx, x1); miny = min(miny, y1)
                        maxx = max(maxx, x2); maxy = max(maxy, y2)
                        cx += self.size * xa
        w = (maxx - minx + 1) if maxx >= minx else 0
        h = (maxy - miny + 1) if maxy >= miny else 0
        return w, h


FreeMonoBold12 = GFXFont(os.path.join(FONTS, "FreeMonoBold12pt7b.h"))
FreeMonoBold18 = GFXFont(os.path.join(FONTS, "FreeMonoBold18pt7b.h"))
