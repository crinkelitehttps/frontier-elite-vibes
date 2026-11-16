/*
 * orbit_guide.c - see orbit_guide.h.
 */
#include "orbit_guide.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cheats.h"
#include "game_state.h"
#include "host.h"
#include "time_guard.h"

#define OBJ_VELOCITY 98
#define OBJ_VEL_FRAC 192
#define A6_CLOCK_LO  524
#define A6_CLOCK_HI  528
#define A6_TICK_DT   14446
#define PU_KM        4294.967296
#define REF_DT       994.0
#define CLOCK_HZ     49710.0
#define SAMPLES      180
#define TWO_PI       6.283185307179586
#define TURN_STEP    0.35 /* radians */

static double dot3(const double a[3], const double b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* the planned orbit's position and velocity at true anomaly nu */
static void orbit_at(const OrbitCtx *c, const OrbitEl *o, double nu, double r[3], double u[3])
{
	double rad = o->p / (1 + o->e * cos(nu)), vs = sqrt(c->mu / o->p);
	for (int i = 0; i < 3; i++)
	{
		r[i] = rad * (cos(nu) * o->pe[i] + sin(nu) * o->q[i]);
		u[i] = vs * (-sin(nu) * o->pe[i] + (o->e + cos(nu)) * o->q[i]);
	}
}

static double dist_at(const OrbitCtx *c, const OrbitEl *o, const double r[3], double nu)
{
	double p[3], u[3];
	orbit_at(c, o, nu, p, u);
	double d[3] = {p[0] - r[0], p[1] - r[1], p[2] - r[2]};
	return sqrt(dot3(d, d));
}

/* the true anomaly of the point of the orbit nearest r, searched within [lo, hi] */
static double refine(const OrbitCtx *c, const OrbitEl *o, const double r[3], double lo, double hi)
{
	for (int i = 0; i < 40; i++)
	{
		double a = lo + (hi - lo) / 3, b = hi - (hi - lo) / 3;
		if (dist_at(c, o, r, a) < dist_at(c, o, r, b))
			hi = b;
		else
			lo = a;
	}
	return (lo + hi) / 2;
}

static double nearest_nu(const OrbitCtx *c, const OrbitEl *o, const double r[3], double *best_d)
{
	int bi = 0;
	double bd = 1e300;
	for (int i = 0; i < SAMPLES; i++)
	{
		double d = dist_at(c, o, r, -M_PI + TWO_PI * i / SAMPLES);
		if (d < bd)
			bd = d, bi = i;
	}
	double step = TWO_PI / SAMPLES, nu0 = -M_PI + step * bi;
	double nu = refine(c, o, r, nu0 - step, nu0 + step);
	*best_d = dist_at(c, o, r, nu);
	return nu;
}

/* Going straight to vc trades speed for direction (a turn through an angle by a straight change of velocity dips
 * to cos(angle / 2) of the speed, which for a plane change drops a low orbit into the planet). The ship turns at
 * its own speed instead, a step of at most TURN_STEP radians at a time, and the speed is brought to vc's apart. */
static void turn_toward(const OrbitCtx *c, const double vc[3], double uw[3])
{
	double us = sqrt(dot3(c->u, c->u)), vs = sqrt(dot3(vc, vc));
	for (int i = 0; i < 3; i++)
		uw[i] = vc[i];
	if (us > 0.3 * vs && vs > 0)
	{
		double uh[3], ch = 0, e[3], el = 0;
		for (int i = 0; i < 3; i++)
			uh[i] = c->u[i] / us, ch += uh[i] * vc[i] / vs;
		ch = ch > 1 ? 1 : ch < -1 ? -1 : ch;
		for (int i = 0; i < 3; i++)
			e[i] = vc[i] / vs - ch * uh[i], el += e[i] * e[i];
		el = sqrt(el);
		double th = acos(ch);
		if (el > 1e-9)
		{
			double step = th < TURN_STEP ? th : TURN_STEP;
			for (int i = 0; i < 3; i++)
				uw[i] = c->u[i] + e[i] / el * us * sin(step) + uh[i] * (vs - us);
		}
	}
}

void orbit_guide_reset(OrbitGuide *g)
{
	g->have_prev = false;
	g->nu_prev = 0;
}

bool orbit_guide_step(const OrbitCtx *c, const OrbitPlan *pl, OrbitGuide *g, double amax, GuideOut *out)
{
	memset(out, 0, sizeof *out);
	if (!c->ok || !pl->valid || !(c->mu > 0))
		return false;
	const OrbitEl *o = &pl->el;
	double bd, nu = nearest_nu(c, o, c->r, &bd);
	/* between two equally near points the one it was following stays */
	if (g->have_prev)
	{
		double lo = g->nu_prev - 0.5, hi = g->nu_prev + 0.5;
		double nl = refine(c, o, c->r, lo, hi), dl = dist_at(c, o, c->r, nl);
		if (dl < 2 * bd + 1e-12)
			nu = nl, bd = dl;
	}
	g->nu_prev = nu;
	g->have_prev = true;
	double p[3], v[3];
	orbit_at(c, o, nu, p, v);
	/* the pull: close the distance in about the time a radian of the orbit takes, but no faster than the ship could
	 * stop in (the braking curve of the autopilot's own steering) */
	double n = sqrt(c->mu / (o->a * o->a * o->a)), d[3] = {p[0] - c->r[0], p[1] - c->r[1], p[2] - c->r[2]};
	double cap = sqrt(2 * amax * bd), want = n * bd;
	double scale = bd > 0 ? fmin(want, cap) / bd : 0;
	double vc[3];
	for (int i = 0; i < 3; i++)
		vc[i] = v[i] + d[i] * scale;
	turn_toward(c, vc, out->uw);
	out->dist = bd;
	out->nu = nu;
	OrbitEl se;
	if (orbit_el_from_state(c, c->r, c->u, &se) && se.bound && se.e < 1)
	{
		out->have_orbit = true;
		OrbitErr er;
		orbit_plan_errors(pl, &se, &er);
		out->plane_deg = er.plane_deg;
		out->d_rp = er.d_rp;
		out->d_ra = er.d_ra;
		out->d_argpe = fabs(remainder(se.argpe - o->argpe, TWO_PI)) * 180 / M_PI;
		bool ellipse = o->e > 0.02 && se.e > 0.005;
		out->done = er.plane_deg < GUIDE_PLANE_TOL_DEG && fabs(er.d_rp) < GUIDE_APSIS_TOL * o->rp &&
					fabs(er.d_ra) < GUIDE_APSIS_TOL * o->ra && (!ellipse || out->d_argpe < GUIDE_ARGPE_TOL_DEG);
	}
	return true;
}

/* The slingshot (see orbit_guide.h). The conic the ship should be on: its present speed (so its energy), periapsis rp, in the
 * plane of the plan that holds the ship's position (the plan's normal less its part along the position), turned the
 * way the plan goes. The periapsis lies ahead of the ship by the angle the conic puts it at. Gives the velocity that
 * puts it on that conic here (the same speed, so only a turn), and the radial speed fraction for the periapsis test. */
bool orbit_flyby_step(const OrbitCtx *c, const OrbitPlan *pl, double rp, OrbitFlyby *out)
{
	memset(out, 0, sizeof *out);
	if (!c->ok || !pl->valid || !(c->mu > 0))
		return false;
	double r = sqrt(dot3(c->r, c->r)), v2 = dot3(c->u, c->u), v = sqrt(v2);
	if (r < rp || v <= 0)
		return false;
	double rh[3], uh[3];
	for (int i = 0; i < 3; i++)
		rh[i] = c->r[i] / r, uh[i] = c->u[i] / v;
	out->vr_frac = dot3(rh, uh);
	double eps = 0.5 * v2 - c->mu / r;
	double e = 1 + 2 * eps * rp / c->mu;
	if (e <= 0.02)
		return false;
	double p = rp * (1 + e), cn = (p / r - 1) / e;
	if (cn > 1 || cn < -1)
		return false;
	double nu = -acos(cn); /* inbound */
	double hn[3], hl = 0, d = dot3(pl->el.h, rh);
	for (int i = 0; i < 3; i++)
		hn[i] = pl->el.h[i] - d * rh[i], hl += hn[i] * hn[i];
	hl = sqrt(hl);
	if (hl < 0.05) /* coming in over the plan's pole: keep to the plane the ship is flying in */
	{
		double x[3] = {rh[1] * uh[2] - rh[2] * uh[1], rh[2] * uh[0] - rh[0] * uh[2], rh[0] * uh[1] - rh[1] * uh[0]};
		double xl = sqrt(dot3(x, x));
		if (xl < 1e-9)
			return false;
		for (int i = 0; i < 3; i++)
			hn[i] = x[i] / xl;
		if (dot3(hn, pl->el.h) < 0)
			for (int i = 0; i < 3; i++)
				hn[i] = -hn[i];
	}
	else
		for (int i = 0; i < 3; i++)
			hn[i] /= hl;
	double th[3] = {hn[1] * rh[2] - hn[2] * rh[1], hn[2] * rh[0] - hn[0] * rh[2], hn[0] * rh[1] - hn[1] * rh[0]};
	double hm = sqrt(c->mu * p), vr = c->mu * e * sin(nu) / hm, vt = hm / r;
	double vc[3];
	for (int i = 0; i < 3; i++)
		vc[i] = vr * rh[i] + vt * th[i];
	turn_toward(c, vc, out->uw);
	for (int i = 0; i < 3; i++)
		out->pe[i] = cos(-nu) * rh[i] + sin(-nu) * th[i];
	out->e = e;
	out->turn_deg = acos(fmax(-1, fmin(1, dot3(uh, vc) / sqrt(dot3(vc, vc))))) * 180 / M_PI;
	return true;
}

/* The speed that makes the orbit through the ship's position (at periapsis) the planned apsides: vis-viva with the
 * semi-major axis (rp + ra) / 2 of the plan */
double orbit_brake_speed(const OrbitCtx *c, const OrbitPlan *pl)
{
	double r = sqrt(dot3(c->r, c->r)), a = 0.5 * (pl->el.rp + pl->el.ra);
	double v2 = c->mu * (2 / r - 1 / a);
	return v2 > 0 ? sqrt(v2) : 0;
}

/* ---- driving the ship ---- */

static double game_clock(void)
{
	return (double)(unsigned int)rdlong(GAME_A6 + A6_CLOCK_HI) * 4294967296.0 + (unsigned int)rdlong(GAME_A6 + A6_CLOCK_LO);
}

/* a ms^-2 in pu per step^2: a km/s^2 is 1/PU_KM pu/s^2, a step is REF_DT/CLOCK_HZ seconds */
static double accel_pu(double ms2)
{
	double sec = REF_DT / CLOCK_HZ;
	return ms2 * 1e-3 / PU_KM * sec * sec;
}

/* one look of the guidance, and the ship's velocity changed towards the wanted one by at most `accel_ms2` over the game
 * time since `*last_clock` (which this updates). No thrust of the ship's own: the engines are off. */
static bool drive(const OrbitCtx *c, OrbitGuide *g, double *last_clock, double accel_ms2, GuideOut *go, double *du)
{
	const OrbitPlan *pl = orbit_plan_current();
	double clock = game_clock(), steps = (clock - *last_clock) / REF_DT;
	*last_clock = clock;
	*du = 0;
	double amax = accel_pu(accel_ms2);
	if (!orbit_guide_step(c, pl, g, amax, go))
		return false;
	if (!go->done && steps > 0)
	{
		double dv[3] = {go->uw[0] - c->u[0], go->uw[1] - c->u[1], go->uw[2] - c->u[2]}, l = sqrt(dot3(dv, dv)), lim = amax * steps;
		double f = l > lim ? lim / l : 1;
		*du = l * f;
		unsigned int ship = game_player_ship();
		for (int i = 0; i < 3; i++)
		{
			double v = (c->u[i] + dv[i] * f) / c->k, fl = floor(v);
			wrlong(ship + OBJ_VELOCITY + 4 * i, (int)fl);
			wrword(ship + OBJ_VEL_FRAC + 2 * i, (int)((v - fl) * 65536.0));
		}
	}
	return true;
}

static void engines_off(bool off)
{
	uint8_t b = (uint8_t)rdbyte(GAME_A6 + 705);
	wrbyte(GAME_A6 + 705, off ? (b | 1) : (b & ~1));
}

static void log_line(const OrbitCtx *c, const GuideOut *go, double du, double clock)
{
	OrbitEl se;
	double alt = (sqrt(dot3(c->r, c->r)) - c->radius) * PU_KM;
	if (orbit_el_from_state(c, c->r, c->u, &se))
		printf("GUIDE t=%.1f alt=%.1f pe=%.1f ap=%.1f e=%.4f inc=%.2f | to plan: dist %.1f km plane %.2f deg rp %+.1f ra %+.1f argpe %.1f | du %.4f km/s%s\n",
			clock / CLOCK_HZ, alt, (se.rp - c->radius) * PU_KM, se.bound ? (se.ra - c->radius) * PU_KM : -1.0, se.e,
			se.inc * 180 / M_PI, go->dist * PU_KM, go->plane_deg, go->d_rp * PU_KM, go->d_ra * PU_KM, go->d_argpe,
			du * PU_KM * CLOCK_HZ / REF_DT, go->done ? " DONE" : "");
	else
		printf("GUIDE t=%.1f alt=%.1f no orbit yet | dist %.1f km | du %.4f km/s\n", clock / CLOCK_HZ, alt, go->dist * PU_KM,
			du * PU_KM * CLOCK_HZ / REF_DT);
	fflush(stdout);
}

/* ---- the test driver (script commands fly, flystop) ---- */

static struct
{
	bool on;
	double accel_ms2;
	int every, countdown;
	double last_clock;
	bool done_said;
	OrbitGuide g;
} fly;

void orbit_fly_start(double accel_ms2, int every)
{
	memset(&fly, 0, sizeof fly);
	if (!orbit_plan_current()->valid)
	{
		printf("script: fly: no planned orbit (use plan first)\n");
		return;
	}
	fly.on = true;
	fly.accel_ms2 = accel_ms2 > 0 ? accel_ms2 : 10;
	fly.every = every > 0 ? every : 50;
	fly.last_clock = game_clock();
	orbit_guide_reset(&fly.g);
	engines_off(true);
	printf("script: fly: guiding at up to %.1f m/s^2\n", fly.accel_ms2);
}

void orbit_fly_stop(void)
{
	if (fly.on)
		printf("script: fly: stopped\n");
	fly.on = false;
}

static void fly_poll(const OrbitCtx *have)
{
	const OrbitPlan *pl = orbit_plan_current();
	if (!have || !pl->valid || pl->body_slot != have->body_slot)
	{
		printf("script: fly: the plan or the body is gone, stopped\n");
		fly.on = false;
		return;
	}
	GuideOut go;
	double du;
	if (!drive(have, &fly.g, &fly.last_clock, fly.accel_ms2, &go, &du))
		return;
	if (go.done && !fly.done_said)
		printf("script: fly: in the planned orbit\n"), fly.done_said = true;
	else if (!go.done)
		fly.done_said = false;
	if (fly.countdown-- <= 0)
	{
		fly.countdown = fly.every - 1;
		log_line(have, &go, du, game_clock());
	}
}

/* ---- the orbit autopilot (the ORBIT page's ENGAGE) ---- */

#define AP_ACCEL_MS2 200.0 /* what the guidance counts on: the game's own autopilot arrives at 100 km/s from 50,000 km out,
							* which is a braking of about 110 m/s^2, so this is more than the ships do */
#define AP_FLYBY_ACCEL_MS2 50000.0 /* the flyby's turning and braking: the velocity is changed directly, so there is no thrust limit to
							* respect, and what the arrival needs is set by the speed it comes in at (about 1,100 km/s at 130,000 km at
							* Kerbin, from 10 AU: 5-10 km/s^2; at 200 m/s^2 it dived in with e still 26,000). The braking rate is the one
							* the distance needs (with margin), so slow arrivals are not braked harder because of this cap. */
#define AP_DONE_LOOKS 3   /* looks in a row in the planned orbit before it is called reached */
#define A6_FLIGHT_STATE 704
#define A6_TARGET       708
#define A6_SPEED_NOW    688
#define A6_SPEED_SET    848
#define OBJ_AI_STATE    211
#define OBJ_AI_DEST     212
#define OBJ_EQUIP       158

enum { AP_OFF, AP_APPROACH, AP_FLYBY, AP_GUIDE };
static struct
{
	int st;
	uint8_t eng_flag; /* the engines-off bit as it was when the autopilot started: the pilot's, restored at the end */
	int request; /* 1 engage, -1 cancel (from the page: it asks, the next update does) */
	double last_clock;
	int done_looks, log_countdown;
	double cap_clock, cap_km;
	double cap_frac; /* the fraction of the way to the body a frame may cover */
	bool thrusting;  /* the last update changed the velocity (more than a trim): the time cap allows for a frame of thrust */
	/* burn mode (the guidance on a closed orbit): one-update burns at the apsides and nodes, coasting in between */
	bool burn_mode;               /* the last update was in burn mode (the distance cap is off then: the event cap rules) */
	bool bm_have_prev;            /* the values below are from the last update in burn mode */
	double bm_nu, bm_ang, bm_side; /* the ship's true anomaly, its angle from the plan's periapsis, its side of the plan's plane */
	int bm_burns;
	OrbitGuide g;
	char status[24];
} ap;

static void ap_status(const char *s)
{
	if (!strcmp(ap.status, s))
		return;
	snprintf(ap.status, sizeof ap.status, "%s", s);
	if (getenv("GLF_MAP_LOG"))
		printf("script: ap: %s\n", s);
}

/* the game's own autopilot off, the ship under manual control as it is */
static void game_autopilot_off(unsigned int ship)
{
	if ((uint8_t)rdbyte(GAME_A6 + A6_FLIGHT_STATE) == 4)
	{
		wrlong(GAME_A6 + A6_SPEED_SET, rdlong(GAME_A6 + A6_SPEED_NOW));
		wrbyte(GAME_A6 + A6_FLIGHT_STATE, 0);
		wrbyte(ship + OBJ_AI_STATE, 0);
	}
}

static void ap_stop(const char *why)
{
	unsigned int ship = game_player_ship();
	if (ship && ap.st != AP_OFF)
	{
		game_autopilot_off(ship);
		/* The speed governor must not be left with the set speed it had (0 after the game's autopilot): with the engines-off flag
		 * clear it brakes the ship to a stop at full thrust, which dropped the ship out of orbit twice. The set speed is the
		 * present one, and after reaching the orbit the flag stays set (no governor, the ship coasts as in manual flight);
		 * else it goes back to the pilot's. */
		wrlong(GAME_A6 + A6_SPEED_SET, rdlong(GAME_A6 + A6_SPEED_NOW));
		engines_off(!strcmp(why, "IN ORBIT") || (ap.eng_flag & 1));
	}
	ap.st = AP_OFF;
	ap.burn_mode = false;
	ap.bm_have_prev = false;
	ap_status(why);
}

/* One frame must not cover more than a tenth of the way to the body, at the speed it is closing on it and the speed a
 * frame of thrust could add: the game's own autopilot (arriving, braking) and the guidance decide once a frame, which at
 * 10000x is 200 game seconds, enough to cross the last 20,000 km and hit the body. Same as the time guard does for ships. */
static void ap_time_cap(int body)
{
	double km;
	if (ap.burn_mode)
	{
		ap.cap_clock = 0; /* the event cap rules (burn_poll); a fresh closing speed when it is left */
		return;
	}
	if (!cheat_body_distance_km(body, &km))
		return;
	double clock = game_clock(), dt = (clock - ap.cap_clock) / CLOCK_HZ;
	if (ap.cap_clock > 0 && dt > 0 && dt < 1e5)
	{
		double closing = (ap.cap_km - km) / dt; /* km per game second */
		/* km/s^2: more than the ships brake at (about 0.11). Only while thrusting: coasting (a closed orbit, a trim of a few
		 * m/s) the speed cannot change within a frame, so the closing speed alone decides and 10000x is allowed far out. */
		const double accel = ap.thrusting ? 0.2 : 0.0;
		double v = (closing > 0 ? closing : 0) + accel * dt;
		time_guard_cap(ap.cap_frac * km / v);
	}
	ap.cap_clock = clock;
	ap.cap_km = km;
}

static void ap_start(const OrbitCtx *have)
{
	const OrbitPlan *pl = orbit_plan_current();
	unsigned int ship = game_player_ship();
	if (!ship || !pl->valid)
	{
		ap_status("NO ORBIT");
		return;
	}
	ap.eng_flag = (uint8_t)rdbyte(GAME_A6 + 705);
	memset(&ap.g, 0, sizeof ap.g);
	ap.cap_clock = 0;
	ap.cap_frac = 0.1;
	ap.thrusting = true;
	ap.burn_mode = false;
	ap.bm_have_prev = false;
	ap.bm_burns = 0;
	orbit_guide_reset(&ap.g);
	ap.last_clock = game_clock();
	ap.done_looks = 0;
	if (have && have->body_slot == pl->body_slot)
	{
		/* already in the body's frame: straight to the guidance */
		game_autopilot_off(ship);
		engines_off(true);
		ap.st = AP_GUIDE;
		ap_status("GUIDING");
		return;
	}
	/* otherwise the game's own autopilot takes the ship to the body, as F7 does: the destination is the target, the flight
	 * state 4, the AI state $20; it needs the Automatic Pilot and manual flight */
	if ((uint8_t)rdbyte(GAME_A6 + A6_TARGET) != (unsigned)pl->body_slot)
	{
		ap_status("TARGET IT");
		return;
	}
	if (!(rdlong(ship + OBJ_EQUIP) & 0x20))
	{
		ap_status("NO AUTOPILOT");
		return;
	}
	if ((uint8_t)rdbyte(GAME_A6 + A6_FLIGHT_STATE) != 0)
	{
		ap_status("NOT IN FLIGHT");
		return;
	}
	engines_off(false);
	wrbyte(ship + OBJ_AI_DEST, rdbyte(GAME_A6 + A6_TARGET));
	wrbyte(GAME_A6 + A6_FLIGHT_STATE, 4);
	wrbyte(ship + OBJ_AI_STATE, 0x20);
	ap.st = AP_APPROACH;
	ap_status("APPROACH");
}

static void set_velocity(unsigned int ship, const OrbitCtx *have, const double u[3])
{
	for (int i = 0; i < 3; i++)
	{
		double v = u[i] / have->k, fl = floor(v);
		wrlong(ship + OBJ_VELOCITY + 4 * i, (int)fl);
		wrword(ship + OBJ_VEL_FRAC + 2 * i, (int)((v - fl) * 65536.0));
	}
}

/* in the body's frame and not yet in orbit: a slingshot if the ship is coming in, else the guidance */
static void enter_flyby(const OrbitCtx *have)
{
	const OrbitPlan *pl = orbit_plan_current();
	OrbitFlyby fb;
	if (orbit_flyby_step(have, pl, pl->el.rp, &fb) && fb.vr_frac < -0.05)
	{
		ap.cap_frac = 0.03; /* finer frames: the burn is at the periapsis, and a frame there is a few degrees of the orbit */
		ap.st = AP_FLYBY;
		ap_status("FLYBY");
	}
	else
	{
		ap.st = AP_GUIDE;
		ap_status("GUIDING");
	}
}

/* The flyby: each update the velocity is turned (at most amax * time) so the ship's conic has its periapsis at the planned
 * height, on the side the plan's direction gives; at the periapsis the speed is brought down, along the ship's motion, to
 * what the planned apsides need (in one update: the burn has to be at the periapsis, and at any thrust the ships have it
 * would take a quarter of an orbit). Then it is a closed orbit and the guidance does the fine tuning. */
static void apsis_velocity(const OrbitCtx *c, const double rh[3], double r, const double hh[3], double R, double u[3]);

static void flyby_poll(const OrbitCtx *have, const OrbitPlan *pl)
{
	OrbitFlyby fb;
	unsigned int ship = game_player_ship();
	double clock = game_clock(), steps = (clock - ap.last_clock) / REF_DT;
	ap.last_clock = clock;
	/* aimed a little under the planned periapsis (so the step stays valid down to it), by 2% of the ALTITUDE: 2% of the radius
	 * from the centre was 610 km at Aster and 120 km at Kerbin, under the surface for a low plan, and the ship flew in */
	if (!orbit_flyby_step(have, pl, have->radius + 0.98 * (pl->el.rp - have->radius), &fb))
	{
		ap.st = AP_GUIDE; /* no flyby possible any more (too slow, or inside the periapsis) */
		ap.cap_frac = 0.1;
		ap_status("GUIDING");
		return;
	}
	if (fb.vr_frac >= -0.05 && sqrt(dot3(have->r, have->r)) > 1.3 * pl->el.rp)
	{
		/* turned round far out (an apoapsis, not the periapsis): the guidance has it from here */
		ap.st = AP_GUIDE;
		ap.cap_frac = 0.1;
		orbit_guide_reset(&ap.g);
		ap_status("GUIDING");
		return;
	}
	if (fb.vr_frac >= -0.05)
	{
		/* The burn makes this point an apsis (the velocity horizontal) and the opposite apsis the plan's one this point is not
		 * nearer, as burn mode does. Scaling the whole velocity down kept the inward part: the test fires with up to 5% of the
		 * speed still inward (2 km/s at Aster), and the orbit after it had its periapsis 400 km under the surface. */
		double vs = sqrt(dot3(have->u, have->u)), vt = 0, u[3];
		double r = sqrt(dot3(have->r, have->r)), rh[3] = {have->r[0] / r, have->r[1] / r, have->r[2] / r};
		double hv[3] = {have->r[1] * have->u[2] - have->r[2] * have->u[1], have->r[2] * have->u[0] - have->r[0] * have->u[2],
						have->r[0] * have->u[1] - have->r[1] * have->u[0]};
		double hl = sqrt(dot3(hv, hv));
		if (hl > 0)
		{
			for (int i = 0; i < 3; i++)
				hv[i] /= hl;
			double R = fabs(r - pl->el.ra) < fabs(r - pl->el.rp) ? pl->el.rp : pl->el.ra;
			apsis_velocity(have, rh, r, hv, R, u);
			vt = sqrt(dot3(u, u));
			set_velocity(ship, have, u);
		}
		if (getenv("GLF_MAP_LOG"))
		{
			double kms = PU_KM * CLOCK_HZ / REF_DT;
			printf("FLYBY burn at alt %.1f km: %.3f -> %.3f km/s\n", (sqrt(dot3(have->r, have->r)) - have->radius) * PU_KM, vs * kms, vt * kms);
			fflush(stdout);
		}
		ap.cap_frac = 0.1;
		orbit_guide_reset(&ap.g);
		ap.st = AP_GUIDE;
		ap_status("GUIDING");
		return;
	}
	double amax = accel_pu(AP_FLYBY_ACCEL_MS2), dv[3] = {fb.uw[0] - have->u[0], fb.uw[1] - have->u[1], fb.uw[2] - have->u[2]};
	double l = sqrt(dot3(dv, dv)), lim = amax * steps, f = l > lim ? lim / l : 1;
	double brake = 0;
	if (steps > 0)
	{
		double u[3];
		for (int i = 0; i < 3; i++)
			u[i] = have->u[i] + dv[i] * f;
		/* The way a pilot does it (the pathlog of a manual approach to Aster): not one burn at the periapsis but a steady
		 * braking along the motion (the nose stays prograde) over the whole inbound leg, at the rate that has the speed at
		 * the periapsis the planned orbit's. The speed the ship would have there if it coasted, against the speed it
		 * needs, shed over the time left to get there; with some margin so it is done before. What is left at the
		 * periapsis the burn below takes. */
		/* Not down to the planned orbit's energy yet: a ship with that energy cannot be farther out than the orbit is (for a
		 * circular plan it would turn round at its apoapsis, short of the periapsis). The target is the ellipse with the
		 * planned periapsis and an apoapsis 30% above where the ship is, which tightens as it falls in (the pilot's
		 * capture orbit, apoapsis 22,000 km, was 1.6 times the radius at the end of the burn) */
		double r = sqrt(dot3(have->r, have->r)), v = sqrt(dot3(u, u)), rp = pl->el.rp;
		double ra_cap = fmax(pl->el.ra, 1.3 * r), a = 0.5 * (rp + ra_cap);
		double vfree2 = v * v + 2 * have->mu * (1 / rp - 1 / r), vp2 = have->mu * (2 / rp - 1 / a);
		if (r > rp && vfree2 > 0 && vp2 > 0 && v > 0)
		{
			double vfree = sqrt(vfree2), need = vfree - sqrt(vp2), tleft = (r - rp) / (0.5 * (v + vfree));
			if (need > 0 && tleft > 0)
			{
				double areq = need / tleft * 1.3, db = areq > amax ? amax : areq;
				db *= steps; /* tleft is in steps, as a velocity is a distance per step */
				if (db > need)
					db = need;
				if (db > v)
					db = v;
				for (int i = 0; i < 3; i++)
					u[i] *= 1 - db / v;
				brake = db;
			}
		}
		set_velocity(ship, have, u);
	}
	if (getenv("GLF_MAP_LOG") && ap.log_countdown-- <= 0)
	{
		ap.log_countdown = 12;
		printf("FLYBY t=%.1f alt=%.0f km e=%.3f turn %.2f deg du %.3f km/s brake %.3f km/s\n", clock / CLOCK_HZ,
			(sqrt(dot3(have->r, have->r)) - have->radius) * PU_KM, fb.e, fb.turn_deg, l * f * PU_KM * CLOCK_HZ / REF_DT,
			brake * PU_KM * CLOCK_HZ / REF_DT);
		fflush(stdout);
	}
}


/* ---- burn mode ----
 * On a closed orbit that clears the planet, the guidance does what a pilot does: coast, and make each correction in one
 * update at the point of the orbit where it changes only what it is meant to:
 *  - plane: where the ship crosses the plan's plane (a node), the velocity is turned about the radius into it, speed kept;
 *  - apsides: at an apsis the speed (made horizontal) is set so the opposite apsis is the plan's. For a circular plan the
 *    apsides are the ship's own (any point of a near-circular ship orbit is one); for an ellipse they are the plan's
 *    periapsis and apoapsis directions, which puts the periapsis where the plan has it too.
 * The other apsis is always one of the plan's, so no burn lowers the orbit below the plan. Burns are skipped when what
 * they would fix is already within the tolerances (a deadband), and events are found as crossings between two updates,
 * so a burn is late by at most a frame; the time acceleration is stepped down ahead of the next event (Kepler's equation)
 * so a frame there is a small part of the orbit, and is free in between. */
#define BM_FRAME_FRAC  0.1   /* a frame at most this part of the time to the next event ... */
#define BM_MIN_FRAME   360.0 /* ... but never below the period / this (a degree of the orbit): the burn's own lateness */
#define BM_SHIP_CIRC_E 0.002 /* a ship orbit rounder than this: every point is an apsis */

static double wrap_pi(double a)
{
	return remainder(a, TWO_PI);
}

/* the update moved the angle across `at` (either way round), not by going the long way */
static bool crossed(double prev, double cur, double at)
{
	double d1 = wrap_pi(prev - at), d2 = wrap_pi(cur - at);
	return ((d1 < 0 && d2 >= 0) || (d1 > 0 && d2 <= 0)) && fabs(d2 - d1) < M_PI;
}

/* game seconds for the ship to go from true anomaly nu0 to nu1 on its orbit (forward) */
static double time_to_nu(const OrbitCtx *c, const OrbitEl *se, double nu0, double nu1)
{
	double k = sqrt((1 - se->e) / (1 + se->e));
	double e0 = 2 * atan(k * tan(nu0 / 2)), e1 = 2 * atan(k * tan(nu1 / 2));
	double m0 = e0 - se->e * sin(e0), m1 = e1 - se->e * sin(e1);
	double dm = fmod(m1 - m0 + 2 * TWO_PI, TWO_PI), n = sqrt(c->mu / (se->a * se->a * se->a)); /* rad / step */
	return dm / n * REF_DT / CLOCK_HZ;
}

/* the ship's true anomaly where its orbit points along the unit vector d (projected into its plane) */
static double nu_of_dir(const OrbitEl *se, const double d[3])
{
	return atan2(dot3(d, se->q), dot3(d, se->pe));
}

/* horizontal velocity at r (pu from the centre, direction rh) in the plane of normal hh, for the orbit whose apsides are r and R */
static void apsis_velocity(const OrbitCtx *c, const double rh[3], double r, const double hh[3], double R, double u[3])
{
	double a = 0.5 * (r + R), v = sqrt(fmax(0, c->mu * (2 / r - 1 / a)));
	double t[3] = {hh[1] * rh[2] - hh[2] * rh[1], hh[2] * rh[0] - hh[0] * rh[2], hh[0] * rh[1] - hh[1] * rh[0]};
	double tl = sqrt(dot3(t, t));
	for (int i = 0; i < 3; i++)
		u[i] = t[i] / tl * v;
}

static void bm_log(const char *what, const OrbitCtx *c, double dv)
{
	if (getenv("GLF_MAP_LOG"))
	{
		OrbitEl se;
		if (orbit_el_from_state(c, c->r, c->u, &se))
			printf("BURN %d %s alt %.1f km dv %.4f km/s -> pe %.1f ap %.1f inc %.2f\n", ap.bm_burns, what,
				(sqrt(dot3(c->r, c->r)) - c->radius) * PU_KM, dv * PU_KM * CLOCK_HZ / REF_DT, (se.rp - c->radius) * PU_KM,
				se.bound ? (se.ra - c->radius) * PU_KM : -1.0, se.inc * 180 / M_PI);
		fflush(stdout);
	}
}

/* sets the ship's velocity to u, and the copy in c (so a second burn in the same update starts from it) */
static void bm_apply(OrbitCtx *c, const double u[3], const char *what)
{
	double d[3] = {u[0] - c->u[0], u[1] - c->u[1], u[2] - c->u[2]};
	set_velocity(game_player_ship(), c, u);
	memcpy(c->u, u, sizeof c->u);
	ap.bm_burns++;
	bm_log(what, c, sqrt(dot3(d, d)));
}

/* One update of burn mode. False if the ship's orbit is not one for it (open, or a periapsis too low): the continuous
 * guidance flies it then. */
static bool burn_poll(const OrbitCtx *have, const OrbitPlan *pl)
{
	const OrbitEl *o = &pl->el;
	OrbitCtx c = *have;
	OrbitEl se;
	if (!orbit_el_from_state(&c, c.r, c.u, &se) || !se.bound || se.e >= 1 || se.rp < c.radius + 0.5 * (o->rp - c.radius) ||
		se.ra > 3 * o->ra + c.radius)
	{
		ap.burn_mode = false;
		ap.bm_have_prev = false;
		return false;
	}
	if (!ap.burn_mode)
	{
		ap.burn_mode = true;
		ap.bm_have_prev = false;
		ap.thrusting = false;
	}
	bool ellipse = o->e > 0.02;
	double r = sqrt(dot3(c.r, c.r)), rh[3] = {c.r[0] / r, c.r[1] / r, c.r[2] / r};
	double nu = se.nu, ang = atan2(dot3(rh, o->q), dot3(rh, o->pe)), side = dot3(rh, o->h);
	OrbitErr er;
	orbit_plan_errors(pl, &se, &er);
	double d_argpe = fabs(wrap_pi(se.argpe - o->argpe)) * 180 / M_PI;
	bool plane_ok = er.plane_deg < GUIDE_PLANE_TOL_DEG;
	bool rp_ok = fabs(er.d_rp) < GUIDE_APSIS_TOL * o->rp, ra_ok = fabs(er.d_ra) < GUIDE_APSIS_TOL * o->ra;
	bool argpe_ok = !ellipse || se.e <= 0.005 || d_argpe < GUIDE_ARGPE_TOL_DEG;

	if (ap.bm_have_prev)
	{
		/* plane, at a node */
		if (!plane_ok && ((ap.bm_side < 0) != (side < 0)))
		{
			double vr = dot3(c.u, rh), uh[3], u[3];
			for (int i = 0; i < 3; i++)
				uh[i] = c.u[i] - vr * rh[i];
			double vh = sqrt(dot3(uh, uh));
			double t[3] = {o->h[1] * rh[2] - o->h[2] * rh[1], o->h[2] * rh[0] - o->h[0] * rh[2], o->h[0] * rh[1] - o->h[1] * rh[0]};
			double tl = sqrt(dot3(t, t));
			if (tl > 1e-9)
			{
				for (int i = 0; i < 3; i++)
					u[i] = vr * rh[i] + t[i] / tl * vh;
				bm_apply(&c, u, "PLANE");
			}
		}
		/* apsides */
		double hv[3] = {c.r[1] * c.u[2] - c.r[2] * c.u[1], c.r[2] * c.u[0] - c.r[0] * c.u[2], c.r[0] * c.u[1] - c.r[1] * c.u[0]};
		double hl = sqrt(dot3(hv, hv));
		for (int i = 0; i < 3; i++)
			hv[i] /= hl;
		double u[3];
		if (ellipse)
		{
			/* at the plan's periapsis direction the opposite apsis is its apoapsis, and the other way round */
			bool at_pe = crossed(ap.bm_ang, ang, 0), at_ap = crossed(ap.bm_ang, ang, M_PI);
			if ((at_pe || at_ap) && !(rp_ok && ra_ok && argpe_ok))
			{
				double R = at_pe ? o->ra : o->rp;
				/* the deadband: the far apsis is already right and the periapsis already points the plan's way */
				double far = (at_pe == (r < 0.5 * (se.rp + se.ra))) ? se.ra : se.rp;
				if (!(fabs(far - R) < GUIDE_APSIS_TOL * R && argpe_ok))
				{
					apsis_velocity(&c, rh, r, hv, R, u);
					bm_apply(&c, u, at_pe ? "AT PLAN PE" : "AT PLAN AP");
				}
			}
		}
		else
		{
			bool circ = se.e < BM_SHIP_CIRC_E;
			bool at_pe = crossed(ap.bm_nu, nu, 0), at_ap = crossed(ap.bm_nu, nu, M_PI);
			if ((circ || at_pe || at_ap) && !(rp_ok && ra_ok))
			{
				/* this point stays an apsis; the opposite one goes to whichever of the plan's this point is not nearer */
				double R = fabs(r - o->ra) < fabs(r - o->rp) ? o->rp : o->ra;
				double far = circ ? r : at_pe ? se.ra : se.rp;
				if (!(fabs(far - R) < GUIDE_APSIS_TOL * R))
				{
					apsis_velocity(&c, rh, r, hv, R, u);
					bm_apply(&c, u, circ ? "CIRC" : at_pe ? "AT PE" : "AT AP");
				}
			}
		}
	}
	orbit_el_from_state(&c, c.r, c.u, &se); /* after any burn */
	ap.bm_have_prev = true;
	ap.bm_nu = se.nu;
	ap.bm_ang = atan2(dot3(rh, o->q), dot3(rh, o->pe));
	ap.bm_side = side;

	/* done, as the guidance has it */
	orbit_plan_errors(pl, &se, &er);
	d_argpe = fabs(wrap_pi(se.argpe - o->argpe)) * 180 / M_PI;
	plane_ok = er.plane_deg < GUIDE_PLANE_TOL_DEG;
	rp_ok = fabs(er.d_rp) < GUIDE_APSIS_TOL * o->rp;
	ra_ok = fabs(er.d_ra) < GUIDE_APSIS_TOL * o->ra;
	argpe_ok = !ellipse || se.e <= 0.005 || d_argpe < GUIDE_ARGPE_TOL_DEG;
	bool done = se.bound && plane_ok && rp_ok && ra_ok && argpe_ok;
	ap.done_looks = done ? ap.done_looks + 1 : 0;
	if (done)
	{
		if (ap.done_looks >= AP_DONE_LOOKS)
			ap_stop("IN ORBIT");
		return true;
	}
	ap_status("COASTING");

	/* the time to the next event that has something to fix, and the frame cap from it */
	double t_next = INFINITY;
	if (!plane_ok)
	{
		double nl[3] = {se.h[1] * o->h[2] - se.h[2] * o->h[1], se.h[2] * o->h[0] - se.h[0] * o->h[2], se.h[0] * o->h[1] - se.h[1] * o->h[0]};
		double l = sqrt(dot3(nl, nl));
		if (l > 1e-9)
			for (int s2 = -1; s2 <= 1; s2 += 2)
			{
				double d[3] = {s2 * nl[0] / l, s2 * nl[1] / l, s2 * nl[2] / l};
				t_next = fmin(t_next, time_to_nu(&c, &se, se.nu, nu_of_dir(&se, d)));
			}
	}
	if (!(rp_ok && ra_ok && argpe_ok))
	{
		if (ellipse)
			for (int s2 = -1; s2 <= 1; s2 += 2)
			{
				double d[3] = {s2 * o->pe[0], s2 * o->pe[1], s2 * o->pe[2]};
				t_next = fmin(t_next, time_to_nu(&c, &se, se.nu, nu_of_dir(&se, d)));
			}
		else if (se.e >= BM_SHIP_CIRC_E)
			t_next = fmin(time_to_nu(&c, &se, se.nu, 0), time_to_nu(&c, &se, se.nu, M_PI));
		else
			t_next = 0; /* a burn on the next update */
	}
	double period = TWO_PI / sqrt(c.mu / (se.a * se.a * se.a)) * REF_DT / CLOCK_HZ;
	if (t_next < INFINITY)
		time_guard_cap(fmax(BM_FRAME_FRAC * t_next, period / BM_MIN_FRAME));
	return true;
}

static void ap_poll(const OrbitCtx *have)
{
	const OrbitPlan *pl = orbit_plan_current();
	unsigned int ship = game_player_ship();
	if (ap.request == -1)
	{
		ap.request = 0;
		if (ap.st != AP_OFF)
			ap_stop("");
	}
	else if (ap.request == 1)
	{
		ap.request = 0;
		if (ap.st == AP_OFF)
			ap_start(have);
	}
	if (ap.st == AP_OFF || !ship)
		return;
	if (!pl->valid)
	{
		ap_stop("");
		return;
	}
	if (orbit_ap_controls_locked())
	{
		for (int i = 0; i < 3; i++)
		{
			wrword(ship + 140 + 2 * i, 0); /* thrust */
			wrword(ship + 134 + 2 * i, 0); /* rotation rates */
		}
	}
	ap_time_cap(pl->body_slot);
	if (ap.st == AP_APPROACH)
	{
		/* the game's autopilot flies to the body and ends on arriving in its vicinity (the body is the ship's frame, within
		 * about 68,700 km) with the ship still at speed (it brakes afterwards, by the set speed 0, too late at time
		 * acceleration: it flew into the body). The guidance has it from there, and does the braking. */
		if ((uint8_t)rdbyte(GAME_A6 + A6_FLIGHT_STATE) != 4)
		{
			if (have && have->body_slot == pl->body_slot)
			{
				engines_off(true);
				orbit_guide_reset(&ap.g);
				ap.last_clock = game_clock();
				enter_flyby(have);
			}
			else
				ap_stop("CANCELLED"); /* the pilot or the game ended it (docked, hyperspace, arrived elsewhere...) */
		}
		return;
	}
	/* guiding */
	if (!have || have->body_slot != pl->body_slot)
	{
		ap_stop("LEFT THE BODY");
		return;
	}
	if (ap.st == AP_FLYBY)
	{
		ap.thrusting = true; /* turning and braking every update */
		flyby_poll(have, pl);
		return;
	}
	if (burn_poll(have, pl))
	{
		ap.last_clock = game_clock(); /* the continuous guidance's next look counts from here */
		return;
	}
	ap_status("GUIDING");
	GuideOut go;
	double du, prev_clock = ap.last_clock;
	if (!drive(have, &ap.g, &ap.last_clock, AP_ACCEL_MS2, &go, &du))
		return;
	{
		/* thrusting: the velocity change this update, as an acceleration, above 1% of what the guidance counts on (2 m/s^2) */
		double dt = (ap.last_clock - prev_clock) / CLOCK_HZ, a_ms2 = dt > 0 ? du * PU_KM * 1000.0 * CLOCK_HZ / REF_DT / dt : 0;
		ap.thrusting = a_ms2 > 0.01 * AP_ACCEL_MS2;
	}
	if (getenv("GLF_MAP_LOG") && ap.log_countdown-- <= 0)
	{
		ap.log_countdown = 24;
		log_line(have, &go, du, game_clock());
	}
	ap.done_looks = go.done ? ap.done_looks + 1 : 0;
	if (ap.done_looks >= AP_DONE_LOOKS)
		ap_stop("IN ORBIT");
}

void orbit_ap_request(bool on)
{
	ap.request = on ? 1 : -1;
}

/* The orbit autopilot flies the ship (flyby, guidance): the player's controls are skipped, as the game's own autopilot (flight
 * state 4) skips them. A turn of the nose or a burst of thrust would put the ship off its path (and into the planet). The
 * thrust and rotation words are cleared on engaging so nothing stale goes on. */
bool orbit_ap_controls_locked(void)
{
	return ap.st == AP_FLYBY || ap.st == AP_GUIDE;
}

bool orbit_ap_active(void)
{
	return ap.st != AP_OFF || ap.request == 1;
}

const char *orbit_ap_status(void)
{
	return ap.status;
}

void orbit_fly_poll(void)
{
	OrbitCtx c;
	const bool have = orbit_ctx_get(&c);
	if (have)
		orbit_plan_sync(&c);
	ap_poll(have ? &c : NULL);
	if (fly.on)
		fly_poll(have ? &c : NULL);
}
