/* contact.c - collision shapes, narrow phase and the contact step driver (see contact.h) */
#include "contact.h"

#include "assembly.h"

#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const SHAPE_NAMES[SHAPE_TYPES] = {"sphere", "capsule", "box", "plane", "hull"};
const char *shape_type_name(ShapeType t) { return (unsigned)t < SHAPE_TYPES ? SHAPE_NAMES[t] : "?"; }
int shape_type_from_name(const char *s) {
    for (int i = 0; s && i < SHAPE_TYPES; i++)
        if (!strcmp(s, SHAPE_NAMES[i])) return i;
    return -1;
}

void contact_options_default(ContactOptions *o) {
    memset(o, 0, sizeof *o);
    o->margin = 1e-3;
    o->restitution_speed = 1e-3;
    o->max_iterations = 500;
    o->tolerance = 1e-10;
    o->exclude_jointed = true;
}

/* ------------------------------------------------------------------------------------------------ helpers */

static int emit(MbContactRow *out, int cap, int n, const double nrm[3], const double pa[3], const double pb[3], double gap) {
    if (n >= cap) return n;
    MbContactRow *r = &out[n];
    memset(r, 0, sizeof *r);
    mv3_copy(r->normal, nrm);
    mv3_copy(r->point_a, pa);
    mv3_copy(r->point_b, pb);
    r->gap = gap;
    return n + 1;
}

static void axis_of(const MPose *T, int k, double out[3]) { out[0] = T->R[3 * 0 + k], out[1] = T->R[3 * 1 + k], out[2] = T->R[3 * 2 + k]; }

/* closest points between segments p0 + s d0 and q0 + t d1, s, t in [0, 1] (Ericson, Real-Time Collision Detection 5.1.9) */
static void segment_closest(const double p0[3], const double d0[3], const double q0[3], const double d1[3], double *s_out, double *t_out) {
    double r[3];
    mv3_sub(r, p0, q0);
    double a = mv3_dot(d0, d0), e = mv3_dot(d1, d1), f = mv3_dot(d1, r), s, t;
    if (a <= 1e-30 && e <= 1e-30) {
        *s_out = *t_out = 0;
        return;
    }
    if (a <= 1e-30) {
        s = 0, t = fmin(1, fmax(0, f / e));
    } else {
        double c = mv3_dot(d0, r);
        if (e <= 1e-30) {
            t = 0, s = fmin(1, fmax(0, -c / a));
        } else {
            double b = mv3_dot(d0, d1), denom = a * e - b * b;
            s = denom > 1e-14 * a * e ? fmin(1, fmax(0, (b * f - c * e) / denom)) : 0;
            t = (b * s + f) / e;
            if (t < 0) t = 0, s = fmin(1, fmax(0, -c / a));
            else if (t > 1) t = 1, s = fmin(1, fmax(0, (b - c) / a));
        }
    }
    *s_out = s, *t_out = t;
}

/* sphere A (centre ca, radius ra) against sphere B */
static int sphere_sphere(const double ca[3], double ra, const double cb[3], double rb, double margin, MbContactRow *out, int cap, int n) {
    double d[3];
    mv3_sub(d, ca, cb);
    double dist = mv3_norm(d), gap = dist - ra - rb;
    if (gap > margin) return n;
    double nrm[3] = {0, 0, 1};
    if (dist > 1e-15) mv3_scale(nrm, d, 1 / dist);
    double pa[3], pb[3];
    for (int k = 0; k < 3; k++) pa[k] = ca[k] - ra * nrm[k], pb[k] = cb[k] + rb * nrm[k];
    return emit(out, cap, n, nrm, pa, pb, gap);
}

/* point-like sphere A against plane B (outward normal np through p0) */
static int sphere_plane(const double c[3], double r, const double np[3], const double p0[3], double margin, MbContactRow *out, int cap, int n) {
    double d[3];
    mv3_sub(d, c, p0);
    double dist = mv3_dot(np, d), gap = dist - r;
    if (gap > margin) return n;
    double pa[3], pb[3];
    for (int k = 0; k < 3; k++) pa[k] = c[k] - r * np[k], pb[k] = c[k] - dist * np[k];
    return emit(out, cap, n, np, pa, pb, gap);
}

