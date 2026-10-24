/* kerneltest.c - verification of the physics kernel (src/lab/kernel, G24): every case is a scenario as an agent would
 * write it, run through the lab's own entry point, its numbers read back from the run's record.
 *
 * Criteria, written on 2026-10-08 before the first run of this file and not to be changed without a dated note:
 *   K1  linear elasticity against a manufactured solution (u = a (s, 2 s, 3 s), s = sin(pi x) sin(pi y) sin(pi z), on
 *       the unit cube, E = 1, nu = 0.3, the body force -div(sigma) written as formulas, the boundary held at the exact
 *       values): the L2 error falls at the element's order plus one, the rate between the two finest meshes at least
 *       p + 0.75, for p = 1 (8 and 16 cells a side) and p = 2 (4 and 8).
 *   K2  steady heat conduction against T = s + x / 2 (k = 2, the source 3 pi^2 k s): the same rates.
 *   K3  a slender cantilever (400 x 20 x 20 mm, steel, 100 N on its end, quadratic elements on 40 x 2 x 2): the end's
 *       mean deflection within 1 % of Timoshenko's beam (P L^3 / (3 E I) + P L / (kappa G A), kappa = 5/6), as
 *       geartest M3 has it for the mechanism domain's elements.
 *   K4  thermal strain: (a) a block heated 100 K, held only on three symmetry planes, grows by alpha dT L and stays free
 *       of stress (its largest von Mises under 1e-9 of E alpha dT); (b) the same block held along x at both ends: the
 *       stress -E alpha dT (von Mises E alpha dT) and the end's reaction E alpha dT A, both within 1e-9.
 *   K5  heat through a slab, one face at 400 K, the other cooled by convection (h = 25 W/(m^2 K) into 300 K), the
 *       rest insulated: the heat entering through the held face, the heat leaving by convection and the closed form
 *       (T1 - Tinf) A / (L / k + 1 / h) agree within 1e-9, and the cooled face's temperature within 1e-9 K.
 *   K6  a free bar whose temperature varies linearly through its height bends into an arc with no stress: on quadratic
 *       elements the displacement equals the exact field (u = alpha G ((y - c) x, ((y - c)^2 - x^2 - z^2) / 2,
 *       (y - c) z)) within 1e-9 of its largest value in the L2 sense, and the largest von Mises is under 1e-6 of
 *       E alpha G h.
 *   (added 2026-10-08 with meshes mapped by formulas, before its first run; G24 step 2, an application this repository
 *   verified elsewhere rebuilt as a scenario)
 *   K7  the C-ring of tools/printmesh_ccx.py (inner radius 10 mm, outer 14 mm, 4 mm wide, a gap of 0.25 rad, one tip
 *       held, 10 N pulling the other along the line through the tips), a box in (r, theta, z) mapped onto the ring by
 *       formulas, quadratic elements on quadratic geometry, 4 x 120 x 4: the hoop stress at the apex's inner fibre at
 *       mid-width within 2 % of Winkler-Bach's curved beam (CalculiX on C3D20R elements gives it to 0.7 %; the
 *       engineering core's TET10 path reads a peak at the ring's edges, its strength ratio 12 % under).
 *   Note 2026-10-08, after the first run: K1 to K5 passed; K6's stress was 4.7e-2 Pa (under its 24 Pa) but its
 *   displacement differed from the exact field by 2.9e-9 of its norm, over the 1e-9 written before the run. The same
 *   scenario on twice the cells each way gives 1.2e-8, and on cubic elements 3.0e-9: the difference grows as the mesh is
 *   refined, which round-off does and a discretisation error does not (the field is quadratic, which quadratic elements
 *   hold exactly). 1e-9 asked more than double precision gives a system of this conditioning; the criterion is now
 *   1e-7, the stress criterion unchanged.
 *   Note 2026-10-08, after K7's first run: the hoop stress came out 26.479 MPa, 2.53 % over Winkler-Bach's 25.825: K7
 *   failed. The same scenario refined (cells across the depth x around x across the width, order) gave at mid-width
 *   26.313 (8 x 120 x 4), 26.339 (8 x 160 x 8), 26.293 (16 x 160 x 8), 26.327 (4 x 120 x 4, cubic) and 26.315 (8 x 120
 *   x 8, cubic), and at the same radius on the ring's flat faces 25.26 to 25.30: the solution settles 1.8 to 1.9 % over
 *   Winkler-Bach at mid-width and 2.1 % under it at the faces, and the first mesh added about 0.7 % of its own.
 *   Winkler-Bach is a beam's theory, one stress across the width; on this square section the stress varies by 4 %
 *   across it, so 2 % at mid-width left 0.2 % for the discretisation, less than the first mesh's. The 0.7 % quoted
 *   above for CalculiX was its Tsai-Wu strength ratio of the printed material, the smallest along the inner fibre
 *   (GOALS.md G17 step 4), not this hoop stress. K7 is still run and printed as written, and not counted. In its
 *   place, written before the solution it compares with was computed:
 *   K7b the same ring with the isotropic material tools/printmesh_ccx.py builds for pla_fff (E 3426.9 MPa, nu 0.323),
 *       on 8 x 120 x 4 quadratic elements, against CalculiX 2.23 solving the same 3D problem on that script's default
 *       8 x 160 x 8 C3D20R elements (the nodal stresses it extrapolates and averages): the hoop stress at the apex's
 *       inner fibre at mid-width, at the inner fibre on a flat face and at the outer fibre at mid-width, each within
 *       1 % of CalculiX's.
 *   (added 2026-10-08 with parts read from STL files, before its first run)
 *   K8  a thick-walled tube read from an STL file the test writes (inner radius 10 mm, outer 15 mm, 10 mm long, 256
 *       facets around), meshed by the repository's own tetrahedral mesher (src/geom/tetmesh: surface cells of 1 mm,
 *       sharp edges kept, quadratic tetrahedra whose mid-edge nodes lie on the surface), steel (E 200 GPa, nu 0.3),
 *       10 MPa on the bore chosen as a cylinder, one end held along the axis only and two points against sliding and
 *       turning (a support that carries no load): at mid-length, the hoop stress at the bore on the x and on the y axis
 *       each within 2 % of Lame's p (ro^2 + ri^2) / (ro^2 - ri^2), the bore's radial displacement along x and along y
 *       (half the change of its diameter) each within 1 % of Lame's for free ends (sigma_z = 0, exact in 3D), and the
 *       largest von Mises anywhere within 3 % of Lame's at the bore.
 *   (added 2026-10-08 after K8 passed, before its first run; G24 step 2)
 *   K9  the engineering core's two wall brackets of examples/bracket_comparison (A plain, B with a 24 mm inner chamfer;
 *       6061-T6, E 68.9 GPa, nu 0.33; the back face x = 0 held; 3 kg x 9.80665 m/s^2 = 29.42 N down, spread over the
 *       16 x 16 mm pad on the arm's tip), read from their STL files and meshed at 4 mm, on quadratic and on cubic
 *       elements: the load-region displacement (the pad's area-weighted mean along the load, as the study defines it)
 *       of A on cubic elements within 2.4 % of the core's Richardson-extrapolated 0.025603 mm (2.4 % is its grid
 *       convergence index over its 4, 2 and 1 mm voxel meshes, expected_output/evidence.json); A and B each within 2 %
 *       between quadratic and cubic elements (the study's own convergence criterion, which B's staircase chamfer failed
 *       on voxels); B under A; the back face's reaction equal to the load within 1e-6 of it. B's value beside the core's
 *       unconverged 1 mm one is printed, not judged.
 *   (added 2026-10-08 for equations an agent writes, in time, before any of their code existed; G24 step 2b)
 *   K10 the time stepping's order: u = (x^2 + y^2 + z^2) e^-t on the unit cube as a field of mass 1 and diffusion 1
 *       with the source that makes it exact, held at the exact values on every face, quadratic elements on 2 x 2 x 2
 *       cells (they hold the quadratic in space exactly, so every error left is the time stepping's), integrated to
 *       t = 1 in 5, 10 and 20 steps: the L2 error at t = 1 falls at order at least 0.9 by backward Euler and at least
 *       1.8 by BDF2, between the two finest step sizes.
 *   K11 conduction into a semi-infinite solid written as a field (mass rho c_p 3.6e6 J/(m^3 K), diffusion k 45
 *       W/(m K)): a bar 100 mm long and 10 x 10 mm across at 300 K, its end x = 0 held at 400 K from t = 0, insulated
 *       elsewhere, quadratic elements on 100 x 1 x 1 cells, BDF2 in steps of 0.1 s: at t = 20 s the temperature at
 *       x = 5, 10 and 20 mm within 0.5 % of the 100 K step of T0 + dT erfc(x / (2 sqrt(alpha t))).
 *   K12 two fields coupled through a gradient (Joule heating): a copper bar 100 mm long (sigma 5.8e7 S/m, k 400
 *       W/(m K)), 0.01 V across its ends for the potential phi, the ends held at 300 K, the temperature's source
 *       sigma |grad phi|^2 written with dx(phi), dy(phi), dz(phi), quadratic elements on 10 x 1 x 1 cells: phi
 *       linear and T = 300 + sigma V^2 x (L - x) / (2 k L^2), which quadratic elements hold exactly; phi's L2 error
 *       under 1e-9 of its norm and the temperature at mid-length within 1e-9 of its 1.8125 K rise.
 *   K13 a conductivity that depends on the temperature itself (k = k0 (1 + beta (T - T0)), beta 0.01 /K): a slab 10 mm
 *       thick held at 300 K and 400 K on its faces, steady, quadratic elements on 20 x 1 x 1 cells: the fixed-point
 *       iteration settles (its last change under 1e-10 of the 100 K difference) within 50 iterations, and the
 *       temperature at mid-thickness within 1e-4 of the 100 K difference of Kirchhoff's exact 358.1139 K.
 *   K14 convection and diffusion, a non-symmetric system: -D u'' + v u' = 0 on 0 <= x <= 1 m (D 0.1 m^2/s, v 1 m/s,
 *       Peclet 10), u = 0 and 1 at the ends, steady, quadratic elements on 80 x 1 x 1 cells: the root-mean-square
 *       error against (e^(Pe x) - 1) / (e^Pe - 1) under 1e-4.
 *   (added 2026-10-08 after the first application trial, challenges/agents/trials/, asked for them; before their runs)
 *   K15 the first time a probe crosses a value: K11's bar, the probe at 10 mm, 350 K, reached when x / (2 sqrt(alpha
 *       t)) = erfc^-1(1/2) = 0.4769363 (8.7924 s), BDF2 in steps of 0.1 s, the crossing found at every step and
 *       placed within it: within 0.5 % of that time.
 *   K16 the flow of a field through each boundary: K13's slab (10 x 10 mm across): the heat entering through the hot
 *       face and leaving through the cold one, from the residuals where the field is held, each within 1e-6 of
 *       Kirchhoff's k0 (dT + beta dT^2 / 2) A / L = 67.5 W.
 *   (added 2026-10-08 before the bimetal trial, before its first run)
 *   K17 two bonded materials bent by heat: a bimetal strip 40 mm long, 1 mm wide, of two 0.5 mm layers (Young's
 *       moduli 124 and 148 GPa, expansions 26.46e-6 and 1.30e-6 /K, Poisson's ratio 0 in both so that a beam's theory is
 *       exact away from the ends), heated by 80 K, held against rigid motion only (three points), quadratic elements
 *       on 40 x 2 x 4 cells with the bond on a mesh plane: the curvature at mid-length, the second difference of the
 *       deflection at 15, 20 and 25 mm, within 0.1 % of Timoshenko's bimetal formula (1925).
 *   (added 2026-10-09 with contact against rigid surfaces, G24 step 3, before any of its code existed)
 *   K18 a block on a rigid plane: a steel block 10 x 10 x 5 mm resting on a frictionless rigid plane, held only on two
 *       symmetry planes, 10 MPa on its top, quadratic elements on 4 x 4 x 2 cells: the contact pressure at every point
 *       of the bottom within 1e-6 of 10 MPa, the contact's resultant equal to the load within 1e-9 of it, and the
 *       deepest penetration under 1e-6 of the block's compression.
 *   K19 Hertz's line contact: a steel cylinder of radius 10 mm (a quarter of a ring from 2.5 to 10 mm, 1 mm thick, held
 *       in plane strain, its symmetry plane free to slide) pressed on a frictionless rigid plane by moving its hub down
 *       0.05 mm, quadratic elements graded toward the contact: the contact's resultant equal to the hub's reaction
 *       within 1e-6, the peak contact pressure within 3 % of Hertz's p0 = sqrt(F' E* / (pi R)) and the contact's half
 *       width (where the pressure exceeds 1 % of p0) within 10 % of Hertz's b = sqrt(4 F' R / (pi E*)), F' the
 *       resultant per unit length and E* = E / (1 - nu^2).
 *   Note 2026-10-09, after the first runs of K18 and K19: K18 printed FAIL with every number inside its criterion
 *   (pressure uniform to 1.6e-14, resultant exact, penetration 1e-16 of the compression): the test counted the contact
 *   points after freeing the record that held them, now counted first; the criterion is unchanged. K19 failed: the peak
 *   pressure came out 7.36 GPa against Hertz's 5.60 (+31.6 %), the half width -3.3 %, equilibrium 2.8e-7, and the
 *   multipliers did not settle in 200 updates (822 Newton iterations, the active set flipping). Contact was enforced at
 *   every Gauss point of the quadratic faces, more constraints than the faces have nodes, which over-constrains the
 *   surface and makes the pressure oscillate. It is now enforced at the Gauss-Lobatto points of quadrilateral faces,
 *   which are their nodes, and the multipliers stop at a penetration of 1e-8 of the largest displacement (1e-10
 *   before); K19's criterion is unchanged. Its second run still failed (+26.8 %), converged this time (26 updates): its
 *   pressure along the contact matched Hertz within 0.2 % at the faces' mid-side nodes, while each corner node, kept
 *   once per neighbouring face, carried two pressures whose average was Hertz's (7.10 and 4.10 GPa against 5.60): the
 *   load split arbitrarily between the copies of a shared node. Coinciding points are now merged into one constraint
 *   per node; the criterion is unchanged. Its third run passed: peak pressure +0.24 %, half width -3.3 %.
 *   Note 2026-10-09, later: a ball pressed into a block took more than ten minutes, and two changes followed, neither
 *   touching a criterion. The penalty is now ten times the stiffness of each point's own nodes over its area (it was 10
 *   E over the face's size, which left a quadratic face's corners weaker than the body and the multipliers slow), and
 *   the system is factored again only when the active set changes; a point without area is left out (its penalty would
 *   divide by zero). K18 and K19 ran again and passed: K18 with its pressure uniform to 5.9e-8 (1.6e-14 before; the
 *   penalty now differs between a face's corner, side and centre nodes, and the multipliers stop at the tolerance with a
 *   trace of it), K19 at +0.25 % and -3.3 %, in 15 Newton iterations and 5 multiplier updates (131 and 27 before). K19's
 *   map has no slope at its contact line (0.5 pi y^2): the nodes there keep only a trace of area from the quadratic
 *   elements, carry almost nothing and read pressure 0, as before; the peak is read at the next nodes, 1.7 um away.
 *   (added 2026-10-09 after the bimetal trial asked for it, before its first run)
 *   K20 a field sampled along a line: K12's temperature along the bar's axis at 21 points from end to end, each within
 *   1e-9 of the 1.8125 K rise of the exact parabola, the line's ends exactly where they were asked.
 *   (added 2026-10-09 with the library's elasticity driven by a field in time, before any of its code existed)
 *   K21 thermal stress in time: K11's bar heated at one end (its temperature a field, linear on 0.5 mm cells, BDF2 in
 *       steps of 0.1 s), its sides on rollers and its cold end held along the axis, steel (E 200 GPa, Poisson's ratio
 *       0.3, expansion 12e-6 /K, stress-free at 300 K), the library's elasticity on quadratic elements solved at each
 *       output time with the field as its temperature. At 5, 10 and 20 mm and at 5, 10 and 20 s: (a) the transverse
 *       stresses s_yy and s_zz within 1e-6 of the step's stress E alpha dT / (1 - nu) from -E alpha (T - 300) / (1 - nu)
 *       with the field's own temperature at that point (the displacement quadratic and the temperature linear, the strain
 *       can follow the temperature exactly); (b) s_yy within 0.5 % of the step's stress of the same formula with erfc's
 *       temperature; (c) the axial stress s_xx under 1e-6 of the step's stress.
 *   (added 2026-10-09 with the principal stresses in the record, before their code existed)
 *   K22 principal stresses: a block 10 mm a side, quadratic elements on 2 x 2 x 2 cells, every face displaced as a
 *       uniform strain with shear (exx 1e-4, eyy -2e-5, ezz 3e-5, exy 5e-5, eyz -4e-5, exz 2e-5; steel, E 200 GPa,
 *       Poisson's ratio 0.3): the largest and the smallest principal stress the kernel finds in the body, and the three
 *       at a probe, each within 1e-9 of the largest stress of the eigenvalues of Hooke's stress, computed here by
 *       Jacobi's rotations. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../src/core/json.h"
#include "../src/lab/lab_domains.h"

static int failures;

static void verdict(int ok, const char *name) {
    printf("  %s: %s\n", name, ok ? "pass" : "FAIL");
    fflush(stdout);
    if (!ok) failures++;
}

static double wall(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + 1e-9 * t.tv_nsec;
}

static char workdir[512];

/* runs a scenario (JSON text) through the lab's entry point; returns its whole record (OUT.lab.kernel.json), NULL on
 * failure with the message printed */
