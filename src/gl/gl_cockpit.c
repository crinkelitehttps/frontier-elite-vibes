/*
 * gl_cockpit.c - see gl_cockpit.h.
 *
 * Spaces:
 *   view space   the game's camera space as gl_scene records it: x right,
 *                y up, looking down -z. The control panel is placed in it
 *                too, in metres, with the eye at the origin.
 *   eye space    view space turned by the head: V = head_view maps view
 *                space to eye space, Q (its transpose) back.
 *   view pixels  the game's 2D drawing over its 3D view: x 0..320, y 0..168
 *                downwards, through the classic projection.
 *   game view    the game's own camera space: x right, y up, z ahead (view
 *                space with z flipped).
 */
#include <math.h>
#include <string.h>

#include <SDL.h>

#include "gl_api.h"
#include "gl_camera.h"
#include "gl_mapscreen.h"
#include "gl_clipboard.h"
#include "gl_cockpit.h"
#include "gl_native.h"
#include "gl_draw.h"
#include "gl_fill2d.h"
#include "gl_hands.h"
#include "gl_screen_place.h"
#include "gl_laser.h"
#include "gl_scene.h"

#include "freecam.h"
#include "galaxy/fe2vm.h"
#include "gamepad.h"
#include "game_state.h"
#include "headtrack.h"
#include "input.h"
#include "host.h" /* rdword, wrword */
#include "m68000.h"
#include "main.h"
#include "renderer.h"
#include "screen_text.h"

bool cockpit_enabled = true;
bool cockpit_screen = true;
int cockpit_fov = 60;
int vr_depth = 100;
bool headtrack_enabled = true;
int headtrack_port = HEADTRACK_DEFAULT_PORT;

#define VIEW_W   FILL2D_VIEW_W
#define VIEW_H   FILL2D_VIEW_H
#define HUD_DIST 1000.0f /* where the view pixel plane sits: inside the world's near/far */

/* The game's world is in millimetres (an escape capsule is 5 m long, an
 * Eagle 48 m): the eye's offset in metres times this, scaled by vr_depth */
#define WORLD_UNITS_PER_METRE 1000.0f

#define WORLD_NEAR   1.0f
#define WORLD_FAR    10000000000.0f
#define COCKPIT_NEAR 0.02f
#define COCKPIT_FAR  50.0f

/* Control panel below the view: 320x32 screen pixels at the ST's 1.2
 * pixel aspect, facing the eye, centred PANEL_DOWN degrees below ahead so
 * it starts just under the bottom of the classic view (18.25). */
#define PANEL_W     0.76f
#define PANEL_H     (PANEL_W * 32.0f / 320.0f * 1.2f)
#define PANEL_DIST  0.62f
#define PANEL_DOWN  24.5f
#define PANEL_ROW0  COCKPIT_PANEL_ROW0 /* first screen row of the panel */

/* The cockpit's screens: the game's 320x168 view at the same pixel aspect,
 * facing the eye just above the panel, as far away; the MAP group's
 * ahead, the COMS group's on its left, the SHIP group's on the right
 * beyond the camera screen, turned to face the eye. Inactive ones are
 * drawn this bright. */
#define SUB_W    0.30f
#define SUB_H    (SUB_W * 168.0f / 320.0f * 1.2f)
/* All the cockpit screens show only the view rows (0..167); the SHIP screen's
 * dashboard is redundant (touch toggles the mode), the MAP screen's buttons
 * are put on its view (gl_mapscreen.h). */
static bool full_page(int g)
{
	(void)g;
	return false;
}
#define SUB_DOWN 10.8f
#define SUB_DIM  0.65f

/* The camera screen (gl_camera.h): beside it, on its right, smaller */
#define CAM_W   0.20f
#define CAM_H   (CAM_W * (float)CAMERA_H / (float)CAMERA_W * 1.2f)
#define CAM_GAP 0.02f

static bool active;      /* the frame being drawn is the cockpit */
static bool sub_on;      /* ... with the cockpit's screens */
static int live = -1;    /* ... the live one's group (COCKPIT_SCREEN_*) */
static bool side_beside; /* ... and the MAP and SHIP screens are live beside it (the station's) */
static bool side_on[COCKPIT_SCREENS]; /* ... which of them are shown (they run on the side) */
static GLuint sub_tex[COCKPIT_SCREENS]; /* their pictures, 0: none yet */
static bool cam_on;      /* ... with the camera screen */
static GLuint cam_tex;
static bool flat_saved;  /* active, while cockpit_draw_flat */
static bool game_active; /* the game's frames are (game thread) */
static float aspect = 16.0f / 9.0f;
static HeadPose pose;
static float look_yaw, look_pitch; /* the controller's look-around, radians */
static mat4 head_view; /* view space -> eye space (rotation only) */
static float eye[3];   /* eye position in view space, metres */

/* A VR headset: this frame's centre view, the last eyes it was drawn for
 * (the passes cover them), the eye being drawn (NULL: the window) */
static bool headset, headset_eyes;
static VrView hs_centre, hs_eyes[2], cur_eye_view;
static const VrView *cur_eye;
static mat4 win_head_view;
static float win_eye[3];

/* The view as last drawn, for the game thread: its passes cover it
 * (choose_passes) and the mouse is mapped through it. The drawing may be
 * on another thread (gl_screen.c), hence the copy and the lock. */
typedef struct
{
	bool active;
	mat4 head_view; /* the window's */
	float eye[3];
	float aspect;
	int n_eyes; /* a headset's, last drawn */
	mat4 eye_view[2];
	float eye_tan[2][4]; /* left, right, down, up */
	bool cam_on;
} SharedView;
static SharedView shared = {.head_view = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}, .aspect = 16.0f / 9.0f};
static SDL_mutex *shared_lock;

static void get_shared(SharedView *out)
{
	if (shared_lock)
		SDL_LockMutex(shared_lock);
	*out = shared;
	if (shared_lock)
		SDL_UnlockMutex(shared_lock);
}

/* Panel corners in view space: top left, top right, bottom left, bottom right */
static float panel[4][3];
/* The cockpit's screens', the same way, by group */
static float subs[COCKPIT_SCREENS][4][3];
/* The camera screen's */
static float cam[4][3];

/* Turned passes: rings of directions, the game's view turned up by `pitch`
 * then right by `yaw0` + i * 360 / n. With the view's edges pulled in by
 * PASS_MARGIN they still cover every direction (checked numerically, with
 * room to spare down to 0.93). */
static const struct
{
	float pitch, yaw0;
	int n;
} RINGS[] = {{0, 0, 6}, {30, 30, 6}, {-30, 30, 6}, {60, 0, 4}, {-60, 0, 4}, {85, 0, 3}, {-85, 0, 3}};
#define PASS_MARGIN 0.97f
/* Directions sampled over the window to see which passes it needs */
#define SAMPLES_X 24
#define SAMPLES_Y 14

/* Each pass's camera axes (right, up, ahead) in game view coordinates */
static float pass_axes[COCKPIT_MAX_PASSES][3][3];
static bool passes_built;

/* This frame's passes in the order the game draws them (ahead last) */
static int pass_order[COCKPIT_MAX_PASSES];
static int n_pass_order, pass_step;
static int drawing_pass = -1; /* the game's, -1 outside the passes */
static bool turned;
static int16_t saved_camera[9];

/* The pass being replayed: its view space -> view space */
static mat4 pass_rot = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};

/* =========================================================================
 * Small vector helpers
 * ========================================================================= */
