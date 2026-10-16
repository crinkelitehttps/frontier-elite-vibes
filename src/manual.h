/*
 * manual.h - the pages on the cockpit clipboard (gl_clipboard.h): the
 * controls, made from the loaded gamepad bindings and the port's keys,
 * then the game manual, the pages of a PDF scan the player supplies.
 *
 * HOW IT WORKS
 *   The clipboard image of a page (vibe/clipboard_image.h: board, paper, clip with
 *   "CONTROLS 2/4" or "MANUAL 29/196", the page on the paper) is made on a
 *   thread of its own: the page being read first, then its neighbours;
 *   images more than 3 pages away are dropped. The PDF is read with
 *   vibe::poppler (poppler-cpp) when the build has it (GLF_HAVE_POPPLER); without it, or
 *   without a PDF, there are only the controls pages.
 *
 *   The PDF: --manual FILE, else the first *.pdf in the working directory
 *   (the play folder, beside glfrontier.cfg). The page being read is kept
 *   in glfrontier.cfg (manual_page).
 *
 * Spec: ~/projects/shared/specs/cockpit-clipboard.md.
 */
#ifndef MANUAL_H
#define MANUAL_H

#include <stdbool.h>

#ifdef __cplusplus
#include <cstdint>
#include <memory>
#include <vector>

#include "vibe/cockpit_clipboard.h"

extern "C"
{
#endif

/* --manual FILE (main.c), NULL: look for one */
extern const char *manual_arg;

/* Main thread, once the bindings and settings are loaded: makes the
 * controls pages, opens the PDF and asks for the page being read */
void manual_init(void);

/* Pages: controls, then the PDF's */
int manual_pages(void);
/* The page being read (0-based), any thread */
int manual_page(void);
/* Sets it (glfrontier.cfg; kept for manual_init if before it) */
void manual_set_page(int page);
/* Turns `n` pages (- back), held to the first and last; any thread */
void manual_turn(int n);

#ifdef __cplusplus
}

/* The clipboard: the board, its pages and the page being read
 * (gl_clipboard.cpp moves and draws it) */
vibe::Clipboard &clipboard();
#endif

#endif /* MANUAL_H */
