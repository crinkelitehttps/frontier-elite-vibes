/*
 * gl_prims.c - the basic 3D primitives the game draws with.
 *
 * Each primitive has two halves:
 *   Nu_PutXxx()   host call, runs inside the emulator: copies registers and
 *                 emulated RAM into a payload record (gl_scene).
 *   draw_xxx()    runs at present time: turns the payload into geometry
 *                 for the gl_draw batch.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gl_api.h"
#include "glutess.h"

#include "gl_cockpit.h"
#include "gl_draw.h"
#include "gl_scene.h"
#include "gl_prims.h"
#include "main.h"
#include "m68000.h"
#include "renderer.h"

/* =========================================================================
 * Shared helpers
 * ========================================================================= */
static inline void set_rgb8(Rgb8 c)
{
	gd_color3ub(c.r, c.g, c.b);
}

static void eval_bezier(float out[3], float t, const float cp[4][3])
{
	float c = 1.0f - t;
	float a = c * c * c, b = 3.0f * c * c * t, cc = 3.0f * c * t * t, d = t * t * t;
	for (int i = 0; i < 3; i++)
		out[i] = cp[0][i] * a + cp[1][i] * b + cp[2][i] * cc + cp[3][i] * d;
}

/* ST rgb444 nibble -> 0..1 */
void gl_rgb444_to_f(int rgb, float out[3])
{
	out[0] = ((rgb >> 8) & 0xf) / 16.0f;
	out[1] = ((rgb >> 4) & 0xf) / 16.0f;
	out[2] = (rgb & 0xf) / 16.0f;
}

void gl_material_init(GlMaterial *m, const float light_dir[3], int light_col, int extra_col, int obj_col)
{
	/* bit 8: object is self-lit, draw flat in its own colour */
	m->lit = !(obj_col & (1 << 8));
	obj_col &= ~(1 << 8);

	gl_rgb444_to_f(light_col, m->diffuse);
	if (obj_col & (1 << 4))
	{
		/* bit 4: add the model's "extra" colour on top */
		float e[3];
		gl_rgb444_to_f(obj_col ^ (1 << 4), m->ambient);
		gl_rgb444_to_f(extra_col, e);
		for (int i = 0; i < 3; i++)
			m->ambient[i] = fminf(m->ambient[i] + e[i], 1.0f);
	}
	else
	{
		gl_rgb444_to_f(obj_col, m->ambient);
	}
	m->light[0] = light_dir[0];
	m->light[1] = light_dir[1];
	m->light[2] = light_dir[2];
	vec3_normalize(m->light);
}

void gl_material_shade(const GlMaterial *m, const float n_eye[3], float out[3])
{
	if (!m->lit)
	{
		out[0] = m->ambient[0];
		out[1] = m->ambient[1];
		out[2] = m->ambient[2];
		return;
	}
	float d = n_eye[0] * m->light[0] + n_eye[1] * m->light[1] + n_eye[2] * m->light[2];
	if (d < 0.0f)
		d = 0.0f;
	for (int i = 0; i < 3; i++)
		out[i] = fminf(m->ambient[i] + d * m->diffuse[i], 1.0f);
}

/* Light source vector as the emulator stores it (3 words, x/y flipped) */
void gl_read_light_dir(uint32_t addr, float out[3])
{
	out[0] = (float)-STMemory_ReadWord(addr);
	out[1] = (float)-STMemory_ReadWord(addr + 2);
	out[2] = (float)STMemory_ReadWord(addr + 4);
}

/* =========================================================================
 * Triangle / quad / line
 * ========================================================================= */
typedef struct
{
	Vec3i v[3];
	Rgb8 col;
} PTriangle;

void Nu_PutTriangle(void)
{
	PTriangle *p = scene_record(PRIM_TRIANGLE, sizeof *p);
	if (!p)
		return;
	p->v[0] = m68k_vertex(GetReg(REG_A0) + 4);
	p->v[1] = m68k_vertex(GetReg(REG_A1) + 4);
	p->v[2] = m68k_vertex(GetReg(REG_A2) + 4);
	p->col = rgb444_to_rgb8(GetReg(REG_D6));
}

void draw_triangle(const void *payload)
{
	const PTriangle *p = payload;
	float a[3], b[3], c[3];
	vec3i_to_f(p->v[0], a);
	vec3i_to_f(p->v[1], b);
	vec3i_to_f(p->v[2], c);
	set_rgb8(p->col);
	gd_tri(a, b, c);
}

typedef struct
{
	Vec3i v[4];
	Rgb8 col;
} PQuad;

