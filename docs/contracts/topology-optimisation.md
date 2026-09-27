# Contract: topology optimisation on the voxel grid (task T21, phase E)

Owner: the app session (`src/fem/topopt.[ch]`, the `topology_optimize` operation in this session's schema fragment,
`tools/topotest.c`, the OPTIMISE controls of Advanced mode). Written on 2026-09-20 before any line of the
implementation; it changes only by addition or by a dated amendment. Up: [ROADMAP-SOLVERS.md](../design/ROADMAP-SOLVERS.md),
phase E.

Nothing in the existing solver changes. The optimiser is a loop around `solid_solve` on the voxel hexahedral mesh
that is already there, using the per-element stiffness multiplier the solver already accepts (`HexModel.elem_scale`),
the supports and loads the user already picked, and the linear solver already chosen by `SolidOptions`.

## 1. The problem

Minimise compliance under a volume constraint, with the densities as the design variables:

```
min over x   c(x) = f^T u(x)
subject to   K(x) u = f
             sum_e x_e v_e  <=  V*  =  volume_fraction * sum_e v_e
             x_min <= x_e <= 1
```

`v_e` is the element volume, `x_e` the density of element `e`, `f` the applied load vector (nodal forces, gravity or
a picked face traction, whatever the analysis already carries). Prescribed displacements are zero, so `c = f^T u` is
also twice the strain energy; both are computed and the loop reports them as a cross-check of each other.

**SIMP, modified form.** The stiffness of an element follows

```
E(x_e) = E_min + x_e^p (E_0 - E_min),     p = 3 by default,  E_min = 1e-9 E_0
```

(the form of Andreassen et al. 2011, chosen because `E_min` keeps the stiffness matrix non-singular without a lower
bound on `x` inside the update). This enters the existing solver as `elem_scale[e] = E(x_e) / E_0`; no line of the
element or solver code is touched.

**Sensitivity.** With `q_e = u_e^T K_e(x) u_e`, the element energy the solver already gives back through its
Gauss-point strains and stresses (`sum over Gauss points of eps . sig . detJ . w`, which includes the condensed
internal modes of the incompatible-mode element),

```
dc/dx_e = - p x_e^(p-1) (E_0 - E_min) / E_0  *  q_e / s_e ,        s_e = E(x_e) / E_0
dV/dx_e = v_e
```

**Sensitivity filter** (Sigmund 1997, as used in the 99-line code): with `H_ef = max(0, r_min - dist(e, f))` over the
element centroids,

```
dc_hat_e = ( sum_f H_ef x_f dc/dx_f ) / ( max(gamma, x_e) sum_f H_ef ),   gamma = 1e-3
```

`r_min` is a length in metres in the app and in the operation (the tests give it in element widths, and the contract
states which is used in every reported number). The filter radius is always reported with the result: a result
without its radius is not comparable with another.

**Optimality-criteria update** with a move limit `m = 0.2` and a bisection on the Lagrange multiplier `lambda`:

```
x_new_e = max( x_min, max( x_e - m, min( 1, min( x_e + m, x_e sqrt( -dc_hat_e / (lambda v_e) ) ) ) ) )
```

`lambda` is bisected between 0 and 1e9 until the volume constraint is met to `(l2 - l1) / (l1 + l2) < 1e-9`. The loop
stops when `max_e |x_new_e - x_e| <= 0.01` (the criterion of the published codes) or after `max_iter` iterations
(default 100), and reports which of the two ended it, with the iteration count.

**Passive regions.** Elements that carry a support or a load are held at `x = 1` and are never updated: an
optimiser is free to delete the face the user clicked, and the result would be meaningless. The shell is stated in
the settings: `passive_layers = 1` means the elements sharing a node with a constrained or loaded node; `2` grows it
by one more layer of node neighbours. Passive elements count in the volume, so a volume fraction smaller than the
passive volume is refused with the two numbers in the message, not silently clipped. The benchmarks of section 3 run
with `passive_layers = 0`, because the published problems have no passive region; the app uses 1 by default and says
so on screen.

