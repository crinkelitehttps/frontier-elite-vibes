/*
 * gl_planet.c - planets, drawn from the game's own 2D planet shape (with
 * all its surface detail and shading); a ray traced sphere is the fallback
 * when that shape can't be read.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gl_api.h"
#include "gl_cockpit.h"
#include "gl_draw.h"
#include "gl_fill2d.h"
#include "gl_native.h"
#include "gl_planet.h"
#include "gl_prims.h"
#include "gl_scene.h"
#include "main.h"
#include "m68000.h"

/* =========================================================================
 * Capture (runs inside the emulator)
 * ========================================================================= */

#define ADDR_PRIMITIVES_END 0x2caa4 /* end of the software display list */
#define ADDR_DYN_COLS       0x56    /* dynamic colour table */

enum
{
	PL_START = 156,
	PL_LINE = 166,
	PL_BEZIER = 182,
	PL_ROW = 198,
	PL_COLOURS = 208
};

/* Copies the 2D shape list the game just built for this planet (it starts
 * at the address saved in A6-190 and ends at the display list end). Colour
 * tables hold dynamic colour ids that are only valid right now, so they are
 * replaced by their rgb444 values in the copy. */
/* Words in the list the game just built, 0 if none or too long */
static int list_words(uint32_t a6)
{
	uint32_t start = (uint32_t)STMemory_ReadLong(a6 - 190);
	uint32_t end = (uint32_t)STMemory_ReadLong(ADDR_PRIMITIVES_END);
	if (end <= start)
		return 0;
	if ((end - start) / 2 > PLANET_LIST_MAX)
	{
		static bool warned;
		if (!warned)
			log_printf("Planet: shape list of %u words is too long, drawing a plain sphere\n",
					   (unsigned)((end - start) / 2));
		warned = true;
		return 0;
	}
	return (int)(end - start) / 2;
}

static void capture_list(PlanetDesc *p, int n)
{
	uint32_t start = (uint32_t)STMemory_ReadLong(p->ctx.a[6] - 190);
	p->list_len = 0;
	p->list_ok = false;
	if (n == 0)
		return;

	for (int i = 0; i < n; i++)
		p->list[i] = STMemory_ReadWord(start + i * 2);
	p->list_len = (uint16_t)n;

	int i = 0, fills = 0;
	while (i < n)
	{
		switch (p->list[i++])
		{
		case PL_START:
			break;
		case PL_LINE:
			i += 5;
			break;
		case PL_BEZIER:
			i += 9;
			break;
		case PL_ROW:
			i += 2;
			break;
		case PL_COLOURS:
			if (i + 16 > n)
				return;
			for (int k = 0; k < 16; k++)
				p->list[i + k] = STMemory_ReadWord(ADDR_DYN_COLS + p->list[i + k] + 2) & 0xfff;
			i += 16;
			fills++;
			break;
		default:
			return; /* unknown opcode: fall back to the plain sphere */
		}
	}
	p->list_ok = (i == n && fills > 0);
}

/* The planet command's data (gl_planet_tex.h). Called at the end of the
 * planet routine (fuck_planet), whose frame (A3) has been given back: its
 * first words still hold the colours and the data's size, and A5 has gone
 * just past the data. */
static void capture_surface(PlanetDesc *p)
{
	PlanetSurface *s = &p->surface;
	p->surface_ok = false;
	uint32_t a3 = (uint32_t)GetReg(REG_A3), a5 = (uint32_t)GetReg(REG_A5), a6 = (uint32_t)GetReg(REG_A6);
	int len = (uint16_t)STMemory_ReadWord(a3 + 32);
	if (len <= 0 || len > PLANET_DATA_MAX || a5 < (uint32_t)len + 6)
		return;
	uint32_t start = a5 - (uint32_t)len;
	for (int i = 0; i < len; i++)
		s->data[i] = (uint8_t)STMemory_ReadByte(start + (uint32_t)i);
	s->len = len;
	s->radius_word = (uint16_t)STMemory_ReadWord(start - 4);
	uint32_t obj = (uint32_t)STMemory_ReadLong(a6 - 212);
	s->seed = (uint32_t)STMemory_ReadLong(obj + 118);
	/* the second table, with the sunlight added, is only made when the
	 * flags word has bit 7 or 6 (L3d0be) */
	s->lit_colours = (s->data[0] & 0xc0) != 0;
	for (int i = 0; i < 16; i++)
		s->colours[i] = (uint16_t)(STMemory_ReadWord(a3 + 2 * (uint32_t)(s->lit_colours ? i : i & 7)) & 0xfff);
	p->surface_ok = planet_surface_valid(s);
}

