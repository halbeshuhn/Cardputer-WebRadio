#!/usr/bin/env python3
"""Nachbau der M5GFX-Zeichenroutinen fuer den Mac - malt in ein PNG statt aufs
Display des Cardputer.

Nachgebildet ist nur, was der Sketch wirklich benutzt: fillScreen, fillRect,
drawRect, drawPixel, drawString/drawCentreString/drawRightString, setTextColor,
setFont, setTextSize, textWidth, fontHeight. Das sind zehn Aufrufe, mehr kommt
im Sketch nicht vor.

Die Schriften werden nicht nachgebaut, sondern aus denselben Quellen gelesen,
die auch der Build benutzt:
  NokiaFC / NokiaFCSmall  aus NokiaFC.h daneben
  Font0                   aus glcdfont.h der M5GFX-Bibliothek
Die Farbnamen (TFT_RED und Verwandte) kommen aus enum.hpp derselben
Bibliothek. Damit kann hier nichts auseinanderlaufen, was dort geaendert wird.

Die Satzregeln - Grundlinie, Vorschub, Hintergrundfeld eines Zeichens - sind
aus lgfx_fonts.cpp und LGFXBase.cpp uebernommen (GFXfont::drawChar,
GLCDfont::drawChar, LGFXBase::draw_string, text_width). Deshalb stimmen
Umbrueche und Breiten pixelgenau, nicht nur ungefaehr.

Alle Farben laufen durch die 16-Bit-Wandlung des Panels (RGB565), auch die mit
color888 gesetzten. Was hier im Bild steht, ist also die Farbe, die das Geraet
tatsaechlich anzeigt - nicht die, die im Quelltext steht.
"""
import os
import re
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))

SKETCH_NAME = 'M5Cardputer_WebRadio.ino'


def _find_sketch_dir():
    """Der Ordner mit dem Sketch.

    Zwei Anordnungen kommen vor: in der Arbeitskopie liegt tools/ im
    Sketchordner selbst, im Repo daneben, und der Sketch steckt dort in einem
    eigenen Unterordner.
    """
    up = os.path.dirname(HERE)
    for c in (up, os.path.join(up, 'M5Cardputer_WebRadio')):
        if os.path.exists(os.path.join(c, SKETCH_NAME)):
            return c
    return up


SKETCH_DIR = _find_sketch_dir()
SKETCH = os.path.join(SKETCH_DIR, SKETCH_NAME)


def _find_m5gfx():
    """Den src-Ordner der M5GFX-Bibliothek suchen.

    Von dort kommen der Zeichensatz Font0 und die TFT_-Farbnamen. Wer die
    Bibliothek woanders liegen hat, setzt M5GFX_DIR.
    """
    candidates = [os.environ.get('M5GFX_DIR')] if os.environ.get('M5GFX_DIR') else []
    candidates += [
        os.path.expanduser('~/Documents/Arduino/libraries/M5GFX/src'),
        os.path.expanduser('~/Arduino/libraries/M5GFX/src'),
        os.path.expanduser(
            '~/Library/Arduino15/libraries/M5GFX/src'),
        '/usr/share/arduino/libraries/M5GFX/src',
    ]
    for c in candidates:
        if c and os.path.exists(os.path.join(c, 'lgfx/v1/misc/enum.hpp')):
            return c
    raise RuntimeError(
        'M5GFX nicht gefunden. Gesucht wurde in:\n  '
        + '\n  '.join(c for c in candidates if c)
        + '\nDen src-Ordner der Bibliothek in M5GFX_DIR eintragen, etwa:\n'
          '  M5GFX_DIR=~/Arduino/libraries/M5GFX/src python3 tools/screen_main.py')


LIB_DIR = _find_m5gfx()

WIDTH = 240
HEIGHT = 135


# ---------------------------------------------------------------------------
# Farben. Intern wird ueberall mit 8-8-8 gerechnet, aber jede Farbe wird vorher
# durch 5-6-5 geschickt - genau wie im Panel.
# ---------------------------------------------------------------------------

def rgb565_to_888(v):
    """Ein 16-Bit-Wort auf die Farbe aufziehen, die das Panel daraus macht."""
    r = (v >> 11) & 0x1F
    g = (v >> 5) & 0x3F
    b = v & 0x1F
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def color565(r, g, b):
    """Wie M5GFX color565(): drei Bytes zu einem 16-Bit-Wort."""
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def color888(r, g, b):
    """Wie lgfx::color888(). Der Wert wird trotzdem im Panel gerundet."""
    return rgb565_to_888(color565(r, g, b))


def c565(v):
    """Ein rohes 16-Bit-Wort, so wie TFT_RED und Konsorten im Sketch stehen."""
    return rgb565_to_888(v)


def rgb(r, g, b):
    """Kurzform fuer color565(r, g, b) an einer Zeichenstelle."""
    return rgb565_to_888(color565(r, g, b))


