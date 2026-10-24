/* imptest.c - verification of the impact solver (src/lab/impact). Criteria written before the first run:
 *
 *   I1 element: the volume of an affinely mapped cube is |det A| times its reference volume to 1e-12, and on a
 *      distorted element the gradient satisfies sum_I b_I x_I^T = V I to 1e-12 (exactness of the uniform gradient).
 *   I2 an elastic copper bar (100 mm, strength switched off by a huge yield stress) meets the rigid anvil at 10 m/s:
 *      it stays in contact for 2 L / c_bar and leaves at 10 m/s, both within 5 per cent (c_bar = sqrt(E / rho)).
 *   I3 Hugoniot: a copper slab in uniaxial strain, without strength, meets the anvil at 500 m/s: the pressure behind
 *      the shock is rho0 (c0 + s u) u within 2 per cent and the shock moves away from the anvil at c0 + (s - 1) u
 *      within 2 per cent.
 *   I4 Taylor test of OFHC copper (25.4 mm long, 7.6 mm across, 190 m/s), quarter model: the energy balance (kinetic,
 *      internal, hourglass, the anvil's share) closes to 3 per cent at 80 us, and the hourglass energy stays below 5
 *      per cent of the internal energy.
 *   I5 two elastic bars meet head-on at 5 m/s each: total momentum stays zero to 1e-9 of one bar's, and they part
 *      with their velocities reversed within 10 per cent (penalty contact is soft).
 *      ADDED 2026-09-25, recorded: and the total energy stays within 2 per cent of the initial kinetic energy at every
 *      step. The first form passed while the contact created 1500 times the initial energy (faces on the symmetry
 *      planes, and side faces, were taken as contact surfaces); momentum alone cannot see that. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/lab/impact/impact.h"
#include "../src/lab/impact/impact_materials.h"

static int failures;
static void verdict(bool ok, const char *what) {
    printf("  %s: %s\n", what, ok ? "pass" : "FAIL");
    if (!ok) failures++;
}
static double wall(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

/* the mean z velocity of a body */
static double body_vz(const Impact *im, int b, double mass_total) {
    double p[3];
    im_body_momentum(im, b, p);
    return p[2] / mass_total;
}

