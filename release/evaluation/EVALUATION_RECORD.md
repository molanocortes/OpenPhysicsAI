# Evaluation record: NAVIER 0.3.0-rc1

A running record of the independent evaluation that started on 2026-09-16 at 23:26 CEST. Entries are appended as
evidence is produced; conclusions are in the final report.

## Baseline

| Item | Value |
|---|---|
| Artifact under evaluation | `dist/navier-0.3.0-rc1-macos-arm64.tar.gz`, preserved unchanged in `release/evaluation/baseline-rc1/` |
| Artifact SHA-256 | `14330796e8ea9a83f421d5478e2dc51db7a66e56cb9bed8983b6705562f33d7b` (matches the `.sha256` file shipped beside it) |
| Sources of the artifact (`VERSION.sources_sha256`) | `d7f50b0a476bc0320492cd153c4c6164cd74fbea87493529a263f6151aa26544`; recomputed from the working tree at the start: identical |
| Per-file source manifest | `baseline-rc1/SOURCES.sha256` (251 files; manifest SHA-256 `f21fd5ab…`) |
| Recovery snapshot | `.recovery/2026-09-16T2326-rc1-evaluation-baseline.tar.gz` (SHA-256 `f0dfbfb4…`) |
| Binaries under test | the tarball's `bin/`, extracted to an isolated scratch folder; the working tree's `build/` was not used for the audit |

## Test environment

| Item | Value |
|---|---|
| Machine | Apple M2 MacBook Air, 8 GB RAM, fanless; macOS 15.7.3 (24G419) |
| Compiler of the artifact | Apple clang 17.0.0 (clang-1700.0.13.5), `-mcpu=apple-m1` |
| Python | 3.10.0 with NumPy 2.2.6 (evaluation scripts only) |
| Free disk at start | 3.4 GB |
| CalculiX | 2.23, conda-forge `calculix-2.23-pl5321h33a25c5_4` (osx-arm64), in `~/.navier-eval-tools/ccx-env`. Dependencies: arpack 3.9.1, openblas 0.3.34, libgfortran 16.2.0, llvm-openmp 23.1.1. Installed with micromamba 2.9.0 (`micromamba-2.9.0-0.tar.bz2`, 6,648,894 bytes, SHA-256 `500f5074…`, from `https://micro.mamba.pm/api/micromamba/osx-arm64/latest`, served by conda-forge package storage). Configuration files ignored (`--no-rc`) and only the conda-forge channel used. Removable by deleting `~/.navier-eval-tools` |

## Evaluation criteria

- `CRITERIA-independent.md`: CalculiX comparisons (X1 same assembled problem, X2 independently built case) and the
  beam-discrepancy hypothesis tests. Fixed before any of those runs.
- `reference_cases/CRITERIA.md` and `CASES.md` from rc1 remain in force for regression.

## Results

### Scientific claims of rc1 (bracket example)

Verified from `examples/bracket_comparison/expected_output/evidence.json` (identical to the packaged file), and
extended by a four-level refinement run (4, 2, 1, 0.5 mm) with the baseline binaries.

| rc1 claim | Stored or measured value | Verdict |
|---|---|---|
| B displaces about 65.5 % less than A | 65.45 % at 1 mm. On every tested mesh B displaced less: 60.5 % (4 mm), 63.9 % (2 mm), 65.5 % (1 mm), 66.2 % (0.5 mm) | confirmed as a finest-mesh value; the size of the difference had not converged |
| B is about 27 % heavier | 27.3 % (geometry mass 0.145152 / 0.114048 kg); mesh masses at 1 mm differ by 26.1 % | confirmed |
| B changes by 3.4 % on the finest refinement, failing 2 % | 3.53 % by the stored definition (change relative to the finer mesh); 3.41 % relative to the coarser | corrected to 3.5 %, with the definition stated; the criterion failure stands |
| mesh uncertainty intervals ±0.00025 mm (A), ±0.00032 mm (B) | a grid convergence index with safety factor 1.25 and the observed order from exactly three meshes, printed with "±" and called "numerical uncertainty" | terminology and justification not supported (see below) |

**Mesh uncertainty: what the numbers are.** A Richardson extrapolation with p from three meshes, turned into a GCI
with safety factor 1.25:
- Roache reserves 1.25 for studies whose observed order is shown to be in the asymptotic range, which three meshes
  cannot show;