def load_tft_colors():
    """TFT_-Namen aus enum.hpp der Bibliothek ziehen, nicht abschreiben."""
    path = os.path.join(LIB_DIR, 'lgfx/v1/misc/enum.hpp')
    out = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = re.search(r'constexpr int (TFT_\w+)\s*=\s*(0x[0-9A-Fa-f]+)', line)
            if m:
                out[m.group(1)] = int(m.group(2), 16)
    if not out:
        raise RuntimeError('keine TFT_-Farben in %s gefunden' % path)
    return out


TFT = load_tft_colors()


# ---------------------------------------------------------------------------
# Schriften
# ---------------------------------------------------------------------------

def _c_array(text, name):
    """Den Rumpf eines C-Arrays namens `name` aus `text` holen."""
    m = re.search(r'\b' + name + r'\s*\[\s*\]\s*(?:PROGMEM\s*)?=\s*\{(.*?)\n\}\s*;',
                  text, re.S)
    if not m:
        raise RuntimeError('Array %s nicht gefunden' % name)
    return m.group(1)


class GfxFont:
    """Schrift im Adafruit-GFX-Format, wie sie make_nokia_font.py erzeugt."""

    def __init__(self, name, bitmaps, glyphs, first, last, y_advance):
        self.name = name
        self.bitmaps = bitmaps
        self.glyphs = glyphs          # je Zeichen (offset, w, h, xadv, xoff, yoff)
        self.first = first
        self.last = last
        self.y_advance = y_advance

        # getDefaultMetric(): Grundlinie ist der groesste Ueberstand nach oben,
        # die Hoehe reicht bis zum tiefsten Unterlaengenpixel.
        above = 0
        below = 0
        for (_o, _w, h, _xa, _xo, yo) in glyphs:
            ab = -yo
            if ab > above:
                above = ab
            bb = h - ab
            if bb > below:
                below = bb
        self.baseline = above
        self.height = above + below

    def glyph(self, code):
        if code < self.first or code > self.last:
            return None
        return self.glyphs[code - self.first]

    def metric(self, code):
        """(x_offset, width, x_advance) - wie GFXfont::updateFontMetric."""
        g = self.glyph(code) or self.glyph(0x20)
        if g is None:
            return (0, self.y_advance >> 1, self.y_advance >> 1)
        return (g[4], g[1], g[3])

    def rows(self, g):
        """Bitmuster eines Zeichens zeilenweise, ein bool je Pixel."""
        off, w, h = g[0], g[1], g[2]
        out = []
        bit = 0
        for _ in range(h):
            row = []
            for _ in range(w):
                byte = self.bitmaps[off + (bit >> 3)]
                row.append(bool(byte & (0x80 >> (bit & 7))))
                bit += 1
            out.append(row)
        return out


def load_gfx_fonts(path, names):
    """Schriften im GFXfont-Format aus einem erzeugten Header lesen.

    Benutzt fuer NokiaFC.h (zwei Groessen) und NotoSans.h (eine). Beide
    Dateien entstehen aus einer TTF, siehe make_nokia_font.py und
    make_noto_font.py.
    """
    with open(path, encoding='utf-8', errors='replace') as f:
        text = f.read()

    fonts = {}
    for name in names:
        raw = _c_array(text, name + 'Bitmaps')
        bitmaps = [int(v, 16) for v in re.findall(r'0x([0-9A-Fa-f]{2})', raw)]

        glyphs = []
        for line in _c_array(text, name + 'Glyphs').splitlines():
            m = re.search(r'\{([^}]*)\}', line)
            if m:
                vals = [int(v) for v in m.group(1).split(',')]
                glyphs.append(tuple(vals))

        m = re.search(r'GFXfont ' + name + r'\s+PROGMEM\s*=\s*\{(.*?)\}\s*;',
                      text, re.S)
        nums = re.findall(r'(0x[0-9A-Fa-f]+|\d+)\s*(?=[,}])', m.group(1) + '}')
        first, last, yadv = (int(n, 0) for n in nums[-3:])

        fonts[name] = GfxFont(name, bitmaps, glyphs, first, last, yadv)
    return fonts


def load_nokia_fonts(path=None):
    return load_gfx_fonts(path or os.path.join(SKETCH_DIR, 'NokiaFC.h'),
                          ('NokiaFC', 'NokiaFCSmall'))


def load_noto_font(path=None):
    return load_gfx_fonts(path or os.path.join(SKETCH_DIR, 'NotoSans.h'),
                          ('NotoSans',))


class GlcdFont:
    """Font0: fester 6x8-Zeichensatz, 5 Datenspalten und eine Spalte Luft."""

    name = 'Font0'
    width = 6
    height = 8
    baseline = 7
    datawidth = 5

    def __init__(self, data):
        self.data = data

    def metric(self, _code):
        return (0, self.width, self.width)