static float dot3(const float a[3], const float b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void sub3(const float a[3], const float b[3], float out[3])
{
	for (int i = 0; i < 3; i++)
		out[i] = a[i] - b[i];
}

static void cross3(const float a[3], const float b[3], float out[3])
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

/* Q * v: eye space direction -> view space, for the view V */
static void eye_to_view_of(const mat4 *V, const float v[3], float out[3])
{
	const float *m = V->m;
	for (int r = 0; r < 3; r++)
		out[r] = m[r * 4 + 0] * v[0] + m[r * 4 + 1] * v[1] + m[r * 4 + 2] * v[2];
}

/* Half-extents of the classic projection at distance 1 */
static float classic_ky(void)
{
	return tanf(CLASSIC_FOV * GLM_PI / 360.0f);
}
static float classic_kx(void)
{
	return CLASSIC_ASPECT * classic_ky();
}

/* =========================================================================
 * Per frame
 * ========================================================================= */
static void update_tracker(void)
{
	static bool open, failed;
	static int open_port;
	if (open && (!headtrack_enabled || open_port != headtrack_port))
	{
		headtrack_close();
		open = false;
	}
	if (headtrack_enabled && !open && !(failed && open_port == headtrack_port))
	{
		open_port = headtrack_port;
		open = headtrack_open(headtrack_port);
		failed = !open;
	}
	HeadPose zero = {0};
	pose = open ? headtrack_poll() : zero;

	/* the controller's look-around adds on */
	float look_x, look_y;
	gamepad_look(&look_x, &look_y);
	look_yaw = look_x * GLM_PI / 180.0f;
	look_pitch = -look_y * GLM_PI / 180.0f;
}

/* V = Rz(roll) Rx(-pitch) Ry(yaw): the inverse of the head's orientation
 * Ry(-yaw) Rx(pitch) Rz(-roll) (yaw right +, pitch up +, roll right ear
 * down +) */
static mat4 head_rotation(float yaw, float pitch, float roll)
{
	const float deg = 180.0f / GLM_PI;
	mat4 ry = mat4_rotate(yaw * deg, 0, 1, 0);
	mat4 rx = mat4_rotate(-pitch * deg, 1, 0, 0);
	mat4 rz = mat4_rotate(roll * deg, 0, 0, 1);
	mat4 t = mat4_mul(&rx, &ry);
	return mat4_mul(&rz, &t);
}

/* A headset's view (play space, which has view space's axes) after the
 * look-around: V = R^T L, and the eye's position back in view space */
static void headset_view(const VrView *v, mat4 *V, float pos[3])
{
	mat4 look = head_rotation(look_yaw, look_pitch, 0);
	mat4 rt = mat4_identity();
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			rt.m[c * 4 + r] = v->rot[c][r];
	*V = mat4_mul(&rt, &look);
	if (pos)
		eye_to_view_of(&look, v->pos, pos);
}

/* The look-around alone: play space -> view space is L^T (eye_to_view_of) */
static mat4 look_rotation(void)
{
	return head_rotation(look_yaw, look_pitch, 0);
}

void cockpit_set_headset(const VrView *centre, const VrView eyes[2])
{
	headset = centre != NULL;
	if (centre)
		hs_centre = *centre;
	headset_eyes = eyes != NULL;
	if (eyes)
		memcpy(hs_eyes, eyes, sizeof hs_eyes);
}

void cockpit_use_eye(const VrView *v)
{
	if (v && !cur_eye)
	{
		win_head_view = head_view;
		memcpy(win_eye, eye, sizeof eye);
	}
	if (v)
	{
		cur_eye_view = *v;
		cur_eye = &cur_eye_view;
		headset_view(v, &head_view, eye);
	}
	else if (cur_eye)
	{
		cur_eye = NULL;
		head_view = win_head_view;
		memcpy(eye, win_eye, sizeof eye);
	}
}

/* A w x h rectangle PANEL_DIST ahead, centred `down_deg` below ahead and
 * facing the eye, then turned `yaw` (radians, + left) round the eye */
static void place_facing(float down_deg, float yaw, float w, float h, float k, float out[4][3])
{
	float a = down_deg * GLM_PI / 180.0f;
	float c[3] = {0, -PANEL_DIST * k * sinf(a), -PANEL_DIST * k * cosf(a)};
	float down[3] = {0, -cosf(a), sinf(a)}; /* in its plane, towards its bottom edge */
	float cy = cosf(yaw), sy = sinf(yaw);
	for (int i = 0; i < 4; i++)
	{
		float u = (i & 1) ? 0.5f : -0.5f, v = (i & 2) ? 0.5f : -0.5f;
		float x = c[0] + u * w * k, z = c[2] + v * h * k * down[2];
		out[i][0] = x * cy + z * sy;
		out[i][1] = c[1] + v * h * k * down[1];
		out[i][2] = -x * sy + z * cy;
	}
}

/* Group -> its screen in gl_screen_place.h */
static const int GROUP_PLACE[COCKPIT_SCREENS] = {PLACE_SCREEN, PLACE_SHIP, PLACE_COMS};

/* The screens: in their usual places above the panel (scaled with it by
 * `k`), unless moved (gl_screen_place.h) */
static float panel_scale = 1.0f;
static void place_screens(float k)
{
	panel_scale = k;
	/* the angles round the eye (the same at any k) */
	const float r = PANEL_DIST * cosf(SUB_DOWN * GLM_PI / 180.0f);
	const float half = atanf(SUB_W * 0.5f / r), gap = CAM_GAP / r;
	const float cam_right = atanf(((SUB_W + CAM_W) * 0.5f + CAM_GAP + CAM_W * 0.5f) / r);
	place_facing(SUB_DOWN, 0.0f, SUB_W, SUB_H, k, subs[COCKPIT_SCREEN_MAP]);
	place_facing(SUB_DOWN, 2.0f * half + gap, SUB_W, SUB_H, k, subs[COCKPIT_SCREEN_COMS]);
	place_facing(SUB_DOWN, -(cam_right + gap + half), SUB_W, SUB_H, k, subs[COCKPIT_SCREEN_SHIP]);
	place_facing(SUB_DOWN, 0.0f, CAM_W, CAM_H, k, cam);
	for (int i = 0; i < 4; i++)
		cam[i][0] += (SUB_W + CAM_W) * 0.5f * k + CAM_GAP * k;
	for (int g = 0; g < COCKPIT_SCREENS; g++)
		screen_place_corners(GROUP_PLACE[g], subs[g]);
	screen_place_corners(PLACE_CAMERA, cam);
}

/* With a headset's hands (gl_hands.h) the panel is within reach: nearer,
 * scaled about the eye so it looks the same size. The cockpit's screen
 * goes with it. */
static void build_panel(void)
{
	float k = 1.0f;
	if (headset && vr_hands && hands_panel_distance() > 0)
		k = (float)hands_panel_distance() / 100.0f / PANEL_DIST;
	place_facing(PANEL_DOWN, 0.0f, PANEL_W, PANEL_H, k, panel);
	place_screens(k);
}

void cockpit_place_screens(void)
{
	if (active)
		place_screens(panel_scale);
}

/* With a headset's hands: the virtual screen's plane, where a menu over
 * the flight view (the comms menu) is brought, and the view's own pixel
 * plane at the same distance, where the view is touched (targets are out
 * in space, but their direction crosses it). View space, like the panel. */
static float screen_plane[4][3], view_plane[4][3];
static bool menu_on;
static int menu_y0, menu_y1;
/* In flight the comms menu is on the COMS screen (gl_screen_coms_menu), not
 * in the view: its rows are left out of the view */
static volatile bool coms_menu;
static int cut_y0, cut_y1;

static void build_planes(void)
{
	menu_on = false;
	if (!(headset && vr_hands) || sub_on)
		return;
	float dist, w, drop;
	vr_screen_place(&dist, &w, &drop);
	float h = w * 0.75f, kx = classic_kx() * dist, ky = classic_ky() * dist;
	for (int i = 0; i < 4; i++)
	{
		float u = (i & 1) ? 0.5f : -0.5f, v = (i & 2) ? -0.5f : 0.5f;
		screen_plane[i][0] = u * w;
		screen_plane[i][1] = -drop + v * h;
		screen_plane[i][2] = -dist;
		view_plane[i][0] = u * 2.0f * kx;
		view_plane[i][1] = v * 2.0f * ky;
		view_plane[i][2] = -dist;
	}
	menu_on = !coms_menu && gl_screen_menu_rows(&menu_y0, &menu_y1);
}

bool cockpit_game_frame(void)
{
	game_active = cockpit_enabled && use_renderer != R_OLD && game_screen() == SCREEN_FLIGHT &&
				  game_flight_view() == VIEW_FRONT && !freecam_active();
	return game_active;
}

bool cockpit_screens_on(void)
{
	return cockpit_enabled && cockpit_screen && use_renderer != R_OLD && native_mode() == 2 && !freecam_active();
}

int cockpit_screen_group(void)
{
	if (!cockpit_screens_on())
		return -1;
	switch (game_screen())
	{
	case SCREEN_GALAXY_MAP:
	case SCREEN_SYSTEM_MAP:
		return COCKPIT_SCREEN_MAP;
	case SCREEN_SHIP:
		return COCKPIT_SCREEN_SHIP;
	case SCREEN_OPTIONS:
	case SCREEN_STATION:
		return COCKPIT_SCREEN_COMS;
	default:
		return -1;
	}
}

bool cockpit_screen_frame(void)
{
	return cockpit_screen_group() >= 0;
}

/* The flight view: the camera set up from the player's ship, then the
 * stars and the objects drawn (Lcockpit_world, so the passes) */
#ifdef FE2_SetupPlayerCamera
#define FLIGHT_VIEW FE2_SetupPlayerCamera
#else
#define FLIGHT_VIEW 0x57060 /* L62966 in fe2.s, L62966_SetupPlayerCamera in fe2_modded.s */
#endif

/* Mouse steering (L62678) turns the ship by how far the mouse control moved
 * since the last frame (A6+682); under a screen the mouse is the screen's
 * (a right-button drag turns the maps), so no motion is left for it */
#define MOUSE_CTRL      14488 /* A6_mouse_motion_ctrl_x, _y */
#define MOUSE_CTRL_LAST 682

void Call_ScreenControls(void)
{
	bool on = cockpit_screen_frame();
	if (on)
	{
		uint32_t a6 = (uint32_t)GetReg(REG_A6);
		wrlong(a6 + MOUSE_CTRL_LAST, rdlong(a6 + MOUSE_CTRL));
	}
	SetReg(REG_D0, on);
}

void Call_ScreenWorld(void)
{
	SetReg(REG_D0, cockpit_screen_frame());
}

bool cockpit_capture_world(void)
{
	if (!game_player_ship())
		return false;
	static bool failed;
	fe2vm_game_hcalls(1);
	fe2vm_begin();
	wrbyte(GAME_A6 + A6_3DVIEW_MODE, VIEW_FRONT);
	bool was = game_active;
	game_active = true; /* the passes, as for a cockpit frame */
	fe2vm_regs r;
	memset(&r, 0, sizeof r);
	int err = fe2vm_call(FE2_Clear3DView, &r);
	if (!err)
	{
		memset(&r, 0, sizeof r);
		err = fe2vm_call(FLIGHT_VIEW, &r);
	}
	game_active = was;
	fe2vm_end();
	fe2vm_game_hcalls(0);
	/* the flight view's own text (labels on things): the caller's to take */
	if (err && !failed)
		log_printf("Cockpit screen: no world under it (%s)\n", fe2vm_error());
	failed = err != 0;
	return !err;
}

static void publish_view(void)
{
	SharedView v;
	v.active = active;
	v.head_view = head_view;
	memcpy(v.eye, eye, sizeof v.eye);
	v.aspect = aspect;
	v.cam_on = cam_on;
	v.n_eyes = headset_eyes ? 2 : 0;
	for (int i = 0; i < v.n_eyes; i++)
	{
		headset_view(&hs_eyes[i], &v.eye_view[i], NULL);
		v.eye_tan[i][0] = hs_eyes[i].tan_left;
		v.eye_tan[i][1] = hs_eyes[i].tan_right;
		v.eye_tan[i][2] = hs_eyes[i].tan_down;
		v.eye_tan[i][3] = hs_eyes[i].tan_up;
	}
	if (shared_lock)
		SDL_LockMutex(shared_lock);
	shared = v;
	if (shared_lock)
		SDL_UnlockMutex(shared_lock);
}

bool cockpit_shown(void)
{
	SharedView v;
	get_shared(&v);
	return v.active;
}

bool cockpit_begin_frame(int window_w, int window_h, bool cockpit, int screen)
{
	update_tracker();

	active = cockpit || screen >= 0;
	sub_on = !cockpit && screen >= 0 && screen < COCKPIT_SCREENS;
	live = sub_on ? screen : -1;
	for (int g = 0; g < COCKPIT_SCREENS; g++)
		sub_tex[g] = sub_on ? gl_screen_sub_texture(g) : 0;
	/* the MAP and SHIP screens are up in flight too (gl_mapscreen.h) */
	memset(side_on, 0, sizeof side_on);
	if (cockpit && cockpit_screens_on())
		for (int g = COCKPIT_SCREEN_MAP; g <= COCKPIT_SCREEN_SHIP; g++)
			if (mapscreen_group_up(g) && (sub_tex[g] = gl_screen_sub_texture(g)))
				side_on[g] = true;
	if (side_on[COCKPIT_SCREEN_MAP])
		live = COCKPIT_SCREEN_MAP;
	/* ... and they go on beside the station's services */
	side_beside = sub_on && live == COCKPIT_SCREEN_COMS && mapscreen_up();
	if (side_beside)
		for (int g = COCKPIT_SCREEN_MAP; g <= COCKPIT_SCREEN_SHIP; g++)
			side_on[g] = mapscreen_group_up(g) && sub_tex[g];
	/* in flight, the COMS screen shows the comms menu's text */
	if (active && !sub_on && gl_screen_coms_menu())
		sub_tex[COCKPIT_SCREEN_COMS] = gl_screen_coms_menu();
	/* the menu's rows stay out of the view a while after the game stops
	 * drawing its boxes: its text can outlast them (the screen keeps it) */
	static int coms_hold;
	if (active && !sub_on && sub_tex[COCKPIT_SCREEN_COMS] && gl_screen_menu_rows(&cut_y0, &cut_y1))
		coms_hold = 60;
	else if (coms_hold > 0)
		coms_hold--;
	coms_menu = active && !sub_on && sub_tex[COCKPIT_SCREEN_COMS] && coms_hold > 0;
	cam_tex = active && cockpit_camera ? camera_texture() : 0;
	cam_on = cam_tex != 0;
	if (window_w > 0 && window_h > 0)
		aspect = (float)window_w / (float)window_h;

	cockpit_use_eye(NULL);
	if (headset)
	{
		headset_view(&hs_centre, &head_view, eye);
	}
	else
	{
		head_view = head_rotation(pose.yaw + look_yaw, pose.pitch + look_pitch, pose.roll);
		eye[0] = pose.x / 100.0f;
		eye[1] = pose.y / 100.0f;
		eye[2] = pose.z / 100.0f;
	}

	build_panel();
	build_planes();
	publish_view();
	return active;
}

bool cockpit_active(void)
{
	return active;
}

void cockpit_draw_flat(bool flat)
{
	if (flat)
	{
		flat_saved = active;
		active = false;
	}
	else
		active = flat_saved;
}

/* =========================================================================
 * Projections
 * ========================================================================= */
/* An off-centre lens for a headset's eye */
static mat4 eye_lens(const VrView *v, float n, float f)
{
	mat4 m;
	memset(&m, 0, sizeof m);
	float l = v->tan_left, r = v->tan_right, d = v->tan_down, u = v->tan_up;
	m.m[0] = 2.0f / (r - l);
	m.m[5] = 2.0f / (u - d);
	m.m[8] = (r + l) / (r - l);
	m.m[9] = (u + d) / (u - d);
	m.m[10] = (f + n) / (n - f);
	m.m[11] = -1.0f;
	m.m[14] = 2.0f * f * n / (n - f);
	return m;
}

static mat4 lens(float n, float f)
{
	if (cur_eye)
		return eye_lens(cur_eye, n, f);
	return mat4_perspective((float)cockpit_fov, aspect, n, f);
}

static mat4 world_lens(void)
{
	return lens(WORLD_NEAR, WORLD_FAR);
}

/* The world without the eye's offset: for what sits at infinity (the
 * game's 2D drawing over its view) */
static mat4 world_rotation_projection(void)
{
	mat4 lens = world_lens();
	mat4 view = mat4_mul(&head_view, &pass_rot);
	return mat4_mul(&lens, &view);
}

/* The world seen from the eye: its offset from the centre of the head (and
 * the head's movement) gives the stereo depth and the parallax */
mat4 cockpit_world_projection(void)
{
	float k = WORLD_UNITS_PER_METRE * vr_depth / 100.0f;
	mat4 lens = world_lens();
	mat4 t = mat4_translate(-eye[0] * k, -eye[1] * k, -eye[2] * k);
	mat4 tr = mat4_mul(&t, &pass_rot);
	mat4 view = mat4_mul(&head_view, &tr);
	return mat4_mul(&lens, &view);
}

void cockpit_world_split(mat4 *lens, mat4 *head_rotation)
{
	*lens = world_lens();
	*head_rotation = mat4_mul(&head_view, &pass_rot);
}

mat4 cockpit_head_rotation(void)
{
	return head_view;
}

/* Cockpit geometry: lens with a near plane for things a metre away, head
 * rotation, then the eye's position */
static mat4 cockpit_projection(void)
{
	mat4 l = lens(COCKPIT_NEAR, COCKPIT_FAR);
	mat4 t = mat4_translate(-eye[0], -eye[1], -eye[2]);
	mat4 vt = mat4_mul(&head_view, &t);
	return mat4_mul(&l, &vt);
}

mat4 cockpit_play_projection(float eye_play[3])
{
	mat4 look = look_rotation();
	mat4 to_view = mat4_identity();
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			to_view.m[c * 4 + r] = look.m[r * 4 + c];
	mat4_xform_dir(&look, eye, eye_play);
	mat4 cp = cockpit_projection();
	return mat4_mul(&cp, &to_view);
}

mat4 cockpit_headset_projection(const VrView *v)
{
	mat4 l = eye_lens(v, COCKPIT_NEAR, COCKPIT_FAR);
	mat4 rt = mat4_identity();
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			rt.m[c * 4 + r] = v->rot[c][r];
	mat4 t = mat4_translate(-v->pos[0], -v->pos[1], -v->pos[2]);
	mat4 vt = mat4_mul(&rt, &t);
	return mat4_mul(&l, &vt);
}

mat4 cockpit_direction_projection(void)
{
	if (!active)
		return mat4_perspective(CLASSIC_FOV, CLASSIC_ASPECT, WORLD_NEAR, WORLD_FAR);
	return world_rotation_projection();
}

mat4 cockpit_view_pixel_projection(void)
{
	if (!active)
		return mat4_ortho(0, VIEW_W, VIEW_H, 0, -1, 1);

	/* view pixel -> the point on the plane z = -HUD_DIST that the classic
	 * projection shows at that pixel */
	float kx = classic_kx(), ky = classic_ky();
	mat4 l = mat4_identity();
	l.m[0] = HUD_DIST * 2.0f * kx / VIEW_W;
	l.m[5] = -HUD_DIST * 2.0f * ky / VIEW_H;
	l.m[12] = -HUD_DIST * kx;
	l.m[13] = HUD_DIST * ky;
	l.m[14] = -HUD_DIST;
	mat4 w = world_rotation_projection();
	return mat4_mul(&w, &l);
}

/* =========================================================================
 * Drawing
 * ========================================================================= */
static GLuint prog, vao, vbo;
static GLint u_tex, u_discard, u_tint;

static const char *VS_QUAD = "in vec4 aPos;\n"
							 "in vec2 aUV;\n"
							 "out vec2 vUV;\n"
							 "void main(){ gl_Position = aPos; vUV = aUV; }\n";

static const char *FS_QUAD = "in vec2 vUV;\n"
							 "uniform sampler2D uTex;\n"
							 "uniform int uDiscard;\n"
							 "uniform float uTint;\n"
							 "out vec4 fragColor;\n"
							 "void main(){\n"
							 "  vec4 c = texture(uTex, vUV);\n"
							 "  if (uDiscard != 0 && c.a < 0.5) discard;\n"
							 "  fragColor = vec4(c.rgb * uTint, 1.0);\n"
							 "}\n";

void cockpit_init(void)
{
	if (!shared_lock)
		shared_lock = SDL_CreateMutex();
	camera_init();
	build_panel();
	static const char *const attribs[] = {"aPos", "aUV"};
	prog = gd_build_program(VS_QUAD, FS_QUAD, attribs, 2);
	u_tex = glGetUniformLocation(prog, "uTex");
	u_discard = glGetUniformLocation(prog, "uDiscard");
	u_tint = glGetUniformLocation(prog, "uTint");

	glGenVertexArrays(1, &vao);
	glGenBuffers(1, &vbo);
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)(4 * sizeof(float)));
	glBindVertexArray(0);
}

