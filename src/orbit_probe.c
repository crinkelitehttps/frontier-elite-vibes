/*
 * orbit_probe.c - test tools for the orbit-autopilot work (docs/orbit-autopilot.md).
 *
 *   circ        set the player's ship on a circular orbit at its current radius, in the plane it is
 *               already moving in (a plane through the centre and the velocity; if it is at rest, the
 *               plane of the frame's x-z axes), engines off
 *   orbitlog N K  log one ORBIT line every K host updates, N times
 *
 * The elements are worked out with the same constants as the HUD orbit (gl_hud.c orbit_calc) but always
 * for one reference step (dt 994, no sub-steps), since pe, ap and e do not depend on the step. If the
 * game's own gravity agrees with that mu, a circular orbit keeps its pe/ap/e for as long as it runs.
 */
#include "orbit_probe.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "cheats.h"
#include "game_state.h"
#include "orbit_plan.h"
#include "host.h"

#define A6_BIG_SPACE  14466
#define OBJ_POS_LO    44
#define OBJ_POS_HI    56
#define OBJ_VELOCITY  98
#define OBJ_VEL_FRAC  192
#define OBJ_GRAV_MANT 122
#define OBJ_RADIUS_LO 266
#define OBJ_RADIUS_HI 270
#define A6_TICK_DT    14446
#define PU_KM         4294.967296
#define REF_DT        994.0

static int left, every, countdown;

static unsigned int slot_object(int i)
{
	return (unsigned int)rdlong(GAME_A6 + A6_BIG_SPACE) + (unsigned int)(int)rdword(FE2_obj_offsets + 2 * (unsigned int)i);
}

typedef struct
{
	double r[3], u[3], rr, uu, mu, k, radius, e, rp, ra, energy, vr;
} State;

static int state_get(State *s)
{
	unsigned int ship = game_player_ship();
	if (!ship)
		return 0;
	int par = (unsigned char)rdbyte(ship + 68);
	if (!par)
		return 0;
	unsigned int po = slot_object(par);
	int mant = (short)rdword(po + OBJ_GRAV_MANT), exp = (short)rdword(po + OBJ_GRAV_MANT + 2);
	if (mant <= 0)
		return 0;
	s->k = (REF_DT / 128.0) / 17179869184.0;
	s->mu = (double)mant * ldexp(1.0, exp - 51) * REF_DT * s->k;
	for (int i = 0; i < 3; i++)
	{
		s->r[i] = (double)(int)rdlong(ship + OBJ_POS_HI + 4 * i) +
				  (double)(unsigned int)rdlong(ship + OBJ_POS_LO + 4 * i) / 4294967296.0;
		s->u[i] = s->k * ((double)(int)rdlong(ship + OBJ_VELOCITY + 4 * i) +
						  (double)(rdword(ship + OBJ_VEL_FRAC + 2 * i) & 0xffff) / 65536.0);
	}
	s->rr = sqrt(s->r[0] * s->r[0] + s->r[1] * s->r[1] + s->r[2] * s->r[2]);
	s->uu = s->u[0] * s->u[0] + s->u[1] * s->u[1] + s->u[2] * s->u[2];
	s->radius = (double)rdword(po + OBJ_RADIUS_HI) + (double)(unsigned int)rdlong(po + OBJ_RADIUS_LO) / 4294967296.0;
	double h[3] = {s->r[1] * s->u[2] - s->r[2] * s->u[1], s->r[2] * s->u[0] - s->r[0] * s->u[2],
				   s->r[0] * s->u[1] - s->r[1] * s->u[0]};
	double hh = h[0] * h[0] + h[1] * h[1] + h[2] * h[2];
	double ev[3] = {(s->u[1] * h[2] - s->u[2] * h[1]) / s->mu - s->r[0] / s->rr,
					(s->u[2] * h[0] - s->u[0] * h[2]) / s->mu - s->r[1] / s->rr,
					(s->u[0] * h[1] - s->u[1] * h[0]) / s->mu - s->r[2] / s->rr};
	s->e = sqrt(ev[0] * ev[0] + ev[1] * ev[1] + ev[2] * ev[2]);
	s->energy = s->uu / 2 - s->mu / s->rr;
	s->rp = hh / (s->mu * (1 + s->e));
	s->ra = s->energy < 0 ? -s->mu / (2 * s->energy) * (1 + s->e) : 0;
	s->vr = (s->r[0] * s->u[0] + s->r[1] * s->u[1] + s->r[2] * s->u[2]) / s->rr / s->k;
	return 1;
}

