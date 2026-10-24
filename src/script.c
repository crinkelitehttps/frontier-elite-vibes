/*
 * script.c - see script.h.
 *
 * One command per line (or separated by ';'), '#' starts a comment. Commands run on the game thread
 * (script_poll, from the host update), one after another; a command that has to wait for the game
 * (a load, a pause) holds the rest. Output goes to stdout.
 *
 *   starting and loading
 *     new N              title menu entry N (1-5 as the keys)
 *     load NAME          load the commander file savs/NAME through the title menu's list
 *     waitgame           wait until the player has a ship
 *     wait SECONDS
 *   moving (the cheat menu's Location section, cheats.h)
 *     enter ID           make the system with this id (hex, as in tools/galaxy_catalog) the current one
 *     goto NAME KM       KM above planet NAME's surface, or from station NAME; NAME may be
 *                        @planet, @star, @station for the first of that kind
 *     pos X Y Z          km from the centre of the body the ship is relative to
 *     stop               zero velocity
 *   the ship
 *     refuel, repair, cash CREDITS
 *   input
 *     key NAME           tap a key: 0-9, a-z, f1-f10, esc, enter, space, or 0xNN (ST scancode)
 *     click X Y          click at game screen coordinates (320 x 200)
 *     ascii N            push an ASCII key into the game (menu items are $80 + n)
 *   other
 *     print              where we are
 *     bodies             the system's named bodies
 *     peek [b|w|l] ADDR  read game memory (ADDR: a number, or a6+N for a game global)
 *     poke [b|w|l] ADDR VALUE
 *     snap               write savs/SnapNN
 *     echo TEXT
 *     quit
 */
#include "script.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "cheats.h"
#include "game_state.h"
#include "host.h"
#include "input.h"
#include "state_file.h"

#define MAX_QUEUE 512
#define LINE_MAX_LEN 256
#define A6_DIR_LIST_LEN 17448
#define A6_DIR_LIST_ENTRIES 17454
#define DIR_ENTRY_SIZE 0x1c
#define A6_PUSHED_ASCII 16337
#define A6_SCANCODE_ASCII 16336
#define A6_SYSTEM_ID 726

static char queue[MAX_QUEUE][LINE_MAX_LEN];
static int q_head, q_tail;
static int control_fd = -1;
static char control_buf[1024];
static int control_len;

/* what a waiting command is doing */
static enum { IDLE, WAIT_TIME, WAIT_GAME, NEW_GAME, LOAD_GAME } waiting;
static Uint32 wait_until, last_try, started;
static int new_key;
static char load_name[64];

static void enqueue(const char *line)
{
	while (*line == ' ' || *line == '\t')
		line++;
	if (!*line || *line == '#')
		return;
	if ((q_tail + 1) % MAX_QUEUE == q_head)
	{
		fprintf(stderr, "script: queue full\n");
		return;
	}
	snprintf(queue[q_tail], LINE_MAX_LEN, "%s", line);
	q_tail = (q_tail + 1) % MAX_QUEUE;
}

void script_add_text(const char *commands)
{
	char buf[LINE_MAX_LEN * 8];
	snprintf(buf, sizeof(buf), "%s", commands);
	for (char *line = strtok(buf, ";\n"); line; line = strtok(NULL, ";\n"))
		enqueue(line);
}

void script_add_file(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
	{
		fprintf(stderr, "script: can't read %s\n", path);
		return;
	}
	char line[LINE_MAX_LEN];
	while (fgets(line, sizeof(line), f))
	{
		line[strcspn(line, "\r\n")] = 0;
		script_add_text(line);
	}
	fclose(f);
}

void script_set_control(const char *fifo_path)
{
#ifndef _WIN32
	mkfifo(fifo_path, 0600);
	control_fd = open(fifo_path, O_RDWR | O_NONBLOCK); /* read/write: no end of file when a writer leaves */
	if (control_fd < 0)
		fprintf(stderr, "script: can't open %s\n", fifo_path);
#else
	(void)fifo_path;
#endif
}

