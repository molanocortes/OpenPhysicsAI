# Engineering Evidence Record: bracket_ab_4levels

Status: **complete**. Generated 2026-09-16T21:30:25Z. Study hash `6e3532240acab5e24f6202763ab38331542ba2013db27a8164fc834b44f46b1e`.

> a record of what a linear-elastic model predicts under stated conditions, with its numerical checks; not a certificate, not a measurement, and not a statement that a part is safe. Every number below is read from `evidence.json` in this folder; the Traceability section says where.

## Question

Here are two versions of my bracket. They attach through the back face and carry approximately 3 kg on the pad at the end of the arm. Compare their stiffness and mass, explain the assumptions, and tell me what information could change the comparison.

Decision to support: choose between version A (plain L, 8 mm plates) and version B (the same with a 24 mm inner chamfer)

## Outcome

**Lower predicted load-region displacement under the modeled conditions: design B (0.0086276 mm) against design A (0.025498 mm), a 66.2% difference; numerical uncertainty ±0.00017 mm and ±0.0001 mm at 0.5 mm elements.**

Outcome code: `resolved` (compared at the finest mesh every design completed validly: 0.5 mm).

| Design | Load-region displacement (mm) | Numerical uncertainty of the primary quantity | Stiffness (N/mm) | Mass, geometry (kg) | Stiffness / mass (N/mm/kg) | Peak displacement (mm) |
|---|---|---|---|---|---|---|
| A | 0.025498 | ±0.0001 (0.41%) | 1153.8 | 0.114 | 10117 | 0.03016 |
| B | 0.0086276 | ±0.00017 (1.9%) | 3410 | 0.1452 | 23493 | 0.0109 |

Primary quantity: `load_region_displacement` (lower is better). The peak displacement is supporting information: the question concerns the load region.

Robustness of the ranking:

- youngs_modulus: ranking unchanged. every design uses the same material and the loads are forces: all displacements scale with 1/E, so the ranking and the ratio between designs do not depend on E; absolute values scale with it

## Modeled conditions

- **Material:** `al6061_t6_nominal` (Aluminium 6061-T6, nominal handbook values); status user_supplied, source database. E = 68.9 GPa, nu = 0.33, density 2700 kg/m3. Reference: nominal handbook values for 6061-T6 aluminium
- **Manufacturing:** machined (source user)
- **Mounting:** fixed, on each design's mounting region (source user). back face clamped to a rigid wall (the stiffest idealisation of a bolted joint)
- **Load:** 3 kg payload x g = 9.80665 m/s2 (source default) = 29.42 N along (0, 0, -1) (design frame), spread uniformly over the load region; self-weight not included; source user. 3 kg item resting on the pad; z points up in both files
- **Analysis:** static, small-strain linear elasticity, isotropic material; 8-node hexahedra on voxel meshes (staircase boundary), incompatible_modes formulation; solver auto; properties at 20 degC.
- **Frames and units:** design frame: the STL file's axes and origin, lengths in mm; each design's build frame differs by the translation recorded with it.

| Design | File | SHA-256 | Units | Size (mm) | Volume (mm3) | Thinnest wall (mm) | Mounting area (mm2) | Load area (mm2) | Lever arm (mm) |
|---|---|---|---|---|---|---|---|---|---|
| A | /private/tmp/claude-501/-Users-sebas-Downloads-Fluid-Sim/22500e7f-fd6d-4763-a5f1-db09bf6ffe3d/scratchpad/eval/navier-0.3.0-rc1-macos-arm64/share/navier/examples/bracket_comparison/geometry/bracket_a_plain.stl | `7cf8844569be` | mm | (80, 40, 60) | 42240 | 8 | 2400 | 256 | 78 |
| B | /private/tmp/claude-501/-Users-sebas-Downloads-Fluid-Sim/22500e7f-fd6d-4763-a5f1-db09bf6ffe3d/scratchpad/eval/navier-0.3.0-rc1-macos-arm64/share/navier/examples/bracket_comparison/geometry/bracket_b_chamfer.stl | `806efa160fae` | mm | (80, 40, 60) | 53760 | 8 | 2400 | 256 | 78 |

