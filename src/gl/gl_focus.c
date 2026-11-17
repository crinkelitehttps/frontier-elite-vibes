/* gl_focus.c - keyboard / pad focus on the cockpit's input surfaces (gl_focus.h) */

#include <math.h>
#include <string.h>

#include <SDL.h>

#include "gl_focus.h"

#include "gl_camera.h"
#include "gl_cockpit.h"
#include "gl_hud.h"
#include "gl_mapscreen.h"

#include "ui/ui.h"

#include "freecam.h"
#include "game_state.h"
#include "host.h" /* rdword */
#include "input.h"

#define ROW0 COCKPIT_PANEL_ROW0
#define MAX_BTNS 96
#define PRESS_MS 150
#define LET_GO_MS 100

/* The surfaces (Tab / Y goes round them clockwise as they are placed now, step) */
enum
{
	S_DASH,
	S_COMS,
	S_MAP,
	S_SHIP,
	S_CAMERA,
	S_COUNT
};
static const int GROUP_OF[S_COUNT] = {-1, COCKPIT_SCREEN_COMS, COCKPIT_SCREEN_MAP, COCKPIT_SCREEN_SHIP, -1};

/* ids of the host's own buttons */
enum
{
	ID_ORBIT = -1,
	ID_COG = -2,
	ID_WHOLE = -3, /* a dimmed screen, whole */
	ID_PAGE = -4,  /* the SHIP screen's next page */
	ID_PAN = -10,  /* - the pan button's number */
	ID_PICK = -100 /* - the object slot of a body on the system map or a thing in the camera's picture */
};
#define PICK_MAX 128

#define PICK_R 4 /* the box round a body's point */

#define GROUP_KEY_SHIP 0x3d /* F3 */

typedef struct
{
	int x, y, w, h; /* where it is drawn, page pixels */
	int id;
	int ax, ay; /* where it is pressed (the surface's own coordinates) */
} Btn;

static int cur = S_DASH;
static int sel_id[S_COUNT], sel_cx[S_COUNT], sel_cy[S_COUNT];
static bool sel_set[S_COUNT];

static SDL_SpinLock view_lock;
static FocusView view;
static bool view_ok;

/* A press on a side screen's pointer, to be let go */
static struct
{
	int phase; /* 0 none, 1 held, 2 let go, pointer to be taken off */
	bool held; /* the key / button is still down: phase 1 lasts till it isn't */
	int group, x, y;
	Uint32 at;
} press;
/* The game's own pointer held (Input_HoldAt) */
static bool game_held;

static bool surface_shown(int s)
{
	if (s == S_DASH)
		return true;
	if (s == S_CAMERA)
		return cockpit_focus_camera();
	return cockpit_focus_mode(GROUP_OF[s]) != COCKPIT_FOCUS_NONE;
}

static int quad_of(int s)
{
	return s == S_DASH ? 0 : s == S_CAMERA ? 1 + COCKPIT_SCREENS : 1 + GROUP_OF[s];
}

bool focus_enabled(void)
{
	return cockpit_focus_active() && !freecam_active();
}

static int add(Btn *b, int n, int x, int y, int w, int h, int id, int ax, int ay)
{
	if (n < MAX_BTNS)
		b[n] = (Btn){x, y, w, h, id, ax, ay};
	return n < MAX_BTNS ? n + 1 : n;
}

static int game_icons(int rects[][4], int ids[], int max)
{
	int n = game_icon_rects(rects, max);
	for (int i = 0; i < n; i++)
		ids[i] = (uint16_t)rdword(GAME_A6 + A6_ICON_POSITIONS + (uint32_t)i * 10 + 8);
	return n;
}

/* Things to make the target (x, y, slot): the box round each, by its slot, so it stays on one as it moves */
static int add_targets(Btn *b, int n, int picks[][3], int c)
{
	for (int i = 0; i < c; i++)
		n = add(b, n, picks[i][0] - PICK_R, picks[i][1] - PICK_R, 2 * PICK_R + 1, 2 * PICK_R + 1, ID_PICK - picks[i][2],
				picks[i][0], picks[i][1]);
	return n;
}

static int add_picks(Btn *b, int n, bool live)
{
	int picks[MAPSCREEN_PICKS][3];
	return add_targets(b, n, picks, mapscreen_picks(live, picks));
}