void Nu_PutQuad(void)
{
	PQuad *p = scene_record(PRIM_QUAD, sizeof *p);
	if (!p)
		return;
	p->v[0] = m68k_vertex(GetReg(REG_A0) + 4);
	p->v[1] = m68k_vertex(GetReg(REG_A1) + 4);
	p->v[2] = m68k_vertex(GetReg(REG_A2) + 4);
	p->v[3] = m68k_vertex(GetReg(REG_A3) + 4);
	p->col = rgb444_to_rgb8(GetReg(REG_D6));
}

void draw_quad(const void *payload)
{
	const PQuad *p = payload;
	float v[4][3];
	for (int i = 0; i < 4; i++)
		vec3i_to_f(p->v[i], v[i]);
	set_rgb8(p->col);
	gd_quad(v[0], v[1], v[2], v[3]);
}

typedef struct
{
	Vec3i v[2];
	Rgb8 col;
} PLine;

void Nu_PutLine(void)
{
	PLine *p = scene_record(PRIM_LINE, sizeof *p);
	if (!p)
		return;
	p->v[0] = m68k_vertex(GetReg(REG_A0) + 4);
	p->v[1] = m68k_vertex(GetReg(REG_A1) + 4);
	p->col = rgb444_to_rgb8(GetReg(REG_D6));
}

void draw_line(const void *payload)
{
	const PLine *p = payload;
	float a[3], b[3];
	vec3i_to_f(p->v[0], a);
	vec3i_to_f(p->v[1], b);
	set_rgb8(p->col);
	gd_line(a, b);
}

/* =========================================================================
 * Bezier line / teardrop (engine exhaust)
 * ========================================================================= */
typedef struct
{
	Vec3i v[4];
	Rgb8 col;
} PBezier;

void Nu_PutBezierLine(void)
{
	PBezier *p = scene_record(PRIM_BEZIER_LINE, sizeof *p);
	if (!p)
		return;
	p->v[0] = m68k_vertex(GetReg(REG_A0) + 4);
	p->v[1] = m68k_vertex(GetReg(REG_A1) + 4);
	p->v[2] = m68k_vertex(GetReg(REG_A2) + 4);
	p->v[3] = m68k_vertex(GetReg(REG_A3) + 4);
	p->col = rgb444_to_rgb8(GetReg(REG_D6));
}

#define BEZIER_LINE_STEPS 20
void draw_bezier_line(const void *payload)
{
	const PBezier *p = payload;
	float cp[4][3], prev[3], cur[3];
	for (int i = 0; i < 4; i++)
		vec3i_to_f(p->v[i], cp[i]);
	set_rgb8(p->col);
	eval_bezier(prev, 0.0f, cp);
	for (int i = 1; i <= BEZIER_LINE_STEPS; i++)
	{
		eval_bezier(cur, i / (float)BEZIER_LINE_STEPS, cp);
		gd_line(prev, cur);
		memcpy(prev, cur, sizeof(cur));
	}
}

typedef struct
{
	Vec3i tip, base;
	Rgb8 col;
} PTeardrop;

void Nu_PutTeardrop(void)
{
	PTeardrop *p = scene_record(PRIM_TEARDROP, sizeof *p);
	if (!p)
		return;
	p->tip = m68k_vertex(GetReg(REG_A0) + 4);
	p->base = m68k_vertex(GetReg(REG_A1) + 4);
	p->col = rgb444_to_rgb8(GetReg(REG_D6));
}

#define TD_STRETCH 1.3333333333f
#define TD_BROADEN 0.33f
#define TD_STEPS   24
void draw_teardrop(const void *payload)
{
	const PTeardrop *p = payload;
	float dir[3], cp[4][3];
	vec3i_to_f(p->tip, dir);
	vec3i_to_f(p->base, cp[0]);
	for (int i = 0; i < 3; i++)
		dir[i] -= cp[0][i];
	float ppd[2] = {-dir[1], dir[0]};

	cp[1][0] = cp[0][0] + TD_STRETCH * dir[0] + TD_BROADEN * ppd[0];
	cp[1][1] = cp[0][1] + TD_STRETCH * dir[1] + TD_BROADEN * ppd[1];
	cp[1][2] = cp[0][2] + dir[2];
	cp[2][0] = cp[0][0] + TD_STRETCH * dir[0] - TD_BROADEN * ppd[0];
	cp[2][1] = cp[0][1] + TD_STRETCH * dir[1] - TD_BROADEN * ppd[1];
	cp[2][2] = cp[0][2] + dir[2];
	memcpy(cp[3], cp[0], sizeof(cp[0]));

	set_rgb8(p->col);
	float prev[3], cur[3];
	eval_bezier(prev, 0.0f, cp);
	for (int i = 1; i <= TD_STEPS; i++)
	{
		eval_bezier(cur, i / (float)TD_STEPS, cp);
		gd_tri(cp[0], prev, cur);
		memcpy(prev, cur, sizeof(cur));
	}
}

