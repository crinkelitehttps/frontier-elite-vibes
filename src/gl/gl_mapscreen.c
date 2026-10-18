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
#include "input.h"
#include "m68000.h"
#include "renderer.h"
#include "screen_text.h"

bool cockpit_map_live = true;
bool cockpit_ship_live = true;

#define IDLE_FPS  2
#define MAP_IDLE_FPS 20 /* the maps move: the orrery turns, the galaxy map scrolls */
#define SHIP_IDLE_FPS 30 /* the ship turns */
#define FOCUS_FPS 50
#define FOCUS_MS  300 /* the screen counts as in use this long after the pointer left it */

#define HC_DRAW_SCREEN 0x6f /* Nu_DrawScreen */
#define HC_IDLE        0x1f /* Call_Idle */

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

/* The ship screen's module (L7ded6): what it keeps of itself, the angles the
 * ship is turned to, the page it will show next (fn 8 shows that page and
 * sets this to the one after) and the page shown. The rest of what it reads
 * (the cargo lists, the money owed) the game changes as it plays, and is
 * the game's live. */
#define A6_MOD_SHIP        124
#define SHIP_ANGLES        11914 /* 2 words */
#define SHIP_NEXT_PAGE     11959 /* byte */
#define SHIP_PAGE          12920 /* byte */
#define SHIP_KEEP_A        11958 /* 3 bytes round the next page */
#define SHIP_KEEP_A_LEN    3
#define A6_FRAME_TIME      14454 /* long: the time the last frame took, 994 for a 50th of a second */
#define VBLANK_TIME        994
#define SHIP_PAGE_ROWS     168	 /* the page's rows above the panel, where a tap away from the buttons turns it */
#define SHIP_ITEM_ID       0x80  /* the icons from this on are the page's items: they do something */
#define SCAN_F3            0x3d
#define HAND_OVER_MS       400

/* L457d8, the modules' fn 16 loop, is 0x7c before L45854 (no label) */
#define MODULES_FN16 (FE2_CallHUDModfuncs - 0x7c)
#define MODULES_OP   0x022e /* its first instruction, andi.b #,598(a6) */

/* Which map is up, and the motion of its right drag (the flight's own mouse
 * control is not the map's, so this is kept here), and what the ship screen
 * keeps */
static struct
{
	bool held;
	int owner;
	uint8_t ctrl[KEEP2_LEN];
} kept;

#define SHIP_STATE_LEN (4 + SHIP_KEEP_A_LEN + 1)
static struct
{
	bool held;
	uint8_t state[SHIP_STATE_LEN]; /* angles, 11958..11960, the page shown */
	int icons;
	int rect[64][4], id[64]; /* the buttons the page set, as the game had them at its end */
	Uint64 last;			 /* the page was last made */
	Uint32 handed;			 /* the real screen was opened (ticks) */
} ship;

/* No vblank comes in a batch: the game's wait for one (L41b98, the ship
 * screen draws after it) has it now */
static void vblank_now(void)
{
	wrword(FE2_vblank_waits, 0);
}

static MapPage *flipped; /* the batch's flip puts the picture here */
static bool got_page;

void mapscreen_forget(void)
{
	kept.held = false;
	ship.held = false;
}

/* =========================================================================
 * The panel's buttons on the MAP screen
 * ========================================================================= */
#define PANEL_ROW0 168
/* The icons of the map's panel that are kept (ids as the game numbers them):
 * the time controls (system map), the map's own (the 2: galaxy or system), and 6..10.
 * The scanner (245), the flight icons and the system map's info box (128)
 * stay in the panel. */
#define ICON_TIME_FIRST 193 /* 193..197 and 254 */
#define ICON_TIME_LAST  197
#define ICON_TIME_PAUSE 254
#define ICON_MAP        242
#define ICON_6          246
#define SCAN_F2         0x3c
/* A press on icon 6 opens the real map and presses it there: when the map is
 * up, a few polls on (its icons are set by then), for a moment at most */
#define HAND_POLLS      200
#define HAND_WAIT       6
static volatile int hand_polls, hand_x, hand_y;
static int hand_up;
#define ICON_10         250
#define BTN_GAP         2
#define BTN_RIGHT  316 /* the buttons' right edge, bottom edge */
#define BTN_BOTTOM 164

static SDL_SpinLock btn_lock;
static MapButton btns[MAPSCREEN_BUTTONS];
static int nbtns;

