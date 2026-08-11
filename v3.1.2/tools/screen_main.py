#!/usr/bin/env python3
"""Den Hauptbildschirm des Radios als PNG malen, ohne Geraet.

Die Reihenfolge der Aufrufe ist die aus dem Sketch: erst redrawUI()
(showStation, showVolume, updateBatteryDisplay, drawFooter, drawVisFrame),
dann die rote Trennlinie, die drawStreamTitle() in loop() nachzieht.

Die Koordinaten stehen nicht hier, sondern werden bei jedem Lauf aus den
#define-Zeilen des Sketches geholt. Was hier steht, ist nur die Abfolge.

Beispiele:

    python3 tools/screen_main.py
    python3 tools/screen_main.py --station "Radio Paradise" --title "Pink Floyd - Time"
    python3 tools/screen_main.py --vis spectrum --volume 200
    python3 tools/screen_main.py --vis vupeak --vu 90,70 --scale 4
"""
import argparse
import math
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
YELLOW = c565(TFT['TFT_YELLOW'])
ORANGE = c565(TFT['TFT_ORANGE'])
DARKGREY = c565(TFT['TFT_DARKGREY'])
LIGHTGREY = c565(TFT['TFT_LIGHTGREY'])

FOOTER_TEXT_COLOR = D.FOOTER_TEXT_COLOR   # #define im Sketch, mit color888()

UI_FONT = 'NokiaFC'
UI_FONT_SMALL = 'NokiaFCSmall'
FOOTER_FONT = 'Font0'
TITLE_FONT = 'NotoSans'


# ---------------------------------------------------------------------------
# Kopfbereich
# ---------------------------------------------------------------------------

def show_station(d, station):
    d.fill_rect(0, D.HDR_NAME_Y, 240, D.HDR_NAME_H, BLACK)
    d.set_font(UI_FONT)
    d.set_text_color(WHITE, BLACK)
    d.draw_string(station, D.HDR_TEXT_X, D.HDR_NAME_Y)


def draw_title_block(d, title, line_h=None, lines=None):
    """drawTitleBlock(): Streamtitel unter der Linie, nur bei leerer Anzeige.

    Umbrochen wird durch Messen, Zeichen fuer Zeichen - dieselbe Schleife wie
    im Sketch. Noto Sans ist proportional, mit fester Zeichenbreite kaeme ein
    anderer Umbruch heraus als auf dem Geraet.
    """
    line_h = int(D.TITLE_LINE_H if line_h is None else line_h)
    lines = int(D.TITLE_LINES if lines is None else lines)

    d.fill_rect(0, D.TITLE_Y, 240,
                d.height() - D.FOOTER_HEIGHT - D.TITLE_Y, BLACK)
    if not title:
        return

    d.set_font(TITLE_FONT)
    d.set_text_color(FOOTER_TEXT_COLOR, BLACK)

    max_w = 240 - 2 * D.UI_MARGIN

    p = title
    for line in range(lines):
        if not p:
            break

        take = 0
        last_space = -1
        while take < len(p):
            if d.text_width(p[:take + 1]) > max_w:
                break
            if p[take] == ' ':
                last_space = take
            take += 1

        if take < len(p) and last_space > 0:
            take = last_space
        if take == 0:
            take = 1

        d.draw_string(p[:take], D.HDR_TEXT_X,
                      D.TITLE_Y + line * line_h)
        p = p[take:].lstrip(' ')

    d.set_font(UI_FONT)


def draw_rule(d):
    """Zieht drawStreamTitle() bei jedem Durchlauf nach."""
    d.fill_rect(0, D.HDR_RULE_Y, d.width(), 1, RED)


# ---------------------------------------------------------------------------
# Lautstaerkeskala und Batterie
# ---------------------------------------------------------------------------

def show_volume(d, volume):
    # Die beiden Toene stehen in showVolume() im Sketch.
    col_on, col_grid = defines.need(defines.func_consts('showVolume'),
                                    'colOn', 'colGrid')

    dots = int(D.VOL_DOTS)
    rows = int(D.VOL_ROWS)
    filled = (volume * dots + 127) // 255

    last = dots - 1
    quarter = last // 4
    half = last // 2
    three = (3 * last) // 4

    for i in range(dots):
        x = D.VOL_DOT_L + i * D.VOL_DOT_PITCH

        mark = 1
        if i == 0 or i == last:
            mark = rows
        elif i in (quarter, half, three):
            mark = 2

        for r in range(rows):
            y = D.VOL_BASE_Y - r * D.VOL_ROW_PITCH
            col = col_on if (i < filled or r < mark) else col_grid
            d.draw_pixel(x, y, col)


