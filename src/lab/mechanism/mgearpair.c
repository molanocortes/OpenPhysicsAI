/* mgearpair.c - see mgearpair.h */
#include "mgearpair.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct Flank {
    int *node; /* sorted by profile radius */
    double *r;
    int n;
} Flank;

typedef struct Gauge {
    GearGauge spec;
    int body, tooth, side, layer0, layer1;
    double zw;                      /* weight of layer1 */
    int nsamp;
    double samp_r[9], samp_t[9][3]; /* outline radius and unit tangent (part frame) of each sample */
    Flank surf[2];                  /* surface nodes of that tooth and side on the two layers */
    double r_centre;
} Gauge;

struct GearPair {
    GearPairSpec spec;
    GearGeometry geo[2];
    GearGeometry kin[2]; /* the same gears without tip relief: roll angles are kinematic, of perfect involutes */
    bool roll_known;
    double roll_ref;     /* the gauged tooth's roll angle at turn 0, degrees (it falls by z0/z1 per degree of turn) */
    GearMesh mesh[2];
    MBody body[2];
    double centre[2][2];
    double angle0[2];
    int sign;           /* +1: the pinion turns counter-clockwise */
    int side[2];        /* the driving flank of the pinion, the mating flank of the gear */
    int nlayer[2];
    Flank *mflank;      /* the gear's mating flank, [tooth * nlayer + layer] */
    Flank *sflank;      /* the pinion's driving flank, the same way */
    int *slave;         /* the pinion's driving-flank nodes */
    int nslave;
    Gauge gauge[8];
    int ngauge;
    MContactSet cs;
    double *lambda;
    double *f[2], *u[2], *nstrain[2], *world[2];
    float *mises[2], *contact[2], *nmises[2]; /* per element; per node the contact force; per node von Mises */
    double *ocum[2];    /* arc length along each part's outline (geo.right) from the space middle */
    double *nf_R, *nf_a, *nf_w; /* per contact: the curvature radius, the strip's half-width, the face width it stands for */
    int nf_cap;
    double setup_seconds;
    char summary[2048];
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

static int layer_of(const GearMesh *m, int node, int nz, double b) {
    return (int)lround(m->xyz[3 * node + 2] / b * nz);
}

/* the nearest point of a side's outline (right side of the local tooth, involute part from index i0) to P; returns
 * the signed distance along the outward normal, the point, its normal, and whether it fell inside the range */
static double outline_nearest(const GearFlank *s, int i0, double px, double py, double *qx, double *qy, double *nx, double *ny, bool *inside) {
    double best = INFINITY;
    int bi = i0;
    double bt = 0;
    for (int i = i0; i < s->n - 1; i++) {
        double ax = s->p[i].x, ay = s->p[i].y, ex = s->p[i + 1].x - ax, ey = s->p[i + 1].y - ay;
        double L2 = ex * ex + ey * ey, t = L2 > 0 ? ((px - ax) * ex + (py - ay) * ey) / L2 : 0;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        double dx = px - (ax + t * ex), dy = py - (ay + t * ey), d2 = dx * dx + dy * dy;
        if (d2 < best) best = d2, bi = i, bt = t;
    }
    const GearPoint *a = &s->p[bi], *b = &s->p[bi + 1];
    *qx = a->x + bt * (b->x - a->x), *qy = a->y + bt * (b->y - a->y);
    double ux = a->nx + bt * (b->nx - a->nx), uy = a->ny + bt * (b->ny - a->ny), L = hypot(ux, uy);
    *nx = ux / L, *ny = uy / L;
    *inside = !((bi == i0 && bt <= 0) || (bi == s->n - 2 && bt >= 1));
    return (px - *qx) * *nx + (py - *qy) * *ny;
}

/* sort node ids by their profile radius (a small insertion sort: a flank has tens of nodes) */
static void sort_by_radius(int *ids, int n, const double *profile) {
    for (int i = 1; i < n; i++) {
        int v = ids[i], j = i - 1;
        while (j >= 0 && profile[ids[j]] > profile[v]) ids[j + 1] = ids[j], j--;
        ids[j + 1] = v;
    }
}

static bool flank_collect(const GearMesh *m, int tooth, int side, int layer, int nz, double b, bool surface, Flank *out) {
    memset(out, 0, sizeof *out);
    int n = 0;
    for (int pass = 0; pass < 2; pass++) {
        n = 0;
        for (int i = 0; i < m->nnodes; i++) {
            int t = m->tag[i];
            if (m->tooth[i] != tooth || !(t & side)) continue;
            if (!(t & GM_FLANK) && !(surface && (t & GM_FILLET))) continue;
            if (layer_of(m, i, nz, b) != layer) continue;
            if (pass) out->node[n] = i;
            n++;
        }
        if (!pass && !(out->node = malloc((size_t)(n ? n : 1) * sizeof(int)))) return false;
    }
    sort_by_radius(out->node, n, m->profile);
    out->r = malloc((size_t)(n ? n : 1) * sizeof(double));
    for (int k = 0; k < n; k++) out->r[k] = m->profile[out->node[k]];
    out->n = n;
    return true;
}

static void flank_free(Flank *f) {
    free(f->node), free(f->r);
    f->node = NULL, f->r = NULL, f->n = 0;
}

/* the bracketing pair of a radius along a flank: index k with r[k] <= r <= r[k+1], and the weight of k+1 */
static int flank_bracket(const Flank *f, double r, double *w) {
    if (f->n < 2 || r < f->r[0] - 1e-12 || r > f->r[f->n - 1] + 1e-12) return -1;
    int k = 0;
    while (k < f->n - 2 && f->r[k + 1] < r) k++;
    double d = f->r[k + 1] - f->r[k];
    *w = d > 0 ? (r - f->r[k]) / d : 0;
    if (*w < 0) *w = 0;
    if (*w > 1) *w = 1;
    return k;
}

double gearpair_base_radius(const GearPair *gp, int body) { return gp->geo[body].r_b; }
void gearpair_set_torque(GearPair *gp, double torque_nm) { gp->spec.torque_nm = torque_nm; }

void gearpair_free(GearPair *gp) {
    if (!gp) return;
    for (int b = 0; b < 2; b++) {
        mbody_free(&gp->body[b]);
        gear_mesh_free(&gp->mesh[b]);
        gear_free(&gp->geo[b]);
        gear_free(&gp->kin[b]);
        free(gp->f[b]), free(gp->u[b]), free(gp->nstrain[b]), free(gp->world[b]), free(gp->mises[b]), free(gp->contact[b]), free(gp->nmises[b]);
    }
    if (gp->mflank)
        for (int i = 0; i < gp->mesh[1].teeth * gp->nlayer[1]; i++) flank_free(&gp->mflank[i]);
    if (gp->sflank)
        for (int i = 0; i < gp->mesh[0].teeth * gp->nlayer[0]; i++) flank_free(&gp->sflank[i]);
    free(gp->mflank), free(gp->sflank), free(gp->slave), free(gp->lambda), free(gp->ocum[0]), free(gp->ocum[1]);
    free(gp->nf_R), free(gp->nf_a), free(gp->nf_w);
    for (int g = 0; g < gp->ngauge; g++) flank_free(&gp->gauge[g].surf[0]), flank_free(&gp->gauge[g].surf[1]);
    mcontact_free(&gp->cs);
    free(gp);
}

/* Hofer's 30 degree tangent point on a side's fillet: where the outline's tangent makes 30 degrees with the tooth's
 * centre line (|t_x| = 1/2 in the tooth's frame, t the unit tangent); returns its radius */
static double hofer30(const GearGeometry *g) {
    const GearFlank *s = &g->right;
    for (int i = 1; i <= s->form && i < s->n; i++) {
        double tx0 = -s->p[i - 1].ny, tx1 = -s->p[i].ny; /* tangent (-ny, nx): its x component */
        if (fabs(tx0) >= 0.5 && fabs(tx1) < 0.5) {
            double w = (fabs(tx0) - 0.5) / (fabs(tx0) - fabs(tx1));
            return hypot(s->p[i - 1].x + w * (s->p[i].x - s->p[i - 1].x), s->p[i - 1].y + w * (s->p[i].y - s->p[i - 1].y));
        }
    }
    return 0;
}

/* a point of the right side's outline at radius r (the side is monotonic in radius): position and unit tangent pointing
 * toward the tip, in the tooth's frame */
static bool outline_at_radius(const GearGeometry *g, double r, double *x, double *y, double *tx, double *ty) {
    const GearFlank *s = &g->right;
    for (int i = 1; i < s->n; i++) {
        double r0 = hypot(s->p[i - 1].x, s->p[i - 1].y), r1 = hypot(s->p[i].x, s->p[i].y);
        if (r0 <= r && r <= r1) {
            double w = r1 > r0 ? (r - r0) / (r1 - r0) : 0;
            *x = s->p[i - 1].x + w * (s->p[i].x - s->p[i - 1].x), *y = s->p[i - 1].y + w * (s->p[i].y - s->p[i - 1].y);
            double ex = s->p[i].x - s->p[i - 1].x, ey = s->p[i].y - s->p[i - 1].y, L = hypot(ex, ey);
            *tx = ex / L, *ty = ey / L;
            return true;
        }
    }
    return false;
}

GearPair *gearpair_create(const GearPairSpec *spec, char *err, size_t errlen) {
    double t0 = now();
    GearPair *gp = calloc(1, sizeof *gp);
    if (!gp) return snprintf(err, errlen, "out of memory"), NULL;
    gp->spec = *spec;
    const GearPairSpec *S = &gp->spec;
    gp->sign = 1;
    double a = S->centre_distance_m > 0 ? S->centre_distance_m : gear_centre_distance(&S->gear[0], &S->gear[1]);
    gp->centre[0][0] = 0, gp->centre[0][1] = 0, gp->centre[1][0] = 0, gp->centre[1][1] = a;
    for (int b = 0; b < 2; b++) {
        if (S->mesh[b].teeth_modelled % 2 == 0 && S->mesh[b].teeth_modelled != S->gear[b].teeth) {
            snprintf(err, errlen, "%s: model an odd number of teeth (or the whole gear), so one sits in the middle", b ? "gear" : "pinion");
            gearpair_free(gp);
            return NULL;
        }
        GearSpec plain = S->gear[b];
        plain.tip_relief_m = 0;
        if (!gear_build(&S->gear[b], 600, &gp->geo[b], err, errlen) || !gear_build(&plain, 600, &gp->kin[b], err, errlen)) {
            gearpair_free(gp);
            return NULL;
        }
        int z = S->gear[b].teeth, T = S->mesh[b].teeth_modelled, mid = (T - 1) / 2;
        /* the pinion's middle tooth points at the gear (+y); a space of the gear faces it (-y), with the gear's middle tooth
         * on the side the pinion's driving flank turns toward, so the middle teeth of the two engage */
        double centre_angle = b == 0 ? M_PI / 2 : -M_PI / 2 - M_PI / z;
        gp->angle0[b] = centre_angle - mid * 2 * M_PI / z;
        if (!gear_mesh_build(&gp->geo[b], &S->mesh[b], gp->angle0[b], &gp->mesh[b], err, errlen)) {
            gearpair_free(gp);
            return NULL;
        }
        gp->nlayer[b] = S->mesh[b].n_face + 1;
        const GearFlank *of = &gp->geo[b].right;
        if (!(gp->ocum[b] = malloc((size_t)of->n * sizeof(double)))) {
            gearpair_free(gp);
            return snprintf(err, errlen, "out of memory"), NULL;
        }
        gp->ocum[b][0] = 0;
        for (int i = 1; i < of->n; i++) gp->ocum[b][i] = gp->ocum[b][i - 1] + hypot(of->p[i].x - of->p[i - 1].x, of->p[i].y - of->p[i - 1].y);
        unsigned char *held = calloc((size_t)gp->mesh[b].nnodes, 1);
        for (int i = 0; i < gp->mesh[b].nnodes; i++)
            held[i] = (gp->mesh[b].tag[i] & GM_BORE) || (S->hold_cut_faces && (gp->mesh[b].tag[i] & GM_CUT));
        bool ok = mbody_create(&gp->body[b], gp->mesh[b].nnodes, gp->mesh[b].xyz, gp->mesh[b].nelems, gp->mesh[b].conn, S->E[b], S->nu[b], held,
                               err, errlen);
        free(held);
        if (!ok) {
            gearpair_free(gp);
            return NULL;
        }
    }
    if (fabs(S->gear[0].face_width_m - S->gear[1].face_width_m) > 1e-12 || S->mesh[0].n_face != S->mesh[1].n_face) {
        snprintf(err, errlen, "both gears need the same face width and the same layers through it (contact is node to node across the face)");
        gearpair_free(gp);
        return NULL;
    }
    /* the pinion drives counter-clockwise: its leading flank faces increasing polar angle (GM_LEFT), and so does the
     * gear's flank that receives it */
    gp->side[0] = GM_LEFT, gp->side[1] = GM_LEFT;
    /* zones: the pinion's driving flank nodes, the gear's mating flank nodes, directed into each part */
    for (int b = 0; b < 2; b++) {
        GearMesh *m = &gp->mesh[b];
        int *zn = malloc((size_t)m->nnodes * sizeof(int)), nz = 0;
        double *zd = malloc((size_t)m->nnodes * 3 * sizeof(double));
        for (int i = 0; i < m->nnodes; i++)
            if ((m->tag[i] & GM_FLANK) && (m->tag[i] & gp->side[b])) {
                zn[nz] = i, zd[3 * nz] = -m->normal[2 * i], zd[3 * nz + 1] = -m->normal[2 * i + 1], zd[3 * nz + 2] = 0;
                nz++;
            }
        bool ok = mbody_zone(&gp->body[b], nz, zn, zd, err, errlen);
        if (b == 0 && ok) {
            gp->slave = malloc((size_t)nz * sizeof(int));
            memcpy(gp->slave, zn, (size_t)nz * sizeof(int));
            gp->nslave = nz;
        }
        free(zn), free(zd);
        if (!ok) {
            gearpair_free(gp);
            return NULL;
        }
    }
    /* the gear's mating flank, tooth by tooth and layer by layer, sorted along the profile */
    int TB = gp->mesh[1].teeth, NL = gp->nlayer[1];
    gp->mflank = calloc((size_t)TB * NL, sizeof(Flank));
    for (int t = 0; t < TB; t++)
        for (int l = 0; l < NL; l++)
            flank_collect(&gp->mesh[1], t, gp->side[1], l, NL - 1, S->gear[1].face_width_m, false, &gp->mflank[t * NL + l]);
    int TA = gp->mesh[0].teeth;
    gp->sflank = calloc((size_t)TA * NL, sizeof(Flank));
    for (int t = 0; t < TA; t++)
        for (int l = 0; l < NL; l++)
            flank_collect(&gp->mesh[0], t, gp->side[0], l, NL - 1, S->gear[0].face_width_m, false, &gp->sflank[t * NL + l]);
    /* gauges */
    for (int k = 0; k < S->ngauges && k < 8; k++) {
        Gauge *G = &gp->gauge[gp->ngauge];
        G->spec = S->gauge[k];
        int b = G->spec.body, T = gp->mesh[b].teeth, mid = (T - 1) / 2;
        G->body = b, G->tooth = mid + G->spec.tooth;
        if (G->tooth < 0 || G->tooth >= T) {
            snprintf(err, errlen, "gauge %s: tooth %d is outside the modelled sector", G->spec.name, G->spec.tooth);
            gearpair_free(gp);
            return NULL;
        }
        int other = gp->side[b] == GM_LEFT ? GM_RIGHT : GM_LEFT;
        G->side = G->spec.loaded_side ? gp->side[b] : other;
        double bw = S->gear[b].face_width_m, z = G->spec.z_m < 0 ? bw / 2 : G->spec.z_m;
        double lz = z / bw * (NL - 1);
        G->layer0 = (int)floor(lz), G->layer1 = G->layer0 + 1 < NL ? G->layer0 + 1 : G->layer0, G->zw = lz - G->layer0;
        flank_collect(&gp->mesh[b], G->tooth, G->side, G->layer0, NL - 1, bw, true, &G->surf[0]);
        flank_collect(&gp->mesh[b], G->tooth, G->side, G->layer1, NL - 1, bw, true, &G->surf[1]);
        double rc = G->spec.radius_m > 0 ? G->spec.radius_m : hofer30(&gp->geo[b]);
        if (rc <= 0) {
            snprintf(err, errlen, "gauge %s: no point on the fillet at that position", G->spec.name);
            gearpair_free(gp);
            return NULL;
        }
        G->r_centre = rc;
        /* samples along the outline over the gauge's length, by arc length about the centre */
        double x, y, tx, ty;
        if (!outline_at_radius(&gp->geo[b], rc, &x, &y, &tx, &ty)) {
            snprintf(err, errlen, "gauge %s: radius %.4g m is not on the tooth's outline", G->spec.name, rc);
            gearpair_free(gp);
            return NULL;
        }
        int ns = G->spec.length_m > 0 ? 9 : 1;
        G->nsamp = 0;
        for (int i = 0; i < ns; i++) {
            double ds = ns > 1 ? G->spec.length_m * ((double)i / (ns - 1) - 0.5) : 0;
            /* walk along the dense outline from the centre by ds */
            const GearFlank *s = &gp->geo[b].right;
            double r = rc;
            if (ds != 0) {
                int ic = 0;
                while (ic < s->n - 1 && hypot(s->p[ic + 1].x, s->p[ic + 1].y) < rc) ic++;
                double acc = 0, px = x, py = y;
                int dir = ds > 0 ? 1 : -1, j = dir > 0 ? ic + 1 : ic;
                while (j >= 0 && j < s->n) {
                    double qx = s->p[j].x, qy = s->p[j].y, seg = hypot(qx - px, qy - py);
                    if (acc + seg >= fabs(ds)) {
                        double w = (fabs(ds) - acc) / seg;
                        r = hypot(px + w * (qx - px), py + w * (qy - py));
                        break;
                    }
                    acc += seg, px = qx, py = qy, j += dir;
                }
            }
            double sx, sy, stx, sty;
            if (!outline_at_radius(&gp->geo[b], r, &sx, &sy, &stx, &sty)) continue;
            /* to the part's frame: the right side's tangent, mirrored for a left side, turned to the tooth */
            if (G->side == GM_LEFT) stx = -stx;
            double ta = gp->angle0[b] + G->tooth * 2 * M_PI / S->gear[b].teeth - M_PI / 2, ox, oy;
            rot(ta, stx, sty, &ox, &oy);
            G->samp_r[G->nsamp] = r, G->samp_t[G->nsamp][0] = ox, G->samp_t[G->nsamp][1] = oy, G->samp_t[G->nsamp][2] = 0;
            G->nsamp++;
        }
        gp->ngauge++;
    }
    for (int b = 0; b < 2; b++) {
        int nn = gp->mesh[b].nnodes;
        gp->f[b] = calloc((size_t)nn * 3, sizeof(double));
        gp->u[b] = calloc((size_t)nn * 3, sizeof(double));
        gp->nstrain[b] = calloc((size_t)nn * 6, sizeof(double));
        gp->world[b] = calloc((size_t)nn * 3, sizeof(double));
        gp->mises[b] = calloc((size_t)gp->mesh[b].nelems, sizeof(float));
        gp->nmises[b] = calloc((size_t)nn, sizeof(float));
        gp->contact[b] = calloc((size_t)nn, sizeof(float));
    }
    gp->setup_seconds = now() - t0;
    snprintf(gp->summary, sizeof gp->summary,
             "{\"centre_distance_m\":%.9g,\"pinion\":{\"nodes\":%d,\"elements\":%d,\"equations\":%d,\"factor_mb\":%.0f,\"factor_s\":%.2f,"
             "\"zone_nodes\":%d,\"zone_s\":%.1f,\"min_jacobian_ratio\":%.3f},\"gear\":{\"nodes\":%d,\"elements\":%d,\"equations\":%d,"
             "\"factor_mb\":%.0f,\"factor_s\":%.2f,\"zone_nodes\":%d,\"zone_s\":%.1f,\"min_jacobian_ratio\":%.3f},\"solver\":\"%s\",\"setup_s\":%.1f}",
             a, gp->mesh[0].nnodes, gp->mesh[0].nelems, gp->body[0].neq, mfactor_megabytes(gp->body[0].K), gp->body[0].factor_seconds,
             gp->body[0].nzone, gp->body[0].zone_seconds, gp->mesh[0].min_jacobian_ratio, gp->mesh[1].nnodes, gp->mesh[1].nelems,
             gp->body[1].neq, mfactor_megabytes(gp->body[1].K), gp->body[1].factor_seconds, gp->body[1].nzone, gp->body[1].zone_seconds,
             gp->mesh[1].min_jacobian_ratio, mfactor_method(gp->body[0].K), gp->setup_seconds);
    return gp;
}

const char *gearpair_summary(GearPair *gp) { return gp->summary; }

static double gauge_read(const GearPair *gp, const Gauge *G) {
    const double *ns = gp->nstrain[G->body];
    double sum = 0;
    int cnt = 0;
    for (int i = 0; i < G->nsamp; i++) {
        double eps[2][6];
        bool ok = true;
        for (int L = 0; L < 2; L++) {
            double w;
            int k = flank_bracket(&G->surf[L], G->samp_r[i], &w);
            if (k < 0) {
                ok = false;
                break;
            }
            for (int c = 0; c < 6; c++) eps[L][c] = (1 - w) * ns[6 * G->surf[L].node[k] + c] + w * ns[6 * G->surf[L].node[k + 1] + c];
        }
        if (!ok) continue;
        double e[6];
        for (int c = 0; c < 6; c++) e[c] = (1 - G->zw) * eps[0][c] + G->zw * eps[1][c];
        const double *t = G->samp_t[i];
        /* normal strain along t: t' eps t with engineering shears */
        double v = t[0] * t[0] * e[0] + t[1] * t[1] * e[1] + t[2] * t[2] * e[2] + t[0] * t[1] * e[3] + t[1] * t[2] * e[4] + t[2] * t[0] * e[5];
        sum += v, cnt++;
    }
    return cnt ? sum / cnt : NAN;
}

/* A point of tooth t's mating side of part b (0 the pinion, 1 the gear) at outline radius r, in the world at that
 * part's turn phi, with the side's outward normal there */
static bool side_point(const GearPair *gp, int b, int t, double phi, double r, double *wx, double *wy, double *nx, double *ny) {
    double x, y, tx, ty;
    if (!outline_at_radius(&gp->geo[b], r, &x, &y, &tx, &ty)) return false;
    double ox = ty, oy = -tx; /* the right side's outward normal: its tangent toward the tip, turned a quarter clockwise */
    if (gp->side[b] == GM_LEFT) x = -x, ox = -ox; /* the left side is the right side mirrored */
    double ta = gp->angle0[b] + t * 2 * M_PI / gp->spec.gear[b].teeth - M_PI / 2, px, py, qx, qy;
    rot(ta, x, y, &px, &py);
    rot(phi, px, py, wx, wy);
    *wx += gp->centre[b][0], *wy += gp->centre[b][1];
    rot(ta, ox, oy, &qx, &qy);
    rot(phi, qx, qy, nx, ny);
    return true;
}

/* A world point against tooth t's mating side of part b: its distance along the side's outward normal (positive
 * outside the material), the foot's radius and the outward normal there (world); `inside` is false when the foot
 * falls past either end of the active flank (the form circle, the tip corner) */
static double side_project(const GearPair *gp, int b, int t, double phi, double wx, double wy, double *radius, double *nwx, double *nwy, bool *inside) {
    const GearGeometry *g = &gp->geo[b];
    double bx, by, lx, ly, ta = gp->angle0[b] + t * 2 * M_PI / gp->spec.gear[b].teeth - M_PI / 2;
    rot(-phi, wx - gp->centre[b][0], wy - gp->centre[b][1], &bx, &by);
    rot(-ta, bx, by, &lx, &ly);
    if (gp->side[b] == GM_LEFT) lx = -lx;
    double qx, qy, nx, ny, ox, oy;
    double d = outline_nearest(&g->right, g->right.form, lx, ly, &qx, &qy, &nx, &ny, inside);
    *radius = hypot(qx, qy);
    if (gp->side[b] == GM_LEFT) nx = -nx;
    rot(ta, nx, ny, &ox, &oy);
    rot(phi, ox, oy, nwx, nwy);
    return d;
}

/* the arc length along part b's outline (from the space middle) at radius r, and back */
static double side_arc(const GearPair *gp, int b, double r) {
    const GearFlank *s = &gp->geo[b].right;
    const double *c = gp->ocum[b];
    for (int i = 1; i < s->n; i++) {
        double r0 = hypot(s->p[i - 1].x, s->p[i - 1].y), r1 = hypot(s->p[i].x, s->p[i].y);
        if (r0 <= r && r <= r1 && r1 > r0) return c[i - 1] + (r - r0) / (r1 - r0) * (c[i] - c[i - 1]);
    }
    return r < hypot(s->p[0].x, s->p[0].y) ? 0 : c[s->n - 1];
}
static double side_radius(const GearPair *gp, int b, double arc) {
    const GearFlank *s = &gp->geo[b].right;
    const double *c = gp->ocum[b];
    int i = 1;
    while (i < s->n - 1 && c[i] < arc) i++;
    double d = c[i] - c[i - 1], w = d > 0 ? (arc - c[i - 1]) / d : 0;
    w = w < 0 ? 0 : w > 1 ? 1 : w;
    return hypot(s->p[i - 1].x + w * (s->p[i].x - s->p[i - 1].x), s->p[i - 1].y + w * (s->p[i].y - s->p[i - 1].y));
}

/* Where pinion tooth t touches gear tooth u: the closest approach of the two exact sides. Searched both ways: the
 * pinion's points against the gear's side, and the gear's points against the pinion's side (which finds the gear's
 * tip corner on the pinion's flank, at the start of contact). The contact point is on the pinion; the normal is the
 * gear's outward normal there (from the gear toward the pinion). */
typedef struct PairHit {
    double g;        /* gap, m (negative: the unloaded outlines overlap) */
    double rA, rB;   /* outline radii of the contact on the pinion and on the gear */
    double wx, wy;   /* the contact point (world) */
    double nx, ny;   /* the gear's outward normal (world) */
    bool ok;
} PairHit;

/* A side seen through its own tooth is not facing: a point more than a twentieth of a module inside it is past that
 * tooth, on its far side. */
static double pair_gap(const GearPair *gp, int dir, int t, int u, double phi0, double phi1, double r, PairHit *h) {
    double wx, wy, nx, ny, rr, mx, my, deep = -0.05 * gp->spec.gear[1].module_m;
    bool in;
    if (dir == 0) { /* the pinion's point at radius r against the gear's side */
        if (!side_point(gp, 0, t, phi0, r, &wx, &wy, &nx, &ny)) return INFINITY;
        double g = side_project(gp, 1, u, phi1, wx, wy, &rr, &mx, &my, &in);
        if (!in || g < deep) return INFINITY;
        if (h) h->g = g, h->rA = r, h->rB = rr, h->wx = wx, h->wy = wy, h->nx = mx, h->ny = my, h->ok = true;
        return g;
    }
    /* the gear's point at radius r against the pinion's side; the contact point is the foot on the pinion */
    if (!side_point(gp, 1, u, phi1, r, &wx, &wy, &nx, &ny)) return INFINITY;
    double g = side_project(gp, 0, t, phi0, wx, wy, &rr, &mx, &my, &in);
    if (!in || g < deep) return INFINITY;
    if (h) h->g = g, h->rA = rr, h->rB = r, h->wx = wx - g * mx, h->wy = wy - g * my, h->nx = -mx, h->ny = -my, h->ok = true;
    return g;
}

static PairHit pair_contact(const GearPair *gp, int t, int u, double phi0, double phi1) {
    PairHit best;
    memset(&best, 0, sizeof best);
    best.g = INFINITY;
    for (int dir = 0; dir < 2; dir++) {
        const GearGeometry *g = &gp->geo[dir];
        double r0 = g->right.r_form, r1 = g->r_a, bg = INFINITY;
        enum { N = 72 };
        int bi = -1;
        for (int i = 0; i <= N; i++) {
            double v = pair_gap(gp, dir, t, u, phi0, phi1, r0 + (r1 - r0) * i / N, NULL);
            if (v < bg) bg = v, bi = i;
        }
        if (bi < 0) continue;
        double a = r0 + (r1 - r0) * (bi > 0 ? bi - 1 : 0) / N, b = r0 + (r1 - r0) * (bi < N ? bi + 1 : N) / N;
        const double phi = 0.6180339887498949;
        double c = b - phi * (b - a), d = a + phi * (b - a);
        double gc = pair_gap(gp, dir, t, u, phi0, phi1, c, NULL), gd = pair_gap(gp, dir, t, u, phi0, phi1, d, NULL);
        for (int it = 0; it < 60; it++) {
            if (gc < gd) b = d, d = c, gd = gc, c = b - phi * (b - a), gc = pair_gap(gp, dir, t, u, phi0, phi1, c, NULL);
            else a = c, c = d, gc = gd, d = a + phi * (b - a), gd = pair_gap(gp, dir, t, u, phi0, phi1, d, NULL);
        }
        double ropt = r0 + (r1 - r0) * bi / N, gopt = bg, rm = 0.5 * (a + b), gm = pair_gap(gp, dir, t, u, phi0, phi1, rm, NULL);
        if (gm < gopt) ropt = rm, gopt = gm;
        PairHit h;
        memset(&h, 0, sizeof h);
        if (pair_gap(gp, dir, t, u, phi0, phi1, ropt, &h) < best.g && h.ok) best = h;
    }
    return best;
}

/* a point of the pinion's driving flank (radius r on tooth t's exact outline) in the world, at the pinion's turn phi0 */
static void pinion_point(const GearPair *gp, int t, double phi0, double r, double *wx, double *wy, double *ds) {
    double nx, ny;
    if (!side_point(gp, 0, t, phi0, r, wx, wy, &nx, &ny)) *wx = *wy = 0;
    (void)ds;
}

static bool nf_reserve(GearPair *gp, int n) {
    if (n <= gp->nf_cap) return true;
    int cap = gp->nf_cap ? 2 * gp->nf_cap : 256;
    while (cap < n) cap *= 2;
    double *R = realloc(gp->nf_R, (size_t)cap * sizeof(double)), *a = R ? realloc(gp->nf_a, (size_t)cap * sizeof(double)) : NULL;
    if (R) gp->nf_R = R;
    if (a) gp->nf_a = a;
    double *w = a ? realloc(gp->nf_w, (size_t)cap * sizeof(double)) : NULL;
    if (!w) return false;
    gp->nf_w = w, gp->nf_cap = cap;
    return true;
}

/* The near field of a line contact. The strip's pressure is spread over at least an element, so the parts' meshes
 * show the deformation of a strip of half-width a; a Hertzian strip of half-width b < a (b^2 = 4 q R / (pi E*), q the
 * load per unit length, R the relative radius of curvature) indents further by (2 q / (pi E*)) ln(a / b) (the
 * half-space's displacement under a strip of pressure, relative to the same point far away). Per contact that is a
 * compliance in series of (2 / (pi E* w)) ln(a / b) for the face width w it stands for, which depends on its own
 * load: set from the loads of the last solve (or the whole torque on one pair to begin). */
static void near_field(GearPair *gp, const double *lambda, double q_default, double Estar) {
    MContactSet *cs = &gp->cs;
    for (int j = 0; j < cs->n; j++) {
        double q = lambda && lambda[j] > 0 ? lambda[j] / gp->nf_w[j] : q_default;
        double b = sqrt(4 * q * gp->nf_R[j] / (M_PI * Estar));
        cs->cdiag[j] = gp->nf_a[j] > b ? 2 / (M_PI * Estar * gp->nf_w[j]) * log(gp->nf_a[j] / b) : 0;
    }
}

bool gearpair_solve(GearPair *gp, double roll_deg, GearPairPosition *pos, bool fields, char *err, size_t errlen) {
    memset(pos, 0, sizeof *pos);
    const GearPairSpec *S = &gp->spec;
    int z0 = S->gear[0].teeth, z1 = S->gear[1].teeth;
    double phi0 = gp->sign * roll_deg * M_PI / 180, phi1 = -phi0 * z0 / z1;
    const GearMesh *mA = &gp->mesh[0], *mB = &gp->mesh[1];
    double bw = S->gear[0].face_width_m;
    int NL = gp->nlayer[0];
    double gap_cut = S->gap_cut_m > 0 ? S->gap_cut_m : 60e-6;
    MContactSet *cs = &gp->cs;
    mcontact_clear(cs);
    int TA = mA->teeth, TB = mB->teeth;
    (void)pinion_point;
    /* The tooth pairs that may touch: each pinion tooth against the gear teeth within a pitch and a half of it, at
     * their closest approach on the exact outlines. The window of candidates opens from the position's smallest gap,
     * which includes the backlash. */
    enum { MAXPAIR = 64 };
    PairHit hit[MAXPAIR];
    int hit_t[MAXPAIR], hit_u[MAXPAIR], nhit = 0;
    double gap_floor = INFINITY;
    for (int t = 0; t < TA; t++) {
        if (S->single_tooth && t != (TA - 1) / 2) continue;
        double wx, wy, nx, ny, bx, by;
        if (!side_point(gp, 0, t, phi0, 0.5 * (gp->geo[0].right.r_form + gp->geo[0].r_a), &wx, &wy, &nx, &ny)) continue;
        rot(-phi1, wx - gp->centre[1][0], wy - gp->centre[1][1], &bx, &by);
        double pa = atan2(by, bx);
        for (int u = 0; u < TB; u++) {
            double tb = gp->angle0[1] + u * 2 * M_PI / z1;
            if (fabs(remainder(pa - tb, 2 * M_PI)) > 1.5 * 2 * M_PI / z1) continue;
            PairHit h = pair_contact(gp, t, u, phi0, phi1);
            if (!h.ok || nhit == MAXPAIR) continue;
            hit[nhit] = h, hit_t[nhit] = t, hit_u[nhit] = u, nhit++;
            gap_floor = fmin(gap_floor, h.g);
        }
    }
    if (!(gap_floor < INFINITY)) return snprintf(err, errlen, "the gears do not face each other at turn %.3f deg", roll_deg), false;
    /* Each pair's contact is a line across the face. Per layer of nodes, one contact: its force is a Hertzian strip
     * along the profile centred on the contact point, at least an element wide so that the finite elements see a
     * smooth load as the point moves past their nodes, and the gap it closes is the exact one at the contact point;
     * its share goes to the nodes around each sample point on both flanks (linear along the profile, across the face
     * by the layer's tributary width at NQZ points), and the approach is measured with the same weights. */
    enum { NKS = 9, NQZ = 4 };
    double zl[64];
    for (int l = 0; l < NL && l < 64; l++) zl[l] = bw * l / (NL - 1);
    double Estar = 1 / ((1 - S->nu[0] * S->nu[0]) / S->E[0] + (1 - S->nu[1] * S->nu[1]) / S->E[1]);
    double q_est = fabs(S->torque_nm) / (gp->geo[0].r_b * bw); /* one pair carrying the whole torque, N/m */
    for (int h = 0; h < nhit; h++) {
        if (hit[h].g > gap_floor + gap_cut) continue;
        int t = hit_t[h], u = hit_u[h];
        const PairHit *H = &hit[h];
        /* the strip's half-width: Hertz for the two involutes' curvatures (radius of curvature sqrt(r^2 - r_b^2)), or
         * three quarters of the longer of the elements under it */
        double rhoA = sqrt(fmax(0, H->rA * H->rA - gp->geo[0].r_b * gp->geo[0].r_b)), rhoB = sqrt(fmax(0, H->rB * H->rB - gp->geo[1].r_b * gp->geo[1].r_b));
        double rmin = 0.05 * S->gear[0].module_m;
        rhoA = fmax(rhoA, rmin), rhoB = fmax(rhoB, rmin);
        double Rs = rhoA * rhoB / (rhoA + rhoB), bH = sqrt(4 * q_est * Rs / (M_PI * Estar));
        const Flank *Fa = &gp->sflank[t * NL + 0], *Fb = &gp->mflank[u * NL + 0];
        double wsA, wsB;
        int ka = flank_bracket(Fa, H->rA, &wsA), kb = flank_bracket(Fb, H->rB, &wsB);
        if (ka < 0 || kb < 0) continue;
        double la = fabs(side_arc(gp, 0, Fa->r[ka + 1]) - side_arc(gp, 0, Fa->r[ka]));
        double lb = fabs(side_arc(gp, 1, Fb->r[kb + 1]) - side_arc(gp, 1, Fb->r[kb]));
        double a_k = fmax(bH, 0.75 * fmax(la, lb));
        double arcA = side_arc(gp, 0, H->rA), arcA0 = side_arc(gp, 0, gp->geo[0].right.r_form), arcA1 = gp->ocum[0][gp->geo[0].right.n - 1];
        double ks_rA[NKS], ks_rB[NKS], ks_w[NKS];
        int nks = 0;
        for (int k = 0; k < NKS; k++) {
            double sk = -1 + (2.0 * k + 1) / NKS, arc = arcA + sk * a_k, w = sqrt(fmax(0, 1 - sk * sk));
            if (arc < arcA0 || arc > arcA1) continue; /* past the pinion's flank: the strip is cut there */
            double rA = side_radius(gp, 0, arc), wx, wy, nx, ny, rB, mx, my;
            bool in;
            if (!side_point(gp, 0, t, phi0, rA, &wx, &wy, &nx, &ny)) continue;
            side_project(gp, 1, u, phi1, wx, wy, &rB, &mx, &my, &in);
            rB = fmin(fmax(rB, gp->geo[1].right.r_form), gp->geo[1].r_a); /* past the gear's tip: its corner carries it */
            ks_rA[nks] = rA, ks_rB[nks] = rB, ks_w[nks] = w, nks++;
        }
        if (nks == 0) continue;
        /* the driver's rotation moves the contact point by sign * (-(y - cy), x - cx): the gap changes by n . that */
        double dxdt = gp->sign * -(H->wy - gp->centre[0][1]), dydt = gp->sign * (H->wx - gp->centre[0][0]);
        double mm = H->nx * dxdt + H->ny * dydt;
        for (int l = 0; l < NL; l++) {
            double z_lo = l > 0 ? 0.5 * (zl[l - 1] + zl[l]) : zl[l], z_hi = l < NL - 1 ? 0.5 * (zl[l] + zl[l + 1]) : zl[l];
            int tb_[MC_MAX_TERMS], tz_[MC_MAX_TERMS], nt = 0;
            double tw_[MC_MAX_TERMS];
            bool overflow = false;
#define ADD(BODY, NODE, W) do { \
        int zz_ = gp->body[BODY].zone_of[NODE]; \
        if (zz_ < 0) break; \
        int f_ = 0; \
        while (f_ < nt && !(tb_[f_] == (BODY) && tz_[f_] == zz_)) f_++; \
        if (f_ == nt) { if (nt == MC_MAX_TERMS) { overflow = true; break; } tb_[nt] = (BODY), tz_[nt] = zz_, tw_[nt] = 0, nt++; } \
        tw_[f_] += (W); \
    } while (0)
            for (int k = 0; k < nks; k++)
                for (int iz = 0; iz < NQZ; iz++) {
                    double zq = z_lo + (z_hi - z_lo) * (iz + 0.5) / NQZ, w = ks_w[k] * (z_hi > z_lo ? (z_hi - z_lo) / NQZ : 1.0 / NQZ);
                    if (z_hi <= z_lo) zq = z_lo; /* a contact on an end face has no width: its share is weighted by length */
                    double lz = zq / bw * (NL - 1);
                    int l0 = (int)floor(lz);
                    if (l0 > NL - 2) l0 = NL - 2;
                    if (l0 < 0) l0 = 0;
                    double wz = lz - l0;
                    for (int L = 0; L < 2; L++) {
                        int lay = l0 + L;
                        double wl = L ? wz : 1 - wz, wa, wb;
                        if (wl == 0) continue;
                        const Flank *Fs = &gp->sflank[t * NL + lay], *Fm = &gp->mflank[u * NL + lay];
                        int ia = flank_bracket(Fs, ks_rA[k], &wa), ib = flank_bracket(Fm, ks_rB[k], &wb);
                        if (ia < 0 || ib < 0) continue;
                        ADD(0, Fs->node[ia], w * wl * (1 - wa));
                        ADD(0, Fs->node[ia + 1], w * wl * wa);
                        ADD(1, Fm->node[ib], w * wl * (1 - wb));
                        ADD(1, Fm->node[ib + 1], w * wl * wb);
                    }
                }
#undef ADD
            if (overflow) return snprintf(err, errlen, "a contact strip touches more than %d nodes", MC_MAX_TERMS), false;
            double sw0 = 0, sw1 = 0;
            for (int f = 0; f < nt; f++) {
                if (tb_[f]) sw1 += tw_[f];
                else sw0 += tw_[f];
            }
            if (sw0 <= 0 || sw1 <= 0) continue;
            for (int f = 0; f < nt; f++) tw_[f] /= tb_[f] ? sw1 : sw0;
            int s = gp->sflank[t * NL + l].node[ka];
            double where[3] = {H->wx, H->wy, zl[l]}, nrm[3] = {H->nx, H->ny, 0};
            double zc = 2 * zl[l] / bw - 1, crown = (S->gear[0].lead_crown_m + S->gear[1].lead_crown_m) * zc * zc; /* both flanks stand back */
            int j = mcontact_add(cs, H->g + crown, mm, nt, tb_, tz_, tw_, 0, s, where, nrm);
            if (j < 0 || !nf_reserve(gp, j + 1)) return snprintf(err, errlen, "out of memory"), false;
            gp->nf_R[j] = Rs, gp->nf_a[j] = a_k, gp->nf_w[j] = z_hi > z_lo ? z_hi - z_lo : 0.5 * bw / (NL - 1);
        }
    }
    pos->roll_deg = roll_deg;
    if (!gearpair_gauged_roll(gp, roll_deg, &pos->gauged_roll_deg)) pos->gauged_roll_deg = NAN;
    pos->contacts = cs->n;
    free(gp->lambda);
    gp->lambda = malloc((size_t)(cs->n ? cs->n : 1) * sizeof(double));
    MBody *bodies[2] = {&gp->body[0], &gp->body[1]};
    MContactResult res;
    near_field(gp, NULL, q_est, Estar);
    for (int pass = 0; pass < 3; pass++) {
        if (!mcontact_solve(bodies, 2, cs, S->torque_nm, 1e-13, gp->lambda, &res, err, errlen)) return false;
        if (pass < 2) near_field(gp, gp->lambda, q_est, Estar);
    }
    if (getenv("OPAI_MECH_DEBUG")) {
        double byg[16] = {0}, marm = 0, fsum = 0;
        for (int j = 0; j < cs->n; j++) {
            if (gp->lambda[j] <= 0) continue;
            fsum += gp->lambda[j], marm += gp->lambda[j] * cs->m[j];
            for (int t = 0; t < cs->nterm[j]; t++)
                if (cs->term_body[MC_MAX_TERMS * j + t] == 1) {
                    int nd = gp->body[1].zone_node[cs->term_zone[MC_MAX_TERMS * j + t]], tt = gp->mesh[1].tooth[nd];
                    if (tt >= 0 && tt < 16) byg[tt] += gp->lambda[j] * cs->term_w[MC_MAX_TERMS * j + t];
                }
        }
        fprintf(stderr, "  debug: %d candidates, gap floor %.2f um, force %.1f N, mean arm %.3f mm; on gear teeth:", cs->n, 1e6 * gap_floor, fsum,
                fsum > 0 ? -1e3 * marm / fsum : 0);
        for (int t = 0; t < mB->teeth && t < 16; t++) fprintf(stderr, " %.0f", byg[t]);
        fprintf(stderr, "\n");
    }
    double geom = INFINITY;
    for (int j = 0; j < cs->n; j++)
        if (cs->m[j] < 0) geom = fmin(geom, cs->g0[j] / -cs->m[j]);
    double rb = gp->geo[0].r_b;
    pos->theta_rad = res.theta, pos->theta_geom_rad = geom;
    pos->te_um = 1e6 * res.theta * rb, pos->te_elastic_um = 1e6 * (res.theta - geom) * rb;
    pos->active = res.active, pos->force_total_n = res.total_force;
    for (int j = 0; j < cs->n; j++) {
        int t = mA->tooth[cs->slave_node[j]];
        if (t >= 0 && t < 8) pos->load_by_tooth[t] += gp->lambda[j];
    }
    if (!fields && gp->ngauge == 0) return true;
    /* the fields: each part under its contact forces */
    mcontact_forces(bodies, 2, cs, gp->lambda, gp->f);
    for (int b = 0; b < 2; b++) {
        if (!mbody_solve(&gp->body[b], gp->f[b], gp->u[b])) return snprintf(err, errlen, "a part's solve failed"), false;
        mbody_fields(&gp->body[b], gp->u[b], gp->nstrain[b], NULL, gp->mises[b]);
        mbody_node_mises(&gp->body[b], gp->nstrain[b], gp->nmises[b]);
        float mx = 0;
        for (int e = 0; e < gp->mesh[b].nelems; e++) mx = fmaxf(mx, gp->mises[b][e]);
        pos->max_mises_pa[b] = mx;
        double phi = b ? phi1 : phi0;
        for (int i = 0; i < gp->mesh[b].nnodes; i++) {
            double x, y;
            rot(phi, gp->mesh[b].xyz[3 * i], gp->mesh[b].xyz[3 * i + 1], &x, &y);
            gp->world[b][3 * i] = x + gp->centre[b][0], gp->world[b][3 * i + 1] = y + gp->centre[b][1], gp->world[b][3 * i + 2] = gp->mesh[b].xyz[3 * i + 2];
            gp->contact[b][i] = (float)sqrt(gp->f[b][3 * i] * gp->f[b][3 * i] + gp->f[b][3 * i + 1] * gp->f[b][3 * i + 1]);
        }
    }
    for (int k = 0; k < gp->ngauge; k++) pos->gauge_strain[k] = gauge_read(gp, &gp->gauge[k]);
    if (getenv("OPAI_MECH_DEBUG") && gp->ngauge > 0) { /* the tangential strain along the gauge's fillet and flank */
        const Gauge *G = &gp->gauge[0];
        const Flank *F = &G->surf[0];
        const double *ns = gp->nstrain[G->body];
        double ta = gp->angle0[G->body] + G->tooth * 2 * M_PI / S->gear[G->body].teeth - M_PI / 2;
        fprintf(stderr, "  debug: gauge %s centre r %.4f mm; along its side (layer %d):\n", G->spec.name, 1e3 * G->r_centre, G->layer0);
        for (int i = 0; i < F->n; i++) {
            double x, y, tx, ty;
            if (!outline_at_radius(&gp->geo[G->body], F->r[i], &x, &y, &tx, &ty)) continue;
            double ang = atan2(fabs(tx), fabs(ty)) * 180 / M_PI; /* from the tooth's centre line */
            if (G->side == GM_LEFT) tx = -tx;
            double ox, oy;
            rot(ta, tx, ty, &ox, &oy);
            const double *e = &ns[6 * F->node[i]];
            double v = ox * ox * e[0] + oy * oy * e[1] + ox * oy * e[3];
            fprintf(stderr, "    r %8.4f mm  tangent %5.1f deg from the centre line  strain %8.1f ue\n", 1e3 * F->r[i], ang, 1e6 * v);
        }
        /* the stress state at each gauge's centre, and its strain through the face width */
        for (int k = 0; k < gp->ngauge; k++) {
            const Gauge *H = &gp->gauge[k];
            const double *hs = gp->nstrain[H->body];
            int NL = gp->nlayer[H->body], c = H->nsamp / 2;
            const double *t = H->samp_t[c];
            double E = S->E[H->body], nu = S->nu[H->body], lam = E * nu / ((1 + nu) * (1 - 2 * nu)), mu = E / (2 * (1 + nu));
            fprintf(stderr, "  debug: gauge %s through the face (layer: tangential strain ue, sigma_t MPa, sigma_z MPa, sigma_n MPa):\n", H->spec.name);
            for (int l = 0; l < NL; l++) {
                Flank Fl;
                if (!flank_collect(&gp->mesh[H->body], H->tooth, H->side, l, NL - 1, S->gear[H->body].face_width_m, true, &Fl)) continue;
                double w;
                int q = flank_bracket(&Fl, H->samp_r[c], &w);
                if (q >= 0) {
                    double e[6];
                    for (int m = 0; m < 6; m++) e[m] = (1 - w) * hs[6 * Fl.node[q] + m] + w * hs[6 * Fl.node[q + 1] + m];
                    double tr = e[0] + e[1] + e[2];
                    double sxx = lam * tr + 2 * mu * e[0], syy = lam * tr + 2 * mu * e[1], szz = lam * tr + 2 * mu * e[2], sxy = mu * e[3];
                    double nx = -t[1], ny = t[0];
                    double st = t[0] * t[0] * sxx + t[1] * t[1] * syy + 2 * t[0] * t[1] * sxy;
                    double sn = nx * nx * sxx + ny * ny * syy + 2 * nx * ny * sxy;
                    double et = t[0] * t[0] * e[0] + t[1] * t[1] * e[1] + t[0] * t[1] * e[3];
                    fprintf(stderr, "    layer %d  %8.1f  %8.1f  %8.1f  %8.1f\n", l, 1e6 * et, 1e-6 * st, 1e-6 * szz, 1e-6 * sn);
                }
                flank_free(&Fl);
            }
        }
    }
    return true;
}

