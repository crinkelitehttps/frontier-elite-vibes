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
 * 0..1 across the map, d the depth along the light in 0..1.
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

/* Whether the experiment is on (setting or GLF_SHIP_SHADOWS) */
bool gl_shadow_wanted(void);

/* Render thread, while the frame's draw list is being made, before anything
 * that receives the shadows: `tris` is 9 floats per triangle (view space),
 * `light` the unit vector away from the light. Makes the map's pass the next
 * command. Returns whether there is a map this frame. */
bool gl_shadow_set(const float light[3], const float *tris, size_t ntris);
/* No map this frame (the draw list has no pass) */
void gl_shadow_clear(void);

/* The map of this frame, if any: the 3x4 matrix (rows u, v, d), its texture,
 * and how much of the colour stays in shadow */
const float *gl_shadow_matrix(void);
GLuint gl_shadow_texture(void);
float gl_shadow_dark(void);
/* The depth bias to compare with, in the map's depth units */
float gl_shadow_bias(void);

void gl_shadow_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
