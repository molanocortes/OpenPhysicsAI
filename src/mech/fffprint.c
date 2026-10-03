/* fffprint.c - layer-by-layer fused-filament print simulation (see fffprint.h) */
#include "fffprint.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../fem/dense.h"
#include "../fem/hex8.h"
#include "../fem/solid.h"
#include "../fem/thermal.h"
#include "../threads.h"

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

/* Thermal increment of an element from T0 to T1 with a temperature-dependent modulus: the free thermal strain integral
 * alpha dT and the alpha-weighted modulus of the path, integral E alpha dT / integral alpha dT. Solving the increment with
 * that modulus and that strain is exact in both limits: a fully restrained element gains the stress integral E alpha dT
 * (stiffening while it cools does not load it), and a free element takes exactly its thermal strain, so a free
 * heating-cooling cycle returns it to its size. Above the relaxation temperature the material flows: that part of the
 * path adds neither strain nor stress. */
static void thermal_increment(const FffMaterial *mat, double T0, double T1, double *eps, double *E_eff) {
    double lo = fmin(T0, T1), hi = fmin(fmax(T0, T1), mat->T_relax), E1 = mat_eval(&mat->E, T1);
    if (!(E1 >= mat->E_floor)) E1 = mat->E_floor;
    *eps = 0, *E_eff = E1;
    if (!(hi > lo)) return;
    double sa = 0, sea = 0;
    for (double a = lo; a < hi;) {
        double b = hi;
        for (int i = 0; i < mat->E.n; i++)
            if (mat->E.t[i] > a && mat->E.t[i] < b) b = mat->E.t[i];
        for (int i = 0; i < mat->alpha.n; i++)
            if (mat->alpha.t[i] > a && mat->alpha.t[i] < b) b = mat->alpha.t[i];
        double Ea = mat_eval(&mat->E, a), Eb = mat_eval(&mat->E, b);
        /* Clipping a linear modulus at E_floor adds a breakpoint inside its table interval. */
        if ((Ea < mat->E_floor && Eb > mat->E_floor) || (Ea > mat->E_floor && Eb < mat->E_floor)) {
            double cross = a + (b - a) * (mat->E_floor - Ea) / (Eb - Ea);
            if (cross > a && cross < b) b = cross;
        }
        double mid = 0.5 * (a + b), aa = mat_eval(&mat->alpha, a), am = mat_eval(&mat->alpha, mid);
        double ab = mat_eval(&mat->alpha, b), w = (b - a) / 6.0;
        /* E and alpha are linear here, so their product is quadratic: Simpson is exact. */
        sa += w * (aa + 4 * am + ab);
        sea += w * (fmax(mat->E_floor, mat_eval(&mat->E, a)) * aa +
                    4 * fmax(mat->E_floor, mat_eval(&mat->E, mid)) * am +
                    fmax(mat->E_floor, mat_eval(&mat->E, b)) * ab);
        a = b;
    }
    *eps = T1 > T0 ? sa : -sa;
    if (fabs(sa) > 1e-30) *E_eff = sea / sa;
}

static void elem_X(const FffMesh *M, int e, double X[8][3]) {
    for (int a = 0; a < 8; a++)
        for (int k = 0; k < 3; k++) X[a][k] = M->xyz[3 * (size_t)M->conn[8 * (size_t)e + a] + (size_t)k];
}

static double elem_mean_T(const FffMesh *M, int e, const double *T) {
    double s = 0;
    for (int a = 0; a < 8; a++) s += T[M->conn[8 * (size_t)e + a]];
    return s / 8;
}

/* Independently integrate physical enthalpy at the same eight points as the thermal element. At birth, the incoming
 * material would be uniformly at T_nozzle, while conforming interface nodes keep their old temperatures. The nodal
 * pulse sums to that exact missing enthalpy because sum_a N_a (T_nozzle - T_a) = T_nozzle - T_gp. */
static double element_heat(const FffMesh *M, int e, const ThermalMaterial *mat, const double *T, double T_ref,
                           double T_nozzle, double *pulse, double *incoming) {
    double X[8][3], energy = 0;
    elem_X(M, e, X);
    const double gp = 1.0 / sqrt(3.0), href = thermal_enthalpy(mat, T_ref);
    if (incoming) *incoming = 0;
    for (int g = 0; g < 8; g++) {
        double N[8], dN[8][3], J[3][3], dNdx[8][3], Tg = 0;
        hex8_shape(HEX8_XI[g][0] * gp, HEX8_XI[g][1] * gp, HEX8_XI[g][2] * gp, N, dN);
        double det = hex8_jacobian(X, dN, J, dNdx);
        for (int a = 0; a < 8; a++) Tg += N[a] * T[M->conn[8 * (size_t)e + (size_t)a]];
        energy += det * (thermal_enthalpy(mat, Tg) - href);
        if (incoming) *incoming += det * (thermal_enthalpy(mat, T_nozzle) - href);
        if (pulse) {
            double secant = thermal_secant_capacity(mat, Tg, T_nozzle);
            for (int a = 0; a < 8; a++) {
                int n = M->conn[8 * (size_t)e + (size_t)a];
                pulse[n] += det * N[a] * secant * (T_nozzle - T[n]);
            }
        }
    }
    return energy;
}

/* ------------------------------------------------------------------------------------------------ stress history */

enum { MODE_BED = 0, MODE_RELEASE, MODE_FREE };

struct FffMech {
    FffMesh mesh;
    FffMaterial mat;
    int solver;      /* SolidSolver of the increments */
    double pcg_tol;
    ThreadPool *pool; /* the increments assemble, solve and recover in parallel, like every other structural solve */
    double E_ref;
    unsigned char *bed, *active, *born;
    double *T_birth, *T_prev;
    double *u, *stress, *f_relax, *bed_reaction;
    int support[3]; /* isostatic support nodes after release (A: xyz, B: yz, C: z) */
    double *escale; /* nelems: stiffness multiplier (homogenised supports), NULL: 1 */
    bool released;
    double release_reaction;
    int solves;
};

