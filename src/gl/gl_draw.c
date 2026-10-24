/*
 * gl_draw.c - batched draw list, see gl_draw.h.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gl_draw.h"
#include "main.h"

/* One vertex = clip-space position + RGBA8 colour + point size (24 bytes) */
typedef struct
{
	float pos[4];
	uint8_t col[4];
	float size;
	float depth[2]; /* depth tested: window depth a + b * log(w); b 0 = gl_FragCoord.z */
} GdVert;

enum GdKind
{
	GD_TRIS,
	GD_LINES,
	GD_POINTS,
	GD_CUSTOM
};

typedef struct
{
	uint8_t kind;
	uint8_t vp;
	uint8_t cull;
	uint8_t blend;
	uint8_t depth;
	int first; /* vertex index, or data offset for GD_CUSTOM */
	int count;
	gd_custom_fn fn;
} GdCmd;

/* ---- growable frame arrays -------------------------------------------- */
static GdVert *verts;
static int n_verts, cap_verts;
static GdCmd *cmds;
static int n_cmds, cap_cmds;
static unsigned char *custom_data;
static size_t custom_used, custom_cap;

/* ---- state ---------------------------------------------------------- */
#define MV_STACK_DEPTH 32
static mat4 mv_stack[MV_STACK_DEPTH];
static int mv_sp;
static mat4 proj;
static mat4 xf; /* proj * modelview, cached */
static bool xf_dirty = true;

static uint8_t cur_col[4] = {255, 255, 255, 255};
static uint8_t cur_vp = GD_VP_VIEW3D;
static uint8_t cur_cull;
static uint8_t cur_blend;
static uint8_t cur_depth; /* 0 off, else 1 | 2 write | 4 depth only */
static float depth_a, depth_b; /* z_clip = a * w + b */
static float log_a, log_b;     /* window depth = a + b * log(w) */
static bool wireframe;
static int vp_rects[GD_VP_COUNT][4];

/* ---- GL objects ----------------------------------------------------- */
static GLuint prog, vao, vbo;
static size_t vbo_bytes;

static const char *VS_FLAT = "in vec4 aPos;\n"
							 "in vec4 aColor;\n"
							 "in float aSize;\n"
							 "in vec2 aDepth;\n"
							 "out vec4 vColor;\n"
							 "out highp float vW;\n"
							 "flat out highp vec2 vDepth;\n"
							 "void main(){\n"
							 "  gl_Position = aPos;\n"
							 "  gl_PointSize = aSize;\n"
							 "  vColor = aColor;\n"
							 "  vW = aPos.w;\n"
							 "  vDepth = aDepth;\n"
							 "}\n";

/* Depth tested, depth goes with log(w) (gd_set_depth): as precise far away
 * as near */
static const char *FS_FLAT = "in vec4 vColor;\n"
							 "in highp float vW;\n"
							 "flat in highp vec2 vDepth;\n"
							 "out vec4 fragColor;\n"
							 "void main(){\n"
							 "  fragColor = vColor;\n"
							 "  gl_FragDepth = vDepth.y != 0.0 ? clamp(vDepth.x + vDepth.y * log(max(vW, 1e-6)), 0.0, 1.0)\n"
							 "                                 : gl_FragCoord.z;\n"
							 "}\n";

/* =========================================================================
 * Shader helper
 * ========================================================================= */
#if GL_IS_GLES
static const char *VS_HEADER = "#version 300 es\nprecision highp float;\n";
static const char *FS_HEADER = "#version 300 es\nprecision highp float;\n";
#else
static const char *VS_HEADER = "#version 330 core\n";
static const char *FS_HEADER = "#version 330 core\n";
#endif

static GLuint compile_stage(GLenum type, const char *header, const char *body)
{
	const char *src[2] = {header, body};
	GLuint s = glCreateShader(type);
	glShaderSource(s, 2, src, NULL);
	glCompileShader(s);

	GLint ok = GL_FALSE;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char buf[1024];
		glGetShaderInfoLog(s, sizeof(buf), NULL, buf);
		log_printf("GLSL %s compile failed:\n%s\n", type == GL_VERTEX_SHADER ? "vertex" : "fragment", buf);
	}
	return s;
}

