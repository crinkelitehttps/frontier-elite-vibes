/*
 * gamepad.c - see gamepad.h. The binding format follows the F-19 project's
 * gamepad.cfg (native/src/host/gamepad.cpp).
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gamepad.h"
#include "game_state.h"
#include "gl/gl_clipboard.h"
#include "gl/gl_cockpit.h"
#include "headtrack.h"
#include "host.h"
#include "input.h"
#include "keymap.h"
#include "main.h"
#include "manual.h"
#include "strafe.h"
#include "vr.h"

/* The built-in bindings, also written out as gamepad.cfg */
static const char DEFAULT_CFG[] =
	"# GLFrontier gamepad bindings (Xbox layout names; other controllers map\n"
	"# onto the same buttons by position).\n"
	"#\n"
	"# Delete this file to get the defaults back.\n"
	"#\n"
	"# One binding per line:   [manual] [shift1|shift2] <control> <action>\n"
	"# A control may have several lines; '#' starts a comment.\n"
	"#\n"
	"# Shift layers: a control bound to `shift1` or `shift2` is held to switch\n"
	"# key bindings to the ones prefixed with that shift (and off the\n"
	"# unprefixed ones). Holding both reaches neither. The layer counts when a\n"
	"# control is pressed. Axis bindings (steer, roll, yaw, look, strafe) act\n"
	"# whatever the shifts, unless the axis has one prefixed with the shift\n"
	"# being held.\n"
	"#\n"
	"# The manual layer: a binding prefixed `manual` acts only while the\n"
	"# clipboard is out (whatever the shifts), and its control's other\n"
	"# bindings don't then. Only key, cycle, recenter, cockpit, pointer,\n"
	"# manual and manual-page bindings; axes keep flying the ship.\n"
	"#\n"
	"# Controls (SDL names):\n"
	"#   buttons   a b x y back guide start leftstick rightstick\n"
	"#             leftshoulder rightshoulder dpup dpdown dpleft dpright\n"
	"#             misc1 paddle1 paddle2 paddle3 paddle4 touchpad\n"
	"#   axes      leftx lefty rightx righty lefttrigger righttrigger\n"
	"#   half axes leftx- leftx+ lefty- lefty+ ... (- is left / up), usable as\n"
	"#             buttons; the triggers work as buttons too\n"
	"#\n"
	"# VR motion controllers are read as this pad too. Left hand: thumbstick\n"
	"# = left stick (click: leftstick), trigger = lefttrigger, grip =\n"
	"# leftshoulder, trackpad click = the d-pad (by where it is pressed),\n"
	"# menu = back, X / Y buttons = x / y. Right hand: thumbstick = right\n"
	"# stick (click: rightstick), trigger = righttrigger, grip =\n"
	"# rightshoulder, trackpad click = y / a / x / b (top, bottom, left,\n"
	"# right), menu = start, A / B buttons = a / b. A trigger pulled with its\n"
	"# beam on the panel or a screen, or with the grip held, is a mouse click\n"
	"# instead.\n"
	"#\n"
	"# On the galaxy map the sticks scroll it (the arrow keys) instead of\n"
	"# doing what they are bound to (the right one not while it moves the\n"
	"# pointer).\n"
	"#\n"
	"# Actions:\n"
	"#   steer-x, steer-y [invert]  the ship's steering (full axes only):\n"
	"#                              x turns (rolls with Elite controls), y pitches\n"
	"#   roll, yaw [invert]         steer-x, but rolling / yawing whatever the\n"
	"#                              game's Elite controls option (full axes)\n"
	"#   look-x, look-y [invert]    look around in the cockpit (springs back)\n"
	"#   strafe-x, strafe-y [invert] sideways / vertical thrust (full axes;\n"
	"#                              x right +, y up +, so lefty wants invert)\n"
	"#   strafe left|right|up|down  the same, full thrust while held\n"
	"#   key <key>                  a keyboard key while held, with optional\n"
	"#                              modifiers: shift+f1, ctrl+k\n"
	"#   cycle <key> <key> ...      each press sends the next key in turn\n"
	"#   shift1, shift2             hold for that shift layer\n"
	"#   recenter                   head tracking: the current pose is ahead\n"
	"#   cockpit                    cockpit view on / off (also Ctrl-K)\n"
	"#   pointer                    a mouse pointer for the control panel and\n"
	"#                              menus: press for it, press again to click\n"
	"#                              there (left button) and put it away (not\n"
	"#                              with VR motion controllers: they point)\n"
	"#   pointer-x, pointer-y [invert] move it (full axes); while it is out,\n"
	"#                              these axes do nothing else\n"
	"#   manual                     the clipboard (controls and the game\n"
	"#                              manual): up to read / put away (also F11)\n"
	"#   manual-page <n>            turn n pages (-n: back); repeats while held\n"
	"#   Key names as SDL spells them: a-z 0-9 f1-f10 = - , . / ; return space\n"
	"#   escape tab backspace up down left right home end pageup pagedown\n"
	"#   insert delete \"left shift\" \"right shift\" (aliases: enter esc lshift\n"
	"#   rshift pgup pgdn ins del)\n"
	"#\n"
	"# Settings:\n"
	"#   deadzone 0.15              stick deadzone (fraction of full travel)\n"
	"#   threshold 0.5              how far an axis must go to act as a button\n"
	"#   look-yaw 100               look-around range, degrees\n"
	"#   look-pitch 60\n"
	"#   pointer-speed 200          pointer speed, screen pixels (of 320) a second\n"
	"\n"
	"deadzone   0.15\n"
	"threshold  0.5\n"
	"look-yaw   100\n"
	"look-pitch 60\n"
	"pointer-speed 200\n"
	"\n"
	"# ---- Flying\n"
	"leftx          strafe-x            # thrust left / right\n"
	"lefty          strafe-y invert     # thrust up / down\n"
	"rightx         roll\n"
	"righty         steer-y invert      # pitch: push forward to dive (drop invert to climb)\n"
	"shift2 rightx  yaw                 # right shoulder held: yaw instead of roll\n"
	"leftshoulder   key return          # forward: set speed up, or main thrust (hold)\n"
	"lefttrigger    key rshift          # back: set speed down, or retro thrust (hold)\n"
	"righttrigger   key space           # fire\n"
	"b              key f1              # flight view: front, rear, turrets, external\n"
	"y              cockpit             # cockpit view on / off\n"
	"start          key escape          # pause\n"
	"leftstick      pointer             # pointer for the panel: press, aim (right stick), press to click\n"
	"rightx         pointer-x\n"
	"righty         pointer-y\n"
	"rightstick     recenter            # head tracking / VR: this pose is straight ahead\n"
	"dpup           key =               # zoom in (maps, external view)\n"
	"dpdown         key -               # zoom out\n"
	"\n"
	"# ---- Back held: the console's function keys, laid out like the panel\n"
	"# (F1-F4 on the left, F6-F10 on the right)\n"
	"back           shift1\n"
	"shift1 dpleft  key f1\n"
	"shift1 dpup    key f2\n"
	"shift1 dpright key f3\n"
	"shift1 dpdown  key f4\n"
	"shift1 leftstick key f5\n"
	"shift1 x       key f6\n"
	"shift1 y       key f7\n"
	"shift1 b       key f8\n"
	"shift1 a       key f9\n"
	"shift1 start   key f10\n"
	"\n"
	"# ---- Right shoulder held: yaw (above) and time acceleration\n"
	"rightshoulder  shift2\n"
	"shift2 dpdown  key shift+f1        # normal time\n"
	"shift2 dpleft  key shift+f2\n"
	"shift2 dpup    key shift+f3\n"
	"shift2 dpright key shift+f4\n"
	"shift2 y       key shift+f5        # fastest\n"
	"\n"
	"# ---- The clipboard: Right shoulder + A (VR: right grip + right trackpad\n"
	"# down), or grab it with a VR hand. While it is out the d-pad (VR: left\n"
	"# trackpad) turns pages.\n"
	"shift2 a       manual              # clipboard: controls and manual\n"
	"manual dpleft  manual-page -1      # previous page\n"
	"manual dpright manual-page +1      # next page\n"
	"manual dpup    manual-page -10     # 10 pages back\n"
	"manual dpdown  manual-page +10     # 10 pages on\n";

