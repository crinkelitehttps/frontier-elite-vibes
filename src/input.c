/*
 * input.c - keyboard queue and mouse state, read by the game through host
 * calls.
 */
#include <math.h>

#include <SDL.h>

#include "input.h"
#include "m68000.h"
#include "main.h"
#include "renderer.h"

CINPUT input;

/* The gamepad's pointer, and where the game's pointer last was */
static bool pad_on;
static float pad_x, pad_y;
static int click_polls; /* clicking: polls left with the button down, then released */
static bool have_last;
static int last_x, last_y;

#define CLICK_POLLS 3

void Input_PadPointerBegin(void)
{
	if (pad_on)
		return;
	pad_on = true;
	click_polls = 0;
	/* where the game's pointer is, else the middle of the control panel */
	pad_x = have_last ? (float)last_x : 160.0f;
	pad_y = have_last ? (float)last_y : 184.0f;
}

void Input_PadPointerMove(float dx, float dy)
{
	if (!pad_on || click_polls)
		return;
	pad_x = fminf(fmaxf(pad_x + dx, 0.0f), 319.0f);
	pad_y = fminf(fmaxf(pad_y + dy, 0.0f), 199.0f);
}

void Input_PadPointerClick(void)
{
	if (!pad_on || click_polls)
		return;
	click_polls = CLICK_POLLS;
	input.cur_mousebut_state |= 0x2;
}

bool Input_PadPointer(int *x, int *y)
{
	if (!pad_on)
		return false;
	*x = (int)pad_x;
	*y = (int)pad_y;
	return true;
}

/* Host call: fills the game's mouse structures (big endian words)
 *   A7+2: motion x, motion y, buttons
 *   A7+6: absolute x, y in 320x200 screen coordinates */
void Call_GetMouseInput(void)
{
	unsigned long params = GetReg(REG_A7) - SIZE_WORD;
	short *mouse_mov = (short *)(STRam + STMemory_ReadLong(params + SIZE_WORD));
	short *mouse_abs = (short *)(STRam + STMemory_ReadLong(params + SIZE_WORD + SIZE_LONG));

	/* SDL occasionally reports a huge jump (e.g. when grabbing the mouse) */
	if (abs(input.motion_x) > 100 || abs(input.motion_y) > 100)
		input.motion_x = input.motion_y = 0;

	mouse_mov[0] = SDL_SwapBE16(SDL_SwapBE16(mouse_mov[0]) + input.motion_x);
	mouse_mov[1] = SDL_SwapBE16(SDL_SwapBE16(mouse_mov[1]) + input.motion_y);
	mouse_mov[2] = SDL_SwapBE16(0xf8 | input.cur_mousebut_state);

	/* window pixels -> game screen (letterboxed game area, or the cockpit),
	 * or the gamepad's pointer */
	int gx, gy;
	if (Input_PadPointer(&gx, &gy) || Screen_WindowToGame(input.abs_x, input.abs_y, &gx, &gy))
	{
		mouse_abs[0] = SDL_SwapBE16(gx);
		mouse_abs[1] = SDL_SwapBE16(gy);
		last_x = gx;
		last_y = gy;
		have_last = true;
	}

	input.motion_x = input.motion_y = 0;

	/* the gamepad's click: the button held for a few polls, then released
	 * and the pointer put away */
	if (click_polls && --click_polls == 0)
	{
		input.cur_mousebut_state &= ~0x2;
		pad_on = false;
	}
}

/* Host call: next queued ST scancode in D0, or 0 */
void Call_GetKeyboardEvent(void)
{
	if (input.buf_head != input.buf_tail)
	{
		SetReg(REG_D0, input.key_buf[input.buf_head++]);
		input.buf_head %= SIZE_KEYBUF;
	}
	else
	{
		SetReg(REG_D0, 0);
	}
}

void Input_PressSTKey(unsigned char scancode, BOOL press)
{
	if (!press)
		scancode |= 0x80;
	input.key_buf[input.buf_tail++] = scancode;
	input.buf_tail %= SIZE_KEYBUF;
}

/* While the right button is held the game steers with the mouse, so
 * capture it */
static void update_mouse_grab(void)
{
	SDL_SetRelativeMouseMode((input.cur_mousebut_state & 0x1) ? SDL_TRUE : SDL_FALSE);
}

void Input_MousePress(int sdl_button)
{
	if (sdl_button == SDL_BUTTON_RIGHT)
		input.cur_mousebut_state |= 0x1;
	else if (sdl_button == SDL_BUTTON_LEFT)
		input.cur_mousebut_state |= 0x2;
	else
		return;
	update_mouse_grab();
}

void Input_MouseRelease(int sdl_button)
{
	if (sdl_button == SDL_BUTTON_RIGHT)
		input.cur_mousebut_state &= ~0x1;
	else if (sdl_button == SDL_BUTTON_LEFT)
		input.cur_mousebut_state &= ~0x2;
	else
		return;
	update_mouse_grab();
}
