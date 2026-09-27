/* explicit.c - central-difference explicit dynamics (explicit.h, docs/contracts/dynamics.md step 2) */
#include "explicit.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hex8.h"

/* the four hourglass modes of the trilinear hexahedron, in this project's node order */
static const double HG[4][8] = {{1, 1, -1, -1, -1, -1, 1, 1}, {1, -1, -1, 1, -1, 1, 1, -1}, {1, -1, 1, -1, 1, -1, 1, -1}, {-1, 1, -1, 1, 1, -1, 1, -1}};

struct ExDyn {
    ExMesh mesh;
    ExMaterial mat;
    ExOptions opt;
    unsigned char *fixed;
    double *fixed_v;
    double *u, *v, *a, *f_ext, *f_int, *mass;
    double *stress;      /* 6 * nelems */
    Hex8NlState *state;  /* one point per element */
    double *gamma;       /* 32 * nelems: the hourglass shape vectors */
    ExContact *contact;
    double *f_con, *x_now;
    bool contact_set_step;
    double dt, time, mass_added;
    int steps, critical_elem;
    double e_internal, e_hourglass, e_external, e_start, e_stored;
    bool started;
};

static double bulk_modulus(const Hex8NlMaterial *m) { return m->E / (3 * (1 - 2 * m->nu)); }

/* The element's highest natural frequency with its lumped mass, by power iteration on M^-1 K, K being the one-point
 * stiffness plus the hourglass stiffness. The stable step of the central difference is 2 / omega_max, and this is
 * what it means: a characteristic length over the wave speed is an estimate of it that a distorted element breaks
 * (a mapped sphere blew up while its step "looked safe"), so the step is taken from the frequency itself and the
 * length is kept only for the report. */
static double element_max_frequency(const double dNdX[8][3], double vol, const double gam[4][8], double lam, double mu, double rho, double hg) {
    double K[24][24] = {{0}};
    double C[6][6] = {{0}};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) C[i][j] = lam + (i == j ? 2 * mu : 0);
    for (int i = 3; i < 6; i++) C[i][i] = mu;
    double B[6][24] = {{0}};
    for (int a = 0; a < 8; a++) {
        B[0][3 * a] = dNdX[a][0], B[1][3 * a + 1] = dNdX[a][1], B[2][3 * a + 2] = dNdX[a][2];
        B[3][3 * a] = dNdX[a][1], B[3][3 * a + 1] = dNdX[a][0];
        B[4][3 * a + 1] = dNdX[a][2], B[4][3 * a + 2] = dNdX[a][1];
        B[5][3 * a] = dNdX[a][2], B[5][3 * a + 2] = dNdX[a][0];
    }
    for (int i = 0; i < 24; i++)
        for (int j = 0; j < 24; j++) {
            double v = 0;
            for (int k = 0; k < 6; k++)
                for (int l = 0; l < 6; l++) v += B[k][i] * C[k][l] * B[l][j];
            K[i][j] = v * vol;
        }
    double gsum = 0;
    for (int a = 0; a < 8; a++)
        for (int i = 0; i < 3; i++) gsum += dNdX[a][i] * dNdX[a][i];
    double kh = hg * mu * vol * gsum;
    for (int m = 0; m < 4; m++)
        for (int a = 0; a < 8; a++)
            for (int b2 = 0; b2 < 8; b2++)
                for (int i = 0; i < 3; i++) K[3 * a + i][3 * b2 + i] += kh * gam[m][a] * gam[m][b2];
    double mnode = rho * vol / 8, x[24], y[24], lambda_max = 0;
    for (int i = 0; i < 24; i++) x[i] = (i % 3 == 0) ? 1.0 : (i % 3 == 1 ? -0.7 : 0.4);
    for (int it = 0; it < 60; it++) {
        double n2 = 0;
        for (int i = 0; i < 24; i++) {
            double v = 0;
            for (int j = 0; j < 24; j++) v += K[i][j] * x[j];
            y[i] = v / mnode;
        }
        for (int i = 0; i < 24; i++) n2 += y[i] * y[i];
        double n = sqrt(n2);
        if (!(n > 0)) break;
        double ray = 0;
        for (int i = 0; i < 24; i++) ray += x[i] * y[i];
        lambda_max = fmax(lambda_max, ray);
        for (int i = 0; i < 24; i++) x[i] = y[i] / n;
    }
    return lambda_max > 0 ? sqrt(lambda_max) : 0;
}

