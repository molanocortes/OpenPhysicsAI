/* acoustic.c - see acoustic.h. */
#include "acoustic.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../threads.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct BNode {
    long idx;
    unsigned char missing; /* bit d set: the neighbour in direction d (x-, x+, y-, y+, z-, z+) is not air */
    double beta;           /* sum over the missing directions of lambda / xi */
} BNode;

struct Acoustic {
    AcSpec spec;
    int nx, ny, nz;
    long n;
    double dt, lambda, t;
    long steps;
    double *p0, *p1;        /* p at step n-1 (overwritten by n+1) and at step n */
    unsigned char *solid;   /* 1 inside an obstacle */
    unsigned char *material;/* of a solid cell */
    unsigned char *kind;    /* 0 interior air, 1 boundary air, 2 solid */
    BNode *bnodes;
    long nbnodes;
    long src_idx[AC_MAX_SOURCES];
    double src_gain[AC_MAX_SOURCES];
    double *traces;         /* receivers x capacity */
    long *pist, npist;      /* the piston's boundary nodes (indices into bnodes) */
    double pist_gain, pist_acc;
    long trace_cap;
    ThreadPool *pool;
};

void ac_spec_defaults(AcSpec *s) {
    memset(s, 0, sizeof *s);
    s->c = 343.2;
    s->rho = 1.204;
    s->dx = 0.05;
    s->nmaterials = 1;
    s->xi[0] = 0; /* rigid */
    snprintf(s->material_name[0], sizeof s->material_name[0], "rigid");
}

static inline long at(const Acoustic *a, int i, int j, int k) { return ((long)k * a->ny + j) * a->nx + i; }

static double material_beta(const Acoustic *a, int m) {
    double xi = (m >= 0 && m < a->spec.nmaterials) ? a->spec.xi[m] : 0;
    if (!(xi > 0) || xi > 1e12) return 0; /* rigid */
    return a->lambda / xi;
}

