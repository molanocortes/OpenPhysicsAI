/* cellstifftest.c - GOALS.md G23 step 5a, H1 and H2: support_cell_stiffness (src/mech/support_cell.h), the periodic cell
 * solver's entry for any cell given as voxel solid fractions, against closed forms.
 *
 *     make build/cellstifftest && ./build/cellstifftest      (make test-cad runs it)
 *
 * Criteria, written 2026-10-08 before the first run (GOALS.md G23 step 5a):
 * H1 a full cell gives the solid's isotropic matrix within 1e-9 relative
 * H2 a cell holding one solid plate parallel to xy over a quarter of its height: C11 = C22 = (1/4) / (1 - nu^2),
 *    C12 = (1/4) nu / (1 - nu^2) and C66 = (1/4) / (2 (1 + nu)) within 1e-6 relative, and C33, C13, C23, C44 and C55
 *    below 1e-6 (plane stress: the void carries nothing)
 * The criteria name the terms in the textbook order (4 yz, 5 zx, 6 xy); the solver's order is the project's, [xx, yy,
 * zz, xy, yz, zx] (src/fem/hex8.h). The first run read C66 and C44 at the textbook's places and failed both; the
 * indices below were corrected, the criteria are as written. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "src/mech/support_cell.h"

static int failures;

static void check(int ok, const char *what, double got, double want) {
    printf("  %s  %s: %.12g against %.12g\n", ok ? "ok  " : "FAIL", what, got, want);
    if (!ok) failures++;
}

static double rel(double a, double b) { return fabs(a - b) / fabs(b); }

int main(void) {
    const int n = 8, nz = 8;
    const double nu = 0.3;
    double *phi = malloc(sizeof(double) * n * n * nz), C[36];
    char err[300];
    int elements = 0;

    printf("H1 a full cell\n");
    for (int v = 0; v < n * n * nz; v++) phi[v] = 1;
    if (!support_cell_stiffness(phi, n, nz, 1.0, 1.0, 1, nu, C, &elements, err, sizeof err)) {
        printf("  FAIL  %s\n", err);
        return 1;
    }
    double lam = nu / ((1 + nu) * (1 - 2 * nu)), mu = 1 / (2 * (1 + nu)), worst = 0;
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 6; j++) {
            double want = (i < 3 && j < 3) ? lam + (i == j ? 2 * mu : 0) : (i == j ? mu : 0);
            double scale = lam + 2 * mu, e = fabs(C[6 * i + j] - want) / scale;
            if (e > worst) worst = e;
        }
    check(worst < 1e-9, "the largest difference from the isotropic matrix, over C11", worst, 0);
    check(rel(C[0], lam + 2 * mu) < 1e-9, "C11", C[0], lam + 2 * mu);
    check(rel(C[1], lam) < 1e-9, "C12", C[1], lam);
    check(rel(C[21], mu) < 1e-9, "C44", C[21], mu);

    printf("H2 one plate over a quarter of the height\n");
    for (int k = 0; k < nz; k++)
        for (int v = 0; v < n * n; v++) phi[k * n * n + v] = k < nz / 4 ? 1 : 0;
    if (!support_cell_stiffness(phi, n, nz, 1.0, 1.0, 1, nu, C, &elements, err, sizeof err)) {
        printf("  FAIL  %s\n", err);
        return 1;
    }
    double c11 = 0.25 / (1 - nu * nu), c12 = 0.25 * nu / (1 - nu * nu), c66 = 0.25 / (2 * (1 + nu));
    check(rel(C[0], c11) < 1e-6, "C11", C[0], c11);
    check(rel(C[7], c11) < 1e-6, "C22", C[7], c11);
    check(rel(C[1], c12) < 1e-6, "C12", C[1], c12);
    check(rel(C[21], c66) < 1e-6, "C66 (xy, the solver's index 3)", C[21], c66);
    const int zero[5][2] = {{2, 2}, {0, 2}, {1, 2}, {4, 4}, {5, 5}};
    const char *names[5] = {"C33", "C13", "C23", "C44 (yz, the solver's index 4)", "C55 (zx, the solver's index 5)"};
    for (int q = 0; q < 5; q++) check(fabs(C[6 * zero[q][0] + zero[q][1]]) < 1e-6, names[q], C[6 * zero[q][0] + zero[q][1]], 0);
    free(phi);
    printf("cellstifftest: %s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
