/* topopt.c - compliance topology optimisation on voxel hexahedral meshes (docs/contracts/topology-optimisation.md) */
#include "topopt.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common.h"
#include "hex8.h"

void topopt_settings_default(TopOptSettings *s) {
    memset(s, 0, sizeof *s);
    s->volume_fraction = 0.5;
    s->penalty = 3.0;
    s->filter_radius = 0.0; /* 1.5 element widths */
    s->move_limit = 0.2;
    s->x_min = 1e-3;
    s->e_min_ratio = 1e-9;
    s->max_iter = 100;
    s->change_tol = 0.01;
    s->passive_layers = 1;
}

static void fail(char *err, size_t cap, const char *fmt, ...) {
    if (!err || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

/* element centroid and volume from its 8 corner nodes */
static void elem_geometry(const HexModel *m, int e, double c[3], double *vol, double X[8][3]) {
    const int *cn = m->conn + 8 * (size_t)e;
    c[0] = c[1] = c[2] = 0;
    for (int i = 0; i < 8; i++) {
        for (int d = 0; d < 3; d++) {
            X[i][d] = m->xyz[3 * (size_t)cn[i] + d];
            c[d] += X[i][d];
        }
    }
    for (int d = 0; d < 3; d++) c[d] /= 8.0;
    *vol = hex8_volume(X);
}

/* the elements that touch a supported or loaded node, grown by `layers - 1` rings of node neighbours */
static void mark_passive(const HexModel *m, const SolidLoads *loads, int layers, unsigned char *passive, int *count) {
    int n = m->nelems, nn = m->nnodes;
    memset(passive, 0, (size_t)n);
    *count = 0;
    if (layers <= 0) return;
    unsigned char *node = calloc((size_t)nn, 1);
    if (!node) return;
    for (int i = 0; i < nn; i++)
        for (int d = 0; d < 3; d++) {
            if (loads->fixed && loads->fixed[3 * (size_t)i + d]) node[i] = 1;
            if (loads->nodal_force && loads->nodal_force[3 * (size_t)i + d] != 0.0) node[i] = 1;
        }
    for (int ring = 0; ring < layers; ring++) {
        for (int e = 0; e < n; e++) {
            if (passive[e]) continue;
            const int *cn = m->conn + 8 * (size_t)e;
            for (int i = 0; i < 8; i++)
                if (node[cn[i]]) { passive[e] = 1; break; }
        }
        if (ring + 1 < layers) { /* the nodes of the marked elements seed the next ring */
            for (int e = 0; e < n; e++)
                if (passive[e]) {
                    const int *cn = m->conn + 8 * (size_t)e;
                    for (int i = 0; i < 8; i++) node[cn[i]] = 1;
                }
        }
    }
    for (int e = 0; e < n; e++) *count += passive[e];
    free(node);
}

/* neighbours within the filter radius, as a compressed row structure over a bucket grid of the centroids */
typedef struct Filter {
    int *start;   /* nelems + 1 */
    int *idx;     /* entries */
    double *w;    /* entries: rmin - distance */
    double *wsum; /* nelems */
} Filter;

static void filter_free(Filter *f) {
    free(f->start);
    free(f->idx);
    free(f->w);
    free(f->wsum);
    memset(f, 0, sizeof *f);
}

static bool filter_build(Filter *f, int n, const double *centre, double r, char *err, size_t errlen) {
    memset(f, 0, sizeof *f);
    double lo[3] = {centre[0], centre[1], centre[2]}, hi[3] = {centre[0], centre[1], centre[2]};
    for (int e = 1; e < n; e++)
        for (int d = 0; d < 3; d++) {
            if (centre[3 * (size_t)e + d] < lo[d]) lo[d] = centre[3 * (size_t)e + d];
            if (centre[3 * (size_t)e + d] > hi[d]) hi[d] = centre[3 * (size_t)e + d];
        }
    int dim[3];
    long long cells = 1;
    for (int d = 0; d < 3; d++) {
        double span = hi[d] - lo[d];
        dim[d] = (int)(span / r) + 1;
        if (dim[d] < 1) dim[d] = 1;
        cells *= dim[d];
        if (cells > 40LL * n + 1000) { /* a radius far below the element size would make a useless grid */
            fail(err, errlen, "the filter radius %g m is too small for this mesh", r);
            return false;
        }
    }
    int *head = malloc(sizeof(int) * (size_t)cells), *next = malloc(sizeof(int) * (size_t)n);
    if (!head || !next) { free(head); free(next); fail(err, errlen, "out of memory for the filter"); return false; }
    for (long long i = 0; i < cells; i++) head[i] = -1;
    int *cell_of = malloc(sizeof(int) * (size_t)n);
    if (!cell_of) { free(head); free(next); fail(err, errlen, "out of memory for the filter"); return false; }
    for (int e = 0; e < n; e++) {
        int ix[3];
        for (int d = 0; d < 3; d++) {
            ix[d] = (int)((centre[3 * (size_t)e + d] - lo[d]) / r);
            if (ix[d] < 0) ix[d] = 0;
            if (ix[d] >= dim[d]) ix[d] = dim[d] - 1;
        }
        int c = (ix[2] * dim[1] + ix[1]) * dim[0] + ix[0];
        cell_of[e] = c;
        next[e] = head[c];
        head[c] = e;
    }
    f->start = calloc((size_t)n + 1, sizeof(int));
    f->wsum = calloc((size_t)n, sizeof(double));
    if (!f->start || !f->wsum) { free(head); free(next); free(cell_of); filter_free(f); fail(err, errlen, "out of memory for the filter"); return false; }
    /* two passes: count, then fill */
    long long total = 0;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            f->start[0] = 0;
            for (int e = 0; e < n; e++) f->start[e + 1] += f->start[e];
            f->idx = malloc(sizeof(int) * (size_t)total);
            f->w = malloc(sizeof(double) * (size_t)total);
            if (!f->idx || !f->w) { free(head); free(next); free(cell_of); filter_free(f); fail(err, errlen, "out of memory for the filter (%lld entries)", total); return false; }
        }
        int *fill = NULL;
        if (pass == 1) {
            fill = malloc(sizeof(int) * (size_t)n);
            if (!fill) { free(head); free(next); free(cell_of); filter_free(f); fail(err, errlen, "out of memory for the filter"); return false; }
            for (int e = 0; e < n; e++) fill[e] = f->start[e];
        }
        for (int e = 0; e < n; e++) {
            int base = cell_of[e], ix[3];
            ix[0] = base % dim[0];
            ix[1] = (base / dim[0]) % dim[1];
            ix[2] = base / (dim[0] * dim[1]);
            int count = 0;
            for (int dz = -1; dz <= 1; dz++)
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int jx = ix[0] + dx, jy = ix[1] + dy, jz = ix[2] + dz;
                        if (jx < 0 || jy < 0 || jz < 0 || jx >= dim[0] || jy >= dim[1] || jz >= dim[2]) continue;
                        for (int g = head[(jz * dim[1] + jy) * dim[0] + jx]; g >= 0; g = next[g]) {
                            double dd = 0;
                            for (int d = 0; d < 3; d++) {
                                double t = centre[3 * (size_t)e + d] - centre[3 * (size_t)g + d];
                                dd += t * t;
                            }
                            double dist = sqrt(dd);
                            if (dist >= r) continue;
                            if (pass == 0) count++;
                            else {
                                f->idx[fill[e]] = g;
                                f->w[fill[e]] = r - dist;
                                f->wsum[e] += r - dist;
                                fill[e]++;
                            }
                        }
                    }
            if (pass == 0) {
                f->start[e + 1] = count;
                total += count;
                if (total > 400LL * 1000 * 1000) {
                    free(head); free(next); free(cell_of); filter_free(f);
                    fail(err, errlen, "the filter radius %g m reaches too far on this mesh (over 400 million pairs)", r);
                    return false;
                }
            }
        }
        free(fill);
    }
    free(head);
    free(next);
    free(cell_of);
    return true;
}

