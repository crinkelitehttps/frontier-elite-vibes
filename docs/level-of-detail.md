# Level of detail: ships, stations, buildings, planets

How Frontier decides how much of an object to draw at a given distance, and where our native renderer
follows or departs from it. Written from `fe2/fe2.s` (the original game; `fe2_modded.s` has the same layout),
the model data (via `tools/fe2ShipBuilder/fe2core.cpp`) and our own code. Nothing here was measured in a
running game unless it says so.

Tags: **[code]** read from the disassembly, **[native]** our renderer (`src/gl/`), **[inferred]** a
conclusion that could be wrong.

## 1. Short version

The game has four layers of level of detail, from coarse to fine:

1. **Does the object exist?** Scenery (buildings, cities, the station crowd) is spawned near the player and
   removed again when its distance exponent passes a limit (`L61cf4`, `L759dc`). Section 6.
2. **Whole model, dot, or nothing?** Per object, `L390f2` works out a distance exponent `d7` and compares it with
   two words in the model's header: at or over `+$10` the object is not drawn at all; at or over `+$14` it is
   drawn as a plain disc of the right size and the header's colour (`l39092`). Section 3.
3. **Which parts of the model?** Inside the model's bytecode, `if z` instructions (ops `$0b`/`$0c`) jump over
   parts when the object is farther than a threshold. The thresholds scale with the **Shape detail** option.
   Section 4.
4. **Planets** draw their surface features by splitting coastline edges until they are a few pixels long on
   screen, with the split depth, the pixel limits and the number of atmosphere bands all set by the Shape detail
   option. Section 7.

All distances are powers of two: the game never takes a square root here, it uses the bit length of the largest
of |x|, |y|, |z| (or the depth along the camera, for the `if z` tests). So every threshold moves in factor-of-2
steps and depends a little on direction (up to x1.7 along a diagonal).

## 2. Units and the distance exponent

[code] Positions are 64-bit, in millimetres (the convention used across this repo, e.g. `Call_TargetRange`).
`L390f2` (called for every object by the object loop `L44480`) gets the object's position relative to the
camera from `L3901a`, ORs the absolute values of x, y and z together, doubles that, and counts how many times it
must halve it to get under `$4000`. Then:

```
d7 = exponent - 88(obj)          stored in 95(obj)
```

