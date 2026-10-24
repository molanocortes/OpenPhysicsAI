/* lpbf.c - inherent-strain build of an LPBF part (see lpbf.h) */
#include "lpbf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../fem/dense.h"
#include "../threads.h"

struct LpbfModel {
    LpbfMesh mesh;
    double E, nu;
    Hex8Formulation form;
    SolidSolver solver;
    double pcg_tol;
    unsigned char *fixed;  /* 3 * nnodes: held components (the plate holds all three) */
    unsigned char *active; /* nelems */
    unsigned char *born;   /* nelems: activated but not yet part of a solve */
    double *u;             /* 3 * nnodes */
    double *stress;        /* 48 * nelems */
    double *release;       /* 3 * nnodes: internal forces released by removed elements, applied in the next step */
    double *reaction;      /* 3 * nnodes: plate reactions of the last step */
    ThreadPool *pool;
    int solves;
    double seconds, equilibrium_error;
    /* plasticity (section 9 of the contract); sigma_y <= 0 means the elastic path, unchanged */
    double sigma_y, hardening, newton_tol;
    int max_newton, newton_iterations;
    double last_residual;
    double *alpha;         /* 8 * nelems: equivalent plastic strain per Gauss point */
    double *scale;         /* nelems: stiffness fraction (homogenised supports); NULL means 1 */
    int nhang;             /* hanging nodes of an adaptive mesh, with the coarse element that owns each */
    int *hang_node, *hang_nmaster, *hang_master, *hang_owner;
    double *hang_weight;
    int nD;                /* anisotropic elements (homogenised supports): their own 6x6 matrices, elastic */
    int *D_of;             /* nelems: index into Dmat, -1 for the isotropic material; NULL: none */
    double *Dmat;          /* 36 * nD, Pa */
};

bool lpbf_set_elem_D(LpbfModel *m, const int *D_of, int nD, const double *D) {
    free(m->D_of), free(m->Dmat), m->D_of = NULL, m->Dmat = NULL, m->nD = 0;
    if (!D_of || nD <= 0) return true;
    m->D_of = malloc((size_t)(m->mesh.nelems ? m->mesh.nelems : 1) * sizeof(int)), m->Dmat = malloc(36 * (size_t)nD * sizeof(double));
    if (!m->D_of || !m->Dmat) return false;
    memcpy(m->D_of, D_of, (size_t)m->mesh.nelems * sizeof(int)), memcpy(m->Dmat, D, 36 * (size_t)nD * sizeof(double));
    m->nD = nD;
    return true;
}

bool lpbf_set_hanging(LpbfModel *m, int n, const int *node, const int *nmaster, const int *master, const double *weight,
                      const int *owner) {
    free(m->hang_node), free(m->hang_nmaster), free(m->hang_master), free(m->hang_owner), free(m->hang_weight);
    m->hang_node = m->hang_nmaster = m->hang_master = m->hang_owner = NULL, m->hang_weight = NULL, m->nhang = 0;
    if (n <= 0) return true;
    m->hang_node = malloc((size_t)n * sizeof(int)), m->hang_nmaster = malloc((size_t)n * sizeof(int));
    m->hang_owner = malloc((size_t)n * sizeof(int)), m->hang_master = malloc(4 * (size_t)n * sizeof(int));
    m->hang_weight = malloc(4 * (size_t)n * sizeof(double));
    if (!m->hang_node || !m->hang_nmaster || !m->hang_owner || !m->hang_master || !m->hang_weight) return false;
    memcpy(m->hang_node, node, (size_t)n * sizeof(int)), memcpy(m->hang_nmaster, nmaster, (size_t)n * sizeof(int));
    memcpy(m->hang_owner, owner, (size_t)n * sizeof(int)), memcpy(m->hang_master, master, 4 * (size_t)n * sizeof(int));
    memcpy(m->hang_weight, weight, 4 * (size_t)n * sizeof(double));
    m->nhang = n;
    return true;
}

