/*
 * ui.h - the emulator menu (Dear ImGui), usable from C.
 *
 * The menu is toggled with toggle_m68k_menu (Ctrl-F or the cog icon).
 * Renderers call ui_init_* once, then ui_render() at the end of each frame;
 * the event loop offers every SDL event to ui_handle_event() first.
 */
#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <SDL.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Pass the window and its GL context */
void ui_init_gl(SDL_Window *window, SDL_GLContext context);
void ui_shutdown(void);

/* Returns true when the menu used the event and the game should not see
 * it (e.g. a click on a menu window). */
bool ui_handle_event(const SDL_Event *event);

/* Draws the menu, if it is open, on top of the current frame. */
void ui_render(void);
/* Whether ui_render has anything to draw (the menu, the galaxy atlas or
 * the free camera's help) */
bool ui_visible(void);

/* A VR headset shows the menu on a screen in the cockpit (gl_screen_place.h,
 * PLACE_MENU): the menu is open, and not the galaxy atlas over it */
bool ui_menu_open(void);
/* Draws the menu alone into the bound framebuffer, w x h pixels, laid out
 * as the window's game area would be (renderer.h's Screen_GetGame*), the
 * pointer being the hands' (cockpit_menu_pointer_get). */
void ui_render_vr(int w, int h);
/* The menu is on that screen: ui_render leaves it out of the window */
void ui_set_vr_menu(bool shown);
/* Drawing it there now (the menu leaves out what is no use in VR) */
bool ui_in_vr_menu(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_H */
