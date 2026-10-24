/*
 * ui.cpp - Dear ImGui setup: SDL2 input and the OpenGL 3 (GLES 3 on
 * Android and the web) drawing backend. The menu itself is in ui_menu.cpp.
 */
#include <SDL.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"

#include "../galaxy/map_view.h"
#include "ui.h"
#include "ui_galaxy.h"
#include "ui_menu.h"
#include "ui_style.h"

extern "C"
{
#include "freecam.h"
#include "gl/gl_cockpit.h"
#include "main.h"
#include "renderer.h"
}

static bool initialised;

static void init_common(void)
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr; /* nothing worth persisting */
	io.LogFilename = nullptr;
	/* font and style are built on the first frame (ui_style.cpp) */
}

/* The menu is drawn in game pixels: rebuild the pixel font and style
 * whenever the size of one game pixel on screen changes. */
static void update_style(void)
{
	int px = (Screen_GetGameHeight() + 120) / 240;
	if (!ui_style_update(px))
		return;
	ImGui_ImplOpenGL3_DestroyFontsTexture();
	ImGui_ImplOpenGL3_CreateFontsTexture();
}

extern "C" void ui_init_gl(SDL_Window *window, SDL_GLContext context)
{
	init_common();
	ImGui_ImplSDL2_InitForOpenGL(window, context);
#if defined(__EMSCRIPTEN__) || defined(ANDROID)
	ImGui_ImplOpenGL3_Init("#version 300 es");
#else
	ImGui_ImplOpenGL3_Init("#version 330 core");
#endif
	initialised = true;
}

extern "C" void ui_shutdown(void)
{
	if (!initialised)
		return;
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplSDL2_Shutdown();
	ImGui::DestroyContext();
	initialised = false;
}

extern "C" bool ui_handle_event(const SDL_Event *event)
{
	/* fingers for pinch zoom in the atlas (touch_finger, map_view.h), always,
	   so a finger lifted while the atlas is closed is not left behind. SDL
	   gives them 0..1 across the window, ImGui works in window units */
	if (initialised &&
		(event->type == SDL_FINGERDOWN || event->type == SDL_FINGERUP || event->type == SDL_FINGERMOTION))
	{
		const ImVec2 &ds = ImGui::GetIO().DisplaySize;
		TouchFingerEvent what = event->type == SDL_FINGERDOWN ? TOUCH_DOWN
								: event->type == SDL_FINGERUP ? TOUCH_UP
															  : TOUCH_MOVE;
		touch_finger((long long)event->tfinger.fingerId, what, event->tfinger.x * ds.x, event->tfinger.y * ds.y);
	}

	/* While the menu is closed ImGui gets nothing: its input queue is only
	 * drained by a frame, and there are no frames when it is hidden. */
	if (!initialised || (!toggle_m68k_menu && !ui_galaxy_open()))
		return false;

	/* ImGui works in drawable pixels, like the rest of the game (see
	   ui_render): mouse events come in window units, which are bigger on
	   high DPI screens (phone browsers, Retina) */
	SDL_Event scaled = *event;
	Screen_MouseToPixels(&scaled);
	ImGui_ImplSDL2_ProcessEvent(&scaled);

	/* the galaxy atlas covers the (paused) game: it has all the input */
	if (ui_galaxy_open())
		switch (event->type)
		{
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
		case SDL_MOUSEWHEEL:
		case SDL_MOUSEMOTION:
		case SDL_KEYDOWN:
		case SDL_KEYUP:
		case SDL_TEXTINPUT:
		case SDL_FINGERDOWN:
		case SDL_FINGERUP:
		case SDL_FINGERMOTION:
			return true;
		default:
			break;
		}

	const ImGuiIO &io = ImGui::GetIO();
	switch (event->type)
	{
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP:
	case SDL_MOUSEWHEEL:
	case SDL_MOUSEMOTION:
		return io.WantCaptureMouse;
	case SDL_KEYDOWN:
	case SDL_KEYUP:
	case SDL_TEXTINPUT:
		return io.WantCaptureKeyboard;
	default:
		return false;
	}
}

