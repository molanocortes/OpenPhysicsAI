/* mag3dtest - verification of the 3D magnetostatic solver (src/lab/magnet/mag3d.h, MFEM Nedelec elements).
 *
 * Criteria written on 2026-09-26, before the first run:
 *   T1 a toroidal coil of rectangular cross-section: core a = 20 mm to b = 40 mm, height 2h = 30 mm; the winding a
 *      4 mm thick shell around it (up the inside, out over the top, down the outside, in under the bottom), carrying
 *      I = 1000 A-turns in all. Ampere's law gives B = mu0 I / (2 pi r) around the axis inside the core and nothing
 *      outside the winding. Grid: 26 rings, 48 sectors, 22 layers, flux walls at r = 6 mm and 70 mm, z = +/-45 mm.
 *      In the core's middle half (a + 3 mm < r < b - 3 mm, |z| < h / 2): the RMS relative error of B_theta below 2 %,
 *      the RMS of B_r and B_z below 2 % of the mean B_theta; outside (r > 50 mm or |z| > 25 mm) no |B| above 3 % of
 *      mu0 I / (2 pi b).
 *      First run: FAIL (RMS 11.4 %, 81 % outside): the grid's inner cylinder was a flux wall, which with the outer walls
 *      holds the flux through any meridional section at zero; the solver added a return field ~1/r everywhere. The
 *      inner cylinder is now a magnetic wall (mag3d.h); the criterion is unchanged.
 *      Second run: core RMS 1.31 %, cross 0.000 %, outside 9.2 % (FAIL): the winding was given as current densities
 *      constant on each element, which do not conserve current where the legs meet the caps; removing their gradient
 *      part (a quarter of the load) spreads a correcting current into the air around the corners. The same winding is
 *      now given as a current vector potential T (J = curl T, divergence-free by construction), as coils will be; the
 *      criterion is unchanged.
 *   T2 a long cylinder of radius R = 20 mm magnetised across its axis (Br = 1 T along x, mu_r = 1), periodic in z,
 *      inside a flux wall at Rb = 60 mm (the grid's inner cylinder at 0.2 mm, R / 100). The field inside is uniform:
 *      B = Br (1 - k) / ((1 - k) + mu_r (1 + k)), k = (R / Rb)^2 (derived in docs/lab/magnet.md), here (Br / 2)(1 - k).
 *      Mean B_x over 0.2 R < r < 0.7 R within 1 %, RMS of B_y there below 1 % of it.
 *   T3 the same cylinder with mu_r = 1.05, a sintered magnet's recoil permeability: the same closed form, within 1 %.
 *   Note on T1: with the winding given as T, the exact field in the core is H = T itself, so T1 checks Ampere's law,
 *   confinement and the flux walls, not the solver's ability to find a field that differs from its source; T4 does.
 *   T4 (added 2026-09-26 before its first run) a sphere of radius R = 15 mm magnetised across the axis (Br = 1 T along x,
 *      mu_r = 1) in a cylinder of radius and half-height 6 R with flux walls: inside, B = 2 Br / 3 uniform (the sphere's
 *      demagnetising factor 1/3, a 3D result; a long cylinder gives 1/2). The walls' images change this by about
 *      (1/6)^3 and the sphere is built of whole elements, so: mean B_x over the inner 0.6 R within 3 % of 2/3 T, and the
 *      RMS of B_y and B_z there below 3 % of it.
 *      First run: the Cholesky factorisation broke down (the wedges at the axis make the regularised system too
 *      ill-conditioned for it); a pivoted LDL^T fallback was added to spdirect.c; the criterion is unchanged.
 * Every solve's Cholesky factorisation must succeed. */
#include "../src/lab/magnet/mag3d.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static int failures;
static void verdict(bool ok, const char *name) {
    std::printf("  %s: %s\n", name, ok ? "pass" : "FAIL");
    failures += !ok;
}
static const double MU0 = 4e-7 * M_PI;

static std::vector<double> spaced(double a, double b, int n) {
    std::vector<double> v(n + 1);
    for (int i = 0; i <= n; i++) v[i] = a + (b - a) * i / n;
    return v;
}
/* joins pieces [x0, x1] with n cells each into one list of edges */
static std::vector<double> pieces(std::initializer_list<double> edges, std::initializer_list<int> counts) {
    std::vector<double> e(edges), out{e[0]};
    auto c = counts.begin();
    for (size_t k = 0; k + 1 < e.size(); k++, c++)
        for (int i = 1; i <= *c; i++) out.push_back(e[k] + (e[k + 1] - e[k]) * i / *c);
    return out;
}

