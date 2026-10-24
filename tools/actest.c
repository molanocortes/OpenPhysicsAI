/* actest.c - verification of the acoustic solver (src/lab/acoustic). Criteria written before the first run:
 *
 *   A1 rigid rectangular room 1.0 x 0.7 x 0.45 m: the modes (1,0,0), (0,1,0), (1,1,0) and (2,0,0) at the frequencies
 *      c / 2 sqrt((l/Lx)^2 + (m/Ly)^2 + (n/Lz)^2) within 0.5 per cent.
 *   A2 free field (walls of impedance rho c, far away): the pulse from a source calibrated to 1 Pa at 1 m peaks at
 *      1 m within 5 per cent of 1 Pa, and at 0.5 m twice as high within 3 per cent (spherical spreading).
 *   A3 a plane pulse in a rigid duct meets an end wall of normalised impedance xi: the reflected over the incident
 *      peak is (xi - 1) / (xi + 1) within 0.02, for xi = 3 (R = 0.5) and xi = 0.5 (R = -1/3).
 *   A4 a room 4 x 3 x 2.5 m with every wall of xi = 17.9: the reverberation time from the Schroeder decay (T30,
 *      -5 to -35 dB) within 15 per cent of Eyring's formula with the random-incidence absorption of a locally reacting
 *      wall of that impedance (Paris). The field in a small room is not diffuse; 15 per cent is the allowance.
 * Added 2026-09-26 before their first run, for the loudspeaker (src/lab/acoustic/speaker.c and the piston source):
 *   A5 a circular piston of radius 0.063 m (the Dayton Audio RS180-8's Sd, 124.7 cm^2) in a rigid face, the other faces
 *      absorbing (xi = 1), dx 1 cm, moving with a Gaussian velocity pulse (sigma 0.25 ms): on its axis at 0.4 m the
 *      direct pulse matches the exact on-axis solution rho c [v(t - r/c) - v(t - sqrt(r^2 + a^2)/c)]: the peak within
 *      3 %, the RMS difference over the pulse (plus or minus 4 sigma) within 5 % of the peak.
 *   A6 the RS180-8 (its maker's Thiele-Small parameters) driven from rest by 1 V at 100 Hz: after 1 s, the cone's
 *      velocity amplitude equals the steady phasor solution Bl E / ((Re + j w Le)(Rms + j w Mms + 1/(j w Cms)) + Bl^2)
 *      within 0.1 %.
 *   A7 (against the maker's figure) at 500 Hz and 2.83 V the cone's velocity gives, radiated into half space as a point
 *      source, a sound pressure level at 1 m within 1 dB of the 87.1 dB on the RS180-8's specification sheet.
 *      Amended 2026-09-26 after the first run (84.05 dB, -3.05 dB, FAIL): the test drove a sine of 2.83 V amplitude,
 *      but the sheet's 2.83 V is an RMS voltage (1 W into 8 ohm). The drive is now 2.83 sqrt(2) V peak; the criterion
 *      is unchanged. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/lab/acoustic/acoustic.h"
#include "../src/lab/acoustic/speaker.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int failures;

/* the RS180-8, from Dayton Audio's specification sheet (295-355): Re 6.4 ohm, Le 0.73 mH at 1 kHz, Bl 7.82 T m, Mms 17.9 g,
 * Cms 1.12 mm/N, Sd 124.7 cm^2; Rms from Qms 1.22 and Fs 35.7 Hz as 2 pi Fs Mms / Qms */
