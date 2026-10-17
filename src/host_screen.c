/*
 * host_screen.c - host calls the game uses to draw into its 320x200
 * screen and to set up palettes and screen buffers.
 *
 * Arguments come on the 68k stack (read with param_long / param_word) or in
 * registers, as noted per call.
 */
#include <SDL_endian.h>

#include "host.h"
#include "hostcall.h"
#include "main.h"
#include "renderer.h"
#include "screen_text.h"

/* n-th long / word argument on the 68k stack */
static u32 param_long(int n)
{
	return (u32)STMemory_ReadLong(GetReg(REG_A7) + n * SIZE_LONG);
}
static u16 param_word(int n)
{
	return (u16)STMemory_ReadWord(GetReg(REG_A7) + n * SIZE_WORD);
}

/* The game still thinks lines are 160 bytes long (4 bitplanes); the host
 * screens use 320 bytes per line (one byte per pixel). A3 addresses are
 * relative to one of the two screen buffers. */
static u32 fix_line_address(u32 scr)
{
	u32 base = (scr & 0x100000) ? 0x100000 : 0xf0000;
	return (scr - base) * 2 + base;
}

/* Colour arguments are palette index * 4 */
static int colour_arg(void)
{
	return param_word(0) >> 2;
}

/* ---- clears / copies --------------------------------------------------- */

/* (count, address): clear to black, or to "transparent" (255) when the GL
 * renderer draws the 3D view underneath */
void Call_Memset(void)
{
	memset(STRam + param_long(1), use_renderer == R_OLD ? 0 : 255, param_long(0));
	fe2_bgcol = 0;
}

/* (count, address): same, with the blue background colour (14) */
void Call_MemsetBlue(void)
{
	memset(STRam + param_long(1), use_renderer == R_OLD ? 0xe : 255, param_long(0));
	fe2_bgcol = 0xe;
}

/* (dest, src, count) */
void Call_Memcpy(void)
{
	memcpy(STRam + param_long(0), STRam + param_long(1), param_long(2));
}

/* The pointer is drawn by the host, so these only note that it is shown */
void Call_BlitCursor(void)
{
	mouse_shown = 1;
}
void Call_RestoreUnderCursor(void) {}

/* ---- pixels and lines (A3 = line address, D4 = x) ------------------------ */
void Call_PutPix(void)
{
	STRam[fix_line_address(GetReg(REG_A3)) + (u16)GetReg(REG_D4)] = (char)colour_arg();
}

/* A whole screen line (the sky haze bands) */
void Call_FillLine(void)
{
	memset(STRam + fix_line_address(GetReg(REG_A3)), colour_arg(), SCREENBYTES_LINE);
}

/* D4 = x, D5 = length * 2 */
void Call_OldHLine(void)
{
	char *pix = STRam + fix_line_address(GetReg(REG_A3)) + (u16)GetReg(REG_D4);
	memset(pix, colour_arg(), (u16)GetReg(REG_D5) / 2);
}

/* D1 = colour * 4, D4 = x, D5 = length; A3 is already a host line address */
void Call_HLine(void)
{
	char *pix = STRam + GetReg(REG_A3) + (GetReg(REG_D4) & 0xffff);
	memset(pix, (GetReg(REG_D1) & 0xffff) >> 2, GetReg(REG_D5) & 0xffff);
}

/* Scanner stalks below the scanner plane: set the 16 pixels selected by
 * the D7 mask, but only where the screen is still colour 0. */
void Call_BackHLine(void)
{
	char *pix = STRam + fix_line_address(GetReg(REG_A3));
	int col = colour_arg(), mask = (u16)GetReg(REG_D7);
	for (int i = 15; i >= 0; i--, pix++)
		if ((mask & (1 << i)) && *pix == 0)
			*pix = (char)col;
}

/* (width in 16 pixel words, height, x, y, bitmap, screen): blits a 4
 * bitplane ST bitmap as one byte per pixel */
