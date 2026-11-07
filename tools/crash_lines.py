#!/usr/bin/env python3
"""crash_lines.py - turns the last crash in crash.log (src/crash_log.c) into
source lines, and frames in the game code into lines of fe2/fe2_modded.s
(or fe2/fe2.s) with the nearest 68k label.

    tools/crash_lines.py [crash.log] [--exe build-modded/GLFrontier] [--all]

Needs addr2line (binutils) and the SAME executable that crashed: a rebuild
moves every address (the script warns when the exe's mtime differs).
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)


def main():
    args = sys.argv[1:]
    exe = os.path.join(REPO, "build-modded", "GLFrontier")
    show_all = "--all" in args
    if "--all" in args:
        args.remove("--all")
    if "--exe" in args:
        i = args.index("--exe")
        exe = args[i + 1]
        del args[i:i + 2]
    log = args[0] if args else "crash.log"

    text = open(log, errors="replace").read()
    crashes = text.split("==== CRASH")[1:]
    if not crashes:
        sys.exit(f"no crash in {log}")
    for crash in (crashes if show_all else crashes[-1:]):
        report(crash, exe)


def report(crash, exe):
    lines = crash.splitlines()
    print("==== CRASH" + lines[0])
    for ln in lines[1:]:
        if ln.startswith("  #") or ln.startswith("  pc ") or ln.startswith("==== END"):
            continue
        print(ln)

    m = re.search(r"exe mtime (\d+)", crash)
    if m and int(m.group(1)) != int(os.stat(exe).st_mtime):
        print(f"!! {exe} was rebuilt since this crash: the lines below are probably wrong")

    exe_name = os.path.basename(exe)
    frames = re.findall(r"  (?:pc |#(\d+)\s+)\s*(\S+)\+0x([0-9a-f]+) (\S+)", crash)
    print("where:")
    for num, mod, off, sym in frames:
        tag = "pc " if num == "" else f"#{num:<2}"
        if mod != exe_name:
            print(f"  {tag} {mod}+0x{off} {sym}")
            continue
        # return addresses point after the call: -1 lands on the call itself
        addr = int(off, 16) - (1 if num not in ("", "0") else 0)
        out = subprocess.run(["addr2line", "-e", exe, "-f", "-i", "-C", hex(addr)],
                             capture_output=True, text=True).stdout.split("\n")
        pairs = [(out[i], out[i + 1]) for i in range(0, len(out) - 1, 2)]
        for k, (func, where) in enumerate(pairs):
            print(f"  {tag if k == 0 else '   '} {func} at {short(where)}")
            game = game_line(where)
            if game:
                print(f"       game code: {game}")


def short(path):
    return path.replace(REPO + "/", "")


_src_cache = {}


def read_lines(path):
    if path not in _src_cache:
        try:
            _src_cache[path] = open(path, errors="replace").read().split("\n")
        except OSError:
            _src_cache[path] = None
    return _src_cache[path]


def game_line(where):
    """fe2*.s.c:N -> the .s line (from the nearest 'line_no = N' above) and label"""
    m = re.match(r"(.*fe2[^/]*\.s)\.c:(\d+)", where)
    if not m:
        return None
    c_lines = read_lines(m.group(1) + ".c")
    if not c_lines:
        return None
    n = int(m.group(2)) - 1
    s_line = label = None
    for i in range(min(n, len(c_lines) - 1), -1, -1):
        t = c_lines[i]
        if s_line is None:
            mm = re.search(r"line_no = (\d+);", t)
            if mm:
                s_line = int(mm.group(1))
        mm = re.match(r"__N?L?(\w+):", t)
        if mm:
            label = t.rstrip(":").strip()
            break
    if s_line is None:
        return f"label {label}"
    s_lines = read_lines(m.group(1)) or []
    asm = s_lines[s_line - 1].strip() if 0 < s_line <= len(s_lines) else "?"
    return f"{os.path.basename(m.group(1))}:{s_line} '{asm}' (in {label})"


if __name__ == "__main__":
    main()
