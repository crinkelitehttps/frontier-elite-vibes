/*
 * gl_clipboard.cpp - see gl_clipboard.h. The poses and grabbing follow the
 * F-19 port's native/src/host/clipboard_pose.cpp.
 */
#include <algorithm>
#include <atomic>
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

#include "clipboard.h"

#ifndef GL_TEXTURE_MAX_ANISOTROPY
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#endif

namespace
{

using clipboard::Board;

/* Grab when the hand is this near the board (cm); let go this near its
 * stowed place to put it back (cm); the show / hide animation (s) */
const float GRAB_DISTANCE = 8.0f;
const float STOW_SNAP = 15.0f;
const float ANIMATION_S = 0.3f;

float dot(const float *a, const float *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void cross(const float *a, const float *b, float *o)
{
	const float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
	o[0] = x, o[1] = y, o[2] = z;
}

void normalize(float *v)
{
	const float l = std::sqrt(dot(v, v));
	if (l > 1e-6f)
		for (int i = 0; i < 3; i++)
			v[i] /= l;
}

/* r unit, u unit and at right angles to r */
void orthonormalize(Board &p)
{
	normalize(p.r);
	const float k = dot(p.u, p.r);
	for (int i = 0; i < 3; i++)
		p.u[i] -= k * p.r[i];
	normalize(p.u);
}

/* A hand in clipboard.h's frame (cm, +z ahead) */
struct Hand
{
	bool valid = false;
	float pos[3] = {}, fwd[3] = {0, 0, 1}, up[3] = {0, 1, 0};
	bool grip = false;
};

/* The hand's frame: right = up x forward, up, forward */
void hand_axes(const Hand &h, float rt[3], float up[3], float fw[3])
{
	for (int i = 0; i < 3; i++)
		fw[i] = h.fwd[i], up[i] = h.up[i];
	normalize(fw);
	cross(up, fw, rt);
	normalize(rt);
	cross(fw, rt, up);
}

enum class Mode
{
	Stowed,
	Placed,
	Held
};

/* Where the board is (drawing thread) */
struct Holder
{
	const Board stowed = clipboard::stowed_board(), reading = clipboard::reading_board();
	const float width = clipboard::kBoardWidth, height = clipboard::board_height();
	Mode mode = Mode::Stowed, target = Mode::Stowed;
	Board at = stowed, from = stowed;
	Uint64 anim_t0 = 0;
	bool animating = false;
	int holder = -1;
	float local[3][3] = {}; /* held: c, r, u in the hand's frame */
	bool grip_was[2] = {}, taken[2] = {};

	bool out() const { return target != Mode::Stowed; }

	Board pose(Uint64 now) const
	{
		if (!animating)
			return at;
		const float t = std::min(1.0f, float(now - anim_t0) / float(SDL_GetPerformanceFrequency()) / ANIMATION_S);
		const float s = t * t * (3 - 2 * t);
		Board p;
		for (int i = 0; i < 3; i++)
		{
			p.c[i] = from.c[i] + (at.c[i] - from.c[i]) * s;
			p.r[i] = from.r[i] + (at.r[i] - from.r[i]) * s;
			p.u[i] = from.u[i] + (at.u[i] - from.u[i]) * s;
		}
		orthonormalize(p);
		return p;
	}

	void move_to(const Board &p, Mode m, Uint64 now)
	{
		from = pose(now);
		at = p;
		mode = target = m;
		holder = -1;
		anim_t0 = now;
		animating = true;
	}

	void toggle(Uint64 now)
	{
		if (target == Mode::Stowed)
			move_to(reading, Mode::Placed, now);
		else
			move_to(stowed, Mode::Stowed, now);
	}

	/* From point p to the board's rectangle */
	float distance(const Board &b, const float p[3]) const
	{
		float d[3] = {p[0] - b.c[0], p[1] - b.c[1], p[2] - b.c[2]}, n[3];
		cross(b.u, b.r, n);
		const float x = dot(d, b.r), y = dot(d, b.u), z = dot(d, n);
		const float dx = x - std::clamp(x, -width / 2, width / 2), dy = y - std::clamp(y, -height / 2, height / 2);
		return std::sqrt(dx * dx + dy * dy + z * z);
	}

	/* Once per frame with both hands: board[h] 1 in reach, 2 holding;
	 * take[h]: hide that grip from the bindings; pulse[h]: buzz */
	void update(Uint64 now, const Hand hands[2], int board[2], bool take[2], bool pulse[2])
	{
		if (animating && now - anim_t0 >= Uint64(ANIMATION_S * float(SDL_GetPerformanceFrequency())))
			animating = false;
		const Board cur = pose(now);
		for (int h = 0; h < 2; h++)
		{
			board[h] = 0;
			pulse[h] = false;
			const Hand &hd = hands[h];
			const bool press = hd.grip && !grip_was[h];
			grip_was[h] = hd.grip;
			if (!hd.grip)
				taken[h] = false; /* given back once released */
			if (!hd.valid)
				continue;
			const bool in_reach = distance(cur, hd.pos) < GRAB_DISTANCE;
			board[h] = in_reach;
			if (press && in_reach)
			{
				/* grab (also from the other hand), keeping it where it
				 * is relative to the hand */
				float rt[3], up[3], fw[3];
				hand_axes(hd, rt, up, fw);
				const float d[3] = {cur.c[0] - hd.pos[0], cur.c[1] - hd.pos[1], cur.c[2] - hd.pos[2]};
				const float *v[3] = {d, cur.r, cur.u};
				for (int k = 0; k < 3; k++)
					local[k][0] = dot(v[k], rt), local[k][1] = dot(v[k], up), local[k][2] = dot(v[k], fw);
				at = cur;
				animating = false;
				mode = target = Mode::Held;
				holder = h;
				taken[h] = true;
				pulse[h] = true;
			}
		}
		if (mode == Mode::Held)
		{
			const Hand &hd = hands[holder];
			if (!hd.grip)
			{
				/* let go: near its place it goes back in, else it stays */
				const float d[3] = {at.c[0] - stowed.c[0], at.c[1] - stowed.c[1], at.c[2] - stowed.c[2]};
				if (std::sqrt(dot(d, d)) < STOW_SNAP)
				{
					pulse[holder] = true;
					move_to(stowed, Mode::Stowed, now);
				}
				else
				{
					mode = target = Mode::Placed;
					holder = -1;
				}
			}
			else if (hd.valid)
			{
				/* (tracking lost: it stays where it was) */
				float rt[3], up[3], fw[3];
				hand_axes(hd, rt, up, fw);
				float *dst[3] = {at.c, at.r, at.u};
				for (int k = 0; k < 3; k++)
					for (int i = 0; i < 3; i++)
						dst[k][i] = local[k][0] * rt[i] + local[k][1] * up[i] + local[k][2] * fw[i] +
									(k == 0 ? hd.pos[i] : 0);
				orthonormalize(at);
			}
		}
		for (int h = 0; h < 2; h++)
		{
			take[h] = taken[h];
			if (mode == Mode::Held && holder == h)
				board[h] = 2;
		}
	}
};

Holder state;
std::atomic<int> toggles{0};
std::atomic<bool> out_now{false};

/* Pending toggles, then publishes whether it is out */
Uint64 step()
{
	const Uint64 now = SDL_GetPerformanceCounter();
	for (int n = toggles.exchange(0); n > 0; n--)
		state.toggle(now);
	out_now = state.out();
	return now;
}

/* ---- the texture ---------------------------------------------------------- */
GLuint prog, vao, vbo, tex;
GLint u_tex;
int tex_page = -1;

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
	const int want = manual_page();
	if (want != tex_page)
		if (std::shared_ptr<const ManualImage> img = manual_image(want))
		{
			glBindTexture(GL_TEXTURE_2D, tex);
			glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, clipboard::kWidth, clipboard::kHeight, 0, GL_RGBA,
						 GL_UNSIGNED_BYTE, img->argb.data());
			glGenerateMipmap(GL_TEXTURE_2D);
			glBindTexture(GL_TEXTURE_2D, 0);
			tex_page = want;
		}
	return tex_page >= 0;
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

/* clipboard.h's frame (cm, +z ahead) <-> view space (m, -z ahead) */
void to_view(const float a[3], float v[3])
{
	v[0] = a[0] * 0.01f, v[1] = a[1] * 0.01f, v[2] = -a[2] * 0.01f;
}

void from_view(const float v[3], float a[3], float scale)
{
	a[0] = v[0] * scale, a[1] = v[1] * scale, a[2] = -v[2] * scale;
}

} /* namespace */

