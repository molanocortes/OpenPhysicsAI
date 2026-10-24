/* impact.c - see impact.h. Layout: element kinematics, material (Johnson-Cook, Mie-Gruneisen), element loop, contact,
 * the step, output, meshes. */
#include "impact.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../threads.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* reference-element signs of the eight nodes */
static const double XI[8] = {-1, 1, 1, -1, -1, 1, 1, -1}, ET[8] = {-1, -1, 1, 1, -1, -1, 1, 1}, ZE[8] = {-1, -1, -1, -1, 1, 1, 1, 1};
/* outward faces, for the surface */
static const int HF[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};

typedef struct Face {
    int n[4];
    int body, elem;
} Face;

struct Impact {
    ImSpec spec;
    int nn, ne;
    double *x, *v, *a, *m, *f; /* nodes */
    int *node_body;
    double *V0, *V;            /* element volumes */
    double *sig;               /* 6 per element: deviatoric stress s (xx, yy, zz, xy, yz, zx) */
    double *E;                 /* internal energy per unit reference volume, J/m^3 */
    double *ep, *T, *p, *q, *vm, *rate;
    unsigned char *eroded;
    int neroded;
    double *fe;                /* 24 per element: element nodal forces this step */
    double *ehg;               /* per element: energy dissipated by the hourglass viscosity, J */
    double *dt_el;             /* per element: its stable step */
    int *adj_start, *adj;      /* node -> (element * 8 + local) */
    double t, dt;
    long steps;
    double w_hourglass, w_contact, w_anvil, e_eroded;
    Face *faces;
    int nfaces;
    bool faces_dirty;
    double *fc;                /* contact forces, 3 per node */
    double *vz_prev;           /* each node's z velocity before this step's update, for the anvil's account */
    unsigned char *touching;   /* the node was in contact at the last step */
    ThreadPool *pool;
};

void im_spec_defaults(ImSpec *s) {
    memset(s, 0, sizeof *s);
    s->safety = 0.6;
    s->hourglass = 0.1;
    s->q_linear = 0.06, s->q_quadratic = 1.5;
    s->contact_scale = 0.1;
}

/* ---- element kinematics ------------------------------------------------------------------------------------------- */

/* the volume and its gradient b (b[I][i] = dV / dx_iI), exactly: det J of the trilinear map is at most quadratic in each
 * reference coordinate, so the 2 x 2 x 2 Gauss rule integrates it and its derivatives exactly, and
 * d det J / d x_iI = det J dN_I / dx_i */
static double hex_volume_gradient(const double X[8][3], double b[8][3]) {
    static const double g = 0.57735026918962576;
    double V = 0;
    for (int I = 0; I < 8; I++) b[I][0] = b[I][1] = b[I][2] = 0;
    for (int gp = 0; gp < 8; gp++) {
        double xi = XI[gp] * g, et = ET[gp] * g, ze = ZE[gp] * g;
        double dN[8][3], J[3][3] = {{0}};
        for (int I = 0; I < 8; I++) {
            dN[I][0] = 0.125 * XI[I] * (1 + ET[I] * et) * (1 + ZE[I] * ze);
            dN[I][1] = 0.125 * ET[I] * (1 + XI[I] * xi) * (1 + ZE[I] * ze);
            dN[I][2] = 0.125 * ZE[I] * (1 + XI[I] * xi) * (1 + ET[I] * et);
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++) J[r][c] += dN[I][r] * X[I][c];
        }
        double det = J[0][0] * (J[1][1] * J[2][2] - J[1][2] * J[2][1]) - J[0][1] * (J[1][0] * J[2][2] - J[1][2] * J[2][0]) +
                     J[0][2] * (J[1][0] * J[2][1] - J[1][1] * J[2][0]);
        double inv[3][3] = {{(J[1][1] * J[2][2] - J[1][2] * J[2][1]), -(J[0][1] * J[2][2] - J[0][2] * J[2][1]), (J[0][1] * J[1][2] - J[0][2] * J[1][1])},
                            {-(J[1][0] * J[2][2] - J[1][2] * J[2][0]), (J[0][0] * J[2][2] - J[0][2] * J[2][0]), -(J[0][0] * J[1][2] - J[0][2] * J[1][0])},
                            {(J[1][0] * J[2][1] - J[1][1] * J[2][0]), -(J[0][0] * J[2][1] - J[0][1] * J[2][0]), (J[0][0] * J[1][1] - J[0][1] * J[1][0])}};
        /* inv is adj(J): dN/dx = adj(J) dN/dxi / det, and det J dN/dx = adj(J) dN/dxi */
        for (int I = 0; I < 8; I++)
            for (int i = 0; i < 3; i++) b[I][i] += inv[i][0] * dN[I][0] + inv[i][1] * dN[I][1] + inv[i][2] * dN[I][2];
        V += det;
    }
    return V;
}

/* ---- material ------------------------------------------------------------------------------------------------------ */

double im_jc_flow(const ImMaterial *M, double ep, double rate, double T) {
    double th = (T - M->T_room) / (M->T_melt - M->T_room);
    if (th >= 1) return 0;
    double soft = th > 0 ? 1 - pow(th, M->m) : 1;
    double r = rate / (M->rate0 > 0 ? M->rate0 : 1);
    double rf = r > 1 ? 1 + M->C * log(r) : 1;
    return (M->A + M->B * (ep > 0 ? pow(ep, M->n) : 0)) * rf * soft;
}

/* Mie-Gruneisen, Hugoniot reference, for mu = rho / rho0 - 1 and internal energy per reference volume E */
double im_mg_pressure(const ImMaterial *M, double mu, double E) {
    double k = M->rho0 * M->c0 * M->c0;
    if (mu > 0) {
        double den = 1 - (M->s - 1) * mu;
        return k * mu * (1 + (1 - 0.5 * M->gamma0) * mu) / (den * den) + M->gamma0 * E;
    }
    return k * mu + M->gamma0 * E;
}

static int elem_material(const Impact *im, int e);

/* ---- setup --------------------------------------------------------------------------------------------------------- */