void Call_BlitBmp(void)
{
	int width = (short)param_word(0);
	int height = (short)param_word(1);
	int org_x = (short)param_word(2);
	int org_y = (short)param_word(3);
	u32 bmp = (u32)STMemory_ReadLong(GetReg(REG_A7) + 4 * SIZE_WORD);
	u32 scr = (u32)STMemory_ReadLong(GetReg(REG_A7) + 4 * SIZE_WORD + SIZE_LONG);

	if (org_x < 0 || org_y < 0 || height > 200 || width > 320)
		return;

	const char *src = STRam + bmp + 4;
	char *line = STRam + scr + org_y * SCREENBYTES_LINE + org_x;
	const int plane = 2 * height * width; /* bytes between bitplanes */

	for (int y = 0; y < height; y++, line += SCREENBYTES_LINE)
	{
		char *dst = line;
		for (int w = 0; w < width; w++, src += 2)
		{
			u16 p0 = SDL_SwapBE16(*(const u16 *)src);
			u16 p1 = SDL_SwapBE16(*(const u16 *)(src + plane));
			u16 p2 = SDL_SwapBE16(*(const u16 *)(src + 2 * plane));
			u16 p3 = SDL_SwapBE16(*(const u16 *)(src + 3 * plane));
			for (int bit = 15; bit >= 0; bit--)
				*dst++ = (char)(((p0 >> bit) & 1) | ((p1 >> bit) & 1) << 1 | ((p2 >> bit) & 1) << 2 |
								((p3 >> bit) & 1) << 3);
		}
	}
}

/* ---- text (A0 = string, D0 = colour, D1 = x, D2 = y; returns x in D1) ---- */
void Call_DrawStr(void)
{
	SetReg(REG_D1, DrawStr(GetReg(REG_D1), GetReg(REG_D2), GetReg(REG_D0),
						   (unsigned char *)(STRam + GetReg(REG_A0)), false));
}

void Call_DrawStrShadowed(void)
{
	SetReg(REG_D1, DrawStr(GetReg(REG_D1), GetReg(REG_D2), GetReg(REG_D0),
						   (unsigned char *)(STRam + GetReg(REG_A0)), true));
}

/* ---- palettes and screen buffers ---------------------------------------- */

/* Dynamic colours allocated this frame, copied into the main palette
 * (entries 16+) when the screen is flipped */
static int ext_pal_len;
static unsigned short ext_pal[240];

static void read_palette(unsigned short *dst, u32 src)
{
	for (int i = 0; i < 16; i++, src += 2)
		dst[i] = STMemory_ReadWord(src);
}

void Call_SetMainPalette(void)
{
	read_palette(MainPalette, param_long(0));
}
void Call_SetCtrlPalette(void)
{
	read_palette(CtrlPalette, param_long(0));
}

/* (phys2, log2, phys, log): ST RAM locations of the screen pointers */
void Call_InformScreens(void)
{
	physcreen2 = param_long(0);
	logscreen2 = param_long(1);
	physcreen = param_long(2);
	logscreen = param_long(3);
}

/* (screen address): the screen to show; also installs this frame's
 * dynamic colours */
void Call_SetScreenBase(void)
{
	VideoBase = param_long(0);
	VideoRaster = (unsigned char *)STRam + VideoBase;
	memcpy(&MainPalette[16], ext_pal, ext_pal_len * sizeof(ext_pal[0]));
	len_main_palette = 16 + ext_pal_len;
}

/* What the screen calls leave in the host (palettes, the screen shown, the
 * dynamic colours): a batch of game calls on the side (gl_mapscreen.c)
 * puts it back after */
void host_screen_save(HostScreenState *s)
{
	memcpy(s->main, MainPalette, sizeof s->main);
	memcpy(s->ctrl, CtrlPalette, sizeof s->ctrl);
	memcpy(s->ext, ext_pal, sizeof s->ext);
	s->len_main = len_main_palette;
	s->ext_len = ext_pal_len;
	s->base = VideoBase;
	s->raster = VideoRaster;
	s->bgcol = fe2_bgcol;
	s->mouse_shown = mouse_shown;
}

void host_screen_restore(const HostScreenState *s)
{
	memcpy(MainPalette, s->main, sizeof s->main);
	memcpy(CtrlPalette, s->ctrl, sizeof s->ctrl);
	memcpy(ext_pal, s->ext, sizeof s->ext);
	len_main_palette = s->len_main;
	ext_pal_len = s->ext_len;
	VideoBase = s->base;
	VideoRaster = s->raster;
	fe2_bgcol = s->bgcol;
	mouse_shown = s->mouse_shown;
}

/* (colour list): list of (rgb444, index * 4) pairs. Dynamic colours live
 * in palette entries 16+, so the indices are moved up by 16 in place. */
void Call_MakeExtPalette(void)
{
	u32 list = param_long(0);
	int len = STMemory_ReadWord(list) >> 2;
	if (len > (int)(sizeof(ext_pal) / sizeof(ext_pal[0])))
		len = sizeof(ext_pal) / sizeof(ext_pal[0]);
	ext_pal_len = len;
	list += 2;
	for (int i = 0; i < len; i++, list += 4)
	{
		ext_pal[i] = STMemory_ReadWord(list);
		STMemory_WriteWord(list + 2, STMemory_ReadWord(list + 2) + (16 << 2));
	}
}
