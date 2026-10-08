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

#include "gl_api.h"
#include "gl_cockpit.h"
#include "gl_draw.h"
#include "gl_fill2d.h"
#include "gl_scene.h"

#include "freecam.h"
#include "gamepad.h"
#include "game_state.h"
#include "headtrack.h"
#include "host.h" /* rdword, wrword */
#include "m68000.h"
#include "main.h"
#include "renderer.h"

bool cockpit_enabled = true;
int cockpit_fov = 60;
bool headtrack_enabled = true;
int headtrack_port = HEADTRACK_DEFAULT_PORT;

#define VIEW_W   FILL2D_VIEW_W
#define VIEW_H   FILL2D_VIEW_H
#define HUD_DIST 1000.0f /* where the view pixel plane sits: inside the world's near/far */

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
#define PANEL_ROW0  168 /* first screen row of the panel */

static bool active;
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

/* Panel corners in view space: top left, top right, bottom left, bottom right */
static float panel[4][3];

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

static void build_panel(void)
{
	float a = PANEL_DOWN * GLM_PI / 180.0f;
	float c[3] = {0, -PANEL_DIST * sinf(a), -PANEL_DIST * cosf(a)};
	float down[3] = {0, -cosf(a), sinf(a)}; /* in the panel's plane, towards its bottom edge */
	for (int i = 0; i < 4; i++)
	{
		float u = (i & 1) ? 0.5f : -0.5f, v = (i & 2) ? 0.5f : -0.5f;
		panel[i][0] = c[0] + u * PANEL_W;
		panel[i][1] = c[1] + v * PANEL_H * down[1];
		panel[i][2] = c[2] + v * PANEL_H * down[2];
	}
}

bool cockpit_begin_frame(int window_w, int window_h)
{
	update_tracker();

	active = cockpit_enabled && use_renderer != R_OLD && game_screen() == SCREEN_FLIGHT &&
			 game_flight_view() == VIEW_FRONT && !freecam_active();
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
	return active;
}

bool cockpit_active(void)
{
	return active;
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

mat4 cockpit_world_projection(void)
{
	mat4 lens = world_lens();
	mat4 view = mat4_mul(&head_view, &pass_rot);
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
	mat4 w = cockpit_world_projection();
	return mat4_mul(&w, &l);
}

/* =========================================================================
 * Drawing
 * ========================================================================= */
static GLuint prog, vao, vbo;
static GLint u_tex, u_discard;

static const char *VS_QUAD = "in vec4 aPos;\n"
							 "in vec2 aUV;\n"
							 "out vec2 vUV;\n"
							 "void main(){ gl_Position = aPos; vUV = aUV; }\n";

static const char *FS_QUAD = "in vec2 vUV;\n"
							 "uniform sampler2D uTex;\n"
							 "uniform int uDiscard;\n"
							 "out vec4 fragColor;\n"
							 "void main(){\n"
							 "  vec4 c = texture(uTex, vUV);\n"
							 "  if (uDiscard != 0 && c.a < 0.5) discard;\n"
							 "  fragColor = vec4(c.rgb, 1.0);\n"
							 "}\n";

void cockpit_init(void)
{
	static const char *const attribs[] = {"aPos", "aUV"};
	prog = gd_build_program(VS_QUAD, FS_QUAD, attribs, 2);
	u_tex = glGetUniformLocation(prog, "uTex");
	u_discard = glGetUniformLocation(prog, "uDiscard");

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
} TexQuad;

static void tex_quad_pass(const void *data)
{
	const TexQuad *q = data;
	if (q->upload)
		gl_screen_upload();
	glUseProgram(prog);
	glUniform1i(u_tex, 0);
	glUniform1i(u_discard, q->discard);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, gl_screen_texture());
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof q->v, q->v, GL_STREAM_DRAW);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_2D, 0);
}

/* Queues screen rows y0..y1 on the quad `corners` (through `m`) */
static void queue_screen_quad(const mat4 *m, const float corners[4][3], int y0, int y1, bool discard, bool upload)
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
}

