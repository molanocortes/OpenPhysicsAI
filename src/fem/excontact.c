/* excontact.c - node-to-surface penalty contact for the explicit path (excontact.h) */
#include "excontact.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Facet {
    int n[4];
    int elem;
} Facet;

struct ExContact {
    int nnodes, nelems;
    const double *xyz;
    const int *conn;
    ExContactOptions opt;
    ExPlane *planes;
    int nplanes;
    Facet *facets;
    int nfacets;
    int *snode;    /* the surface nodes */
    int nsnode;
    int *sindex;   /* node -> surface index, -1 when interior */
    double *kn;    /* per surface node */
    double *cn;    /* the normal dashpot of the same node */
    double *mass;  /* the lumped mass of the surface nodes */
    double *slip;  /* 3 * nsnode * (nplanes + 1): one tangential spring per plane, the last one for self-contact */
    int *self_facet;
    int *excl_off, *excl;  /* per surface node, the elements too close in the mesh to be contact partners */
    double cell, facet_size;
    int grid[3];
    double origin[3];
    int *bucket, *next_facet;
    int nbuckets, last_build;
    double energy, friction_work, plate_work, damping_work, max_pen, max_freq;
    double impulse[3], force[3], patch_r, peak_p;
    double *area; /* each surface node's share of the free surface */
    int passed, active;
};

/* the six faces of a hex8, each outward for the usual node ordering; the sign is checked per element anyway */
static const int HEX_FACE[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};

static void v_sub(const double a[3], const double b[3], double r[3]) { for (int i = 0; i < 3; i++) r[i] = a[i] - b[i]; }
static double v_dot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void v_cross(const double a[3], const double b[3], double r[3]) {
    r[0] = a[1] * b[2] - a[2] * b[1], r[1] = a[2] * b[0] - a[0] * b[2], r[2] = a[0] * b[1] - a[1] * b[0];
}
static double v_norm(const double a[3]) { return sqrt(v_dot(a, a)); }

typedef struct FaceKey {
    int key[4];
    int elem, face;
} FaceKey;

static int cmp_face(const void *A, const void *B) {
    const FaceKey *a = A, *b = B;
    for (int i = 0; i < 4; i++)
        if (a->key[i] != b->key[i]) return a->key[i] < b->key[i] ? -1 : 1;
    return 0;
}

/* the faces no second element shares, oriented outward from their element */
static bool build_surface(ExContact *c, char *err, size_t errlen) {
    size_t nf = 6 * (size_t)c->nelems;
    FaceKey *keys = malloc(nf * sizeof *keys);
    if (!keys) {
        snprintf(err, errlen, "out of memory extracting the contact surface");
        return false;
    }
    for (int e = 0; e < c->nelems; e++)
        for (int f = 0; f < 6; f++) {
            FaceKey *k = keys + 6 * (size_t)e + (size_t)f;
            for (int i = 0; i < 4; i++) k->key[i] = c->conn[8 * (size_t)e + (size_t)HEX_FACE[f][i]];
            for (int i = 0; i < 4; i++) /* sorted, so the two copies of a shared face match */
                for (int j = i + 1; j < 4; j++)
                    if (k->key[j] < k->key[i]) {
                        int t = k->key[i];
                        k->key[i] = k->key[j], k->key[j] = t;
                    }
            k->elem = e, k->face = f;
        }
    qsort(keys, nf, sizeof *keys, cmp_face);
    c->facets = malloc((nf ? nf : 1) * sizeof *c->facets);
    if (!c->facets) {
        free(keys);
        snprintf(err, errlen, "out of memory extracting the contact surface");
        return false;
    }
    c->nfacets = 0;
    for (size_t i = 0; i < nf;) {
        size_t j = i + 1;
        while (j < nf && cmp_face(keys + i, keys + j) == 0) j++;
        if (j - i == 1) { /* a free face */
            Facet *fc = c->facets + c->nfacets++;
            fc->elem = keys[i].elem;
            for (int a = 0; a < 4; a++) fc->n[a] = c->conn[8 * (size_t)fc->elem + (size_t)HEX_FACE[keys[i].face][a]];
            /* orient it away from the element centre, whatever the mesh's node ordering is */
            double ctr[3] = {0, 0, 0}, fctr[3] = {0, 0, 0}, e1[3], e2[3], nrm[3], d[3];
            for (int a = 0; a < 8; a++)
                for (int k = 0; k < 3; k++) ctr[k] += c->xyz[3 * (size_t)c->conn[8 * (size_t)fc->elem + (size_t)a] + (size_t)k] / 8;
            for (int a = 0; a < 4; a++)
                for (int k = 0; k < 3; k++) fctr[k] += c->xyz[3 * (size_t)fc->n[a] + (size_t)k] / 4;
            v_sub(c->xyz + 3 * (size_t)fc->n[1], c->xyz + 3 * (size_t)fc->n[0], e1);
            v_sub(c->xyz + 3 * (size_t)fc->n[3], c->xyz + 3 * (size_t)fc->n[0], e2);
            v_cross(e1, e2, nrm);
            v_sub(fctr, ctr, d);
            if (v_dot(nrm, d) < 0) {
                int t = fc->n[1];
                fc->n[1] = fc->n[3], fc->n[3] = t;
            }
        }
        i = j;
    }
    free(keys);
    return true;
}