void clipboard_toggle(void)
{
	toggles++;
}

bool clipboard_out(void)
{
	return out_now;
}

void clipboard_hands(VrHand hands[2], bool grab, int board[2])
{
	const Uint64 now = step();
	Hand h[2];
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
		from_view(v, h[i].pos, 100.0f);
		cockpit_play_to_view(hands[i].dir, v);
		from_view(v, h[i].fwd, 1.0f);
		cockpit_play_to_view(hands[i].up, v);
		from_view(v, h[i].up, 1.0f);
	}
	bool take[2], pulse[2];
	state.update(now, h, board, take, pulse);
	out_now = state.out();
	for (int i = 0; i < 2; i++)
	{
		if (take[i])
			hands[i].squeeze = false;
		if (pulse[i])
			vr_buzz(i, 0.02f, 0.6f);
	}
}

void clipboard_queue_cockpit(const mat4 *view_to_clip, const float eye[3])
{
	const Uint64 now = step();
	if (!ready())
		return;
	const Board b = state.pose(now);
	float e[3];
	from_view(eye, e, 100.0f);
	const bool back = clipboard::back_facing(b, e);
	float c[4][5];
	clipboard::corners(b, state.width, c);
	Quad *q = (Quad *)gd_custom(quad_pass, sizeof(Quad));
	if (!q)
		return;
	for (int j = 0; j < 4; j++)
	{
		float v[3];
		to_view(c[j], v);
		mat4_xform(view_to_clip, v[0], v[1], v[2], q->v[j]);
		/* the back plain: the board's edge colour */
		q->v[j][4] = back ? 2.5f / clipboard::kWidth : c[j][3];
		q->v[j][5] = back ? 2.5f / clipboard::kHeight : c[j][4];
	}
}

void clipboard_queue_flat(int w, int h)
{
	step();
	if (!state.out() || w <= 0 || h <= 0 || !ready())
		return;
	float hw, hh;
	clipboard::flat_extent(w, h, &hw, &hh);
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
	tex_page = -1;
}