/* =========================================================================
 * Points, circles, blobs
 * ========================================================================= */
typedef struct
{
	Vec3i v;
	Rgb8 col;
	float size;
} PPoint;

void Nu_PutColoredPoint(void)
{
	PPoint *p = scene_record(PRIM_POINT, sizeof *p);
	if (!p)
		return;
	p->v = m68k_vertex(GetReg(REG_A0) + 4);
	p->col = rgb444_to_rgb8(GetReg(REG_D0));
	p->size = 2.0f;
}

void Nu_PutPoint(void)
{
	PPoint *p = scene_record(PRIM_POINT, sizeof *p);
	if (!p)
		return;
	p->v = m68k_vertex(GetReg(REG_A0) + 4);
	p->col = rgb444_to_rgb8(0xfff);
	p->size = 1.0f;
}

void draw_point(const void *payload)
{
	const PPoint *p = payload;
	float v[3];
	vec3i_to_f(p->v, v);
	set_rgb8(p->col);
	gd_point(v, p->size);
}

typedef struct
{
	Vec3i v;
	int32_t radius; /* D2, low word signed */
	Rgb8 col;
} PCircle;

static void put_circle(enum PrimOp op)
{
	PCircle *p = scene_record(op, sizeof *p);
	if (!p)
		return;
	p->radius = GetReg(REG_D2);
	p->v = m68k_vertex(GetReg(REG_A0) + 4);
	p->col = rgb444_to_rgb8(GetReg(REG_D6));
}
void Nu_PutTwinklyCircle(void)
{
	put_circle(PRIM_TWINKLY_CIRCLE);
}
void Nu_PutCircle(void)
{
	put_circle(PRIM_CIRCLE);
}

void draw_twinkly_circle(const void *payload)
{
	const PCircle *p = payload;
	float z = (float)p->v.z;
	set_rgb8(p->col);
	gd_push();
	gd_translate((float)p->v.x, (float)p->v.y, z);
	float size = -0.002f * (int16_t)p->radius * z;
	if (size > 0.0f)
		gd_disk(size, 32);
	size -= 0.016f * z;
	if (size > 0.0f)
	{
		gd_line2(-size, 0, size, 0);
		gd_line2(0, -size, 0, size);
	}
	gd_pop();
}

void draw_circle(const void *payload)
{
	const PCircle *p = payload;
	float z = (float)p->v.z;
	set_rgb8(p->col);
	gd_push();
	gd_translate((float)p->v.x, (float)p->v.y, z);
	gd_disk(-0.002f * (int16_t)p->radius * z, 32);
	gd_pop();
}

typedef struct
{
	Vec3i v;
	int32_t col444;
	int32_t radius;
} PBlob;

void Nu_PutBlob(void)
{
	PBlob *p = scene_record(PRIM_BLOB, sizeof *p);
	if (!p)
		return;
	p->v = m68k_vertex(GetReg(REG_A0) + 4);
	p->col444 = GetReg(REG_D0);
	p->radius = GetReg(REG_D1) & 0xffff;
}

void draw_blob(const void *payload)
{
	const PBlob *p = payload;
	float v[3], col[3];
	vec3i_to_f(p->v, v);
	gl_rgb444_to_f(p->col444, col);
	gd_color3f(col[0], col[1], col[2]);
	if (p->radius < 3)
	{
		gd_point(v, (float)(p->radius / 2 + 1));
		return;
	}
	gd_push();
	gd_translate(v[0], v[1], v[2]);
	gd_disk(-0.002f * p->radius * v[2], p->radius + 4);
	gd_pop();
}

typedef struct
{
	Vec3i v;
	int32_t d3, d4, d5, radius;
} POval;

void Nu_PutOval(void)
{
	POval *p = scene_record(PRIM_OVAL, sizeof *p);
	if (!p)
		return;
	p->v = m68k_vertex(GetReg(REG_A0) + 4);
	p->d3 = GetReg(REG_D3);
	p->d4 = GetReg(REG_D4);
	p->d5 = GetReg(REG_D5);
	p->radius = GetReg(REG_D6);
}

