/*
 * gl_screen_place.cpp - see gl_screen_place.h.
 */
#include <algorithm>
#include <atomic>
#include <cmath>

extern "C"
{
#include "game_state.h"
#include "gl_cockpit.h"
#include "gl_hands.h"
#include "gl_screen_place.h"
#include "input.h"
#include "main.h"
}

#include "vibe/surface.h"

ScreenPlace screen_place[PLACE_COUNT];

/* A grip grabs a screen with the palm or fingertip this near it (m) */
#define PLACE_REACH 0.10f
/* With the left trigger held, a hand this near a map's screen turns it
 * (its palm or fingertip, m), and keeps turning it out to the farther */
#define TURN_TOUCH 0.06f
#define TURN_KEEP  0.15f
/* Mouse motion per metre the hand moves across the map's screen */
#define ROTATE_GAIN 1000.0f
/* Screen widths (m) the two hands can size it to */
#define WIDTH_MIN 0.08f
#define WIDTH_MAX 2.0f
/* The trigger goes down above this, up below the lower (as gl_laser.c) */
#define TRIGGER_DOWN 0.6f
#define TRIGGER_UP   0.35f

namespace
{

using vibe::Vec3;

/* A frame: origin and unit axes (x right, y up, z towards the viewer) */
struct Frame
{
	Vec3 o, x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};

	Vec3 to_local(Vec3 p) const
	{
		const Vec3 d = p - o;
		return {vibe::dot(d, x), vibe::dot(d, y), vibe::dot(d, z)};
	}
	Vec3 dir_to_local(Vec3 v) const
	{
		return {vibe::dot(v, x), vibe::dot(v, y), vibe::dot(v, z)};
	}
	Vec3 dir_from_local(Vec3 v) const
	{
		return x * v.x + y * v.y + z * v.z;
	}
	Vec3 from_local(Vec3 p) const
	{
		return o + dir_from_local(p);
	}
};

/* A screen's pose: its centre and axes as a Frame, and its width; its
 * height is aspect * width */
struct Pose
{
	Frame f;
	float w = 0.3f;
};

/* An orthonormal frame from x and an approximate y */
Frame make_frame(Vec3 o, Vec3 x, Vec3 y_near)
{
	Frame f;
	f.o = o;
	f.x = vibe::normalized(x);
	Vec3 y = y_near - f.x * vibe::dot(y_near, f.x);
	if (vibe::length(y) < 1e-4f)
	{
		/* y along x: any up will do */
		y = Vec3{0, 1, 0} - f.x * f.x.y;
		if (vibe::length(y) < 1e-4f)
			y = Vec3{0, 0, 1} - f.x * f.x.z;
	}
	f.y = vibe::normalized(y);
	f.z = vibe::cross(f.x, f.y);
	return f;
}

/* view-space corners (top left, top right, bottom left, bottom right) */
Pose pose_of(const float c[4][3], float *aspect)
{
	const Vec3 tl = Vec3::of(c[0]), tr = Vec3::of(c[1]), bl = Vec3::of(c[2]), br = Vec3::of(c[3]);
	Pose p;
	p.w = vibe::length(tr - tl);
	if (aspect)
		*aspect = p.w > 1e-6f ? vibe::length(tl - bl) / p.w : 0.5f;
	p.f = make_frame((tl + tr + bl + br) * 0.25f, tr - tl, tl - bl);
	return p;
}

void corners_of(const Pose &p, float aspect, float c[4][3])
{
	const float hw = p.w * 0.5f, hh = p.w * aspect * 0.5f;
	for (int i = 0; i < 4; i++)
	{
		const float u = (i & 1) ? hw : -hw, v = (i & 2) ? -hh : hh;
		(p.f.o + p.f.x * u + p.f.y * v).to(c[i]);
	}
}

/* The settings' integers and back */
Pose pose_from_place(const ScreenPlace &s)
{
	float q[4] = {s.qx * 1e-4f, s.qy * 1e-4f, s.qz * 1e-4f, s.qw * 1e-4f};
	const float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	if (l < 1e-6f)
		q[0] = q[1] = q[2] = 0, q[3] = 1;
	else
		for (float &k : q)
			k /= l;
	const float x = q[0], y = q[1], z = q[2], w = q[3];
	Pose p;
	p.f.o = {s.x * 1e-3f, s.y * 1e-3f, s.z * 1e-3f};
	p.f.x = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)};
	p.f.y = {2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)};
	p.f.z = {2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)};
	p.w = std::min(std::max(s.w * 1e-3f, WIDTH_MIN), WIDTH_MAX);
	return p;
}