- for B the geometry changes between meshes (staircase volume error −3.57, −1.79, −0.89, −0.45 %), so the meshes do not
  discretise one fixed problem;
- the "±" and the term "uncertainty" read like a confidence interval or a bound, which a GCI is not.

**The fourth mesh:**

| Design | Values at 4 / 2 / 1 / 0.5 mm (mm) | Changes (relative to the finer mesh) | Observed order (4-2-1, 2-1-0.5) |
|---|---|---|---|
| A (geometry exact at every mesh) | 0.0247427 / 0.0251846 / 0.0253994 / 0.0254981 | +1.75 %, +0.85 %, +0.39 % | 1.040, 1.122 |
| B (staircase chamfer) | 0.0097683 / 0.0090846 / 0.0087746 / 0.0086276 | −7.53 %, −3.53 %, −1.70 % | 1.141, 1.077 |

A's orders are close to the rate expected where a sharp re-entrant 270° corner limits convergence (about 1.09).
Nothing in these data contradicts the rc1 estimates: the 0.5 mm values fall inside the rc1 bands. The terminology and
the conditions under which an estimate is offered still need correcting (defect D1).

**Solver error.** At 1 and 0.5 mm the iterative solver (block-Jacobi PCG, relative residual 1e-10) was repeated with a
relative residual of 1e-13. Iterations roughly doubled (893 → 1819 for A at 1 mm) and the displacements changed by at
most 1.3e-12 relative. The recomputed true residual stays near 2e-10 in both runs, a floating-point floor. The solver
error is therefore negligible against the mesh changes.

Cost of the 0.5 mm level on this machine:
- A: 337,920 elements, 1.09 M dofs, 44 s, 1.2 GB peak;
- B: 428,160 elements, 1.37 M dofs, 54 s, 1.4 GB peak.

It required `limits.max_elements` 450,000 (default 400,000).

### Equivalence of the compared conditions (claim C)

Checked in the four-level record (`audit/four_levels_evidence.json`), at every level of both designs.

| Aspect | A | B | Reading |
|---|---|---|---|
| Load | 3 kg × g = 29.41995 N along (0, 0, −1), uniform traction over the pad | identical | same force, direction and distribution |
| Load region | pad 64–80 × 12–28 mm at z = 60, 256 mm², normal +z, centroid (72, 20, 60) | identical | the same evaluation region. On every mesh its area is 256 mm² to 1e-12, and the load's line of action moved by at most 1.1e-12 mm |
| Support | back face x = 0, fixed, 2400 mm², normal −x, centroid (0, 20, 30) | identical | the same support. The mesh area is 2400 mm² at every level |
| Lever arm (mounting centroid to load centroid) | 78.000 mm | 78.000 mm | equal |
| Reactions | 29.41995 N; moment −2.118236 N·m about y (mounting centroid) at every level | identical to 1e-9 N·m | force and moment balance to round-off; the moment equals load × lever arm projected (29.41995 N × 72 mm) |
| Self-weight | not included | not included | excluded for both. As a declared sensitivity it changes the displacement by at most 0.98 %, and the ranking is unchanged |
| Evaluated quantity | area-weighted mean of u·(0, 0, −1) over the load region; the work-conjugate displacement equals it to ≤ 5.4e-14 | same | the same definition |

The designs are compared under the same force, moment, direction, distribution, support region and evaluation
region; only the chamfer differs.

### Independent numerical comparison with CalculiX

Criteria: `CRITERIA-independent.md`, written before any CalculiX solve; amendments appended there. Scripts, decks,
output extracts and hashes: `independent/`. The NAVIER records behind the comparisons are in `independent/navier_studies/`.
Field files are not retained; their SHA-256 values are in `field_files.sha256`.

**Commands.** Run from the repository root.
- `REL` is the extracted rc1 package.
- `W` is a work folder.
- `F` is the four-level study folder, produced by `navier-ctl --embedded ... study run release/evaluation/audit/study_4levels.json --dir $F`.

```bash
micromamba create --no-rc -y -p ~/.navier-eval-tools/ccx-env -c conda-forge --override-channels "calculix=2.23=pl5321h33a25c5_4"
FOUR_LEVELS=$F python3 release/evaluation/independent/navier_runs.py $REL $W
python3 release/evaluation/independent/x1_ccx.py $W $F release/evaluation/independent/x1
python3 release/evaluation/independent/x2_ccx.py $W release/evaluation/independent/x2
python3 release/evaluation/independent/beam_tests.py $W release/evaluation/independent/x2 release/evaluation/independent/beam_hypothesis.json
python3 release/evaluation/independent/retain_navier.py $W $F
```

