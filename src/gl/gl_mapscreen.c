/*
 * gl_mapscreen.c - see gl_mapscreen.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "gl_mapscreen.h"

#include "galaxy/fe2vm.h"
#include "game_state.h"
#include "gl_cockpit.h"
#include "gl_scene.h"
#include "host.h" /* rdword, wrword, hcalls_count */
#include "hostcall.h"
#include "m68000.h"
#include "renderer.h"
#include "screen_text.h"

bool cockpit_map_live = true;

#define IDLE_FPS  2
#define FOCUS_FPS 50
#define FOCUS_MS  300 /* the screen counts as in use this long after the pointer left it */

#define HC_DRAW_SCREEN 0x6f /* Nu_DrawScreen */

/* A6 offsets */
#define A6_MOD_GALAXY       28    /* long: module pointers */
#define A6_MOD_SYSTEM       52
#define A6_GALAXY_SHOWN     34    /* word: the galaxy map is the screen ($ffff) */
#define A6_DO_CLR           536   /* byte: bit 7 = a screen of the 3D view's kind, bit 0 = fill it */
#define A6_MAIN_PALETTE1    15876
#define OWNER_GALAXY        16    /* A6_SCREEN_OWNER values */
#define OWNER_SYSTEM        40
/* The two map modules' state: the galaxy map's 912..1076, the system map's
 * 1082..1196. It is the game's own, and what the flight reads of it (the
 * selected system, for the jump) is the map's last: the batch starts from it
 * and what it ends with stays, where the rest of the machine is put back. */
#define KEEP_FIRST 912 /* (906..909 are the panel module's: which module has it, the scanner shown) */
#define KEEP_END   1200
/* ... and where the right-drag has turned it: the mouse control's motion,
 * the game's A6_mouse_motion_ctrl_x, y (the view's angles are these) */
#define KEEP2_FIRST 14488
#define KEEP2_LEN   4
/* The key state the maps read: the arrows and right shift, and the game's
 * keyboard queue (the flight view's keys are not the map's) */
#define A6_MOUSE_ABS_X     14494 /* words */
#define A6_MOUSE_ABS_Y     14496
#define MOUSE_BUTTONS      0xf8  /* what the host puts in the buttons word with none held */
#define A6_SCANCODE_ASCII  16336
#define A6_PUSHED_ASCII    16337
#define A6_SCANCODE_ASCII2 16339
#define A6_KEY_RSHIFT      16396
#define A6_KEY_CURSDOWN    16414
#define A6_KEY_CURSLEFT    16417
#define A6_KEY_CURSRIGHT   16419
#define A6_KEY_CURSUP      16422

/* L457d8, the modules' fn 16 loop, is 0x7c before L45854 (no label) */
#define MODULES_FN16 (FE2_CallHUDModfuncs - 0x7c)
#define MODULES_OP   0x022e /* its first instruction, andi.b #,598(a6) */

/* Which map is up, and the motion of its right drag (the flight's own mouse
 * control is not the map's, so this is kept here) */
static struct
{
	bool held;
	int owner;
	uint8_t ctrl[KEEP2_LEN];
} kept;

static MapPage *flipped; /* the batch's flip puts the picture here */
static bool got_page;

void mapscreen_forget(void)
{
	kept.held = false;
}

/* =========================================================================
 * Input
 * ========================================================================= */
#define MAX_KEYS 8
typedef struct
{
	bool on;
	int x, y, buttons, dx, dy;
	bool pressed, released; /* the left button, since the last batch */
} Pointer;

static SDL_SpinLock in_lock;
static struct
{
	Pointer ptr[MAPSCREEN_SOURCES];
	bool drag; /* a drag other than a mouse's right button */
	int drag_dx, drag_dy;
	Uint64 seen; /* the pointer was last on the screen */
	unsigned char keys[MAX_KEYS];
	int nkeys;
	bool arrows[4]; /* held: up, down, left, right (a controller's stick) */
	bool grabbed;	/* the stick scrolls the map until a press elsewhere */
} in;

void mapscreen_pointer(int source, bool on, int x, int y, int buttons, int dx, int dy)
{
	SDL_AtomicLock(&in_lock);
	Pointer *p = &in.ptr[source];
	if (on)
	{
		if ((buttons & 2) && !(p->on && (p->buttons & 2)))
			p->pressed = true, in.grabbed = true;
		if (!(buttons & 2) && p->on && (p->buttons & 2))
			p->released = true;
		p->dx += dx;
		p->dy += dy;
		p->x = x;
		p->y = y;
		p->buttons = buttons;
		in.seen = SDL_GetPerformanceCounter();
	}
	else if (p->on)
	{
		if (p->buttons & 2)
			p->released = true;
		p->buttons = 0;
	}
	p->on = on;
	SDL_AtomicUnlock(&in_lock);
}

