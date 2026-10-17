/*
 * settings.c - see settings.h.
 *
 * The file is plain "key=value" lines, unknown keys ignored, so it survives
 * settings being added or removed. Where it lives:
 *   desktop   glfrontier.cfg in the working directory (beside the saves)
 *   Android   the app's internal storage
 *   web       not kept yet (the save folder is only ready asynchronously)
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "freecam.h"
#include "gl/gl_cockpit.h"
#include "gl/gl_hands.h"
#include "gl/gl_hud.h"
#include "gl/gl_mapscreen.h"
#include "gl/gl_screen_place.h"
#include "main.h"
#include "manual.h"
#include "mods.h"
#include "renderer.h"
#include "settings.h"
#include "touch_input.h"
#include "vr.h"

#define FILE_NAME "glfrontier.cfg"

typedef struct
{
	int keep_aspect;
	int renderer;
	int fullscreen;
	int freecam_invert_y;
	int freecam_show_help;
	int vanilla_saves;
	int emulation_speed;
	int touch_pads_hidden;
	int cockpit;
	int cockpit_screen;
	int cockpit_map_live;
	int cockpit_ship_live;
	int cockpit_fov;
	int own_hud;
	int headtrack;
	int headtrack_port;
	int vr;
	int vr_resolution;
	int vr_depth;
	int vr_hands;
	int vr_panel_distance;
	int vr_screen_distance;
	int vr_reach;
	int vr_finger;
	int vr_near;
	int manual_page;
	int manual_book;
	ScreenPlace screen_place[PLACE_COUNT];
} Settings;

static const struct
{
	const char *key;
	size_t offset;
} fields[] = {
	{"keep_aspect", offsetof(Settings, keep_aspect)},
	{"renderer", offsetof(Settings, renderer)},
	{"fullscreen", offsetof(Settings, fullscreen)},
	{"freecam_invert_y", offsetof(Settings, freecam_invert_y)},
	{"freecam_show_help", offsetof(Settings, freecam_show_help)},
	{"vanilla_saves", offsetof(Settings, vanilla_saves)},
	{"emulation_speed", offsetof(Settings, emulation_speed)},
	{"touch_pads_hidden", offsetof(Settings, touch_pads_hidden)},
	{"cockpit", offsetof(Settings, cockpit)},
	{"cockpit_screen", offsetof(Settings, cockpit_screen)},
	{"cockpit_map_live", offsetof(Settings, cockpit_map_live)},
	{"cockpit_ship_live", offsetof(Settings, cockpit_ship_live)},
	{"cockpit_fov", offsetof(Settings, cockpit_fov)},
	{"own_hud", offsetof(Settings, own_hud)},
	{"headtrack", offsetof(Settings, headtrack)},
	{"headtrack_port", offsetof(Settings, headtrack_port)},
	{"vr", offsetof(Settings, vr)},
	{"vr_resolution", offsetof(Settings, vr_resolution)},
	{"vr_depth", offsetof(Settings, vr_depth)},
	{"vr_hands", offsetof(Settings, vr_hands)},
	{"vr_panel_distance", offsetof(Settings, vr_panel_distance)},
	{"vr_screen_distance", offsetof(Settings, vr_screen_distance)},
	{"vr_reach", offsetof(Settings, vr_reach)},
	{"vr_finger", offsetof(Settings, vr_finger)},
	{"vr_near", offsetof(Settings, vr_near)},
	{"manual_page", offsetof(Settings, manual_page)},
	{"manual_book", offsetof(Settings, manual_book)},
	{"screen_place_screen_on", offsetof(Settings, screen_place[0].on)},
	{"screen_place_screen_x", offsetof(Settings, screen_place[0].x)},
	{"screen_place_screen_y", offsetof(Settings, screen_place[0].y)},
	{"screen_place_screen_z", offsetof(Settings, screen_place[0].z)},
	{"screen_place_screen_qx", offsetof(Settings, screen_place[0].qx)},
	{"screen_place_screen_qy", offsetof(Settings, screen_place[0].qy)},
	{"screen_place_screen_qz", offsetof(Settings, screen_place[0].qz)},
	{"screen_place_screen_qw", offsetof(Settings, screen_place[0].qw)},
	{"screen_place_screen_w", offsetof(Settings, screen_place[0].w)},
	{"screen_place_camera_on", offsetof(Settings, screen_place[1].on)},
	{"screen_place_camera_x", offsetof(Settings, screen_place[1].x)},
	{"screen_place_camera_y", offsetof(Settings, screen_place[1].y)},
	{"screen_place_camera_z", offsetof(Settings, screen_place[1].z)},
	{"screen_place_camera_qx", offsetof(Settings, screen_place[1].qx)},
	{"screen_place_camera_qy", offsetof(Settings, screen_place[1].qy)},
	{"screen_place_camera_qz", offsetof(Settings, screen_place[1].qz)},
	{"screen_place_camera_qw", offsetof(Settings, screen_place[1].qw)},
	{"screen_place_camera_w", offsetof(Settings, screen_place[1].w)},
	{"screen_place_ship_on", offsetof(Settings, screen_place[2].on)},
	{"screen_place_ship_x", offsetof(Settings, screen_place[2].x)},
	{"screen_place_ship_y", offsetof(Settings, screen_place[2].y)},
	{"screen_place_ship_z", offsetof(Settings, screen_place[2].z)},
	{"screen_place_ship_qx", offsetof(Settings, screen_place[2].qx)},
	{"screen_place_ship_qy", offsetof(Settings, screen_place[2].qy)},
	{"screen_place_ship_qz", offsetof(Settings, screen_place[2].qz)},
	{"screen_place_ship_qw", offsetof(Settings, screen_place[2].qw)},
	{"screen_place_ship_w", offsetof(Settings, screen_place[2].w)},
	{"screen_place_coms_on", offsetof(Settings, screen_place[3].on)},
	{"screen_place_coms_x", offsetof(Settings, screen_place[3].x)},
	{"screen_place_coms_y", offsetof(Settings, screen_place[3].y)},
	{"screen_place_coms_z", offsetof(Settings, screen_place[3].z)},
	{"screen_place_coms_qx", offsetof(Settings, screen_place[3].qx)},
	{"screen_place_coms_qy", offsetof(Settings, screen_place[3].qy)},
	{"screen_place_coms_qz", offsetof(Settings, screen_place[3].qz)},
	{"screen_place_coms_qw", offsetof(Settings, screen_place[3].qw)},
	{"screen_place_coms_w", offsetof(Settings, screen_place[3].w)},
};
#define N_FIELDS ((int)(sizeof(fields) / sizeof(fields[0])))

static Settings saved;
static bool loaded;
int settings_vr_arg = -1;
static int file_vr; /* the file's "vr", kept while --vr/--novr overrides it */

