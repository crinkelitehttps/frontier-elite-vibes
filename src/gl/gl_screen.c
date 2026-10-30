/*
 * gl_screen.c - OpenGL 3.3 / GLES 3.0 front end of the renderer.
 *
 * Owns the window and GL context, the letterboxed layout, the 320x200
 * emulated screen (2D UI, text, control panel), the hand over of the frames
 * the game flips (Nu_DrawScreen) to the render thread that draws them (see
 * "Frames" below) and what is drawn per picture. The 3D view itself is
 * recorded by gl_scene / gl_prims / gl_planet and drawn through the gl_draw
 * batch.
 *
 * Frame layout (bottom to top):
 *   1. 3D view        scene_draw() into GD_VP_VIEW3D
 *   2. screen blit    emulated 320x200 framebuffer, colour 255 transparent
 *                     (in the cockpit view: HUD, cockpit and panel, gl_cockpit.c)
 *   3. overlays       touch controls / settings cog
 *   4. emulator menu  Dear ImGui, drawn after the batch is flushed
 *
 * With a VR headset (vr.h) the frame is drawn for it first: the cockpit
 * once per eye, anything else on its virtual screen. The window follows,
 * as without one (the cockpit turned by the headset).
 */
#include <stdlib.h>

#include <SDL.h>

#include "gl_api.h"
#include "gl_camera.h"
#include "gl_clipboard.h"
#include "gl_cockpit.h"
#include "gl_native.h"
#include "gl_draw.h"
#include "gl_hud.h"
#include "gl_laser.h"
#include "gl_mapscreen.h"
#include "gl_overlay.h"
#include "gl_planet.h"
#include "gl_prims.h"
#include "gl_scene.h"

#include "frame_prof.h"
#include "game_state.h"
#include "input.h"
#include "main.h"
#include "m68000.h"
#include "netplay.h"
#include "renderer.h"
#include "quiz_buttons.h"
#include "screen_text.h"
#include "touch_input.h"
#include "ui/ui.h"
#include "vr.h"

/* =========================================================================
 * Public globals (declared extern in renderer.h)
 * ========================================================================= */
unsigned long VideoBase;
unsigned char *VideoRaster;

int len_main_palette;
unsigned short MainPalette[256];
unsigned short CtrlPalette[16];
int fe2_bgcol;

unsigned int MainRGBPalette[256];
unsigned int CtrlRGBPalette[16];

unsigned long logscreen, logscreen2, physcreen, physcreen2;

BOOL bGrabMouse = FALSE;
BOOL bInFullScreen = FALSE;

enum RENDERERS use_renderer = R_GL;
int mouse_shown = 0;

int screen_w = 640;
int screen_h = 480;

/* =========================================================================
 * Layout / letterboxing
 * ========================================================================= */
#define GAME_W      320
#define GAME_H      240
#define SCR_W       GL_SCREEN_W /* emulated framebuffer */
#define SCR_H       GL_SCREEN_H
#define SCR_TEX_W   GL_SCREEN_TEX_W
#define SCR_TEX_H   GL_SCREEN_TEX_H
#define PANEL_LINES 38 /* control panel height in the 240 line game area */

static int lb_x, lb_y, lb_w = 640, lb_h = 480;
static bool lb_mode = true; /* keep 4:3 aspect */

/* The game area (x, y, w, h) in a w x h framebuffer */
static void letterbox(int w, int h, int r[4])
{
	r[0] = r[1] = 0;
	r[2] = w;
	r[3] = h;
	if (!lb_mode)
		return;
	float aspect = (float)GAME_W / (float)GAME_H;
	r[3] = (int)(w / aspect + 0.5f);
	if (r[3] > h)
	{
		r[3] = h;
		r[2] = (int)(h * aspect + 0.5f);
	}
	r[0] = (w - r[2]) / 2;
	r[1] = (h - r[3]) / 2;
}

static void update_letterbox(void)
{
	int r[4];
	letterbox(screen_w, screen_h, r);
	lb_x = r[0];
	lb_y = r[1];
	lb_w = r[2];
	lb_h = r[3];
}

bool Screen_GetLetterboxMode(void)
{
	return lb_mode;
}

void Screen_SetLetterboxMode(bool mode)
{
	lb_mode = mode;
	update_letterbox();
	reinit_touch_buttons();
}

void call_update_letterbox(void)
{
	update_letterbox();
	reinit_touch_buttons();
}

/* The emulator menu drawn on its screen in the headset's cockpit (draw_menu_vr)
 * is laid out in a game area of its own */
#define MENU_PX  4 /* texture pixels to a game unit (320 x 240) */
#define MENU_TEX_W (300 * MENU_PX) /* the menu's windows cover 300 x 180 of it */
#define MENU_TEX_H (180 * MENU_PX)
static bool menu_rect;

int Screen_GetGameOffsetX(void)
{
	return menu_rect ? 0 : lb_x;
}
int Screen_GetGameOffsetY(void)
{
	return menu_rect ? 0 : screen_h - lb_y - lb_h;
}
bool Screen_WindowToGame(int wx, int wy, int *gx, int *gy)
{
	if (cockpit_shown())
		return cockpit_window_to_screen(wx, wy, gx, gy);
	if (lb_w <= 0 || lb_h <= 0)
		return false;
	*gx = SCR_W * (wx - Screen_GetGameOffsetX()) / lb_w;
	*gy = SCR_H * (wy - Screen_GetGameOffsetY()) / lb_h;
	return true;
}

int Screen_GetGameHeight(void)
{
	return menu_rect ? 240 * MENU_PX : lb_h;
}
int Screen_GetGameWidth(void)
{
	return menu_rect ? 320 * MENU_PX : lb_w;
}

static void layout_rects(int w, int h, int r[GD_VP_COUNT][4])
{
	int game[4];
	letterbox(w, h, game);
	int panel_h = game[3] * PANEL_LINES / GAME_H;
	int win[4] = {0, 0, w, h};
	int view[4] = {game[0], game[1] + panel_h, game[2], game[3] - panel_h};
	memcpy(r[GD_VP_WINDOW], win, sizeof(win));
	memcpy(r[GD_VP_GAME], game, sizeof(game));
	memcpy(r[GD_VP_VIEW3D], view, sizeof(view));
}

/* =========================================================================
 * Window / context
 * ========================================================================= */
static SDL_Window *window;
static SDL_GLContext gl_context;

