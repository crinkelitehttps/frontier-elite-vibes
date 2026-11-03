/*
 * scenery.c - see scenery.h
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game_state.h"
#include "gl/gl_planet.h"
#include "host.h"
#include "main.h"
#include "scenery.h"

/* The surface is cut into cubes of 2^22 mm (4.2 km) in the planet's frame; a cell holds a feature when
   its hash says so (1 in 48), and everything about the feature comes from the hash */
#define CELL_SH   22
#define CELL_MM   4194304.0
#define DENSITY   48
#define OBJ_BYTES 286
#define MAX_TMPL  1536
#define MAX_PEND  8192
#define TRIED_N   32768 /* a power of 2 */
#define BUDGET    8     /* features made in a frame */
#define MAX_DRAW  900   /* copies drawn in a frame */

int scenery_radius_km = 80;

typedef struct
{
	uint64_t key; /* 0: free */
	double pos[3]; /* mm, in the planet's frame */
	uint8_t obj[OBJ_BYTES];
} Tmpl;

typedef struct
{
	int i, j, k;
	float d;
} Cell;

static Tmpl tmpl[MAX_TMPL];
static int ntmpl;
static uint32_t cache_seed;
static int cache_slot = -1;

/* Cells already made, or tried and failed (key | 1<<63 is not used: keys are never 0 or 1) */
static uint64_t tried[TRIED_N];
static int tried_count;

static Cell pend[MAX_PEND];
static int npend, pend_at;
static int64_t scan_cell[3];
static bool scan_valid;
static int budget;
static uint64_t pending_key;
static double pending_pos[3];

static int draw_list[MAX_DRAW];
static int ndraw, draw_at;
static uint32_t virt_cur;

/* ---- helpers ------------------------------------------------------------ */

static uint32_t mix32(uint32_t a, uint32_t b)
{
	uint32_t h = a ^ (b * 0x9e3779b1u);
	h ^= h >> 15;
	h *= 0x2c1b3c6du;
	h ^= h >> 12;
	h *= 0x297a2d39u;
	h ^= h >> 15;
	return h;
}

static uint64_t cell_key(int i, int j, int k)
{
	return ((((uint64_t)(uint32_t)(i + 0x100000)) & 0x1fffff) << 42 | (((uint64_t)(uint32_t)(j + 0x100000)) & 0x1fffff) << 21 |
			(((uint64_t)(uint32_t)(k + 0x100000)) & 0x1fffff)) +
		   2;
}

static uint32_t cell_hash(uint32_t seed, int i, int j, int k)
{
	return mix32(mix32(mix32(seed, (uint32_t)i), (uint32_t)j), (uint32_t)k);
}

static bool tried_has(uint64_t key)
{
	for (uint32_t p = (uint32_t)(key * 0x9e3779b97f4a7c15ull >> 40) & (TRIED_N - 1);; p = (p + 1) & (TRIED_N - 1))
	{
		if (tried[p] == key)
			return true;
		if (tried[p] == 0)
			return false;
	}
}

static void tried_add(uint64_t key)
{
	if (tried_count > TRIED_N / 2)
	{
		/* too full: forget the failures, keep the cells made */
		memset(tried, 0, sizeof tried);
		tried_count = 0;
		for (int t = 0; t < MAX_TMPL; t++)
			if (tmpl[t].key)
			{
				uint32_t p = (uint32_t)(tmpl[t].key * 0x9e3779b97f4a7c15ull >> 40) & (TRIED_N - 1);
				while (tried[p])
					p = (p + 1) & (TRIED_N - 1);
				tried[p] = tmpl[t].key;
				tried_count++;
			}
	}
	uint32_t p = (uint32_t)(key * 0x9e3779b97f4a7c15ull >> 40) & (TRIED_N - 1);
	while (tried[p])
	{
		if (tried[p] == key)
			return;
		p = (p + 1) & (TRIED_N - 1);
	}
	tried[p] = key;
	tried_count++;
}

/* 64 bit position at obj+44 (low longs) / +56 (high longs), mm */
static void pos_of(uint32_t obj, double out[3])
{
	for (int a = 0; a < 3; a++)
		out[a] = (double)rdlong(obj + 56 + 4 * (uint32_t)a) * 4294967296.0 + (double)(uint32_t)rdlong(obj + 44 + 4 * (uint32_t)a);
}

static uint32_t seed_of(uint32_t planet)
{
	uint32_t h = (uint16_t)rdword(planet + 90);
	for (int c = 0; c < 8; c++)
		h = mix32(h, (uint8_t)rdbyte(planet + 246 + (uint32_t)c));
	return h;
}

