# Cloud shadows

Native scene only (`gl_native.cpp`, `Runner::cloud_shadow_org`). Setting: glfrontier.cfg `cloud_shadows` (default 1;
SETTINGS page, "Cloud shadows fall away from the sun"). `GLF_CLOUD_SHADOWS=0/1` overrides it, `=2` also logs the sun's
height and the cloud's height now and then.

## What the game does

A cloud field (model 99) places each cloud (model 98, at model height y=30) and, under it, a shadow (model 206): a flat
copy of the cloud's outline at y=-3, called with the "no light" flag (bit 14 of the vertex word) because it has no faces
to shade. The shadow's vertex is the cloud's id plus 10, the same x and z at y=0. Nothing about the light enters its
position, so the shadow is always straight below. The routine that places both, `L3c8d8`, is the generic scaled
sub-model call (op `$1b`; it was named `ProjectCloudShadowPos` by mistake, now `L3c8d8_ProjectScaledSubModel`).

## What we do

When the shadow sub-model (206) is placed from a field, its origin moves to where the ray from the cloud's vertex,
along the light vector (`o.light`, the game's lighting vector in view space, pointing away from the light), meets the
field's ground plane (the field's own y axis is up): `shift = d - (d.up / light.up) light`, d = cloud - shadow vertex.
Sun overhead: no shift, as before. Sun lower than about 3.4 degrees (sin < 0.06; 7 degrees, sin < 0.12, with the cog menu's "Shadows at low sun" off, `low_sun_shadows=0`), or below the horizon: no shadow.
The shadow stays a flat polygon at the field's plane (hills and buildings do not receive it) and keeps the cloud's
outline (no stretching along the light).

Which star the lighting vector follows in multiple-star systems has not been checked.

## Free camera

The free camera (modded build, Ctrl-V) keeps the game running at a rate of your choice (keys 0-5) and, with the 3D
cockpit on, is shown through the cockpit renderer (wide, the panel floating below, head tracking). It is a good way to
watch cloud shadows move from a fixed point under time acceleration.

## Translucent, drawn once

The shadow faces (model 206) are drawn as black at alpha 110/255 (colour bit 15 in the native scene's `Tri`), so the
ground colour and detail show through, as with the ship shadows. One light means one shadow: where shadows overlap the
ground is darkened once. All the underlay nodes' shadow triangles, of every object, are drawn together at the first
underlay node that has any, with the stencil counting (`gd_once_phase`: the first draw of a pixel passes and
increments, later ones fail; a second invisible draw decrements back, so the cockpit's stencil refs are untouched).
`GLF_CLOUD_SHADOWS=3` logs shadow group sizes (per-node draws only). Shadows are painted at that point of the underlay
order, so a nearer ground plate drawn later could hide one.

Cloud shadows over ship/building shadows: the shadow maps darken the ground in its own shader (to 0.5 in full
shadow), and a cloud's shadow is painted over it afterwards, so where both fell the ground went twice as dark. The
cloud shadows are drawn with `gd_set_shadow_blend` (blend mode 2): the flat shader looks the maps up at the shadow
triangle's point (every caster's layer, as the planet's ground does; only when all the underlays' objects are at
true size) and lowers the alpha to `max(1 - (1 - a) / sh, 0)`, sh the map's darkening there. The ground then ends
as dark as the darker of the two, never both. (2026-11-08)

## Depth tested, per slice (2026-11-19)

Painted at one point of the underlay order, a shadow covered whatever of its depth slice had been drawn before that
point, and the order changes with the viewpoint: a dome under a cloud looked shadowed from one height and clear (the
shadow "in front" of it) from another (saves Snap06/Snap07). Now each depth slice's cloud shadows are drawn at the end
of the slice, depth tested but not written, so buildings, ships and clouds standing in front of the shadow's ground
hide it from any viewpoint. Only the slices' objects write depth (the planet's ground doesn't), so the ground never
hides a shadow. Still once per pixel within a slice (stencil), and still the shadow-map blend. A raised object is not
darkened by the shadow (it is still a flat polygon on the field's plane). `GLF_CLOUD_SHADOW_DEPTH=0` restores the old
painting for comparison.

## The real cause: the field's size exponent (2026-11-19)

The depth test was not enough (saves Snap10-12: a shadow on a dome's green base at one height, off it a few hundred
metres higher). Measured: the bases sat 2 m under the planet's sphere at every height, but the cloud fields' origins
hung at about half the eye's height over the ground (eye 35 m: fields at 0-9 m; eye 293 m: 126-201 m). The copies'
stored positions were right; the drawing was not. The scenery's cloud fields carry a size exponent at 88(obj), 1 or 2:
L390f2 subtracts it from the distance exponent (L39180) and so brings the object 2^n nearer while leaving its model's
scale (-208(a6)) as it is, which on screen makes the field 2^n times its model's size. The native scene took the
position as it came, so the whole field (clouds and shadows) was drawn uniformly shrunk toward the eye: about right on
screen, but at half (or a quarter) of the distance in 3D, so its shadows lay on a plane above the bases' ground and slid
over them with the viewpoint, and the depth slices and bends saw it in the wrong place.

Now, after capture, an object with 88(obj) = n > 0 has its position multiplied by 2^n and its scale raised by n (as
L390f2 itself moves -208 and -192 when d0 < 0), so it is drawn where it is at its true size. The distance tests of its
model (`K << (detail + s)`) scale the same way, so clouds keep their detail at distance. `GLF_OBJ_SIZE=0` restores the
old placement, `=2` logs which models have a size exponent (only model 99 in the saves checked).

Also: the scenery copies (land and cloud) are moved along their up to the planet's radius when made; the game's cast,
run relative to the ship, left them 2-30 m under it (`GLF_SCEN_GROUND=0` keeps the game's height, `GLF_SCEN_LOG=1`
logs the moves).

## The shadow under the ground (2026-11-19, the one that mattered)

With the field in place the shadow still slid off a dome as the eye climbed (screenshots: shadow on the dome's green
base from 45 m up, on bare ground beyond it from ~500 m, at the same place on the screen). Measured in planet
coordinates: the field's origin 2 m under the sphere, the shadow's vertices **195 m** under it. The shadow model (206)
draws its outline at y = -3 of its own units, and at a big field's scale a unit is ~65 m. The game paints the shadow,
so its depth never showed; drawn in 3D it was a sheet 200 m under the ground, seen through the ground (which writes no
depth), and a point of it showed where the eye's ray crossed the ground: at h / (h + 195 m) of its distance, so near
the eye when low, farther out when high. Now the shadow sub-model is lifted (along its own up, in `call`) so its
lowest vertex lies at its origin, on the field's ground plane: measured 1.6 m under the sphere, the same from 35 m and
531 m up. `GLF_CLOUD_SHADOW_LIFT=0` turns it off.
