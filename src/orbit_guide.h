#ifndef ORBIT_GUIDE_H
#define ORBIT_GUIDE_H

/*
 * orbit_guide.h - the orbit autopilot's guidance (docs/orbit-autopilot-plan.md, step 4).
 *
 * Given the ship's position and velocity in its frame and the planned orbit (orbit_plan.h), it says what velocity the
 * ship should have now: the velocity of the planned orbit at the point of it nearest the ship, plus a pull towards
 * that point. Nothing depends on the time or on where along the orbit the ship is, so it holds at any time
 * acceleration and needs no burn scheduling: the plane, the apsides and the capture of a ship at rest all come out of
 * following that field with whatever thrust there is. It is done when the ship's own orbit is the planned one (plane,
 * periapsis, apoapsis and, for an ellipse, the argument of periapsis within the tolerances below), wherever the ship
 * is on it.
 *
 * Step 5 feeds the wanted velocity to the AI's thrust request; until then `fly` (script command) applies it as a
 * limited change of the ship's velocity, so the guidance can be run in the game's own physics.
 */
#include <stdbool.h>

#include "orbit_plan.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define GUIDE_PLANE_TOL_DEG 0.5  /* the planes this close */
#define GUIDE_APSIS_TOL     0.01 /* periapsis and apoapsis distances within this fraction of the planned ones */
#define GUIDE_ARGPE_TOL_DEG 5.0  /* an ellipse (e > 0.02): the periapsis direction this close */

typedef struct
{
	double nu_prev; /* the nearest point last time (true anomaly), kept so it does not jump between two */
	bool have_prev;
} OrbitGuide;

typedef struct
{
	double uw[3];      /* the velocity wanted now (pu / step) */
	double dist;       /* from the ship to the nearest point of the planned orbit (pu) */
	double nu;         /* that point's true anomaly */
	double plane_deg;  /* the ship's orbit against the plan: planes apart, ... */
	double d_rp, d_ra; /* ... periapsis and apoapsis distance differences (pu), ... */
	double d_argpe;    /* ... argument of periapsis (deg) */
	bool have_orbit;   /* the ship has a bound orbit to compare */
	bool done;
} GuideOut;

/* The slingshot into orbit: the ship, inbound, is turned so its conic has its periapsis at rp (pu from the centre) in the plan's
 * plane (the part of it through the ship's position) and sense; `uw` is the velocity that puts it on that conic (same speed),
 * `vr_frac` the radial part of its speed (-1 straight in .. 0 at the periapsis) */
typedef struct
{
	double uw[3], pe[3], e, turn_deg, vr_frac;
} OrbitFlyby;
bool orbit_flyby_step(const OrbitCtx *c, const OrbitPlan *pl, double rp, OrbitFlyby *out);
/* the speed at the ship's position for the planned apsides (the burn at the periapsis) */
double orbit_brake_speed(const OrbitCtx *c, const OrbitPlan *pl);

/* amax: the acceleration the ship can be counted on for (pu / step^2), which shapes how fast the pull is allowed to ask
 * the ship to move. Pure maths: no game memory. */
void orbit_guide_reset(OrbitGuide *g);
bool orbit_guide_step(const OrbitCtx *c, const OrbitPlan *pl, OrbitGuide *g, double amax, GuideOut *out);

/* Test driver (script commands `fly` and `flystop`, game thread): each host update the guidance runs and the ship's
 * velocity is changed towards the wanted one by at most `accel_ms2` metres a second squared over the game time since
 * the last update; a line is logged every `every` updates, and when it is done */
void orbit_fly_start(double accel_ms2, int every);
void orbit_fly_stop(void);
void orbit_fly_poll(void);

/* The orbit autopilot (the ORBIT page's ENGAGE; game thread). A request is taken at the next update. Engaged, the game's own
 * autopilot flies the ship to the planned orbit's body if it is not in its frame yet (the body must be the target and the
 * Automatic Pilot fitted), then the guidance flies it into the orbit, by changing its velocity (engines off) as the test driver
 * does, and it ends in orbit with the engines as they were. `orbit_ap_status` says what it is doing or why it stopped. */
void orbit_ap_request(bool on);
bool orbit_ap_active(void);
/* True while the autopilot is flying the ship itself (flyby, guidance): the game skips the player's controls then */
bool orbit_ap_controls_locked(void);
const char *orbit_ap_status(void);

#ifdef __cplusplus
}
#endif

#endif
