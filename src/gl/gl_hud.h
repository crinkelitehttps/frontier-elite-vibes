/*
 * gl_hud.h - our own crosshair, velocity (trajectory) marker, target
 * tunnel and range, and labels on things in the flight view, in place of
 * the game's.
 *
 * HOW IT WORKS
 *   The game draws both into its 2D screen (L63574_mod3_fn20_DrawPlayerHUD):
 *   the crosshair (L63cca) when the view has a gun, the velocity marker
 *   (L63cf2_DrawPlayerVelocityVector) in the front view. Hooks in fe2.s ask
 *   the host first (Call_HudCross, Call_HudVelocity); with own_hud on they
 *   say skip and the host notes what to draw instead.
 *
 *   The game's marker is approximate: the velocity is cut to 16 bits,
 *   turned into the ship's axes, then 255 is added ahead (about 24 m/s,
 *   against a division by zero) before it is projected to whole pixels.
 *   Slow, as when docking or landing, that bias wins: sliding sideways at
 *   20 m/s shows about 40 degrees off the true direction, and backing off at
 *   up to about 18 m/s shows a marker ahead, as if going forwards. Ours
 *   turns the full 32 bit velocity with floats, has no bias, and marks
 *   the reverse direction (retrograde) as well; nothing under 1 m/s.
 *
 *   The target and the labels (L63990, and L6393c for each thing on the
 *   game's screen) likewise: Call_HudTarget gets the target with its range
 *   formatted by the game and whether it has a tunnel, Call_HudLabels says
 *   skip and the host goes through the things itself, all of them, not
 *   only those on the game's screen. Where each is comes from the native
 *   scene (native_object_pos); without it, or when it has not got the
 *   thing, the game draws its own. The text is the game's font, a quad a
 *   run of pixels.
 *
 *   All are drawn as directions, on the world's projection: in the
 *   classic view they sit where the game's would (sharp, at any window
 *   size); in the cockpit they are at infinity like the world, the
 *   crosshair along the ship's nose, the velocity markers, the target and
 *   the labels wherever they are, also outside the classic view (look
 *   round for them). The tunnel's squares go from the target towards the
 *   middle of the view, as the game's do, along the sphere.
 *
 *   The camera screen (gl_camera.h) is the game's own picture: there the
 *   game draws its own target and labels.
 */
#ifndef GL_HUD_H
#define GL_HUD_H

#include <stdbool.h>

#include "gl_math.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* Setting: our crosshair, velocity marker, target and labels instead of
 * the game's */
extern bool own_hud;

/* The orbit display: the conic and Ap / Pe / time to the next apsis (R3) */
extern bool orbit_display;

/* The orbit's conic round the ship's parent in the parent's frame (position
 * units, 2^32 to the game's altitude unit): points are
 * p / (1 + e cos nu) * (cos nu * pe + sin nu * q), the ship at nu0 */
typedef struct
{
	double pe[3], q[3]; /* periapsis direction; in the plane, 90 degrees on, along the motion */
	double p, e, rp;    /* semi-latus rectum, eccentricity, periapsis distance */
	double radius;      /* the body's */
	double lim;         /* |nu| out to the asymptotes (an escape orbit) */
	bool closed;
} ConicElems;

/* Game thread: the orbit now, false when the display is off or there is none */
bool hud_conic_elements(ConicElems *out);

/* Host calls from the hooks (game thread): d0 = 1 to skip the game's */
void Call_HudCross(void);
void Call_HudVelocity(void);
void Call_HudTarget(void);
void Call_HudLabels(void);

/* Game thread, under the frame lock: what the hooks noted this frame
 * becomes the latest; for the cockpit's screen's frames (no HUD drawn by
 * the game) the last flight frame's crosshair and the velocity now */
void hud_publish(bool screen);
/* Game thread, at the end of the frame's work (after any world capture,
 * whose hooks must not count) */
void hud_frame_end(void);
/* Whoever draws, under the frame lock: takes the latest */
void hud_acquire(void);

/* Queues the markers (GD_VP_VIEW3D): `dir_proj` takes view space
 * directions (x right, y up, -z ahead, any length) to clip space */
void hud_draw(const mat4 *dir_proj);

#ifdef __cplusplus
}
#endif

#endif /* GL_HUD_H */
