# Fixes to bugs in the original game

Bugs in Frontier: Elite II's own 68k code (present in the unmodified game, not
introduced by this port) that this fork fixes. Each entry gives the symptom,
the cause in terms of the original routine, the original instructions replaced,
and the fix, so the same change can be reproduced elsewhere, for example as a
patch to the original binary.

Addresses are routine labels from `fe2/fe2.s` (the original game code,
disassembled), not file offsets. Object fields are offsets into the 3D object
struct (`a0`/`a1`); `a6` is the game's global block. Every fix is a same-size
`jsr`/`jmp` hook with the new code appended at the end of the code, so nothing
in the middle moves. `tools/atari_patch/patch_atari.py` applies the fixes to the Atari ST executable
(the new code goes after the end of the text; no cave needed). A binary patch needs room for that code (the game's code
ends at the end of `fe2.s`; the new routines are `Lgrav_norm`, `Lgrav_mass`,
`Lcool_dec`, `Lprox_log`, `Lframe_cand`) or a code cave. All fixes are in both
`fe2/fe2.s` and `fe2/fe2_modded.s`. The `hcall` in `Lprox_log` is a
diagnostic and can be dropped.

## 1. Gravity doubles in low orbit (`L43ed6`) - 52b2a35

**Symptom:** a ~100-300 km orbit is stable, then the pull suddenly doubles at
one point of the orbit (position within +-1 unit = 4295 km per axis) and the
ship crashes.

**Cause:** `L43ed6` normalises |r| by shifting it up by sigma, and builds the
exponent for the r^3 divisor as `d7 + d7/2` (about 2*e) instead of 3*e. The
pull is therefore wrong by 2^-sigma, and sigma changes by one wherever the
position exponent e changes (e = 19 vs 18), giving a factor 2 step.

**Original code** (three places in `L43ed6`):

    l43f5a: add.w d6,d7 / move.w d7,d3 / asr.w #1,d3 / add.w d7,d3 / sub.w 176(a1),d3
    l43f7e: ... bpl.s l43f7e / addq.w #1,d7 / lsr.w #1,d0
    l43faa: movem.w 122(a1),d5-6

**Fix:** the exponent in d3 gets sigma-1 subtracted (`Lgrav_norm`) and is added
to the mass exponent (`Lgrav_mass`), so the pull always equals the old sigma = 1
behaviour. See the comment above `Lgrav_norm`.

## 2. Ship destroyed far out in orbit (`L3f566`, `L3f460`) - 62e1dc5

**Symptom:** with e = 19 the ship explodes each time its orbit crosses
|r| = 2.0 position units (about 2490 km above Lave).

**Cause:** `L3f566` (proximity / altitude) sums the squared normalised
coordinates into d0. The altitude square root at `l3f864` is a signed 32-bit
root, valid only for d0 < 2^30, but normalisation keeps each coordinate below
2.0, so d0 can reach 3*2^30. Past 2^30 the root returns a small value, the
altitude reads 0, and the game believes the ship has landed: drag, damage,
death. (At e = 20 the scale halves, so it never shows there.)

**Original code:** `add.l d1,d0 / add.l d2,d0 / cmp.l d5,d0` before
`bhi.w l3f864`.

**Fix:** `Lprox_log` does the same three instructions but first clamps d0 to
`$3fffffff` when it is >= 2^30 and d5 (the squared radius) is below that, so
the altitude stays "unknown" (-1).

**Second part, same symptom at time acceleration:** the frame-switch cooldown
`278(a0)` in `L3f460` ran down by dt/2 per frame, so at high acceleration it was
gone in a frame or two and the ship flapped between frame byte 255 and 0 at the
zone edge. Original `lsr.l #1,d7 / sub.w d7,278(a0)` became `jsr Lcool_dec`,
which takes at most the 1x amount (497) per frame.

## 3. "Relative to" never switches to a station - 835304a

**Symptom:** the speed/position reference (HUD "Relative to:") stays on the
planet however close you fly to a station, and the station drifts away in its
orbit. Matches the original-game bug report of the speed reference staying on
the centre body "even when a few hundred km away".

**Cause:** the reference frame is chosen by `L440fe`, which scans object slots
114..0 and only considers those whose per-object flag byte (the array at
`14462(a6)`) has bit 4 set. Station bodies are created with flag `$1f`, but the
comms-menu action `L76990` (entry 24 of the table at `L76476`) sets the
*targeted* object's flag to `$4f` (`moveq #79,d0 / jsr 528(a5)`), clearing
bit 4, and saved games load stations as `$4f` too. A station you have targeted,
or any station after a load, is never a frame candidate. Stations never
targeted (for example Mars High on a fresh start) keep `$1f` and switch at about
500 km.

**Original code:** at `l4414a` in `L440fe`: `btst #$4,0(a4,d0.w)` (6 bytes).

**Fix:** replaced by `jsr Lframe_cand` (6 bytes). It does the same test, and
additionally treats an occupied slot whose object is a starport body (type
`94(a1) == 1` and bit 4 of `280(a1)`) as a candidate. Z flag out as for `btst`.
The flag writes themselves are left alone.

## Known, not fixed

- Speed bumps briefly at 20 km on a climb: `L61c26` in the player physics
  handler cuts the linear atmospheric drag off with a hard `cmpi.l
  #$1312d00,692(a6)` (no density fade).