def update_battery_display(d, level):
    # Farben, Gesamtbreite, linke Kante und Fuellbreite stehen als benannte
    # Konstanten in updateBatteryDisplay() - von dort geholt, nicht geraten.
    col_frame, col_fill, batt_total, batt_x, fill_max = defines.need(
        defines.func_consts('updateBatteryDisplay'),
        'colFrame', 'colFill', 'battTotal', 'battX', 'fillMax')

    d.fill_rect(batt_x, 0, batt_total, D.HDR_BATT_H, BLACK)

    # Rahmen: oben, unten, links, rechts - die Ecken bleiben frei.
    d.fill_rect(batt_x + D.BATT_R, D.BATT_Y,
                D.BATT_W - 2 * D.BATT_R, 1, col_frame)
    d.fill_rect(batt_x + D.BATT_R, D.BATT_Y + D.BATT_H - 1,
                D.BATT_W - 2 * D.BATT_R, 1, col_frame)
    d.fill_rect(batt_x, D.BATT_Y + D.BATT_R,
                1, D.BATT_H - 2 * D.BATT_R, col_frame)
    d.fill_rect(batt_x + D.BATT_W - 1, D.BATT_Y + D.BATT_R,
                1, D.BATT_H - 2 * D.BATT_R, col_frame)

    # Ab BATT_R = 2 je ein Punkt diagonal in die Ecke, das rundet sie.
    if D.BATT_R > 1:
        cx = batt_x + D.BATT_W - 2
        cy = D.BATT_Y + D.BATT_H - 2
        for px, py in ((batt_x + 1, D.BATT_Y + 1), (cx, D.BATT_Y + 1),
                       (batt_x + 1, cy), (cx, cy)):
            d.fill_rect(px, py, 1, 1, col_frame)

    d.fill_rect(batt_x + D.BATT_W + D.BATT_GAP,
                D.BATT_Y + (D.BATT_H - D.BATT_TIP_H) // 2,
                D.BATT_TIP_W, D.BATT_TIP_H, col_frame)

    fill_w = (level * fill_max + 50) // 100
    if level > 0 and fill_w < 1:
        fill_w = 1
    fill_w = min(fill_w, fill_max)

    d.fill_rect(batt_x + D.BATT_PAD, D.BATT_Y + D.BATT_PAD,
                fill_w, D.BATT_H - 2 * D.BATT_PAD, col_fill)


# ---------------------------------------------------------------------------
# Footer
# ---------------------------------------------------------------------------

def footer_text_y(d):
    d.set_font(FOOTER_FONT)
    return d.height() - D.FOOTER_TEXT_DY - d.font_height()


WIFI_ICON = [0x1FC, 0x202, 0x401, 0x070, 0x088, 0x000, 0x020, 0x000]


def draw_wifi_icon(d, x0, y0, col):
    w = int(D.WIFI_ICON_W)
    for y, bits in enumerate(WIFI_ICON[:int(D.WIFI_ICON_H)]):
        for x in range(w):
            if bits & (1 << (w - 1 - x)):
                d.draw_pixel(x0 + x, y0 + y, col)


def draw_wifi(d, pct):
    y = d.height() - D.FOOTER_HEIGHT
    ty = footer_text_y(d)

    d.fill_rect(D.FOOTER_WIFI_X, y, D.FOOTER_RATE_X - D.FOOTER_WIFI_X,
                D.FOOTER_HEIGHT, BLACK)

    buf = '%d%%' % pct if pct > 0 else '--'

    block_w = D.WIFI_ICON_W + D.WIFI_GAP + d.text_width(buf)
    x0 = (D.FOOTER_WIFI_X + D.FOOTER_RATE_X - block_w) // 2

    col = (GREEN if pct >= D.WIFI_PCT_GREEN
           else YELLOW if pct >= D.WIFI_PCT_YELLOW else RED)

    draw_wifi_icon(d, x0, ty, col)
    d.set_text_color(FOOTER_TEXT_COLOR, BLACK)
    d.draw_string(buf, x0 + D.WIFI_ICON_W + D.WIFI_GAP, ty)


