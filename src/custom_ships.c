/*
 * custom_ships.c - see custom_ships.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h> /* readlink */
#endif

#include "custom_ships.h"
#include "game_state.h" /* FE2_USE_MODDED, labels */
#include "host.h"       /* rdword, wrlong, m68kram */
#include "main.h"       /* log_printf */
#include "mods.h"       /* mods_strip_word */

#define SHIPS_DIR  "custom_ships"
#define MODEL_MAX  512 /* MDLMOD_MAX in fe2_modded.s */
#define SHIP_FIRST 240 /* new ship types from here (Lmdlmod_like) */
#define FILE_MAX   (256 * 1024)

/* .fe2m v2 records, after the models; they describe the first model */
enum
{
	REC_NAME = 1,  /* ASCII ship name */
	REC_LIKE = 2,  /* u16: original model number it behaves like */
	REC_PICKS = 3, /* (u8 category, u8 weight) pairs: where it appears */
};

#define PICK_CATS     32
#define PICK_EXTRA    4096 /* (model, category, weight) records across all files */
#define PICKS_BYTES   8192 /* Lmdlmod_picks in fe2_modded.s */
#define NAMES_BYTES   12288 /* Lmdlmod_strings in fe2_modded.s */
#define CORE_STRINGS  112 /* L3e894 */
#define NAME_MAX      40
#define NAMES_MAX     (MODEL_MAX - SHIP_FIRST) /* one per new ship type */

static int installed;
static int new_ships;

#define INFO_MAX 512
static CustomShipInfo infos[INFO_MAX];
static int info_count;

int custom_ships_info_count(void)
{
	return info_count;
}

const CustomShipInfo *custom_ships_info(int i)
{
	return (i >= 0 && i < info_count) ? &infos[i] : NULL;
}

int custom_ships_count(void)
{
	return installed;
}

int custom_ships_new_ships(void)
{
	return new_ships;
}

#if FE2_USE_MODDED

static struct
{
	int model, cat, weight;
} picks[PICK_EXTRA];
static int pick_count;

static char names[NAMES_MAX][NAME_MAX + 1];
static int name_count;

/* rdword is signed; offsets and ids here are not */
static u32 rdu16(u32 pos)
{
	return (u16)rdword(pos);
}

static u32 be16(const unsigned char *p)
{
	return ((u32)p[0] << 8) | p[1];
}