/* =========================================================================
 * Bindings
 * ========================================================================= */
typedef enum
{
	SRC_BUTTON,
	SRC_AXIS
} Source;

typedef struct
{
	Source source;
	int index; /* SDL_GameControllerButton or SDL_GameControllerAxis */
	int half;  /* axis as a button: -1 / +1; 0 the full axis (or a trigger) */
} Control;

typedef enum
{
	ACT_STEER_X,
	ACT_STEER_Y,
	ACT_ROLL,
	ACT_YAW,
	ACT_LOOK_X,
	ACT_LOOK_Y,
	ACT_STRAFE_X,
	ACT_STRAFE_Y,
	ACT_STRAFE, /* a button: strafe_axis, strafe_sign */
	ACT_KEY,
	ACT_CYCLE,
	ACT_SHIFT,
	ACT_RECENTER,
	ACT_COCKPIT,
	ACT_POINTER,   /* a button: pointer on, or click and off */
	ACT_POINTER_X, /* axes moving it while it is on */
	ACT_POINTER_Y,
	ACT_MANUAL,     /* the clipboard up / away */
	ACT_MANUAL_PAGE /* turn `pages` pages */
} Action;

#define MAX_CHORDS 8

typedef struct
{
	SDL_Keycode key;
	bool shift, ctrl;
} Chord;

typedef struct
{
	Control control;
	Action action;
	bool invert;
	int strafe_axis;   /* strafe: 0 x, 1 y */
	float strafe_sign; /* strafe: -1 / +1 */
	Chord keys[MAX_CHORDS];
	int n_keys;
	int step;  /* cycle: the next key */
	int layer; /* key, cycle, recenter, cockpit, steer, look, strafe: shift layer (0 none, 1, 2); shift: which */
	int axis_shadowed; /* axis actions without a layer: bit n set if shift n has its own binding */
	bool pointer_axis; /* other axis actions: the control moves the pointer while it is on */
	bool manual_layer; /* acts only while the clipboard is out */
	bool shadowed;     /* its control has a manual layer binding, which acts instead while it is out */
	int pages;         /* manual-page */
	bool gap;          /* a blank line before it in the file */
	char note[96];     /* the line's comment */

	bool was_down;  /* the control, last update */
	bool held;      /* key: sent down */
	Chord sent;     /* the chord sent down */
	bool repeating; /* manual-page: turning pages while held */
	Uint64 next_repeat;
} Binding;

#define MAX_BINDINGS 128

