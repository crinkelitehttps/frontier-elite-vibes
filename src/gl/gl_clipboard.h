/*
 * gl_clipboard.h - a clipboard in the cockpit holding the controls and the
 * game manual (the pages: manual.h).
 *
 * HOW IT WORKS
 *   The board (42 x 34 cm, clipboard.h, from the F-19 port; spec:
 *   ~/projects/shared/specs/cockpit-clipboard.md) is stowed upright beside
 *   the left of the seat at hip height, where it can be read by looking
 *   down to the left. The clipboard key (F11) or a `manual` binding brings
 *   it up to a reading position in front of the panel (42 cm away, 9
 *   degrees down) and puts it away again.
 *
 *   In VR a hand at the board (aim point within 8 cm, the fingertip turns
 *   yellow) grabs it with a fresh grip press and carries it; let go, it
 *   stays where it is, fixed in the cockpit, or goes back in if let go
 *   within 15 cm of its stowed place. That grip is the board's until it is
 *   released: the bindings (the virtual pad) and the two-grip distance
 *   gesture don't see it. A hand at or holding the board doesn't touch the
 *   panel.
 *
 *   While it is out of its place (held, left somewhere, or reading) the
 *   `manual` binding layer and the page keys act, and pictures that are
 *   not the cockpit (menus, maps, other views, the VR virtual screen) show
 *   it flat over the picture. In the cockpit it is drawn after the panel
 *   without depth, then the hands.
 *
 * Board positions are in the cockpit's frame (view space, metres, -z
 * ahead); clipboard.h's are in cm with +z ahead.
 */
#ifndef GL_CLIPBOARD_H
#define GL_CLIPBOARD_H

#include <stdbool.h>

#include "gl_math.h"
#include "vr.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* Any thread: up to the reading position, or put away */
void clipboard_toggle(void);
/* Any thread: it is out of its place (pages turn, flat pictures show it) */
bool clipboard_out(void);

/* Drawing thread, per headset frame before the hands (gl_laser.c), with
 * the controllers as read (play space): grabs and carries the board.
 * `grab`: hands can grab it this frame (the cockpit, with hands). Clears
 * the squeeze of a grip the board has taken; board[i]: hand i is at the
 * board (1) or holds it (2), else 0. */
void clipboard_hands(VrHand hands[2], bool grab, int board[2]);

/* Queues the board in the cockpit, through `view_to_clip`, seen from
 * `eye` (view space) */
void clipboard_queue_cockpit(const mat4 *view_to_clip, const float eye[3]);
/* Queues it over a flat picture w x h (GD_VP_WINDOW), while it is out */
void clipboard_queue_flat(int w, int h);

/* With the GL context */
void clipboard_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* GL_CLIPBOARD_H */
