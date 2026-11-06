/*
 * gl_planet.c - planets, drawn from the game's own 2D planet shape (with
 * all its surface detail and shading); a ray traced sphere is the fallback
 * when that shape can't be read.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cheats.h"
#include "gl_api.h"
#include "gl_cockpit.h"
#include "gl_draw.h"
#include "gl_fill2d.h"
#include "gl_native.h"
#include "gl_planet.h"
#include "game_state.h"
#include "gl_prims.h"
#include "gl_scene.h"
#include "gl_shadow.h"
#include "main.h"
#include "m68000.h"

/* =========================================================================
 * Capture (runs inside the emulator)
 * ========================================================================= */

#define ADDR_PRIMITIVES_END 0x2caa4 /* end of the software display list */
#define ADDR_DYN_COLS       0x56    /* dynamic colour table */

enum
{
	PL_START = 156,
	PL_LINE = 166,
	PL_BEZIER = 182,
	PL_ROW = 198,
	PL_COLOURS = 208
};

/* Copies the 2D shape list the game just built for this planet (it starts
 * at the address saved in A6-190 and ends at the display list end). Colour
 * tables hold dynamic colour ids that are only valid right now, so they are
 * replaced by their rgb444 values in the copy. */
/* Words in the list the game just built, 0 if none or too long */
static int list_words(uint32_t a6)
{
	uint32_t start = (uint32_t)STMemory_ReadLong(a6 - 190);
	uint32_t end = (uint32_t)STMemory_ReadLong(ADDR_PRIMITIVES_END);
	if (end <= start)
		return 0;
	if ((end - start) / 2 > PLANET_LIST_MAX)
	{
		static bool warned;
		if (!warned)
			log_printf("Planet: shape list of %u words is too long, drawing a plain sphere\n",
					   (unsigned)((end - start) / 2));
		warned = true;
		return 0;
	}
	return (int)(end - start) / 2;
}

static void capture_list(PlanetDesc *p, int n)
{
	uint32_t start = (uint32_t)STMemory_ReadLong(p->ctx.a[6] - 190);
	p->list_len = 0;
	p->list_ok = false;
	if (n == 0)
		return;

	for (int i = 0; i < n; i++)
		p->list[i] = STMemory_ReadWord(start + i * 2);
	p->list_len = (uint16_t)n;

	int i = 0, fills = 0;
	while (i < n)
	{
		switch (p->list[i++])
		{
		case PL_START:
			break;
		case PL_LINE:
			i += 5;
			break;
		case PL_BEZIER:
			i += 9;
			break;
		case PL_ROW:
			i += 2;
			break;
		case PL_COLOURS:
			if (i + 16 > n)
				return;
			for (int k = 0; k < 16; k++)
				p->list[i + k] = STMemory_ReadWord(ADDR_DYN_COLS + p->list[i + k] + 2) & 0xfff;
			i += 16;
			fills++;
			break;
		default:
			return; /* unknown opcode: fall back to the plain sphere */
		}
	}
	p->list_ok = (i == n && fills > 0);
}

/* The planet command's data (gl_planet_tex.h). Called at the end of the
 * planet routine (fuck_planet), whose frame (A3) has been given back: its
 * first words still hold the colours and the data's size, and A5 has gone
 * just past the data. */
static uint32_t sea_obj[4];
static PlanetSurface sea_surface[4];

static void capture_surface(PlanetDesc *p)
{
	PlanetSurface *s = &p->surface;
	p->surface_ok = false;
	uint32_t a3 = (uint32_t)GetReg(REG_A3), a5 = (uint32_t)GetReg(REG_A5), a6 = (uint32_t)GetReg(REG_A6);
	int len = (uint16_t)STMemory_ReadWord(a3 + 32);
	if (len <= 0 || len > PLANET_DATA_MAX || a5 < (uint32_t)len + 6)
		return;
	uint32_t start = a5 - (uint32_t)len;
	for (int i = 0; i < len; i++)
		s->data[i] = (uint8_t)STMemory_ReadByte(start + (uint32_t)i);
	s->len = len;
	s->radius_word = (uint16_t)STMemory_ReadWord(start - 4);
	uint32_t obj = (uint32_t)STMemory_ReadLong(a6 - 212);
	s->seed = (uint32_t)STMemory_ReadLong(obj + 118);
	/* the second table, with the sunlight added, is only made when the
	 * flags word has bit 7 or 6 (L3d0be) */
	s->lit_colours = (s->data[0] & 0xc0) != 0;
	for (int i = 0; i < 16; i++)
		s->colours[i] = (uint16_t)(STMemory_ReadWord(a3 + 2 * (uint32_t)(s->lit_colours ? i : i & 7)) & 0xfff);
	p->surface_ok = planet_surface_valid(s);
	if (p->surface_ok)
	{
		/* kept for planet_is_sea, by planet object */
		static int next;
		int k = -1;
		for (int i = 0; i < 4; i++)
			if (sea_obj[i] == obj)
				k = i;
		if (k < 0)
		{
			k = next;
			next = (next + 1) & 3;
		}
		sea_obj[k] = obj;
		sea_surface[k] = *s;
	}
}

/* Game thread: 1 if the point `rel` (mm, game axes, from the centre of the planet object `obj`) is sea, 0 if
 * not, -1 if unknown (the planet's surface not seen yet, or no outlines) */
int planet_is_sea(uint32_t obj, const double rel[3])
{
	for (int i = 0; i < 4; i++)
		if (sea_obj[i] == obj)
		{
			double l = sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
			if (l <= 0)
				return -1;
			/* the game's own axes (checked on Earth: London land, 20 E on the equator land, 20 W sea) */
			float v[3] = {(float)(rel[0] / l), (float)(rel[1] / l), (float)(rel[2] / l)};
			return planet_surface_is_sea(&sea_surface[i], v);
		}
	return -1;
}

/* The colours the game fills this planet with (its 2D shape's first colour
 * table, rgb444 by now), else its colour tables from the frame (without a
 * lit table, a darker copy for the dark side). When no coastline crosses
 * the game's view it fills the whole table with the colour under the view
 * (L3d5dc_PushPlanetCol, 210(a3) set): right for its flat shape, but the
 * sphere shows every feature, so a table of one colour is left out. */
static void capture_palette(PlanetDesc *p)
{
	const PlanetSurface *s = &p->surface;
	for (int i = 0; i < 8; i++)
	{
		p->palette[i] = s->lit_colours ? s->colours[i] : (uint16_t)((s->colours[i] >> 2) & 0x333);
		p->palette[i + 8] = s->colours[i + 8];
	}
	if (!p->list_ok)
		return;
	for (int i = 0; i < p->list_len;)
	{
		switch (p->list[i])
		{
		case PL_START:
			i += 1;
			break;
		case PL_LINE:
			i += 6;
			break;
		case PL_BEZIER:
			i += 10;
			break;
		case PL_ROW:
			i += 3;
			break;
		case PL_COLOURS:
		{
			int same = 1;
			while (same < 16 && p->list[i + 1 + same] == p->list[i + 1])
				same++;
			if (same == 16 && p->surface_ok)
				return;
			/* The game leaves colours its view doesn't use at 0 (the lit
			 * table's ice, with the pole in view: black ice on the sphere):
			 * keep the frame's own for those */
			for (int k = 0; k < 16; k++)
			{
				uint16_t c = (uint16_t)(p->list[i + 1 + k] & 0xfff);
				if (c || !p->surface_ok || !p->palette[k])
					p->palette[k] = c;
			}
			return;
		}
		default:
			return;
		}
	}
}

