/*
 * input.c - keyboard queue and mouse state, read by the game through host
 * calls.
 */
#include <math.h>

#include <SDL.h>

#include "input.h"
#include "gl/gl_cockpit.h"
#include "gl/gl_hud.h"
#include "gl/gl_mapscreen.h"
#include "m68000.h"
#include "main.h"
#include "quiz_buttons.h"
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
	bool drag, drag_pressed; /* the right button: held, pressed since the last poll */
	float drag_x, drag_y;    /* its motion not yet polled */
} laser;
/* The laser, rather than the mouse, has the pointer: since it last moved
 * (LASER_TAKE screen pixels) or clicked, until the mouse moves */
static bool laser_owns;
static float laser_x, laser_y; /* as the game last had it */
static int laser_swallowed;    /* held buttons whose press went elsewhere */
static SDL_atomic_t key_tap;   /* Input_TapSTKey's, 0: none */
#define LASER_TAKE 3.0f

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

void Input_PadPointerAt(int x, int y)
{
	Input_PadPointerBegin();
	if (!click_polls)
		pad_x = (float)x, pad_y = (float)y;
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

void Input_ClickAt(int x, int y)
{
	if (click_polls)
		return;
	pad_x = (float)x;
	pad_y = (float)y;
	pad_on = true;
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

void Input_SetDrag(bool held, float dx, float dy)
{
	SDL_AtomicLock(&laser_lock);
	laser.drag_pressed |= held && !laser.drag;
	laser.drag = held;
	laser.drag_x += dx;
	laser.drag_y += dy;
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

/* The watchdog's report (main.c): the buttons as the game would see them */
void Input_LogButtons(void)
{
	log_printf("Buttons: laser active=%d buttons=%x pressed=%x drag=%d swallowed=%x owns=%d mouse=%x pos=%d,%d\n", laser.active,
		laser.buttons, laser.pressed, laser.drag, laser_swallowed, laser_owns, input.cur_mousebut_state, (int)laser.x, (int)laser.y);
}

/* Main thread, at each poll: the laser's buttons for the game, and a
 * drag's motion */
static int take_laser(int *mx, int *my)
{
	SDL_AtomicLock(&laser_lock);
	bool active = laser.active;
	float x = laser.x, y = laser.y;
	int pressed = laser.pressed, buttons = laser.buttons | pressed;
	laser.pressed = 0;
	int drag = (laser.drag || laser.drag_pressed) ? 0x1 : 0;
	laser.drag_pressed = false;
	*mx = (int)laser.drag_x;
	*my = (int)laser.drag_y;
	laser.drag_x -= (float)*mx; /* the fractions kept for the next poll */
	laser.drag_y -= (float)*my;
	if (!active)
		laser_owns = false;
	else if (pressed || fabsf(x - laser_x) >= LASER_TAKE || fabsf(y - laser_y) >= LASER_TAKE)
		laser_owns = true;
	SDL_AtomicUnlock(&laser_lock);

	/* the system map's button, as for the mouse (main.c) */
	if ((pressed & 0x2) && !mapscreen_dashboard_up() && (int)x >= 18 && (int)x <= 31 && (int)y * 240 / 200 >= 226)
	{
		Input_PressSTKey(0x3c, TRUE); /* F2 */
		Input_PressSTKey(0x3c, FALSE);
		laser_swallowed |= 0x2;
	}
	int tap = SDL_AtomicSet(&key_tap, 0);
	if (tap)
	{
		Input_PressSTKey((unsigned char)tap, TRUE);
		Input_PressSTKey((unsigned char)tap, FALSE);
	}
	laser_swallowed &= buttons;
	buttons &= ~laser_swallowed;

	if (laser_owns || !active)
	{
		laser_x = x;
		laser_y = y;
	}
	return buttons | drag;
}

/* Host call: fills the game's mouse structures (big endian words)
 *   A7+2: motion x, motion y, buttons
 *   A7+6: absolute x, y in 320x200 screen coordinates */
void Call_GetMouseInput(void)
{
	unsigned long params = GetReg(REG_A7) - SIZE_WORD;
	short *mouse_mov = (short *)(STRam + STMemory_ReadLong(params + SIZE_WORD));
	short *mouse_abs = (short *)(STRam + STMemory_ReadLong(params + SIZE_WORD + SIZE_LONG));
	int drag_x, drag_y;
	mapscreen_poll();
	int laser_btn = take_laser(&drag_x, &drag_y);

	/* SDL occasionally reports a huge jump (e.g. when grabbing the mouse) */
	if (abs(input.motion_x) > 100 || abs(input.motion_y) > 100)
		input.motion_x = input.motion_y = 0;

	/* the mouse on one of the cockpit's side screens (the MAP and the SHIP
	 * up in flight): theirs, not the flight view's. Nothing of it goes on to
	 * the game. */
	int mx = 0, my = 0, px, py, side = -1;
	bool on_map = !Input_PadPointer(&px, &py) && !Input_LaserPointer(&px, &py) &&
				  cockpit_window_to_side(input.abs_x, input.abs_y, &side, &mx, &my);
	mapscreen_pointer(0, on_map ? side : -1, mx, my, (input.cur_mousebut_state | laser_btn) & 3,
					  input.motion_x + drag_x, input.motion_y + drag_y);
	if (on_map)
	{
		input.motion_x = input.motion_y = 0;
		drag_x = drag_y = 0;
		laser_btn = 0;
		mapscreen_drag(false, 0, 0);
	}
	else if (mapscreen_group_up(COCKPIT_SCREEN_MAP))
	{
		/* a hand's or a controller's drag is the map's (the mouse's own is
		 * the flight's steering unless the pointer is on the screen) */
		mapscreen_drag(laser_btn & 1, drag_x, drag_y);
		laser_btn &= ~1;
		drag_x = drag_y = 0;
	}

	/* window pixels -> game screen (letterboxed game area, or the cockpit),
	 * or the gamepad's pointer, or the laser's */
	int gx, gy;
	bool got = false;
	if (!on_map && (Input_PadPointer(&gx, &gy) || Input_LaserPointer(&gx, &gy) ||
					Screen_WindowToGame(input.abs_x, input.abs_y, &gx, &gy)))
	{
		got = true;
		mouse_abs[0] = SDL_SwapBE16(gx);
		mouse_abs[1] = SDL_SwapBE16(gy);
		last_x = gx;
		last_y = gy;
		have_last = true;
	}

	mouse_mov[0] = SDL_SwapBE16(SDL_SwapBE16(mouse_mov[0]) + input.motion_x + drag_x);
	mouse_mov[1] = SDL_SwapBE16(SDL_SwapBE16(mouse_mov[1]) + input.motion_y + drag_y);
	/* a press on the dashboard's buttons 2-4 shows or hides a screen, and is
	 * the game's no more (till it is let go) */
	static bool dash_held, dash_left;
	int buttons = (on_map ? 0 : input.cur_mousebut_state) | laser_btn;
	if (!(buttons & 0x2))
		dash_held = false;
	else if (!dash_left && got &&
			 (hud_orbit_button_press(gx, gy) || mapscreen_dashboard_press(gx, gy) || quiz_buttons_click(gx, gy)))
		dash_held = true;
	dash_left = (buttons & 0x2) != 0;
	if (dash_held)
		buttons &= ~0x2;
	if (mapscreen_real_pan(got ? gx : -1, got ? gy : -1, buttons & 0x2))
		buttons &= ~0x2;
	mouse_mov[2] = SDL_SwapBE16(0xf8 | buttons);

	input.motion_x = input.motion_y = 0;

	/* the left button going down: the map follows its select mode */
	static bool left_was;
	bool left = ((on_map ? 0 : input.cur_mousebut_state) | laser_btn) & 0x2;
	if (left && !left_was && have_last)
		cockpit_map_click(last_x, last_y);
	left_was = left;

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

void Input_TapSTKey(unsigned char scancode)
{
	SDL_AtomicSet(&key_tap, scancode);
}

void Input_PressSTKey(unsigned char scancode, BOOL press)
{
	/* the key of a screen the dashboard has hidden shows it again */
	static bool shown_by[3];
	if (scancode >= 0x3c && scancode <= 0x3e)
	{
		int g = scancode - 0x3c;
		if (getenv("GLF_MAP_LOG") && press)
			log_printf("key F%d: group %d hidden %d\n", g + 2, g, mapscreen_hidden(g));
		if (press ? mapscreen_hidden(g) : shown_by[g])
		{
			shown_by[g] = press;
			if (press)
				mapscreen_show(g);
			return;
		}
	}
	/* F2 with the MAP screen up in flight is the screen's (the galaxy map
	 * and the system map take turns on it), not the game's (but for the one
	 * that opens the real map for it, mapscreen_poll) */
	static bool f2_to_map;
	if (scancode == 0x3c && (press ? mapscreen_group_up(COCKPIT_SCREEN_MAP) && !mapscreen_handing() : f2_to_map))
	{
		f2_to_map = press;
		if (press)
			mapscreen_key(COCKPIT_SCREEN_MAP, 0x3c);
		return;
	}
	/* ... and F3 the ship screen's page */
	static bool f3_to_ship;
	if (scancode == 0x3d && (press ? mapscreen_group_up(COCKPIT_SCREEN_SHIP) : f3_to_ship))
	{
		f3_to_ship = press;
		if (press)
			mapscreen_key(COCKPIT_SCREEN_SHIP, 0x3d);
		return;
	}
	if (!press)
		scancode |= 0x80;
	/* full (the game takes one a vblank): drop this one, rather than wrap
	 * round onto the head and lose the lot */
	if ((input.buf_tail + 1) % SIZE_KEYBUF == input.buf_head)
		return;
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
