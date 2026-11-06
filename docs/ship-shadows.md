# Ship shadows (experiment)

Native scene only. On by default: glfrontier.cfg `ship_shadows=0` turns it off (SETTINGS page checkbox), or `GLF_SHIP_SHADOWS=1/0`
overrides. Code: `src/gl/gl_shadow.c`, receivers in `gl_draw.c` (flat shader) and `gl_planet.c` (sphere shader),
casters chosen in `gl_native.cpp` (`setup_shadows`).

## How it works

1. `prepare()` (once per frame, first thing in `native_draw_sky`) collects the triangles of every ship in view
   (objects of the game's model table with model number 10-41, not shrunk) and hands them with the light vector
   (`Object.light`, view space, away from the light) to `gl_shadow_set`.
2. That builds an orthographic light view round the ships (1024x1024 depth texture, 24 bit, depth reaching 1000 radii (was 200) of
   the ships along the light) and records a custom command, the first of the frame's draw list, that renders the
   triangles depth only into an FBO, restoring the framebuffer, viewport and enables after.
3. The scene's objects are drawn with `gd_set_shadow(matrix)` set, except the ships themselves. `gd_tri` etc. then
   store the shadow map's u, v and depth per vertex (the matrix applied to the point in view space, on the CPU), so the
   flat shader needs no inverse projection: it compares with a `sampler2DShadow` (hardware 2x2 filtering) and
   multiplies the colour by 0.5 in shadow. Underlay nodes (ground plates) carry their object's flag.
4. The planet's sphere shader works out the hit point in eye space, turns it back into view space (`uToView` = the
   head's turn transposed, `uViewR` = the sphere's unit) and looks the map up too, on the lit side only.

Casters: only the ships near the nearest one (within 8 of its radii, at least 100 m) go into the map. The map is fitted
round its casters, so with a second ship kilometres away every texel was metres wide: the shadow came out several
times too big and in coarse blocks (Merlin, Snap21/22). Ships outside that reach neither cast nor are kept from receiving.

Ground height on the planet: near the ground the drawn sphere can sit well above the scenery (a Merlin pad 258 m down had
the sphere 16 m down), which made the shadow on it about 16 times too big. In the cockpit/front views the sphere shader
scales the hit point along its ray by game altitude / sphere height (`shadow_k`, capped at 200), so the shadow falls on
the ground the scenery stands on. `GLF_SHIP_SHADOWS=2` also logs the sphere's distance, radius and height.

## Casters are the whole hull

The drawn model only holds the faces turned to the camera (per-face cull, and the model's "skip unless normal faces"
jumps), so a shadow made from it had holes: the Long Range Eagle's underside, the shuttle's whole hull. `setup_shadows`
runs each caster again with `Runner::casting`, which skips those facing tests (as the sky does), and takes the triangles
from that. The map's pass has culling off, so both sides of the hull count. Costs one more model run per caster.

## The player's own ship

In the cockpit's front view the game skips the player's ship (the slot at A6+608 that the object loop compares with each
object's slot, set by the camera set-up). With ship shadows on, `Call_CockpitBackground` (run before the object loop
of each pass) clears that byte, so the loop draws the ship, which the native scene captures like any object
(`Object.own_added`, `cockpit_own_ship_added`). Its `Drawn` is marked `hide`: it is kept as a caster but left out of
the scene (`placed`), so nothing of the hull is drawn round the eye. In the external view and the free camera the ship
is drawn as usual. `GLF_SHIP_SHADOWS=2` logs the object, caster and triangle counts now and then.

## Ships on ships

The map is a texture array (`GL_TEXTURE_2D_ARRAY`, 1024x1024, `1 + GL_SHADOW_CASTER_LAYERS` = 6 layers, 24 MB). Layer 0 has every
caster (ground, plates, station, planet). Layer k + 1 has every caster except the k-th, nearest the eye first, and is what
that ship looks up, so it cannot shadow itself and needs no bias against its own hull. All layers share one matrix (fitted to
all casters), so the per-vertex shadow coordinates are unchanged; `gd_set_shadow(m, layer)` picks the layer per draw command
(batches break when it changes) and the flat shader's `uShadowLayer` goes into `sampler2DArrayShadow`. The pass renders each
layer with `glFramebufferTextureLayer`, drawing the runs of triangles before and after the excluded caster. Only the five
nearest casters get a layer; the rest are casters that receive nothing (as before). `native_draw`'s `shadow_layer` has the
layer per object (0 for non-casters, -1 for casters without one). Untested on Quest (array depth textures are core in GLES 3.0).

Resolution: the map is fitted to the casters' box in the light's frame (separate u and v extents, 2% margin), not a sphere round
them, and the lookup (`shadowLit`, `GL_SHADOW_GLSL` in gl_shadow.h, used by both shaders) averages a 3x3 grid of the hardware's
2x2 comparisons a texel apart, so coarse texels give a soft edge instead of stairs. `GLF_SHIP_SHADOWS=2` logs the map's size and
texel size in metres. A far caster still stretches the map: that is what the second cascade is for.

Cascades: with one map the cruiser (2.4 km radius) pulled the Lynx carriers 19 km away into it, so the map was 20 km wide
(20 m texels, the Cobra one texel). Now there are two maps (`GL_SHADOW_CASCADES`), each fitted to its own casters:
cascade 0 has the ships up to 5 times the smallest in view, cascade 1 the bigger ones (each class keeps the "near the
nearest of the class" reach rule). The array has 2 x 6 layers (48 MB; each cascade's layer 0 all of its casters, then one
per caster without itself). Receivers look up both (`gd_set_shadow(m0, l0, m1, l1)`, a second per-vertex coordinate
`shadow2`, `uShadowLayer` vec2) and take the darker; a ship uses its own cascade's layer without itself and the other
cascade's whole layer. The planet's sphere shader has the same pair. `GLF_SHIP_SHADOWS=2` logs each map's size, texel
size (the game's units are mm) and caster count. Not done: a caster big enough to need its own fine map where a ship
casts onto a ship of the same class far away.

Stations: objects of models 43-54 (stations and bases; Lave Station is model 49, 265 nodes, about 1,760 triangles drawn) are
casters of cascade 1 whatever their size, so the Cobra's map is not stretched by them. The station gets its own cascade 1 layer
(it does not shadow itself) and the cascade 0 layer with every ship, so it still receives the ships' shadows; the ships look up
cascade 1 with the station in it. At 3 km from Lave Station the map is 6.7 x 4.1 km (6.5 x 4 m texels). Not looked at: the
Cobra inside the bay (the station's hull casts onto it); the station's baked night-face colours are darkened again.
`GLF_SHIP_SHADOWS=2` also lists the objects in view (model, triangles, caster, layers).

Next (see the discussion): scenery (buildings, mountains) as casters.

## Limits / next steps

- Ships receive shadows from the other ships, not their own (so no wing-on-fuselage shadows). See "Ships on ships".
- Step 1 only: the ships cast on the ground (plates, scenery, the planet's sphere). The same lookup already applies to
  every receiver of the flat shader, so stations get it too, but that has not been looked at (and the station's own
  lighting is baked in the triangles' colours: night faces get darkened again).
- Objects shrunk by the game (far away) neither cast nor receive.
- A shadow falls on the lit side of the planet only; flat-shaded receivers cannot tell night from day.
- Quest/Android (GLES 3.0): depth textures and `sampler2DShadow` are core there, untested; the pass costs one more
  pass per frame (twice with VR eyes).
