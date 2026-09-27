/* labshape.c - bodies as signed distances, and their surfaces (labshape.h). */
#include "labshape.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double clampd(double x, double a, double b) { return x < a ? a : x > b ? b : x; }

/* the unit-chord NACA four-digit section as a closed polygon: upper surface from the trailing edge to the leading edge,
 * then the lower back to the trailing edge, cosine spaced */
static void naca_profile(LabShape *w) {
    const int N = SHAPE_PROFILE;
    for (int k = 0; k < N; k++) {
        double b = M_PI * k / (N - 1), x = 0.5 * (1 - cos(b));
        double yt = 5 * w->t * (0.2969 * sqrt(x) - 0.1260 * x - 0.3516 * x * x + 0.2843 * x * x * x - 0.1036 * x * x * x * x);
        double yc = 0, dy = 0;
        if (w->m > 0 && w->p > 0) {
            if (x < w->p) yc = w->m / (w->p * w->p) * (2 * w->p * x - x * x), dy = 2 * w->m / (w->p * w->p) * (w->p - x);
            else yc = w->m / ((1 - w->p) * (1 - w->p)) * ((1 - 2 * w->p) + 2 * w->p * x - x * x), dy = 2 * w->m / ((1 - w->p) * (1 - w->p)) * (w->p - x);
        }
        double th = atan(dy);
        /* upper, from the trailing edge (k = 0 at x = 1 means reversing): index N - 1 - k */
        w->prof[N - 1 - k][0] = x - yt * sin(th), w->prof[N - 1 - k][1] = yc + yt * cos(th);
        w->prof[N + k][0] = x + yt * sin(th), w->prof[N + k][1] = yc - yt * cos(th);
    }
}

static double poly_sdf(const double (*P)[2], int n, double u, double v) {
    double best = 1e300;
    bool inside = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        double ax = P[j][0], ay = P[j][1], bx = P[i][0], by = P[i][1], ex = bx - ax, ey = by - ay;
        double l2 = ex * ex + ey * ey, t = l2 > 0 ? clampd(((u - ax) * ex + (v - ay) * ey) / l2, 0, 1) : 0;
        double dx = u - (ax + t * ex), dy = v - (ay + t * ey), d = dx * dx + dy * dy;
        if (d < best) best = d;
        if ((ay > v) != (by > v) && u < ax + (v - ay) * ex / ey) inside = !inside;
    }
    return inside ? -sqrt(best) : sqrt(best);
}

/* distance to a profile closed along its axis, the axis edges (both ends at r = 0) not counted as surface */
static double revolved_sdf(const double (*P)[2], int n, double u, double v) {
    double best = 1e300;
    bool inside = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        double ax = P[j][0], ay = P[j][1], bx = P[i][0], by = P[i][1], ex = bx - ax, ey = by - ay;
        if ((ay > v) != (by > v) && u < ax + (v - ay) * ex / ey) inside = !inside;
        if (fabs(ay) < 1e-12 && fabs(by) < 1e-12) continue;
        double l2 = ex * ex + ey * ey, t = l2 > 0 ? clampd(((u - ax) * ex + (v - ay) * ey) / l2, 0, 1) : 0;
        double dx = u - (ax + t * ex), dy = v - (ay + t * ey), d = dx * dx + dy * dy;
        if (d < best) best = d;
    }
    return inside ? -sqrt(best) : sqrt(best);
}

static double extrude(double d2, double dh) { /* a 2D distance extruded over a half-height: exact for a prism */
    return fmin(fmax(d2, dh), 0.0) + hypot(fmax(d2, 0.0), fmax(dh, 0.0));
}

