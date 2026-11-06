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