/* The colours the game fills this planet with (its 2D shape's first colour
 * table, rgb444 by now), else its colour tables from the frame (without a
 * lit table, a darker copy for the dark side) */
static void capture_palette(PlanetDesc *p)
{
	const PlanetSurface *s = &p->surface;
	for (int i = 0; i < 8; i++)
	{
		p->palette[i] = s->lit_colours ? s->colours[i] : (uint16_t)((s->colours[i] >> 2) & 0x333);
		p->palette[i + 8] = s->colours[i + 8];
	}
	if (!p->list_ok)
		return;
	for (int i = 0; i < p->list_len;)
	{
		switch (p->list[i])
		{
		case PL_START:
			i += 1;
			break;
		case PL_LINE:
			i += 6;
			break;
		case PL_BEZIER:
			i += 10;
			break;
		case PL_ROW:
			i += 3;
			break;
		case PL_COLOURS:
			for (int k = 0; k < 16; k++)
				p->palette[k] = (uint16_t)(p->list[i + 1 + k] & 0xfff);
			return;
		default:
			return;
		}
	}
}

/* Each planet's distance in the cockpit's straight ahead pass. The turned
 * passes get the camera matrix rounded to words, no longer quite a rotation,
 * so each puts the planet a little nearer or further (up to about 3 in
 * 100000: a couple of hundred metres on Merlin, more than the height of a
 * ship just off the ground, which then sees the ground all over some
 * passes). The ahead pass is the game's own camera, drawn last; every pass
 * draws the planet at its distance. Its colours too: the game picks them
 * for the view it draws (its 2D shape's colour table), so each pass would
 * shade the ground a little differently and show where the passes meet. */
#define DIST_PLANETS 16
typedef struct
{
	int n;
	float radius[DIST_PLANETS];
	double dist[DIST_PLANETS];
	uint16_t palette[DIST_PLANETS][16];
	int light_col[DIST_PLANETS], obj_col[DIST_PLANETS];
} PlanetDists;

static PlanetDists dists_rec, dists_pub, dists_show;

/* The native scene's planets (GLF_NATIVE_SCENE=2): each planet once a
 * frame, from the pass of the game's own camera when that one has it, else
 * the first pass that does, in the game camera's view space; drawn by
 * native_draw_sky instead of the passes. The planet routine leaves some
 * views out (all ground, L3cd8e), so a planet object captured without its
 * planet gets its last one, carried along by the object's turn
 * (planet_native_object). */
#define NATIVE_PLANETS 8
typedef struct
{
	uint32_t obj;  /* the planet object */
	bool ahead;    /* from the game camera's own pass */
	double pos[3]; /* centre, GL view space */
	float radius;
	float light[3];
	float axes[9]; /* as PlanetDesc.axes, in the game camera's view */
	int light_col, obj_col;
	uint16_t palette[16];
	PlanetSurface surface;
	bool surface_ok;
	/* the object as gl_native.cpp captures it: its view position and axes */
	float obj_pos[3], obj_cols[3][3];
	uint32_t seen;  /* render frame it was last captured in (remembered ones) */
	bool stand_in;  /* a remembered one standing in this frame */
} NativePlanet;

typedef struct
{
	int n;
	NativePlanet p[NATIVE_PLANETS];
} NativePlanets;

static NativePlanets native_rec, native_pub, native_show, native_last;
static uint32_t native_frame;

void planet_dists_reset(void)
{
	dists_rec.n = 0;
	native_rec.n = 0;
}

void planet_dists_publish(void)
{
	dists_pub = dists_rec;
	native_pub.n = native_rec.n;
	memcpy(native_pub.p, native_rec.p, sizeof native_rec.p[0] * (size_t)native_rec.n);
}

