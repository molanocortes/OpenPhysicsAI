# Engineering Evidence Record: plate_task_reference

Status: **complete**. Generated 2026-09-16T22:45:29Z. Study hash `3145152d97d81b34557b9705200eb22bb8954338b277d7b05ed8fb2f96cdffe4`.

> a record of what a linear-elastic model predicts under stated conditions, with its numerical checks; not a certificate, not a measurement, and not a statement that a part is safe. Every number below is read from `evidence.json` in this folder; the Traceability section says where.

## Question

Two versions of a mounting plate carry a 2 kg instrument hanging under the free end. Which plate deflects less where the instrument hangs, how do their masses compare, and how reliable is the comparison?

Decision to support: choose between the uniform 6 mm plate (U) and the stepped 8 mm / 4 mm plate (S) of equal mass

## Outcome

**Lower predicted load-region displacement under the modeled conditions: design S (0.1267 mm) against design U (0.13311 mm) at 0.5 mm elements, a 4.8% difference. The difference is larger than the sum of the two values' estimated discretisation errors (0.002 mm and 0.0017 mm; grid convergence index, which is an estimate, not a bound). Every design met the 2% convergence criterion (change between its two finest meshes, relative to the finer).**

Outcome code: `resolved` (compared at the finest mesh every design completed validly: 0.5 mm).

| Design | Load-region displacement (mm) | Last change of the primary quantity (relative to the finer mesh) | Convergence criterion | Discretisation-error estimate of the primary quantity | Stiffness (N/mm) | Mass, geometry (kg) | Stiffness / mass (N/mm/kg) | Peak displacement (mm) |
|---|---|---|---|---|---|---|---|---|
| U | 0.13311 | +0.43% | met | 0.0017 (1.3%; safety factor 3) | 147.35 | 0.05832 | 2526.5 | 0.1551 |
| S | 0.1267 | +0.56% | met | 0.002 (1.6%; safety factor 3) | 154.8 | 0.05832 | 2654.4 | 0.1565 |

Primary quantity: `load_region_displacement` (lower is better). The peak displacement is supporting information: the question concerns the load region. A discretisation-error estimate is a grid convergence index: an estimate, not a bound or a confidence interval; it is offered only when its conditions hold (see the refinement of each design).

Ranking on the tested meshes (the same on all 3):

| h (mm) | Ranking, best first | Difference between the best two (relative to the second) |
|---|---|---|
| 2 | S, U | 5.21% |
| 1 | S, U | 4.94% |
| 0.5 | S, U | 4.82% |

Robustness of the ranking:

