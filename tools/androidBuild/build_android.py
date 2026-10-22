#!/usr/bin/env python3
"""
build_android.py - build GLFrontier as an Android APK, without Gradle.

    python tools/androidBuild/build_android.py [--variant original|modded] [--abi arm64-v8a ...]
                                    [--install] [--run] [--clean]

Needs only Python 3 and an internet connection the first time. Everything the
build needs that isn't on the machine is downloaded into a cache folder
OUTSIDE the repository, so the repo stays small:

    %LOCALAPPDATA%\\glfrontier-android     (Windows)
    ~/.cache/glfrontier-android           (Linux / macOS)
    or $GLF_ANDROID_CACHE

    jdk/        a JDK 17, if no JDK 17+ is found (JAVA_HOME / PATH)
    sdk/        Android command line tools, platform, build-tools, NDK and
                CMake, if no complete SDK is found (ANDROID_HOME / ANDROID_SDK_ROOT)
    debug.keystore   the signing key (~/.android/debug.keystore if it exists)

The steps, each with the SDK's own tools:
    1. cmake + NDK   -> libGLFrontier.so per ABI (SDL, PhysFS, ImGui linked in)
    2. javac + d8    -> classes.dex (SDL's Java glue + tools/androidBuild/java)
    3. aapt2         -> APK with the manifest and tools/androidBuild/res
    4. zipalign + apksigner -> build-android/GLFrontier[-original].apk
                               (-<abis> added unless it's the arm64 phone build)

Game data is compiled into the .so (no assets). Saves go to the app's
internal storage.
"""
import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ANDROID_DIR = Path(__file__).resolve().parent
OUT_DIR = ROOT / "build-android"

APP_VERSION_CODE = "1"
APP_VERSION_NAME = "1.0"
MIN_SDK = 21
TARGET_SDK = 34

# What gets installed into a private SDK
SDK_PLATFORM = f"platforms;android-{TARGET_SDK}"
SDK_BUILD_TOOLS = "build-tools;34.0.0"
SDK_NDK = "ndk;27.2.12479018"  # r27 LTS
SDK_CMAKE = "cmake;3.22.1"     # only for its ninja
CMDLINE_TOOLS_BUILD = "13114758"

IS_WIN = os.name == "nt"
EXE = ".exe" if IS_WIN else ""
BAT = ".bat" if IS_WIN else ""

def log(msg):
    print(f"=== {msg}", flush=True)

def die(msg):
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)

def run(cmd, **kw):
    cmd = [str(c) for c in cmd]
    print("  $ " + " ".join(cmd), flush=True)
    r = subprocess.run(cmd, **kw)
    if r.returncode != 0:
        die(f"{Path(cmd[0]).name} failed ({r.returncode})")
    return r

def cache_dir():
    if os.environ.get("GLF_ANDROID_CACHE"):
        d = Path(os.environ["GLF_ANDROID_CACHE"])
    elif IS_WIN:
        d = Path(os.environ.get("LOCALAPPDATA", Path.home())) / "glfrontier-android"
    else:
        d = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "glfrontier-android"
    d.mkdir(parents=True, exist_ok=True)
    return d

def download(url, dest):
    log(f"downloading {url}")
    tmp = dest.with_suffix(dest.suffix + ".part")
    # (some hosts refuse Python's default User-Agent)
    req = urllib.request.Request(url, headers={"User-Agent": "glfrontier-android-build"})
    with urllib.request.urlopen(req) as r, open(tmp, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        done = 0
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)
            done += len(chunk)
            if total:
                print(f"\r  {done >> 20} / {total >> 20} MB", end="", flush=True)
    print()
    tmp.replace(dest)

def extract(archive, dest):
    """Unpacks a zip or tar.gz, keeping the executable bits of zip entries."""
    if str(archive).endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            for info in z.infolist():
                path = z.extract(info, dest)
                mode = info.external_attr >> 16
                if mode and not IS_WIN:
                    os.chmod(path, mode & 0o777 or 0o644)
    else:
        with tarfile.open(archive) as t:
            t.extractall(dest)

def host_tag():
    s = platform.system()
    if s == "Windows":
        return "windows-x86_64"
    if s == "Darwin":
        return "darwin-x86_64"  # NDK ships universal binaries under this name
    return "linux-x86_64"

# --------------------------------------------------------------------------
# JDK

def java_major(java):
    try:
        out = subprocess.run([str(java), "-version"], capture_output=True, text=True).stderr
    except OSError:
        return 0
    m = re.search(r'version "(\d+)(?:\.(\d+))?', out)
    if not m:
        return 0
    major = int(m.group(1))
    return int(m.group(2)) if major == 1 else major