/* the unloaded contact between the pinion's middle tooth and the gear's middle tooth: the point of the pinion's flank
 * (sampled finely along its exact outline) nearest the gear's flank, and the radius of its foot on the gear */
/* the gap from a point of the pinion's driving flank (radius r on its exact outline) to the gear's mating flank of its
 * middle tooth, and the radius of its foot on the gear */
static double kin_gap(GearPair *gp, double turn_deg, double r, double *rfoot, bool *inside) {
    const GearPairSpec *S = &gp->spec;
    int z0 = S->gear[0].teeth, z1 = S->gear[1].teeth;
    double phi0 = gp->sign * turn_deg * M_PI / 180, phi1 = -phi0 * z0 / z1;
    int ta_i = (gp->mesh[0].teeth - 1) / 2, tb_i = (gp->mesh[1].teeth - 1) / 2;
    double ta = gp->angle0[0] + ta_i * 2 * M_PI / z0 - M_PI / 2, tb = gp->angle0[1] + tb_i * 2 * M_PI / z1 - M_PI / 2;
    double x, y, tx, ty;
    if (!outline_at_radius(&gp->kin[0], r, &x, &y, &tx, &ty)) {
        *inside = false;
        return INFINITY;
    }
    if (gp->side[0] == GM_LEFT) x = -x;
    double px, py, wx, wy, bx, by, lx, ly;
    rot(ta, x, y, &px, &py);
    rot(phi0, px, py, &wx, &wy);
    wx += gp->centre[0][0], wy += gp->centre[0][1];
    rot(-phi1, wx - gp->centre[1][0], wy - gp->centre[1][1], &bx, &by);
    rot(-tb, bx, by, &lx, &ly);
    if (gp->side[1] == GM_LEFT) lx = -lx;
    double qx, qy, nx, ny;
    double g = outline_nearest(&gp->kin[1].right, gp->kin[1].right.form, lx, ly, &qx, &qy, &nx, &ny, inside);
    *rfoot = hypot(qx, qy);
    return g;
}