static void read_control(void)
{
#ifndef _WIN32
	if (control_fd < 0)
		return;
	int n = (int)read(control_fd, control_buf + control_len, sizeof(control_buf) - 1 - control_len);
	if (n > 0)
		control_len += n;
	char *nl;
	control_buf[control_len] = 0;
	while ((nl = strchr(control_buf, '\n')))
	{
		*nl = 0;
		script_add_text(control_buf);
		control_len -= (int)(nl + 1 - control_buf);
		memmove(control_buf, nl + 1, control_len + 1);
	}
	if (control_len >= (int)sizeof(control_buf) - 1)
		control_len = 0;
#endif
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	printf("script: ");
	vprintf(fmt, ap);
	printf("\n");
	fflush(stdout);
	va_end(ap);
}

/* ST scancode of a key name, or -1 */
static int scancode(const char *name)
{
	static const char row[] = "1234567890";
	static const char qwerty[] = "qwertyuiop";
	static const char asdf[] = "asdfghjkl";
	static const char zxcv[] = "zxcvbnm";
	if (!strncmp(name, "0x", 2))
		return (int)strtol(name, NULL, 16);
	if (name[0] && !name[1])
	{
		const char *p;
		if ((p = strchr(row, name[0])))
			return 0x02 + (int)(p - row);
		if ((p = strchr(qwerty, name[0])))
			return 0x10 + (int)(p - qwerty);
		if ((p = strchr(asdf, name[0])))
			return 0x1e + (int)(p - asdf);
		if ((p = strchr(zxcv, name[0])))
			return 0x2c + (int)(p - zxcv);
	}
	if (name[0] == 'f' && isdigit((unsigned char)name[1]))
	{
		int n = atoi(name + 1);
		if (n >= 1 && n <= 10)
			return 0x3b + n - 1;
	}
	if (!strcmp(name, "esc"))
		return 0x01;
	if (!strcmp(name, "enter"))
		return 0x1c;
	if (!strcmp(name, "space"))
		return 0x39;
	return -1;
}

/* (case and trailing spaces do not matter: some generated names end in a space) */
static int name_equal(const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b);
	while (la && a[la - 1] == ' ')
		la--;
	while (lb && b[lb - 1] == ' ')
		lb--;
	return la == lb && strncasecmp(a, b, la) == 0;
}

/* the dir list the load screen shows: the index of the file `name`, or -1 */
static int dir_index(const char *name)
{
	uint32_t a6 = GAME_A6;
	int count = (uint16_t)rdword(a6 + A6_DIR_LIST_LEN);
	for (int i = 0; i < count && i < 0x40; i++)
	{
		uint32_t e = a6 + A6_DIR_LIST_ENTRIES + DIR_ENTRY_SIZE * i;
		char text[24];
		int k = 0;
		for (; k < 22 && rdbyte(e + 6 + k); k++)
			text[k] = (char)rdbyte(e + 6 + k);
		text[k] = 0;
		if ((uint16_t)rdword(e) == 0 && name_equal(text, name))
			return i;
	}
	return -1;
}

static void push_ascii(int key)
{
	wrbyte(GAME_A6 + A6_PUSHED_ASCII, key);
	wrbyte(GAME_A6 + A6_SCANCODE_ASCII, 0xff);
}

static const char *body_name_of(int slot, CheatBody *bodies, int count)
{
	for (int i = 0; i < count; i++)
		if (bodies[i].slot == slot)
			return bodies[i].name;
	return "-";
}

