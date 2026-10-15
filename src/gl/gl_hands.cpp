/*
 * gl_hands.cpp - see gl_hands.h.
 */
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

extern "C"
{
#include "gl_camera.h"
#include "gl_cockpit.h"
#include "gl_draw.h"
#include "gl_hands.h"
#include "input.h"
}

#include "vibe/hand.h"
#include "vibe/surface.h"

bool vr_hands = true;
int vr_panel_distance = 50;
int vr_screen_distance = 55;
int vr_reach = 100;
int vr_finger = 4;
int vr_near_cm = 6;

/* The grips gesture: surface distance per metre the hands move */
#define GESTURE_GAIN 2.0f
/* Where the virtual screen is with lasers (m), and its size there */
#define SCREEN_DIST  1.8f
#define SCREEN_WIDTH 1.6f
#define SCREEN_DROP  0.1f
/* The dot on the surface under a near tip: this wide, this far out (cm) */
#define DOT_SIZE 0.8f
#define DOT_LIFT 0.1f

namespace
{

/* A surface to touch: corners in the play space (top left, top right,
 * bottom left, bottom right) and the screen rows it shows, or `view`: it is
 * the view's pixel plane, a point on it meaning the view pixel the eye sees
 * through it (a ship out in space, touched where its direction crosses) */
struct Surface
{
	float c[4][3];
	int y0, y1;
	bool view;
};

/* The surfaces a hand can touch: the panel or the virtual screen, and in
 * the cockpit the plane of the view (or of the menu over it) and the
 * camera screen (gl_camera.h: a press there is its next direction, not a
 * click) */
enum
{
	SURF_MAIN,
	SURF_VIEW,
	SURF_CAMERA,
	SURF_COUNT
};

/* Everything in the play space, in cm (vibe's unit): x right, y up, -z ahead */
struct Hand
{
	bool shown;
	vibe::HandPose pose; /* as drawn: moved back onto the surface when behind it */
	vibe::FingerTouch touch[SURF_COUNT];
	bool near, down;
	bool on_camera; /* the surface it is near is the camera screen */
	float height;               /* the tip above the surface (cm, + on the eye's side) */
	vibe::Vec3 foot, n;         /* the tip's foot on the surface, its normal */
	vibe::Vec3 su, sv;          /* the surface's axes (unit) */
	int sx, sy;                 /* the screen pixel under the tip */
};

Hand hand[2];
int current = -1; /* the hand pointing the mouse */
bool was_active;

/* The grips gesture */
bool gesture, gesture_cockpit;
float gesture_m0;                  /* the hands' mean forward position as it started (m) */
int gesture_cm0, gesture_cm = -1;  /* the distance then, and now (-1: no gesture) */

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
	s->view = false;
	return true;
}

bool panel_surface(Surface *s)
{
	s->y0 = COCKPIT_PANEL_ROW0;
	s->y1 = GL_SCREEN_H;
	s->view = false;
	return cockpit_panel_play(s->c);
}

/* The cockpit's screen while one of the game's other screens is on it;
 * else a menu over the view is drawn on the virtual screen's plane, its
 * rows where they are on the screen; else the view's plane lets the game's
 * pointer click what is out in space */
bool view_surface(Surface *s)
{
	if (cockpit_screen_play(s->c))
	{
		s->y0 = 0;
		s->y1 = COCKPIT_PANEL_ROW0;
		s->view = false;
		return true;
	}
	int y0, y1;
	if (cockpit_menu(&y0, &y1))
	{
		float p[4][3];
		if (!cockpit_screen_plane_play(p))
			return false;
		for (int i = 0; i < 4; i++)
		{
			float t = float((i & 2) ? y1 : y0) / float(GL_SCREEN_H);
			for (int k = 0; k < 3; k++)
				s->c[i][k] = p[i & 1][k] + (p[2 + (i & 1)][k] - p[i & 1][k]) * t;
		}
		s->y0 = y0;
		s->y1 = y1;
		s->view = false;
		return true;
	}
	s->y0 = 0;
	s->y1 = COCKPIT_PANEL_ROW0;
	s->view = true;
	return cockpit_view_plane_play(s->c);
}

/* `s` as a vibe::Surface in cm whose front faces the eye (the play
 * space's origin). `mirrored`: its u runs the other way round then. */