Impact *im_create(const ImSpec *s, char *err, size_t errlen) {
    if (s->mesh.nnodes <= 0 || s->mesh.nelems <= 0 || s->nbodies < 1) {
        snprintf(err, errlen, "impact: a mesh and at least one body are required");
        return NULL;
    }
    Impact *im = calloc(1, sizeof *im);
    im->spec = *s;
    if (!(im->spec.safety > 0)) im->spec.safety = 0.6;
    int nn = im->nn = s->mesh.nnodes, ne = im->ne = s->mesh.nelems;
    im->x = malloc((size_t)nn * 3 * sizeof(double));
    im->v = calloc((size_t)nn * 3, sizeof(double));
    im->a = calloc((size_t)nn * 3, sizeof(double));
    im->f = calloc((size_t)nn * 3, sizeof(double));
    im->fc = calloc((size_t)nn * 3, sizeof(double));
    im->vz_prev = calloc((size_t)nn, sizeof(double));
    im->touching = calloc((size_t)nn, 1);
    im->m = calloc((size_t)nn, sizeof(double));
    im->node_body = calloc((size_t)nn, sizeof(int));
    im->V0 = malloc((size_t)ne * sizeof(double));
    im->V = malloc((size_t)ne * sizeof(double));
    im->sig = calloc((size_t)ne * 6, sizeof(double));
    im->E = calloc((size_t)ne, sizeof(double));
    im->ep = calloc((size_t)ne, sizeof(double));
    im->T = calloc((size_t)ne, sizeof(double));
    im->p = calloc((size_t)ne, sizeof(double));
    im->q = calloc((size_t)ne, sizeof(double));
    im->vm = calloc((size_t)ne, sizeof(double));
    im->rate = calloc((size_t)ne, sizeof(double));
    im->eroded = calloc((size_t)ne, 1);
    im->fe = calloc((size_t)ne * 24, sizeof(double));
    im->ehg = calloc((size_t)ne, sizeof(double));
    im->dt_el = calloc((size_t)ne, sizeof(double));
    if (!im->x || !im->v || !im->m || !im->V0 || !im->fe || !im->dt_el || !im->eroded) {
        snprintf(err, errlen, "impact: out of memory");
        im_free(im);
        return NULL;
    }
    memcpy(im->x, s->mesh.xyz, (size_t)nn * 3 * sizeof(double));
    /* bodies: materials of the elements, initial velocities of the nodes */
    int *emat = malloc((size_t)ne * sizeof(int));
    for (int e = 0; e < ne; e++) emat[e] = -1;
    for (int b = 0; b < s->nbodies; b++) {
        const ImBody *B = &s->bodies[b];
        for (int e = B->first_elem; e < B->first_elem + B->nelems; e++) emat[e] = B->material;
        for (int n = B->first_node; n < B->first_node + B->nnodes; n++) {
            im->node_body[n] = b;
            for (int k = 0; k < 3; k++) im->v[3 * n + k] = B->velocity[k];
        }
    }
    for (int e = 0; e < ne; e++) {
        if (emat[e] < 0 || emat[e] >= s->nmaterials) {
            snprintf(err, errlen, "impact: element %d belongs to no body", e);
            free(emat);
            im_free(im);
            return NULL;
        }
        double X[8][3], b[8][3];
        for (int I = 0; I < 8; I++)
            for (int k = 0; k < 3; k++) X[I][k] = im->x[3 * s->mesh.conn[8 * e + I] + k];
        double V = hex_volume_gradient(X, b);
        if (!(V > 0)) {
            snprintf(err, errlen, "impact: element %d has volume %g (the node order must be right-handed)", e, V);
            free(emat);
            im_free(im);
            return NULL;
        }
        im->V0[e] = im->V[e] = V;
        const ImMaterial *M = &s->materials[emat[e]];
        im->T[e] = M->T_room;
        for (int I = 0; I < 8; I++) im->m[s->mesh.conn[8 * e + I]] += 0.125 * M->rho0 * V;
    }
    /* keep the element materials in the spec's copy of the mesh: reuse the body table at run time */
    free(emat);
    /* node -> element adjacency, for a gather of the element forces */
    im->adj_start = calloc((size_t)nn + 1, sizeof(int));
    im->adj = malloc((size_t)ne * 8 * sizeof(int));
    for (int e = 0; e < ne; e++)
        for (int I = 0; I < 8; I++) im->adj_start[s->mesh.conn[8 * e + I] + 1]++;
    for (int n = 0; n < nn; n++) im->adj_start[n + 1] += im->adj_start[n];
    int *fill = calloc((size_t)nn, sizeof(int));
    for (int e = 0; e < ne; e++)
        for (int I = 0; I < 8; I++) {
            int n = s->mesh.conn[8 * e + I];
            im->adj[im->adj_start[n] + fill[n]++] = 8 * e + I;
        }
    free(fill);
    /* the spec keeps pointers into the caller's mesh: copy the mesh */
    im->spec.mesh.xyz = malloc((size_t)nn * 3 * sizeof(double));
    im->spec.mesh.conn = malloc((size_t)ne * 8 * sizeof(int));
    memcpy(im->spec.mesh.xyz, s->mesh.xyz, (size_t)nn * 3 * sizeof(double));
    memcpy(im->spec.mesh.conn, s->mesh.conn, (size_t)ne * 8 * sizeof(int));
    im->faces_dirty = true;
    /* a first step before any stress exists, from the elastic wave speed; the element loop sets the real one */
    im->dt = INFINITY;
    for (int e = 0; e < ne; e++) {
        const ImMaterial *M = &s->materials[elem_material(im, e)];
        double c = sqrt((M->rho0 * M->c0 * M->c0 + 4.0 / 3.0 * M->shear_modulus) / M->rho0);
        im->dt = fmin(im->dt, im->spec.safety * 0.5 * cbrt(im->V0[e]) / c);
    }
    int nt = s->threads > 0 ? s->threads : cpu_perf_count();
    im->pool = pool_create(nt < 1 ? 1 : nt);
    return im;
}

void im_free(Impact *im) {
    if (!im) return;
    free(im->x), free(im->v), free(im->a), free(im->f), free(im->fc), free(im->vz_prev), free(im->touching), free(im->m), free(im->node_body);
    free(im->V0), free(im->V), free(im->sig), free(im->E), free(im->ep), free(im->T), free(im->p), free(im->q), free(im->vm), free(im->rate);
    free(im->eroded), free(im->fe), free(im->ehg), free(im->dt_el), free(im->adj_start), free(im->adj), free(im->faces);
    free(im->spec.mesh.xyz), free(im->spec.mesh.conn);
    if (im->pool) pool_destroy(im->pool);
    free(im);
}

static int elem_material(const Impact *im, int e) {
    for (int b = 0; b < im->spec.nbodies; b++) {
        const ImBody *B = &im->spec.bodies[b];
        if (e >= B->first_elem && e < B->first_elem + B->nelems) return B->material;
    }
    return 0;
}

/* ---- the element loop ------------------------------------------------------------------------------------------------ */

