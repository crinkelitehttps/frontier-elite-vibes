#!/usr/bin/env python3
"""Build the very simplified Kerbal Space Center for Kerbin (systems/kerbol.json): a surface starport
whose pads are the runway and the launch pad.

The buildings are a few boxes each, sized and placed from the layout tools/ksp_ksc.py prints (rotated
rectangles fitted to each building, then simplified by hand), scaled up by --scale (default 4: KSP's
KSC next to Frontier's ships and starports looks tiny). No KSP geometry is in here or in the output,
only these dimensions. Output: one .fe2m holding

  - the port model (number --index, default 480): vertices = the facility origins and pad origins, on
    the ground (y = 0); code = draw each facility model at its vertex; hull = the pads, as the game's own
    ground ports have them (model 48 -> 164 -> 209): record $80 + n, bits $22 (bit 5: "over pad n"),
    then a pad model at its vertex; then model 48's "50 04" record;
  - one model per facility (index + 1, 2, ...), built from boxes by FE2ShipBuilder's builder;
  - one hull-only model per pad: four vertical planes facing in round a rectangle. A ship over the
    rectangle that touches down gently on the ground is on that pad (L620a4: pad bit 5 + touch bit 6).

Each facility is its own model so its boxes keep fine precision (a model's coordinates are signed bytes
<< scale), and its origin is on the ground, so the port's coarse grid only moves it sideways (the
origins are snapped to that grid first and the boxes placed relative to the snapped origin, so nothing
moves at all). The KSC is centred on the port's origin.

kerbol.json needs the pad centres and parking spots this prints (the game's autopilot, traffic and
launches use them; see Call_CustomPad in src/custom_system.c), in the same order as the pads here.

    g++ ... tools/fe2ShipBuilder/build_cli.cpp ...        (see the top of that file)
    python3 tools/ksc_model.py --builder ./fe2build_cli   # writes tools/fe2ShipBuilder/custom_ships/480.fe2m
    python3 tools/ksc_model.py --builder ./fe2build_cli --obj /tmp/ksc   # also the OBJs, to look at

Frame of the box list: metres, x = east, z = north, y = up from the ground (KSP's KSC frame with its
facility ground at 0), before scaling.
"""
import argparse, math, os, struct, subprocess, sys, tempfile

