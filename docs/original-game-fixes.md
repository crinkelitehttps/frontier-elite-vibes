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
`Lcool_dec`, `Lprox_log`, `Lframe_cand`, `Lpax_late`) or a code cave. All fixes are in both
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

## 4. Altitude readout appears too late over stars and gas giants (`L6366c`)

**Symptom:** scooping from a star is nearly impossible: the "Alt." readout only
shows below 60 km, and a ship in orbit round a star is too fast to react.
Above that the HUD gives no altitude. (Original-game bug report 8.)

**Cause:** the HUD (`l6366c` in the flight display code) shows the altitude
`692(a6)` (mm) only when `mm / 1000` fits a 16 bit number and is at most
`$ea60` = 60000 m. The altitude itself is exact out to 2147 km.

**Original code:** `divu #$3e8,d3 / bvs.s / andi.l #$ffff,d3 / cmp.w #$ea60,d3 /
bhi.s` (18 bytes).

**Fix:** `jsr Lalt_hud / bhi.s / 5 nop`. `Lalt_hud` divides to metres in 32 bit
and uses a limit of 2,000,000 m when the nearest body is a star or gas giant
(model `$92`-`$ae`, bit 0 of `893(a6)`), 60 km otherwise. Both variants.

## 5. Smoke of a fast damaged ship is left behind (`L4600a`) - option

**Symptom:** a damaged ship at high speed leaves a trail of smoke puffs that
hang at rest where they were made, so the trail stretches hundreds of km
behind the ship. (Original-game bug report 4.)

**Cause:** `L4600a` (puff spawner, called by damaged ships each frame, by hits
and by debris) makes every puff with zero velocity.

**Original code:** `move.b #$b,94(a1)` in `L4600a`.

**Fix:** `jsr Lsmoke_vel` (same size). `Lsmoke_vel` calls the host
(`Call_SmokeVel`), which gives the puff the part of the ship's velocity
(`98(a0)`) over a limit. Below the limit nothing changes, so slow combat is as
before. It is an option, off by default: `smoke_inherit` in `glfrontier.cfg`,
and `smoke_cap` is the limit in m/s (10-2000, default 500; one velocity unit is
about 0.095 m/s). In the cog menu under Flight. Both variants. Tested with
`tools/savetool/scenarios/smoke-fast-*.txt` (ships at 1.5 and 5 km/s).

## 6. Passengers past their month never leave (`l7edac`, `Lpax_late`)

**Symptom:** passengers taken from the bulletin board and not delivered within
a month stay in the ship, and their cabins stay used, until the player ejects
in an escape pod. (Original-game bug report 10.)

**Cause:** the daily event (`L7f0cc`) only marks a late entry (bit 15 of its
flags word at `2(a0)`); the entry is removed later by `L7ee5e`. For the
bulletin-board entries (bit 13 set, no client name at `12(a0)`) `l7edac` does
that only when the ship is in the entry's destination system, and its late
arrival path (`L7ef98` message `$9c35`) removes the entry without freeing the
cabins (`744(a6)`) the way the named-client path `l7ee06` does. So a late group
waits for a destination the player may never visit, and when it is reached the
cabins stay counted.

**Original code:** `cmp.l 4(a0),d2` / `dbeq d7,l7ee7e` at `l7edac` (8 bytes).

**Fix:** `jmp Lpax_late` + `nop` (same size). For an unnamed entry that is not
a parcel (low byte of the flags is not `$1c`), `Lpax_late` jumps to `l7ee06`
at once: cabins freed, the "Don't even ask for payment" message, entry removed,
at the next landing wherever it is. Named clients and parcels keep the
destination test. Both variants. Tested by writing an expired entry into the
list (`poke`) with a different destination: it leaves and `744(a6)` drops.

## 7. Police Vipers explode on launch (`L80f5c`, `L80ebc`, `L755d0`, `L750c0`)

**Symptom:** after a crime near a station the police Vipers launch and are
destroyed in or at the bay, some without ever leaving it. (Original-game bug
report 9.)