/* sphere A against box B */
static int sphere_box(const double c[3], double r, const MPose *TB, const double h[3], double margin, MbContactRow *out, int cap, int n) {
    double d[3], cl[3], ql[3];
    mv3_sub(d, c, TB->p);
    mm3_tmulv(cl, TB->R, d);
    bool inside = true;
    for (int k = 0; k < 3; k++) {
        ql[k] = fmin(h[k], fmax(-h[k], cl[k]));
        inside &= fabs(cl[k]) <= h[k];
    }
    double nrm[3], pa[3], pb[3], gap;
    if (inside) { /* centre inside the box: push out through the nearest face */
        int best = 0;
        double depth = h[0] - fabs(cl[0]);
        for (int k = 1; k < 3; k++)
            if (h[k] - fabs(cl[k]) < depth) depth = h[k] - fabs(cl[k]), best = k;
        double nl[3] = {0, 0, 0};
        nl[best] = cl[best] >= 0 ? 1 : -1;
        mm3_mulv(nrm, TB->R, nl);
        ql[best] = nl[best] * h[best];
        gap = -depth - r;
    } else {
        double q[3], v[3];
        mm3_mulv(q, TB->R, ql);
        mv3_addto(q, TB->p);
        mv3_sub(v, c, q);
        double dist = mv3_norm(v);
        gap = dist - r;
        if (gap > margin) return n;
        mv3_scale(nrm, v, 1 / dist);
    }
    if (gap > margin) return n;
    mm3_mulv(pb, TB->R, ql);
    mv3_addto(pb, TB->p);
    for (int k = 0; k < 3; k++) pa[k] = c[k] - r * nrm[k];
    return emit(out, cap, n, nrm, pa, pb, gap);
}

static double box_distance_sq(const double x[3], const MPose *TB, const double h[3]) {
    double d[3], l[3], s = 0;
    mv3_sub(d, x, TB->p);
    mm3_tmulv(l, TB->R, d);
    for (int k = 0; k < 3; k++) {
        double e = fabs(l[k]) - h[k];
        if (e > 0) s += e * e;
    }
    return s;
}

/* Sutherland-Hodgman clipping of a polygon against the half-space dot(nrm, x) <= off */
static int clip_polygon(const double in[][3], int nin, const double nrm[3], double off, double out[][3]) {
    int nout = 0;
    for (int i = 0; i < nin; i++) {
        const double *a = in[i], *b = in[(i + 1) % nin];
        double da = mv3_dot(nrm, a) - off, db = mv3_dot(nrm, b) - off;
        if (da <= 0) mv3_copy(out[nout++], a);
        if ((da < 0 && db > 0) || (da > 0 && db < 0)) {
            double t = da / (da - db);
            for (int k = 0; k < 3; k++) out[nout][k] = a[k] + t * (b[k] - a[k]);
            nout++;
        }
    }
    return nout;
}

