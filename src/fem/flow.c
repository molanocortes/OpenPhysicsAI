/* flow.c - steady laminar flow from the lattice Boltzmann kernel, in SI units (see flow.h) */
#include "flow.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../lbm.h"

enum { CHECK_EVERY = 200 };
static const double U_LATTICE_MAX = 0.06, TAU_START = 0.8, TAU_MIN = 0.52;
/* the estimated density variation a run is designed for, and the measured one it may not exceed */
static const double DRHO_TARGET = 0.01, DRHO_LIMIT = 0.03;

static pthread_mutex_t g_lattice_lock = PTHREAD_MUTEX_INITIALIZER;

bool flow_parameters(const FlowSpec *s, double *tau, double *u_lb, double *re_cell, char *err, size_t errlen) {
    if (s->nx < 4 || s->ny < 4 || s->nz < 4) {
        snprintf(err, errlen, "the flow lattice needs at least 4 cells along every axis (got %d x %d x %d)", s->nx, s->ny, s->nz);
        return false;
    }
    if (!(s->dx > 0) || !(s->viscosity > 0) || !(s->inlet_velocity > 0) || !isfinite(s->dx + s->viscosity + s->inlet_velocity)) {
        snprintf(err, errlen, "the flow needs a positive cell size, viscosity and inlet velocity (got %g m, %g m^2/s, %g m/s)", s->dx, s->viscosity,
                 s->inlet_velocity);
        return false;
    }
    double rc = s->inlet_velocity * s->dx / s->viscosity, nu = (TAU_START - 0.5) / 3;
    bool mach_limited = false, pressure_limited = false;
    if (rc * nu > U_LATTICE_MAX) nu = U_LATTICE_MAX / rc, mach_limited = true;
    /* compressibility of a confined flow: the viscous pressure drop along a channel of D cells over L cells is about
     * 12 nu u L / D^2 in lattice units, and the density follows the pressure as rho' = 3 p'. With u = Re_c nu that is
     * 36 nu^2 Re_c L / D^2, bounded here by lowering the lattice viscosity (and with it the lattice velocity) */
    int D = 1 << 30;
    if (s->wall[0] != FLOW_WALL_PERIODIC || s->wall[1] != FLOW_WALL_PERIODIC) D = s->ny;
    if ((s->wall[2] != FLOW_WALL_PERIODIC || s->wall[3] != FLOW_WALL_PERIODIC) && s->nz < D) D = s->nz;
    if (D < (1 << 30)) {
        double nu_c = D / 6.0 * sqrt(DRHO_TARGET / (rc * s->nx));
        if (nu > nu_c) nu = nu_c, pressure_limited = true, mach_limited = false;
    }
    double t = 0.5 + 3 * nu, u = rc * nu;
    if (t < TAU_MIN) {
        if (pressure_limited)
            snprintf(err, errlen,
                     "the flow cannot be run incompressibly at this resolution: with a cell Reynolds number of %.3g, a domain %d cells long and %d "
                     "cells across, keeping the lattice pressure drop below a %.0f %% density change needs a relaxation time of %.4f (below %.2f). "
                     "Refine the mesh across the flow, shorten the domain or lower the velocity",
                     rc, s->nx, D, 100 * DRHO_TARGET, t, TAU_MIN);
        else {
            double umax = U_LATTICE_MAX / ((TAU_MIN - 0.5) / 3) * s->viscosity / s->dx;
            snprintf(err, errlen,
                     "the cell Reynolds number U dx / nu = %.3g is too high for a stable lattice: it would need a relaxation time of %.4f (below %.2f). "
                     "At this cell size (%.3g mm) the largest admissible inlet velocity is %.3g m/s; refine the mesh or lower the velocity",
                     rc, t, TAU_MIN, 1e3 * s->dx, umax);
        }
        return false;
    }
    (void)mach_limited;
    *tau = t, *u_lb = u, *re_cell = rc;
    return true;
}

void flow_field_free(FlowField *f) {
    free(f->u), free(f->rho);
    memset(f, 0, sizeof *f);
}

static LbmWall lbm_wall(FlowWall w) { return w == FLOW_WALL_SLIP ? LBM_WALL_SLIP : w == FLOW_WALL_PERIODIC ? LBM_WALL_PERIODIC : LBM_WALL_NOSLIP; }

