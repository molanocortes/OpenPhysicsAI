# Numerical evidence

This document covers three things:
- what the comparison workflow checks;
- how it reports mesh refinement;
- what the reference cases and the independent CalculiX comparison showed.

Verification (the equations are solved correctly) is kept apart from validation (the equations describe reality).
**Only verification was done.** No result has been compared with a measurement.

## Checks on every analysis

| Check | Criterion | Why it is exact or bounded |
|---|---|---|
| force balance of applied loads, body forces and reactions | ≤ 1e-6 of the force scale | a converged discrete linear-elastic solution balances forces to round-off |
| moment balance about the mounting centroid | ≤ 1e-6 of the moment scale | rigid rotations lie in the null space of the stiffness matrix, so moments balance to round-off |
| Clapeyron: 2U / W | within 1e-6 of 1 | exact for linear statics |
| linear solver | converged; true residual recomputed | a small residual is **not** a discretisation-error estimate |
| rigid-body modes | none before solving | an unsupported motion is refused, never stabilised with springs |
| load application | force preserved to 1e-6; line of action within max(h/2, 1 % of the lever arm) of the load as specified on the geometry | the staircase mesh may move a surface load |
| work-conjugate displacement = region-mean displacement | ≤ 1e-9 relative | a uniform traction does work with the area mean of the displacement |
| mounting region on the mesh | faces present, area ratio in (0.5, 2) | a lost support is not a valid model |
| mesh topology and volume | one connected region; volume within 10 % | a mesh that lost a wall does not represent the design |
| small deformation | displacement ≤ 1 % of model size and rotation error θ²/2 ≤ 1e-3 (questionable up to 5 % / 1e-2) | beyond this a linear result does not describe the loaded part |

## Quantities

- **Load-region displacement:** the area-weighted mean over the region's mesh faces of u · d, with d the unit load
  direction. The displacement is bilinear on each voxel face, so each face contributes the mean of its corners
  exactly. This is the primary quantity by default. It is never the largest nodal displacement.
- **Stiffness:** k = |F|² / W = |F| / (work-conjugate displacement).
- **Mass:** density × the closed STL volume (the design), with the mesh volume beside it.
- **Reactions:** force and moment per support, about the mounting centroid.
- **Stress (supporting information only):**
  - raw Gauss-point and nodal-average maxima, and the nodal 99th percentile;
  - flags for peaks at supports and at sharp inside corners, which are singular;
  - no strength margin.

## Refinement: three separate conclusions

For each design and quantity over its valid meshes (element sizes descending), the record keeps three conclusions
apart. It never merges them into one "uncertainty".

**1. Changes between meshes and the ranking on the tested meshes.**
- Each change is stated with its definition: (q_fine − q_coarse) / |q_fine|, relative to the finer mesh.
- `ranking_by_mesh` lists the ranking of the designs on every mesh they all completed validly.
- `ranking_consistent_on_tested_meshes` says whether the ranking was the same on all of them. It is a fact about the
  tested meshes only.

**2. The convergence criterion.** It is met when the change between a design's two finest valid meshes is at most
`convergence_criterion` (2 % by default). A criterion that is not met is reported, in the statement and as a failed
row of the numerical-evidence table, and never dropped.

**3. A discretisation-error estimate, offered only when its conditions hold.**
- The estimate is a grid convergence index (Roache): E = Fs |f₃ − f₂| / (rᵖ − 1), with the observed order p of the
  three finest valid meshes.
- The conditions:
  - at least three valid meshes;
  - a constant refinement ratio of at least 1.3;
  - a monotone change;
  - an observed order in [0.5, 4];
  - the geometry represented identically on those meshes, with the voxel volume equal to the STL volume within 1e-6.
- The last condition fails for any staircase boundary, such as a chamfer or a slope. Its volume error changes with
  every mesh, so the meshes discretise different shapes, and Richardson extrapolation does not apply.
- Fs = 3. Fs = 1.25 only when four meshes give two observed orders that agree within 10 %, so that asymptotic
  behaviour is indicated rather than assumed.
- An estimate is not a bound and not a confidence interval, and it is never printed with "±".
- When a condition fails, `estimate_unavailable_reasons` says which one, with the observed values.

**Comparison outcome** (the two best designs, at the finest mesh every design completed validly):

| Outcome | When |
|---|---|
| `resolved` | both designs have estimates and the difference exceeds their sum |
| `ranking_consistent_on_tested_meshes` | at least one design has no estimate, the ranking was the same on every tested mesh, and the difference exceeds the sum of the designs' last changes between meshes. The size of the difference is not estimated beyond the tested meshes |
| `too_small_to_distinguish` | every design meets the convergence criterion, but the difference does not exceed the sum of the estimates (or of the last changes when an estimate is missing) |
| `not_resolved` / `more_refinement_required` | otherwise: a design with one valid mesh, a ranking that changed between meshes without estimates, or a difference within the estimates or changes of designs that have not converged; `more_refinement_required` when planned levels did not run |
| `cannot_establish` | no level is valid for every design, or the deformation is outside the small-deformation assumption |

The statement always ends with the convergence criterion: met by every design, or not met by the named designs with
their last change.

