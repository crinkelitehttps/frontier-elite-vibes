/*
 * spawn_log.c - see spawn_log.h
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "cheats.h"
#include "game_state.h"
#include "host.h"
#include "main.h"
#include "spawn_log.h"

#define A6_TIME_OF_DAY 524
#define A6_RATE        14482
#define A6_RATE_INDEX  14486
#define GAME_SECOND    49710.0
#define RATE_1X        994.0
#define MAX_TRACKED    115

typedef struct
{
	bool valid;
	unsigned sig;
	int model;
	double born; /* game seconds */
	double born_real;
	double born_dist;
	CheatScenery last; /* as the previous look saw it */
} Tracked;

static Tracked tracked[MAX_TRACKED];

static double dot(const double *a, const double *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void unit(const double *in, double *out)
{
	double l = sqrt(dot(in, in));
	for (int i = 0; i < 3; i++)
		out[i] = l > 0 ? in[i] / l : 0;
}

/* pitch above the local horizon and azimuth about the local up (0 = the parent frame's +y projected on
 * the horizon, turning towards the horizon's other axis), in degrees */
static void heading(const double up[3], const double dir[3], double *pitch, double *az)
{
	double u[3], n[3], e[3], d[3];
	unit(up, u);
	unit(dir, d);
	const double y[3] = {0, 1, 0};
	double k = dot(y, u);
	double proj[3] = {y[0] - k * u[0], y[1] - k * u[1], y[2] - k * u[2]};
	unit(proj, n);
	e[0] = u[1] * n[2] - u[2] * n[1];
	e[1] = u[2] * n[0] - u[0] * n[2];
	e[2] = u[0] * n[1] - u[1] * n[0];
	*pitch = asin(fmax(-1.0, fmin(1.0, dot(d, u)))) * 180.0 / M_PI;
	*az = atan2(dot(d, e), dot(d, n)) * 180.0 / M_PI;
}

static double now_game, now_real, real0;
static int now_rate;

static void me_text(const CheatPlayerMotion *m, char *out, int size, double fwd_unit[3], double vel_unit[3])
{
	double speed = sqrt(dot(m->vel_mps, m->vel_mps));
	unit(m->vel_mps, vel_unit);
	for (int i = 0; i < 3; i++)
		fwd_unit[i] = m->axis[2][i];
	double np, naz, vp, vaz;
	heading(m->up_km, fwd_unit, &np, &naz);
	heading(m->up_km, vel_unit, &vp, &vaz);
	snprintf(out, size,
			 "speed=%.1fm/s(raw %ld) alt=%.0fm par=%d pos=(%.2f,%.2f,%.2f)km nose pitch=%.1f az=%.1f vel pitch=%.1f az=%.1f "
			 "nose=(%.3f,%.3f,%.3f) vel=(%.3f,%.3f,%.3f)",
			 speed, m->raw_speed, m->alt_m, m->parent, m->up_km[0], m->up_km[1], m->up_km[2], np, naz, vp, vaz, fwd_unit[0],
			 fwd_unit[1], fwd_unit[2], vel_unit[0], vel_unit[1], vel_unit[2]);
}

static void report(const char *what, const CheatScenery *s, const Tracked *t, const CheatPlayerMotion *m)
{
	char me[400];
	double fwd[3], vel[3];
	me_text(m, me, sizeof me, fwd, vel);
	double dist = sqrt(dot(s->rel_km, s->rel_km));
	double ahead = dot(s->rel_km, vel), ahead_nose = dot(s->rel_km, fwd);
	char age[80] = "";
	if (t)
		snprintf(age, sizeof age, " age=%.2fgs/%.2freal bornat=%.2fkm", now_game - t->born, now_real - t->born_real, t->born_dist);
	log_printf("SPAWN t=%.2f real=%.2f %s slot=%d model=%d exp=%d b95=%d sig=%08x type=%d flags=%02x state=%02x class=%02x parent=%d(%s) pos=(%.3f,%.3f,%.3f)km height=%.3fkm%s dist=%.2fkm rel=(%.2f,%.2f,%.2f)km "
			   "ahead_vel=%.2fkm ahead_nose=%.2fkm | rate=%dx %s\n",
			   now_game, now_real, what, s->slot, s->model, s->exponent, s->b95, s->sig, s->type, s->flags, s->state, s->class_, s->parent, s->parent_name, s->pos_km[0],
			   s->pos_km[1], s->pos_km[2], s->height_km, age, dist, s->rel_km[0], s->rel_km[1],
			   s->rel_km[2], ahead, ahead_nose, now_rate, me);
}

void spawn_log_update(void)
{
	static int enabled = -1;
	static bool have_time;
	static uint32_t last_time;
	static double game;
	static int last_index = -1, last_rate = -1;
	if (enabled < 0)
	{
		const char *e = getenv("GLF_SPAWN_LOG");
		enabled = e && *e && *e != '0';
		if (enabled)
			log_printf("SPAWN log on: t = game seconds since the log started, real = wall seconds; speed from the velocity "
					   "longs at 0.3884 m/s each; pitch/az about the local up, az 0 = parent +y\n");
	}
	if (!enabled || !GAME_A6)
		return;

	uint32_t clock = (uint32_t)rdlong(GAME_A6 + A6_TIME_OF_DAY);
	if (have_time && clock != last_time)
		game += (double)(uint32_t)(clock - last_time) / GAME_SECOND;
	last_time = clock;
	if (!have_time)
		real0 = SDL_GetTicks() / 1000.0;
	have_time = true;
	now_game = game;
	now_real = SDL_GetTicks() / 1000.0 - real0;

	int rate = (int)rdlong(GAME_A6 + A6_RATE);
	int index = (int)(uint16_t)rdword(GAME_A6 + A6_RATE_INDEX);
	now_rate = (int)(rate / RATE_1X + 0.5);
	if (index != last_index || rate != last_rate)
	{
		log_printf("SPAWN t=%.2f real=%.2f RATE index %d -> %d, rate raw %d -> %d (%dx)\n", now_game, now_real, last_index, index,
				   last_rate, rate, now_rate);
		last_index = index;
		last_rate = rate;
	}

	CheatPlayerMotion me;
	if (!cheat_player_motion(&me))
	{
		memset(tracked, 0, sizeof tracked);
		return;
	}
	static CheatScenery list[MAX_TRACKED];
	int n = cheat_scenery(list, MAX_TRACKED);
	bool seen[MAX_TRACKED] = {false};
	for (int i = 0; i < n; i++)
	{
		const CheatScenery *s = &list[i];
		Tracked *t = &tracked[s->slot];
		if (t->valid && (t->sig != s->sig || t->model != s->model))
		{
			/* the slot was freed and used again between two looks */
			report("DESTROY", &t->last, t, &me);
			t->valid = false;
		}
		if (!t->valid)
		{
			t->valid = true;
			t->sig = s->sig;
			t->model = s->model;
			t->born = now_game;
			t->born_real = now_real;
			t->born_dist = sqrt(dot(s->rel_km, s->rel_km));
			report("CREATE", s, NULL, &me);
		}
		t->last = *s;
		seen[s->slot] = true;
	}
	for (int slot = 0; slot < MAX_TRACKED; slot++)
	{
		Tracked *t = &tracked[slot];
		if (t->valid && !seen[slot])
		{
			report("DESTROY", &t->last, t, &me);
			t->valid = false;
		}
	}
}