static void element_fn(void *ctx, int begin, int end, int tid) {
    Impact *im = ctx;
    const double dt = im->dt;
    static const double HG[4][8] = {{1, -1, 1, -1, 1, -1, 1, -1}, {1, 1, -1, -1, -1, -1, 1, 1}, {1, -1, -1, 1, -1, 1, 1, -1}, {-1, 1, -1, 1, 1, -1, 1, -1}};
    for (int e = begin; e < end; e++) {
        double *fe = im->fe + 24 * (size_t)e;
        if (im->eroded[e]) {
            memset(fe, 0, 24 * sizeof(double));
            im->dt_el[e] = INFINITY;
            continue;
        }
        const ImMaterial *M = &im->spec.materials[elem_material(im, e)];
        const int *cn = im->spec.mesh.conn + 8 * (size_t)e;
        double X[8][3], U[8][3], b[8][3];
        for (int I = 0; I < 8; I++)
            for (int k = 0; k < 3; k++) X[I][k] = im->x[3 * cn[I] + k], U[I][k] = im->v[3 * cn[I] + k];
        double V = hex_volume_gradient(X, b);
        if (!(V > 0)) { /* inverted: erode rather than propagate a negative volume */
            im->eroded[e] = 1;
            memset(fe, 0, 24 * sizeof(double));
            im->dt_el[e] = INFINITY;
            continue;
        }
        /* velocity gradient L_ij = sum v_iI b_jI / V */
        double L[3][3] = {{0}};
        for (int I = 0; I < 8; I++)
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) L[i][j] += U[I][i] * b[I][j];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) L[i][j] /= V;
        double D[3][3], W[3][3];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) D[i][j] = 0.5 * (L[i][j] + L[j][i]), W[i][j] = 0.5 * (L[i][j] - L[j][i]);
        double trD = D[0][0] + D[1][1] + D[2][2];
        double rho = M->rho0 * im->V0[e] / V;
        double Vold = im->V[e];
        im->V[e] = V;
        /* deviatoric stress: Jaumann rate, elastic trial */
        double *sv = im->sig + 6 * (size_t)e;
        double s[3][3] = {{sv[0], sv[3], sv[5]}, {sv[3], sv[1], sv[4]}, {sv[5], sv[4], sv[2]}};
        double G = M->shear_modulus;
        double sn[3][3];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) {
                double ws = 0, sw = 0;
                for (int k = 0; k < 3; k++) ws += W[i][k] * s[k][j], sw += s[i][k] * W[k][j];
                sn[i][j] = s[i][j] + dt * (2 * G * (D[i][j] - (i == j ? trD / 3 : 0)) + ws - sw);
            }
        double J2 = 0;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) J2 += sn[i][j] * sn[i][j];
        double seq = sqrt(1.5 * J2);
        /* Johnson-Cook return: sigma_eq_trial - 3 G dep = sigma_y(ep + dep, dep / dt, T) */
        double dep = 0;
        double sy0 = im_jc_flow(M, im->ep[e], 0, im->T[e]);
        if (seq > sy0) {
            double lo = 0, hi = seq / (3 * G);
            for (int it = 0; it < 60; it++) { /* bisection: the residual decreases in dep */
                double mid = 0.5 * (lo + hi);
                double r = seq - 3 * G * mid - im_jc_flow(M, im->ep[e] + mid, mid / dt, im->T[e]);
                if (r > 0) lo = mid;
                else hi = mid;
                if (hi - lo < 1e-14 * (1 + hi)) break;
            }
            dep = 0.5 * (lo + hi);
            double scale = seq > 0 ? 1 - 3 * G * dep / seq : 1;
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) sn[i][j] *= scale;
            double sy = seq - 3 * G * dep;
            im->ep[e] += dep;
            im->T[e] += M->chi * sy * dep / (rho * M->cp);
        }
        im->rate[e] = dep / dt;
        /* the element's smallest dimension (volume over its largest face): the length the viscosity and the stable
         * step both use; the cube root of the volume overstates it for a flat element and made the viscosity unstable
         * (found on the Hugoniot test, 2026-09-25) */
        double amax = 0;
        for (int f = 0; f < 6; f++) {
            const double *p0 = X[HF[f][0]], *p1 = X[HF[f][1]], *p2 = X[HF[f][2]], *p3 = X[HF[f][3]];
            double d1[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]}, d2[3] = {p3[0] - p1[0], p3[1] - p1[1], p3[2] - p1[2]};
            double cr[3] = {d1[1] * d2[2] - d1[2] * d2[1], d1[2] * d2[0] - d1[0] * d2[2], d1[0] * d2[1] - d1[1] * d2[0]};
            amax = fmax(amax, 0.5 * sqrt(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]));
        }
        double lc = V / amax;
        /* bulk viscosity */
        double c = sqrt(fmax((M->rho0 * M->c0 * M->c0 + 4.0 / 3.0 * G) / rho, 1e-12));
        double q = 0;
        if (trD < 0) q = rho * lc * (im->spec.q_quadratic * lc * trD * trD - im->spec.q_linear * c * trD);
        /* energy per reference volume, the pressure work centred in time: with dv = dV / V0 of this step,
         *   E+ = E + dt Vm / V0 s:D - (p / 2 + q) dv - p+ / 2 dv,   p+ = f(mu+) + gamma0 E+,
         * solved for E+ because the Mie-Gruneisen pressure is linear in E (the lagged pressure alone left the Taylor
         * test's energy 3 per cent short) */
        double sd = 0;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) sd += 0.5 * (s[i][j] + sn[i][j]) * D[i][j];
        double Vm = 0.5 * (V + Vold);
        double dv = (V - Vold) / im->V0[e];
        double mu = rho / M->rho0 - 1;
        double f0 = im_mg_pressure(M, mu, 0), gam = im_mg_pressure(M, mu, 1) - f0; /* p = f0 + gam E */
        double Es = im->E[e] + dt * Vm / im->V0[e] * sd - (0.5 * im->p[e] + q) * dv;
        double En = (Es - 0.5 * f0 * dv) / (1 + 0.5 * gam * dv);
        im->E[e] = En;
        double pr = f0 + gam * En;
        im->p[e] = pr, im->q[e] = q;
        sv[0] = sn[0][0], sv[1] = sn[1][1], sv[2] = sn[2][2], sv[3] = sn[0][1], sv[4] = sn[1][2], sv[5] = sn[0][2];
        im->vm[e] = sqrt(1.5 * (sn[0][0] * sn[0][0] + sn[1][1] * sn[1][1] + sn[2][2] * sn[2][2] + 2 * (sn[0][1] * sn[0][1] + sn[1][2] * sn[1][2] + sn[0][2] * sn[0][2])));
        /* erosion */
        if ((M->fail_strain > 0 && im->ep[e] > M->fail_strain) || (M->fail_volumetric > 0 && fabs(V / im->V0[e] - 1) > M->fail_volumetric)) {
            im->eroded[e] = 1;
            memset(fe, 0, 24 * sizeof(double));
            im->dt_el[e] = INFINITY;
            continue;
        }
        /* nodal forces: f_iI = -sigma_ij b_jI (b already carries V) */
        double sg[3][3];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) sg[i][j] = sn[i][j] - (i == j ? pr + q : 0);
        for (int I = 0; I < 8; I++)
            for (int i = 0; i < 3; i++) fe[3 * I + i] = -(sg[i][0] * b[I][0] + sg[i][1] * b[I][1] + sg[i][2] * b[I][2]);
        /* viscous hourglass control (Flanagan and Belytschko): gamma_a = h_a - (h_a . x_i) b_i / V */
        double coef = im->spec.hourglass * rho * c * cbrt(V * V) / 4; /* Flanagan and Belytschko's scaling */
        double whg = 0;
        for (int a = 0; a < 4; a++) {
            double hx[3] = {0, 0, 0};
            for (int J = 0; J < 8; J++)
                for (int k = 0; k < 3; k++) hx[k] += HG[a][J] * X[J][k];
            double gam[8];
            for (int I = 0; I < 8; I++) gam[I] = HG[a][I] - (hx[0] * b[I][0] + hx[1] * b[I][1] + hx[2] * b[I][2]) / V;
            double qa[3] = {0, 0, 0};
            for (int I = 0; I < 8; I++)
                for (int k = 0; k < 3; k++) qa[k] += U[I][k] * gam[I];
            for (int I = 0; I < 8; I++)
                for (int k = 0; k < 3; k++) {
                    double fh = -coef * qa[k] * gam[I];
                    fe[3 * I + k] += fh;
                    whg -= fh * U[I][k];
                }
        }
        im->ehg[e] += dt * whg; /* dissipated by the hourglass viscosity, accounted apart from E */
        /* the stable step: the smallest dimension over the wave speed, with the viscosity's share */
        double Q = im->spec.q_linear * c + im->spec.q_quadratic * lc * fabs(fmin(trD, 0));
        im->dt_el[e] = lc / (Q + sqrt(Q * Q + c * c));
        (void)tid;
    }
}