Notes:
- The `micromamba` line recreates the recorded environment; the installation was made with micromamba 2.9.0 from
  conda-forge only.
- CalculiX ran with `OMP_NUM_THREADS=4`; NAVIER used its default thread count.

**X1: NAVIER's assembled problem solved again by CalculiX** (`x1_ccx.py`, `x1/x1_summary.json`). The mesh, supports and
nodal loads are exported from NAVIER's stored run. The compared quantities are:
- the nodal mean of the displacement over the load region;
- the reaction force, and the reaction moment about the mounting centroid;
- the strain energy;
- the σxx mean of one element's integration points.

Values are relative differences, CalculiX against NAVIER.

| Case | CalculiX element (NAVIER formulation) | CalculiX solver | Displacement | Strain energy | Reaction force | Reaction moment | σxx | CalculiX balance | NAVIER balance |
|---|---|---|---|---|---|---|---|---|---|
| C10, 1.25 mm | C3D8 (full) | SPOOLES | +5.2e-10 | +1.9e-8 | +2.9e-7 | +2.2e-8 | −1.9e-8 | 2.9e-7 | 4.6e-11 |
| C10, 1.25 mm | C3D8I (incompatible modes) | SPOOLES | +6.4e-9 | +4.3e-8 | +3.9e-7 | +4.3e-11 | +2.1e-11 | 3.9e-7 | 6.6e-11 |
| C10, 0.625 mm | C3D8 | SPOOLES | −5.8e-9 | −4.6e-8 | +1.5e-7 | +1.9e-8 | +7.1e-9 | 1.5e-7 | 3.8e-10 |
| C10, 0.625 mm | C3D8I | SPOOLES | −5.4e-9 | −1.5e-7 | +7.8e-8 | +1.2e-8 | +4.5e-8 | 7.8e-8 | 2.6e-10 |
| A, 1 mm | C3D8 | SPOOLES | +6.9e-9 | −2.4e-8 | +6.8e-8 | +1.0e-8 | +6.2e-8 | 6.8e-8 | 1.4e-10 |
| A, 1 mm | C3D8I | SPOOLES | +4.1e-10 | +1.2e-7 | +5.3e-8 | +1.2e-8 | +8.2e-8 | 5.2e-8 | 1.3e-10 |
| B, 1 mm | C3D8 | SPOOLES | −4.7e-9 | +1.4e-7 | +1.8e-8 | +3.6e-9 | −3.3e-8 | 1.8e-8 | 5.1e-11 |
| B, 1 mm | C3D8I | SPOOLES | **CalculiX exited with signal 11 after 42 s**: no result | | | | | | |
| B, 1 mm | C3D8I | iterative Cholesky (219 iterations) | −3.1e-7 | −5.0e-7 | +2.9e-3 | +1.3e-3 | −4.8e-6 | **2.9e-3** | 5.7e-11 |
| B, 2 mm (added) | C3D8 | SPOOLES | −1.9e-9 | −3.4e-7 | +4.8e-8 | +7.3e-9 | +4.8e-8 | 4.8e-8 | 1.2e-11 |
| B, 2 mm (added) | C3D8I | SPOOLES | −2.0e-8 | −3.7e-7 | +9.8e-8 | +3.1e-8 | −5.6e-8 | 9.8e-8 | 2.4e-11 |

The largest nodal displacement difference over all nodes is 5.0e-9 mm in every direct-solver case, and 4.1e-7 mm with the
iterative solver. The direct-solver values sit at CalculiX's print resolution of 7 significant digits.

| Criterion | Outcome |
|---|---|
| X1a: C3D8 against full integration, within 1e-6 | met in all 4 cases (largest 3.4e-7) |
| X1b: each code balances to 1e-6; reactions agree to 1e-6 | met in the 9 direct-solver cases. **Not met** by CalculiX in the iterative B 1 mm run (2.9e-3), where the stopping limit is fixed by CalculiX; NAVIER balanced to 5.7e-11 in that case |
| X1c: C3D8I against incompatible modes, within 2 % (5 % stress) | met in all 5 cases with a result (largest 4.8e-6) |
| X1d: no singular-system or connectivity warnings | met; CalculiX printed no warnings. One run crashed with signal 11 (SPOOLES, largest deck) |

