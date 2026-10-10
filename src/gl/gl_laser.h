/*
 * gl_laser.h - a VR headset's motion controllers as laser pointers.
 *
 * HOW IT WORKS
 *   Each controller (vr.h) casts a beam from its aim pose. The one last
 *   clicked with points the game's mouse: where its beam meets what the
 *   game draws on, as the window's mouse is mapped (in the cockpit: the
 *   control panel, else the HUD's directions; otherwise the virtual
 *   screen), is the pointer (input.h). A trigger pulled with its beam on
 *   the panel or a screen, or with the grip held (to click on things out
 *   in space: targets), is the left button there; otherwise it is the
 *   pad's trigger. The controllers' sticks and buttons are a virtual pad
 *   in the Xbox layout (gamepad.h), so the gamepad's bindings fly the ship.
 *
 *   The beams are drawn in the cockpit (cockpit_draw) and, over the
 *   virtual screen, in a layer of their own (laser_draw_overlay).
 *
 * Drawing thread, per headset frame:
 *   laser_update(cockpit)   after cockpit_begin_frame (and vr_begin_frame)
 *   laser_off()             no headset
 */
#ifndef GL_LASER_H
#define GL_LASER_H

#include <stdbool.h>

#include "gl_math.h"
#include "vr.h"

/* Reads the controllers and points the mouse; `cockpit`: the cockpit is
 * drawn per eye this frame, else the virtual screen */
void laser_update(bool cockpit);
void laser_off(void);
/* There are beams to draw */
bool laser_shown(void);
/* Queues the beams (gl_draw.h), through `play_to_clip` from the play space,
 * facing the eye at `eye` (play space) */
void laser_queue(const mat4 *play_to_clip, const float eye[3]);
/* Draws the beams alone into the bound framebuffer (w x h, cleared to
 * nothing first), as headset eye `v` sees them */
void laser_draw_overlay(int w, int h, const VrView *v);

#endif /* GL_LASER_H */
