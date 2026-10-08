# Contract: the inherent-strain LPBF build (`lpbf_build_run`) and its results

Owner: the AM-process session (`src/mech/lpbf.[ch]`, `src/ctl/lpbf_build.[ch]`). Written before the implementation;
it changes only by addition or dated amendment. The **validation** criteria are not here: they were fixed in a
pre-registered protocol (A0-A4) that this session could not change. What follows are the solver's own acceptance
criteria (V1-V7), the mechanics, the refusals and the file layout.

**Amendment, 2026-09-27: the validation data removed.** The LPBF cantilever measurements this build was compared with,
the commercial simulator's calibrated strains, the protocol, the independent CalculiX model of the cantilever and every
result file computed on them were removed from the repository before publication, by the owner's decision. The named
strain sets (`strain_set`) were removed from the operation with them: the inherent strain is now always three typed
numbers with a provenance and a source, and a request that names a set is refused. Numbers below that came from the
removed data were taken out; our own results of that time stay as the record, and can no longer be rerun from here.

Related: [`validation/RESULT_FORMAT.md`](../../validation/RESULT_FORMAT.md) (the JSON every run writes),
[`Mech Sim/docs/lpbf_outlook.md`](../../Mech%20Sim/docs/lpbf_outlook.md) (why this method and not a melt pool),
[`print-results.md`](print-results.md) (the FFF print, which shares the result file).

## 1. The method, and the one point everything rests on

An LPBF part distorts because **every layer solidifies stress-free on an already deformed, already stressed body and
only then contracts**. A whole-part eigenstrain cannot produce that: a uniform eigenstrain in a homogeneous body
produces no bending, which is exactly what the CalculiX cross-check measured (-0.19 mm, a small deflection of the wrong sign).
So the build is stepped:

1. The part sits on the plate: every node at z = 0 is fixed in x, y and z.
2. Simulation layers of thickness `t_sim` are activated bottom to top by element centroid height. Powder carries
   overhangs, so a layer is activated whole; nothing waits for support (this is the difference from the FFF print).
3. **A layer is activated stress-free on the deformed part below it.** Its lower nodes are the nodes of the layer
   below and keep the displacement they already have; its new nodes are placed at their nominal position; it inherits
   no strain and no stress. In the incremental formulation used here this is exactly the statement that an element's
   stress integral starts at the increment in which it is activated: the element's reference configuration is the
   deformed configuration at its birth. `eps0` and `elem_scale` of the shared hex8 solver express it without any
   change to `src/fem/solid.c` — see §2, verified by V3.
4. The activated layer then receives the orthotropic inherent strain as an eigenstrain
   (`sigma = C : (eps - eps_inherent)`, the same statement CalculiX realises with `*EXPANSION, TYPE=ORTHO`), and the
   whole active part re-equilibrates in one linear solve.
5. After the last layer the **cut** removes the elements the wire crosses; the internal forces they carried are
   released onto the remaining structure, which re-equilibrates. The teeth stubs below the cut stay on the plate.
6. Reported: the tip deflection before the cut, after the cut, and the springback, as
   [`RESULT_FORMAT.md`](../../validation/RESULT_FORMAT.md) defines them (max `u_z` over the free-end face, +z up).

`mode: "whole_part"` activates and strains everything at once; it exists only as the cross-check against CalculiX
(V7) and is never mixed with a layer-by-layer number.

## 2. Why no change to the shared solver is needed

Each step solves for an **increment** `du`:

    K(active) du = f(eps_inherent of the elements strained in this step) + f(released internal forces)

with the accumulated displacement `u += du` and the accumulated stress `sigma_e += D (B du - eps0_e)`. An element
activated in step k contributes to `K` from step k on and accumulates stress from step k on; the displacement its
nodes already carry is in `u`, not in its stress. That is "born stress-free on a deformed base", and it needs only
what `SolidLoads.eps0` and `HexModel.elem_scale` already give. **No parameter was added to `src/fem/solid.h/.c`.**
V3 is the test that decides whether this is true: activating a layer must change nothing (V3a), and a stack whose
layers are each laid at their programmed height and then strained must end at one layer's contraction (V3b). An
implementation that inherits strain on activation fails both.

## 3. Inputs (all with units; provenance where a value is a choice)

