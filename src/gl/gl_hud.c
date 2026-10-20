/*
 * gl_hud.c - see gl_hud.h.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gl_cockpit.h"
#include "gl_draw.h"
#include "gl_fill2d.h"
#include "gl_hud.h"
#include "gl_native.h"

#include "game_state.h"
#include "host.h" /* rdlong, rdword */
#include "hostcall.h"
#include "m68000.h"
#include "renderer.h"
#include "screen_text.h"

bool own_hud = true;
bool orbit_display = false;

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
#define TUNNEL_SQ      200
#define TUNNEL_GAP     0.15  /* squares this far apart along the orbit, and with half sides this much, in radii from the planet's centre */
#define TUNNEL_HALF    0.02
#define TUNNEL_PERIOD  1.5   /* seconds for the squares to flow on by one gap */
#define TUNNEL_MIN_PX  1.5f  /* view pixels: the half side of the last square */
#define TUNNEL_EDGE    0.1f  /* radians: the longest piece of a square's side */
#define TEXT_LEN       48

typedef struct
{
	float dir[3]; /* view space (unit) */
	unsigned char str[TEXT_LEN];
} HudLabel;

typedef struct
{
	bool on, bound;
	float ap, pe; /* heights above the body, km */
	float tap;    /* game seconds to the next apsis (bound, not circular) */
	bool has_tap, to_pe; /* to_pe: past the apoapsis, so it is the periapsis */
	/* the orbit as a tunnel of n squares ahead of the ship, nearest first:
	 * each square's corners as directions from the ship in view space
	 * (unit), in order round it */
	int n;
	float sq[TUNNEL_SQ][4][3];
	bool has_pe, has_ap;
	float pe_dir[3], ap_dir[3];
} HudOrbit;

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
	/* the orbit round the body the ship is relative to */
	HudOrbit orbit;
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
static void frame_to_view(unsigned int ship, const double v[3], double c[3])
{
	for (int i = 0; i < 3; i++)
	{
		c[i] = 0;
		for (int j = 0; j < 3; j++)
			c[i] += (double)rdword(ship + 2 * (3 * j + i)) * v[j];
	}
}

