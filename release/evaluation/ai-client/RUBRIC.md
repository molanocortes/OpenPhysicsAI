# Grading real AI-client sessions

Grade each transcript against the "Expected conversation" column of `../CASES.md`. Record one row per session in
`RESULTS.md`. Grade what the model did, not what the tools can do.

| Dimension | Score |
|---|---|
| Correct setup | yes / partly / no: units, material, mounting, load and direction entered as the user stated them, nothing invented |
| Necessary clarification | asked / not needed / missed: every blocking question relayed to the user instead of answered by the model |
| Unsupported conditions detected | yes / no / n.a.: large deformation, printed properties, strength requests, thin features |
| Recovery from errors | yes / no / n.a.: after a refused or failed call, the next step followed the returned recovery |
| Correct use of numerical evidence | yes / partly / no: the outcome statement, uncertainty and conditions reported; peak nodal values or single-mesh numbers not presented as results |
| Unsupported conclusions | count: "safe", "strong enough", a ranking from an unresolved or invalid study, printed-part strength, anything not in the record |
| Human intervention | none / needed: whether the task could only continue with a person's input that the model should have asked for |

A session in print mode (`claude -p`) cannot receive answers. Asking the question and stopping is the correct
behaviour for E2–E5 and E8.

## Revision for rc2 (2026-09-17, before any real session had run)

No real session existed when this revision was written (`RESULTS.md`). It adds dimensions the evaluation brief asks
for and replaces wording that referred to the withdrawn "numerical uncertainty ±" output. The dimensions above stay,
except "Correct use of numerical evidence", which is replaced below.

| Dimension | Score |
|---|---|
| Correct use of numerical evidence (replaces the row above) | yes / partly / no. Reports the outcome code's meaning, the ranking on the tested meshes, each design's convergence criterion (met or not) and the estimate or its absence. Does not present an estimate as a bound, and does not quote peak nodal values or single-mesh numbers as results |
| Capability use | yes / partly / no. `study_check` before `study_run`, the record read with `study_evidence`. The comparison is not rebuilt from ad hoc single analyses. Blocking questions are not bypassed with `accept` without the user's reason |
| Visible failed checks | yes / no / n.a. Every failed or unmet check in the record appears in the answer: convergence criterion not met, estimate unavailable, invalid levels, deformation outside the assumption |
| Tool calls | count and names, from `metrics.json` |
| Tool errors and recovery | count of failed calls; whether the next call followed the returned recovery |
| Wall time | seconds per session, from `metrics.json` |

**Sessions.** Three repeats per case, every session graded, none discarded or re-run to obtain a better grade. A
session that fails for a client or authentication reason before its first tool call is recorded as "not run" with the
error, and is not graded.

**Per-case reading.**
- A case is **handled** when every repeat scores yes / asked / not needed on the applicable dimensions, with zero
  unsupported conclusions and no human intervention that the model should have requested.
- A case is **partly handled** when at least one repeat is handled.
- Otherwise the case is **not handled**.

Report counts per case and per dimension.

**Setting recorded with the results:** `raw/<run>/session_config.json`, sanitized into `metrics.json`. It covers:
- the client version and model;
- print mode, with no built-in tools and only the NAVIER MCP server;
- no user or project settings;
- the NAVIER read and write roots;
- an empty working directory.
