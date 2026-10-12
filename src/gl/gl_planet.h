/*
 * gl_planet.h - planet rendering.
 *
 * The game's planet routine builds the planet as a 2D shape list for its
 * software renderer and then (label fuck_planet in fe2_modded.s.c) calls hostcall
 * 0x78 -> Nu_PutPlanet. Everything the GL renderer knows about a planet is
 * captured there into PlanetDesc, including a copy of that 2D list.
 *
 * 2D planet list format (words, 3D view pixel coordinates):
 *   156                                    start
 *   166 x0 y0 x1 y1 code                   straight edge
 *   182 x0 y0 x1 y1 x2 y2 x3 y3 code       bezier edge
 *   198 row code                           region code change at a row
 *   208 id x16                             colour table (dynamic colour ids),
 *                                          ends the shape and fills it
 * The software renderer fills it scanline by scanline, XOR-ing edge codes to
 * pick a colour from the table.
 *
 * To add planet features (surface detail, shading, rings, ...):
 *   1. Add fields to PlanetDesc.
 *   2. Fill them in Nu_PutPlanet() from emulated RAM. Only copy data there:
 *      RAM will have changed by draw time.
 *   3. Draw them in draw_planet(), with gd_* geometry, fill2d_* shapes or
 *      another gd_custom() GPU pass.
 */
#ifndef GL_PLANET_H
#define GL_PLANET_H

#include <stdbool.h>
#include <stdint.h>
#include "gl_planet_tex.h"
#include "gl_scene.h"

/* Words. Detailed planets (Earth's hand-made continents) have long lists;
 * a scene record holds at most 64K bytes, which bounds this. */
#define PLANET_LIST_MAX 30000

typedef struct
{
	Vec3i pos;         /* centre, view space */
	float radius;      /* view-space units */
	float light[3];    /* light direction, view space */
	int32_t obj_col;   /* rgb444 surface colour (D6) */
	int32_t light_col; /* rgb444 light colour  (D1) */
	float rot[9];      /* planet orientation, column-major 3x3 */
	/* The planet's matrix as the game turns its surface points with it:
	 * game view = A * planet, A column-major (columns: the planet's axes) */
	float axes[9];

	/* Its surface features, for the native texture (gl_planet_tex.h) */
	PlanetSurface surface;
	bool surface_ok;
	/* rgb444 by code / 4: dark side 0-7, lit side 8-15 (the game's own
	 * colour table from its 2D shape, whose terminator toggles the lit
	 * half in; else the unlit colours) */
	uint16_t palette[16];

	/* Raw emulator context at capture time, for decoding extra features */
	struct
	{
		int32_t d[8];
		uint32_t a[8];
	} ctx;

	/* The game's own 2D shape of the planet (see format above) */
	uint16_t list_len; /* words, 0 if not captured */
	bool list_ok;      /* well formed: draw from it instead of the sphere */
	int16_t list[];    /* list_len words; colour tables already hold rgb444 */
} PlanetDesc;

void gl_planet_init(void);
void gl_planet_shutdown(void);

/* Fills the directions between acos(cos_inner) and acos(cos_outer) from a
 * planet's centre (view space, not turned by the pass being drawn) with an
 * rgb444 colour: an atmosphere band (gl_atmos.c). cos_inner > 1 for no
 * inner edge, cos_outer < -1 for no outer one. */
void planet_draw_shell(const float centre[3], float cos_outer, float cos_inner, int rgb);
/* The native scene's planets (GLF_NATIVE_SCENE=2; render thread): each
 * planet object captured this frame, its view position and axes (as
 * gl_native.cpp has them), before counting; then how many there are, how
 * far, and drawing one */
void planet_native_object(uint32_t addr, const float pos[3], const float cols[3][3]);
int planet_native_count(void);
uint32_t planet_native_object_addr(int i); /* its planet object */
double planet_native_dist(int i);
void planet_native_draw(int i);
/* The atmosphere bands round them (gl_atmos.c), before the planets */
void atmos_draw_native(void);

/* Says which planet the atmosphere bands just made are round (gl_atmos.c) */
void atmos_place_bands(const float centre[3], float radius);

#endif /* GL_PLANET_H */