vibe::Surface to_vibe(const Surface &s, bool *mirrored)
{
	vibe::Vec3 tl = vibe::Vec3::of(s.c[0]) * 100.0f, tr = vibe::Vec3::of(s.c[1]) * 100.0f,
			   bl = vibe::Vec3::of(s.c[2]) * 100.0f;
	vibe::Surface v = vibe::Surface::from_corners(tl, tr, bl);
	*mirrored = vibe::dot(v.normal(), vibe::Vec3{} - tl) < 0;
	if (*mirrored)
		v = vibe::Surface::from_corners(tr, tl, vibe::Vec3::of(s.c[3]) * 100.0f);
	return v;
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

bool hands_update(const VrHand hands[2], bool cockpit, const int board[2])
{
	bool held = update_gesture(hands, cockpit);

	Surface surfs[SURF_COUNT];
	bool have[SURF_COUNT], mirrored[SURF_COUNT] = {false, false, false};
	vibe::Surface surf[SURF_COUNT];
	have[SURF_MAIN] = cockpit ? panel_surface(&surfs[SURF_MAIN]) : screen_surface(&surfs[SURF_MAIN]);
	have[SURF_VIEW] = cockpit && view_surface(&surfs[SURF_VIEW]);
	have[SURF_CAMERA] = cockpit && cockpit_camera_play(surfs[SURF_CAMERA].c);
	surfs[SURF_CAMERA].y0 = surfs[SURF_CAMERA].y1 = 0;
	surfs[SURF_CAMERA].view = false;
	for (int k = 0; k < SURF_COUNT; k++)
		if (have[k])
			surf[k] = to_vibe(surfs[k], &mirrored[k]);
	const float reach = float(vr_reach) / 100.0f, finger = float(vr_finger);
	const float near_cm = float(vr_near_cm);
	bool active = false;
	for (int i = 0; i < 2; i++)
	{
		const VrHand &c = hands[i];
		Hand &h = hand[i];
		h.shown = c.active;
		if (!c.active)
		{
			h.near = h.down = false;
			for (auto &t : h.touch)
				t.reset();
			continue;
		}
		active = true;
		vibe::HandPose &p = h.pose;
		p = vibe::HandPose::from_aim(i == 1, vibe::Vec3::of(c.pos) * 100.0f, vibe::Vec3::of(c.dir),
									 vibe::Vec3::of(c.up), reach);
		const vibe::Vec3 tip = vibe::fingertip(p, finger);
		/* every surface keeps its own press history; the one pressed, else
		 * the nearest the tip is over, is the pointer's */
		vibe::FingerTouch::State t, st[SURF_COUNT];
		int pick = -1;
		for (int k = 0; k < SURF_COUNT; k++)
		{
			if (have[k] && !board[i])
			{
				st[k] = h.touch[k].update(tip, surf[k], near_cm);
				float height = surf[k].project(tip).height;
				if (st[k].is_near && (pick < 0 || (st[k].down && !st[pick].down) ||
									  (st[k].down == st[pick].down && height < h.height)))
				{
					pick = k;
					h.height = height;
				}
			}
			else
				h.touch[k].reset(); /* at the board, or nothing to touch */
		}
		if (pick >= 0)
			t = st[pick];
		h.down = t.down;
		h.near = t.is_near;
		h.on_camera = pick == SURF_CAMERA;
		if (t.pressed)
			vr_buzz(i, 0.03f, 0.7f);
		if (t.pressed && h.on_camera)
			camera_next();
		if (t.is_near)
		{
			const Surface &s = surfs[pick];
			if (s.view)
			{
				float q[3];
				(t.foot * 0.01f).to(q);
				if (!cockpit_play_point_to_pixel(q, &h.sx, &h.sy))
				{
					h.near = h.down = false;
					h.sx = h.sy = 0;
				}
			}
			else
			{
				float u = mirrored[pick] ? 1 - t.u : t.u;
				h.sx = int(u * GL_SCREEN_W);
				h.sy = s.y0 + int(t.v * float(s.y1 - s.y0));
			}
			h.foot = t.foot;
			h.n = surf[pick].normal();
			h.su = vibe::normalized(surf[pick].right);
			h.sv = vibe::normalized(surf[pick].down);
		}
		/* drawn: the tip resting on the surface when behind it */
		p.pos = p.pos + t.draw_shift;
		p.touch = h.down || board[i] == 2		 ? vibe::Touch::Pressing
				  : h.near || board[i] == 1 ? vibe::Touch::Near
											: vibe::Touch::Away;
	}

	/* the mouse: the pressing hand until it lets go, else the nearest */
	if (current < 0 || !hand[current].down || hand[current].on_camera)
	{
		current = -1;
		for (int i = 0; i < 2; i++)
			if (hand[i].shown && hand[i].near && !hand[i].on_camera &&
				(current < 0 || hand[i].height < hand[current].height))
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
	vibe::HandStyle style;
	style.finger = float(vr_finger);
	/* from above, a little behind (the play space's +z) */
	style.light[2] = -style.light[2];
	const vibe::Vec3 eye_cm = vibe::Vec3::of(eye) * 100.0f;
	std::vector<vibe::HandTri> tris;
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
				const vibe::Vec3 pt = h.foot + (h.su * a + h.sv * b) * DOT_SIZE + h.n * DOT_LIFT;
				(pt * 0.01f).to(q[j]);
			}
			gd_tri_rgb(q[0], rgb, q[1], rgb, q[2], rgb);
			gd_tri_rgb(q[1], rgb, q[3], rgb, q[2], rgb);
		}
		tris.clear();
		vibe::build_hand(h.pose, style, &tris);
		vibe::cull_and_sort(eye_cm, &tris);
		for (const vibe::HandTri &t : tris)
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