static double shape_sdf(const LabShape *s, const double p[3]) {
    /* far from a shape, the distance to its box is enough (a lower bound with the right sign); near it (within a
     * quarter of the box's largest side) the exact distance, which contact needs (a sheet resting on a sphere rested on
     * its box before, 2026-09-26) */
    double out = 0, ext = 0;
    for (int k = 0; k < 3; k++) {
        double e = fmax(s->aabb_lo[k] - p[k], p[k] - s->aabb_hi[k]);
        if (e > 0) out += e * e;
        ext = fmax(ext, s->aabb_hi[k] - s->aabb_lo[k]);
    }
    if (out > 0.0625 * ext * ext) return sqrt(out);
    switch (s->kind) {
    case SHAPE_SPHERE: return sqrt((p[0] - s->c[0]) * (p[0] - s->c[0]) + (p[1] - s->c[1]) * (p[1] - s->c[1]) + (p[2] - s->c[2]) * (p[2] - s->c[2])) - s->r;
    case SHAPE_ROOT: { /* the fluid is the bore and the three sinuses: the solid's distance is the fluid's, negated */
        double f = s->r - hypot(p[1] - s->c[1], p[2] - s->c[2]);
        for (int k = 0; k < 3; k++)
            f = fmax(f, s->sr - sqrt((p[0] - s->sc[k][0]) * (p[0] - s->sc[k][0]) + (p[1] - s->sc[k][1]) * (p[1] - s->sc[k][1]) +
                                     (p[2] - s->sc[k][2]) * (p[2] - s->sc[k][2])));
        return f;
    }
    case SHAPE_PIPE: {
        int a = s->axis, b = (a + 1) % 3, c = (a + 2) % 3;
        return s->r - hypot(p[b] - s->c[b], p[c] - s->c[c]);
    }
    case SHAPE_CYLINDER: {
        int a = s->axis, b = (a + 1) % 3, c = (a + 2) % 3;
        return extrude(hypot(p[b] - s->c[b], p[c] - s->c[c]) - s->r, fabs(p[a] - s->c[a]) - 0.5 * s->len);
    }
    case SHAPE_BOX: {
        double q[3], m = -1e300, o = 0;
        for (int k = 0; k < 3; k++) {
            q[k] = fabs(p[k] - 0.5 * (s->lo[k] + s->hi[k])) - 0.5 * (s->hi[k] - s->lo[k]);
            m = fmax(m, q[k]);
            if (q[k] > 0) o += q[k] * q[k];
        }
        return sqrt(o) + fmin(m, 0.0);
    }
    case SHAPE_REVOLVED: {
        double r[3] = {p[0] - s->c[0], p[1] - s->c[1], p[2] - s->c[2]}, a = r[0] * s->dir[0] + r[1] * s->dir[1] + r[2] * s->dir[2];
        double q[3] = {r[0] - a * s->dir[0], r[1] - a * s->dir[1], r[2] - a * s->dir[2]};
        return revolved_sdf((const double(*)[2])s->prof, s->nprof, a, sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]));
    }
    case SHAPE_WING: {
        double pp[3] = {p[0], p[1], p[2]};
        if (s->span_axis == 1) pp[1] = 2 * s->le[1] - p[1]; /* a left wing: mirrored */
        else if (s->span_axis == 2) pp[1] = s->le[1] + (p[2] - s->le[2]), pp[2] = s->le[2] - (p[1] - s->le[1]); /* a fin: z as span */
        p = pp;
        double pv[3] = {s->le[0] + 0.25 * s->chord, s->le[1], s->le[2]}, rx = p[0] - pv[0], rz = p[2] - pv[2];
        double ca = cos(s->angle), sa = sin(s->angle), xb = rx * ca - rz * sa, zb = rx * sa + rz * ca;
        double sy = p[1] - s->le[1], st = clampd(sy, 0, s->span), cl = s->chord + (s->tip_chord - s->chord) * st / s->span;
        double xle = -0.25 * s->chord + st * tan(s->sweep);
        double d2 = poly_sdf((const double(*)[2])s->prof, 2 * SHAPE_PROFILE, (xb - xle) / cl, zb / cl) * cl;
        return extrude(d2, fabs(sy - 0.5 * s->span) - 0.5 * s->span);
    }
    }
    return 1e300;
}

