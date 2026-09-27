/* gpu_none.c - the GPU engines where there is no Metal (Linux and every system but macOS). The flow engine
 * (lbm3d_metal.m) and the water engine (water_metal.m) run on Metal, a system framework of macOS; elsewhere their
 * functions are these: creating an engine fails with a message that says so, and every scenario and test that can run
 * on the CPU does ("engine": "cpu" in a scenario). On macOS this file compiles to nothing. */
#ifndef __APPLE__
#include <stdio.h>

#include "../water/water_metal.h"
#include "lbm3d_metal.h"

#define NO_METAL "the GPU engine needs Metal, which only macOS has; set \"engine\": \"cpu\" in the scenario"

Lbm3DGpu *lbm3d_gpu_create(const Lbm3D *geometry, char *err, size_t errlen) {
    (void)geometry;
    if (err && errlen) snprintf(err, errlen, "lbm3d: %s", NO_METAL);
    return NULL;
}
void lbm3d_gpu_free(Lbm3DGpu *G) { (void)G; }
void lbm3d_gpu_init(Lbm3DGpu *G) { (void)G; }
void lbm3d_gpu_set_inlet(Lbm3DGpu *G, const double u[3]) { (void)G, (void)u; }
bool lbm3d_gpu_steps(Lbm3DGpu *G, int nsteps, double *forces) { (void)G, (void)nsteps, (void)forces; return false; }
void lbm3d_gpu_macro(Lbm3DGpu *G, double *rho, double *u) { (void)G, (void)rho, (void)u; }
long lbm3d_gpu_steps_done(const Lbm3DGpu *G) { (void)G; return 0; }
const char *lbm3d_gpu_name(const Lbm3DGpu *G) { (void)G; return "none"; }
bool lbm3d_gpu_ib_enable(Lbm3DGpu *G, int npts, int iterations, double relax, char *err, size_t errlen) {
    (void)G, (void)npts, (void)iterations, (void)relax;
    if (err && errlen) snprintf(err, errlen, "lbm3d: %s", NO_METAL);
    return false;
}
float *lbm3d_gpu_ib_points(Lbm3DGpu *G) { (void)G; return NULL; }
float *lbm3d_gpu_ib_velocities(Lbm3DGpu *G) { (void)G; return NULL; }
const float *lbm3d_gpu_ib_forces(const Lbm3DGpu *G) { (void)G; return NULL; }
bool lbm3d_gpu_ib_step(Lbm3DGpu *G, double force[3]) { (void)G, (void)force; return false; }

WtGpu *wt_gpu_create(Wt *cpu, char *err, size_t errlen) {
    (void)cpu;
    if (err && errlen) snprintf(err, errlen, "water: %s", NO_METAL);
    return NULL;
}
void wt_gpu_free(WtGpu *G) { (void)G; }
bool wt_gpu_steps(WtGpu *G, int nsteps) { (void)G, (void)nsteps; return false; }
bool wt_gpu_run_to(WtGpu *G, double until) { (void)G, (void)until; return false; }
double wt_gpu_time(WtGpu *G) { (void)G; return 0; }
long wt_gpu_steps_done(const WtGpu *G) { (void)G; return 0; }
void wt_gpu_sync(WtGpu *G, Wt *cpu) { (void)G, (void)cpu; }
const char *wt_gpu_name(const WtGpu *G) { (void)G; return "none"; }
#endif
