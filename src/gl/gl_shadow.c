/*
 * gl_shadow.c - see gl_shadow.h.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gl_draw.h"
#include "gl_shadow.h"
#include "main.h"

bool ship_shadows = true;
bool low_sun_shadows = true;

#define MAP_SIZE   1024
#define N_LAYERS   (1 + GL_SHADOW_CASTER_LAYERS) /* per cascade */
/* the map's depth reaches this many radii of the ships along the light (low_sun_shadows off: the old 200) */
#define DEPTH_SPAN (low_sun_shadows ? 1000.0f : 200.0f)
#define DARK       0.5f   /* the colour left in shadow */

static GLuint tex, fbo, prog, vao, vbo;
static size_t vbo_bytes;
static bool inited, failed;

/* one map per cascade (casters of a size; see gl_shadow.h): its matrix, the pass's triangles in its clip space
 * (x, y, z in -1..1) until the frame is drawn, and where each caster's triangles end in them (in vertices) */
typedef struct
{
	float matrix[12];
	bool have;
	float bias;
	float *clip;
	size_t clip_cap, clip_n; /* floats */
	size_t caster_end[GL_SHADOW_CASTER_LAYERS];
	int n_casters;
} Cascade;
static Cascade cas[GL_SHADOW_CASCADES];

static const char *VS = "in vec3 aPos;\n"
						"void main(){ gl_Position = vec4(aPos, 1.0); }\n";
static const char *FS = "out vec4 fragColor;\n"
						"void main(){ fragColor = vec4(1.0); }\n";

bool gl_shadow_wanted(void)
{
	static int env = -2;
	if (env == -2)
	{
		const char *e = getenv("GLF_SHIP_SHADOWS");
		env = e ? atoi(e) : -1;
	}
	return env >= 0 ? env != 0 : ship_shadows;
}

static void init(void)
{
	inited = true;
	static const char *const attribs[] = {"aPos"};
	prog = gd_build_program(VS, FS, attribs, 1);

	glGenTextures(1, &tex);
	/* cascade c: layer c * N_LAYERS has every caster of it; layer c * N_LAYERS + k + 1 every caster but the k-th (what
	 * that ship is lit by) */
	glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
	glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT24, MAP_SIZE, MAP_SIZE, N_LAYERS * GL_SHADOW_CASCADES, 0, GL_DEPTH_COMPONENT,
				 GL_UNSIGNED_INT, NULL);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
	glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

	glGenFramebuffers(1, &fbo);
	GLint prev = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, tex, 0, 0);
	GLenum none = GL_NONE;
	glDrawBuffers(1, &none);
#if !GL_IS_GLES
	glReadBuffer(GL_NONE);
#endif
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
	{
		log_printf("Ship shadows: the shadow map's framebuffer is incomplete\n");
		failed = true;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev);

	glGenVertexArrays(1, &vao);
	glGenBuffers(1, &vbo);
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *)0);
	glBindVertexArray(0);
}

typedef struct
{
	int cascade;
} PassData;

