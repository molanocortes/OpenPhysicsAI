/* thermal.c - transient heat conduction on hex8 meshes: theta method, Picard iterations, discrete energy balance */
#include "thermal.h"
#include "hex8.h"
#include "krylov.h"
#include "sparse.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const double GPT = 0.57735026918962576451;
enum { FACE_COEFFS = 24 }; /* radiation: tangent[16], linearised load[4], old outward flux[4]; convection uses 4 */

double thermal_table_eval(const ThermalTable *t, double T) {
    if (!t || t->n <= 0) return NAN;
    if (t->n == 1 || T <= t->t[0]) return t->v[0];
    if (T >= t->t[t->n - 1]) return t->v[t->n - 1];
    int lo = 0, hi = t->n - 1;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (t->t[mid] <= T) lo = mid;
        else hi = mid;
    }
    double w = (T - t->t[lo]) / (t->t[hi] - t->t[lo]);
    return t->v[lo] + w * (t->v[hi] - t->v[lo]);
}

double thermal_table_integral(const ThermalTable *t, double a, double b) {
    if (!t || t->n <= 0) return NAN;
    double sign = 1;
    if (b < a) {
        double x = a;
        a = b, b = x, sign = -1;
    }
    if (t->n == 1) return sign * t->v[0] * (b - a);
    double s = 0, x = a;
    if (x < t->t[0]) {
        double x1 = fmin(b, t->t[0]);
        s += t->v[0] * (x1 - x);
        x = x1;
    }
    for (int i = 0; i + 1 < t->n && x < b; i++) {
        if (x >= t->t[i + 1]) continue;
        double x1 = fmin(b, t->t[i + 1]);
        s += 0.5 * (thermal_table_eval(t, x) + thermal_table_eval(t, x1)) * (x1 - x); /* exact for a linear piece */
        x = x1;
    }
    if (x < b) s += t->v[t->n - 1] * (b - x);
    return sign * s;
}

/* ---- constitutive evaluation ----------------------------------------------------------------------
 *
 * Enthalpy. The volumetric enthalpy measured from THERMAL_H_REF is
 *     H(T) = integral rho(s) cp(s) ds + rho(T_melt) L f_liq(s)
 * The sensible part integrates the product of two piecewise-linear tables. On an interval where both are linear the
 * product is a quadratic, which Simpson's rule integrates exactly, so the integral below is exact (not a quadrature
 * approximation) as long as the interval ends at a breakpoint of either table.
 *
 * The latent part uses a liquid fraction that is linear between the solidus and the liquidus. Its density is taken at
 * the melting midpoint so that the latent heat released on freezing equals the latent heat absorbed on melting for a
 * temperature-dependent density; a model that varies the latent density with T would not conserve energy over a melt
 * and re-freeze cycle. */

static double rho_cp(const ThermalMaterial *m, double T) { return thermal_table_eval(&m->rho, T) * thermal_table_eval(&m->cp, T); }

static bool has_phase_change(const ThermalMaterial *m) {
    return m->latent_heat > 0 && m->liquidus > m->solidus && m->solidus > 0;
}

double thermal_liquid_fraction(const ThermalMaterial *m, double T) {
    if (!has_phase_change(m)) return 0;
    if (T <= m->solidus) return 0;
    if (T >= m->liquidus) return 1;
    return (T - m->solidus) / (m->liquidus - m->solidus);
}

/* true when the enthalpy is linear in T, so that H = rho cp T and the secant capacity is rho cp with no table work.
 * Constant properties are the common case (and the whole of most benchmarks), so this fast path matters. */
static bool linear_enthalpy(const ThermalMaterial *m) { return m->rho.n == 1 && m->cp.n == 1 && !has_phase_change(m); }

/* exact integral of rho cp from a to b: split at every breakpoint of either table, Simpson on each piece */
static double sensible_enthalpy(const ThermalMaterial *m, double a, double b) {
    if (m->rho.n == 1 && m->cp.n == 1) return m->rho.v[0] * m->cp.v[0] * (b - a);
    double sign = 1;
    if (b < a) {
        double x = a;
        a = b, b = x, sign = -1;
    }
    if (!(b > a)) return 0;
    int ir = 0, ic = 0;
    double x = a, s = 0;
    while (ir < m->rho.n && m->rho.t[ir] <= x) ir++;
    while (ic < m->cp.n && m->cp.t[ic] <= x) ic++;
    for (int guard = 0; x < b && guard <= m->rho.n + m->cp.n + 2; guard++) {
        double x1 = b;
        if (ir < m->rho.n && m->rho.t[ir] < x1) x1 = m->rho.t[ir];
        if (ic < m->cp.n && m->cp.t[ic] < x1) x1 = m->cp.t[ic];
        if (!(x1 > x)) x1 = b;
        double mid = 0.5 * (x + x1);
        s += (x1 - x) / 6 * (rho_cp(m, x) + 4 * rho_cp(m, mid) + rho_cp(m, x1)); /* exact for the quadratic product */
        x = x1;
        while (ir < m->rho.n && m->rho.t[ir] <= x) ir++;
        while (ic < m->cp.n && m->cp.t[ic] <= x) ic++;
    }
    return sign * s;
}

double thermal_enthalpy(const ThermalMaterial *m, double T) {
    double h = sensible_enthalpy(m, THERMAL_H_REF, T);
    if (has_phase_change(m)) h += thermal_table_eval(&m->rho, 0.5 * (m->solidus + m->liquidus)) * m->latent_heat * thermal_liquid_fraction(m, T);
    return h;
}

double thermal_capacity(const ThermalMaterial *m, double T) {
    double c = rho_cp(m, T);
    if (has_phase_change(m) && T > m->solidus && T < m->liquidus)
        c += thermal_table_eval(&m->rho, 0.5 * (m->solidus + m->liquidus)) * m->latent_heat / (m->liquidus - m->solidus);
    return c;
}

double thermal_secant_capacity(const ThermalMaterial *m, double T0, double T1) {
    if (linear_enthalpy(m)) return m->rho.v[0] * m->cp.v[0];
    double d = T1 - T0;
    if (fabs(d) < THERMAL_DH_MIN) return thermal_capacity(m, 0.5 * (T0 + T1));
    return (thermal_enthalpy(m, T1) - thermal_enthalpy(m, T0)) / d;
}

bool thermal_is_anisotropic(const ThermalMaterial *m) { return m->k2.n > 0 || m->k3.n > 0; }

void thermal_conductivity(const ThermalMaterial *m, double T, double K[9]) {
    double k1 = thermal_table_eval(&m->k, T);
    if (!thermal_is_anisotropic(m)) {
        for (int i = 0; i < 9; i++) K[i] = 0;
        K[0] = K[4] = K[8] = k1;
        return;
    }
    double k2 = m->k2.n > 0 ? thermal_table_eval(&m->k2, T) : k1, k3 = m->k3.n > 0 ? thermal_table_eval(&m->k3, T) : k1;
    double R[9];
    bool ident = true;
    for (int i = 0; i < 9 && ident; i++) ident = m->axes[i] == 0;
    if (ident) {
        for (int i = 0; i < 9; i++) R[i] = 0;
        R[0] = R[4] = R[8] = 1;
    } else {
        memcpy(R, m->axes, 9 * sizeof(double));
    }
    /* K_global = R^T diag(k) R with the rows of R the material directions expressed in global coordinates */
    double d[3] = {k1, k2, k3};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double v = 0;
            for (int a = 0; a < 3; a++) v += R[3 * a + i] * d[a] * R[3 * a + j];
            K[3 * i + j] = v;
        }
}

/* the principal values must be positive and the axes must be orthonormal (a valid rotation), otherwise the
 * conductivity tensor is not positive definite and Fourier's law would transport heat up the gradient */
static bool material_axes_ok(const ThermalMaterial *m, char *err, size_t errlen, int idx) {
    if (!thermal_is_anisotropic(m)) return true;
    bool ident = true;
    for (int i = 0; i < 9 && ident; i++) ident = m->axes[i] == 0;
    if (ident) return true;
    for (int a = 0; a < 3; a++)
        for (int b = a; b < 3; b++) {
            double d = 0;
            for (int k = 0; k < 3; k++) d += m->axes[3 * a + k] * m->axes[3 * b + k];
            if (fabs(d - (a == b ? 1.0 : 0.0)) > 1e-9) {
                snprintf(err, errlen, "material %d: the conductivity axes must be orthonormal (row %d . row %d = %.12g)", idx, a, b, d);
                return false;
            }
        }
    return true;
}

/* ---- element geometry ----------------------------------------------------------------------------- */

typedef struct {
    double K[64];     /* integral of grad Na . grad Nb dV */
    double M[64];     /* integral of Na Nb dV */
    double ML[8];     /* row sums of M = integral of Na dV */
    double F[6][16];  /* per face: integral of Nq Nr dA (face-local node order) */
    double Fv[6][4];  /* per face: integral of Nq dA */
    double area[6];
    double vol;
    double dNdx[8][8][3]; /* [gauss point][node][direction]: shape-function gradients, for per-quadrature-point properties */
    double detw[8];       /* det J at each Gauss point (unit weights) */
    /* faces, for open (advective) boundaries: 2x2 Gauss points g = 2 gs + gt */
    double face_n[6][3];  /* unit outward normal (faces of hex8 voxel elements are planar) */
    double face_w[6][4];  /* |ds x dt| at each face Gauss point */
    double face_N[4][4];  /* [face Gauss point][face node]: bilinear shape values, the same for every face */
    double rad_w[6][9];   /* 3x3 radiation Gauss weights times the physical surface Jacobian */
    double rad_N[9][4];   /* radiation needs degree-five face integrals: Ni T^4 and Ni Nj T^3 */
    double Bc[8][3];      /* shape-function gradients averaged over the element (volume-weighted), for SUPG */
} Geom;

/* trilinear shape functions at the 8 Gauss points: identical for every hexahedron, so they are tabulated once
 * (pthread_once: the error norm may be the first user, from any thread) */
static double GP_N[8][8];
static void fill_gp_shapes(void) {
    for (int gp = 0; gp < 8; gp++) {
        double dN[8][3];
        hex8_shape(HEX8_XI[gp][0] * GPT, HEX8_XI[gp][1] * GPT, HEX8_XI[gp][2] * GPT, GP_N[gp], dN);
    }
}
static void init_gp_shapes(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, fill_gp_shapes);
}

enum { GEOM_CACHE_MAX = 4096, GEOM_HASH = 8192 };

static void element_coords(const ThermalModel *m, int e, double X[8][3]) {
    for (int a = 0; a < 8; a++) {
        int nd = m->conn[8 * (size_t)e + a];
        for (int k = 0; k < 3; k++) X[a][k] = m->xyz[3 * (size_t)nd + k];
    }
}