int mapscreen_buttons(MapButton out[MAPSCREEN_BUTTONS])
{
	SDL_AtomicLock(&btn_lock);
	int n = nbtns;
	memcpy(out, btns, sizeof btns);
	SDL_AtomicUnlock(&btn_lock);
	return n;
}

/* The icons the game has in the panel (rects: x0, y0, x1, y1 inclusive),
 * packed in rows up from the bottom right of the view, in the panel's order */
static bool time_icon(int id)
{
	return (id >= ICON_TIME_FIRST && id <= ICON_TIME_LAST) || id == ICON_TIME_PAUSE;
}

/* the time controls are not overlaid, nor icon 6 on the system map */
static bool kept_icon(int id, int owner)
{
	if (id == ICON_6)
		return owner != OWNER_SYSTEM;
	return id == ICON_MAP || (id > ICON_6 && id <= ICON_10);
}

/* the real map screens (the system information's own buttons are ids below
 * 128): the time controls and flight icons are not wanted */
static bool kept_real(int id)
{
	return id < 128 || id == ICON_MAP || (id >= ICON_6 && id <= ICON_10);
}

static void layout_buttons(int rects[][4], const int ids[], int n, int owner, bool real)
{
	MapButton b[MAPSCREEN_BUTTONS];
	int nb = 0;
	for (int i = 0; i < n && nb < MAPSCREEN_BUTTONS; i++)
	{
		const int *r = rects[i];
		if (!(real ? kept_real(ids[i]) : kept_icon(ids[i], owner)) || r[1] < PANEL_ROW0 || r[3] > 199 || r[0] < 0 || r[2] > 319 || r[2] < r[0] || r[3] < r[1])
			continue;
		MapButton m = {r[0], r[1], r[2] - r[0] + 1, r[3] - r[1] + 1, 0, 0, ids[i]};
		int j = nb++;
		for (; j > 0 && b[j - 1].sx > m.sx; j--)
			b[j] = b[j - 1];
		b[j] = m;
	}
	/* rows, each as wide as fits */
	int row_end[MAPSCREEN_BUTTONS], rows = 0, width = 0;
	for (int i = 0; i < nb; i++)
	{
		if (i > 0 && width + BTN_GAP + b[i].w > BTN_RIGHT - 4)
			row_end[rows++] = i, width = 0;
		width += (width ? BTN_GAP : 0) + b[i].w;
	}
	if (nb)
		row_end[rows++] = nb;
	int y = BTN_BOTTOM;
	for (int r = rows - 1; r >= 0; r--)
	{
		int a = r ? row_end[r - 1] : 0, e = row_end[r], w = 0, h = 0;
		for (int i = a; i < e; i++)
			w += b[i].w + (i > a ? BTN_GAP : 0), h = b[i].h > h ? b[i].h : h;
		int x = BTN_RIGHT - w;
		for (int i = a; i < e; i++)
		{
			b[i].dx = x;
			b[i].dy = y - h + 1 + (h - b[i].h);
			x += b[i].w + BTN_GAP;
		}
		y -= h + BTN_GAP;
	}
	if (getenv("GLF_MAP_LOG"))
		for (int i = 0; i < nb; i++)
			if (b[i].id == ICON_6)
			{
				int hist[256] = {0};
				for (int y = 0; y < b[i].h; y++)
					for (int x = 0; x < b[i].w; x++)
						hist[(uint8_t)VideoRaster[(b[i].sy + y) * 320 + b[i].sx + x]]++;
				char buf[512];
				int len = snprintf(buf, sizeof buf, "icon 6 pixels:");
				for (int v = 0; v < 256 && len < 450; v++)
					if (hist[v])
						len += snprintf(buf + len, sizeof buf - (size_t)len, " %d:%d", v, hist[v]);
				static char last[512];
				if (strcmp(buf, last))
					log_printf("%s (len_main %d)\n", buf, len_main_palette), memcpy(last, buf, sizeof last);
			}
	SDL_AtomicLock(&btn_lock);
	memcpy(btns, b, sizeof b);
	nbtns = nb;
	SDL_AtomicUnlock(&btn_lock);
}

/* The kept icon at a point of the game's panel, 0 if none */
static int button_at(int x, int y)
{
	int id = 0;
	SDL_AtomicLock(&btn_lock);
	for (int i = 0; i < nbtns; i++)
		if (x >= btns[i].sx && x < btns[i].sx + btns[i].w && y >= btns[i].sy && y < btns[i].sy + btns[i].h)
			id = btns[i].id;
	SDL_AtomicUnlock(&btn_lock);
	return id;
}

