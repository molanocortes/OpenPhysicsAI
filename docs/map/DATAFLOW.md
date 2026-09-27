# Data flow: how a request becomes a result

One request, from a client to an answer, with the file that implements each hop. Read from the code on 2026-09-17;
line numbers are where the hop is visible, not where the whole hop lives.

Entry point: [../../AGENTS.md](../../AGENTS.md) · modules: [MODULES.md](MODULES.md) · history: [HISTORY.md](HISTORY.md).

## The short version

```
client ──► transport ──► operation registry ──► engine + handler ──► job queue ──► solver
                                                                          │
   evidence record ◄── study layer ◄── result files ◄───────────────────── ┘
                                            │
                            queries, probes, images, exports
```

**One dispatcher for every client.** MCP, the socket, `navier-ctl` and the app's buttons all end in the same
`ops_invoke`. Nothing the interface can do is unreachable for an agent, and the other way round.

## 1. Client to transport

| Client | Binary | Transport | File |
|---|---|---|---|
| AI client (MCP host) | `navier-mcp` | newline-delimited JSON-RPC on stdio | `src/bin/navier_mcp.c` → `mcp_handle_text` (`src/net/mcp.c:284` `tools/list`, `tools/call`) |
| Shell / script | `navier-ctl` | Unix or TCP control socket | `src/bin/navier_ctl.c` → `ctl_request` (`src/net/ctlclient.c`) |
| Shell, no server | `navier-ctl --embedded` | in-process engine | `src/ctl/cli_local.c:cli_local_main` |
| Long-running engine | `navier-server` | listens on the socket | `src/bin/navier_server.c` → `ctl_server_start` (`src/net/ctlserver.c`) |
| The app | `./navier` | in-process engine | `src/fembridge.c:294` calls `ops_invoke` directly |

MCP `tools/call` unwraps to an operation name and arguments in `src/bin/navier_mcp.c:49`; the socket does the same in
`src/net/ctlserver.c:195-203` after `ops_find(method)`.

## 2. Operation registry and validation — `src/ctl/ops.c`

`ops_registry_init` builds the registry from `src/ctl/ops_schema.json`, which `tools/embed.c` turned into a C array at
build time (`Makefile`: `build/gen/ops_schema.c`), so every binary carries the contract it serves
(`ops_contract_version()` → **0.6.0**, 50 operations; the 25 of `src/mech/mech_ops_schema.json` are merged in at start, 75 in all over MCP, counted 2026-09-27).

`ops_invoke` (`src/ctl/ops.c:319`) then, in order:

1. finds the operation, else `UNKNOWN_OPERATION`;
2. validates the parameters against the operation's JSON Schema and **applies defaults** —
   `jschema_validate` (`src/core/jschema.c`), `src/ctl/ops.c:354`;
3. takes the engine lock unless the operation declares `engine_lock: false` (jobs and results do their own locking,
   `src/ctl/ops.c:371`);
4. replays an identical earlier call when an `idempotency_key` repeats, and refuses a reused key with different
   parameters (`src/ctl/ops.c:382-398`);
5. checks `expected_revision` against `engine_revision` for mutating operations (`src/ctl/engine.c`);
6. calls the handler and returns an `OpResult`: `ok` with a value, or a typed error from `src/core/errors.h`
   (`NOT_FOUND`, `UNITS_REQUIRED`, `INSUFFICIENT_CONSTRAINTS`, …) with a hint and, where useful, attached images.

