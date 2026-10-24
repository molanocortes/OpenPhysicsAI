/* lbmbench - solver throughput benchmark and physical validation (drag on a sphere)
 *   lbmbench bench [nx ny nz steps]
 *   lbmbench sphere [Re D steps reg|bgk cs sponge(0/1) ramp_steps width_in_D]
 *   lbmbench noise [nx ny nz steps tau obstacle(0/1) reg|bgk cs]
 */
#include "../src/common.h"
#include "../src/lbm.h"
#include "../src/threads.h"

#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#endif

static uint64_t make_sphere(uint8_t *s, int nx, int ny, int nz, float cx, float cy, float cz, float r,
                            uint64_t *frontal) {
    uint64_t n = 0;
    uint8_t *col = calloc((size_t)ny * (size_t)nz, 1);
    for (int k = 0; k < nz; k++)
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                float dx = i + 0.5f - cx, dy = j + 0.5f - cy, dz = k + 0.5f - cz;
                bool in = dx * dx + dy * dy + dz * dz <= r * r;
                s[(size_t)i + (size_t)nx * ((size_t)j + (size_t)ny * (size_t)k)] = in;
                if (in) n++, col[(size_t)j + (size_t)ny * (size_t)k] = 1;
            }
    *frontal = 0;
    for (size_t i = 0; i < (size_t)ny * (size_t)nz; i++) *frontal += col[i];
    free(col);
    return n;
}

/* Exact wall fractions for a sphere: the entering root of |o + t d - c| = r along each boundary link, looked for up
 * to twice the link length; misses keep halfway bounce-back. Returns the number of links that cross the sphere. */
static size_t sphere_deltas(Lbm *L, float cx, float cy, float cz, float r) {
    float *delta = malloc(L->nlinks * sizeof(float));
    if (!delta) return 0;
    size_t hit = 0;
    for (size_t n = 0; n < L->nlinks; n++) {
        float seg[6];
        lbm_link_segment(L, n, seg);
        const double ox = seg[0] - cx, oy = seg[1] - cy, oz = seg[2] - cz;
        const double a = (double)seg[3] * seg[3] + (double)seg[4] * seg[4] + (double)seg[5] * seg[5];
        const double bq = 2.0 * (ox * seg[3] + oy * seg[4] + oz * seg[5]), c = ox * ox + oy * oy + oz * oz - (double)r * r;
        const double disc = bq * bq - 4.0 * a * c;
        double t = -1.0;
        if (disc >= 0.0) t = (-bq - sqrt(disc)) / (2.0 * a);
        if (t > 0.0 && t <= 2.0) delta[n] = (float)t, hit++;
        else delta[n] = 0.5f;
    }
    lbm_set_link_deltas(L, delta);
    return hit;
}

static bool env_curved(void) { return getenv("LBM_CURVED") && atoi(getenv("LBM_CURVED")) != 0; }

static LbmConfig default_cfg(float tau, float u, LbmCollision coll, float cs) {
    LbmConfig c;
    memset(&c, 0, sizeof c);
    c.tau0 = tau;
    c.u_in = u;
    c.collision = coll;
    c.smagorinsky = cs;
    for (int s = 0; s < 4; s++) c.wall[s] = LBM_WALL_SLIP;
    /* collision-model experiments without recompiling: LBM_SIGMA (hybrid weight), LBM_TAUB (bulk relaxation time) */
    const char *sg = getenv("LBM_SIGMA"), *tb = getenv("LBM_TAUB");
    c.hrr_sigma = sg ? (float)atof(sg) : 0.0f;
    c.tau_bulk = tb ? (float)atof(tb) : 0.0f;
    return c;
}

