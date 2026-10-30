/*
 * gl_scene.c - depth-sorted record/replay of emulator draw calls.
 * See gl_scene.h for the overview.
 */
#include <string.h>

#include "gl_cockpit.h"
#include "gl_native.h"
#include "gl_prims.h"
#include "gl_scene.h"
#include "main.h"
#include "m68000.h"
#include "renderer.h"

#define SCENE_DATA_SIZE (1 << 20) /* bytes of records per frame */
#define SCENE_MAX_NODES 4096
#define SCENE_MAX_PASSES (COCKPIT_MAX_PASSES + 1)

typedef struct
{
	uint16_t op;
	uint16_t size; /* payload bytes, rounded up to 8 */
} RecHeader;

typedef struct
{
	uint32_t z;
	int less, more; /* child node indices, -1 = none */
	uint32_t start, end;
} ZNode;

typedef struct
{
	int root;      /* -1 until the pass's first node */
	int id;        /* the cockpit's id for it */
	uint32_t zmin; /* its nearest node so far (root >= 0) */
} Pass;

/* A frame's records. Pass 0 is the frame's ordinary drawing. More passes
 * are the world drawn again with the camera turned (cockpit_pass_* in
 * gl_cockpit.c), each its own depth tree. The game records into `rec`;
 * scene_publish copies it to `pub` and scene_acquire from there to `show`,
 * which is what gets drawn (on the render thread, see gl_screen.c). */
typedef struct
{
	uint32_t data_pos;
	int n_nodes;
	int n_passes;
	Pass passes[SCENE_MAX_PASSES];
	ZNode nodes[SCENE_MAX_NODES];
	unsigned char data[SCENE_DATA_SIZE];
} SceneBuf;

static SceneBuf rec = {.n_passes = 1, .passes = {{-1, -1, 0}}};
static SceneBuf pub = {.n_passes = 1, .passes = {{-1, -1, 0}}};
static SceneBuf show = {.n_passes = 1, .passes = {{-1, -1, 0}}};
/* A second screen's frame beside the first (the MAP screen's page, while
 * the station's services are the game's screen): its own copy, drawn when
 * scene_use_page is on */
static SceneBuf pub_page = {.n_passes = 1, .passes = {{-1, -1, 0}}};
static SceneBuf show_page = {.n_passes = 1, .passes = {{-1, -1, 0}}};
static SceneBuf *drawn = &show;

static int cur_node = -1; /* -1: no node, records are dropped */
static bool node_locked;
static int cur_pass;

/* Soft node records not captured yet (offsets of their payloads) */
static uint32_t soft_pending[SCENE_MAX_NODES];
static int n_soft_pending;
static SoftNode *cur_soft; /* the current node's */

typedef void (*PrimDrawFn)(const void *);
static const PrimDrawFn draw_table[PRIM_COUNT] = {NULL,
#define X(name, fn) fn,
												  GL_PRIMITIVES(X)
#undef X
};

/* =========================================================================
 * Helpers shared by primitive files
 * ========================================================================= */
Vec3i m68k_vertex(uint32_t addr)
{
	Vec3i v;
	v.x = STMemory_ReadLong(addr);
	v.y = STMemory_ReadLong(addr + 4);
	v.z = -STMemory_ReadLong(addr + 8);
	return v;
}

Rgb8 rgb444_to_rgb8(int rgb)
{
	Rgb8 c;
	c.r = (uint8_t)((rgb & 0xf00) >> 4);
	c.g = (uint8_t)(rgb & 0xf0);
	c.b = (uint8_t)((rgb & 0xf) << 4);
	c.a = 255;
	return c;
}

bool scene_active(void)
{
	return use_renderer != R_OLD;
}

/* =========================================================================
 * Recording
 * ========================================================================= */
void scene_reset(void)
{
	native_reset();
	rec.data_pos = 0;
	rec.n_nodes = 0;
	cur_node = -1;
	node_locked = false;
	rec.passes[0].root = -1;
	rec.passes[0].id = -1;
	rec.n_passes = 1;
	cur_pass = 0;
	n_soft_pending = 0;
	cur_soft = NULL;
	scene_mute_object(0);
	soft_nodes_reset();
	planet_dists_reset();
}

