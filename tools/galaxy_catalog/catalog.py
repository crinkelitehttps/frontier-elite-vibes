#!/usr/bin/env python3
"""Galaxy catalog: a small set of systems that between them have every kind of
star, planet, moon and starport the game makes, at both ends of their ranges.

  catalog.py build OUT SCAN.jsonl...   pick the systems from scans made by
                                       FE2GalaxyViewer --catalog (see README.md)
  catalog.py find WORDS...             search systems.json, e.g.
                                       find gas giant mass>100
                                       find red radius>400000
                                       find station moon-of-gas
  catalog.py kinds                     the body kinds and their ranges
  catalog.py go [--km N] [--pick K] [--run GAME] WORDS...
                                       the same search, then the game command line that
                                       starts a new game, enters the K-th match's system
                                       (default the first) and goes N km above (or from) it;
                                       --run starts GAME with it

The scan lines are the atlas's own numbers (src/galaxy): the game's system
generator output, the mass and temperature its body panel prints, and the
radius of the model the game gives the body's object.
"""
import json
import os
import re
import sys
from array import array

HERE = os.path.dirname(os.path.abspath(__file__))
CATALOG = os.path.join(HERE, "systems.json")
MULTIPLES = ["", "Binary", "Ternary", "Quaternary", "Quintuple", "Sextuple"]


def star_class(s):
    """the primary's kind: 'Type'G'yellow star' etc. (the line without the 'N system. Primary:' prefix)"""
    d = s["star"]
    return d.split("Primary: ")[-1]


def multiplicity(s):
    for i, m in enumerate(MULTIPLES):
        if m and s["star"].startswith(m):
            return i + 1
    return 1


def read_scans(paths):
    seen = set()
    for p in paths:
        for line in open(p, encoding="utf-8", errors="replace"):
            try:
                s = json.loads(line)
            except ValueError:
                continue  # a scan still being written
            if s["id"] in seen:
                continue
            seen.add(s["id"])
            yield s


def tags(s):
    """the things a system can be the example of"""
    t = set()
    t.add("star:%s" % star_class(s))
    t.add("multiple:%d" % multiplicity(s))
    t.add("allegiance:%d" % s["allegiance"])
    bodies = {b["i"]: b for b in s["bodies"]}
    if sum(1 for b in s["bodies"] if b["port"]) >= 8:
        t.add("ports:many")
    for b in s["bodies"]:
        pk = bodies[b["p"]]["kn"] if b["p"] in bodies else "-"
        if b["port"]:
            t.add("port:%s" % b["kn"])
            t.add("port:%s on %s" % (b["kn"], pk))
        elif b["p"] >= 0:
            t.add("%s of %s" % (b["kn"], pk))
    return t


MEASURES = ("mass", "radius", "temp")
LABELS = (("min", 0.0), ("p50", 0.5), ("max", 1.0))


def build(out, scans):
    """Three passes over the scans, so the 100s of MB never sit in memory: the
    first finds every kind's value ranges and which systems have which tags,
    the second the system whose body is closest to each point of a range, the
    third picks the systems (greedy cover, rare things first, ties to the
    system nearest Sol) and keeps their lines"""
    values = {}  # (kind name, measure) -> all values
    providers = {}  # tag -> set of system ids
    where = {}
    for s in read_scans(scans):
        where[s["id"]] = s["sx"] ** 2 + s["sy"] ** 2
        for t in tags(s):
            providers.setdefault(t, set()).add(s["id"])
        for b in s["bodies"]:
            if not b["port"]:
                for m in MEASURES:
                    if m in b:
                        values.setdefault((b["kn"], m), array("d")).append(b[m])
    print("%d systems scanned, %d tags" % (len(where), len(providers)))
    points = {}  # (kind, measure, label) -> value
    for (kn, m), v in values.items():
        v = sorted(v)
        for label, q in LABELS:
            points[(kn, m, label)] = v[min(len(v) - 1, int(q * len(v)))]
    del values
    best = {}  # point -> (distance, id)
    for s in read_scans(scans):
        for b in s["bodies"]:
            if b["port"]:
                continue
            for m in MEASURES:
                if m not in b:
                    continue
                for label, _ in LABELS:
                    key = (b["kn"], m, label)
                    d = abs(b[m] - points[key])
                    if key not in best or d < best[key][0]:
                        best[key] = (d, s["id"])
    for (kn, m, label), (_, i) in best.items():
        providers.setdefault("%s %s %s" % (kn, m, label), set()).add(i)
    print("%d things to cover" % len(providers))
    covered, picked, why = set(), [], {}
    while len(covered) < len(providers):
        gain = {}
        for e, ids in providers.items():
            if e not in covered:
                w = 1.0 + 1.0 / len(ids)
                for i in ids:
                    gain[i] = gain.get(i, 0) + w
        top = max(gain, key=lambda i: (gain[i], -where[i]))
        picked.append(top)
        for e, ids in providers.items():
            if top in ids and e not in covered:
                covered.add(e)
                why.setdefault(top, []).append(e)
    result = []
    for s in read_scans(scans):
        if s["id"] in why:
            s["why"] = sorted(why[s["id"]])
            result.append(s)
    result.sort(key=lambda s: picked.index(s["id"]))
    with open(out, "w", encoding="utf-8") as f:
        json.dump({"note": "made by catalog.py build; sector x,y are relative to Sol (the game's own numbering)",
                   "systems": result}, f, ensure_ascii=False, separators=(",", ":"))
    print("%d systems picked -> %s" % (len(result), out))