/* The unloaded contact between the pinion's middle tooth and the gear's middle tooth: the point of the pinion's flank
 * nearest the gear's flank, found on a scan of the flank's radius and refined by golden section, and the roll angle of
 * its foot on the gear. Not in mesh (false) when that gap is more than the backlash the thinning leaves plus 10 um. */
static bool nearest_roll(GearPair *gp, double turn_deg, double *roll_deg) {
    const GearGeometry *gA = &gp->kin[0];
    double r0 = gA->right.r_form, r1 = gA->r_a;
    int n = 120, bi = -1;
    double best = INFINITY, rf;
    bool in;
    for (int i = 0; i <= n; i++) {
        double r = r0 + (r1 - r0) * i / n, g = kin_gap(gp, turn_deg, r, &rf, &in);
        if (in && g < best) best = g, bi = i;
    }
    if (bi < 0) return false;
    double a = r0 + (r1 - r0) * fmax(0, bi - 1) / n, b = r0 + (r1 - r0) * fmin(n, bi + 1) / n;
    const double phi = 0.6180339887498949;
    double c = b - phi * (b - a), d = a + phi * (b - a), gc = kin_gap(gp, turn_deg, c, &rf, &in), gd = kin_gap(gp, turn_deg, d, &rf, &in);
    for (int it = 0; it < 80; it++) {
        if (gc < gd) b = d, d = c, gd = gc, c = b - phi * (b - a), gc = kin_gap(gp, turn_deg, c, &rf, &in);
        else a = c, c = d, gc = gd, d = a + phi * (b - a), gd = kin_gap(gp, turn_deg, d, &rf, &in);
    }
    double ropt = 0.5 * (a + b), g = kin_gap(gp, turn_deg, ropt, &rf, &in);
    /* the backlash: the flanks' thinning, and a centre distance beyond the tight-mesh one (each mm of it opens the
     * flanks by about 2 sin(alpha_w) mm along the normal, shared by the two sides) */
    double cd = hypot(gp->centre[1][0] - gp->centre[0][0], gp->centre[1][1] - gp->centre[0][1]);
    double a0 = gear_centre_distance(&gp->spec.gear[0], &gp->spec.gear[1]), rb_sum = gp->geo[0].r_b + gp->geo[1].r_b;
    double aw = acos(fmin(1, rb_sum / cd)), backlash = gp->spec.gear[0].thinning_m + gp->spec.gear[1].thinning_m + fmax(0, cd - a0) * 2 * sin(aw);
    if (!in || g > backlash + 10e-6) return false;
    if (ropt - r0 < 1e-3 * (r1 - r0) || r1 - ropt < 1e-3 * (r1 - r0)) return false; /* at an end: an edge, not conjugate action */
    double u = rf / gp->kin[1].r_b;
    *roll_deg = sqrt(fmax(0, u * u - 1)) * 180 / M_PI;
    return true;
}