static bool element_geometry(const double X[8][3], Geom *g) {
    memset(g, 0, sizeof *g);
    for (int gp = 0; gp < 8; gp++) {
        double N[8], dN[8][3], J[3][3], dNdx[8][3];
        hex8_shape(HEX8_XI[gp][0] * GPT, HEX8_XI[gp][1] * GPT, HEX8_XI[gp][2] * GPT, N, dN);
        double det = hex8_jacobian(X, dN, J, dNdx);
        if (!(det > 0)) return false;
        g->vol += det;
        g->detw[gp] = det;
        memcpy(g->dNdx[gp], dNdx, sizeof dNdx);
        for (int a = 0; a < 8; a++)
            for (int b = 0; b < 8; b++) {
                g->K[8 * a + b] += det * (dNdx[a][0] * dNdx[b][0] + dNdx[a][1] * dNdx[b][1] + dNdx[a][2] * dNdx[b][2]);
                g->M[8 * a + b] += det * N[a] * N[b];
            }
    }
    for (int a = 0; a < 8; a++)
        for (int b = 0; b < 8; b++) g->ML[a] += g->M[8 * a + b];
    for (int gp = 0; gp < 8; gp++)
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) g->Bc[a][k] += g->detw[gp] * g->dNdx[gp][a][k] / g->vol;
    double ec[3] = {0, 0, 0};
    for (int a = 0; a < 8; a++)
        for (int k = 0; k < 3; k++) ec[k] += 0.125 * X[a][k];
    static const double S[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (int f = 0; f < 6; f++) {
        const int *fn = HEX8_FACE_NODES[f];
        double nsum[3] = {0, 0, 0}, fc[3] = {0, 0, 0};
        for (int q = 0; q < 4; q++)
            for (int k = 0; k < 3; k++) fc[k] += 0.25 * X[fn[q]][k];
        for (int gs = 0; gs < 2; gs++)
            for (int gt = 0; gt < 2; gt++) {
                double sv = (gs ? 1 : -1) * GPT, tv = (gt ? 1 : -1) * GPT, Nq[4], ds[3] = {0, 0, 0}, dt[3] = {0, 0, 0};
                for (int q = 0; q < 4; q++) {
                    Nq[q] = 0.25 * (1 + S[q][0] * sv) * (1 + S[q][1] * tv);
                    double dNs = 0.25 * S[q][0] * (1 + S[q][1] * tv), dNt = 0.25 * S[q][1] * (1 + S[q][0] * sv);
                    for (int k = 0; k < 3; k++) ds[k] += dNs * X[fn[q]][k], dt[k] += dNt * X[fn[q]][k];
                }
                double n0 = ds[1] * dt[2] - ds[2] * dt[1], n1 = ds[2] * dt[0] - ds[0] * dt[2], n2 = ds[0] * dt[1] - ds[1] * dt[0];
                double jac = sqrt(n0 * n0 + n1 * n1 + n2 * n2);
                g->area[f] += jac;
                for (int q = 0; q < 4; q++) {
                    g->Fv[f][q] += Nq[q] * jac;
                    for (int r = 0; r < 4; r++) g->F[f][4 * q + r] += Nq[q] * Nq[r] * jac;
                }
                int gq = 2 * gs + gt;
                g->face_w[f][gq] = jac;
                memcpy(g->face_N[gq], Nq, sizeof Nq);
                nsum[0] += n0, nsum[1] += n1, nsum[2] += n2;
            }
        double nl = sqrt(nsum[0] * nsum[0] + nsum[1] * nsum[1] + nsum[2] * nsum[2]);
        double out = (fc[0] - ec[0]) * nsum[0] + (fc[1] - ec[1]) * nsum[1] + (fc[2] - ec[2]) * nsum[2];
        for (int k = 0; k < 3; k++) g->face_n[f][k] = nl > 0 ? (out < 0 ? -1 : 1) * nsum[k] / nl : 0;
        /* A bilinear face temperature gives degree <=5 in each coordinate for both the radiative nodal flux and
         * its tangent. 3x3 Gauss integrates them exactly on planar parallelograms; a warped face retains its actual
         * pointwise Jacobian (there the geometric quadrature is an approximation). Coordinates are in metres. */
        static const double rp[3] = {-0.77459666924148337704, 0, 0.77459666924148337704};
        static const double rw[3] = {5.0/9, 8.0/9, 5.0/9};
        for (int gs = 0; gs < 3; gs++) for (int gt = 0; gt < 3; gt++) {
            int gp = 3 * gs + gt;
            double ds[3] = {0}, dt[3] = {0};
            for (int q = 0; q < 4; q++) {
                g->rad_N[gp][q] = 0.25 * (1 + S[q][0]*rp[gs]) * (1 + S[q][1]*rp[gt]);
                double dNs = 0.25 * S[q][0] * (1 + S[q][1]*rp[gt]);
                double dNt = 0.25 * S[q][1] * (1 + S[q][0]*rp[gs]);
                for (int k = 0; k < 3; k++) ds[k] += dNs * X[fn[q]][k], dt[k] += dNt * X[fn[q]][k];
            }
            double n0 = ds[1]*dt[2] - ds[2]*dt[1], n1 = ds[2]*dt[0] - ds[0]*dt[2], n2 = ds[0]*dt[1] - ds[1]*dt[0];
            g->rad_w[f][gp] = rw[gs] * rw[gt] * sqrt(n0*n0 + n1*n1 + n2*n2);
        }
    }
    return true;
}

static uint64_t key_hash(const int64_t key[24]) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 24; i++) {
        h ^= (uint64_t)key[i];
        h *= 1099511628211ull;
        h ^= h >> 29;
    }
    return h;
}

/* ---- solver state ------------------------------------------------------------------------------- */

struct ThermalSolver {
    int nn, ne, neq;
    ThermalOptions opt;
    int *eq;                    /* node -> equation, -1 for nodes of no element */
    CsrMatrix A;
    int64_t *diag;
    int *emap;                  /* 64 per element: CSR entry of (a, b); NULL: looked up */
    int ncolors, *order, *cstart;
    int *geom;                  /* per element: cache entry or -1 */
    Geom *cache;
    int ncache;
    double *coef;               /* 3 per element: k at the iterate, secant capacity of the step, k at the start of the step
                                 * (the element-mean path; the quadrature path reads the material directly) */
    bool any_anisotropic, any_phase;
    unsigned char *node_active;
    int *face_start, *face_list, *face_cursor, face_cap;
    double *face_h;             /* FACE_COEFFS per face; convection: h/ambient now/old; radiation: tangent/load/old flux */
    int *src_slot, nsrc, src_cap, *src_elem;
    double *src_vec, src_scale, src_power;
    double *rhs, *x, *res, *ax, *prev;
};

static double elapsed(struct timespec a, struct timespec b) { return (double)(b.tv_sec - a.tv_sec) + 1e-9 * (double)(b.tv_nsec - a.tv_nsec); }

double thermal_theta(const ThermalSolver *s) { return s->opt.theta; }

double thermal_node_reaction(const ThermalSolver *s, const ThermalModel *m, int n) {
    if (n < 0 || n >= s->nn || !s->node_active[n] || !m->fixed || !m->fixed[n]) return 0;
    return s->res[n]; /* energy_pass stores A T1 - b per node: at a prescribed node, the heat the constraint supplied (W) */
}

void thermal_free(ThermalSolver *s) {
    if (!s) return;
    free(s->eq), csr_free(&s->A), free(s->diag), free(s->emap), free(s->order), free(s->cstart), free(s->geom), free(s->cache);
    free(s->coef), free(s->node_active), free(s->face_start), free(s->face_list), free(s->face_cursor), free(s->face_h);
    free(s->src_slot), free(s->src_elem), free(s->src_vec), free(s->rhs), free(s->x), free(s->res), free(s->ax), free(s->prev);
    free(s);
}

static bool table_ok(const ThermalTable *t) {
    if (t->n <= 0 || !t->t || !t->v) return false;
    for (int i = 0; i < t->n; i++) {
        if (!(t->v[i] > 0) || !isfinite(t->v[i])) return false;
        if (i && !(t->t[i] > t->t[i - 1])) return false;
    }
    return true;
}