static void create_window(void)
{
#if GL_IS_GLES
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
#else
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif
	/* Painter's algorithm, but for the native scene (gl_native.h), which
	 * has the depth buffer. The stencil keeps the cockpit's turned passes
	 * apart (gl_cockpit.c). */
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

	Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
	if (bInFullScreen)
		flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
#ifdef ANDROID
	SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
	flags |= SDL_WINDOW_FULLSCREEN;
#endif

	window = SDL_CreateWindow(PROG_NAME, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, screen_w, screen_h,
							  flags);
	if (!window)
	{
		log_printf("SDL_CreateWindow failed: %s\n", SDL_GetError());
		SDL_Quit();
		exit(-1);
	}

	gl_context = SDL_GL_CreateContext(window);
	if (!gl_context)
	{
		log_printf("SDL_GL_CreateContext failed: %s\n", SDL_GetError());
		SDL_DestroyWindow(window);
		SDL_Quit();
		exit(-1);
	}

#if !GL_IS_GLES
	if (!gladLoadGLLoader(SDL_GL_GetProcAddress))
	{
		log_printf("Failed to initialize GLAD\n");
		exit(1);
	}
#endif

	log_printf("GL: %s / %s / %s\n", (const char *)glGetString(GL_VENDOR),
			   (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
}

/* The GL drawable can be bigger than the window SDL reports sizes and mouse
 * positions in (SDL_WINDOW_ALLOW_HIGHDPI: browsers with devicePixelRatio
 * != 1, Retina). Everything here lays out and draws in drawable pixels;
 * mouse events are scaled to match. */
static int win_w = 640, win_h = 480;

void Screen_SyncSize(void)
{
	int dw, dh, ww, wh;
	if (!window)
		return;
	SDL_GL_GetDrawableSize(window, &dw, &dh);
	SDL_GetWindowSize(window, &ww, &wh);
	if (dw <= 0 || dh <= 0 || ww <= 0 || wh <= 0)
		return;
	win_w = ww;
	win_h = wh;
	if (dw != screen_w || dh != screen_h)
	{
		screen_w = dw;
		screen_h = dh;
		call_update_letterbox();
	}
}

void Screen_MouseToPixels(SDL_Event *event)
{
	if (win_w == screen_w && win_h == screen_h)
		return;
	float sx = (float)screen_w / win_w, sy = (float)screen_h / win_h;
	switch (event->type)
	{
	case SDL_MOUSEMOTION:
		event->motion.x = (Sint32)(event->motion.x * sx);
		event->motion.y = (Sint32)(event->motion.y * sy);
		event->motion.xrel = (Sint32)(event->motion.xrel * sx);
		event->motion.yrel = (Sint32)(event->motion.yrel * sy);
		break;
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP:
		event->button.x = (Sint32)(event->button.x * sx);
		event->button.y = (Sint32)(event->button.y * sy);
		break;
	}
}

/* =========================================================================
 * Emulated 320x200 screen -> texture
 * ========================================================================= */
static GLuint blit_prog, blit_vao, blit_vbo, screen_tex;
static GLint u_blit_tex, u_blit_discard;
static uint32_t framebuf[SCR_W * SCR_H];

/* What the drawing needs of a frame the game finished, besides its 3D
 * scene (gl_scene.c): its screen and palettes, as they were at the flip.
 * The game fills `scr_pub`; drawing takes it into `scr_show`. */
typedef struct
{
	uint8_t screen[SCR_W * SCR_H];
	unsigned int main_rgb[256];
	unsigned int ctrl_rgb[16];
	int bgcol;
	int renderer; /* enum RENDERERS */
	bool cockpit;
	bool sub;  /* one of the game's other screens, for the cockpit's screens */
	int group; /* ... its group's (COCKPIT_SCREEN_*), -1 if not */
	bool mouse_shown;
	bool quiz; /* the station's quiz waits: its buttons go on the screen (quiz_buttons.h) */
	/* A cockpit frame that carries a new picture for the MAP screen
	 * (gl_mapscreen.h): that page's screen and palettes; the frame's own are
	 * the flight view's */
	bool page;     /* the frame went the way of a game screen's: the world aside */
	bool page_new; /* ... and the page is new */
	bool page_beside; /* the frame is a screen's (the station's): the page's scene is its own, beside it */
	int page_group;	  /* ... and the page is this group's: the MAP screen's or the SHIP screen's */
	uint8_t page_screen[SCR_W * SCR_H];
	unsigned int page_main_rgb[256];
	unsigned int page_ctrl_rgb[16];
	int page_bgcol;
} ScreenFrame;
static ScreenFrame scr_pub, scr_show;

/* The flight view's text under the cockpit's screen (the world capture's
 * labels), in the flight view's palette: the game thread fills `lab_pub`
 * (new: lab_new), drawing takes it into `lab_show` */
typedef struct
{
	bool on;
	uint8_t px[SCR_W * SCR_H];
	unsigned int rgb[256];
} LabelFrame;
static LabelFrame lab_pub, lab_show;
static bool lab_new, lab_uploaded;
static unsigned int flight_rgb[256]; /* game thread: the last flight frame's palette */
static GLuint label_tex;

/* The render thread and the hand over of frames, see "Frames" below */
#if defined(__EMSCRIPTEN__) || defined(ANDROID)
#define USE_RENDER_THREAD 0
#else
#define USE_RENDER_THREAD 1
#endif

volatile unsigned long dbg_frames; /* for the watchdog in main.c */
bool render_thread_enabled = USE_RENDER_THREAD;

static SDL_mutex *frame_lock;  /* scr_pub and the scene's published copy */
static bool frame_new;         /* published since the drawing last took one */

static SDL_Thread *render_thread;
static SDL_mutex *ctx_lock;     /* the fields below */
static SDL_cond *ctx_cond;
static bool ctx_on_render;      /* the render thread has (or is to take) the context */
static bool render_parked;      /* it has let go of it and waits */
static bool render_quit;

static const char *VS_BLIT = "in vec2 aPos;\n"
							 "in vec2 aUV;\n"
							 "out vec2 vUV;\n"
							 "void main(){ gl_Position = vec4(aPos, 0.0, 1.0); vUV = aUV; }\n";

/* int not bool: uniform bool is broken on many Android drivers */
static const char *FS_BLIT = "in vec2 vUV;\n"
							 "uniform sampler2D uTex;\n"
							 "uniform int uDiscard;\n"
							 "out vec4 fragColor;\n"
							 "void main(){\n"
							 "  vec4 c = texture(uTex, vUV);\n"
							 "  if (uDiscard != 0 && c.a < 0.5) discard;\n"
							 "  fragColor = c;\n"
							 "}\n";

static void init_screen_blit(void)
{
	static const char *const attribs[] = {"aPos", "aUV"};
	blit_prog = gd_build_program(VS_BLIT, FS_BLIT, attribs, 2);
	u_blit_tex = glGetUniformLocation(blit_prog, "uTex");
	u_blit_discard = glGetUniformLocation(blit_prog, "uDiscard");

	const float u1 = (float)SCR_W / SCR_TEX_W, v1 = (float)SCR_H / SCR_TEX_H;
	/* texture row 0 is the top of the screen */
	const float quad[4][4] = {
		{-1, -1, 0, v1},
		{1, -1, u1, v1},
		{-1, 1, 0, 0},
		{1, 1, u1, 0},
	};
	glGenVertexArrays(1, &blit_vao);
	glGenBuffers(1, &blit_vbo);
	glBindVertexArray(blit_vao);
	glBindBuffer(GL_ARRAY_BUFFER, blit_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));
	glBindVertexArray(0);

	glGenTextures(1, &screen_tex);
	glBindTexture(GL_TEXTURE_2D, screen_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, SCR_TEX_W, SCR_TEX_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glBindTexture(GL_TEXTURE_2D, 0);
}

/* The gamepad's pointer (input.h) is drawn into the screen, on the panel,
 * the HUD or the headset's virtual screen. So is the mouse's in the
 * headset, where the window's pointer can't be seen; where it points is
 * worked out in the window, as for clicks. */
static bool vr_pointer;

static void draw_pointer(void)
{
	static const char *const ARROW[] = {
		"X",       "XX",      "XWX",     "XWWX",     "XWWWX",   "XWWWWX",
		"XWWWWWX", "XWWWWWWX", "XWWWWXXX", "XWXWWX",  "XX XWWX", "    XWX", "     X",
	};
	int gx, gy;
	if (!Input_PadPointer(&gx, &gy) && !(scr_show.mouse_shown && Input_LaserPointer(&gx, &gy)) &&
		!(vr_pointer && scr_show.mouse_shown && Screen_WindowToGame(input.abs_x, input.abs_y, &gx, &gy)))
		return;
	for (int j = 0; j < (int)(sizeof ARROW / sizeof ARROW[0]); j++)
		for (int i = 0; ARROW[j][i]; i++)
		{
			int x = gx + i, y = gy + j;
			if (ARROW[j][i] == ' ' || x < 0 || x >= SCR_W || y < 0 || y >= SCR_H)
				continue;
			framebuf[y * SCR_W + x] = ARROW[j][i] == 'X' ? 0xff000000u : 0xffffffffu;
		}
}

/* The map's icon 6 (gl_mapscreen.h): its picture's inside is drawn by the
 * native renderer, not into the screen: the raster has colour 0 there, which
 * shows black. The classic picture stands in for it. */
#define ICON_6_ID 246
static const char ICON_6_ROWS[14][16] = {"AAAAAAAAAAAAAAA", "BBBBBBBBBBBBBBB", "BCCCCCCCCCCCCCB", "BCCCCCCCCCCCCCB",
										 "BDEDCCCCCCCCCCB", "BEEEDCFCGGCHCCB", "BEEEDCCCGGCCCCB", "BDEDCCBCCCCCCCB",
										 "BCCCCCCCBBCCIIB", "BCCCCCCCBBCCJCB", "BCCCCCCCCCCCJIB", "BCCCCCCCBCCCJJB",
										 "BCCCCCCCCCCCIIB", "BBBBBBBBBBBBBBB"};
static unsigned int icon6_rgb(char k)
{
	switch (k)
	{
	case 'A': return 0xfff0f0f0u;
	case 'B': return 0xffa0a0a0u;
	case 'D': return 0xff0080f0u; /* (the framebuffer is abgr) */
	case 'E': return 0xff00d0d0u;
	case 'F': return 0xfff00000u;
	case 'G': return 0xff80a0d0u;
	case 'H': return 0xff6080a0u;
	case 'I': return 0xff606060u;
	case 'J': return 0xff000000u;
	default: return 0xff808080u;
	}
}

/* The galaxy map's pan buttons: a frame and an arrow each, up, down, left, right */
static void draw_pans(void)
{
	int p[MAPSCREEN_PANS][4], n = mapscreen_pans(p);
	const unsigned int back = 0xff402010, edge = 0xffe0c080, arrow = 0xffffffff; /* ABGR */
	for (int i = 0; i < n; i++)
	{
		int x0 = p[i][0], y0 = p[i][1], w = p[i][2], h = p[i][3];
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				framebuf[(y0 + y) * SCR_W + x0 + x] = (x == 0 || y == 0 || x == w - 1 || y == h - 1) ? edge : back;
		/* a triangle pointing the way, its tip `d` = 0 and 7 rows deep */
		int cx = x0 + w / 2, cy = y0 + h / 2;
		for (int d = 0; d < 7; d++)
			for (int k = -(d / 2); k <= d / 2; k++)
			{
				int along = i == 0 || i == 2 ? d - 3 : 3 - d;
				framebuf[(cy + (i < 2 ? along : k)) * SCR_W + cx + (i < 2 ? k : along)] = arrow;
			}
	}
}

static bool sub_page; /* draw_sub draws the page beside the screen's frame */
static bool page_phone; /* the picture is the station's services': the phone icon on it */
static bool page_buttons; /* the picture is the MAP page: its panel's buttons are moved onto the view */

/* Over the caption (the buttons show the view): its rows are the stripes
 * left of it, then a frame and the word, lit while the orbit is shown */
static void draw_orbit_button(void)
{
	const unsigned int back = orbit_display ? 0xff006090 : 0xff302820, edge = 0xffa0a0a0, text = 0xffffffff; /* ABGR */
	for (int y = ORBIT_BTN_Y0 - 1; y <= ORBIT_BTN_Y1 + 1; y++)
		for (int x = ORBIT_BTN_X0 - 2; x <= 316; x++)
			framebuf[y * SCR_W + x] = framebuf[y * SCR_W + ORBIT_BTN_X0 - 3];
	for (int y = ORBIT_BTN_Y0; y <= ORBIT_BTN_Y1; y++)
		for (int x = ORBIT_BTN_X0; x <= ORBIT_BTN_X1; x++)
			framebuf[y * SCR_W + x] =
				(x == ORBIT_BTN_X0 || x == ORBIT_BTN_X1 || y == ORBIT_BTN_Y0 || y == ORBIT_BTN_Y1) ? edge : back;
	const char *word = "ORBIT";
	int x = ORBIT_BTN_X0 + 7;
	for (const char *c = word; *c; c++)
	{
		const unsigned char *g = screen_text_glyph(*c);
		if (!g)
			continue;
		for (int row = 0; row < 7; row++)
			for (int bit = 0; bit < 8; bit++)
				if (g[row] & (0x80 >> bit))
					framebuf[(ORBIT_BTN_Y0 + 1 + row) * SCR_W + x + bit] = text;
		x += g[9];
	}
}

void gl_screen_upload(void)
{
	const uint8_t *src = scr_show.screen;
	for (int y = 0; y < SCR_H; y++)
	{
		const unsigned int *pal = (y < 168) ? scr_show.main_rgb : scr_show.ctrl_rgb;
		const uint8_t *row = src + y * SCR_W;
		uint32_t *out = framebuf + y * SCR_W;
		for (int x = 0; x < SCR_W; x++)
			out[x] = (row[x] == 255) ? 0 : pal[row[x]];
	}
	if (page_buttons)
	{
		/* the panel's buttons, on the view (gl_mapscreen.h) */
		MapButton b[MAPSCREEN_BUTTONS];
		int n = mapscreen_buttons(b);
		for (int i = 0; i < n; i++)
			for (int y = 0; y < b[i].h; y++)
				for (int x = 0; x < b[i].w; x++)
				{
					uint8_t c = src[(b[i].sy + y) * SCR_W + b[i].sx + x];
					unsigned int px = scr_show.ctrl_rgb[c & 15];
					if (b[i].id == ICON_6_ID && c == 0 && b[i].w == 15 && b[i].h == 14)
						px = icon6_rgb(ICON_6_ROWS[y][x]);
					if (c != 255)
						framebuf[(b[i].dy + y) * SCR_W + b[i].dx + x] = px;
				}
	}
	/* the flight panel: the view's name gives way to the ORBIT button */
	bool orbit_btn = scr_show.cockpit && !scr_show.sub;
	hud_orbit_button_show(orbit_btn);
	if (orbit_btn)
		draw_orbit_button();
	MapButton ph;
	if (page_phone && mapscreen_phone(&ph))
		for (int y = 0; y < ph.h; y++)
			for (int x = 0; x < ph.w; x++)
			{
				uint8_t c = src[(ph.sy + y) * SCR_W + ph.sx + x];
				if (c != 255)
					framebuf[(ph.dy + y) * SCR_W + ph.dx + x] = scr_show.ctrl_rgb[c & 15];
			}
	if (page_buttons)
		draw_pans();
	/* (not on a side page: the frame's own screen is the station's) */
	if (scr_show.quiz && !sub_page)
		quiz_buttons_draw(framebuf, scr_show.main_rgb);
	draw_pointer();
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, screen_tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SCR_W, SCR_H, GL_RGBA, GL_UNSIGNED_BYTE, framebuf);
}

GLuint gl_screen_texture(void)
{
	return screen_tex;
}

GLuint gl_screen_labels_texture(void)
{
	if (!lab_show.on || !scr_show.sub)
		return 0;
	if (!label_tex)
	{
		glGenTextures(1, &label_tex);
		glBindTexture(GL_TEXTURE_2D, label_tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, SCR_TEX_W, SCR_TEX_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		lab_uploaded = false;
	}
	if (!lab_uploaded)
	{
		static uint32_t px[SCR_W * SCR_H];
		for (int i = 0; i < SCR_W * SCR_H; i++)
		{
			unsigned int c = lab_show.rgb[lab_show.px[i]];
			/* no flight frame seen yet: the game's white */
			px[i] = lab_show.px[i] == 0xff ? 0 : c ? c : 0xffffffffu;
		}
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, label_tex);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SCR_W, SCR_H, GL_RGBA, GL_UNSIGNED_BYTE, px);
		glBindTexture(GL_TEXTURE_2D, 0);
		lab_uploaded = true;
	}
	return label_tex;
}

