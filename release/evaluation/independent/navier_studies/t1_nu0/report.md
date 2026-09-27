# Engineering Evidence Record: t1_nu0

Status: **complete**. Generated 2026-09-16T21:38:58Z. Study hash `f30f8ddaf08dfd4a8a601b09ffc9891c11b4011db03a79d1c5dea59860266333`.

> a record of what a linear-elastic model predicts under stated conditions, with its numerical checks; not a certificate, not a measurement, and not a statement that a part is safe. Every number below is read from `evidence.json` in this folder; the Traceability section says where.

## Question

evaluation: cantilever tip deflection

## Outcome

**Lower predicted load-region displacement under the modeled conditions: design C10 (0.02012 mm) against design C20 (0.16024 mm), a 87.4% difference; numerical uncertainty ±8.1e-07 mm and ±7e-06 mm at 0.625 mm elements.**

Outcome code: `resolved` (compared at the finest mesh every design completed validly: 0.625 mm).

| Design | Load-region displacement (mm) | Numerical uncertainty of the primary quantity | Stiffness (N/mm) | Mass, geometry (kg) | Stiffness / mass (N/mm/kg) | Peak displacement (mm) |
|---|---|---|---|---|---|---|
| C10 | 0.02012 | ±8.1e-07 (0.004%) | 497.03 | 0.0785 | 6331.6 | 0.02018 |
| C20 | 0.16024 | ±7e-06 (0.0044%) | 62.408 | 0.03925 | 1590 | 0.1603 |

Primary quantity: `load_region_displacement` (lower is better). The peak displacement is supporting information: the question concerns the load region.

Robustness of the ranking:

- youngs_modulus: ranking unchanged. every design uses the same material and the loads are forces: all displacements scale with 1/E, so the ranking and the ratio between designs do not depend on E; absolute values scale with it

## Modeled conditions

- **Material:** `steel_nu00` (steel test values, nu 0.0); status user_supplied, source user. E = 200 GPa, nu = 0, density 7850 kg/m3. Reference: evaluation test values
- **Manufacturing:** unspecified (source default)
- **Mounting:** fixed, on each design's mounting region (source user). fixed root
- **Load:** 10 N along (0, 0, -1) (design frame), spread uniformly over the load region; self-weight not included; source user. 
- **Analysis:** static, small-strain linear elasticity, isotropic material; 8-node hexahedra on voxel meshes (staircase boundary), incompatible_modes formulation; solver auto; properties at 20 degC.
- **Frames and units:** design frame: the STL file's axes and origin, lengths in mm; each design's build frame differs by the translation recorded with it.

| Design | File | SHA-256 | Units | Size (mm) | Volume (mm3) | Thinnest wall (mm) | Mounting area (mm2) | Load area (mm2) | Lever arm (mm) |
|---|---|---|---|---|---|---|---|---|---|
| C10 | /private/tmp/claude-501/-Users-sebas-Downloads-Fluid-Sim/22500e7f-fd6d-4763-a5f1-db09bf6ffe3d/scratchpad/eval/independent/C10.stl | `755a5b8cf997` | mm | (100, 10, 10) | 10000 | 10 | 100 | 100 | 100 |
| C20 | /private/tmp/claude-501/-Users-sebas-Downloads-Fluid-Sim/22500e7f-fd6d-4763-a5f1-db09bf6ffe3d/scratchpad/eval/independent/C20.stl | `e28df8633101` | mm | (100, 10, 5) | 5000 | 5 | 50 | 50 | 100 |

Equivalence of the conditions between designs: **differs**. every design against the first: the lever arm from the mounting-region centroid to the load-region centroid (design frame), the areas of both regions and their mean normals. Equal face ids are not evidence of equivalence; these physical quantities are

- lever_arm, design C20: difference 0 mm (tolerance 2) ok
- load_region_area, design C20: difference 50 mm2 (tolerance 2) **differs**
- mounting_region_area, design C20: difference 50 mm2 (tolerance 2) **differs**
- mounting_normal_angle, design C20: difference 0 deg (tolerance 5) ok
- load_normal_angle, design C20: difference 0 deg (tolerance 5) ok

## Results by design and mesh

### Design C10