void draw_oval(const void *payload)
{
	/* Not really understood yet (d3..d5 look like axis data); drawn as a
	 * black disc exactly like the previous renderer. */
	const POval *p = payload;
	gd_color3ub(0, 0, 0);
	gd_push();
	gd_translate((float)p->v.x, (float)p->v.y, (float)p->v.z);
	gd_disk((float)(int16_t)p->radius, 32);
	gd_pop();
}

/* =========================================================================
 * Cylinder (lit on the CPU; only ~60 vertices)
 * ========================================================================= */
typedef struct
{
	float light[3];
	int32_t light_col, obj_col, extra_col;
	Vec3i v1, v2;
	int32_t rad1, rad2;
	int32_t cap1_col, cap2_col;
} PCylinder;

void Nu_PutCylinder(void)
{
	PCylinder *p = scene_record(PRIM_CYLINDER, sizeof *p);
	if (!p)
		return;
	gl_read_light_dir(GetReg(REG_A4), p->light);
	p->light_col = GetReg(REG_D3);
	p->obj_col = GetReg(REG_D2);
	p->extra_col = GetReg(REG_D6);
	p->v1 = m68k_vertex(GetReg(REG_A2) + 4);
	p->v2 = m68k_vertex(GetReg(REG_A3) + 4);
	p->rad1 = GetReg(REG_D0) & 0xffff;
	p->rad2 = GetReg(REG_D1) & 0xffff;
	p->cap1_col = GetReg(REG_D5);
	p->cap2_col = GetReg(REG_D4);
}

#define CYL_SLICES 20
static void cylinder_cap(const PCylinder *p, int col, float radius, float z, float nz)
{
	GlMaterial m;
	float n[3] = {0, 0, nz}, ne[3], c[3];
	gl_material_init(&m, p->light, p->light_col, p->extra_col, col);
	mat4_xform_dir(gd_modelview(), n, ne);
	gl_material_shade(&m, ne, c);
	gd_color3f(c[0], c[1], c[2]);
	gd_push();
	gd_translate(0, 0, z);
	gd_disk(radius, CYL_SLICES);
	gd_pop();
}

void draw_cylinder(const void *payload)
{
	const PCylinder *p = payload;
	float d[3] = {(float)(p->v2.x - p->v1.x), (float)(p->v2.y - p->v1.y), (float)(p->v2.z - p->v1.z)};
	float h = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	if (h <= 0.0f)
		return;
	float r1 = (float)p->rad1, r2 = (float)p->rad2;

	gd_push();
	gd_translate((float)p->v1.x, (float)p->v1.y, (float)p->v1.z);
	gd_rotate(-GLM_RAD2DEG * (atan2f(d[2], d[0]) - GLM_PI / 2), 0, 1, 0);
	gd_rotate(-GLM_RAD2DEG * asinf(d[1] / h), 1, 0, 0);

	cylinder_cap(p, p->cap1_col, r1, 0.0f, -1.0f);
	cylinder_cap(p, p->cap2_col, r2, h, 1.0f);

	/* side, back faces culled */
	GlMaterial m;
	gl_material_init(&m, p->light, p->light_col, p->extra_col, p->obj_col);
	const mat4 *mv = gd_modelview();
	float nz = (r1 - r2) / h;
	float pos[CYL_SLICES + 1][2][3], col[CYL_SLICES + 1][3];
	for (int i = 0; i <= CYL_SLICES; i++)
	{
		float a = (i == CYL_SLICES ? 0 : i) * 2.0f * GLM_PI / CYL_SLICES;
		float x = -sinf(a), y = cosf(a);
		float n[3] = {x, y, nz}, ne[3];
		vec3_normalize(n);
		mat4_xform_dir(mv, n, ne);
		gl_material_shade(&m, ne, col[i]);
		pos[i][0][0] = x * r1;
		pos[i][0][1] = y * r1;
		pos[i][0][2] = 0.0f;
		pos[i][1][0] = x * r2;
		pos[i][1][1] = y * r2;
		pos[i][1][2] = h;
	}
	gd_set_cull(true);
	for (int i = 0; i < CYL_SLICES; i++)
	{
		/* same winding the old triangle strips produced */
		gd_tri_rgb(pos[i][0], col[i], pos[i + 1][0], col[i + 1], pos[i][1], col[i]);
		gd_tri_rgb(pos[i][1], col[i], pos[i + 1][0], col[i + 1], pos[i + 1][1], col[i + 1]);
	}
	gd_set_cull(false);
	gd_pop();
}

/* =========================================================================
 * 2D line (drawn over the 3D view in 320x200 screen coordinates)
 * ========================================================================= */