/* The frame (A6) of the object whose primitives are dropped, 0 if none */
static uint32_t mute_a6;

int scene_cur_pass(void)
{
	return cur_pass;
}

static void close_current(void)
{
	if (cur_node >= 0)
		rec.nodes[cur_node].end = rec.data_pos;
	cur_soft = NULL;
}

static int new_node(uint32_t z)
{
	if (rec.n_nodes >= SCENE_MAX_NODES)
		return -1;
	int i = rec.n_nodes++;
	rec.nodes[i].z = z;
	rec.nodes[i].less = rec.nodes[i].more = -1;
	rec.nodes[i].start = rec.nodes[i].end = rec.data_pos;
	return i;
}

bool scene_begin_pass(int id)
{
	if (!scene_active() || rec.n_passes >= SCENE_MAX_PASSES)
		return false;
	close_current();
	cur_node = -1;
	cur_pass = rec.n_passes++;
	mute_a6 = 0;
	rec.passes[cur_pass].root = -1;
	rec.passes[cur_pass].id = id;
	return true;
}

void scene_end_pass(void)
{
	close_current();
	cur_node = -1;
	cur_pass = 0;
}

void scene_capture_soft_nodes(void)
{
	for (int i = 0; i < n_soft_pending; i++)
		soft_node_capture((SoftNode *)(rec.data + soft_pending[i]));
	n_soft_pending = 0;
}

bool scene_insert_node(uint32_t z)
{
	if (!scene_active() || node_locked)
		return false;

	close_current();
	int n = new_node(z);
	cur_node = n;
	if (n < 0)
		return false;
	Pass *p = &rec.passes[cur_pass];
	if (p->root < 0) /* the pass's first node is its root */
	{
		p->root = n;
		p->zmin = z;
		return true;
	}
	if (z < p->zmin)
		p->zmin = z;

	/* Iterative BST insert: larger z to `more`, ties to `less` */
	int i = p->root;
	for (;;)
	{
		int *next = (z > rec.nodes[i].z) ? &rec.nodes[i].more : &rec.nodes[i].less;
		if (*next < 0)
		{
			*next = n;
			return true;
		}
		i = *next;
	}
}

void scene_insert_after_background(enum PrimOp op)
{
	const Pass *p = &rec.passes[cur_pass];
	uint32_t z = p->root < 0 ? UINT32_MAX : p->zmin > 0 ? p->zmin - 1 : 0;
	if (!scene_insert_node(z))
		return;
	scene_record(op, 0);
	close_current();
	cur_node = -1;
}

void scene_lock_node(bool lock)
{
	node_locked = lock;
}

void scene_mute_object(uint32_t a6)
{
	mute_a6 = a6;
}

void *scene_record(enum PrimOp op, size_t size)
{
	if (!scene_active() || cur_node < 0)
		return NULL;
	if (mute_a6)
	{
		/* below the object's frame: still drawing it */
		if ((uint32_t)GetReg(REG_A7) < mute_a6)
		{
			if (op != PRIM_PLANET)
				return NULL;
		}
		else
			mute_a6 = 0;
	}

	size_t padded = (size + 7) & ~(size_t)7;
	if (rec.data_pos + sizeof(RecHeader) + padded > SCENE_DATA_SIZE || padded > 0xffff)
	{
		static bool warned;
		if (!warned)
			log_printf("gl_scene: frame record buffer full, dropping geometry\n");
		warned = true;
		return NULL;
	}

	RecHeader *h = (RecHeader *)(rec.data + rec.data_pos);
	h->op = (uint16_t)op;
	h->size = (uint16_t)padded;
	rec.data_pos += sizeof(RecHeader);
	void *payload = rec.data + rec.data_pos;
	memset(payload, 0, padded);
	rec.data_pos += (uint32_t)padded;
	rec.nodes[cur_node].end = rec.data_pos;
	if (op == PRIM_SOFT_NODE)
	{
		cur_soft = payload;
		if (n_soft_pending < SCENE_MAX_NODES)
			soft_pending[n_soft_pending++] = (uint32_t)((unsigned char *)payload - rec.data);
	}
	return payload;
}

