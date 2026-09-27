# Stage D evidence: the shared numerical orchestrator

Criteria were fixed before the first run (`stageD-criteria.md`). D2 failed on the first runs and D3's criterion was
amended; both are recorded in its Amendments section. Raw output: `orchtest.log` (`tools/orchtest.c`) and the MCP
suites `thermflow.log`, `transientflow.log`, `restartflow.log`, `contactflow.log`. One machine (Apple M2, 8 GB, macOS
15.7.3, Apple clang 17.0.0).

**Partitioned reference problem (D1–D3).** A 100 × 5 × 5 mm bar of two materials meeting at x = 50 mm. 400 K is held at
x = 0 and the end at x = 100 mm is cooled by convection at 200 W/(m²·K) to 300 K; initially 300 K. The mesh is 20 hex8
elements along x (5 mm) × 1 × 1. The monolithic model is one mesh with shared interface nodes. The partitioned model is
A and B as separate thermal participants with four matching interface nodes, coupled Dirichlet–Neumann: B holds the
interface at A's temperature (the cut, relaxed link) and returns the reaction energy of the held nodes, which A receives
as nodal heat. Backward Euler throughout, Picard tolerance 1e-12, PCG tolerance 1e-14.

| # | Governing model | Reference | Mesh / time | Metric | Observed | Criterion | Limitations |
|---|---|---|---|---|---|---|---|
| D1 | conduction, DN partitioned | monolithic model, same mesh and steps | 5 mm; Δt = 20 s, 0–2000 s, coupling tolerance 1e-12 rel. | max nodal difference at the 10 stored times; per-step heat out of B vs into A | **3.524e-12 K**; heat mismatch **0** (identity transfer); 3.00 coupling iterations per step (Aitken) | ≤ 1e-6 K; ≤ 1e-12 rel. | matching interface nodes only; linear materials |
| D2 | same, adaptive | adaptive monolithic run at the same tolerance; both against a fixed-step monolithic reference (Δt = 0.25 s, 8000 steps) | tolerance 1e-4 rel. / 1e-3 K | error against the reference, both runs | partitioned **7.960e-02 K** in **527** steps, monolithic **7.960e-02 K** in **527** steps: the same step sequence. 3 error rejections, 0 coupling rejections, 3053 coupling iterations; largest accepted temporal error 0.948 and largest final coupling change ≤ 1, reported separately | within a factor 3 of each other | global error is 7× the per-step allowance: per-step control, as documented in §3.2.5 |
| D3 | same, plain DN (ω = 1, no Aitken, ≤ 40 iterations); A: k = 4, ρc_p = 4e6; B: k = 40, ρc_p = 1e6, which diverges for long steps | (a) adaptive monolithic run; (b) monolithic replay of the accepted step sequence | tolerance 1e-4 / 1e-3 K | (a) error vs reference; (b) max difference at stored times; participant times after every step | **1357 coupling rejections**, 4007 accepted steps; (a) 5.944e-03 K vs 6.065e-02 K (monolithic, 371 steps); (b) **6.787e-10 K**; both participants at the orchestrator's time after every step | amended: (a) ≤ 3× monolithic; (b) ≤ 1e-6 K. The original two-sided criterion is not met: coupling-limited steps make the run more accurate | the controller does not remember the step size at which coupling failed, so it grows into failure repeatedly: 1357 coupling rejections for 4007 accepted steps, each costing up to 40 sweeps |
| D4 | graph analysis | by construction | — | plan description; refusal | chain described "one-way"; loop described as a strongly coupled feedback cycle; a quasi-static participant in a cycle refused with a message naming it | as stated | at most 8 participants and 16 couplings |
| D5 | synchronisation | by construction | fixed Δt = 30 s, 10 stored times every 200 s (off the grid, so 73 accepted steps) | evaluation counts | without history: **11** = initial + 10 stored times; history-dependent: **74** = initial + 73 steps | as stated | — |
| D6 | the job path | the direct integrator path (`tint_step`) and every existing MCP regression | fixed and adaptive | stored frames, step counts, attempt/rejection counts, recent-attempt records; suite results | orchestrated single-participant runs **bitwise identical** to `tint_step` in fixed and adaptive mode, including the integrator's bookkeeping. thermflow 95/95 (5 new checks: coupling capability, participants, one-way declaration, structural results at every stored time), transientflow 81/81, restartflow 154/154 (bitwise resume, including SIGKILL and a new process), contactflow 104/104 | unchanged regressions; summary declares one-way coupling with the structure at stored times | one transient participant: the job path exercises commit, rollback bookkeeping, checkpoints and quasi-static synchronisation, not cycles |

## Defects found by the criteria

1. **Coupled data held at end-of-step values inside step doubling.** The partitioned trial's half steps saw the partner's
   end-of-step interface data, so the estimate described a different problem. Fixed by resolving interface fields per
   trial stage (`src/fem/thermal_participant.h`).
2. **Estimates of unconverged sweeps kept.** The orchestrator kept the largest temporal estimate over all sweeps.
   Fixed: only the converged sweep counts.

The first run took 1536 steps against 527; after fix 1 it took 1257; after fix 2 it took 527. Neither defect produced a
*less* accurate answer on this problem: both made the controller over-conservative. They would equally have been able
to hide an error on a problem where the inconsistent estimate is smaller than the true one. The criteria found them
because they compared step sequences and errors with a monolithic model, not only final temperatures.

## Cost

| run | wall time | work |
|---|---|---|
| D1 monolithic, 100 fixed steps | 0.002 s | 100 solves |
| D1 partitioned | 0.005 s | 300 coupling sweeps × 2 participants |
| D2 monolithic adaptive | 0.023 s | 1590 solves |
| D2 partitioned adaptive | 0.123 s | 3053 sweeps × 2 participants × 3 solves |

Partitioning costs 5× here because every sweep repeats all three step-doubling solves of both participants.

**Orchestration overhead on a single participant** (the job path's configuration), best of 5 on the same model, as
recorded in `orchtest.log`: fixed, 100 steps: `tint_step` 0.0028 s, orchestrated 0.0026 s; adaptive, 527 steps / 1590
solves: 0.0234 s and 0.0235 s. An earlier run gave 0.0019 / 0.0019 s and 0.0242 / 0.0242 s. The difference is inside
the run-to-run scatter on a model whose solves take about 15 µs each, so the overhead is negligible for any real mesh.

**Added after the first runs (not original criteria):** equal step counts in D2 (a regression guard for the
stage-resolved fields), the monolithic replay in D3 (the amended criterion), fixed-mode bitwise identity and identical
integrator bookkeeping in D6, and estimating participants driven at fixed steps. The last uses the three-block layout
with derived blocks and gives the D1 answer to 3.8e-12 K with exactly conservative transfer.

## What Stage D does not establish

* No strongly coupled study is reachable through MCP. Feedback cycles are verified in the core only.
* Only matching interface meshes (identity transfer) have been exercised. Conservative transfer between non-matching
  meshes is not implemented.
* The thermomechanical job remains one-way by design. Its declaration is tested, but there is no two-way physics to
  verify.