**Cause:** several things at once. (1) A burst is four ships made on the same
frame at four bay slots (fn 9 of the module at `68(a6)`: x = +-25 m, z = +120 m
for slots 0 and 1, z = -30 m for 2 and 3, in the station's frame). The z = -30 m
slots are inside the station: a ship put there hits it every tick (the bounce
of `l3f9d6` takes its velocity to zero in a few ticks), stays in the bay in
close combat and blows up. (2) The ships are held in mode `$7c` (`L755d0`, no
physics) until the offender flag `159(a1)` clears, so a later burst is put on
top of ships still held in the same slots; all of them leave on the same frame
and hit each other. (3) Even from a good slot a ship collides with the station
for a tick or two on its way out (the collision damage routine at `l3f9b4` /
`l3f9c6` kills it), and in mode 4 close to the player it goes into close
combat (mode 8) inside the bay.

**Original code** (all replaced by same-size hooks): `moveq #9,d1` / `movea.l
68(a6),a0` / `jsr 32(a0)` in `L80f5c`, `move.b d0,211(a1)` / `btst #5,280(a0)`
there, `moveq #3,d7` at `l80ef6`, `move.b #4,211(a0)` at `l755e6`, `moveq #0,d7`
/ `move.b 211(a0),d7` in `L750c0`, and the two `jsr 486(a5)` damage calls in the
collision routine.

**Fix** (`Lpol_slot`, `Lpol_spawn`, `Lpol_release`, `Lpol_phys`, `Lpol_grace`,
`Lpol_dmg1/2`): at a station (not a ground base, which keeps its own pads and
four ships) a burst is two ships and only slots 0 and 1 are used. A held
ship is given a release time (kept in the unused `262(a0)`) by the once-a-second
AI step; release times are 0.75 s apart across all bursts. From its release time
the physics runs as mode 4 but the ship stays in hold mode (no steering, no
combat) for 3 s, so it coasts straight out on its launch velocity, then goes to
mode 4. While held or coasting, and for 2 s after, a police Viper takes no
collision damage. Both variants. Tested on Lave Station with a save outside it
at a steep angle to the bay: three bursts (six ships) all left the bay and
attacked; before the fix the rear-slot ships exploded or jammed in the bay.

## 8. Proximity mines only go off if they touch a ship (`L74eda`, `Lmine_phys`)

**Symptom:** a proximity mine does not explode when a ship passes near it; it seems to go off only when the
target points at it from about 1 km. (Original-game bug report 5.)

**Cause:** a mine (AI mode `$48`) has no AI step (`L757de` is an `rts`) and its physics (`L74eda`) only runs
the ordinary collision test (`786(a5)`, `L3f460`), which reports a hit when the mine's model touches the other
ship's hull, worked out in the other ship's frame. There is no proximity fuse at all.

**Original code:** `jsr 750(a5)` / `bra.s l74f04` in `L74eda` (6 bytes).

**Fix:** `jmp Lmine_phys` (same size). Every tick the mine looks at all objects in its frame that are ships
(model above `$10`, not a body) and, when one is within the radius, goes off by the game's own hit path
(`l75824`, a1 = that ship): the normal damage for the mine's model (100 for the XB74), the explosion, the
owner's credit. Only its owner does not set it off; every other ship does, the player too when it is an enemy's mine. The radius is an
option (menu, Flight: a checkbox and a slider, 10-2000 m; `mine_prox` and `mine_radius` in `glfrontier.cfg`;
default on, 250 m); off is the game's own behaviour. The host gives it to the 68k code with `Call_MineRadius`
(`$97`). A hook at the start of `l75824` (`Lmine_boom`, replaces `move.b 93(a1),d0` / `cmp.b 212(a0),d0`)
makes the blast: when a mine goes off (by the fuse or by hitting a ship) every other ship within the radius,
the player and the owner too, takes the mine's damage, as the ship that was hit does. Both variants. Tested
with `tools/savetool/scenarios/mine-test.txt` (a pad ship 380 m away, the player's hull set to 400): at 500 m
the pad ship loses 100 hull and so does the player; at the default 250 m nothing goes off; in the old build the
mine just sat there. A third ship in range was not tested.

## Known, not fixed

- Speed bumps briefly at 20 km on a climb: `L61c26` in the player physics
  handler cuts the linear atmospheric drag off with a hard `cmpi.l
  #$1312d00,692(a6)` (no density fade).
