/*
 * fe2vm.c - see fe2vm.h. The game code's side is Start680x0_at and the
 * fe2vm_depth / fe2vm_check / fe2vm_bad_jump hooks (host.h), which
 * tools/as68k puts in the translated code.
 */
#include "fe2vm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "galaxy_labels.h"

/* Every jump counts as a tick. The biggest call the atlas makes (a system
 * with 60 bodies) runs well under 100000; this catches a routine that never
 * returns, like one waiting for a vblank that never comes. */
#define MAX_TICKS 3000000

/* The screens L426e2_main sets up; the game's stack starts below them */
#define LOGSCREEN 0x100000u
#define PHYSCREEN 0xf0000u
#define SCREEN_BYTES (320 * 200)

/* Calls get a 4K scratch area for arguments and results, and run on a stack
 * below it. Inside the game both go below the game's own stack (it is all
 * put back after, see fe2vm_begin). */
#define SCRATCH_SIZE 0x1000u

int fe2vm_depth;

static long ticks;
static int failed; /* 1 error, 2 ran too long */
static char error[256];

/* the last jumps, to say where a call got stuck */
#define RECENT 256
static int recent[RECENT];

/* -- hooks the translated code calls (host.h) ------------------------------ */

int fe2vm_check(s32 jdest)
{
	if (jdest == FE2VM_STOP)
		return 1;
	recent[ticks & (RECENT - 1)] = jdest;
	if (++ticks < MAX_TICKS)
		return 0;
	/* the most common recent target is in the loop */
	int best = 0, best_n = 0;
	for (int i = 0; i < RECENT; i++)
	{
		int n = 0;
		for (int j = 0; j < RECENT; j++)
			n += recent[j] == recent[i];
		if (n > best_n)
			best = recent[i], best_n = n;
	}
	failed = 2;
	snprintf(error, sizeof(error), "game code never returned: looping at $%x", best);
	return 1;
}

void fe2vm_bad_jump(s32 jdest)
{
	failed = 1;
	snprintf(error, sizeof(error), "game code jumped to a bad address $%x (after $%x $%x $%x)", jdest,
			 recent[(ticks - 1) & (RECENT - 1)], recent[(ticks - 2) & (RECENT - 1)], recent[(ticks - 3) & (RECENT - 1)]);
}

/* -- host calls while the atlas runs game code ----------------------------- */

static fe2vm_text_fn text_fn;
static void *text_user;

void fe2vm_set_text_hook(fe2vm_text_fn fn, void *user)
{
	text_fn = fn;
	text_user = user;
}

static void hcall_nothing(void) {}

/* Call_DrawStr / Call_DrawStrShadowed (src/host_screen.c). Text is not drawn
 * into the screen: the capture hook keeps it, and the atlas draws it. */
static void hcall_draw_str(void)
{
	if (text_fn)
		Regs[1]._s32 = text_fn(Regs[1]._s32, Regs[2]._s32, Regs[0]._s32, (const uint8_t *)m68kram + Regs[8]._u32,
							   text_user);
}

/* src/host_screen.c's drawing calls, into 68k RAM as in the game (one byte
 * per pixel, 320 bytes a line) */
static u32 param_long(int n)
{
	return (u32)rdlong(Regs[15]._u32 + n * 4);
}
static u16 param_word(int n)
{
	return (u16)rdword(Regs[15]._u32 + n * 2);
}
static u32 fix_line_address(u32 scr)
{
	u32 base = (scr & 0x100000) ? 0x100000 : 0xf0000;
	return (scr - base) * 2 + base;
}
static int in_ram(u32 addr, u32 len)
{
	return addr < MEM_SIZE && len <= MEM_SIZE - addr;
}


