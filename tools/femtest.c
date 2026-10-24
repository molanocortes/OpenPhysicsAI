/* femtest.c - numerical verification of the structural FEM kernel against exact and analytical solutions.
 *
 *  1. single distorted element: rigid-body modes (6 zero eigenvalues), constant-strain reproduction
 *  2. patch test: 2x2x2 elements with distorted interior nodes, linear displacement on the boundary
 *  3. uniaxial tension with roller constraints: exact displacements and stresses, direct and iterative solvers
 *  4. cantilever under an end shear load: shear locking of the standard element, incompatible modes vs Timoshenko,
 *     mesh convergence
 *  5. gravity load: reactions equal the weight
 *  6. initial (thermal) strain: stress-free free expansion, fully restrained hydrostatic stress
 *  7. constraint analysis: free bodies and hinge-connected regions are reported before solving
 *   make build/femtest && ./build/femtest */
#include "../src/fem/dense.h"
#include "../src/fem/hex8.h"
#include "../src/fem/solid.h"
#include "../src/fem/sparse.h"
#include "../src/threads.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (cond) g_pass++;                                                                                           \
        else {                                                                                                        \
            g_fail++;                                                                                                 \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                                                             \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
        }                                                                                                             \
    } while (0)

typedef struct {
    int nx, ny, nz, nn, ne;
    double *xyz;
    int *conn;
} Box;

static int bnode(const Box *b, int i, int j, int k) { return i + (b->nx + 1) * (j + (b->ny + 1) * k); }

static void box_make(Box *b, int nx, int ny, int nz, double lx, double ly, double lz) {
    b->nx = nx, b->ny = ny, b->nz = nz;
    b->nn = (nx + 1) * (ny + 1) * (nz + 1);
    b->ne = nx * ny * nz;
    b->xyz = malloc((size_t)b->nn * 3 * sizeof(double));
    b->conn = malloc((size_t)b->ne * 8 * sizeof(int));
    for (int k = 0; k <= nz; k++)
        for (int j = 0; j <= ny; j++)
            for (int i = 0; i <= nx; i++) {
                int n = bnode(b, i, j, k);
                b->xyz[3 * n] = lx * i / nx, b->xyz[3 * n + 1] = ly * j / ny, b->xyz[3 * n + 2] = lz * k / nz;
            }
    int e = 0;
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++, e++) {
                int *c = b->conn + 8 * e;
                c[0] = bnode(b, i, j, k), c[1] = bnode(b, i + 1, j, k), c[2] = bnode(b, i + 1, j + 1, k), c[3] = bnode(b, i, j + 1, k);
                c[4] = bnode(b, i, j, k + 1), c[5] = bnode(b, i + 1, j, k + 1), c[6] = bnode(b, i + 1, j + 1, k + 1), c[7] = bnode(b, i, j + 1, k + 1);
            }
}

static void box_free(Box *b) {
    free(b->xyz);
    free(b->conn);
}

static double lcg(unsigned *s) {
    *s = *s * 1664525u + 1013904223u;
    return ((*s >> 8) & 0xFFFFFF) / (double)0x1000000 * 2 - 1; /* [-1, 1) */
}