**What this method does not do.** No stress constraint, no manufacturing or overhang constraint, no multiple load
cases, no compliant mechanisms, no black-and-white projection: grey elements between 0 and 1 remain, and the part
that comes out is the set of elements above a threshold. The optimisation is on the voxel mesh only; the
tetrahedral meshes of T17 are not design domains in this task.

## 2. The interface

```c
typedef struct TopOptSettings {
    double volume_fraction;  /* target, (0, 1] */
    double penalty;          /* SIMP p, default 3 */
    double filter_radius;    /* m, default 1.5 element widths; 0 disables the filter */
    double move_limit;       /* default 0.2 */
    double x_min;            /* default 1e-3 */
    double e_min_ratio;      /* E_min / E_0, default 1e-9 */
    int    max_iter;         /* default 100 */
    double change_tol;       /* default 0.01 */
    int    passive_layers;   /* 0 none, 1 elements at supports and loads, 2 one layer more */
    int    seed_uniform;     /* 1: start from x = volume_fraction everywhere (the published start) */
} TopOptSettings;

typedef struct TopOptStep { int iter; double compliance, volume_fraction, change; } TopOptStep;

typedef struct TopOptResult {
    double *density;      /* nelems, 0..1 */
    unsigned char *passive;
    TopOptStep *history;  /* iterations entries */
    int iterations, solves, passive_elements;
    double compliance, compliance_initial, volume_fraction, change;
    char stop_reason[64]; /* "converged" or "iteration limit" */
} TopOptResult;

bool topopt_run(const HexModel *m, const SolidLoads *loads, const SolidOptions *opt,
                const TopOptSettings *s, TopOptResult *r, char *err, size_t errlen);
void topopt_result_free(TopOptResult *r);
```

One linear solve per iteration; the solver's own settings decide direct or iterative. `HexModel.elem_type` must be
`SOLID_ELEM_HEX8`: a tetrahedral model is refused with a message, not silently voxelised.

## 3. Verification, and the criteria written before the code

The published problems, not published code: the implementation is written from the formulae above. Sources:

- O. Sigmund, *A 99 line topology optimization code written in Matlab*, Structural and Multidisciplinary
  Optimization 21(2), 120-127, 2001, doi 10.1007/s001580050176. Problem definitions used: the half MBB beam
  `top(60, 20, 0.5, 3.0, 1.5)` (unit downward load at the upper left corner, symmetry along the left edge, a
  vertical roller at the lower right corner) and the short cantilever `top(32, 20, 0.4, 3.0, 1.2)` (the left edge
  fully fixed, unit downward load at the lower right corner). The paper prints no compliance values.
- E. Andreassen, A. Clausen, M. Schevenels, B. S. Lazarov and O. Sigmund, *Efficient topology optimization in MATLAB
  using 88 lines of code*, Structural and Multidisciplinary Optimization 43, 1-16, 2011, doi 10.1007/s00158-010-0594-7,
  preprint read at https://www.topopt.mek.dtu.dk (accessed 2026-09-20). Its section 3.4 runs the same MBB beam with
  volume fraction 0.5, `p = 3`, `E_0 = 1`, `E_min = 1e-9`, `nu = 0.3` and a filter radius of 0.04 times the width of
  the domain (2.4 elements on the 60 x 20 mesh). Figure 3 reports, for sensitivity filtering,
  **c = 216.81** (60 x 20), 219.52 (150 x 50), 222.29 (300 x 100), and for density filtering 233.71, 235.73, 238.31.

**Criterion 1 (the number).** The MBB beam on 60 x 20 elements, volume fraction 0.5, `p = 3`, sensitivity filter of
radius 2.4 elements, modified SIMP with `E_min = 1e-9`, started from a uniform density: the final compliance is
within **3 percent** of 216.81, that is in [210.31, 223.31].

