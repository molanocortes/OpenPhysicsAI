/* vistest - validation and benchmarks for the CPU visualisation kernels (vis_fields / vis_stream / vis_iso)
 *   build/vistest [threads]
 * Images go to build/vistest_out/ (run from the project root).
 */
#include "../src/common.h"
#include "../src/threads.h"
#include "../src/vis.h"

#include <sys/stat.h>

#define PI 3.14159265358979323846
#define OUTDIR "build/vistest_out"

static int n_pass = 0, n_fail = 0;

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        bool ok_ = (cond);                                                                                            \
        if (ok_) n_pass++;                                                                                            \
        else n_fail++;                                                                                                \
        printf("  [%s] ", ok_ ? "pass" : "FAIL");                                                                     \
        printf(__VA_ARGS__);                                                                                          \
        printf("\n");                                                                                                 \
    } while (0)

/* ---------------------------------------------------------------------------------------------------------------- */
/* synthetic flows: uniform U along x + Lamb-Oseen vortex tubes (optionally helical) + optional solid sphere          */

typedef struct {
    double y0, z0, rc, gamma; /* axis position, core radius, circulation */
    double amp, lambda, phase; /* helical centreline displacement (amp = 0: straight tube along x) */
} Tube;

typedef struct {
    int nx, ny, nz;
    float *rho, *ux, *uy, *uz;
    uint8_t *solid;
    VisGrid g;
} Flow;

typedef struct {
    Flow *f;
    double U;
    const Tube *tubes;
    int ntubes;
    double sc[3], sr; /* sphere centre / radius (sr <= 0: none) */
} FillCtx;

static size_t idx3(const Flow *f, int i, int j, int k) {
    return (size_t)i + (size_t)f->nx * ((size_t)j + (size_t)f->ny * (size_t)k);
}

static void fill_slab(void *vctx, int k0, int k1, int tid) {
    (void)tid;
    FillCtx *c = vctx;
    Flow *f = c->f;
    for (int k = k0; k < k1; k++)
        for (int j = 0; j < f->ny; j++)
            for (int i = 0; i < f->nx; i++) {
                double x = i + 0.5, y = j + 0.5, z = k + 0.5, uy = 0.0, uz = 0.0;
                for (int t = 0; t < c->ntubes; t++) {
                    const Tube *T = &c->tubes[t];
                    double yc = T->y0, zc = T->z0;
                    if (T->amp != 0.0) {
                        yc += T->amp * sin(2.0 * PI * x / T->lambda + T->phase);
                        zc += T->amp * cos(2.0 * PI * x / T->lambda + T->phase);
                    }
                    double dy = y - yc, dz = z - zc, r2 = dy * dy + dz * dz, rc2 = T->rc * T->rc;
                    double K = r2 > 1e-12 ? T->gamma / (2.0 * PI) * (1.0 - exp(-r2 / rc2)) / r2 : T->gamma / (2.0 * PI * rc2);
                    uy -= K * dz;
                    uz += K * dy;
                }
                size_t id = idx3(f, i, j, k);
                bool s = false;
                if (c->sr > 0.0) {
                    double ex = x - c->sc[0], ey = y - c->sc[1], ez = z - c->sc[2];
                    s = ex * ex + ey * ey + ez * ez <= c->sr * c->sr;
                }
                f->solid[id] = s;
                f->rho[id] = (float)(1.0 + 0.002 * sin(2.0 * PI * x / f->nx) * cos(2.0 * PI * y / f->ny));
                f->ux[id] = s ? 0.0f : (float)c->U;
                f->uy[id] = s ? 0.0f : (float)uy;
                f->uz[id] = s ? 0.0f : (float)uz;
            }
}

static bool flow_make(Flow *f, int nx, int ny, int nz, double U, const Tube *tubes, int ntubes, const double *sc,
                      double sr, ThreadPool *pool) {
    size_t n = (size_t)nx * (size_t)ny * (size_t)nz;
    memset(f, 0, sizeof *f);
    f->nx = nx, f->ny = ny, f->nz = nz;
    f->rho = malloc(n * sizeof(float));
    f->ux = malloc(n * sizeof(float));
    f->uy = malloc(n * sizeof(float));
    f->uz = malloc(n * sizeof(float));
    f->solid = malloc(n);
    if (!f->rho || !f->ux || !f->uy || !f->uz || !f->solid) return false;
    f->g = (VisGrid){nx, ny, nz, f->rho, f->ux, f->uy, f->uz, f->solid};
    FillCtx c = {f, U, tubes, ntubes, {sc ? sc[0] : 0, sc ? sc[1] : 0, sc ? sc[2] : 0}, sc ? sr : 0.0};
    pool_for(pool, nz, 1, fill_slab, &c);
    return true;
}

static void flow_free(Flow *f) {
    free(f->rho);
    free(f->ux);
    free(f->uy);
    free(f->uz);
    free(f->solid);
    memset(f, 0, sizeof *f);
}

