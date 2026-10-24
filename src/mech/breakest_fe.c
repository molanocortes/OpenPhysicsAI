/* breakest_fe.c - the break estimate on a solved part (breakest_fe.h). */
#include "breakest_fe.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../fem/hex8.h"
#include "../fem/tet.h"
#include "breakest.h"

/* the elements binned on a grid by their bounding boxes, for finding the element around a point */
typedef struct Bins {
    double lo[3], cell;
    int n[3];
    int *start, *list;
} Bins;

static void bins_free(Bins *b) { free(b->start), free(b->list); }

static bool bins_build(const StaticModel *m, Bins *b) {
    memset(b, 0, sizeof *b);
    int npe = sa_npe(m);
    double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (int i = 0; i < m->nnodes; i++)
        for (int k = 0; k < 3; k++) lo[k] = fmin(lo[k], m->xyz[3 * (size_t)i + k]), hi[k] = fmax(hi[k], m->xyz[3 * (size_t)i + k]);
    double vol = (hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]);
    b->cell = cbrt(fmax(vol, 1e-30) / fmax(1, m->nelems)) * 2;
    for (int k = 0; k < 3; k++) {
        b->lo[k] = lo[k] - 1e-9;
        b->n[k] = (int)fmin(512, fmax(1, ceil((hi[k] - lo[k]) / b->cell) + 1));
    }
    size_t ncell = (size_t)b->n[0] * b->n[1] * b->n[2];
    b->start = calloc(ncell + 1, sizeof(int));
    if (!b->start) return false;
    int pass;
    size_t total = 0;
    for (pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            for (size_t c = 0; c < ncell; c++) b->start[c + 1] += b->start[c];
            total = (size_t)b->start[ncell];
            b->list = malloc((total ? total : 1) * sizeof(int));
            if (!b->list) return false;
        }
        int *fill = pass == 1 ? calloc(ncell, sizeof(int)) : NULL;
        if (pass == 1 && !fill) return false;
        for (int e = 0; e < m->nelems; e++) {
            double elo[3] = {INFINITY, INFINITY, INFINITY}, ehi[3] = {-INFINITY, -INFINITY, -INFINITY};
            for (int a = 0; a < npe; a++) {
                const double *x = m->xyz + 3 * (size_t)m->conn[(size_t)npe * e + a];
                for (int k = 0; k < 3; k++) elo[k] = fmin(elo[k], x[k]), ehi[k] = fmax(ehi[k], x[k]);
            }
            int i0[3], i1[3];
            for (int k = 0; k < 3; k++) {
                i0[k] = (int)fmax(0, floor((elo[k] - b->lo[k]) / b->cell));
                i1[k] = (int)fmin(b->n[k] - 1, floor((ehi[k] - b->lo[k]) / b->cell));
            }
            for (int z = i0[2]; z <= i1[2]; z++)
                for (int y = i0[1]; y <= i1[1]; y++)
                    for (int x = i0[0]; x <= i1[0]; x++) {
                        size_t c = ((size_t)z * b->n[1] + y) * b->n[0] + x;
                        if (pass == 0) b->start[c + 1]++;
                        else b->list[b->start[c] + fill[c]++] = e;
                    }
        }
        free(fill);
    }
    return true;
}