/* Each planet's distance in the cockpit's straight ahead pass. The turned
 * passes get the camera matrix rounded to words, no longer quite a rotation,
 * so each puts the planet a little nearer or further (up to about 3 in
 * 100000: a couple of hundred metres on Merlin, more than the height of a
 * ship just off the ground, which then sees the ground all over some
 * passes). The ahead pass is the game's own camera, drawn last; every pass
 * draws the planet at its distance. Its colours too: the game picks them
 * for the view it draws (its 2D shape's colour table), so each pass would
 * shade the ground a little differently and show where the passes meet. */
#define DIST_PLANETS 16
typedef struct
{
	int n;
	float radius[DIST_PLANETS];
	double dist[DIST_PLANETS];
	uint16_t palette[DIST_PLANETS][16];
	int light_col[DIST_PLANETS], obj_col[DIST_PLANETS];
} PlanetDists;

static PlanetDists dists_rec, dists_pub, dists_show;

/* The native scene's planets (GLF_NATIVE_SCENE=2): each planet once a
 * frame, from the pass of the game's own camera when that one has it, else
 * the first pass that does, in the game camera's view space; drawn by
 * native_draw_sky instead of the passes. The planet routine leaves some
 * views out (all ground, L3cd8e), so a planet object captured without its
 * planet gets its last one, carried along by the object's turn
 * (planet_native_object). */
#define NATIVE_PLANETS 8
typedef struct
{
	uint32_t obj;  /* the planet object */
	bool ahead;    /* from the game camera's own pass */
	double pos[3]; /* centre, GL view space */
	float radius;
	float shrink; /* native_frame_shrink of the object's frame */
	float light[3];
	float axes[9]; /* as PlanetDesc.axes, in the game camera's view */
	int light_col, obj_col;
	uint16_t palette[16];
	PlanetSurface surface;
	bool surface_ok;
	/* the object as gl_native.cpp captures it: its view position and axes */
	float obj_pos[3], obj_cols[3][3];
	uint32_t seen;  /* render frame it was last captured in (remembered ones) */
	bool stand_in;  /* a remembered one standing in this frame */
} NativePlanet;

typedef struct
{
	int n;
	NativePlanet p[NATIVE_PLANETS];
} NativePlanets;

static NativePlanets native_rec, native_pub, native_show, native_last;
/* the cockpit screen's own (its maps' planets), drawn instead of the
 * world's while its picture is drawn (planet_use_screen) */
static NativePlanets screen_pub, screen_show;
static NativePlanets page_pub, page_show; /* ... and a page's beside it (gl_scene.h) */
static NativePlanets *drawn_planets = &native_show;
static uint32_t native_frame;

/* Where each planet was when last seen, for the cockpit's capture passes
 * (planet_native_find; game thread) */
#define KNOWN_PLANETS 16
typedef struct
{
	uint32_t obj;
	double pos[3];
	float radius;
	float obj_pos[3], obj_cols[3][3];
	uint32_t frame; /* game frame it was seen in */
	bool ahead;
} KnownPlanet;
static KnownPlanet known[KNOWN_PLANETS];
static int n_known;
static uint32_t game_frame, published_frame;

void planet_dists_reset(void)
{
	dists_rec.n = 0;
	native_rec.n = 0;
	game_frame++;
}

void planet_dists_publish(void)
{
	published_frame = game_frame;
	dists_pub = dists_rec;
	native_pub.n = native_rec.n;
	memcpy(native_pub.p, native_rec.p, sizeof native_rec.p[0] * (size_t)native_rec.n);
}

void planet_dists_acquire(void)
{
	dists_show = dists_pub;
	native_show.n = native_pub.n;
	memcpy(native_show.p, native_pub.p, sizeof native_pub.p[0] * (size_t)native_pub.n);
	native_frame++;
}

void planet_screen_publish(void)
{
	screen_pub.n = native_rec.n;
	memcpy(screen_pub.p, native_rec.p, sizeof native_rec.p[0] * (size_t)native_rec.n);
}

void planet_screen_acquire(void)
{
	screen_show.n = screen_pub.n;
	memcpy(screen_show.p, screen_pub.p, sizeof screen_pub.p[0] * (size_t)screen_pub.n);
}

void planet_use_screen(bool screen)
{
	drawn_planets = screen ? &screen_show : &native_show;
}

void planet_page_publish(void)
{
	page_pub.n = native_rec.n;
	memcpy(page_pub.p, native_rec.p, sizeof native_rec.p[0] * (size_t)native_rec.n);
}

void planet_page_acquire(void)
{
	page_show.n = page_pub.n;
	memcpy(page_show.p, page_pub.p, sizeof page_pub.p[0] * (size_t)page_pub.n);
}

void planet_use_page(bool page)
{
	drawn_planets = page ? &page_show : &native_show;
}

/* A turned pass's game view vector (z ahead) in the game camera's */
static void pass_to_view_game(const float v[3], float out[3])
{
	float g[3] = {v[0], v[1], -v[2]};
	cockpit_pass_to_view(g, out);
	out[2] = -out[2];
}

static void remember_known(const NativePlanet *n)
{
	int i = 0;
	while (i < n_known && known[i].obj != n->obj)
		i++;
	if (i == n_known)
	{
		if (n_known < KNOWN_PLANETS)
			n_known++;
		else /* forget the one seen longest ago */
			for (int j = i = 0; j < n_known; j++)
				if (known[j].frame < known[i].frame)
					i = j;
	}
	KnownPlanet *k = &known[i];
	k->obj = n->obj;
	memcpy(k->pos, n->pos, sizeof k->pos);
	k->radius = n->radius;
	memcpy(k->obj_pos, n->obj_pos, sizeof k->obj_pos);
	memcpy(k->obj_cols, n->obj_cols, sizeof k->obj_cols);
	k->frame = game_frame;
	k->ahead = n->ahead;
}

/* The planet the ship flies relative to at its exact distance. The game's
 * own comes from positions rounded to a power of two per pass: at a few
 * hundred metres up it was over a kilometre out, different each frame, and
 * the eye went in and out of the planet (save "Flicker3"). Only its length
 * is put right. */
static void exact_distance(NativePlanet *n)
{
	/* only a view from the ship (not the external one, a map, a screen's page) */
	FlightView v = game_flight_view();
	if (game_screen() != SCREEN_FLIGHT || v == VIEW_EXTERNAL || v >= VIEW_COUNT)
		return;
	double ratio = cheat_ship_height_ratio(n->obj);
	double d = sqrt(n->pos[0] * n->pos[0] + n->pos[1] * n->pos[1] + n->pos[2] * n->pos[2]);
	if (ratio <= 0 || d <= 0 || n->radius <= 0)
		return;
	double k = n->radius * ratio / d;
	for (int i = 0; i < 3; i++)
		n->pos[i] *= k;
}

/* The game's planet position and axes come rounded (steps of ~500 km at Earth's centre, and 190 km from
 * the 16-bit words of each), so the ground under the eye jumped by that much while the height stayed smooth,
 * most when the ship pitched. Both are rebuilt from the ship's exact position and its matrix made orthonormal
 * in double: in the planet's frame the sub-eye point is then just the exact position's direction. */
