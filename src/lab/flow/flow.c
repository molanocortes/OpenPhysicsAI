/* flow.c - see flow.h. */
#include "flow.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../lbm.h"
#include "../../threads.h"

struct Flow {
    FlowSpec spec;
    Lbm L;
    ThreadPool *pool;
    LbmConfig cfg;
    LbmOutput out;
    int nx, ny, nz;
    double cx, cy;          /* body centre, lattice units */
    double *poly;           /* aerofoil outline, lattice units */
    float *profile;         /* a channel's inflow parabola, one factor per row */
    int npoly;
    long steps;
    double *cd, *cl;        /* per step */
    long cap;
    double *dye;            /* particles x, y (lattice), their age */
    int ndye, capdye;
    double dt_s, dx_m, u_phys;
    bool unstable;
};

void flow_spec_defaults(FlowSpec *s) {
    memset(s, 0, sizeof *s);
    s->Re = 100;
    s->D = 16;
    snprintf(s->naca, sizeof s->naca, "0012");
    s->width_D = 16, s->length_D = 30, s->upstream_D = 8;
    s->u_lb = 0.08;
    s->nu_m2_s = 1.0e-6;
    s->D_m = 0.01;
    s->dye_every = 4;
}

/* the NACA 4-digit outline, chord D, leading edge at the origin before rotation about the quarter chord */
static int naca_polygon(const char *code, double chord, double aoa_deg, double cx, double cy, double **out) {
    int m = code[0] - '0', p = code[1] - '0', t = (code[2] - '0') * 10 + (code[3] - '0');
    double M = m / 100.0, P = p / 10.0, T = t / 100.0;
    const int n = 120;
    double *pts = malloc((size_t)(2 * n) * 2 * sizeof(double));
    int k = 0;
    for (int side = 0; side < 2; side++)
        for (int q = 0; q < n; q++) {
            int i = side == 0 ? n - 1 - q : q; /* upper from trailing to leading edge, then lower back */
            double beta = M_PI * i / (n - 1), x = 0.5 * (1 - cos(beta));
            double yt = 5 * T * (0.2969 * sqrt(x) - 0.1260 * x - 0.3516 * x * x + 0.2843 * x * x * x - 0.1036 * x * x * x * x);
            double yc = 0, dyc = 0;
            if (M > 0 && P > 0) {
                if (x < P) yc = M / (P * P) * (2 * P * x - x * x), dyc = 2 * M / (P * P) * (P - x);
                else yc = M / ((1 - P) * (1 - P)) * (1 - 2 * P + 2 * P * x - x * x), dyc = 2 * M / ((1 - P) * (1 - P)) * (P - x);
            }
            double th = atan(dyc), sgn = side == 0 ? 1 : -1;
            double X = x - sgn * yt * sin(th), Y = yc + sgn * yt * cos(th);
            if (side == 1 && (q == 0 || q == n - 1)) continue; /* the ends are shared */
            /* about the quarter chord, clockwise by the angle of attack (nose up for a flow along +x) */
            double a = -aoa_deg * M_PI / 180, xr = (X - 0.25) * chord, yr = Y * chord;
            pts[2 * k] = cx + xr * cos(a) - yr * sin(a), pts[2 * k + 1] = cy + xr * sin(a) + yr * cos(a);
            k++;
        }
    *out = pts;
    return k;
}

static bool in_poly(const double *p, int n, double x, double y) {
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        double xi = p[2 * i], yi = p[2 * i + 1], xj = p[2 * j], yj = p[2 * j + 1];
        if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi)) in = !in;
    }
    return in;
}

static bool inside(const Flow *f, double x, double y) {
    if (f->spec.shape == FLOW_CYLINDER) return (x - f->cx) * (x - f->cx) + (y - f->cy) * (y - f->cy) < 0.25 * f->spec.D * f->spec.D;
    return in_poly(f->poly, f->npoly, x, y);
}

