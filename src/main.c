/*
 * main.c - start up, the SDL event loop and logging.
 */
#include <signal.h>
#include <time.h>

#include <SDL.h>

#include "main.h"
#include "settings.h"
#include "audio.h"
#include "freecam.h"
#include "gamepad.h"
#include "m68000.h"
#include "hostcall.h"
#include "input.h"
#include "keymap.h"
#include "custom_ships.h"
#include "renderer.h"
#include "touch_input.h"
#include "ui/ui.h"
#include "vr.h"

#ifdef ANDROID
#include <android/log.h>
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

BOOL bQuitProgram = FALSE;
BOOL bEmulationActive = TRUE;
char szBootDiscImage[MAX_FILENAME_LENGTH] = {""};
char szWorkingDir[MAX_FILENAME_LENGTH] = {""};
char szCurrentDir[MAX_FILENAME_LENGTH] = {""};

static bool use_fullscreen;
static bool right_button_held; /* Ctrl-M toggles a held right button */

bool toggle_touch_controls = false;
bool toggle_m68k_menu = false;
bool toggle_fps_draw = false;
bool toggle_debug_draw = false;
int dump_m68k_toggle = 0;
int emulation_speed = 20;

/* =========================================================================
 * Logging
 * ========================================================================= */
#define CLSLOG_MAX (256 * 1024) /* keep the console log bounded */

char *clslog = NULL;
static size_t clslog_len, clslog_cap;

static void clslog_append(const char *text, size_t len)
{
	if (clslog_len + len + 2 > CLSLOG_MAX)
		clslog_len = 0; /* start over rather than grow forever */
	if (clslog_len + len + 2 > clslog_cap)
	{
		size_t cap = clslog_cap ? clslog_cap : 4096;
		while (cap < clslog_len + len + 2)
			cap *= 2;
		char *p = realloc(clslog, cap);
		if (!p)
			return;
		clslog = p;
		clslog_cap = cap;
	}
	memcpy(clslog + clslog_len, text, len);
	clslog_len += len;
	clslog[clslog_len++] = '\n';
	clslog[clslog_len] = '\0';
}

void log_printf(const char *fmt, ...)
{
	char buf[1024];
	size_t time_len = 0;
	time_t now = time(NULL);
	struct tm tm_now;
	int have_tm;
	va_list args;
	int n;
	size_t total_len;

	/* Thread-safe localtime; each toolchain spells it differently. */
#if defined(_MSC_VER)
	have_tm = (localtime_s(&tm_now, &now) == 0);
#elif defined(_WIN32)
	{
		/* MinGW: CRT localtime() is already per-thread. */
		struct tm *t = localtime(&now);
		have_tm = (t != NULL);
		if (have_tm)
			tm_now = *t;
	}
#else
	have_tm = (localtime_r(&now, &tm_now) != NULL);
#endif
	/* "HH:MM:SS: " prefix; log without it if the time is unavailable. */
	if (have_tm)
		time_len = strftime(buf, sizeof(buf), "%H:%M:%S: ", &tm_now);

	va_start(args, fmt);
	n = vsnprintf(buf + time_len, sizeof(buf) - time_len, fmt, args);
	va_end(args);
	if (n < 0)
		return;

	total_len = time_len + (size_t)n;
	if (total_len >= sizeof(buf))
		total_len = sizeof(buf) - 1;

	fputs(buf, stdout);
	fflush(stdout); /* keep the log when the game is killed or crashes */
#ifdef ANDROID
	__android_log_write(ANDROID_LOG_INFO, "FE2", buf);
#endif
	clslog_append(buf, total_len);
}

/* =========================================================================
 * Emulation control
 * ========================================================================= */
void Main_PauseEmulation(void)
{
	if (bEmulationActive)
	{
		Audio_EnableAudio(FALSE);
		bEmulationActive = FALSE;
	}
}

void Main_UnPauseEmulation(void)
{
	if (!bEmulationActive)
	{
		Audio_EnableAudio(1);
		bEmulationActive = TRUE;
	}
}

/* =========================================================================
 * Events
 * ========================================================================= */

/* Is the click inside (x1,y1)-(x2,y2) in 320x240 coordinates of the
 * letterboxed game area (window overlays like the cog)? */
