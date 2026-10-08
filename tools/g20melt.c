/* g20melt - one laser track through the melt solver, measured as a metallographic cross-section measures it
 * (GOALS.md G20 step 2): the scenario of the melt domain (docs/lab/melt.md, with block.held_faces and block.mirror_y)
 * plus "measure": {"from_m", "to_m"}, the stretch of track whose cross-sections are read. No frames are written.
 * Prints one JSON line: the mean width and depth of the sections by melt_track_section (melttest M11), their largest,
 * the whole-cell values of melt_track_size, the pool's length when the run ends, the energy balance and the cost.
 *
 *   make build/g20melt && ./build/g20melt case.json */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/core/json.h"
#include "../src/lab/melt/melt.h"
#include "../src/lab/melt/mt_scenario.h"
#include "../src/threads.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: g20melt case.json\n");
        return 2;
    }
    char err[512] = "";
    JsonError je;
    JsonValue *root = json_read_file(argv[1], 16u << 20, &je);
    if (!root) {
        fprintf(stderr, "g20melt: %s: line %d: %s\n", argv[1], je.line, je.message);
        return 2;
    }
    MeltSpec s;
    double end;
    const JsonValue *ms = json_get(root, "measure");
    double x0 = json_get_num(ms, "from_m", NAN), x1 = json_get_num(ms, "to_m", NAN);
    bool mirror = json_get_bool(json_get(root, "block"), "mirror_y", false);
    if (!melt_scenario_spec(root, &s, &end, err, sizeof err) || !(x1 > x0)) {
        fprintf(stderr, "g20melt: %s\n", err[0] ? err : "measure {from_m, to_m} is required");
        json_free(root);
        return 2;
    }
    Melt *M = melt_create(&s, err, sizeof err);
    if (!M) {
        fprintf(stderr, "g20melt: %s\n", err);
        json_free(root);
        return 2;
    }
    ThreadPool *pool = pool_create(cpu_perf_count());
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long steps = 0;
    double dt = melt_stable_dt(M);
    if (!s.flow) { /* a fixed step that ends the run exactly */
        long n = (long)ceil(end / dt);
        dt = end / n;
        for (long i = 0; i < n; i++) melt_step(M, dt, pool), steps++;
    } else
        while (melt_time(M) < end - 1e-15) {
            double d = fmin(melt_stable_dt(M), end - melt_time(M));
            melt_step(M, d, pool), steps++;
        }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double W = 0, D = 0, Wm = 0, Dm = 0, Wc = 0, Dc = 0, L = 0, Wp = 0, Dp = 0, ab, lo, ho;
    bool touch = false, got = melt_track_section(M, x0, x1, mirror, &W, &D, &Wm, &Dm, &touch);
    melt_track_size(M, x0, x1, &Wc, &Dc);
    melt_pool_size(M, &L, &Wp, &Dp);
    melt_energy(M, &ab, &lo, &ho);
    double stored = 0, H0 = melt_H_of_T(M, s.T0);
    const double *H = melt_enthalpy(M);
    for (size_t c = 0; c < melt_cells(M); c++) stored += H[c] - H0;
    stored *= s.h * s.h * s.h;
    printf("{\"melted\":%s,\"width_m\":%.9g,\"depth_m\":%.9g,\"width_max_m\":%.9g,\"depth_max_m\":%.9g,\"touches_a_face\":%s,"
           "\"whole_cell_width_m\":%.9g,\"whole_cell_depth_m\":%.9g,\"pool_length_at_end_m\":%.9g,\"absorbed_j\":%.9g,\"surface_loss_j\":%.9g,"
           "\"held_faces_out_j\":%.9g,\"stored_j\":%.9g,\"cells\":%zu,\"steps\":%ld,\"cell_m\":%.9g,\"seconds\":%.3f}\n",
           got ? "true" : "false", W, D, Wm, Dm, touch ? "true" : "false", mirror ? 2 * Wc : Wc, Dc, L, ab, lo, ho, stored, melt_cells(M), steps, s.h,
           (double)(t1.tv_sec - t0.tv_sec) + 1e-9 * (double)(t1.tv_nsec - t0.tv_nsec));
    melt_free(M);
    pool_destroy(pool);
    json_free(root);
    return got ? 0 : 1;
}