Acoustic *ac_create(const AcSpec *s, char *err, size_t errlen) {
    if (!(s->dx > 0) || !(s->c > 0) || s->size[0] <= 0 || s->size[1] <= 0 || s->size[2] <= 0) {
        snprintf(err, errlen, "acoustic: size, dx and c must be positive");
        return NULL;
    }
    Acoustic *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->spec = *s;
    /* nodes at i dx, i = 0 .. n - 1: the walls of the domain lie on the outermost nodes (the boundary condition is
     * written at the boundary node), so a room of length L has L / dx + 1 nodes along it */
    a->nx = (int)lround(s->size[0] / s->dx) + 1, a->ny = (int)lround(s->size[1] / s->dx) + 1, a->nz = (int)lround(s->size[2] / s->dx) + 1;
    if (a->nx < 3 || a->ny < 3 || a->nz < 3) {
        snprintf(err, errlen, "acoustic: at least 3 cells per direction");
        free(a);
        return NULL;
    }
    a->n = (long)a->nx * a->ny * a->nz;
    if (a->n > 400000000L) {
        snprintf(err, errlen, "acoustic: %ld cells is more than this solver allows (4e8)", a->n);
        free(a);
        return NULL;
    }
    a->lambda = 1.0 / sqrt(3.0);
    a->dt = a->lambda * s->dx / s->c;
    a->p0 = calloc((size_t)a->n, sizeof(double));
    a->p1 = calloc((size_t)a->n, sizeof(double));
    a->solid = calloc((size_t)a->n, 1);
    a->material = calloc((size_t)a->n, 1);
    a->kind = calloc((size_t)a->n, 1);
    if (!a->p0 || !a->p1 || !a->solid || !a->material || !a->kind) {
        snprintf(err, errlen, "acoustic: out of memory for %ld cells", a->n);
        ac_free(a);
        return NULL;
    }
    /* obstacles: a node is solid when it lies strictly inside a box; the nodes on the box's surface stay air and
     * become boundary nodes, so the surface is where the box says */
    for (int b = 0; b < s->nboxes; b++) {
        const AcBox *B = &s->boxes[b];
        int lo[3], hi[3];
        for (int d = 0; d < 3; d++) {
            lo[d] = (int)floor(B->lo[d] / s->dx + 1e-9) + 1;
            hi[d] = (int)ceil(B->hi[d] / s->dx - 1e-9) - 1;
            if (lo[d] < 0) lo[d] = 0;
        }
        if (hi[0] > a->nx - 1) hi[0] = a->nx - 1;
        if (hi[1] > a->ny - 1) hi[1] = a->ny - 1;
        if (hi[2] > a->nz - 1) hi[2] = a->nz - 1;
        for (int k = lo[2]; k <= hi[2]; k++)
            for (int j = lo[1]; j <= hi[1]; j++)
                for (int i = lo[0]; i <= hi[0]; i++) {
                    long c = at(a, i, j, k);
                    a->solid[c] = 1;
                    a->material[c] = (unsigned char)B->material;
                }
    }
    /* classify air cells; count and fill the boundary nodes */
    for (int pass = 0; pass < 2; pass++) {
        long nb = 0;
        for (int k = 0; k < a->nz; k++)
            for (int j = 0; j < a->ny; j++)
                for (int i = 0; i < a->nx; i++) {
                    long c = at(a, i, j, k);
                    if (a->solid[c]) {
                        a->kind[c] = 2;
                        continue;
                    }
                    unsigned char miss = 0;
                    double beta = 0;
                    int ii[6] = {i - 1, i + 1, i, i, i, i}, jj[6] = {j, j, j - 1, j + 1, j, j}, kk[6] = {k, k, k, k, k - 1, k + 1};
                    for (int d = 0; d < 6; d++) {
                        bool outside = ii[d] < 0 || jj[d] < 0 || kk[d] < 0 || ii[d] >= a->nx || jj[d] >= a->ny || kk[d] >= a->nz;
                        if (outside) {
                            miss |= (unsigned char)(1 << d);
                            beta += material_beta(a, s->wall_material[d]);
                        } else if (a->solid[at(a, ii[d], jj[d], kk[d])]) {
                            miss |= (unsigned char)(1 << d);
                            beta += material_beta(a, a->material[at(a, ii[d], jj[d], kk[d])]);
                        }
                    }
                    a->kind[c] = miss ? 1 : 0;
                    if (miss) {
                        if (pass == 1) a->bnodes[nb] = (BNode){c, miss, beta};
                        nb++;
                    }
                }
        if (pass == 0) {
            a->bnodes = malloc((size_t)(nb ? nb : 1) * sizeof(BNode));
            if (!a->bnodes) {
                ac_free(a);
                return NULL;
            }
        }
        a->nbnodes = nb;
    }
    /* sources: the pressure injected at the source cell so that the free-field pressure at 1 m has the given
     * amplitude (the discrete point source: p(r) = f dx^3 / (4 pi r c^2 dt^2)) */
    for (int q = 0; q < s->nsources; q++) {
        int i = (int)lround(s->sources[q].pos[0] / s->dx), j = (int)lround(s->sources[q].pos[1] / s->dx), k = (int)lround(s->sources[q].pos[2] / s->dx);
        if (i < 0 || j < 0 || k < 0 || i >= a->nx || j >= a->ny || k >= a->nz || a->solid[at(a, i, j, k)]) {
            snprintf(err, errlen, "acoustic: source %d is outside the air", q);
            ac_free(a);
            return NULL;
        }
        a->src_idx[q] = at(a, i, j, k);
        a->src_gain[q] = s->sources[q].amplitude * 4 * M_PI * 1.0 * s->c * s->c * a->dt * a->dt / (s->dx * s->dx * s->dx);
    }
    if (s->piston) { /* the face's boundary nodes within the radius */
        int ax = s->piston_face / 2, hi = s->piston_face % 2, u = ax == 0 ? 1 : 0, v = ax == 2 ? 1 : 2;
        int nn[3] = {a->nx, a->ny, a->nz};
        a->pist = malloc((size_t)(a->nbnodes ? a->nbnodes : 1) * sizeof(long));
        for (long q = 0; q < a->nbnodes; q++) {
            long c = a->bnodes[q].idx;
            int ijk[3] = {(int)(c % a->nx), (int)((c / a->nx) % a->ny), (int)(c / ((long)a->nx * a->ny))};
            if (ijk[ax] != (hi ? nn[ax] - 1 : 0)) continue;
            double du = ijk[u] * s->dx - s->piston_centre[0], dv = ijk[v] * s->dx - s->piston_centre[1];
            if (du * du + dv * dv <= s->piston_radius * s->piston_radius) a->pist[a->npist++] = q;
        }
        if (!a->npist) {
            snprintf(err, errlen, "acoustic: the piston covers no node of its face");
            ac_free(a);
            return NULL;
        }
        a->pist_gain = M_PI * s->piston_radius * s->piston_radius / (a->npist * s->dx * s->dx);
    }
    a->trace_cap = 4096;
    a->traces = calloc((size_t)(s->nreceivers ? s->nreceivers : 1) * (size_t)a->trace_cap, sizeof(double));
    int nt = s->threads > 0 ? s->threads : cpu_perf_count();
    a->pool = pool_create(nt < 1 ? 1 : nt);
    return a;
}

