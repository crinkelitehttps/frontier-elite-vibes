/*
 * spawn_log.h - GLF_SPAWN_LOG=1 logs scenery (terrain and cloud) objects as the game makes and removes them,
 * and every change of the time acceleration, to the log with a "SPAWN" prefix.
 *
 * Each line carries the game time since the log started (game seconds; "real" is wall-clock seconds), the
 * time acceleration, and the player's speed, altitude, position, heading (the ship's nose and its velocity,
 * as pitch above the local horizon and an azimuth about the local up) and where the object is relative to the
 * ship. The objects are seen by polling the object table once per game frame, so a spawn and removal inside
 * one frame is not seen, and under time acceleration several game ticks may pass between two looks.
 */
#ifndef SPAWN_LOG_H
#define SPAWN_LOG_H

#ifdef __cplusplus
extern "C"
{
#endif

/* Game thread, from the host update */
void spawn_log_update(void);

#ifdef __cplusplus
}
#endif

#endif