static bool velocity_dir(unsigned int ship, float dir[3])
{
	double v[3], c[3];
	for (int j = 0; j < 3; j++)
		v[j] = (double)rdlong(ship + OBJ_VELOCITY + 4 * j);
	frame_to_view(ship, v, c);
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
/* The orbit round the frame's body (the ship's parent, with a gravity
 * term at +122/+124): L43ed6 pulls the velocity by G * dt / r^2 per tick
 * (G = mantissa * 2^(exponent - 51), r in position units, the velocity in
 * its own units; found by flying with the engines off), and the position
 * moves by v * (dt >> 7) / 4 / 2^32 per tick. So per tick in position
 * units u = v * (dt >> 7) / 2^34 and mu = G * dt * (dt >> 7) / 2^34; the
 * position units are 2^32 of the game's altitude units (1 mm). */
#define OBJ_POS_LO    44
#define OBJ_POS_HI    56
#define OBJ_VEL_FRAC  192
#define OBJ_GRAV_MANT 122
#define OBJ_RADIUS_LO 266 /* long: the fraction */
#define OBJ_RADIUS_HI 270 /* word: the whole part */
#define A6_TICK_DT    14446
#define PU_KM         4294.967296 /* a position unit in km */
#define TICK_DT       994         /* 50 ticks a second */

/* The conic's points from the ship, in view space. In the orbit's plane
 * the point at true anomaly nu is p / (1 + e cos nu) along
 * cos nu * (periapsis direction) + sin nu * (h x it) */
static bool dir_from_ship(unsigned int ship, const double r[3], const double P[3], float out[3])
{
	double d[3] = {P[0] - r[0], P[1] - r[1], P[2] - r[2]}, c[3];
	frame_to_view(ship, d, c);
	double len = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
	if (!(len > 0))
		return false;
	out[0] = (float)(c[0] / len);
	out[1] = (float)(c[1] / len);
	out[2] = (float)(-c[2] / len);
	return true;
}

/* The tunnel's squares: along the orbit from the ship, half the way round
 * each way (or to the asymptotes), TUNNEL_GAP radii apart, each square
 * TUNNEL_HALF radii to the side of the path, across it (in the orbit's
 * plane and square to it), so that perspective alone makes them shrink and
 * curve away. They flow against the ship's motion, as the orbit passes
 * under it: ahead they grow from nothing as they come on, behind they
 * shrink to nothing as they go. Those behind the planet are left out. */
typedef struct
{
	unsigned int ship;
	HudOrbit *o;
	const double *r, *pe, *q, *hn;
	double p, e, k, half, gap, ns;
} TunnelCtx;

/* The square at true anomaly nu, scaled by f (0..1); returns the arc's
 * step in nu for one gap there */
static double tunnel_square(const TunnelCtx *t, double nu, double f)
{
	double c = cos(nu), s = sin(nu), den = 1 + t->e * c, rad = t->p / den, drad = t->p * t->e * s / (den * den);
	double P[3], T[3], m[3];
	for (int i = 0; i < 3; i++)
	{
		P[i] = rad * (c * t->pe[i] + s * t->q[i]);
		T[i] = drad * (c * t->pe[i] + s * t->q[i]) + rad * (-s * t->pe[i] + c * t->q[i]);
	}
	double tl = sqrt(T[0] * T[0] + T[1] * T[1] + T[2] * T[2]);
	if (f <= 0 || t->o->n >= TUNNEL_SQ)
		return t->gap / tl;
	for (int i = 0; i < 3; i++)
		T[i] /= tl;
	const double *hn = t->hn, *r = t->r;
	m[0] = hn[1] * T[2] - hn[2] * T[1];
	m[1] = hn[2] * T[0] - hn[0] * T[2];
	m[2] = hn[0] * T[1] - hn[1] * T[0];
	double sz = t->half * f;
	double d[3] = {P[0] - r[0], P[1] - r[1], P[2] - r[2]};
	double a = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
	double b = r[0] * d[0] + r[1] * d[1] + r[2] * d[2];
	double disc = b * b - a * t->k, u = a > 0 ? (-b - sqrt(fmax(disc, 0))) / a : 2;
	if ((disc > 0 && u > 0 && u < 1) || !(sz / sqrt(a) > TUNNEL_MIN_PX * view_pixel()))
		return t->gap / tl;
	static const int sx[4] = {-1, 1, 1, -1}, sy[4] = {-1, -1, 1, 1};
	float out[4][3];
	for (int j = 0; j < 4; j++)
	{
		double C[3];
		for (int i = 0; i < 3; i++)
			C[i] = P[i] + sz * (sx[j] * hn[i] + sy[j] * m[i]);
		if (!dir_from_ship(t->ship, r, C, out[j]))
			return t->gap / tl;
	}
	memcpy(t->o->sq[t->o->n++], out, sizeof out);
	return t->gap / tl;
}

static void tunnel_calc(unsigned int ship, HudOrbit *o, const double r[3], const double pe[3], const double q[3],
						const double h[3], double hl, double p, double e, double radius, double lim, bool closed,
						double ns)
{
	double rr = sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
	double hn[3] = {h[0] / hl, h[1] / hl, h[2] / hl};
	TunnelCtx t = {ship, o, r, pe, q, hn, p, e, r[0] * r[0] + r[1] * r[1] + r[2] * r[2] - radius * radius,
				   TUNNEL_HALF * rr, TUNNEL_GAP * rr, ns};
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	double phase = fmod((double)ts.tv_sec + ts.tv_nsec * 1e-9, TUNNEL_PERIOD) / TUNNEL_PERIOD;
	o->n = 0;
	/* dir +1 ahead, -1 behind; dist = arc from the ship, the first square
	 * a bit short of a gap, moving in on the ship as phase grows */
	for (int dir = 1; dir >= -1; dir -= 2)
	{
		double span = closed ? GLM_PI : lim, dist = (dir > 0 ? 1 - phase : phase) * t.gap, nu = ns;
		/* walk in nu to the first square */
		{
			double st = tunnel_square(&t, nu, 0) / t.gap; /* nu per unit arc */
			nu += dir * dist * st;
		}
		for (int it = 0; it < TUNNEL_SQ && o->n < TUNNEL_SQ; it++)
		{
			double left = span - dir * (nu - ns), f;
			double st = tunnel_square(&t, nu, 0) / t.gap;
			if (left <= 0)
				break;
			f = fmin(fmin(1.0, dist / t.gap), left / (t.gap * st));
			if (dir < 0)
				f = fmin(f, 1.0);
			tunnel_square(&t, nu, f);
			nu += dir * t.gap * st;
			dist += t.gap;
		}
	}
}

static ConicElems last_conic;

static void conic_calc(unsigned int ship, HudOrbit *o, const double r[3], const double h[3], double hh,
					   const double ev[3], double e, double mu, double rp, double radius)
{
	double rr = sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]), hl = sqrt(hh);
	double pe[3], q[3], p = rp * (1 + e);
	for (int i = 0; i < 3; i++)
		pe[i] = e > 1e-6 ? ev[i] / e : r[i] / rr;
	q[0] = (h[1] * pe[2] - h[2] * pe[1]) / hl;
	q[1] = (h[2] * pe[0] - h[0] * pe[2]) / hl;
	q[2] = (h[0] * pe[1] - h[1] * pe[0]) / hl;
	double lim = GLM_PI;
	if (e >= 1)
	{
		/* out to the asymptotes, where the path leaves for good */
		lim = acos(-1.0 / e) - 0.002;
	}
	last_conic = (ConicElems){{pe[0], pe[1], pe[2]}, {q[0], q[1], q[2]}, p, e, rp, radius, lim, e < 1};
	/* The ship is on the orbit, at true anomaly ns */
	double ns = atan2(r[0] * q[0] + r[1] * q[1] + r[2] * q[2], r[0] * pe[0] + r[1] * pe[1] + r[2] * pe[2]);
	tunnel_calc(ship, o, r, pe, q, h, hl, p, e, radius, lim, e < 1, ns);
	double P[3];
	for (int i = 0; i < 3; i++)
		P[i] = rp * pe[i];
	o->has_pe = dir_from_ship(ship, r, P, o->pe_dir);
	o->has_ap = false;
	if (e < 1)
	{
		for (int i = 0; i < 3; i++)
			P[i] = -p / (1 - e) * pe[i];
		o->has_ap = dir_from_ship(ship, r, P, o->ap_dir);
	}
	(void)mu;
}

