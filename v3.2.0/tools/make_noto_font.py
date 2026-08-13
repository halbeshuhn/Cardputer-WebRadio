#!/usr/bin/env python3
"""NotoSans.ttf -> NotoSans.h (lgfx::GFXfont fuer M5GFX), eine Groesse.

ERZEUGT NotoSans.h NEU. Diese Datei nicht von Hand aendern, sondern hier.
Aufbau und Ausgabeformat sind dieselben wie in make_nokia_font.py; die
Unterschiede stehen unten.

    cd ~/Documents/Arduino/M5Cardputer_WebRadio
    python3 -m venv venv && ./venv/bin/pip install freetype-py
    ./venv/bin/python make_noto_font.py

Schriftquelle: Noto Sans Regular von Google, SIL Open Font License 1.1. Geholt
als variable Schrift aus dem Google-Fonts-Bestand:

    https://github.com/google/fonts/raw/main/ofl/notosans/NotoSans%5Bwdth,wght%5D.ttf

Freetype nimmt davon die Vorgabeinstanz, und die ist Regular (Gewicht 400,
Breite 100). Die TTF liegt als NotoSans.ttf daneben, der Arduino-Build
uebergeht sie wie die anderen .ttf und .py auch.

Zwei Unterschiede zum Nokia-Erzeuger, beide erzwungen durch die Vorlage:

1. Noto Sans ist eine Konturschrift, keine Pixelschrift auf einem Raster.
   Ohne Hinting zerfallen die duennen Stellen bei 10 px. Deshalb laeuft der
   Autohinter mit (FT_LOAD_FORCE_AUTOHINT), der die Striche auf die Pixel-
   kanten zieht. Kantenglaettung bleibt aus - das Panel hat sie nicht noetig
   und Graustufen wuerden das Format sprengen.

2. Die Schrift ist proportional, und zwar mit stark unterschiedlichen
   Breiten (i drei Pixel, m neun). Wer damit umbricht, muss messen; eine
   Rechnung mit fester Zeichenbreite geht daneben. drawTitleBlock() im
   Sketch tut das ueber textWidth().
"""
import os
import freetype

HERE = os.path.dirname(os.path.abspath(__file__))

TTF = os.path.join(HERE, 'NotoSans.ttf')
OUT = os.path.join(HERE, 'NotoSans.h')

# Bis 0xFF statt bis 0x7E: Sendertitel deutscher und franzoesischer Sender
# enthalten Umlaute und Akzente, und die sind dort der Normalfall. 0x7F..0x9F
# sind in Latin-1 Steuerzeichen und kommen leer heraus - das kostet ein paar
# Bytes und erspart eine zweite Tabelle.
FIRST, LAST = 0x20, 0xFF

# 14 px gerastert: Oberlaenge 12, Unterlaenge 4, also 16 px Tinte und
# yAdvance 19. Mittlerer Vorschub 7,4 px, damit rund 31 Zeichen je Zeile.
NAME = 'NotoSans'
PPEM = 14


def build(face, ppem):
    """Glyphen rastern und im Adafruit-GFX-Format packen."""
    face.set_pixel_sizes(0, ppem)
    data = bytearray()
    glyphs = []

    for code in range(FIRST, LAST + 1):
        face.load_char(chr(code), freetype.FT_LOAD_RENDER
                                | freetype.FT_LOAD_TARGET_MONO
                                | freetype.FT_LOAD_FORCE_AUTOHINT)
        g, b = face.glyph, face.glyph.bitmap
        w, h, pitch = b.width, b.rows, b.pitch

        offset = len(data)
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

data, glyphs, y_advance = build(face, PPEM)
ascent  = max(-g['yoff'] for g in glyphs)
descent = max(g['h'] + g['yoff'] for g in glyphs)

out = []
out.append('// ' + '=' * 73)
out.append('// Noto Sans Regular von Google, aus NotoSans.ttf erzeugt.')
out.append('// SIL Open Font License 1.1.')
out.append('//')
out.append(f'// Eine Groesse: {PPEM} px, mit Autohinter auf ganze Pixel gerastert,')
out.append('// ohne Kantenglaettung. Oberlaenge '
           f'{ascent} px, Unterlaenge {descent} px,')
out.append(f'// fontHeight() = {ascent + descent} px, yAdvance {y_advance} px.')
out.append('//')
out.append('// Proportional, und deutlich staerker als die Nokia-Schrift: das i ist')
out.append('// drei Pixel breit, das m neun. Wer damit Zeilen umbricht, muss mit')
out.append('// textWidth() messen statt mit einer festen Zeichenbreite zu rechnen.')
out.append('//')
out.append(f'// Zeichen 0x{FIRST:02X}..0x{LAST:02X} (ASCII).')
out.append('//')
out.append('// Nicht von Hand aendern - wird aus der TTF erzeugt,'
           ' siehe make_noto_font.py.')
out.append('// ' + '=' * 73)
out.append('')
out.append('#pragma once')
out.append('')
out.append('#include <M5GFX.h>')
out.append('')
out.append(f'static const uint8_t {NAME}Bitmaps[] PROGMEM = {{')
for i in range(0, len(data), 12):
    out.append('  ' + ', '.join(f'0x{b:02X}' for b in data[i:i + 12]) + ',')
out.append('};')
out.append('')
out.append(f'static const lgfx::GFXglyph {NAME}Glyphs[] PROGMEM = {{')
for g in glyphs:
    ch = chr(g['code'])
    label = 'Leerzeichen' if ch == ' ' else f"'{ch}'"
    out.append('  {{ {:5d}, {:3d}, {:3d}, {:3d}, {:4d}, {:4d} }},   // 0x{:02X} {}'.format(
        g['offset'], g['w'], g['h'], g['xadv'], g['xoff'], g['yoff'],
        g['code'], label))
out.append('};')
out.append('')
out.append(f'static const lgfx::GFXfont {NAME} PROGMEM = {{')
out.append(f'  (uint8_t *){NAME}Bitmaps,')
out.append(f'  (lgfx::GFXglyph *){NAME}Glyphs,')
out.append(f'  0x{FIRST:02X}, 0x{LAST:02X}, {y_advance} }};')
out.append('')

open(OUT, 'w').write('\n'.join(out))

print(OUT)
print(f'  {NAME:14s} ppem {PPEM:2d}  Oberlaenge {ascent:2d}  Unterlaenge {descent}'
      f'  fontHeight {ascent + descent:2d}  yAdvance {y_advance:2d}'
      f'  {len(data):5d} Bytes Bitmap + {len(glyphs) * 8} Bytes Tabelle')
