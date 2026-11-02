# Plan: sharp, steady coastlines that gain detail as you approach

Status 2026-11-02: phase 0 done (see its Findings); phase 1 implemented (untested visually, uncommitted); phases 0, 2, 3 not started. Background (data format, the fractal, the current texture
and shader): `docs/planet-textures.md`; read sections 3, 5 and 6 of it first.

## Goal

Keep the game's look (flat colours, hard coastlines, the game's own fractal), but:

1. **No blur**: the coast is a crisp line at any altitude, not a 20 km colour ramp (user screenshot
   2026-11-01: Earth at 25 km altitude shows the staircase of 20 km texels, smeared by the shader's 4-texel
   colour blend).
2. **No jiggle**: the coastline must not shift as you fly over it.
3. **More detail closer**: the coastline keeps refining as you approach, continuing the game's own
   deterministic fractal (the game does this too, screen-adaptively), down to tens of metres.
4. **Better Earth outlines** (later phase): more, better-placed points for Earth only, same style.

Rejected: a real-world Earth texture (would look unlike every other planet).

## Facts the plan relies on

- [code] The fractal is **depth-independent**: `split_edge()` in `gl_planet_tex.c` steps the generator once on
  entry, and a split saves/restores it round its two children, so after the left child the state is the same
  however deep the left side went. So an edge refined to level 12 near the camera and left at level 6 far away
  gives exactly the same points at the shared levels. Adaptive refinement is safe and needs no new randomness.
- [code] Displacement amplitude is `scale * 2^(-8 - detail - level)`, halving per level; the game stops
  displacing at about a 4 millionth of the radius (~25 m on Earth). Beyond that edges are straight.
- [code] Colour = palette[(code >> 2) & 7], code = XOR of feature codes. Only **code bits 2, 3, 4** matter.
  XOR filling per bit is an even-odd fill of the features having that bit, so **each bit is an independent
  two-region mask**. That makes a per-bit distance field exact even where three colours meet (land under an ice
  cap), with no "which two codes is this edge between" ambiguity.
- [code] The planet's orientation (`PlanetDesc.axes`, from `A6-36`) is 9 words in 1.15 fixed point; the
  planet object's own matrix is also words (`obj+0..17`, copied by `L39290`). One word step is 3e-5 rad,
  about **200 m on Earth's surface**.
- [measured] Model 72 is Earth only (galaxy catalog: other living worlds use model 69). So "replace model 72's
  outlines" means "replace Earth's".
- Android/Quest builds use GLES 3 (`GL_IS_GLES`): no `glLogicOp`, no geometry shaders. Keep to GLES 3.0
  features (RGBA8 textures, texelFetch, linear filtering, fwidth).
- Our renderer only. The game's own 2D planet (classic view with `GLF_NATIVE_PLANETS=0`, Ctrl-E software
  renderer) keeps the original data and look. No asm changes in any phase.

## Phase 0: measure and fix the jiggle (do first)

The likely cause is the word-precision orientation, but measure before fixing: a crisp edge (phase 1) will
make any jiggle **more** visible, not less.

1. Temporary logging (env `GLF_PLANET_LOG`, removed afterwards) in `record_native()` / `draw_sphere()`
   (`gl_planet.c`): each frame, the point under the eye in the planet's own axes:
   `local = A^T * normalize(-centre)` (with the z flip used for `uToLocal`), as lat/lon in double, and the
   frame-to-frame step converted to metres (x radius in km). Also log the camera-to-centre distance.
2. Hover and fly slowly at 25 km over a coast (`goto Earth 25` in a test instance; see "Testing").
   Expect smooth motion; a jiggle shows as steps of tens to hundreds of metres that don't follow the ship's
   speed, or as back-and-forth noise.