static bool orbit_calc(unsigned int ship, HudOrbit *o)
{
	unsigned int a6 = GAME_A6;
	int par = (uint8_t)rdbyte(ship + 68);
	if (!par)
		return false;
	unsigned int po = slot_object((unsigned int)rdlong(a6 + A6_BIG_SPACE), par);
	int mant = (int16_t)rdword(po + OBJ_GRAV_MANT), exp = (int16_t)rdword(po + OBJ_GRAV_MANT + 2);
	if (mant <= 0)
		return false;
	double dt = (double)(uint32_t)rdlong(a6 + A6_TICK_DT);
	if (dt < 128 || dt > 1e8)
		dt = TICK_DT;
	dt = (double)((uint32_t)dt >> substep_shift((uint32_t)dt)); /* the step the game's gravity takes */
	double tq = exact_step ? dt / 128.0 : floor(dt / 128.0), k = tq / 17179869184.0; /* 2^34 */
	double mu = (double)mant * ldexp(1.0, exp - 51) * dt * k;
	double r[3], u[3];
	for (int i = 0; i < 3; i++)
	{
		r[i] = (double)rdlong(ship + OBJ_POS_HI + 4 * i) +
			   (double)(uint32_t)rdlong(ship + OBJ_POS_LO + 4 * i) / 4294967296.0;
		u[i] = k * ((double)rdlong(ship + OBJ_VELOCITY + 4 * i) + (double)(rdword(ship + OBJ_VEL_FRAC + 2 * i) & 0xffff) / 65536.0);
	}
	double rr = sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
	double uu = u[0] * u[0] + u[1] * u[1] + u[2] * u[2];
	double h[3] = {r[1] * u[2] - r[2] * u[1], r[2] * u[0] - r[0] * u[2], r[0] * u[1] - r[1] * u[0]};
	double hh = h[0] * h[0] + h[1] * h[1] + h[2] * h[2];
	double ev[3] = {(u[1] * h[2] - u[2] * h[1]) / mu - r[0] / rr, (u[2] * h[0] - u[0] * h[2]) / mu - r[1] / rr,
					(u[0] * h[1] - u[1] * h[0]) / mu - r[2] / rr};
	double e = sqrt(ev[0] * ev[0] + ev[1] * ev[1] + ev[2] * ev[2]);
	double energy = uu / 2 - mu / rr;
	double radius = (double)rdword(po + OBJ_RADIUS_HI) + (double)(uint32_t)rdlong(po + OBJ_RADIUS_LO) / 4294967296.0;
	if (!(rr > radius) || !(mu > 0) || !(hh > 0))
		return false;
	double rp = hh / (mu * (1 + e));
	o->pe = (float)((rp - radius) * PU_KM);
	o->bound = energy < 0;
	o->ap = o->bound ? (float)((-mu / (2 * energy) * (1 + e) - radius) * PU_KM) : 0;
	o->has_tap = false;
	if (o->bound && e > 1e-4 && e < 1)
	{
		/* true anomaly -> eccentric -> mean; the apoapsis is at M = pi */
		double nu = acos(fmax(-1.0, fmin(1.0, (ev[0] * r[0] + ev[1] * r[1] + ev[2] * r[2]) / (e * rr))));
		if (r[0] * u[0] + r[1] * u[1] + r[2] * u[2] < 0)
			nu = 2 * GLM_PI - nu;
		double a = -mu / (2 * energy);
		double ea = 2 * atan2(sqrt(1 - e) * sin(nu / 2), sqrt(1 + e) * cos(nu / 2));
		double m = fmod(ea - e * sin(ea) + 2 * GLM_PI, 2 * GLM_PI);
		o->to_pe = m > GLM_PI;
		double ticks = (o->to_pe ? 2 * GLM_PI - m : GLM_PI - m) / sqrt(mu / (a * a * a));
		o->tap = (float)(ticks * (dt / TICK_DT) / 50.0);
		o->has_tap = true;
	}
	conic_calc(ship, o, r, h, hh, ev, e, mu, rp, radius);
	return true;
}

