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

#include "game_state.h"
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

void orbit_probe_start(int n, int k)
{
	left = n;
	every = k > 0 ? k : 1;
	countdown = 0;
}

void orbit_probe_poll(void)
{
	static int row;
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