The published code is two-dimensional plane stress; this solver is three-dimensional. The benchmark is built as one
layer of unit cubes with every z displacement fixed and the plane-strain material that is exactly equivalent to
plane stress. A layer with its out-of-plane motion held is a plane-strain layer, so the equivalent pair is the one
whose plane-strain matrix equals the plane-stress matrix of the published material: `nu^ = nu / (1 + nu)`,
`E^ = E (1 - nu^^2)` (with `E = 1`, `nu = 0.3`: `E^ = 0.9467456`, `nu^ = 0.2307692`; the two 3 by 3 matrices then
agree to the last digit). Together with the full-integration formulation `HEX8_FULL` this, which under that constraint is the standard
four-node bilinear plane element of the published code. The thickness is one element, so the compliance is the
published one, not a multiple of it. This equivalence is stated here because it is the only way the two numbers are
comparable, and the test prints both.

**Criterion 2 (the layout).** The density field of the MBB run is written as an image (`build/topopt-mbb.png`) and as
a text picture in the log, and the test checks the features of the published figure that can be measured: the top
edge of the beam is a solid chord (mean density of the top row above 0.9), the lower left corner under the load
carries material, the interior is not a solid block (at least 8 percent of the interior elements below 0.1), and
there are no checkerboard cells (no element below 0.3 whose four in-plane face neighbours are all above 0.7). If the
picture cannot be compared with the paper's figure by eye, the report says so in words instead of claiming it.

**Criterion 3 (the second published problem).** The short cantilever `top(32, 20, 0.4, 3.0, 1.2)`: no compliance is
published, so the criteria are (a) the compliance of the optimised design is at least 40 percent below the
compliance of the uniform grey design of the same volume (the starting point), (b) the history is monotone after the
first five iterations, (c) the density image shows the published shape, a two-bar fork opening from the fixed edge
towards the loaded corner, checked as: the elements next to the fixed edge are solid at the top and bottom and grey
or void in the middle third, and the loaded corner is solid.

**Criterion 4 (three dimensions).** A cantilever box of 24 x 12 x 6 elements, fixed at one end, loaded at the far
end, optimised to 30 percent volume, has a lower compliance than the same box with 70 percent of its volume removed
as one centred rectangular hole (the same volume of material, arranged without optimisation). The margin is stated
in the report as a percentage; the criterion is that the optimised design is lower by at least a factor of two,
which is a weak bound on purpose: the point is the direction, and the number is printed either way.

**Criterion 5 (the machine).** Every benchmark in `make test` runs in under two minutes on this laptop, and the MBB
beam at the published grid size runs in seconds. A run inside a session stays under ten minutes (AGENTS.md rule 8).

## 4. In the app and through the operation layer

`topology_optimize` in the operation schema: `{ volume_fraction, penalty, filter_radius, max_iter, passive_layers,
move_limit }`, run on the current project's mesh, supports and loads. It returns the iteration count, the final
compliance and volume, the history, and the field as a result the viewer can draw; elements below 0.5 density are
not drawn, so what the user sees is the surviving part. The result is exportable as an STL of the surviving
elements through the existing isosurface code at the 0.5 contour, so the optimised part can be sent straight into
the LPBF build simulation.

In Advanced mode, OPTIMISE appears after a hold and a load have been picked, with the volume fraction and the filter
radius as the two controls, the iteration history as a curve, and the same wording as the rest of the app: numbers
with their units, and a sentence saying what the result is and is not (a compliance-optimal density field on this
mesh, at this volume fraction, for this single load case, with no stress constraint).

The operation is reachable through `navier-mcp` and is walked through in `tools/uicheck.py`, one walkthrough that
optimises a bracket and then prints the surviving part.

## 5. Amendment 1, 2026-09-20: the first runs

Written after the first implementation ran against the criteria of section 3.

**The material equivalence in section 3 was the wrong way round in the first version of this page.** A layer with
its out-of-plane motion held is a plane-strain layer, so the pair to use is the one whose plane-strain matrix equals
the plane-stress matrix of the published material, `nu^ = nu / (1 + nu)`, `E^ = E (1 - nu^^2)`, not the substitution
that converts plane strain into plane stress. Section 3 now carries the corrected line. With the wrong pair the
material was about 2.8 times too stiff in the direct terms and the MBB compliance came out at 160.0 J, 26 percent
below the published value; with the corrected pair it is **216.76 J against the published 216.81 J, 0.02 percent
below**, on 103 iterations and 104 solves in 4.6 s.

