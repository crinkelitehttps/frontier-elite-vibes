/*
 * freecam.c - see freecam.h.
 *
 * The camera is kept here as a position relative to the player's ship plus
 * three axes (right, up, forward), in the same frame and units as the
 * ship's own matrix and position. Every tick they go to Lfreecam_state in
 * emulated RAM, where Lfreecam_camera picks them up when the flight view
 * is drawn.
 */
#include <math.h>
#include <string.h>

#include "freecam.h"
#include "game_state.h"
#include "gl/gl_cockpit.h"
#include "host.h" /* rdbyte, wrbyte, ... */
#include "main.h"
#include "renderer.h" /* game area, screen_w / screen_h */

#define A6_TIME_ACCEL 14482 /* A6_time_accel: game time per frame, 0 = stopped */
#define A6_MODAL          598   /* byte: bit 0 a modal screen, which takes keys itself */
#define A6_SCANCODE_ASCII 16336 /* the game's key queue, as time_guard.c pushes a key */
#define A6_PUSHED_ASCII   16337
#define A6_RATE_INDEX     14486 /* word: 0, 3, 7, 10, 12 for 1x to 10000x, 18 hyperspace */
#define KEY_1X            0xc1  /* Shift+F1 .. Shift+F5: the game's five rates */

/* Lfreecam_state: byte active, byte unused, 3x3 matrix words, 3 offset longs */
#define STATE_ACTIVE FE2_Lfreecam_active
#define STATE_MATRIX FE2_Lfreecam_matrix
#define STATE_OFFSET FE2_Lfreecam_offset

/* Where the external view starts behind the ship (A6_extcam_dist, $ff40,
 * times 256): a good unit for "about a ship away" */
#define SHIP_DISTANCE 49152.0
#define SPEED_MIN     -8
#define SPEED_MAX     24
#define TURN_RATE     1.5   /* radians per second, keys */
#define MOUSE_RATE    0.004 /* radians per pixel */
#define MAX_OFFSET    2.0e9 /* the offset is a 32 bit long */

enum
{
	RIGHT,
	UP,
	FORWARD
};

bool freecam_invert_y;
bool freecam_show_help = true;

/* Lfreecam_ only exists in fe2_modded.s: the original game build gets the
 * stubs at the end of this file, so the free camera just stays off */
#if FE2_USE_MODDED

static bool active;
static double pos[3];
static double axis[3][3];
static int speed_level;
static int saved_time_accel, saved_view;
/* 0: the game is held (A6_time_accel zeroed every tick); 1-5: it runs at the
 * game's own rate 1x .. 10000x (set with its Shift+F1-5 keys, so its panel,
 * its time guard and its refusals all work as in flight) */
static int time_level;
static int pending_key;
static int look_dx, look_dy;
static int mouse_buttons;
static Uint32 last_ticks;

/* Touch controls, done like the rest of touch_input.c: the mouse (which
 * is also what SDL makes of the first finger) presses the on-screen
 * controls, so they work with a mouse too; extra fingers come as finger
 * events, e.g. to look around while the first one holds the stick. */
#define MAX_POINTERS  10
#define MOUSE_POINTER 0
enum
{
	ROLE_STICK = FC_BUTTONS,
	ROLE_LOOK
};
static struct
{
	SDL_FingerID id; /* extra fingers only */
	int role;        /* an FC_ button, ROLE_STICK or ROLE_LOOK */
	bool used;
} pointers[MAX_POINTERS];
static FreecamTouchView touch; /* layout plus what is held */

/* =========================================================================
 * Vector helpers
 * ========================================================================= */
