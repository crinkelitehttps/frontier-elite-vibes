/*
 * gl_cockpit.h - the 3D cockpit view: the flight view seen from a seat in
 * the ship, looking around with head tracking (headtrack.c).
 *
 * HOW IT WORKS
 *   The world fills the window with a real perspective and the head's
 *   rotation. The game's own 2D drawing over its 3D view (HUD, target
 *   brackets, planets and atmosphere, which the game draws as 2D shapes)
 *   lives in "view pixels": x 0..320, y 0..168 downwards, each pixel a
 *   direction through the game's original projection. In the cockpit those
 *   pixels are put back on that direction, far away and turning with the
 *   world, so they stay on what they mark (a collimated display). The
 *   control panel (screen rows 168..200) is a texture on a panel below,
 *   which also moves with the head's position. Mouse positions are mapped
 *   back through the same surfaces.
 *
 *   The game itself only draws what is inside its own view (about 64 x 36
 *   degrees). To show the world all round, the game draws it several times
 *   a frame with the camera turned (a hook in fe2.s calls Call_CockpitPass
 *   around each pass): only the directions the head can see this frame,
 *   from a fixed set that covers the whole sphere. Each pass is its own
 *   depth tree in gl_scene.c, drawn turned back by its rotation and kept by
 *   the stencil buffer to its own part of the view. The game's 2D shapes
 *   only fit the view it made them for, so would jog where passes meet:
 *   in the cockpit, planets are drawn as spheres (gl_planet.c) and their
 *   atmosphere bands as rings round them (gl_atmos.c) instead.
 *
 *   Only in the front flight view, when the cockpit is enabled and the free
 *   camera is off; everything else keeps the classic letterboxed layout.
 *
 *   With a VR headset (vr.h) the head pose is the headset's, and the
 *   frame is drawn again for each eye (cockpit_use_eye: its off-centre
 *   lens and position). The world and the HUD are at infinity, so the
 *   same in both eyes; the panel is near, in stereo. The passes are chosen
 *   to cover the eyes as well as the window.
 */
#ifndef GL_COCKPIT_H
#define GL_COCKPIT_H

#include <stdbool.h>

#include "gl_api.h"
#include "gl_math.h"
#include "vr.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* The game's own projection of its 3D view (gl_screen.c's classic layout) */
#define CLASSIC_FOV    36.5f /* vertical, degrees */
#define CLASSIC_ASPECT 1.9f

/* The emulated screen and its texture (gl_screen.c) */
#define GL_SCREEN_W     320
#define GL_SCREEN_H     200
#define GL_SCREEN_TEX_W 512
#define GL_SCREEN_TEX_H 256
void gl_screen_upload(void);
GLuint gl_screen_texture(void);

/* Settings */
extern bool cockpit_enabled;
extern int cockpit_fov;      /* vertical field of view, degrees */
extern int vr_depth;         /* stereo depth, percent: 100 = the game's world at life size */
extern bool headtrack_enabled;
extern int headtrack_port;

/* With the GL context (Screen_Init / Screen_UnInit) */
void cockpit_init(void);
void cockpit_shutdown(void);

/* Game thread, at the end of each frame the game draws: whether it is
 * drawn as the cockpit (and the next one gets the turned passes) */
bool cockpit_game_frame(void);

/* Once per drawing, before it: polls the head tracker and sets up the
 * view for a frame the game drew as the cockpit (`cockpit`) or not.
 * Returns `cockpit`. */
bool cockpit_begin_frame(int window_w, int window_h, bool cockpit);
/* What the last cockpit_begin_frame was told */
bool cockpit_active(void);
/* Whether the frame last shown is the cockpit; any thread */
bool cockpit_shown(void);

/* A VR headset (vr.h), before cockpit_begin_frame: this frame's head pose
 * is `centre`'s instead of the head tracker's, and the turned passes
 * cover `eyes` too. NULL: no headset this frame. */
void cockpit_set_headset(const VrView *centre, const VrView eyes[2]);
/* Draws what follows (projections, cockpit_draw, the passes) as seen by a
 * headset's eye, with its lens; NULL: back to the window */
void cockpit_use_eye(const VrView *eye);

/* Projection for the 3D scene: lens * head rotation */
mat4 cockpit_world_projection(void);
/* The same, split: the lens alone and the head rotation (view matrix),
 * which includes the turn of the pass being drawn */
void cockpit_world_split(mat4 *lens, mat4 *head_rotation);
/* The head rotation alone, without the pass's turn */
mat4 cockpit_head_rotation(void);

/* Projection for 2D drawing in view pixels (x 0..320, y 0..168 down, z 0),
 * cockpit or not, to be used with the GD_VP_VIEW3D viewport */
mat4 cockpit_view_pixel_projection(void);

/* Queues the HUD plane and the panel */
void cockpit_draw(void);

/* Turned passes (see HOW IT WORKS); ids 0 .. COCKPIT_MAX_PASSES-1, 0 is
 * straight ahead. The host call, d0 = 0 to start and 1 after each pass:
 * returns d0 = 1 if the game is to draw a pass with the camera at a3. */
#define COCKPIT_MAX_PASSES 32
void Call_CockpitPass(void);
/* Host call: the game has drawn the background of the pass (Lcockpit_world) */
void Call_CockpitBackground(void);
/* Host call: places the sky model for the pass (Lcockpit_sky) */
void Call_CockpitSky(void);
/* Host call: a type $d / $e vertex in a turned pass (Lcockpit_vertex_d) */
void Call_CockpitVertexD(void);
/* True while the game draws a pass other than straight ahead */
bool cockpit_turned_pass(void);
/* A direction in the view space of the pass the game is drawing -> view
 * space (unchanged outside the passes) */
void cockpit_pass_to_view(const float v[3], float out[3]);
/* Replay: draw what follows as pass `id`; then back to normal */
void cockpit_pass_draw_begin(int id);
void cockpit_pass_draw_end(void);

/* Window pixel (drawable pixels, y down) -> emulated screen pixel
 * (320x200). False if it points at nothing the game draws on. */
bool cockpit_window_to_screen(int wx, int wy, int *sx, int *sy);

#ifdef __cplusplus
}
#endif

#endif /* GL_COCKPIT_H */
