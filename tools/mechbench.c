/* mechbench.c - throughput and memory of the rigid and flexible dynamics core (not a test; numbers depend on the machine) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/fem/hex8.h"
#include "../src/mech/contact.h"
#include "../src/mech/flexbody.h"
#include "../src/mech/mechsim.h"
#include "../src/mech/multibody.h"

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

static MbModelDef *chain(int n) { /* planar chain of n revolute links under gravity */
    MbModelDef *d = mbdef_new();
    double I[9] = {1e-4, 0, 0, 0, 1e-3, 0, 0, 0, 1e-3};
    int parent = -1;
    for (int i = 0; i < n; i++) {
        char nm[16];
        snprintf(nm, sizeof nm, "l%d", i);
        int b = mbdef_add_body(d, nm, 0.3, (double[3]){0.05, 0, 0}, I);
        snprintf(nm, sizeof nm, "j%d", i);
        int j = mbdef_add_joint(d, nm, MB_REVOLUTE, parent, b);
        mv3_set(d->joints[j].axis, 0, 1, 0);
        if (parent >= 0) mv3_set(d->joints[j].parent_frame.p, 0.1, 0, 0);
        d->joints[j].q0[0] = 0.1;
        parent = b;
    }
    return d;
}


/* printed-arm-like beam: nx x ny x nz hex8 over L x W x H, clamped at x = 0 with an interface at x = L */
static void flex_bench(int nx, int ny, int nz, int fixed_modes, double fcut, bool run) {
    double L = 0.12, W = 0.016, H = 0.010, E = 3e9, rho = 1240;
    int nn = (nx + 1) * (ny + 1) * (nz + 1), ne = nx * ny * nz, nf = (ny + 1) * (nz + 1);
    double *xyz = malloc(3 * (size_t)nn * sizeof(double)), *dens = malloc((size_t)ne * sizeof(double));
    int *conn = malloc(8 * (size_t)ne * sizeof(int)), *root = malloc((size_t)nf * sizeof(int)), *tip = malloc((size_t)nf * sizeof(int));
#define NODE(i, j, k) ((i) + (nx + 1) * ((j) + (ny + 1) * (k)))
    for (int k = 0; k <= nz; k++)
        for (int j = 0; j <= ny; j++)
            for (int i = 0; i <= nx; i++) {
                double *x = xyz + 3 * NODE(i, j, k);
                x[0] = L * i / nx, x[1] = -W / 2 + W * j / ny, x[2] = -H / 2 + H * k / nz;
            }
    for (int k = 0, e = 0; k < nz; k++)
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++, e++) {
                int n8[8] = {NODE(i, j, k), NODE(i + 1, j, k), NODE(i + 1, j + 1, k), NODE(i, j + 1, k),
                             NODE(i, j, k + 1), NODE(i + 1, j, k + 1), NODE(i + 1, j + 1, k + 1), NODE(i, j + 1, k + 1)};
                memcpy(conn + 8 * e, n8, sizeof n8);
                dens[e] = rho;
            }
    for (int k = 0, f = 0; k <= nz; k++)
        for (int j = 0; j <= ny; j++, f++) root[f] = NODE(0, j, k), tip[f] = NODE(nx, j, k);
