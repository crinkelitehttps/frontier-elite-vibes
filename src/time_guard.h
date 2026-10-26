/*
 * time_guard.h - steps time acceleration down as a ship closes on the player.
 *
 * The game's AI decides once per game second, batched: at 10000x one decision covers 200 seconds of
 * flight, so a pirate crosses the last thousands of km without a decision and the game's own
 * shortcut places it a few km away ("ships out of nowhere", docs/time-acceleration-and-ship-spawning.md).
 * Every game frame this looks at each ship that is flying (not parked, docked or waiting to
 * arrive): its distance to the player, how fast that fell since the last frame, and so how long it
 * would take to come within 7 km. When that time, in real seconds at the present rate, falls to a
 * tenth of time_guard_seconds, the rate goes down one step (10000x, 1000x, 100x, 10x, 1x) by the
 * game's own Shift+F key, which makes the time left ten times longer: each speed shows the
 * approach for about time_guard_seconds. At 7 km the game's own warning takes it from there.
 *
 * Off with 0, and while landed, docked, in hyperspace or out of the flight view. A ship that
 * is put close at once (the game's shortcut, or too fast a rate for one frame) is logged on the SHIP
 * screen's debug page (debug_screen.h).
 */
#ifndef TIME_GUARD_H
#define TIME_GUARD_H

#ifdef __cplusplus
extern "C"
{
#endif

extern int time_guard_seconds; /* 0 = off; a setting */

/* Game thread, from the host update; does its work once per game frame */
void time_guard_update(void);

#ifdef __cplusplus
}
#endif

#endif
