#!/usr/bin/env python3
"""Der 5-Band-Equalizer im Stil des Spektrumanalyzers.

Alle Masze kommen aus den EQ_*-Zeilen des Sketches, die Frequenzen aus der
Tabelle eqFreqLabel[]. Wer dort etwas verschiebt, sieht es im naechsten Bild.

Die Schalter --style und --knob stammen aus dem Entwurf und zeigen Varianten,
die es aufs Geraet nicht geschafft haben. Gebaut ist: balken, --knob 1.

Beispiele:

    python3 tools/screen_eq.py --style knopf
    python3 tools/screen_eq.py --gains 4,2,0,-3,-6 --sel 3 --style balken
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

EQ_FREQS = defines.expect(defines.array_strings('eqFreqLabel'), EQ_BANDS,
                          'Frequenzen in eqFreqLabel[]')

# Die Farben stehen als benannte Konstanten in eqDrawColumn() und eqDrawGrid()
# - von dort geholt, nicht hier gepflegt. Wer am Geraet nachjustiert, sieht es
# im naechsten Bild.
_col = defines.need(defines.func_consts('eqDrawColumn'),
                    'dim', 'lit', 'mark', 'half', 'sel')
COL_DIM, COL_LIT, COL_MARK, COL_HALF, COL_SEL = (rgb(*c) for c in _col)
COL_DOT,  = defines.need(defines.func_consts('eqDrawGrid'), 'dot')
COL_DOT   = rgb(*COL_DOT)
COL_LABEL = c565(TFT['TFT_DARKGREY'])

FONT_SMALL = 'Font0'


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


def draw_scale(d):
    d.set_font(FONT_SMALL)
    d.set_text_color(COL_LABEL, BLACK)
    dy = d.font_height() // 2
    # Ohne "dB" und mittig in den Streifen links neben der Punktreihe gesetzt,
    # statt rechtsbuendig an sie herangeschoben.
    x = (EQ_X0 - EQ_COL_GAP // 2) // 2
    d.draw_centre_string('+6', x, row_y(EQ_ROWS - 1) - dy)
    d.draw_centre_string('0',  x, row_y(EQ_ROWS // 2) - dy)
    d.draw_centre_string('-6', x, row_y(0) - dy)


def draw_band(d, band, db, selected, style, knob_h=3):
    x = col_x(band)
    mid = EQ_ROWS // 2
    knob = gain_to_row(db)
    knob_col = COL_SEL if selected else COL_LIT

    for r in range(EQ_ROWS):
        col = COL_DIM
        if r == mid:
            col = COL_MARK          # der rote Mittelmarker
        if style == 'balken' and r != mid and min(mid, knob) <= r <= max(mid, knob):
            col = COL_HALF
        d.fill_rect(x, row_y(r), EQ_COL_W, 1, col)

    # Der Regler zuletzt — er verdeckt den roten Marker, wenn er darauf steht.
    h = 1 if style == 'linie' else knob_h
    y = row_y(knob) - h // 2
    d.fill_rect(x, y, EQ_COL_W, h, knob_col)


def draw_freqs(d, sel):
    d.set_font(FONT_SMALL)
    for b in range(EQ_BANDS):
        d.set_text_color(WHITE if b == sel else COL_LABEL, BLACK)
        d.draw_centre_string(EQ_FREQS[b], col_x(b) + EQ_COL_W // 2, EQ_FREQ_Y)


def draw_eq(d, gains, sel, style, knob_h=3):
    draw_dots(d)
    draw_scale(d)
    for b in range(EQ_BANDS):
        draw_band(d, b, gains[b], b == sel, style, knob_h)
    draw_freqs(d, sel)
    d.set_font('NokiaFC')
    d.set_text_color(WHITE, BLACK)


def build(args):
    d = Display()
    d.set_font('NokiaFC')
    d.fill_screen(BLACK)

    sm.show_station(d, args.station)
    sm.show_volume(d, args.volume)
    sm.update_battery_display(d, args.battery)
    sm.draw_footer(d, args.wifi, args.kbit, False, False, args.codec)

    draw_eq(d, args.gains, args.sel, args.style, args.knob)

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
                   help='gewaehltes Band, 0..4')
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
