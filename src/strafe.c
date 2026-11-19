/*
 * strafe.c - see strafe.h
 */
#include <SDL.h>
#include <math.h>

#include "host.h"
#include "keymap.h"
#include "main.h"
#include "strafe.h"

/* Ship thrust limits (words): x +, x -, y +, y -, z +, z - */
#define SHIP_THRUST_LIMITS 146

static float pad[2];

void strafe_set_pad(float x, float y)
{
	pad[0] = x;
	pad[1] = y;
}

static float clamp1(float v)
{
	return fmaxf(-1.0f, fminf(1.0f, v));
}

/* v (-1..1) of the ship's thrust along one axis; limits at ship + offset */
static int thrust(float v, unsigned int limits)
{
	int max = rdword(limits), min = rdword(limits + 2);
	return (int)lrintf(v > 0.0f ? v * max : -v * min);
}

void Call_Strafe(void)
{
	const Uint8 *k = SDL_GetKeyboardState(NULL);
	float kx, ky;
	Keymap_Strafe(&kx, &ky); /* W / A / S / D in flight */
	float x = clamp1(pad[0] + kx + k[SDL_SCANCODE_KP_6] - k[SDL_SCANCODE_KP_4]);
	float y = clamp1(pad[1] + ky + k[SDL_SCANCODE_KP_8] - k[SDL_SCANCODE_KP_2]);
	unsigned int ship = GetReg(REG_A0);
	if (x != 0.0f)
		SetReg(REG_D0, thrust(x, ship + SHIP_THRUST_LIMITS));
	if (y != 0.0f)
		SetReg(REG_D1, thrust(y, ship + SHIP_THRUST_LIMITS + 4));
}
