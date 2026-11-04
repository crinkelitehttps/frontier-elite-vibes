/*
 * freecam.h - free camera: pauses the game and flies a camera around the
 * player's ship (the Lfreecam_ mod in fe2/fe2_modded.s).
 *
 * Keys: Ctrl-V on / off, Esc off, W/S forward / back, A/D left / right,
 * R/F up / down, arrows (or mouse with a button held) look, Q/E roll,
 * +/- or mouse wheel speed, Shift fast, H hide / show the help box,
 * 0 (or P) hold the game, 1-5 let it run at the game's rates 1x-10000x.
 *
 * Touch controls (when they are on): drag on the left half moves (a
 * thumbstick that starts where you press), drag on the right half looks,
 * buttons for up / down, roll, speed, help and exit. Like the other touch
 * controls they are pressed with the mouse (SDL's mouse is also the first
 * finger), so they work with a mouse too; a second finger can look around
 * while the first holds the stick.
 */
#ifndef FREECAM_H
#define FREECAM_H

#include <SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

bool freecam_active(void);
/* Only works in the flight view; returns whether it is now on */
bool freecam_toggle(void);
/* Takes the event if it is free camera input; call before the game sees it */
bool freecam_handle_event(const SDL_Event *event);
/* Once per tick: moves the camera and hands it to the game */
void freecam_update(void);

/* speed multiplier as a power of two, for the overlay */
int freecam_speed_level(void);
/* 0 the game is held, 1-5 it runs at its rates 1x .. 10000x (keys 0-5) */
int freecam_time_level(void);
extern bool freecam_invert_y;
/* The help box while the free camera is on (H) */
extern bool freecam_show_help;

/* Touch controls, in window pixels, for touch_overlay.c to draw */
typedef struct
{
	float x, y, w, h;
	bool held;
} FreecamButton;

enum
{
	FC_SPEED_DOWN,
	FC_SPEED_UP,
	FC_HELP,
	FC_EXIT,
	FC_UP,
	FC_DOWN,
	FC_ROLL_LEFT,
	FC_ROLL_RIGHT,
	FC_BUTTONS
};

typedef struct
{
	FreecamButton button[FC_BUTTONS];
	bool stick_active;
	float stick_x, stick_y; /* where the finger went down */
	float knob_x, knob_y;
	float stick_radius;
	float speed_fraction; /* 0 slowest .. 1 fastest */
} FreecamTouchView;

/* Fills in the touch controls; false when they are not shown */
bool freecam_touch_view(FreecamTouchView *view);

#ifdef __cplusplus
}
#endif

#endif /* FREECAM_H */