static double facet_area_normal(const ExContact *c, const Facet *f, const double *x, double nrm[3], double ctr[3]) {
    double e1[3], e2[3];
    for (int k = 0; k < 3; k++) ctr[k] = 0;
    for (int a = 0; a < 4; a++)
        for (int k = 0; k < 3; k++) ctr[k] += x[3 * (size_t)f->n[a] + (size_t)k] / 4;
    v_sub(x + 3 * (size_t)f->n[2], x + 3 * (size_t)f->n[0], e1);
    v_sub(x + 3 * (size_t)f->n[3], x + 3 * (size_t)f->n[1], e2);
    v_cross(e1, e2, nrm);
    double len = v_norm(nrm);
    if (len > 0)
        for (int k = 0; k < 3; k++) nrm[k] /= len;
    return 0.5 * len; /* the area of the quad from its diagonals */
}

/* the closest point of the triangle (a, b, cc) to p, with its barycentric weights */
static void closest_on_triangle(const double p[3], const double a[3], const double b[3], const double cc[3], double out[3], double w[3]) {
    double ab[3], ac[3], ap[3];
    v_sub(b, a, ab), v_sub(cc, a, ac), v_sub(p, a, ap);
    double d1 = v_dot(ab, ap), d2 = v_dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) {
        memcpy(out, a, 3 * sizeof(double)), w[0] = 1, w[1] = w[2] = 0;
        return;
    }
    double bp[3];
    v_sub(p, b, bp);
    double d3 = v_dot(ab, bp), d4 = v_dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) {
        memcpy(out, b, 3 * sizeof(double)), w[1] = 1, w[0] = w[2] = 0;
        return;
    }
    double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        double v = d1 / (d1 - d3);
        for (int k = 0; k < 3; k++) out[k] = a[k] + v * ab[k];
        w[0] = 1 - v, w[1] = v, w[2] = 0;
        return;
    }
    double cp[3];
    v_sub(p, cc, cp);
    double d5 = v_dot(ab, cp), d6 = v_dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) {
        memcpy(out, cc, 3 * sizeof(double)), w[2] = 1, w[0] = w[1] = 0;
        return;
    }
    double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        double v = d2 / (d2 - d6);
        for (int k = 0; k < 3; k++) out[k] = a[k] + v * ac[k];
        w[0] = 1 - v, w[1] = 0, w[2] = v;
        return;
    }
    double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        double v = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        for (int k = 0; k < 3; k++) out[k] = b[k] + v * (cc[k] - b[k]);
        w[0] = 0, w[1] = 1 - v, w[2] = v;
        return;
    }
    double den = 1.0 / (va + vb + vc), v = vb * den, ww = vc * den;
    for (int k = 0; k < 3; k++) out[k] = a[k] + ab[k] * v + ac[k] * ww;
    w[0] = 1 - v - ww, w[1] = v, w[2] = ww;
}