static void exact_view(double pos[3], float axes[9], unsigned obj)
{
	double r[3], col[3][3];
	unsigned me;
	double len = sqrt(pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2]);
	if (len <= 0 || !cheat_ship_object(&me) || !cheat_ship_body_vector(obj, r))
		return;
	double rl = sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
	if (rl <= 0)
		return;
	/* the ship's axes, orthonormal in double: column i = (right, up, forward)[i] in the ship's frame. Within the
	 * zone the game switches at (1.25 radii for Earth) that frame turns with the planet, and the planet's axes
	 * are the frame's own; outside it is fixed and the planet's own matrix turns its surface. Without that the
	 * map was a whole turn of the planet's spin out (38.6 degrees at the time of save Snap17) and jumped by
	 * that much as the ship crossed the zone's edge (saves Snap17/Snap18). */
	cheat_object_axes(me, col);
	double p[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}; /* p[j]: the planet's axis j in the ship's frame */
	if (!cheat_ship_in_body_frame())
	{
		double pc[3][3];
		cheat_object_axes(obj, pc);
		for (int j = 0; j < 3; j++)
			for (int k = 0; k < 3; k++)
				p[j][k] = pc[j][k];
	}
	double e[3], a[3][3]; /* e: centre in ship axes; a[j]: the planet's axis j in ship axes */
	for (int c = 0; c < 3; c++)
	{
		e[c] = (col[c][0] * r[0] + col[c][1] * r[1] + col[c][2] * r[2]) / rl;
		for (int j = 0; j < 3; j++)
			a[j][c] = col[c][0] * p[j][0] + col[c][1] * p[j][1] + col[c][2] * p[j][2];
	}
	/* The game's position has z flipped against its axes (the shader flips it back): work in the axes' view. The
	 * view (front, rear, side) is a signed permutation of the ship's axes: take the one that agrees with the
	 * game's own vector. */
	double gp[3] = {pos[0] / len, pos[1] / len, -pos[2] / len};
	/* The direction alone can't tell them apart when two of its components are equal (a swapped view fits as
	 * well, and the ground came out inverted on and off while the ship turned); the game's three surface axes
	 * can: score a view by how well its axes agree with them as well. */
	double ga[3][3];
	for (int j = 0; j < 3; j++)
	{
		double l = sqrt(axes[3 * j] * axes[3 * j] + axes[3 * j + 1] * axes[3 * j + 1] + axes[3 * j + 2] * axes[3 * j + 2]);
		for (int k = 0; k < 3; k++)
			ga[j][k] = l > 0 ? axes[3 * j + k] / l : 0;
	}
	static const int perm[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
	double best = -2, bestscore = -10;
	int bp = 0, bs = 0;
	/* The front and rear views are known: the ship's axes as they are, or turned half round its up axis (x and z
	 * negated). Scored instead, a wrong view could win (saves Snap03/04, when the planet's spin was still left
	 * out above and its axes disagreed with the game's), fail the check below and leave the game's axes. */
	FlightView view = game_flight_view();
	if (view == VIEW_FRONT || view == VIEW_REAR)
	{
		bs = view == VIEW_REAR ? 5 : 0;
		best = 0;
		for (int k = 0; k < 3; k++)
			best += (bs >> k & 1 ? -1.0 : 1.0) * e[k] * gp[k];
	}
	else
		for (int pi = 0; pi < 6; pi++)
			for (int sg = 0; sg < 8; sg++)
			{
				double d = 0, score = 0;
				for (int k = 0; k < 3; k++)
				{
					double sgn = sg >> k & 1 ? -1.0 : 1.0;
					d += sgn * e[perm[pi][k]] * gp[k];
					for (int j = 0; j < 3; j++)
						score += sgn * a[j][perm[pi][k]] * ga[j][k];
				}
				score += d;
				if (score > bestscore)
				{
					bestscore = score;
					best = d;
					bp = pi;
					bs = sg;
				}
			}
	/* within ~14 mrad (the ship's matrix is a frame ahead or behind the game's camera while it turns); else
	 * not the ship's own view */
	if (best < 1 - 1e-4)
		return;
	double v[3], w[3][3];
	for (int k = 0; k < 3; k++)
	{
		double sgn = bs >> k & 1 ? -1.0 : 1.0;
		v[k] = sgn * e[perm[bp][k]];
		for (int j = 0; j < 3; j++)
			w[j][k] = sgn * a[j][perm[bp][k]];
	}
	/* Turn all of it about the eye, by the little that takes v to the game's own direction (its camera, which
	 * the ship's matrix leads or lags by a frame): the planet's centre then sits where the game has it, and
	 * the surface axes turn with it, so the point under the eye stays the exact one. */
	double k[3] = {v[1] * gp[2] - v[2] * gp[1], v[2] * gp[0] - v[0] * gp[2], v[0] * gp[1] - v[1] * gp[0]};
	double s = sqrt(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]), c = v[0] * gp[0] + v[1] * gp[1] + v[2] * gp[2];
	if (s > 1e-12)
	{
		for (int i = 0; i < 3; i++)
			k[i] /= s;
		for (int j = 0; j < 3; j++)
		{
			double kw = k[0] * w[j][0] + k[1] * w[j][1] + k[2] * w[j][2];
			double cr[3] = {k[1] * w[j][2] - k[2] * w[j][1], k[2] * w[j][0] - k[0] * w[j][2], k[0] * w[j][1] - k[1] * w[j][0]};
			for (int i = 0; i < 3; i++)
				w[j][i] = w[j][i] * c + cr[i] * s + k[i] * kw * (1 - c);
		}
	}
	pos[0] = gp[0] * len;
	pos[1] = gp[1] * len;
	pos[2] = -gp[2] * len;
	for (int j = 0; j < 3; j++)
		for (int i = 0; i < 3; i++)
			axes[3 * j + i] = (float)w[j][i];
}

static void record_native(const PlanetDesc *p)
{
	if (native_mode() != 2)
		return;
	uint32_t a6 = p->ctx.a[6];
	uint32_t obj = (uint32_t)STMemory_ReadLong(a6 - 212);
	bool ahead = !cockpit_turned_pass();
	int i = 0;
	while (i < native_rec.n && native_rec.p[i].obj != obj)
		i++;
	if (i < native_rec.n ? native_rec.p[i].ahead || !ahead : native_rec.n >= NATIVE_PLANETS)
		return;
	if (i == native_rec.n)
		native_rec.n++;
	NativePlanet *n = &native_rec.p[i];
	n->obj = obj;
	n->ahead = ahead;
	n->stand_in = false;
	float pos[3], v[3];
	vec3i_to_f(p->pos, pos);
	cockpit_pass_to_view(pos, v);
	for (int c = 0; c < 3; c++)
		pass_to_view_game(&p->axes[3 * c], &n->axes[3 * c]);
	/* A capture pass's planet is as rounded as the game camera's (more: its camera too), so it gets the exact view
	 * as well, once turned into the game camera's view: over London at 16 km its map was ~5 mrad (30 km) off the
	 * game camera's (save "flickeri"). */
	if (game_screen() == SCREEN_FLIGHT && game_flight_view() != VIEW_EXTERNAL)
	{
		double dp[3] = {v[0], v[1], v[2]};
		exact_view(dp, n->axes, obj);
		for (int k = 0; k < 3; k++)
			v[k] = (float)dp[k];
	}
	for (int k = 0; k < 3; k++)
		n->pos[k] = v[k];
	n->radius = p->radius;
	exact_distance(n);
	n->shrink = native_frame_shrink(a6);
	cockpit_pass_to_view(p->light, n->light);
	n->light_col = p->light_col;
	n->obj_col = p->obj_col;
	memcpy(n->palette, p->palette, sizeof n->palette);
	n->surface_ok = p->surface_ok;
	if (p->surface_ok)
		n->surface = p->surface;
	/* as capture() in gl_native.cpp */
	float op[3] = {(float)(int32_t)STMemory_ReadLong(a6 - 16), (float)(int32_t)STMemory_ReadLong(a6 - 12),
				   -(float)(int32_t)STMemory_ReadLong(a6 - 8)};
	cockpit_pass_to_view(op, n->obj_pos);
	for (int c = 0; c < 3; c++)
	{
		uint32_t m = a6 - 36 + 6 * (uint32_t)c;
		float col[3] = {(int16_t)STMemory_ReadWord(m) / 32768.0f, (int16_t)STMemory_ReadWord(m + 2) / 32768.0f,
						-(int16_t)STMemory_ReadWord(m + 4) / 32768.0f};
		cockpit_pass_to_view(col, n->obj_cols[c]);
	}
	remember_known(n);
}