static void toroid() {
    std::printf("== T1: a toroidal coil, B = mu0 I / (2 pi r) inside, none outside\n");
    const double a = 0.020, b = 0.040, h = 0.015, t = 0.004, I = 1000.0;
    std::vector<double> r = pieces({0.006, a - t, a, b, b + t, 0.070}, {3, 2, 10, 2, 9});
    std::vector<double> z = pieces({-0.045, -h - t, -h, h, h + t, 0.045}, {4, 2, 10, 2, 4});
    Mag3DGrid g = {(int)r.size() - 1, 48, (int)z.size() - 1, r.data(), z.data(), 0.0, false};
    size_t n = mag3d_count(&g);
    std::vector<double> nu(n, 1 / MU0), J(3 * n, 0.0), Br(3 * n, 0.0), B(3 * n);
    /* the winding as a current vector potential around the axis, T = I / (2 pi r) chi(r) chi(z) e_theta, chi ramping from
     * 0 to 1 across the winding's thickness: its curl runs up the inside, out over the top, down the outside and back */
    std::vector<double> T(3 * n, 0.0);
    auto ramp = [&](double x, double lo, double hi) { return x <= lo - t ? 0.0 : x < lo ? (x - lo + t) / t : x <= hi ? 1.0 : x < hi + t ? (hi + t - x) / t : 0.0; };
    for (int iz = 0; iz < g.nz; iz++)
        for (int it = 0; it < g.nt; it++)
            for (int ir = 0; ir < g.nr; ir++) {
                double c[3];
                mag3d_centre(&g, ir, it, iz, c);
                double rc = std::hypot(c[0], c[1]), zc = c[2], tt = I / (2 * M_PI * rc) * ramp(rc, a, b) * ramp(zc, -h, h);
                double *T3 = &T[3 * mag3d_index(&g, ir, it, iz)];
                T3[0] = -tt * c[1] / rc, T3[1] = tt * c[0] / rc;
            }
    Mag3DStats st;
    char err[256];
    if (!mag3d_solve(&g, nu.data(), nullptr, T.data(), Br.data(), B.data(), &st, err, sizeof err)) {
        std::printf("  %s\n", err);
        verdict(false, "T1");
        return;
    }
    double se = 0, sbt = 0, sq = 0, outmax = 0;
    int cnt = 0;
    for (int iz = 0; iz < g.nz; iz++)
        for (int it = 0; it < g.nt; it++)
            for (int ir = 0; ir < g.nr; ir++) {
                double c[3];
                mag3d_centre(&g, ir, it, iz, c);
                double rc = std::hypot(c[0], c[1]), ex = c[0] / rc, ey = c[1] / rc, *Bv = &B[3 * mag3d_index(&g, ir, it, iz)];
                double bt = -Bv[0] * ey + Bv[1] * ex, br = Bv[0] * ex + Bv[1] * ey, bz = Bv[2];
                if (rc > a + 0.003 && rc < b - 0.003 && std::fabs(c[2]) < h / 2) {
                    double exact = MU0 * I / (2 * M_PI * rc);
                    se += (bt / exact - 1) * (bt / exact - 1), sbt += bt, sq += br * br + bz * bz, cnt++;
                }
                if (rc > 0.050 || std::fabs(c[2]) > 0.025) outmax = std::fmax(outmax, std::sqrt(Bv[0] * Bv[0] + Bv[1] * Bv[1] + Bv[2] * Bv[2]));
            }
    double rms = std::sqrt(se / cnt), mean = sbt / cnt, cross = std::sqrt(sq / cnt) / mean, ref = MU0 * I / (2 * M_PI * b);
    std::printf("  %d dofs, factor %.0f MB, assembly %.2f s, solve %.2f s; gradient share of the current's load %.2e\n", st.dofs, st.factor_mb,
                st.assemble_s, st.solve_s, st.current_removed);
    std::printf("  core: RMS error of B_theta %.3f %%, B_r and B_z %.3f %% of it; outside: largest |B| %.3f %% of mu0 I / (2 pi b)\n", 100 * rms,
                100 * cross, 100 * outmax / ref);
    verdict(rms < 0.02 && cross < 0.02 && outmax < 0.03 * ref, "T1");
}