GLuint gd_build_program(const char *vs_body, const char *fs_body, const char *const *attribs, int n_attribs)
{
	GLuint vs = compile_stage(GL_VERTEX_SHADER, VS_HEADER, vs_body);
	GLuint fs = compile_stage(GL_FRAGMENT_SHADER, FS_HEADER, fs_body);
	GLuint p = glCreateProgram();
	glAttachShader(p, vs);
	glAttachShader(p, fs);
	/* Binding locations before linking works on every driver, unlike
	 * layout(location=N) which some Mali/Adreno drivers ignore. */
	for (int i = 0; i < n_attribs; i++)
		glBindAttribLocation(p, (GLuint)i, attribs[i]);
	glLinkProgram(p);

	GLint ok = GL_FALSE;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		char buf[1024];
		glGetProgramInfoLog(p, sizeof(buf), NULL, buf);
		log_printf("GLSL link failed:\n%s\n", buf);
	}
	glDeleteShader(vs);
	glDeleteShader(fs);
	return p;
}

/* =========================================================================
 * Init / shutdown
 * ========================================================================= */
static void *grow(void *p, int *cap, int need, size_t elem)
{
	int n = *cap ? *cap : 1024;
	while (n < need)
		n *= 2;
	void *np = realloc(p, (size_t)n * elem);
	if (!np)
		return NULL;
	*cap = n;
	return np;
}

void gd_init(void)
{
	static const char *const attribs[] = {"aPos", "aColor", "aSize", "aDepth"};
	prog = gd_build_program(VS_FLAT, FS_FLAT, attribs, 4);

	glGenVertexArrays(1, &vao);
	glGenBuffers(1, &vbo);
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(GdVert), (void *)offsetof(GdVert, pos));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GdVert), (void *)offsetof(GdVert, col));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(GdVert), (void *)offsetof(GdVert, size));
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(GdVert), (void *)offsetof(GdVert, depth));
	glBindVertexArray(0);

	verts = grow(NULL, &cap_verts, 65536, sizeof(GdVert));
	cmds = grow(NULL, &cap_cmds, 4096, sizeof(GdCmd));

#if !GL_IS_GLES
	glEnable(GL_PROGRAM_POINT_SIZE);
#endif
}

void gd_shutdown(void)
{
	glDeleteBuffers(1, &vbo);
	glDeleteVertexArrays(1, &vao);
	glDeleteProgram(prog);
	free(verts);
	free(cmds);
	free(custom_data);
	verts = NULL;
	cmds = NULL;
	custom_data = NULL;
	cap_verts = cap_cmds = 0;
	custom_cap = 0;
}

void gd_begin_frame(const int rects[GD_VP_COUNT][4])
{
	memcpy(vp_rects, rects, sizeof(vp_rects));
	n_verts = n_cmds = 0;
	custom_used = 0;
	mv_sp = 0;
	mv_stack[0] = mat4_identity();
	proj = mat4_identity();
	xf_dirty = true;
	cur_vp = GD_VP_VIEW3D;
	cur_cull = 0;
	cur_blend = 0;
	cur_depth = 0;
	wireframe = false;
	gd_color3ub(255, 255, 255);
}

/* =========================================================================
 * State
 * ========================================================================= */
void gd_set_viewport(enum GdViewport vp)
{
	cur_vp = (uint8_t)vp;
}
const int *gd_viewport_rect(enum GdViewport vp)
{
	return vp_rects[vp];
}

void gd_set_projection(const mat4 *p)
{
	proj = *p;
	xf_dirty = true;
}
const mat4 *gd_projection(void)
{
	return &proj;
}
void gd_set_cull(bool c)
{
	cur_cull = c ? 1 : 0;
}
void gd_set_blend(bool b)
{
	cur_blend = b ? 1 : 0;
}
void gd_set_wireframe(bool w)
{
	wireframe = w;
}
bool gd_wireframe(void)
{
	return wireframe;
}
void gd_set_depth(const GdDepth *d)
{
	cur_depth = d ? (uint8_t)(1 | (d->write ? 2 : 0) | (d->depth_only ? 4 : 0)) : 0;
	if (!d)
		return;
	/* ndc(w) = lo + (hi - lo) * far * (w - near) / (w * (far - near)) - bias,
	 * times w */
	float k = (d->hi - d->lo) * d->far / (d->far - d->near);
	depth_a = d->lo - d->bias + k;
	depth_b = -k * d->near;
	/* What is drawn: window depth (lo..hi -> 0..1) with log(w) instead,
	 * so the slice is precise all the way out, not only near `near` (a
	 * slice starting under the ship reaches far over the ground) */
	float l = logf(d->far / d->near);
	log_b = (d->hi - d->lo) / (2 * l);
	log_a = (d->lo + 1 - d->bias) / 2 - log_b * logf(d->near);
}