#undef NODE
    OrthoConstants oc;
    ortho_isotropic(E, 0.36, &oc);
    OrthoModel om = {nn, ne, xyz, conn, 1, &oc, NULL, NULL, 1, HEX8_INCOMPATIBLE};
    const int *nodes[1] = {tip};
    int count[1] = {nf};
    const double point[1][3] = {{L, 0, 0}};
    FlexReduceInput in = {&om, dens, root, nf, 1, nodes, count, point, fixed_modes, 0.02, {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}, fcut, false};
    MbFlexDef F;
    FlexReduceReport rep;
    char err[256];
    double t0 = now();
    bool ok = flex_reduce(&in, &F, &rep, err, sizeof err);
    double tr = now() - t0;
    if (!ok) {
        printf("  %dx%dx%d: reduction failed: %s\n", nx, ny, nz, err);
        goto done;
    }
    printf("  %4dx%2dx%2d elements (%6d nodes), %2d fixed-interface modes%s: reduction %.3f s, %d coordinates, highest %.0f Hz", nx, ny, nz, nn, fixed_modes,
           fcut > 0 ? " + cutoff" : "", tr, F.nmodes, rep.max_frequency_hz);
    if (run) { /* the reduced beam as a pendulum with a 150 g tip mass */
        MbModelDef *d = mbdef_new();
        mv3_set(d->gravity, 0, 0, -9.81);
        int b = mbdef_add_body(d, "arm", rep.fe_mass, rep.fe_com, rep.fe_inertia);
        int j = mbdef_add_joint(d, "hinge", MB_REVOLUTE, -1, b);
        mv3_set(d->joints[j].axis, 0, 1, 0);
        double It[9] = {2e-5, 0, 0, 0, 2e-5, 0, 0, 0, 2e-5};
        int c = mbdef_add_body(d, "payload", 0.15, (double[3]){0, 0, 0}, It);
        int jt = mbdef_add_joint(d, "mount", MB_FIXED, b, c);
        mv3_set(d->joints[jt].parent_frame.p, L, 0, 0);
        d->joints[jt].parent_interface = 0;
        int k = mbdef_add_flex(d, b, F.nmodes, 1);
        MbFlexDef *G = &d->flex[k];
        memcpy(G->omega2, F.omega2, (size_t)F.nmodes * sizeof(double)), memcpy(G->zeta, F.zeta, (size_t)F.nmodes * sizeof(double));
        memcpy(G->ell, F.ell, 6 * (size_t)F.nmodes * sizeof(double)), memcpy(G->dJ, F.dJ, 9 * (size_t)F.nmodes * sizeof(double));
        G->interfaces[0] = F.interfaces[0];
        G->interfaces[0].phi = malloc(3 * (size_t)F.nmodes * sizeof(double)), G->interfaces[0].psi = malloc(3 * (size_t)F.nmodes * sizeof(double));
        memcpy(G->interfaces[0].phi, F.interfaces[0].phi, 3 * (size_t)F.nmodes * sizeof(double));
        memcpy(G->interfaces[0].psi, F.interfaces[0].psi, 3 * (size_t)F.nmodes * sizeof(double));
        MechDiag g;
        mdiag_init(&g);
        MbModel *m = mb_compile(d, NULL, &g);
        MbSim *sim = m ? mb_sim_new(m) : NULL;
        MbState *st = m ? mb_state_new(m) : NULL;
        if (sim && st && mb_state_initial(m, st, &g) && mb_assemble(sim, st, &g)) {
            double h = 0.5 / sqrt(F.omega2[F.nmodes - 1]);
            MbStepReport sr;
            int steps = 20000;
            t0 = now();
            for (int n = 0; n < steps; n++) mb_step(sim, st, h, NULL, &sr);
            double dt = now() - t0;
            printf("; pendulum with payload: %.0f steps/s at the stability step %.2e s (%.3g x real time)", steps / dt, h, steps * h / dt);
        }
        mb_state_free(st), mb_sim_free(sim), mb_model_free(m), mbdef_free(d);
        mdiag_free(&g);
    }
    printf("\n");
    mbflex_free(&F), free(rep.interface_loss);
done:
    free(xyz), free(dens), free(conn), free(root), free(tip);
}

