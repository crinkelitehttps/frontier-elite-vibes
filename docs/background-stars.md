# Background stars: what decides whether they are drawn

Scope: the stars and Milky Way seen from a ship flying outside any atmosphere. Facts are marked **[code]** (read in
`fe2/fe2.s` or the host source) or **[inferred]**. Nothing here was changed; this is a survey.

## 1. What the stars are

- One fixed model: game-data table 1 (`FE2_gamedata1`), model **88** (the object's model byte offset is `$b0`; index =
  offset / 2; `SKY_MODEL` in `gl_native.cpp`). `mdump 88` (devtools/models): 63 vertices (37 type `$01`, 25 type
  `$0d`, 1 type `$05`), 7 normals, radius 64. Its bytecode is `if < normal:N` / `if > normal:N` tests that pick
  parts by the direction they face, and each part is either a star dot (sub-model 192, a 15-vertex dot with a
  brightness/colour index in its op, e.g. `7f02`, `7f0a`, `7f58`) or a Milky Way blob (`complex #106`, bezier
  outlines with a colour in the op). **[code]**
- The model is **the same in every system**. Nothing about the star field depends on the galaxy, the system, the
  star type or the ship's position, so there is no parallax and no per-system variation. **[code]**
- Orientation: `L44200_DrawBGStars` (`fe2.s:26302`, reached as `jsr 612(a5)` = `A5_DrawBGStars`) copies the camera's
  rotation matrix (`0(a3)`, or after `L38d66_MatrixMulWTF` the second matrix at `70(a3)` when `69(a3)` is set) into a
  scratch object on the stack, sets its position to (0, 0, `$200`), model `$b0`, and draws it with
  `L39426_PutGameData3DObj`. So the stars are 512 units straight ahead of the camera and turn with the ship's
  orientation only. **[code]**
- Draw order: the sky is drawn first in the picture, before planets and ships, so everything else covers it. It is
  not depth tested. **[code]** (native: `native_draw_sky` runs it "under everything").

## 2. The three gates, in the order the game applies them

All are in the hook `Lcockpit_world` (`fe2.s:66423`, replacing the code at the end of the flight camera setup
`L62b…`, call at `fe2.s:41610`; the same in `fe2_modded.s:67361`). Per pass (the straight-ahead pass in the
original game, several in the cockpit view):

```
A6_plr_in_atmosphere(a6) -> saved
if A6_opt_bg_stars == 0:  skip the stars            (gate 1)
elif A6_plr_in_atmosphere < 0: skip the stars       (gate 2)
else DrawBGStars
A6_plr_in_atmosphere := 0                           (re-armed for this pass's planets)
... objects (planets, ships) are drawn, which may set the flag again (gate 3)
```

### Gate 1: the "background stars" option

- `A6_opt_bg_stars` (`10441(a6)`, a byte). The options screen key `$86` toggles it (`eori.b #$ff`, `fe2.s:48523`,
  right after the "space dust, clouds" toggle `$85`). Zero means no stars and no Milky Way at all. **[code]**
- Not set by the host (no use in `src/`); it is part of the saved game memory, so it comes back with a loaded
  commander. **[code]** Whether the default is on was not checked.

### Gate 2: `A6_plr_in_atmosphere` (`546(a6)`, word; negative means no stars)

- Declared `L60d8_plr_in_atmosphere` / `FE2_plr_in_atmosphere`, `A6_IN_ATMOSPHERE 546` in `game_state.h`. **[code]**
- **Latency:** the flag is tested before the objects are drawn and cleared right after, so the test sees what the
  *previous* frame's (previous pass's) planet drawing left. One frame lag in the original game; in the cockpit the
  flag at entry is restored at the start of each pass (`2(a7)`), and after all passes the value left is the OR of
  what the passes set (`0(a7)`), so a flag set by any pass hides the stars in all passes next frame. **[code]**
- Only writer in the game: the end of `L3da2e_AtmosphereColNShit` (`fe2.s:17719`), `move.w #$ffff`. See gate 3.

### Gate 3: when `L3da2e` sets the flag

`L3da2e` is called from planet drawing, in two places: `l3cd84` (`fe2.s:16446`, inside `L3cd9c_ProjectPlanet`'s
size test) and `fe2.s:17561` (planet band construction). It decides the haze colour of the sky. It sets the flag
only when all of these hold: **[code]**

1. The planet is being drawn with an atmosphere record (`208(a3)` non-zero: the band's colour word; with zero,
   `l3cd8e_AbortPlanetDraw` skips the call).