| Input | Meaning |
|---|---|
| `body` | the meshed body that is built (one body, one material) |
| `build_orientation` | `X` or `Y`: the machine axis the part's long axis lay along. `Y` swaps `exx` and `eyy`, because the tensor is given in the machine frame and the mesh is always built with the part's long axis along model x |
| `layer_thickness_sim` | the simulation layer, which lumps many printed layers (30 um here) |
| `inherent_strain` | `{exx, eyy, ezz}` with `provenance` and `source`. Until 2026-09-27 a named `strain_set` was also accepted; it is now refused with `INVALID_PARAMS` |
| `cut` | `{height, kerf, provenance}`; every element whose z-extent intersects the kerf band, away from the solid block, is removed. Omit for no cut |
| `material` | `{youngs_modulus, poissons_ratio, provenance}`. For a body loaded only by an eigenstrain the displacement does not depend on the modulus at all; it is used for the stresses and reported |
| `mode` | `layer_by_layer` (default) or `whole_part` |
| `numerics` | solver, tolerance, as in the FFF print |

## 4. Refusals (before any solve)

| Condition | Error |
|---|---|
| no inherent strain, or a strain without `provenance` | `PRECONDITION_FAILED` |
| the cut without `provenance` | `PRECONDITION_FAILED` |
| elastic constants without `provenance` | `PRECONDITION_FAILED` |
| the mesh is stale | `PRECONDITION_FAILED` |
| no element has a whole face on the build plate (z = 0) | `PRECONDITION_FAILED` |
| more than one body or material | `UNSUPPORTED` |
| the cut removes no element | `PRECONDITION_FAILED`, naming the kerf and the element size |

## 5. Results

`results.nvt` with one stored time per layer and one after the cut. **There is no temperature field**: the file is
written without `T`, loads without it, and the summary says `"temperature": "not modelled"`. Per stored time: `mech_u`
and `mech_vm`. Per element: `elem_birth` (as for the FFF print) and, new,

```c
int *elem_death;           /* nelems: first stored time at which the element no longer exists; -1 never */
unsigned char *elem_group; /* nelems: 0 part, 1 support, 2 plate; NULL = all part */
```

written behind their own header flags at format version 4, which still reads versions 1-3. A reader hides element `e`
at stored time `i` when `i < elem_birth[e]` or (`elem_death[e] >= 0` and `i >= elem_death[e]`).

## 6. Acceptance of the solver (V1-V7), fixed before the first run

