/* orbit.h - gravitating bodies: planets, moons, asteroids, spacecraft; Newtonian N-body with the first post-Newtonian
 * correction of general relativity around the central body.
 *
 * Integrator: Gauss-Legendre implicit Runge-Kutta with s stages (order 2s, symplectic and symmetric: energy errors stay
 * bounded over long runs instead of growing). The nodes are the roots of the Legendre polynomial and the coefficients
 * the integrals of the Lagrange polynomials on them, both computed at start-up in double precision rather than typed
 * in. The stage equations are solved by fixed-point iteration to round-off; the solution is accumulated with
 * compensated summation. Fixed step (symplectic) or adaptive by step doubling (for close encounters).
 *
 * Relativity: for every body i other than body 0 (the Sun), the acceleration relative to body 0 gains
 *   a = GM0 / (c^2 r^3) [ (4 GM0 / r - v^2) r + 4 (r . v) v ]
 * (Anderson, Esposito, Martin, Thornton and Muhleman, Astrophys. J. 200, 1975, 221), which gives Mercury's perihelion
 * its 43 arcseconds a century.
 *
 * Units: SI (m, s, m^3/s^2 for GM). */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../labio.h"

#define ORB_MAX_BODIES 64

typedef struct OrbBody {
    char name[32];
    double gm;     /* m^3 / s^2; 0 for a test particle */
    double radius; /* m, for drawing and for impact detection; 0 unknown */
    double x[3], v[3];
} OrbBody;

typedef struct OrbSpec {
    int n;
    OrbBody bodies[ORB_MAX_BODIES];
    int stages;         /* Gauss-Legendre stages, 1..10, default 6 (order 12) */
    double dt;          /* s; the fixed step, or the first step when adaptive */
    bool adaptive;      /* step doubling with a relative position tolerance */
    double tolerance;   /* default 1e-13 */
    bool relativity;    /* 1PN correction around body 0 */
} OrbSpec;

typedef struct Orbit Orbit;

void orb_spec_defaults(OrbSpec *s);
Orbit *orb_create(const OrbSpec *s, char *err, size_t errlen);
void orb_free(Orbit *o);
/* advance to time t (s after the start), in steps of the fixed or adaptive size; the last step is shortened */
void orb_advance(Orbit *o, double t);
double orb_time(const Orbit *o);
long orb_steps(const Orbit *o);
const OrbBody *orb_body(const Orbit *o, int i); /* its current state */
/* total energy (J / G-units: the sum of 1/2 v^2 GM / G is not formed; this is energy per unit G, m^5/s^4 ... ) */
double orb_energy(const Orbit *o);       /* sum 1/2 gm_i v_i^2 - sum gm_i gm_j / r_ij  (energy times G) */
void orb_momentum(const Orbit *o, double p[3], double L[3]); /* times G */
/* the closest distance between bodies a and b so far, and when (s) */
double orb_closest(const Orbit *o, int a, int b, double *when);
void orb_track(Orbit *o, int a, int b); /* start tracking the closest approach of a and b (one pair) */

/* a frame: part "bodies" (points, fields gm and radius) and part "paths" (each body's track as lines, when kept) */
void orb_write_frame(Orbit *o, LabWriter *w, bool paths);
/* draw in the frame of body `origin` (its position subtracted from every position and every path point as it is
 * recorded); -1 for the barycentric frame */
void orb_set_frame_origin(Orbit *o, int origin);
char *orb_header_json(const OrbSpec *s, const char *title);