/* The game's matrix words are rounded, so its columns are neither unit nor square to each other (about 1 in 120
 * short): made a rotation, Gram-Schmidt in double */
static void orthonormal(const float in[3][3], double out[3][3])
{
	for (int c = 0; c < 3; c++)
	{
		for (int k = 0; k < 3; k++)
			out[c][k] = in[c][k];
		for (int p = 0; p < c; p++)
		{
			double d = out[c][0] * out[p][0] + out[c][1] * out[p][1] + out[c][2] * out[p][2];
			for (int k = 0; k < 3; k++)
				out[c][k] -= d * out[p][k];
		}
		double l = sqrt(out[c][0] * out[c][0] + out[c][1] * out[c][1] + out[c][2] * out[c][2]);
		for (int k = 0; k < 3; k++)
			out[c][k] = l > 0 ? out[c][k] / l : 0;
	}
}

/* Q v: the object's turn since `old` was captured, Q = new old^T. Both made rotations first: with the rounded
 * words Q was no turn at all for a planet that hadn't moved but squashed and skewed it (the paused game), and a
 * remembered planet's map lay about a degree off the captured one's: over Earth at 15 km the coast jumped
 * between two shapes every frame (save "flickeri"). */
static void turn_by(const float old_cols[3][3], const float cols[3][3], const float v[3], float out[3])
{
	double o[3][3], n[3][3], a[3];
	orthonormal(old_cols, o);
	orthonormal(cols, n);
	for (int i = 0; i < 3; i++)
		a[i] = o[i][0] * v[0] + o[i][1] * v[1] + o[i][2] * v[2];
	for (int k = 0; k < 3; k++)
		out[k] = (float)(n[0][k] * a[0] + n[1][k] * a[1] + n[2][k] * a[2]);
}

/* How long a remembered planet stands in, render frames */
#define NATIVE_REMEMBER 120

void planet_native_object(uint32_t addr, const float pos[3], const float cols[3][3])
{
	for (int i = 0; i < native_show.n; i++)
		if (native_show.p[i].obj == addr)
		{
			if (native_show.p[i].stand_in)
				return;
			/* remember it */
			int j = 0;
			while (j < native_last.n && native_last.p[j].obj != addr)
				j++;
			if (j == native_last.n)
			{
				if (native_last.n >= NATIVE_PLANETS)
					j = 0; /* forget the first */
				else
					native_last.n++;
			}
			native_last.p[j] = native_show.p[i];
			native_last.p[j].seen = native_frame;
			return;
		}
	for (int j = 0; j < native_last.n; j++)
	{
		const NativePlanet *o = &native_last.p[j];
		if (o->obj != addr || native_frame - o->seen > NATIVE_REMEMBER || native_show.n >= NATIVE_PLANETS)
			continue;
		NativePlanet *n = &native_show.p[native_show.n++];
		*n = *o;
		n->stand_in = true;
		float d[3] = {(float)(o->pos[0] - o->obj_pos[0]), (float)(o->pos[1] - o->obj_pos[1]),
					  (float)(o->pos[2] - o->obj_pos[2])}, t[3];
		turn_by(o->obj_cols, cols, d, t);
		for (int k = 0; k < 3; k++)
			n->pos[k] = (double)pos[k] + t[k];
		exact_distance(n);
		turn_by(o->obj_cols, cols, o->light, n->light);
		for (int c = 0; c < 3; c++)
		{
			float g[3] = {o->axes[3 * c], o->axes[3 * c + 1], -o->axes[3 * c + 2]};
			turn_by(o->obj_cols, cols, g, &n->axes[3 * c]);
			n->axes[3 * c + 2] = -n->axes[3 * c + 2];
		}
		return;
	}
}

bool planet_native_find(uint32_t addr, const float pos[3], const float cols[3][3], float centre[3], float *radius,
						bool *ahead)
{
	for (int i = 0; i < n_known; i++)
	{
		const KnownPlanet *k = &known[i];
		if (k->obj != addr)
			continue;
		float d[3] = {(float)(k->pos[0] - k->obj_pos[0]), (float)(k->pos[1] - k->obj_pos[1]),
					  (float)(k->pos[2] - k->obj_pos[2])}, t[3];
		turn_by(k->obj_cols, cols, d, t);
		for (int c = 0; c < 3; c++)
			centre[c] = pos[c] + t[c];
		*radius = k->radius;
		*ahead = k->ahead && k->frame == published_frame;
		return true;
	}
	return false;
}

bool planet_native_centre(float radius, float centre[3])
{
	if (native_mode() != 2)
		return false;
	for (int i = 0; i < drawn_planets->n; i++)
		if (fabsf(drawn_planets->p[i].radius - radius) <= radius * 1e-3f)
		{
			for (int k = 0; k < 3; k++)
				centre[k] = (float)drawn_planets->p[i].pos[k];
			return true;
		}
	return false;
}

uint32_t planet_native_object_addr(int i)
{
	return drawn_planets->p[i].obj;
}

int planet_native_count(void)
{
	return native_mode() == 2 ? drawn_planets->n : 0;
}

float planet_native_shrink(int i)
{
	return drawn_planets->p[i].shrink;
}

double planet_native_dist(int i)
{
	const double *p = drawn_planets->p[i].pos;
	return sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
}

/* The same planet in another pass: the same radius, give or take rounding */
static int find_dist(const PlanetDists *d, float radius)
{
	for (int i = 0; i < d->n; i++)
		if (fabsf(d->radius[i] - radius) <= radius * 1e-3f)
			return i;
	return -1;
}

static void record_dist(const PlanetDesc *p)
{
	if (cockpit_turned_pass() || p->radius <= 0.0f)
		return;
	int i = find_dist(&dists_rec, p->radius);
	if (i < 0)
	{
		if (dists_rec.n >= DIST_PLANETS)
			return;
		i = dists_rec.n++;
	}
	double x = p->pos.x, y = p->pos.y, z = p->pos.z;
	dists_rec.radius[i] = p->radius;
	dists_rec.dist[i] = sqrt(x * x + y * y + z * z);
	memcpy(dists_rec.palette[i], p->palette, sizeof dists_rec.palette[i]);
	dists_rec.light_col[i] = p->light_col;
	dists_rec.obj_col[i] = p->obj_col;
}

/* The game rounds a planet's position to 14 bits (L3cd9c "itsybitsyfy"),
 * so close to the ground its sphere is out by up to thousands of metres,
 * and in steps as you climb: the horizon dips below where the surface
 * objects stand, and mountains float over a band of sky. The ship's own
 * altitude (A6+692, L622de, in view units) is exact: when the camera is
 * the ship's and the planet's distance is within that rounding of radius
 * + altitude, the planet is the one it is measured from; its centre goes
 * where it puts the ground just that far below the eye. Its direction,
 * size and look stay the game's. */