## Reference cases (tested on 2026-09-17 with 0.3.0-rc2, macOS 15.7.3, Apple M2)

Criteria were written before the first run: `REFERENCE_CRITERIA.md`.

### R1: cantilevers against Timoshenko beam theory, through the comparison workflow

Steel test values (E 200 GPa, ν 0.3); root face fixed; 10 N over the tip face; 2.5, 1.25 and 0.625 mm elements.

| Design | Beam theory (mm) | 2.5 / 1.25 / 0.625 mm (mm) | Finest vs theory | Observed order | Extrapolated |
|---|---|---|---|---|---|
| C10 (100 × 10 × 10) | 0.0201560 | 0.0199622 / 0.0199973 / 0.0200115 | −0.717 % | 1.30 | 0.0200211 (−0.67 %) |
| C20 (100 × 10 × 5) | 0.1603120 | 0.1588127 / 0.1590618 / 0.1591834 | −0.704 % | 1.04 | 0.1592991 (−0.63 %) |

- **Stiffness ratio:** C10/C20 = 7.95461 against 7.95356 (+0.013 %).
- **Mass:** exact to 1e-9.
- **Identities:** conjugate displacement = region mean, and balance and energy, all to round-off.
- **E scaling:** verified to 1e-6.
- **Outcome:** `resolved`; the box geometry is exact at every mesh, so both designs have estimates.
- **Reading:** about 0.7 % stiffer than beam theory. The independent evaluation tested the explanation (see below): the
  fully clamped root restrains the Poisson contraction of the section. It is not discretisation error and not a
  NAVIER error.

### R2: replay of the bracket example

The replay resolved to the same study hash. 24 of 24 quantities (4 per level, 3 levels, 2 designs) were bitwise
identical: outcome `reproduced`.

### R3: bracket example (no analytical reference)

- Every level of both designs passed its checks.
- Outcome `ranking_consistent_on_tested_meshes`: B's load-region displacement was lower than A's on all three meshes,
  60.5 %, 63.9 % and 65.5 % lower at 4, 2 and 1 mm. At 1 mm: 0.0087746 mm against 0.025399 mm.
- **A:** last change +0.85 % (criterion met); discretisation-error estimate 0.00061 mm (2.4 %, safety factor 3,
  observed order 1.04).
- **B:** last change −3.53 % relative to the finer mesh, which does **not meet** the 2 % criterion. No estimate:
  the chamfer's staircase volume error changes with the mesh (−3.57, −1.79, −0.89 %).
- rc1 reported `resolved` with "numerical uncertainty ±0.00025 / ±0.00032 mm". That estimate was not supported
  (`release/evaluation/EVALUATION_RECORD.md`, defect D1).
- **A fourth mesh (0.5 mm), run during the evaluation:** B displaced less on all four meshes (66.2 % at 0.5 mm). B's
  last change fell to −1.70 %, which meets the criterion.

## Independent comparison with CalculiX 2.23

Run during the independent evaluation, with criteria written before any CalculiX solve:
`release/evaluation/CRITERIA-independent.md`. Decks, output extracts and scripts are in `release/evaluation/independent/`.

- **The same assembled problem (X1)** exports NAVIER's mesh, supports and nodal loads for the C10 cantilever
  (1.25 / 0.625 mm) and brackets A and B (1 mm, plus B at 2 mm):
  - CalculiX `C3D8` against NAVIER's full integration agreed to 3.4e-7 or better in displacement, strain energy and
    element stress.
  - `C3D8I` against incompatible modes agreed to 3.7e-7 or better.
  - Reactions balanced; CalculiX printed no warnings.
  - CalculiX's direct solver crashed on the largest deck (B, 1 mm, `C3D8I`). CalculiX's iterative solver then
    reproduced NAVIER's displacement to 3.1e-7, but its own reactions balanced only to 2.9e-3.
- **An independently built cantilever (X2):** CalculiX meshes and loads were generated from a written specification.
  - The finest quadratic `C3D20R` mesh agreed with NAVIER's finest mesh within 0.0015 %.
  - An independent `C3D8I` mesh at 0.625 mm agreed within 1e-8.
- **The beam discrepancy (tests fixed in advance):**
  - The 0.7 % difference from Timoshenko theory disappears with ν = 0 (−0.002 %).
  - It halves when the length doubles (−0.359 %) and grows with the width (−1.318 %).
  - CalculiX with a root that may contract matches beam theory within 0.05 %.

**What this establishes:** agreement with a second code on these meshes and cases. Numerical evidence, not validation.

## Evaluation set

Ten cases with behaviour defined before running are listed in `EVALUATION_CASES.md`:
- a valid comparison;
- missing units;
- missing material;
- ambiguous mounting;
- an under-constrained part;
- a thin feature;
- large deformation;
- a printed part without process data;
- an inconclusive difference;
- a stress plot presented as proof of safety.

Three regression cases from the independent evaluation are added: a record that cannot be written, an unwritable
operations log, and the access-root limit. The scripted MCP client passes all of them (`REFERENCE_CASES.md`). That
shows the tools behave as specified. It does not show how an AI model or a person uses them.
