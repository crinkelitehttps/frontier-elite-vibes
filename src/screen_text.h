/*
 * screen_text.h - text drawing into the emulated framebuffer.
 */
#ifndef SCREEN_TEXT_H
#define SCREEN_TEXT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Draws a game string (with its embedded colour / position codes) into the
 * logical screen. Returns the x position after the text. */
int DrawStr(int xpos, int ypos, int col, unsigned char *str, bool shadowed);

/* The game's 8x8 font: 10 bytes per glyph, rows 0..7 (bit 7 = leftmost
 * pixel), byte 9 = advance width. NULL if the character has no glyph. */
const unsigned char *screen_text_glyph(int ch);

/* Host call: queue a string to be drawn at present time */
void Nu_QueueDrawStr(void);
void screen_text_clear_queue(void);
void screen_text_draw_queue(void);
/* The queue drawn into `buf` (320x200 colour indices) instead, 0xff where
 * there is no text; false if there was none */
bool screen_text_render_queue(unsigned char *buf);

#ifdef __cplusplus
}
#endif

#endif /* SCREEN_TEXT_H */