/* natural coordinates of x in element e and its shape functions there; true when inside */
static bool locate(const StaticModel *m, int e, const double x[3], double *N, double (*dNdx)[3]) {
    int npe = sa_npe(m);
    double X[10][3];
    sa_elem_coords(m, e, X);
    if (m->elem_type == SOLID_ELEM_HEX8) {
        double xi[3] = {0, 0, 0}, dN[8][3], J[3][3], dx[8][3];
        for (int it = 0; it < 30; it++) {
            double f[3] = {0, 0, 0};
            hex8_shape(xi[0], xi[1], xi[2], N, dN);
            for (int a = 0; a < 8; a++)
                for (int k = 0; k < 3; k++) f[k] += N[a] * X[a][k];
            for (int k = 0; k < 3; k++) f[k] -= x[k];
            if (!(hex8_jacobian((const double (*)[3])X, (const double (*)[3])dN, J, dx) > 0)) return false;
            /* dxi = -J^-1 f, J[i][j] = dx_i / dxi_j */
            double det = J[0][0] * (J[1][1] * J[2][2] - J[1][2] * J[2][1]) - J[0][1] * (J[1][0] * J[2][2] - J[1][2] * J[2][0]) +
                         J[0][2] * (J[1][0] * J[2][1] - J[1][1] * J[2][0]);
            if (!(fabs(det) > 0)) return false;
            double inv[3][3] = {{(J[1][1] * J[2][2] - J[1][2] * J[2][1]) / det, (J[0][2] * J[2][1] - J[0][1] * J[2][2]) / det,
                                 (J[0][1] * J[1][2] - J[0][2] * J[1][1]) / det},
                                {(J[1][2] * J[2][0] - J[1][0] * J[2][2]) / det, (J[0][0] * J[2][2] - J[0][2] * J[2][0]) / det,
                                 (J[0][2] * J[1][0] - J[0][0] * J[1][2]) / det},
                                {(J[1][0] * J[2][1] - J[1][1] * J[2][0]) / det, (J[0][1] * J[2][0] - J[0][0] * J[2][1]) / det,
                                 (J[0][0] * J[1][1] - J[0][1] * J[1][0]) / det}};
            double step = 0;
            for (int i = 0; i < 3; i++) {
                double d = -(inv[i][0] * f[0] + inv[i][1] * f[1] + inv[i][2] * f[2]);
                xi[i] += d, step = fmax(step, fabs(d));
            }
            if (step < 1e-13) break;
        }
        hex8_shape(xi[0], xi[1], xi[2], N, dN);
        if (dNdx) hex8_jacobian((const double (*)[3])X, (const double (*)[3])dN, J, dx), memcpy(dNdx, dx, sizeof(double) * 24);
        return fabs(xi[0]) <= 1 + 1e-9 && fabs(xi[1]) <= 1 + 1e-9 && fabs(xi[2]) <= 1 + 1e-9;
    }
    double xi[3] = {0.25, 0.25, 0.25}, L[4], dN[10][3], J[3][3], dx[10][3];
    for (int it = 0; it < 30; it++) {
        double f[3] = {0, 0, 0};
        L[0] = 1 - xi[0] - xi[1] - xi[2], L[1] = xi[0], L[2] = xi[1], L[3] = xi[2];
        tet_shape(m->elem_type, L, N, dN);
        for (int a = 0; a < npe; a++)
            for (int k = 0; k < 3; k++) f[k] += N[a] * X[a][k];
        for (int k = 0; k < 3; k++) f[k] -= x[k];
        tet_jacobian(m->elem_type, (const double (*)[3])X, (const double (*)[3])dN, J, NULL);
        double det = J[0][0] * (J[1][1] * J[2][2] - J[1][2] * J[2][1]) - J[0][1] * (J[1][0] * J[2][2] - J[1][2] * J[2][0]) +
                     J[0][2] * (J[1][0] * J[2][1] - J[1][1] * J[2][0]);
        if (!(fabs(det) > 0)) return false;
        double inv[3][3] = {{(J[1][1] * J[2][2] - J[1][2] * J[2][1]) / det, (J[0][2] * J[2][1] - J[0][1] * J[2][2]) / det,
                             (J[0][1] * J[1][2] - J[0][2] * J[1][1]) / det},
                            {(J[1][2] * J[2][0] - J[1][0] * J[2][2]) / det, (J[0][0] * J[2][2] - J[0][2] * J[2][0]) / det,
                             (J[0][2] * J[1][0] - J[0][0] * J[1][2]) / det},
                            {(J[1][0] * J[2][1] - J[1][1] * J[2][0]) / det, (J[0][1] * J[2][0] - J[0][0] * J[2][1]) / det,
                             (J[0][0] * J[1][1] - J[0][1] * J[1][0]) / det}};
        double step = 0;
        for (int i = 0; i < 3; i++) {
            double d = -(inv[i][0] * f[0] + inv[i][1] * f[1] + inv[i][2] * f[2]);
            xi[i] += d, step = fmax(step, fabs(d));
        }
        if (step < 1e-13) break;
    }
    L[0] = 1 - xi[0] - xi[1] - xi[2], L[1] = xi[0], L[2] = xi[1], L[3] = xi[2];
    tet_shape(m->elem_type, L, N, dN);
    if (dNdx) tet_jacobian(m->elem_type, (const double (*)[3])X, (const double (*)[3])dN, J, dx), memcpy(dNdx, dx, sizeof(double) * 3 * (size_t)npe);
    return L[0] >= -1e-9 && L[1] >= -1e-9 && L[2] >= -1e-9 && L[3] >= -1e-9;
}

/* the governing stress at every node: the normal stress along material axis comp */
static double gov(const double *s, const double a[3]) {
    /* Voigt xx yy zz xy yz zx */
    return s[0] * a[0] * a[0] + s[1] * a[1] * a[1] + s[2] * a[2] * a[2] + 2 * (s[3] * a[0] * a[1] + s[4] * a[1] * a[2] + s[5] * a[2] * a[0]);
}

/* the governing stress at x interpolated in the element around it; false when x is outside the mesh */
static bool sample(const StaticModel *m, const Bins *b, const double *g, const double x[3], double *v) {
    int ix[3];
    for (int k = 0; k < 3; k++) {
        ix[k] = (int)floor((x[k] - b->lo[k]) / b->cell);
        if (ix[k] < 0 || ix[k] >= b->n[k]) return false;
    }
    size_t c = ((size_t)ix[2] * b->n[1] + ix[1]) * b->n[0] + ix[0];
    int npe = sa_npe(m);
    double N[10];
    for (int q = b->start[c]; q < b->start[c + 1]; q++) {
        int e = b->list[q];
        if (!locate(m, e, x, N, NULL)) continue;
        double s = 0;
        for (int a = 0; a < npe; a++) s += N[a] * g[m->conn[(size_t)npe * e + a]];
        *v = s;
        return true;
    }
    return false;
}