3. Likely fixes, in order of preference:
   - **Rebuild the orientation in double**: read the planet object's matrix words (`obj+0`) and the camera's,
     orthonormalise each in double (Gram-Schmidt on the 1.15 columns), multiply. Rounding per word stays, but
     the 16-bit rounding of the *product* (the game's `A6-36`) goes away.
   - If the object's own matrix steps (the game turns it by a small rotation each tick and re-rounds),
     **filter it**: keep our own double-precision orientation per planet object, advance it by the measured
     spin (rate from consecutive frames, averaged), and re-sync slowly to the game's matrix (a long time
     constant, a small correction per frame). Planets turn at a steady rate, so this is safe.
   - Also check `exact_distance()` (rescales the centre to radius x height ratio each frame): its rounding
     moves the ground in and out, not sideways; it should not cause sideways jiggle but log it anyway.
4. Note that the cockpit's turned passes (`pass_to_view_game`) rotate `axes` too; whatever is fixed must be
   applied in both `draw_planet()` and `planet_native_draw()` paths (`SphereIn.axes`).

**Findings and fix (2026-11-01/02, done, user-tested on saves `jiggle`, `jiggle2`, `jiggle3`):** the jiggle was **not** the
orientation words alone. The game's planet position (`PlanetDesc.pos`) is rounded (steps of ~8e-5 rad, hundreds of km at
Earth's centre) and its surface axes (`A6-36`) are rounded and skewed by ~3 mrad separately, so the point under the eye
jumped by tens to hundreds of km while the height stayed smooth, most when the ship pitched or turned.
- In a ship whose frame parent is the planet, the ship's position and matrix are in the **planet's own frame**, and the
  game's surface axes are the ship's matrix transposed (x ~0.985). So the sub-eye point is just the direction of the
  exact 64-bit position, and the matrix cancels if both the centre and the axes are built from the same one.
