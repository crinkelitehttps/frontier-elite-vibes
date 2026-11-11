#!/usr/bin/env python3
"""Dump the layout of KSP's Kerbal Space Center from YOUR OWN copy of Kerbal Space Program.

Nothing from KSP is shipped with this project. This script reads the space center buildings out of
your KSP install (the Unity asset files in KSP_Data) and writes their positions and sizes, so a
very simplified KSC (a few boxes per building) can be built for Kerbin. Keep the output for your own
use; do not redistribute the OBJ files.

    pip install UnityPy numpy
    python3 tools/ksp_ksc.py /path/to/KSP                      # print the layout
    python3 tools/ksp_ksc.py /path/to/KSP --json ksc.json      # write it as JSON
    python3 tools/ksp_ksc.py /path/to/KSP --obj ksc_obj        # plus one OBJ per facility (Blender)
    python3 tools/ksp_ksc.py /path/to/KSP --level 1            # the starting (unupgraded) buildings
    python3 tools/ksp_ksc.py /path/to/KSP --all                # list every part, decals and props too

Frame: metres from the KSC origin, x = east, y = up, z = north (Unity's axes; the launch pad is east
of the VAB, the runway north). Facility ground is at y = 24.8. OBJ files use x = east, y = up,
z = south (right-handed, so Blender's OBJ import shows them the right way round).

Each facility comes in up to three upgrade levels (--level 1-3, default 3 = fully upgraded). Parts
are marked "ground" (flat plates), "detail" (decals, baked shadow shells, props, lights) or
"building"; the simplified KSC should only need the buildings plus the runway and pad surfaces.
Without a path the usual Steam / GOG / ~/ksp locations are searched.
"""
import argparse, glob, json, os, re, sys

GUESSES = ["~/ksp/kerbalspaceprogram/KSP_linux", "~/.steam/steam/steamapps/common/Kerbal Space Program",
           "~/.local/share/Steam/steamapps/common/Kerbal Space Program", "~/GOG Games/Kerbal Space Program/game",
           "C:/Program Files (x86)/Steam/steamapps/common/Kerbal Space Program",
           "C:/GOG Games/Kerbal Space Program", "~/Library/Application Support/Steam/steamapps/common/Kerbal Space Program"]

# KSC child node -> prefab name per upgrade level 1..3 (KSP's own numbering skips some)
FACILITIES = {
    "VehicleAssemblyBuilding": ["VABLevel1", "VABLevel2", "VABLevel4"],
    "SpaceplaneHangar": ["SPHLevel1", "SPHLevel2", "SPHLevel3"],
    "Runway": ["RunwayLevel1", "RunwayLevel2", "RunwayLevel3"],
    "LaunchPad": ["LaunchPadLevel1", "LaunchPadLevel2", "LaunchPadLevel4"],
    "TrackingStation": ["TrackingStationLevel1", "TrackingStationLevel2", "TrackingStationLevel3"],
    "AstronautComplex": ["AstronautComplexLevel1", "AstronautComplexLevel2", "AstronautComplexLevel3"],
    "MissionControl": ["MissionControlLevel1", "MissionControlLevel2", "MissionControlLevel3"],
    "Administration": ["AdminLevel1", "AdminLevel2", "AdminLevel3"],
    "ResearchAndDevelopment": ["RnDLevel1", "RnDLevel3", "RnDLevel4"],
    "FlagPole": ["FlagPole_lvl1", "FlagPole_lvl2", "FlagPole_lvl3"],
    "Crawlerway": ["CrawlerwayLevel1", "CrawlerwayLevel2", "CrawlerwayLevel4"],
}
DETAIL = re.compile(r"decal|_ao$|_ao_|detail|props|lights|collider|rock\d|flare", re.I)


def find_data(root):
    for sub in ("KSP_Data", "KSP_x64_Data", "KSP.app/Contents/Resources/Data"):
        d = os.path.join(root, sub)
        if glob.glob(os.path.join(d, "sharedassets*.assets")):
            return d
    return None