SoftNode *scene_current_soft_node(void)
{
	return cur_soft;
}

/* =========================================================================
 * Replay
 * ========================================================================= */
static void draw_node(const ZNode *n)
{
	uint32_t pos = n->start;
	while (pos + sizeof(RecHeader) <= n->end)
	{
		const RecHeader *h = (const RecHeader *)(drawn->data + pos);
		pos += sizeof(RecHeader);
		if (h->op > PRIM_NONE && h->op < PRIM_COUNT)
			draw_table[h->op](drawn->data + pos);
		pos += h->size;
	}
}

static void draw_tree(int root)
{
	if (root < 0 || root >= drawn->n_nodes)
		return;

	/* In-order walk, `more` (farther) side first: painter's algorithm */
	static int stack[SCENE_MAX_NODES];
	int sp = 0;
	int i = root;
	while (i >= 0 || sp > 0)
	{
		while (i >= 0)
		{
			stack[sp++] = i;
			i = drawn->nodes[i].more;
		}
		i = stack[--sp];
		draw_node(&drawn->nodes[i]);
		i = drawn->nodes[i].less;
	}
}

/* =========================================================================
 * Hand over (gl_screen.c holds the lock around these)
 * ========================================================================= */
static void copy_scene(SceneBuf *dst, const SceneBuf *src)
{
	dst->data_pos = src->data_pos;
	dst->n_nodes = src->n_nodes;
	dst->n_passes = src->n_passes;
	memcpy(dst->passes, src->passes, sizeof(Pass) * (size_t)src->n_passes);
	memcpy(dst->nodes, src->nodes, sizeof(ZNode) * (size_t)src->n_nodes);
	memcpy(dst->data, src->data, src->data_pos);
}

/* A world captured under a screen (scene_publish_world) waits for the
 * drawing to take it */
static bool world_new;

void scene_publish(bool screen)
{
	close_current();
	scene_capture_soft_nodes();
	copy_scene(&pub, &rec);
	/* a screen's haze: none, the world's is kept; its planets are its own */
	if (!screen)
	{
		atmos_publish();
		planet_dists_publish();
	}
	else
		planet_screen_publish();
	native_publish(screen);
}

void scene_publish_page(void)
{
	close_current();
	scene_capture_soft_nodes();
	copy_scene(&pub_page, &rec);
	planet_page_publish();
	native_publish_page();
}

void scene_publish_world(void)
{
	close_current();
	scene_capture_soft_nodes();
	atmos_publish();
	planet_dists_publish();
	native_publish_world();
	world_new = true;
	/* the screen's next frame starts empty, as if the game had cleared it */
	scene_reset();
}

void scene_acquire(bool screen)
{
	copy_scene(&show, &pub);
	/* under a screen the world's planets and haze stay as they were, unless
	 * a new world was captured */
	if (!screen || world_new)
	{
		atmos_acquire();
		planet_dists_acquire();
	}
	if (screen)
		planet_screen_acquire();
	native_acquire(screen, world_new);
	world_new = false;
	gl_prims_new_scene();
}

/* The scene being drawn has nothing: the page's, drawn into its screen, is
 * not the world's */
void scene_drop_shown(void)
{
	show.n_nodes = 0;
	show.data_pos = 0;
	show.n_passes = 1;
	show.passes[0].root = -1;
}

void scene_acquire_page(void)
{
	copy_scene(&show_page, &pub_page);
	planet_page_acquire();
	native_acquire_page();
}

void scene_use_page(bool on)
{
	drawn = on ? &show_page : &show;
	native_use_page(on);
}

void scene_draw(void)
{
	draw_tree(drawn->passes[0].root);

	/* the turned passes, in the cockpit's order: each only where no
	 * earlier one has drawn */
	if (drawn->n_passes > 1)
	{
		for (int k = 0; k < COCKPIT_MAX_PASSES; k++)
			for (int p = 1; p < drawn->n_passes; p++)
				if (drawn->passes[p].id == k && drawn->passes[p].root >= 0)
				{
					cockpit_pass_draw_begin(k);
					draw_tree(drawn->passes[p].root);
				}
		cockpit_pass_draw_end();
	}
}
