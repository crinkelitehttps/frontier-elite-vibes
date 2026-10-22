/*
 * gl_screen_edit.cpp - see gl_screen_edit.h.
 */
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

extern "C"
{
#include "gl_cockpit.h"
#include "gl_draw.h"
#include "gl_screen_edit.h"
#include "gl_screen_place.h"
#include "renderer.h"
#include "vr.h"
}

#include "vibe/surface.h"

/* How near the pointer is to a handle to take it: the angle between the
 * pointer's ray and the handle (radians) */
#define PICK_ANGLE 0.03f
/* Lines this wide, and handle dots this big, as an angle from the eye */
#define LINE_ANGLE 0.0013f
#define DOT_ANGLE  0.008f
#define RING_STEPS 48

namespace
{

using vibe::Vec3;

enum Mode
{
	MODE_MOVE,
	MODE_TURN,
	MODE_SIZE,
	MODE_COUNT
};

/* What the main thread hands over (under `lock`) */
struct Shared
{
	bool on = false;
	int mx = 0, my = 0;
	bool down = false;
	bool press = false, release = false; /* latched since the last frame */
	int press_x = 0, press_y = 0;
	int mode_req = -1;
	bool cycle = false, reset = false;
};
std::mutex lock;
Shared sh;

/* The drawing thread's */
int sel = -1; /* the picked screen */
int mode = MODE_MOVE;
int hover = -1; /* the handle under the pointer: Handle::id */
bool was_on;

/* One part of the gizmo: a polyline (a dot is one point) */
struct Handle
{
	int id;
	Vec3 colour;
	bool dot;
	std::vector<Vec3> pts;
};
std::vector<Handle> handles;
ScreenPose cur; /* the picked screen as shown */
bool cur_ok;
ScreenPose outlines[PLACE_COUNT];
bool outline_ok[PLACE_COUNT];

struct Drag
{
	bool on = false;
	int id = -1;
	ScreenPose start;
	float t0 = 0;  /* move: where on the axis the pointer's ray was */
	float a0 = 0;  /* turn: the angle round the axis */
	float r0 = 1;  /* size: the distance from the centre */
	Vec3 h0;
	ScreenPose last; /* where it was put last */
} drag;

const Vec3 AXIS[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
const Vec3 AXIS_COLOUR[3] = {{1.0f, 0.25f, 0.25f}, {0.3f, 0.9f, 0.3f}, {0.35f, 0.5f, 1.0f}};
const Vec3 HOVER_COLOUR{1.0f, 0.9f, 0.2f}, DRAG_COLOUR{1.0f, 1.0f, 1.0f};
const Vec3 SIZE_COLOUR{0.9f, 0.9f, 0.9f};

enum
{
	ID_MOVE = 0, /* + axis */
	ID_TURN = 3, /* + axis */
	ID_SIZE = 6  /* + corner */
};

Vec3 centre(const ScreenPose &p)
{
	return Vec3::of(p.o);
}

/* Where a ray meets the plane through `c` with unit normal `n`; false if
 * parallel */
bool ray_plane(Vec3 o, Vec3 d, Vec3 c, Vec3 n, Vec3 *hit)
{
	const float den = vibe::dot(d, n);
	if (std::fabs(den) < 1e-4f)
		return false;
	const float t = vibe::dot(c - o, n) / den;
	if (t < 0)
		return false;
	*hit = o + d * t;
	return true;
}

/* The angle between the ray and the direction to the segment a-b at its
 * nearest */
float ray_segment_angle(Vec3 o, Vec3 d, Vec3 a, Vec3 b)
{
	const Vec3 u = b - a, w = a - o;
	const float uu = vibe::dot(u, u), ud = vibe::dot(u, d), wu = vibe::dot(w, u), wd = vibe::dot(w, d);
	/* the segment point a + s u nearest the ray o + t d (d unit) */
	const float den = uu - ud * ud;
	float s = den > 1e-9f ? (ud * wd - wu) / den : 0.0f;
	s = std::min(std::max(s, 0.0f), 1.0f);
	const Vec3 p = a + u * s - o;
	const float t = vibe::dot(p, d);
	if (t <= 0)
		return 1e9f;
	return vibe::length(p - d * t) / t;
}

/* The nearest a handle's lines come to the ray, as an angle */
float handle_angle(const Handle &h, Vec3 o, Vec3 d)
{
	float best = 1e9f;
	if (h.pts.size() == 1)
	{
		const Vec3 p = h.pts[0] - o;
		const float t = vibe::dot(p, d);
		return t <= 0 ? 1e9f : vibe::length(p - d * t) / t;
	}
	for (size_t i = 0; i + 1 < h.pts.size(); i++)
		best = std::min(best, ray_segment_angle(o, d, h.pts[i], h.pts[i + 1]));
	return best;
}

float gizmo_length(const ScreenPose &p, Vec3 eye)
{
	return std::max(p.w * 0.65f, vibe::length(centre(p) - eye) * 0.12f);
}

void build_handles(Vec3 eye)
{
	handles.clear();
	if (!cur_ok)
		return;
	const Vec3 c = centre(cur);
	const float len = gizmo_length(cur, eye);
	for (int a = 0; a < 3; a++)
	{
		if (mode == MODE_MOVE)
			handles.push_back({ID_MOVE + a, AXIS_COLOUR[a], false, {c, c + AXIS[a] * len}});
		else if (mode == MODE_TURN)
		{
			const Vec3 u = AXIS[(a + 1) % 3], v = AXIS[(a + 2) % 3];
			Handle h{ID_TURN + a, AXIS_COLOUR[a], false, {}};
			for (int i = 0; i <= RING_STEPS; i++)
			{
				const float t = 2.0f * 3.14159265f * float(i) / RING_STEPS;
				h.pts.push_back(c + (u * std::cos(t) + v * std::sin(t)) * (len * 0.8f));
			}
			handles.push_back(h);
		}
	}
	if (mode == MODE_SIZE)
	{
		const float hw = cur.w * 0.5f, hh = cur.w * cur.aspect * 0.5f;
		for (int i = 0; i < 4; i++)
		{
			const float u = (i & 1) ? hw : -hw, v = (i & 2) ? -hh : hh;
			handles.push_back({ID_SIZE + i, SIZE_COLOUR, true, {c + Vec3::of(cur.x) * u + Vec3::of(cur.y) * v}});
		}
	}
}

/* The ray through window pixel (x, y) in view space */
bool pointer_ray(int x, int y, Vec3 *o, Vec3 *d)
{
	float oo[3], dd[3];
	if (!cockpit_pixel_ray(x, y, oo, dd))
		return false;
	*o = Vec3::of(oo);
	*d = vibe::normalized(Vec3::of(dd));
	return true;
}

/* Where the ray meets screen `p`'s rectangle: its distance, else a miss */
bool ray_screen(const ScreenPose &p, Vec3 o, Vec3 d, float *t)
{
	const Vec3 n = Vec3::of(p.z);
	const float den = vibe::dot(d, n);
	if (std::fabs(den) < 1e-5f)
		return false;
	*t = vibe::dot(centre(p) - o, n) / den;
	if (*t <= 0)
		return false;
	const Vec3 l = o + d * *t - centre(p);
	return std::fabs(vibe::dot(l, Vec3::of(p.x))) <= p.w * 0.5f &&
		   std::fabs(vibe::dot(l, Vec3::of(p.y))) <= p.w * p.aspect * 0.5f;
}

/* The parameter on the axis line (c0 + s a) nearest the ray */
bool axis_param(Vec3 c0, Vec3 a, Vec3 o, Vec3 d, float *s)
{
	const float b = vibe::dot(a, d);
	const Vec3 w = c0 - o;
	const float den = 1 - b * b;
	if (den < 1e-4f)
		return false;
	*s = (b * vibe::dot(d, w) - vibe::dot(a, w)) / den;
	return true;
}

/* Turns v by `ang` about the unit axis k */
Vec3 rotate(Vec3 v, Vec3 k, float ang)
{
	const float c = std::cos(ang), s = std::sin(ang);
	return v * c + vibe::cross(k, v) * s + k * (vibe::dot(k, v) * (1 - c));
}

float ring_angle(Vec3 h, Vec3 c, int a)
{
	const Vec3 l = h - c;
	return std::atan2(vibe::dot(l, AXIS[(a + 2) % 3]), vibe::dot(l, AXIS[(a + 1) % 3]));
}

/* Starts dragging handle `id` with the pointer ray (o, d); false if the
 * ray can't grab it from here */
bool drag_begin(int id, Vec3 o, Vec3 d)
{
	drag = Drag();
	drag.id = id;
	drag.start = drag.last = cur;
	const Vec3 c = centre(cur);
	if (id < ID_TURN)
	{
		if (!axis_param(c, AXIS[id - ID_MOVE], o, d, &drag.t0))
			return false;
	}
	else if (id < ID_SIZE)
	{
		const int a = id - ID_TURN;
		if (!ray_plane(o, d, c, AXIS[a], &drag.h0))
			return false;
		drag.a0 = ring_angle(drag.h0, c, a);
	}
	else
	{
		if (!ray_plane(o, d, c, Vec3::of(cur.z), &drag.h0))
			return false;
		drag.r0 = std::max(vibe::length(drag.h0 - c), 1e-3f);
	}
	drag.on = true;
	return true;
}

void drag_move(Vec3 o, Vec3 d)
{
	ScreenPose p = drag.start;
	const Vec3 c = centre(drag.start);
	if (drag.id < ID_TURN)
	{
		float s;
		if (!axis_param(c, AXIS[drag.id - ID_MOVE], o, d, &s))
			return;
		(c + AXIS[drag.id - ID_MOVE] * (s - drag.t0)).to(p.o);
	}
	else if (drag.id < ID_SIZE)
	{
		const int a = drag.id - ID_TURN;
		Vec3 h;
		if (!ray_plane(o, d, c, AXIS[a], &h))
			return;
		const float ang = ring_angle(h, c, a) - drag.a0;
		rotate(Vec3::of(p.x), AXIS[a], ang).to(p.x);
		rotate(Vec3::of(p.y), AXIS[a], ang).to(p.y);
		rotate(Vec3::of(p.z), AXIS[a], ang).to(p.z);
	}
	else
	{
		Vec3 h;
		if (!ray_plane(o, d, c, Vec3::of(drag.start.z), &h))
			return;
		p.w = std::min(std::max(drag.start.w * vibe::length(h - c) / drag.r0, PLACE_WIDTH_MIN), PLACE_WIDTH_MAX);
	}
	drag.last = p;
	screen_place_edit(sel, &p, false);
}

void drag_end(bool keep)
{
	if (!drag.on)
		return;
	drag.on = false;
	if (keep)
		screen_place_edit(sel, &drag.last, true);
	else
		screen_place_edit(sel, nullptr, false);
}

/* A quad facing the eye along a..b, `half` wide */
void ribbon(Vec3 a, Vec3 b, float half, Vec3 eye, const float col[3])
{
	const Vec3 side = vibe::cross(b - a, eye - (a + b) * 0.5f);
	if (vibe::length(side) < 1e-9f)
		return;
	const Vec3 s = vibe::normalized(side) * half;
	float p[4][3];
	(a - s).to(p[0]);
	(a + s).to(p[1]);
	(b - s).to(p[2]);
	(b + s).to(p[3]);
	gd_tri_rgb(p[0], col, p[1], col, p[2], col);
	gd_tri_rgb(p[1], col, p[3], col, p[2], col);
}

void polyline(const std::vector<Vec3> &pts, Vec3 eye, Vec3 colour)
{
	const float col[3] = {colour.x, colour.y, colour.z};
	for (size_t i = 0; i + 1 < pts.size(); i++)
	{
		const float half = LINE_ANGLE * vibe::length((pts[i] + pts[i + 1]) * 0.5f - eye);
		ribbon(pts[i], pts[i + 1], half, eye, col);
	}
}

/* A square facing the eye */
void dot(Vec3 p, Vec3 eye, Vec3 colour)
{
	const float half = DOT_ANGLE * vibe::length(p - eye);
	const float col[3] = {colour.x, colour.y, colour.z};
	ribbon(p - Vec3{0, half, 0}, p + Vec3{0, half, 0}, half, eye, col);
}

void outline(const ScreenPose &p, Vec3 eye, Vec3 colour)
{
	const Vec3 c = centre(p), x = Vec3::of(p.x) * (p.w * 0.5f), y = Vec3::of(p.y) * (p.w * p.aspect * 0.5f);
	polyline({c - x + y, c + x + y, c + x - y, c - x - y, c - x + y}, eye, colour);
}

void leave(void)
{
	drag_end(true);
	hover = -1;
}

} /* namespace */

bool screen_edit_on(void)
{
	std::lock_guard<std::mutex> g(lock);
	return sh.on;
}

void screen_edit_toggle(void)
{
	bool on;
	{
		std::lock_guard<std::mutex> g(lock);
		on = sh.on = !sh.on;
		sh.down = sh.press = sh.release = false;
		sh.mode_req = -1;
		sh.cycle = sh.reset = false;
	}
	/* a free pointer to point with; the grab comes back as it was */
	if (on)
		SDL_SetRelativeMouseMode(SDL_FALSE);
	else if (bGrabMouse && !bInFullScreen)
		SDL_SetRelativeMouseMode(SDL_TRUE);
}

bool screen_edit_event(const SDL_Event *e)
{
	if (e->type == SDL_KEYDOWN && e->key.keysym.sym == SDLK_g && (e->key.keysym.mod & KMOD_CTRL))
	{
		if (!e->key.repeat && cockpit_shown() && !vr_running())
			screen_edit_toggle();
		return true;
	}
	std::lock_guard<std::mutex> g(lock);
	if (!sh.on)
		return false;
	switch (e->type)
	{
	case SDL_MOUSEMOTION:
		sh.mx = e->motion.x, sh.my = e->motion.y;
		return true;
	case SDL_MOUSEBUTTONDOWN:
		sh.mx = e->button.x, sh.my = e->button.y;
		if (e->button.button == SDL_BUTTON_LEFT)
		{
			sh.down = sh.press = true;
			sh.press_x = sh.mx, sh.press_y = sh.my;
		}
		return true;
	case SDL_MOUSEBUTTONUP:
		sh.mx = e->button.x, sh.my = e->button.y;
		if (e->button.button == SDL_BUTTON_LEFT)
		{
			sh.down = false;
			sh.release = true;
		}
		return true;
	case SDL_MOUSEWHEEL:
		return true;
	case SDL_KEYDOWN:
	case SDL_KEYUP:
	{
		if (e->key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI))
			return false;
		const bool down = e->type == SDL_KEYDOWN && !e->key.repeat;
		switch (e->key.keysym.sym)
		{
		case SDLK_w:
			if (down)
				sh.mode_req = MODE_MOVE;
			return true;
		case SDLK_e:
			if (down)
				sh.mode_req = MODE_TURN;
			return true;
		case SDLK_r:
			if (down)
				sh.mode_req = MODE_SIZE;
			return true;
		case SDLK_TAB:
			if (down)
				sh.cycle = true;
			return true;
		case SDLK_DELETE:
		case SDLK_BACKSPACE:
			if (down)
				sh.reset = true;
			return true;
		case SDLK_ESCAPE:
			if (down)
			{
				sh.on = false;
				sh.down = false;
				if (bGrabMouse && !bInFullScreen)
					SDL_SetRelativeMouseMode(SDL_TRUE);
			}
			return true;
		}
		return false;
	}
	}
	return false;
}

