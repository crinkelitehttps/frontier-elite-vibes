/*
 * time_guard.c - see time_guard.h
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cheats.h"
#include "debug_screen.h"
#include "game_state.h"
#include "host.h" /* rdbyte, rdword, rdlong, wrbyte */
#include "main.h" /* log_printf */
#include "time_guard.h"

int time_guard_seconds = 9;

/* A6 offsets */
#define A6_TIME_OF_DAY    524   /* long: the clock's low part, 49710 units a game second */
#define A6_MODAL          598   /* byte: bit 0 a modal screen, which takes keys itself */
#define A6_RATE           14482 /* long: game time added per vblank, 994 at 1x */
#define A6_RATE_INDEX     14486 /* word: 0, 3, 7, 10, 12 for 1x to 10000x, 18 hyperspace */
#define A6_UNDER_ATTACK   10476 /* byte: bit 1 something hostile is attacking; Shift+F2..F5 are refused */
#define A6_SCANCODE_ASCII 16336 /* the game's key queue, as script.c's `ascii` pushes a key */
#define A6_PUSHED_ASCII   16337
#define GAME_SECOND       49710.0
#define VBLANK_HZ         50.0
#define RATE_1X           994.0
#define ZONE_KM           7.0  /* the game's own warning, when the largest component is under 4.2 km */
#define STEP_FRAMES       4    /* frames to wait for a pushed key to take */
#define ARRIVE_FAR_KM     100.0
#define ARRIVE_NEAR_KM    15.0
#define KEY_1X            0xc1 /* Shift+F1 .. Shift+F5 */
#define KEY_10X           0xc2
#define KEY_100X          0xc3
#define KEY_1000X         0xc4
#define HYPERSPACE_INDEX  18

typedef struct
{
	bool valid;
	char registration[16];
	double km;
	double closing; /* km per game second, as of the last frame */
} Track;

static Track track[CHEAT_SHIPS_MAX + 1]; /* by slot: the ship as the last frame saw it */

static uint32_t a6(int offset)
{
	return GAME_A6 + offset;
}

/* Ships that are not flying: parked, docked or launching at a station, waiting to arrive, or the
 * orbital ones that do nothing (npc-ship-ai.md section 5) */
static bool grounded(int state)
{
	switch (state)
	{
	case 0x2c:
	case 0x30:
	case 0x34:
	case 0x3c:
	case 0x70:
	case 0x74:
	case 0x78:
	case 0x7c:
	case 0x84:
		return true;
	}
	return false;
}

static bool in_flight(void)
{
	if (game_screen_in_batch() || game_screen() != SCREEN_FLIGHT)
		return false;
	if (rdbyte(a6(A6_FLIGHT_STATE)) == 8) /* hyperspace */
		return false;
	FlightStatus status = game_flight_status();
	return status != FLIGHT_DOCKED && status != FLIGHT_LANDED && status != FLIGHT_LANDED_ROUGH;
}

/* The key that takes the rate one step down from index `index` */
static int step_down_key(int index)
{
	return index >= 12 ? KEY_1000X : index >= 10 ? KEY_100X : index >= 7 ? KEY_10X : KEY_1X;
}

