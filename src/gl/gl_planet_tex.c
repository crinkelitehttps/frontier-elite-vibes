/*
 * gl_planet_tex.c - see gl_planet_tex.h.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "gl_planet_tex.h"
#include "main.h"

#define TEX_W 2048
#define TEX_H 1024

/* The game splits an edge while its counter (16 + detail option, 2 per
 * level) is above 4: six levels at the default detail */
#define LEVELS 6

#define MAX_FEATURES 128
#define MAX_POINTS   2048 /* outline points of all features, before splitting */
#define MAX_OUTLINE  (64 * (1 << LEVELS) + 2)

typedef struct
{
	int code, detail;
	int first, count; /* in points[] */
} Outline;

typedef struct
{
	int code;
	float centre[3];
	float cos_r;
	bool both; /* the opposite one too */
} Circle;

/* A feature in list order: an outline or a circle */
typedef struct
{
	bool circle;
	int index;
} Feature;

typedef struct
{
	Feature features[MAX_FEATURES];
	int n_features;
	Outline outlines[MAX_FEATURES];
	int n_outlines;
	Circle circles[MAX_FEATURES];
	int n_circles;
	float points[MAX_POINTS][3];
	int n_points;
} Parsed;

static int s8(int b)
{
	return (int8_t)(uint8_t)b;
}

static int word_at(const uint8_t *d, int i)
{
	return (d[i] << 8) | d[i + 1];
}

static void normalize3(float v[3])
{
	float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (l > 0.0f)
		for (int k = 0; k < 3; k++)
			v[k] /= l;
}

/* Parses the command's data; false if it doesn't add up to its length */
static bool parse(const PlanetSurface *s, Parsed *p)
{
	const uint8_t *d = s->data;
	const int len = s->len;
	memset(p, 0, sizeof *p);
	if (len < 10)
		return false;
	int flags = d[0];
	int i = 8;
	if (flags & 0x10)
		i += 2 + 56;
	else if (!(flags & 0x40))
		i += 8;
	if (i + 2 > len)
		return false;
	if (word_at(d, i) != 0)
	{
		i += 4;
		for (;;)
		{
			i += 2;
			if (i + 2 > len)
				return false;
			int w = word_at(d, i);
			i += 2;
			if (!w)
				break;
		}
	}
	else
		i += 2;

	for (;;)
	{
		if (i >= len)
			return false;
		int c = d[i++];
		if (c == 0)
			break;
		if (p->n_features >= MAX_FEATURES)
			return false;
		if (c & 0x80)
		{
			if (i + 4 > len)
				return false;
			Circle *ci = &p->circles[p->n_circles];
			for (int k = 0; k < 3; k++)
				ci->centre[k] = (float)-s8(d[i + k]);
			normalize3(ci->centre);
			ci->cos_r = cosf((float)d[i + 3] * 64.0f / 65536.0f * 6.2831853f);
			ci->code = c & 0x1c;
			ci->both = (c & 0x40) != 0;
			i += 4;
			p->features[p->n_features++] = (Feature){true, p->n_circles++};
			continue;
		}
		if (i + 2 > len)
			return false;
		Outline *o = &p->outlines[p->n_outlines];
		o->code = c;
		o->detail = s8(d[i++]);
		o->first = p->n_points;
		int x = d[i++];
		for (;;)
		{
			if (i + 3 > len)
				return false;
			int y = d[i], z = d[i + 1];
			i += 2;
			if (p->n_points == o->first && d[i] == 0)
			{
				/* a marker (detail trigger): skip it and its size */
				i += 2;
				if (i >= len)
					return false;
				x = d[i++];
				continue;
			}
			if (p->n_points >= MAX_POINTS)
				return false;
			float *v = p->points[p->n_points++];
			v[0] = (float)s8(x);
			v[1] = (float)s8(y);
			v[2] = (float)s8(z);
			normalize3(v);
			x = d[i++];
			if (x == 0)
				break;
		}
		o->count = p->n_points - o->first;
		p->features[p->n_features++] = (Feature){false, p->n_outlines++};
	}
	i += i & 1; /* the game keeps its bytecode pointer even */
	return i == len;
}

bool planet_surface_valid(const PlanetSurface *s)
{
	static Parsed p;
	return s->len > 0 && s->len <= PLANET_DATA_MAX && parse(s, &p);
}

/* ---- outlines ------------------------------------------------------------ */

/* The game's pseudo-random sequence (a0 in L3ddc4 / L3de2e): each step adds
 * the value with its halves swapped. Returns that swapped value. */
static uint32_t rng_step(uint32_t *state)
{
	uint32_t a = *state, sw = (a << 16) | (a >> 16);
	*state = a + sw;
	return sw;
}

typedef struct
{
	float (*pts)[3];
	int n, cap;
	uint32_t rng;
	int detail;
	float scale; /* 1 / (radius word / $4000) */
} Splitter;

static void out_point(Splitter *sp, const float v[3])
{
	if (sp->n < sp->cap)
		memcpy(sp->pts[sp->n++], v, sizeof sp->pts[0]);
}

