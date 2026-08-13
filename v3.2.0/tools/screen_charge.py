#!/usr/bin/env python3
"""Entwurf des Lademodus als PNG malen, ohne Geraet.

ACHTUNG: Dieser Schirm gibt es im Sketch noch nicht. Das hier ist ein
Vorschlag zum Ansehen und Nachmessen. Die Koordinaten stehen deshalb
ausnahmsweise in dieser Datei und nicht als #define im Sketch - beim Einbau
wandern sie dorthin und werden hier wie ueberall sonst ueber defines.py
geholt.

Was der Schirm zeigt, ist auf das beschraenkt, was der Cardputer Adv wirklich
messen kann: die Batteriespannung ueber den ADC. Ladestrom und Leistung gibt
die Hardware nicht her (TP4057 ohne Messwiderstand und ohne I2C), deshalb
stehen sie hier auch nicht als leere Felder herum.

    python3 tools/screen_charge.py
    python3 tools/screen_charge.py --mv 4180 --trend 3 --elapsed 96
    python3 tools/screen_charge.py --state full
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import defines
from gfx import Display, TFT, c565, rgb

D = defines.defs()

BLACK = c565(TFT['TFT_BLACK'])
WHITE = c565(TFT['TFT_WHITE'])
RED = c565(TFT['TFT_RED'])
GREEN = c565(TFT['TFT_GREEN'])
DARKGREY = c565(TFT['TFT_DARKGREY'])

UI_FONT = 'NokiaFC'
UI_FONT_SMALL = 'NokiaFCSmall'
FOOTER_FONT = 'Font0'

# --- Vorschlag fuer die Layoutkonstanten ------------------------------------
# Kopfzeile und rote Linie bleiben, wo sie auf dem Radioschirm auch sind:
# HDR_BATT_H 13, HDR_RULE_Y 50. Dazwischen steht statt Sendername und
# Streamtitel die Spannung.
CHG_TITLE_X   = 4      # "CHARGE" in der Kopfzeile, links neben der Batterie
CHG_VOLT_Y    = 17     # Grosse Spannungsanzeige, wie HDR_NAME_Y
CHG_STATE_Y   = 20     # Ladezustand rechts daneben, Font0
CHG_PCT_Y     = 32     # Prozent darunter
CHG_GRAPH_X   = 4
CHG_GRAPH_Y   = 56
CHG_GRAPH_W   = 232
CHG_GRAPH_H   = 42
CHG_INFO_Y    = 102    # zwei Zeilen Font0 darunter
CHG_INFO_Y2   = 113
CHG_FOOT_Y    = 125    # Tastenhinweis
CHG_TEXT_COL  = rgb(150, 170, 190)
CHG_CURVE_COL = GREEN
CHG_GRID_COL  = rgb(45, 60, 75)

# Fenster der Kurve: so viele Minuten passen in die Grafik.
CHG_GRAPH_MIN = 30


def draw_battery(d, level):
    """Dasselbe Symbol wie auf dem Radioschirm, aus updateBatteryDisplay()."""
    col_frame, col_fill, batt_total, batt_x, fill_max = defines.need(
        defines.func_consts('updateBatteryDisplay'),
        'colFrame', 'colFill', 'battTotal', 'battX', 'fillMax')

    d.fill_rect(batt_x, 0, batt_total, D.HDR_BATT_H, BLACK)

    d.fill_rect(batt_x + D.BATT_R, D.BATT_Y,
                D.BATT_W - 2 * D.BATT_R, 1, col_frame)
    d.fill_rect(batt_x + D.BATT_R, D.BATT_Y + D.BATT_H - 1,
                D.BATT_W - 2 * D.BATT_R, 1, col_frame)
    d.fill_rect(batt_x, D.BATT_Y + D.BATT_R,
                1, D.BATT_H - 2 * D.BATT_R, col_frame)
    d.fill_rect(batt_x + D.BATT_W - 1, D.BATT_Y + D.BATT_R,
                1, D.BATT_H - 2 * D.BATT_R, col_frame)

    if D.BATT_R > 1:
        cx = batt_x + D.BATT_W - 2
        cy = D.BATT_Y + D.BATT_H - 2
        for px, py in ((batt_x + 1, D.BATT_Y + 1), (cx, D.BATT_Y + 1),
                       (batt_x + 1, cy), (cx, cy)):
            d.fill_rect(px, py, 1, 1, col_frame)

    d.fill_rect(batt_x + D.BATT_W + D.BATT_GAP,
                D.BATT_Y + (D.BATT_H - D.BATT_TIP_H) // 2,
                D.BATT_TIP_W, D.BATT_TIP_H, col_frame)

    w = (fill_max * max(0, min(100, level))) // 100
    if w > 0:
        d.fill_rect(batt_x + 2, D.BATT_Y + 2, w, D.BATT_H - 4, col_fill)


def draw_header(d, level):
    d.fill_rect(0, 0, 240, D.HDR_BATT_H, BLACK)
    d.set_font(FOOTER_FONT)
    d.set_text_color(CHG_TEXT_COL, BLACK)
    d.draw_string('CHARGE', CHG_TITLE_X, 3)
    draw_battery(d, level)
    d.fill_rect(0, D.HDR_RULE_Y, 240, 1, RED)


def draw_values(d, mv, level, state):
    d.set_font(UI_FONT)
    d.set_text_color(WHITE, BLACK)
    d.draw_string('%d.%02d V' % (mv // 1000, (mv % 1000) // 10),
                  CHG_TITLE_X, CHG_VOLT_Y)

    # Rechts uebereinander: Zustand und Prozent. Beide in Font0, damit die
    # grosse Spannung die einzige laute Zahl bleibt.
    d.set_font(FOOTER_FONT)
    col = GREEN if state == 'charging' else (
        CHG_TEXT_COL if state == 'full' else rgb(255, 190, 0))
    d.set_text_color(col, BLACK)
    label = {'charging': 'charging',
             'full': 'full',
             'idle': 'on battery'}[state]
    d.draw_right_string(label, 236, CHG_STATE_Y)

    d.set_text_color(CHG_TEXT_COL, BLACK)
    d.draw_right_string('%d %%' % level, 236, CHG_PCT_Y)


def draw_graph(d, samples, lo, hi):
    d.draw_rect(CHG_GRAPH_X, CHG_GRAPH_Y, CHG_GRAPH_W, CHG_GRAPH_H,
                CHG_GRID_COL)

    # Waagerechte Hilfslinie in der Mitte, gepunktet wie das Lautstaerkeraster.
    ym = CHG_GRAPH_Y + CHG_GRAPH_H // 2
    for x in range(CHG_GRAPH_X + 2, CHG_GRAPH_X + CHG_GRAPH_W - 2, 4):
        d.draw_pixel(x, ym, CHG_GRID_COL)

    if len(samples) < 2:
        return

    inner_w = CHG_GRAPH_W - 4
    inner_h = CHG_GRAPH_H - 4
    span = max(1, hi - lo)

    prev = None
    for i, mv in enumerate(samples):
        x = CHG_GRAPH_X + 2 + (i * (inner_w - 1)) // (len(samples) - 1)
        y = (CHG_GRAPH_Y + 2 + inner_h - 1
             - ((mv - lo) * (inner_h - 1)) // span)
        if prev is not None:
            px, py = prev
            steps = max(abs(x - px), abs(y - py), 1)
            for s in range(steps + 1):
                d.draw_pixel(px + (x - px) * s // steps,
                             py + (y - py) * s // steps, CHG_CURVE_COL)
        prev = (x, y)


def hhmm(minutes):
    return '%d:%02d' % (minutes // 60, minutes % 60)


def draw_info(d, trend, elapsed, remain, state, lo, hi):
    d.set_font(FOOTER_FONT)
    d.set_text_color(CHG_TEXT_COL, BLACK)

    d.draw_string('%+d mV/min' % trend, CHG_TITLE_X, CHG_INFO_Y)
    d.draw_right_string('running %s' % hhmm(elapsed), 236, CHG_INFO_Y)

    if state == 'charging' and remain is not None:
        d.draw_string('full in %s est.' % hhmm(remain),
                      CHG_TITLE_X, CHG_INFO_Y2)
    elif state == 'full':
        d.draw_string('battery full', CHG_TITLE_X, CHG_INFO_Y2)
    else:
        d.draw_string('no charger', CHG_TITLE_X, CHG_INFO_Y2)

    # Achsenbeschriftung der Kurve, hier statt im Bild - dort verdeckt sie
    # den Verlauf.
    d.set_text_color(CHG_GRID_COL, BLACK)
    d.draw_right_string('%d-%d mV/%dmin' % (lo, hi, CHG_GRAPH_MIN),
                        236, CHG_INFO_Y2)


def draw_footer(d):
    d.set_font(FOOTER_FONT)
    d.set_text_color(rgb(90, 105, 120), BLACK)
    d.draw_string('BtnG0/ESC exit', CHG_TITLE_X, CHG_FOOT_Y)
    d.draw_right_string('<>  vol  M  B', 236, CHG_FOOT_Y)


def make_samples(mv, trend, n=48):
    """Kurve rueckwaerts aus dem jetzigen Wert und dem Anstieg."""
    step = trend * CHG_GRAPH_MIN / float(n)
    out = []
    for i in range(n):
        v = mv - step * (n - 1 - i)
        # etwas Zappeln, wie es der ADC liefert
        v += (7 if i % 3 == 0 else -5 if i % 3 == 1 else 1)
        out.append(int(v))
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('-o', '--out', default='tools/screen_charge.png')
    p.add_argument('--scale', type=int, default=3)
    p.add_argument('--mv', type=int, default=4062, help='Batteriespannung in mV')
    p.add_argument('--level', type=int, default=78, help='Prozent')
    p.add_argument('--trend', type=int, default=12, help='mV je Minute')
    p.add_argument('--elapsed', type=int, default=23, help='Minuten im Lademodus')
    p.add_argument('--remain', type=int, default=70, help='Minuten bis voll')
    p.add_argument('--state', default='charging',
                   choices=['charging', 'full', 'idle'])
    a = p.parse_args()

    d = Display()
    d.fill_screen(BLACK)

    samples = make_samples(a.mv, a.trend)
    lo = (min(samples) // 20) * 20
    hi = ((max(samples) + 19) // 20) * 20

    draw_header(d, a.level)
    draw_values(d, a.mv, a.level, a.state)
    draw_graph(d, samples, lo, hi)
    draw_info(d, a.trend, a.elapsed,
              a.remain if a.state == 'charging' else None, a.state, lo, hi)
    draw_footer(d)

    d.save_png(a.out, scale=a.scale)
    print(a.out)


if __name__ == '__main__':
    main()