static Binding bindings[MAX_BINDINGS];
static int n_bindings;
static float deadzone = 0.15f, threshold = 0.5f;
static float look_yaw = 100.0f, look_pitch = 60.0f;
static float pointer_speed = 200.0f; /* screen pixels a second, stick right over */
static bool pointer_on;
static Uint64 pointer_ticks;

static SDL_GameController *pad;
/* The virtual pad: as set, and as this update reads it */
static SDL_SpinLock virtual_lock;
static GamepadVirtual virtual_in, vpad;
static unsigned virtual_pressed;
static float look[2];
static bool steering; /* wrote steering last update */
static int elite_saved = -1; /* the game's Elite controls option while roll / yaw force it, else -1 */
static bool arrows[4];       /* scrolling the galaxy map: up, down, left, right held */

/* ---- parsing ------------------------------------------------------------ */
static bool parse_control(const char *name, Control *c)
{
	char buf[64];
	snprintf(buf, sizeof buf, "%s", name);
	size_t len = strlen(buf);
	c->half = 0;
	if (len > 1 && (buf[len - 1] == '-' || buf[len - 1] == '+'))
	{
		c->half = buf[len - 1] == '-' ? -1 : 1;
		buf[len - 1] = 0;
	}
	SDL_GameControllerAxis axis = SDL_GameControllerGetAxisFromString(buf);
	if (axis != SDL_CONTROLLER_AXIS_INVALID)
	{
		c->source = SRC_AXIS;
		c->index = axis;
		return true;
	}
	SDL_GameControllerButton button = SDL_GameControllerGetButtonFromString(buf);
	if (button != SDL_CONTROLLER_BUTTON_INVALID && c->half == 0)
	{
		c->source = SRC_BUTTON;
		c->index = button;
		return true;
	}
	return false;
}

static bool parse_chord(const char *text, Chord *chord)
{
	static const struct
	{
		const char *alias, *name;
	} aliases[] = {{"enter", "Return"},   {"esc", "Escape"},     {"lshift", "Left Shift"}, {"rshift", "Right Shift"},
				   {"pgup", "PageUp"},    {"pgdn", "PageDown"}, {"ins", "Insert"},        {"del", "Delete"},
				   {"lctrl", "Left Ctrl"}, {"alt", "Left Alt"}};
	char buf[64];
	snprintf(buf, sizeof buf, "%s", text);
	char *name = buf;
	chord->shift = chord->ctrl = false;
	for (;;)
	{
		if (!SDL_strncasecmp(name, "shift+", 6) && name[6])
		{
			chord->shift = true;
			name += 6;
		}
		else if (!SDL_strncasecmp(name, "ctrl+", 5) && name[5])
		{
			chord->ctrl = true;
			name += 5;
		}
		else
			break;
	}
	for (size_t i = 0; i < sizeof aliases / sizeof aliases[0]; i++)
		if (!SDL_strcasecmp(name, aliases[i].alias))
			name = (char *)aliases[i].name;
	chord->key = SDL_GetKeyFromName(name);
	return chord->key != SDLK_UNKNOWN;
}

/* Splits a line into words; a quoted word may hold spaces. Returns the count. */
static int split(char *line, char **words, int max)
{
	int n = 0;
	char *p = line;
	while (*p && n < max)
	{
		while (isspace((unsigned char)*p))
			p++;
		if (!*p)
			break;
		if (*p == '"')
		{
			words[n++] = ++p;
			while (*p && *p != '"')
				p++;
		}
		else
		{
			words[n++] = p;
			while (*p && !isspace((unsigned char)*p))
				p++;
		}
		if (*p)
			*p++ = 0;
	}
	return n;
}

static bool is_axis_action(Action a)
{
	return a == ACT_STEER_X || a == ACT_STEER_Y || a == ACT_ROLL || a == ACT_YAW || a == ACT_LOOK_X ||
		   a == ACT_LOOK_Y || a == ACT_STRAFE_X || a == ACT_STRAFE_Y || a == ACT_POINTER_X || a == ACT_POINTER_Y;
}

/* Bindings that take a press (and may be in the manual layer) */
static bool is_press_action(Action a)
{
	return a == ACT_KEY || a == ACT_CYCLE || a == ACT_RECENTER || a == ACT_COCKPIT || a == ACT_POINTER ||
		   a == ACT_MANUAL || a == ACT_MANUAL_PAGE;
}