- `exact_view()` in `gl_planet.c` (called from `record_native`, own flight views only, ahead pass, parent == planet):
  builds the centre direction and the axes from the exact position and the ship's matrix made orthonormal in double
  (`cheat_object_axes`), picks the view (a signed permutation of the ship's axes) by matching the game's position
  **and** its axes, then turns both about the eye by the little (<= ~3.5 mrad) that puts the centre where the game's own
  camera has it (the ship's matrix leads or lags the camera by a frame while turning).
- Pitfalls found on the way: the game's position has z flipped against its axes (match in the axes' convention); matching
  on the position alone is ambiguous when two components of the direction are equal (inverted terrain on and off);
  never fall back to the game's skewed axes mid-turn.
- Test aids: script command `yaw DEG` (turns the ship about its up axis), saves `jiggle*`.
- Not done: the planet's own spin steps (16-bit words, ~190 km) were never seen; other views/planets/stand-in
  (remembered) planets not examined.

Done when: the logged sub-eye point moves smoothly (no steps above a metre or so) and the user sees no jiggle.

## Phase 1: crisp global texture (option 2: distance fields)

Implemented as: RGB8 texture (no alpha), distance by stamping the split segments within 4 texels (unsigned, byte per channel) with the sign from the existing even-odd fill at 2048x1024 (no supersampled masks, no EDT); circles get an analytic rim distance. Shader: `FS_SPHERE` per-bit `fwidth` coverage, 8 palette mixes. Built on the render thread on first use as before (49 ms for Earth); worker thread not done.

Replace the R8 code texture with a **per-bit signed distance field**, same 2048 x 1024 equirectangular layout,
for every planet.

### Texture

- `GL_RGBA8`: R, G, B = signed distance for code bits 2, 3, 4; A unused (or keep the raw code for debugging).
- Encoding: `0.5` on the edge, `+` inside the bit's region, scaled so the byte covers +-`SDF_RANGE` texels
  (start with 4): `value = 0.5 + d / (2 * SDF_RANGE)`, clamped.
- Distances measured **on the sphere** would be ideal; measuring in texel space is fine away from the poles,
  and near the poles the ice caps are circles anyway. Start in texel space; revisit if pole coasts look wrong.

### Building it (`gl_planet_tex.c`, `make_texture`)

1. Split outlines as now (6 levels; keep `LEVELS`).
2. Fill **three 1-bit masks** at 4x supersample (8192 x 4096 bytes, one byte per texel holding the 3 bits:
   the existing `fill_polygon` / `fill_circle` already XOR the code into a byte; just run them at the higher
   resolution). 32 MB temporary; fine on desktop; on Quest see "Costs".
3. For each bit, compute the distance from each output texel centre to the nearest mask boundary: a two-pass
   exact Euclidean distance transform (Felzenszwalb/Huttenlocher 1D-squared passes) on the supersampled mask,
   once for "inside" and once for "outside", sampled at the output texels and divided by 4. The u direction
   wraps (pad by the SDF range on both sides, or run the 1D pass on a wrapped row).
4. Pack into RGBA8, upload with `GL_LINEAR` min/mag filtering (distances interpolate, codes didn't),
   `GL_REPEAT` in s, clamp in t. Mipmaps: no (the shader handles minification by `fwidth`).

Better alternative for step 2 if the EDT is awkward (step 3): compute distances directly from the split polylines:
for each segment, visit the output texels within `SDF_RANGE` of it and keep the minimum distance; sign from the
supersampled mask. Segments: ~6,000 for Earth, cheap.

### Shader (`FS_SPHERE` in `gl_planet.c`)

```
vec3 s = texture(uSurface, uv).rgb;          // three distances, 0.5 = edge
vec3 w = fwidth(s);                          // one screen pixel in the same units
vec3 a = clamp((s - 0.5) / max(w, 1e-5) + 0.5, 0.0, 1.0);   // per-bit coverage, ~1 px antialiasing
// colour = sum over the 8 bit combinations of product(coverage) * palette[combo] (dark/lit mixed as now)
```

- 8 palette mixes per pixel; cheap. For most pixels all three `a` are 0 or 1.
- `uv` seam at u = 0/1: `fwidth(uv)` jumps there; compute `fwidth` on the 3D normal-derived angle or use
  `textureGrad` with gradients from a seam-free u (e.g. computed from `atan` on both sides). The current shader
  avoids the issue with texelFetch; the new one must not show a one-pixel line along the date line.
- Keep the terminator mix (`lit`) as is.

### Result

From orbit: crisp coasts. Close up: the edge stays crisp (a distance field magnifies well) but its *shape* is
still the 6-level polyline with corners rounded at texel scale (25-75 km segments). Phase 2 fixes that.

## Phase 2: local detail that grows as you approach

A small **clipmap**: 2 or 3 extra distance-field textures centred on the point under the eye, each covering a
smaller area at higher resolution, built from outlines refined deeper only where they are near.

### Levels

| level | covers (Earth) | texture | texel |
|---|---|---|---|
| global (phase 1) | whole planet | 2048 x 1024 | ~20 km |
| L1 | ~2,000 km square | 1024 x 1024 | ~2 km |
| L2 | ~250 km square | 1024 x 1024 | ~250 m |
| L3 (optional) | ~30 km square | 1024 x 1024 | ~30 m |

Sizes scale with the planet's radius (define them as angles, not km). Only build the levels the eye's height
needs: a level is useful while its texel is bigger than a screen pixel at the nearest ground it covers,
roughly `texel > height * pixel_angle`. At 300 km up, L1 is enough; at 25 km, L1 + L2; near the ground, all.

### Projection

Each level is a square in an **azimuthal (gnomonic or equidistant) projection** centred on its own centre
point `c` on the sphere, with axes `e1, e2` tangent at `c`: `p -> (dot(p, e1), dot(p, e2)) / dot(p, c)`
(gnomonic: straight segments stay straight, which makes rasterising the split polyline exact). No pole
distortion, unlike the equirectangular global level.

Precision: points are unit vectors in float, error ~6e-8, i.e. ~0.4 m on Earth: enough for 30 m texels.
Do the CPU side in double.

### Building a level (worker thread)

1. **Refine**: for every outline, split edges with `split_edge`'s exact maths (same rng, same amplitude rule),
   but the stop test changes from "level >= LEVELS" to: stop when the edge is entirely outside the level's
   square (plus a margin), or when its length is below half the level's texel, or when level reaches the
   displacement cut-off (then straight). Edges far from the square stay at whatever depth (6) the global
   level used. Write it as a variant of `split_edge` taking a predicate; reuse the rng code unchanged.
2. **Clip** the refined polygon to the square (Sutherland-Hodgman on the projected 2D polygon, done per
   feature, keeping the even-odd semantics: clip each closed outline as a polygon). Circles: rasterise
   directly per texel (cheap).
3. **Fill + distance** exactly as phase 1 (supersampled per-bit masks + distances, or segment stamping).
4. Hand the finished RGBA8 buffer to the render thread; upload; swap in. Keep the old one until then.

Rebuild a level when the eye's sub-point moves more than ~1/4 of the level's width from its centre (re-centre
on the current sub-point). Rebuild cost per level: refinement (thousands of segments) + 1024^2 fill + distance,
target < 30 ms on desktop. On Quest, smaller (512^2) or fewer levels.

Thread: there is a render thread and a game thread (`GLF_RENDER_THREAD`). Use a dedicated worker
(SDL_CreateThread) with a one-slot request/result handoff; the render thread only uploads. Planet identity for
caching: the existing `surface_key()` hash plus the object address.

### Shader

- Uniforms per level: centre `c`, axes `e1, e2`, half-width (in gnomonic units), sampler. Up to 3 levels.
- For a hit normal `p` (already in planet axes in the shader as `uToLocal * n`): for the finest level whose
  square contains `p` with a margin, compute its uv and sample it; cross-fade to the next coarser level over
  the outer 10-15% of the square so level boundaries don't show. Same per-bit coverage maths as phase 1.
- Back-facing hemisphere test: `dot(p, c) > 0` before using a level.

### Popping

New detail appears when a level is rebuilt or the eye descends; with the length-below-half-a-texel rule,
added wiggle is below a texel and hard to see. If it shows, cross-fade a freshly built level in over ~0.5 s
(two textures per level, mix by a uniform).

## Phase 3: better Earth outlines (option 5)

### Format and loading

- A host-side data file, e.g. `data/planets/earth.txt` (text, one feature per block: code, detail byte, then
  points as `lat lon` in degrees with decimals; circles as `circle code lat lon radius_deg [both]`). Points
  are **not** limited to the byte grid.
- Release packaging: the game currently embeds all data in the binary ("Game data is compiled into the .so
  (no assets)" for Android). Simplest consistent choice: **compile the file in** (a generated C array, like
  `fe2*_bin.h`), so Windows/Linux/Android/Quest all get it with no packaging changes.
- Don't put the points in `PlanetSurface`: that struct is copied into every `NativePlanet` record per frame.
  Instead: when `capture_surface()` sees model 72's data (compare the 372 bytes, or `surface_key`), set a flag /
  override id in `PlanetSurface`; the texture builders (phases 1 and 2) take outlines from the override.
- Keep the game's colours, band list and polar circles from the original data; replace only the outlines (and
  add or drop circles as needed).
- Config switch `planet_earth_outlines=1` (glfrontier.cfg) so the original Earth can be compared.

### Making the outlines

Decided (2026-11-01): traced from coastline data, about 1,000 points.

1. A script `tools/planet_outlines/trace.py` reads a public-domain coastline (Natural Earth 1:110m land, GeoJSON,
   CC0) and simplifies each landmass (Visvalingam-Whyatt) to a total budget of about **1,000 points**
   (split between landmasses roughly by coastline length, with a floor of 3-4 points per kept island).
   Small islands below an area threshold are dropped; keep the islands the game has (Britain, Ireland, Japan,
   Madagascar, ...), add obvious missing ones (Iceland, New Zealand, Sri Lanka, Sumatra/Java, Cuba, ...).
2. Inland seas as holes (even-odd fill): Caspian, Black Sea, Great Lakes, Hudson Bay is sea already if traced.
3. Antarctica: the game uses an ice circle; keep the circle, optionally add a land outline under it (land under
   ice is grey too, so only the parts outside the circle would show).
4. Detail byte per outline: smaller wiggle than the original (the trace already carries real shape): choose
   the amplitude so the first-level displacement is about the trace's error (~5-10 km at 1:110m), i.e. detail
   around -2 to -3 instead of -4 to -6. The fractal then supplies everything finer, as the game would.
5. **Frame alignment**: the game's axes are mirrored relative to real longitude (doc section 6.4: Britain at
   about 3 W..3 E; Ireland's game data at "lon 5..10" = real 10..5 W). Convert real (lat, lon) to the game's
   direction as `x = cos(lat) * sin(-lon)`, `y = sin(lat)`, `z = cos(lat) * cos(-lon)`, then **check** by
   rendering the original and the traced outlines into the same equirectangular image and overlaying them:
   continents should line up within ~1 degree. Fix the sign/offset there, not by eye in game.
6. Commit the decoder/overlay tool as `tools/fe2ShipBuilder/planet_dump.cpp` (like `lod_survey.cpp`; the 2026-10-31
   scratch version linked `fe2core.cpp` with a GL-free copy of `gl_planet_tex.c`: find op `$1f` in the model's
   code, data starts 6 bytes after the op, length `(w0 >> 4) & $ffe`).

### Limits to raise

`PLANET_DATA_MAX` (2048), `MAX_FEATURES` (128), `MAX_POINTS` (2048), `MAX_OUTLINE` in `gl_planet_tex.c` only
bound the game's data path; the override path should use dynamic arrays.

## Costs

- Global texture memory: RGBA8 2048x1024 = 8 MB per planet (was 2 MB); cache of 16 slots = up to 128 MB worst
  case. Reduce `CACHE_SLOTS` to 8, or use RGB8/`RG8 + R8`, or 1024x512 for planets that have no outlines.
  Planets with no outlines at all (one colour) need no texture: skip them.
- Build time: currently "on first use" on the render thread (a hitch). Phase 1 makes it slower (supersample +
  distances): move global builds to the same worker as phase 2 and draw the plain sphere until ready.
- Quest: build at 2x supersample, L1 + L2 only at 512^2; measure with `GLF_FRAME_PROF=1`.

## Testing

- Test instances only (`../devtools/README.md`, drive by PID, scratch build dir, never `build-original` while
  the user may be playing). Script: `--do "new 1; waitgame; goto Earth 25"`, then 300, 2000, 0.5 km; also Mars
  (outlines + caps), a gas giant (model 73-76: nested circles = bands), model 63 (colour sets), Merlin.
- Checks: no date-line seam, no pole artefacts, caps still round, gas-giant bands unchanged in colour, classic
  view with `GLF_NATIVE_PLANETS=0` still the game's own picture, cockpit and VR passes agree.
- The user tests visuals in game; build into `build-original` when they're ready to look.
- Update `docs/planet-textures.md` sections 6 and 7 when done.

## Order and rough size

| phase | what | size |
|---|---|---|
| 0 | measure jiggle, double-precision / filtered orientation | small |
| 1 | per-bit SDF global texture + shader, worker-thread build | medium |
| 2 | clipmap levels with adaptive refinement | large |
| 3 | Earth outline file, trace script, overlay tool | medium (+ tuning) |

Phases 0 and 1 are independent of 2 and 3 and fix most of what the screenshot shows from orbit. Phase 2 is what
makes low flight look right. Phase 3 can be done any time after phase 1.

## Decisions (user, 2026-11-01)

1. Phase 3 outlines are **traced from public-domain coastline data** (Natural Earth 1:110m land), not placed by
   hand.
2. Earth's point budget is **about 1,000 points** (fits the original game's ~1,300-point format limit too,
   though this plan does not put it back into the game).
3. Other planets: nothing to decide for phase 3 (model 72 is Earth only); phases 0-2 improve every planet.