static void bench(int nx, int ny, int nz, int steps) {
    int counts[4] = {1, cpu_perf_count(), cpu_count() - 1, cpu_count()};
    printf("grid %dx%dx%d = %.2fM cells, %.0f MB\n", nx, ny, nz, nx * (double)ny * nz / 1e6,
           (double)lbm_bytes_estimate(nx, ny, nz) / 1048576.0);
    for (int ci = 0; ci < 4; ci++) {
        ThreadPool *pool = pool_create(counts[ci]);
        Lbm L;
        if (!lbm_create(&L, nx, ny, nz, pool)) exit(1);
        uint8_t *solid = calloc(L.ncells, 1);
        uint64_t frontal;
        make_sphere(solid, nx, ny, nz, nx * 0.25f, ny * 0.5f, nz * 0.5f, ny * 0.12f, &frontal);
        lbm_set_solids(&L, solid);
        lbm_reset(&L, 0.05f);
        for (int coll = 0; coll < 3; coll++) {
            LbmConfig c = default_cfg(0.5005f, 0.05f, (LbmCollision)coll, 0.16f);
            c.sponge_in = 6, c.sponge_out = nx / 12, c.sponge_tau = 0.8f;
            for (int s = 0; s < 3; s++) lbm_step(&L, &c, NULL);
            double t0 = now_seconds();
            for (int s = 0; s < steps; s++) lbm_step(&L, &c, NULL);
            double dt = now_seconds() - t0;
            printf("  threads %d  %-4s  %7.1f MLUPS  (%.2f ms/step)\n", counts[ci], coll == 2 ? "RR3" : coll ? "REG" : "BGK",
                   (double)L.ncells * steps / dt / 1e6, dt / steps * 1e3);
        }
        free(solid);
        lbm_destroy(&L);
        pool_destroy(pool);
    }
}

static void sphere(double Re, int D, int steps, LbmCollision coll, float cs, bool sponge, int ramp, int width) {
    int nx = 8 * D, ny = width * D, nz = width * D;
    float u = 0.05f;
    double nu = u * D / Re;
    float tau = (float)(3.0 * nu + 0.5);
    ThreadPool *pool = pool_create(cpu_count());
    Lbm L;
    if (!lbm_create(&L, nx, ny, nz, pool)) exit(1);
    uint8_t *solid = calloc(L.ncells, 1);
    uint64_t frontal;
    uint64_t ns = make_sphere(solid, nx, ny, nz, 2.5f * D, ny * 0.5f, nz * 0.5f, D * 0.5f, &frontal);
    lbm_set_solids(&L, solid);
    if (env_curved())
        printf("curved walls: %zu of %zu links cross the sphere\n", sphere_deltas(&L, 2.5f * D, ny * 0.5f, nz * 0.5f, D * 0.5f),
               L.nlinks);
    lbm_reset(&L, ramp > 0 ? 0.0f : u);
    LbmOutput out = {malloc(L.ncells * 4), malloc(L.ncells * 4), malloc(L.ncells * 4), malloc(L.ncells * 4)};
    double A = M_PI * D * D / 4.0;
    printf("sphere Re=%.1f D=%d grid %dx%dx%d tau=%.5f %s cs=%.2f sponge=%d ramp=%d blockage=%.2f%% links=%zu\n", Re,
           D, nx, ny, nz, tau, coll ? "REG" : "BGK", cs, sponge, ramp, 100.0 * A / (ny * nz), L.nlinks);
    (void)ns, (void)frontal;
    LbmConfig c = default_cfg(tau, u, coll, cs);
    if (sponge) c.sponge_in = D / 2, c.sponge_out = D, c.sponge_tau = 0.8f, c.sponge_sigma = 0.2f;
    double win_fx = 0, fx_acc = 0, fy_acc = 0;
    int win_n = 0, acc = 0;
    double t0 = now_seconds();
    int avg_from = (int)(steps * 0.6);
    for (int s = 1; s <= steps; s++) {
        float r = ramp > 0 ? CLAMP((float)s / (float)ramp, 0.0f, 1.0f) : 1.0f;
        c.u_in = u * r * r * (3.0f - 2.0f * r);
        bool want = s % 500 == 0;
        lbm_step(&L, &c, want ? &out : NULL);
        win_fx += L.force[0], win_n++;
        if (s > avg_from) fx_acc += L.force[0], fy_acc += L.force[1], acc++;
        if (want) {
            printf("  step %6d  Cd(500-step mean)=%.4f  Cd(inst)=%.4f  umax=%.4f  rho=[%.4f %.4f]%s\n", s,
                   win_fx / win_n / (0.5 * u * u * A), L.force[0] / (0.5 * u * u * A), L.umax, L.rho_min, L.rho_max,
                   L.unstable ? "  UNSTABLE" : "");
            win_fx = 0, win_n = 0;
            if (L.unstable) break;
        }
    }
    double dt = now_seconds() - t0;
    double cd = fx_acc / acc / (0.5 * u * u * A);
    double cd_ref = 24.0 / Re * (1.0 + 0.15 * pow(Re, 0.687));
    printf("mean Cd (last 40%%) = %.4f  |  Schiller-Naumann (unbounded) %.4f  |  Cl = %.5f  |  %.1f MLUPS\n", cd, cd_ref,
           fy_acc / acc / (0.5 * u * u * A), (double)L.ncells * steps / dt / 1e6);
    free(out.rho), free(out.ux), free(out.uy), free(out.uz);
    free(solid);
    lbm_destroy(&L);
    pool_destroy(pool);
}