void excontact_free(ExContact *c) {
    if (!c) return;
    free(c->planes), free(c->facets), free(c->snode), free(c->sindex), free(c->kn), free(c->cn), free(c->mass), free(c->area), free(c->slip);
    free(c->self_facet), free(c->excl_off), free(c->excl), free(c->bucket), free(c->next_facet);
    free(c);
}

ExContact *excontact_create(int nnodes, int nelems, const double *xyz, const int *conn, const double *mass, double bulk,
                            const ExPlane *planes, int nplanes, const ExContactOptions *opt, char *err, size_t errlen) {
    ExContact *c = calloc(1, sizeof *c);
    if (!c) {
        snprintf(err, errlen, "out of memory for the contact");
        return NULL;
    }
    c->nnodes = nnodes, c->nelems = nelems, c->xyz = xyz, c->conn = conn;
    if (opt) c->opt = *opt;
    if (!(c->opt.scale > 0)) c->opt.scale = 0.1;
    if (c->opt.rebuild_every <= 0) c->opt.rebuild_every = 10;
    if (c->opt.damping < 0) c->opt.damping = 0;
    c->last_build = -1000000;
    if (nplanes > 0) {
        c->planes = malloc((size_t)nplanes * sizeof *c->planes);
        if (!c->planes) {
            excontact_free(c);
            snprintf(err, errlen, "out of memory for the contact");
            return NULL;
        }
        memcpy(c->planes, planes, (size_t)nplanes * sizeof *c->planes);
        for (int p = 0; p < nplanes; p++) { /* a normal that is not unit would scale every gap */
            double len = v_norm(c->planes[p].normal);
            if (!(len > 0)) {
                excontact_free(c);
                snprintf(err, errlen, "contact plane %d has no normal", p);
                return NULL;
            }
            for (int k = 0; k < 3; k++) c->planes[p].normal[k] /= len;
        }
        c->nplanes = nplanes;
    }
    if (!build_surface(c, err, errlen)) {
        excontact_free(c);
        return NULL;
    }
    c->sindex = malloc((size_t)nnodes * sizeof(int));
    c->snode = malloc((size_t)nnodes * sizeof(int));
    if (!c->sindex || !c->snode) {
        excontact_free(c);
        snprintf(err, errlen, "out of memory for the contact");
        return NULL;
    }
    for (int i = 0; i < nnodes; i++) c->sindex[i] = -1;
    for (int f = 0; f < c->nfacets; f++)
        for (int a = 0; a < 4; a++)
            if (c->sindex[c->facets[f].n[a]] < 0) {
                c->sindex[c->facets[f].n[a]] = c->nsnode;
                c->snode[c->nsnode++] = c->facets[f].n[a];
            }
    c->kn = calloc((size_t)(c->nsnode ? c->nsnode : 1), sizeof(double));
    c->cn = calloc((size_t)(c->nsnode ? c->nsnode : 1), sizeof(double));
    c->mass = calloc((size_t)(c->nsnode ? c->nsnode : 1), sizeof(double));
    c->area = calloc((size_t)(c->nsnode ? c->nsnode : 1), sizeof(double));
    c->slip = calloc(3 * (size_t)(c->nsnode ? c->nsnode : 1) * (size_t)(c->nplanes + 1), sizeof(double));
    c->self_facet = malloc((size_t)(c->nsnode ? c->nsnode : 1) * sizeof(int));
    if (!c->kn || !c->cn || !c->mass || !c->area || !c->slip || !c->self_facet) {
        excontact_free(c);
        snprintf(err, errlen, "out of memory for the contact");
        return NULL;
    }
    for (int i = 0; i < c->nsnode; i++) c->self_facet[i] = -1;
    /* the penalty stiffness: scale * K * A / h per facet, h the thickness of the element behind it */
    double area_sum = 0;
    for (int f = 0; f < c->nfacets; f++) {
        double nrm[3], ctr[3], vol = 0;
        double A = facet_area_normal(c, c->facets + f, c->xyz, nrm, ctr);
        { /* the element's volume, from its eight corners about its centre */
            const int *cn = c->conn + 8 * (size_t)c->facets[f].elem;
            double ec[3] = {0, 0, 0};
            for (int a = 0; a < 8; a++)
                for (int k = 0; k < 3; k++) ec[k] += c->xyz[3 * (size_t)cn[a] + (size_t)k] / 8;
            for (int ff = 0; ff < 6; ff++) { /* the volume as the sum of the pyramids over its faces */
                double fn[3], fc[3];
                Facet t = {{cn[HEX_FACE[ff][0]], cn[HEX_FACE[ff][1]], cn[HEX_FACE[ff][2]], cn[HEX_FACE[ff][3]]}, 0};
                double a2 = facet_area_normal(c, &t, c->xyz, fn, fc);
                double d[3];
                v_sub(fc, ec, d);
                vol += fabs(v_dot(d, fn)) * a2 / 3;
            }
        }
        double h = A > 0 ? vol / A : 0;
        double k = h > 0 ? c->opt.scale * bulk * A / h : 0;
        for (int a = 0; a < 4; a++) {
            c->kn[c->sindex[c->facets[f].n[a]]] += k / 4;
            c->area[c->sindex[c->facets[f].n[a]]] += A / 4;
        }
        area_sum += A;
    }
    { /* Elements within two rings of a surface node can never be its contact partner: a node on the side of a strip
       * sits half a thickness from its own top and bottom faces, and without this the surface contacts itself where
       * it is merely thin. Two rings, because a node's own elements and their neighbours share its surface. */
        int *cnt = calloc((size_t)nnodes + 1, sizeof(int));
        int *n2e_off = NULL, *n2e = NULL;
        if (cnt) {
            for (int e = 0; e < nelems; e++)
                for (int a = 0; a < 8; a++) cnt[conn[8 * (size_t)e + (size_t)a] + 1]++;
            for (int i = 0; i < nnodes; i++) cnt[i + 1] += cnt[i];
            n2e_off = malloc(((size_t)nnodes + 1) * sizeof(int));
            n2e = malloc((size_t)(8 * (size_t)nelems ? 8 * (size_t)nelems : 1) * sizeof(int));
            if (n2e_off && n2e) {
                memcpy(n2e_off, cnt, ((size_t)nnodes + 1) * sizeof(int));
                int *fill = malloc((size_t)nnodes * sizeof(int));
                if (fill) {
                    memcpy(fill, cnt, (size_t)nnodes * sizeof(int));
                    for (int e = 0; e < nelems; e++)
                        for (int a = 0; a < 8; a++) n2e[fill[conn[8 * (size_t)e + (size_t)a]]++] = e;
                    free(fill);
                }
            }
        }
        free(cnt);
        c->excl_off = malloc(((size_t)c->nsnode + 1) * sizeof(int));
        int cap = 64 * (c->nsnode ? c->nsnode : 1), used = 0;
        c->excl = malloc((size_t)cap * sizeof(int));
        unsigned char *seen = calloc((size_t)nelems ? (size_t)nelems : 1, 1);
        if (n2e_off && n2e && c->excl_off && c->excl && seen) {
            for (int i = 0; i < c->nsnode; i++) {
                c->excl_off[i] = used;
                int n = c->snode[i], start = used;
                for (int p1 = n2e_off[n]; p1 < n2e_off[n + 1]; p1++) { /* the elements of the node, then of their nodes */
                    int e1 = n2e[p1];
                    for (int a = 0; a < 8; a++) {
                        int m = conn[8 * (size_t)e1 + (size_t)a];
                        for (int p2 = n2e_off[m]; p2 < n2e_off[m + 1]; p2++) {
                            int e2 = n2e[p2];
                            if (seen[e2]) continue;
                            seen[e2] = 1;
                            if (used == cap) {
                                cap *= 2;
                                int *bigger = realloc(c->excl, (size_t)cap * sizeof(int));
                                if (!bigger) break;
                                c->excl = bigger;
                            }
                            c->excl[used++] = e2;
                        }
                    }
                }
                for (int p = start; p < used; p++) seen[c->excl[p]] = 0;
                for (int p = start; p < used; p++) /* sorted, so the search can bisect */
                    for (int q = p + 1; q < used; q++)
                        if (c->excl[q] < c->excl[p]) {
                            int t = c->excl[p];
                            c->excl[p] = c->excl[q], c->excl[q] = t;
                        }
            }
            c->excl_off[c->nsnode] = used;
        } else if (c->excl_off) {
            for (int i = 0; i <= c->nsnode; i++) c->excl_off[i] = 0;
        }
        free(seen), free(n2e_off), free(n2e);
    }
    c->facet_size = c->nfacets ? sqrt(area_sum / c->nfacets) : 0;
    if (!(c->opt.depth_limit > 0)) c->opt.depth_limit = 0.5 * c->facet_size;
    c->max_freq = 0;
    for (int i = 0; i < c->nsnode; i++) {
        double m = mass[c->snode[i]];
        c->mass[i] = m;
        if (m > 0 && c->kn[i] > 0) {
            c->max_freq = fmax(c->max_freq, sqrt(c->kn[i] / m));
            c->cn[i] = 2 * c->opt.damping * sqrt(c->kn[i] * m);
        }
    }
    /* A spring against a rigid surface sees the node's mass; a spring between two pieces of the same deforming body
     * sees their reduced mass, and the facet's share is spread over four nodes by the shape weights. Both make the
     * contact stiffer than sqrt(k/m) suggests, and a step taken from that bound blew up on the folded strip. The
     * factor two below covers the reduced mass (sqrt 2) with the same margin again for the weights. */
    if (c->opt.self_contact) c->max_freq *= 2;
    c->cell = 2 * c->facet_size;
    return c;
}

