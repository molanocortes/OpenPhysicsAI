/* mgearpart.c - see mgearpart.h */
#include "mgearpart.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void rot(double a, double x, double y, double *ox, double *oy) {
    double c = cos(a), s = sin(a);
    *ox = c * x - s * y, *oy = s * x + c * y;
}

/* the outline point at radius r with its outward normal (interpolated from the generated points'); the radius grows
 * along an external gear's side and falls along an internal gear's, either is found */
static bool outline_at_radius(const GearGeometry *g, double r, double *x, double *y, double *nx, double *ny) {
    const GearFlank *s = &g->right;
    /* the radius runs monotonically from the space middle to the tip corner (up an external gear's side, down an
     * internal gear's): the segment holding r is found by bisection */
    double sg = hypot(s->p[s->n - 1].x, s->p[s->n - 1].y) >= hypot(s->p[0].x, s->p[0].y) ? 1 : -1;
    int lo = 0, hi = s->n - 1;
    if (sg * (r - hypot(s->p[lo].x, s->p[lo].y)) < 0 || sg * (r - hypot(s->p[hi].x, s->p[hi].y)) > 0) return false;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (sg * (hypot(s->p[mid].x, s->p[mid].y) - r) < 0) lo = mid;
        else hi = mid;
    }
    for (int i = hi; i < s->n; i++) {
        double r0 = hypot(s->p[i - 1].x, s->p[i - 1].y), r1 = hypot(s->p[i].x, s->p[i].y);
        if ((r0 <= r && r <= r1) || (r1 <= r && r <= r0)) {
            double w = r1 != r0 ? (r - r0) / (r1 - r0) : 0;
            *x = s->p[i - 1].x + w * (s->p[i].x - s->p[i - 1].x), *y = s->p[i - 1].y + w * (s->p[i].y - s->p[i - 1].y);
            double ux = s->p[i - 1].nx + w * (s->p[i].nx - s->p[i - 1].nx), uy = s->p[i - 1].ny + w * (s->p[i].ny - s->p[i - 1].ny), L = hypot(ux, uy);
            *nx = ux / L, *ny = uy / L;
            return true;
        }
    }
    return false;
}

/* the squared distance from (px, py) to the step from point i to point i + 1 of a side, and where on it (0 to 1) */
static double step_distance(const GearFlank *s, int i, double px, double py, double *tp) {
    double ax = s->p[i].x, ay = s->p[i].y, ex = s->p[i + 1].x - ax, ey = s->p[i + 1].y - ay;
    double L2 = ex * ex + ey * ey, t = L2 > 0 ? ((px - ax) * ex + (py - ay) * ey) / L2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    double dx = px - (ax + t * ex), dy = py - (ay + t * ey);
    *tp = t;
    return dx * dx + dy * dy;
}

/* whether every point of the step from point i to point i + 1 (its ends' radii R[i] and R[i + 1], half its length at
 * most h) lies further than sqrt(best) from a point at radius rp, with room for rounding */
static bool step_beyond(const double *R, int i, double h, double rp, double best) {
    double lb = fmax(fmin(R[i], R[i + 1]) - h - rp, rp - fmax(R[i], R[i + 1]));
    return lb > sqrt(best) * (1 + 1e-6) + 1e-14;
}

/* the nearest point of the active flank (from the form point to the tip corner) to (px, py) in the tooth's frame, on
 * the first of its steps that comes nearest. The radii run one way along the active flank (m->run), so the search
 * starts at the step where the radius passes |p| and widens only while a step could come as near as the nearest
 * found: no point of a step lies further than half its length inside its ends' smaller radius, nor outside their
 * larger one, and a point at radius r is at least | r - |p| | from p. The same step and point as a search of them all,
 * in a few of them. */
static double outline_nearest(const GearModel *m, double px, double py, double *qx, double *qy, double *nx, double *ny, bool *inside) {
    const GearFlank *s = &m->geo.right;
    int i0 = s->form, last = s->n - 2, bi = i0;
    double best = INFINITY, bt = 0, t;
    if (m->run == 0) {
        for (int i = i0; i <= last; i++) {
            double d2 = step_distance(s, i, px, py, &t);
            if (d2 < best) best = d2, bi = i, bt = t;
        }
    } else {
        const double *R = m->orad;
        double rp = hypot(px, py), h = m->step_half;
        int lo = i0, hi = s->n - 1; /* the last point at or before |p| along the run, by bisection */
        if (m->run * (R[lo] - rp) > 0) hi = lo;
        else if (m->run * (R[hi] - rp) <= 0) lo = hi;
        while (hi - lo > 1) {
            int mid = (lo + hi) / 2;
            if (m->run * (R[mid] - rp) <= 0) lo = mid;
            else hi = mid;
        }
        int k = lo > last ? last : lo;
        best = step_distance(s, k, px, py, &bt), bi = k;
        for (int i = k - 1; i >= i0 && !step_beyond(R, i, h, rp, best); i--) { /* an equal one nearer the form point wins, as in a full search */
            double d2 = step_distance(s, i, px, py, &t);
            if (d2 <= best) best = d2, bi = i, bt = t;
        }
        for (int i = k + 1; i <= last && !step_beyond(R, i, h, rp, best); i++) {
            double d2 = step_distance(s, i, px, py, &t);
            if (d2 < best) best = d2, bi = i, bt = t;
        }
    }
    const GearPoint *a = &s->p[bi], *b = &s->p[bi + 1];
    *qx = a->x + bt * (b->x - a->x), *qy = a->y + bt * (b->y - a->y);
    double ux = a->nx + bt * (b->nx - a->nx), uy = a->ny + bt * (b->ny - a->ny), L = hypot(ux, uy);
    *nx = ux / L, *ny = uy / L;
    *inside = !((bi == i0 && bt <= 0) || (bi == last && bt >= 1));
    return (px - *qx) * *nx + (py - *qy) * *ny;
}