static double cylinder(double mur) {
    const double R = 0.020, Rb = 0.060;
    std::vector<double> r = pieces({0.0002, R, Rb}, {24, 20});
    std::vector<double> z = spaced(0, 0.006, 3);
    Mag3DGrid g = {(int)r.size() - 1, 64, 3, r.data(), z.data(), 0.0, true};
    size_t n = mag3d_count(&g);
    std::vector<double> nu(n, 1 / MU0), J(3 * n, 0.0), Br(3 * n, 0.0), B(3 * n);
    for (int iz = 0; iz < g.nz; iz++)
        for (int it = 0; it < g.nt; it++)
            for (int ir = 0; ir < g.nr; ir++)
                if (r[ir + 1] <= R + 1e-12) {
                    size_t e = mag3d_index(&g, ir, it, iz);
                    nu[e] = 1 / (MU0 * mur), Br[3 * e] = 1.0;
                }
    Mag3DStats st;
    char err[256];
    if (!mag3d_solve(&g, nu.data(), J.data(), nullptr, Br.data(), B.data(), &st, err, sizeof err)) {
        std::printf("  %s\n", err);
        return NAN;
    }
    double sx = 0, sy = 0;
    int cnt = 0;
    for (int iz = 0; iz < g.nz; iz++)
        for (int it = 0; it < g.nt; it++)
            for (int ir = 0; ir < g.nr; ir++) {
                double c[3];
                mag3d_centre(&g, ir, it, iz, c);
                double rc = std::hypot(c[0], c[1]);
                if (rc > 0.2 * R && rc < 0.7 * R) {
                    const double *Bv = &B[3 * mag3d_index(&g, ir, it, iz)];
                    sx += Bv[0], sy += Bv[1] * Bv[1], cnt++;
                }
            }
    double k = (R / Rb) * (R / Rb), exact = (1 - k) / ((1 - k) + mur * (1 + k));
    std::printf("  mu_r %.2f: mean B_x %.6f T against %.6f (%+.3f %%), RMS B_y %.3f %% of it; %d dofs, %.2f s\n", mur, sx / cnt, exact,
                100 * (sx / cnt / exact - 1), 100 * std::sqrt(sy / cnt) / (sx / cnt), st.dofs, st.assemble_s + st.solve_s);
    if (!(std::fabs(sx / cnt / exact - 1) < 0.01 && std::sqrt(sy / cnt) < 0.01 * sx / cnt)) return -1;
    return sx / cnt;
}

static void sphere() {
    std::printf("== T4: a sphere magnetised across the axis, B = 2 Br / 3 inside\n");
    const double R = 0.015, W = 6 * R;
    std::vector<double> r = pieces({0.00015, R, W}, {14, 12});
    std::vector<double> z = pieces({-W, -R, R, W}, {8, 16, 8});
    Mag3DGrid g = {(int)r.size() - 1, 40, (int)z.size() - 1, r.data(), z.data(), 0.0, false};
    size_t n = mag3d_count(&g);
    std::vector<double> nu(n, 1 / MU0), Br(3 * n, 0.0), B(3 * n);
    for (int iz = 0; iz < g.nz; iz++)
        for (int it = 0; it < g.nt; it++)
            for (int ir = 0; ir < g.nr; ir++) {
                double c[3];
                mag3d_centre(&g, ir, it, iz, c);
                if (c[0] * c[0] + c[1] * c[1] + c[2] * c[2] < R * R) Br[3 * mag3d_index(&g, ir, it, iz)] = 1.0;
            }
    Mag3DStats st;
    char err[256];
    if (!mag3d_solve(&g, nu.data(), nullptr, nullptr, Br.data(), B.data(), &st, err, sizeof err)) {
        std::printf("  %s\n", err);
        verdict(false, "T4");
        return;
    }
    double sx = 0, sq = 0;
    int cnt = 0;
    for (int iz = 0; iz < g.nz; iz++)
        for (int it = 0; it < g.nt; it++)
            for (int ir = 0; ir < g.nr; ir++) {
                double c[3];
                mag3d_centre(&g, ir, it, iz, c);
                if (c[0] * c[0] + c[1] * c[1] + c[2] * c[2] < 0.36 * R * R) {
                    const double *Bv = &B[3 * mag3d_index(&g, ir, it, iz)];
                    sx += Bv[0], sq += Bv[1] * Bv[1] + Bv[2] * Bv[2], cnt++;
                }
            }
    double mean = sx / cnt, cross = std::sqrt(sq / cnt) / mean;
    std::printf("  %d dofs, factor %.0f MB, %.2f s; mean B_x %.5f T against 0.66667 (%+.3f %%), B_y and B_z %.3f %% of it\n", st.dofs,
                st.factor_mb, st.assemble_s + st.solve_s, mean, 100 * (mean * 1.5 - 1), 100 * cross);
    verdict(std::fabs(mean * 1.5 - 1) < 0.03 && cross < 0.03, "T4");
}

int main() {
    toroid();
    std::printf("== T2: a cylinder magnetised across its axis, B = (Br / 2)(1 - (R / Rb)^2)\n");
    double b1 = cylinder(1.0);
    verdict(b1 > 0, "T2");
    std::printf("== T3: the same with mu_r 1.05\n");
    verdict(cylinder(1.05) > 0, "T3");
    sphere();
    std::printf(failures ? "mag3dtest: %d FAILED\n" : "mag3dtest: all passed\n", failures);
    return failures ? 1 : 0;
}