void gd_push(void)
{
	if (mv_sp >= MV_STACK_DEPTH - 1)
	{
		log_printf("gd_push: modelview stack overflow\n");
		return;
	}
	mv_stack[mv_sp + 1] = mv_stack[mv_sp];
	mv_sp++;
}
void gd_pop(void)
{
	if (mv_sp > 0)
		mv_sp--;
	xf_dirty = true;
}
void gd_identity(void)
{
	mv_stack[mv_sp] = mat4_identity();
	xf_dirty = true;
}
void gd_mult(const mat4 *m)
{
	mv_stack[mv_sp] = mat4_mul(&mv_stack[mv_sp], m);
	xf_dirty = true;
}
void gd_translate(float x, float y, float z)
{
	mat4 t = mat4_translate(x, y, z);
	gd_mult(&t);
}
void gd_rotate(float deg, float ax, float ay, float az)
{
	mat4 r = mat4_rotate(deg, ax, ay, az);
	gd_mult(&r);
}
void gd_scale(float x, float y, float z)
{
	mat4 s = mat4_scale(x, y, z);
	gd_mult(&s);
}
const mat4 *gd_modelview(void)
{
	return &mv_stack[mv_sp];
}

static const mat4 *get_xf(void)
{
	if (xf_dirty)
	{
		xf = mat4_mul(&proj, &mv_stack[mv_sp]);
		xf_dirty = false;
	}
	return &xf;
}
mat4 gd_mvp(void)
{
	return *get_xf();
}

static inline uint8_t to_u8(float f)
{
	if (f <= 0.0f)
		return 0;
	if (f >= 1.0f)
		return 255;
	return (uint8_t)(f * 255.0f + 0.5f);
}

void gd_color3ub(int r, int g, int b)
{
	cur_col[0] = (uint8_t)r;
	cur_col[1] = (uint8_t)g;
	cur_col[2] = (uint8_t)b;
	cur_col[3] = 255;
}
void gd_color3f(float r, float g, float b)
{
	gd_color3ub(to_u8(r), to_u8(g), to_u8(b));
}

/* =========================================================================
 * Emission
 * ========================================================================= */

/* Reserve `n` vertices belonging to a primitive of `kind`; merges with the
 * previous command when possible. Returns NULL if out of memory. */
static GdVert *reserve(enum GdKind kind, int n)
{
	if (n_verts + n > cap_verts)
	{
		GdVert *nv = grow(verts, &cap_verts, n_verts + n, sizeof(GdVert));
		if (!nv)
			return NULL;
		verts = nv;
	}

	GdCmd *last = n_cmds ? &cmds[n_cmds - 1] : NULL;
	if (last && last->kind == kind && last->vp == cur_vp && last->cull == cur_cull &&
		last->blend == cur_blend && last->depth == cur_depth && last->first + last->count == n_verts)
	{
		last->count += n;
	}
	else
	{
		if (n_cmds + 1 > cap_cmds)
		{
			GdCmd *nc = grow(cmds, &cap_cmds, n_cmds + 1, sizeof(GdCmd));
			if (!nc)
				return NULL;
			cmds = nc;
		}
		GdCmd *c = &cmds[n_cmds++];
		c->kind = (uint8_t)kind;
		c->vp = cur_vp;
		c->cull = cur_cull;
		c->blend = cur_blend;
		c->depth = cur_depth;
		c->first = n_verts;
		c->count = n;
		c->fn = NULL;
	}
	GdVert *v = &verts[n_verts];
	n_verts += n;
	return v;
}

