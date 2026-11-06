/*
 * rt_clock.c - see rt_clock.h
 */
#include <SDL.h>
#include <stdint.h>
#include <stdlib.h>

#include "game_state.h" /* FE2_vblank_waits */
#include "host.h"		/* rdlong, wrlong, wrword, GetReg, fe2vm_depth */
#include "m68000.h"
#include "renderer.h" /* Screen_WaitFrameTaken */
#include "rt_clock.h"

bool realtime_clock = true;

/* A6 offsets */
#define A6_TIMER	666	  /* long: a real-time counter (mission deadlines, l63cae), +994 a vblank */
#define A6_GAME_ACC 14450 /* long: game time not yet taken, + the rate a vblank */
#define A6_REAL_ACC 14458 /* long: real time not yet taken, +994 a vblank */
#define A6_RATE		14482 /* long: game time per vblank, 994 at 1x */
#define VBLANK_TIME 994
#define VBLANK_HZ	50
#define MAX_STEP_MS 100 /* the most one take is credited: a load, a stall */
#define TAKE_WAIT_MS 50 /* the longest a frame waits for the drawing to take the last */
#define MIN_FRAME_US 2000 /* a ceiling on game frames: 500 a second */

static Uint64 last;		  /* the clock at the last credit, 0 = start again */
static Uint64 el_rem;	  /* of elapsed * 50 * 65536 / freq */
static uint32_t real_rem; /* fractions of a unit, in 1/65536 */
static uint32_t game_rem;

static bool on(void)
{
	static int env = -2;
	if (env == -2)
	{
		const char *e = SDL_getenv("GLF_RTCLOCK");
		env = e ? atoi(e) != 0 : -1;
	}
	return env >= 0 ? env != 0 : realtime_clock;
}

static uint32_t a6(uint32_t off)
{
	return (uint32_t)GetReg(REG_A6) + off;
}

void Call_RtVbl(void)
{
	if (on())
		return;
	last = 0; /* turned on later: no jump */
	wrlong(a6(A6_GAME_ACC), rdlong(a6(A6_GAME_ACC)) + rdlong(a6(A6_RATE)));
	wrlong(a6(A6_REAL_ACC), rdlong(a6(A6_REAL_ACC)) + VBLANK_TIME);
	wrlong(a6(A6_TIMER), rdlong(a6(A6_TIMER)) + VBLANK_TIME);
}

void Call_RtCredit(void)
{
	/* the second VM (MAP screen, camera) runs game frames of its own on a
	 * copy: its takes are not the game's time */
	if (!on() || fe2vm_depth)
		return;
	Uint64 now = SDL_GetPerformanceCounter(), freq = SDL_GetPerformanceFrequency();
	if (!last)
	{
		last = now;
		el_rem = real_rem = game_rem = 0;
		return;
	}
	Uint64 el = now - last, cap = freq * MAX_STEP_MS / 1000;
	last = now;
	if (el > cap)
		el = cap, el_rem = 0;
	/* elapsed in vblanks, 16.16 fixed point */
	Uint64 x = el * VBLANK_HZ * 65536 + el_rem;
	Uint64 fv = x / freq;
	el_rem = x % freq;
	if (!fv)
		return;

	Uint64 r = (Uint64)VBLANK_TIME * fv + real_rem;
	uint32_t real_add = (uint32_t)(r >> 16);
	real_rem = (uint32_t)(r & 0xffff);
	Uint64 g = (Uint64)rdlong(a6(A6_RATE)) * fv + game_rem;
	uint32_t game_add = (uint32_t)(g >> 16);
	game_rem = (uint32_t)(g & 0xffff);

	wrlong(a6(A6_GAME_ACC), rdlong(a6(A6_GAME_ACC)) + game_add);
	wrlong(a6(A6_REAL_ACC), rdlong(a6(A6_REAL_ACC)) + real_add);
	wrlong(a6(A6_TIMER), rdlong(a6(A6_TIMER)) + real_add);
}

void Call_RtPace(void)
{
	if (!on() || fe2vm_depth)
		return;
	static int free_us = -1; /* GLF_RTPACE=US: no wait for the drawing, a frame every US at most */
	if (free_us < 0)
	{
		const char *e = SDL_getenv("GLF_RTPACE");
		free_us = e ? atoi(e) : 0;
	}
	/* one game frame a picture: the drawing has taken the last one */
	if (!free_us)
		Screen_WaitFrameTaken(TAKE_WAIT_MS);
	static Uint64 prev;
	Uint64 freq = SDL_GetPerformanceFrequency(), gap = freq * (free_us ? free_us : MIN_FRAME_US) / 1000000;
	Uint64 now = SDL_GetPerformanceCounter();
	if (prev && now - prev < gap)
		SDL_Delay((Uint32)((gap - (now - prev)) * 1000 / freq) + 1);
	prev = SDL_GetPerformanceCounter();
	wrword(FE2_vblank_waits, 0);
}