`88(obj)` is a per-object adjustment. It is 0 for ordinary ships and is set for explosions/debris (from the
dying model's scale, `L44480` near `l4452a`) and a few other objects (`L61cf4`, `$4b...`). [inferred] For most
objects d7 depends on distance alone.

Because of the doubling and the two code paths (32-bit and 64-bit), the threshold for "d7 >= N" is:

| N | distance (largest coordinate) | N | distance |
|---|---|---|---|
| 6 | 262 m | 18 | 2,147 km |
| 7 | 524 m | 19 | 4,295 km |
| 8 | 1.0 km | 20 | 8,590 km |
| 9 | 2.1 km | 21 | 17,180 km |
| 10 | 4.2 km | 22 | 34,360 km |
| 11 | 8.4 km | 23 | 68,700 km |
| 12 | 16.8 km | 24 | 137,000 km |
| 13 | 33.6 km | 25 | 275,000 km |
| 14 | 67 km | 26 | 550,000 km |
| 15 | 134 km | 30 | 0.06 AU |
| 16 | 268 km | 35 | 1.9 AU |
| 17 | 537 km | 40 | 60 AU |

(N <= 17: 2^(12+N) mm; N >= 18: 2^(13+N) mm. The step from 17 to 18 is a factor of 4, an artefact of the
two code paths.) This matches the "Target out of range at about 10000 km" finding in
`docs/original-game-fixes.md` section 11: small ships have `+$10` = 20, which is 8,590 km.

`95(obj)` keeps the last d7, so other code can use it without recomputing: the scenery despawn test reads it
(section 6).

## 3. Per object: full model, disc, or nothing (`L390f2`)

The model header (byte offsets from the model start; `fe2core.h` has the full layout):

| offset | in `L390f2` | meaning |
|---|---|---|
| `+$0a` | `10(a5)` -> `-208(a6)` | scale `s`: a vertex is `v * 2^s` mm |
| `+$0c` | `12(a5)` -> `-192(a6)` | scale2: extra shift for big models (planets); also juggled for precision when far |
| `+$0e` | `14(a5)` | radius (model units): used for the disc size and the off-screen cull |
| `+$10` | `16(a5)` | **gone from**: d7 >= this -> not drawn |
| `+$12` | `18(a5)` | disc colour (low 12 bits, 444 RGB) and flags (high byte -> `-148(a6)`; bit 15 = "twinkly") |
| `+$14` | `20(a5)` | **dot from**: d7 >= this -> drawn as a disc |

[code] Order of tests in `L390f2`:

1. **Gone** (`cmp.w 16(a5),d7 / bge l390d6`, now inside our hook `Lrange_chk`): no screen position
   (`110(obj) = $80008000`), and `96(obj) = 1 << (d7 - limit)` (127 if that overflows), a "how far past"
   value. Our fix keeps targets drawn as a dot out to `target_range_au` (default 3 AU) by cutting d7 to
   `limit - 1` (fix 11).
2. Lighting and view-space position are worked out. `Lnative_clip` hook runs here (section 8).
3. **Behind the eye** (view z < `$40`): no screen position, but the full model path still runs (`l3940e` ->
   `L39290`). Close objects that straddle the eye need this.
4. Screen position stored in `110(obj)` (used by the HUD/target marker).
5. **Disc** (`cmp.w 20(a5),d7 / bge l39092`): diameter in pixels = `radius << s` * 2 * 256 / depth; drawn by
   `L3a68c` as a filled circle (`L3a68c` also handles the twinkly flag and the tiny sizes: under 9 pixels it
   uses small fixed shapes). No model code runs. Suns, far planets and far ships all end up here.
6. **Full model**: `L39290` builds the rotation, then `L393de` -> `L3e14e_SetupNProjectObj` runs the model's
   bytecode. Before that, `L39646_3DObjClipOffscreen` culls the whole object if its bounding sphere
   (`radius << s`) is outside the view.

Sub-models (ops `$0e`/`$1b`, `L3c928`) get their own off-screen test with their own bounding radius
(`L39658_3DClipOffscreen`) [code], so big models (stations, cities) skip parts that are off screen.

Typical values (full table in the appendix):

| kind | dot from | gone from |
|---|---|---|
| Sidewinder, Adder, Cobra Mk I (s=9) | 11 ≈ 8.4 km | 20 ≈ 8,590 km |
| Cobra Mk III | 11 ≈ 8.4 km | 23 ≈ 68,700 km |
| Anaconda (s=12) | 14 ≈ 67 km | 23 |
| Missiles | 9 ≈ 2.1 km | 18 ≈ 2,147 km |
| Model 43 (big station-like model, 30 sub-models) | 16 ≈ 268 km | 24 ≈ 137,000 km |
| Models 45-58 (bases; `docs/system-generation.md` says bases are byte ids `$5a..$6c` = models 45-54) | 16-20 | 21-23 |
| Planets (61-76) | 23-35 | 36-50 |
| Stars (77-86) | 32-41 | 63-72 (always drawn) |

Note that a ship within 8 km is a full model and beyond it is a disc; whatever detail a model has is all
decided inside that first 8 km by the `if z` tests below.

## 4. Inside the model: `if z` tests (ops `$0b`, `$0c`)

[code] `L3ca9c` (op `$0b`) and `L3cafa` (op `$0c`). The instruction's second word is either a normal test
(bit 15 set: "skip unless this normal faces the camera", back-face culling of a part, not level of detail) or
a distance K:

```
shift = (K even ? optdetail1 : optdetail2) + s      (s = -208(a6), the model's scale)
limit = K << shift                                   (overflow -> treated as "not beyond")
beyond = depth > limit                               depth = -8(a6), the view-space z of the model's origin
$0b: jump (skip the block) if beyond                 "if > z"  - draw only when near
$0c: jump if not beyond                              "if < z"  - draw only when far (low-detail stand-ins)
```

- The distance is the **depth along the game camera** (z), not the true distance. Turning the view changes it:
  the native code notes that pitching 20° at Merlin made the cloud field (model 99) drop its smaller clouds.
- Behind the camera (depth < 0): `$0b` never skips, `$0c` always skips.
- **Odd K uses `optdetail2`, even K uses `optdetail1`.** Model authors chose per test how strongly it reacts
  to the detail option (section 5): odd ones move twice as far per option step.
- In millimetres, at Medium detail (shift = s) the limit is `K * 2^s`. Example, Cobra Mk III (s = 9):
  `> 1170` = 600 m; at the default (highest) detail it becomes 1.2 km (even K, x2), `> 584` similarly.
- Pairs `> K` / `< K` with the same K (e.g. models 134-136, 153, 164) are a switch between a near and a far
  version of the same part.

Other size-based choices in the bytecode [code + native]:

- **circle** (op `$01`, `L3a558`) and **blobs** (`L3ab42`): when the disc is small on screen (depth / 8 >
  its size) it is drawn as a whole number of pixels; larger, as a true-size disc. Lighting rules also differ
  between the two (`gl_native.cpp` `circle()`).
- **ifdist** (op `$17`): "how far apart are two vertices on screen". No model in the main table uses it
  (survey), so it can be ignored for now.
- **Variables and temporaries** (ops `$13`/`$14`, `calc`) also hide parts (landing gear, damage, city
  lights); not distance related.

## 5. The Shape detail option

[code] Four buttons on the options screen (key codes `$81..$84`) pick a long from `L7261c` into
`A6_opt_shape_detail` (`10434(a6)`); `L725d4_SetDetailOpts` turns it into three words:

| button | `optdetail1` (even K) | `optdetail2` (odd K) | `optdetail3` (atmos bands) | even-K reach | odd-K reach | planet split counter |
|---|---|---|---|---|---|---|
| 1st (`$81`), lowest | -2 | -4 | `$4200` | x1/4 | x1/16 | 12 |
| 2nd (`$82`) | -1 | -2 | `$4100` | x1/2 | x1/4 | 14 |
| 3rd (`$83`) | 0 | 0 | `$4030` | x1 | x1 | 16 |
| 4th (`$84`), **default** | 1 | 2 | 0 | x2 | x4 | 18 |

The defaults (`L7228e_opts_defaults`) are the 4th button, the highest detail. The words live at
`L60d2_optdetail1`, `L60d4_optdetail2`, `L60d6_optdetail3` (and `540/542/544(a6)`), and are read by:

- the `if z` tests (section 4);
- the planet: split counter `172(a3) = 16 + optdetail2`, the dark/light shading shift `6 - optdetail2`
  (`l3d168` area), and atmosphere bands (`l3d8da`: a band whose scale `202(a3)` is <= `optdetail3` is
  skipped; `$4000` = 1.0, so lower settings drop the thinnest bands);
- `L427xx` (around `l427ce`) saves `optdetail1`, clears it while drawing something, and restores it.
  [inferred] a view drawn at fixed detail (the ship view on a screen, or similar); not looked at closely.

The option does not change the object-level dot/gone limits (section 3) or the scenery spawn rules.

## 6. Object existence: scenery spawn and despawn

[code] (details in the issue 3 notes) Below 20 km over a planet, `L61cf4` spawns ground objects (buildings,
the crowd of ships parked at a starport) at random ray hits on the planet, about 20% of frames, while fewer
than 15 temporary objects exist. Each one's module (`L759dc`) removes it when `95(obj) >= $f` (d7 >= 15, about
134 km of largest coordinate). There is no fade: objects pop in at the spawn point and out at the limit.