void cockpit_shutdown(void)
{
	camera_shutdown();
	glDeleteBuffers(1, &vbo);
	glDeleteVertexArrays(1, &vao);
	glDeleteProgram(prog);
}

/* A part of the emulated screen drawn on a quad: clip space corners (top
 * left, top right, bottom left, bottom right) and their texture coords */
typedef struct
{
	float v[4][6];
	int discard; /* colour 255 (transparent) shows what is behind */
	int upload;  /* first use this frame: upload the screen */
	GLuint tex;  /* 0: the screen's */
	float tint;  /* brightness */
} TexQuad;

static void tex_quad_pass(const void *data)
{
	const TexQuad *q = data;
	if (q->upload)
		gl_screen_upload();
	glUseProgram(prog);
	glUniform1i(u_tex, 0);
	glUniform1i(u_discard, q->discard);
	glUniform1f(u_tint, q->tint);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, q->tex ? q->tex : gl_screen_texture());
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof q->v, q->v, GL_STREAM_DRAW);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_2D, 0);
}

/* Queues screen rows y0..y1 on the quad `corners` (through `m`), from
 * `tex` laid out as the screen's texture (0: the screen's) */
static void queue_layer_quad(const mat4 *m, const float corners[4][3], GLuint tex, int y0, int y1, bool discard,
							 bool upload)
{
	TexQuad *q = gd_custom(tex_quad_pass, sizeof *q);
	if (!q)
		return;
	const float u1 = (float)GL_SCREEN_W / GL_SCREEN_TEX_W;
	for (int i = 0; i < 4; i++)
	{
		mat4_xform(m, corners[i][0], corners[i][1], corners[i][2], q->v[i]);
		q->v[i][4] = (i & 1) ? u1 : 0.0f;
		q->v[i][5] = (float)((i & 2) ? y1 : y0) / GL_SCREEN_TEX_H;
	}
	q->discard = discard;
	q->upload = upload;
	q->tex = tex;
	q->tint = 1.0f;
}