def load_glcd_font(path=None):
    path = path or os.path.join(LIB_DIR, 'lgfx/Fonts/glcdfont.h')
    with open(path, encoding='utf-8', errors='replace') as f:
        raw = _c_array(f.read(), 'font')
    return GlcdFont([int(v, 16) for v in re.findall(r'0x([0-9A-Fa-f]{2})', raw)])


# ---------------------------------------------------------------------------
# Die Anzeige
# ---------------------------------------------------------------------------

class Display:
    """Ein Bildspeicher mit den Zeichenbefehlen, die der Sketch benutzt."""

    def __init__(self, width=WIDTH, height=HEIGHT):
        self._w = width
        self._h = height
        self.buf = bytearray(width * height * 3)

        self.fonts = dict(load_nokia_fonts())
        self.fonts.update(load_noto_font())
        self.fonts['Font0'] = load_glcd_font()

        self.font = self.fonts['NokiaFC']
        self.fore = (255, 255, 255)
        self.back = (0, 0, 0)
        self.size_x = 1
        self.size_y = 1

    # -- Zustand ----------------------------------------------------------
    def width(self):
        return self._w

    def height(self):
        return self._h

    def set_font(self, name):
        if name not in self.fonts:
            raise KeyError('unbekannte Schrift: %s' % name)
        self.font = self.fonts[name]

    def set_text_color(self, fore, back=None):
        self.fore = fore
        self.back = back if back is not None else fore

    def set_text_size(self, n):
        # Gebrochene Faktoren sind erlaubt - M5GFX rechnet sie in 16.16-
        # Festkomma um, siehe _draw_char_glcd(). setTextSize(1.5f) benutzen
        # die WLAN-Masken, und seit v3.1.2 der Streamtitel.
        self.size_x = self.size_y = float(n)

    def font_height(self):
        return int(self.font.height * self.size_y)

    # -- Flaechen ---------------------------------------------------------
    def _px(self, x, y, col):
        if 0 <= x < self._w and 0 <= y < self._h:
            i = (y * self._w + x) * 3
            self.buf[i:i + 3] = bytes(col)

    def draw_pixel(self, x, y, col):
        self._px(int(x), int(y), col)

    def fill_rect(self, x, y, w, h, col):
        x, y, w, h = int(x), int(y), int(w), int(h)
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                self._px(xx, yy, col)

    def draw_fast_hline(self, x, y, w, col):
        for xx in range(int(x), int(x) + int(w)):
            self._px(xx, int(y), col)

    def fill_triangle(self, x0, y0, x1, y1, x2, y2, col):
        """Reicht fuer die kleinen Scrollmarken. Kantentest im umgebenden
        Rechteck - langsam, aber hier sind es sieben mal fuenf Pixel."""
        pts = [(x0, y0), (x1, y1), (x2, y2)]
        xs = [int(p[0]) for p in pts]
        ys = [int(p[1]) for p in pts]

        def seite(ax, ay, bx, by, px, py):
            return (bx - ax) * (py - ay) - (by - ay) * (px - ax)

        for yy in range(min(ys), max(ys) + 1):
            for xx in range(min(xs), max(xs) + 1):
                d1 = seite(x0, y0, x1, y1, xx, yy)
                d2 = seite(x1, y1, x2, y2, xx, yy)
                d3 = seite(x2, y2, x0, y0, xx, yy)
                neg = (d1 < 0) or (d2 < 0) or (d3 < 0)
                pos = (d1 > 0) or (d2 > 0) or (d3 > 0)
                if not (neg and pos):
                    self._px(xx, yy, col)

    def draw_rect(self, x, y, w, h, col):
        x, y, w, h = int(x), int(y), int(w), int(h)
        for xx in range(x, x + w):
            self._px(xx, y, col)
            self._px(xx, y + h - 1, col)
        for yy in range(y, y + h):
            self._px(x, yy, col)
            self._px(x + w - 1, yy, col)

    def fill_screen(self, col):
        self.fill_rect(0, 0, self._w, self._h, col)

    # -- Text -------------------------------------------------------------
    def text_width(self, s):
        """LGFXBase::text_width, Schritt fuer Schritt uebernommen."""
        if not s:
            return 0
        left = 0
        right = 0
        for ch in s:
            xoff, w, xadv = self.font.metric(ord(ch))
            sxoff = xoff * self.size_x
            if left == 0 and right == 0 and xoff < 0:
                left = right = -sxoff
            right = left + max(xadv * self.size_x, w * self.size_x + sxoff)
            left += xadv * self.size_x
        return int(right)

    def draw_string(self, s, x, y, datum='top_left'):
        """Text setzen. y ist die Oberkante der Zeile (Vorgabe top_left)."""
        if not s:
            return 0

        cwidth = self.text_width(s)
        if datum == 'top_center':
            x -= cwidth >> 1
        elif datum == 'top_right':
            x -= cwidth

        # Steht das erste Zeichen links ueber, rueckt die ganze Zeile nach.
        sum_x = 0
        xoff0 = self.font.metric(ord(s[0]))[0]
        if xoff0 < 0:
            sum_x = -xoff0 * self.size_x

        filled_x = [0]
        for ch in s:
            sum_x += self._draw_char(ord(ch), x + sum_x, y, filled_x)
        return sum_x

    def draw_centre_string(self, s, x, y):
        return self.draw_string(s, x, y, 'top_center')

    def draw_right_string(self, s, x, y):
        return self.draw_string(s, x, y, 'top_right')

    def _draw_char(self, code, x, y, filled_x):
        if isinstance(self.font, GlcdFont):
            return self._draw_char_glcd(code, x, y)
        return self._draw_char_gfx(code, x, y, filled_x)

    def _draw_char_gfx(self, code, x, y, filled_x):
        """GFXfont::drawChar. y ist die Oberkante der Zeile."""
        font = self.font
        sx, sy = self.size_x, self.size_y
        fillbg = self.fore != self.back

        g = font.glyph(code) or font.glyph(0x20)
        if g is None:
            return 0
        _off, w, h, xadv, xoff, yoff = g

        x_advance = xadv * sx
        x_offset = xoff * sx

        if fillbg:
            left = max(filled_x[0], x + (x_offset if x_offset < 0 else 0))
            right = x + max(w * sx + x_offset, x_advance)
            filled_x[0] = right
        else:
            left = right = 0

        # Zeilenoberkante des Zeichens: Grundlinie plus sein eigener Versatz.
        y_off = font.baseline + yoff

        if left < right:
            if y_off > 0:
                self.fill_rect(left, y, right - left, y_off * sy, self.back)
            below = (y_off + h) * sy
            total = font.height * sy
            if below < total:
                self.fill_rect(left, y + below, right - left, total - below,
                               self.back)

        gx = x + x_offset
        for r, row in enumerate(font.rows(g)):
            ry = y + (y_off + r) * sy
            if left < right:
                self.fill_rect(left, ry, right - left, sy, self.back)
            for c, on in enumerate(row):
                if on:
                    self.fill_rect(gx + c * sx, ry, sx, sy, self.fore)

        return x_advance

    def _draw_char_glcd(self, code, x, y):
        """GLCDfont::drawChar. 5 Datenspalten, danach eine Spalte Luft.

        Die Kantenlagen kommen wie in M5GFX aus 16.16-Festkomma: Spalte i
        reicht von (i*sx)>>16 bis ((i+1)*sx)>>16. Bei ganzzahligen Faktoren
        ist das dasselbe wie vorher, bei 1,5 entstehen abwechselnd 1 und 2 px
        breite Spalten - genau wie auf dem Geraet.
        """
        font = self.font
        sx = int(65536 * self.size_x)
        sy = int(65536 * self.size_y)
        fillbg = self.fore != self.back

        c = code + 1 if code >= 176 else code   # 'classic charset', wie M5GFX

        base = c * font.datawidth
        x1 = 0
        for i in range(font.datawidth):
            col = font.data[base + i] if base + i < len(font.data) else 0

            x0 = x1
            x1 = ((i + 1) * sx) >> 16
            w = x1 - x0

            for j in range(font.height):
                on = bool(col & (1 << j))
                y0 = (j * sy) >> 16
                y1 = ((j + 1) * sy) >> 16
                if (on or fillbg) and y1 > y0:
                    self.fill_rect(x + x0, y + y0, w, y1 - y0,
                                   self.fore if on else self.back)

        x2 = (font.width * sx) >> 16
        if fillbg and font.datawidth < font.width:
            self.fill_rect(x + x1, y, x2 - x1,
                           (font.height * sy) >> 16, self.back)
        return x2

    # -- Ausgabe ----------------------------------------------------------
    def save_png(self, path, scale=3):
        """Bild als PNG schreiben, vergroessert mit harten Pixelkanten."""
        w, h = self._w * scale, self._h * scale
        raw = bytearray()
        for y in range(self._h):
            row = bytearray()
            base = y * self._w * 3
            for x in range(self._w):
                row += self.buf[base + x * 3: base + x * 3 + 3] * scale
            for _ in range(scale):
                raw += b'\x00' + row

        def chunk(tag, data):
            return (struct.pack('>I', len(data)) + tag + data
                    + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF))

        png = (b'\x89PNG\r\n\x1a\n'
               + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
               + chunk(b'IDAT', zlib.compress(bytes(raw), 9))
               + chunk(b'IEND', b''))
        with open(path, 'wb') as f:
            f.write(png)
        return path
