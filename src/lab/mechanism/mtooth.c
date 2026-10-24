/* mtooth.c - see mtooth.h */
#include "mtooth.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* the surface nodes of the middle tooth on one side and one layer, ordered from the tip to the root */
typedef struct Side {
    int *node;
    double *arc;   /* m from the tip corner */
    double *t;     /* 2 each: the unit tangent in the part frame, pointing toward the tip */
    int n;
} Side;

struct GearTooth {
    GearToothSpec spec;
    GearGeometry geo;
    GearMesh mesh;
    MBody body;
    int mid, nlayer;
    double tooth_angle;      /* polar angle of the middle tooth's centre line */
    double *ocum;            /* outline arc length from the space middle, per outline point */
    double olen;             /* the outline's whole length (space middle to tip corner) */
    Side *side[2];           /* [loaded][layer]: 1 the loaded (left) side, 0 the other */
    double *f, *u, *nstrain, *nstress, *xyz_def;
    float *mises, *surf, *nmises;
    double prof_arc[2][TOOTH_PROFILE_MAX], prof_s[2][TOOTH_PROFILE_MAX];
    int prof_n[2];
    double setup_seconds;
    char summary[1024];
};

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

static void rot(double a, double x, double y, double *ox, double *oy) {
    double c = cos(a), s = sin(a);
    *ox = c * x - s * y, *oy = s * x + c * y;
}

/* the nearest point of the right side's outline (local tooth frame) to (px, py): its arc length from the space
 * middle and the unit tangent there (toward the tip) */
static double outline_project(const GearTooth *gt, double px, double py, double *tx, double *ty) {
    const GearFlank *s = &gt->geo.right;
    double best = INFINITY, barc = 0;
    for (int i = 0; i + 1 < s->n; i++) {
        double ax = s->p[i].x, ay = s->p[i].y, ex = s->p[i + 1].x - ax, ey = s->p[i + 1].y - ay;
        double L2 = ex * ex + ey * ey, w = L2 > 0 ? ((px - ax) * ex + (py - ay) * ey) / L2 : 0;
        if (w < 0) w = 0;
        if (w > 1) w = 1;
        double dx = px - (ax + w * ex), dy = py - (ay + w * ey), d2 = dx * dx + dy * dy;
        if (d2 < best) {
            double L = sqrt(L2);
            best = d2, barc = gt->ocum[i] + w * L;
            *tx = L > 0 ? ex / L : 0, *ty = L > 0 ? ey / L : 1;
        }
    }
    return barc;
}

/* the point of the right side's outline at an arc length from the space middle, with its outward normal */
static void outline_at_arc(const GearTooth *gt, double arc, double *x, double *y, double *nx, double *ny) {
    const GearFlank *s = &gt->geo.right;
    int i = 0;
    while (i < s->n - 2 && gt->ocum[i + 1] < arc) i++;
    double d = gt->ocum[i + 1] - gt->ocum[i], w = d > 0 ? (arc - gt->ocum[i]) / d : 0;
    if (w < 0) w = 0;
    if (w > 1) w = 1;
    *x = s->p[i].x + w * (s->p[i + 1].x - s->p[i].x), *y = s->p[i].y + w * (s->p[i + 1].y - s->p[i].y);
    double ux = s->p[i].nx + w * (s->p[i + 1].nx - s->p[i].nx), uy = s->p[i].ny + w * (s->p[i + 1].ny - s->p[i].ny), L = hypot(ux, uy);
    *nx = ux / L, *ny = uy / L;
}

/* the arc length (from the space middle) of the outline point at radius r, the first one going up from the root */
static double outline_arc_at_radius(const GearTooth *gt, double r) {
    const GearFlank *s = &gt->geo.right;
    for (int i = 1; i < s->n; i++) {
        double r0 = hypot(s->p[i - 1].x, s->p[i - 1].y), r1 = hypot(s->p[i].x, s->p[i].y);
        if (r0 <= r && r <= r1 && r1 > r0) return gt->ocum[i - 1] + (r - r0) / (r1 - r0) * (gt->ocum[i] - gt->ocum[i - 1]);
    }
    return r <= hypot(s->p[0].x, s->p[0].y) ? 0 : gt->olen;
}