static JsonValue *run(const char *name, const char *scenario) {
    JsonError je;
    JsonValue *root = json_parse(scenario, strlen(scenario), NULL, &je);
    if (!root) {
        printf("  %s: the scenario does not parse: %s (line %d)\n", name, je.message, je.line);
        return NULL;
    }
    char out[700], rec[760], err[512] = "";
    snprintf(out, sizeof out, "%s/%s.lab", workdir, name);
    snprintf(rec, sizeof rec, "%s.kernel.json", out);
    LabRunInfo info;
    bool ok = lab_run_domain("kernel", root, out, true, &info, err, sizeof err);
    json_free(root);
    if (!ok) {
        printf("  %s: %s\n", name, err);
        return NULL;
    }
    JsonValue *r = json_read_file(rec, 1 << 26, &je);
    if (!r) printf("  %s: no record (%s)\n", name, je.message);
    unlink(out), unlink(rec);
    return r;
}

static double num(const JsonValue *r, const char *a, const char *b, const char *c) {
    const JsonValue *v = json_get(r, a);
    if (b) v = json_get(v, b);
    if (c) v = json_get(v, c);
    return v && v->type == JSON_NUMBER ? v->u.number : NAN;
}

static double vec(const JsonValue *r, const char *a, const char *b, const char *c, int k) {
    const JsonValue *v = json_get(json_get(json_get(r, a), b), c);
    return json_len(v) > (size_t)k && json_at(v, k)->type == JSON_NUMBER ? json_at(v, k)->u.number : NAN;
}

/* ---- K1: elasticity, manufactured */

static void k1_body_force(double lam, double mu, double a, char f[3][1024]) {
    /* u = a (s, 2 s, 3 s): f_i = 3 pi^2 mu c_i s - (lambda + mu) a (s_i1 + 2 s_i2 + 3 s_i3), c = a (1, 2, 3) */
    const char *S = "sin(pi*x)*sin(pi*y)*sin(pi*z)";
    const char *H[3][3] = {
        {"(-pi^2*sin(pi*x)*sin(pi*y)*sin(pi*z))", "(pi^2*cos(pi*x)*cos(pi*y)*sin(pi*z))", "(pi^2*cos(pi*x)*sin(pi*y)*cos(pi*z))"},
        {"(pi^2*cos(pi*x)*cos(pi*y)*sin(pi*z))", "(-pi^2*sin(pi*x)*sin(pi*y)*sin(pi*z))", "(pi^2*sin(pi*x)*cos(pi*y)*cos(pi*z))"},
        {"(pi^2*cos(pi*x)*sin(pi*y)*cos(pi*z))", "(pi^2*sin(pi*x)*cos(pi*y)*cos(pi*z))", "(-pi^2*sin(pi*x)*sin(pi*y)*sin(pi*z))"}};
    for (int i = 0; i < 3; i++)
        snprintf(f[i], 1024, "3*pi^2*%.17g*%.17g*%s - %.17g*(%s + 2*%s + 3*%s)", mu, a * (i + 1), S, (lam + mu) * a, H[i][0], H[i][1], H[i][2]);
}

static double k1_error(int p, int n) {
    double E = 1, nu = 0.3, lam = E * nu / ((1 + nu) * (1 - 2 * nu)), mu = E / (2 * (1 + nu)), a = 1e-3;
    char f[3][1024], sc[8192];
    k1_body_force(lam, mu, a, f);
    const char *S = "sin(pi*x)*sin(pi*y)*sin(pi*z)";
    snprintf(sc, sizeof sc,
             "{\"domain\":\"kernel\",\"title\":\"K1\",\"mesh\":{\"box\":{\"size_m\":[1,1,1],\"cells\":[%d,%d,%d]}},\"order\":%d,"
             "\"physics\":[\"elasticity\"],\"materials\":{\"m\":{\"youngs_modulus_pa\":1,\"poisson_ratio\":0.3,\"source\":\"test\"}},"
             "\"loads\":{\"body_force_n_m3\":[\"%s\",\"%s\",\"%s\"]},"
             "\"boundaries\":[{\"name\":\"all\",\"where\":\"all\",\"displacement_m\":[\"%g*%s\",\"%g*%s\",\"%g*%s\"]}],"
             "\"check\":{\"displacement_m\":[\"%g*%s\",\"%g*%s\",\"%g*%s\"]},\"run\":{\"frames\":false}}",
             n, n, n, p, f[0], f[1], f[2], a, S, 2 * a, S, 3 * a, S, a, S, 2 * a, S, 3 * a, S);
    JsonValue *r = run("k1", sc);
    double e = r ? num(r, "check", "displacement_l2_error_m", NULL) : NAN;
    json_free(r);
    return e;
}

