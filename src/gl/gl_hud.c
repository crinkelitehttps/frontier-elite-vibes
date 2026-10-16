/*
 * gl_hud.c - see gl_hud.h.
 */
#include <math.h>
#include <string.h>

#include "gl_cockpit.h"
#include "gl_draw.h"
#include "gl_fill2d.h"
#include "gl_hud.h"

#include "game_state.h"
#include "host.h" /* rdlong, rdword */
#include "m68000.h"
#include "renderer.h"

bool own_hud = true;

#define HUD_COLOUR   0x3c   /* the game's colour for both (L63d4e, L63d76) */
#define VEL_MIN      10     /* velocity units (about 0.095 m/s each): 1 m/s */
#define DIST         1000.0f
#define LINE_W       0.6f   /* view pixels */
#define OBJ_VELOCITY 98     /* three longs, in the frame's axes */

typedef struct
{
	bool cross;     /* the crosshair: the view has a gun */
	bool vel;       /* the velocity marker */
	float dir[3];   /* the velocity in view space (unit) */
	float rgb[3];
} HudFrame;

static HudFrame pending, pub, show;

static bool hud_on(void)
{
	return own_hud && use_renderer != R_OLD;
}

static void note_colour(void)
{
	int c = len_main_palette > HUD_COLOUR ? MainPalette[HUD_COLOUR] : 0x0f0;
	pending.rgb[0] = (float)((c >> 8) & 0xf) / 15.0f;
	pending.rgb[1] = (float)((c >> 4) & 0xf) / 15.0f;
	pending.rgb[2] = (float)(c & 0xf) / 15.0f;
}

void Call_HudCross(void)
{
	bool on = hud_on();
	if (on)
	{
		pending.cross = true;
		note_colour();
	}
	SetReg(REG_D0, on);
}

/* The velocity turned into the ship's axes (the front view's camera) as
 * L43bc6 does: camera i = sum over j of m[3j + i] * v[j], m in 1.15 */
void Call_HudVelocity(void)
{
	bool on = hud_on();
	unsigned int ship = game_player_ship();
	if (on && ship)
	{
		double v[3], c[3] = {0, 0, 0};
		for (int j = 0; j < 3; j++)
			v[j] = (double)rdlong(ship + OBJ_VELOCITY + 4 * j);
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				c[i] += (double)rdword(ship + 2 * (3 * j + i)) * v[j];
		double speed = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		double len = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
		if (speed >= VEL_MIN && len > 0)
		{
			pending.vel = true;
			pending.dir[0] = (float)(c[0] / len);
			pending.dir[1] = (float)(c[1] / len);
			pending.dir[2] = (float)(-c[2] / len);
		}
		note_colour();
	}
	SetReg(REG_D0, on);
}

void hud_publish(bool screen)
{
	if (screen)
		memset(&pub, 0, sizeof pub);
	else
		pub = pending;
}

void hud_frame_end(void)
{
	memset(&pending, 0, sizeof pending);
}

void hud_acquire(void)
{
	show = pub;
}

/* =========================================================================
 * Drawing: shapes in view pixels (y up) round a direction d, on its
 * tangent plane, so they keep the game's size wherever they are
 * ========================================================================= */
typedef struct
{
	float d[3], r[3], u[3];
	float s; /* a view pixel at distance 1 */
} Spot;

static void cross3(const float a[3], const float b[3], float out[3])
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

/* Right along the view's x as far as it can be, up = right x d */
static void spot_at(Spot *p, const float d[3])
{
	memcpy(p->d, d, sizeof p->d);
	float x[3] = {1, 0, 0};
	if (fabsf(d[0]) > 0.99f)
		x[0] = 0, x[1] = d[0] > 0 ? -1.0f : 1.0f;
	float k = x[0] * d[0] + x[1] * d[1] + x[2] * d[2];
	for (int i = 0; i < 3; i++)
		p->r[i] = x[i] - k * d[i];
	vec3_normalize(p->r);
	cross3(p->r, d, p->u);
	p->s = 2.0f * tanf(CLASSIC_FOV * GLM_PI / 360.0f) / FILL2D_VIEW_H;
}

static void spot_point(const Spot *p, float x, float y, float out[3])
{
	for (int i = 0; i < 3; i++)
		out[i] = DIST * (p->d[i] + p->s * (x * p->r[i] + y * p->u[i]));
}

/* A line LINE_W view pixels wide */
static void spot_line(const Spot *p, float x0, float y0, float x1, float y1)
{
	float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
	if (l <= 0)
		return;
	float nx = -dy / l * LINE_W * 0.5f, ny = dx / l * LINE_W * 0.5f;
	float a[3], b[3], c[3], e[3];
	spot_point(p, x0 + nx, y0 + ny, a);
	spot_point(p, x1 + nx, y1 + ny, b);
	spot_point(p, x1 - nx, y1 - ny, c);
	spot_point(p, x0 - nx, y0 - ny, e);
	gd_quad(a, b, c, e);
}

/* The game's crosshair (L63d4e..L63d6c): four ticks 7..15 out */
static void draw_cross(const Spot *p)
{
	spot_line(p, -15, 0, -7, 0);
	spot_line(p, 7, 0, 15, 0);
	spot_line(p, 0, -15, 0, -7);
	spot_line(p, 0, 7, 0, 15);
}

/* The game's velocity marker (L63d76..L63d94): four diagonal ticks 9..14
 * out; retrograde with a ring inside them */
static void draw_velocity(const Spot *p, bool retro)
{
	for (int i = 0; i < 4; i++)
	{
		float sx = (i & 1) ? 1.0f : -1.0f, sy = (i & 2) ? 1.0f : -1.0f;
		spot_line(p, 9 * sx, 9 * sy, 14 * sx, 14 * sy);
	}
	if (retro)
	{
		const int n = 16;
		const float r = 6.0f;
		for (int i = 0; i < n; i++)
		{
			float a0 = 2.0f * GLM_PI * i / n, a1 = 2.0f * GLM_PI * (i + 1) / n;
			spot_line(p, r * cosf(a0), r * sinf(a0), r * cosf(a1), r * sinf(a1));
		}
	}
	else
	{
		spot_line(p, -0.5f, 0, 0.5f, 0);
	}
}

void hud_draw(const mat4 *dir_proj)
{
	if (!show.cross && !show.vel)
		return;
	gd_set_viewport(GD_VP_VIEW3D);
	gd_set_projection(dir_proj);
	gd_push();
	gd_identity();
	gd_set_cull(false);
	gd_color3f(show.rgb[0], show.rgb[1], show.rgb[2]);
	Spot p;
	if (show.cross)
	{
		const float ahead[3] = {0, 0, -1};
		spot_at(&p, ahead);
		draw_cross(&p);
	}
	if (show.vel)
	{
		/* both ways: whichever is in front of the eye shows */
		float back[3] = {-show.dir[0], -show.dir[1], -show.dir[2]};
		spot_at(&p, show.dir);
		draw_velocity(&p, false);
		spot_at(&p, back);
		draw_velocity(&p, true);
	}
	gd_pop();
}
