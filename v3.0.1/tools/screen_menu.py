#!/usr/bin/env python3
"""Systemmenue und Online-Liste zeichnen, wie screen_main.py den Radioschirm.

Zwei Ansichten:

  menu    das schwebende Fenster des Systemmenues (BtnG0), ueber dem
          laufenden Radioschirm - so, wie es auf dem Geraet aussieht
  online  die Vollbildliste der Laenderauswahl aus der Online-Suche

Beschriftungen kommen aus Lang.h, die Laendernamen aus der Tabelle
rbCountries[] im Sketch, Masse und Farben wie ueberall aus den #define-Zeilen
und den benannten Konstanten der zugehoerigen Zeichenfunktionen. Hier steht
nur die Reihenfolge der Aufrufe.

    python3 tools/screen_menu.py menu   -o menu.png
    python3 tools/screen_menu.py online -o online.png --sprache DE
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import defines
import screen_main as sm
from gfx import Display, TFT, c565, rgb

D = sm.D

BLACK = c565(TFT['TFT_BLACK'])
WHITE = c565(TFT['TFT_WHITE'])

MENU_FONT = 'Font0'


# ---------------------------------------------------------------------------
# Systemmenue
# ---------------------------------------------------------------------------

def menu_labels(lang='EN'):
    """Die Eintraege des Hauptmenues, in der Reihenfolge aus sysBuildMain().

    IT_SAVE faellt weg - der Eintrag erscheint nur, wenn gerade ein Sender aus
    der Online-Suche laeuft.
    """
    t = defines.lang(lang)
    return [t['STR_MENU_WIFI'], t['STR_MENU_ONLINE'], t['STR_MENU_LOCAL'],
            t['STR_MENU_LANG'], t['STR_MENU_EXIT']]


def draw_sys_menu(d, labels, index=0):
    """drawSysMenu() aus dem Sketch."""
    frame, dim = defines.need(defines.func_consts('drawSysMenu'),
                              'frame', 'dim')

    count = len(labels)
    h = 2 * D.SYS_MENU_PAD + count * D.SYS_MENU_LINE_H
    x = (240 - D.SYS_MENU_W) // 2
    y = (135 - h) // 2

    d.fill_rect(x - 2, y - 2, D.SYS_MENU_W + 4, h + 4, BLACK)
    d.draw_rect(x, y, D.SYS_MENU_W, h, frame)

    d.set_font(MENU_FONT)
    d.set_text_size(D.MENU_TEXT_SIZE)

    for i, label in enumerate(labels):
        ly = y + D.SYS_MENU_PAD + i * D.SYS_MENU_LINE_H
        ty = ly + (D.SYS_MENU_LINE_H - d.font_height()) // 2

        if i == index:
            d.set_text_color(WHITE, BLACK)
            d.draw_string('>', x + D.SYS_MENU_PAD, ty)
        else:
            d.set_text_color(dim, BLACK)

        d.draw_string(label, x + D.SYS_MENU_PAD + D.SYS_MENU_MARK_W, ty)

    d.set_text_size(1)
    d.set_font('NokiaFC')
    d.set_text_color(WHITE, BLACK)


# ---------------------------------------------------------------------------
# Online-Liste, Ebene Laenderauswahl
# ---------------------------------------------------------------------------

def draw_rb_list(d, title, labels, sel=0, top=0, foot=('', '')):
    """drawRbList() aus dem Sketch, Ebene RB_PAGE_COUNTRY."""
    head, = defines.need(defines.func_consts('drawRbList'), 'head')

    d.fill_screen(BLACK)
    d.set_font(sm.FOOTER_FONT)

    d.set_text_color(head, BLACK)
    d.draw_string(title, D.UI_MARGIN, 1)

    for i in range(int(D.RB_VISIBLE)):
        idx = top + i
        if idx >= len(labels):
            break

        y = D.RB_TOP_Y + i * D.RB_LINE_H
        d.set_text_color(WHITE if idx == sel else rgb(120, 128, 128), BLACK)
        d.draw_string('>' if idx == sel else ' ', D.UI_MARGIN, y)
        d.draw_string(labels[idx], D.UI_MARGIN + 8, y)

    d.set_text_color(head, BLACK)
    d.draw_string(foot[0], D.UI_MARGIN, 114)
    d.draw_string(foot[1], D.UI_MARGIN, 124)

    d.set_font('NokiaFC')
    d.set_text_color(WHITE, BLACK)


# ---------------------------------------------------------------------------

def build_menu(args):
    """Das Menue liegt ueber dem Radioschirm - der wird zuerst gezeichnet."""
    d = sm.build(args)
    draw_sys_menu(d, menu_labels(args.sprache), args.auswahl)
    return d


def build_online(args):
    t = defines.lang(args.sprache)
    d = Display()
    draw_rb_list(d,
                 t['STR_RB_TITLE_C'],
                 defines.struct_field('rbCountries', 0),
                 sel=args.auswahl,
                 top=args.oben,
                 foot=(t['STR_RB_FOOT_NEXT'], t['STR_RB_FOOT_BACK']))
    return d


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('ansicht', choices=('menu', 'online'))
    p.add_argument('-o', '--out', default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), 'screen_menu.png'))
    p.add_argument('--scale', type=int, default=3)
    p.add_argument('--sprache', default='EN', choices=('EN', 'DE'))
    p.add_argument('--auswahl', type=int, default=0, help='markierte Zeile')
    p.add_argument('--oben', type=int, default=0, help='erste sichtbare Zeile')

    # Der Radioschirm hinter dem Menue - dieselben Schalter wie screen_main.py
    p.add_argument('--station', default='SWR3')
    p.add_argument('--title', default='Fleetwood Mac - Dreams')
    p.add_argument('--volume', type=int, default=160)
    p.add_argument('--battery', type=int, default=76)
    p.add_argument('--wifi', type=int, default=72)
    p.add_argument('--kbit', type=int, default=128)
    p.add_argument('--vis', choices=('vu', 'vupeak', 'spectrum', 'off'), default='vu')
    args = p.parse_args()

    args.failed = False
    args.vu = [96, 84]
    args.spec = None

    d = build_menu(args) if args.ansicht == 'menu' else build_online(args)
    d.save_png(args.out, args.scale)
    print(args.out)


if __name__ == '__main__':
    main()
