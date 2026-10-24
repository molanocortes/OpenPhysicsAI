/* euler3d_scenario.c - a supersonic or hypersonic body in three dimensions (euler3d.h), described in SI
 * (docs/lab/gas.md, "Bodies in 3D"): the gas and its source, the free stream (Mach number, pressure, temperature), the
 * box, the cell, the faces, the bodies (labshape.h), the run.
 *
 * Written per frame: density, pressure, temperature and Mach number on the grid, and the bodies' surface with the
 * pressure and temperature of the gas against it. The run record gives the aerodynamic coefficients from the surface
 * pressure (drag along the stream, lift across it in the x-z plane, their ratio), and the stagnation-point heat flux of
 * Sutton and Graves (1971) for air, q = 1.7415e-4 sqrt(rho / R_n) V^3, from the nose radius given; the solver itself is
 * inviscid and perfect-gas: it computes no heat flux, no dissociation, no radiation. */
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
#include "euler3d.h"

static double sdf_m(const double p[3], void *ctx) { return labshape_sdf(ctx, p); }

static int face_kind(const char *s) { return !strcmp(s, "inflow") ? E3_INFLOW : !strcmp(s, "symmetry") ? E3_SYMMETRY : E3_OUTFLOW; }

bool lab_run_compressible3d(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen) {
    const JsonValue *gas = json_get(root, "gas"), *fs = json_get(root, "freestream"), *run = json_get(root, "run"), *ref = json_get(root, "reference");
    const JsonValue *faces = json_get(root, "faces");
    double gamma = json_get_num(gas, "gamma", 1.4), Rg = json_get_num(gas, "gas_constant_j_kgk", -1);
    double M = json_get_num(fs, "mach", -1), pinf = json_get_num(fs, "pressure_pa", -1), Tinf = json_get_num(fs, "temperature_k", -1);
    double dx = json_get_num(root, "cell_m", -1), box[6], end = json_get_num(run, "end_s", -1);
    int frames = (int)json_get_int(run, "frames", 40), stride = (int)json_get_int(run, "output_stride", 1);
    if (!json_get_str(gas, "source", NULL) || !(Rg > 0) || !(gamma > 1) || !(M > 0) || !(pinf > 0) || !(Tinf > 0) || !(dx > 0) ||
        !json_get_numbers(json_get(root, "box_m"), box, 6) || !(end > 0) || frames < 1 || stride < 1) {
        snprintf(err, errlen, "compressible3d: gas {gamma, gas_constant_j_kgk, source}, freestream {mach, pressure_pa, temperature_k}, box_m, "
                              "cell_m and run {end_s, frames} are required");
        return false;
    }
    LabShapes S;
    if (!labshape_parse(json_get(root, "bodies"), &S, err, errlen)) return false;
    double rho = pinf / (Rg * Tinf), c = sqrt(gamma * pinf / rho), V = M * c, dir = json_get_num(fs, "direction_deg", 0) * M_PI / 180;
    static const char *FACE[6] = {"x_low", "x_high", "y_low", "y_high", "z_low", "z_high"};
    static const char *DEF[6] = {"inflow", "outflow", "outflow", "outflow", "outflow", "outflow"};
    Euler3DSpec sp = {.nx = (int)lround((box[3] - box[0]) / dx), .ny = (int)lround((box[4] - box[1]) / dx), .nz = (int)lround((box[5] - box[2]) / dx),
                      .dx = dx, .origin = {box[0], box[1], box[2]}, .gamma = gamma, .rho_inf = rho, .u_inf = {V * cos(dir), 0, V * sin(dir)},
                      .p_inf = pinf, .cfl = json_get_num(run, "cfl", 0.4)};
    for (int f = 0; f < 6; f++) sp.face[f] = face_kind(json_get_str(faces, FACE[f], DEF[f]));
    if ((double)sp.nx * sp.ny * sp.nz > 1.2e7) {
        snprintf(err, errlen, "compressible3d: %d x %d x %d cells is beyond this machine's memory for the solver (12 million)", sp.nx, sp.ny, sp.nz);
        return false;
    }
    Euler3D *E = euler3d_create(&sp, S.n ? sdf_m : NULL, &S, err, errlen);
    if (!E) return false;
    const double bmax[3] = {box[3], box[4], box[5]};
    int ntri = 0;
    double *tri = labshape_mesh(&S, box, bmax, dx, &ntri);
    double area = json_get_num(ref, "area_m2", -1), Rn = json_get_num(ref, "nose_radius_m", -1);
    if (!(area > 0)) area = S.n ? labshape_frontal_area(&S, box, bmax, 0.5 * dx) : 1;

    JsonValue *meta = json_object();
    bool mok = meta && json_set_string(meta, "domain", "gas") && json_set_string(meta, "title", json_get_str(root, "title", "compressible flow in 3D")) &&
               json_set_string(meta, "model", "3D Euler, MUSCL-HLLC/HLL, ghost-cell bodies; perfect gas, inviscid") && json_set(meta, "scenario", json_clone(root));
    JsonValue *fields = mok ? json_set_object(meta, "fields") : NULL;
    mok = mok && fields && json_set_string(fields, "rho", "kg/m^3") && json_set_string(fields, "p", "Pa") && json_set_string(fields, "T", "K") &&
          json_set_string(fields, "mach", "1");
    char *hdr = mok ? json_dump(meta, 0, NULL, NULL) : NULL;
    json_free(meta);
    LabWriter *w = hdr ? lab_create(out, hdr, err, errlen) : NULL;
    free(hdr);
    size_t n = (size_t)sp.nx * sp.ny * sp.nz;
    /* a symmetry plane at y low is written mirrored: the whole body and its flow, exact by the symmetry */
    const bool mir = sp.face[2] == E3_SYMMETRY;
    int ox = (sp.nx + stride - 1) / stride, oyh = (sp.ny + stride - 1) / stride, oy = mir ? 2 * oyh : oyh, oz = (sp.nz + stride - 1) / stride;
    size_t on = (size_t)ox * oy * oz;
    double *r = malloc(n * sizeof *r), *u = malloc(3 * n * sizeof *u), *p = malloc(n * sizeof *p), *np = malloc(3 * (size_t)(ntri ? ntri : 1) * sizeof *np);
    double *nT = malloc(3 * (size_t)(ntri ? ntri : 1) * sizeof *nT);
    float *fr = malloc(on * sizeof *fr), *fp = malloc(on * sizeof *fp), *fT = malloc(on * sizeof *fT), *fM = malloc(on * sizeof *fM);
    int *conn = malloc(6 * (size_t)(ntri ? ntri : 1) * sizeof *conn);
    double *mtri = malloc(18 * (size_t)(ntri ? ntri : 1) * sizeof *mtri), *mp = malloc(6 * (size_t)(ntri ? ntri : 1) * sizeof *mp);
    double *mT = malloc(6 * (size_t)(ntri ? ntri : 1) * sizeof *mT);
    ThreadPool *pool = pool_create(cpu_perf_count());
    bool ok = w && r && u && p && np && nT && fr && fp && fT && fM && conn && pool && mtri && mp && mT;
    if (!ok && w) snprintf(err, errlen, "compressible3d: out of memory for the output");
    for (int i = 0; ok && i < 6 * ntri; i++) conn[i] = i;
    double t0 = (double)clock() / CLOCKS_PER_SEC, next = end / frames, Cd = 0, Cl = 0, Cdn = 0, Cln = 0;
    long steps = 0;
    while (ok && euler3d_time(E) < end - 1e-15) {
        if (!(euler3d_step(E, pool) > 0)) {
            snprintf(err, errlen, "compressible3d: the state went invalid at t = %.4g s (step %ld)", euler3d_time(E), steps);
            ok = false;
            break;
        }
        steps++;
        if (euler3d_time(E) + 1e-15 < next && euler3d_time(E) < end - 1e-15) continue;
        next += end / frames;
        euler3d_fields(E, r, u, p);
        for (int z = 0; z < oz; z++)
            for (int y = 0; y < oy; y++)
                for (int x = 0; x < ox; x++) {
                    int ys = mir ? (y < oyh ? oyh - 1 - y : y - oyh) : y; /* the mirrored half first, then the computed one */
                    size_t i = (size_t)(x * stride) + (size_t)sp.nx * ((size_t)(ys * stride) + (size_t)sp.ny * (z * stride)), o = (size_t)x + (size_t)ox * ((size_t)y + (size_t)oy * z);
                    double T = p[i] / (r[i] * Rg), sp2 = u[3 * i] * u[3 * i] + u[3 * i + 1] * u[3 * i + 1] + u[3 * i + 2] * u[3 * i + 2];
                    fr[o] = (float)r[i], fp[o] = (float)p[i], fT[o] = (float)T, fM[o] = (float)sqrt(sp2 / (gamma * p[i] / r[i]));
                }
        /* the surface: the gas's pressure and temperature at each vertex, and the force from the pressure */
        double F[3] = {0, 0, 0}, Fn[3] = {0, 0, 0}; /* Fn: modified Newtonian theory on the same surface, an independent estimate */
        const double pit = pow((gamma + 1) * (gamma + 1) * M * M / (4 * gamma * M * M - 2 * (gamma - 1)), gamma / (gamma - 1)) * (1 - gamma + 2 * gamma * M * M) / (gamma + 1);
        const double cpmax = (pit - 1) / (0.5 * gamma * M * M), vhat[3] = {cos(dir), 0, sin(dir)};
        for (int t = 0; t < ntri; t++) {
            double *a = &tri[9 * t], *b = a + 3, *cc = a + 6, e1[3], e2[3], nrm[3], cen[3];
            for (int k = 0; k < 3; k++) e1[k] = b[k] - a[k], e2[k] = cc[k] - a[k], cen[k] = (a[k] + b[k] + cc[k]) / 3;
            nrm[0] = 0.5 * (e1[1] * e2[2] - e1[2] * e2[1]), nrm[1] = 0.5 * (e1[2] * e2[0] - e1[0] * e2[2]), nrm[2] = 0.5 * (e1[0] * e2[1] - e1[1] * e2[0]);
            double g[3], h = 0.25 * dx; /* outward, by the distance's gradient */
            for (int k = 0; k < 3; k++) {
                double pa[3] = {cen[0], cen[1], cen[2]}, pb[3] = {cen[0], cen[1], cen[2]};
                pa[k] += h, pb[k] -= h;
                g[k] = labshape_sdf(&S, pa) - labshape_sdf(&S, pb);
            }
            if (nrm[0] * g[0] + nrm[1] * g[1] + nrm[2] * g[2] < 0)
                for (int k = 0; k < 3; k++) nrm[k] = -nrm[k];
            /* one probe per triangle, just off its centre into the gas (per-vertex probes speckled the picture) */
            double gl = sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]), qc[3];
            for (int k = 0; k < 3; k++) qc[k] = cen[k] + (gl > 0 ? 0.75 * dx * g[k] / gl : 0);
            double pc = euler3d_probe_p(E, qc);
            if (!isfinite(pc)) pc = pinf;
            np[t] = pc;
            for (int k = 0; k < 3; k++) F[k] -= (pc - pinf) * nrm[k];
            double al = sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]), cth = al > 0 ? -(nrm[0] * vhat[0] + nrm[1] * vhat[1] + nrm[2] * vhat[2]) / al : 0;
            if (cth > 0) /* windward: Cp = Cp_max sin^2 of the surface's angle to the stream; leeward: nothing */
                for (int k = 0; k < 3; k++) Fn[k] -= cpmax * cth * cth * 0.5 * rho * V * V * nrm[k];
        }
        /* the surface temperature from the gas: the stagnation temperature scaled by the local pressure along an isentrope
         * from the stagnation point (the gas at the wall of an inviscid solution) */
        double pmax = 0;
        for (int i = 0; i < ntri; i++) pmax = fmax(pmax, np[i]);
        double T0 = Tinf * (1 + 0.5 * (gamma - 1) * M * M);
        for (int i = 0; i < ntri; i++) nT[i] = pmax > 0 ? T0 * pow(np[i] / pmax, (gamma - 1) / gamma) : Tinf;
        /* a symmetry plane that cuts the bodies holds half of them: the forces are doubled for each such plane */
        double mirror = 1;
        for (int f = 0; f < 6; f++)
            if (sp.face[f] == E3_SYMMETRY) {
                int a = f / 2;
                double plane = f % 2 ? box[3 + a] : box[a];
                for (int b2 = 0; b2 < S.n; b2++)
                    if (S.s[b2].aabb_lo[a] < plane - 1e-12 && S.s[b2].aabb_hi[a] > plane + 1e-12) { mirror *= 2; break; }
            }
        for (int k = 0; k < 3; k++) F[k] *= mirror, Fn[k] *= mirror;
        double q = 0.5 * rho * V * V * area, ex[3] = {cos(dir), 0, sin(dir)}, ez[3] = {-sin(dir), 0, cos(dir)};
        Cd = (F[0] * ex[0] + F[2] * ex[2]) / q, Cl = (F[0] * ez[0] + F[2] * ez[2]) / q;
        Cdn = (Fn[0] * ex[0] + Fn[2] * ex[2]) / q, Cln = (Fn[0] * ez[0] + Fn[2] * ez[2]) / q;
        LabBlock blk = {{ox, oy, oz}, 0, LAB_PLANE_XY, {box[0], mir ? box[1] - oyh * dx * stride : box[1], box[2]}, {dx * stride, dx * stride, dx * stride}};
        lab_frame_begin(w, euler3d_time(E));
        lab_part_blocks(w, "gas", 1, &blk);
        lab_field(w, "rho", LAB_AT_CELL, on, fr), lab_field(w, "p", LAB_AT_CELL, on, fp), lab_field(w, "T", LAB_AT_CELL, on, fT);
        lab_field(w, "mach", LAB_AT_CELL, on, fM);
        if (ntri) {
            int nt2 = mir ? 2 * ntri : ntri;
            memcpy(mtri, tri, 9 * (size_t)ntri * sizeof *tri), memcpy(mp, np, (size_t)ntri * sizeof *np), memcpy(mT, nT, (size_t)ntri * sizeof *nT);
            if (mir) {
                for (int i = 0; i < 3 * ntri; i++) {
                    double *a = &mtri[9 * (size_t)ntri + 3 * (size_t)i];
                    a[0] = tri[3 * i], a[1] = 2 * box[1] - tri[3 * i + 1], a[2] = tri[3 * i + 2];
                }
                for (int i = 0; i < ntri; i++) mp[ntri + i] = np[i], mT[ntri + i] = nT[i];
            }
            lab_part_cells(w, "body", 3 * nt2, mtri, nt2, LAB_TRI, conn);
            lab_field_d(w, "p", LAB_AT_CELL, (size_t)nt2, mp), lab_field_d(w, "T", LAB_AT_CELL, (size_t)nt2, mT);
        }
        if (!lab_frame_end(w)) {
            snprintf(err, errlen, "cannot write a frame (disk full?)");
            ok = false;
        }
        if (!quiet)
            fprintf(stderr, "  t %.4g s  step %ld  Cd %.4f  Cl %+.4f  L/D %+.4f  %.0f s cpu\n", euler3d_time(E), steps, Cd, Cl, Cd != 0 ? Cl / Cd : 0,
                    (double)clock() / CLOCKS_PER_SEC - t0);
    }
    double qsg = Rn > 0 ? 1.7415e-4 * sqrt(rho / Rn) * V * V * V : 0;
    snprintf(info->diagnostics, sizeof info->diagnostics,
             "{\"cells\":[%d,%d,%d],\"freestream_density_kg_m3\":%.6g,\"freestream_speed_m_s\":%.6g,\"stagnation_temperature_k\":%.6g,"
             "\"reference_area_m2\":%.6g,\"cd\":%.6g,\"cl\":%.6g,\"lift_to_drag\":%.6g,\"newtonian_cd\":%.6g,\"newtonian_cl\":%.6g,\"sutton_graves_stagnation_heat_flux_w_m2\":%.6g,"
             "\"surface_triangles\":%d,\"steps\":%ld}",
             sp.nx, sp.ny, sp.nz, rho, V, Tinf * (1 + 0.5 * (gamma - 1) * M * M), area, Cd, Cl, Cd != 0 ? Cl / Cd : 0, Cdn, Cln, qsg, ntri, steps);
    info->frames = w ? lab_frames_written(w) : 0;
    info->steps = steps;
    if (w && !lab_close(w) && ok) {
        snprintf(err, errlen, "write error on %s", out);
        ok = false;
    }
    free(r), free(u), free(p), free(np), free(nT), free(fr), free(fp), free(fT), free(fM), free(conn), free(tri), free(mtri), free(mp), free(mT);
    pool_destroy(pool), euler3d_free(E);
    return ok;
}