static void test_element(void) {
    printf("== 1. distorted element: rigid-body modes and constant strain\n");
    unsigned seed = 12345;
    double X[8][3];
    for (int a = 0; a < 8; a++)
        for (int k = 0; k < 3; k++) X[a][k] = 0.5 * HEX8_XI[a][k] + 0.12 * lcg(&seed);
    double D[6][6];
    isotropic_D(210e9, 0.3, D);
    for (int f = 0; f < 2; f++) {
        double Ke[576], mdj, w[24];
        CHECK(hex8_stiffness(X, D, (Hex8Formulation)f, Ke, &mdj), "stiffness f=%d", f);
        double sym = 0;
        for (int i = 0; i < 24; i++)
            for (int j = 0; j < 24; j++) sym = fmax(sym, fabs(Ke[i * 24 + j] - Ke[j * 24 + i]));
        CHECK(sym <= 1e-12 * fabs(Ke[0]), "symmetric (%g)", sym);
        double A[576];
        memcpy(A, Ke, sizeof A);
        dense_sym_eigen(A, 24, w, NULL);
        int zero = 0, neg = 0;
        for (int i = 0; i < 24; i++) {
            if (fabs(w[i]) <= 1e-9 * w[23]) zero++;
            else if (w[i] < 0) neg++;
        }
        CHECK(zero == 6 && neg == 0, "formulation %d: %d zero eigenvalues (want 6), %d negative, lambda7/lambda24 = %.3g", f, zero, neg, w[6] / w[23]);
        /* linear displacement field: exact constant strain at every Gauss point */
        double G[3][3] = {{1e-3, 2e-4, -3e-4}, {5e-4, -2e-3, 1e-4}, {-1e-4, 3e-4, 1.5e-3}};
        double ue[24];
        for (int a = 0; a < 8; a++)
            for (int i = 0; i < 3; i++) ue[3 * a + i] = G[i][0] * X[a][0] + G[i][1] * X[a][1] + G[i][2] * X[a][2] + 1e-3 * (i + 1);
        double exact[6] = {G[0][0], G[1][1], G[2][2], G[0][1] + G[1][0], G[1][2] + G[2][1], G[2][0] + G[0][2]};
        double eps[8][6], sig[8][6], maxe = 0;
        hex8_gauss_strain_stress(X, D, (Hex8Formulation)f, ue, NULL, eps, sig);
        for (int g = 0; g < 8; g++)
            for (int k = 0; k < 6; k++) maxe = fmax(maxe, fabs(eps[g][k] - exact[k]));
        CHECK(maxe < 1e-13, "formulation %d reproduces constant strain (error %.2e)", f, maxe);
    }
}

static void test_patch(void) {
    printf("== 2. patch test with distorted elements\n");
    Box b;
    box_make(&b, 2, 2, 2, 2, 2, 2);
    unsigned seed = 777;
    for (int k = 0; k <= 2; k++)
        for (int j = 0; j <= 2; j++)
            for (int i = 0; i <= 2; i++) {
                bool corner = (i != 1) + (j != 1) + (k != 1) == 3;
                if (corner) continue;
                int n = bnode(&b, i, j, k);
                for (int c = 0; c < 3; c++) b.xyz[3 * n + c] += 0.2 * lcg(&seed);
            }
    double G[3][3] = {{1e-3, 2e-4, -3e-4}, {5e-4, -2e-3, 1e-4}, {-1e-4, 3e-4, 1.5e-3}};
    unsigned char fixed[81];
    double val[81];
    for (int n = 0; n < b.nn; n++)
        for (int c = 0; c < 3; c++) {
            fixed[3 * n + c] = n != bnode(&b, 1, 1, 1);
            val[3 * n + c] = G[c][0] * b.xyz[3 * n] + G[c][1] * b.xyz[3 * n + 1] + G[c][2] * b.xyz[3 * n + 2];
        }
    SolidMaterial mat = {70e9, 0.33, 2700};
    double exact[6] = {G[0][0], G[1][1], G[2][2], G[0][1] + G[1][0], G[1][2] + G[2][1], G[2][0] + G[0][2]};
    for (int f = 0; f < 2; f++) {
        HexModel m = {b.nn, b.ne, b.xyz, b.conn, NULL, 1, &mat, (Hex8Formulation)f};
        SolidLoads L = {fixed, val, NULL, NULL, {0, 0, 0}};
        SolidResult r;
        char err[256];
        bool ok = solid_solve(&m, &L, NULL, &r, err, sizeof err);
        CHECK(ok, "solve: %s", err);
        if (!ok) continue;
        int c = bnode(&b, 1, 1, 1);
        double du = 0, de = 0;
        for (int k = 0; k < 3; k++) du = fmax(du, fabs(r.u[3 * c + k] - val[3 * c + k]));
        for (int e = 0; e < b.ne; e++)
            for (int g = 0; g < 8; g++)
                for (int k = 0; k < 6; k++) de = fmax(de, fabs(r.gp_strain[48 * e + 6 * g + k] - exact[k]));
        CHECK(du < 1e-14 && de < 1e-12, "formulation %d: interior node error %.2e m, strain error %.2e", f, du, de);
        solid_result_free(&r);
    }
    box_free(&b);
}