ThermalSolver *thermal_create(const ThermalModel *m, const ThermalOptions *o, char *err, size_t errlen) {
    ThermalSolver *s = calloc(1, sizeof *s);
    if (!s) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    s->opt = o ? *o : (ThermalOptions){0};
    if (s->opt.theta == 0) s->opt.theta = 1;
    if (s->opt.max_picard <= 0) s->opt.max_picard = 50;
    if (s->opt.picard_tol <= 0) s->opt.picard_tol = 1e-6;
    if (s->opt.pcg_tol <= 0) s->opt.pcg_tol = 1e-12;
    if (s->opt.pcg_max_iter <= 0) s->opt.pcg_max_iter = 20000;
    int nn = m->nnodes, ne = m->nelems, *edofs = NULL, *color = NULL, *hslot = NULL;
    uint64_t *nodecol = NULL;
    int64_t (*keys)[24] = NULL;
    s->nn = nn, s->ne = ne;
    if (!(s->opt.theta >= 0.5 && s->opt.theta <= 1)) {
        snprintf(err, errlen, "theta must lie within 0.5 and 1 (the unconditionally stable range)");
        goto fail;
    }
    if (nn <= 0 || ne <= 0 || !m->xyz || !m->conn || m->nmat <= 0 || !m->mat) {
        snprintf(err, errlen, "empty thermal model");
        goto fail;
    }
    init_gp_shapes();
    for (int i = 0; i < m->nmat; i++) {
        const ThermalMaterial *mt = &m->mat[i];
        if (!table_ok(&mt->k) || !table_ok(&mt->cp) || !table_ok(&mt->rho)) {
            snprintf(err, errlen, "material %d needs positive conductivity, specific heat and density tables with increasing temperatures", i);
            goto fail;
        }
        if ((mt->k2.n > 0 && !table_ok(&mt->k2)) || (mt->k3.n > 0 && !table_ok(&mt->k3))) {
            snprintf(err, errlen, "material %d: the second and third principal conductivities need positive increasing tables", i);
            goto fail;
        }
        if (!material_axes_ok(mt, err, errlen, i)) goto fail;
        if (mt->latent_heat != 0) {
            if (!(mt->latent_heat > 0) || !(mt->solidus > 0) || !(mt->liquidus > mt->solidus)) {
                snprintf(err, errlen, "material %d: a latent heat needs a positive solidus and a liquidus above it (got L = %g, Ts = %g, Tl = %g)", i,
                         mt->latent_heat, mt->solidus, mt->liquidus);
                goto fail;
            }
            s->any_phase = true;
        }
        if (thermal_is_anisotropic(mt)) s->any_anisotropic = true;
    }
    s->eq = malloc((size_t)nn * sizeof(int));
    edofs = malloc(8 * (size_t)ne * sizeof(int));
    color = malloc((size_t)ne * sizeof(int));
    nodecol = calloc((size_t)nn, sizeof(uint64_t));
    if (!s->eq || !edofs || !color || !nodecol) goto oom;
    for (int i = 0; i < nn; i++) s->eq[i] = -1;
    for (int e = 0; e < ne; e++) {
        int mi = m->elem_mat ? m->elem_mat[e] : 0;
        if (mi < 0 || mi >= m->nmat) {
            snprintf(err, errlen, "element %d refers to material %d (valid 0..%d)", e, mi, m->nmat - 1);
            goto fail;
        }
        for (int a = 0; a < 8; a++) {
            int nd = m->conn[8 * (size_t)e + a];
            if (nd < 0 || nd >= nn) {
                snprintf(err, errlen, "element %d refers to node %d outside 0..%d", e, nd, nn - 1);
                goto fail;
            }
            s->eq[nd] = 0;
        }
    }
    for (int i = 0; i < nn; i++)
        if (s->eq[i] == 0) s->eq[i] = s->neq++;
        else s->eq[i] = -1;
    for (int e = 0; e < ne; e++)
        for (int a = 0; a < 8; a++) edofs[8 * (size_t)e + a] = s->eq[m->conn[8 * (size_t)e + a]];
    /* interface pairs couple two nodes that need not share an element, so they must be in the sparsity pattern:
     * they enter csr_from_elements as two-node "elements" padded with -1 */
    int npat = ne;
    if (m->ninterface > 0) {
        if (!m->interfaces) {
            snprintf(err, errlen, "ninterface is %d but no interface array was given", m->ninterface);
            goto fail;
        }
        int *ed2 = realloc(edofs, 8 * ((size_t)ne + (size_t)m->ninterface) * sizeof(int));
        if (!ed2) goto oom;
        edofs = ed2;
        for (int i = 0; i < m->ninterface; i++) {
            const ThermalInterfaceNode *it = &m->interfaces[i];
            if (it->node_a < 0 || it->node_a >= nn || it->node_b < 0 || it->node_b >= nn || it->node_a == it->node_b) {
                snprintf(err, errlen, "interface %d joins invalid nodes (%d, %d) of 0..%d", i, it->node_a, it->node_b, nn - 1);
                goto fail;
            }
            if (!(it->conductance >= 0) || !(it->area >= 0) || !isfinite(it->conductance) || !isfinite(it->area)) {
                snprintf(err, errlen, "interface %d needs a non-negative conductance and area (got %g, %g)", i, it->conductance, it->area);
                goto fail;
            }
            int *row = edofs + 8 * ((size_t)ne + (size_t)i);
            row[0] = s->eq[it->node_a], row[1] = s->eq[it->node_b];
            for (int a = 2; a < 8; a++) row[a] = -1;
        }
        npat = ne + m->ninterface;
    }
    if (!csr_from_elements(&s->A, s->neq, npat, 8, edofs, err, errlen)) goto fail;
    s->diag = malloc((size_t)s->neq * sizeof(int64_t));
    if (!s->diag) goto oom;
    for (int q = 0; q < s->neq; q++) s->diag[q] = csr_find(&s->A, q, q);
    if (s->A.nnz < INT32_MAX && (uint64_t)ne * 64 * sizeof(int) <= (uint64_t)256 << 20) {
        s->emap = malloc(64 * (size_t)ne * sizeof(int));
        for (int e = 0; s->emap && e < ne; e++)
            for (int a = 0; a < 8; a++)
                for (int b = 0; b < 8; b++) s->emap[64 * (size_t)e + 8 * a + b] = (int)csr_find(&s->A, edofs[8 * (size_t)e + a], edofs[8 * (size_t)e + b]);
    }

    /* colours: elements of one colour share no node and assemble in parallel */
    for (int e = 0; e < ne; e++) {
        uint64_t mask = 0;
        for (int a = 0; a < 8; a++) mask |= nodecol[m->conn[8 * (size_t)e + a]];
        int c = 0;
        while (c < 63 && (mask >> c) & 1) c++;
        color[e] = c;
        if (c + 1 > s->ncolors) s->ncolors = c + 1;
        for (int a = 0; a < 8; a++) nodecol[m->conn[8 * (size_t)e + a]] |= (uint64_t)1 << c;
    }
    s->cstart = calloc((size_t)s->ncolors + 1, sizeof(int));
    s->order = malloc((size_t)ne * sizeof(int));
    if (!s->cstart || !s->order) goto oom;
    for (int e = 0; e < ne; e++) s->cstart[color[e] + 1]++;
    for (int c = 0; c < s->ncolors; c++) s->cstart[c + 1] += s->cstart[c];
    {
        int fill[64];
        memcpy(fill, s->cstart, (size_t)s->ncolors * sizeof(int));
        for (int e = 0; e < ne; e++) s->order[fill[color[e]]++] = e;
    }

    /* geometry cache for elements that are translated copies (voxel meshes) */
    s->geom = malloc((size_t)ne * sizeof(int));
    s->cache = malloc(64 * sizeof(Geom));
    keys = malloc(64 * sizeof *keys);
    hslot = malloc(GEOM_HASH * sizeof(int));
    if (!s->geom || !s->cache || !keys || !hslot) goto oom;
    int cache_cap = 64;
    for (int i = 0; i < GEOM_HASH; i++) hslot[i] = -1;
    for (int e = 0; e < ne; e++) {
        double X[8][3];
        element_coords(m, e, X);
        double size = 0;
        for (int k = 0; k < 3; k++) size = fmax(size, fabs(X[6][k] - X[0][k]));
        int64_t key[24];
        bool keyed = size > 0;
        for (int a = 0; keyed && a < 8; a++)
            for (int k = 0; k < 3; k++) {
                double r = (X[a][k] - X[0][k]) / (size * 1e-9);
                if (!(fabs(r) < 9e15)) keyed = false;
                else key[3 * a + k] = llround(r);
            }
        s->geom[e] = -1;
        if (keyed) {
            uint64_t h = key_hash(key) & (GEOM_HASH - 1);
            while (hslot[h] >= 0 && memcmp(keys[hslot[h]], key, sizeof key)) h = (h + 1) & (GEOM_HASH - 1);
            if (hslot[h] >= 0) {
                s->geom[e] = hslot[h];
                continue;
            }
            if (s->ncache < GEOM_CACHE_MAX) {
                if (s->ncache == cache_cap) {
                    cache_cap *= 2;
                    Geom *nc = realloc(s->cache, (size_t)cache_cap * sizeof(Geom));
                    int64_t (*nk)[24] = realloc(keys, (size_t)cache_cap * sizeof *keys);
                    if (nc) s->cache = nc;
                    if (nk) keys = nk;
                    if (!nc || !nk) goto oom;
                }
                if (!element_geometry(X, &s->cache[s->ncache])) {
                    snprintf(err, errlen, "element %d is inverted or degenerate", e);
                    goto fail;
                }
                memcpy(keys[s->ncache], key, sizeof key);
                hslot[h] = s->ncache;
                s->geom[e] = s->ncache++;
                continue;
            }
        }
        Geom tmp;
        if (!element_geometry(X, &tmp)) {
            snprintf(err, errlen, "element %d is inverted or degenerate", e);
            goto fail;
        }
    }
    s->coef = calloc(3 * (size_t)ne, sizeof(double));
    s->node_active = calloc((size_t)nn, 1);
    s->face_start = calloc((size_t)ne + 1, sizeof(int));
    s->face_cursor = malloc((size_t)ne * sizeof(int));
    s->src_slot = malloc((size_t)ne * sizeof(int));
    s->rhs = malloc((size_t)(s->neq ? s->neq : 1) * sizeof(double));
    s->x = malloc((size_t)(s->neq ? s->neq : 1) * sizeof(double));
    s->res = malloc((size_t)nn * sizeof(double));
    s->ax = malloc((size_t)(s->neq ? s->neq : 1) * sizeof(double));
    s->prev = malloc((size_t)(s->neq ? s->neq : 1) * sizeof(double));
    if (!s->coef || !s->node_active || !s->face_start || !s->face_cursor || !s->src_slot || !s->rhs || !s->x || !s->res || !s->ax || !s->prev) goto oom;
    for (int e = 0; e < ne; e++) s->src_slot[e] = -1;
    free(edofs), free(color), free(nodecol), free(keys), free(hslot);
    return s;
oom:
    snprintf(err, errlen, "out of memory in the thermal solver (%d nodes, %d elements)", nn, ne);
fail:
    free(edofs), free(color), free(nodecol), free(keys), free(hslot);
    thermal_free(s);
    return NULL;
}

/* ---- per-step preparation ------------------------------------------------------------------------ */

typedef struct {
    ThermalSolver *s;
    const ThermalModel *m;
    const double *T0, *T1;
    double dt, theta;
    bool steady;
    const int *order; /* elements of the colour being assembled */
} StepCtx;

static const Geom *geom_of(const ThermalSolver *s, const ThermalModel *m, int e, Geom *local) {
    if (s->geom[e] >= 0) return &s->cache[s->geom[e]];
    double X[8][3];
    element_coords(m, e, X);
    element_geometry(X, local);
    return local;
}

static bool group_faces(ThermalSolver *s, const ThermalModel *m, char *err, size_t errlen) {
    int ne = s->ne;
    memset(s->face_start, 0, ((size_t)ne + 1) * sizeof(int));
    for (int i = 0; i < m->nfaces; i++) {
        const ThermalFace *f = &m->faces[i];
        if (f->elem < 0 || f->elem >= ne || f->face > 5 || f->kind > THERMAL_OPEN) {
            snprintf(err, errlen, "boundary face %d is invalid (element %d, face %d, kind %d)", i, f->elem, f->face, f->kind);
            return false;
        }
        if ((f->kind == THERMAL_CONVECTION && !(f->value >= 0)) || (f->kind == THERMAL_RADIATION && !(f->value >= 0 && f->value <= 1)) ||
            (f->kind != THERMAL_FLUX && !(f->ambient > 0)) || !isfinite(f->value)) {
            snprintf(err, errlen, "boundary face %d has an invalid value (%g) or ambient temperature (%g K)", i, f->value, f->ambient);
            return false;
        }
        s->face_start[f->elem + 1]++;
    }
    for (int e = 0; e < ne; e++) s->face_start[e + 1] += s->face_start[e];
    if (m->nfaces > s->face_cap) {
        int *nl = realloc(s->face_list, (size_t)m->nfaces * sizeof(int));
        double *nh = realloc(s->face_h, FACE_COEFFS * (size_t)m->nfaces * sizeof(double));
        if (nl) s->face_list = nl;
        if (nh) s->face_h = nh;
        if (!nl || !nh) {
            snprintf(err, errlen, "out of memory");
            return false;
        }
        s->face_cap = m->nfaces;
    }
    memcpy(s->face_cursor, s->face_start, (size_t)ne * sizeof(int));
    for (int i = 0; i < m->nfaces; i++) s->face_list[s->face_cursor[m->faces[i].elem]++] = i;
    return true;
}

