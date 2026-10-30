/*
 * quiz_buttons.h - buttons on the station's copy-protection quiz.
 *
 * Docking, the original game may ask for the letter at a page, line and word
 * of the manual (always P). Players with no keyboard (touch, gamepad, VR)
 * can't type it, so while the quiz waits two buttons sit in the bottom right
 * of the station screen, in its style (the list's raised square, white text
 * beside it): PRESS P, and PRESS OTHER KEY (a wrong letter, A). They
 * are painted into the screen's picture (gl_screen.c) and clicked through the
 * game's pointer (input.c), so they work on every display.
 */
#ifndef QUIZ_BUTTONS_H
#define QUIZ_BUTTONS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define QUIZ_BUTTONS 2

typedef struct
{
	int x0, y0, x1, y1; /* screen pixels, inclusive */
	const char *label;
	unsigned char scancode; /* the ST key it presses */
} QuizButton;

/* The buttons' places and labels (fixed) */
const QuizButton *quiz_buttons(void);

/* Draws them into a 320-wide ABGR picture, in the screen's palette `pal`
 * (the main one: the station's colours) */
void quiz_buttons_draw(unsigned int *pixels, const unsigned int *pal);

/* Game thread, at each pointer poll: the left button went down at (x, y)
 * in screen pixels. True if it was on a button (its key is pressed, and the
 * click is not the game's). */
bool quiz_buttons_click(int x, int y);

#ifdef __cplusplus
}
#endif

#endif /* QUIZ_BUTTONS_H */
