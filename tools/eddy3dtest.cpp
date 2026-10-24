/* eddy3dtest - verification of induction: eddy currents and the heat they make (src/lab/magnet/eddy3d.h).
 *
 * Criteria written on 2026-09-26, before the first run:
 *   I1 skin effect: a long cylinder of radius R = 10 mm (sigma 1.4e6 S/m, stainless steel, mu_r 1) inside a solenoid at
 *      the frequency that makes the skin depth R / 3 (16.3 kHz); periodic along the axis. Inside, H_z = H_s J0(k r) /
 *      J0(k R) with k = (1 - j) / delta, so the current density is k H_s J1(k r) / J0(k R) and the loss per metre is
 *      the integral of |J|^2 / (2 sigma) over the section, with H_s the field at the surface (measured in the air just
 *      outside: the grid's outer flux wall, not an ideal solenoid, sets it). The computed loss per metre within 3 % of
 *      that, on 48 sectors and about 6 elements per skin depth.
 *   I2 heat: 1e8 W/m^3 in the same cylinder for 0.5 s (fifty backward-Euler steps), the grid's rim held at ambient
 *      3 cm away: the heat stored (the integral of rho c (T - ambient)) within 1 % of the heat put in.
 *   I3 (added before its first run) the same cylinder as a quarter: a sector of 90 degrees with its sides periodic,
 *      12 sectors: the loss per metre, times 4, within 0.5 % of I1's on the full grid (the rotational periodicity that
 *      a gear's single tooth will use).
 *   First run: I1 +23.36 % (FAIL), I2 exact. The grid's inner cylinder (0.2 mm) was a magnetic wall, an infinitely
 *   permeable rod on the axis into which an axial field pours; for fields along the axis it is now a flux wall
 *   (Mag3DGrid.inner_flux_wall); the criterion is unchanged. */
#include "../src/lab/magnet/eddy3d.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

typedef std::complex<double> cd;
static int failures;
static void verdict(bool ok, const char *name) {
    std::printf("  %s: %s\n", name, ok ? "pass" : "FAIL");
    failures += !ok;
}
static const double MU0 = 4e-7 * M_PI;

/* Bessel functions of the first kind, orders 0 and 1, of a complex argument, by their series (|z| here below 10) */
static cd besselJ(int nu, cd z) {
    cd sum = 0, term = nu == 0 ? cd(1) : z / 2.0, q = -(z * z) / 4.0;
    for (int k = 0; k < 80; k++) {
        sum += term;
        term *= q / ((double)(k + 1) * (double)(k + 1 + nu));
    }
    return sum;
}

static std::vector<double> pieces(std::initializer_list<double> edges, std::initializer_list<int> counts) {
    std::vector<double> e(edges), out{e[0]};
    auto c = counts.begin();
    for (size_t k = 0; k + 1 < e.size(); k++, c++)
        for (int i = 1; i <= *c; i++) out.push_back(e[k] + (e[k + 1] - e[k]) * i / *c);
    return out;
}