static double dot(const double *a, const double *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void normalise(double *v)
{
	double len = sqrt(dot(v, v));
	if (len > 1e-9)
		for (int i = 0; i < 3; i++)
			v[i] /= len;
}

/* Turns a towards b by `angle` in their plane (both stay perpendicular) */
static void rotate(double *a, double *b, double angle)
{
	double c = cos(angle), s = sin(angle);
	for (int i = 0; i < 3; i++)
	{
		double na = a[i] * c + b[i] * s, nb = b[i] * c - a[i] * s;
		a[i] = na;
		b[i] = nb;
	}
}

/* Keeps the axes square after many small rotations */
static void orthonormalise(void)
{
	double *f = axis[FORWARD], *r = axis[RIGHT], *u = axis[UP];
	normalise(f);
	double fr = dot(f, r);
	for (int i = 0; i < 3; i++)
		r[i] -= f[i] * fr;
	normalise(r);
	double fu = dot(f, u), ru = dot(r, u);
	for (int i = 0; i < 3; i++)
		u[i] -= f[i] * fu + r[i] * ru;
	normalise(u);
}

/* =========================================================================
 * Game side
 * ========================================================================= */
static void write_state(void)
{
	/* the matrix is row major; its columns are right, up, forward */
	for (int row = 0; row < 3; row++)
		for (int col = 0; col < 3; col++)
		{
			int w = (int)lrint(axis[col][row] * 32767.0);
			wrword(STATE_MATRIX + 2 * (row * 3 + col), w);
		}
	for (int i = 0; i < 3; i++)
		wrlong(STATE_OFFSET + 4 * i, (int)lrint(pos[i]));
	wrbyte(STATE_ACTIVE, 1);
}

static void start(void)
{
	u32 ship = game_player_ship();
	for (int row = 0; row < 3; row++)
		for (int col = 0; col < 3; col++)
			axis[col][row] = (s16)rdword(ship + 2 * (row * 3 + col)) / 32767.0;
	orthonormalise();

	/* start behind the ship, like the external view */
	for (int i = 0; i < 3; i++)
		pos[i] = -axis[FORWARD][i] * SHIP_DISTANCE * 1.5;

	saved_time_accel = rdlong(GAME_A6 + A6_TIME_ACCEL);
	saved_view = (u8)rdbyte(GAME_A6 + A6_3DVIEW_MODE);
	speed_level = 0;
	time_level = 0;
	pending_key = 0;
	look_dx = look_dy = 0;
	mouse_buttons = 0;
	memset(pointers, 0, sizeof(pointers));
	for (int i = 0; i < FC_BUTTONS; i++)
		touch.button[i].held = false;
	touch.stick_active = false;
	last_ticks = SDL_GetTicks();
	active = true;
	write_state();
	// log_printf("Free camera on\n");
}

static void stop(void)
{
	wrbyte(STATE_ACTIVE, 0);
	/* held: the rate it had; running: the game's own */
	if (!time_level)
		wrlong(GAME_A6 + A6_TIME_ACCEL, saved_time_accel);
	wrbyte(GAME_A6 + A6_3DVIEW_MODE, saved_view);
	SDL_SetRelativeMouseMode(SDL_FALSE);
	active = false;
	// log_printf("Free camera off\n");
}

bool freecam_active(void)
{
	return active;
}

int freecam_speed_level(void)
{
	return speed_level;
}

/* What the game is doing: it may refuse (or its time guard take back) a rate, so once the
 * key has gone in its own rate index says (0, 3, 7, 10, 12 for 1x .. 10000x) */
int freecam_time_level(void)
{
	if (!time_level || pending_key)
		return time_level;
	int index = (s16)rdword(GAME_A6 + A6_RATE_INDEX);
	return index >= 12 ? 5 : index >= 10 ? 4 : index >= 7 ? 3 : index >= 3 ? 2 : 1;
}

bool freecam_toggle(void)
{
	if (active)
		stop();
	else if (game_screen() == SCREEN_FLIGHT && game_player_ship())
		start();
	return active;
}

/* =========================================================================
 * Input
 * ========================================================================= */
/* 0 holds the game, 1-5 run it at its rates */
static void set_time_level(int level)
{
	time_level = level;
	pending_key = level ? KEY_1X + level - 1 : 0;
}

static void change_speed(int steps)
{
	speed_level += steps;
	if (speed_level < SPEED_MIN)
		speed_level = SPEED_MIN;
	if (speed_level > SPEED_MAX)
		speed_level = SPEED_MAX;
}

/* =========================================================================
 * Touch
 * ========================================================================= */
static void place(int b, float x, float y, float size)
{
	touch.button[b].x = x;
	touch.button[b].y = y;
	touch.button[b].w = size;
	touch.button[b].h = size;
}

/* Lays the touch controls out in the game area */
static void touch_layout(void)
{
	float gx = (float)Screen_GetGameOffsetX(), gy = (float)Screen_GetGameOffsetY();
	float gw = (float)Screen_GetGameWidth(), gh = (float)Screen_GetGameHeight();
	float b = gh * 0.09f, m;
	if (b < 24.0f)
		b = 24.0f;
	m = b * 0.3f;

	place(FC_SPEED_DOWN, gx + m, gy + m, b);
	place(FC_SPEED_UP, gx + m + b * 4.0f, gy + m, b);
	place(FC_HELP, gx + gw - m - b * 2.3f, gy + m, b);
	place(FC_EXIT, gx + gw - m - b, gy + m, b);
	place(FC_UP, gx + gw - m - b, gy + gh * 0.42f, b);
	place(FC_DOWN, gx + gw - m - b, gy + gh * 0.42f + b * 1.3f, b);
	place(FC_ROLL_LEFT, gx + m, gy + gh - m - b, b);
	place(FC_ROLL_RIGHT, gx + gw - m - b, gy + gh - m - b, b);
	touch.stick_radius = gh * 0.14f;
	touch.speed_fraction = (float)(speed_level - SPEED_MIN) / (SPEED_MAX - SPEED_MIN);
}

static bool on_button(int b, float x, float y)
{
	const FreecamButton *r = &touch.button[b];
	return x >= r->x && x <= r->x + r->w && y >= r->y && y <= r->y + r->h;
}

/* An extra finger's pointer slot, -1 if it isn't one we follow */
static int find_finger(SDL_FingerID id)
{
	for (int i = MOUSE_POINTER + 1; i < MAX_POINTERS; i++)
		if (pointers[i].used && pointers[i].id == id)
			return i;
	return -1;
}

static void pointer_down(int slot, float x, float y)
{
	int role = -1;
	for (int b = 0; b < FC_BUTTONS && role < 0; b++)
		if (on_button(b, x, y))
			role = b;

	switch (role)
	{
	case FC_SPEED_DOWN:
		change_speed(-1);
		return;
	case FC_SPEED_UP:
		change_speed(1);
		return;
	case FC_HELP:
		freecam_show_help = !freecam_show_help;
		return;
	case FC_EXIT:
		stop();
		return;
	case -1:
		/* left half: the thumbstick starts where the pointer lands */
		if (x < Screen_GetGameOffsetX() + Screen_GetGameWidth() / 2 && !touch.stick_active)
		{
			role = ROLE_STICK;
			touch.stick_active = true;
			touch.stick_x = touch.knob_x = x;
			touch.stick_y = touch.knob_y = y;
		}
		else
		{
			role = ROLE_LOOK;
		}
		break;
	default: /* up, down, roll: held */
		touch.button[role].held = true;
		break;
	}
	pointers[slot].role = role;
	pointers[slot].used = true;
}

static void pointer_motion(int slot, float x, float y, float dx, float dy)
{
	if (!pointers[slot].used)
		return;
	if (pointers[slot].role == ROLE_LOOK)
	{
		look_dx += (int)lrintf(dx);
		look_dy += (int)lrintf(dy);
	}
	else if (pointers[slot].role == ROLE_STICK)
	{
		float ox = x - touch.stick_x, oy = y - touch.stick_y;
		float len = sqrtf(ox * ox + oy * oy), r = touch.stick_radius;
		if (len > r)
		{
			ox *= r / len;
			oy *= r / len;
		}
		touch.knob_x = touch.stick_x + ox;
		touch.knob_y = touch.stick_y + oy;
	}
}

static void pointer_up(int slot)
{
	if (!pointers[slot].used)
		return;
	int role = pointers[slot].role;
	if (role == ROLE_STICK)
		touch.stick_active = false;
	else if (role >= 0 && role < FC_BUTTONS)
		touch.button[role].held = false;
	pointers[slot].used = false;
}

/* With the touch controls on, the mouse presses them */
static bool touch_mouse_event(const SDL_Event *event)
{
	touch_layout();
	switch (event->type)
	{
	case SDL_MOUSEBUTTONDOWN:
		if (event->button.button == SDL_BUTTON_LEFT)
			pointer_down(MOUSE_POINTER, (float)event->button.x, (float)event->button.y);
		break;
	case SDL_MOUSEMOTION:
		pointer_motion(MOUSE_POINTER, (float)event->motion.x, (float)event->motion.y,
					   (float)event->motion.xrel, (float)event->motion.yrel);
		break;
	case SDL_MOUSEBUTTONUP:
		if (event->button.button == SDL_BUTTON_LEFT)
			pointer_up(MOUSE_POINTER);
		break;
	}
	return true;
}

/* Fingers after the first (the first one is the mouse) */
static bool touch_finger_event(const SDL_Event *event)
{
	touch_layout();
	float x = event->tfinger.x * screen_w, y = event->tfinger.y * screen_h;
	int slot = find_finger(event->tfinger.fingerId);
	switch (event->type)
	{
	case SDL_FINGERDOWN:
		if (SDL_GetNumTouchFingers(event->tfinger.touchId) < 2)
			break;
		for (int i = MOUSE_POINTER + 1; slot < 0 && i < MAX_POINTERS; i++)
			if (!pointers[i].used)
				slot = i;
		if (slot < 0)
			break;
		pointers[slot].id = event->tfinger.fingerId;
		pointer_down(slot, x, y);
		break;
	case SDL_FINGERMOTION:
		if (slot >= 0)
			pointer_motion(slot, x, y, event->tfinger.dx * screen_w, event->tfinger.dy * screen_h);
		break;
	case SDL_FINGERUP:
		if (slot >= 0)
			pointer_up(slot);
		break;
	}
	return true;
}

bool freecam_touch_view(FreecamTouchView *view)
{
	if (!active || !toggle_touch_controls)
		return false;
	touch_layout();
	*view = touch;
	return true;
}

bool freecam_handle_event(const SDL_Event *event)
{
	if (event->type == SDL_KEYDOWN && event->key.keysym.sym == SDLK_v && (event->key.keysym.mod & KMOD_CTRL))
	{
		freecam_toggle();
		return true;
	}
	if (!active)
		return false;

	if (event->type == SDL_FINGERDOWN || event->type == SDL_FINGERUP || event->type == SDL_FINGERMOTION)
	{
		toggle_touch_controls = true; /* as the main event loop does */
		return touch_finger_event(event);
	}
	if (toggle_touch_controls && (event->type == SDL_MOUSEBUTTONDOWN || event->type == SDL_MOUSEBUTTONUP ||
								  event->type == SDL_MOUSEMOTION))
		return touch_mouse_event(event);

	switch (event->type)
	{
	case SDL_KEYDOWN:
		switch (event->key.keysym.sym)
		{
		case SDLK_ESCAPE:
			stop();
			break;
		case SDLK_h:
			freecam_show_help = !freecam_show_help;
			break;
		case SDLK_0:
		case SDLK_KP_0:
		case SDLK_p:
			set_time_level(0);
			break;
		case SDLK_1:
		case SDLK_2:
		case SDLK_3:
		case SDLK_4:
		case SDLK_5:
			set_time_level(event->key.keysym.sym - SDLK_0);
			break;
		case SDLK_KP_1:
		case SDLK_KP_2:
		case SDLK_KP_3:
		case SDLK_KP_4:
		case SDLK_KP_5:
			set_time_level(event->key.keysym.sym - SDLK_KP_1 + 1);
			break;
		case SDLK_EQUALS:
		case SDLK_PLUS:
		case SDLK_KP_PLUS:
			change_speed(1);
			break;
		case SDLK_MINUS:
		case SDLK_KP_MINUS:
			change_speed(-1);
			break;
		default:
			break;
		}
		return true;
	case SDL_KEYUP:
	case SDL_TEXTINPUT:
		return true;
	case SDL_MOUSEWHEEL:
		change_speed(event->wheel.y > 0 ? 1 : event->wheel.y < 0 ? -1 : 0);
		return true;
	case SDL_MOUSEBUTTONDOWN:
		mouse_buttons |= 1 << event->button.button;
		SDL_SetRelativeMouseMode(SDL_TRUE);
		return true;
	case SDL_MOUSEBUTTONUP:
		mouse_buttons &= ~(1 << event->button.button);
		if (!mouse_buttons)
			SDL_SetRelativeMouseMode(SDL_FALSE);
		return true;
	case SDL_MOUSEMOTION:
		if (mouse_buttons)
		{
			look_dx += event->motion.xrel;
			look_dy += event->motion.yrel;
		}
		return true;
	default:
		return false;
	}
}

/* =========================================================================
 * Per tick
 * ========================================================================= */
void freecam_update(void)
{
	if (!active)
		return;
	if (game_screen() != SCREEN_FLIGHT || !game_player_ship())
	{
		stop();
		return;
	}

	Uint32 now = SDL_GetTicks();
	double dt = (now - last_ticks) / 1000.0;
	last_ticks = now;
	if (dt > 0.1)
		dt = 0.1;

	const Uint8 *key = SDL_GetKeyboardState(NULL);
	bool fast = (SDL_GetModState() & KMOD_SHIFT) != 0;

	/* look */
	double yaw = (key[SDL_SCANCODE_RIGHT] - key[SDL_SCANCODE_LEFT]) * TURN_RATE * dt + look_dx * MOUSE_RATE;
	double pitch = (key[SDL_SCANCODE_UP] - key[SDL_SCANCODE_DOWN]) * TURN_RATE * dt - look_dy * MOUSE_RATE;
	double roll = ((key[SDL_SCANCODE_E] || touch.button[FC_ROLL_RIGHT].held) -
				   (key[SDL_SCANCODE_Q] || touch.button[FC_ROLL_LEFT].held)) *
				  TURN_RATE * dt;
	look_dx = look_dy = 0;
	if (freecam_invert_y)
		pitch = -pitch;
	rotate(axis[FORWARD], axis[RIGHT], yaw);
	rotate(axis[FORWARD], axis[UP], pitch);
	rotate(axis[UP], axis[RIGHT], roll);
	orthonormalise();

	/* move */
	double forward = key[SDL_SCANCODE_W] - key[SDL_SCANCODE_S];
	double right = key[SDL_SCANCODE_D] - key[SDL_SCANCODE_A];
	double up = (key[SDL_SCANCODE_R] || key[SDL_SCANCODE_PAGEUP] || touch.button[FC_UP].held) -
				(key[SDL_SCANCODE_F] || key[SDL_SCANCODE_PAGEDOWN] || touch.button[FC_DOWN].held);
	if (touch.stick_active && touch.stick_radius > 0)
	{
		/* further from the centre is faster */
		right += (touch.knob_x - touch.stick_x) / touch.stick_radius;
		forward -= (touch.knob_y - touch.stick_y) / touch.stick_radius;
	}
	double speed = SHIP_DISTANCE * ldexp(1.0, speed_level) * (fast ? 8.0 : 1.0) * dt;
	for (int i = 0; i < 3; i++)
	{
		pos[i] += (axis[FORWARD][i] * forward + axis[RIGHT][i] * right + axis[UP][i] * up) * speed;
		if (pos[i] > MAX_OFFSET)
			pos[i] = MAX_OFFSET;
		if (pos[i] < -MAX_OFFSET)
			pos[i] = -MAX_OFFSET;
	}

	/* a rate asked for goes in as the game's own key, when its queue is free */
	if (pending_key && !(rdbyte(GAME_A6 + A6_MODAL) & 1) && !rdbyte(GAME_A6 + A6_SCANCODE_ASCII))
	{
		wrbyte(GAME_A6 + A6_PUSHED_ASCII, pending_key);
		wrbyte(GAME_A6 + A6_SCANCODE_ASCII, 0xff);
		pending_key = 0;
	}
	/* held, and out of the cockpit */
	if (!time_level)
		wrlong(GAME_A6 + A6_TIME_ACCEL, 0);
	/* With the cockpit renderer on, the free camera is seen through it (wide, the
	 * panel and head tracking as in the front view); the classic layout, the
	 * external view, without */
	wrbyte(GAME_A6 + A6_3DVIEW_MODE, cockpit_enabled && use_renderer != R_OLD ? VIEW_FRONT : VIEW_EXTERNAL);
	write_state();
}

#else /* !FE2_USE_MODDED */

bool freecam_active(void)
{
	return false;
}

int freecam_speed_level(void)
{
	return 0;
}

int freecam_time_level(void)
{
	return 0;
}

bool freecam_toggle(void)
{
	return false;
}

bool freecam_handle_event(const SDL_Event *event)
{
	(void)event;
	return false;
}

void freecam_update(void) {}

bool freecam_touch_view(FreecamTouchView *view)
{
	(void)view;
	return false;
}

#endif /* FE2_USE_MODDED */
