/*
 * shortcut.c - Ctrl+key emulator shortcuts:
 *   Ctrl-F11 fullscreen, Ctrl-M mouse grab, Ctrl-Q quit,
 *   Ctrl-D debug dump, Ctrl-E cycle renderer, Ctrl-K cockpit view on / off,
 *   Ctrl-O orbit display (conic, Ap, Pe) on / off,
 *   Ctrl-C recentre head tracking (and the VR headset),
 *   Ctrl-S write the game state as savs/SnapNN (loadable, see state_file.h).
 */
#include <SDL.h>

#include "gl/gl_cockpit.h"
#include "gl/gl_hud.h"
#include "headtrack.h"
#include "hostcall.h"
#include "renderer.h"
#include "shortcut.h"
#include "state_file.h"
#include "vr.h"

SHORTCUT_KEY ShortCutKey;

static void toggle_mouse_grab(void)
{
	bGrabMouse = !bGrabMouse;
	if (!bInFullScreen)
		SDL_SetRelativeMouseMode(bGrabMouse ? SDL_TRUE : SDL_FALSE);
}

void ShortCut_CheckKeys(void)
{
	switch (ShortCutKey.Key)
	{
	case SDLK_F11:
		Screen_ToggleFullScreen();
		break;
	case SDLK_m:
		toggle_mouse_grab();
		break;
	case SDLK_q:
		bQuitProgram = TRUE;
		Screen_StopRenderThread();
		SDL_Quit();
		exit(0);
	case SDLK_d:
		Call_DumpDebug();
		break;
	case SDLK_e:
		Screen_ToggleRenderer();
		break;
	case SDLK_k:
		cockpit_enabled = !cockpit_enabled;
		break;
	case SDLK_o:
		orbit_display = !orbit_display;
		break;
	case SDLK_c:
		headtrack_recenter();
		vr_recenter();
		break;
	case SDLK_s:
	{
		char name[32];
		state_file_snapshot(name, sizeof(name));
		break;
	}
	default:
		break;
	}
	memset(&ShortCutKey, 0, sizeof(ShortCutKey));
}