static double side_arc(const GearModel *m, double r) {
    const GearFlank *s = &m->geo.right;
    for (int i = 1; i < s->n; i++) {
        double r0 = hypot(s->p[i - 1].x, s->p[i - 1].y), r1 = hypot(s->p[i].x, s->p[i].y);
        if (((r0 <= r && r <= r1) || (r1 <= r && r <= r0)) && r1 != r0) return m->ocum[i - 1] + (r - r0) / (r1 - r0) * (m->ocum[i] - m->ocum[i - 1]);
    }
    double rs = hypot(s->p[0].x, s->p[0].y), re = hypot(s->p[s->n - 1].x, s->p[s->n - 1].y);
    return fabs(r - rs) < fabs(r - re) ? 0 : m->ocum[s->n - 1];
}

static double side_radius(const GearModel *m, double arc) {
    const GearFlank *s = &m->geo.right;
    int i = 1;
    while (i < s->n - 1 && m->ocum[i] < arc) i++;
    double d = m->ocum[i] - m->ocum[i - 1], w = d > 0 ? (arc - m->ocum[i - 1]) / d : 0;
    w = w < 0 ? 0 : w > 1 ? 1 : w;
    return hypot(s->p[i - 1].x + w * (s->p[i].x - s->p[i - 1].x), s->p[i - 1].y + w * (s->p[i].y - s->p[i - 1].y));
}

static double pitch_of(const GearModel *m) { return 2 * M_PI / m->geo.spec.teeth; }

double gearpart_tooth_angle(const GearPart *p, int t) { return p->m->mesh.tooth_angle0 + t * pitch_of(p->m) + p->rot0; }

void gearpart_init(GearPart *p, const char *name, GearModel *m, double ax, double ay, double az, double tooth_angle) {
    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", name);
    p->m = m, p->ax = ax, p->ay = ay, p->az = az, p->cx = ax, p->cy = ay;
    p->rot0 = tooth_angle - (m->mesh.tooth_angle0 + m->mid * pitch_of(m));
    p->dof = p->dofx = p->dofy = p->cdof = -1;
}

bool gearpart_point(const GearPart *p, int side, int tooth, double phi, double zf, double r, double *wx, double *wy, double *nx, double *ny) {
    double x, y, ox, oy;
    if (!outline_at_radius(&p->m->geo, r, &x, &y, &ox, &oy)) return false;
    if (side == GM_LEFT) x = -x, ox = -ox; /* the left side is the right side mirrored */
    double ta = gearpart_tooth_angle(p, tooth) - M_PI / 2 + phi + p->m->geo.twist * zf, px, py;
    rot(ta, x, y, &px, &py);
    *wx = px + p->cx, *wy = py + p->cy;
    rot(ta, ox, oy, nx, ny);
    return true;
}

double gearpart_project(const GearPart *p, int side, int tooth, double phi, double zf, double wx, double wy, double *radius, double *nwx, double *nwy,
                        bool *inside) {
    const GearGeometry *g = &p->m->geo;
    double lx, ly, ta = gearpart_tooth_angle(p, tooth) - M_PI / 2 + phi + g->twist * zf;
    rot(-ta, wx - p->cx, wy - p->cy, &lx, &ly);
    if (side == GM_LEFT) lx = -lx;
    double qx, qy, nx, ny;
    double d = outline_nearest(p->m, lx, ly, &qx, &qy, &nx, &ny, inside);
    *radius = hypot(qx, qy);
    if (side == GM_LEFT) nx = -nx;
    rot(ta, nx, ny, nwx, nwy);
    return d;
}

void gearpart_world(const GearPart *p, double phi, int node, double out[3]) {
    const double *X = &p->m->mesh.xyz[3 * node];
    double x, y;
    rot(p->rot0 + phi, X[0], X[1], &x, &y);
    out[0] = x + p->cx, out[1] = y + p->cy, out[2] = X[2] + p->az;
}

static void flank_free(PartFlank *f) {
    free(f->node), free(f->r);
    memset(f, 0, sizeof *f);
}