static bool source_position(const ThermalMovingSource *ms, double t, double p[3]) {
    if (!ms || ms->npoints < 1 || !ms->path || !(ms->power > 0)) return false;
    const double *P = ms->path;
    int n = ms->npoints, i = 0;
    if (t < P[3] || t > P[4 * (n - 1) + 3]) return false;
    while (i + 1 < n && P[4 * (i + 1) + 3] < t) i++;
    if (i + 1 >= n) {
        memcpy(p, P + 4 * i, 3 * sizeof(double));
        return true;
    }
    double t0 = P[4 * i + 3], t1 = P[4 * (i + 1) + 3], w = t1 > t0 ? (t - t0) / (t1 - t0) : 0;
    for (int k = 0; k < 3; k++) p[k] = P[4 * i + k] + w * (P[4 * (i + 1) + k] - P[4 * i + k]);
    return true;
}

static bool add_source(ThermalSolver *s, int e, const double vec[8]) {
    if (s->nsrc == s->src_cap) {
        int cap = s->src_cap ? 2 * s->src_cap : 64;
        int *ne = realloc(s->src_elem, (size_t)cap * sizeof(int));
        double *nv = realloc(s->src_vec, 8 * (size_t)cap * sizeof(double));
        if (ne) s->src_elem = ne;
        if (nv) s->src_vec = nv;
        if (!ne || !nv) return false;
        s->src_cap = cap;
    }
    s->src_elem[s->nsrc] = e;
    memcpy(s->src_vec + 8 * (size_t)s->nsrc, vec, 8 * sizeof(double));
    s->src_slot[e] = s->nsrc++;
    return true;
}

static bool prepare_source(ThermalSolver *s, const ThermalModel *m, double tsrc, ThermalStepStats *st, char *err, size_t errlen) {
    for (int i = 0; i < s->nsrc; i++) s->src_slot[s->src_elem[i]] = -1;
    s->nsrc = 0, s->src_scale = 0, s->src_power = 0;
    const ThermalMovingSource *ms = m->moving;
    double p[3];
    if (!source_position(ms, tsrc, p)) return true;
    if (!(ms->radius > 0) || !(ms->depth > 0)) {
        snprintf(err, errlen, "the moving source needs a positive radius and absorption depth");
        return false;
    }
    st->source_on = true;
    memcpy(st->source_position, p, sizeof p);
    double R = ms->radius, D = ms->depth, rcut = 3 * R, dcut = 10 * D, amp = 2 * ms->power / (M_PI * R * R * D), total = 0;
    for (int e = 0; e < m->nelems; e++) {
        if (m->active && !m->active[e]) continue;
        double X[8][3], lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
        element_coords(m, e, X);
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) lo[k] = fmin(lo[k], X[a][k]), hi[k] = fmax(hi[k], X[a][k]);
        if (lo[0] > p[0] + rcut || hi[0] < p[0] - rcut || lo[1] > p[1] + rcut || hi[1] < p[1] - rcut || lo[2] > p[2] || hi[2] < p[2] - dcut) continue;
        double vec[8] = {0}, pe = 0;
        for (int gp = 0; gp < 8; gp++) {
            double N[8], dN[8][3], J[3][3], x[3] = {0, 0, 0};
            hex8_shape(HEX8_XI[gp][0] * GPT, HEX8_XI[gp][1] * GPT, HEX8_XI[gp][2] * GPT, N, dN);
            double det = hex8_jacobian(X, dN, J, NULL);
            for (int a = 0; a < 8; a++)
                for (int k = 0; k < 3; k++) x[k] += N[a] * X[a][k];
            double dx = x[0] - p[0], dy = x[1] - p[1], d = p[2] - x[2], r2 = dx * dx + dy * dy;
            if (d < 0 || r2 > rcut * rcut || d > dcut) continue;
            double q = amp * exp(-2 * r2 / (R * R)) * exp(-d / D);
            for (int a = 0; a < 8; a++) vec[a] += q * N[a] * det;
            pe += q * det;
        }
        if (pe > 0) {
            if (!add_source(s, e, vec)) {
                snprintf(err, errlen, "out of memory");
                return false;
            }
            total += pe;
        }
    }
    if (total > 0) {
        s->src_power = total;
        s->src_scale = s->opt.raw_source ? 1 : ms->power / total;
    } else {
        /* the Gaussian falls between integration points: deposit its power in the element under the source point */
        int best = -1;
        double zp = p[2] - 1e-9 * fmax(1.0, fabs(p[2]));
        for (int e = 0; e < m->nelems && best < 0; e++) {
            if (m->active && !m->active[e]) continue;
            double X[8][3], lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
            element_coords(m, e, X);
            for (int a = 0; a < 8; a++)
                for (int k = 0; k < 3; k++) lo[k] = fmin(lo[k], X[a][k]), hi[k] = fmax(hi[k], X[a][k]);
            if (p[0] >= lo[0] && p[0] <= hi[0] && p[1] >= lo[1] && p[1] <= hi[1] && zp >= lo[2] && zp <= hi[2]) best = e;
        }
        if (best >= 0) {
            Geom local;
            const Geom *g = geom_of(s, m, best, &local);
            double vec[8];
            for (int a = 0; a < 8; a++) vec[a] = ms->power * g->ML[a] / g->vol;
            if (!add_source(s, best, vec)) {
                snprintf(err, errlen, "out of memory");
                return false;
            }
            s->src_power = ms->power, s->src_scale = 1;
            st->source_fallback = true;
        } else {
            st->source_missed = true;
        }
    }
    st->source_power_mesh = s->src_power;
    st->source_scale = s->src_scale;
    return true;
}

/* ---- element system ------------------------------------------------------------------------------ */

static void eval_coefficients(ThermalSolver *s, const StepCtx *c, int e, const Geom *g) {
    const ThermalModel *m = c->m;
    const int *cn = m->conn + 8 * (size_t)e;
    double t0 = 0, t1 = 0;
    for (int a = 0; a < 8; a++) t0 += c->T0[cn[a]], t1 += c->T1[cn[a]];
    t0 /= 8, t1 /= 8;
    const ThermalMaterial *mt = &m->mat[m->elem_mat ? m->elem_mat[e] : 0];
    double tm = 0.5 * (t0 + t1);
    s->coef[3 * (size_t)e] = thermal_table_eval(&mt->k, t1);
    /* secant capacity (H(T1) - H(T0)) / (T1 - T0): the discrete stored energy of the step is then exactly the
     * enthalpy change, which is what carries the latent heat without counting it twice. element_capacity restores
     * the older rho cp at the mean temperature. */
    s->coef[3 * (size_t)e + 1] = s->opt.element_capacity ? thermal_table_eval(&mt->rho, tm) * thermal_table_eval(&mt->cp, tm)
                                                         : thermal_secant_capacity(mt, t0, t1);
    s->coef[3 * (size_t)e + 2] = thermal_table_eval(&mt->k, t0);
    for (int k = s->face_start[e]; k < s->face_start[e + 1]; k++) {
        int fi = s->face_list[k];
        const ThermalFace *f = &m->faces[fi];
        double *fh = s->face_h + FACE_COEFFS * (size_t)fi;
        double h1 = 0, h0 = 0, a1 = f->ambient, a0 = f->ambient;
        if (f->kind == THERMAL_CONVECTION) {
            h1 = h0 = f->value;
        } else if (f->kind == THERMAL_RADIATION) {
            /* Newton tangent and flux at every face point, never at the face mean. At convergence the nodal
             * load is integral Ni eps sigma (T^4 - Ta^4) dA, with the same 3x3 rule used for its tangent and ledger. */
            const int *fn = HEX8_FACE_NODES[f->face];
            double Ta4 = pow(f->ambient,4), es = f->value * THERMAL_SIGMA;
            memset(fh,0,FACE_COEFFS * sizeof(double));
            for (int gp = 0; gp < 9; gp++) {
                const double *N = g->rad_N[gp];
                double t1 = 0, t0 = 0;
                for (int q = 0; q < 4; q++) t1 += N[q] * c->T1[cn[fn[q]]], t0 += N[q] * c->T0[cn[fn[q]]];
                /* Positive temperatures are physical; retain the old 1 K tangent safeguard for a cold iterate. */
                double h = 4 * es * pow(fmax(t1,1.0),3);
                double q1 = es * (pow(t1,4) - Ta4), q0 = es * (pow(t0,4) - Ta4), w = g->rad_w[f->face][gp];
                for (int a = 0; a < 4; a++) {
                    fh[16+a] += w * N[a] * (h * t1 - q1);
                    fh[20+a] += w * N[a] * q0;
                    for (int b = 0; b < 4; b++) fh[4*a+b] += w * h * N[a] * N[b];
                }
            }
            continue;
        }
        fh[0] = h1, fh[1] = a1, fh[2] = h0, fh[3] = a0;
    }
}

/* conduction matrix  Kc_ab = integral grad Na . K(T) grad Nb dV  at the nodal temperatures Te.
 * THERMAL_PROP_QUADRATURE evaluates K at every Gauss point from the temperature interpolated there; the cached
 * Laplacian g->K is still used whenever the conductivity is a single isotropic constant, where it is exact. */
static void conduction_matrix(const ThermalSolver *s, const ThermalMaterial *mt, const Geom *g, const double Te[8], double kelem, double Kc[64]) {
    bool aniso = thermal_is_anisotropic(mt);
    bool varying = mt->k.n > 1 || (aniso && (mt->k2.n > 1 || mt->k3.n > 1));
    if (s->opt.property_eval == THERMAL_PROP_ELEMENT && !aniso) {
        for (int i = 0; i < 64; i++) Kc[i] = kelem * g->K[i];
        return;
    }
    if (!aniso && !varying) {
        double kv = thermal_table_eval(&mt->k, Te[0]); /* constant table: any temperature gives the same value */
        for (int i = 0; i < 64; i++) Kc[i] = kv * g->K[i];
        return;
    }
    for (int i = 0; i < 64; i++) Kc[i] = 0;
    if (s->opt.property_eval == THERMAL_PROP_ELEMENT) { /* anisotropic, one tensor per element */
        double Tm = 0, K3[9];
        for (int a = 0; a < 8; a++) Tm += 0.125 * Te[a];
        thermal_conductivity(mt, Tm, K3);
        for (int gp = 0; gp < 8; gp++) {
            const double(*B)[3] = g->dNdx[gp];
            double w = g->detw[gp];
            for (int a = 0; a < 8; a++)
                for (int b = 0; b < 8; b++) {
                    double v = 0;
                    for (int i = 0; i < 3; i++)
                        for (int j = 0; j < 3; j++) v += B[a][i] * K3[3 * i + j] * B[b][j];
                    Kc[8 * a + b] += w * v;
                }
        }
        return;
    }
    for (int gp = 0; gp < 8; gp++) {
        double Tg = 0;
        for (int a = 0; a < 8; a++) Tg += GP_N[gp][a] * Te[a];
        const double(*B)[3] = g->dNdx[gp];
        double w = g->detw[gp];
        if (!aniso) {
            double kv = w * thermal_table_eval(&mt->k, Tg);
            for (int a = 0; a < 8; a++)
                for (int b = 0; b < 8; b++) Kc[8 * a + b] += kv * (B[a][0] * B[b][0] + B[a][1] * B[b][1] + B[a][2] * B[b][2]);
        } else {
            double K3[9];
            thermal_conductivity(mt, Tg, K3);
            for (int i = 0; i < 9; i++) K3[i] *= w;
            for (int a = 0; a < 8; a++) {
                double KB[3];
                for (int i = 0; i < 3; i++) KB[i] = K3[3 * i] * B[a][0] + K3[3 * i + 1] * B[a][1] + K3[3 * i + 2] * B[a][2];
                for (int b = 0; b < 8; b++) Kc[8 * a + b] += KB[0] * B[b][0] + KB[1] * B[b][1] + KB[2] * B[b][2];
            }
        }
    }
}

