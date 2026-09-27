# Engineering Evidence Record: bracket_ab

Status: **complete**. Generated 2026-09-18T00:10:30Z. Study hash `ef0f326cc05dc6e98bc3b723c141fc251cacc7a4bba2da05bdbbd1f15955b86a`.

> a record of what a linear-elastic model predicts under stated conditions, with its numerical checks; not a certificate, not a measurement, and not a statement that a part is safe. Every number below is read from `evidence.json` in this folder; the Traceability section says where.

## Question

Here are two versions of my bracket. They attach through the back face and carry approximately 3 kg on the pad at the end of the arm. Compare their stiffness and mass, explain the assumptions, and tell me what information could change the comparison.

Decision to support: choose between version A (plain L, 8 mm plates) and version B (the same with a 24 mm inner chamfer)

## Outcome

**Lower predicted load-region displacement under the modeled conditions on every tested mesh: design B against design A, a difference of 60.5% to 65.5% over 3 meshes (4 to 1 mm); at 1 mm 0.0087746 mm against 0.025399 mm. No discretisation-error estimate is available for design B (the voxel representation of the geometry changes with the mesh, so the meshes discretise different shapes (volume errors -3.57%, -1.79%, -0.893%)), so the size of the difference is not estimated beyond the tested meshes. The difference at the finest mesh (0.017 mm) is larger than the sum of the designs' last changes between meshes (0.00052 mm). Convergence criterion (2% change between the two finest meshes, relative to the finer) not met by design B (-3.53%), so the size of the difference is less certain than the ranking.**

Outcome code: `ranking_consistent_on_tested_meshes` (compared at the finest mesh every design completed validly: 1 mm).

| Design | Load-region displacement (mm) | Last change of the primary quantity (relative to the finer mesh) | Convergence criterion | Discretisation-error estimate of the primary quantity | Stiffness (N/mm) | Mass, geometry (kg) | Stiffness / mass (N/mm/kg) | Peak displacement (mm) |
|---|---|---|---|---|---|---|---|---|
| A | 0.025399 | +0.85% | met | 0.00061 (2.4%; safety factor 3) | 1158.3 | 0.114 | 10156 | 0.03005 |
| B | 0.0087746 | -3.53% | **not met** | not available: the voxel representation of the geometry changes with the mesh, so the meshes discretise different shapes (volume errors -3.57%, -1.79%, -0.893%) | 3352.9 | 0.1452 | 23099 | 0.01107 |

Primary quantity: `load_region_displacement` (lower is better). The peak displacement is supporting information: the question concerns the load region. A discretisation-error estimate is a grid convergence index: an estimate, not a bound or a confidence interval; it is offered only when its conditions hold (see the refinement of each design).

Ranking on the tested meshes (the same on all 3):

| h (mm) | Ranking, best first | Difference between the best two (relative to the second) |
|---|---|---|
| 4 | B, A | 60.52% |
| 2 | B, A | 63.93% |
| 1 | B, A | 65.45% |

Robustness of the ranking:

- youngs_modulus: ranking unchanged. every design uses the same material and the loads are forces: all displacements scale with 1/E, so the ranking and the ratio between designs do not depend on E; absolute values scale with it
- youngs_modulus (Young's modulus x 1.1): ranking unchanged, largest change 9.09%
- poisson_ratio (Poisson's ratio 0.3): ranking unchanged, largest change 0.791%
- poisson_ratio (Poisson's ratio 0.36): ranking unchanged, largest change 0.876%
- mounting_alternative (two_bolt_pads): ranking unchanged, largest change 25.5%
- self_weight (own weight added): ranking unchanged, largest change 0.977%

## Modeled conditions

- **Material:** `al6061_t6_nominal` (Aluminium 6061-T6, nominal handbook values); status user_supplied, source database. E = 68.9 GPa, nu = 0.33, density 2700 kg/m3. Reference: nominal handbook values for 6061-T6 aluminium
- **Manufacturing:** machined (source user)
- **Mounting:** fixed, on each design's mounting region (source user). back face clamped to a rigid wall (the stiffest idealisation of a bolted joint)
- **Load:** 3 kg payload x g = 9.80665 m/s2 (source default) = 29.42 N along (0, 0, -1) (design frame), spread uniformly over the load region; self-weight not included; source user. 3 kg item resting on the pad; z points up in both files
- **Analysis:** static, small-strain linear elasticity, isotropic material; 8-node hexahedra on voxel meshes (staircase boundary), incompatible_modes formulation; solver auto; properties at 20 degC.
- **Frames and units:** design frame: the STL file's axes and origin, lengths in mm; each design's build frame differs by the translation recorded with it.

| Design | File | SHA-256 | Units | Size (mm) | Volume (mm3) | Thinnest wall (mm) | Mounting area (mm2) | Load area (mm2) | Lever arm (mm) |
|---|---|---|---|---|---|---|---|---|---|
| A | /private/tmp/navier-package-g8ENVv/navier-0.3.0-rc2-macos-arm64/share/navier/examples/bracket_comparison/geometry/bracket_a_plain.stl | `7cf8844569be` | mm | (80, 40, 60) | 42240 | 8 | 2400 | 256 | 78 |
| B | /private/tmp/navier-package-g8ENVv/navier-0.3.0-rc2-macos-arm64/share/navier/examples/bracket_comparison/geometry/bracket_b_chamfer.stl | `806efa160fae` | mm | (80, 40, 60) | 53760 | 8 | 2400 | 256 | 78 |

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
| 4 | 660 | 6.58e-13 | 0.0247427 | 1189.03 | 0.11405 | 9.8e-13 | 1e-12 | 1.3e-11 | 7.5e-15 | within_small_deformation_assumption | yes | sparse Cholesky (nested dissection) | 0.116 | 4.91 |
| 2 | 5280 | -4.97e-12 | 0.0251846 | 1168.17 | 0.11405 | 4.1e-12 | 4.9e-12 | 6e-11 | 1.8e-13 | within_small_deformation_assumption | yes | sparse Cholesky (nested dissection) | 4.62 | 76.8 |
| 1 | 42240 | 2.48e-11 | 0.0253994 | 1158.29 | 0.11405 | 1.5e-11 | 1.9e-11 | 1.5e-10 | 2.4e-13 | within_small_deformation_assumption | yes | pcg/block-jacobi | 11.7 | n/a |

Refinement of the load-region displacement: last change +0.85% (relative to the finer mesh) meets the 2% criterion; discretisation-error estimate 0.00061 (2.4%; observed order 1.04, safety factor 3: asymptotic behaviour not demonstrated by three meshes).
- Changes between meshes (relative to the finer mesh): +1.75%, +0.85%
- Observed order over (4, 2, 1) mm: 1.04
- Estimate condition "at least three valid meshes": met (3 valid meshes)
- Estimate condition "constant refinement ratio of at least 1.3 over the three finest meshes": met (ratios 2 and 2)
- Estimate condition "monotone change over the three finest meshes": met (changes +0.0004418 and +0.0002149)
- Estimate condition "the geometry is represented identically on the three finest meshes (volume error within 1e-6)": met (volume errors 6.58e-13%, -4.97e-12%, 2.48e-11%)
- Estimate condition "observed order of the three finest meshes within [0.5, 4]": met (p = 1.04)
- Stress at 4 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 3.381 MPa, nodal-average maximum 4.223 MPa, nodal 99th percentile 3.833 MPa; peak at a sharp inside corner (singular)
- Stress at 2 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 4.552 MPa, nodal-average maximum 4.967 MPa, nodal 99th percentile 3.823 MPa; peak at a sharp inside corner (singular)
- Stress at 1 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 5.984 MPa, nodal-average maximum 6.071 MPa, nodal 99th percentile 3.749 MPa; peak at a sharp inside corner (singular)

| Sensitivity | Parameter | h (mm) | Load-region displacement (mm) | Stiffness (N/mm) | Valid |
|---|---|---|---|---|---|
| youngs_modulus | Young's modulus x 1.1 | 1 | 0.0230904 | 1274.12 | yes |
| poisson_ratio | Poisson's ratio 0.3 | 1 | 0.0256004 | 1149.2 | yes |
| poisson_ratio | Poisson's ratio 0.36 | 1 | 0.025177 | 1168.52 | yes |
| mounting_alternative | two_bolt_pads | 1 | 0.0318755 | 922.965 | yes |
| self_weight | own weight added | 1 | 0.0256475 | 1147.09 | yes |

### Design B

| h (mm) | Elements | Volume error (%) | Load-region displacement (mm) | Stiffness (N/mm) | Mass, mesh (kg) | Equilibrium | Moment balance | 2U/W - 1 | Line-of-action shift (mm) | Deformation | Valid | Solver | Wall (s) | Factor (MB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 4 | 810 | -3.57 | 0.0097683 | 3011.78 | 0.13997 | 7.5e-13 | 5.2e-13 | 5.1e-12 | 7.5e-15 | within_small_deformation_assumption | yes | sparse Cholesky (nested dissection) | 0.264 | 7.27 |
| 2 | 6600 | -1.79 | 0.00908461 | 3238.44 | 0.14256 | 2.8e-12 | 1.9e-12 | 1.9e-11 | 1.8e-13 | within_small_deformation_assumption | yes | sparse Cholesky (nested dissection) | 11.9 | 132 |
| 1 | 53280 | -0.893 | 0.00877455 | 3352.87 | 0.14386 | 2.1e-11 | 5.4e-12 | 4.4e-11 | 2.4e-13 | within_small_deformation_assumption | yes | pcg/block-jacobi | 15.5 | n/a |

Refinement of the load-region displacement: last change -3.53% (relative to the finer mesh) does not meet the 2% criterion; no discretisation-error estimate: the voxel representation of the geometry changes with the mesh, so the meshes discretise different shapes (volume errors -3.57%, -1.79%, -0.893%).
- Changes between meshes (relative to the finer mesh): -7.53%, -3.53%
- Observed order over (4, 2, 1) mm: 1.14
- Estimate condition "at least three valid meshes": met (3 valid meshes)
- Estimate condition "constant refinement ratio of at least 1.3 over the three finest meshes": met (ratios 2 and 2)
- Estimate condition "monotone change over the three finest meshes": met (changes -0.0006837 and -0.0003101)
- Estimate condition "the geometry is represented identically on the three finest meshes (volume error within 1e-6)": not met (volume errors -3.57%, -1.79%, -0.893%)
- Estimate condition "observed order of the three finest meshes within [0.5, 4]": met (p = 1.14)
- Stress at 4 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 2.29 MPa, nodal-average maximum 2.824 MPa, nodal 99th percentile 2.558 MPa; peak at a sharp inside corner (singular)
- Stress at 2 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 2.955 MPa, nodal-average maximum 3.192 MPa, nodal 99th percentile 2.414 MPa; peak at a sharp inside corner (singular)
- Stress at 1 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 3.744 MPa, nodal-average maximum 3.778 MPa, nodal 99th percentile 2.246 MPa; peak at a sharp inside corner (singular)

| Sensitivity | Parameter | h (mm) | Load-region displacement (mm) | Stiffness (N/mm) | Valid |
|---|---|---|---|---|---|
| youngs_modulus | Young's modulus x 1.1 | 1 | 0.00797687 | 3688.16 | yes |
| poisson_ratio | Poisson's ratio 0.3 | 1 | 0.00882905 | 3332.17 | yes |
| poisson_ratio | Poisson's ratio 0.36 | 1 | 0.00871408 | 3376.14 | yes |
| mounting_alternative | two_bolt_pads | 1 | 0.0104734 | 2809.01 | yes |
| self_weight | own weight added | 1 | 0.00884362 | 3326.69 | yes |

## Numerical evidence

| Check | Applies to | Criterion | Observed | Outcome |
|---|---|---|---|---|
| force equilibrium | design A, 3 completed levels (3 valid) | \|sum of forces\| / force scale <= 1e-6 | largest 1.48e-11 | pass |
| moment balance | design A, 3 completed levels (3 valid) | \|sum of moments\| / moment scale <= 1e-6 | largest 1.9e-11 | pass |
| energy (Clapeyron) | design A, 3 completed levels (3 valid) | \|2 U / W - 1\| <= 1e-6 | largest \|2U/W - 1\| 1.5e-10 | pass |
| linear solver | design A, 3 completed levels (3 valid) | converged, true residual recomputed | converged at every level | pass |
| rigid-body modes | design A, 3 completed levels (3 valid) | setup_validate finds none before each solve | none (every solve passed validation) | pass |
| load line of action on the mesh | design A, 3 completed levels (3 valid) | shift <= max(0.5 h, 1% of the lever arm) and force preserved to 1e-6 | largest shift / criterion 3.05e-13 | pass |
| work-conjugate displacement = region mean | design A, 3 completed levels (3 valid) | relative difference <= 1e-9 (a uniform traction does work with the area mean) | largest 1.12e-14 | pass |
| mounting region on the mesh | design A, 3 completed levels (3 valid) | faces present, mesh/geometry area ratio in (0.5, 2) | represented at every level | pass |
| mesh topology | design A, 3 completed levels (3 valid) | one face-connected region | one region at every level | pass |
| small deformation | design A, 3 completed levels (3 valid) | displacement <= 1% of size and rotation error <= 1e-3 (questionable up to 5% / 1e-2) | within_small_deformation_assumption | pass |
| geometry approximation (volume) | design A, 3 completed levels (3 valid) | reported; > 3% changes stiffness and mass noticeably | 2.48e-11% at the finest completed level | pass |
| discretisation (load-region displacement) | design A, 3 completed levels (3 valid) | convergence criterion: change between the two finest valid meshes, relative to the finer, within 2% | last change +0.85% (relative to the finer mesh) meets the 2% criterion; discretisation-error estimate 0.00061 (2.4%; observed order 1.04, safety factor 3: asymptotic behaviour not demonstrated by three meshes) | pass |
| Young's modulus scaling | design A, 3 completed levels (3 valid) | linear elasticity: displacement proportional to 1/E, to 1e-6 | \|u(E x 1.1) x 1.1 / u(E) - 1\| = 2.65e-10 | pass |
| force equilibrium | design B, 3 completed levels (3 valid) | \|sum of forces\| / force scale <= 1e-6 | largest 2.07e-11 | pass |
| moment balance | design B, 3 completed levels (3 valid) | \|sum of moments\| / moment scale <= 1e-6 | largest 5.45e-12 | pass |
| energy (Clapeyron) | design B, 3 completed levels (3 valid) | \|2 U / W - 1\| <= 1e-6 | largest \|2U/W - 1\| 4.43e-11 | pass |
| linear solver | design B, 3 completed levels (3 valid) | converged, true residual recomputed | converged at every level | pass |
| rigid-body modes | design B, 3 completed levels (3 valid) | setup_validate finds none before each solve | none (every solve passed validation) | pass |
| load line of action on the mesh | design B, 3 completed levels (3 valid) | shift <= max(0.5 h, 1% of the lever arm) and force preserved to 1e-6 | largest shift / criterion 3.05e-13 | pass |
| work-conjugate displacement = region mean | design B, 3 completed levels (3 valid) | relative difference <= 1e-9 (a uniform traction does work with the area mean) | largest 1.05e-14 | pass |
| mounting region on the mesh | design B, 3 completed levels (3 valid) | faces present, mesh/geometry area ratio in (0.5, 2) | represented at every level | pass |
| mesh topology | design B, 3 completed levels (3 valid) | one face-connected region | one region at every level | pass |
| small deformation | design B, 3 completed levels (3 valid) | displacement <= 1% of size and rotation error <= 1e-3 (questionable up to 5% / 1e-2) | within_small_deformation_assumption | pass |
| geometry approximation (volume) | design B, 3 completed levels (3 valid) | reported; > 3% changes stiffness and mass noticeably | -0.893% at the finest completed level | pass |
| discretisation (load-region displacement) | design B, 3 completed levels (3 valid) | convergence criterion: change between the two finest valid meshes, relative to the finer, within 2% | last change -3.53% (relative to the finer mesh) does not meet the 2% criterion; no discretisation-error estimate: the voxel representation of the geometry changes with the mesh, so the meshes discretise different shapes (volume errors -3.57%, -1.79%, -0.893%) | **fail** |
| Young's modulus scaling | design B, 3 completed levels (3 valid) | linear elasticity: displacement proportional to 1/E, to 1e-6 | \|u(E x 1.1) x 1.1 / u(E) - 1\| = 6.73e-11 | pass |

A small solver residual is not a discretisation-error estimate: the discretisation rows come from the refinement study.

## Interpretation

- Lower predicted load-region displacement under the modeled conditions on every tested mesh: design B against design A, a difference of 60.5% to 65.5% over 3 meshes (4 to 1 mm); at 1 mm 0.0087746 mm against 0.025399 mm. No discretisation-error estimate is available for design B (the voxel representation of the geometry changes with the mesh, so the meshes discretise different shapes (volume errors -3.57%, -1.79%, -0.893%)), so the size of the difference is not estimated beyond the tested meshes. The difference at the finest mesh (0.017 mm) is larger than the sum of the designs' last changes between meshes (0.00052 mm). Convergence criterion (2% change between the two finest meshes, relative to the finer) not met by design B (-3.53%), so the size of the difference is less certain than the ranking.
- These are predictions of a static, small-strain, linear-elastic model for: material 'al6061_t6_nominal' (user_supplied values, source database); mounting region rigidly fixed; a static payload weight spread uniformly over the load region. They are conditional on those inputs and are not measurements.
- Stress values are supporting information only. No strength margin or safety statement is made (see not evaluated).

## Important uncertainties

- Poisson's ratio 0.3: the load-region displacement changes by up to 0.79%; the ranking is unchanged.
- Poisson's ratio 0.36: the load-region displacement changes by up to 0.88%; the ranking is unchanged.
- Mounting: with the alternative 'two_bolt_pads' the load-region displacement changes by up to 25.5%; the ranking is unchanged. A real attachment can lie between these idealisations or outside them (for example a flexible fastener), so this indicates the effect of the mounting without bounding it.
- Self-weight: adding each bracket's own weight changes the load-region displacement by up to 0.98%; the ranking is unchanged.
- Discretisation, design A: last change +0.85% (relative to the finer mesh) meets the 2% criterion; discretisation-error estimate 0.00061 (2.4%; observed order 1.04, safety factor 3: asymptotic behaviour not demonstrated by three meshes).
- Geometry approximation, design A: the finest voxel mesh has 0.00% volume error against the STL.
- Discretisation, design B: last change -3.53% (relative to the finer mesh) does not meet the 2% criterion; no discretisation-error estimate: the voxel representation of the geometry changes with the mesh, so the meshes discretise different shapes (volume errors -3.57%, -1.79%, -0.893%).
- Geometry approximation, design B: the finest voxel mesh has -0.89% volume error against the STL.
- Load distribution and location: a uniform traction over the load region is assumed; a payload bearing on part of it or with an offset centre of mass is not assessed.
- Manufacturing (machined): its effect on stiffness is represented only through the material values given.

## Assumptions

| Subject | Source | Assumption | Effect |
|---|---|---|---|
| mounting_idealization | user | The mounting region is idealised as rigidly fixed (all displacements zero): back face clamped to a rigid wall (the stiffest idealisation of a bolted joint). No contact, slip, fastener or wall flexibility is modelled. | assessed by the mounting alternatives in the sensitivity section |
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

- Resolved study: `study.json` (SHA-256 of its content `ef0f326cc05dc6e98bc3b723c141fc251cacc7a4bba2da05bdbbd1f15955b86a`); original request `request.json`; every executed operation in `operations.jsonl`.
- Geometry of design A: stored copy `designs/A/inputs/part-7cf8844569be.stl`, SHA-256 `7cf8844569be4b9bd80fab7c8f2033eadd160e29b012760e2bb35ddab8ebef19` (original `/private/tmp/navier-package-g8ENVv/navier-0.3.0-rc2-macos-arm64/share/navier/examples/bracket_comparison/geometry/bracket_a_plain.stl`).
- Geometry of design B: stored copy `designs/B/inputs/part-806efa160fae.stl`, SHA-256 `806efa160faedf8b5b3fb6bef1e4e9a8e764bc0caa4445cca76f9069deb0c528` (original `/private/tmp/navier-package-g8ENVv/navier-0.3.0-rc2-macos-arm64/share/navier/examples/bracket_comparison/geometry/bracket_b_chamfer.stl`).
- Software: NAVIER 0.3.0-rc2, operation contract 0.6.0, compiler Apple LLVM 17.0.0 (clang-1700.0.13.5); IEEE 754 double precision; no -ffast-math; no floating-point contraction.
- Platform: Darwin 24.6.0, arm64, 8 CPUs.
- MCP: study_replay {"directory": "/private/tmp/navier-package-g8ENVv/example-run"} reruns the study from study.json and the stored geometry copies and compares every quantity
- CLI: navier-ctl --embedded study replay "/private/tmp/navier-package-g8ENVv/example-run"
- Inspect a design: project_open {"path": "/private/tmp/navier-package-g8ENVv/example-run/designs/<design>"}, then results_query / results_quantities with a run's job_id
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