/* The comms menu of the flight view: its check boxes are icon rectangles
 * (above the panel) with something drawn in them. The rows they take with
 * the text beside them. */
bool gl_screen_menu_rows(int *y0, int *y1)
{
	int rects[64][4], n = game_icon_rects(rects, 64);
	int top = SCR_H, bottom = -1;
	for (int i = 0; i < n; i++)
	{
		int *r = rects[i];
		if (r[1] < 0 || r[3] >= COCKPIT_PANEL_ROW0 || r[0] < 0 || r[2] >= SCR_W || r[1] > r[3] || r[0] > r[2])
			continue;
		bool drawn = false;
		for (int y = r[1]; y <= r[3] && !drawn; y++)
			for (int x = r[0]; x <= r[2]; x++)
				if (scr_show.screen[y * SCR_W + x] != 255)
				{
					drawn = true;
					break;
				}
		if (drawn)
		{
			top = r[1] < top ? r[1] : top;
			bottom = r[3] > bottom ? r[3] : bottom;
		}
	}
	if (bottom < 0)
		return false;
	*y0 = top > 4 ? top - 4 : 0;
	*y1 = bottom + 10 < COCKPIT_PANEL_ROW0 ? bottom + 10 : COCKPIT_PANEL_ROW0;
	return true;
}

