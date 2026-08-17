"""K-Earth 101: das Nummernschild vor Sonnenuntergang mit Palmen.

Der weisse Grund wird von den Raendern her weggefuellt - so bleibt alles
Helle *im* Schild erhalten. Darunter kommt ein gemalter Himmel.
"""
import struct, sys, math
S = "/private/tmp/claude-501/-Users-ecki-Documents-Arduino/865dc204-b288-4c61-9af9-d7a96a7adea5/scratchpad"
N = 76

# ---- Vorlage laden -------------------------------------------------------
b = open(S + '/krth76.bmp', 'rb').read()
off = struct.unpack_from('<I', b, 10)[0]
w, h = struct.unpack_from('<ii', b, 18)
bits = struct.unpack_from('<H', b, 28)[0]
n, zl = bits // 8, ((w * (bits // 8) + 3) // 4) * 4
h = abs(h)

def quelle(x, y):
    i = off + y * zl + x * n
    return (b[i+3], b[i+2], b[i+1]) if bits == 32 else (b[i+2], b[i+1], b[i])

# ---- Weiss von aussen wegfluten -----------------------------------------
frei = [[False]*w for _ in range(h)]
stapel = [(x, 0) for x in range(w)] + [(x, h-1) for x in range(w)] \
       + [(0, y) for y in range(h)] + [(w-1, y) for y in range(h)]
def hell(p): return p[0] > 225 and p[1] > 225 and p[2] > 225
while stapel:
    x, y = stapel.pop()
    if x < 0 or y < 0 or x >= w or y >= h or frei[y][x]: continue
    if not hell(quelle(x, y)): continue
    frei[y][x] = True
    stapel += [(x+1,y), (x-1,y), (x,y+1), (x,y-1)]

# ---- Himmel --------------------------------------------------------------
HORIZONT = 58
def mische(a, c, t):
    return tuple(int(a[i] + (c[i]-a[i]) * t) for i in range(3))

hg = [[(0,0,0)]*N for _ in range(N)]
OBEN, MITTE, UNTEN = (38, 20, 74), (233, 96, 60), (255, 206, 120)
for y in range(N):
    if y <= HORIZONT:
        t = y / float(HORIZONT)
        f = mische(OBEN, MITTE, min(1.0, t*1.45)) if t < 0.7 \
            else mische(MITTE, UNTEN, (t-0.7)/0.3)
    else:                                    # Wasser, dunkler und kuehler
        t = (y - HORIZONT) / float(N - HORIZONT)
        f = mische((196,110,96), (48,28,74), t)
    for x in range(N):
        hg[y][x] = f

# Sonne
SX, SY, SR = 38, 11, 9
for y in range(SY - SR, min(HORIZONT + 1, SY + SR + 1)):
    for x in range(SX - SR, SX + SR + 1):
        if 0 <= x < N and 0 <= y < N:
            d = math.hypot(x - SX, y - SY)
            if d <= SR:
                k = 1.0 - (d / SR) * 0.35
                hg[y][x] = tuple(min(255, int(c*k)) for c in (255, 232, 150))
# Retro-Streifen durch die Sonne
for i, yy in enumerate(range(SY + 1, SY + SR, 3)):
    for x in range(N):
        if 0 <= yy < N and math.hypot(x - SX, yy - SY) <= SR:
            hg[yy][x] = mische(hg[yy][x], (150, 50, 70), 0.75)

# Spiegelung auf dem Wasser
for y in range(HORIZONT + 1, N):
    if (y - HORIZONT) % 3 == 0:
        for x in range(SX - 9, SX + 10):
            if 0 <= x < N:
                hg[y][x] = mische(hg[y][x], (255, 200, 130), 0.45)

# ---- Palmen --------------------------------------------------------------
SCHWARZ = (14, 10, 22)
def punkt(x, y, dick=1):
    for dy in range(dick):
        for dx in range(dick):
            xi, yi = int(round(x))+dx, int(round(y))+dy
            if 0 <= xi < N and 0 <= yi < N: hg[yi][xi] = SCHWARZ

def bogen(p0, p1, p2, dick=1):
    """Quadratische Bezier - Anfang, Zugpunkt, Ende."""
    for i in range(31):
        t = i / 30.0
        x = (1-t)**2*p0[0] + 2*(1-t)*t*p1[0] + t*t*p2[0]
        y = (1-t)**2*p0[1] + 2*(1-t)*t*p1[1] + t*t*p2[1]
        punkt(x, y, dick)

def palme(fuss_x, hoehe, neigung, spann):
    kx, ky = fuss_x, N - 1
    sx, sy = fuss_x + neigung, N - 1 - hoehe          # Krone

    # Stamm, leicht gebogen und unten dicker
    for i in range(41):
        t = i / 40.0
        x = (1-t)**2*kx + 2*(1-t)*t*(kx + neigung*0.15) + t*t*sx
        y = (1-t)**2*ky + 2*(1-t)*t*(ky - hoehe*0.5)   + t*t*sy
        punkt(x, y, 2 if t < 0.55 else 1)

    # Sechs Wedel: erst hinaus und hinauf, dann herunterhaengend. Der
    # Zugpunkt liegt ueber der Mitte, das gibt den Bogen.
    wedel = [(-1.00, 0.50, 0.55), (-0.78, 0.85, 0.62), (-0.32, 1.00, 0.55),
             ( 0.32, 1.00, 0.55), ( 0.78, 0.85, 0.62), ( 1.00, 0.50, 0.55)]
    for fx, fy, hoch in wedel:
        ex, ey = sx + fx*spann, sy + fy*spann
        zx, zy = sx + fx*spann*0.55, sy - hoch*spann
        bogen((sx, sy), (zx, zy), (ex, ey), 1)

    # Kronenansatz etwas fuellen
    punkt(sx-1, sy-1, 2)

palme(8,  66, 4, 10)
palme(69, 62, -4, 8)

# ---- Schild darueber -----------------------------------------------------
ox, oy = (N - w)//2, (N - h)//2     # unveraendert, wie in der Vorlage
for y in range(h):
    for x in range(w):
        if frei[y][x]: continue
        zx, zy = ox + x, oy + y
        if 0 <= zx < N and 0 <= zy < N:
            hg[zy][zx] = quelle(x, y)

# ---- Ausgabe: Vorschau und Rohdaten -------------------------------------
sys.path.insert(0, '/Users/ecki/Documents/Arduino/M5Cardputer_WebRadio/tools')
from gfx import Display, rgb
d = Display(N, N)
for y in range(N):
    for x in range(N):
        r, g, bl = hg[y][x]
        d.draw_pixel(x, y, rgb(r, g, bl))
d.save_png(S + '/krth_sunset.png', scale=4)

roh = bytearray()
for y in range(N):
    for x in range(N):
        r, g, bl = hg[y][x]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (bl >> 3)
        roh += bytes((v >> 8, v & 0xFF))
open(S + '/krth_sunset.565', 'wb').write(roh)
print('Vorschau und %d Bytes Rohdaten erzeugt' % len(roh))
