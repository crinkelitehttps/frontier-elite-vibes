/*
 * gl_camera.c - see gl_camera.h.
 */
#include <math.h>
#include <string.h>

#include <SDL.h>

#include "gl_camera.h"

#include "audio.h" /* Call_PlaySFX */
#include "galaxy/fe2vm.h"
#include "game_state.h"
#include "gl_cockpit.h"
#include "host.h" /* rdword, wrword, hcalls_count */
#include "hostcall.h"
#include "m68000.h"
#include "main.h"
#include "renderer.h" /* logscreen2 */

bool cockpit_camera = true;
int camera_zoom_max = 1024;

#define CAMERA_FPS 20 /* each picture is a run of the game's flight view (2 to 12 ms) */

/* The flight view, as cockpit_capture_world runs it (gl_cockpit.c) */
#ifdef FE2_SetupPlayerCamera
#define FLIGHT_VIEW FE2_SetupPlayerCamera
#else
#define FLIGHT_VIEW 0x57060 /* L62966 in fe2.s, L62966_SetupPlayerCamera in fe2_modded.s */
#endif
/* fe2.s: "gl_renderer_on: dc.w 1" just before L41b2e_FlipScreen; 0 = the
 * game's software renderer */
#define GL_RENDERER_ON (FE2_FlipScreen - 2)
#define A6_MAIN_PALETTE1       15876 /* 16 words, then A6_main_palette2's 16 */
#define A6_DO_CLR_2_SPACE_COLS 536
#define A6_SPACE_BG_COL        16004 /* word, as L41a3a_SetDefaultPalette sets them */
#define A6_DYN_COLS_FIRST      16006 /* word */
#define A6_TARGET              708   /* byte: the target's slot, 0 none */
#define A6_BIG_SPACE           14466
#define NUM_SLOTS              0x73 /* objects 0..$72 (L4477c) */
#define OBJ_NOT_DRAWN          97   /* byte: L44480 skips the object while set */
#define OBJ_SLOT               93
/* byte: what the object's drawing adds this frame, set as it happens
 * (L45dc8 firing: bit 1 a shot, bits 2-4 from which gun) and cleared by
 * L44480 as it draws them (bit 0 a hit's flash) */
#define OBJ_SHOTS              285
#define SHOT_BITS              0x1f
#define SFX_TARGET             28 /* L62f1e's, a target picked */
/* ZProject: 256 pixels at distance 1, the view's centre (L390f2) */
#define VIEW_F  256.0f
#define VIEW_CX 160.0f
#define VIEW_CY 84.0f
#define TAP_PX  10.0f /* a tap that near a thing's centre picks it */
#define MAX_HITS NUM_SLOTS

enum
{
	CAM_FRONT,
	CAM_REAR,
	CAM_LEFT,
	CAM_RIGHT,
	CAM_UP,
	CAM_DOWN,
	CAM_TARGET, /* aimed at the target, moved up close to it */
	CAM_COUNT
};
/* Buttons after the directions: the target view's zoom */
#define BTN_ZOOM_IN  CAM_COUNT
#define BTN_ZOOM_OUT (CAM_COUNT + 1)
#define BTN_COUNT    (CAM_COUNT + 2)
/* Each camera's axes (right, up, ahead) in the game's view of the ship: x
 * right, y up, z ahead */
