/* eddy3d.h - induction: alternating currents in a coil drive eddy currents in a conductor, whose losses heat it.
 *
 * Eddy currents, time-harmonic at angular frequency omega (the A* formulation):
 *
 *   curl (nu curl A) + j omega sigma A = curl T        E = -j omega A,   q = sigma |E|^2 / 2 (time-averaged loss)
 *
 * on the cylindrical grid of mag3d.h with MFEM's first-order Nedelec elements; the coil's current enters as a current
 * vector potential T (its amplitude). Real and imaginary parts make a symmetric indefinite system of twice the size,
 * factorised by Accelerate's pivoted LDL^T; a mass term of 1e-9 of the air's stiffness keeps the non-conducting regions
 * definite, as in mag3d. The outer cylinder and the ends are flux walls, the inner cylinder a magnetic wall.
 *
 * Heat: rho c dT/dt = div (k grad T) + q on first-order H1 elements over the same mesh, backward Euler (one Cholesky
 * factorisation for the whole run), the outer cylinder and ends held at the ambient temperature, the inner cylinder
 * insulated. Properties are constant in time: the steel's loss of magnetism at its Curie point and the change of its
 * conductivity with temperature are not followed (one-way coupling). Units SI. */
#pragma once
#include "mag3d.h"

#ifdef __cplusplus
extern "C" {
#endif

/* per element: nu, sigma; T (3 per element, amplitude). Out, per element (any may be NULL): the time-averaged loss q
 * (W/m^3), |B| amplitude (T), and the current density amplitude |J| = sigma omega |A| (A/m^2). */
bool eddy3d_solve(const Mag3DGrid *g, const double *nu, const double *sigma, const double *T, double omega, double *q, double *Bamp, double *Jamp,
                  Mag3DStats *st, char *err, size_t errlen);

typedef struct Heat3DFem Heat3DFem;
/* k (W/m K) and rho c (J/m^3 K) per element; the whole grid starts at the ambient temperature */
Heat3DFem *heat3dfem_create(const Mag3DGrid *g, const double *k, const double *rhocp, double ambient_k, double dt, char *err, size_t errlen);
bool heat3dfem_step(Heat3DFem *H, const double *q); /* one step with the heat source q (W/m^3 per element) */
void heat3dfem_temperature(const Heat3DFem *H, double *T); /* per element, at its centre (K) */
double heat3dfem_energy(const Heat3DFem *H);  /* integral of rho c (T - ambient), J */
void heat3dfem_free(Heat3DFem *H);

#ifdef __cplusplus
}
#endif