void planet_dists_acquire(void)
{
	dists_show = dists_pub;
	native_show.n = native_pub.n;
	memcpy(native_show.p, native_pub.p, sizeof native_pub.p[0] * (size_t)native_pub.n);
	native_frame++;
}

/* A turned pass's game view vector (z ahead) in the game camera's */
static void pass_to_view_game(const float v[3], float out[3])
{
	float g[3] = {v[0], v[1], -v[2]};
	cockpit_pass_to_view(g, out);
	out[2] = -out[2];
}

static void record_native(const PlanetDesc *p)
{
	if (native_mode() != 2)
		return;
	uint32_t a6 = p->ctx.a[6];
	uint32_t obj = (uint32_t)STMemory_ReadLong(a6 - 212);
	bool ahead = !cockpit_turned_pass();
	int i = 0;
	while (i < native_rec.n && native_rec.p[i].obj != obj)
		i++;
	if (i < native_rec.n ? native_rec.p[i].ahead || !ahead : native_rec.n >= NATIVE_PLANETS)
		return;
	if (i == native_rec.n)
		native_rec.n++;
	NativePlanet *n = &native_rec.p[i];
	n->obj = obj;
	n->ahead = ahead;
	n->stand_in = false;
	float pos[3], v[3];
	vec3i_to_f(p->pos, pos);
	cockpit_pass_to_view(pos, v);
	for (int k = 0; k < 3; k++)
		n->pos[k] = v[k];
	n->radius = p->radius;
	cockpit_pass_to_view(p->light, n->light);
	for (int c = 0; c < 3; c++)
		pass_to_view_game(&p->axes[3 * c], &n->axes[3 * c]);
	n->light_col = p->light_col;
	n->obj_col = p->obj_col;
	memcpy(n->palette, p->palette, sizeof n->palette);
	n->surface_ok = p->surface_ok;
	if (p->surface_ok)
		n->surface = p->surface;
	/* as capture() in gl_native.cpp */
	float op[3] = {(float)(int32_t)STMemory_ReadLong(a6 - 16), (float)(int32_t)STMemory_ReadLong(a6 - 12),
				   -(float)(int32_t)STMemory_ReadLong(a6 - 8)};
	cockpit_pass_to_view(op, n->obj_pos);
	for (int c = 0; c < 3; c++)
	{
		uint32_t m = a6 - 36 + 6 * (uint32_t)c;
		float col[3] = {(int16_t)STMemory_ReadWord(m) / 32768.0f, (int16_t)STMemory_ReadWord(m + 2) / 32768.0f,
						-(int16_t)STMemory_ReadWord(m + 4) / 32768.0f};
		cockpit_pass_to_view(col, n->obj_cols[c]);
	}
}

/* Q v: the object's turn since `old` was captured, Q = new old^T */
static void turn_by(const float old_cols[3][3], const float cols[3][3], const float v[3], float out[3])
{
	float a[3];
	for (int i = 0; i < 3; i++)
		a[i] = old_cols[i][0] * v[0] + old_cols[i][1] * v[1] + old_cols[i][2] * v[2];
	for (int k = 0; k < 3; k++)
		out[k] = cols[0][k] * a[0] + cols[1][k] * a[1] + cols[2][k] * a[2];
}

/* How long a remembered planet stands in, render frames */
#define NATIVE_REMEMBER 120

void planet_native_object(uint32_t addr, const float pos[3], const float cols[3][3])
{
	for (int i = 0; i < native_show.n; i++)
		if (native_show.p[i].obj == addr)
		{
			if (native_show.p[i].stand_in)
				return;
			/* remember it */
			int j = 0;
			while (j < native_last.n && native_last.p[j].obj != addr)
				j++;
			if (j == native_last.n)
			{
				if (native_last.n >= NATIVE_PLANETS)
					j = 0; /* forget the first */
				else
					native_last.n++;
			}
			native_last.p[j] = native_show.p[i];
			native_last.p[j].seen = native_frame;
			return;
		}
	for (int j = 0; j < native_last.n; j++)
	{
		const NativePlanet *o = &native_last.p[j];
		if (o->obj != addr || native_frame - o->seen > NATIVE_REMEMBER || native_show.n >= NATIVE_PLANETS)
			continue;
		NativePlanet *n = &native_show.p[native_show.n++];
		*n = *o;
		n->stand_in = true;
		float d[3] = {(float)(o->pos[0] - o->obj_pos[0]), (float)(o->pos[1] - o->obj_pos[1]),
					  (float)(o->pos[2] - o->obj_pos[2])}, t[3];
		turn_by(o->obj_cols, cols, d, t);
		for (int k = 0; k < 3; k++)
			n->pos[k] = (double)pos[k] + t[k];
		turn_by(o->obj_cols, cols, o->light, n->light);
		for (int c = 0; c < 3; c++)
		{
			float g[3] = {o->axes[3 * c], o->axes[3 * c + 1], -o->axes[3 * c + 2]};
			turn_by(o->obj_cols, cols, g, &n->axes[3 * c]);
			n->axes[3 * c + 2] = -n->axes[3 * c + 2];
		}
		return;
	}
}