void time_guard_update(void)
{
	static uint32_t last_time;
	static bool have_time;
	static int cooldown, last_rate, last_index = -1;
	static double last_real = INFINITY;

	uint32_t now = (uint32_t)rdlong(a6(A6_TIME_OF_DAY));
	if (have_time && now == last_time)
		return; /* the same game frame (or paused) */
	double dt = have_time ? (double)(uint32_t)(now - last_time) / GAME_SECOND : 0.0;
	last_time = now;
	have_time = true;

	if (cooldown > 0)
		cooldown--;
	/* a long gap is a load or a jump of the clock: nothing to compare with */
	if (time_guard_seconds <= 0 || !in_flight() || dt <= 0.0 || dt > 1e6)
	{
		memset(track, 0, sizeof track);
		return;
	}

	static CheatShip ships[CHEAT_SHIPS_MAX];
	const int count = cheat_ships(ships);
	const int rate = (int)rdlong(a6(A6_RATE));
	const int index = (int)(uint16_t)rdword(a6(A6_RATE_INDEX));
	const double per_second = rate * VBLANK_HZ / GAME_SECOND; /* game seconds in a real second */
	double soonest = INFINITY;                                /* game seconds until the first is in the zone */
	const CheatShip *first = NULL;
	bool seen[CHEAT_SHIPS_MAX + 1] = {false};

	for (int i = 0; i < count; i++)
	{
		const CheatShip *s = &ships[i];
		Track *t = &track[s->slot];
		if (t->valid && !strcmp(t->registration, s->registration) && !grounded(s->state))
		{
			const double was = t->km, closing = (was - s->distance_km) / dt; /* km per game second */
			if (was >= ARRIVE_FAR_KM && s->distance_km <= ARRIVE_NEAR_KM && s->distance_km * 4.0 <= was)
			{
				if (getenv("GLF_GUARD_LOG"))
					log_printf("time guard: %s arrived at %.1f km from %.4g km at %dx%s\n", s->registration, s->distance_km, was,
							   (int)(last_rate / RATE_1X + 0.5), s->at_rest ? ", at rest" : "");
				debug_screen_log("ARRIVE %s %.1fkm<-%.3gkm %dx%s", s->registration, s->distance_km, was,
								 (int)(last_rate / RATE_1X + 0.5), s->at_rest ? " TP" : "");
			}
			if (closing > 0.0)
			{
				/* a ship braking along the AI's curve (speed falling with the distance) needs twice the time
				 * that its present speed gives */
				double left = (s->distance_km - ZONE_KM) / closing * (closing < t->closing * 0.9995 ? 2.0 : 1.0);
				if (left < soonest)
					soonest = left > 0.0 ? left : 0.0, first = s;
			}
		}
		t->closing = t->valid && !strcmp(t->registration, s->registration) ? (t->km - s->distance_km) / dt : 0.0;
		t->valid = true;
		snprintf(t->registration, sizeof t->registration, "%s", s->registration);
		t->km = s->distance_km;
		seen[s->slot] = true;
	}
	for (int slot = 0; slot <= CHEAT_SHIPS_MAX; slot++)
		if (!seen[slot])
			track[slot].valid = false;
	/* GLF_GUARD_PROBE: only watch (no keys from here, the game code's hook may be at work), logging how
	 * many real seconds of flight a ship still had when the rate changed */
	const bool probe = getenv("GLF_GUARD_PROBE") != NULL;
	if (probe && index != last_index && last_index >= 0)
		log_printf("probe: rate index %d -> %d, %.2f real s of flight were left\n", last_index, index, last_real);
	last_index = index;
	last_real = isfinite(soonest) && per_second > 0 ? soonest / per_second : INFINITY;
	last_rate = rate; /* (an arrival cancels the rate in the same frame: the log says what it was) */

	if (probe || index <= 0 || index >= HYPERSPACE_INDEX || rate <= 0 || cooldown > 0 || !isfinite(soonest))
		return;
	/* each step makes the time left ten times longer in real seconds */
	if (soonest / per_second > time_guard_seconds / 10.0)
		return;
	if ((rdbyte(a6(A6_MODAL)) & 1) || rdbyte(a6(A6_SCANCODE_ASCII)))
		return; /* a screen is taking keys, or one is waiting: the next frame */
	/* while something attacks the game refuses the faster steps (but not 1x) */
	const int key = (rdbyte(a6(A6_UNDER_ATTACK)) & 2) ? KEY_1X : step_down_key(index);
	wrbyte(a6(A6_PUSHED_ASCII), key);
	wrbyte(a6(A6_SCANCODE_ASCII), 0xff);
	cooldown = STEP_FRAMES;
	if (getenv("GLF_GUARD_LOG"))
		log_printf("time guard: %s at %.4g km, rate index %d, %.2f s of flight left: key %02x\n", first->registration,
				   first->distance_km, index, soonest / per_second, key);
}
