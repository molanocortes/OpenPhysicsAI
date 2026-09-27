/* fsi.c - the sheet and the flow, stepped together (fsi.h). */
#include "fsi.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Fsi {
    Lbm3DGpu *G;
    Sheet *S;
    double dx, dt, rho, o[3], F[3];
    int n, sub;
    bool pinned_mass_zero;
    double *f, *save, *vpred;
};

Fsi *fsi_create(Lbm3DGpu *G, Sheet *S, double dx, double dt, double rho, const double origin[3], int iterations, double relax, char *err,
                size_t errlen) {
    if (!G || !S || !(dx > 0) || !(dt > 0) || !(rho > 0)) {
        snprintf(err, errlen, "fsi: a flow, a sheet, a cell size, a step and a density are needed");
        return NULL;
    }
    Fsi *C = calloc(1, sizeof *C);
    if (!C) return NULL;
    C->G = G, C->S = S, C->dx = dx, C->dt = dt, C->rho = rho, C->n = sheet_nodes(S), C->pinned_mass_zero = true;
    memcpy(C->o, origin, sizeof C->o);
    C->f = calloc(3 * (size_t)C->n, sizeof(double)), C->save = malloc(6 * (size_t)C->n * sizeof(double));
    C->vpred = malloc(3 * (size_t)C->n * sizeof(double));
    double sdt = sheet_stable_dt(S);
    C->sub = (int)ceil(dt / sdt);
    if (C->sub < 1) C->sub = 1;
    if (!C->f || !C->save || !C->vpred || !sheet_node_areas(S)) {
        snprintf(err, errlen, "fsi: out of memory");
        fsi_free(C);
        return NULL;
    }
    if (!lbm3d_gpu_ib_enable(G, C->n, iterations, relax, err, errlen)) {
        fsi_free(C);
        return NULL;
    }
    return C;
}

void fsi_free(Fsi *C) {
    if (!C) return;
    free(C->f), free(C->save), free(C->vpred), free(C);
}

bool fsi_step(Fsi *C, ThreadPool *pool, double body_force[3]) {
    float *pt = lbm3d_gpu_ib_points(C->G), *vel = lbm3d_gpu_ib_velocities(C->G);
    const double *x = sheet_positions(C->S), *v = sheet_velocities(C->S), *A = sheet_node_areas(C->S);
    const double vs = C->dt / C->dx, fs = C->rho * pow(C->dx, 4) / (C->dt * C->dt); /* lattice force to newtons */
    /* predict: the sheet over the step under the fluid's force of the step before (its own stiffness included), then
     * back; the flow is asked to move with the predicted velocity, and the step's change of force acts on the node's
     * mass (lbm3d_metal.h). Without the prediction, a stiff sheet ringing within the step took energy from nothing. */
    sheet_save(C->S, C->save);
    sheet_set_external(C->S, C->f);
    for (int s = 0; s < C->sub; s++) sheet_step(C->S, C->dt / C->sub, pool);
    memcpy(C->vpred, sheet_velocities(C->S), 3 * (size_t)C->n * sizeof(double));
    sheet_restore(C->S, C->save);
    for (int i = 0; i < C->n; i++) {
        double m = C->pinned_mass_zero && sheet_pinned(C->S, i) ? 0 : sheet_node_mass(C->S, i) / (C->rho * C->dx * C->dx * C->dx);
        for (int k = 0; k < 3; k++) {
            pt[4 * i + k] = (float)((x[3 * i + k] - C->o[k]) / C->dx);
            /* the kernel subtracts F_new / m; the prediction already holds the old force's effect: add it back */
            vel[4 * i + k] = (float)(C->vpred[3 * i + k] * vs + (m > 0 ? -C->f[3 * i + k] / fs / m : 0));
        }
        pt[4 * i + 3] = (float)(A[i] / (C->dx * C->dx)), vel[4 * i + 3] = (float)m;
    }
    double bf[3];
    bool ok = lbm3d_gpu_ib_step(C->G, bf);
    if (body_force) memcpy(body_force, bf, sizeof bf);
    const float *F = lbm3d_gpu_ib_forces(C->G);
    C->F[0] = C->F[1] = C->F[2] = 0;
    for (int i = 0; i < C->n; i++)
        for (int k = 0; k < 3; k++) C->f[3 * i + k] = -F[4 * i + k] * fs, C->F[k] += C->f[3 * i + k];
    sheet_set_external(C->S, C->f);
    for (int s = 0; s < C->sub; s++) sheet_step(C->S, C->dt / C->sub, pool);
    static int debug = -1; /* FSI_DEBUG=1: each step's largest node speed and force, and the sheet's energies */
    if (debug < 0) debug = getenv("FSI_DEBUG") != NULL;
    if (debug) {
        double vmax = 0, fmx = 0;
        for (int i = 0; i < C->n; i++) {
            vmax = fmax(vmax, sqrt(v[3 * i] * v[3 * i] + v[3 * i + 1] * v[3 * i + 1] + v[3 * i + 2] * v[3 * i + 2]));
            fmx = fmax(fmx, sqrt(C->f[3 * i] * C->f[3 * i] + C->f[3 * i + 1] * C->f[3 * i + 1] + C->f[3 * i + 2] * C->f[3 * i + 2]));
        }
        double em, eb, ek;
        sheet_energy(C->S, &em, &eb, &ek);
        fprintf(stderr, "fsi: vmax %.4g m/s  fmax %.4g N  membrane %.4g bending %.4g kinetic %.4g  ok %d\n", vmax, fmx, em, eb, ek, ok);
    }
    return ok;
}

void fsi_force(const Fsi *C, double F[3]) { memcpy(F, C->F, sizeof C->F); }
int fsi_substeps(const Fsi *C) { return C->sub; }