Handlers are bound in eleven tables (`OPS_*_BINDINGS`, see [MODULES.md](MODULES.md#operations-by-handler-file-all-45-from-ops_schemajson-and-the-ops__bindings-tables)).

## 3. Engine and project state — `src/ctl/engine.c`, `src/ctl/project.c`

The `Engine` holds the open project, the job manager, the access roots and a revision counter with a change journal.
Every path a client supplies is resolved and confined to a root (`engine_resolve_read_path` /
`engine_resolve_write_path` → `path_within`, `src/core/paths.c`); a path outside them is `PERMISSION_DENIED`.

A project is a directory: `project.json` (bodies with units and placement, patches, selections, materials, boundary
conditions, each entry carrying its provenance), the imported geometry, and `runs/<job_id>/` for each analysis
(`src/ctl/project.c:999` writes `project.json`).

## 4. Geometry, selection, mesh

| Step | Operation | Implementation |
|---|---|---|
| import an STL, check it | `geometry_import`, `geometry_diagnostics` | `src/ctl/ops_geometry.c` → `mesh_load_stl`, `surface_build`, `bvh_build` (`src/geom/`) |
| place it in the build frame | `geometry_place` | `src/ctl/ops_geometry.c` |
| list addressable faces | `surfaces_list` | `patches_build` (`src/geom/patches.c`) |
| name a face set | `selection_create`, `selection_preview` | `src/ctl/selection.c` (query evaluation, hashing, revalidation) + `src/ctl/viewrender.c` for the image |
| build the volume mesh | `mesh_generate`, `mesh_inspect` | `hexmesh_generate` (`src/geom/hexmesh.c`): layer-aligned voxel hex8 mesh, boundary faces, staircase statistics |

Selections are stored with the geometry revision they were resolved against and revalidated when the geometry changes
(`selection_revalidate`), so a stale selection is refused rather than silently reinterpreted.

## 5. Setup and validation

`material_define` / `material_assign` (`src/ctl/ops_setup.c`, records and library in `src/ctl/matlib.c`, built-in
library embedded from `src/ctl/materials.json`), `boundary_apply` (supports, forces, pressures, gravity, thermal
conditions, each with a `Provenance`), `interface_define` for thermal contact (`src/ctl/contact.c`).

`setup_validate` (`src/ctl/ops_analysis.c`) builds the model without solving: `static_model_build`
(`src/ctl/static_model.c`) → `solid_check_constraints` (`src/fem/solid.c`) reports rigid-body motion as
`INSUFFICIENT_CONSTRAINTS` **before** any solve.

## 6. Submission to the job queue — `src/ctl/ops_analysis.c`, `src/ctl/jobs.c`

`analysis_run`:

1. builds the model and the run specification, hashes it (`spec_hash` covers every field except `job_id`, `created`,
   `project_revision`, `spec_hash` — `src/ctl/ops_analysis.c:181`);
2. creates `runs/<job_id>/` and writes the immutable `spec.json` there (`:184`);
3. submits a `JobSpec` with the run function `static_job_run` (`:196`);
4. returns `job_id`, `run_directory`, the model summary, warnings and a `next_step` string at once — it never blocks.

`src/ctl/jobs.c` runs one worker thread in submission order (each solve already uses every core through
`src/threads.c`). `job_status` reports state, progress and the summary; `job_cancel`, `job_pause`, `job_resume`,
`job_checkpoint` act on a running job; `jobs_find_active` deduplicates identical specifications.

## 7. Solve

**Static structural** — `src/ctl/static_run.c`:
`static_model_build` → assemble (`hex8_stiffness`, `src/fem/hex8.c`) → `solid_solve` (`src/fem/solid.c:309`,
sparse Cholesky or PCG from `src/fem/sparse.c`) → stress recovery (`:327`) → write results (`:336`) → summary with the
equilibrium, energy and singularity checks (`job_set_summary`, `:361`).

**Transient thermal / thermomechanical** — `src/ctl/thermal_run.c`:
`thermal_model.c` builds the case → the shared `orch_step` loop (`src/fem/orchestrator.c`) drives the transactional
integrator (`src/fem/thermal_integrator.c`) with the event schedule and PI controller (`src/fem/timestep.c`) →
frames and checkpoints through `src/ctl/thermal_checkpoint.c`.

**Fluid** — the lattice Boltzmann kernel `src/lbm.c` runs in the app's own solver thread (`src/sim.c`); the headless
path uses it only through `src/fem/flow.c` for a steady field in conjugate heat transfer.

## 8. Result files

| File | Written by | Holds |
|---|---|---|
| `runs/<job>/spec.json` | `src/ctl/ops_analysis.c:184` | the immutable, hashed specification |
| `runs/<job>/results.nvr` | `src/ctl/static_io.c` | JSON header + little-endian arrays: coordinates, connectivity, supports, loads, displacement, reaction, nodal and Gauss stress, von Mises |
| `runs/<job>/results.nvt` | `src/ctl/thermal_io.c` | transient frames (+ `.pvd` series on export) |
| `runs/<job>/summary.json` | the job summary | model size, solver, checks, warnings |
| checkpoints | `src/ctl/thermal_checkpoint.c` | bit-for-bit resume of a transient run |

Results are immutable once written, which is why the result operations run without the engine lock.

## 9. Queries, images, exports — `src/ctl/ops_results.c`

`results_query` (fields and statistics), `results_quantities` (engineering quantities, each with one stated
definition, `src/ctl/static_quantities.c`), `results_probe` (points), `results_render` (PNG through
`src/ctl/viewrender.c` → `src/render/swrender.c`), `results_export` (VTU / CSV, `src/ctl/static_io.c`),
`results_interface` (thermal interface fluxes).

A result not in this session is loaded from its run directory on demand (`src/ctl/ops_results.c:37`
`static_results_load`, then `jobs_attach`), so an agent can query a run made by another process.

## 10. Comparison study and the Engineering Evidence Record — `src/ctl/study*.c`

`study_check` resolves a definition without solving: missing information that changes the answer becomes a **blocking
question**, a default that does not becomes a recorded **assumption**, and each carries its provenance
(`src/ctl/study_resolve.c`). `study_run` writes the study directory and queues one job (`src/ctl/study.c`); the job
(`src/ctl/study_run.c`) gives every design its own private engine (`src/ctl/study_design.c`) and runs the ordinary
operations on it — the same 45 an agent would call, logged to `operations.jsonl`.

`src/ctl/study_evidence.c` then writes `evidence.json` (format 2): per design and mesh the quantities and checks, the
refinement conclusions (ranking per tested mesh, the convergence criterion, and a discretisation-error estimate only
when its conditions hold), the comparison outcome, and the reproduction section with hashes.
`src/ctl/study_report.c` renders `report.md` from that file alone. `study_replay` re-runs from `study.json` and the
stored, hashed geometry and compares every quantity.

This record is the seed of the [certificate](../design/certificate.md).

## 11. The app takes the same path

`src/fembridge.c` mirrors engine state for the interface and calls `ops_invoke` (`:294`) for every action, polling
`job_status` (`:385`) exactly as a remote client does. The in-app terminal (`src/commands.c`) drives the same bridge.

## Where a request can stop, by design

| Stop | Where | Error |
|---|---|---|
| parameters do not match the schema | `ops.c` step 2 | `INVALID_PARAMS` with the failing pointer |
| a unit or a material source is missing | handlers, `src/core/units.c` | `UNITS_REQUIRED`, or a blocking question in a study |
| the path is outside the allowed roots | `engine_resolve_*_path` | `PERMISSION_DENIED` |
| the part is not held | `solid_check_constraints` before solving | `INSUFFICIENT_CONSTRAINTS` |
| the mesh cannot represent the design | level validity in `src/ctl/study_run.c` | the level is marked invalid, the study says why |
| the deformation leaves the linear range | `src/ctl/static_quantities.c` classification | reported; a study outcome becomes `cannot_establish` |
| storage is short | `src/ctl/study_storage.c` | `RESOURCE_LIMIT` before the run, `INSUFFICIENT_STORAGE` during it |