void mapscreen_drag(bool held, int dx, int dy)
{
	SDL_AtomicLock(&in_lock);
	in.drag = held;
	if (held)
	{
		in.drag_dx += dx;
		in.drag_dy += dy;
		in.seen = SDL_GetPerformanceCounter();
	}
	SDL_AtomicUnlock(&in_lock);
}

void mapscreen_key(unsigned char scancode)
{
	SDL_AtomicLock(&in_lock);
	if (in.nkeys < MAX_KEYS)
		in.keys[in.nkeys++] = scancode;
	SDL_AtomicUnlock(&in_lock);
}

void mapscreen_scroll(bool up, bool down, bool left, bool right)
{
	SDL_AtomicLock(&in_lock);
	in.arrows[0] = up, in.arrows[1] = down, in.arrows[2] = left, in.arrows[3] = right;
	if (up || down || left || right)
		in.seen = SDL_GetPerformanceCounter();
	SDL_AtomicUnlock(&in_lock);
}

void mapscreen_grab(bool on)
{
	SDL_AtomicLock(&in_lock);
	in.grabbed = on;
	SDL_AtomicUnlock(&in_lock);
}

bool mapscreen_grabbed(void)
{
	SDL_AtomicLock(&in_lock);
	bool g = in.grabbed;
	SDL_AtomicUnlock(&in_lock);
	return g;
}

bool mapscreen_pointed(void)
{
	SDL_AtomicLock(&in_lock);
	bool used = in.drag || in.nkeys;
	for (int i = 0; i < MAPSCREEN_SOURCES; i++)
		used = used || in.ptr[i].on || in.ptr[i].pressed || in.ptr[i].released;
	used = used ||
		   (in.seen && SDL_GetPerformanceCounter() - in.seen < SDL_GetPerformanceFrequency() * FOCUS_MS / 1000);
	SDL_AtomicUnlock(&in_lock);
	return used;
}

bool mapscreen_up(void)
{
	if (!cockpit_map_live || !cockpit_screens_on() || !game_player_ship())
		return false;
	/* in flight, or beside the station's services (docked: the game is on
	 * their screen, the world captured round it) */
	return game_screen() == SCREEN_FLIGHT || game_screen() == SCREEN_STATION;
}

bool mapscreen_due(void)
{
	if (!mapscreen_up())
		return false;
	static Uint64 last;
	Uint64 now = SDL_GetPerformanceCounter(), freq = SDL_GetPerformanceFrequency();
	if (last && now - last < freq / (mapscreen_pointed() ? FOCUS_FPS : IDLE_FPS))
		return false;
	last = now;
	return true;
}

/* The batch's Nu_DrawScreen: the game has flipped the page, the picture is
 * the screen shown */
static void page_flip(void)
{
	screen_text_draw_queue();
	memcpy(flipped->screen, VideoRaster, sizeof flipped->screen);
	memcpy(flipped->main_pal, MainPalette, sizeof flipped->main_pal);
	flipped->len_main = len_main_palette;
	memcpy(flipped->ctrl_pal, CtrlPalette, sizeof flipped->ctrl_pal);
	flipped->bgcol = fe2_bgcol;
	screen_text_clear_queue();
	got_page = true;
}

static bool call_a1(uint32_t addr, uint32_t a1)
{
	fe2vm_regs r;
	memset(&r, 0, sizeof r);
	r.a[1] = a1;
	return fe2vm_call(addr, &r) == 0;
}

static bool call(uint32_t addr)
{
	return call_a1(addr, 0);
}

/* N34174_FillBlueLogscreen (Call_MemsetBlue) */
static void fill_blue(void)
{
	memset(STRam + (uint32_t)rdlong(FE2_logscreen2), use_renderer == R_OLD ? 0xe : 255, 53760);
	fe2_bgcol = 0xe;
}

/* What the game's loop does for the frame once the modules have run
 * (L428c6_GameLoop): a screen of the 3D view's kind (A6_do_clr_2_space_cols
 * negative: the galaxy map's stars are objects) has its palette made, its
 * background filled and the view drawn, then the HUD functions and the
 * flip; the others draw straight into the screen shown */
