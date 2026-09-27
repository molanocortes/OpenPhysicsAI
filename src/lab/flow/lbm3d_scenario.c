/* lbm3d_scenario.c - a body in a stream in three dimensions (lbm3d.h), described in SI (docs/lab/flow.md, "Bodies in a
 * stream in 3D"): the fluid and its source, the stream's speed, the box, the cell, the bodies (labshape.h), and the run.
 *
 * The lattice's speed (0.05 by default: Mach 0.087, compressibility 0.3 %) sets the time step, dt = u_lattice dx / U,
 * and the lattice viscosity follows, nu dt / dx^2. Written per frame: the flow on the grid (every output_stride-th cell)
 * with its speed, velocity, vorticity and Q (the second invariant of the velocity gradient, whose positive regions are
 * vortex cores), and the bodies' surface. The run record gives the force coefficients (averaged over the settled part
 * of the run), the frequency of the side force (Strouhal number), and the speed of the computation. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../core/json.h"
#include "../../threads.h"
#include "../lab_domains.h"
#include "../labio.h"
#include "../labshape.h"
#include "../fsi/fsi.h"
#include "../sheet/sheet.h"
#include "lbm3d.h"
#include "lbm3d_metal.h"

typedef struct {
    const LabShapes *S;
    double o[3], dx;
} SdfCtx;

static double sdf_cells(const double p[3], void *vctx) { /* lbm3d asks in cells, the shapes answer in metres */
    const SdfCtx *c = vctx;
    double q[3] = {c->o[0] + p[0] * c->dx, c->o[1] + p[1] * c->dx, c->o[2] + p[2] * c->dx};
    return labshape_sdf(c->S, q) / c->dx;
}

/* a sheet in the stream (fsi.h): a rectangle of size[0] along `along` and size[1] along `across` from `corner`, meshed
 * at `cell` with alternating diagonals; nodes of its first edge (along = 0) held when `pinned` */
static Sheet *make_sheet(const JsonValue *j, int **tri, int *ntri, int *nu, int *nv, char *err, size_t errlen) {
    const JsonValue *m = json_get(j, "material");
    double size[2], c0[3], a[3], b[3], H = json_get_num(j, "thickness_m", -1), h = json_get_num(j, "cell_m", -1);
    if (!json_get_str(m, "source", NULL) || !json_get_numbers(json_get(j, "size_m"), size, 2) || !json_get_numbers(json_get(j, "corner_m"), c0, 3) ||
        !json_get_numbers(json_get(j, "along"), a, 3) || !json_get_numbers(json_get(j, "across"), b, 3) || !(H > 0) || !(h > 0)) {
        snprintf(err, errlen, "lbm3d: sheet {material {shear_modulus_pa, density_kg_m3, source}, thickness_m, cell_m, size_m [a, b], corner_m, "
                              "along [x, y, z], across [x, y, z], pinned} is required for a sheet in the flow");
        return NULL;
    }
    double la = sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), lb = sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    int n1 = (int)lround(size[0] / h) + 1, n2 = (int)lround(size[1] / h) + 1, nn = n1 * n2, nt = 2 * (n1 - 1) * (n2 - 1), k = 0;
    double *X = malloc(3 * (size_t)nn * sizeof(double));
    *tri = malloc(3 * (size_t)nt * sizeof(int));
    if (!X || !*tri || !(la > 0) || !(lb > 0) || n1 < 2 || n2 < 2) {
        free(X), free(*tri), *tri = NULL;
        snprintf(err, errlen, "lbm3d: the sheet's directions must be nonzero and it needs two nodes each way");
        return NULL;
    }
    for (int j2 = 0; j2 < n2; j2++)
        for (int i = 0; i < n1; i++)
            for (int c = 0; c < 3; c++) X[3 * (j2 * n1 + i) + c] = c0[c] + a[c] / la * size[0] * i / (n1 - 1) + b[c] / lb * size[1] * j2 / (n2 - 1);
    for (int j2 = 0; j2 + 1 < n2; j2++)
        for (int i = 0; i + 1 < n1; i++) {
            int p = j2 * n1 + i, q = p + 1, r = p + n1, t = r + 1;
            int *T = *tri;
            if ((i + j2) & 1) T[k++] = p, T[k++] = q, T[k++] = r, T[k++] = q, T[k++] = t, T[k++] = r;
            else T[k++] = p, T[k++] = q, T[k++] = t, T[k++] = p, T[k++] = t, T[k++] = r;
        }
    SheetSpec sp = {.shear_modulus = json_get_num(m, "shear_modulus_pa", -1), .thickness = H, .density = json_get_num(m, "density_kg_m3", -1),
                    .bending_scale = json_get_num(j, "bending_scale", 1), .gravity = json_get_num(j, "gravity_m_s2", 9.80665),
                    .viscosity = json_get_num(m, "viscosity_pa_s", 0), .self_contact = json_get_num(j, "self_contact_m", 0)};
    Sheet *S = sheet_create(&sp, nn, X, nt, *tri, err, errlen);
    free(X);
    if (S && strcmp(json_get_str(j, "pinned", "first_edge"), "none"))
        for (int j2 = 0; j2 < n2; j2++) sheet_pin(S, j2 * n1);
    *ntri = nt, *nu = n1, *nv = n2;
    return S;
}

