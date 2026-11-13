"""Find the user's own Kerbal Space Program install (shared by ksp_textures.py and ksp_ksc.py).

find_data(path) -> the Data folder (KSP_Data / KSP_x64_Data / KSP.app/.../Data) of an install, or None.
It accepts the install folder, the Data folder itself, or a folder holding "Kerbal Space Program".
candidates() lists the places searched when no path is given: $KSP_DIR; every Steam library (read from
Steam's libraryfolders.vdf, and on Windows from the registry); the usual Steam, GOG and Program Files
folders, on every Windows drive letter; ~/ksp and ~/GOG Games; macOS Steam.
"""
import glob, os, re, string, sys

NAME = "Kerbal Space Program"
SUBS = ("KSP_x64_Data", "KSP_Data", "KSP.app/Contents/Resources/Data", "KSP_linux/KSP_Data", "KSP_linux/KSP_x64_Data")


def _is_data(d):
    return bool(glob.glob(os.path.join(d, "sharedassets*.assets")))


def find_data(root):
    root = os.path.expanduser(root)
    for d in [root] + [os.path.join(root, s) for s in SUBS] + [os.path.join(root, NAME, s) for s in SUBS]:
        if os.path.isdir(d) and _is_data(d):
            return d
    return None


def _steam_roots():
    roots = []
    if os.name == "nt":
        try:
            import winreg
            for hive, key, val in ((winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", "SteamPath"),
                                   (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath"),
                                   (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Valve\Steam", "InstallPath")):
                try:
                    with winreg.OpenKey(hive, key) as k:
                        roots.append(winreg.QueryValueEx(k, val)[0])
                except OSError:
                    pass
        except ImportError:
            pass
        for pf in (os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles"), "C:/Program Files (x86)", "C:/Program Files"):
            if pf:
                roots.append(os.path.join(pf, "Steam"))
        for dl in string.ascii_uppercase[2:]:
            roots += [f"{dl}:/Steam", f"{dl}:/SteamLibrary", f"{dl}:/Games/Steam", f"{dl}:/Games/SteamLibrary"]
    else:
        roots += ["~/.steam/steam", "~/.steam/root", "~/.local/share/Steam", "~/Library/Application Support/Steam",
                  "~/.var/app/com.valvesoftware.Steam/.local/share/Steam"]
    return [os.path.expanduser(r) for r in roots]


def candidates():
    out = []
    if os.environ.get("KSP_DIR"):
        out.append(os.environ["KSP_DIR"])
    libs = []
    for s in _steam_roots():
        libs.append(s)
        vdf = os.path.join(s, "steamapps", "libraryfolders.vdf")  # extra libraries on other drives
        try:
            with open(vdf, encoding="utf-8", errors="replace") as f:
                libs += [p.replace("\\\\", "\\") for p in re.findall(r'"path"\s+"([^"]+)"', f.read())]
        except OSError:
            pass
    out += [os.path.join(l, "steamapps", "common", NAME) for l in libs]
    gog = ["~/GOG Games/" + NAME, "~/GOG Games/" + NAME + "/game", "~/ksp", "~/ksp/kerbalspaceprogram", "~/ksp/kerbalspaceprogram/KSP_linux"]
    if os.name == "nt":
        for dl in string.ascii_uppercase[2:]:
            gog += [f"{dl}:/GOG Games/{NAME}", f"{dl}:/Games/{NAME}", f"{dl}:/{NAME}", f"{dl}:/Program Files (x86)/GOG Galaxy/Games/{NAME}"]
    out += [os.path.expanduser(g) for g in gog]
    seen = set()
    return [c for c in out if not (c in seen or seen.add(c))]


def locate(path=None):
    """The Data folder for `path`, or the first install found in the usual places; exits with a message if none."""
    if path:
        d = find_data(path)
        if d:
            return d
        sys.exit(f"no KSP_Data / KSP_x64_Data with sharedassets*.assets in {path}")
    for c in candidates():
        d = find_data(c)
        if d:
            return d
    sys.exit("KSP install not found: give its folder as the first argument, or set KSP_DIR\n(the folder with KSP_x64_Data or KSP_Data; Steam: right-click the game > Manage > Browse local files)")