static void ground_planet(PlanetDesc *p)
{
	int32_t alt = (int32_t)STMemory_ReadLong(GAME_A6 + 692);
	if (alt < 0 || STMemory_ReadByte(GAME_A6 + A6_3DVIEW_MODE) == VIEW_EXTERNAL || p->radius <= 0.0f)
		return;
	double x = p->pos.x, y = p->pos.y, z = p->pos.z;
	double d = sqrt(x * x + y * y + z * z);
	/* altitude is the ship's, the eye is about 5 m (5000 units) above the
	 * ground when landed (a starport's ground objects at Mars) */
	double want = (double)p->radius + (alt + 5000.0) / native_frame_shrink(p->ctx.a[6]);
	if (d <= 0 || fabs(want - d) > d / 1024) /* each part and the radius cut to 2^-14 */
		return;
	double k = want / d;
	p->pos.x = (int32_t)lround(x * k);
	p->pos.y = (int32_t)lround(y * k);
	p->pos.z = (int32_t)lround(z * k);
}

void Nu_PutPlanet(void)
{
	int words = list_words((uint32_t)GetReg(REG_A6));
	PlanetDesc *p = scene_record(PRIM_PLANET, sizeof *p + (size_t)words * sizeof p->list[0]);
	if (!p)
		return;

	for (int i = 0; i < 8; i++)
	{
		p->ctx.d[i] = GetReg(REG_D0 + i);
		p->ctx.a[i] = (uint32_t)GetReg(REG_A0 + i);
	}

	p->obj_col = GetReg(REG_D6);
	p->light_col = GetReg(REG_D1);
	p->radius = (float)GetReg(REG_D0);
	gl_read_light_dir(GetReg(REG_A1), p->light);
	p->pos = m68k_vertex(GetReg(REG_A0) + 4);
	ground_planet(p);

	/* 3x3 rotation matrix of signed 1.15 fixed point words, stored at A6-36 */
	uint32_t m = GetReg(REG_A6) - 36;
	for (int i = 0; i < 9; i++)
	{
		p->rot[i] = STMemory_ReadWord(m + i * 2) / -32768.0f;
		p->axes[i] = STMemory_ReadWord(m + i * 2) / 32768.0f;
	}

	capture_list(p, words);
	capture_surface(p);
	capture_palette(p);
	record_dist(p);
	record_native(p);
	/* the rest of the planet's model (rings) is the native scene's */
	if (native_mode() == 2)
		scene_mute_object(p->ctx.a[6]);

	float centre[3];
	vec3i_to_f(p->pos, centre);
	atmos_place_bands(centre, p->radius);
}

/* =========================================================================
 * GPU resources
 *
 * Planets are exact spheres ray traced per pixel, drawn over a screen
 * rectangle that bounds the planet. That gives a perfectly round silhouette
 * at any distance (no mesh facets or level-of-detail pops) and puts the
 * ground exactly where the game's surface objects expect it when you are
 * landed. Everything is in units of the planet radius so the maths stays
 * precise in float even metres above a planet millions of units big.
 * ========================================================================= */
static GLuint sphere_prog, shell_prog, quad_vao, quad_vbo;
static struct
{
	GLint rect, proj, centre, c, light_dir, diffuse, ambient, surface, textured, to_local, palette, flat;
	GLint shadow_k, shadow_on, shadow_u, shadow_v, shadow_d, shadow_u2, shadow_v2, shadow_d2, shadow_layer, shadow_dark, shadow_bias,
		shadow, to_view, view_r;
} us;
static struct
{
	GLint rect, proj, dir, cos_edges, colour;
} ush;

static const char *VS_SPHERE =
	"in vec2 aPos;\n"       /* unit quad 0..1 */
	"uniform vec4 uRect;\n" /* NDC x0 y0 x1 y1 to cover */
	"uniform vec4 uProj;\n" /* m[0], m[5], m[8], m[9] of the projection */
	"out vec3 vRay;\n"
	"void main(){\n"
	"  vec2 ndc = mix(uRect.xy, uRect.zw, aPos);\n"
	"  gl_Position = vec4(ndc, 0.0, 1.0);\n"
	"  vRay = vec3((ndc.x + uProj.z) / uProj.x, (ndc.y + uProj.w) / uProj.y, -1.0);\n"
	"}\n";

static const char *FS_SPHERE =
	"precision highp float;\n" GL_SHADOW_GLSL
	"in vec3 vRay;\n"
	"uniform vec3 uCentre;\n" /* sphere centre / radius, eye space */
	"uniform float uC;\n"     /* |centre|^2 - 1, computed in double on the CPU */
	"uniform vec3 uLightDir;\n"
	"uniform vec3 uDiffuse;\n"
	"uniform vec3 uAmbient;\n"
	"uniform sampler2D uSurface;\n" /* the surface, equirectangular in the planet's axes */
	"uniform int uTextured;\n"
	"uniform mat3 uToLocal;\n" /* eye space -> the planet's axes */
	"uniform vec3 uPalette[16];\n" /* by code / 4: dark 0-7, lit 8-15 */
	"uniform float uFlat;\n" /* the sphere drawn is this many times the planet (draw_sphere) */
	/* ship shadows (gl_shadow.h): the map, the rows of the view -> map matrix, and the
	 * eye -> view turn and size of the unit the sphere is drawn in */
	"uniform float uShadowOn;\n"
	"uniform vec4 uShadowU;\n"
	"uniform vec4 uShadowV;\n"
	"uniform vec4 uShadowD;\n"
	"uniform vec4 uShadowU2;\n" /* the second cascade */
	"uniform vec4 uShadowV2;\n"
	"uniform vec4 uShadowD2;\n"
	"uniform vec2 uShadowLayer;\n" /* each cascade's layer, negative: none */
	"uniform float uShadowDark;\n"
	"uniform vec2 uShadowBias;\n"
	"uniform sampler2DArrayShadow uShadow;\n"
	"uniform mat3 uToView;\n"
	"uniform float uViewR;\n"
	"uniform float uShadowK;\n"
	"out vec4 fragColor;\n"
	"void main(){\n"
	"  vec3 d = normalize(vRay);\n"
	"  float b = dot(d, uCentre);\n"
	"  vec3 n;\n"
	"  float tt = 0.0;\n"
	"  if (uFlat > 1.0) {\n"
	/* flattened, the eye just over a huge sphere: b and uC are both tiny
	 * near the horizon, so b^2 - uC rather than 1 - |c x d|^2, and the
	 * near root without cancelling */
	"    float disc = b * b - uC;\n"
	"    if (disc < 0.0 || b <= 0.0) discard;\n"
	"    float t = uC / (b + sqrt(disc));\n"
	"    tt = t;\n"
	/* the point is as far over the ground from the one under the eye as on
	 * the planet itself, so its surface and light are the planet's there:
	 * its normal turned uFlat times as far from straight up */
	"    vec3 up = -normalize(uCentre), p = t * d;\n"
	"    vec3 w = p - dot(p, up) * up;\n"
	"    float sw = length(w);\n"
	"    float a = atan(sw, dot(p, up) + length(uCentre)) * uFlat;\n"
	"    n = sw > 0.0 ? up * cos(a) + (w / sw) * sin(a) : up;\n"
	"  } else {\n"
	/* 1 - (distance from centre to the ray)^2; the cross product form avoids
	 * cancelling two large numbers when the planet is far away */
	"  vec3 x = cross(uCentre, d);\n"
	"  float disc = 1.0 - dot(x, x);\n"
	"  if (disc < 0.0) discard;\n"
	"  float s = sqrt(disc);\n"
	"  float t = (uC > 0.0) ? b - s : b + s;\n" /* outside: near hit; inside: far hit */
	"  if (t <= 0.0) discard;\n"
	"  n = t * d - uCentre;\n"
	"  tt = t;\n"
	"  }\n"
	"  float l = max(dot(normalize(n), uLightDir), 0.0);\n"
	/* in a ship's shadow, on the lit side */
	"  float sh = 1.0;\n"
	"  if (uShadowOn > 0.5 && l > 0.0) {\n"
	"    vec3 pv = (uToView * (tt * d)) * uViewR * uShadowK;\n"
	"    float lit = 1.0;\n"
	"    vec3 s = vec3(dot(uShadowU.xyz, pv) + uShadowU.w, dot(uShadowV.xyz, pv) + uShadowV.w, dot(uShadowD.xyz, pv) + uShadowD.w);\n"
	"    if (uShadowLayer.x >= 0.0 && s.x >= 0.0 && s.x <= 1.0 && s.y >= 0.0 && s.y <= 1.0)\n"
	"      lit = shadowLit(uShadow, s.xy, uShadowLayer.x, min(s.z, 1.0) - uShadowBias.x);\n"
	"    vec3 s2 = vec3(dot(uShadowU2.xyz, pv) + uShadowU2.w, dot(uShadowV2.xyz, pv) + uShadowV2.w, dot(uShadowD2.xyz, pv) + uShadowD2.w);\n"
	"    if (uShadowLayer.y >= 0.0 && s2.x >= 0.0 && s2.x <= 1.0 && s2.y >= 0.0 && s2.y <= 1.0)\n"
	"      lit = min(lit, shadowLit(uShadow, s2.xy, uShadowLayer.y, min(s2.z, 1.0) - uShadowBias.y));\n"
	"    sh = mix(uShadowDark, 1.0, lit);\n"
	"  }\n"
	"  if (uTextured != 0) {\n"
	"    vec3 p = normalize(uToLocal * n);\n"
	"    vec2 uv = vec2(atan(p.x, p.z) / 6.2831853 + 0.5, acos(clamp(p.y, -1.0, 1.0)) / 3.14159265);\n"
	/* a hard terminator, one pixel wide: the game's flat shaded look */
	"    float ld = dot(normalize(n), uLightDir);\n"
	"    float lit = clamp(ld / max(fwidth(ld), 1e-6) + 0.5, 0.0, 1.0);\n"
	/* three signed distances (0.5 on the edge), one per colour bit: each bit's
	 * coverage is about one pixel wide, whatever the zoom; the colour mixes
	 * the eight palette entries by the coverages */
		"    vec3 sd = texture(uSurface, uv).rgb;\n"
		"    vec3 w = max(fwidth(sd), vec3(1e-5));\n"
		"    vec3 c = clamp((sd - 0.5) / w + 0.5, 0.0, 1.0);\n"
		"    vec3 col = vec3(0.0);\n"
		"    for (int k = 0; k < 8; k++) {\n"
		"      float wt = ((k & 1) != 0 ? c.x : 1.0 - c.x) * ((k & 2) != 0 ? c.y : 1.0 - c.y) * ((k & 4) != 0 ? c.z : 1.0 - c.z);\n"
		"      if (wt > 0.0) col += wt * mix(uPalette[k], uPalette[k + 8], lit);\n"
		"    }\n"
		"    fragColor = vec4(col * sh, 1.0);\n"
	"  } else\n"
	"    fragColor = vec4(clamp(uAmbient + (l > 0.0 ? 1.0 : 0.0) * uDiffuse, 0.0, 1.0) * sh, 1.0);\n"
	"}\n";

