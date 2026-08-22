#!/usr/bin/env python3
"""Ein gerendertes Bild ausmessen statt es nur anzusehen.

Rendert den Hauptbildschirm und zaehlt aus, welche Farbe welchen Bereich
belegt: x von..bis, y von..bis, Breite, Hoehe. Damit laesst sich pruefen, ob
zwei Bauteile wirklich buendig sitzen - das Auge taeuscht sich bei drei
Pixeln, die Zahlen nicht.

Farben, die aus dem Sketch stammen (UI_SCALE_ON, TFT_RED und Verwandte),
werden mit Namen ausgewiesen.

    python3 tools/measure.py                  ganzes Bild
    python3 tools/measure.py --band 0 16      nur die Zeilen 0 bis 15
    python3 tools/measure.py --band 0 16 --von 190   nur die Batterie
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import defines
import screen_main as sm
from gfx import TFT, c565


def color_names():
    """Farbe -> Name, aus den TFT_-Konstanten und den #define-Farben."""
    out = {}
    for name, word in TFT.items():
        out.setdefault(c565(word), name)
    for name, val in defines.load().items():
        if isinstance(val, tuple):
            out[val] = name
    return out


def regions(d, y0, y1, x0, x1):
    """Je Farbe der belegte Bereich. Schwarz bleibt aussen vor."""
    box = {}
    for y in range(y0, min(y1, d.height())):
        for x in range(x0, min(x1, d.width())):
            i = (y * d.width() + x) * 3
            col = tuple(d.buf[i:i + 3])
            if col == (0, 0, 0):
                continue
            b = box.get(col)
            if b is None:
                box[col] = [x, x, y, y, 1]
            else:
                b[0] = min(b[0], x); b[1] = max(b[1], x)
                b[2] = min(b[2], y); b[3] = max(b[3], y)
                b[4] += 1
    return box


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--band', nargs=2, type=int, metavar=('VON', 'BIS'),
                   default=[0, 135], help='Zeilenbereich')
    p.add_argument('--von', type=int, default=0, help='linke Spalte')
    p.add_argument('--bis', type=int, default=240, help='rechte Spalte')
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

    d = sm.build(args)
    names = color_names()

    print('%-16s %-18s %8s %8s %7s' % ('Farbe', 'RGB', 'x', 'y', 'Pixel'))
    print('-' * 62)
    rows = regions(d, args.band[0], args.band[1], args.von, args.bis)
    for col, (x0, x1, y0, y1, n) in sorted(rows.items(), key=lambda kv: -kv[1][4]):
        print('%-16s %-18s %8s %8s %7d'
              % (names.get(col, '')[:16], '%d,%d,%d' % col,
                 '%d..%d' % (x0, x1), '%d..%d' % (y0, y1), n))


if __name__ == '__main__':
    main()
