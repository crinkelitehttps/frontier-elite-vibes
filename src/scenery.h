/*
 * scenery.h - the ground scenery (cloud fields, mountains, hills, trees) kept by the host.
 *
 * The game's spawner only makes an object at a time and holds each in one of its 114 slots, so its
 * population was small and followed the direction of travel. Now the host decides, from a hash of the
 * planet and the 4.2 km cell, where every feature is within scenery_radius_km of the ship; has the
 * game's spawner make each one once (kind, matrix and height from the game's own code, Lscen_pick /
 * Lscen_made in fe2.s), keeps a copy of the object and deletes the game's; and each frame hands the
 * copies within that distance to the game's object drawing (Lvirt_a): the model's own limits (dot,
 * gone, detail) decide how each is shown, by distance from the ship alone. Details:
 * docs/deterministic-scenery.md.
 */
#ifndef SCENERY_H
#define SCENERY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* How far from the ship features are made and shown (L-infinity, like the game's distance), km; 0 = none */
extern int scenery_radius_km;

/* How fast the clouds drift, m/s of game time, in a direction that is fixed per planet; 0 = they stand still */
extern int scenery_cloud_wind_mps;

void Call_ScenNext(void);
void Call_ScenMade(void);
void Call_VirtNext(void);
void Call_ScenDrive(void);

/* While one of the host's copies is being drawn: 1 + its index, else 0 (they share one buffer in RAM) */
uint32_t scenery_virtual_id(void);

#ifdef __cplusplus
}
#endif

#endif
