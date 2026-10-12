/*
 * gl_scene.h - records what the emulated game asks us to draw, and replays
 * it in depth order when the frame is presented.
 *
 * HOW IT WORKS
 *   The 68k code calls host functions (Nu_Put*, see hostcall.c) while it walks
 *   its 3D scene. Each call copies what it needs out of emulated RAM into a
 *   small typed record, stored in the current depth node (Nu_InsertZNode).
 *   At Nu_DrawScreen the nodes are walked far-to-near and every record's
 *   draw function is called, which pushes geometry into the gl_draw batch.
 *
 * ADDING A NEW PRIMITIVE / FEATURE
 *   1. Add a line to GL_PRIMITIVES below:   X(MY_THING, draw_my_thing)
 *   2. Define the payload struct and the draw function
 *          void draw_my_thing(const void *payload);
 *      in any file under src/gl (see gl_prims.c / gl_planet.c for examples).
 *   3. Write the host call that captures data from the emulator:
 *          void Nu_PutMyThing(void) {
 *              MyThing *p = scene_record(PRIM_MY_THING, sizeof *p);
 *              if (!p) return;
 *              p->pos = m68k_vertex(GetReg(REG_A0) + 4);
 *              ...
 *          }
 *      and hook it into the hcalls[] table in hostcall.c.
 *   Draw functions must only read the payload: emulated RAM has moved on by
 *   the time they run.
 */
#ifndef GL_SCENE_H
#define GL_SCENE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Every primitive the scene knows how to replay: X(NAME, draw_function) */
#define GL_PRIMITIVES(X)                                                                                     \
	X(TRIANGLE, draw_triangle)                                                                               \
	X(QUAD, draw_quad)                                                                                       \
	X(LINE, draw_line)                                                                                       \
	X(BEZIER_LINE, draw_bezier_line)                                                                         \
	X(TEARDROP, draw_teardrop)                                                                               \
	X(POINT, draw_point)                                                                                     \
	X(TWINKLY_CIRCLE, draw_twinkly_circle)                                                                   \
	X(CIRCLE, draw_circle)                                                                                   \
	X(BLOB, draw_blob)                                                                                       \
	X(OVAL, draw_oval)                                                                                       \
	X(CYLINDER, draw_cylinder)                                                                               \
	X(LINE_2D, draw_line_2d)                                                                                 \
	X(COMPLEX_START, draw_complex_start)                                                                     \
	X(COMPLEX_VERTEX, draw_complex_vertex)                                                                   \
	X(COMPLEX_BEZIER, draw_complex_bezier)                                                                   \
	X(COMPLEX_INNER, draw_complex_inner)                                                                     \
	X(COMPLEX_END, draw_complex_end)                                                                         \
	X(PLANET, draw_planet)                                                                                   \
	X(SOFT_NODE, draw_soft_node)                                                                             \
	X(ATMOS, draw_atmos)

enum PrimOp
{
	PRIM_NONE = 0,
#define X(name, fn) PRIM_##name,
	GL_PRIMITIVES(X)
#undef X
		PRIM_COUNT
};

#define X(name, fn) void fn(const void *payload);
GL_PRIMITIVES(X)
#undef X

/* ---- common payload building blocks ------------------------------------ */
typedef struct
{
	int32_t x, y, z;
} Vec3i;

typedef struct
{
	uint8_t r, g, b, a;
} Rgb8;

/* Reads an emulator vertex (x, y, z longs) into view space (z flipped). */
Vec3i m68k_vertex(uint32_t addr);
/* Converts an Atari ST rgb444 colour word to 8-bit rgb */
Rgb8 rgb444_to_rgb8(int rgb444);

static inline void vec3i_to_f(Vec3i v, float out[3])
{
	out[0] = (float)v.x;
	out[1] = (float)v.y;
	out[2] = (float)v.z;
}

/* ---- recording (called from Nu_Put* host calls) ------------------------ */

/* Reserve `size` bytes for a record of type `op` in the current depth node.
 * Returns NULL when recording is disabled or the frame is full: callers just
 * return in that case. */
void *scene_record(enum PrimOp op, size_t size);

/* True when the GL renderer is active (not the original software one) */
bool scene_active(void);

/* Start a new frame of recording (Nu_3DViewInit) */
void scene_reset(void);
/* Start a new depth node (Nu_InsertZNode); larger z is drawn first.
 * Returns false if no node was started (renderer off, locked or full). */
bool scene_insert_node(uint32_t z);
/* Keep appending to the current node even if the game inserts new ones
 * (used while a complex polygon is being recorded). */
void scene_lock_node(bool lock);

/* Further passes over the world in the same frame (the cockpit's turned
 * camera, see cockpit_pass_* in gl_cockpit.h). Records between begin and
 * end go to a depth tree of their own, drawn after pass 0's in the order
 * of `id` (0 .. COCKPIT_MAX_PASSES-1). False if there is no room for it:
 * the records then stay in pass 0. */
bool scene_begin_pass(int id);
void scene_end_pass(void);
/* The pass being recorded: 0, or the further pass's number */
int scene_cur_pass(void);
/* A node with just a record of type `op`, in front of everything the pass
 * has so far (its background) and behind all that comes after. Records
 * after it are dropped until the next scene_insert_node. */
void scene_insert_after_background(enum PrimOp op);

/* PRIM_SOFT_NODE payload: a node of the game's software depth tree. The
 * game fills it in after the GL node is made, and reuses that memory for
 * the next pass, so it is copied out (gl_atmos.c) once the pass is over. */
typedef struct
{
	uint32_t node;       /* address in emulated RAM */
	int32_t band;        /* the copy, -1 if none */
	int32_t shell;       /* its 3D version (gl_atmos.c), -1 if none */
} SoftNode;
/* The current node's soft node, NULL if it has none */
SoftNode *scene_current_soft_node(void);
/* Copies every soft node recorded so far that is not copied yet */
void scene_capture_soft_nodes(void);
void soft_node_capture(SoftNode *s); /* gl_atmos.c */
void soft_nodes_reset(void);         /* gl_atmos.c, with scene_reset */
void atmos_publish(void);            /* gl_atmos.c, with scene_publish */
void atmos_acquire(void);            /* gl_atmos.c, with scene_acquire */
/* Drops what the object L390f2 is drawing (frame a6) records from here on,
 * but its planet, until it is done (the native scene draws the rest of a
 * planet's model, its rings); 0 stops */
void scene_mute_object(uint32_t a6);
void planet_dists_reset(void);       /* gl_planet.c, with scene_reset */
void planet_dists_publish(void);     /* gl_planet.c, with scene_publish */
void planet_dists_acquire(void);     /* gl_planet.c, with scene_acquire */

/* ---- hand over --------------------------------------------------------- *
 * The game records a frame while the last one it finished is drawn (on the
 * render thread, gl_screen.c). scene_publish (game thread, at the end of
 * the frame) makes the recorded frame the latest; scene_acquire (whoever
 * draws) takes the latest to draw. gl_screen.c holds a lock around both. */
void scene_publish(void);
void scene_acquire(void);

/* ---- replay (called from Nu_DrawScreen) -------------------------------- */
/* The acquired frame: pass 0, then the further passes (each set up by
 * cockpit_pass_draw_begin) */
void scene_draw(void);

#endif /* GL_SCENE_H */