static int add_pans(Btn *b, int n)
{
	int pans[MAPSCREEN_PANS][4];
	int c = mapscreen_pans(pans);
	for (int i = 0; i < c; i++)
		n = add(b, n, pans[i][0], pans[i][1], pans[i][2], pans[i][3], ID_PAN - i, pans[i][0] + pans[i][2] / 2,
				pans[i][1] + pans[i][3] / 2);
	return n;
}

static int collect(int s, Btn *b, int *mode_out)
{
	int n = 0, rects[64][4], ids[64];
	int mode = COCKPIT_FOCUS_NONE;
	if (s == S_DASH)
	{
		int c = game_icons(rects, ids, 64);
		for (int i = 0; i < c; i++)
			if (rects[i][1] >= ROW0 && rects[i][3] < 200 && rects[i][2] >= rects[i][0])
				n = add(b, n, rects[i][0], rects[i][1], rects[i][2] - rects[i][0] + 1, rects[i][3] - rects[i][1] + 1, ids[i],
						(rects[i][0] + rects[i][2]) / 2, (rects[i][1] + rects[i][3]) / 2);
		if (hud_orbit_button_shown())
		{
			int h = ORBIT_BTN_Y1 - ORBIT_BTN_Y0 + 1;
			n = add(b, n, ORBIT_BTN_X0, ORBIT_BTN_Y0, ORBIT_BTN_X1 - ORBIT_BTN_X0 + 1, h, ID_ORBIT,
					(ORBIT_BTN_X0 + ORBIT_BTN_X1) / 2, (ORBIT_BTN_Y0 + ORBIT_BTN_Y1) / 2);
			n = add(b, n, COG_BTN_X0, ORBIT_BTN_Y0, COG_BTN_X1 - COG_BTN_X0 + 1, h, ID_COG, (COG_BTN_X0 + COG_BTN_X1) / 2,
					(ORBIT_BTN_Y0 + ORBIT_BTN_Y1) / 2);
		}
		*mode_out = COCKPIT_FOCUS_LIVE;
		return n;
	}
	if (s == S_CAMERA)
	{
		CameraButton cb[CAMERA_BUTTONS];
		int c = camera_buttons(cb);
		for (int i = 0; i < c; i++)
			n = add(b, n, cb[i].x, cb[i].y, cb[i].w, cb[i].h, cb[i].id, cb[i].x + cb[i].w / 2, cb[i].y + cb[i].h / 2);
		int picks[PICK_MAX][3];
		n = add_targets(b, n, picks, camera_targets(picks, PICK_MAX));
		*mode_out = COCKPIT_FOCUS_LIVE;
		return n;
	}
	const int g = GROUP_OF[s];
	mode = cockpit_focus_mode(g);
	*mode_out = mode;
	if (mode == COCKPIT_FOCUS_HOST)
		return add(b, n, 4, 4, 312, ROW0 - 8, ID_WHOLE, 160, ROW0 / 2);
	if (mode == COCKPIT_FOCUS_SIDE)
	{
		if (g == COCKPIT_SCREEN_MAP)
		{
			MapButton mb[MAPSCREEN_BUTTONS];
			int c = mapscreen_buttons(mb);
			for (int i = 0; i < c; i++)
				n = add(b, n, mb[i].dx, mb[i].dy, mb[i].w, mb[i].h, mb[i].id, mb[i].dx + mb[i].w / 2, mb[i].dy + mb[i].h / 2);
			n = add_pans(b, n);
			n = add_picks(b, n, false);
		}
		else
		{
			MapButton mb[MAPSCREEN_SHIP_BUTTONS];
			int c = mapscreen_ship_buttons(mb);
			for (int i = 0; i < c; i++)
				n = add(b, n, mb[i].dx, mb[i].dy, mb[i].w, mb[i].h, mb[i].id, mb[i].dx + mb[i].w / 2, mb[i].dy + mb[i].h / 2);
			n = add(b, n, 148, 2, 24, 12, ID_PAGE, 160, 8);
		}
		return n;
	}
	/* the game's own screen, or the flight comms menu: its icons on the page (rows 0 .. 168), and the panel's
	 * that the host moved onto it */
	int c = game_icons(rects, ids, 64);
	MapButton moved[MAPSCREEN_BUTTONS], phone;
	int nm = 0;
	bool has_phone = false;
	if (mode == COCKPIT_FOCUS_LIVE && g == COCKPIT_SCREEN_MAP && mapscreen_real_buttons())
		nm = mapscreen_buttons(moved);
	if (mode == COCKPIT_FOCUS_LIVE && g == COCKPIT_SCREEN_COMS)
		has_phone = mapscreen_phone(&phone);
	for (int i = 0; i < c; i++)
	{
		const int *r = rects[i];
		if (r[2] < r[0] || r[3] < r[1] || r[0] < 0)
			continue;
		int x = r[0], y = r[1];
		if (r[1] >= ROW0)
		{
			bool found = false;
			for (int k = 0; k < nm && !found; k++)
				if (moved[k].sx == r[0] && moved[k].sy == r[1])
					x = moved[k].dx, y = moved[k].dy, found = true;
			if (has_phone && phone.sx == r[0] && phone.sy == r[1])
				x = phone.dx, y = phone.dy, found = true;
			if (!found)
				continue;
		}
		n = add(b, n, x, y, r[2] - r[0] + 1, r[3] - r[1] + 1, ids[i], (r[0] + r[2]) / 2, (r[1] + r[3]) / 2);
	}
	if (mode == COCKPIT_FOCUS_LIVE && g == COCKPIT_SCREEN_SHIP)
		n = add(b, n, 148, 2, 24, 12, ID_PAGE, 160, 8);
	if (mode == COCKPIT_FOCUS_LIVE && g == COCKPIT_SCREEN_MAP && nm)
	{
		n = add_pans(b, n);
		n = add_picks(b, n, true);
	}
	return n;
}

