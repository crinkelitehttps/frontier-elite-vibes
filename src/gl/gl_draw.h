/*
 * gl_draw.h - batched draw list.
 *
 * Everything drawn in a frame is appended to ONE vertex array which is
 * uploaded once and drawn with as few glDrawArrays calls as possible.
 * Consecutive primitives of the same kind (triangles / lines / points) that
 * share viewport and cull state are merged into a single draw call, so
 * painter's-algorithm ordering is kept while the driver sees a handful of
 * calls instead of thousands.
 *
 * Vertices are transformed to clip space on the CPU (projection * modelview),
 * so matrix state can change freely between primitives without breaking a
 * batch.  Geometry that needs its own shader (e.g. planets, the 2D screen
 * blit) is inserted into the stream as a "custom" command that runs a
 * callback at the right point in the draw order.
 *
 * Typical use:
 *     gd_set_viewport(GD_VP_VIEW3D);
 *     gd_color3ub(255, 0, 0);
 *     gd_tri(a, b, c);
 *     ...
 *     gd_flush();   // once per frame
 */
#ifndef GL_DRAW_H
#define GL_DRAW_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "gl_api.h"
#include "gl_math.h"

enum GdViewport
{
	GD_VP_WINDOW, /* whole SDL window                         */
	GD_VP_GAME,   /* letterboxed 320x240 game area            */
	GD_VP_VIEW3D, /* 3D view: game area minus the ctrl panel  */
	GD_VP_COUNT
};

/* ---- lifetime ---------------------------------------------------------- */
void gd_init(void);
void gd_shutdown(void);

/* Called at the start of every frame with the current viewport rects
 * (x, y, w, h in GL window coordinates). Resets all state. */
void gd_begin_frame(const int rects[GD_VP_COUNT][4]);

/* Uploads and draws everything recorded since gd_begin_frame. */
void gd_flush(void);

/* ---- state ------------------------------------------------------------- */
void gd_set_viewport(enum GdViewport vp);
const int *gd_viewport_rect(enum GdViewport vp);
void gd_set_projection(const mat4 *p);
const mat4 *gd_projection(void);
void gd_set_cull(bool cull_back_faces);
void gd_set_wireframe(bool wire); /* triangles become outlines */
/* Alpha blending (src alpha, 1 - src alpha) for what follows; off at the
 * start of every frame */
void gd_set_blend(bool blend);
bool gd_wireframe(void);

/* Depth testing for what follows (the native scene, gl_native.cpp), NULL =
 * off (the default at the start of every frame; everything else is drawn by
 * painter's algorithm). Depth is mapped like a perspective projection's
 * between view distances near and far (clip w), onto lo..hi of the NDC
 * range -1..1, minus bias: each group of objects gets its own slice with
 * tight bounds, so depth stays precise at any distance. Linear in clip
 * space, so interpolation and clipping stay exact. */
typedef struct
{
	float near, far;
	float lo, hi;
	float bias;
} GdDepth;
void gd_set_depth(const GdDepth *d);

/* Modelview stack */
void gd_push(void);
void gd_pop(void);
void gd_identity(void);
void gd_translate(float x, float y, float z);
void gd_rotate(float deg, float ax, float ay, float az);
void gd_scale(float x, float y, float z);
void gd_mult(const mat4 *m);
const mat4 *gd_modelview(void);
mat4 gd_mvp(void);

/* Current colour for subsequently emitted vertices */
void gd_color3ub(int r, int g, int b);
void gd_color3f(float r, float g, float b);

/* ---- geometry (model-space, transformed by projection * modelview) ---- */
void gd_point(const float p[3], float size);
void gd_line(const float a[3], const float b[3]);
void gd_line2(float x1, float y1, float x2, float y2);
void gd_line_loop(const float (*pts)[3], int n);
void gd_tri(const float a[3], const float b[3], const float c[3]);
void gd_quad(const float a[3], const float b[3], const float c[3], const float d[3]);
void gd_rect2(float x1, float y1, float x2, float y2); /* filled, z = 0 */

/* Per-vertex colours (0..1), e.g. for CPU lit geometry */
void gd_tri_rgb(const float a[3], const float ca[3], const float b[3], const float cb[3], const float c[3],
				const float cc[3]);

/* Per-vertex RGBA8 colours, e.g. for anti-aliased overlay edges (with
 * gd_set_blend); never wireframed */
void gd_tri_rgba(const float a[3], const uint8_t ca[4], const float b[3], const uint8_t cb[4], const float c[3],
				 const uint8_t cc[4]);

/* Filled disc in the modelview xy-plane, centred on the origin */
void gd_disk(float radius, int slices);

/* ---- custom commands ----------------------------------------------------
 * Runs fn(data) during gd_flush at this point in the draw order, with the
 * current viewport already set.  `size` bytes are reserved and returned for
 * the caller to fill in; the data lives until the end of the frame.
 * The callback may bind its own program/VAO/textures; it must leave blending
 * disabled.  Returns NULL if out of memory. */
typedef void (*gd_custom_fn)(const void *data);
void *gd_custom(gd_custom_fn fn, size_t size);

/* ---- shader helper ------------------------------------------------------
 * Compiles a program from GLSL bodies (no #version line: the right header
 * for GL 3.3 core / GLES 3.0 is prepended). Attributes are bound to
 * locations 0..n-1 in the order given. */
GLuint gd_build_program(const char *vs_body, const char *fs_body, const char *const *attribs, int n_attribs);

#endif /* GL_DRAW_H */
