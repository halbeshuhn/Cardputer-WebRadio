#!/usr/bin/env python3
"""Der 5-Band-Equalizer im Stil des Spektrumanalyzers.

Alle Masze kommen aus den EQ_*-Zeilen des Sketches, die Frequenzen aus der
Tabelle eqFreqLabel[]. Wer dort etwas verschiebt, sieht es im naechsten Bild.

Stand 20.8.2026: schmaleres Raster (28/10/50), keine dB-Skala mehr links,
Beschriftung ohne Einheit - und links die Vordaempfung als eigene halbbreite
Saeule mit Punktlinien als Rand. Ihr Regler laeuft im halben Reihenraster,
sie darf also zwischen den Linien stehen.

Das gewaehlte Band atmet am Geraet. Ein Standbild kann das nicht, deshalb
--atem: 255 ist der helle Umkehrpunkt (Vorgabe), 0 der dunkle.

Die Schalter --style und --knob stammen aus dem Entwurf und zeigen Varianten,
die es aufs Geraet nicht geschafft haben. Gebaut ist: balken, --knob 1.

Beispiele:

    python3 tools/screen_eq.py --gains 4,2,0,-3,-6 --sel 3
    python3 tools/screen_eq.py --sel -1 --pre 6      # Vordaempfung gewaehlt
    python3 tools/screen_eq.py --sel 0 --atem 0      # dunkler Umkehrpunkt
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import defines
import screen_main as sm
from gfx import Display, TFT, c565, rgb

D = defines.defs()

BLACK = c565(TFT['TFT_BLACK'])
WHITE = c565(TFT['TFT_WHITE'])

# --- alles aus dem Sketch ------------------------------------------------
EQ_BANDS      = int(D.EQ_BANDS)
EQ_ROWS       = int(D.EQ_ROWS)
EQ_ROW_PITCH  = int(D.EQ_ROW_PITCH)
EQ_BOTTOM_Y   = int(D.EQ_BOTTOM_Y)
EQ_COL_W      = int(D.EQ_COL_W)
EQ_COL_GAP    = int(D.EQ_COL_GAP)
EQ_X0         = int(D.EQ_X0)
EQ_FREQ_Y     = int(D.EQ_FREQ_Y)

EQ_PRE_DOT_L = int(D.EQ_PRE_DOT_L)
EQ_PRE_W     = int(D.EQ_PRE_W)
EQ_PRE_X     = int(D.EQ_PRE_X)
EQ_PRE_DOT_R = int(D.EQ_PRE_DOT_R)

# Die Grenzen der Vordaempfung stehen in AudioCompat.h, nicht im Sketch.
_AC = defines.load(os.path.join(os.path.dirname(defines.SKETCH), 'AudioCompat.h'))
PRE_MIN = int(_AC['AC_EQ_PRE_MIN_DB'])
PRE_MAX = int(_AC['AC_EQ_PRE_MAX_DB'])

EQ_FREQS = defines.expect(defines.array_strings('eqFreqLabel'), EQ_BANDS,
                          'Frequenzen in eqFreqLabel[]')

# Die Farben werden geholt, nicht gepflegt. Raster und Marker stehen als
# benannte Konstanten in eqDrawColumn()/eqDrawGrid(), die Endpunkte des Atems
# als EQ_C_*-Tabellen daneben.
_col = defines.need(defines.func_consts('eqDrawColumn'), 'dim', 'mark')
COL_DIM, COL_MARK = (rgb(*c) for c in _col)
COL_DOT,  = defines.need(defines.func_consts('eqDrawGrid'), 'dot')
COL_DOT   = rgb(*COL_DOT)

C_HALF = defines.array_ints('EQ_C_HALF')
C_LIT  = defines.array_ints('EQ_C_LIT')
C_SEL  = defines.array_ints('EQ_C_SEL')
C_LBL  = defines.array_ints('EQ_C_LBL')
C_WHT  = defines.array_ints('EQ_C_WHT')

COL_HALF  = rgb(*C_HALF)
COL_LIT   = rgb(*C_LIT)
COL_SEL   = rgb(*C_SEL)
COL_LABEL = rgb(*C_LBL)

FONT_SMALL = 'Font0'


def mix(a, b, t):
    """uiMixColor() aus dem Sketch: t von 0..255 zwischen zwei Farben."""
    return rgb(*[a[i] + (b[i] - a[i]) * t // 255 for i in range(3)])


def row_y(row):
    return EQ_BOTTOM_Y - row * EQ_ROW_PITCH


def col_x(band):
    return EQ_X0 + band * (EQ_COL_W + EQ_COL_GAP)


def grid_right():
    return col_x(EQ_BANDS - 1) + EQ_COL_W


def gain_to_row(db):
    """-6..+6 dB auf Reihe 0..12. Mitte (Reihe 6) ist 0 dB."""
    return max(0, min(EQ_ROWS - 1, int(round(db)) + EQ_ROWS // 2))


def draw_dots(d):
    """Punktreihen in den Zwischenraeumen und an beiden Aussenraendern.

    Sie tragen die dB-Skala durch das ganze Bild: jede Reihe ein Dezibel.
    """
    xs = [EQ_X0 - EQ_COL_GAP // 2, grid_right() + EQ_COL_GAP // 2]
    for b in range(EQ_BANDS - 1):
        xs.append(col_x(b) + EQ_COL_W + EQ_COL_GAP // 2)

    for r in range(EQ_ROWS):
        y = row_y(r)
        for x in xs:
            d.draw_pixel(x, y, COL_DOT)


def pre_y(db):
    """eqPreY() aus dem Sketch: halbes Reihenraster, 25 Lagen statt 13."""
    halb = EQ_ROW_PITCH // 2
    spanne = (EQ_ROWS - 1) * EQ_ROW_PITCH
    dd = (PRE_MAX - db) * spanne // (PRE_MAX - PRE_MIN)
    return EQ_BOTTOM_Y - ((dd + halb // 2) // halb) * halb


def draw_pre(d, db, selected, atem):
    """Die Vordaempfung: halbe Breite, Punktlinien als Rand, Regler zwischen
    den Reihen erlaubt. Aufbau sonst wie ein Band."""
    t = atem if selected else 0
    half_col = mix(C_HALF, C_LIT, t)
    knob_col = mix(C_LIT, C_SEL, t)

    mark_y, knob_y = pre_y(0), pre_y(db)
    lo, hi = min(mark_y, knob_y), max(mark_y, knob_y)

    for r in range(EQ_ROWS):
        y = row_y(r)
        d.fill_rect(EQ_PRE_X, y, EQ_PRE_W, 1,
                    half_col if lo <= y <= hi else COL_DIM)
        d.draw_pixel(EQ_PRE_DOT_L, y, COL_DOT)
        d.draw_pixel(EQ_PRE_DOT_R, y, COL_DOT)

    d.fill_rect(EQ_PRE_X, mark_y, EQ_PRE_W, 1, COL_MARK)
    d.fill_rect(EQ_PRE_X, knob_y, EQ_PRE_W, 1, knob_col)


def draw_band(d, band, db, selected, style, knob_h=3, atem=255):
    x = col_x(band)
    mid = EQ_ROWS // 2
    knob = gain_to_row(db)

    # Das gewaehlte Band atmet: Bereich und Regler wandern gemeinsam zur
    # vollen Helligkeit und zurueck, nie nach unten.
    t = atem if selected else 0
    half_col = mix(C_HALF, C_LIT, t)
    knob_col = mix(C_LIT, C_SEL, t)

    for r in range(EQ_ROWS):
        col = COL_DIM
        if r == mid:
            col = COL_MARK          # der rote Mittelmarker
        if style == 'balken' and r != mid and min(mid, knob) <= r <= max(mid, knob):
            col = half_col
        d.fill_rect(x, row_y(r), EQ_COL_W, 1, col)

    # Der Regler zuletzt — er verdeckt den roten Marker, wenn er darauf steht.
    h = 1 if style == 'linie' else knob_h
    y = row_y(knob) - h // 2
    d.fill_rect(x, y, EQ_COL_W, h, knob_col)


def draw_freqs(d, sel, atem=255):
    d.set_font(FONT_SMALL)
    for b in range(EQ_BANDS):
        d.set_text_color(mix(C_LBL, C_WHT, atem if b == sel else 0), BLACK)
        d.draw_centre_string(EQ_FREQS[b], col_x(b) + EQ_COL_W // 2, EQ_FREQ_Y)


def draw_eq(d, gains, sel, style, knob_h=3, pre=6, atem=255):
    draw_dots(d)
    draw_pre(d, pre, sel == -1, atem)
    for b in range(EQ_BANDS):
        draw_band(d, b, gains[b], b == sel, style, knob_h, atem)
    draw_freqs(d, sel, atem)
    d.set_font('NokiaFC')
    d.set_text_color(WHITE, BLACK)


def build(args):
    d = Display()
    d.set_font('NokiaFC')
    d.fill_screen(BLACK)

    sm.show_station(d, args.station)
    sm.show_volume(d, args.volume)
    sm.update_battery_display(d, args.battery)
    # Im Equalizer steht links in der Fusszeile die Vordaempfung, und sie
    # atmet mit, wenn die Saeule gewaehlt ist (drawSleepOrName/eqDrawPreFooter).
    sm.draw_footer(d, args.wifi, args.kbit, False, False, args.codec,
                   left='Pre %d dB' % args.pre)
    if args.sel == -1:
        d.set_font(FONT_SMALL)
        d.set_text_color(mix([128, 128, 128], C_WHT, args.atem), BLACK)
        d.draw_string('Pre %d dB' % args.pre, int(D.UI_MARGIN),
                      sm.footer_text_y(d))
        d.set_font('NokiaFC')

    draw_eq(d, args.gains, args.sel, args.style, args.knob, args.pre, args.atem)

    sm.draw_rule(d)
    return d


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('-o', '--out', default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), 'screen_eq.png'))
    p.add_argument('--scale', type=int, default=3)
    p.add_argument('--station', default='SWR3')
    p.add_argument('--volume', type=int, default=160)
    p.add_argument('--battery', type=int, default=76)
    p.add_argument('--wifi', type=int, default=72)
    p.add_argument('--kbit', type=int, default=128)
    p.add_argument('--codec', default='MP3')
    p.add_argument('--gains', default='4,2,0,-3,-6', help='dB je Band, -6..+6')
    p.add_argument('--sel', type=int, default=2,
                   help='gewaehltes Band, 0..4; -1 ist die Vordaempfung')
    p.add_argument('--pre', type=int, default=6,
                   help='Vordaempfung in dB, %d..%d' % (PRE_MIN, PRE_MAX))
    p.add_argument('--atem', type=int, default=255,
                   help='Stufe des Atems, 0..255 (255 = heller Umkehrpunkt)')
    p.add_argument('--style', choices=('linie', 'knopf', 'balken'),
                   default='balken')
    p.add_argument('--knob', type=int, default=1,
                   help='Dicke des Reglers in Pixeln, 1 wie eine Reihe')
    args = p.parse_args()

    args.gains = [float(v) for v in args.gains.split(',')][:EQ_BANDS]

    d = build(args)
    d.save_png(args.out, args.scale)
    print(args.out)


if __name__ == '__main__':
    main()
