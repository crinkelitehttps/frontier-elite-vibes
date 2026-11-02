# Frontierverse bug list against our fixes

Source: https://www.sharoma.com/frontierverse/bugs.htm (read 2026-11-02, nothing on the page
updated). This compares every entry with what this fork has fixed
(`original-game-fixes.md`), what is open, and what does not apply.

GLFrontier runs the Atari ST (UK 1993) code. The page says the Amiga and Atari ST versions share
the UK PC bugs, so UK 1993 bugs apply. Items marked CD32, German, DOSBox or Windows 98 do not.

Status words: **fixed** (in our fixes list, number given), **partial**, **open** (applies, not
fixed, not investigated), **unverified** (we have not looked in the code or in game; the status
is a guess from our notes), **n/a** (other version or platform).

## Major bugs

| Bug | Status | Notes |
|---|---|---|
| Passenger Bug (trade ship with a passenger contract, price difference credited each time) | open | UK 1993. Likely a missing cabin/contract check in the ship-trade path. Related code: the passenger list at `744(a6)` (fix 6). |
| Wormhole Bug | open, unverified | Page defers to its Wormholes page. See `hyperspace.md`. |
| Bribe Bug (bribe police repeatedly, fine climbs to thousands, claim innocence resets it to 500) | open | Present in every version except the CD32. A 68k arithmetic bug; a same-size hook is the likely fix. A user report says paying a fine inflated this way still deducts the full amount. |
| Elite Rating Bug (mining laser at Phobos raises rating) | open | UK 1993. |
| Mining Bug (unattended MB4 left in a system, hyperspace out crashes) | open | The page says a patch is on its Files page; not fetched. |
| Imperial Ship Bug (Courier and Trader need 1 crew, not 3 and 6) | open | Data, not a crash. |
| Cemiess Negativity Bug (removing gems and metals pays hugely) | open | UK 1993. Economy data; see `starport-economy.md`. |

## Version notes

| Item | Status | Notes |
|---|---|---|
| CD32 joypad/mouse laser bug, CD32 "Flying Instructor" 50% speed bug | n/a | |
| CD32 Free Up Space Bug 2 | n/a | The ordinary Free Up Space below does apply. |
| Scoop Conversion Bug (from Lave, scoop conversion fails and you fly through cargo until you hyperspace out and back) | open | The page says it was seen on the GLFrontier Atari ST build. |
| Interference Bug (corrupt screen pressing F4 on a kill mission at Nakasone Station) | n/a | DOSBox version. |

## Strange happenings and minor bugs (all versions)

| Bug | Status | Notes |
|---|---|---|
| Landing pad collision (low flight over an occupied pad, both ships explode) | open | Engine side in `collision-and-raycast.md`. |
| Escape pod talk | open | Harmless. |
| Graphical glitches: fly through planets, buildings, domes; shoot police through walls | open | |
| Graphical glitches: sky and ground flicker | fixed | Sky/haze flicker fixes, Mars haze fix, cloud depth-slice clip. Not in `original-game-fixes.md` because they are renderer fixes. |
| Graphical glitches: spaceports in the wrong place, horizon jumps | open, unverified | |
| Graphical glitches: enemies impossible to hit near planets | open | Possible link to the unchecked sqrt-overflow rendering copies; see below. |
| Station fireballs (lasers within 5 km of a station) | open | |
| Stupid police: crashes during launches | fixed (7) | |
| Stupid police: crashing into the ground, kamikaze charges | open | |
| "Ship under attack" loop with no ship nearby, blocks the stardreamer | open | May be related to the time guard (`time_guard.c`) warning path; not checked. |
| Manual refuel with Panther Clipper + auto-refueller empties internal tanks | open | |
| Lingering passengers (over a month, stay forever) | fixed (6) | Issue 10. |
| Free Up Space (jettison click frees cargo space) | open | |
| Military fuel cargo cheat | open | |
| Missile Bug (radar-mapped missiles show your drive) | open | |
| Mountains on pads | open | |
| Time-acceleration altitude (3x gains altitude on descent, 1x falls) | open | Possibly the same area as the 20 km drag bump and the fix 2 cooldown. See below. |
| Valkyries message overlap | open | |
| Black Hole Bug (Mercury/Mars-mass planets: cannot land, gravity pulls you down) | open, see below | Probably related to fixes 1 and 2. |
| Busy starport at Ross 154 with free pads | open | |
| Rough landing crash (memory exception) | open, unverified | |
| Target deselection (click near a function icon) | open | The click bug is also in `TODO.txt`. |
| System map centering | open | |
| Undercarriage warning on ships without one | open | |
| Missile collisions (NPC missiles hit you unless you are stationary) | open | See `npc-ship-ai.md`. |
| Long starport names overflow police launch bays | open | |
| Turret problems (Lion fires into its wings; shield drain switching turrets) | open | |
| Imperial rank demotion (failing a Navy mission lowers the Federal rank) | open | |
| German cover slogan | n/a | |
| Cargo scoop without fuel scoop | open | |