static void hcall_memset(void) /* (count, address) */
{
	if (in_ram(param_long(1), param_long(0)))
		memset(m68kram + param_long(1), 0, param_long(0));
}
static void hcall_memset_blue(void)
{
	if (in_ram(param_long(1), param_long(0)))
		memset(m68kram + param_long(1), 0xe, param_long(0));
}
static void hcall_memcpy(void) /* (dest, src, count) */
{
	if (in_ram(param_long(0), param_long(2)) && in_ram(param_long(1), param_long(2)))
		memmove(m68kram + param_long(0), m68kram + param_long(1), param_long(2));
}
static void hcall_put_pix(void) /* A3 line, D4 x */
{
	u32 a = fix_line_address(Regs[11]._u32) + (u16)Regs[4]._u32;
	if (in_ram(a, 1))
		m68kram[a] = (s8)(param_word(0) >> 2);
}
static void hcall_fill_line(void)
{
	u32 a = fix_line_address(Regs[11]._u32);
	if (in_ram(a, 320))
		memset(m68kram + a, param_word(0) >> 2, 320);
}
static void hcall_old_hline(void) /* D4 x, D5 length * 2 */
{
	u32 a = fix_line_address(Regs[11]._u32) + (u16)Regs[4]._u32;
	if (in_ram(a, (u16)Regs[5]._u32 / 2))
		memset(m68kram + a, param_word(0) >> 2, (u16)Regs[5]._u32 / 2);
}
static void hcall_hline(void) /* D1 colour * 4, D4 x, D5 length, A3 host line */
{
	u32 a = Regs[11]._u32 + (Regs[4]._u32 & 0xffff);
	if (in_ram(a, Regs[5]._u32 & 0xffff))
		memset(m68kram + a, (Regs[1]._u32 & 0xffff) >> 2, Regs[5]._u32 & 0xffff);
}
/* (width in 16 pixel words, height, x, y, bitmap, screen): 4 bitplane ST
 * bitmap to one byte per pixel */
static u32 drawn_screen; /* the screen the last bitmap went to */

static void hcall_blit_bmp(void)
{
	int width = (short)param_word(0), height = (short)param_word(1);
	int org_x = (short)param_word(2), org_y = (short)param_word(3);
	u32 bmp = (u32)rdlong(Regs[15]._u32 + 8), scr = (u32)rdlong(Regs[15]._u32 + 12);
	if (org_x < 0 || org_y < 0 || height > 200 || width > 320 || width <= 0 || height <= 0)
		return;
	const int plane = 2 * height * width;
	if (!in_ram(bmp + 4, 4 * plane) || !in_ram(scr + org_y * 320 + org_x, (height - 1) * 320 + width * 16))
		return;
	if (scr == LOGSCREEN || scr == PHYSCREEN)
		drawn_screen = scr;
	const u8 *src = (const u8 *)m68kram + bmp + 4;
	u8 *line = (u8 *)m68kram + scr + org_y * 320 + org_x;
	for (int y = 0; y < height; y++, line += 320)
	{
		u8 *dst = line;
		for (int w = 0; w < width; w++, src += 2)
		{
			u16 p0 = (u16)(src[0] << 8 | src[1]), p1 = (u16)(src[plane] << 8 | src[plane + 1]);
			u16 p2 = (u16)(src[2 * plane] << 8 | src[2 * plane + 1]);
			u16 p3 = (u16)(src[3 * plane] << 8 | src[3 * plane + 1]);
			for (int bit = 15; bit >= 0; bit--)
				*dst++ = (u8)(((p0 >> bit) & 1) | ((p1 >> bit) & 1) << 1 | ((p2 >> bit) & 1) << 2 |
							  ((p3 >> bit) & 1) << 3);
		}
	}
}

/* the host calls the atlas answers; everything else does nothing */
static void atlas_hcalls(HOSTCALL *table)
{
	for (int i = 0; i < hcalls_count; i++)
		table[i] = hcall_nothing;
	table[0x01] = hcall_memset;
	table[0x02] = hcall_memset_blue;
	table[0x05] = hcall_blit_bmp;
	table[0x06] = hcall_old_hline;
	table[0x08] = hcall_memcpy;
	table[0x09] = hcall_put_pix;
	table[0x0b] = hcall_fill_line;
	table[0x16] = hcall_hline;
	table[0x1b] = hcall_draw_str; /* Call_DrawStrShadowed */
	table[0x1c] = hcall_draw_str; /* Call_DrawStr */
}

/* -- the machine: the tool's own, or the running game's --------------------- */

#ifdef FE2GALAXY_TOOL

/* what the translated code expects from the host */
HOSTCALL hcalls[256];
const int hcalls_count = 256;
int dump_m68k_toggle;
void log_printf(const char *fmt, ...)
{
	(void)fmt; /* load_binfile's report; nothing to show */
}

static void hcall_set_exception_handler(void) /* D0.b = number, A0 = handler */
{
	exception_handlers[Regs[0]._u32 & 31] = Regs[8]._u32;
}

static int live; /* the tool owns the machine; fe2vm_set_live tests the game's way */