#ifndef __EMSCRIPTEN__
static const char *file_path(void)
{
	static char path[512];
#ifdef ANDROID
	snprintf(path, sizeof(path), "%s/%s", SDL_AndroidGetInternalStoragePath(), FILE_NAME);
#else
	snprintf(path, sizeof(path), "%s", FILE_NAME);
#endif
	return path;
}
#endif

static int *field(Settings *s, int i)
{
	return (int *)((char *)s + fields[i].offset);
}

static Settings current(void)
{
	Settings s;
	s.keep_aspect = Screen_GetLetterboxMode();
	s.renderer = (int)use_renderer;
	s.fullscreen = bInFullScreen ? 1 : 0;
	s.freecam_invert_y = freecam_invert_y;
	s.freecam_show_help = freecam_show_help;
	s.vanilla_saves = mods_vanilla_saves;
	s.emulation_speed = emulation_speed;
	s.touch_pads_hidden = touch_pads_hidden();
	s.cockpit = cockpit_enabled;
	s.cockpit_screen = cockpit_screen;
	s.cockpit_map_live = cockpit_map_live;
	s.cockpit_ship_live = cockpit_ship_live;
	s.cockpit_fov = cockpit_fov;
	s.own_hud = own_hud;
	s.headtrack = headtrack_enabled;
	s.headtrack_port = headtrack_port;
	s.vr = settings_vr_arg >= 0 ? file_vr : vr_enabled;
	s.vr_resolution = vr_resolution;
	s.vr_depth = vr_depth;
	s.vr_hands = vr_hands;
	s.vr_panel_distance = vr_panel_distance;
	s.vr_screen_distance = vr_screen_distance;
	s.vr_reach = vr_reach;
	s.vr_finger = vr_finger;
	s.vr_near = vr_near_cm;
	s.manual_page = manual_page();
	s.manual_book = manual_book;
	memcpy(s.screen_place, screen_place, sizeof s.screen_place);
	return s;
}