static inline void put(GdVert *v, const float p[3], const uint8_t col[4], float size)
{
	mat4_xform(get_xf(), p[0], p[1], p[2], v->pos);
	v->depth[0] = v->depth[1] = 0;
	if (cur_depth)
	{
		v->pos[2] = depth_a * v->pos[3] + depth_b;
		v->depth[0] = log_a;
		v->depth[1] = log_b;
	}
	memcpy(v->col, col, 4);
	v->size = size;
}

void gd_point(const float p[3], float size)
{
	GdVert *v = reserve(GD_POINTS, 1);
	if (v)
		put(v, p, cur_col, size);
}

void gd_line(const float a[3], const float b[3])
{
	GdVert *v = reserve(GD_LINES, 2);
	if (!v)
		return;
	put(&v[0], a, cur_col, 1.0f);
	put(&v[1], b, cur_col, 1.0f);
}

void gd_line2(float x1, float y1, float x2, float y2)
{
	float a[3] = {x1, y1, 0}, b[3] = {x2, y2, 0};
	gd_line(a, b);
}

void gd_line_loop(const float (*pts)[3], int n)
{
	for (int i = 0; i < n; i++)
		gd_line(pts[i], pts[(i + 1) % n]);
}

void gd_tri(const float a[3], const float b[3], const float c[3])
{
	if (wireframe)
	{
		gd_line(a, b);
		gd_line(b, c);
		gd_line(c, a);
		return;
	}
	GdVert *v = reserve(GD_TRIS, 3);
	if (!v)
		return;
	put(&v[0], a, cur_col, 1.0f);
	put(&v[1], b, cur_col, 1.0f);
	put(&v[2], c, cur_col, 1.0f);
}

void gd_quad(const float a[3], const float b[3], const float c[3], const float d[3])
{
	if (wireframe)
	{
		gd_line(a, b);
		gd_line(b, c);
		gd_line(c, d);
		gd_line(d, a);
		return;
	}
	gd_tri(a, b, c);
	gd_tri(a, c, d);
}

void gd_rect2(float x1, float y1, float x2, float y2)
{
	float a[3] = {x1, y1, 0}, b[3] = {x2, y1, 0}, c[3] = {x2, y2, 0}, d[3] = {x1, y2, 0};
	bool w = wireframe;
	wireframe = false;
	gd_quad(a, b, c, d);
	wireframe = w;
}

void gd_tri_rgb(const float a[3], const float ca[3], const float b[3], const float cb[3], const float c[3],
				const float cc[3])
{
	uint8_t k[3][4] = {
		{to_u8(ca[0]), to_u8(ca[1]), to_u8(ca[2]), 255},
		{to_u8(cb[0]), to_u8(cb[1]), to_u8(cb[2]), 255},
		{to_u8(cc[0]), to_u8(cc[1]), to_u8(cc[2]), 255},
	};
	if (wireframe)
	{
		memcpy(cur_col, k[0], 4);
		gd_tri(a, b, c);
		return;
	}
	GdVert *v = reserve(GD_TRIS, 3);
	if (!v)
		return;
	put(&v[0], a, k[0], 1.0f);
	put(&v[1], b, k[1], 1.0f);
	put(&v[2], c, k[2], 1.0f);
}

void gd_tri_rgba(const float a[3], const uint8_t ca[4], const float b[3], const uint8_t cb[4], const float c[3],
				 const uint8_t cc[4])
{
	GdVert *v = reserve(GD_TRIS, 3);
	if (!v)
		return;
	put(&v[0], a, ca, 1.0f);
	put(&v[1], b, cb, 1.0f);
	put(&v[2], c, cc, 1.0f);
}

#define DISK_MAX_SLICES 64
void gd_disk(float radius, int slices)
{
	if (slices < 3)
		slices = 3;
	if (slices > DISK_MAX_SLICES)
		slices = DISK_MAX_SLICES;

	float ring[DISK_MAX_SLICES + 1][3];
	float da = 2.0f * GLM_PI / (float)slices;
	for (int s = 0; s < slices; s++)
	{
		ring[s][0] = radius * sinf(s * da);
		ring[s][1] = radius * cosf(s * da);
		ring[s][2] = 0.0f;
	}
	ring[slices][0] = ring[0][0];
	ring[slices][1] = ring[0][1];
	ring[slices][2] = 0.0f;

	static const float centre[3] = {0, 0, 0};
	for (int s = 0; s < slices; s++)
		gd_tri(centre, ring[s + 1], ring[s]);
}

