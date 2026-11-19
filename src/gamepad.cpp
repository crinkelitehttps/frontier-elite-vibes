/*
 * gamepad.cpp - see gamepad.h. The bindings (the file format, the shift
 * layers and modes, the keys sent) are vibe-lib's vibe::Bindings; this reads
 * the controllers through SDL2 and carries out the game's actions.
 */
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <set>
#include <string>

#include "gamepad.h"
#include "gamepad_legacy.h"
#include "manual.h"
#include "vibe/binding_editor.h"
#include "vibe/bindings.h"
#include "vibe/input.h"

extern "C"
{
#include "game_state.h"
#include "gl/gl_clipboard.h"
#include "gl/gl_cockpit.h"
#include "gl/gl_focus.h"
#include "gl/gl_hud.h"
#include "gl/gl_mapscreen.h"
#include "headtrack.h"
#include "headlook.h"
#include "host.h"
#include "hostcall.h"
#include "input.h"
#include "keymap.h"
#include "main.h"
#include "strafe.h"
#include "vr.h"
}

/* The built-in bindings, also written out as gamepad.cfg */
static const char DEFAULT_CFG[] = R"cfg(# GLFrontier gamepad bindings (gamepad.cfg format 2; Xbox layout names,
# other controllers map onto the same buttons by position).
#
# Delete this file to get the defaults back.
#
# One binding per line:
#   [shift1|shift2] <control> <action> [args]
#   in <mode> <control> <action> [args]
#   <setting> <value>
# A control may have several lines; '#' starts a comment, which the
# clipboard shows beside the binding; a blank line starts a new group.
#
# Shift layers: a control bound to `shift1` or `shift2` is held to switch
# key bindings to the ones prefixed with that shift (and off the
# unprefixed ones). Holding both reaches neither. The layer counts when a
# control is pressed. Axis bindings (steer, roll, yaw, look, strafe) act
# whatever the shifts, unless the axis has one prefixed with the shift
# being held.
#
# Modes: a binding prefixed `in <mode>` acts only while that mode is on, and
# its control's other bindings don't then. The modes are
#   manual         the clipboard is out (only key and press bindings:
#                  axes keep flying the ship)
#   map            the full-screen galaxy map
#   map-nopointer  the same, while the pointer is not out
#   pointer        the pointer is out (see the pointer action)
# `in map leftx none` silences a control in a mode without replacing it.
#
# Controls (SDL names):
#   buttons   a b x y back guide start leftstick rightstick
#             leftshoulder rightshoulder dpup dpdown dpleft dpright
#             misc1 paddle1 paddle2 paddle3 paddle4 touchpad
#   axes      leftx lefty rightx righty lefttrigger righttrigger
#   half axes leftx- leftx+ lefty- lefty+ ... (- is left / up), usable as
#             buttons; the triggers work as buttons too
#
# VR motion controllers have their own controls, prefixed left- or right-:
#   trigger squeeze (the grip) menu thumbstick (the click) thumbstick-x
#   thumbstick-y trackpad (the click) trackpad-up trackpad-down trackpad-left
#   trackpad-right trackpad-center trackpad-x trackpad-y trackpad-touch, and
#   the face buttons left-x left-y right-a right-b. Thumbstick and trackpad
#   axes have halves too (right-thumbstick-y-). A trigger pulled with its
#   beam on the panel or a screen, or with the grip held, is a mouse click
#   instead.
#
# Actions:
#   steer-x, steer-y [invert]  the ship's steering (axes):
#                              x turns (rolls with Elite controls), y pitches
#   roll, yaw [invert]         steer-x, but rolling / yawing whatever the
#                              game's Elite controls option (roll yaws flat
#                              below 40 km, where the game's roll also yaws)
#   look-x, look-y [invert]    look around in the cockpit (springs back)
#   head-x, head-y [invert]    turn the head in the cockpit; it stays where it
#                              is left (y: push up to look up)
#   head-hold                  double-tap: the head straight ahead again
#                              (give it the control held for head-x / head-y)
#   strafe-x, strafe-y [invert] sideways / vertical thrust (x right +, y up +,
#                              so lefty wants invert); a button gives full
#                              thrust while held
#   key <key>                  a keyboard key while held, with optional
#                              modifiers: shift+f1, ctrl+k
#   cycle <key> <key> ...      each press sends the next key in turn
#   shift1, shift2             hold for that shift layer
#   recenter                   head tracking: the current pose is ahead
#   cockpit                    cockpit view on / off (also Ctrl-K)
#   focus-next, focus-prev     the cockpit's next / previous screen (dashboard,
#                              COMS, MAP, CAMERA, SHIP clockwise): a green outline
#   nav-up, nav-down, nav-left, nav-right
#                              move the green box between the focused screen's buttons
#   activate                   press the button under the box
#   orbit                      orbit display on / off: the orbit's conic on the HUD
#                              with Ap, Pe and the time to the next apsis
#   pointer                    a mouse pointer for the control panel and
#                              menus: press for it, press again to click
#                              there (left button) and put it away (not
#                              with VR motion controllers: they point)
#   pointer-x, pointer-y [invert] move it; only in the pointer mode
#   manual                     the clipboard (controls and the game
#                              manual): up to read / put away (also F11)
#   manual-page <n>            turn n pages (-n: back); repeats while held
#   edit                       change these bindings on the clipboard's
#                              controls pages: pick a row (d-pad / stick up and
#                              down), A / trigger / Enter, then press the new
#                              control; B / menu / Esc ends. Start+Back, Enter
#                              or a click / fingertip and trigger on a row
#                              work whatever this file says.
#   menu                       the emulator menu (the cog) on / off; with a VR
#                              headset it is a screen in the cockpit: grip it
#                              to move, turn and size it (it is kept)
#   none                       nothing (in a mode)
#   Key names as SDL spells them, lower case: a-z 0-9 f1-f10 = - , . / ;
#   return space escape tab backspace up down left right home end pageup
#   pagedown insert delete (aliases: enter esc lshift rshift lctrl pgup pgdn
#   ins del)
#
# Settings:
#   deadzone 0.15              stick deadzone (fraction of full travel)
#   threshold 0.5              how far an axis must go to act as a button
#   repeat off                 held keys don't repeat (on: like a PC keyboard)
#   look-yaw 100               look-around range, degrees
#   look-pitch 60
#   head-speed 150             head-x / head-y turn rate, degrees a second
#   pointer-speed 200          pointer speed, screen pixels (of 320) a second

