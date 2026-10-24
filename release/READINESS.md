# NAVIER 0.3.0-rc1: release candidate and readiness assessment

> Kept as the rc1 assessment. The independent evaluation and the rc2 recommendation are in
> `release/evaluation/EVALUATION_REPORT.md`; several rc1 statements below (the "numerical uncertainty" of R3, the pending
> CalculiX comparison) were corrected there.

Assessed on 2026-09-16 at the end of the integration work. The baseline and its gaps are in `GAP_ASSESSMENT.md`, and
the source hashes at the start in `BASELINE-2026-09-16.sha256`.

## Verdict

**A technically complete release candidate for internal review. Not a dependable public release yet.**

The bracket-comparison workflow runs end to end, headless, through MCP and the command line, from a clean
installation. Its evidence is recorded, it replays bitwise, it fails usefully on the ten evaluation cases, and every
regression suite passes. What it lacks is evaluation by anyone other than its author:

- no real AI-client session has completed (command-line authentication had expired);
- no external user has tried it;
- the independent CalculiX comparison is pending (CalculiX is not installed);
- nothing has been validated against measurements;
- only one platform was tested;
- no licence has been chosen.

## Artefacts

| Artefact | Where |
|---|---|
| Release candidate package (macOS arm64, 1.6 MB) | `dist/navier-0.3.0-rc1-macos-arm64.tar.gz`, SHA-256 in the `.sha256` file beside it |
| Package build (isolated copy of the sources, `-mcpu=apple-m1`) | `tools/package.sh` |
| Clean-installation test | `tools/install_test.sh dist/navier-0.3.0-rc1-macos-arm64.tar.gz` |
| Release documentation | `docs/release/` (copied into the package's `doc/`) |
| Example with expected output | `examples/bracket_comparison/` |
| Reference-case criteria and results | `reference_cases/CRITERIA.md` |
| Evaluation set, and the real-client harness with its blocker | `evaluation/CASES.md`, `evaluation/ai-client/` |

## Stage reports

### 1. Baseline and gaps

- **Implemented:** an inspection of the structural path, the source hashes, a snapshot
  (`.recovery/2026-09-16T2106-pre-release.tar.gz`) and the gap table.
- **Findings:** the path from STL import to static solve was complete and verified. Missing were:
  - region quantities, stiffness, mass, moments;
  - refinement, comparison and evidence;
  - payload mass loads;
  - packaging, a diagnostic and a CLI route;
  - a real-client check.

  Three misleading statements were found on the release path.

### 2. One complete saved comparison study

- **Implemented:**
  - operations `study_check`, `study_run`, `study_evidence` and `study_replay`;
  - `results_quantities`;
  - mass, reaction moments, moment balance and deformation indicators in every static summary.

  The study definition is schema-validated, with provenance for every value (user, measured, database, inferred,
  default). A resolved, hashed `study.json` is written once. Each design becomes an ordinary project, composed from
  the existing operations on a private engine and logged in `operations.jsonl`.
- **Tested:** the example (2 designs, 3 meshes, 5 sensitivities, 16 analyses) completes in about a minute.
- **Unresolved:**
  - a study holds one load case, one material per design and 2–4 designs;
  - by default, result fields of sensitivity runs are removed after evaluation to save disk (hash kept, replay
    regenerates them).

### 3. Geometry, selection and load consistency

- **Implemented:**
  - design-frame region queries (no rotation, translation recorded);
  - selection coverage and centroid shift per mesh;
  - the load's line of action against the load as specified on the STL surface;
  - an equivalence check between designs (lever arm, areas, normals, with tolerances);
  - detection of regions spanning parallel planes;
  - thin-wall and vanishing-feature warnings;
  - invalid levels (lost connection, volume error above 10 %, unmapped mounting);
  - blocking questions for missing units or material, under-constrained mounting, unassessed mounting assumptions
    and printed parts without process data.
- **Tested:** evaluation cases E2–E6 and E8; line-of-action shifts of about 1e-13 of the criterion on the example.
- **Unresolved:**
  - equivalence is geometric and cannot see every way designs might be held differently;
  - ambiguity detection is limited to parallel planes;
  - no remote point loads or moments.

### 4. Numerical evidence and independent checks

- **Implemented:** checks on every level (force and moment balance, energy, solver, rigid-body modes, load mapping,
  conjugate displacement = region mean, topology, volume, small deformation); refinement statistics (Richardson/GCI
  or conservative estimates); comparison outcomes gated by those; Young's-modulus scaling verification; a CalculiX
  export and compare tool.
- **Tested:**
  - R1, against beam theory with pre-registered criteria: C10 −0.717 %, C20 −0.704 %, the extrapolated values about
    0.65 % stiffer (clamped 3D root, not discretisation);
  - all identities to round-off.
- **Unresolved:**
  - **the CalculiX comparison has not been run** (CalculiX is not installed; the deck export was exercised, the solve
    was not);
  - the uncertainty estimates follow standard practice but are estimates, not bounds.

### 5. MCP workflow and failure recovery

- **Implemented:** the MCP tools, with questions (id, blocking, acceptable, why it matters, how to answer) and
  failures (stage, design, element size, failure class, code, recovery, partial results) in `job_status`, the
  evidence and the report. `capabilities_get` declares `comparison_study` and its exclusions. The stale
  implementation-stage text was fixed.
- **Tested:** `tools/studyflow.py`, a scripted MCP client: R1, R2, R3/E1 and E2–E10, 73 checks, part of `make test`.
- **Unresolved:** the conversation-level behaviour of a model is untested (see stage 8).

### 6. Evidence report and replay

- **Implemented:** `evidence.json`, and `report.md` generated only from it, with a traceability table. The report
  covers the question, the modeled conditions, the comparison, the numerical evidence, interpretation, important
  uncertainties, what was not evaluated and reproduction. Replay starts from the stored inputs, verified by hash.
- **Tested:**
  - R2: 24 of 24 quantities bitwise identical;
  - the install test: 44 of 44 quantities identical to the packaged expected output, from another folder and home.
- **Unresolved:**
  - reproducibility is shown on one build and machine only;
  - other CPUs, compilers or thread counts may differ in the last digits, and the replay tolerance of 1e-9 covers
    that.

### 7. Packaging and clean environment

- **Implemented:**
  - `navier-ctl --embedded` with `call`, `study check|run|replay|evidence|report` and `doctor`;
  - the package script (isolated build, docs, example with expected output, tests, tools, VERSION, NOTICE,
    SHA256SUMS);
  - the install test.
- **Tested:** `doctor` passes on the development tree and in a fresh home. The install test passed 22 of 22 checks
  under `env -i` with a new HOME: checksums, docs, no developer paths in the binaries, doctor, example, bitwise
  comparison with the expected output, replay, MCP cases.
- **Unresolved:**
  - macOS arm64 only;
  - Linux builds untested;
  - no code signing or notarisation (Gatekeeper may quarantine downloads);
  - no licence.

### 8. Real AI-client evaluation

- **Implemented:** a session harness (`evaluation/ai-client/`: prompts for E1–E10, runner, grading rubric).
- **Tested:** one attempt. Both installed Claude Code CLIs (2.1.217, 2.1.271) stopped at "OAuth session expired and
  could not be refreshed" before any model turn. The transcript is recorded.
- **Unresolved:** **no model-driven session exists.** Logging in needs the account owner's credentials. Codex is not
  installed.

### 9. Release candidate

- **Regressions (`make test`, exit 0):**

  | Suite | Checks passed |
  |---|---|
  | coretest | 199 |
  | surftest | 30 |
  | meshtest | 29 |
  | femtest | 42 |
  | thermtest | 72 |
  | tsteptest | 54 |
  | orchtest | 24 |
  | advtest | 43 |
  | rendertest | 29, plus the PNG check |
  | opstest | 137 |
  | ctltest | 47 |
  | amtest | 102 |
  | mcptest | 246 |
  | amflow | 71 |
  | thermflow | 95 |
  | transientflow | 81 |
  | restartflow | 154 |
  | contactflow | 104 |
  | chtflow | 102 |
  | studyflow (new) | 73 |
  | doctor (new) | all checks |

  The opstest and mcptest counts grew with the new operations.
- The GUI builds without warnings, and `make test-ui` passes 52 interface checks.

## Definition of done

| Requirement | State |
|---|---|
| both geometries analysed through the same headless workflow | **holds** |
| inputs, selections and assumptions inspectable | **holds**: study.json, per-design projects, region previews, operations log, questions and assumptions with sources |
| the comparison uses meaningful quantities | **holds**: area-weighted load-region displacement, energy-consistent stiffness, geometry mass, reactions with moments; stress only as supporting information |
| numerical checks and refinement evidence recorded | **holds** |
| the result can be reopened and reproduced | **holds** on the tested platform (bitwise replay, reopenable projects) |
| invalid and unsupported requests fail usefully | **holds** for the ten defined cases with a scripted client; unproven with a model |
| reachable through MCP and CLI | **holds** |
| the evidence record states what is and is not established | **holds** |
| the package works outside the development directory | **holds**: clean-environment test |
| applicable regression tests pass | **holds** |
| external-client, experimental and user-testing gaps disclosed | **holds**: this file, `docs/release/LIMITATIONS.md`, `evaluation/ai-client/RESULTS.md` |

## Before calling it dependable

1. Log in to Claude Code and run `evaluation/ai-client/run_sessions.sh`. Grade the ten transcripts with `RUBRIC.md`,
   and fix what the model misuses. The tools have been checked; the conversations have not.
2. Install CalculiX and run `tools/ccx_crosscheck.py` on the example's 1 mm runs of A and B. Explain the differences.
3. Put the release in front of at least one engineer who did not build it, with the example and one of their own
   brackets, and record what failed.
4. Choose a licence. Decide on code signing, and build and test on Linux if it is to be supported.
5. Consider next: conforming or body-fitted meshes for inclined features, remote point loads with moment, bolt-group
   mounting models, load cases.
