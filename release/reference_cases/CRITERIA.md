# Reference cases: acceptance criteria

Written on 2026-09-16 at 21:58 CEST, before any of these cases was run through the comparison workflow. Changes after
a first run are appended under "Amendments", with the observed values and the reason. They never replace the original
text.

All cases run through the public operations (`study_run` / `study_replay` over MCP stdio, `tools/studyflow.py`), not
through internal C calls.

## R1: analytical benchmark, two cantilevers through the comparison workflow

**Model.** Designs C10 (100 × 10 × 10 mm, L/h = 10) and C20 (100 × 10 × 5 mm, L/h = 20, thin in the load direction).
Steel test values: E = 200 GPa, ν = 0.3, ρ = 7850 kg/m³, status user_supplied. Mounting: the root face x = 0, fixed.
Load: a 10 N force along −z, uniform over the tip face x = 100 mm. Element sizes: 2.5, 1.25 and 0.625 mm. Every face lies
on the voxel grid at every size.

**Reference.** Timoshenko beam deflection δ = F L³ / (3 E I) + F L / (κ G A), with κ = 5/6 and G = E / (2 (1 + ν)):
C10 0.0201560 mm, C20 0.1603120 mm. The ratio of their stiffnesses is 7.9536.

| # | Criterion | Why this bound |
|---|---|---|
| R1a | the study completes; every level of both designs is valid (all checks pass) | nothing in this case should trip a check |
| R1b | finest-mesh load-region displacement within 2 % of the reference, for both designs | femtest measured −0.96 % for L/h = 10 at 40 × 4 × 4 incompatible-mode elements. A fully clamped 3D root restrains warping and Poisson contraction, which beam theory ignores, at about the 1 % level for L/h ≥ 10 |
| R1c | stiffness ratio C10 / C20 within 2 % of 7.9536 | same modelling differences, partly cancelling |
| R1d | comparison outcome `resolved`, ranking C10 before C20 | the displacements differ by a factor of about 8 |
| R1e | geometry mass equal to ρ V to 1e-9, and mesh mass equal to it to 1e-9 | the boxes are represented exactly by the voxel grid |
| R1f | work-conjugate displacement equal to the region mean to 1e-9; force and moment balance and energy to 1e-6 | exact identities of the discrete problem |
| R1g | Young's-modulus scaling (sensitivity factor 1.1) verified to 1e-6 | displacements are exactly proportional to 1/E |
| R1h | load-region displacement either converges monotonically with observed order in [0.5, 4], or changes by at most 2 % between the two finest meshes, for both designs | smooth problem apart from the clamped root corners |

## R2: replay

The bracket example (designs A and B, 4 / 2 / 1 mm), replayed with `study_replay` on the same build and machine,
resolves to the same study hash. Every quantity at every level is bitwise identical (outcome `reproduced`,
`bitwise_identical` equal to `compared`).

## R3: bracket example (no analytical reference)

Designs A and B of `examples/bracket_comparison`: the study completes and every level passes its checks. The outcome is
reported with a numerical uncertainty for each design. No accuracy claim is made beyond the refinement evidence.

## Amendments

No acceptance criterion was changed. One clarification of the R1 set-up, recorded on 2026-09-16 at 22:10 CEST after
the first attempt: the check asked whether the designs are held and loaded equivalently, because the root and tip
faces of C20 (50 mm²) are half those of C10 (100 mm²). The cross-section is the difference being compared, so the R1
definition accepts that question with this reason. The criteria R1a–R1h were then evaluated unchanged:

| # | Observed | Result |
|---|---|---|
| R1a | complete; every level of both designs valid | met |
| R1b | C10 −0.717 %, C20 −0.704 % (finest mesh against beam theory) | met |
| R1c | ratio 7.95461 against 7.95356 (+0.013 %) | met |
| R1d | `resolved`, C10 before C20 | met |
| R1e | geometry and mesh mass equal to ρV to 1e-9 | met |
| R1f | identities to round-off (largest 1e-14 for conjugate = mean; balance and energy about 1e-11) | met |
| R1g | Young's-modulus scaling verified for both designs | met |
| R1h | monotone with observed order 1.30 (C10) and 1.04 (C20) | met |
| R2 | same study hash; 24 of 24 quantities bitwise identical | met |
| R3 | complete, every level valid, uncertainty reported (A 1.0 %, B 3.7 %) | met |

## Amendment for rc2 (2026-09-17)

R3's criterion text, "reported with a numerical uncertainty for each design", is revised. The independent evaluation
found that the reported values were a grid convergence index with an unsupported safety factor, applied to design B
whose geometry changes with the mesh (defect D1 in `release/evaluation/EVALUATION_RECORD.md`).

**Revised R3:** the study completes; every level passes its checks; for each design the record reports:
- the change between meshes with its definition;
- whether the convergence criterion is met;
- a discretisation-error estimate with the conditions it satisfies, or the reasons none is offered.

Observed with rc2: complete, every level valid. A meets the criterion (+0.85 %) and has an estimate of 2.4 % (safety
factor 3). B does not meet it (−3.53 %) and has no estimate (volume error −3.57, −1.79, −0.89 %). Result: met under the
revised text. Under the original text, rc2 reports no "numerical uncertainty" for B by design.

R1d, `resolved` with C10 before C20, is unchanged and still met with rc2: the box geometry is exact at every mesh, so
both designs have estimates.
