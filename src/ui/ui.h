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

#ifdef __cplusplus
}
#endif

#endif /* UI_H */