static double radius_of(uint32_t planet)
{
	return (double)(uint16_t)rdword(planet + 270) * 4294967296.0 + (double)(uint32_t)rdlong(planet + 266);
}

static double limit_mm(void)
{
	return (double)scenery_radius_km * 1e6;
}

static double linf(const double a[3], const double b[3])
{
	double m = 0;
	for (int i = 0; i < 3; i++)
	{
		double d = fabs(a[i] - b[i]);
		if (d > m)
			m = d;
	}
	return m;
}

static void flush(uint32_t seed, int slot)
{
	memset(tmpl, 0, sizeof tmpl);
	ntmpl = 0;
	memset(tried, 0, sizeof tried);
	tried_count = 0;
	npend = pend_at = 0;
	scan_valid = false;
	cache_seed = seed;
	cache_slot = slot;
}

static int cmp_cell(const void *a, const void *b)
{
	float x = ((const Cell *)a)->d, y = ((const Cell *)b)->d;
	return x < y ? -1 : x > y;
}

/* Every cell the surface passes through within the limit of the ship (L-infinity, the game's own distance),
   not yet tried, that holds a feature, nearest first */
static void scan(const double s[3], double rp, uint32_t seed)
{
	npend = pend_at = 0;
	double c = CELL_MM, lim = limit_mm() + c;
	double rc = rp / c, w = 0.8661; /* half a cell's diagonal */
	double sc[3] = {s[0] / c, s[1] / c, s[2] / c};
	int n = (int)ceil(lim / c);
	int i0 = (int)floor(sc[0]), j0 = (int)floor(sc[1]), k0 = (int)floor(sc[2]);
	double hi2 = (rc + w) * (rc + w), lo = rc - w, lo2 = lo > 0 ? lo * lo : 0;
	for (int i = i0 - n; i <= i0 + n; i++)
	{
		double x = i + 0.5;
		for (int j = j0 - n; j <= j0 + n; j++)
		{
			double y = j + 0.5, rho2 = x * x + y * y;
			if (rho2 > hi2)
				continue;
			double zmax = sqrt(hi2 - rho2), zmin = lo2 > rho2 ? sqrt(lo2 - rho2) : 0;
			for (int side = 0; side < 2; side++)
			{
				int kmin, kmax;
				if (!side)
				{
					kmin = (int)ceil(zmin - 0.5);
					kmax = (int)floor(zmax - 0.5);
					if (kmin < 0)
						kmin = 0;
				}
				else
				{
					kmin = (int)ceil(-zmax - 0.5);
					kmax = (int)floor(-zmin - 0.5);
					if (kmax > -1)
						kmax = -1;
				}
				if (kmin < k0 - n)
					kmin = k0 - n;
				if (kmax > k0 + n)
					kmax = k0 + n;
				for (int k = kmin; k <= kmax; k++)
				{
					double dx = fabs(x - sc[0]), dy = fabs(y - sc[1]), dz = fabs(k + 0.5 - sc[2]);
					double d = dx > dy ? dx : dy;
					if (dz > d)
						d = dz;
					if (d * c > lim)
						continue;
					if ((cell_hash(seed, i, j, k) >> 16) % DENSITY != 0)
						continue;
					if (tried_has(cell_key(i, j, k)) || npend >= MAX_PEND)
						continue;
					pend[npend++] = (Cell){i, j, k, (float)d};
				}
			}
		}
	}
	qsort(pend, (size_t)npend, sizeof pend[0], cmp_cell);
	for (int a = 0; a < 3; a++)
		scan_cell[a] = (int64_t)floor(sc[a]);
	scan_valid = true;
}

static int cmp_f(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;
	return x < y ? -1 : x > y;
}

/* ---- host calls ----------------------------------------------------------- */

/* hcall Call_ScenDrive (Lscen_drive, once per run of the object loop, whatever the spawner's own rules): d6 = 1 and
   a0 = the player's ship when the object table in a4 is the flight's, so the features around it can be made */
void Call_ScenDrive(void)
{
	SetReg(REG_D6, 0);
	uint32_t tab = (uint32_t)GetReg(REG_A4), ship = game_player_ship();
	if (!ship || !tab || scenery_radius_km <= 0)
		return;
	if (tab + (uint16_t)rdword(FE2_obj_offsets + 2 * (uint32_t)(uint8_t)rdbyte(ship + 93)) != ship)
		return;
	SetReg(REG_A0, (int32_t)ship);
	SetReg(REG_D6, 1);
}

