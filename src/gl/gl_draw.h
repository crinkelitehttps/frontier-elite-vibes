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
 * tight bounds, so depth stays precise at any distance. The depth written
 * is the fragment's log(w); clipping is at `near` only. Tested (LEQUAL) always;
 * written if `write`; colour drawn unless `depth_only`. */
typedef struct
{
	float near, far;
	float lo, hi;
	float bias;
	bool write, depth_only;
} GdDepth;
void gd_set_depth(const GdDepth *d);

/* Ship shadows (gl_shadow.h): vertices emitted until the next call look the
 * shadow maps up in the flat shader. Each of the two cascades has `m`, the 3x4 matrix
 * (rows u, v, depth) from the point passed to gd_tri etc. (before the modelview) to its
 * coordinates, and `layer`, its layer of the map (gl_shadow_layer, gl_shadow_all_layer);
 * a negative layer = that cascade is not looked up. Both off is the default at the
 * start of every frame */
void gd_set_shadow(const float *m0, int layer0, const float *m1, int layer1, const float *m2, int layer2);

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
void gd_color4ub(int r, int g, int b, int a);
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

/* Draw each pixel at most once between phase 0 and phase 2 (the stencil
 * counts: a pixel passes the first time only). Draw the triangles after
 * phase 0, again (nothing visible) after phase 1, then phase 2 restores
 * the stencil as it was. Custom commands. */
void gd_once_phase(int phase);

/* ---- kept meshes -------------------------------------------------------
 * Triangles uploaded once in their own frame and drawn many times by a pose
 * (the native scene's cached models). Each vertex is x, y, z and a face
 * index; the face's colour comes from the face table (gd_face_colours),
 * alpha 0 = the face is not drawn. */
typedef struct GdMesh GdMesh;
GdMesh *gd_mesh_new(void);
void gd_mesh_free(GdMesh *m);
/* n vertices of 4 floats each (x, y, z, face), 3 per triangle */
void gd_mesh_upload(GdMesh *m, const float *xyzf, int n);
/* The face table (RGBA8 per face) the meshes drawn until the next call use */
void gd_face_colours(const uint8_t *rgba, int n);
/* Vertices first..first + count of m placed by pose (column-major: the
 * mesh's frame -> the modelview's), their faces at face_base + index in the
 * table: in the current state, as gd_tri would draw them (depth, shadows,
 * cull, blend). Never wireframed: draw those with gd_tri. */
/* bend: NULL, or 24 floats bending the mesh onto a planet (gl_native.cpp, Bend): the pose's 3 axes (.xyz), the
 * bend point in mesh units (.xyz) and the planet radius there (.w), the up direction in view units (.xyz) and
 * 1 (.w), the up direction in mesh units (.xyz) */
void gd_mesh_draw(GdMesh *m, const float pose[16], int face_base, int first, int count, const float *bend);
/* m's vertex array (attribute 0 the position, 1 the face) and vertex count, for passes of their own (gl_shadow.c) */
GLuint gd_mesh_vao(const GdMesh *m);
int gd_mesh_count(const GdMesh *m);

/* ---- shader helper ------------------------------------------------------
 * Compiles a program from GLSL bodies (no #version line: the right header
 * for GL 3.3 core / GLES 3.0 is prepended). Attributes are bound to
 * locations 0..n-1 in the order given. */
GLuint gd_build_program(const char *vs_body, const char *fs_body, const char *const *attribs, int n_attribs);

#endif /* GL_DRAW_H */
