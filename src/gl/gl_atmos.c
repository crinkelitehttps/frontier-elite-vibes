/*
 * gl_atmos.c - atmosphere haze.
 *
 * Near a planet the game builds its atmosphere (sky haze on the ground,
 * the glowing rim seen from space) as 2D shapes, bands between two bezier
 * curves, and puts them at the back of its software depth tree. No GL hook
 * draws them; the GL depth node made for each one remembers the address of
 * the game's node (PRIM_SOFT_NODE), and once the game has filled it in
 * the band is copied out (the cockpit reuses that memory for its next
 * pass, and the colour ids are only valid now). The band is filled when
 * that node is drawn, so it lands in exactly the game's draw order.
 *
 * Band node layout (words, after the 12 byte tree header):
 *   42                            start shape
 *   90  x0 y0 x1 y1 x2 y2 x3 y3   bezier edge
 *   56  x0 y0 x1 y1               straight edge
 *   104 colour_id                 fill (even-odd) and end
 * Coordinates are 3D view pixels: x 0..320, y 0..168 downwards. The copy
 * is the same words from the first edge on, with the fill's colour id
 * replaced by its rgb444 value.
 *
 * Those shapes only fit the view the game drew them for, so the cockpit's
 * turned passes (gl_cockpit.c) would each draw slightly different bands that
 * jog where the passes meet. There the bands are drawn in 3D instead, as
 * rings round the planet (AtmosBand): a hook in fe2.s (Lcockpit_band) tells
 * Nu_AtmosBand each band's scale and colour as the game builds it,
 * Nu_PutPlanet, which comes after the bands, says which planet they are
 * round, and every pass draws them all (PRIM_ATMOS, draw_atmos) just in
 * front of its background (the stars and the Milky Way) and behind
 * everything else, as the game puts them at the back of its scene. A pass
 * draws them whether or not it has the planet: it can see the haze round
 * a planet just outside it.
 *
 * The game makes each band by scaling the previous band's outer edge (the
 * planet's horizon arc for the first) by the band's scale about a pivot
 * towards the planet's centre, 2 * $600 = 3072 view pixels away (the code
 * before Lcockpit_band's hook; landed on Merlin the bands are 48 view
 * pixels deep for scales multiplying to 1.0151, so 3190, and from orbit
 * just as deep). Band n's outer edge is then (S - 1) * 3072 view pixels
 * beyond the planet's edge, S being the product of the scales so far: in
 * angles, (S - 1) * 3072 / f radians, f being the game's focal length in
 * view pixels. That is for a planet filling half the sky, the only kind
 * the game's own view makes bands for; but a turned pass with a distant
 * planet just outside it makes them too, about ten times thinner
 * (measured: a gas giant 8 degrees across). Kept like that they were huge
 * rings round distant planets that came and went with the passes, so the
 * depth is scaled by sin(a)^2, a being the planet's angular radius: the
 * same close up, a thin rim far away.
 */
#include <math.h>
#include <string.h>

#include "gl_cockpit.h"
#include "gl_fill2d.h"
#include "gl_native.h"
#include "gl_planet.h"
#include "gl_scene.h"
#include "game_state.h"
#include "m68000.h"
#include "host.h"
#include "renderer.h"

#define ADDR_DYN_COLS 0x56 /* dynamic colour table */
#define TREE_HEADER   12   /* z, two child pointers */

#define OP_SHAPE  42
#define OP_LINE   56
#define OP_BEZIER 90
#define OP_FILL   104

#define BAND_OPS_MAX 64
#define ARENA_WORDS  (1 << 16)

#define SCALE_ONE    16384.0f /* a band's scale word for 1.0 */
#define PIVOT_MAX    3072.0f  /* furthest the game puts its pivot, view pixels */
#define BANDS_MAX    256      /* a frame's, all passes */
#define PLANETS_MAX  16
#define HELD_FRAMES  120 /* publishes bands are kept for, as gl_planet.c's NATIVE_REMEMBER */


/* This frame's bands in 3D (cockpit only). Not every pass the game draws
 * makes them (it only does when the planet's horizon is in that pass's
 * view), so each planet's are kept once, from the first pass that made
 * them, round its centre in view space, and every pass draws them all. */
typedef struct
{
	float inner, outer; /* edges: the products of the scales before it and with it */
	int rgb;            /* rgb444 */
	int planet;         /* in planets[], -1 until its planet is known */
	bool draw;          /* false: another pass's copy of a band kept already */
} AtmosBand;

typedef struct
{
	float centre[3]; /* view space */
	float radius;
} AtmosPlanet;

/* A frame's bands and copied soft nodes: recorded into `rec`, drawn from
 * `show` (see scene_publish / scene_acquire in gl_scene.h) */