Equivalence of the conditions between designs: **equivalent**. every design against the first: the lever arm from the mounting-region centroid to the load-region centroid (design frame), the areas of both regions and their mean normals. Equal face ids are not evidence of equivalence; these physical quantities are

- lever_arm, design B: difference 0 mm (tolerance 1.56) ok
- load_region_area, design B: difference 0 mm2 (tolerance 5.12) ok
- mounting_region_area, design B: difference 0 mm2 (tolerance 48) ok
- mounting_normal_angle, design B: difference 0 deg (tolerance 5) ok
- load_normal_angle, design B: difference 0 deg (tolerance 5) ok

## Results by design and mesh

### Design A

| h (mm) | Elements | Volume error (%) | Load-region displacement (mm) | Stiffness (N/mm) | Mass, mesh (kg) | Equilibrium | Moment balance | 2U/W - 1 | Line-of-action shift (mm) | Deformation | Valid | Solver | Wall (s) | Factor (MB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 4 | 660 | 6.58e-13 | 0.0247427 | 1189.03 | 0.11405 | 9.8e-13 | 1e-12 | 1.3e-11 | 7.5e-15 | within_small_deformation_assumption | yes | sparse Cholesky (nested dissection) | 0.0413 | 4.91 |
| 2 | 5280 | -4.97e-12 | 0.0251846 | 1168.17 | 0.11405 | 4.1e-12 | 4.9e-12 | 6e-11 | 1.8e-13 | within_small_deformation_assumption | yes | sparse Cholesky (nested dissection) | 0.89 | 76.8 |
| 1 | 42240 | 2.48e-11 | 0.0253994 | 1158.29 | 0.11405 | 1.5e-11 | 1.9e-11 | 1.5e-10 | 2.4e-13 | within_small_deformation_assumption | yes | pcg/block-jacobi | 3.32 | n/a |
| 0.5 | 337920 | 8.22e-11 | 0.0254981 | 1153.81 | 0.11405 | 5.2e-11 | 5.7e-11 | 6.5e-10 | 1.1e-12 | within_small_deformation_assumption | yes | pcg/block-jacobi | 44.1 | n/a |

Refinement of the load-region displacement: monotone convergence with observed order 1.12: asymptotic range plausible. Method: Richardson extrapolation and grid convergence index over the three finest valid meshes (safety factor 1.25).
- Stress at 4 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 3.381 MPa, nodal-average maximum 4.223 MPa, nodal 99th percentile 3.833 MPa; peak at a sharp inside corner (singular)
- Stress at 2 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 4.552 MPa, nodal-average maximum 4.967 MPa, nodal 99th percentile 3.823 MPa; peak at a sharp inside corner (singular)
- Stress at 1 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 5.984 MPa, nodal-average maximum 6.071 MPa, nodal 99th percentile 3.749 MPa; peak at a sharp inside corner (singular)
- Stress at 0.5 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 7.867 MPa, nodal-average maximum 7.66 MPa, nodal 99th percentile 3.57 MPa; peak at a sharp inside corner (singular)

### Design B

| h (mm) | Elements | Volume error (%) | Load-region displacement (mm) | Stiffness (N/mm) | Mass, mesh (kg) | Equilibrium | Moment balance | 2U/W - 1 | Line-of-action shift (mm) | Deformation | Valid | Solver | Wall (s) | Factor (MB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 4 | 810 | -3.57 | 0.0097683 | 3011.78 | 0.13997 | 7.5e-13 | 5.2e-13 | 5.1e-12 | 7.5e-15 | within_small_deformation_assumption | yes | sparse Cholesky (nested dissection) | 0.0672 | 7.27 |
| 2 | 6600 | -1.79 | 0.00908461 | 3238.44 | 0.14256 | 2.8e-12 | 1.9e-12 | 1.9e-11 | 1.8e-13 | within_small_deformation_assumption | yes | sparse Cholesky (nested dissection) | 2.19 | 132 |
| 1 | 53280 | -0.893 | 0.00877455 | 3352.87 | 0.14386 | 2.1e-11 | 5.4e-12 | 4.4e-11 | 2.4e-13 | within_small_deformation_assumption | yes | pcg/block-jacobi | 4.25 | n/a |
| 0.5 | 428160 | -0.446 | 0.00862756 | 3410 | 0.1445 | 5.1e-11 | 1.8e-11 | 1.8e-10 | 1.1e-12 | within_small_deformation_assumption | yes | pcg/block-jacobi | 54.1 | n/a |

Refinement of the load-region displacement: monotone convergence with observed order 1.08: asymptotic range plausible. Method: Richardson extrapolation and grid convergence index over the three finest valid meshes (safety factor 1.25).
- Stress at 4 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 2.29 MPa, nodal-average maximum 2.824 MPa, nodal 99th percentile 2.558 MPa; peak at a sharp inside corner (singular)
- Stress at 2 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 2.955 MPa, nodal-average maximum 3.192 MPa, nodal 99th percentile 2.414 MPa; peak at a sharp inside corner (singular)
- Stress at 1 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 3.744 MPa, nodal-average maximum 3.778 MPa, nodal 99th percentile 2.246 MPa; peak at a sharp inside corner (singular)
- Stress at 0.5 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 4.71 MPa, nodal-average maximum 4.593 MPa, nodal 99th percentile 2.086 MPa; peak at a sharp inside corner (singular)

## Numerical evidence

| Check | Applies to | Criterion | Observed | Outcome |
|---|---|---|---|---|
| force equilibrium | design A, 4 completed levels (4 valid) | \|sum of forces\| / force scale <= 1e-6 | largest 5.17e-11 | pass |
| moment balance | design A, 4 completed levels (4 valid) | \|sum of moments\| / moment scale <= 1e-6 | largest 5.68e-11 | pass |
| energy (Clapeyron) | design A, 4 completed levels (4 valid) | \|2 U / W - 1\| <= 1e-6 | largest \|2U/W - 1\| 6.46e-10 | pass |
| linear solver | design A, 4 completed levels (4 valid) | converged, true residual recomputed | converged at every level | pass |
| rigid-body modes | design A, 4 completed levels (4 valid) | setup_validate finds none before each solve | none (every solve passed validation) | pass |
| load line of action on the mesh | design A, 4 completed levels (4 valid) | shift <= max(0.5 h, 1% of the lever arm) and force preserved to 1e-6 | largest shift / criterion 1.45e-12 | pass |
| work-conjugate displacement = region mean | design A, 4 completed levels (4 valid) | relative difference <= 1e-9 (a uniform traction does work with the area mean) | largest 5.33e-14 | pass |
| mounting region on the mesh | design A, 4 completed levels (4 valid) | faces present, mesh/geometry area ratio in (0.5, 2) | represented at every level | pass |
| mesh topology | design A, 4 completed levels (4 valid) | one face-connected region | one region at every level | pass |
| small deformation | design A, 4 completed levels (4 valid) | displacement <= 1% of size and rotation error <= 1e-3 (questionable up to 5% / 1e-2) | within_small_deformation_assumption | pass |
| geometry approximation (volume) | design A, 4 completed levels (4 valid) | reported; > 3% changes stiffness and mass noticeably | 8.22e-11% at the finest completed level | pass |
| discretisation (load-region displacement) | design A, 4 completed levels (4 valid) | last relative change <= 0.02 | monotone convergence with observed order 1.12: asymptotic range plausible; relative uncertainty 0.00411 | pass |
| force equilibrium | design B, 4 completed levels (4 valid) | \|sum of forces\| / force scale <= 1e-6 | largest 5.07e-11 | pass |
| moment balance | design B, 4 completed levels (4 valid) | \|sum of moments\| / moment scale <= 1e-6 | largest 1.83e-11 | pass |
| energy (Clapeyron) | design B, 4 completed levels (4 valid) | \|2 U / W - 1\| <= 1e-6 | largest \|2U/W - 1\| 1.83e-10 | pass |
| linear solver | design B, 4 completed levels (4 valid) | converged, true residual recomputed | converged at every level | pass |
| rigid-body modes | design B, 4 completed levels (4 valid) | setup_validate finds none before each solve | none (every solve passed validation) | pass |
| load line of action on the mesh | design B, 4 completed levels (4 valid) | shift <= max(0.5 h, 1% of the lever arm) and force preserved to 1e-6 | largest shift / criterion 1.45e-12 | pass |
| work-conjugate displacement = region mean | design B, 4 completed levels (4 valid) | relative difference <= 1e-9 (a uniform traction does work with the area mean) | largest 5.41e-14 | pass |
| mounting region on the mesh | design B, 4 completed levels (4 valid) | faces present, mesh/geometry area ratio in (0.5, 2) | represented at every level | pass |
| mesh topology | design B, 4 completed levels (4 valid) | one face-connected region | one region at every level | pass |
| small deformation | design B, 4 completed levels (4 valid) | displacement <= 1% of size and rotation error <= 1e-3 (questionable up to 5% / 1e-2) | within_small_deformation_assumption | pass |
| geometry approximation (volume) | design B, 4 completed levels (4 valid) | reported; > 3% changes stiffness and mass noticeably | -0.446% at the finest completed level | pass |
| discretisation (load-region displacement) | design B, 4 completed levels (4 valid) | last relative change <= 0.02 | monotone convergence with observed order 1.08: asymptotic range plausible; relative uncertainty 0.0192 | pass |

A small solver residual is not a discretisation-error estimate: the discretisation rows come from the refinement study.

## Interpretation

- Lower predicted load-region displacement under the modeled conditions: design B (0.0086276 mm) against design A (0.025498 mm), a 66.2% difference; numerical uncertainty ±0.00017 mm and ±0.0001 mm at 0.5 mm elements.
- These are predictions of a static, small-strain, linear-elastic model for: material 'al6061_t6_nominal' (user_supplied values, source database); mounting region rigidly fixed; a static payload weight spread uniformly over the load region. They are conditional on those inputs and are not measurements.
- Stress values are supporting information only. No strength margin or safety statement is made (see not evaluated).

## Important uncertainties

- Discretisation, design A: monotone convergence with observed order 1.12: asymptotic range plausible (Richardson extrapolation and grid convergence index over the three finest valid meshes (safety factor 1.25)).
- Geometry approximation, design A: the finest voxel mesh has 0.00% volume error against the STL.
- Discretisation, design B: monotone convergence with observed order 1.08: asymptotic range plausible (Richardson extrapolation and grid convergence index over the three finest valid meshes (safety factor 1.25)).
- Geometry approximation, design B: the finest voxel mesh has -0.45% volume error against the STL.
- Load distribution and location: a uniform traction over the load region is assumed; a payload bearing on part of it or with an offset centre of mass is not assessed.
- Manufacturing (machined): its effect on stiffness is represented only through the material values given.

## Assumptions

| Subject | Source | Assumption | Effect |
|---|---|---|---|
| mounting_idealization | user | The mounting region is idealised as rigidly fixed (all displacements zero): back face clamped to a rigid wall (the stiffest idealisation of a bolted joint). No contact, slip, fastener or wall flexibility is modelled. | not assessed: no mounting alternative was declared |
| gravitational_acceleration | default | The payload weighs mass x standard gravity 9.80665 m/s^2, applied statically: no dynamic amplification from shocks, vibration or handling. | a different g scales every displacement equally and does not change the ranking |
| load_distribution | default | The load acts as a uniform traction over the load region, so its resultant passes through the region's centroid. A payload that bears on part of the region, or whose centre of mass is offset, loads the bracket differently. | not assessed |
| self_weight | default | The bracket's own weight is not applied: only the payload loads it. | not assessed; the weight of each design is reported |

## Not evaluated

| Item | Reason |
|---|---|
| dynamic loads, impact and vibration | the load is static |
| strength margin or safety | no failure criterion and no validated strength data for the modelled condition; stress peaks at ideal supports and sharp corners are singular |
| fatigue | cyclic loading is not modelled |
| fastener loads, pull-out and joint slip | the mounting is an ideal support; fasteners and contact are not modelled |
| buckling and geometric nonlinearity | linear static analysis; large-deformation indicators are checked after solving |
| creep and temperature effects | properties at the reference temperature, time-independent |

## Reproduction

- Resolved study: `study.json` (SHA-256 of its content `6e3532240acab5e24f6202763ab38331542ba2013db27a8164fc834b44f46b1e`); original request `request.json`; every executed operation in `operations.jsonl`.
- Geometry of design A: stored copy `designs/A/inputs/part-7cf8844569be.stl`, SHA-256 `7cf8844569be4b9bd80fab7c8f2033eadd160e29b012760e2bb35ddab8ebef19` (original `/private/tmp/claude-501/-Users-sebas-Downloads-Fluid-Sim/22500e7f-fd6d-4763-a5f1-db09bf6ffe3d/scratchpad/eval/navier-0.3.0-rc1-macos-arm64/share/navier/examples/bracket_comparison/geometry/bracket_a_plain.stl`).
- Geometry of design B: stored copy `designs/B/inputs/part-806efa160fae.stl`, SHA-256 `806efa160faedf8b5b3fb6bef1e4e9a8e764bc0caa4445cca76f9069deb0c528` (original `/private/tmp/claude-501/-Users-sebas-Downloads-Fluid-Sim/22500e7f-fd6d-4763-a5f1-db09bf6ffe3d/scratchpad/eval/navier-0.3.0-rc1-macos-arm64/share/navier/examples/bracket_comparison/geometry/bracket_b_chamfer.stl`).
- Software: NAVIER 0.3.0-rc1, operation contract 0.5.0, compiler Apple LLVM 17.0.0 (clang-1700.0.13.5); IEEE 754 double precision; no -ffast-math; no floating-point contraction.
- Platform: Darwin 24.6.0, arm64, 8 CPUs.
- MCP: study_replay {"directory": "/private/tmp/claude-501/-Users-sebas-Downloads-Fluid-Sim/22500e7f-fd6d-4763-a5f1-db09bf6ffe3d/scratchpad/eval/audit/four_levels"} reruns the study from study.json and the stored geometry copies and compares every quantity
- CLI: navier-ctl --embedded study replay "/private/tmp/claude-501/-Users-sebas-Downloads-Fluid-Sim/22500e7f-fd6d-4763-a5f1-db09bf6ffe3d/scratchpad/eval/audit/four_levels"
- Inspect a design: project_open {"path": "/private/tmp/claude-501/-Users-sebas-Downloads-Fluid-Sim/22500e7f-fd6d-4763-a5f1-db09bf6ffe3d/scratchpad/eval/audit/four_levels/designs/<design>"}, then results_query / results_quantities with a run's job_id
- replays on the same build and platform are expected to agree bitwise; the replay record compares quantities with a relative tolerance of 1e-9. Other compilers, CPUs or thread counts may differ in the last digits.

## Traceability

| Reported item | Location in evidence.json |
|---|---|
| Outcome statement and code | `/comparison/statement`, `/comparison/outcome` |
| Outcome table (design i) | `/comparison/values/i/*` |
| Ranking robustness | `/comparison/ranking_robustness` |
| Modeled conditions | `/modeled_conditions/*` |
| Results by mesh (design i, level k) | `/designs/i/levels/k/{mesh,quantities,checks,stress,cost,run}` |
| Refinement readings | `/designs/i/refinement/*` |
| Sensitivity runs | `/designs/i/sensitivity/*` |
| Numerical evidence | `/numerical_evidence/*` |
| Assumptions, questions, exclusions, failures | `/assumptions`, `/unresolved_inputs`, `/accepted_questions`, `/not_evaluated`, `/failures` |
| Each analysis | `designs/<design>/runs/<job_id>/spec.json`, `summary.json`, `results.nvr` (hash in `/designs/i/levels/k/run/results_sha256`) |