static void cmd_goto(char *args)
{
	char *last = strrchr(args, ' ');
	if (!last)
	{
		say("goto NAME KM");
		return;
	}
	*last = 0;
	double km = atof(last + 1);
	const char *name = args;
	CheatBody bodies[64];
	int count = cheat_bodies(bodies, 64), slot = -1;
	for (int i = 0; i < count && slot < 0; i++)
	{
		const CheatBody *b = &bodies[i];
		if (!strcmp(name, "@planet") ? (b->planet && !b->star)
			: !strcmp(name, "@star")  ? b->star
			: !strcmp(name, "@station") ? !b->planet
										: name_equal(name, b->name))
			slot = b->slot;
	}
	if (slot < 0)
	{
		say("goto: no body called %s", name);
		return;
	}
	const char *why = cheat_goto_body(slot, km);
	say("goto %s %.1f km: %s", name, km, why ? why : "ok");
}

static void cmd_print(void)
{
	uint32_t a6 = GAME_A6;
	say("system %08x screen %s flight %s", (unsigned)rdlong(a6 + A6_SYSTEM_ID), game_screen_name(game_screen()),
		game_status_name(game_flight_status()));
	double km[3];
	if (cheat_have_ship() && cheat_ship_position(km))
	{
		CheatBody bodies[64];
		int count = cheat_bodies(bodies, 64);
		uint32_t ship = game_player_ship();
		say("ship %.1f %.1f %.1f km from %s (%.1f km)", km[0], km[1], km[2],
			body_name_of((uint8_t)rdbyte(ship + 68), bodies, count), sqrt(km[0] * km[0] + km[1] * km[1] + km[2] * km[2]));
	}
	else
		say("no ship");
}

static void cmd_bodies(void)
{
	CheatBody bodies[64];
	int count = cheat_bodies(bodies, 64);
	for (int i = 0; i < count; i++)
		say("body %d %s %s radius %.1f km", bodies[i].slot, bodies[i].name, bodies[i].planet ? "planet" : "station",
			bodies[i].radius_km);
}

static uint32_t parse_addr(const char *text)
{
	if (!strncmp(text, "a6+", 3))
		return GAME_A6 + (uint32_t)strtol(text + 3, NULL, 0);
	return (uint32_t)strtoul(text, NULL, 0);
}

static void cmd_peek_poke(int poke, char *args)
{
	int size = 4;
	if (args[0] == 'b' || args[0] == 'w' || args[0] == 'l')
	{
		size = args[0] == 'b' ? 1 : args[0] == 'w' ? 2 : 4;
		args += args[1] ? 2 : 1;
	}
	char *rest = args;
	while (*rest && *rest != ' ')
		rest++;
	if (*rest)
		*rest++ = 0;
	uint32_t addr = parse_addr(args);
	if (poke)
	{
		uint32_t v = (uint32_t)strtoul(rest, NULL, 0);
		size == 1 ? wrbyte(addr, v) : size == 2 ? wrword(addr, v) : wrlong(addr, (int)v);
	}
	uint32_t v = size == 1 ? (uint8_t)rdbyte(addr) : size == 2 ? (uint16_t)rdword(addr) : (uint32_t)rdlong(addr);
	say("%08x = %0*x", addr, size * 2, v);
}