void ac_free(Acoustic *a) {
    if (!a) return;
    free(a->p0), free(a->p1), free(a->solid), free(a->material), free(a->kind), free(a->bnodes), free(a->traces), free(a->pist);
    if (a->pool) pool_destroy(a->pool);
    free(a);
}

/* interior update over z-planes: p0 <- 2 p1 - p0 + lambda^2 (sum of six neighbours - 6 p1) */
static void interior_fn(void *ctx, int begin, int end, int tid) {
    Acoustic *a = ctx;
    const double l2 = a->lambda * a->lambda;
    const long sx = 1, sy = a->nx, sz = (long)a->nx * a->ny;
    for (int k = begin; k < end; k++)
        for (int j = 0; j < a->ny; j++) {
            long c0 = at(a, 0, j, k);
            for (int i = 0; i < a->nx; i++) {
                long c = c0 + i;
                if (a->kind[c]) continue;
                const double *p = a->p1;
                double lap = p[c - sx] + p[c + sx] + p[c - sy] + p[c + sy] + p[c - sz] + p[c + sz] - 6 * p[c];
                a->p0[c] = 2 * p[c] - a->p0[c] + l2 * lap;
            }
        }
}

static void boundary_fn(void *ctx, int begin, int end, int tid) {
    Acoustic *a = ctx;
    const double l2 = a->lambda * a->lambda;
    const long st[3] = {1, a->nx, (long)a->nx * a->ny};
    const double *p = a->p1;
    for (long q = begin; q < end; q++) {
        const BNode *b = &a->bnodes[q];
        long c = b->idx;
        double sum = 0;
        for (int ax = 0; ax < 3; ax++) {
            bool mlo = b->missing & (1 << (2 * ax)), mhi = b->missing & (1 << (2 * ax + 1));
            double lo = mlo ? (mhi ? p[c] : p[c + st[ax]]) : p[c - st[ax]];
            double hi = mhi ? (mlo ? p[c] : p[c - st[ax]]) : p[c + st[ax]];
            sum += lo + hi - 2 * p[c];
        }
        a->p0[c] = (2 * p[c] - a->p0[c] * (1 - b->beta) + l2 * sum) / (1 + b->beta);
    }
}

static void boundary_chunks(void *ctx, int begin, int end, int tid) {
    Acoustic *a = ctx;
    long per = (a->nbnodes + 255) / 256;
    for (int q = begin; q < end; q++) {
        long b0 = q * per, b1 = b0 + per;
        if (b1 > a->nbnodes) b1 = a->nbnodes;
        if (b0 < b1) boundary_fn(a, (int)b0, (int)b1, tid);
    }
}

