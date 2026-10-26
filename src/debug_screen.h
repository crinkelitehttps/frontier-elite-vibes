/*
 * debug_screen.h - live debug text for the cockpit SHIP screen's last page.
 *
 * The SHIP screen turns through the game's pages (status, cargo, ...) with a
 * tap away from its buttons or F3; one more turn shows this page, drawn by the
 * host (gl_mapscreen.c) instead of the game. Any code, on any thread, puts
 * its debug information here as the game is played:
 *
 *   debug_screen_set("pirates", "%d near, nearest %.1f km", n, km);  a line that is updated in place
 *   debug_screen_log("pirate %d spawned at %d km", id, km);          a line added to the scrolling log
 */
#ifndef DEBUG_SCREEN_H
#define DEBUG_SCREEN_H

#ifdef __cplusplus
extern "C"
{
#endif

#define DEBUG_SCREEN_COLS 40 /* characters across the page */
#define DEBUG_SCREEN_ROWS 17 /* lines on it: the named ones, then the newest of the log */

void debug_screen_set(const char *key, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void debug_screen_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Removes a named line */
void debug_screen_clear(const char *key);

/* The page's lines as they are now (each a NUL-terminated string of at most
 * DEBUG_SCREEN_COLS characters); returns how many were put in `out` */
int debug_screen_lines(char out[DEBUG_SCREEN_ROWS][DEBUG_SCREEN_COLS + 1]);

#ifdef __cplusplus
}
#endif

#endif