bool flow_solve(const FlowSpec *s, FlowField *f, char *err, size_t errlen) {
    memset(f, 0, sizeof *f);
    double tau, u_lb, rc;
    if (!flow_parameters(s, &tau, &u_lb, &rc, err, errlen)) return false;
    size_t nc = (size_t)s->nx * (size_t)s->ny * (size_t)s->nz;
    double tol = s->steady_tolerance > 0 ? s->steady_tolerance : 2e-6;
    long max_steps = s->max_steps > 0 ? s->max_steps : 400000;
    f->nx = s->nx, f->ny = s->ny, f->nz = s->nz, f->dx = s->dx;
    f->tau = tau, f->u_lattice = u_lb, f->mach = u_lb * sqrt(3.0), f->reynolds_cell = rc, f->dt = u_lb * s->dx / s->inlet_velocity;
    f->u = calloc(3 * nc, sizeof(double));
    f->rho = calloc(nc, sizeof(double));
    float *rho = malloc(nc * sizeof(float)), *ux = malloc(nc * sizeof(float)), *uy = malloc(nc * sizeof(float)), *uz = malloc(nc * sizeof(float));
    float *prev = calloc(3 * nc, sizeof(float));
    uint8_t *solid = calloc(nc, 1);
    if (!f->u || !f->rho || !rho || !ux || !uy || !uz || !prev || !solid) {
        free(rho), free(ux), free(uy), free(uz), free(prev), free(solid);
        flow_field_free(f);
        snprintf(err, errlen, "out of memory for a %d x %d x %d flow lattice", s->nx, s->ny, s->nz);
        return false;
    }
    if (s->solid)
        for (size_t c = 0; c < nc; c++) solid[c] = s->solid[c] ? 1 : 0;
    pthread_mutex_lock(&g_lattice_lock);
    Lbm L;
    bool ok = lbm_create(&L, s->nx, s->ny, s->nz, s->pool);
    if (!ok) {
        pthread_mutex_unlock(&g_lattice_lock);
        free(rho), free(ux), free(uy), free(uz), free(prev), free(solid);
        flow_field_free(f);
        snprintf(err, errlen, "the %d x %d x %d flow lattice could not be allocated (%.0f MB)", s->nx, s->ny, s->nz,
                 (double)lbm_bytes_estimate(s->nx, s->ny, s->nz) / 1048576.0);
        return false;
    }
    L.periodic_z = s->wall[2] == FLOW_WALL_PERIODIC && s->wall[3] == FLOW_WALL_PERIODIC;
    lbm_set_solids(&L, solid);
    lbm_reset(&L, 0.0f);
    LbmConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.tau0 = (float)tau;
    cfg.collision = LBM_RECURSIVE;
    for (int w = 0; w < 4; w++) cfg.wall[w] = lbm_wall(s->wall[w]);
    /* start-up: the inlet velocity is ramped over a few acoustic crossings of the domain so that no strong pressure
     * pulse is launched */
    long ramp = (long)ceil(4 * sqrt(3.0) * fmax(s->nx, fmax(s->ny, s->nz)));
    if (ramp < 500) ramp = 500;
    LbmOutput out = {rho, ux, uy, uz};
    long step = 0;
    double change = INFINITY;
    bool have_prev = false;
    while (step < max_steps) {
        double r = step < ramp ? (double)step / (double)ramp : 1.0;
        cfg.u_in = (float)(u_lb * r * r * (3 - 2 * r));
        bool sample = (step + 1) % CHECK_EVERY == 0;
        lbm_step(&L, &cfg, sample ? &out : NULL);
        step++;
        if (L.unstable) break;
        if (!sample) continue;
        double worst = 0;
        bool finite = true;
        for (size_t c = 0; c < nc; c++) {
            if (solid[c]) continue;
            float jx = rho[c] * ux[c], jy = rho[c] * uy[c], jz = rho[c] * uz[c];
            finite &= isfinite(jx) && isfinite(jy) && isfinite(jz);
            if (have_prev) {
                double d = fmax(fabs((double)jx - prev[3 * c]), fmax(fabs((double)jy - prev[3 * c + 1]), fabs((double)jz - prev[3 * c + 2])));
                worst = fmax(worst, d);
            }
            prev[3 * c] = jx, prev[3 * c + 1] = jy, prev[3 * c + 2] = jz;
        }
        if (!finite) {
            L.unstable = true;
            break;
        }
        change = have_prev ? worst / u_lb : INFINITY;
        have_prev = true;
        if (step > ramp && change <= tol) break;
    }
    bool unstable = L.unstable;
    lbm_destroy(&L);
    pthread_mutex_unlock(&g_lattice_lock);
    f->steps = step;
    f->physical_time = (double)step * f->dt;
    f->change = change;
    f->converged = !unstable && change <= tol;
    if (unstable) {
        free(rho), free(ux), free(uy), free(uz), free(prev), free(solid);
        flow_field_free(f);
        snprintf(err, errlen, "the lattice became unstable after %ld steps (relaxation time %.3f, lattice velocity %.3f)", step, tau, u_lb);
        return false;
    }
    double scale = s->dx / f->dt; /* m/s per lattice velocity */
    f->density_min = INFINITY, f->density_max = -INFINITY;
    for (size_t c = 0; c < nc; c++) {
        if (solid[c]) continue;
        f->rho[c] = rho[c];
        f->u[3 * c] = scale * prev[3 * c], f->u[3 * c + 1] = scale * prev[3 * c + 1], f->u[3 * c + 2] = scale * prev[3 * c + 2];
        f->density_min = fmin(f->density_min, rho[c]), f->density_max = fmax(f->density_max, rho[c]);
    }
    double a = s->dx * s->dx;
    for (int k = 0; k < s->nz; k++)
        for (int j = 0; j < s->ny; j++) {
            size_t c0 = (size_t)s->nx * ((size_t)j + (size_t)s->ny * (size_t)k);
            f->inlet_flux += a * f->u[3 * c0];
            f->outlet_flux += a * f->u[3 * (c0 + (size_t)s->nx - 1)];
        }
    free(rho), free(ux), free(uy), free(uz), free(prev), free(solid);
    if (f->converged && f->density_max - f->density_min > DRHO_LIMIT) {
        snprintf(err, errlen,
                 "the lattice density varied by %.2f %% (%.4f to %.4f), more than the %.0f %% an incompressible flow may show: the pressure "
                 "differences of this flow are too large for the lattice. Refine the mesh or lower the velocity",
                 100 * (f->density_max - f->density_min), f->density_min, f->density_max, 100 * DRHO_LIMIT);
        f->converged = false;
        return false;
    }
    if (!f->converged) {
        snprintf(err, errlen, "the flow did not become steady in %ld lattice steps (%.3g s): the momentum field still changes by %.3g of the inlet velocity per %d steps (tolerance %.3g)",
                 step, f->physical_time, change, CHECK_EVERY, tol);
        return false;
    }
    return true;
}