def draw_bitrate(d, kbit, codec='MP3'):
    y = d.height() - D.FOOTER_HEIGHT
    ty = footer_text_y(d)

    d.fill_rect(D.FOOTER_RATE_X, y, d.width() - D.FOOTER_RATE_X,
                D.FOOTER_HEIGHT, BLACK)

    if kbit > 0:
        d.set_text_color(FOOTER_TEXT_COLOR, BLACK)
        d.draw_right_string('%s %dkBit' % (codec, kbit), d.width() - D.UI_MARGIN, ty)


def draw_footer(d, pct, kbit, failed, lowram=False, codec='MP3'):
    y = d.height() - D.FOOTER_HEIGHT
    ty = footer_text_y(d)

    d.fill_rect(0, y, 240, D.FOOTER_HEIGHT, BLACK)

    if failed:
        # Zwei Meldungen, in der Reihenfolge des Quelltextes: erst der Fall
        # "zu wenig Speicher", dann der tote Sender. Beide stehen nur dort.
        lowmem, tot = defines.expect(defines.func_strings('drawFooter'), 2,
                                     'Meldungen in drawFooter()')
        d.set_text_color(ORANGE, BLACK)
        d.draw_centre_string(lowmem if lowram else tot, 120, ty)
    else:
        d.set_text_color(FOOTER_TEXT_COLOR, BLACK)
        d.draw_string(D.FOOTER_NAME, D.UI_MARGIN, ty)
        draw_wifi(d, pct)
        draw_bitrate(d, kbit, codec)

    d.set_font(UI_FONT)
    d.set_text_color(WHITE, BLACK)


# ---------------------------------------------------------------------------
# VU-Meter
# ---------------------------------------------------------------------------

def vu_band_rgb(seg):
    if seg <= D.VU_GREEN_MAX:
        return (0, 230, 60)
    if seg <= D.VU_YELLOW_MAX:
        return (255, 190, 0)
    return (255, 0, 0)