void fff_mech_free(FffMech *m) {
    if (!m) return;
    if (m->pool) pool_destroy(m->pool);
    free(m->bed), free(m->active), free(m->born), free(m->T_birth), free(m->T_prev);
    free(m->u), free(m->stress), free(m->f_relax), free(m->bed_reaction), free(m->escale);
    free(m);
}

FffMech *fff_mech_new(const FffMesh *mesh, const FffMaterial *mat, const unsigned char *bed_node, char *err, size_t errlen) {
    if (!mat->E.n || !mat->alpha.n) {
        snprintf(err, errlen, "the material needs Young's modulus and a thermal expansion coefficient");
        return NULL;
    }
    FffMech *m = calloc(1, sizeof *m);
    size_t nn = (size_t)mesh->nnodes, ne = (size_t)mesh->nelems;
    if (!m) goto oom;
    m->mesh = *mesh, m->mat = *mat;
    m->E_ref = mat_eval(&mat->E, 293.15);
    int threads = cpu_perf_count();
    m->pool = pool_create(threads > 0 ? threads : 1);
    m->bed = malloc(nn ? nn : 1), m->active = calloc(ne ? ne : 1, 1), m->born = calloc(ne ? ne : 1, 1);
    m->T_birth = calloc(ne ? ne : 1, sizeof(double)), m->T_prev = calloc(ne ? ne : 1, sizeof(double));
    m->u = calloc(3 * nn, sizeof(double)), m->stress = calloc(48 * ne, sizeof(double));
    m->f_relax = calloc(3 * nn, sizeof(double)), m->bed_reaction = calloc(3 * nn, sizeof(double));
    if (!m->bed || !m->active || !m->born || !m->T_birth || !m->T_prev || !m->u || !m->stress || !m->f_relax || !m->bed_reaction) goto oom;
    memcpy(m->bed, bed_node, nn);
    m->support[0] = m->support[1] = m->support[2] = -1;
    if (!(m->E_ref > 0)) {
        snprintf(err, errlen, "Young's modulus at 20 degC is not positive");
        fff_mech_free(m);
        return NULL;
    }
    return m;
oom:
    snprintf(err, errlen, "out of memory for the stress history (%d elements)", mesh->nelems);
    fff_mech_free(m);
    return NULL;
}

void fff_mech_activate(FffMech *m, int e, double T_birth) {
    if (e < 0 || e >= m->mesh.nelems || m->active[e]) return;
    m->active[e] = 1, m->born[e] = 1, m->T_birth[e] = T_birth;
}

/* nodal forces of an element's Gauss-point stresses: f_a = sum_g B_a^T sigma_g detJ_g (unit weights) */
static void internal_force(const FffMesh *M, int e, const double *sig, double *f) {
    double X[8][3];
    elem_X(M, e, X);
    const double G = 1 / sqrt(3.0);
    for (int g = 0; g < 8; g++) {
        double dN[8][3], J[3][3], dNdx[8][3];
        hex8_shape(HEX8_XI[g][0] * G, HEX8_XI[g][1] * G, HEX8_XI[g][2] * G, NULL, dN);
        double detJ = hex8_jacobian(X, dN, J, dNdx);
        const double *s = sig + 6 * g; /* xx yy zz xy yz zx */
        for (int a = 0; a < 8; a++) {
            size_t n = (size_t)M->conn[8 * (size_t)e + a];
            f[3 * n] += (dNdx[a][0] * s[0] + dNdx[a][1] * s[3] + dNdx[a][2] * s[5]) * detJ;
            f[3 * n + 1] += (dNdx[a][1] * s[1] + dNdx[a][0] * s[3] + dNdx[a][2] * s[4]) * detJ;
            f[3 * n + 2] += (dNdx[a][2] * s[2] + dNdx[a][1] * s[4] + dNdx[a][0] * s[5]) * detJ;
        }
    }
}

static bool choose_support(FffMech *m, const unsigned char *used_in, char *err, size_t errlen) {
    const FffMesh *M = &m->mesh;
    int A = -1, B = -1, C = -1;
    /* the nodes it may use: the printed nodes on the bed, or, once the supports are gone and the part no longer
     * touches the bed, the part's lowest plane */
    unsigned char *base = malloc((size_t)(M->nnodes ? M->nnodes : 1));
    if (!base) {
        snprintf(err, errlen, "out of memory choosing the isostatic support");
        return false;
    }
    bool any_bed = false;
    double zlow = INFINITY;
    for (int n = 0; n < M->nnodes; n++)
        if (used_in[n]) any_bed |= m->bed[n] != 0, zlow = fmin(zlow, M->xyz[3 * (size_t)n + 2]);
    for (int n = 0; n < M->nnodes; n++)
        base[n] = used_in[n] && (any_bed ? m->bed[n] != 0 : fabs(M->xyz[3 * (size_t)n + 2] - zlow) < 1e-9 + 1e-9 * fabs(zlow));
    const unsigned char *used = base;
    for (int n = 0; n < M->nnodes; n++) {
        if (!used[n]) continue;
        const double *p = M->xyz + 3 * (size_t)n;
        if (A < 0 || p[0] + p[1] < M->xyz[3 * (size_t)A] + M->xyz[3 * (size_t)A + 1]) A = n;
    }
    if (A < 0) {
        free(base);
        snprintf(err, errlen, "no printed node on the bed to support the released part");
        return false;
    }
    const double *pa = M->xyz + 3 * (size_t)A;
    double best = -1;
    for (int n = 0; n < M->nnodes; n++) { /* B: farthest along x from A */
        if (!used[n]) continue;
        double d = M->xyz[3 * (size_t)n] - pa[0] - 0.01 * fabs(M->xyz[3 * (size_t)n + 1] - pa[1]);
        if (d > best) best = d, B = n;
    }
    const double *pb = M->xyz + 3 * (size_t)B;
    double ab[3] = {pb[0] - pa[0], pb[1] - pa[1], 0}, lab = sqrt(ab[0] * ab[0] + ab[1] * ab[1]);
    best = -1;
    for (int n = 0; n < M->nnodes; n++) { /* C: farthest from the line AB */
        if (!used[n] || lab == 0) continue;
        const double *p = M->xyz + 3 * (size_t)n;
        double d = fabs((p[0] - pa[0]) * ab[1] - (p[1] - pa[1]) * ab[0]) / lab;
        if (d > best) best = d, C = n;
    }
    free(base);
    if (B < 0 || C < 0 || !(lab > 0) || !(best > 1e-9)) {
        snprintf(err, errlen, "the bed contact is a line or a point: an isostatic support cannot be placed");
        return false;
    }
    m->support[0] = A, m->support[1] = B, m->support[2] = C;
    return true;
}