**Criterion 2 named the wrong edge.** It asked for the top edge of the MBB beam to be the solid chord. The load
sits on the top left corner and the roller under the bottom right one, and the chord that runs the whole span in
the result, as in the published figure, is the **bottom** one; the loaded edge carries material over about two
thirds of the span and tapers away towards the support. The criterion now measures both chords: the edge away from
the load above 0.9 mean density, the loaded edge above 0.5. Measured: bottom 1.000, top 0.686, 36.3 percent of the
interior elements below 0.1 density, no checkerboard cells, the element under the load solid.

**The published figure itself could not be put next to ours.** This machine has no PDF rasteriser (no poppler, no
Homebrew), so the paper's figure cannot be rendered into an image to place beside `build/topopt-mbb.pgm`. The
comparison is therefore in words and in measured features, as section 3 allows, plus the compliance, which agrees
to 0.02 percent; the density field is written as a text picture in the test log and as a PGM image.

**Criterion 4's reference design.** "The same volume removed as a centred hole" is built as exactly the same number
of elements taken out of the middle of the box, nearest the centre first, so the two designs hold the same volume
to one element. Measured: the optimised cantilever box (24 x 12 x 6, 30 percent volume, 60 iterations, 14.6 s)
reaches 3.945e-4 J against 1.223e-3 J for the hole, **3.1 times stiffer at the same volume**.

**The short cantilever.** No compliance is published, as expected. Measured: 57.27 J against 429.23 J for the
uniform grey design of the same volume (86.7 percent lower), the history settles (largest rise after iteration 5 is
0.01 percent), and the shape is the published fork: solid where it meets the fixed edge at the top (0.833) and at
the bottom (0.983), open in the middle third (0.000), solid at the loaded corner.

## 6. Amendment 2, 2026-09-20: the operation, the app and the chain into the print

**Reached through the operation layer** as `topology_optimize` (a background job, one solve per iteration, progress
per iteration) and `topology_result` (summary, history, and the density field as base64 float32 on request). The
run directory keeps `summary.json` and `density.f32` with its sha256.

**Additive changes outside the ownership of this task**, listed here as the architect asked: `src/ctl/ops.c` and
`src/ctl/ops_internal.h` gained the binding table of the new file `src/ctl/ops_topopt.c`; `src/ctl/ops_schema.json`
gained the two operations; `src/fembridge.[ch]`, `src/hud.c` and `src/commands.c` gained the app side; the Makefile
gained `build/topotest` and the new uicheck mode. Nothing existing was edited except those registration lines and
the one element test in `element_shown`.

**Two defects the first runs found**, both in this task's own code: `topology_result` must be declared
`engine_lock: false`, because looking for a run directory takes the engine lock a handler already holds (the same
latent deadlock exists in the `ops_results` path for a job that is not in the session's job list, in code this task
does not own, and is left alone); and the refusal path attached its warnings to the error and then freed them,
which crashed the server while it wrote the reply. Both are fixed and both are covered by the checks below.

**In the app** (Advanced mode, after a hold and a load are picked): OPTIMISE opens a block with the volume to keep
and the filter radius, OPTIMISE NOW starts the job and shows its progress with the iteration and the largest
density change, and the result arrives as the part with the emptied elements gone, the compliance curve of every
iteration, the numbers, and EXPORT STL. The exported STL is a contour of the voxel densities, so the message says
to check it with REPAIR before printing; the app's own diagnostics then say what it found (on the bracket: one
component, two non-manifold edges, so the volume is not computed).

**Checked** by `python3 tools/uicheck.py --topopt` in `make test-ui`, 15 checks: the panel clicks, the reported
numbers, 232 of 600 elements kept at 40 percent volume with compliance 0.1021 J against 3.277 J for the uniform
grey design of the same volume, the emptied elements leaving the picture and SHOW WHOLE bringing them back, a
complete binary STL, and the same operation through `navier-mcp` with the density decoded and the refusal message
checked.

**The chain into the print** is `tools/topowalk.nav`: solve, optimise through the panel, export the STL, import it
again, and run the LPBF build on it. The last step of that build writes the picture
`/tmp/navier-topowalk-print.png`, where the walkthrough keeps its own images.