bool breakest_fe_section(const StaticModel *mdl, const double *node_stress, const double R[9], int comp, const double x0[3], double s_t, double c,
                         double mm, BreakSection *out) {
    memset(out, 0, sizeof *out);
    if (comp < 0 || comp > 2 || !(s_t > 0)) return false;
    const double a[3] = {R[0 * 3 + comp], R[1 * 3 + comp], R[2 * 3 + comp]}; /* material axis comp: column comp of R */
    double *g = malloc((size_t)(mdl->nnodes ? mdl->nnodes : 1) * sizeof(double));
    Bins b;
    if (!g || !bins_build(mdl, &b)) {
        free(g);
        return false;
    }
    for (int i = 0; i < mdl->nnodes; i++) g[i] = gov(node_stress + 6 * (size_t)i, a);
    /* the stress and its gradient at the point, averaged over the elements around it */
    int npe = sa_npe(mdl), found = 0;
    double grad[3] = {0, 0, 0}, val = 0, N[10], dNdx[10][3];
    int ix[3];
    bool inside_grid = true;
    for (int k = 0; k < 3; k++) {
        ix[k] = (int)floor((x0[k] - b.lo[k]) / b.cell);
        if (ix[k] < 0 || ix[k] >= b.n[k]) inside_grid = false;
    }
    if (inside_grid) {
        size_t cc = ((size_t)ix[2] * b.n[1] + ix[1]) * b.n[0] + ix[0];
        for (int q = b.start[cc]; q < b.start[cc + 1]; q++) {
            int e = b.list[q];
            if (!locate(mdl, e, x0, N, dNdx)) continue;
            double v = 0;
            for (int i = 0; i < npe; i++) {
                double gi = g[mdl->conn[(size_t)npe * e + i]];
                v += N[i] * gi;
                for (int k = 0; k < 3; k++) grad[k] += gi * dNdx[i][k];
            }
            val += v, found++;
        }
    }
    if (!found) {
        free(g), bins_free(&b);
        return false;
    }
    val /= found;
    for (int k = 0; k < 3; k++) grad[k] /= found;
    out->at_point = val;
    if (!(val > 0)) { /* the break estimate reads a tensile governing stress */
        free(g), bins_free(&b);
        return false;
    }
    double gn = sqrt(grad[0] * grad[0] + grad[1] * grad[1] + grad[2] * grad[2]);
    double hmin = fmin(mdl->h[0], fmin(mdl->h[1], mdl->h[2]));
    if (!(hmin > 0)) hmin = 1e-3;
    if (gn * hmin < 1e-6 * fabs(val)) { /* uniform: all membrane */
        out->ok = true, out->membrane = val, out->bending = 0, out->wall = 0;
        out->load_factor = breakest_load_factor(val / s_t, 0, c, mm);
        free(g), bins_free(&b);
        return true;
    }
    for (int k = 0; k < 3; k++) out->dir[k] = -grad[k] / gn;
    /* along the line until it leaves the part, the exit found by bisection */
    const double ds = 0.25 * hmin;
    double *sv = malloc(4002 * sizeof(double)), *ss = malloc(4002 * sizeof(double));
    int n = 0;
    double s = 0, last_in = 0;
    sv[n] = val, ss[n] = 0, n++;
    while (n < 4000) {
        double x[3] = {x0[0] + (s + ds) * out->dir[0], x0[1] + (s + ds) * out->dir[1], x0[2] + (s + ds) * out->dir[2]}, v;
        if (!sample(mdl, &b, g, x, &v)) break;
        s += ds, last_in = s;
        sv[n] = v, ss[n] = s, n++;
    }
    double lo = last_in, hi = last_in + ds, vlast = sv[n - 1];
    for (int it = 0; it < 30; it++) {
        double mid = 0.5 * (lo + hi), x[3] = {x0[0] + mid * out->dir[0], x0[1] + mid * out->dir[1], x0[2] + mid * out->dir[2]}, v;
        if (sample(mdl, &b, g, x, &v)) lo = mid, vlast = v;
        else hi = mid;
    }
    if (lo > ss[n - 1] + 1e-12) sv[n] = vlast, ss[n] = lo, n++;
    double t = ss[n - 1];
    if (!(t > 0)) {
        free(sv), free(ss), free(g), bins_free(&b);
        return false;
    }
    double I0 = 0, I1 = 0; /* the integral of the stress and of the stress times (t/2 - s), trapezoids */
    for (int i = 0; i + 1 < n; i++) {
        double d = ss[i + 1] - ss[i];
        I0 += 0.5 * d * (sv[i] + sv[i + 1]);
        I1 += 0.5 * d * (sv[i] * (0.5 * t - ss[i]) + sv[i + 1] * (0.5 * t - ss[i + 1]));
    }
    out->ok = true, out->wall = t, out->samples = n;
    out->membrane = I0 / t, out->bending = 6 * I1 / (t * t);
    out->load_factor = breakest_load_factor(out->membrane / s_t, out->bending / s_t, c, mm);
    free(sv), free(ss), free(g), bins_free(&b);
    return true;
}