int fe2vm_init(void)
{
	atlas_hcalls(hcalls);
	hcalls[0x00] = hcall_set_exception_handler;
	Init680x0(); /* load and relocate the binary, as the game does */

	/* the screen pointers, as L426e2_main sets them */
	wrlong(G_logscreen, LOGSCREEN);
	wrlong(G_physcreen, PHYSCREEN);
	wrlong(G_logscreen2, LOGSCREEN);
	wrlong(G_physcreen2, PHYSCREEN);

	/* the parts of L426e2_main the galaxy code relies on */
	fe2vm_regs r;
	memset(&r, 0, sizeof(r));
	if (fe2vm_call(G_SetupA6Jumptab, &r)) /* module table (A6+12...) and module setup */
		return -1;
	memset(&r, 0, sizeof(r));
	if (fe2vm_call(G_UseMainGameData, &r)) /* game data and string table pointers */
		return -1;
	return 0;
}

#else

static int live = 1; /* inside the running game */

int fe2vm_init(void)
{
	return 0; /* the game has set itself up */
}

#endif

void fe2vm_set_live(int on)
{
	live = on;
}

/* Inside the running game: what a batch of calls saves and puts back */
static int batch;		/* fe2vm_begin depth */
static u32 batch_stack; /* the top of what the calls may use */
static union Reg saved_regs[16];
static s32 saved_flags[10], saved_rdest, saved_pending;
static u32 saved_handlers[32];
static HOSTCALL *saved_hcalls; /* hcalls_count of them */
static int game_hcalls, swapped; /* fe2vm_game_hcalls; the batch swapped them */

void fe2vm_game_hcalls(int on)
{
	game_hcalls = on;
}

/* All of the game's RAM. The calls write the galaxy map's globals, the body
   list, string buffers, the screens, the stack below the game's, and a few
   other odd globals (the mouse redraw's frame counter...): a copy of the
   lot, 1.1MB, is simpler and surer than a list, and costs well under a
   millisecond a frame. */
static u8 *saved_ram;
static int keep_ram; /* fe2vm_end_keep: leave what the calls wrote */


static u32 stack_top(void)
{
	return live && batch ? batch_stack : PHYSCREEN;
}

void fe2vm_begin(void)
{
	if (!live || batch++)
		return;
	/* no vblank may start inside a call: take the handlers away first, then
	   what is already pending */
	memcpy(saved_handlers, exception_handlers, sizeof(saved_handlers));
	memset(exception_handlers, 0, sizeof(exception_handlers));
	saved_pending = EXCEPTIONS_PENDING_SWAP(0);
	EXCEPTIONS_DEFERRED_SWAP(0); /* none from before (no handler then) */

	memcpy(saved_regs, Regs, sizeof(saved_regs));
	s32 flags[10] = {N, nZ, V, C, X, bN, bnZ, bV, bC, bX};
	memcpy(saved_flags, flags, sizeof(flags));
	saved_rdest = rdest;

	/* below the game's stack, clear of where it is now */
	u32 sp = Regs[15]._u32 < PHYSCREEN ? Regs[15]._u32 : PHYSCREEN;
	batch_stack = (sp - 64) & ~3u;
	if (!saved_ram)
		saved_ram = (u8 *)malloc(MEM_SIZE);
	memcpy(saved_ram, m68kram, MEM_SIZE);

	swapped = !game_hcalls;
	if (swapped)
	{
		if (!saved_hcalls)
			saved_hcalls = (HOSTCALL *)malloc(hcalls_count * sizeof(HOSTCALL));
		memcpy(saved_hcalls, hcalls, hcalls_count * sizeof(HOSTCALL));
		atlas_hcalls(hcalls);
	}
	fe2vm_depth = 1;
}

void fe2vm_end(void)
{
	if (!live || !batch || --batch)
		return;
	fe2vm_depth = 0;
	if (swapped)
		memcpy(hcalls, saved_hcalls, hcalls_count * sizeof(HOSTCALL));
	if (!keep_ram)
		memcpy(m68kram, saved_ram, MEM_SIZE);
	keep_ram = 0;

	memcpy(Regs, saved_regs, sizeof(saved_regs));
	N = saved_flags[0], nZ = saved_flags[1], V = saved_flags[2], C = saved_flags[3], X = saved_flags[4];
	bN = saved_flags[5], bnZ = saved_flags[6], bV = saved_flags[7], bC = saved_flags[8], bX = saved_flags[9];
	rdest = saved_rdest;

	/* what was pending, and any vblank that came in while the handlers were
	   gone (deferred, host.h), back; then the handlers. Dropping them lost
	   the game's input polls: a world capture and a camera picture take up
	   to half of each second, so half its vblanks. */
	EXCEPTIONS_PENDING_OR(saved_pending | EXCEPTIONS_DEFERRED_SWAP(0));
	memcpy(exception_handlers, saved_handlers, sizeof(saved_handlers));
}