void *gd_custom(gd_custom_fn fn, size_t size)
{
	size = (size + 15) & ~(size_t)15;
	if (custom_used + size > custom_cap)
	{
		size_t ncap = custom_cap ? custom_cap : 4096;
		while (ncap < custom_used + size)
			ncap *= 2;
		unsigned char *nd = realloc(custom_data, ncap);
		if (!nd)
			return NULL;
		custom_data = nd;
		custom_cap = ncap;
	}
	if (n_cmds + 1 > cap_cmds)
	{
		GdCmd *nc = grow(cmds, &cap_cmds, n_cmds + 1, sizeof(GdCmd));
		if (!nc)
			return NULL;
		cmds = nc;
	}
	GdCmd *c = &cmds[n_cmds++];
	c->kind = GD_CUSTOM;
	c->vp = cur_vp;
	c->cull = cur_cull;
	c->blend = 0;
	c->depth = 0;
	c->first = (int)custom_used;
	c->count = 0;
	c->fn = fn;
	custom_used += size;
	return custom_data + c->first;
}

/* =========================================================================
 * Flush
 * ========================================================================= */
static void bind_flat(void)
{
	glUseProgram(prog);
	glBindVertexArray(vao);
}

void gd_flush(void)
{
	if (n_cmds == 0)
		return;

	if (n_verts)
	{
		size_t bytes = (size_t)n_verts * sizeof(GdVert);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		if (bytes > vbo_bytes)
			vbo_bytes = bytes * 2;
		/* orphan the old storage so the driver never stalls on last frame */
		glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vbo_bytes, NULL, GL_STREAM_DRAW);
		glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes, verts);
	}

	glDisable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_CULL_FACE);
	glCullFace(GL_BACK);
	bind_flat();

	int vp = -1, cull = 0, blend = 0, depth = 0;
	for (int i = 0; i < n_cmds; i++)
	{
		const GdCmd *c = &cmds[i];
		if (c->vp != vp && c->kind != GD_CUSTOM)
		{
			vp = c->vp;
			glViewport(vp_rects[vp][0], vp_rects[vp][1], vp_rects[vp][2], vp_rects[vp][3]);
		}
		if (c->kind == GD_CUSTOM)
		{
			/* custom commands always run with their own viewport set */
			glViewport(vp_rects[c->vp][0], vp_rects[c->vp][1], vp_rects[c->vp][2], vp_rects[c->vp][3]);
		}
		else
		{
			if (c->cull != cull)
			{
				cull = c->cull;
				if (cull)
					glEnable(GL_CULL_FACE);
				else
					glDisable(GL_CULL_FACE);
			}
			if (c->depth != depth)
			{
				depth = c->depth;
				if (depth)
					glEnable(GL_DEPTH_TEST);
				else
					glDisable(GL_DEPTH_TEST);
				glDepthMask((depth & 2) ? GL_TRUE : GL_FALSE);
				bool colour = !(depth & 4);
				glColorMask(colour, colour, colour, colour);
			}
			if (c->blend != blend)
			{
				blend = c->blend;
				if (blend)
				{
					glEnable(GL_BLEND);
					glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
				}
				else
				{
					glDisable(GL_BLEND);
				}
			}
		}
		switch (c->kind)
		{
		case GD_TRIS:
			glDrawArrays(GL_TRIANGLES, c->first, c->count);
			break;
		case GD_LINES:
			glDrawArrays(GL_LINES, c->first, c->count);
			break;
		case GD_POINTS:
			glDrawArrays(GL_POINTS, c->first, c->count);
			break;
		case GD_CUSTOM:
			if (blend)
				glDisable(GL_BLEND); /* custom commands start without blending */
			if (depth)
			{
				/* or depth testing */
				glDisable(GL_DEPTH_TEST);
				glDepthMask(GL_TRUE);
				glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			}
			c->fn(custom_data + c->first);
			/* callback may have touched any state: re-apply ours */
			bind_flat();
			vp = -1;
			cull = -1;
			blend = 0;
			depth = 0;
			break;
		}
	}

	glBindVertexArray(0);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	n_verts = n_cmds = 0;
	custom_used = 0;
}