static bool hit_region_clicked(const SDL_Event *event, int x1, int y1, int x2, int y2)
{
	int gw = Screen_GetGameWidth(), gh = Screen_GetGameHeight();
	if (gw <= 0 || gh <= 0)
		return false;
	int gx = 320 * (event->button.x - Screen_GetGameOffsetX()) / gw;
	int gy = 240 * (event->button.y - Screen_GetGameOffsetY()) / gh;
	return gx >= x1 && gx <= x2 && gy >= y1 && gy <= y2;
}

/* The same for what the game draws (also through the cockpit view) */
static bool hit_game_region_clicked(const SDL_Event *event, int x1, int y1, int x2, int y2)
{
	int gx, gy;
	if (!Screen_WindowToGame(event->button.x, event->button.y, &gx, &gy))
		return false;
	gy = gy * 240 / 200;
	return gx >= x1 && gx <= x2 && gy >= y1 && gy <= y2;
}

/* Work around the original game's system map left click glitch: the
 * system view icon sends F2 instead. */
static bool systemview_button(const SDL_Event *event)
{
	if (event->button.button != SDL_BUTTON_LEFT || !hit_game_region_clicked(event, 18, 226, 31, 240))
		return false;
	SDL_Keysym key = {.scancode = SDL_SCANCODE_F2, .sym = SDLK_F2};
	Keymap_KeyDown(&key);
	Keymap_KeyUp(&key);
	return true;
}

/* The small cog in the top left corner opens the menu */
static bool settings_button(const SDL_Event *event)
{
	if (event->button.button != SDL_BUTTON_LEFT || !hit_region_clicked(event, 0, 0, 10, 14))
		return false;
	toggle_m68k_menu = !toggle_m68k_menu;
	return true;
}

static bool ctrl_held(const SDL_Event *event)
{
	return (event->key.keysym.mod & (KMOD_LCTRL | KMOD_RCTRL)) != 0;
}

/* Reads SDL events and passes them to the menu, the touch controls or the
 * emulated keyboard / mouse. */
void Main_EventHandler(void)
{
	SDL_Event event;
	while (SDL_PollEvent(&event))
	{
		/* Ctrl-F toggles the menu even when the menu has focus */
		if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_f && ctrl_held(&event))
		{
			toggle_m68k_menu = !toggle_m68k_menu;
			continue;
		}
		if (ui_handle_event(&event))
			continue;
		/* ImGui wants window units; everything below lays out in pixels */
		Screen_MouseToPixels(&event);
		if (freecam_handle_event(&event))
			continue;
		if (gamepad_handle_event(&event))
			continue;

		if ((event.type == SDL_FINGERDOWN || event.type == SDL_FINGERUP || event.type == SDL_FINGERMOTION) &&
			!toggle_touch_controls)
			toggle_touch_controls = 1;

		if (toggle_touch_controls)
		{
			if (handle_touch_inputs(&event))
				continue; /* event consumed by touch UI — skip emulator mouse handling */
		}

		switch (event.type)
		{
		case SDL_QUIT:
			bQuitProgram = TRUE;
			SDL_Quit();
			exit(0);
			break;

		case SDL_WINDOWEVENT:
			if (event.window.event == SDL_WINDOWEVENT_RESIZED ||
				event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
			{
				Screen_SyncSize(); /* data1/data2 are window units, not pixels */
			}
			break;

		case SDL_MOUSEMOTION:
			/* Store raw physical pixels. input.c scales using letterbox dims. */
			input.motion_x += event.motion.xrel;
			input.motion_y += event.motion.yrel;
			input.abs_x = event.motion.x;
			input.abs_y = event.motion.y;
			break;

		case SDL_MOUSEBUTTONDOWN:
			input.abs_x = event.button.x;
			input.abs_y = event.button.y;
			if (!toggle_touch_controls && !toggle_m68k_menu && settings_button(&event))
				break;
			if (!systemview_button(&event))
				Input_MousePress(event.button.button);
			break;

		case SDL_MOUSEBUTTONUP:
			input.abs_x = event.button.x;
			input.abs_y = event.button.y;
			Input_MouseRelease(event.button.button);
			break;

		case SDL_KEYDOWN:
			if (event.key.keysym.sym == SDLK_m && ctrl_held(&event))
			{
				right_button_held = !right_button_held;
				if (right_button_held)
					Input_MousePress(SDL_BUTTON_RIGHT);
				else
					Input_MouseRelease(SDL_BUTTON_RIGHT);
			}
			Keymap_KeyDown(&event.key.keysym);
			break;

		case SDL_KEYUP:
			Keymap_KeyUp(&event.key.keysym);
			break;
		}
	}
	if (toggle_touch_controls)
		touch_input_tick();
	gamepad_update();
	settings_poll(); /* writes glfrontier.cfg if a setting changed */
}