bool excontact_set_plane(ExContact *c, int index, const ExPlane *plane) {
    if (!c || index < 0 || index >= c->nplanes) return false;
    c->planes[index] = *plane;
    double len = v_norm(c->planes[index].normal);
    if (!(len > 0)) return false;
    for (int k = 0; k < 3; k++) c->planes[index].normal[k] /= len;
    return true;
}

int excontact_nfacets(const ExContact *c) { return c->nfacets; }
int excontact_nsurface_nodes(const ExContact *c) { return c->nsnode; }
double excontact_max_frequency(const ExContact *c) { return c->max_freq; }
double excontact_energy(const ExContact *c) { return c->energy; }
double excontact_friction_work(const ExContact *c) { return c->friction_work; }
double excontact_plate_work(const ExContact *c) { return c->plate_work; }
double excontact_damping_work(const ExContact *c) { return c->damping_work; }
void excontact_impulse(const ExContact *c, double out[3]) { memcpy(out, c->impulse, sizeof c->impulse); }
void excontact_force(const ExContact *c, double out[3]) { memcpy(out, c->force, sizeof c->force); }
double excontact_patch_radius(const ExContact *c) { return c->patch_r; }
double excontact_peak_pressure(const ExContact *c) { return c->peak_p; }
double excontact_max_penetration(const ExContact *c) { return c->max_pen; }
int excontact_passed_through(const ExContact *c) { return c->passed; }
int excontact_active(const ExContact *c) { return c->active; }