void screen_edit_update(bool cockpit, const float eye_in[3])
{
	Shared s;
	{
		std::lock_guard<std::mutex> g(lock);
		s = sh;
		sh.press = sh.release = false;
		sh.mode_req = -1;
		sh.cycle = sh.reset = false;
	}
	if (!s.on || !cockpit)
	{
		if (was_on)
			leave();
		was_on = false;
		cur_ok = false;
		handles.clear();
		for (bool &b : outline_ok)
			b = false;
		if (!s.on)
			sel = -1;
		return;
	}
	was_on = true;
	const Vec3 eye = Vec3::of(eye_in);

	if (s.mode_req >= 0 && !drag.on)
		mode = s.mode_req;
	if (s.cycle && !drag.on)
		mode = (mode + 1) % MODE_COUNT;
	if (s.reset && sel >= 0 && !drag.on)
		screen_place[sel].on = 0;

	for (int i = 0; i < PLACE_COUNT; i++)
		outline_ok[i] = screen_place_shown(i, &outlines[i]);
	if (sel >= 0 && !outline_ok[sel])
	{
		drag_end(false);
		sel = -1;
	}
	cur_ok = sel >= 0;
	if (cur_ok)
		cur = outlines[sel];

	Vec3 o, d;
	if (drag.on)
	{
		/* a press and release inside one frame still moves it once */
		if (pointer_ray(s.mx, s.my, &o, &d))
			drag_move(o, d);
		if (s.release || !s.down)
			drag_end(true);
		return;
	}
	if (!pointer_ray(s.mx, s.my, &o, &d))
		return;

	/* the handle under the pointer, else the screen: that becomes the
	 * picked one */
	build_handles(eye);
	hover = -1;
	float best = PICK_ANGLE;
	for (const Handle &h : handles)
	{
		const float a = handle_angle(h, o, d);
		if (a < best)
			hover = h.id, best = a;
	}
	if (hover < 0)
	{
		int near = -1;
		float near_t = 1e9f;
		for (int i = 0; i < PLACE_COUNT; i++)
		{
			float t;
			if (outline_ok[i] && ray_screen(outlines[i], o, d, &t) && t < near_t)
				near = i, near_t = t;
		}
		if (near >= 0 && near != sel)
		{
			sel = near;
			cur = outlines[sel];
			cur_ok = true;
			build_handles(eye);
		}
	}
	if (s.press && hover >= 0)
	{
		Vec3 po, pd;
		if (pointer_ray(s.press_x, s.press_y, &po, &pd) && drag_begin(hover, po, pd))
		{
			if (pointer_ray(s.mx, s.my, &o, &d))
				drag_move(o, d);
			if (!s.down)
				drag_end(true);
		}
	}
}

void screen_edit_queue(const mat4 *cp, const float eye_in[3])
{
	if (!was_on)
		return;
	(void)cp;
	const Vec3 eye = Vec3::of(eye_in);
	gd_set_cull(false);
	for (int i = 0; i < PLACE_COUNT; i++)
		if (outline_ok[i] && i != sel)
			outline(outlines[i], eye, Vec3{0.55f, 0.6f, 0.65f});
	if (!cur_ok)
		return;
	outline(cur, eye, Vec3{0.3f, 0.9f, 1.0f});
	build_handles(eye);
	for (const Handle &h : handles)
	{
		const Vec3 col = drag.on && drag.id == h.id ? DRAG_COLOUR : hover == h.id && !drag.on ? HOVER_COLOUR : h.colour;
		if (h.dot)
			dot(h.pts[0], eye, col);
		else
		{
			polyline(h.pts, eye, col);
			if (mode == MODE_MOVE)
				dot(h.pts.back(), eye, col);
		}
	}
}