repeat     off
deadzone   0.15
threshold  0.5
look-yaw   100
look-pitch 60
head-speed 150
pointer-speed 200

# ---- Flying
leftx             strafe-x           # thrust left / right
lefty             strafe-y invert    # thrust up / down
rightx            roll
righty            steer-y invert     # pitch: push forward to dive (drop invert to climb)
leftshoulder      key return         # forward: set speed up, or main thrust (hold)
lefttrigger       key rshift         # back: set speed down, or retro thrust (hold)
righttrigger      key space          # fire
b                 key f1             # flight view: front, rear, turrets, external
start             key escape         # pause
leftstick         pointer            # pointer for the panel: press, aim (right stick), press to click
in pointer rightx pointer-x
in pointer righty pointer-y
rightstick        recenter           # head tracking / VR: this pose is straight ahead

# ---- The cockpit's screens: Y / X pick the next / previous one (clockwise) and a
# green box on a button, moved with the d-pad, is pressed with A. (The keyboard:
# Tab / Shift-Tab, the arrows, F.) Zoom is =  and - on the keyboard; the cockpit view
# is Right shoulder + Y or Ctrl-K.
y                 focus-next         # next screen, clockwise
x                 focus-prev         # previous screen
a                 activate           # press the button under the green box
dpup              nav-up             # move the green box
dpdown            nav-down
dpleft            nav-left
dpright           nav-right

# ---- Back held: the console's function keys, laid out like the panel
# (F1-F4 on the left, F6-F10 on the right)
back              shift1
shift1 dpleft     key f1
shift1 dpup       key f2
shift1 dpright    key f3
shift1 dpdown     key f4
shift1 leftstick  key f5
shift1 x          key f6
shift1 y          key f7
shift1 b          key f8
shift1 a          key f9
shift1 start      key f10
shift1 rightstick menu               # the menu (VR: left menu button + right stick click)

# ---- Right shoulder held: the right stick turns the head (it stays put; a
# double tap of the shoulder looks ahead again), time acceleration on the
# d-pad and Y the cockpit view
rightshoulder     shift2
rightshoulder     head-hold          # double-tap: head straight ahead
shift2 rightx     head-x             # turn the head
shift2 righty     head-y
shift2 dpdown     key shift+f1       # normal time
shift2 dpleft     key shift+f2
shift2 dpup       key shift+f3
shift2 dpright    key shift+f4
shift2 y          cockpit            # cockpit view on / off (also Ctrl-K)

# ---- The clipboard: Right shoulder + A (VR: right grip + right trackpad
# down), or grab it with a VR hand. While it is out the d-pad (VR: left
# trackpad) turns pages.
shift2 a          manual             # clipboard: controls and manual
in manual dpleft  manual-page -1     # previous page
in manual dpright manual-page +1     # next page
in manual dpup    manual-page -10    # 10 pages back
in manual dpdown  manual-page +10    # 10 pages on
in manual x       edit               # change the bindings on these pages

# ---- The galaxy map: the sticks scroll it (the arrow keys) instead of flying.
# The right one moves the pointer, not the map, while the pointer is out.
in map leftx      none
in map lefty      none
in map leftx-     key left
in map leftx+     key right
in map lefty-     key up
in map lefty+     key down
in map-nopointer rightx none
in map-nopointer righty none
in map-nopointer rightx- key left
in map-nopointer rightx+ key right
in map-nopointer righty- key up
in map-nopointer righty+ key down

# ---- VR motion controllers: the same bindings on the headset's controllers
left-thumbstick-x strafe-x           # thrust left / right
left-thumbstick-y strafe-y invert    # thrust up / down
right-thumbstick-x roll
right-thumbstick-y steer-y invert    # pitch: push forward to dive (drop invert to climb)
shift2 right-thumbstick-x yaw        # right shoulder held: yaw instead of roll
left-squeeze      key return         # forward: set speed up, or main thrust (hold)
left-trigger      key rshift         # back: set speed down, or retro thrust (hold)
right-trigger     key space          # fire
right-b           key f1             # flight view: front, rear, turrets, external
right-trackpad-right key f1          # flight view: front, rear, turrets, external
left-y            cockpit            # cockpit view on / off
right-trackpad-up cockpit            # cockpit view on / off
right-menu        key escape         # pause
right-thumbstick  recenter           # head tracking / VR: this pose is straight ahead
left-trackpad-up  key =              # zoom in (maps, external view)
left-trackpad-down key -             # zoom out