static void rebuild_buckets(ExContact *c, const double *x) {
    double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (int i = 0; i < c->nsnode; i++)
        for (int k = 0; k < 3; k++) {
            double v = x[3 * (size_t)c->snode[i] + (size_t)k];
            lo[k] = fmin(lo[k], v), hi[k] = fmax(hi[k], v);
        }
    if (!(c->cell > 0)) return;
    int total = 1;
    for (int k = 0; k < 3; k++) {
        c->origin[k] = lo[k] - c->cell;
        c->grid[k] = (int)((hi[k] - lo[k]) / c->cell) + 3;
        if (c->grid[k] < 1) c->grid[k] = 1;
        total *= c->grid[k];
    }
    if (total > 4000000) { /* a grid that large would cost more than the search saves */
        c->cell *= 2;
        rebuild_buckets(c, x);
        return;
    }
    if (total != c->nbuckets) {
        free(c->bucket);
        c->bucket = malloc((size_t)total * sizeof(int));
        c->nbuckets = total;
    }
    if (!c->next_facet) c->next_facet = malloc((size_t)(c->nfacets ? c->nfacets : 1) * sizeof(int));
    if (!c->bucket || !c->next_facet) return;
    for (int i = 0; i < total; i++) c->bucket[i] = -1;
    for (int f = 0; f < c->nfacets; f++) {
        double nrm[3], ctr[3];
        facet_area_normal(c, c->facets + f, x, nrm, ctr);
        int ix[3];
        for (int k = 0; k < 3; k++) {
            ix[k] = (int)((ctr[k] - c->origin[k]) / c->cell);
            if (ix[k] < 0) ix[k] = 0;
            if (ix[k] >= c->grid[k]) ix[k] = c->grid[k] - 1;
        }
        int b = (ix[2] * c->grid[1] + ix[1]) * c->grid[0] + ix[0];
        c->next_facet[f] = c->bucket[b];
        c->bucket[b] = f;
    }
}

