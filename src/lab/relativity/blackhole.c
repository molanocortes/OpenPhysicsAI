/* blackhole.c - see blackhole.h. M = 1 throughout. */
#include "blackhole.h"

#include "../../threads.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void bh_spec_defaults(BhSpec *s) {
    memset(s, 0, sizeof *s);
    s->cam_r = 30, s->incl_deg = 82, s->azim_deg = 0, s->fov_deg = 38;
    s->w = 960, s->h = 540;
    s->disk = true, s->r_in = 6, s->r_out = 22, s->t_in_k = 9000;
    s->stars = true, s->aa = 2;
}

static inline void cross(const double *a, const double *b, double *o) {
    o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
}
static inline double dot(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline void norm(double *a) {
    double n = sqrt(dot(a, a));
    if (n > 0) a[0] /= n, a[1] /= n, a[2] /= n;
}

/* one RK4 step of u'' = 3 u^2 - u */
static void rk4(double *u, double *v, double h) {
    double k1u = *v, k1v = 3 * *u * *u - *u;
    double u2 = *u + 0.5 * h * k1u, v2 = *v + 0.5 * h * k1v, k2u = v2, k2v = 3 * u2 * u2 - u2;
    double u3 = *u + 0.5 * h * k2u, v3 = *v + 0.5 * h * k2v, k3u = v3, k3v = 3 * u3 * u3 - u3;
    double u4 = *u + h * k3u, v4 = *v + h * k3v, k4u = v4, k4v = 3 * u4 * u4 - u4;
    *u += h / 6 * (k1u + 2 * k2u + 2 * k3u + k4u);
    *v += h / 6 * (k1v + 2 * k2v + 2 * k3v + k4v);
}

/* the step in phi: small near the hole and at the turning point, larger far away */
static inline double step(double u) { return fmin(0.02, fmax(0.0015, 0.02 * (1 - 2.5 * u))); }

double bh_deflection(double b, double *drift) {
    double u = 0, v = 1 / b, phi = 0, h = 1e-4, e0 = 1 / (b * b), emax = 0;
    for (long k = 0; k < 100000000; k++) {
        double up = u, vp = v;
        rk4(&u, &v, h);
        phi += h;
        double e = v * v + u * u - 2 * u * u * u;
        emax = fmax(emax, fabs(e - e0) / e0);
        if (u <= 0 && up > 0) { /* back at infinity: interpolate the crossing */
            phi -= h * u / (u - up);
            break;
        }
        if (u >= 0.5) return NAN; /* captured */
        (void)vp;
    }
    if (drift) *drift = emax;
    return phi - M_PI;
}

bool bh_captured(double b) {
    double u = 0, v = 1 / b, h = 1e-4;
    for (long k = 0; k < 20000000; k++) {
        double up = u;
        rk4(&u, &v, h);
        if (u >= 0.5) return true;
        if (u <= 0 && up > 0) return false;
    }
    return false;
}

void bh_trace(const BhSpec *s, const double cam[3], const double ray[3], BhHit *out) {
    memset(out, 0, sizeof *out);
    double r0 = sqrt(dot(cam, cam)), e1[3] = {cam[0] / r0, cam[1] / r0, cam[2] / r0}, n[3], e2[3];
    cross(cam, ray, n);
    double nn = sqrt(dot(n, n));
    double dr = dot(ray, e1);
    if (nn < 1e-12 * r0) { /* a radial ray: straight in or straight out */
        out->fate = dr < 0 ? 1 : 0;
        memcpy(out->dir, ray, sizeof out->dir);
        return;
    }
    n[0] /= nn, n[1] /= nn, n[2] /= nn;
    cross(n, e1, e2);
    /* the impact parameter from the angle to the radial direction, as a static observer at r0 measures it */
    double dt = dot(ray, e2), sinpsi = fabs(dt), u = 1 / r0;
    double b = r0 * sinpsi / sqrt(1 - 2 * u);
    double v = sqrt(fmax(0, 1 / (b * b) - u * u + 2 * u * u * u)); /* |du/dphi| from the first integral */
    if (dr > 0) v = -v;                                             /* moving outward: u falls */
    double phi = 0, zprev = r0 * e1[2];
    for (int k = 0; k < 400000; k++) {
        double h = step(u);
        double up = u, pp = phi;
        rk4(&u, &v, h);
        phi += h;
        if (u >= 0.5) {
            out->fate = 1;
            return;
        }
        if (u <= 0 || (u < 1e-3 && v < 0)) { /* far away and leaving: its direction at infinity */
            double pinf = u <= 0 ? phi - h * u / (u - up) : phi + u / fabs(v);
            for (int a = 0; a < 3; a++) out->dir[a] = cos(pinf) * e1[a] + sin(pinf) * e2[a];
            out->fate = 0;
            return;
        }
        double r = 1 / u, z = r * (cos(phi) * e1[2] + sin(phi) * e2[2]);
        if ((z > 0) != (zprev > 0)) { /* through the disk's plane */
            double f = zprev / (zprev - z), pc = pp + f * (phi - pp), uc = up + f * (u - up), rc = 1 / uc;
            out->crossings++;
            if (s->disk && rc >= s->r_in && rc <= s->r_out) {
                /* the photon, going forward in time, carries angular momentum -b n; its z part */
                double Lz = -b * n[2], Om = pow(rc, -1.5), ut = 1 / sqrt(1 - 3 / rc);
                out->g = (1 / sqrt(1 - 2 / r0)) / (ut * (1 - Om * Lz));
                double hx = cos(pc) * e1[0] + sin(pc) * e2[0], hy = cos(pc) * e1[1] + sin(pc) * e2[1];
                out->r_hit = rc, out->phi_hit = atan2(hy, hx), out->fate = 2;
                return;
            }
        }
        zprev = z;
        if (phi > 14 * M_PI) { /* orbiting the photon sphere for good: dark */
            out->fate = 1;
            return;
        }
    }
    out->fate = 1;
}

/* the colour of a blackbody at T kelvin (a fit to the Planckian locus in sRGB, Helland's), 0..1 */
static void blackbody(double T, float rgb[3]) {
    double t = T / 100, r, g, b;
    if (t <= 66) r = 255, g = 99.4708025861 * log(t) - 161.1195681661;
    else r = 329.698727446 * pow(t - 60, -0.1332047592), g = 288.1221695283 * pow(t - 60, -0.0755148492);
    if (t >= 66) b = 255;
    else if (t <= 19) b = 0;
    else b = 138.5177312231 * log(t - 10) - 305.0447927307;
    rgb[0] = (float)fmin(1, fmax(0, r / 255)), rgb[1] = (float)fmin(1, fmax(0, g / 255)), rgb[2] = (float)fmin(1, fmax(0, b / 255));
}

static int cmpf(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static inline unsigned hash2(int a, int b) {
    unsigned h = (unsigned)a * 374761393u + (unsigned)b * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static inline unsigned hash3(int a, int b, int c) { return hash2((int)hash2(a, b), c); }
static inline double h01(unsigned h) { return (h & 0xffffff) / 16777216.0; }

/* smooth value noise on a grid periodic in its first coordinate (period px cells), for the disk and the galaxy */
static double vnoise(double x, double y, int px, int seed) {
    int i = (int)floor(x), j = (int)floor(y);
    double fx = x - i, fy = y - j;
    fx = fx * fx * (3 - 2 * fx), fy = fy * fy * (3 - 2 * fy);
    int i0 = ((i % px) + px) % px, i1 = (i0 + 1) % px;
    double a = h01(hash3(i0, j, seed)), b = h01(hash3(i1, j, seed)), c = h01(hash3(i0, j + 1, seed)), d = h01(hash3(i1, j + 1, seed));
    return (1 - fy) * ((1 - fx) * a + fx * b) + fy * ((1 - fx) * c + fx * d);
}

/* the sky far away (a picture, not a measurement): point stars from a hash of the direction, their brightness drawn from
 * a steep power law and their colours from blackbodies of 3000 to 12000 K, and a faint band of a galaxy along one great
 * circle. The stars behind the hole are displaced, stretched and doubled by the bending. pix: a pixel's angle; foot: the
 * angle on the sky that one sample stands for there (large where the bending squeezes the sky into few pixels), over
 * which a star is spread with its light kept, so that squeezed stars are not missed between samples. */
static void sky(const double d[3], double pix, double foot, float rgb[3]) {
    rgb[0] = 0.004f, rgb[1] = 0.005f, rgb[2] = 0.009f;
    static const double NB[3] = {0.28, 0.35, 0.894}; /* the galaxy's pole */
    double sl = fmax(-1, fmin(1, dot(d, NB))), lat = asin(sl);
    double lon = atan2(d[1] * NB[0] - d[0] * NB[1], d[2] - NB[2] * sl);
    double band = exp(-pow(lat / 0.20, 2)), dust = vnoise(lon * 24 / M_PI + 3, lat * 30, 48, 7);
    double glow = band * (0.35 + 0.65 * vnoise(lon * 12 / M_PI, lat * 12, 24, 3)) * (1 - 0.7 * exp(-pow(lat / 0.05, 2)) * dust);
    rgb[0] += (float)(0.050 * glow), rgb[1] += (float)(0.045 * glow), rgb[2] += (float)(0.055 * glow);
    const double N = 70; /* star cells across the unit cube */
    double s0 = fmax(0.9 * pix * N, 0.012), sig = sqrt(s0 * s0 + foot * foot * N * N), keep = (s0 * s0) / (sig * sig);
    double p[3] = {d[0] * N, d[1] * N, d[2] * N};
    int ci = (int)floor(p[0]), cj = (int)floor(p[1]), ck = (int)floor(p[2]);
    if (h01(hash3(ci * 7919 + cj, ck, 11)) < 0.35 + 0.5 * band) {
        double q[3] = {ci + 0.2 + 0.6 * h01(hash3(ci, cj, ck)), cj + 0.2 + 0.6 * h01(hash3(cj, ck, ci + 5)), ck + 0.2 + 0.6 * h01(hash3(ck, ci, cj + 9))};
        double qn = sqrt(dot(q, q)); /* the star's direction on the sky: it belongs to this cell only if it lies in it */
        for (int k = 0; k < 3; k++) q[k] *= N / qn;
        double e[3] = {p[0] - q[0], p[1] - q[1], p[2] - q[2]}, r2 = dot(e, e), bri = 0.02 + 2.5 * pow(h01(hash3(ci, ck, cj + 1)), 9);
        if ((int)floor(q[0]) == ci && (int)floor(q[1]) == cj && (int)floor(q[2]) == ck && r2 < 25 * sig * sig) {
            float c[3];
            blackbody(3000 + 9000 * h01(hash3(ck, cj, ci + 3)), c);
            double a = keep * bri * exp(-r2 / (sig * sig));
            for (int k = 0; k < 3; k++) rgb[k] += (float)(a * c[k]);
        }
    }
}

/* the disk's surface: streaks of brighter and darker gas that orbit at the Keplerian rate, so the inner parts turn
 * faster (a texture for the eye; the emission's radial profile and its shifts are the physics) */
static double disk_texture(const BhSpec *s, double r, double phi) {
    double ph = phi - pow(r, -1.5) * s->time_M, x = ph / (2 * M_PI), y = log(r) * 9;
    x -= floor(x);
    double n = 0.55 * vnoise(x * 24, y, 24, 1) + 0.30 * vnoise(x * 60, y * 2.3, 60, 2) + 0.15 * vnoise(x * 150, y * 5.1, 150, 4);
    double edge = fmin(1, fmax(0, (s->r_out - r) / (0.25 * (s->r_out - s->r_in))));
    return (0.35 + 1.05 * n * n) * edge * edge * (3 - 2 * edge);
}

typedef struct RowCtx {
    const BhSpec *s;
    const double *cam, *f, *right, *u2;
    double th, asp, pix;
    float *rgb, *lum;
} RowCtx;
static void rows(void *vc, int y0, int y1, int tid);

double bh_render(const BhSpec *s, float *rgb) {
    double ci = s->incl_deg * M_PI / 180, ca = s->azim_deg * M_PI / 180;
    double cam[3] = {s->cam_r * sin(ci) * cos(ca), s->cam_r * sin(ci) * sin(ca), s->cam_r * cos(ci)};
    double f[3] = {-cam[0], -cam[1], -cam[2]}, up[3] = {0, 0, 1}, right[3], u2[3];
    norm(f);
    if (fabs(f[2]) > 0.999) up[0] = 1, up[2] = 0;
    cross(f, up, right), norm(right);
    cross(right, f, u2), norm(u2);
    double th = tan(0.5 * s->fov_deg * M_PI / 180), asp = (double)s->w / s->h;
    size_t np = (size_t)s->w * s->h;
    float *lum = malloc(np * sizeof(float));
    RowCtx C = {s, cam, f, right, u2, th, asp, 2 * th / s->h, rgb, lum};
    int nt = cpu_perf_count();
    ThreadPool *pool = nt > 1 ? pool_create(nt) : NULL;
    if (pool) pool_for(pool, s->h, 2, rows, &C), pool_destroy(pool);
    else rows(&C, 0, s->h, 0);
    /* exposure: the 99.8th percentile of the bright part maps to near white (a few hot pixels clip, the rest keeps its
     * colour), unless the scenario fixes it so that a film does not flicker */
    double ref = s->exposure_ref;
    if (!(ref > 0)) {
        size_t m = 0;
        for (size_t i = 0; i < np; i++)
            if (lum[i] > 0.05f) lum[m++] = lum[i];
        qsort(lum, m, sizeof(float), cmpf);
        ref = m ? lum[(size_t)(0.998 * (m - 1))] : 1;
    }
    for (size_t i = 0; i < np * 3; i++) { /* a filmic curve (Narkowicz's fit of ACES), then the sRGB gamma */
        double v = rgb[i] * 0.75 / ref;
        v = (v * (2.51 * v + 0.03)) / (v * (2.43 * v + 0.59) + 0.14);
        rgb[i] = (float)pow(fmin(1, fmax(0, v)), 1 / 2.2);
    }
    free(lum);
    return ref;
}

static void rows(void *vc, int y0, int y1, int tid) {
    (void)tid;
    const RowCtx *C = vc;
    const BhSpec *s = C->s;
    const double *cam = C->cam, *f = C->f, *right = C->right, *u2 = C->u2;
    double th = C->th, asp = C->asp;
    float *rgb = C->rgb, *lum = C->lum;
    int aa = s->aa > 0 ? s->aa : 1;
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < s->w; x++) {
            double acc[3] = {0, 0, 0}, sd[16][3];
            BhHit hits[16];
            int na = aa > 4 ? 4 : aa;
            for (int ay = 0; ay < na; ay++)
                for (int ax = 0; ax < na; ax++) {
                    double sx = (2 * (x + (ax + 0.5) / na) / s->w - 1) * th * asp, sy = (1 - 2 * (y + (ay + 0.5) / na) / s->h) * th;
                    double *d = sd[ay * na + ax];
                    for (int k = 0; k < 3; k++) d[k] = f[k] + sx * right[k] + sy * u2[k];
                    norm(d);
                    bh_trace(s, cam, d, &hits[ay * na + ax]);
                }
            /* the footprint on the sky: how far apart the escaped samples of this pixel land, per sample */
            double foot = 0, mean[3] = {0, 0, 0};
            int ne = 0;
            for (int k = 0; k < na * na; k++)
                if (hits[k].fate == 0) ne++, mean[0] += hits[k].dir[0], mean[1] += hits[k].dir[1], mean[2] += hits[k].dir[2];
            if (ne > 1) {
                norm(mean);
                for (int k = 0; k < na * na; k++)
                    if (hits[k].fate == 0) foot = fmax(foot, acos(fmin(1, dot(mean, hits[k].dir))));
            }
            foot = fmin(foot, 0.05);
            for (int k = 0; k < na * na; k++) {
                const BhHit *hit = &hits[k];
                float c[3] = {0, 0, 0};
                if (hit->fate == 0 && s->stars) sky(hit->dir, C->pix, foot, c);
                else if (hit->fate == 2) {
                    double r = hit->r_hit, T = s->t_in_k * pow(r / s->r_in, -0.75) * pow(fmax(0, 1 - sqrt(s->r_in / r)), 0.25) / 0.488;
                    float bb[3];
                    blackbody(fmax(1000, hit->g * T), bb);
                    /* bolometric, relative to the hottest ring at rest: I = g^4 T^4 */
                    double I = pow(hit->g, 4) * pow(T / s->t_in_k, 4) * 6 * disk_texture(s, r, hit->phi_hit);
                    for (int q = 0; q < 3; q++) c[q] = (float)(I * bb[q]);
                }
                for (int q = 0; q < 3; q++) acc[q] += c[q];
            }
            aa = na;
            size_t i = (size_t)y * s->w + x;
            for (int k = 0; k < 3; k++) rgb[3 * i + k] = (float)(acc[k] / (aa * aa));
            lum[i] = 0.2126f * rgb[3 * i] + 0.7152f * rgb[3 * i + 1] + 0.0722f * rgb[3 * i + 2];
        }
}

void bh_write_frame(const BhSpec *s, const float *rgb, LabWriter *w) {
    /* the picture as a 2D block, one field per channel (the viewer shows r, g and b together as an image) */
    LabBlock blk = {{s->w, s->h, 1}, 0, LAB_PLANE_XY, {0, 0, 0}, {1.0 / s->h, 1.0 / s->h, 1.0 / s->h}};
    size_t n = (size_t)s->w * s->h;
    float *c = malloc(n * sizeof(float));
    lab_part_blocks(w, "image", 1, &blk);
    static const char *NM[3] = {"r", "g", "b"};
    for (int k = 0; k < 3; k++) {
        for (int y = 0; y < s->h; y++) /* the block's first row is its bottom */
            for (int x = 0; x < s->w; x++) c[(size_t)y * s->w + x] = rgb[3 * ((size_t)(s->h - 1 - y) * s->w + x) + k];
        lab_field(w, NM[k], LAB_AT_CELL, n, c);
    }
    free(c);
}

char *bh_header_json(const BhSpec *s, const char *title) {
    char *o = malloc(1024);
    snprintf(o, 1024,
             "{\"domain\":\"relativity\",\"title\":\"%s\",\"solver\":\"src/lab/relativity: null geodesics of the Schwarzschild metric (u'' + u = "
             "3u^2, RK4), thin Keplerian disk, Shakura-Sunyaev temperatures, gravitational and Doppler shift, g^4 beaming\",\"image\":true,\"time_unit\":\"M\","
             "\"camera_r_M\":%.4g,\"inclination_deg\":%.4g,\"fields\":{\"r\":\"1\",\"g\":\"1\",\"b\":\"1\"}}",
             title, s->cam_r, s->incl_deg);
    return o;
}