static void gather_fn(void *ctx, int begin, int end, int tid) {
    Impact *im = ctx;
    for (int n = begin; n < end; n++) {
        double f[3] = {0, 0, 0};
        for (int k = im->adj_start[n]; k < im->adj_start[n + 1]; k++) {
            const double *fe = im->fe + 3 * (size_t)im->adj[k];
            f[0] += fe[0], f[1] += fe[1], f[2] += fe[2];
        }
        im->f[3 * n] = f[0], im->f[3 * n + 1] = f[1], im->f[3 * n + 2] = f[2];
    }
}

/* ---- contact ------------------------------------------------------------------------------------------------------- */

static int cmp_face_key(const void *a, const void *b) {
    const int *x = a, *y = b;
    for (int k = 0; k < 4; k++)
        if (x[k] != y[k]) return x[k] < y[k] ? -1 : 1;
    return 0;
}

static void build_faces(Impact *im) {
    int ne = im->ne;
    int *keys = malloc((size_t)ne * 6 * 6 * sizeof(int)); /* 4 sorted nodes, element, face */
    int nk = 0;
    for (int e = 0; e < ne; e++) {
        if (im->eroded[e]) continue;
        const int *cn = im->spec.mesh.conn + 8 * (size_t)e;
        for (int f = 0; f < 6; f++) {
            int *k = keys + 6 * nk++;
            for (int j = 0; j < 4; j++) k[j] = cn[HF[f][j]];
            for (int i = 1; i < 4; i++)
                for (int j = i; j > 0 && k[j - 1] > k[j]; j--) {
                    int t = k[j];
                    k[j] = k[j - 1], k[j - 1] = t;
                }
            k[4] = e, k[5] = f;
        }
    }
    qsort(keys, (size_t)nk, 6 * sizeof(int), cmp_face_key);
    free(im->faces);
    im->faces = malloc((size_t)(nk ? nk : 1) * sizeof(Face));
    im->nfaces = 0;
    for (int i = 0; i < nk;) {
        int j = i + 1;
        while (j < nk && cmp_face_key(keys + 6 * i, keys + 6 * j) == 0) j++;
        if (j - i == 1) {
            int e = keys[6 * i + 4], f = keys[6 * i + 5];
            Face F;
            for (int k = 0; k < 4; k++) F.n[k] = im->spec.mesh.conn[8 * (size_t)e + HF[f][k]];
            F.elem = e;
            F.body = im->node_body[F.n[0]];
            /* a face on a symmetry plane is not a surface of the body, it is where its mirror image begins */
            bool on_sym = false;
            for (int ax = 0; ax < 2 && !on_sym; ax++) {
                if (!(ax == 0 ? im->spec.sym_x : im->spec.sym_y)) continue;
                bool all = true;
                for (int k = 0; k < 4; k++) all = all && fabs(im->spec.mesh.xyz[3 * F.n[k] + ax]) < 1e-12;
                on_sym = all;
            }
            if (!on_sym) im->faces[im->nfaces++] = F;
        }
        i = j;
    }
    free(keys);
    im->faces_dirty = false;
}

/* penalty contact between the surfaces of different bodies: every surface node of one body is pushed out of the
 * faces of the others it has entered (both ways, so the pair is symmetric) */