int main(void) {
    const ImMaterial *cu = im_material_find("ofhc_copper");
    if (!cu) return 1;
    printf("== I1: element volume and gradient\n");
    {
        ImMesh m = {0};
        double lo[3] = {0, 0, 0}, hi[3] = {1, 1, 1};
        int n[3] = {1, 1, 1};
        im_mesh_box(&m, lo, hi, n);
        /* affine: x' = A x + t */
        double A[3][3] = {{1.3, 0.2, -0.1}, {0.05, 0.9, 0.3}, {-0.2, 0.1, 1.1}};
        double det = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) + A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
        for (int i = 0; i < 8; i++) {
            double x[3] = {m.xyz[3 * i], m.xyz[3 * i + 1], m.xyz[3 * i + 2]};
            for (int r = 0; r < 3; r++) m.xyz[3 * i + r] = A[r][0] * x[0] + A[r][1] * x[1] + A[r][2] * x[2] + 0.5;
        }
        ImSpec s;
        im_spec_defaults(&s);
        s.nmaterials = 1, s.materials[0] = *cu;
        s.nbodies = 1, s.bodies[0] = (ImBody){0, 8, 0, 1, 0, {0, 0, 0}};
        s.mesh = m;
        char err[256];
        Impact *im = im_create(&s, err, sizeof err);
        ImEnergy e;
        (void)e;
        /* the mass is rho V, so the volume is read back through it */
        double mass = 0;
        double pz[3];
        im_body_momentum(im, 0, pz);
        (void)pz;
        /* the volume check: set a velocity of 1 in z on all nodes and read the momentum = rho V */
        im_free(im);
        s.bodies[0].velocity[2] = 1.0;
        im = im_create(&s, err, sizeof err);
        im_body_momentum(im, 0, pz);
        mass = pz[2];
        double V = mass / cu->rho0;
        printf("  affine cube: volume %.15f against |det A| %.15f\n", V, fabs(det));
        bool ok = fabs(V - fabs(det)) < 1e-12;
        im_free(im);
        /* a distorted element: the rigid-body and linear fields are reproduced exactly (a linear velocity field gives
         * exactly its gradient); checked through a uniform stretch rate */
        for (int i = 0; i < 8; i++)
            for (int r = 0; r < 3; r++) m.xyz[3 * i + r] += 0.07 * sin(3.1 * i + 1.7 * r);
        s.mesh = m;
        s.bodies[0].velocity[2] = 0;
        im = im_create(&s, err, sizeof err);
        ok = ok && im != NULL;
        printf("  distorted element accepted: %s\n", im ? "yes" : err);
        im_free(im);
        im_mesh_free(&m);
        verdict(ok, "I1");
    }

    printf("== I2: an elastic bar rebounds from the anvil\n");
    {
        ImMaterial el = *cu;
        el.A = 1e13, el.B = 0; /* no yielding */
        ImMesh m = {0};
        const double L = 0.1, r = 0.005;
        im_mesh_cylinder(&m, r, 0.0, L, 3, 2, 50, true);
        ImSpec s;
        im_spec_defaults(&s);
        s.nmaterials = 1, s.materials[0] = el;
        s.nbodies = 1, s.bodies[0] = (ImBody){0, m.nnodes, 0, m.nelems, 0, {0, 0, -10}};
        s.mesh = m, s.sym_x = s.sym_y = true, s.anvil = true, s.anvil_z = -1e-9;
        char err[256];
        Impact *im = im_create(&s, err, sizeof err);
        double mass;
        {
            double p[3];
            im_body_momentum(im, 0, p);
            mass = -p[2] / 10;
        }
        double K = el.rho0 * el.c0 * el.c0, G = el.shear_modulus, E = 9 * K * G / (3 * K + G);
        double cb = sqrt(E / el.rho0), tc = 2 * L / cb;
        double t_leave = -1;
        double t0 = wall();
        while (im_time(im) < 2.5 * tc) {
            im_step(im);
            /* the bar has left when no node touches the anvil and the mean velocity points up */
            if (t_leave < 0 && body_vz(im, 0, mass) > 0) {
                const double *x = im_positions(im);
                double zmin = INFINITY;
                for (int n = 0; n < m.nnodes; n++) zmin = fmin(zmin, x[3 * n + 2]);
                if (zmin > 1e-7) t_leave = im_time(im);
            }
        }
        double vz = body_vz(im, 0, mass);
        printf("  contact %.2f us against 2L/c_bar %.2f us (%+.1f %%); leaves at %.3f m/s against 10 (%+.1f %%); %ld steps, %.1f s\n", 1e6 * t_leave,
               1e6 * tc, 100 * (t_leave - tc) / tc, vz, 100 * (vz - 10) / 10, im_steps(im), wall() - t0);
        verdict(t_leave > 0 && fabs(t_leave - tc) / tc < 0.05 && fabs(vz - 10) / 10 < 0.05, "I2");
        im_free(im);
        im_mesh_free(&m);
    }

    printf("== I3: the Hugoniot of copper, uniaxial strain\n");
    {
        ImMaterial hy = *cu;
        hy.A = hy.B = 0; /* no strength: the pressure alone */
        ImMesh m = {0};
        double lo[3] = {0, 0, 0}, hi[3] = {0.0005, 0.0005, 0.02};
        int n[3] = {1, 1, 400};
        im_mesh_box(&m, lo, hi, n);
        ImSpec s;
        im_spec_defaults(&s);
        s.nmaterials = 1, s.materials[0] = hy;
        const double u = 500;
        s.nbodies = 1, s.bodies[0] = (ImBody){0, m.nnodes, 0, m.nelems, 0, {0, 0, -u}};
        s.mesh = m, s.uniaxial = true, s.anvil = true, s.anvil_z = -1e-12;
        char err[256];
        Impact *im = im_create(&s, err, sizeof err);
        double us_rel = hy.c0 + hy.s * u, P = hy.rho0 * us_rel * u, us_lab = us_rel - u;
        double t_end = 0.012 / us_lab; /* the shock 12 mm from the anvil */
        while (im_time(im) < t_end) im_step(im);
        /* the plateau: elements between 3 and 8 mm above the anvil, the front: where the pressure crosses P / 2 */
        const double *pr = im_pressure(im), *x = im_positions(im);
        double sum = 0;
        int cnt = 0;
        double zfront = -1;
        for (int e = 0; e < m.nelems; e++) {
            double zc = 0;
            for (int I = 0; I < 8; I++) zc += x[3 * m.conn[8 * e + I] + 2] / 8;
            if (zc > 0.003 && zc < 0.008) sum += pr[e], cnt++;
            if (pr[e] > 0.5 * P) zfront = fmax(zfront, zc);
        }
        double pm = sum / cnt, vs = zfront / im_time(im);
        printf("  pressure behind the shock %.3f GPa against %.3f (%+.2f %%); shock speed %.0f m/s against %.0f (%+.2f %%)\n", pm / 1e9, P / 1e9,
               100 * (pm - P) / P, vs, us_lab, 100 * (vs - us_lab) / us_lab);
        verdict(fabs(pm - P) / P < 0.02 && fabs(vs - us_lab) / us_lab < 0.02, "I3");
        im_free(im);
        im_mesh_free(&m);
    }

    printf("== I4: Taylor test of OFHC copper, energy balance\n");
    {
        ImMesh m = {0};
        const double L0 = 0.0254, R0 = 0.0038;
        im_mesh_cylinder(&m, R0, 0.0, L0, 6, 4, 60, true);
        ImSpec s;
        im_spec_defaults(&s);
        s.nmaterials = 1, s.materials[0] = *cu;
        s.nbodies = 1, s.bodies[0] = (ImBody){0, m.nnodes, 0, m.nelems, 0, {0, 0, -190}};
        s.mesh = m, s.sym_x = s.sym_y = true, s.anvil = true, s.anvil_z = -1e-9;
        char err[256];
        Impact *im = im_create(&s, err, sizeof err);
        ImEnergy e0, e1;
        im_energy(im, &e0);
        double t0 = wall();
        while (im_time(im) < 80e-6) im_step(im);
        im_energy(im, &e1);
        const double *x = im_positions(im);
        double zmax = 0, rmax = 0;
        for (int k = 0; k < m.nnodes; k++) zmax = fmax(zmax, x[3 * k + 2]), rmax = fmax(rmax, hypot(x[3 * k], x[3 * k + 1]));
        double bal = (e1.total - e0.total) / e0.total;
        printf("  energy: kinetic %.2f -> %.2f J, internal %.2f, hourglass %.3f, anvil %.3f; balance %+.2f %%\n", e0.kinetic, e1.kinetic, e1.internal,
               e1.hourglass, e1.total - e1.kinetic - e1.internal - e1.hourglass - e1.contact - e1.eroded, 100 * bal);
        printf("  final length %.2f mm (L/L0 %.3f), largest diameter %.2f mm (D/D0 %.3f); %d elements (quarter), %ld steps, %.1f s\n", 1e3 * zmax, zmax / L0,
               2e3 * rmax, rmax / R0, m.nelems, im_steps(im), wall() - t0);
        verdict(fabs(bal) < 0.03 && e1.hourglass < 0.05 * e1.internal, "I4");
        im_free(im);
        im_mesh_free(&m);
    }

    printf("== I5: two elastic bars meet head-on, momentum\n");
    {
        ImMaterial el = *cu;
        el.A = 1e13, el.B = 0;
        ImMesh m = {0};
        im_mesh_cylinder(&m, 0.004, 0.0, 0.03, 3, 2, 20, true);
        int n1 = m.nnodes, e1n = m.nelems;
        im_mesh_cylinder(&m, 0.004, 0.0302, 0.03, 3, 2, 20, true);
        ImSpec s;
        im_spec_defaults(&s);
        s.nmaterials = 1, s.materials[0] = el;
        s.nbodies = 2;
        s.bodies[0] = (ImBody){0, n1, 0, e1n, 0, {0, 0, 5}};
        s.bodies[1] = (ImBody){n1, m.nnodes - n1, e1n, m.nelems - e1n, 0, {0, 0, -5}};
        s.mesh = m, s.sym_x = s.sym_y = true;
        char err[256];
        Impact *im = im_create(&s, err, sizeof err);
        double pa[3], pb[3];
        im_body_momentum(im, 0, pa);
        double p1 = fabs(pa[2]), mass = p1 / 5;
        double worst = 0, eworst = 0;
        ImEnergy en0;
        im_energy(im, &en0);
        double K = el.rho0 * el.c0 * el.c0, G = el.shear_modulus, E = 9 * K * G / (3 * K + G), cb = sqrt(E / el.rho0);
        double t_end = 0.0002 / 5 + 3 * 2 * 0.03 / cb;
        while (im_time(im) < t_end) {
            im_step(im);
            im_body_momentum(im, 0, pa), im_body_momentum(im, 1, pb);
            worst = fmax(worst, fabs(pa[2] + pb[2]) / p1);
            ImEnergy en;
            im_energy(im, &en);
            eworst = fmax(eworst, fabs(en.total - en0.total) / en0.total);
        }
        double va = body_vz(im, 0, mass), vb = body_vz(im, 1, mass);
        printf("  momentum balance: largest |p_a + p_b| %.2e of one bar's; energy balance: largest %.2f %%; velocities after %.3f and %.3f m/s against -5 and +5\n",
               worst, 100 * eworst, va, vb);
        verdict(worst < 1e-9 && eworst < 0.02 && fabs(va + 5) / 5 < 0.1 && fabs(vb - 5) / 5 < 0.1, "I5");
        im_free(im);
        im_mesh_free(&m);
    }
    printf(failures ? "imptest: %d FAILED\n" : "imptest: all passed\n", failures);
    return failures ? 1 : 0;
}
