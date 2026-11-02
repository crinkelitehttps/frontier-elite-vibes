#!/usr/bin/env python3
"""Trace Earth's land outlines from Natural Earth 1:110m land (CC0) into
src/gl/gl_earth_outlines.h, for planet model 72 (see docs/planet-coastline-plan.md, phase 3).

  trace.py [--budget 1000] [--min-area 25000] [--detail 1] [--out FILE]
  trace.py --game-hex HEXFILE --overlay overlay.png    # compare with the game's own Earth

The data file is data/ne_110m_land.geojson (next to this script). Rings are simplified together
(Visvalingam-Whyatt on one queue) to a total point budget; small islands are dropped; Antarctica is left
to the game's ice circle. The header holds real latitude/longitude in degrees; the game's mirrored axes
are applied in gl_planet_tex.c (x = cos(lat) sin(-lon), y = sin(lat), z = cos(lat) cos(-lon)).
--game-hex takes the hex dump of model 72's planet command (devtools/models/praw 72), which is not
committed (game data); --overlay then draws both outline sets on an equirectangular map in real
longitudes so the frame alignment can be checked by eye.
"""
import argparse, heapq, json, math, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
R_KM = 6371.0


def ring_area_km2(ring):
    lat0 = sum(p[1] for p in ring) / len(ring)
    k = math.cos(math.radians(lat0))
    a = 0.0
    for i in range(len(ring)):
        x1, y1 = ring[i][0] * k, ring[i][1]
        x2, y2 = ring[(i + 1) % len(ring)][0] * k, ring[(i + 1) % len(ring)][1]
        a += x1 * y2 - x2 * y1
    return abs(a) / 2 * (math.pi / 180 * R_KM) ** 2


def tri_area(a, b, c):
    k = math.cos(math.radians((a[1] + b[1] + c[1]) / 3))
    ax, ay = a[0] * k, a[1]
    bx, by = b[0] * k, b[1]
    cx, cy = c[0] * k, c[1]
    return abs((bx - ax) * (cy - ay) - (cx - ax) * (by - ay)) / 2 * (math.pi / 180 * R_KM) ** 2


def load_rings(path, min_area):
    d = json.load(open(path))
    rings = []
    for f in d["features"]:
        g = f["geometry"]
        polys = [g["coordinates"]] if g["type"] == "Polygon" else g["coordinates"]
        for poly in polys:
            for ri, r in enumerate(poly):
                pts = [(p[0], p[1]) for p in r[:-1]]  # drop the repeated closing point
                if ri == 0 and min(p[1] for p in pts) < -60:
                    continue  # Antarctica: the game's ice circle covers it
                if ring_area_km2(pts) < min_area:
                    continue
                rings.append((ri > 0, pts))
    return rings


def simplify(rings, budget, floor=4):
    """Visvalingam-Whyatt over all rings with one queue until `budget` points remain."""
    prev, nxt, alive, count = [], [], [], []
    heap = []
    ver = []
    for ri, (_, pts) in enumerate(rings):
        n = len(pts)
        prev.append([(i - 1) % n for i in range(n)])
        nxt.append([(i + 1) % n for i in range(n)])
        alive.append([True] * n)
        ver.append([0] * n)
        count.append(n)
        for i in range(n):
            heapq.heappush(heap, (tri_area(pts[prev[ri][i]], pts[i], pts[nxt[ri][i]]), ri, i, 0))
    total = sum(count)
    while total > budget and heap:
        a, ri, i, v = heapq.heappop(heap)
        if not alive[ri][i] or ver[ri][i] != v or count[ri] <= floor:
            continue
        alive[ri][i] = False
        p, n = prev[ri][i], nxt[ri][i]
        nxt[ri][p], prev[ri][n] = n, p
        count[ri] -= 1
        total -= 1
        pts = rings[ri][1]
        for j in (p, n):
            ver[ri][j] += 1
            heapq.heappush(heap, (tri_area(pts[prev[ri][j]], pts[j], pts[nxt[ri][j]]), ri, j, ver[ri][j]))
    out = []
    for ri, (hole, pts) in enumerate(rings):
        out.append([pts[i] for i in range(len(pts)) if alive[ri][i]])
    return out


# ---- the game's own Earth, for the overlay ------------------------------------------------------

def s8(b):
    return b - 256 if b > 127 else b