| h (mm) | Elements | Volume error (%) | Load-region displacement (mm) | Stiffness (N/mm) | Mass, mesh (kg) | Equilibrium | Moment balance | 2U/W - 1 | Line-of-action shift (mm) | Deformation | Valid | Solver | Wall (s) | Factor (MB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2.5 | 640 | 1.41e-12 | 0.0201109 | 497.243 | 0.0785 | 1.7e-12 | 4.7e-13 | -5.5e-13 | 2.2e-14 | within_small_deformation_assumption | yes | sparse Cholesky (nested dissection) | 0.0465 | 5.54 |
| 1.25 | 5120 | 6.93e-12 | 0.0201178 | 497.073 | 0.0785 | 7.1e-12 | 8.8e-12 | 6.5e-11 | 3.4e-14 | within_small_deformation_assumption | yes | sparse Cholesky (nested dissection) | 0.97 | 83.5 |
| 0.625 | 40960 | 4.96e-11 | 0.0201196 | 497.028 | 0.0785 | 2e-11 | 1e-11 | 6.6e-11 | 2.4e-14 | within_small_deformation_assumption | yes | pcg/block-jacobi | 3.05 | n/a |

Refinement of the load-region displacement: monotone convergence with observed order 1.92: asymptotic range plausible. Method: Richardson extrapolation and grid convergence index over the three finest valid meshes (safety factor 1.25).
- Stress at 2.5 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 5.333 MPa, nodal-average maximum 5.981 MPa, nodal 99th percentile 5.875 MPa; peak at a support (singular: grows with refinement)
- Stress at 1.25 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 5.725 MPa, nodal-average maximum 6.069 MPa, nodal 99th percentile 5.78 MPa; peak at a support (singular: grows with refinement)
- Stress at 0.625 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 5.948 MPa, nodal-average maximum 6.135 MPa, nodal 99th percentile 5.512 MPa; peak at a support (singular: grows with refinement)

### Design C20

| h (mm) | Elements | Volume error (%) | Load-region displacement (mm) | Stiffness (N/mm) | Mass, mesh (kg) | Equilibrium | Moment balance | 2U/W - 1 | Line-of-action shift (mm) | Deformation | Valid | Solver | Wall (s) | Factor (MB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2.5 | 320 | 3.9e-13 | 0.160175 | 62.4317 | 0.03925 | 3.9e-12 | 3.2e-12 | 1.9e-11 | 2.2e-14 | within_small_deformation_assumption | yes | sparse Cholesky (nested dissection) | 0.0181 | 2.09 |
| 1.25 | 2560 | 5.93e-13 | 0.160222 | 62.4135 | 0.03925 | 1.4e-11 | 1.4e-11 | 8.7e-11 | 2.2e-14 | within_small_deformation_assumption | yes | sparse Cholesky (nested dissection) | 0.256 | 30.2 |
| 0.625 | 20480 | -8.47e-13 | 0.160235 | 62.4082 | 0.03925 | 5.1e-11 | 7e-11 | 5.1e-10 | 4.2e-15 | within_small_deformation_assumption | yes | sparse Cholesky (nested dissection) | 8.72 | 485 |

Refinement of the load-region displacement: monotone convergence with observed order 1.78: asymptotic range plausible. Method: Richardson extrapolation and grid convergence index over the three finest valid meshes (safety factor 1.25).
- Stress at 2.5 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 18.69 MPa, nodal-average maximum 23.7 MPa, nodal 99th percentile 23.7 MPa; peak at a support (singular: grows with refinement)
- Stress at 1.25 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 21.4 MPa, nodal-average maximum 23.96 MPa, nodal 99th percentile 23.4 MPa; peak at a support (singular: grows with refinement)
- Stress at 0.625 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 22.82 MPa, nodal-average maximum 24.14 MPa, nodal 99th percentile 22.95 MPa; peak at a support (singular: grows with refinement)

## Numerical evidence

| Check | Applies to | Criterion | Observed | Outcome |
|---|---|---|---|---|
| force equilibrium | design C10, 3 completed levels (3 valid) | \|sum of forces\| / force scale <= 1e-6 | largest 1.96e-11 | pass |
| moment balance | design C10, 3 completed levels (3 valid) | \|sum of moments\| / moment scale <= 1e-6 | largest 1.04e-11 | pass |
| energy (Clapeyron) | design C10, 3 completed levels (3 valid) | \|2 U / W - 1\| <= 1e-6 | largest \|2U/W - 1\| 6.61e-11 | pass |
| linear solver | design C10, 3 completed levels (3 valid) | converged, true residual recomputed | converged at every level | pass |
| rigid-body modes | design C10, 3 completed levels (3 valid) | setup_validate finds none before each solve | none (every solve passed validation) | pass |
| load line of action on the mesh | design C10, 3 completed levels (3 valid) | shift <= max(0.5 h, 1% of the lever arm) and force preserved to 1e-6 | largest shift / criterion 3.37e-14 | pass |
| work-conjugate displacement = region mean | design C10, 3 completed levels (3 valid) | relative difference <= 1e-9 (a uniform traction does work with the area mean) | largest 4.31e-15 | pass |
| mounting region on the mesh | design C10, 3 completed levels (3 valid) | faces present, mesh/geometry area ratio in (0.5, 2) | represented at every level | pass |
| mesh topology | design C10, 3 completed levels (3 valid) | one face-connected region | one region at every level | pass |
| small deformation | design C10, 3 completed levels (3 valid) | displacement <= 1% of size and rotation error <= 1e-3 (questionable up to 5% / 1e-2) | within_small_deformation_assumption | pass |
| geometry approximation (volume) | design C10, 3 completed levels (3 valid) | reported; > 3% changes stiffness and mass noticeably | 4.96e-11% at the finest completed level | pass |
| discretisation (load-region displacement) | design C10, 3 completed levels (3 valid) | last relative change <= 0.02 | monotone convergence with observed order 1.92: asymptotic range plausible; relative uncertainty 4.02e-05 | pass |
| force equilibrium | design C20, 3 completed levels (3 valid) | \|sum of forces\| / force scale <= 1e-6 | largest 5.1e-11 | pass |
| moment balance | design C20, 3 completed levels (3 valid) | \|sum of moments\| / moment scale <= 1e-6 | largest 6.97e-11 | pass |
| energy (Clapeyron) | design C20, 3 completed levels (3 valid) | \|2 U / W - 1\| <= 1e-6 | largest \|2U/W - 1\| 5.09e-10 | pass |
| linear solver | design C20, 3 completed levels (3 valid) | converged, true residual recomputed | converged at every level | pass |
| rigid-body modes | design C20, 3 completed levels (3 valid) | setup_validate finds none before each solve | none (every solve passed validation) | pass |
| load line of action on the mesh | design C20, 3 completed levels (3 valid) | shift <= max(0.5 h, 1% of the lever arm) and force preserved to 1e-6 | largest shift / criterion 2.22e-14 | pass |
| work-conjugate displacement = region mean | design C20, 3 completed levels (3 valid) | relative difference <= 1e-9 (a uniform traction does work with the area mean) | largest 1.73e-16 | pass |
| mounting region on the mesh | design C20, 3 completed levels (3 valid) | faces present, mesh/geometry area ratio in (0.5, 2) | represented at every level | pass |
| mesh topology | design C20, 3 completed levels (3 valid) | one face-connected region | one region at every level | pass |
| small deformation | design C20, 3 completed levels (3 valid) | displacement <= 1% of size and rotation error <= 1e-3 (questionable up to 5% / 1e-2) | within_small_deformation_assumption | pass |
| geometry approximation (volume) | design C20, 3 completed levels (3 valid) | reported; > 3% changes stiffness and mass noticeably | -8.47e-13% at the finest completed level | pass |
| discretisation (load-region displacement) | design C20, 3 completed levels (3 valid) | last relative change <= 0.02 | monotone convergence with observed order 1.78: asymptotic range plausible; relative uncertainty 4.35e-05 | pass |

A small solver residual is not a discretisation-error estimate: the discretisation rows come from the refinement study.

## Interpretation

- Lower predicted load-region displacement under the modeled conditions: design C10 (0.02012 mm) against design C20 (0.16024 mm), a 87.4% difference; numerical uncertainty ±8.1e-07 mm and ±7e-06 mm at 0.625 mm elements.
- These are predictions of a static, small-strain, linear-elastic model for: material 'steel_nu00' (user_supplied values, source user); mounting region rigidly fixed; a static force spread uniformly over the load region. They are conditional on those inputs and are not measurements.
- Stress values are supporting information only. No strength margin or safety statement is made (see not evaluated).

## Important uncertainties

- Discretisation, design C10: monotone convergence with observed order 1.92: asymptotic range plausible (Richardson extrapolation and grid convergence index over the three finest valid meshes (safety factor 1.25)).
- Geometry approximation, design C10: the finest voxel mesh has 0.00% volume error against the STL.
- Discretisation, design C20: monotone convergence with observed order 1.78: asymptotic range plausible (Richardson extrapolation and grid convergence index over the three finest valid meshes (safety factor 1.25)).
- Geometry approximation, design C20: the finest voxel mesh has -0.00% volume error against the STL.
- Load distribution and location: a uniform traction over the load region is assumed; a payload bearing on part of it or with an offset centre of mass is not assessed.
- Manufacturing: not specified; the material is treated as homogeneous and isotropic.

## Assumptions

| Subject | Source | Assumption | Effect |
|---|---|---|---|
| manufacturing | default | The manufacturing process is not specified: the material values are used as given, as for a homogeneous, isotropic, defect-free part. | not assessed |
| mounting_idealization | user | The mounting region is idealised as rigidly fixed (all displacements zero): fixed root. No contact, slip, fastener or wall flexibility is modelled. | not assessed: no mounting alternative was declared |
| load_distribution | default | The load acts as a uniform traction over the load region, so its resultant passes through the region's centroid. A payload that bears on part of the region, or whose centre of mass is offset, loads the bracket differently. | not assessed |
| self_weight | default | The bracket's own weight is not applied: only the payload loads it. | not assessed; the weight of each design is reported |
| equivalence | user | The designs are not mounted and loaded equivalently: load_region_area of 'C20' differs by 50 mm2 (tolerance 2); mounting_region_area of 'C20' differs by 50 mm2 (tolerance 2). Is this difference part of the design change (accept it with the reason), or should the regions be redefined? Accepted: the cross-section or length is the difference being studied | accepted by the user; not assessed further |

Questions the user answered by accepting an assumption:

- `equivalence`: The designs are not mounted and loaded equivalently: load_region_area of 'C20' differs by 50 mm2 (tolerance 2); mounting_region_area of 'C20' differs by 50 mm2 (tolerance 2). Is this difference part of the design change (accept it with the reason), or should the regions be redefined? Reason given: the cross-section or length is the difference being studied

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

- Resolved study: `study.json` (SHA-256 of its content `f30f8ddaf08dfd4a8a601b09ffc9891c11b4011db03a79d1c5dea59860266333`); original request `request.json`; every executed operation in `operations.jsonl`.
- Geometry of design C10: stored copy `designs/C10/inputs/part-755a5b8cf997.stl`, SHA-256 `755a5b8cf9977e04d204095d881c6c249af9451cfe1f91f47ad552df2d2fd728` (original `/private/tmp/claude-501/-Users-sebas-Downloads-Fluid-Sim/22500e7f-fd6d-4763-a5f1-db09bf6ffe3d/scratchpad/eval/independent/C10.stl`).
- Geometry of design C20: stored copy `designs/C20/inputs/part-e28df8633101.stl`, SHA-256 `e28df8633101154bddf1c175c7795223358e8011d41589dd117111d538faa702` (original `/private/tmp/claude-501/-Users-sebas-Downloads-Fluid-Sim/22500e7f-fd6d-4763-a5f1-db09bf6ffe3d/scratchpad/eval/independent/C20.stl`).
- Software: NAVIER 0.3.0-rc1, operation contract 0.5.0, compiler Apple LLVM 17.0.0 (clang-1700.0.13.5); IEEE 754 double precision; no -ffast-math; no floating-point contraction.
- Platform: Darwin 24.6.0, arm64, 8 CPUs.
- MCP: study_replay {"directory": "/private/tmp/claude-501/-Users-sebas-Downloads-Fluid-Sim/22500e7f-fd6d-4763-a5f1-db09bf6ffe3d/scratchpad/eval/independent/t1_nu0"} reruns the study from study.json and the stored geometry copies and compares every quantity
- CLI: navier-ctl --embedded study replay "/private/tmp/claude-501/-Users-sebas-Downloads-Fluid-Sim/22500e7f-fd6d-4763-a5f1-db09bf6ffe3d/scratchpad/eval/independent/t1_nu0"
- Inspect a design: project_open {"path": "/private/tmp/claude-501/-Users-sebas-Downloads-Fluid-Sim/22500e7f-fd6d-4763-a5f1-db09bf6ffe3d/scratchpad/eval/independent/t1_nu0/designs/<design>"}, then results_query / results_quantities with a run's job_id
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
