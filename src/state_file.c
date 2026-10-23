/*
 * state_file.c - see state_file.h. The format is documented in
 * tools/savetool/savefile.py.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "state_file.h"
#include "game_state.h"
#include "host.h"
#include "main.h"

#define BIG_SIZE 0x80ee
#define A6_RAW   0x20c
#define A6_RLE   0x3662
#define KEY0     0x12350fd4u

/* Zero bytes are followed by a count of further zeros */
static size_t rle(u8 *out, const u8 *in, size_t n)
{
	size_t o = 0;
	for (size_t i = 0; i < n;)
	{
		if (in[i])
		{
			out[o++] = in[i++];
			continue;
		}
		size_t run = 1;
		while (i + run < n && !in[i + run] && run < 256)
			run++;
		out[o++] = 0;
		out[o++] = (u8)(run - 1);
		i += run;
	}
	return o;
}

bool state_file_write(const char *path)
{
	static u8 body[BIG_SIZE * 2 + (A6_RAW + A6_RLE) * 2 + 16];
	size_t n = 0;

	n += rle(body + n, (const u8 *)m68kram + FE2_big_space, BIG_SIZE);
	memcpy(body + n, (const u8 *)m68kram + GAME_A6, A6_RAW);
	n += A6_RAW;
	n += rle(body + n, (const u8 *)m68kram + GAME_A6 + A6_RAW, A6_RLE);
	if (n & 1)
		body[n++] = 0;

	u32 key = KEY0;
	for (size_t i = 0; i < n; i += 2)
	{
		u16 plain = (u16)(body[i] << 8 | body[i + 1]);
		u16 w = plain ^ (u16)key;
		body[i] = (u8)(w >> 8);
		body[i + 1] = (u8)w;
		key += (u32)(s32)(s16)plain;
		key = key << 1 | key >> 31;
	}

	FILE *f = fopen(path, "wb");
	if (!f)
		return false;
	u8 head[2] = {0, 0x11}, tail[4] = {(u8)(key >> 24), (u8)(key >> 16), (u8)(key >> 8), (u8)key};
	bool ok = fwrite(head, 1, 2, f) == 2 && fwrite(body, 1, n, f) == n && fwrite(tail, 1, 4, f) == 4;
	return fclose(f) == 0 && ok;
}

bool state_file_snapshot(char *name, int size)
{
	for (int i = 1; i < 100; i++)
	{
		snprintf(name, size, "savs/Snap%02d", i);
		FILE *f = fopen(name, "rb");
		if (f)
		{
			fclose(f);
			continue;
		}
		bool ok = state_file_write(name);
		log_printf("state snapshot %s: %s\n", name, ok ? "written" : "FAILED");
		return ok;
	}
	return false;
}
