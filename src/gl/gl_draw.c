/*
 * gl_draw.c - batched draw list, see gl_draw.h.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gl_draw.h"
#include "gl_shadow.h"
#include "main.h"

#define SHADOW_UNIT 5 /* the texture unit of the ship shadow map */

/* One vertex = clip-space position + RGBA8 colour + point size + depth
 * constants + the shadow map's coordinates (gd_set_shadow) */
typedef struct
{
	float pos[4];
	uint8_t col[4];
	float size;
	float depth[2]; /* depth tested: window depth a + b * log(w); b 0 = gl_FragCoord.z */
	float shadow[3]; /* shadow map u, v, depth (0..1); 0 unless a shadow is set */
	float shadow2[3]; /* the same in the second cascade */
	float shadow3[3]; /* and the third */
} GdVert;

enum GdKind
{
	GD_TRIS,
	GD_LINES,
	GD_POINTS,
	GD_CUSTOM,
	GD_MESH /* a kept mesh (gd_mesh_draw): data is a MeshDraw */
};

#define FACE_UNIT 6	  /* the texture unit of the face table */
#define FACE_W 1024	  /* faces per row of the face table */

struct GdMesh
{
	GLuint vao, vbo;
	int n;
};

/* A kept mesh's draw: everything put() would have used, with the pose in */
typedef struct
{
	GdMesh *mesh;
	float mvp[16];   /* projection x modelview x pose */
	float sh[9][4];  /* each cascade's shadow rows x pose, 0 when off */
	float depth[4];  /* 1 when depth tested, depth_a, depth_b */
	float logk[2];   /* log_a, log_b */
	float bend[6][4]; /* bent onto a planet (gd_mesh_draw), [4][3] = 1 when on */
	int base;
	int range0, nranges; /* in range_first/range_count */
} MeshDraw;