uint32_t planet_native_object_addr(int i)
{
	return native_show.p[i].obj;
}

int planet_native_count(void)
{
	return native_mode() == 2 ? native_show.n : 0;
}

double planet_native_dist(int i)
{
	const double *p = native_show.p[i].pos;
	return sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
}

/* The same planet in another pass: the same radius, give or take rounding */
static int find_dist(const PlanetDists *d, float radius)
{
	for (int i = 0; i < d->n; i++)
		if (fabsf(d->radius[i] - radius) <= radius * 1e-3f)
			return i;
	return -1;
}

static void record_dist(const PlanetDesc *p)
{
	if (cockpit_turned_pass() || p->radius <= 0.0f)
		return;
	int i = find_dist(&dists_rec, p->radius);
	if (i < 0)
	{
		if (dists_rec.n >= DIST_PLANETS)
			return;
		i = dists_rec.n++;
	}
	double x = p->pos.x, y = p->pos.y, z = p->pos.z;
	dists_rec.radius[i] = p->radius;
	dists_rec.dist[i] = sqrt(x * x + y * y + z * z);
	memcpy(dists_rec.palette[i], p->palette, sizeof dists_rec.palette[i]);
	dists_rec.light_col[i] = p->light_col;
	dists_rec.obj_col[i] = p->obj_col;
}

void Nu_PutPlanet(void)
{
	int words = list_words((uint32_t)GetReg(REG_A6));
	PlanetDesc *p = scene_record(PRIM_PLANET, sizeof *p + (size_t)words * sizeof p->list[0]);
	if (!p)
		return;

	for (int i = 0; i < 8; i++)
	{
		p->ctx.d[i] = GetReg(REG_D0 + i);
		p->ctx.a[i] = (uint32_t)GetReg(REG_A0 + i);
	}

	p->obj_col = GetReg(REG_D6);
	p->light_col = GetReg(REG_D1);
	p->radius = (float)GetReg(REG_D0);
	gl_read_light_dir(GetReg(REG_A1), p->light);
	p->pos = m68k_vertex(GetReg(REG_A0) + 4);

	/* 3x3 rotation matrix of signed 1.15 fixed point words, stored at A6-36 */
	uint32_t m = GetReg(REG_A6) - 36;
	for (int i = 0; i < 9; i++)
	{
		p->rot[i] = STMemory_ReadWord(m + i * 2) / -32768.0f;
		p->axes[i] = STMemory_ReadWord(m + i * 2) / 32768.0f;
	}

	capture_list(p, words);
	capture_surface(p);
	capture_palette(p);
	record_dist(p);
	record_native(p);
	/* the rest of the planet's model (rings) is the native scene's */
	if (native_mode() == 2)
		scene_mute_object(p->ctx.a[6]);

	float centre[3];
	vec3i_to_f(p->pos, centre);
	atmos_place_bands(centre, p->radius);
}

/* =========================================================================
 * GPU resources
 *
 * Planets are exact spheres ray traced per pixel, drawn over a screen
 * rectangle that bounds the planet. That gives a perfectly round silhouette
 * at any distance (no mesh facets or level-of-detail pops) and puts the
 * ground exactly where the game's surface objects expect it when you are
 * landed. Everything is in units of the planet radius so the maths stays
 * precise in float even metres above a planet millions of units big.
 * ========================================================================= */
