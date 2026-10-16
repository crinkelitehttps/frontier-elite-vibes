#!/usr/bin/env python3
"""
build_fe2.py - assembles Frontier: Elite 2's 68k code into the files the game
is built from. Two variants of the source exist, see VARIANTS below:

  modded    fe2/fe2_modded.s   Tom Morton's disassembly, extended with this
                                fork's mods (see the top of that file). The
                                default: what "cmake --build" links unless
                                GLF_MODDED_FE2 is turned OFF.
  original  fe2/fe2.s          The same disassembly with none of the mods -
                                unmodified Frontier: Elite 2, annotated but
                                otherwise untouched. Builds and runs fine with
                                GLF_MODDED_FE2=OFF; host C code that needs a
                                mod's labels is compiled out for that build
                                (see FE2_USE_MODDED in src/).

Each variant assembles to its own three files, so both can be checked in and
built from at once:

  fe2/<source>.c           the 68k code translated to C (as68k --output-c) -
                            what actually runs
  fe2/<out_bin_h>           the assembled binary, loaded into emulated RAM at 0
  fe2/<out_labels_h>        RAM addresses of the named labels, for host code

Run this after every change to fe2/fe2_modded.s or fe2/fe2.s, then rebuild the
game. Host code must use FE2_* from the labels header instead of hard coded
addresses, because adding code moves everything after it.

Usage: python tools/build_fe2.py [--variant modded|original|all] [--check] [--as68k path/to/as68k]

  --variant  which source to assemble (default: modded)
  --check    only report whether the checked in outputs are up to date

The web build links prebuilt copies of fe2_modded.s.c (fe2/web/*.a); rebuild those
with tools/build-fe2-web.bat after changing the assembly.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FE2 = os.path.join(ROOT, "fe2")

# The binary starts with a 28 byte TOS header that as68k does not count in
# its label addresses, but the loader copies the whole file to address 0.
HEADER = 0x1C

# Labels host code needs besides the ones with a descriptive name
# L3e894: the core string table (ship names), for tools/fe2ShipBuilder
# The galaxy map routines the galaxy atlas calls (src/galaxy/galaxy_labels.h)
EXTRA_LABELS = ["L3e894",
                "L4beee", "L83b46", "L83d3c", "L84616", "L84700", "L84830", "L849a2", "L84ae6",
                "L8ad0e", "l84b34", "l84c04", "l84cde", "l84f02"]

# The two things this script can build. Everything each variant needs is
# named here explicitly, rather than derived cleverly from the source name,
# so it's obvious at a glance what each one produces.
VARIANTS = {
    "modded": {
        "source": "fe2_modded.s",
        "out_bin_h": "fe2_bin.h",
        "out_labels_h": "fe2_labels.h",
        "labels_guard": "FE2_LABELS_H",
        # Lbuildtag_marker only exists in fe2_modded.s (see BUILD_TAG_SIZE
        # below) - the original disassembly has no such slot to patch.
        "build_tag": True,
    },
    "original": {
        "source": "fe2.s",
        "out_bin_h": "fe2_orig_bin.h",
        "out_labels_h": "fe2_orig_labels.h",
        "labels_guard": "FE2_ORIG_LABELS_H",
        "build_tag": False,
        # fe2.s has these tables too, just without the descriptive names
        # fe2_modded.s gave them. Host code (src/cheats.c) reads them by
        # FE2_<name> in both builds, so export them under the same names.
        "aliases": {
            "obj_offsets": "L44b02",
            "equipment_list": "L7655c",
            "fuel_tank_sizes": "L7e398",
            "elite_rating_points": "L7e87a",
            "military_rank_points": "L7e8b0",
        },
    },
}

# fe2/fe2_modded.s ends with a reserved, zero-filled slot at Lbuildtag_marker
# (see the comment above it there). BUILD_TAG_SIZE must match its size in
# bytes exactly (2 dc.b rows of 12 there = 24).
BUILD_TAG_SIZE = 24
BUILD_TAG_ADJECTIVES = [
    "AMBER", "BRAVE", "COPPER", "DUSTY", "EAGER", "FROSTY", "GOLDEN", "HASTY",
    "IVORY", "JOLLY", "KEEN", "LUCKY", "MISTY", "NOBLE", "ODD", "PLUCKY",
    "QUIET", "ROWDY", "SALTY", "TARTAN", "URGENT", "VIVID", "WOOLY", "ZESTY",
]
BUILD_TAG_NOUNS = [
    "BADGER", "COMET", "DINGO", "EMBER", "FALCON", "GECKO", "HORNET", "IBEX",
    "JACKAL", "KESTREL", "LEMUR", "MAGPIE", "NEWT", "OTTER", "PUFFIN", "QUOKKA",
    "RAVEN", "STOAT", "TOUCAN", "URCHIN", "VIPER", "WALRUS", "YAK", "ZEBRA",
]


def build_tag(source_path):
    """A short, deterministic, easy-to-eyeball word pair from a hash of the
    source file's own bytes - changes if and only if that file's content
    changes. See BUILD_TAG_SIZE and Lbuildtag_marker in fe2_modded.s."""
    with open(source_path, "rb") as f:
        digest = hashlib.sha1(f.read()).digest()
    adjective = BUILD_TAG_ADJECTIVES[digest[0] % len(BUILD_TAG_ADJECTIVES)]
    noun = BUILD_TAG_NOUNS[digest[1] % len(BUILD_TAG_NOUNS)]
    return "%s-%s" % (adjective, noun)


def patch_build_tag(bin_path, labels, source_path):
    addr = labels.get("Lbuildtag_marker")
    if addr is None:
        sys.exit("Lbuildtag_marker not found - see fe2_modded.s")
    tag = build_tag(source_path).encode("ascii")
    if len(tag) >= BUILD_TAG_SIZE:
        sys.exit("build tag %r too long for its %d byte slot" % (tag, BUILD_TAG_SIZE))
    with open(bin_path, "r+b") as f:
        f.seek(addr)
        f.write(tag + bytes(BUILD_TAG_SIZE - len(tag)))
    return tag.decode("ascii")


def find_as68k(path):
    if path:
        return path
    exe = "as68k.exe" if os.name == "nt" else "as68k"
    for d in ("tools/as68k/build/Release", "tools/as68k/build", "tools/as68k"):
        p = os.path.join(ROOT, d, exe)
        if os.path.exists(p):
            return p
    sys.exit("as68k not found: build it with tools/as68k/build_as68k.bat (or make), or pass --as68k")


def assemble(as68k, work, source):
    shutil.copy(os.path.join(FE2, source), work)
    # as68k writes source.c and source.bin to the current directory
    # and dumps labels to stderr
    result = subprocess.run([as68k, "--dump-labels", "--output-c", source], cwd=work,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if result.returncode != 0 or "Error" in result.stdout or "Error" in result.stderr:
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        sys.exit("as68k failed on " + source)
    labels = {}
    for line in result.stderr.splitlines():
        m = re.match(r"0x([0-9a-f]+): (\w+)$", line)
        if m:
            labels[m.group(2)] = int(m.group(1), 16) + HEADER
    return labels


def bin_header(bin_path, out_bin_h):
    """<out_bin_h> in xxd -i layout. Older as68k builds write one too, but read
    the .bin back before it has finished writing it, so that copy can be
    incomplete."""
    with open(bin_path, "rb") as f:
        data = f.read()
    var = out_bin_h.replace(".", "_")
    lines = ["unsigned char %s[] = {" % var]
    for i in range(0, len(data), 12):
        chunk = ", ".join("0x%02x" % b for b in data[i:i + 12])
        lines.append("  " + chunk + ("," if i + 12 < len(data) else ""))
    lines += ["};", "unsigned int %s_len = %d;" % (var, len(data)), ""]
    return "\n".join(lines)


def labels_header(labels, source, out_labels_h, guard, aliases=None):
    # "L<address>_Name" from the original game, "L<mod>_Name" for mods
    wanted = {n: a for n, a in labels.items() if re.match(r"L[0-9a-z]+_\w+$", n) or n in EXTRA_LABELS}
    lines = [
        "/* Generated by tools/build_fe2.py from fe2/" + source + " - do not edit.",
        " * RAM addresses of the game's named labels. */",
        "#ifndef " + guard,
        "#define " + guard,
        "",
    ]
    shorts = [re.sub(r"^L[0-9a-f]+_", "", n) for n in wanted]
    # mod labels keep their prefix: FE2_Lafmu_equipment
    for name, addr in sorted(wanted.items(), key=lambda kv: kv[1]):
        short = re.sub(r"^L[0-9a-f]+_", "", name)
        if shorts.count(short) > 1 or short == name:
            short = name  # e.g. two L..._rts: keep the address to tell them apart
        lines.append("#define FE2_%-40s 0x%05x /* %s */" % (short, addr, name))
    if aliases:
        lines += ["", "/* Same tables as the named ones in fe2_modded.s */"]
        for short, name in sorted(aliases.items()):
            if name not in labels:
                sys.exit("alias FE2_%s: label %s not found in fe2/%s" % (short, name, source))
            lines.append("#define FE2_%-40s 0x%05x /* %s */" % (short, labels[name], name))
    lines += ["", "#endif /* " + guard + " */", ""]
    return "\n".join(lines)


def bin_bytes(path):
    with open(path) as f:
        text = f.read()
    return re.findall(r"0x[0-9a-fA-F]{2}", text[text.index("{"):text.index("}")])


def same(a, b):
    if not os.path.exists(b):
        return False
    if a.endswith("_bin.h"):
        # the array name and bytes matter, not line endings
        with open(a) as fa, open(b) as fb:
            if fa.readline().strip() != fb.readline().strip():
                return False
        return bin_bytes(a) == bin_bytes(b)
    with open(a, "rb") as fa, open(b, "rb") as fb:
        return fa.read() == fb.read()


def build_variant(name, cfg, as68k, check):
    source = cfg["source"]
    out_c = source + ".c"
    print("=== %s (fe2/%s) ===" % (name, source))
    with tempfile.TemporaryDirectory() as work:
        labels = assemble(as68k, work, source)
        tag = None
        out_bin = source + ".bin"
        if cfg["build_tag"]:
            tag = patch_build_tag(os.path.join(work, out_bin), labels, os.path.join(FE2, source))
        with open(os.path.join(work, cfg["out_labels_h"]), "w", newline="\n") as f:
            f.write(labels_header(labels, source, cfg["out_labels_h"], cfg["labels_guard"],
                                  cfg.get("aliases")))
        with open(os.path.join(work, cfg["out_bin_h"]), "w", newline="\n") as f:
            f.write(bin_header(os.path.join(work, out_bin), out_bin))

        outputs = [out_c, cfg["out_bin_h"], cfg["out_labels_h"]]
        stale = [o for o in outputs if not same(os.path.join(work, o), os.path.join(FE2, o))]
        if check:
            print("up to date" if not stale else "out of date: " + ", ".join(stale))
        else:
            for o in stale:
                # os.replace (rename-over) instead of shutil.copy: on Windows,
                # copy opens the destination for writing and fails with
                # "Invalid argument" if some other process (e.g. VS Code's
                # cpptools indexer) has the big generated .s.c memory-mapped.
                # Renaming a new file over it works even then.
                os.replace(os.path.join(work, o), os.path.join(FE2, o))
            print("updated: " + ", ".join(stale) if stale else "nothing changed")
        if tag:
            print("build tag: " + tag + "  (also shown in-game on the DEBUG page)")
        return bool(stale)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", choices=["modded", "original", "all"], default="modded")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--as68k")
    args = ap.parse_args()

    as68k = os.path.abspath(find_as68k(args.as68k))
    names = list(VARIANTS) if args.variant == "all" else [args.variant]
    any_stale = False
    for name in names:
        any_stale |= build_variant(name, VARIANTS[name], as68k, args.check)
    return 1 if (args.check and any_stale) else 0


if __name__ == "__main__":
    sys.exit(main())