typedef struct
{
	int transparent;
	int scissor[4]; /* x, y, w, h in the framebuffer; w = 0: none */
} BlitPass;

static void screen_blit_pass(const void *data)
{
	const BlitPass *b = data;
	gl_screen_upload();
	glUseProgram(blit_prog);
	glUniform1i(u_blit_tex, 0);
	glUniform1i(u_blit_discard, b->transparent);
	glBindVertexArray(blit_vao);
	if (b->scissor[2])
	{
		glEnable(GL_SCISSOR_TEST);
		glScissor(b->scissor[0], b->scissor[1], b->scissor[2], b->scissor[3]);
	}
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisable(GL_SCISSOR_TEST);
	glBindTexture(GL_TEXTURE_2D, 0);
}

/* =========================================================================
 * Screen_* API
 * ========================================================================= */
void Screen_Init(void)
{
	frame_lock = SDL_CreateMutex();
	ctx_lock = SDL_CreateMutex();
	ctx_cond = SDL_CreateCond();
	const char *rt = SDL_getenv("GLF_RENDER_THREAD"); /* 0: draw with the game, as before */
	if (rt && !atoi(rt))
		render_thread_enabled = false;
	create_window();
	Screen_SyncSize();
	update_letterbox();

	gd_init();
	gl_prims_init();
	gl_planet_init();
	init_screen_blit();
	cockpit_init();

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);

	SDL_EventState(SDL_MOUSEMOTION, SDL_ENABLE);
	SDL_EventState(SDL_MOUSEBUTTONDOWN, SDL_ENABLE);
	SDL_EventState(SDL_MOUSEBUTTONUP, SDL_ENABLE);
	SDL_ShowCursor(SDL_ENABLE);
	ui_init_gl(window, gl_context);
}

void Screen_UnInit(void)
{
	Screen_StopRenderThread();
	vr_shutdown();
	ui_shutdown();
	cockpit_shutdown();
	clipboard_shutdown();
	glDeleteTextures(1, &screen_tex);
	if (label_tex)
		glDeleteTextures(1, &label_tex);
	label_tex = 0;
	glDeleteBuffers(1, &blit_vbo);
	glDeleteVertexArrays(1, &blit_vao);
	glDeleteProgram(blit_prog);
	gl_planet_shutdown();
	gl_prims_shutdown();
	gd_shutdown();
}

void Screen_ToggleFullScreen(void)
{
	bInFullScreen = !bInFullScreen;
	if (SDL_SetWindowFullscreen(window, bInFullScreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) != 0)
		log_printf("SDL_SetWindowFullscreen failed: %s\n", SDL_GetError());
}

void Screen_ToggleRenderer(void)
{
	use_renderer++;
	if (use_renderer >= R_MAX)
		use_renderer = 0;
}

void Screen_GetRendererInfo(char *buf, int len)
{
	snprintf(buf, (size_t)len, "OpenGL %s\n%s\n%s", (const char *)glGetString(GL_VERSION),
			 (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER));
}

/* =========================================================================
 * Host calls that are not primitives
 * ========================================================================= */
void Nu_3DViewInit(void)
{
	screen_text_clear_queue();
	scene_reset();
}

void Nu_InsertZNode(void)
{
	if (!scene_insert_node((uint32_t)GetReg(REG_D4)))
		return;
	/* A1 is where the game is about to build the matching node of its own
	 * software depth tree. Some of those (atmosphere haze) have no GL hook,
	 * so remember the address and copy what the game put there later. */
	SoftNode *soft = scene_record(PRIM_SOFT_NODE, sizeof *soft);
	if (soft)
	{
		soft->node = (uint32_t)GetReg(REG_A1);
		soft->band = -1;
		soft->shell = -1;
	}
}

void Nu_IsGLRenderer(void)
{
	SetReg(REG_D0, use_renderer != R_OLD ? 1 : 0);
}

/* Marks a rectangle of the emulated screen transparent so the 3D view
 * shows through it. */
void Nu_GLClearArea(void)
{
	if (use_renderer == R_OLD)
		return;
	int x1 = GetReg(REG_D0) & 0xffff, y1 = GetReg(REG_D1) & 0xffff;
	int x2 = GetReg(REG_D2) & 0xffff, y2 = GetReg(REG_D3) & 0xffff;
	if (x2 > SCR_W)
		x2 = SCR_W;
	if (y2 > SCR_H)
		y2 = SCR_H;
	if (x1 >= x2 || y1 >= y2)
		return;

	unsigned char *s1 = (unsigned char *)PHYSCREEN + SCREENBYTES_LINE * y1;
	unsigned char *s2 = (unsigned char *)LOGSCREEN + SCREENBYTES_LINE * y1;
	for (int y = y1; y < y2; y++, s1 += SCREENBYTES_LINE, s2 += SCREENBYTES_LINE)
	{
		memset(s1 + x1, 255, (size_t)(x2 - x1));
		memset(s2 + x1, 255, (size_t)(x2 - x1));
	}
}

/* =========================================================================
 * Frame
 * ========================================================================= */
static void build_rgb_palette(unsigned int *rgb, const unsigned short *st, int len)
{
	for (int i = 0; i < len; i++)
	{
		int c = st[i];
		int b = (c & 0xf) << 4, g = (c & 0xf0), r = (c & 0xf00) >> 4;
		rgb[i] = 0xff000000u | (unsigned)(b << 16) | (unsigned)(g << 8) | (unsigned)r;
	}
}

static void clear_rect(const int r[4], unsigned int rgba)
{
	glEnable(GL_SCISSOR_TEST);
	glScissor(r[0], r[1], r[2], r[3]);
	glClearColor((rgba & 0xff) / 255.0f, ((rgba >> 8) & 0xff) / 255.0f, ((rgba >> 16) & 0xff) / 255.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_SCISSOR_TEST);
}

static void update_cursor(void)
{
	static int shown = -1;
	int want = mouse_shown ? 1 : 0;
	mouse_shown = 0;
	if (want != shown)
	{
		SDL_ShowCursor(want ? SDL_ENABLE : SDL_DISABLE);
		shown = want;
	}
}

static bool drawing_sub; /* draw_sub: the cockpit's screen's picture */
static unsigned int world_bg; /* the last cockpit frame's sky, kept under the cockpit's screen */

/* Draws the frame into the bound framebuffer, w x h pixels: the window's,
 * or a headset's (no touch overlays there) */
/* GLF_FRAME_LOG: milliseconds per stage, summed over the interval; 0-5 draw_picture's, 6-9 draw_frame's */
static double stage_ms[10];

static double stage_lap(Uint64 *t)
{
	Uint64 now = SDL_GetPerformanceCounter();
	double ms = (double)(now - *t) * 1000.0 / (double)SDL_GetPerformanceFrequency();
	*t = now;
	return ms;
}

