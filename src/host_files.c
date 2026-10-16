/*
 * host_files.c - file host calls for save games: read, write, delete and
 * listing a directory.
 *
 * Where saves live:
 *   desktop   the path the game asks for, relative to the working directory
 *   Android   the app's internal storage
 *   web       /saves, an IndexedDB backed folder synced after every change
 */
#include <SDL.h>

#include "host.h"
#include "hostcall.h"
#include "main.h"
#include "station_resume.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <physfs.h>
#endif

#define GAME_NAME_MAX 64
#define DIR_NAME_LEN  14 /* ST file names: 8.3 plus terminator, padded */

/* Copies a NUL terminated string out of emulated RAM */
static void read_game_string(u32 addr, char *buf, size_t size)
{
	size_t i = 0;
	for (; i + 1 < size; i++)
	{
		buf[i] = STMemory_ReadByte(addr + (u32)i);
		if (!buf[i])
			return;
	}
	buf[i] = 0;
}

#ifdef __EMSCRIPTEN__
static void sync_filesystem(void)
{
	/* clang-format off */
	EM_ASM(FS.syncfs(false, function(err) {
		if (err)
			console.log('FS sync error:', err);
	}););
	/* clang-format on */
}
#endif

/* Maps the game's file name to a host path */
static void host_path(const char *name, char *out, size_t size)
{
#if defined(__EMSCRIPTEN__)
	if (strncmp(name, "/saves/", 7) == 0)
		snprintf(out, size, "%s", name);
	else
		snprintf(out, size, "/saves/%s", name);
#elif defined(ANDROID)
	const char *rel = name;
	if (*rel)
		rel++; /* drive letter */
	if (*rel == '/' || *rel == '\\')
		rel++;
	snprintf(out, size, "%s/%s", SDL_AndroidGetInternalStoragePath(), rel);
#else
	snprintf(out, size, "%s", name);
#endif
}

/* D1 = file name; returns remove() result in D0 */
void Call_Fdelete(void)
{
	char name[GAME_NAME_MAX], path[512];
	read_game_string(GetReg(REG_D1), name, sizeof(name));
#if defined(__EMSCRIPTEN__)
	/* the web build deletes by the game's name, as before */
	snprintf(path, sizeof(path), "%s", name);
#else
	host_path(name, path, sizeof(path));
#endif
	SetReg(REG_D0, remove(path));
#ifdef __EMSCRIPTEN__
	sync_filesystem();
#endif
}

/* D1 = file name, A4 = buffer, D7 = length; returns bytes written in D0 */
void Call_Fwrite(void)
{
	char name[GAME_NAME_MAX], path[512];
	read_game_string(GetReg(REG_D1), name, sizeof(name));
	host_path(name, path, sizeof(path));

	FILE *f = fopen(path, "wb");
	if (!f)
	{
		log_printf("Could not write '%s'\n", path);
		SetReg(REG_D0, 0);
		return;
	}
	SetReg(REG_D0, (int)fwrite(STRam + GetReg(REG_A4), 1, GetReg(REG_D7), f));
	fclose(f);
#ifdef __EMSCRIPTEN__
	sync_filesystem();
#endif
}

/* D1 = file name, A4 = buffer, D7 = length; returns bytes read in D0 */
void Call_Fread(void)
{
	char name[GAME_NAME_MAX], path[512];
	read_game_string(GetReg(REG_D1), name, sizeof(name));
	host_path(name, path, sizeof(path));

	station_resume_forget(); /* a saved game, likely: not the station kept */
	FILE *f = fopen(path, "rb");
	if (!f)
	{
		SetReg(REG_D0, 0);
		return;
	}
	SetReg(REG_D0, (int)fread(STRam + GetReg(REG_A4), 1, GetReg(REG_D7), f));
	fclose(f);
}

/* =========================================================================
 * Directory listing (the load / save file selector)
 * ========================================================================= */
static char cur_dir[1024];
static int dir_index;

#ifdef __EMSCRIPTEN__

static int dir_count;

/* A2 = directory name (drive letter first) */
void Call_Fopendir(void)
{
	char name[GAME_NAME_MAX], path[128];
	read_game_string(GetReg(REG_A2), name, sizeof(name));
	const char *dir = name + 1; /* skip drive letter */
	if (strncmp(dir, "saves", 5) == 0)
		snprintf(path, sizeof(path), "/%s", dir);
	else
		snprintf(path, sizeof(path), "/saves/%s", dir);

	/* clang-format off */
	int result = EM_ASM_INT({
		try {
			var dir = UTF8ToString($0);
			try { FS.mkdir(dir); } catch (e) {}
			var files = FS.readdir(dir).filter(function(f) { return f !== '.' && f !== '..'; });
			Module._dir_files = files;
			return files.length;
		} catch (e) {
			return -1;
		}
	}, path);
/* clang-format on */

if (result < 0)
{
	SetReg(REG_D0, -1);
	return;
}
snprintf(cur_dir, sizeof(cur_dir), "%s", path);
dir_count = result;
dir_index = 0;
SetReg(REG_D0, 0);
}