/* The map's pass, at the start of the frame's draw list */
static void shadow_pass(const void *payload)
{
	Cascade *cs = &cas[((const PassData *)payload)->cascade];
	const int base = ((const PassData *)payload)->cascade * N_LAYERS;
	if (!inited)
		init();
	if (failed || !cs->clip_n)
		return;

	GLint prev_fbo = 0, vp[4];
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
	glGetIntegerv(GL_VIEWPORT, vp);
	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST), stencil = glIsEnabled(GL_STENCIL_TEST);
	GLboolean cull = glIsEnabled(GL_CULL_FACE), blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST);
	GLboolean cmask[4];
	glGetBooleanv(GL_COLOR_WRITEMASK, cmask);

	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glViewport(0, 0, MAP_SIZE, MAP_SIZE);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_TRUE);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

	glUseProgram(prog);
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	size_t bytes = cs->clip_n * sizeof(float);
	if (bytes > vbo_bytes)
		vbo_bytes = bytes * 2;
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vbo_bytes, NULL, GL_STREAM_DRAW);
	glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes, cs->clip);
	const size_t nverts = cs->clip_n / 3;
	for (int layer = 0; layer <= cs->n_casters; layer++)
	{
		glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, tex, 0, base + layer);
		glClear(GL_DEPTH_BUFFER_BIT);
		if (layer == 0)
			glDrawArrays(GL_TRIANGLES, 0, (GLsizei)nverts);
		else
		{
			/* all the casters but the (layer - 1)-th: the runs before and after it */
			size_t a = layer > 1 ? cs->caster_end[layer - 2] : 0, b = cs->caster_end[layer - 1];
			if (a)
				glDrawArrays(GL_TRIANGLES, 0, (GLsizei)a);
			if (b < nverts)
				glDrawArrays(GL_TRIANGLES, (GLint)b, (GLsizei)(nverts - b));
		}
	}
	glBindVertexArray(0);

	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
	glViewport(vp[0], vp[1], vp[2], vp[3]);
	glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
	if (scissor)
		glEnable(GL_SCISSOR_TEST);
	if (stencil)
		glEnable(GL_STENCIL_TEST);
	if (cull)
		glEnable(GL_CULL_FACE);
	if (blend)
		glEnable(GL_BLEND);
	if (!depth)
		glDisable(GL_DEPTH_TEST);
}

bool gl_shadow_set(int cascade, const float light[3], const float *tris, size_t ntris, const size_t *caster_tris_end,
				   int ncasters)
{
	Cascade *cs = &cas[cascade];
	float *matrix = cs->matrix;
	cs->have = false;
	if (!ntris || !gl_shadow_wanted() || failed)
		return false;

	/* an orthonormal frame with z along the light */
	float L[3] = {light[0], light[1], light[2]};
	float len = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
	if (len < 1e-6f)
		return false;
	for (int k = 0; k < 3; k++)
		L[k] /= len;
	float t[3] = {fabsf(L[1]) < 0.9f ? 0.0f : 1.0f, fabsf(L[1]) < 0.9f ? 1.0f : 0.0f, 0.0f};
	float X[3] = {t[1] * L[2] - t[2] * L[1], t[2] * L[0] - t[0] * L[2], t[0] * L[1] - t[1] * L[0]};
	float xl = sqrtf(X[0] * X[0] + X[1] * X[1] + X[2] * X[2]);
	for (int k = 0; k < 3; k++)
		X[k] /= xl;
	float Y[3] = {L[1] * X[2] - L[2] * X[1], L[2] * X[0] - L[0] * X[2], L[0] * X[1] - L[1] * X[0]};
	const float *axis[3] = {X, Y, L};

	/* the casters' box in that frame (a tight rectangle: ships spread along one axis of the light's
	 * view no longer waste the map on the other) */
	float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
	for (size_t i = 0; i < ntris * 3; i++)
		for (int row = 0; row < 3; row++)
		{
			float v = axis[row][0] * tris[i * 3] + axis[row][1] * tris[i * 3 + 1] + axis[row][2] * tris[i * 3 + 2];
			lo[row] = fminf(lo[row], v);
			hi[row] = fmaxf(hi[row], v);
		}
	/* R: half the box's diagonal, the size the depth range and bias are in */
	float R = 0.5f * sqrtf((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) +
						   (hi[2] - lo[2]) * (hi[2] - lo[2]));
	if (R < 1.0f)
		return false;
	/* a margin round the sides so the shadows' edges and the filter stay inside */
	float margin = 0.02f * R;
	float extent[2] = {hi[0] - lo[0] + 2 * margin, hi[1] - lo[1] + 2 * margin};
	extent[0] = fmaxf(extent[0], 1.0f);
	extent[1] = fmaxf(extent[1], 1.0f);

	/* u = (p.X - lo.x + margin) / extent.x, v likewise, d = (p.L - lo.z + margin) / span */
	float span = DEPTH_SPAN * R;
	const float scale[3] = {1.0f / extent[0], 1.0f / extent[1], 1.0f / span};
	for (int row = 0; row < 3; row++)
	{
		for (int k = 0; k < 3; k++)
			matrix[row * 4 + k] = axis[row][k] * scale[row];
		matrix[row * 4 + 3] = (margin - lo[row]) * scale[row];
	}
	/* a depth a hundredth of the ships' size apart is the same surface */
	/* (the scenery cascade is fitted tight, a hundredth of its box would be a gap at the foot of a hangar) */
	cs->bias = (cascade == 2 ? 0.001f : 0.01f) * R / span;
	{
		static int dbg = -1, fr;
		if (dbg < 0)
			dbg = getenv("GLF_SHIP_SHADOWS") && atoi(getenv("GLF_SHIP_SHADOWS")) == 2;
		if (dbg && fr++ % 100 == 0)
			log_printf("shadow map %d: %.1f x %.1f m, texel %.3f x %.3f m, R %.1f m, %d casters\n", cascade, extent[0] / 1000,
					   extent[1] / 1000, extent[0] / MAP_SIZE / 1000, extent[1] / MAP_SIZE / 1000, R / 1000, ncasters);
	}

	cs->n_casters = ncasters < GL_SHADOW_CASTER_LAYERS ? ncasters : GL_SHADOW_CASTER_LAYERS;
	for (int i = 0; i < cs->n_casters; i++)
		cs->caster_end[i] = caster_tris_end[i] * 3;

	size_t need = ntris * 9;
	if (need > cs->clip_cap)
	{
		float *n = realloc(cs->clip, need * sizeof(float));
		if (!n)
			return false;
		cs->clip = n;
		cs->clip_cap = need;
	}
	float *clip = cs->clip;
	for (size_t i = 0; i < ntris * 3; i++)
	{
		const float *p = tris + i * 3;
		for (int row = 0; row < 3; row++)
			clip[i * 3 + (size_t)row] =
				2.0f * (matrix[row * 4] * p[0] + matrix[row * 4 + 1] * p[1] + matrix[row * 4 + 2] * p[2] + matrix[row * 4 + 3]) -
				1.0f;
	}
	cs->clip_n = need;

	PassData *pd = gd_custom(shadow_pass, sizeof(PassData));
	if (!pd)
		return false;
	pd->cascade = cascade;
	cs->have = true;
	return true;
}

