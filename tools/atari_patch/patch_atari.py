#!/usr/bin/env python3
"""
patch_atari.py - apply the original-game bug fixes (docs/original-game-fixes.md)
to the Atari ST executable of Frontier: Elite II (GEMDOS .PRG).

It never ships any game data: the input is your own copy, the output is that
copy plus a few same-size hooks and one block of new code appended to the end
of the text segment. For each hook the original bytes (and a little context
around them) are searched for, so the patch also applies to other releases of
the executable as long as that code is unchanged, and refuses to touch
anything it does not recognise. Run it on the unpacked executable.

usage: patch_atari.py <in.prg> <out.prg> [--check] [--verify-cave AS68K]

  --check             only report what would be patched
  --verify-cave PATH  assemble fixes.s with the given as68k and check that it
                      matches the code embedded below

What it does to the file:
  - each hook (a jsr/jmp/bsr of the same size as the bytes it replaces)
  - the new routines appended after the text, tlen grown to cover them
  - the GEMDOS relocation table rewritten: every new absolute address (the
    hook targets and the jsr inside the new code) gets an entry, and entries
    in front of the new code are kept in order
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

HEADER = 0x1C

# Code of fixes.s (assembled on its own, starting at 0): Lgrav_norm 0x00,
# Lgrav_mass 0x0e, Lcool_dec 0x18, Lprox_log 0x2e, Lframe_cand 0x4c, Lpax_late 0x80,
# Ltguard 0xaa (and its table at the end).
CAVE = bytes.fromhex(
    "5247e2484443d647444353434e754ca90060007adc434e75"
    "e28fbebc000001f163062e3c000001f19f6801164e75d081"
    "d082b0bc40000000650ebabc3fffffff6406203c3fffffff"
    "b0854e7508340004000066224a340000672248e780404eb9"
    "000391d00c290001005e660c0829000401184cdf02014e75"
    "4cdf0201b0004e754aa8000c660e0c28001c000367064ef9"
    "000734aab4a80004670451cf00084ef9000734584ef90007"
    "3522bc7c00086e424a2e3fd0663ab3ee02a266340c2e0008"
    "02c0672c362e28c0b67c000c6222d6433a3b30283805e044"
    "bc446e14082e000128ec67027ac11d453fd11d7c00ff3fd0"
    "4e75588f4ef900037ffe80008000800006c1800080008000"
    "05c28000800003c3800003c4"
)
L_GRAV_NORM, L_GRAV_MASS, L_COOL_DEC, L_PROX_LOG, L_FRAME_CAND, L_PAX_LATE, L_TGUARD = 0x00, 0x0E, 0x18, 0x2E, 0x4C, 0x80, 0xaa
# the jsr to the original L44af8 inside Lframe_cand, the three jmps of Lpax_late, the jmp of Ltguard
CAVE_RELOCS = [0x60, 0x90, 0xa0, 0xa6, 0xf6]

# Each hook: name, original bytes, context before / after (bytes that must be
# there too), new bytes as a function of (hook text offset, cave text base)
# returning (bytes, [offsets of absolute longs in them]).
def jsr(target_off):
    return lambda at, base: (b"\x4e\xb9" + struct.pack(">L", base + target_off), [2])

def jmp_nop(target_off):
    # a jmp and a nop over an 8 byte instruction pair
    return lambda at, base: (b"\x4e\xf9" + struct.pack(">L", base + target_off) + b"\x4e\x71", [2])

def jmp_after_bra(target_off):
    # bra.s over a jmp: the jmp is the trampoline a later bsr.s goes through
    return lambda at, base: (b"\x60\x06\x4e\xf9" + struct.pack(">L", base + target_off), [4])

def bsr_to_trampoline(trampoline_at):
    def f(at, base):
        disp = (trampoline_at(at) - (at + 2)) & 0xFF
        return b"\x61" + bytes([disp]) + b"\x4e\x71", []
    return f

HOOKS = [
    # name, original bytes, hint offset, new
    ("gravity: exponent (L43ed6 l43f5a)",
     bytes.fromhex("e243d647966900b0"), 0x38636, jmp_after_bra(L_GRAV_NORM)),
    ("gravity: normalisation (L43ed6 l43f7e)",
     bytes.fromhex("5247e248"), 0x3865C, None),   # filled in below, needs the trampoline
    ("gravity: mass exponent (L43ed6 l43faa)",
     bytes.fromhex("4ca90060007a"), 0x38682, jsr(L_GRAV_MASS)),
    ("sqrt overflow: proximity test (L3f566)",
     bytes.fromhex("d081d082b085"), 0x33EB0, jsr(L_PROX_LOG)),
    ("sqrt overflow: frame-switch cooldown (L3f460)",
     bytes.fromhex("e28f9f680116"), 0x33CBC, jsr(L_COOL_DEC)),
    ("relative to: frame candidates (L440fe)",
     bytes.fromhex("083400040000"), 0x38822, jsr(L_FRAME_CAND)),
    ("passengers: late group leaves (l7edac)",
     bytes.fromhex("b4a8000457cf00cc"), 0x73450, jmp_nop(L_PAX_LATE)),
    ("time acceleration: step down for ships closing in (L4388c l438f0)",
     bytes.fromhex("bc7c00086e0e"), 0x37FEA, jsr(L_TGUARD)),
]


# Byte signatures taken from fe2.s.bin ("??" = bytes of a relocated address,
# which differ between releases): 12 bytes of context before and after each
# hook, and 24 bytes at the start of each original routine the new code jumps
# to. They are searched for in the executable, so the hooks and those routines
# are found wherever the release has put them (the retail disk's FRONTIER.PRG
# has the same code as fe2.s.bin about 0xb900 bytes further in).
HOOK_SIG = [
    "eea0eea1eea27c00de463607e243d647966900b04a69007a670000d448ae0007",
    "????4a40670a5347d0406afa5247e2483200670000b4c1c0d0804840",
    "670a5347d0406afa5247e2484ca90060007a4a40671a9c4748459a45e485",
    "c6c5d68396434843cac5da83d081d082b085620001d24cdf00385c8f1c29",
    "00dc4a68011667102e2e386ee28f9f68011664264268011608000001670c",
    "00284cdf0240be576f72707208340004000056c8fff8670000543f006100",
    "6100059208ae000000814e75b4a8000457cf00cc66ea4aa8000c664a48e70180",
    "9c433a2e28c09c45e4459c45bc7c00086e0e422801194229006155466b00",
]
HOOK_CTX = 12
TARGET_SIG = {   # name: (signature, offset in CAVE of the long to fill in)
    "L44af8": ("d040224cd2fb00044e75007400d401f20310042e054c066a", 0x60),
    "l7ee06": ("66ea4aa8000c664a48e7018070fd78ff0828000600026618", 0x90),
    "l7edb4": ("b83c001c6604e099600ae40c48845244996e02e848e74180", 0xa0),
    "l7ee7e": ("41e800100828000700026600ff66b4a8000457cfffec6600", 0xa6),
    "L43926": ("de5f51476b3ce24764085247e240e241e24248a7e0004eb9", 0xf6),
}


def fail(msg):
    sys.exit("patch_atari: " + msg)


def decode_relocs(rel):
    """GEMDOS relocation table -> sorted list of text offsets."""
    if len(rel) < 4 or struct.unpack(">L", rel[:4])[0] == 0 and len(rel) == 4:
        return []
    pos = struct.unpack(">L", rel[:4])[0]
    out = [pos]
    i = 4
    while i < len(rel) and rel[i]:
        if rel[i] == 1:
            pos += 254
        else:
            pos += rel[i]
            out.append(pos)
        i += 1
    if i >= len(rel):
        fail("relocation table has no terminator")
    return out


def encode_relocs(offs):
    if not offs:
        return struct.pack(">L", 0)
    out = bytearray(struct.pack(">L", offs[0]))
    prev = offs[0]
    for o in offs[1:]:
        d = o - prev
        while d > 254:
            out.append(1)
            d -= 254
        out.append(d)
        prev = o
    out.append(0)
    return bytes(out)


def find_unique(text, orig, ctx_before, ctx_after, masked):
    """Offsets where ctx_before + orig + ctx_after occurs with the bytes at
    relocated longs ignored."""
    pat = ctx_before + orig + ctx_after
    first = len(ctx_before)
    hits = []
    start = 0
    while True:
        i = text.find(orig, start)
        if i < 0:
            break
        start = i + 1
        lo = i - len(ctx_before)
        if lo < 0 or i + len(orig) + len(ctx_after) > len(text):
            continue
        ok = True
        for k, b in enumerate(pat):
            if lo + k in masked:
                continue
            if text[lo + k] != b:
                ok = False
                break
        if ok:
            hits.append(i)
    return hits


def find_sig(text, sig, masked):
    """Offsets where the signature matches (wildcards and relocated bytes of
    the executable ignored)."""
    pat = [None if sig[i:i + 2] == "??" else int(sig[i:i + 2], 16) for i in range(0, len(sig), 2)]
    first = next(k for k, v in enumerate(pat) if v is not None)
    hits = []
    i = text.find(bytes([pat[first]]))
    while i >= 0:
        lo = i - first
        if lo >= 0 and lo + len(pat) <= len(text) and all(
                v is None or text[lo + k] == v for k, v in enumerate(pat)):
            hits.append(lo)
        i = text.find(bytes([pat[first]]), i + 1)
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("input")
    ap.add_argument("output", nargs="?")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--verify-cave")
    a = ap.parse_args()

    if a.verify_cave:
        verify_cave(a.verify_cave)
    d = open(a.input, "rb").read()
    if len(d) < HEADER or struct.unpack(">H", d[:2])[0] != 0x601A:
        fail("not a GEMDOS program (no $601A header)")
    magic, tlen, dlen, blen, slen, res1, flags, absflag = struct.unpack(">HLLLLLLH", d[:HEADER])
    if dlen or blen or slen:
        fail("data/bss/symbol segments present; expected a single text segment")
    if absflag:
        fail("absolute flag set; no relocation table to extend")
    text = bytearray(d[HEADER:HEADER + tlen])
    relocs = decode_relocs(d[HEADER + tlen:])
    if encode_relocs(relocs) != d[HEADER + tlen:]:
        fail("relocation table does not re-encode identically; refusing to rewrite it")
    if tlen & 1:
        fail("odd text length")
    base = tlen  # the new code goes at the end of the text
    masked = set()
    for r in relocs:
        masked.update(range(r, r + 4))

    # the bsr.s needs the position of the trampoline, which is the first hook
    sites = {}
    plan = []
    for idx, (name, orig, hint, new) in enumerate(HOOKS):
        hits = [h + HOOK_CTX for h in find_sig(bytes(text), HOOK_SIG[idx], masked)]
        if len(hits) != 1 or bytes(text[hits[0]:hits[0] + len(orig)]) != orig:
            fail("%s: found %d places (expected exactly 1); this executable is "
                 "not one this patch knows" % (name, len(hits)))
        sites[idx] = hits[0]
        plan.append((name, hits[0], orig, new))

    # fill in the bsr.s that goes through the first hook's jmp
    tramp = lambda at: sites[0] + 2
    plan[1] = (plan[1][0], plan[1][1], plan[1][2], bsr_to_trampoline(tramp))
    if not (-128 <= tramp(0) - (sites[1] + 2) < 0):
        fail("the gravity hooks are too far apart for a bsr.s")

    new_relocs = list(relocs)
    drop = set()
    print("%s: text %d bytes, %d relocations" % (os.path.basename(a.input), tlen, len(relocs)))
    for name, at, orig, new in plan:
        nb, absolute = new(at, base)
        assert len(nb) == len(orig), (name, nb.hex(), orig.hex())
        inside = [r for r in relocs if at < r + 4 and r < at + len(orig)]
        for r in inside:
            drop.add(r)
        print("  %-52s at $%05x  %s -> %s%s" % (name, at, orig.hex(), nb.hex(),
              "  (drops reloc %s)" % ",".join("$%x" % r for r in inside) if inside else ""))
        text[at:at + len(orig)] = nb
        new_relocs.extend(at + o for o in absolute)
    code = bytearray(CAVE)
    for tname, (tsig, coff) in TARGET_SIG.items():
        th = find_sig(bytes(text), tsig, masked)
        if len(th) != 1:
            fail("original routine %s: found %d places (expected exactly 1)" % (tname, len(th)))
        code[coff:coff + 4] = struct.pack(">L", th[0])
    new_relocs.extend(base + o for o in CAVE_RELOCS)
    print("  new code: %d bytes at $%05x, relocations at %s" % (
        len(code), base, ", ".join("$%x" % (base + o) for o in CAVE_RELOCS)))
    new_relocs = sorted(set(new_relocs) - drop)
    if len(set(new_relocs)) != len(new_relocs):
        fail("duplicate relocation")
    if a.check or not a.output:
        print("check only, nothing written")
        return
    text += code
    rel = encode_relocs(new_relocs)
    out = bytearray(d[:HEADER]) + text + rel
    struct.pack_into(">L", out, 2, len(text))
    if res1 == len(d):  # some tool stored the file size here; keep it true
        struct.pack_into(">L", out, 18, len(out))
    open(a.output, "wb").write(out)
    print("wrote %s: %d bytes, %d relocations, sha1 %s" % (
        a.output, len(out), len(new_relocs), hashlib.sha1(out).hexdigest()))


def verify_cave(as68k):
    here = os.path.dirname(os.path.abspath(__file__))
    with tempfile.TemporaryDirectory() as t:
        src = os.path.join(t, "fixes.s")
        open(src, "wb").write(open(os.path.join(here, "fixes.s"), "rb").read())
        r = subprocess.run([as68k, "fixes.s"], cwd=t, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, text=True)
        b = open(os.path.join(t, "fixes.s.bin"), "rb").read()
        code = b[HEADER:HEADER + struct.unpack(">L", b[2:6])[0]]
        if code != CAVE:
            fail("embedded code differs from fixes.s:\n  embedded %s\n  assembled %s" % (
                CAVE.hex(), code.hex()))
    print("embedded code matches fixes.s")


if __name__ == "__main__":
    main()