static bool finish_frame(void)
{
	uint32_t a6 = GAME_A6;
	int clr = (uint8_t)rdbyte(a6 + A6_DO_CLR);
	if (clr & 0x80)
	{
		if (!call_a1(FE2_MakePalette, a6 + A6_MAIN_PALETTE1))
			return false;
		if (clr & 1)
			fill_blue();
		return call(FE2_Draw3DView) && call(FE2_CallHUDModfuncs) && call(FE2_FlipScreen);
	}
	if (!call(FE2_CallHUDModfuncs))
		return false;
	page_flip();
	return true;
}

/* The map module's fn 8, as the game's F2 / F3 key runs it */
static bool enter(void)
{
	uint32_t a6 = GAME_A6;
	int owner = kept.held ? kept.owner : OWNER_GALAXY;
	uint32_t mod = (uint32_t)rdlong(a6 + (owner == OWNER_SYSTEM ? A6_MOD_SYSTEM : A6_MOD_GALAXY));
	/* with the galaxy map shown its fn 8 goes on to the system map */
	wrword(a6 + A6_GALAXY_SHOWN, 0);
	return call(mod + 8);
}

/* L41fe8_UpdateMouse as the vblank handler calls it: d0, d1 the motion,
 * d2 the buttons */
static bool update_mouse(int dx, int dy, int buttons)
{
	fe2vm_regs r;
	memset(&r, 0, sizeof r);
	r.d[0] = (uint16_t)dx;
	r.d[1] = (uint16_t)dy;
	r.d[2] = MOUSE_BUTTONS | (unsigned)buttons;
	return fe2vm_call(FE2_UpdateMouse, &r) == 0;
}

/* The input since the last batch, as the game would have had it */
static bool play_input(void)
{
	uint32_t a6 = GAME_A6;
	Pointer p;
	bool drag;
	int drag_dx, drag_dy, nkeys;
	unsigned char keys[MAX_KEYS];
	SDL_AtomicLock(&in_lock);
	/* the pointer that is on the screen, a hand's (the later source) before
	 * the mouse's */
	int src = 0;
	for (int i = 0; i < MAPSCREEN_SOURCES; i++)
		if (in.ptr[i].on || in.ptr[i].released || in.ptr[i].pressed)
			src = i;
	p = in.ptr[src];
	for (int i = 0; i < MAPSCREEN_SOURCES; i++)
		in.ptr[i].pressed = in.ptr[i].released = false, in.ptr[i].dx = in.ptr[i].dy = 0;
	drag = in.drag;
	drag_dx = in.drag_dx;
	drag_dy = in.drag_dy;
	in.drag_dx = in.drag_dy = 0;
	nkeys = in.nkeys;
	memcpy(keys, in.keys, sizeof keys);
	in.nkeys = 0;
	bool arrows[4];
	memcpy(arrows, in.arrows, sizeof arrows);
	SDL_AtomicUnlock(&in_lock);

	if (getenv("GLF_MAP_LOG") && (p.pressed || p.released || p.buttons || drag))
		log_printf("map input: on %d at %d,%d buttons %d pressed %d released %d d %d,%d drag %d %d,%d\n", p.on, p.x,
				   p.y, p.buttons, p.pressed, p.released, p.dx, p.dy, drag, drag_dx, drag_dy);
	bool ok = true;
	const bool right = (p.on && (p.buttons & 1)) || drag;
	if (p.on || p.released || drag)
	{
		if (p.on || p.released)
		{
			wrword(a6 + A6_MOUSE_ABS_X, p.x);
			wrword(a6 + A6_MOUSE_ABS_Y, p.y);
		}
		/* the click, one press (the game acts on the edge), then what the
		 * buttons are now: a right drag moves the map */
		if (p.pressed)
			ok = update_mouse(0, 0, 2);
		if (ok && (p.pressed || p.released) && !(p.buttons & 2))
			ok = update_mouse(0, 0, 0);
		if (ok && right)
			ok = update_mouse(p.dx + drag_dx, p.dy + drag_dy, 1);
		else if (ok && !p.pressed && !p.released && p.on)
			ok = update_mouse(0, 0, 0);
	}
	for (int i = 0; ok && i < nkeys; i++)
	{
		wrbyte(a6 + A6_SCANCODE_ASCII, keys[i]);
		ok = call(FE2_UIKeyboardPoll);
	}
	wrbyte(a6 + A6_KEY_CURSUP, arrows[0] ? 0xff : 0);
	wrbyte(a6 + A6_KEY_CURSDOWN, arrows[1] ? 0xff : 0);
	wrbyte(a6 + A6_KEY_CURSLEFT, arrows[2] ? 0xff : 0);
	wrbyte(a6 + A6_KEY_CURSRIGHT, arrows[3] ? 0xff : 0);
	return ok;
}