def find_jdk(cache):
    """A JDK home (17+) with javac, jar and keytool; downloaded if needed."""
    candidates = []
    if os.environ.get("JAVA_HOME"):
        candidates.append(Path(os.environ["JAVA_HOME"]))
    javac = shutil.which("javac")
    if javac:
        candidates.append(Path(javac).resolve().parent.parent)
    candidates += sorted((cache / "jdk").glob("*"), reverse=True)
    for home in candidates:
        if (home / "bin" / f"javac{EXE}").exists() and java_major(home / "bin" / f"java{EXE}") >= 17:
            return home

    arch = "aarch64" if platform.machine().lower() in ("arm64", "aarch64") else "x64"
    os_name = {"Windows": "windows", "Darwin": "mac"}.get(platform.system(), "linux")
    ext = "zip" if IS_WIN else "tar.gz"
    url = f"https://api.adoptium.net/v3/binary/latest/17/ga/{os_name}/{arch}/jdk/hotspot/normal/eclipse"
    archive = cache / f"jdk17.{ext}"
    download(url, archive)
    log("unpacking the JDK")
    extract(archive, cache / "jdk")
    archive.unlink()
    for home in (cache / "jdk").glob("*"):
        if (home / "Contents" / "Home").exists():  # macOS layout
            home = home / "Contents" / "Home"
        if (home / "bin" / f"javac{EXE}").exists():
            return home
    die("the downloaded JDK has no javac")

# --------------------------------------------------------------------------
# Android SDK

def newest(dirs, minimum=None):
    def key(p):
        return [int(x) for x in re.findall(r"\d+", p.name)]
    dirs = [d for d in dirs if d.is_dir() and (minimum is None or key(d) >= minimum)]
    return max(dirs, key=key) if dirs else None

class Sdk:
    def __init__(self, root):
        self.root = Path(root)
        self.platform = self.root / "platforms" / f"android-{TARGET_SDK}" / "android.jar"
        self.build_tools = newest((self.root / "build-tools").glob("*"), [34])
        self.ndk = newest((self.root / "ndk").glob("*"), [26])
        cmake = newest((self.root / "cmake").glob("*"))
        self.ninja = cmake / "bin" / f"ninja{EXE}" if cmake else None

    def missing(self):
        m = []
        if not self.platform.exists():
            m.append(SDK_PLATFORM)
        if not self.build_tools:
            m.append(SDK_BUILD_TOOLS)
        if not self.ndk:
            m.append(SDK_NDK)
        if not (self.ninja and self.ninja.exists()) and not shutil.which("ninja"):
            m.append(SDK_CMAKE)
        return m

    def tool(self, name):
        for suffix in (EXE, BAT, ""):
            p = self.build_tools / f"{name}{suffix}"
            if p.exists():
                return p
        die(f"{name} not found in {self.build_tools}")

    def adb(self):
        p = self.root / "platform-tools" / f"adb{EXE}"
        return p if p.exists() else shutil.which("adb")

def sdkmanager(sdk_root, jdk, packages):
    exe = sdk_root / "cmdline-tools" / "latest" / "bin" / f"sdkmanager{BAT}"
    if not exe.exists():
        os_name = {"Windows": "win", "Darwin": "mac"}.get(platform.system(), "linux")
        archive = sdk_root / "cmdline-tools.zip"
        sdk_root.mkdir(parents=True, exist_ok=True)
        download(f"https://dl.google.com/android/repository/commandlinetools-{os_name}-{CMDLINE_TOOLS_BUILD}_latest.zip", archive)
        tmp = sdk_root / "cmdline-tools" / "_tmp"
        shutil.rmtree(tmp, ignore_errors=True)
        extract(archive, tmp)
        (tmp / "cmdline-tools").rename(sdk_root / "cmdline-tools" / "latest")
        shutil.rmtree(tmp, ignore_errors=True)
        archive.unlink()
    env = dict(os.environ, JAVA_HOME=str(jdk))
    log("accepting SDK licenses")
    subprocess.run([str(exe), f"--sdk_root={sdk_root}", "--licenses"], input="y\n" * 20,
                   text=True, env=env, capture_output=True)
    log("installing " + ", ".join(packages))
    run([exe, f"--sdk_root={sdk_root}"] + packages, env=env)

