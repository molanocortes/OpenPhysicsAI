# Index: every document in the repository

One line per document, so that nothing is reachable only by knowing it exists. `tools/linkcheck.py` fails if a
markdown file is not reachable from [../../AGENTS.md](../../AGENTS.md), and this index is where documents that belong
to no other page are linked.

Map: [MODULES.md](MODULES.md) · [DATAFLOW.md](DATAFLOW.md) · [HISTORY.md](HISTORY.md) · [GEMS.md](GEMS.md) ·
design: [certificate.md](../design/certificate.md).

## Start here

| Document | What it is |
|---|---|
| [AGENTS.md](../../AGENTS.md) | the entry point for agents: build, drive, understand, the rules of the house |
| [README.md](../../README.md) | the front page for humans, with renders of each environment |
| [VISION.md](../../VISION.md) | the owner's stated intent — the source of truth when documents disagree |
| [STATUS.md](../../STATUS.md) | the project checkpoint: milestones, what was tested, known failures, next tasks |

## The flags and the physics lab

| Document | What it is |
|---|---|
| [README.md](../../README.md) | the front page: the thirteen flags, their boards, the hall of fame |
| [flags/README.md](../../flags/README.md) | how the flags work: tiers, credit, entries, rules, scoring; [proposing a flag](../../flags/CONTRIBUTING-FLAGS.md) |
| [flags/LEADERBOARD.md](../../flags/LEADERBOARD.md) | every board in one page, drawn by `tools/flags.py board` |
| the thirteen flags | [01 car wake](../../flags/car-wake/FLAG.md), [02 re-entry](../../flags/reentry-fire-ii/FLAG.md), [03 jet flame](../../flags/jet-flame/FLAG.md), [04 splash](../../flags/drop-splash/FLAG.md), [05 metal printing](../../flags/metal-printing/FLAG.md), [06 fracture](../../flags/metal-fracture/FLAG.md), [07 radar](../../flags/radar-almond/FLAG.md), [08 spark](../../flags/spark-streamer/FLAG.md), [09 Apophis](../../flags/apophis-2029/FLAG.md), [10 corona](../../flags/corona-2027/FLAG.md), [11 fusion](../../flags/fusion-shot/FLAG.md), [12 hurricane](../../flags/hurricane-otis/FLAG.md), [13 heartbeat](../../flags/heartbeat/FLAG.md) |
| the trials | [T1 cylinder shedding](../../flags/flow-cylinder-shedding/FLAG.md), [T2 the bow shock ahead of a sphere](../../flags/gas-sphere-bowshock/FLAG.md), [T3 a shock through a bubble of gas](../../flags/gas-shock-bubble/FLAG.md), [T4 planet positions](../../flags/orbit-planets-2020/FLAG.md) |
| [docs/map/PHYSICS.md](PHYSICS.md) | the physics map: every solver, what it computes, how it was verified, what to read, which scenarios run it |
| [docs/LAB.md](../LAB.md) | the lab in pictures, and the verification record |
| [llms.txt](../../llms.txt) | the one-page summary for language models |
| [GOALS.md](../../GOALS.md) | the programme the architect works through, goal by goal |
| [docs/lab/README.md](../lab/README.md) | how to run the lab's solvers; one page per domain |
| [src/lab/README.md](../../src/lab/README.md) | the code of the lab and its tests |

## Using and driving it

| Document | What it is |
|---|---|
| [docs/app/tet-comparison.md](../app/tet-comparison.md) | voxels against quadratic tetrahedra on the same bracket (formerly linked from the front page) |

| Document | What it is |
|---|---|
| [docs/analysis.md](../analysis.md) | a structural and thermal analysis end to end |
| [docs/water-tunnel.md](../water-tunnel.md) | the fluid tunnel: the app, its terminal commands and its benchmarks |
| [docs/mcp-clients.md](../mcp-clients.md) | connecting AI clients (Claude Code, Codex) to the MCP server |
| [docs/control-protocol.md](../control-protocol.md) | the control socket protocol |

## The release documentation set (shipped inside the package)

| Document | What it is |
|---|---|
| [docs/release/README.md](../release/README.md) | the package's own front page |
| [docs/release/INSTALL.md](../release/INSTALL.md) | installing and verifying the package |
| [docs/release/WORKFLOW.md](../release/WORKFLOW.md) | the comparison workflow and its outcome codes |
| [docs/release/EVIDENCE.md](../release/EVIDENCE.md) | what is checked, how refinement is reported, the reference cases |
| [docs/release/ASSUMPTIONS.md](../release/ASSUMPTIONS.md) | material and manufacturing assumptions, and how sources are recorded |
| [docs/release/LIMITATIONS.md](../release/LIMITATIONS.md) | the known limits, including the AI-client verdict |
| [docs/release/AI_CLIENTS.md](../release/AI_CLIENTS.md) | driving the release from an AI client, and what to check in its answer |
| [docs/release/REFERENCE_CASES.md](../release/REFERENCE_CASES.md) | reproducing the reference and evaluation cases |
| [docs/release/ADDING_A_TEST.md](../release/ADDING_A_TEST.md) | where a new test case goes |
| [docs/release/REPORTING_FAILURES.md](../release/REPORTING_FAILURES.md) | what a useful failure report contains |
| [docs/release/NOTICE.md](../release/NOTICE.md) | licence state, third-party status, data provenance |