static void no_keys(void)
{
	uint32_t a6 = GAME_A6;
	wrbyte(a6 + A6_SCANCODE_ASCII, 0);
	wrbyte(a6 + A6_PUSHED_ASCII, 0);
	wrbyte(a6 + A6_SCANCODE_ASCII2, 0);
	wrbyte(a6 + A6_KEY_RSHIFT, 0);
	wrbyte(a6 + A6_KEY_CURSDOWN, 0);
	wrbyte(a6 + A6_KEY_CURSLEFT, 0);
	wrbyte(a6 + A6_KEY_CURSRIGHT, 0);
	wrbyte(a6 + A6_KEY_CURSUP, 0);
}

bool mapscreen_page(MapPage *out)
{
	static bool failed;
	uint32_t a6 = GAME_A6;
	if ((uint16_t)rdword(MODULES_FN16) != MODULES_OP)
	{
		if (!failed)
			log_printf("Map screen: the modules' loop is not where expected\n");
		failed = true;
		return false;
	}

	HostScreenState host;
	host_screen_save(&host);
	static HOSTCALL saved[256];
	memcpy(saved, hcalls, (size_t)hcalls_count * sizeof(HOSTCALL));
	fe2vm_game_hcalls(1);
	fe2vm_begin();
	hcalls[HC_DRAW_SCREEN] = page_flip;
	flipped = out;
	got_page = false;

	int step = 0;
	uint8_t state[KEEP_END - KEEP_FIRST], end_state[KEEP_END - KEEP_FIRST];
	for (int i = 0; i < KEEP_END - KEEP_FIRST; i++)
		state[i] = (uint8_t)rdbyte(a6 + KEEP_FIRST + (uint32_t)i);
	bool ok = call(FE2_Clear3DView) && (step = 1, enter());
	if (ok)
	{
		step = 2;
		/* entering resets some of it: as it was, then */
		for (int i = 0; i < KEEP_END - KEEP_FIRST; i++)
			wrbyte(a6 + KEEP_FIRST + (uint32_t)i, state[i]);
		if (kept.held)
			for (int i = 0; i < KEEP2_LEN; i++)
				wrbyte(a6 + KEEP2_FIRST + (uint32_t)i, kept.ctrl[i]);
		no_keys();
		ok = (step = 3, play_input()) && call(FE2_UIKeyboardPoll) && (step = 4, call(MODULES_FN16)) && (step = 5, finish_frame()) &&
			 (step = 6, got_page);
	}
	bool keep = false;
	if (ok)
	{
		int owner = (uint16_t)rdword(a6 + A6_SCREEN_OWNER);
		if (owner == OWNER_GALAXY || owner == OWNER_SYSTEM)
		{
			kept.owner = owner;
			for (int i = 0; i < KEEP_END - KEEP_FIRST; i++)
				end_state[i] = (uint8_t)rdbyte(a6 + KEEP_FIRST + (uint32_t)i);
			for (int i = 0; i < KEEP2_LEN; i++)
				kept.ctrl[i] = (uint8_t)rdbyte(a6 + KEEP2_FIRST + (uint32_t)i);
			kept.held = true;
			keep = true;
		}
	}
	int d_owner = (uint16_t)rdword(a6 + A6_SCREEN_OWNER), d_clr = (uint8_t)rdbyte(a6 + A6_DO_CLR);
	memcpy(hcalls, saved, (size_t)hcalls_count * sizeof(HOSTCALL));
	fe2vm_end();
	fe2vm_game_hcalls(0);
	if (keep)
		for (int i = 0; i < KEEP_END - KEEP_FIRST; i++)
			wrbyte(a6 + KEEP_FIRST + (uint32_t)i, end_state[i]);
	host_screen_restore(&host);
	flipped = NULL;

	if (!ok && !failed)
		log_printf("Map screen: no page (step %d, owner %d, clr %02x: %s)\n", step, d_owner, d_clr, fe2vm_error());
	failed = !ok;
	return ok;
}
