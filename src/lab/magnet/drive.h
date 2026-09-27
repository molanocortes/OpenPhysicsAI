/* drive.h - a motor that runs: the rotor's equation of motion under the electromagnetic torque and a load, and the run
 * that spins a motor up and then follows it heating (docs/lab/magnet.md, "A motor that runs and heats").
 *
 *   J d omega / dt = T_em(theta) - T_load(omega),  d theta / dt = omega,
 *   T_load = T_0 omega / (|omega| + 1e-3) + c omega |omega|   (a friction-like constant part and a fan)
 *
 * integrated by fourth-order Runge-Kutta. With the phase currents locked to the rotor's angle (a current-controlled
 * drive), the electromagnetic torque depends on the angle alone, so it is tabulated once over its period from the
 * magnetostatic solver and interpolated. Eddy currents in the magnets and the iron, and iron losses, are not modelled. */
#pragma once

#include <stdbool.h>

#include "../../core/json.h"
#include "../lab_domains.h"

typedef struct MgLoad {
    double inertia;   /* kg m^2 */
    double torque;    /* T_0, N m */
    double fan;       /* c, N m s^2 */
} MgLoad;

typedef double (*MgTorqueFn)(double theta, void *ctx); /* N m at the rotor angle theta (rad) */

void mg_rotor_rk4(double *theta, double *omega, double dt, const MgLoad *load, MgTorqueFn torque, void *ctx);