/* box A against box B by separating axes and face clipping; the normal points from B to A */
static int box_box(const MPose *TA, const double ha[3], const MPose *TB, const double hb[3], double margin, MbContactRow *out, int cap, int n) {
    double a[3][3], b[3][3], d[3];
    for (int k = 0; k < 3; k++) axis_of(TA, k, a[k]), axis_of(TB, k, b[k]);
    mv3_sub(d, TB->p, TA->p);
    double best_face = -DBL_MAX, best_edge = -DBL_MAX, face_axis[3] = {0}, edge_axis[3] = {0};
    int face_owner = -1, face_k = -1, edge_i = -1, edge_j = -1;
    for (int pass = 0; pass < 2; pass++)
        for (int k = 0; k < 3; k++) {
            double L[3];
            mv3_copy(L, pass ? b[k] : a[k]);
            if (mv3_dot(d, L) < 0) mv3_scale(L, L, -1);
            double ra = 0, rb = 0;
            for (int m = 0; m < 3; m++) ra += ha[m] * fabs(mv3_dot(a[m], L)), rb += hb[m] * fabs(mv3_dot(b[m], L));
            double sep = mv3_dot(d, L) - ra - rb;
            if (sep > margin) return n;
            if (sep > best_face + 1e-12 * (ra + rb)) best_face = sep, face_owner = pass, face_k = k, mv3_copy(face_axis, L);
        }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double L[3];
            mv3_cross(L, a[i], b[j]);
            double len = mv3_norm(L);
            if (len < 1e-6) continue;
            mv3_scale(L, L, 1 / len);
            if (mv3_dot(d, L) < 0) mv3_scale(L, L, -1);
            double ra = 0, rb = 0;
            for (int m = 0; m < 3; m++) ra += ha[m] * fabs(mv3_dot(a[m], L)), rb += hb[m] * fabs(mv3_dot(b[m], L));
            double sep = mv3_dot(d, L) - ra - rb;
            if (sep > margin) return n;
            if (sep > best_edge) best_edge = sep, edge_i = i, edge_j = j, mv3_copy(edge_axis, L);
        }
    double size = fmax(ha[0] + ha[1] + ha[2], hb[0] + hb[1] + hb[2]);
    double nrm[3];
    if (edge_i >= 0 && best_edge > best_face + 1e-3 * size) { /* edge-edge */
        const double *L = edge_axis;
        double ea[3], eb[3], da[3], db[3];
        mv3_copy(ea, TA->p), mv3_copy(eb, TB->p);
        for (int m = 0; m < 3; m++) {
            if (m != edge_i) mv3_addscaled(ea, a[m], (mv3_dot(a[m], L) >= 0 ? 1 : -1) * ha[m]);
            if (m != edge_j) mv3_addscaled(eb, b[m], -(mv3_dot(b[m], L) >= 0 ? 1 : -1) * hb[m]);
        }
        double p0[3], q0[3];
        for (int k = 0; k < 3; k++) {
            p0[k] = ea[k] - ha[edge_i] * a[edge_i][k], da[k] = 2 * ha[edge_i] * a[edge_i][k];
            q0[k] = eb[k] - hb[edge_j] * b[edge_j][k], db[k] = 2 * hb[edge_j] * b[edge_j][k];
        }
        double s, t, pa[3], pb[3];
        segment_closest(p0, da, q0, db, &s, &t);
        for (int k = 0; k < 3; k++) pa[k] = p0[k] + s * da[k], pb[k] = q0[k] + t * db[k];
        double diff[3];
        mv3_sub(diff, pb, pa);
        double gap = mv3_dot(L, diff);
        mv3_scale(nrm, L, -1);
        return gap <= margin ? emit(out, cap, n, nrm, pa, pb, gap) : n;
    }
    /* face contact: reference face on the owner, incident face on the other box */
    const MPose *Tr = face_owner ? TB : TA, *Ti = face_owner ? TA : TB;
    const double *hr = face_owner ? hb : ha, *hi = face_owner ? ha : hb;
    double (*ax_r)[3] = face_owner ? b : a, (*ax_i)[3] = face_owner ? a : b;
    double nref[3]; /* outward normal of the reference face, towards the other box */
    if (face_owner == 0)
        mv3_copy(nref, face_axis);
    else
        mv3_scale(nref, face_axis, -1);
    int inc = 0;
    double most = DBL_MAX, sgn = 1;
    for (int k = 0; k < 3; k++)
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            double dt = s2 * mv3_dot(ax_i[k], nref);
            if (dt < most) most = dt, inc = k, sgn = s2;
        }
    double fc[3], u[3], v[3], poly[16][3], tmp[16][3];
    int u_k = (inc + 1) % 3, v_k = (inc + 2) % 3;
    mv3_copy(fc, Ti->p);
    mv3_addscaled(fc, ax_i[inc], sgn * hi[inc]);
    mv3_scale(u, ax_i[u_k], hi[u_k]);
    mv3_scale(v, ax_i[v_k], hi[v_k]);
    static const int SU[4] = {1, -1, -1, 1}, SV[4] = {1, 1, -1, -1};
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 3; k++) poly[c][k] = fc[k] + SU[c] * u[k] + SV[c] * v[k];
    int np = 4;
    for (int m = 0; m < 3 && np > 0; m++) {
        if (m == face_k) continue;
        double off = mv3_dot(ax_r[m], Tr->p) + hr[m], negax[3];
        np = clip_polygon((const double (*)[3])poly, np, ax_r[m], off, tmp);
        mv3_scale(negax, ax_r[m], -1);
        np = clip_polygon((const double (*)[3])tmp, np, negax, -mv3_dot(ax_r[m], Tr->p) + hr[m], poly);
    }
    double rc[3];
    mv3_copy(rc, Tr->p);
    mv3_addscaled(rc, nref, hr[face_k]);
    /* normal from B to A */
    if (face_owner == 0)
        mv3_scale(nrm, nref, -1);
    else
        mv3_copy(nrm, nref);
    for (int c = 0; c < np; c++) {
        double diff[3];
        mv3_sub(diff, poly[c], rc);
        double gap = mv3_dot(nref, diff);
        if (gap > margin) continue;
        double onref[3];
        for (int k = 0; k < 3; k++) onref[k] = poly[c][k] - gap * nref[k];
        if (face_owner == 0) /* reference on A: incident points belong to B */
            n = emit(out, cap, n, nrm, onref, poly[c], gap);
        else
            n = emit(out, cap, n, nrm, poly[c], onref, gap);
    }
    return n;
}