/* ---- nodal mapping ------------------------------------------------------------------------------------------------ */

bool flow_nodal_velocity(const FlowSpec *s, const FlowField *f, double *vel, FlowMapStats *stats, char *err, size_t errlen) {
    int nx = s->nx, ny = s->ny, nz = s->nz;
    if (f->nx != nx || f->ny != ny || f->nz != nz || !f->u) {
        snprintf(err, errlen, "the flow field does not belong to this lattice");
        return false;
    }
    size_t nn = (size_t)(nx + 1) * (size_t)(ny + 1) * (size_t)(nz + 1);
    memset(vel, 0, 3 * nn * sizeof(double));
    for (int k = 0; k <= nz; k++)
        for (int j = 0; j <= ny; j++)
            for (int i = 0; i <= nx; i++) {
                size_t n = (size_t)i + (size_t)(nx + 1) * ((size_t)j + (size_t)(ny + 1) * (size_t)k);
                /* sides: y- (j = 0), y+ (j = ny), z- (k = 0), z+ (k = nz) */
                bool noslip = (j == 0 && s->wall[0] == FLOW_WALL_NOSLIP) || (j == ny && s->wall[1] == FLOW_WALL_NOSLIP) ||
                              (k == 0 && s->wall[2] == FLOW_WALL_NOSLIP) || (k == nz && s->wall[3] == FLOW_WALL_NOSLIP);
                if (noslip) continue;
                bool slip_y = (j == 0 && s->wall[0] == FLOW_WALL_SLIP) || (j == ny && s->wall[1] == FLOW_WALL_SLIP);
                bool slip_z = (k == 0 && s->wall[2] == FLOW_WALL_SLIP) || (k == nz && s->wall[3] == FLOW_WALL_SLIP);
                double sum[3] = {0, 0, 0};
                int count = 0;
                bool touches_solid = false;
                for (int dk = -1; dk <= 0 && !touches_solid; dk++)
                    for (int dj = -1; dj <= 0 && !touches_solid; dj++)
                        for (int di = -1; di <= 0 && !touches_solid; di++) {
                            int ci = i + di, cj = j + dj, ck = k + dk;
                            if (ci < 0 || ci >= nx) continue; /* beyond the inlet or outlet plane */
                            if (cj < 0 || cj >= ny) {
                                FlowWall w = cj < 0 ? s->wall[0] : s->wall[1];
                                if (w != FLOW_WALL_PERIODIC) continue;
                                cj = (cj + ny) % ny;
                            }
                            if (ck < 0 || ck >= nz) {
                                FlowWall w = ck < 0 ? s->wall[2] : s->wall[3];
                                if (w != FLOW_WALL_PERIODIC) continue;
                                ck = (ck + nz) % nz;
                            }
                            size_t c = (size_t)ci + (size_t)nx * ((size_t)cj + (size_t)ny * (size_t)ck);
                            if (s->solid && s->solid[c]) {
                                touches_solid = true;
                                break;
                            }
                            for (int d = 0; d < 3; d++) sum[d] += f->u[3 * c + (size_t)d];
                            count++;
                        }
                if (touches_solid || count == 0) continue;
                for (int d = 0; d < 3; d++) vel[3 * n + (size_t)d] = sum[d] / count;
                if (slip_y) vel[3 * n + 1] = 0;
                if (slip_z) vel[3 * n + 2] = 0;
            }
    /* mass conservation per cross-section. Every plane x = i must carry the inlet volume flow (the sides are walls or
     * periodic). The cell-to-node averaging does not guarantee it: at the inlet the lattice imposes a plug profile
     * whose wall layer the bilinear faces between zero wall nodes under-represent by several percent, less so
     * downstream where the profile has developed. Each plane's nodes are therefore scaled so that its finite-element
     * volume flux equals U times the open inlet area; the remaining transverse divergence is left for the energy
     * equation's divergence diagnostic to report. */
    double a = s->dx * s->dx, open_area = 0;
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < ny; j++)
            if (!(s->solid && s->solid[(size_t)nx * ((size_t)j + (size_t)ny * (size_t)k)])) open_area += a;
    double target = s->inlet_velocity * open_area, lo = INFINITY, hi = -INFINITY;
    for (int i = 0; i <= nx; i++) {
        double flux = 0;
        for (int k = 0; k < nz; k++)
            for (int j = 0; j < ny; j++) {
                double q = 0;
                for (int dk = 0; dk <= 1; dk++)
                    for (int dj = 0; dj <= 1; dj++)
                        q += 0.25 * vel[3 * ((size_t)i + (size_t)(nx + 1) * ((size_t)(j + dj) + (size_t)(ny + 1) * (size_t)(k + dk)))];
                flux += a * q;
            }
        if (!(flux > 0)) {
            snprintf(err, errlen, "the mapped flow carries no volume through the cross-section at x = %d cells (blocked or reversed)", i);
            return false;
        }
        double sc = target / flux;
        lo = fmin(lo, sc), hi = fmax(hi, sc);
        for (int k = 0; k <= nz; k++)
            for (int j = 0; j <= ny; j++) {
                size_t n = (size_t)i + (size_t)(nx + 1) * ((size_t)j + (size_t)(ny + 1) * (size_t)k);
                for (int d = 0; d < 3; d++) vel[3 * n + (size_t)d] *= sc;
            }
    }
    if (stats) stats->scale_min = lo, stats->scale_max = hi;
    return true;
}
