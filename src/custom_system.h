/*
 * custom_system.h - hand-made star systems that replace generated ones.
 *
 * The game builds every system's bodies (stars, planets, moons, starports)
 * with the system generator L4655a, from the system's seed. Ten systems (Sol,
 * Alpha Centauri ...) are hand-made instead: their seed is 0 and their
 * bodies come from a table in the game (L47390, docs/system-generation.md).
 * Lcsys_hook in fe2*.s asks Call_CustomSystem first; a system described in a
 * .json file is copied the same way as those ten, so it gets real orbits,
 * working starports and ground bases.
 *
 * Files: every systems/NAME.json in the repo is compiled into the game
 * (CMakeLists.txt); a file of the same name in the play folder's systems/
 * replaces it, and other files there are added. The format is described at
 * the top of systems/kerbol.json. glfrontier.cfg custom_systems=0 turns them
 * all off (the generated systems come back).
 *
 * Not changed: what the galaxy map and the system info screen say about the
 * system before you arrive (star type, population, government): those come
 * from the galaxy generator, not from the body list.
 */
#ifndef CUSTOM_SYSTEM_H
#define CUSTOM_SYSTEM_H

#include <stdbool.h>

typedef struct
{
	const char *name;
	const char *text;
} CustomSystemFile;

extern bool custom_systems_on; /* glfrontier.cfg custom_systems */

/* hcall Call_CustomSystem: d6 = the system key; a1 = the records or 0 */
void Call_CustomSystem(void);

/* hcalls for a body's radius other than its model's (radius_km): set it
 * when the body is made, scale the planet as drawn, and its cull size */
void Call_CustomBody(void);
void Call_CustomPlanet(void);
void Call_CustomCull(void);
void Call_CustomBase(void);
void Call_CustomHull(void);

/* hcall: the system's name, if its file gives one (name = ...) */
void Call_CustomName(void);

/* how many systems are loaded, after the first Call_CustomSystem */
int custom_systems_count(void);

#endif