left-menu         shift1
shift1 left-trackpad-left key f1
shift1 left-trackpad-up key f2
shift1 left-trackpad-right key f3
shift1 left-trackpad-down key f4
shift1 left-thumbstick key f5
shift1 left-x     key f6
shift1 right-trackpad-left key f6
shift1 left-y     key f7
shift1 right-trackpad-up key f7
shift1 right-b    key f8
shift1 right-trackpad-right key f8
shift1 right-a    key f9
shift1 right-trackpad-down key f9
shift1 right-menu key f10
shift1 right-thumbstick menu         # the menu (VR: left menu button + right stick click)

right-squeeze     shift2
shift2 left-trackpad-down key shift+f1 # normal time
shift2 left-trackpad-left key shift+f2
shift2 left-trackpad-up key shift+f3
shift2 left-trackpad-right key shift+f4
shift2 left-y     key shift+f5       # fastest
shift2 right-trackpad-up key shift+f5 # fastest

shift2 right-a    manual             # clipboard: controls and manual
shift2 right-trackpad-down manual    # clipboard: controls and manual
in manual left-trackpad-left manual-page -1 # previous page
in manual left-trackpad-right manual-page +1 # next page
in manual left-trackpad-up manual-page -10 # 10 pages back
in manual left-trackpad-down manual-page +10 # 10 pages on
in manual left-x  edit               # change the bindings on these pages
in manual left-trackpad-center edit  # change the bindings on these pages

in map left-thumbstick-x none
in map left-thumbstick-y none
in map left-thumbstick-x- key left
in map left-thumbstick-x+ key right
in map left-thumbstick-y- key up
in map left-thumbstick-y+ key down
in map-nopointer right-thumbstick-x none
in map-nopointer right-thumbstick-y none
in map-nopointer right-thumbstick-x- key left
in map-nopointer right-thumbstick-x+ key right
in map-nopointer right-thumbstick-y- key up
in map-nopointer right-thumbstick-y+ key down
)cfg";