/* Touch screens: drag a panel to scroll it, as on a phone, instead of
 * hunting for the scroll bar. The drag goes to the nearest scrollable
 * window under the finger. A drag that starts on a button scrolls (and the
 * button is not pressed); widgets that drag themselves are left alone:
 * sliders and text fields (they edit their value) and the atlas map / orbit
 * view (touch_scroll_block). Runs after the frame's widgets. */
static void touch_drag_scroll(void)
{
	static ImGuiWindow *target;
	static bool scrolling;
	ImGuiContext &g = *GImGui;
	ImGuiIO &io = g.IO;
	bool block = touch_scroll_block;
	touch_scroll_block = false;

	if (!io.MouseDown[ImGuiMouseButton_Left] || io.MouseSource != ImGuiMouseSource_TouchScreen)
	{
		target = nullptr;
		scrolling = false;
		return;
	}
	if (io.MouseClicked[ImGuiMouseButton_Left])
	{
		target = nullptr;
		scrolling = false;
		for (ImGuiWindow *w = g.HoveredWindow; w; w = w->ParentWindow)
			if (w->ScrollMax.y > 0 || w->ScrollMax.x > 0)
			{
				target = w;
				break;
			}
		return;
	}
	if (!target)
		return;
	if (!scrolling)
	{
		float threshold = io.MouseDragThreshold * 2;
		if (block || g.ActiveIdHasBeenEditedBefore ||
			io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < threshold * threshold)
			return;
		scrolling = true;
		ImGui::ClearActiveID(); /* the button the drag started on is not pressed */
	}
	if (target->ScrollMax.y > 0)
		ImGui::SetScrollY(target, target->Scroll.y - io.MouseDelta.y);
	if (target->ScrollMax.x > 0)
		ImGui::SetScrollX(target, target->Scroll.x - io.MouseDelta.x);
}

extern "C" bool ui_visible(void)
{
	return initialised && (toggle_m68k_menu || ui_galaxy_open() || (freecam_active() && freecam_show_help));
}

static bool vr_menu_shown, in_vr_menu;

extern "C" bool ui_menu_open(void)
{
	return initialised && toggle_m68k_menu && !ui_galaxy_open();
}

extern "C" void ui_set_vr_menu(bool shown)
{
	vr_menu_shown = shown;
}

extern "C" bool ui_in_vr_menu(void)
{
	return in_vr_menu;
}

/* The hands' pointer as a touch screen's: the menu scrolls by dragging */
extern "C" void ui_render_vr(int w, int h)
{
	if (!ui_menu_open())
		return;
	update_style();
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplSDL2_NewFrame();
	ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize = ImVec2((float)w, (float)h);
	io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
	io.ConfigInputTrickleEventQueue = false; /* a position and a press in one frame */
	float u, v;
	bool down;
	/* (after the window's mouse, which the backend has just queued) */
	io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
	if (cockpit_menu_pointer_get(&u, &v, &down))
	{
		io.AddMousePosEvent(u * (float)w, v * (float)h);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
	}
	else
	{
		io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	}
	ImGui::NewFrame();
	in_vr_menu = true;
	ui_menu_draw();
	in_vr_menu = false;
	touch_drag_scroll();
	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

extern "C" void ui_render(void)
{
	/* (no frame of the window's between the menu's: a widget being dragged
	 * would be lost to a frame that doesn't have it) */
	if (!ui_visible() || vr_menu_shown)
		return;

	update_style();
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplSDL2_NewFrame();
	/* the menu lays out in drawable pixels (game_pos, the pixel font), so
	   ImGui's display is the drawable, not the window (they differ on high
	   DPI screens: a phone browser's devicePixelRatio, Retina) */
	ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize = ImVec2((float)screen_w, (float)screen_h);
	io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
	ImGui::NewFrame();

	if (ui_galaxy_open())
		ui_galaxy_draw();
	else if (toggle_m68k_menu && !vr_menu_shown)
		ui_menu_draw();
	if (freecam_active() && freecam_show_help)
		ui_freecam_overlay();
	touch_drag_scroll();

	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
	touch_pinch_end_frame();
}
