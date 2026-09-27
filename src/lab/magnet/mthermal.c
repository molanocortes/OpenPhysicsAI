/* mthermal.c - implicit heat conduction on the magnet grid (mthermal.h). */
#include "mthermal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct MThermal {
    int nx, ny;
    double dx;
    double *T, *C;       /* temperature; heat capacity per metre of length, rho cp dx^2 */
    double *ge, *gn;     /* face conductances to the east and north neighbour, W/(m K) per metre of length */
    unsigned char *held;
    double *r, *z, *p, *Ap, *b, *diag;
    double held_heat;
};

static inline double harmonic(double a, double b) { return a + b > 0 ? 2 * a * b / (a + b) : 0; }

MThermal *mt_create(int nx, int ny, double dx, const double *k, const double *rho_cp, const unsigned char *held, double T_init,
                    int threads) {
    (void)threads;
    if (nx < 2 || ny < 2 || !(dx > 0)) return NULL;
    size_t n = (size_t)nx * ny;
    MThermal *t = calloc(1, sizeof *t);
    t->nx = nx, t->ny = ny, t->dx = dx;
    double **arr[] = {&t->T, &t->C, &t->ge, &t->gn, &t->r, &t->z, &t->p, &t->Ap, &t->b, &t->diag};
    for (size_t a = 0; a < sizeof arr / sizeof arr[0]; a++) *arr[a] = calloc(n, sizeof(double));
    t->held = malloc(n);
    memcpy(t->held, held, n);
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++) {
            size_t c = (size_t)j * nx + i;
            t->T[c] = T_init, t->C[c] = rho_cp[c] * dx * dx;
            /* a face of length dx between centres dx apart: conductance k per metre of length */
            t->ge[c] = i + 1 < nx ? harmonic(k[c], k[c + 1]) : 0;
            t->gn[c] = j + 1 < ny ? harmonic(k[c], k[c + nx]) : 0;
        }
    return t;
}

void mt_free(MThermal *t) {
    if (!t) return;
    free(t->T), free(t->C), free(t->ge), free(t->gn), free(t->r), free(t->z), free(t->p), free(t->Ap), free(t->b), free(t->diag);
    free(t->held);
    free(t);
}

double *mt_temperature(MThermal *t) { return t->T; }
double mt_held_heat(const MThermal *t) { return t->held_heat; }

double mt_stored(const MThermal *t, double T_ref) {
    double s = 0;
    for (size_t c = 0; c < (size_t)t->nx * t->ny; c++)
        if (!t->held[c]) s += t->C[c] * (t->T[c] - T_ref);
    return s;
}

/* y = A x over the free cells (held cells count as zero in x) */
static void apply(const MThermal *t, double idt, const double *x, double *y) {
    const int nx = t->nx, ny = t->ny;
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++) {
            size_t c = (size_t)j * nx + i;
            if (t->held[c]) {
                y[c] = 0;
                continue;
            }
            double v = t->diag[c] * x[c];
            if (i + 1 < nx && !t->held[c + 1]) v -= t->ge[c] * x[c + 1];
            if (i > 0 && !t->held[c - 1]) v -= t->ge[c - 1] * x[c - 1];
            if (j + 1 < ny && !t->held[c + nx]) v -= t->gn[c] * x[c + nx];
            if (j > 0 && !t->held[c - nx]) v -= t->gn[c - nx] * x[c - nx];
            y[c] = v;
        }
    (void)idt;
}

int mt_step(MThermal *t, double dt, const double *q) {
    const int nx = t->nx, ny = t->ny;
    const size_t n = (size_t)nx * ny;
    const double idt = 1 / dt, V = t->dx * t->dx;
    /* the system (C / dt + K) T' = C / dt T + q V + (conductance to held neighbours) T_held */
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++) {
            size_t c = (size_t)j * nx + i;
            if (t->held[c]) {
                t->diag[c] = 1, t->b[c] = 0;
                continue;
            }
            double g[4] = {i + 1 < nx ? t->ge[c] : 0, i > 0 ? t->ge[c - 1] : 0, j + 1 < ny ? t->gn[c] : 0, j > 0 ? t->gn[c - nx] : 0};
            size_t nb[4] = {c + 1, c - 1, c + nx, c - nx};
            double d = t->C[c] * idt, b = t->C[c] * idt * t->T[c] + (q ? q[c] * V : 0);
            for (int f = 0; f < 4; f++) {
                if (g[f] == 0) continue;
                d += g[f];
                if (t->held[nb[f]]) b += g[f] * t->T[nb[f]];
            }
            t->diag[c] = d, t->b[c] = b;
        }
    /* preconditioned conjugate gradients from the current temperature */
    double *x = t->T, *r = t->r, *z = t->z, *p = t->p, *Ap = t->Ap;
    apply(t, idt, x, Ap);
    double rz = 0, bb = 0;
    for (size_t c = 0; c < n; c++) {
        if (t->held[c]) {
            r[c] = z[c] = p[c] = 0;
            continue;
        }
        r[c] = t->b[c] - Ap[c], z[c] = r[c] / t->diag[c], p[c] = z[c];
        rz += r[c] * z[c], bb += t->b[c] * t->b[c];
    }
    /* held cells keep their value: apply() treats them as zero, so hide them during the iteration */
    double *hv = malloc(n * sizeof(double));
    for (size_t c = 0; c < n; c++) hv[c] = t->held[c] ? x[c] : 0, x[c] = t->held[c] ? 0 : x[c];
    int it = 0;
    const double tol2 = 1e-24 * (bb > 0 ? bb : 1);
    double rr = 0;
    for (size_t c = 0; c < n; c++) rr += r[c] * r[c];
    while (rr > tol2 && it < 20000) {
        apply(t, idt, p, Ap);
        double pAp = 0;
        for (size_t c = 0; c < n; c++) pAp += p[c] * Ap[c];
        if (!(pAp > 0)) break;
        double a = rz / pAp, rz2 = 0;
        rr = 0;
        for (size_t c = 0; c < n; c++) {
            x[c] += a * p[c], r[c] -= a * Ap[c];
            z[c] = t->held[c] ? 0 : r[c] / t->diag[c];
            rz2 += r[c] * z[c], rr += r[c] * r[c];
        }
        double beta = rz2 / rz;
        rz = rz2;
        for (size_t c = 0; c < n; c++) p[c] = z[c] + beta * p[c];
        it++;
    }
    for (size_t c = 0; c < n; c++)
        if (t->held[c]) x[c] = hv[c];
    free(hv);
    /* the heat into the held cells, from the new temperatures (the same fluxes the step used) */
    double h = 0;
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++) {
            size_t c = (size_t)j * nx + i;
            if (t->held[c]) continue;
            if (i + 1 < nx && t->held[c + 1]) h += t->ge[c] * (x[c] - x[c + 1]);
            if (i > 0 && t->held[c - 1]) h += t->ge[c - 1] * (x[c] - x[c - 1]);
            if (j + 1 < ny && t->held[c + nx]) h += t->gn[c] * (x[c] - x[c + nx]);
            if (j > 0 && t->held[c - nx]) h += t->gn[c - nx] * (x[c] - x[c - nx]);
        }
    t->held_heat = h;
    return rr <= tol2 ? it : -1;
}