def load():
    return json.load(open(CATALOG, encoding="utf-8"))["systems"]


OPS = {">": lambda a, b: a > b, "<": lambda a, b: a < b}


def matches(words):
    """(system, body) for each body that fits the search words"""
    conds, text = [], []
    for w in words:
        m = re.match(r"^(mass|radius|temp|au)([<>])(-?[0-9.]+)$", w)
        if m:
            conds.append((m.group(1), m.group(2), float(m.group(3))))
        else:
            text.append(w.lower())
    needle = " ".join(text)
    for s in load():
        bodies = {b["i"]: b for b in s["bodies"]}
        for b in s["bodies"]:
            parent = bodies.get(b["p"])
            desc = ("%s %s %s %s" % (b["kn"], b["name"], ("of " + parent["kn"]) if parent else "", s["star"])).lower()
            if needle and needle.replace("-", " ") not in desc and not all(t in desc for t in needle.split()):
                continue
            if all(m in b and OPS[o](b[m], v) for m, o, v in conds):
                yield s, b


def find(words):
    n = 0
    for s, b in matches(words):
        n += 1
        extra = " ".join("%s=%s" % (k, b[k]) for k in ("mass", "radius", "temp") if k in b)
        print("%-18s sector %d,%d #%d  %-24s %s  [%s]" % (s["name"], s["sx"], s["sy"], s["n"], b["name"], b["kn"], extra))
    print("%d matches" % n)


def go(args):
    km, pick, run = None, 0, None
    words = []
    while args:
        a = args.pop(0)
        if a == "--km":
            km = float(args.pop(0))
        elif a == "--pick":
            pick = int(args.pop(0))
        elif a == "--run":
            run = args.pop(0)
        else:
            words.append(a)
    found = list(matches(words))
    if len(found) <= pick:
        sys.exit("%d matches; no number %d" % (len(found), pick))
    s, b = found[pick]
    if km is None:
        km = 3.0 if b["port"] else 1000.0 if b["kn"].endswith("star") else 100.0
    if ";" in b["name"]:
        sys.exit("%s has a ; in its name, which the script language cannot say" % b["name"])
    cmd = "new 1; waitgame; wait 1; enter %08x; goto %s %g; wait 1; print" % (s["id"], b["name"], km)
    print("%s (sector %d,%d): %s, %s" % (s["name"], s["sx"], s["sy"], b["name"], b["kn"]))
    if run:
        os.execv(run, [run, "--do", cmd])
    print('--do "%s"' % cmd)


def kinds():
    k = {}
    for s in load():
        for b in s["bodies"]:
            r = k.setdefault((b["k"], b["kn"]), {"n": 0})
            r["n"] += 1
            for m in ("mass", "radius", "temp"):
                if m in b:
                    lo, hi = r.get(m, (b[m], b[m]))
                    r[m] = (min(lo, b[m]), max(hi, b[m]))
    for (num, name), r in sorted(k.items()):
        print("%3d %-60s %s" % (num, name, " ".join("%s %s..%s" % (m, r[m][0], r[m][1]) for m in ("mass", "radius", "temp") if m in r)))


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[1] == "build":
        build(sys.argv[2], sys.argv[3:])
    elif len(sys.argv) >= 2 and sys.argv[1] == "find":
        find(sys.argv[2:])
    elif len(sys.argv) >= 2 and sys.argv[1] == "go":
        go(sys.argv[2:])
    elif len(sys.argv) >= 2 and sys.argv[1] == "kinds":
        kinds()
    else:
        print(__doc__)
