/*
 * gl_hands.h - a VR headset's motion controllers as hands that touch the
 * control panel and the virtual screen.
 *
 * HOW IT WORKS
 *   Each controller is a low-poly pointing hand (vibe/hand.h, in
 *   ~/projects/vibe-lib, shared with the other ports; spec: ~/projects/shared/specs/
 *   vr-hand-touch-panel.md) following its aim pose. The surface it can
 *   touch is the control panel in the cockpit, else the virtual screen;
 *   both are brought within arm's reach, scaled about the eye so they look
 *   the same size.
 *
 *   The index fingertip within vr_near_cm of the surface, over it, is the
 *   game's mouse pointer there (input.h) and turns yellow, with a dot
 *   where it will touch. Pushed onto the surface it is the left button
 *   (the tip turns white, the controller buzzes); pulling back 5 mm
 *   releases it. A hand already behind the surface, or reaching in past
 *   its edge, never presses; one behind it is drawn moved back so its tip
 *   rests on it.
 *
 *   Both grips held: moving the hands forward or back pushes the surface
 *   away or pulls it nearer (vr_panel_distance / vr_screen_distance).
 *
 * Drawing thread, per headset frame (from gl_laser.c, when vr_hands):
 *   hands_update(hands, cockpit)   after cockpit_begin_frame
 *   hands_queue(...)               with the cockpit or the overlay
 *   hands_off()                    no headset
 */
#ifndef GL_HANDS_H
#define GL_HANDS_H

#include <stdbool.h>

#include "gl_math.h"
#include "vr.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* Settings (glfrontier.cfg) */
extern bool vr_hands;          /* hands that touch, else laser pointers */
extern int vr_panel_distance;  /* cm, the panel's centre from the eye with hands (0: the usual 62) */
extern int vr_screen_distance; /* cm, the virtual screen's with hands (lasers: 180) */
extern int vr_reach;           /* percent: hand movement scale */
extern int vr_finger;          /* cm: the fingertip beyond the controller's aim point */
extern int vr_near_cm;         /* cm: how near the tip is the pointer */
#define VR_DISTANCE_MIN 25
#define VR_DISTANCE_MAX 200

/* The panel's distance (cm) while the grips gesture moves it, else the
 * setting's */
int hands_panel_distance(void);

/* Touches with the controllers and points the mouse; `cockpit`: the
 * cockpit is drawn per eye this frame (the panel), else the virtual
 * screen. board[i]: hand i is at the clipboard (1) or holds it (2)
 * (gl_clipboard.h): it touches nothing else, and its tip shows that.
 * Returns true while both grips are held (the distance gesture: the grips
 * are not the pad's then). */
bool hands_update(const VrHand hands[2], bool cockpit, const int board[2]);
void hands_off(void);
/* There are hands to draw */
bool hands_shown(void);
/* Queues the hands (gl_draw.h), through `play_to_clip` from the play
 * space, seen from `eye` (play space) */
void hands_queue(const mat4 *play_to_clip, const float eye[3]);

#ifdef __cplusplus
}
#endif

#endif /* GL_HANDS_H */