/* capacity of the step. Lumped (row-sum) or consistent; the coefficient is the secant capacity
 * (H(T1) - H(T0)) / (T1 - T0) evaluated at each Gauss point, so that
 *     sum_a CL_a (T1_a - T0_a) = sum_gp w_gp (H(T1_gp) - H(T0_gp))
 * exactly (row-sum lumping uses sum_b Nb = 1). The discrete stored energy of the step is therefore the Gauss
 * quadrature of the enthalpy change, which is what keeps latent heat consistent. */
static void capacity_matrix(const ThermalSolver *s, const ThermalMaterial *mt, const Geom *g, const double T0e[8], const double T1e[8], double celem,
                            double CL[8], double CC[64]) {
    bool per_gp = s->opt.property_eval == THERMAL_PROP_QUADRATURE && !s->opt.element_capacity && !linear_enthalpy(mt);
    if (!per_gp) {
        if (!s->opt.element_capacity && linear_enthalpy(mt)) celem = mt->rho.v[0] * mt->cp.v[0];
        for (int a = 0; a < 8; a++) CL[a] = celem * g->ML[a];
        if (CC)
            for (int i = 0; i < 64; i++) CC[i] = celem * g->M[i];
        return;
    }
    for (int a = 0; a < 8; a++) CL[a] = 0;
    if (CC)
        for (int i = 0; i < 64; i++) CC[i] = 0;
    for (int gp = 0; gp < 8; gp++) {
        double t0 = 0, t1 = 0;
        for (int a = 0; a < 8; a++) t0 += GP_N[gp][a] * T0e[a], t1 += GP_N[gp][a] * T1e[a];
        double cw = g->detw[gp] * thermal_secant_capacity(mt, t0, t1);
        for (int a = 0; a < 8; a++) {
            CL[a] += cw * GP_N[gp][a];
            if (CC)
                for (int b = 0; b < 8; b++) CC[8 * a + b] += cw * GP_N[gp][a] * GP_N[gp][b];
        }
    }
}

/* ---- advection ------------------------------------------------------------------------------------------------ */

static bool advected(const ThermalModel *m, int e) { return m->velocity && (!m->advect || m->advect[e]); }

static double advected_rho_cp(const ThermalMaterial *mt) { return mt->rho.v[0] * mt->cp.v[0]; } /* constant by validation */

/* the SUPG parameter of an element (see ThermalOptions): independent of the time step by design */
static double supg_tau(const ThermalSolver *s, const ThermalMaterial *mt, const Geom *g, const double ue[8][3], const double T1e[8]) {
    if (s->opt.advection_stabilization == THERMAL_STAB_NONE) return 0;
    double uc[3] = {0, 0, 0};
    for (int a = 0; a < 8; a++)
        for (int k = 0; k < 3; k++) uc[k] += 0.125 * ue[a][k];
    double un = sqrt(uc[0] * uc[0] + uc[1] * uc[1] + uc[2] * uc[2]);
    if (!(un > 0)) return 0;
    double sum = 0;
    for (int a = 0; a < 8; a++) sum += fabs(uc[0] * g->Bc[a][0] + uc[1] * g->Bc[a][1] + uc[2] * g->Bc[a][2]);
    if (!(sum > 0)) return 0;
    double h = 2 * un / sum, Tm = 0, K3[9];
    for (int a = 0; a < 8; a++) Tm += 0.125 * T1e[a];
    thermal_conductivity(mt, Tm, K3);
    double ku = 0; /* conductivity along the flow */
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) ku += uc[i] * K3[3 * i + j] * uc[j];
    ku /= un * un;
    double alpha = ku / advected_rho_cp(mt), a1 = 2 * un / h, a2 = 12 * alpha / (h * h); /* 3 * (4 alpha / h^2) */
    return 1.0 / sqrt(a1 * a1 + a2 * a2);
}

/* advection, SUPG capacity and SUPG source terms of one advected element (theta rule, lumped Galerkin capacity kept in
 * the caller) */
static void advection_terms(const ThermalSolver *s, const StepCtx *c, int e, const Geom *g, const ThermalMaterial *mt, const double T0e[8],
                            const double T1e[8], double Ae[64], double be[8]) {
    const ThermalModel *m = c->m;
    const int *cn = m->conn + 8 * (size_t)e;
    double ue[8][3];
    for (int a = 0; a < 8; a++)
        for (int k = 0; k < 3; k++) ue[a][k] = m->velocity[3 * (size_t)cn[a] + k];
    double rc = advected_rho_cp(mt), tau = supg_tau(s, mt, g, ue, T1e), th = c->theta, idt = c->steady ? 0 : 1.0 / c->dt;
    double q = m->elem_source ? m->elem_source[e] : 0;
    for (int gp = 0; gp < 8; gp++) {
        const double *N = GP_N[gp];
        const double(*B)[3] = g->dNdx[gp];
        double w = g->detw[gp], u[3] = {0, 0, 0};
        for (int b = 0; b < 8; b++)
            for (int k = 0; k < 3; k++) u[k] += N[b] * ue[b][k];
        double adv[8], gT0 = 0, T0g = 0;
        for (int a = 0; a < 8; a++) {
            adv[a] = u[0] * B[a][0] + u[1] * B[a][1] + u[2] * B[a][2];
            gT0 += adv[a] * T0e[a];
            T0g += N[a] * T0e[a];
        }
        for (int a = 0; a < 8; a++) {
            double wa = w * rc * (N[a] + tau * adv[a]); /* Petrov-Galerkin weight of row a */
            for (int b = 0; b < 8; b++) Ae[8 * a + b] += th * wa * adv[b];
            if (!c->steady) {
                double ws = w * rc * tau * adv[a] * idt; /* the streamline part of the capacity term (consistent SUPG) */
                for (int b = 0; b < 8; b++) Ae[8 * a + b] += ws * N[b];
                be[a] += ws * T0g;
                if (th < 1) be[a] -= (1 - th) * wa * gT0;
            }
            if (q != 0 && tau > 0) be[a] += w * tau * adv[a] * q;
        }
    }
}

/* weak inflow on an open face where u.n < 0: rho cp |u.n| (T_ambient - T); nothing where the fluid leaves */
static void open_face_terms(const StepCtx *c, int e, const Geom *g, const ThermalMaterial *mt, const ThermalFace *f, const double T0e[8], double Ae[64],
                            double be[8]) {
    const ThermalModel *m = c->m;
    const int *cn = m->conn + 8 * (size_t)e, *fn = HEX8_FACE_NODES[f->face];
    const double *nrm = g->face_n[f->face];
    double rc = advected_rho_cp(mt), th = c->theta;
    for (int gq = 0; gq < 4; gq++) {
        const double *Nq = g->face_N[gq];
        double u[3] = {0, 0, 0}, T0q = 0;
        for (int q = 0; q < 4; q++) {
            for (int k = 0; k < 3; k++) u[k] += Nq[q] * m->velocity[3 * (size_t)cn[fn[q]] + k];
            T0q += Nq[q] * T0e[fn[q]];
        }
        double un = u[0] * nrm[0] + u[1] * nrm[1] + u[2] * nrm[2];
        if (!(un < 0)) continue;
        double coef = rc * (-un) * g->face_w[f->face][gq];
        for (int q = 0; q < 4; q++) {
            for (int r = 0; r < 4; r++) Ae[8 * fn[q] + fn[r]] += th * coef * Nq[q] * Nq[r];
            be[fn[q]] += th * coef * f->ambient * Nq[q];
            if (!c->steady && th < 1) be[fn[q]] += (1 - th) * coef * (f->ambient - T0q) * Nq[q];
        }
    }
}

