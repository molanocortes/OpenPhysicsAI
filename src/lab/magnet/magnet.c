/* magnet.c - see magnet.h. */
#include "magnet.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MU0 (4e-7 * M_PI)

struct Magnet {
    MgSpec s;
    int nx, ny;
    size_t n;
    unsigned char *mat;
    double *J, *brx, *bry;  /* per cell */
    double *A, *b, *r, *z, *p, *q, *ce, *cn, *cd, *L; /* solution, right side, CG work, east/north couplings, diagonal, IC(0) */
    double angle;
};

void mg_spec_defaults(MgSpec *s) {
    memset(s, 0, sizeof *s);
    s->nx = s->ny = 128, s->dx = 1e-3;
    s->nmaterials = 1;
    snprintf(s->materials[0].name, sizeof s->materials[0].name, "air");
    s->materials[0].mu_r = 1;
    s->tol = 1e-9;
}

static inline size_t at(const Magnet *m, int i, int j) { return (size_t)j * m->nx + i; }
static inline double nu_of(const Magnet *m, size_t c) { return 1.0 / (MU0 * m->s.materials[m->mat[c]].mu_r); }

static bool inside(const MgRegion *R, double x, double y) {
    const double *a = R->a;
    if (R->shape == MG_BOX) return x >= a[0] && x < a[2] && y >= a[1] && y < a[3];
    double dx = x - a[0], dy = y - a[1], r = hypot(dx, dy);
    if (R->shape == MG_CIRCLE) return r < a[2];
    if (r < a[2] || r >= a[3]) return false;
    double th = atan2(dy, dx) * 180 / M_PI, a0 = a[4], a1 = a[5];
    if (a1 - a0 >= 360.0) return true; /* a whole ring (the modulo below would make its span zero) */
    double d = fmod(th - a0 + 720.0, 360.0), w = fmod(a1 - a0 + 720.0, 360.0);
    return d < w;
}

static void paint(Magnet *m) {
    const MgSpec *s = &m->s;
    memset(m->mat, 0, m->n);
    memset(m->J, 0, m->n * sizeof(double)), memset(m->brx, 0, m->n * sizeof(double)), memset(m->bry, 0, m->n * sizeof(double));
    double th = m->angle * M_PI / 180, ct = cos(th), st = sin(th), cx = s->rotor_centre[0], cy = s->rotor_centre[1];
    for (int k = 0; k < s->nregions; k++) {
        const MgRegion *R = &s->regions[k];
        for (int j = 0; j < m->ny; j++)
            for (int i = 0; i < m->nx; i++) {
                double x = (i + 0.5) * s->dx, y = (j + 0.5) * s->dx, xr = x, yr = y;
                if (R->rotor) { /* the point in the rotor's own frame */
                    double u = x - cx, v = y - cy;
                    xr = cx + ct * u + st * v, yr = cy - st * u + ct * v;
                }
                if (!inside(R, xr, yr)) continue;
                size_t c = at(m, i, j);
                m->mat[c] = (unsigned char)R->material;
                m->J[c] = R->J;
                double Br = s->materials[R->material].Br, dx_ = 0, dy_ = 0;
                if (R->magnetisation == MG_MAG_PARALLEL) {
                    double a = R->mag_angle_deg * M_PI / 180 + (R->rotor ? th : 0);
                    dx_ = cos(a), dy_ = sin(a);
                } else if (R->magnetisation == MG_MAG_RADIAL) {
                    double u = xr - R->a[0], v = yr - R->a[1], rr = hypot(u, v), sg = R->mag_angle_deg >= 0 ? 1 : -1;
                    if (rr > 0) {
                        u /= rr, v /= rr;
                        dx_ = sg * (R->rotor ? ct * u - st * v : u), dy_ = sg * (R->rotor ? st * u + ct * v : v);
                    }
                }
                m->brx[c] = Br * dx_, m->bry[c] = Br * dy_;
            }
    }
}

Magnet *mg_create(const MgSpec *s, char *err, size_t errlen) {
    if (s->nx < 4 || s->ny < 4 || !(s->dx > 0)) {
        snprintf(err, errlen, "magnet: nx, ny >= 4 and dx > 0 are required");
        return NULL;
    }
    for (int k = 0; k < s->nmaterials; k++)
        if (!(s->materials[k].mu_r > 0)) {
            snprintf(err, errlen, "magnet: material %d needs mu_r > 0", k);
            return NULL;
        }
    Magnet *m = calloc(1, sizeof *m);
    m->s = *s;
    if (!(m->s.tol > 0)) m->s.tol = 1e-9;
    m->nx = s->nx, m->ny = s->ny, m->n = (size_t)s->nx * s->ny;
    size_t n = m->n;
    m->mat = calloc(n, 1);
    double **arr[] = {&m->J, &m->brx, &m->bry, &m->A, &m->b, &m->r, &m->z, &m->p, &m->q, &m->ce, &m->cn, &m->cd, &m->L};
    for (size_t k = 0; k < sizeof arr / sizeof arr[0]; k++) *arr[k] = calloc(n, sizeof(double));
    m->angle = s->rotor_angle_deg;
    paint(m);
    return m;
}