namespace
{

std::string gamepad_file(const char *suffix = "");
void log_bindings_text(const std::string &text);

vibe::Bindings bindings;
/* the actions and modes the file names (registered in gamepad_init) */
int steer_x, steer_y, roll_axis, yaw_axis, strafe_x, strafe_y, pointer_x, pointer_y;
int press_cockpit, press_orbit, press_menu, press_pointer;
/* the green box's d-pad repeat when held: first after, then every */
#define NAV_REPEAT_DELAY_NS 400000000ull
#define NAV_REPEAT_NS       120000000ull
int press_focus_next, press_focus_prev, press_nav[4], press_activate; /* gl_focus.h: up, down, left, right */
int mode_map, mode_map_nopointer, mode_pointer;
int set_look_yaw, set_look_pitch, set_pointer_speed, set_head_speed;
int head_x, head_y, button_head_hold;
HeadlookTap head_tap;

SDL_GameController *pad;
/* The VR controllers: as set, and as this update reads them */
SDL_SpinLock vr_lock;
GamepadVr vr_in, vr_pending;
vibe::MotionControllers motion;
bool vr_active;

float look[2];     /* -1..1, y down + */
bool steering;     /* wrote steering last update */
int elite_saved = -1; /* the game's Elite controls option while roll / yaw force it, else -1 */
bool pointer_on;
Uint64 pointer_ticks;

/* The binding editor, once the clipboard has its pages (manual_init) */
vibe::BindingEditor *editor;
/* A VR hand at the board (gamepad_edit_pointer_later), for the next update */
SDL_SpinLock board_lock;
struct BoardPointer
{
	bool on = false, click = false;
	float u = 0, v = 0;
} board_in, board_was;

uint64_t now_ns()
{
	return SDL_GetTicks64() * 1000000ull;
}

vibe::BindingEditor *the_editor()
{
	if (!editor && clipboard().started())
	{
		editor = new vibe::BindingEditor(clipboard(), bindings);
		editor->source = GAMEPAD_FILE;
		editor->save = [](const std::string &text) {
			std::string path = gamepad_file();
			bool ok = vibe::save_bindings(text, "", path);
			if (ok)
				log_printf("Gamepad: rebinding saved to %s, now:\n", path.c_str());
			else
				log_printf("Gamepad: rebinding NOT saved, can't write %s (%s); in use until the game ends:\n",
						   path.c_str(), std::strerror(errno));
			log_bindings_text(text);
			return ok;
		};
		editor->set_hint("Change these bindings: Enter, Start+Back, or X (VR: left X, or trigger at a row)");
	}
	return editor;
}

/* The VR hand at the board: a click asks for the row's control; while
 * editing, the row under the fingertip is picked */
void board_pointer()
{
	BoardPointer p;
	SDL_AtomicLock(&board_lock);
	p = board_in;
	board_in.click = false;
	SDL_AtomicUnlock(&board_lock);
	vibe::BindingEditor *e = the_editor();
	if (!e || !p.on)
	{
		board_was.on = false;
		return;
	}
	if (p.click)
		e->pointer(p.u, p.v, true, now_ns());
	else if (e->active() && (!board_was.on || p.u != board_was.u || p.v != board_was.v))
		e->pointer(p.u, p.v, false, now_ns());
	board_was = p;
}

/* ---- keys --------------------------------------------------------------- */
void send(SDL_Keycode key, Uint16 mod, bool down)
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

/* Bindings sends a chord as its modifiers down, the key down ... key up,
 * the modifiers up. The emulator's Ctrl shortcuts act on the press alone,
 * with Ctrl as the key's modifier; Shift is a key of its own. */
void send_key(int code, unsigned mods, bool down)
{
	if (code == SDLK_LCTRL || code == SDLK_LALT)
		return;
	if (mods & vibe::kModCtrl)
	{
		if (down)
			send((SDL_Keycode)code, KMOD_LCTRL, true);
		return;
	}
	send((SDL_Keycode)code, 0, down);
}

bool resolve_key(const std::string &name, int *code)
{
	static const struct
	{
		const char *alias, *sdl;
	} aliases[] = {{"enter", "Return"},  {"esc", "Escape"},	   {"lshift", "Left Shift"}, {"rshift", "Right Shift"},
				   {"pgup", "PageUp"},	 {"pgdn", "PageDown"}, {"ins", "Insert"},		 {"del", "Delete"},
				   {"lctrl", "Left Ctrl"}, {"lalt", "Left Alt"}};
	std::string n = name;
	for (const auto &a : aliases)
		if (!SDL_strcasecmp(n.c_str(), a.alias))
			n = a.sdl;
	SDL_Keycode k = SDL_GetKeyFromName(n.c_str());
	if (k == SDLK_UNKNOWN)
		return false;
	*code = (int)k;
	return true;
}

std::string key_label(int code)
{
	return SDL_GetKeyName((SDL_Keycode)code);
}

/* ---- the file ----------------------------------------------------------- */
/* gamepad.cfg (and its .old) beside the saves. On Android the working
 * directory is "/", so it is the app's files folder, like glfrontier.cfg. */
std::string gamepad_file(const char *suffix)
{
#ifdef ANDROID
	const char *dir = SDL_AndroidGetInternalStoragePath();
	if (dir)
		return std::string(dir) + "/" + GAMEPAD_FILE + suffix;
#endif
	return std::string(GAMEPAD_FILE) + suffix;
}

bool read_file(const char *path, std::string *text)
{
	std::ifstream f(path, std::ios::binary);
	if (!f)
		return false;
	std::stringstream ss;
	ss << f.rdbuf();
	*text = ss.str();
	return true;
}

bool write_file(const char *path, const std::string &text)
{
	std::ofstream f(path, std::ios::binary);
	f << text;
	return (bool)f;
}

/* The bindings in use into the log, without comments and blank lines, so a
 * diagnostics upload shows what the player had */
void log_bindings_text(const std::string &text)
{
	std::istringstream in(text);
	std::string line;
	while (std::getline(in, line))
	{
		size_t a = line.find_first_not_of(" \t\r");
		if (a == std::string::npos || line[a] == '#')
			continue;
		size_t b = line.find_last_not_of(" \t\r");
		log_printf("Gamepad:   %s\n", line.substr(a, b - a + 1).c_str());
	}
}

/* A line's words before any comment */
std::vector<std::string> line_words(const std::string &line)
{
	std::istringstream in(line.substr(0, line.find('#')));
	std::vector<std::string> w;
	std::string t;
	while (in >> t)
		w.push_back(t);
	return w;
}

/* Files from before the held head get its bindings on the right shoulder
 * (when that is shift2), in place of the old right-shoulder yaw. False if
 * nothing changed. */
bool add_head_bindings(std::string *text)
{
	if (text->find("head-") != std::string::npos)
		return false;
	std::istringstream in(*text);
	std::string line, out;
	bool added = false;
	while (std::getline(in, line))
	{
		std::vector<std::string> w = line_words(line);
		if (w == std::vector<std::string>{"shift2", "rightx", "yaw"})
			continue;
		out += line + "\n";
		if (!added && w == std::vector<std::string>{"rightshoulder", "shift2"})
		{
			out += "rightshoulder     head-hold          # double-tap: head straight ahead\n"
				   "shift2 rightx     head-x             # turn the head (it stays put)\n"
				   "shift2 righty     head-y\n";
			added = true;
		}
	}
	if (!added)
		return false;
	*text = out;
	return true;
}

/* Files from before the cockpit focus navigation: the old default pad Y
 * cockpit and d-pad zoom lines make way for focus-next / focus-prev, nav-*
 * and activate, on whichever of Y, X, A and the d-pad the player hasn't
 * bound to something else. False if nothing changed. */
bool add_focus_bindings(std::string *text)
{
	if (text->find("focus-next") != std::string::npos || text->find("nav-up") != std::string::npos)
		return false;
	typedef std::vector<std::string> Words;
	static const Words old_lines[] = {{"y", "cockpit"}, {"dpup", "key", "="}, {"dpdown", "key", "-"}};
	static const char *const new_lines[][2] = {
		{"y", "y                 focus-next         # next screen, clockwise\n"},
		{"x", "x                 focus-prev         # previous screen\n"},
		{"a", "a                 activate           # press the button under the green box\n"},
		{"dpup", "dpup              nav-up             # move the green box\n"},
		{"dpdown", "dpdown            nav-down\n"},
		{"dpleft", "dpleft            nav-left\n"},
		{"dpright", "dpright           nav-right\n"},
	};
	/* the controls still bound outside any shift or mode, once the old lines go */
	std::istringstream in(*text);
	std::string line;
	std::vector<std::string> kept;
	std::set<std::string> bound;
	size_t insert_at = std::string::npos;
	while (std::getline(in, line))
	{
		Words w = line_words(line);
		bool old = false;
		for (const Words &o : old_lines)
			old = old || w == o;
		if (old)
		{
			if (insert_at == std::string::npos)
				insert_at = kept.size();
			continue;
		}
		if (!w.empty())
			bound.insert(w[0]);
		kept.push_back(line);
	}
	std::string block;
	for (const auto &n : new_lines)
		if (!bound.count(n[0]))
			block += n[1];
	if (block.empty())
		return false;
	block = "# cockpit screens: Y / X next / previous, d-pad moves the green box, A presses it\n" + block;
	if (insert_at == std::string::npos)
		insert_at = kept.size();
	std::string out;
	for (size_t i = 0; i < kept.size(); i++)
	{
		if (i == insert_at)
			out += block;
		out += kept[i] + "\n";
	}
	if (insert_at == kept.size())
		out += block;
	*text = out;
	return true;
}

/* Files without a pad button for the cockpit view (pad Y became focus-next):
 * Right shoulder + Y takes it, in place of the old fastest time there. VR
 * controls (left-, right-) don't count. False if nothing changed. */
bool add_cockpit_binding(std::string *text)
{
	typedef std::vector<std::string> Words;
	std::istringstream in(*text);
	std::string line, out;
	bool shift2_y = false, replaced = false;
	size_t shoulder_end = std::string::npos;
	while (std::getline(in, line))
	{
		Words w = line_words(line);
		if (!w.empty() && w.back() == "cockpit")
		{
			const std::string &c = w.size() >= 3 ? w[w.size() - 2] : w[0];
			if (c.compare(0, 5, "left-") != 0 && c.compare(0, 6, "right-") != 0)
				return false;
		}
		if (w.size() >= 2 && w[0] == "shift2" && w[1] == "y")
		{
			shift2_y = true;
			if (w == Words{"shift2", "y", "key", "shift+f5"})
			{
				out += "shift2 y          cockpit            # cockpit view on / off (also Ctrl-K)\n";
				replaced = true;
				continue;
			}
		}
		out += line + "\n";
		if (w.size() >= 2 && (w[0] == "rightshoulder" || w[0] == "shift2"))
			shoulder_end = out.size();
	}
	if (!replaced)
	{
		if (shift2_y || shoulder_end == std::string::npos)
			return false; /* Y taken by the player, or no right-shoulder layer */
		out.insert(shoulder_end, "shift2 y          cockpit            # cockpit view on / off (also Ctrl-K)\n");
	}
	*text = out;
	return true;
}

void load_bindings()
{
	std::string text, path = gamepad_file();
	if (!read_file(path.c_str(), &text))
	{
		log_printf("Gamepad: can't read %s (%s): the built-in bindings\n", path.c_str(), std::strerror(errno));
		bindings.set_defaults(DEFAULT_CFG);
		bindings.load(DEFAULT_CFG, "built-in gamepad bindings");
		if (write_file(path.c_str(), DEFAULT_CFG))
			log_printf("Gamepad: wrote the default bindings to %s\n", path.c_str());
		else
			log_printf("Gamepad: can't write %s (%s): rebinding won't be kept\n", path.c_str(), std::strerror(errno));
		log_bindings_text(DEFAULT_CFG);
		return;
	}
	if (gamepad_text_is_legacy(text))
	{
		/* keep the old file beside it, once */
		std::string old = gamepad_file(".old");
		std::string existing;
		if (!read_file(old.c_str(), &existing))
			write_file(old.c_str(), text);
		text = gamepad_convert_legacy(text);
		if (write_file(path.c_str(), text))
			log_printf("Gamepad: %s converted to the new format (the old one is %s)\n", path.c_str(), old.c_str());
		else
			log_printf("Gamepad: converted %s, but can't write it (%s)\n", path.c_str(), std::strerror(errno));
	}
	if (add_head_bindings(&text))
	{
		if (write_file(path.c_str(), text))
			log_printf("Gamepad: added the head look (right shoulder + right stick) to %s\n", path.c_str());
		else
			log_printf("Gamepad: added the head look, but can't write %s (%s)\n", path.c_str(), std::strerror(errno));
	}
	if (add_focus_bindings(&text))
	{
		if (write_file(path.c_str(), text))
			log_printf("Gamepad: added the cockpit focus keys (Y / X, d-pad, A) to %s\n", path.c_str());
		else
			log_printf("Gamepad: added the cockpit focus keys, but can't write %s (%s)\n", path.c_str(), std::strerror(errno));
	}
	if (add_cockpit_binding(&text))
	{
		if (write_file(path.c_str(), text))
			log_printf("Gamepad: the cockpit view on Right shoulder + Y in %s\n", path.c_str());
		else
			log_printf("Gamepad: the cockpit view on Right shoulder + Y, but can't write %s (%s)\n", path.c_str(), std::strerror(errno));
	}
	bindings.set_defaults(DEFAULT_CFG); /* the editor's reset */
	if (!bindings.load(text, GAMEPAD_FILE))
		log_printf("Gamepad: some lines of %s were skipped\n", path.c_str());
	else
		log_printf("Gamepad: %d bindings from %s\n", (int)bindings.help().size(), path.c_str());
	log_bindings_text(text);
}

/* ---- the game's end ------------------------------------------------------ */

/* The game has one sideways steering input, which rolls or yaws as its
 * Elite controls option says (rolling also yaws close to a planet). While
 * a roll or yaw binding steers, the option is set to match and put back
 * afterwards. mode: 0 leave it, 1 roll, 2 yaw. */
/* Below 40 km (away from the port) the game's Elite controls turn the ship
 * with roll and yaw together (L626ea), an arc, unless elite_roll_low (hook
 * Lelite_low) keeps them a pure roll: with that off a roll binding yaws flat
 * there instead. A6+692 is the altitude in mm (0 landed or docked, $ffffffff
 * when there is no planet near). */
bool roll_low()
{
	return !elite_roll_low && (u32)rdlong(GAME_A6 + 692) <= 40000000u;
}

void force_elite_controls(int mode)
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

/* Steering diagnosis (GLF_STEER_LOG=1; glfrontier.log, lines "STEER"): the game's state
 * around the pad's steering whenever it changes, and twice a second while
 * the pad steers. turned = degrees the ship turned since the last line and
 * the axis, in the ship's own axes (right, up, forward): yaw is about up,
 * roll about forward, pitch about right. */
void log_steering(float sx, float sy, float roll, float yaw)
{
	static int last_key = -1;
	static Uint64 last_ticks;
	static double prev[9];
	static bool have_prev;
	static int enabled = -1;
	if (enabled < 0)
	{
		const char *e = SDL_getenv("GLF_STEER_LOG");
		enabled = e && *e && *e != '0';
	}
	if (!enabled)
		return;
	u32 ship = game_player_ship();
	if (!ship)
	{
		have_prev = false;
		return;
	}
	int state = rdbyte(GAME_A6 + A6_FLIGHT_STATE) & 0xff;
	int flags = rdbyte(GAME_A6 + A6_FLIGHT_FLAGS) & 0xff;
	int near = rdbyte(GAME_A6 + 893) & 0xff;
	int elite = rdbyte(GAME_A6 + A6_OPT_ELITE_CONTROLS) & 0xff;
	bool on = sx != 0.0f || sy != 0.0f;
	int key = state | (flags & 1) << 8 | (near & 0x20) << 4 | (elite ? 1 << 10 : 0) | (on ? 1 << 11 : 0) |
		(roll != 0.0f) << 12 | (yaw != 0.0f) << 13;
	Uint64 now = SDL_GetTicks64();
	if (key == last_key && !(on && now - last_ticks >= 500))
		return;
	double m[9];
	for (int k = 0; k < 9; k++)
		m[k] = (int16_t)rdword(ship + 2 * k) / 32768.0;
	/* the game's axes are not quite unit length: a turn of nothing would read as ~14 degrees */
	for (int j = 0; j < 3; j++)
	{
		double l = std::sqrt(m[j] * m[j] + m[3 + j] * m[3 + j] + m[6 + j] * m[6 + j]);
		if (l > 1e-9)
			for (int i = 0; i < 3; i++)
				m[3 * i + j] /= l;
	}
	char turned[96] = "";
	if (have_prev)
	{
		/* R = prev^T m: the turn in the ship's axes */
		double r[3][3];
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				r[i][j] = prev[0 + i] * m[0 + j] + prev[3 + i] * m[3 + j] + prev[6 + i] * m[6 + j];
		double c = std::max(-1.0, std::min(1.0, (r[0][0] + r[1][1] + r[2][2] - 1.0) / 2.0));
		double ax[3] = {r[2][1] - r[1][2], r[0][2] - r[2][0], r[1][0] - r[0][1]};
		double l = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
		if (l < 1e-9)
			l = 1.0;
		snprintf(turned, sizeof turned, " turned %.1f deg about (r %+.2f u %+.2f f %+.2f)",
				 std::acos(c) * 180.0 / M_PI, ax[0] / l, ax[1] / l, ax[2] / l);
	}
	memcpy(prev, m, sizeof prev);
	have_prev = true;
	log_printf("STEER state $%02x engines %s near $%02x alt %.2f km speed %d set %d elite %s | pad x %+.2f y %+.2f roll %+.2f "
			   "yaw %+.2f | word x %d y %d | queue pitch %d roll %d yaw %d | align %d %d %d | 3d %d%s\n",
			   state, flags & 1 ? "off" : "on", near, (int)rdlong(GAME_A6 + 692) / 1e6, (int)rdlong(GAME_A6 + 688),
			   (int)rdlong(GAME_A6 + 848), elite ? "on" : "off", sx, sy, roll, yaw, (int16_t)rdword(GAME_A6 + 14508),
			   (int16_t)rdword(GAME_A6 + 14510), (int16_t)rdword(ship + 134), (int16_t)rdword(ship + 136),
			   (int16_t)rdword(ship + 138), (int16_t)rdword(ship + 228), (int16_t)rdword(ship + 230),
			   (int16_t)rdword(ship + 232), rdbyte(GAME_A6 + A6_3DVIEW_MODE) & 0xff, turned);
	last_key = key;
	last_ticks = now;
}

void write_steering(float x, float y)
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

/* The VR controllers as this update reads them: a button pressed since the
 * last update counts as down for one */
void take_vr()
{
	GamepadVr v;
	SDL_AtomicLock(&vr_lock);
	v = vr_in;
	for (int h = 0; h < 2; h++)
	{
		v.squeeze[h] |= vr_pending.squeeze[h];
		v.menu[h] |= vr_pending.menu[h];
		v.lower[h] |= vr_pending.lower[h];
		v.upper[h] |= vr_pending.upper[h];
		v.stick_click[h] |= vr_pending.stick_click[h];
		v.pad_click[h] |= vr_pending.pad_click[h];
	}
	memset(&vr_pending, 0, sizeof vr_pending);
	SDL_AtomicUnlock(&vr_lock);
	vr_active = v.active[0] || v.active[1];
	for (int h = 0; h < 2; h++)
	{
		motion.active[h] = v.active[h];
		motion.trigger[h] = v.trigger[h];
		motion.squeeze[h] = v.squeeze[h];
		motion.menu[h] = v.menu[h];
		motion.lower[h] = v.lower[h];
		motion.upper[h] = v.upper[h];
		motion.stick_click[h] = v.stick_click[h];
		motion.pad_click[h] = v.pad_click[h];
		for (int k = 0; k < 2; k++)
		{
			motion.stick[h][k] = v.stick[h][k];
			motion.pad[h][k] = v.pad[h][k];
		}
		motion.pad_touch[h] = v.pad_click[h] || v.pad[h][0] != 0.0f || v.pad[h][1] != 0.0f;
	}
}

void read_pad(vibe::ControllerReading *r)
{
	r->pad = pad != NULL;
	if (!pad)
		return;
	for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX && i < vibe::kPadButtonCount; i++)
		r->button[i] = SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)i) != 0;
	for (int i = 0; i < vibe::kPadAxisCount; i++)
		r->axis[i] = SDL_GameControllerGetAxis(pad, (SDL_GameControllerAxis)i) / 32767.0f;
}

} /* namespace */