typedef struct
{
	uint8_t kind;
	uint8_t vp;
	uint8_t cull;
	uint8_t blend;
	uint8_t depth;
	uint32_t shadow; /* looks the ship shadows up (gd_set_shadow): (layer0 + 1) | (layer1 + 1) << 8 | (layer2 + 1) << 16, 0 = off */
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
static GLint *range_first, *range_count; /* the kept meshes' vertex ranges (MeshDraw) */
static int n_ranges, cap_ranges;

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
static uint32_t cur_shadow;
static float shadow_m[12], shadow_m2[12], shadow_m3[12]; /* view point -> shadow map u, v, depth (3 rows of 4), per cascade */
static float depth_a, depth_b; /* z_clip = a * w + b */
static float log_a, log_b;     /* window depth = a + b * log(w) */
static bool wireframe;
static int vp_rects[GD_VP_COUNT][4];

/* ---- GL objects ----------------------------------------------------- */
static GLuint prog, vao, vbo;
static GLint u_shadow_layer, u_shadow_on, u_shadow_dark, u_shadow_bias, u_shadow;
static size_t vbo_bytes;
static GLuint mesh_prog, face_tex;
static int face_rows;
static GLint um_mvp, um_depth, um_log, um_sh, um_base, um_faces, um_bend;
static GLint um_shadow_layer, um_shadow_on, um_shadow_dark, um_shadow_bias, um_shadow;

/* A kept mesh's vertices: placed by the pose in uMvp, coloured by their
 * face's entry in the face table; the rest as put() and VS_FLAT */
static const char *VS_MESH = "in vec3 aPos;\n"
							 "in float aFace;\n"
							 "uniform mat4 uMvp;\n"
							 "uniform vec3 uDepth;\n"
							 "uniform vec2 uLog;\n"
							 "uniform vec4 uSh[9];\n"
							 "uniform vec4 uBend[6];\n"
							 "uniform int uBase;\n"
							 "uniform sampler2D uFaces;\n"
							 "out vec4 vColor;\n"
							 "out vec3 vShadow;\n"
							 "out vec3 vShadow2;\n"
							 "out vec3 vShadow3;\n"
							 "out highp float vW;\n"
							 "flat out highp vec2 vDepth;\n"
							 "void main(){\n"
							 "  int f = int(aFace + 0.5) + uBase;\n"
							 "  vec4 c = texelFetch(uFaces, ivec2(f & 1023, f >> 10), 0);\n"
							 "  vec4 q = vec4(aPos, 1.0);\n"
							 /* bent onto the planet (gd_mesh_draw): lowered by how far the sphere falls away
							  * from the plane it touches at the bend's point */
							 "  if (uBend[4].w > 0.5) {\n"
							 "    vec3 dq = q.xyz - uBend[3].xyz;\n"
							 "    vec3 d = uBend[0].xyz * dq.x + uBend[1].xyz * dq.y + uBend[2].xyz * dq.z;\n"
							 "    float h = dot(d, uBend[4].xyz);\n"
							 "    float w2 = max(dot(d, d) - h * h, 0.0);\n"
							 "    float R = uBend[3].w;\n"
							 "    q.xyz -= uBend[5].xyz * (w2 / (R + sqrt(max(R * R - w2, 0.0))));\n"
							 "  }\n"
							 "  vec4 p = uMvp * q;\n"
							 "  vDepth = vec2(0.0);\n"
							 "  if (uDepth.x > 0.5) {\n"
							 "    p.z = uDepth.y * p.w + uDepth.z;\n"
							 "    vDepth = uLog;\n"
							 "  }\n"
							 "  vW = p.w;\n"
							 "  vColor = vec4(c.rgb, 1.0);\n"
							 "  vShadow = vec3(dot(uSh[0], q), dot(uSh[1], q), dot(uSh[2], q));\n"
							 "  vShadow2 = vec3(dot(uSh[3], q), dot(uSh[4], q), dot(uSh[5], q));\n"
							 "  vShadow3 = vec3(dot(uSh[6], q), dot(uSh[7], q), dot(uSh[8], q));\n"
							 /* a face turned away: all three corners at one point, nothing drawn */
							 "  gl_Position = c.a > 0.0 ? p : vec4(0.0, 0.0, 2.0, 1.0);\n"
							 "}\n";

static const char *VS_FLAT = "in vec4 aPos;\n"
							 "in vec4 aColor;\n"
							 "in float aSize;\n"
							 "in vec2 aDepth;\n"
							 "in vec3 aShadow;\n"
							 "in vec3 aShadow2;\n"
							 "in vec3 aShadow3;\n"
							 "out vec4 vColor;\n"
							 "out vec3 vShadow;\n"
							 "out vec3 vShadow2;\n"
							 "out vec3 vShadow3;\n"
							 "out highp float vW;\n"
							 "flat out highp vec2 vDepth;\n"
							 "void main(){\n"
							 "  gl_Position = aPos;\n"
							 "  gl_PointSize = aSize;\n"
							 "  vColor = aColor;\n"
							 "  vW = aPos.w;\n"
							 "  vDepth = aDepth;\n"
							 "  vShadow = aShadow;\n"
							 "  vShadow2 = aShadow2;\n"
							 "  vShadow3 = aShadow3;\n"
							 "}\n";

/* Depth tested, depth goes with log(w) (gd_set_depth): as precise far away
 * as near */
static const char *FS_FLAT = GL_SHADOW_GLSL "in vec4 vColor;\n"
							 "in vec3 vShadow;\n"
							 "in vec3 vShadow2;\n"
							 "in vec3 vShadow3;\n"
							 "in highp float vW;\n"
							 "flat in highp vec2 vDepth;\n"
							 "uniform float uShadowOn;\n"
							 "uniform float uShadowDark;\n"
							 "uniform vec3 uShadowBias;\n"
							 "uniform vec3 uShadowLayer;\n"
							 "uniform sampler2DArrayShadow uShadow;\n"
							 "out vec4 fragColor;\n"
							 "void main(){\n"
							 "  fragColor = vColor;\n"
							 /* in the ships' shadow maps (outside one: lit by it) */
							 "  if (uShadowOn > 0.5) {\n"
							 "    float lit = 1.0;\n"
							 "    if (uShadowLayer.x >= 0.0 && vShadow.x >= 0.0 && vShadow.x <= 1.0 && vShadow.y >= 0.0 && vShadow.y <= 1.0)\n"
							 "      lit = shadowLit(uShadow, vShadow.xy, uShadowLayer.x, min(vShadow.z, 1.0) - uShadowBias.x);\n"
							 "    if (uShadowLayer.y >= 0.0 && vShadow2.x >= 0.0 && vShadow2.x <= 1.0 && vShadow2.y >= 0.0 && vShadow2.y <= 1.0)\n"
							 "      lit = min(lit, shadowLit(uShadow, vShadow2.xy, uShadowLayer.y, min(vShadow2.z, 1.0) - uShadowBias.y));\n"
							 "    if (uShadowLayer.z >= 0.0 && vShadow3.x >= 0.0 && vShadow3.x <= 1.0 && vShadow3.y >= 0.0 && vShadow3.y <= 1.0)\n"
							 "      lit = min(lit, shadowLit(uShadow, vShadow3.xy, uShadowLayer.z, min(vShadow3.z, 1.0) - uShadowBias.z));\n"
							 "    fragColor.rgb *= mix(uShadowDark, 1.0, lit);\n"
							 "  }\n"
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
	static const char *const attribs[] = {"aPos", "aColor", "aSize", "aDepth", "aShadow", "aShadow2", "aShadow3"};
	prog = gd_build_program(VS_FLAT, FS_FLAT, attribs, 7);
	u_shadow_layer = glGetUniformLocation(prog, "uShadowLayer");
	u_shadow_on = glGetUniformLocation(prog, "uShadowOn");
	u_shadow_dark = glGetUniformLocation(prog, "uShadowDark");
	u_shadow_bias = glGetUniformLocation(prog, "uShadowBias");
	u_shadow = glGetUniformLocation(prog, "uShadow");
	glUseProgram(prog);
	glUniform1i(u_shadow, SHADOW_UNIT);
	glUseProgram(0);

	static const char *const mesh_attribs[] = {"aPos", "aFace"};
	mesh_prog = gd_build_program(VS_MESH, FS_FLAT, mesh_attribs, 2);
	um_mvp = glGetUniformLocation(mesh_prog, "uMvp");
	um_depth = glGetUniformLocation(mesh_prog, "uDepth");
	um_log = glGetUniformLocation(mesh_prog, "uLog");
	um_sh = glGetUniformLocation(mesh_prog, "uSh");
	um_base = glGetUniformLocation(mesh_prog, "uBase");
	um_bend = glGetUniformLocation(mesh_prog, "uBend");
	um_faces = glGetUniformLocation(mesh_prog, "uFaces");
	um_shadow_layer = glGetUniformLocation(mesh_prog, "uShadowLayer");
	um_shadow_on = glGetUniformLocation(mesh_prog, "uShadowOn");
	um_shadow_dark = glGetUniformLocation(mesh_prog, "uShadowDark");
	um_shadow_bias = glGetUniformLocation(mesh_prog, "uShadowBias");
	um_shadow = glGetUniformLocation(mesh_prog, "uShadow");
	glUseProgram(mesh_prog);
	glUniform1i(um_shadow, SHADOW_UNIT);
	glUniform1i(um_faces, FACE_UNIT);
	glUseProgram(0);

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
	glEnableVertexAttribArray(4);
	glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(GdVert), (void *)offsetof(GdVert, shadow));
	glEnableVertexAttribArray(5);
	glVertexAttribPointer(5, 3, GL_FLOAT, GL_FALSE, sizeof(GdVert), (void *)offsetof(GdVert, shadow2));
	glEnableVertexAttribArray(6);
	glVertexAttribPointer(6, 3, GL_FLOAT, GL_FALSE, sizeof(GdVert), (void *)offsetof(GdVert, shadow3));
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
	glDeleteProgram(mesh_prog);
	if (face_tex)
		glDeleteTextures(1, &face_tex);
	face_tex = 0;
	face_rows = 0;
	free(verts);
	free(cmds);
	free(custom_data);
	free(range_first);
	free(range_count);
	verts = NULL;
	cmds = NULL;
	custom_data = NULL;
	range_first = range_count = NULL;
	cap_ranges = n_ranges = 0;
	cap_verts = cap_cmds = 0;
	custom_cap = 0;
}

