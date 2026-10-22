/*
 * gl_screen_edit.h - moving, turning and sizing the cockpit's screens (the
 * game's screens and the camera screen, gl_cockpit.h) with the mouse and
 * keyboard, the flat-mode counterpart of the VR hands (gl_screen_place.h).
 *
 * HOW IT WORKS
 *   Ctrl-G (or the menu's button) switches the editor on and off. While it
 *   is on the mouse belongs to it and not to the game: the mouse pointer
 *   resting on a screen picks it, and it stays the picked one when the
 *   pointer leaves (its outline is bright, the others' dim), so only that
 *   screen can be changed, one at a time. A gizmo on it, as in a graphics
 *   editor, shows one kind of change; drag one of its handles to change
 *   one axis at a time:
 *     W  move:   an arrow per axis (red x right, green y up, blue z
 *                towards you) slides the screen along that axis
 *     E  turn:   a ring per axis turns the screen about that axis
 *     R  size:   a dot on each corner, dragged out or in, sizes it
 *   (Tab goes through the three.) Delete puts the picked screen back
 *   above the panel; Esc or Ctrl-G leaves. The handle under the pointer is
 *   yellow, the one being dragged white. The places are kept as the VR
 *   hands' are (glfrontier.cfg screen_place_*), and a screen that is not
 *   shown (a map that isn't up) cannot be picked.
 *
 * The events come from the main thread (screen_edit_event); everything else
 * runs on the drawing thread, per frame, in cockpit_begin_frame
 * (screen_edit_update) and cockpit_draw (screen_edit_queue).
 */
#ifndef GL_SCREEN_EDIT_H
#define GL_SCREEN_EDIT_H

#include <SDL.h>
#include <stdbool.h>

#include "gl_math.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* Main thread: whether the editor took the event (while on, all mouse
 * events and its keys) */
bool screen_edit_event(const SDL_Event *event);

/* Main thread: the editor on / off, for the menu's button */
bool screen_edit_on(void);
void screen_edit_toggle(void);

/* Drawing thread, once a frame as the cockpit's view is set: `cockpit`:
 * the cockpit is drawn this frame, `eye`: the eye in view space */
void screen_edit_update(bool cockpit, const float eye[3]);

/* Drawing thread: the outlines and gizmo, with the cockpit's projection
 * `cp` (view space) */
void screen_edit_queue(const mat4 *cp, const float eye[3]);

#ifdef __cplusplus
}
#endif

#endif /* GL_SCREEN_EDIT_H */