/* hcall Call_ScenNext (Lscen_pick): a0 the player's ship, a1 the planet it is over, d7 = 1 on the first call of
   a frame. Sets d6 = 1 and the ray (d0-2 origin from the ship, d3-5 direction) of the next feature to make */
void Call_ScenNext(void)
{
	SetReg(REG_D6, 0);
	uint32_t ship = (uint32_t)GetReg(REG_A0), planet = (uint32_t)GetReg(REG_A1);
	if (GetReg(REG_D7) & 1)
		budget = BUDGET;
	if (budget <= 0 || scenery_radius_km <= 0)
		return;
	double rp = radius_of(planet);
	if (rp < 4.0 * CELL_MM || rp > 16000.0 * CELL_MM)
		return;
	int slot = (uint8_t)rdbyte(ship + 68);
	uint32_t seed = seed_of(planet);
	if (seed != cache_seed || slot != cache_slot)
		flush(seed, slot);
	double s[3];
	pos_of(ship, s);

	/* copies too far to be drawn any more go, and their cells may be made again */
	double lim = limit_mm();
	for (int t = 0; t < MAX_TMPL; t++)
		if (tmpl[t].key && linf(tmpl[t].pos, s) > lim * 1.3 + CELL_MM)
		{
			uint32_t p = (uint32_t)(tmpl[t].key * 0x9e3779b97f4a7c15ull >> 40) & (TRIED_N - 1);
			while (tried[p] && tried[p] != tmpl[t].key)
				p = (p + 1) & (TRIED_N - 1);
			if (tried[p])
				tried[p] = 1; /* a tombstone: keeps the probe chain */
			tmpl[t].key = 0;
			ntmpl--;
		}

	if (!scan_valid || (int64_t)floor(s[0] / CELL_MM) != scan_cell[0] || (int64_t)floor(s[1] / CELL_MM) != scan_cell[1] ||
		(int64_t)floor(s[2] / CELL_MM) != scan_cell[2])
		scan(s, rp, seed);

	while (pend_at < npend)
	{
		Cell c = pend[pend_at++];
		uint64_t key = cell_key(c.i, c.j, c.k);
		if (tried_has(key))
			continue;
		if (ntmpl >= MAX_TMPL - 1)
			return;
		uint32_t h = cell_hash(seed, c.i, c.j, c.k);
		/* the place in the cell and the ray from 2 cells above it, straight down */
		double p[3], len = 0;
		int cc[3] = {c.i, c.j, c.k};
		for (int a = 0; a < 3; a++)
		{
			p[a] = (double)cc[a] * CELL_MM + (double)(mix32(h, (uint32_t)a + 1) & 0x3fffff);
			len += p[a] * p[a];
		}
		len = sqrt(len);
		if (len < 1)
			continue;
		/* over liquid sea only clouds: the kind becomes the first cloud of the planet type's row, none: no feature */
		uint32_t kind = (h >> 8) & 7;
		int row_i = (int)(uint16_t)rdword(planet + 90) - 0x7a;
		if (row_i >= 0 && row_i <= 30 && planet_is_sea(planet, p) > 0)
		{
			uint32_t row = FE2_Lscen_rows + (uint32_t)(int32_t)(int16_t)rdword(FE2_Lscen_rows + (uint32_t)row_i);
			uint32_t pick = 8;
			for (uint32_t k = 0; k < 8 && pick == 8; k++)
				if ((uint16_t)rdword(row + 4 * ((kind + k) & 7)) == 0xc6)
					pick = (kind + k) & 7;
			if (pick == 8)
			{
				tried_add(key);
				continue;
			}
			kind = pick;
		}
		double up[3], origin[3];
		for (int a = 0; a < 3; a++)
		{
			up[a] = p[a] / len;
			origin[a] = p[a] + up[a] * 2.0 * CELL_MM - s[a];
			if (fabs(origin[a]) > 2.0e9)
				goto skip;
		}
		SetReg(REG_D0, (int32_t)origin[0]);
		SetReg(REG_D1, (int32_t)origin[1]);
		SetReg(REG_D2, (int32_t)origin[2]);
		SetReg(REG_D3, (int32_t)(-up[0] * 16384.0));
		SetReg(REG_D4, (int32_t)(-up[1] * 16384.0));
		SetReg(REG_D5, (int32_t)(-up[2] * 16384.0));
		wrlong(FE2_Lscen_h, (int32_t)h);
		wrlong(FE2_Lscen_hk, (int32_t)(kind * 4)); /* the kind: one of the planet type's 8 */
		wrlong(FE2_Lscen_ha, (int32_t)mix32(h, 0x3333));
		tried_add(key);
		pending_key = key;
		memcpy(pending_pos, p, sizeof p);
		budget--;
		SetReg(REG_D6, 1);
		return;
	skip:;
	}
}