static void queue_screen_quad(const mat4 *m, const float corners[4][3], int y0, int y1, bool discard, bool upload)
{
	queue_layer_quad(m, corners, 0, y0, y1, discard, upload);
}

/* Queues the whole of texture `tex` on the quad `corners`, its top edge
 * at texture v `v_top` and its bottom at `v_bottom`, `tint` bright */
static void queue_tex_quad(const mat4 *m, const float corners[4][3], GLuint tex, float v_top, float v_bottom,
						   float tint, bool discard)
{
	TexQuad *q = gd_custom(tex_quad_pass, sizeof *q);
	if (!q)
		return;
	for (int i = 0; i < 4; i++)
	{
		mat4_xform(m, corners[i][0], corners[i][1], corners[i][2], q->v[i]);
		q->v[i][4] = (i & 1) ? 1.0f : 0.0f;
		q->v[i][5] = (i & 2) ? v_bottom : v_top;
	}
	q->discard = discard;
	q->upload = false;
	q->tex = tex;
	q->tint = tint;
}

void cockpit_draw(void)
{
	if (!active)
		return;

	/* the game's 2D layer over its 3D view, on its classic directions; a
	 * menu in it (with hands) is left out here and put on the screen's
	 * plane. With the cockpit's screen it is on that screen instead. */
	mat4 hud = cockpit_view_pixel_projection();
	gd_set_viewport(GD_VP_WINDOW);
	bool upload = true;
	const bool cut = menu_on || coms_menu;
	const int c0 = menu_on ? menu_y0 : cut_y0, c1 = menu_on ? menu_y1 : cut_y1;
	const int bands[2][2] = {{0, cut ? c0 : PANEL_ROW0}, {cut ? c1 : PANEL_ROW0, PANEL_ROW0}};
	for (int i = 0; i < 2 && !sub_on; i++)
	{
		int a = bands[i][0], b = bands[i][1];
		if (b <= a)
			continue;
		const float rows[4][3] = {{0, a, 0}, {VIEW_W, a, 0}, {0, b, 0}, {VIEW_W, b, 0}};
		queue_screen_quad(&hud, rows, a, b, true, upload);
		upload = false;
	}
	/* under the cockpit's screen the world's labels, as the 2D layer had them */
	GLuint labels = sub_on ? gl_screen_labels_texture() : 0;
	if (labels)
	{
		const float rows[4][3] = {{0, 0, 0}, {VIEW_W, 0, 0}, {0, PANEL_ROW0, 0}, {VIEW_W, PANEL_ROW0, 0}};
		queue_layer_quad(&hud, rows, labels, 0, PANEL_ROW0, true, false);
	}

	/* the control panel (with the scanner), on its own in front of the eye */
	mat4 cp = cockpit_projection();
	gd_set_projection(&cp);
	gd_push();
	gd_identity();
	gd_set_cull(false);
	queue_screen_quad(&cp, (const float(*)[3])panel, PANEL_ROW0, GL_SCREEN_H, false, upload);
	if (menu_on)
	{
		/* rows menu_y0 .. menu_y1 of the screen at their place on its plane */
		float c[4][3];
		for (int i = 0; i < 4; i++)
		{
			float t = (float)((i & 2) ? menu_y1 : menu_y0) / (float)GL_SCREEN_H;
			for (int k = 0; k < 3; k++)
				c[i][k] = screen_plane[i & 1][k] + (screen_plane[2 + (i & 1)][k] - screen_plane[i & 1][k]) * t;
		}
		queue_screen_quad(&cp, c, menu_y0, menu_y1, true, false);
	}
	for (int g = 0; g < COCKPIT_SCREENS; g++) /* their textures are bottom up */
		if (sub_tex[g])
			queue_tex_quad(&cp, (const float(*)[3])subs[g], sub_tex[g], 1.0f, full_page(g) ? 0.0f : GL_SCREEN_SUB_V0,
						   g == live || side_on[g] ? 1.0f : SUB_DIM, g == COCKPIT_SCREEN_COMS && gl_screen_coms_clear());
	if (cam_on)
		queue_tex_quad(&cp, (const float(*)[3])cam, cam_tex, 0.0f, 1.0f, 1.0f, false);
	gd_pop();

	/* the clipboard (gl_clipboard.h), nearer than the panel */
	clipboard_queue_cockpit(&cp, eye);

	/* the controllers' hands or lasers (gl_laser.c), in front of it all */
	float eye_play[3];
	mat4 pp = cockpit_play_projection(eye_play);
	laser_queue(&pp, eye_play);
}