static void test_k1(void) {
    printf("K1 elasticity against a manufactured solution\n");
    double t0 = wall();
    for (int p = 1; p <= 2; p++) {
        int n0 = p == 1 ? 8 : 4;
        double e0 = k1_error(p, n0), e1 = k1_error(p, 2 * n0), rate = log2(e0 / e1);
        printf("  p = %d: L2 error %.4e (%d cells a side), %.4e (%d): rate %.3f; %.1f s\n", p, e0, n0, e1, 2 * n0, rate, wall() - t0);
        char name[64];
        snprintf(name, sizeof name, "K1 p = %d converges at order %d", p, p + 1);
        verdict(rate >= p + 0.75, name);
    }
}

/* ---- K2: heat, manufactured */

static double k2_error(int p, int n) {
    char sc[4096];
    snprintf(sc, sizeof sc,
             "{\"domain\":\"kernel\",\"title\":\"K2\",\"mesh\":{\"box\":{\"size_m\":[1,1,1],\"cells\":[%d,%d,%d]}},\"order\":%d,"
             "\"physics\":[\"heat\"],\"materials\":{\"m\":{\"conductivity_w_mk\":2,\"source\":\"test\"}},"
             "\"loads\":{\"heat_source_w_m3\":\"3*pi^2*2*sin(pi*x)*sin(pi*y)*sin(pi*z)\"},"
             "\"boundaries\":[{\"name\":\"all\",\"where\":\"all\",\"temperature_k\":\"sin(pi*x)*sin(pi*y)*sin(pi*z) + x/2\"}],"
             "\"check\":{\"temperature_k\":\"sin(pi*x)*sin(pi*y)*sin(pi*z) + x/2\"},\"run\":{\"frames\":false}}",
             n, n, n, p);
    JsonValue *r = run("k2", sc);
    double e = r ? num(r, "check", "temperature_l2_error_k", NULL) : NAN;
    json_free(r);
    return e;
}

static void test_k2(void) {
    printf("K2 heat conduction against a manufactured solution\n");
    for (int p = 1; p <= 2; p++) {
        int n0 = p == 1 ? 8 : 4;
        double e0 = k2_error(p, n0), e1 = k2_error(p, 2 * n0), rate = log2(e0 / e1);
        printf("  p = %d: L2 error %.4e (%d cells a side), %.4e (%d): rate %.3f\n", p, e0, n0, e1, 2 * n0, rate);
        char name[64];
        snprintf(name, sizeof name, "K2 p = %d converges at order %d", p, p + 1);
        verdict(rate >= p + 0.75, name);
    }
}

/* ---- K3: cantilever */

static void test_k3(void) {
    printf("K3 a slender cantilever against Timoshenko's beam\n");
    double L = 0.4, h = 0.02, b = 0.02, E = 207e9, nu = 0.3, P = 100;
    char sc[4096];
    snprintf(sc, sizeof sc,
             "{\"domain\":\"kernel\",\"title\":\"K3\",\"mesh\":{\"box\":{\"size_m\":[%g,%g,%g],\"cells\":[40,2,2]}},\"order\":2,"
             "\"physics\":[\"elasticity\"],\"materials\":{\"steel\":{\"youngs_modulus_pa\":%g,\"poisson_ratio\":%g,\"source\":\"nominal\"}},"
             "\"boundaries\":[{\"name\":\"clamp\",\"where\":{\"plane\":[\"x\",0]},\"fixed\":true},"
             "{\"name\":\"tip\",\"where\":{\"plane\":[\"x\",%g]},\"force_n\":[0,%g,0]}],\"run\":{\"frames\":false}}",
             L, h, b, E, nu, L, -P);
    JsonValue *r = run("k3", sc);
    double tip = r ? -vec(r, "boundaries", "tip", "mean_displacement_m", 1) : NAN;
    json_free(r);
    double I = b * h * h * h / 12, G = E / (2 * (1 + nu)), ref = P * L * L * L / (3 * E * I) + P * L / (5.0 / 6 * G * b * h);
    printf("  end deflection %.6f mm, Timoshenko %.6f mm (%+.3f %%)\n", 1e3 * tip, 1e3 * ref, 100 * (tip / ref - 1));
    verdict(fabs(tip / ref - 1) < 0.01, "K3 within 1 % of Timoshenko");
}

/* ---- K4: thermal strain */

static void test_k4(void) {
    printf("K4 thermal strain of a heated block\n");
    double L = 0.1, W = 0.05, H = 0.02, E = 200e9, al = 1.2e-5, dT = 100, s = E * al * dT;
    const char *mat = "\"materials\":{\"steel\":{\"youngs_modulus_pa\":200e9,\"poisson_ratio\":0.3,\"expansion_per_k\":1.2e-5,\"source\":\"nominal\"}}";
    char sc[4096];
    snprintf(sc, sizeof sc,
             "{\"domain\":\"kernel\",\"title\":\"K4a\",\"mesh\":{\"box\":{\"size_m\":[%g,%g,%g],\"cells\":[4,3,2]}},\"order\":1,"
             "\"physics\":[\"elasticity\"],%s,\"reference_temperature_k\":293,\"loads\":{\"temperature_k\":393},"
             "\"boundaries\":[{\"name\":\"sx\",\"where\":{\"plane\":[\"x\",0]},\"fix\":[\"x\"]},{\"name\":\"sy\",\"where\":{\"plane\":[\"y\",0]},\"fix\":[\"y\"]},"
             "{\"name\":\"sz\",\"where\":{\"plane\":[\"z\",0]},\"fix\":[\"z\"]},{\"name\":\"end\",\"where\":{\"plane\":[\"x\",%g]}}],\"run\":{\"frames\":false}}",
             L, W, H, mat, L);
    JsonValue *r = run("k4a", sc);
    double grow = r ? vec(r, "boundaries", "end", "mean_displacement_m", 0) : NAN, vm = r ? num(r, "results", "max_von_mises_pa", NULL) : NAN;
    json_free(r);
    printf("  free: the end moves %.9e m (alpha dT L %.9e), largest von Mises %.3e Pa\n", grow, al * dT * L, vm);
    verdict(fabs(grow / (al * dT * L) - 1) < 1e-9 && vm < 1e-9 * s, "K4 (a) free growth alpha dT L, no stress");
    snprintf(sc, sizeof sc,
             "{\"domain\":\"kernel\",\"title\":\"K4b\",\"mesh\":{\"box\":{\"size_m\":[%g,%g,%g],\"cells\":[4,3,2]}},\"order\":1,"
             "\"physics\":[\"elasticity\"],%s,\"reference_temperature_k\":293,\"loads\":{\"temperature_k\":393},"
             "\"boundaries\":[{\"name\":\"sx\",\"where\":{\"plane\":[\"x\",0]},\"fix\":[\"x\"]},{\"name\":\"sy\",\"where\":{\"plane\":[\"y\",0]},\"fix\":[\"y\"]},"
             "{\"name\":\"sz\",\"where\":{\"plane\":[\"z\",0]},\"fix\":[\"z\"]},{\"name\":\"end\",\"where\":{\"plane\":[\"x\",%g]},\"fix\":[\"x\"]}],"
             "\"run\":{\"frames\":false}}",
             L, W, H, mat, L);
    r = run("k4b", sc);
    double rx = r ? vec(r, "boundaries", "end", "reaction_n", 0) : NAN, vm2 = r ? num(r, "results", "max_von_mises_pa", NULL) : NAN;
    json_free(r);
    printf("  held: the end's reaction %.9e N (E alpha dT A %.9e), von Mises %.9e Pa (%.9e)\n", rx, s * W * H, vm2, s);
    verdict(fabs(fabs(rx) / (s * W * H) - 1) < 1e-9 && fabs(vm2 / s - 1) < 1e-9, "K4 (b) held: -E alpha dT and its reaction");
}

/* ---- K5: a slab, held and cooled */

static void test_k5(void) {
    printf("K5 heat through a slab to a cooled face\n");
    double L = 0.1, A = 0.02 * 0.03, k = 50, h = 25, T1 = 400, Ti = 300, q = (T1 - Ti) / (L / k + 1 / h);
    char sc[4096];
    snprintf(sc, sizeof sc,
             "{\"domain\":\"kernel\",\"title\":\"K5\",\"mesh\":{\"box\":{\"size_m\":[%g,0.02,0.03],\"cells\":[5,2,2]}},\"order\":1,"
             "\"physics\":[\"heat\"],\"materials\":{\"m\":{\"conductivity_w_mk\":%g,\"source\":\"test\"}},"
             "\"boundaries\":[{\"name\":\"hot\",\"where\":{\"plane\":[\"x\",0]},\"temperature_k\":%g},"
             "{\"name\":\"cooled\",\"where\":{\"plane\":[\"x\",%g]},\"convection\":{\"h_w_m2k\":%g,\"ambient_k\":%g}}],\"run\":{\"frames\":false}}",
             L, k, T1, L, h, Ti);
    JsonValue *r = run("k5", sc);
    double in = r ? num(r, "boundaries", "hot", "heat_in_w") : NAN, out = r ? -num(r, "boundaries", "cooled", "heat_in_w") : NAN;
    double Tc = r ? num(r, "boundaries", "cooled", "mean_temperature_k") : NAN;
    json_free(r);
    printf("  in %.12f W, out by convection %.12f W, closed form %.12f W; cooled face %.9f K (%.9f)\n", in, out, q * A, Tc, T1 - q * L / k);
    verdict(fabs(in / (q * A) - 1) < 1e-9 && fabs(out / (q * A) - 1) < 1e-9 && fabs(Tc - (T1 - q * L / k)) < 1e-9, "K5 the heat balance and the closed form");
}

/* ---- K6: thermal bending without stress */

static void test_k6(void) {
    printf("K6 a bar bent by a temperature gradient, free of stress\n");
    double L = 0.2, h = 0.01, w = 0.01, E = 200e9, al = 1.2e-5, Gr = 1000, c = h / 2;
    char sc[4096];
    snprintf(sc, sizeof sc,
             "{\"domain\":\"kernel\",\"title\":\"K6\",\"mesh\":{\"box\":{\"size_m\":[%g,%g,%g],\"cells\":[10,2,2]}},\"order\":2,"
             "\"physics\":[\"elasticity\"],\"materials\":{\"steel\":{\"youngs_modulus_pa\":200e9,\"poisson_ratio\":0.3,\"expansion_per_k\":1.2e-5,\"source\":\"nominal\"}},"
             "\"reference_temperature_k\":293,\"loads\":{\"temperature_k\":\"293 + %g*(y - %g)\"},"
             "\"boundaries\":[{\"name\":\"sx\",\"where\":{\"plane\":[\"x\",0]},\"fix\":[\"x\"]},{\"name\":\"sz\",\"where\":{\"plane\":[\"z\",0]},\"fix\":[\"z\"]},"
             "{\"name\":\"pin\",\"where\":{\"point\":[0,%g,0]},\"fix\":[\"y\"]}],"
             "\"check\":{\"displacement_m\":[\"%g*(y - %g)*x\",\"%g*((y - %g)^2 - x^2 - z^2)/2\",\"%g*(y - %g)*z\"]},\"run\":{\"frames\":false}}",
             L, h, w, Gr, c, c, al * Gr, c, al * Gr, c, al * Gr, c);
    JsonValue *r = run("k6", sc);
    double e = r ? num(r, "check", "displacement_l2_error_m", NULL) : NAN, n = r ? num(r, "check", "displacement_l2_norm_m", NULL) : NAN;
    double vm = r ? num(r, "results", "max_von_mises_pa", NULL) : NAN;
    json_free(r);
    printf("  L2 error %.3e m against a norm of %.3e m; largest von Mises %.3e Pa (E alpha G h %.3e)\n", e, n, vm, E * al * Gr * h);
    verdict(e < 1e-7 * n && vm < 1e-6 * E * al * Gr * h, "K6 the exact arc, no stress"); /* 1e-9 before the first run: see the note */
}


