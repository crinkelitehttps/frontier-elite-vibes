/*
 * gl_hands.cpp - see gl_hands.h.
 */
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

extern "C"
{
#include "gl_cockpit.h"
#include "gl_draw.h"
#include "gl_hands.h"
#include "input.h"
}

#include "vr_hand.h"

bool vr_hands = true;
int vr_panel_distance = 50;
int vr_screen_distance = 55;
int vr_reach = 100;
int vr_finger = 4;
int vr_near_cm = 6;

/* Press rule (m): no press from further behind than this, and the release
 * is this far out */
#define BEHIND  0.08f
#define RELEASE 0.005f
/* The grips gesture: surface distance per metre the hands move */
#define GESTURE_GAIN 2.0f
/* Where the virtual screen is with lasers (m), and its size there */
#define SCREEN_DIST  1.8f
#define SCREEN_WIDTH 1.6f
#define SCREEN_DROP  0.1f
/* The dot on the surface under a near tip: this wide, this far out (m) */
#define DOT_SIZE 0.008f
#define DOT_LIFT 0.001f

namespace
{

/* A surface to touch: corners in the play space (top left, top right,
 * bottom left, bottom right) and the screen rows it shows */
struct Surface
{
	float c[4][3];
	int y0, y1;
};

struct Hand
{
	bool shown;
	vr_hand::Pose pose; /* as drawn: moved back onto the surface when behind it */
	bool near, down;
	bool was_down, was_in_front;
	float height;               /* the tip above the surface (m, + on the eye's side) */
	float foot[3], n[3];        /* the tip's foot on the surface, its normal */
	float su[3], sv[3];         /* the surface's axes (unit) */
	int sx, sy;                 /* the screen pixel under the tip */
};

Hand hand[2];
int current = -1; /* the hand pointing the mouse */
bool was_active;

/* The grips gesture */
bool gesture, gesture_cockpit;
float gesture_m0;                  /* the hands' mean forward position as it started (m) */
int gesture_cm0, gesture_cm = -1;  /* the distance then, and now (-1: no gesture) */

float dot3(const float a[3], const float b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void cross3(const float a[3], const float b[3], float out[3])
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

void normalize3(float v[3])
{
	float l = std::sqrt(dot3(v, v));
	if (l > 1e-12f)
		for (int k = 0; k < 3; k++)
			v[k] /= l;
}

bool screen_surface(Surface *s)
{
	float dist, w, drop;
	vr_screen_place(&dist, &w, &drop);
	float h = w * 0.75f;
	for (int i = 0; i < 4; i++)
	{
		s->c[i][0] = (i & 1) ? w * 0.5f : -w * 0.5f;
		s->c[i][1] = -drop + ((i & 2) ? -h * 0.5f : h * 0.5f);
		s->c[i][2] = -dist;
	}
	s->y0 = 0;
	s->y1 = GL_SCREEN_H;
	return true;
}

bool panel_surface(Surface *s)
{
	s->y0 = COCKPIT_PANEL_ROW0;
	s->y1 = GL_SCREEN_H;
	return cockpit_panel_play(s->c);
}

/* The fingertip `tip` over surface `s`: whether it is over it, its height
 * above it, its foot and the screen pixel there (into `h`) */
bool touch(const Surface &s, const float tip[3], Hand *h)
{
	float u[3], v[3], rel[3], eye_rel[3];
	for (int k = 0; k < 3; k++)
	{
		u[k] = s.c[1][k] - s.c[0][k];
		v[k] = s.c[2][k] - s.c[0][k];
		rel[k] = tip[k] - s.c[0][k];
		eye_rel[k] = -s.c[0][k]; /* the eye point is the play space's origin */
	}
	cross3(u, v, h->n);
	normalize3(h->n);
	if (dot3(h->n, eye_rel) < 0)
		for (float &x : h->n)
			x = -x;
	h->height = dot3(rel, h->n);
	for (int k = 0; k < 3; k++)
		h->foot[k] = tip[k] - h->n[k] * h->height;
	float fu = dot3(rel, u) / dot3(u, u), fv = dot3(rel, v) / dot3(v, v);
	std::memcpy(h->su, u, sizeof u);
	std::memcpy(h->sv, v, sizeof v);
	normalize3(h->su);
	normalize3(h->sv);
	if (fu < 0 || fu >= 1 || fv < 0 || fv >= 1)
		return false;
	h->sx = int(fu * GL_SCREEN_W);
	h->sy = s.y0 + int(fv * float(s.y1 - s.y0));
	return true;
}

/* Both grips held: the surface's distance follows the hands forward and
 * back. True from then until both are let go. */
bool update_gesture(const VrHand hands[2], bool cockpit)
{
	bool both = hands[0].active && hands[1].active && hands[0].squeeze && hands[1].squeeze;
	bool any = hands[0].squeeze || hands[1].squeeze;
	float m = -(hands[0].pos[2] + hands[1].pos[2]) * 0.5f;
	if (gesture && gesture_cockpit != cockpit)
		gesture_cm = -1; /* the surface changed under it: dropped */
	if (both && !gesture)
	{
		gesture = true;
		gesture_cockpit = cockpit;
		gesture_m0 = m;
		gesture_cm0 = cockpit ? (vr_panel_distance > 0 ? vr_panel_distance : 62) : vr_screen_distance;
		gesture_cm = gesture_cm0;
	}
	else if (gesture && both && gesture_cm >= 0)
	{
		int cm = gesture_cm0 + int(std::lround((m - gesture_m0) * 100.0f * GESTURE_GAIN));
		gesture_cm = std::min(std::max(cm, VR_DISTANCE_MIN), VR_DISTANCE_MAX);
	}
	else if (gesture && !both && gesture_cm >= 0)
	{
		/* one let go: kept (the setting, saved once, not every frame) */
		if (gesture_cockpit)
			vr_panel_distance = gesture_cm;
		else
			vr_screen_distance = gesture_cm;
		gesture_cm = -1;
	}
	if (gesture && !any)
		gesture = false;
	return gesture;
}

} /* namespace */

int hands_panel_distance(void)
{
	return gesture_cm >= 0 && gesture_cockpit ? gesture_cm : vr_panel_distance;
}

extern "C" void vr_screen_place(float *dist, float *width, float *drop)
{
	float d = SCREEN_DIST;
	if (vr_hands)
		d = float(gesture_cm >= 0 && !gesture_cockpit ? gesture_cm : vr_screen_distance) / 100.0f;
	float k = d / SCREEN_DIST;
	*dist = d;
	*width = SCREEN_WIDTH * k;
	*drop = SCREEN_DROP * k;
}

bool hands_update(const VrHand hands[2], bool cockpit)
{
	bool held = update_gesture(hands, cockpit);

	Surface s;
	bool have = cockpit ? panel_surface(&s) : screen_surface(&s);
	const float reach = float(vr_reach) / 100.0f, finger = float(vr_finger) / 100.0f;
	const float near_m = float(vr_near_cm) / 100.0f;
	bool active = false;
	for (int i = 0; i < 2; i++)
	{
		const VrHand &c = hands[i];
		Hand &h = hand[i];
		h.shown = c.active;
		if (!c.active)
		{
			h.near = h.down = h.was_down = h.was_in_front = false;
			continue;
		}
		active = true;
		float pos[3], tip[3];
		for (int k = 0; k < 3; k++)
		{
			pos[k] = c.pos[k] * reach;
			tip[k] = pos[k] + c.dir[k] * finger;
		}
		bool over = have && touch(s, tip, &h);
		/* the press rule: no presses from behind, a release 5 mm out */
		h.down = over && h.height > -BEHIND &&
				 (h.was_down ? h.height < RELEASE : h.height <= 0 && h.was_in_front);
		if (h.down && !h.was_down)
			vr_buzz(i, 0.03f, 0.7f);
		h.was_in_front = over && h.height > 0;
		h.was_down = h.down;
		h.near = over && h.height < near_m && h.height > -BEHIND;

		/* drawn: in cm (vr_hand.h), the tip resting on the surface when
		 * behind it */
		vr_hand::Pose &p = h.pose;
		p.right = i == 1;
		for (int k = 0; k < 3; k++)
		{
			float shift = over && h.height < 0 ? -h.n[k] * h.height : 0.0f;
			p.pos[k] = (pos[k] + shift) * 100.0f;
			p.fwd[k] = c.dir[k];
			p.up[k] = c.up[k];
		}
		p.touch = h.down ? vr_hand::Touch::Pressing : h.near ? vr_hand::Touch::Near : vr_hand::Touch::Away;
	}

	/* the mouse: the pressing hand until it lets go, else the nearest */
	if (current < 0 || !hand[current].down)
	{
		current = -1;
		for (int i = 0; i < 2; i++)
			if (hand[i].shown && hand[i].near && (current < 0 || hand[i].height < hand[current].height))
				current = i;
	}
	if (active || was_active)
	{
		const Hand *h = current >= 0 ? &hand[current] : nullptr;
		Input_SetLaser(active, h != nullptr, h ? float(h->sx) + 0.5f : 0, h ? float(h->sy) + 0.5f : 0,
					   h && h->down ? 0x2 : 0);
	}
	was_active = active;
	return held;
}

void hands_off(void)
{
	for (Hand &h : hand)
		h = Hand{};
	current = -1;
	if (was_active)
		Input_SetLaser(false, false, 0, 0, 0);
	was_active = false;
	if (gesture && gesture_cm >= 0)
	{
		if (gesture_cockpit)
			vr_panel_distance = gesture_cm;
		else
			vr_screen_distance = gesture_cm;
	}
	gesture = false;
	gesture_cm = -1;
}

bool hands_shown(void)
{
	return hand[0].shown || hand[1].shown;
}

void hands_queue(const mat4 *play_to_clip, const float eye[3])
{
	if (!hands_shown())
		return;
	gd_set_projection(play_to_clip);
	gd_push();
	gd_identity();
	gd_set_cull(false);
	vr_hand::Style style;
	style.finger = float(vr_finger);
	/* from above, a little behind (the play space's +z) */
	style.light[2] = -style.light[2];
	const float eye_cm[3] = {eye[0] * 100.0f, eye[1] * 100.0f, eye[2] * 100.0f};
	std::vector<vr_hand::Tri> tris;
	for (const Hand &h : hand)
	{
		if (!h.shown)
			continue;
		/* the dot where the tip will touch, under the hand */
		if (h.near)
		{
			const float *rgb = h.down ? style.press_tip : style.near_tip;
			float q[4][3];
			for (int j = 0; j < 4; j++)
			{
				float a = (j & 1) ? 0.5f : -0.5f, b = (j & 2) ? 0.5f : -0.5f;
				for (int k = 0; k < 3; k++)
					q[j][k] = h.foot[k] + (h.su[k] * a + h.sv[k] * b) * DOT_SIZE + h.n[k] * DOT_LIFT;
			}
			gd_tri_rgb(q[0], rgb, q[1], rgb, q[2], rgb);
			gd_tri_rgb(q[1], rgb, q[3], rgb, q[2], rgb);
		}
		tris.clear();
		vr_hand::build(h.pose, style, &tris);
		vr_hand::cull_and_sort(eye_cm, &tris);
		for (const vr_hand::Tri &t : tris)
		{
			float p[3][3];
			for (int j = 0; j < 3; j++)
				for (int k = 0; k < 3; k++)
					p[j][k] = t.p[j][k] * 0.01f;
			gd_tri_rgb(p[0], t.rgb, p[1], t.rgb, p[2], t.rgb);
		}
	}
	gd_pop();
}
