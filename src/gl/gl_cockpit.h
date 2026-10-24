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
 *   world, so they stay on what they mark (a collimated display). (With a
 *   headset's hands a menu in them, the comms menu, is on the virtual
 *   screen's plane within reach instead: see gl_hands.h.) The
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
 *   camera is off; everything else keeps the classic letterboxed layout,
 *   except the game's other screens (maps, the station's, the ship's,
 *   options) with the native scene (gl_native.h) and cockpit_screen on:
 *   the cockpit stays, with the world live round it (cockpit_capture_world),
 *   and the screen's picture (drawn the classic way into a texture,
 *   gl_screen.c) is on a screen in the cockpit, above the panel, its top
 *   168 rows. Each group of the game's screens (COCKPIT_SCREEN_*: what its
 *   keys 2, 3 and 4 cycle through) has a screen of its own; the game only
 *   runs one at a time, so the others show the last picture they had,
 *   dimmed, and a tap on one is the host's: it presses that group's key,
 *   which brings it up. The live screen is touched like the panel (mouse,
 *   lasers, hands); on the SHIP group's, a tap on none of the game's
 *   buttons presses its key too, which shows the next page. VR hands can
 *   move them and the camera screen anywhere (gl_screen_place.h).
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
/* A menu drawn over the flight view (the comms menu) takes screen rows
 * *y0 .. *y1 */
bool gl_screen_menu_rows(int *y0, int *y1);
/* The game's screen drawn the classic way for the cockpit's screen of its
 * group (COCKPIT_SCREEN_*), the last picture it had: the 3D view and its
 * 2D layer (rows 0..168) between texture v GL_SCREEN_SUB_V0 and 1; 0 if it
 * never had one */
GLuint gl_screen_sub_texture(int group);
#define GL_SCREEN_SUB_V0 (1.0f - 168.0f / 200.0f)
/* The COMS screen's picture while undocked and the screen isn't live: the
 * flight comms menu's text on nothing (the texture, else 0 when there is no
 * menu); and whether the picture is such (transparent where the alpha is 0,
 * and not the station's page it last had) */
GLuint gl_screen_coms_menu(void);
bool gl_screen_coms_clear(void);
/* A saved game or a new one was loaded: no station's services are on the COMS screen */
void gl_screen_coms_forget(void);
/* With the cockpit's screen: the labels on things in the world round it,
 * the flight view's text layer, laid out as the emulated screen's
 * texture (transparent where there is none); 0 when there are none */
GLuint gl_screen_labels_texture(void);

/* Settings */
extern bool cockpit_enabled;
extern bool cockpit_screen; /* the game's other screens on the cockpit's screen */
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
/* Game thread, the same: whether the frame is one of the game's other
 * screens, for the cockpit's screens (see HOW IT WORKS), and which
 * group's (-1: none) */
/* The MAP and SHIP screens up in flight (gl_mapscreen.h): the screen's group
 * and the pixel of its page (0..319, 0..199) a window pixel / a ray (the play
 * frame, as cockpit_ray_to_screen) is on; false if not on one or none is up */
bool cockpit_window_to_side(int wx, int wy, int *group, int *px, int *py);
/* The ray (view space: origin the eye, direction not unit) through window
 * pixel (wx, wy), as last drawn; false when the cockpit isn't shown */
bool cockpit_pixel_ray(int wx, int wy, float o[3], float d[3]);
bool cockpit_ray_to_side(const float origin[3], const float dir[3], int *group, int *px, int *py, float *dist);
bool cockpit_screen_frame(void);
/* Whether the cockpit's screens are in use at all (the group a game
 * screen would go to, though the flight view is up now) */
