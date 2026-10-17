/*
 * gl_mapscreen.h - the cockpit's MAP screen kept live while the game stays
 * in the flight view: the galaxy map and the system map run on the side.
 *
 * The game has one screen owner at a time (A6+6): the flight view, a map,
 * the ship screen... and the cockpit's other screens were a view of
 * whichever it was, so opening a map left the flight view and the world
 * was captured behind it. Here the owner stays the flight view. The map is
 * a module of its own (L820ee galaxy, L4b1ca system), with its state in a
 * few hundred bytes of A6 and everything it draws going through the same
 * host calls as the flight view, so what the game does for a frame of it,
 * one game loop turn without the physics, is run in a batch that puts the
 * machine back afterwards (fe2vm.h, as the camera screen's picture is made):
 *
 *   the map module's state put in (kept from the last batch), the module
 *   entered as F2 would (its own fn 8, so its panel, palette and icons are
 *   set), then input (a tap: the game's mouse routine with the position
 *   and buttons, as its vblank handler would), UIKeyboardPoll, the modules'
 *   fn 16, the HUD functions and FlipScreen, whose Nu_DrawScreen is ours:
 *   it takes the finished 320x200 picture and its palettes. The scene the
 *   drawing recorded (the map's objects, planets) is published as a game
 *   screen's, with the flight frame as the world (gl_screen.c).
 *
 * The page is made 2 times a second, or 50 while it is the one being used.
 */
#ifndef GL_MAPSCREEN_H
#define GL_MAPSCREEN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Setting: the map screen live in flight */
extern bool cockpit_map_live;

typedef struct
{
	uint8_t screen[320 * 200]; /* palette indices, 255 transparent */
	unsigned short main_pal[256];
	int len_main;
	unsigned short ctrl_pal[16];
	int bgcol;
} MapPage;

/* Input for the screen, from any thread. The pointer is on the screen (on)
 * at page pixel x, y (0..319, 0..167), the game's buttons held (1 right,
 * 2 left) and the mouse's motion since the last call (dx, dy, what the
 * right button drags the map with); the batch plays it back as the game's
 * vblank handler would have (UpdateMouse), once per press, and nothing of
 * it reaches the flight view. */
#define MAPSCREEN_SOURCES 2 /* pointers: 0 the mouse (game thread), 1 the hands (render thread) */
void mapscreen_pointer(int source, bool on, int x, int y, int buttons, int dx, int dy);
/* A drag that turns the map (the right button with a mouse; a hand or a
 * controller's own): held now, and its motion since the last call in mouse
 * units */
void mapscreen_drag(bool held, int dx, int dy);
/* A key for the screen (ST scancode), as if pressed with it up */
void mapscreen_key(unsigned char scancode);
/* The arrow keys held for the screen (a controller's stick), and whether the
 * stick is the map's: a press on the screen takes it, one elsewhere gives
 * it back */
void mapscreen_scroll(bool up, bool down, bool left, bool right);
void mapscreen_grab(bool on);
bool mapscreen_grabbed(void);
/* The screen is up (the setting on, in flight with the cockpit's screens) */
bool mapscreen_up(void);
/* The screen is being used: the pointer is on it, or it was a moment ago */
bool mapscreen_pointed(void);

/* Game thread, in the flip of a cockpit frame of the flight view: a page is
 * wanted now (the map screen is on, enough time has passed) */
bool mapscreen_due(void);

/* Runs the page as above; false when it could not (the picture is then not
 * to be used, and the machine and the host are as they were) */
bool mapscreen_page(MapPage *out);

/* Forgets what the map was showing (a new game was loaded) */
void mapscreen_forget(void);

#ifdef __cplusplus
}
#endif

#endif