typedef struct
{
	int16_t x1, y1, x2, y2, col;
} PLine2D;

void Nu_Put2DLine(void)
{
	if (!scene_active())
		return;
	scene_insert_node(0);
	PLine2D *p = scene_record(PRIM_LINE_2D, sizeof *p);
	if (!p)
		return;
	p->x1 = (int16_t)GetReg(REG_D0);
	p->y1 = (int16_t)GetReg(REG_D1);
	p->x2 = (int16_t)GetReg(REG_D2);
	p->y2 = (int16_t)GetReg(REG_D3);
	p->col = (int16_t)GetReg(REG_D4);
}

void draw_line_2d(const void *payload)
{
	const PLine2D *p = payload;
	mat4 saved_proj = *gd_projection();
	mat4 view = cockpit_view_pixel_projection();

	gd_set_viewport(GD_VP_VIEW3D);
	gd_set_projection(&view);
	gd_push();
	gd_identity();
	gd_color3ub(0, 255, 0); /* the palette colour in p->col was never used */
	gd_line2(p->x1, p->y1, p->x2, p->y2);
	gd_pop();
	gd_set_projection(&saved_proj);
	gd_set_viewport(GD_VP_VIEW3D);
}

/* =========================================================================
 * Complex polygons (arbitrary, possibly concave, with holes and bezier
 * edges) tessellated in 3D view space with the GLU tessellator. The
 * triangles stay 3D so the GPU clips them properly at the camera; the old
 * renderer projected on the CPU and threw away vertices behind the camera,
 * which warped ground shapes whenever you looked around.
 * ========================================================================= */
static struct
{
	bool pending; /* ComplexStart seen but START record not written yet */
	int col444;
} complex_rec;

/* Writes the START record lazily, like the original renderer: the game may
 * call ComplexStart and then never add a vertex. */
static bool complex_begin_record(void)
{
	if (!scene_active())
		return false;
	if (complex_rec.pending)
	{
		complex_rec.pending = false;
		Rgb8 *c = scene_record(PRIM_COMPLEX_START, sizeof *c);
		if (c)
			*c = rgb444_to_rgb8(complex_rec.col444);
		scene_lock_node(true);
	}
	return true;
}

void Nu_PutComplexStart(void) {}

/* Set by Nu_ComplexNearCurve: the line the game makes in place of the
 * curve just recorded comes next, and is dropped */
static bool skip_line;

void Nu_ComplexStart(void)
{
	if (!scene_active())
		return;
	complex_rec.pending = true;
	complex_rec.col444 = GetReg(REG_D6);
	skip_line = false;
}

void Nu_ComplexSNext(void)
{
	if (!complex_begin_record())
		return;
	Vec3i *v = scene_record(PRIM_COMPLEX_VERTEX, sizeof *v);
	if (v)
		*v = m68k_vertex(GetReg(REG_A0) + 4);
}

/* Local in the game's complex polygon code (L3b5c6..): the first vertex of
 * the current outline, which the close ops (L3b680, L3b5fc) join back to */
#define A6_COMPLEX_OUTLINE_START (-172)

/* One complex polygon can hold several separate outlines (a nebula is a
 * cluster of blobs). L3b6b4 starts a straight-edged one: it stores its first
 * vertex (A1) as the outline start and calls this with the second (A0);
 * bezier-edged ones start with Nu_ComplexStartInner instead. Each needs its
 * own contour, or the tessellator joins them all into one outline, drawing
 * edges from blob to blob and filling between them.
 *
 * The bezier code's near-camera fallback also lands here, as a plain line
 * to A0 in the middle of an outline; there A1 is not the outline start. */
void Nu_ComplexSBegin(void)
{
	if (skip_line)
	{
		skip_line = false;
		return;
	}
	if (!complex_begin_record())
		return;
	u32 start = (u32)STMemory_ReadLong(GetReg(REG_A6) + A6_COMPLEX_OUTLINE_START);
	if ((u32)GetReg(REG_A1) == start)
		scene_record(PRIM_COMPLEX_INNER, 0);
	Nu_ComplexSNext();
}

void Nu_ComplexStartInner(void)
{
	if (!complex_begin_record())
		return;
	scene_record(PRIM_COMPLEX_INNER, 0);
}

void Nu_ComplexBezier(void)
{
	if (!complex_begin_record())
		return;
	Vec3i *v = scene_record(PRIM_COMPLEX_BEZIER, 4 * sizeof *v);
	if (!v)
		return;
	v[0] = m68k_vertex(GetReg(REG_A0) + 4);
	v[1] = m68k_vertex(GetReg(REG_A1) + 4);
	v[2] = m68k_vertex(GetReg(REG_A2) + 4);
	v[3] = m68k_vertex(GetReg(REG_A3) + 4);
}

