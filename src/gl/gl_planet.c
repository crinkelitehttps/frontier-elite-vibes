/*
 * gl_planet.c - planets, drawn from the game's own 2D planet shape (with
 * all its surface detail and shading); a ray traced sphere is the fallback
 * when that shape can't be read.
 */
#include <stdlib.h>

#include "gl_api.h"
#include "gl_cockpit.h"
#include "gl_draw.h"
#include "gl_fill2d.h"
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
	for (int i = 0; i < 8; i++)
		s->colours[i] = (uint16_t)(STMemory_ReadWord(a3 + 2 * (uint32_t)i) & 0xfff);
	p->surface_ok = planet_surface_valid(s);
}

/* The colours the game fills this planet with (its 2D shape's first colour
 * table, rgb444 by now), else the unlit ones */
static void capture_palette(PlanetDesc *p)
{
	for (int i = 0; i < 8; i++)
	{
		int c = p->surface.colours[i];
		p->palette[i] = (uint16_t)((c >> 2) & 0x333);
		p->palette[i + 8] = (uint16_t)c;
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
static bool screen_rect(const PlanetDesc *p, float rect[4])
{
	const mat4 *pr = gd_projection();
	float r = p->radius * 1.01f;
	float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
	for (int i = 0; i < 8; i++)
	{
		float c[3] = {p->pos.x + ((i & 1) ? r : -r), p->pos.y + ((i & 2) ? r : -r),
					  p->pos.z + ((i & 4) ? r : -r)};
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

void draw_planet(const void *payload)
{
	const PlanetDesc *p = payload;
	if (p->radius <= 0.0f)
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

	/* The sphere shader takes a plain lens (no rotation): in the cockpit,
	 * turn the planet by the head instead */
	mat4 lens = *gd_projection(), head = mat4_identity();
	if (cockpit_active())
		cockpit_world_split(&lens, &head);
	float view_pos[3], pos[3], light[3];
	vec3i_to_f(p->pos, view_pos);
	mat4_xform_dir(&head, view_pos, pos);
	mat4_xform_dir(&head, p->light, light);

	double r = p->radius;
	double cx = pos[0] / r, cy = pos[1] / r, cz = pos[2] / r;

	float rect[4];
	if (!screen_rect(p, rect))
		return;

	SpherePass *pass = gd_custom(planet_sphere_pass, sizeof *pass);
	if (!pass)
		return;
	memcpy(pass->rect, rect, sizeof(rect));
	const mat4 *pr = &lens;
	pass->proj[0] = pr->m[0];
	pass->proj[1] = pr->m[5];
	pass->proj[2] = pr->m[8];
	pass->proj[3] = pr->m[9];

	pass->centre[0] = (float)cx;
	pass->centre[1] = (float)cy;
	pass->centre[2] = (float)cz;
	pass->c = (float)(cx * cx + cy * cy + cz * cz - 1.0);

	memcpy(pass->light, light, sizeof(pass->light));
	vec3_normalize(pass->light);
	gl_rgb444_to_f(p->light_col, pass->diffuse);
	gl_rgb444_to_f(p->obj_col, pass->ambient);

	/* The surface: eye space -> view space (H^T) -> the game's view (z
	 * flipped, S) -> the planet's axes (A^T) */
	pass->surface = p->surface_ok ? planet_surface_texture(&p->surface) : 0;
	for (int i = 0; i < 16; i++)
		gl_rgb444_to_f(p->palette[i], pass->palette[i]);
	static const float s[3] = {1, 1, -1};
	for (int i = 0; i < 3; i++)
		for (int k = 0; k < 3; k++)
		{
			float v = 0;
			for (int j = 0; j < 3; j++)
				v += p->axes[3 * i + j] * s[j] * head.m[j * 4 + k];
			pass->to_local[k * 3 + i] = v;
		}
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