# shapes: (kind, cx, cz, length, width, yaw_deg, y0, y1, colour[, pitch_deg])
#   kind "box", "wedge" (gable roof, ridge along the length at y1) or "flat" (one face at y0, facing up)
#   yaw: angle of the length axis from east toward north; colour: the game's 12-bit value (fe2_RGB)
FACILITIES = [
    ("vab", (-195, -14), [
        ("box", -195, -10, 104, 92, 90, 0, 105, "666"),     # main hall
        ("box", -201, -90, 56, 56, 90, 0, 52, "555"),       # south tower
        ("box", -201, -90, 88, 56, 0, 0, 15, "444"),        # south annex
        ("box", -189, 54, 64, 24, 0, 0, 22, "555"),         # north offices
        ("box", -196, -63, 50, 1, 0, 6, 98, "235"),         # door (south face)
        ("box", -283, 72, 15, 15, 0, 0, 18, "666"),         # tank
    ]),
    ("sph", (-340, 268), [
        ("box", -339, 266, 160, 100, -60, 0, 30, "555"),    # hangar walls
        ("wedge", -339, 266, 160, 100, -60, 30, 46, "444"), # roof
        ("box", -347, 344, 24, 24, -60, 0, 88, "666"),      # control tower
        ("box", -458, 273, 16, 16, 0, 0, 18, "666"),        # tank
    ]),
    ("mission", (-150, 230), [
        ("box", -150, 230, 97, 40, 90, 0, 21, "556"),
        ("box", -150, 230, 60, 20, 90, 21, 25, "444"),      # roof block
    ]),
    ("crew", (-560, 60), [
        ("box", -478, 28, 72, 32, 90, 0, 22, "556"),        # astronaut complex, south wing
        ("box", -490, 95, 110, 40, -20, 0, 22, "556"),      # astronaut complex, north wing
        ("box", -657, 47, 74, 29, 90, 0, 14, "565"),        # administration
        ("box", -657, 41, 18, 18, 0, 14, 17, "235"),        # its roof
    ]),
    ("flag", (-557, 25), [
        ("box", -557, 25, 1.5, 1.5, 0, 0, 46, "777"),       # flag pole
    ]),
    ("rnd_north", (-430, -120), [
        ("box", -386, -88, 104, 32, 90, 0, 25, "666"),      # main building, north arm
        ("box", -370, -172, 96, 80, 0, 0, 25, "666"),       # main building, south part
        ("box", -492, -132, 82, 56, 30, 0, 31, "666"),      # central building
        ("box", -560, -159, 87, 53, 90, 0, 20, "555"),      # side lab
        ("box", -452, -169, 48, 10, -60, 15, 21, "444"),    # bridge
        ("box", -462, -66, 29, 25, 30, 0, 8, "555"),        # small lab
    ]),
    ("rnd_south", (-450, -280), [
        ("box", -433, -308, 145, 69, 30, 0, 27, "555"),     # wind tunnel
        ("box", -553, -251, 96, 52, -77, 0, 22, "666"),     # observatory
        ("box", -553, -251, 30, 30, -77, 22, 32, "777"),    # its dome
        ("box", -343, -232, 51, 50, 0, 0, 10, "555"),       # corner lab
    ]),
    ("tracking", (-50, -318), [
        ("box", -50, -318, 60, 60, 0, 0, 15, "666"),        # hub
        ("box", 57, -318, 12, 12, 0, 0, 18, "444"),         # east dish: pedestal
        ("box", 57, -318, 22, 22, 0, 22, 25, "777", 45),    # dish, tilted
    ]),
    ("dishes", (-103, -318), [
        ("box", -103, -225, 12, 12, 0, 0, 18, "444"),
        ("box", -103, -410, 12, 12, 0, 0, 18, "444"),
        ("box", -103, -225, 22, 22, 0, 22, 25, "777", 45),
        ("box", -103, -410, 22, 22, 0, 22, 25, "777", 45),
    ]),
    ("pad", (440, 0), [
        ("box", 427, 0, 86, 40, 0, 0, 5, "444"),            # pad
        ("box", 447, 0, 13, 13, 0, 5, 6, "222"),            # flame trench grate
        ("box", 475, -66, 10, 10, 0, 0, 11, "666"),         # tank
        ("box", 500, 55, 8, 8, 0, 0, 16, "666"),            # water tower
        ("box", 440, 75, 1.5, 1.5, 0, 0, 38, "777"),        # lightning pole
    ]),
    ("ground", (0, 250), [
        ("flat", -88, 497, 2745, 70, 0, 0, 0, "222"),       # runway
        ("flat", 122, 0, 351, 40, 0, 0, 0, "444"),          # crawlerway
    ]),
]
# pads, in order (pad 1, 2, ...): name, rectangle x0, z0, x1, z1 (same frame as the boxes)
PADS = [
    ("launch pad", 370, -40, 490, 40),
    ("runway", -1461, 462, 1284, 532),
]
# where parked ships stand (the game picks one of 4), x, z
PARKING = [(200, 400), (700, 400), (300, -160), (650, -160)]
TEMPLATE = 155  # a starport building: header values for the facility models
PORT_X10, PORT_X14 = 0x15, 0x14  # header words as model 48's (London's starport)


def corners(cx, cz, length, width, yaw):
    """the 4 base corners (x, z), counter-clockwise seen from above in east/north axes"""
    c, s = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    out = []
    for du, dv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
        u, v = du * length / 2, dv * width / 2
        out.append((cx + u * c - v * s, cz + u * s + v * c))
    return out