ScreenPlace place_from_pose(const Pose &p)
{
	/* the rotation's columns are the axes */
	const float m00 = p.f.x.x, m10 = p.f.x.y, m20 = p.f.x.z;
	const float m01 = p.f.y.x, m11 = p.f.y.y, m21 = p.f.y.z;
	const float m02 = p.f.z.x, m12 = p.f.z.y, m22 = p.f.z.z;
	float x, y, z, w;
	const float tr = m00 + m11 + m22;
	if (tr > 0)
	{
		const float s = std::sqrt(tr + 1) * 2;
		w = s / 4, x = (m21 - m12) / s, y = (m02 - m20) / s, z = (m10 - m01) / s;
	}
	else if (m00 > m11 && m00 > m22)
	{
		const float s = std::sqrt(1 + m00 - m11 - m22) * 2;
		w = (m21 - m12) / s, x = s / 4, y = (m01 + m10) / s, z = (m02 + m20) / s;
	}
	else if (m11 > m22)
	{
		const float s = std::sqrt(1 + m11 - m00 - m22) * 2;
		w = (m02 - m20) / s, x = (m01 + m10) / s, y = s / 4, z = (m12 + m21) / s;
	}
	else
	{
		const float s = std::sqrt(1 + m22 - m00 - m11) * 2;
		w = (m10 - m01) / s, x = (m02 + m20) / s, y = (m12 + m21) / s, z = s / 4;
	}
	ScreenPlace s;
	s.on = 1;
	s.x = int(std::lround(p.f.o.x * 1e3f));
	s.y = int(std::lround(p.f.o.y * 1e3f));
	s.z = int(std::lround(p.f.o.z * 1e3f));
	s.qx = int(std::lround(x * 1e4f));
	s.qy = int(std::lround(y * 1e4f));
	s.qz = int(std::lround(z * 1e4f));
	s.qw = int(std::lround(w * 1e4f));
	s.w = int(std::lround(p.w * 1e3f));
	return s;
}

/* A screen being carried */
struct Carry
{
	bool by[2];    /* the hands holding it */
	Pose cur;      /* where it is now */
	Pose local;    /* one hand: relative to the hand's frame */
	Pose start;    /* two hands: where it was as they took it, */
	Vec3 mid0, v0; /* the point between them, their direction (unit) */
	float d0;      /* and how far apart they were */
};

Carry carry[PLACE_COUNT];
float aspect[PLACE_COUNT] = {0.63f, 0.63f, 0.63f, 0.63f};
int grip_owner[2] = {-1, -1}; /* the screen hand i's grip holds */
bool grip_was[2];
std::atomic<bool> reset_wanted{false};

/* The map turning */
bool trigger_was;
bool trigger_taken; /* pulled with a map on the screen: the map's */
int turner = -1;    /* the hand turning it */
Vec3 turn_last;

/* Hand i in view space: palm, fingertip and its frame */
struct HandAt
{
	bool on;
	Vec3 palm, tip;
	Frame f;
};

Vec3 to_view(Vec3 p)
{
	float a[3], b[3];
	p.to(a);
	cockpit_play_to_view(a, b);
	return Vec3::of(b);
}

HandAt hand_at(const VrHand &h)
{
	HandAt a;
	a.on = h.active;
	if (!a.on)
		return a;
	const float reach = float(vr_reach) / 100.0f;
	const Vec3 fwd = vibe::normalized(to_view(Vec3::of(h.dir)));
	const Vec3 up = to_view(Vec3::of(h.up));
	a.palm = to_view(Vec3::of(h.pos) * reach);
	a.tip = a.palm + fwd * (float(vr_finger) / 100.0f);
	/* z back along the arm, y the back of the hand */
	a.f = make_frame(a.palm, vibe::cross(up, fwd * -1.0f), up);
	return a;
}

/* Turns `a` the way that takes unit `from` to unit `to` (the shortest
 * way round) */
Vec3 turn(Vec3 a, Vec3 from, Vec3 to)
{
	const Vec3 k = vibe::cross(from, to);
	const float c = vibe::dot(from, to);
	if (c < -0.999f)
		return a; /* half a turn: no telling which way, leave it */
	return a * c + vibe::cross(k, a) + k * (vibe::dot(k, a) / (1 + c));
}

