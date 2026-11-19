/*
 * gl_focus.h - moving about the cockpit's input surfaces without the mouse:
 * the keyboard's Tab / Shift-Tab and arrows and F, the pad's Y / X and d-pad
 * and A (gamepad.cfg: focus-next, focus-prev, nav-up ..., activate).
 *
 * HOW IT WORKS
 *   Five surfaces take presses: the DASHBOARD (the control panel), COMS, MAP,
 *   CAMERA and SHIP (the screens, gl_cockpit.h). Tab goes clockwise round them
 *   as the eye sees them where they are placed now (worked out at each press,
 *   so it follows screens that were moved), Shift-Tab anticlockwise, skipping
 *   the ones not shown. The
 *   focused surface has a green outline; a green box sits on one of its
 *   buttons and the arrows move it to the nearest button that way (the system
 *   map's bodies are buttons too). F presses the button under it, held as long
 *   as F is, by the path a click on it takes:
 *     - the dashboard, a screen the game has up, the flight comms menu:
 *       Input_ClickAt, the game's own mouse (with the dashboard's host buttons
 *       taken in Call_GetMouseInput);
 *     - the MAP and SHIP screens that run on the side (gl_mapscreen.h):
 *       mapscreen_pointer on a source of its own, pressed and let go a moment
 *       later (focus_tick);
 *     - a dimmed screen: the tap that brings it up (cockpit_host_tap);
 *     - the camera screen: camera_tap.
 *   Every list of buttons is made fresh from the game's icon rectangles and
 *   the host's own buttons, so the box only has to remember which one it was on
 *   (by icon number, and where).
 *
 *   Only while the cockpit's view is shown (the box is drawn by it, cockpit_draw)
 *   and the free camera is off. Everything runs on the main thread (events
 *   and focus_tick); the drawing thread reads focus_view.
 */
#ifndef GL_FOCUS_H
#define GL_FOCUS_H

#include <SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* What cockpit_draw draws: the quad (cockpit_focus_point's) of the focused
 * surface, and the box on its button (page pixels) if it has one */
typedef struct
{
	int quad;
	bool has_box;
	int x, y, w, h;
} FocusView;
bool focus_view(FocusView *out);

/* Main thread: focus the next surface clockwise / the one before */
void focus_next(void);
void focus_prev(void);
/* Move the box: dx, dy one of -1, 0, 1 (x right, y down) */
void focus_move(int dx, int dy);
/* The button under the box pressed (down) and let go: held as long as the key or pad button is (the map's zoom
 * and turn buttons go on while held) */
void focus_press(bool down);

/* Main thread, once a frame: keeps the box on its button as the lists change,
 * lets go of presses */
void focus_tick(void);

/* Seconds the green outline and box stay up after the last focus key (Tab / arrows / F, pad Y / X /
 * d-pad / A); 0 = always shown. A setting (focus_highlight_seconds). */
extern int focus_highlight_seconds;

/* Whether the focus is in use (the cockpit view is shown and the free camera is off) */
bool focus_enabled(void);

/* Main thread: a key event. True if it is the focus's (Tab, the arrows and F, with no
 * Ctrl / Alt / GUI), key up as well as down */
bool focus_key_event(const SDL_Event *event);

#ifdef __cplusplus
}
#endif

#endif /* GL_FOCUS_H */