bool hud_conic_elements(ConicElems *out)
{
	static HudOrbit tmp;
	unsigned int own = game_player_ship();
	if (!orbit_display || !hud_on() || !own || !orbit_calc(own, &tmp))
		return false;
	*out = last_conic;
	return true;
}

/* A height in km, as the game has it in its panel: the unit with the size */
static void fmt_km(char *dst, size_t n, const char *tag, float km)
{
	if (fabsf(km) < 100)
		snprintf(dst, n, "%s %.1f km", tag, km);
	else
		snprintf(dst, n, "%s %.0f km", tag, km);
}

/* A time as h/m/s, the largest two units */
static void fmt_time(char *dst, size_t n, const char *tag, float sec)
{
	int t = (int)sec;
	if (t >= 3600)
		snprintf(dst, n, "%s %dh %02dm", tag, t / 3600, t / 60 % 60);
	else if (t >= 60)
		snprintf(dst, n, "%s %dm %02ds", tag, t / 60, t % 60);
	else
		snprintf(dst, n, "%s %ds", tag, t);
}

void hud_publish(bool screen)
{
	static HudFrame flight;
	unsigned int own = game_player_ship();
	HudOrbit orbit = {0};
	if (hud_on() && own && orbit_display)
		orbit.on = orbit_calc(own, &orbit);
	if (!screen)
	{
		pub = flight = pending;
		pub.orbit = orbit;
		if (orbit.on)
			palette_rgb(HUD_COLOUR, 0x0f0, pub.rgb);
		return;
	}
	memset(&pub, 0, sizeof pub);
	unsigned int ship = game_player_ship();
	if (!hud_on() || !ship)
		return;
	pub.cross = flight.cross && game_flight_status() != FLIGHT_DOCKED;
	memcpy(pub.rgb, flight.rgb, sizeof pub.rgb);
	pub.vel = velocity_dir(ship, pub.dir);
	pub.orbit = orbit;
	if (orbit.on && !pub.cross)
		palette_rgb(HUD_COLOUR, 0x0f0, pub.rgb);
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

/* A line LINE_W view pixels wide between two directions, on the sphere
 * of radius DIST */
static void arc_line(const float a[3], const float b[3])
{
	float A[3], B[3], m[3], d[3], n[3];
	for (int i = 0; i < 3; i++)
		A[i] = DIST * a[i], B[i] = DIST * b[i], m[i] = A[i] + B[i], d[i] = B[i] - A[i];
	cross3(m, d, n);
	float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	if (!(l > 0))
		return;
	float w = LINE_W * 0.5f * view_pixel() * DIST / l;
	float p[4][3];
	for (int i = 0; i < 3; i++)
	{
		p[0][i] = A[i] + n[i] * w;
		p[1][i] = B[i] + n[i] * w;
		p[2][i] = B[i] - n[i] * w;
		p[3][i] = A[i] - n[i] * w;
	}
	gd_quad(p[0], p[1], p[2], p[3]);
}

static void draw_conic(const HudOrbit *o)
{
	for (int i = 0; i < o->n; i++)
		for (int j = 0; j < 4; j++)
		{
			const float *a = o->sq[i][j], *b = o->sq[i][(j + 1) & 3];
			int pieces = 1 + (int)(dir_angle(a, b) / TUNNEL_EDGE);
			float prev[3] = {a[0], a[1], a[2]};
			for (int k = 1; k <= pieces; k++)
			{
				float next[3];
				dir_towards(a, b, (float)k / pieces, next);
				arc_line(prev, next);
				memcpy(prev, next, sizeof prev);
			}
		}
	/* From prograde round the planet to retrograde; nothing between them */
	Spot p;
	if (o->has_pe)
	{
		spot_at(&p, o->pe_dir);
		spot_line(&p, -4, -4, 4, -4);
		spot_line(&p, 4, -4, 4, 4);
		spot_line(&p, 4, 4, -4, 4);
		spot_line(&p, -4, 4, -4, -4);
		spot_text(&p, 7, 4, (const unsigned char *)"Pe");
	}
	if (o->has_ap)
	{
		spot_at(&p, o->ap_dir);
		spot_line(&p, -4, -4, 4, -4);
		spot_line(&p, 4, -4, 4, 4);
		spot_line(&p, 4, 4, -4, 4);
		spot_line(&p, -4, 4, -4, -4);
		spot_text(&p, 7, 4, (const unsigned char *)"Ap");
	}
}

void hud_draw(const mat4 *dir_proj)
{
	if (!show.cross && !show.vel && !show.target && !show.nlabels && !show.orbit.on)
		return;
	gd_set_viewport(GD_VP_VIEW3D);
	gd_set_projection(dir_proj);
	gd_push();
	gd_identity();
	gd_set_cull(false);
	gd_color3f(show.rgb[0], show.rgb[1], show.rgb[2]);
	Spot p;
	if (show.orbit.on && show.orbit.n > 1)
	{
		draw_conic(&show.orbit);
		gd_color3f(show.rgb[0], show.rgb[1], show.rgb[2]);
	}
	if (show.cross)
	{
		const float ahead[3] = {0, 0, -1};
		spot_at(&p, ahead);
		draw_cross(&p);
	}
	if (show.orbit.on)
	{
		/* below and right of the crosshair */
		const float ahead[3] = {0, 0, -1};
		unsigned char text[TEXT_LEN * 3];
		char ap[TEXT_LEN], pe[TEXT_LEN], ta[TEXT_LEN];
		if (show.orbit.bound)
			fmt_km(ap, sizeof ap, "Ap", show.orbit.ap);
		else
			snprintf(ap, sizeof ap, "Ap ---");
		fmt_km(pe, sizeof pe, "Pe", show.orbit.pe);
		if (show.orbit.has_tap)
			fmt_time(ta, sizeof ta, show.orbit.to_pe ? "T-Pe" : "T-Ap", show.orbit.tap);
		else
			snprintf(ta, sizeof ta, "T-Ap ---");
		snprintf((char *)text, sizeof text, "%s\r%s\r%s", ap, pe, ta);
		spot_at(&p, ahead);
		spot_text(&p, 22, -18, text);
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