static int cx_of(const Btn *b)
{
	return b->x * 2 + b->w; /* twice the centre */
}

static int cy_of(const Btn *b)
{
	return b->y * 2 + b->h;
}

/* The box's button in the list: the one with the remembered number nearest where it was, else
 * the one nearest where it was, else the first */
static int resolve(int s, const Btn *b, int n)
{
	if (n <= 0)
		return -1;
	int best = 0;
	if (sel_set[s])
	{
		long best_d = -1;
		for (int i = 0; i < n; i++)
		{
			long dx = cx_of(&b[i]) - sel_cx[s], dy = cy_of(&b[i]) - sel_cy[s];
			long d = dx * dx + dy * dy + (b[i].id == sel_id[s] ? 0 : 1000000L);
			if (best_d < 0 || d < best_d)
				best_d = d, best = i;
		}
	}
	sel_set[s] = true;
	sel_id[s] = b[best].id;
	sel_cx[s] = cx_of(&b[best]);
	sel_cy[s] = cy_of(&b[best]);
	return best;
}

static void publish(int s, const Btn *b, int n, int pick)
{
	FocusView v = {quad_of(s), pick >= 0, 0, 0, 0, 0};
	if (pick >= 0 && pick < n)
		v.x = b[pick].x, v.y = b[pick].y, v.w = b[pick].w, v.h = b[pick].h;
	else
		v.has_box = false;
	SDL_AtomicLock(&view_lock);
	view = v;
	view_ok = true;
	SDL_AtomicUnlock(&view_lock);
}

bool focus_view(FocusView *out)
{
	SDL_AtomicLock(&view_lock);
	bool ok = view_ok;
	*out = view;
	SDL_AtomicUnlock(&view_lock);
	return ok;
}

/* Keeps the focus on a shown surface; then its list and the box's button */
static int refresh(Btn *b, int *n, int *mode)
{
	if (!surface_shown(cur))
		cur = S_DASH;
	*n = collect(cur, b, mode);
	if (*n > MAX_BTNS)
		*n = MAX_BTNS;
	int pick = resolve(cur, b, *n);
	publish(cur, b, *n, pick);
	return pick;
}

/* Where a surface's middle is seen from the eye (view space, the eye at the origin) */
static bool surface_centre(int s, float out[3])
{
	return cockpit_focus_point(quad_of(s), 160.0f, s == S_DASH ? (ROW0 + 200) * 0.5f : ROW0 * 0.5f, out);
}

/* The next shown surface clockwise as the eye sees them (dir 1), or anticlockwise (-1): their directions from the
 * eye put on a plane facing the middle of them all, and taken in turn by the angle round that middle. Worked out
 * at each step, so it follows the screens wherever they are placed. */
