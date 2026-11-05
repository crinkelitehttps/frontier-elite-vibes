# Scenery: cells, the host's copies, and drawing by distance

How the ground objects (cloud fields, models 99 and 101-104) are placed and drawn now. Code: `src/scenery.c`
(host), `Lscen_pick`, `Lscen_made`, `Lvirt_a` at the end of `fe2/fe2.s` and `fe2/fe2_modded.s`, hooks in the spawner
`L61cf4` and the object loop `L44480`. `GLF_SCEN_LOG=1` prints the number of copies now and then.

## 1. What the original did, and why it popped

[code] Each try (20% of frames, below 20 km, fewer than 15 temporary objects) cast a ray in a random direction from a
point 33.5 km above the ship and made an object where it hit the planet within 67 km. So objects appeared 13-58 km away,
half of them behind, and the population (15 over a 280 km radius) only refilled when one reached the despawn limit
(`95(obj) >= $f`). Nothing remembered where objects had been. My first deterministic version (cells, but the same 114
object slots, random tries, forward lead, rear despawn) still depended on the direction of travel and on what the
random tries had reached.

## 2. The design

The scenery no longer lives in the game's object slots.

1. **Cells.** The surface is cut into cubes of `2^22` mm (4.2 km) in the planet's frame. A cell has a feature when
   `(h >> 16) % 8 == 0`, `h` = hash of the planet (model word and name) and the cell. Kind (one of the planet type's 8
   table entries), the turn, the cloud bits and the place inside the cell all come from `h`.
2. **The host decides what is needed** (`scan` in `scenery.c`): every cell the surface passes through whose centre is
   within `scenery_radius_km` of the ship by the game's own distance (largest coordinate), nearest first. Cells the
   surface passes through are found per (i, j) column from the sphere equation.
3. **The game makes each one once.** `Lscen_drive`, called from `Lvirt_a` once per run of the object loop, calls
   `Call_ScenDrive` (the player's ship, flight's object table) and then `Lscen_pick`, whatever the spawner's own rules
   (altitude, chance, flight mode, temporary-object cap) say: the spawner itself (`L61cf4`) now only jumps to
   `Lscen_idle`. `Lscen_pick` asks `Call_ScenNext` for the next cell and gets a
   ray (origin 2 cells above the point, straight down) plus the two numbers the spawner's random calls return; the
   spawner's own code then makes the object: kind, matrix to the surface, height of the hull. Both its success exits
   (`l61ee0`, `l61f06`: near a starport it makes cloud fields) call `Lscen_made`: the host copies the 286 byte object
   (`Call_ScenMade`) and the game deletes it (`474(a5)`), so no slot stays used. Up to 8 a frame.
4. **Each frame the copies are drawn.** `Lvirt_a`, at the start of `L44480`, asks `Call_VirtNext` for the copies whose
   position is within the radius of the camera (same parent frame, flight's object table, the planet's seed matches) and
   runs each through `L390f2` from a buffer (`Lvirt_obj`), as the loop does for objects. So the game's own rules apply
   unchanged: the distance exponent less the object's size (`88(obj)`), the model's dot (`+$14`) and gone (`+$10`)
   limits, the `if z` detail tests, off-screen culling. With the native scene on, `gl_native.cpp` captures them like any
   object (each copy has its own address, `0xf0000000 + index + 1`); without it the game draws them.
5. **Eviction.** A copy farther than 1.3 times the radius from the ship is dropped (and its cell may be made again).

Nothing depends on the direction of travel: a copy exists if it is within the radius of the ship, and is shown by the
model's own limits. `scenery_radius_km` (glfrontier.cfg, default 80, 0 = none) is the one number; at 80 km there are
about 50-70 copies. The object cap and `scenery_rear_limit` no longer matter for these (real objects are deleted at once).

## 2a. Sea: clouds only

Over the planet's sea only cloud fields are made. `Call_ScenNext` asks `planet_is_sea` (`gl_planet.c`; it keeps the
surface data the planet draw captured, per planet object, and `planet_surface_is_sea` in `gl_planet_tex.c` looks the
point up in the same feature-code grid the native texture is made from) whether the cell's point is in the base
region of a planet that has outlines (code 0, the colour the continents sit in). If so, the kind becomes the first
entry of the planet type's row (`Lscen_rows`, the game's kind table) whose model is 99 (`$c6`, clouds); a row with
none makes no feature there. Planets without outlines are not tested. The direction is the point's game
position as it is (checked on Earth: London and 20 E on the equator are land, 20 W is sea; a z flip, as `exact_view`
uses, gave sea at London). [inferred] That code 0 is the liquid on every planet with outlines is not checked.

## 2b. Two layers: land and drifting clouds (2026-11-05, untested in game)

- **Land layer**: as before (fixed in the planet's frame, hash of planet and cell, 1 cell in 48), but now only the row's non-cloud
  kinds, and nothing over liquid sea (`planet_is_sea`). A land cell that would have been a cloud is no feature.
- **Cloud layer**: its own hash (`seed` mixed with `0xc10d`), 1 cell in 48 (`CLOUD_DENSITY`), anywhere, sea or land; the kind is
  the first model-`$c6` entry of the planet type's row (a row with none, the airless types, has no clouds). Its cells are in a
  **wind frame**: the planet's frame turned about a fixed axis (from the planet's hash) by `wind * game time / radius`
  (`scenery_cloud_wind_mps`, cfg `cloud_wind_mps`, default 40 m/s; 0 = still; the clock is the day count at `A6+528` and time
  of day at `A6+524`). `scan_layer` looks for cells around the ship's place turned back into that frame, so the windward cells
  come into range by themselves and the same game time gives the same sky. The ray is cast at the cell's present place;
  the copy is then turned back into the wind frame (position and matrix, `turn_obj`) and turned forward again each frame when
  it is handed to the object loop, so a cloud is a rigid turn about the planet's centre and keeps its height. Cloud shadows
  follow (they are part of the field model). Which of the matrix's rows or columns are the axes is found from the first
  objects made (`detect_mat`; `GLF_SCEN_LOG=1` prints it).
- Cloud fields the spawner makes near a starport (`l61f06`) from land cells are fixed in place: not kept (`Call_ScenMade` checks
  the model, `$c6`, of a land-layer object), so only the drifting layer makes clouds.
- Not checked: how the axis looks to the eye (the wind stands still at the axis' two poles), and the cost of the second scan.

## 3. Limits

- Cells are made every frame the object loop runs with the ship in flight and "dust and clouds" on, at any altitude and in
  any flight mode (before 2026-11-03 the spawner's own gates, such as the 20 km limit and the orbit mode, decided that).
  Above the radius there is nothing to make, since the L-infinity distance includes altitude.
- Near a starport (`893(a6)` bit 0) the game's spawner makes only cloud fields half the time and deletes anything that
  falls within its base-clearance test (`774(a5)`); that is the original rule, kept.
- The ray start is exact but the cast runs relative to the ship, so the hit differs by 10-20 m with where the ship was
  when the copy was made. A copy keeps its place until evicted.
- Planets over 16000 cells (67,000 km) radius get none; under 4 cells neither.
- Only the player's flight view draws copies: a camera not over the planet's frame, or the system map's object table,
  gets none.
- Cost: each drawn copy runs `L390f2` on the 68k side (a few thousand cycles) every frame; raise the radius with care on
  a slow machine.