void cockpit_draw(void)
{
	if (!active)
		return;

	/* the game's 2D layer over its 3D view, on its classic directions */
	mat4 hud = cockpit_view_pixel_projection();
	const float view_corners[4][3] = {{0, 0, 0}, {VIEW_W, 0, 0}, {0, VIEW_H, 0}, {VIEW_W, VIEW_H, 0}};
	gd_set_viewport(GD_VP_WINDOW);
	queue_screen_quad(&hud, view_corners, 0, PANEL_ROW0, true, true);

	/* the control panel (with the scanner), on its own in front of the eye */
	mat4 cp = cockpit_projection();
	gd_set_projection(&cp);
	gd_push();
	gd_identity();
	gd_set_cull(false);
	queue_screen_quad(&cp, (const float(*)[3])panel, PANEL_ROW0, GL_SCREEN_H, false, false);
	gd_pop();
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

/* The passes the window (and a headset's eyes) show this frame, by
 * sampling them a little beyond their edges: each sample goes to the first
 * pass that has it, as the stencil does when they are drawn. Ahead is
 * always drawn, last. The eyes are last frame's, so with more room for
 * the head turning since. */
static void choose_passes(void)
{
	if (!passes_built)
		build_passes();
	bool want[COCKPIT_MAX_PASSES] = {false};
	float t = tanf((float)cockpit_fov * GLM_PI / 360.0f) * 1.1f;
	sample_view(&head_view, -aspect * t, aspect * t, -t, t, want);
	if (headset_eyes)
		for (int i = 0; i < 2; i++)
		{
			const float m = 1.25f;
			const VrView *v = &hs_eyes[i];
			mat4 V;
			headset_view(v, &V, NULL);
			sample_view(&V, v->tan_left * m, v->tan_right * m, v->tan_down * m, v->tan_up * m, want);
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
		if (active && scene_active())
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
bool cockpit_window_to_screen(int wx, int wy, int *sx, int *sy)
{
	if (!active || screen_w <= 0 || screen_h <= 0)
		return false;

	/* the ray through the pixel, in view space */
	float t = tanf((float)cockpit_fov * GLM_PI / 360.0f);
	float nx = 2.0f * ((float)wx + 0.5f) / (float)screen_w - 1.0f;
	float ny = 1.0f - 2.0f * ((float)wy + 0.5f) / (float)screen_h;
	float e[3] = {nx * aspect * t, ny * t, -1.0f}, d[3];
	eye_to_view_of(&head_view, e, d);

	/* the panel, from the eye's position */
	float u_axis[3], v_axis[3], n[3], to[3];
	sub3(panel[1], panel[0], u_axis);
	sub3(panel[2], panel[0], v_axis);
	cross3(u_axis, v_axis, n);
	float dn = dot3(d, n);
	if (fabsf(dn) > 1e-9f)
	{
		sub3(panel[0], eye, to);
		float dist = dot3(to, n) / dn;
		if (dist > 0.0f)
		{
			float p[3], rel[3];
			for (int i = 0; i < 3; i++)
				p[i] = eye[i] + d[i] * dist;
			sub3(p, panel[0], rel);
			float u = dot3(rel, u_axis) / dot3(u_axis, u_axis);
			float v = dot3(rel, v_axis) / dot3(v_axis, v_axis);
			if (u >= 0.0f && u < 1.0f && v >= 0.0f && v < 1.0f)
			{
				*sx = (int)(u * GL_SCREEN_W);
				*sy = PANEL_ROW0 + (int)(v * (GL_SCREEN_H - PANEL_ROW0));
				return true;
			}
		}
	}

	/* the view pixel plane, held to its edges */
	if (d[2] >= -1e-6f)
		return false;
	float px = (d[0] / -d[2] / classic_kx() + 1.0f) * VIEW_W * 0.5f;
	float py = (1.0f - d[1] / -d[2] / classic_ky()) * VIEW_H * 0.5f;
	*sx = (int)fminf(fmaxf(px, 0.0f), VIEW_W - 1);
	*sy = (int)fminf(fmaxf(py, 0.0f), VIEW_H - 1);
	return true;
}