/* max over x of (1 - exp(-x^2)) / x: peak swirl = gamma / (2 pi rc) * factor */
static double lamb_oseen_peak_factor(void) {
    double best = 0.0;
    for (int i = 1; i <= 300000; i++) {
        double x = i * 1e-5, v = (1.0 - exp(-x * x)) / x;
        best = v > best ? v : best;
    }
    return best;
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* exact statistics for checking VisRange                                                                             */

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static double sorted_pct(const float *v, size_t m, double q) {
    double r = q * (double)(m - 1);
    size_t i = (size_t)r;
    if (i + 1 >= m) return v[m - 1];
    return v[i] + (r - (double)i) * (v[i + 1] - v[i]);
}

static void exact_range(const float *v, const uint8_t *solid, size_t n, VisRange *r) {
    float *t = malloc(n * sizeof(float));
    size_t m = 0;
    for (size_t i = 0; i < n; i++)
        if (!solid[i]) t[m++] = v[i];
    qsort(t, m, sizeof(float), cmp_float);
    r->min = t[0], r->max = t[m - 1];
    r->lo = (float)sorted_pct(t, m, 0.01), r->hi = (float)sorted_pct(t, m, 0.99);
    free(t);
}

static bool range_close(const VisRange *a, const VisRange *e, double tol_frac) {
    double span = (double)e->max - e->min, tol = span > 0 ? tol_frac * span : 1e-7;
    return a->min == e->min && a->max == e->max && fabs((double)a->lo - e->lo) <= tol && fabs((double)a->hi - e->hi) <= tol;
}

static void check_range(const char *name, const float *out, const Flow *f, const VisRange *r) {
    VisRange e;
    exact_range(out, f->solid, (size_t)f->nx * f->ny * f->nz, &e);
    CHECK(range_close(r, &e, 2e-3), "%-9s range min %.4g max %.4g lo %.4g hi %.4g | exact lo %.4g hi %.4g", name,
          r->min, r->max, r->lo, r->hi, e.lo, e.hi);
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* 1. derived fields                                                                                                  */

typedef struct {
    double U, rc, gamma, y0, z0, sc[3], sr;
} Analytic;

static void test_fields(const Flow *f, const Analytic *an, ThreadPool *p1, ThreadPool *pn) {
    printf("\n== 1. derived fields: uniform flow + Lamb-Oseen tube + sphere (%dx%dx%d) ==\n", f->nx, f->ny, f->nz);
    size_t n = (size_t)f->nx * f->ny * f->nz;
    float *out = malloc(n * sizeof(float)), *out1 = malloc(n * sizeof(float));
    VisRange r, r1;
    int ia = 64, ja = (int)an->y0, ka = (int)an->z0; /* the axis passes through the centre of cell (ja, ka) */
    size_t axis = idx3(f, ia, ja, ka), in_sphere = idx3(f, (int)an->sc[0], (int)an->sc[1], (int)an->sc[2]);
    double w0 = an->gamma / (PI * an->rc * an->rc);

    vis_compute_field(&f->g, VIS_VORTICITY, out, &r, pn);
    CHECK(fabs(out[axis] / w0 - 1.0) < 0.03, "vorticity on axis %.6f vs analytic w0 = G/(pi rc^2) = %.6f (%+.2f%%)",
          out[axis], w0, 100.0 * (out[axis] / w0 - 1.0));
    size_t at_rc = idx3(f, ia, ja, ka + (int)an->rc);
    double w_rc = w0 * exp(-1.0);
    CHECK(fabs(out[at_rc] / w_rc - 1.0) < 0.05, "vorticity at r = rc %.6f vs analytic %.6f (%+.2f%%)", out[at_rc], w_rc,
          100.0 * (out[at_rc] / w_rc - 1.0));
    CHECK(out[in_sphere] == 0.0f, "vorticity inside the sphere = %g", out[in_sphere]);
    check_range("vorticity", out, f, &r);
    vis_compute_field(&f->g, VIS_VORTICITY, out1, &r1, p1);
    CHECK(!memcmp(out, out1, n * sizeof(float)) && !memcmp(&r, &r1, sizeof r), "vorticity: 1 thread == %d threads (bitwise)",
          pool_size(pn));

    vis_compute_field(&f->g, VIS_QCRITERION, out, &r, pn);
    double q0 = 0.25 * w0 * w0;
    CHECK(fabs(out[axis] / q0 - 1.0) < 0.05, "Q on axis %.4g vs analytic w0^2/4 = %.4g (%+.2f%%)", out[axis], q0,
          100.0 * (out[axis] / q0 - 1.0));
    bool neg = true;
    for (int d = 15; d <= 30; d++) { /* outside the core, away from the sphere (+z side of the axis) */
        size_t id = idx3(f, ia, ja, ka + d);
        double rr = d, qa = -pow(an->gamma / (2.0 * PI * rr * rr), 2.0);
        if (!(out[id] < 0.0f)) neg = false;
        if (d == 15 || d == 30) printf("         Q(r=%d) = %.4g (potential vortex: %.4g)\n", d, out[id], qa);
    }
    CHECK(neg, "Q < 0 for 15 <= r <= 30 outside the core");
    CHECK(out[in_sphere] == 0.0f, "Q inside the sphere = %g", out[in_sphere]);
    check_range("Q", out, f, &r);
    vis_compute_field(&f->g, VIS_QCRITERION, out1, &r1, p1);
    CHECK(!memcmp(out, out1, n * sizeof(float)) && !memcmp(&r, &r1, sizeof r), "Q: 1 thread == %d threads (bitwise)",
          pool_size(pn));

    vis_compute_field(&f->g, VIS_UX, out, &r, pn);
    CHECK(out[axis] == (float)an->U && out[in_sphere] == 0.0f && r.min == (float)an->U && r.max == (float)an->U &&
              r.lo == r.min && r.hi == r.max,
          "UX: %.3f in fluid, %.3f in solid, range [%g %g] lo %g hi %g", out[axis], out[in_sphere], r.min, r.max, r.lo,
          r.hi);

    vis_compute_field(&f->g, VIS_UZ, out, &r, pn);
    check_range("UZ", out, f, &r);

    vis_compute_field(&f->g, VIS_PRESSURE, out, &r, pn);
    double perr = 0.0;
    for (size_t i = 0; i < n; i++) perr = fmax(perr, fabs(out[i] - (f->rho[i] - 1.0) / 3.0));
    CHECK(perr < 1e-7, "pressure = (rho-1)/3 everywhere incl. solids (max err %.2g)", perr);
    check_range("pressure", out, f, &r);

    vis_compute_field(&f->g, VIS_SPEED, out, &r, pn);
    double smax = sqrt(an->U * an->U + 0.05 * 0.05);
    CHECK(r.min >= 0.0499f && fabs(r.max / smax - 1.0) < 0.01, "speed range [%.5f %.5f]: solids excluded, max ~ %.5f",
          r.min, r.max, smax);
    check_range("speed", out, f, &r);

    float u[3];
    bool okc = vis_sample_velocity(&f->g, ia + 0.5f, (float)an->y0 + 3.0f, (float)an->z0, u);
    double K3 = an->gamma / (2.0 * PI) * (1.0 - exp(-9.0 / (an->rc * an->rc))) / 9.0;
    CHECK(okc && fabs(u[0] - an->U) < 1e-6 && fabs(u[2] - K3 * 3.0) < 2e-3 * fabs(K3 * 3.0) + 2e-4,
          "sample_velocity at cell centre r=3: u=(%.5f %.5f %.5f), analytic uz %.5f", u[0], u[1], u[2], K3 * 3.0);
    CHECK(!vis_sample_velocity(&f->g, (float)an->sc[0], (float)an->sc[1], (float)an->sc[2], u) &&
              !vis_sample_velocity(&f->g, -0.1f, 5, 5, u) && !vis_sample_velocity(&f->g, 5, 5, f->nz + 0.01f, u) &&
              vis_sample_velocity(&f->g, (float)f->nx, 0.0f, 5.0f, u),
          "sample_velocity: false in solid / outside, true on the domain faces");
    float sv = vis_sample_scalar(f->ux, f->nx, f->ny, f->nz, 3.25f, 7.75f, 9.5f);
    float mid = vis_sample_scalar(f->uz, f->nx, f->ny, f->nz, ia + 1.0f, (float)an->y0 + 3.5f, (float)an->z0);
    float ref = 0.5f * (f->uz[idx3(f, ia, ja + 3, ka)] + f->uz[idx3(f, ia, ja + 4, ka)]);
    CHECK(sv == (float)an->U && fabs(mid - ref) < 1e-7f, "sample_scalar: constant %.4f, midpoint %.6f vs %.6f", sv, mid, ref);
    free(out);
    free(out1);
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* 2. streamlines                                                                                                     */

static const float *svert(const StreamlineSet *s, uint32_t l, uint32_t v) {
    return s->verts + 5 * ((size_t)s->line_first[l] + v);
}

/* contiguous storage, >= 2 vertices, all finite, flight time 0 at the first vertex and strictly increasing */
static bool lines_sane(const StreamlineSet *s) {
    size_t expect = 0;
    for (uint32_t l = 0; l < s->nlines; l++) {
        if (s->line_first[l] != expect || s->line_count[l] < 2) return false;
        expect += s->line_count[l];
        for (uint32_t v = 0; v < s->line_count[l]; v++) {
            const float *p = svert(s, l, v);
            for (int q = 0; q < 5; q++)
                if (!is_finite_f32(p[q])) return false;
            if (v == 0 && p[4] != 0.0f) return false;
            if (v > 0 && !(p[4] > (p - 5)[4])) return false;
        }
    }
    return expect == s->nverts;
}

static double wrap_pi(double d) {
    while (d > PI) d -= 2.0 * PI;
    while (d < -PI) d += 2.0 * PI;
    return d;
}

static void test_streamlines(const Flow *f, const Analytic *an, ThreadPool *pn) {
    printf("\n== 2. streamlines ==\n");
    StreamParams sp = {.max_steps = 1500, .step = 0.5f, .both_directions = false, .min_speed = 1e-4f};
    StreamlineSet s = {0}, sb = {0};
    const double radii[3] = {2.0, 4.0, 8.0};
    float seeds[3 * 24];
    for (int r = 0; r < 3; r++)
        for (int a = 0; a < 8; a++) {
            float *sd = seeds + 3 * (r * 8 + a);
            sd[0] = 4.0f;
            sd[1] = (float)(an->y0 + radii[r] * cos(2.0 * PI * a / 8.0));
            sd[2] = (float)(an->z0 + radii[r] * sin(2.0 * PI * a / 8.0));
        }
    vis_streamlines(&f->g, NULL, seeds, 24, &sp, &s, pn);
    CHECK(s.nlines == 24 && lines_sane(&s), "24 seeds around the vortex -> %u lines, %u verts: finite, contiguous, time 0 -> increasing",
          s.nlines, s.nverts);
    double e_ang = 0, e_time = 0, e_speed = 0, e_rad = 0, exit_x = 1e9;
    bool spiral = true;
    for (uint32_t l = 0; l < s.nlines && l < 24; l++) {
        double rr = radii[l / 8], om = an->gamma / (2 * PI) * (1 - exp(-rr * rr / (an->rc * an->rc))) / (rr * rr);
        double spd = sqrt(an->U * an->U + om * om * rr * rr), ang = 0.0;
        const float *p0 = svert(&s, l, 0), *pe = svert(&s, l, s.line_count[l] - 1);
        for (uint32_t v = 0; v < s.line_count[l]; v++) {
            const float *p = svert(&s, l, v);
            e_speed = fmax(e_speed, fabs(p[3] / spd - 1.0));
            e_rad = fmax(e_rad, fabs(hypot(p[1] - an->y0, p[2] - an->z0) - rr));
            if (v == 0) continue;
            const float *q = p - 5;
            double d = wrap_pi(atan2(p[2] - an->z0, p[1] - an->y0) - atan2(q[2] - an->z0, q[1] - an->y0));
            if (!(p[0] > q[0]) || !(d > 0.0)) spiral = false; /* x and swept angle both grow along the line */
            ang += d;
        }
        e_ang = fmax(e_ang, fabs(ang / (om * pe[4]) - 1.0));
        e_time = fmax(e_time, fabs(pe[4] / ((pe[0] - p0[0]) / an->U) - 1.0));
        exit_x = fmin(exit_x, pe[0]);
        if (l % 8 == 0)
            printf("         r=%.0f: %u verts, swept %.2f rad (analytic %.2f), time %.1f\n", rr, s.line_count[l], ang,
                   om * pe[4], pe[4]);
    }
    CHECK(spiral, "lines spiral: x and swept angle around the axis increase monotonically");
    CHECK(e_ang < 0.02, "swept angle vs Omega(r)*T: max error %.3f%%", 100 * e_ang);
    CHECK(e_time < 0.005, "flight time vs dx/U: max error %.4f%%", 100 * e_time);
    CHECK(e_rad < 0.05, "radius drift from the seed circle: max %.4f cells", e_rad);
    CHECK(e_speed < 0.01, "vertex scalar (NULL -> speed) vs analytic: max error %.3f%%", 100 * e_speed);
    CHECK(exit_x > f->nx - 1e-3, "lines reach the outlet face (min end x = %.4f)", exit_x);

    float *uxf = malloc((size_t)f->nx * f->ny * f->nz * sizeof(float));
    vis_compute_field(&f->g, VIS_UX, uxf, NULL, pn);
    vis_streamlines(&f->g, uxf, seeds, 24, &sp, &s, pn);
    double e_sc = 0;
    for (uint32_t i = 0; i < s.nverts; i++) e_sc = fmax(e_sc, fabs(s.verts[5 * i + 3] - an->U));
    CHECK(s.nlines == 24 && e_sc < 1e-6, "scalar field sampled per vertex (UX = %.2f): max error %.2g", an->U, e_sc);
    free(uxf);

    /* lines aimed at the sphere stop at its surface */
    float sph[3 * 9];
    for (int a = 0; a < 9; a++) {
        sph[3 * a] = 12.0f;
        sph[3 * a + 1] = (float)(an->sc[1] - 4.0 + 2.0 * (a % 3 - 1));
        sph[3 * a + 2] = (float)(an->sc[2] + 2.0 * (a / 3 - 1));
    }
    vis_streamlines(&f->g, NULL, sph, 9, &sp, &s, pn);
    double dmin = 1e9, dmax = 0;
    for (uint32_t l = 0; l < s.nlines; l++) {
        const float *pe = svert(&s, l, s.line_count[l] - 1);
        double d = sqrt(pow(pe[0] - an->sc[0], 2) + pow(pe[1] - an->sc[1], 2) + pow(pe[2] - an->sc[2], 2));
        dmin = fmin(dmin, d), dmax = fmax(dmax, d);
    }
    CHECK(s.nlines == 9 && lines_sane(&s) && dmin > an->sr - 1.0 && dmax < an->sr + 1.5,
          "9 lines into the sphere stop at its surface: end distance from centre in [%.3f, %.3f], R = %.0f", dmin, dmax,
          an->sr);

    /* both directions: one polyline inlet -> outlet; its downstream part equals the one-way line */
    StreamParams spb = sp;
    spb.both_directions = true;
    float mid[3 * 8];
    for (int a = 0; a < 8; a++) {
        mid[3 * a] = 64.25f;
        mid[3 * a + 1] = (float)(an->y0 + 4.0 * cos(2.0 * PI * a / 8.0));
        mid[3 * a + 2] = (float)(an->z0 + 4.0 * sin(2.0 * PI * a / 8.0));
    }
    vis_streamlines(&f->g, NULL, mid, 8, &spb, &sb, pn);
    vis_streamlines(&f->g, NULL, mid, 8, &sp, &s, pn);
    bool okb = sb.nlines == 8 && s.nlines == 8 && lines_sane(&sb) && lines_sane(&s);
    double e_tt = 0, e_tail = 0;
    for (uint32_t l = 0; okb && l < 8; l++) {
        uint32_t nb = sb.line_count[l], ns = s.line_count[l];
        const float *b0 = svert(&sb, l, 0), *be = svert(&sb, l, nb - 1);
        okb = okb && nb > ns && b0[0] < 1e-3f && be[0] > f->nx - 1e-3f;
        e_tt = fmax(e_tt, fabs(be[4] / (f->nx / an->U) - 1.0));
        const float *seedv = svert(&sb, l, nb - ns);
        okb = okb && !memcmp(seedv, mid + 3 * l, 3 * sizeof(float));
        for (uint32_t v = 0; okb && v < ns; v++) {
            const float *pb = svert(&sb, l, nb - ns + v), *ps = svert(&s, l, v);
            okb = !memcmp(pb, ps, 4 * sizeof(float));
            e_tail = fmax(e_tail, fabs((pb[4] - seedv[4]) - ps[4]) / (ps[4] + 1.0));
        }
    }
    CHECK(okb && e_tt < 0.005 && e_tail < 1e-4,
          "both_directions: 8 lines x 0 -> %d, seed inside, downstream part identical (time err %.2g), total time err %.3f%%",
          f->nx, e_tail, 100 * e_tt);

    /* invalid seeds produce no line; buffers are reused/grown across calls */
    uint32_t nan_bits = 0x7FC00000u;
    float fnan;
    memcpy(&fnan, &nan_bits, 4);
    float bad[3 * 4] = {(float)an->sc[0], (float)an->sc[1], (float)an->sc[2], -5, 10, 10, fnan, 1, 1, 10, 48, 80};
    vis_streamlines(&f->g, NULL, bad, 4, &sp, &s, pn);
    CHECK(s.nlines == 1 && !memcmp(svert(&s, 0, 0), bad + 9, 3 * sizeof(float)),
          "seeds in a solid / outside / NaN give no line (%u lines from 4 seeds)", s.nlines);
    int nbig = 2000;
    float *big = malloc(3 * (size_t)nbig * sizeof(float));
    for (int a = 0; a < nbig; a++) {
        big[3 * a] = 1.0f + (a % 7);
        big[3 * a + 1] = 1.0f + (float)((a * 37) % (f->ny - 2));
        big[3 * a + 2] = 1.0f + (float)((a * 53) % (f->nz - 2));
    }
    vis_streamlines(&f->g, NULL, big, nbig, &spb, &s, pn);
    uint32_t big_lines = s.nlines, big_verts = s.nverts, cap = s.cap_verts;
    bool okr = lines_sane(&s);
    vis_streamlines(&f->g, NULL, seeds, 24, &sp, &s, pn);
    okr = okr && lines_sane(&s) && s.nlines == 24 && s.cap_verts == cap;
    vis_streamlines(&f->g, NULL, big, nbig, &spb, &s, pn);
    okr = okr && s.nlines == big_lines && s.nverts == big_verts && lines_sane(&s);
    CHECK(okr, "reuse: 2000 seeds (%u lines, %u verts) -> 24 seeds -> 2000 seeds, buffers reused", big_lines, big_verts);
    free(big);
    streamlines_free(&s);
    streamlines_free(&sb);
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* 3. isosurfaces                                                                                                     */

typedef struct {
    uint64_t key;
    uint32_t count, fwd;
} EdgeSlot;

/* Edges not shared by exactly two triangles, once in each direction (0 = closed, consistently oriented). */
static size_t mesh_bad_edges(const IsoMesh *m) {
    size_t nt = m->nindices / 3, cap = 16, bad = 0;
    int bits = 4;
    while (cap < nt * 4) cap <<= 1, bits++;
    EdgeSlot *h = calloc(cap, sizeof *h);
    for (size_t t = 0; t < nt; t++)
        for (int e = 0; e < 3; e++) {
            uint32_t a = m->indices[3 * t + e], b = m->indices[3 * t + (e + 1) % 3];
            if (a == b) {
                bad++;
                continue;
            }
            uint64_t key = ((uint64_t)(a < b ? a : b) << 32 | (a < b ? b : a)) + 1;
            size_t i = (size_t)((key * 0x9E3779B97F4A7C15ull) >> (64 - bits));
            while (h[i].key && h[i].key != key) i = (i + 1) & (cap - 1);
            h[i].key = key;
            h[i].count++;
            h[i].fwd += a < b;
        }
    for (size_t i = 0; i < cap; i++)
        if (h[i].key && (h[i].count != 2 || h[i].fwd != 1)) bad++;
    free(h);
    return bad;
}

typedef struct {
    double area, outward, winding, unit, rmin, rmax, rmean, rstd, cerr, xmin, xmax;
} MeshStats;

/* tube: distances/outward direction relative to the axis through c along x, else relative to the point c */
static MeshStats mesh_stats(const IsoMesh *m, const double c[3], bool tube) {
    MeshStats st = {0, 0, 0, 0, 1e30, 0, 0, 0, 0, 1e30, -1e30};
    size_t nout = 0, nunit = 0, nw = 0, nt = m->nindices / 3;
    double r2sum = 0;
    for (uint32_t v = 0; v < m->nverts; v++) {
        const float *p = m->verts + 7 * (size_t)v;
        double d[3] = {tube ? 0.0 : p[0] - c[0], p[1] - c[1], p[2] - c[2]};
        double r = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), nl = sqrt(p[3] * p[3] + p[4] * p[4] + p[5] * p[5]);
        st.rmin = fmin(st.rmin, r), st.rmax = fmax(st.rmax, r), st.rmean += r, r2sum += r * r;
        st.xmin = fmin(st.xmin, p[0]), st.xmax = fmax(st.xmax, p[0]);
        nout += p[3] * d[0] + p[4] * d[1] + p[5] * d[2] > 0.0;
        nunit += fabs(nl - 1.0) < 1e-3;
        st.cerr = fmax(st.cerr, fabs(p[6] - p[0]));
    }
    for (size_t t = 0; t < nt; t++) {
        const float *a = m->verts + 7 * (size_t)m->indices[3 * t], *b = m->verts + 7 * (size_t)m->indices[3 * t + 1],
                    *q = m->verts + 7 * (size_t)m->indices[3 * t + 2];
        double e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {q[0] - a[0], q[1] - a[1], q[2] - a[2]};
        double fn[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        st.area += 0.5 * sqrt(fn[0] * fn[0] + fn[1] * fn[1] + fn[2] * fn[2]);
        nw += fn[0] * (a[3] + b[3] + q[3]) + fn[1] * (a[4] + b[4] + q[4]) + fn[2] * (a[5] + b[5] + q[5]) > 0.0;
    }
    double nv = m->nverts ? m->nverts : 1;
    st.rmean /= nv;
    st.rstd = sqrt(fmax(0.0, r2sum / nv - st.rmean * st.rmean));
    st.outward = nout / nv, st.unit = nunit / nv, st.winding = nt ? (double)nw / nt : 0;
    return st;
}

static bool mesh_equal(const IsoMesh *a, const IsoMesh *b) {
    return a->nverts == b->nverts && a->nindices == b->nindices &&
           !memcmp(a->verts, b->verts, (size_t)a->nverts * 7 * sizeof(float)) &&
           !memcmp(a->indices, b->indices, (size_t)a->nindices * sizeof(uint32_t));
}

/* analytic Lamb-Oseen Q(r) = w^2/4 - S_rt^2, S_rt = r/2 d/dr(u_t/r); first r where Q = frac * Q(0) */
static double lamb_oseen_q_radius(const Analytic *an, double frac) {
    double w0 = an->gamma / (PI * an->rc * an->rc), q0 = 0.25 * w0 * w0, h = 1e-4;
    for (double r = 1e-3; r < 4.0 * an->rc; r += 1e-3) {
        double w = w0 * exp(-r * r / (an->rc * an->rc));
#define LO_F(rr) (an->gamma / (2.0 * PI) * (1.0 - exp(-(rr) * (rr) / (an->rc * an->rc))) / ((rr) * (rr)))
        double S = 0.5 * r * (LO_F(r + h) - LO_F(r - h)) / (2.0 * h);
#undef LO_F
        if (0.25 * w * w - S * S < frac * q0) return r;
    }
    return -1.0;
}

static void test_iso(const Flow *fl, const Analytic *an, ThreadPool *p1, ThreadPool *pn) {
    printf("\n== 3. isosurfaces (surface nets) ==\n");
    const int N = 64;
    const double c[3] = {32.3, 31.7, 32.1}, R = 20.0, A = 4.0 * PI * R * R;
    size_t n = (size_t)N * N * N;
    float *sf = malloc(n * sizeof(float)), *xf = malloc(n * sizeof(float));
    for (int k = 0; k < N; k++)
        for (int j = 0; j < N; j++)
            for (int i = 0; i < N; i++) {
                size_t id = (size_t)i + (size_t)N * ((size_t)j + (size_t)N * k);
                double dx = i + 0.5 - c[0], dy = j + 0.5 - c[1], dz = k + 0.5 - c[2];
                sf[id] = (float)(R - sqrt(dx * dx + dy * dy + dz * dz));
                xf[id] = (float)(i + 0.5);
            }
    VisGrid g = {N, N, N, NULL, NULL, NULL, NULL, NULL};
    IsoMesh m = {0}, m1 = {0};
    IsoParams ip = {.iso = 0.0f, .downsample = 1, .exclude_near_solid = false};
    for (int ds = 1; ds <= 2; ds++) {
        ip.downsample = ds;
        vis_isosurface(&g, sf, xf, &ip, &m, pn);
        MeshStats st = mesh_stats(&m, c, false);
        size_t bad = mesh_bad_edges(&m);
        printf("         sphere R=20 ds %d: %u verts, %u tris, vertex radius [%.3f, %.3f]\n", ds, m.nverts, m.nindices / 3,
               st.rmin, st.rmax);
        CHECK(fabs(st.area / A - 1.0) < (ds == 1 ? 0.03 : 0.05), "ds %d: area %.1f vs 4 pi R^2 = %.1f (%+.2f%%)", ds,
              st.area, A, 100.0 * (st.area / A - 1.0));
        CHECK(st.outward > 0.99 && st.unit > 0.999, "ds %d: normals outward for %.3f%% of vertices (unit length %.3f%%)",
              ds, 100 * st.outward, 100 * st.unit);
        CHECK(st.winding > 0.99, "ds %d: triangle winding agrees with vertex normals for %.3f%% of triangles", ds,
              100 * st.winding);
        CHECK(m.nindices > 0 && bad == 0, "ds %d: mesh closed, every edge shared by 2 triangles in opposite directions (%zu bad)",
              ds, bad);
        CHECK(st.cerr < 2e-3, "ds %d: colour field sampled at vertices (max err %.2g)", ds, st.cerr);
        vis_isosurface(&g, sf, xf, &ip, &m1, p1);
        CHECK(mesh_equal(&m, &m1), "ds %d: 1 thread == %d threads (bitwise)", ds, pool_size(pn));
    }
    ip.iso = 25.0f; /* nothing above: empty mesh, buffers kept */
    vis_isosurface(&g, sf, NULL, &ip, &m, pn);
    CHECK(m.nverts == 0 && m.nindices == 0 && m.verts, "iso above the field maximum gives an empty mesh");

    /* Q-criterion of the analytic vortex: a tube along x */
    size_t nf = (size_t)fl->nx * fl->ny * fl->nz;
    float *q = malloc(nf * sizeof(float));
    vis_compute_field(&fl->g, VIS_QCRITERION, q, NULL, pn);
    float q0 = q[idx3(fl, 64, (int)an->y0, (int)an->z0)];
    const double axis[3] = {0.0, an->y0, an->z0};
    double r_an = lamb_oseen_q_radius(an, 0.25);
    ip = (IsoParams){.iso = 0.25f * q0, .downsample = 1, .exclude_near_solid = true};
    for (int ds = 1; ds <= 2; ds++) {
        ip.downsample = ds;
        vis_isosurface(&fl->g, q, NULL, &ip, &m, pn);
        MeshStats st = mesh_stats(&m, axis, true);
        CHECK(m.nindices / 3 > 1000 && st.xmin < 2.5 * ds && st.xmax > fl->nx - 2.5 * ds && st.rstd < 0.25 &&
                  fabs(st.rmean - r_an) < 0.4 && st.outward > 0.99 && st.winding > 0.99,
              "ds %d: Q = Q0/4 surface is a tube along x: %u tris, x [%.1f, %.1f], radius %.3f +- %.3f (analytic %.3f), "
              "outward %.2f%%, winding %.2f%%",
              ds, m.nindices / 3, st.xmin, st.xmax, st.rmean, st.rstd, r_an, 100 * st.outward, 100 * st.winding);
    }
    ip.downsample = 1;
    ip.exclude_near_solid = false;
    vis_isosurface(&fl->g, q, NULL, &ip, &m, pn);
    size_t stray = 0;
    for (uint32_t v = 0; v < m.nverts; v++)
        stray += hypot(m.verts[7 * (size_t)v + 1] - an->y0, m.verts[7 * (size_t)v + 2] - an->z0) > 2.0 * an->rc;
    printf("         without exclude_near_solid: %u tris, %zu vertices away from the tube (sphere wall artefacts)\n",
           m.nindices / 3, stray);
    isomesh_free(&m);
    isomesh_free(&m1);
    free(q);
    free(sf);
    free(xf);
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* 5. images (BMP)                                                                                                    */

typedef struct {
    int w, h;
    uint8_t *rgb; /* top-down rows */
} Img;

static Img img_new(int w, int h, uint8_t bg) {
    Img im = {w, h, malloc((size_t)w * h * 3)};
    memset(im.rgb, bg, (size_t)w * h * 3);
    return im;
}

static void img_put(Img *im, int x, int y, const uint8_t c[3]) {
    if (x < 0 || y < 0 || x >= im->w || y >= im->h) return;
    memcpy(im->rgb + 3 * ((size_t)y * im->w + x), c, 3);
}

static void img_line(Img *im, double x0, double y0, double x1, double y1, const uint8_t c[3]) {
    int n = (int)ceil(fmax(fabs(x1 - x0), fabs(y1 - y0))) + 1;
    for (int i = 0; i <= n; i++) {
        double t = (double)i / n;
        img_put(im, (int)(x0 + t * (x1 - x0)), (int)(y0 + t * (y1 - y0)), c);
    }
}

static void colormap(double t, uint8_t out[3]) { /* viridis-like */
    static const double stops[5][3] = {{68, 1, 84}, {59, 82, 139}, {33, 145, 140}, {94, 201, 98}, {253, 231, 37}};
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    double s = t * 4.0;
    int i = s >= 4.0 ? 3 : (int)s;
    double f = s - i;
    for (int q = 0; q < 3; q++) out[q] = (uint8_t)(stops[i][q] + f * (stops[i + 1][q] - stops[i][q]));
}

static bool img_write_bmp(const Img *im, const char *path) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    int pad = (4 - (im->w * 3) % 4) % 4;
    uint32_t data = (uint32_t)((im->w * 3 + pad) * im->h);
    uint8_t hd[54] = {'B', 'M'};
    uint32_t v32[][2] = {{2, 54 + data}, {10, 54}, {14, 40}, {18, (uint32_t)im->w}, {22, (uint32_t)im->h}, {34, data}};
    for (size_t i = 0; i < ARRAY_LEN(v32); i++)
        for (int b = 0; b < 4; b++) hd[v32[i][0] + b] = (uint8_t)(v32[i][1] >> (8 * b));
    hd[26] = 1, hd[28] = 24;
    fwrite(hd, 1, 54, fp);
    uint8_t zero[3] = {0, 0, 0};
    for (int y = im->h - 1; y >= 0; y--) {
        for (int x = 0; x < im->w; x++) {
            const uint8_t *p = im->rgb + 3 * ((size_t)y * im->w + x);
            uint8_t bgr[3] = {p[2], p[1], p[0]};
            fwrite(bgr, 1, 3, fp);
        }
        fwrite(zero, 1, (size_t)pad, fp);
    }
    return fclose(fp) == 0;
}

/* Slice of a cell field, normal to z (horizontal x, vertical y) or to x (horizontal y, vertical z). Solids grey. */
static void slice_bmp(const char *path, const Flow *f, const float *v, bool normal_x, int index, float lo, float hi,
                      int scale) {
    int W = (normal_x ? f->ny : f->nx) * scale, H = (normal_x ? f->nz : f->ny) * scale;
    Img im = img_new(W, H, 0);
    for (int py = 0; py < H; py++)
        for (int px = 0; px < W; px++) {
            int a = px / scale, b = (H - 1 - py) / scale;
            size_t id = normal_x ? idx3(f, index, a, b) : idx3(f, a, b, index);
            uint8_t c[3] = {110, 110, 110};
            if (!f->solid[id]) colormap((v[id] - lo) / (hi - lo), c);
            img_put(&im, px, py, c);
        }
    printf("  wrote %s (%dx%d)\n", path, W, H);
    img_write_bmp(&im, path);
    free(im.rgb);
}

/* Orthographic projections: side view (x right, vert = axis `up`) and end view (the other axis right, `up` up). */
static void lines_bmp(const char *path, const StreamlineSet *s, int nx, int ny, int nz, int up, int scale, float smin,
                      float smax) {
    int other = up == 2 ? 1 : 2, next[3] = {nx, ny, nz};
    int W1 = nx * scale, W2 = next[other] * scale, H = next[up] * scale, gap = 8;
    Img im = img_new(W1 + gap + W2, H, 16);
    for (int y = 0; y < H; y++)
        for (int x = W1; x < W1 + gap; x++) img_put(&im, x, y, (uint8_t[3]){60, 60, 60});
    for (uint32_t l = 0; l < s->nlines; l++)
        for (uint32_t v = 1; v < s->line_count[l]; v++) {
            const float *p = s->verts + 5 * ((size_t)s->line_first[l] + v), *q = p - 5;
            uint8_t c[3];
            colormap((p[3] - smin) / (smax - smin), c);
            img_line(&im, q[0] * scale, H - q[up] * scale, p[0] * scale, H - p[up] * scale, c);
            img_line(&im, W1 + gap + q[other] * scale, H - q[up] * scale, W1 + gap + p[other] * scale, H - p[up] * scale, c);
        }
    printf("  wrote %s (%dx%d, %u lines)\n", path, im.w, im.h, s->nlines);
    img_write_bmp(&im, path);
    free(im.rgb);
}

static void images_analytic(const Flow *f, const Analytic *an, ThreadPool *pn) {
    printf("\n== 5a. images of the analytic flow ==\n");
    size_t n = (size_t)f->nx * f->ny * f->nz;
    float *w = malloc(n * sizeof(float));
    VisRange r;
    vis_compute_field(&f->g, VIS_VORTICITY, w, &r, pn);
    slice_bmp(OUTDIR "/vort_slice_yz_x40.bmp", f, w, true, (int)an->sc[0], 0.0f, r.hi, 4);
    float seeds[3 * (256 + 32)];
    int ns = 0;
    for (int b = 0; b < 16; b++)
        for (int a = 0; a < 16; a++, ns++)
            seeds[3 * ns] = 1.0f, seeds[3 * ns + 1] = 3.0f + 6.0f * a, seeds[3 * ns + 2] = 3.0f + 6.0f * b;
    for (int a = 0; a < 32; a++, ns++) {
        double rr = a % 2 ? 3.0 : 8.0, ang = 2.0 * PI * a / 32.0;
        seeds[3 * ns] = 1.0f, seeds[3 * ns + 1] = (float)(an->y0 + rr * cos(ang)), seeds[3 * ns + 2] = (float)(an->z0 + rr * sin(ang));
    }
    StreamlineSet s = {0};
    StreamParams sp = {.max_steps = 1500, .step = 0.5f, .both_directions = false, .min_speed = 1e-4f};
    vis_streamlines(&f->g, NULL, seeds, ns, &sp, &s, pn);
    lines_bmp(OUTDIR "/streamlines_analytic.bmp", &s, f->nx, f->ny, f->nz, 2, 4, (float)an->U, (float)hypot(an->U, 0.05));
    streamlines_free(&s);
    free(w);
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* 4. performance                                                                                                     */

#define TIME_BEST(reps, out_ms, stmt)                                                                                \
    do {                                                                                                              \
        double best_ = 1e30;                                                                                          \
        for (int rep_ = 0; rep_ < (reps); rep_++) {                                                                   \
            double t0_ = now_seconds();                                                                               \
            stmt;                                                                                                     \
            double dt_ = now_seconds() - t0_;                                                                         \
            best_ = dt_ < best_ ? dt_ : best_;                                                                        \
        }                                                                                                             \
        (out_ms) = best_ * 1e3;                                                                                       \
    } while (0)

static void bench(ThreadPool *p1, ThreadPool *pn, double peak) {
    const int NX = 256, NY = 128, NZ = 128, NS = 2000;
    const double U = 0.02, rc = 5.0, G = 0.04 * 2.0 * PI * rc / peak, w0 = G / (PI * rc * rc);
    const double pos[8][2] = {{28, 28}, {28, 64}, {28, 100}, {64, 28}, {64, 100}, {100, 28}, {100, 64}, {100, 100}};
    Tube tubes[8];
    for (int t = 0; t < 8; t++)
        tubes[t] = (Tube){pos[t][0], pos[t][1], rc, (t % 2 ? -G : G), 3.0 + t % 3, 64.0 * (1 + t % 3), 0.7 * t};
    const double sc[3] = {48.0, 64.0, 64.0};
    printf("\n== 4. performance: %dx%dx%d (%.2fM cells), 8 helical vortex tubes + sphere, U = %.2f ==\n", NX, NY, NZ,
           NX * NY * NZ / 1e6, U);
    Flow f;
    double t0 = now_seconds();
    if (!flow_make(&f, NX, NY, NZ, U, tubes, 8, sc, 10.0, pn)) {
        printf("  out of memory\n");
        return;
    }
    printf("  (flow synthesis %.0f ms)\n", (now_seconds() - t0) * 1e3);
    size_t n = (size_t)NX * NY * NZ;
    float *vort = malloc(n * sizeof(float)), *qf = malloc(n * sizeof(float));
    float *seeds = malloc(3 * NS * sizeof(float)), *inlet = malloc(3 * NS * sizeof(float));
    uint32_t lcg = 12345;
#define RND() ((lcg = lcg * 1664525u + 1013904223u) >> 8) / 16777216.0f
    for (int i = 0; i < NS; i++) {
        seeds[3 * i] = NX * RND(), seeds[3 * i + 1] = NY * RND(), seeds[3 * i + 2] = NZ * RND();
        inlet[3 * i] = 0.5f + 2.0f * RND(), inlet[3 * i + 1] = NY * RND(), inlet[3 * i + 2] = NZ * RND();
    }
#undef RND
    StreamParams spb = {.max_steps = 1500, .step = 0.5f, .both_directions = true, .min_speed = 1e-4f};
    StreamParams sp1 = spb;
    sp1.both_directions = false;
    StreamlineSet s = {0};
    IsoMesh m = {0};
    ThreadPool *pools[2] = {p1, pn};
    double tv[2], tq[2], ts[2], tl[2], ti1[2], ti2[2];
    float qiso = (float)(0.05 * 0.25 * w0 * w0);
    for (int p = 0; p < 2; p++) {
        ThreadPool *pool = pools[p];
        int reps = p == 0 ? 2 : 5;
        VisRange r;
        printf("  -- %d thread(s) (best of %d)\n", pool_size(pool), reps);
        TIME_BEST(reps, tv[p], vis_compute_field(&f.g, VIS_VORTICITY, vort, &r, pool));
        printf("  vorticity field        %8.2f ms   range [%.3g %.3g] lo/hi [%.3g %.3g]\n", tv[p], r.min, r.max, r.lo, r.hi);
        TIME_BEST(reps, tq[p], vis_compute_field(&f.g, VIS_QCRITERION, qf, &r, pool));
        printf("  Q-criterion field      %8.2f ms   range [%.3g %.3g] lo/hi [%.3g %.3g]\n", tq[p], r.min, r.max, r.lo, r.hi);
        TIME_BEST(reps, ts[p], vis_streamlines(&f.g, vort, seeds, NS, &spb, &s, pool));
        printf("  streamlines both dirs  %8.2f ms   %d random seeds -> %u lines, %u verts (%.0f steps/line)\n", ts[p], NS,
               s.nlines, s.nverts, (double)s.nverts / (s.nlines ? s.nlines : 1));
        TIME_BEST(reps, tl[p], vis_streamlines(&f.g, vort, inlet, NS, &sp1, &s, pool));
        printf("  streamlines from inlet %8.2f ms   %d seeds -> %u lines, %u verts (%.0f steps/line)\n", tl[p], NS,
               s.nlines, s.nverts, (double)s.nverts / (s.nlines ? s.nlines : 1));
        StreamParams spl = sp1;
        spl.step = 0.1f; /* lines cannot exit within 1500 steps: the 2000 seeds x 1500 steps worst case */
        double tml;
        TIME_BEST(p == 0 ? 1 : 3, tml, vis_streamlines(&f.g, vort, inlet, NS, &spl, &s, pool));
        printf("  streamlines 1500 steps %8.2f ms   step 0.1: %u lines, %u verts (%.0f steps/line, %.1f ns/vertex)\n",
               tml, s.nlines, s.nverts, (double)s.nverts / (s.nlines ? s.nlines : 1), tml * 1e6 / (s.nverts ? s.nverts : 1));
        IsoParams ip = {.iso = qiso, .downsample = 1, .exclude_near_solid = true};
        TIME_BEST(reps, ti1[p], vis_isosurface(&f.g, qf, vort, &ip, &m, pool));
        printf("  isosurface Q ds1       %8.2f ms   %u verts, %u tris\n", ti1[p], m.nverts, m.nindices / 3);
        ip.downsample = 2;
        TIME_BEST(reps, ti2[p], vis_isosurface(&f.g, qf, vort, &ip, &m, pool));
        printf("  isosurface Q ds2       %8.2f ms   %u verts, %u tris\n", ti2[p], m.nverts, m.nindices / 3);
    }
    printf("  speedup 1 -> %d threads: vort %.1fx  Q %.1fx  lines %.1fx / %.1fx  iso %.1fx / %.1fx\n", pool_size(pn),
           tv[0] / tv[1], tq[0] / tq[1], ts[0] / ts[1], tl[0] / tl[1], ti1[0] / ti1[1], ti2[0] / ti2[1]);

    printf("\n== 5b. images of the benchmark flow ==\n");
    VisRange r;
    vis_compute_field(&f.g, VIS_VORTICITY, vort, &r, pn);
    slice_bmp(OUTDIR "/vort_slice_xy_z64.bmp", &f, vort, false, NZ / 2, 0.0f, r.hi, 2);
    slice_bmp(OUTDIR "/vort_slice_yz_x128.bmp", &f, vort, true, NX / 2, 0.0f, r.hi, 3);
    vis_streamlines(&f.g, vort, seeds, 400, &spb, &s, pn);
    lines_bmp(OUTDIR "/streamlines_bench.bmp", &s, NX, NY, NZ, 1, 2, 0.0f, r.hi);
    streamlines_free(&s);
    isomesh_free(&m);
    free(vort);
    free(qf);
    free(seeds);
    free(inlet);
    flow_free(&f);
}

static void test_robustness(ThreadPool *pn);

int main(int argc, char **argv) {
    int nthreads = argc > 1 ? atoi(argv[1]) : 8;
    mkdir("build", 0755);
    mkdir(OUTDIR, 0755);
    ThreadPool *p1 = pool_create(1), *pn = pool_create(nthreads);
    double peak = lamb_oseen_peak_factor();
    Analytic an = {.U = 0.05, .rc = 6.0, .y0 = 48.5, .z0 = 48.5, .sc = {40.0, 48.0, 20.0}, .sr = 10.0};
    an.gamma = 0.05 * 2.0 * PI * an.rc / peak;
    Tube tube = {an.y0, an.z0, an.rc, an.gamma, 0.0, 1.0, 0.0};
    Flow f;
    if (!flow_make(&f, 128, 96, 96, an.U, &tube, 1, an.sc, an.sr, pn)) return 1;
    printf("vistest: %d threads; analytic flow gamma = %.4f, w0 = %.5f, peak swirl 0.05 at r = 1.12 rc\n", nthreads,
           an.gamma, an.gamma / (PI * an.rc * an.rc));
    test_fields(&f, &an, p1, pn);
    test_streamlines(&f, &an, pn);
    test_iso(&f, &an, p1, pn);
    test_robustness(pn);
    images_analytic(&f, &an, pn);
    flow_free(&f);
    bench(p1, pn, peak);
    pool_destroy(p1);
    pool_destroy(pn);
    printf("\n%d checks passed, %d failed\n", n_pass, n_fail);
    return n_fail ? 1 : 0;
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* 6. robustness: NaN/Inf inputs, zero gradients, tiny grids, NULL pool                                              */

static bool mesh_valid(const IsoMesh *m) {
    if (m->nindices % 3) return false;
    for (uint32_t i = 0; i < m->nindices; i++)
        if (m->indices[i] >= m->nverts) return false;
    for (size_t i = 0; i < (size_t)m->nverts * 7; i++)
        if (!is_finite_f32(m->verts[i])) return false;
    return true;
}

static void test_robustness(ThreadPool *pn) {
    printf("\n== 6. robustness ==\n");
    uint32_t nan_bits = 0x7FC00000u, inf_bits = 0x7F800000u;
    float fnan, finf;
    memcpy(&fnan, &nan_bits, 4);
    memcpy(&finf, &inf_bits, 4);
    const int N = 40;
    size_t n = (size_t)N * N * N;
    Tube tube = {20.5, 20.5, 4.0, 1.0, 0.0, 1.0, 0.0};
    const double sc[3] = {10.0, 8.0, 8.0};
    Flow f;
    flow_make(&f, N, N, N, 0.05, &tube, 1, sc, 4.0, pn);
    for (int k = 16; k < 24; k++) /* a diverged block downstream on the vortex axis */
        for (int j = 16; j < 24; j++)
            for (int i = 28; i < 32; i++) {
                size_t id = idx3(&f, i, j, k);
                f.ux[id] = (i + j + k) % 2 ? fnan : finf;
                f.uy[id] = -finf;
                f.rho[id] = fnan;
            }
    float *out = malloc(n * sizeof(float)), *out2 = malloc(n * sizeof(float));
    bool fin = true, rng = true;
    for (int fld = 0; fld < VIS_FIELD_COUNT; fld++) {
        VisRange r;
        vis_compute_field(&f.g, (VisField)fld, out, &r, pn);
        for (size_t i = 0; i < n; i++) fin = fin && is_finite_f32(out[i]);
        rng = rng && is_finite_f32(r.min) && is_finite_f32(r.max) && r.min <= r.lo && r.lo <= r.hi && r.hi <= r.max;
        vis_compute_field(&f.g, (VisField)fld, out2, NULL, NULL);
        fin = fin && !memcmp(out, out2, n * sizeof(float));
    }
    CHECK(fin && rng, "fields with NaN/Inf velocity and rho: outputs finite, ranges ordered, NULL pool/range identical");

    float seeds[3 * 8];
    for (int a = 0; a < 8; a++)
        seeds[3 * a] = 2.0f, seeds[3 * a + 1] = (float)(20.5 + 2.0 * cos(a * PI / 4)),
        seeds[3 * a + 2] = (float)(20.5 + 2.0 * sin(a * PI / 4));
    StreamlineSet s = {0}, s2 = {0};
    StreamParams sp = {.max_steps = 1500, .step = 0.5f, .both_directions = true, .min_speed = 0.0f};
    vis_streamlines(&f.g, NULL, seeds, 8, &sp, &s, pn);
    vis_streamlines(&f.g, NULL, seeds, 8, &sp, &s2, NULL);
    float xmax = 0.0f;
    for (uint32_t i = 0; i < s.nverts; i++) xmax = fmax(xmax, s.verts[5 * i]);
    CHECK(s.nlines == 8 && lines_sane(&s) && xmax < 28.0f && s.nverts == s2.nverts &&
              !memcmp(s.verts, s2.verts, (size_t)s.nverts * 5 * sizeof(float)),
          "streamlines stop before non-finite velocity (max x %.2f), min_speed 0 ok, NULL pool identical", xmax);

    for (int k = 0; k < N; k++) /* sphere distance field sprinkled with NaN and +-Inf */
        for (int j = 0; j < N; j++)
            for (int i = 0; i < N; i++) {
                size_t id = idx3(&f, i, j, k);
                double dx = i + 0.5 - 20.3, dy = j + 0.5 - 19.7, dz = k + 0.5 - 20.1;
                out[id] = (float)(12.0 - sqrt(dx * dx + dy * dy + dz * dz));
                if (id % 7 == 0) out[id] = fnan;
                if (id % 11 == 0) out[id] = finf;
                if (id % 13 == 0) out[id] = -finf;
            }
    IsoMesh m = {0}, m2 = {0};
    bool okm = true;
    for (int ds = 1; ds <= 2; ds++) {
        IsoParams ip = {.iso = 0.0f, .downsample = ds, .exclude_near_solid = true};
        vis_isosurface(&f.g, out, out, &ip, &m, pn);
        vis_isosurface(&f.g, out, out, &ip, &m2, NULL);
        okm = okm && m.nindices > 0 && mesh_valid(&m) && mesh_equal(&m, &m2);
    }
    CHECK(okm, "isosurface of a field with NaN/+-Inf cells: indices in range, vertex data finite, NULL pool identical");

    const int M = 12; /* 3D checkerboard: every interior central difference is zero */
    float cb[12 * 12 * 12];
    for (int i = 0; i < M * M * M; i++) cb[i] = (float)((i % M + (i / M) % M + i / (M * M)) % 2);
    VisGrid gc = {M, M, M, NULL, NULL, NULL, NULL, NULL};
    vis_isosurface(&gc, cb, NULL, &(IsoParams){0.5f, 1, false}, &m, pn);
    bool unit = true;
    for (uint32_t v = 0; v < m.nverts; v++) {
        const float *p = m.verts + 7 * (size_t)v + 3;
        unit = unit && fabs(sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) - 1.0) < 1e-3;
    }
    CHECK(m.nindices > 0 && mesh_valid(&m) && unit, "checkerboard (zero gradients): %u tris, fallback normals unit length",
          m.nindices / 3);

    float tq[64], tu[64], tr[64], f2[8] = {1, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 64; i++) tu[i] = 0.01f * (float)(i % 7), tr[i] = 1.0f;
    VisGrid gt = {1, 8, 8, tr, tu, tu, tu, NULL}, g2 = {2, 2, 2, NULL, NULL, NULL, NULL, NULL};
    VisRange r;
    bool okf = true, oki, okl;
    for (int fld = 0; fld < VIS_FIELD_COUNT; fld++) {
        vis_compute_field(&gt, (VisField)fld, tq, &r, pn);
        for (int i = 0; i < 64; i++) okf = okf && is_finite_f32(tq[i]);
    }
    vis_isosurface(&gt, tu, NULL, &(IsoParams){0.03f, 1, false}, &m, pn);
    oki = m.nindices == 0;
    vis_isosurface(&g2, f2, NULL, &(IsoParams){0.5f, 1, false}, &m, pn);
    oki = oki && m.nindices == 0;
    float seed1[3] = {0.5f, 4.0f, 4.0f};
    vis_streamlines(&gt, NULL, seed1, 1, &sp, &s, pn);
    okl = lines_sane(&s) && vis_sample_scalar(tu, 1, 8, 8, 0.5f, 2.5f, 2.5f) == tu[2 * 8 + 2];
    CHECK(okf && oki && okl, "tiny grids (1x8x8, 2x2x2): finite fields %d, empty meshes %d, sane lines/sample %d (%u lines)",
          okf, oki, okl, s.nlines);
    streamlines_free(&s);
    streamlines_free(&s2);
    isomesh_free(&m);
    isomesh_free(&m2);
    free(out);
    free(out2);
    flow_free(&f);
}
