# Release gap assessment: bracket comparison workflow

Baseline inspected on 2026-09-16 at 21:06 CEST. No version control; source hashes are in `BASELINE-2026-09-16.sha256`
(manifest sha256 `9626e15d…`) and a snapshot is in `.recovery/2026-09-16T2106-pre-release.tar.gz` (sha256 `a698e9e7…`).
Other Claude Code processes were running, but nothing in the tree had changed in the previous four hours apart from
the thermal-stage files of this session.

Legend: ✔ yes, ◐ partial, ✘ no. "Verified" means a numerical test with a stated reference exists and passes.

| Workflow step | Implemented | Verified | Operation layer | MCP | Documented accurately | Gap for this release |
|---|---|---|---|---|---|---|
| Import STL, units required, diagnostics (bounds, components, defects) | ✔ | ✔ surftest, opstest | ✔ | ✔ | ✔ | none |
| Orientation / placement | ✔ | ✔ opstest | ✔ | ✔ | ◐ AM-centric "build frame" wording | explain the frame for non-AM parts |
| Surface patches, selections, previews, coverage on the mesh | ✔ | ◐ amflow (no refinement coverage) | ✔ | ✔ | ✔ | no coverage criterion across refinement; no equivalence check between designs |
| Materials with provenance | ✔ | ✔ | ✔ | ✔ | ✔ | provenance lacks *database* / *measured*; library is demonstration only |
| Manufacturing assumptions | ✘ | – | ✘ | ✘ | – | not represented; printed-part requests cannot be qualified |
| Supports (fixed, displacement, frictionless) | ✔ | ✔ femtest | ✔ | ✔ | ✔ | none |
| Loads (force, pressure, traction, gravity) | ✔ | ✔ resultant | ✔ | ✔ | ✔ | payload **mass** → force with recorded g ✘; **moment** preservation not checked |
| Voxel hex8 mesh with approximation errors | ✔ | ✔ meshtest | ✔ | ✔ | ✔ | thin-feature loss detected only indirectly |
| Linear static solve (incompatible modes) | ✔ | ✔ cantilever −0.96 %, convergence | ✔ | ✔ | ✔ | none |
| Checks: equilibrium (force), energy, rigid-body modes, residual | ✔ | ✔ | ✔ | ✔ | ✔ | **moment balance** ✘, **large-deformation** indicator ✘ |
| Quantities: max displacement, stress peaks, reaction forces | ✔ | ◐ | ✔ | ✔ | ✔ | **region-mean displacement**, **stiffness**, **mass**, **reaction moment** ✘ |
| Mesh refinement study | ✘ (manual) | – | ✘ | ✘ | – | no controlled sequence, no change/extrapolation analysis |
| Two-design comparison under equivalent conditions | ✘ | – | ✘ | ✘ | – | missing entirely |
| Study specification (question, assumptions, unresolved inputs, limits) | ◐ project.json + immutable run spec | – | ◐ | ◐ | ◐ | no comparison-study document |
| Evidence record (JSON + readable report) | ✘ | – | ✘ | ✘ | – | missing |
| Replay and reproduce | ◐ reopen checks hashes | ◐ | ◐ | ◐ | ✔ | no quantity-level replay comparison |
| Analytical benchmark through the workflow | ◐ C-level only | ✔ | ✘ | ✘ | ✔ | not exercised through MCP |
| External solver comparison | ✘ | – | – | – | – | no CalculiX, scikit-fem or SciPy on this machine |
| Failure recovery (stage, class, recovery, partial results) | ◐ stable codes and hints | – | ◐ | ◐ | ◐ | no failure class or partial-result report for multi-run workflows |
| Conversation-level evaluation set | ✘ | – | – | – | – | missing |
| Real AI client | ✘ (login had expired) | – | – | – | ✔ says unverified | retry with the installed `claude` CLI |
| Packaged headless runtime, diagnostic, clean install | ✘ | – | – | – | – | missing; docs contain developer home paths |
| CLI route to the same workflow | ◐ navier-server + navier-ctl call | – | ✔ | – | ✔ | no single-command embedded route |

Misleading claims found on the release path: `capabilities_get.implementation_stage` says transient thermal is not
available (it is); `docs/mcp-clients.md` hard-codes an absolute home-folder path; `docs/analysis.md` says transient jobs do not
checkpoint (they do).

## Shortest credible path

1. Factor mesh generation out of its operation handler so a job can mesh private copies of a project at several sizes.
2. Add to every static result: mass, reaction moments, moment balance, large-deformation indicators; add
   `results_quantities` for region-mean displacement along a direction, work-conjugate displacement and stiffness.
3. A comparison-study document (`study.json`) and operations `study_check`, `study_run`, `study_evidence`,
   `study_replay`. One background job runs every design at every mesh size plus the declared sensitivities on private
   projects. It saves each design as an ordinary project for inspection, and writes `evidence.json` and `report.md`.
4. Consistency checks: region coverage and centroid on each mesh, applied force and moment against the intended
   load, and equivalence of mounting and load between designs.
5. Evidence: predefined criteria, a cantilever benchmark through the same study operations, and a CalculiX deck
   exported from the resolved model (the run stays pending until a solver is available).
6. Scripted MCP evaluation set (10 cases), real `claude -p` sessions where authentication allows, CLI route
   (`navier-ctl --embedded`), `navier-ctl doctor`, a relocatable package, and a clean-directory install test.

## Status at the end of the release work (2026-09-16)

| Gap | Now |
|---|---|
| payload mass, load moment preservation | payload mass × recorded g in study definitions; line-of-action check on every level |
| moment balance, deformation indicators, region-mean displacement, stiffness, mass, reaction moments | in every static summary and in `results_quantities` |
| refinement study, comparison, study specification, evidence record, replay | `study_check`, `study_run`, `study_evidence`, `study_replay`; `evidence.json` and `report.md` |
| coverage and equivalence checks, ambiguity, thin features | implemented (geometric; see `docs/release/LIMITATIONS.md`) |
| analytical benchmark through the workflow | R1 met with criteria written before the run |
| external solver comparison | **pending**: export and compare tool ready, CalculiX not installed |
| failure recovery | failure class, stage, recovery and partial results in job status, evidence and report |
| evaluation set | 10 cases defined before running; scripted client passes all |
| real AI client | **blocked**: expired command-line authentication; harness ready |
| packaging, diagnostic, clean install, CLI route | `tools/package.sh`, `navier-ctl doctor`, `tools/install_test.sh` (22/22), `navier-ctl study ...` |
| misleading claims | capabilities text, MCP client paths and checkpoint statement corrected |

Change of plan: step 1 (factoring mesh generation out of its handler) was not needed. Studies run the existing
operations on private engines, one per design, which reuses validation, provenance, project format and run
directories unchanged.

Readiness: `READINESS.md`.
