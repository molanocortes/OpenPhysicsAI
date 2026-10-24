/* sphtest.c - verification of the particle solver for hypervelocity impact (src/lab/impact/sph.c).
 *
 * Criteria, written on 2026-09-26 before the first run and not to be changed without a dated note:
 *   S1  Hugoniot: a copper flyer at 500 m/s on a copper target, a column with four symmetry planes (uniaxial strain),
 *       pressure only. The symmetric impact puts the target behind the shock at half the flyer speed, up = 250 m/s,
 *       and the Mie-Gruneisen Hugoniot of the material gives the shock speed us = c0 + s up and the pressure
 *       p = rho0 us up. Criteria: particle velocity behind the shock within 2 % of up, pressure within 3 % of p,
 *       shock speed (from the front's positions at two times) within 3 % of us.
 *   S2  Conservation: a copper sphere at 1 km/s into a copper plate, quarter model (two symmetry planes), with strength.
 *       Momentum along the impact within 1e-9 of the initial momentum; kinetic plus internal energy within 1 %.
 *   S3  Against the Lagrangian solver: the Taylor test of examples/lab/taylor_copper.json (OFHC copper rod 25.4 mm long,
 *       7.62 mm across, 190 m/s onto a rigid wall) run with particles. The hexahedral solver's final length,
 *       L / L0 = 0.685 on two meshes (docs/lab/impact.md), is the independent solution; criterion: within 3 %.
 *       The largest diameter is recorded, not judged (1.91 D0 in the hexahedral run). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../src/lab/impact/impact_materials.h"
#include "../src/lab/impact/sph.h"

static int failures;

static void verdict(bool ok, const char *name) {
    printf("  %s: %s\n", name, ok ? "pass" : "FAIL");
    fflush(stdout);
    if (!ok) failures++;
}

static double wall(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + 1e-9 * t.tv_nsec;
}

static void copper(SphSpec *s) {
    s->nmaterials = 1;
    s->materials[0] = *im_material_find("ofhc_copper");
}

/* the front: the largest z at which a target particle carries more than half the Hugoniot pressure */
static double front(Sph *p, double phalf) {
    const double *x = sph_positions(p), *pr = sph_pressure(p);
    double z = -1;
    for (int i = 0; i < sph_count(p); i++)
        if (sph_body(p)[i] == 1 && pr[i] > phalf && x[3 * i + 2] > z) z = x[3 * i + 2];
    return z;
}

/* the Taylor test with particles of spacing dx; returns L / L0 and D / D0 */
static bool g_correct;

static void taylor(double dx, double alpha, double beta, double art, double *LL0, double *DD0, int *np, long *steps) {
    SphSpec s;
    sph_spec_defaults(&s);
    copper(&s);
    s.correct_gradient = g_correct;
    s.dx = dx;
    if (alpha >= 0) s.av_alpha = alpha;
    if (beta >= 0) s.av_beta = beta;
    if (art >= 0) s.art_stress = art;
    s.nplanes = 3;
    s.planes[0] = (SphPlane){0, 0.0, +1, false}, s.planes[1] = (SphPlane){1, 0.0, +1, false};
    s.planes[2] = (SphPlane){2, 0.0, +1, true}; /* the anvil */
    char err[256];
    Sph *p = sph_create(&s, err, sizeof err);
    const double R = 3.81e-3, L0 = 25.4e-3;
    double base[3] = {0, 0, 0}, v[3] = {0, 0, -190};
    sph_add_cylinder(p, base, R, L0, 0, v);
    while (sph_time(p) < 80e-6 && !sph_unstable(p)) sph_step(p);
    const double *x = sph_positions(p);
    double zmax = 0, rmax = 0;
    for (int i = 0; i < sph_count(p); i++) {
        zmax = fmax(zmax, x[3 * i + 2]);
        if (x[3 * i + 2] < 2 * s.dx) rmax = fmax(rmax, hypot(x[3 * i], x[3 * i + 1]));
    }
    *LL0 = (zmax + 0.5 * s.dx) / L0, *DD0 = (rmax + 0.5 * s.dx) / R;
    *np = sph_count(p), *steps = sph_unstable(p) ? -1 : sph_steps(p);
    sph_free(p);
}