## System bugs (crashes in particular systems)

Mizar (0,5) to Alcor (0,5) shows range 0.00 ly (workaround: jump elsewhere and return);
Zeioqu (-17,-29), Greandqu (-19,-31), Andlaar (-27,-32) crash on info or hyperspace; Beta Lyrae
(-146,85) crashes on arrival; Tiquwa (-34,-29) crashes on system details; unexplored systems often
crash on economy/social info; Greliain (4,1) is unsafe and has no F6 info.

All **unverified** in this port. They are cheap to test with the cheat go-to and the galaxy
catalog (`system-generation.md`, `hyperspace.md`), and a crash there is a hard failure, so these
are the first thing to test.

## Other reports

| Item | Status | Notes |
|---|---|---|
| Illegal Operation flying low over cities (Windows 98) | n/a | |
| Newtonian physics (autopilot stops at once) | open | Design, not a bug. |
| German version speed controller lock | n/a | |
| Freezing BBS (two or fewer messages, scroll down) | open, unverified | Workaround: fast-forward to midnight. |
| Low-altitude kill credit (enemies crash on takeoff but count towards rating) | open | |
| Pulse laser timing with time stopped (Amiga) | open, unverified | |
| MB4 mining cargo trick (Amiga) | open, unverified | |
| Escape capsule experiments | open | |

## What the page changes about our fixes and approach

1. **Black Hole Bug and fixes 1 and 2.** Fix 1 (gravity doubles in low orbit) and fix 2 (altitude
   square root overflow) are about the pull and altitude near a body. The page's report is the
   same symptom on small rocky planets: gravity pulls you down, landing fails. We have not tested
   Mars or Mercury-mass bodies after those fixes. The mass/radius exponents of small planets may take
   another path through `L43ed6` or the altitude routine.
   **Test first:** land at Mars (and Phobos), with the old and new build, at normal and low orbit.
2. **Time-acceleration altitude vs. the 20 km drag bump.** Both are altitude and drag behaviours
   that depend on the frame rate or time step. The fix 2 cooldown clamp is the only fix so far in
   this area. The 3x gain may share a cause with the `L61c26` hard drag cutoff.
3. **Enemies impossible to hit near planets.** This supports reviving the parked sqrt-overflow render
   audit (two rendering copies of the same square root unchecked), since the symptom is geometric
   precision near a planet.
4. **Systems that crash.** If any of them crash here, the cause is data in system generation, not code
   we have touched. Test them before the next release.
5. **Bribe Bug and Passenger Bug** are exploits rather than crashes, and fixing them would change
   play. Same question as our other fixes: option, or just fix. Not decided.
6. **Nothing on the page contradicts an existing fix.** It supports the approach of same-size
   hooks in the original code. The CD32 and US 1994 builds fixed some of these in the binary, so
   they are a reference for the Bribe Bug in particular.

## Next steps (not started)

1. Test the six crash systems and Mizar to Alcor.
2. Test landing on Mars and Mercury-mass planets.
3. Decide on Bribe, Passenger, Scoop Conversion and Mining Bug fixes.
4. Review the "Ship under attack" loop against the time guard.
