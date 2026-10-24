/* patches.c - planar and smooth region growing, adjacency, classification */
#include "patches.h"
#include "../fem/dense.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

const char *patch_type_name(PatchType t) {
    switch (t) {
    case PATCH_PLANAR: return "planar";
    case PATCH_CYLINDRICAL: return "cylindrical";
    default: return "other";
    }
}

void patches_free(PatchSet *ps) {
    free(ps->tri_patch);
    free(ps->start);
    free(ps->order);
    free(ps->type);
    free(ps->adj_start);
    free(ps->adj);
    memset(ps, 0, sizeof *ps);
}

static const double *tn(const Surface *s, int t) { return s->normal + 3 * (size_t)t; }
static double dot3(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

typedef struct {
    int t;
    double area;
} AreaIdx;

static int cmp_ai(const void *a, const void *b) {
    const AreaIdx *x = a, *y = b;
    if (x->area != y->area) return x->area > y->area ? -1 : 1;
    return x->t - y->t;
}


/* Planar patches that are really facets of a tessellated curved surface (cylinder strips, fillet facets) are released to
 * the smooth pass: most of their boundary meets neighbours at small dihedral angles and they are not much larger than
 * those neighbours. A genuine flat face bordered by a fillet keeps its identity because it is far larger than the
 * fillet's facets. */
static int dissolve_facets(const Surface *s, int *tri_patch, int np, PatchType *type, double cos_feature) {
    if (np <= 0) return np;
    double *area = calloc((size_t)np, sizeof(double)), *ls = calloc((size_t)np, sizeof(double));
    double *lt = calloc((size_t)np, sizeof(double)), *ref = calloc((size_t)np, sizeof(double));
    int *newid = malloc((size_t)np * sizeof(int));
    if (!area || !ls || !lt || !ref || !newid) {
        free(area), free(ls), free(lt), free(ref), free(newid);
        return np;
    }
    for (int t = 0; t < s->nt; t++)
        if (tri_patch[t] >= 0) area[tri_patch[t]] += s->area[t];
    for (int t = 0; t < s->nt; t++) {
        int p = tri_patch[t];
        if (p < 0) continue;
        for (int e = 0; e < 3; e++) {
            int o = s->nbr[3 * t + e];
            int q = o >= 0 ? tri_patch[o] : -1;
            if (o >= 0 && q == p) continue;
            const double *a = s->v + 3 * (size_t)s->tri[3 * t + e], *b = s->v + 3 * (size_t)s->tri[3 * t + (e + 1) % 3];
            double len = sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) + (b[2] - a[2]) * (b[2] - a[2]));
            lt[p] += len;
            if (o >= 0 && dot3(tn(s, t), tn(s, o)) >= cos_feature) {
                ls[p] += len;
                double qa = q >= 0 ? area[q] : s->area[o];
                if (qa > ref[p]) ref[p] = qa;
            }
        }
    }
    int w = 0;
    for (int p = 0; p < np; p++) {
        bool facet = lt[p] > 0 && ls[p] > 0.5 * lt[p] && area[p] < 4.0 * ref[p];
        newid[p] = facet ? -1 : w++;
    }
    for (int p = 0; p < np; p++)
        if (newid[p] >= 0) type[newid[p]] = type[p];
    for (int t = 0; t < s->nt; t++)
        if (tri_patch[t] >= 0) tri_patch[t] = newid[tri_patch[t]];
    free(area), free(ls), free(lt), free(ref), free(newid);
    return w;
}

