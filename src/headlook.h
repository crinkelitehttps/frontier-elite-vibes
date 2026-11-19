/*
 * headlook.h - a held look-around for the cockpit view, moved by the mouse
 * while Left Alt is held, or by the controller's right stick while the
 * right shoulder is held (gamepad.cpp: head-x / head-y / head-hold). The
 * pose stays where it is left; a double tap of the key or button puts it
 * back straight ahead. It adds to head tracking and the springing look-x /
 * look-y (gl_cockpit.c).
 */
#ifndef HEADLOOK_H
#define HEADLOOK_H

#include <SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Left Alt and the mouse while it is held; true if the event was taken */
bool headlook_event(const SDL_Event *event);
/* Turns the held pose: degrees, yaw right +, pitch up + */
void headlook_turn(float yaw, float pitch);
void headlook_center(void);
/* The held pose in degrees (yaw right +, pitch up +) */
void headlook_get(float *yaw, float *pitch);

/* Double-tap detection for a hold control: call each press / release with
 * the time in ms; true on the press that completes a double tap (a short
 * tap, then a press soon after it) */
typedef struct
{
	Uint64 down_at, tap_at;
	bool down, tapped;
} HeadlookTap;
bool headlook_tap(HeadlookTap *t, bool down, Uint64 now_ms);

#ifdef __cplusplus
}
#endif

#endif /* HEADLOOK_H */