/* where a link from a fluid node enters the body: the smallest t in (0, 2] */
static double entry(const Flow *f, const float seg[6]) {
    double ox = seg[0], oy = seg[1], dx = seg[3], dy = seg[4];
    if (dx == 0 && dy == 0) return -1;
    if (f->spec.shape == FLOW_CYLINDER) {
        double r = 0.5 * f->spec.D, px = ox - f->cx, py = oy - f->cy;
        double a = dx * dx + dy * dy, b = 2 * (px * dx + py * dy), c = px * px + py * py - r * r, disc = b * b - 4 * a * c;
        if (disc < 0) return -1;
        return (-b - sqrt(disc)) / (2 * a);
    }
    double best = -1;
    for (int i = 0, j = f->npoly - 1; i < f->npoly; j = i++) {
        double ax = f->poly[2 * j], ay = f->poly[2 * j + 1], bx = f->poly[2 * i] - ax, by = f->poly[2 * i + 1] - ay;
        double den = dx * by - dy * bx;
        if (fabs(den) < 1e-15) continue;
        double t = ((ax - ox) * by - (ay - oy) * bx) / den, u = ((ax - ox) * dy - (ay - oy) * dx) / den;
        if (u >= 0 && u <= 1 && t > 0 && (best < 0 || t < best)) best = t;
    }
    return best;
}

Flow *flow_create(const FlowSpec *s, char *err, size_t errlen) {
    if (!(s->Re > 0) || s->D < 4 || !(s->u_lb > 0 && s->u_lb < 0.2) || !(s->width_D > 1) || !(s->length_D > s->upstream_D + 1)) {
        snprintf(err, errlen, "flow: Re > 0, D >= 4 cells, lattice velocity in (0, 0.2), a tunnel longer than the body's distance from the inlet");
        return NULL;
    }
    Flow *f = calloc(1, sizeof *f);
    f->spec = *s;
    f->nx = (int)lround(s->length_D * s->D), f->ny = (int)lround(s->width_D * s->D), f->nz = 4; /* the lattice needs 4; nothing varies along the periodic span, so the flow stays two-dimensional */
    f->cx = s->upstream_D * s->D, f->cy = 0.5 * f->ny + s->body_y_D * s->D;
    if (s->shape == FLOW_NACA4) f->npoly = naca_polygon(s->naca, s->D, s->aoa_deg, f->cx, f->cy, &f->poly);
    int nt = s->threads > 0 ? s->threads : cpu_perf_count();
    f->pool = pool_create(nt < 1 ? 1 : nt);
    if (!lbm_create(&f->L, f->nx, f->ny, f->nz, f->pool)) {
        snprintf(err, errlen, "flow: cannot allocate the lattice (%d x %d x %d)", f->nx, f->ny, f->nz);
        pool_destroy(f->pool);
        free(f->poly);
        free(f);
        return NULL;
    }
    f->L.periodic_z = true; /* the span wraps; set before the solids so the wall links are built across it */
    uint8_t *solid = calloc(f->L.ncells, 1);
    for (int k = 0; k < f->nz; k++)
        for (int j = 0; j < f->ny; j++)
            for (int i = 0; i < f->nx; i++)
                if (inside(f, i + 0.5, j + 0.5)) solid[(size_t)i + (size_t)f->nx * ((size_t)j + (size_t)f->ny * k)] = 1;
    lbm_set_solids(&f->L, solid);
    free(solid);
    /* curved walls: the exact crossing of every boundary link */
    float *delta = malloc(f->L.nlinks * sizeof(float));
    for (size_t n = 0; n < f->L.nlinks; n++) {
        float seg[6];
        lbm_link_segment(&f->L, n, seg);
        double t = entry(f, seg);
        delta[n] = (t > 0 && t <= 2) ? (float)t : 0.5f;
    }
    lbm_set_link_deltas(&f->L, delta);
    lbm_reset(&f->L, 0.0f);
    double nu_lb = s->u_lb * s->D / s->Re;
    LbmConfig c;
    memset(&c, 0, sizeof c);
    c.tau0 = (float)(3 * nu_lb + 0.5);
    c.collision = s->collision == 1 ? LBM_BGK : s->collision == 2 ? LBM_REGULARIZED : LBM_RECURSIVE;
    c.wall[LBM_SIDE_FLOOR] = c.wall[LBM_SIDE_CEILING] = LBM_WALL_FREESTREAM;
    c.wall[LBM_SIDE_LEFT] = c.wall[LBM_SIDE_RIGHT] = LBM_WALL_PERIODIC;
    c.sponge_in = s->D / 2, c.sponge_out = s->D, c.sponge_tau = 0.8f, c.sponge_sigma = 0.2f;
    if (s->channel) {
        /* no-slip walls halfway between the ghost layer and the first row, so the channel is ny cells high; the
         * parabola u = 6 U y (H - y) / H^2 at the row centres has mean U; no absorbing layer at the inlet (it would
         * flatten the parabola) and none pulling the outlet towards a uniform stream */
        c.wall[LBM_SIDE_FLOOR] = c.wall[LBM_SIDE_CEILING] = LBM_WALL_NOSLIP;
        f->profile = malloc((size_t)f->ny * sizeof(float));
        for (int j = 0; j < f->ny; j++) {
            double y = j + 0.5, H = f->ny;
            f->profile[j] = (float)(6 * y * (H - y) / (H * H));
        }
        c.in_profile = f->profile;
        c.sponge_in = 0, c.sponge_sigma = 0;
        /* the channel starts from its Poiseuille flow, not from rest: the parabola would otherwise take a viscous time
         * H^2 / nu (hundreds of D / U at Re 20) to diffuse across, and a steady answer would never be reached */
        lbm_reset_profile(&f->L, (float)s->u_lb, f->profile);
    }
    f->cfg = c;
    size_t nc = f->L.ncells;
    f->out = (LbmOutput){malloc(nc * 4), malloc(nc * 4), malloc(nc * 4), malloc(nc * 4)};
    /* units of the pictures */
    f->dx_m = s->D_m / s->D;
    f->u_phys = s->Re * s->nu_m2_s / s->D_m;
    f->dt_s = s->u_lb * f->dx_m / f->u_phys;
    return f;
}