int main() {
    const double R = 0.010, Rs = 0.015, sigma_s = 1.4e6, delta = R / 3, omega = 2 / (MU0 * sigma_s * delta * delta), H0 = 1000.0;
    std::vector<double> r = pieces({0.0002, 0.005, R, Rs, 0.030}, {5, 15, 5, 6}), z = pieces({0, 0.003}, {3});
    Mag3DGrid g = {(int)r.size() - 1, 48, 3, r.data(), z.data(), 0.0, true, 0, true};
    size_t n = mag3d_count(&g);
    std::vector<double> nu(n, 1 / MU0), sg(n, 0.0), T(3 * n, 0.0), q(n), B(n), J(n);
    for (int iz = 0; iz < g.nz; iz++)
        for (int it = 0; it < g.nt; it++)
            for (int ir = 0; ir < g.nr; ir++) {
                size_t e = mag3d_index(&g, ir, it, iz);
                double rc = 0.5 * (r[ir] + r[ir + 1]);
                if (rc < R) sg[e] = sigma_s;
                if (rc < Rs) T[3 * e + 2] = H0; /* the solenoid: T = H0 z inside it, a surface current H0 at Rs */
            }
    std::printf("== I1: the skin effect in a cylinder against the Bessel solution\n");
    Mag3DStats st;
    char err[256];
    if (!eddy3d_solve(&g, nu.data(), sg.data(), T.data(), omega, q.data(), B.data(), J.data(), &st, err, sizeof err)) {
        std::printf("  %s\n", err);
        verdict(false, "I1");
        return 1;
    }
    double loss = 0, Hs = 0;
    int nh = 0;
    for (int iz = 0; iz < g.nz; iz++)
        for (int it = 0; it < g.nt; it++)
            for (int ir = 0; ir < g.nr; ir++) {
                size_t e = mag3d_index(&g, ir, it, iz);
                loss += q[e] * mag3d_volume(&g, ir, it, iz);
                double rc = 0.5 * (r[ir] + r[ir + 1]);
                if (rc > R && rc < Rs) Hs += B[e] / MU0, nh++;
            }
    loss /= (z.back() - z.front());
    Hs /= nh;
    cd k = cd(1, -1) / delta, J0R = besselJ(0, k * R);
    double exact = 0;
    const int M = 4000;
    for (int i = 0; i < M; i++) {
        double rr = R * (i + 0.5) / M;
        cd Jc = k * Hs * besselJ(1, k * rr) / J0R;
        exact += std::norm(Jc) / (2 * sigma_s) * 2 * M_PI * rr * (R / M);
    }
    std::printf("  f %.0f Hz, %d unknowns, factor %.0f MB, %.2f s; surface field %.2f A/m; loss %.6g W/m against %.6g (%+.2f %%)\n", omega / (2 * M_PI),
                st.dofs, st.factor_mb, st.assemble_s + st.solve_s, Hs, loss, exact, 100 * (loss / exact - 1));
    verdict(std::fabs(loss / exact - 1) < 0.03, "I1");

    std::printf("== I3: a quarter with periodic sides against the whole\n");
    {
        Mag3DGrid q4 = g;
        q4.nt = 12, q4.sector = 4;
        size_t n4 = mag3d_count(&q4);
        std::vector<double> nu4(n4, 1 / MU0), sg4(n4, 0.0), T4(3 * n4, 0.0), qq4(n4);
        for (int iz = 0; iz < q4.nz; iz++)
            for (int it = 0; it < q4.nt; it++)
                for (int ir = 0; ir < q4.nr; ir++) {
                    size_t e = mag3d_index(&q4, ir, it, iz);
                    double rc = 0.5 * (r[ir] + r[ir + 1]);
                    if (rc < R) sg4[e] = sigma_s;
                    if (rc < Rs) T4[3 * e + 2] = H0;
                }
        if (!eddy3d_solve(&q4, nu4.data(), sg4.data(), T4.data(), omega, qq4.data(), nullptr, nullptr, &st, err, sizeof err)) {
            std::printf("  %s\n", err);
            verdict(false, "I3");
        } else {
            double l4 = 0;
            for (int iz = 0; iz < q4.nz; iz++)
                for (int it = 0; it < q4.nt; it++)
                    for (int ir = 0; ir < q4.nr; ir++) l4 += qq4[mag3d_index(&q4, ir, it, iz)] * mag3d_volume(&q4, ir, it, iz);
            l4 = 4 * l4 / (z.back() - z.front());
            std::printf("  quarter x 4: %.6g W/m against the whole %.6g (%+.3f %%), %d unknowns\n", l4, loss, 100 * (l4 / loss - 1), st.dofs);
            verdict(std::fabs(l4 / loss - 1) < 0.005, "I3");
        }
    }
    std::printf("== I2: heat stored against heat put in\n");
    {
        std::vector<double> k(n, 0.0263), c(n, 1.1614 * 1007), qq(n, 0.0);
        double put = 0;
        for (int iz = 0; iz < g.nz; iz++)
            for (int it = 0; it < g.nt; it++)
                for (int ir = 0; ir < g.nr; ir++) {
                    size_t e = mag3d_index(&g, ir, it, iz);
                    if (0.5 * (r[ir] + r[ir + 1]) < R) k[e] = 16.2, c[e] = 8000 * 500, qq[e] = 1e8, put += 1e8 * mag3d_volume(&g, ir, it, iz);
                }
        double dt = 0.01;
        Heat3DFem *H = heat3dfem_create(&g, k.data(), c.data(), 293.15, dt, err, sizeof err);
        if (!H) {
            std::printf("  %s\n", err);
            verdict(false, "I2");
        } else {
            for (int s = 0; s < 50; s++) heat3dfem_step(H, qq.data());
            double stored = heat3dfem_energy(H), in = put * 50 * dt;
            std::vector<double> Tt(n);
            heat3dfem_temperature(H, Tt.data());
            std::printf("  stored %.6g J against %.6g J put in (%+.3f %%); centre %.2f K\n", stored, in, 100 * (stored / in - 1), Tt[mag3d_index(&g, 0, 0, 1)]);
            verdict(std::fabs(stored / in - 1) < 0.01, "I2");
            heat3dfem_free(H);
        }
    }
    std::printf(failures ? "eddy3dtest: %d FAILED\n" : "eddy3dtest: all passed\n", failures);
    return failures ? 1 : 0;
}
