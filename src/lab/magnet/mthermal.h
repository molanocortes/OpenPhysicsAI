/* mthermal.h - heat conduction in the cross-section of a machine, on the magnet solver's grid: the losses of the
 * windings warm the copper, the heat crosses the slot liners and the iron to a cooled housing, and the rotor warms
 * through the air gap.
 *
 *   rho cp dT/dt = div(k grad T) + q
 *
 * Finite volumes on the same square cells as the field, each cell with its own conductivity and heat capacity
 * (harmonic means at faces, so that temperature and heat flux are continuous across materials); cells marked as held
 * keep their temperature (a water-cooled housing). Implicit Euler in time, which is stable at any step, solved by
 * conjugate gradients with a Jacobi preconditioner (the matrix is symmetric and positive definite). The outer edge of the
 * grid is adiabatic. Convection in the air gap and radiation are not modelled: the gap conducts as still air.
 *
 * Units: SI (K, W/m^3, W/(m K), J/(m^3 K)). */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct MThermal MThermal;

/* per cell (row by row from the lower left): conductivity, volumetric heat capacity, and whether it is held */
MThermal *mt_create(int nx, int ny, double dx, const double *k, const double *rho_cp, const unsigned char *held, double T_init,
                    int threads);
void mt_free(MThermal *t);
/* one implicit step of dt with the heat sources q (W/m^3 per cell); returns the conjugate-gradient iterations, or -1 */
int mt_step(MThermal *t, double dt, const double *q);
double *mt_temperature(MThermal *t); /* writable: set held cells' temperatures here */
/* the heat taken by the held cells during the last step, W per metre of length (positive out of the free cells) */
double mt_held_heat(const MThermal *t);
/* the heat stored in the free cells relative to a uniform T_ref, J per metre of length */
double mt_stored(const MThermal *t, double T_ref);
