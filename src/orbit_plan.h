#ifndef ORBIT_PLAN_H
#define ORBIT_PLAN_H

/*
 * orbit_plan.h - the target orbit of the orbit autopilot (docs/orbit-autopilot-plan.md, step 1).
 *
 * Maths only: the wanted elements (what the player types) are turned into an orbit in the axes of the ship's
 * frame (the body it is relative to), the ship's own orbit is turned into the same elements, and the two can be
 * compared. Lengths are position units (2^32 to the game's altitude unit, PU_KM km) and velocities position units
 * per integrator step, as orbit_probe.c and gl_hud.c have them; the user-facing values are km and degrees.
 *
 * The frame is inertial: the ship's position and velocity do not turn with the body, so a plan's vectors (orbit normal,
 * periapsis direction) stay where they are. What turns is the body's own matrix, in steps of 360/4096 degrees at the
 * body's spin (24 h / 2^(spin-1) a turn): its "forward" column, which is longitude 0 here, so a longitude measured
 * from it (the node of a ship's orbit, below) creeps by the spin although the orbit is fixed. A node the player types
 * is taken from longitude 0 as it is when the plan is made, and is stored as vectors, so the plan stays put.
 *
 * The reference plane is the body's equator: its pole is the "up" column of the body object's matrix (the tilt only,
 * measured on Earth: 22.95 degrees from the frame's y for a record tilt of 23.0) and node longitudes are measured
 * from the body's "forward" column (the direction of longitude 0 at spin 0), counter-clockwise about the pole.
 */
#include <stdbool.h>

#include "gl/gl_hud.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* What the player sets. Apsides are altitudes above the surface in km, angles in degrees; a value that has
 * `has_*` clear is left to the program ("auto") */
typedef struct
{
	double hp_km, ha_km;
	bool has_inc, has_node, has_argpe;
	double inc_deg, node_deg, argpe_deg;
	bool orbital_plane; /* reference plane: the body's orbit round its parent, not its equator (not done yet) */
} OrbitSpec;

/* Everything the maths needs to know of the game: the body the ship is relative to and the ship, in the frame's axes */
typedef struct
{
	bool ok, has_ship;
	int body_slot;
	char name[24];            /* the body's name */
	double mu;                /* gravitational parameter, in pu^3 / step^2 */
	double radius;            /* the body's, pu */
	double pole[3], prime[3]; /* unit vectors: the pole, and the direction of longitude 0 (in the equator) */
	double tilt_deg;          /* angle of the pole from the frame's y */
	double tilt_rec_deg;      /* the body record's axial tilt (object word 162) */
	double r[3], u[3];        /* the ship: position from the body's centre (pu) and velocity (pu / step) */
	double k;                 /* the ship's velocity words (+98, +192) times this are u */
	int regime;               /* the ship's byte +69: 255 = the frame is the body's own axes (the pole is y, longitude 0 is z),
	                           * else the system's axes, where the body's matrix says where its pole is. The game
	                           * turns the ship's position and velocity by the matrix when it changes. */
	double m[3][3];           /* the body's matrix columns (right, up, forward): own axes -> system axes */
} OrbitCtx;

/* Elements of an orbit in the reference frame (pole, prime) */
typedef struct
{
	bool bound;
	double rp, ra, a, e, p;          /* pu; ra = 0 for an unbound orbit */
	double inc, node, argpe;         /* radians; inc 0..pi, node and argpe 0..2pi */
	double h[3], pe[3], q[3];        /* orbit normal, periapsis direction, in-plane direction 90 degrees on (unit) */
	double lat_arg;                  /* the ship's argument of latitude (from the node, along the motion), radians */
	double nu;                       /* the ship's true anomaly, radians */
	bool circular;                   /* e too small for the periapsis direction to mean anything */
} OrbitEl;

/* The orbit asked for, resolved: the optional elements the player left out are filled in, `auto_*` marks them */
typedef struct
{
	bool valid;
	OrbitSpec spec;
	int body_slot;        /* the body it is round (an object slot) */
	char body_name[24];
	int regime;           /* the coordinates it is in (OrbitCtx::regime); orbit_plan_sync turns it with the ship's */
	double radius;        /* that body's radius, pu */
	OrbitEl el;           /* fixed in the frame; `node` is from longitude 0 as it was when the plan was made */
	bool auto_inc, auto_node, auto_argpe;
	double period_s;  /* game seconds at 1x */
	double vp_kms, va_kms;
} OrbitPlan;

/* Pure maths (no game memory) */
bool orbit_el_from_state(const OrbitCtx *c, const double r[3], const double u[3], OrbitEl *out);
bool orbit_plan_resolve(const OrbitCtx *c, const OrbitSpec *s, OrbitPlan *out);
/* The position and velocity on the planned orbit at true anomaly nu (radians) */
void orbit_plan_state_at(const OrbitCtx *c, const OrbitPlan *pl, double nu, double r[3], double u[3]);
/* How far the ship's orbit is from the plan: the angle between the planes, and the differences in periapsis and
 * apoapsis altitude (pu) */
typedef struct
{
	double plane_deg, d_rp, d_ra;
} OrbitErr;
void orbit_plan_errors(const OrbitPlan *pl, const OrbitEl *ship, OrbitErr *out);
/* The plan as the conic the system map draws (gl_hud.h) */
void orbit_plan_conic(const OrbitPlan *pl, double radius, ConicElems *out);

/* The plan, and so its vectors, turned by R (a rotation, row-major, applied to column vectors): for when the game turns the
 * ship's coordinates. The angles are worked out again for the basis in `c` */
void orbit_plan_rotate(const OrbitCtx *c, OrbitPlan *pl, const double R[3][3]);
void orbit_plan_rotate_current(const OrbitCtx *c, const double R[3][3]);

/* Game thread */
bool orbit_ctx_get(OrbitCtx *c);

/* Script commands (script.c): `plan` with no arguments describes the context, `plan HP HA [INC NODE ARGPE]` sets the
 * planned orbit ("-" leaves a value to the program) and checks it. Output goes to stdout as `script: ...` lines */
void orbit_plan_script(const char *args);

/* The current planned orbit (set by the script command and the ORBIT page) */
const OrbitPlan *orbit_plan_current(void);
/* Game thread: the plan for what is asked, round the body the ship is relative to, fixed from now (what is left to the
 * program is taken from the ship as it is now). False (and no plan) if there is no such orbit */
bool orbit_plan_set(const OrbitSpec *s);
void orbit_plan_clear(void);
/* The body an orbit is wanted round: the selected target if it can be orbited (a star, planet or moon: it has gravity),
 * else the body the ship is relative to; 0 if neither. Its context: the ship's too when it is the ship's frame body,
 * else just the body, in the system's axes (regime 0) */
int orbit_body_pick(void);
bool orbit_ctx_body(int slot, OrbitCtx *c);
/* Turns the plan if it is round the ship's frame body and the ship's regime is not the plan's */
void orbit_plan_sync(const OrbitCtx *c);

#ifdef __cplusplus
}
#endif

#endif