/* ---- K7, K7b: the C-ring, a box in (r, theta, z) mapped onto the ring by formulas */

static const double RING_RI = 0.010, RING_RO = 0.014, RING_W = 0.004, RING_HALF_GAP = 0.125, RING_F = 10;

/* the ring on nr x nth x nz quadratic elements: the hoop stress's size (Pa) at the apex's inner fibre at mid-width, at
 * the inner fibre on a flat face and at the outer fibre at mid-width (at the apex the hoop direction is x); 0 if the
 * run failed */
static int ring(const char *name, int nr, int nth, int nz, double E, double nu, const char *source, double hoop[3]) {
    double RI = RING_RI, RO = RING_RO, W = RING_W, t0 = -M_PI / 2 + RING_HALF_GAP, t1 = 1.5 * M_PI - RING_HALF_GAP;
    char sc[4096];
    snprintf(sc, sizeof sc,
             "{\"domain\":\"kernel\",\"title\":\"%s\",\"mesh\":{\"box\":{\"size_m\":[%.17g,%.17g,%.17g],\"cells\":[%d,%d,%d],\"origin_m\":[%.17g,%.17g,0]},"
             "\"map_m\":[\"x*cos(y)\",\"x*sin(y)\",\"z\"]},\"order\":2,"
             "\"physics\":[\"elasticity\"],\"materials\":{\"m\":{\"youngs_modulus_pa\":%.17g,\"poisson_ratio\":%.17g,\"source\":\"%s\"}},"
             "\"boundaries\":[{\"name\":\"held\",\"where\":{\"plane\":[\"y\",%.17g],\"space\":\"box\"},\"fixed\":true},"
             "{\"name\":\"pulled\",\"where\":{\"plane\":[\"y\",%.17g],\"space\":\"box\"},\"force_n\":[%g,0,0]}],"
             "\"outputs\":{\"probes\":[{\"name\":\"inner fibre, mid-width\",\"point_m\":[0,%.17g,%.17g]},"
             "{\"name\":\"inner fibre, flat face\",\"point_m\":[0,%.17g,0]},{\"name\":\"outer fibre, mid-width\",\"point_m\":[0,%.17g,%.17g]}]},"
             "\"run\":{\"frames\":false}}",
             name, RO - RI, t1 - t0, W, nr, nth, nz, RI, t0, E, nu, source, t1, t0, RING_F, RI, W / 2, RI, RO, W / 2);
    JsonValue *r = run(name, sc);
    int ok = r != NULL;
    for (int k = 0; k < 3; k++) {
        const JsonValue *st = json_get(json_at(json_get(r, "probes"), (size_t)k), "stress_pa");
        hoop[k] = json_len(st) == 6 ? fabs(json_at(st, 0)->u.number) : NAN;
        ok = ok && isfinite(hoop[k]);
    }
    json_free(r);
    return ok;
}

/* Winkler-Bach's hoop stress at radius r of the apex section (tension positive) */
static double winkler_bach(double r) {
    double RI = RING_RI, RO = RING_RO, A = (RO - RI) * RING_W, rbar = 0.5 * (RI + RO), rn = (RO - RI) / log(RO / RI);
    double M = RING_F * rbar * (1 + cos(RING_HALF_GAP));
    return RING_F / A + M * (rn - r) / (A * (rbar - rn) * r);
}

static void test_k7(void) {
    printf("K7 the C-ring against Winkler-Bach, as written before its first run (not counted: see the note of 2026-10-08)\n");
    double h[3], cf = winkler_bach(RING_RI);
    if (!ring("k7", 4, 120, 4, 3.5e9, 0.35, "nominal PLA", h)) {
        verdict(0, "K7 runs");
        return;
    }
    printf("  hoop stress at the apex's inner fibre %.4f MPa, Winkler-Bach %.4f MPa (%+.2f %%): %s 2 %%\n", h[0] / 1e6, cf / 1e6,
           100 * (h[0] / cf - 1), fabs(h[0] / cf - 1) < 0.02 ? "within" : "not within");
}

static void test_k7b(void) {
    printf("K7b the same ring against CalculiX's solution of the same 3D problem\n");
    /* CalculiX 2.23 on 8 x 160 x 8 C3D20R elements, the nodal stresses it extrapolates and averages, 2026-10-08:
     * python3 tools/printmesh_ccx.py --iso (MPa, printed to two decimals) */
    static const double ccx[3] = {26.22e6, 25.29e6, 19.41e6};
    static const char *where[3] = {"inner fibre, mid-width", "inner fibre, flat face", "outer fibre, mid-width"};
    double h[3], wb[3] = {winkler_bach(RING_RI), winkler_bach(RING_RI), fabs(winkler_bach(RING_RO))};
    int ran = ring("k7b", 8, 120, 4, 3426.9e6, 0.323, "pla_fff made isotropic as tools/printmesh_ccx.py does", h), ok = ran;
    for (int k = 0; k < 3 && ran; k++) {
        printf("  %s: %.3f MPa, CalculiX %.2f MPa (%+.2f %%), Winkler-Bach %.3f MPa\n", where[k], h[k] / 1e6, ccx[k] / 1e6,
               100 * (h[k] / ccx[k] - 1), wb[k] / 1e6);
        ok = ok && fabs(h[k] / ccx[k] - 1) < 0.01;
    }
    verdict(ok, "K7b within 1 % of CalculiX at all three places");
}

/* ---- K8: a thick-walled tube read from an STL file, against Lame */

/* an ASCII STL of a tube (radii ri, ro, length L, n facets around), wound outward; 0 if it cannot be written */
static int write_tube_stl(const char *path, double ri, double ro, double L, int n) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "solid tube\n");
#define TRI(a, b, c) fprintf(f, "facet normal 0 0 0\nouter loop\nvertex %.17g %.17g %.17g\nvertex %.17g %.17g %.17g\nvertex %.17g %.17g %.17g\nendloop\nendfacet\n", \
                             a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2])
    for (int k = 0; k < n; k++) {
        double t0 = 2 * M_PI * k / n, t1 = 2 * M_PI * (k + 1) / n, c0 = cos(t0), s0 = sin(t0), c1 = cos(t1), s1 = sin(t1);
        double o00[3] = {ro * c0, ro * s0, 0}, o10[3] = {ro * c1, ro * s1, 0}, o01[3] = {ro * c0, ro * s0, L}, o11[3] = {ro * c1, ro * s1, L};
        double i00[3] = {ri * c0, ri * s0, 0}, i10[3] = {ri * c1, ri * s1, 0}, i01[3] = {ri * c0, ri * s0, L}, i11[3] = {ri * c1, ri * s1, L};
        TRI(o00, o10, o11), TRI(o00, o11, o01);  /* outer wall, facing out */
        TRI(i00, i11, i10), TRI(i00, i01, i11);  /* the bore, facing the axis */
        TRI(i01, o01, o11), TRI(i01, o11, i11);  /* the end at z = L, facing +z */
        TRI(i00, i10, o10), TRI(i00, o10, o00);  /* the end at z = 0, facing -z */
    }
#undef TRI
    fprintf(f, "endsolid tube\n");
    return fclose(f) == 0;
}

static void test_k8(void) {
    printf("K8 a thick-walled tube read from an STL file, against Lame\n");
    double ri = 0.010, ro = 0.015, L = 0.010, p = 10e6, E = 200e9, nu = 0.3, eps = 1e-6;
    char stl[700], sc[4096];
    snprintf(stl, sizeof stl, "%s/tube.stl", workdir);
    if (!write_tube_stl(stl, ri, ro, L, 256)) {
        verdict(0, "K8 writes its STL");
        return;
    }
    snprintf(sc, sizeof sc,
             "{\"domain\":\"kernel\",\"title\":\"K8\",\"mesh\":{\"stl\":\"%s\",\"element_size_m\":0.001},\"order\":2,"
             "\"physics\":[\"elasticity\"],\"materials\":{\"steel\":{\"youngs_modulus_pa\":%g,\"poisson_ratio\":%g,\"source\":\"nominal steel\"}},"
             "\"boundaries\":[{\"name\":\"bore\",\"where\":{\"cylinder\":{\"point_m\":[0,0,0],\"axis\":[0,0,1],\"radius_m\":%g}},\"pressure_pa\":%g},"
             "{\"name\":\"end\",\"where\":{\"plane\":[\"z\",0]},\"fix\":[\"z\"]},"
             "{\"name\":\"pin\",\"where\":{\"point\":[%g,0,0]},\"fix\":[\"x\",\"y\"]},{\"name\":\"turn\",\"where\":{\"point\":[%g,0,0]},\"fix\":[\"y\"]}],"
             "\"outputs\":{\"probes\":[{\"name\":\"+x\",\"point_m\":[%.17g,0,%g]},{\"name\":\"-x\",\"point_m\":[%.17g,0,%g]},"
             "{\"name\":\"+y\",\"point_m\":[0,%.17g,%g]},{\"name\":\"-y\",\"point_m\":[0,%.17g,%g]}]},\"run\":{\"frames\":false}}",
             stl, E, nu, ri, p, ro, -ro, ri + eps, L / 2, -(ri + eps), L / 2, ri + eps, L / 2, -(ri + eps), L / 2);
    JsonValue *r = run("k8", sc);
    unlink(stl);
    if (!r) {
        verdict(0, "K8 runs");
        return;
    }
    double d = ro * ro - ri * ri, rp = ri + eps;
    double hoop = p * ri * ri / d * (1 + ro * ro / (rp * rp)), radial = p * ri * ri / d * (1 - ro * ro / (rp * rp));
    double u = rp / E * (p * ri * ri / d) * ((1 - nu) + (1 + nu) * ro * ro / (rp * rp));
    double vm_bore = sqrt(hoop * hoop + radial * radial - hoop * radial); /* sigma_z = 0 */
    const JsonValue *pr = json_get(r, "probes");
    double sxy[2], ux[2];
    /* the hoop direction is y on the x axis and x on the y axis; half the change of the diameter along each */
    const JsonValue *s0 = json_get(json_at(pr, 0), "stress_pa"), *s2 = json_get(json_at(pr, 2), "stress_pa");
    sxy[0] = json_len(s0) == 6 ? json_at(s0, 1)->u.number : NAN;
    sxy[1] = json_len(s2) == 6 ? json_at(s2, 0)->u.number : NAN;
    for (int k = 0; k < 2; k++) {
        const JsonValue *a = json_get(json_at(pr, (size_t)(2 * k)), "displacement_m"), *b = json_get(json_at(pr, (size_t)(2 * k + 1)), "displacement_m");
        ux[k] = json_len(a) == 3 && json_len(b) == 3 ? 0.5 * (json_at(a, (size_t)k)->u.number - json_at(b, (size_t)k)->u.number) : NAN;
    }
    double vm = num(r, "results", "max_von_mises_pa", NULL);
    const JsonValue *m = json_get(r, "mesh");
    printf("  mesh: %.0f tetrahedra from %.0f STL triangles, dihedral angles %.1f to %.1f degrees, volume %+.3f %% of the file's\n",
           num(m, "tetrahedra", NULL, NULL), num(m, "stl_triangles", NULL, NULL), num(m, "min_dihedral_deg", NULL, NULL),
           num(m, "max_dihedral_deg", NULL, NULL), 100 * (num(m, "volume_m3", NULL, NULL) / num(m, "stl_volume_m3", NULL, NULL) - 1));
    printf("  hoop stress at the bore: %.4f MPa on x, %.4f MPa on y, Lame %.4f MPa (%+.2f %%, %+.2f %%)\n", sxy[0] / 1e6, sxy[1] / 1e6, hoop / 1e6,
           100 * (sxy[0] / hoop - 1), 100 * (sxy[1] / hoop - 1));
    printf("  the bore's radial displacement: %.5f um along x, %.5f um along y, Lame %.5f um (%+.3f %%, %+.3f %%)\n", 1e6 * ux[0], 1e6 * ux[1], 1e6 * u,
           100 * (ux[0] / u - 1), 100 * (ux[1] / u - 1));
    printf("  largest von Mises %.4f MPa, Lame's at the bore %.4f MPa (%+.2f %%)\n", vm / 1e6, vm_bore / 1e6, 100 * (vm / vm_bore - 1));
    json_free(r);
    int ok = 1;
    for (int k = 0; k < 2; k++) ok = ok && fabs(sxy[k] / hoop - 1) < 0.02 && fabs(ux[k] / u - 1) < 0.01;
    verdict(ok && fabs(vm / vm_bore - 1) < 0.03, "K8 the hoop stress within 2 %, the bore's growth within 1 %, the largest von Mises within 3 %");
}

