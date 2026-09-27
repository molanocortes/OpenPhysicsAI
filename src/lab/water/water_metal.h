/* water_metal.h - the free-surface water solver of water.h on the GPU, by Metal: the same weakly compressible SPH
 * (Wendland kernel, Tait's equation, delta-SPH with the hydrostatic correction, Monaghan's viscosity, Adami's walls,
 * symplectic Euler), one GPU thread per particle, in single precision: a particle's position as its cell and its offset
 * from the cell's corner, density as its deviation from rho0, both summed with Kahan's compensation. Still water stays
 * about a third more restless than on the CPU (wtest W1 fails on the GPU; water.md says why).
 *
 * Every step the particles are re-sorted into cell order on the GPU (counted, scanned, scattered), so that each pass
 * reads its neighbours in memory order; the step is computed on the GPU from the largest speed and acceleration, so
 * that batches of steps run without the CPU. Within each cell the particles are put in the order of their identities,
 * so that every sum runs in a fixed order and a run repeats bit for bit. The double-precision CPU engine is the
 * reference; wtest runs its cases on both. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "water.h"

typedef struct WtGpu WtGpu;

/* the particles and walls of a CPU solver made and filled with wt_add_* (its constants are fixed here) */
WtGpu *wt_gpu_create(Wt *cpu, char *err, size_t errlen);
void wt_gpu_free(WtGpu *G);
/* nsteps steps; false if the solver went unstable */
bool wt_gpu_steps(WtGpu *G, int nsteps);
/* up to `until` seconds, in batches; false if unstable */
bool wt_gpu_run_to(WtGpu *G, double until);
double wt_gpu_time(WtGpu *G);
long wt_gpu_steps_done(const WtGpu *G);
/* the water particles' state into the CPU solver (in its own order), with the clock, so that its queries answer */
void wt_gpu_sync(WtGpu *G, Wt *cpu);
const char *wt_gpu_name(const WtGpu *G);