int main(void) {
    printf("rigid chain, RKMK4, h = 1 ms, 2 s simulated\n");
    for (int n = 2; n <= 32; n *= 2) {
        MbModelDef *d = chain(n);
        MechDiag g;
        mdiag_init(&g);
        MbModel *m = mb_compile(d, NULL, &g);
        MbSim *sim = mb_sim_new(m);
        MbState *s = mb_state_new(m);
        mb_state_initial(m, s, &g);
        mb_assemble(sim, s, &g);
        double t0 = now();
        MbStepReport rep;
        int steps = 2000;
        for (int k = 0; k < steps; k++) mb_step(sim, s, 1e-3, NULL, &rep);
        double dt = now() - t0;
        MbEval *ev = mb_eval_new(m);
        mb_evaluate(sim, s, NULL, ev);
        printf("  %2d links: %8.0f steps/s, %.3g x real time, energy residual %.2e J, state %zu bytes\n", n, steps / dt, 2.0 / dt, ev->energy_residual,
               sizeof *s + (size_t)m->nq * 8 + (size_t)m->nv * 8 + (size_t)m->nlimits);
        mb_eval_free(ev), mb_state_free(s), mb_sim_free(sim), mb_model_free(m), mbdef_free(d);
        mdiag_free(&g);
    }
    printf("DC motor + encoder + PID study, 2 s simulated, max step 0.5 ms, control 1 kHz\n");
    MbModelDef *d = chain(2);
    d->joints[0].motion = MB_ACTUATED;
    ActuatorDef a = {.name = "m", .type = ACT_DC_MOTOR, .joint = 0, .gear_ratio = 100, .efficiency = 0.7, .rotor_inertia = 1e-6, .resistance = 3,
                     .torque_constant = 0.02, .back_emf_constant = 0.02, .voltage_limit = 12, .current_limit = 2};
    SensorDef se = {.name = "enc", .type = SENS_JOINT_POSITION, .joint = 0, .period = 1e-3, .resolution = 1e-3, .noise_std = 1e-4, .seed = 1};
    ControllerDef c = {.name = "pid", .type = CTRL_PID, .actuator = 0, .feedback_sensor = 0, .period = 1e-3, .kp = 30, .ki = 10, .kd = 0.5,
                       .output_min = -12, .output_max = 12, .antiwindup = AW_CLAMP, .reference = {.type = TRAJ_SINE, .amplitude = 0.5, .frequency = 1}};
    StudySettings st = {.end_time = 2, .max_step = 5e-4, .record_period = 1e-3};
    MechDiag g;
    mdiag_init(&g);
    MechSim *ms = mechsim_new(d, NULL, &a, 1, &c, 1, &se, 1, &st, &g);
    mechsim_init(ms, &g);
    char err[200];
    double t0 = now();
    mechsim_run_until(ms, 2, err, sizeof err);
    double dt = now() - t0;
    printf("  %.3g x real time; %d recorded rows x %d channels = %.1f MB of histories; checkpoint %zu bytes\n", 2 / dt, mechsim_rows(ms), mechsim_channels(ms),
           mechsim_rows(ms) * mechsim_channels(ms) * 8e-6, mechsim_state_size(ms));
    mechsim_free(ms);
    mbdef_free(d);
    mdiag_free(&g);
    printf("box stacks on a floor with contact (Moreau-Jean, PGS), h = 1 ms, 1 s simulated, 50 mm boxes of 0.1 kg, mu = 0.5\n");
    for (int n = 1; n <= 16; n *= 2) {
        MbModelDef *sd = mbdef_new();
        double I[9] = {0};
        I[0] = I[4] = I[8] = 0.1 * 0.05 * 0.05 / 6;
        ContactShape *sh = calloc((size_t)n + 1, sizeof *sh);
        snprintf(sh[0].name, sizeof sh[0].name, "floor");
        sh[0].type = SHAPE_PLANE, sh[0].body = -1, sh[0].friction = 0.5;
        mpose_identity(&sh[0].pose);
        for (int i = 0; i < n; i++) {
            char nm[16];
            snprintf(nm, sizeof nm, "b%d", i);
            int b = mbdef_add_body(sd, nm, 0.1, (double[3]){0, 0, 0}, I);
            snprintf(nm, sizeof nm, "f%d", i);
            int j = mbdef_add_joint(sd, nm, MB_FREE, -1, b);
            sd->joints[j].q0[2] = 0.025 + 0.05 * i;
            snprintf(sh[i + 1].name, sizeof sh[i + 1].name, "box%d", i);
            sh[i + 1].type = SHAPE_BOX, sh[i + 1].body = b, sh[i + 1].friction = 0.5;
            sh[i + 1].half[0] = sh[i + 1].half[1] = sh[i + 1].half[2] = 0.025;
            mpose_identity(&sh[i + 1].pose);
        }
        mv3_set(sd->gravity, 0, 0, -9.81);
        mdiag_init(&g);
        MbModel *m = mb_compile(sd, NULL, &g);
        MbSim *sim = mb_sim_new(m);
        MbState *s = mb_state_new(m);
        mb_state_initial(m, s, &g);
        mb_assemble(sim, s, &g);
        MbContactRow *rows = malloc(CT_MAX_POINTS * sizeof *rows);
        int *sa = malloc(CT_MAX_POINTS * sizeof(int)), *sb = malloc(CT_MAX_POINTS * sizeof(int)), nrows = 0, maxit = 0, unconv = 0;
        ContactOptions co;
        contact_options_default(&co);
        ContactWarm warm;
        contact_warm_init(&warm, contact_warm_capacity(m, sh, n + 1, &co));
        ContactReport cr;
        MbStepReport rep;
        double pen = 0;
        t0 = now();
        for (int k = 0; k < 1000; k++) {
            if (!contact_step(sim, s, 1e-3, NULL, sh, n + 1, &co, rows, sa, sb, &nrows, &cr, &rep, &warm)) break;
            maxit = cr.solve.iterations > maxit ? cr.solve.iterations : maxit;
            unconv += !cr.solve.converged;
            pen = fmax(pen, cr.max_penetration);
        }
        dt = now() - t0;
        double top = s->q[m->joint_qadr[n - 1] + 2], ref = 0.025 + 0.05 * (n - 1);
        printf("  %2d boxes: %7.0f steps/s (%.3g x real time), %4d contact rows, max sweeps %d, unconverged steps %d, max penetration %.2e m, top box drift %.2e m\n",
               n, 1000 / dt, 1 / dt, nrows, maxit, unconv, pen, top - ref);
        free(rows), free(sa), free(sb), free(sh);
        contact_warm_free(&warm);
        mb_state_free(s), mb_sim_free(sim), mb_model_free(m), mbdef_free(sd);
        mdiag_free(&g);
    }
    printf("flexible bodies: Craig-Bampton reduction of a clamped 120 x 16 x 10 mm beam with a tip interface; reduced beam as a pendulum\n");
    flex_bench(60, 8, 5, 4, 0, true);
    flex_bench(60, 8, 5, 4, 2000, true);
    flex_bench(60, 8, 5, 24, 0, true);
    flex_bench(120, 16, 10, 8, 0, false);
    return 0;
}