void mg_free(Magnet *m) {
    if (!m) return;
    free(m->mat), free(m->J), free(m->brx), free(m->bry), free(m->A), free(m->b), free(m->r), free(m->z), free(m->p), free(m->q);
    free(m->ce), free(m->cn), free(m->cd), free(m->L);
    free(m);
}

void mg_set_region_current(Magnet *m, int k, double J) {
    if (k >= 0 && k < m->s.nregions) m->s.regions[k].J = J;
}

void mg_set_rotor_angle(Magnet *m, double deg) {
    m->angle = deg;
    paint(m);
}

/* the boundary potential of the applied uniform field: B = (dA/dy, -dA/dx) */
static double A_bnd(const Magnet *m, double x, double y) { return m->s.applied_B[0] * y - m->s.applied_B[1] * x; }

static void assemble(Magnet *m) {
    const double dx = m->s.dx;
    for (int j = 0; j < m->ny; j++)
        for (int i = 0; i < m->nx; i++) {
            size_t c = at(m, i, j);
            double nc = nu_of(m, c), d = 0, rhs = m->J[c] * dx * dx;
            /* east and north couplings (the west and south ones are the neighbours' east and north) */
            m->ce[c] = i + 1 < m->nx ? 2 * nc * nu_of(m, c + 1) / (nc + nu_of(m, c + 1)) : 0;
            m->cn[c] = j + 1 < m->ny ? 2 * nc * nu_of(m, c + m->nx) / (nc + nu_of(m, c + m->nx)) : 0;
            double x = (i + 0.5) * dx, y = (j + 0.5) * dx;
            /* faces: east, west, north, south; boundary faces at half a cell with the boundary potential */
            if (i + 1 < m->nx) d += m->ce[c];
            else d += 2 * nc, rhs += 2 * nc * A_bnd(m, (i + 1) * dx, y);
            if (i > 0) d += m->ce[c - 1];
            else d += 2 * nc, rhs += 2 * nc * A_bnd(m, 0, y);
            if (j + 1 < m->ny) d += m->cn[c];
            else d += 2 * nc, rhs += 2 * nc * A_bnd(m, x, (j + 1) * dx);
            if (j > 0) d += m->cn[c - m->nx];
            else d += 2 * nc, rhs += 2 * nc * A_bnd(m, x, 0);
            /* remanence: the flux through a face is nu_f [(A_nb - A_c) / dx + P.n], P = (Br_y, -Br_x), with nu_f the same harmonic
             * mean as the potential's and P the mean of the two cells: the exact series flux of two half cells. (The first
             * version averaged nu Br itself; at the stepped edge of a radially magnetised magnet on iron that left
             * alternating spurious currents weighted by the magnet's reluctivity, which the iron amplified to 6 T.) */
            double by_e = i + 1 < m->nx ? m->ce[c] * 0.5 * (m->bry[c] + m->bry[c + 1]) : nc * m->bry[c];
            double by_w = i > 0 ? m->ce[c - 1] * 0.5 * (m->bry[c] + m->bry[c - 1]) : nc * m->bry[c];
            double bx_n = j + 1 < m->ny ? m->cn[c] * 0.5 * (m->brx[c] + m->brx[c + m->nx]) : nc * m->brx[c];
            double bx_s = j > 0 ? m->cn[c - m->nx] * 0.5 * (m->brx[c] + m->brx[c - m->nx]) : nc * m->brx[c];
            rhs += dx * (by_e - by_w - bx_n + bx_s);
            m->cd[c] = d, m->b[c] = rhs;
        }
    /* incomplete Cholesky, no fill: L_kk^2 = d_k - (c_w / L_w)^2 - (c_s / L_s)^2 */
    for (int j = 0; j < m->ny; j++)
        for (int i = 0; i < m->nx; i++) {
            size_t c = at(m, i, j);
            double v = m->cd[c];
            if (i > 0) v -= m->ce[c - 1] * m->ce[c - 1] / (m->L[c - 1] * m->L[c - 1]);
            if (j > 0) v -= m->cn[c - m->nx] * m->cn[c - m->nx] / (m->L[c - m->nx] * m->L[c - m->nx]);
            m->L[c] = sqrt(v > 1e-12 * m->cd[c] ? v : m->cd[c]);
        }
}

