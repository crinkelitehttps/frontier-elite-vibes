/*
 * keymap.h - SDL key events to Atari ST scancodes.
 */
#ifndef KEYMAP_H
#define KEYMAP_H

#include <SDL_keyboard.h>

#ifdef __cplusplus
extern "C"
{
#endif

void Keymap_Init(void);
void Keymap_KeyDown(SDL_Keysym *sdlkey);
void Keymap_KeyUp(SDL_Keysym *sdlkey);
/* The flight keys (keymap.c): J / L held roll (1), U / O yaw (2), else 0 */
int Keymap_SteerMode(void);
/* W / A / S / D held: strafe, -1..1 each (x right +, y up +) */
void Keymap_Strafe(float *x, float *y);
/* Called by the game every frame; nothing to do with SDL key events */
void Keymap_DebounceAllKeys(void);

#ifdef __cplusplus
}
#endif

#endif /* KEYMAP_H */