static GLuint sphere_prog, shell_prog, quad_vao, quad_vbo;
static struct
{
	GLint rect, proj, centre, c, light_dir, diffuse, ambient, surface, textured, to_local, palette;
} us;
static struct
{
	GLint rect, proj, dir, cos_edges, colour;
} ush;

static const char *VS_SPHERE =
	"in vec2 aPos;\n"       /* unit quad 0..1 */
	"uniform vec4 uRect;\n" /* NDC x0 y0 x1 y1 to cover */
	"uniform vec4 uProj;\n" /* m[0], m[5], m[8], m[9] of the projection */
	"out vec3 vRay;\n"
	"void main(){\n"
	"  vec2 ndc = mix(uRect.xy, uRect.zw, aPos);\n"
	"  gl_Position = vec4(ndc, 0.0, 1.0);\n"
	"  vRay = vec3((ndc.x + uProj.z) / uProj.x, (ndc.y + uProj.w) / uProj.y, -1.0);\n"
	"}\n";

static const char *FS_SPHERE =
	"precision highp float;\n"
	"in vec3 vRay;\n"
	"uniform vec3 uCentre;\n" /* sphere centre / radius, eye space */
	"uniform float uC;\n"     /* |centre|^2 - 1, computed in double on the CPU */
	"uniform vec3 uLightDir;\n"
	"uniform vec3 uDiffuse;\n"
	"uniform vec3 uAmbient;\n"
	"uniform sampler2D uSurface;\n" /* the surface, equirectangular in the planet's axes */
	"uniform int uTextured;\n"
	"uniform mat3 uToLocal;\n" /* eye space -> the planet's axes */
	"uniform vec3 uPalette[16];\n" /* by code / 4: dark 0-7, lit 8-15 */
	"out vec4 fragColor;\n"
	/* a texel's colour: the code through the palette (dark half, lit half) */
	"vec3 texel(ivec2 c, ivec2 size, float lit){\n"
	"  c.x = ((c.x % size.x) + size.x) % size.x;\n"
	"  c.y = clamp(c.y, 0, size.y - 1);\n"
	"  int k = (int(texelFetch(uSurface, c, 0).r * 255.0 + 0.5) >> 2) & 7;\n"
	"  return mix(uPalette[k], uPalette[k + 8], lit);\n"
	"}\n"
	"void main(){\n"
	"  vec3 d = normalize(vRay);\n"
	"  float b = dot(d, uCentre);\n"
	/* 1 - (distance from centre to the ray)^2; the cross product form avoids
	 * cancelling two large numbers when the planet is far away */
	"  vec3 x = cross(uCentre, d);\n"
	"  float disc = 1.0 - dot(x, x);\n"
	"  if (disc < 0.0) discard;\n"
	"  float s = sqrt(disc);\n"
	"  float t = (uC > 0.0) ? b - s : b + s;\n" /* outside: near hit; inside: far hit */
	"  if (t <= 0.0) discard;\n"
	"  vec3 n = t * d - uCentre;\n"
	"  float l = max(dot(normalize(n), uLightDir), 0.0);\n"
	"  if (uTextured != 0) {\n"
	"    vec3 p = normalize(uToLocal * n);\n"
	"    vec2 uv = vec2(atan(p.x, p.z) / 6.2831853 + 0.5, acos(clamp(p.y, -1.0, 1.0)) / 3.14159265);\n"
	"    float lit = smoothstep(-0.03, 0.03, dot(normalize(n), uLightDir));\n"
	/* codes can't be blended: blend the four nearest texels' colours */
	"    ivec2 size = textureSize(uSurface, 0);\n"
	"    vec2 t = uv * vec2(size) - 0.5;\n"
	"    ivec2 i = ivec2(floor(t));\n"
	"    vec2 f = t - floor(t);\n"
	"    vec3 a = mix(texel(i, size, lit), texel(i + ivec2(1, 0), size, lit), f.x);\n"
	"    vec3 b = mix(texel(i + ivec2(0, 1), size, lit), texel(i + ivec2(1, 1), size, lit), f.x);\n"
	"    fragColor = vec4(mix(a, b, f.y), 1.0);\n"
	"  } else\n"
	"    fragColor = vec4(clamp(uAmbient + l * uDiffuse, 0.0, 1.0), 1.0);\n"
	"}\n";