/* =========================================================================
 * Turned passes
 * ========================================================================= */
static void build_passes(void)
{
	int k = 0;
	for (size_t r = 0; r < sizeof RINGS / sizeof RINGS[0]; r++)
		for (int i = 0; i < RINGS[r].n && k < COCKPIT_MAX_PASSES; i++, k++)
		{
			float yaw = (RINGS[r].yaw0 + 360.0f * (float)i / (float)RINGS[r].n) * GLM_PI / 180.0f;
			float pitch = RINGS[r].pitch * GLM_PI / 180.0f;
			float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
			float axes[3][3] = {{cy, 0, -sy}, {-sy * sp, cp, -cy * sp}, {sy * cp, sp, cy * cp}};
			memcpy(pass_axes[k], axes, sizeof axes);
		}
	passes_built = true;
}

/* Is the game view direction g inside pass k's view (edges pulled in)? */
static bool pass_contains(int k, const float g[3])
{
	const float(*a)[3] = pass_axes[k];
	float z = dot3(a[2], g);
	if (z <= 1e-6f)
		return false;
	return fabsf(dot3(a[0], g)) <= classic_kx() * PASS_MARGIN * z &&
		   fabsf(dot3(a[1], g)) <= classic_ky() * PASS_MARGIN * z;
}

/* Marks the passes a view (V, frustum edges at distance 1) shows */
static void sample_view(const mat4 *V, float l, float r, float d, float u, bool want[COCKPIT_MAX_PASSES])
{
	for (int iy = 0; iy < SAMPLES_Y; iy++)
		for (int ix = 0; ix < SAMPLES_X; ix++)
		{
			float fx = (float)ix / (SAMPLES_X - 1), fy = (float)iy / (SAMPLES_Y - 1);
			float e[3] = {l + (r - l) * fx, d + (u - d) * fy, -1.0f}, v[3];
			eye_to_view_of(V, e, v);
			float g[3] = {v[0], v[1], -v[2]};
			for (int k = 0; k < COCKPIT_MAX_PASSES; k++)
				if (pass_contains(k, g))
				{
					want[k] = true;
					break;
				}
		}
}

/* The native scene (gl_native.h) draws the world for any view from what
 * the game's own view captures, so the game draws just that, and a
 * capture pass towards each planet it needs to see (native_capture_dirs).
 * None of what the game draws is shown. */
static void choose_capture_passes(void)
{
	float dirs[4][3];
	int n = native_capture_dirs(dirs, 4);
	n_pass_order = 0;
	for (int i = 0; i < n; i++)
	{
		int best = 0;
		float best_dot = -2.0f;
		for (int k = 0; k < COCKPIT_MAX_PASSES; k++)
		{
			float d = dot3(pass_axes[k][2], dirs[i]);
			if (pass_contains(k, dirs[i]))
				d += 2.0f; /* one that has it, the nearest of those */
			if (d > best_dot)
				best = k, best_dot = d;
		}
		bool have = best == 0;
		for (int j = 0; j < n_pass_order && !have; j++)
			have = pass_order[j] == best;
		if (!have)
			pass_order[n_pass_order++] = best;
	}
	pass_order[n_pass_order++] = 0;
}

/* The passes the window (and a headset's eyes) show this frame, by
 * sampling them a little beyond their edges: each sample goes to the first
 * pass that has it, as the stencil does when they are drawn. Ahead is
 * always drawn, last. The eyes are last frame's, so with more room for
 * the head turning since. */
static void choose_passes(void)
{
	if (!passes_built)
		build_passes();
	if (native_mode() == 2)
	{
		choose_capture_passes();
		return;
	}
	SharedView sv;
	get_shared(&sv);
	bool want[COCKPIT_MAX_PASSES] = {false};
	float t = tanf((float)cockpit_fov * GLM_PI / 360.0f) * 1.1f;
	sample_view(&sv.head_view, -sv.aspect * t, sv.aspect * t, -t, t, want);
	for (int i = 0; i < sv.n_eyes; i++)
	{
		const float m = 1.25f;
		const float *e = sv.eye_tan[i];
		sample_view(&sv.eye_view[i], e[0] * m, e[1] * m, e[2] * m, e[3] * m, want);
	}
	n_pass_order = 0;
	for (int k = 1; k < COCKPIT_MAX_PASSES; k++)
		if (want[k])
			pass_order[n_pass_order++] = k;
	pass_order[n_pass_order++] = 0;
}

/* The camera object's matrix (3x3 words, 1.0 = 32767, row major, columns
 * right, up, ahead in the world) turned to pass k: M * axes */
static void turn_camera(uint32_t cam, int k)
{
	const float(*a)[3] = pass_axes[k];
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
		{
			float v = 0;
			for (int i = 0; i < 3; i++)
				v += (float)saved_camera[r * 3 + i] * a[c][i];
			wrword(cam + 2 * (r * 3 + c), (int)fmaxf(-32767.0f, fminf(32767.0f, roundf(v))));
		}
}

/* Empties the game's own display list (as L385ea_Clear3DView does, and
 * nothing else of what it does) so the next pass has all of it. The GL
 * scene has taken what it needs from it. */
static void reset_game_list(void)
{
	uint32_t base = (uint32_t)rdlong(FE2_primitives_base);
	for (int i = 0; i < 12; i += 4)
		wrlong(base + i, 0);
	wrword(base + 12, -8);
	wrlong(FE2_3dview_thing2, base);
	wrlong(FE2_primitives_end, base + 14);
}

