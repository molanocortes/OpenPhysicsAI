# Stage D — acceptance criteria, fixed before the first comparison

Written 2026-09-16 before `tools/orchtest.c` was run for the first time.

**Partitioned reference problem.** A 100 × 5 × 5 mm bar of two materials meeting at x = 50 mm (A: k = 40 W/(m·K),
B: k = 8 W/(m·K), both ρc_p = 4e6 J/(m³·K)), 400 K held at x = 0, convection 200 W/(m²·K) to 300 K at x = 100 mm, initially
300 K. Monolithic model: one mesh, shared interface nodes. Partitioned model: A and B as separate participants with
matching interface nodes, Dirichlet–Neumann: B holds the interface at A's temperature (the backward, relaxed link) and
returns the heat that left it through the interface, which A receives as nodal heat.

| # | Test | Criterion |
|---|---|---|
| D1 | fixed-step partitioned vs monolithic (backward Euler, Δt = 20 s, 0–2000 s) | at every stored time, max nodal difference ≤ 1e-6 K with a coupling tolerance of 1e-12 relative; for every step the heat leaving B equals the heat entering A to 1e-12 relative (conservative transfer) |
| D2 | adaptive partitioned vs adaptive monolithic (tolerance 1e-4 / 1e-3 K) | both runs' error against a fine fixed-step monolithic reference within a factor 3 of each other; coupling iterations and coupling residual reported separately from the temporal error |
| D3 | rollback after a coupling failure | with relaxation disabled (ω = 1, no Aitken) and a small iteration cap, at least one attempt is rejected for coupling; every accepted step commits both participants at the orchestrator's time (checked after every step); the committed solution still agrees with the monolithic reference to the D2 standard |
| D4 | graph analysis | a one-way chain reports no cycle; a two-participant loop reports one iterated cycle; a quasi-static participant inside a cycle is refused at creation |
| D5 | synchronisation of quasi-static participants | a participant without history is evaluated exactly at the initial time and at every stored time; one declared history-dependent is evaluated at the initial time and at every accepted step |
| D6 | the job path through the orchestrator | the thermal and thermomechanical jobs run through orchestrator participants; every existing thermal, transient, restart and contact regression passes unchanged, including bitwise restart equivalence; the run summary describes the coupling as one-way with the structure evaluated at stored times |

## Amendments (recorded after the first runs, 2026-09-16)

**D2: first run failed. Two defects fixed; criterion unchanged.**
First run: monolithic adaptive 527 steps, error 7.960e-02 K; partitioned 1536 steps, 2.284e-02 K (a factor of 3.5, so the
criterion failed). There were two defects:
1. Each participant's step-doubling trial held the imported interface data at its end-of-step value for both half steps.
   The half-step solutions therefore solved a different problem from the monolithic half steps, and the estimate did not
   measure the temporal error of the coupled system. Fix: interface fields are resolved per trial stage (full step, first
   half, second half; see `src/fem/thermal_participant.h`), and each solve uses the data of the matching solve of its
   partner. After this fix: 1257 steps, 4.265e-02 K. The criterion was met (a factor of 1.9), but the run still took
   2.4× the monolithic step count.
2. The orchestrator kept the largest estimate over all coupling sweeps of an attempt, including unconverged sweeps.
   Fix: it keeps only the converged sweep's estimate.

After both fixes: 527 steps, 7.960e-02 K, the monolithic step sequence. A regression guard (equal step counts) was added
after this run. It is not an original criterion.

**D3: criterion amended.** The scenario forces coupling failures by making plain Dirichlet–Neumann iteration diverge
for long steps (A: k = 4, ρc_p = 4e6; B: k = 40, ρc_p = 1e6; ω = 1, no Aitken, at most 40 iterations). The runs so far:

| run | accepted steps | coupling rejections | error vs fine reference | monolithic adaptive |
|---|---|---|---|---|
| first run | 4668 | 379 | 1.971e-03 K | 6.065e-02 K |
| after D2 fix 1 | 3224 | 271 | 7.761e-03 K | 6.065e-02 K |
| after D2 fix 2 | 4007 | 1357 | 5.944e-03 K | 6.065e-02 K (371 steps) |

The steps are limited by coupling, so the run is more accurate than the monolithic adaptive run and the literal
two-sided factor of 3 fails even though nothing is wrong. The two-sided factor also doesn't test what D3 is for:
checking that rejected attempts leave no trace. Amended criterion:
- (a) one-sided: the error against the fine reference is at most 3× that of the monolithic adaptive run;
- (b) replay: a monolithic model advanced over exactly the accepted steps of the partitioned run, with the same
  step-doubling candidate and stored after the same steps, agrees with the committed partitioned solution within
  1e-6 K at every stored time.

The test still prints whether the original criterion is met. It is not, for the reason above.