void Call_Fclosedir(void)
{
	EM_ASM({ delete Module._dir_files; });
}

/* Next directory entry; false at the end */
static bool dir_next(char name[DIR_NAME_LEN], int *len, int *attribs)
{
	if (dir_index >= dir_count)
		return false;
	/* clang-format off */
	EM_ASM({
		var file = Module._dir_files[$0];
		stringToUTF8(file, $2, $1);
		try {
			var stat = FS.stat(UTF8ToString($3) + '/' + file);
			setValue($4, stat.size, 'i32');
			setValue($5, FS.isDir(stat.mode) ? 0x10 : 0, 'i32');
		} catch (e) {
			setValue($4, 0, 'i32');
			setValue($5, 0, 'i32');
		}
	}, dir_index, DIR_NAME_LEN, name, cur_dir, len, attribs);
	/* clang-format on */
	return true;
}

#else /* PhysFS */

static char **dir_files;
static bool physfs_ready;
#ifdef ANDROID
static char mount_path[512];
#define MOUNT_PATH mount_path
#else
#define MOUNT_PATH cur_dir
#endif

static bool init_physfs(void)
{
	if (physfs_ready)
		return true;
	if (!PHYSFS_init(NULL))
		return false;
#ifdef ANDROID
	const char *write_dir = SDL_AndroidGetInternalStoragePath();
#else
	const char *write_dir = ".";
#endif
	if (!write_dir || !PHYSFS_setWriteDir(write_dir))
	{
		log_printf("Could not use '%s' for saves\n", write_dir ? write_dir : "(null)");
		return false;
	}
	physfs_ready = true;
	return true;
}

/* A2 = directory name (drive letter first); D0 = 0 on success */
void Call_Fopendir(void)
{
	char name[GAME_NAME_MAX];
	read_game_string(GetReg(REG_A2), name, sizeof(name));
	if (!init_physfs())
	{
		SetReg(REG_D0, -1);
		return;
	}

	const char *dir = name + 1; /* skip drive letter */
	if (*dir == '/')
		dir++;
	PHYSFS_mkdir(dir); /* make sure the saves folder exists */

	snprintf(cur_dir, sizeof(cur_dir), "%s", name);
	if (dir_files)
	{
		PHYSFS_freeList(dir_files);
		dir_files = NULL;
	}
#ifdef ANDROID
	snprintf(mount_path, sizeof(mount_path), "%s/%s", SDL_AndroidGetInternalStoragePath(), dir);
#endif
	if (!PHYSFS_mount(MOUNT_PATH, "/", 1))
	{
		log_printf("PHYSFS_mount(%s) failed: %s\n", MOUNT_PATH,
				   PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
		SetReg(REG_D0, -1);
		return;
	}

	dir_files = PHYSFS_enumerateFiles("/");
	dir_index = 0;
	while (dir_files && dir_files[dir_index] &&
		   (!strcmp(dir_files[dir_index], ".") || !strcmp(dir_files[dir_index], "..")))
		dir_index++;
	SetReg(REG_D0, dir_files ? 0 : -1);
}

void Call_Fclosedir(void)
{
	if (dir_files)
	{
		PHYSFS_freeList(dir_files);
		dir_files = NULL;
	}
	PHYSFS_unmount(MOUNT_PATH);
}

/* Next directory entry; false at the end */
static bool dir_next(char name[DIR_NAME_LEN], int *len, int *attribs)
{
	if (!dir_files || !dir_files[dir_index])
		return false;
	const char *file = dir_files[dir_index];
	snprintf(name, DIR_NAME_LEN, "%s", file);

	char path[2048];
	PHYSFS_Stat st;
	snprintf(path, sizeof(path), "/%s", file);
	if (!PHYSFS_stat(path, &st))
	{
		snprintf(path, sizeof(path), "%s/%s", cur_dir, file);
		if (!PHYSFS_stat(path, &st))
			st.filesize = 0, st.filetype = PHYSFS_FILETYPE_REGULAR;
	}
	*len = (int)st.filesize;
	*attribs = (st.filetype == PHYSFS_FILETYPE_DIRECTORY) ? 0x10 : 0;
	return true;
}

#endif /* PhysFS */

/* Next entry: name at A0, length in D1, attributes in D2; D0 = -1 at end */
void Call_Freaddir(void)
{
	char name[DIR_NAME_LEN] = {0};
	int len = 0, attribs = 0;
	if (!dir_next(name, &len, &attribs))
	{
		SetReg(REG_D0, -1);
		return;
	}
	u32 out = GetReg(REG_A0);
	for (int i = 0; i < DIR_NAME_LEN; i++)
		STMemory_WriteByte(out + i, name[i]);
	SetReg(REG_D2, attribs);
	SetReg(REG_D1, len);
	SetReg(REG_D0, 0);
	dir_index++;
}