## The map and the design

| Document | What it is |
|---|---|
| [MODULES.md](MODULES.md) | every directory: purpose, key files, entry points, tests, state |
| [DATAFLOW.md](DATAFLOW.md) | how a request becomes a result, with the file for each hop |
| [HISTORY.md](HISTORY.md) | what was built when, by which effort, and what was abandoned |
| [GEMS.md](GEMS.md) | working things that are linked from almost nowhere |
| [certificate.md](../design/certificate.md) | the graded engineering certificate: design proposal |
| [src/README.md](../../src/README.md) | the fluid tunnel and the app |
| [src/core/README.md](../../src/core/README.md) · [src/geom/README.md](../../src/geom/README.md) · [src/fem/README.md](../../src/fem/README.md) | foundations, geometry, solvers |
| [src/ctl/README.md](../../src/ctl/README.md) · [src/net/README.md](../../src/net/README.md) · [src/render/README.md](../../src/render/README.md) · [src/bin/README.md](../../src/bin/README.md) | engine and operations, transports, images, executables |

## Thermal module

| Document | What it is |
|---|---|
| [Thermal Sim/README.md](../../Thermal%20Sim/README.md) | what the thermal module is and where its code lives |
| [Thermal Sim/STATUS.md](../../Thermal%20Sim/STATUS.md) | its checkpoint: stages A–E, suite counts, gaps |
| [Thermal Sim/docs/theory.md](../../Thermal%20Sim/docs/theory.md) | governing equations, discretisation, declared unsupported regimes |
| [Thermal Sim/docs/capability-matrix.md](../../Thermal%20Sim/docs/capability-matrix.md) | every feature marked unavailable / experimental / verified / validated |
| [Thermal Sim/docs/audit.md](../../Thermal%20Sim/docs/audit.md) | the 2026-09-16 baseline audit |
| [Thermal Sim/verification/README.md](../../Thermal%20Sim/verification/README.md) | the verification logs |
| Stage criteria and evidence, fixed before each run: [A criteria](../../Thermal%20Sim/verification/stageA-criteria.md) · [A evidence](../../Thermal%20Sim/verification/stageA-evidence.md) · [B criteria](../../Thermal%20Sim/verification/stageB-criteria.md) · [B evidence](../../Thermal%20Sim/verification/stageB-evidence.md) · [C criteria](../../Thermal%20Sim/verification/stageC-criteria.md) · [C evidence](../../Thermal%20Sim/verification/stageC-evidence.md) · [D criteria](../../Thermal%20Sim/verification/stageD-criteria.md) · [D evidence](../../Thermal%20Sim/verification/stageD-evidence.md) · [E criteria](../../Thermal%20Sim/verification/stageE-criteria.md) · [E evidence](../../Thermal%20Sim/verification/stageE-evidence.md) | pre-registered criteria and what was observed |
| Demonstration studies: [01 heated bar](../../Thermal%20Sim/examples/01_heated_bar_convection/README.md) · [02 thermal expansion](../../Thermal%20Sim/examples/02_thermal_expansion/README.md) · [03 melting block](../../Thermal%20Sim/examples/03_melting_block/README.md) · [04 three-domain](../../Thermal%20Sim/examples/04_three_domain_cht/README.md) | each with its analytical reference and limitations |

## Validation: the result format and the overnight queue

| Document | What it is |
|---|---|
| [validation/RESULT_FORMAT.md](../../validation/RESULT_FORMAT.md) | the one-JSON-per-run contract between a solver and the challenge scorer |
| [validation/QUEUE.md](../../validation/QUEUE.md) | the overnight queue: long runs a session must not wait for, each with the number it exists to answer |

## Examples

| Document | What it is |
|---|---|
| [examples/bracket_comparison/README.md](../../examples/bracket_comparison/README.md) | the bracket comparison example and what to expect from it |
| [expected_output/report.md](../../examples/bracket_comparison/expected_output/report.md) | the evidence record the packaged example must reproduce bitwise |

## Release, readiness and distribution

| Document | What it is |
|---|---|
| [release/READINESS.md](../../release/READINESS.md) | the rc1 readiness assessment (superseded in parts by the evaluation) |
| [release/GAP_ASSESSMENT.md](../../release/GAP_ASSESSMENT.md) | the gaps found before the release |
| [release/DISTRIBUTION_INVENTORY.md](../../release/DISTRIBUTION_INVENTORY.md) | every item to be distributed, and the eight open licensing decisions |
| [release/reference_cases/CRITERIA.md](../../release/reference_cases/CRITERIA.md) | the reference cases' acceptance criteria |

## The independent evaluation (read-only; this branch does not modify it)