**What X1 establishes.** For these meshes, NAVIER's element matrices, assembly, supports, load vectors, reactions, energy
and stress recovery match a second, independently written code to round-off. On box elements, NAVIER's incompatible-mode
element coincides with CalculiX's `C3D8I`. X1 cannot detect errors in how NAVIER builds the problem, because the mesh,
regions and load distribution come from NAVIER. It is numerical evidence, not validation.

**X2: a cantilever built separately for both codes from a written specification** (`x2_ccx.py`, `x2/x2_summary.json`).
CalculiX mesh, supports and consistent loads were generated from the text. `C3D20R` is a different element family.

| CalculiX model | Element size | Tip-face mean (mm) | Against Timoshenko (κ = 5/6) | Against NAVIER at 0.625 mm |
|---|---|---|---|---|
| clamped C3D20R 20 × 2 × 2 | 5 mm | 0.0199634 | −0.955 % | −0.240 % |
| clamped C3D20R 40 × 4 × 4 | 2.5 mm | 0.0199989 | −0.779 % | −0.063 % |
| clamped C3D20R 80 × 8 × 8 | 1.25 mm | 0.0200118 | −0.716 % | +0.0015 % |
| clamped C3D8I 160 × 16 × 16 | 0.625 mm | 0.0200115 | −0.717 % | −9e-7 % |
| NAVIER, incompatible modes, 2.5 / 1.25 / 0.625 mm | | 0.0199622 / 0.0199973 / 0.0200115 | −0.962 / −0.788 / −0.717 % | |

Extrapolated from each code's three meshes (for information): CalculiX C3D20R −0.679 %, NAVIER −0.669 % against Timoshenko.

| Criterion | Outcome |
|---|---|
| X2a: finest C3D20R within 0.5 % of NAVIER's finest | met (+0.0015 %) |
| X2b: reaction = 10 N within 1e-9 in both codes | NAVIER met (8.3e-11). **CalculiX not evaluable at 1e-9**: 7-digit output, printed totals exactly 10 N at a 5e-7 resolution (amendment 2) |
| X2c: independent C3D8I at 0.625 mm within 2 % of NAVIER | met (−9.1e-9) |

The independently built `C3D8I` model reproduces NAVIER to round-off, so NAVIER's box geometry, plane selections,
supports and load distribution produce the intended discrete problem for this case. The quadratic-element model converges
to the same value from a different element family.

### Beam discrepancy of 0.7 % (hypothesis tests)

`independent/beam_tests.py` applies the reading fixed in `CRITERIA-independent.md` and writes `independent/beam_hypothesis.json`.

| Test | Observed at the finest mesh (against Timoshenko) | Pre-registered expectation under H1 | Met |
|---|---|---|---|
| baseline C10 (L 100, b 10, h 10) / C20 (h 5) | −0.717 % / −0.704 % | | |
| T1: ν = 0 (C10, C20) | −0.002 % / −0.003 % | below 0.1 % | yes |
| T2: L = 200 mm | −0.359 % | about half of C10 (−0.358 %), ±0.15 pp | yes |
| T3: b = 20 mm | −1.318 % | larger than C10 | yes |
| CalculiX clamped C3D20R against NAVIER | +0.0015 % | within 0.2 % | yes |
| T4: CalculiX C3D20R, root free to contract | +0.050 % | within 0.2 % of Timoshenko | yes |

**Conclusion (by the fixed reading):**
- H1 is supported. The fully clamped root restrains the Poisson contraction of the section, which stiffens the beam by
  about 3ν²c/L with c ≈ 0.25 b.
- Discretisation error (H2) and setup error (H3) are not indicated.

The rc1 attribution was a hypothesis and is now tested. The 0.7 % is a real difference between a clamped 3D solid and
beam theory, not a NAVIER error.

## Defects found

