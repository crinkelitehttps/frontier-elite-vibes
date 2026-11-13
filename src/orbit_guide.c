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
	OrbitGuide g;
	char status[24];
} ap;

static void ap_status(const char *s)
{
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
	ap_status(why);
}

/* One frame must not cover more than a tenth of the way to the body, at the speed it is closing on it and the speed a
 * frame of thrust could add: the game's own autopilot (arriving, braking) and the guidance decide once a frame, which at
 * 10000x is 200 game seconds, enough to cross the last 20,000 km and hit the body. Same as the time guard does for ships. */
static void ap_time_cap(int body)
{
	double km;
	if (!cheat_body_distance_km(body, &km))
		return;
	double clock = game_clock(), dt = (clock - ap.cap_clock) / CLOCK_HZ;
	if (ap.cap_clock > 0 && dt > 0 && dt < 1e5)
	{
		double closing = (ap.cap_km - km) / dt; /* km per game second */
		const double accel = 0.2;               /* km/s^2: more than the ships brake at (about 0.11) */
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
static void flyby_poll(const OrbitCtx *have, const OrbitPlan *pl)
{
	OrbitFlyby fb;
	unsigned int ship = game_player_ship();
	double clock = game_clock(), steps = (clock - ap.last_clock) / REF_DT;
	ap.last_clock = clock;
	if (!orbit_flyby_step(have, pl, pl->el.rp * 0.98, &fb))
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
		double vs = sqrt(dot3(have->u, have->u)), vt = orbit_brake_speed(have, pl), u[3];
		if (vt > 0 && vt < vs)
		{
			for (int i = 0; i < 3; i++)
				u[i] = have->u[i] / vs * vt;
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
		flyby_poll(have, pl);
		return;
	}
	GuideOut go;
	double du;
	if (!drive(have, &ap.g, &ap.last_clock, AP_ACCEL_MS2, &go, &du))
		return;
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