/* L3ddc4: an edge from prev to cur (unit vectors), cur emitted last */
static void split_edge(Splitter *sp, const float prev[3], const float cur[3], int level)
{
	rng_step(&sp->rng);
	if (level >= LEVELS)
	{
		out_point(sp, cur);
		return;
	}
	/* L3de2e: midpoint, pushed along the planet's axes */
	uint32_t saved = sp->rng;
	uint32_t sw = rng_step(&sp->rng);
	float mid[3] = {prev[0] + cur[0], prev[1] + cur[1], prev[2] + cur[2]};
	if (sp->detail != 0)
	{
		/* 2^(-8 - detail - level) of the radius (level from 1), half when
		 * the random sign bit is set; the game leaves it out from about a
		 * 4 millionth of the radius down, past any texel */
		int lv = level + 1;
		float amp = ldexpf(sp->scale, -8 - sp->detail - lv - ((sw & 0x80000000u) ? 1 : 0));
		unsigned bits = sw & 0xffff;
		for (int axis = 0; axis < 3; axis++)
		{
			bits = (bits << 1) & 0xffff;
			if (bits & 0x8000)
			{
				bits = (bits << 1) & 0xffff;
				mid[axis] += (bits & 0x8000) ? -amp : amp;
			}
		}
	}
	normalize3(mid);
	split_edge(sp, prev, mid, level + 1);
	split_edge(sp, mid, cur, level + 1);
	sp->rng = saved;
}

/* ---- filling ------------------------------------------------------------- */

typedef struct
{
	int row;
	float x;
} Crossing;

static int cmp_crossing(const void *a, const void *b)
{
	const Crossing *p = a, *q = b;
	if (p->row != q->row)
		return p->row - q->row;
	return (p->x > q->x) - (p->x < q->x);
}

static void to_tex(const float v[3], float *x, float *y)
{
	*x = (atan2f(v[0], v[2]) / 6.2831853f + 0.5f) * TEX_W;
	*y = acosf(fmaxf(-1.0f, fminf(1.0f, v[1]))) / 3.14159265f * TEX_H;
}

/* XORs `code` into the texels inside the spherical polygon pts[0..n) (even
 * odd), on the equirectangular map */
static void fill_polygon(uint8_t *codes, float (*pts)[3], int n, int code)
{
	if (n < 3)
		return;
	/* texture coordinates, longitudes unwrapped along the outline, the
	 * first point again at the end (unwrapped too) */
	float(*uv)[2] = malloc(sizeof *uv * (size_t)(n + 3));
	if (!uv)
		return;
	float sum_y = 0.0f;
	for (int i = 0; i <= n; i++)
	{
		to_tex(pts[i % n], &uv[i][0], &uv[i][1]);
		sum_y += pts[i % n][1];
		if (i > 0)
		{
			float dx = uv[i][0] - uv[i - 1][0];
			uv[i][0] -= TEX_W * roundf(dx / TEX_W);
		}
	}
	int m = n + 1;
	/* all the way round a pole: close through it (the one on the outline's
	 * side) */
	if (fabsf(uv[n][0] - uv[0][0]) > TEX_W * 0.5f)
	{
		float pole = sum_y > 0.0f ? 0.0f : (float)TEX_H;
		uv[m][0] = uv[n][0];
		uv[m++][1] = pole;
		uv[m][0] = uv[0][0];
		uv[m++][1] = pole;
	}

	/* crossings of each row's centre */
	size_t cap = 4096, nx = 0;
	Crossing *xs = malloc(cap * sizeof *xs);
	if (!xs)
	{
		free(uv);
		return;
	}
	for (int i = 0; i < m; i++)
	{
		const float *a = uv[i], *b = uv[(i + 1) % m];
		float y0 = fminf(a[1], b[1]), y1 = fmaxf(a[1], b[1]);
		int r0 = (int)ceilf(y0 - 0.5f), r1 = (int)ceilf(y1 - 0.5f);
		for (int r = r0 < 0 ? 0 : r0; r < r1 && r < TEX_H; r++)
		{
			float t = ((float)r + 0.5f - a[1]) / (b[1] - a[1]);
			if (nx == cap)
			{
				cap *= 2;
				Crossing *g = realloc(xs, cap * sizeof *xs);
				if (!g)
					goto out;
				xs = g;
			}
			xs[nx].row = r;
			xs[nx++].x = a[0] + (b[0] - a[0]) * t;
		}
	}
	qsort(xs, nx, sizeof *xs, cmp_crossing);
	for (size_t i = 0; i + 1 < nx; i += 2)
	{
		if (xs[i].row != xs[i + 1].row)
		{
			i--; /* an odd count on a row (rounding): resync */
			continue;
		}
		int x0 = (int)ceilf(xs[i].x - 0.5f), x1 = (int)ceilf(xs[i + 1].x - 0.5f);
		if (x1 - x0 >= TEX_W)
			x1 = x0 + TEX_W;
		uint8_t *row = codes + (size_t)xs[i].row * TEX_W;
		for (int x = x0; x < x1; x++)
			row[((x % TEX_W) + TEX_W) % TEX_W] ^= (uint8_t)code;
	}
out:
	free(xs);
	free(uv);
}