/* Free-stream noise. Ahead of a body in a uniform inflow without turbulence nothing should move. Uses the app's
 * sponge and wall settings, starts from rest with the app's ramp and (optionally) moves a sphere by one cell at
 * half time, as dragging the model does. Reported over the upstream region every 250 steps:
 *   |u-U|/U  RMS deviation from the inflow      du50/U  RMS change over the last 50 steps
 *   rho'     RMS density fluctuation            grid-x / grid-z  mean 2-cell (checkerboard) content of ux */
static void noise(int nx, int ny, int nz, int steps, double tau, bool obstacle, LbmCollision coll, float cs) {
    const float u = 0.08f;
    ThreadPool *pool = pool_create(cpu_count());
    Lbm L;
    if (!lbm_create(&L, nx, ny, nz, pool)) exit(1);
    uint8_t *solid = calloc(L.ncells, 1);
    uint64_t frontal = 0;
    const int D = ny / 4;
    const float cx = 0.35f * nx;
    if (obstacle) make_sphere(solid, nx, ny, nz, cx, ny * 0.5f, nz * 0.5f, D * 0.5f, &frontal);
    lbm_set_solids(&L, solid);
    if (obstacle && env_curved()) sphere_deltas(&L, cx, ny * 0.5f, nz * 0.5f, D * 0.5f);
    lbm_reset(&L, 0.0f);
    LbmConfig c = default_cfg((float)tau, u, coll, cs);
    c.sponge_in = MAXI(3, nx / 40), c.sponge_out = MAXI(6, nx / 12), c.sponge_tau = 0.8f, c.sponge_sigma = 0.2f;
    const size_t n = L.ncells, sxy = (size_t)nx * (size_t)ny;
    LbmOutput out = {malloc(n * 4), malloc(n * 4), malloc(n * 4), malloc(n * 4)};
    float *pux = malloc(n * 4), *puy = malloc(n * 4), *puz = malloc(n * 4);
    const int x0 = c.sponge_in + 4, x1 = obstacle ? (int)(cx - D) : nx - c.sponge_out - 4;
    printf("noise %dx%dx%d tau=%.6f %s cs=%.2f sigma=%.3f tau_bulk=%.2f obstacle=%d upstream region x %d..%d\n", nx, ny,
           nz, tau, coll == LBM_BGK ? "BGK" : coll == LBM_RECURSIVE ? "RR3" : "REG", cs, c.hrr_sigma, c.tau_bulk, obstacle,
           x0, x1);
    printf("   step    |u-U|/U     du50/U       rho'      grid-x      grid-z     umax\n");
    const int ramp = 600;
    const double t0 = now_seconds();
    for (int s = 1; s <= steps; s++) {
        const float r = CLAMP((float)s / ramp, 0.0f, 1.0f);
        c.u_in = u * r * r * (3.0f - 2.0f * r);
        if (obstacle && s == steps / 2) {
            make_sphere(solid, nx, ny, nz, cx, ny * 0.5f + 1.0f, nz * 0.5f, D * 0.5f, &frontal);
            lbm_set_solids(&L, solid);
            if (env_curved()) sphere_deltas(&L, cx, ny * 0.5f + 1.0f, nz * 0.5f, D * 0.5f);
        }
        const bool meas = s % 250 == 0, prev = (s + 50) % 250 == 0;
        lbm_step(&L, &c, (meas || prev) ? &out : NULL);
        if (prev) memcpy(pux, out.ux, n * 4), memcpy(puy, out.uy, n * 4), memcpy(puz, out.uz, n * 4);
        if (!meas) continue;
        double su = 0, sd = 0, sr = 0, rm = 0, gx = 0, gz = 0;
        uint64_t cnt = 0;
        for (int k = 4; k < nz - 4; k++)
            for (int j = 4; j < ny - 4; j++)
                for (int i = x0; i < x1; i++) {
                    const size_t id = (size_t)i + (size_t)nx * ((size_t)j + (size_t)ny * (size_t)k);
                    const double ex = out.ux[id] - c.u_in, ey = out.uy[id], ez = out.uz[id];
                    const double dx = out.ux[id] - pux[id], dy = out.uy[id] - puy[id], dz = out.uz[id] - puz[id];
                    su += ex * ex + ey * ey + ez * ez;
                    sd += dx * dx + dy * dy + dz * dz;
                    rm += out.rho[id], sr += (double)out.rho[id] * out.rho[id];
                    gx += fabs(out.ux[id] - 0.5 * ((double)out.ux[id - 1] + out.ux[id + 1]));
                    gz += fabs(out.ux[id] - 0.5 * ((double)out.ux[id - sxy] + out.ux[id + sxy]));
                    cnt++;
                }
        const double U = MAXI(c.u_in, 1e-6f), mr = rm / (double)cnt;
        printf("  %5d   %.3e   %.3e   %.3e   %.3e   %.3e   %.4f%s\n", s, sqrt(su / cnt) / U, sqrt(sd / cnt) / U,
               sqrt(MAXI(sr / cnt - mr * mr, 0.0)), gx / cnt / U, gz / cnt / U, L.umax, L.unstable ? "  UNSTABLE" : "");
        fflush(stdout);
        if (L.unstable) break;
    }
    printf("  %.1f MLUPS\n", (double)n * steps / (now_seconds() - t0) / 1e6);
    free(out.rho), free(out.ux), free(out.uy), free(out.uz), free(pux), free(puy), free(puz);
    free(solid);
    lbm_destroy(&L);
    pool_destroy(pool);
}