/* One command. Returns after starting any wait */
static void run(char *line)
{
	char *args = line;
	while (*args && !isspace((unsigned char)*args))
		args++;
	if (*args)
		*args++ = 0;
	while (*args == ' ')
		args++;

	if (!strcmp(line, "wait"))
	{
		wait_until = SDL_GetTicks() + (Uint32)(atof(args) * 1000.0);
		waiting = WAIT_TIME;
	}
	else if (!strcmp(line, "waitgame"))
		waiting = WAIT_GAME, started = SDL_GetTicks();
	else if (!strcmp(line, "new"))
		waiting = NEW_GAME, new_key = atoi(args), last_try = 0, started = SDL_GetTicks();
	else if (!strcmp(line, "load"))
	{
		snprintf(load_name, sizeof(load_name), "%s", args);
		waiting = LOAD_GAME, last_try = 0, started = SDL_GetTicks();
	}
	else if (!strcmp(line, "enter"))
	{
		const char *why = cheat_enter_system((unsigned)strtoul(args, NULL, 16));
		say("enter %s: %s", args, why ? why : "ok");
	}
	else if (!strcmp(line, "goto"))
		cmd_goto(args);
	else if (!strcmp(line, "pos"))
	{
		double p[3] = {0, 0, 0};
		sscanf(args, "%lf %lf %lf", &p[0], &p[1], &p[2]);
		cheat_ship_set_position(p);
	}
	else if (!strcmp(line, "stop"))
		cheat_ship_stop();
	else if (!strcmp(line, "refuel"))
		cheat_refuel();
	else if (!strcmp(line, "repair"))
		cheat_repair();
	else if (!strcmp(line, "cash"))
		cheat_set_cash((long)(atof(args) * 10.0));
	else if (!strcmp(line, "key"))
	{
		int code = scancode(args);
		if (code < 0)
			say("key: unknown key %s", args);
		else
			Input_TapSTKey((unsigned char)code);
	}
	else if (!strcmp(line, "click"))
	{
		int x = 0, y = 0;
		sscanf(args, "%d %d", &x, &y);
		Input_ClickAt(x, y);
	}
	else if (!strcmp(line, "ascii"))
		push_ascii((int)strtol(args, NULL, 0));
	else if (!strcmp(line, "peek") || !strcmp(line, "poke"))
		cmd_peek_poke(line[1] == 'o', args);
	else if (!strcmp(line, "print"))
		cmd_print();
	else if (!strcmp(line, "bodies"))
		cmd_bodies();
	else if (!strcmp(line, "snap"))
	{
		char name[32];
		state_file_snapshot(name, sizeof(name));
		say("snap %s", name);
	}
	else if (!strcmp(line, "echo"))
		say("%s", args);
	else if (!strcmp(line, "quit"))
	{
		SDL_Event e = {.type = SDL_QUIT};
		SDL_PushEvent(&e);
	}
	else
		say("unknown command: %s", line);
}

/* A wait that is over? */
static int wait_done(void)
{
	Uint32 now = SDL_GetTicks();
	switch (waiting)
	{
	case WAIT_TIME:
		return now >= wait_until;
	case WAIT_GAME:
		if (now - started > 120000)
		{
			say("waitgame: gave up");
			return 1;
		}
		return game_player_ship() && game_screen() == SCREEN_FLIGHT;
	case NEW_GAME:
		if (game_player_ship() && game_screen() == SCREEN_FLIGHT)
			return 1;
		/* the title menu may not be listening yet: keep tapping the key until the game starts */
		if (game_screen() == SCREEN_TITLE && now - started > 1500 && now - last_try > 1500)
		{
			last_try = now;
			Input_TapSTKey((unsigned char)(0x02 + new_key - 1));
		}
		return 0;
	case LOAD_GAME:
		if (game_player_ship() && game_screen() == SCREEN_FLIGHT && last_try == 1)
			return 1;
		if (now - started > 60000)
		{
			say("load %s: gave up", load_name);
			return 1;
		}
		if (game_screen() == SCREEN_OPTIONS && now - started > 500)
		{
			int n = dir_index(load_name);
			if (n < 0)
			{
				say("load: no file %s in the list", load_name);
				return 1;
			}
			if (now - last_try > 1000)
			{
				push_ascii(0x80 + n);
				last_try = 1; /* sent; the game takes it from here */
			}
		}
		else if (game_screen() == SCREEN_TITLE && now - started > 1500 && now - last_try > 1500 && last_try != 1)
		{
			last_try = now;
			Input_TapSTKey(0x06); /* key 5 */
		}
		return 0;
	default:
		return 1;
	}
}

void script_poll(void)
{
	read_control();
	for (int guard = 0; guard < 64; guard++)
	{
		if (waiting != IDLE)
		{
			if (!wait_done())
				return;
			waiting = IDLE;
		}
		if (q_head == q_tail)
			return;
		char line[LINE_MAX_LEN];
		snprintf(line, sizeof(line), "%s", queue[q_head]);
		q_head = (q_head + 1) % MAX_QUEUE;
		run(line);
	}
}