static void fill_circle(uint8_t *codes, const float c[3], float cos_r, int code)
{
	float r = acosf(cos_r), colat = acosf(fmaxf(-1.0f, fminf(1.0f, c[1])));
	int r0 = (int)floorf((colat - r) / 3.14159265f * TEX_H), r1 = (int)ceilf((colat + r) / 3.14159265f * TEX_H);
	for (int row = r0 < 0 ? 0 : r0; row < r1 && row < TEX_H; row++)
	{
		float th = ((float)row + 0.5f) / TEX_H * 3.14159265f, st = sinf(th), ct = cosf(th);
		for (int x = 0; x < TEX_W; x++)
		{
			float ph = (((float)x + 0.5f) / TEX_W - 0.5f) * 6.2831853f;
			float v[3] = {st * sinf(ph), ct, st * cosf(ph)};
			if (v[0] * c[0] + v[1] * c[1] + v[2] * c[2] > cos_r)
				codes[(size_t)row * TEX_W + x] ^= (uint8_t)code;
		}
	}
}

/* ---- the texture ---------------------------------------------------------- */

static GLuint make_texture(const PlanetSurface *s)
{
	static Parsed p;
	if (!parse(s, &p))
		return 0;
	Uint64 t0 = SDL_GetPerformanceCounter();
	uint8_t *codes = calloc((size_t)TEX_W * TEX_H, 1);
	Splitter sp = {0};
	sp.cap = MAX_OUTLINE * 64;
	sp.pts = malloc(sizeof *sp.pts * (size_t)sp.cap);
	if (!codes || !sp.pts)
	{
		free(codes);
		free(sp.pts);
		return 0;
	}
	/* the radius word as the game normalises it: top bit at 14 */
	unsigned rw = s->radius_word ? s->radius_word : 0x4000;
	while (rw < 0x4000)
		rw <<= 1;
	while (rw > 0x7fff)
		rw >>= 1;
	sp.scale = 16384.0f / (float)rw;
	sp.rng = s->seed;

	for (int f = 0; f < p.n_features; f++)
	{
		if (p.features[f].circle)
		{
			const Circle *c = &p.circles[p.features[f].index];
			/* the game draws each as four edges (start vertex, three
			 * arcs and the close) and moves its sequence once per edge */
			for (int k = 0; k < (c->both ? 2 : 1); k++)
			{
				float cen[3] = {c->centre[0], c->centre[1], c->centre[2]};
				if (k)
					for (int j = 0; j < 3; j++)
						cen[j] = -cen[j];
				fill_circle(codes, cen, c->cos_r, c->code);
				for (int e = 0; e < 4; e++)
					rng_step(&sp.rng);
			}
			continue;
		}
		const Outline *o = &p.outlines[p.features[f].index];
		sp.n = 0;
		sp.detail = o->detail;
		const float(*pts)[3] = (const float(*)[3])p.points[o->first];
		out_point(&sp, pts[0]);
		for (int i = 1; i <= o->count; i++)
			split_edge(&sp, pts[i - 1], pts[i % o->count], 0);
		if (sp.n > 1)
			sp.n--; /* the close ends on the first point again */
		fill_polygon(codes, sp.pts, sp.n, o->code);
	}
	free(sp.pts);

	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, TEX_W, TEX_H, 0, GL_RED, GL_UNSIGNED_BYTE, codes);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
	free(codes);
	log_printf("Planet surface: %d outlines, %d circles, made in %.0f ms\n", p.n_outlines, p.n_circles,
			   (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency());
	return tex;
}

/* ---- cache --------------------------------------------------------------- */

#define CACHE_SLOTS 16

static struct
{
	uint32_t key;
	GLuint tex;
	uint32_t used;
} cache[CACHE_SLOTS];
static uint32_t use_clock;

static uint32_t surface_key(const PlanetSurface *s)
{
	uint32_t h = 2166136261u;
	for (int i = 0; i < s->len; i++)
		h = (h ^ s->data[i]) * 16777619u;
	h = (h ^ s->seed) * 16777619u;
	h = (h ^ s->radius_word) * 16777619u;
	return h ? h : 1;
}

GLuint planet_surface_texture(const PlanetSurface *s)
{
	uint32_t key = surface_key(s);
	int oldest = 0;
	for (int i = 0; i < CACHE_SLOTS; i++)
	{
		if (cache[i].key == key)
		{
			cache[i].used = ++use_clock;
			return cache[i].tex;
		}
		if (cache[i].used < cache[oldest].used)
			oldest = i;
	}
	GLuint tex = make_texture(s);
	if (!tex)
		return 0;
	if (cache[oldest].tex)
		glDeleteTextures(1, &cache[oldest].tex);
	cache[oldest].key = key;
	cache[oldest].tex = tex;
	cache[oldest].used = ++use_clock;
	return tex;
}

void planet_tex_shutdown(void)
{
	for (int i = 0; i < CACHE_SLOTS; i++)
	{
		if (cache[i].tex)
			glDeleteTextures(1, &cache[i].tex);
		cache[i].key = 0;
		cache[i].tex = 0;
	}
}