/* hcall Call_ScenMade (Lscen_made): a1 is the object the game made for the cell given; keep a copy.
   d6 = 1 to go on with the next cell */
void Call_ScenMade(void)
{
	uint32_t o = (uint32_t)GetReg(REG_A1);
	SetReg(REG_D6, 0);
	if (!pending_key)
		return;
	for (int t = 0; t < MAX_TMPL; t++)
		if (!tmpl[t].key)
		{
			tmpl[t].key = pending_key;
			for (int b = 0; b < OBJ_BYTES; b++)
				tmpl[t].obj[b] = (uint8_t)rdbyte(o + (uint32_t)b);
			pos_of(o, tmpl[t].pos);
			ntmpl++;
			break;
		}
	pending_key = 0;
	SetReg(REG_D6, budget > 0 ? 1 : 0);
}

/* hcall Call_VirtNext (Lvirt_a, in the object loop): d6 = 1 starts the list of copies near the camera (a3);
   d0 = 1 when the next is in the buffer Lvirt_obj, 0 when there are no more */
void Call_VirtNext(void)
{
	virt_cur = 0;
	if (GetReg(REG_D6) & 1)
	{
		ndraw = draw_at = 0;
		uint32_t cam = (uint32_t)GetReg(REG_A3), tab = (uint32_t)GetReg(REG_A4);
		uint32_t ship = game_player_ship();
		if (ntmpl > 0 && cam && ship && cache_slot >= 0)
		{
			int slot = (uint8_t)rdbyte(cam + 68);
			/* the flight's own object table (not the system map's copy) and a camera over the planet */
			uint32_t pl = tab + (uint16_t)rdword(FE2_obj_offsets + 2 * (uint32_t)cache_slot);
			if (slot == cache_slot && tab + (uint16_t)rdword(FE2_obj_offsets + 2 * (uint32_t)(uint8_t)rdbyte(ship + 93)) == ship &&
				seed_of(pl) == cache_seed)
			{
				double c[3];
				pos_of(cam, c);
				double lim = limit_mm() + CELL_MM;
				static float dist[MAX_TMPL];
				int all[MAX_TMPL], nall = 0;
				for (int t = 0; t < MAX_TMPL; t++)
					if (tmpl[t].key && (dist[t] = (float)linf(tmpl[t].pos, c)) <= lim)
						all[nall++] = t;
				if (nall > MAX_DRAW)
				{
					/* too many for one frame: the nearest */
					float cut[MAX_TMPL];
					for (int q = 0; q < nall; q++)
						cut[q] = dist[all[q]];
					qsort(cut, (size_t)nall, sizeof cut[0], cmp_f);
					float lim2 = cut[MAX_DRAW - 1];
					for (int q = 0; q < nall && ndraw < MAX_DRAW; q++)
						if (dist[all[q]] <= lim2)
							draw_list[ndraw++] = all[q];
				}
				else
					for (int q = 0; q < nall; q++)
						draw_list[ndraw++] = all[q];
			}
		}
	}
	if (draw_at >= ndraw)
	{
		SetReg(REG_D0, 0);
		return;
	}
	int t = draw_list[draw_at++];
	if (draw_at == 1 && getenv("GLF_SCEN_LOG"))
	{
		static int n;
		if ((n++ % 120) == 0)
			{
				double cc[3];
				int bin[8] = {0};
				pos_of((uint32_t)GetReg(REG_A3), cc);
				for (int q = 0; q < ndraw; q++)
				{
					int b = (int)(linf(tmpl[draw_list[q]].pos, cc) / 20e6);
					bin[b > 7 ? 7 : b]++;
				}
				fprintf(stderr, "scenery: %d copies, %d to draw, %d pending; per 20 km: %d %d %d %d %d %d %d %d\n", ntmpl, ndraw,
						npend - pend_at, bin[0], bin[1], bin[2], bin[3], bin[4], bin[5], bin[6], bin[7]);
			}
	}
	for (int b = 0; b < OBJ_BYTES; b++)
		wrbyte(FE2_Lvirt_obj + (uint32_t)b, tmpl[t].obj[b]);
	virt_cur = (uint32_t)t + 1;
	SetReg(REG_D0, 1);
}

uint32_t scenery_virtual_id(void)
{
	return virt_cur;
}
