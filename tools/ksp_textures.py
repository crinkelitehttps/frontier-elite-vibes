#!/usr/bin/env python3
"""Make small planet surface images for systems/kerbol.json from YOUR OWN copy of Kerbal Space Program.

Nothing from KSP is shipped with this project: this script reads the planet colour maps out of your
KSP install (the Unity asset files in KSP_Data), shrinks them and writes PNGs that the game's
`look.surface` option draws on the planets. Keep the output for your own use; do not redistribute it.

    pip install UnityPy Pillow
    python3 tools/ksp_textures.py /path/to/KSP --out systems            # all bodies
    python3 tools/ksp_textures.py /path/to/KSP --out systems --size 512 --colours 32 duna mun
    python3 tools/ksp_textures.py /path/to/KSP --list                    # texture names found

Options: --size W  output width (height W/2, default 1024); --colours N  reduce to N colours (0 = keep
all, default 64); --shift DEG  turn the map east by DEG degrees (to line features up in the game);
--sharpen  keep more fine detail.  Output files are ksp_<body>.png (e.g. ksp_kerbin.png).
Without a path the usual Steam / GOG / ~/ksp locations are searched.
"""
import argparse, glob, os, re, sys

# body -> name of the colour map texture inside KSP's assets (guessed from looking at them)
BODIES = {
    "kerbin": "KerbinScaledSpace300", "mun": "NewMunSurfaceMapDiffuse", "minmus": "NewMunSurfaceMap00",
    "moho": "moho01", "eve": "Eve2_00", "gilly": "evemoon100", "duna": "Duna [Albedo]",
    "ike": "desertplanetmoon00", "dres": "dwarfplanet100", "laythe": "newoceanmoon00",
    "vall": "gp1icemoon00", "tylo": "rockyMoon00", "bop": "gp1minormoon100", "pol": "gp1minormoon200",
    "eeloo": "snowydwarfplanet00",
}
GUESSES = ["~/ksp/kerbalspaceprogram/KSP_linux", "~/.steam/steam/steamapps/common/Kerbal Space Program",
           "~/.local/share/Steam/steamapps/common/Kerbal Space Program", "~/GOG Games/Kerbal Space Program/game",
           "C:/Program Files (x86)/Steam/steamapps/common/Kerbal Space Program",
           "C:/GOG Games/Kerbal Space Program", "~/Library/Application Support/Steam/steamapps/common/Kerbal Space Program"]

def find_data(root):
    for sub in ("KSP_Data", "KSP_x64_Data", "KSP.app/Contents/Resources/Data"):
        d = os.path.join(root, sub)
        if glob.glob(os.path.join(d, "sharedassets*.assets")):
            return d
    return None

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ksp", nargs="?", help="KSP install folder (the one with KSP_Data)")
    ap.add_argument("bodies", nargs="*", help="bodies to make (default: all)")
    ap.add_argument("--out", default="systems"); ap.add_argument("--size", type=int, default=1024)
    ap.add_argument("--colours", type=int, default=64); ap.add_argument("--shift", type=float, default=0)
    ap.add_argument("--sharpen", action="store_true"); ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    root = a.ksp
    if root and root.lower() in BODIES:      # path left out: first word is a body
        a.bodies.insert(0, root); root = None
    data = find_data(os.path.expanduser(root)) if root else next(
        filter(None, (find_data(os.path.expanduser(g)) for g in GUESSES)), None)
    if not data:
        sys.exit("KSP_Data not found: give the KSP install folder as the first argument")
    try:
        import UnityPy
        from PIL import Image
    except ImportError:
        sys.exit("needs:  pip install UnityPy Pillow")
    want = {BODIES[b]: b for b in (a.bodies or BODIES)} if not a.list else None
    if a.bodies and any(b not in BODIES for b in a.bodies):
        sys.exit("unknown body; known: " + " ".join(BODIES))
    print("reading", data); os.makedirs(a.out, exist_ok=True); done = set()
    for f in sorted(glob.glob(os.path.join(data, "*.assets"))):
        for o in UnityPy.load(f).objects:
            if o.type.name != "Texture2D": continue
            try: t = o.read()
            except Exception: continue
            if a.list:
                if t.m_Width == 2 * t.m_Height and t.m_Width >= 1024: print(f"{t.m_Name}  {t.m_Width}x{t.m_Height}")
                continue
            body = want.get(t.m_Name)
            if not body or body in done: continue
            im = t.image.convert("RGB")
            w = a.size; im = im.resize((w, w // 2), Image.LANCZOS if not a.sharpen else Image.BICUBIC)
            if a.shift:
                px = int(round(a.shift / 360 * w)) % w
                import numpy; im = Image.fromarray(numpy.roll(numpy.asarray(im), px, axis=1))
            if a.colours > 0:
                im = im.quantize(a.colours, method=Image.MEDIANCUT, dither=Image.NONE).convert("RGB")
            p = os.path.join(a.out, f"ksp_{body}.png"); im.save(p, optimize=True); done.add(body)
            print(f"{body:8} <- {t.m_Name}  ->  {p}  ({os.path.getsize(p)//1024} KB)")
    if not a.list:
        for b in (a.bodies or BODIES):
            if b not in done: print("not found:", b, "(texture", BODIES[b] + ")")
        print('Use in a system file:  "looks": { "kerbin": { "surface": "ksp_kerbin.png" } }, body: "look": "kerbin"')

if __name__ == "__main__": main()
