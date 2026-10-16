/*
 * gl_hud.c - see gl_hud.h.
 */
#include <math.h>
#include <string.h>

#include "gl_cockpit.h"
#include "gl_draw.h"
#include "gl_fill2d.h"
#include "gl_hud.h"
#include "gl_native.h"

#include "game_state.h"
#include "host.h" /* rdlong, rdword */
#include "m68000.h"
#include "renderer.h"
#include "screen_text.h"

bool own_hud = true;

#define HUD_COLOUR   0x3c   /* the game's colour for both (L63d4e, L63d76) */
#define VEL_MIN      10     /* velocity units (about 0.095 m/s each): 1 m/s */
#define DIST         1000.0f
#define LINE_W       0.6f   /* view pixels */
#define OBJ_VELOCITY 98     /* three longs, in the frame's axes */
#define OBJ_NAME     246    /* the label's string */
#define LABEL_COLOUR 15     /* L6393c's */
#define A6_TARGET_COL  716   /* word: the target's colour, palette index * 4 (L63990) */
#define A6_TARGET_DIST 718   /* words: its distance's mantissa, exponent */
#define A6_FMT_BUF     16832 /* L45bba_FmtDrawStr's buffer: the range (Lhud_target) */
#define A6_BIG_SPACE   14466
#define NUM_SLOTS      0x73  /* objects 0..$72 (L4477c) */
#define MAX_LABELS     32
#define TEXT_LEN       48

typedef struct
{
	float dir[3]; /* view space (unit) */
	unsigned char str[TEXT_LEN];
} HudLabel;

typedef struct
{
	bool cross;     /* the crosshair: the view has a gun */
	bool vel;       /* the velocity marker */
	float dir[3];   /* the velocity in view space (unit) */
	float rgb[3];
	/* the target (Lhud_target): its direction, the range as the game
	 * formats it, the tunnel's biggest square (half its side, view
	 * pixels) if the game would draw one */
	bool target, tunnel;
	float tdir[3];
	int tsize;
	unsigned char range[TEXT_LEN];
	float trgb[3];
	/* the labels on things (Lhud_labels) */
	int nlabels;
	HudLabel label[MAX_LABELS];
	float lrgb[3];
} HudFrame;

static HudFrame pending, pub, show;

static bool hud_on(void)
{
	return own_hud && use_renderer != R_OLD;
}

/* The game's colour `i`, `fallback` (rgb 444) while its palette has none */
static void palette_rgb(int i, int fallback, float rgb[3])
{
	int c = i >= 0 && i < len_main_palette ? MainPalette[i] : fallback;
	rgb[0] = (float)((c >> 8) & 0xf) / 15.0f;
	rgb[1] = (float)((c >> 4) & 0xf) / 15.0f;
	rgb[2] = (float)(c & 0xf) / 15.0f;
}

