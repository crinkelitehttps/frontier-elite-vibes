# Atari ST patcher for the original-game bug fixes

`patch_atari.py` applies this fork's fixes for bugs in Frontier: Elite II's own
68k code to the **Atari ST executable** (a GEMDOS `.PRG`). It ships no game
data: you give it your own copy, it writes a patched copy.

```
python3 tools/atari_patch/patch_atari.py <in.prg> <out.prg>
python3 tools/atari_patch/patch_atari.py <in.prg> --check      # report only
python3 tools/atari_patch/patch_atari.py <in.prg> <out.prg> --verify-cave <as68k>
```

Run it on the unpacked executable (a single text segment, no data/bss/symbols,
relocation table at the end). It needs only Python 3.

## What it does

- Finds each of the six hook sites by the original bytes (the bytes at
  relocated addresses are ignored). Every pattern must match exactly once, so
  an executable it does not recognise is refused and nothing is written.
- Replaces the original instructions with a same-size `jsr`/`jmp`/`bsr.s`, so
  nothing in the middle of the program moves.
- Appends 128 bytes of new routines after the end of the text, grows the text
  length in the header and extends the relocation table (a new entry for every
  absolute address: the hook targets and the `jsr` inside the new code).

The new routines are in `fixes.s` and embedded in the script, so the patcher
needs no assembler. `--verify-cave` assembles `fixes.s` with `as68k`
(`tools/as68k`) and checks it still matches the embedded copy.

Tested only structurally on `fe2.s.bin` (the text differs from the original in
the hook bytes only; relocations sorted and all inside the text; hooks and
routines identical to what `fe2/fe2.s` assembles to). Not yet run on a real
Atari or in an emulator.

## The bugs it fixes

Full write-ups with the replaced instructions are in
`docs/original-game-fixes.md`.

| Fix | Symptom | Where |
|---|---|---|
| Gravity doubles in low orbit | A stable ~100-300 km orbit suddenly feels twice the pull at one point and the ship crashes. | `L43ed6`: the r^3 exponent was built from 2*e, not 3*e, so the pull was wrong by 2^-sigma and stepped by 2 wherever the position exponent changes. Three hooks (`Lgrav_norm`, `Lgrav_mass`). |
| Ship destroyed far out in orbit | The ship explodes when its orbit crosses |r| = 2.0 position units (about 2490 km above Lave). | `L3f566`: the altitude square root is only valid below 2^30; past it the altitude reads 0 and the game thinks the ship landed. `Lprox_log` clamps the sum. |
| Same, at high time acceleration | The ship flaps between frames at the zone edge and dies. | `L3f460`: the frame-switch cooldown ran down by dt/2 a frame; `Lcool_dec` takes at most the 1x amount. |
| "Relative to" never switches to a station | The speed reference stays on the planet however close you fly to a station. | `L440fe`: targeted or loaded stations are flagged `$4f`, clearing the bit the frame search tests; `Lframe_cand` also accepts starport bodies. |

Not fixed: the speed bump at 20 km on a climb (see the end of
`docs/original-game-fixes.md`).

## Other versions

Only the Atari ST executable is handled. The Amiga and DOS releases need their
own patchers (different executable formats; DOS is x86).