def vu_color(seg, state):
    r, g, b = vu_band_rgb(seg)
    div = int(D.VU_OFF_DIV)
    if state == 0:
        return rgb(r // div, g // div, b // div)
    if state == 2:
        return rgb(128 + r // 2, 128 + g // 2, 128 + b // 2)
    return rgb(r, g, b)


def vu_level_to_segs(level):
    if level == 0:
        return 0
    db = 20.0 * math.log10(level / 127.0)
    if db <= D.VU_DB_FLOOR:
        return 0
    db = min(db, 0.0)
    segs = int(((db - D.VU_DB_FLOOR) / -D.VU_DB_FLOOR) * D.VU_SEGMENTS + 0.5)
    return min(segs, int(D.VU_SEGMENTS))


def vu_draw_seg(d, ch, seg, state):
    y = D.VU_L_Y if ch == 0 else D.VU_R_Y
    x = D.VU_X0 + seg * (D.VU_SEG_W + D.VU_SEG_GAP)
    d.fill_rect(x, y, D.VU_SEG_W, D.VU_BAR_H, vu_color(seg, state))


def draw_vu_frame(d):
    bar_w = D.VU_SEGMENTS * (D.VU_SEG_W + D.VU_SEG_GAP) - D.VU_SEG_GAP

    # Die drei festen Beschriftungen kommen aus drawVuFrame() im Sketch, in
    # der Reihenfolge, in der sie dort gezeichnet werden.
    lbl_l, lbl_r, lbl_db = defines.expect(
        defines.func_strings('drawVuFrame'), 3, 'Beschriftungen in drawVuFrame()')

    d.set_font(UI_FONT_SMALL)
    label_dy = (D.VU_BAR_H - d.font_height()) // 2

    d.set_text_color(LIGHTGREY, BLACK)
    d.draw_string(lbl_l, D.UI_MARGIN, D.VU_L_Y + label_dy)
    d.draw_string(lbl_r, D.UI_MARGIN, D.VU_R_Y + label_dy)

    d.set_font('Font0')
    d.set_text_color(DARKGREY, BLACK)
    d.draw_string(lbl_db, D.UI_MARGIN, D.VU_SCALE_Y)

    marks = list(zip(defines.array_ints('marksDb'),
                     defines.array_strings('marksTxt')))
    for db, txt in marks:
        if db == 0:
            d.draw_right_string(txt, D.VU_X0 + bar_w, D.VU_SCALE_Y)
        else:
            x = D.VU_X0 + int(((db - D.VU_DB_FLOOR) / -D.VU_DB_FLOOR) * bar_w)
            d.draw_centre_string(txt, x, D.VU_SCALE_Y)

    d.set_font(UI_FONT)
    d.set_text_color(WHITE, BLACK)


def update_vu_meter(d, levels, with_peak):
    for ch in range(2):
        segs = vu_level_to_segs(levels[ch])
        for s in range(int(D.VU_SEGMENTS)):
            state = 1 if s < segs else 0
            if with_peak and segs > 0 and s == segs - 1:
                state = 2
            vu_draw_seg(d, ch, s, state)


# ---------------------------------------------------------------------------
# Spektrum
# ---------------------------------------------------------------------------

def spec_draw_row(d, band, row, lit):
    x = D.SPEC_X0 + band * D.SPEC_COL_PITCH
    y = D.SPEC_BOTTOM_Y - row * D.SPEC_ROW_PITCH

    if row == 0:
        d.fill_rect(x, y, D.SPEC_COL_W, 1, rgb(180, 75, 65))
        return

    col = rgb(120, 165, 210) if lit else rgb(15, 22, 30)
    d.fill_rect(x, y, D.SPEC_COL_W, 1, col)


def draw_spectrum_frame(d, levels=None):
    dot, = defines.need(defines.func_consts('drawSpectrumFrame'), 'dot')
    for r in range(int(D.SPEC_ROWS)):
        y = D.SPEC_BOTTOM_Y - r * D.SPEC_ROW_PITCH
        d.draw_pixel(D.SPEC_DOT_L, y, dot)
        d.draw_pixel(D.SPEC_DOT_R, y, dot)

    for b in range(int(D.SPEC_BANDS)):
        top = levels[b] if levels else 0
        for r in range(int(D.SPEC_ROWS)):
            spec_draw_row(d, b, r, r <= top)


# ---------------------------------------------------------------------------

def build(args):
    d = Display()
    d.set_font(UI_FONT)
    d.fill_screen(BLACK)          # setup(), vor redrawUI()

    show_station(d, args.station)
    show_volume(d, args.volume)
    update_battery_display(d, args.battery)
    draw_footer(d, args.wifi, args.kbit, args.failed, args.lowram, args.codec)

    if args.vis in ('vu', 'vupeak'):
        draw_vu_frame(d)
        update_vu_meter(d, args.vu, args.vis == 'vupeak')
    elif args.vis == 'spectrum':
        draw_spectrum_frame(d, args.spec)
    else:
        draw_title_block(d, args.title, args.titleline, args.titlelines)

    draw_rule(d)                  # aus drawStreamTitle() in loop()
    return d


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('-o', '--out', default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), 'screen_main.png'))
    p.add_argument('--scale', type=int, default=3, help='Vergroesserung (Vorgabe 3)')
    p.add_argument('--station', default='SWR3')
    p.add_argument('--title', default='Fleetwood Mac - Dreams')
    p.add_argument('--volume', type=int, default=160, help='0..255')
    p.add_argument('--battery', type=int, default=76, help='0..100')
    p.add_argument('--wifi', type=int, default=72, help='Prozent, 0 = keine Verbindung')
    p.add_argument('--kbit', type=int, default=128)
    p.add_argument('--failed', action='store_true', help='Stream tot: Meldung im Footer')
    p.add_argument('--codec', default='MP3', help="MP3, AAC, AAC+, FLAC ...")
    p.add_argument('--lowram', action='store_true', help='mit --failed: Meldung wegen Speichermangel')
    p.add_argument('--vis', choices=('vu', 'vupeak', 'spectrum', 'off'), default='vu')
    p.add_argument('--vu', default='96,84', help='Pegel beider Kanaele, 0..127')
    p.add_argument('--spec', default='', help='Saeulenhoehen, 10 Zahlen 0..12')
    p.add_argument('--titleline', type=int, default=None,
                   help='Zeilenhoehe des Streamtitels, sonst TITLE_LINE_H')
    p.add_argument('--titlelines', type=int, default=None,
                   help='Zeilenzahl des Streamtitels, sonst TITLE_LINES')
    args = p.parse_args()

    args.vu = [int(v) for v in args.vu.split(',')][:2]
    args.spec = [int(v) for v in args.spec.split(',')] if args.spec else None

    d = build(args)
    d.save_png(args.out, args.scale)
    print(args.out)


if __name__ == '__main__':
    main()