double geartooth_arc_at_radius(const GearTooth *gt, double r) { return gt->olen - outline_arc_at_radius(gt, r); }

/* Hofer's 30 degree tangent point: where the fillet's tangent makes 30 degrees with the tooth's centre line */
static double hofer30_arc(const GearTooth *gt) {
    const GearFlank *s = &gt->geo.right;
    for (int i = 1; i <= s->form && i < s->n; i++) {
        double tx0 = -s->p[i - 1].ny, tx1 = -s->p[i].ny;
        if (fabs(tx0) >= 0.5 && fabs(tx1) < 0.5) {
            double w = (fabs(tx0) - 0.5) / (fabs(tx0) - fabs(tx1));
            return gt->olen - (gt->ocum[i - 1] + w * (gt->ocum[i] - gt->ocum[i - 1]));
        }
    }
    return -1;
}

static void side_free(Side *s) {
    free(s->node), free(s->arc), free(s->t);
    memset(s, 0, sizeof *s);
}

void geartooth_free(GearTooth *gt) {
    if (!gt) return;
    for (int k = 0; k < 2; k++) {
        if (gt->side[k])
            for (int l = 0; l < gt->nlayer; l++) side_free(&gt->side[k][l]);
        free(gt->side[k]);
    }
    mbody_free(&gt->body);
    gear_mesh_free(&gt->mesh);
    gear_free(&gt->geo);
    free(gt->ocum), free(gt->f), free(gt->u), free(gt->nstrain), free(gt->nstress), free(gt->xyz_def), free(gt->mises), free(gt->surf), free(gt->nmises);
    free(gt);
}

/* collects one side's surface nodes of the middle tooth on one layer, with their arc and tangent */
static bool side_collect(GearTooth *gt, int loaded, int layer, Side *out) {
    const GearMesh *m = &gt->mesh;
    int sideflag = loaded ? GM_LEFT : GM_RIGHT, nz = gt->nlayer - 1;
    double b = gt->spec.gear.face_width_m;
    memset(out, 0, sizeof *out);
    int cap = 0;
    for (int i = 0; i < m->nnodes; i++)
        if (m->tooth[i] == gt->mid && (m->tag[i] & sideflag) && (m->tag[i] & (GM_FLANK | GM_FILLET)) && lround(m->xyz[3 * i + 2] / b * nz) == layer)
            cap++;
    out->node = malloc((size_t)(cap ? cap : 1) * sizeof(int));
    out->arc = malloc((size_t)(cap ? cap : 1) * sizeof(double));
    out->t = malloc((size_t)(cap ? cap : 1) * 2 * sizeof(double));
    if (!out->node || !out->arc || !out->t) return false;
    for (int i = 0; i < m->nnodes; i++) {
        if (!(m->tooth[i] == gt->mid && (m->tag[i] & sideflag) && (m->tag[i] & (GM_FLANK | GM_FILLET)) && lround(m->xyz[3 * i + 2] / b * nz) == layer))
            continue;
        double lx, ly, tx, ty;
        rot(-(gt->tooth_angle - M_PI / 2), m->xyz[3 * i], m->xyz[3 * i + 1], &lx, &ly);
        if (loaded) lx = -lx; /* the left side is the right side mirrored */
        double arc = gt->olen - outline_project(gt, lx, ly, &tx, &ty);
        if (loaded) tx = -tx;
        double ox, oy;
        rot(gt->tooth_angle - M_PI / 2, tx, ty, &ox, &oy);
        /* insertion by arc, tip first */
        int k = out->n;
        while (k > 0 && out->arc[k - 1] > arc) {
            out->node[k] = out->node[k - 1], out->arc[k] = out->arc[k - 1], out->t[2 * k] = out->t[2 * k - 2], out->t[2 * k + 1] = out->t[2 * k - 1];
            k--;
        }
        out->node[k] = i, out->arc[k] = arc, out->t[2 * k] = ox, out->t[2 * k + 1] = oy;
        out->n++;
    }
    return true;
}