/* a three-leaflet valve (aortic-like) in a pipe along x: each leaflet spans a third of the wall, attached along a
 * U-shaped line (commissures at x_c, its lowest point `height` upstream), its free edge two straight lines from the
 * commissures to near the axis; the leaflets start closed, bellied upstream by `belly`, their free edges `gap` apart
 * (the contact keeps them from passing through each other when they meet). The attachment is held. */
static Sheet *make_valve(const JsonValue *j, const JsonValue *v, int **tri, int *ntri, int *free_mid, char *err, size_t errlen) {
    const JsonValue *m = json_get(j, "material");
    double c0[3], R = json_get_num(v, "radius_m", -1), Hc = json_get_num(v, "height_m", -1), belly = json_get_num(v, "belly_m", 0);
    double gap = json_get_num(v, "gap_m", -1), H = json_get_num(j, "thickness_m", -1);
    int na = (int)json_get_int(v, "nodes_around", 25), nr = 0;
    if (!json_get_str(m, "source", NULL) || !json_get_numbers(json_get(v, "centre_m"), c0, 3) || !(R > 0) || !(Hc > 0) || !(gap > 0) || !(H > 0) ||
        na < 5 || !(gap < R)) {
        snprintf(err, errlen, "lbm3d: a valve needs material {shear_modulus_pa, density_kg_m3, source}, thickness_m and valve {centre_m, "
                              "radius_m, height_m, belly_m, gap_m, nodes_around}");
        return NULL;
    }
    (void)nr;
    /* each leaflet: columns across it (s), each with as many nodes from the attachment (t = 0) to the free edge (t = 1)
     * as its height needs at the columns' spacing, neighbouring columns joined by walking up both: triangles stay near
     * equilateral where the leaflet narrows to its commissures (a grid of equal rows made slivers there). The outermost
     * column each side is held with the attachment: the commissure posts. */
    const double tw = M_PI / 3 - asin(0.5 * gap / R), cd = 0.5 * gap / sin(M_PI / 3), smax = 1 - 1.0 / (na - 1);
    const double hs = R * 2 * tw * smax / (na - 1); /* the columns' spacing along the wall */
    int *rows = malloc((size_t)na * sizeof(int)), per = 0, nt = 0;
    for (int a = 0; a < na; a++) {
        double sv = smax * (-1 + 2.0 * a / (na - 1)), th = sv * tw, W[2] = {R * cos(sv < 0 ? -tw : tw), R * sin(sv < 0 ? -tw : tw)};
        double A[3] = {-Hc * (1 - sv * sv), R * cos(th), R * sin(th)}, E[3] = {0, cd + fabs(sv) * (W[0] - cd), fabs(sv) * W[1]};
        double hgt = sqrt((A[0] - E[0]) * (A[0] - E[0]) + (A[1] - E[1]) * (A[1] - E[1]) + (A[2] - E[2]) * (A[2] - E[2])) + 2 * belly * (1 - sv * sv);
        rows[a] = (int)fmax(1, lround(hgt / hs));
        per += rows[a] + 1;
        if (a) nt += rows[a] + rows[a - 1];
    }
    int nn = 3 * per, ntot = 3 * nt, k = 0;
    double *X = malloc(3 * (size_t)nn * sizeof(double));
    *tri = malloc(3 * (size_t)ntot * sizeof(int));
    int *start = malloc((size_t)na * sizeof(int));
    if (!X || !*tri || !rows || !start) {
        free(X), free(*tri), free(rows), free(start), *tri = NULL;
        snprintf(err, errlen, "lbm3d: out of memory for the valve");
        return NULL;
    }
    for (int L = 0; L < 3; L++) {
        double phi = 2 * M_PI * L / 3, C[2] = {cd * cos(phi), cd * sin(phi)};
        int base = L * per, at = base;
        for (int a = 0; a < na; a++) {
            start[a] = at;
            double sv = smax * (-1 + 2.0 * a / (na - 1)), th = phi + sv * tw;
            double A[3] = {c0[0] - Hc * (1 - sv * sv), c0[1] + R * cos(th), c0[2] + R * sin(th)};
            double W[2] = {R * cos(phi + (sv < 0 ? -tw : tw)), R * sin(phi + (sv < 0 ? -tw : tw))};
            double E[3] = {c0[0], c0[1] + C[0] + fabs(sv) * (W[0] - C[0]), c0[2] + C[1] + fabs(sv) * (W[1] - C[1])};
            for (int r = 0; r <= rows[a]; r++, at++) {
                double t = (double)r / rows[a], *P = &X[3 * at];
                for (int q = 0; q < 3; q++) P[q] = (1 - t) * A[q] + t * E[q];
                P[0] -= belly * sin(M_PI * t) * (1 - sv * sv);
            }
        }
        for (int a = 0; a + 1 < na; a++) { /* the walk between columns a and a + 1 */
            int p = 0, q = 0, m0 = rows[a], m1 = rows[a + 1], *T = *tri;
            while (p < m0 || q < m1) {
                if (q < m1 && (p == m0 || (double)(q + 1) / m1 <= (double)(p + 1) / m0))
                    T[k++] = start[a] + p, T[k++] = start[a + 1] + q, T[k++] = start[a + 1] + q + 1, q++;
                else T[k++] = start[a] + p, T[k++] = start[a + 1] + q, T[k++] = start[a] + p + 1, p++;
            }
        }
        int mid = na / 2 - na / 4;
        free_mid[L] = start[mid] + rows[mid]; /* the free edge, halfway from the axis to the commissure */
    }
    nt = k / 3;
    SheetSpec sp = {.shear_modulus = json_get_num(m, "shear_modulus_pa", -1), .thickness = H, .density = json_get_num(m, "density_kg_m3", -1),
                    .bending_scale = json_get_num(j, "bending_scale", 1), .gravity = json_get_num(j, "gravity_m_s2", 0),
                    .viscosity = json_get_num(m, "viscosity_pa_s", 0), .self_contact = json_get_num(j, "self_contact_m", 0)};
    Sheet *S = sheet_create(&sp, nn, X, nt, *tri, err, errlen);
    free(X);
    for (int L = 0; S && L < 3; L++) {
        int at = L * per;
        for (int a = 0; a < na; a++)
            for (int r = 0; r <= rows[a]; r++, at++)
                if (r == 0 || a == 0 || a == na - 1) sheet_pin(S, at);
    }
    free(rows), free(start);
    *ntri = nt;
    return S;
}

