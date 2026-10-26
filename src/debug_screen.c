/*
 * debug_screen.c - see debug_screen.h
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <SDL.h>
#include "debug_screen.h"

#define MAX_KEYS 12
#define KEY_LEN 16
#define LOG_LINES 64

static SDL_mutex *lock;
static struct
{
	char key[KEY_LEN];
	char text[DEBUG_SCREEN_COLS + 1];
} keys[MAX_KEYS];
static int nkeys;
static char log_ring[LOG_LINES][DEBUG_SCREEN_COLS + 1];
static unsigned log_count;

static void lock_it(void)
{
	if (!lock)
		lock = SDL_CreateMutex();
	SDL_LockMutex(lock);
}

void debug_screen_set(const char *key, const char *fmt, ...)
{
	char buf[DEBUG_SCREEN_COLS + 1];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	lock_it();
	int i = 0;
	while (i < nkeys && strncmp(keys[i].key, key, KEY_LEN - 1))
		i++;
	if (i == nkeys && nkeys < MAX_KEYS)
	{
		nkeys++;
		snprintf(keys[i].key, KEY_LEN, "%s", key);
	}
	if (i < nkeys)
		memcpy(keys[i].text, buf, sizeof buf);
	SDL_UnlockMutex(lock);
}

void debug_screen_clear(const char *key)
{
	lock_it();
	for (int i = 0; i < nkeys; i++)
		if (!strncmp(keys[i].key, key, KEY_LEN - 1))
		{
			keys[i] = keys[--nkeys];
			break;
		}
	SDL_UnlockMutex(lock);
}

void debug_screen_log(const char *fmt, ...)
{
	char buf[DEBUG_SCREEN_COLS + 1];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	lock_it();
	memcpy(log_ring[log_count++ % LOG_LINES], buf, sizeof buf);
	SDL_UnlockMutex(lock);
}

int debug_screen_lines(char out[DEBUG_SCREEN_ROWS][DEBUG_SCREEN_COLS + 1])
{
	int n = 0;
	lock_it();
	for (int i = 0; i < nkeys && n < DEBUG_SCREEN_ROWS; i++)
		memcpy(out[n++], keys[i].text, DEBUG_SCREEN_COLS + 1);
	/* the newest log lines fill the rest, oldest of them first */
	int room = DEBUG_SCREEN_ROWS - n;
	unsigned have = log_count < LOG_LINES ? log_count : LOG_LINES;
	unsigned show = have < (unsigned)room ? have : (unsigned)room;
	for (unsigned i = log_count - show; i < log_count; i++)
		memcpy(out[n++], log_ring[i % LOG_LINES], DEBUG_SCREEN_COLS + 1);
	SDL_UnlockMutex(lock);
	return n;
}