/* ---- K9: the engineering core's wall brackets, read from their STL files */

/* one bracket on 4 mm tetrahedra of the given order: the pad's mean displacement down (m), the wall's reaction along z
 * (N) and the pad's area (m^2); 0 if the run failed */
static int bracket(const char *name, const char *stl, int order, double out[3]) {
    char sc[4096];
    snprintf(sc, sizeof sc,
             "{\"domain\":\"kernel\",\"title\":\"%s\",\"mesh\":{\"stl\":\"%s\",\"scale\":0.001,\"element_size_m\":0.004},\"order\":%d,"
             "\"physics\":[\"elasticity\"],\"materials\":{\"al6061\":{\"youngs_modulus_pa\":68.9e9,\"poisson_ratio\":0.33,"
             "\"source\":\"nominal handbook values for 6061-T6, as examples/bracket_comparison/study.json states them\"}},"
             "\"boundaries\":[{\"name\":\"wall\",\"where\":{\"plane\":[\"x\",0]},\"fixed\":true},"
             "{\"name\":\"pad\",\"where\":{\"box\":[[0.064,0.012,0.0599],[0.080,0.028,0.0601]]},\"force_n\":[0,0,%.17g]}],"
             "\"run\":{\"frames\":false}}",
             name, stl, order, -3 * 9.80665);
    JsonValue *r = run(name, sc);
    if (!r) return 0;
    out[0] = -vec(r, "boundaries", "pad", "mean_displacement_m", 2);
    out[1] = vec(r, "boundaries", "wall", "reaction_n", 2);
    out[2] = num(r, "boundaries", "pad", "area_m2");
    json_free(r);
    return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
}

static void test_k9(void) {
    printf("K9 the engineering core's wall brackets, read from their STL files\n");
    static const char *stl[2] = {"examples/bracket_comparison/geometry/bracket_a_plain.stl", "examples/bracket_comparison/geometry/bracket_b_chamfer.stl"};
    /* examples/bracket_comparison/expected_output/evidence.json: A extrapolated, its grid convergence index; both at 1 mm */
    const double core_a = 0.02560279690901558e-3, core_gci = 0.024, core_1mm[2] = {0.025399428205250717e-3, 0.008774553670408315e-3};
    const double F = 3 * 9.80665;
    double v[2][2][3]; /* design A, B; quadratic, cubic; the pad's displacement, the wall's reaction, the pad's area */
    for (int d = 0; d < 2; d++)
        for (int o = 0; o < 2; o++) {
            char name[16];
            snprintf(name, sizeof name, "k9%c%d", 'a' + d, o + 2);
            if (!bracket(name, stl[d], o + 2, v[d][o])) {
                verdict(0, "K9 runs");
                return;
            }
        }
    int ok = 1;
    for (int d = 0; d < 2; d++) {
        printf("  %c: the pad moves %.6f mm on quadratic and %.6f mm on cubic elements (%+.2f %%), the core's voxels at 1 mm %.6f mm; "
               "the pad %.1f mm^2, the wall holds %.6f N (the load %.6f N)\n",
               'A' + d, 1e3 * v[d][0][0], 1e3 * v[d][1][0], 100 * (v[d][0][0] / v[d][1][0] - 1), 1e3 * core_1mm[d], 1e6 * v[d][1][2], v[d][1][1], F);
        for (int o = 0; o < 2; o++) ok = ok && fabs(fabs(v[d][o][1]) - F) < 1e-6 * F;
        ok = ok && fabs(v[d][0][0] / v[d][1][0] - 1) < 0.02;
    }
    printf("  A on cubic elements against the core's extrapolated %.6f mm: %+.2f %% (its index %.1f %%); B / A %.4f (the core's voxels at 1 mm %.4f)\n",
           1e3 * core_a, 100 * (v[0][1][0] / core_a - 1), 100 * core_gci, v[1][1][0] / v[0][1][0], core_1mm[1] / core_1mm[0]);
    ok = ok && fabs(v[0][1][0] / core_a - 1) < core_gci && v[1][1][0] < v[0][1][0];
    verdict(ok, "K9 A within the core's 2.4 %, both settled within 2 % from quadratic to cubic, B under A, the wall holds the load");
}

/* ---- K10 to K14: equations an agent writes, in time */

/* K10: the time stepping's order on a field quadratic in space (held exactly by quadratic elements) */
static double k10_error(const char *method, int steps) {
    char sc[2048], name[32];
    snprintf(name, sizeof name, "k10%c%d", method[0], steps);
    snprintf(sc, sizeof sc,
             "{\"domain\":\"kernel\",\"title\":\"K10\",\"mesh\":{\"box\":{\"size_m\":[1,1,1],\"cells\":[2,2,2]}},\"order\":2,"
             "\"fields\":{\"u\":{\"mass\":1,\"diffusion\":1,\"initial\":\"x^2+y^2+z^2\",\"source\":\"-(x^2+y^2+z^2)*exp(-t) - 6*exp(-t)\"}},"
             "\"boundaries\":[{\"name\":\"all\",\"where\":\"all\",\"value\":{\"u\":\"(x^2+y^2+z^2)*exp(-t)\"}}],"
             "\"time\":{\"end_s\":1,\"step_s\":%.17g,\"method\":\"%s\",\"outputs\":1},"
             "\"check\":{\"fields\":{\"u\":\"(x^2+y^2+z^2)*exp(-t)\"}},\"run\":{\"frames\":false}}",
             1.0 / steps, method);
    JsonValue *r = run(name, sc);
    double e = r ? num(json_get(json_get(r, "check"), "fields"), "u", "l2_error", NULL) : NAN;
    json_free(r);
    return e;
}

static void test_k10(void) {
    printf("K10 the time stepping's order, the error left only in time\n");
    static const char *methods[2] = {"backward_euler", "bdf2"};
    const double need[2] = {0.9, 1.8};
    int ok = 1;
    for (int m = 0; m < 2; m++) {
        double e5 = k10_error(methods[m], 5), e10 = k10_error(methods[m], 10), e20 = k10_error(methods[m], 20), rate = log2(e10 / e20);
        printf("  %s: L2 error at t = 1 %.4e (5 steps), %.4e (10), %.4e (20): order %.3f\n", methods[m], e5, e10, e20, rate);
        ok = ok && isfinite(rate) && rate >= need[m];
    }
    verdict(ok, "K10 backward Euler at order 0.9 or more, BDF2 at 1.8 or more");
}

/* K11: conduction into a semi-infinite solid, the heat equation written as a field */
static void test_k11(void) {
    printf("K11 conduction into a semi-infinite solid, written as a field\n");
    double rc = 3.6e6, k = 45, T0 = 300, dT = 100, t = 20, alpha = k / rc;
    const double xs[3] = {0.005, 0.010, 0.020};
    char sc[4096];
    snprintf(sc, sizeof sc,
             "{\"domain\":\"kernel\",\"title\":\"K11\",\"mesh\":{\"box\":{\"size_m\":[0.1,0.01,0.01],\"cells\":[100,1,1]}},\"order\":2,"
             "\"fields\":{\"T\":{\"mass\":%g,\"diffusion\":%g,\"initial\":%g,\"unit\":\"K\"}},"
             "\"boundaries\":[{\"name\":\"heated end\",\"where\":{\"plane\":[\"x\",0]},\"value\":{\"T\":%g}}],"
             "\"time\":{\"end_s\":%g,\"step_s\":0.1,\"method\":\"bdf2\",\"outputs\":1},"
             "\"outputs\":{\"probes\":[{\"name\":\"5 mm\",\"point_m\":[%g,0.005,0.005]},{\"name\":\"10 mm\",\"point_m\":[%g,0.005,0.005]},"
             "{\"name\":\"20 mm\",\"point_m\":[%g,0.005,0.005]}]},\"run\":{\"frames\":false}}",
             rc, k, T0, T0 + dT, t, xs[0], xs[1], xs[2]);
    JsonValue *r = run("k11", sc);
    const JsonValue *pr = json_get(r, "probes");
    int ok = r != NULL;
    for (int i = 0; i < 3 && r; i++) {
        double T = num(json_at(pr, (size_t)i), "T", NULL, NULL), ex = T0 + dT * erfc(xs[i] / (2 * sqrt(alpha * t)));
        printf("  x = %2.0f mm: %.4f K, erfc's %.4f K (%+.3f %% of the step)\n", 1e3 * xs[i], T, ex, 100 * (T - ex) / dT);
        ok = ok && fabs(T - ex) < 0.005 * dT;
    }
    json_free(r);
    verdict(ok, "K11 within 0.5 % of the step at 5, 10 and 20 mm");
}