static void elem_coords(const ExDyn *d, int e, double X[8][3]) {
    for (int a = 0; a < 8; a++)
        for (int k = 0; k < 3; k++) X[a][k] = d->mesh.xyz[3 * (size_t)d->mesh.conn[8 * (size_t)e + (size_t)a] + (size_t)k];
}

/* the shape-function gradients and the volume at the element centre, and the hourglass vectors that go with them */
static bool centre_gradients(const double X[8][3], double dNdX[8][3], double *vol) {
    double N[8], dN[8][3], J[3][3];
    hex8_shape(0, 0, 0, N, dN);
    double dj = hex8_jacobian(X, dN, J, dNdX);
    if (!(dj > 0)) return false;
    *vol = 8 * dj; /* one point, weight 8 */
    return true;
}

static void hourglass_vectors(const double X[8][3], const double dNdX[8][3], double gamma[4][8]) {
    for (int m = 0; m < 4; m++)
        for (int a = 0; a < 8; a++) {
            double g = HG[m][a];
            for (int i = 0; i < 3; i++) {
                double xg = 0;
                for (int b = 0; b < 8; b++) xg += HG[m][b] * X[b][i];
                g -= dNdX[a][i] * xg;
            }
            gamma[m][a] = g;
        }
}

/* The characteristic length of a hex for the stable step: its volume over its largest face, which is the distance a
 * wave has to cross between the two faces that are furthest apart in the flattest direction. For a cube it is the
 * edge; for a distorted element it is much smaller than the shortest edge, and taking the edge there (as this did
 * first) gives a step that looks safe and is not: a mapped sphere blew up in twelve steps. */