/* The screen's corners in view space as shown now */
bool shown_corners(int s, float c[4][3])
{
	float p[4][3];
	if (!cockpit_place_play(s, p))
		return false;
	for (int i = 0; i < 4; i++)
		cockpit_play_to_view(p[i], c[i]);
	return true;
}

float rect_distance(const Pose &p, float asp, Vec3 q)
{
	const Vec3 l = p.f.to_local(q);
	const float hw = p.w * 0.5f, hh = p.w * asp * 0.5f;
	const Vec3 on{std::min(std::max(l.x, -hw), hw), std::min(std::max(l.y, -hh), hh), 0};
	return vibe::length(l - on);
}

float hand_distance(const Pose &p, float asp, const HandAt &h)
{
	return std::min(rect_distance(p, asp, h.palm), rect_distance(p, asp, h.tip));
}

int holders(const Carry &c)
{
	return int(c.by[0]) + int(c.by[1]);
}

/* The holding hands changed: what they hold from now, as it is */
void anchor(Carry &c, const HandAt h[2])
{
	if (c.by[0] && c.by[1])
	{
		const Vec3 v = h[1].palm - h[0].palm;
		c.start = c.cur;
		c.mid0 = (h[0].palm + h[1].palm) * 0.5f;
		c.d0 = std::max(vibe::length(v), 0.01f);
		c.v0 = v * (1 / c.d0);
	}
	else
	{
		const Frame &f = h[c.by[0] ? 0 : 1].f;
		c.local.f.o = f.to_local(c.cur.f.o);
		c.local.f.x = f.dir_to_local(c.cur.f.x);
		c.local.f.y = f.dir_to_local(c.cur.f.y);
		c.local.f.z = f.dir_to_local(c.cur.f.z);
	}
}

/* Follows the holding hands: one carries it as held; two move it with the
 * point between them, turn it as the line between them turns and size it
 * by their distance */
void follow(Carry &c, const HandAt h[2])
{
	if (c.by[0] && c.by[1])
	{
		const Vec3 v = h[1].palm - h[0].palm;
		const float d = vibe::length(v);
		const Vec3 dir = d > 1e-4f ? v * (1 / d) : c.v0;
		float s = d / c.d0;
		s = std::min(std::max(s, WIDTH_MIN / c.start.w), WIDTH_MAX / c.start.w);
		const Vec3 mid = (h[0].palm + h[1].palm) * 0.5f;
		c.cur.w = c.start.w * s;
		const Vec3 o = mid + turn(c.start.f.o - c.mid0, c.v0, dir) * s;
		c.cur.f = make_frame(o, turn(c.start.f.x, c.v0, dir), turn(c.start.f.y, c.v0, dir));
		return;
	}
	const Frame &f = h[c.by[0] ? 0 : 1].f;
	c.cur.f =
		make_frame(f.from_local(c.local.f.o), f.dir_from_local(c.local.f.x), f.dir_from_local(c.local.f.y));
}

void let_go(int s)
{
	Carry &c = carry[s];
	if (holders(c) == 0)
		return;
	c.by[0] = c.by[1] = false;
	screen_place[s] = place_from_pose(c.cur);
}

void drop_all(void)
{
	for (int s = 0; s < PLACE_COUNT; s++)
		let_go(s);
	grip_owner[0] = grip_owner[1] = -1;
}

void stop_turning(void)
{
	if (turner >= 0)
		Input_SetDrag(false, 0, 0);
	turner = -1;
}

/* The screens are moved only with the game paused (the options screen or
 * the emulator's menu): flying, the hands rest on the panel and a squeeze
 * is a flight control */
bool game_paused(void)
{
	return !bEmulationActive || game_screen() == SCREEN_OPTIONS;
}

bool map_on_screen(void)
{
	float c[4][3];
	if (!cockpit_screen_play(c))
		return false;
	const GameScreen g = game_screen();
	return g == SCREEN_GALAXY_MAP || g == SCREEN_SYSTEM_MAP;
}

} /* namespace */

void screen_place_reset(void)
{
	reset_wanted = true;
	for (ScreenPlace &s : screen_place)
		s.on = 0;
}