static void draw_frame(int w, int h, bool cockpit, bool window)
{
	Uint64 lap = SDL_GetPerformanceCounter();
	int rects[GD_VP_COUNT][4];
	layout_rects(w, h, rects);
	if (window) /* where a mouse click on the clipboard lands */
		clipboard_window_view(cockpit);
	if (cockpit) /* the 3D view fills the window */
	{
		memcpy(rects[GD_VP_GAME], rects[GD_VP_WINDOW], sizeof(rects[0]));
		memcpy(rects[GD_VP_VIEW3D], rects[GD_VP_WINDOW], sizeof(rects[0]));
	}
	glViewport(0, 0, w, h);
	glClearColor(0, 0, 0, 1);
	glStencilMask(0xff);
	glClearStencil(0);
	glDepthMask(GL_TRUE);
	glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	if (scr_show.renderer != R_GLWIRE)
	{
		unsigned int bg = cockpit && scr_show.sub ? world_bg : scr_show.main_rgb[scr_show.bgcol & 0xff];
		/* the sky's colour, not the ground's the game fills an all-ground
		 * view with (gl_atmos.c) */
		int sky = atmos_sky_colour();
		if (cockpit && sky >= 0 && (scr_show.sub || (scr_show.bgcol & 0xff) == 0xe))
			build_rgb_palette(&bg, (const unsigned short[]){(unsigned short)sky}, 1);
		clear_rect(rects[GD_VP_GAME], bg);
	}

	gd_begin_frame(rects);

	/* 1. 3D view */
	mat4 persp = cockpit ? cockpit_world_projection()
						 : mat4_perspective(CLASSIC_FOV, CLASSIC_ASPECT, 1.0f, 10000000000.0f);
	gd_set_viewport(GD_VP_VIEW3D);
	gd_set_projection(&persp);
	gd_set_wireframe(scr_show.renderer == R_GLWIRE);
	stage_ms[6] += stage_lap(&lap);
	native_draw_sky();
	stage_ms[7] += stage_lap(&lap);
	/* the cockpit's world is all native: the game's passes only capture it */
	if (!(cockpit && native_mode() == 2))
		scene_draw();
	gl_prims_finish();
	native_draw();
	stage_ms[8] += stage_lap(&lap);
	gd_set_wireframe(false);
	/* our crosshair and velocity marker (gl_hud.h), on the world (not on
	 * the cockpit's screen's picture) */
	mat4 dirs = cockpit_direction_projection();
	if (!drawing_sub)
		hud_draw(&dirs);

	/* 2. emulated screen */
	if (cockpit)
	{
		cockpit_draw(); /* HUD, cockpit and panel */
	}
	else
	{
		mat4 ortho = mat4_ortho(0, 320, 0, 200, -1, 1);
		gd_set_viewport(GD_VP_GAME);
		gd_set_projection(&ortho);
		gd_identity();
		gd_color3ub(0, 0, 0);
		gd_rect2(0, 0, 320, 32); /* black strip behind the control panel */

		BlitPass *blit = gd_custom(screen_blit_pass, sizeof *blit);
		if (blit)
		{
			blit->transparent = (scr_show.renderer != R_OLD);
			blit->scissor[2] = 0;
		}

		/* the clipboard over the picture while it is out */
		gd_set_viewport(GD_VP_WINDOW);
		if (!drawing_sub)
			clipboard_queue_flat(w, h);
	}

	/* 3. overlays */
	if (window)
		gl_overlay_draw();

	stage_ms[9] += stage_lap(&lap);
	gd_flush();
}

/* The window waits for its vsync, which paces the drawing, except while
 * a headset runs: then the headset paces it and the window must not wait
 * as well */
static void update_swap_interval(bool vr)
{
	static int current = -1;
	int want = vr || SDL_getenv("GLF_NOVSYNC") ? 0 : 1; /* GLF_NOVSYNC: measuring */
	if (want == current)
		return;
	if (SDL_GL_SetSwapInterval(want) != 0 && want == 1)
		log_printf("No vsync for the window: %s\n", SDL_GetError());
	current = want;
}

/* The emulator menu's picture for its screen in the cockpit (PLACE_MENU),
 * while the menu is open: 0 if it is not. Mipmapped, as it is seen small;
 * what no window covers is clear. */
static GLuint menu_fbo, menu_tex;