void gd_begin_frame(const int rects[GD_VP_COUNT][4])
{
	memcpy(vp_rects, rects, sizeof(vp_rects));
	n_verts = n_cmds = 0;
	custom_used = 0;
	n_ranges = 0;
	mv_sp = 0;
	mv_stack[0] = mat4_identity();
	proj = mat4_identity();
	xf_dirty = true;
	cur_vp = GD_VP_VIEW3D;
	cur_cull = 0;
	cur_blend = 0;
	cur_depth = 0;
	cur_shadow = 0;
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
	/* Clip z only cuts at the near plane (z = -w at w = near), never at a
	 * far one: the slice's own lo + k (w - near) / w form put the far plane
	 * where float rounding of k ~ 2 left it, well short of `far` when far /
	 * near is 1e7 (clouds a few km off cut into wedges); the depth itself
	 * is the fragment's log(w) below */
	depth_a = 1;
	depth_b = -2 * d->near;
	/* What is drawn: window depth (lo..hi -> 0..1) with log(w) instead,
	 * so the slice is precise all the way out, not only near `near` (a
	 * slice starting under the ship reaches far over the ground) */
	float l = logf(d->far / d->near);
	log_b = (d->hi - d->lo) / (2 * l);
	log_a = (d->lo + 1 - d->bias) / 2 - log_b * logf(d->near);
}