/* `gap`: a blank line came before, taken by the next binding */
static bool parse_line(char *line, const char *source, int line_no, bool *gap)
{
	char note[96] = "";
	char *hash = strchr(line, '#');
	if (hash)
	{
		*hash = 0;
		char *c = hash + 1;
		while (isspace((unsigned char)*c))
			c++;
		snprintf(note, sizeof note, "%s", c);
		for (size_t l = strlen(note); l > 0 && isspace((unsigned char)note[l - 1]); l--)
			note[l - 1] = 0;
	}
	char *w[16];
	int n = split(line, w, 16);
	if (n == 0)
	{
		*gap |= !hash;
		return true;
	}

	/* settings */
	if (n == 2)
	{
		float *setting = !strcmp(w[0], "deadzone")	  ? &deadzone
						 : !strcmp(w[0], "threshold")  ? &threshold
						 : !strcmp(w[0], "look-yaw")   ? &look_yaw
						 : !strcmp(w[0], "look-pitch") ? &look_pitch
						 : !strcmp(w[0], "pointer-speed") ? &pointer_speed
													   : NULL;
		if (setting)
		{
			*setting = (float)atof(w[1]);
			return true;
		}
	}

	Binding b;
	memset(&b, 0, sizeof b);
	int i = 0;
	if (!strcmp(w[0], "manual"))
	{
		b.manual_layer = true;
		i++;
	}
	if (i < n && (!strcmp(w[i], "shift1") || !strcmp(w[i], "shift2")))
	{
		if (b.manual_layer)
		{
			log_printf("%s:%d: manual layer bindings take no shift\n", source, line_no);
			return false;
		}
		b.layer = w[i][5] - '0';
		i++;
	}
	if (i + 2 > n || !parse_control(w[i], &b.control))
	{
		log_printf("%s:%d: unknown control '%s'\n", source, line_no, i < n ? w[i] : "");
		return false;
	}
	const char *act = w[i + 1];
	int args = i + 2;
	bool full_axis = b.control.source == SRC_AXIS && b.control.half == 0;

	if (!strcmp(act, "steer-x") || !strcmp(act, "steer-y") || !strcmp(act, "roll") || !strcmp(act, "yaw") ||
		!strcmp(act, "look-x") || !strcmp(act, "look-y") || !strcmp(act, "strafe-x") || !strcmp(act, "strafe-y") ||
		!strcmp(act, "pointer-x") || !strcmp(act, "pointer-y"))
	{
		b.action = !strcmp(act, "steer-x")	  ? ACT_STEER_X
				   : !strcmp(act, "steer-y")  ? ACT_STEER_Y
				   : !strcmp(act, "roll")	  ? ACT_ROLL
				   : !strcmp(act, "yaw")	  ? ACT_YAW
				   : !strcmp(act, "look-x")	  ? ACT_LOOK_X
				   : !strcmp(act, "look-y")	  ? ACT_LOOK_Y
				   : !strcmp(act, "strafe-x") ? ACT_STRAFE_X
				   : !strcmp(act, "strafe-y") ? ACT_STRAFE_Y
				   : !strcmp(act, "pointer-x") ? ACT_POINTER_X
											  : ACT_POINTER_Y;
		if (!full_axis)
		{
			log_printf("%s:%d: %s needs a full axis\n", source, line_no, act);
			return false;
		}
		b.invert = args < n && !strcmp(w[args], "invert");
	}
	else if (!strcmp(act, "strafe"))
	{
		static const char *const dirs[] = {"left", "right", "down", "up"};
		int d = 0;
		while (d < 4 && (args >= n || strcmp(w[args], dirs[d])))
			d++;
		if (d == 4)
		{
			log_printf("%s:%d: strafe takes left, right, up or down\n", source, line_no);
			return false;
		}
		b.action = ACT_STRAFE;
		b.strafe_axis = d / 2;
		b.strafe_sign = d % 2 ? 1.0f : -1.0f;
	}
	else if (!strcmp(act, "key") || !strcmp(act, "cycle"))
	{
		b.action = act[0] == 'k' ? ACT_KEY : ACT_CYCLE;
		for (int k = args; k < n && b.n_keys < MAX_CHORDS; k++)
		{
			if (!parse_chord(w[k], &b.keys[b.n_keys]))
			{
				log_printf("%s:%d: unknown key '%s'\n", source, line_no, w[k]);
				return false;
			}
			b.n_keys++;
		}
		if (b.n_keys == 0 || (b.action == ACT_KEY && b.n_keys != 1))
		{
			log_printf("%s:%d: %s takes %s\n", source, line_no, act, b.action == ACT_KEY ? "one key" : "keys");
			return false;
		}
	}
	else if (!strcmp(act, "shift1") || !strcmp(act, "shift2"))
	{
		if (b.layer)
		{
			log_printf("%s:%d: a shift can't be shifted\n", source, line_no);
			return false;
		}
		b.action = ACT_SHIFT;
		b.layer = act[5] - '0';
	}
	else if (!strcmp(act, "recenter") || !strcmp(act, "cockpit") || !strcmp(act, "pointer"))
	{
		b.action = act[0] == 'r' ? ACT_RECENTER : act[0] == 'c' ? ACT_COCKPIT : ACT_POINTER;
	}
	else if (!strcmp(act, "manual"))
	{
		b.action = ACT_MANUAL;
	}
	else if (!strcmp(act, "manual-page"))
	{
		char *end = NULL;
		long pages = args < n ? strtol(w[args], &end, 10) : 0;
		if (!pages || *end || pages < -1000 || pages > 1000)
		{
			log_printf("%s:%d: manual-page takes a page count, e.g. +1 or -10\n", source, line_no);
			return false;
		}
		b.action = ACT_MANUAL_PAGE;
		b.pages = (int)pages;
	}
	else
	{
		log_printf("%s:%d: unknown action '%s'\n", source, line_no, act);
		return false;
	}

	if (b.manual_layer && !is_press_action(b.action))
	{
		log_printf("%s:%d: only key, cycle, recenter, cockpit, pointer, manual and manual-page bindings can be in "
				   "the manual layer\n",
				   source, line_no);
		return false;
	}

	if (n_bindings >= MAX_BINDINGS)
	{
		log_printf("%s:%d: too many bindings\n", source, line_no);
		return false;
	}
	b.gap = *gap;
	*gap = false;
	snprintf(b.note, sizeof b.note, "%s", note);
	bindings[n_bindings++] = b;
	return true;
}

static bool same_control(const Control *a, const Control *b)
{
	return a->source == b->source && a->index == b->index && a->half == b->half;
}

