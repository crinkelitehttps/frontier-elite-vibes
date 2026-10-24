/*
 * netplay.h - two players in the same place (an experiment, race mode).
 *
 * Each game stays the master of its own world. Every instance sends the
 * pose of the player's ship (the game's object record: rotation matrix,
 * position, speed, model, shields) over UDP, and shows the other player's
 * as a "ghost": a record in a free object slot that the game draws like any
 * ship but never moves (no physics flag), the host writing the pose into it.
 * Both games must start from the same save in the same place (e.g. Lave
 * station), at normal time speed.
 *
 *   --mp-listen PORT       wait for the other player on UDP PORT
 *   --mp-connect HOST:PORT send to the other player (who is listening)
 */
#ifndef NETPLAY_H
#define NETPLAY_H

#include <stdbool.h>

extern bool netplay_enabled;

/* Command line: returns false if the address can't be used */
bool netplay_listen(int port);
bool netplay_connect(const char *host_port);

/* Game thread, once per drawn frame (Nu_DrawScreen): sends our ship, and
 * puts the other player's ghost into the game's objects */
void netplay_tick(void);

/* hcalls from the game's hooks (fe2.s: Lnet_damage, Lnet_physics) */
void Call_NetDamage(void);
void Call_NetPending(void);

#endif