static bool solve_increment(FffMech *m, const double *T, int mode, char *err, size_t errlen) {
    const FffMesh *M = &m->mesh;
    int ne = M->nelems, nn = M->nnodes, na = 0;
    for (int e = 0; e < ne; e++) na += m->active[e] != 0;
    if (!na) return true;
    bool ok = false;
    int *conn = malloc(8 * (size_t)na * sizeof(int)), *map = malloc((size_t)na * sizeof(int));
    double *scale = malloc((size_t)na * sizeof(double)), *eps0 = calloc(6 * (size_t)na, sizeof(double)), *force = calloc(3 * (size_t)nn, sizeof(double));
    unsigned char *fixed = calloc(3 * (size_t)nn, 1), *used = calloc((size_t)nn, 1);
    SolidResult res;
    double worst_dt = 0; /* the largest element temperature change this increment carries (profiling) */
    memset(&res, 0, sizeof res);
    if (!conn || !map || !scale || !eps0 || !force || !fixed || !used) {
        snprintf(err, errlen, "out of memory for a stress increment");
        goto done;
    }
    for (int e = 0, a = 0; e < ne; e++) {
        if (!m->active[e]) continue;
        map[a] = e;
        memcpy(conn + 8 * (size_t)a, M->conn + 8 * (size_t)e, 8 * sizeof(int));
        double Te = elem_mean_T(M, e, T), E = mat_eval(&m->mat.E, Te);
        if (!(E >= m->mat.E_floor)) E = m->mat.E_floor;
        if (mode != MODE_RELEASE) {
            double T0 = m->born[e] ? m->T_birth[e] : m->T_prev[e], de;
            thermal_increment(&m->mat, T0, Te, &de, &E);
            eps0[6 * (size_t)a] = eps0[6 * (size_t)a + 1] = eps0[6 * (size_t)a + 2] = de;
            if (!m->born[e]) worst_dt = fmax(worst_dt, fabs(Te - m->T_prev[e]));
            m->T_prev[e] = Te, m->born[e] = 0;
        }
        scale[a] = E / m->E_ref * (m->escale ? m->escale[e] : 1.0);
        for (int k = 0; k < 8; k++) used[M->conn[8 * (size_t)e + k]] = 1;
        a++;
    }
    for (size_t i = 0; i < 3 * (size_t)nn; i++) force[i] = m->f_relax[i];
    memset(m->f_relax, 0, 3 * (size_t)nn * sizeof(double));
    if (mode == MODE_BED) {
        for (int n = 0; n < nn; n++)
            if (used[n] && m->bed[n]) fixed[3 * n] = fixed[3 * n + 1] = fixed[3 * n + 2] = 1;
    } else {
        if (m->support[0] < 0 && !choose_support(m, used, err, errlen)) goto done;
        int A = m->support[0], B = m->support[1], C = m->support[2];
        fixed[3 * A] = fixed[3 * A + 1] = fixed[3 * A + 2] = 1;
        fixed[3 * B + 1] = fixed[3 * B + 2] = 1;
        fixed[3 * C + 2] = 1;
        if (mode == MODE_RELEASE)
            for (size_t i = 0; i < 3 * (size_t)nn; i++) force[i] -= m->bed_reaction[i];
    }
    SolidMaterial smat = {m->E_ref, m->mat.nu, mat_eval(&m->mat.rho, 293.15)};
    HexModel hm = {nn, na, M->xyz, conn, NULL, 1, &smat, HEX8_INCOMPATIBLE, scale};
    SolidLoads L = {fixed, NULL, force, eps0, {0, 0, 0}};
    SolidOptions opt = {(SolidSolver)m->solver, m->pcg_tol > 0 ? m->pcg_tol : 1e-10, 20000, 0, m->pool, NULL, NULL};
    double t_solve = now_s();
    if (getenv("NAVIER_PRINT_SOLVE_LOG")) fprintf(stderr, "  dT %8.3f K", worst_dt);
    if (!solid_solve(&hm, &L, &opt, &res, err, errlen)) goto done;
    m->solves++;
    if (getenv("NAVIER_PRINT_SOLVE_LOG")) /* profiling: one line per stress increment */
        fprintf(stderr, "  solve %3d  mode %d  elements %6d  equations %7d  %-18s %6.3f s (assembly and recovery %5.3f s) factor %6.1f MB\n", m->solves,
                mode, na, res.neq, res.stats.method, res.stats.seconds, now_s() - t_solve - res.stats.seconds, res.stats.factor_mb);
    for (int n = 0; n < nn; n++)
        if (used[n])
            for (int k = 0; k < 3; k++) m->u[3 * n + k] += res.u[3 * n + k];
    for (int a = 0; a < na; a++)
        for (int k = 0; k < 48; k++) m->stress[48 * (size_t)map[a] + (size_t)k] += res.gp_stress[48 * (size_t)a + (size_t)k];
    if (mode == MODE_BED) {
        for (int n = 0; n < nn; n++)
            if (used[n] && m->bed[n])
                for (int k = 0; k < 3; k++) m->bed_reaction[3 * n + k] += res.reaction[3 * n + k];
    } else {
        double rmax = 0;
        for (int i = 0; i < 3; i++)
            for (int k = 0; k < 3; k++) rmax = fmax(rmax, fabs(res.reaction[3 * m->support[i] + k]));
        if (mode == MODE_RELEASE) {
            m->release_reaction = rmax;
            memset(m->bed_reaction, 0, 3 * (size_t)nn * sizeof(double));
            m->released = true;
        }
    }
    /* stress relaxation of material reheated above the relaxation temperature */
    for (int a = 0; a < na; a++) {
        int e = map[a];
        if (elem_mean_T(M, e, T) <= m->mat.T_relax) continue;
        internal_force(M, e, m->stress + 48 * (size_t)e, m->f_relax);
        memset(m->stress + 48 * (size_t)e, 0, 48 * sizeof(double));
    }
    ok = true;
done:
    solid_result_free(&res);
    free(conn), free(map), free(scale), free(eps0), free(force), free(fixed), free(used);
    return ok;
}