static double pulse(const AcSource *s, double t) {
    /* the derivative of a Gaussian, peak 1: no net volume is injected, so a closed room returns to rest */
    double x = (t - s->delay_s) / s->width_s;
    return -x * exp(0.5 - 0.5 * x * x);
}

static double trilinear(const Acoustic *a, const double *p, const double x[3]) {
    double u = x[0] / a->spec.dx, v = x[1] / a->spec.dx, w = x[2] / a->spec.dx;
    int i = (int)floor(u), j = (int)floor(v), k = (int)floor(w);
    double fx = u - i, fy = v - j, fz = w - k;
    double s = 0, ws = 0;
    for (int dk = 0; dk < 2; dk++)
        for (int dj = 0; dj < 2; dj++)
            for (int di = 0; di < 2; di++) {
                int ii = i + di, jj = j + dj, kk = k + dk;
                if (ii < 0 || jj < 0 || kk < 0 || ii >= a->nx || jj >= a->ny || kk >= a->nz) continue;
                long c = at(a, ii, jj, kk);
                if (a->solid[c]) continue;
                double wt = (di ? fx : 1 - fx) * (dj ? fy : 1 - fy) * (dk ? fz : 1 - fz);
                s += wt * p[c];
                ws += wt;
            }
    return ws > 0 ? s / ws : 0;
}

void ac_step(Acoustic *a) {
    pool_for(a->pool, a->nz, 1, interior_fn, a);
    pool_for(a->pool, 256, 1, boundary_chunks, a);
    /* the piston: its face's mirror neighbour carries the pressure gradient -rho a_n, i.e. the ghost value is the
     * mirrored one plus 2 dx rho a (the update is linear in the neighbour sum, so the term is added afterwards) */
    if (a->npist) {
        const double add = a->lambda * a->lambda * 2 * a->spec.dx * a->spec.rho * a->pist_acc * a->pist_gain;
        for (long q = 0; q < a->npist; q++) {
            const BNode *b = &a->bnodes[a->pist[q]];
            a->p0[b->idx] += add / (1 + b->beta);
        }
    }
    /* sources, at the new time level */
    for (int q = 0; q < a->spec.nsources; q++) a->p0[a->src_idx[q]] += a->src_gain[q] * pulse(&a->spec.sources[q], a->t + a->dt);
    double *tmp = a->p0;
    a->p0 = a->p1;
    a->p1 = tmp;
    a->t += a->dt;
    a->steps++;
    if (a->spec.nreceivers > 0) {
        if (a->steps >= a->trace_cap) {
            long cap = a->trace_cap * 2;
            double *nt = calloc((size_t)a->spec.nreceivers * (size_t)cap, sizeof(double));
            if (nt) {
                for (int r = 0; r < a->spec.nreceivers; r++) memcpy(nt + (size_t)r * cap, a->traces + (size_t)r * a->trace_cap, (size_t)a->trace_cap * sizeof(double));
                free(a->traces);
                a->traces = nt;
                a->trace_cap = cap;
            }
        }
        if (a->steps < a->trace_cap)
            for (int r = 0; r < a->spec.nreceivers; r++) a->traces[(size_t)r * a->trace_cap + (size_t)(a->steps - 1)] = trilinear(a, a->p1, a->spec.receivers[r]);
    }
}

double ac_dt(const Acoustic *a) { return a->dt; }
void ac_set_piston_acceleration(Acoustic *a, double acc) { a->pist_acc = acc; }
void ac_piston_info(const Acoustic *a, long *nodes, double *gain) {
    if (nodes) *nodes = a->npist;
    if (gain) *gain = a->pist_gain;
}
double ac_time(const Acoustic *a) { return a->t; }
long ac_steps(const Acoustic *a) { return a->steps; }
void ac_dims(const Acoustic *a, int n[3]) { n[0] = a->nx, n[1] = a->ny, n[2] = a->nz; }
double ac_pressure_at(const Acoustic *a, const double x[3]) { return trilinear(a, a->p1, x); }

const double *ac_receiver_trace(const Acoustic *a, int r, long *nsamples) {
    *nsamples = a->steps < a->trace_cap ? a->steps : a->trace_cap;
    return a->traces + (size_t)r * a->trace_cap;
}