void flow_free(Flow *f) {
    if (!f) return;
    lbm_destroy(&f->L);
    pool_destroy(f->pool);
    free(f->out.rho), free(f->out.ux), free(f->out.uy), free(f->out.uz);
    free(f->poly), free(f->cd), free(f->cl), free(f->dye), free(f->profile);
    free(f);
}

static inline size_t cell(const Flow *f, int i, int j) { return (size_t)i + (size_t)f->nx * (size_t)j; }

static void velocity_at(const Flow *f, double x, double y, double *u, double *v) {
    double a = x - 0.5, b = y - 0.5;
    int i = (int)floor(a), j = (int)floor(b);
    double tx = a - i, ty = b - j;
    if (i < 0) i = 0, tx = 0;
    if (j < 0) j = 0, ty = 0;
    if (i > f->nx - 2) i = f->nx - 2, tx = 1;
    if (j > f->ny - 2) j = f->ny - 2, ty = 1;
    const float *U = f->out.ux, *V = f->out.uy;
    size_t c00 = cell(f, i, j), c10 = c00 + 1, c01 = c00 + (size_t)f->nx, c11 = c01 + 1;
    *u = (1 - ty) * ((1 - tx) * U[c00] + tx * U[c10]) + ty * ((1 - tx) * U[c01] + tx * U[c11]);
    *v = (1 - ty) * ((1 - tx) * V[c00] + tx * V[c10]) + ty * ((1 - tx) * V[c01] + tx * V[c11]);
}

