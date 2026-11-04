# Ship shadows (experiment)

Native scene only. Off by default: glfrontier.cfg `ship_shadows=1` (SETTINGS page checkbox), or `GLF_SHIP_SHADOWS=1/0`
overrides. Code: `src/gl/gl_shadow.c`, receivers in `gl_draw.c` (flat shader) and `gl_planet.c` (sphere shader),
casters chosen in `gl_native.cpp` (`setup_shadows`).

## How it works

1. `prepare()` (once per frame, first thing in `native_draw_sky`) collects the triangles of every ship in view
   (objects of the game's model table with model number 10-41, not shrunk) and hands them with the light vector
   (`Object.light`, view space, away from the light) to `gl_shadow_set`.
2. That builds an orthographic light view round the ships (1024x1024 depth texture, 24 bit, depth reaching 200 radii of
   the ships along the light) and records a custom command, the first of the frame's draw list, that renders the
   triangles depth only into an FBO, restoring the framebuffer, viewport and enables after.
3. The scene's objects are drawn with `gd_set_shadow(matrix)` set, except the ships themselves. `gd_tri` etc. then
   store the shadow map's u, v and depth per vertex (the matrix applied to the point in view space, on the CPU), so the
   flat shader needs no inverse projection: it compares with a `sampler2DShadow` (hardware 2x2 filtering) and
   multiplies the colour by 0.5 in shadow. Underlay nodes (ground plates) carry their object's flag.
4. The planet's sphere shader works out the hit point in eye space, turns it back into view space (`uToView` = the
   head's turn transposed, `uViewR` = the sphere's unit) and looks the map up too, on the lit side only.

## The player's own ship

In the cockpit's front view the game skips the player's ship (the slot at A6+608 that the object loop compares with each
object's slot, set by the camera set-up). With ship shadows on, `Call_CockpitBackground` (run before the object loop
of each pass) clears that byte, so the loop draws the ship, which the native scene captures like any object
(`Object.own_added`, `cockpit_own_ship_added`). Its `Drawn` is marked `hide`: it is kept as a caster but left out of
the scene (`placed`), so nothing of the hull is drawn round the eye. In the external view and the free camera the ship
is drawn as usual. `GLF_SHIP_SHADOWS=2` logs the object, caster and triangle counts now and then.

## Limits / next steps

- Step 1 only: the ships cast on the ground (plates, scenery, the planet's sphere). The same lookup already applies to
  every receiver of the flat shader, so stations get it too, but that has not been looked at (and the station's own
  lighting is baked in the triangles' colours: night faces get darkened again).
- Objects shrunk by the game (far away) neither cast nor receive.
- A shadow falls on the lit side of the planet only; flat-shaded receivers cannot tell night from day.
- Quest/Android (GLES 3.0): depth textures and `sampler2DShadow` are core there, untested; the pass costs one more
  pass per frame (twice with VR eyes).
