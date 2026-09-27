# History: what was built, when, by which effort

So that nothing valuable is rebuilt because nobody knew it existed. Assembled on 2026-09-17 from `STATUS.md`,
`Thermal Sim/STATUS.md`, `release/`, `.recovery/`, `Context/*.md` (both removed from the repository on 2026-09-27, see
the table of abandoned things), this repository's git log and the read-only git log of a private mechanics working
copy. Each entry says where it comes from; nothing here was re-run to confirm it unless the
line says so.

Entry point: [../../AGENTS.md](../../AGENTS.md) · modules: [MODULES.md](MODULES.md) · request path:
[DATAFLOW.md](DATAFLOW.md) · hidden working things: [GEMS.md](GEMS.md).

## How to read the origins

| Tag | Means |
|---|---|
| [CHECKED] | the file, the commit or the directory was read on disk by this session while writing this file |
| [STATUS] | stated in `STATUS.md` or `Thermal Sim/STATUS.md` |
| [SESSION] | stated in a `Context/session-*.md` hand-off (removed 2026-09-27), that session's own claim |
| [OWNER] | recorded as the owner's own words in `VISION.md` or `Context/ARCHITECT_RECORD.md` (removed 2026-09-27) |

## Timeline

### Before 2026-09-15 — the fluid tunnel

The original NAVIER: a real-time 3D lattice Boltzmann water tunnel in plain C with its own OpenGL renderer, UI,
in-app terminal and visualisation kernels (`src/lbm.c`, `src/sim.c`, `src/render.c`, `src/ui.c`, `src/hud.c`,
`src/console.c`, `src/commands.c`, `src/vis_*.c`, `src/geom/`). Guide: [../water-tunnel.md](../water-tunnel.md).
No dated record of this work exists in the repository; it is the substrate everything else was added to. [CHECKED]
(the code and the guide exist; the dates do not).

### 2026-09-15 — NAVIER-AM begins: operations, socket, MCP, structural FEM

Milestones 1–3 of the AM brief: the repository audit; the typed operation layer with the control socket and a minimal
MCP server; then surface selection, the voxel hexahedral mesher and the static structural FEM, with the first complete
MCP-driven path. `STATUS.md` records suite counts from this day. [STATUS]

### 2026-09-15 evening → 2026-09-16 01:30 — the combined interface (session *gui*)

Milestone 4's control layer and the SOLID workspace in the app: `src/fembridge.c`, so that the app's buttons call the
same operations an agent calls, one window for both solvers. [SESSION] `Context/session-gui.md` §1.

### 2026-09-16 — transient thermal, then multiphysics (Thermal Sim)

Milestone 4 proper and a five-stage release, each stage with pre-registered criteria and its own evidence file under
`Thermal Sim/verification/`: [STATUS]

| Stage | What | Evidence |
|---|---|---|
| A | adaptive time integration: step doubling, PI controller, transactional trials, events | `tools/tsteptest.c` 54 checks, `transientflow.py` 77 |
| B | durable checkpoint and restart, bitwise resume after SIGKILL | `restartflow.py` 154 checks |
| C | thermal contact through MCP (`interface_*`, `results_interface`) | `contactflow.py` 104 checks |
| D | the shared orchestrator: participants, cycles with Aitken relaxation, commit/rollback | `orchtest.c` 24 checks; D2 failed first and exposed two defects |
| E | nonsymmetric solver, SUPG advection, LBM as a steady-flow participant, conjugate heat transfer through MCP | `advtest.c` 43, `chtflow.py` 102; **E5c not met** and recorded: the enthalpy-flux energy balance closes only to 2.4e-4 because the mapped flow is not discretely divergence-free |

Earlier in the same effort: quadrature-point properties, anisotropic conductivity, the enthalpy formulation, phase
change with latent heat (three defects found and fixed, including latent heat that was parsed and then discarded
before the solve), and Aitken relaxation for the melting Picard map. `Thermal Sim/STATUS.md` §"What changed in this
work" lists twelve items. [STATUS]