/* capsule A (segment s0 + t sd, radius r) against box B: closest point on the segment by golden-section search on the
 * convex squared distance, plus both end points when they are in contact (a capsule lying on a face) */
static int capsule_box(const double s0[3], const double sd[3], double r, const MPose *TB, const double h[3], double margin, MbContactRow *out, int cap, int n) {
    double lo = 0, hi = 1, g = 0.5 * (sqrt(5) - 1);
    double x1 = hi - g * (hi - lo), x2 = lo + g * (hi - lo), p[3];
    for (int k = 0; k < 3; k++) p[k] = s0[k] + x1 * sd[k];
    double f1 = box_distance_sq(p, TB, h);
    for (int k = 0; k < 3; k++) p[k] = s0[k] + x2 * sd[k];
    double f2 = box_distance_sq(p, TB, h);
    for (int it = 0; it < 60; it++) {
        if (f1 <= f2) {
            hi = x2, x2 = x1, f2 = f1, x1 = hi - g * (hi - lo);
            for (int k = 0; k < 3; k++) p[k] = s0[k] + x1 * sd[k];
            f1 = box_distance_sq(p, TB, h);
        } else {
            lo = x1, x1 = x2, f1 = f2, x2 = lo + g * (hi - lo);
            for (int k = 0; k < 3; k++) p[k] = s0[k] + x2 * sd[k];
            f2 = box_distance_sq(p, TB, h);
        }
    }
    double tbest = 0.5 * (lo + hi);
    int n0 = n;
    for (int k = 0; k < 3; k++) p[k] = s0[k] + tbest * sd[k];
    n = sphere_box(p, r, TB, h, margin, out, cap, n);
    for (int e = 0; e < 2; e++) {
        double q[3], t = (double)e;
        if (fabs(t - tbest) < 1e-3) continue;
        for (int k = 0; k < 3; k++) q[k] = s0[k] + t * sd[k];
        int before = n;
        n = sphere_box(q, r, TB, h, margin, out, cap, n);
        if (n > before && n0 < before && fabs(out[before].gap - out[n0].gap) > 0.25 * r) n = before; /* keep end points only when parallel */
    }
    return n;
}

static void flip(MbContactRow *r) {
    double t[3];
    mv3_scale(r->normal, r->normal, -1);
    mv3_copy(t, r->point_a);
    mv3_copy(r->point_a, r->point_b);
    mv3_copy(r->point_b, t);
}

/* ------------------------------------------------------------------------------------------------ dispatch */