The issue 3 work-in-progress (cap 40, try rate `$8000`, despawn `$12`, spawn direction biased ahead) is in
`git stash@{0}`, not in the current asm.

## 7. Planets

**Far**: a disc via section 3, using the header colour. For Earth-like model 72 that is beyond ≈ 275,000 km.

**Nearer**: the model's planet command (op `$1f`, `L3cd9c_ProjectPlanet`) draws a 2D shape: the outline
(`L3d6d0_PutPlanetCircle`), surface features (`L3d452_PlanetFeatureLoop`), rings (`L3d648_PutPlanetRings`),
atmosphere bands (`L3da2e`), shading.

[code] Feature outlines are refined by recursive midpoint splitting (`L3ddc4` / `L3de2e`):

- The counter `172(a3)` starts at `16 + optdetail2` (18 at default) and drops by 2 per level.
- An on-screen edge is split while its screen length `|dx| + |dy|` is greater than `L3de18[counter / 2]`:

  | counter | 18 | 16 | 14 | 12 | 10 | 8 | 6 | 4, 2, 0 |
  |---|---|---|---|---|---|---|---|---|
  | split if longer than (px) | 4 | 4 | 5 | 12 | 15 | 20 | 20 | never |

  So at the default an edge is cut down to about 4 pixels over up to 7 levels; at the lowest setting it starts
  at counter 12 and stops at 12-20 pixels after 4 levels.