/* K12: Joule heating, two fields coupled through a gradient */
static void test_k12(void) {
    printf("K12 Joule heating: a potential and a temperature coupled through a gradient\n");
    double L = 0.1, sigma = 5.8e7, k = 400, V = 0.01, rise = sigma * V * V / (8 * k);
    char sc[4096];
    snprintf(sc, sizeof sc,
             "{\"domain\":\"kernel\",\"title\":\"K12\",\"mesh\":{\"box\":{\"size_m\":[%g,0.01,0.01],\"cells\":[10,1,1]}},\"order\":2,"
             "\"fields\":{\"phi\":{\"diffusion\":%g,\"unit\":\"V\"},"
             "\"T\":{\"diffusion\":%g,\"source\":\"%g*(dx(phi)^2 + dy(phi)^2 + dz(phi)^2)\",\"initial\":300,\"unit\":\"K\"}},"
             "\"boundaries\":[{\"name\":\"plus\",\"where\":{\"plane\":[\"x\",0]},\"value\":{\"phi\":%g,\"T\":300}},"
             "{\"name\":\"minus\",\"where\":{\"plane\":[\"x\",%g]},\"value\":{\"phi\":0,\"T\":300}}],"
             "\"outputs\":{\"probes\":[{\"name\":\"middle\",\"point_m\":[%g,0.005,0.005]}]},"
             "\"check\":{\"fields\":{\"phi\":\"%g*(1 - x/%g)\"}},\"run\":{\"frames\":false}}",
             L, sigma, k, sigma, V, L, L / 2, V, L);
    JsonValue *r = run("k12", sc);
    const JsonValue *cp = json_get(json_get(json_get(r, "check"), "fields"), "phi");
    double e = num(cp, "l2_error", NULL, NULL), n = num(cp, "l2_norm", NULL, NULL);
    double Tm = num(json_at(json_get(r, "probes"), 0), "T", NULL, NULL);
    printf("  phi: L2 error %.3e against a norm of %.3e; T at mid-length %.12f K, exact %.12f K (%.2e of the rise)\n", e, n, Tm, 300 + rise,
           fabs(Tm - 300 - rise) / rise);
    printf("  solved in %.0f passes over the two fields\n", num(r, "solver", "iterations_max", NULL));
    json_free(r);
    verdict(e < 1e-9 * n && fabs(Tm - 300 - rise) < 1e-9 * rise, "K12 phi to 1e-9 of its norm, the temperature to 1e-9 of its rise");
}

/* K13: a conductivity that depends on the temperature, against Kirchhoff's transform */
static void test_k13(void) {
    printf("K13 a conductivity that depends on the temperature itself\n");
    double L = 0.01, k0 = 45, beta = 0.01, T0 = 300, T1 = 400, d1 = T1 - T0, K1 = d1 + beta * d1 * d1 / 2;
    double ex = T0 + (-1 + sqrt(1 + 2 * beta * 0.5 * K1)) / beta;
    char sc[4096];
    snprintf(sc, sizeof sc,
             "{\"domain\":\"kernel\",\"title\":\"K13\",\"mesh\":{\"box\":{\"size_m\":[%g,0.01,0.01],\"cells\":[20,1,1]}},\"order\":2,"
             "\"fields\":{\"T\":{\"diffusion\":\"%g*(1 + %g*(T - %g))\",\"initial\":%g,\"unit\":\"K\"}},"
             "\"boundaries\":[{\"name\":\"cold\",\"where\":{\"plane\":[\"x\",0]},\"value\":{\"T\":%g}},"
             "{\"name\":\"hot\",\"where\":{\"plane\":[\"x\",%g]},\"value\":{\"T\":%g}}],"
             "\"outputs\":{\"probes\":[{\"name\":\"middle\",\"point_m\":[%g,0.005,0.005]}]},\"run\":{\"frames\":false}}",
             L, k0, beta, T0, T0, T0, L, T1, L / 2);
    JsonValue *r = run("k13", sc);
    double Tm = num(json_at(json_get(r, "probes"), 0), "T", NULL, NULL), it = num(r, "solver", "iterations_max", NULL);
    double ch = num(r, "solver", "last_change", NULL);
    int conv = json_get_bool(json_get(r, "solver"), "converged", false);
    printf("  %.0f iterations, the last change %.3e K (converged: %s); mid-thickness %.6f K, Kirchhoff %.6f K (%.2e of the difference)\n", it, ch,
           conv ? "yes" : "no", Tm, ex, fabs(Tm - ex) / d1);
    json_free(r);
    verdict(conv && it <= 50 && ch < 1e-10 * d1 && fabs(Tm - ex) < 1e-4 * d1, "K13 settles within 50 iterations to Kirchhoff's within 1e-4");
}

/* K14: convection and diffusion, a non-symmetric system */
static void test_k14(void) {
    printf("K14 convection and diffusion, a non-symmetric system\n");
    char sc[4096];
    snprintf(sc, sizeof sc,
             "{\"domain\":\"kernel\",\"title\":\"K14\",\"mesh\":{\"box\":{\"size_m\":[1,0.1,0.1],\"cells\":[80,1,1]}},\"order\":2,"
             "\"fields\":{\"u\":{\"diffusion\":0.1,\"convection\":[1,0,0]}},"
             "\"boundaries\":[{\"name\":\"in\",\"where\":{\"plane\":[\"x\",0]},\"value\":{\"u\":0}},{\"name\":\"out\",\"where\":{\"plane\":[\"x\",1]},\"value\":{\"u\":1}}],"
             "\"check\":{\"fields\":{\"u\":\"(exp(10*x) - 1)/(exp(10) - 1)\"}},\"run\":{\"frames\":false}}");
    JsonValue *r = run("k14", sc);
    double e = num(json_get(json_get(r, "check"), "fields"), "u", "l2_error", NULL), rms = e / sqrt(1 * 0.1 * 0.1);
    printf("  root-mean-square error %.3e; the system: %s\n", rms, json_get_str(json_get(json_get(r, "fields"), "u"), "system", "?"));
    json_free(r);
    verdict(rms < 1e-4, "K14 within 1e-4 of the exact layer");
}

/* K15: the first time a probe crosses a value, on K11's bar */
static void test_k15(void) {
    printf("K15 the first time a probe crosses a value\n");
    double rc = 3.6e6, k = 45, alpha = k / rc, x = 0.010, lo = 0, hi = 2, z;
    for (int i = 0; i < 200; i++) z = 0.5 * (lo + hi), (erfc(z) > 0.5 ? (lo = z) : (hi = z)); /* erfc^-1(1/2) */
    z = 0.5 * (lo + hi);
    double ex = pow(x / (2 * z), 2) / alpha;
    char sc[4096];
    snprintf(sc, sizeof sc,
             "{\"domain\":\"kernel\",\"title\":\"K15\",\"mesh\":{\"box\":{\"size_m\":[0.1,0.01,0.01],\"cells\":[100,1,1]}},\"order\":2,"
             "\"fields\":{\"T\":{\"mass\":%g,\"diffusion\":%g,\"initial\":300,\"unit\":\"K\"}},"
             "\"boundaries\":[{\"name\":\"heated end\",\"where\":{\"plane\":[\"x\",0]},\"value\":{\"T\":400}}],"
             "\"time\":{\"end_s\":20,\"step_s\":0.1,\"method\":\"bdf2\",\"outputs\":1},"
             "\"outputs\":{\"probes\":[{\"name\":\"10 mm\",\"point_m\":[%g,0.005,0.005]}],\"crossings\":[{\"probe\":\"10 mm\",\"field\":\"T\",\"value\":350}]},"
             "\"run\":{\"frames\":false}}",
             rc, k, x);
    JsonValue *r = run("k15", sc);
    double t = num(json_at(json_get(r, "crossings"), 0), "first_s", NULL, NULL);
    printf("  350 K at 10 mm first reached at %.4f s; erfc's %.4f s (%+.3f %%)\n", t, ex, 100 * (t / ex - 1));
    json_free(r);
    verdict(isfinite(t) && fabs(t / ex - 1) < 0.005, "K15 within 0.5 % of erfc's time");
}

/* K16: the flow of a field through each boundary, on K13's slab */
static void test_k16(void) {
    printf("K16 the flow of a field through each boundary\n");
    double L = 0.01, A = 1e-4, k0 = 45, beta = 0.01, d1 = 100, Q = k0 * (d1 + beta * d1 * d1 / 2) * A / L;
    char sc[4096];
    snprintf(sc, sizeof sc,
             "{\"domain\":\"kernel\",\"title\":\"K16\",\"mesh\":{\"box\":{\"size_m\":[%g,0.01,0.01],\"cells\":[20,1,1]}},\"order\":2,"
             "\"fields\":{\"T\":{\"diffusion\":\"%g*(1 + %g*(T - 300))\",\"initial\":300,\"unit\":\"K\"}},"
             "\"boundaries\":[{\"name\":\"cold\",\"where\":{\"plane\":[\"x\",0]},\"value\":{\"T\":300}},"
             "{\"name\":\"hot\",\"where\":{\"plane\":[\"x\",%g]},\"value\":{\"T\":400}}],\"run\":{\"frames\":false}}",
             L, k0, beta, L);
    JsonValue *r = run("k16", sc);
    double hot = num(json_get(json_get(r, "boundaries"), "hot"), "flow_in", "T", NULL);
    double cold = num(json_get(json_get(r, "boundaries"), "cold"), "flow_in", "T", NULL);
    printf("  in through the hot face %.9f W, through the cold face %.9f W; Kirchhoff %.9f W (%.2e, %.2e)\n", hot, cold, Q, fabs(hot / Q - 1),
           fabs(-cold / Q - 1));
    json_free(r);
    verdict(fabs(hot / Q - 1) < 1e-6 && fabs(-cold / Q - 1) < 1e-6, "K16 both faces within 1e-6 of Kirchhoff's 67.5 W");
}

/* K17: two bonded materials bent by heat, against Timoshenko's bimetal formula */
static void test_k17(void) {
    printf("K17 a bimetal strip bent by heat, against Timoshenko\n");
    double L = 0.040, w = 0.001, t = 0.0005, E1 = 124e9, E2 = 148e9, a1 = 26.46e-6, a2 = 1.30e-6, dT = 80;
    double m = 1, n = E1 / E2, h = 2 * t;
    double k = 6 * (a1 - a2) * dT * (1 + m) * (1 + m) / (h * (3 * (1 + m) * (1 + m) + (1 + m * n) * (m * m + 1 / (m * n))));
    char sc[4096];
    snprintf(sc, sizeof sc,
             "{\"domain\":\"kernel\",\"title\":\"K17\",\"mesh\":{\"box\":{\"size_m\":[%g,%g,%g],\"cells\":[40,2,4]}},\"order\":2,"
             "\"physics\":[\"elasticity\"],\"materials\":{\"active\":{\"youngs_modulus_pa\":%g,\"poisson_ratio\":0,\"expansion_per_k\":%g,\"source\":\"K17's values\"},"
             "\"passive\":{\"youngs_modulus_pa\":%g,\"poisson_ratio\":0,\"expansion_per_k\":%g,\"source\":\"K17's values\"}},"
             "\"regions\":[{\"where\":\"all\",\"material\":\"active\"},{\"where\":{\"box\":[[0,0,0],[%g,%g,%g]]},\"material\":\"passive\"}],"
             "\"reference_temperature_k\":293.15,\"loads\":{\"temperature_k\":%g},"
             "\"boundaries\":[{\"name\":\"a\",\"where\":{\"point\":[0,0,0]},\"fix\":[\"x\",\"y\",\"z\"]},{\"name\":\"b\",\"where\":{\"point\":[%g,0,0]},\"fix\":[\"y\",\"z\"]},"
             "{\"name\":\"c\",\"where\":{\"point\":[0,%g,0]},\"fix\":[\"z\"]}],"
             "\"outputs\":{\"probes\":[{\"name\":\"15\",\"point_m\":[0.015,%g,%g]},{\"name\":\"20\",\"point_m\":[0.020,%g,%g]},{\"name\":\"25\",\"point_m\":[0.025,%g,%g]}]},"
             "\"run\":{\"frames\":false}}",
             L, w, h, E1, a1, E2, a2, L, w, t, 293.15 + dT, L, w, w / 2, t, w / 2, t, w / 2, t);
    JsonValue *r = run("k17", sc);
    const JsonValue *pr = json_get(r, "probes");
    double u[3];
    for (int i = 0; i < 3; i++) {
        const JsonValue *d = json_get(json_at(pr, (size_t)i), "displacement_m");
        u[i] = json_len(d) == 3 ? json_at(d, 2)->u.number : NAN;
    }
    json_free(r);
    double kfe = fabs(u[0] - 2 * u[1] + u[2]) / (0.005 * 0.005);
    printf("  curvature at mid-length %.6f 1/m, Timoshenko %.6f 1/m (%+.4f %%); specific curvature k h / dT %.4e /K\n", kfe, k, 100 * (kfe / k - 1),
           kfe * h / dT);
    verdict(isfinite(kfe) && fabs(kfe / k - 1) < 1e-3, "K17 within 0.1 % of Timoshenko");
}

