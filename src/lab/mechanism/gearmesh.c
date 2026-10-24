/* gearmesh.c - see gearmesh.h */
#include "gearmesh.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../fem/hex8.h"

void gear_mesh_defaults(GearMeshSpec *m) {
    memset(m, 0, sizeof *m);
    m->teeth_modelled = 5;
    m->n_across = 14;
    m->n_flank = 28;
    m->n_fillet = 12;
    m->n_rim = 12;
    m->n_face = 8;
    m->flank_grading = 0.25;
    m->rim_grading = 0.25;
    m->bore_blend = 0.5;
    m->junction_deg = 40;
}

/* Geometric growth on [0, len]: m intervals, the first h0 long, their sum len; returns the growth ratio. */
static double growth(int m, double h0, double len) {
    if (m < 1 || h0 * m >= len) return 1;
    double target = len / h0, lo = 1, hi = 4;
    while ((pow(hi, m) - 1) / (hi - 1) < target) hi *= 2;
    for (int it = 0; it < 200; it++) {
        double q = 0.5 * (lo + hi), s = (pow(q, m) - 1) / (q - 1);
        if (s < target) lo = q;
        else hi = q;
    }
    return 0.5 * (lo + hi);
}

/* A distribution of n intervals on [0, 1]: the first interval is `first` times an even one (first < 1 clusters toward
 * 0), growing geometrically; two_sided clusters toward both ends, half the intervals growing from each. */
static void grading(int n, double first, bool two_sided, double *t) {
    for (int i = 0; i <= n; i++) t[i] = (double)i / n;
    if (first <= 0 || first >= 1 || n < 2) return;
    double h0 = first / n;
    if (!two_sided) {
        double q = growth(n, h0, 1);
        double h = h0;
        for (int k = 0; k < n; k++) t[k + 1] = t[k] + h, h *= q;
        t[n] = 1;
        return;
    }
    int m1 = n / 2, m2 = n - m1;
    double q1 = growth(m1, h0, 0.5), q2 = growth(m2, h0, 0.5), h = h0;
    for (int k = 0; k < m1; k++) t[k + 1] = t[k] + h, h *= q1;
    t[m1] = 0.5;
    h = h0;
    double u = 1;
    for (int k = 0; k < m2; k++) t[n - k] = u, u -= h, h *= q2;
    t[m1] = 0.5, t[n] = 1;
}

/* the right side of the outline by arc length */
typedef struct Curve {
    const GearPoint *p;
    double *s; /* cumulative arc length */
    int n;
} Curve;

static void curve_eval(const Curve *c, int i0, int i1, double t, double *x, double *y, double *nx, double *ny) {
    double s = c->s[i0] + t * (c->s[i1] - c->s[i0]);
    int lo = i0, hi = i1;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (c->s[mid] <= s) lo = mid;
        else hi = mid;
    }
    double d = c->s[hi] - c->s[lo], w = d > 0 ? (s - c->s[lo]) / d : 0;
    const GearPoint *a = &c->p[lo], *b = &c->p[hi];
    *x = a->x + w * (b->x - a->x), *y = a->y + w * (b->y - a->y);
    double ux = a->nx + w * (b->nx - a->nx), uy = a->ny + w * (b->ny - a->ny), L = hypot(ux, uy);
    *nx = ux / L, *ny = uy / L;
}

void gear_mesh_free(GearMesh *gm) {
    free(gm->xyz), free(gm->conn), free(gm->tag), free(gm->tooth), free(gm->normal), free(gm->profile);
    free(gm->face), free(gm->face_side), free(gm->face_tooth), free(gm->face_elem);
    memset(gm, 0, sizeof *gm);
}