vibe::Bindings &gamepad_bindings()
{
	return bindings;
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
	bindings.resolve_key = resolve_key;
	bindings.key_label = key_label;
	bindings.send_key = send_key;
	bindings.report = [](const std::string &message) { log_printf("%s\n", message.c_str()); };
	steer_x = bindings.add_axis("steer-x", "steer x");
	steer_y = bindings.add_axis("steer-y", "steer y");
	roll_axis = bindings.add_axis("roll");
	yaw_axis = bindings.add_axis("yaw");
	strafe_x = bindings.add_axis("strafe-x", "strafe x");
	strafe_y = bindings.add_axis("strafe-y", "strafe y");
	pointer_x = bindings.add_axis("pointer-x", "pointer x");
	pointer_y = bindings.add_axis("pointer-y", "pointer y");
	head_x = bindings.add_axis("head-x", "head x");
	head_y = bindings.add_axis("head-y", "head y");
	button_head_hold = bindings.add_button("head-hold", "head ahead (double-tap)");
	press_cockpit = bindings.add_press("cockpit", "cockpit view");
	press_orbit = bindings.add_press("orbit", "orbit display");
	press_menu = bindings.add_press("menu");
	press_pointer = bindings.add_press("pointer");
	press_focus_next = bindings.add_press("focus-next", "next screen");
	press_focus_prev = bindings.add_press("focus-prev", "previous screen");
	press_nav[0] = bindings.add_press("nav-up", "select up");
	press_nav[1] = bindings.add_press("nav-down", "select down");
	press_nav[2] = bindings.add_press("nav-left", "select left");
	press_nav[3] = bindings.add_press("nav-right", "select right");
	press_activate = bindings.add_press("activate", "press button");
	mode_map = bindings.add_mode("map");
	mode_map_nopointer = bindings.add_mode("map-nopointer");
	mode_pointer = bindings.add_mode("pointer");
	set_look_yaw = bindings.add_setting("look-yaw", 100.0f, 0.0f, 360.0f);
	set_look_pitch = bindings.add_setting("look-pitch", 60.0f, 0.0f, 180.0f);
	set_pointer_speed = bindings.add_setting("pointer-speed", 200.0f, 0.0f, 5000.0f);
	set_head_speed = bindings.add_setting("head-speed", 150.0f, 0.0f, 1000.0f);
	load_bindings();
}

