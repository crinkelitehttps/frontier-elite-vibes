/*
 * gl_shadow.c - see gl_shadow.h.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gl_draw.h"
#include "gl_shadow.h"
#include "main.h"

bool ship_shadows;

#define MAP_SIZE   1024
#define DEPTH_SPAN 200.0f /* the map's depth reaches this many radii of the ships along the light */
#define DARK       0.5f   /* the colour left in shadow */

static GLuint tex, fbo, prog, vao, vbo;
static size_t vbo_bytes;
static bool inited, failed;

static float matrix[12];
static bool have_map;
static float bias_depth;

/* the pass's triangles in map clip space (x, y, z in -1..1), until the frame is drawn */
static float *clip;
static size_t clip_cap, clip_n; /* floats */

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
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, MAP_SIZE, MAP_SIZE, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT,
				 NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenFramebuffers(1, &fbo);
	GLint prev = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, tex, 0);
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
	int unused;
} PassData;

/* The map's pass, at the start of the frame's draw list */
static void shadow_pass(const void *payload)
{
	(void)payload;
	if (!inited)
		init();
	if (failed || !clip_n)
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
	glClear(GL_DEPTH_BUFFER_BIT);

	glUseProgram(prog);
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	size_t bytes = clip_n * sizeof(float);
	if (bytes > vbo_bytes)
		vbo_bytes = bytes * 2;
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vbo_bytes, NULL, GL_STREAM_DRAW);
	glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes, clip);
	glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(clip_n / 3));
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

bool gl_shadow_set(const float light[3], const float *tris, size_t ntris)
{
	have_map = false;
	if (!ntris || !gl_shadow_wanted() || failed)
		return false;

	/* the ships' bounds: centre of the box, radius from it */
	float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
	for (size_t i = 0; i < ntris * 3; i++)
		for (int k = 0; k < 3; k++)
		{
			float v = tris[i * 3 + k];
			lo[k] = fminf(lo[k], v);
			hi[k] = fmaxf(hi[k], v);
		}
	float c[3], r2 = 0;
	for (int k = 0; k < 3; k++)
		c[k] = 0.5f * (lo[k] + hi[k]);
	for (size_t i = 0; i < ntris * 3; i++)
	{
		float d2 = 0;
		for (int k = 0; k < 3; k++)
		{
			float d = tris[i * 3 + k] - c[k];
			d2 += d * d;
		}
		r2 = fmaxf(r2, d2);
	}
	float R = sqrtf(r2);
	if (R < 1.0f)
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

	/* u = 0.5 + (p - c).X / 2R, v likewise, d = ((p - c).L + R) / span */
	float span = DEPTH_SPAN * R;
	const float *axis[3] = {X, Y, L};
	const float scale[3] = {0.5f / R, 0.5f / R, 1.0f / span};
	const float base[3] = {0.5f, 0.5f, R / span};
	for (int row = 0; row < 3; row++)
	{
		float off = base[row];
		for (int k = 0; k < 3; k++)
		{
			matrix[row * 4 + k] = axis[row][k] * scale[row];
			off -= axis[row][k] * c[k] * scale[row];
		}
		matrix[row * 4 + 3] = off;
	}
	/* a depth a hundredth of the ships' size apart is the same surface */
	bias_depth = 0.01f * R / span;

	size_t need = ntris * 9;
	if (need > clip_cap)
	{
		float *n = realloc(clip, need * sizeof(float));
		if (!n)
			return false;
		clip = n;
		clip_cap = need;
	}
	for (size_t i = 0; i < ntris * 3; i++)
	{
		const float *p = tris + i * 3;
		for (int row = 0; row < 3; row++)
			clip[i * 3 + (size_t)row] =
				2.0f * (matrix[row * 4] * p[0] + matrix[row * 4 + 1] * p[1] + matrix[row * 4 + 2] * p[2] + matrix[row * 4 + 3]) -
				1.0f;
	}
	clip_n = need;

	if (!gd_custom(shadow_pass, sizeof(PassData)))
		return false;
	have_map = true;
	return true;
}

void gl_shadow_clear(void)
{
	have_map = false;
	clip_n = 0;
}

const float *gl_shadow_matrix(void)
{
	return have_map ? matrix : NULL;
}

GLuint gl_shadow_texture(void)
{
	return have_map && inited && !failed ? tex : 0;
}

float gl_shadow_dark(void)
{
	return DARK;
}

float gl_shadow_bias(void)
{
	return bias_depth;
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
	free(clip);
	clip = NULL;
	clip_cap = clip_n = 0;
	inited = false;
}
