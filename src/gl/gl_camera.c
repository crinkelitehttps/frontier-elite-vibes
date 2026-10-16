/*
 * gl_camera.c - see gl_camera.h.
 */
#include <string.h>

#include <SDL.h>

#include "gl_camera.h"

#include "clipboard_font.h"
#include "galaxy/fe2vm.h"
#include "game_state.h"
#include "host.h" /* rdword, wrword, hcalls_count */
#include "hostcall.h"
#include "m68000.h"
#include "main.h"

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
#define A6_MAIN_PALETTE1       15876
#define A6_DO_CLR_2_SPACE_COLS 536

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

void camera_next(void)
{
	int d = SDL_AtomicGet(&cam_dir);
	SDL_AtomicSet(&cam_dir, (d + 1) % CAM_COUNT);
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
	hcalls[0x73] = cam_no; /* Nu_IsGLRenderer */
	hcalls[0x7a] = cam_pass;
	hcalls[0x82] = cam_no; /* Nu_NativeCulled */
	hcalls[0x83] = cam_no; /* Nu_NativeFar */
	hcalls[0x85] = cam_no; /* Call_ScreenControls */
	hcalls[0x86] = cam_no; /* Call_HudCross */
	hcalls[0x87] = cam_no; /* Call_HudVelocity */
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

void camera_capture(void)
{
	if (!cockpit_camera || !lock || !game_player_ship())
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

	fe2vm_game_hcalls(1);
	fe2vm_begin();
	batch_hcalls();
	wrword(GL_RENDERER_ON, 0);
	wrbyte(GAME_A6 + A6_3DVIEW_MODE, VIEW_FRONT);
	uint32_t scr = (uint32_t)rdlong(FE2_logscreen);
	bool ok = run(FE2_Clear3DView, 0) && run(FLIGHT_VIEW, 0) && run(FE2_MakePalette, GAME_A6 + A6_MAIN_PALETTE1);
	if (ok)
	{
		/* the background as the game's frame loop has it, then the view */
		memset(STRam + scr, (rdbyte(GAME_A6 + A6_DO_CLR_2_SPACE_COLS) & 1) ? 0xe : 0, CAMERA_W * CAMERA_H);
		ok = run(FE2_Draw3DView, 0);
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