void gearmodel_release(GearModel *m) {
    if (!m || --m->refs > 0) return;
    for (int s = 0; s < 2; s++) {
        if (m->fl[s])
            for (int i = 0; i < m->mesh.teeth * m->nlayer; i++) flank_free(&m->fl[s][i]);
        free(m->fl[s]);
    }
    mbody_free(&m->body);
    gear_mesh_free(&m->mesh);
    gear_free(&m->geo);
    free(m->ocum), free(m->orad);
    free(m);
}

/* the involute flank nodes of one tooth's side on one layer, sorted by radius */
static bool flank_collect(const GearMesh *m, int tooth, int side, int layer, int nz, double b, PartFlank *out) {
    memset(out, 0, sizeof *out);
    int n = 0;
    for (int i = 0; i < m->nnodes; i++)
        if (m->tooth[i] == tooth && (m->tag[i] & side) && (m->tag[i] & GM_FLANK) && lround(m->xyz[3 * i + 2] / b * nz) == layer) n++;
    out->node = malloc((size_t)(n ? n : 1) * sizeof(int));
    out->r = malloc((size_t)(n ? n : 1) * sizeof(double));
    if (!out->node || !out->r) return false;
    for (int i = 0; i < m->nnodes; i++) {
        if (!(m->tooth[i] == tooth && (m->tag[i] & side) && (m->tag[i] & GM_FLANK) && lround(m->xyz[3 * i + 2] / b * nz) == layer)) continue;
        int k = out->n;
        while (k > 0 && out->r[k - 1] > m->profile[i]) out->node[k] = out->node[k - 1], out->r[k] = out->r[k - 1], k--;
        out->node[k] = i, out->r[k] = m->profile[i];
        out->n++;
    }
    return true;
}

/* the bracketing pair of a radius along a flank: index k with r[k] <= r <= r[k+1], and the weight of k+1 */
static int flank_bracket(const PartFlank *f, double r, double *w) {
    if (f->n < 2 || r < f->r[0] - 1e-12 || r > f->r[f->n - 1] + 1e-12) return -1;
    int k = 0;
    while (k < f->n - 2 && f->r[k + 1] < r) k++;
    double d = f->r[k + 1] - f->r[k];
    *w = d > 0 ? (r - f->r[k]) / d : 0;
    *w = *w < 0 ? 0 : *w > 1 ? 1 : *w;
    return k;
}

/* the helicoid's outward normal at a point (x, y) of a section (relative to the axis) whose transverse outward normal
 * is (nx, ny): the surface is the section turning by `twist` per unit length along the axis, so its normal is
 * (n_t, -twist n_t . (-y, x)) normalised. On an involute |n_t . (-y, x)| = r_b, and the normal leans out of the
 * transverse plane by the base helix angle. */
static void helicoid_normal(double twist, double x, double y, double nx, double ny, double n3[3]) {
    double c = -twist * (nx * -y + ny * x), L = sqrt(1 + c * c);
    n3[0] = nx / L, n3[1] = ny / L, n3[2] = c / L;
}