| # | Defect | Class | Status |
|---|---|---|---|
| D1 | Discretisation estimate labelled "numerical uncertainty ±…": a GCI with safety factor 1.25 offered from three meshes and for a geometry that changes with refinement; the comparison outcome `resolved` rests on it | inappropriate estimator and terminology | fixed in rc2 |
| D2 | rc1 documentation quotes B's finest change as 3.4 % while the record stores 3.53 % (different denominators, neither stated) | documentation inconsistency | fixed in rc2 |
| D3 | `navier-mcp` accepts at most 8 roots, 5 of them defaults. The fourth `--allow-read`/`--allow-write` root makes the server exit at start-up with a message that doesn't say which limit applies; an MCP client sees only end-of-file. A write root whose implied read root did not fit was accepted silently | robustness and error reporting | fixed in rc2 |
| D4 | No storage estimate or free-space check before or during a study; the write results of `evidence.json`, `report.md`, `replay.json` and the previews were ignored; a failed `operations.jsonl` open went unnoticed | graceful storage and write failures | fixed in rc2 |
| D5 | "the real attachment lies between idealisations, so this bounds … the uncertainty": the mounting sensitivity does not bound a real attachment (a flexible fastener lies outside both idealisations) | unsupported claim | fixed in rc2 |
| D6 | "Poisson's ratio Poisson's ratio 0.3" in the record's uncertainty list | wording | fixed in rc2 |
| D7 | rc1 documentation attributed the beam discrepancy to root warping and Poisson restraint without a test; warping restraint is not supported (T4 restrains warping and matches beam theory within 0.05 %) | unsupported claim | corrected in the documentation |

Observations that are not NAVIER defects:
- CalculiX's SPOOLES crash on the largest deck (external solver).
- X2b cannot be evaluated at 1e-9, because CalculiX prints 7 digits.
- The developer's own reference set-up for the user-test task selected too many triangles twice with box queries. The
  documented centroid rule is correct, and the reported region areas exposed both errors. It is recorded in
  `user-test-kit/facilitator/REFERENCE.md` as a usability risk for the user test.

## Fixes and their impact

Each fix preserves the failing case as a regression test (`tools/evaltest.c`, 62 checks, with the stored rc1 and
audit values), and the scripted flow (`tools/studyflow.py`) is extended.

- **D1: the estimator and the outcome** (`src/ctl/study_evidence.c`, `study_report.c`).
  - Three conclusions are kept apart:
    - the ranking on every tested mesh (`ranking_by_mesh`);
    - each design's convergence criterion, met or not, which stays in the statement and as a failed evidence row;
    - a discretisation-error estimate, offered only when three or more valid meshes have a constant ratio of at
      least 1.3, a monotone change, an observed order in [0.5, 4], and a geometry represented identically (volume
      error within 1e-6).
  - The safety factor is 3, or 1.25 when four meshes give orders that agree within 10 %.
  - New outcome `ranking_consistent_on_tested_meshes` for a consistent ranking without estimates.
  - No "±" and no "uncertainty" wording for the estimate.
  - Evidence format 2, operation contract 0.6.0.
  - **Impact on the example:**
    - rc1 `resolved` becomes `ranking_consistent_on_tested_meshes`, with B lower on all three meshes (60.5–65.5 %);
    - A has an estimate of 2.4 % (safety factor 3);
    - B has no estimate, and its failed criterion (−3.53 %) is stated.
    - Numerics are unchanged. The packaged rc2 expected output was compared with rc1's:
      - bitwise identical: every level's displacement, work-conjugate displacement, stiffness, geometry and mesh mass,
        and peak displacement; the sensitivity values; the equilibrium errors and solver residuals; the study hash.
      - `energy_ratio` (the Clapeyron check value) differs by one unit in the last place at three levels, because the
        strain energy is summed per thread.
      - The `results.nvr` SHA-256 differs at every level, because the file header records the job id and timings; the
        hash identifies the file, not the solution.
  - **Criteria changed and reported with both versions:** E1 and R3 (`CASES.md` amendment 1,
    `reference_cases/CRITERIA.md` amendment for rc2). R1 remains `resolved`; E9 remains `too_small_to_distinguish`.
- **D2:** every refinement reading states `change_definition` (relative to the finer mesh); documents quote −3.53 %.
- **D3** (`src/ctl/engine.c`, `engine.h`):
  - a root inside an existing root takes no slot (compared on canonical paths);
  - the limit is raised to 16 per kind;
  - the message names the limit, the roots in use and the remedy;
  - a write root whose implied read root cannot be added is refused and withdrawn.
  - Test: scripted case E13, plus `evaltest`.