static GLuint draw_menu_vr(void)
{
	if (!ui_menu_open())
		return 0;
	if (!menu_fbo)
	{
		glGenTextures(1, &menu_tex);
		glBindTexture(GL_TEXTURE_2D, menu_tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, MENU_TEX_W, MENU_TEX_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glGenFramebuffers(1, &menu_fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, menu_fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, menu_tex, 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			log_printf("The menu's framebuffer is incomplete\n");
	}
	glBindFramebuffer(GL_FRAMEBUFFER, menu_fbo);
	glViewport(0, 0, MENU_TEX_W, MENU_TEX_H);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	menu_rect = true;
	ui_render_vr(MENU_TEX_W, MENU_TEX_H);
	menu_rect = false;
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, menu_tex);
	glGenerateMipmap(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, 0);
	return menu_tex;
}

/* The frame for the headset, if it wants one: the cockpit per eye, or
 * the classic picture on the virtual screen. Before the window's: the
 * window's cockpit is turned by the same head pose. */
/* GLF_FRAME_PROF: the GPU time of the headset's eyes, from timer queries read
 * a few pictures late so as never to wait for them */
#define VR_GPU_QUERIES 6
static GLuint vr_gpu_q[VR_GPU_QUERIES];
static bool vr_gpu_issued[VR_GPU_QUERIES];
static int vr_gpu_next;
static bool vr_gpu_open;

static void vr_gpu_begin(void)
{
#if !GL_IS_GLES
	if (!prof_on)
		return;
	if (!vr_gpu_q[0])
		glGenQueries(VR_GPU_QUERIES, vr_gpu_q);
	/* (a query still unread when its turn comes round is dropped) */
	glBeginQuery(GL_TIME_ELAPSED, vr_gpu_q[vr_gpu_next]);
	vr_gpu_open = true;
#endif
}

static void vr_gpu_end(void)
{
#if !GL_IS_GLES
	if (!vr_gpu_open)
		return;
	glEndQuery(GL_TIME_ELAPSED);
	vr_gpu_issued[vr_gpu_next] = true;
	vr_gpu_next = (vr_gpu_next + 1) % VR_GPU_QUERIES;
	vr_gpu_open = false;
	for (int i = 0; i < VR_GPU_QUERIES; i++)
	{
		GLuint ready = 0;
		if (!vr_gpu_issued[i])
			continue;
		glGetQueryObjectuiv(vr_gpu_q[i], GL_QUERY_RESULT_AVAILABLE, &ready);
		if (!ready)
			continue;
		GLuint64 ns = 0;
		glGetQueryObjectui64v(vr_gpu_q[i], GL_QUERY_RESULT, &ns);
		prof_vr_gpu((unsigned)ns);
		vr_gpu_issued[i] = false;
	}
#endif
}

static bool draw_vr(bool with_menu)
{
	vr_update();
	update_swap_interval(vr_running());
	cockpit_set_menu_texture(0);
	ui_set_vr_menu(false);
	if (!vr_running())
	{
		laser_off();
		cockpit_set_headset(NULL, NULL);
		return cockpit_begin_frame(screen_w, screen_h, scr_show.cockpit, scr_show.sub ? scr_show.group : -1);
	}

	uint64_t prof_t0 = PROF_BEGIN();
	bool render = vr_begin_frame();
	uint64_t prof_t1 = PROF_BEGIN();
	VrView eyes[2], centre;
	bool tracked = render && vr_tracking();
	if (tracked)
	{
		vr_view(0, &eyes[0]);
		vr_view(1, &eyes[1]);
		vr_centre_view(&centre);
		cockpit_set_headset(&centre, eyes);
	}
	else
	{
		cockpit_set_headset(NULL, NULL);
	}
	/* the menu, on a screen in the cockpit (the window then doesn't show it) */
	if (with_menu && tracked && (scr_show.cockpit || scr_show.sub))
	{
		GLuint menu = draw_menu_vr();
		cockpit_set_menu_texture(menu);
		ui_set_vr_menu(menu != 0);
	}
	bool cockpit = cockpit_begin_frame(screen_w, screen_h, scr_show.cockpit, scr_show.sub ? scr_show.group : -1);
	laser_update(cockpit && tracked);

	if (render)
	{
		vr_pointer = true;
		vr_gpu_begin();
		if (cockpit && tracked)
		{
			for (int i = 0; i < 2; i++)
			{
				int w, h;
				vr_eye_size(i, &w, &h);
				glBindFramebuffer(GL_FRAMEBUFFER, vr_acquire_eye(i));
				cockpit_use_eye(&eyes[i]);
				draw_frame(w, h, true, false);
				vr_release_eye(i);
			}
			cockpit_use_eye(NULL);
		}
		else
		{
			glBindFramebuffer(GL_FRAMEBUFFER, vr_acquire_screen());
			draw_frame(VR_SCREEN_W, VR_SCREEN_H, cockpit, false);
			vr_release_screen();
			/* the lasers over it */
			for (int i = 0; tracked && laser_shown() && i < 2; i++)
			{
				int w, h;
				vr_eye_size(i, &w, &h);
				glBindFramebuffer(GL_FRAMEBUFFER, vr_acquire_overlay_eye(i));
				laser_draw_overlay(w, h, &eyes[i]);
				vr_release_overlay_eye(i);
			}
		}
		vr_gpu_end();
		vr_pointer = false;
	}
	uint64_t prof_t2 = PROF_BEGIN();
	vr_end_frame();
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (prof_on)
		prof_vr_add(prof_t1 - prof_t0, prof_t2 - prof_t1, prof_now() - prof_t2);
	return cockpit;
}

/* =========================================================================
 * Frames: the game and the drawing
 *
 * The game draws a frame (records it, gl_scene.c) and flips it at
 * Nu_DrawScreen, at most 50 times a second. The drawing shows the latest
 * finished one as often as the display (the window's vsync, or a VR
 * headset) wants a picture, each time with the newest head pose, so
 * looking around is as smooth as the display allows and the game never
 * waits for the drawing.
 *
 * The drawing runs on a thread of its own (the render thread), which owns
 * the GL context. The game, the window and SDL's events stay on the main
 * thread. While the menu (or the galaxy atlas, or the free camera's help)
 * is open, the render thread hands the context back and the game draws
 * each frame itself as it flips, with the menu on top, as it always did:
 * the menu works on the game's memory and the window, which only the main
 * thread may touch. Without threads (the web build, Android) it always
 * draws that way.
 * ========================================================================= */
static SDL_atomic_t published; /* frames, for GLF_FRAME_LOG */

/* Game thread: frames for the cockpit's screen no faster than about this.
 * The game's FPS display turns its own 50 a second limit off, so a screen,
 * quick to draw, comes a thousand or more times a second, each now with
 * the world captured: wasted work, as the drawing shows at most one a
 * picture. A ceiling, not a beat: the drawing is not waited for. */
#define SCREEN_FPS 60
static void pace_screen_frames(void)
{
	static Uint64 last;
	Uint64 freq = SDL_GetPerformanceFrequency(), gap = freq / SCREEN_FPS;
	Uint64 now = SDL_GetPerformanceCounter();
	if (last && now - last < gap)
		SDL_Delay((Uint32)((gap - (now - last)) * 1000 / freq));
	last = SDL_GetPerformanceCounter();
}

/* The MAP screen's page, made on the side for the cockpit frame being
 * published (game thread) */
static MapPage map_page;

#define POINTER_HOLD 8

/* Game thread: makes the frame the game just flipped the latest */
static void publish_frame(void)
{
	bool cockpit = cockpit_game_frame();
	int group = cockpit ? -1 : cockpit_screen_group();
	/* beside the station's services the frame is their screen's (the world is
	 * captured), and the page's scene is kept apart from theirs */
	bool beside = group == COCKPIT_SCREEN_COMS;
	int page_group = (cockpit || beside) ? mapscreen_due() : -1;
	bool page = page_group >= 0;

	uint64_t t = PROF_BEGIN();
	SDL_LockMutex(frame_lock);
	PROF_END(PS_PUB_LOCK, t);
	t = PROF_BEGIN();
	scr_pub.cockpit = cockpit;
	scr_pub.group = group;
	scr_pub.sub = group >= 0;
	scr_pub.page = scr_pub.page_new = scr_pub.page_beside = false;
	build_rgb_palette(scr_pub.main_rgb, MainPalette, len_main_palette);
	build_rgb_palette(scr_pub.ctrl_rgb, CtrlPalette, 16);
	scr_pub.bgcol = fe2_bgcol;
	scr_pub.renderer = use_renderer;
	/* the game doesn't ask for the pointer in every frame (landed, away from
	 * the station's services): it stays a few frames, not flickering */
	static int pointer_hold;
	pointer_hold = mouse_shown ? POINTER_HOLD : pointer_hold > 0 ? pointer_hold - 1 : 0;
	mouse_shown = pointer_hold > 0;
	scr_pub.mouse_shown = mouse_shown;
	scr_pub.quiz = game_quiz_waiting();

	if (page && cockpit)
	{
		/* the flight frame is the world, as a captured one is, and the page
		 * is made next; the frame is published after it */
		scene_publish_world();
		PROF_END(PS_PUB_SCENE, t);
		t = PROF_BEGIN();
		hud_publish(false);
		PROF_END(PS_PUB_HUD, t);
		t = PROF_BEGIN();
		/* the frame before, if the drawing hasn't taken it, is dropped: it
		 * would be taken with this one half made, and a page's scene (the
		 * published one) as the world's */
		frame_new = false;
		/* ... and its picture is the one before the batch drew its own */
		memcpy(scr_pub.screen, VideoRaster, sizeof scr_pub.screen);
	}
	else
	{
		memcpy(scr_pub.screen, VideoRaster, sizeof scr_pub.screen);
		PROF_END(PS_PUB_REST, t);
		t = PROF_BEGIN();
		scene_publish(scr_pub.sub);
		PROF_END(PS_PUB_SCENE, t);
		t = PROF_BEGIN();
		hud_publish(scr_pub.sub);
		PROF_END(PS_PUB_HUD, t);
		t = PROF_BEGIN();
		frame_new = true;
	}
	if (scr_pub.cockpit)
		memcpy(flight_rgb, scr_pub.main_rgb, sizeof flight_rgb);
	if (!scr_pub.sub && lab_pub.on)
	{
		lab_pub.on = false;
		lab_new = true;
	}
	SDL_UnlockMutex(frame_lock);
	PROF_END(PS_PUB_REST, t);

	t = PROF_BEGIN();
	if (page)
	{
		bool ok = mapscreen_page(page_group, &map_page);
		SDL_LockMutex(frame_lock);
		if (beside)
		{
			scene_publish_page();
			scr_pub.page_beside = true;
		}
		else
		{
			if (!scr_pub.cockpit)
				memcpy(scr_pub.screen, VideoRaster, sizeof scr_pub.screen);
			scene_publish(true);
		}
		scr_pub.page = true;
		if (ok)
		{
			scr_pub.page_new = true;
			scr_pub.page_group = page_group;
			memcpy(scr_pub.page_screen, map_page.screen, sizeof scr_pub.page_screen);
			build_rgb_palette(scr_pub.page_main_rgb, map_page.main_pal, map_page.len_main);
			build_rgb_palette(scr_pub.page_ctrl_rgb, map_page.ctrl_pal, 16);
			scr_pub.page_bgcol = map_page.bgcol;
		}
		frame_new = true;
		SDL_UnlockMutex(frame_lock);
	}
	PROF_END(PS_PUB_PAGE, t);

	/* the world round the cockpit's screen, live, with its labels */
	if (scr_pub.sub)
	{
		uint64_t t = PROF_BEGIN();
		pace_screen_frames();
		PROF_END(PS_PACE, t);
	}
	t = PROF_BEGIN();
	bool world = scr_pub.sub && cockpit_capture_world();
	if (world)
	{
		SDL_LockMutex(frame_lock);
		scene_publish_world();
		lab_pub.on = screen_text_render_queue(lab_pub.px);
		memcpy(lab_pub.rgb, flight_rgb, sizeof lab_pub.rgb);
		lab_new = true;
		SDL_UnlockMutex(frame_lock);
	}
	screen_text_clear_queue();
	PROF_END(PS_PUB_WORLD, t);
	/* the camera screen's picture, the game's own (gl_camera.h) */
	t = PROF_BEGIN();
	if (scr_pub.cockpit || scr_pub.sub)
		camera_capture(scr_pub.sub);
	if (world)
		camera_shots_drawn();
	PROF_END(PS_PUB_CAMERA, t);
	hud_frame_end();
	SDL_AtomicIncRef(&published);
}

/* Whoever draws: takes the latest frame, if there is a new one */
static bool acquire_frame(void)
{
	uint64_t prof_t0 = PROF_BEGIN();
	SDL_LockMutex(frame_lock);
	bool got = frame_new;
	if (frame_new)
	{
		scr_show = scr_pub;
		if (scr_show.cockpit)
			world_bg = scr_show.main_rgb[scr_show.bgcol & 0xff];
		scene_acquire(scr_show.sub || scr_show.page);
		if (scr_show.page_beside && scr_show.page_new)
			scene_acquire_page();
		hud_acquire();
		frame_new = false;
	}
	if (lab_new)
	{
		lab_show = lab_pub;
		lab_new = false;
		lab_uploaded = false;
	}
	SDL_UnlockMutex(frame_lock);
	if (prof_on)
		prof_render_acquire(prof_t0);
	return got;
}

/* GLF_FRAME_LOG=1: pictures drawn and game frames per second, every two
 * seconds */
static void log_rates(bool new_frame)
{
	static int log = -1, pictures, frames;
	static Uint32 since;
	if (log < 0)
#ifdef ANDROID
		log = 1; /* no environment to set there */
#else
		log = SDL_getenv("GLF_FRAME_LOG") != NULL;
#endif
	if (!log)
		return;
	pictures++;
	frames += new_frame;
	Uint32 now = SDL_GetTicks();
	if (now - since >= 2000)
	{
		if (since)
			log_printf("frames: %.1f pictures/s, %.1f game frames/s (%.1f shown)\n",
					   pictures * 1000.0f / (now - since), SDL_AtomicGet(&published) * 1000.0f / (now - since),
					   frames * 1000.0f / (now - since));
		if (since)
		{
			log_printf("stages ms/picture: acquire %.2f sub %.2f vr %.2f window %.2f menu %.2f swap %.2f\n",
					   stage_ms[0] / pictures, stage_ms[1] / pictures, stage_ms[2] / pictures,
					   stage_ms[3] / pictures, stage_ms[4] / pictures, stage_ms[5] / pictures);
		}
		log_printf("window ms/picture: clear+setup %.2f sky %.2f native %.2f cockpit/screen %.2f\n", stage_ms[6] / pictures,
				   stage_ms[7] / pictures, stage_ms[8] / pictures, stage_ms[9] / pictures);
		memset(stage_ms, 0, sizeof stage_ms);
		SDL_AtomicSet(&published, 0);
		pictures = frames = 0;
		since = now;
	}
}

/* The cockpit's screens' pictures (gl_cockpit.h): the game's screen drawn
 * the classic way, as the window would show it without the cockpit, into
 * its group's texture; mipmapped, as it is seen small. The other groups'
 * keep the last picture they had. */
#define SUB_W 1280
#define SUB_H 960
static GLuint sub_fbos[COCKPIT_SCREENS], sub_texs[COCKPIT_SCREENS], sub_depth;
static bool sub_drawn[COCKPIT_SCREENS];
static volatile bool coms_station_drawn; /* the station's services have drawn the COMS texture */

void gl_screen_coms_forget(void)
{
	coms_station_drawn = false;
}

GLuint gl_screen_sub_texture(int group)
{
	return group >= 0 && group < COCKPIT_SCREENS && sub_drawn[group] ? sub_texs[group] : 0;
}

/* The group's framebuffer, made at first use; its texture in *tex */
static GLuint sub_target(int group, GLuint *tex)
{
	if (!sub_depth)
	{
		glGenRenderbuffers(1, &sub_depth);
		glBindRenderbuffer(GL_RENDERBUFFER, sub_depth);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, SUB_W, SUB_H);
	}
	GLuint *sub_fbo = &sub_fbos[group], *sub_tex = &sub_texs[group];
	if (!*sub_fbo)
	{
		glGenTextures(1, sub_tex);
		glBindTexture(GL_TEXTURE_2D, *sub_tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, SUB_W, SUB_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glGenFramebuffers(1, sub_fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, *sub_fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *sub_tex, 0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, sub_depth);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			log_printf("The cockpit's screen's framebuffer is incomplete\n");
	}
	*tex = *sub_tex;
	return *sub_fbo;
}


static void draw_sub(int group)
{
	if (group < 0 || group >= COCKPIT_SCREENS)
		return;
	GLuint tex;
	GLuint fbo = sub_target(group, &tex);
	GLuint *sub_fbo = &fbo, *sub_tex = &tex;
	glBindFramebuffer(GL_FRAMEBUFFER, *sub_fbo);
	cockpit_draw_flat(true);
	if (sub_page)
		scene_use_page(true);
	else
		native_use_screen(true);
	drawing_sub = true;
	draw_frame(SUB_W, SUB_H, false, false);
	drawing_sub = false;
	if (sub_page)
		scene_use_page(false);
	else
		native_use_screen(false);
	cockpit_draw_flat(false);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, *sub_tex);
	glGenerateMipmap(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, 0);
	sub_drawn[group] = true;
	if (group == COCKPIT_SCREEN_COMS)
		coms_station_drawn = true;
}