double ac_energy(const Acoustic *a) {
    /* potential energy only: sum of p^2 / (2 rho c^2) dV over air cells */
    double e = 0;
    for (long c = 0; c < a->n; c++)
        if (!a->solid[c]) e += a->p1[c] * a->p1[c];
    return e * a->spec.dx * a->spec.dx * a->spec.dx / (2 * a->spec.rho * a->spec.c * a->spec.c);
}

void ac_write_frame(Acoustic *a, LabWriter *w, double z_slice, double y_slice, double x_slice, double threshold, int stride) {
    const double dx = a->spec.dx;
    /* slices */
    struct {
        const char *name;
        double at;
        int plane;
    } sl[3] = {{"slice_xy", z_slice, LAB_PLANE_XY}, {"slice_xz", y_slice, LAB_PLANE_XZ}, {"slice_yz", x_slice, LAB_PLANE_YZ}};
    for (int s = 0; s < 3; s++) {
        if (sl[s].at < 0) continue;
        LabBlock b = {{0, 0, 1}, 0, sl[s].plane, {-0.5 * dx, -0.5 * dx, -0.5 * dx}, {dx, dx, dx}}; /* cells centred on the nodes */
        int nu, nv;
        if (sl[s].plane == LAB_PLANE_XY) nu = a->nx, nv = a->ny, b.origin[2] = sl[s].at;
        else if (sl[s].plane == LAB_PLANE_XZ) nu = a->nx, nv = a->nz, b.origin[1] = sl[s].at, b.origin[2] = -0.5 * dx;
        else nu = a->ny, nv = a->nz, b.origin[0] = sl[s].at, b.origin[1] = -0.5 * dx, b.origin[2] = -0.5 * dx;
        if (sl[s].plane == LAB_PLANE_XZ) b.origin[1] = sl[s].at;
        b.n[0] = nu, b.n[1] = nv;
        float *f = malloc((size_t)nu * nv * sizeof(float));
        if (!f) continue;
        int fixed = (int)lround(sl[s].at / dx);
        for (int v = 0; v < nv; v++)
            for (int u = 0; u < nu; u++) {
                long c;
                if (sl[s].plane == LAB_PLANE_XY) c = at(a, u, v, fixed < a->nz ? fixed : a->nz - 1);
                else if (sl[s].plane == LAB_PLANE_XZ) c = at(a, u, fixed < a->ny ? fixed : a->ny - 1, v);
                else c = at(a, fixed < a->nx ? fixed : a->nx - 1, u, v);
                f[(size_t)v * nu + u] = a->solid[c] ? 0.0f : (float)a->p1[c];
            }
        lab_part_blocks(w, sl[s].name, 1, &b);
        lab_field(w, "p", LAB_AT_CELL, (size_t)nu * nv, f);
        free(f);
    }
    /* the wavefront as points */
    if (threshold > 0 && stride > 0) {
        long cap = 0, np = 0;
        for (int k = 0; k < a->nz; k += stride)
            for (int j = 0; j < a->ny; j += stride)
                for (int i = 0; i < a->nx; i += stride)
                    if (!a->solid[at(a, i, j, k)] && fabs(a->p1[at(a, i, j, k)]) > threshold) cap++;
        double *xyz = malloc((size_t)(cap ? cap : 1) * 3 * sizeof(double));
        float *pv = malloc((size_t)(cap ? cap : 1) * sizeof(float));
        if (xyz && pv) {
            for (int k = 0; k < a->nz; k += stride)
                for (int j = 0; j < a->ny; j += stride)
                    for (int i = 0; i < a->nx; i += stride) {
                        long c = at(a, i, j, k);
                        if (a->solid[c] || !(fabs(a->p1[c]) > threshold)) continue;
                        xyz[3 * np] = i * dx, xyz[3 * np + 1] = j * dx, xyz[3 * np + 2] = k * dx;
                        pv[np++] = (float)a->p1[c];
                    }
            lab_part_points(w, "wavefront", (int)np, xyz);
            lab_field(w, "p", LAB_AT_NODE, (size_t)np, pv);
        }
        free(xyz);
        free(pv);
    }
    /* obstacles as hexahedra, and the room's edges as lines */
    int nb = a->spec.nboxes;
    if (nb > 0) {
        double *X = malloc((size_t)nb * 8 * 3 * sizeof(double));
        int *C = malloc((size_t)nb * 8 * sizeof(int));
        float *mat = malloc((size_t)nb * sizeof(float));
        if (X && C && mat) {
            for (int b = 0; b < nb; b++) {
                const AcBox *B = &a->spec.boxes[b];
                for (int v = 0; v < 8; v++) {
                    X[(8 * b + v) * 3 + 0] = (v & 1) ? B->hi[0] : B->lo[0];
                    X[(8 * b + v) * 3 + 1] = (v & 2) ? B->hi[1] : B->lo[1];
                    X[(8 * b + v) * 3 + 2] = (v & 4) ? B->hi[2] : B->lo[2];
                }
                static const int ORD[8] = {0, 1, 3, 2, 4, 5, 7, 6};
                for (int v = 0; v < 8; v++) C[8 * b + v] = 8 * b + ORD[v];
                mat[b] = (float)B->material;
            }
            lab_part_cells(w, "room", nb * 8, X, nb, LAB_HEX, C);
            lab_field(w, "material", LAB_AT_CELL, (size_t)nb, mat);
        }
        free(X), free(C), free(mat);
    }
    double L[3] = {(a->nx - 1) * dx, (a->ny - 1) * dx, (a->nz - 1) * dx};
    double E[8 * 3];
    for (int v = 0; v < 8; v++) E[3 * v] = (v & 1) ? L[0] : 0, E[3 * v + 1] = (v & 2) ? L[1] : 0, E[3 * v + 2] = (v & 4) ? L[2] : 0;
    static const int EDGES[24] = {0, 1, 2, 3, 4, 5, 6, 7, 0, 2, 1, 3, 4, 6, 5, 7, 0, 4, 1, 5, 2, 6, 3, 7};
    lab_part_cells(w, "walls", 8, E, 12, LAB_LINE, EDGES);
}

