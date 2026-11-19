/*
 * keymap.c - maps SDL key presses to Atari ST scancodes for the game.
 * Ctrl+key combinations are passed to the shortcuts instead.
 *
 * In flight some letters are the port's flight keys instead of the game's
 * (see FlightKeys below).
 */
#include <SDL_keyboard.h>

#include "freecam.h"
#include "game_state.h"
#include "gl/gl_cockpit.h"
#include "input.h"
#include "keymap.h"
#include "main.h"
#include "shortcut.h"

/*-----------------------------------------------------------------------*/
/* The flight keys: while flying, these letters press the game's own
 * flight keys (or the port's strafe) instead of their letters, which the
 * game would otherwise see. Taken from the game this way:
 *   E  was ECM (game key e)        -> accelerate
 *   D  was the photo of a military reconnaissance job (game key d, L64184)
 *                                  -> thrust right
 *   A  was pitch (game key A, nose down) -> thrust left
 * The game's other steering and speed keys still work: Z (pitch, nose up),
 * , and . (roll or yaw as the Elite controls option says), Return and
 * Right Shift (speed up / down), and the keypad's strafe. */
enum
{
	STEER_NONE,
	STEER_ROLL, /* the Elite controls option is set to roll while held */
	STEER_YAW,  /* ... to yaw */
};

typedef struct
{
	SDL_Keycode sym;
	uint8_t st;    /* game key it presses, 0xFF: none (strafe, read by strafe.c) */
	uint8_t steer; /* STEER_* */
	int8_t sx, sy; /* strafe: x right +, y up + */
} FlightKey;

static const FlightKey FlightKeys[] = {
	{SDLK_q, 0x36, STEER_NONE, 0, 0},  /* decelerate: Right Shift */
	{SDLK_e, 0x1C, STEER_NONE, 0, 0},  /* accelerate: Return */
	{SDLK_w, 0xFF, STEER_NONE, 0, 1},  /* thrust up */
	{SDLK_s, 0xFF, STEER_NONE, 0, -1}, /* thrust down */
	{SDLK_a, 0xFF, STEER_NONE, -1, 0}, /* thrust left */
	{SDLK_d, 0xFF, STEER_NONE, 1, 0},  /* thrust right */
	{SDLK_u, 0x33, STEER_YAW, 0, 0},   /* yaw left: , */
	{SDLK_o, 0x34, STEER_YAW, 0, 0},   /* yaw right: . */
	{SDLK_i, 0x1E, STEER_NONE, 0, 0},  /* pitch down (nose down): A */
	{SDLK_k, 0x2C, STEER_NONE, 0, 0},  /* pitch up (nose up): Z */
	{SDLK_j, 0x33, STEER_ROLL, 0, 0},  /* roll left: , */
	{SDLK_l, 0x34, STEER_ROLL, 0, 0},  /* roll right: . */
};
#define N_FLIGHT_KEYS (int)(sizeof FlightKeys / sizeof FlightKeys[0])

static volatile uint8_t flight_held[N_FLIGHT_KEYS];

static int flight_key_index(SDL_Keycode sym)
{
	for (int i = 0; i < N_FLIGHT_KEYS; i++)
		if (FlightKeys[i].sym == sym)
			return i;
	return -1;
}

/* Flying: the flight view, or a map / ship page on a cockpit screen while
 * the ship flies on. Not the station's or the options screen (the quiz's
 * letter, save names), the free camera (its own WASD) or the title. */
static bool flight_keys_on(void)
{
	if (freecam_active() || !game_player_ship())
		return false;
	GameScreen s = game_screen();
	if (s == SCREEN_FLIGHT)
		return true;
	return cockpit_screen_frame() && (s == SCREEN_GALAXY_MAP || s == SCREEN_SYSTEM_MAP || s == SCREEN_SHIP);
}

/* Presses or lets go a game key that several flight keys share: it is down
 * while any of them is held */
static void flight_update_st(uint8_t st)
{
	if (st == 0xFF)
		return;
	bool down = false;
	for (int i = 0; i < N_FLIGHT_KEYS; i++)
		if (FlightKeys[i].st == st && flight_held[i])
			down = true;
	Input_PressSTKey(st, down);
}

/* True if the key was taken as a flight key */
static bool flight_key(SDL_Keycode sym, bool down)
{
	int i = flight_key_index(sym);
	if (i < 0)
		return false;
	if (down)
	{
		if (!flight_held[i] && !flight_keys_on())
			return false;
		flight_held[i] = 1;
	}
	else
	{
		if (!flight_held[i])
			return false; /* pressed as a letter: let it go as one */
		flight_held[i] = 0;
	}
	flight_update_st(FlightKeys[i].st);
	return true;
}