bool flow_advance(Flow *f, long n) {
    const double u = f->spec.u_lb;
    const long ramp = 800;
    for (long q = 0; q < n && !f->unstable; q++) {
        long s = f->steps + 1;
        double r = (s < ramp && !f->spec.channel) ? (double)s / ramp : 1; /* a channel starts at full speed */
        f->cfg.u_in = (float)(u * r * r * (3 - 2 * r));
        /* a brief cross-flow pulse after the ramp breaks the symmetry, so shedding starts without waiting for noise */
        f->cfg.v_in = (s > ramp && s < ramp + 2 * f->spec.D / u) ? (float)(0.1 * u) : 0.0f;
        /* the velocity field is copied out only when the dye needs it or when the call ends (frames, the wake) */
        lbm_step(&f->L, &f->cfg, (f->spec.ndye > 0 || q == n - 1) ? &f->out : NULL);
        if (f->L.unstable) f->unstable = true;
        if (f->steps + 1 > f->cap) {
            f->cap = f->cap ? 2 * f->cap : 65536;
            f->cd = realloc(f->cd, (size_t)f->cap * sizeof(double)), f->cl = realloc(f->cl, (size_t)f->cap * sizeof(double));
        }
        double q0 = 0.5 * u * u * f->spec.D * f->nz;
        f->cd[f->steps] = f->L.force[0] / q0, f->cl[f->steps] = f->L.force[1] / q0;
        f->steps = s;
        /* dye: released at the seeds, carried with the local velocity (midpoint rule), dropped when it leaves */
        if (f->spec.ndye > 0 && s % f->spec.dye_every == 0) {
            if (f->ndye + f->spec.ndye > f->capdye) {
                f->capdye = f->capdye ? 2 * f->capdye : 4096;
                f->dye = realloc(f->dye, (size_t)f->capdye * 3 * sizeof(double));
            }
            for (int d = 0; d < f->spec.ndye; d++) {
                double *p = f->dye + 3 * f->ndye++;
                p[0] = f->cx + f->spec.dye_xy[d][0] * f->spec.D, p[1] = f->cy + f->spec.dye_xy[d][1] * f->spec.D, p[2] = 0;
            }
        }
        int k = 0;
        for (int d = 0; d < f->ndye; d++) {
            double *p = f->dye + 3 * d, u1, v1, u2, v2;
            velocity_at(f, p[0], p[1], &u1, &v1);
            velocity_at(f, p[0] + 0.5 * u1, p[1] + 0.5 * v1, &u2, &v2);
            double x = p[0] + u2, y = p[1] + v2;
            if (x < 1 || x > f->nx - 2 || y < 1 || y > f->ny - 2 || inside(f, x, y)) continue;
            double *o = f->dye + 3 * k++;
            o[0] = x, o[1] = y, o[2] = p[2] + 1;
        }
        f->ndye = k;
    }
    return !f->unstable;
}

long flow_steps(const Flow *f) { return f->steps; }
double flow_time_s(const Flow *f) { return f->steps * f->dt_s; }
double flow_convective_time(const Flow *f) { return f->steps * f->spec.u_lb / f->spec.D; }
bool flow_unstable(const Flow *f) { return f->unstable; }

void flow_coefficients(const Flow *f, long from, double *cd, double *cl, double *cd_mean, double *cl_rms) {
    long n = f->steps;
    if (from < 0) from = 0;
    *cd = n ? f->cd[n - 1] : 0, *cl = n ? f->cl[n - 1] : 0;
    double s = 0, s2 = 0, sl = 0;
    long m = 0;
    for (long i = from; i < n; i++) s += f->cd[i], sl += f->cl[i], m++;
    double ml = m ? sl / m : 0;
    for (long i = from; i < n; i++) s2 += (f->cl[i] - ml) * (f->cl[i] - ml);
    *cd_mean = m ? s / m : 0, *cl_rms = m ? sqrt(s2 / m) : 0;
}

void flow_extrema(const Flow *f, long from, double *cd_max, double *cl_max) {
    if (from < 0) from = 0;
    double a = -INFINITY, b = -INFINITY;
    for (long i = from; i < f->steps; i++) a = fmax(a, f->cd[i]), b = fmax(b, f->cl[i]);
    *cd_max = a, *cl_max = b;
}

double flow_strouhal(const Flow *f, long from, int *periods) {
    /* upward zero crossings of the lift about its mean, located by linear interpolation */
    long n = f->steps;
    if (from < 1) from = 1;
    double mean = 0;
    for (long i = from; i < n; i++) mean += f->cl[i];
    mean /= (n > from ? n - from : 1);
    double first = -1, last = -1;
    int cnt = 0;
    double amp = 0;
    for (long i = from; i < n; i++) amp = fmax(amp, fabs(f->cl[i] - mean));
    for (long i = from + 1; i < n; i++) {
        double a = f->cl[i - 1] - mean, b = f->cl[i] - mean;
        if (a < 0 && b >= 0) {
            double t = (i - 1) + a / (a - b);
            if (first < 0) first = t;
            last = t;
            cnt++;
        }
    }
    if (periods) *periods = cnt > 1 ? cnt - 1 : 0;
    if (cnt < 3 || amp < 1e-4) return 0;
    double T = (last - first) / (cnt - 1);
    return f->spec.D / (T * f->spec.u_lb);
}

