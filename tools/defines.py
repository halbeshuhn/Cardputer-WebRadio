#!/usr/bin/env python3
"""Werte aus dem Sketch lesen: #define-Zeilen, feste Beschriftungen, Tabellen.

Damit holt sich der Renderer alles aus derselben Quelle, aus der auch der
Build es nimmt. Wer HDR_NAME_Y verschiebt oder "db" in "dB" aendert, sieht es
im naechsten Bild, ohne dass hier etwas nachgetragen werden muss.

Von den #define-Zeilen wird nur ausgewertet, was sich als Zahl oder
Zeichenkette ergibt: ganze Zahlen, Fliesskomma, einfache Rechnungen aus schon
bekannten Namen. Alles andere - Makros mit Klammern, Verweise auf Schriften,
color888(...) - wird uebergangen; solche Werte stehen im Szenenskript.

Fuer feste Beschriftungen gibt es drei Funktionen, damit kein Text zweimal
gepflegt werden muss:

    func_strings('drawVuFrame')   die Texte der drawString-Aufrufe darin
    array_strings('marksTxt')     die Texte einer const char*-Tabelle
    array_ints('marksDb')         die Zahlen einer int-Tabelle

Sie melden sich mit einer Ausnahme, wenn sie nichts finden oder wenn die
erwartete Anzahl nicht stimmt (expect()). Lieber ein lautes Scheitern als ein
Bild, das stillschweigend den alten Text zeigt.
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
# Den Sketch sucht gfx.py, das kennt beide Anordnungen. Mit WEBRADIO_SKETCH
# laesst sich eine andere Datei einsetzen - etwa eine Sicherung aus dem
# backups-Ordner, um einen alten Stand nachzuzeichnen.
from gfx import SKETCH as _SKETCH_DEFAULT   # noqa: E402

SKETCH = os.environ.get('WEBRADIO_SKETCH', _SKETCH_DEFAULT)

_DEFINE = re.compile(r'^\s*#define\s+([A-Za-z_]\w*)\s+(.+?)\s*(?://.*)?$')


def load(path=SKETCH):
    """Liefert ein dict Name -> Wert (int, float oder str)."""
    out = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = _DEFINE.match(line)
            if not m:
                continue
            name, body = m.group(1), m.group(2).strip()

            if body.startswith('"') and body.endswith('"'):
                out[name] = body[1:-1]
                continue

            # Zahlen, Rechnungen und Farben. Alles andere - Verweise auf
            # Schriften etwa - liefert None und wird uebergangen.
            expr = body.rstrip('f') if re.fullmatch(r'-?[\d.]+f', body) else body
            val = _value(expr, out)
            if val is not None:
                out[name] = val
    return out


def _text(path=SKETCH):
    with open(path, encoding='utf-8', errors='replace') as f:
        return f.read()


def func_body(name, path=SKETCH):
    """Rumpf einer Funktion, ueber Klammernzaehlen abgegrenzt."""
    text = _text(path)
    m = re.search(r'\b' + re.escape(name) + r'\s*\([^)]*\)\s*\{', text)
    if not m:
        raise LookupError('Funktion %s() nicht im Sketch gefunden' % name)

    depth = 0
    start = m.end() - 1
    for i in range(start, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return text[start + 1:i]
    raise LookupError('Ende von %s() nicht gefunden' % name)


_DRAWSTR = re.compile(r'draw(?:Centre|Right)?String\s*\(\s*"((?:[^"\\]|\\.)*)"')


def func_strings(name, path=SKETCH):
    """Die festen Texte, die eine Funktion zeichnet, in der Reihenfolge des
    Quelltextes. Texte aus Variablen oder Tabellen kommen hier nicht vor."""
    return _DRAWSTR.findall(func_body(name, path))


def _array_body(name, path=SKETCH):
    m = re.search(r'\b' + re.escape(name) + r'\s*\[\s*\]\s*=\s*\{([^}]*)\}',
                  _text(path))
    if not m:
        raise LookupError('Tabelle %s[] nicht im Sketch gefunden' % name)
    return m.group(1)


def array_strings(name, path=SKETCH):
    return re.findall(r'"((?:[^"\\]|\\.)*)"', _array_body(name, path))


def array_ints(name, path=SKETCH):
    return [int(v) for v in re.findall(r'-?\d+', _array_body(name, path))]


_CONST = re.compile(r'\bconst\s+\w+\s+(\w+)\s*=\s*([^;]+);')


def _value(expr, known):
    """Einen C-Ausdruck auswerten, soweit er hier auswertbar ist.

    Erkannt werden Farben - color565(r,g,b), color888(r,g,b) und die
    TFT_-Namen - sowie Zahlen und Rechnungen aus schon bekannten Namen.
    Alles andere liefert None und wird vom Aufrufer uebergangen.
    """
    from gfx import TFT, rgb, color888, c565      # erst hier, haelt den Kopf frei

    expr = ' '.join(expr.split())

    m = re.search(r'color565\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)', expr)
    if m:
        return rgb(*(int(v) for v in m.groups()))

    m = re.search(r'color888\s*\(\s*(0x[0-9A-Fa-f]+|\d+)\s*,'
                  r'\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*\)', expr)
    if m:
        return color888(*(int(v, 0) for v in m.groups()))

    m = re.fullmatch(r'(TFT_\w+)', expr)
    if m and m.group(1) in TFT:
        return c565(TFT[m.group(1)])

    if not re.fullmatch(r'[\w\s()+\-*/.]+', expr):
        return None
    try:
        val = eval(expr, {'__builtins__': {}}, dict(known))  # noqa: S307
    except Exception:
        return None
    # Farben stecken als (r, g, b) im dict, deshalb tuple mit erlauben.
    return val if isinstance(val, (int, float, tuple)) else None


def func_consts(name, path=SKETCH):
    """Die benannten Konstanten eines Funktionsrumpfes als dict.

    Aus `const int battW = 20;` wird {'battW': 20}, aus
    `const uint16_t colOn = ...color565(120, 165, 210);` die fertige Farbe.
    Ueber den Namen angesprochen, nicht ueber die Reihenfolge - eine
    zusaetzliche Zeile im Sketch bringt das also nicht durcheinander.
    """
    known = load(path)
    out = {}
    for var, expr in _CONST.findall(func_body(name, path)):
        val = _value(expr, {**known, **out})
        if val is not None:
            out[var] = val
    return out


def need(d, *names):
    """Konstanten holen und laut scheitern, wenn eine fehlt - etwa weil sie
    im Sketch umbenannt wurde."""
    missing = [n for n in names if n not in d]
    if missing:
        raise KeyError('im Sketch nicht gefunden: %s' % ', '.join(missing))
    return [d[n] for n in names]


def expect(values, count, what):
    """Anzahl pruefen. Aendert sich der Sketch so, dass die Zuordnung im
    Szenenskript nicht mehr passt, faellt es hier auf statt im Bild."""
    if len(values) != count:
        raise ValueError('%s: %d Eintraege erwartet, %d gefunden: %r'
                         % (what, count, len(values), values))
    return values


class Defs(dict):
    """dict mit Punktzugriff, damit D.HDR_NAME_Y lesbarer bleibt."""

    def __getattr__(self, name):
        try:
            return self[name]
        except KeyError:
            raise AttributeError('#define %s steht nicht im Sketch' % name)


def defs(path=SKETCH):
    return Defs(load(path))


if __name__ == '__main__':
    d = load()
    for k in sorted(d):
        print('%-20s %s' % (k, d[k]))
