#!/usr/bin/env python3
"""Inspect, diff and edit Frontier: Elite II save files (see savefile.py).

  fe2save.py info  SAVE
  fe2save.py diff  A B                 changed bytes, A6 globals and big space, grouped
  fe2save.py set   IN OUT field=value ...   (fields: see FIELDS; a6[OFF]=hex, ship[OFF]=hex, ram[ADDR]=hex too)
  fe2save.py scenario FILE [OUT]     build a save from a scenario file (see scenarios/README.md)
  fe2save.py raw   IN OUT              decoded memory image (big space then A6 block) for hex editors

Addresses are 68k RAM addresses as the game sees them: A6 = 0x15e, big space = 0xd15e.
Fields: name, cash (credits), ship (model word), fuel (tonnes), hull, equipment (hex long),
ship_pos=X,Y,Z (km, relative to the ship's frame parent, normally the planet),
ship_up=KM (move the ship that far further from its parent, keeping direction).
Objects: 115 slots, the flag bytes are the first 115 bytes of big space; slot 0 is at $74, slot n at
$d4+286*(n-1). In an object +44/+48/+52 are the low longs and +56/+60/+64 the high longs of the 64-bit
position (mm; one high unit = 2^32 mm = 4295 km), relative to the frame parent.
"""
import struct, sys
from savefile import Save, BIG_SIZE

A6_BASE = 0x15E
BIG_BASE = 0xD15E


def ship_addr(s):
    return struct.unpack(">I", s.a6[674:678])[0]


def mem(s, addr, n):
    """(buffer, offset) holding RAM addr, or raise"""
    if A6_BASE <= addr and addr + n <= A6_BASE + len(s.a6):
        return s.a6, addr - A6_BASE
    if BIG_BASE <= addr and addr + n <= BIG_BASE + BIG_SIZE:
        return s.big, addr - BIG_BASE
    raise ValueError("address %#x is not in the saved memory" % addr)


def put(s, addr, data):
    buf, off = mem(s, addr, len(data))
    buf[off:off + len(data)] = data


def get(s, addr, n):
    buf, off = mem(s, addr, n)
    return bytes(buf[off:off + n])


def slot_off(i):
    return 0x74 if i == 0 else 0xD4 + 286 * (i - 1)


def obj_pos(s, off):
    """position of the object at big-space offset `off` in mm"""
    return [(struct.unpack(">i", s.big[off + 56 + 4 * a:off + 60 + 4 * a])[0] << 32) +
            struct.unpack(">I", s.big[off + 44 + 4 * a:off + 48 + 4 * a])[0] for a in range(3)]


def set_obj_pos(s, off, p):
    for a in range(3):
        s.big[off + 56 + 4 * a:off + 60 + 4 * a] = struct.pack(">i", p[a] >> 32)
        s.big[off + 44 + 4 * a:off + 48 + 4 * a] = struct.pack(">I", p[a] & 0xFFFFFFFF)


def move_ship(s, new):
    """new(old position in mm) -> new position; objects sharing the ship's position (its parts) follow"""
    ship = ship_addr(s) - BIG_BASE
    old = obj_pos(s, ship)
    target = new(old)
    for i in range(115):
        off = slot_off(i)
        if s.big[i] and (off == ship or obj_pos(s, off) == old):
            set_obj_pos(s, off, target)


def info(s):
    a = s.a6
    sh = ship_addr(s)
    print("commander %s" % bytes(a[14400:14416]).split(b"\0")[0].decode("latin1"))
    print("cash %.1f cr, cargo space %d, cargo used %d" % (struct.unpack(">i", a[734:738])[0] / 10,
                                                           *struct.unpack(">HH", a[738:742])))
    print("time of day %d, day %d" % struct.unpack(">ii", a[524:532]))
    print("elite pts %d, federal %d, imperial %d" % (struct.unpack(">I", a[824:828])[0], *struct.unpack(">HH", a[828:832])))
    print("flight state %#x, screen owner %d" % (a[704], struct.unpack(">H", a[6:8])[0]))
    print("player ship object %#x" % sh)
    o = lambda off, n: get(s, sh + off, n)
    print("  model word %d, equipment %#010x, damaged %#010x" % (struct.unpack(">H", o(90, 2))[0],
          struct.unpack(">I", o(158, 4))[0], struct.unpack(">I", o(162, 4))[0]))
    print("  drive %d, mounts %d, lasers %s, pylons %s" % (o(166, 1)[0], o(167, 1)[0], list(o(168, 4)), list(o(172, 8))))
    print("  position km %s" % [round(c / 1e6, 3) for c in obj_pos(s, sh - BIG_BASE)])
    print("  hull %d, shield %d/%d, fuel %.2ft" % (*struct.unpack(">H", o(184, 2)), *struct.unpack(">HH", o(180, 4))[:2],
          struct.unpack(">I", o(242, 4))[0] / 0x20000000))