/* ---- K18, K19: contact with rigid obstacles */

/* K18: a block resting on a frictionless rigid plane under a uniform pressure */
static void test_k18(void) {
    printf("K18 a block resting on a rigid plane, pressed by 10 MPa\n");
    double p = 10e6, E = 200e9, H = 0.005, A = 1e-4;
    char sc[4096];
    snprintf(sc, sizeof sc,
             "{\"domain\":\"kernel\",\"title\":\"K18\",\"mesh\":{\"box\":{\"size_m\":[0.01,0.01,%g],\"cells\":[4,4,2]}},\"order\":2,"
             "\"physics\":[\"elasticity\"],\"materials\":{\"steel\":{\"youngs_modulus_pa\":%g,\"poisson_ratio\":0.3,\"source\":\"nominal steel\"}},"
             "\"boundaries\":[{\"name\":\"floor\",\"where\":{\"plane\":[\"z\",0]},\"contact\":{\"plane\":{\"point_m\":[0,0,0],\"normal\":[0,0,1]},\"report_points\":true}},"
             "{\"name\":\"sym x\",\"where\":{\"plane\":[\"x\",0]},\"fix\":[\"x\"]},{\"name\":\"sym y\",\"where\":{\"plane\":[\"y\",0]},\"fix\":[\"y\"]},"
             "{\"name\":\"top\",\"where\":{\"plane\":[\"z\",%g]},\"pressure_pa\":%g}],\"run\":{\"frames\":false}}",
             H, E, H, p);
    JsonValue *r = run("k18", sc);
    const JsonValue *c = json_get(json_get(json_get(r, "boundaries"), "floor"), "contact"), *pts = json_get(c, "points");
    double worst = 0;
    for (size_t i = 0; i < json_len(pts); i++) worst = fmax(worst, fabs(json_at(json_at(pts, i), 3)->u.number / p - 1));
    double Fz = num(c, "force_n", NULL, NULL);
    const JsonValue *fv = json_get(c, "force_n");
    Fz = json_len(fv) == 3 ? json_at(fv, 2)->u.number : NAN;
    double pen = num(c, "max_penetration_m", NULL, NULL), comp = p * H / E;
    printf("  %zu points, pressure within %.2e of 10 MPa at the worst; resultant %.9f N (load %.9f N); deepest penetration %.2e of the compression\n",
           json_len(pts), worst, Fz, p * A, pen / comp);
    printf("  %g Newton iterations, %g multiplier updates, converged: %s\n", num(r, "results", "contact", "newton_iterations"),
           num(r, "results", "contact", "multiplier_updates"), json_get_bool(json_get(json_get(r, "results"), "contact"), "converged", false) ? "yes" : "no");
    size_t npts = json_len(pts);
    json_free(r);
    verdict(npts > 0 && worst < 1e-6 && fabs(Fz / (p * A) - 1) < 1e-9 && pen < 1e-6 * comp, "K18 uniform 10 MPa, the load carried, no penetration");
}

/* K19: Hertz's line contact, a cylinder pressed on a rigid plane */
static void test_k19(void) {
    printf("K19 Hertz's line contact: a cylinder pressed on a rigid plane\n");
    double R = 0.010, r0 = 0.0025, t = 0.001, E = 200e9, nu = 0.3, d = 5e-5;
    char sc[4096];
    snprintf(sc, sizeof sc,
             "{\"domain\":\"kernel\",\"title\":\"K19\",\"mesh\":{\"box\":{\"size_m\":[1,1,%g],\"cells\":[12,48,1]},"
             "\"map_m\":[\"(%g - %g*(1-x)^2)*cos(1.5*pi + 0.5*pi*y^2)\",\"(%g - %g*(1-x)^2)*sin(1.5*pi + 0.5*pi*y^2)\",\"z\"]},\"order\":2,"
             "\"physics\":[\"elasticity\"],\"materials\":{\"steel\":{\"youngs_modulus_pa\":%g,\"poisson_ratio\":%g,\"source\":\"nominal steel\"}},"
             "\"boundaries\":[{\"name\":\"hub\",\"where\":{\"plane\":[\"x\",0],\"space\":\"box\"},\"displacement_m\":[0,%g,0]},"
             "{\"name\":\"symmetry\",\"where\":{\"plane\":[\"y\",0],\"space\":\"box\"},\"fix\":[\"x\"]},"
             "{\"name\":\"front\",\"where\":{\"plane\":[\"z\",0]},\"fix\":[\"z\"]},{\"name\":\"back\",\"where\":{\"plane\":[\"z\",%g]},\"fix\":[\"z\"]},"
             "{\"name\":\"rim\",\"where\":{\"plane\":[\"x\",1],\"space\":\"box\"},\"contact\":{\"plane\":{\"point_m\":[0,%g,0],\"normal\":[0,1,0]},\"report_points\":true}}],"
             "\"run\":{\"frames\":false}}",
             t, R, R - r0, R, R - r0, E, nu, -d, t, -R);
    JsonValue *r = run("k19", sc);
    const JsonValue *c = json_get(json_get(json_get(r, "boundaries"), "rim"), "contact"), *pts = json_get(c, "points");
    const JsonValue *fv = json_get(c, "force_n"), *hv = json_get(json_get(json_get(r, "boundaries"), "hub"), "reaction_n");
    double Fy = json_len(fv) == 3 ? json_at(fv, 1)->u.number : NAN, Hy = json_len(hv) == 3 ? json_at(hv, 1)->u.number : NAN;
    double Es = E / (1 - nu * nu), Fl = 2 * Fy / t; /* the whole contact's force per unit length: the model is one half */
    double p0 = sqrt(Fl * Es / (M_PI * R)), b = sqrt(4 * Fl * R / (M_PI * Es)), pmax = 0, half = 0;
    for (size_t i = 0; i < json_len(pts); i++) {
        const JsonValue *q = json_at(pts, i);
        double x = json_at(q, 0)->u.number, pr = json_at(q, 3)->u.number;
        pmax = fmax(pmax, pr);
        if (pr > 0.01 * p0) half = fmax(half, fabs(x));
    }
    printf("  resultant %.3f N, the hub's reaction %.3f N (%.1e); per unit length of the whole contact %.4e N/m\n", Fy, Hy, fabs(Fy + Hy) / fabs(Fy), Fl);
    printf("  peak pressure %.4e Pa, Hertz %.4e Pa (%+.2f %%); half width %.4f mm, Hertz %.4f mm (%+.1f %%)\n", pmax, p0, 100 * (pmax / p0 - 1), 1e3 * half,
           1e3 * b, 100 * (half / b - 1));
    printf("  %g Newton iterations, %g multiplier updates, converged: %s\n", num(r, "results", "contact", "newton_iterations"),
           num(r, "results", "contact", "multiplier_updates"), json_get_bool(json_get(json_get(r, "results"), "contact"), "converged", false) ? "yes" : "no");
    json_free(r);
    verdict(isfinite(Fy) && fabs(Fy + Hy) < 1e-6 * fabs(Fy) && fabs(pmax / p0 - 1) < 0.03 && fabs(half / b - 1) < 0.10,
            "K19 equilibrium to 1e-6, the peak pressure within 3 %, the half width within 10 % of Hertz");
}

/* K20: a field sampled along a line, on K12's Joule-heated bar */
static void test_k20(void) {
    printf("K20 a field sampled along a line\n");
    double L = 0.1, sigma = 5.8e7, k = 400, V = 0.01, rise = sigma * V * V / (8 * k);
    char sc[4096];
    snprintf(sc, sizeof sc,
             "{\"domain\":\"kernel\",\"title\":\"K20\",\"mesh\":{\"box\":{\"size_m\":[%g,0.01,0.01],\"cells\":[10,1,1]}},\"order\":2,"
             "\"fields\":{\"phi\":{\"diffusion\":%g},\"T\":{\"diffusion\":%g,\"source\":\"%g*(dx(phi)^2 + dy(phi)^2 + dz(phi)^2)\",\"initial\":300}},"
             "\"boundaries\":[{\"name\":\"plus\",\"where\":{\"plane\":[\"x\",0]},\"value\":{\"phi\":%g,\"T\":300}},"
             "{\"name\":\"minus\",\"where\":{\"plane\":[\"x\",%g]},\"value\":{\"phi\":0,\"T\":300}}],"
             "\"outputs\":{\"lines\":[{\"name\":\"axis\",\"from_m\":[0,0.005,0.005],\"to_m\":[%g,0.005,0.005],\"points\":21}]},\"run\":{\"frames\":false}}",
             L, sigma, k, sigma, V, L, L);
    JsonValue *r = run("k20", sc);
    const JsonValue *ln = json_get(json_get(r, "lines"), "axis"), *T = json_get(ln, "T"), *sa = json_get(ln, "s_m"), *pr = json_get(r, "probes");
    double worst = json_len(T) == 21 ? 0 : INFINITY;
    for (size_t i = 0; i < json_len(T) && i < json_len(sa); i++) {
        double x = json_at(sa, i)->u.number, ex = 300 + sigma * V * V * x * (L - x) / (2 * k * L * L);
        worst = fmax(worst, fabs(json_at(T, i)->u.number - ex) / rise);
    }
    const JsonValue *p0 = json_get(json_at(pr, 0), "point_m"), *p20 = json_get(json_at(pr, 20), "point_m");
    int ends = json_len(p0) == 3 && json_len(p20) == 3 && json_at(p0, 0)->u.number == 0 && json_at(p20, 0)->u.number == L;
    printf("  %zu samples, the worst %.2e of the rise from the exact parabola; ends where asked: %s\n", json_len(T), worst, ends ? "yes" : "no");
    json_free(r);
    verdict(worst < 1e-9 && ends, "K20 every sample within 1e-9 of the rise, the ends where asked");
}