def find_sdk(cache, jdk, need_adb):
    for var in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        root = os.environ.get(var)
        if root and Path(root).is_dir():
            sdk = Sdk(root)
            if not sdk.missing() and (not need_adb or sdk.adb()):
                return sdk
            log(f"{var}={root} lacks {', '.join(sdk.missing()) or 'adb'}: using a private SDK")
    root = cache / "sdk"
    sdk = Sdk(root)
    missing = sdk.missing()
    if need_adb and not sdk.adb():
        missing.append("platform-tools")
    if missing:
        sdkmanager(root, jdk, missing)
        sdk = Sdk(root)
        if sdk.missing():
            die("SDK still lacks " + ", ".join(sdk.missing()))
    return sdk

# --------------------------------------------------------------------------
# Build steps

def find_cmake(sdk):
    exe = shutil.which("cmake")
    if exe:
        out = subprocess.run([exe, "--version"], capture_output=True, text=True).stdout
        m = re.search(r"(\d+)\.(\d+)", out)
        if m and (int(m.group(1)), int(m.group(2))) >= (3, 22):
            return exe
    cmake = newest((sdk.root / "cmake").glob("*"))
    if cmake and (cmake / "bin" / f"cmake{EXE}").exists():
        return cmake / "bin" / f"cmake{EXE}"
    die("CMake 3.22+ not found")

def build_native(sdk, abi, modded, jobs):
    bdir = OUT_DIR / ("modded" if modded else "original") / abi
    ninja = sdk.ninja if sdk.ninja and sdk.ninja.exists() else shutil.which("ninja")
    cmake = find_cmake(sdk)
    log(f"native code: {abi} (NDK {sdk.ndk.name})")
    if not (bdir / "CMakeCache.txt").exists():
        run([cmake, "-S", ROOT, "-B", bdir, "-G", "Ninja",
             f"-DCMAKE_MAKE_PROGRAM={ninja}",
             f"-DCMAKE_TOOLCHAIN_FILE={sdk.ndk / 'build' / 'cmake' / 'android.toolchain.cmake'}",
             f"-DANDROID_ABI={abi}",
             f"-DANDROID_PLATFORM=android-{MIN_SDK}",
             "-DANDROID_STL=c++_static",
             "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON",  # 16 KB pages (Android 15+)
             "-DCMAKE_BUILD_TYPE=Release",
             f"-DGLF_MODDED_FE2={'ON' if modded else 'OFF'}"])
    run([cmake, "--build", bdir, "--parallel", str(jobs)])
    so = bdir / "libGLFrontier.so"
    if not so.exists():
        die(f"{so} was not built")
    return bdir, so

def build_dex(sdk, jdk, sdl_src, work):
    log("Java: SDL glue + tools/androidBuild/java -> classes.dex")
    classes = work / "classes"
    shutil.rmtree(classes, ignore_errors=True)
    classes.mkdir(parents=True)
    sources = sorted((sdl_src / "android-project" / "app" / "src" / "main" / "java").rglob("*.java"))
    sources += sorted((ANDROID_DIR / "java").rglob("*.java"))
    argfile = work / "sources.txt"
    argfile.write_text("\n".join(f'"{s.as_posix()}"' for s in sources))
    # core-lambda-stubs: android.jar has no LambdaMetafactory (d8 desugars lambdas)
    bootclasspath = os.pathsep.join([str(sdk.platform), str(sdk.build_tools / "core-lambda-stubs.jar")])
    run([jdk / "bin" / f"javac{EXE}", "-nowarn", "-Xlint:-options", "-encoding", "UTF-8",
         "-source", "1.8", "-target", "1.8", "-bootclasspath", bootclasspath,
         "-d", classes, f"@{argfile}"])
    jar = work / "classes.jar"
    with zipfile.ZipFile(jar, "w") as z:
        for f in classes.rglob("*.class"):
            z.write(f, f.relative_to(classes).as_posix())
    dex_dir = work / "dex"
    shutil.rmtree(dex_dir, ignore_errors=True)
    dex_dir.mkdir()
    env = dict(os.environ, JAVA_HOME=str(jdk), PATH=str(jdk / "bin") + os.pathsep + os.environ["PATH"])
    run([sdk.tool("d8"), "--release", "--min-api", MIN_SDK, "--lib", sdk.platform,
         "--output", dex_dir, jar], env=env)
    return dex_dir / "classes.dex"

def debug_keystore(jdk, cache):
    home = Path.home() / ".android" / "debug.keystore"
    if home.exists():
        return home
    ks = cache / "debug.keystore"
    if not ks.exists():
        log("making a debug signing key")
        run([jdk / "bin" / f"keytool{EXE}", "-genkeypair", "-keystore", ks,
             "-storepass", "android", "-keypass", "android", "-alias", "androiddebugkey",
             "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000",
             "-dname", "CN=Android Debug,O=Android,C=US"])
    return ks

