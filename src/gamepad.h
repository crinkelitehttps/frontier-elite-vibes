/*
 * gamepad.h - game controllers (SDL GameController: Xbox, PlayStation,
 * Switch and most others), mapped by a binding file onto the game's
 * steering, look-around in the cockpit and keyboard keys.
 *
 * The bindings are vibe-lib's (vibe/bindings.h) and come from gamepad.cfg in
 * the working directory (beside glfrontier.cfg); it is written with the
 * built-in default (gamepad.cpp, which documents the format) the first time
 * the game runs without one. A file in the earlier format is converted
 * (gamepad_legacy.h) and the old one kept as gamepad.cfg.old.
 *
 * A VR headset's motion controllers are read on the same bindings
 * (gamepad_set_vr).
 *
 * The `manual` layer acts while the cockpit clipboard is out
 * (gl_clipboard.h), in place of its controls' other bindings.
 *
 * Analog steering goes straight into the game's keyboard / joystick
 * steering words (A6+14508 x, A6+14510 y: what A/Z and ,/. ramp up), so
 * the stick's position is the turn rate.
 */
#ifndef GAMEPAD_H
#define GAMEPAD_H

#include <SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define GAMEPAD_FILE "gamepad.cfg"

/* Starts the controller subsystem and loads the bindings */
void gamepad_init(void);
void gamepad_shutdown(void);
/* Opens / closes controllers; true if the event was a controller event */
bool gamepad_handle_event(const SDL_Event *event);
/* Once per emulated frame (vblank): reads the controller, sends keys and
 * writes the steering into the game */
void gamepad_update(void);

/* Look-around from the controller, degrees (x right +, y down +) */
void gamepad_look(float *x, float *y);
/* A controller is connected */
bool gamepad_connected(void);

/* A VR headset's motion controllers (gl_laser.c), per hand (0 left, 1
 * right), read with the real pad through the same bindings. Any thread; a
 * button pressed since the last update counts as down for one. */
typedef struct
{
	bool active[2];     /* on, with the input focus */
	float trigger[2];   /* 0..1; 0 where the trigger is a mouse click */
	bool squeeze[2];    /* the grip button */
	bool menu[2];
	bool lower[2], upper[2]; /* face buttons (Touch, Index): A / X, B / Y */
	bool stick_click[2];
	bool pad_click[2];
	float stick[2][2];  /* thumbstick, -1..1, x right +, y down + */
	float pad[2][2];    /* trackpad, as the stick */
} GamepadVr;
void gamepad_set_vr(const GamepadVr *v);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
namespace vibe
{
class Bindings;
}
/* The loaded bindings (the clipboard's controls pages, manual.cpp) */
vibe::Bindings &gamepad_bindings();
#endif

#endif /* GAMEPAD_H */
