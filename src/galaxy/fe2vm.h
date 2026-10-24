/*
 * fe2vm.h - runs Frontier: Elite 2's own code (the 68k code translated to
 * C, Start680x0_at) for the galaxy atlas, so everything it shows is
 * computed by the game itself, not by a copy of its maths.
 *
 * Two ways to run (fe2vm.c):
 *  - the standalone viewer (FE2GALAXY_TOOL): fe2vm_init() loads the game
 *    binary like the game does and runs the start of its setup; the tool
 *    owns the machine.
 *  - inside the running game: the machine is the game's, stopped in one of
 *    its host calls while the menu draws. Calls go between fe2vm_begin()
 *    and fe2vm_end(), which save the machine (registers, flags, interrupt
 *    state, the game memory the calls may write) and put it all back, so
 *    the game carries on as if nothing happened.
 *
 * fe2vm_call() runs any game subroutine: registers in, registers out. The
 * host calls the game makes while it runs are the atlas's (text capture and
 * the drawing ones below); the rest do nothing.
 *
 * Not thread safe: the game has one set of registers and one RAM.
 */
#ifndef FE2VM_H
#define FE2VM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	uint32_t d[8];
	uint32_t a[8]; /* a[5], a[6] and a[7] are set by fe2vm_call */
} fe2vm_regs;

/* 0 on success, else fe2vm_error() says why */
int fe2vm_init(void);
/* Inside the running game: bracket every group of calls (and reads of RAM
 * they leave behind) with these. They nest; the outermost pair saves and
 * restores. Calls made outside a pair make their own. */
void fe2vm_begin(void);
void fe2vm_end(void);
/* fe2vm_end that keeps what the calls changed in RAM (the game goes on from there) */
void fe2vm_end_keep(void);
/* Batches begun from now on keep the game's own host calls (the drawing
 * ones record the scene, gl_scene.h) instead of the atlas's; 0 back */
void fe2vm_game_hcalls(int on);
/* for testing in the standalone viewer: work the way the game does */
void fe2vm_set_live(int on);
/* runs the subroutine at addr until it returns; 0 on success, else -1, or
 * FE2VM_TIMEOUT when it never returns (the game code loops forever) */
#define FE2VM_TIMEOUT (-2)
int fe2vm_call(uint32_t addr, fe2vm_regs *r);
const char *fe2vm_error(void);

/* Called for the game's DrawStr / DrawStrShadowed host calls (d0 colour,
 * d1 x, d2 y, a0 string); returns the x the text ended at, which the game
 * gets back in d1. NULL (the default) draws nothing and returns x. */
typedef int (*fe2vm_text_fn)(int x, int y, int colour, const uint8_t *str, void *user);
void fe2vm_set_text_hook(fe2vm_text_fn fn, void *user);

/* The game's screen (320x200, a palette index per pixel), with what the
 * drawing host calls put there: bitmaps, lines, clears. Text goes to the
 * hook above instead. */
const uint8_t *fe2vm_screen(void);
void fe2vm_clear_screen(void);

/* a hash of the game binary compiled in, to tell caches of other builds apart */
uint32_t fe2vm_game_hash(void);

uint32_t fe2vm_a6(void); /* the game's globals (L5eb6_a6_base) */
uint32_t fe2vm_scratch(void); /* 4K of free RAM for arguments and results */

uint8_t fe2vm_rd8(uint32_t addr);
uint16_t fe2vm_rd16(uint32_t addr);
uint32_t fe2vm_rd32(uint32_t addr);
void fe2vm_wr8(uint32_t addr, uint8_t v);
void fe2vm_wr16(uint32_t addr, uint16_t v);
void fe2vm_wr32(uint32_t addr, uint32_t v);

#ifdef __cplusplus
}
#endif

#endif /* FE2VM_H */