/* An atmosphere band: the directions between two angles from the planet's */
static const char *FS_SHELL = "precision highp float;\n"
							  "in vec3 vRay;\n"
							  "uniform vec3 uDir;\n" /* to the planet's centre, eye space, unit */
							  "uniform vec2 uCosEdges;\n" /* outer, inner */
							  "uniform vec3 uColour;\n"
							  "out vec4 fragColor;\n"
							  "void main(){\n"
							  "  float c = dot(normalize(vRay), uDir);\n"
							  "  if (c < uCosEdges.x || c > uCosEdges.y) discard;\n"
							  "  fragColor = vec4(uColour, 1.0);\n"
							  "}\n";

void gl_planet_init(void)
{
	static const char *const attribs[] = {"aPos"};
	sphere_prog = gd_build_program(VS_SPHERE, FS_SPHERE, attribs, 1);
	shell_prog = gd_build_program(VS_SPHERE, FS_SHELL, attribs, 1);
	ush.rect = glGetUniformLocation(shell_prog, "uRect");
	ush.proj = glGetUniformLocation(shell_prog, "uProj");
	ush.dir = glGetUniformLocation(shell_prog, "uDir");
	ush.cos_edges = glGetUniformLocation(shell_prog, "uCosEdges");
	ush.colour = glGetUniformLocation(shell_prog, "uColour");
	us.rect = glGetUniformLocation(sphere_prog, "uRect");
	us.proj = glGetUniformLocation(sphere_prog, "uProj");
	us.centre = glGetUniformLocation(sphere_prog, "uCentre");
	us.c = glGetUniformLocation(sphere_prog, "uC");
	us.light_dir = glGetUniformLocation(sphere_prog, "uLightDir");
	us.diffuse = glGetUniformLocation(sphere_prog, "uDiffuse");
	us.ambient = glGetUniformLocation(sphere_prog, "uAmbient");
	us.surface = glGetUniformLocation(sphere_prog, "uSurface");
	us.textured = glGetUniformLocation(sphere_prog, "uTextured");
	us.to_local = glGetUniformLocation(sphere_prog, "uToLocal");
	us.palette = glGetUniformLocation(sphere_prog, "uPalette");

	static const float quad[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
	glGenVertexArrays(1, &quad_vao);
	glGenBuffers(1, &quad_vbo);
	glBindVertexArray(quad_vao);
	glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);
	glBindVertexArray(0);
}

void gl_planet_shutdown(void)
{
	glDeleteBuffers(1, &quad_vbo);
	glDeleteVertexArrays(1, &quad_vao);
	glDeleteProgram(sphere_prog);
	glDeleteProgram(shell_prog);
	planet_tex_shutdown();
}

/* =========================================================================
 * Drawing (runs at present time)
 * ========================================================================= */
typedef struct
{
	float rect[4];
	float proj[4];
	float centre[3];
	float c;
	float light[3], diffuse[3], ambient[3];
	GLuint surface; /* 0: plain */
	float to_local[9];
	float palette[16][3];
} SpherePass;

static void planet_sphere_pass(const void *data)
{
	const SpherePass *p = data;
	glUseProgram(sphere_prog);
	glUniform4fv(us.rect, 1, p->rect);
	glUniform4fv(us.proj, 1, p->proj);
	glUniform3fv(us.centre, 1, p->centre);
	glUniform1f(us.c, p->c);
	glUniform3fv(us.light_dir, 1, p->light);
	glUniform3fv(us.diffuse, 1, p->diffuse);
	glUniform3fv(us.ambient, 1, p->ambient);
	glUniform1i(us.textured, p->surface != 0);
	if (p->surface)
	{
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, p->surface);
		glUniform1i(us.surface, 0);
		glUniformMatrix3fv(us.to_local, 1, GL_FALSE, p->to_local);
		glUniform3fv(us.palette, 16, &p->palette[0][0]);
	}
	glBindVertexArray(quad_vao);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* NDC rectangle covering the sphere: the projected corners of its bounding
 * cube. Falls back to the whole view if any corner is at or behind the
 * camera. Returns false if the planet is entirely off screen. */