static u32 be32(const unsigned char *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static unsigned char *read_file(const char *path, long *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *buf = NULL;
	if (*len > 0 && *len <= FILE_MAX)
	{
		buf = malloc((size_t)*len);
		if (buf && fread(buf, 1, (size_t)*len, f) != (size_t)*len)
		{
			free(buf);
			buf = NULL;
		}
	}
	fclose(f);
	return buf;
}

/* A model's header offsets must stay inside the data we were given, or
 * the game would read (and draw) whatever follows it in the pool. */
static bool model_sane(const unsigned char *m, u32 len)
{
	if (len < 0x1e)
		return false;
	u32 code = be16(m + 0), verts = be16(m + 2), norms = be16(m + 6);
	u32 coll = be16(m + 0x1a), ship = be16(m + 0x1c);
	return code < len && verts <= len && norms <= len && coll < len && ship < len;
}

/* New ship name -> its string id ($4070 on), NULL-safe */
static int add_name(const unsigned char *s, u32 len)
{
	if (name_count == NAMES_MAX)
		return -1;
	if (len > NAME_MAX)
		len = NAME_MAX;
	char *d = names[name_count];
	for (u32 i = 0; i < len; i++)
		d[i] = (s[i] >= 32 && s[i] < 127) ? (char)s[i] : ' ';
	d[len] = 0;
	return 0x4000 + CORE_STRINGS + name_count++;
}

/* Records for the file's first model (at RAM address root, number index) */
static void apply_records(const char *path, const unsigned char *p, long len, u32 root, u32 index,
						  CustomShipInfo *info)
{
	if (len < 2)
		return;
	int count = (int)be16(p);
	long pos = 2;
	for (int i = 0; i < count && pos + 4 <= len; i++)
	{
		u32 type = be16(p + pos), size = be16(p + pos + 2);
		const unsigned char *d = p + pos + 4;
		pos += 4 + ((size + 1) & ~1u);
		if (pos > len)
			break;
		u32 ship = root + rdu16(root + 0x1c);
		switch (type)
		{
		case REC_NAME:
		{
			int id = add_name(d, size);
			if (id >= 0 && info)
				snprintf(info->name, sizeof(info->name), "%s", names[name_count - 1]);
			if (id >= 0 && rdu16(root + 0x1c))
				wrword(ship + 14, id);
			break;
		}
		case REC_LIKE:
			if (index >= SHIP_FIRST && size >= 2 && info)
				info->like = (int)be16(d);
			if (index >= SHIP_FIRST && size >= 2)
				wrword(FE2_Lmdlmod_like + 2 * (index - SHIP_FIRST), (int)(be16(d) * 2));
			break;
		case REC_PICKS:
			if (info)
				for (u32 k = 0; k + 1 < size; k += 2)
					info->lists += d[k + 1] > 0;
			for (u32 k = 0; k + 1 < size && pick_count < PICK_EXTRA; k += 2)
			{
				picks[pick_count].model = (int)index;
				picks[pick_count].cat = d[k];
				picks[pick_count].weight = d[k + 1];
				pick_count++;
			}
			break;
		default:
			log_printf("Models: %s: unknown record %u ignored\n", path, type);
		}
	}
	if (index >= SHIP_FIRST && rdu16(root + 0x1c))
	{
		new_ships++;
		if (info)
			info->new_ship = 1;
	}
}

/* Which file each model number came from, to catch two files using the
 * same number (FE2ShipBuilder hands numbers out so this doesn't happen, but
 * files can be copied in by hand) */
static const char *model_owner[MODEL_MAX];
static u32 pool_next; /* first free pool byte after custom_ships_install (0: not run) */
static char owner_names[MODEL_MAX][40];

/* Returns the number of models installed from this file, -1 on error */
static int install_file(const char *path, const char *source, const unsigned char *buf, long len, u32 *pool)
{
	u32 version = len >= 8 ? be16(buf + 4) : 0;
	if (len < 8 || memcmp(buf, "FE2M", 4) != 0 || version < 1 || version > 2)
	{
		log_printf("Models: %s is not a .fe2m file this build understands\n", path);
		return -1;
	}
	int count = (int)be16(buf + 6);
	long pos = 8;
	u32 first = 0, first_index = 0;
	for (int i = 0; i < count; i++)
	{
		if (pos + 8 > len)
			return -1;
		u32 index = be16(buf + pos);
		u32 size = be32(buf + pos + 4);
		pos += 8;
		if (pos + (long)size > len || index >= MODEL_MAX || !model_sane(buf + pos, size))
		{
			log_printf("Models: %s: model %u is broken, skipped\n", path, index);
			return -1;
		}
		u32 end = *pool + ((size + 1) & ~1u);
		if (end > FE2_Lmdlmod_pool_end)
		{
			log_printf("Models: out of pool space for %s (Lmdlmod_pool in fe2_modded.s)\n", path);
			return -1;
		}
		if (model_owner[index])
			log_printf("Models: model %u is in both %s and %s; %s wins. Load one into FE2ShipBuilder and save it "
					   "to give it a free number.\n",
					   index, model_owner[index], path, path);
		snprintf(owner_names[index], sizeof(owner_names[index]), "%s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
		model_owner[index] = owner_names[index];
		memcpy(m68kram + *pool, buf + pos, size);
		wrlong(FE2_Lmdlmod_ptrs + 4 * index, *pool);
		if (i == 0)
		{
			first = *pool;
			first_index = index;
		}
		*pool = end;
		pos += (long)((size + 1) & ~1u);
		installed++;
	}
	CustomShipInfo *info = (count > 0 && info_count < INFO_MAX) ? &infos[info_count++] : NULL;
	if (info)
	{
		const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : strrchr(path, ' ') ? strrchr(path, ' ') + 1 : path;
		memset(info, 0, sizeof(*info));
		snprintf(info->file, sizeof(info->file), "%s", base);
		snprintf(info->source, sizeof(info->source), "%s", source);
		info->model = (int)first_index;
		info->models = count;
		info->like = -1;
	}
	if (version >= 2 && count > 0)
		apply_records(path, buf + pos, len - pos, first, first_index, info);
	log_printf("Models: %s: %d model(s), model %u%s\n", path, count, first_index,
			   first_index >= SHIP_FIRST ? " (new ship type)" : "");
	return count;
}

/* Lmdlmod_picks = L5fac2_ship_picks plus the new ships. Layout: a (count,
 * offset) word pair per category, then the lists of model byte indices;
 * offsets are from the table start. */
static void build_picks(void)
{
	if (!pick_count)
		return;
	u32 src = FE2_ship_picks;
	int cats = 0;
	u32 first_list = 0xffff;
	while (cats < PICK_CATS && (u32)cats * 4 < first_list)
	{
		u32 off = rdu16(src + cats * 4 + 2);
		if (off < first_list)
			first_list = off;
		cats++;
	}
	u32 dst = FE2_Lmdlmod_picks, end = dst + PICKS_BYTES;
	bool full = false;
	u32 out = dst + cats * 4;
	for (int c = 0; c < cats; c++)
	{
		int n = (int)rdu16(src + c * 4);
		u32 list = src + rdu16(src + c * 4 + 2);
		u32 start = out;
		for (int k = 0; k < n && out + 2 <= end; k++, out += 2)
			wrword(out, (int)rdu16(list + 2 * k));
		for (int p = 0; p < pick_count; p++)
			if (picks[p].cat == c)
				for (int w = 0; w < picks[p].weight; w++, out += 2)
				{
					/* the game's own ships are always all there; ours stop when it's full */
					if (out + 2 > end)
					{
						full = true;
						break;
					}
					wrword(out, picks[p].model * 2);
				}
		wrword(dst + c * 4, (int)((out - start) / 2));
		wrword(dst + c * 4 + 2, (int)(start - dst));
	}
	for (int p = 0; p < pick_count; p++)
		if (picks[p].cat >= cats)
			log_printf("Models: model %d: no ship category %d (0-%d)\n", picks[p].model, picks[p].cat,
					   cats - 1);
	if (full)
		log_printf("Models: the ship lists are full (Lmdlmod_picks), some weights were cut; use lower weights\n");
	wrword(FE2_Lmdlmod_picks_on, 1);
}

/* Lmdlmod_strings, laid out like L3e894: offsets from the table start */
static void build_names(void)
{
	u32 base = FE2_Lmdlmod_strings, end = base + NAMES_BYTES;
	u32 at = base + 2 * name_count;
	for (int i = 0; i < name_count; i++)
	{
		size_t len = strlen(names[i]);
		if (at + len + 1 > end)
		{
			log_printf("Models: no room for more ship names (Lmdlmod_strings)\n");
			break;
		}
		wrword(base + 2 * i, (int)(at - base));
		memcpy(m68kram + at, names[i], len + 1);
		at += (u32)len + 1;
	}
}

/* Folder the game's executable is in, with a trailing separator (malloc'd).
 * SDL_GetBasePath first; this SDL build can be compiled without its
 * filesystem module, where that returns NULL. */
static char *exe_dir(void)
{
	char *base = SDL_GetBasePath();
	if (base)
	{
		char *copy = strdup(base);
		SDL_free(base);
		return copy;
	}
#ifdef _WIN32
	char path[MAX_PATH];
	DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
	if (n > 0 && n < MAX_PATH)
	{
		char *slash = strrchr(path, '\\');
		if (slash)
		{
			slash[1] = 0;
			return strdup(path);
		}
	}
#else
	char path[512];
	ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
	if (n > 0)
	{
		path[n] = 0;
		char *slash = strrchr(path, '/');
		if (slash)
		{
			slash[1] = 0;
			return strdup(path);
		}
	}
#endif
	return NULL;
}

/* custom_ships in the working directory, else next to the executable, else
 * the repo's tools/fe2ShipBuilder/custom_ships (when running build/GLFrontier).
 * The first of those holding any model file wins. */
static bool find_dir(char *dir, size_t size)
{
	char probe[600];
	char *base = exe_dir();
	const char *prefix[3] = {"", base, base};
	const char *up[3] = {"", "", "../tools/fe2ShipBuilder/"};
	bool found = false;
	for (int i = 0; i < 3 && !found; i++)
	{
		if (!prefix[i])
			continue;
		snprintf(dir, size, "%s%s" SHIPS_DIR, prefix[i], up[i]);
		for (int n = 0; n < MODEL_MAX && !found; n++)
		{
			snprintf(probe, sizeof(probe), "%s/%03d.fe2m", dir, n);
			FILE *f = fopen(probe, "rb");
			if (f)
			{
				fclose(f);
				found = true;
			}
		}
	}
	free(base);
	return found;
}

/* Ship pack that FE2 Ship Builder appends to GLFrontier.exe (tools/fe2ShipBuilder
 * util.h): "FE2P" u32 count, per file u16 name length, name, u32 length,
 * data; then u32 size of all that and "FE2MPACK". Big endian. */
#define PACK_MAX 64
static CustomShipFile pack_files[PACK_MAX + 1];
static unsigned char *pack_data;

static u32 get32(const unsigned char *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static bool exe_path(char *out, size_t size)
{
#ifdef _WIN32
	DWORD n = GetModuleFileNameA(NULL, out, (DWORD)size);
	return n > 0 && n < size;
#else
	ssize_t n = readlink("/proc/self/exe", out, size - 1);
	if (n <= 0)
		return false;
	out[n] = 0;
	return true;
#endif
}

/* Returns the number of files in the pack at the end of our own exe */
static int read_pack(void)
{
	char path[1024];
	if (!exe_path(path, sizeof(path)))
		return 0;
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	unsigned char foot[12];
	int count = 0;
	if (fseek(f, -12, SEEK_END) == 0 && fread(foot, 1, 12, f) == 12 && memcmp(foot + 4, "FE2MPACK", 8) == 0)
	{
		u32 size = get32(foot);
		pack_data = malloc(size);
		if (pack_data && size >= 8 && fseek(f, -12 - (long)size, SEEK_END) == 0 &&
			fread(pack_data, 1, size, f) == size && memcmp(pack_data, "FE2P", 4) == 0)
		{
			u32 n = get32(pack_data + 4), pos = 8;
			for (u32 i = 0; i < n && count < PACK_MAX && pos + 2 <= size; i++)
			{
				u32 nl = ((u32)pack_data[pos] << 8) | pack_data[pos + 1];
				pos += 2;
				if (pos + nl + 4 > size)
					break;
				char *name = malloc(nl + 1);
				memcpy(name, pack_data + pos, nl);
				name[nl] = 0;
				pos += nl;
				u32 len = get32(pack_data + pos);
				pos += 4;
				if (pos + len > size)
				{
					free(name);
					break;
				}
				pack_files[count].name = name;
				pack_files[count].data = pack_data + pos;
				pack_files[count].len = len;
				count++;
				pos += len;
			}
		}
	}
	fclose(f);
	return count;
}

/* Ships put into the exe by the studio replace the list compiled in by CMake,
 * so what the studio saved is exactly what the game has */
static const CustomShipFile *built_in = custom_ships_embedded;

static const CustomShipFile *embedded_file(const char *name)
{
	for (const CustomShipFile *m = built_in; m->name; m++)
		if (strcmp(m->name, name) == 0)
			return m;
	return NULL;
}

void custom_ships_install(void)
{
	u32 pool = FE2_Lmdlmod_pool;
	char dir[512], path[600], name[16];
	memset(model_owner, 0, sizeof(model_owner));
	int packed = read_pack();
	if (packed)
		built_in = pack_files;
	bool have_dir = find_dir(dir, sizeof(dir));
	int embedded = 0, from_dir = 0;
	/* One file per replaced or new model, named by its number; a file may
	 * also carry parts of a big ship, at numbers from 511 down. Compiled in
	 * first; a custom_ships folder beside the game (for trying things out
	 * without a rebuild) takes over from a compiled in file of the same name. */
	for (int n = 0; n < MODEL_MAX; n++)
	{
		snprintf(name, sizeof(name), "%03d.fe2m", n);
		long len = 0;
		unsigned char *buf = NULL;
		if (have_dir)
		{
			snprintf(path, sizeof(path), "%s/%s", dir, name);
			buf = read_file(path, &len);
		}
		const CustomShipFile *m = embedded_file(name);
		if (buf)
		{
			/* the same as the compiled in file: count it as built in */
			if (!m || m->len != (u32)len || memcmp(m->data, buf, (size_t)len) != 0)
				from_dir++;
			else
				embedded++;
			install_file(path, "folder", buf, len, &pool);
			free(buf);
		}
		else if (m)
		{
			snprintf(path, sizeof(path), "(%s) %s", packed ? "saved into the exe" : "built in", name);
			install_file(path, packed ? "saved into the exe" : "built in", m->data, (long)m->len, &pool);
			embedded++;
		}
	}
	build_names();
	build_picks();
	pool_next = pool;
	if (installed)
		log_printf("Models: %d custom model(s) (%d file(s) built in, %d from %s), %d new ship type(s), "
				   "%u of %u pool bytes used\n",
				   installed, embedded, from_dir, have_dir ? dir : "no folder", new_ships,
				   pool - FE2_Lmdlmod_pool, FE2_Lmdlmod_pool_end - FE2_Lmdlmod_pool);
}

/* The system's starports (see L5f546, which fills them on arrival): A6+1248
 * word count, then 474 bytes each from A6+1254; the ships for sale are 14
 * (model * 2, random word) pairs at +416, and how many at +472. */
#define A6_STARPORTS     1248
#define A6_STARPORT_LIST 1254
#define STARPORT_SIZE    474
#define STARPORT_MAX     18
#define STOCK_OFF        416
#define STOCK_COUNT_OFF  472
#define STOCK_MAX        14

/* Saves loadable by the unmodded game: every object of a new ship type
 * becomes the ship it's like (see L44758 for the object list), and so does
 * every one for sale in the system's shipyards. */
void custom_ships_strip(void)
{
	u32 space = (u32)rdlong(GAME_A6 + 14466); /* A6_big_space */
	if (!space || !new_ships)
		return;
	for (int slot = 1; slot <= 0x72; slot++)
	{
		int flags = rdbyte(space + slot);
		if (!flags || (flags & 0x20))
			continue;
		u32 obj = space + rdu16(FE2_obj_offsets + 2 * slot);
		int model = (int)rdu16(obj + 90) / 2;
		if (model < SHIP_FIRST || model >= MODEL_MAX)
			continue;
		int like = (int)rdu16(FE2_Lmdlmod_like + 2 * (model - SHIP_FIRST));
		if (like)
			mods_strip_word(obj + 90, like);
	}
	/* ships for sale in this system's shipyards (saved with the rest of A6) */
	int ports = (int)rdu16(GAME_A6 + A6_STARPORTS);
	if (ports > STARPORT_MAX)
		ports = STARPORT_MAX;
	for (int p = 0; p < ports; p++)
	{
		u32 port = GAME_A6 + A6_STARPORT_LIST + (u32)p * STARPORT_SIZE;
		int count = (int)rdu16(port + STOCK_COUNT_OFF);
		for (int k = 0; k < count && k < STOCK_MAX; k++)
		{
			u32 entry = port + STOCK_OFF + 4 * (u32)k;
			int model = (int)rdu16(entry) / 2;
			if (model < SHIP_FIRST || model >= MODEL_MAX)
				continue;
			int like = (int)rdu16(FE2_Lmdlmod_like + 2 * (model - SHIP_FIRST));
			mods_strip_word(entry, like ? like : 26 * 2); /* Cobra Mk III if it has none */
		}
	}
}

int custom_ships_stock_shipyards(void)
{
	/* a shipyard holds 14: with more new ship types, the first 14 */
	int ships[STOCK_MAX], n = 0;
	for (int m = SHIP_FIRST; m < MODEL_MAX && n < STOCK_MAX; m++)
	{
		u32 root = (u32)rdlong(FE2_Lmdlmod_ptrs + 4 * m);
		if (root && rdu16(root + 0x1c)) /* has ship data */
			ships[n++] = m;
	}
	int ports = (int)rdu16(GAME_A6 + A6_STARPORTS);
	if (!n || ports <= 0)
		return 0;
	if (ports > STARPORT_MAX)
		ports = STARPORT_MAX;
	for (int p = 0; p < ports; p++)
	{
		u32 port = GAME_A6 + A6_STARPORT_LIST + (u32)p * STARPORT_SIZE;
		u32 list = port + STOCK_OFF;
		int count = (int)rdu16(port + STOCK_COUNT_OFF);
		if (count > STOCK_MAX)
			count = STOCK_MAX;
		for (int i = 0; i < n; i++)
		{
			bool there = false;
			for (int k = 0; k < count; k++)
				there |= rdu16(list + 4 * k) == (u32)ships[i] * 2;
			if (there)
				continue;
			/* full: the new ship takes the last place */
			int at = count < STOCK_MAX ? count++ : STOCK_MAX - 1 - i % STOCK_MAX;
			wrword(list + 4 * at, ships[i] * 2);
			wrword(list + 4 * at + 2, rand() & 0xffff);
		}
		wrword(port + STOCK_COUNT_OFF, count);
	}
	log_printf("Models: %d custom ship type(s) put up for sale at %d starport(s)\n", n, ports);
	return ports;
}

/* -- models made at run time (planet models from a system file) ------------ */

#define PLANET_MODEL_TOP 479 /* free numbers are searched downwards from here: ships use 240 up, big-ship parts 511 down */
#define PLANET_MODEL_LOW 300

u32 custom_ships_pool_alloc(u32 size)
{
	u32 end = pool_next + ((size + 1) & ~1u);
	if (!pool_next || end > FE2_Lmdlmod_pool_end)
		return 0;
	u32 at = pool_next;
	pool_next = end;
	return at;
}

int custom_ships_free_index(void)
{
	for (int i = PLANET_MODEL_TOP; i >= PLANET_MODEL_LOW; i--)
		if (!model_owner[i] && !(u32)rdlong(FE2_Lmdlmod_ptrs + 4 * i))
			return i;
	return -1;
}

void custom_ships_set_model(int index, u32 addr)
{
	if (index >= 0 && index < MODEL_MAX)
		wrlong(FE2_Lmdlmod_ptrs + 4 * index, (int)addr);
}

u32 custom_ships_model_addr(int index)
{
	return (index >= 0 && index < MODEL_MAX) ? (u32)rdlong(FE2_Lmdlmod_ptrs + 4 * index) : 0;
}

#else /* original game: no hook to install into */

u32 custom_ships_pool_alloc(u32 size)
{
	(void)size;
	return 0;
}

int custom_ships_free_index(void)
{
	return -1;
}

void custom_ships_set_model(int index, u32 addr)
{
	(void)index;
	(void)addr;
}

u32 custom_ships_model_addr(int index)
{
	(void)index;
	return 0;
}

int custom_ships_stock_shipyards(void)
{
	return 0;
}

void custom_ships_install(void)
{
}

void custom_ships_strip(void)
{
}

#endif
