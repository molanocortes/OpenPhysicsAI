# `src/ctl/` — engine, operations, projects, jobs, studies

The headless application core and the largest module (53 files, ~19,800 lines). Everything an agent, the command line
or the app can do arrives here as one of the **50 typed operations** of `ops_schema.json` (contract 0.6.0), or one of the 25
of mechanics merged in from `src/mech/mech_ops_schema.json`.

Up: [module map](../../docs/map/MODULES.md#srcctl--engine-operations-projects-jobs-studies) ·
request path: [DATAFLOW.md](../../docs/map/DATAFLOW.md) · [AGENTS.md](../../AGENTS.md).

## Read first

| File | What it gives you |
|---|---|
| `ops.h` / `ops.c` | the registry and `ops_invoke`: schema validation with defaults, the engine lock, idempotent replay, revision checks, dispatch |
| `ops_schema.json` | **the contract itself** — every operation, its input schema, its title and description. Embedded into each binary at build time |
| `engine.h` / `engine.c` | engine lifetime, access roots (nothing outside them can be read or written), revisions, change journal |
| `project.h` / `project.c` | the persisted project: bodies with units and placement, patches, selections, setup entries, `project.json` |
| `study.h` + `study_*.c` | comparison studies and the Engineering Evidence Record — the seed of the [certificate](../../docs/design/certificate.md) |

## The rest, by job

- **Handlers** (one file per area, each with an `OPS_*_BINDINGS` table):
  `ops_project.c`, `ops_geometry.c`, `ops_surface.c`, `ops_view.c`, `ops_setup.c`, `ops_mesh.c`, `ops_contact.c`,
  `ops_analysis.c`, `ops_results.c`, `ops_study.c`, and `ops_transient.c` (no bindings of its own: it serves the
  transient side of `analysis_run`, `job_*` and `results_*`).
- **Static structural:** `static_model.c` (assembly + validation), `static_run.c` (the job), `static_io.c`
  (`results.nvr`, VTU, CSV), `static_quantities.c` (engineering quantities, each with one stated definition).
- **Transient thermal:** `thermal_model.c`, `thermal_run.c`, `thermal_io.c` (`results.nvt`), `thermal_checkpoint.c`
  (durable checkpoints, bit-for-bit resume), `contact.c` (thermal interfaces).
- **Selection and setup:** `selection.c` (queries, hashing, revalidation against a geometry revision), `setup.c`,
  `matlib.c` (material records with provenance; the built-in library is **demonstration data**, labelled as such).
- **Views:** `views.c` (camera cache, ray picking), `viewrender.c` (project and field images).
- **Jobs:** `jobs.c` — one worker thread, queue, progress, cancel, pause, resume.
- **Command line without a server:** `cli_local.c` — `navier-ctl study …` and `navier-ctl doctor`.

## Talks to

- [`net/`](../net/README.md) and [`bin/`](../bin/README.md) — the transports that call `ops_invoke`.
- [`fem/`](../fem/README.md) — the solvers a job runs.
- [`geom/`](../geom/README.md) — geometry, patches, the hex mesh.
- [`render/`](../render/README.md) — images for `view_render`, `selection_preview`, `results_render`.
- [`../fembridge.c`](../README.md) — the app, through the same operations.

## Tests

```bash
make headless
./build/opstest ./build/ctltest ./build/amtest ./build/evaltest
python3 tools/mcptest.py       # protocol level
python3 tools/amflow.py        # the complete structural path over MCP
python3 tools/studyflow.py     # comparison studies and the evidence record
./navier-ctl doctor            # self-diagnosis including a reference solve
```

## Rules this module enforces

1. **Never guess.** A missing unit, material source or mounting is an error or a blocking question, never a default
   that changes the answer. Defaults that do not change the ranking become recorded assumptions with their provenance.
2. **Everything is hashed.** Run specifications, geometry and evidence records carry SHA-256 so a result can be traced
   and replayed.
3. **Results are immutable** once written, which is why result operations run without the engine lock.
4. **No strength or safety statement** is produced anywhere in this module; see
   [../../docs/design/certificate.md](../../docs/design/certificate.md) for what a graded certificate would need.
