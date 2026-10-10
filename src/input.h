/*
 * input.h - keyboard and mouse state handed to the emulated game.
 */
#ifndef INPUT_H
#define INPUT_H

#include "main.h"

#define SIZE_KEYBUF 16

typedef struct
{
	unsigned char key_buf[SIZE_KEYBUF]; /* ST scancodes waiting for the game */
	int buf_head, buf_tail;
	int cur_mousebut_state; /* bit 0 right, bit 1 left (ST order) */

	int motion_x, motion_y; /* mouse movement since the game last polled */
	int abs_x, abs_y;       /* pointer position, window pixels */
} CINPUT;

extern CINPUT input;

/* Queue an ST key press / release */
void Input_PressSTKey(unsigned char scancode, BOOL press);
void Input_MousePress(int sdl_button);
void Input_MouseRelease(int sdl_button);

/* The gamepad's pointer (gamepad.c). While it is on, the game's pointer is
 * here (320x200 screen pixels) rather than where the mouse is, and the
 * host draws it (gl_screen.c). */
void Input_PadPointerBegin(void); /* starts where the game's pointer is */
void Input_PadPointerMove(float dx, float dy);
void Input_PadPointerClick(void); /* clicks the left button there, then ends */
bool Input_PadPointer(int *x, int *y); /* on: where it is */

/* The VR laser pointer (gl_laser.c). Set by the drawing, any thread:
 * controllers `active`, and if one `hit`s the screen, where (320x200
 * screen pixels); its buttons (bit 0 right, bit 1 left). Once it moves or
 * clicks it has the game's pointer, until the mouse moves. */
void Input_SetLaser(bool active, bool hit, float x, float y, int buttons);
bool Input_LaserPointer(int *x, int *y); /* it has the pointer: where */
void Input_MouseMoved(void);             /* the mouse moved (main.c) */

/* Host calls: the game polls the mouse and keyboard */
void Call_GetMouseInput(void);
void Call_GetKeyboardEvent(void);

#endif /* INPUT_H */