static const float AXES[CAM_COUNT][3][3] = {
	{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
	{{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}},
	{{0, 0, 1}, {0, 1, 0}, {-1, 0, 0}},
	{{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},
	{{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
	{{1, 0, 0}, {0, 0, 1}, {0, -1, 0}},
	{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, /* CAM_TARGET: tgt_axes instead */
};
/* CAM_TARGET's axes, worked out for each picture (aim_at) */
static float tgt_axes[3][3];
/* The target view closes to the target's distance / 2^zoom_level */
#define ZOOM_MIN 1
#define ZOOM_MAX 20 /* levels: 2^level, as far as camera_zoom_max lets it */
static SDL_atomic_t zoom_level = {3};

/* The highest level camera_zoom_max allows */
static int zoom_top(void)
{
	int top = ZOOM_MIN;
	while (top < ZOOM_MAX && (1 << (top + 1)) <= camera_zoom_max)
		top++;
	return top;
}

static SDL_atomic_t cam_dir;

/* The game draws `pixels`; the drawing takes them (pub_seq newer than its
 * own) into the texture */
static SDL_mutex *lock;
static uint32_t pixels[CAMERA_W * CAMERA_H], pub[CAMERA_W * CAMERA_H];
static unsigned pub_seq, tex_seq;
static GLuint tex;

void camera_init(void)
{
	if (!lock)
		lock = SDL_CreateMutex();
}

void camera_shutdown(void)
{
	if (tex)
		glDeleteTextures(1, &tex);
	tex = 0;
	tex_seq = 0;
}

static void camera_next(void)
{
	int d = SDL_AtomicGet(&cam_dir);
	SDL_AtomicSet(&cam_dir, (d + 1) % CAM_COUNT);
}

/* =========================================================================
 * Things in the picture, to pick as the target by tapping them
 * ========================================================================= */
typedef struct
{
	uint32_t addr;
	int slot;
	float x, y; /* its centre in the picture, where its label starts */
} Hit;

/* The batch's (game thread), and the shown picture's (under `lock`) */
static Hit hits[MAX_HITS], pub_hits[MAX_HITS];
static int nhits, pub_nhits;
/* A tap's pick, for the game thread (under `lock`) */
static int want_slot;
static uint32_t want_addr;

static uint32_t big_space(void)
{
	return (uint32_t)rdlong(GAME_A6 + A6_BIG_SPACE);
}

static uint32_t slot_object(uint32_t space, int i)
{
	return space + (uint32_t)(int)rdword(FE2_obj_offsets + 2 * (uint32_t)i);
}

/* The slot holds a thing the flight view labels and lets you click on
 * (L4477c) */
static bool slot_shown(uint32_t space, int i)
{
	int flags = (uint8_t)rdbyte(space + (uint32_t)i);
	return i > 0 && i < NUM_SLOTS && (flags & 0x08) && !(flags & 0x20);
}

static int button_at(float x, float y);

void camera_tap(float u, float v)
{
	if (!lock)
		return;
	float x = u * CAMERA_W, y = v * CAMERA_H;
	int b = button_at(x, y);
	if (b >= 0)
	{
		if (b < CAM_COUNT)
			SDL_AtomicSet(&cam_dir, b);
		else
		{
			int z = SDL_AtomicGet(&zoom_level) + (b == BTN_ZOOM_IN ? 1 : -1);
			int top = zoom_top();
			SDL_AtomicSet(&zoom_level, z < ZOOM_MIN ? ZOOM_MIN : z > top ? top : z);
		}
		return;
	}
	int best = -1;
	float best_d = 0;
	SDL_LockMutex(lock);
	/* the thing whose centre is nearest the tap, within TAP_PX: each has
	 * that one point, as clicking in the flight view (L62f1e) */
	for (int i = 0; i < pub_nhits; i++)
	{
		float d = hypotf(x - pub_hits[i].x, y - pub_hits[i].y);
		if (d <= TAP_PX && (best < 0 || d < best_d))
		{
			best = i;
			best_d = d;
		}
	}
	if (best >= 0)
	{
		want_slot = pub_hits[best].slot;
		want_addr = pub_hits[best].addr;
	}
	SDL_UnlockMutex(lock);
	if (best < 0)
		camera_next();
}

/* Game thread: the tap's pick becomes the target, as clicking it in the
 * flight view does (L62f1e) */
static void take_target(void)
{
	SDL_LockMutex(lock);
	int slot = want_slot;
	uint32_t addr = want_addr;
	want_slot = 0;
	SDL_UnlockMutex(lock);
	uint32_t space = big_space();
	if (!slot || !slot_shown(space, slot) || slot_object(space, slot) != addr ||
		(uint8_t)rdbyte(GAME_A6 + A6_TARGET) == slot)
		return;
	wrbyte(GAME_A6 + A6_TARGET, (uint8_t)slot);
	uint32_t d0 = (uint32_t)GetReg(REG_D0), d1 = (uint32_t)GetReg(REG_D1);
	SetReg(REG_D0, SFX_TARGET);
	SetReg(REG_D1, 0);
	Call_PlaySFX();
	SetReg(REG_D0, d0);
	SetReg(REG_D1, d1);
}

/* =========================================================================
 * Shots: the flight view's drawing clears them, so the batch (drawn after
 * it) puts back what the frame had
 * ========================================================================= */
static uint8_t shots[NUM_SLOTS];

void camera_note_shots(void)
{
	uint32_t space = big_space();
	for (int i = 1; i < NUM_SLOTS; i++)
		shots[i] = slot_shown(space, i) ? (uint8_t)rdbyte(slot_object(space, i) + OBJ_SHOTS) & SHOT_BITS : 0;
}

static void put_shots(void)
{
	uint32_t space = big_space();
	for (int i = 1; i < NUM_SLOTS; i++)
		if (shots[i] && slot_shown(space, i))
		{
			uint32_t obj = slot_object(space, i);
			wrbyte(obj + OBJ_SHOTS, (uint8_t)rdbyte(obj + OBJ_SHOTS) | shots[i]);
		}
}

void camera_shots_drawn(void)
{
	uint32_t space = big_space();
	for (int i = 1; i < NUM_SLOTS; i++)
	{
		int flags = (uint8_t)rdbyte(space + (uint32_t)i);
		uint32_t obj = slot_object(space, i);
		if (!(flags & 0x08) || rdbyte(obj + OBJ_NOT_DRAWN))
			continue;
		/* L44480: a shot from one gun (L445e8), then the hit's flash */
		uint8_t b = (uint8_t)rdbyte(obj + OBJ_SHOTS);
		if (b & 0x02)
		{
			b &= (uint8_t)~0x02;
			b &= (uint8_t)~((b & 0x04) ? 0x04 : (b & 0x08) ? 0x08 : 0x10);
		}
		b &= (uint8_t)~0x01;
		wrbyte(obj + OBJ_SHOTS, b);
	}
}

/* =========================================================================
 * The batch's host calls
 * ========================================================================= */
static int batch_dir;
static int16_t saved_camera[9];
static uint16_t ext_pal[240];
static int ext_len;

static uint32_t param_long(int n)
{
	return (uint32_t)rdlong((uint32_t)GetReg(REG_A7) + 4 * (uint32_t)n);
}

static void cam_nothing(void) {}

static void cam_no(void)
{
	SetReg(REG_D0, 0);
}

/* Nu_NativeFar (Lnative_clip in L390f2, every object before the cull, the
 * object's frame at a6, its model at a5): where a thing the flight view
 * would let you click on is in the picture. The game goes on as without
 * the native scene. */
static void cam_far(void)
{
	SetReg(REG_D0, 0);
	uint32_t a6 = (uint32_t)GetReg(REG_A6);
	float z = (float)(int32_t)rdlong(a6 - 8);
	uint32_t addr = (uint32_t)rdlong(a6 - 212), space = big_space();
	int slot = (uint8_t)rdbyte(addr + OBJ_SLOT);
	if (z < 64.0f || nhits >= MAX_HITS || !slot_shown(space, slot) || slot_object(space, slot) != addr ||
		addr == game_player_ship())
		return;
	for (int i = 0; i < nhits; i++)
		if (hits[i].addr == addr)
			return;
	Hit *h = &hits[nhits++];
	h->addr = addr;
	h->slot = slot;
	/* as L390f2 puts it at 110(a4) */
	h->x = VIEW_CX + VIEW_F * (float)(int32_t)rdlong(a6 - 16) / z;
	h->y = VIEW_CY - VIEW_F * (float)(int32_t)rdlong(a6 - 12) / z;
}

/* Clears: to black or blue, as the software renderer has them */
static void cam_memset(void)
{
	memset(STRam + param_long(1), 0, param_long(0));
}

static void cam_memset_blue(void)
{
	memset(STRam + param_long(1), 0xe, param_long(0));
}

/* Call_MakeExtPalette's work, into this batch's palette */
static void cam_ext_palette(void)
{
	uint32_t list = param_long(0);
	int len = (uint16_t)rdword(list) >> 2;
	if (len > (int)(sizeof ext_pal / sizeof ext_pal[0]))
		len = sizeof ext_pal / sizeof ext_pal[0];
	ext_len = len;
	list += 2;
	for (int i = 0; i < len; i++, list += 4)
	{
		ext_pal[i] = (uint16_t)rdword(list);
		wrword(list + 2, (uint16_t)(rdword(list + 2) + (16 << 2)));
	}
}

/* Text (labels on things, drawn by the software renderer's text
 * primitive): into the screen the view is drawn in, not the 2D layer's */
static void cam_draw_str(void)
{
	unsigned long saved = logscreen2;
	logscreen2 = FE2_logscreen; /* where the batch draws (camera_capture) */
	Call_DrawStr();
	logscreen2 = saved;
}

static void cam_draw_str_shadowed(void)
{
	unsigned long saved = logscreen2;
	logscreen2 = FE2_logscreen; /* where the batch draws (camera_capture) */
	Call_DrawStrShadowed();
	logscreen2 = saved;
}

/* Call_CockpitPass (Lcockpit_world): one pass, the camera object at a3
 * (3x3 words, 1.0 = 32767, row major, columns right, up, ahead) turned to
 * the camera's axes, then put back */
static void cam_pass(void)
{
	uint32_t cam = (uint32_t)GetReg(REG_A3);
	if (GetReg(REG_D0) == 0)
	{
		const float(*a)[3] = batch_dir == CAM_TARGET ? tgt_axes : AXES[batch_dir];
		for (int i = 0; i < 9; i++)
			saved_camera[i] = (int16_t)rdword(cam + 2 * (uint32_t)i);
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
			{
				float v = 0;
				for (int i = 0; i < 3; i++)
					v += (float)saved_camera[r * 3 + i] * a[c][i];
				wrword(cam + 2 * (uint32_t)(r * 3 + c), (int)(v < -32767.0f ? -32767.0f : v > 32767.0f ? 32767.0f : v));
			}
		wrbyte(cam + 92, 0); /* its frame-turned copy is stale */
		SetReg(REG_D0, 1);
		return;
	}
	for (int i = 0; i < 9; i++)
		wrword(cam + 2 * (uint32_t)i, saved_camera[i]);
	wrbyte(cam + 92, 0);
	SetReg(REG_D0, 0);
}

/* The batch's table: drawing into the game's screen as the original does,
 * nothing recorded for GL, the native scene's hooks off */
static void batch_hcalls(void)
{
	for (int i = 0; i < hcalls_count; i++)
		hcalls[i] = cam_nothing;
	hcalls[0x01] = cam_memset;
	hcalls[0x02] = cam_memset_blue;
	hcalls[0x05] = Call_BlitBmp;
	hcalls[0x06] = Call_OldHLine;
	hcalls[0x08] = Call_Memcpy;
	hcalls[0x09] = Call_PutPix;
	hcalls[0x0a] = Call_BackHLine;
	hcalls[0x0b] = Call_FillLine;
	hcalls[0x11] = cam_ext_palette;
	hcalls[0x16] = Call_HLine;
	hcalls[0x1b] = cam_draw_str_shadowed;
	hcalls[0x1c] = cam_draw_str;
	hcalls[0x73] = cam_no; /* Nu_IsGLRenderer */
	hcalls[0x7a] = cam_pass;
	hcalls[0x82] = cam_no; /* Nu_NativeCulled */
	hcalls[0x83] = cam_far; /* Nu_NativeFar */
	hcalls[0x85] = cam_no; /* Call_ScreenControls */
	hcalls[0x86] = cam_no; /* Call_HudCross */
	hcalls[0x87] = cam_no; /* Call_HudVelocity */
	hcalls[0x88] = cam_no; /* Call_HudTarget */
	hcalls[0x89] = cam_no; /* Call_HudLabels */
	hcalls[0x8a] = cam_no; /* Call_ScreenLeave */
	hcalls[0x8b] = cam_no; /* Call_StationResume */
	hcalls[0x8c] = cam_no; /* Call_ScreenWorld */
}

/* =========================================================================
 * Capture (game thread)
 * ========================================================================= */
static uint32_t rgb(uint16_t c)
{
	uint32_t b = (c & 0xfu) << 4, g = c & 0xf0u, r = (c & 0xf00u) >> 4;
	return 0xff000000u | b << 16 | g << 8 | r;
}

/* The direction's name in the top left corner, shadowed */
/* The direction buttons, bottom right of the picture: FORWARD, BACK, LEFT,
 * RIGHT, UP, DOWN left to right (the CAM_ order) */
#define BTN_SIZE   14
#define BTN_GAP    2
#define BTN_RIGHT  316
#define BTN_BOTTOM 164

static void button_rect(int i, int *x, int *y)
{
	/* the zoom buttons go left of the directions, + then - from the right */
	int k = i < CAM_COUNT ? i : -1 - (i - CAM_COUNT);
	*x = BTN_RIGHT - (CAM_COUNT - k) * BTN_SIZE - (CAM_COUNT - 1 - k) * BTN_GAP + 1;
	*y = BTN_BOTTOM - BTN_SIZE + 1;
}

/* Which button a point of the picture is on, -1 if none */
static int button_at(float x, float y)
{
	for (int i = 0; i < BTN_COUNT; i++)
	{
		if (i >= CAM_COUNT && SDL_AtomicGet(&cam_dir) != CAM_TARGET)
			break;
		int bx, by;
		button_rect(i, &bx, &by);
		if (x >= bx && x < bx + BTN_SIZE && y >= by && y < by + BTN_SIZE)
			return i;
	}
	return -1;
}

/* The map's pan buttons' look: a frame and a triangle; UP and DOWN have a
 * bar on the far side, FORWARD and BACK are plain */
static void draw_buttons(int current)
{
	const uint32_t edge = 0xffc08040, arrow = 0xffffffff; /* ABGR */
	for (int i = 0; i < BTN_COUNT; i++)
	{
		if (i >= CAM_COUNT && current != CAM_TARGET)
			break;
		int bx, by;
		button_rect(i, &bx, &by);
		uint32_t back = i == current ? 0xff805020 : 0xff402010;
		for (int y = 0; y < BTN_SIZE; y++)
			for (int x = 0; x < BTN_SIZE; x++)
				pixels[(by + y) * CAMERA_W + bx + x] =
					(x == 0 || y == 0 || x == BTN_SIZE - 1 || y == BTN_SIZE - 1) ? edge : back;
		int cx = bx + BTN_SIZE / 2, cy = by + BTN_SIZE / 2;
		if (i == CAM_TARGET || i >= CAM_COUNT)
		{
			if (i == CAM_TARGET) /* a sight: a ring with a dot */
			{
				for (int a = -4; a <= 4; a++)
					for (int b = -4; b <= 4; b++)
					{
						int d2 = a * a + b * b;
						if ((d2 >= 12 && d2 <= 20) || d2 <= 1)
							pixels[(cy + b) * CAMERA_W + cx + a] = arrow;
					}
			}
			else
			{
				for (int a = -4; a <= 4; a++)
					pixels[cy * CAMERA_W + cx + a] = arrow;
				if (i == BTN_ZOOM_IN)
					for (int b = -4; b <= 4; b++)
						pixels[(cy + b) * CAMERA_W + cx] = arrow;
			}
			continue;
		}
		bool horiz = i == CAM_LEFT || i == CAM_RIGHT;
		/* the way the tip points: -1 up / left, +1 down / right */
		int sign = (i == CAM_REAR || i == CAM_DOWN || i == CAM_RIGHT) ? 1 : -1;
		for (int d = 0; d < 7; d++)
			for (int k = -(d / 2); k <= d / 2; k++)
			{
				int along = sign > 0 ? 3 - d : d - 3;
				int px = horiz ? cx + along : cx + k;
				int py = horiz ? cy + k : cy + along;
				pixels[py * CAMERA_W + px] = arrow;
			}
		if (i == CAM_UP || i == CAM_DOWN)
			for (int x = -3; x <= 3; x++)
				pixels[(cy + sign * 5) * CAMERA_W + cx + x] = arrow;
	}
}

static bool run(uint32_t addr, uint32_t a1)
{
	fe2vm_regs r;
	memset(&r, 0, sizeof r);
	r.a[1] = a1;
	return fe2vm_call(addr, &r) == 0;
}

/* The flight view's colours: a game screen (a map, the station) puts its
 * own palette and background in the same places, so under the cockpit's
 * screen the batch draws with the last flight frame's */
static struct
{
	bool have;
	uint8_t pal[64];
	uint16_t bg, first;
	uint8_t clr;
} flight;

static void note_flight_colours(void)
{
	for (uint32_t i = 0; i < sizeof flight.pal; i++)
		flight.pal[i] = (uint8_t)rdbyte(GAME_A6 + A6_MAIN_PALETTE1 + i);
	flight.bg = (uint16_t)rdword(GAME_A6 + A6_SPACE_BG_COL);
	flight.first = (uint16_t)rdword(GAME_A6 + A6_DYN_COLS_FIRST);
	flight.clr = (uint8_t)rdbyte(GAME_A6 + A6_DO_CLR_2_SPACE_COLS);
	flight.have = true;
}

static void use_flight_colours(void)
{
	if (!flight.have)
		return;
	for (uint32_t i = 0; i < sizeof flight.pal; i++)
		wrbyte(GAME_A6 + A6_MAIN_PALETTE1 + i, flight.pal[i]);
	wrword(GAME_A6 + A6_SPACE_BG_COL, flight.bg);
	wrword(GAME_A6 + A6_DYN_COLS_FIRST, flight.first);
	wrbyte(GAME_A6 + A6_DO_CLR_2_SPACE_COLS, flight.clr);
}

/* =========================================================================
 * The target view: the camera moves up close to the target (the ship is
 * moved inside the batch, then put back) and turns to face it
 * ========================================================================= */
#define OBJ_POS_LO 44 /* three longs, mm; the high longs at +56 (2^32 mm a unit) */
#define OBJ_POS_HI 56
#define OBJ_CACHE  19 /* the game's cache of the absolute position: 19..43 */
#define OBJ_PARENT 68 /* byte, the frame's slot */
#define OBJ_ROTATING 69 /* byte, set if the position is in the parent's turning frame */
#define OBJ_TYPE   94 /* byte, 1 for a body */
#define OBJ_RADIUS_LO 266
#define OBJ_RADIUS_HI 270
#define MM_PER_KM  1e6
#define MIN_SHIP_KM 0.25 /* nearest it gets to a thing, by its kind */
#define MIN_BODY_KM 1.0
#define DOLLY_TRIES 6
#define DOLLY_TOLERANCE 0.0005 /* of the distance wanted */

typedef long long pos64;

static pos64 obj_pos(uint32_t obj, int axis)
{
	return ((pos64)rdlong(obj + OBJ_POS_HI + 4 * (uint32_t)axis) << 32) |
		   (uint32_t)rdlong(obj + OBJ_POS_LO + 4 * (uint32_t)axis);
}

/* Moves the ship and drops the game's cache of where it is */
static void ship_move(uint32_t ship, const double d[3])
{
	for (int a = 0; a < 3; a++)
	{
		pos64 v = obj_pos(ship, a) + (pos64)llround(d[a]);
		wrlong(ship + OBJ_POS_LO + 4 * (uint32_t)a, (int)(uint32_t)(v & 0xffffffff));
		wrlong(ship + OBJ_POS_HI + 4 * (uint32_t)a, (int)(v >> 32));
	}
	wrbyte(ship + OBJ_CACHE, 0);
}

/* The ship's position less the target's, mm, as the game works it out
 * (Lcam_relvec); the distance, or -1 */
static double relative(uint32_t target, uint32_t ship, double w[3])
{
	/* in the same frame the game just subtracts the positions: so do we,
	 * to the millimetre (its vector below has 15 bits) */
	if (rdbyte(ship + OBJ_PARENT) == rdbyte(target + OBJ_PARENT) && rdbyte(ship + OBJ_ROTATING) == rdbyte(target + OBJ_ROTATING))
	{
		for (int i = 0; i < 3; i++)
			w[i] = (double)(obj_pos(ship, i) - obj_pos(target, i));
		return sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
	}
	fe2vm_regs r;
	memset(&r, 0, sizeof r);
	r.a[0] = target;
	r.a[1] = ship;
	r.a[4] = big_space();
	if (fe2vm_call(FE2_Lcam_relvec, &r) != 0)
		return -1;
	double scale = ldexp(1.0, (int16_t)r.d[7]);
	for (int i = 0; i < 3; i++)
		w[i] = (double)(int16_t)r.d[i] * scale;
	return sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
}

/* How near the view goes to the target, mm */
static double nearest(uint32_t target)
{
	if ((uint8_t)rdbyte(target + OBJ_TYPE) != 1)
		return MIN_SHIP_KM * MM_PER_KM;
	double r = ((double)(uint16_t)rdword(target + OBJ_RADIUS_HI) +
				(double)(uint32_t)rdlong(target + OBJ_RADIUS_LO) / 4294967296.0) * 4294.967296;
	return (r * 1.5 > MIN_BODY_KM ? r * 1.5 : MIN_BODY_KM) * MM_PER_KM;
}

/* Moves the ship towards the target to distance / zoom. The sign of the
 * game's vector is tried, not trusted. False if it can't tell. */
static bool dolly(uint32_t target, uint32_t ship, double zoom)
{
	double w[3];
	double dist = relative(target, ship, w);
	if (dist <= 0)
		return false;
	double want = dist / zoom, least = nearest(target);
	if (want < least)
		want = least;
	if (dist <= want)
		return true;
	double sign = -1;
	for (int t = 0; t < DOLLY_TRIES; t++)
	{
		double err = dist - want;
		if (t > 0 && fabs(err) < want * DOLLY_TOLERANCE)
			break;
		double d[3];
		for (int i = 0; i < 3; i++)
			d[i] = sign * w[i] / dist * err;
		ship_move(ship, d);
		double now = relative(target, ship, w);
		if (now <= 0)
			return false;
		if (t == 0 && now > dist)
		{
			/* it went the wrong way: back, and the other way */
			for (int i = 0; i < 3; i++)
				d[i] = -2 * d[i];
			sign = 1;
			ship_move(ship, d);
			now = relative(target, ship, w);
			if (now <= 0)
				return false;
		}
		dist = now;
	}
	return true;
}

/* Nu_NativeFar for the probe run: where the target is in the view */
static uint32_t probe_target;
static float probe_pos[3];
static bool probe_seen;

static void cam_probe(void)
{
	SetReg(REG_D0, 0);
	uint32_t a6 = (uint32_t)GetReg(REG_A6);
	if ((uint32_t)rdlong(a6 - 212) != probe_target)
		return;
	for (int i = 0; i < 3; i++)
		probe_pos[i] = (float)(int32_t)rdlong(a6 - 16 + 4 * (uint32_t)i);
	probe_seen = true;
}

static uint32_t target_object(void)
{
	uint32_t space = big_space();
	int slot = (uint8_t)rdbyte(GAME_A6 + A6_TARGET);
	if (!slot || !slot_shown(space, slot))
		return 0;
	return slot_object(space, slot);
}

/* The axes that face the camera at `v`, the target's place in the front
 * view. Roll is the ship's up kept as near as it goes, but straight above
 * or below the ship that has no direction (the target under a ship flying
 * over it), so near there the last picture's roll is carried on instead,
 * and the two are blended on the way. */
#define ROLL_FREE_FROM 0.6f /* |ahead . up| from which the last roll counts */
#define ROLL_FREE_TO   0.95f

static void normalize3(float v[3], float len)
{
	for (int i = 0; i < 3; i++)
		v[i] /= len;
}

static float length3(const float v[3])
{
	return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

static void aim_axes(const float v[3], bool keep_roll)
{
	static float last_right[3];
	float *r = tgt_axes[0], *u = tgt_axes[1], *f = tgt_axes[2];
	memcpy(f, v, sizeof(float) * 3);
	normalize3(f, length3(f));
	/* the ship's up decides: right = up x ahead */
	float by_up[3] = {f[2], 0, -f[0]};
	float up_len = length3(by_up);
	/* the last right, made square to the new ahead */
	float by_last[3] = {0, 0, 0};
	float last_len = 0;
	if (keep_roll)
	{
		float d = last_right[0] * f[0] + last_right[1] * f[1] + last_right[2] * f[2];
		for (int i = 0; i < 3; i++)
			by_last[i] = last_right[i] - d * f[i];
		last_len = length3(by_last);
	}
	bool have_up = up_len > 1e-3f, have_last = last_len > 1e-3f;
	if (have_up)
		normalize3(by_up, up_len);
	if (have_last)
		normalize3(by_last, last_len);
	float t = (fabsf(f[1]) - ROLL_FREE_FROM) / (ROLL_FREE_TO - ROLL_FREE_FROM);
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	if (!have_up && !have_last)
	{
		r[0] = 1;
		r[1] = r[2] = 0;
	}
	else if (!have_up || !have_last)
		memcpy(r, have_up ? by_up : by_last, sizeof(float) * 3);
	else
	{
		float dot = by_up[0] * by_last[0] + by_up[1] * by_last[1] + by_up[2] * by_last[2];
		if (dot < 0) /* turned right round: the nearer end wins */
			memcpy(r, t > 0.5f ? by_last : by_up, sizeof(float) * 3);
		else
		{
			for (int i = 0; i < 3; i++)
				r[i] = (1 - t) * by_up[i] + t * by_last[i];
			normalize3(r, length3(r));
		}
	}
	memcpy(last_right, r, sizeof last_right);
	u[0] = f[1] * r[2] - f[2] * r[1];
	u[1] = f[2] * r[0] - f[0] * r[2];
	u[2] = f[0] * r[1] - f[1] * r[0];
}

/* The target view's set up: the ship moved close to the target and the axes
 * turned to it. A run with only the target drawn finds where it is. False
 * (the ship put back) if there is none. `saved` is the ship's bytes to put
 * back by target_done. */
static uint8_t ship_saved[64 - OBJ_CACHE + 4];

static void ship_keep(uint32_t ship)
{
	for (uint32_t i = 0; i < sizeof ship_saved; i++)
		ship_saved[i] = (uint8_t)rdbyte(ship + OBJ_CACHE + i);
}

static void ship_back(uint32_t ship)
{
	for (uint32_t i = 0; i < sizeof ship_saved; i++)
		wrbyte(ship + OBJ_CACHE + i, ship_saved[i]);
}

static bool run_view(bool draw)
{
	bool ok = run(FE2_Clear3DView, 0) && run(FLIGHT_VIEW, 0) && run(FE2_MakePalette, GAME_A6 + A6_MAIN_PALETTE1);
	return ok && (!draw || run(FE2_Draw3DView, 0));
}

static bool aim_at(uint32_t target, uint32_t ship, bool keep_roll)
{
	ship_keep(ship);
	if (!dolly(target, ship, (double)(1 << (SDL_AtomicGet(&zoom_level) < zoom_top() ? SDL_AtomicGet(&zoom_level) : zoom_top()))))
	{
		ship_back(ship);
		return false;
	}
	/* the run with only the target drawn, looking ahead */
	uint32_t space = big_space();
	uint8_t flags[NUM_SLOTS];
	for (int i = 1; i < NUM_SLOTS; i++)
	{
		flags[i] = (uint8_t)rdbyte(space + (uint32_t)i);
		if (slot_object(space, i) != target)
			wrbyte(space + (uint32_t)i, flags[i] & ~0x08);
	}
	probe_target = target;
	probe_seen = false;
	hcalls[0x83] = cam_probe;
	int dir = batch_dir;
	batch_dir = CAM_FRONT;
	bool ok = run_view(true);
	batch_dir = dir;
	hcalls[0x83] = cam_far;
	for (int i = 1; i < NUM_SLOTS; i++)
		wrbyte(space + (uint32_t)i, flags[i]);
	if (!ok || !probe_seen || !(probe_pos[0] || probe_pos[1] || probe_pos[2]))
	{
		ship_back(ship);
		return false;
	}
	aim_axes(probe_pos, keep_roll);
	return true;
}

void camera_capture(bool screen)
{
	if (!screen)
		note_flight_colours();
	if (!lock)
		return;
	take_target();
	if (!cockpit_camera || !game_player_ship() || !cockpit_camera_seen())
		return;
	static Uint64 last;
	Uint64 now = SDL_GetPerformanceCounter();
	if (last && now - last < SDL_GetPerformanceFrequency() / CAMERA_FPS)
		return;
	last = now;

	static HOSTCALL saved[256];
	memcpy(saved, hcalls, (size_t)hcalls_count * sizeof(HOSTCALL));
	batch_dir = SDL_AtomicGet(&cam_dir);
	ext_len = 0;
	nhits = 0;

	fe2vm_game_hcalls(1);
	fe2vm_begin();
	batch_hcalls();
	wrword(GL_RENDERER_ON, 0);
	wrbyte(GAME_A6 + A6_3DVIEW_MODE, VIEW_FRONT);
	if (screen)
		use_flight_colours();
	put_shots();
	uint32_t scr = (uint32_t)rdlong(FE2_logscreen);
	/* the target view: the ship is up close to the target for the picture */
	uint32_t ship = game_player_ship(), moved = 0;
	int shown = batch_dir;
	if (batch_dir == CAM_TARGET)
	{
		uint32_t target = target_object();
		/* the roll carries on while the same target stays in view */
		static uint32_t last_target;
		static unsigned last_seq;
		static unsigned seq;
		seq++;
		bool keep = target == last_target && last_seq + 1 == seq;
		if (target && target != ship && aim_at(target, ship, keep))
		{
			moved = ship;
			last_target = target;
			last_seq = seq;
		}
		else
			batch_dir = CAM_FRONT;
		ext_len = 0;
		nhits = 0;
	}
	bool ok = run_view(false);
	if (ok)
	{
		/* the background as the game's frame loop has it, then the view */
		memset(STRam + scr, (rdbyte(GAME_A6 + A6_DO_CLR_2_SPACE_COLS) & 1) ? 0xe : 0, CAMERA_W * CAMERA_H);
		/* then the target and the labels on things, as the game's HUD
		 * draws them (Lcam_hud) */
		ok = run(FE2_Draw3DView, 0);
		/* where the ship is, for the range on the target */
		if (moved)
			ship_back(moved);
		ok = ok && run(FE2_Lcam_hud, 0);
	}
	if (ok)
	{
		uint32_t pal[256];
		for (int i = 0; i < 256; i++)
			pal[i] = i < 16 ? rgb((uint16_t)rdword(GAME_A6 + A6_MAIN_PALETTE1 + 2 * (uint32_t)i))
				   : i < 16 + ext_len ? rgb(ext_pal[i - 16])
									  : 0xff000000u;
		const uint8_t *src = (const uint8_t *)STRam + scr;
		for (int i = 0; i < CAMERA_W * CAMERA_H; i++)
			pixels[i] = pal[src[i]];
	}
	memcpy(hcalls, saved, (size_t)hcalls_count * sizeof(HOSTCALL));
	fe2vm_end();
	fe2vm_game_hcalls(0);

	static bool failed;
	if (!ok && !failed)
		log_printf("Camera screen: no picture (%s)\n", fe2vm_error());
	failed = !ok;
	if (!ok)
		return;
	draw_buttons(shown);
	SDL_LockMutex(lock);
	memcpy(pub, pixels, sizeof pub);
	memcpy(pub_hits, hits, (size_t)nhits * sizeof hits[0]);
	pub_nhits = nhits;
	pub_seq++;
	SDL_UnlockMutex(lock);
}

/* =========================================================================
 * Drawing
 * ========================================================================= */
GLuint camera_texture(void)
{
	if (!lock)
		return 0;
	SDL_LockMutex(lock);
	unsigned seq = pub_seq;
	if (seq != tex_seq && seq)
	{
		if (!tex)
		{
			glGenTextures(1, &tex);
			glBindTexture(GL_TEXTURE_2D, tex);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, CAMERA_W, CAMERA_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			/* the original's pixels, big and square */
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		}
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, tex);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, CAMERA_W, CAMERA_H, GL_RGBA, GL_UNSIGNED_BYTE, pub);
		glBindTexture(GL_TEXTURE_2D, 0);
		tex_seq = seq;
	}
	SDL_UnlockMutex(lock);
	return tex;
}