/* A point on the screen -> where the game has it: a button's pixels are the
 * panel's */
void mapscreen_unmove(int *x, int *y)
{
	SDL_AtomicLock(&btn_lock);
	for (int i = 0; i < nbtns; i++)
		if (*x >= btns[i].dx && *x < btns[i].dx + btns[i].w && *y >= btns[i].dy && *y < btns[i].dy + btns[i].h)
		{
			*x += btns[i].sx - btns[i].dx;
			*y += btns[i].sy - btns[i].dy;
			break;
		}
	SDL_AtomicUnlock(&btn_lock);
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
typedef struct
{
	Pointer ptr[MAPSCREEN_SOURCES];
	bool drag; /* a drag other than a mouse's right button */
	int drag_dx, drag_dy;
	Uint64 seen; /* the pointer was last on the screen */
	unsigned char keys[MAX_KEYS];
	int nkeys;
	bool arrows[4]; /* held: up, down, left, right (a controller's stick) */
	bool grabbed;	/* the stick scrolls the map until a press elsewhere */
} SideInput;
static SideInput ins[COCKPIT_SCREENS];
#define mapin ins[COCKPIT_SCREEN_MAP] /* the map's, for the drag, the stick and the grab */

static void pointer_set(SideInput *in, int group, int source, bool on, int x, int y, int buttons, int dx, int dy)
{
	Pointer *p = &in->ptr[source];
	if (on)
	{
		if ((buttons & 2) && !(p->on && (p->buttons & 2)))
		{
			p->pressed = true;
			if (group == COCKPIT_SCREEN_MAP)
				in->grabbed = true;
		}
		if (!(buttons & 2) && p->on && (p->buttons & 2))
			p->released = true;
		p->dx += dx;
		p->dy += dy;
		p->x = x;
		p->y = y;
		p->buttons = buttons;
		in->seen = SDL_GetPerformanceCounter();
	}
	else if (p->on)
	{
		if (p->buttons & 2)
			p->released = true;
		p->buttons = 0;
	}
	p->on = on;
}

void mapscreen_pointer(int source, int group, int x, int y, int buttons, int dx, int dy)
{
	if (group == COCKPIT_SCREEN_MAP)
		mapscreen_unmove(&x, &y);
	SDL_AtomicLock(&in_lock);
	for (int g = 0; g < COCKPIT_SCREENS; g++)
		if (g == COCKPIT_SCREEN_MAP || g == COCKPIT_SCREEN_SHIP)
			pointer_set(&ins[g], g, source, g == group, x, y, buttons, dx, dy);
	SDL_AtomicUnlock(&in_lock);
}

void mapscreen_drag(bool held, int dx, int dy)
{
	SDL_AtomicLock(&in_lock);
	mapin.drag = held;
	if (held)
	{
		mapin.drag_dx += dx;
		mapin.drag_dy += dy;
		mapin.seen = SDL_GetPerformanceCounter();
	}
	SDL_AtomicUnlock(&in_lock);
}

void mapscreen_key(int group, unsigned char scancode)
{
	SDL_AtomicLock(&in_lock);
	SideInput *i = &ins[group];
	if (i->nkeys < MAX_KEYS)
		i->keys[i->nkeys++] = scancode;
	SDL_AtomicUnlock(&in_lock);
}

void mapscreen_scroll(bool up, bool down, bool left, bool right)
{
	SDL_AtomicLock(&in_lock);
	mapin.arrows[0] = up, mapin.arrows[1] = down, mapin.arrows[2] = left, mapin.arrows[3] = right;
	if (up || down || left || right)
		mapin.seen = SDL_GetPerformanceCounter();
	SDL_AtomicUnlock(&in_lock);
}

void mapscreen_grab(bool on)
{
	SDL_AtomicLock(&in_lock);
	mapin.grabbed = on;
	SDL_AtomicUnlock(&in_lock);
}

bool mapscreen_grabbed(void)
{
	SDL_AtomicLock(&in_lock);
	bool g = mapin.grabbed;
	SDL_AtomicUnlock(&in_lock);
	return g;
}

bool mapscreen_pointed(int group)
{
	SDL_AtomicLock(&in_lock);
	SideInput *i = &ins[group];
	bool used = i->drag || i->nkeys;
	for (int s = 0; s < MAPSCREEN_SOURCES; s++)
		used = used || i->ptr[s].on || i->ptr[s].pressed || i->ptr[s].released;
	used = used ||
		   (i->seen && SDL_GetPerformanceCounter() - i->seen < SDL_GetPerformanceFrequency() * FOCUS_MS / 1000);
	SDL_AtomicUnlock(&in_lock);
	return used;
}

bool mapscreen_handing(void)
{
	return hand_polls > 0;
}

/* (log) what the real game's screen has after a hand-over: its owner and icons, when they change */
static void log_real_screen(void)
{
	static int last_owner = -1;
	static char last[2048];
	int o = game_screen_owner();
	if (game_screen() == SCREEN_FLIGHT || o == OWNER_GALAXY)
	{
		last_owner = -1;
		return;
	}
	int rects[64][4], n = game_icon_rects(rects, 64);
	char buf[2048];
	int len = snprintf(buf, sizeof buf, "real screen (owner %d, screen %d):", o, (int)game_screen());
	for (int i = 0; i < n && len < 1900; i++)
		len += snprintf(buf + len, sizeof buf - (size_t)len, " [id %d %d,%d-%d,%d]",
						(uint16_t)rdword(GAME_A6 + A6_ICON_POSITIONS + (uint32_t)i * 10 + 8), rects[i][0], rects[i][1],
						rects[i][2], rects[i][3]);
	if (o != last_owner || strcmp(buf, last))
		log_printf("%s\n", buf), memcpy(last, buf, sizeof last);
	last_owner = o;
}

/* The game's own map screens (the system information, the hand-over's
 * result) have their buttons in rows the screen does not show: moved as the
 * batch's are */
static volatile bool real_buttons;

bool mapscreen_real_buttons(void)
{
	return real_buttons;
}

static void real_layout(void)
{
	static char last[2048];
	bool real = cockpit_screens_on() && !game_screen_in_batch() &&
				(game_screen() == SCREEN_GALAXY_MAP || game_screen() == SCREEN_SYSTEM_MAP);
	if (!real)
	{
		real_buttons = false;
		return;
	}
	int rects[64][4], ids[64], n = game_icon_rects(rects, 64);
	char buf[2048];
	int len = 0;
	for (int i = 0; i < n; i++)
	{
		ids[i] = (uint16_t)rdword(GAME_A6 + A6_ICON_POSITIONS + (uint32_t)i * 10 + 8);
		if (len < 1900)
			len += snprintf(buf + len, sizeof buf - (size_t)len, "%d,%d,%d,%d,%d;", ids[i], rects[i][0], rects[i][1],
							rects[i][2], rects[i][3]);
	}
	if (!real_buttons || strcmp(buf, last))
	{
		memcpy(last, buf, sizeof last);
		layout_buttons(rects, ids, n, game_screen_owner(), true);
	}
	real_buttons = true;
}

void mapscreen_poll(void)
{
	if (getenv("GLF_MAP_LOG"))
		log_real_screen();
	real_layout();
	if (hand_polls <= 0)
		return;
	hand_polls--;
	if (getenv("GLF_MAP_LOG"))
	{
		static int last_owner = -1;
		int o = game_screen_owner();
		if (o != last_owner)
			log_printf("hand-over: owner %d (screen %d), %d polls left\n", o, (int)game_screen(), hand_polls);
		last_owner = o;
	}
	if (game_screen() != SCREEN_GALAXY_MAP)
	{
		hand_up = 0;
		return;
	}
	/* the game finds what is under the pointer before it is pressed: put it
	 * there first */
	if (hand_up == HAND_WAIT / 2)
		Input_PadPointerAt(hand_x, hand_y);
	if (++hand_up >= HAND_WAIT + 6)
	{
		if (getenv("GLF_MAP_LOG"))
		{
			int rects[64][4], n = game_icon_rects(rects, 64);
			log_printf("hand-over: click at %d,%d, the game's icons:", hand_x, hand_y);
			for (int i = 0; i < n; i++)
				log_printf(" [id %d %d,%d-%d,%d]", (uint16_t)rdword(GAME_A6 + A6_ICON_POSITIONS + (uint32_t)i * 10 + 8),
						   rects[i][0], rects[i][1], rects[i][2], rects[i][3]);
			log_printf("\n");
		}
		Input_ClickAt(hand_x, hand_y);
		hand_polls = 0;
		hand_up = 0;
	}
}

bool mapscreen_up(void)
{
	if (!cockpit_screens_on() || !game_player_ship())
		return false;
	/* in flight, or beside the station's services (docked: the game is on
	 * their screen, the world captured round it) */
	return game_screen() == SCREEN_FLIGHT || game_screen() == SCREEN_STATION;
}

bool mapscreen_group_up(int group)
{
	if (group == COCKPIT_SCREEN_MAP)
		return cockpit_map_live && mapscreen_up();
	/* (not for a moment after it opens the real screen: the key is the game's) */
	if (group == COCKPIT_SCREEN_SHIP)
		return cockpit_ship_live && mapscreen_up() && SDL_GetTicks() - ship.handed >= HAND_OVER_MS;
	return false;
}

int mapscreen_due(void)
{
	static Uint64 last[COCKPIT_SCREENS];
	Uint64 now = SDL_GetPerformanceCounter(), freq = SDL_GetPerformanceFrequency();
	int due = -1;
	Uint64 most = 0;
	for (int g = COCKPIT_SCREEN_MAP; g <= COCKPIT_SCREEN_SHIP; g++)
	{
		if (!mapscreen_group_up(g))
			continue;
		if (!last[g])
		{
			due = g; /* the first page */
			break;
		}
		Uint64 gap = freq / (mapscreen_pointed(g) ? FOCUS_FPS : g == COCKPIT_SCREEN_SHIP ? SHIP_IDLE_FPS : MAP_IDLE_FPS);
		if (now - last[g] >= gap && now - last[g] - gap >= most)
			due = g, most = now - last[g] - gap;
	}
	if (due >= 0)
		last[due] = now;
	return due;
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

/* The galaxy map's wrench (icon 9) draws its menu over the icons once, into
 * the panel: the batch does not keep the panel, so it is drawn again for as
 * long as the menu is up (fn 16's code at the F9 key, with 935 the options) */
#define MENU_FLAG 1075
#define MENU_OPT  935
static void draw_menu(void)
{
	uint32_t a6 = GAME_A6;
	if (!rdbyte(a6 + MENU_FLAG))
		return;
	static const struct
	{
		uint32_t bmp, fn;
		int d0, bit;
	} icons[] = {{FE2_L8a16e, FE2_L83fcc, 0x100, -1},
				 {FE2_L8a2d2, FE2_L83fcc, 0x100, 5},
				 {FE2_L8a32e, FE2_L83fd0, 0x110, 0},
				 {FE2_L8a38a, FE2_L83fd0, 0x120, 3},
				 {FE2_L8a3e6, FE2_L83fd0, 0x130, 6}};
	for (size_t i = 0; i < sizeof icons / sizeof *icons; i++)
	{
		if (icons[i].bit >= 0 && (rdbyte(a6 + MENU_OPT) >> icons[i].bit & 1))
			continue;
		fe2vm_regs r;
		memset(&r, 0, sizeof r);
		r.a[0] = icons[i].bmp;
		r.d[0] = (uint32_t)icons[i].d0;
		fe2vm_call(icons[i].fn, &r);
	}
}

/* The map module's fn 8, as the game's F2 key runs it */
static bool enter_map(void)
{
	uint32_t a6 = GAME_A6;
	int owner = kept.held ? kept.owner : OWNER_GALAXY;
	uint32_t mod = (uint32_t)rdlong(a6 + (owner == OWNER_SYSTEM ? A6_MOD_SYSTEM : A6_MOD_GALAXY));
	/* with the galaxy map shown its fn 8 goes on to the system map */
	wrword(a6 + A6_GALAXY_SHOWN, 0);
	return call(mod + 8);
}

/* The ship screen's fn 8, as F3 runs it: it shows the page set to be next
 * and sets the one after. Put back what it keeps, the page shown (the next
 * page, if F3 is to turn it) as the one to show. */
static bool enter_ship(bool turn)
{
	uint32_t a6 = GAME_A6;
	if (ship.held)
	{
		for (int i = 0; i < 4; i++)
			wrbyte(a6 + SHIP_ANGLES + (uint32_t)i, ship.state[i]);
		for (int i = 0; i < SHIP_KEEP_A_LEN; i++)
			wrbyte(a6 + SHIP_KEEP_A + (uint32_t)i, ship.state[4 + i]);
		wrbyte(a6 + SHIP_PAGE, ship.state[4 + SHIP_KEEP_A_LEN]);
		if (!turn)
			wrbyte(a6 + SHIP_NEXT_PAGE, ship.state[4 + SHIP_KEEP_A_LEN]);
	}
	else
		wrbyte(a6 + SHIP_NEXT_PAGE, 0);
	return call((uint32_t)rdlong(a6 + A6_MOD_SHIP) + 8);
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

/* The input for a batch, since the last */
typedef struct
{
	Pointer p;
	bool drag;
	int drag_dx, drag_dy, nkeys;
	unsigned char keys[MAX_KEYS];
	bool arrows[4];
	bool turn;	   /* the ship screen: F3, or a tap away from its buttons */
	bool handover; /* ... a tap on one of its items */
} Batch;

static void take_input(int group, Batch *b)
{
	memset(b, 0, sizeof *b);
	SDL_AtomicLock(&in_lock);
	SideInput *in = &ins[group];
	/* the pointer that is on the screen, a hand's (the later source) before
	 * the mouse's */
	int src = 0;
	for (int i = 0; i < MAPSCREEN_SOURCES; i++)
		if (in->ptr[i].on || in->ptr[i].released || in->ptr[i].pressed)
			src = i;
	b->p = in->ptr[src];
	for (int i = 0; i < MAPSCREEN_SOURCES; i++)
		in->ptr[i].pressed = in->ptr[i].released = false, in->ptr[i].dx = in->ptr[i].dy = 0;
	b->drag = in->drag;
	b->drag_dx = in->drag_dx;
	b->drag_dy = in->drag_dy;
	in->drag_dx = in->drag_dy = 0;
	b->nkeys = in->nkeys;
	memcpy(b->keys, in->keys, sizeof b->keys);
	in->nkeys = 0;
	memcpy(b->arrows, in->arrows, sizeof b->arrows);
	SDL_AtomicUnlock(&in_lock);

	if (group == COCKPIT_SCREEN_MAP)
	{
		/* What the time controls do is the game's (the batch puts the machine
		 * back): the press goes to its own panel, where they are too. The 2
		 * is F2, as it is for the game's map. Neither reaches the map. */
		static bool swallow;
		if (b->p.pressed)
		{
			int id = button_at(b->p.x, b->p.y);
			if (id == ICON_6 && kept.owner == OWNER_GALAXY)
			{
				/* what it opens is the game's own screen: the real map, and the
				 * button pressed there (mapscreen_poll) */
				swallow = true;
				hand_x = b->p.x, hand_y = b->p.y, hand_polls = HAND_POLLS;
				Input_TapSTKey(SCAN_F2);
			}
			else if (time_icon(id) || id == ICON_MAP)
			{
				swallow = true;
				if (id == ICON_MAP && b->nkeys < MAX_KEYS)
					b->keys[b->nkeys++] = 0x3c;
				else if (time_icon(id) && game_screen() == SCREEN_FLIGHT)
					Input_ClickAt(b->p.x, b->p.y);
			}
		}
		if (!(b->p.on && (b->p.buttons & 2)))
			swallow = false;
		if (swallow)
		{
			b->p.pressed = b->p.released = false;
			b->p.buttons &= ~2;
		}
		return;
	}
	if (group != COCKPIT_SCREEN_SHIP)
		return;
	/* the ship screen: the keys but F3 are not its, a tap on an item opens
	 * the real screen, a tap away from the buttons turns the page, and the
	 * pointer only moves */
	for (int i = 0; i < b->nkeys; i++)
		if (b->keys[i] == SCAN_F3)
			b->turn = true;
	b->nkeys = 0;
	if (b->p.pressed)
	{
		int hit = -1;
		for (int i = 0; i < ship.icons; i++)
			if (b->p.x >= ship.rect[i][0] && b->p.x <= ship.rect[i][2] && b->p.y >= ship.rect[i][1] &&
				b->p.y <= ship.rect[i][3])
				hit = i;
		if (hit >= 0)
			b->handover = ship.id[hit] >= SHIP_ITEM_ID;
		else
			b->turn = b->turn || b->p.y < SHIP_PAGE_ROWS;
	}
	b->p.pressed = b->p.released = false;
	b->p.buttons &= ~2;
}

/* The input, as the game would have had it */
static bool play_input(const Batch *b)
{
	uint32_t a6 = GAME_A6;
	const Pointer *p = &b->p;
	if (getenv("GLF_MAP_LOG") && (p->pressed || p->released || p->buttons || b->drag))
		log_printf("map input: on %d at %d,%d buttons %d pressed %d released %d d %d,%d drag %d %d,%d\n", p->on, p->x,
				   p->y, p->buttons, p->pressed, p->released, p->dx, p->dy, b->drag, b->drag_dx, b->drag_dy);
	bool ok = true;
	const bool right = (p->on && (p->buttons & 1)) || b->drag;
	if (p->on || p->released || b->drag)
	{
		if (p->on || p->released)
		{
			wrword(a6 + A6_MOUSE_ABS_X, p->x);
			wrword(a6 + A6_MOUSE_ABS_Y, p->y);
		}
		/* the click, one press (the game acts on the edge), then what the
		 * buttons are now: a right drag moves the map */
		if (p->pressed)
			ok = update_mouse(0, 0, 2);
		if (ok && (p->pressed || p->released) && !(p->buttons & 2))
			ok = update_mouse(0, 0, 0);
		if (ok && right)
			ok = update_mouse(p->dx + b->drag_dx, p->dy + b->drag_dy, 1);
		else if (ok && !p->pressed && !p->released && p->on && !(p->buttons & 2))
			ok = update_mouse(0, 0, p->buttons & 2); /* (a held left button stays held: the zoom icons repeat) */
	}
	for (int i = 0; ok && i < b->nkeys; i++)
	{
		wrbyte(a6 + A6_SCANCODE_ASCII, b->keys[i]);
		ok = call(FE2_UIKeyboardPoll);
	}
	wrbyte(a6 + A6_KEY_CURSUP, b->arrows[0] ? 0xff : 0);
	wrbyte(a6 + A6_KEY_CURSDOWN, b->arrows[1] ? 0xff : 0);
	wrbyte(a6 + A6_KEY_CURSLEFT, b->arrows[2] ? 0xff : 0);
	wrbyte(a6 + A6_KEY_CURSRIGHT, b->arrows[3] ? 0xff : 0);
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

/* The ship screen's page ran: what it keeps, and its buttons */
static void ship_take(void)
{
	uint32_t a6 = GAME_A6;
	for (int i = 0; i < 4; i++)
		ship.state[i] = (uint8_t)rdbyte(a6 + SHIP_ANGLES + (uint32_t)i);
	for (int i = 0; i < SHIP_KEEP_A_LEN; i++)
		ship.state[4 + i] = (uint8_t)rdbyte(a6 + SHIP_KEEP_A + (uint32_t)i);
	ship.state[4 + SHIP_KEEP_A_LEN] = (uint8_t)rdbyte(a6 + SHIP_PAGE);
	int rects[64][4];
	ship.icons = game_icon_rects(rects, 64);
	memcpy(ship.rect, rects, sizeof ship.rect);
	for (int i = 0; i < ship.icons; i++)
		ship.id[i] = (uint16_t)rdword(a6 + A6_ICON_POSITIONS + (uint32_t)i * 10 + 8);
	ship.held = true;
}

/* The real ship screen on the page shown (a tap on an item: what it does is
 * the game's to do) */
static void ship_hand_over(void)
{
	uint32_t a6 = GAME_A6;
	wrbyte(a6 + SHIP_NEXT_PAGE, ship.state[4 + SHIP_KEEP_A_LEN]);
	ship.handed = SDL_GetTicks();
	Input_TapSTKey(SCAN_F3);
}

bool mapscreen_page(int group, MapPage *out)
{
	static bool failed[COCKPIT_SCREENS];
	const bool is_ship = group == COCKPIT_SCREEN_SHIP;
	uint32_t a6 = GAME_A6;
	if ((uint16_t)rdword(MODULES_FN16) != MODULES_OP)
	{
		if (!failed[group])
			log_printf("Map screen: the modules' loop is not where expected\n");
		failed[group] = true;
		return false;
	}

	Batch input;
	take_input(group, &input);

	game_screen_freeze(true);
	scene_mute_object(0); /* (the frame before may have left one: its stack is above the batch's) */
	HostScreenState host;
	host_screen_save(&host);
	static HOSTCALL saved[256];
	memcpy(saved, hcalls, (size_t)hcalls_count * sizeof(HOSTCALL));
	fe2vm_game_hcalls(1);
	fe2vm_begin();
	hcalls[HC_DRAW_SCREEN] = page_flip;
	hcalls[HC_IDLE] = vblank_now;
	flipped = out;
	got_page = false;

	int step = 0;
	uint8_t state[KEEP_END - KEEP_FIRST], end_state[KEEP_END - KEEP_FIRST];
	if (!is_ship)
		for (int i = 0; i < KEEP_END - KEEP_FIRST; i++)
			state[i] = (uint8_t)rdbyte(a6 + KEEP_FIRST + (uint32_t)i);
	bool ok = call(FE2_Clear3DView) && (step = 1, is_ship ? enter_ship(input.turn) : enter_map());
	if (ok)
	{
		step = 2;
		if (!is_ship)
		{
			/* entering resets some of it: as it was, then */
			for (int i = 0; i < KEEP_END - KEEP_FIRST; i++)
				wrbyte(a6 + KEEP_FIRST + (uint32_t)i, state[i]);
			if (kept.held)
				for (int i = 0; i < KEEP2_LEN; i++)
					wrbyte(a6 + KEEP2_FIRST + (uint32_t)i, kept.ctrl[i]);
		}
		else
		{
			/* the ship turns by the time that has passed, as the frames
			 * the game has not run would have */
			Uint64 now = SDL_GetPerformanceCounter(), freq = SDL_GetPerformanceFrequency();
			Uint64 vblanks = ship.last ? (now - ship.last) * 50 / freq : 1;
			if (vblanks < 1)
				vblanks = 1;
			if (vblanks > 100)
				vblanks = 100;
			ship.last = now;
			wrlong(a6 + A6_FRAME_TIME, (uint32_t)(VBLANK_TIME * vblanks));
		}
		if (!is_ship && kept.owner == OWNER_GALAXY)
			draw_menu();
		no_keys();
		ok = (step = 3, play_input(&input)) && call(FE2_UIKeyboardPoll) && (step = 4, call(MODULES_FN16)) &&
			 (step = 5, finish_frame()) && (step = 6, got_page);
	}
	bool keep = false;
	if (ok)
	{
		int owner = (uint16_t)rdword(a6 + A6_SCREEN_OWNER);
		if (is_ship)
		{
			if (owner == OWNER_SHIP_SCREEN)
				ship_take();
		}
		else if (owner == OWNER_GALAXY || owner == OWNER_SYSTEM)
		{
			kept.owner = owner;
			for (int i = 0; i < KEEP_END - KEEP_FIRST; i++)
				end_state[i] = (uint8_t)rdbyte(a6 + KEEP_FIRST + (uint32_t)i);
			for (int i = 0; i < KEEP2_LEN; i++)
				kept.ctrl[i] = (uint8_t)rdbyte(a6 + KEEP2_FIRST + (uint32_t)i);
			kept.held = true;
			keep = true;
			int rects[64][4];
			int n = game_icon_rects(rects, 64), ids[64];
			for (int i = 0; i < n; i++)
				ids[i] = (uint16_t)rdword(a6 + A6_ICON_POSITIONS + (uint32_t)i * 10 + 8);
			if (getenv("GLF_MAP_LOG"))
			{
				static char last[2048];
				char buf[2048];
				int len = snprintf(buf, sizeof buf, "map icons (owner %d):", owner);
				for (int i = 0; i < n && len < 1900; i++)
					len += snprintf(buf + len, sizeof buf - (size_t)len, " [id %d %d,%d-%d,%d]",
									(uint16_t)rdword(a6 + A6_ICON_POSITIONS + (uint32_t)i * 10 + 8), rects[i][0],
									rects[i][1], rects[i][2], rects[i][3]);
				if (strcmp(buf, last))
					log_printf("%s\n", buf), memcpy(last, buf, sizeof last);
			}
			layout_buttons(rects, ids, n, owner, false);
		}
	}
	if (!is_ship && getenv("GLF_MAP_LOG"))
	{
		int now = (uint16_t)rdword(a6 + A6_SCREEN_OWNER);
		static int last_owner = -1;
		if (now != last_owner || input.nkeys || input.p.pressed || input.p.released)
			log_printf("map batch: entered %d, ended %d (ok %d), keys %d pressed %d released %d at %d,%d\n",
					   kept.held ? kept.owner : OWNER_GALAXY, now, ok, input.nkeys, input.p.pressed, input.p.released,
					   input.p.x, input.p.y);
		last_owner = now;
	}
	int d_owner = (uint16_t)rdword(a6 + A6_SCREEN_OWNER), d_clr = (uint8_t)rdbyte(a6 + A6_DO_CLR);
	memcpy(hcalls, saved, (size_t)hcalls_count * sizeof(HOSTCALL));
	fe2vm_end();
	fe2vm_game_hcalls(0);
	if (keep)
		for (int i = 0; i < KEEP_END - KEEP_FIRST; i++)
			wrbyte(a6 + KEEP_FIRST + (uint32_t)i, end_state[i]);
	host_screen_restore(&host);
	game_screen_freeze(false);
	flipped = NULL;
	if (ok && is_ship && input.handover)
		ship_hand_over();

	if (!ok && !failed[group])
		log_printf("%s screen: no page (step %d, owner %d, clr %02x: %s)\n", is_ship ? "Ship" : "Map", step, d_owner,
				   d_clr, fe2vm_error());
	failed[group] = !ok;
	return ok;
}