bool fff_mech_increment(FffMech *m, const double *T, char *err, size_t errlen) {
    return solve_increment(m, T, m->released ? MODE_FREE : MODE_BED, err, errlen);
}

bool fff_mech_increment_opt(FffMech *m, const double *T, double skip_dt, bool *skipped, char *err, size_t errlen) {
    if (skipped) *skipped = false;
    if (skip_dt > 0) { /* nothing new, and no element changed temperature enough to matter */
        double worst = 0;
        bool born = false;
        for (int e = 0; e < m->mesh.nelems && !born; e++) {
            if (!m->active[e]) continue;
            born = m->born[e];
            worst = fmax(worst, fabs(elem_mean_T(&m->mesh, e, T) - m->T_prev[e]));
        }
        if (!born && worst < skip_dt) {
            if (skipped) *skipped = true;
            if (getenv("NAVIER_PRINT_SOLVE_LOG")) fprintf(stderr, "  skip        largest element temperature change %.3f K < %.3f K\n", worst, skip_dt);
            return true;
        }
    }
    return fff_mech_increment(m, T, err, errlen);
}

bool fff_mech_release(FffMech *m, const double *T, char *err, size_t errlen) {
    if (m->released) return true;
    return solve_increment(m, T, MODE_RELEASE, err, errlen);
}

bool fff_mech_set_scale(FffMech *m, const double *scale) {
    free(m->escale), m->escale = NULL;
    if (!scale) return true;
    m->escale = malloc((size_t)(m->mesh.nelems ? m->mesh.nelems : 1) * sizeof(double));
    if (!m->escale) return false;
    memcpy(m->escale, scale, (size_t)m->mesh.nelems * sizeof(double));
    return true;
}

bool fff_mech_remove(FffMech *m, const int *elems, int n, const double *T, double *tear_max, double *tear_sum, char *err, size_t errlen) {
    const FffMesh *M = &m->mesh;
    size_t nn = (size_t)M->nnodes;
    double *f = calloc(3 * nn, sizeof(double));
    unsigned char *leave = calloc(nn, 1), *stay = calloc(nn, 1);
    if (!f || !leave || !stay) {
        free(f), free(leave), free(stay);
        snprintf(err, errlen, "out of memory removing elements");
        return false;
    }
    int removed = 0;
    for (int i = 0; i < n; i++) {
        int e = elems[i];
        if (e < 0 || e >= M->nelems || !m->active[e]) continue;
        internal_force(M, e, m->stress + 48 * (size_t)e, f);
        for (int a = 0; a < 8; a++) leave[M->conn[8 * (size_t)e + (size_t)a]] = 1;
        memset(m->stress + 48 * (size_t)e, 0, 48 * sizeof(double));
        m->active[e] = 0;
        removed++;
    }
    for (int e = 0; e < M->nelems; e++)
        if (m->active[e])
            for (int a = 0; a < 8; a++) stay[M->conn[8 * (size_t)e + (size_t)a]] = 1;
    double tm = 0, ts = 0;
    for (size_t q = 0; q < nn; q++) {
        if (!(leave[q] && stay[q])) continue;
        double g = sqrt(f[3 * q] * f[3 * q] + f[3 * q + 1] * f[3 * q + 1] + f[3 * q + 2] * f[3 * q + 2]);
        tm = fmax(tm, g), ts += g;
    }
    if (tear_max) *tear_max = tm;
    if (tear_sum) *tear_sum = ts;
    for (size_t i = 0; i < 3 * nn; i++) m->f_relax[i] += f[i];
    free(f), free(leave), free(stay);
    if (!removed) return true;
    m->support[0] = m->support[1] = m->support[2] = -1; /* chosen again on what stays */
    return solve_increment(m, T, MODE_FREE, err, errlen);
}

const double *fff_mech_u(const FffMech *m) { return m->u; }
const double *fff_mech_stress(const FffMech *m) { return m->stress; }
const unsigned char *fff_mech_active(const FffMech *m) { return m->active; }
double fff_mech_release_reaction(const FffMech *m) { return m->release_reaction; }
int fff_mech_solves(const FffMech *m) { return m->solves; }

void fff_mech_von_mises(const FffMech *m, double *vm) {
    for (int e = 0; e < m->mesh.nelems; e++) {
        vm[e] = 0;
        if (!m->active[e]) continue;
        for (int g = 0; g < 8; g++) vm[e] += von_mises(m->stress + 48 * (size_t)e + 6 * (size_t)g) / 8;
    }
}

double fff_mech_bed_reaction(const FffMech *m, double r[3]) {
    r[0] = r[1] = r[2] = 0;
    for (int n = 0; n < m->mesh.nnodes; n++)
        for (int k = 0; k < 3; k++) r[k] += m->bed_reaction[3 * n + k];
    return fmax(fabs(r[0]), fmax(fabs(r[1]), fabs(r[2])));
}

/* ------------------------------------------------------------------------------------------------ the print */

/* ratio r > 1 with dt0 (r^S - 1) / (r - 1) = duration: a short first step, then geometric growth */
static double geometric_ratio(double duration, double dt0, int S) {
    if (dt0 * S >= duration) return 1.0;
    double lo = 1.0, hi = 2.0;
    while (dt0 * (pow(hi, S) - 1) / (hi - 1) < duration) hi *= 2;
    for (int it = 0; it < 100; it++) {
        double mid = 0.5 * (lo + hi);
        if (dt0 * (pow(mid, S) - 1) / (mid - 1) < duration) lo = mid;
        else hi = mid;
    }
    return 0.5 * (lo + hi);
}

typedef struct FaceKey {
    int n[4];
    int elem, face;
} FaceKey;

static int cmp_int(const void *a, const void *b) { return (*(const int *)a > *(const int *)b) - (*(const int *)a < *(const int *)b); }

static uint64_t face_hash(const int *n) {
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < 4; i++) h = (h ^ (uint64_t)(uint32_t)n[i]) * 1099511628211ULL;
    return h;
}

