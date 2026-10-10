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

/* The VR laser pointer (gl_laser.c), set by the drawing and taken by the
 * game's polls */
static SDL_SpinLock laser_lock;
static struct
{
	bool active, hit;
	float x, y;
	int buttons, pressed; /* held, and pressed since the last poll */
} laser;
/* The laser, rather than the mouse, has the pointer: since it last moved
 * (LASER_TAKE screen pixels) or clicked, until the mouse moves */
static bool laser_owns;
static float laser_x, laser_y; /* as the game last had it */
static int laser_buttons;      /* as the game last had them */
static int laser_swallowed;    /* held buttons whose press went elsewhere */

/* The laser moving with the right button held moves the mouse, as mouse
 * pixels per screen pixel (steering, turning the maps) */
#define LASER_MOTION 4.0f
#define LASER_TAKE   3.0f

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

void Input_SetLaser(bool active, bool hit, float x, float y, int buttons)
{
	SDL_AtomicLock(&laser_lock);
	laser.pressed |= buttons & ~laser.buttons;
	laser.buttons = buttons;
	laser.active = active;
	laser.hit = hit;
	if (hit)
	{
		laser.x = x;
		laser.y = y;
	}
	SDL_AtomicUnlock(&laser_lock);
}

bool Input_LaserPointer(int *x, int *y)
{
	SDL_AtomicLock(&laser_lock);
	bool on = laser.active && laser_owns;
	*x = (int)laser.x;
	*y = (int)laser.y;
	SDL_AtomicUnlock(&laser_lock);
	return on;
}

void Input_MouseMoved(void)
{
	SDL_AtomicLock(&laser_lock);
	laser_owns = false;
	SDL_AtomicUnlock(&laser_lock);
}

/* Main thread, at each poll: the laser's buttons for the game, and its
 * movement with the right button held */
static int take_laser(void)
{
	SDL_AtomicLock(&laser_lock);
	bool active = laser.active;
	float x = laser.x, y = laser.y;
	int pressed = laser.pressed, buttons = laser.buttons | pressed;
	laser.pressed = 0;
	if (!active)
		laser_owns = false;
	else if (pressed || fabsf(x - laser_x) >= LASER_TAKE || fabsf(y - laser_y) >= LASER_TAKE)
		laser_owns = true;
	SDL_AtomicUnlock(&laser_lock);

	/* the system map's button, as for the mouse (main.c) */
	if ((pressed & 0x2) && (int)x >= 18 && (int)x <= 31 && (int)y * 240 / 200 >= 226)
	{
		Input_PressSTKey(0x3c, TRUE); /* F2 */
		Input_PressSTKey(0x3c, FALSE);
		laser_swallowed |= 0x2;
	}
	laser_swallowed &= buttons;
	buttons &= ~laser_swallowed;

	if (laser_owns && (buttons & laser_buttons & 0x1))
	{
		input.motion_x += (int)lrintf((x - laser_x) * LASER_MOTION);
		input.motion_y += (int)lrintf((y - laser_y) * LASER_MOTION);
	}
	if (laser_owns || !active)
	{
		laser_x = x;
		laser_y = y;
	}
	laser_buttons = buttons;
	return buttons;
}

/* Host call: fills the game's mouse structures (big endian words)
 *   A7+2: motion x, motion y, buttons
 *   A7+6: absolute x, y in 320x200 screen coordinates */
void Call_GetMouseInput(void)
{
	unsigned long params = GetReg(REG_A7) - SIZE_WORD;
	short *mouse_mov = (short *)(STRam + STMemory_ReadLong(params + SIZE_WORD));
	short *mouse_abs = (short *)(STRam + STMemory_ReadLong(params + SIZE_WORD + SIZE_LONG));
	int laser_btn = take_laser();

	/* SDL occasionally reports a huge jump (e.g. when grabbing the mouse) */
	if (abs(input.motion_x) > 100 || abs(input.motion_y) > 100)
		input.motion_x = input.motion_y = 0;

	mouse_mov[0] = SDL_SwapBE16(SDL_SwapBE16(mouse_mov[0]) + input.motion_x);
	mouse_mov[1] = SDL_SwapBE16(SDL_SwapBE16(mouse_mov[1]) + input.motion_y);
	mouse_mov[2] = SDL_SwapBE16(0xf8 | input.cur_mousebut_state | laser_btn);

	/* window pixels -> game screen (letterboxed game area, or the cockpit),
	 * or the gamepad's pointer, or the laser's */
	int gx, gy;
	if (Input_PadPointer(&gx, &gy) || Input_LaserPointer(&gx, &gy) ||
		Screen_WindowToGame(input.abs_x, input.abs_y, &gx, &gy))
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