void Call_CockpitPass(void)
{
	uint32_t cam = (uint32_t)GetReg(REG_A3);
	if (GetReg(REG_D0) == 0)
	{
		pass_step = 0;
		n_pass_order = 0;
		camera_note_shots();
		if (game_active && scene_active())
			choose_passes();
		/* the game draws some things differently for the passes
		 * (Lcockpit_passes in fe2.s) */
		wrword(FE2_Lcockpit_passes, n_pass_order > 0);
		for (int i = 0; i < 9; i++)
			saved_camera[i] = rdword(cam + 2 * i);
	}
	else if (n_pass_order > 0)
	{
		scene_end_pass();
		scene_capture_soft_nodes();
		if (pass_step < n_pass_order)
			reset_game_list();
	}

	drawing_pass = -1;
	/* not in the cockpit: the one pass the game always drew */
	if (n_pass_order == 0)
	{
		SetReg(REG_D0, pass_step++ == 0);
		return;
	}

	while (pass_step < n_pass_order)
	{
		int k = pass_order[pass_step++];
		if (!scene_begin_pass(k) && k != 0)
			continue;
		turn_camera(cam, k);
		wrbyte(cam + 92, 0); /* its frame-turned copy (MatrixMulWTF) is stale */
		turned = k != 0;
		drawing_pass = k;
		SetReg(REG_D0, 1);
		return;
	}
	for (int i = 0; i < 9; i++)
		wrword(cam + 2 * i, saved_camera[i]);
	wrbyte(cam + 92, 0);
	turned = false;
	wrword(FE2_Lcockpit_passes, 0);
	SetReg(REG_D0, 0);
}

bool cockpit_turned_pass(void)
{
	return turned;
}

void Call_CockpitBackground(void)
{
	/* the atmosphere bands go just in front of the stars (gl_atmos.c) */
	if (drawing_pass >= 0)
		scene_insert_after_background(PRIM_ATMOS);
}

/* The sky model's place in the pass (Lcockpit_sky): 512 ahead of the
 * game's own camera, in the turned camera's axes. */
#define SKY_AHEAD 512.0f
void Call_CockpitSky(void)
{
	if (drawing_pass < 0)
		return;
	uint32_t obj = (uint32_t)GetReg(REG_A6);
	for (int c = 0; c < 3; c++)
		wrlong(obj + 20 + 4 * c, (int)lroundf(SKY_AHEAD * pass_axes[drawing_pass][c][2]));
}

/* A vertex of type $d / $e in a turned pass (Lcockpit_vertex_d): between
 * vertices a0 and a1 (their camera coordinates at +4: x, y, z ahead), x and
 * y averaged and the nearer z, worked out in the game's own camera axes as
 * the straight ahead pass has it, and returned in the pass's (d2-d4). Pass
 * k's axis c is pass_axes[k][c] in the game's camera. */
void Call_CockpitVertexD(void)
{
	if (drawing_pass <= 0)
		return;
	const float(*a)[3] = pass_axes[drawing_pass];
	const uint32_t v[2] = {(uint32_t)GetReg(REG_A0), (uint32_t)GetReg(REG_A1)};
	double cam[2][3];
	for (int i = 0; i < 2; i++)
	{
		double p[3];
		for (int c = 0; c < 3; c++)
			p[c] = (double)rdlong(v[i] + 4 + 4 * c);
		for (int k = 0; k < 3; k++)
			cam[i][k] = p[0] * a[0][k] + p[1] * a[1][k] + p[2] * a[2][k];
	}
	const double q[3] = {(cam[0][0] + cam[1][0]) * 0.5, (cam[0][1] + cam[1][1]) * 0.5, fmin(cam[0][2], cam[1][2])};
	static const int reg[3] = {REG_D2, REG_D3, REG_D4};
	for (int c = 0; c < 3; c++)
		SetReg(reg[c], (int)lround(a[c][0] * q[0] + a[c][1] * q[1] + a[c][2] * q[2]));
}

static mat4 pass_matrix(int id);

void cockpit_pass_to_view(const float v[3], float out[3])
{
	if (drawing_pass < 0)
	{
		memcpy(out, v, 3 * sizeof(float));
		return;
	}
	mat4 m = pass_matrix(drawing_pass);
	mat4_xform_dir(&m, v, out);
}

typedef struct
{
	int ref;
	int box[4]; /* scissor: x, y, w, h (stencil_mark) */
} StencilCmd;

/* Marks where the pass will draw: the part of its view no earlier pass
 * has. Earlier passes have larger refs, the screen starts at 0. The
 * scissor keeps the pass to the box round its view: its full-screen
 * shader quads (atmosphere bands) are not shaded everywhere again. */
static void stencil_mark(const void *data)
{
	const StencilCmd *c = data;
	glEnable(GL_SCISSOR_TEST);
	glScissor(c->box[0], c->box[1], c->box[2], c->box[3]);
	glEnable(GL_STENCIL_TEST);
	glStencilMask(0xff);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	glStencilFunc(GL_GREATER, c->ref, 0xff);
	glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
}

static void stencil_test(const void *data)
{
	const StencilCmd *c = data;
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glStencilFunc(GL_EQUAL, c->ref, 0xff);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
}