/* An atmosphere band: the directions between two angles from the planet's */
static const char *FS_SHELL = "precision highp float;\n"
							  "in vec3 vRay;\n"
							  "uniform vec3 uDir;\n" /* to the planet's centre, eye space, unit */
							  "uniform vec2 uCosEdges;\n" /* outer, inner */
							  "uniform vec3 uColour;\n"
							  "out vec4 fragColor;\n"
							  "void main(){\n"
							  "  float c = dot(normalize(vRay), uDir);\n"
							  "  if (c < uCosEdges.x || c > uCosEdges.y) discard;\n"
							  "  fragColor = vec4(uColour, 1.0);\n"
							  "}\n";

void gl_planet_init(void)
{
	static const char *const attribs[] = {"aPos"};
	sphere_prog = gd_build_program(VS_SPHERE, FS_SPHERE, attribs, 1);
	shell_prog = gd_build_program(VS_SPHERE, FS_SHELL, attribs, 1);
	ush.rect = glGetUniformLocation(shell_prog, "uRect");
	ush.proj = glGetUniformLocation(shell_prog, "uProj");
	ush.dir = glGetUniformLocation(shell_prog, "uDir");
	ush.cos_edges = glGetUniformLocation(shell_prog, "uCosEdges");
	ush.colour = glGetUniformLocation(shell_prog, "uColour");
	us.rect = glGetUniformLocation(sphere_prog, "uRect");
	us.proj = glGetUniformLocation(sphere_prog, "uProj");
	us.centre = glGetUniformLocation(sphere_prog, "uCentre");
	us.c = glGetUniformLocation(sphere_prog, "uC");
	us.light_dir = glGetUniformLocation(sphere_prog, "uLightDir");
	us.diffuse = glGetUniformLocation(sphere_prog, "uDiffuse");
	us.ambient = glGetUniformLocation(sphere_prog, "uAmbient");
	us.surface = glGetUniformLocation(sphere_prog, "uSurface");
	us.textured = glGetUniformLocation(sphere_prog, "uTextured");
	us.to_local = glGetUniformLocation(sphere_prog, "uToLocal");
	us.palette = glGetUniformLocation(sphere_prog, "uPalette");
	us.flat = glGetUniformLocation(sphere_prog, "uFlat");
	us.shadow_on = glGetUniformLocation(sphere_prog, "uShadowOn");
	us.shadow_u = glGetUniformLocation(sphere_prog, "uShadowU");
	us.shadow_v = glGetUniformLocation(sphere_prog, "uShadowV");
	us.shadow_d = glGetUniformLocation(sphere_prog, "uShadowD");
	us.shadow_u2 = glGetUniformLocation(sphere_prog, "uShadowU2");
	us.shadow_v2 = glGetUniformLocation(sphere_prog, "uShadowV2");
	us.shadow_d2 = glGetUniformLocation(sphere_prog, "uShadowD2");
	us.shadow_layer = glGetUniformLocation(sphere_prog, "uShadowLayer");
	us.shadow_dark = glGetUniformLocation(sphere_prog, "uShadowDark");
	us.shadow_bias = glGetUniformLocation(sphere_prog, "uShadowBias");
	us.shadow = glGetUniformLocation(sphere_prog, "uShadow");
	us.to_view = glGetUniformLocation(sphere_prog, "uToView");
	us.view_r = glGetUniformLocation(sphere_prog, "uViewR");
	us.shadow_k = glGetUniformLocation(sphere_prog, "uShadowK");

	static const float quad[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
	glGenVertexArrays(1, &quad_vao);
	glGenBuffers(1, &quad_vbo);
	glBindVertexArray(quad_vao);
	glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);
	glBindVertexArray(0);
}

void gl_planet_shutdown(void)
{
	glDeleteBuffers(1, &quad_vbo);
	glDeleteVertexArrays(1, &quad_vao);
	glDeleteProgram(sphere_prog);
	glDeleteProgram(shell_prog);
	planet_tex_shutdown();
}

/* =========================================================================
 * Drawing (runs at present time)
 * ========================================================================= */