| Document | What it is |
|---|---|
| [EVALUATION_REPORT.md](../../release/evaluation/EVALUATION_REPORT.md) | the conclusions and the release recommendation |
| [EVALUATION_RECORD.md](../../release/evaluation/EVALUATION_RECORD.md) | the running record: baseline, defects, fixes, artefact identity |
| [CRITERIA-independent.md](../../release/evaluation/CRITERIA-independent.md) | criteria fixed before the CalculiX comparison and the beam tests |
| [CASES.md](../../release/evaluation/CASES.md) | the evaluation set E1–E13 and its amendments |
| [audit/four_levels_report.md](../../release/evaluation/audit/four_levels_report.md) | the four-mesh refinement audit record |
| CalculiX comparison records: [cantilevers](../../release/evaluation/independent/navier_studies/cantilevers/report.md) · [t1_nu0](../../release/evaluation/independent/navier_studies/t1_nu0/report.md) · [t2_t3](../../release/evaluation/independent/navier_studies/t2_t3/report.md) | the NAVIER runs behind X1, X2 and the beam tests |
| [ai-client/RESULTS.md](../../release/evaluation/ai-client/RESULTS.md) | the 18 graded real-client sessions and the five open findings |
| [ai-client/RUBRIC.md](../../release/evaluation/ai-client/RUBRIC.md) | the grading rubric, fixed before the sessions |
| The case prompts given to the model: [E1](../../release/evaluation/ai-client/prompts/E1.md) · [E2](../../release/evaluation/ai-client/prompts/E2.md) · [E3](../../release/evaluation/ai-client/prompts/E3.md) · [E4](../../release/evaluation/ai-client/prompts/E4.md) · [E5](../../release/evaluation/ai-client/prompts/E5.md) · [E6](../../release/evaluation/ai-client/prompts/E6.md) · [E7](../../release/evaluation/ai-client/prompts/E7.md) · [E8](../../release/evaluation/ai-client/prompts/E8.md) · [E9](../../release/evaluation/ai-client/prompts/E9.md) · [E10](../../release/evaluation/ai-client/prompts/E10.md) | one user request per case, verbatim |
| [physical/PROTOCOL.md](../../release/evaluation/physical/PROTOCOL.md) | the physical comparison protocol; no measurement exists yet |
| User-test kit: [README](../../release/evaluation/user-test-kit/README.md) · [QUICK_START](../../release/evaluation/user-test-kit/QUICK_START.md) · [TASK](../../release/evaluation/user-test-kit/TASK.md) · [EXPECTED_ARTIFACTS](../../release/evaluation/user-test-kit/EXPECTED_ARTIFACTS.md) · [FEEDBACK_FORM](../../release/evaluation/user-test-kit/FEEDBACK_FORM.md) · [DIAGNOSTICS](../../release/evaluation/user-test-kit/DIAGNOSTICS.md) | everything a participant needs |
| Facilitator only: [REFERENCE](../../release/evaluation/user-test-kit/facilitator/REFERENCE.md) · [reference_report](../../release/evaluation/user-test-kit/facilitator/reference_report.md) | the developer's own run and the grading checklist — not for participants |

## Challenges, contributions and publication

| Document | What it is |
|---|---|
| [challenges/README.md](../../challenges/README.md) | the challenges: rules, list, how to enter |
| [validation/contrib/README.md](../../validation/contrib/README.md) | how to contribute a measurement |
| [CONTRIBUTING.md](../../CONTRIBUTING.md) | contribution rules for people and agents |
| [.github/pull_request_template.md](../../.github/pull_request_template.md) | what a pull request says; a flag entry needs nothing, it is scored by a machine |
| [TASKS.md](../../TASKS.md) | open tasks as prompts with acceptance tests |
| [challenges/agents/README.md](../../challenges/agents/README.md) | the agent leaderboard: which model solved which task |
| [challenges/agents/AGENT_LEADERBOARD.md](../../challenges/agents/AGENT_LEADERBOARD.md) | its board, generated |
| [challenges/nist-am-bench/CHALLENGE.md](../../challenges/nist-am-bench/CHALLENGE.md) | the NIST AM-Bench import, defined |
| [kit/README.md](../../kit/README.md) | the physical measurement kit |
| [docs/design/ROADMAP-SOLVERS.md](../../docs/design/ROADMAP-SOLVERS.md) | the solver roadmap: conforming mesh, large deformation, compressible flow, assemblies, topology optimisation, presentation |
| [materials/README.md](../../materials/README.md) | the materials library: sources, batches, the rule |
| [docs/release/LAUNCH.md](../../docs/release/LAUNCH.md) | the launch checklist |

## Mechanics layer documents (merged 2026-09-18)

| Document | What |
|---|---|
| [Mech Sim/INTEGRATION_CONTRACT.md](../../Mech%20Sim/INTEGRATION_CONTRACT.md) | the four shared-file hooks and the merge rules |
| [Mech Sim/docs/assembly-and-study-formats.md](../../Mech%20Sim/docs/assembly-and-study-formats.md) | native assembly and study file formats |
| [Mech Sim/docs/backend-decision.md](../../Mech%20Sim/docs/backend-decision.md) | why a native C multibody core instead of MuJoCo |