static void matvec(const Magnet *m, const double *x, double *y) {
    for (int j = 0; j < m->ny; j++)
        for (int i = 0; i < m->nx; i++) {
            size_t c = at(m, i, j);
            double v = m->cd[c] * x[c];
            if (i + 1 < m->nx) v -= m->ce[c] * x[c + 1];
            if (i > 0) v -= m->ce[c - 1] * x[c - 1];
            if (j + 1 < m->ny) v -= m->cn[c] * x[c + m->nx];
            if (j > 0) v -= m->cn[c - m->nx] * x[c - m->nx];
            y[c] = v;
        }
}

/* z = (L L^T)^-1 r */
static void precondition(const Magnet *m, const double *r, double *z) {
    for (int j = 0; j < m->ny; j++) /* forward: L y = r */
        for (int i = 0; i < m->nx; i++) {
            size_t c = at(m, i, j);
            double v = r[c];
            if (i > 0) v += m->ce[c - 1] / m->L[c - 1] * z[c - 1];
            if (j > 0) v += m->cn[c - m->nx] / m->L[c - m->nx] * z[c - m->nx];
            z[c] = v / m->L[c];
        }
    for (int j = m->ny - 1; j >= 0; j--) /* backward: L^T z = y */
        for (int i = m->nx - 1; i >= 0; i--) {
            size_t c = at(m, i, j);
            double v = z[c];
            if (i + 1 < m->nx) v += m->ce[c] / m->L[c] * z[c + 1];
            if (j + 1 < m->ny) v += m->cn[c] / m->L[c] * z[c + m->nx];
            z[c] = v / m->L[c];
        }
}

static double dot(const double *a, const double *b, size_t n) {
    double s = 0;
    for (size_t k = 0; k < n; k++) s += a[k] * b[k];
    return s;
}

int mg_solve(Magnet *m) {
    assemble(m);
    size_t n = m->n;
    matvec(m, m->A, m->q);
    for (size_t k = 0; k < n; k++) m->r[k] = m->b[k] - m->q[k];
    double bn = sqrt(dot(m->b, m->b, n));
    if (bn == 0) bn = 1;
    precondition(m, m->r, m->z);
    memcpy(m->p, m->z, n * sizeof(double));
    double rz = dot(m->r, m->z, n);
    for (int it = 1; it <= 20000; it++) {
        matvec(m, m->p, m->q);
        double al = rz / dot(m->p, m->q, n);
        for (size_t k = 0; k < n; k++) m->A[k] += al * m->p[k], m->r[k] -= al * m->q[k];
        if (sqrt(dot(m->r, m->r, n)) < m->s.tol * bn) return it;
        precondition(m, m->r, m->z);
        double rz1 = dot(m->r, m->z, n), be = rz1 / rz;
        rz = rz1;
        for (size_t k = 0; k < n; k++) m->p[k] = m->z[k] + be * m->p[k];
    }
    return -1;
}

const double *mg_potential(const Magnet *m) { return m->A; }
int mg_material_at(const Magnet *m, int i, int j) { return m->mat[at(m, i, j)]; }

/* B at a cell centre, central differences of A (one-sided at the boundary) */
static void cell_B(const Magnet *m, int i, int j, double B[2]) {
    const double dx = m->s.dx;
    size_t c = at(m, i, j);
    double Ae = i + 1 < m->nx ? m->A[c + 1] : 2 * A_bnd(m, (i + 1) * dx, (j + 0.5) * dx) - m->A[c];
    double Aw = i > 0 ? m->A[c - 1] : 2 * A_bnd(m, 0, (j + 0.5) * dx) - m->A[c];
    double An = j + 1 < m->ny ? m->A[c + m->nx] : 2 * A_bnd(m, (i + 0.5) * dx, (j + 1) * dx) - m->A[c];
    double As = j > 0 ? m->A[c - m->nx] : 2 * A_bnd(m, (i + 0.5) * dx, 0) - m->A[c];
    B[0] = (An - As) / (2 * dx), B[1] = -(Ae - Aw) / (2 * dx);
}