GearTooth *geartooth_create(const GearToothSpec *spec, char *err, size_t errlen) {
    double t0 = now();
    GearTooth *gt = calloc(1, sizeof *gt);
    if (!gt) return snprintf(err, errlen, "out of memory"), NULL;
    gt->spec = *spec;
    const GearToothSpec *S = &gt->spec;
    if (S->mesh.teeth_modelled % 2 == 0) {
        snprintf(err, errlen, "gear tooth: model an odd number of teeth, so one sits in the middle");
        geartooth_free(gt);
        return NULL;
    }
    if (!gear_build(&S->gear, 600, &gt->geo, err, errlen)) {
        geartooth_free(gt);
        return NULL;
    }
    int z = S->gear.teeth;
    gt->mid = (S->mesh.teeth_modelled - 1) / 2;
    gt->tooth_angle = M_PI / 2;
    double angle0 = gt->tooth_angle - gt->mid * 2 * M_PI / z;
    if (!gear_mesh_build(&gt->geo, &S->mesh, angle0, &gt->mesh, err, errlen)) {
        geartooth_free(gt);
        return NULL;
    }
    gt->nlayer = S->mesh.n_face + 1;
    /* the outline's arc lengths */
    const GearFlank *o = &gt->geo.right;
    gt->ocum = malloc((size_t)o->n * sizeof(double));
    if (!gt->ocum) {
        geartooth_free(gt);
        return snprintf(err, errlen, "out of memory"), NULL;
    }
    gt->ocum[0] = 0;
    for (int i = 1; i < o->n; i++) gt->ocum[i] = gt->ocum[i - 1] + hypot(o->p[i].x - o->p[i - 1].x, o->p[i].y - o->p[i - 1].y);
    gt->olen = gt->ocum[o->n - 1];
    if (S->load_radius_m <= o->r_form || S->load_radius_m >= gt->geo.r_a) {
        snprintf(err, errlen, "gear tooth: the load radius %.6g m is not on the involute (form circle %.6g m, tip %.6g m)", S->load_radius_m, o->r_form,
                 gt->geo.r_a);
        geartooth_free(gt);
        return NULL;
    }
    unsigned char *held = calloc((size_t)gt->mesh.nnodes, 1);
    if (!held) {
        geartooth_free(gt);
        return snprintf(err, errlen, "out of memory"), NULL;
    }
    for (int i = 0; i < gt->mesh.nnodes; i++) held[i] = (gt->mesh.tag[i] & GM_BORE) || (S->hold_cut_faces && (gt->mesh.tag[i] & GM_CUT));
    bool ok = mbody_create(&gt->body, gt->mesh.nnodes, gt->mesh.xyz, gt->mesh.nelems, gt->mesh.conn, S->E, S->nu, held, err, errlen);
    free(held);
    if (!ok) {
        geartooth_free(gt);
        return NULL;
    }
    for (int k = 0; k < 2; k++) {
        gt->side[k] = calloc((size_t)gt->nlayer, sizeof(Side));
        if (!gt->side[k]) {
            geartooth_free(gt);
            return snprintf(err, errlen, "out of memory"), NULL;
        }
        for (int l = 0; l < gt->nlayer; l++)
            if (!side_collect(gt, k, l, &gt->side[k][l])) {
                geartooth_free(gt);
                return snprintf(err, errlen, "out of memory"), NULL;
            }
    }
    int nn = gt->mesh.nnodes;
    gt->f = calloc((size_t)nn * 3, sizeof(double));
    gt->u = calloc((size_t)nn * 3, sizeof(double));
    gt->nstrain = calloc((size_t)nn * 6, sizeof(double));
    gt->nstress = calloc((size_t)nn * 6, sizeof(double));
    gt->xyz_def = calloc((size_t)nn * 3, sizeof(double));
    gt->mises = calloc((size_t)gt->mesh.nelems, sizeof(float));
    gt->surf = calloc((size_t)nn, sizeof(float));
    gt->nmises = calloc((size_t)nn, sizeof(float));
    if (!gt->f || !gt->u || !gt->nstrain || !gt->nstress || !gt->xyz_def || !gt->mises || !gt->surf || !gt->nmises) {
        geartooth_free(gt);
        return snprintf(err, errlen, "out of memory"), NULL;
    }
    gt->setup_seconds = now() - t0;
    snprintf(gt->summary, sizeof gt->summary,
             "{\"nodes\":%d,\"elements\":%d,\"equations\":%d,\"factor_mb\":%.0f,\"factor_s\":%.2f,\"min_jacobian_ratio\":%.3f,"
             "\"r_form_m\":%.6g,\"r_root_m\":%.6g,\"r_tip_m\":%.6g,\"solver\":\"%s\",\"setup_s\":%.1f}",
             nn, gt->mesh.nelems, gt->body.neq, mfactor_megabytes(gt->body.K), gt->body.factor_seconds, gt->mesh.min_jacobian_ratio, o->r_form,
             gt->geo.r_f, gt->geo.r_a, mfactor_method(gt->body.K), gt->setup_seconds);
    return gt;
}