bool patches_build(const Surface *s, double planar_tol_deg, double feature_angle_deg, PatchSet *ps, char *err, size_t errlen) {
    memset(ps, 0, sizeof *ps);
    ps->planar_tol_deg = planar_tol_deg;
    ps->feature_angle_deg = feature_angle_deg;
    int nt = s->nt;
    ps->tri_patch = malloc((size_t)(nt ? nt : 1) * sizeof(int));
    int *queue = malloc((size_t)(nt ? nt : 1) * sizeof(int));
    AreaIdx *ai = malloc((size_t)(nt ? nt : 1) * sizeof(AreaIdx));
    if (!ps->tri_patch || !queue || !ai) {
        free(queue), free(ai);
        patches_free(ps);
        snprintf(err, errlen, "out of memory segmenting %d triangles", nt);
        return false;
    }
    for (int t = 0; t < nt; t++) ps->tri_patch[t] = -1, ai[t] = (AreaIdx){t, s->area[t]};
    qsort(ai, (size_t)nt, sizeof *ai, cmp_ai);
    double cos_planar = cos(planar_tol_deg * M_PI / 180.0), cos_feature = cos(feature_angle_deg * M_PI / 180.0);
    int np = 0;
    int cap_types = 64;
    ps->type = malloc((size_t)cap_types * sizeof(PatchType));
    if (!ps->type) goto oom;
    /* pass 1: planar patches; a patch of a single triangle is not accepted as planar here */
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) np = dissolve_facets(s, ps->tri_patch, np, ps->type, cos_feature);
        for (int k = 0; k < nt; k++) {
            int seed = ai[k].t;
            if (ps->tri_patch[seed] >= 0) continue;
            const double *sn = tn(s, seed);
            if (dot3(sn, sn) < 0.5) continue; /* degenerate normal: handled in pass 2 */
            int head = 0, tail = 0;
            queue[tail++] = seed;
            ps->tri_patch[seed] = np;
            while (head < tail) {
                int t = queue[head++];
                for (int e = 0; e < 3; e++) {
                    int o = s->nbr[3 * t + e];
                    if (o < 0 || ps->tri_patch[o] >= 0) continue;
                    bool join = pass == 0 ? dot3(tn(s, o), sn) >= cos_planar : dot3(tn(s, o), tn(s, t)) >= cos_feature;
                    if (!join) continue;
                    ps->tri_patch[o] = np;
                    queue[tail++] = o;
                }
            }
            if (pass == 0 && tail < 2) {
                ps->tri_patch[seed] = -1; /* revisit in the smooth pass */
                continue;
            }
            if (np == cap_types) {
                cap_types *= 2;
                PatchType *nty = realloc(ps->type, (size_t)cap_types * sizeof(PatchType));
                if (!nty) goto oom;
                ps->type = nty;
            }
            ps->type[np] = pass == 0 ? PATCH_PLANAR : PATCH_OTHER;
            np++;
        }
    }
    for (int t = 0; t < nt; t++) {
        if (ps->tri_patch[t] >= 0) continue;
        if (np == cap_types) {
            cap_types *= 2;
            PatchType *nty = realloc(ps->type, (size_t)cap_types * sizeof(PatchType));
            if (!nty) goto oom;
            ps->type = nty;
        }
        ps->type[np] = PATCH_OTHER;
        ps->tri_patch[t] = np++;
    }
    ps->n = np;
    ps->start = calloc((size_t)np + 1, sizeof(int));
    ps->order = malloc((size_t)(nt ? nt : 1) * sizeof(int));
    if (!ps->start || !ps->order) goto oom;
    for (int t = 0; t < nt; t++) ps->start[ps->tri_patch[t] + 1]++;
    for (int p = 0; p < np; p++) ps->start[p + 1] += ps->start[p];
    {
        int *fill = malloc((size_t)(np ? np : 1) * sizeof(int));
        if (!fill) goto oom;
        memcpy(fill, ps->start, (size_t)np * sizeof(int));
        for (int t = 0; t < nt; t++) ps->order[fill[ps->tri_patch[t]]++] = t;
        free(fill);
    }
    /* adjacency (unique neighbour patches across shared edges) */
    ps->adj_start = calloc((size_t)np + 1, sizeof(int));
    if (!ps->adj_start) goto oom;
    {
        int *mark = malloc((size_t)(np ? np : 1) * sizeof(int));
        if (!mark) goto oom;
        for (int p = 0; p < np; p++) mark[p] = -1;
        int total = 0;
        for (int p = 0; p < np; p++) {
            for (int k = ps->start[p]; k < ps->start[p + 1]; k++) {
                int t = ps->order[k];
                for (int e = 0; e < 3; e++) {
                    int o = s->nbr[3 * t + e];
                    if (o < 0) continue;
                    int q = ps->tri_patch[o];
                    if (q != p && mark[q] != p) mark[q] = p, total++;
                }
            }
            ps->adj_start[p + 1] = total;
        }
        ps->adj = malloc((size_t)(total ? total : 1) * sizeof(int));
        if (!ps->adj) {
            free(mark);
            goto oom;
        }
        for (int p = 0; p < np; p++) mark[p] = -1;
        int w = 0;
        for (int p = 0; p < np; p++)
            for (int k = ps->start[p]; k < ps->start[p + 1]; k++) {
                int t = ps->order[k];
                for (int e = 0; e < 3; e++) {
                    int o = s->nbr[3 * t + e];
                    if (o < 0) continue;
                    int q = ps->tri_patch[o];
                    if (q != p && mark[q] != p) mark[q] = p, ps->adj[w++] = q;
                }
            }
        free(mark);
    }
    /* classify curved patches: cylinders */
    for (int p = 0; p < np; p++) {
        if (ps->type[p] != PATCH_OTHER || ps->start[p + 1] - ps->start[p] < 4) continue;
        PatchGeom g;
        patch_geometry(s, s->v, NULL, ps, p, &g);
        if (g.type == PATCH_CYLINDRICAL) ps->type[p] = PATCH_CYLINDRICAL;
    }
    free(queue), free(ai);
    return true;