double labshape_sdf(const LabShapes *S, const double p[3]) {
    double d = 1e300;
    for (int i = 0; i < S->n; i++) d = fmin(d, shape_sdf(&S->s[i], p));
    return d;
}

static bool numbers(const JsonValue *o, const char *key, double *v, int n) { return json_get_numbers(json_get(o, key), v, (size_t)n); }

bool labshape_parse(const JsonValue *bodies, LabShapes *S, char *err, size_t errlen) {
    memset(S, 0, sizeof *S);
    for (size_t i = 0; bodies && i < json_len(bodies); i++) {
        if (S->n >= SHAPE_MAX) {
            snprintf(err, errlen, "bodies: at most %d", SHAPE_MAX);
            return false;
        }
        const JsonValue *b = json_at(bodies, i);
        LabShape *s = &S->s[S->n];
        memset(s, 0, sizeof *s);
        const char *k = json_get_str(b, "shape", "");
        double pad = 0;
        if (!strcmp(k, "sphere")) {
            s->kind = SHAPE_SPHERE, s->r = json_get_num(b, "radius_m", -1);
            if (!numbers(b, "centre_m", s->c, 3) || !(s->r > 0)) goto bad;
            for (int a = 0; a < 3; a++) s->aabb_lo[a] = s->c[a] - s->r, s->aabb_hi[a] = s->c[a] + s->r;
        } else if (!strcmp(k, "cylinder")) {
            s->kind = SHAPE_CYLINDER, s->r = json_get_num(b, "radius_m", -1), s->len = json_get_num(b, "length_m", -1);
            const char *ax = json_get_str(b, "axis", "z");
            s->axis = ax[0] == 'x' ? 0 : ax[0] == 'y' ? 1 : 2;
            if (!numbers(b, "centre_m", s->c, 3) || !(s->r > 0) || !(s->len > 0)) goto bad;
            for (int a = 0; a < 3; a++) {
                double h = a == s->axis ? 0.5 * s->len : s->r;
                s->aabb_lo[a] = s->c[a] - h, s->aabb_hi[a] = s->c[a] + h;
            }
        } else if (!strcmp(k, "aortic_root")) { /* a vessel along x with three sinuses (of Valsalva) behind a valve's leaflets */
            s->kind = SHAPE_ROOT, s->r = json_get_num(b, "radius_m", -1), s->sr = json_get_num(b, "sinus_radius_m", -1);
            double off = json_get_num(b, "sinus_offset_m", -1), sx = json_get_num(b, "sinus_x_m", 0), ph = json_get_num(b, "sinus_phase_deg", 0);
            if (!numbers(b, "centre_m", s->c, 3) || !(s->r > 0) || !(s->sr > 0) || !(off >= 0)) goto bad;
            for (int q = 0; q < 3; q++) {
                double a = (ph + 120.0 * q) * M_PI / 180;
                s->sc[q][0] = s->c[0] + sx, s->sc[q][1] = s->c[1] + off * cos(a), s->sc[q][2] = s->c[2] + off * sin(a);
            }
            for (int a = 0; a < 3; a++) s->aabb_lo[a] = -1e30, s->aabb_hi[a] = 1e30;
        } else if (!strcmp(k, "pipe")) { /* a straight vessel: the fluid in its bore, everything outside solid */
            s->kind = SHAPE_PIPE, s->r = json_get_num(b, "radius_m", -1);
            const char *ax = json_get_str(b, "axis", "x");
            s->axis = ax[0] == 'x' ? 0 : ax[0] == 'y' ? 1 : 2;
            if (!numbers(b, "centre_m", s->c, 3) || !(s->r > 0)) goto bad;
            for (int a = 0; a < 3; a++) s->aabb_lo[a] = -1e30, s->aabb_hi[a] = 1e30;
        } else if (!strcmp(k, "box")) {
            double bx[6];
            s->kind = SHAPE_BOX;
            if (!numbers(b, "box_m", bx, 6)) goto bad;
            for (int a = 0; a < 3; a++) s->lo[a] = s->aabb_lo[a] = bx[a], s->hi[a] = s->aabb_hi[a] = bx[a + 3];
        } else if (!strcmp(k, "revolved")) {
            const JsonValue *pr = json_get(b, "profile_m");
            s->kind = SHAPE_REVOLVED, s->nprof = (int)json_len(pr);
            if (!numbers(b, "origin_m", s->c, 3) || s->nprof < 3 || s->nprof > 2 * SHAPE_PROFILE) goto bad;
            double amax = 0, rmax = 0, amin = 0;
            for (int v = 0; v < s->nprof; v++) {
                if (!json_get_numbers(json_at(pr, (size_t)v), s->prof[v], 2) || s->prof[v][1] < 0) goto bad;
                amax = fmax(amax, s->prof[v][0]), amin = fmin(amin, s->prof[v][0]), rmax = fmax(rmax, s->prof[v][1]);
            }
            double pa = json_get_num(b, "pitch_deg", 0) * M_PI / 180;
            s->dir[0] = cos(pa), s->dir[1] = 0, s->dir[2] = sin(pa);
            double ext = fmax(fabs(amax), fabs(amin)) + rmax;
            for (int a = 0; a < 3; a++) s->aabb_lo[a] = s->c[a] - ext, s->aabb_hi[a] = s->c[a] + ext;
        } else if (!strcmp(k, "wing")) {
            s->kind = SHAPE_WING, s->chord = json_get_num(b, "chord_m", -1), s->span = json_get_num(b, "span_m", -1);
            s->tip_chord = json_get_num(b, "tip_chord_m", s->chord), s->angle = json_get_num(b, "angle_deg", 0) * M_PI / 180;
            s->sweep = json_get_num(b, "sweep_deg", 0) * M_PI / 180;
            const char *sa = json_get_str(b, "span_axis", "y");
            s->span_axis = !strcmp(sa, "-y") ? 1 : !strcmp(sa, "z") ? 2 : 0;
            const char *nc = json_get_str(b, "naca", "0012");
            if (strlen(nc) != 4 || !numbers(b, "root_leading_edge_m", s->le, 3) || !(s->chord > 0) || !(s->span > 0) || !(s->tip_chord > 0)) goto bad;
            s->m = (nc[0] - '0') / 100.0, s->p = (nc[1] - '0') / 10.0, s->t = ((nc[2] - '0') * 10 + (nc[3] - '0')) / 100.0;
            if (!(s->t > 0)) goto bad;
            naca_profile(s);
            double ext = fmax(s->chord, s->tip_chord) * 1.1 + s->span * fabs(tan(s->sweep));
            s->aabb_lo[0] = s->le[0] - ext, s->aabb_hi[0] = s->le[0] + 2 * ext;
            s->aabb_lo[1] = s->le[1], s->aabb_hi[1] = s->le[1] + s->span;
            s->aabb_lo[2] = s->le[2] - ext, s->aabb_hi[2] = s->le[2] + ext;
            if (s->span_axis == 1) s->aabb_lo[1] = s->le[1] - s->span, s->aabb_hi[1] = s->le[1];
            if (s->span_axis == 2) s->aabb_lo[1] = s->le[1] - ext, s->aabb_hi[1] = s->le[1] + ext, s->aabb_lo[2] = s->le[2], s->aabb_hi[2] = s->le[2] + s->span;
        } else {
            snprintf(err, errlen, "bodies[%zu]: shape is sphere, cylinder, box, revolved or wing", i);
            return false;
        }
        for (int a = 0; a < 3; a++) s->aabb_lo[a] -= pad, s->aabb_hi[a] += pad;
        S->n++;
        continue;
    bad:
        snprintf(err, errlen, "bodies[%zu] (%s): missing or invalid sizes (docs in src/lab/labshape.h)", i, k);
        return false;
    }
    return true;
}