int contact_pair(const ContactShape *A, const MPose *TA, const ContactShape *B, const MPose *TB, double margin, MbContactRow *out, int cap) {
    /* canonical order by type so each pair is written once */
    if (A->type > B->type) {
        int n = contact_pair(B, TB, A, TA, margin, out, cap);
        for (int i = 0; i < n; i++) flip(&out[i]);
        return n;
    }
    double za[3], zb[3], ca[3], cb[3];
    axis_of(TA, 2, za), axis_of(TB, 2, zb);
    mv3_copy(ca, TA->p), mv3_copy(cb, TB->p);
    int n = 0;
    switch (A->type) {
    case SHAPE_SPHERE:
        switch (B->type) {
        case SHAPE_SPHERE: return sphere_sphere(ca, A->radius, cb, B->radius, margin, out, cap, 0);
        case SHAPE_CAPSULE: {
            double s0[3], sd[3], s, t, q[3];
            for (int k = 0; k < 3; k++) s0[k] = cb[k] - B->half_length * zb[k], sd[k] = 2 * B->half_length * zb[k];
            segment_closest(ca, (double[3]){0, 0, 0}, s0, sd, &s, &t);
            for (int k = 0; k < 3; k++) q[k] = s0[k] + t * sd[k];
            return sphere_sphere(ca, A->radius, q, B->radius, margin, out, cap, 0);
        }
        case SHAPE_BOX: return sphere_box(ca, A->radius, TB, B->half, margin, out, cap, 0);
        case SHAPE_PLANE: return sphere_plane(ca, A->radius, zb, cb, margin, out, cap, 0);
        default: return -1;
        }
    case SHAPE_CAPSULE: {
        double s0[3], sd[3];
        for (int k = 0; k < 3; k++) s0[k] = ca[k] - A->half_length * za[k], sd[k] = 2 * A->half_length * za[k];
        switch (B->type) {
        case SHAPE_CAPSULE: {
            double q0[3], qd[3], s, t, p[3], q[3];
            for (int k = 0; k < 3; k++) q0[k] = cb[k] - B->half_length * zb[k], qd[k] = 2 * B->half_length * zb[k];
            segment_closest(s0, sd, q0, qd, &s, &t);
            for (int k = 0; k < 3; k++) p[k] = s0[k] + s * sd[k], q[k] = q0[k] + t * qd[k];
            n = sphere_sphere(p, A->radius, q, B->radius, margin, out, cap, 0);
            if (fabs(mv3_dot(za, zb)) > 0.9999) /* parallel: the overlap ends give a stable two-point contact */
                for (int e = 0; e < 2; e++) {
                    double pe[3], tt, ss;
                    for (int k = 0; k < 3; k++) pe[k] = s0[k] + e * sd[k];
                    segment_closest(pe, (double[3]){0, 0, 0}, q0, qd, &ss, &tt);
                    if (tt <= 0 || tt >= 1) continue;
                    for (int k = 0; k < 3; k++) q[k] = q0[k] + tt * qd[k];
                    n = sphere_sphere(pe, A->radius, q, B->radius, margin, out, cap, n);
                }
            return n;
        }
        case SHAPE_BOX: return capsule_box(s0, sd, A->radius, TB, B->half, margin, out, cap, 0);
        case SHAPE_PLANE:
            for (int e = 0; e < 2; e++) {
                double pe[3];
                for (int k = 0; k < 3; k++) pe[k] = s0[k] + e * sd[k];
                n = sphere_plane(pe, A->radius, zb, cb, margin, out, cap, n);
            }
            return n;
        default: return -1;
        }
    }
    case SHAPE_BOX:
        switch (B->type) {
        case SHAPE_BOX: return box_box(TA, A->half, TB, B->half, margin, out, cap, 0);
        case SHAPE_PLANE:
            for (int c = 0; c < 8; c++) {
                double l[3] = {(c & 1 ? 1 : -1) * A->half[0], (c & 2 ? 1 : -1) * A->half[1], (c & 4 ? 1 : -1) * A->half[2]}, v[3];
                mpose_apply(v, TA, l);
                n = sphere_plane(v, 0, zb, cb, margin, out, cap, n);
            }
            return n;
        default: return -1;
        }
    case SHAPE_PLANE:
        if (B->type == SHAPE_HULL) {
            for (int i = 0; i < B->nverts; i++) {
                double v[3];
                mpose_apply(v, TB, B->verts + 3 * i);
                n = sphere_plane(v, 0, za, ca, margin, out, cap, n);
            }
            for (int i = 0; i < n; i++) flip(&out[i]);
            return n;
        }
        return -1;
    default: return -1;
    }
}

/* a parent and a child of a constraining joint (a free joint constrains nothing) */
static bool jointed(const MbModel *m, int a, int b) {
    for (int j = 0; j < m->njoints; j++) {
        if (m->joints[j].type == MB_FREE) continue;
        if ((m->joints[j].parent == a && m->joints[j].child == b) || (m->joints[j].parent == b && m->joints[j].child == a)) return true;
    }
    return false;
}

static double shape_extent(const ContactShape *S) {
    switch (S->type) {
    case SHAPE_SPHERE: return S->radius;
    case SHAPE_CAPSULE: return S->radius + S->half_length;
    case SHAPE_BOX: return mv3_norm(S->half);
    case SHAPE_HULL: {
        double r = 0;
        for (int i = 0; i < S->nverts; i++) r = fmax(r, mv3_norm(S->verts + 3 * i));
        return r;
    }
    default: return 0;
    }
}

