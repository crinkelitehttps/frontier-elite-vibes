/*
 * gl_screen_place.h - the cockpit's screens (the game's screens and the
 * camera screen, gl_cockpit.h) picked up and put anywhere with a VR
 * headset's hands, and the maps turned with the left trigger.
 *
 * HOW IT WORKS
 *   A grip pressed with the hand within PLACE_REACH of a screen (its
 *   palm or fingertip, from the rectangle or its edges) takes that screen:
 *   it follows the hand as if held at that point, so it moves, turns,
 *   tilts, and comes nearer or goes farther with it. The other grip
 *   pressed near the same screen holds it with both hands: then the
 *   distance between the hands sizes it, and it turns with the line
 *   between them. Letting go of one hand goes back to the other holding
 *   it. Let go, it stays there, kept in glfrontier.cfg (screen_place_*,
 *   view space) for the next start; until it is first moved it has its
 *   usual place above the panel. screen_place_reset() puts both back.
 *
 *   While a map (the galaxy's or the system's) is on the cockpit's
 *   screen, the left trigger is the map's: held, either hand touching the
 *   screen (within TURN_TOUCH) turns the map, its movement across the
 *   screen a right-button mouse drag (the game turns its maps by dragging
 *   with the right button), until it leaves (TURN_KEEP). The turning hand
 *   presses nothing. Under a screen the ship ignores mouse steering
 *   (Call_ScreenControls), so the drag doesn't turn it too.
 *
 *   A grip or trigger taken this way is the screen's until it is let go:
 *   the bindings (the virtual pad: left grip and trigger are flight
 *   controls) and the two-grip distance gesture don't see it, and a hand
 *   holding a screen touches nothing.
 *
 * Drawing thread, per headset frame (gl_laser.c), after the clipboard and
 * before the hands: screen_place_hands(hands, cockpit, held).
 */
#ifndef GL_SCREEN_PLACE_H
#define GL_SCREEN_PLACE_H

#include <stdbool.h>

#include "vr.h"

#ifdef __cplusplus
extern "C"
{
#endif

enum
{
	PLACE_SCREEN, /* the game's screens (maps, station, ship, options) */
	PLACE_CAMERA, /* the camera screen */
	PLACE_COUNT
};

/* Where a screen was put, in the cockpit's view space (m, x right, y up,
 * -z ahead) as whole numbers for glfrontier.cfg: on = 0 is its usual
 * place */
typedef struct
{
	int on;
	int x, y, z;        /* centre, mm */
	int qx, qy, qz, qw; /* turn from facing +z (towards the eye), x 10000 */
	int w;              /* width, mm */
} ScreenPlace;
extern ScreenPlace screen_place[PLACE_COUNT];

/* Both screens back to their usual places (any thread) */
void screen_place_reset(void);

/* Drawing thread (gl_cockpit.c, as the cockpit is built): `c` (view
 * space; top left, top right, bottom left, bottom right) is the screen's
 * usual place; replaced by where it was put, or where it is being
 * carried, keeping its height / width */
void screen_place_corners(int screen, float c[4][3]);

/* Drawing thread, per headset frame: grabs, carries and lets go, and turns
 * the maps. `cockpit`: the cockpit is drawn per eye. Clears the squeeze /
 * trigger of controls the screens have taken; held[i]: hand i holds a
 * screen (2) or turns a map (1): it touches nothing else. */
void screen_place_hands(VrHand hands[2], bool cockpit, int held[2]);

#ifdef __cplusplus
}
#endif

#endif /* GL_SCREEN_PLACE_H */