void fe2vm_end_keep(void)
{
	keep_ram = batch == 1;
	fe2vm_end();
}

/* -- API ------------------------------------------------------------------ */

const char *fe2vm_error(void)
{
	return error;
}

uint32_t fe2vm_a6(void)
{
	return G_a6_base;
}

uint32_t fe2vm_scratch(void)
{
	return stack_top() - SCRATCH_SIZE;
}

uint8_t fe2vm_rd8(uint32_t addr)
{
	return (uint8_t)rdbyte(addr);
}
uint16_t fe2vm_rd16(uint32_t addr)
{
	return (uint16_t)rdword(addr);
}
uint32_t fe2vm_rd32(uint32_t addr)
{
	return (uint32_t)rdlong(addr);
}
void fe2vm_wr8(uint32_t addr, uint8_t v)
{
	wrbyte(addr, v);
}
void fe2vm_wr16(uint32_t addr, uint16_t v)
{
	wrword(addr, v);
}
void fe2vm_wr32(uint32_t addr, uint32_t v)
{
	wrlong(addr, (int)v);
}

/* fe2_bin.h (or fe2_orig_bin.h), compiled into the game code (host.c) */
#if FE2_USE_MODDED
extern unsigned char fe2_modded_s_bin[];
extern unsigned int fe2_modded_s_bin_len;
#define GAME_BIN fe2_modded_s_bin
#define GAME_BIN_LEN fe2_modded_s_bin_len
#else
extern unsigned char fe2_s_bin[];
extern unsigned int fe2_s_bin_len;
#define GAME_BIN fe2_s_bin
#define GAME_BIN_LEN fe2_s_bin_len
#endif

uint32_t fe2vm_game_hash(void)
{
	uint32_t h = 2166136261u; /* FNV-1a */
	for (unsigned int i = 0; i < GAME_BIN_LEN; i++)
		h = (h ^ GAME_BIN[i]) * 16777619u;
	return h;
}

/* The screen the drawing went to. Screens draw into logscreen2, but the game
   points that at the front buffer for a moment to draw there too
   (PhysToLog2 / LogToLog2) and flips the two, so which one it ends up in is
   best taken from where the bitmaps actually went. */
const uint8_t *fe2vm_screen(void)
{
	return (const uint8_t *)m68kram + (drawn_screen ? drawn_screen : (u32)rdlong(G_logscreen2));
}

void fe2vm_clear_screen(void)
{
	memset(m68kram + LOGSCREEN, 0, SCREEN_BYTES);
	memset(m68kram + PHYSCREEN, 0, SCREEN_BYTES);
	drawn_screen = 0;
}

int fe2vm_call(uint32_t addr, fe2vm_regs *r)
{
	fe2vm_begin();
	u32 sp = fe2vm_scratch();
	for (int i = 0; i < 8; i++)
	{
		Regs[i]._u32 = r->d[i];
		Regs[8 + i]._u32 = r->a[i];
	}
	Regs[13]._u32 = G_a5_jumptab;
	Regs[14]._u32 = G_a6_base;
	Regs[15]._u32 = sp - 4;
	wrlong(sp - 4, FE2VM_STOP); /* the return address */
	ticks = 0;
	failed = 0;
	int depth = fe2vm_depth;
	fe2vm_depth = 1;
	Start680x0_at((s32)addr);
	fe2vm_depth = depth;
	for (int i = 0; i < 8; i++)
	{
		r->d[i] = Regs[i]._u32;
		r->a[i] = Regs[8 + i]._u32;
	}
	if (!failed && Regs[15]._u32 != sp)
	{
		failed = 1;
		snprintf(error, sizeof(error), "game routine $%x left the stack unbalanced", (unsigned)addr);
	}
	fe2vm_end();
	return failed == 2 ? FE2VM_TIMEOUT : failed ? -1 : 0;
}