static bool screen_rect(const double centre[3], float radius, float rect[4])
{
	const mat4 *pr = gd_projection();
	double r = radius * 1.01;
	float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
	for (int i = 0; i < 8; i++)
	{
		float c[3] = {(float)(centre[0] + ((i & 1) ? r : -r)), (float)(centre[1] + ((i & 2) ? r : -r)),
					  (float)(centre[2] + ((i & 4) ? r : -r))};
		float clip[4];
		mat4_xform(pr, c[0], c[1], c[2], clip);
		if (clip[3] <= 1e-3f * fabsf(c[2]) || c[2] >= 0.0f)
		{
			rect[0] = rect[1] = -1.0f;
			rect[2] = rect[3] = 1.0f;
			return true;
		}
		float x = clip[0] / clip[3], y = clip[1] / clip[3];
		x0 = fminf(x0, x);
		y0 = fminf(y0, y);
		x1 = fmaxf(x1, x);
		y1 = fmaxf(y1, y);
	}
	rect[0] = fmaxf(x0, -1.0f);
	rect[1] = fmaxf(y0, -1.0f);
	rect[2] = fminf(x1, 1.0f);
	rect[3] = fminf(y1, 1.0f);
	return rect[0] < rect[2] && rect[1] < rect[3];
}

static int table_colour(int code, const void *ctx)
{
	return ((const int16_t *)ctx)[(code >> 2) & 15] & 0xfff;
}

/* Draws the planet exactly as the game's software renderer would, from its
 * 2D shape list: outline, shading bands, surface features and, close up,
 * the ground with the horizon where the game puts it. */
static void draw_list(const PlanetDesc *p)
{
	fill2d_begin();
	for (int i = 0; i < p->list_len;)
	{
		const int16_t *a = &p->list[i + 1];
		switch (p->list[i])
		{
		case PL_START:
			fill2d_begin();
			i += 1;
			break;
		case PL_LINE:
			fill2d_line(a[0], a[1], a[2], a[3], a[4]);
			i += 6;
			break;
		case PL_BEZIER:
			fill2d_bezier(a, a[8]);
			i += 10;
			break;
		case PL_ROW:
			fill2d_row_code(a[0], a[1]);
			i += 3;
			break;
		case PL_COLOURS:
			fill2d_draw(FILL2D_PLANET, table_colour, a);
			i += 17;
			break;
		default:
			return;
		}
	}
}

/* A planet as the sphere pass draws it */
typedef struct
{
	double pos[3];  /* centre, view space as drawn (GL, not turned by the head) */
	float radius;
	float light[3]; /* view space */
	int light_col, obj_col;
	const uint16_t *palette; /* 16 rgb444 */
	const float *axes;       /* PlanetDesc.axes */
	const PlanetSurface *surface; /* null: plain */
} SphereIn;

static void draw_sphere(const SphereIn *in)
{
	/* The sphere shader takes a plain lens (no rotation): in the cockpit,
	 * turn the planet by the head instead */
	mat4 lens = *gd_projection(), head = mat4_identity();
	if (cockpit_active())
		cockpit_world_split(&lens, &head);

	float rect[4];
	if (!screen_rect(in->pos, in->radius, rect))
		return;

	/* the head's turn in double: the centre may be millions of units off */
	double r = in->radius, c[3];
	for (int k = 0; k < 3; k++)
		c[k] = (head.m[k] * in->pos[0] + head.m[4 + k] * in->pos[1] + head.m[8 + k] * in->pos[2]) / r;
	float light[3];
	mat4_xform_dir(&head, in->light, light);

	SpherePass *pass = gd_custom(planet_sphere_pass, sizeof *pass);
	if (!pass)
		return;
	memcpy(pass->rect, rect, sizeof(rect));
	pass->proj[0] = lens.m[0];
	pass->proj[1] = lens.m[5];
	pass->proj[2] = lens.m[8];
	pass->proj[3] = lens.m[9];

	for (int k = 0; k < 3; k++)
		pass->centre[k] = (float)c[k];
	pass->c = (float)(c[0] * c[0] + c[1] * c[1] + c[2] * c[2] - 1.0);

	memcpy(pass->light, light, sizeof(pass->light));
	vec3_normalize(pass->light);
	gl_rgb444_to_f(in->light_col, pass->diffuse);
	gl_rgb444_to_f(in->obj_col, pass->ambient);

	/* The surface: eye space -> view space (H^T) -> the game's view (z
	 * flipped, S) -> the planet's axes (A^T) */
	pass->surface = in->surface ? planet_surface_texture(in->surface) : 0;
	for (int i = 0; i < 16; i++)
		gl_rgb444_to_f(in->palette[i], pass->palette[i]);
	static const float s[3] = {1, 1, -1};
	for (int i = 0; i < 3; i++)
		for (int k = 0; k < 3; k++)
		{
			float v = 0;
			for (int j = 0; j < 3; j++)
				v += in->axes[3 * i + j] * s[j] * head.m[j * 4 + k];
			pass->to_local[k * 3 + i] = v;
		}
}