typedef struct
{
	int n_bands, n_planets, arena_used;
	bool held; /* the bands of an earlier frame: only while their planet is drawn */
	/* The sky's colour (rgb444, -1: the game's own). The game sets its
	 * background colour 14 to the haze's while it makes the bands, but to
	 * the ground's when its view is all ground (l3cd84): that filled its
	 * whole picture, but the cockpit looks every way, and its background
	 * is whatever the last pass left. So it is taken as the bands are
	 * made, from the game camera's own pass when that has them. */
	int sky;
	bool sky_ahead;
	bool sky_air; /* ... and the game said the player is in the air then: no stars */
	AtmosBand bands[BANDS_MAX];
	AtmosPlanet planets[PLANETS_MAX];
	int16_t arena[ARENA_WORDS];
} AtmosBuf;

static AtmosBuf rec, pub, show;
static int first_pending, planet_puts;
static float scale_so_far = 1.0f;

void soft_nodes_reset(void)
{
	rec.arena_used = 0;
	rec.n_bands = first_pending = rec.n_planets = planet_puts = 0;
	rec.sky = -1;
	rec.sky_ahead = rec.sky_air = false;
	scale_so_far = 1.0f;
}

static void copy_atmos(AtmosBuf *dst, const AtmosBuf *src)
{
	dst->n_bands = src->n_bands;
	dst->n_planets = src->n_planets;
	dst->arena_used = src->arena_used;
	dst->held = src->held;
	dst->sky = src->sky;
	dst->sky_ahead = src->sky_ahead;
	dst->sky_air = src->sky_air;
	memcpy(dst->bands, src->bands, sizeof(AtmosBand) * (size_t)src->n_bands);
	memcpy(dst->planets, src->planets, sizeof(AtmosPlanet) * (size_t)src->n_planets);
	memcpy(dst->arena, src->arena, sizeof(int16_t) * (size_t)src->arena_used);
}

void atmos_publish(void)
{
	/* The game makes the haze only in a frame whose planet routine runs
	 * with the planet's horizon in view; a capture pass towards it comes
	 * every few frames, the others make no planet at all (the planet itself
	 * stands in, gl_planet.c). Those keep the last frame's bands, which are
	 * drawn round the planet's current place (draw_bands). A frame whose
	 * planet routine ran without making any has none. */
	static AtmosBuf held;
	static int held_age = HELD_FRAMES;
	if (rec.n_bands > 0)
	{
		copy_atmos(&held, &rec);
		held_age = 0;
	}
	else if (planet_puts == 0 && held_age < HELD_FRAMES)
	{
		held_age++;
		copy_atmos(&pub, &held);
		pub.held = true;
		return;
	}
	else
		held_age = HELD_FRAMES;
	copy_atmos(&pub, &rec);
	/* The sky's colour stays between the frames that make bands, whatever
	 * the others' planet routine did (all ground, or a planet put without
	 * its horizon): only those know it */
	static int sky = -1, sky_age = HELD_FRAMES;
	static bool sky_air;
	if (rec.sky >= 0)
	{
		sky = rec.sky;
		sky_air = rec.sky_air;
		sky_age = 0;
	}
	else if (sky_age < HELD_FRAMES)
	{
		sky_age++;
		pub.sky = sky;
		pub.sky_air = sky_air;
	}
}

void atmos_acquire(void)
{
	copy_atmos(&show, &pub);
}

static int dyn_colour(int id)
{
	return STMemory_ReadWord(ADDR_DYN_COLS + id + 2) & 0xfff;
}

void Nu_AtmosBand(void)
{
	float scale = (int16_t)STMemory_ReadWord(GetReg(REG_A3) + 202) / SCALE_ONE;
	if (rec.n_bands >= BANDS_MAX)
		return;
	int id = rec.n_bands++;
	AtmosBand *b = &rec.bands[id];
	b->inner = scale_so_far;
	scale_so_far *= scale;
	b->outer = scale_so_far;
	b->rgb = dyn_colour(GetReg(REG_D6) & 0xffff);
	b->planet = -1;
	b->draw = false;
	SoftNode *s = scene_current_soft_node();
	if (s)
		s->shell = id;
}

void atmos_place_bands(const float centre[3], float radius)
{
	planet_puts++;
	if (first_pending < rec.n_bands)
	{
		bool ahead = !cockpit_turned_pass();
		if (ahead || !rec.sky_ahead)
		{
			rec.sky = STMemory_ReadWord(FE2_main_pal1_col14) & 0xfff;
			rec.sky_ahead = ahead;
			/* set with it (L3da2e); each pass clears it first (Lcockpit_world) */
			rec.sky_air = (int16_t)STMemory_ReadWord(GAME_A6 + A6_IN_ATMOSPHERE) < 0;
		}
	}
	if (first_pending < rec.n_bands && radius > 0.0f)
	{
		/* the same planet seen by an earlier pass has the same radius */
		int p = 0;
		while (p < rec.n_planets && rec.planets[p].radius != radius)
			p++;
		bool known = p < rec.n_planets;
		if (!known && rec.n_planets < PLANETS_MAX)
		{
			cockpit_pass_to_view(centre, rec.planets[p].centre);
			rec.planets[p].radius = radius;
			rec.n_planets++;
		}
		if (p < rec.n_planets)
			for (int i = first_pending; i < rec.n_bands; i++)
			{
				rec.bands[i].planet = p;
				rec.bands[i].draw = !known;
			}
	}
	first_pending = rec.n_bands;
	scale_so_far = 1.0f;
}