/* what a hanging node carries belongs to its masters; repeated so that a master that hangs in turn passes it on */
static void push_hanging(const HexModel *hm, double *v) {
    for (int pass = 0; pass < 8; pass++) {
        bool any = false;
        for (int i = 0; i < hm->nhang; i++) {
            size_t h = 3 * (size_t)hm->hang_node[i];
            if (v[h] == 0 && v[h + 1] == 0 && v[h + 2] == 0) continue;
            any = true;
            for (int q = 0; q < hm->hang_nmaster[i]; q++)
                for (int k = 0; k < 3; k++) v[3 * (size_t)hm->hang_master[4 * i + q] + (size_t)k] += hm->hang_weight[4 * i + q] * v[h + (size_t)k];
            v[h] = v[h + 1] = v[h + 2] = 0;
        }
        if (!any) break;
    }
}

bool lpbf_set_scale(LpbfModel *m, const double *scale) {
    free(m->scale), m->scale = NULL;
    if (!scale) return true;
    m->scale = malloc((size_t)(m->mesh.nelems ? m->mesh.nelems : 1) * sizeof(double));
    if (!m->scale) return false;
    memcpy(m->scale, scale, (size_t)m->mesh.nelems * sizeof(double));
    return true;
}

void lpbf_set_plasticity(LpbfModel *m, double sigma_y, double hardening, int max_newton, double newton_tol) {
    m->sigma_y = sigma_y, m->hardening = hardening > 0 ? hardening : 0;
    m->max_newton = max_newton > 0 ? max_newton : 80;
    m->newton_tol = newton_tol > 0 ? newton_tol : 1e-6;
    if (sigma_y > 0 && !m->alpha) m->alpha = calloc(8 * (size_t)(m->mesh.nelems ? m->mesh.nelems : 1), sizeof(double));
}

bool lpbf_is_plastic(const LpbfModel *m) { return m->sigma_y > 0 && m->alpha; }

void lpbf_set_fixed(LpbfModel *m, const unsigned char *fixed_dof) {
    memcpy(m->fixed, fixed_dof, 3 * (size_t)m->mesh.nnodes);
}
int lpbf_newton_iterations(const LpbfModel *m) { return m->newton_iterations; }
double lpbf_last_newton_residual(const LpbfModel *m) { return m->last_residual; }

void lpbf_plastic_strain(const LpbfModel *m, double *out) {
    for (int e = 0; e < m->mesh.nelems; e++) {
        double a = 0;
        if (m->alpha && m->active[e])
            for (int g = 0; g < 8; g++) a += m->alpha[8 * (size_t)e + (size_t)g] / 8;
        out[e] = a;
    }
}

double lpbf_peak_plastic_strain(const LpbfModel *m) {
    double peak = 0;
    if (!m->alpha) return 0;
    for (int e = 0; e < m->mesh.nelems; e++)
        if (m->active[e])
            for (int g = 0; g < 8; g++) peak = fmax(peak, m->alpha[8 * (size_t)e + (size_t)g]);
    return peak;
}

/* Radial return for J2 with isotropic linear hardening. `sig` is the trial stress in and the returned stress out;
 * `a` is the equivalent plastic strain, updated. Voigt order xx yy zz xy yz zx, shear as tensor components here. */