GearModel *gearmodel_create(const GearSpec *spec, const GearSpec *cutter, const GearMeshSpec *ms, double E, double nu, bool hold_cuts,
                            const GearHold *hold, char *err, size_t errlen) {
    GearModel *m = calloc(1, sizeof *m);
    if (!m) return snprintf(err, errlen, "out of memory"), NULL;
    m->refs = 1, m->E = E, m->nu = nu;
    bool ok = cutter ? gear_build_internal(spec, cutter, 600, &m->geo, err, errlen) : gear_build(spec, 600, &m->geo, err, errlen);
    if (!ok) {
        free(m);
        return NULL;
    }
    int z = spec->teeth, T = ms->teeth_modelled;
    if (T != z && T % 2 == 0) {
        gear_free(&m->geo), free(m);
        return snprintf(err, errlen, "model an odd number of teeth, or the whole gear"), NULL;
    }
    m->mid = T == z ? 0 : (T - 1) / 2;
    double a0 = M_PI / 2 - m->mid * 2 * M_PI / z;
    ok = cutter ? gear_mesh_build_internal(&m->geo, ms, a0, &m->mesh, err, errlen) : gear_mesh_build(&m->geo, ms, a0, &m->mesh, err, errlen);
    if (!ok) {
        gear_free(&m->geo), free(m);
        return NULL;
    }
    m->nlayer = ms->n_face + 1;
    m->held_all_round = !hold || hold->lugs <= 0;
    const GearFlank *of = &m->geo.right;
    m->ocum = malloc((size_t)of->n * sizeof(double)), m->orad = malloc((size_t)of->n * sizeof(double));
    unsigned char *held = calloc((size_t)m->mesh.nnodes, 1);
    int *sn = NULL, ns = 0;
    double *sk = NULL;
    for (int s = 0; s < 2; s++) m->fl[s] = calloc((size_t)m->mesh.teeth * m->nlayer, sizeof(PartFlank));
    if (!m->ocum || !m->orad || !held || !m->fl[0] || !m->fl[1]) {
        free(held), gearmodel_release(m);
        return snprintf(err, errlen, "out of memory"), NULL;
    }
    m->ocum[0] = 0;
    for (int i = 1; i < of->n; i++) m->ocum[i] = m->ocum[i - 1] + hypot(of->p[i].x - of->p[i - 1].x, of->p[i].y - of->p[i - 1].y);
    for (int i = 0; i < of->n; i++) m->orad[i] = hypot(of->p[i].x, of->p[i].y);
    bool up = true, down = true;
    for (int i = of->form; i + 1 < of->n; i++) {
        up &= m->orad[i + 1] >= m->orad[i], down &= m->orad[i + 1] <= m->orad[i];
        m->step_half = fmax(m->step_half, 0.5 * (m->ocum[i + 1] - m->ocum[i]));
    }
    m->run = of->n - of->form < 2 ? 0 : up ? 1 : down ? -1 : 0;
    if (m->held_all_round) {
        for (int i = 0; i < m->mesh.nnodes; i++) held[i] = (m->mesh.tag[i] & GM_BORE) || (hold_cuts && (m->mesh.tag[i] & GM_CUT));
    } else { /* lugs: the bore's nodes within a lug's width of its centre */
        sn = malloc((size_t)m->mesh.nnodes * sizeof(int)), sk = malloc((size_t)m->mesh.nnodes * 9 * sizeof(double));
        if (!sn || !sk) {
            free(held), free(sn), free(sk), gearmodel_release(m);
            return snprintf(err, errlen, "out of memory"), NULL;
        }
        double pitch = 2 * M_PI / hold->lugs, half = 0.5 * hold->lug_deg * M_PI / 180, ph = hold->phase_deg * M_PI / 180;
        double kpen = 1e3 * E * spec->module_m; /* far stiffer than the rim around it: holds the direction */
        for (int i = 0; i < m->mesh.nnodes; i++) {
            held[i] = hold_cuts && (m->mesh.tag[i] & GM_CUT);
            if (!(m->mesh.tag[i] & GM_BORE)) continue;
            double x = m->mesh.xyz[3 * i], y = m->mesh.xyz[3 * i + 1], a = atan2(y, x);
            if (fabs(remainder(a - ph, pitch)) > half) continue;
            if (!hold->radial_free) {
                held[i] = 1;
                continue;
            }
            double tx = -sin(a), ty = cos(a), *K = &sk[9 * ns]; /* k (t t' + z z'): along the circumference and the axis */
            K[0] = kpen * tx * tx, K[1] = kpen * tx * ty, K[2] = 0;
            K[3] = kpen * ty * tx, K[4] = kpen * ty * ty, K[5] = 0;
            K[6] = 0, K[7] = 0, K[8] = kpen;
            sn[ns++] = i;
        }
    }
    ok = mbody_create_springs(&m->body, m->mesh.nnodes, m->mesh.xyz, m->mesh.nelems, m->mesh.conn, E, nu, held, ns, sn, sk, err, errlen);
    free(held), free(sn), free(sk);
    if (!ok) {
        gearmodel_release(m);
        return NULL;
    }
    double b = spec->face_width_m;
    int NL = m->nlayer;
    for (int s = 0; s < 2; s++)
        for (int t = 0; t < m->mesh.teeth; t++)
            for (int l = 0; l < NL; l++)
                if (!flank_collect(&m->mesh, t, s ? GM_RIGHT : GM_LEFT, l, NL - 1, b, &m->fl[s][t * NL + l])) {
                    gearmodel_release(m);
                    return snprintf(err, errlen, "out of memory"), NULL;
                }
    return m;
}

bool gearmodel_set_zone(GearModel *m, int zone_sides, const unsigned char *teeth, char *err, size_t errlen) {
    /* the zone: the involute flank nodes of the sides that can touch, tooth by tooth (side, layer, radius), directed
     * into the part along the helicoid's normal; a whole gear's teeth are alike, so its compliance is cyclic */
    int NL = m->nlayer, *zn = malloc((size_t)m->mesh.nnodes * sizeof(int)), nzn = 0, nper = -1;
    double *zd = malloc((size_t)m->mesh.nnodes * 3 * sizeof(double));
    if (!zn || !zd) {
        free(zn), free(zd);
        return snprintf(err, errlen, "out of memory"), false;
    }
    bool cyclic = m->mesh.teeth == m->geo.spec.teeth && m->held_all_round && !teeth;
    for (int t = 0; t < m->mesh.teeth; t++) {
        if (teeth && !teeth[t]) continue;
        int n0 = nzn;
        for (int s = 0; s < 2; s++) {
            if (!(zone_sides & (s ? GM_RIGHT : GM_LEFT))) continue;
            for (int l = 0; l < NL; l++) {
                const PartFlank *F = &m->fl[s][t * NL + l];
                for (int k = 0; k < F->n; k++) {
                    int i = F->node[k];
                    double n3[3];
                    helicoid_normal(m->geo.twist, m->mesh.xyz[3 * i], m->mesh.xyz[3 * i + 1], m->mesh.normal[2 * i], m->mesh.normal[2 * i + 1], n3);
                    zn[nzn] = i, zd[3 * nzn] = -n3[0], zd[3 * nzn + 1] = -n3[1], zd[3 * nzn + 2] = -n3[2];
                    nzn++;
                }
            }
        }
        if (nper < 0) nper = nzn - n0;
        else if (nzn - n0 != nper) cyclic = false;
    }
    bool ok = cyclic && getenv("OPAI_MECH_FULL_ZONE") == NULL ? mbody_zone_cyclic(&m->body, m->mesh.teeth, nper, zn, zd, err, errlen)
                                                              : mbody_zone(&m->body, nzn, zn, zd, err, errlen);
    free(zn), free(zd);
    return ok;
}

