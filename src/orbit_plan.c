/*
 * orbit_plan.c - see orbit_plan.h. Step 1 of docs/orbit-autopilot-plan.md: the maths and a script command to try it.
 */
#include "orbit_plan.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cheats.h"
#include "game_state.h"
#include "host.h"

#define A6_BIG_SPACE  14466
#define OBJ_POS_LO    44
#define OBJ_POS_HI    56
#define OBJ_VELOCITY  98
#define OBJ_VEL_FRAC  192
#define OBJ_GRAV_MANT 122
#define OBJ_RADIUS_LO 266
#define OBJ_RADIUS_HI 270
#define OBJ_TILT      162
#define PU_KM         4294.967296
#define REF_DT        994.0 /* the step the elements are worked out for (pe, ap, e do not depend on it) */
#define CLOCK_HZ      49710.0 /* game clock units a second (orbit_probe.c's orbitlog) */
#define TWO_PI        6.283185307179586

static double dot(const double a[3], const double b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross(const double a[3], const double b[3], double out[3])
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static double norm(const double a[3])
{
	return sqrt(dot(a, a));
}

static double wrap_2pi(double x)
{
	x = fmod(x, TWO_PI);
	return x < 0 ? x + TWO_PI : x;
}

static double clamp1(double x)
{
	return x < -1 ? -1 : x > 1 ? 1 : x;
}

/* the in-plane basis for (inc, node): the pole n, the prime direction e1 and e2 = n x e1 */
static void ref_basis(const OrbitCtx *c, double e1[3], double e2[3], double n[3])
{
	for (int i = 0; i < 3; i++)
	{
		e1[i] = c->prime[i];
		n[i] = c->pole[i];
	}
	cross(n, e1, e2);
}

bool orbit_el_from_state(const OrbitCtx *c, const double r[3], const double u[3], OrbitEl *o)
{
	double h[3], e1[3], e2[3], n[3];
	cross(r, u, h);
	double hh = dot(h, h), rr = norm(r);
	if (!(hh > 0) || !(rr > 0) || !(c->mu > 0))
		return false;
	memset(o, 0, sizeof *o);
	double uh[3], ev[3], rhat[3], hhat[3];
	cross(u, h, uh);
	for (int i = 0; i < 3; i++)
	{
		rhat[i] = r[i] / rr;
		hhat[i] = h[i] / sqrt(hh);
		ev[i] = uh[i] / c->mu - rhat[i];
	}
	o->e = norm(ev);
	double energy = dot(u, u) / 2 - c->mu / rr;
	o->bound = energy < 0;
	o->p = hh / c->mu;
	o->rp = o->p / (1 + o->e);
	o->a = o->bound ? -c->mu / (2 * energy) : 0;
	o->ra = o->bound && o->e < 1 ? o->p / (1 - o->e) : 0;
	ref_basis(c, e1, e2, n);
	o->inc = acos(clamp1(dot(hhat, n)));
	double nv[3], nl;
	cross(n, hhat, nv);
	nl = norm(nv);
	if (nl < 1e-9)
	{
		/* in the reference plane: no node, longitude 0 */
		for (int i = 0; i < 3; i++)
			nv[i] = e1[i];
		nl = 1;
		o->node = 0;
	}
	else
	{
		for (int i = 0; i < 3; i++)
			nv[i] /= nl;
		o->node = wrap_2pi(atan2(dot(nv, e2), dot(nv, e1)));
	}
	double w[3];
	cross(hhat, nv, w);
	o->circular = o->e < 1e-6;
	for (int i = 0; i < 3; i++)
		o->pe[i] = o->circular ? rhat[i] : ev[i] / o->e;
	o->argpe = wrap_2pi(atan2(dot(o->pe, w), dot(o->pe, nv)));
	o->lat_arg = wrap_2pi(atan2(dot(rhat, w), dot(rhat, nv)));
	cross(hhat, o->pe, o->q);
	o->nu = atan2(dot(rhat, o->q), dot(rhat, o->pe));
	memcpy(o->h, hhat, sizeof o->h);
	return true;
}

/* the speed in km/s of a velocity in pu per step */
static double kms(double u)
{
	return u * PU_KM * CLOCK_HZ / REF_DT;
}

bool orbit_plan_resolve(const OrbitCtx *c, const OrbitSpec *s, OrbitPlan *pl)
{
	memset(pl, 0, sizeof *pl);
	if (!c->ok || !(c->mu > 0) || !(s->hp_km == s->hp_km) || !(s->ha_km == s->ha_km))
		return false;
	double rp = c->radius + s->hp_km / PU_KM, ra = c->radius + s->ha_km / PU_KM;
	if (rp > ra)
	{
		double t = rp;
		rp = ra;
		ra = t;
	}
	if (!(rp > 0))
		return false;
	OrbitEl se;
	bool ship = c->has_ship && orbit_el_from_state(c, c->r, c->u, &se);
	OrbitEl *o = &pl->el;
	pl->spec = *s;
	pl->body_slot = c->body_slot;
	pl->regime = c->regime;
	memcpy(pl->body_name, c->name, sizeof pl->body_name);
	pl->radius = c->radius;
	o->bound = true;
	o->rp = rp;
	o->ra = ra;
	o->a = (rp + ra) / 2;
	o->e = (ra - rp) / (ra + rp);
	o->p = o->a * (1 - o->e * o->e);
	o->circular = o->e < 1e-6;
	/* the plane: what is not asked for stays as the ship has it, so the plane change is the least there is and falls
	 * at the node the ship reaches first (guidance, later) */
	pl->auto_inc = !s->has_inc;
	pl->auto_node = !s->has_node;
	pl->auto_argpe = !s->has_argpe;
	double inc = s->has_inc ? fmin(fmax(s->inc_deg, 0), 180) * M_PI / 180 : ship ? se.inc : 0;
	double node = s->has_node ? wrap_2pi(s->node_deg * M_PI / 180) : ship ? se.node : 0;
	double e1[3], e2[3], n[3], hv[3], nv[3], w[3];
	ref_basis(c, e1, e2, n);
	for (int i = 0; i < 3; i++)
	{
		nv[i] = cos(node) * e1[i] + sin(node) * e2[i];
		hv[i] = cos(inc) * n[i] + sin(inc) * (sin(node) * e1[i] - cos(node) * e2[i]);
	}
	cross(hv, nv, w);
	/* the argument of periapsis: kept if the ship's orbit has a periapsis, else the periapsis lies under the ship
	 * (its argument of latitude in the new plane, the ship's position taken into it) */
	double argpe = 0;
	if (s->has_argpe)
		argpe = wrap_2pi(s->argpe_deg * M_PI / 180);
	else if (ship)
	{
		if (!se.circular && se.e > 1e-3)
			argpe = se.argpe;
		else
		{
			double rl = norm(c->r), rh[3] = {c->r[0] / rl, c->r[1] / rl, c->r[2] / rl};
			argpe = wrap_2pi(atan2(dot(rh, w), dot(rh, nv)));
		}
	}
	for (int i = 0; i < 3; i++)
		o->pe[i] = cos(argpe) * nv[i] + sin(argpe) * w[i];
	cross(hv, o->pe, o->q);
	memcpy(o->h, hv, sizeof o->h);
	o->inc = inc;
	o->node = node;
	o->argpe = argpe;
	pl->period_s = TWO_PI * sqrt(o->a * o->a * o->a / c->mu) * REF_DT / CLOCK_HZ;
	pl->vp_kms = kms(sqrt(c->mu * (2 / rp - 1 / o->a)));
	pl->va_kms = kms(sqrt(c->mu * (2 / ra - 1 / o->a)));
	pl->valid = true;
	return true;
}

void orbit_plan_state_at(const OrbitCtx *c, const OrbitPlan *pl, double nu, double r[3], double u[3])
{
	const OrbitEl *o = &pl->el;
	double rad = o->p / (1 + o->e * cos(nu)), vs = sqrt(c->mu / o->p);
	for (int i = 0; i < 3; i++)
	{
		r[i] = rad * (cos(nu) * o->pe[i] + sin(nu) * o->q[i]);
		u[i] = vs * (-sin(nu) * o->pe[i] + (o->e + cos(nu)) * o->q[i]);
	}
}

void orbit_plan_errors(const OrbitPlan *pl, const OrbitEl *ship, OrbitErr *out)
{
	out->plane_deg = acos(clamp1(dot(pl->el.h, ship->h))) * 180 / M_PI;
	out->d_rp = pl->el.rp - ship->rp;
	out->d_ra = ship->bound ? pl->el.ra - ship->ra : 0;
}

void orbit_plan_conic(const OrbitPlan *pl, double radius, ConicElems *out)
{
	memset(out, 0, sizeof *out);
	memcpy(out->pe, pl->el.pe, sizeof out->pe);
	memcpy(out->q, pl->el.q, sizeof out->q);
	out->p = pl->el.p;
	out->e = pl->el.e;
	out->rp = pl->el.rp;
	out->radius = radius;
	out->lim = M_PI;
	out->closed = true;
}

/* ---- the game ---- */

static unsigned int slot_object(int i)
{
	return (unsigned int)rdlong(GAME_A6 + A6_BIG_SPACE) + (unsigned int)(int)rdword(FE2_obj_offsets + 2 * (unsigned int)i);
}

/* what a body of the system gives: its gravity, radius, name and matrix (own axes -> system axes) */
static bool fill_body(OrbitCtx *c, int slot)
{
	unsigned int po = slot_object(slot);
	int mant = (short)rdword(po + OBJ_GRAV_MANT), exp = (short)rdword(po + OBJ_GRAV_MANT + 2);
	c->radius = (double)rdword(po + OBJ_RADIUS_HI) + (double)(unsigned int)rdlong(po + OBJ_RADIUS_LO) / 4294967296.0;
	if (mant <= 0 || !(c->radius > 0))
		return false;
	double k = (REF_DT / 128.0) / 17179869184.0;
	c->k = k;
	c->mu = (double)mant * ldexp(1.0, exp - 51) * REF_DT * k;
	double col[3][3];
	cheat_object_axes(po, col);
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			c->m[j][i] = col[i][j]; /* column i: right, up, forward */
	c->tilt_deg = acos(clamp1(col[1][1])) * 180 / M_PI; /* (the matrix's own pole) */
	c->tilt_rec_deg = (double)(unsigned short)rdword(po + OBJ_TILT) * 360.0 / 65536.0;
	c->body_slot = slot;
	for (int i = 0; i < 23; i++)
		if (!(c->name[i] = (char)rdbyte(po + 246 + (unsigned)i)))
			break;
	return true;
}

/* the pole and longitude 0 in the coordinates of a regime */
static void fill_basis(OrbitCtx *c, int regime)
{
	c->regime = regime;
	for (int i = 0; i < 3; i++)
	{
		c->pole[i] = regime == 255 ? (i == 1) : c->m[i][1];
		c->prime[i] = regime == 255 ? (i == 2) : c->m[i][2];
	}
}

bool orbit_ctx_get(OrbitCtx *c)
{
	memset(c, 0, sizeof *c);
	unsigned int ship = game_player_ship();
	if (!ship)
		return false;
	int par = (unsigned char)rdbyte(ship + 68);
	if (!par || !fill_body(c, par))
		return false;
	fill_basis(c, (signed char)rdbyte(ship + 69) == -1 ? 255 : 0);
	double k = c->k;
	for (int i = 0; i < 3; i++)
	{
		c->r[i] = (double)(int)rdlong(ship + OBJ_POS_HI + 4 * i) +
				  (double)(unsigned int)rdlong(ship + OBJ_POS_LO + 4 * i) / 4294967296.0;
		c->u[i] = k * ((double)(int)rdlong(ship + OBJ_VELOCITY + 4 * i) +
					   (double)(rdword(ship + OBJ_VEL_FRAC + 2 * i) & 0xffff) / 65536.0);
	}
	c->has_ship = true;
	c->ok = true;
	return true;
}

/* the body of `slot` on its own, in the system's axes */
bool orbit_ctx_body(int slot, OrbitCtx *c)
{
	unsigned int ship = game_player_ship();
	if (ship && slot == (unsigned char)rdbyte(ship + 68))
		return orbit_ctx_get(c);
	memset(c, 0, sizeof *c);
	if (slot <= 0 || !fill_body(c, slot))
		return false;
	fill_basis(c, 0);
	c->ok = true;
	return true;
}

int orbit_body_pick(void)
{
	unsigned int ship = game_player_ship();
	if (!ship)
		return 0;
	int target = cheat_target();
	OrbitCtx c;
	if (target > 0 && target != (unsigned char)rdbyte(ship + 93) && fill_body(&c, target))
		return target;
	int par = (unsigned char)rdbyte(ship + 68);
	return par && fill_body(&c, par) ? par : 0;
}

/* ---- the script command ---- */

static OrbitPlan current;

static void mul_vec(const double R[3][3], double v[3])
{
	double o[3];
	for (int i = 0; i < 3; i++)
		o[i] = R[i][0] * v[0] + R[i][1] * v[1] + R[i][2] * v[2];
	for (int i = 0; i < 3; i++)
		v[i] = o[i];
}

void orbit_plan_rotate(const OrbitCtx *c, OrbitPlan *pl, const double R[3][3])
{
	OrbitEl *o = &pl->el;
	mul_vec(R, o->h);
	mul_vec(R, o->pe);
	mul_vec(R, o->q);
	/* the angles again, for the basis of the new coordinates */
	double e1[3], e2[3], n[3], nv[3], w[3];
	ref_basis(c, e1, e2, n);
	o->inc = acos(clamp1(dot(o->h, n)));
	cross(n, o->h, nv);
	double nl = norm(nv);
	if (nl < 1e-9)
		memcpy(nv, e1, sizeof nv), o->node = 0;
	else
	{
		for (int i = 0; i < 3; i++)
			nv[i] /= nl;
		o->node = wrap_2pi(atan2(dot(nv, e2), dot(nv, e1)));
	}
	cross(o->h, nv, w);
	o->argpe = wrap_2pi(atan2(dot(o->pe, w), dot(o->pe, nv)));
}

const OrbitPlan *orbit_plan_current(void)
{
	return &current;
}

bool orbit_plan_set(const OrbitSpec *s)
{
	OrbitCtx c;
	OrbitPlan pl;
	if (!orbit_ctx_body(orbit_body_pick(), &c) || !orbit_plan_resolve(&c, s, &pl))
	{
		current.valid = false;
		return false;
	}
	current = pl;
	return true;
}

void orbit_plan_rotate_current(const OrbitCtx *c, const double R[3][3])
{
	if (current.valid)
		orbit_plan_rotate(c, &current, R);
}

void orbit_plan_sync(const OrbitCtx *c)
{
	if (!current.valid || !c->ok || current.body_slot != c->body_slot || current.regime == c->regime)
		return;
	double R[3][3];
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			R[i][j] = c->regime == 0 ? c->m[i][j] : c->m[j][i]; /* own axes -> system's is m, back is its transpose */
	orbit_plan_rotate(c, &current, R);
	current.regime = c->regime;
}

void orbit_plan_clear(void)
{
	current.valid = false;
}

static double deg(double rad)
{
	return rad * 180 / M_PI;
}

static void print_el(const char *what, const OrbitCtx *c, const OrbitEl *o, bool lat_arg)
{
	char lat[32] = "";
	if (lat_arg)
		snprintf(lat, sizeof lat, " (lat arg %.3f)", deg(o->lat_arg));
	printf("script: plan: %s: pe %.3f km ap %.3f km e %.6f inc %.3f node %.3f argpe %.3f deg%s%s\n", what,
		(o->rp - c->radius) * PU_KM, o->bound ? (o->ra - c->radius) * PU_KM : -1.0, o->e, deg(o->inc), deg(o->node),
		deg(o->argpe), lat, o->circular ? " circular" : "");
}

/* does the state at some true anomalies give back the elements the plan was made of? */
static void self_check(const OrbitCtx *c, const OrbitPlan *pl)
{
	static const double nus[] = {0, 1.2, 2.5, 3.9, 5.5};
	double worst = 0;
	for (unsigned i = 0; i < sizeof nus / sizeof *nus; i++)
	{
		double r[3], u[3];
		OrbitEl e;
		orbit_plan_state_at(c, pl, nus[i], r, u);
		if (!orbit_el_from_state(c, r, u, &e))
		{
			printf("script: plan: check: no elements at nu %.1f\n", nus[i]);
			worst = 1e9;
			continue;
		}
		double d[5];
		d[0] = fabs(e.rp - pl->el.rp) / pl->el.rp;
		d[1] = fabs(e.ra - pl->el.ra) / pl->el.ra;
		d[2] = fabs(e.inc - pl->el.inc);
		bool flat = pl->el.inc < 1e-6 || pl->el.inc > M_PI - 1e-6;
		d[3] = flat ? 0 : fabs(remainder(e.node - pl->el.node, TWO_PI));
		d[4] = pl->el.circular ? 0 : fabs(remainder(e.argpe - pl->el.argpe, TWO_PI));
		for (int j = 0; j < 5; j++)
			worst = fmax(worst, d[j]);
	}
	printf("script: plan: check: elements from states round the orbit, worst difference %.2e (%s)\n", worst,
		worst < 1e-6 ? "ok" : "BAD");
}

void orbit_plan_script(const char *args)
{
	OrbitCtx c;
	if (!orbit_ctx_get(&c))
	{
		printf("script: plan: no ship, or no gravity body here\n");
		return;
	}
	printf("script: plan: body slot %d radius %.3f km mu %.6g pole %.4f %.4f %.4f (%.2f deg from y, record tilt %.2f) "
		   "prime %.6f %.6f %.6f\n",
		c.body_slot, c.radius * PU_KM, c.mu, c.pole[0], c.pole[1], c.pole[2], c.tilt_deg, c.tilt_rec_deg, c.prime[0],
		c.prime[1], c.prime[2]);
	OrbitEl se;
	bool ship = orbit_el_from_state(&c, c.r, c.u, &se);
	printf("script: plan: ship frame byte +69 = %d\n", (int)(signed char)rdbyte(game_player_ship() + 69));
	if (ship)
		print_el("ship", &c, &se, true);
	else
		printf("script: plan: ship at rest, no orbit\n");
	char buf[128], *tok[5];
	int n = 0;
	snprintf(buf, sizeof buf, "%s", args);
	for (char *t = strtok(buf, " \t"); t && n < 5; t = strtok(NULL, " \t"))
		tok[n++] = t;
	if (n < 2)
	{
		/* no numbers: how far the ship is from the plan (fixed in the frame) */
		if (current.valid && ship)
		{
			OrbitErr er;
			orbit_plan_errors(&current, &se, &er);
			printf("script: plan: from the ship: planes %.3f deg apart, periapsis %+.3f km, apoapsis %+.3f km\n", er.plane_deg,
				er.d_rp * PU_KM, er.d_ra * PU_KM);
		}
		return;
	}
	OrbitSpec s;
	memset(&s, 0, sizeof s);
	s.hp_km = atof(tok[0]);
	s.ha_km = atof(tok[1]);
	if (n > 2 && strcmp(tok[2], "-"))
		s.has_inc = true, s.inc_deg = atof(tok[2]);
	if (n > 3 && strcmp(tok[3], "-"))
		s.has_node = true, s.node_deg = atof(tok[3]);
	if (n > 4 && strcmp(tok[4], "-"))
		s.has_argpe = true, s.argpe_deg = atof(tok[4]);
	OrbitPlan pl;
	if (!orbit_plan_resolve(&c, &s, &pl))
	{
		printf("script: plan: cannot make that orbit (periapsis below the centre?)\n");
		return;
	}
	current = pl;
	print_el("plan", &c, &pl.el, false);
	printf("script: plan: auto:%s%s%s  period %.1f s  Vp %.4f km/s  Va %.4f km/s\n", pl.auto_inc ? " inc" : "",
		pl.auto_node ? " node" : "", pl.auto_argpe ? " argpe" : "", pl.period_s, pl.vp_kms, pl.va_kms);
	self_check(&c, &pl);
	if (ship)
	{
		OrbitErr er;
		orbit_plan_errors(&pl, &se, &er);
		printf("script: plan: from the ship: planes %.3f deg apart, periapsis %+.3f km, apoapsis %+.3f km\n", er.plane_deg,
			er.d_rp * PU_KM, er.d_ra * PU_KM);
	}
	fflush(stdout);
}