/* upper bound of the speed of any point of the shape: |v_origin + w x c| + |w| extent */
static double shape_speed(const ContactShape *S, const MPose *TS, const double *twist) {
    if (S->body < 0 || !twist) return 0;
    const double *V = twist + 6 * S->body;
    double wc[3], vc[3];
    mv3_cross(wc, V, TS->p);
    mv3_add(vc, V + 3, wc);
    return mv3_norm(vc) + mv3_norm(V) * shape_extent(S);
}

int contact_detect(const MbModel *m, const ContactShape *shapes, int nshapes, const MPose *body_pose, const double *twist, double h, const ContactOptions *opt,
                   MbContactRow *rows, int *shape_a, int *shape_b, int cap, ContactReport *rep) {
    MPose *T = malloc((size_t)(nshapes ? nshapes : 1) * sizeof *T);
    if (!T) return 0;
    for (int i = 0; i < nshapes; i++) {
        if (shapes[i].body >= 0)
            mpose_mul(&T[i], &body_pose[shapes[i].body], &shapes[i].pose);
        else
            T[i] = shapes[i].pose;
    }
    int n = 0;
    for (int i = 0; i < nshapes; i++)
        for (int j = i + 1; j < nshapes; j++) {
            const ContactShape *A = &shapes[i], *B = &shapes[j];
            if (A->body == B->body) continue;
            if (A->group && A->group == B->group) continue;
            if (A->body < 0 && B->body < 0) continue;
            if (opt->exclude_jointed && jointed(m, A->body, B->body)) continue; /* parent and child, including the world */
            double margin = opt->margin + h * (shape_speed(A, &T[i], twist) + shape_speed(B, &T[j], twist));
            if (A->type != SHAPE_PLANE && B->type != SHAPE_PLANE) { /* bounding spheres */
                double dd[3];
                mv3_sub(dd, T[i].p, T[j].p);
                if (mv3_norm(dd) > shape_extent(A) + shape_extent(B) + margin) continue;
            }
            int added = contact_pair(A, &T[i], B, &T[j], margin, rows + n, cap - n);
            if (added < 0) {
                if (rep) {
                    if (!rep->unsupported_pairs)
                        snprintf(rep->unsupported, sizeof rep->unsupported, "%s (%s) with %s (%s)", A->name, shape_type_name(A->type), B->name, shape_type_name(B->type));
                    rep->unsupported_pairs++;
                }
                continue;
            }
            for (int k = n; k < n + added; k++) {
                rows[k].body_a = A->body, rows[k].body_b = B->body;
                rows[k].friction = fmin(A->friction, B->friction);
                rows[k].restitution = fmax(A->restitution, B->restitution);
                if (shape_a) shape_a[k] = i;
                if (shape_b) shape_b[k] = j;
            }
            n += added;
        }
    free(T);
    if (rep) rep->points = n;
    return n;
}

bool contact_warm_init(ContactWarm *w, int cap) {
    memset(w, 0, sizeof *w);
    size_t c = (size_t)(cap > 0 ? cap : 1);
    w->sa = calloc(c, sizeof *w->sa), w->sb = calloc(c, sizeof *w->sb);
    w->point = calloc(3 * c, sizeof *w->point), w->force = calloc(4 * c, sizeof *w->force);
    if (!w->sa || !w->sb || !w->point || !w->force) {
        contact_warm_free(w);
        return false;
    }
    w->cap = cap > 0 ? cap : 0;
    return true;
}

void contact_warm_free(ContactWarm *w) {
    free(w->sa), free(w->sb), free(w->point), free(w->force);
    memset(w, 0, sizeof *w);
}

static int max_manifold(const ContactShape *A, const ContactShape *B) {
    ShapeType a = A->type < B->type ? A->type : B->type, b = A->type < B->type ? B->type : A->type;
    if (a == SHAPE_SPHERE) return 1;
    if (a == SHAPE_CAPSULE) return b == SHAPE_PLANE ? 2 : 3;
    if (a == SHAPE_BOX) return 8; /* clipped face polygon, or the eight corners against a plane */
    if (a == SHAPE_PLANE && b == SHAPE_HULL) return A->type == SHAPE_HULL ? A->nverts : B->nverts;
    return 0;
}