void gd_set_shadow(const float *m0, int layer0, const float *m1, int layer1, const float *m2, int layer2)
{
	cur_shadow = 0;
	if (m2 && layer2 >= 0)
	{
		cur_shadow |= (uint32_t)(1 + layer2) << 16;
		memcpy(shadow_m3, m2, sizeof shadow_m3);
	}
	if (m0 && layer0 >= 0)
	{
		cur_shadow |= (uint32_t)(1 + layer0);
		memcpy(shadow_m, m0, sizeof shadow_m);
	}
	if (m1 && layer1 >= 0)
	{
		cur_shadow |= (uint32_t)(1 + layer1) << 8;
		memcpy(shadow_m2, m1, sizeof shadow_m2);
	}
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
void gd_color4ub(int r, int g, int b, int a)
{
	gd_color3ub(r, g, b);
	cur_col[3] = (uint8_t)a;
}
static void once_fn(const void *data)
{
	static GLboolean was;
	static GLint ref;
	int phase = *(const int *)data;
	if (phase == 0)
	{
		was = glIsEnabled(GL_STENCIL_TEST);
		ref = 0;
		if (was)
			glGetIntegerv(GL_STENCIL_REF, &ref);
		glEnable(GL_STENCIL_TEST);
		glStencilMask(0xff);
		glStencilFunc(GL_EQUAL, ref, 0xff);
		glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
	}
	else if (phase == 1)
	{
		glStencilFunc(GL_EQUAL, ref + 1, 0xff);
		glStencilOp(GL_KEEP, GL_KEEP, GL_DECR);
	}
	else
	{
		glStencilFunc(GL_EQUAL, ref, 0xff);
		glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
		if (!was)
			glDisable(GL_STENCIL_TEST);
	}
}
void gd_once_phase(int phase)
{
	int *p = gd_custom(once_fn, sizeof *p);
	if (p)
		*p = phase;
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
		last->blend == cur_blend && last->depth == cur_depth && last->shadow == cur_shadow && last->first + last->count == n_verts)
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
		c->shadow = cur_shadow;
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
	for (int c = 0; c < 3; c++)
	{
		float *out = c == 2 ? v->shadow3 : c ? v->shadow2 : v->shadow;
		const float *m = c == 2 ? shadow_m3 : c ? shadow_m2 : shadow_m;
		if ((cur_shadow >> (8 * c)) & 0xff)
			for (int row = 0; row < 3; row++)
				out[row] = m[row * 4] * p[0] + m[row * 4 + 1] * p[1] + m[row * 4 + 2] * p[2] + m[row * 4 + 3];
		else
			out[0] = out[1] = out[2] = 0;
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

/* A new command with `size` bytes of data (its first) */
static GdCmd *data_cmd(enum GdKind kind, size_t size)
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
	c->kind = (uint8_t)kind;
	c->vp = cur_vp;
	c->cull = cur_cull;
	c->blend = 0;
	c->depth = 0;
	c->shadow = 0;
	c->first = (int)custom_used;
	c->count = 0;
	c->fn = NULL;
	custom_used += size;
	return c;
}

void *gd_custom(gd_custom_fn fn, size_t size)
{
	GdCmd *c = data_cmd(GD_CUSTOM, size);
	if (!c)
		return NULL;
	c->fn = fn;
	return custom_data + c->first;
}

/* =========================================================================
 * Kept meshes
 * ========================================================================= */
GdMesh *gd_mesh_new(void)
{
	return calloc(1, sizeof(GdMesh));
}

void gd_mesh_free(GdMesh *m)
{
	if (!m)
		return;
	if (m->vbo)
		glDeleteBuffers(1, &m->vbo);
	if (m->vao)
		glDeleteVertexArrays(1, &m->vao);
	free(m);
}

void gd_mesh_upload(GdMesh *m, const float *xyzf, int n)
{
	if (!m)
		return;
	if (!m->vao)
	{
		glGenVertexArrays(1, &m->vao);
		glGenBuffers(1, &m->vbo);
		glBindVertexArray(m->vao);
		glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(3 * sizeof(float)));
		glBindVertexArray(0);
	}
	glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)(n > 0 ? n : 1) * 4 * sizeof(float)), n > 0 ? xyzf : NULL,
				 GL_STATIC_DRAW);
	m->n = n;
}

