# Engineering Evidence Record: t2_t3

Status: **complete**. Generated 2026-09-16T21:39:33Z. Study hash `d9e287ce7a2802d2bf031f30780f404d4f5a122d14cdc2a8213bb8f57f74f0c7`.

> a record of what a linear-elastic model predicts under stated conditions, with its numerical checks; not a certificate, not a measurement, and not a statement that a part is safe. Every number below is read from `evidence.json` in this folder; the Traceability section says where.

## Question

evaluation: cantilever tip deflection

## Outcome

**Lower predicted load-region displacement under the modeled conditions: design C10W (0.0099452 mm) against design C10L (0.15974 mm), a 93.8% difference; numerical uncertainty ±5.6e-06 mm and ±4.8e-05 mm at 0.625 mm elements.**

Outcome code: `resolved` (compared at the finest mesh every design completed validly: 0.625 mm).

| Design | Load-region displacement (mm) | Numerical uncertainty of the primary quantity | Stiffness (N/mm) | Mass, geometry (kg) | Stiffness / mass (N/mm/kg) | Peak displacement (mm) |
|---|---|---|---|---|---|---|
| C10L | 0.15974 | ±4.8e-05 (0.03%) | 62.603 | 0.157 | 398.74 | 0.1599 |
| C10W | 0.0099452 | ±5.6e-06 (0.056%) | 1005.5 | 0.157 | 6404.5 | 0.009975 |

Primary quantity: `load_region_displacement` (lower is better). The peak displacement is supporting information: the question concerns the load region.

Robustness of the ranking:

- youngs_modulus: ranking unchanged. every design uses the same material and the loads are forces: all displacements scale with 1/E, so the ranking and the ratio between designs do not depend on E; absolute values scale with it

## Modeled conditions

- **Material:** `steel_nu03` (steel test values, nu 0.3); status user_supplied, source user. E = 200 GPa, nu = 0.3, density 7850 kg/m3. Reference: evaluation test values
- **Manufacturing:** unspecified (source default)
- **Mounting:** fixed, on each design's mounting region (source user). fixed root
- **Load:** 10 N along (0, 0, -1) (design frame), spread uniformly over the load region; self-weight not included; source user. 
- **Analysis:** static, small-strain linear elasticity, isotropic material; 8-node hexahedra on voxel meshes (staircase boundary), incompatible_modes formulation; solver auto; properties at 20 degC.
- **Frames and units:** design frame: the STL file's axes and origin, lengths in mm; each design's build frame differs by the translation recorded with it.

| Design | File | SHA-256 | Units | Size (mm) | Volume (mm3) | Thinnest wall (mm) | Mounting area (mm2) | Load area (mm2) | Lever arm (mm) |
|---|---|---|---|---|---|---|---|---|---|
| C10L | /private/tmp/claude-501/-Users-sebas-Downloads-Fluid-Sim/22500e7f-fd6d-4763-a5f1-db09bf6ffe3d/scratchpad/eval/independent/C10L.stl | `7c44e8eebb5e` | mm | (200, 10, 10) | 20000 | 10 | 100 | 100 | 200 |
| C10W | /private/tmp/claude-501/-Users-sebas-Downloads-Fluid-Sim/22500e7f-fd6d-4763-a5f1-db09bf6ffe3d/scratchpad/eval/independent/C10W.stl | `cff6b24aaabb` | mm | (100, 20, 10) | 20000 | 10 | 200 | 200 | 100 |

Equivalence of the conditions between designs: **differs**. every design against the first: the lever arm from the mounting-region centroid to the load-region centroid (design frame), the areas of both regions and their mean normals. Equal face ids are not evidence of equivalence; these physical quantities are

- lever_arm, design C10W: difference 100 mm (tolerance 4) **differs**
- load_region_area, design C10W: difference 100 mm2 (tolerance 2) **differs**
- mounting_region_area, design C10W: difference 100 mm2 (tolerance 2) **differs**
- mounting_normal_angle, design C10W: difference 0 deg (tolerance 5) ok
- load_normal_angle, design C10W: difference 0 deg (tolerance 5) ok

## Results by design and mesh

### Design C10L