void draw_planet(const void *payload)
{
	const PlanetDesc *p = payload;
	if (p->radius <= 0.0f || native_mode() == 2) /* native_draw_sky draws it */
		return;

	/* The game's shape only fits the view it was made for: the cockpit's
	 * turned passes would each draw a slightly different one, so there the
	 * planet is the sphere, which every pass draws the same. */
	static int native = -1;
	if (native < 0)
		native = getenv("GLF_NATIVE_PLANETS") && atoi(getenv("GLF_NATIVE_PLANETS")) > 0;
	if (p->list_ok && !cockpit_active() && !(native && p->surface_ok))
	{
		draw_list(p);
		return;
	}

	SphereIn in = {{p->pos.x, p->pos.y, p->pos.z}, p->radius, {p->light[0], p->light[1], p->light[2]},
				   p->light_col, p->obj_col, p->palette, p->axes, p->surface_ok ? &p->surface : NULL};
	if (cockpit_active())
	{
		int i = find_dist(&dists_show, p->radius);
		double len = sqrt(in.pos[0] * in.pos[0] + in.pos[1] * in.pos[1] + in.pos[2] * in.pos[2]);
		if (i >= 0)
		{
			in.palette = dists_show.palette[i];
			in.light_col = dists_show.light_col[i];
			in.obj_col = dists_show.obj_col[i];
			if (len > 0.0)
				for (int k = 0; k < 3; k++)
					in.pos[k] *= dists_show.dist[i] / len;
		}
	}
	draw_sphere(&in);
}

void planet_native_draw(int i)
{
	const NativePlanet *n = &native_show.p[i];
	SphereIn in = {{n->pos[0], n->pos[1], n->pos[2]},
				   n->radius,
				   {n->light[0], n->light[1], n->light[2]},
				   n->light_col,
				   n->obj_col,
				   n->palette,
				   n->axes,
				   n->surface_ok ? &n->surface : NULL};
	draw_sphere(&in);
}

typedef struct
{
	float proj[4];
	float dir[3];
	float cos_edges[2];
	float colour[3];
} ShellPass;

static void planet_shell_pass(const void *data)
{
	const ShellPass *p = data;
	static const float whole[4] = {-1, -1, 1, 1};
	glUseProgram(shell_prog);
	glUniform4fv(ush.rect, 1, whole);
	glUniform4fv(ush.proj, 1, p->proj);
	glUniform3fv(ush.dir, 1, p->dir);
	glUniform2fv(ush.cos_edges, 1, p->cos_edges);
	glUniform3fv(ush.colour, 1, p->colour);
	glBindVertexArray(quad_vao);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void planet_draw_shell(const float centre[3], float cos_outer, float cos_inner, int rgb)
{
	mat4 lens = *gd_projection(), head = mat4_identity(), turned;
	if (cockpit_active())
	{
		cockpit_world_split(&lens, &turned);
		head = cockpit_head_rotation();
	}
	float dir[3];
	mat4_xform_dir(&head, centre, dir);
	vec3_normalize(dir);

	ShellPass *pass = gd_custom(planet_shell_pass, sizeof *pass);
	if (!pass)
		return;
	pass->proj[0] = lens.m[0];
	pass->proj[1] = lens.m[5];
	pass->proj[2] = lens.m[8];
	pass->proj[3] = lens.m[9];
	memcpy(pass->dir, dir, sizeof dir);
	pass->cos_edges[0] = cos_outer;
	pass->cos_edges[1] = cos_inner;
	gl_rgb444_to_f(rgb, pass->colour);
}