void gl_shadow_again(void)
{
	for (int c = 0; c < GL_SHADOW_CASCADES; c++)
		if (cas[c].have)
		{
			PassData *pd = gd_custom(shadow_pass, sizeof(PassData));
			if (pd)
				pd->cascade = c;
		}
}

void gl_shadow_clear(void)
{
	for (int c = 0; c < GL_SHADOW_CASCADES; c++)
	{
		cas[c].have = false;
		cas[c].clip_n = 0;
		cas[c].n_casters = 0;
	}
}

int gl_shadow_layer(int cascade, int caster)
{
	return caster >= 0 && caster < GL_SHADOW_CASTER_LAYERS ? cascade * N_LAYERS + caster + 1 : -1;
}

int gl_shadow_all_layer(int cascade)
{
	return cas[cascade].have ? cascade * N_LAYERS : -1;
}

const float *gl_shadow_matrix(int cascade)
{
	return cas[cascade].have ? cas[cascade].matrix : NULL;
}

GLuint gl_shadow_texture(void)
{
	for (int c = 0; c < GL_SHADOW_CASCADES; c++)
		if (cas[c].have)
			return inited && !failed ? tex : 0;
	return 0;
}

float gl_shadow_dark(void)
{
	return DARK;
}

float gl_shadow_bias(int cascade)
{
	return cas[cascade].bias;
}

void gl_shadow_shutdown(void)
{
	if (inited)
	{
		glDeleteTextures(1, &tex);
		glDeleteFramebuffers(1, &fbo);
		glDeleteProgram(prog);
		glDeleteVertexArrays(1, &vao);
		glDeleteBuffers(1, &vbo);
	}
	for (int c = 0; c < GL_SHADOW_CASCADES; c++)
	{
		free(cas[c].clip);
		cas[c].clip = NULL;
		cas[c].clip_cap = cas[c].clip_n = 0;
	}
	inited = false;
}
