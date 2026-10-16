/*
 * gl_clipboard.cpp - see gl_clipboard.h. Where the board is, grabbing it
 * and its pages are vibe-lib's (vibe/cockpit_clipboard.h, kept in
 * manual.cpp); this converts to and from the cockpit's frame, and draws it.
 */
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include <SDL.h>

extern "C"
{
#include "gl_api.h"
#include "gl_clipboard.h"
#include "gl_cockpit.h"
#include "gl_draw.h"
#include "gl_hands.h"
}
#include "manual.h"

#include "vibe/clipboard.h"

#ifndef GL_TEXTURE_MAX_ANISOTROPY
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#endif

namespace
{

using vibe::Board;

/* Nanoseconds on the performance counter */
uint64_t now_ns()
{
	const uint64_t t = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
	return t / f * 1000000000ull + t % f * 1000000000ull / f;
}

/* Pending toggles */
uint64_t step()
{
	const uint64_t now = now_ns();
	clipboard().step(now);
	return now;
}

/* ---- the texture ---------------------------------------------------------- */
GLuint prog, vao, vbo, tex;
GLint u_tex;
bool have_page = false; /* a page is in the texture */

const char *VS = "in vec4 aPos;\n"
				 "in vec2 aUV;\n"
				 "out vec2 vUV;\n"
				 "void main(){ gl_Position = aPos; vUV = aUV; }\n";

/* the image is 0xAARRGGBB words, uploaded as bytes: b, g, r, a */
const char *FS = "in vec2 vUV;\n"
				 "uniform sampler2D uTex;\n"
				 "out vec4 fragColor;\n"
				 "void main(){ fragColor = vec4(texture(uTex, vUV).bgr, 1.0); }\n";

bool has_anisotropy()
{
	GLint n = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &n);
	for (GLint i = 0; i < n; i++)
	{
		const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
		if (e && (!strcmp(e, "GL_EXT_texture_filter_anisotropic") || !strcmp(e, "GL_ARB_texture_filter_anisotropic")))
			return true;
	}
	return false;
}

/* The page being read into the texture once it is ready (until then the
 * last one stays). False: nothing to show yet. */
bool ready()
{
	if (!prog)
	{
		static const char *const attribs[] = {"aPos", "aUV"};
		prog = gd_build_program(VS, FS, attribs, 2);
		u_tex = glGetUniformLocation(prog, "uTex");
		glGenVertexArrays(1, &vao);
		glGenBuffers(1, &vbo);
		glBindVertexArray(vao);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)(4 * sizeof(float)));
		glBindVertexArray(0);
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		if (has_anisotropy())
			glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, 8.0f);
		glBindTexture(GL_TEXTURE_2D, 0);
	}
	if (std::shared_ptr<const vibe::Manual::Image> img = clipboard().fresh_image())
	{
		glBindTexture(GL_TEXTURE_2D, tex);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, vibe::kClipboardW, vibe::kClipboardH, 0, GL_RGBA, GL_UNSIGNED_BYTE,
					 img->argb.data());
		glGenerateMipmap(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, 0);
		have_page = true;
	}
	return have_page;
}

/* Clip space corners (top left, top right, bottom right, bottom left) and
 * texture coords */
struct Quad
{
	float v[4][6];
};

void quad_pass(const void *data)
{
	const Quad *q = (const Quad *)data;
	glDisable(GL_CULL_FACE);
	glDisable(GL_STENCIL_TEST);
	glUseProgram(prog);
	glUniform1i(u_tex, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex);
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof q->v, q->v, GL_STREAM_DRAW);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_2D, 0);
}

/* vibe's frame (cm, +z ahead) <-> view space (m, -z ahead) */
void to_view(const float a[3], float v[3])
{
	v[0] = a[0] * 0.01f, v[1] = a[1] * 0.01f, v[2] = -a[2] * 0.01f;
}

vibe::Vec3 from_view(const float v[3], float scale)
{
	return {v[0] * scale, v[1] * scale, -v[2] * scale};
}

} /* namespace */

void clipboard_toggle(void)
{
	clipboard().toggle();
}

bool clipboard_out(void)
{
	return clipboard().out();
}

void clipboard_hands(VrHand hands[2], bool grab, int board[2])
{
	const uint64_t now = now_ns();
	vibe::Clipboard &clip = clipboard();
	vibe::ClipboardHolder::Hand h[2];
	const float reach = float(vr_reach) / 100.0f;
	for (int i = 0; i < 2; i++)
	{
		h[i].grip = hands[i].squeeze;
		if (!grab || !hands[i].active)
			continue;
		h[i].valid = true;
		float p[3], v[3];
		for (int k = 0; k < 3; k++)
			p[k] = hands[i].pos[k] * reach;
		cockpit_play_to_view(p, v);
		h[i].pos = from_view(v, 100.0f);
		cockpit_play_to_view(hands[i].dir, v);
		h[i].fwd = from_view(v, 1.0f);
		cockpit_play_to_view(hands[i].up, v);
		h[i].up = from_view(v, 1.0f);
	}
	/* let go beside the seat below the panel: stowed */
	clip.holder.stow_below = cockpit_panel_bottom() * 100.0f;
	/* (a swipe across the held board turns the page in there) */
	vibe::ClipboardHolder::HandState st[2];
	clip.update(now, h, st);
	for (int i = 0; i < 2; i++)
	{
		/* holding it, or the fingertip on it: the hand touches nothing else */
		board[i] = st[i].holding || st[i].on_board ? 2 : st[i].in_reach ? 1 : 0;
		if (st[i].take_grip)
			hands[i].squeeze = false;
		if (st[i].pulse)
			vr_buzz(i, 0.02f, 0.6f);
	}
}

void clipboard_queue_cockpit(const mat4 *view_to_clip, const float eye[3])
{
	const uint64_t now = step();
	if (!ready())
		return;
	const Board b = clipboard().pose(now);
	const bool back = vibe::back_facing(b, from_view(eye, 100.0f));
	float c[4][5];
	vibe::corners(b, clipboard().holder.width, c);
	Quad *q = (Quad *)gd_custom(quad_pass, sizeof(Quad));
	if (!q)
		return;
	for (int j = 0; j < 4; j++)
	{
		float v[3];
		to_view(c[j], v);
		mat4_xform(view_to_clip, v[0], v[1], v[2], q->v[j]);
		/* the back plain: the board's edge colour */
		q->v[j][4] = back ? 2.5f / vibe::kClipboardW : c[j][3];
		q->v[j][5] = back ? 2.5f / vibe::kClipboardH : c[j][4];
	}
}

void clipboard_queue_flat(int w, int h)
{
	step();
	if (!clipboard().out() || w <= 0 || h <= 0 || !ready())
		return;
	float hw, hh;
	vibe::flat_extent(w, h, &hw, &hh);
	Quad *q = (Quad *)gd_custom(quad_pass, sizeof(Quad));
	if (!q)
		return;
	const float k[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
	for (int j = 0; j < 4; j++)
	{
		q->v[j][0] = k[j][0] * hw;
		q->v[j][1] = k[j][1] * hh;
		q->v[j][2] = 0;
		q->v[j][3] = 1;
		q->v[j][4] = (k[j][0] + 1) / 2;
		q->v[j][5] = (1 - k[j][1]) / 2;
	}
}

void clipboard_shutdown(void)
{
	if (!prog)
		return;
	glDeleteTextures(1, &tex);
	glDeleteBuffers(1, &vbo);
	glDeleteVertexArrays(1, &vao);
	glDeleteProgram(prog);
	prog = vao = vbo = tex = 0;
	have_page = false;
	clipboard().image_lost();
}
