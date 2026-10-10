/*
 * gl_laser.c - see gl_laser.h.
 */
#include <math.h>
#include <string.h>

#include "gl_api.h"
#include "gl_cockpit.h"
#include "gl_draw.h"
#include "gl_laser.h"

#include "input.h"

/* Beams this long (m) where they meet nothing near (the HUD is at infinity) */
#define BEAM_LENGTH 4.0f
#define BEAM_WIDTH  0.003f
#define DOT_SIZE    0.008f
/* The trigger goes down above this, up below the lower */
#define TRIGGER_DOWN 0.6f
#define TRIGGER_UP   0.35f

typedef struct
{
	bool on;
	float pos[3], end[3];
	bool hit_near; /* ends on a surface: a dot there */
} Beam;

static Beam beams[2];
static int current = 1;      /* the hand pointing the mouse */
static bool trigger_on[2], squeeze_on[2];
static int buttons;          /* bit 0 right, bit 1 left (input.h) */
static int held;             /* the pointing hand's buttons, on something or not */
static bool was_active;

static float dot3(const float a[3], const float b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross3(const float a[3], const float b[3], float out[3])
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

/* The virtual screen (vr.h): the ray's screen pixel and distance */
static bool ray_to_virtual_screen(const float o[3], const float d[3], int *sx, int *sy, float *dist)
{
	const float w = VR_SCREEN_WIDTH_M, h = VR_SCREEN_WIDTH_M * 0.75f;
	if (d[2] >= -1e-6f)
		return false;
	float t = (-VR_SCREEN_DIST_M - o[2]) / d[2];
	if (t <= 0.0f)
		return false;
	float u = (o[0] + d[0] * t + w * 0.5f) / w;
	float v = (h * 0.5f - VR_SCREEN_DROP_M - (o[1] + d[1] * t)) / h;
	if (u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f)
		return false;
	*sx = (int)(u * GL_SCREEN_W);
	*sy = (int)(v * GL_SCREEN_H);
	*dist = t;
	return true;
}

void laser_update(bool cockpit)
{
	VrHand hands[2];
	bool any = false;
	for (int i = 0; i < 2; i++)
	{
		bool was_trigger = trigger_on[i], was_squeeze = squeeze_on[i];
		any = vr_hand(i, &hands[i]) || any;
		trigger_on[i] = hands[i].active && hands[i].trigger > (was_trigger ? TRIGGER_UP : TRIGGER_DOWN);
		squeeze_on[i] = hands[i].active && hands[i].squeeze;
		/* a click picks the hand that points */
		if ((trigger_on[i] && !was_trigger) || (squeeze_on[i] && !was_squeeze))
		{
			if (i != current)
				buttons = held = 0;
			current = i;
		}
	}
	if (!hands[current].active && hands[!current].active)
	{
		current = !current;
		buttons = held = 0;
	}

	int sx = 0, sy = 0;
	bool hit = false;
	for (int i = 0; i < 2; i++)
	{
		Beam *b = &beams[i];
		const VrHand *h = &hands[i];
		b->on = h->active;
		if (!b->on)
			continue;
		int x, y;
		float dist = -1.0f;
		bool on = cockpit ? cockpit_ray_to_screen(h->pos, h->dir, &x, &y, &dist)
						  : ray_to_virtual_screen(h->pos, h->dir, &x, &y, &dist);
		b->hit_near = on && dist > 0.0f;
		float len = b->hit_near ? dist : BEAM_LENGTH;
		memcpy(b->pos, h->pos, sizeof b->pos);
		for (int k = 0; k < 3; k++)
			b->end[k] = h->pos[k] + h->dir[k] * len;
		if (i == current && on)
		{
			hit = true;
			sx = x;
			sy = y;
		}
	}

	/* the buttons go down only when pressed on something, and up anywhere */
	int want = (squeeze_on[current] ? 0x1 : 0) | (trigger_on[current] ? 0x2 : 0);
	buttons = (buttons & want) | (hit ? want & ~held : 0);
	held = want;
	Input_SetLaser(any, hit, (float)sx + 0.5f, (float)sy + 0.5f, buttons);
	was_active = any;
}

void laser_off(void)
{
	beams[0].on = beams[1].on = false;
	trigger_on[0] = trigger_on[1] = squeeze_on[0] = squeeze_on[1] = false;
	buttons = held = 0;
	if (was_active)
		Input_SetLaser(false, false, 0, 0, 0);
	was_active = false;
}

bool laser_shown(void)
{
	return beams[0].on || beams[1].on;
}

/* A quad facing the eye: a..b, `half` wide either side, colours ca to cb */
static void ribbon(const float a[3], const float b[3], float half, const float eye[3], const float ca[3],
				   const float cb[3])
{
	float along[3], to_eye[3], side[3];
	for (int k = 0; k < 3; k++)
	{
		along[k] = b[k] - a[k];
		to_eye[k] = eye[k] - (a[k] + b[k]) * 0.5f;
	}
	cross3(along, to_eye, side);
	float l = sqrtf(dot3(side, side));
	if (l < 1e-12f)
		return;
	float p[4][3];
	for (int k = 0; k < 3; k++)
	{
		float s = side[k] / l * half;
		p[0][k] = a[k] - s;
		p[1][k] = a[k] + s;
		p[2][k] = b[k] - s;
		p[3][k] = b[k] + s;
	}
	gd_tri_rgb(p[0], ca, p[1], ca, p[2], cb);
	gd_tri_rgb(p[1], ca, p[3], cb, p[2], cb);
}

void laser_queue(const mat4 *play_to_clip, const float eye[3])
{
	if (!laser_shown())
		return;
	gd_set_projection(play_to_clip);
	gd_push();
	gd_identity();
	gd_set_cull(false);
	for (int i = 0; i < 2; i++)
	{
		const Beam *b = &beams[i];
		if (!b->on)
			continue;
		/* the pointing hand's bright, fading along the beam */
		static const float bright[2][3] = {{0.5f, 1.0f, 0.5f}, {0.1f, 0.45f, 0.1f}};
		static const float dim[2][3] = {{0.35f, 0.4f, 0.35f}, {0.1f, 0.12f, 0.1f}};
		const float(*c)[3] = i == current ? bright : dim;
		ribbon(b->pos, b->end, BEAM_WIDTH * 0.5f, eye, c[0], c[1]);
		if (b->hit_near)
		{
			/* the dot: a short ribbon across the beam's end */
			float along[3], to_eye[3], up[3];
			for (int k = 0; k < 3; k++)
			{
				along[k] = b->end[k] - b->pos[k];
				to_eye[k] = eye[k] - b->end[k];
			}
			cross3(along, to_eye, up);
			float l = sqrtf(dot3(up, up));
			if (l < 1e-12f)
				continue;
			float a[3], e[3];
			for (int k = 0; k < 3; k++)
			{
				a[k] = b->end[k] - up[k] / l * DOT_SIZE * 0.5f;
				e[k] = b->end[k] + up[k] / l * DOT_SIZE * 0.5f;
			}
			ribbon(a, e, DOT_SIZE * 0.5f, eye, c[0], c[0]);
		}
	}
	gd_pop();
}

void laser_draw_overlay(int w, int h, const VrView *v)
{
	int rects[GD_VP_COUNT][4];
	for (int i = 0; i < GD_VP_COUNT; i++)
	{
		rects[i][0] = rects[i][1] = 0;
		rects[i][2] = w;
		rects[i][3] = h;
	}
	glViewport(0, 0, w, h);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	glClearColor(0, 0, 0, 1);
	gd_begin_frame(rects);
	gd_set_viewport(GD_VP_WINDOW);
	mat4 p = cockpit_headset_projection(v);
	laser_queue(&p, v->pos);
	gd_flush();
}