/* =========================================================================
 * Start up / shut down
 * ========================================================================= */
static void read_parameters(int argc, char *argv[])
{
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
		{
			printf("Usage:\n frontier [options]\n"
				   "Where options are:\n"
				   "  --help or -h          Print this help text and exit.\n"
				   "  --fullscreen or -f    Try to use fullscreen mode.\n"
				   "  --nosound             Disable sound (faster!).\n"
				   "  --size w h            Start at specified window size.\n"
				   "  --vr                  Use a VR headset (OpenXR).\n");
			exit(0);
		}
		else if (!strcmp(argv[i], "--fullscreen") || !strcmp(argv[i], "-f"))
		{
			use_fullscreen = true;
		}
		else if (!strcmp(argv[i], "--vr"))
		{
			vr_enabled = true;
		}
		else if (!strcmp(argv[i], "--nosound"))
		{
			bDisableSound = TRUE;
		}
		else if (!strcmp(argv[i], "--size"))
		{
			screen_h = 0;
			if (++i < argc)
				screen_w = atoi(argv[i]);
			if (++i < argc)
				screen_h = atoi(argv[i]);
			if (screen_h == 0)
				screen_h = 5 * screen_w / 8; /* the game likes 16:10 */
		}
		else if (argv[i][0])
		{
			fprintf(stderr, "Illegal parameter: %s\n", argv[i]);
		}
	}
}

static void main_init(void)
{
#ifdef __EMSCRIPTEN__
	/* Persistent save directory, backed by IndexedDB */
	/* clang-format off */
	EM_ASM(
		FS.mkdir('/saves');
		FS.mount(IDBFS, {}, '/saves');
		FS.syncfs(true, function(err) {
			if (err)
				console.log('FS init error:', err);
			else
				console.log('Save data loaded from persistent storage');
		}););
	/* clang-format on */
#endif

	/* audio is initialised later; failing there is not fatal */
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0)
	{
		log_printf("Could not initialize the SDL library:\n %s\n", SDL_GetError());
		exit(-1);
	}

	Screen_Init();
	gamepad_init();
	Init680x0();
#if FE2_USE_MODDED
	custom_ships_install(); /* tools/fe2ShipBuilder/custom_ships/NNN.fe2m, see custom_ships.h */
#endif
	Audio_Init();
	Keymap_Init();

	if (bQuitProgram)
	{
		SDL_Quit();
		exit(-2);
	}
}

static void main_uninit(void)
{
	gamepad_shutdown();
	Audio_UnInit();
	Screen_UnInit();
	SDL_Quit();
}

/* The emulated VBL interrupt; drives the game at emulation_speed ms */
static Uint32 vbl_callback(Uint32 interval, void *param)
{
	(void)interval;
	(void)param;
	FlagException(0);
	return (Uint32)emulation_speed;
}

static void sig_handler(int signum)
{
	if (signum == SIGSEGV)
	{
		log_printf("Segfault! All is lost! Abandon ship!\n");
		Call_DumpDebug();
	}
}

int main(int argc, char *argv[])
{
	signal(SIGSEGV, sig_handler);
	srand((unsigned)time(NULL));
	read_parameters(argc, argv);
	log_printf("Logger started: %s\n", __TIME__);

	main_init();
	settings_load(); /* glfrontier.cfg: the settings from last time */
	if (use_fullscreen && !bInFullScreen)
		Screen_ToggleFullScreen();

	SDL_AddTimer(20, &vbl_callback, NULL);
	Main_UnPauseEmulation();
	Start680x0(); /* runs until the game quits */

	main_uninit();
	return 0;
}