# Stage B evidence — durable checkpoint and restart

Criteria were fixed before the first run (`stageB-criteria.md`). Raw output: `restartflow.log`. All runs through MCP
(`tools/restartflow.py`, 154 checks), one machine (Apple M2, 8 GB, macOS 15.7.3, Apple clang 17.0.0), the job worker's
default thread count.

**Study used by every comparison.** 100 × 10 × 10 mm bar, 1.25 mm voxel mesh (5 120 hex8 elements), verification alloy
(constant properties), one end held at 250 °C until 900 s then released, a volumetric heater on from 300 to 700 s,
convection 40 W/(m²·K) at the other end; adaptive backward Euler, relative tolerance 1e-5, absolute 0.001 K, 0–1500 s,
stored every 100 s. The uninterrupted reference took 1 534 accepted steps (13 s wall).

| # | Governing model | Scenario | Error metric | Observed | Criterion | Known limitations |
|---|---|---|---|---|---|---|
| B1 | transient thermal, schedules | `job_pause` at ≈ 36 % of the run (t = 538.959 s), `job_resume` in the same session | stored frames, energy terms, step counts, accepted-step history against the reference | **bitwise identical** | ≤ 1e-12 relative | bitwise identity requires the same build, machine and thread count |
| B2 | same | first periodic checkpoint (t = 150.987 s) published, then **SIGKILL** of `navier-server`; new server process; `job_status` → `interrupted`; mesh regenerated; `job_resume` | same | **bitwise identical**; before regeneration the resume was correctly refused as a mesh change | ≤ 1e-12 relative | a run killed before its first checkpoint cannot be resumed and must be restarted |
| B3 | one-way thermomechanical | pause at ≈ 40 %, resume, 0–600 s, frames every 150 s | temperature, displacement and von Mises frames | **bitwise identical** | ≤ 1e-12 relative | the structure is elastic and has no history; plasticity would add state that is not in the checkpoint yet |
| B4 | transient thermal | `job_cancel` at ≈ 50 % | policy, refusal of partial results, completion by resume | checkpoint written; partial-results policy stated; `results_query` refused naming `job_resume`; resumed run **bitwise identical** | as stated | — |
| B5 | checkpoint integrity | (a) the only checkpoint truncated by 13 bytes; (b) two checkpoints, the newest cut in half; (c) one payload bit flipped | detection and fallback | (a) refused, message names the truncation; (b) previous checkpoint (t = 402.970 s) used, fallback reported, completed run **bitwise identical**; (c) refused by the SHA-256 check | as stated | frames written after the fallback checkpoint are recomputed, which costs time but not correctness |
| B6 | compatibility | convection 40 → 41 W/(m²·K); mesh 1.25 → 2.5 mm; conductivity 20 → 21 W/(m·K); a tampered temporal tolerance in `spec.json` | the `mismatches` list | exactly `conditions`; includes `mesh`; exactly `materials`; exactly `settings` | the changed component named | a change that leaves the resolved solver inputs identical (e.g. renaming a selection) is not a mismatch, by design |
| B7 | transient thermal | `job_checkpoint` on a running job | job continues; checkpoint reported; completion | continued and completed; reported while running; restart files removed; **bitwise identical** | as stated | — |
| B8 | record consistency | all resumed runs | stored times strictly increasing and equal to the reference; budget not double-counted | holds in B1–B5, B7 | as stated | — |

## What a checkpoint contains

Accepted time and temperature; integrator controller (proposed step, previous accepted error, rejection counters) and
its settings; accepted energy budget; work counters; the stored-frame count and times; the accepted-step history; the
resume history; hashes of the resolved mesh, materials, conditions and physics settings; software version. Everything
else is rebuilt from the project and verified against those hashes. The phase state needs no separate record (equilibrium
liquid fraction of the temperature); schedules need none (functions of time). A future material model with memory would
have to add its history variables — the file records `history_variables` explicitly so that such a change is visible.

## Cost

A checkpoint of this study holds, by construction of the format, 52,488 bytes of temperatures (6 561 nodes), at most
36,816 bytes of accepted-step history (1 534 steps), 384 bytes of integrator state and a JSON header of a few kB —
under 95 kB — plus an fsync of the separate frame stream; the wall-clock default is one checkpoint per 60 s of run time. Checkpoints never change the step sequence, which
is why the checkpointed runs above are bitwise identical to the reference.