int Keymap_SteerMode(void)
{
	int mode = STEER_NONE;
	for (int i = 0; i < N_FLIGHT_KEYS; i++)
		if (flight_held[i] && FlightKeys[i].steer > mode)
			mode = FlightKeys[i].steer;
	return mode;
}

void Keymap_Strafe(float *x, float *y)
{
	int sx = 0, sy = 0;
	for (int i = 0; i < N_FLIGHT_KEYS; i++)
		if (flight_held[i])
		{
			sx += FlightKeys[i].sx;
			sy += FlightKeys[i].sy;
		}
	*x = (float)sx;
	*y = (float)sy;
}

/*-----------------------------------------------------------------------*/
/* SDL Keycode to ST scancode mapping table */
typedef struct
{
	SDL_Keycode sdl_key;
	uint8_t st_scancode;
} KeyMapping;

static const KeyMapping KeyMappingTable[] = {
	/* Main keyboard */
	{SDLK_BACKSPACE, 0x0E},
	{SDLK_TAB, 0x0F},
	{SDLK_RETURN, 0x1C},
	{SDLK_ESCAPE, 0x01},
	{SDLK_SPACE, 0x39},
	{SDLK_QUOTE, 0x28},
	{SDLK_COMMA, 0x33},
	{SDLK_MINUS, 0x0C},
	{SDLK_PERIOD, 0x34},
	{SDLK_SLASH, 0x35},
	{SDLK_SEMICOLON, 0x27},
	{SDLK_EQUALS, 0x0D},
	{SDLK_LEFTBRACKET, 0x1A},
	{SDLK_BACKSLASH, 0x2B},
	{SDLK_RIGHTBRACKET, 0x1B},
	{SDLK_BACKQUOTE, 0x29},

	/* Numbers */
	{SDLK_0, 0x0B},
	{SDLK_1, 0x02},
	{SDLK_2, 0x03},
	{SDLK_3, 0x04},
	{SDLK_4, 0x05},
	{SDLK_5, 0x06},
	{SDLK_6, 0x07},
	{SDLK_7, 0x08},
	{SDLK_8, 0x09},
	{SDLK_9, 0x0A},

	/* Letters */
	{SDLK_a, 0x1E},
	{SDLK_b, 0x30},
	{SDLK_c, 0x2E},
	{SDLK_d, 0x20},
	{SDLK_e, 0x12},
	{SDLK_f, 0x21},
	{SDLK_g, 0x22},
	{SDLK_h, 0x23},
	{SDLK_i, 0x17},
	{SDLK_j, 0x24},
	{SDLK_k, 0x25},
	{SDLK_l, 0x26},
	{SDLK_m, 0x32},
	{SDLK_n, 0x31},
	{SDLK_o, 0x18},
	{SDLK_p, 0x19},
	{SDLK_q, 0x10},
	{SDLK_r, 0x13},
	{SDLK_s, 0x1F},
	{SDLK_t, 0x14},
	{SDLK_u, 0x16},
	{SDLK_v, 0x2F},
	{SDLK_w, 0x11},
	{SDLK_x, 0x2D},
	{SDLK_y, 0x15},
	{SDLK_z, 0x2C},

	/* Function keys */
	{SDLK_F1, 0x3B},
	{SDLK_F2, 0x3C},
	{SDLK_F3, 0x3D},
	{SDLK_F4, 0x3E},
	{SDLK_F5, 0x3F},
	{SDLK_F6, 0x40},
	{SDLK_F7, 0x41},
	{SDLK_F8, 0x42},
	{SDLK_F9, 0x43},
	{SDLK_F10, 0x44},

	/* Modifiers */
	{SDLK_LSHIFT, 0x2A},
	{SDLK_RSHIFT, 0x36},
	{SDLK_LCTRL, 0x1D},
	{SDLK_RCTRL, 0x1D},
	{SDLK_LALT, 0x38},
	{SDLK_RALT, 0x38},
	{SDLK_CAPSLOCK, 0x3A},

	/* Arrow keys */
	{SDLK_UP, 0x48},
	{SDLK_DOWN, 0x50},
	{SDLK_LEFT, 0x4B},
	{SDLK_RIGHT, 0x4D},

	/* Special keys */
	{SDLK_INSERT, 0x52},
	{SDLK_HOME, 0x47},
	{SDLK_END, 0x4F},
	{SDLK_PAGEUP, 0x49},
	{SDLK_PAGEDOWN, 0x51},
	{SDLK_DELETE, 0x53},
	{SDLK_PRINTSCREEN, 0x54},
	{SDLK_SCROLLLOCK, 0x46},
	{SDLK_PAUSE, 0x45},

	/* Keypad (NumLock on) */
	{SDLK_KP_0, 0x70},
	{SDLK_KP_1, 0x6D},
	{SDLK_KP_2, 0x6E},
	{SDLK_KP_3, 0x6F},
	{SDLK_KP_4, 0x6A},
	{SDLK_KP_5, 0x6B},
	{SDLK_KP_6, 0x6C},
	{SDLK_KP_7, 0x67},
	{SDLK_KP_8, 0x68},
	{SDLK_KP_9, 0x69},
	{SDLK_KP_PERIOD, 0x71},
	{SDLK_KP_DIVIDE, 0x65},
	{SDLK_KP_MULTIPLY, 0x66},
	{SDLK_KP_MINUS, 0x4A},
	{SDLK_KP_PLUS, 0x4E},
	{SDLK_KP_ENTER, 0x72},

	/* Terminator */
	{SDLK_UNKNOWN, 0xFF}};