static void element_system(const ThermalSolver *s, const StepCtx *c, int e, const Geom *g, const double T0e[8], double Ae[64], double be[8]) {
    const ThermalModel *m = c->m;
    const ThermalMaterial *mt = &m->mat[m->elem_mat ? m->elem_mat[e] : 0];
    const int *cn = m->conn + 8 * (size_t)e;
    double T1e[8];
    for (int a = 0; a < 8; a++) T1e[a] = c->T1[cn[a]];
    double kelem = s->coef[3 * (size_t)e], rc = s->coef[3 * (size_t)e + 1], k0elem = s->coef[3 * (size_t)e + 2], th = c->theta;
    conduction_matrix(s, mt, g, T1e, kelem, Ae);
    for (int i = 0; i < 64; i++) Ae[i] *= th;
    for (int a = 0; a < 8; a++) be[a] = 0;
    if (!c->steady) {
        double CL[8], CC[64];
        capacity_matrix(s, mt, g, T0e, T1e, rc, CL, s->opt.consistent_capacity ? CC : NULL);
        double idt = 1.0 / c->dt;
        if (s->opt.consistent_capacity) {
            for (int a = 0; a < 8; a++)
                for (int b = 0; b < 8; b++) {
                    Ae[8 * a + b] += idt * CC[8 * a + b];
                    be[a] += idt * CC[8 * a + b] * T0e[b];
                }
        } else {
            for (int a = 0; a < 8; a++) {
                Ae[9 * a] += idt * CL[a];
                be[a] += idt * CL[a] * T0e[a];
            }
        }
        if (th < 1) {
            double K0[64];
            conduction_matrix(s, mt, g, T0e, k0elem, K0);
            for (int a = 0; a < 8; a++) {
                double s0 = 0;
                for (int b = 0; b < 8; b++) s0 += K0[8 * a + b] * T0e[b];
                be[a] -= (1 - th) * s0;
            }
        }
    }
    if (advected(m, e)) advection_terms(s, c, e, g, mt, T0e, T1e, Ae, be);
    if (m->elem_source && m->elem_source[e] != 0)
        for (int a = 0; a < 8; a++) be[a] += m->elem_source[e] * g->ML[a];
    if (s->src_slot[e] >= 0)
        for (int a = 0; a < 8; a++) be[a] += s->src_scale * s->src_vec[8 * (size_t)s->src_slot[e] + a];
    for (int kf = s->face_start[e]; kf < s->face_start[e + 1]; kf++) {
        int fi = s->face_list[kf];
        const ThermalFace *f = &m->faces[fi];
        const int *fn = HEX8_FACE_NODES[f->face];
        if (f->kind == THERMAL_OPEN) {
            open_face_terms(c, e, g, mt, f, T0e, Ae, be);
            continue;
        }
        if (f->kind == THERMAL_FLUX) {
            for (int q = 0; q < 4; q++) be[fn[q]] += f->value * g->Fv[f->face][q];
            continue;
        }
        const double *fh = s->face_h + FACE_COEFFS * (size_t)fi;
        if (f->kind == THERMAL_RADIATION) {
            for (int q = 0; q < 4; q++) {
                for (int r = 0; r < 4; r++) Ae[8 * fn[q] + fn[r]] += th * fh[4*q+r];
                be[fn[q]] += th * fh[16+q];
                if (!c->steady && th < 1) be[fn[q]] -= (1-th) * fh[20+q];
            }
            continue;
        }
        for (int q = 0; q < 4; q++) {
            for (int r = 0; r < 4; r++) Ae[8 * fn[q] + fn[r]] += th * fh[0] * g->F[f->face][4 * q + r];
            be[fn[q]] += th * fh[0] * fh[1] * g->Fv[f->face][q];
            if (!c->steady && th < 1) {
                double s0 = 0;
                for (int r = 0; r < 4; r++) s0 += g->F[f->face][4 * q + r] * T0e[fn[r]];
                be[fn[q]] += (1 - th) * fh[2] * (fh[3] * g->Fv[f->face][q] - s0);
            }
        }
    }
}

/* finite-conductance interfaces: for a pair (a, b) with conductance h and nodal area A the exchange is
 * Q_a = h A (T_b - T_a), so the pair adds the symmetric positive-semidefinite block h A [[1,-1],[-1,1]].
 * Assembled sequentially after the coloured element passes (few pairs, and they may join any two nodes). */
static void assemble_interfaces(ThermalSolver *s, const StepCtx *c) {
    const ThermalModel *m = c->m;
    double th = c->theta;
    for (int i = 0; i < m->ninterface; i++) {
        const ThermalInterfaceNode *it = &m->interfaces[i];
        int na = it->node_a, nb = it->node_b;
        if (!s->node_active[na] || !s->node_active[nb]) continue; /* a side that is not yet built exchanges nothing */
        double g = it->conductance * it->area;
        if (!(g > 0)) continue;
        int q[2] = {s->eq[na], s->eq[nb]};
        int nd[2] = {na, nb};
        bool freev[2] = {!(m->fixed && m->fixed[na]), !(m->fixed && m->fixed[nb])};
        double T0[2] = {c->T0[na], c->T0[nb]};
        double blk[2][2] = {{th * g, -th * g}, {-th * g, th * g}};
        for (int a = 0; a < 2; a++) {
            if (!freev[a] || q[a] < 0) continue;
            double rhs = 0;
            if (!c->steady && th < 1) rhs -= (1 - th) * (a == 0 ? g * (T0[0] - T0[1]) : g * (T0[1] - T0[0]));
            for (int b = 0; b < 2; b++) {
                if (freev[b] && q[b] >= 0) {
                    int64_t kk = csr_find(&s->A, q[a], q[b]);
                    if (kk >= 0) s->A.val[kk] += blk[a][b];
                } else {
                    rhs -= blk[a][b] * m->fixed_T[nd[b]];
                }
            }
            s->rhs[q[a]] += rhs;
        }
    }
}

static void assemble_range(void *vctx, int begin, int end, int tid) {
    (void)tid;
    StepCtx *c = vctx;
    ThermalSolver *s = c->s;
    const ThermalModel *m = c->m;
    for (int idx = begin; idx < end; idx++) {
        int e = c->order[idx];
        if (m->active && !m->active[e]) continue;
        Geom local;
        const Geom *g = geom_of(s, m, e, &local);
        eval_coefficients(s, c, e, g);
        const int *cn = m->conn + 8 * (size_t)e;
        double T0e[8], Ae[64], be[8];
        bool freev[8];
        for (int a = 0; a < 8; a++) T0e[a] = c->T0[cn[a]], freev[a] = !(m->fixed && m->fixed[cn[a]]);
        element_system(s, c, e, g, T0e, Ae, be);
        for (int a = 0; a < 8; a++) {
            if (!freev[a]) continue;
            int qa = s->eq[cn[a]];
            for (int b = 0; b < 8; b++) {
                if (freev[b]) {
                    int64_t kk = s->emap ? s->emap[64 * (size_t)e + 8 * a + b] : csr_find(&s->A, qa, s->eq[cn[b]]);
                    s->A.val[kk] += Ae[8 * a + b];
                } else {
                    be[a] -= Ae[8 * a + b] * m->fixed_T[cn[b]];
                }
            }
            s->rhs[qa] += be[a];
        }
    }
}

