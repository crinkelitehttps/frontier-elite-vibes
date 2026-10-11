/*
 * gl_planet_tex.h - native planet surfaces: the game's surface features
 * (continents, seas, ice caps, craters) drawn once into a texture in the
 * planet's own frame, for the ray traced sphere to sample.
 *
 * The data is the planet bytecode command (L3cd9c_ProjectPlanet in fe2.s):
 *
 *   +0  colour word: flags (high nibble) and the base colour
 *   +2  three more base colours
 *       flags bit 4: a selector word and 7 alternative sets of 4 colours
 *       (else, unless bit 6, 4 more colours)
 *   a word list (0 ends it; ring data, not used here)
 *   features, each:
 *     code byte (0 ends the list)
 *       bit 7 clear: an outline. Detail byte (signed: the fractal edges'
 *         size), then points: x y z signed bytes, a direction from the
 *         centre (length about 64). The x of the next point follows each
 *         point; 0 there ends the outline. A first point followed by 0 is a
 *         marker (the game's detail trigger) with a size byte, not part of
 *         the outline.
 *       bit 7 set: a circle (ice cap, crater): centre x y z (negated
 *         bytes), angular radius byte (x64, 16 bit angle units); bit 6 also
 *         draws the opposite one. Its code is the byte & $1c.
 *
 * A point's colour is the XOR of the codes of the features round it,
 * through the planet's colour table (code / 4). Edges between outline
 * points are split recursively the way the game splits them (L3ddc4,
 * L3de2e): the midpoint on the sphere, pushed along the planet's axes by
 * pseudo-random amounts that halve at each level. The game splits only
 * edges that are long on screen, so its coastlines shift with distance;
 * here every edge is split to the game's deepest level, which fixes them
 * to the planet.
 */
#ifndef GL_PLANET_TEX_H
#define GL_PLANET_TEX_H

#include <stdbool.h>
#include <stdint.h>

#include "gl_api.h"

/* The planet command's data longer than this is not textured */
#define PLANET_DATA_MAX 2048

typedef struct
{
	uint8_t data[PLANET_DATA_MAX]; /* from the colour word on */
	int len;                       /* bytes */
	uint32_t seed;                 /* the planet object's long at +118 */
	uint16_t radius_word;          /* the command's radius word */
	uint16_t colours[16];          /* rgb444, by code / 4: unlit 0-7, lit 8-15 */
	bool lit_colours;              /* 8-15 are the game's (else copies of 0-7) */
} PlanetSurface;

/* Checks that `s` parses to exactly its length */
bool planet_surface_valid(const PlanetSurface *s);
/* The surface texture for `s`: each texel's feature code (GL_R8, code / 255;
 * the colour is the planet's colour table at code / 4), equirectangular: u =
 * longitude from atan2(x, z), v = acos(y) / pi, in the planet's own axes.
 * Made on first use and kept; 0 if it can't be made. Needs the GL context. */
GLuint planet_surface_texture(const PlanetSurface *s);
void planet_tex_shutdown(void);

#endif /* GL_PLANET_TEX_H */