static void step(int dir)
{
	int ss[S_COUNT], n = 0;
	float d[S_COUNT][3], m[3] = {0, 0, 0};
	for (int s = 0; s < S_COUNT; s++)
	{
		float c[3];
		if (!surface_shown(s) || !surface_centre(s, c))
			continue;
		float len = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
		if (len < 1e-6f)
			continue;
		for (int k = 0; k < 3; k++)
			d[n][k] = c[k] / len, m[k] += d[n][k];
		ss[n++] = s;
	}
	if (n == 0)
	{
		cur = S_DASH;
	}
	else
	{
		float ml = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
		if (ml < 1e-6f)
			m[0] = 0, m[1] = 0, m[2] = -1, ml = 1;
		for (int k = 0; k < 3; k++)
			m[k] /= ml;
		/* right = m x up, up' = right x m (y up, looking down -z) */
		float r[3] = {-m[2], 0.0f, m[0]};
		float rl = sqrtf(r[0] * r[0] + r[2] * r[2]);
		if (rl < 1e-6f)
			r[0] = 1, r[2] = 0, rl = 1;
		r[0] /= rl, r[2] /= rl;
		float u[3] = {r[1] * m[2] - r[2] * m[1], r[2] * m[0] - r[0] * m[2], r[0] * m[1] - r[1] * m[0]};
		float ang[S_COUNT], cu = 0, cv = 0, pu[S_COUNT], pv[S_COUNT];
		for (int i = 0; i < n; i++)
		{
			float z = d[i][0] * m[0] + d[i][1] * m[1] + d[i][2] * m[2];
			if (z < 0.05f)
				z = 0.05f; /* (behind: kept on its side) */
			pu[i] = (d[i][0] * r[0] + d[i][1] * r[1] + d[i][2] * r[2]) / z;
			pv[i] = (d[i][0] * u[0] + d[i][1] * u[1] + d[i][2] * u[2]) / z;
			cu += pu[i] / (float)n, cv += pv[i] / (float)n;
		}
		/* clockwise to the eye (right and up): the angle going down */
		for (int i = 0; i < n; i++)
			ang[i] = -atan2f(pv[i] - cv, pu[i] - cu);
		int at = -1;
		for (int i = 0; i < n; i++)
			if (ss[i] == cur)
				at = i;
		if (at < 0)
			cur = ss[0];
		else
		{
			/* the nearest angle on that way round */
			int best = at;
			float best_a = 0;
			for (int i = 0; i < n; i++)
			{
				if (i == at)
					continue;
				float a = (ang[i] - ang[at]) * (float)dir;
				while (a <= 0)
					a += 6.2831853f;
				if (best == at || a < best_a)
					best = i, best_a = a;
			}
			cur = ss[best];
		}
	}
	Btn b[MAX_BTNS];
	int nb, mode;
	refresh(b, &nb, &mode);
}

/* The emulator menu (the cog) is up: the keys and pad actions move its own green box */
void focus_next(void)
{
	if (ui_menu_open())
	{
		ui_nav_tab(false);
		return;
	}
	if (focus_enabled())
		step(1);
}

void focus_prev(void)
{
	if (ui_menu_open())
	{
		ui_nav_tab(true);
		return;
	}
	if (focus_enabled())
		step(-1);
}

void focus_move(int dx, int dy)
{
	if (ui_menu_open())
	{
		ui_nav_move(dx, dy);
		return;
	}
	if (!focus_enabled())
		return;
	Btn b[MAX_BTNS];
	int n, mode;
	int pick = refresh(b, &n, &mode);
	if (pick < 0)
		return;
	const int px = cx_of(&b[pick]), py = cy_of(&b[pick]);
	int best = -1;
	float best_score = 0;
	for (int i = 0; i < n; i++)
	{
		if (i == pick)
			continue;
		float vx = (float)(cx_of(&b[i]) - px), vy = (float)(cy_of(&b[i]) - py);
		float along = vx * (float)dx + vy * (float)dy, across = fabsf(vx * (float)dy - vy * (float)dx);
		/* within 60 degrees of the direction */
		if (along <= 0.0f || across > along * 1.732f)
			continue;
		float score = along + 2.0f * across;
		if (best < 0 || score < best_score)
			best = i, best_score = score;
	}
	if (best < 0)
		return;
	sel_id[cur] = b[best].id;
	sel_cx[cur] = cx_of(&b[best]);
	sel_cy[cur] = cy_of(&b[best]);
	publish(cur, b, n, best);
}