const char *geartooth_summary(const GearTooth *gt) { return gt->summary; }

/* a field along one side at one layer, at an arc length: linear between the bracketing nodes; false outside */
static bool side_value(const Side *s, const double *v, double arc, double *out) {
    if (s->n < 2 || arc < s->arc[0] - 1e-12 || arc > s->arc[s->n - 1] + 1e-12) return false;
    int k = 0;
    while (k < s->n - 2 && s->arc[k + 1] < arc) k++;
    double d = s->arc[k + 1] - s->arc[k], w = d > 0 ? (arc - s->arc[k]) / d : 0;
    w = w < 0 ? 0 : w > 1 ? 1 : w;
    *out = (1 - w) * v[k] + w * v[k + 1];
    return true;
}

/* the tangential strain (or stress) of a side's nodes along their tangents */
static void side_tangential(const Side *s, const double *tensor, bool engineering_shear, double *out) {
    for (int k = 0; k < s->n; k++) {
        const double *e = &tensor[6 * s->node[k]];
        double tx = s->t[2 * k], ty = s->t[2 * k + 1];
        out[k] = tx * tx * e[0] + ty * ty * e[1] + (engineering_shear ? 1 : 2) * tx * ty * e[3];
    }
}

/* a gauge's reading: the tangential strain averaged over its length, at its depth in the face */
static double gauge_read(const GearTooth *gt, const ToothGauge *G, double arc_c) {
    int nz = gt->nlayer - 1;
    double b = gt->spec.gear.face_width_m, z = G->z_m < 0 ? b / 2 : G->z_m, lz = z / b * nz;
    int l0 = (int)floor(lz), l1 = l0 + 1 <= nz ? l0 + 1 : l0;
    double zw = lz - l0;
    const Side *s0 = &gt->side[G->loaded_side ? 1 : 0][l0], *s1 = &gt->side[G->loaded_side ? 1 : 0][l1];
    double *v0 = malloc((size_t)(s0->n + 1) * sizeof(double)), *v1 = malloc((size_t)(s1->n + 1) * sizeof(double));
    side_tangential(s0, gt->nstrain, true, v0);
    side_tangential(s1, gt->nstrain, true, v1);
    int ns = G->length_m > 0 ? 9 : 1;
    double sum = 0;
    int cnt = 0;
    for (int i = 0; i < ns; i++) {
        double a = arc_c + (ns > 1 ? G->length_m * ((double)i / (ns - 1) - 0.5) : 0), e0, e1;
        if (side_value(s0, v0, a, &e0) && side_value(s1, v1, a, &e1)) sum += (1 - zw) * e0 + zw * e1, cnt++;
    }
    free(v0), free(v1);
    return cnt ? sum / cnt : NAN;
}

