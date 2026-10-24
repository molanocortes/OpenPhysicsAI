/* acoustic.h - sound in rooms and around objects: the linear wave equation for the acoustic pressure on a 3D grid.
 *
 *   d2p/dt2 = c^2 lap(p)
 *
 * Scheme: the standard rectilinear finite-difference time-domain scheme (seven-point Laplacian, leapfrog in time) at
 * a Courant number lambda = c dt / dx of 1/sqrt(3), its stability limit, where it is most accurate along the axes
 * (Kowalczyk and van Walstijn, IEEE Trans. Audio Speech Lang. Process. 19, 2011, compare its variants). About ten
 * cells per wavelength keep the phase error below one per cent along the axes; the grid dispersion is largest along
 * the diagonals.
 *
 * Walls: every surface is locally reacting with a frequency-independent normalised impedance xi = Z / (rho c), from the
 * boundary condition dp/dn = -(1 / (c xi)) dp/dt, discretised centrally at the boundary node. With m missing
 * neighbours (1 on a face, 2 on an edge, 3 in a corner) the update is
 *   p+ (1 + m lambda / xi) = 2 p - p- (1 - m lambda / xi) + lambda^2 sum over the six directions (p_i - p),
 * a missing neighbour replaced by its mirror. xi = infinity is rigid; the reflection coefficient at normal incidence
 * is R = (xi - 1) / (xi + 1), the absorption 1 - R^2. The domain's outer faces are walls like any other; xi = 1 there
 * absorbs a wave at normal incidence (a first-order open boundary, not a perfectly matched layer).
 *
 * Units: SI. Pressures are relative to the static pressure, Pa. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../labio.h"

#define AC_MAX_MATERIALS 16
#define AC_MAX_SOURCES 8
#define AC_MAX_RECEIVERS 32
#define AC_MAX_BOXES 4096

typedef struct AcBox {
    double lo[3], hi[3]; /* m */
    int material;        /* index into materials */
} AcBox;

typedef struct AcSource {
    double pos[3];     /* m */
    double amplitude;  /* Pa at 1 m in free field, for the Gaussian pulse */
    double width_s;    /* the pulse's standard deviation in time, s */
    double delay_s;    /* its centre */
} AcSource;

typedef struct AcSpec {
    double size[3];   /* the domain, m, from the origin */
    double dx;        /* cell size, m */
    double c;         /* speed of sound, m/s (343.2 in air at 20 C) */
    double rho;       /* density, kg/m^3, for velocities and intensities only */
    int nmaterials;
    double xi[AC_MAX_MATERIALS];  /* normalised impedance of each material; <= 0 or > 1e12 means rigid */
    char material_name[AC_MAX_MATERIALS][32];
    int wall_material[6]; /* the domain faces x-, x+, y-, y+, z-, z+ */
    int nboxes;
    AcBox boxes[AC_MAX_BOXES]; /* solid obstacles, their surfaces made of their material */
    int nsources;
    AcSource sources[AC_MAX_SOURCES];
    int nreceivers;
    double receivers[AC_MAX_RECEIVERS][3];
    /* a piston in one face of the domain (a loudspeaker's cone in its baffle): a disk of the face moves into the air
     * with an acceleration set before every step; the face around it keeps its material (rigid for a baffle) */
    bool piston;
    int piston_face;           /* 0 x-, 1 x+, 2 y-, 3 y+, 4 z-, 5 z+ */
    double piston_centre[2];   /* m, in the face's other two axes in order (x-: y, z; z-: x, y) */
    double piston_radius;      /* m */
    int threads;
} AcSpec;

typedef struct Acoustic Acoustic;

void ac_spec_defaults(AcSpec *s);
Acoustic *ac_create(const AcSpec *s, char *err, size_t errlen);
void ac_free(Acoustic *a);

void ac_step(Acoustic *a);   /* one time step */
double ac_dt(const Acoustic *a);
double ac_time(const Acoustic *a);
long ac_steps(const Acoustic *a);
void ac_dims(const Acoustic *a, int n[3]);
/* the pressure at a point (trilinear), Pa */
double ac_pressure_at(const Acoustic *a, const double x[3]);
/* the receivers' recorded pressure histories, one sample per step: receiver r, sample k */
const double *ac_receiver_trace(const Acoustic *a, int r, long *nsamples);
/* the piston's acceleration into the air, m/s^2, used by the next step (its velocity's derivative at the current time) */
void ac_set_piston_acceleration(Acoustic *a, double acc);
/* the piston's node count and the gain that makes its volume flow that of a disk of the given radius */
void ac_piston_info(const Acoustic *a, long *nodes, double *gain);
/* energy in the domain: sum over air cells of (p^2 / (rho c^2) + rho |u|^2 ) / 2 dV, u from the pressure history */
double ac_energy(const Acoustic *a);

/* a frame for labfilm: slices through the grid (part "slice_xy" at height z_slice, "slice_xz" at y_slice, "slice_yz" at x_slice; a
 * value < 0 leaves that slice out), the wavefront as points where |p| exceeds `threshold` (every `stride` cells; part
 * "wavefront", fields p), and the obstacles as hexahedra (part "room") */
void ac_write_frame(Acoustic *a, LabWriter *w, double z_slice, double y_slice, double x_slice, double threshold, int stride);
char *ac_header_json(const AcSpec *s, const char *title);

/* Complete computed grid; pressure Pa, material occupancy. */
bool ac_write_volume(Acoustic *a, LabWriter *w);