- Off-screen edges keep splitting only as far as needed for clipping (down to counter -20, `l3df3c`).
- Each midpoint is pushed out by a pseudo-random amount that halves each level; the generator is restored
  around each split, so the coastline is the same every frame; only how deep it goes depends on the view
  (`gl_planet_tex.c` has the C version). The displacement stops once `174(a3) - 172(a3) >= 22`.

So planet detail is screen-space adaptive, unlike ships, whose detail is set by fixed distances.

**At the surface**: when the view is inside the planet's disc the game falls back to a flat fill of the
ground colour; buildings and starports are separate objects (section 6 and the bases in section 3).

[native] `gl_planet.c` draws planets as ray-traced spheres, and `gl_planet_tex.c` decodes the features once
into a 2048x1024 texture with a fixed 6 levels of splitting. No mesh, no view-dependent detail, so no
popping; the cost is that close up the coastline is only as sharp as the texture.

## 8. The native renderer

[native] `src/gl/gl_native.cpp` replays model bytecode in 3D. It keeps the game's rules where they matter for
matching the game:

- **Disc bodies**: hook `Lnative_clip` -> `Nu_NativeFar` catches every object that `L390f2` would draw as a
  disc (d7 >= `+$14`, z >= `$40`) and records it as a "far body" (`Frame.far`, max 64) with its true radius
  (`radius << (s+1)`), colour, light. It draws these itself with a real size.
- **Culled objects**: `Nu_NativeCulled` lets it keep objects the game would cull as off screen
  (`L39646`), since the cockpit and VR see more than the game's view.
