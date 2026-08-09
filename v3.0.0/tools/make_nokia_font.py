#!/usr/bin/env python3
"""nokiafc22.ttf -> NokiaFC.h (lgfx::GFXfont fuer M5GFX), in zwei Groessen.

ERZEUGT NokiaFC.h NEU. Diese Datei nicht von Hand aendern, sondern hier.
Der Arduino-Build ignoriert .py und .ttf, sie stoeren im Sketchordner nicht.

Die Schrift ist eine Pixelschrift auf einem 1/8-em-Raster (2048 Units/em,
256 Units je Pixel). Bei ppem 8 wird sie 1:1 abgebildet, bei ppem 16 exakt
verdoppelt - jeder Designpixel wird zu einem 2x2-Block, ohne Kantenglaettung.
Deshalb werden beide Groessen einzeln gerastert statt setTextSize(2) zu
benutzen: so stimmen textWidth() und fontHeight() im Sketch von allein.

Erzeugt werden:
  NokiaFC       ppem 16, die Hauptschrift der Oberflaeche
  NokiaFCSmall  ppem  8, Originalgroesse, fuer Streamtitel und VU-Beschriftung

Gebraucht wird freetype-py. Einmalig einrichten und laufen lassen:

    cd ~/Documents/Arduino/M5Cardputer_WebRadio
    python3 -m venv venv && ./venv/bin/pip install freetype-py
    ./venv/bin/python make_nokia_font.py

Schriftquelle: Nokia Cellphone FC Small von Zeh Fernando (2003), frei nutzbar.
Die TTF liegt daneben; fehlt sie, wird im Download-Ordner nachgesehen.
"""
import os
import freetype

HERE = os.path.dirname(os.path.abspath(__file__))

TTF = os.path.join(HERE, 'nokiafc22.ttf')
if not os.path.exists(TTF):
    TTF = os.path.expanduser('~/Downloads/nokia-cellphone-fc/nokiafc22.ttf')

OUT = os.path.join(HERE, 'NokiaFC.h')

FIRST, LAST = 0x20, 0x7E

SIZES = [
    ('NokiaFC',      16, 'Hauptschrift, doppelte Entwurfsgroesse'),
    ('NokiaFCSmall',  8, 'Originalgroesse, halb so gross wie NokiaFC'),
]


def build(face, ppem):
    """Glyphen rastern und im Adafruit-GFX-Format packen."""
    face.set_pixel_sizes(0, ppem)
    data = bytearray()
    glyphs = []

    for code in range(FIRST, LAST + 1):
        face.load_char(chr(code), freetype.FT_LOAD_RENDER
                                | freetype.FT_LOAD_TARGET_MONO
                                | freetype.FT_LOAD_NO_HINTING)
        g, b = face.glyph, face.glyph.bitmap
        w, h, pitch = b.width, b.rows, b.pitch

        offset = len(data)
        # Zeilen fortlaufend bitweise packen (MSB zuerst), am Glyphenende auf
        # volle Bytes auffuellen - genau das Format, das Adafruit-GFX erwartet.
        acc, nbits = 0, 0
        for row in range(h):
            src = b.buffer[row * pitch:(row + 1) * pitch]
            for i in range(w):
                acc = (acc << 1) | ((src[i >> 3] >> (7 - (i & 7))) & 1)
                nbits += 1
                if nbits == 8:
                    data.append(acc)
                    acc, nbits = 0, 0
        if nbits:
            data.append(acc << (8 - nbits))

        glyphs.append(dict(code=code, offset=offset, w=w, h=h,
                           xadv=g.advance.x // 64,
                           xoff=g.bitmap_left, yoff=-g.bitmap_top))

    return data, glyphs, face.size.height // 64


face = freetype.Face(TTF)

out = []
out.append('// ' + '=' * 73)
out.append('// Nokia Cellphone FC (Small) von Zeh Fernando, aus nokiafc22.ttf')
out.append('// erzeugt - die Pixelschrift der alten Nokia-Handys.')
out.append('//')
out.append('// Zwei Groessen, beide auf ganze Pixel gerastert (kein Antialiasing):')
out.append('//   NokiaFC       bei 16 px, jeder Originalpixel ein 2x2-Block')
out.append('//   NokiaFCSmall  bei  8 px, Originalgroesse')
out.append('// Deshalb im Sketch kein setTextSize() noetig, textWidth() und')
out.append('// fontHeight() liefern gleich die echten Masse.')
out.append('//')
out.append(f'// Zeichen 0x{FIRST:02X}..0x{LAST:02X} (ASCII), proportional.')
out.append('//')
out.append('// Nicht von Hand aendern - wird aus der TTF erzeugt.')
out.append('// ' + '=' * 73)
out.append('')
out.append('#pragma once')
out.append('')
out.append('#include <M5GFX.h>')
out.append('')

summary = []

for name, ppem, note in SIZES:
    data, glyphs, y_advance = build(face, ppem)
    ascent  = max(-g['yoff'] for g in glyphs)
    descent = max(g['h'] + g['yoff'] for g in glyphs)
    summary.append((name, ppem, ascent, descent, y_advance, len(data), len(glyphs)))

    out.append('// ' + '-' * 73)
    out.append(f'// {name}: {note}')
    out.append(f'// Oberlaenge {ascent} px, Unterlaenge {descent} px,'
               f' fontHeight() = {ascent + descent} px, yAdvance {y_advance} px.')
    out.append('// ' + '-' * 73)
    out.append(f'static const uint8_t {name}Bitmaps[] PROGMEM = {{')
    for i in range(0, len(data), 12):
        out.append('  ' + ', '.join(f'0x{b:02X}' for b in data[i:i + 12]) + ',')
    out.append('};')
    out.append('')
    out.append(f'static const lgfx::GFXglyph {name}Glyphs[] PROGMEM = {{')
    for g in glyphs:
        ch = chr(g['code'])
        label = 'Leerzeichen' if ch == ' ' else f"'{ch}'"
        out.append('  {{ {:5d}, {:3d}, {:3d}, {:3d}, {:4d}, {:4d} }},   // 0x{:02X} {}'.format(
            g['offset'], g['w'], g['h'], g['xadv'], g['xoff'], g['yoff'],
            g['code'], label))
    out.append('};')
    out.append('')
    out.append(f'static const lgfx::GFXfont {name} PROGMEM = {{')
    out.append(f'  (uint8_t *){name}Bitmaps,')
    out.append(f'  (lgfx::GFXglyph *){name}Glyphs,')
    out.append(f'  0x{FIRST:02X}, 0x{LAST:02X}, {y_advance} }};')
    out.append('')

open(OUT, 'w').write('\n'.join(out))

print(OUT)
for name, ppem, asc, desc, yadv, nbytes, nglyphs in summary:
    print(f'  {name:14s} ppem {ppem:2d}  Oberlaenge {asc:2d}  Unterlaenge {desc}'
          f'  fontHeight {asc + desc:2d}  yAdvance {yadv:2d}'
          f'  {nbytes:5d} Bytes Bitmap + {nglyphs * 8} Bytes Tabelle')
