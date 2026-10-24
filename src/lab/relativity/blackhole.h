/* blackhole.h - light near a black hole: null geodesics of the Schwarzschild metric, traced from a camera to where each
 * ray ends: on the sky far away (bent, so the stars behind are displaced and doubled), inside the horizon (the shadow),
 * or on a thin accretion disk in the equatorial plane (seen above and, through the bending, below the hole).
 *
 * Units: G = c = 1 and lengths in the hole's mass M (the horizon at r = 2, the photon sphere at r = 3, the innermost
 * stable circular orbit at r = 6).
 *
 * Geodesics: every photon moves in a plane through the centre; with u = 1/r and phi the angle in that plane,
 *   d2u/dphi2 + u = 3 u^2,
 * with the first integral (du/dphi)^2 + u^2 - 2 u^3 = 1 / b^2 (b the impact parameter), integrated by fourth-order
 * Runge-Kutta with a step that shrinks near the hole.
 *
 * The disk: thin and Keplerian (angular velocity r^(-3/2)) from r_in to r_out, its temperature T(r) proportional to
 * r^(-3/4) (1 - sqrt(r_in / r))^(1/4) (the Shakura-Sunyaev profile with a zero-torque inner edge), shifted by the
 * factor g = E_observed / E_emitted that combines gravitational redshift and the Doppler shift of the orbit, and
 * brightened as g^4 (bolometric, from the invariance of I / nu^3). The colour of a blackbody at the shifted temperature. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../labio.h"

typedef struct BhSpec {
    double cam_r;          /* camera distance, M */
    double incl_deg;       /* the camera's angle from the disk's axis (90: edge on) */
    double azim_deg;       /* the camera's angle around the axis */
    double fov_deg;        /* vertical field of view */
    int w, h;              /* pixels */
    bool disk;
    double r_in, r_out;    /* M, the disk's edges (r_in 6: the innermost stable orbit) */
    double t_in_k;         /* the disk's temperature scale, K (the shifted colours depend on it) */
    bool stars;            /* a procedural star field and a galaxy's band on the sky (a picture, not a catalogue) */
    int aa;                /* samples per pixel along each axis (2: four rays per pixel) */
    double time_M;         /* time in M: the disk's texture turns at the Keplerian rate */
    double exposure_ref;   /* the light that maps to white; 0: from the picture itself */
} BhSpec;

void bh_spec_defaults(BhSpec *s);

/* the fate of one ray leaving the camera: returns 0 escaped (dir: its direction far away, unit vector),
 * 1 captured by the hole, 2 hit the disk (r_hit, and g the frequency shift, and whether it crossed the plane from above) */
typedef struct BhHit {
    int fate;
    double dir[3];
    double r_hit, g, phi_hit; /* phi_hit: the disk's azimuth where the ray met it */
    int crossings;         /* how many times the ray crossed the equatorial plane before its end */
} BhHit;
void bh_trace(const BhSpec *s, const double cam[3], const double ray[3], BhHit *out);

/* the whole picture: rgb, 3 floats per pixel, tone mapped to 0..1, row by row from the top; returns the light that was
 * mapped to white (pass it back as exposure_ref to keep a film's exposure steady) */
double bh_render(const BhSpec *s, float *rgb);

/* for the verification: the total bending of a ray of impact parameter b coming from infinity (radians, pi excluded),
 * and whether a ray of impact parameter b is captured */
double bh_deflection(double b, double *first_integral_drift);
bool bh_captured(double b);

void bh_write_frame(const BhSpec *s, const float *rgb, LabWriter *w);
char *bh_header_json(const BhSpec *s, const char *title);
