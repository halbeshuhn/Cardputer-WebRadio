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

def draw_search_box(d, text, treffer):
    """rbDrawSearchBox() aus dem Sketch - Rahmen, Text, Trefferzahl, Strich."""
    head, = defines.need(defines.func_consts('drawRbList'), 'head')
    dim = rgb(70, 78, 86)

    x, y = int(D.RB_BOX_X), int(D.RB_BOX_Y)
    w, h = int(D.RB_BOX_W), int(D.RB_BOX_H)
    ty = int(D.RB_BOX_TY)

    d.draw_rect(x, y, w, h, dim)
    d.set_font(sm.FOOTER_FONT)

    if text:
        d.set_text_color(WHITE, BLACK)
        d.draw_string(text, x + 5, ty)
        d.fill_rect(x + 5 + d.text_width(text), ty, 1, 8, WHITE)   # der Strich
    else:
        d.set_text_color(dim, BLACK)
        d.draw_string(defines.lang('EN')['STR_RB_SEARCH'], x + 5, ty)

    d.set_text_color(head, BLACK)
    d.draw_right_string(str(treffer), x + w - 5, ty)


def draw_rb_list(d, title, labels, sel=0, top=0, foot=('', ''),
                 suche=None, gross=False, seite=None):
    """drawRbList() aus dem Sketch.

    gross=True zeichnet die Senderebene: sechs Zeilen zu 15 px. Auf dem Geraet
    steht dort efont, das der Renderer nicht lesen kann - hier vertritt sie
    die Noto-Schrift in derselben Groesse. Die Masze stimmen, die Formen der
    Buchstaben nicht ganz.
    """
    head, = defines.need(defines.func_consts('drawRbList'), 'head')

    d.fill_screen(BLACK)
    d.set_font(sm.FOOTER_FONT)

    d.set_text_color(head, BLACK)
    d.draw_string(title, D.UI_MARGIN, 1)

    if seite:
        d.draw_right_string(seite, 240 - D.UI_MARGIN, 1)

    zeilen = int(D.RB_VISIBLE_UNI) if gross else int(D.RB_VISIBLE)
    hoehe  = int(D.RB_LINE_H_UNI)  if gross else int(D.RB_LINE_H)

    for i in range(zeilen):
        idx = top + i
        if idx >= len(labels):
            break

        y = D.RB_TOP_Y + i * hoehe
        d.set_text_color(WHITE if idx == sel else rgb(120, 128, 128), BLACK)
        d.set_font(sm.FOOTER_FONT)
        d.draw_string('>' if idx == sel else ' ', D.UI_MARGIN, y)
        d.set_font(sm.TITLE_FONT if gross else sm.FOOTER_FONT)
        d.draw_string(labels[idx], D.UI_MARGIN + 8, y)

    d.set_font(sm.FOOTER_FONT)
    d.set_text_color(head, BLACK)

    if suche is not None:
        # Auf dem Geraet ersetzt die Leiste die Hilfszeile, sie steht nicht
        # darunter - siehe drawRbList() im Sketch.
        draw_search_box(d, suche, len(labels))
    elif gross:
        d.draw_string(foot[0], D.UI_MARGIN, 124)
    else:
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
    namen = defines.struct_field('rbCountries', 0)
    if args.suche:
        namen = [n for n in namen if args.suche.lower() in n.lower()]

    d = Display()
    draw_rb_list(d,
                 t['STR_RB_TITLE_C'],
                 namen,
                 sel=args.auswahl,
                 top=args.oben,
                 foot=(t['STR_RB_FOOT_NEXT'], t['STR_RB_FOOT_BACK']),
                 suche=args.suche)
    return d


# Beispielnamen fuer die Senderebene - sonst kommen sie vom Dienst.
SENDER = ['1LIVE', 'Radio Eins', 'NDR Info', 'WDR 5', 'Sunshine Live',
          'Rock Antenne', 'HR1', 'Bayern 3', 'Deutschlandfunk', 'MDR Aktuell',
          'Antenne Bayern']


def build_sender(args):
    t = defines.lang(args.sprache)
    namen = SENDER
    if args.suche:
        namen = [n for n in namen if args.suche.lower() in n.lower()]

    d = Display()
    draw_rb_list(d,
                 t['STR_RB_TITLE_S'] % ('Germany', 1),
                 namen,
                 sel=args.auswahl,
                 top=args.oben,
                 foot=(t['STR_RB_FOOT_PLAY'], ''),
                 suche=args.suche,
                 gross=True,
                 seite=args.seite)
    return d


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('ansicht', choices=('menu', 'online', 'sender'))
    p.add_argument('--suche', default=None,
                   help='Text im Suchfeld; ohne Angabe bleibt die Leiste aus')
    p.add_argument('--seite', default=None,
                   help='Seitenzahl oben rechts, etwa 1/12')
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
    args.lowram = False   # beides verlangt screen_main.build()
    args.codec  = 'MP3'
    args.vu = [96, 84]
    args.spec = None

    d = {'menu': build_menu, 'online': build_online,
         'sender': build_sender}[args.ansicht](args)
    d.save_png(args.out, args.scale)
    print(args.out)


if __name__ == '__main__':
    main()