static void stencil_off(const void *data)
{
	(void)data;
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

static mat4 pass_matrix(int id)
{
	/* view space is game view with z flipped: S * axes * S */
	static const float s[3] = {1, 1, -1};
	mat4 m = mat4_identity();
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			m.m[c * 4 + r] = s[r] * s[c] * pass_axes[id][c][r];
	return m;
}

/* The window pixels (x, y, w, h) the quad `pts` (clip space, a convex
 * polygon) covers in viewport `vp`: clipped to w > 0 first, as the GPU
 * does, since a pass's view can reach behind the eye */
static void clip_box(const float pts[4][4], const int vp[4], int box[4])
{
	const float eps = 1e-6f;
	float lo[2] = {1.0f, 1.0f}, hi[2] = {-1.0f, -1.0f};
	for (int i = 0; i < 4; i++)
	{
		const float *a = pts[i], *b = pts[(i + 1) % 4];
		float out[2][2];
		int n = 0;
		if (a[3] > eps)
		{
			out[n][0] = a[0] / a[3];
			out[n++][1] = a[1] / a[3];
		}
		if ((a[3] > eps) != (b[3] > eps)) /* the edge crosses w = eps */
		{
			float t = (eps - a[3]) / (b[3] - a[3]);
			float x = a[0] + (b[0] - a[0]) * t, y = a[1] + (b[1] - a[1]) * t;
			out[n][0] = x / eps;
			out[n++][1] = y / eps;
		}
		for (int k = 0; k < n; k++)
			for (int j = 0; j < 2; j++)
			{
				lo[j] = fminf(lo[j], out[k][j]);
				hi[j] = fmaxf(hi[j], out[k][j]);
			}
	}
	for (int j = 0; j < 2; j++)
	{
		lo[j] = fmaxf(lo[j], -1.0f);
		hi[j] = fminf(hi[j], 1.0f);
	}
	if (lo[0] >= hi[0] || lo[1] >= hi[1])
	{
		box[0] = vp[0];
		box[1] = vp[1];
		box[2] = box[3] = 0;
		return;
	}
	int x0 = (int)floorf((lo[0] + 1.0f) * 0.5f * (float)vp[2]), x1 = (int)ceilf((hi[0] + 1.0f) * 0.5f * (float)vp[2]);
	int y0 = (int)floorf((lo[1] + 1.0f) * 0.5f * (float)vp[3]), y1 = (int)ceilf((hi[1] + 1.0f) * 0.5f * (float)vp[3]);
	box[0] = vp[0] + x0;
	box[1] = vp[1] + y0;
	box[2] = x1 - x0;
	box[3] = y1 - y0;
}

void cockpit_pass_draw_begin(int id)
{
	if (id < 0 || id >= COCKPIT_MAX_PASSES || !passes_built)
		return;
	pass_rot = pass_matrix(id);
	gd_set_viewport(GD_VP_VIEW3D);

	mat4 vp = cockpit_view_pixel_projection();
	float mx = VIEW_W * 0.5f * (1.0f - PASS_MARGIN), my = VIEW_H * 0.5f * (1.0f - PASS_MARGIN);
	StencilCmd *c = gd_custom(stencil_mark, sizeof *c);
	if (c)
	{
		c->ref = 255 - id;
		const float corners[4][2] = {{mx, my}, {VIEW_W - mx, my}, {VIEW_W - mx, VIEW_H - my}, {mx, VIEW_H - my}};
		float clip[4][4];
		for (int i = 0; i < 4; i++)
			mat4_xform(&vp, corners[i][0], corners[i][1], 0.0f, clip[i]);
		clip_box((const float(*)[4])clip, gd_viewport_rect(GD_VP_VIEW3D), c->box);
	}
	bool wire = gd_wireframe();
	gd_set_wireframe(false);
	gd_set_cull(false);
	gd_set_projection(&vp);
	gd_push();
	gd_identity();
	gd_rect2(mx, my, VIEW_W - mx, VIEW_H - my);
	gd_pop();
	gd_set_wireframe(wire);
	c = gd_custom(stencil_test, sizeof *c);
	if (c)
		c->ref = 255 - id;

	mat4 w = cockpit_world_projection();
	gd_set_projection(&w);
}

void cockpit_pass_draw_end(void)
{
	StencilCmd *c = gd_custom(stencil_off, sizeof *c);
	(void)c;
	pass_rot = mat4_identity();
	mat4 w = cockpit_world_projection();
	gd_set_projection(&w);
}

/* =========================================================================
 * Mouse
 * ========================================================================= */
/* Where the ray from `eye` along d meets the quad `q` (corners as the
 * panel's): u, v 0..1 across it, *t along d */
static bool ray_hits_quad(const float q[4][3], const float eye[3], const float d[3], float *u, float *v, float *t)
{
	float u_axis[3], v_axis[3], n[3], to[3];
	sub3(q[1], q[0], u_axis);
	sub3(q[2], q[0], v_axis);
	cross3(u_axis, v_axis, n);
	float dn = dot3(d, n);
	if (fabsf(dn) <= 1e-9f)
		return false;
	sub3(q[0], eye, to);
	*t = dot3(to, n) / dn;
	if (*t <= 0.0f)
		return false;
	float p[3], rel[3];
	for (int i = 0; i < 3; i++)
		p[i] = eye[i] + d[i] * *t;
	sub3(p, q[0], rel);
	*u = dot3(rel, u_axis) / dot3(u_axis, u_axis);
	*v = dot3(rel, v_axis) / dot3(v_axis, v_axis);
	return *u >= 0.0f && *u < 1.0f && *v >= 0.0f && *v < 1.0f;
}

/* The host screens (cockpit_host_screens) in view space: ids are
 * HOST_CAMERA or a group */
#define HOST_CAMERA COCKPIT_SCREENS
static int host_screens(bool cam_shown, const float *out[COCKPIT_HOST_MAX], int ids[COCKPIT_HOST_MAX])
{
	int n = 0;
	if (cam_shown)
	{
		out[n] = &cam[0][0];
		ids[n++] = HOST_CAMERA;
	}
	for (int g = 0; g < COCKPIT_SCREENS && n < COCKPIT_HOST_MAX; g++)
		if (g != live && sub_tex[g] && !side_on[g])
		{
			out[n] = &subs[g][0][0];
			ids[n++] = g;
		}
	return n;
}

/* The nearest host screen the ray from `eye` (view space) along d hits */
static bool ray_hits_host(const float eye[3], const float d[3], bool cam_shown, float *dist, float *u, float *v,
						  int *id)
{
	const float *q[COCKPIT_HOST_MAX];
	int ids[COCKPIT_HOST_MAX], n = host_screens(cam_shown, q, ids);
	bool hit = false;
	for (int i = 0; i < n; i++)
	{
		float qu, qv, t;
		if (ray_hits_quad((const float(*)[3])q[i], eye, d, &qu, &qv, &t) && (!hit || t < *dist))
		{
			hit = true;
			*dist = t;
			*u = qu;
			*v = qv;
			*id = ids[i];
		}
	}
	return hit;
}

/* The ray -> the page pixel of the side screen (MAP or SHIP, gl_mapscreen.h)
 * it is on, when that is up */
static bool ray_to_side(const float eye[3], const float d[3], int *group, int *px, int *py, float *dist)
{
	for (int g = COCKPIT_SCREEN_MAP; g <= COCKPIT_SCREEN_SHIP; g++)
	{
		float u, v, t;
		if (!side_on[g] || !ray_hits_quad((const float(*)[3])subs[g], eye, d, &u, &v, &t))
			continue;
		*group = g;
		*px = (int)(u * GL_SCREEN_W);
		*py = (int)(v * (full_page(g) ? GL_SCREEN_H : PANEL_ROW0));
		*dist = t;
		return true;
	}
	return false;
}

/* The ray from `eye` (view space) along d -> emulated screen pixel: on the
 * cockpit's live screen or the panel (*dist: how far along d), else on the
 * view pixel plane, held to its edges (*dist -1: at infinity) */
static bool ray_to_screen(const float eye[3], const float d[3], bool cam_shown, int *sx, int *sy, float *dist)
{
	float u, v, t;
	int id;
	/* the host's screens are no part of the game's */
	if (ray_hits_host(eye, d, cam_shown, &t, &u, &v, &id))
		return false;
	if (sub_on && ray_hits_quad((const float(*)[3])subs[live], eye, d, &u, &v, &t))
	{
		*sx = (int)(u * GL_SCREEN_W);
		*sy = (int)(v * (full_page(live) ? GL_SCREEN_H : PANEL_ROW0));
		if (live == COCKPIT_SCREEN_MAP && mapscreen_real_buttons())
			mapscreen_unmove(sx, sy);
		*dist = t;
		return true;
	}
	/* the side screens up in flight are not the game's pointer either */
	int sg, spx, spy;
	if (!sub_on && ray_to_side(eye, d, &sg, &spx, &spy, &t))
		return false;
	/* the panel, from the eye's position */
	if (ray_hits_quad((const float(*)[3])panel, eye, d, &u, &v, &t))
	{
		*sx = (int)(u * GL_SCREEN_W);
		*sy = PANEL_ROW0 + (int)(v * (GL_SCREEN_H - PANEL_ROW0));
		*dist = t;
		return true;
	}
	/* the screen's 2D layer is on the cockpit's screen only */
	if (sub_on)
		return false;

	/* the view pixel plane, held to its edges */
	if (d[2] >= -1e-6f)
		return false;
	float px = (d[0] / -d[2] / classic_kx() + 1.0f) * VIEW_W * 0.5f;
	float py = (1.0f - d[1] / -d[2] / classic_ky()) * VIEW_H * 0.5f;
	*sx = (int)fminf(fmaxf(px, 0.0f), VIEW_W - 1);
	*sy = (int)fminf(fmaxf(py, 0.0f), VIEW_H - 1);
	*dist = -1.0f;
	return true;
}

bool cockpit_window_to_screen(int wx, int wy, int *sx, int *sy)
{
	SharedView sv;
	get_shared(&sv);
	if (!sv.active || screen_w <= 0 || screen_h <= 0)
		return false;

	/* the ray through the pixel, in view space */
	float t = tanf((float)cockpit_fov * GLM_PI / 360.0f);
	float nx = 2.0f * ((float)wx + 0.5f) / (float)screen_w - 1.0f;
	float ny = 1.0f - 2.0f * ((float)wy + 0.5f) / (float)screen_h;
	float e[3] = {nx * sv.aspect * t, ny * t, -1.0f}, d[3], dist;
	eye_to_view_of(&sv.head_view, e, d);
	return ray_to_screen(sv.eye, d, sv.cam_on, sx, sy, &dist);
}

bool cockpit_window_to_side(int wx, int wy, int *group, int *px, int *py)
{
	SharedView sv;
	get_shared(&sv);
	if (!sv.active || screen_w <= 0 || screen_h <= 0)
		return false;
	float t = tanf((float)cockpit_fov * GLM_PI / 360.0f);
	float nx = 2.0f * ((float)wx + 0.5f) / (float)screen_w - 1.0f;
	float ny = 1.0f - 2.0f * ((float)wy + 0.5f) / (float)screen_h;
	float e[3] = {nx * sv.aspect * t, ny * t, -1.0f}, d[3], dist;
	eye_to_view_of(&sv.head_view, e, d);
	return ray_to_side(sv.eye, d, group, px, py, &dist);
}

bool cockpit_ray_to_side(const float origin[3], const float dir[3], int *group, int *px, int *py, float *dist)
{
	if (!active)
		return false;
	mat4 look = look_rotation();
	float o[3], d[3];
	eye_to_view_of(&look, origin, o);
	eye_to_view_of(&look, dir, d);
	return ray_to_side(o, d, group, px, py, dist);
}

bool cockpit_host_clicked(int wx, int wy)
{
	SharedView sv;
	get_shared(&sv);
	if (!sv.active || screen_w <= 0 || screen_h <= 0)
		return false;
	float t = tanf((float)cockpit_fov * GLM_PI / 360.0f);
	float nx = 2.0f * ((float)wx + 0.5f) / (float)screen_w - 1.0f;
	float ny = 1.0f - 2.0f * ((float)wy + 0.5f) / (float)screen_h;
	float e[3] = {nx * sv.aspect * t, ny * t, -1.0f}, d[3], u, v, dist;
	int id, sx, sy;
	eye_to_view_of(&sv.head_view, e, d);
	if (ray_hits_host(sv.eye, d, sv.cam_on, &dist, &u, &v, &id))
	{
		cockpit_host_tap(id, u, v);
		return true;
	}
	return ray_to_screen(sv.eye, d, sv.cam_on, &sx, &sy, &dist) && cockpit_screen_press(sx, sy);
}

/* The keys that bring up each group, or show its next page (ST scancodes) */
static const unsigned char GROUP_KEY[COCKPIT_SCREENS] = {0x3c, 0x3d, 0x3e}; /* F2, F3, F4 */

void cockpit_host_tap(int id, float u, float v)
{
	/* the options screen holds the game paused: only it is touched, a tap
	 * on another screen would leave it (and unpause) */
	if (game_screen() == SCREEN_OPTIONS)
		return;
	if (id == HOST_CAMERA)
		camera_tap(u, v);
	else if (id == COCKPIT_SCREEN_COMS && coms_menu)
		Input_ClickAt((int)(u * GL_SCREEN_W), (int)(v * PANEL_ROW0)); /* the comms menu's own */
	else if (id >= 0 && id < COCKPIT_SCREENS)
		Input_TapSTKey(GROUP_KEY[id]);
}

/* The galaxy map on the cockpit's screen holds the left stick (it scrolls
 * the map) until the screen is touched; touched again, the stick flies the
 * ship again. Not while the map is in select mode (the panel's button
 * MAP_SELECT_ICON toggles it): then touches are the game's. */
#define MAP_SELECT_ICON 10
static volatile bool map_focus = true, map_select;

bool cockpit_map_focused(void)
{
	if (game_screen() != SCREEN_GALAXY_MAP)
		map_focus = true, map_select = false;
	return map_focus;
}

void cockpit_map_click(int sx, int sy)
{
	int rects[64][4], n = game_icon_rects(rects, 64);
	if (game_screen() == SCREEN_GALAXY_MAP && n > MAP_SELECT_ICON && sx >= rects[MAP_SELECT_ICON][0] &&
		sx <= rects[MAP_SELECT_ICON][2] && sy >= rects[MAP_SELECT_ICON][1] && sy <= rects[MAP_SELECT_ICON][3])
		map_select = !map_select;
}

bool cockpit_screen_press(int sx, int sy)
{
	int g = live;
	if (sub_on && g == COCKPIT_SCREEN_MAP && game_screen() == SCREEN_GALAXY_MAP && sy >= 0 && sy < PANEL_ROW0 &&
		!map_select)
	{
		map_focus = !map_focus;
		return true;
	}
	if (!sub_on || g != COCKPIT_SCREEN_SHIP || sy < 0 || sy >= PANEL_ROW0)
		return false;
	int rects[64][4], n = game_icon_rects(rects, 64);
	for (int i = 0; i < n; i++)
		if (sx >= rects[i][0] && sx <= rects[i][2] && sy >= rects[i][1] && sy <= rects[i][3])
			return false;
	Input_TapSTKey(GROUP_KEY[g]);
	return true;
}

bool cockpit_ray_to_screen(const float origin[3], const float dir[3], int *sx, int *sy, float *dist)
{
	if (!active)
		return false;
	mat4 look = look_rotation();
	float o[3], d[3];
	eye_to_view_of(&look, origin, o);
	eye_to_view_of(&look, dir, d);
	return ray_to_screen(o, d, cam_on, sx, sy, dist);
}

bool cockpit_ray_hits_host(const float origin[3], const float dir[3], float *dist, float *u, float *v, int *id)
{
	if (!active)
		return false;
	mat4 look = look_rotation();
	float o[3], d[3];
	eye_to_view_of(&look, origin, o);
	eye_to_view_of(&look, dir, d);
	return ray_hits_host(o, d, cam_on, dist, u, v, id);
}

void cockpit_play_to_view(const float v[3], float out[3])
{
	mat4 look = look_rotation();
	eye_to_view_of(&look, v, out);
}

void cockpit_view_to_play(const float v[3], float out[3])
{
	mat4 look = look_rotation();
	mat4_xform_dir(&look, v, out);
}

float cockpit_panel_bottom(void)
{
	float y = panel[0][1];
	for (int i = 1; i < 4; i++)
		y = fminf(y, panel[i][1]);
	return y;
}

bool cockpit_menu(int *y0, int *y1)
{
	if (!active || !menu_on)
		return false;
	*y0 = menu_y0;
	*y1 = menu_y1;
	return true;
}

bool cockpit_screen_plane_play(float corners[4][3])
{
	if (!active || !headset || !vr_hands)
		return false;
	mat4 look = look_rotation();
	for (int i = 0; i < 4; i++)
		mat4_xform_dir(&look, screen_plane[i], corners[i]);
	return true;
}

bool cockpit_view_plane_play(float corners[4][3])
{
	if (!active || !headset || !vr_hands)
		return false;
	mat4 look = look_rotation();
	for (int i = 0; i < 4; i++)
		mat4_xform_dir(&look, view_plane[i], corners[i]);
	return true;
}

bool cockpit_play_point_to_pixel(const float p[3], int *sx, int *sy)
{
	if (!active)
		return false;
	float v[3], d[3];
	cockpit_play_to_view(p, v);
	sub3(v, eye, d);
	if (d[2] >= -1e-6f)
		return false;
	float px = (d[0] / -d[2] / classic_kx() + 1.0f) * VIEW_W * 0.5f;
	float py = (1.0f - d[1] / -d[2] / classic_ky()) * VIEW_H * 0.5f;
	*sx = (int)fminf(fmaxf(px, 0.0f), VIEW_W - 1);
	*sy = (int)fminf(fmaxf(py, 0.0f), VIEW_H - 1);
	return true;
}

bool cockpit_panel_play(float corners[4][3])
{
	if (!active)
		return false;
	mat4 look = look_rotation();
	for (int i = 0; i < 4; i++)
		mat4_xform_dir(&look, panel[i], corners[i]);
	return true;
}

static void to_play(const float c[4][3], float corners[4][3])
{
	mat4 look = look_rotation();
	for (int i = 0; i < 4; i++)
		mat4_xform_dir(&look, c[i], corners[i]);
}

bool cockpit_screen_play(float corners[4][3])
{
	if (!active || !sub_on)
		return false;
	to_play((const float(*)[3])subs[live], corners);
	return true;
}

bool cockpit_side_map_shown(void)
{
	return active && !sub_on && side_on[COCKPIT_SCREEN_MAP];
}

bool cockpit_map_play(float corners[4][3])
{
	if (!active || !side_on[COCKPIT_SCREEN_MAP])
		return false;
	to_play((const float(*)[3])subs[COCKPIT_SCREEN_MAP], corners);
	return true;
}

bool cockpit_place_play(int place, float corners[4][3])
{
	if (place == PLACE_CAMERA)
		return cockpit_camera_play(corners);
	for (int g = 0; g < COCKPIT_SCREENS; g++)
		if (GROUP_PLACE[g] == place)
		{
			if (g == COCKPIT_SCREEN_MAP && cockpit_map_play(corners))
				return true;
			if (g == COCKPIT_SCREEN_SHIP && active && side_on[g])
			{
				to_play((const float(*)[3])subs[g], corners);
				return true;
			}
			if (!active || !sub_on || !sub_tex[g])
				return false;
			to_play((const float(*)[3])subs[g], corners);
			return true;
		}
	return false;
}

int cockpit_host_screens(float corners[][4][3], int ids[], int max)
{
	if (!active)
		return 0;
	const float *q[COCKPIT_HOST_MAX];
	int id[COCKPIT_HOST_MAX], n = host_screens(cam_on, q, id);
	if (n > max)
		n = max;
	for (int i = 0; i < n; i++)
	{
		to_play((const float(*)[3])q[i], corners[i]);
		ids[i] = id[i];
	}
	return n;
}

bool cockpit_camera_play(float corners[4][3])
{
	if (!active || !cam_on)
		return false;
	mat4 look = look_rotation();
	for (int i = 0; i < 4; i++)
		mat4_xform_dir(&look, cam[i], corners[i]);
	return true;
}