typedef struct
{
	float rect[4];
	float proj[4];
	float centre[3];
	float c;
	float light[3], diffuse[3], ambient[3];
	float flat;
	float to_view[9]; /* eye space -> view space (the head's turn undone), column major */
	float view_r;     /* view units to the unit the sphere is drawn in */
	float shadow_k;   /* the ground the ships' shadows fall on is this many times as far below the eye as the sphere's */
	GLuint surface;   /* 0: plain */
	float to_local[9];
	float palette[16][3];
} SpherePass;

static void planet_sphere_pass(const void *data)
{
	const SpherePass *p = data;
	glUseProgram(sphere_prog);
	glUniform4fv(us.rect, 1, p->rect);
	glUniform4fv(us.proj, 1, p->proj);
	glUniform3fv(us.centre, 1, p->centre);
	glUniform1f(us.c, p->c);
	glUniform1f(us.flat, p->flat);
	glUniform3fv(us.light_dir, 1, p->light);
	glUniform3fv(us.diffuse, 1, p->diffuse);
	glUniform3fv(us.ambient, 1, p->ambient);
	glUniform1i(us.textured, p->surface != 0);
	/* the ships' shadows, near the ground */
	static const float no_map[12];
	const float *sm = gl_shadow_matrix(0), *sm2 = gl_shadow_matrix(1);
	GLuint stex = gl_shadow_texture();
	if (!sm)
		sm = no_map;
	if (!sm2)
		sm2 = no_map;
	glUniform1f(us.shadow_on, stex ? 1.0f : 0.0f);
	if (stex)
	{
		glActiveTexture(GL_TEXTURE0 + 5);
		glBindTexture(GL_TEXTURE_2D_ARRAY, stex);
		glActiveTexture(GL_TEXTURE0);
		glUniform1i(us.shadow, 5);
		glUniform4fv(us.shadow_u, 1, sm);
		glUniform4fv(us.shadow_v, 1, sm + 4);
		glUniform4fv(us.shadow_d, 1, sm + 8);
		glUniform4fv(us.shadow_u2, 1, sm2);
		glUniform4fv(us.shadow_v2, 1, sm2 + 4);
		glUniform4fv(us.shadow_d2, 1, sm2 + 8);
		glUniform2f(us.shadow_layer, (float)gl_shadow_all_layer(0), (float)gl_shadow_all_layer(1));
		glUniform1f(us.shadow_dark, gl_shadow_dark());
		glUniform2f(us.shadow_bias, gl_shadow_bias(0), gl_shadow_bias(1));
		glUniformMatrix3fv(us.to_view, 1, GL_FALSE, p->to_view);
		glUniform1f(us.view_r, p->view_r);
		glUniform1f(us.shadow_k, p->shadow_k);
	}
	if (p->surface)
	{
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, p->surface);
		glUniform1i(us.surface, 0);
		glUniformMatrix3fv(us.to_local, 1, GL_FALSE, p->to_local);
		glUniform3fv(us.palette, 16, &p->palette[0][0]);
	}
	glBindVertexArray(quad_vao);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* NDC rectangle covering the sphere: the projected corners of its bounding
 * cube. Falls back to the whole view if any corner is at or behind the
 * camera. Returns false if the planet is entirely off screen. */
static bool screen_rect(const double centre[3], float radius, float rect[4])
{
	const mat4 *pr = gd_projection();
	double r = radius * 1.01;
	float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
	for (int i = 0; i < 8; i++)
	{
		float c[3] = {(float)(centre[0] + ((i & 1) ? r : -r)), (float)(centre[1] + ((i & 2) ? r : -r)),
					  (float)(centre[2] + ((i & 4) ? r : -r))};
		float clip[4];
		mat4_xform(pr, c[0], c[1], c[2], clip);
		if (clip[3] <= 1e-3f * fabsf(c[2]) || c[2] >= 0.0f)
		{
			rect[0] = rect[1] = -1.0f;
			rect[2] = rect[3] = 1.0f;
			return true;
		}
		float x = clip[0] / clip[3], y = clip[1] / clip[3];
		x0 = fminf(x0, x);
		y0 = fminf(y0, y);
		x1 = fmaxf(x1, x);
		y1 = fmaxf(y1, y);
	}
	rect[0] = fmaxf(x0, -1.0f);
	rect[1] = fmaxf(y0, -1.0f);
	rect[2] = fminf(x1, 1.0f);
	rect[3] = fminf(y1, 1.0f);
	return rect[0] < rect[2] && rect[1] < rect[3];
}

static int table_colour(int code, const void *ctx)
{
	return ((const int16_t *)ctx)[(code >> 2) & 15] & 0xfff;
}

/* Draws the planet exactly as the game's software renderer would, from its
 * 2D shape list: outline, shading bands, surface features and, close up,
 * the ground with the horizon where the game puts it. */
static void draw_list(const PlanetDesc *p)
{
	fill2d_begin();
	for (int i = 0; i < p->list_len;)
	{
		const int16_t *a = &p->list[i + 1];
		switch (p->list[i])
		{
		case PL_START:
			fill2d_begin();
			i += 1;
			break;
		case PL_LINE:
			fill2d_line(a[0], a[1], a[2], a[3], a[4]);
			i += 6;
			break;
		case PL_BEZIER:
			fill2d_bezier(a, a[8]);
			i += 10;
			break;
		case PL_ROW:
			fill2d_row_code(a[0], a[1]);
			i += 3;
			break;
		case PL_COLOURS:
			fill2d_draw(FILL2D_PLANET, table_colour, a);
			i += 17;
			break;
		default:
			return;
		}
	}
}

/* A planet as the sphere pass draws it */
typedef struct
{
	double pos[3];  /* centre, view space as drawn (GL, not turned by the head) */
	float radius;
	float light[3]; /* view space */
	int light_col, obj_col;
	const uint16_t *palette; /* 16 rgb444 */
	const float *axes;       /* PlanetDesc.axes */
	const PlanetSurface *surface; /* null: plain */
} SphereIn;

/* The game lays out a planet's surface (cities, mountains, starports) on
 * flat ground: out to 35 km and more, their feet stay about level with the
 * ground under the ship, while the planet curves away from it. Its horizon
 * then dips below them, and they stand over a band of sky. Close by, the
 * planet is drawn flatter, as a sphere `k` times as big touching the same
 * ground under the eye: over FLAT_REACH (view units, mm) its horizon dips
 * no more than objects there stand below the eye, so they stand on it. k
 * is 1 again once the true planet's horizon is that low anyway (some km
 * up). The surface and light are still mapped as on the planet. */
#define FLAT_REACH 6.0e7
static double flatten(const double pos[3], double radius)
{
	double d = sqrt(pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2]);
	double h = d - radius;
	if (h <= 0 || radius <= 0)
		return 1;
	double k = 2 * FLAT_REACH * FLAT_REACH / (h * radius);
	return k < 1 ? 1 : k > 1e5 ? 1e5 : k;
}