int main(int argc, char **argv) {
    if (argc == 6) { /* exploration outside the verdicts: sphtest DX ALPHA BETA ART_STRESS CORRECT(0|1) */
        g_correct = atoi(argv[5]) != 0;
        double L, D;
        int np;
        long st;
        double c0 = wall();
        taylor(atof(argv[1]), atof(argv[2]), atof(argv[3]), atof(argv[4]), &L, &D, &np, &st);
        printf("dx %s alpha %s beta %s art %s: L/L0 %.4f D/D0 %.3f, %d particles, %ld steps, %.0f s\n", argv[1], argv[2], argv[3], argv[4], L, D, np, st,
               wall() - c0);
        return 0;
    }
    const ImMaterial *Cu = im_material_find("ofhc_copper");
    printf("== S1: symmetric copper impact at 500 m/s, uniaxial strain, against the Hugoniot\n");
    {
        double c0 = wall();
        SphSpec s;
        sph_spec_defaults(&s);
        copper(&s);
        s.hydro = true, s.art_stress = 0;
        s.dx = 0.05e-3;
        const double W = 4 * s.dx;
        s.nplanes = 4;
        s.planes[0] = (SphPlane){0, 0.0, +1, false}, s.planes[1] = (SphPlane){0, W, -1, false};
        s.planes[2] = (SphPlane){1, 0.0, +1, false}, s.planes[3] = (SphPlane){1, W, -1, false};
        char err[256];
        Sph *p = sph_create(&s, err, sizeof err);
        double lo0[3] = {0, 0, -3e-3}, hi0[3] = {W, W, 0}, lo1[3] = {0, 0, 0}, hi1[3] = {W, W, 6e-3};
        double vf[3] = {0, 0, 500}, v0[3] = {0, 0, 0};
        sph_add_box(p, lo0, hi0, 0, vf);
        sph_add_box(p, lo1, hi1, 0, v0);
        double up = 250, us = Cu->c0 + Cu->s * up, pH = Cu->rho0 * us * up;
        double t1 = 0.4e-6, t2 = 0.8e-6, z1 = 0, z2 = 0;
        while (sph_time(p) < t1 && !sph_unstable(p)) sph_step(p);
        z1 = front(p, 0.5 * pH), t1 = sph_time(p);
        while (sph_time(p) < t2 && !sph_unstable(p)) sph_step(p);
        z2 = front(p, 0.5 * pH), t2 = sph_time(p);
        /* the plateau: target particles between the interface region and the front */
        const double *x = sph_positions(p), *v = sph_velocities(p), *pr = sph_pressure(p);
        double zi = up * t2, sp = 0, sv = 0;
        int n = 0;
        for (int i = 0; i < sph_count(p); i++) {
            double z = x[3 * i + 2];
            if (sph_body(p)[i] != 1 || z < zi + 0.5e-3 || z > z2 - 0.5e-3) continue;
            sp += pr[i], sv += v[3 * i + 2], n++;
        }
        double pm = n ? sp / n : 0, vm = n ? sv / n : 0, usm = (z2 - z1) / (t2 - t1);
        printf("  Hugoniot: up %.1f m/s, us %.1f m/s, p %.4g GPa\n", up, us, pH * 1e-9);
        printf("  measured over %d particles: up %.2f m/s (%+.2f %%), p %.4g GPa (%+.2f %%), us %.1f m/s (%+.2f %%); %d particles, %ld steps, %.1f s\n",
               n, vm, 100 * (vm / up - 1), pm * 1e-9, 100 * (pm / pH - 1), usm, 100 * (usm / us - 1), sph_count(p), sph_steps(p), wall() - c0);
        verdict(!sph_unstable(p) && n > 20 && fabs(vm / up - 1) < 0.02 && fabs(pm / pH - 1) < 0.03 && fabs(usm / us - 1) < 0.03, "S1");
        sph_free(p);
    }
    printf("== S2: copper sphere at 1 km/s into a copper plate, conservation\n");
    {
        double c0 = wall();
        SphSpec s;
        sph_spec_defaults(&s);
        copper(&s);
        s.dx = 0.25e-3;
        s.nplanes = 2;
        s.planes[0] = (SphPlane){0, 0.0, +1, false}, s.planes[1] = (SphPlane){1, 0.0, +1, false};
        char err[256];
        Sph *p = sph_create(&s, err, sizeof err);
        double c[3] = {0, 0, 2.0e-3 + 0.5 * s.dx}, v[3] = {0, 0, -1000}, lo[3] = {0, 0, -2e-3}, hi[3] = {6e-3, 6e-3, 0}, z[3] = {0, 0, 0};
        sph_add_sphere(p, c, 2e-3, 0, v);
        sph_add_box(p, lo, hi, 0, z);
        double P0[3], k0, u0;
        sph_momentum(p, -1, P0);
        sph_energy(p, &k0, &u0);
        while (sph_time(p) < 4e-6 && !sph_unstable(p)) sph_step(p);
        double P1[3], k1, u1;
        sph_momentum(p, -1, P1);
        sph_energy(p, &k1, &u1);
        double dP = fabs(P1[2] - P0[2]) / fabs(P0[2]), dE = (k1 + u1 - k0 - u0) / (k0 + u0);
        printf("  momentum along the impact: change %.2e of the initial; energy: kinetic %.4g J to %.4g J, internal %.4g J, total change %+.3f %%\n", dP,
               k0, k1, u1, 100 * dE);
        printf("  %d particles, %ld steps, %.1f s\n", sph_count(p), sph_steps(p), wall() - c0);
        verdict(!sph_unstable(p) && dP < 1e-9 && fabs(dE) < 0.01, "S2");
        sph_free(p);
    }
    printf("== S3: Taylor test, OFHC copper at 190 m/s, against the hexahedral solver\n");
    {
        double c0 = wall(), L, D;
        int np;
        long st;
        taylor(0.381e-3, -1, -1, -1, &L, &D, &np, &st);
        printf("  L / L0 %.4f (hexahedral 0.685, %+.2f %%), D / D0 %.3f (hexahedral 1.91, recorded); %d particles, %ld steps, %.1f s\n", L,
               100 * (L / 0.685 - 1), D, np, st, wall() - c0);
        printf("  recorded first run (2026-09-26, viscosity on every approaching pair): L / L0 0.7166 (+4.6 %%), D / D0 1.70; FAIL\n");
        verdict(st > 0 && fabs(L / 0.685 - 1) < 0.03, "S3");
    }
    printf(failures ? "sphtest: %d FAILED\n" : "sphtest: all passed\n", failures);
    return failures ? 1 : 0;
}