int *fff_face_neighbours(const FffMesh *M) {
    size_t ne = (size_t)M->nelems, cap = 1;
    while (cap < 12 * ne) cap <<= 1;
    FaceKey *tab = malloc(cap * sizeof *tab);
    int *nb = malloc(6 * ne * sizeof(int));
    if (!tab || !nb) {
        free(tab), free(nb);
        return NULL;
    }
    for (size_t i = 0; i < cap; i++) tab[i].elem = -1;
    for (size_t i = 0; i < 6 * ne; i++) nb[i] = -1;
    for (size_t e = 0; e < ne; e++)
        for (int f = 0; f < 6; f++) {
            int key[4];
            for (int i = 0; i < 4; i++) key[i] = M->conn[8 * e + (size_t)HEX8_FACE_NODES[f][i]];
            qsort(key, 4, sizeof(int), cmp_int);
            size_t h = (size_t)(face_hash(key) & (cap - 1));
            for (;; h = (h + 1) & (cap - 1)) {
                if (tab[h].elem < 0) {
                    memcpy(tab[h].n, key, sizeof key);
                    tab[h].elem = (int)e, tab[h].face = f;
                    break;
                }
                if (!memcmp(tab[h].n, key, sizeof key)) {
                    nb[6 * e + (size_t)f] = tab[h].elem;
                    nb[6 * (size_t)tab[h].elem + (size_t)tab[h].face] = (int)e;
                    break;
                }
            }
        }
    free(tab);
    return nb;
}

int *fff_plan(const FffMesh *M, double layer_height, FffPlanStats *stats) {
    size_t nn = (size_t)M->nnodes, ne = (size_t)M->nelems;
    FffPlanStats st = {0, 0, 0};
    if (stats) *stats = st;
    if (!(layer_height > 0) || !ne) return NULL;
    double zmin = INFINITY, zmax = -INFINITY;
    for (size_t n = 0; n < nn; n++) zmin = fmin(zmin, M->xyz[3 * n + 2]), zmax = fmax(zmax, M->xyz[3 * n + 2]);
    double tol = 1e-6 * fmax(zmax - zmin, 1e-3);
    int *geo = malloc(ne * sizeof(int)), *dep = malloc(ne * sizeof(int)), *queue = malloc(ne * sizeof(int)), *nb = fff_face_neighbours(M);
    unsigned char *on_bed = calloc(ne, 1);
    if (!geo || !dep || !queue || !nb || !on_bed) {
        free(geo), free(dep), free(queue), free(nb), free(on_bed);
        return NULL;
    }
    for (size_t e = 0; e < ne; e++) {
        double zc = 0;
        int nbed = 0;
        for (int a = 0; a < 8; a++) {
            double z = M->xyz[3 * (size_t)M->conn[8 * e + (size_t)a] + 2];
            zc += z / 8, nbed += fabs(z - zmin) < tol;
        }
        geo[e] = (int)floor((zc - zmin) / layer_height);
        if (geo[e] < 0) geo[e] = 0;
        if (geo[e] + 1 > st.nlayers) st.nlayers = geo[e] + 1;
        on_bed[e] = nbed >= 4, dep[e] = -1;
    }
    for (int k = 0; k < st.nlayers; k++) {
        int qh = 0, qt = 0;
        for (size_t e = 0; e < ne; e++) { /* seeds: on the bed or against material deposited before */
            if (dep[e] >= 0 || geo[e] > k) continue;
            bool supported = on_bed[e];
            for (int f = 0; f < 6 && !supported; f++) {
                int o = nb[6 * e + (size_t)f];
                supported = o >= 0 && dep[o] >= 0 && dep[o] < k;
            }
            if (supported) dep[e] = k, queue[qt++] = (int)e;
        }
        while (qh < qt) { /* and everything of this layer or below that rests on them */
            int e = queue[qh++];
            for (int f = 0; f < 6; f++) {
                int o = nb[6 * (size_t)e + (size_t)f];
                if (o >= 0 && dep[o] < 0 && geo[o] <= k) dep[o] = k, queue[qt++] = o;
            }
        }
    }
    for (size_t e = 0; e < ne; e++) {
        if (dep[e] < 0) st.unprintable++;
        else if (dep[e] > geo[e]) st.late++;
    }
    free(geo), free(queue), free(nb), free(on_bed);
    if (stats) *stats = st;
    return dep;
}

/* A support element exchanges heat with the air over the surface of its pattern (surface per volume x its volume),
 * spread evenly over its six faces: convection with the film coefficient and grey radiation, the radiation split into
 * as many terms as keep each emissivity within 1. Returns the new face count. */
static int support_faces(const FffMesh *M, int e, const FffProcess *P, double emis, ThermalFace *faces, int nf) {
    double X[8][3];
    elem_X(M, e, X);
    double area = M->band_props[6 * (size_t)M->support_band[e] + 5] * hex8_volume(X), per_face = 0;
    for (int f = 0; f < 6; f++) {
        double t[3] = {0, 0, 0}, fe[24], a = 0, nrm[3];
        hex8_face_load(X, f, t, fe, &a, nrm);
        per_face += a;
    }
    if (!(area > 0) || !(per_face > 0)) return nf;
    double kappa = area / per_face; /* pattern surface per unit of voxel face */
    int nrad = P->radiation ? (int)ceil(kappa * emis - 1e-12) : 0;
    for (int f = 0; f < 6; f++) {
        faces[nf++] = (ThermalFace){e, (unsigned char)f, THERMAL_CONVECTION, P->h_conv * kappa, P->T_ambient};
        for (int r = 0; r < nrad; r++) faces[nf++] = (ThermalFace){e, (unsigned char)f, THERMAL_RADIATION, kappa * emis / nrad, P->T_ambient};
    }
    return nf;
}