/* u_e^T K_e u_e from the Gauss-point strains and stresses the solver returned (engineering shear strains, so the
 * energy density is 0.5 eps . sig; the incompatible internal modes are already recovered in those values) */
static double element_energy2(const HexModel *m, const SolidResult *res, int e, const double X[8][3]) {
    double detj[8];
    hex8_gauss_detj(X, detj);
    const double *eps = res->gp_strain + 48 * (size_t)e, *sig = res->gp_stress + 48 * (size_t)e;
    double q = 0;
    for (int g = 0; g < 8; g++) {
        double s = 0;
        for (int i = 0; i < 6; i++) s += eps[6 * g + i] * sig[6 * g + i];
        q += s * detj[g];
    }
    return q;
}

void topopt_result_free(TopOptResult *r) {
    if (!r) return;
    free(r->density);
    free(r->passive);
    free(r->history);
    memset(r, 0, sizeof *r);
}

bool topopt_run(const HexModel *m, const SolidLoads *loads, const SolidOptions *opt, const TopOptSettings *set,
                TopOptResult *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    if (!m || !loads || !set) { fail(err, errlen, "no model"); return false; }
    if (m->elem_type != SOLID_ELEM_HEX8) {
        fail(err, errlen, "topology optimisation runs on the voxel mesh (8-node hexahedra); this model has %s elements",
             m->elem_type == SOLID_ELEM_TET4 ? "TET4" : "TET10");
        return false;
    }
    int n = m->nelems;
    if (n <= 0) { fail(err, errlen, "the mesh has no elements"); return false; }
    TopOptSettings s = *set;
    if (s.penalty <= 0) s.penalty = 3.0;
    if (s.move_limit <= 0) s.move_limit = 0.2;
    if (s.x_min < 0) s.x_min = 0;
    if (s.e_min_ratio <= 0) s.e_min_ratio = 1e-9;
    if (s.max_iter <= 0) s.max_iter = 100;
    if (s.change_tol <= 0) s.change_tol = 0.01;
    if (!(s.volume_fraction > 0 && s.volume_fraction <= 1)) {
        fail(err, errlen, "the volume fraction must be between 0 and 1, not %g", s.volume_fraction);
        return false;
    }

    double t0 = now_seconds();
    double *centre = malloc(sizeof(double) * 3 * (size_t)n), *vol = malloc(sizeof(double) * (size_t)n);
    double *x = malloc(sizeof(double) * (size_t)n), *xnew = malloc(sizeof(double) * (size_t)n);
    double *dc = malloc(sizeof(double) * (size_t)n), *dcf = malloc(sizeof(double) * (size_t)n);
    double *scale = malloc(sizeof(double) * (size_t)n);
    unsigned char *passive = malloc((size_t)n);
    TopOptStep *hist = malloc(sizeof(TopOptStep) * (size_t)s.max_iter);
    if (!centre || !vol || !x || !xnew || !dc || !dcf || !scale || !passive || !hist) {
        free(centre); free(vol); free(x); free(xnew); free(dc); free(dcf); free(scale); free(passive); free(hist);
        fail(err, errlen, "out of memory for %d elements", n);
        return false;
    }
    double total_volume = 0, mean_size = 0;
    for (int e = 0; e < n; e++) {
        double X[8][3];
        elem_geometry(m, e, centre + 3 * (size_t)e, &vol[e], X);
        total_volume += vol[e];
    }
    mean_size = cbrt(total_volume / n);
    double r = s.filter_radius > 0 ? s.filter_radius : 1.5 * mean_size;

    int npassive = 0;
    mark_passive(m, loads, s.passive_layers, passive, &npassive);
    double passive_volume = 0;
    for (int e = 0; e < n; e++)
        if (passive[e]) passive_volume += vol[e];
    double target = s.volume_fraction * total_volume;
    if (passive_volume > target) {
        free(centre); free(vol); free(x); free(xnew); free(dc); free(dcf); free(scale); free(passive); free(hist);
        fail(err, errlen,
             "the supports and loads hold %.1f %% of the volume solid, more than the %.1f %% asked for. Ask for a "
             "larger volume fraction, or pick smaller faces",
             100 * passive_volume / total_volume, 100 * s.volume_fraction);
        return false;
    }

    Filter filt;
    bool filtering = r > 0;
    if (filtering && !filter_build(&filt, n, centre, r, err, errlen)) {
        free(centre); free(vol); free(x); free(xnew); free(dc); free(dcf); free(scale); free(passive); free(hist);
        return false;
    }

    /* the uniform start satisfies the volume constraint: with no passive region it is the volume fraction itself
     * (the published starting point), and with one it is what is left for the elements that may move */
    double free_volume = total_volume - passive_volume;
    double x0 = free_volume > 0 ? (target - passive_volume) / free_volume : 1.0;
    if (x0 > 1) x0 = 1;
    if (x0 < s.x_min) x0 = s.x_min;
    for (int e = 0; e < n; e++) x[e] = passive[e] ? 1.0 : x0;
    HexModel mm = *m;
    const double emin = s.e_min_ratio, p = s.penalty;
    double mismatch = 0;
    int iter = 0, solves = 0;
    double compliance = 0, change = 1.0;
    const char *stop = "iteration limit";
    bool ok = true;

    for (iter = 0; iter < s.max_iter; iter++) {
        for (int e = 0; e < n; e++) scale[e] = emin + pow(x[e], p) * (1.0 - emin);
        mm.elem_scale = scale;
        SolidResult res;
        if (!solid_solve(&mm, loads, opt, &res, err, errlen)) { ok = false; break; }
        solves++;
        compliance = res.external_work;
        double twice_energy = 2.0 * res.strain_energy;
        if (fabs(compliance) > 0) {
            double d = fabs(compliance - twice_energy) / fabs(compliance);
            if (d > mismatch) mismatch = d;
        }
        if (iter == 0) out->compliance_initial = compliance;

        for (int e = 0; e < n; e++) {
            double X[8][3], c[3], v;
            elem_geometry(m, e, c, &v, X);
            double q = element_energy2(m, &res, e, X);
            dc[e] = -p * pow(x[e], p - 1.0) * (1.0 - emin) * q / scale[e];
            if (dc[e] > 0) dc[e] = 0; /* the compliance cannot grow with material; guard against round-off */
        }
        solid_result_free(&res);

        if (filtering) {
            for (int e = 0; e < n; e++) {
                double num = 0;
                for (int k = filt.start[e]; k < filt.start[e + 1]; k++) num += filt.w[k] * x[filt.idx[k]] * dc[filt.idx[k]];
                double den = (x[e] > 1e-3 ? x[e] : 1e-3) * filt.wsum[e];
                dcf[e] = den > 0 ? num / den : dc[e];
            }
        } else {
            memcpy(dcf, dc, sizeof(double) * (size_t)n);
        }

        /* optimality criteria: bisection on the Lagrange multiplier of the volume constraint */
        double l1 = 0, l2 = 1e12;
        while ((l2 - l1) / (l1 + l2 + 1e-30) > 1e-9) {
            double lmid = 0.5 * (l1 + l2), v = 0;
            for (int e = 0; e < n; e++) {
                if (passive[e]) { xnew[e] = 1.0; v += vol[e]; continue; }
                double be = -dcf[e] / (lmid * vol[e]);
                double t = x[e] * sqrt(be > 0 ? be : 0);
                double up = x[e] + s.move_limit, dn = x[e] - s.move_limit;
                if (t > up) t = up;
                if (t > 1) t = 1;
                if (t < dn) t = dn;
                if (t < s.x_min) t = s.x_min;
                xnew[e] = t;
                v += t * vol[e];
            }
            if (v > target) l1 = lmid;
            else l2 = lmid;
        }
        change = 0;
        for (int e = 0; e < n; e++) {
            double d = fabs(xnew[e] - x[e]);
            if (d > change) change = d;
            x[e] = xnew[e];
        }
        hist[iter].iter = iter + 1;
        hist[iter].compliance = compliance;
        double vsum = 0;
        for (int e = 0; e < n; e++) vsum += x[e] * vol[e];
        hist[iter].volume_fraction = vsum / total_volume;
        hist[iter].change = change;
        if (set->progress && !set->progress(set->ctx, iter + 1, change)) { stop = "cancelled"; iter++; break; }
        if (change <= s.change_tol) { stop = "converged"; iter++; break; }
    }

    /* the loop reports the compliance of the design it started the iteration with; one more solve gives the
     * compliance of the field that comes out */
    if (ok) {
        for (int e = 0; e < n; e++) scale[e] = emin + pow(x[e], p) * (1.0 - emin);
        mm.elem_scale = scale;
        SolidResult res;
        if (solid_solve(&mm, loads, opt, &res, err, errlen)) {
            solves++;
            compliance = res.external_work;
            if (fabs(compliance) > 0) {
                double d = fabs(compliance - 2.0 * res.strain_energy) / fabs(compliance);
                if (d > mismatch) mismatch = d;
            }
            solid_result_free(&res);
        } else {
            ok = false;
        }
    }

    if (filtering) filter_free(&filt);
    if (!ok) {
        free(centre); free(vol); free(x); free(xnew); free(dc); free(dcf); free(scale); free(passive); free(hist);
        return false;
    }
    if (iter > s.max_iter) iter = s.max_iter;
    double vsum = 0;
    for (int e = 0; e < n; e++) vsum += x[e] * vol[e];
    out->density = x;
    out->passive = passive;
    out->history = hist;
    out->iterations = iter;
    out->solves = solves;
    out->passive_elements = npassive;
    out->compliance = compliance;
    out->volume_fraction = vsum / total_volume;
    out->change = change;
    out->energy_mismatch = mismatch;
    out->seconds = now_seconds() - t0;
    snprintf(out->stop_reason, sizeof out->stop_reason, "%s", stop);
    free(centre);
    free(vol);
    free(xnew);
    free(dc);
    free(dcf);
    free(scale);
    return true;
}