/* the inflow's speed at time t from a waveform {period_s, points [[t, u], ...]} (linear between points), or U */
static double waveform(const JsonValue *wf, double t, double U) {
    const JsonValue *pts = json_get(wf, "points");
    double T = json_get_num(wf, "period_s", 0);
    size_t n = pts ? json_len(pts) : 0;
    if (!n || !(T > 0)) return U;
    t = fmod(t, T);
    double p0[2], p1[2];
    for (size_t i = 0; i + 1 < n; i++)
        if (json_get_numbers(json_at(pts, i), p0, 2) && json_get_numbers(json_at(pts, i + 1), p1, 2) && t >= p0[0] && t <= p1[0])
            return p1[0] > p0[0] ? p0[1] + (p1[1] - p0[1]) * (t - p0[0]) / (p1[0] - p0[0]) : p0[1];
    return json_get_numbers(json_at(pts, n - 1), p0, 2) ? p0[1] : U;
}

static double wall_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + 1e-9 * t.tv_nsec;
}

bool lab_run_lbm3d(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen) {
    const JsonValue *fl = json_get(root, "fluid"), *st = json_get(root, "stream"), *run = json_get(root, "run"), *les = json_get(root, "les");
    const JsonValue *ref = json_get(root, "reference");
    double nu = json_get_num(fl, "kinematic_viscosity_m2_s", -1), rho = json_get_num(fl, "density_kg_m3", -1), U = json_get_num(st, "speed_m_s", -1);
    double dx = json_get_num(root, "cell_m", -1), ul = json_get_num(root, "lattice_speed", 0.05), box[6];
    const JsonValue *pert = json_get(root, "disturbance"); /* a brief lateral inlet flow that breaks the start's symmetry */
    double pert_frac = json_get_num(pert, "lateral_fraction", 0), pert_until = json_get_num(pert, "until_s", 0);
    double noise = json_get_num(pert, "noise_fraction", 0);
    double end = json_get_num(run, "end_s", -1), avg_from = json_get_num(run, "average_from_s", -1), out_from = json_get_num(run, "output_from_s", 0);
    int frames = (int)json_get_int(run, "frames", 40), stride = (int)json_get_int(run, "output_stride", 2);
    if (!(nu > 0) || !(rho > 0) || !json_get_str(fl, "source", NULL) || !(U > 0) || !(dx > 0) || !(ul > 0 && ul <= 0.1) ||
        !json_get_numbers(json_get(root, "box_m"), box, 6) || !(end > 0) || frames < 1 || stride < 1) {
        snprintf(err, errlen, "lbm3d: fluid {kinematic_viscosity_m2_s, density_kg_m3, source}, stream {speed_m_s}, box_m, cell_m, "
                              "lattice_speed (0 to 0.1) and run {end_s, frames, output_stride} are required");
        return false;
    }
    LabShapes S;
    if (!labshape_parse(json_get(root, "bodies"), &S, err, errlen)) return false;
    const char *sides = json_get_str(root, "sides", "slip");
    int bcy = !strcmp(json_get_str(root, "sides_y", sides), "periodic") ? LBM3D_PERIODIC : LBM3D_SLIP;
    int bcz = !strcmp(json_get_str(root, "sides_z", sides), "periodic") ? LBM3D_PERIODIC : LBM3D_SLIP;
    Lbm3DSpec sp = {.nx = (int)lround((box[3] - box[0]) / dx), .ny = (int)lround((box[4] - box[1]) / dx), .nz = (int)lround((box[5] - box[2]) / dx),
                    .u_in = {ul, 0, 0}, .bc_x = LBM3D_INOUT, .bc_y = bcy, .bc_z = bcz,
                    .smagorinsky = json_get_num(les, "smagorinsky", 0)};
    double dt = ul * dx / U;
    sp.nu = nu * dt / (dx * dx);
    double tau = 3 * sp.nu + 0.5;
    if (sp.smagorinsky == 0 && tau < 0.505) {
        snprintf(err, errlen, "lbm3d: relaxation time %.5f is too close to 1/2 for a direct simulation at this cell and speed; "
                              "refine the cell, or give \"les\": {\"smagorinsky\": 0.1} for a large-eddy simulation", tau);
        return false;
    }
    if ((double)sp.nx * sp.ny * sp.nz > 3.2e7) {
        snprintf(err, errlen, "lbm3d: %d x %d x %d cells is beyond this machine's memory for the solver (32 million)", sp.nx, sp.ny, sp.nz);
        return false;
    }
    /* the GPU engine by default (lbm3d_metal.h, verified against this CPU engine by lbm3dtest L1, L4); "engine": "cpu"
     * for the double-precision one */
    bool gpu = strcmp(json_get_str(root, "engine", "gpu"), "cpu") != 0;
    sp.geometry_only = gpu;
    sp.sponge = (int)json_get_int(root, "sponge_cells", 0), sp.sponge_nu = json_get_num(root, "sponge_viscosity_lattice", 0.05);
    sp.floor = !strcmp(json_get_str(root, "floor", "none"), "wall");
    sp.outlet = !strcmp(json_get_str(root, "outlet", "extrapolated"), "pressure");
    sp.noise = noise, sp.inlet_noise = json_get_num(pert, "inlet_noise_fraction", 0);
    sp.collision = !strcmp(json_get_str(root, "collision", "trt"), "regularized") ? LBM3D_REGULARIZED : LBM3D_TRT;
    Lbm3D *L = lbm3d_create(&sp, err, errlen);
    if (!L) return false;
    SdfCtx sc = {&S, {box[0], box[1], box[2]}, dx};
    if (!lbm3d_set_bodies(L, S.n ? sdf_cells : NULL, &sc)) {
        snprintf(err, errlen, "lbm3d: out of memory for the bodies' links");
        lbm3d_free(L);
        return false;
    }
    Lbm3DGpu *G = NULL;
    if (gpu) {
        G = lbm3d_gpu_create(L, err, errlen);
        if (!G) {
            lbm3d_free(L);
            return false;
        }
        lbm3d_gpu_init(G);
    } else lbm3d_init(L);
    const JsonValue *shj = json_get(root, "sheet");
    Sheet *SH = NULL;
    Fsi *FS = NULL;
    int *sh_tri = NULL, sh_nt = 0, sh_n1 = 0, sh_n2 = 0, valve_mid[3] = {-1, -1, -1};
    const JsonValue *valve = shj ? json_get(shj, "valve") : NULL;
    if (shj) {
        if (!G) snprintf(err, errlen, "lbm3d: a sheet in the flow needs the GPU engine (its immersed boundary lives there)");
        SH = !G ? NULL : valve ? make_valve(shj, valve, &sh_tri, &sh_nt, valve_mid, err, errlen) : make_sheet(shj, &sh_tri, &sh_nt, &sh_n1, &sh_n2, err, errlen);
        const double o[3] = {box[0], box[1], box[2]};
        FS = SH ? fsi_create(G, SH, dx, dt, rho, o, (int)json_get_int(shj, "iterations", 4), json_get_num(shj, "relaxation", 1.0), err, errlen) : NULL;
        if (!FS) {
            sheet_free(SH), free(sh_tri), lbm3d_gpu_free(G), lbm3d_free(L);
            return false;
        }
    }
    const bool batched = G && !FS; /* the GPU runs batches of steps; with a sheet, one step at a time */
    if (json_get(st, "waveform")) { /* a pulsing inflow starts the field at the waveform's first speed, not the scale's */
        double u0[3] = {ul * waveform(json_get(st, "waveform"), 0, U) / U, 0, 0};
        if (G) lbm3d_gpu_set_inlet(G, u0), lbm3d_gpu_init(G);
        else lbm3d_set_inlet(L, u0), lbm3d_init(L);
    }
    const double *bmin = box, bmax[3] = {box[3], box[4], box[5]};
    double area = json_get_num(ref, "area_m2", -1), lref = json_get_num(ref, "length_m", -1);
    if (!(area > 0)) area = S.n ? labshape_frontal_area(&S, bmin, bmax, 0.5 * dx) : 1;
    if (!(lref > 0)) lref = sqrt(area);
    int tri_n = 0, tri_nodes = 0, *tri_conn = NULL;
    double *tri = NULL, *soup = labshape_mesh(&S, bmin, bmax, dx, &tri_n);
    double cut[3];
    if (soup && json_get_numbers(json_get(root, "bodies_cutaway"), cut, 3)) { /* draw the bodies' far half: see inside a vessel */
        double mid[3] = {0.5 * (box[0] + box[3]), 0.5 * (box[1] + box[4]), 0.5 * (box[2] + box[5])}, ref = cut[0] * mid[0] + cut[1] * mid[1] + cut[2] * mid[2];
        int kept = 0;
        for (int t = 0; t < tri_n; t++) {
            const double *a = &soup[9 * t];
            double cx = (a[0] + a[3] + a[6]) / 3, cy = (a[1] + a[4] + a[7]) / 3, cz = (a[2] + a[5] + a[8]) / 3;
            if (cut[0] * cx + cut[1] * cy + cut[2] * cz >= ref) memmove(&soup[9 * kept++], a, 9 * sizeof(double));
        }
        tri_n = kept;
    }
    if (tri_n > 0 && (tri_nodes = labshape_weld(soup, tri_n, &tri, &tri_conn)) < 0) tri_n = 0;
    free(soup);
    double iso = json_get_num(run, "iso_level_q_per_s2", (U / lref) * (U / lref));

    /* the result's header */
    JsonValue *meta = json_object();
    bool mok = meta && json_set_string(meta, "domain", "flow") && json_set_string(meta, "title", json_get_str(root, "title", "3D flow")) &&
               json_set_string(meta, "model", sp.collision == LBM3D_REGULARIZED ? "3D lattice Boltzmann, D3Q19 regularised, interpolated bounce-back" : "3D lattice Boltzmann, D3Q19 TRT, interpolated bounce-back") && json_set_string(meta, "volume_look", "iso") &&
               json_set_string(meta, "iso_field", "Q") && json_set_number(meta, "iso_level", iso) && json_set(meta, "scenario", json_clone(root));
    JsonValue *fields = mok ? json_set_object(meta, "fields") : NULL, *looks = mok ? json_set_object(meta, "looks") : NULL;
    mok = mok && fields && looks && json_set_string(fields, "speed", "m/s") && json_set_string(fields, "ux", "m/s") &&
          json_set_string(fields, "uy", "m/s") && json_set_string(fields, "uz", "m/s") && json_set_string(fields, "vorticity", "1/s") &&
          json_set_string(fields, "Q", "1/s^2") && json_set_string(looks, "body", json_get_str(root, "body_look", "aluminium")) &&
          (!shj || json_set_string(looks, "sheet", json_get_str(shj, "look", "fabric")));
    char *hdr = mok ? json_dump(meta, 0, NULL, NULL) : NULL;
    json_free(meta);
    LabWriter *w = hdr ? lab_create(out, hdr, err, errlen) : NULL;
    free(hdr);
    int ox = (sp.nx + stride - 1) / stride, oy = (sp.ny + stride - 1) / stride, oz = (sp.nz + stride - 1) / stride;
    size_t on = (size_t)ox * oy * oz, n = (size_t)sp.nx * sp.ny * sp.nz;
    double *u = malloc(3 * n * sizeof *u);
    float *fs = malloc(on * sizeof *fs), *fx = malloc(on * sizeof *fx), *fy = malloc(on * sizeof *fy), *fz = malloc(on * sizeof *fz);
    float *fw = malloc(on * sizeof *fw), *fq = malloc(on * sizeof *fq);
    ThreadPool *pool = pool_create(cpu_perf_count());
    if (!w || !u || !fs || !fx || !fy || !fz || !fw || !fq || !pool) {
        if (w) lab_close(w);
        else if (!err[0]) snprintf(err, errlen, "lbm3d: cannot write the result");
        free(u), free(fs), free(fx), free(fy), free(fz), free(fw), free(fq), free(tri), free(tri_conn), pool_destroy(pool), lbm3d_gpu_free(G), lbm3d_free(L);
        if (w) snprintf(err, errlen, "lbm3d: out of memory for the output");
        return false;
    }
    long steps = (long)ceil(end / dt), first_out = (long)ceil(fmax(out_from, 0) / dt), per = (steps - first_out) / frames > 0 ? (steps - first_out) / frames : 1;
    const double Fscale = rho * pow(dx, 4) / (dt * dt), q = 0.5 * rho * U * U * area;
    double cxs = 0, cys = 0, czs = 0, prev_side = 0, amp_y = 0, amp_z = 0;
    long navg = 0, cross_y = 0, cross_z = 0, first_cross = -1, last_cross = -1;
    double ymean_run = 0, zmean_run = 0;
    bool ok = true;
    double t0 = wall_seconds();
    /* two passes over the averaging window would need the history; a running mean from the settled part is enough to
     * count crossings of the side force, taken against the mean of the window so far */
    size_t hcap = 0, hn = 0;
    double *hy = NULL, *hz = NULL; /* the side forces after average_from_s, for their spectrum */
    enum { BATCH = 100 };
    long next_out = first_out > per ? first_out : per; /* frames every `per` steps from output_from_s */
    double hist[3 * BATCH], *sh_hist = NULL;
    size_t sh_hn = 0, sh_hcap = 0;
    int hk = BATCH; /* the next unread entry of hist */
    const JsonValue *wf = json_get(st, "waveform");
    double open_max = 0, open_min = 1e300, open_end = 0; /* a valve: how far its free edges move from the axis, in radii */
    for (long k = 1; k <= steps && ok; k++) {
        double F[3];
        if (wf && (k == 1 || !batched || hk == BATCH)) { /* a pulsing inflow: the waveform, scaled to the lattice's speed */
            double uin[3] = {ul * waveform(wf, k * dt, U) / U, 0, 0};
            if (G) lbm3d_gpu_set_inlet(G, uin);
            else lbm3d_set_inlet(L, uin);
        }
        if (pert_frac != 0 && (k == 1 || (batched ? hk == BATCH : true))) { /* the disturbance, then the plain stream */
            double t = k * dt, uin[3] = {ul, t < pert_until ? pert_frac * ul * sin(M_PI * t / pert_until) : 0, 0};
            if (G) lbm3d_gpu_set_inlet(G, uin);
            else lbm3d_set_inlet(L, uin);
        }
        if (FS) {
            if (!fsi_step(FS, pool, F)) {
                /* where: the fastest cell (in the stream's speed) and the lowest density */
                double *rh = malloc(n * sizeof *rh), um = 0, rmin = 1e9;
                long iu = 0, ir = 0;
                if (rh) {
                    lbm3d_gpu_macro(G, rh, u);
                    for (size_t i = 0; i < n; i++) {
                        double a = sqrt(u[3 * i] * u[3 * i] + u[3 * i + 1] * u[3 * i + 1] + u[3 * i + 2] * u[3 * i + 2]);
                        if (a > um || a != a) um = a, iu = (long)i;
                        if (rh[i] < rmin) rmin = rh[i], ir = (long)i;
                    }
                    free(rh);
                }
                snprintf(err, errlen, "lbm3d: the flow with the sheet went unstable at step %ld (t = %.4g s): speed %.3g of the stream's at cell "
                                      "(%ld, %ld, %ld), lowest density %.4g at (%ld, %ld, %ld)", k, k * dt, um / ul, iu % sp.nx, (iu / sp.nx) % sp.ny,
                         iu / ((long)sp.nx * sp.ny), rmin, ir % sp.nx, (ir / sp.nx) % sp.ny, ir / ((long)sp.nx * sp.ny));
                ok = false;
                break;
            }
            if (SH && valve) {
                const double *xs = sheet_positions(SH);
                double R = json_get_num(valve, "radius_m", 1), c[3];
                json_get_numbers(json_get(valve, "centre_m"), c, 3);
                double o = 0;
                for (int L2 = 0; L2 < 3; L2++) o += hypot(xs[3 * valve_mid[L2] + 1] - c[1], xs[3 * valve_mid[L2] + 2] - c[2]) / (3 * R);
                open_max = fmax(open_max, o), open_min = fmin(open_min, o), open_end = o;
            } else if (SH) { /* the sheet's trailing edge, middle: its sideways swing */
                const double *xs = sheet_positions(SH);
                int node = (sh_n2 / 2) * sh_n1 + sh_n1 - 1;
                if (k * dt >= avg_from && avg_from >= 0) {
                    double yv = xs[3 * node + 1];
                    if (sh_hn == sh_hcap) {
                        sh_hcap = sh_hcap ? 2 * sh_hcap : 4096;
                        double *a = realloc(sh_hist, sh_hcap * sizeof *a);
                        if (a) sh_hist = a;
                        else sh_hcap = sh_hn;
                    }
                    if (sh_hn < sh_hcap) sh_hist[sh_hn++] = yv;
                }
            }
        } else if (G) {
            if (hk == BATCH) { /* the GPU runs a batch of steps; their forces are read one by one */
                if (!lbm3d_gpu_steps(G, BATCH, hist)) {
                    snprintf(err, errlen, "lbm3d: the flow went unstable near step %ld (t = %.4g s); refine the cell or use les", k, k * dt);
                    ok = false;
                    break;
                }
                hk = 0;
            }
            F[0] = hist[3 * hk], F[1] = hist[3 * hk + 1], F[2] = hist[3 * hk + 2], hk++;
        } else {
            if (!lbm3d_step(L, pool)) {
                snprintf(err, errlen, "lbm3d: the flow went unstable at step %ld (t = %.4g s); refine the cell or use les", k, k * dt);
                ok = false;
                break;
            }
            lbm3d_force(L, F);
        }

        double cx = F[0] * Fscale / q, cy = F[1] * Fscale / q, cz = F[2] * Fscale / q, t = k * dt;
        if (avg_from >= 0 && t >= avg_from) {
            navg++, cxs += cx, cys += cy, czs += cz;
            if (hn == hcap) {
                hcap = hcap ? 2 * hcap : 4096;
                double *a = realloc(hy, hcap * sizeof *a), *b = a ? realloc(hz, hcap * sizeof *b) : NULL;
                if (a) hy = a;
                if (b) hz = b;
                if (!a || !b) hcap = hn; /* keep what fits */
            }
            if (hn < hcap) hy[hn] = cy, hz[hn] = cz, hn++;
            ymean_run = cys / navg, zmean_run = czs / navg;
            amp_y = fmax(amp_y, fabs(cy - ymean_run)), amp_z = fmax(amp_z, fabs(cz - zmean_run));
            double side = amp_y >= amp_z ? cy - ymean_run : cz - zmean_run;
            if (navg > 1 && prev_side < 0 && side >= 0) {
                if (amp_y >= amp_z) cross_y++;
                else cross_z++;
                if (first_cross < 0) first_cross = k;
                last_cross = k;
            }
            prev_side = side;
        }
        if ((k >= next_out || k == steps) && (!batched || hk == BATCH || k == steps)) {
            next_out += per;
            if (G) lbm3d_gpu_macro(G, NULL, u);
            else lbm3d_macro(L, NULL, u);
            const double vs = dx / dt; /* lattice speed to m/s */
            for (int z = 0; z < oz; z++)
                for (int y = 0; y < oy; y++)
                    for (int x = 0; x < ox; x++) {
                        int X = x * stride, Y = y * stride, Z = z * stride;
                        size_t i = (size_t)X + (size_t)sp.nx * ((size_t)Y + (size_t)sp.ny * Z), o = (size_t)x + (size_t)ox * ((size_t)y + (size_t)oy * z);
                        double a = u[3 * i], b = u[3 * i + 1], c = u[3 * i + 2];
                        fx[o] = (float)(a * vs), fy[o] = (float)(b * vs), fz[o] = (float)(c * vs), fs[o] = (float)(sqrt(a * a + b * b + c * c) * vs);
                        /* velocity gradient by central differences on the solver's grid (one-sided at the faces) */
                        double G[3][3];
                        int P[3] = {X, Y, Z}, N[3] = {sp.nx, sp.ny, sp.nz};
                        bool per[3] = {false, bcy == LBM3D_PERIODIC, bcz == LBM3D_PERIODIC}; /* wrap where the flow does */
                        for (int dir = 0; dir < 3; dir++) {
                            int lo = P[dir] > 0 ? P[dir] - 1 : per[dir] ? N[dir] - 1 : P[dir], hi = P[dir] + 1 < N[dir] ? P[dir] + 1 : per[dir] ? 0 : P[dir];
                            int A[3] = {X, Y, Z}, B[3] = {X, Y, Z};
                            A[dir] = lo, B[dir] = hi;
                            size_t ia = (size_t)A[0] + (size_t)sp.nx * ((size_t)A[1] + (size_t)sp.ny * A[2]);
                            size_t ib = (size_t)B[0] + (size_t)sp.nx * ((size_t)B[1] + (size_t)sp.ny * B[2]);
                            double h = (per[dir] && (hi < lo || hi - lo > 2) ? 2 : hi - lo) * dx;
                            for (int comp = 0; comp < 3; comp++) G[comp][dir] = h > 0 ? (u[3 * ib + comp] - u[3 * ia + comp]) * vs / h : 0;
                        }
                        double wx = G[2][1] - G[1][2], wy = G[0][2] - G[2][0], wz = G[1][0] - G[0][1], s2 = 0, o2 = 0;
                        for (int r = 0; r < 3; r++)
                            for (int c2 = 0; c2 < 3; c2++) {
                                double sym = 0.5 * (G[r][c2] + G[c2][r]), asy = 0.5 * (G[r][c2] - G[c2][r]);
                                s2 += sym * sym, o2 += asy * asy;
                            }
                        fw[o] = (float)sqrt(wx * wx + wy * wy + wz * wz), fq[o] = (float)(0.5 * (o2 - s2));
                        /* the sponges and the two planes of the inlet and outlet are boundary zones, not flow: no vortex
                         * cores are shown there (a uniform inlet meeting a floor makes a false one along their corner) */
                        int margin = (sp.sponge > 2 ? sp.sponge : 2);
                        if (X < margin || X >= sp.nx - margin) fq[o] = 0;
                    }
            LabBlock blk = {{ox, oy, oz}, 0, LAB_PLANE_XY, {box[0], box[1], box[2]}, {dx * stride, dx * stride, dx * stride}};
            lab_frame_begin(w, k * dt);
            lab_part_blocks(w, "flow", 1, &blk);
            lab_field(w, "speed", LAB_AT_CELL, on, fs), lab_field(w, "ux", LAB_AT_CELL, on, fx), lab_field(w, "uy", LAB_AT_CELL, on, fy);
            lab_field(w, "uz", LAB_AT_CELL, on, fz), lab_field(w, "vorticity", LAB_AT_CELL, on, fw), lab_field(w, "Q", LAB_AT_CELL, on, fq);
            if (tri_n > 0) lab_part_cells(w, "body", tri_nodes, tri, tri_n, LAB_TRI, tri_conn);
            if (SH) lab_part_cells(w, "sheet", sheet_nodes(SH), sheet_positions(SH), sh_nt, LAB_TRI, sh_tri);
            if (!lab_frame_end(w)) {
                snprintf(err, errlen, "cannot write a frame (disk full?)");
                ok = false;
            }
            if (!quiet)
                fprintf(stderr, "  t %.4g s  step %ld/%ld  Cx %.4f  Cy %+.4f  Cz %+.4f  %.0f MLUPS\n", k * dt, k, steps, cx, cy, cz,
                        (double)n * k / (wall_seconds() - t0) / 1e6);
        }
    }
    long done = G ? lbm3d_gpu_steps_done(G) : lbm3d_steps(L);
    double el = wall_seconds() - t0, mlups = (double)n * done / el / 1e6;
    /* the side force's dominant frequency: the peak of its spectrum between Strouhal 0.08 and 1 (below, slow drift) (a direct transform
     * of the settled record, mean removed), on whichever side force swings more */
    double freq = 0;
    if (hn > 64) {
        double my = 0, mz = 0, vy = 0, vz = 0;
        for (size_t i = 0; i < hn; i++) my += hy[i], mz += hz[i];
        my /= hn, mz /= hn;
        for (size_t i = 0; i < hn; i++) vy += (hy[i] - my) * (hy[i] - my), vz += (hz[i] - mz) * (hz[i] - mz);
        const double *h = vy >= vz ? hy : hz, m = vy >= vz ? my : mz;
        double best = 0;
        for (int k2 = 0; k2 < 2000; k2++) {
            double St = 0.08 + 0.92 * k2 / 1999, f = St * U / lref, re = 0, im = 0;
            size_t stride = hn > 20000 ? hn / 20000 : 1;
            for (size_t i = 0; i < hn; i += stride) {
                double ph = 2 * M_PI * f * i * dt;
                re += (h[i] - m) * cos(ph), im += (h[i] - m) * sin(ph);
            }
            if (re * re + im * im > best) best = re * re + im * im, freq = f;
        }
    }
    (void)cross_y, (void)cross_z, (void)first_cross, (void)last_cross;
    /* the sheet's flapping: the trailing edge's sideways swing, peak to peak, and its frequency from mean crossings */
    double flap_f = 0, flap_a = 0;
    if (sh_hn > 16) {
        double m = 0, lo = 1e300, hi = -1e300;
        for (size_t i = 0; i < sh_hn; i++) m += sh_hist[i], lo = fmin(lo, sh_hist[i]), hi = fmax(hi, sh_hist[i]);
        m /= sh_hn, flap_a = hi - lo;
        long first = -1, last = -1, nc = 0;
        for (size_t i = 1; i < sh_hn; i++)
            if (sh_hist[i - 1] < m && sh_hist[i] >= m) {
                if (first < 0) first = (long)i;
                last = (long)i, nc++;
            }
        if (nc > 1) flap_f = (nc - 1) / ((last - first) * dt);
    }
    snprintf(info->diagnostics, sizeof info->diagnostics,
             "{\"cells\":[%d,%d,%d],\"time_step_s\":%.6g,\"lattice_viscosity\":%.6g,\"relaxation_time\":%.6g,\"reynolds\":%.6g,"
             "\"reference_area_m2\":%.6g,\"reference_length_m\":%.6g,\"wall_links\":%ld,\"body_triangles\":%d,"
             "\"cx_mean\":%.6g,\"cy_mean\":%.6g,\"cz_mean\":%.6g,\"averaged_steps\":%ld,\"side_force_frequency_hz\":%.6g,\"strouhal\":%.6g,"
             "\"mlups\":%.4g,\"wall_s\":%.4g,\"engine\":\"%s\",\"sheet_flap_frequency_hz\":%.6g,\"sheet_flap_amplitude_m\":%.6g,"
             "\"sheet_strouhal\":%.6g,\"sheet_substeps\":%d,\"valve_opening_min\":%.4g,\"valve_opening_max\":%.4g,\"valve_opening_end\":%.4g}",
             sp.nx, sp.ny, sp.nz, dt, sp.nu, tau, U * lref / nu, area, lref, lbm3d_links(L), tri_n, navg ? cxs / navg : 0,
             navg ? cys / navg : 0, navg ? czs / navg : 0, navg, freq, freq * lref / U, mlups, el, G ? lbm3d_gpu_name(G) : "CPU, double", flap_f, flap_a,
             flap_f * flap_a / U, FS ? fsi_substeps(FS) : 0, open_min < 1e300 ? open_min : 0, open_max, open_end);
    info->frames = lab_frames_written(w);
    info->steps = done;
    if (!lab_close(w) && ok) {
        snprintf(err, errlen, "write error on %s", out);
        ok = false;
    }
    free(hy), free(hz), free(sh_hist);
    fsi_free(FS), sheet_free(SH), free(sh_tri);
    free(u), free(fs), free(fx), free(fy), free(fz), free(fw), free(fq), free(tri), free(tri_conn), pool_destroy(pool), lbm3d_gpu_free(G), lbm3d_free(L);
    return ok;
}
