/*
 * gamepad.h - game controllers (SDL GameController: Xbox, PlayStation,
 * Switch and most others), mapped by a binding file onto the game's
 * steering, look-around in the cockpit and keyboard keys.
 *
 * The bindings come from gamepad.cfg in the working directory (beside
 * glfrontier.cfg); it is written with the built-in default (gamepad.c,
 * which documents the format) the first time the game runs without one.
 *
 * A VR headset's motion controllers are a second, virtual pad on the same
 * bindings (gamepad_set_virtual).
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

/* A pad made of other controls (a VR headset's motion controllers,
 * gl_laser.c) in the Xbox layout, read with the real one through the same
 * bindings. Any thread; a button pressed since the last update counts as
 * down for one. */
typedef struct
{
	bool active;
	unsigned buttons;                     /* bit n: SDL_GameControllerButton n */
	float axes[SDL_CONTROLLER_AXIS_MAX]; /* -1..1, SDL's directions (y down +), triggers 0..1 */
} GamepadVirtual;
void gamepad_set_virtual(const GamepadVirtual *v);

#ifdef __cplusplus
}
#endif

#endif /* GAMEPAD_H */
