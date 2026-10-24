# Stage B — acceptance criteria, fixed before the first comparison

Written 2026-09-16 before `tools/restartflow.py` was run for the first time. Equivalence is judged against an
uninterrupted run of the identical specification in the same build on the same machine. The numerical tolerance is
1e-12 relative (per stored frame, max over nodes, relative to the largest absolute temperature; per budget term,
relative to the term). Bitwise identity is reported when observed but is not claimed across platforms, compilers or
thread counts.

| # | Test | Criterion |
|---|---|---|
| B1 | pause mid-run and resume in the same session | job reaches `paused` with a checkpoint at t > 0; resumed run completes; stored times identical; every stored temperature frame, the energy budget, the accepted/rejected step counts and the accepted-step time history within tolerance of the uninterrupted run |
| B2 | process killed (SIGKILL) after a checkpoint, resumed by a new server process | `job_status` in the new process reports `interrupted` with the checkpoint; `job_resume` completes the run; same equivalence as B1 |
| B3 | thermomechanical run paused and resumed | as B1, plus displacement and von Mises frames within tolerance |
| B4 | cancellation | a cancelled transient run writes a checkpoint, reports the partial-results policy, and `results_query` refuses it naming `job_resume`; resuming it completes the run with B1 equivalence |
| B5 | damaged checkpoints | a truncated newest checkpoint is detected and the previous valid one is used (and reported); with both damaged, `job_resume` fails with a message naming the damage; a single flipped payload byte is detected by the hash |
| B6 | mismatched specifications | a changed condition value, mesh size or material makes `job_resume` fail with PRECONDITION_FAILED and a `mismatches` entry naming exactly the changed component; a tampered physics setting in `spec.json` is reported as `settings` |
| B7 | checkpoint request on a running job | the job keeps running and completes; `job_status` reports the checkpoint while running; after completion the restart files are removed and `results.nvt` holds all frames |
| B8 | no duplicate or missing time records | after every resume, stored times are strictly increasing and identical to the uninterrupted run; the energy budget is not double-counted (B1 equivalence) |