static void test_tension(ThreadPool *pool) {
    printf("== 3. uniaxial tension (exact)\n");
    Box b;
    const double Lx = 0.1, Ly = 0.02, Lz = 0.01, sigma = 50e6, E = 200e9, nu = 0.3;
    box_make(&b, 10, 4, 2, Lx, Ly, Lz);
    unsigned char *fixed = calloc((size_t)b.nn * 3, 1);
    double *force = calloc((size_t)b.nn * 3, sizeof(double));
    for (int k = 0; k <= b.nz; k++)
        for (int j = 0; j <= b.ny; j++)
            for (int i = 0; i <= b.nx; i++) {
                int n = bnode(&b, i, j, k);
                if (i == 0) fixed[3 * n] = 1;
                if (j == 0) fixed[3 * n + 1] = 1;
                if (k == 0) fixed[3 * n + 2] = 1;
            }
    /* consistent loads of the traction on x = Lx */
    for (int e = 0; e < b.ne; e++) {
        int i = e % b.nx;
        if (i != b.nx - 1) continue;
        double X[8][3], fe[24], area, nrm[3];
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) X[a][k] = b.xyz[3 * b.conn[8 * e + a] + k];
        double t[3] = {sigma, 0, 0};
        hex8_face_load(X, 3, t, fe, &area, nrm);
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) force[3 * b.conn[8 * e + a] + k] += fe[3 * a + k];
    }
    SolidMaterial mat = {E, nu, 7850};
    for (int solver = 1; solver <= 2; solver++) {
        HexModel m = {b.nn, b.ne, b.xyz, b.conn, NULL, 1, &mat, HEX8_INCOMPATIBLE};
        SolidLoads L = {fixed, NULL, force, NULL, {0, 0, 0}};
        SolidOptions o = {(SolidSolver)solver, 1e-12, 50000, 0, pool, NULL, NULL};
        SolidResult r;
        char err[256];
        bool ok = solid_solve(&m, &L, &o, &r, err, sizeof err);
        CHECK(ok, "solve: %s", err);
        if (!ok) continue;
        double eu = 0, es = 0;
        for (int n = 0; n < b.nn; n++) {
            double x = b.xyz[3 * n], y = b.xyz[3 * n + 1], z = b.xyz[3 * n + 2];
            double ux = sigma * x / E, uy = -nu * sigma * y / E, uz = -nu * sigma * z / E;
            eu = fmax(eu, fmax(fabs(r.u[3 * n] - ux), fmax(fabs(r.u[3 * n + 1] - uy), fabs(r.u[3 * n + 2] - uz))));
        }
        for (int e = 0; e < b.ne; e++)
            for (int g = 0; g < 8; g++) {
                const double *s = r.gp_stress + 48 * e + 6 * g;
                es = fmax(es, fabs(s[0] - sigma));
                for (int k = 1; k < 6; k++) es = fmax(es, fabs(s[k]));
            }
        double umax = sigma * Lx / E;
        CHECK(eu < 1e-8 * umax && es < 1e-8 * sigma, "%s: displacement error %.2e of %.3g m, stress error %.2e Pa (%d iterations, residual %.1e)",
              r.stats.method, eu, umax, es, r.stats.iterations, r.stats.true_residual);
        CHECK(fabs(r.reaction_total[0] + sigma * Ly * Lz) < 1e-8 * sigma * Ly * Lz, "reaction %.6g N balances %.6g N", r.reaction_total[0], sigma * Ly * Lz);
        CHECK(r.equilibrium_error < 1e-9, "equilibrium error %.2e", r.equilibrium_error);
        CHECK(fabs(r.external_work - 2 * r.strain_energy) < 1e-8 * r.external_work, "work %.6g J = 2 x strain energy %.6g J", r.external_work, r.strain_energy);
        solid_result_free(&r);
    }
    free(fixed), free(force);
    box_free(&b);
}