def shape_faces(sh):
    """faces of one shape as lists of (east, up, north) points"""
    kind, cx, cz, length, width, yaw, y0, y1 = sh[:8]
    pitch = sh[9] if len(sh) > 9 else 0
    base = corners(cx, cz, length, width, yaw)
    if kind == "flat":
        return [[(x, y0, z) for x, z in base]], None
    if kind == "wedge":
        c, s = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
        r0 = (cx - length / 2 * c, y1, cz - length / 2 * s)
        r1 = (cx + length / 2 * c, y1, cz + length / 2 * s)
        b = [(x, y0, z) for x, z in base]  # b0 b1 on the -v side, b2 b3 on +v
        faces = [[b[0], b[1], r1, r0], [b[2], b[3], r0, r1], [b[1], b[2], r1], [b[3], b[0], r0]]
        return faces, (cx, (y0 + y1) / 2, cz)
    lo = [(x, y0, z) for x, z in base]
    hi = [(x, y1, z) for x, z in base]
    faces = [hi] + [[lo[i], lo[(i + 1) % 4], hi[(i + 1) % 4], hi[i]] for i in range(4)]
    centre = (cx, (y0 + y1) / 2, cz)
    if pitch:  # turn about the length axis through the centre
        c, s = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
        u = (c, 0.0, s)
        a = math.radians(pitch)

        def rot(p):
            d = [p[i] - centre[i] for i in range(3)]
            k = sum(d[i] * u[i] for i in range(3))
            cr = (u[1] * d[2] - u[2] * d[1], u[2] * d[0] - u[0] * d[2], u[0] * d[1] - u[1] * d[0])
            return tuple(centre[i] + d[i] * math.cos(a) + cr[i] * math.sin(a) + u[i] * k * (1 - math.cos(a))
                         for i in range(3))
        faces = [[rot(p) for p in f] for f in faces]
        lo = [rot(p) for p in lo]
        faces.append(lo[::-1])  # a tilted slab shows its underside
    return faces, centre


def write_obj(path, shapes, origin):
    """OBJ in metres relative to origin (east, north): x = east, y = up, z = south"""
    ox, oz = origin
    verts, lines = {}, []
    out_v, out_f = [], []
    for sh in shapes:
        faces, centre = shape_faces(sh)
        for f in faces:
            pts = [(x - ox, y, -(z - oz)) for x, y, z in f]
            # counter-clockwise from outside: the normal (b-a)x(c-a) points away from the centre (or up)
            a, b, c = pts[0], pts[1], pts[2]
            u = [b[i] - a[i] for i in range(3)]
            v = [c[i] - a[i] for i in range(3)]
            n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
            if centre is None:
                out = (0, 1, 0)
            else:
                m = [sum(p[i] for p in pts) / len(pts) for i in range(3)]
                cc = (centre[0] - ox, centre[1], -(centre[2] - oz))
                out = [m[i] - cc[i] for i in range(3)]
            if sum(n[i] * out[i] for i in range(3)) < 0:
                pts = pts[::-1]
            idx = []
            for p in pts:
                key = tuple(round(q, 3) for q in p)
                if key not in verts:
                    verts[key] = len(out_v) + 1
                    out_v.append(key)
                idx.append(verts[key])
            out_f.append((sh[8], idx))
    with open(path, "w") as f:
        f.write("# simplified KSC facility (tools/ksc_model.py)\n")
        for v in out_v:
            f.write("v %.3f %.3f %.3f\n" % v)
        cur = None
        for col, idx in out_f:
            if col != cur:
                f.write("usemtl fe2_%s\n" % col)
                cur = col
            f.write("f " + " ".join(map(str, idx)) + "\n")
    return len(out_v)


def read_fe2m(path):
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"FE2M":
        raise SystemExit(path + ": not a .fe2m")
    count = struct.unpack(">H", d[6:8])[0]
    pos, models = 8, []
    for _ in range(count):
        index, _, size = struct.unpack(">HHI", d[pos:pos + 8])
        pos += 8
        models.append((index, d[pos:pos + size]))
        pos += (size + 1) & ~1
    return models