class Scene:
    """Transforms, game objects and meshes of one Unity asset file."""

    def __init__(self, path):
        import UnityPy
        self.env = UnityPy.load(path)
        self.go, self.tr, self.mf, self.mesh_obj = {}, {}, {}, {}
        for o in self.env.objects:
            t = o.type.name
            if t not in ("GameObject", "Transform", "MeshFilter", "Mesh"):
                continue
            if t == "Mesh":
                self.mesh_obj[o.path_id] = o
                continue
            try:
                d = o.read_typetree()
            except Exception:
                continue
            if t == "GameObject":
                self.go[o.path_id] = d
            elif t == "Transform":
                self.tr[o.path_id] = d
            else:
                self.mf[d["m_GameObject"]["m_PathID"]] = d["m_Mesh"]["m_PathID"]
        self.kids, self.roots = {}, {}
        for k, t in self.tr.items():
            f = t["m_Father"]["m_PathID"]
            if f in self.tr:
                self.kids.setdefault(f, []).append(k)
            else:
                self.roots.setdefault(self.name(k), k)
        self.mesh_cache = {}

    def gid(self, t):
        return self.tr[t]["m_GameObject"]["m_PathID"]

    def name(self, t):
        return self.go.get(self.gid(t), {}).get("m_Name", "?")

    def active(self, t):
        return self.go.get(self.gid(t), {}).get("m_IsActive", 1)

    def matrix(self, t):
        import numpy as np
        d = self.tr[t]
        p, q, s = d["m_LocalPosition"], d["m_LocalRotation"], d["m_LocalScale"]
        x, y, z, w = q["x"], q["y"], q["z"], q["w"]
        r = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                      [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                      [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
        m = np.eye(4)
        m[:3, :3] = r @ np.diag([s["x"], s["y"], s["z"]])
        m[:3, 3] = [p["x"], p["y"], p["z"]]
        return m

    def mesh(self, pid):
        """(vertices Nx3 in Unity axes, faces as index lists in OBJ winding with z flipped)."""
        if pid not in self.mesh_cache:
            import numpy as np
            m = self.mesh_obj[pid].read()
            vs, fs = [], []
            for line in m.export().splitlines():
                if line.startswith("v "):
                    x, y, z = map(float, line.split()[1:4])
                    vs.append((-x, y, z))  # UnityPy's OBJ export negates x
                elif line.startswith("f "):
                    fs.append([int(p.split("/")[0]) - 1 for p in line.split()[1:]])
            self.mesh_cache[pid] = (np.array(vs).reshape(-1, 3), fs, m.m_Name)
        return self.mesh_cache[pid]

    def parts(self, top, base):
        """Every mesh under transform `top` (its own transform replaced by `base`)."""
        import numpy as np
        out = []

        def walk(t, m, depth, path):
            m = m @ (self.matrix(t) if depth else np.eye(4))
            if not self.active(t):
                return
            pid = self.mf.get(self.gid(t))
            if pid in self.mesh_obj:
                vs, fs, mname = self.mesh(pid)
                if len(vs):
                    w = vs @ m[:3, :3].T + m[:3, 3]
                    out.append({"node": path + "/" + self.name(t), "mesh": mname, "verts": w, "faces": fs})
            for c in self.kids.get(t, []):
                walk(c, m, depth + 1, path + "/" + self.name(t))

        walk(top, base, 0, "")
        return out


def classify(p, lo, hi):
    size = hi - lo
    if DETAIL.search(p["mesh"]) or DETAIL.search(p["node"].rsplit("/", 1)[-1]):
        return "detail"
    if size[1] < 6 and size[0] * size[2] > 20000:
        return "ground"
    return "building"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("ksp", nargs="?", help="KSP folder (the one with KSP_Data)")
    ap.add_argument("--level", type=int, default=3, choices=(1, 2, 3), help="upgrade level (default 3)")
    ap.add_argument("--json", help="write the layout to this JSON file")
    ap.add_argument("--obj", help="write one OBJ per facility into this folder")
    ap.add_argument("--all", action="store_true", help="print detail parts and tiny parts too")
    ap.add_argument("--file", help="asset file holding the KSC (default: search sharedassets*.assets)")
    a = ap.parse_args()
    try:
        import numpy  # noqa: F401
        import UnityPy  # noqa: F401
    except ImportError:
        sys.exit("needs: pip install UnityPy numpy")

    roots = [a.ksp] if a.ksp else [os.path.expanduser(g) for g in GUESSES]
    data = next((d for d in map(find_data, roots) if d), None)
    if not data:
        sys.exit("KSP_Data not found; give the KSP folder as the first argument")

    if a.file:
        files = [a.file if os.path.isabs(a.file) else os.path.join(data, a.file)]
    else:  # the KSC prefabs are in the biggest asset file (sharedassets9 in KSP 1.12)
        files = sorted(glob.glob(os.path.join(data, "sharedassets*.assets")), key=os.path.getsize, reverse=True)
    scene = None
    for f in files:
        s = Scene(f)
        if "KSC" in s.roots and "RunwayLevel1" in s.roots:
            scene = s
            print("KSC found in", os.path.basename(f), file=sys.stderr)
            break
    if not scene:
        sys.exit("no asset file with the KSC prefabs found (try --file)")

    import numpy as np
    ksc = scene.roots["KSC"]
    nodes = {scene.name(c): c for c in scene.kids[ksc]}
    layout = {"frame": "metres from KSC origin; x east, y up, z north; facility ground y = 24.8",
              "level": a.level, "facilities": []}
    if a.obj:
        os.makedirs(a.obj, exist_ok=True)

    jobs = [(fac, levels[a.level - 1]) for fac, levels in FACILITIES.items()]
    jobs.append(("Grounds", None))  # the paths between facilities, placed directly under KSC
    for fac, prefab in jobs:
        node = nodes.get(fac)
        if node is None:
            print("missing KSC node", fac, file=sys.stderr)
            continue
        m_node = scene.matrix(node)
        if prefab is None:
            parts = []
            for c in scene.kids.get(node, []):
                parts += scene.parts(c, m_node @ scene.matrix(c))
        else:
            if prefab not in scene.roots:
                print("missing prefab", prefab, file=sys.stderr)
                continue
            facility = scene.kids[node][0] if scene.kids.get(node) else None
            base = m_node @ (scene.matrix(facility) if facility else np.eye(4))
            parts = scene.parts(scene.roots[prefab], base)
        q = scene.tr[node]["m_LocalRotation"]
        yaw = round(float(np.degrees(2 * np.arctan2(q["y"], q["w"]))) % 360, 1)
        entry = {"name": fac, "prefab": prefab, "yaw_deg": yaw,
                 "position": [round(float(v), 1) for v in m_node[:3, 3]], "parts": []}
        print(f"\n{fac} ({prefab or 'KSC child'}) at {entry['position']} yaw {yaw}")
        for p in parts:
            lo, hi = p["verts"].min(0), p["verts"].max(0)
            kind = classify(p, lo, hi)
            size, ctr = hi - lo, (hi + lo) / 2
            rec = {"mesh": p["mesh"], "node": p["node"], "kind": kind,
                   "centre": [round(float(v), 1) for v in ctr], "size": [round(float(v), 1) for v in size],
                   "min": [round(float(v), 1) for v in lo], "max": [round(float(v), 1) for v in hi],
                   "triangles": sum(max(0, len(f) - 2) for f in p["faces"])}
            entry["parts"].append(rec)
        for rec in sorted(entry["parts"], key=lambda r: -r["size"][1]):
            if not a.all and (rec["kind"] == "detail" or max(rec["size"]) < 8):
                continue
            s, c = rec["size"], rec["centre"]
            print(f"  {rec['kind']:8s} {rec['mesh'][:40]:40s} size {s[0]:7.1f} {s[1]:6.1f} {s[2]:7.1f}"
                  f"  centre {c[0]:7.1f} {c[1]:6.1f} {c[2]:7.1f}  tris {rec['triangles']}")
        layout["facilities"].append(entry)
        if a.obj and parts:
            with open(os.path.join(a.obj, fac + ".obj"), "w") as f:
                f.write(f"# {fac} ({prefab}) from your KSP install - personal use only\n")
                base_i = 1
                for p in parts:
                    f.write(f"o {p['mesh']}\n")
                    for x, y, z in p["verts"]:
                        f.write(f"v {x:.3f} {y:.3f} {-z:.3f}\n")
                    for face in p["faces"]:
                        f.write("f " + " ".join(str(i + base_i) for i in face) + "\n")
                    base_i += len(p["verts"])

    if a.json:
        with open(a.json, "w") as f:
            json.dump(layout, f, indent=1)
        print("\nwrote", a.json, file=sys.stderr)


if __name__ == "__main__":
    main()