void excontact_forces(ExContact *c, const double *x, const double *v, double dt, int step, double *f) {
    c->energy = 0, c->active = 0, c->patch_r = 0, c->peak_p = 0;
    c->force[0] = c->force[1] = c->force[2] = 0;
    double mu_body = c->opt.friction;
    /* the rigid planes */
    for (int p = 0; p < c->nplanes; p++) {
        const ExPlane *pl = c->planes + p;
        double mu = fmin(pl->friction, mu_body > 0 ? mu_body : pl->friction); /* the pair rule of src/mech/contact.h */
        for (int i = 0; i < c->nsnode; i++) {
            int n = c->snode[i];
            double d[3];
            v_sub(x + 3 * (size_t)n, pl->point, d);
            double gap = v_dot(d, pl->normal);
            double *s = c->slip + 3 * ((size_t)p * (size_t)c->nsnode + (size_t)i);
            if (gap >= 0) {
                s[0] = s[1] = s[2] = 0;
                continue;
            }
            double g = -gap, kn = c->kn[i], fn = kn * g;
            c->max_pen = fmax(c->max_pen, g);
            if (g > c->opt.depth_limit) c->passed++;
            c->active++;
            c->energy += 0.5 * kn * g * g;
            /* Coulomb friction with a tangential spring */
            double vr[3], vt[3];
            for (int k = 0; k < 3; k++) vr[k] = v[3 * (size_t)n + (size_t)k] - pl->velocity[k];
            double vn = v_dot(vr, pl->normal);
            if (c->cn[i] > 0) { /* the dashpot, never pulling the node back onto the surface */
                double fd = -c->cn[i] * vn;
                if (fn + fd < 0) fd = -fn;
                fn += fd;
                c->damping_work += -fd * vn * dt;
            }
            for (int k = 0; k < 3; k++) f[3 * (size_t)n + (size_t)k] += fn * pl->normal[k];
            for (int k = 0; k < 3; k++) vt[k] = vr[k] - vn * pl->normal[k];
            double stored0 = 0.5 * kn * v_dot(s, s);
            for (int k = 0; k < 3; k++) s[k] += vt[k] * dt;
            double sn = v_dot(s, pl->normal);
            for (int k = 0; k < 3; k++) s[k] -= sn * pl->normal[k];
            double ft[3];
            for (int k = 0; k < 3; k++) ft[k] = -kn * s[k];
            double ftn = v_norm(ft), limit = mu * fn;
            if (ftn > limit && ftn > 0) { /* sliding: hold the force at the Coulomb limit and pull the spring back */
                for (int k = 0; k < 3; k++) ft[k] *= limit / ftn, s[k] = -ft[k] / kn;
            }
            for (int k = 0; k < 3; k++) f[3 * (size_t)n + (size_t)k] += ft[k];
            double stored1 = 0.5 * kn * v_dot(s, s);
            c->energy += stored1;
            c->friction_work += -v_dot(ft, vt) * dt - (stored1 - stored0);
            /* a plane that moves feeds the springs: that is external work, and the balance has to see it */
            double f_tot[3];
            for (int k = 0; k < 3; k++) f_tot[k] = fn * pl->normal[k] + ft[k];
            c->plate_work += v_dot(f_tot, pl->velocity) * dt;
            for (int k = 0; k < 3; k++) c->impulse[k] += f_tot[k] * dt, c->force[k] += f_tot[k];
            { /* the patch: how far from the deepest point contact reaches, and the pressure there */
                double r2 = 0, pr[3];
                v_sub(x + 3 * (size_t)n, pl->point, pr);
                double along = v_dot(pr, pl->normal);
                for (int k = 0; k < 3; k++) {
                    double t = pr[k] - along * pl->normal[k];
                    r2 += t * t;
                }
                c->patch_r = fmax(c->patch_r, sqrt(r2));
                if (c->area[i] > 0) c->peak_p = fmax(c->peak_p, fn / c->area[i]);
            }
        }
    }
    if (!c->opt.self_contact || c->nfacets == 0) return;
    if (step - c->last_build >= c->opt.rebuild_every) {
        rebuild_buckets(c, x);
        c->last_build = step;
    }
    if (!c->bucket) return;
    for (int i = 0; i < c->nsnode; i++) {
        int n = c->snode[i];
        const double *xp = x + 3 * (size_t)n;
        int ix[3];
        for (int k = 0; k < 3; k++) {
            ix[k] = (int)((xp[k] - c->origin[k]) / c->cell);
            if (ix[k] < 0 || ix[k] >= c->grid[k]) ix[k] = ix[k] < 0 ? 0 : c->grid[k] - 1;
        }
        double best = -INFINITY, bw[4] = {0, 0, 0, 0}, bnrm[3] = {0, 0, 0};
        int bf = -1;
        for (int dz = -1; dz <= 1; dz++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int jx = ix[0] + dx, jy = ix[1] + dy, jz = ix[2] + dz;
                    if (jx < 0 || jy < 0 || jz < 0 || jx >= c->grid[0] || jy >= c->grid[1] || jz >= c->grid[2]) continue;
                    for (int fi = c->bucket[(jz * c->grid[1] + jy) * c->grid[0] + jx]; fi >= 0; fi = c->next_facet[fi]) {
                        const Facet *fc = c->facets + fi;
                        bool own = false;
                        for (int a = 0; a < 4; a++) own = own || fc->n[a] == n;
                        if (own) continue; /* a surface does not contact itself across its own corner */
                        if (c->excl_off) { /* nor within two element rings of the node */
                            int lo2 = c->excl_off[i], hi2 = c->excl_off[i + 1] - 1;
                            while (lo2 <= hi2) {
                                int mid = (lo2 + hi2) / 2;
                                if (c->excl[mid] == fc->elem) {
                                    own = true;
                                    break;
                                }
                                if (c->excl[mid] < fc->elem) lo2 = mid + 1;
                                else hi2 = mid - 1;
                            }
                            if (own) continue;
                        }
                        double nrm[3], ctr[3];
                        facet_area_normal(c, fc, x, nrm, ctr);
                        double cp1[3], cp2[3], w1[3], w2[3];
                        closest_on_triangle(xp, x + 3 * (size_t)fc->n[0], x + 3 * (size_t)fc->n[1], x + 3 * (size_t)fc->n[2], cp1, w1);
                        closest_on_triangle(xp, x + 3 * (size_t)fc->n[0], x + 3 * (size_t)fc->n[2], x + 3 * (size_t)fc->n[3], cp2, w2);
                        double d1[3], d2[3];
                        v_sub(xp, cp1, d1), v_sub(xp, cp2, d2);
                        bool first = v_norm(d1) <= v_norm(d2);
                        const double *cp = first ? cp1 : cp2;
                        double d[3];
                        v_sub(xp, cp, d);
                        double gap = v_dot(d, nrm);
                        if (gap >= 0) continue;
                        if (v_norm(d) > c->opt.depth_limit) { /* too deep to be this node's partner any more */
                            if (c->self_facet[i] == fi) c->passed++; /* it was touching this facet: it went through it */
                            continue;
                        }
                        if (gap > best) {
                            best = gap, bf = fi;
                            memcpy(bnrm, nrm, sizeof bnrm);
                            if (first) bw[0] = w1[0], bw[1] = w1[1], bw[2] = w1[2], bw[3] = 0;
                            else bw[0] = w2[0], bw[1] = 0, bw[2] = w2[1], bw[3] = w2[2];
                        }
                    }
                }
        double *s = c->slip + 3 * ((size_t)c->nplanes * (size_t)c->nsnode + (size_t)i);
        if (bf < 0) {
            c->self_facet[i] = -1, s[0] = s[1] = s[2] = 0;
            continue;
        }
        if (c->self_facet[i] != bf) s[0] = s[1] = s[2] = 0; /* a new partner starts with no stored slip */
        c->self_facet[i] = bf;
        const Facet *fc = c->facets + bf;
        double g = -best, kn = c->kn[i], fn = kn * g;
        c->max_pen = fmax(c->max_pen, g);
        c->active++;
        for (int k = 0; k < 3; k++) {
            f[3 * (size_t)n + (size_t)k] += fn * bnrm[k];
            for (int a = 0; a < 4; a++) f[3 * (size_t)fc->n[a] + (size_t)k] -= bw[a] * fn * bnrm[k];
        }
        c->energy += 0.5 * kn * g * g;
        double vr[3] = {0, 0, 0}, vt[3];
        for (int k = 0; k < 3; k++) {
            vr[k] = v[3 * (size_t)n + (size_t)k];
            for (int a = 0; a < 4; a++) vr[k] -= bw[a] * v[3 * (size_t)fc->n[a] + (size_t)k];
        }
        double vn = v_dot(vr, bnrm);
        for (int k = 0; k < 3; k++) vt[k] = vr[k] - vn * bnrm[k];
        double stored0 = 0.5 * kn * v_dot(s, s);
        for (int k = 0; k < 3; k++) s[k] += vt[k] * dt;
        double sn = v_dot(s, bnrm);
        for (int k = 0; k < 3; k++) s[k] -= sn * bnrm[k];
        double ft[3];
        for (int k = 0; k < 3; k++) ft[k] = -kn * s[k];
        double ftn = v_norm(ft), limit = mu_body * fn;
        if (ftn > limit && ftn > 0)
            for (int k = 0; k < 3; k++) ft[k] *= limit / ftn, s[k] = -ft[k] / kn;
        for (int k = 0; k < 3; k++) {
            f[3 * (size_t)n + (size_t)k] += ft[k];
            for (int a = 0; a < 4; a++) f[3 * (size_t)fc->n[a] + (size_t)k] -= bw[a] * ft[k];
        }
        double stored1 = 0.5 * kn * v_dot(s, s);
        c->energy += stored1;
        c->friction_work += -v_dot(ft, vt) * dt - (stored1 - stored0);
    }
}