static void mark_shadowed(void)
{
	for (int i = 0; i < n_bindings; i++)
	{
		Binding *b = &bindings[i];
		if (is_press_action(b->action) && !b->manual_layer)
			for (int j = 0; j < n_bindings; j++)
				if (bindings[j].manual_layer && same_control(&bindings[j].control, &b->control))
					b->shadowed = true;
		if (!is_axis_action(b->action))
			continue;
		if (b->action != ACT_POINTER_X && b->action != ACT_POINTER_Y)
			for (int j = 0; j < n_bindings; j++)
			{
				const Binding *o = &bindings[j];
				if ((o->action == ACT_POINTER_X || o->action == ACT_POINTER_Y) &&
					o->control.index == b->control.index && o->control.source == b->control.source)
					b->pointer_axis = true;
			}
		if (b->layer)
			continue;
		for (int j = 0; j < n_bindings; j++)
		{
			const Binding *o = &bindings[j];
			if (o->layer && is_axis_action(o->action) && o->control.index == b->control.index &&
				o->control.source == b->control.source)
				b->axis_shadowed |= 1 << o->layer;
		}
	}
}

static bool load_text(const char *text, const char *source)
{
	n_bindings = 0;
	bool ok = true, gap = false;
	int line_no = 0;
	const char *p = text;
	while (*p)
	{
		const char *end = strchr(p, '\n');
		size_t len = end ? (size_t)(end - p) : strlen(p);
		char line[256];
		snprintf(line, sizeof line, "%.*s", (int)(len < sizeof line ? len : sizeof line - 1), p);
		line_no++;
		ok = parse_line(line, source, line_no, &gap) && ok;
		p += len;
		if (*p)
			p++;
	}
	mark_shadowed();
	return ok;
}

static void load_bindings(void)
{
	FILE *f = fopen(GAMEPAD_FILE, "rb");
	if (!f)
	{
		load_text(DEFAULT_CFG, "built-in gamepad bindings");
		f = fopen(GAMEPAD_FILE, "wb");
		if (f)
		{
			fputs(DEFAULT_CFG, f);
			fclose(f);
			log_printf("Gamepad: wrote the default bindings to %s\n", GAMEPAD_FILE);
		}
		return;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *text = malloc((size_t)size + 1);
	if (!text)
	{
		fclose(f);
		return;
	}
	size_t got = fread(text, 1, (size_t)size, f);
	text[got] = 0;
	fclose(f);
	if (!load_text(text, GAMEPAD_FILE))
		log_printf("Gamepad: some lines of %s were skipped\n", GAMEPAD_FILE);
	else
		log_printf("Gamepad: %d bindings from %s\n", n_bindings, GAMEPAD_FILE);
	free(text);
}

/* =========================================================================
 * Controllers
 * ========================================================================= */
void gamepad_init(void)
{
	if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) < 0)
	{
		log_printf("Gamepad: %s\n", SDL_GetError());
		return;
	}
	load_bindings();
}

void gamepad_shutdown(void)
{
	if (pad)
		SDL_GameControllerClose(pad);
	pad = NULL;
}

bool gamepad_connected(void)
{
	return pad != NULL;
}

bool gamepad_handle_event(const SDL_Event *event)
{
	switch (event->type)
	{
	case SDL_CONTROLLERDEVICEADDED:
		if (!pad)
		{
			pad = SDL_GameControllerOpen(event->cdevice.which);
			if (pad)
				log_printf("Gamepad: %s\n", SDL_GameControllerName(pad));
		}
		return true;
	case SDL_CONTROLLERDEVICEREMOVED:
		if (pad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) == event->cdevice.which)
		{
			SDL_GameControllerClose(pad);
			pad = NULL;
			log_printf("Gamepad: disconnected\n");
			/* another one may still be plugged in */
			for (int i = 0; i < SDL_NumJoysticks() && !pad; i++)
				if (SDL_IsGameController(i))
					pad = SDL_GameControllerOpen(i);
		}
		return true;
	case SDL_CONTROLLERAXISMOTION:
	case SDL_CONTROLLERBUTTONDOWN:
	case SDL_CONTROLLERBUTTONUP:
		return true; /* read by polling in gamepad_update */
	}
	return false;
}

void gamepad_set_virtual(const GamepadVirtual *v)
{
	SDL_AtomicLock(&virtual_lock);
	virtual_pressed |= v->buttons & ~virtual_in.buttons;
	virtual_in = *v;
	SDL_AtomicUnlock(&virtual_lock);
}

static void take_virtual(void)
{
	SDL_AtomicLock(&virtual_lock);
	vpad = virtual_in;
	vpad.buttons |= virtual_pressed;
	virtual_pressed = 0;
	SDL_AtomicUnlock(&virtual_lock);
}

/* An axis, -1..1: the real pad's or the virtual one's, whichever is
 * further over */
static float raw_axis(int index)
{
	float p = pad ? SDL_GameControllerGetAxis(pad, (SDL_GameControllerAxis)index) / 32767.0f : 0.0f;
	float v = vpad.active ? vpad.axes[index] : 0.0f;
	v = fabsf(v) > fabsf(p) ? v : p;
	return fmaxf(-1.0f, fminf(1.0f, v));
}

/* -1..1, with the deadzone taken out (full axes) */
static float axis_value(const Control *c)
{
	float v = raw_axis(c->index);
	float a = fabsf(v);
	if (a < deadzone)
		return 0.0f;
	return copysignf((a - deadzone) / (1.0f - deadzone), v);
}