static double cantilever(int nx, int ny, int nz, Hex8Formulation f, ThreadPool *pool, SolidSolver solver, double *seconds) {
    const double L = 10, h = 1, w = 1, P = 1e-3, E = 1, nu = 0;
    Box b;
    box_make(&b, nx, ny, nz, L, w, h);
    unsigned char *fixed = calloc((size_t)b.nn * 3, 1);
    double *force = calloc((size_t)b.nn * 3, sizeof(double));
    for (int n = 0; n < b.nn; n++)
        if (b.xyz[3 * n] == 0) fixed[3 * n] = fixed[3 * n + 1] = fixed[3 * n + 2] = 1;
    for (int e = 0; e < b.ne; e++) {
        if (e % nx != nx - 1) continue;
        double X[8][3], fe[24], area, nrm[3];
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) X[a][k] = b.xyz[3 * b.conn[8 * e + a] + k];
        double t[3] = {0, 0, -P / (w * h)};
        hex8_face_load(X, 3, t, fe, &area, nrm);
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) force[3 * b.conn[8 * e + a] + k] += fe[3 * a + k];
    }
    SolidMaterial mat = {E, nu, 1};
    HexModel m = {b.nn, b.ne, b.xyz, b.conn, NULL, 1, &mat, f};
    SolidLoads Ld = {fixed, NULL, force, NULL, {0, 0, 0}};
    SolidOptions o = {solver, 1e-12, 100000, 0, pool, NULL, NULL};
    SolidResult r;
    char err[256];
    double tip = NAN;
    if (solid_solve(&m, &Ld, &o, &r, err, sizeof err)) {
        double s = 0;
        int cnt = 0;
        for (int n = 0; n < b.nn; n++)
            if (fabs(b.xyz[3 * n] - L) < 1e-12) s += r.u[3 * n + 2], cnt++;
        tip = -s / cnt;
        if (seconds) *seconds = r.stats.seconds;
        solid_result_free(&r);
    } else {
        printf("  cantilever solve failed: %s\n", err);
    }
    free(fixed), free(force);
    box_free(&b);
    return tip;
}