bool fff_simulate(const FffMesh *M, const FffMaterial *mat, const FffProcess *P, const FffCallbacks *cb, FffSummary *sum, char *err,
                  size_t errlen) {
    FffFrameFn fn = cb ? cb->frame : NULL;
    FffStepFn step_fn = cb ? cb->step : NULL;
    void *ctx = cb ? cb->ctx : NULL;
    memset(sum, 0, sizeof *sum);
    if (!(P->layer_height > 0) || !(P->deposition_rate > 0) || P->thermal_substeps < 2 || !(P->T_nozzle > P->T_ambient)) {
        snprintf(err, errlen, "the process needs a positive layer height and deposition rate, at least 2 thermal substeps and a nozzle hotter than the air");
        return false;
    }
    if (!mat->rho.n || !mat->cp.n || !mat->k.n) {
        snprintf(err, errlen, "the material needs density, specific heat and conductivity");
        return false;
    }
    size_t nn = (size_t)M->nnodes, ne = (size_t)M->nelems;
    bool ok = false;
    double zmin = INFINITY, zmax = -INFINITY;
    for (size_t n = 0; n < nn; n++) zmin = fmin(zmin, M->xyz[3 * n + 2]), zmax = fmax(zmax, M->xyz[3 * n + 2]);
    double tol = 1e-6 * fmax(zmax - zmin, 1e-3);
    unsigned char *bed = calloc(nn, 1), *active = calloc(ne, 1), *node_on = calloc(nn, 1), *fixed = calloc(nn, 1);
    FffPlanStats plan;
    int *layer = fff_plan(M, P->layer_height, &plan), *nb = fff_face_neighbours(M);
    double *vol = malloc(ne * sizeof(double)), *T = malloc(nn * sizeof(double)), *T1 = malloc(nn * sizeof(double));
    double *fixed_T = malloc(nn * sizeof(double)), *vm = calloc(ne, sizeof(double));
    double *birth_heat = calloc(nn, sizeof(double)), *node_power = calloc(nn, sizeof(double));
    size_t face_cap = 12 * ne; /* two per exposed face; a support element takes 6 x (1 + its radiation terms) */
    if (M->support_band && M->band_props)
        for (size_t e = 0; e < ne; e++)
            if (M->support_band[e] >= 0) {
                double X[8][3];
                elem_X(M, (int)e, X);
                double L = cbrt(hex8_volume(X)), kap = M->band_props[6 * (size_t)M->support_band[e] + 5] * L / 6.0;
                face_cap += 6 * (size_t)(2 + (int)ceil(kap));
            }
    ThermalFace *faces = malloc(face_cap * sizeof *faces);
    ThermalMaterial *tmats = NULL; /* the part and the support bands */
    double *tvals = NULL;
    int *emat = NULL;
    ThermalSolver *solver = NULL;
    FffMech *mech = NULL;
    if (!bed || !active || !node_on || !fixed || !layer || !nb || !vol || !T || !T1 || !fixed_T || !vm || !faces || !birth_heat || !node_power) {
        snprintf(err, errlen, "out of memory for the print simulation (%zu elements)", ne);
        goto done;
    }
    for (size_t n = 0; n < nn; n++) {
        bed[n] = fabs(M->xyz[3 * n + 2] - zmin) < tol;
        fixed[n] = bed[n], fixed_T[n] = P->T_bed, T[n] = P->T_ambient;
    }
    const int nlayers = plan.nlayers;
    sum->unprintable_elements = plan.unprintable, sum->late_elements = plan.late;
    for (size_t e = 0; e < ne; e++) {
        double X[8][3];
        elem_X(M, (int)e, X);
        vol[e] = hex8_volume(X);
    }
    mech = fff_mech_new(M, mat, bed, err, errlen);
    if (!mech) goto done;
    mech->solver = P->solver, mech->pcg_tol = P->pcg_tol;
    /* material 0 is the part; material 1 + b a support band: orthotropic conductivity (x, y, z) and a density scaled by
     * its capacity fraction, cp unchanged */
    int nb_ = M->support_band && M->nbands > 0 ? M->nbands : 0;
    tmats = calloc((size_t)(nb_ + 1), sizeof *tmats);
    tvals = malloc((size_t)(nb_ ? nb_ : 1) * 4 * MAT_TABLE_MAX * sizeof(double));
    emat = nb_ ? malloc(ne * sizeof(int)) : NULL;
    if (!tmats || !tvals || (nb_ && !emat)) {
        snprintf(err, errlen, "out of memory for the support materials");
        goto done;
    }
    ThermalMaterial *tmp_ = tmats;
    tmp_[0].k = (ThermalTable){mat->k.n, mat->k.t, mat->k.v};
    tmp_[0].cp = (ThermalTable){mat->cp.n, mat->cp.t, mat->cp.v};
    tmp_[0].rho = (ThermalTable){mat->rho.n, mat->rho.t, mat->rho.v};
    for (int b = 0; b < nb_; b++) {
        const double *q = M->band_props + 6 * (size_t)b;
        double *kx = tvals + (size_t)b * 4 * MAT_TABLE_MAX, *ky = kx + MAT_TABLE_MAX, *kz = ky + MAT_TABLE_MAX, *rh = kz + MAT_TABLE_MAX;
        for (int i = 0; i < mat->k.n; i++) kx[i] = q[0] * mat->k.v[i], ky[i] = q[1] * mat->k.v[i], kz[i] = q[2] * mat->k.v[i];
        for (int i = 0; i < mat->rho.n; i++) rh[i] = q[3] * mat->rho.v[i];
        tmp_[b + 1] = tmp_[0];
        tmp_[b + 1].k = (ThermalTable){mat->k.n, mat->k.t, kx};
        tmp_[b + 1].k2 = (ThermalTable){mat->k.n, mat->k.t, ky};
        tmp_[b + 1].k3 = (ThermalTable){mat->k.n, mat->k.t, kz};
        tmp_[b + 1].rho = (ThermalTable){mat->rho.n, mat->rho.t, rh};
    }
    if (nb_) {
        for (size_t e = 0; e < ne; e++) emat[e] = M->support_band[e] >= 0 ? 1 + M->support_band[e] : 0;
        double *sc = malloc(ne * sizeof(double));
        if (!sc) {
            snprintf(err, errlen, "out of memory for the support stiffness");
            goto done;
        }
        for (size_t e = 0; e < ne; e++) sc[e] = M->support_band[e] >= 0 ? M->band_props[6 * (size_t)M->support_band[e] + 4] : 1.0;
        bool okscale = fff_mech_set_scale(mech, sc);
        free(sc);
        if (!okscale) {
            snprintf(err, errlen, "out of memory for the support stiffness");
            goto done;
        }
    }
    ThermalModel tmod;
    memset(&tmod, 0, sizeof tmod);
    tmod.nnodes = M->nnodes, tmod.nelems = M->nelems, tmod.xyz = M->xyz, tmod.conn = M->conn;
    tmod.nmat = nb_ + 1, tmod.mat = tmats, tmod.elem_mat = emat;
    tmod.active = active, tmod.fixed = fixed, tmod.fixed_T = fixed_T, tmod.faces = faces;
    ThermalOptions topt;
    memset(&topt, 0, sizeof topt);
    topt.theta = 1.0, topt.max_picard = 50, topt.picard_tol = 1e-6, topt.pcg_tol = 1e-12, topt.pcg_max_iter = 20000;
    double emis = mat->emissivity.n ? mat_eval(&mat->emissivity, 293.15) : 0.9;
    double time = 0, worst = 0, t_mech = 0, t_therm = 0;
    bool birth_pending = false;
    int frame_index = 0;
    const int S = P->thermal_substeps;

    /* exposed faces of the printed region: convection and radiation to the chamber */
#define REBUILD_FACES()                                                                                                                    \
    do {                                                                                                                                   \
        int nf = 0;                                                                                                                        \
        for (size_t e = 0; e < ne; e++) {                                                                                                  \
            if (!active[e]) continue;                                                                                                      \
            if (M->support_band && M->support_band[e] >= 0) { nf = support_faces(M, (int)e, P, emis, faces, nf); continue; }                \
            for (int f = 0; f < 6; f++) {                                                                                                  \
                int o = nb[6 * e + (size_t)f];                                                                                             \
                if (o >= 0 && active[o]) continue;                                                                                         \
                bool on_bed = true;                                                                                                        \
                for (int i = 0; i < 4; i++) on_bed &= bed[M->conn[8 * e + (size_t)HEX8_FACE_NODES[f][i]]] != 0;                          \
                if (on_bed) continue;                                                                                                      \
                faces[nf++] = (ThermalFace){(int)e, (unsigned char)f, THERMAL_CONVECTION, P->h_conv, P->T_ambient};                      \
                if (P->radiation) faces[nf++] = (ThermalFace){(int)e, (unsigned char)f, THERMAL_RADIATION, emis, P->T_ambient};          \
            }                                                                                                                              \
        }                                                                                                                                  \
        tmod.nfaces = nf;                                                                                                                  \
    } while (0)

#define EMIT(stage_, layer_)                                                                                                               \
    do {                                                                                                                                   \
        fff_mech_von_mises(mech, vm);                                                                                                      \
        FffFrame fr = {frame_index++, layer_, nlayers, stage_, time, active, T, fff_mech_u(mech), vm, 0, 0, 0, worst};                   \
        const double *uu = fff_mech_u(mech);                                                                                               \
        for (size_t n = 0; n < nn; n++)                                                                                                    \
            if (node_on[n])                                                                                                                \
                fr.T_max = fmax(fr.T_max, T[n]),                                                                                           \
                fr.u_max = fmax(fr.u_max, sqrt(uu[3 * n] * uu[3 * n] + uu[3 * n + 1] * uu[3 * n + 1] + uu[3 * n + 2] * uu[3 * n + 2]));  \
        for (size_t e = 0; e < ne; e++) fr.vm_max = fmax(fr.vm_max, vm[e]);                                                              \
        sum->worst_energy_balance = fmax(sum->worst_energy_balance, worst);                                                               \
        worst = 0;                                                                                                                         \
        if (!strcmp(stage_, "released")) sum->peak_vm_released = fr.vm_max;                                                              \
        else sum->peak_vm_bed = fmax(sum->peak_vm_bed, fr.vm_max);                                                                        \
        if (fn && !fn(&fr, ctx)) {                                                                                                         \
            snprintf(err, errlen, "stopped by the frame callback");                                                                        \
            goto done;                                                                                                                     \
        }                                                                                                                                  \
    } while (0)

#define THERMAL_PHASE(duration_, mech_first_, stage_first_, stage_last_, layer_)                                                          \
    do {                                                                                                                                   \
        double dur_ = (duration_), dt0_ = fmin(20.0, dur_ / 50), r_ = geometric_ratio(dur_, dt0_, S);                                     \
        if (r_ == 1.0) dt0_ = dur_ / S;                                                                                                    \
        for (int i = 0; i < S; i++) {                                                                                                      \
            double dt = dt0_ * pow(r_, i);                                                                                                 \
            if (birth_pending) {                                                                                                          \
                for (size_t n = 0; n < nn; n++) node_power[n] = fixed[n] ? 0 : birth_heat[n] / dt;                                         \
                tmod.node_power = node_power;                                                                                              \
            } else tmod.node_power = NULL;                                                                                                 \
            ThermalStepStats st;                                                                                                           \
            double t0 = now_s();                                                                                                           \
            if (!thermal_step(solver, &tmod, T, T1, time, dt, &st, err, errlen)) goto done;                                                \
            t_therm += now_s() - t0;                                                                                                       \
            worst = fmax(worst, st.balance_error);                                                                                         \
            sum->bed_heat -= st.prescribed_energy;                                                                                         \
            sum->air_heat -= st.boundary_energy;                                                                                           \
            if (birth_pending) {                                                                                                          \
                for (size_t n = 0; n < nn; n++) if (fixed[n]) sum->bed_heat += birth_heat[n];                                               \
                birth_pending = false;                                                                                                    \
            }                                                                                                                             \
            double *sw = T;                                                                                                                \
            T = T1, T1 = sw;                                                                                                               \
            time += dt, sum->thermal_steps++;                                                                                              \
            if (step_fn) step_fn(time, T, active, ctx);                                                                                    \
            if ((i == 0 && (mech_first_)) || i == S - 1 || P->frame_every_substep) {                                                       \
                t0 = now_s();                                                                                                              \
                bool skipped_ = false;                                                                                                     \
                if (!fff_mech_increment_opt(mech, T, P->skip_dt, &skipped_, err, errlen)) goto done;                                        \
                sum->skipped_increments += skipped_;                                                                                       \
                t_mech += now_s() - t0;                                                                                                    \
                EMIT(i == 0 && (mech_first_) ? (stage_first_) : (i == S - 1 ? (stage_last_) : "cooling"), layer_);                       \
            }                                                                                                                              \
        }                                                                                                                                  \
    } while (0)

    for (int k = 0; k < nlayers; k++) {
        double Vk = 0;
        for (size_t e = 0; e < ne; e++) {
            if (layer[e] != k) continue;
            active[e] = 1, Vk += vol[e] * (M->support_band && M->support_band[e] >= 0 ? M->band_props[6 * (size_t)M->support_band[e] + 3] : 1.0);
            fff_mech_activate(mech, (int)e, P->T_nozzle);
            for (int a = 0; a < 8; a++) {
                int n = M->conn[8 * e + (size_t)a];
                if (!node_on[n]) node_on[n] = 1, T[n] = bed[n] ? P->T_bed : P->T_nozzle;
            }
        }
        if (Vk == 0) continue;
        memset(birth_heat, 0, nn * sizeof(double));
        double incoming = 0, initial = 0, correction = 0;
        for (size_t e = 0; e < ne; e++) {
            if (layer[e] != k) continue;
            double supplied = 0;
            initial += element_heat(M, (int)e, &tmats[emat ? emat[e] : 0], T, P->T_ambient, P->T_nozzle, birth_heat, &supplied);
            incoming += supplied;
        }
        for (size_t n = 0; n < nn; n++) correction += birth_heat[n];
        sum->deposition_heat += incoming, sum->deposition_correction += correction;
        double largest = fmax(fabs(incoming), fmax(fabs(initial), fabs(correction)));
        sum->deposition_balance = fmax(sum->deposition_balance, largest > 0 ? fabs(incoming - initial - correction) / largest : 0);
        birth_pending = true;
        sum->printed_volume += Vk;
        REBUILD_FACES();
        if (!solver && !(solver = thermal_create(&tmod, &topt, err, errlen))) goto done;
        double t_layer = fmax(P->min_layer_time, Vk / P->deposition_rate);
        THERMAL_PHASE(t_layer, true, "deposited", "layer cooled", k);
    }
    sum->print_time = time;
    sum->bed_heat_print = sum->bed_heat;
    if (P->cooldown_bed_on > 0) THERMAL_PHASE(P->cooldown_bed_on, false, "", "cool-down, bed on", nlayers - 1);
    for (size_t n = 0; n < nn; n++) fixed_T[n] = P->T_ambient;
    if (P->cooldown_bed_off > 0) THERMAL_PHASE(P->cooldown_bed_off, false, "", "cool-down, bed off", nlayers - 1);
    {
        double rr[3];
        sum->bed_reaction_total = fff_mech_bed_reaction(mech, rr);
        double t0 = now_s();
        if (!fff_mech_release(mech, T, err, errlen)) goto done;
        t_mech += now_s() - t0;
        sum->release_support_reaction = fff_mech_release_reaction(mech);
        EMIT("released", nlayers - 1);
    }
    if (nb_) { /* the supports come off after the release */
        int *rm = malloc(ne * sizeof(int)), nrm = 0;
        if (!rm) {
            snprintf(err, errlen, "out of memory removing the supports");
            goto done;
        }
        for (size_t e = 0; e < ne; e++)
            if (M->support_band[e] >= 0 && active[e]) rm[nrm++] = (int)e;
        double t0 = now_s();
        bool okr = fff_mech_remove(mech, rm, nrm, T, &sum->tearoff_max, &sum->tearoff_sum, err, errlen);
        t_mech += now_s() - t0;
        for (int i = 0; i < nrm; i++) {
            sum->removed_heat += element_heat(M, rm[i], &tmats[emat ? emat[rm[i]] : 0], T, P->T_ambient, 0, NULL, NULL);
            active[rm[i]] = 0;
        }
        sum->support_elements = nrm;
        free(rm);
        if (!okr) goto done;
        memset(node_on, 0, nn);
        for (size_t e = 0; e < ne; e++)
            if (active[e])
                for (int a = 0; a < 8; a++) node_on[M->conn[8 * e + (size_t)a]] = 1;
        EMIT("supports removed", nlayers - 1);
    }
    {
        const double *uu = fff_mech_u(mech);
        sum->warp_z_max = -INFINITY, sum->warp_z_min = INFINITY;
        for (size_t n = 0; n < nn; n++)
            if (node_on[n]) sum->warp_z_max = fmax(sum->warp_z_max, uu[3 * n + 2]), sum->warp_z_min = fmin(sum->warp_z_min, uu[3 * n + 2]);
    }
    sum->nlayers = nlayers, sum->frames = frame_index, sum->mech_solves = fff_mech_solves(mech);
    sum->total_time = time, sum->seconds_thermal = t_therm, sum->seconds_mech = t_mech;
    sum->elements = M->nelems, sum->nodes = M->nnodes;
    for (size_t e = 0; e < ne; e++)
        if (active[e]) sum->stored_heat += element_heat(M, (int)e, &tmats[emat ? emat[e] : 0], T, P->T_ambient, 0, NULL, NULL);
    {
        double largest = fmax(fabs(sum->deposition_heat), fmax(fabs(sum->stored_heat),
                         fmax(fabs(sum->air_heat), fmax(fabs(sum->bed_heat), fabs(sum->removed_heat)))));
        sum->global_heat_balance = largest > 0 ? fabs(sum->stored_heat + sum->air_heat + sum->bed_heat + sum->removed_heat -
                                                     sum->deposition_heat) / largest : 0;
    }
    ok = true;
#undef REBUILD_FACES
#undef EMIT
#undef THERMAL_PHASE
done:
    thermal_free(solver);
    fff_mech_free(mech);
    free(bed), free(active), free(node_on), free(fixed), free(layer), free(nb), free(vol);
    free(T), free(T1), free(fixed_T), free(vm), free(faces), free(tmats), free(tvals), free(emat);
    free(birth_heat), free(node_power);
    return ok;
}