static void draw_sphere(const SphereIn *in)
{
	/* The sphere shader takes a plain lens (no rotation): in the cockpit,
	 * turn the planet by the head instead */
	mat4 lens = *gd_projection(), head = mat4_identity();
	if (cockpit_active())
		cockpit_world_split(&lens, &head);

	/* The eye is never inside a planet. Just off the ground the planet's
	 * distance can come a little short (the game's own, a turned pass's
	 * rounded camera, a remembered planet), and from inside the sphere
	 * shows its far side: the other hemisphere's seas on the ground and
	 * the ground's colours all over the sky. Keep the eye just over it. */
	double centre[3] = {in->pos[0], in->pos[1], in->pos[2]};
	double dist = sqrt(centre[0] * centre[0] + centre[1] * centre[1] + centre[2] * centre[2]);
	double lowest = in->radius * (1.0 + 1e-6);
	if (dist > 0 && dist < lowest)
	{
		for (int k = 0; k < 3; k++)
			centre[k] *= lowest / dist;
		dist = lowest;
	}

	/* flattened: the centre k times as far below the ground under the eye */
	double flat = flatten(centre, in->radius);
	double h = dist - in->radius, r = in->radius * flat, pos[3];
	for (int k = 0; k < 3; k++)
		pos[k] = flat > 1 ? centre[k] * (h + r) / dist : centre[k];

	float rect[4];
	if (!screen_rect(pos, (float)r, rect))
		return;
	{
		static int dn;
		if (getenv("GLF_SHIP_SHADOWS") && atoi(getenv("GLF_SHIP_SHADOWS")) == 2 && dn++ % 200 < 2)
			log_printf("sphere: dist %.0f radius %.0f h %.0f flat %.1f in-radius %.0f\n", dist, (double)in->radius, h,
					   flat, (double)in->radius);
	}

	/* the head's turn in double: the centre may be millions of units off */
	double c[3];
	for (int k = 0; k < 3; k++)
		c[k] = (head.m[k] * pos[0] + head.m[4 + k] * pos[1] + head.m[8 + k] * pos[2]) / r;
	float light[3];
	mat4_xform_dir(&head, in->light, light);

	SpherePass *pass = gd_custom(planet_sphere_pass, sizeof *pass);
	if (!pass)
		return;
	memcpy(pass->rect, rect, sizeof(rect));
	pass->proj[0] = lens.m[0];
	pass->proj[1] = lens.m[5];
	pass->proj[2] = lens.m[8];
	pass->proj[3] = lens.m[9];

	for (int k = 0; k < 3; k++)
		pass->centre[k] = (float)c[k];
	/* |c|^2 - 1 = (2 h r + h^2) / r^2 without cancelling (h tiny next to a
	 * flattened r) */
	pass->c = flat > 1 ? (float)((2 * h * r + h * h) / (r * r))
					   : (float)(c[0] * c[0] + c[1] * c[1] + c[2] * c[2] - 1.0);
	pass->flat = (float)flat;
	/* view = H^T eye: column j of the GL matrix holds head.m[i * 4 + j] over i */
	for (int j = 0; j < 3; j++)
		for (int i = 0; i < 3; i++)
			pass->to_view[j * 3 + i] = head.m[i * 4 + j];
	pass->view_r = (float)r;
	/* Near the ground the sphere can sit well above the ground the scenery
	 * stands on (the planet's drawn radius is a 16 bit mantissa: a Merlin pad
	 * 258 m down had the sphere 16 m down), and a shadow cast on it would be
	 * that much too big. Cast it on the ground at the game's altitude. */
	pass->shadow_k = 1.0f;
	if (flat > 1 && h > 0 && STMemory_ReadByte(GAME_A6 + A6_3DVIEW_MODE) != VIEW_EXTERNAL)
	{
		double alt = (double)(int32_t)STMemory_ReadLong(GAME_A6 + 692);
		if (alt > h)
			pass->shadow_k = (float)(alt / h < 200 ? alt / h : 200);
	}

	memcpy(pass->light, light, sizeof(pass->light));
	vec3_normalize(pass->light);
	gl_rgb444_to_f(in->light_col, pass->diffuse);
	gl_rgb444_to_f(in->obj_col, pass->ambient);

	/* The surface: eye space -> view space (H^T) -> the game's view (z
	 * flipped, S) -> the planet's axes (A^T) */
	pass->surface = in->surface ? planet_surface_texture(in->surface) : 0;
	for (int i = 0; i < 16; i++)
		gl_rgb444_to_f(in->palette[i], pass->palette[i]);
	static const float s[3] = {1, 1, -1};
	for (int i = 0; i < 3; i++)
		for (int k = 0; k < 3; k++)
		{
			float v = 0;
			for (int j = 0; j < 3; j++)
				v += in->axes[3 * i + j] * s[j] * head.m[j * 4 + k];
			pass->to_local[k * 3 + i] = v;
		}
}

void draw_planet(const void *payload)
{
	const PlanetDesc *p = payload;
	if (p->radius <= 0.0f || native_mode() == 2) /* native_draw_sky draws it */
		return;

	/* The game's shape only fits the view it was made for: the cockpit's
	 * turned passes would each draw a slightly different one, so there the
	 * planet is the sphere, which every pass draws the same. */
	static int native = -1;
	if (native < 0)
		native = !getenv("GLF_NATIVE_PLANETS") || atoi(getenv("GLF_NATIVE_PLANETS")) > 0;
	if (p->list_ok && !cockpit_active() && !(native && p->surface_ok))
	{
		draw_list(p);
		return;
	}

	SphereIn in = {{p->pos.x, p->pos.y, p->pos.z}, p->radius, {p->light[0], p->light[1], p->light[2]},
				   p->light_col, p->obj_col, p->palette, p->axes, p->surface_ok ? &p->surface : NULL};
	if (cockpit_active())
	{
		int i = find_dist(&dists_show, p->radius);
		double len = sqrt(in.pos[0] * in.pos[0] + in.pos[1] * in.pos[1] + in.pos[2] * in.pos[2]);
		if (i >= 0)
		{
			in.palette = dists_show.palette[i];
			in.light_col = dists_show.light_col[i];
			in.obj_col = dists_show.obj_col[i];
			if (len > 0.0)
				for (int k = 0; k < 3; k++)
					in.pos[k] *= dists_show.dist[i] / len;
		}
	}
	draw_sphere(&in);
}

void planet_native_draw(int i)
{
	const NativePlanet *n = &drawn_planets->p[i];
	SphereIn in = {{n->pos[0], n->pos[1], n->pos[2]},
				   n->radius,
				   {n->light[0], n->light[1], n->light[2]},
				   n->light_col,
				   n->obj_col,
				   n->palette,
				   n->axes,
				   n->surface_ok ? &n->surface : NULL};
	draw_sphere(&in);
}

typedef struct
{
	float proj[4];
	float dir[3];
	float cos_edges[2];
	float colour[3];
} ShellPass;

static void planet_shell_pass(const void *data)
{
	const ShellPass *p = data;
	static const float whole[4] = {-1, -1, 1, 1};
	glUseProgram(shell_prog);
	glUniform4fv(ush.rect, 1, whole);
	glUniform4fv(ush.proj, 1, p->proj);
	glUniform3fv(ush.dir, 1, p->dir);
	glUniform2fv(ush.cos_edges, 1, p->cos_edges);
	glUniform3fv(ush.colour, 1, p->colour);
	glBindVertexArray(quad_vao);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void planet_draw_shell(const float centre[3], float cos_outer, float cos_inner, int rgb)
{
	mat4 lens = *gd_projection(), head = mat4_identity(), turned;
	if (cockpit_active())
	{
		cockpit_world_split(&lens, &turned);
		head = cockpit_head_rotation();
	}
	float dir[3];
	mat4_xform_dir(&head, centre, dir);
	vec3_normalize(dir);

	ShellPass *pass = gd_custom(planet_shell_pass, sizeof *pass);
	if (!pass)
		return;
	pass->proj[0] = lens.m[0];
	pass->proj[1] = lens.m[5];
	pass->proj[2] = lens.m[8];
	pass->proj[3] = lens.m[9];
	memcpy(pass->dir, dir, sizeof dir);
	pass->cos_edges[0] = cos_outer;
	pass->cos_edges[1] = cos_inner;
	gl_rgb444_to_f(rgb, pass->colour);
}