| h (mm) | Elements | Volume error (%) | Load-region displacement (mm) | Stiffness (N/mm) | Mass, mesh (kg) | Equilibrium | Moment balance | 2U/W - 1 | Line-of-action shift (mm) | Deformation | Valid | Solver | Wall (s) | Factor (MB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2.5 | 1280 | -6.61e-13 | 0.159558 | 62.6732 | 0.157 | 1.1e-11 | 1.7e-11 | -5.8e-11 | 4.4e-14 | within_small_deformation_assumption | yes | sparse Cholesky (nested dissection) | 0.0934 | 11.6 |
| 1.25 | 10240 | -1.05e-11 | 0.159684 | 62.6236 | 0.157 | 4.5e-11 | 1.2e-10 | -8.9e-10 | 6.7e-14 | within_small_deformation_assumption | yes | sparse Cholesky (nested dissection) | 2.11 | 176 |
| 0.625 | 81920 | 7.48e-11 | 0.159737 | 62.6029 | 0.157 | 3e-10 | 2.8e-10 | 8.8e-10 | 4.5e-14 | within_small_deformation_assumption | yes | pcg/block-jacobi | 19.3 | n/a |

Refinement of the load-region displacement: monotone convergence with observed order 1.26: asymptotic range plausible. Method: Richardson extrapolation and grid convergence index over the three finest valid meshes (safety factor 1.25).
- Stress at 2.5 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 10.8 MPa, nodal-average maximum 12.37 MPa, nodal 99th percentile 11.53 MPa; peak at a support (singular: grows with refinement)
- Stress at 1.25 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 12.57 MPa, nodal-average maximum 13.04 MPa, nodal 99th percentile 11.33 MPa; peak at a support (singular: grows with refinement)
- Stress at 0.625 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 15.12 MPa, nodal-average maximum 14.7 MPa, nodal 99th percentile 10.91 MPa; peak at a support (singular: grows with refinement)

### Design C10W

| h (mm) | Elements | Volume error (%) | Load-region displacement (mm) | Stiffness (N/mm) | Mass, mesh (kg) | Equilibrium | Moment balance | 2U/W - 1 | Line-of-action shift (mm) | Deformation | Valid | Solver | Wall (s) | Factor (MB) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2.5 | 1280 | -6.61e-13 | 0.00992162 | 1007.9 | 0.157 | 2e-12 | 2.1e-12 | -1.5e-11 | 2.2e-14 | within_small_deformation_assumption | yes | sparse Cholesky (nested dissection) | 0.12 | 13.7 |
| 1.25 | 10240 | -1.05e-11 | 0.00993847 | 1006.19 | 0.157 | 8.6e-12 | 5e-12 | -3.4e-11 | 8.3e-15 | within_small_deformation_assumption | yes | sparse Cholesky (nested dissection) | 3.76 | 227 |
| 0.625 | 81920 | 7.48e-11 | 0.00994518 | 1005.51 | 0.157 | 3.4e-11 | 1.6e-11 | 6.6e-11 | 1.1e-13 | within_small_deformation_assumption | yes | pcg/block-jacobi | 9.28 | n/a |

Refinement of the load-region displacement: monotone convergence with observed order 1.33: asymptotic range plausible. Method: Richardson extrapolation and grid convergence index over the three finest valid meshes (safety factor 1.25).
- Stress at 2.5 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 2.699 MPa, nodal-average maximum 3.047 MPa, nodal 99th percentile 2.78 MPa; peak at a support (singular: grows with refinement)
- Stress at 1.25 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 3.157 MPa, nodal-average maximum 3.253 MPa, nodal 99th percentile 2.701 MPa; peak at a support (singular: grows with refinement)
- Stress at 0.625 mm (supporting information, not a strength assessment): von Mises Gauss-point maximum 3.795 MPa, nodal-average maximum 3.681 MPa, nodal 99th percentile 2.651 MPa; peak at a support (singular: grows with refinement)

## Numerical evidence

| Check | Applies to | Criterion | Observed | Outcome |
|---|---|---|---|---|
| force equilibrium | design C10L, 3 completed levels (3 valid) | \|sum of forces\| / force scale <= 1e-6 | largest 3e-10 | pass |
| moment balance | design C10L, 3 completed levels (3 valid) | \|sum of moments\| / moment scale <= 1e-6 | largest 2.78e-10 | pass |
| energy (Clapeyron) | design C10L, 3 completed levels (3 valid) | \|2 U / W - 1\| <= 1e-6 | largest \|2U/W - 1\| 8.94e-10 | pass |
| linear solver | design C10L, 3 completed levels (3 valid) | converged, true residual recomputed | converged at every level | pass |
| rigid-body modes | design C10L, 3 completed levels (3 valid) | setup_validate finds none before each solve | none (every solve passed validation) | pass |
| load line of action on the mesh | design C10L, 3 completed levels (3 valid) | shift <= max(0.5 h, 1% of the lever arm) and force preserved to 1e-6 | largest shift / criterion 3.34e-14 | pass |
| work-conjugate displacement = region mean | design C10L, 3 completed levels (3 valid) | relative difference <= 1e-9 (a uniform traction does work with the area mean) | largest 2.43e-15 | pass |
| mounting region on the mesh | design C10L, 3 completed levels (3 valid) | faces present, mesh/geometry area ratio in (0.5, 2) | represented at every level | pass |
| mesh topology | design C10L, 3 completed levels (3 valid) | one face-connected region | one region at every level | pass |
| small deformation | design C10L, 3 completed levels (3 valid) | displacement <= 1% of size and rotation error <= 1e-3 (questionable up to 5% / 1e-2) | within_small_deformation_assumption | pass |
| geometry approximation (volume) | design C10L, 3 completed levels (3 valid) | reported; > 3% changes stiffness and mass noticeably | 7.48e-11% at the finest completed level | pass |
| discretisation (load-region displacement) | design C10L, 3 completed levels (3 valid) | last relative change <= 0.02 | monotone convergence with observed order 1.26: asymptotic range plausible; relative uncertainty 0.000298 | pass |
| force equilibrium | design C10W, 3 completed levels (3 valid) | \|sum of forces\| / force scale <= 1e-6 | largest 3.38e-11 | pass |
| moment balance | design C10W, 3 completed levels (3 valid) | \|sum of moments\| / moment scale <= 1e-6 | largest 1.62e-11 | pass |
| energy (Clapeyron) | design C10W, 3 completed levels (3 valid) | \|2 U / W - 1\| <= 1e-6 | largest \|2U/W - 1\| 6.62e-11 | pass |
| linear solver | design C10W, 3 completed levels (3 valid) | converged, true residual recomputed | converged at every level | pass |
| rigid-body modes | design C10W, 3 completed levels (3 valid) | setup_validate finds none before each solve | none (every solve passed validation) | pass |
| load line of action on the mesh | design C10W, 3 completed levels (3 valid) | shift <= max(0.5 h, 1% of the lever arm) and force preserved to 1e-6 | largest shift / criterion 1.1e-13 | pass |
| work-conjugate displacement = region mean | design C10W, 3 completed levels (3 valid) | relative difference <= 1e-9 (a uniform traction does work with the area mean) | largest 1.92e-15 | pass |
| mounting region on the mesh | design C10W, 3 completed levels (3 valid) | faces present, mesh/geometry area ratio in (0.5, 2) | represented at every level | pass |
| mesh topology | design C10W, 3 completed levels (3 valid) | one face-connected region | one region at every level | pass |
| small deformation | design C10W, 3 completed levels (3 valid) | displacement <= 1% of size and rotation error <= 1e-3 (questionable up to 5% / 1e-2) | within_small_deformation_assumption | pass |
| geometry approximation (volume) | design C10W, 3 completed levels (3 valid) | reported; > 3% changes stiffness and mass noticeably | 7.48e-11% at the finest completed level | pass |
| discretisation (load-region displacement) | design C10W, 3 completed levels (3 valid) | last relative change <= 0.02 | monotone convergence with observed order 1.33: asymptotic range plausible; relative uncertainty 0.00056 | pass |

A small solver residual is not a discretisation-error estimate: the discretisation rows come from the refinement study.

## Interpretation

- Lower predicted load-region displacement under the modeled conditions: design C10W (0.0099452 mm) against design C10L (0.15974 mm), a 93.8% difference; numerical uncertainty ±5.6e-06 mm and ±4.8e-05 mm at 0.625 mm elements.
- These are predictions of a static, small-strain, linear-elastic model for: material 'steel_nu03' (user_supplied values, source user); mounting region rigidly fixed; a static force spread uniformly over the load region. They are conditional on those inputs and are not measurements.
- Stress values are supporting information only. No strength margin or safety statement is made (see not evaluated).

## Important uncertainties

- Discretisation, design C10L: monotone convergence with observed order 1.26: asymptotic range plausible (Richardson extrapolation and grid convergence index over the three finest valid meshes (safety factor 1.25)).
- Geometry approximation, design C10L: the finest voxel mesh has 0.00% volume error against the STL.
- Discretisation, design C10W: monotone convergence with observed order 1.33: asymptotic range plausible (Richardson extrapolation and grid convergence index over the three finest valid meshes (safety factor 1.25)).
- Geometry approximation, design C10W: the finest voxel mesh has 0.00% volume error against the STL.
- Load distribution and location: a uniform traction over the load region is assumed; a payload bearing on part of it or with an offset centre of mass is not assessed.
- Manufacturing: not specified; the material is treated as homogeneous and isotropic.

## Assumptions

| Subject | Source | Assumption | Effect |
|---|---|---|---|
| manufacturing | default | The manufacturing process is not specified: the material values are used as given, as for a homogeneous, isotropic, defect-free part. | not assessed |
| mounting_idealization | user | The mounting region is idealised as rigidly fixed (all displacements zero): fixed root. No contact, slip, fastener or wall flexibility is modelled. | not assessed: no mounting alternative was declared |
| load_distribution | default | The load acts as a uniform traction over the load region, so its resultant passes through the region's centroid. A payload that bears on part of the region, or whose centre of mass is offset, loads the bracket differently. | not assessed |
| self_weight | default | The bracket's own weight is not applied: only the payload loads it. | not assessed; the weight of each design is reported |
| equivalence | user | The designs are not mounted and loaded equivalently: lever_arm of 'C10W' differs by 100 mm (tolerance 4); load_region_area of 'C10W' differs by 100 mm2 (tolerance 2); mounting_region_area of 'C10W' differs by 100 mm2 (tolerance 2). Is this difference part of the design change (accept it with the reason), or should the regions be redefined? Accepted: the cross-section or length is the difference being studied | accepted by the user; not assessed further |

Questions the user answered by accepting an assumption:

- `equivalence`: The designs are not mounted and loaded equivalently: lever_arm of 'C10W' differs by 100 mm (tolerance 4); load_region_area of 'C10W' differs by 100 mm2 (tolerance 2); mounting_region_area of 'C10W' differs by 100 mm2 (tolerance 2). Is this difference part of the design change (accept it with the reason), or should the regions be redefined? Reason given: the cross-section or length is the difference being studied

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

- Resolved study: `study.json` (SHA-256 of its content `d9e287ce7a2802d2bf031f30780f404d4f5a122d14cdc2a8213bb8f57f74f0c7`); original request `request.json`; every executed operation in `operations.jsonl`.
- Geometry of design C10L: stored copy `designs/C10L/inputs/part-7c44e8eebb5e.stl`, SHA-256 `7c44e8eebb5ec9051d1b9213165771f3da5011e38ad3bf0c6eb335b415fa30b2` (original `/private/tmp/claude-501/-Users-sebas-Downloads-Fluid-Sim/22500e7f-fd6d-4763-a5f1-db09bf6ffe3d/scratchpad/eval/independent/C10L.stl`).
- Geometry of design C10W: stored copy `designs/C10W/inputs/part-cff6b24aaabb.stl`, SHA-256 `cff6b24aaabb007fb2b82713a902199a60275453eeefab32cd1c5cdab82dce3b` (original `/private/tmp/claude-501/-Users-sebas-Downloads-Fluid-Sim/22500e7f-fd6d-4763-a5f1-db09bf6ffe3d/scratchpad/eval/independent/C10W.stl`).
- Software: NAVIER 0.3.0-rc1, operation contract 0.5.0, compiler Apple LLVM 17.0.0 (clang-1700.0.13.5); IEEE 754 double precision; no -ffast-math; no floating-point contraction.
- Platform: Darwin 24.6.0, arm64, 8 CPUs.
- MCP: study_replay {"directory": "/private/tmp/claude-501/-Users-sebas-Downloads-Fluid-Sim/22500e7f-fd6d-4763-a5f1-db09bf6ffe3d/scratchpad/eval/independent/t2_t3"} reruns the study from study.json and the stored geometry copies and compares every quantity
- CLI: navier-ctl --embedded study replay "/private/tmp/claude-501/-Users-sebas-Downloads-Fluid-Sim/22500e7f-fd6d-4763-a5f1-db09bf6ffe3d/scratchpad/eval/independent/t2_t3"
- Inspect a design: project_open {"path": "/private/tmp/claude-501/-Users-sebas-Downloads-Fluid-Sim/22500e7f-fd6d-4763-a5f1-db09bf6ffe3d/scratchpad/eval/independent/t2_t3/designs/<design>"}, then results_query / results_quantities with a run's job_id
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