static void contact(Impact *im) {
    memset(im->fc, 0, (size_t)im->nn * 3 * sizeof(double));
    if (im->spec.nbodies < 2) return;
    if (im->faces_dirty) build_faces(im);
    /* a uniform grid of the faces' bounding boxes */
    double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY}, hmax = 0;
    for (int f = 0; f < im->nfaces; f++)
        for (int k = 0; k < 4; k++) {
            const double *p = im->x + 3 * im->faces[f].n[k];
            for (int d = 0; d < 3; d++) lo[d] = fmin(lo[d], p[d]), hi[d] = fmax(hi[d], p[d]);
            const double *q = im->x + 3 * im->faces[f].n[(k + 1) & 3];
            hmax = fmax(hmax, sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2])));
        }
    if (im->nfaces == 0 || !(hmax > 0)) return;
    double cell = 2 * hmax;
    int g[3];
    for (int d = 0; d < 3; d++) {
        lo[d] -= cell, hi[d] += cell;
        g[d] = (int)ceil((hi[d] - lo[d]) / cell);
        if (g[d] < 1) g[d] = 1;
        if (g[d] > 256) g[d] = 256;
    }
    double cs[3] = {(hi[0] - lo[0]) / g[0], (hi[1] - lo[1]) / g[1], (hi[2] - lo[2]) / g[2]};
    long ncell = (long)g[0] * g[1] * g[2];
    int *start = calloc((size_t)ncell + 1, sizeof(int));
    /* two passes: count, then fill */
    int *list = NULL;
    for (int pass = 0; pass < 2; pass++) {
        int *fill = pass ? calloc((size_t)ncell, sizeof(int)) : NULL;
        for (int f = 0; f < im->nfaces; f++) {
            double bl[3] = {INFINITY, INFINITY, INFINITY}, bh[3] = {-INFINITY, -INFINITY, -INFINITY};
            for (int k = 0; k < 4; k++)
                for (int d = 0; d < 3; d++) bl[d] = fmin(bl[d], im->x[3 * im->faces[f].n[k] + d]), bh[d] = fmax(bh[d], im->x[3 * im->faces[f].n[k] + d]);
            int a[3], b[3];
            for (int d = 0; d < 3; d++) {
                a[d] = (int)((bl[d] - 0.5 * hmax - lo[d]) / cs[d]), b[d] = (int)((bh[d] + 0.5 * hmax - lo[d]) / cs[d]);
                a[d] = a[d] < 0 ? 0 : a[d] >= g[d] ? g[d] - 1 : a[d];
                b[d] = b[d] < 0 ? 0 : b[d] >= g[d] ? g[d] - 1 : b[d];
            }
            for (int k = a[2]; k <= b[2]; k++)
                for (int j = a[1]; j <= b[1]; j++)
                    for (int i = a[0]; i <= b[0]; i++) {
                        long c = ((long)k * g[1] + j) * g[0] + i;
                        if (pass == 0) start[c + 1]++;
                        else list[start[c] + fill[c]++] = f;
                    }
        }
        if (pass == 0) {
            for (long c = 0; c < ncell; c++) start[c + 1] += start[c];
            list = malloc((size_t)(start[ncell] ? start[ncell] : 1) * sizeof(int));
        }
        free(fill);
    }
    /* the surface nodes, with their outward normals (the sum of their faces' area vectors) */
    unsigned char *surf = calloc((size_t)im->nn, 1);
    double *nn_ = calloc((size_t)im->nn * 3, sizeof(double));
    for (int f = 0; f < im->nfaces; f++) {
        const int *q = im->faces[f].n;
        const double *p0 = im->x + 3 * q[0], *p1 = im->x + 3 * q[1], *p2 = im->x + 3 * q[2], *p3 = im->x + 3 * q[3];
        double d1[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]}, d2[3] = {p3[0] - p1[0], p3[1] - p1[1], p3[2] - p1[2]};
        double cr[3] = {d1[1] * d2[2] - d1[2] * d2[1], d1[2] * d2[0] - d1[0] * d2[2], d1[0] * d2[1] - d1[1] * d2[0]};
        for (int k = 0; k < 4; k++) {
            surf[q[k]] = 1;
            for (int d = 0; d < 3; d++) nn_[3 * q[k] + d] += cr[d];
        }
    }
    double wc = 0;
    for (int n = 0; n < im->nn; n++) {
        if (!surf[n]) continue;
        const double *P = im->x + 3 * n;
        int ci[3];
        for (int d = 0; d < 3; d++) {
            ci[d] = (int)((P[d] - lo[d]) / cs[d]);
            if (ci[d] < 0 || ci[d] >= g[d]) goto next_node;
        }
        long c = ((long)ci[2] * g[1] + ci[1]) * g[0] + ci[0];
        double best = INFINITY, bn[3] = {0}, bw[3] = {0};
        int bt[3] = {-1, -1, -1}, bf = -1;
        for (int k = start[c]; k < start[c + 1]; k++) {
            const Face *F = &im->faces[list[k]];
            if (F->body == im->node_body[n]) continue;
            if (F->n[0] == n || F->n[1] == n || F->n[2] == n || F->n[3] == n) continue;
            /* two triangles */
            for (int t = 0; t < 2; t++) {
                int ia = F->n[0], ib = F->n[t ? 2 : 1], ic = F->n[t ? 3 : 2];
                const double *A = im->x + 3 * ia, *Bp = im->x + 3 * ib, *C = im->x + 3 * ic;
                double e1[3] = {Bp[0] - A[0], Bp[1] - A[1], Bp[2] - A[2]}, e2[3] = {C[0] - A[0], C[1] - A[1], C[2] - A[2]};
                double nrm[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                double nl = sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
                if (!(nl > 0)) continue;
                for (int d = 0; d < 3; d++) nrm[d] /= nl;
                /* the node's own surface must face this one: a node on the end of one bar is not in contact with the
                 * side of the other, whatever the distances say */
                const double *sn = nn_ + 3 * n;
                double sl = sqrt(sn[0] * sn[0] + sn[1] * sn[1] + sn[2] * sn[2]);
                if (sl > 0 && (sn[0] * nrm[0] + sn[1] * nrm[1] + sn[2] * nrm[2]) / sl > -0.5) continue;
                double w[3] = {P[0] - A[0], P[1] - A[1], P[2] - A[2]};
                double dist = w[0] * nrm[0] + w[1] * nrm[1] + w[2] * nrm[2]; /* > 0 outside */
                if (dist >= 0 || dist < -0.5 * hmax) continue;
                /* barycentric coordinates of the projection */
                double pw[3] = {w[0] - dist * nrm[0], w[1] - dist * nrm[1], w[2] - dist * nrm[2]};
                double d00 = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2], d01 = e1[0] * e2[0] + e1[1] * e2[1] + e1[2] * e2[2];
                double d11 = e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2];
                double d20 = pw[0] * e1[0] + pw[1] * e1[1] + pw[2] * e1[2], d21 = pw[0] * e2[0] + pw[1] * e2[1] + pw[2] * e2[2];
                double den = d00 * d11 - d01 * d01;
                double vb = (d11 * d20 - d01 * d21) / den, wb = (d00 * d21 - d01 * d20) / den, ub = 1 - vb - wb;
                const double tol = -0.02;
                if (ub < tol || vb < tol || wb < tol) continue;
                if (-dist < best) {
                    best = -dist, bf = F->elem;
                    for (int d = 0; d < 3; d++) bn[d] = nrm[d];
                    bt[0] = ia, bt[1] = ib, bt[2] = ic, bw[0] = ub, bw[1] = vb, bw[2] = wb;
                }
            }
        }
        /* a contact found deeper than a step can bring a node is a face that erosion has just exposed with the node
         * already behind it: pushing on it would release energy that was never there (the first sphere-and-plate run
         * gained 31 per cent). It is left alone, as eroding contact algorithms do with initial penetrations */
        if (bf >= 0 && !im->touching[n]) {
            const double *vn = im->v + 3 * n;
            double step = 3 * im->dt * sqrt(vn[0] * vn[0] + vn[1] * vn[1] + vn[2] * vn[2]) + 0.02 * hmax;
            if (best > step) bf = -1;
        }
        im->touching[n] = bf >= 0;
        if (bf >= 0) {
            const ImMaterial *M = &im->spec.materials[elem_material(im, bf)];
            double K = M->rho0 * M->c0 * M->c0;
            double Ve = im->V[bf];
            /* the face area of the master element, about V^(2/3) */
            double k = im->spec.contact_scale * K * cbrt(Ve);
            double fmag = k * best;
            for (int d = 0; d < 3; d++) {
                im->fc[3 * n + d] += fmag * bn[d];
                for (int j = 0; j < 3; j++) im->fc[3 * bt[j] + d] -= fmag * bn[d] * bw[j];
            }
            wc += 0.5 * k * best * best;
        }
    next_node:;
    }
    im->w_contact = wc;
    free(surf), free(nn_), free(start), free(list);
}

/* ---- the step ------------------------------------------------------------------------------------------------------ */

static void apply_constraints(Impact *im) {
    const double tol = 1e-12;
    double lost = 0;
    for (int n = 0; n < im->nn; n++) {
        double *x = im->x + 3 * n, *v = im->v + 3 * n;
        if (im->spec.uniaxial) v[0] = v[1] = 0, x[0] = im->spec.mesh.xyz[3 * n], x[1] = im->spec.mesh.xyz[3 * n + 1];
        if (im->spec.sym_x && fabs(im->spec.mesh.xyz[3 * n]) < tol) v[0] = 0, x[0] = 0;
        if (im->spec.sym_y && fabs(im->spec.mesh.xyz[3 * n + 1]) < tol) v[1] = 0, x[1] = 0;
        if (im->spec.anvil && x[2] < im->spec.anvil_z) {
            x[2] = im->spec.anvil_z;
            if (v[2] < 0) {
                /* what is lost is the velocity the node brought to the wall; the velocity its element gave it during
                 * this step, while it rested there, was never kinetic energy (the element did no work on it) */
                if (im->vz_prev[n] < 0) lost += 0.5 * im->m[n] * im->vz_prev[n] * im->vz_prev[n];
                v[2] = 0;
            }
        }
    }
    im->w_anvil += lost;
}