void Keymap_Init(void)
{
	/* nothing to set up: the mapping table is static */
}

/*-----------------------------------------------------------------------*/
/*
  Get ST scancode from SDL key event
*/
static uint8_t Keymap_GetSTScanCode(SDL_Keysym *keysym)
{
	/* keypad with NumLock off works as cursor keys */
	if (keysym->sym >= SDLK_KP_1 && keysym->sym <= SDLK_KP_9)
	{
		if (!(SDL_GetModState() & KMOD_NUM))
		{
			/* NumLock off - use alternate mappings */
			switch (keysym->sym)
			{
			case SDLK_KP_1:
				return 0x6D; /* End */
			case SDLK_KP_2:
				return 0x50; /* Down */
			case SDLK_KP_3:
				return 0x6F; /* Page Down */
			case SDLK_KP_4:
				return 0x4B; /* Left */
			case SDLK_KP_6:
				return 0x4D; /* Right */
			case SDLK_KP_7:
				return 0x47; /* Home */
			case SDLK_KP_8:
				return 0x48; /* Up */
			case SDLK_KP_9:
				return 0x49; /* Page Up */
			default:
				break;
			}
		}
	}

	/* Lookup in table */
	for (int i = 0; KeyMappingTable[i].sdl_key != SDLK_UNKNOWN; i++)
	{
		if (KeyMappingTable[i].sdl_key == keysym->sym)
		{
			return KeyMappingTable[i].st_scancode;
		}
	}

	return 0xFF; /* Not found */
}

/*-----------------------------------------------------------------------*/
/*
  Key down event
*/
void Keymap_KeyDown(SDL_Keysym *sdlkey)
{
	uint8_t st_code;
	int symkey = sdlkey->sym;
	int modkey = sdlkey->mod;

	/* Handle shortcut keys */
	if ((modkey & KMOD_MODE) || (modkey & KMOD_RGUI) || (modkey & KMOD_CTRL))
	{
		ShortCutKey.Key = symkey;
		if (modkey & (KMOD_LCTRL | KMOD_RCTRL))
			ShortCutKey.bCtrlPressed = TRUE;
		if (modkey & (KMOD_LSHIFT | KMOD_RSHIFT))
			ShortCutKey.bShiftPressed = TRUE;
		return;
	}

	/* Ignore keys we don't want to process */
	if (symkey == SDLK_MODE || symkey == SDLK_LGUI || symkey == SDLK_NUMLOCKCLEAR)
	{
		return;
	}

	if (flight_key(symkey, true))
		return;

	/* Get ST scancode */
	st_code = Keymap_GetSTScanCode(sdlkey);
	if (st_code != 0xFF)
	{
		Input_PressSTKey(st_code, TRUE);
	}
}

/*-----------------------------------------------------------------------*/
/*
  Key up event
*/
void Keymap_KeyUp(SDL_Keysym *sdlkey)
{
	uint8_t st_code;
	int symkey = sdlkey->sym;

	/* Handle special cases */
	if (symkey == SDLK_CAPSLOCK)
	{
		/* Simulate capslock toggle */
		Input_PressSTKey(0x3A, TRUE);
		return;
	}

	/* Ignore keys we don't want to process */
	if (symkey == SDLK_MODE || symkey == SDLK_LGUI || symkey == SDLK_NUMLOCKCLEAR)
	{
		return;
	}

	if (flight_key(symkey, false))
		return;

	/* Get ST scancode */
	st_code = Keymap_GetSTScanCode(sdlkey);
	if (st_code != 0xFF)
	{
		Input_PressSTKey(st_code, FALSE);
	}
}

/*-----------------------------------------------------------------------*/
/*
  Debounce all keys (called each frame)
*/
void Keymap_DebounceAllKeys(void)
{
	/* Not needed in this implementation */
}