void screen_place_corners(int screen, float c[4][3])
{
	if (reset_wanted.exchange(false))
	{
		for (Carry &k : carry)
			k.by[0] = k.by[1] = false;
		grip_owner[0] = grip_owner[1] = -1;
		for (ScreenPlace &s : screen_place)
			s.on = 0;
	}
	pose_of(c, &aspect[screen]);
	if (holders(carry[screen]))
		corners_of(carry[screen].cur, aspect[screen], c);
	else if (screen_place[screen].on)
		corners_of(pose_from_place(screen_place[screen]), aspect[screen], c);
}

void screen_place_hands(VrHand hands[2], bool cockpit, int held[2])
{
	held[0] = held[1] = 0;
	if (!cockpit)
	{
		drop_all();
		stop_turning();
		grip_was[0] = grip_was[1] = false;
		trigger_was = trigger_taken = false;
		return;
	}
	HandAt h[2] = {hand_at(hands[0]), hand_at(hands[1])};

	/* the screens as shown */
	Pose shown[PLACE_COUNT];
	bool have[PLACE_COUNT];
	for (int s = 0; s < PLACE_COUNT; s++)
	{
		float c[4][3];
		have[s] = shown_corners(s, c);
		if (have[s])
			shown[s] = pose_of(c, nullptr);
	}

	/* grips: a press near a screen takes it; letting go leaves it there */
	for (int i = 0; i < 2; i++)
	{
		const bool grip = hands[i].connected && hands[i].squeeze;
		const bool pressed = grip && !grip_was[i];
		grip_was[i] = grip;
		int &own = grip_owner[i];
		if (own >= 0 && (!grip || !h[i].on))
		{
			Carry &c = carry[own];
			c.by[i] = false;
			if (holders(c))
				anchor(c, h);
			else
				screen_place[own] = place_from_pose(c.cur);
			own = -1;
		}
		if (!pressed || !h[i].on || own >= 0 || !game_paused())
			continue;
		/* the nearest screen within reach; one the other hand holds counts
		 * where it is being carried */
		int best = -1;
		float best_d = PLACE_REACH;
		for (int s = 0; s < PLACE_COUNT; s++)
		{
			if (!have[s])
				continue;
			const Pose &p = holders(carry[s]) ? carry[s].cur : shown[s];
			const float d = hand_distance(p, aspect[s], h[i]);
			if (d < best_d)
				best = s, best_d = d;
		}
		if (best < 0)
			continue;
		Carry &c = carry[best];
		if (!holders(c))
			c.cur = shown[best];
		c.by[i] = true;
		own = best;
		anchor(c, h);
		vr_buzz(i, 0.02f, 0.6f);
	}
	for (int s = 0; s < PLACE_COUNT; s++)
		if (holders(carry[s]))
			follow(carry[s], h);
	for (int i = 0; i < 2; i++)
		if (grip_owner[i] >= 0)
		{
			hands[i].squeeze = false; /* the screen's, not the pad's */
			held[i] = 2;
		}

	/* the left trigger held with a map on the screen: the hand touching
	 * it drags across it with the right button */
	const VrHand &l = hands[0];
	const bool trig = l.connected && l.trigger > (trigger_was ? TRIGGER_UP : TRIGGER_DOWN);
	const bool map = have[PLACE_SCREEN] && map_on_screen();
	if (trig && !trigger_was)
		trigger_taken = map;
	trigger_was = trig;
	if (!trig || !map)
		trigger_taken = false;
	if (!trigger_taken)
	{
		stop_turning();
		return;
	}
	hands[0].trigger = 0; /* the map's, not the pad's */
	const Pose &screen = shown[PLACE_SCREEN];
	const float asp = aspect[PLACE_SCREEN];
	if (turner >= 0 && (!h[turner].on || grip_owner[turner] >= 0 || hand_distance(screen, asp, h[turner]) > TURN_KEEP))
		stop_turning();
	if (turner < 0)
	{
		float best_d = TURN_TOUCH;
		for (int i = 0; i < 2; i++)
		{
			if (!h[i].on || grip_owner[i] >= 0)
				continue;
			const float d = hand_distance(screen, asp, h[i]);
			if (d < best_d)
				turner = i, best_d = d;
		}
		if (turner < 0)
			return;
		turn_last = h[turner].tip;
		Input_SetDrag(true, 0, 0);
	}
	const Vec3 d = h[turner].tip - turn_last;
	turn_last = h[turner].tip;
	Input_SetDrag(true, vibe::dot(d, screen.f.x) * ROTATE_GAIN, -vibe::dot(d, screen.f.y) * ROTATE_GAIN);
	held[turner] = std::max(held[turner], 1); /* it turns, it doesn't press */
}
