/*
 * ui_menu.h - the emulator menu windows (ImGui, called inside a frame).
 */
#ifndef UI_MENU_H
#define UI_MENU_H

void ui_menu_draw(void);

/* Tab / Shift-Tab, pad Y / X: the keyboard / pad focus goes to the menu's other window (the side
 * window: console, more cheats, ship log), on the next frame */
void ui_menu_switch_window(void);

/* Help and speed shown while the free camera is on */
void ui_freecam_overlay(void);

#endif /* UI_MENU_H */
