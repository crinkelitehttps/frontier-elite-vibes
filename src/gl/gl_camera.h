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
 *   gl_cockpit.c shows it on a screen beside the cockpit's screen; a tap
 *   on it (mouse, laser, hand) goes to the next direction.
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

/* Any thread: on to the next direction */
void camera_next(void);

/* Whoever draws: the latest picture's texture (top row first), 0 while
 * there is none */
GLuint camera_texture(void);

#ifdef __cplusplus
}
#endif

#endif /* GL_CAMERA_H */