static void radial_return(double *sig, double *a, double G, double sigma_y, double H) {
    double p = (sig[0] + sig[1] + sig[2]) / 3.0;
    double s[6] = {sig[0] - p, sig[1] - p, sig[2] - p, sig[3], sig[4], sig[5]};
    double j2 = 0.5 * (s[0] * s[0] + s[1] * s[1] + s[2] * s[2]) + s[3] * s[3] + s[4] * s[4] + s[5] * s[5];
    double q = sqrt(3.0 * j2);
    double f = q - (sigma_y + H * *a);
    if (f <= 0 || q <= 0) return;
    double dgamma = f / (3.0 * G + H);
    double scale = 1.0 - 3.0 * G * dgamma / q;
    for (int k = 0; k < 6; k++) sig[k] = s[k] * scale + (k < 3 ? p : 0);
    *a += dgamma;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

void lpbf_free(LpbfModel *m) {
    if (!m) return;
    if (m->pool) pool_destroy(m->pool);
    free(m->fixed), free(m->active), free(m->born), free(m->u), free(m->stress), free(m->release), free(m->reaction);
    free(m->alpha), free(m->scale), free(m->D_of), free(m->Dmat);
    free(m->hang_node), free(m->hang_nmaster), free(m->hang_master), free(m->hang_owner), free(m->hang_weight);
    free(m);
}

LpbfModel *lpbf_new(const LpbfMesh *mesh, double E, double nu, const unsigned char *fixed_dof, Hex8Formulation form, SolidSolver solver, double pcg_tol,
                    char *err, size_t errlen) {
    if (!(E > 0) || !(nu > -1 && nu < 0.5)) {
        snprintf(err, errlen, "the build needs a positive Young's modulus and a Poisson ratio in (-1, 0.5)");
        return NULL;
    }
    LpbfModel *m = calloc(1, sizeof *m);
    if (!m) goto oom;
    size_t nn = (size_t)mesh->nnodes, ne = (size_t)mesh->nelems;
    m->mesh = *mesh, m->E = E, m->nu = nu, m->form = form, m->solver = solver, m->pcg_tol = pcg_tol > 0 ? pcg_tol : 1e-10;
    m->fixed = malloc(3 * (nn ? nn : 1)), m->active = calloc(ne ? ne : 1, 1), m->born = calloc(ne ? ne : 1, 1);
    m->u = calloc(3 * nn, sizeof(double)), m->stress = calloc(48 * ne, sizeof(double));
    m->release = calloc(3 * nn, sizeof(double)), m->reaction = calloc(3 * nn, sizeof(double));
    if (!m->fixed || !m->active || !m->born || !m->u || !m->stress || !m->release || !m->reaction) goto oom;
    memcpy(m->fixed, fixed_dof, 3 * nn);
    int threads = cpu_perf_count();
    m->pool = pool_create(threads > 0 ? threads : 1);
    return m;
oom:
    snprintf(err, errlen, "out of memory for a build of %d elements", mesh->nelems);
    lpbf_free(m);
    return NULL;
}

void lpbf_activate(LpbfModel *m, const int *elems, int n) {
    for (int i = 0; i < n; i++) {
        int e = elems[i];
        if (e < 0 || e >= m->mesh.nelems || m->active[e]) continue;
        m->active[e] = 1, m->born[e] = 1;
    }
}

/* nodal forces of an element's Gauss-point stresses: f_a = sum_g B_a^T sigma_g detJ_g (unit weights) */
static void internal_force(const LpbfMesh *M, int e, const double *sig, double *f) {
    double X[8][3];
    for (int a = 0; a < 8; a++)
        for (int k = 0; k < 3; k++) X[a][k] = M->xyz[3 * (size_t)M->conn[8 * (size_t)e + (size_t)a] + (size_t)k];
    const double G = 1 / sqrt(3.0);
    for (int g = 0; g < 8; g++) {
        double dN[8][3], J[3][3], dNdx[8][3];
        hex8_shape(HEX8_XI[g][0] * G, HEX8_XI[g][1] * G, HEX8_XI[g][2] * G, NULL, dN);
        double detJ = hex8_jacobian(X, dN, J, dNdx);
        const double *s = sig + 6 * g; /* xx yy zz xy yz zx */
        for (int a = 0; a < 8; a++) {
            size_t nd = (size_t)M->conn[8 * (size_t)e + (size_t)a];
            f[3 * nd] += (dNdx[a][0] * s[0] + dNdx[a][1] * s[3] + dNdx[a][2] * s[5]) * detJ;
            f[3 * nd + 1] += (dNdx[a][1] * s[1] + dNdx[a][0] * s[3] + dNdx[a][2] * s[4]) * detJ;
            f[3 * nd + 2] += (dNdx[a][2] * s[2] + dNdx[a][1] * s[4] + dNdx[a][0] * s[5]) * detJ;
        }
    }
}

/* The plate carries total accumulated stress, including layers strained in earlier steps. Recovering the reaction
 * from just the most recent linear solve loses that history (and reports zero after a zero-strain equilibration). */
static void accumulated_reaction(LpbfModel *m, const HexModel *hm, const int *map, int na,
                                 const unsigned char *used, double *fint) {
    int nn = m->mesh.nnodes;
    memset(fint, 0, 3 * (size_t)nn * sizeof(double));
    for (int a = 0; a < na; a++) internal_force(&m->mesh, map[a], m->stress + 48 * (size_t)map[a], fint);
    push_hanging(hm, fint);
    memset(m->reaction, 0, 3 * (size_t)nn * sizeof(double));
    for (int n = 0; n < nn; n++)
        for (int k = 0; k < 3; k++) {
            if (!used[n]) continue;
            if (m->fixed[3 * n + k]) m->reaction[3 * n + k] = fint[3 * n + k];
        }
}

/* The stress of one element at the current step displacement, returned onto the yield surface from the state at the
 * start of the step. `u_step` is the displacement increment of this step, `eps0e` its eigenstrain (may be NULL). */
static void element_stress(const LpbfModel *m, int e, const double D[6][6], const double *u_step, const double *eps0e,
                           const double *sig_start, const double *alpha_start, double sig[8][6], double *alpha_out) {
    const LpbfMesh *M = &m->mesh;
    double X[8][3], ue[24], eps[8][6], dsig[8][6];
    for (int a = 0; a < 8; a++) {
        size_t nd = (size_t)M->conn[8 * (size_t)e + (size_t)a];
        for (int k = 0; k < 3; k++) X[a][k] = M->xyz[3 * nd + (size_t)k], ue[3 * a + k] = u_step[3 * nd + (size_t)k];
    }
    if (m->D_of && m->D_of[e] >= 0) { /* a homogenised anisotropic support: elastic with its own matrix */
        double Da[6][6];
        for (int a = 0; a < 36; a++) Da[a / 6][a % 6] = m->Dmat[36 * (size_t)m->D_of[e] + (size_t)a];
        hex8_gauss_strain_stress(X, Da, m->form, ue, eps0e, eps, dsig);
        for (int g = 0; g < 8; g++) {
            for (int k = 0; k < 6; k++) sig[g][k] = sig_start[6 * g + k] + dsig[g][k];
            alpha_out[g] = alpha_start[g];
        }
        return;
    }
    hex8_gauss_strain_stress(X, D, m->form, ue, eps0e, eps, dsig);
    double sc = m->scale ? m->scale[e] : 1.0; /* a homogenised element is softer and yields at a lower stress */
    double G = sc * m->E / (2.0 * (1.0 + m->nu));
    for (int g = 0; g < 8; g++) {
        for (int k = 0; k < 6; k++) sig[g][k] = sig_start[6 * g + k] + sc * dsig[g][k];
        alpha_out[g] = alpha_start[g];
        radial_return(sig[g], &alpha_out[g], G, sc * m->sigma_y, sc * m->hardening);
    }
}

/* One step with plasticity: modified Newton on the elastic operator. The residual is the released force minus the
 * internal force the returned stresses carry; iteration zero is exactly the elastic predictor, so a step that never
 * yields converges immediately and gives the elastic answer. Contract section 9. */
static bool plastic_step(LpbfModel *m, const HexModel *hm, const SolidLoads *L, const SolidOptions *opt,
                         const int *map, int na, const unsigned char *used, const double *force, const double *eps0,
                         char *err, size_t errlen) {
    const LpbfMesh *M = &m->mesh;
    int nn = M->nnodes;
    bool ok = false;
    double D[6][6];
    isotropic_D(m->E, m->nu, D);
    double *u_step = calloc(3 * (size_t)nn, sizeof(double));
    double *R = calloc(3 * (size_t)nn, sizeof(double));
    double *fint = calloc(3 * (size_t)nn, sizeof(double));
    double *sig_start = malloc(48 * (size_t)na * sizeof(double));
    double *alpha_start = malloc(8 * (size_t)na * sizeof(double));
    double *sig_new = malloc(48 * (size_t)na * sizeof(double));
    double *alpha_new = malloc(8 * (size_t)na * sizeof(double));
    if (!u_step || !R || !fint || !sig_start || !alpha_start || !sig_new || !alpha_new) {
        snprintf(err, errlen, "out of memory for a plastic step");
        goto done;
    }
    for (int a = 0; a < na; a++) {
        memcpy(sig_start + 48 * (size_t)a, m->stress + 48 * (size_t)map[a], 48 * sizeof(double));
        memcpy(alpha_start + 8 * (size_t)a, m->alpha + 8 * (size_t)map[a], 8 * sizeof(double));
    }
    double r0 = 0;
    int it = 0;
    for (; it <= m->max_newton; it++) {
        memset(fint, 0, 3 * (size_t)nn * sizeof(double));
        for (int a = 0; a < na; a++) {
            double sig[8][6];
            element_stress(m, map[a], D, u_step, eps0 ? eps0 + 6 * (size_t)a : NULL, sig_start + 48 * (size_t)a,
                           alpha_start + 8 * (size_t)a, sig, alpha_new + 8 * (size_t)a);
            double d[48];
            for (int g = 0; g < 8; g++)
                for (int k = 0; k < 6; k++) {
                    sig_new[48 * (size_t)a + 6 * (size_t)g + (size_t)k] = sig[g][k];
                    d[6 * g + k] = sig[g][k] - sig_start[48 * (size_t)a + 6 * (size_t)g + (size_t)k];
                }
            internal_force(M, map[a], d, fint);
        }
        double norm = 0;
        for (int n = 0; n < nn; n++)
            for (int k = 0; k < 3; k++) R[3 * n + k] = used[n] ? force[3 * n + k] - fint[3 * n + k] : 0;
        push_hanging(hm, R); /* the residual of the reduced system: a hanging node's share belongs to its masters */
        for (int n = 0; n < nn; n++)
            for (int k = 0; k < 3; k++) {
                if (!used[n] || m->fixed[3 * n + k]) {
                    R[3 * n + k] = 0;
                    continue;
                }
                norm += R[3 * n + k] * R[3 * n + k];
            }
        norm = sqrt(norm);
        if (it == 0) r0 = norm;
        m->last_residual = norm;
        if (it > 0 && (norm <= m->newton_tol * fmax(r0, 1e-300) || norm <= 1e-9)) break;
        if (it == m->max_newton) {
            snprintf(err, errlen, "the plastic step did not converge: residual %.3e after %d iterations, against %.3e "
                                  "at the start of the step (tolerance %.1e relative)", norm, it, r0, m->newton_tol);
            goto done;
        }
        SolidResult res;
        memset(&res, 0, sizeof res);
        SolidLoads step_loads = {L->fixed, NULL, R, NULL, {0, 0, 0}};
        double t0 = now_s();
        if (!solid_solve(hm, &step_loads, opt, &res, err, errlen)) {
            solid_result_free(&res);
            goto done;
        }
        m->seconds += now_s() - t0;
        m->solves++, m->newton_iterations++;
        for (int n = 0; n < nn; n++)
            if (used[n])
                for (int k = 0; k < 3; k++) u_step[3 * n + k] += res.u[3 * n + k];
        solid_result_free(&res);
    }
    /* The accepted nonlinear residual needs a force scale that includes the step's eigenstrain predictor, even
     * when free contraction leaves almost no final stress. Reduce applied and increment internal forces with the
     * same active hanging constraints as the residual. This is reporting only, after Newton has converged. */
    memcpy(R, force, 3 * (size_t)nn * sizeof(double));
    push_hanging(hm, R);
    push_hanging(hm, fint);
    double applied2 = 0, reaction2 = 0;
    for (int n = 0; n < nn; n++)
        for (int k = 0; k < 3; k++) {
            if (!used[n]) continue;
            double f = R[3 * n + k];
            applied2 += f * f;
            if (L->fixed[3 * n + k]) {
                double r = fint[3 * n + k] - f;
                reaction2 += r * r;
            }
        }
    double force_scale = sqrt(applied2) + sqrt(reaction2) + r0;
    double equilibrium = force_scale > 0 ? m->last_residual / force_scale : m->last_residual;
    /* accept the step */
    for (int n = 0; n < nn; n++)
        if (used[n])
            for (int k = 0; k < 3; k++) m->u[3 * n + k] += u_step[3 * n + k];
    for (int a = 0; a < na; a++) {
        memcpy(m->stress + 48 * (size_t)map[a], sig_new + 48 * (size_t)a, 48 * sizeof(double));
        memcpy(m->alpha + 8 * (size_t)map[a], alpha_new + 8 * (size_t)a, 8 * sizeof(double));
    }
    /* Plate reactions carry total accumulated stress; the last solve diagnostic instead describes the nonlinear
     * step just accepted, not total-stress roundoff and not the last inner linear correction. */
    accumulated_reaction(m, hm, map, na, used, fint);
    m->equilibrium_error = equilibrium;
    ok = true;
done:
    free(u_step), free(R), free(fint), free(sig_start), free(alpha_start), free(sig_new), free(alpha_new);
    return ok;
}

/* one equilibrium step over the active elements; strained[] (nelems) marks the elements that receive eps this step */
static bool step(LpbfModel *m, const unsigned char *strained, const double eps[3], char *err, size_t errlen) {
    const LpbfMesh *M = &m->mesh;
    int ne = M->nelems, nn = M->nnodes, na = 0;
    for (int e = 0; e < ne; e++) na += m->active[e] != 0;
    if (!na) return true;
    bool ok = false;
    int nah = 0, *ahn = NULL, *ahm = NULL, *ahma = NULL; /* the hanging nodes of this step */
    int *emat = NULL;          /* anisotropic support elements: their materials */
    SolidMaterial *mats = NULL;
    double *matD = NULL;
    double *ahw = NULL;
    unsigned char *born_before_step = m->nhang ? malloc((size_t)(ne ? ne : 1)) : NULL;
    if (born_before_step) memcpy(born_before_step, m->born, (size_t)ne);
    int *conn = malloc(8 * (size_t)na * sizeof(int)), *map = malloc((size_t)na * sizeof(int));
    double *escale = m->scale ? malloc((size_t)na * sizeof(double)) : NULL;
    double *eps0 = calloc(6 * (size_t)na, sizeof(double)), *force = calloc(3 * (size_t)nn, sizeof(double));
    unsigned char *fixed = calloc(3 * (size_t)nn, 1), *used = calloc((size_t)nn, 1);
    SolidResult res;
    memset(&res, 0, sizeof res);
    if (!conn || !map || !eps0 || !force || !fixed || !used || (m->scale && !escale)) {
        snprintf(err, errlen, "out of memory for a build step");
        goto done;
    }
    for (int e = 0, a = 0; e < ne; e++) {
        if (!m->active[e]) continue;
        map[a] = e;
        if (escale) escale[a] = m->scale[e];
        memcpy(conn + 8 * (size_t)a, M->conn + 8 * (size_t)e, 8 * sizeof(int));
        if (strained && strained[e]) {
            eps0[6 * (size_t)a] = eps[0], eps0[6 * (size_t)a + 1] = eps[1], eps0[6 * (size_t)a + 2] = eps[2];
        }
        m->born[e] = 0;
        for (int k = 0; k < 8; k++) used[M->conn[8 * (size_t)e + k]] = 1;
        a++;
    }
    memcpy(force, m->release, 3 * (size_t)nn * sizeof(double));
    memset(m->release, 0, 3 * (size_t)nn * sizeof(double));
    /* A released force can sit on a hanging node that no active element uses any more: the underside of a coarse part
     * element where fine supports were removed. The node carries no equation, so the force goes to its masters with the
     * constraint's weights (repeated, for masters that hang in turn), instead of being lost. */
    for (int pass = 0; pass < 8 && m->nhang; pass++) {
        bool moved = false;
        for (int i = 0; i < m->nhang; i++) {
            int nd = m->hang_node[i];
            if (used[nd]) continue;
            double *f = force + 3 * (size_t)nd;
            if (f[0] == 0 && f[1] == 0 && f[2] == 0) continue;
            for (int q = 0; q < m->hang_nmaster[i]; q++) {
                int ms = m->hang_master[4 * i + q];
                double w = m->hang_weight[4 * i + q];
                for (int k = 0; k < 3; k++) force[3 * (size_t)ms + (size_t)k] += w * f[k];
            }
            f[0] = f[1] = f[2] = 0, moved = true;
        }
        if (!moved) break;
    }
    for (int n = 0; n < nn; n++)
        if (used[n])
            for (int k = 0; k < 3; k++) fixed[3 * n + k] = m->fixed[3 * n + k];
    /* the constraints of this step: those whose coarse owner is active and whose node an active element uses */
    if (m->nhang) {
        /* a node used for the first time in this step that hangs on a coarse face lies on that face as it is now: it
         * starts at its masters' interpolation, as it would have moved with the layer below on a uniform mesh */
        unsigned char *old_node = calloc((size_t)nn, 1);
        if (old_node) {
            for (int e = 0; e < ne; e++)
                if (m->active[e] && !born_before_step[e])
                    for (int k = 0; k < 8; k++) old_node[M->conn[8 * (size_t)e + k]] = 1;
            for (int pass = 0; pass < 2; pass++)
                for (int i = 0; i < m->nhang; i++) {
                    int nd = m->hang_node[i];
                    if (!m->active[m->hang_owner[i]] || !used[nd] || old_node[nd] || !born_before_step) continue;
                    for (int k = 0; k < 3; k++) {
                        double v = 0;
                        for (int q = 0; q < m->hang_nmaster[i]; q++) v += m->hang_weight[4 * i + q] * m->u[3 * (size_t)m->hang_master[4 * i + q] + (size_t)k];
                        m->u[3 * (size_t)nd + (size_t)k] = v;
                    }
                }
            free(old_node);
        }
        ahn = malloc((size_t)m->nhang * sizeof(int)), ahm = malloc((size_t)m->nhang * sizeof(int));
        ahma = malloc(4 * (size_t)m->nhang * sizeof(int)), ahw = malloc(4 * (size_t)m->nhang * sizeof(double));
        unsigned char *taken = calloc((size_t)nn, 1);
        if (!ahn || !ahm || !ahma || !ahw || !taken) {
            free(taken);
            snprintf(err, errlen, "out of memory for the hanging nodes of a step");
            goto done;
        }
        for (int i = 0; i < m->nhang; i++) {
            int nd = m->hang_node[i];
            if (!m->active[m->hang_owner[i]] || !used[nd] || taken[nd]) continue;
            taken[nd] = 1;
            ahn[nah] = nd, ahm[nah] = m->hang_nmaster[i];
            memcpy(ahma + 4 * nah, m->hang_master + 4 * i, 4 * sizeof(int));
            memcpy(ahw + 4 * nah, m->hang_weight + 4 * i, 4 * sizeof(double));
            nah++;
        }
        free(taken);
    }
    SolidMaterial smat = {m->E, m->nu, 1.0};
    HexModel hm = {nn, na, M->xyz, conn, NULL, 1, &smat, m->form, escale, nah, ahn, ahm, ahma, ahw, NULL};
    /* anisotropic elements: material 1 + their matrix index, a full matrix each; material 0 stays isotropic */
    if (m->D_of) {
        emat = malloc((size_t)na * sizeof(int)), mats = malloc((size_t)(m->nD + 1) * sizeof *mats);
        matD = calloc(36 * (size_t)(m->nD + 1), sizeof(double));
        if (!emat || !mats || !matD) {
            snprintf(err, errlen, "out of memory for the support materials");
            goto done;
        }
        for (int i = 0; i <= m->nD; i++) mats[i] = smat;
        memcpy(matD + 36, m->Dmat, 36 * (size_t)m->nD * sizeof(double));
        for (int a = 0; a < na; a++) {
            int d = m->D_of[map[a]];
            emat[a] = d >= 0 ? d + 1 : 0;
            if (d >= 0 && escale) escale[a] = 1.0; /* the matrix is already the support's */
        }
        hm.elem_mat = emat, hm.nmat = m->nD + 1, hm.mat = mats, hm.mat_D = matD;
    }
    SolidLoads L = {fixed, NULL, force, eps0, {0, 0, 0}};
    SolidOptions opt = {m->solver, m->pcg_tol, 20000, 0, m->pool, NULL, NULL};
    if (lpbf_is_plastic(m)) {
        ok = plastic_step(m, &hm, &L, &opt, map, na, used, force, eps0, err, errlen);
        goto done;
    }
    double t0 = now_s();
    if (!solid_solve(&hm, &L, &opt, &res, err, errlen)) goto done;
    m->seconds += now_s() - t0;
    m->solves++;
    for (int n = 0; n < nn; n++)
        if (used[n])
            for (int k = 0; k < 3; k++) m->u[3 * n + k] += res.u[3 * n + k];
    for (int a = 0; a < na; a++)
        for (int k = 0; k < 48; k++) m->stress[48 * (size_t)map[a] + (size_t)k] += res.gp_stress[48 * (size_t)a + (size_t)k];
    accumulated_reaction(m, &hm, map, na, used, force);
    /* Last elastic solve residual uses its eigenstrain right-hand side as well as reactions. Normalising by only
     * recovered stress would turn harmless roundoff in a freely contracted, stress-free part into an O(1) error. */
    m->equilibrium_error = res.equilibrium_error;
    ok = true;
done:
    solid_result_free(&res);
    free(conn), free(map), free(eps0), free(force), free(fixed), free(used), free(escale);
    free(ahn), free(ahm), free(ahma), free(ahw), free(born_before_step);
    free(emat), free(mats), free(matD);
    return ok;
}

bool lpbf_strain(LpbfModel *m, const int *elems, int n, const double eps[3], char *err, size_t errlen) {
    unsigned char *strained = calloc((size_t)(m->mesh.nelems ? m->mesh.nelems : 1), 1);
    if (!strained) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    for (int i = 0; i < n; i++)
        if (elems[i] >= 0 && elems[i] < m->mesh.nelems && m->active[elems[i]]) strained[elems[i]] = 1;
    bool ok = step(m, strained, eps, err, errlen);
    free(strained);
    return ok;
}

bool lpbf_remove(LpbfModel *m, const int *elems, int n, char *err, size_t errlen) {
    int removed = 0;
    for (int i = 0; i < n; i++) {
        int e = elems[i];
        if (e < 0 || e >= m->mesh.nelems || !m->active[e]) continue;
        internal_force(&m->mesh, e, m->stress + 48 * (size_t)e, m->release); /* what it carried is released onto the rest */
        memset(m->stress + 48 * (size_t)e, 0, 48 * sizeof(double));
        m->active[e] = 0;
        removed++;
    }
    if (!removed) return true;
    double zero[3] = {0, 0, 0};
    return step(m, NULL, zero, err, errlen);
}

void lpbf_element_forces(const LpbfModel *m, const int *elems, int n, double *f) {
    for (int i = 0; i < n; i++)
        if (m->active[elems[i]]) internal_force(&m->mesh, elems[i], m->stress + 48 * (size_t)elems[i], f);
}

const double *lpbf_u(const LpbfModel *m) { return m->u; }
const double *lpbf_stress(const LpbfModel *m) { return m->stress; }
const unsigned char *lpbf_active(const LpbfModel *m) { return m->active; }
int lpbf_solves(const LpbfModel *m) { return m->solves; }
double lpbf_seconds(const LpbfModel *m) { return m->seconds; }
double lpbf_equilibrium_error(const LpbfModel *m) { return m->equilibrium_error; }

void lpbf_von_mises(const LpbfModel *m, double *vm) {
    for (int e = 0; e < m->mesh.nelems; e++) {
        vm[e] = 0;
        if (!m->active[e]) continue;
        for (int g = 0; g < 8; g++) vm[e] += von_mises(m->stress + 48 * (size_t)e + 6 * (size_t)g) / 8;
    }
}

double lpbf_plate_reaction(const LpbfModel *m, double r[3]) {
    r[0] = r[1] = r[2] = 0;
    double largest = 0;
    for (int n = 0; n < m->mesh.nnodes; n++)
        for (int k = 0; k < 3; k++) {
            r[k] += m->reaction[3 * n + k];
            largest = fmax(largest, fabs(m->reaction[3 * n + k]));
        }
    return largest;
}