def fit(ext_mm):
    scale = 0
    while ext_mm / (1 << scale) > 126:
        scale += 1
    return scale


def header(code_off, nverts, norm_off, nnorms, scale, radius, colour, coll_off):
    radius = max(1, min(255, radius))
    return struct.pack(">15H", code_off, 0x1E, nverts * 64, norm_off, (nnorms + 1) * 4, scale, 0, radius,
                       PORT_X10, colour, PORT_X14, (radius << 8) | 0xDF, (radius << 8) | 0xDF, coll_off, 0)


def pad_model(x0, z0, x1, z1):
    """a hull-only model: the rectangle (metres from its origin, x east, z north) as four vertical planes
    facing in; the game counts a point inside all four (by the ship's radius) as over the pad"""
    scale = fit(max(abs(x0), abs(x1), abs(z0), abs(z1)) * 1000)
    k = (1 << scale) / 1000.0
    q = lambda v: round(v / k)
    corners = [(q(x0), q(z0)), (q(x1), q(z0)), (q(x1), q(z1)), (q(x0), q(z1))]
    verts = b"".join(struct.pack(">bbbb", 1, x, 0, z) for x, z in corners)
    # (vertex 2N, nx, ny, nz): west side +x, east side -x, south side +z, north side -z
    norms = struct.pack(">bbbb", 0, 127, 0, 0) + struct.pack(">bbbb", 2, -127, 0, 0) + \
        struct.pack(">bbbb", 0, 0, 0, 127) + struct.pack(">bbbb", 4, 0, 0, -127)
    code = struct.pack(">H", 0)  # draws nothing
    hull = bytes([0x40, 0x22, 2, 4, 6, 8, 0, 0])
    norm_off = 0x1E + len(verts)
    code_off = norm_off + len(norms)
    coll_off = code_off + len(code)
    radius = max(max(abs(x), abs(z)) for x, z in corners) + 1
    return header(code_off, 4, norm_off, 4, scale, radius, 0x666, coll_off) + verts + norms + code + hull


def port_model(points, scale, children, pads, radius_m):
    """the port: vertices = points (x, z metres, already on the grid); code draws children[i] at vertex i;
    hull: pad n (1-based) = pads[n - 1] = (model, vertex index), then model 48's always-hit "50 04" record"""
    k = (1 << scale) / 1000.0
    verts = b"".join(struct.pack(">bbbb", 1, round(x / k), 0, round(z / k)) for x, z in points)
    code = b""
    for i, child in enumerate(children):
        code += struct.pack(">HH", (child << 5) | 0x0E, 2 * i)  # model CHILD v:2i
    code += struct.pack(">H", 0)  # done
    hull = b""
    for n, (model, vertex) in enumerate(pads, 1):
        hull += bytes([0x80 | n, 0x22]) + struct.pack(">H", model) + bytes([0, 2 * vertex])
    hull += bytes([0x50, 0x04, 0x00, 0x00])
    norm_off = code_off = 0x1E + len(verts)
    coll_off = code_off + len(code)
    m = header(code_off, len(points), norm_off, 0, scale, int(radius_m / k) + 1, 0x666, coll_off) + verts + code + hull
    return m + (b"\0" if len(m) & 1 else b"")