static const SpkDriver RS180 = {6.4, 0.73e-3, 7.82, 17.9e-3, 1.12e-3, 2 * M_PI * 35.7 * 17.9e-3 / 1.22, 124.7e-4};
static double sine_v(double t, void *ctx) {
    const double *a = ctx; /* amplitude, frequency */
    return a[0] * sin(2 * M_PI * a[1] * t);
}
static double vel_pulse(double t, double t0, double sg) { return exp(-0.5 * (t - t0) * (t - t0) / (sg * sg)); }
static void verdict(bool ok, const char *what) {
    printf("  %s: %s\n", what, ok ? "pass" : "FAIL");
    if (!ok) failures++;
}
static double wall(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

/* |DFT| of a Hann-windowed trace at frequency f */
static double dft_mag(const double *x, long n, double dt, double f) {
    double re = 0, im = 0;
    for (long k = 0; k < n; k++) {
        double w = 0.5 - 0.5 * cos(2 * M_PI * k / (n - 1));
        double ph = 2 * M_PI * f * k * dt;
        re += w * x[k] * cos(ph);
        im -= w * x[k] * sin(ph);
    }
    return hypot(re, im);
}

static double peak_near(const double *x, long n, double dt, double f0, double span) {
    double best = f0, bm = -1;
    for (double f = f0 * (1 - span); f <= f0 * (1 + span); f += f0 * 1e-4) {
        double m = dft_mag(x, n, dt, f);
        if (m > bm) bm = m, best = f;
    }
    return best;
}

int main(void) {
    const double c = 343.2;
    printf("== A1: modes of a rigid rectangular room\n");
    {
        AcSpec s;
        ac_spec_defaults(&s);
        s.size[0] = 1.0, s.size[1] = 0.7, s.size[2] = 0.45, s.dx = 0.01;
        s.nsources = 1;
        s.sources[0] = (AcSource){{0.13, 0.11, 0.07}, 1.0, 2e-4, 1e-3};
        s.nreceivers = 1;
        s.receivers[0][0] = 0.91, s.receivers[0][1] = 0.63, s.receivers[0][2] = 0.39;
        char err[256];
        Acoustic *a = ac_create(&s, err, sizeof err);
        if (!a) {
            printf("  %s\n", err);
            return 1;
        }
        double t0 = wall();
        while (ac_time(a) < 0.4) ac_step(a);
        long n;
        const double *tr = ac_receiver_trace(a, 0, &n);
        int modes[4][3] = {{1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {2, 0, 0}};
        double worst = 0;
        for (int m = 0; m < 4; m++) {
            double f = c / 2 * sqrt(pow(modes[m][0] / 1.0, 2) + pow(modes[m][1] / 0.7, 2) + pow(modes[m][2] / 0.45, 2));
            double fm = peak_near(tr, n, ac_dt(a), f, 0.03);
            worst = fmax(worst, fabs(fm - f) / f);
            printf("  mode (%d,%d,%d): %.2f Hz against %.2f (%+.3f %%)\n", modes[m][0], modes[m][1], modes[m][2], fm, f, 100 * (fm - f) / f);
        }
        printf("  %ld steps in %.1f s\n", ac_steps(a), wall() - t0);
        verdict(worst < 0.005, "A1");
        ac_free(a);
    }

    printf("== A2: free field, a calibrated source\n");
    {
        AcSpec s;
        ac_spec_defaults(&s);
        s.size[0] = s.size[1] = s.size[2] = 3.2;
        s.dx = 0.02;
        s.nmaterials = 2;
        s.xi[1] = 1.0;
        for (int d = 0; d < 6; d++) s.wall_material[d] = 1;
        s.nsources = 1;
        s.sources[0] = (AcSource){{1.1, 1.6, 1.6}, 1.0, 3e-4, 1.5e-3};
        s.nreceivers = 2;
        s.receivers[0][0] = 1.6, s.receivers[0][1] = 1.6, s.receivers[0][2] = 1.6;
        s.receivers[1][0] = 2.1, s.receivers[1][1] = 1.6, s.receivers[1][2] = 1.6;
        char err[256];
        Acoustic *a = ac_create(&s, err, sizeof err);
        while (ac_time(a) < 1.5e-3 + 1.2 / c + 1.5e-3) ac_step(a);
        long n;
        double pk[2] = {0, 0};
        for (int r = 0; r < 2; r++) {
            const double *tr = ac_receiver_trace(a, r, &n);
            for (long k = 0; k < n; k++) pk[r] = fmax(pk[r], fabs(tr[k]));
        }
        printf("  peak at 0.5 m %.4f Pa, at 1.0 m %.4f Pa (source calibrated to 1 Pa at 1 m), ratio %.4f\n", pk[0], pk[1], pk[0] / pk[1]);
        verdict(fabs(pk[1] - 1) < 0.05 && fabs(pk[0] / pk[1] - 2) / 2 < 0.03, "A2");
        ac_free(a);
    }

    printf("== A3: reflection from an impedance wall\n");
    {
        double xis[2] = {3.0, 0.5};
        double worst = 0;
        for (int q = 0; q < 2; q++) {
            AcSpec s;
            ac_spec_defaults(&s);
            s.size[0] = 4.0, s.size[1] = s.size[2] = 0.1, s.dx = 0.01;
            s.nmaterials = 3;
            s.xi[1] = 1.0, s.xi[2] = xis[q];
            s.wall_material[0] = 1, s.wall_material[1] = 2;
            s.nsources = 1;
            s.sources[0] = (AcSource){{0.3, 0.05, 0.05}, 1.0, 3e-4, 1.5e-3};
            s.nreceivers = 1;
            s.receivers[0][0] = 2.0, s.receivers[0][1] = 0.05, s.receivers[0][2] = 0.05;
            char err[256];
            Acoustic *a = ac_create(&s, err, sizeof err);
            while (ac_time(a) < 1.5e-3 + 6.5 / c) ac_step(a); /* the reflection passes the receiver at 5.7 m of travel */
            long n;
            const double *tr = ac_receiver_trace(a, 0, &n);
            /* the incident pulse passes the receiver at 1.7 m of travel, the reflection at 1.7 + 4.0 m */
            double t_inc = 1.5e-3 + 1.7 / c, t_ref = 1.5e-3 + 5.7 / c, win = 1.2e-3;
            /* the reflected window projected on the incident one, both centred on their arrival times: the pulse has
             * two lobes of equal size, so comparing single peaks could pair opposite lobes */
            double dt = ac_dt(a), num = 0, den = 0;
            long ki = (long)lround(t_inc / dt) - 1, kr = (long)lround(t_ref / dt) - 1, h = (long)(win / dt);
            for (long j = -h; j <= h; j++) {
                if (ki + j < 0 || kr + j >= n) continue;
                num += tr[ki + j] * tr[kr + j];
                den += tr[ki + j] * tr[ki + j];
            }
            double R = num / den, Rth = (xis[q] - 1) / (xis[q] + 1);
            printf("  xi %.2f: R %.4f against %.4f\n", xis[q], R, Rth);
            worst = fmax(worst, fabs(R - Rth));
            ac_free(a);
        }
        verdict(worst < 0.02, "A3");
    }

    printf("== A4: reverberation time of a room with absorbing walls\n");
    {
        const double xi = 17.9, Lx = 4, Ly = 3, Lz = 2.5;
        AcSpec s;
        ac_spec_defaults(&s);
        s.size[0] = Lx, s.size[1] = Ly, s.size[2] = Lz, s.dx = 0.05;
        s.nmaterials = 2;
        s.xi[1] = xi;
        for (int d = 0; d < 6; d++) s.wall_material[d] = 1;
        s.nsources = 1;
        s.sources[0] = (AcSource){{1.1, 0.9, 1.2}, 1.0, 2.5e-4, 1.5e-3};
        s.nreceivers = 4;
        double rc[4][3] = {{2.9, 2.1, 1.4}, {3.1, 0.8, 0.9}, {2.2, 2.3, 1.9}, {0.8, 2.2, 0.7}};
        memcpy(s.receivers, rc, sizeof rc);
        char err[256];
        Acoustic *a = ac_create(&s, err, sizeof err);
        double t0 = wall();
        while (ac_time(a) < 0.8) ac_step(a);
        long n;
        /* energy decay: sum of the receivers' squared pressures, backward-integrated (Schroeder) */
        double *e = calloc((size_t)(n = ac_steps(a)), sizeof(double));
        for (int r = 0; r < 4; r++) {
            long m;
            const double *tr = ac_receiver_trace(a, r, &m);
            for (long k = 0; k < m; k++) e[k] += tr[k] * tr[k];
        }
        for (long k = n - 2; k >= 0; k--) e[k] += e[k + 1];
        /* least-squares line through the decay between -5 and -35 dB */
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        long cnt = 0;
        for (long k = 0; k < n; k++) {
            double db = 10 * log10(e[k] / e[0]);
            if (db > -5 || db < -35) continue;
            double t = k * ac_dt(a);
            sx += t, sy += db, sxx += t * t, sxy += t * db, cnt++;
        }
        double slope = (cnt * sxy - sx * sy) / (cnt * sxx - sx * sx);
        double T60 = -60 / slope;
        /* random-incidence absorption of a locally reacting surface of real impedance xi (Paris) */
        double a_st = 8 / xi * (1 + 1 / (1 + xi) - 2 / xi * log(1 + xi));
        double V = Lx * Ly * Lz, S = 2 * (Lx * Ly + Ly * Lz + Lx * Lz);
        double Tey = 24 * log(10) / c * V / (-S * log(1 - a_st));
        printf("  T30 %.3f s against Eyring %.3f s (random-incidence absorption %.3f), %+.1f %%, %ld steps, %.1f s\n", T60, Tey, a_st,
               100 * (T60 - Tey) / Tey, n, wall() - t0);
        verdict(fabs(T60 - Tey) / Tey < 0.15, "A4");
        free(e);
        ac_free(a);
    }
    printf("== A5: a piston in a baffle, on its axis\n");
    {
        AcSpec s;
        ac_spec_defaults(&s);
        const double L = 0.8, dx = 0.01, a0 = 0.063, r = 0.4, sg = 0.25e-3, t0 = 1e-3, V0 = 0.01;
        s.size[0] = s.size[1] = s.size[2] = L, s.dx = dx, s.c = 343.2, s.rho = 1.204;
        s.nmaterials = 2, s.xi[0] = 1.0, s.xi[1] = 0; /* 0: absorbing, 1: rigid */
        for (int f = 0; f < 6; f++) s.wall_material[f] = 0;
        s.wall_material[4] = 1;
        s.piston = true, s.piston_face = 4, s.piston_centre[0] = s.piston_centre[1] = L / 2, s.piston_radius = a0;
        s.nreceivers = 1, s.receivers[0][0] = s.receivers[0][1] = L / 2, s.receivers[0][2] = r;
        char err[256];
        Acoustic *a = ac_create(&s, err, sizeof err);
        long np;
        double gain;
        ac_piston_info(a, &np, &gain);
        const double tend = t0 + r / s.c + 5 * sg;
        while (ac_time(a) < tend) {
            double t = ac_time(a);
            ac_set_piston_acceleration(a, V0 * vel_pulse(t, t0, sg) * (-(t - t0) / (sg * sg)));
            ac_step(a);
        }
        long m;
        const double *tr = ac_receiver_trace(a, 0, &m);
        double pk = 0, pke = 0, se = 0;
        int cnt = 0;
        const double r2 = sqrt(r * r + a0 * a0);
        for (long k = 0; k < m; k++) {
            double t = (k + 1) * ac_dt(a), e = s.rho * s.c * V0 * (vel_pulse(t - r / s.c, t0, sg) - vel_pulse(t - r2 / s.c, t0, sg));
            if (fabs(t - t0 - r / s.c) > 4 * sg) continue;
            pk = fmax(pk, fabs(tr[k])), pke = fmax(pke, fabs(e)), se += (tr[k] - e) * (tr[k] - e), cnt++;
        }
        double rms = sqrt(se / cnt);
        printf("  piston of %ld nodes (area gain %.4f); peak %.5f Pa against %.5f (%+.2f %%); RMS difference %.2f %% of the peak\n", np, gain, pk, pke,
               100 * (pk / pke - 1), 100 * rms / pke);
        verdict(fabs(pk / pke - 1) < 0.03 && rms < 0.05 * pke, "A5");
        ac_free(a);
    }
    printf("== A6: the RS180-8 driven at 100 Hz, against the phasor solution\n");
    {
        double drive[2] = {1.0, 100.0}, w = 2 * M_PI * drive[1];
        SpkState st = {0, 0, 0};
        const double dt = 1e-6;
        double vmax = 0;
        for (long k = 0; k < (long)(1.0 / dt) + (long)(1 / drive[1] / dt); k++) {
            spk_step(&RS180, &st, k * dt, dt, sine_v, drive);
            if (k * dt >= 1.0) vmax = fmax(vmax, fabs(st.v));
        }
        /* |Bl E / (Ze Zm + Bl^2)| with complex arithmetic by hand */
        double zer = RS180.Re, zei = w * RS180.Le, zmr = RS180.Rms, zmi = w * RS180.Mms - 1 / (w * RS180.Cms);
        double dr = zer * zmr - zei * zmi + RS180.Bl * RS180.Bl, di = zer * zmi + zei * zmr;
        double ve = RS180.Bl * drive[0] / sqrt(dr * dr + di * di);
        printf("  velocity amplitude %.6e m/s against %.6e (%+.4f %%)\n", vmax, ve, 100 * (vmax / ve - 1));
        verdict(fabs(vmax / ve - 1) < 1e-3, "A6");
    }
    printf("== A7: sensitivity against the maker's figure (2.83 V RMS, 1 m, half space)\n");
    {
        double drive[2] = {2.83 * sqrt(2.0), 500.0}, w = 2 * M_PI * drive[1];
        SpkState st = {0, 0, 0};
        const double dt = 2e-7;
        double vmax = 0;
        for (long k = 0; k < (long)(0.5 / dt) + (long)(1 / drive[1] / dt); k++) {
            spk_step(&RS180, &st, k * dt, dt, sine_v, drive);
            if (k * dt >= 0.5) vmax = fmax(vmax, fabs(st.v));
        }
        double p = 1.204 * w * RS180.Sd * vmax / (2 * M_PI * 1.0), spl = 20 * log10(p / sqrt(2) / 20e-6);
        printf("  cone velocity %.4e m/s; %.2f dB SPL at 1 m against the specification sheet's 87.1 dB (%+.2f dB)\n", vmax, spl, spl - 87.1);
        verdict(fabs(spl - 87.1) < 1.0, "A7");
    }
    printf(failures ? "actest: %d FAILED\n" : "actest: all passed\n", failures);
    return failures ? 1 : 0;
}