double im_step(Impact *im) {
    /* forces at the current configuration (x^n, v^(n-1/2)) and the stable step */
    pool_for(im->pool, im->ne, 256, element_fn, im);
    int before = im->neroded;
    im->neroded = 0;
    double dtmin = INFINITY;
    for (int e = 0; e < im->ne; e++) {
        im->neroded += im->eroded[e];
        if (im->dt_el[e] < dtmin) dtmin = im->dt_el[e];
    }
    if (im->neroded != before) im->faces_dirty = true;
    pool_for(im->pool, im->nn, 1024, gather_fn, im);
    contact(im);
    double dtn = im->spec.safety * dtmin;
    if (!isfinite(dtn)) dtn = 1e-9;
    if (im->steps > 0 && dtn > 1.1 * im->dt) dtn = 1.1 * im->dt; /* the step grows gently */
    if (im->steps == 0 && dtn > im->dt) dtn = im->dt;
    /* central difference */
    for (int n = 0; n < im->nn; n++) im->vz_prev[n] = im->v[3 * n + 2];
    for (int n = 0; n < im->nn; n++)
        for (int k = 0; k < 3; k++) {
            double a = (im->f[3 * n + k] + im->fc[3 * n + k]) / im->m[n];
            im->v[3 * n + k] += (im->steps == 0 ? 0.5 : 1.0) * dtn * a;
            im->x[3 * n + k] += dtn * im->v[3 * n + k];
        }
    apply_constraints(im);
    im->dt = dtn;
    im->t += dtn;
    im->steps++;
    return dtn;
}

double im_time(const Impact *im) { return im->t; }
long im_steps(const Impact *im) { return im->steps; }
double im_dt(const Impact *im) { return im->dt; }
const double *im_positions(const Impact *im) { return im->x; }
const double *im_velocities(const Impact *im) { return im->v; }
const double *im_plastic_strain(const Impact *im) { return im->ep; }
const double *im_pressure(const Impact *im) { return im->p; }
const double *im_von_mises(const Impact *im) { return im->vm; }
const double *im_temperature(const Impact *im) { return im->T; }
const unsigned char *im_eroded(const Impact *im) { return im->eroded; }
int im_eroded_count(const Impact *im) { return im->neroded; }

void im_energy(const Impact *im, ImEnergy *e) {
    memset(e, 0, sizeof *e);
    for (int n = 0; n < im->nn; n++) e->kinetic += 0.5 * im->m[n] * (im->v[3 * n] * im->v[3 * n] + im->v[3 * n + 1] * im->v[3 * n + 1] + im->v[3 * n + 2] * im->v[3 * n + 2]);
    for (int k = 0; k < im->ne; k++) {
        if (im->eroded[k]) e->eroded += im->E[k] * im->V0[k];
        else e->internal += im->E[k] * im->V0[k];
        e->hourglass += im->ehg[k];
    }
    e->contact = im->w_contact;
    e->total = e->kinetic + e->internal + e->eroded + e->hourglass + e->contact + im->w_anvil;
}

void im_body_momentum(const Impact *im, int body, double p[3]) {
    p[0] = p[1] = p[2] = 0;
    const ImBody *B = &im->spec.bodies[body];
    for (int n = B->first_node; n < B->first_node + B->nnodes; n++)
        for (int k = 0; k < 3; k++) p[k] += im->m[n] * im->v[3 * n + k];
}

/* ---- output -------------------------------------------------------------------------------------------------------- */