int contact_warm_capacity(const MbModel *m, const ContactShape *shapes, int nshapes, const ContactOptions *opt) {
    long total = 0;
    for (int i = 0; i < nshapes; i++)
        for (int j = i + 1; j < nshapes; j++) {
            const ContactShape *A = &shapes[i], *B = &shapes[j];
            if (A->body == B->body || (A->group && A->group == B->group) || (A->body < 0 && B->body < 0)) continue;
            if ((!opt || opt->exclude_jointed) && jointed(m, A->body, B->body)) continue;
            total += max_manifold(A, B);
        }
    return total > CT_MAX_POINTS ? CT_MAX_POINTS : (int)total;
}

bool contact_step(MbSim *sim, MbState *s, double h, const MbInputs *in, const ContactShape *shapes, int nshapes, const ContactOptions *opt, MbContactRow *rows,
                  int *shape_a, int *shape_b, int *nrows, ContactReport *rep, MbStepReport *step, ContactWarm *warm) {
    const MbModel *m = mb_sim_model(sim);
    ContactOptions local;
    if (!opt) contact_options_default(&local), opt = &local;
    ContactReport lrep;
    if (!rep) rep = &lrep;
    memset(rep, 0, sizeof *rep);
    MPose *poses = malloc((size_t)(m->nbodies ? m->nbodies : 1) * sizeof *poses);
    double *twist = malloc((size_t)(6 * (m->nbodies ? m->nbodies : 1)) * sizeof(double));
    if (!poses || !twist) {
        free(poses), free(twist);
        return false;
    }
    mb_body_poses(sim, s, poses, twist);
    int n = contact_detect(m, shapes, nshapes, poses, twist, h, opt, rows, shape_a, shape_b, CT_MAX_POINTS, rep);
    if (warm && (!shape_a || !shape_b)) warm = NULL;
    MbContactSolve so = {opt->max_iterations, opt->tolerance, opt->restitution_speed, warm != NULL};
    if (warm) { /* greedy nearest-point matching within the margin, each stored contact used once */
        unsigned char *used = calloc((size_t)(warm->n ? warm->n : 1), 1);
        double tol2 = opt->margin * opt->margin;
        for (int i = 0; i < n; i++) {
            memset(rows[i].warm, 0, sizeof rows[i].warm);
            int best = -1;
            double bd = tol2;
            for (int k = 0; used && k < warm->n; k++) {
                if (used[k] || warm->sa[k] != shape_a[i] || warm->sb[k] != shape_b[i]) continue;
                double d[3];
                mv3_sub(d, warm->point + 3 * k, rows[i].point_a);
                double d2 = mv3_dot(d, d);
                if (d2 <= bd) bd = d2, best = k;
            }
            if (best < 0) continue;
            used[best] = 1;
            for (int k = 0; k < 4; k++) rows[i].warm[k] = warm->force[4 * best + k] * h;
        }
        free(used);
    }
    bool ok = mb_step_contacts(sim, s, h, in, rows, n, &so, &rep->solve, step);
    if (ok && warm) {
        warm->n = 0;
        for (int i = 0; i < n && warm->n < warm->cap; i++) {
            const MbContactRow *C = &rows[i];
            if (!(C->impulse_normal > 0) || C->impact) continue;
            int k = warm->n++;
            warm->sa[k] = shape_a[i], warm->sb[k] = shape_b[i];
            mv3_copy(warm->point + 3 * k, C->point_a);
            warm->force[4 * k] = C->impulse_normal / h;
            for (int c = 0; c < 3; c++) warm->force[4 * k + 1 + c] = (C->impulse_tangent[0] * C->tangent[0][c] + C->impulse_tangent[1] * C->tangent[1][c]) / h;
        }
    }
    if (ok) { /* penetration at the new positions */
        mb_body_poses(sim, s, poses, NULL);
        MbContactRow *after = malloc(CT_MAX_POINTS * sizeof *after);
        if (after) {
            ContactOptions o2 = *opt;
            o2.margin = 0;
            int na = contact_detect(m, shapes, nshapes, poses, NULL, 0, &o2, after, NULL, NULL, CT_MAX_POINTS, NULL);
            for (int i = 0; i < na; i++) rep->max_penetration = fmax(rep->max_penetration, -after[i].gap);
            free(after);
        }
    }
    free(poses), free(twist);
    if (nrows) *nrows = n;
    return ok;
}