void gamepad_shutdown(void)
{
	bindings.release_all();
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

void gamepad_set_vr(const GamepadVr *v)
{
	SDL_AtomicLock(&vr_lock);
	for (int h = 0; h < 2; h++)
	{
		vr_pending.squeeze[h] |= v->squeeze[h] && !vr_in.squeeze[h];
		vr_pending.menu[h] |= v->menu[h] && !vr_in.menu[h];
		vr_pending.lower[h] |= v->lower[h] && !vr_in.lower[h];
		vr_pending.upper[h] |= v->upper[h] && !vr_in.upper[h];
		vr_pending.stick_click[h] |= v->stick_click[h] && !vr_in.stick_click[h];
		vr_pending.pad_click[h] |= v->pad_click[h] && !vr_in.pad_click[h];
	}
	vr_in = *v;
	SDL_AtomicUnlock(&vr_lock);
}

void gamepad_update(void)
{
	look[0] = look[1] = 0;
	/* the click ends the pointer (input.c) */
	int px, py;
	pointer_on = pointer_on && Input_PadPointer(&px, &py);
	Uint64 now = SDL_GetTicks64();
	float dt = pointer_ticks ? std::min((float)(now - pointer_ticks) / 1000.0f, 0.1f) : 0.0f;
	pointer_ticks = now;
	take_vr();
	board_pointer();
	vibe::BindingEditor *ed = the_editor();
	if (!pad && !vr_active)
	{
		if (ed) /* the keyboard may be editing: time out a capture */
			ed->update(vibe::ControllerReading(), now_ns());
		bindings.release_all();
		write_steering(0, 0);
		force_elite_controls(0);
		strafe_set_pad(0, 0);
		return;
	}

	vibe::ControllerReading reading;
	read_pad(&reading);
	reading.vr = vr_active ? &motion : nullptr;
	/* on the full-screen galaxy map the sticks scroll it instead of flying
	 * (the right one unless it moves the pointer); on the cockpit's screen
	 * they never do: its buttons pan the map, and the ship flies on */
	bool map = game_screen() == SCREEN_GALAXY_MAP && !cockpit_screen_frame();
	vibe::BindingContext ctx;
	ctx.now_ns = now * 1000000ull;
	ctx.clipboard_out = clipboard_out();
	ctx.modes = (map ? 1u << mode_map : 0u) | (map && !pointer_on ? 1u << mode_map_nopointer : 0u) |
				(pointer_on ? 1u << mode_pointer : 0u);
	ctx.suspended = ed && ed->active();
	vibe::Bindings::Frame f = bindings.update(reading, ctx);
	if (ed)
	{
		if (f.edit)
			ed->toggle(ctx.now_ns);
		ed->update(reading, ctx.now_ns);
	}

	if (f.recenter)
	{
		headtrack_recenter();
		vr_recenter();
	}
	if (f.manual)
		clipboard_toggle();
	if (f.manual_page)
		manual_turn(f.manual_page);
	if (f.press[press_cockpit])
		cockpit_enabled = !cockpit_enabled;
	if (f.press[press_orbit])
		orbit_display = !orbit_display;
	if (f.press[press_menu])
		toggle_m68k_menu = !toggle_m68k_menu;
	if (f.press[press_focus_next])
		focus_next();
	if (f.press[press_focus_prev])
		focus_prev();
	/* the green box: a held d-pad moves it again after a moment, like a key held on a keyboard; activate holds
	 * the button under it down as long as it is held */
	{
		static const int DIR[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
		static vibe::Repeater nav_repeat[4];
		static bool activate_held;
		for (int i = 0; i < 4; i++)
		{
			nav_repeat[i].delay_ns = NAV_REPEAT_DELAY_NS;
			nav_repeat[i].period_ns = NAV_REPEAT_NS;
			if (nav_repeat[i].update(f.press_held[press_nav[i]] || f.press[press_nav[i]], ctx.now_ns))
				focus_move(DIR[i][0], DIR[i][1]);
		}
		const bool act = f.press_held[press_activate] || f.press[press_activate];
		if (act != activate_held)
			focus_press(act);
		activate_held = act;
	}
	if (f.press[press_pointer] && !vr_active) /* the motion controllers point */
	{
		if (pointer_on)
			Input_PadPointerClick();
		else
			Input_PadPointerBegin();
		pointer_on = true;
	}

	/* the pointer: squared for fine aim near the middle of the stick */
	if (pointer_on)
	{
		float x = f.axis[pointer_x], y = f.axis[pointer_y];
		float speed = bindings.setting(set_pointer_speed);
		Input_PadPointerMove(x * fabsf(x) * speed * dt, y * fabsf(y) * speed * dt);
	}

	float roll = f.axis[roll_axis], yaw = f.axis[yaw_axis];
	float steer[2] = {f.axis[steer_x] + roll + yaw, f.axis[steer_y]};
	force_elite_controls(yaw != 0.0f ? 2 : roll != 0.0f ? (roll_low() ? 2 : 1) : 0);
	for (float &s : steer)
		s = std::max(-1.0f, std::min(1.0f, s));
	look[0] = f.look_x;
	look[1] = -f.look_y; /* y down + */

	/* the held head: squared like the pointer; a double tap of its hold
	 * control looks ahead */
	if (headlook_tap(&head_tap, f.button[button_head_hold], now))
		headlook_center();
	float hx = f.axis[head_x], hy = f.axis[head_y];
	float hs = bindings.setting(set_head_speed) * dt;
	if (hx != 0.0f || hy != 0.0f)
		headlook_turn(hx * fabsf(hx) * hs, -hy * fabsf(hy) * hs);
	write_steering(steer[0], steer[1]);
	log_steering(steer[0], steer[1], roll, yaw);
	strafe_set_pad(f.axis[strafe_x], f.axis[strafe_y]);
}

void gamepad_look(float *x, float *y)
{
	*x = look[0] * bindings.setting(set_look_yaw) * 0.5f;
	*y = look[1] * bindings.setting(set_look_pitch) * 0.5f;
}

/* =========================================================================
 * The binding editor
 * ========================================================================= */
bool gamepad_editing(void)
{
	return editor && editor->active();
}

bool gamepad_edit_key(int key)
{
	using K = vibe::BindingEditor::Key;
	static const K keys[] = {K::Up, K::Down, K::Select, K::Cancel, K::Reset, K::ResetAll};
	vibe::BindingEditor *e = the_editor();
	if (!e || key < 0 || key > GAMEPAD_EDIT_RESET_ALL)
		return false;
	return e->key(keys[key], now_ns());
}

bool gamepad_edit_pointer(float u, float v, bool click)
{
	vibe::BindingEditor *e = the_editor();
	return e && e->pointer(u, v, click, now_ns());
}

void gamepad_edit_pointer_later(bool on, float u, float v, bool click)
{
	SDL_AtomicLock(&board_lock);
	bool clicked = board_in.click; /* not lost before the update takes it */
	board_in.on = on || clicked;
	if (on)
	{
		board_in.u = u;
		board_in.v = v;
	}
	board_in.click = clicked || (on && click);
	SDL_AtomicUnlock(&board_lock);
}
