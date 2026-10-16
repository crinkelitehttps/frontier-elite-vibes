/*
 * gl_camera.c - see gl_camera.h.
 */
#include <math.h>
#include <string.h>

#include <SDL.h>

#include "gl_camera.h"

#include "clipboard_font.h"
#include "audio.h" /* Call_PlaySFX */
#include "galaxy/fe2vm.h"
#include "game_state.h"
#include "host.h" /* rdword, wrword, hcalls_count */
#include "hostcall.h"
#include "m68000.h"
#include "main.h"
#include "renderer.h" /* logscreen2 */

bool cockpit_camera = true;

#define CAMERA_FPS 50

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
#define TAP_PX  10.0f /* a tap that near a thing picks it */
#define MAX_HITS NUM_SLOTS

enum
{
	CAM_FRONT,
	CAM_REAR,
	CAM_LEFT,
	CAM_RIGHT,
	CAM_UP,
	CAM_DOWN,
	CAM_COUNT
};
static const char *const NAMES[CAM_COUNT] = {"FRONT", "REAR", "LEFT", "RIGHT", "UP", "DOWN"};
/* Each camera's axes (right, up, ahead) in the game's view of the ship: x
 * right, y up, z ahead */
static const float AXES[CAM_COUNT][3][3] = {
	{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
	{{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}},
	{{0, 0, 1}, {0, 1, 0}, {-1, 0, 0}},
	{{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},
	{{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
	{{1, 0, 0}, {0, 0, 1}, {0, -1, 0}},
};

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
	float pos[3], r; /* the camera's axes (x right, y up, z ahead): where
	                    it is and its radius, view units */
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

void camera_tap(float u, float v)
{
	if (!lock)
		return;
	/* the ray through the tapped pixel */
	float d[3] = {(u * CAMERA_W - VIEW_CX) / VIEW_F, (VIEW_CY - v * CAMERA_H) / VIEW_F, 1.0f};
	float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	int best = -1;
	float best_a = 0;
	SDL_LockMutex(lock);
	/* the thing whose sphere the ray passes through (TAP_PX near does for a
	 * small one), the one that looks smallest if several: a ship in front
	 * of a planet */
	for (int i = 0; i < pub_nhits; i++)
	{
		const Hit *h = &pub_hits[i];
		float dist = sqrtf(h->pos[0] * h->pos[0] + h->pos[1] * h->pos[1] + h->pos[2] * h->pos[2]);
		if (!(dist > h->r))
			continue; /* inside it */
		float a = asinf(h->r / dist), tap = TAP_PX / VIEW_F;
		if (a < tap)
			a = tap;
		float c = (d[0] * h->pos[0] + d[1] * h->pos[1] + d[2] * h->pos[2]) / (dl * dist);
		if (acosf(c > 1.0f ? 1.0f : c < -1.0f ? -1.0f : c) <= a && (best < 0 || a < best_a))
		{
			best = i;
			best_a = a;
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
	uint32_t a6 = (uint32_t)GetReg(REG_A6), m = (uint32_t)GetReg(REG_A5);
	uint32_t addr = (uint32_t)rdlong(a6 - 212), space = big_space();
	int slot = (uint8_t)rdbyte(addr + OBJ_SLOT);
	if (nhits >= MAX_HITS || !slot_shown(space, slot) || slot_object(space, slot) != addr ||
		addr == game_player_ship())
		return;
	for (int i = 0; i < nhits; i++)
		if (hits[i].addr == addr)
			return;
	Hit *h = &hits[nhits++];
	h->addr = addr;
	h->slot = slot;
	for (int k = 0; k < 3; k++)
		h->pos[k] = (float)(int32_t)rdlong(a6 - 16 + 4 * (uint32_t)k);
	/* the model's radius, as L390f2's disc for a far one */
	h->r = ldexpf((float)(int16_t)rdword(m + 14), (int16_t)rdword(a6 - 208) + 1);
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
		const float(*a)[3] = AXES[batch_dir];
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
static void label(const char *s)
{
	for (int pass = 0; pass < 2; pass++)
	{
		uint32_t col = pass ? 0xffffffffu : 0xff000000u;
		int x0 = 4 + !pass, y0 = 3 + !pass;
		for (int k = 0; s[k]; k++)
			for (int y = 0; y < 16; y++)
			{
				unsigned char bits = clipboard_font_8x16[(unsigned char)s[k] * 16 + y];
				for (int x = 0; x < 8; x++)
					if (bits & (0x80 >> x))
						pixels[(y0 + y) * CAMERA_W + x0 + k * 8 + x] = col;
			}
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

void camera_capture(bool screen)
{
	if (!screen)
		note_flight_colours();
	if (!lock)
		return;
	take_target();
	if (!cockpit_camera || !game_player_ship())
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
	bool ok = run(FE2_Clear3DView, 0) && run(FLIGHT_VIEW, 0) && run(FE2_MakePalette, GAME_A6 + A6_MAIN_PALETTE1);
	if (ok)
	{
		/* the background as the game's frame loop has it, then the view */
		memset(STRam + scr, (rdbyte(GAME_A6 + A6_DO_CLR_2_SPACE_COLS) & 1) ? 0xe : 0, CAMERA_W * CAMERA_H);
		/* then the target and the labels on things, as the game's HUD
		 * draws them (Lcam_hud) */
		ok = run(FE2_Draw3DView, 0) && run(FE2_Lcam_hud, 0);
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
	label(NAMES[batch_dir]);
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