GearModel *gearmodel_build(const GearSpec *spec, const GearSpec *cutter, const GearMeshSpec *ms, double E, double nu, bool hold_cuts, int zone_sides,
                           char *err, size_t errlen) {
    GearModel *m = gearmodel_create(spec, cutter, ms, E, nu, hold_cuts, NULL, err, errlen);
    if (m && !gearmodel_set_zone(m, zone_sides, NULL, err, errlen)) {
        gearmodel_release(m);
        return NULL;
    }
    return m;
}

/* ---------------------------------------------------------------- contact between two parts */

typedef struct Hit {
    double g, rA, rB, wx, wy, nx, ny; /* transverse gap; radii on A and B; the contact point on A (world); B's transverse
                                         outward normal there */
    bool ok;
} Hit;

typedef struct Pair {
    const GearPart *A, *B;
    int sA, sB, tA, tB;
    double phiA, phiB, zf;
} Pair;

/* dir 0: A's point at radius r against B's side; dir 1: B's point at radius r against A's side (its foot on A is the
 * contact point). A side seen through its own tooth is not facing: more than a twentieth of a module inside it. */
static double pair_gap(const Pair *P, int dir, double r, Hit *h) {
    double wx, wy, nx, ny, rr, mx, my, deep = -0.05 * P->B->m->geo.spec.module_m;
    bool in;
    if (dir == 0) {
        if (!gearpart_point(P->A, P->sA, P->tA, P->phiA, P->zf, r, &wx, &wy, &nx, &ny)) return INFINITY;
        double g = gearpart_project(P->B, P->sB, P->tB, P->phiB, P->zf, wx, wy, &rr, &mx, &my, &in);
        if (!in || g < deep) return INFINITY;
        if (h) *h = (Hit){g, r, rr, wx, wy, mx, my, true};
        return g;
    }
    if (!gearpart_point(P->B, P->sB, P->tB, P->phiB, P->zf, r, &wx, &wy, &nx, &ny)) return INFINITY;
    double g = gearpart_project(P->A, P->sA, P->tA, P->phiA, P->zf, wx, wy, &rr, &mx, &my, &in);
    if (!in || g < deep) return INFINITY;
    if (h) *h = (Hit){g, rr, r, wx - g * mx, wy - g * my, -mx, -my, true};
    return g;
}

static Hit pair_contact(const Pair *P) {
    Hit best;
    memset(&best, 0, sizeof best);
    best.g = INFINITY;
    for (int dir = 0; dir < 2; dir++) {
        const GearGeometry *g = dir ? &P->B->m->geo : &P->A->m->geo;
        double r0 = fmin(g->right.r_form, g->r_a), r1 = fmax(g->right.r_form, g->r_a), bg = INFINITY;
        enum { N = 72 };
        int bi = -1;
        for (int i = 0; i <= N; i++) {
            double v = pair_gap(P, dir, r0 + (r1 - r0) * i / N, NULL);
            if (v < bg) bg = v, bi = i;
        }
        if (bi < 0) continue;
        double a = r0 + (r1 - r0) * (bi > 0 ? bi - 1 : 0) / N, b = r0 + (r1 - r0) * (bi < N ? bi + 1 : N) / N;
        const double ph = 0.6180339887498949;
        double c = b - ph * (b - a), d = a + ph * (b - a), gc = pair_gap(P, dir, c, NULL), gd = pair_gap(P, dir, d, NULL);
        for (int it = 0; it < 44; it++) { /* the bracket shrinks to 1e-9 of its size */
            if (gc < gd) b = d, d = c, gd = gc, c = b - ph * (b - a), gc = pair_gap(P, dir, c, NULL);
            else a = c, c = d, gc = gd, d = a + ph * (b - a), gd = pair_gap(P, dir, d, NULL);
        }
        double ropt = r0 + (r1 - r0) * bi / N, rm = 0.5 * (a + b), gm = pair_gap(P, dir, rm, NULL);
        if (gm < bg) ropt = rm;
        Hit h;
        memset(&h, 0, sizeof h);
        if (pair_gap(P, dir, ropt, &h) < best.g && h.ok) best = h;
    }
    return best;
}

/* the polar angle (about part X's axis) of the teeth of X that face part Y */
static double facing_angle(const GearPart *X, const GearPart *Y) {
    double dx = Y->cx - X->cx, dy = Y->cy - X->cy;
    if (!X->m->geo.internal && Y->m->geo.internal) return atan2(-dy, -dx); /* X inside Y: its teeth pointing away from Y's axis */
    return atan2(dy, dx);
}