void focus_press(bool down)
{
	if (!down)
	{
		if (game_held)
			Input_HoldAt(0, 0, false);
		game_held = false;
		press.held = false;
		return;
	}
	if (ui_menu_open())
	{
		ui_nav_activate();
		return;
	}
	if (!focus_enabled() || game_held || press.phase)
		return;
	Btn b[MAX_BTNS];
	int n, mode;
	int pick = refresh(b, &n, &mode);
	if (pick < 0)
		return;
	const Btn *p = &b[pick];
	/* a body on the system map, a thing in the camera's picture: the target (the map's own click would only pick
	 * it in its F10 mode) */
	if (p->id <= ID_PICK)
	{
		mapscreen_target(ID_PICK - p->id);
		return;
	}
	if (cur == S_CAMERA)
	{
		camera_tap((float)p->ax / 320.0f, (float)p->ay / (float)ROW0);
		return;
	}
	if (cur == S_DASH)
	{
		game_held = Input_HoldAt(p->ax, p->ay, true);
		return;
	}
	const int g = GROUP_OF[cur];
	switch (mode)
	{
	case COCKPIT_FOCUS_HOST:
		cockpit_host_tap(g, 0.5f, 0.5f);
		break;
	case COCKPIT_FOCUS_SIDE:
		if (p->id == ID_PAGE)
			mapscreen_key(g, GROUP_KEY_SHIP);
		else
		{
			mapscreen_pointer(2, g, p->ax, p->ay, 2, 0, 0);
			press.phase = 1, press.held = true, press.group = g, press.x = p->ax, press.y = p->ay;
			press.at = SDL_GetTicks() + PRESS_MS;
		}
		break;
	default: /* the game's own screen, the comms menu */
		if (p->id == ID_PAGE)
			Input_TapSTKey(GROUP_KEY_SHIP);
		else
			game_held = Input_HoldAt(p->ax, p->ay, true);
		break;
	}
}

void focus_tick(void)
{
	ui_nav_tick();
	if (press.phase && !(press.phase == 1 && press.held) && (Sint32)(SDL_GetTicks() - press.at) >= 0)
	{
		if (press.phase == 1)
		{
			mapscreen_pointer(2, press.group, press.x, press.y, 0, 0, 0);
			press.phase = 2;
			press.at = SDL_GetTicks() + LET_GO_MS;
		}
		else
		{
			mapscreen_pointer(2, -1, 0, 0, 0, 0, 0);
			press.phase = 0;
		}
	}
	if (!focus_enabled() || ui_menu_open()) /* the menu has its own box */
	{
		SDL_AtomicLock(&view_lock);
		view_ok = false;
		SDL_AtomicUnlock(&view_lock);
		return;
	}
	Btn b[MAX_BTNS];
	int n, mode;
	refresh(b, &n, &mode);
}

/* Keys the focus takes: held ones are tracked so a release never reaches the game unpaired */
bool focus_key_event(const SDL_Event *event)
{
	static const SDL_Keycode KEYS[6] = {SDLK_TAB, SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT, SDLK_f};
	static bool held[6];
	if (event->type != SDL_KEYDOWN && event->type != SDL_KEYUP)
		return false;
	int k = -1;
	for (int i = 0; i < 6; i++)
		if (event->key.keysym.sym == KEYS[i])
			k = i;
	if (k < 0)
		return false;
	if (event->type == SDL_KEYUP)
	{
		bool was = held[k];
		held[k] = false;
		if (was && k == 5)
			focus_press(false);
		return was;
	}
	const Uint16 mod = event->key.keysym.mod;
	if (mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI))
		return false;
	if (k != 0 && (mod & KMOD_SHIFT))
		return false;
	/* the options screen types file names */
	if (!focus_enabled() || game_screen() == SCREEN_OPTIONS)
		return false;
	held[k] = true;
	if (event->key.repeat && (k == 0 || k == 5))
		return true;
	switch (k)
	{
	case 0:
		if (mod & KMOD_SHIFT)
			focus_prev();
		else
			focus_next();
		break;
	case 1:
		focus_move(0, -1);
		break;
	case 2:
		focus_move(0, 1);
		break;
	case 3:
		focus_move(-1, 0);
		break;
	case 4:
		focus_move(1, 0);
		break;
	case 5:
		focus_press(true);
		break;
	}
	return true;
}
