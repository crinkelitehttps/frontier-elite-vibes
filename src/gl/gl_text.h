/*
 * gl_text.h - the game's text in high resolution: the characters DrawStr
 * noted (screen_text_record) drawn as quads from an atlas of the game's own
 * 8x8 font, smoothed (Scale2x three times: 64x64 a glyph, the steps of its
 * diagonals rounded off) and mipmapped. Each lands where the game would
 * have put its pixels, so the layout is the game's.
 *
 * Used for the MAP and SHIP screens' pages (gl_mapscreen.h): their text above
 * the panel's rows, the host's own pages' (ship log, debug) too.
 */
#ifndef GL_TEXT_H
#define GL_TEXT_H

#include "screen_text.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* Render thread, while recording a frame (gl_draw.h): draws `t` over what is
 * there, on the current viewport taken as the 320x200 screen, in the colours
 * `pal` (ABGR by colour index). Glyphs that touch one of the `nskip`
 * rectangles (x, y, w, h in screen pixels: buttons drawn over the view) are
 * left out. `t` and `pal` must stay as they are until the frame is flushed. */
void gl_text_queue(const TextGlyphs *t, const unsigned int *pal, const int (*skip)[4], int nskip);

void gl_text_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