static void note_colour(void)
{
	palette_rgb(HUD_COLOUR, 0x0f0, pending.rgb);
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
 * L43bc6 does: camera i = sum over j of m[3j + i] * v[j], m in 1.15. False
 * when too slow to show. */
static bool velocity_dir(unsigned int ship, float dir[3])
{
	double v[3], c[3] = {0, 0, 0};
	for (int j = 0; j < 3; j++)
		v[j] = (double)rdlong(ship + OBJ_VELOCITY + 4 * j);
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			c[i] += (double)rdword(ship + 2 * (3 * j + i)) * v[j];
	double speed = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	double len = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
	if (speed < VEL_MIN || len <= 0)
		return false;
	dir[0] = (float)(c[0] / len);
	dir[1] = (float)(c[1] / len);
	dir[2] = (float)(-c[2] / len);
	return true;
}

void Call_HudVelocity(void)
{
	bool on = hud_on();
	unsigned int ship = game_player_ship();
	if (on && ship)
	{
		pending.vel = velocity_dir(ship, pending.dir);
		note_colour();
	}
	SetReg(REG_D0, on);
}

/* Where the native scene has the object this frame, view space (unit) */
static bool object_dir(unsigned int obj, float dir[3])
{
	float shrink;
	if (!native_object_pos(obj, dir, &shrink))
		return false;
	float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	if (!(len > 0))
		return false;
	for (int i = 0; i < 3; i++)
		dir[i] /= len;
	return true;
}

static void copy_str(unsigned char *dst, unsigned int src)
{
	int i = 0;
	for (; i < TEXT_LEN - 1 && rdbyte(src + i); i++)
		dst[i] = (unsigned char)rdbyte(src + i);
	dst[i] = 0;
}

/* The target, a0 (L63990, its range formatted at A6_FMT_BUF, d1 = 1 for a
 * tunnel): ours if the native scene has it */
void Call_HudTarget(void)
{
	unsigned int a6 = (unsigned int)GetReg(REG_A6);
	bool on = hud_on() && native_enabled() && object_dir((unsigned int)GetReg(REG_A0), pending.tdir);
	if (on)
	{
		pending.target = true;
		pending.tunnel = (GetReg(REG_D1) & 0xffff) != 0;
		/* L63990's: the distance's mantissa, negated, picks the size */
		int mant = (uint16_t)rdword(a6 + A6_TARGET_DIST);
		pending.tsize = (((-mant) & 0x7ff) >> 5) + 0x40;
		copy_str(pending.range, a6 + A6_FMT_BUF);
		palette_rgb((uint16_t)rdword(a6 + A6_TARGET_COL) >> 2, 0x0f0, pending.trgb);
	}
	SetReg(REG_D0, on);
}

static unsigned int slot_object(unsigned int space, int i)
{
	return space + (unsigned int)(int)rdword(FE2_obj_offsets + 2 * (unsigned int)i);
}

/* Angle between unit directions */
static float dir_angle(const float a[3], const float b[3])
{
	float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
	return acosf(d > 1.0f ? 1.0f : d < -1.0f ? -1.0f : d);
}

static float view_pixel(void)
{
	return 2.0f * tanf(CLASSIC_FOV * GLM_PI / 360.0f) / FILL2D_VIEW_H;
}

/* The labels: every object L4477c and L6393c would label if it were on
 * the game's screen, wherever it is. A thing on a planet (type 1) beside
 * its parent, within 10 pixels, has none, as there. */
void Call_HudLabels(void)
{
	bool on = hud_on() && native_enabled();
	if (on)
	{
		unsigned int space = (unsigned int)rdlong((unsigned int)GetReg(REG_A6) + A6_BIG_SPACE);
		float px = view_pixel();
		pending.nlabels = 0;
		palette_rgb(LABEL_COLOUR, 0xfff, pending.lrgb);
		for (int i = NUM_SLOTS - 1; i >= 0 && pending.nlabels < MAX_LABELS; i--)
		{
			int flags = (uint8_t)rdbyte(space + (unsigned int)i);
			if (!(flags & 0x08) || (flags & 0x20))
				continue;
			unsigned int obj = slot_object(space, i);
			HudLabel *l = &pending.label[pending.nlabels];
			if (rdbyte(obj + 96) || !object_dir(obj, l->dir))
				continue;
			if ((uint8_t)rdbyte(obj + 94) == 1)
			{
				int parent = (uint8_t)rdbyte(obj + 68);
				float pd[3];
				if (parent && object_dir(slot_object(space, parent), pd) && dir_angle(l->dir, pd) < 10 * px)
					continue;
			}
			else if ((uint8_t)rdbyte(obj + 95) > 0x11 || (uint16_t)rdword(obj + 90) == 0xba)
				continue;
			copy_str(l->str, obj + OBJ_NAME);
			pending.nlabels++;
		}
	}
	SetReg(REG_D0, on);
}

/* Under the cockpit's screen the game draws no HUD, but the flight view is
 * still seen round it: the crosshair as the last flight frame had it (and
 * its colour: the screen's palette is not the flight view's), the velocity
 * as it is now */
void hud_publish(bool screen)
{
	static HudFrame flight;
	if (!screen)
	{
		pub = flight = pending;
		return;
	}
	memset(&pub, 0, sizeof pub);
	unsigned int ship = game_player_ship();
	if (!hud_on() || !ship)
		return;
	pub.cross = flight.cross && game_flight_status() != FLIGHT_DOCKED;
	memcpy(pub.rgb, flight.rgb, sizeof pub.rgb);
	pub.vel = velocity_dir(ship, pub.dir);
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
	p->s = view_pixel();
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

static void spot_rect(const Spot *p, float x0, float y0, float x1, float y1)
{
	float a[3], b[3], c[3], e[3];
	spot_point(p, x0, y0, a);
	spot_point(p, x1, y0, b);
	spot_point(p, x1, y1, c);
	spot_point(p, x0, y1, e);
	gd_quad(a, b, c, e);
}

/* A string in the game's font, its top left at x, y (y up), a quad for
 * each run of lit pixels. DrawStr's codes: \r a new line 10 pixels down,
 * the others (a colour, a new position) skipped. */
static void spot_text(const Spot *p, float x, float y, const unsigned char *s)
{
	float cx = x, cy = y;
	for (; *s; s++)
	{
		int ch = *s;
		if (ch < 0x20)
		{
			int skip = ch == 1 || ch == 0x1e ? 1 : ch == 0x1f ? 2 : 0;
			if (ch == '\r')
				cx = x, cy -= 10;
			for (; skip && s[1]; skip--)
				s++;
			continue;
		}
		const unsigned char *g = screen_text_glyph(ch);
		if (!g)
			continue;
		for (int row = 0; row < 8; row++)
			for (int b = 0; b < 8;)
			{
				int e = b;
				while (e < 8 && (g[row] & (0x80 >> e)))
					e++;
				if (e > b)
					spot_rect(p, cx + b, cy - row - 1, cx + e, cy - row);
				b = e + 1;
			}
		cx += g[9];
	}
}

/* `a` turned towards `b` by the fraction t of the angle between them */
static void dir_towards(const float a[3], const float b[3], float t, float out[3])
{
	float th = dir_angle(a, b), k = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
	float w[3] = {b[0] - k * a[0], b[1] - k * a[1], b[2] - k * a[2]};
	if (w[0] * w[0] + w[1] * w[1] + w[2] * w[2] < 1e-8f)
	{
		/* straight behind: any way round */
		const float up[3] = {0, 1, 0}, right[3] = {1, 0, 0};
		const float *x = fabsf(a[1]) < 0.9f ? up : right;
		k = a[0] * x[0] + a[1] * x[1] + a[2] * x[2];
		for (int i = 0; i < 3; i++)
			w[i] = x[i] - k * a[i];
	}
	vec3_normalize(w);
	float c = cosf(t * th), s = sinf(t * th);
	for (int i = 0; i < 3; i++)
		out[i] = c * a[i] + s * w[i];
}

/* The game's tunnel (L63990, L63d9e_DrawTargetTunnelSquare): three
 * squares, half sides size, size / 2, size / 4, each from the target
 * towards the middle of the view by its half side / 128 */
static void draw_tunnel(const float target[3], int size)
{
	const float ahead[3] = {0, 0, -1};
	for (int k = 0; k < 3; k++, size >>= 1)
	{
		float c[3];
		dir_towards(target, ahead, (float)size / 128.0f, c);
		Spot p;
		spot_at(&p, c);
		float h = (float)size;
		spot_line(&p, -h, -h, h, -h);
		spot_line(&p, h, -h, h, h);
		spot_line(&p, h, h, -h, h);
		spot_line(&p, -h, h, -h, -h);
	}
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
	if (!show.cross && !show.vel && !show.target && !show.nlabels)
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
	/* the labels as L6393c puts them: 4 right of and 4 above the thing */
	gd_color3f(show.lrgb[0], show.lrgb[1], show.lrgb[2]);
	for (int i = 0; i < show.nlabels; i++)
	{
		spot_at(&p, show.label[i].dir);
		spot_text(&p, 4, 4, show.label[i].str);
	}
	/* the target: its tunnel, then the range as l63a62 puts it, 4 left of
	 * and 4 above (the string starts on the next line) */
	if (show.target)
	{
		gd_color3f(show.trgb[0], show.trgb[1], show.trgb[2]);
		if (show.tunnel)
			draw_tunnel(show.tdir, show.tsize);
		spot_at(&p, show.tdir);
		spot_text(&p, -4, 4, show.range);
	}
	gd_pop();
}