void mg_field(const Magnet *m, double x, double y, double B[2]) {
    double a = x / m->s.dx - 0.5, b = y / m->s.dx - 0.5;
    int i = (int)floor(a), j = (int)floor(b);
    if (i < 0) i = 0;
    if (j < 0) j = 0;
    if (i > m->nx - 2) i = m->nx - 2;
    if (j > m->ny - 2) j = m->ny - 2;
    double tx = a - i, ty = b - j, b00[2], b10[2], b01[2], b11[2];
    cell_B(m, i, j, b00), cell_B(m, i + 1, j, b10), cell_B(m, i, j + 1, b01), cell_B(m, i + 1, j + 1, b11);
    for (int k = 0; k < 2; k++) B[k] = (1 - ty) * ((1 - tx) * b00[k] + tx * b10[k]) + ty * ((1 - tx) * b01[k] + tx * b11[k]);
}

double mg_torque(const Magnet *m) {
    double r = m->s.gap_radius, cx = m->s.rotor_centre[0], cy = m->s.rotor_centre[1];
    if (!(r > 0)) return 0;
    const int N = 1440;
    double t = 0;
    for (int k = 0; k < N; k++) {
        double th = 2 * M_PI * (k + 0.5) / N, ct = cos(th), st = sin(th), B[2];
        mg_field(m, cx + r * ct, cy + r * st, B);
        double Br = B[0] * ct + B[1] * st, Bt = -B[0] * st + B[1] * ct;
        t += r * r * Br * Bt;
    }
    return t * (2 * M_PI / N) / MU0;
}

double mg_copper_loss(const Magnet *m) {
    double P = 0, a = m->s.dx * m->s.dx;
    for (size_t c = 0; c < m->n; c++) {
        double sg = m->s.materials[m->mat[c]].conductivity;
        if (sg > 0 && m->J[c] != 0) P += m->J[c] * m->J[c] / sg * a;
    }
    return P;
}

void mg_write_frame(Magnet *m, LabWriter *w) {
    LabBlock blk = {{m->nx, m->ny, 1}, 0, LAB_PLANE_XY, {0, 0, 0}, {m->s.dx, m->s.dx, m->s.dx}};
    size_t n = m->n;
    float *B = malloc(n * sizeof(float)), *bx = malloc(n * sizeof(float)), *by = malloc(n * sizeof(float)), *A = malloc(n * sizeof(float)),
          *fl = malloc(n * sizeof(float)), *mt = malloc(n * sizeof(float)), *J = malloc(n * sizeof(float));
    double amin = INFINITY, amax = -INFINITY;
    for (size_t c = 0; c < n; c++) amin = fmin(amin, m->A[c]), amax = fmax(amax, m->A[c]);
    double step = (amax - amin) / 40; /* forty flux lines across the range of A */
    for (int j = 0; j < m->ny; j++)
        for (int i = 0; i < m->nx; i++) {
            size_t c = at(m, i, j);
            double b[2];
            cell_B(m, i, j, b);
            B[c] = (float)hypot(b[0], b[1]), bx[c] = (float)b[0], by[c] = (float)b[1], A[c] = (float)m->A[c];
            double f = step > 0 ? (m->A[c] - amin) / step : 0, d = fabs(f - floor(f + 0.5));
            fl[c] = (float)(d < 0.12 ? 1.0 - d / 0.12 : 0.0); /* thin bands where A crosses a multiple of the step */
            mt[c] = (float)m->mat[c], J[c] = (float)m->J[c];
        }
    lab_part_blocks(w, "magnet", 1, &blk);
    lab_field(w, "B", LAB_AT_CELL, n, B);
    lab_field(w, "Bx", LAB_AT_CELL, n, bx);
    lab_field(w, "By", LAB_AT_CELL, n, by);
    lab_field(w, "A", LAB_AT_CELL, n, A);
    lab_field(w, "flux_lines", LAB_AT_CELL, n, fl);
    lab_field(w, "material", LAB_AT_CELL, n, mt);
    lab_field(w, "J", LAB_AT_CELL, n, J);
    free(B), free(bx), free(by), free(A), free(fl), free(mt), free(J);
}

char *mg_header_json(const MgSpec *s, const char *title) {
    char *o = malloc(1024);
    snprintf(o, 1024,
             "{\"domain\":\"magnet\",\"title\":\"%s\",\"solver\":\"src/lab/magnet: 2D magnetostatics for the axial vector potential, finite "
             "volumes, harmonic reluctivity at faces, remanent magnets, IC(0) conjugate gradients, Maxwell-stress torque\",\"nx\":%d,\"ny\":%d,"
             "\"dx_m\":%.6g,\"fields\":{\"B\":\"T\",\"Bx\":\"T\",\"By\":\"T\",\"A\":\"Wb/m\",\"flux_lines\":\"1\",\"material\":\"1\",\"J\":\"A/m^2\"}}",
             title, s->nx, s->ny, s->dx);
    return o;
}