Recovery snapshots of that day, on the development machine only [CHECKED]: `.recovery/2026-09-16T1137-pre-stageA.tar.gz`, plus
pre-rewrite copies of single files (`thermal_run.pre-orchestrator.c`, `thermal.pre-advection.c`,
`ops_schema.pre-cht.json`, `thermal_model.pre-cht.c`, `thermal_checkpoint.pre-cht.c`, `transient_analysis.pre-cht.h`,
`hexmesh.c/.h`, `thermal_integrator.c/.h`; tracked in `.recovery/` until 2026-09-27).

### 2026-09-16 21:00–23:20 — integration release 0.3.0-rc1

The comparison-study workflow: `study_check` / `study_run` / `study_evidence` / `study_replay`, the Engineering
Evidence Record, the replay, the packaging (`tools/package.sh`) and the clean-install test (`tools/install_test.sh`),
the release documents in `docs/release/`, and the bracket example with its expected output.
Snapshot `.recovery/2026-09-16T2106-pre-release.tar.gz` [CHECKED]. Package `dist/navier-0.3.0-rc1-macos-arm64.tar.gz`
[CHECKED]. [SESSION] `Context/session-release-eval.md` §1.

### 2026-09-16 23:26 → 2026-09-17 03:20 — the independent evaluation, giving 0.3.0-rc2

rc1 was preserved (`release/evaluation/baseline-rc1/`, snapshot
`.recovery/2026-09-16T2326-rc1-evaluation-baseline.tar.gz` [CHECKED]) and then audited:

- **Scientific claims** re-checked; the "numerical uncertainty ±…" of rc1 was found unsupported and replaced by three
  separate conclusions (ranking per tested mesh, the convergence criterion, an estimate only under stated conditions).
- **Independent solver comparison** against CalculiX 2.23: the same assembled problem agreed to ~1e-7, an
  independently built cantilever to 0.0015 %.
- **The 0.7 % beam discrepancy** was explained by pre-registered tests (clamped-root Poisson restraint).
- **Defects fixed with regression tests** (`tools/evaltest.c`, now in `make test`): the estimator and its wording,
  the change definition, the access-root limit, storage estimates and write-failure handling.
- **0.3.0-rc2** built and clean-install tested; rc1 left byte-identical.

[SESSION] + [CHECKED] (the report, the record and both packages exist). Report:
[../../release/evaluation/EVALUATION_REPORT.md](../../release/evaluation/EVALUATION_REPORT.md).

### 2026-09-17 — the real AI-client evaluation

18 graded sessions (six cases, three repeats) with Claude Code driving only the NAVIER MCP tools, after the owner
logged the client in. Verdict: **competent tool use, not dependable yet** — the model answered questions meant for the
user (units 0 of 4 asked, the ambiguous face 0 of 3), accepted open questions with its own reason in 6 of 18 sessions,
reported numbers under `cannot_establish`, and once called a part safe. Five NAVIER design findings are open; they are
the starting point of [../design/certificate.md](../design/certificate.md) §5. Cost USD 29.23, reported by the client.
[SESSION] + the graded results file `release/evaluation/ai-client/RESULTS.md` [CHECKED].

### 2026-09-16 to 2026-09-17: the mechanics layer (a private working copy)

Built in an isolated copy, never merged into this checkout. Stages, from the git log read on 2026-09-17 [CHECKED]:

| Commit | What |
|---|---|
| `ffda6b8` | stages 1–2: isolation, integration contract, rigid multibody core |
| `cbdd00b` | mass properties from closed meshes, printed fill models (78 checks) |
| `c81cc6e` | assembly model, URDF subset, XML reader (110 checks) |
| `2f12446` | study runtime: motors, sampled PID, sensors, checkpoint rollback (130 checks) |
| `a9eb11b` | load transfer to FEM, isostatic supports (141 checks) |
| `d97415a` | mechanics operations over MCP + `mechflow` end-to-end test (58 checks) |
| `9bf6a5d`, `cae06c0` | rigid contact with Coulomb friction, through assembly, runtime and MCP (152 checks) |
| `0d1157d` | orthotropic printed parts |
| `487a4df` | modal and transient structural response |
| `fb50a1a` | flexible bodies coupled with the multibody dynamics |
| `70d63f1`, `e52001f`, `aced45a` | re-baseline onto 0.3.0-rc1, robot-arm demonstration, status |
| `9639919` | **layer-by-layer FFF print simulation**: deposition plan, activation, incremental thermo-elastic stress, bed release; `test_fff_print` 8 checks inside "235 passed, 0 failed" (from the commit message, not re-run) |

Note for the architect: `Context/ARCHITECT_RECORD.md` B1 records the FFF work as *uncommitted* with last commit
`aced45a`. As of 2026-09-17 it **is committed** as `9639919`; only `demo/print_player/` remains untracked. [CHECKED]
`git log`/`git status` in that copy.

### 2026-09-17 — the project becomes OpenPhysicsAI

This repository's git history begins here [CHECKED] `git log`:

| Commit | What |
|---|---|
| `a082738` | baseline: the 0.3.0-rc2 working tree as found |
| `acb235e` | `VISION.md`, the architect record, provisional work packages |
| `804bfef` | provisional name FreePhysLab |
| `df55c18` | rule: commit messages carry no AI attribution |
| `61aada0` | renamed to **OpenPhysicsAI** |
| `1f7162b` | the front page: `README.md` with real renders, `AGENTS.md` as the entry point |

Three worker branches were opened from `1f7162b`: `am-process` (mechanics and print, port in progress),
`app-am`, `quality-map` (this branch). [CHECKED] `git worktree list`.

### 2026-09-18: the first physical validation data, and a reference model written before the solver

The owner's LPBF cantilever measurements from Hochschule Anhalt entered the repository (`validation/hs-anhalt/`,
commits `5e7b423`, `68d7b86`) [CHECKED]. On this branch: the quality gate (`make check`), a pre-registered protocol,
the geometry parameter file and an independent CalculiX reference model, all written before the LPBF solver on
`am-process` existed, so that nothing could be tuned towards a known answer. The data, the protocol, the reference
model and the challenges built on them were removed on 2026-09-27 (below); the solver stays.

### 2026-09-25 to 2026-09-27: the physics lab, then the thirteen flags

The physics lab (`src/lab/`) grew from the owner's brief of 2026-09-25 into more than twenty verified solvers:
compressible flow with adaptive meshes, acoustics, impact, orbits, electromagnetism, flow on the GPU, magnetics in 2D
and 3D (MFEM), relativity, water, fracture, sheets, melting with Marangoni flow, fluid-structure interaction, fire and
rooms on a multigrid pressure solver, batteries. Its programme and log: [GOALS.md](../../GOALS.md) G1 to G14 [CHECKED]
(the git log of `lab/physics-lab`). On 2026-09-27 the owner redirected the whole repository around thirteen flags,
measurements of the real world across the main branches of physics, three of them holy grails beyond today's reach, with
boards that credit the person who captures a flag, the libraries and the AI model (GOALS.md G15) [OWNER]. The front
page, the flag pages and cards (each flag a gem with its own animated glyph and colour), `tools/flags.py board` and its
`--check` in CI and `make check`, the physics map ([PHYSICS.md](PHYSICS.md)) and `llms.txt` date from that day [CHECKED].

## Abandoned, superseded or rejected