/* Perfect involutes keep the contact on the line of action, so the gauged tooth's roll angle falls by exactly z0/z1
 * degrees per degree of the pinion's turn; only where the line starts needs measuring. It is fitted once, over every
 * position in mesh, to the nearest-point estimates, whose small scatter (the outline is a fine polyline) averages out. */
static bool roll_line(GearPair *gp) {
    if (gp->roll_known) return true;
    const GearPairSpec *S = &gp->spec;
    double k = (double)S->gear[0].teeth / S->gear[1].teeth * gp->sign, pitch = 360.0 / S->gear[0].teeth, sum = 0;
    int n = 0;
    for (double t = -2 * pitch; t <= 2 * pitch; t += pitch / 40) {
        double r;
        if (nearest_roll(gp, t, &r)) sum += r + k * t, n++;
    }
    if (n < 5) return false;
    gp->roll_ref = sum / n, gp->roll_known = true;
    return true;
}

bool gearpair_gauged_roll(GearPair *gp, double turn_deg, double *roll_deg) {
    double r;
    if (!roll_line(gp) || !nearest_roll(gp, turn_deg, &r)) return false; /* in mesh at all */
    double k = (double)gp->spec.gear[0].teeth / gp->spec.gear[1].teeth * gp->sign;
    *roll_deg = gp->roll_ref - k * turn_deg;
    return true;
}

bool gearpair_turn_for_roll(GearPair *gp, double roll_deg, double *turn_deg, char *err, size_t errlen) {
    if (!roll_line(gp)) return snprintf(err, errlen, "the middle teeth never meet: check the gears' alignment"), false;
    double k = (double)gp->spec.gear[0].teeth / gp->spec.gear[1].teeth * gp->sign, t = (gp->roll_ref - roll_deg) / k, r;
    if (!nearest_roll(gp, t, &r)) return snprintf(err, errlen, "roll angle %.3f deg is outside the gear's working flank", roll_deg), false;
    *turn_deg = t;
    return true;
}

void gearpair_frame(GearPair *gp, int body, const double **xyz_world, int *nnodes, const int **conn, int *nelems, const float **mises,
                    const float **contact, const float **node_mises) {
    *xyz_world = gp->world[body], *nnodes = gp->mesh[body].nnodes, *conn = gp->mesh[body].conn, *nelems = gp->mesh[body].nelems;
    *mises = gp->mises[body], *contact = gp->contact[body], *node_mises = gp->nmises[body];
}