static void test_cantilever(ThreadPool *pool) {
    printf("== 4. cantilever, end shear load (L/h = 10, nu = 0)\n");
    const double L = 10, h = 1, w = 1, P = 1e-3, E = 1;
    double I = w * h * h * h / 12, G = E / 2, A = w * h, kappa = 5.0 / 6.0;
    double eb = P * L * L * L / (3 * E * I), timo = eb + P * L / (kappa * G * A);
    printf("  Euler-Bernoulli %.6g, Timoshenko %.6g\n", eb, timo);
    printf("  %-12s %-14s %-14s %-12s\n", "mesh", "full", "incompatible", "error (IM)");
    int meshes[4][3] = {{10, 1, 1}, {20, 2, 2}, {40, 4, 4}, {80, 8, 8}};
    double full[4], im[4];
    for (int i = 0; i < 4; i++) {
        full[i] = cantilever(meshes[i][0], meshes[i][1], meshes[i][2], HEX8_FULL, pool, SOLID_SOLVER_AUTO, NULL);
        im[i] = cantilever(meshes[i][0], meshes[i][1], meshes[i][2], HEX8_INCOMPATIBLE, pool, SOLID_SOLVER_AUTO, NULL);
        char name[32];
        snprintf(name, sizeof name, "%dx%dx%d", meshes[i][0], meshes[i][1], meshes[i][2]);
        printf("  %-12s %-14.6g %-14.6g %+.3f%%\n", name, full[i], im[i], 100 * (im[i] - timo) / timo);
    }
    CHECK(full[0] < 0.8 * timo, "standard hex8 locks on the coarse mesh (%.1f%% of Timoshenko)", 100 * full[0] / timo);
    CHECK(full[0] < full[1] && full[1] < full[2] && full[2] < full[3], "standard hex8 converges monotonically from below");
    CHECK(fabs(im[0] - timo) / timo < 0.03, "incompatible modes, one element through the thickness: within 3%% (%.2f%%)", 100 * (im[0] - timo) / timo);
    CHECK(fabs(im[3] - timo) / timo < 0.01, "incompatible modes, fine mesh within 1%% (%.2f%%)", 100 * (im[3] - timo) / timo);
    CHECK(fabs(im[3] - im[2]) < fabs(im[1] - im[0]) + 1e-12, "incompatible modes: successive differences shrink");
    double td, tp;
    double dd = cantilever(40, 4, 4, HEX8_INCOMPATIBLE, pool, SOLID_SOLVER_DIRECT, &td);
    double dp = cantilever(40, 4, 4, HEX8_INCOMPATIBLE, pool, SOLID_SOLVER_PCG, &tp);
    CHECK(fabs(dd - dp) < 1e-8 * dd, "direct (%.3f s) and PCG (%.3f s) agree: %.10g vs %.10g", td, tp, dd, dp);
}