static double characteristic_length(const double X[8][3]) {
    static const int F[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
    double ctr[3] = {0, 0, 0}, vol = 0, amax = 0;
    for (int a = 0; a < 8; a++)
        for (int k = 0; k < 3; k++) ctr[k] += X[a][k] / 8;
    for (int f = 0; f < 6; f++) {
        double d1[3], d2[3], nr[3], fc[3] = {0, 0, 0}, dd[3];
        for (int a = 0; a < 4; a++)
            for (int k = 0; k < 3; k++) fc[k] += X[F[f][a]][k] / 4;
        for (int k = 0; k < 3; k++) d1[k] = X[F[f][2]][k] - X[F[f][0]][k], d2[k] = X[F[f][3]][k] - X[F[f][1]][k];
        nr[0] = d1[1] * d2[2] - d1[2] * d2[1], nr[1] = d1[2] * d2[0] - d1[0] * d2[2], nr[2] = d1[0] * d2[1] - d1[1] * d2[0];
        double len = sqrt(nr[0] * nr[0] + nr[1] * nr[1] + nr[2] * nr[2]), area = 0.5 * len;
        amax = fmax(amax, area);
        for (int k = 0; k < 3; k++) dd[k] = fc[k] - ctr[k];
        vol += fabs(dd[0] * nr[0] + dd[1] * nr[1] + dd[2] * nr[2]) / 6;
    }
    return amax > 0 ? vol / amax : 0;
}

bool ex_set_contact(ExDyn *d, const ExPlane *planes, int nplanes, const ExContactOptions *opt, char *err, size_t errlen) {
    size_t nn = (size_t)d->mesh.nnodes;
    excontact_free(d->contact);
    d->contact = excontact_create(d->mesh.nnodes, d->mesh.nelems, d->mesh.xyz, d->mesh.conn, d->mass, bulk_modulus(&d->mat.mech), planes,
                                  nplanes, opt, err, errlen);
    if (!d->contact) return false;
    if (!d->f_con) d->f_con = calloc(3 * nn, sizeof(double));
    if (!d->x_now) d->x_now = malloc(3 * nn * sizeof(double));
    if (!d->f_con || !d->x_now) {
        snprintf(err, errlen, "out of memory for the contact forces");
        return false;
    }
    double w = excontact_max_frequency(d->contact); /* the penalty's own stability limit */
    if (w > 0) {
        double dt_c = d->opt.safety * 2.0 / w;
        if (dt_c < d->dt) d->dt = dt_c, d->contact_set_step = true;
    }
    return true;
}

const ExContact *ex_contact(const ExDyn *d) { return d->contact; }
bool ex_set_contact_plane(ExDyn *d, int index, const ExPlane *plane) { return excontact_set_plane(d->contact, index, plane); }
bool ex_contact_cut_step(const ExDyn *d) { return d->contact_set_step; }

void ex_free(ExDyn *d) {
    if (!d) return;
    excontact_free(d->contact);
    free(d->f_con), free(d->x_now);
    free(d->fixed), free(d->fixed_v), free(d->u), free(d->v), free(d->a), free(d->f_ext), free(d->f_int), free(d->mass);
    free(d->stress), free(d->state), free(d->gamma);
    free(d);
}

ExDyn *ex_create(const ExMesh *mesh, const ExMaterial *mat, const unsigned char *fixed, const double *fixed_velocity, const ExOptions *opt,
                 char *err, size_t errlen) {
    ExDyn *d = calloc(1, sizeof *d);
    if (!d) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    d->mesh = *mesh, d->mat = *mat;
    d->opt = opt ? *opt : (ExOptions){0};
    if (!(d->opt.safety > 0)) d->opt.safety = 0.9;
    if (!(d->opt.hourglass > 0)) d->opt.hourglass = 0.05;
    size_t nn = (size_t)mesh->nnodes, ne = (size_t)mesh->nelems;
    d->fixed = calloc(3 * nn, 1), d->fixed_v = calloc(3 * nn, sizeof(double));
    d->u = calloc(3 * nn, sizeof(double)), d->v = calloc(3 * nn, sizeof(double)), d->a = calloc(3 * nn, sizeof(double));
    d->f_ext = calloc(3 * nn, sizeof(double)), d->f_int = calloc(3 * nn, sizeof(double)), d->mass = calloc(nn, sizeof(double));
    d->stress = calloc(6 * ne, sizeof(double)), d->state = calloc(ne ? ne : 1, sizeof *d->state);
    d->gamma = calloc(32 * (ne ? ne : 1), sizeof(double));
    if (!d->fixed || !d->fixed_v || !d->u || !d->v || !d->a || !d->f_ext || !d->f_int || !d->mass || !d->stress || !d->state || !d->gamma) {
        ex_free(d);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    if (fixed) memcpy(d->fixed, fixed, 3 * nn);
    if (fixed_velocity) memcpy(d->fixed_v, fixed_velocity, 3 * nn * sizeof(double));
    /* masses, hourglass vectors and the stable step */
    double lam = mat->mech.E * mat->mech.nu / ((1 + mat->mech.nu) * (1 - 2 * mat->mech.nu));
    double mu = mat->mech.E / (2 * (1 + mat->mech.nu));
    double c = sqrt((lam + 2 * mu) / mat->density);
    double dt_min = INFINITY, real_mass = 0, added = 0;
    for (int e = 0; e < mesh->nelems; e++) {
        double X[8][3], dNdX[8][3], vol, gam[4][8];
        elem_coords(d, e, X);
        if (!centre_gradients(X, dNdX, &vol)) {
            ex_free(d);
            snprintf(err, errlen, "element %d is degenerate", e);
            return NULL;
        }
        hourglass_vectors(X, dNdX, gam);
        for (int m = 0; m < 4; m++)
            for (int a = 0; a < 8; a++) d->gamma[32 * (size_t)e + 8 * m + a] = gam[m][a];
        double m_e = mat->density * vol, scale = 1.0;
        double w = element_max_frequency(dNdX, vol, gam, lam, mu, mat->density, d->opt.hourglass);
        double dt_len = characteristic_length(X) / c;
        double dt_e = d->opt.safety * (w > 0 ? fmin(2.0 / w, dt_len) : dt_len);
        if (d->opt.mass_scale_dt > 0 && dt_e < d->opt.mass_scale_dt) {
            scale = (d->opt.mass_scale_dt / dt_e) * (d->opt.mass_scale_dt / dt_e); /* dt goes with the square root of the mass */
            dt_e = d->opt.mass_scale_dt;
        }
        if (dt_e < dt_min) dt_min = dt_e, d->critical_elem = e;
        real_mass += m_e, added += m_e * (scale - 1);
        for (int a = 0; a < 8; a++) d->mass[d->mesh.conn[8 * (size_t)e + (size_t)a]] += scale * m_e / 8;
    }
    d->dt = dt_min;
    d->mass_added = real_mass > 0 ? added / real_mass : 0;
    if (!(d->dt > 0)) {
        ex_free(d);
        snprintf(err, errlen, "the stable time step came out as %g", d->dt);
        return NULL;
    }
    for (size_t i = 0; i < nn; i++)
        if (!(d->mass[i] > 0)) d->mass[i] = 1e-300; /* a node no element uses never moves */
    return d;
}

double ex_dt(const ExDyn *d) { return d->dt; }
int ex_critical_element(const ExDyn *d) { return d->critical_elem; }
double ex_mass_added(const ExDyn *d) { return d->mass_added; }
double ex_time(const ExDyn *d) { return d->time; }
int ex_steps(const ExDyn *d) { return d->steps; }
const double *ex_u(const ExDyn *d) { return d->u; }
const double *ex_v(const ExDyn *d) { return d->v; }
const double *ex_stress(const ExDyn *d) { return d->stress; }
const double *ex_mass(const ExDyn *d) { return d->mass; }

void ex_set_velocity(ExDyn *d, const double *v) { memcpy(d->v, v, 3 * (size_t)d->mesh.nnodes * sizeof(double)); }
void ex_set_displacement(ExDyn *d, const double *u) { memcpy(d->u, u, 3 * (size_t)d->mesh.nnodes * sizeof(double)); }
void ex_set_force(ExDyn *d, const double *f) { memcpy(d->f_ext, f, 3 * (size_t)d->mesh.nnodes * sizeof(double)); }
void ex_set_damping(ExDyn *d, double damping) { d->opt.damping = damping; }

/* internal and hourglass forces at the current displacement; returns false when an element inverts */
/* the contact forces at the current state, into d->f_con */
static void contact_forces(ExDyn *d, double dt) {
    if (!d->contact) return;
    size_t nn = (size_t)d->mesh.nnodes;
    memset(d->f_con, 0, 3 * nn * sizeof(double));
    for (size_t i = 0; i < 3 * nn; i++) d->x_now[i] = d->mesh.xyz[i] + d->u[i];
    excontact_forces(d->contact, d->x_now, d->v, dt, d->steps, d->f_con);
}

static bool forces(ExDyn *d, double *f_hg_work_rate) {
    size_t nn = (size_t)d->mesh.nnodes;
    d->e_stored = 0;
    memset(d->f_int, 0, 3 * nn * sizeof(double));
    double mu = d->mat.mech.E / (2 * (1 + d->mat.mech.nu));
    bool ok = true;
    for (int e = 0; e < d->mesh.nelems; e++) {
        double X[8][3], ue[24], fe[24] = {0}, dNdX[8][3], vol;
        elem_coords(d, e, X);
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) ue[3 * a + k] = d->u[3 * (size_t)d->mesh.conn[8 * (size_t)e + (size_t)a] + (size_t)k];
        if (!centre_gradients(X, dNdX, &vol)) {
            ok = false;
            continue;
        }
        /* one-point evaluation of the large-deformation element */
        double S[6];
        if (!hex8_nl_one_point(X, ue, &d->mat.mech, &d->state[e], true, fe, S, NULL)) {
            ok = false;
            continue;
        }
        memcpy(d->stress + 6 * (size_t)e, S, 6 * sizeof(double));
        { /* the stored energy of this element: 1/2 S : E at its centre (exact for St. Venant-Kirchhoff) */
            double F[9], dF;
            if (hex8_nl_centre_gradient(X, ue, F, &dF)) {
                double Cg[9] = {0}, Ev[6];
                for (int i = 0; i < 3; i++)
                    for (int j = 0; j < 3; j++)
                        for (int k = 0; k < 3; k++) Cg[3 * i + j] += F[3 * k + i] * F[3 * k + j];
                Ev[0] = 0.5 * (Cg[0] - 1), Ev[1] = 0.5 * (Cg[4] - 1), Ev[2] = 0.5 * (Cg[8] - 1);
                Ev[3] = Cg[1], Ev[4] = Cg[5], Ev[5] = Cg[2];
                double w = 0;
                for (int k = 0; k < 6; k++) w += S[k] * Ev[k] * (k < 3 ? 1.0 : 0.5);
                d->e_stored += 0.5 * w * vol;
            }
        }
        /* hourglass: a stiffness against each mode, orthogonal to the linear field */
        double gsum = 0;
        for (int a = 0; a < 8; a++)
            for (int i = 0; i < 3; i++) gsum += dNdX[a][i] * dNdX[a][i];
        double kh = d->opt.hourglass * mu * vol * gsum;
        for (int m = 0; m < 4; m++) {
            const double *gam = d->gamma + 32 * (size_t)e + 8 * m;
            double q[3] = {0, 0, 0};
            for (int a = 0; a < 8; a++)
                for (int i = 0; i < 3; i++) q[i] += gam[a] * ue[3 * a + i];
            for (int a = 0; a < 8; a++)
                for (int i = 0; i < 3; i++) fe[3 * a + i] += kh * gam[a] * q[i];
        }
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 3; k++) d->f_int[3 * (size_t)d->mesh.conn[8 * (size_t)e + (size_t)a] + (size_t)k] += fe[3 * a + k];
        if (f_hg_work_rate) { /* the hourglass part alone, for the energy account */
            double hgf = 0;
            for (int m = 0; m < 4; m++) {
                const double *gam = d->gamma + 32 * (size_t)e + 8 * m;
                double q[3] = {0, 0, 0};
                for (int a = 0; a < 8; a++)
                    for (int i = 0; i < 3; i++) q[i] += gam[a] * ue[3 * a + i];
                for (int i = 0; i < 3; i++) hgf += 0.5 * kh * q[i] * q[i];
            }
            *f_hg_work_rate += hgf;
        }
    }
    return ok;
}

