/*
 * gl_scene.c - depth-sorted record/replay of emulator draw calls.
 * See gl_scene.h for the overview.
 */
#include <string.h>

#include "gl_cockpit.h"
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

static unsigned char data[SCENE_DATA_SIZE];
static uint32_t data_pos;

static ZNode nodes[SCENE_MAX_NODES];
static int n_nodes;
static int cur_node = -1; /* -1: no node, records are dropped */
static bool node_locked;

/* Pass 0 is the frame's ordinary drawing. More passes are the world drawn
 * again with the camera turned (cockpit_pass_* in gl_cockpit.c), each its
 * own depth tree. */
typedef struct
{
	int root;      /* -1 until the pass's first node */
	int id;        /* the cockpit's id for it */
	uint32_t zmin; /* its nearest node so far (root >= 0) */
} Pass;
static Pass passes[SCENE_MAX_PASSES] = {{-1, -1, 0}};
static int n_passes = 1, cur_pass;

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
	data_pos = 0;
	n_nodes = 0;
	cur_node = -1;
	node_locked = false;
	passes[0].root = -1;
	passes[0].id = -1;
	n_passes = 1;
	cur_pass = 0;
	n_soft_pending = 0;
	cur_soft = NULL;
	soft_nodes_reset();
	gl_prims_new_scene();
}

static void close_current(void)
{
	if (cur_node >= 0)
		nodes[cur_node].end = data_pos;
	cur_soft = NULL;
}

static int new_node(uint32_t z)
{
	if (n_nodes >= SCENE_MAX_NODES)
		return -1;
	int i = n_nodes++;
	nodes[i].z = z;
	nodes[i].less = nodes[i].more = -1;
	nodes[i].start = nodes[i].end = data_pos;
	return i;
}

bool scene_begin_pass(int id)
{
	if (!scene_active() || n_passes >= SCENE_MAX_PASSES)
		return false;
	close_current();
	cur_node = -1;
	cur_pass = n_passes++;
	passes[cur_pass].root = -1;
	passes[cur_pass].id = id;
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
		soft_node_capture((SoftNode *)(data + soft_pending[i]));
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
	if (passes[cur_pass].root < 0) /* the pass's first node is its root */
	{
		passes[cur_pass].root = n;
		passes[cur_pass].zmin = z;
		return true;
	}
	if (z < passes[cur_pass].zmin)
		passes[cur_pass].zmin = z;

	/* Iterative BST insert: larger z to `more`, ties to `less` */
	int i = passes[cur_pass].root;
	for (;;)
	{
		int *next = (z > nodes[i].z) ? &nodes[i].more : &nodes[i].less;
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
	const Pass *p = &passes[cur_pass];
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

void *scene_record(enum PrimOp op, size_t size)
{
	if (!scene_active() || cur_node < 0)
		return NULL;

	size_t padded = (size + 7) & ~(size_t)7;
	if (data_pos + sizeof(RecHeader) + padded > SCENE_DATA_SIZE || padded > 0xffff)
	{
		static bool warned;
		if (!warned)
			log_printf("gl_scene: frame record buffer full, dropping geometry\n");
		warned = true;
		return NULL;
	}

	RecHeader *h = (RecHeader *)(data + data_pos);
	h->op = (uint16_t)op;
	h->size = (uint16_t)padded;
	data_pos += sizeof(RecHeader);
	void *payload = data + data_pos;
	memset(payload, 0, padded);
	data_pos += (uint32_t)padded;
	nodes[cur_node].end = data_pos;
	if (op == PRIM_SOFT_NODE)
	{
		cur_soft = payload;
		if (n_soft_pending < SCENE_MAX_NODES)
			soft_pending[n_soft_pending++] = (uint32_t)((unsigned char *)payload - data);
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
		const RecHeader *h = (const RecHeader *)(data + pos);
		pos += sizeof(RecHeader);
		if (h->op > PRIM_NONE && h->op < PRIM_COUNT)
			draw_table[h->op](data + pos);
		pos += h->size;
	}
}

static void draw_tree(int root)
{
	if (root < 0 || root >= n_nodes)
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
			i = nodes[i].more;
		}
		i = stack[--sp];
		draw_node(&nodes[i]);
		i = nodes[i].less;
	}
}

void scene_draw(void)
{
	close_current();
	scene_capture_soft_nodes();

	draw_tree(passes[0].root);

	/* the turned passes, in the cockpit's order: each only where no
	 * earlier one has drawn */
	if (n_passes > 1)
	{
		for (int k = 0; k < COCKPIT_MAX_PASSES; k++)
			for (int p = 1; p < n_passes; p++)
				if (passes[p].id == k && passes[p].root >= 0)
				{
					cockpit_pass_draw_begin(k);
					draw_tree(passes[p].root);
				}
		cockpit_pass_draw_end();
	}
}