- youngs_modulus: ranking unchanged. every design uses the same material and the loads are forces: all displacements scale with 1/E, so the ranking and the ratio between designs do not depend on E; absolute values scale with it
- youngs_modulus (Young's modulus x 1.1): ranking unchanged, largest change 9.09%
- self_weight (own weight added): ranking unchanged, largest change 1.1%

## Modeled conditions

- **Material:** `al6082_t6_task` (Aluminium 6082-T6, supplier datasheet values from the task sheet); status user_supplied, source user. E = 70 GPa, nu = 0.33, density 2700 kg/m3. Reference: supplier datasheet values quoted in the task sheet
- **Manufacturing:** machined (source user)
- **Mounting:** fixed, on each design's mounting region (source user). underside patch bolted to a rigid base (stiffest idealisation)
- **Load:** 2 kg payload x g = 9.80665 m/s2 (source default) = 19.6133 N along (0, 0, -1) (design frame), spread uniformly over the load region; self-weight not included; source user. 2 kg instrument hanging under the free end; z up
- **Analysis:** static, small-strain linear elasticity, isotropic material; 8-node hexahedra on voxel meshes (staircase boundary), incompatible_modes formulation; solver auto; properties at 20 degC.
- **Frames and units:** design frame: the STL file's axes and origin, lengths in mm; each design's build frame differs by the translation recorded with it.

| Design | File | SHA-256 | Units | Size (mm) | Volume (mm3) | Thinnest wall (mm) | Mounting area (mm2) | Load area (mm2) | Lever arm (mm) |
|---|---|---|---|---|---|---|---|---|---|
| U | <source tree>/release/evaluation/user-test-kit/task_geometry/plate_uniform.stl | `cca1c768edf2` | mm | (120, 30, 6) | 21600 | 6 | 600 | 600 | 100 |
| S | <source tree>/release/evaluation/user-test-kit/task_geometry/plate_stepped.stl | `b95f89b51108` | mm | (120, 30, 8) | 21600 | 4 | 600 | 600 | 100 |

Equivalence of the conditions between designs: **equivalent**. every design against the first: the lever arm from the mounting-region centroid to the load-region centroid (design frame), the areas of both regions and their mean normals. Equal face ids are not evidence of equivalence; these physical quantities are

- lever_arm, design S: difference 0 mm (tolerance 2) ok
- load_region_area, design S: difference 0 mm2 (tolerance 12) ok
- mounting_region_area, design S: difference 0 mm2 (tolerance 12) ok
- mounting_normal_angle, design S: difference 0 deg (tolerance 5) ok
- load_normal_angle, design S: difference 0 deg (tolerance 5) ok

## Results by design and mesh

### Design U

| h (mm) | Elements | Volume error (%) | Load-region displacement (mm) | Stiffness (N/mm) | Mass, mesh (kg) | Equilibrium | Moment balance | 2U/W - 1 | Line-of-action shift (mm) | Deformation | Valid | Solver | Wall (s) | Factor (MB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2 | 2700 | 6.87e-12 | 0.131399 | 149.265 | 0.05832 | 6.6e-12 | 3.3e-11 | 2.4e-10 | 5.6e-13 | within_small_deformation_assumption | yes | sparse Cholesky (nested dissection) | 0.313 | 33.5 |
| 1 | 21600 | -2.03e-11 | 0.132542 | 147.979 | 0.05832 | 2.6e-11 | 1.4e-10 | 1.1e-09 | 1.8e-12 | within_small_deformation_assumption | yes | sparse Cholesky (nested dissection) | 10 | 513 |
| 0.5 | 172800 | -2.24e-10 | 0.13311 | 147.347 | 0.05832 | 1.3e-10 | 4.1e-10 | 3.1e-09 | 3.1e-12 | within_small_deformation_assumption | yes | pcg/block-jacobi | 36 | n/a |

Refinement of the load-region displacement: last change +0.43% (relative to the finer mesh) meets the 2% criterion; discretisation-error estimate 0.0017 (1.3%; observed order 1.01, safety factor 3: asymptotic behaviour not demonstrated by three meshes).
- Changes between meshes (relative to the finer mesh): +0.86%, +0.43%
- Observed order over (2, 1, 0.5) mm: 1.01
- Estimate condition "at least three valid meshes": met (3 valid meshes)
- Estimate condition "constant refinement ratio of at least 1.3 over the three finest meshes": met (ratios 2 and 2)
- Estimate condition "monotone change over the three finest meshes": met (changes +0.001143 and +0.0005683)
- Estimate condition "the geometry is represented identically on the three finest meshes (volume error within 1e-6)": met (volume errors 6.87e-12%, -2.03e-11%, -2.24e-10%)
- Estimate condition "observed order of the three finest meshes within [0.5, 4]": met (p = 1.01)
- Stress at 2 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 10.07 MPa, nodal-average maximum 11.48 MPa, nodal 99th percentile 8.838 MPa; peak at a support (singular: grows with refinement)
- Stress at 1 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 14.78 MPa, nodal-average maximum 15.05 MPa, nodal 99th percentile 8.745 MPa; peak at a support (singular: grows with refinement)
- Stress at 0.5 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 21.67 MPa, nodal-average maximum 20.82 MPa, nodal 99th percentile 8.531 MPa; peak at a support (singular: grows with refinement)

| Sensitivity | Parameter | h (mm) | Load-region displacement (mm) | Stiffness (N/mm) | Valid |
|---|---|---|---|---|---|
| youngs_modulus | Young's modulus x 1.1 | 0.5 | 0.121009 | 162.081 | yes |
| self_weight | own weight added | 0.5 | 0.134574 | 145.744 | yes |

### Design S

| h (mm) | Elements | Volume error (%) | Load-region displacement (mm) | Stiffness (N/mm) | Mass, mesh (kg) | Equilibrium | Moment balance | 2U/W - 1 | Line-of-action shift (mm) | Deformation | Valid | Solver | Wall (s) | Factor (MB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2 | 2700 | 8.78e-12 | 0.124547 | 157.477 | 0.05832 | 5.7e-12 | 1e-11 | 6.8e-11 | 5.6e-13 | within_small_deformation_assumption | yes | sparse Cholesky (nested dissection) | 0.292 | 32.1 |
| 1 | 21600 | -1.84e-11 | 0.12599 | 155.673 | 0.05832 | 2.3e-11 | 7.4e-11 | 5.1e-10 | 1.8e-12 | within_small_deformation_assumption | yes | sparse Cholesky (nested dissection) | 14.1 | 575 |
| 0.5 | 172800 | -2.22e-10 | 0.126698 | 154.803 | 0.05832 | 1.1e-10 | 2.4e-10 | 1.7e-09 | 3.1e-12 | within_small_deformation_assumption | yes | pcg/block-jacobi | 29.6 | n/a |

Refinement of the load-region displacement: last change +0.56% (relative to the finer mesh) meets the 2% criterion; discretisation-error estimate 0.002 (1.6%; observed order 1.03, safety factor 3: asymptotic behaviour not demonstrated by three meshes).
- Changes between meshes (relative to the finer mesh): +1.15%, +0.56%
- Observed order over (2, 1, 0.5) mm: 1.03
- Estimate condition "at least three valid meshes": met (3 valid meshes)
- Estimate condition "constant refinement ratio of at least 1.3 over the three finest meshes": met (ratios 2 and 2)
- Estimate condition "monotone change over the three finest meshes": met (changes +0.001444 and +0.0007078)
- Estimate condition "the geometry is represented identically on the three finest meshes (volume error within 1e-6)": met (volume errors 8.78e-12%, -1.84e-11%, -2.22e-10%)
- Estimate condition "observed order of the three finest meshes within [0.5, 4]": met (p = 1.03)
- Stress at 2 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 9.285 MPa, nodal-average maximum 11.84 MPa, nodal 99th percentile 10.72 MPa; peak at a sharp inside corner (singular)
- Stress at 1 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 12.33 MPa, nodal-average maximum 13.64 MPa, nodal 99th percentile 10.47 MPa; peak at a sharp inside corner (singular)
- Stress at 0.5 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 16.1 MPa, nodal-average maximum 16.47 MPa, nodal 99th percentile 9.895 MPa; peak at a sharp inside corner (singular)

| Sensitivity | Parameter | h (mm) | Load-region displacement (mm) | Stiffness (N/mm) | Valid |
|---|---|---|---|---|---|
| youngs_modulus | Young's modulus x 1.1 | 0.5 | 0.11518 | 170.284 | yes |
| self_weight | own weight added | 0.5 | 0.127508 | 153.821 | yes |

## Numerical evidence

| Check | Applies to | Criterion | Observed | Outcome |
|---|---|---|---|---|
| force equilibrium | design U, 3 completed levels (3 valid) | \|sum of forces\| / force scale <= 1e-6 | largest 1.3e-10 | pass |
| moment balance | design U, 3 completed levels (3 valid) | \|sum of moments\| / moment scale <= 1e-6 | largest 4.14e-10 | pass |
| energy (Clapeyron) | design U, 3 completed levels (3 valid) | \|2 U / W - 1\| <= 1e-6 | largest \|2U/W - 1\| 3.09e-09 | pass |
| linear solver | design U, 3 completed levels (3 valid) | converged, true residual recomputed | converged at every level | pass |
| rigid-body modes | design U, 3 completed levels (3 valid) | setup_validate finds none before each solve | none (every solve passed validation) | pass |
| load line of action on the mesh | design U, 3 completed levels (3 valid) | shift <= max(0.5 h, 1% of the lever arm) and force preserved to 1e-6 | largest shift / criterion 3.06e-12 | pass |
| work-conjugate displacement = region mean | design U, 3 completed levels (3 valid) | relative difference <= 1e-9 (a uniform traction does work with the area mean) | largest 1.17e-13 | pass |
| mounting region on the mesh | design U, 3 completed levels (3 valid) | faces present, mesh/geometry area ratio in (0.5, 2) | represented at every level | pass |
| mesh topology | design U, 3 completed levels (3 valid) | one face-connected region | one region at every level | pass |
| small deformation | design U, 3 completed levels (3 valid) | displacement <= 1% of size and rotation error <= 1e-3 (questionable up to 5% / 1e-2) | within_small_deformation_assumption | pass |
| geometry approximation (volume) | design U, 3 completed levels (3 valid) | reported; > 3% changes stiffness and mass noticeably | -2.24e-10% at the finest completed level | pass |
| discretisation (load-region displacement) | design U, 3 completed levels (3 valid) | convergence criterion: change between the two finest valid meshes, relative to the finer, within 2% | last change +0.43% (relative to the finer mesh) meets the 2% criterion; discretisation-error estimate 0.0017 (1.3%; observed order 1.01, safety factor 3: asymptotic behaviour not demonstrated by three meshes) | pass |
| Young's modulus scaling | design U, 3 completed levels (3 valid) | linear elasticity: displacement proportional to 1/E, to 1e-6 | \|u(E x 1.1) x 1.1 / u(E) - 1\| = 1.68e-09 | pass |
| force equilibrium | design S, 3 completed levels (3 valid) | \|sum of forces\| / force scale <= 1e-6 | largest 1.07e-10 | pass |
| moment balance | design S, 3 completed levels (3 valid) | \|sum of moments\| / moment scale <= 1e-6 | largest 2.43e-10 | pass |
| energy (Clapeyron) | design S, 3 completed levels (3 valid) | \|2 U / W - 1\| <= 1e-6 | largest \|2U/W - 1\| 1.67e-09 | pass |
| linear solver | design S, 3 completed levels (3 valid) | converged, true residual recomputed | converged at every level | pass |
| rigid-body modes | design S, 3 completed levels (3 valid) | setup_validate finds none before each solve | none (every solve passed validation) | pass |
| load line of action on the mesh | design S, 3 completed levels (3 valid) | shift <= max(0.5 h, 1% of the lever arm) and force preserved to 1e-6 | largest shift / criterion 3.06e-12 | pass |
| work-conjugate displacement = region mean | design S, 3 completed levels (3 valid) | relative difference <= 1e-9 (a uniform traction does work with the area mean) | largest 1.27e-13 | pass |
| mounting region on the mesh | design S, 3 completed levels (3 valid) | faces present, mesh/geometry area ratio in (0.5, 2) | represented at every level | pass |
| mesh topology | design S, 3 completed levels (3 valid) | one face-connected region | one region at every level | pass |
| small deformation | design S, 3 completed levels (3 valid) | displacement <= 1% of size and rotation error <= 1e-3 (questionable up to 5% / 1e-2) | within_small_deformation_assumption | pass |
| geometry approximation (volume) | design S, 3 completed levels (3 valid) | reported; > 3% changes stiffness and mass noticeably | -2.22e-10% at the finest completed level | pass |
| discretisation (load-region displacement) | design S, 3 completed levels (3 valid) | convergence criterion: change between the two finest valid meshes, relative to the finer, within 2% | last change +0.56% (relative to the finer mesh) meets the 2% criterion; discretisation-error estimate 0.002 (1.6%; observed order 1.03, safety factor 3: asymptotic behaviour not demonstrated by three meshes) | pass |
| Young's modulus scaling | design S, 3 completed levels (3 valid) | linear elasticity: displacement proportional to 1/E, to 1e-6 | \|u(E x 1.1) x 1.1 / u(E) - 1\| = 9.64e-10 | pass |

A small solver residual is not a discretisation-error estimate: the discretisation rows come from the refinement study.

## Interpretation

- Lower predicted load-region displacement under the modeled conditions: design S (0.1267 mm) against design U (0.13311 mm) at 0.5 mm elements, a 4.8% difference. The difference is larger than the sum of the two values' estimated discretisation errors (0.002 mm and 0.0017 mm; grid convergence index, which is an estimate, not a bound). Every design met the 2% convergence criterion (change between its two finest meshes, relative to the finer).
- These are predictions of a static, small-strain, linear-elastic model for: material 'al6082_t6_task' (user_supplied values, source user); mounting region rigidly fixed; a static payload weight spread uniformly over the load region. They are conditional on those inputs and are not measurements.
- Stress values are supporting information only. No strength margin or safety statement is made (see not evaluated).

## Important uncertainties

- Self-weight: adding each bracket's own weight changes the load-region displacement by up to 1.10%; the ranking is unchanged.
- Discretisation, design U: last change +0.43% (relative to the finer mesh) meets the 2% criterion; discretisation-error estimate 0.0017 (1.3%; observed order 1.01, safety factor 3: asymptotic behaviour not demonstrated by three meshes).
- Geometry approximation, design U: the finest voxel mesh has -0.00% volume error against the STL.
- Discretisation, design S: last change +0.56% (relative to the finer mesh) meets the 2% criterion; discretisation-error estimate 0.002 (1.6%; observed order 1.03, safety factor 3: asymptotic behaviour not demonstrated by three meshes).
- Geometry approximation, design S: the finest voxel mesh has -0.00% volume error against the STL.
- Load distribution and location: a uniform traction over the load region is assumed; a payload bearing on part of it or with an offset centre of mass is not assessed.
- Manufacturing (machined): its effect on stiffness is represented only through the material values given.

## Assumptions

| Subject | Source | Assumption | Effect |
|---|---|---|---|
| mounting_idealization | user | The mounting region is idealised as rigidly fixed (all displacements zero): underside patch bolted to a rigid base (stiffest idealisation). No contact, slip, fastener or wall flexibility is modelled. | not assessed: no mounting alternative was declared |
| gravitational_acceleration | default | The payload weighs mass x standard gravity 9.80665 m/s^2, applied statically: no dynamic amplification from shocks, vibration or handling. | a different g scales every displacement equally and does not change the ranking |
| load_distribution | default | The load acts as a uniform traction over the load region, so its resultant passes through the region's centroid. A payload that bears on part of the region, or whose centre of mass is offset, loads the bracket differently. | not assessed |
| self_weight | default | The bracket's own weight is not applied: only the payload loads it. | assessed in the sensitivity section |

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

- Resolved study: `study.json` (SHA-256 of its content `3145152d97d81b34557b9705200eb22bb8954338b277d7b05ed8fb2f96cdffe4`); original request `request.json`; every executed operation in `operations.jsonl`.
- Geometry of design U: stored copy `designs/U/inputs/part-cca1c768edf2.stl`, SHA-256 `cca1c768edf296c3900640aa483a8f425eb7446a9563e145da348b6ee0ad4ff6` (original `<source tree>/release/evaluation/user-test-kit/task_geometry/plate_uniform.stl`).
- Geometry of design S: stored copy `designs/S/inputs/part-b95f89b51108.stl`, SHA-256 `b95f89b5110855e2e5acd3e429dd485fba6dc4d6e25bec8ec1ed80bb266f01eb` (original `<source tree>/release/evaluation/user-test-kit/task_geometry/plate_stepped.stl`).
- Software: NAVIER 0.3.0-rc1, operation contract 0.6.0, compiler Apple LLVM 17.0.0 (clang-1700.0.13.5); IEEE 754 double precision; no -ffast-math; no floating-point contraction.
- Platform: Darwin 24.6.0, arm64, 8 CPUs.
- MCP: study_replay {"directory": "<study directory>"} reruns the study from study.json and the stored geometry copies and compares every quantity
- CLI: navier-ctl --embedded study replay "<study directory>"
- Inspect a design: project_open {"path": "<study directory>/designs/<design>"}, then results_query / results_quantities with a run's job_id
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