static void test_gravity_thermal_constraints(ThreadPool *pool) {
    printf("== 5. gravity\n");
    Box b;
    box_make(&b, 3, 3, 6, 0.03, 0.03, 0.06);
    unsigned char *fixed = calloc((size_t)b.nn * 3, 1);
    for (int n = 0; n < b.nn; n++)
        if (b.xyz[3 * n + 2] == 0) fixed[3 * n] = fixed[3 * n + 1] = fixed[3 * n + 2] = 1;
    SolidMaterial mat = {70e9, 0.33, 2700};
    HexModel m = {b.nn, b.ne, b.xyz, b.conn, NULL, 1, &mat, HEX8_INCOMPATIBLE};
    SolidLoads L = {fixed, NULL, NULL, NULL, {0, 0, -9.81}};
    SolidResult r;
    char err[512];
    bool ok = solid_solve(&m, &L, NULL, &r, err, sizeof err);
    double weight = 2700 * 9.81 * 0.03 * 0.03 * 0.06;
    CHECK(ok && fabs(r.reaction_total[2] - weight) < 1e-9 * weight, "base reaction %.9g N = weight %.9g N", ok ? r.reaction_total[2] : 0, weight);
    if (ok) solid_result_free(&r);

    printf("== 6. initial (thermal) strain\n");
    const double alpha = 23e-6, dT = 100;
    double *eps0 = calloc((size_t)b.ne * 6, sizeof(double));
    for (int e = 0; e < b.ne; e++) eps0[6 * e] = eps0[6 * e + 1] = eps0[6 * e + 2] = alpha * dT;
    /* free expansion: 3-2-1 statically determinate support */
    memset(fixed, 0, (size_t)b.nn * 3);
    int n0 = bnode(&b, 0, 0, 0), nxn = bnode(&b, 3, 0, 0), nyn = bnode(&b, 0, 3, 0);
    fixed[3 * n0] = fixed[3 * n0 + 1] = fixed[3 * n0 + 2] = 1;
    fixed[3 * nxn + 1] = fixed[3 * nxn + 2] = 1;
    fixed[3 * nyn + 2] = 1;
    L = (SolidLoads){fixed, NULL, NULL, eps0, {0, 0, 0}};
    ok = solid_solve(&m, &L, NULL, &r, err, sizeof err);
    CHECK(ok, "free expansion solve: %s", err);
    if (ok) {
        double smax = 0, eu = 0;
        for (int e = 0; e < b.ne * 48; e++) smax = fmax(smax, fabs(r.gp_stress[e]));
        for (int n = 0; n < b.nn; n++)
            for (int k = 0; k < 3; k++) eu = fmax(eu, fabs(r.u[3 * n + k] - alpha * dT * b.xyz[3 * n + k]));
        double scale = 70e9 * alpha * dT; /* the stress a fully restrained expansion would produce */
        double rmax = 0;
        for (int n = 0; n < b.nn * 3; n++) rmax = fmax(rmax, fabs(r.reaction[n]));
        CHECK(smax < 1e-10 * scale && eu < 1e-12, "stress-free expansion: max |stress| %.2e Pa (%.1e of E alpha dT), displacement error %.2e m", smax,
              smax / scale, eu);
        CHECK(rmax < 1e-10 * scale * 0.03 * 0.06, "no reactions for free expansion (largest %.2e N against a force scale of %.2e N)", rmax, scale * 0.03 * 0.06);
        CHECK(r.equilibrium_error < 1e-9, "free expansion equilibrium error %.2e", r.equilibrium_error);
        solid_result_free(&r);
    }
    /* fully restrained: rollers on all six faces */
    for (int n = 0; n < b.nn; n++) {
        double x = b.xyz[3 * n], y = b.xyz[3 * n + 1], z = b.xyz[3 * n + 2];
        fixed[3 * n] = x == 0 || fabs(x - 0.03) < 1e-12;
        fixed[3 * n + 1] = y == 0 || fabs(y - 0.03) < 1e-12;
        fixed[3 * n + 2] = z == 0 || fabs(z - 0.06) < 1e-12;
    }
    ok = solid_solve(&m, &L, NULL, &r, err, sizeof err);
    double expect = -70e9 * alpha * dT / (1 - 2 * 0.33);
    CHECK(ok, "restrained solve: %s", err);
    if (ok) {
        double es = 0;
        for (int e = 0; e < b.ne; e++)
            for (int g = 0; g < 8; g++) {
                const double *s = r.gp_stress + 48 * e + 6 * g;
                for (int k = 0; k < 3; k++) es = fmax(es, fabs(s[k] - expect));
                for (int k = 3; k < 6; k++) es = fmax(es, fabs(s[k]));
            }
        CHECK(es < 1e-8 * fabs(expect), "hydrostatic stress %.6g Pa (error %.2e Pa)", expect, es);
        CHECK(r.equilibrium_error < 1e-9, "restrained expansion equilibrium error %.2e", r.equilibrium_error);
        double U = 0.5 * 3 * expect * (-alpha * dT) * 0.03 * 0.03 * 0.06; /* 1/2 sigma:(0 - eps0) V */
        CHECK(fabs(r.strain_energy - U) < 1e-8 * U, "stored energy %.9g J = %.9g J", r.strain_energy, U);
        solid_result_free(&r);
    }
    free(eps0);

    printf("== 7. constraint analysis\n");
    memset(fixed, 0, (size_t)b.nn * 3);
    ConstraintReport rep;
    bool fine = solid_check_constraints(&m, fixed, &rep);
    CHECK(!fine && rep.nissues == 1 && rep.issue[0].free_modes == 6, "unsupported body: 6 free modes (%d)", rep.nissues ? rep.issue[0].free_modes : -1);
    for (int n = 0; n < b.nn; n++)
        if (b.xyz[3 * n + 2] == 0) fixed[3 * n + 2] = 1; /* only vertical support on the base: slides and spins */
    fine = solid_check_constraints(&m, fixed, &rep);
    char text[600];
    if (rep.nissues) constraint_issue_text(&rep.issue[0], text, sizeof text);
    CHECK(!fine && rep.nissues == 1 && rep.issue[0].free_modes == 3, "base on rollers: 3 free modes (%d): %s", rep.nissues ? rep.issue[0].free_modes : -1, rep.nissues ? text : "");
    fixed[3 * n0] = fixed[3 * n0 + 1] = 1;
    fixed[3 * nxn + 1] = 1;
    fine = solid_check_constraints(&m, fixed, &rep);
    CHECK(fine && rep.nissues == 0, "rollers plus a 3-2-1 set: fully constrained");
    free(fixed);
    box_free(&b);

    /* two blocks touching along one edge: the second hangs on a hinge */
    Box h;
    box_make(&h, 2, 1, 1, 2, 1, 1);
    /* move element 1 so it shares only the edge (x=1, z=1) with element 0: rebuild its nodes */
    int nn = h.nn + 6;
    double *xyz = malloc((size_t)nn * 3 * sizeof(double));
    memcpy(xyz, h.xyz, (size_t)h.nn * 3 * sizeof(double));
    int conn[16];
    memcpy(conn, h.conn, 8 * sizeof(int));
    /* element 1 occupies x 1..2, z 1..2, sharing nodes (1,0,1) and (1,1,1) with element 0 */
    int shared0 = bnode(&h, 1, 0, 1), shared1 = bnode(&h, 1, 1, 1);
    const double pts[6][3] = {{2, 0, 1}, {2, 1, 1}, {1, 0, 2}, {2, 0, 2}, {2, 1, 2}, {1, 1, 2}};
    for (int i = 0; i < 6; i++)
        for (int k = 0; k < 3; k++) xyz[3 * (h.nn + i) + k] = pts[i][k];
    int e1[8] = {shared0, h.nn + 0, h.nn + 1, shared1, h.nn + 2, h.nn + 3, h.nn + 4, h.nn + 5};
    memcpy(conn + 8, e1, sizeof e1);
    unsigned char *fx = calloc((size_t)nn * 3, 1);
    for (int n = 0; n < h.nn; n++)
        if (xyz[3 * n + 2] == 0 && xyz[3 * n] <= 1) fx[3 * n] = fx[3 * n + 1] = fx[3 * n + 2] = 1;
    SolidMaterial mm = {1e9, 0.3, 1000};
    HexModel hm = {nn, 2, xyz, conn, NULL, 1, &mm, HEX8_INCOMPATIBLE};
    fine = solid_check_constraints(&hm, fx, &rep);
    if (rep.nissues) constraint_issue_text(&rep.issue[0], text, sizeof text);
    CHECK(!fine && rep.regions == 2 && rep.nissues == 1 && rep.issue[0].weak_links > 0, "edge-connected block reported as a hinge: %s", rep.nissues ? text : "(none)");
    double *zero = calloc((size_t)nn * 3, sizeof(double));
    SolidLoads hl = {fx, NULL, zero, NULL, {0, 0, -9.81}};
    SolidOptions so = {SOLID_SOLVER_DIRECT, 0, 0, 0, pool, NULL, NULL};
    ok = solid_solve(&hm, &hl, &so, &r, err, sizeof err);
    CHECK(!ok && strstr(err, "singular"), "solver refuses the mechanism: %s", err);
    if (ok) solid_result_free(&r);
    free(zero), free(fx), free(xyz);
    box_free(&h);
}

int main(void) {
    ThreadPool *pool = pool_create(cpu_perf_count() > 0 ? cpu_perf_count() : 4);
    test_element();
    test_patch();
    test_tension(pool);
    test_cantilever(pool);
    test_gravity_thermal_constraints(pool);
    pool_destroy(pool);
    printf("\n%s: %d passed, %d failed\n", g_fail ? "FEM VERIFICATION FAILED" : "ALL FEM VERIFICATION TESTS PASSED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