static bool control_down(const Control *c)
{
	if (c->source == SRC_BUTTON)
		return (pad && SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)c->index)) ||
			   (vpad.active && (vpad.buttons >> c->index & 1));
	float v = raw_axis(c->index);
	if (c->half < 0)
		return v < -threshold;
	return v > threshold; /* + half, or a trigger */
}

/* ---- keys --------------------------------------------------------------- */
static void send(SDL_Keycode key, Uint16 mod, bool down)
{
	SDL_Keysym ks;
	memset(&ks, 0, sizeof ks);
	ks.sym = key;
	ks.scancode = SDL_GetScancodeFromKey(key);
	ks.mod = mod;
	if (down)
		Keymap_KeyDown(&ks);
	else
		Keymap_KeyUp(&ks);
}

static void chord(const Chord *c, bool down)
{
	if (c->ctrl) /* the emulator's Ctrl shortcuts act on the press */
	{
		if (down)
			send(c->key, KMOD_LCTRL, true);
		return;
	}
	if (down)
	{
		if (c->shift)
			send(SDLK_LSHIFT, 0, true);
		send(c->key, 0, true);
	}
	else
	{
		send(c->key, 0, false);
		if (c->shift)
			send(SDLK_LSHIFT, 0, false);
	}
}

/* The galaxy map scrolls with the arrow keys: held while a stick is over
 * (x, y: -1..1, y down +) */
static void scroll_map(float x, float y)
{
	static const SDL_Keycode KEYS[4] = {SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT};
	bool want[4] = {y < -threshold, y > threshold, x < -threshold, x > threshold};
	for (int i = 0; i < 4; i++)
		if (want[i] != arrows[i])
		{
			send(KEYS[i], 0, want[i]);
			arrows[i] = want[i];
		}
}

static void release_all(void)
{
	scroll_map(0, 0);
	for (int i = 0; i < n_bindings; i++)
	{
		Binding *b = &bindings[i];
		if (b->held)
			chord(&b->sent, false);
		b->held = false;
		b->was_down = false;
		b->repeating = false;
	}
}

/* ---- per frame ---------------------------------------------------------- */
static int current_layer(void)
{
	bool s[3] = {false, false, false};
	for (int i = 0; i < n_bindings; i++)
		if (bindings[i].action == ACT_SHIFT && control_down(&bindings[i].control))
			s[bindings[i].layer] = true;
	if (s[1] && s[2])
		return -1;
	return s[1] ? 1 : s[2] ? 2 : 0;
}

/* The game has one sideways steering input, which rolls or yaws as its
 * Elite controls option says (rolling also yaws close to a planet). While
 * a roll or yaw binding steers, the option is set to match and put back
 * afterwards. mode: 0 leave it, 1 roll, 2 yaw. */
static void force_elite_controls(int mode)
{
	u32 opt = GAME_A6 + A6_OPT_ELITE_CONTROLS;
	if (mode && game_player_ship())
	{
		if (elite_saved < 0)
			elite_saved = rdbyte(opt) & 0xff;
		wrbyte(opt, mode == 1 ? 0xff : 0);
	}
	else if (elite_saved >= 0)
	{
		wrbyte(opt, elite_saved);
		elite_saved = -1;
	}
}

static void write_steering(float x, float y)
{
	bool on = x != 0.0f || y != 0.0f;
	if (!on && !steering)
		return; /* leave the keyboard's steering alone */
	steering = on;
	if (!game_player_ship())
		return;
	wrword(GAME_A6 + 14508, (int)lrintf(x * 32767.0f));
	wrword(GAME_A6 + 14510, (int)lrintf(y * 32767.0f));
}