- **D4** (`src/ctl/study_storage.c`, `study.c`, `study_run.c`):
  - `plan.storage` gives the estimated retained and peak bytes (700 bytes per element plus 80 kB per analysis,
    checked against measured runs: +10 %, +7 %), the free space and sufficiency with a 256 MB reserve.
  - `study_check` warns `INSUFFICIENT_STORAGE`, and `study_run` refuses with `RESOURCE_LIMIT`.
  - Each analysis checks the free space first and stops the study gracefully.
  - A failed operations log stops the study at setup with `IO_ERROR`.
  - A failed evidence or report write fails the job with `IO_ERROR`, naming the file and keeping the comparison in the
    error details.
  - `reproduction.artifacts` lists retained originals, extracted summaries and what a replay regenerates.
  - Tests: scripted cases E11 and E12, plus `evaltest`.
- **D5 and D6:** wording in `study_evidence.c`.
- **D7:** `docs/release/EVIDENCE.md`.

## The evaluated artefact (rc2)

| Item | Value |
|---|---|
| Artefact | `dist/navier-0.3.0-rc2-macos-arm64.tar.gz`, 1,787,593 bytes, SHA-256 `564a691f5cdd2eb35bd3d0ec51bcfb9cc50f0d74211e76eae16d38cfe461ecb0` (copy of the `.sha256` in `rc2/`). Earlier rc2 builds (`14c72652…`, `030b8dd0…`) had byte-identical binaries. They were superseded because documents were corrected, including the AI-client results. Packaging now excludes Finder metadata (`.DS_Store`) from the source hash: a `src/.DS_Store` created while the folder was browsed had changed it |
| Sources | `VERSION.sources_sha256` `77d2f7ed675c16adc385c8db70b97958c8536aa41520a11dc926952971c21402`, equal to the working tree at packaging; per-file manifest `rc2/SOURCES.sha256` (164 files) |
| Changed against rc1 | `Makefile`, `src/ctl/engine.c`, `engine.h`, `ops_project.c`, `ops_schema.json`, `ops_study.c`, `study.c`, `study_evidence.c`, `study_internal.h`, `study_report.c`, `study_run.c`; new `study_storage.c`; new test `tools/evaltest.c` |
| rc1 artefact | unchanged: `dist/` and `baseline-rc1/` still hash to `14330796…` |
| Build | isolated copy, Apple clang 17, `-mcpu=apple-m1`, no warnings |
| Unit and integration tests (isolated build, final sources) | evaltest 62/62, opstest 137/137, ctltest 47/47, amtest 102/102, mcptest 246/246. The long physics flows (amflow, thermflow, transientflow, restartflow, contactflow, chtflow) were not rerun: no numerical code changed, and the example is bitwise identical |
| Clean-install test of the final artefact | 27/27 (`tools/install_test.sh`): checksums; documents and kit present, facilitator reference absent; version, expected output and record format agree; no developer paths in the binaries; `doctor`; the example bitwise equal to the expected output (44/44); replay reproduced (24/24); scripted cases E2–E5, E8, E11–E13 |
| Scripted MCP client on the installed artefact | 87/87 (`studyflow-rc2-results.json`), run on the first rc2 build, whose `bin/` files are byte-identical to the final artefact's (same `SHA256SUMS` entries) |

## AI-client evaluation

**Run on 2026-09-17, after the owner logged the command line in.** Results and grading: `ai-client/RESULTS.md`;
sanitized transcripts and metrics were kept in `ai-client/transcripts/<run>/` until 2026-09-27, when they were removed
from the repository with the raw ones before publication.

- **Setting:**
  - model `claude-opus-4-8[1m]`, Claude Code 2.1.217, print mode;
  - only the NAVIER MCP server, from the installed rc2 package;
  - built-in tools disabled (7 attempts were refused by the client) and no settings loaded;
  - three repeats of E1, E2, E4, E7, E9 and E10.