bool gear_mesh_build(const GearGeometry *g, const GearMeshSpec *ms, double angle0, GearMesh *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    int z = g->spec.teeth, T = ms->teeth_modelled;
    int NA = ms->n_across, NF = ms->n_flank, NQ = ms->n_fillet, NR = ms->n_rim, NZ = ms->n_face;
    if (T < 1 || T > z || NA < 2 || NF < 2 || NQ < 2 || NR < 2 || NZ < 1) {
        snprintf(err, errlen, "gear mesh: 1 to %d teeth and at least 2 elements in each direction (1 through the face)", z);
        return false;
    }
    bool full = T == z;
    int NK = 2 * NQ + NA;
    /* the outline */
    Curve c = {g->right.p, malloc((size_t)g->right.n * sizeof(double)), g->right.n};
    if (!c.s) return snprintf(err, errlen, "gear mesh: out of memory"), false;
    c.s[0] = 0;
    for (int i = 1; i < c.n; i++) c.s[i] = c.s[i - 1] + hypot(c.p[i].x - c.p[i - 1].x, c.p[i].y - c.p[i - 1].y);
    int form = g->right.form, last = c.n - 1;
    /* The tooth block reaches down the fillet to where the outline is inclined `junction_deg` from the circumference,
     * so the rim block's corner there is gentle and the upper fillet, where the root stress peaks, lies in the tooth
     * block with thin elements along it. */
    int junction = form;
    for (int i = 1; i < form; i++) {
        const GearPoint *p = &c.p[i];
        double r = hypot(p->x, p->y), tx = -p->ny, ty = p->nx; /* tangent */
        double along_r = fabs(tx * p->x + ty * p->y) / r, along_t = fabs(-tx * p->y + ty * p->x) / r;
        if (atan2(along_r, along_t) * 180 / M_PI >= ms->junction_deg) {
            junction = i;
            break;
        }
    }
    double r_form = hypot(c.p[form].x, c.p[form].y);
    /* distributions */
    double *sa = malloc((NA + 1) * sizeof(double)), *tf = malloc((NF + 1) * sizeof(double)), *tq = malloc((NQ + 1) * sizeof(double));
    double *wr = malloc((NR + 1) * sizeof(double));
    grading(NA, ms->flank_grading, true, sa);
    grading(NF, 0.35, false, tf); /* finer toward the junction: the upper fillet, where the root stress peaks */
    for (int k = 0; k <= NQ; k++) tq[k] = (double)k / NQ;
    grading(NR, ms->rim_grading, false, wr); /* wr[0] = 0 at the surface */
    /* one pitch in the local frame (tooth on +y): tooth block (NA+1) x (NF+1), rim block (NK+1) x (NR+1) */
    int ntb = (NA + 1) * (NF + 1), nrb = (NK + 1) * (NR + 1);
    double *TB = malloc((size_t)ntb * 2 * sizeof(double)), *RB = malloc((size_t)nrb * 2 * sizeof(double));
    double *TBn = calloc((size_t)ntb * 2, sizeof(double)), *RBn = calloc((size_t)nrb * 2, sizeof(double));
    double *TBr = calloc((size_t)ntb, sizeof(double)), *RBr = calloc((size_t)nrb, sizeof(double));
    if (!sa || !tf || !tq || !wr || !TB || !RB || !TBn || !RBn || !TBr || !RBr) {
        snprintf(err, errlen, "gear mesh: out of memory");
        free(c.s), free(sa), free(tf), free(tq), free(wr), free(TB), free(RB), free(TBn), free(RBn), free(TBr), free(RBr);
        return false;
    }
#define TBI(i, j) ((j) * (NA + 1) + (i))
#define RBI(k, q) ((q) * (NK + 1) + (k))
    /* tooth block by transfinite interpolation between its sides (upper fillet and flank) and the arcs at the junction and tip radii */
    double Lx0, Ly0, Rx0, Ry0, Lx1, Ly1, Rx1, Ry1, nx, ny;
    curve_eval(&c, junction, last, 0, &Rx0, &Ry0, &nx, &ny);
    curve_eval(&c, junction, last, 1, &Rx1, &Ry1, &nx, &ny);
    Lx0 = -Rx0, Ly0 = Ry0, Lx1 = -Rx1, Ly1 = Ry1;
    double rf0 = hypot(Rx0, Ry0), rt = hypot(Rx1, Ry1);
    double thL0 = atan2(Ly0, Lx0), thR0 = atan2(Ry0, Rx0), thL1 = atan2(Ly1, Lx1), thR1 = atan2(Ry1, Rx1);
    for (int j = 0; j <= NF; j++) {
        double rx, ry, rnx, rny;
        curve_eval(&c, junction, last, tf[j], &rx, &ry, &rnx, &rny);
        double lx = -rx, ly = ry;
        for (int i = 0; i <= NA; i++) {
            double s = sa[i], t = tf[j];
            double bth = thL0 + s * (thR0 - thL0), tth = thL1 + s * (thR1 - thL1);
            double bx = rf0 * cos(bth), by = rf0 * sin(bth), tx = rt * cos(tth), ty = rt * sin(tth);
            double x = (1 - s) * lx + s * rx + (1 - t) * bx + t * tx - ((1 - s) * (1 - t) * Lx0 + s * (1 - t) * Rx0 + (1 - s) * t * Lx1 + s * t * Rx1);
            double y = (1 - s) * ly + s * ry + (1 - t) * by + t * ty - ((1 - s) * (1 - t) * Ly0 + s * (1 - t) * Ry0 + (1 - s) * t * Ly1 + s * t * Ry1);
            int id = TBI(i, j);
            TB[2 * id] = x, TB[2 * id + 1] = y;
            if (i == 0) TBn[2 * id] = -rnx, TBn[2 * id + 1] = rny, TBr[id] = hypot(rx, ry);
            if (i == NA) TBn[2 * id] = rnx, TBn[2 * id + 1] = rny, TBr[id] = hypot(rx, ry);
        }
    }
    /* rim block: the top edge (surface) from the left space middle to the right one; columns are cubic curves that
     * leave the surface along its inward normal and meet the bore along the radius */
    double *top = malloc((size_t)(NK + 1) * 4 * sizeof(double));
    for (int k = 0; k <= NK; k++) {
        double x, y, nxx, nyy;
        if (k <= NQ) { /* left fillet, from the space middle (index 0) up to the form point: mirror of the right */
            curve_eval(&c, 0, junction, tq[k], &x, &y, &nxx, &nyy);
            x = -x, nxx = -nxx;
        } else if (k < NQ + NA) {
            int i = k - NQ;
            x = TB[2 * TBI(i, 0)], y = TB[2 * TBI(i, 0) + 1];
            double th = atan2(y, x);
            nxx = cos(th), nyy = sin(th); /* inside the material: the column runs radially */
        } else {
            int kk = NK - k; /* right fillet, from the form point down to the space middle */
            curve_eval(&c, 0, junction, tq[kk], &x, &y, &nxx, &nyy);
        }
        top[4 * k] = x, top[4 * k + 1] = y, top[4 * k + 2] = nxx, top[4 * k + 3] = nyy;
    }
    double th_first = atan2(top[1], top[0]), th_last = atan2(top[4 * NK + 1], top[4 * NK]);
    double rb = g->spec.bore_radius_m;
    /* Inward directions along the top edge: the outline's inward normal on the fillets, the radius under the tooth.
     * Where a fillet meets the tooth's base the edge has a convex corner (it turns by the junction angle), and
     * directions offset from it converge: rows a depth d below cross once the directions turn faster than about 1/d
     * per unit length along the edge. So the directions are smoothed by a Gaussian of the arc length along the edge
     * whose width is the boundary layer's depth, the same however finely the edge is divided; the edge is mirrored
     * about the two space middles (the pitch's cut lines) so that their columns stay radial and the pitch stays
     * symmetric, and so its copies join their neighbours. */
    int nbl = NR / 3 < 2 ? 2 : NR / 3;
    double depth = g->spec.module_m / 6;
    double *dir = malloc((size_t)(NK + 1) * 2 * sizeof(double)), *tmp = malloc((size_t)(NK + 1) * 2 * sizeof(double));
    double *S = malloc((size_t)(NK + 1) * sizeof(double));
    for (int k = 0; k <= NK; k++) {
        double tx = top[4 * k], ty = top[4 * k + 1];
        bool fil = k <= NQ || k >= NQ + NA;
        double dx = fil ? -top[4 * k + 2] : -tx, dy = fil ? -top[4 * k + 3] : -ty, L = hypot(dx, dy);
        tmp[2 * k] = dx / L, tmp[2 * k + 1] = dy / L;
        S[k] = k ? S[k - 1] + hypot(tx - top[4 * (k - 1)], ty - top[4 * (k - 1) + 1]) : 0;
    }
    {
        double sig = depth, u0x = top[0] / hypot(top[0], top[1]), u0y = top[1] / hypot(top[0], top[1]);
        double u1x = top[4 * NK] / hypot(top[4 * NK], top[4 * NK + 1]), u1y = top[4 * NK + 1] / hypot(top[4 * NK], top[4 * NK + 1]);
        for (int k = 0; k <= NK; k++) {
            double sx = 0, sy = 0;
            for (int j = 0; j <= NK; j++)
                for (int img = 0; img < 3; img++) {
                    /* img 0: the edge itself; 1 and 2: its mirror images about the space middles at either end */
                    double s = img == 0 ? S[j] : img == 1 ? -S[j] : 2 * S[NK] - S[j], d = (s - S[k]) / sig;
                    if (fabs(d) > 4) continue;
                    double vx = tmp[2 * j], vy = tmp[2 * j + 1];
                    if (img) {
                        double ux = img == 1 ? u0x : u1x, uy = img == 1 ? u0y : u1y, p = vx * ux + vy * uy;
                        vx = 2 * p * ux - vx, vy = 2 * p * uy - vy;
                    }
                    double wgt = exp(-0.5 * d * d);
                    sx += wgt * vx, sy += wgt * vy;
                }
            double L = hypot(sx, sy);
            dir[2 * k] = sx / L, dir[2 * k + 1] = sy / L;
        }
        dir[0] = -u0x, dir[1] = -u0y, dir[2 * NK] = -u1x, dir[2 * NK + 1] = -u1y;
    }
    free(S);
    /* the boundary layer: its rows offset from the surface along those directions, `nbl` rows to a depth of a
     * sixth of a module; below it, each column runs to the bore in polar coordinates (radius and polar angle both
     * linear along it), from the boundary layer's last row to a bore point whose polar angle is that row's blended
     * toward an even spacing. At every row the columns' polar angles are then a blend of two increasing sequences,
     * so they never cross, however many columns crowd under the fillets. */
    double *bl = malloc((size_t)(NK + 1) * 2 * sizeof(double));
    for (int k = 0; k <= NK; k++) {
        double tx = top[4 * k], ty = top[4 * k + 1];
        for (int q = 0; q <= NR; q++) {
            double w = wr[q], x, y;
            int id = RBI(k, NR - q); /* q counts from the surface; rows count from the bore */
            if (q <= nbl) {
                double d = depth * w / wr[nbl];
                x = tx + d * dir[2 * k], y = ty + d * dir[2 * k + 1];
                if (q == nbl) bl[2 * k] = x, bl[2 * k + 1] = y;
            } else {
                x = y = 0; /* filled below, once the boundary layer's last row is known */
            }
            RB[2 * id] = x, RB[2 * id + 1] = y;
            if (q == 0 && (k <= NQ || k >= NQ + NA)) RBn[2 * id] = top[4 * k + 2], RBn[2 * id + 1] = top[4 * k + 3], RBr[id] = hypot(tx, ty);
        }
    }
    double thb_first = atan2(bl[1], bl[0]), thb_last = atan2(bl[2 * NK + 1], bl[2 * NK]);
    for (int k = 0; k <= NK; k++) {
        double th_bl = atan2(bl[2 * k + 1], bl[2 * k]), th_even = thb_first + (thb_last - thb_first) * k / NK;
        double thb = (1 - ms->bore_blend) * th_bl + ms->bore_blend * th_even, r_bl = hypot(bl[2 * k], bl[2 * k + 1]);
        for (int q = nbl + 1; q <= NR; q++) {
            double s2 = (wr[q] - wr[nbl]) / (1 - wr[nbl]), rr = (1 - s2) * r_bl + s2 * rb, th = (1 - s2) * th_bl + s2 * thb;
            int id = RBI(k, NR - q);
            RB[2 * id] = rr * cos(th), RB[2 * id + 1] = rr * sin(th);
        }
    }
    (void)th_first, (void)th_last;
    free(dir), free(tmp), free(bl);
    free(top);
    /* global numbering: pitches p = 0 .. T-1 counter-clockwise; pitch p's right column (k = NK) is pitch p-1's left
     * column (k = 0); the tooth block's bottom row is the rim block's top row between the fillets */
    int *tbmap = malloc((size_t)T * ntb * sizeof(int)), *rbmap = malloc((size_t)T * nrb * sizeof(int));
    int n2 = 0;
    for (int p = 0; p < T; p++) {
        /* pitch p-1 is numbered already: its left column is this pitch's right column */
        for (int q = 0; q <= NR; q++)
            for (int k = 0; k <= NK; k++)
                rbmap[p * nrb + RBI(k, q)] = (k == NK && p > 0) ? rbmap[(p - 1) * nrb + RBI(0, q)] : n2++;
        for (int i = 0; i <= NA; i++) tbmap[p * ntb + TBI(i, 0)] = rbmap[p * nrb + RBI(NQ + i, NR)];
        for (int j = 1; j <= NF; j++)
            for (int i = 0; i <= NA; i++) tbmap[p * ntb + TBI(i, j)] = n2++;
    }
    if (full) { /* close the ring: the last pitch's left column is the first pitch's right column */
        int n_old = n2;
        int *alias = malloc((size_t)n_old * sizeof(int));
        for (int i = 0; i < n_old; i++) alias[i] = i;
        for (int q = 0; q <= NR; q++) alias[rbmap[(T - 1) * nrb + RBI(0, q)]] = rbmap[RBI(NK, q)];
        /* renumber densely */
        int *dense = malloc((size_t)n_old * sizeof(int)), nd = 0;
        for (int i = 0; i < n_old; i++) dense[i] = alias[i] == i ? nd++ : -1;
        for (int i = 0; i < n_old; i++) if (dense[i] < 0) dense[i] = dense[alias[i]];
        for (int i = 0; i < T * nrb; i++) rbmap[i] = dense[rbmap[i]];
        for (int i = 0; i < T * ntb; i++) tbmap[i] = dense[tbmap[i]];
        n2 = nd;
        free(alias), free(dense);
    }
    /* 2D coordinates, normals, profile radii, tags */
    double *x2 = malloc((size_t)n2 * 2 * sizeof(double)), *n2v = calloc((size_t)n2 * 2, sizeof(double)), *r2 = calloc((size_t)n2, sizeof(double));
    int *tag2 = calloc((size_t)n2, sizeof(int)), *tooth2 = malloc((size_t)n2 * sizeof(int));
    for (int i = 0; i < n2; i++) tooth2[i] = -1;
    double pitch = 2 * M_PI / z;
    for (int p = 0; p < T; p++) {
        double rot = angle0 + p * pitch - M_PI / 2, cr = cos(rot), sr = sin(rot);
        for (int q = 0; q <= NR; q++)
            for (int k = 0; k <= NK; k++) {
                int id = RBI(k, q), gid = rbmap[p * nrb + id];
                double x = RB[2 * id], y = RB[2 * id + 1];
                x2[2 * gid] = x * cr - y * sr, x2[2 * gid + 1] = x * sr + y * cr;
                if (q == 0) tag2[gid] |= GM_BORE;
                if (q == NR && (k <= NQ || k >= NQ + NA)) {
                    tag2[gid] |= GM_FILLET | (k <= NQ ? GM_LEFT : GM_RIGHT);
                    if (tooth2[gid] < 0) tooth2[gid] = p;
                    double ux = RBn[2 * id], uy = RBn[2 * id + 1];
                    n2v[2 * gid] = ux * cr - uy * sr, n2v[2 * gid + 1] = ux * sr + uy * cr, r2[gid] = RBr[id];
                }
                if (!full && ((p == T - 1 && k == 0) || (p == 0 && k == NK))) tag2[gid] |= GM_CUT;
            }
        for (int j = 0; j <= NF; j++)
            for (int i = 0; i <= NA; i++) {
                int id = TBI(i, j), gid = tbmap[p * ntb + id];
                double x = TB[2 * id], y = TB[2 * id + 1];
                x2[2 * gid] = x * cr - y * sr, x2[2 * gid + 1] = x * sr + y * cr;
                if (i == 0 || i == NA) {
                    bool inv = TBr[id] >= r_form * (1 - 1e-9);
                    tag2[gid] |= (inv ? GM_FLANK : GM_FILLET) | (i == 0 ? GM_LEFT : GM_RIGHT);
                    tooth2[gid] = p;
                    double ux = TBn[2 * id], uy = TBn[2 * id + 1];
                    n2v[2 * gid] = ux * cr - uy * sr, n2v[2 * gid + 1] = ux * sr + uy * cr, r2[gid] = TBr[id];
                }
                if (j == NF) tag2[gid] |= GM_TIP;
            }
    }
    /* 2D quads, counter-clockwise */
    int nq = T * (NA * NF + NK * NR);
    int *quad = malloc((size_t)nq * 4 * sizeof(int)), qn = 0;
    int *quad_tb = malloc((size_t)nq * sizeof(int)); /* the tooth-block index (p, i, j) of a quad, or -1 */
    for (int p = 0; p < T; p++) {
        for (int q = 0; q < NR; q++)
            for (int k = 0; k < NK; k++) {
                int *Q = &quad[4 * qn];
                Q[0] = rbmap[p * nrb + RBI(k, q)], Q[1] = rbmap[p * nrb + RBI(k + 1, q)];
                Q[2] = rbmap[p * nrb + RBI(k + 1, q + 1)], Q[3] = rbmap[p * nrb + RBI(k, q + 1)];
                quad_tb[qn++] = -1;
            }
        for (int j = 0; j < NF; j++)
            for (int i = 0; i < NA; i++) {
                int *Q = &quad[4 * qn];
                Q[0] = tbmap[p * ntb + TBI(i, j)], Q[1] = tbmap[p * ntb + TBI(i + 1, j)];
                Q[2] = tbmap[p * ntb + TBI(i + 1, j + 1)], Q[3] = tbmap[p * ntb + TBI(i, j + 1)];
                quad_tb[qn++] = (p * NF + j) * NA + i;
            }
    }
    /* extrusion */
    double b = g->spec.face_width_m;
    int nn = n2 * (NZ + 1), ne = qn * NZ;
    out->xyz = malloc((size_t)nn * 3 * sizeof(double));
    out->conn = malloc((size_t)ne * 8 * sizeof(int));
    out->tag = malloc((size_t)nn * sizeof(int));
    out->tooth = malloc((size_t)nn * sizeof(int));
    out->normal = malloc((size_t)nn * 2 * sizeof(double));
    out->profile = malloc((size_t)nn * sizeof(double));
    for (int l = 0; l <= NZ; l++) { /* a helical gear's section turns by twist z about the axis */
        double zz = b * l / NZ, ct = cos(g->twist * zz), st = sin(g->twist * zz);
        for (int i = 0; i < n2; i++) {
            int id = l * n2 + i;
            double x = x2[2 * i], y = x2[2 * i + 1], nx = n2v[2 * i], ny = n2v[2 * i + 1];
            out->xyz[3 * id] = ct * x - st * y, out->xyz[3 * id + 1] = st * x + ct * y, out->xyz[3 * id + 2] = zz;
            out->tag[id] = tag2[i] | (l == 0 ? GM_END0 : 0) | (l == NZ ? GM_END1 : 0);
            out->tooth[id] = tooth2[i];
            out->normal[2 * id] = ct * nx - st * ny, out->normal[2 * id + 1] = st * nx + ct * ny;
            out->profile[id] = r2[i];
        }
    }
    for (int l = 0; l < NZ; l++)
        for (int e = 0; e < qn; e++) {
            int *C = &out->conn[8 * (l * qn + e)];
            for (int a = 0; a < 4; a++) C[a] = l * n2 + quad[4 * e + a], C[4 + a] = (l + 1) * n2 + quad[4 * e + a];
        }
    out->nnodes = nn, out->nelems = ne, out->teeth = T, out->tooth_angle0 = angle0;
    /* flank faces for contact: left flank (i = 0) and right flank (i = NA) of every tooth, outward */
    int nf = T * 2 * NF * NZ;
    out->face = malloc((size_t)nf * 4 * sizeof(int));
    out->face_side = malloc((size_t)nf * sizeof(int));
    out->face_tooth = malloc((size_t)nf * sizeof(int));
    out->face_elem = malloc((size_t)nf * sizeof(int));
    int fn = 0;
    for (int p = 0; p < T; p++)
        for (int side = 0; side < 2; side++) {
            int i = side ? NA : 0, ie = side ? NA - 1 : 0;
            for (int j = 0; j < NF; j++)
                for (int l = 0; l < NZ; l++) {
                    int a = l * n2 + tbmap[p * ntb + TBI(i, j)], bnode = l * n2 + tbmap[p * ntb + TBI(i, j + 1)];
                    int a1 = (l + 1) * n2 + tbmap[p * ntb + TBI(i, j)], b1 = (l + 1) * n2 + tbmap[p * ntb + TBI(i, j + 1)];
                    int *F = &out->face[4 * fn];
                    if (side) F[0] = a, F[1] = bnode, F[2] = b1, F[3] = a1;
                    else F[0] = a, F[1] = a1, F[2] = b1, F[3] = bnode;
                    out->face_side[fn] = side ? GM_RIGHT : GM_LEFT;
                    out->face_tooth[fn] = p;
                    /* the element: quads are numbered pitch by pitch, rim block first */
                    int qidx = p * (NA * NF + NK * NR) + NK * NR + j * NA + ie;
                    out->face_elem[fn] = l * qn + qidx;
                    fn++;
                }
        }
    out->nfaces = fn;
    /* quality: the smallest Gauss-point Jacobian over the element's mean, over the mesh */
    double worst = 1e300, wc[3] = {0, 0, 0};
    for (int e = 0; e < ne; e++) {
        double X[8][3], dj[8], mean = 0, mn = 1e300;
        for (int a = 0; a < 8; a++)
            for (int d = 0; d < 3; d++) X[a][d] = out->xyz[3 * out->conn[8 * e + a] + d];
        hex8_gauss_detj(X, dj);
        double mx = -1e300;
        for (int k = 0; k < 8; k++) mean += dj[k] / 8, mn = fmin(mn, dj[k]), mx = fmax(mx, dj[k]);
        double ratio = mx > 0 ? mn / mx : -1; /* <= 0: inverted somewhere */
        if (ratio < worst) {
            worst = ratio;
            for (int d = 0; d < 3; d++) wc[d] = 0.125 * (X[0][d] + X[1][d] + X[2][d] + X[3][d] + X[4][d] + X[5][d] + X[6][d] + X[7][d]);
        }
        (void)mean;
    }
    out->min_jacobian_ratio = worst;
    free(c.s), free(sa), free(tf), free(tq), free(wr), free(TB), free(RB), free(TBn), free(RBn), free(TBr), free(RBr);
    free(tbmap), free(rbmap), free(x2), free(n2v), free(r2), free(tag2), free(tooth2), free(quad), free(quad_tb);
    if (worst <= 0) {
        gear_mesh_free(out);
        snprintf(err, errlen, "gear mesh: an element is inverted (Jacobian ratio %.3g at r = %.4g mm, polar angle %.2f deg); change the element counts or the grading",
                 worst, 1e3 * hypot(wc[0], wc[1]), atan2(wc[1], wc[0]) * 180 / M_PI);
        return false;
    }
    return true;
#undef TBI
#undef RBI
}