/* K21: thermal stress in time, the library's elasticity driven by a field */
static void test_k21(void) {
    printf("K21 thermal stress in time: K11's bar heated at one end, its sides on rollers\n");
    double rc = 3.6e6, k = 45, T0 = 300, dT = 100, alpha = k / rc, E = 200e9, nu = 0.3, ae = 12e-6, sstep = E * ae * dT / (1 - nu);
    const double xs[3] = {0.005, 0.010, 0.020}, ts[3] = {5, 10, 20};
    char sc[4096];
    snprintf(sc, sizeof sc,
             "{\"domain\":\"kernel\",\"title\":\"K21\",\"mesh\":{\"box\":{\"size_m\":[0.1,0.01,0.01],\"cells\":[200,1,1]}},\"order\":2,"
             "\"physics\":[\"elasticity\"],\"materials\":{\"steel\":{\"youngs_modulus_pa\":%g,\"poisson_ratio\":%g,\"expansion_per_k\":%g,\"source\":\"nominal steel\"}},"
             "\"fields\":{\"T\":{\"order\":1,\"mass\":%g,\"diffusion\":%g,\"initial\":%g,\"unit\":\"K\"}},"
             "\"loads\":{\"temperature_k\":\"T\"},\"reference_temperature_k\":%g,"
             "\"boundaries\":[{\"name\":\"heated end\",\"where\":{\"plane\":[\"x\",0]},\"value\":{\"T\":%g}},"
             "{\"name\":\"cold end\",\"where\":{\"plane\":[\"x\",0.1]},\"fix\":[\"x\"]},"
             "{\"name\":\"side y0\",\"where\":{\"plane\":[\"y\",0]},\"fix\":[\"y\"]},{\"name\":\"side y1\",\"where\":{\"plane\":[\"y\",0.01]},\"fix\":[\"y\"]},"
             "{\"name\":\"side z0\",\"where\":{\"plane\":[\"z\",0]},\"fix\":[\"z\"]},{\"name\":\"side z1\",\"where\":{\"plane\":[\"z\",0.01]},\"fix\":[\"z\"]}],"
             "\"time\":{\"end_s\":20,\"step_s\":0.1,\"method\":\"bdf2\",\"outputs\":4},"
             "\"outputs\":{\"probes\":[{\"name\":\"5 mm\",\"point_m\":[%g,0.005,0.005]},{\"name\":\"10 mm\",\"point_m\":[%g,0.005,0.005]},"
             "{\"name\":\"20 mm\",\"point_m\":[%g,0.005,0.005]}]},\"run\":{\"frames\":false}}",
             E, nu, ae, rc, k, T0, T0, T0 + dT, xs[0], xs[1], xs[2]);
    JsonValue *r = run("k21", sc);
    const JsonValue *series = json_get(r, "series");
    int ok = r != NULL && json_len(series) == 5, checked = 0;
    double wa = 0, wb = 0, wc = 0;
    for (size_t si = 0; si < json_len(series); si++) {
        const JsonValue *o = json_at(series, si);
        double t = num(o, "t_s", NULL, NULL);
        int want = 0;
        for (int j = 0; j < 3; j++) want = want || fabs(t - ts[j]) < 1e-9;
        if (!want) continue;
        const JsonValue *pr = json_get(o, "probes");
        for (int i = 0; i < 3; i++) {
            const JsonValue *p = json_at(pr, (size_t)i), *st = json_get(p, "stress_pa");
            double T = num(p, "T", NULL, NULL), sxx = json_len(st) == 6 ? json_at(st, 0)->u.number : NAN,
                   syy = json_len(st) == 6 ? json_at(st, 1)->u.number : NAN, szz = json_len(st) == 6 ? json_at(st, 2)->u.number : NAN;
            double own = -E * ae * (T - T0) / (1 - nu), Te = T0 + dT * erfc(xs[i] / (2 * sqrt(alpha * t))), ex = -E * ae * (Te - T0) / (1 - nu);
            double ea = fmax(fabs(syy - own), fabs(szz - own)) / sstep, eb = fabs(syy - ex) / sstep, ec = fabs(sxx) / sstep;
            if (!(ea == ea && eb == eb && ec == ec)) ea = eb = ec = INFINITY;
            wa = fmax(wa, ea), wb = fmax(wb, eb), wc = fmax(wc, ec);
            printf("  t = %2.0f s, x = %2.0f mm: T %.4f K; s_yy %.4f MPa, from the field's T %.4f MPa, erfc's %.4f MPa (%+.3f %% of the step); s_xx %.2e Pa\n", t,
                   1e3 * xs[i], T, syy / 1e6, own / 1e6, ex / 1e6, 100 * (syy - ex) / sstep, sxx);
            checked++;
        }
    }
    printf("  the step's stress %.2f MPa; worst (a) %.2e, (b) %.3f %%, (c) %.2e of it, %d points checked\n", sstep / 1e6, wa, 100 * wb, wc, checked);
    json_free(r);
    verdict(ok && checked == 9 && wa < 1e-6 && wb < 0.005 && wc < 1e-6,
            "K21 the transverse stress from the field's temperature to 1e-6, from erfc's to 0.5 %, no axial stress");
}

/* K22: principal stresses of a uniform strain with shear */
static void jacobi3(double a[3][3], double ev[3]) { /* eigenvalues of a symmetric 3 x 3 matrix, by Jacobi's rotations */
    for (int sweep = 0; sweep < 60; sweep++) {
        double off = fabs(a[0][1]) + fabs(a[0][2]) + fabs(a[1][2]);
        if (off < 1e-300) break;
        for (int p = 0; p < 2; p++)
            for (int q = p + 1; q < 3; q++) {
                if (a[p][q] == 0) continue;
                double th = 0.5 * atan2(2 * a[p][q], a[q][q] - a[p][p]), c = cos(th), sn = sin(th);
                for (int k = 0; k < 3; k++) { /* columns p and q */
                    double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - sn * akq, a[k][q] = sn * akp + c * akq;
                }
                for (int k = 0; k < 3; k++) { /* rows p and q */
                    double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - sn * aqk, a[q][k] = sn * apk + c * aqk;
                }
            }
    }
    for (int k = 0; k < 3; k++) ev[k] = a[k][k];
}

static void test_k22(void) {
    printf("K22 principal stresses of a uniform strain with shear\n");
    double E = 200e9, nu = 0.3, lam = E * nu / ((1 + nu) * (1 - 2 * nu)), mu = E / (2 * (1 + nu));
    double e[3][3] = {{1e-4, 5e-5, 2e-5}, {5e-5, -2e-5, -4e-5}, {2e-5, -4e-5, 3e-5}}, sg[3][3], ev[3];
    double tr = e[0][0] + e[1][1] + e[2][2];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) sg[i][j] = 2 * mu * e[i][j] + (i == j ? lam * tr : 0);
    double big = 0;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) big = fmax(big, fabs(sg[i][j]));
    jacobi3(sg, ev);
    double p1 = fmax(ev[0], fmax(ev[1], ev[2])), p3 = fmin(ev[0], fmin(ev[1], ev[2])), p2 = ev[0] + ev[1] + ev[2] - p1 - p3;
    char sc[4096];
    snprintf(sc, sizeof sc,
             "{\"domain\":\"kernel\",\"title\":\"K22\",\"mesh\":{\"box\":{\"size_m\":[0.01,0.01,0.01],\"cells\":[2,2,2]}},\"order\":2,"
             "\"physics\":[\"elasticity\"],\"materials\":{\"steel\":{\"youngs_modulus_pa\":%g,\"poisson_ratio\":%g,\"source\":\"nominal steel\"}},"
             "\"boundaries\":[{\"name\":\"all\",\"where\":\"all\",\"displacement_m\":[\"%.17g*x + %.17g*y + %.17g*z\",\"%.17g*x + %.17g*y + %.17g*z\",\"%.17g*x + %.17g*y + %.17g*z\"]}],"
             "\"outputs\":{\"probes\":[{\"name\":\"inside\",\"point_m\":[0.003,0.006,0.004]}]},\"run\":{\"frames\":false}}",
             E, nu, e[0][0], e[0][1], e[0][2], e[1][0], e[1][1], e[1][2], e[2][0], e[2][1], e[2][2]);
    JsonValue *r = run("k22", sc);
    double mx = num(r, "results", "max_principal_pa", NULL), mn = num(r, "results", "min_principal_pa", NULL);
    const JsonValue *pp = json_get(json_at(json_get(r, "probes"), 0), "principal_pa");
    double q1 = json_len(pp) == 3 ? json_at(pp, 0)->u.number : NAN, q2 = json_len(pp) == 3 ? json_at(pp, 1)->u.number : NAN,
           q3 = json_len(pp) == 3 ? json_at(pp, 2)->u.number : NAN;
    double worst = fmax(fmax(fabs(mx - p1), fabs(mn - p3)), fmax(fabs(q1 - p1), fmax(fabs(q2 - p2), fabs(q3 - p3)))) / big;
    if (worst != worst) worst = INFINITY;
    printf("  Jacobi: %.6f, %.6f, %.6f MPa; the body's largest %.6f and smallest %.6f MPa; the probe's %.6f, %.6f, %.6f MPa\n", p1 / 1e6, p2 / 1e6, p3 / 1e6,
           mx / 1e6, mn / 1e6, q1 / 1e6, q2 / 1e6, q3 / 1e6);
    printf("  worst difference %.2e of the largest stress component (%.3f MPa)\n", worst, big / 1e6);
    json_free(r);
    verdict(worst < 1e-9, "K22 the principal stresses within 1e-9 of Jacobi's");
}

int main(int argc, char **argv) {
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {{"K1", test_k1},   {"K2", test_k2},   {"K3", test_k3},   {"K4", test_k4},   {"K5", test_k5},
                 {"K6", test_k6},   {"K7", test_k7},   {"K7b", test_k7b}, {"K8", test_k8},   {"K9", test_k9},
                 {"K10", test_k10}, {"K11", test_k11}, {"K12", test_k12}, {"K13", test_k13}, {"K14", test_k14}, {"K15", test_k15}, {"K16", test_k16}, {"K17", test_k17}, {"K18", test_k18}, {"K19", test_k19}, {"K20", test_k20}, {"K21", test_k21}, {"K22", test_k22}};
    double t0 = wall();
    snprintf(workdir, sizeof workdir, "/tmp/openphysics-kerneltest-%d", (int)getpid());
    char cmd[600];
    snprintf(cmd, sizeof cmd, "mkdir -p %s", workdir);
    if (system(cmd) != 0) return 2;
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) { /* every case, or the ones named: kerneltest K10 K11 */
        int want = argc < 2;
        for (int a = 1; a < argc; a++) want = want || !strcmp(argv[a], cases[c].name);
        if (want) cases[c].run();
    }
    snprintf(cmd, sizeof cmd, "rm -rf %s", workdir); /* the workspace goes when the run ends, pass or fail */
    if (system(cmd) != 0) printf("  (could not remove %s)\n", workdir);
    printf("kerneltest: %s (%d failures, %.1f s)\n", failures ? "FAIL" : "pass", failures, wall() - t0);
    return failures ? 1 : 0;
}
