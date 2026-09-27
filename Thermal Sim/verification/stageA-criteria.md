# Stage A — acceptance criteria, fixed before the first comparison

Written 2026-09-16 before `tools/tsteptest.c` was run for the first time. If a measurement fails one of these, the
cause is investigated; the criterion is not loosened to make it pass. Any later change to a criterion is recorded here
with its reason.

| # | Test | Criterion |
|---|---|---|
| A1 | Event schedule | steps land on every event exactly (the accepted time equals the stored event time bit for bit); no accepted step interval strictly contains an event; coincident events merge; no step shorter than 0.25 x its proposal is left before an event except when forced by the schedule |
| A2 | Controller | first acceptance uses the elementary controller; growth never exceeds max_growth; after a rejection the next factor is <= 1; a step already at dt_min that is rejected fails with MIN_STEP; the consecutive-rejection limit fails with TOO_MANY_REJECTIONS |
| A3 | Estimator order (smooth decaying sine mode) | the step-doubling estimate from a fixed state scales as dt^2 for backward Euler (ratio of successive halvings within [3.5, 4.5]) and dt^3 for Crank-Nicolson (within [7, 9]); for backward Euler at the smallest step the estimate is within a factor 2 of the true local error of the accepted two-half-step solution |
| A4 | Tolerance convergence (sudden surface heating) | the achieved error (max over output times, against a Richardson-extrapolated fine reference on the same mesh) decreases monotonically as the tolerance tightens over three decades, and falls by at least a factor 10 overall |
| A5 | Phase change against a refined reference | with temperature and enthalpy control at the moderate tolerance: melt-front position within 2 % and molten volume within 2 % of the fine fixed-step reference. Temperature-only control at the same tolerance must not have a smaller latent-energy error than temperature + enthalpy control |
| A6 | Discontinuous heating | switch-on and switch-off times are landed on exactly in fixed and adaptive mode; deposited energy equals Q V (t_off - t_on) to 1e-12 relative; final uniform temperature within 1e-9 K of the exact value |
| A7 | Rejection without double counting | at least one rejection occurs; the accepted source energy equals Q V t_end to 1e-12 relative; the accepted budget closes (stored = supplied) to 1e-9 and the independent enthalpy agrees to 1e-9 |
| A8 | Oversized initial step | a first step equal to the whole interval is rejected at least once, the run finishes, and its achieved error is within a factor 3 of the same tolerance started from a sensible step |
| A9 | Minimum step insufficient | fails with MIN_STEP, names the time and the error measure, and leaves a finite accepted state whose budget still closes |
| A10 | Identical accepted state around a rejection | after a rejected trial the accepted time, temperature and budget are bit-identical to their values before it, and the model arrays are unchanged; a second integrator restored from the pre-step state produces a bit-identical accepted step |
| A11 | Work at equal accuracy | on the sudden-heating problem, report assemblies, Picard and CG iterations, solves and wall time for adaptive runs and for the fixed step that reaches at least the same accuracy. Expected (from theory, not required to pass): adaptive needs fewer solves |