def snapped(x, z, scale):
    k = (1 << scale) / 1000.0
    return round(x / k) * k, round(z / k) * k


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--builder", required=True, help="fe2build_cli (tools/fe2ShipBuilder/build_cli.cpp) or FE2ShipBuilder")
    ap.add_argument("--index", type=int, default=480, help="port model number (facilities and pads follow it)")
    ap.add_argument("--scale", type=float, default=4.0, help="size against KSP's KSC (default 4)")
    ap.add_argument("--out", default=os.path.join(here, "fe2ShipBuilder", "custom_ships"))
    ap.add_argument("--obj", help="keep the facility OBJs in this folder")
    a = ap.parse_args()

    f = a.scale
    # centre the KSC (boxes and pads) on the port's origin, then scale
    xs, zs = [], []
    for _, _, shapes in FACILITIES:
        for sh in shapes:
            for x, z in corners(*sh[1:6]):
                xs.append(x)
                zs.append(z)
    cx, cz = (min(xs) + max(xs)) / 2, (min(zs) + max(zs)) / 2
    tr = lambda x, z: ((x - cx) * f, (z - cz) * f)

    def scaled(sh):
        x, z = tr(sh[1], sh[2])
        return (sh[0], x, z, sh[3] * f, sh[4] * f, sh[5], sh[6] * f, sh[7] * f) + tuple(sh[8:])

    facilities = [(name, tr(*o), [scaled(sh) for sh in shapes]) for name, o, shapes in FACILITIES]
    pads = [(name, tr(x0, z0), tr(x1, z1)) for name, x0, z0, x1, z1 in PADS]
    reach = 0.0
    for _, _, shapes in facilities:
        for sh in shapes:
            for x, z in corners(*sh[1:6]):
                reach = max(reach, abs(x), abs(z))
    pscale = fit(reach * 1000)

    work = a.obj or tempfile.mkdtemp(prefix="ksc_")
    os.makedirs(work, exist_ok=True)
    models, points, children = [], [], []
    index = a.index + 1
    for name, (ox, oz), shapes in facilities:
        px, pz = snapped(ox, oz, pscale)
        obj = os.path.join(work, name + ".obj")
        nv = write_obj(obj, shapes, (px, pz))
        fe2m = os.path.join(work, name + ".fe2m")
        r = subprocess.run([a.builder, "--build", obj, str(TEMPLATE), str(index), fe2m, "bare"],
                           capture_output=True, text=True)
        if r.returncode:
            raise SystemExit(f"{name}: {r.stderr.strip() or r.stdout.strip()}")
        built = read_fe2m(fe2m)
        if len(built) != 1:
            raise SystemExit(f"{name}: split into {len(built)} models (too many vertices: {nv}); make two facilities")
        print(f"{name:10s} model {index}  {nv:2d} vertices  at {px:8.1f} {pz:8.1f}  {r.stdout.splitlines()[0]}")
        for line in r.stdout.splitlines()[1:]:
            print("   ", line)
        models += built
        points.append((px, pz))
        children.append(index)
        index += 1
    pad_refs, centres = [], []
    for name, (x0, z0), (x1, z1) in pads:
        px, pz = snapped((x0 + x1) / 2, (z0 + z1) / 2, pscale)
        models.append((index, pad_model(x0 - px, z0 - pz, x1 - px, z1 - pz)))
        pad_refs.append((index, len(points)))
        points.append((px, pz))
        centres.append(((x0 + x1) / 2, (z0 + z1) / 2))
        print(f"pad {len(pad_refs)} ({name}) model {index}: {x1 - x0:.0f} x {z1 - z0:.0f} m")
        index += 1
    models.insert(0, (a.index, port_model(points, pscale, children, pad_refs, reach * 1.42)))
    os.makedirs(a.out, exist_ok=True)
    out = os.path.join(a.out, f"{a.index}.fe2m")
    with open(out, "wb") as fo:
        fo.write(b"FE2M" + struct.pack(">HH", 1, len(models)))
        for i, m in models:
            fo.write(struct.pack(">HHI", i, 0, len(m)) + m + (b"\0" if len(m) & 1 else b""))
    print(f"port model {a.index} (scale {pscale}, reach {reach:.0f} m, x{f:g}) -> {out}")
    fmt = lambda pts: "[" + ", ".join(f"[{x:.0f}, {z:.0f}]" for x, z in pts) + "]"
    print(f'kerbol.json: "pads": {fmt(centres)}, "parking": {fmt([tr(x, z) for x, z in PARKING])}')


if __name__ == "__main__":
    main()