char *ac_header_json(const AcSpec *s, const char *title) {
    char *h = malloc(2048);
    if (!h) return NULL;
    snprintf(h, 2048,
             "{\"domain\":\"acoustic\",\"title\":\"%s\",\"solver\":\"src/lab/acoustic: 3D FDTD, 7-point Laplacian, Courant 1/sqrt(3), locally reacting walls\","
             "\"size_m\":[%.6g,%.6g,%.6g],\"dx_m\":%.6g,\"c_m_s\":%.6g,\"cells\":%ld,"
             "\"fields\":{\"p\":\"Pa\",\"material\":\"1\"}}",
             title ? title : "", s->size[0], s->size[1], s->size[2], s->dx, s->c,
             (long)(lround(s->size[0] / s->dx) + 1) * (lround(s->size[1] / s->dx) + 1) * (lround(s->size[2] / s->dx) + 1));
    return h;
}

/* Values remain at the solver nodes, represented as cell centres in labio. */
bool ac_write_volume(Acoustic *a, LabWriter *w) {
    double dx=a->spec.dx;
    LabBlock b={{a->nx,a->ny,a->nz},0,LAB_PLANE_XY,
                {-0.5*dx,-0.5*dx,-0.5*dx},{dx,dx,dx}};
    float *v=malloc((size_t)a->n*sizeof *v);
    if(!v) return false;
    lab_part_blocks(w,"acoustic_volume",1,&b);
    for(long q=0;q<a->n;q++) v[q]=a->solid[q]?0:(float)a->p1[q];
    lab_field(w,"p",LAB_AT_CELL,(size_t)a->n,v);
    for(long q=0;q<a->n;q++) v[q]=a->solid[q]?1:0;
    lab_field(w,"material",LAB_AT_CELL,(size_t)a->n,v);
    free(v); return true;
}