bool cockpit_screens_on(void);
enum
{
	COCKPIT_SCREEN_MAP,  /* the galaxy and system maps (key 2) */
	COCKPIT_SCREEN_SHIP, /* the ship's pages (key 3) */
	COCKPIT_SCREEN_COMS, /* the station's services (key 4), and options */
	COCKPIT_SCREENS
};
int cockpit_screen_group(void);
/* Game thread, for such a frame, once the game has drawn it (in a host
 * call): runs the game's flight view as the cockpit would have it, with
 * the game's machine put back as it was afterwards (fe2vm.h), so the
 * world is recorded (gl_scene.h, scene_publish_world) though the game
 * shows the screen. False if there is none to capture. The flight view's
 * text (labels on things) is left queued (screen_text.h) for the caller. */
bool cockpit_capture_world(void);
/* Host call (Lscreen_controls in fe2.s): d0 = 1 if the player's controls
 * are to work though a game screen is up, as it is on the cockpit's */
void Call_ScreenControls(void);
/* Host call (Ldust_gate in fe2.s): d0 = 1 if the world goes on round the
 * cockpit's screen (dust and clouds are made), without the controls */
void Call_ScreenWorld(void);

/* Once per drawing, before it: polls the head tracker and sets up the
 * view for a frame the game drew as the cockpit (`cockpit`), or for the
 * cockpit's screens with group `screen` live (COCKPIT_SCREEN_*), or
 * neither (-1). Returns whether it is the cockpit. */
bool cockpit_begin_frame(int window_w, int window_h, bool cockpit, int screen);
/* Draws what follows as not the cockpit (the cockpit's screen's picture),
 * until called with false */
void cockpit_draw_flat(bool flat);
/* What the last cockpit_begin_frame was told */
bool cockpit_active(void);
/* Whether the frame last shown is the cockpit; any thread */
bool cockpit_shown(void);

/* A VR headset (vr.h), before cockpit_begin_frame: this frame's head pose
 * is `centre`'s instead of the head tracker's, and the turned passes
 * cover `eyes` too. NULL: no headset this frame. */
void cockpit_set_headset(const VrView *centre, const VrView eyes[2]);
/* The emulator menu's picture (ui_render_vr) for its screen in the cockpit
 * (PLACE_MENU), set before cockpit_begin_frame; 0: the menu isn't there */
void cockpit_set_menu_texture(GLuint tex);
/* A hand (0, 1) is pointing at the menu's screen at u, v (0..1 from its top
 * left), pressing or not; or no longer (on false). Drawing thread. */
void cockpit_menu_pointer(int hand, bool on, float u, float v, bool down);
/* The pointer the menu has now: the pressing hand, else a pointing one */
bool cockpit_menu_pointer_get(float *u, float *v, bool *down);
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

/* Projection for things in a headset's play space (vr.h: metres, as the
 * seat is) as the cockpit is being drawn, with the look-around; and
 * where the eye is, in the play space */
mat4 cockpit_play_projection(float eye_play[3]);
/* Projection for the play space as headset eye `v` sees it (not the
 * cockpit: no look-around) */
mat4 cockpit_headset_projection(const VrView *v);

/* Projection for 2D drawing in view pixels (x 0..320, y 0..168 down, z 0),
 * cockpit or not, to be used with the GD_VP_VIEW3D viewport */
mat4 cockpit_view_pixel_projection(void);
/* Projection for view space directions at infinity (x right, y up, -z
 * ahead; put them inside the world's near/far, 1 .. 1e10): the cockpit's
 * world without the eye's offset, or the classic 3D view's */
mat4 cockpit_direction_projection(void);

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
/* The same for a ray in a headset's play space (a controller's laser), as
 * the cockpit is being drawn; *dist: how far along `dir` it hits the panel
 * (-1: the HUD, at infinity) */
bool cockpit_ray_to_screen(const float origin[3], const float dir[3], int *sx, int *sy, float *dist);
/* The control panel's corners in a headset's play space (top left, top
 * right, bottom left, bottom right), as the cockpit is being drawn; it
 * shows screen rows COCKPIT_PANEL_ROW0 .. GL_SCREEN_H */
#define COCKPIT_PANEL_ROW0 168
bool cockpit_panel_play(float corners[4][3]);
/* The same for the cockpit's live screen while it is shown (screen rows
 * 0 .. COCKPIT_PANEL_ROW0) */