/* residuals with the coefficients of the last assembly: heat through prescribed nodes, and the energy terms */
static void energy_pass(ThermalSolver *s, const StepCtx *c, ThermalStepStats *st) {
    const ThermalModel *m = c->m;
    memset(s->res, 0, (size_t)s->nn * sizeof(double));
    double stored = 0, source = 0, boundary = 0, enthalpy = 0, liquid = 0, iface = 0, w = c->steady ? 1 : c->dt, th = c->theta;
    double advection = 0, inflow = 0, enth_in = 0, enth_out = 0;
    for (int e = 0; e < m->nelems; e++) {
        if (m->active && !m->active[e]) continue;
        Geom local;
        const Geom *g = geom_of(s, m, e, &local);
        const int *cn = m->conn + 8 * (size_t)e;
        double T0e[8], T1e[8], Ae[64], be[8];
        for (int a = 0; a < 8; a++) T0e[a] = c->T0[cn[a]], T1e[a] = c->T1[cn[a]];
        element_system(s, c, e, g, T0e, Ae, be);
        for (int a = 0; a < 8; a++) {
            double r = -be[a];
            for (int b = 0; b < 8; b++) r += Ae[8 * a + b] * T1e[b];
            s->res[cn[a]] += r;
        }
        double rc = s->coef[3 * (size_t)e + 1];
        const ThermalMaterial *mt = &m->mat[m->elem_mat ? m->elem_mat[e] : 0];
        if (!c->steady) {
            double CL[8], CC[64];
            capacity_matrix(s, mt, g, T0e, T1e, rc, CL, s->opt.consistent_capacity ? CC : NULL);
            double e_stored = 0;
            if (s->opt.consistent_capacity) {
                for (int a = 0; a < 8; a++)
                    for (int b = 0; b < 8; b++) e_stored += CC[8 * a + b] * (T1e[b] - T0e[b]);
            } else {
                for (int a = 0; a < 8; a++) e_stored += CL[a] * (T1e[a] - T0e[a]);
            }
            stored += e_stored;
            int sg = m->elem_group ? m->elem_group[e] : -1;
            if (sg >= 0 && sg < THERMAL_MAX_GROUPS) st->group_stored[sg] += e_stored;
            /* independent measure: the enthalpy H(T) integrated over the element by the same Gauss rule, with no
             * reference to the capacity matrix that was assembled. Agreement is evidence of energy conservation;
             * a small linear-system residual on its own is not. */
            if (linear_enthalpy(mt)) { /* H = rho cp T: the Gauss sum collapses onto the cached row sums */
                double rcp = mt->rho.v[0] * mt->cp.v[0];
                for (int a = 0; a < 8; a++) enthalpy += rcp * g->ML[a] * (T1e[a] - T0e[a]);
            } else {
                for (int gp = 0; gp < 8; gp++) {
                    double t0 = 0, t1 = 0;
                    for (int a = 0; a < 8; a++) t0 += GP_N[gp][a] * T0e[a], t1 += GP_N[gp][a] * T1e[a];
                    enthalpy += g->detw[gp] * (thermal_enthalpy(mt, t1) - thermal_enthalpy(mt, t0));
                }
            }
        }
        if (s->any_phase)
            for (int gp = 0; gp < 8; gp++) {
                double t1 = 0;
                for (int a = 0; a < 8; a++) t1 += GP_N[gp][a] * T1e[a];
                liquid += g->detw[gp] * thermal_liquid_fraction(mt, t1);
            }
        if (advected(m, e)) {
            /* the Galerkin part only: the streamline terms sum to zero over the element's nodes */
            double rcA = advected_rho_cp(mt), adv = 0;
            for (int gp = 0; gp < 8; gp++) {
                double u[3] = {0, 0, 0}, g1[3] = {0, 0, 0}, g0[3] = {0, 0, 0};
                for (int b = 0; b < 8; b++)
                    for (int k = 0; k < 3; k++) {
                        u[k] += GP_N[gp][b] * m->velocity[3 * (size_t)cn[b] + k];
                        g1[k] += g->dNdx[gp][b][k] * T1e[b];
                        g0[k] += g->dNdx[gp][b][k] * T0e[b];
                    }
                double d1 = u[0] * g1[0] + u[1] * g1[1] + u[2] * g1[2], d0 = u[0] * g0[0] + u[1] * g0[1] + u[2] * g0[2];
                adv += g->detw[gp] * rcA * (c->steady ? d1 : th * d1 + (1 - th) * d0);
            }
            double ea = -w * adv;
            advection += ea;
            int ag = m->elem_group ? m->elem_group[e] : -1;
            if (ag >= 0 && ag < THERMAL_MAX_GROUPS) st->group_advection[ag] += ea;
        }
        double e_source = 0;
        if (m->elem_source) e_source += w * m->elem_source[e] * g->vol;
        if (s->src_slot[e] >= 0)
            for (int a = 0; a < 8; a++) e_source += w * s->src_scale * s->src_vec[8 * (size_t)s->src_slot[e] + a];
        source += e_source;
        int eg = m->elem_group ? m->elem_group[e] : -1;
        if (eg >= 0 && eg < THERMAL_MAX_GROUPS) st->group_source[eg] += e_source;
        for (int kf = s->face_start[e]; kf < s->face_start[e + 1]; kf++) {
            int fi = s->face_list[kf];
            const ThermalFace *f = &m->faces[fi];
            const int *fn = HEX8_FACE_NODES[f->face];
            int fg = m->face_group ? m->face_group[fi] : -1;
            if (f->kind == THERMAL_OPEN) {
                double rcA = advected_rho_cp(mt), e_in = 0;
                for (int gq = 0; gq < 4; gq++) {
                    const double *Nq = g->face_N[gq], *nrm = g->face_n[f->face];
                    double u[3] = {0, 0, 0}, T1q = 0, T0q = 0;
                    for (int q = 0; q < 4; q++) {
                        for (int k = 0; k < 3; k++) u[k] += Nq[q] * m->velocity[3 * (size_t)cn[fn[q]] + k];
                        T1q += Nq[q] * T1e[fn[q]], T0q += Nq[q] * T0e[fn[q]];
                    }
                    double un = u[0] * nrm[0] + u[1] * nrm[1] + u[2] * nrm[2], Tq = c->steady ? T1q : th * T1q + (1 - th) * T0q;
                    double flux = w * rcA * un * g->face_w[f->face][gq];
                    if (un < 0) {
                        e_in += -flux * (f->ambient - Tq);
                        enth_in += -flux * f->ambient;
                    } else {
                        enth_out += flux * Tq;
                    }
                }
                inflow += e_in;
                if (fg >= 0 && fg < THERMAL_MAX_GROUPS) st->group_inflow[fg] += e_in;
                continue;
            }
            if (f->kind == THERMAL_FLUX) {
                double qf = w * f->value * g->area[f->face];
                boundary += qf;
                if (fg >= 0 && fg < THERMAL_MAX_GROUPS) st->group_boundary[fg] += qf;
                continue;
            }
            const double *fh = s->face_h + FACE_COEFFS * (size_t)fi;
            if (f->kind == THERMAL_RADIATION) {
                double out1 = 0, out0 = 0;
                for (int q = 0; q < 4; q++) {
                    out1 -= fh[16+q], out0 += fh[20+q];
                    for (int r = 0; r < 4; r++) out1 += fh[4*q+r] * T1e[fn[r]];
                }
                double qf = -w * (th * out1 + (c->steady ? 0 : (1-th) * out0));
                boundary += qf;
                if (fg >= 0 && fg < THERMAL_MAX_GROUPS) st->group_boundary[fg] += qf;
                continue;
            }
            double a1 = 0, a0 = 0;
            for (int q = 0; q < 4; q++) {
                a1 += fh[1] * g->Fv[f->face][q];
                a0 += fh[3] * g->Fv[f->face][q];
                for (int r = 0; r < 4; r++) {
                    a1 -= g->F[f->face][4 * q + r] * T1e[fn[r]];
                    a0 -= g->F[f->face][4 * q + r] * T0e[fn[r]];
                }
            }
            double qf = w * th * fh[0] * a1;
            if (!c->steady && th < 1) qf += w * (1 - th) * fh[2] * a0;
            boundary += qf;
            if (fg >= 0 && fg < THERMAL_MAX_GROUPS) st->group_boundary[fg] += qf;
        }
    }
    if (m->node_power)
        for (int n = 0; n < s->nn; n++) {
            if (!s->node_active[n] || m->node_power[n] == 0) continue;
            if (m->fixed && m->fixed[n]) continue; /* a load on a prescribed node goes into its reaction, not into the body */
            s->res[n] -= m->node_power[n];
            source += w * m->node_power[n];
        }
    for (int i = 0; i < m->ninterface; i++) {
        const ThermalInterfaceNode *it = &m->interfaces[i];
        int na = it->node_a, nb = it->node_b;
        if (!s->node_active[na] || !s->node_active[nb]) continue;
        double g = it->conductance * it->area;
        if (!(g > 0)) continue;
        double q1 = g * (c->T1[na] - c->T1[nb]), q0 = g * (c->T0[na] - c->T0[nb]);
        double q = c->steady ? q1 : th * q1 + (1 - th) * q0; /* out of node a, into node b */
        s->res[na] += q;
        s->res[nb] -= q;
        int ig = m->interface_group ? m->interface_group[i] : -1;
        if (ig >= 0 && ig < THERMAL_MAX_GROUPS) st->interface_heat[ig] -= w * q; /* from side b into side a */
        /* both sides are inside the model, so a closed set of pairs transports energy without creating any: the
         * reported figure is the net into the active region, which is non-zero only when one side is prescribed */
        if (m->fixed && m->fixed[na] && !(m->fixed && m->fixed[nb])) iface += w * q;
        else if (m->fixed && m->fixed[nb] && !(m->fixed && m->fixed[na])) iface -= w * q;
    }
    double prescribed = 0;
    for (int n = 0; n < s->nn; n++)
        if (s->node_active[n] && m->fixed && m->fixed[n]) {
            prescribed += w * s->res[n];
            int ng = m->node_group ? m->node_group[n] : -1;
            if (ng >= 0 && ng < THERMAL_MAX_GROUPS) st->group_prescribed[ng] += w * s->res[n];
        }
    st->stored_energy = stored;
    st->source_energy = source;
    st->boundary_energy = boundary;
    st->prescribed_energy = prescribed;
    st->interface_energy = iface;
    st->enthalpy_change = enthalpy;
    st->liquid_volume = liquid;
    st->advection_energy = advection;
    st->inflow_energy = inflow;
    st->enthalpy_inflow = enth_in;
    st->enthalpy_outflow = enth_out;
    st->divergence_energy = advection + inflow - (enth_in - enth_out);
    double big = fmax(fabs(stored), fmax(fabs(source), fmax(fabs(boundary), fabs(prescribed))));
    big = fmax(big, fmax(fabs(advection), fabs(inflow)));
    st->balance_error = big > 0 ? fabs(stored - source - boundary - prescribed - advection - inflow) / big : 0;
    double eb = fmax(fabs(stored), fabs(enthalpy));
    st->enthalpy_mismatch = eb > 0 ? fabs(stored - enthalpy) / eb : 0;
}

