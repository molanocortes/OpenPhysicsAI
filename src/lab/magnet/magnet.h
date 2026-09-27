/* magnet.h - magnetic fields in the cross-section of electric machines, actuators and shields: two-dimensional
 * magnetostatics for the axial vector potential A (B = curl A, B_x = dA/dy, B_y = -dA/dx), so that the lines of constant
 * A are the lines of magnetic flux.
 *
 *   -div(nu grad A) = J + d(nu Br_y)/dx - d(nu Br_x)/dy,   nu = 1 / (mu0 mu_r)
 *
 * with J the current density along the axis (windings) and Br the remanence of permanent magnets (B = mu0 mu_r H + Br).
 * Finite volumes on square cells: the reluctivity at a face is the harmonic mean of its two cells (flux continuity at
 * material interfaces), the remanence term the flux of nu Br through the faces. The outer boundary holds A fixed: zero
 * (flux parallel to it) or an applied uniform field. Solved by conjugate gradients with an incomplete Cholesky
 * preconditioner. Linear materials; saturation of the iron is not modelled.
 *
 * The torque on a rotor comes from the Maxwell stress on a circle in the air gap, T = (1 / mu0) oint r^2 B_r B_theta
 * dtheta per unit length. Copper losses J^2 / sigma per cell are reported for a thermal model.
 *
 * Units: SI (T, A/m^2, m). */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../labio.h"

#define MG_MAX_MATERIALS 16
#define MG_MAX_REGIONS 256

enum { MG_BOX = 0, MG_CIRCLE = 1, MG_SECTOR = 2 };
enum { MG_MAG_NONE = 0, MG_MAG_PARALLEL = 1, MG_MAG_RADIAL = 2 };

typedef struct MgMaterial {
    char name[32];
    double mu_r;          /* relative permeability (the recoil permeability of a magnet) */
    double Br;            /* remanence, T; 0 for a material that is not a magnet */
    double conductivity;  /* S/m, for copper losses; 0 if none */
} MgMaterial;

typedef struct MgRegion {
    int shape;            /* MG_BOX, MG_CIRCLE, MG_SECTOR */
    double a[6];          /* box: x0 y0 x1 y1; circle: cx cy r; sector: cx cy r_in r_out angle0_deg angle1_deg */
    int material;
    double J;             /* current density along the axis, A/m^2 */
    int magnetisation;    /* MG_MAG_PARALLEL (direction angle_deg) or MG_MAG_RADIAL (outward if sign > 0) */
    double mag_angle_deg; /* parallel: the direction; radial: +1 outward, -1 inward */
    bool rotor;           /* turns with the rotor */
} MgRegion;

typedef struct MgSpec {
    int nx, ny;
    double dx;                /* m */
    int nmaterials;           /* materials[0] is air (mu_r 1) */
    MgMaterial materials[MG_MAX_MATERIALS];
    int nregions;
    MgRegion regions[MG_MAX_REGIONS]; /* painted in order, later over earlier */
    double applied_B[2];      /* uniform field imposed on the outer boundary, T (zero: flux parallel to it) */
    double rotor_centre[2], rotor_angle_deg;
    double gap_radius;        /* torque circle, m; <= 0: no torque */
    double tol;               /* relative residual, default 1e-9 */
    int threads;
} MgSpec;

typedef struct Magnet Magnet;

void mg_spec_defaults(MgSpec *s);
Magnet *mg_create(const MgSpec *s, char *err, size_t errlen);
void mg_free(Magnet *m);
/* solves for the current rotor angle; returns the iterations taken, or -1 if the solver did not converge */
int mg_solve(Magnet *m);
void mg_set_rotor_angle(Magnet *m, double deg); /* repaints the rotor's regions; solve again after */
void mg_set_region_current(Magnet *m, int region, double J); /* A/m^2; takes effect at the next repaint (the angle) */
const double *mg_potential(const Magnet *m);   /* A, Wb/m, per cell */
void mg_field(const Magnet *m, double x, double y, double B[2]); /* bilinear, T */
double mg_torque(const Magnet *m);   /* on the rotor, N m per metre of length, counter-clockwise positive */
double mg_copper_loss(const Magnet *m); /* W per metre of length */
int mg_material_at(const Magnet *m, int i, int j);

void mg_write_frame(Magnet *m, LabWriter *w);
char *mg_header_json(const MgSpec *s, const char *title);