def package(sdk, jdk, cache, libs, dex, work, apk):
    log("packaging the APK")
    aapt2 = sdk.tool("aapt2")
    res_zip = work / "res.zip"
    run([aapt2, "compile", "--dir", ANDROID_DIR / "res", "-o", res_zip])
    unsigned = work / "unsigned.apk"
    run([aapt2, "link", "-o", unsigned, "-I", sdk.platform,
         "--manifest", ANDROID_DIR / "AndroidManifest.xml",
         "--min-sdk-version", MIN_SDK, "--target-sdk-version", TARGET_SDK,
         "--version-code", APP_VERSION_CODE, "--version-name", APP_VERSION_NAME, res_zip])

    # native libraries, stripped: debug info makes up most of an unstripped .so
    strip = sdk.ndk / "toolchains" / "llvm" / "prebuilt" / host_tag() / "bin" / f"llvm-strip{EXE}"
    with zipfile.ZipFile(unsigned, "a", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.write(dex, "classes.dex")
        for abi, so in libs.items():
            stripped = work / f"{abi}.so"
            run([strip, "--strip-unneeded", "-o", stripped, so])
            z.write(stripped, f"lib/{abi}/libGLFrontier.so")

    aligned = work / "aligned.apk"
    run([sdk.tool("zipalign"), "-f", "-p", "4", unsigned, aligned])
    env = dict(os.environ, JAVA_HOME=str(jdk), PATH=str(jdk / "bin") + os.pathsep + os.environ["PATH"])
    run([sdk.tool("apksigner"), "sign", "--ks", debug_keystore(jdk, cache),
         "--ks-pass", "pass:android", "--key-pass", "pass:android",
         "--out", apk, aligned], env=env)
    idsig = apk.with_suffix(".apk.idsig")
    if idsig.exists():
        idsig.unlink()

def main():
    ap = argparse.ArgumentParser(description="Build GLFrontier for Android (no Gradle)")
    ap.add_argument("--variant", choices=["modded", "original"], default="original")
    ap.add_argument("--abi", nargs="+", default=["arm64-v8a"],
                    choices=["arm64-v8a", "armeabi-v7a", "x86_64", "x86"],
                    help="ABIs to include (default arm64-v8a; add x86_64 for the emulator)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--install", action="store_true", help="adb install the APK")
    ap.add_argument("--run", action="store_true", help="install and start it")
    ap.add_argument("--clean", action="store_true", help="delete build-android first")
    args = ap.parse_args()
    if args.run:
        args.install = True

    if args.clean:
        shutil.rmtree(OUT_DIR, ignore_errors=True)
    cache = cache_dir()
    log(f"tool cache: {cache}")
    jdk = find_jdk(cache)
    log(f"JDK: {jdk}")
    sdk = find_sdk(cache, jdk, args.install)
    log(f"SDK: {sdk.root} (build-tools {sdk.build_tools.name}, NDK {sdk.ndk.name})")

    modded = args.variant == "modded"
    libs = {}
    sdl_src = None
    for abi in args.abi:
        bdir, so = build_native(sdk, abi, modded, args.jobs)
        libs[abi] = so
        sdl_src = bdir / "_deps" / "sdl2-src"

    work = OUT_DIR / args.variant / "apk"
    work.mkdir(parents=True, exist_ok=True)
    dex = build_dex(sdk, jdk, sdl_src, work)
    # GLFrontier.apk is always the phone build (arm64-v8a); any other ABI set
    # is named after its ABIs, e.g. GLFrontier-x86_64.apk for the emulator,
    # so an emulator build never replaces the one that goes on a phone
    name = "GLFrontier-modded" if modded else "GLFrontier"
    if args.abi != ["arm64-v8a"]:
        name += "-" + "-".join(args.abi)
    apk = OUT_DIR / f"{name}.apk"
    package(sdk, jdk, cache, libs, dex, work, apk)
    log(f"built {apk} ({apk.stat().st_size / (1 << 20):.1f} MB)")

    if args.install:
        adb = sdk.adb()
        if not adb:
            die("adb not found")
        run([adb, "install", "-r", apk])
        if args.run:
            run([adb, "shell", "am", "start", "-n",
                 "org.glfrontier.extended/org.glfrontier.GLFrontierActivity"])

if __name__ == "__main__":
    main()