bool cockpit_screen_play(float corners[4][3]);
/* The MAP screen up in flight (gl_mapscreen.h), in the play space as the
 * above; its page's rows are all 0 .. GL_SCREEN_H */
bool cockpit_map_play(float corners[4][3]);
/* ... and the SHIP screen */
bool cockpit_ship_play(float corners[4][3]);
/* The camera screen (gl_camera.h) while it is shown, the same way */
bool cockpit_camera_play(float corners[4][3]);
/* The same for screen `place` of gl_screen_place.h (PLACE_*), live or not */
bool cockpit_place_play(int place, float corners[4][3]);
/* The screens a tap on is the host's, not the game's: the camera screen
 * and the groups' screens that aren't live, while shown, the same way,
 * up to COCKPIT_HOST_MAX; ids for cockpit_host_tap. Returns how many. */
#define COCKPIT_HOST_MAX 5
#define COCKPIT_HOST_MENU (COCKPIT_SCREENS + 1) /* the emulator menu's screen */
#define COCKPIT_HOST_IDS (COCKPIT_SCREENS + 2) /* ids are 0 .. this - 1 */
int cockpit_host_screens(float corners[][4][3], int ids[], int max);
/* A tap on host screen `id` at u, v (0..1 from its top left): the camera
 * screen's camera_tap, or a group's key. Any thread. */
void cockpit_host_tap(int id, float u, float v);
/* A press on the live screen at screen pixel (sx, sy): true if it is the
 * host's (on none of the game's buttons on a screen whose pages a tap
 * cycles), and then done: the game is not to see it. Any thread. */
bool cockpit_screen_press(int sx, int sy);
/* Whether the galaxy map on the cockpit's screen has the left stick (it
 * scrolls the map); a touch on the screen toggles it, unless the map is in
 * select mode. Always true off the galaxy map. */
bool cockpit_map_focused(void);
/* The MAP screen is up beside the flight view (it runs on the side) */
bool cockpit_side_map_shown(void);
/* Game thread: a left click at screen pixel (sx, sy) went to the game (to
 * follow the map's select mode) */
void cockpit_map_click(int sx, int sy);
/* Places the screens again, after one was moved (gl_screen_place.h) */
void cockpit_place_screens(void);
/* A ray in the play space, as cockpit_ray_to_screen: does it hit a host
 * screen (cockpit_host_screens), which (*id), how far along `dir`, and
 * where on it (cockpit_host_tap's u, v) */
bool cockpit_ray_hits_host(const float origin[3], const float dir[3], float *dist, float *u, float *v, int *id);
/* Main thread, a mouse click at window pixel (wx, wy): if it is the
 * host's (a host screen, or cockpit_screen_press), done, and true */
bool cockpit_host_clicked(int wx, int wy);
/* With a headset's hands (gl_hands.h), as the cockpit is being drawn: a
 * menu over the flight view (the comms menu) is drawn on the virtual
 * screen's plane instead of out in space; its screen rows. */
bool cockpit_menu(int *y0, int *y1);
/* That plane's corners in the play space, and the view's pixel plane at
 * the same distance (what the game sees through each of its points), in the
 * order of cockpit_panel_play */
bool cockpit_screen_plane_play(float corners[4][3]);
bool cockpit_view_plane_play(float corners[4][3]);
/* The view pixel (0..320 x 0..168) the eye sees in the direction of a
 * point in the play space (metres): what a finger there points at */
bool cockpit_play_point_to_pixel(const float p[3], int *sx, int *sy);
/* The height of the control panel's lowest edge in view space (metres) */
float cockpit_panel_bottom(void);
/* A point or direction in a headset's play space -> view space (where the
 * panel and the clipboard are), as the cockpit is being drawn */
void cockpit_play_to_view(const float v[3], float out[3]);
/* And back: view space -> the headset's play space */
void cockpit_view_to_play(const float v[3], float out[3]);

#ifdef __cplusplus
}
#endif

#endif /* GL_COCKPIT_H */