oom:
    free(queue), free(ai);
    patches_free(ps);
    snprintf(err, errlen, "out of memory segmenting %d triangles", nt);
    return false;
}

void patch_geometry(const Surface *s, const double *v, const double *R, const PatchSet *ps, int p, PatchGeom *g) {
    memset(g, 0, sizeof *g);
    g->ntri = ps->start[p + 1] - ps->start[p];
    g->type = ps->type[p] == PATCH_PLANAR ? PATCH_PLANAR : PATCH_OTHER;
    for (int k = 0; k < 3; k++) g->bmin[k] = INFINITY, g->bmax[k] = -INFINITY;
    double N[9] = {0}; /* sum of area-weighted normal outer products */
    for (int k = ps->start[p]; k < ps->start[p + 1]; k++) {
        int t = ps->order[k];
        double a = s->area[t];
        const double *n0 = tn(s, t);
        double n[3];
        if (R) for (int i = 0; i < 3; i++) n[i] = R[3 * i] * n0[0] + R[3 * i + 1] * n0[1] + R[3 * i + 2] * n0[2];
        else memcpy(n, n0, sizeof n);
        double c[3] = {0, 0, 0};
        for (int q = 0; q < 3; q++) {
            const double *x = v + 3 * (size_t)s->tri[3 * t + q];
            for (int i = 0; i < 3; i++) {
                c[i] += x[i] / 3.0;
                g->bmin[i] = fmin(g->bmin[i], x[i]);
                g->bmax[i] = fmax(g->bmax[i], x[i]);
            }
        }
        g->area += a;
        for (int i = 0; i < 3; i++) {
            g->centroid[i] += a * c[i];
            g->normal[i] += a * n[i];
            for (int j = 0; j < 3; j++) N[3 * i + j] += a * n[i] * n[j];
        }
    }
    if (g->area > 0)
        for (int i = 0; i < 3; i++) g->centroid[i] /= g->area;
    double ln = sqrt(dot3(g->normal, g->normal));
    if (ln > 0)
        for (int i = 0; i < 3; i++) g->normal[i] /= ln;
    double min_cos = 1;
    for (int k = ps->start[p]; k < ps->start[p + 1]; k++) {
        int t = ps->order[k];
        const double *n0 = tn(s, t);
        double n[3];
        if (R) for (int i = 0; i < 3; i++) n[i] = R[3 * i] * n0[0] + R[3 * i + 1] * n0[1] + R[3 * i + 2] * n0[2];
        else memcpy(n, n0, sizeof n);
        min_cos = fmin(min_cos, dot3(n, g->normal));
    }
    g->normal_spread_deg = acos(fmax(-1.0, fmin(1.0, min_cos))) * 180.0 / M_PI;
    if (g->type == PATCH_PLANAR) {
        g->plane_d = dot3(g->normal, g->centroid);
        double ss = 0;
        int cnt = 0;
        for (int k = ps->start[p]; k < ps->start[p + 1]; k++)
            for (int q = 0; q < 3; q++) {
                double d = dot3(g->normal, v + 3 * (size_t)s->tri[3 * ps->order[k] + q]) - g->plane_d;
                ss += d * d, cnt++;
            }
        g->fit_rms = cnt ? sqrt(ss / cnt) : 0;
        return;
    }
    /* cylinder test: normals perpendicular to the axis (smallest eigenvector of N) and spanning a circle */
    double A[9], w[3], V[9];
    memcpy(A, N, sizeof A);
    if (!dense_sym_eigen(A, 3, w, V) || !(g->area > 0)) return;
    double total = w[0] + w[1] + w[2];
    if (!(total > 0) || w[0] / total > 0.02 || w[1] / total < 0.05) return;
    double axis[3] = {V[0], V[3], V[6]};
    /* orthonormal basis of the plane perpendicular to the axis */
    double e1[3] = {V[2], V[5], V[8]}, e2[3] = {axis[1] * e1[2] - axis[2] * e1[1], axis[2] * e1[0] - axis[0] * e1[2], axis[0] * e1[1] - axis[1] * e1[0]};
    /* Kasa circle fit on the vertices projected to that plane */
    double M[9] = {0}, rhs[3] = {0};
    int cnt = 0;
    for (int k = ps->start[p]; k < ps->start[p + 1]; k++)
        for (int q = 0; q < 3; q++) {
            const double *x = v + 3 * (size_t)s->tri[3 * ps->order[k] + q];
            double u = dot3(x, e1), vv = dot3(x, e2), z = -(u * u + vv * vv);
            double row[3] = {u, vv, 1};
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 3; j++) M[3 * i + j] += row[i] * row[j];
                rhs[i] += row[i] * z;
            }
            cnt++;
        }
    if (!dense_solve(M, rhs, 3)) return;
    double cu = -rhs[0] / 2, cv = -rhs[1] / 2, r2 = cu * cu + cv * cv - rhs[2];
    if (!(r2 > 0)) return;
    double r = sqrt(r2), ss = 0, inward = 0;
    for (int k = ps->start[p]; k < ps->start[p + 1]; k++) {
        int t = ps->order[k];
        for (int q = 0; q < 3; q++) {
            const double *x = v + 3 * (size_t)s->tri[3 * t + q];
            double du = dot3(x, e1) - cu, dv = dot3(x, e2) - cv;
            double d = sqrt(du * du + dv * dv) - r;
            ss += d * d;
        }
        const double *n0 = tn(s, t);
        double n[3];
        if (R) for (int i = 0; i < 3; i++) n[i] = R[3 * i] * n0[0] + R[3 * i + 1] * n0[1] + R[3 * i + 2] * n0[2];
        else memcpy(n, n0, sizeof n);
        double c[3] = {0, 0, 0};
        for (int q = 0; q < 3; q++)
            for (int i = 0; i < 3; i++) c[i] += v[3 * (size_t)s->tri[3 * t + q] + i] / 3.0;
        double du = dot3(c, e1) - cu, dv = dot3(c, e2) - cv;
        inward += s->area[t] * (dot3(n, e1) * du + dot3(n, e2) * dv);
    }
    double rms = sqrt(ss / cnt);
    if (rms > 0.02 * r) return;
    g->type = PATCH_CYLINDRICAL;
    memcpy(g->axis, axis, sizeof axis);
    double along = dot3(g->centroid, axis);
    for (int i = 0; i < 3; i++) g->axis_point[i] = cu * e1[i] + cv * e2[i] + along * axis[i];
    g->radius = r;
    g->concave = inward < 0;
    g->fit_rms = rms;
}
