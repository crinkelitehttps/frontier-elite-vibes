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
 *
 * The SHIP screen (the ship screen module, L7ded6) runs the same way. Its
 * state is mostly the game's own (the cargo lists, the money owed), which the
 * batch reads live; what it keeps is its page and the angles the ship
 * turns at. Its page is cycled by a tap away from the buttons, or F3, as F3
 * does in the game; a tap on one of the page's own buttons (the cargo to
 * jettison, the equipment to use) does something to the game and the batch
 * undoes what it does, so the real screen is opened on the same page for it.
 */
#ifndef GL_MAPSCREEN_H
#define GL_MAPSCREEN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Settings: the map and the ship screens live in flight */
extern bool cockpit_map_live;
extern bool cockpit_ship_live;

typedef struct
{
	uint8_t screen[320 * 200]; /* palette indices, 255 transparent */
	unsigned short main_pal[256];
	int len_main;
	unsigned short ctrl_pal[16];
	int bgcol;
} MapPage;

/* Input for the screens (groups COCKPIT_SCREEN_MAP, COCKPIT_SCREEN_SHIP), from
 * any thread. The pointer is on the screen `group` (-1: on none) at page
 * pixel x, y (0..319, 0..167), the game's buttons held (1 right,
 * 2 left) and the mouse's motion since the last call (dx, dy, what the
 * right button drags the map with); the batch plays it back as the game's
 * vblank handler would have (UpdateMouse), once per press, and nothing of
 * it reaches the flight view. */
#define MAPSCREEN_SOURCES 2 /* pointers: 0 the mouse (game thread), 1 the hands (render thread) */
void mapscreen_pointer(int source, int group, int x, int y, int buttons, int dx, int dy);
/* A drag that turns the map (the right button with a mouse; a hand or a
 * controller's own): held now, and its motion since the last call in mouse
 * units */
void mapscreen_drag(bool held, int dx, int dy);
/* A key for the screen (ST scancode), as if pressed with it up */
void mapscreen_key(int group, unsigned char scancode);
/* The screens can be up (in flight with the cockpit's screens, or beside the
 * station's services), and the one of `group` is (its setting on too) */
bool mapscreen_up(void);
/* Game thread, each mouse poll: finishes a press handed over to the game's own
 * screen */
void mapscreen_poll(void);
/* ... and it is waiting for the real map: F2 is the game's meanwhile */
bool mapscreen_handing(void);
bool mapscreen_group_up(int group);
/* In flight the dashboard's buttons 1 to 4 show and hide the camera, MAP, SHIP
 * and COMS screens (the game's own F2, F3, F4 are the screens' to touch now). A
 * hidden screen is not drawn and cannot be touched. `group` is hidden now
 * (false off the flight view); shown again (the key for it does that too) */
bool mapscreen_hidden(int group);
void mapscreen_show(int group);
/* Game thread: a press at screen pixel (x, y) of the real flight panel: if it
 * is on one of those buttons, it toggles the screen and is done (true), the
 * game is not to see it */
bool mapscreen_dashboard_press(int x, int y);
/* The dashboard's buttons are those (the cockpit's screens, in flight): the
 * workarounds that make button 2 the game's F2 are off then */
bool mapscreen_dashboard_up(void);
/* The screen is being used: the pointer is on it, or it was a moment ago */
bool mapscreen_pointed(int group);

/* Game thread, in the flip of a cockpit frame of the flight view: the group
 * whose page is wanted now (enough time has passed), -1 if none */
int mapscreen_due(void);

/* Runs the group's page as above; false when it could not (the picture is
 * then not to be used, and the machine and the host are as they were) */
bool mapscreen_page(int group, MapPage *out);

/* The MAP screen shows the map's view only: the panel's buttons (the map
 * controls, the zoom and the time controls, the legend) are moved onto its
 * lower right, from where the game has them (source: panel rows 168..199)
 * to (dx, dy), w x h. A pointer there is put back where the button was. */
#define MAPSCREEN_BUTTONS 16
typedef struct
{
	int sx, sy, w, h, dx, dy;
	int id; /* the game's icon number */
} MapButton;
int mapscreen_buttons(MapButton out[MAPSCREEN_BUTTONS]);

/* The maps' pan (galaxy) or turn (system) buttons, up, down, left, right from the top down, in a
 * stack above the rightmost button: x, y, w, h on the screen each, 0 of them
 * when the view is neither map. A press holds the arrow key (galaxy) or a right
 * drag that way (system). */
#define MAPSCREEN_PANS 4
int mapscreen_pans(int out[MAPSCREEN_PANS][4]);

/* The game's own map screen is up and its buttons are moved too (the picture
 * is drawn with them); a point on the screen, put back where the game has it */
bool mapscreen_real_buttons(void);
void mapscreen_unmove(int *x, int *y);

/* Forgets what the map was showing (a new game was loaded) */
void mapscreen_forget(void);

#ifdef __cplusplus
}
#endif

#endif