/* a point and its outward normal mapped by inversion in the circle of radius R: the radius goes to R^2 / r, the polar
 * angle stays, and a direction has its radial part reversed (the map's Jacobian at the point, up to scale) */
static void invert_point(double R2, double *x, double *y, double *nx, double *ny) {
    double r = hypot(*x, *y), ex = *x / r, ey = *y / r, k = R2 / (r * r);
    *x *= k, *y *= k;
    if (nx) {
        double d = *nx * ex + *ny * ey, ux = *nx - 2 * d * ex, uy = *ny - 2 * d * ey, L = hypot(ux, uy);
        *nx = ux / L, *ny = uy / L;
    }
}

bool gear_mesh_build_internal(const GearGeometry *g, const GearMeshSpec *ms, double angle0, GearMesh *out, char *err, size_t errlen) {
    if (!g->internal) return snprintf(err, errlen, "gear mesh: gear_mesh_build_internal needs an internal gear"), false;
    double R2 = g->r * g->r;
    GearGeometry inv = *g;
    inv.internal = false;
    inv.right.p = malloc((size_t)g->right.n * sizeof(GearPoint));
    if (!inv.right.p) return snprintf(err, errlen, "gear mesh: out of memory"), false;
    for (int i = 0; i < g->right.n; i++) {
        GearPoint p = g->right.p[i];
        invert_point(R2, &p.x, &p.y, &p.nx, &p.ny);
        inv.right.p[i] = p;
    }
    inv.right.r_form = R2 / g->right.r_form, inv.r_a = R2 / g->r_a, inv.r_f = R2 / g->r_f;
    inv.spec.bore_radius_m = R2 / g->spec.bore_radius_m;
    bool ok = gear_mesh_build(&inv, ms, angle0, out, err, errlen);
    free(inv.right.p);
    if (!ok) return false;
    for (int i = 0; i < out->nnodes; i++) {
        double *X = &out->xyz[3 * i], nx = out->normal[2 * i], ny = out->normal[2 * i + 1];
        bool surf = nx != 0 || ny != 0;
        invert_point(R2, &X[0], &X[1], surf ? &nx : NULL, surf ? &ny : NULL);
        if (surf) out->normal[2 * i] = nx, out->normal[2 * i + 1] = ny;
        if (out->profile[i] > 0) out->profile[i] = R2 / out->profile[i];
    }
    for (int e = 0; e < out->nelems; e++) { /* [0 1 2 3 | 4 5 6 7] -> [0 3 2 1 | 4 7 6 5] */
        int *c = &out->conn[8 * e], t;
        t = c[1], c[1] = c[3], c[3] = t;
        t = c[5], c[5] = c[7], c[7] = t;
    }
    for (int f = 0; f < out->nfaces; f++) {
        int *F = &out->face[4 * f], t = F[1];
        F[1] = F[3], F[3] = t;
    }
    double worst = 1e300;
    for (int e = 0; e < out->nelems; e++) {
        double X[8][3], dj[8], mn = 1e300, mx = -1e300;
        for (int a = 0; a < 8; a++)
            for (int d = 0; d < 3; d++) X[a][d] = out->xyz[3 * out->conn[8 * e + a] + d];
        hex8_gauss_detj(X, dj);
        for (int k = 0; k < 8; k++) mn = fmin(mn, dj[k]), mx = fmax(mx, dj[k]);
        worst = fmin(worst, mx > 0 ? mn / mx : -1);
    }
    out->min_jacobian_ratio = worst;
    if (worst <= 0) {
        gear_mesh_free(out);
        return snprintf(err, errlen, "gear mesh: an element of the ring is inverted; change the element counts"), false;
    }
    return true;
}