bool ex_advance(ExDyn *d, double t_end, char *err, size_t errlen) {
    size_t nn = (size_t)d->mesh.nnodes;
    if (!d->started) { /* the half-step start: v(-dt/2) = v(0) - a(0) dt / 2 */
        double hg = 0;
        if (!forces(d, &hg)) {
            snprintf(err, errlen, "an element is inverted at the start");
            return false;
        }
        d->e_hourglass = hg;
        contact_forces(d, d->dt);
        for (size_t i = 0; i < 3 * nn; i++) {
            double m = d->mass[i / 3];
            d->a[i] = d->fixed[i] ? 0 : (d->f_ext[i] + (d->f_con ? d->f_con[i] : 0) - d->f_int[i]) / m - d->opt.damping * d->v[i];
            if (d->fixed[i]) d->v[i] = d->fixed_v[i];
        }
        double e_k = 0;
        for (size_t i = 0; i < 3 * nn; i++) e_k += 0.5 * d->mass[i / 3] * d->v[i] * d->v[i];
        d->e_start = e_k + d->e_hourglass + d->e_stored + (d->contact ? excontact_energy(d->contact) : 0); /* the released shape already stores strain energy */
        for (size_t i = 0; i < 3 * nn; i++)
            if (!d->fixed[i]) d->v[i] -= 0.5 * d->dt * d->a[i];
        d->started = true;
    }
    while (d->time < t_end - 1e-15) {
        double dt = fmin(d->dt, t_end - d->time);
        /* v at the half step, then the displacement */
        for (size_t i = 0; i < 3 * nn; i++) {
            if (d->fixed[i]) d->v[i] = d->fixed_v[i];
            else d->v[i] += dt * d->a[i];
        }
        double work_int = 0, work_ext = 0;
        for (size_t i = 0; i < 3 * nn; i++) {
            double du = dt * d->v[i];
            work_int += d->f_int[i] * du;
            work_ext += d->f_ext[i] * du;
            d->u[i] += du;
        }
        d->e_internal += work_int; /* the work of the internal forces, kept for the record */
        d->e_external += work_ext;
        double hg = 0;
        if (!forces(d, &hg)) {
            snprintf(err, errlen, "an element inverted at t = %g s (step %d)", d->time, d->steps);
            return false;
        }
        d->e_hourglass = hg;
        contact_forces(d, dt);
        for (size_t i = 0; i < 3 * nn; i++) {
            double m = d->mass[i / 3];
            d->a[i] = d->fixed[i] ? 0 : (d->f_ext[i] + (d->f_con ? d->f_con[i] : 0) - d->f_int[i]) / m - d->opt.damping * d->v[i];
        }
        d->time += dt, d->steps++;
    }
    return true;
}

void ex_energy(const ExDyn *d, ExEnergy *e) {
    size_t nn = (size_t)d->mesh.nnodes;
    memset(e, 0, sizeof *e);
    for (size_t i = 0; i < 3 * nn; i++) e->kinetic += 0.5 * d->mass[i / 3] * d->v[i] * d->v[i];
    e->internal = d->e_stored, e->hourglass = d->e_hourglass, e->external = d->e_external;
    if (d->contact) {
        e->contact = excontact_energy(d->contact);
        e->friction = excontact_friction_work(d->contact);
        e->contact_damping = excontact_damping_work(d->contact);
        e->external += excontact_plate_work(d->contact); /* what a moving plane fed in */
    }
    e->total = e->kinetic + e->internal + e->hourglass + e->contact;
    double scale = fmax(fabs(e->total), fmax(fabs(e->external), fabs(d->e_start)));
    e->drift = scale > 0 ? (e->total + e->friction + e->contact_damping - e->external - d->e_start) / scale : 0;
}