- **`if z`**: `beyond()` uses the same formula and the same `optdetail` words, plus `native_detail_boost`
  (glfrontier.cfg, cog menu Detail slider, default 2 = parts kept 4x farther, 0 = the game's distances), but measures the **straight
  distance** to the sub-model origin (`len(r.org)`) rather than the camera depth. That removes the
  turn-your-head popping but means our choice can differ from the classic view at the edge of the screen.
- **circle/blob**: the same small (pixel) / large (true size) switch.
- **ifdist**: implemented (`apart()`, assuming the game's 168-pixel-high 36.5° view), though no model uses it.
- **Planets**: drawn with the sky (`draw_fars`), see section 7.
- **Drawing order** (not level of detail, but it decides what hides what): the game keys parts along its camera;
  `sort_nodes_view` keys them again along the view being drawn (as the old cockpit passes did) and places
  underlays among the parts by those keys. Keying along the game's forward camera made Lave Station's
  scaffold (model 141, outside the hull) show through the bay walls when looking sideways.

Things to keep in mind when changing level of detail:

- In the classic cockpit build the world is drawn by running the game's engine several times with the camera
  turned (`gl_cockpit.c` passes). Every rule above that uses camera depth or screen size (the `if z` tests,
  planet splitting, the disc switch) can give different answers in neighbouring passes, which shows as seams.
- On the Quest the native scene is CPU-bound (bytecode replay per frame, see the Quest perf notes), so raising
  detail there costs CPU, not GPU.
- Raising `+$14` (dot from) for ships would let full models show farther out; the bytecode's own `if z` tests
  would then still drop the small parts at their usual distances.

## 9. Open questions

- What `88(obj)` holds for stations, bases and planets (if nonzero, the distances in the appendix shift by
  that many factors of 2). Easy to check by dumping objects from a save.
- Which models are orbital stations vs ground bases vs cities (models 43-58, 94-104 and 141-169 look like
  the candidates by size and sub-model count).
- Whether `-8(a6)` is the sub-model's own depth in the game's `if z` tests (the native renderer assumes yes,
  and has matched the game at Mars, Merlin and Lave).
- What the `optdetail1` clear in `L427xx` is for.

## Appendix: per-model numbers

Generated by `tools/fe2ShipBuilder/lod_survey.cpp` (build line at the top of the file); only models whose
`+$10` is a real distance (< 100) are listed. Others (sub-models, the sky, effects) have junk there and are
only drawn as parts of other models. Distances use the table in section 2 and assume `88(obj) = 0`. `s` is the
scale (`+scale2` for planets). The `if z` tests are top-level code only; each sub-model has its own row.

| model | name | s | radius | dot from (`+$14`) | gone from (`+$10`) | `if z` tests (K) |
|---|---|---|---|---|---|---|
| 3 | Mine | 5 | 65 | 7 ≈ 524 m | 16 ≈ 268 km | <1874 |
| 4 | Mine | 4 | 93 | 6 ≈ 262 m | 15 ≈ 134 km |  |
| 5 | Missile | 7 | 117 | 9 ≈ 2.1 km | 18 ≈ 2,147 km |  |
| 6 | Smart Missile | 7 | 117 | 9 ≈ 2.1 km | 18 ≈ 2,147 km |  |
| 7 | Naval Missile | 7 | 117 | 9 ≈ 2.1 km | 18 ≈ 2,147 km |  |
| 8 | Nuclear Missile | 7 | 117 | 9 ≈ 2.1 km | 18 ≈ 2,147 km |  |
| 9 | Escape Capsule | 5 | 117 | 7 ≈ 524 m | 25 ≈ 274,878 km | >23436 |
| 10 | Interplanetary Shuttle | 6 | 75 | 8 ≈ 1.0 km | 17 ≈ 537 km | >1000 >500 <500 >1500 >874 |
| 11 | Lifter | 7 | 68 | 9 ≈ 2.1 km | 18 ≈ 2,147 km | >312 >624 >592 >406 >4686 |
| 12 | Osprey attack fighter | 8 | 117 | 10 ≈ 4.2 km | 19 ≈ 4,295 km | >936 >468 |
| 13 | Falcon attack fighter | 8 | 119 | 10 ≈ 4.2 km | 19 ≈ 4,295 km | >424 >2124 >530 <530 |
| 14 | Hawk airfighter | 7 | 105 | 9 ≈ 2.1 km | 18 ≈ 2,147 km | >1756 |
| 15 | Kestrel airfighter | 8 | 64 | 10 ≈ 4.2 km | 19 ≈ 4,295 km | >1170 >878 >584 |
| 16 | Eagle long range fighter | 8 | 125 | 10 ≈ 4.2 km | 19 ≈ 4,295 km | >2186 >780 >406 >1170 |
| 17 | Eagle Mk2 fighter | 8 | 125 | 10 ≈ 4.2 km | 19 ≈ 4,295 km |  |
| 18 | Eagle Mk3 fighter | 8 | 125 | 10 ≈ 4.2 km | 19 ≈ 4,295 km |  |
| 19 | Sidewinder | 9 | 97 | 11 ≈ 8.4 km | 20 ≈ 8,590 km | >390 >324 |
| 20 | Krait Assault Craft | 9 | 123 | 11 ≈ 8.4 km | 20 ≈ 8,590 km | >526 >702 |
| 21 | Gecko | 9 | 94 | 11 ≈ 8.4 km | 20 ≈ 8,590 km | >714 >584 |
| 22 | Adder | 9 | 109 | 11 ≈ 8.4 km | 20 ≈ 8,590 km | >648 >546 |
| 23 | Viper Defence Craft | 10 | 63 | 12 ≈ 17 km | 21 ≈ 17,180 km | >340 >292 |
| 24 | Cobra Mk I | 9 | 113 | 11 ≈ 8.4 km | 20 ≈ 8,590 km | >742 >390 |
| 25 | Moray Starboat | 10 | 107 | 12 ≈ 17 km | 21 ≈ 17,180 km | >510 >268 |
| 26 | Cobra Mk III | 9 | 109 | 11 ≈ 8.4 km | 23 ≈ 68,719 km | >1170 >584 >584 |
| 27 | Constrictor | 11 | 75 | 13 ≈ 34 km | 22 ≈ 34,360 km | >462 >244 |
| 28 | Asp Explorer | 9 | 117 | 11 ≈ 8.4 km | 20 ≈ 8,590 km | >742 >780 |
| 29 | Transporter | 10 | 117 | 12 ≈ 17 km | 21 ≈ 17,180 km | >742 >390 |
| 30 | Lion Transport | 10 | 125 | 12 ≈ 17 km | 21 ≈ 17,180 km | <878 >340 |
| 31 | Tiger Trader | 10 | 126 | 12 ≈ 17 km | 21 ≈ 17,180 km | >1206 >634 |
| 32 | Imperial Courier | 10 | 78 | 12 ≈ 17 km | 22 ≈ 34,360 km | >416 >416 |
| 33 | Python Freighter | 11 | 101 | 13 ≈ 34 km | 22 ≈ 34,360 km | >380 >316 |
| 34 | Imperial Trader | 10 | 70 | 12 ≈ 17 km | 22 ≈ 34,360 km |  |
| 35 | Anaconda Freighter | 12 | 63 | 14 ≈ 67 km | 23 ≈ 68,719 km | >292 >244 |
| 36 | Puma Clipper | 11 | 97 | 13 ≈ 34 km | 22 ≈ 34,360 km | >366 <488 >488 <316 >170 >488 |
| 37 | Boa Freighter | 10 | 126 | 12 ≈ 17 km | 21 ≈ 17,180 km | >1170 >976 |
| 38 | Panther Clipper | 10 | 117 | 12 ≈ 17 km | 21 ≈ 17,180 km |  |
| 39 |  | 11 | 126 | 13 ≈ 34 km | 22 ≈ 34,360 km |  |
| 40 | Lynx Bulk Carrier | 12 | 73 | 14 ≈ 67 km | 23 ≈ 68,719 km |  |
| 41 | Long-range Cruiser | 15 | 100 | 17 ≈ 537 km | 26 ≈ 549,756 km | >732 |
| 42 |  | 12 | 117 | 14 ≈ 67 km | 22 ≈ 34,360 km | >682 >48 <488 >488 >683 |
| 43 |  | 13 | 123 | 16 ≈ 268 km | 24 ≈ 137,439 km | >18310 >462 >72 >3662 >1221 >658 <462 >123 >391 >123 >658 >1953 >977 |
| 44 |  | 16 | 90 | 18 ≈ 2,147 km | 27 ≈ 1.1 M km |  |
| 45 |  | 17 | 76 | 17 ≈ 537 km | 21 ≈ 17,180 km |  |
| 46 |  | 17 | 102 | 17 ≈ 537 km | 21 ≈ 17,180 km | >3662 >2058 |
| 47 |  | 18 | 76 | 18 ≈ 2,147 km | 21 ≈ 17,180 km | >1524 |
| 48 |  | 18 | 117 | 20 ≈ 8,590 km | 21 ≈ 17,180 km | >2288 >1830 >1372 |
| 49 |  | 16 | 68 | 18 ≈ 2,147 km | 23 ≈ 68,719 km |  |
| 50 |  | 18 | 121 | 20 ≈ 8,590 km | 21 ≈ 17,180 km | >2174 >4576 >1830 |
| 51 |  | 16 | 122 | 16 ≈ 268 km | 21 ≈ 17,180 km |  |
| 52 |  | 17 | 91 | 17 ≈ 537 km | 22 ≈ 34,360 km | >9154 |
| 53 |  | 18 | 68 | 19 ≈ 4,295 km | 22 ≈ 34,360 km |  |
| 54 |  | 18 | 102 | 19 ≈ 4,295 km | 21 ≈ 17,180 km | >2288 |
| 55 |  | 15 | 76 | 18 ≈ 2,147 km | 21 ≈ 17,180 km |  |
| 56 |  | 15 | 122 | 18 ≈ 2,147 km | 21 ≈ 17,180 km |  |
| 57 |  | 15 | 97 | 18 ≈ 2,147 km | 21 ≈ 17,180 km |  |
| 58 |  | 15 | 97 | 18 ≈ 2,147 km | 21 ≈ 17,180 km |  |
| 59 |  | 19 | 124 | 21 ≈ 17,180 km | 20 ≈ 8,590 km | <200 <142 <56 |
| 60 | Long-range Cruiser | 7 | 97 | 14 ≈ 67 km | 27 ≈ 1.1 M km | >1562 |
| 61 | (planet command) | 20 | 95 | 19 ≈ 4,295 km | 34 ≈ 0.94 AU |  |
| 62 | Long-range Cruiser | 11 | 97 | 13 ≈ 34 km | 21 ≈ 17,180 km | >2440 |
| 63 | (planet command) | 22+3 | 66 | 27 ≈ 1.1 M km | 38 ≈ 15 AU | <1788 |
| 64 | (planet command) | 22+2 | 89 | 23 ≈ 68,719 km | 39 ≈ 30 AU | >3576 |
| 65 | (planet command) | 22+2 | 108 | 23 ≈ 68,719 km | 36 ≈ 3.8 AU |  |
| 66 | (planet command) | 22+4 | 90 | 25 ≈ 274,878 km | 37 ≈ 7.5 AU |  |
| 67 | (planet command) | 22+3 | 90 | 24 ≈ 137,439 km | 36 ≈ 3.8 AU |  |
| 68 | (planet command) | 22+3 | 102 | 24 ≈ 137,439 km | 38 ≈ 15 AU |  |
| 69 | (planet command) | 22+4 | 91 | 25 ≈ 274,878 km | 40 ≈ 60 AU |  |
| 70 | (planet command) | 22+4 | 76 | 25 ≈ 274,878 km | 40 ≈ 60 AU |  |
| 71 | (planet command) | 22+4 | 96 | 25 ≈ 274,878 km | 40 ≈ 60 AU |  |
| 72 | (planet command) | 22+4 | 96 | 25 ≈ 274,878 km | 39 ≈ 30 AU | >4470 >2086 |
| 73 | (planet command) | 22+9 | 118 | 34 ≈ 0.94 AU | 46 ≈ 3.9e+03 AU | <976 >976 |
| 74 | (planet command) | 22+8 | 118 | 33 ≈ 0.47 AU | 44 ≈ 9.6e+02 AU | <976 >976 |
| 75 | (planet command) | 22+10 | 118 | 35 ≈ 1.9 AU | 46 ≈ 3.9e+03 AU | <500 >500 |
| 76 | (planet command) | 22+11 | 118 | 34 ≈ 0.94 AU | 50 ≈ 6.2e+04 AU | <476 >476 |
| 77 | (planet command) | 22+11 | 82 | 32 ≈ 0.24 AU | 63 ≈ 5.1e+08 AU |  |
| 78 | (planet command) | 22+17 | 91 | 38 ≈ 15 AU | 69 ≈ 3.2e+10 AU |  |
| 79 | (planet command) | 22+11 | 117 | 32 ≈ 0.24 AU | 63 ≈ 5.1e+08 AU |  |
| 80 | (planet command) | 22+12 | 76 | 33 ≈ 0.47 AU | 64 ≈ 1e+09 AU |  |
| 81 | (planet command) | 22+12 | 117 | 33 ≈ 0.47 AU | 64 ≈ 1e+09 AU |  |
| 82 | (planet command) | 22+14 | 73 | 35 ≈ 1.9 AU | 66 ≈ 4e+09 AU |  |
| 83 | (planet command) | 22+15 | 117 | 36 ≈ 3.8 AU | 67 ≈ 8.1e+09 AU |  |
| 84 | (planet command) | 22+19 | 68 | 40 ≈ 60 AU | 71 ≈ 1.3e+11 AU |  |
| 85 | (planet command) | 22+20 | 68 | 41 ≈ 1.2e+02 AU | 72 ≈ 2.6e+11 AU |  |
| 86 | (planet command) | 22+17 | 91 | 38 ≈ 15 AU | 69 ≈ 3.2e+10 AU |  |
| 87 |  | 22+11 | 118 | 35 ≈ 1.9 AU | 58 ≈ 1.6e+07 AU |  |
| 89 |  | 19 | 82 | 21 ≈ 17,180 km | 17 ≈ 537 km | <64 >2561 >1601 |
| 90 |  | 22+8 | 93 | 80 ≈ 6.6e+13 AU | 80 ≈ 6.6e+13 AU |  |
| 91 |  | 22+8 | 93 | 80 ≈ 6.6e+13 AU | 80 ≈ 6.6e+13 AU |  |
| 92 | Long-range Cruiser | 4 | 104 | 6 ≈ 262 m | 15 ≈ 134 km | >1500 >1500 >1250 <1250 |
| 93 |  | 7 | 93 | 10 ≈ 4.2 km | 47 ≈ 7.7e+03 AU | >2342 >1874 >1406 >1124 >2342 >1874 >1406 >1124 |
| 94 |  | 14 | 100 | 16 ≈ 268 km | 18 ≈ 2,147 km |  |
| 95 |  | 17 | 64 | 19 ≈ 4,295 km | 18 ≈ 2,147 km | >6 |
| 96 |  | 0 | 70 | 6 ≈ 262 m | 6 ≈ 262 m |  |
| 97 |  | 7 | 70 | 9 ≈ 2.1 km | 18 ≈ 2,147 km |  |
| 99 |  | 16 | 91 | 18 ≈ 2,147 km | 27 ≈ 1.1 M km | >1738 >868 |
| 100 |  | 9 | 97 | 11 ≈ 8.4 km | 16 ≈ 268 km |  |
| 101 |  | 9 | 125 | 11 ≈ 8.4 km | 21 ≈ 17,180 km | <10000 <4874 |
| 102 |  | 15 | 102 | 17 ≈ 537 km | 27 ≈ 1.1 M km | <2746 |
| 103 |  | 17 | 80 | 19 ≈ 4,295 km | 29 ≈ 4.4 M km | <1906 |
| 104 |  | 17 | 76 | 19 ≈ 4,295 km | 29 ≈ 4.4 M km | <2136 |
| 108 |  | 7 | 78 | 9 ≈ 2.1 km | 18 ≈ 2,147 km | <1562 >780 |
| 113 |  | 7 | 117 | 9 ≈ 2.1 km | 18 ≈ 2,147 km | >2928 >1170 |
| 114 |  | 7 | 87 | 9 ≈ 2.1 km | 18 ≈ 2,147 km | >2342 >1170 >526 |
| 117 |  | 7 | 82 | 9 ≈ 2.1 km | 18 ≈ 2,147 km |  |
| 120 |  | 6 | 70 | 8 ≈ 1.0 km | 17 ≈ 537 km |  |
| 129 |  | 9 | 125 | 11 ≈ 8.4 km | 20 ≈ 8,590 km | >624 |
| 131 |  | 10 | 117 | 12 ≈ 17 km | 21 ≈ 17,180 km | >584 |
| 132 |  | 10 | 117 | 12 ≈ 17 km | 21 ≈ 17,180 km |  |
| 133 |  | 9 | 76 | 11 ≈ 8.4 km | 20 ≈ 8,590 km | >350 |
| 138 |  | 11 | 68 | 13 ≈ 34 km | 22 ≈ 34,360 km | >194 >293 |
| 141 |  | 14 | 73 | 16 ≈ 268 km | 25 ≈ 274,878 km | >456 >1830 >914 >456 >228 >914 >456 >228 <456 >1830 >456 >456 |
| 148 |  | 14 | 122 | 16 ≈ 268 km | 25 ≈ 274,878 km | >304 >60 >976 >732 >366 >244 |
| 149 |  | 13 | 68 | 15 ≈ 134 km | 24 ≈ 137,439 km | >976 >72 >3662 >781 >245 |
| 150 |  | 14 | 85 | 16 ≈ 268 km | 25 ≈ 274,878 km |  |
| 151 |  | 15 | 73 | 17 ≈ 537 km | 26 ≈ 549,756 km | >366 >182 >90 |
| 152 |  | 14 | 91 | 16 ≈ 268 km | 25 ≈ 274,878 km | <1830 <1280 <548 >182 |
| 156 |  | 13 | 90 | 15 ≈ 134 km | 25 ≈ 274,878 km | <3662 <1220 >610 |
| 164 |  | 15 | 70 | 17 ≈ 537 km | 26 ≈ 549,756 km | <122 >122 |
| 179 |  | 12 | 102 | 14 ≈ 67 km | 23 ≈ 68,719 km | >1220 |
| 186 |  | 11 | 87 | 13 ≈ 34 km | 22 ≈ 34,360 km | >2196 |
| 208 |  | 11 | 65 | 13 ≈ 34 km | 22 ≈ 34,360 km | <1708 |
| 209 |  | 12 | 80 | 14 ≈ 67 km | 23 ≈ 68,719 km |  |