static bool nf_reserve(NearField *nf, int n) {
    if (n <= nf->cap) return true;
    int cap = nf->cap ? 2 * nf->cap : 256;
    while (cap < n) cap *= 2;
    double *R = realloc(nf->R, (size_t)cap * sizeof(double));
    if (!R) return false;
    nf->R = R;
    double *a = realloc(nf->a, (size_t)cap * sizeof(double));
    if (!a) return false;
    nf->a = a;
    double *w = realloc(nf->w, (size_t)cap * sizeof(double));
    if (!w) return false;
    nf->w = w, nf->cap = cap;
    return true;
}

void nearfield_free(NearField *nf) {
    free(nf->R), free(nf->a), free(nf->w);
    memset(nf, 0, sizeof *nf);
}

void gearpart_near_field(MContactSet *cs, const NearField *nf, const double *lambda, double q_default, double Estar) {
    for (int j = 0; j < cs->n; j++) {
        double q = lambda && lambda[j] > 0 ? lambda[j] / nf->w[j] : q_default;
        double b = sqrt(4 * q * nf->R[j] / (M_PI * Estar));
        cs->cdiag[j] = nf->a[j] > b ? 2 / (M_PI * Estar * nf->w[j]) * log(nf->a[j] / b) : 0;
    }
}

/* the change of the normal gap at a world point (x, y) of part P with each of its rigid motions (n . dP), added to a
 * row of M with sign s (+1 for the gap's part A, -1 for B): its rotation about its axis, its translation, and the
 * rotation of the carrier that carries its axis round (the axis moves; the part's own rotation is absolute) */
static void motion_row(const GearPart *P, double x, double y, const double n3[3], double s, double *mrow) {
    if (P->dof >= 0) mrow[P->dof] += s * (n3[0] * -(y - P->cy) + n3[1] * (x - P->cx));
    if (P->dofx >= 0) mrow[P->dofx] += s * n3[0];
    if (P->dofy >= 0) mrow[P->dofy] += s * n3[1];
    if (P->cdof >= 0) mrow[P->cdof] += s * (n3[0] * -(P->cy - P->ky) + n3[1] * (P->cx - P->kx));
}