- **Size and cost:** 18 primary sessions, plus 5 supplementary (a pilot and run A's E7 and E9). 190 NAVIER calls in
  the primary sessions, about 61 minutes of session time, USD 29.23 reported by the client for all 23 sessions.
- **Harness deviation:**
  - Run A gave all sessions one NAVIER workspace, so later sessions met earlier folders: 22 name collisions, all
    recovered, and one session (E7-r1) read another session's record.
  - The harness now isolates every session.
  - E7, E9 and E10 were rerun isolated and are the primary sessions for those cases. E1, E2 and E4 were not rerun,
    because their graded behaviours do not depend on other sessions' files.
- **Per-case reading** (rules fixed before the sessions): E9 partly handled; E1, E2, E4, E7 and E10 not handled.
- **Observed:**
  - Set-up and tool use were competent, and every tool error was followed by a sensible recovery.
  - Failed checks reported by the tools were almost always passed on (B's convergence criterion, the missing estimate,
    `cannot_establish`).
  - The model did not relay questions it should have put to the user: units asked 0 of 4 times, the ambiguous face
    0 of 3, and acceptable questions accepted with its own reason in 6 of 18 primary sessions.
  - It led with sag numbers and a ranking under `cannot_establish` (E7, 3 of 3).
  - In E10-r2 it declared the part "safe by a very large margin" from its own stress runs.
  - Primary sessions added 20 unsupported statements in all, mostly strength remarks and invented "±" bands.
- **Findings for the owner (NAVIER design, not changed in rc2):**
  1. accepted questions record no author of their reason;
  2. a client can declare units "inferred", which avoids the units question;
  3. ambiguity detection can be pre-empted by a narrowed query;
  4. `cannot_establish` still leaves per-level numbers in summaries;
  5. the strength and safety scope notice exists only in the study workflow, not in single analyses.

## Independent user test

**Pending.** The kit is in `user-test-kit/` (participant files in the package), and its tasks are verified to run. The
facilitator reference is the developer's own run and is not usability evidence. It needs the owner to recruit
participants and obtain consent.

## Physical comparison

**Validation pending, no measurement exists.** `physical/` holds:
- the protocol;
- the measurement template;
- `predict.py`, whose planning predictions are in `predictions.json`: A 0.497 mm and B 0.172 mm at 3 kg for
  E 3.5 GPa, ratio 0.346, allowance 3.6 %, p99 stress 3.7 MPa below the 5 MPa planning limit;
- `analyze.py`, which exits 3 without readings. Its self-test uses in-memory synthetic numbers only.

## Distribution

`../DISTRIBUTION_INVENTORY.md`: the inventory of code, binaries, geometry, material data and notices, and eight
decisions for the owner. No licence was assigned.

## Remaining external dependencies

- Real AI-client sessions: done 2026-09-17 (see above). Rerunning after any change to findings 1–5 needs the owner's
  login and about USD 25–30 for the six cases with three repeats.
- Independent users: the owner recruits participants and obtains consent (`user-test-kit/README.md`).
- Physical measurements: specimens, fixture and instruments (`physical/PROTOCOL.md`).
- Licensing and distribution decisions: `../DISTRIBUTION_INVENTORY.md`, section 3.

## Disposable evaluation artefacts removed

- `solver_tolerance` runs (the four tolerance-check analyses at 1 and 0.5 mm): results.nvr removed after their
  quantities were recorded, freeing 541 MB. spec.json and summary.json were kept, and the file hashes before removal
  are in `audit/solver_tolerance.json`. These were evaluation outputs, not baseline evidence.
- `scratchpad/eval/x1_b1_iter/`: a manual trial of the iterative CalculiX solve, 17 MB. It was repeated by `x1_ccx.py`,
  whose retained copy is `independent/x1/B_1_C3D8I_iterative/`.
- CalculiX working files in `independent/x1/*` and `x2/*`: `.frd`, `.12d`, `.cvg`, and for X1 the full `.dat`. They were
  deleted by the scripts after extraction; the SHA-256 of each full deck and `.dat` is kept in `SHA256.json`.
- `scratchpad/example-dev/` (68 MB): the development-build run of the bracket example. It was superseded by the packaged
  expected output, which is bitwise identical in its quantities.
- `scratchpad/task-ref/` (240 MB): the three reference runs of the user-test task. Their summaries and report are
  kept in `user-test-kit/facilitator/`.
- Temporary folders of the scripted flow runs, which `tempfile` had already emptied.
- `scratchpad/eval/audit/four_levels/` (588 MB) and `scratchpad/eval/independent/{cantilevers,t1_nu0,t2_t3}/` (230 MB):
  the NAVIER studies behind the audit, X1, X2 and T1–T3. They were removed after this record was complete, to free
  disk space (1.5 GB free). Kept: their evidence, reports, definitions, operation logs and run specifications (in
  `audit/` and `independent/navier_studies/`), and the SHA-256 of every field file
  (`independent/navier_studies/field_files.sha256`). They can be regenerated with the commands above.
- `scratchpad/navier-ai-eval-*` (about 1.5 GB): the NAVIER workspaces of the AI-client sessions, which hold the studies
  the model ran. The sanitized and the raw transcripts were removed from the repository on 2026-09-27, before publication.
