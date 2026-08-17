"""Ersatzlogo 76x76 erzeugen: Vorschau als PNG, Rohdaten als RGB565.

Kein Sender, kein Bild, kein Speicher - in allen drei Faellen dasselbe
Zeichen. Es soll nach Absicht aussehen, nicht nach Fehler.
"""
import math, os, struct, sys
sys.path.insert(0, '/Users/ecki/Documents/Arduino/M5Cardputer_WebRadio/tools')
from gfx import Display, rgb

N = 76
d = Display(N, N)

HG    = rgb(8, 10, 14)      # fast schwarz, minimal blaeulich
RAHM  = rgb(52, 62, 76)
MAST  = rgb(150, 170, 190)
WELLE = rgb(88, 108, 130)

d.fill_rect(0, 0, N, N, HG)
d.draw_rect(0, 0, N, N, RAHM)

mx, my = 38, 27                      # Mastspitze

# Funkwellen: je zwei Klammern links und rechts der Spitze, wie im
# Sendesymbol. Zwei Pixel dick, damit sie bei 76 px nicht verschwinden.
import math
for r in (11, 17):
    for seite in (-1, 1):
        for zehntel in range(-420, 421):
            w = math.radians(zehntel / 10.0)
            x = mx + seite * (r * math.cos(w))
            y = my + (r * math.sin(w)) - 4
            xi, yi = int(round(x)), int(round(y))
            if 1 < xi < N - 1 and 1 < yi < N - 1:
                d.draw_pixel(xi, yi, WELLE)

# Spitze
d.fill_rect(mx - 1, my - 5, 3, 5, MAST)

# Mast: zwei Streben, nach unten auseinander
FUSS = 63
for y in range(my, FUSS + 1):
    t = (y - my) / float(FUSS - my)
    dx = 1 + int(round(t * 9))
    d.draw_pixel(mx - dx, y, MAST)
    d.draw_pixel(mx + dx, y, MAST)

# Querstreben
for y in (36, 45, 54, 62):
    t = (y - my) / float(FUSS - my)
    dx = 1 + int(round(t * 9))
    for x in range(mx - dx, mx + dx + 1):
        d.draw_pixel(x, y, MAST)

# Boden
for x in range(mx - 15, mx + 16):
    d.draw_pixel(x, FUSS + 3, RAHM)

d.save_png('/private/tmp/claude-501/-Users-ecki-Documents-Arduino/865dc204-b288-4c61-9af9-d7a96a7adea5/scratchpad/dummy_logo.png', scale=4)

# --- Rohdaten: RGB565, hoechstwertiges Byte zuerst, Zeile fuer Zeile -------
roh = bytearray()
for i in range(N * N):
    r, g, b = d.buf[i*3], d.buf[i*3+1], d.buf[i*3+2]
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    roh += bytes((v >> 8, v & 0xFF))

BASIS = '/private/tmp/claude-501/-Users-ecki-Documents-Arduino/865dc204-b288-4c61-9af9-d7a96a7adea5/scratchpad'
open(BASIS + '/logo_dummy.565', 'wb').write(roh)

# --- C-Feld fuer die Firmware ---------------------------------------------
werte = [ (roh[i] << 8) | roh[i+1] for i in range(0, len(roh), 2) ]
zeilen = []
for i in range(0, len(werte), 12):
    zeilen.append('  ' + ' '.join('0x%04X,' % v for v in werte[i:i+12]))
kopf = """// ---------------------------------------------------------------------------
// LogoDummy.h - Ersatzlogo, 76 x 76 Pixel als RGB565
//
// Kommt zum Zug, wenn ein Sender kein Logo hat, das Logo nicht zu holen ist
// oder der Speicher fuer den PNG-Decoder nicht reicht. Absichtlich **roh**
// und nicht als PNG: der Ausweg darf nicht das brauchen, was gerade
// gescheitert ist.
//
// Erzeugt von tools/make_logo_dummy.py - nicht von Hand aendern.
// ---------------------------------------------------------------------------
#pragma once

#define LOGO_DUMMY_W 76
#define LOGO_DUMMY_H 76

static const uint16_t logoDummy[LOGO_DUMMY_W * LOGO_DUMMY_H] PROGMEM = {
"""
open(BASIS + '/LogoDummy.h', 'w').write(kopf + '\n'.join(zeilen) + '\n};\n')
print('logo_dummy.565: %d Bytes' % len(roh))
print('LogoDummy.h:    %d Bytes Quelltext' % os.path.getsize(BASIS + '/LogoDummy.h'))