int gearpart_mesh_contacts(MContactSet *cs, NearField *nf, const GearPart *A, int bodyA, int sideA, double phiA, const GearPart *B, int bodyB, int sideB,
                           double phiB, double q_est, double gap_cut, char *err, size_t errlen) {
    const GearModel *MA = A->m, *MB = B->m;
    double bw = MA->geo.spec.face_width_m;
    int NL = MA->nlayer;
    if (fabs(MB->geo.spec.face_width_m - bw) > 1e-12 || MB->nlayer != NL || fabs(A->az - B->az) > 1e-9)
        return snprintf(err, errlen, "%s and %s: gears in mesh need the same face (width, position along the axis, layers)", A->name, B->name), -1;
    if (NL > 64) return snprintf(err, errlen, "%s: at most 63 elements through the face", A->name), -1;
    int zA = MA->geo.spec.teeth, zB = MB->geo.spec.teeth, sA = sideA == GM_LEFT ? 0 : 1, sB = sideB == GM_LEFT ? 0 : 1;
    bool helical = MA->geo.twist != 0 || MB->geo.twist != 0;
    double fA = facing_angle(A, B), fB = facing_angle(B, A), zl[64];
    for (int l = 0; l < NL; l++) zl[l] = bw * l / (NL - 1);
    /* the tooth pairs that may touch: on every layer of a helical pair (its sections turn along the face), once for a
     * spur pair (the same on all) */
    typedef struct {
        Hit h;
        int tA, tB, layer;
    } LHit;
    int maxhit = 64 * (helical ? NL : 1), nh = 0;
    LHit *hit = malloc((size_t)maxhit * sizeof(LHit));
    if (!hit) return snprintf(err, errlen, "out of memory"), -1;
    double floor_ = INFINITY;
    /* a pair can touch only if its teeth are near each other along the pitch lines: their arcs from the line of centres
     * agree (opposite senses for two external gears, the same inside a ring) within two transverse pitches */
    double sgn = MB->geo.internal ? -1 : 1, near = 2 * 2 * M_PI * MA->geo.r / zA;
    for (int l = 0; l < (helical ? NL : 1); l++) {
        double zf = helical ? zl[l] : 0;
        for (int tA = 0; tA < MA->mesh.teeth; tA++) {
            double aA = remainder(gearpart_tooth_angle(A, tA) + phiA + MA->geo.twist * zf - fA, 2 * M_PI);
            if (fabs(aA) > 2.5 * 2 * M_PI / zA) continue;
            for (int tB = 0; tB < MB->mesh.teeth; tB++) {
                double aB = remainder(gearpart_tooth_angle(B, tB) + phiB + MB->geo.twist * zf - fB, 2 * M_PI);
                if (fabs(aB) > 2.5 * 2 * M_PI / zB || fabs(MA->geo.r * aA + sgn * MB->geo.r * aB) > near) continue;
                Pair P = {A, B, sideA, sideB, tA, tB, phiA, phiB, zf};
                Hit h = pair_contact(&P);
                if (!h.ok || nh == maxhit) continue;
                hit[nh].h = h, hit[nh].tA = tA, hit[nh].tB = tB, hit[nh].layer = l, nh++;
                floor_ = fmin(floor_, h.g);
            }
        }
    }
    if (!(floor_ < INFINITY)) {
        free(hit);
        return snprintf(err, errlen, "%s and %s do not face each other", A->name, B->name), -1;
    }
    double Estar = 1 / ((1 - MA->nu * MA->nu) / MA->E + (1 - MB->nu * MB->nu) / MB->E);
    double cbb = cos(fmax(MA->geo.beta_b, MB->geo.beta_b)), crown = MA->geo.spec.lead_crown_m + MB->geo.spec.lead_crown_m;
    int added = 0;
    enum { NKS = 9, NQZ = 4 };
    for (int h = 0; h < nh; h++) {
        if (hit[h].h.g > floor_ + gap_cut) continue;
        const Hit *H = &hit[h].h;
        int tA = hit[h].tA, tB = hit[h].tB, lh = helical ? hit[h].layer : 0;
        double zf = helical ? zl[lh] : 0;
        /* the strip's half-width along the profile: Hertz for the two flanks' curvatures (a concave internal flank
         * subtracts; normal to a helical contact line the radius is the transverse one over cos(beta_b)), or three
         * quarters of the longer element under it */
        double rhoA = sqrt(fmax(0, H->rA * H->rA - MA->geo.r_b * MA->geo.r_b)) / cbb, rhoB = sqrt(fmax(0, H->rB * H->rB - MB->geo.r_b * MB->geo.r_b)) / cbb;
        double rmin = 0.05 * MA->geo.spec.module_m;
        rhoA = fmax(rhoA, rmin), rhoB = fmax(rhoB, rmin);
        double Rs = (MA->geo.internal != MB->geo.internal) ? rhoA * rhoB / fmax(fabs(rhoB - rhoA), rmin) : rhoA * rhoB / (rhoA + rhoB);
        double bH = sqrt(4 * q_est * Rs / (M_PI * Estar));
        const PartFlank *Fa = &MA->fl[sA][tA * NL + lh], *Fb = &MB->fl[sB][tB * NL + lh];
        double wa, wb;
        int ka = flank_bracket(Fa, H->rA, &wa), kb = flank_bracket(Fb, H->rB, &wb);
        if (ka < 0 || kb < 0) continue;
        double la = fabs(side_arc(MA, Fa->r[ka + 1]) - side_arc(MA, Fa->r[ka])), lb = fabs(side_arc(MB, Fb->r[kb + 1]) - side_arc(MB, Fb->r[kb]));
        double a_k = fmax(bH / cbb, 0.75 * fmax(la, lb));
        double arcA = side_arc(MA, H->rA), arc0 = side_arc(MA, MA->geo.right.r_form), arc1 = MA->ocum[MA->geo.right.n - 1];
        double ks_rA[NKS], ks_rB[NKS], ks_w[NKS];
        double rB0 = fmin(MB->geo.right.r_form, MB->geo.r_a), rB1 = fmax(MB->geo.right.r_form, MB->geo.r_a);
        int nks = 0;
        for (int k = 0; k < NKS; k++) {
            double sk = -1 + (2.0 * k + 1) / NKS, arc = arcA + sk * a_k, w = sqrt(fmax(0, 1 - sk * sk));
            if (arc < fmin(arc0, arc1) || arc > fmax(arc0, arc1)) continue; /* past A's flank: the strip is cut there */
            double rA = side_radius(MA, arc), wx, wy, nx, ny, rB, mx, my;
            bool in;
            if (!gearpart_point(A, sideA, tA, phiA, zf, rA, &wx, &wy, &nx, &ny)) continue;
            gearpart_project(B, sideB, tB, phiB, zf, wx, wy, &rB, &mx, &my, &in);
            rB = fmin(fmax(rB, rB0), rB1); /* past B's tip: its corner carries it */
            ks_rA[nks] = rA, ks_rB[nks] = rB, ks_w[nks] = w, nks++;
        }
        if (nks == 0) continue;
        /* the contact's normal: B's helicoid there (pointing from B toward A); the normal gap, and its change with the
         * parts' rigid motions */
        double n3[3];
        helicoid_normal(MB->geo.twist, H->wx - B->cx, H->wy - B->cy, H->nx, H->ny, n3);
        double gn = H->g * hypot(n3[0], n3[1]), mrow[MC_MAX_DOF] = {0};
        motion_row(A, H->wx, H->wy, n3, +1, mrow);
        motion_row(B, H->wx, H->wy, n3, -1, mrow);
        /* the layers this pair stands for: a spur pair's strip runs through the face, quadrature along it spread over
         * the layers on either side; a helical pair's is this layer's (its neighbours have their own) */
        for (int l = helical ? lh : 0; l <= (helical ? lh : NL - 1); l++) {
            double z_lo = l > 0 ? 0.5 * (zl[l - 1] + zl[l]) : zl[l], z_hi = l < NL - 1 ? 0.5 * (zl[l] + zl[l + 1]) : zl[l];
            int tb_[MC_MAX_TERMS], tz_[MC_MAX_TERMS], nt = 0;
            double tw_[MC_MAX_TERMS];
            bool overflow = false;
#define ADD(MODEL, BODY, NODE, W) do { \
        int zz_ = (MODEL)->body.zone_of[NODE]; \
        if (zz_ < 0) break; \
        int f_ = 0; \
        while (f_ < nt && !(tb_[f_] == (BODY) && tz_[f_] == zz_)) f_++; \
        if (f_ == nt) { if (nt == MC_MAX_TERMS) { overflow = true; break; } tb_[nt] = (BODY), tz_[nt] = zz_, tw_[nt] = 0, nt++; } \
        tw_[f_] += (W); \
    } while (0)
            for (int k = 0; k < nks; k++) {
                if (helical) {
                    double w1, w2;
                    const PartFlank *Fs = &MA->fl[sA][tA * NL + l], *Fm = &MB->fl[sB][tB * NL + l];
                    int ia = flank_bracket(Fs, ks_rA[k], &w1), ib = flank_bracket(Fm, ks_rB[k], &w2);
                    if (ia < 0 || ib < 0) continue;
                    ADD(MA, bodyA, Fs->node[ia], ks_w[k] * (1 - w1));
                    ADD(MA, bodyA, Fs->node[ia + 1], ks_w[k] * w1);
                    ADD(MB, bodyB, Fm->node[ib], ks_w[k] * (1 - w2));
                    ADD(MB, bodyB, Fm->node[ib + 1], ks_w[k] * w2);
                    continue;
                }
                for (int iz = 0; iz < NQZ; iz++) {
                    double zq = z_hi > z_lo ? z_lo + (z_hi - z_lo) * (iz + 0.5) / NQZ : z_lo, w = ks_w[k] * (z_hi > z_lo ? (z_hi - z_lo) / NQZ : 1.0 / NQZ);
                    double lz = zq / bw * (NL - 1);
                    int l0 = (int)floor(lz);
                    l0 = l0 > NL - 2 ? NL - 2 : l0 < 0 ? 0 : l0;
                    double wz = lz - l0;
                    for (int L = 0; L < 2; L++) {
                        int lay = l0 + L;
                        double wl = L ? wz : 1 - wz, w1, w2;
                        if (wl == 0) continue;
                        const PartFlank *Fs = &MA->fl[sA][tA * NL + lay], *Fm = &MB->fl[sB][tB * NL + lay];
                        int ia = flank_bracket(Fs, ks_rA[k], &w1), ib = flank_bracket(Fm, ks_rB[k], &w2);
                        if (ia < 0 || ib < 0) continue;
                        ADD(MA, bodyA, Fs->node[ia], w * wl * (1 - w1));
                        ADD(MA, bodyA, Fs->node[ia + 1], w * wl * w1);
                        ADD(MB, bodyB, Fm->node[ib], w * wl * (1 - w2));
                        ADD(MB, bodyB, Fm->node[ib + 1], w * wl * w2);
                    }
                }
            }
#undef ADD
            if (overflow) {
                free(hit);
                return snprintf(err, errlen, "a contact strip touches more than %d nodes", MC_MAX_TERMS), -1;
            }
            double sw0 = 0, sw1 = 0;
            for (int f = 0; f < nt; f++) {
                if (tb_[f] == bodyB) sw1 += tw_[f];
                else sw0 += tw_[f];
            }
            if (sw0 <= 0 || sw1 <= 0) {
                if ((MA->body.zone_cyc <= 0 && MA->body.zone_of[Fa->node[ka]] < 0) || (MB->body.zone_cyc <= 0 && MB->body.zone_of[Fb->node[kb]] < 0)) {
                    free(hit);
                    return snprintf(err, errlen, "%s and %s touch on teeth outside the compliance zone (the run's range)", A->name, B->name), -1;
                }
                continue;
            }
            for (int f = 0; f < nt; f++) tw_[f] /= tb_[f] == bodyB ? sw1 : sw0;
            double zc = 2 * zl[l] / bw - 1, where[3] = {H->wx, H->wy, zl[l] + A->az};
            int j = mcontact_add_q(cs, gn + crown * zc * zc, mrow, nt, tb_, tz_, tw_, bodyA, Fa->node[ka], where, n3);
            if (j < 0 || !nf_reserve(nf, j + 1)) {
                free(hit);
                return snprintf(err, errlen, "out of memory"), -1;
            }
            /* the contact line in this slice is longer than the slice by 1 / cos(beta_b); the strip's half-width
             * normal to it is the profile's times cos(beta_b) */
            nf->R[j] = Rs, nf->a[j] = a_k * cbb, nf->w[j] = (z_hi > z_lo ? z_hi - z_lo : 0.5 * bw / (NL - 1)) / cbb;
            added++;
        }
    }
    free(hit);
    return added;
}