/* Cockpit passes: a curved edge the game is about to make a straight line
 * because a control point is close to its camera (Lcockpit_curve in fe2.s).
 * Recorded as the curve, which the replay cuts at the near plane: the
 * passes look in other directions than the game's view, and a big cloud
 * that is a curve in one pass and a line in the next is cut off where
 * they meet. */
void Nu_ComplexNearCurve(void)
{
	Nu_ComplexBezier();
	skip_line = scene_active();
}

void Nu_ComplexEnd(void)
{
	if (!complex_begin_record())
		return;
	scene_record(PRIM_COMPLEX_END, 0);
	scene_lock_node(false);
}

/* ---- replay side ------------------------------------------------------ *
 *
 * Each outline (contour) is collected in 3D view space, cut at the near
 * plane, and tessellated in SCREEN space (x / depth, y / depth), the way the
 * game's software renderer clips its edges and fills in 2D. Tessellating the
 * 3D points directly only works for flat shapes: GLU flattens everything
 * onto one plane, which folds shapes that wrap around the viewer, like the
 * Milky Way band (a model placed around the camera, L44200_DrawBGStars) -
 * its separate blobs got joined into one outline and filled between. The
 * triangles still go to the GPU in 3D (each screen point lifted back onto
 * its 3D position), so depth and draw order are unchanged. */
/* Tessellating is slow, and a frame's scene can be drawn several times (the
 * window and each eye of a VR headset): each polygon's triangles are kept
 * until the next scene, keyed by its START record. They are in the view
 * space of the pass that recorded them, so whoever looks at them later
 * gets the same ones. */
#define TCACHE_SLOTS 32768 /* a power of two */
typedef struct
{
	const void *key;
	uint32_t scene; /* the slot is only valid in this scene */
	uint32_t first, count;
} TessEntry;
static TessEntry tcache[TCACHE_SLOTS];
static int tcache_used;
static uint32_t tcache_scene = 1;
static float (*tcache_verts)[3];
static uint32_t tcache_n, tcache_cap;

void gl_prims_new_scene(void)
{
	tcache_scene++;
	tcache_used = 0;
	tcache_n = 0;
}

/* The slot for key: its triangles, or empty to fill in (NULL if full) */
static TessEntry *tcache_slot(const void *key)
{
	uint32_t h = (uint32_t)(((uintptr_t)key >> 3) * 2654435761u) & (TCACHE_SLOTS - 1);
	for (int i = 0; i < TCACHE_SLOTS; i++, h = (h + 1) & (TCACHE_SLOTS - 1))
	{
		TessEntry *e = &tcache[h];
		if (e->scene != tcache_scene)
		{
			if (tcache_used >= TCACHE_SLOTS / 2)
				return NULL;
			return e;
		}
		if (e->key == key)
			return e;
	}
	return NULL;
}

static void tcache_add(const float v[3])
{
	if (tcache_n >= tcache_cap)
	{
		uint32_t cap = tcache_cap ? tcache_cap * 2 : 65536;
		float(*p)[3] = realloc(tcache_verts, cap * sizeof *p);
		if (!p)
			return;
		tcache_verts = p;
		tcache_cap = cap;
	}
	memcpy(tcache_verts[tcache_n++], v, sizeof tcache_verts[0]);
}

#define TESS_MAX_VERTS   8192
#define TESS_MAX_COMBINE 8192
#define CONTOUR_MAX      2048
/* View space looks down -z (m68k_vertex negates z); cut at this depth, a
 * little beyond the GL near plane (1.0) */
#define COMPLEX_NEAR     1.5

/* x, y: screen position (tessellated); 3D view space position (drawn) */
typedef struct
{
	GLdouble sx, sy, sz;
	GLdouble x, y, z;
} TessVert;

static GLUtesselator *tess;
static struct
{
	bool active;
	/* The polygon's own colour. Other records (e.g. landing pad lights) can
	 * sit between START and END in the same node and change the current
	 * colour, and the triangles only come out at END, so re-apply it. */
	Rgb8 col;
	/* GLU keeps pointers to vertex data until EndPolygon */
	TessVert verts[TESS_MAX_VERTS];
	int n_verts;
	TessVert combine[TESS_MAX_COMBINE];
	int n_combine;
	const TessVert *tri[3];
	int n_tri;
	float prev[3];
	bool have_prev;
	/* keeping the triangles: the START record, where they start */
	const void *keep_key;
	uint32_t keep_first;
	/* the triangles were kept from an earlier drawing: draw them at END */
	const TessEntry *kept;
	/* the outline being collected, 3D view space */
	float contour[CONTOUR_MAX][3];
	int n_contour;
	/* wireframe mode: outline of the current contour */
	float wire_first[3], wire_last[3];
	int wire_count;
} tp;

