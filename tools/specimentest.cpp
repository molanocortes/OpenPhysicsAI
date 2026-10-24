// specimentest.cpp - GOALS.md G23 step 5c, S1 and S2: specimen_compress (src/cad/specimen.h) against closed forms.
//
//     make build/specimentest && ./build/specimentest      (make test-cad runs it)
//
// Criteria, written 2026-10-08 before the first run (GOALS.md G23 step 5c):
// S1 a full cube between frictionless platens gives E_s within 1e-9 relative (uniaxial stress), and between bonded
//    platens more than E_s
// S2 one wall parallel to the load, 4 voxels thick, between frictionless platens gives its thickness over the cube's
//    width times E_s within 1e-6 relative
#include "../src/cad/specimen.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int failures;

static void check(bool ok, const char *what, double got, double want) {
    std::printf("  %s  %s: %.12g against %.12g\n", ok ? "ok  " : "FAIL", what, got, want);
    if (!ok) failures++;
}

int main() {
    std::string why;
    SpecimenResult r;
    std::printf("S1 a full cube\n");
    int n = 6;
    std::vector<double> phi((size_t)n * n * n, 1.0);
    if (!specimen_compress(phi.data(), n, n, n, 1.0, 0.3, false, 5000000, &r, why)) std::printf("  FAIL  %s\n", why.c_str()), failures++;
    else check(std::fabs(r.modulus_over_Es - 1) < 1e-9, "frictionless platens: E / E_s", r.modulus_over_Es, 1);
    if (!specimen_compress(phi.data(), n, n, n, 1.0, 0.3, true, 5000000, &r, why)) std::printf("  FAIL  %s\n", why.c_str()), failures++;
    else check(r.modulus_over_Es > 1, "bonded platens: E / E_s above 1", r.modulus_over_Es, 1);
    std::printf("S2 one wall along the load\n");
    n = 12;
    phi.assign((size_t)n * n * n, 0.0);
    for (int k = 0; k < n; k++)
        for (int j = 4; j < 8; j++)
            for (int i = 0; i < n; i++) phi[((size_t)k * n + j) * n + i] = 1;
    if (!specimen_compress(phi.data(), n, n, n, 1.0, 0.3, false, 5000000, &r, why)) std::printf("  FAIL  %s\n", why.c_str()), failures++;
    else check(std::fabs(r.modulus_over_Es - 4.0 / 12) / (4.0 / 12) < 1e-6, "frictionless platens: E / E_s", r.modulus_over_Es, 4.0 / 12);
    std::printf("specimentest: %s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