/* the six tetrahedra of a cube around its main diagonal (corners numbered by bits x, y, z) */
static const int TET[6][4] = {{0, 1, 3, 7}, {0, 3, 2, 7}, {0, 2, 6, 7}, {0, 6, 4, 7}, {0, 4, 5, 7}, {0, 5, 1, 7}};

double *labshape_mesh(const LabShapes *S, const double lo[3], const double hi[3], double h, int *ntri) {
    *ntri = 0;
    if (!S->n || !(h > 0)) return NULL;
    /* only the region around the bodies */
    double a[3], b[3];
    for (int k = 0; k < 3; k++) {
        a[k] = 1e300, b[k] = -1e300;
        for (int i = 0; i < S->n; i++) a[k] = fmin(a[k], S->s[i].aabb_lo[k]), b[k] = fmax(b[k], S->s[i].aabb_hi[k]);
        a[k] = fmax(a[k] - 2 * h, lo[k]), b[k] = fmin(b[k] + 2 * h, hi[k]);
        if (!(b[k] > a[k])) return NULL;
    }
    int n[3];
    for (int k = 0; k < 3; k++) n[k] = (int)ceil((b[k] - a[k]) / h) + 1;
    size_t nn = (size_t)n[0] * n[1] * n[2];
    double *v = malloc(nn * sizeof *v);
    if (!v) return NULL;
    for (int z = 0; z < n[2]; z++)
        for (int y = 0; y < n[1]; y++)
            for (int x = 0; x < n[0]; x++) {
                double p[3] = {a[0] + x * h, a[1] + y * h, a[2] + z * h};
                v[(size_t)x + (size_t)n[0] * ((size_t)y + (size_t)n[1] * z)] = labshape_sdf(S, p);
            }
    size_t cap = 4096, cnt = 0;
    double *tri = malloc(cap * 9 * sizeof *tri);
    for (int z = 0; tri && z + 1 < n[2]; z++)
        for (int y = 0; y + 1 < n[1]; y++)
            for (int x = 0; x + 1 < n[0]; x++) {
                double cv[8], cp[8][3];
                bool any_in = false, any_out = false;
                for (int c = 0; c < 8; c++) {
                    int dx = c & 1, dy = (c >> 1) & 1, dz = (c >> 2) & 1;
                    cv[c] = v[(size_t)(x + dx) + (size_t)n[0] * ((size_t)(y + dy) + (size_t)n[1] * (z + dz))];
                    cp[c][0] = a[0] + (x + dx) * h, cp[c][1] = a[1] + (y + dy) * h, cp[c][2] = a[2] + (z + dz) * h;
                    any_in |= cv[c] < 0, any_out |= cv[c] >= 0;
                }
                if (!any_in || !any_out) continue;
                for (int t = 0; t < 6; t++) {
                    int in[4], out[4], ni = 0, no = 0;
                    for (int q = 0; q < 4; q++) {
                        int c = TET[t][q];
                        if (cv[c] < 0) in[ni++] = c;
                        else out[no++] = c;
                    }
                    if (ni == 0 || ni == 4) continue;
                    double pts[4][3];
                    int np = 0;
                    for (int i = 0; i < ni; i++)
                        for (int o = 0; o < no; o++) {
                            double f = cv[in[i]] / (cv[in[i]] - cv[out[o]]);
                            for (int k = 0; k < 3; k++) pts[np][k] = cp[in[i]][k] + f * (cp[out[o]][k] - cp[in[i]][k]);
                            np++;
                        }
                    int ntr = np == 3 ? 1 : 2;
                    if (cnt + (size_t)ntr > cap) {
                        cap *= 2;
                        double *t2 = realloc(tri, cap * 9 * sizeof *tri);
                        if (!t2) { free(tri), tri = NULL; break; }
                        tri = t2;
                    }
                    /* 3 points: one triangle; 4 (two in, two out): the quad 0-1-3-2 as two triangles */
                    static const int ORD[2][3] = {{0, 1, 3}, {0, 3, 2}};
                    for (int r = 0; r < ntr; r++) {
                        double *T = &tri[9 * (cnt + (size_t)r)];
                        for (int q = 0; q < 3; q++) memcpy(&T[3 * q], pts[np == 3 ? q : ORD[r][q]], 3 * sizeof(double));
                        /* outward: the triangle's normal along the distance's gradient (marching tetrahedra leaves the
                         * orientation to chance, and smooth normals averaged over mixed orientations cancel into dots) */
                        double e1[3], e2[3], nn[3], cen[3], gr[3], hh = 0.25 * h;
                        for (int k = 0; k < 3; k++) e1[k] = T[3 + k] - T[k], e2[k] = T[6 + k] - T[k], cen[k] = (T[k] + T[3 + k] + T[6 + k]) / 3;
                        nn[0] = e1[1] * e2[2] - e1[2] * e2[1], nn[1] = e1[2] * e2[0] - e1[0] * e2[2], nn[2] = e1[0] * e2[1] - e1[1] * e2[0];
                        for (int k = 0; k < 3; k++) {
                            double pa[3] = {cen[0], cen[1], cen[2]}, pb[3] = {cen[0], cen[1], cen[2]};
                            pa[k] += hh, pb[k] -= hh;
                            gr[k] = labshape_sdf(S, pa) - labshape_sdf(S, pb);
                        }
                        if (nn[0] * gr[0] + nn[1] * gr[1] + nn[2] * gr[2] < 0)
                            for (int k = 0; k < 3; k++) {
                                double t = T[3 + k];
                                T[3 + k] = T[6 + k], T[6 + k] = t;
                            }
                    }
                    cnt += (size_t)ntr;
                }
            }
    free(v);
    if (!tri || !cnt) {
        free(tri);
        return NULL;
    }
    *ntri = (int)cnt;
    return tri;
}