/* The COMS screen when it isn't the game's live screen and the ship is
 * undocked: the station's page it last had is gone. In its place the flight
 * comms menu's text (rows of the screen where the menu is), on nothing, or
 * nothing at all. */
enum
{
	COMS_KEPT,  /* the texture is the station's (or never drawn) */
	COMS_CLEAR, /* cleared */
	COMS_MENU   /* the comms menu's text */
};
static int coms_state;

static void draw_coms_flight(bool new_frame)
{
	FlightStatus fs = game_flight_status();
	bool station = fs == FLIGHT_DOCKED || fs == FLIGHT_LANDED || fs == FLIGHT_LANDED_ROUGH;
	/* (in flight the station's page is gone, and a flag left from an earlier
	 * visit must not make a later landing's COMS button look like it has one) */
	if (!station)
		coms_station_drawn = false;
	/* (landed, with none of the station's services drawn yet, the texture is
	 * not anyone's: the flight's view is in it, as at take-off) */
	if (!cockpit_screens_on() || (station && coms_station_drawn) ||
		(scr_show.sub && scr_show.group == COCKPIT_SCREEN_COMS))
	{
		coms_state = COMS_KEPT;
		return;
	}
	int y0, y1;
	bool menu = !station && scr_show.cockpit && !scr_show.sub && gl_screen_menu_rows(&y0, &y1);
	/* the last menu's text stays up after the game's own has gone */
	if (!menu && coms_state != COMS_KEPT)
		return;
	if (menu && !new_frame && coms_state == COMS_MENU)
		return;
	GLuint tex;
	GLuint fbo = sub_target(COCKPIT_SCREEN_COMS, &tex);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glViewport(0, 0, SUB_W, SUB_H);
	glClearColor(0, 0, 0, 0);
	glStencilMask(0xff);
	glDepthMask(GL_TRUE);
	glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	if (menu)
	{
		int rects[GD_VP_COUNT][4];
		layout_rects(SUB_W, SUB_H, rects);
		gd_begin_frame(rects);
		gd_set_viewport(GD_VP_GAME);
		BlitPass *blit = gd_custom(screen_blit_pass, sizeof *blit);
		if (blit)
		{
			const int *g = rects[GD_VP_GAME];
			blit->transparent = 1;
			blit->scissor[0] = g[0];
			blit->scissor[1] = g[1] + g[3] * (SCR_H - y1) / SCR_H;
			blit->scissor[2] = g[2];
			blit->scissor[3] = g[3] * (y1 - y0) / SCR_H + 1;
		}
		gd_flush();
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, tex);
	glGenerateMipmap(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, 0);
	sub_drawn[COCKPIT_SCREEN_COMS] = true;
	coms_state = menu ? COMS_MENU : COMS_CLEAR;
}