void gamepad_update(void)
{
	float steer[2] = {0, 0}, strafe[2] = {0, 0}, roll = 0, yaw = 0, point[2] = {0, 0};
	look[0] = look[1] = 0;
	/* the click ends the pointer (input.c) */
	int px, py;
	pointer_on = pointer_on && Input_PadPointer(&px, &py);
	Uint64 now = SDL_GetTicks64();
	float dt = pointer_ticks ? fminf((float)(now - pointer_ticks) / 1000.0f, 0.1f) : 0.0f;
	pointer_ticks = now;
	take_virtual();
	if (!pad && !vpad.active)
	{
		release_all();
		write_steering(0, 0);
		force_elite_controls(0);
		strafe_set_pad(0, 0);
		return;
	}

	/* on the galaxy map the sticks scroll it instead of flying (the right
	 * one unless it moves the pointer) */
	bool map = game_screen() == SCREEN_GALAXY_MAP;
	if (map)
	{
		float mx = raw_axis(SDL_CONTROLLER_AXIS_LEFTX), my = raw_axis(SDL_CONTROLLER_AXIS_LEFTY);
		float rx = pointer_on ? 0.0f : raw_axis(SDL_CONTROLLER_AXIS_RIGHTX);
		float ry = pointer_on ? 0.0f : raw_axis(SDL_CONTROLLER_AXIS_RIGHTY);
		scroll_map(fabsf(rx) > fabsf(mx) ? rx : mx, fabsf(ry) > fabsf(my) ? ry : my);
	}
	else
		scroll_map(0, 0);

	int layer = current_layer();
	bool out = clipboard_out();
	for (int i = 0; i < n_bindings; i++)
	{
		Binding *b = &bindings[i];
		if (is_axis_action(b->action))
		{
			bool acts = b->layer ? b->layer == layer : !(layer > 0 && (b->axis_shadowed & (1 << layer)));
			bool pointing = b->action == ACT_POINTER_X || b->action == ACT_POINTER_Y;
			if (!acts || pointing != pointer_on || (pointer_on && b->pointer_axis) || (map && !pointing))
				continue;
			float v = axis_value(&b->control);
			if (b->invert)
				v = -v;
			switch (b->action)
			{
			case ACT_STEER_X:
				steer[0] += v;
				break;
			case ACT_STEER_Y:
				steer[1] += v;
				break;
			case ACT_ROLL:
				roll += v;
				break;
			case ACT_YAW:
				yaw += v;
				break;
			case ACT_LOOK_X:
				look[0] += v;
				break;
			case ACT_STRAFE_X:
				strafe[0] += v;
				break;
			case ACT_STRAFE_Y:
				strafe[1] += v;
				break;
			case ACT_POINTER_X:
				point[0] += v;
				break;
			case ACT_POINTER_Y:
				point[1] += v;
				break;
			default:
				look[1] += v;
				break;
			}
			continue;
		}

		bool down = control_down(&b->control);
		bool pressed = down && !b->was_down;
		b->was_down = down;
		if (b->action == ACT_SHIFT)
			continue;
		if (b->action == ACT_STRAFE)
		{
			if (down && b->layer == layer)
				strafe[b->strafe_axis] += b->strafe_sign;
			continue;
		}

		/* the manual layer while the clipboard is out, in place of the
		 * control's other bindings */
		bool acts = b->manual_layer ? out : b->layer == layer && !(out && b->shadowed);
		if (b->action == ACT_MANUAL_PAGE)
		{
			/* turns again while held: after half a second, then faster */
			if (!down)
				b->repeating = false;
			else if (pressed && acts)
			{
				manual_turn(b->pages);
				b->repeating = true;
				b->next_repeat = now + 500;
			}
			else if (b->repeating && now >= b->next_repeat)
			{
				manual_turn(b->pages);
				b->next_repeat = now + 150;
			}
			continue;
		}
		if (b->held && !down)
		{
			chord(&b->sent, false);
			b->held = false;
		}
		if (!pressed || !acts)
			continue;
		switch (b->action)
		{
		case ACT_KEY:
		case ACT_CYCLE:
			b->sent = b->keys[b->action == ACT_CYCLE ? b->step : 0];
			if (b->action == ACT_CYCLE)
				b->step = (b->step + 1) % b->n_keys;
			chord(&b->sent, true);
			b->held = true;
			break;
		case ACT_RECENTER:
			headtrack_recenter();
			vr_recenter();
			break;
		case ACT_COCKPIT:
			cockpit_enabled = !cockpit_enabled;
			break;
		case ACT_MANUAL:
			clipboard_toggle();
			break;
		case ACT_POINTER:
			if (vpad.active) /* the motion controllers point */
				break;
			if (pointer_on)
				Input_PadPointerClick();
			else
				Input_PadPointerBegin();
			pointer_on = true;
			break;
		default:
			break;
		}
	}

	/* the pointer: squared for fine aim near the middle of the stick */
	if (pointer_on)
	{
		float px2 = fmaxf(-1.0f, fminf(1.0f, point[0])), py2 = fmaxf(-1.0f, fminf(1.0f, point[1]));
		Input_PadPointerMove(px2 * fabsf(px2) * pointer_speed * dt, py2 * fabsf(py2) * pointer_speed * dt);
	}

	steer[0] += roll + yaw;
	force_elite_controls(yaw != 0.0f ? 2 : roll != 0.0f ? 1 : 0);
	for (int i = 0; i < 2; i++)
	{
		steer[i] = fmaxf(-1.0f, fminf(1.0f, steer[i]));
		look[i] = fmaxf(-1.0f, fminf(1.0f, look[i]));
		strafe[i] = fmaxf(-1.0f, fminf(1.0f, strafe[i]));
	}
	write_steering(steer[0], steer[1]);
	strafe_set_pad(strafe[0], strafe[1]);
}

void gamepad_look(float *x, float *y)
{
	*x = look[0] * look_yaw * 0.5f;
	*y = look[1] * look_pitch * 0.5f;
}

/* =========================================================================
 * The bindings in words (the clipboard's controls pages)
 * ========================================================================= */
int gamepad_help_count(void)
{
	return n_bindings;
}

/* A control's name on the pad, and on VR controllers ("" if they have none
 * that is this button of the virtual pad, gl_laser.c) */