| ID | Criterion | Tolerance |
|---|---|---|
| **V1** | one element, free, uniform eigenstrain: stress zero and displacement `eps . x` | stress <= 1e-9 of `E |eps|`; displacement exact to 1e-12 relative |
| **V2** | one element, every node fixed: `sigma = -C : eps` | 1e-12 relative |
| **V3a** | **strain-free activation**: activating a layer on the deformed part changes no displacement and no stress | exactly zero (the activated element enters the next solve with no load of its own) |
| **V3b** | a stack of N layers, each activated then strained by `eps`, ends with its top surface displaced by **one layer's contraction** `eps h`, whatever N, because every layer is laid at its programmed height | 1e-9 relative |
| **V4** | bilayer strip, eigenstrain in the top layer only: `kappa = 3 d_eps / (2 h)` (Timoshenko bimetal, the form the CalculiX reference re-derived) | <= 1 % |
| **V5** | layer-by-layer cantilever beam, N layers each strained in x on activation: `kappa = sum_{k=1}^{N-1} 6 eps k / ((k+1)^3 t)`, tip `kappa L^2 / 2`, taken as the largest `u_z` over the free-end face (the protocol's quantity). A contracting layer curls the free end **up**, the sign the measurements show | <= 3 % |
| **V6** | V5 with the element size halved **at the same simulation-layer thickness** (halving the layer thickness changes the physics, not the discretisation: the summed layer moments grow with the number of layers) | change <= 5 % (protocol A3) |
| **V7** | `whole_part` mode on the cantilever reproduces the CalculiX table (`ccx/README.md`) for the four strain sets at 1 mm | <= 5 % on `tip_uz_after_cut_mm` |

**Amendment, 2026-09-18, before the first cantilever run.** V3 was first written as "the stack ends at `N eps h` with
zero stress". Both halves were wrong, and the run that showed it is kept: (i) the top of the stack ends at `eps h`, not
`N eps h`, because a new layer's own nodes are placed at their programmed height - that is the recoater, and it is the
sharper test of the semantics; (ii) the stack is **not** stress-free: each layer is born on a base that has already
contracted laterally, so the lateral mismatch leaves stress (9.1e7 Pa in the first run). That stress is the mechanism
the method exists to capture, so the criterion now tests activation (V3a, exactly zero) and the programmed height (V3b)
separately. Measured: V3a 0.0 m and 0.0 Pa, V3b 7.0e-15 relative.

V7 is the cross-check that the geometry, the units, the cut and the element technology are the same as the independent
model's; it says nothing about whether the method is right, which is what the protocol's A0 tests.

**Amendment, 2026-09-18, after the first cantilever runs: where `cut.from_x` is measured from.** It was first
implemented as a coordinate in the build frame. The mesher centres a part in x and y, so for the 70 mm cantilever the
build frame runs from -35 mm to +35 mm and the documented value 10 mm (the solid block, measured along the part from
its fixed end, as the geometry file and the CalculiX model both use it) removed only the teeth beyond the middle of the
part. The part stayed attached to the plate over 35 mm of teeth and the springback came out two to four times too
small. `cut.from_x` is now measured **from the part's own minimum x**, which is frame independent and is the rule the
independent model used (`xc > block`). The numbers below, and every result file of that time, are from after this
correction; the ones quoted in the wave-2 interim notes are not.

| | before the correction | after |
|---|---|---|
| V7, `whole_part`, after the cut, against the CalculiX table | 15-18 % | 0.3-1.5 % |
| Calibrated-P17, orientation Y, layer by layer, after the cut | 1.062 mm | 3.600 mm |

## 7. Which frame the inherent strain is given in, and why (2026-09-18)

`build_orientation` swaps `exx` and `eyy` because the tensor is in the **machine frame** and the part is what rotates on
the plate. That is not an assumption; it is what the source of the calibrated strains stated (one orthotropic tensor
per strategy, fitted to both build orientations at once, in the machine's axes), and the alternative is ruled out:

- The controlled test: with the tensor kept in the scan frame instead, so that the axial strain stays `exx` whatever the
  orientation, the Y build comes out **bit identical to the X build** (Calibrated-P17 1.3326 mm, Calibrated-P05
  0.8993 mm, 1 mm elements). That convention predicts no orientation effect at all, while the measured deflections
  differed markedly between the orientations.

So the machine-frame convention is the one to keep, and the over-prediction in Y is not caused by the frame.

## 8. Fitting our own inherent strain (`lpbf_calibrate`, 2026-09-18)

A borrowed tensor belongs to the tool that produced it: to its element size, its layer thickness and its material law.
`lpbf_calibrate` fits ours, on the same layer-by-layer build every other run uses.

**What it fits.** `exx` and `eyy`, to the measured tip deflection after the cut in **both** build orientations at once,
at one fixed discretisation. `ezz` is held: two numbers cannot separate three components, and the calibration that
produced the reference tensors held it at the same value. The fit matches deflection only, so **it is not evidence
about stress**, and it absorbs every error of the model it was fitted with.

**How.** The model is linear in the inherent strain, so the residual is affine and a bounded secant (one
forward-difference Jacobian, then Broyden updates) reaches it in a single step. Each trial costs two builds, one per
orientation; the budget is an input and is reported with the whole iteration history.

**Criteria, fixed before the first fit:**

| ID | Criterion | Measured |
|---|---|---|
| **C1** | the fit reproduces what it was fitted to, within the requested tolerance | P17 and Rotation and Ot to 0.000 %, P05 to 0.227 % in Y (tolerance 1 %) |
| **C2** | rebuilding with the fitted tensor gives the fitted deflection again (the tensor, not the fitter, carries the answer) | flow test: 0.02000 mm against the 0.02 mm it was fitted to |
| **C3** | the fit converges inside its budget and reports the budget it used | 8 builds of 12 for every strategy |
| **C4** | a case the tensor was fitted on is never reported as a prediction of that case | the result file carries `fitted_on_this_case`, and the comparison script of the time printed such a row as a fit residual with no verdict |
| **C5** | the fitter is not the model: the operation refuses to fit with the whole-part model, refuses a measurement without a source and a provenance, and refuses a budget under four builds | all three refused, covered by `tools/lpbfflow.py` |

**What a fitted tensor may be used for.** Predicting a strategy it was not fitted on, at the discretisation it was
fitted at. The predictions of P14, P11 and P08 interpolate the tensors fitted on P17 and P05 linearly in hatch spacing;
that linearity is an assumption about the process, and it is written into every predicted result file.

## 9. Plasticity (J2 with isotropic linear hardening), criteria fixed before the code (2026-09-18)

**Why.** The borrowed strains are large in plane: held elastic in aluminium they give stresses more than twice the
yield of AlSi10Mg as built (near 230 MPa). An elastic solver carries that stress; the real part does not. The Y
over-prediction grows monotonically with `|eyy|` and grows again when the layers are refined, which is the signature of
a missing yield cap rather than of a discretisation error. Plasticity is therefore added, and the elastic result is
kept alongside it rather than replaced.

**Model.** Small-strain J2 (von Mises) with isotropic **linear** hardening: yield function `q - (sigma_y + H alpha)`,
associative flow, **radial return** at the eight Gauss points of the shared hex8, the equivalent plastic strain `alpha`
carried in the element state between layers and through the cut. The stress at the start of a step is the state; the
plastic strain itself is recoverable from it. Eigenstrain enters exactly as before, as a stress-free strain that the
trial stress subtracts.

**Solution of a step.** A **modified Newton** loop: the elastic stiffness is kept as the operator (it is the same
matrix the elastic build already assembles and factorises, so a plastic step costs one extra solve per iteration and
nothing extra to build), and the residual is `f_released - integral B^T (sigma - sigma_at_step_start)` with `sigma`
from the radial return of the eigenstrain-adjusted trial stress. The eigenstrain load is already in that stress
increment, not added a second time. **Convergence:** the residual norm over the free equations falls below `1e-8` of
the first residual of that step, or below `1e-9 N` absolute; the iteration count and the final residual are reported
per step, and a step that does not converge fails the job rather than returning a number.

**The elastic path stays bit-identical.** With no yield stress the step takes the same single linear solve it takes
today, on the same code path. The proof is that `mechtest` (247) and `tools/lpbfflow.py` (82) pass unchanged.

**Criteria, fixed before the first plastic run:**

| ID | Criterion | Tolerance |
|---|---|---|
| **P1** | uniaxial bar stretched past yield, elastic-perfectly-plastic (`H = 0`) and linear hardening (`H > 0`): the stress follows `sigma = E eps` then `sigma = sigma_y + H_eff (eps - eps_y)` with `H_eff = E H / (E + H)` | 1e-9 relative |
| **P2** | the same bar unloaded after yielding: the stress returns elastically and the residual strain is `eps_max - sigma_max / E` | 1e-9 relative |
| **P3** | rectangular beam in bending, elastic-perfectly-plastic: the fully plastic moment is `1.5` times the first-yield moment | <= 2 % on a fine mesh |
| **P4** | a bar held between fixed ends and given an eigenstrain past yield: the stress is capped at exactly `sigma_y` (and at `sigma_y + H alpha` with hardening) | 1e-9 relative |
| **P5** | the elastic verification set V1 to V8 is unchanged, number for number, with no yield stress given | exactly as recorded in section 6 |

**Scope.** Rate independent, isothermal, small strain, no kinematic hardening and therefore no Bauschinger effect, no
damage, no creep. A yield stress is a **material property with a provenance**, never a fitting parameter: the fit of
section 8 keeps fitting the inherent strain only.

**Amendment, 2026-09-18, on accelerating the Newton loop.** The modified Newton converges linearly here: on the
cantilever it halves the residual per iteration and needs about thirty iterations a layer, which is thirty-five times
the cost of the elastic build. Aitken relaxation was tried, since the rate is so clean that two successive corrections
predict the limit. It cut the iteration count, and it **converged to the wrong state**: the fully plastic moment of P3
came out at 1.5885 N m instead of 2.0673, a ratio of 1.15 instead of 1.50. The reason is in the criterion itself. P3
sits at the limit load of a perfectly plastic section, where the solution is not unique and the path the iteration
takes decides which state it lands in; an extrapolation that is valid for a contraction is not valid there. The
acceleration was removed and the plain modified Newton kept. This is why P3 is in the set: it is the one criterion
that could tell the difference.

**What plasticity costs, measured.** Calibrated-P17 in Y at 1 mm elements and 1 mm layers: elastic 2.9 s, plastic
102.5 s, 297 Newton iterations over ten steps, peak equivalent plastic strain 0.0254. The production sets with
plasticity are therefore run at 1 mm, against the elastic runs at the same discretisation, so that the only difference
between the two tables is the material law.

**Amendment, 2026-09-18, on the convergence criterion of a plastic step.** Section 9 fixed it at `1e-8` relative
before any plastic run existed. Measured on Calibrated-P17 in Y at 1 mm, that costs 297 Newton iterations and 99.6 s,
and the criterion is met only just: Calibrated-P05 in Y, which yields most, reaches 9.9e-8 after sixty iterations and
fails. What the tolerance is worth was then measured rather than argued:

| tolerance | tip after the cut | against 1e-8 | iterations | seconds |
|---|---|---|---|---|
| 1e-8 | 3.4818 mm | - | 297 | 99.6 |
| **1e-6** | 3.4815 mm | **-0.009 %** | 185 | 61.4 |
| 1e-5 | 3.4800 mm | -0.05 % | 135 | 44.5 |

The default is therefore **1e-6**, three orders of magnitude below the tightest tolerance in the protocol (A3, 5 %) and
below the difference between any two discretisations we can afford. 1e-8 remains available and is what the
verification cases P1 to P4 use, where the answer is a closed form and there is no reason to accept less.

## 10. Independent review by the architect (2026-09-19)

Read line by line: `src/mech/lpbf.c` (activation, the increment step, the plastic step, the release on removal) and
`src/ctl/lpbf_build.c` (layer assignment, the cut, support removal, the 3-2-1 hold). Findings, each checked against the
code rather than the tests:

1. **Strain-free activation is exact for the physics it claims.** An element is assembled on the nominal mesh, and its
   stress is accumulated only from increments after its birth. Its bottom nodes carry the displacement of the deformed
   layer below and its top nodes carry none, so the stress-free reference shape is "flat top at the programmed height
   on a deformed base", which is what a recoater produces. Small-strain, so the sub-millimetre change of node positions
   is neglected consistently.
2. **The release on removal has the right sign.** Removing an element whose internal force was f_e leaves the rest
   out of equilibrium by exactly +f_e; `lpbf_remove` adds f_e to the next step's load. V8 (a restrained bar with its
   middle element removed relaxes to zero stress) is the right test of this and it passes.
3. **The plastic step is a correct modified Newton on the elastic operator.** Iteration zero equals the elastic step
   (the eigenstrain enters through the trial stress, so the first residual is force + f(eps0)); the stress is returned
   radially from the converged state at the start of the step with the trial computed from the whole increment, which
   is the standard implicit integration of J2 with linear isotropic hardening; the equivalent plastic strain is carried
   at the Gauss points across layers and through the cut.
4. **Homogenised supports are scaled consistently:** stiffness, eigenstrain force, stress, yield stress and hardening
   all carry the same fraction, in both the elastic path (through `elem_scale` of the shared solver) and the plastic
   path (explicitly in `element_stress`).
5. **One assumption the contract did not state, now stated:** support elements receive the same inherent strain as
   the part in the layer they belong to. That is a modelling choice (a printed lattice does shrink), not a fact from
   any source, and its effect on the on-plate maximum has not been separated from the stiffness fraction's.
6. **Not reviewed:** `lpbf_calibrate.c` beyond its use of `lpbf_build_once`; the adaptive mesh and the support rule
   now being written on branch am-process.

Conclusion: the headline number (the six held-out predictions within 3.2 percent) rests on a formulation that does what
its contract says. The remaining uncertainty is in the inputs (cut height, kerf, the layer size the strains belong to),
not in the mechanics.

## 11. The support rule (wave 5, 2026-09-19)

**What the Simufact Additive tutorial says**, read in the owner's copy of `Simufact_Additive_2025.1_Einfuehrung.pdf`
(outside the repository; nothing copied from it; printed page numbers):

- p. 55: the surfaces that get supports are "calculated based on the Critical surface angle". **No value is given**.
- p. 56: supports are either shell supports (2-D surfaces with a thickness that "directly influences the supports
  stiffness") or volume supports (closed volumes). Default thicknesses are not given.
- p. 60: supports carry a relative material density against the part; a lower density lowers their stiffness; supports
  printed like the part should be set to 100 %. **No default homogenised stiffness is given.**
- p. 72: the solver scales each voxel's stiffness by its volume fraction.
- p. 155: voxels are coarsened in x and y by a coarsening factor; the z size of a coarsened element follows the layers.

**What is built.** A downward voxel face gets support only where a face of the STL flatter than the critical angle,
measured from the horizontal, lies over it (within one and a half voxels in height); and only when the voxel column
below it reaches the plate through empty space. A column that would stand on the part gets none: that is what the
inside of a closed channel is, a cavity whose floor is part, so the rule places no support in channels without a
separate cavity search. The critical angle is **45 degrees, assumed** (the common convention; the tutorial names the
parameter only), and the lattice keeps its **0.3 stiffness fraction, assumed** (the tutorial gives none). Supports
are removed after the cut as element death, as before. The wave-4 rule stays available as `rule: every_column`, so its
results can be reproduced.

**Verified** (`tools/lpbfflow.py`): a box on the plate gets none; the bridge gets exactly the 96 cells under its span; a
block with a 4 x 4 mm channel through it gets none inside (the wave-4 rule put 256 there); a part leaning 60 degrees
from the vertical, whose overhanging face is 30 degrees from the horizontal, gets 800; one leaning 30 degrees, whose
face is 60 degrees from the horizontal, gets none.

**On the cylinder head**, uniform voxels: supports are 13.2 per cent of the elements at 2 mm (48 002 of 364 725) and 14.0
per cent at 4 mm (6 675 of 47 569), against 39.6 per cent with the wave-4 rule (26 834 of 67 728 at 4 mm).

**The sensitivity `rule: overhang_all`** keeps the angle filter but supports every such face down to the plate or to
the part below it, channels included. It exists only to separate the two halves of the rule; it is not the rule. On
the channel block it puts 256 elements, like the wave-4 rule. An angle that is not given is reported as assumed (the
request schema no longer fills in 45 degrees, which had made it read as given).

**What the rule does to the cylinder head** (4 mm, elastic, Rot as the report states it, supports at 0.3, measured on
2026-09-19; the run records were removed with the part on 2026-09-27, see the amendment at the end of this section):

| rule | support elements | on the plate, max | separated, max | valve seats, mean of 16 | wall | peak memory |
|---|---|---|---|---|---|---|
| every_column (wave 4) | 26 834 | 0.732 mm | 1.318 mm | 0.207 mm | 505 to 813 s | not recorded |
| overhang_all (sensitivity) | 19 863 | 0.743 mm | 1.161 mm | 0.190 mm | 356 s | 568 MB |
| **overhang (the rule)** | **6 675** | **1.757 mm** | 1.622 mm | 0.150 mm | 566 s | 544 MB |

The angle filter alone changes almost nothing (0.732 to 0.743 mm). Leaving channels unsupported is what moves the
maximum, and it moves it through one feature: a thin horizontal shelf on the outer wall (model y = 58 mm, z = 96 to
100 mm), over open air down to a flange 40 mm below. Its column lands on the part, so the rule, as the owner phrased it
("none inside closed internal channels, cavity not open to the plate"), treats it like a channel ceiling and leaves it
unsupported, and it curls up by 1.76 mm. Only 66 of 55 158 part nodes move more than 1 mm. The rest of the part moves
less than before: the valve seats drop to 0.150 mm. The new maximum is a different place on the part, reached by a
classification the rule cannot make. A 3-D
flood fill does not separate the cases either: at 4 mm, 3 299 of the downward faces standing on the part lie in
cavities open to the outside through the ports and only 12 in enclosed ones, so "closed" cannot be read off the
voxels. Telling a port ceiling from an external shelf needs either Simufact's own support set-up or a rule the owner
states (for example: part-to-part supports allowed outside the port volumes).

**Amendment, 2026-09-27: the cylinder head removed.** The part and its run records were removed from the repository
for copyright, by the owner's decision. The numbers on the head in this section stay as the record of the runs of
2026-09-19; they can no longer be rerun from this repository.

## 13. Numerical amendments, 2026-10-03

These criteria are fixed before the new checks run. They verify the inherent-strain mechanics; they do not add
a resolved laser, melt-pool flow or thermal cycle to this reduced-order process model.

- V8: for a fully restrained unit hex receiving the same eigenstrain in two increments, the final plate nodal reaction
  is twice the one-increment reaction to 1e-10 relative and equals the closed-form face traction. A following zero-strain
  equilibration preserves it to the same tolerance. Reactions must represent accumulated stress, rather than only the
  last elastic increment; plastic and elastic runs share that definition.
- V9: on a bent two-layer specimen the reported element von Mises equals the mean of the eight Gauss-point von Mises
  values within 1e-12 relative. Uniform stress is unchanged. This scalar is an element mean, not the maximum of the
  integration-point stress and not a guarantee of mesh convergence.
- V10: a freely contracting unit hex on 3-2-1 isostatic constraints, with isotropic eigenstrain -0.001, reports the
  last elastic solve's equilibrium error below 1e-9. That diagnostic retains the solid solver's eigenstrain-load
  normalisation; dividing by the nearly zero final stress would incorrectly amplify harmless roundoff.
- V11, fixed before its first run: the same freely contracting hex, with J2 enabled at E = 100 GPa, nu = 0.3 and
  yield stress = 250 MPa, has affine displacement `u = -0.001 (x - x_anchor)` within 1e-10 relative, stress magnitude
  below 1e-10 of `E * 0.001`, zero plastic strain and a last nonlinear equilibrium error below 1e-9. The existing
  P1/P2 perfect-plastic and hardening bar load/unload checks additionally require that error below 1e-6 after every
  accepted increment, without changing their stress, plastic-strain or Newton convergence criteria. A fresh unloaded
  J2 hex must report zero residual.

The elastic diagnostic is the shared solid solver's recovered free-equation residual norm divided by the sum of
the applied nodal load norm, individual prescribed-DOF reaction norm and assembled free RHS norm (which includes
eigenstrain). The J2 diagnostic uses the accepted nonlinear free residual of the last step, divided by the sum of
the reduced applied/release nodal load norm, individual prescribed-DOF **incremental** reaction norm and first
nonlinear free residual norm (the returned-stress predictor with the step's eigenstrain). Both loads and increment
internal forces transfer hanging-node shares to their active masters before the force scale is evaluated. It is
not the residual of the last inner linear correction, nor a ratio to the almost-zero final total stress. Both
diagnostics are dimensionless when their force scale is nonzero; with zero scale they retain the absolute residual
in N. Zero load and zero residual therefore report zero. This reporting amendment does not change any Newton
threshold, accepted stress, reaction or displacement.

Before the reporting fix, V11 failed at 0.854 nonlinear equilibrium error despite a 1.95e-15 affine displacement
error, 2.63e-15 stress/(E eps) and exactly zero plastic strain: 43 printing checks passed, one failed. The criterion
and Newton thresholds were retained. After the fix, 44 printing checks passed: the free J2 residual was 6.06e-16,
with those physical errors unchanged; the largest load/unload residuals were 4.49e-12 for perfect plasticity and
3.31e-12 with hardening. The printed P1/P2/P4, P3, cut and layer-built cantilever physical results were identical
before and after this diagnostic-only change. The full MCP LPBF workflow passed 239/239 checks, including export
of the different elastic and nonlinear diagnostic definitions and their zero-scale units.

## 12. A cut that frees a part standing on supports (wave 5, 2026-09-19)

When the cut runs through the whole section of a part that stands on supports, the part above it is free the moment
the kerf is gone. The kerf, the stub below it (an island once the kerf is removed) and the supports are then removed
in one step, and the part is held 3-2-1 on its lowest plane like a part whose supports were removed; cutting first
would leave it floating for one solve. The result says so (`cut.with_supports`). First used by a specimen study
(cut 4 mm above the plate, 1 mm into a part built 3 mm above it) that was removed on 2026-09-27.
