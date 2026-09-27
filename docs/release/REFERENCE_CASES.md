# Reproducing the reference and evaluation cases

## Commands

From the extracted release (Python 3 required; the tests start `bin/navier-mcp` themselves):

```bash
NAVIER_BIN="$PWD/bin" python3 share/navier/tests/studyflow.py            # everything, about three minutes on an M2
NAVIER_BIN="$PWD/bin" python3 share/navier/tests/studyflow.py --only R1  # one case
NAVIER_BIN="$PWD/bin" python3 share/navier/tests/studyflow.py --results results.json
bin/navier-ctl doctor                                                     # includes a smaller reference solve
```

From the source tree: `make test` runs all suites, including `tools/studyflow.py` and `navier-ctl doctor`.

The criteria (`REFERENCE_CRITERIA.md`) and the expected behaviour of the evaluation cases (`EVALUATION_CASES.md`) were
written before the first run. Changes to criteria are appended there with the observed values.

## Results of the scripted MCP client (0.3.0-rc2, 2026-09-17, macOS 15.7.3, Apple M2, 8 GB)

| Case | What it checks | Observed |
|---|---|---|
| R1 | cantilevers against beam theory | C10 −0.717 %, C20 −0.704 % at the finest mesh (criterion 2 %); stiffness ratio +0.013 %; mass exact; identities to round-off; E scaling verified; `resolved` (exact box geometry, estimates for both) |
| R2 | replay | same study hash; 24 of 24 quantities bitwise identical |
| R3 / E1 | valid bracket comparison | ready, equivalent, 2 previews; `ranking_consistent_on_tested_meshes`: B before A on all 3 meshes, 60.5–65.5 % lower displacement; A meets the 2 % criterion with an estimate of 2.4 % (safety factor 3); B does not meet it (−3.53 %) and has no estimate (staircase chamfer); every level valid; files written. 34 s |
| E2 | missing units | `needs_input`, blocking question `B.units`, candidate sizes reported; `study_run` refused with the question |
| E3 | missing material | blocking question `study.material` |
| E4 | ambiguous mounting (`facing -z`) | acceptable blocking questions `A.mounting_region.ambiguous` and `B.mounting_region.ambiguous` (faces in planes 52 mm apart); also `equivalence`, because those regions differ in area between A and B |
| E5 | frictionless mounting only | blocking, non-acceptable `A.mounting_underconstrained` (and B) naming the free rigid-body motions |
| E6 | thin walls at 8 / 6 mm | `THIN_FEATURE_MAY_VANISH` warning; every level invalid (volume errors of 17–53 %, a lost mounting region); `cannot_establish` with the reasons |
| E7 | soft material, 60 kg | A classified outside the small-deformation assumption at every level (B questionable); `cannot_establish`; no ranking |
| E8 | FFF part without process data | acceptable blocking question `manufacturing.printed_properties`; interlayer failure and printed anisotropy not evaluated |
| E9 | A against A with a 4 mm tip chamfer | `too_small_to_distinguish`: a 0.07 % difference, not more than the sum of the last changes between meshes (0.00043 mm); the chamfered design has no estimate; both meet the criterion |
| E10 | stress plot as proof of safety | unsupported request `strength_or_safety`; the record states that no strength or safety statement is made; no safety claim |
| E11 | evidence record cannot be written | job `failed` with `IO_ERROR` naming `evidence.json` and the recovery; `report.md` written; the comparison kept in the error details |
| E12 | operations log cannot be opened | study not started; `IO_ERROR` at setup in the record |
| E13 | access-root limit | 24 roots inside the home folder accepted; 24 unrelated roots refused with a message naming the limit of 16 |

The outcome expectations of E1 and R3 were revised for rc2, with the reason, in `EVALUATION_CASES.md` (amendments).
These results show that the tools behave as specified for a scripted client. They say nothing about a model or a
person using them. Real AI-client sessions were run separately and graded (`LIMITATIONS.md`, `AI_CLIENTS.md`); **no
external user has tried the workflow.**

## The independent CalculiX comparison

Results and criteria: `EVIDENCE.md`. To repeat the same-problem check on a run of your own (NumPy and CalculiX needed):

```bash
python3 share/navier/tools/ccx_crosscheck.py export <study>/designs/A/runs/<job_id> ccx_A
(cd ccx_A && ccx model)
python3 share/navier/tools/ccx_crosscheck.py compare <study>/designs/A/runs/<job_id> ccx_A
```

Use a run whose `results.nvr` was retained (a refinement level), without self-weight. CalculiX prints 7 significant
digits, so agreement better than about 1e-7 cannot be read from its output.
