/*
 * gl_laser.c - see gl_laser.h.
 */
#include <math.h>
#include <string.h>

#include "gl_api.h"
#include "gl_cockpit.h"
#include "gl_draw.h"
#include "gl_laser.h"

#include "gamepad.h"
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
	bool aiming;   /* out into space with the grip held: its trigger clicks there (targets) */
} Beam;

/* What a trigger does, decided as it is pulled: a mouse click where its
 * beam is, or the pad's trigger */
typedef enum
{
	TRIGGER_OFF,
	TRIGGER_CLICK,
	TRIGGER_PAD
} TriggerUse;

static Beam beams[2];
static int current = 1; /* the hand pointing the mouse */
static bool trigger_on[2];
static TriggerUse trigger_use[2];
static bool was_active, was_connected;

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

/* A hand's controls as the virtual pad (gamepad.h), in the Xbox layout:
 * left hand on the left half, right on the right, the trackpad as the
 * d-pad (left) or the face buttons (right) */
static void add_to_pad(int i, const VrHand *h, GamepadVirtual *v)
{
	typedef struct
	{
		int x, y, trigger, stick, shoulder, menu, lower, upper;
		int pad[4]; /* up, down, left, right */
	} PadMap;
	static const PadMap MAP[2] = {
		{SDL_CONTROLLER_AXIS_LEFTX,
		 SDL_CONTROLLER_AXIS_LEFTY,
		 SDL_CONTROLLER_AXIS_TRIGGERLEFT,
		 SDL_CONTROLLER_BUTTON_LEFTSTICK,
		 SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
		 SDL_CONTROLLER_BUTTON_BACK,
		 SDL_CONTROLLER_BUTTON_X,
		 SDL_CONTROLLER_BUTTON_Y,
		 {SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT,
		  SDL_CONTROLLER_BUTTON_DPAD_RIGHT}},
		{SDL_CONTROLLER_AXIS_RIGHTX,
		 SDL_CONTROLLER_AXIS_RIGHTY,
		 SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
		 SDL_CONTROLLER_BUTTON_RIGHTSTICK,
		 SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
		 SDL_CONTROLLER_BUTTON_START,
		 SDL_CONTROLLER_BUTTON_A,
		 SDL_CONTROLLER_BUTTON_B,
		 {SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_B}},
	};
	const PadMap *m = &MAP[i];
	v->axes[m->x] = h->stick[0];
	v->axes[m->y] = -h->stick[1]; /* SDL: down + */
	v->axes[m->trigger] = trigger_use[i] == TRIGGER_PAD ? h->trigger : 0.0f;
	unsigned b = 0;
	if (h->stick_click)
		b |= 1u << m->stick;
	if (h->squeeze)
		b |= 1u << m->shoulder;
	if (h->menu)
		b |= 1u << m->menu;
	if (h->lower)
		b |= 1u << m->lower;
	if (h->upper)
		b |= 1u << m->upper;
	if (h->pad_click)
	{
		float x = h->pad[0], y = h->pad[1];
		b |= 1u << m->pad[fabsf(x) > fabsf(y) ? (x < 0 ? 2 : 3) : (y > 0 ? 0 : 1)];
	}
	v->buttons |= b;
}

void laser_update(bool cockpit)
{
	VrHand hands[2];
	bool connected = false, active = false;
	bool hit[2] = {false, false}, clickable[2] = {false, false};
	int hx[2] = {0, 0}, hy[2] = {0, 0};
	for (int i = 0; i < 2; i++)
	{
		connected = vr_hand(i, &hands[i]) || connected;
		const VrHand *h = &hands[i];
		Beam *b = &beams[i];
		b->on = h->active;
		if (!b->on)
			continue;
		active = true;
		float dist = -1.0f;
		hit[i] = cockpit ? cockpit_ray_to_screen(h->pos, h->dir, &hx[i], &hy[i], &dist)
						 : ray_to_virtual_screen(h->pos, h->dir, &hx[i], &hy[i], &dist);
		b->hit_near = hit[i] && dist > 0.0f;
		/* on the panel or a screen; the HUD (out into space) only with the grip */
		clickable[i] = hit[i] && (b->hit_near || h->squeeze);
		b->aiming = hit[i] && !b->hit_near && h->squeeze;
		float len = b->hit_near ? dist : BEAM_LENGTH;
		memcpy(b->pos, h->pos, sizeof b->pos);
		for (int k = 0; k < 3; k++)
			b->end[k] = h->pos[k] + h->dir[k] * len;
	}

	/* the triggers: a click where the beam is, which makes that hand the
	 * pointing one, or the pad's */
	for (int i = 0; i < 2; i++)
	{
		bool was = trigger_on[i];
		trigger_on[i] = hands[i].connected && hands[i].trigger > (was ? TRIGGER_UP : TRIGGER_DOWN);
		if (!trigger_on[i])
			trigger_use[i] = TRIGGER_OFF;
		else if (!was)
		{
			trigger_use[i] = clickable[i] ? TRIGGER_CLICK : TRIGGER_PAD;
			if (clickable[i])
				current = i;
		}
	}
	if (!hands[current].active && hands[!current].active)
		current = !current;

	if (active || was_active)
		Input_SetLaser(active, hit[current], (float)hx[current] + 0.5f, (float)hy[current] + 0.5f,
					   trigger_use[current] == TRIGGER_CLICK ? 0x2 : 0);
	was_active = active;

	GamepadVirtual v;
	memset(&v, 0, sizeof v);
	v.active = connected;
	for (int i = 0; i < 2; i++)
		if (hands[i].connected)
			add_to_pad(i, &hands[i], &v);
	if (connected || was_connected)
		gamepad_set_virtual(&v);
	was_connected = connected;
}

void laser_off(void)
{
	beams[0].on = beams[1].on = false;
	trigger_on[0] = trigger_on[1] = false;
	trigger_use[0] = trigger_use[1] = TRIGGER_OFF;
	if (was_active)
		Input_SetLaser(false, false, 0, 0, 0);
	if (was_connected)
	{
		GamepadVirtual v;
		memset(&v, 0, sizeof v);
		gamepad_set_virtual(&v);
	}
	was_active = was_connected = false;
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
		static const float aim[2][3] = {{1.0f, 0.75f, 0.2f}, {0.5f, 0.3f, 0.05f}};
		const float(*c)[3] = b->aiming ? aim : i == current ? bright : dim;
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
