/*
 * gl_shadow.h - ship shadows (experiment, glfrontier.cfg ship_shadows,
 * GLF_SHIP_SHADOWS=1/0 overrides).
 *
 * The native scene (gl_native.cpp) hands over the triangles of the ships in
 * view (game view space) and the light vector. They are drawn depth only
 * into a small shadow map from the light's side (an orthographic view along
 * the light round the ships), as the first command of the frame's draw list.
 * What is drawn afterwards looks the map up: the flat shader of gl_draw.c
 * for the triangles drawn between gd_set_shadow calls, the planet's sphere
 * shader (gl_planet.c) for the ground. A point the map says is behind a
 * ship is darkened.
 *
 * The matrix turns a view space point into the map's coordinates: u, v in
 * 0..1 across the map, d the depth along the light in 0..1. The map is a texture array (see gl_shadow_layer), GL_SHADOW_CASCADES * (1 + GL_SHADOW_CASTER_LAYERS) layers.
 */
#ifndef GL_SHADOW_H
#define GL_SHADOW_H

#include <stdbool.h>
#include <stddef.h>

#include "gl_api.h"

#ifdef __cplusplus
extern "C"
{
#endif

extern bool ship_shadows; /* the setting */
/* Shadows down to a lower sun: clouds to 3.4 degrees (off: 7), ship map depth 1000 radii (off: 200) */
extern bool low_sun_shadows;

/* Whether the experiment is on (setting or GLF_SHIP_SHADOWS) */
bool gl_shadow_wanted(void);

/* GLSL for the shaders that look the map up: shadowLit(map, u, v, layer, depth) = how much of the light
 * reaches the point, 0..1: a 3x3 grid of the hardware's 2x2 comparisons, a texel apart, so the stairs of
 * a coarse map become a soft edge */
#define GL_SHADOW_GLSL \
	"float shadowLit(highp sampler2DArrayShadow smp, vec2 uv, float layer, float ref) {\n" \
	"  vec2 t = 1.0 / vec2(textureSize(smp, 0).xy);\n" \
	"  float sum = 0.0;\n" \
	"  for (int i = -1; i <= 1; i++)\n" \
	"    for (int j = -1; j <= 1; j++)\n" \
	"      sum += texture(smp, vec4(uv + vec2(float(i), float(j)) * t, layer, ref));\n" \
	"  return sum * (1.0 / 9.0);\n" \
	"}\n"

/* How many casters have a map of their own (what lights them: all the casters but themselves), so can
 * receive shadows from the others */
#define GL_SHADOW_CASTER_LAYERS 5
/* The maps (cascades): a map fitted round all the casters would be as wide as the farthest of them, so the small
 * ones (a Cobra beside a cruiser) would be a texel or two. Cascade 0 has the small ships, 1 the big ones and stations, 2 the scenery near the eye
 * (hangars, mountains: a station-wide map made their shadows mush), each fitted to its own; whatever receives looks both up and is as dark as the darker says. */
#define GL_SHADOW_CASCADES 3

/* Render thread, while the frame's draw list is being made, before anything
 * that receives the shadows: `tris` is 9 floats per triangle (view space),
 * `light` the unit vector away from the light. The triangles are the casters' one after another: `caster_tris_end`
 * says where each of the `ncasters` ends (a triangle count), nearest first. Makes the map's pass the next
 * command. Returns whether there is a map this frame. */
bool gl_shadow_set(int cascade, const float light[3], const float *tris, size_t ntris, const size_t *caster_tris_end,
				   int ncasters);
/* The layer of the map (gd_set_shadow) a caster (its index in that order) looks up: the map without
 * itself; -1 if it has none (it receives nothing). Layer 0 is the map with every caster, for the rest */
int gl_shadow_layer(int cascade, int caster);
/* The layer with every caster of the cascade, -1 if it has no map this frame */
int gl_shadow_all_layer(int cascade);
/* No map this frame (the draw list has no pass) */
void gl_shadow_clear(void);
/* the maps made for this game frame drawn again for another picture of it (same casters, same light) */
void gl_shadow_again(void);

/* The map of this frame, if any: the 3x4 matrix (rows u, v, d), its texture,
 * and how much of the colour stays in shadow */
const float *gl_shadow_matrix(int cascade); /* NULL: no map */
GLuint gl_shadow_texture(void); /* 0: no map at all */
float gl_shadow_dark(void);
/* The depth bias to compare with, in the map's depth units */
float gl_shadow_bias(int cascade);

void gl_shadow_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