void gd_face_colours(const uint8_t *rgba, int n)
{
	int rows = (n + FACE_W - 1) / FACE_W;
	if (rows < 1)
		rows = 1;
	glActiveTexture(GL_TEXTURE0 + FACE_UNIT);
	if (!face_tex)
	{
		glGenTextures(1, &face_tex);
		glBindTexture(GL_TEXTURE_2D, face_tex);
		/* texelFetch only: no mipmaps, so it must not want them */
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	else
		glBindTexture(GL_TEXTURE_2D, face_tex);
	if (rows > face_rows)
	{
		face_rows = rows + rows / 2;
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, FACE_W, face_rows, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	}
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	if (n >= FACE_W)
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, FACE_W, n / FACE_W, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	if (n % FACE_W)
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, n / FACE_W, n % FACE_W, 1, GL_RGBA, GL_UNSIGNED_BYTE,
						rgba + (size_t)(n / FACE_W) * FACE_W * 4);
	glActiveTexture(GL_TEXTURE0);
}

static bool add_range(int first, int count)
{
	if (n_ranges + 1 > cap_ranges)
	{
		int cap = cap_ranges;
		GLint *nf = grow(range_first, &cap, n_ranges + 1, sizeof(GLint));
		if (!nf)
			return false;
		range_first = nf;
		cap = cap_ranges;
		GLint *nc = grow(range_count, &cap, n_ranges + 1, sizeof(GLint));
		if (!nc)
			return false;
		range_count = nc;
		cap_ranges = cap;
	}
	range_first[n_ranges] = first;
	range_count[n_ranges] = count;
	n_ranges++;
	return true;
}

void gd_mesh_draw(GdMesh *m, const float pose[16], int face_base, int first, int count, const float *bend)
{
	if (!m || !m->vao || count <= 0 || first < 0 || first + count > m->n)
		return;
	MeshDraw md;
	memset(&md, 0, sizeof md);
	md.mesh = m;
	const float *x = get_xf()->m;
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++)
		{
			double s = 0;
			for (int k = 0; k < 4; k++)
				s += (double)x[k * 4 + r] * pose[c * 4 + k];
			md.mvp[c * 4 + r] = (float)s;
		}
	if (cur_depth)
	{
		md.depth[0] = 1;
		md.depth[1] = depth_a;
		md.depth[2] = depth_b;
		md.logk[0] = log_a;
		md.logk[1] = log_b;
	}
	for (int cc = 0; cc < 3; cc++)
	{
		if (!((cur_shadow >> (8 * cc)) & 0xff))
			continue;
		const float *sm = cc == 2 ? shadow_m3 : cc ? shadow_m2 : shadow_m;
		for (int row = 0; row < 3; row++)
			for (int c = 0; c < 4; c++)
			{
				double s = 0;
				for (int k = 0; k < 4; k++)
					s += (double)sm[row * 4 + k] * pose[c * 4 + k];
				md.sh[cc * 3 + row][c] = (float)s;
			}
	}
	if (bend)
		memcpy(md.bend, bend, sizeof md.bend);
	md.base = face_base;
	/* the same draw as the last command (the next node of the object): one more range */
	GdCmd *last = n_cmds ? &cmds[n_cmds - 1] : NULL;
	if (last && last->kind == GD_MESH && last->vp == cur_vp && last->cull == cur_cull && last->blend == cur_blend &&
		last->depth == cur_depth && last->shadow == cur_shadow &&
		memcmp(custom_data + last->first, &md, offsetof(MeshDraw, range0)) == 0)
	{
		MeshDraw *lm = (MeshDraw *)(custom_data + last->first);
		int r = lm->range0 + lm->nranges - 1;
		if (range_first[r] + range_count[r] == first)
			range_count[r] += count;
		else if (add_range(first, count))
			lm->nranges++;
		return;
	}
	GdCmd *c = data_cmd(GD_MESH, sizeof md);
	if (!c)
		return;
	c->blend = cur_blend;
	c->depth = cur_depth;
	c->shadow = cur_shadow;
	md.range0 = n_ranges;
	md.nranges = add_range(first, count) ? 1 : 0;
	memcpy(custom_data + c->first, &md, sizeof md);
}

