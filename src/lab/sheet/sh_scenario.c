/* sh_scenario.c - a thin sheet (rubber, a membrane) that falls, drapes over bodies and settles (sheet.h), described in
 * SI (docs/lab/sheet.md): the material and its source, the sheet (a rectangle, its thickness, its cell), the bodies
 * (labshape.h) and a floor it touches, friction, damping and the run.
 *
 * Written per frame: the sheet as triangles with, at its nodes, the stretch (the largest principal stretch of the
 * triangles around a node, 1 at rest) and the speed; the bodies and the floor. The run record gives the energies and
 * the largest stretch. */
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
#include "sheet.h"

typedef struct {
    LabShapes *S;
    double floor_z;
    bool floor;
} World;

static double sdf_world(const double p[3], void *ctx) {
    World *W = ctx;
    double d = W->S->n ? labshape_sdf(W->S, p) : 1e30;
    return W->floor ? fmin(d, p[2] - W->floor_z) : d;
}

bool lab_run_sheet(const JsonValue *root, const char *out, bool quiet, LabRunInfo *info, char *err, size_t errlen) {
    const JsonValue *mat = json_get(root, "material"), *sh = json_get(root, "sheet"), *run = json_get(root, "run"), *fl = json_get(root, "floor");
    double mu = json_get_num(mat, "shear_modulus_pa", -1), rho = json_get_num(mat, "density_kg_m3", -1);
    double H = json_get_num(sh, "thickness_m", -1), cell = json_get_num(sh, "cell_m", -1), size[2], c0[3];
    double end = json_get_num(run, "end_s", -1);
    int frames = (int)json_get_int(run, "frames", 60);
    if (!json_get_str(mat, "source", NULL) || !(mu > 0) || !(rho > 0) || !(H > 0) || !(cell > 0) || !(end > 0) || frames < 1 ||
        !json_get_numbers(json_get(sh, "size_m"), size, 2) || !json_get_numbers(json_get(sh, "centre_m"), c0, 3) || !(size[0] > 0) ||
        !(size[1] > 0)) {
        snprintf(err, errlen, "sheet: material {shear_modulus_pa, density_kg_m3, source}, sheet {size_m [x, y], centre_m, thickness_m, "
                              "cell_m} and run {end_s, frames} are required");
        return false;
    }
    int nx = (int)lround(size[0] / cell) + 1, ny = (int)lround(size[1] / cell) + 1;
    if ((double)nx * ny > 4e6) {
        snprintf(err, errlen, "sheet: %d x %d nodes is more than this build runs (4 million)", nx, ny);
        return false;
    }
    LabShapes shapes;
    if (!labshape_parse(json_get(root, "bodies"), &shapes, err, errlen)) return false;
    World W = {&shapes, fl ? json_get_num(fl, "z_m", 0) : 0, fl != NULL};
    /* the rectangle: right triangles whose diagonals alternate cell by cell, so that no direction is favoured */
    int nn = nx * ny, nt = 2 * (nx - 1) * (ny - 1);
    double *X = malloc(3 * (size_t)nn * sizeof(double));
    int *T = malloc(3 * (size_t)nt * sizeof(int)), k = 0;
    if (!X || !T) {
        free(X), free(T);
        snprintf(err, errlen, "sheet: out of memory");
        return false;
    }
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++) {
            double *p = &X[3 * (j * nx + i)];
            p[0] = c0[0] - 0.5 * size[0] + size[0] * i / (nx - 1), p[1] = c0[1] - 0.5 * size[1] + size[1] * j / (ny - 1), p[2] = c0[2];
        }
    for (int j = 0; j + 1 < ny; j++)
        for (int i = 0; i + 1 < nx; i++) {
            int a = j * nx + i, b = a + 1, c = a + nx, d = c + 1;
            if ((i + j) & 1) T[k++] = a, T[k++] = b, T[k++] = c, T[k++] = b, T[k++] = d, T[k++] = c;
            else T[k++] = a, T[k++] = b, T[k++] = d, T[k++] = a, T[k++] = d, T[k++] = c;
        }
    SheetSpec sp = {.shear_modulus = mu, .thickness = H, .density = rho, .bending_scale = 1,
                    .gravity = json_get_num(root, "gravity_m_s2", 9.80665), .damping = json_get_num(root, "damping_1_s", 0),
                    .friction = json_get_num(root, "friction", 0), .contact_distance = json_get_num(sh, "contact_distance_m", 0),
                    .viscosity = json_get_num(mat, "viscosity_pa_s", 0), .self_contact = json_get_num(sh, "self_contact_m", 0)};
    Sheet *S = sheet_create(&sp, nn, X, nt, T, err, errlen);
    free(X);
    if (!S) {
        free(T);
        return false;
    }
    sheet_set_bodies(S, sdf_world, &W);
    double dt = sheet_stable_dt(S);
    /* the bodies, drawn from their distance function; the floor as a square under everything */
    double lo[3] = {c0[0] - size[0], c0[1] - size[1], W.floor_z - 0.01}, hi[3] = {c0[0] + size[0], c0[1] + size[1], c0[2] + 0.5 * size[0]};
    int nbt = 0, nbn = 0; /* a body drawn at a hundredth of the sheet's size, its nodes shared */
    double *soup = shapes.n ? labshape_mesh(&shapes, lo, hi, 0.01 * fmax(size[0], size[1]), &nbt) : NULL, *btri = NULL;
    int *bconn = NULL;
    if (nbt) nbn = labshape_weld(soup, nbt, &btri, &bconn);
    free(soup);
    if (nbt && nbn < 0) nbt = 0;
    double fx0 = c0[0] - 0.65 * size[0], fx1 = c0[0] + 0.65 * size[0], fy0 = c0[1] - 0.65 * size[1], fy1 = c0[1] + 0.65 * size[1];
    double fq[12] = {fx0, fy0, W.floor_z, fx1, fy0, W.floor_z, fx1, fy1, W.floor_z, fx0, fy1, W.floor_z};
    int fconn[4] = {0, 1, 2, 3};

    JsonValue *meta = json_object();
    bool mok = meta && json_set_string(meta, "domain", "sheet") && json_set_string(meta, "title", json_get_str(root, "title", "sheet")) &&
               json_set_string(meta, "model", "neo-Hookean membrane with mid-edge plate bending, frictional contact") &&
               json_set(meta, "scenario", json_clone(root));
    JsonValue *fields = mok ? json_set_object(meta, "fields") : NULL, *looks = mok ? json_set_object(meta, "looks") : NULL;
    mok = mok && fields && looks && json_set_string(fields, "stretch", "1") && json_set_string(fields, "speed", "m/s") &&
          json_set_string(looks, "body", json_get_str(root, "body_look", "steel")) && json_set_string(looks, "floor", "steel");
    char *hdr = mok ? json_dump(meta, 0, NULL, NULL) : NULL;
    json_free(meta);
    LabWriter *w = hdr ? lab_create(out, hdr, err, errlen) : NULL;
    free(hdr);
    double *lam = malloc((size_t)nt * sizeof(double)), *ns = malloc((size_t)nn * sizeof(double)), *spd = malloc((size_t)nn * sizeof(double));
    int *cnt = malloc((size_t)nn * sizeof(int));
    ThreadPool *pool = pool_create(cpu_perf_count());
    bool ok = w && lam && ns && spd && cnt && pool && (!nbt || bconn);
    long steps = (long)ceil(end / dt), per = steps / frames > 0 ? steps / frames : 1;
    double lam_max = 1;
    clock_t cpu0 = clock();
    time_t wall0 = time(NULL);
    for (long s = 0; ok && s <= steps; s++) {
        if (s % per == 0 || s == steps) {
            const double *x = sheet_positions(S), *v = sheet_velocities(S);
            sheet_stretch(S, lam);
            memset(ns, 0, (size_t)nn * sizeof(double)), memset(cnt, 0, (size_t)nn * sizeof(int));
            for (int t = 0; t < nt; t++)
                for (int c = 0; c < 3; c++) ns[T[3 * t + c]] += lam[t], cnt[T[3 * t + c]]++;
            for (int i = 0; i < nn; i++) {
                ns[i] /= cnt[i] ? cnt[i] : 1, lam_max = fmax(lam_max, ns[i]);
                spd[i] = sqrt(v[3 * i] * v[3 * i] + v[3 * i + 1] * v[3 * i + 1] + v[3 * i + 2] * v[3 * i + 2]);
            }
            lab_frame_begin(w, s * dt);
            lab_part_cells(w, "sheet", nn, x, nt, LAB_TRI, T);
            lab_field_d(w, "stretch", LAB_AT_NODE, (size_t)nn, ns), lab_field_d(w, "speed", LAB_AT_NODE, (size_t)nn, spd);
            if (nbt) lab_part_cells(w, "body", nbn, btri, nbt, LAB_TRI, bconn);
            if (W.floor) lab_part_cells(w, "floor", 4, fq, 1, LAB_QUAD, fconn);
            if (!lab_frame_end(w)) {
                snprintf(err, errlen, "cannot write a frame (disk full?)");
                ok = false;
            }
            if (!quiet) {
                double em, eb, ek;
                sheet_energy(S, &em, &eb, &ek);
                fprintf(stderr, "  t %.4g s  step %ld/%ld  membrane %.4g J  bending %.4g J  kinetic %.4g J  %.0f s cpu, %.0f s\n", s * dt, s, steps, em, eb,
                        ek, (double)(clock() - cpu0) / CLOCKS_PER_SEC, difftime(time(NULL), wall0));
            }
        }
        if (s < steps) sheet_step(S, dt, pool);
    }
    double em, eb, ek;
    sheet_energy(S, &em, &eb, &ek);
    snprintf(info->diagnostics, sizeof info->diagnostics,
             "{\"nodes\":%d,\"triangles\":%d,\"time_step_s\":%.6g,\"membrane_j\":%.6g,\"bending_j\":%.6g,\"kinetic_j\":%.6g,\"largest_stretch\":%.6g}", nn,
             nt, dt, em, eb, ek, lam_max);
    info->frames = w ? lab_frames_written(w) : 0;
    info->steps = steps;
    if (w && !lab_close(w) && ok) {
        snprintf(err, errlen, "write error on %s", out);
        ok = false;
    }
    free(lam), free(ns), free(spd), free(cnt), free(T), free(btri), free(bconn);
    pool_destroy(pool), sheet_free(S);
    return ok;
}