static void tess_begin_cb(GLenum type)
{
	(void)type; /* always GL_TRIANGLES: we register an edge-flag callback */
	tp.n_tri = 0;
	set_rgb8(tp.col);
}

static void tess_edge_cb(GLboolean flag)
{
	(void)flag;
}

static void tess_vertex_cb(void *vertex)
{
	tp.tri[tp.n_tri++] = (const TessVert *)vertex;
	if (tp.n_tri == 3)
	{
		float a[3] = {(float)tp.tri[0]->x, (float)tp.tri[0]->y, (float)tp.tri[0]->z};
		float b[3] = {(float)tp.tri[1]->x, (float)tp.tri[1]->y, (float)tp.tri[1]->z};
		float c[3] = {(float)tp.tri[2]->x, (float)tp.tri[2]->y, (float)tp.tri[2]->z};
		gd_tri(a, b, c);
		if (tp.keep_key)
		{
			tcache_add(a);
			tcache_add(b);
			tcache_add(c);
		}
		tp.n_tri = 0;
	}
}

static void tess_end_cb(void) {}
static void tess_error_cb(GLenum err)
{
	(void)err;
}

/* A new vertex where two edges cross on screen. Its screen position is
 * exact; lift it back to 3D with the depth interpolated the perspective
 * correct way (1 / depth is linear on screen). */
static void tess_combine_cb(GLdouble coords[3], void *data[4], GLfloat w[4], void **out)
{
	if (tp.n_combine >= TESS_MAX_COMBINE)
		tp.n_combine = 0; /* pathological polygon; better than crashing */
	TessVert *v = &tp.combine[tp.n_combine++];
	double inv_depth = 0, wsum = 0;
	for (int i = 0; i < 4; i++)
	{
		const TessVert *d = data[i];
		if (!d || w[i] == 0)
			continue;
		inv_depth += w[i] / -d->z;
		wsum += w[i];
	}
	double depth = (inv_depth > 0 && wsum > 0) ? wsum / inv_depth : COMPLEX_NEAR;
	v->sx = coords[0];
	v->sy = coords[1];
	v->sz = 0;
	v->x = coords[0] * depth;
	v->y = coords[1] * depth;
	v->z = -depth;
	*out = v;
}

void gl_prims_init(void)
{
	tess = gluNewTess();
	gluTessCallback(tess, GLU_TESS_BEGIN, (_GLUfuncptr)tess_begin_cb);
	gluTessCallback(tess, GLU_TESS_VERTEX, (_GLUfuncptr)tess_vertex_cb);
	gluTessCallback(tess, GLU_TESS_END, (_GLUfuncptr)tess_end_cb);
	gluTessCallback(tess, GLU_TESS_ERROR, (_GLUfuncptr)tess_error_cb);
	gluTessCallback(tess, GLU_TESS_COMBINE, (_GLUfuncptr)tess_combine_cb);
	gluTessCallback(tess, GLU_TESS_EDGE_FLAG, (_GLUfuncptr)tess_edge_cb);
	gluTessNormal(tess, 0, 0, 1); /* screen space: everything is in z = 0 */
	gluTessProperty(tess, GLU_TESS_WINDING_RULE, GLU_TESS_WINDING_ODD);
}

void gl_prims_shutdown(void)
{
	if (tess)
		gluDeleteTess(tess);
	tess = NULL;
}

static void wire_flush_contour(void)
{
	set_rgb8(tp.col);
	if (tp.wire_count > 1)
		gd_line(tp.wire_last, tp.wire_first);
	tp.wire_count = 0;
}

static void tess_emit(const float v[3])
{
	if (tp.n_verts >= TESS_MAX_VERTS)
		return;
	TessVert *o = &tp.verts[tp.n_verts++];
	double depth = -v[2];
	o->sx = v[0] / depth;
	o->sy = v[1] / depth;
	o->sz = 0;
	o->x = v[0];
	o->y = v[1];
	o->z = v[2];
	gluTessVertex(tess, &o->sx, o);
}

/* Cuts the collected outline at the near plane (Sutherland-Hodgman against
 * one plane, the outline is closed) and hands what is left to GLU as one
 * contour. */