int main(int argc, char **argv) {
#ifdef __APPLE__
    /* match the application's scheduling: otherwise a benchmark started from a background shell runs on the
     * efficiency cores only and reports a fraction of the real throughput */
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    printf("cores: %d total, %d performance\n", cpu_count(), cpu_perf_count());
    if (argc >= 2 && !strcmp(argv[1], "noise")) {
        int nx = argc > 2 ? atoi(argv[2]) : 192, ny = argc > 3 ? atoi(argv[3]) : 64, nz = argc > 4 ? atoi(argv[4]) : 96;
        int steps = argc > 5 ? atoi(argv[5]) : 4000;
        double tau = argc > 6 ? atof(argv[6]) : 0.50001;
        bool obstacle = argc > 7 ? atoi(argv[7]) != 0 : true;
        LbmCollision coll = argc <= 8 ? LBM_REGULARIZED : !strcmp(argv[8], "bgk") ? LBM_BGK
                          : !strcmp(argv[8], "rr") ? LBM_RECURSIVE : LBM_REGULARIZED;
        float cs = argc > 9 ? (float)atof(argv[9]) : 0.14f;
        noise(nx, ny, nz, steps, tau, obstacle, coll, cs);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "sphere")) {
        double Re = argc > 2 ? atof(argv[2]) : 100;
        int D = argc > 3 ? atoi(argv[3]) : 20;
        int steps = argc > 4 ? atoi(argv[4]) : 6000;
        LbmCollision coll = argc <= 5 ? LBM_REGULARIZED : !strcmp(argv[5], "bgk") ? LBM_BGK
                          : !strcmp(argv[5], "rr") ? LBM_RECURSIVE : LBM_REGULARIZED;
        float cs = argc > 6 ? (float)atof(argv[6]) : 0.0f;
        bool sponge = argc > 7 ? atoi(argv[7]) != 0 : true;
        int ramp = argc > 8 ? atoi(argv[8]) : 800;
        int width = argc > 9 ? atoi(argv[9]) : 4;
        sphere(Re, D, steps, coll, cs, sponge, ramp, width);
        return 0;
    }
    int nx = 192, ny = 96, nz = 96, steps = 100;
    if (argc >= 6 && !strcmp(argv[1], "bench")) nx = atoi(argv[2]), ny = atoi(argv[3]), nz = atoi(argv[4]), steps = atoi(argv[5]);
    bench(nx, ny, nz, steps);
    return 0;
}