void orbit_probe_circ(void)
{
	State s;
	unsigned int ship = game_player_ship();
	if (!state_get(&s))
	{
		printf("script: circ: no ship or no gravity here\n");
		return;
	}
	double t[3];
	if (s.uu > 0)
	{
		double h[3] = {s.r[1] * s.u[2] - s.r[2] * s.u[1], s.r[2] * s.u[0] - s.r[0] * s.u[2],
					   s.r[0] * s.u[1] - s.r[1] * s.u[0]};
		if (h[0] * h[0] + h[1] * h[1] + h[2] * h[2] <= 0)
			goto at_rest;
		t[0] = h[1] * s.r[2] - h[2] * s.r[1];
		t[1] = h[2] * s.r[0] - h[0] * s.r[2];
		t[2] = h[0] * s.r[1] - h[1] * s.r[0];
	}
	else
	{
	at_rest:
		/* tangent in the x-z plane: y x r */
		t[0] = s.r[2];
		t[1] = 0;
		t[2] = -s.r[0];
	}
	double tl = sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]), sp = sqrt(s.mu / s.rr);
	for (int i = 0; i < 3; i++)
	{
		double v = sp * t[i] / tl / s.k, fl = floor(v);
		wrlong(ship + OBJ_VELOCITY + 4 * i, (int)fl);
		wrword(ship + OBJ_VEL_FRAC + 2 * i, (int)((v - fl) * 65536.0));
	}
	/* engines off, no thrust */
	wrbyte(GAME_A6 + 705, rdbyte(GAME_A6 + 705) | 1);
	wrlong(ship + 140, 0);
	wrword(ship + 144, 0);
	state_get(&s);
	printf("script: circ: r=%.3f km alt=%.3f km speed=%.3f raw units (circular) e=%.6f\n", s.rr * PU_KM,
		(s.rr - s.radius) * PU_KM, sqrt(s.uu) / s.k, s.e);
}

/* ---- pathlog: GLF_PATH_LOG=1 writes pathlog.csv, ten rows a second of real time, whatever the game is doing ----
 * For flying a route by hand and reading it afterwards (the orbit autopilot's reference runs). Everything is relative to the
 * target body (the selected target if it has gravity, else the frame body), in the system's axes (the game's body-axes
 * regime is turned back by the body's matrix), so a run is the same whichever frame the game had the ship in:
 *   pos, vel     ship from the body's centre (km), ship velocity (km/s)
 *   nose         the ship's forward axis (unit)
 *   thrust       the ship's thrust words (x y z, ship axes), rate = its rotation words, eng = engines-off flag
 *   n_tgt        angle nose -> direction to the body, n_vel angle nose -> velocity, n_pro angle nose -> prograde... (deg)
 *   pe ap e inc  the ship's orbit round the body when the ship is in its frame (km above the surface, degrees) */