| Thing | Where it is | Why it stopped |
|---|---|---|
| **Browser print player** (`demo/print_player/player.html` in `navier-mech`) | untracked in the mechanics copy | **Rejected by the owner on 2026-09-17**: "A browser viewer for print results was the wrong direction: additive-manufacturing results must be shown in the native application" [OWNER] [VISION.md](../../VISION.md). The physics behind it (`fffprint.c`) is kept and is arriving on `am-process` |
| **Provisional name FreePhysLab** | commit `804bfef` | replaced the same day by OpenPhysicsAI (`61aada0`) [OWNER] `ARCHITECT_RECORD.md` A11→A12 |
| **The OpenPhysics score and the simulator podium** (`flags/RANKINGS.md`, `flags/rankings.svg`) | removed 2026-09-27 | superseded by the thirteen flags' boards, which credit people (the challenger, the libraries, the AI model) rather than a program, and by the hall of fame [OWNER] GOALS.md G15 |
| **The 2D warm-up answers** (bow shock, shock through a gas cylinder, cylinder shedding) | `flags/entries/reference/` | the owner: flags built in 2D and shown in 3D are a mistake; being rebuilt natively in 3D, their boards standing until then [OWNER] GOALS.md G15 |
| **rc1's "numerical uncertainty ±…"** | `dist/navier-0.3.0-rc1-*` and `release/READINESS.md` | the estimator's conditions did not hold for a staircase geometry; replaced in rc2, with both versions reported [SESSION] evaluation report §1 |
| **Print bridge run v1** (`print_bridge`) | mechanics copy | contained a reheating error, superseded by run v2 [SESSION] `ARCHITECT_RECORD.md` B3, not independently verified |
| **`property_evaluation: element_mean`** | still in the code | kept only for comparison with the older element-mean behaviour [STATUS] `Thermal Sim/STATUS.md` |
| **Architect's earlier "vision" answers** | withdrawn in `ARCHITECT_RECORD.md` §C | they presented session inferences as the owner's intent; `VISION.md` replaced them [OWNER] |
| **The HS Anhalt cylinder head** (its geometry, HS Anhalt's Simufact results and reports, our runs, films and screenshots, and the scripts and tests that existed only to use it) | removed 2026-09-27; kept only in the private history, since the public repository starts from a fresh commit ([LAUNCH.md](../release/LAUNCH.md)) | copyright, by the owner's decision [OWNER] |
| **The HS Anhalt material and the internal working files** (`validation/hs-anhalt/` with the measurements, Simufact results, reports, specimen geometry and protocol; the results computed on it in `validation/results/`; the challenges `hsa-cantilever-2025` and `hsa-specimen-2025`; `tools/validate.py`, `tools/ccx_cantilever.py`, the cantilever and specimen scripts in `demo/`, `tools/buildwalk.nav`, `tools/demo_lpbf.nav`, `make validate`; the Simufact strain sets built into the engine; the measured AlSi10Mg record; the university brief and the one-page paper; `Context/`, `ORCHESTRATION_PROMPTS.md`, `.recovery/`, `Mech Sim/baseline/`, the AI-client transcripts; the two agent-run records built on that data) | removed 2026-09-27, before publication; kept only in the private history, since the public repository starts from a single new commit ([LAUNCH.md](../release/LAUNCH.md)) | by the owner's decision [OWNER] |

## Open, by the record

- **Milestone 5 (FFF activation, cooling, bed release)** — prototype exists in the mechanics copy, arriving on
  `am-process`; not in `main`, not in the app, not reachable through MCP [CHECKED].
- **Milestone 6 (LPBF)** — not started [STATUS]. Since 2026-09-17 this is the **anchor application**: the owner
  stated that metal powder-bed process simulation, as a free AI-native alternative to Simufact Additive, is what
  the project is aimed at ([VISION.md](../../VISION.md), "Focus stated by the owner on 2026-09-17") [OWNER].
- **Streamable HTTP transport** — not started [STATUS].
- **Fluid solver through MCP** — the fluid tunnel is app-only; only `src/fem/flow.c` uses the kernel headlessly [CHECKED].
- **Independent users, physical measurements, a licence** — none; the kit, the protocol and the inventory are ready
  [SESSION] evaluation report §5–§7.
- **The five AI-client findings** — who supplied an accepted reason, inferred units, pre-empted ambiguity, numbers
  under `cannot_establish`, and the missing scope notice in single analyses [SESSION] + [CHECKED] in `RESULTS.md`.