int atmos_sky_colour(void)
{
	return show.sky;
}

bool atmos_sky_hides_stars(void)
{
	return show.sky >= 0 && show.sky_air;
}

static void draw_bands(void);

void draw_atmos(const void *payload)
{
	(void)payload;
	if (cockpit_active() && native_mode() != 2)
		draw_bands();
}

/* The native scene draws the planets once, round the game's camera, and
 * the bands with them, cockpit or not */
void atmos_draw_native(void)
{
	if (native_mode() == 2)
		draw_bands();
}

static void draw_bands(void)
{
	double f = FILL2D_VIEW_H * 0.5 / tan(CLASSIC_FOV * GLM_PI / 360.0);
	for (int i = 0; i < show.n_bands; i++)
	{
		const AtmosBand *b = &show.bands[i];
		if (!b->draw)
			continue;
		const AtmosPlanet *p = &show.planets[b->planet];
		double c[3] = {p->centre[0], p->centre[1], p->centre[2]};
		float now[3];
		if (planet_native_centre(p->radius, now))
			c[0] = now[0], c[1] = now[1], c[2] = now[2];
		else if (show.held)
			continue;
		double dist = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
		if (dist <= 0.0)
			continue;
		double a = asin(fmin((double)p->radius / dist, 1.0));
		double s = sin(a), k = PIVOT_MAX / f * s * s;
		float c_f[3] = {(float)c[0], (float)c[1], (float)c[2]};
		double outer = a + (b->outer - 1.0) * k;
		/* the first band reaches in to the planet's edge: no inner edge, the
		 * planet is drawn over it */
		double inner = b->inner > 1.0f ? a + (b->inner - 1.0) * k : 0.0;
		planet_draw_shell(c_f, outer >= GLM_PI ? -2.0f : (float)cos(outer),
						  inner > 0.0 ? (float)cos(inner) : 2.0f, b->rgb);
	}
}

/* Copies the band at p (just past its start word); false if it is not one */
static bool copy_band(uint32_t p, SoftNode *s)
{
	int start = rec.arena_used, n = rec.arena_used;
	for (int guard = 0; guard < BAND_OPS_MAX; guard++)
	{
		int op = STMemory_ReadWord(p);
		int args = op == OP_BEZIER ? 8 : op == OP_LINE ? 4 : op == OP_FILL ? 1 : -1;
		if (args < 0 || n + 1 + args > ARENA_WORDS)
			return false;
		rec.arena[n++] = (int16_t)op;
		for (int i = 0; i < args; i++)
			rec.arena[n++] = (int16_t)STMemory_ReadWord(p + 2 + i * 2);
		p += 2 + args * 2;
		if (op == OP_FILL)
		{
			rec.arena[n - 1] = (int16_t)dyn_colour(rec.arena[n - 1]);
			rec.arena_used = n;
			s->band = start;
			return true;
		}
	}
	return false;
}

void soft_node_capture(SoftNode *s)
{
	uint32_t node = s->node;
	s->band = -1;
	if (node == 0 || node + TREE_HEADER + 2 >= MEM_SIZE)
		return;
	if (STMemory_ReadWord(node + TREE_HEADER) == OP_SHAPE)
		copy_band(node + TREE_HEADER + 2, s);
}

void draw_soft_node(const void *payload)
{
	const SoftNode *s = payload;
	if (s->band < 0)
		return;
	/* drawn in 3D instead (draw_atmos) */
	if (s->shell >= 0 && s->shell < show.n_bands && show.bands[s->shell].planet >= 0 &&
		(cockpit_active() || native_mode() == 2))
		return;

	const int16_t *w = &show.arena[s->band];
	fill2d_begin();
	for (;;)
	{
		switch (*w++)
		{
		case OP_BEZIER:
			fill2d_bezier(w, 0);
			w += 8;
			break;
		case OP_LINE:
			fill2d_line(w[0], w[1], w[2], w[3], 0);
			w += 4;
			break;
		case OP_FILL:
		{
			int rgb = w[0];
			fill2d_draw(FILL2D_EVEN_ODD, fill2d_solid, &rgb);
			return;
		}
		default:
			return;
		}
	}
}