double labshape_frontal_area(const LabShapes *S, const double lo[3], const double hi[3], double h) {
    double area = 0;
    for (double z = lo[2] + 0.5 * h; z < hi[2]; z += h)
        for (double y = lo[1] + 0.5 * h; y < hi[1]; y += h)
            for (double x = lo[0] + 0.5 * h; x < hi[0]; x += 0.5 * h) {
                double p[3] = {x, y, z};
                double d = labshape_sdf(S, p);
                if (d < 0) {
                    area += h * h;
                    break;
                }
                if (d > h) x += d - h; /* step by the distance: nothing nearer than it */
            }
    return area;
}

typedef struct {
    long long q[3];
    int i;
} Vkey;
static int cmp_vkey(const void *a, const void *b) {
    const Vkey *x = a, *y = b;
    for (int k = 0; k < 3; k++)
        if (x->q[k] != y->q[k]) return x->q[k] < y->q[k] ? -1 : 1;
    return 0;
}
int labshape_weld(const double *soup, int ntri, double **nodes, int **conn) {
    int nv = 3 * ntri, nn = 0;
    Vkey *K = malloc((size_t)nv * sizeof *K);
    *nodes = malloc(3 * (size_t)nv * sizeof(double)), *conn = malloc((size_t)nv * sizeof(int));
    if (!K || !*nodes || !*conn) {
        free(K);
        return -1;
    }
    for (int v = 0; v < nv; v++) {
        for (int k = 0; k < 3; k++) K[v].q[k] = llround(soup[3 * v + k] * 1e9);
        K[v].i = v;
    }
    qsort(K, (size_t)nv, sizeof *K, cmp_vkey);
    for (int v = 0; v < nv; v++) {
        if (!v || cmp_vkey(&K[v], &K[v - 1])) memcpy(&(*nodes)[3 * nn], &soup[3 * K[v].i], 3 * sizeof(double)), nn++;
        (*conn)[K[v].i] = nn - 1;
    }
    free(K);
    return nn;
}