static void apply(const Settings *s)
{
	if (s->keep_aspect != Screen_GetLetterboxMode())
		Screen_SetLetterboxMode(s->keep_aspect != 0);
	if (s->renderer >= 0 && s->renderer < R_MAX)
		use_renderer = (enum RENDERERS)s->renderer;
#if !defined(ANDROID)
	if ((s->fullscreen != 0) != (bInFullScreen != 0))
		Screen_ToggleFullScreen();
#endif
	freecam_invert_y = s->freecam_invert_y != 0;
	freecam_show_help = s->freecam_show_help != 0;
	mods_vanilla_saves = s->vanilla_saves != 0;
	if (s->emulation_speed >= 1 && s->emulation_speed <= 100)
		emulation_speed = s->emulation_speed;
	touch_set_pads_hidden(s->touch_pads_hidden != 0);
	cockpit_enabled = s->cockpit != 0;
	cockpit_screen = s->cockpit_screen != 0;
	cockpit_map_live = s->cockpit_map_live != 0;
	cockpit_ship_live = s->cockpit_ship_live != 0;
	if (s->cockpit_fov >= 30 && s->cockpit_fov <= 120)
		cockpit_fov = s->cockpit_fov;
	own_hud = s->own_hud != 0;
	headtrack_enabled = s->headtrack != 0;
	if (s->headtrack_port > 0 && s->headtrack_port < 65536)
		headtrack_port = s->headtrack_port;
	file_vr = s->vr;
	vr_enabled = settings_vr_arg >= 0 ? settings_vr_arg != 0 : s->vr != 0;
	if (s->vr_resolution >= 25 && s->vr_resolution <= 200)
		vr_resolution = s->vr_resolution;
	if (s->vr_depth >= 0 && s->vr_depth <= 1000)
		vr_depth = s->vr_depth;
	vr_hands = s->vr_hands != 0;
	if (s->vr_panel_distance == 0 ||
		(s->vr_panel_distance >= VR_DISTANCE_MIN && s->vr_panel_distance <= VR_DISTANCE_MAX))
		vr_panel_distance = s->vr_panel_distance;
	if (s->vr_screen_distance >= VR_DISTANCE_MIN && s->vr_screen_distance <= VR_DISTANCE_MAX)
		vr_screen_distance = s->vr_screen_distance;
	if (s->vr_reach >= 50 && s->vr_reach <= 300)
		vr_reach = s->vr_reach;
	if (s->vr_finger >= 0 && s->vr_finger <= 20)
		vr_finger = s->vr_finger;
	if (s->vr_near >= 1 && s->vr_near <= 30)
		vr_near_cm = s->vr_near;
	manual_book = s->manual_book != 0; /* (takes effect at manual_init: the next start) */
	if (s->manual_page != manual_page())
		manual_set_page(s->manual_page);
	memcpy(screen_place, s->screen_place, sizeof screen_place);
}

void settings_load(void)
{
	Settings s = current(); /* defaults for anything the file lacks */
	loaded = true;
#ifndef __EMSCRIPTEN__
	FILE *f = fopen(file_path(), "r");
	if (f)
	{
		char line[128];
		while (fgets(line, sizeof(line), f))
		{
			char *eq = strchr(line, '=');
			if (!eq)
				continue;
			*eq = 0;
			for (int i = 0; i < N_FIELDS; i++)
				if (!strcmp(line, fields[i].key))
					*field(&s, i) = atoi(eq + 1);
		}
		fclose(f);
		log_printf("Settings loaded from %s\n", file_path());
	}
#endif
	apply(&s);
	saved = current();
}

void settings_poll(void)
{
	if (!loaded)
		return;
	Settings now = current();
	if (!memcmp(&now, &saved, sizeof(now)))
		return;
	saved = now;
#ifndef __EMSCRIPTEN__
	FILE *f = fopen(file_path(), "w");
	if (!f)
	{
		log_printf("Could not write settings to %s\n", file_path());
		return;
	}
	for (int i = 0; i < N_FIELDS; i++)
		fprintf(f, "%s=%d\n", fields[i].key, *field(&now, i));
	fclose(f);
#endif
}