int asm_contact_shapes(const Assembly *a, const MbModelDef *def, ContactShape *out, int cap, MechDiag *d) {
    int n = 0, errors = 0;
    for (int pass = 0; pass < 2; pass++) {
        int count = pass ? a->nenvironment : a->nbodies;
        for (int i = 0; i < count; i++) {
            int ng = pass ? 1 : a->bodies[i].ngeoms;
            for (int g = 0; g < ng; g++) {
                const AsmGeom *G = pass ? &a->environment[i] : &a->bodies[i].geoms[g];
                if (!pass && strcmp(G->role, "collision")) continue;
                char name[MB_NAME];
                if (G->name[0])
                    snprintf(name, sizeof name, "%s", G->name);
                else if (pass)
                    snprintf(name, sizeof name, "environment%d", i);
                else
                    snprintf(name, sizeof name, "%.40s.collision%d", a->bodies[i].name, g);
                int body = -1;
                if (!pass) {
                    body = mbdef_body_index(def, a->bodies[i].name);
                    if (body < 0) {
                        mdiag_add(d, MD_ERROR, "CONTACT_ON_MERGED_LINK", name, "give the link mass properties so it stays a body, or put the shape on its parent",
                                  "collision shape '%s' belongs to a massless link that became a frame", name);
                        errors++;
                        continue;
                    }
                }
                if (n >= cap) {
                    mdiag_add(d, MD_ERROR, "RESOURCE_LIMIT", name, NULL, "more than %d contact shapes", cap);
                    return -1;
                }
                bool dup = false;
                for (int k = 0; k < n; k++) dup |= !strcmp(out[k].name, name);
                if (dup) {
                    mdiag_add(d, MD_ERROR, "DUPLICATE_CONTACT_SHAPE", name, "give every collision and environment shape a unique name",
                              "contact shape name '%s' is used twice: contact results and load attachments refer to shapes by name", name);
                    errors++;
                    continue;
                }
                ContactShape *S = &out[n];
                memset(S, 0, sizeof *S);
                snprintf(S->name, sizeof S->name, "%s", name);
                S->body = body;
                S->pose = G->pose;
                S->group = G->group;
                switch (G->type) {
                case AG_SPHERE: S->type = SHAPE_SPHERE, S->radius = G->size[0]; break;
                case AG_CAPSULE: S->type = SHAPE_CAPSULE, S->radius = G->size[0], S->half_length = 0.5 * G->size[1]; break;
                case AG_BOX:
                    S->type = SHAPE_BOX;
                    for (int k = 0; k < 3; k++) S->half[k] = 0.5 * G->size[k];
                    break;
                case AG_PLANE: S->type = SHAPE_PLANE; break;
                default:
                    mdiag_add(d, MD_ERROR, "UNSUPPORTED_CONTACT_GEOMETRY", name,
                              "represent the part with box, sphere and capsule pieces (a concave part needs several pieces); nothing is approximated silently",
                              "%s collision geometry cannot take part in contact in this build", asm_geom_type_name(G->type));
                    errors++;
                    continue;
                }
                if (!G->has_friction) {
                    mdiag_add(d, MD_MISSING_INPUT, "CONTACT_FRICTION_REQUIRED", name,
                              "give friction with friction_source (measured on the real surface pair, or an explicit assumption); it decides stick and slip",
                              "contact shape '%s' has no friction coefficient", name);
                    errors++;
                    continue;
                }
                S->friction = G->friction;
                S->restitution = G->has_restitution ? G->restitution : 0;
                if (!G->has_restitution)
                    mdiag_add(d, MD_INFO, "RESTITUTION_ASSUMED", name, NULL, "restitution of '%s' not given: 0 (impacts are fully inelastic)", name);
                if (G->contact_source == SRC_DEFAULT || G->contact_source == SRC_INFERRED)
                    mdiag_add(d, MD_WARNING, "CONTACT_PARAMETER_ASSUMED", name, "measure the coefficient on the real surface pair before relying on slip margins",
                              "friction %.3g of '%s' is %s, not measured", G->friction, name, asm_source_name(G->contact_source));
                n++;
            }
        }
    }
    return errors ? -1 : n;
}
