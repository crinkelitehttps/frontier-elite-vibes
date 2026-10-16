/*
 * screen_text.c - the game's bitmap font and text drawing into the emulated
 * 320x200 framebuffer. Shared by the GL and SDL renderers.
 */
#include <string.h>

#include "main.h"
#include "m68000.h"
#include "renderer.h"
#include "screen_text.h"

#include "gl/gl_cockpit.h"
#include "screen_font.h" /* font_bmp: 8x8 glyphs, 10 bytes each */

#define FONT_FIRST 0x20
#define FONT_COUNT ((int)(sizeof(font_bmp) / 10))

const unsigned char *screen_text_glyph(int ch)
{
	if (ch < FONT_FIRST || ch >= FONT_FIRST + FONT_COUNT)
		return NULL;
	return font_bmp + (ch - FONT_FIRST) * 10;
}

static int draw_char(int col, int xoffset, char *scrline, int chr)
{
	const unsigned char *glyph = font_bmp + (chr & 0xff) * 10;
	if (xoffset < 0)
		return xoffset + glyph[9];
	for (int row = 0; row < 8; row++, scrline += SCREENBYTES_LINE)
	{
		unsigned char bits = glyph[row];
		for (int bit = 0; bit < 8 && xoffset + bit <= 319; bit++)
			if (bits & (0x80 >> bit))
				scrline[xoffset + bit] = (char)col;
	}
	return xoffset + glyph[9];
}

static char *text_base; /* screen_text_render_queue's buffer, else the logical screen */

int DrawStr(int xpos, int ypos, int col, unsigned char *str, bool shadowed)
{
	int x = xpos, y = ypos, chr;
	char *screen;
	if (y > 192 || y < 0)
		return x;
set_line:
	screen = (text_base ? text_base : LOGSCREEN2) + SCREENBYTES_LINE * y;
	while (*str)
	{
		chr = *(str++);
		if (chr < 0x1e)
		{
			if (chr == '\r')
			{
				y += 10;
				x = xpos;
				goto set_line;
			}
			else if (chr == 1)
				col = *(str++); /* colour change */
			continue;
		}
		else if (chr == 0x1e)
		{
			x = (*(str++)) * 2; /* new x */
			continue;
		}
		else if (chr < 0x20)
		{
			x = (*(str++)) * 2; /* new x, y */
			y = *(str++);
			goto set_line;
		}
		if (shadowed)
			draw_char(0, x + 1, screen + SCREENBYTES_LINE, chr - 0x20);
		x = draw_char(col, x, screen, chr - 0x20);
	}
	return x;
}

/* =========================================================================
 * Text the game asks to draw late, on top of the 3D view (GL renderer)
 * ========================================================================= */
#define MAX_QUEUED_STRINGS 200

static struct
{
	int x, y, col;
	unsigned char str[64];
} queued[MAX_QUEUED_STRINGS];
static int n_queued;

void Nu_QueueDrawStr(void)
{
	/* text on things in the 3D view, at the classic view's positions: not
	 * from the cockpit's turned passes */
	if (n_queued >= MAX_QUEUED_STRINGS || cockpit_turned_pass())
		return;
	strncpy((char *)queued[n_queued].str, GetReg(REG_A0) + STRam, sizeof(queued[0].str) - 1);
	queued[n_queued].str[sizeof(queued[0].str) - 1] = 0;
	queued[n_queued].x = GetReg(REG_D1);
	queued[n_queued].y = GetReg(REG_D2);
	queued[n_queued].col = GetReg(REG_D0);
	n_queued++;
}

void screen_text_clear_queue(void)
{
	n_queued = 0;
}

void screen_text_draw_queue(void)
{
	/* drawn into the front buffer so it sits on top of the 3D view */
	unsigned long saved = logscreen2;
	logscreen2 = physcreen;
	for (int i = 0; i < n_queued; i++)
		DrawStr(queued[i].x, queued[i].y, queued[i].col, queued[i].str, false);
	logscreen2 = saved;
}

bool screen_text_render_queue(unsigned char *buf)
{
	memset(buf, 0xff, SCREENBYTES_LINE * 200);
	text_base = (char *)buf;
	for (int i = 0; i < n_queued; i++)
		DrawStr(queued[i].x, queued[i].y, queued[i].col, queued[i].str, false);
	text_base = NULL;
	return n_queued > 0;
}
