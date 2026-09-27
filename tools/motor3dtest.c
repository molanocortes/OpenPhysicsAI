/* motor3dtest - the 3D motor (src/lab/magnet/motor3d.c, MFEM Nedelec elements on a cylindrical grid) against the
 * lab's own 2D motor solver (src/lab/magnet/magnet.c, finite differences on a square grid), an independent solution.
 *
 * Criterion written on 2026-09-26. An exploratory run came first, and is recorded so that the criterion is not taken
 * as blind: at 0, 10, 20 and 30 degrees with the current angle at 0 the periodic 3D slab gave -198, -92, -220 and
 * -198 N m/m against 2D's -203, -95, -220 and -198 (within 3.1 %). The test runs other points:
 *   P1 the motor of examples/lab/pm_motor_3d.json as a periodic slab (the 2D limit: no ends) at rotor angles 2.5, 7.5,
 *      12.5, 17.5, 22.5 and 27.5 degrees with the motoring current angle (180 electrical degrees): its torque per metre
 *      within 5 % of the 2D solver's at each angle. 5 %: the two grids resolve the 1 mm air gap differently (2 cells in
 *      3D, 4 in 2D) and the 2D solver's own torque is known to move by a few per cent with its grid (docs/lab/magnet.md).
 *   P2 with ends (the full motor, 50 mm stack, end windings) at 2.5 degrees: the torque lies within 10 % of the slab's
 *      times the stack length. Not a closed form: a statement that end effects in a 1 mm gap motor are small.
 *   First run (2026-09-26): P1 FAIL at the example's own grid (2 mm rings, 144 sectors): -0.81, -1.23, -2.05 and
 *   -5.46 % at 2.5 to 17.5 degrees, where the torque is smallest; P2 pass (-0.96 %). A convergence study at 17.5 degrees
 *   (slab torque per 50 mm: 5.195 N m at 2 mm and 144 sectors; 5.296 at 288 sectors; 5.371 at 1 mm rings and 288;
 *   5.395 at 0.5 mm and 288; 5.420 at 0.5 mm and 576; the air gap's own cells change nothing) shows the 3D torque rising
 *   towards 2D's 5.495 as the grid refines: the fault was the slab's grid, not the method. P1 now runs its slab at 1 mm
 *   rings and 288 sectors; the criterion is unchanged. The example keeps the coarser grid for memory (a full 3D solve
 *   at the finer one needs about four times the 1.3 GB factor) and its torque is stated as low by up to 5 %.
 * Run in the lab tier (make test-lab3d); about two minutes. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/json.h"
#include "../src/lab/lab_domains.h"

static int failures;
static void verdict(bool ok, const char *name) {
    printf("  %s: %s\n", name, ok ? "pass" : "FAIL");
    failures += !ok;
}

static bool torques(JsonValue *sc, const char *key, double *out, int n) {
    LabRunInfo info;
    char err[256];
    memset(&info, 0, sizeof info);
    if (!lab_run_domain("magnet", sc, "/tmp/navier-motor3dtest.lab", true, &info, err, sizeof err)) {
        printf("  run failed: %s\n", err);
        return false;
    }
    remove("/tmp/navier-motor3dtest.lab");
    JsonValue *d = json_parse(info.diagnostics, strlen(info.diagnostics), NULL, NULL);
    JsonValue *t = json_get(d, key);
    bool ok = t && json_len(t) == (size_t)n;
    for (int i = 0; ok && i < n; i++) out[i] = json_at(t, (size_t)i)->u.number;
    json_free(d);
    return ok;
}

int main(void) {
    JsonError je;
    JsonValue *base = json_read_file("examples/lab/pm_motor_3d.json", 1 << 20, &je);
    if (!base) {
        printf("cannot read examples/lab/pm_motor_3d.json: %s\n", je.message);
        return 1;
    }
    enum { N = 6 };
    double t2[N], t3[N], tf[1];
    /* the 2D reference: the same scenario without its three_d block */
    JsonValue *s2 = json_clone(base);
    json_remove(s2, "three_d");
    JsonValue *run = json_get(s2, "run");
    json_set_number(run, "angle_from_deg", 2.5), json_set_number(run, "angle_to_deg", 27.5), json_set_int(run, "frames", N);
    printf("== P1: the periodic 3D slab against the 2D solver, torque per metre\n");
    bool ok2 = torques(s2, "torque_n_m_per_m", t2, N);
    JsonValue *s3 = json_clone(base);
    run = json_get(s3, "run");
    json_set_number(run, "angle_from_deg", 2.5), json_set_number(run, "angle_to_deg", 27.5), json_set_int(run, "frames", N);
    json_set_bool(json_get(s3, "three_d"), "periodic", true);
    json_set_number(json_get(s3, "three_d"), "radial_cell_m", 0.001), json_set_int(json_get(s3, "three_d"), "sectors", 288);
    double Lst = json_get_num(json_get(s3, "three_d"), "stack_length_m", 0);
    bool ok3 = torques(s3, "torque_n_m", t3, N);
    bool pass = ok2 && ok3;
    for (int i = 0; pass && i < N; i++) {
        double a = t3[i] / Lst, e = a / t2[i] - 1;
        printf("  %5.1f deg: 3D %8.3f N m/m, 2D %8.3f (%+.2f %%)\n", 2.5 + 5.0 * i, a, t2[i], 100 * e);
        if (!(fabs(e) < 0.05)) pass = false;
    }
    verdict(pass, "P1");
    printf("== P2: the full motor with ends at 2.5 degrees against the slab times the stack (both at the example's grid)\n");
    JsonValue *sp = json_clone(base);
    run = json_get(sp, "run");
    json_set_number(run, "angle_from_deg", 2.5), json_set_number(run, "angle_to_deg", 2.5), json_set_int(run, "frames", 1);
    json_set_bool(json_get(sp, "three_d"), "periodic", true);
    double tp[1];
    bool okp = torques(sp, "torque_n_m", tp, 1);
    json_free(sp);
    JsonValue *sf = json_clone(base);
    run = json_get(sf, "run");
    json_set_number(run, "angle_from_deg", 2.5), json_set_number(run, "angle_to_deg", 2.5), json_set_int(run, "frames", 1);
    bool okf = torques(sf, "torque_n_m", tf, 1);
    if (okf && okp) printf("  with ends %.4f N m, slab %.4f N m (%+.2f %%)\n", tf[0], tp[0], 100 * (tf[0] / tp[0] - 1));
    verdict(okf && okp && fabs(tf[0] / tp[0] - 1) < 0.10, "P2");
    json_free(base), json_free(s2), json_free(s3), json_free(sf);
    printf(failures ? "motor3dtest: %d FAILED\n" : "motor3dtest: all passed\n", failures);
    return failures ? 1 : 0;
}