static void pathlog_poll(void)
{
	static FILE *f;
	static int state; /* 0 not asked yet, 1 on, -1 off */
	static struct timespec last;
	static unsigned int shown_body;
	if (state < 0)
		return;
	if (!state)
	{
		state = getenv("GLF_PATH_LOG") ? 1 : -1;
		if (state < 0)
			return;
		f = fopen("pathlog.csv", "w");
		if (!f)
		{
			state = -1;
			return;
		}
		fprintf(f, "t_s,accel,body,frame,x_km,y_km,z_km,alt_km,vx_kms,vy_kms,vz_kms,speed_kms,vr_kms,vt_kms,"
				   "nose_x,nose_y,nose_z,thr_x,thr_y,thr_z,rate_x,rate_y,rate_z,eng_off,flight,speed_set,"
				   "nose_to_body_deg,nose_to_vel_deg,pe_km,ap_km,e,inc_deg\n");
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if ((now.tv_sec - last.tv_sec) * 1000 + (now.tv_nsec - last.tv_nsec) / 1000000 < 100)
		return;
	last = now;
	unsigned int ship = game_player_ship();
	int slot = orbit_body_pick();
	if (!ship || slot <= 0)
		return;
	OrbitCtx c, here;
	if (!orbit_ctx_body(slot, &c))
		return;
	CheatPlayerMotion pm;
	double off[3];
	if (!cheat_player_motion(&pm) || !cheat_body_offset_km(slot, off))
		return;
	/* the ship's frame body: its regime says whether its axes are the system's */
	int rot = 0;
	if (orbit_ctx_get(&here) && here.regime == 255)
		rot = 1;
	double M[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
	if (rot)
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				M[i][j] = here.m[i][j];
#define ROT(v, o) for (int i_ = 0; i_ < 3; i_++) (o)[i_] = M[i_][0] * (v)[0] + M[i_][1] * (v)[1] + M[i_][2] * (v)[2]
	double up[3], vel[3], nose[3], vk[3], r[3];
	for (int i = 0; i < 3; i++)
		vk[i] = pm.vel_mps[i] / 1000.0;
	ROT(pm.up_km, up);
	ROT(vk, vel);
	ROT(pm.axis[2], nose);
	for (int i = 0; i < 3; i++)
		r[i] = up[i] - off[i];
	double rr = sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]), sp = sqrt(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
	double vr = rr > 0 ? (r[0] * vel[0] + r[1] * vel[1] + r[2] * vel[2]) / rr : 0;
	double vt = sqrt(fmax(0, sp * sp - vr * vr));
	double ang_body = rr > 0 ? acos(fmax(-1, fmin(1, -(r[0] * nose[0] + r[1] * nose[1] + r[2] * nose[2]) / rr))) * 180 / M_PI : 0;
	double ang_vel = sp > 0 ? acos(fmax(-1, fmin(1, (vel[0] * nose[0] + vel[1] * nose[1] + vel[2] * nose[2]) / sp))) * 180 / M_PI : 0;
	double pe = -1, ap = -1, e = -1, inc = -1;
	if (here.ok && here.body_slot == slot)
	{
		OrbitEl el;
		if (orbit_el_from_state(&here, here.r, here.u, &el))
		{
			pe = (el.rp - here.radius) * PU_KM;
			ap = el.bound ? (el.ra - here.radius) * PU_KM : -1;
			e = el.e;
			inc = el.inc * 180 / M_PI;
		}
	}
	if (shown_body != (unsigned)slot)
	{
		shown_body = (unsigned)slot;
		fprintf(f, "# body %d %s radius %.1f km pole %.5f %.5f %.5f prime %.5f %.5f %.5f\n", slot, c.name, c.radius * PU_KM,
			c.pole[0], c.pole[1], c.pole[2], c.prime[0], c.prime[1], c.prime[2]);
	}
	unsigned int a6 = GAME_A6;
	double t = ((double)(unsigned int)rdlong(a6 + 528) * 4294967296.0 + (unsigned int)rdlong(a6 + 524)) / 49710.0;
	char fr[24] = "";
	if (here.ok)
		snprintf(fr, sizeof fr, "%s", here.name);
	fprintf(f, "%.1f,%u,%s,%s,%.1f,%.1f,%.1f,%.1f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.1f,%.1f,%.1f,%.1f,%.5f,%.2f\n",
		t, (unsigned int)rdlong(a6 + A6_TICK_DT), c.name, fr, r[0], r[1], r[2], rr - c.radius * PU_KM, vel[0], vel[1], vel[2],
		sp, vr, vt, nose[0], nose[1], nose[2], (short)rdword(ship + 140), (short)rdword(ship + 142), (short)rdword(ship + 144),
		(short)rdword(ship + 134), (short)rdword(ship + 136), (short)rdword(ship + 138), rdbyte(a6 + 705) & 1, rdbyte(a6 + 704),
		(int)rdlong(a6 + 848), ang_body, ang_vel, pe, ap, e, inc);
	fflush(f);
}

void orbit_probe_start(int n, int k)
{
	left = n;
	every = k > 0 ? k : 1;
	countdown = 0;
}

void orbit_probe_poll(void)
{
	static int row;
	pathlog_poll();
	if (left <= 0)
		return;
	if (countdown-- > 0)
		return;
	countdown = every - 1;
	left--;
	State s;
	if (!state_get(&s))
		return;
	double t = ((double)(unsigned int)rdlong(GAME_A6 + 528) * 4294967296.0 + (unsigned int)rdlong(GAME_A6 + 524)) / 49710.0;
	printf("ORBIT %d t=%.1f dt=%u alt=%.3f pe=%.3f ap=%.3f sma=%.3f e=%.6f spd=%.3f vr=%.3f\n", row++, t,
		(unsigned int)rdlong(GAME_A6 + A6_TICK_DT), (s.rr - s.radius) * PU_KM, (s.rp - s.radius) * PU_KM,
		(s.ra - s.radius) * PU_KM, s.energy < 0 ? -s.mu / (2 * s.energy) * PU_KM : 0, s.e, sqrt(s.uu) / s.k, s.vr);
	fflush(stdout);
}
