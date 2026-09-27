/* mag3d.h - three-dimensional magnetostatics on a cylindrical grid, by MFEM's edge (Nedelec) finite elements.
 *
 *   curl (nu curl A) = J + curl (nu Br)        B = curl A,  H = nu (B - Br),  nu = 1 / (mu0 mu_r)
 *
 * The grid is structured in (r, theta, z): rings from r[0] > 0 to r[nr], nt equal sectors around the axis, layers from
 * z[0] to z[nz] (or periodic in z: a 2D cross-section extended without ends). Every element carries its own
 * reluctivity nu, current density J and remanence Br (Cartesian components, constant per element). The outer cylinder
 * and the end planes (unless periodic) are flux walls: n x A = 0, so no flux crosses them. The inner cylinder is a
 * magnetic wall (n x H = 0: flux meets it square, as at the surface of an ideal iron core). It must not be a flux wall:
 * walls all round a meridional section would fix the flux through it at zero, forbidding the flux that circles the axis
 * in a motor's yoke or a toroid's core (found by mag3dtest T1 on its first run).
 *
 * Solution: first-order Nedelec elements on hexahedra (MFEM 4.8). The current's load is made exactly divergence-free
 * in the discrete sense by removing its gradient part (a Poisson solve on the H1 space and the discrete gradient), so
 * the curl-curl system is consistent; a small mass term, 1e-9 of the air's stiffness on the domain's scale, makes it
 * definite without changing B = curl A (gradients have no curl). Both systems are solved by a sparse Cholesky
 * factorisation (Accelerate, spdirect.h). Double precision throughout.
 *
 * Element order: e = ir + nr (it + nt iz); sector it spans theta0 + span [it, it + 1) / nt, span 2 pi or 2 pi / sector.
 * Units SI. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Mag3DGrid {
    int nr, nt, nz;
    const double *r, *z; /* nr + 1 radii (r[0] > 0), nz + 1 axial positions, both increasing (m) */
    double theta0;       /* rad */
    bool periodic_z;     /* needs nz >= 3 */
    int sector;          /* > 1: the grid spans 2 pi / sector around the axis and is periodic across its two sides (a
                            body with that rotational symmetry: one tooth of a gear); 0 or 1: the whole circle */
    bool inner_flux_wall; /* the inner cylinder a flux wall (n x A = 0) instead of a magnetic wall: for fields along the
                             axis (a coil around it), which a magnetic wall would pour into (eddy3dtest I1) */
} Mag3DGrid;

typedef struct Mag3DStats {
    int dofs, elements;
    double factor_mb;            /* the curl-curl Cholesky factor */
    double assemble_s, solve_s;  /* wall time */
    double current_removed;      /* the share of the current's load that was a gradient (projected out) */
} Mag3DStats;

static inline size_t mag3d_index(const Mag3DGrid *g, int ir, int it, int iz) { return (size_t)ir + (size_t)g->nr * ((size_t)it + (size_t)g->nt * (size_t)iz); }
static inline double mag3d_span(const Mag3DGrid *g) { return g->sector > 1 ? 2 * 3.14159265358979323846 / g->sector : 2 * 3.14159265358979323846; }
static inline size_t mag3d_count(const Mag3DGrid *g) { return (size_t)g->nr * (size_t)g->nt * (size_t)g->nz; }

/* nu[e] in; the current as J[3e..3e+2] (A/m^2, its gradient part removed) or as a current vector potential
 * T[3e..3e+2] (A/m, the current being curl T: divergence-free by construction, the natural way to give a winding), or
 * both; the magnets' remanence Br[3e..3e+2] (T). J, T and Br may be NULL. B[3e..3e+2] out, at each element's centre. */
bool mag3d_solve(const Mag3DGrid *g, const double *nu, const double *J, const double *T, const double *Br, double *B, Mag3DStats *st, char *err, size_t errlen);

/* each element's centre (x, y, z) and volume, from the same straight-edged hexahedra the solver uses */
void mag3d_centre(const Mag3DGrid *g, int ir, int it, int iz, double c[3]);
double mag3d_volume(const Mag3DGrid *g, int ir, int it, int iz);

#ifdef __cplusplus
}
#endif