def parse_game(hexfile):
    raw = bytes(int(x, 16) for x in re.findall(r"\b[0-9a-f]{2}\b", open(hexfile).read().split("\n", 1)[1]))
    d = raw[6:]  # the data starts 6 bytes after the op
    i = 8
    if d[0] & 0x10:
        i += 58
    elif not d[0] & 0x40:
        i += 8
    if (d[i] << 8 | d[i + 1]) != 0:
        i += 4
        while True:
            i += 2
            w = d[i] << 8 | d[i + 1]
            i += 2
            if not w:
                break
    else:
        i += 2
    outlines, circles = [], []
    while d[i]:
        c = d[i]
        i += 1
        if c & 0x80:
            circles.append(([-s8(d[i + k]) for k in range(3)], d[i + 3] * 64 / 65536 * 360, bool(c & 0x40)))
            i += 4
            continue
        i += 1  # detail
        pts = []
        x = d[i]
        i += 1
        while True:
            y, z = d[i], d[i + 1]
            i += 2
            if not pts and d[i] == 0:
                i += 2
                x = d[i]
                i += 1
                continue
            pts.append((s8(x), s8(y), s8(z)))
            x = d[i]
            i += 1
            if x == 0:
                break
        outlines.append(pts)
    return outlines, circles


def game_to_latlon(v):
    x, y, z = v
    l = math.sqrt(x * x + y * y + z * z)
    x, y, z = x / l, y / l, z / l
    lat = math.degrees(math.asin(y))
    lon = -math.degrees(math.atan2(x, z))  # x = cos(lat) sin(-lon), z = cos(lat) cos(-lon)
    return lon, lat


def overlay(rings, game, path, w=1600):
    from PIL import Image, ImageDraw
    h = w // 2
    im = Image.new("RGB", (w, h), (20, 30, 60))
    dr = ImageDraw.Draw(im)

    def px(p):
        return ((p[0] + 180) / 360 * w, (90 - p[1]) / 180 * h)

    for ring in rings:
        dr.line([px(p) for p in ring + [ring[0]]], fill=(80, 220, 80), width=1)
    if game:
        for o in game[0]:
            ll = [game_to_latlon(v) for v in o]
            for a, b in zip(ll, ll[1:] + ll[:1]):
                if abs(a[0] - b[0]) < 180:
                    dr.line([px(a), px(b)], fill=(255, 80, 80), width=2)
    for lon in range(-180, 181, 30):
        dr.line([px((lon, -90)), px((lon, 90))], fill=(60, 60, 90))
    for lat in range(-90, 91, 30):
        dr.line([px((-180, lat)), px((180, lat))], fill=(60, 60, 90))
    im.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--budget", type=int, default=1000)
    ap.add_argument("--min-area", type=float, default=25000, help="km^2 below which a ring is dropped")
    ap.add_argument("--detail", type=int, default=1, help="fractal detail byte for every outline")
    ap.add_argument("--data", default=os.path.join(HERE, "data", "ne_110m_land.geojson"))
    ap.add_argument("--out", default=os.path.join(HERE, "..", "..", "src", "gl", "gl_earth_outlines.h"))
    ap.add_argument("--game-hex")
    ap.add_argument("--overlay")
    a = ap.parse_args()

    rings = load_rings(a.data, a.min_area)
    simp = simplify(rings, a.budget)
    print("rings %d, points %d -> %d" % (len(rings), sum(len(r[1]) for r in rings), sum(len(s) for s in simp)),
          file=sys.stderr)
    if a.overlay:
        game = parse_game(a.game_hex) if a.game_hex else None
        overlay(simp, game, a.overlay)
    if a.overlay and not a.out:
        return
    lines = ["/* Generated by tools/planet_outlines/trace.py from Natural Earth 1:110m land (public domain).",
             " * Real latitude, longitude in degrees; one ring per outline (holes are rings too: even-odd). */",
             "#define EARTH_DETAIL %d" % a.detail,
             "static const float earth_points[][2] = {"]
    first = []
    n = 0
    for s in simp:
        first.append(n)
        for p in s:
            lines.append("\t{%.3ff, %.3ff}," % (p[1], p[0]))
        n += len(s)
    lines.append("};")
    lines.append("/* first point, count */")
    lines.append("static const int earth_rings[][2] = {")
    for f, s in zip(first, simp):
        lines.append("\t{%d, %d}," % (f, len(s)))
    lines.append("};")
    lines.append("#define EARTH_N_RINGS %d" % len(simp))
    open(a.out, "w").write("\n".join(lines) + "\n")


main()