double flow_wake_length(const Flow *f) {
    int j = (int)floor(f->cy); /* the cell row nearest the centreline */
    double xr = f->cx + 0.5 * f->spec.D;
    bool neg = false;
    for (int i = (int)ceil(xr); i < f->nx - 1; i++) {
        double u0 = 0.5 * (f->out.ux[cell(f, i, j)] + f->out.ux[cell(f, i, j - 1)]);
        double u1 = 0.5 * (f->out.ux[cell(f, i + 1, j)] + f->out.ux[cell(f, i + 1, j - 1)]);
        if (u0 < 0) neg = true;
        if (neg && u0 < 0 && u1 >= 0) {
            double x = i + 0.5 + u0 / (u0 - u1);
            return (x - xr) / f->spec.D;
        }
    }
    return 0;
}

void flow_write_frame(Flow *f, LabWriter *w) {
    LabBlock b = {{f->nx, f->ny, 1}, 0, LAB_PLANE_XY, {0, 0, 0}, {f->dx_m, f->dx_m, f->dx_m}};
    size_t n = (size_t)f->nx * f->ny;
    float *vort = malloc(n * sizeof(float)), *speed = malloc(n * sizeof(float)), *ux = malloc(n * sizeof(float)), *sol = malloc(n * sizeof(float));
    double vscale = f->u_phys / f->spec.u_lb; /* lattice velocity to m/s */
    for (int j = 0; j < f->ny; j++)
        for (int i = 0; i < f->nx; i++) {
            size_t c = cell(f, i, j);
            int il = i > 0 ? i - 1 : i, ir = i < f->nx - 1 ? i + 1 : i, jl = j > 0 ? j - 1 : j, jr = j < f->ny - 1 ? j + 1 : j;
            double dvdx = (f->out.uy[cell(f, ir, j)] - f->out.uy[cell(f, il, j)]) / (ir - il);
            double dudy = (f->out.ux[cell(f, i, jr)] - f->out.ux[cell(f, i, jl)]) / (jr - jl);
            bool s = inside(f, i + 0.5, j + 0.5);
            vort[c] = s ? 0.0f : (float)((dvdx - dudy) / f->dt_s);
            speed[c] = s ? 0.0f : (float)(hypot(f->out.ux[c], f->out.uy[c]) * vscale);
            ux[c] = s ? 0.0f : (float)(f->out.ux[c] * vscale);
            sol[c] = s ? 1.0f : 0.0f;
        }
    lab_part_blocks(w, "flow", 1, &b);
    lab_field(w, "vorticity", LAB_AT_CELL, n, vort);
    lab_field(w, "speed", LAB_AT_CELL, n, speed);
    lab_field(w, "ux", LAB_AT_CELL, n, ux);
    lab_field(w, "solid", LAB_AT_CELL, n, sol);
    free(vort), free(speed), free(ux), free(sol);
    if (f->ndye > 0) {
        double *X = malloc((size_t)f->ndye * 3 * sizeof(double));
        float *age = malloc((size_t)f->ndye * sizeof(float));
        for (int d = 0; d < f->ndye; d++) {
            X[3 * d] = f->dye[3 * d] * f->dx_m, X[3 * d + 1] = f->dye[3 * d + 1] * f->dx_m, X[3 * d + 2] = 0;
            age[d] = (float)(f->dye[3 * d + 2] * f->dt_s);
        }
        lab_part_points(w, "dye", f->ndye, X);
        lab_field(w, "age", LAB_AT_NODE, (size_t)f->ndye, age);
        free(X), free(age);
    }
}

char *flow_header_json(const FlowSpec *s, const char *title) {
    char *h = malloc(2048);
    snprintf(h, 2048,
             "{\"domain\":\"flow\",\"title\":\"%s\",\"solver\":\"src/lbm.c through src/lab/flow: D3Q19 lattice Boltzmann, recursive regularised "
             "collision, curved walls, two-dimensional (periodic span)\",\"Re\":%.6g,\"D_cells\":%d,\"fields\":{\"vorticity\":\"1/s\",\"speed\":\"m/s\","
             "\"ux\":\"m/s\",\"solid\":\"1\",\"age\":\"s\"}}",
             title ? title : "", s->Re, s->D);
    return h;
}