def diff(a, b):
    for title, x, y, base in (("A6", a.a6, b.a6, A6_BASE), ("big space", a.big, b.big, BIG_BASE)):
        i, n = 0, len(x)
        print("--", title)
        while i < n:
            if x[i] == y[i]:
                i += 1
                continue
            j = i
            while j < n and (x[j] != y[j] or (j + 1 < n and x[j + 1] != y[j + 1])):
                j += 1
            print("%#08x (%s+%d) %d bytes: %s -> %s" % (base + i, "A6" if base == A6_BASE else "big", i, j - i,
                  bytes(x[i:j]).hex(), bytes(y[i:j]).hex()))
            i = j


def setfield(s, spec):
    k, v = spec.split("=", 1)
    sh = ship_addr(s)
    if k == "name":
        s.a6[14400:14416] = v.encode("latin1")[:15].ljust(16, b"\0")
    elif k == "cash":
        s.a6[734:738] = struct.pack(">i", int(float(v) * 10))
    elif k == "ship":
        put(s, sh + 90, struct.pack(">H", int(v, 0)))
    elif k == "equipment":
        put(s, sh + 158, struct.pack(">I", int(v, 0)))
    elif k == "hull":
        put(s, sh + 184, struct.pack(">H", int(v, 0)))
    elif k == "fuel":
        put(s, sh + 242, struct.pack(">I", int(float(v) * 0x20000000)))
    elif k == "vel":
        # vel=SLOT,x,y,z (raw velocity longs, +98); the fraction words at +192 are cleared
        n, *v = [int(x, 0) for x in v.split(",")]
        off = slot_off(n)
        s.big[off + 98:off + 110] = struct.pack(">3i", *v)
        s.big[off + 192:off + 198] = bytes(6)
    elif k == "objup":
        # objup=SLOT,km: move that object further from its parent, keeping direction
        n, d = v.split(",")
        off = slot_off(int(n))
        pos = obj_pos(s, off)
        r = sum(c * c for c in pos) ** 0.5
        set_obj_pos(s, off, [round(c * (r + float(d) * 1e6) / r) for c in pos])
    elif k == "objhull":
        n, h = [int(x, 0) for x in v.split(",")]
        s.big[slot_off(n) + 184:slot_off(n) + 186] = struct.pack(">H", h)
    elif k == "ship_pos":
        km = [float(x) for x in v.split(",")]
        move_ship(s, lambda old: [round(c * 1e6) for c in km])
    elif k == "ship_up":
        d = float(v) * 1e6
        def up(old):
            r = sum(c * c for c in old) ** 0.5
            return [round(c * (r + d) / r) for c in old]
        move_ship(s, up)
    elif k.startswith(("a6[", "ship[", "ram[")):
        which, off = k[:-1].split("[")
        off = int(off, 0)
        addr = {"a6": A6_BASE, "ship": sh, "ram": 0}[which] + off
        put(s, addr, bytes.fromhex(v))
    else:
        raise ValueError("unknown field " + k)


def scenario(path, out=None):
    """lines: `base FILE` (first, relative to the scenario file), `out FILE`, or `field value` as for set;
    # starts a comment"""
    import os
    here = os.path.dirname(os.path.abspath(path))
    s = None
    for line in open(path):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        key, _, val = line.partition(" ")
        val = val.strip()
        if key == "base":
            s = Save.load(os.path.join(here, val))
        elif key == "out":
            out = out or os.path.join(here, val)
        elif s is None:
            raise ValueError("`base` must come first")
        else:
            setfield(s, key + "=" + val)
    if s is None or not out:
        raise ValueError("a scenario needs `base` and `out` (or an OUT argument)")
    s.save(out)
    print("wrote", out)


def main(argv):
    if len(argv) >= 3 and argv[1] == "info":
        info(Save.load(argv[2]))
    elif len(argv) == 4 and argv[1] == "diff":
        diff(Save.load(argv[2]), Save.load(argv[3]))
    elif len(argv) >= 4 and argv[1] == "set":
        s = Save.load(argv[2])
        for spec in argv[4:]:
            setfield(s, spec)
        s.save(argv[3])
    elif len(argv) in (3, 4) and argv[1] == "scenario":
        scenario(argv[2], argv[3] if len(argv) == 4 else None)
    elif len(argv) == 4 and argv[1] == "raw":
        s = Save.load(argv[2])
        open(argv[3], "wb").write(bytes(s.big) + bytes(s.a6))
    else:
        print(__doc__)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