bool geartooth_solve(GearTooth *gt, double force_n, GearToothResult *res, char *err, size_t errlen) {
    const GearToothSpec *S = &gt->spec;
    const GearMesh *m = &gt->mesh;
    memset(res, 0, sizeof *res);
    res->force_n = force_n;
    int nn = m->nnodes, nz = gt->nlayer - 1;
    double b = S->gear.face_width_m;
    memset(gt->f, 0, (size_t)nn * 3 * sizeof(double));
    /* the contact's half-width along the profile: Hertz for a flat steel anvil on the involute, whose radius of
     * curvature at r is its roll length sqrt(r^2 - r_b^2) */
    double rL = S->load_radius_m, rho = sqrt(rL * rL - gt->geo.r_b * gt->geo.r_b);
    double Estar = S->E / (2 * (1 - S->nu * S->nu)), a = S->contact_halfwidth_m;
    if (a <= 0) a = sqrt(4 * (force_n / b) * rho / (M_PI * Estar));
    res->halfwidth_m = a;
    double arcL = outline_arc_at_radius(gt, rL); /* from the space middle */
    /* elliptic pressure along the profile, sampled at the midpoints of equal arc steps; uniform across the face.
     * Each sample's force goes to the two surface nodes bracketing it on every layer (linear along the profile,
     * trapezoidal across the face, which is consistent for a line load on bilinear faces). */
    enum { NS = 64 };
    double wsum = 0, wk[NS], dn[NS][2], dt[NS][2];
    for (int k = 0; k < NS; k++) {
        double s = -1 + (2.0 * k + 1) / NS;
        wk[k] = sqrt(fmax(0, 1 - s * s)), wsum += wk[k];
    }
    double moment = 0, arc_tip[NS];
    for (int k = 0; k < NS; k++) {
        double s = -1 + (2.0 * k + 1) / NS, arc = arcL + s * a, x, y, nx, ny;
        outline_at_arc(gt, arc, &x, &y, &nx, &ny);
        /* the right side's tangent toward the tip is the normal turned a quarter counter-clockwise; the loaded side
         * is the left: mirror, then turn to the tooth */
        double tx = -ny, ty = nx;
        x = -x, nx = -nx, tx = -tx;
        double wx, wy;
        rot(gt->tooth_angle - M_PI / 2, x, y, &wx, &wy);
        rot(gt->tooth_angle - M_PI / 2, nx, ny, &dn[k][0], &dn[k][1]);
        rot(gt->tooth_angle - M_PI / 2, tx, ty, &dt[k][0], &dt[k][1]);
        arc_tip[k] = gt->olen - arc;
        moment += force_n * wk[k] / wsum * (wx * -dn[k][1] - wy * -dn[k][0]);
    }
    /* friction at the anvil (Coulomb, sliding): mu times the pressure along the flank, against the way the tooth's
     * surface moves under the load (the anvil is guided along the load's line), found from a frictionless solve */
    double mu = S->anvil_friction, sense = 0, ut = 0;
    for (int pass = 0; pass < (mu > 0 ? 2 : 1); pass++) {
        memset(gt->f, 0, (size_t)nn * 3 * sizeof(double));
        for (int k = 0; k < NS; k++) {
            double Fk = force_n * wk[k] / wsum;
            for (int l = 0; l <= nz; l++) {
                const Side *sd = &gt->side[1][l];
                double wl = (l == 0 || l == nz ? 0.5 : 1.0) / nz;
                if (sd->n < 2) continue;
                int q = 0;
                while (q < sd->n - 2 && sd->arc[q + 1] < arc_tip[k]) q++;
                double d = sd->arc[q + 1] - sd->arc[q], w = d > 0 ? (arc_tip[k] - sd->arc[q]) / d : 0;
                w = w < 0 ? 0 : w > 1 ? 1 : w;
                int n0 = sd->node[q], n1 = sd->node[q + 1];
                for (int c = 0; c < 2; c++) {
                    double dir = -dn[k][c] + mu * sense * dt[k][c]; /* into the tooth, and the friction along it */
                    gt->f[3 * n0 + c] += (1 - w) * wl * Fk * dir;
                    gt->f[3 * n1 + c] += w * wl * Fk * dir;
                }
            }
        }
        if (!mbody_solve(&gt->body, gt->f, gt->u)) return snprintf(err, errlen, "gear tooth: the solve failed"), false;
        if (mu > 0 && pass == 0) {
            /* the surface's motion along the flank at the contact, from this frictionless solve */
            ut = 0;
            for (int k = 0; k < NS; k++) {
                double Fk = force_n * wk[k] / wsum;
                for (int l = 0; l <= nz; l++) {
                    const Side *sd = &gt->side[1][l];
                    double wl = (l == 0 || l == nz ? 0.5 : 1.0) / nz;
                    if (sd->n < 2) continue;
                    int q = 0;
                    while (q < sd->n - 2 && sd->arc[q + 1] < arc_tip[k]) q++;
                    double d = sd->arc[q + 1] - sd->arc[q], w = d > 0 ? (arc_tip[k] - sd->arc[q]) / d : 0;
                    w = w < 0 ? 0 : w > 1 ? 1 : w;
                    int n0 = sd->node[q], n1 = sd->node[q + 1];
                    for (int c = 0; c < 2; c++) ut += (wl * Fk / force_n) * dt[k][c] * ((1 - w) * gt->u[3 * n0 + c] + w * gt->u[3 * n1 + c]);
                }
            }
            sense = ut > 0 ? -1 : 1;
            res->surface_slip_m = ut;
        }
    }
    res->friction_sense = sense;
    res->load_arm_check_nm = moment;
    mbody_fields(&gt->body, gt->u, gt->nstrain, gt->nstress, gt->mises);
    mbody_node_mises(&gt->body, gt->nstrain, gt->nmises);
    for (int e = 0; e < m->nelems; e++) res->max_mises_pa = fmax(res->max_mises_pa, gt->mises[e]);
    /* the surface stress, and the mid-face profiles */
    memset(gt->surf, 0, (size_t)nn * sizeof(float));
    double lz = 0.5 * nz;
    int l0 = (int)floor(lz), l1 = l0 + 1 <= nz ? l0 + 1 : l0;
    double zw = lz - l0;
    for (int k = 0; k < 2; k++) {
        for (int l = 0; l <= nz; l++) {
            const Side *sd = &gt->side[k][l];
            double *v = malloc((size_t)(sd->n + 1) * sizeof(double));
            side_tangential(sd, gt->nstress, false, v);
            for (int q = 0; q < sd->n; q++) gt->surf[sd->node[q]] = (float)v[q];
            free(v);
        }
        const Side *s0 = &gt->side[k][l0], *s1 = &gt->side[k][l1];
        double *v0 = malloc((size_t)(s0->n + 1) * sizeof(double)), *v1 = malloc((size_t)(s1->n + 1) * sizeof(double));
        side_tangential(s0, gt->nstress, false, v0);
        side_tangential(s1, gt->nstress, false, v1);
        int np = 0;
        for (int q = 0; q < s0->n && np < TOOTH_PROFILE_MAX; q++) {
            double e1;
            if (!side_value(s1, v1, s0->arc[q], &e1)) e1 = v0[q];
            gt->prof_arc[k][np] = s0->arc[q], gt->prof_s[k][np] = (1 - zw) * v0[q] + zw * e1, np++;
        }
        gt->prof_n[k] = np;
        free(v0), free(v1);
    }
    /* the extremes in the root region (below the middle of the loaded flank: the tip corner and the contact are not
     * root stresses), refined by a parabola through the largest node and its neighbours */
    double arc_root = geartooth_arc_at_radius(gt, 0.5 * (gt->geo.right.r_form + rL));
    for (int k = 0; k < 2; k++) {
        int np = gt->prof_n[k], ib = -1;
        double sign = k ? 1 : -1, best = -INFINITY;
        for (int q = 0; q < np; q++)
            if (gt->prof_arc[k][q] >= arc_root && sign * gt->prof_s[k][q] > best) best = sign * gt->prof_s[k][q], ib = q;
        double arc = ib >= 0 ? gt->prof_arc[k][ib] : NAN, val = sign * best;
        if (ib > 0 && ib < np - 1) {
            double x0 = gt->prof_arc[k][ib - 1], x1 = gt->prof_arc[k][ib], x2 = gt->prof_arc[k][ib + 1];
            double y0 = gt->prof_s[k][ib - 1], y1 = gt->prof_s[k][ib], y2 = gt->prof_s[k][ib + 1];
            double d01 = (y1 - y0) / (x1 - x0), d12 = (y2 - y1) / (x2 - x1), c2 = (d12 - d01) / (x2 - x0);
            if (c2 * sign < 0) { /* the parabola's vertex, in Newton form p(x) = y0 + d01 (x - x0) + c2 (x - x0)(x - x1) */
                double xv = 0.5 * (x0 + x1) - d01 / (2 * c2);
                if (xv > x0 && xv < x2) arc = xv, val = y0 + d01 * (xv - x0) + c2 * (xv - x0) * (xv - x1);
            }
        }
        if (k) res->max_tension_pa = val, res->max_tension_arc_m = arc;
        else res->max_compression_pa = val, res->max_compression_arc_m = arc;
    }
    /* gauges */
    for (int g = 0; g < S->ngauges; g++) {
        const ToothGauge *G = &S->gauge[g];
        double arc = NAN;
        switch (G->where) {
        case TG_HOFER30: arc = hofer30_arc(gt); break;
        case TG_MAX: arc = G->loaded_side ? res->max_tension_arc_m : res->max_compression_arc_m; break;
        case TG_ARC: arc = G->arc_m; break;
        case TG_RADIUS: arc = geartooth_arc_at_radius(gt, G->radius_m); break;
        }
        res->gauge_arc_m[g] = arc;
        res->gauge_strain[g] = arc == arc ? gauge_read(gt, G, arc) : NAN;
    }
    for (int i = 0; i < nn; i++)
        for (int c = 0; c < 3; c++) gt->xyz_def[3 * i + c] = m->xyz[3 * i + c];
    return true;
}

void geartooth_frame(GearTooth *gt, double scale, const double **xyz, int *nnodes, const int **conn, int *nelems, const float **mises,
                     const float **surface_stress, const float **node_mises) {
    const GearMesh *m = &gt->mesh;
    for (int i = 0; i < 3 * m->nnodes; i++) gt->xyz_def[i] = m->xyz[i] + scale * gt->u[i];
    *xyz = gt->xyz_def, *nnodes = m->nnodes, *conn = m->conn, *nelems = m->nelems, *mises = gt->mises, *surface_stress = gt->surf;
    *node_mises = gt->nmises;
}

int geartooth_profile(const GearTooth *gt, int loaded_side, const double **arc_m, const double **stress_pa) {
    int k = loaded_side ? 1 : 0;
    *arc_m = gt->prof_arc[k], *stress_pa = gt->prof_s[k];
    return gt->prof_n[k];
}
