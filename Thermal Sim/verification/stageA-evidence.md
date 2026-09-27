# Stage A evidence — adaptive time integration

Criteria were fixed before the first run (`stageA-criteria.md`). Raw output: `tsteptest.log`, `transientflow.log`.
Machine: Apple M2, 8 GB, macOS 15.7.3, Apple clang 17.0.0, `-O2 -ffp-contract=off`. Every case below is a
verification against an analytical, manufactured or independently refined reference; none is a validation against
measurements.

| # | Governing model | Reference problem | Mesh / time settings | Error metric | Observed | Criterion | Known limitations |
|---|---|---|---|---|---|---|---|
| A1 | event schedule | synthetic events incl. coincident and out-of-range times | — | exact landing, merge, sliver rule | all exact | exact | tolerance 64 ulp of the largest time |
| A2 | PI controller | scripted error sequences | — | growth cap, no growth after rejection, MIN_STEP, rejection limit | all as specified | as specified | gains are the textbook PI values, not tuned per problem |
| A3 | 1-D conduction, lumped hex8 | lowest discrete eigenmode, exact semi-discrete solution `a·e^{−λt}` | 20 elements; Δt 20, 10, 5, 2.5 s | step-doubling estimate vs Δt; estimate / true local error | BE ratios 3.726, 3.858, **3.928**; CN 7.716, 7.855, **7.927**; estimate/true **0.994** (BE), **1.001** (CN) | BE ratios in [3.5, 4.5]; CN in [7, 9]; BE estimate within ×2 of true | smooth data only; for non-smooth data (a suddenly applied temperature) Crank–Nicolson's estimate is not asymptotic in the first steps |
| A4 | as A3 | sudden 100 K surface heating, insulated far end | 20 elements, 0–2000 s, 5 stored times | max nodal error over stored times vs 3-run Richardson reference (reference error 6.2e-5 K) | 1.23 / 0.547 / 0.210 / 0.068 K at tol 1e-2 … 1e-5 | monotone, ≥ ×10 over three decades | global error is 0.6–34× the per-step allowance and grows late in the run; scales as tol^0.42 |
| A5 | enthalpy method, equilibrium liquid fraction | one-phase Stefan problem | 160 elements, mushy 0.5 K, 0–1000 s | front position and molten volume vs 3-run extrapolated reference (reference error 3e-4 %) | front **−0.004 %**, molten **+0.003 %** (T+H); T-only: −0.010 %, +0.012 %, latent error ×3.9 | ≤ 2 %; T-only not better than T+H | the T+H error is 12× the reference error: resolved, but not by a wide margin |
| A6 | conduction + scheduled source | insulated uniform block, source on 1.37–4.21 s | 2×2×2 elements; fixed Δt 1 s and adaptive | deposited energy; final temperature | exact to round-off (22.72 J; 300.710000000000 K) in both modes | ≤ 1e-12 relative; ≤ 1e-9 K | uniform problem: exact for backward Euler, so it tests landing and accounting, not accuracy |
| A7 | conduction + source + convection | half-heated bar, oversized start | 20 elements, 0–600 s | accepted source energy vs `Q V t` | exact (1500 J) with 4 rejections; closure 3.2e-14; enthalpy mismatch 0 (1.6e-16 in the log regenerated at Stage D: the per-body accounting added in Stage C changed the order of summation, not the step sequence) | ≤ 1e-12; ≤ 1e-9 | — |
| A8 | as A4 | first step = whole interval | tol 1e-3 | max error vs reference | 0.568 K after 2 rejections (sensible start 0.547 K) | within ×3 | — |
| A9 | as A4 | min_time_step 100 s at tol 1e-5 | — | failure class and state | `MIN_STEP` at t = 0 naming the error measure; state finite, budget closed | `MIN_STEP`, named, finite | — |
| A10 | enthalpy method | Stefan problem 0–200 s (frequent rejections) | 160 elements, tol 1e-3 | bitwise comparison of accepted state around every rejected trial; replay in fresh objects | **141** rejected trials, **0** differences; **8/8** replays bit-identical | all identical | bitwise identity holds on one machine and thread count; not claimed across platforms |
| A11 | as A4 | work at equal accuracy | fixed Δt 50 … 0.04 s vs adaptive | solves, assemblies, Picard, CG, wall time | adaptive needs **22–32× fewer solves** than the cheapest equally accurate fixed step (e.g. 309 vs 10 000 solves at 0.55 K) | reported (expected: fewer) | a multi-scale problem favours adaptivity; a smooth single-time-scale problem would show much less gain |
| MCP | full application path | scheduled heater 5–30 s, convection, explicit output times | 40×10×10 mm block, 2.5 mm | stored times, energy, wiring, failure details | stored times exact; energy exact (200 J); tighter tolerance → 14 → 67 steps; `MIN_STEP` with last accepted time, checkpoint flag and recovery options | as listed in `transientflow.py` (77 checks) | scripted MCP client, not an AI model |

## Cost

Step doubling costs three nonlinear solves per attempt. Against the equally accurate fixed step on the multi-scale
problem that is recovered many times over (A11). The estimate itself (both norms over every node and Gauss point) was
measured through MCP on a 100 mm bar of the 316L demonstration material (temperature-dependent properties and latent
heat, 600 s, default tolerances): 2.75 % of solve time at 1 025 nodes (239 steps) and 3.23 % at 12 221 nodes (295 steps).
Every summary reports both figures under `time_integration.work`.