static void describe_control(const Control *c, char *pad_name, size_t n, char *vr_name, size_t vn)
{
	static const char *const BUTTONS[SDL_CONTROLLER_BUTTON_MAX][2] = {
		[SDL_CONTROLLER_BUTTON_A] = {"A", "R pad down, A"},
		[SDL_CONTROLLER_BUTTON_B] = {"B", "R pad right, B"},
		[SDL_CONTROLLER_BUTTON_X] = {"X", "R pad left, X"},
		[SDL_CONTROLLER_BUTTON_Y] = {"Y", "R pad up, Y"},
		[SDL_CONTROLLER_BUTTON_BACK] = {"Back", "L menu"},
		[SDL_CONTROLLER_BUTTON_GUIDE] = {"Guide", ""},
		[SDL_CONTROLLER_BUTTON_START] = {"Start", "R menu"},
		[SDL_CONTROLLER_BUTTON_LEFTSTICK] = {"L stick click", "L stick click"},
		[SDL_CONTROLLER_BUTTON_RIGHTSTICK] = {"R stick click", "R stick click"},
		[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = {"LB", "L grip"},
		[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = {"RB", "R grip"},
		[SDL_CONTROLLER_BUTTON_DPAD_UP] = {"D-pad up", "L pad up"},
		[SDL_CONTROLLER_BUTTON_DPAD_DOWN] = {"D-pad down", "L pad down"},
		[SDL_CONTROLLER_BUTTON_DPAD_LEFT] = {"D-pad left", "L pad left"},
		[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = {"D-pad right", "L pad right"},
		[SDL_CONTROLLER_BUTTON_MISC1] = {"Share", ""},
		[SDL_CONTROLLER_BUTTON_PADDLE1] = {"P1", ""},
		[SDL_CONTROLLER_BUTTON_PADDLE2] = {"P2", ""},
		[SDL_CONTROLLER_BUTTON_PADDLE3] = {"P3", ""},
		[SDL_CONTROLLER_BUTTON_PADDLE4] = {"P4", ""},
		[SDL_CONTROLLER_BUTTON_TOUCHPAD] = {"Touchpad", ""},
	};
	if (c->source == SRC_BUTTON)
	{
		const char *const *b = c->index >= 0 && c->index < SDL_CONTROLLER_BUTTON_MAX ? BUTTONS[c->index] : NULL;
		snprintf(pad_name, n, "%s", b && b[0] ? b[0] : "?");
		snprintf(vr_name, vn, "%s", b && b[1] ? b[1] : "");
		return;
	}
	if (c->index == SDL_CONTROLLER_AXIS_TRIGGERLEFT || c->index == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
	{
		bool left = c->index == SDL_CONTROLLER_AXIS_TRIGGERLEFT;
		snprintf(pad_name, n, "%s", left ? "LT" : "RT");
		snprintf(vr_name, vn, "%s", left ? "L trigger" : "R trigger");
		return;
	}
	bool left = c->index == SDL_CONTROLLER_AXIS_LEFTX || c->index == SDL_CONTROLLER_AXIS_LEFTY;
	bool x = c->index == SDL_CONTROLLER_AXIS_LEFTX || c->index == SDL_CONTROLLER_AXIS_RIGHTX;
	const char *part = c->half == 0 ? (x ? "x" : "y")
					   : x			? (c->half < 0 ? "left" : "right")
									: (c->half < 0 ? "up" : "down");
	snprintf(pad_name, n, "%c stick %s", left ? 'L' : 'R', part);
	snprintf(vr_name, vn, "%s", pad_name);
}

static void describe_chord(const Chord *c, char *out, size_t n)
{
	snprintf(out, n, "%s%s%s", c->ctrl ? "Ctrl+" : "", c->shift ? "Shift+" : "", SDL_GetKeyName(c->key));
}

void gamepad_help(int i, GamepadHelp *h)
{
	memset(h, 0, sizeof *h);
	if (i < 0 || i >= n_bindings)
		return;
	const Binding *b = &bindings[i];
	h->gap = b->gap;
	snprintf(h->note, sizeof h->note, "%s", b->note);

	char pad_name[24], vr_name[24], prefix[8] = "";
	describe_control(&b->control, pad_name, sizeof pad_name, vr_name, sizeof vr_name);
	if (b->manual_layer)
		strcat(prefix, "M ");
	if (b->layer && b->action != ACT_SHIFT)
		snprintf(prefix + strlen(prefix), sizeof prefix - strlen(prefix), "S%d ", b->layer);
	snprintf(h->control, sizeof h->control, "%s%s", prefix, pad_name);
	if (vr_name[0])
		snprintf(h->vr_control, sizeof h->vr_control, "%s%s", prefix, vr_name);

	static const char *const NAMES[] = {
		[ACT_STEER_X] = "steer x", [ACT_STEER_Y] = "steer y",	  [ACT_ROLL] = "roll",
		[ACT_YAW] = "yaw",		   [ACT_LOOK_X] = "look x",		  [ACT_LOOK_Y] = "look y",
		[ACT_STRAFE_X] = "strafe x", [ACT_STRAFE_Y] = "strafe y", [ACT_RECENTER] = "recentre",
		[ACT_COCKPIT] = "cockpit view", [ACT_POINTER] = "pointer", [ACT_POINTER_X] = "pointer x",
		[ACT_POINTER_Y] = "pointer y", [ACT_MANUAL] = "clipboard",
	};
	switch (b->action)
	{
	case ACT_KEY:
	case ACT_CYCLE:
		h->key = true;
		for (int k = 0; k < b->n_keys; k++)
		{
			char key[32];
			describe_chord(&b->keys[k], key, sizeof key);
			size_t l = strlen(h->action);
			snprintf(h->action + l, sizeof h->action - l, "%s%s", k ? " " : "", key);
		}
		break;
	case ACT_SHIFT:
		snprintf(h->action, sizeof h->action, "shift %d", b->layer);
		break;
	case ACT_STRAFE:
	{
		static const char *const DIRS[2][2] = {{"left", "right"}, {"down", "up"}};
		snprintf(h->action, sizeof h->action, "strafe %s", DIRS[b->strafe_axis][b->strafe_sign > 0]);
		break;
	}
	case ACT_MANUAL_PAGE:
		snprintf(h->action, sizeof h->action, "page %+d", b->pages);
		break;
	default:
		snprintf(h->action, sizeof h->action, "%s%s", NAMES[b->action], b->invert ? " inverted" : "");
		break;
	}
}
