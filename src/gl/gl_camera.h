/*
 * gl_camera.h - the cockpit's camera screen: what a camera fixed on the
 * ship sees (front, rear, left, right, up, down), as the original game drew
 * it: its own software renderer, 320x168 pixels in its palette.
 *
 * HOW IT WORKS
 *   After each frame the game flips (game thread, publish_frame in
 *   gl_screen.c), the game's flight view is run once more inside a batch
 *   that puts the machine back afterwards (fe2vm.h), as the cockpit's
 *   screen's world capture does, but with the game's software renderer on
 *   (gl_renderer_on cleared) and the host calls swapped for the batch: the
 *   GL recording ones do nothing, the native scene's hooks say no, the
 *   drawing ones write into the game's screen as in the original, and the
 *   cockpit's pass hook (Lcockpit_world) turns the camera to the chosen
 *   axis for its one pass. Then the game's own palette making and
 *   Draw3DView, then the target (its tunnel and range) and the labels on
 *   things as the game's HUD draws them (Lcam_hud in fe2.s; the own HUD's
 *   hooks say no here), and the screen's top 168 rows are kept, in
 *   colour. It
 *   does not depend on the head or on the cockpit's own view.
 *
 *   The flight view's drawing clears the shots it shows (lasers, hits:
 *   object byte +285), so the batch puts back what the frame had first
 *   (camera_note_shots). The native scene's far hook (Nu_NativeFar, every
 *   object before the cull) says no too, but notes where each thing the
 *   flight view lets you click on is in the picture: its centre, where
 *   its label starts, the one point a tap picks it by.
 *
 *   The seventh button is the target view: the batch moves the ship up to
 *   the target's distance / 2^zoom (the + and - buttons, 8x at first, never
 *   nearer than 250 m of a ship or 1.5 radii of a body), a run with only the
 *   target drawn finds where it is in the view, and the camera's axes turn
 *   to face it with the ship's up kept. The ship is put back before the
 *   target's range is drawn, so that is the true one. The game's own
 *   relative vector (Lcam_relvec) does the distances.
 *
 *   gl_cockpit.c shows it on a screen beside the cockpit's screen; a tap
 *   on it (mouse, laser, hand) on a thing makes that the target, else goes
 *   to the next direction.
 */
#ifndef GL_CAMERA_H
#define GL_CAMERA_H

#include <stdbool.h>

#include "gl_api.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define CAMERA_W 320
#define CAMERA_H 168

/* Setting: the camera screen in the cockpit */
extern bool cockpit_camera;

/* With the GL context */
void camera_init(void);
void camera_shutdown(void);

/* Game thread, after a frame that shows the cockpit is published: draws
 * the camera's picture (no more than about 50 times a second). `screen`:
 * the frame is a game screen's, for the cockpit's screen (the picture then
 * has the last flight frame's colours) */
void camera_capture(bool screen);

/* Any thread: a tap on the picture at (u, v) (0..1, from its top left):
 * at the centre of a planet, a ship, a station, cargo..., that becomes the target, as
 * clicking it in the flight view does; else on to the next direction */
void camera_tap(float u, float v);

/* Game thread, as the flight view starts drawing its objects
 * (Call_CockpitPass): notes the shots they are to show, which the drawing
 * clears, for the picture drawn after it */
void camera_note_shots(void);
/* Game thread, under the cockpit's screen after the world round it was
 * drawn: the shots cleared as the flight view's drawing would */
void camera_shots_drawn(void);

/* Whoever draws: the latest picture's texture (top row first), 0 while
 * there is none */
GLuint camera_texture(void);

#ifdef __cplusplus
}
#endif

#endif /* GL_CAMERA_H */