2. The sunlight term picks the **lit** direction: index `d0 = -(light vector · planet direction)`, clamped to
   -8..6, and the code runs on only if `d0 < 0` (`tst.w d0; bpl` returns otherwise). The vector is `-198(a6)`
   (from `L39546_CalcRotViewNLight`) and `122(a3)` is the planet's direction in view space. [inferred] This is the
   day side: looking at an atmosphere's lit part gives a blue day sky and hides stars; its night side leaves stars.
3. `206(a3) >= 5`. This is `94(a3) - d1` in `L3cd9c_ProjectPlanet` (`fe2.s:16704`): the planet's size exponent
   minus the distance exponent, so it grows as the planet gets close; it is set to `$10` when the camera is inside
   (`l3cfca`). [inferred] "The planet is close enough that its air fills the view".

When it sets the flag it also writes the haze colour (`208(a3)+204(a3)`) into palette colour 14 of both palettes
(`L9cd6_main_pal1_col14`, `L9cf6_main_pal2_col14`). That is the sky background colour, and why the flag and the sky
colour travel together.

Also in `L3da2e` entry via `l3d274: cmp.w #$b2,d0; bge l3cd84`: the call from `L3cd9c_ProjectPlanet` happens only
when the planet's projected size is at least `$b2` (178 px, more than half the 320 wide view). [code] So a distant
planet never hides stars, even if it is the lit side.

## 3. What does *not* matter

- The ship's position, speed, system, the sun, or distance to any star. No fade with altitude: it is a hard on/off at
  the moment the planet's atmosphere reaches the thresholds above.
- Time acceleration, the weather or cloud setting, "space dust" (its own option, `A6_opt_dust_n_clouds`).
- Level of detail: stars (models 77-86 in `level-of-detail.md`) are *suns*, not the sky model. The sky model has no
  distance cutoff, as it is always 512 away.
- Being far from an atmosphere: the flag is only ever set by planet drawing, so in open space the stars are always
  drawn if gate 1 is on.

## 4. How the host mirrors it (native scene, cockpit)

- `gl_atmos.c`: while a planet's bands are made (`atmos_publish`-side code near line 273-290) the host reads
  `colour 14` and `A6_IN_ATMOSPHERE` after each band made by a pass that has the sky's colour (`set` and
  `ahead || !rec.sky_ahead`) and stores `sky` (colour) and `sky_air` (flag negative). Only a pass that really made
  the sky counts, otherwise frames flip between space's colour and the sky's (save "flicker", Merlin 500 m).
- `atmos_sky_hides_stars()` = `show.sky >= 0 && show.sky_air`.
- `native_draw_sky` (`gl_native.cpp:5186`) draws the kept sky object (captured by `sky_hook` the first time the
  game draws model 88 in a frame; `Lnative_culled` stops the game drawing it itself) unless
  `cockpit_active() && shown == &shows[0] && atmos_sky_hides_stars()`. The window clear colour is the sky colour in
  the same case (`gl_screen.c:896`).
- So in native mode the hide rule uses the frame's *published* flag, not the one-frame-late 68k test, except that the
  68k test is still what the game uses when native scene is off.
- Cockpit: the sky is placed 512 ahead of the game's own camera, expressed in each turned pass's axes
  (`Lcockpit_sky` hook, `Call_CockpitSky`) so that the passes agree and the Milky Way has no seams.

## 5. Practical checklist, "why are there no stars?"

1. Options screen: "background stars" off (`10441(a6)`).
2. Close to a planet with an atmosphere, looking at its lit side so it fills more than about half the view
   (planet projection `>= $b2`): the flag goes negative, stars hidden (and sky colour used). Leaving that view makes
   them come back one frame later.
3. Planet with no atmosphere record (Moon, Mars-like thin cases, airless bodies): never hides stars.
4. A star tint or Milky Way missing but dots present: that is the model's own `if normal` tests (parts by direction),
   not a flag.

## 6. Not verified / open

- Exact meaning of the `208(a3)` word and what `206(a3)` holds when the planet is on the far path (`l3d02c`): it may
  be stale from an earlier call. The test only ever applies to planets that passed the `$b2` size test.
- Whether the lit/dark inference in gate 3 is right (the sign convention of `-198(a6)` was not traced). A test: fly
  at 100 km over Earth's night side with the planet filling the view and see whether stars stay.
- Option default value for `A6_opt_bg_stars` on a new commander.