static void flush_contour(void)
{
	int n = tp.n_contour;
	tp.n_contour = 0;
	if (n < 3)
		return;
	gluTessBeginContour(tess);
	for (int i = 0; i < n; i++)
	{
		const float *a = tp.contour[i];
		const float *b = tp.contour[(i + 1) % n];
		double da = -a[2], db = -b[2];
		bool in_a = da >= COMPLEX_NEAR, in_b = db >= COMPLEX_NEAR;
		if (in_a)
			tess_emit(a);
		if (in_a != in_b)
		{
			double t = (COMPLEX_NEAR - da) / (db - da);
			float c[3];
			for (int k = 0; k < 3; k++)
				c[k] = (float)(a[k] + (b[k] - a[k]) * t);
			c[2] = (float)-COMPLEX_NEAR;
			tess_emit(c);
		}
	}
	gluTessEndContour(tess);
}

static void complex_add_vertex(const float v[3])
{
	if (tp.have_prev && v[0] == tp.prev[0] && v[1] == tp.prev[1] && v[2] == tp.prev[2])
		return;
	memcpy(tp.prev, v, sizeof(tp.prev));
	tp.have_prev = true;

	if (gd_wireframe())
	{
		set_rgb8(tp.col);
		if (tp.wire_count++ == 0)
			memcpy(tp.wire_first, v, sizeof(tp.wire_first));
		else
			gd_line(tp.wire_last, v);
		memcpy(tp.wire_last, v, sizeof(tp.wire_last));
		return;
	}

	if (tp.n_contour < CONTOUR_MAX)
		memcpy(tp.contour[tp.n_contour++], v, sizeof(tp.contour[0]));
}

void gl_prims_finish(void)
{
	if (tp.kept)
	{
		const TessEntry *e = tp.kept;
		tp.kept = NULL;
		set_rgb8(tp.col);
		for (uint32_t i = 0; i + 2 < e->count; i += 3)
			gd_tri(tcache_verts[e->first + i], tcache_verts[e->first + i + 1], tcache_verts[e->first + i + 2]);
		return;
	}
	if (!tp.active)
		return;
	tp.active = false;
	if (gd_wireframe())
	{
		wire_flush_contour();
		return;
	}
	flush_contour();
	gluTessEndPolygon(tess);
	if (tp.keep_key)
	{
		TessEntry *e = tcache_slot(tp.keep_key);
		if (e && e->scene != tcache_scene && tcache_n > tp.keep_first)
		{
			e->key = tp.keep_key;
			e->scene = tcache_scene;
			e->first = tp.keep_first;
			e->count = tcache_n - tp.keep_first;
			tcache_used++;
		}
		tp.keep_key = NULL;
	}
}

void draw_complex_start(const void *payload)
{
	const Rgb8 *c = payload;
	gl_prims_finish(); /* previous polygon never got its END record */
	tp.col = *c;
	tp.keep_key = NULL;
	if (!gd_wireframe())
	{
		const TessEntry *e = tcache_slot(payload);
		if (e && e->scene == tcache_scene)
		{
			tp.kept = e; /* the rest of its records are skipped */
			return;
		}
		if (e)
		{
			tp.keep_key = payload;
			tp.keep_first = tcache_n;
		}
	}
	tp.active = true;
	tp.have_prev = false;
	tp.n_verts = tp.n_combine = tp.n_contour = 0;
	tp.wire_count = 0;
	if (!gd_wireframe())
		gluTessBeginPolygon(tess, NULL);
}

void draw_complex_vertex(const void *payload)
{
	if (!tp.active)
		return;
	float v[3];
	vec3i_to_f(*(const Vec3i *)payload, v);
	complex_add_vertex(v);
}

#define COMPLEX_BEZIER_STEPS 10
void draw_complex_bezier(const void *payload)
{
	if (!tp.active)
		return;
	const Vec3i *pv = payload;
	float cp[4][3], v[3];
	for (int i = 0; i < 4; i++)
		vec3i_to_f(pv[i], cp[i]);
	for (int i = 0; i <= COMPLEX_BEZIER_STEPS; i++)
	{
		eval_bezier(v, i / (float)COMPLEX_BEZIER_STEPS, cp);
		complex_add_vertex(v);
	}
}

void draw_complex_inner(const void *payload)
{
	(void)payload;
	if (!tp.active)
		return;
	tp.have_prev = false;
	if (gd_wireframe())
	{
		wire_flush_contour();
		return;
	}
	flush_contour();
}

void draw_complex_end(const void *payload)
{
	(void)payload;
	gl_prims_finish();
}