static void draw_mesh(const MeshDraw *md, uint32_t shadow)
{
	if (!md->nranges)
		return;
	glUseProgram(mesh_prog);
	glUniformMatrix4fv(um_mvp, 1, GL_FALSE, md->mvp);
	glUniform3f(um_depth, md->depth[0], md->depth[1], md->depth[2]);
	glUniform2f(um_log, md->logk[0], md->logk[1]);
	glUniform4fv(um_sh, 9, &md->sh[0][0]);
	glUniform4fv(um_bend, 6, &md->bend[0][0]);
	glUniform1i(um_base, md->base);
	GLuint st = shadow ? gl_shadow_texture() : 0;
	glUniform3f(um_shadow_layer, (float)(shadow & 0xff) - 1.0f, (float)((shadow >> 8) & 0xff) - 1.0f,
				(float)((shadow >> 16) & 0xff) - 1.0f);
	glUniform1f(um_shadow_on, st ? 1.0f : 0.0f);
	if (st)
	{
		glActiveTexture(GL_TEXTURE0 + SHADOW_UNIT);
		glBindTexture(GL_TEXTURE_2D_ARRAY, st);
		glUniform1f(um_shadow_dark, gl_shadow_dark());
		glUniform3f(um_shadow_bias, gl_shadow_bias(0), gl_shadow_bias(1), gl_shadow_bias(2));
	}
	glActiveTexture(GL_TEXTURE0 + FACE_UNIT);
	glBindTexture(GL_TEXTURE_2D, face_tex);
	glActiveTexture(GL_TEXTURE0);
	glBindVertexArray(md->mesh->vao);
#if GL_IS_GLES
	for (int i = 0; i < md->nranges; i++)
		glDrawArrays(GL_TRIANGLES, range_first[md->range0 + i], range_count[md->range0 + i]);
#else
	glMultiDrawArrays(GL_TRIANGLES, range_first + md->range0, range_count + md->range0, md->nranges);
#endif
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

	int vp = -1, cull = 0, blend = 0, depth = 0, shadow = -1;
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
			if (c->shadow != shadow)
			{
				shadow = c->shadow;
				GLuint st = shadow ? gl_shadow_texture() : 0;
				glUniform3f(u_shadow_layer, (float)(shadow & 0xff) - 1.0f, (float)((shadow >> 8) & 0xff) - 1.0f,
							(float)((shadow >> 16) & 0xff) - 1.0f);
				glUniform1f(u_shadow_on, st ? 1.0f : 0.0f);
				if (st)
				{
					glActiveTexture(GL_TEXTURE0 + SHADOW_UNIT);
					glBindTexture(GL_TEXTURE_2D_ARRAY, st);
					glActiveTexture(GL_TEXTURE0);
					glUniform1f(u_shadow_dark, gl_shadow_dark());
					glUniform3f(u_shadow_bias, gl_shadow_bias(0), gl_shadow_bias(1), gl_shadow_bias(2));
				}
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
		case GD_MESH:
			draw_mesh((const MeshDraw *)(custom_data + c->first), c->shadow);
			bind_flat(); /* the state above sets the flat program's uniforms */
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
			shadow = -1;
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
	n_ranges = 0;
}