void im_write_frame(Impact *im, LabWriter *w, bool mirror) {
    int copies = 1;
    double sgn[4][2] = {{1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
    if (mirror) copies = (im->spec.sym_x ? 2 : 1) * (im->spec.sym_y ? 2 : 1);
    int live = im->ne - im->neroded;
    int nn = im->nn * copies;
    double *X = malloc((size_t)nn * 3 * sizeof(double));
    int *C = malloc((size_t)live * copies * 8 * sizeof(int));
    float *ep = malloc((size_t)live * copies * sizeof(float)), *pr = malloc((size_t)live * copies * sizeof(float));
    float *vm = malloc((size_t)live * copies * sizeof(float)), *T = malloc((size_t)live * copies * sizeof(float));
    float *sp = malloc((size_t)nn * sizeof(float));
    if (!X || !C || !ep || !pr || !vm || !T || !sp) goto done;
    int k = 0;
    for (int cpy = 0; cpy < copies; cpy++) {
        int ci = copies == 2 && !im->spec.sym_x ? (cpy ? 2 : 0) : cpy; /* only y mirrored: copies 0 and 2 */
        double sx = sgn[ci][0], sy = sgn[ci][1];
        for (int n = 0; n < im->nn; n++) {
            X[3 * (cpy * im->nn + n)] = sx * im->x[3 * n], X[3 * (cpy * im->nn + n) + 1] = sy * im->x[3 * n + 1], X[3 * (cpy * im->nn + n) + 2] = im->x[3 * n + 2];
            sp[cpy * im->nn + n] = (float)sqrt(im->v[3 * n] * im->v[3 * n] + im->v[3 * n + 1] * im->v[3 * n + 1] + im->v[3 * n + 2] * im->v[3 * n + 2]);
        }
        bool flip = sx * sy < 0; /* a mirror image turns the element inside out: swap to keep it right-handed */
        for (int e = 0; e < im->ne; e++) {
            if (im->eroded[e]) continue;
            const int *cn = im->spec.mesh.conn + 8 * (size_t)e;
            static const int SW[8] = {1, 0, 3, 2, 5, 4, 7, 6};
            for (int j = 0; j < 8; j++) C[8 * k + j] = cpy * im->nn + cn[flip ? SW[j] : j];
            ep[k] = (float)im->ep[e], pr[k] = (float)im->p[e], vm[k] = (float)im->vm[e], T[k] = (float)im->T[e];
            k++;
        }
    }
    lab_part_cells(w, "solid", nn, X, k, LAB_HEX, C);
    lab_field(w, "ep", LAB_AT_CELL, (size_t)k, ep);
    lab_field(w, "p", LAB_AT_CELL, (size_t)k, pr);
    lab_field(w, "vm", LAB_AT_CELL, (size_t)k, vm);
    lab_field(w, "T", LAB_AT_CELL, (size_t)k, T);
    lab_field(w, "speed", LAB_AT_NODE, (size_t)nn, sp);
done:
    free(X), free(C), free(ep), free(pr), free(vm), free(T), free(sp);
}

char *im_header_json(const ImSpec *s, const char *title) {
    char *h = malloc(4096);
    if (!h) return NULL;
    int n = snprintf(h, 4096,
                     "{\"domain\":\"impact\",\"title\":\"%s\",\"solver\":\"src/lab/impact: explicit Lagrangian hex8, Johnson-Cook, Mie-Gruneisen, "
                     "bulk viscosity, erosion, penalty contact\",\"elements\":%d,\"nodes\":%d,\"materials\":[",
                     title ? title : "", s->mesh.nelems, s->mesh.nnodes);
    for (int m = 0; m < s->nmaterials && n < 3800; m++) n += snprintf(h + n, 4096 - (size_t)n, "%s\"%s\"", m ? "," : "", s->materials[m].name);
    snprintf(h + n, 4096 - (size_t)n, "],\"fields\":{\"ep\":\"1\",\"p\":\"Pa\",\"vm\":\"Pa\",\"T\":\"K\",\"speed\":\"m/s\"}}");
    return h;
}

/* ---- meshes -------------------------------------------------------------------------------------------------------- */

static bool mesh_append(ImMesh *m, int nn, const double *xyz, int ne, const int *conn) {
    double *x = realloc(m->xyz, (size_t)(m->nnodes + nn) * 3 * sizeof(double));
    int *c = realloc(m->conn, (size_t)(m->nelems + ne) * 8 * sizeof(int));
    if (!x || !c) return false;
    m->xyz = x, m->conn = c;
    memcpy(m->xyz + 3 * (size_t)m->nnodes, xyz, (size_t)nn * 3 * sizeof(double));
    for (int i = 0; i < ne * 8; i++) m->conn[8 * (size_t)m->nelems + i] = conn[i] + m->nnodes;
    m->nnodes += nn, m->nelems += ne;
    return true;
}

void im_mesh_free(ImMesh *m) {
    free(m->xyz), free(m->conn);
    memset(m, 0, sizeof *m);
}

/* merge coincident nodes of a node list (sort by rounded coordinates), renumbering the connectivity */
static int merge_nodes(double *xyz, int nn, int *conn, int nconn, double tol) {
    int *order = malloc((size_t)nn * sizeof(int)), *map = malloc((size_t)nn * sizeof(int));
    for (int i = 0; i < nn; i++) order[i] = i, map[i] = -1;
    int nu = 0;
    /* a simple O(n^2) pass bucketed by a coarse hash: meshes here are at most a few hundred thousand nodes */
    int nb = 1 << 16;
    int *head = malloc((size_t)nb * sizeof(int)), *next = malloc((size_t)nn * sizeof(int));
    for (int i = 0; i < nb; i++) head[i] = -1;
    double *out = malloc((size_t)nn * 3 * sizeof(double));
    for (int i = 0; i < nn; i++) {
        long hx = lround(xyz[3 * i] / tol), hy = lround(xyz[3 * i + 1] / tol), hz = lround(xyz[3 * i + 2] / tol);
        int found = -1;
        for (long dx = -1; dx <= 1 && found < 0; dx++)
            for (long dy = -1; dy <= 1 && found < 0; dy++)
                for (long dz = -1; dz <= 1 && found < 0; dz++) {
                    unsigned long h = ((unsigned long)(hx + dx) * 73856093ul) ^ ((unsigned long)(hy + dy) * 19349663ul) ^ ((unsigned long)(hz + dz) * 83492791ul);
                    for (int j = head[h & (unsigned long)(nb - 1)]; j >= 0; j = next[j]) {
                        double d = fabs(out[3 * j] - xyz[3 * i]) + fabs(out[3 * j + 1] - xyz[3 * i + 1]) + fabs(out[3 * j + 2] - xyz[3 * i + 2]);
                        if (d < tol) {
                            found = j;
                            break;
                        }
                    }
                }
        if (found < 0) {
            found = nu++;
            for (int k = 0; k < 3; k++) out[3 * found + k] = xyz[3 * i + k];
            unsigned long h = ((unsigned long)hx * 73856093ul) ^ ((unsigned long)hy * 19349663ul) ^ ((unsigned long)hz * 83492791ul);
            next[found] = head[h & (unsigned long)(nb - 1)];
            head[h & (unsigned long)(nb - 1)] = found;
        }
        map[i] = found;
    }
    for (int i = 0; i < nconn; i++) conn[i] = map[conn[i]];
    memcpy(xyz, out, (size_t)nu * 3 * sizeof(double));
    free(order), free(map), free(head), free(next), free(out);
    return nu;
}

/* make every hexahedron right-handed (positive volume) by swapping its top and bottom faces if needed */
static void orient(double *xyz, int *conn, int ne) {
    for (int e = 0; e < ne; e++) {
        double X[8][3], b[8][3];
        for (int I = 0; I < 8; I++)
            for (int k = 0; k < 3; k++) X[I][k] = xyz[3 * conn[8 * e + I] + k];
        if (hex_volume_gradient(X, b) < 0)
            for (int I = 0; I < 4; I++) {
                int t = conn[8 * e + I];
                conn[8 * e + I] = conn[8 * e + I + 4], conn[8 * e + I + 4] = t;
            }
    }
}

bool im_mesh_cylinder(ImMesh *m, double r, double z0, double length, int nq, int nr, int nz, bool quarter) {
    /* the quarter disc: an inner square [0, a]^2 and two outer blocks mapped onto the arc */
    const double a = 0.5 * r;
    int n2max = 4 * ((nq + 1) * (nq + 1) + 2 * (nq + 1) * (nr + 1));
    double *p2 = malloc((size_t)n2max * 2 * sizeof(double));
    int *q2 = malloc((size_t)4 * (nq * nq + 2 * nq * nr) * 4 * sizeof(int));
    int np = 0, nqd = 0;
    int nquarters = quarter ? 1 : 4;
    for (int qd = 0; qd < nquarters; qd++) {
        double cr = cos(qd * M_PI / 2), sr = sin(qd * M_PI / 2);
#define ADDP(X, Y) (p2[2 * np] = cr * (X) - sr * (Y), p2[2 * np + 1] = sr * (X) + cr * (Y), np++)
        int b0 = np;
        for (int j = 0; j <= nq; j++)
            for (int i = 0; i <= nq; i++) ADDP(a * i / nq, a * j / nq);
        for (int j = 0; j < nq; j++)
            for (int i = 0; i < nq; i++) {
                int *q = q2 + 4 * nqd++;
                q[0] = b0 + j * (nq + 1) + i, q[1] = q[0] + 1, q[2] = q[0] + nq + 2, q[3] = q[0] + nq + 1;
            }
        for (int blk = 0; blk < 2; blk++) {
            int b1 = np;
            for (int k = 0; k <= nr; k++)
                for (int i = 0; i <= nq; i++) {
                    double t = (double)i / nq;
                    double px = blk == 0 ? a : a * t, py = blk == 0 ? a * t : a;
                    double th = blk == 0 ? (M_PI / 4) * t : M_PI / 2 - (M_PI / 4) * t;
                    double qx = r * cos(th), qy = r * sin(th);
                    double s = (double)k / nr;
                    ADDP(px + s * (qx - px), py + s * (qy - py));
                }
            for (int k = 0; k < nr; k++)
                for (int i = 0; i < nq; i++) {
                    int *q = q2 + 4 * nqd++;
                    q[0] = b1 + k * (nq + 1) + i, q[1] = q[0] + 1, q[2] = q[0] + nq + 2, q[3] = q[0] + nq + 1;
                }
        }
#undef ADDP
    }
    /* extrude */
    int nn = np * (nz + 1), ne = nqd * nz;
    double *xyz = malloc((size_t)nn * 3 * sizeof(double));
    int *conn = malloc((size_t)ne * 8 * sizeof(int));
    for (int k = 0; k <= nz; k++)
        for (int i = 0; i < np; i++) {
            xyz[3 * (k * np + i)] = p2[2 * i], xyz[3 * (k * np + i) + 1] = p2[2 * i + 1], xyz[3 * (k * np + i) + 2] = z0 + length * k / nz;
        }
    for (int k = 0; k < nz; k++)
        for (int e = 0; e < nqd; e++)
            for (int j = 0; j < 4; j++) conn[8 * (k * nqd + e) + j] = k * np + q2[4 * e + j], conn[8 * (k * nqd + e) + 4 + j] = (k + 1) * np + q2[4 * e + j];
    int nu = merge_nodes(xyz, nn, conn, ne * 8, 1e-9 * r);
    orient(xyz, conn, ne);
    bool ok = mesh_append(m, nu, xyz, ne, conn);
    free(p2), free(q2), free(xyz), free(conn);
    return ok;
}

bool im_mesh_box(ImMesh *m, const double lo[3], const double hi[3], const int n[3]) {
    int nn = (n[0] + 1) * (n[1] + 1) * (n[2] + 1), ne = n[0] * n[1] * n[2];
    double *xyz = malloc((size_t)nn * 3 * sizeof(double));
    int *conn = malloc((size_t)ne * 8 * sizeof(int));
#define NID(i, j, k) (((k) * (n[1] + 1) + (j)) * (n[0] + 1) + (i))
    for (int k = 0; k <= n[2]; k++)
        for (int j = 0; j <= n[1]; j++)
            for (int i = 0; i <= n[0]; i++) {
                int id = NID(i, j, k);
                xyz[3 * id] = lo[0] + (hi[0] - lo[0]) * i / n[0];
                xyz[3 * id + 1] = lo[1] + (hi[1] - lo[1]) * j / n[1];
                xyz[3 * id + 2] = lo[2] + (hi[2] - lo[2]) * k / n[2];
            }
    int e = 0;
    for (int k = 0; k < n[2]; k++)
        for (int j = 0; j < n[1]; j++)
            for (int i = 0; i < n[0]; i++) {
                int *c = conn + 8 * e++;
                c[0] = NID(i, j, k), c[1] = NID(i + 1, j, k), c[2] = NID(i + 1, j + 1, k), c[3] = NID(i, j + 1, k);
                c[4] = NID(i, j, k + 1), c[5] = NID(i + 1, j, k + 1), c[6] = NID(i + 1, j + 1, k + 1), c[7] = NID(i, j + 1, k + 1);
            }
#undef NID
    bool ok = mesh_append(m, nn, xyz, ne, conn);
    free(xyz), free(conn);
    return ok;
}

bool im_mesh_sphere(ImMesh *m, const double c[3], double r, int nq, bool quarter) {
    /* an inner cube of half side a, and six blocks from its faces to the sphere, nr layers; the cube's grid points are
     * projected along rays from the centre */
    double a = 0.45 * r;
    int nr = (int)ceil(nq * (r - a) / (2 * a));
    if (nr < 1) nr = 1;
    int per_face_nodes = (nq + 1) * (nq + 1) * (nr + 1);
    int nn = (nq + 1) * (nq + 1) * (nq + 1) + 6 * per_face_nodes, ne = nq * nq * nq + 6 * nq * nq * nr;
    double *xyz = malloc((size_t)nn * 3 * sizeof(double));
    int *conn = malloc((size_t)ne * 8 * sizeof(int));
    int np = 0, e = 0;
    int b0 = np;
    for (int k = 0; k <= nq; k++)
        for (int j = 0; j <= nq; j++)
            for (int i = 0; i <= nq; i++) {
                xyz[3 * np] = -a + 2 * a * i / nq, xyz[3 * np + 1] = -a + 2 * a * j / nq, xyz[3 * np + 2] = -a + 2 * a * k / nq;
                np++;
            }
    for (int k = 0; k < nq; k++)
        for (int j = 0; j < nq; j++)
            for (int i = 0; i < nq; i++) {
                int *cc = conn + 8 * e++;
                int id = b0 + (k * (nq + 1) + j) * (nq + 1) + i, sj = nq + 1, sk = (nq + 1) * (nq + 1);
                cc[0] = id, cc[1] = id + 1, cc[2] = id + 1 + sj, cc[3] = id + sj;
                cc[4] = id + sk, cc[5] = id + 1 + sk, cc[6] = id + 1 + sj + sk, cc[7] = id + sj + sk;
            }
    /* the six faces: axis and sign */
    for (int ax = 0; ax < 3; ax++)
        for (int sg = -1; sg <= 1; sg += 2) {
            int b1 = np;
            int u = (ax + 1) % 3, v = (ax + 2) % 3;
            for (int l = 0; l <= nr; l++)
                for (int j = 0; j <= nq; j++)
                    for (int i = 0; i <= nq; i++) {
                        double P[3];
                        P[ax] = sg * a, P[u] = -a + 2 * a * i / nq, P[v] = -a + 2 * a * j / nq;
                        double len = sqrt(P[0] * P[0] + P[1] * P[1] + P[2] * P[2]);
                        double Q[3] = {P[0] / len * r, P[1] / len * r, P[2] / len * r};
                        double s = (double)l / nr;
                        for (int d = 0; d < 3; d++) xyz[3 * np + d] = P[d] + s * (Q[d] - P[d]);
                        np++;
                    }
            for (int l = 0; l < nr; l++)
                for (int j = 0; j < nq; j++)
                    for (int i = 0; i < nq; i++) {
                        int *cc = conn + 8 * e++;
                        int id = b1 + (l * (nq + 1) + j) * (nq + 1) + i, sj = nq + 1, sl = (nq + 1) * (nq + 1);
                        cc[0] = id, cc[1] = id + 1, cc[2] = id + 1 + sj, cc[3] = id + sj;
                        cc[4] = id + sl, cc[5] = id + 1 + sl, cc[6] = id + 1 + sj + sl, cc[7] = id + sj + sl;
                    }
        }
    int nu = merge_nodes(xyz, np, conn, e * 8, 1e-9 * r);
    orient(xyz, conn, e);
    /* keep a quarter: elements whose centroid has x >= 0 and y >= 0 */
    if (quarter) {
        int k = 0;
        for (int q = 0; q < e; q++) {
            double cx = 0, cy = 0;
            for (int I = 0; I < 8; I++) cx += xyz[3 * conn[8 * q + I]], cy += xyz[3 * conn[8 * q + I] + 1];
            if (cx >= 0 && cy >= 0) memmove(conn + 8 * k++, conn + 8 * q, 8 * sizeof(int));
        }
        e = k;
        /* drop the nodes no longer used */
        int *map = malloc((size_t)nu * sizeof(int));
        for (int i = 0; i < nu; i++) map[i] = -1;
        int m2 = 0;
        for (int i = 0; i < e * 8; i++)
            if (map[conn[i]] < 0) map[conn[i]] = m2++;
        double *x2 = malloc((size_t)m2 * 3 * sizeof(double));
        for (int i = 0; i < nu; i++)
            if (map[i] >= 0)
                for (int d = 0; d < 3; d++) x2[3 * map[i] + d] = xyz[3 * i + d];
        for (int i = 0; i < e * 8; i++) conn[i] = map[conn[i]];
        memcpy(xyz, x2, (size_t)m2 * 3 * sizeof(double));
        free(x2), free(map);
        nu = m2;
    }
    for (int i = 0; i < nu; i++)
        for (int d = 0; d < 3; d++) xyz[3 * i + d] += c[d];
    bool ok = mesh_append(m, nu, xyz, e, conn);
    free(xyz), free(conn);
    return ok;
}
