/*
 * strafe.h - sideways and vertical thrust for the player's ship.
 *
 * Every ship has x and y thrusters as strong as its retros, but the game
 * only fires them itself (the flight computer cancelling drift, lift-off).
 * Hooks in fe2.s (Lstrafe_speed, Lstrafe_manual) ask the host for the
 * player's own: in manual mode it adds to the main / retro thrust, in set
 * speed mode it overrides the flight computer on the axes being strafed
 * along (which takes the drift out again once they are let go).
 *
 * Input: W / A / S / D in flight (keymap.c), the keypad (4 / 6 left / right, 8 / 2 up / down, whatever
 * NumLock) and the gamepad's strafe bindings (gamepad.c). The amount is
 * a fraction of the ship's thrust on that axis.
 */
#ifndef STRAFE_H
#define STRAFE_H

#ifdef __cplusplus
extern "C"
{
#endif

/* The gamepad's strafe, -1..1 each (x right +, y up +), once per frame */
void strafe_set_pad(float x, float y);

/* Host call (hook in fe2.s): a0 = the player's ship, d0-d2 = the thrust
 * about to be set; replaces d0 / d1 on the axes being strafed along */
void Call_Strafe(void);

#ifdef __cplusplus
}
#endif

#endif /* STRAFE_H */