bool thermal_step(ThermalSolver *s, const ThermalModel *m, const double *T0, double *T1, double t, double dt, ThermalStepStats *st, char *err,
                  size_t errlen) {
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    memset(st, 0, sizeof *st);
    st->failure = THERMAL_FAIL_INPUT; /* until the step is known to be well posed */
    if (m->nnodes != s->nn || m->nelems != s->ne) {
        snprintf(err, errlen, "the mesh changed after the thermal solver was created");
        return false;
    }
    if (T0 == T1) {
        snprintf(err, errlen, "thermal_step needs distinct start and end temperature arrays");
        return false;
    }
    for (int n = 0; n < s->nn; n++)
        if (!isfinite(T0[n])) {
            st->failure = THERMAL_FAIL_STATE;
            snprintf(err, errlen, "the start temperature of node %d is not finite", n);
            return false;
        }
    bool steady = !(dt > 0);
    StepCtx c = {s, m, T0, T1, dt, steady ? 1.0 : s->opt.theta, steady, NULL};
    if (!group_faces(s, m, err, errlen)) {
        if (strstr(err, "out of memory")) st->failure = THERMAL_FAIL_RESOURCE;
        return false;
    }
    memset(s->node_active, 0, (size_t)s->nn);
    for (int e = 0; e < m->nelems; e++)
        if (!m->active || m->active[e])
            for (int a = 0; a < 8; a++) s->node_active[m->conn[8 * (size_t)e + a]] = 1;
    bool sink = false, nonlinear = false, advective = false;
    for (int n = 0; n < s->nn && !sink; n++) sink = s->node_active[n] && m->fixed && m->fixed[n];
    for (int e = 0; e < m->nelems && m->velocity; e++) {
        if ((m->active && !m->active[e]) || !advected(m, e)) continue;
        advective = true;
        const ThermalMaterial *mt = &m->mat[m->elem_mat ? m->elem_mat[e] : 0];
        if (!linear_enthalpy(mt)) {
            snprintf(err, errlen,
                     "element %d is advected but its material has a temperature-dependent density or specific heat or a latent heat: advection is "
                     "implemented for constant-property fluids only",
                     e);
            return false;
        }
        const int *cn = m->conn + 8 * (size_t)e;
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++)
                if (!isfinite(m->velocity[3 * (size_t)cn[a] + k])) {
                    snprintf(err, errlen, "the velocity at node %d is not finite", cn[a]);
                    return false;
                }
    }
    for (int i = 0; i < m->nfaces; i++) {
        const ThermalFace *f = &m->faces[i];
        if (m->active && !m->active[f->elem]) continue;
        if (f->kind == THERMAL_OPEN) {
            if (!advected(m, f->elem)) {
                snprintf(err, errlen, "face %d is an open boundary but element %d is not advected (an open boundary needs a velocity field)", i, f->elem);
                return false;
            }
            sink = true; /* entering fluid fixes a temperature */
            continue;
        }
        if (f->kind != THERMAL_FLUX && f->value > 0) sink = true;
        if (f->kind == THERMAL_RADIATION) nonlinear = true;
    }
    for (int i = 0; i < m->nmat; i++) {
        const ThermalMaterial *mt = &m->mat[i];
        /* any property that depends on temperature makes the step nonlinear - a latent heat above all, because the
         * secant capacity is only the true enthalpy slope once the iteration has converged on T1 */
        nonlinear |= mt->k.n > 1 || mt->cp.n > 1 || mt->rho.n > 1 || mt->k2.n > 1 || mt->k3.n > 1 || has_phase_change(mt);
    }
    if (steady && !sink) {
        snprintf(err, errlen, "the steady state is undefined without a prescribed temperature, convection or radiation");
        return false;
    }
    if (!prepare_source(s, m, steady ? t : t + 0.5 * dt, st, err, errlen)) {
        st->failure = strstr(err, "out of memory") ? THERMAL_FAIL_RESOURCE : THERMAL_FAIL_INPUT;
        return false;
    }
    st->failure = THERMAL_FAIL_NONE;
    st->linear_method = advective ? "bicgstab/ilu0" : "pcg/jacobi";
    memcpy(T1, T0, (size_t)s->nn * sizeof(double));
    for (int n = 0; n < s->nn; n++)
        if (s->node_active[n] && m->fixed && m->fixed[n]) T1[n] = m->fixed_T[n];
    bool converged = false;
    /* Aitken relaxation of the Picard iteration.
     *
     * The iteration is a fixed point T <- phi(T) whose correction is d = phi(T) - T. For a lumped node crossing the
     * mushy zone of a melting material the map has derivative -B/Q with B = rho L (Ts - T0) / (Tl - Ts) and Q the
     * heat delivered in the step, so it DIVERGES whenever the latent heat still to be absorbed exceeds the heat
     * supplied - exactly the case of an element that crosses the whole mushy interval in one step. Relaxing by
     * omega makes the derivative 1 - omega (1 + B/Q), and Aitken's formula
     *     omega <- -omega (d_prev . (d - d_prev)) / |d - d_prev|^2
     * estimates the omega that cancels it, from the corrections themselves and with no extra solves. It reduces to
     * omega = 1 for a linear problem, so nothing changes for conduction without phase change. */
    double change = 0, omega = 1, omega_min = 1;
    bool have_prev = false;
    struct timespec ta, tb;
    for (int it = 1; it <= s->opt.max_picard; it++) {
        clock_gettime(CLOCK_MONOTONIC, &ta);
        csr_zero(&s->A);
        memset(s->rhs, 0, (size_t)s->neq * sizeof(double));
        for (int col = 0; col < s->ncolors; col++) {
            StepCtx sub = c;
            sub.order = s->order + s->cstart[col];
            int cnt = s->cstart[col + 1] - s->cstart[col];
            if (s->opt.pool && cnt > 512) pool_for(s->opt.pool, cnt, 256, assemble_range, &sub);
            else assemble_range(&sub, 0, cnt, 0);
        }
        if (m->ninterface > 0) assemble_interfaces(s, &c);
        if (m->node_power)
            for (int n = 0; n < s->nn; n++) {
                int q = s->eq[n];
                if (q < 0 || !s->node_active[n] || (m->fixed && m->fixed[n]) || m->node_power[n] == 0) continue;
                s->rhs[q] += m->node_power[n];
            }
        for (int n = 0; n < s->nn; n++) {
            int q = s->eq[n];
            if (q < 0) continue;
            bool fixed = s->node_active[n] && m->fixed && m->fixed[n];
            if (!s->node_active[n] || fixed) {
                s->A.val[s->diag[q]] = 1;
                s->rhs[q] = fixed ? m->fixed_T[n] : T0[n];
            }
            s->x[q] = T1[n];
        }
        clock_gettime(CLOCK_MONOTONIC, &tb);
        st->seconds_assembly += elapsed(ta, tb);
        st->assemblies++;
        /* solve for the correction of the current iterate: the tolerance then applies to the temperature change within the
         * step instead of absolute temperatures, which keeps the discrete energy balance closed to round-off */
        csr_spmv(&s->A, s->x, s->ax, s->opt.pool);
        double rn = 0;
        for (int q = 0; q < s->neq; q++) {
            s->rhs[q] -= s->ax[q];
            rn += s->rhs[q] * s->rhs[q];
            s->ax[q] = 0;
        }
        change = 0;
        if (!isfinite(rn)) {
            st->failure = THERMAL_FAIL_STATE;
            snprintf(err, errlen, "the residual became non-finite in iteration %d", it);
            return false;
        }
        if (rn > 0) {
            SolveStats ss;
            char lerr[256] = "";
            bool solved;
            if (advective) { /* nonsymmetric: conjugate gradients would have no convergence guarantee */
                KrylovOptions ko = {KRYLOV_PRECOND_ILU0, s->opt.pcg_tol, s->opt.pcg_max_iter, 0, s->opt.pool};
                solved = bicgstab_solve(&s->A, s->rhs, s->ax, &ko, &ss, lerr, sizeof lerr);
            } else {
                PcgOptions po = {PRECOND_JACOBI, NULL, s->opt.pcg_tol, s->opt.pcg_max_iter, s->opt.pool, NULL, NULL};
                solved = pcg_solve(&s->A, s->rhs, s->ax, &po, &ss, lerr, sizeof lerr);
            }
            st->linear_iterations += ss.iterations;
            st->linear_residual = ss.rel_residual;
            if (!solved) {
                /* pcg_solve reports breakdown, non-convergence and allocation failure through the same flag */
                st->failure = strstr(lerr, "out of memory") ? THERMAL_FAIL_RESOURCE : THERMAL_FAIL_LINEAR;
                snprintf(err, errlen, "the linear solver failed in Picard iteration %d: %s", it, lerr[0] ? lerr : "no convergence");
                return false;
            }
            if (have_prev) {
                double num = 0, den = 0;
                for (int q = 0; q < s->neq; q++) {
                    double dd = s->ax[q] - s->prev[q];
                    num += s->prev[q] * dd;
                    den += dd * dd;
                }
                if (den > 0) {
                    double w = -omega * num / den;
                    if (isfinite(w)) omega = fmin(1.5, fmax(0.02, w)); /* clamped: Aitken may overshoot early on */
                }
            }
            omega_min = fmin(omega_min, omega);
            bool finite = true;
            for (int n = 0; n < s->nn; n++) {
                int q = s->eq[n];
                if (q < 0) continue;
                change = fmax(change, fabs(s->ax[q])); /* the undamped correction is the convergence measure */
                T1[n] += omega * s->ax[q];
                finite &= isfinite(T1[n]);
            }
            if (!finite) {
                st->failure = THERMAL_FAIL_STATE;
                snprintf(err, errlen, "the temperature became non-finite in Picard iteration %d", it);
                return false;
            }
            memcpy(s->prev, s->ax, (size_t)s->neq * sizeof(double));
            have_prev = true;
        }
        clock_gettime(CLOCK_MONOTONIC, &ta);
        st->seconds_solve += elapsed(tb, ta);
        st->picard_iterations = it;
        if (!nonlinear || change <= s->opt.picard_tol) {
            converged = true;
            break;
        }
    }
    st->relaxation = omega_min;
    if (!converged) {
        st->failure = THERMAL_FAIL_NONLINEAR;
        snprintf(err, errlen,
                 "temperature iterations did not converge: largest change %.3g K after %d iterations (relaxation fell to %.3g). Reduce the time step, "
                 "raise max_picard, or widen the mushy interval: an element that crosses the whole solidus-liquidus range in one step makes the "
                 "latent-heat iteration stiff.",
                 change, s->opt.max_picard, omega_min);
        return false;
    }
    clock_gettime(CLOCK_MONOTONIC, &ta);
    energy_pass(s, &c, st);
    clock_gettime(CLOCK_MONOTONIC, &tb);
    st->seconds_energy = elapsed(ta, tb);
    st->tmin = INFINITY, st->tmax = -INFINITY;
    for (int n = 0; n < s->nn; n++)
        if (s->node_active[n]) st->tmin = fmin(st->tmin, T1[n]), st->tmax = fmax(st->tmax, T1[n]);
    if (st->tmin < 0) {
        /* backward Euler with lumped capacity satisfies a discrete maximum principle and cannot get here; Crank-Nicolson
         * with a large step can ring below absolute zero near a sharp front. Either way the state is not physical. */
        st->failure = THERMAL_FAIL_STATE;
        snprintf(err, errlen, "the solution fell to %.4g K, below absolute zero (Crank-Nicolson ringing at a sharp front, or a bad input)", st->tmin);
        return false;
    }
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    st->seconds = (double)(ts1.tv_sec - ts0.tv_sec) + 1e-9 * (double)(ts1.tv_nsec - ts0.tv_nsec);
    return true;
}

/* ---- temporal error estimate ------------------------------------------------------------------------ */

void thermal_error_norm(const ThermalModel *m, const double *T_start, const double *T_coarse, const double *T_fine, double factor, double T_ref,
                        const ThermalTimeTolerance *tol, ThermalErrorNorm *out) {
    init_gp_shapes();
    memset(out, 0, sizeof *out);
    out->worst_node = out->worst_elem = -1;
    int nn = m->nnodes, ne = m->nelems;
    unsigned char *act = calloc((size_t)(nn > 0 ? nn : 1), 1);
    if (!act) {
        out->err = out->err_T = out->err_H = INFINITY; /* no memory for the estimate: reject rather than accept blindly */
        return;
    }
    for (int e = 0; e < ne; e++)
        if (!m->active || m->active[e])
            for (int a = 0; a < 8; a++) act[m->conn[8 * (size_t)e + a]] = 1;
    double sum2 = 0;
    for (int n = 0; n < nn; n++) {
        if (!act[n] || (m->fixed && m->fixed[n])) continue; /* inactive and prescribed nodes carry no temporal error */
        double d = factor * fabs(T_fine[n] - T_coarse[n]);
        double scale = tol->temperature + tol->relative * fmax(fabs(T_start[n] - T_ref), fabs(T_fine[n] - T_ref));
        double r = scale > 0 ? d / scale : (d > 0 ? INFINITY : 0);
        if (!isfinite(d)) r = INFINITY;
        sum2 += r * r;
        out->nodes++;
        out->max_dT = fmax(out->max_dT, d);
        if (out->worst_node < 0 || r > out->err_T) out->err_T = r, out->worst_node = n;
    }
    out->rms_T = out->nodes ? sqrt(sum2 / out->nodes) : 0;
    free(act);
    if (!tol->temperature_only) {
        /* the enthalpy of every material at the reference temperature, and its absolute tolerance */
        int nm = m->nmat;
        double *Href = malloc(2 * (size_t)(nm > 0 ? nm : 1) * sizeof(double));
        if (!Href) {
            out->err = out->err_H = INFINITY;
            return;
        }
        double *atolH = Href + nm;
        for (int i = 0; i < nm; i++) {
            Href[i] = thermal_enthalpy(&m->mat[i], T_ref);
            atolH[i] = tol->enthalpy > 0 ? tol->enthalpy
                                         : tol->temperature * thermal_table_eval(&m->mat[i].rho, T_ref) * thermal_table_eval(&m->mat[i].cp, T_ref);
        }
        for (int e = 0; e < ne; e++) {
            if (m->active && !m->active[e]) continue;
            int mi = m->elem_mat ? m->elem_mat[e] : 0;
            if (mi < 0 || mi >= nm) continue;
            const ThermalMaterial *mt = &m->mat[mi];
            const int *cn = m->conn + 8 * (size_t)e;
            bool lin = linear_enthalpy(mt);
            double rcp = lin ? mt->rho.v[0] * mt->cp.v[0] : 0;
            for (int gp = 0; gp < 8; gp++) {
                double ts = 0, tc = 0, tf = 0;
                for (int a = 0; a < 8; a++) {
                    double w = GP_N[gp][a];
                    ts += w * T_start[cn[a]], tc += w * T_coarse[cn[a]], tf += w * T_fine[cn[a]];
                }
                double hs, hc, hf;
                if (lin) hs = rcp * ts, hc = rcp * tc, hf = rcp * tf;
                else hs = thermal_enthalpy(mt, ts), hc = thermal_enthalpy(mt, tc), hf = thermal_enthalpy(mt, tf);
                double d = factor * fabs(hf - hc);
                double href = lin ? rcp * T_ref : Href[mi];
                double scale = atolH[mi] + tol->relative * fmax(fabs(hs - href), fabs(hf - href));
                double r = scale > 0 ? d / scale : (d > 0 ? INFINITY : 0);
                if (!isfinite(d)) r = INFINITY;
                out->gauss_points++;
                out->max_dH = fmax(out->max_dH, d);
                if (out->worst_elem < 0 || r > out->err_H) out->err_H = r, out->worst_elem = e;
            }
        }
        free(Href);
    }
    out->err = fmax(out->err_T, out->err_H);
}