GLuint gl_screen_coms_menu(void)
{
	return coms_state == COMS_MENU ? sub_texs[COCKPIT_SCREEN_COMS] : 0;
}

bool gl_screen_coms_clear(void)
{
	return coms_state != COMS_KEPT;
}

/* The MAP or SHIP screen's new page: the frame's picture and scene for the time of
 * drawing it into the group's texture */
static void draw_page(void)
{
	static ScreenFrame flight;
	memcpy(&flight, &scr_show, sizeof flight);
	memcpy(scr_show.screen, flight.page_screen, sizeof scr_show.screen);
	memcpy(scr_show.main_rgb, flight.page_main_rgb, sizeof scr_show.main_rgb);
	memcpy(scr_show.ctrl_rgb, flight.page_ctrl_rgb, sizeof scr_show.ctrl_rgb);
	scr_show.bgcol = flight.page_bgcol;
	scr_show.cockpit = false;
	scr_show.sub = true;
	sub_page = flight.page_beside;
	page_buttons = flight.page_group == COCKPIT_SCREEN_MAP;
	draw_sub(flight.page_group);
	page_buttons = false;
	sub_page = false;
	memcpy(&scr_show, &flight, sizeof scr_show);
	/* the flight view's own frame is next: not the page's objects in it */
	if (flight.cockpit && !flight.page_beside)
		scene_drop_shown();
}

static Uint32 last_draw_ticks;

/* One picture: the headset's (if any), then the window's */
static void draw_picture(bool with_menu)
{
	Uint64 lap = SDL_GetPerformanceCounter(), pic0 = lap;
	bool new_frame = acquire_frame();
	log_rates(new_frame);
	stage_ms[0] += stage_lap(&lap);
	if (scr_show.sub)
	{
		page_buttons = scr_show.group == COCKPIT_SCREEN_MAP && mapscreen_real_buttons();
		draw_sub(scr_show.group);
		page_buttons = page_phone = false;
	}
	if (scr_show.page_new && new_frame)
		draw_page();
	draw_coms_flight(new_frame);
	stage_ms[1] += stage_lap(&lap);
	bool cockpit = draw_vr(with_menu);
	stage_ms[2] += stage_lap(&lap);
#ifdef ANDROID
	/* the headset is all there is to see: nobody looks at the window, and
	 * drawing the world a third time costs a third of the frame */
	bool window_seen = !vr_running() || ui_visible();
#else
	bool window_seen = true;
#endif
	if (window_seen)
		draw_frame(screen_w, screen_h, cockpit, true);
	stage_ms[3] += stage_lap(&lap);
	if (with_menu && window_seen)
		ui_render(); /* 4. emulator menu */
	stage_ms[4] += stage_lap(&lap);
	dbg_frames++;
	last_draw_ticks = SDL_GetTicks();
	Uint64 swap0 = SDL_GetPerformanceCounter();
	if (window_seen)
		SDL_GL_SwapWindow(window);
	stage_ms[5] += stage_lap(&lap);
	if (prof_on)
		prof_render_add(pic0, swap0, SDL_GetPerformanceCounter());
}

static int render_main(void *arg)
{
	(void)arg;
	SDL_LockMutex(ctx_lock);
	for (;;)
	{
		while (!ctx_on_render && !render_quit)
		{
			render_parked = true;
			SDL_CondBroadcast(ctx_cond);
			SDL_CondWait(ctx_cond, ctx_lock);
		}
		if (render_quit)
			break;
		if (render_parked)
		{
			render_parked = false;
			SDL_GL_MakeCurrent(window, gl_context);
		}
		SDL_UnlockMutex(ctx_lock);
		page_phone = scr_show.group == COCKPIT_SCREEN_COMS;

		Uint64 t0 = SDL_GetPerformanceCounter();
		draw_picture(false);
		/* no vsync (and no headset): don't draw much faster than a fast
		 * display would show */
		if (!vr_running() && SDL_GL_GetSwapInterval() == 0 && !SDL_getenv("GLF_NOVSYNC"))
		{
			double ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
			if (ms < 4.0)
				SDL_Delay((Uint32)(4.0 - ms) + 1);
		}

		SDL_LockMutex(ctx_lock);
		if (!ctx_on_render || render_quit)
			SDL_GL_MakeCurrent(window, NULL);
	}
	render_parked = true;
	SDL_CondBroadcast(ctx_cond);
	SDL_UnlockMutex(ctx_lock);
	return 0;
}

/* Main thread: the context to the render thread (starting it), or back */
static void context_to_render(void)
{
	if (!render_thread)
	{
		SDL_GL_MakeCurrent(window, NULL);
		ctx_on_render = true;
		render_parked = true;
		render_thread = SDL_CreateThread(render_main, "render", NULL);
		if (!render_thread)
		{
			log_printf("No render thread (%s): drawing with the game\n", SDL_GetError());
			ctx_on_render = false;
			render_thread_enabled = false;
			SDL_GL_MakeCurrent(window, gl_context);
		}
		return;
	}
	SDL_LockMutex(ctx_lock);
	if (!ctx_on_render)
	{
		SDL_GL_MakeCurrent(window, NULL);
		ctx_on_render = true;
		SDL_CondBroadcast(ctx_cond);
	}
	SDL_UnlockMutex(ctx_lock);
}

static void context_to_main(void)
{
	if (!render_thread)
		return;
	SDL_LockMutex(ctx_lock);
	if (ctx_on_render)
	{
		ctx_on_render = false;
		SDL_CondBroadcast(ctx_cond);
		while (!render_parked)
			SDL_CondWait(ctx_cond, ctx_lock);
		SDL_GL_MakeCurrent(window, gl_context);
	}
	SDL_UnlockMutex(ctx_lock);
}

void Screen_StopRenderThread(void)
{
	if (!render_thread)
		return;
	context_to_main();
	SDL_LockMutex(ctx_lock);
	render_quit = true;
	SDL_CondBroadcast(ctx_cond);
	SDL_UnlockMutex(ctx_lock);
	SDL_WaitThread(render_thread, NULL);
	render_thread = NULL;
}

/* Game thread, from the host update. The game's menus wait for input in a loop
 * that draws nothing, and the headset's frames and controllers are served only
 * by drawing: without this the picture freezes there and no click ever comes.
 * Draws the last frame again when none has been drawn for a while. */
void Screen_RefreshVr(void)
{
	static bool logged;
	if (render_thread_enabled || !vr_running() || SDL_GetTicks() - last_draw_ticks < 10)
		return;
	if (!logged)
	{
		logged = true;
		log_printf("VR refresh: the game stopped drawing, drawing for it\n");
	}
	context_to_main();
	draw_picture(true);
}

void Nu_DrawScreen(void)
{
	uint64_t t = PROF_BEGIN();
	netplay_tick(); /* game thread: the other player's ghost ship */
	PROF_END(PS_NETPLAY, t);

	Screen_SyncSize(); /* a DPI change (browser zoom, other monitor) may come without a resize event */

	/* the text the game queued during the frame, into the emulated screen */
	t = PROF_BEGIN();
	screen_text_draw_queue();
	PROF_END(PS_TEXT, t);

	t = PROF_BEGIN();
	publish_frame();
	update_cursor();
	PROF_END(PS_PUBLISH, t);

	if (render_thread_enabled && !ui_visible())
	{
		t = PROF_BEGIN();
		context_to_render();
		PROF_END(PS_HANDOFF, t);
		return;
	}
	t = PROF_BEGIN();
	context_to_main();
	PROF_END(PS_HANDOFF, t);
	t = PROF_BEGIN();
	draw_picture(true);
	PROF_END(PS_INLINE, t);
}
