# The comparison workflow

## 1. Write the study definition

A definition is a JSON object; its schema is the `definition` input of `study_check`. The example
`share/navier/examples/bracket_comparison/study.json` is complete. The parts:

| Field | Meaning | If missing |
|---|---|---|
| `question`, `decision` | the user's question, verbatim, and what they want to decide | `question` is required |
| `designs[]` | 2–4 designs: `geometry.path` (STL), `geometry.units`, `mounting_region`, `load_region` | the units become a blocking question; regions are required |
| `material` | a library `id` or a `record` (E, ν, density), with `source`: user, measured, database, inferred or default | blocking question |
| `manufacturing` | `process` (machined, cast, fff, sla, sls, mjf, lpbf, ...), orientation, infill, whether the values apply to that process | unspecified: recorded assumption. Additive without process-specific values: blocking question, which may be accepted |
| `mounting` | `fixed` (rigid clamp) or `frictionless_normal`, with its source | required; an idealisation that was not stated by the user and not assessed by an alternative is a blocking, acceptable question |
| `load` | `payload_mass` (mass, g) or `force`; `direction`; `self_weight` | a missing mass or force is a blocking question; g defaults to 9.80665 m/s² (recorded) |
| `refinement.element_sizes` | 2–4 voxel sizes, ideally halving | required |
| `sensitivity` | Young's-modulus factors, Poisson's ratios, mounting alternatives, self-weight | none assessed |
| `equivalence`, `limits`, `analysis`, `retain_results`, `accept`, `notes` | tolerances, element and time budgets, element and solver settings, disk use, accepted questions, stated notes | defaults |

**Frames.** Region queries and the load direction are in each design's frame: the STL file's own axes and origin,
lengths in mm. Designs are never rotated. The build frame used internally is only a translation, recorded per design.

**Regions** are selection queries (see the `selection_query` schema): `plane`, `facing`, `box`, `sphere`, `near`,
`type`, `cylinder`, `area`, `patches`, `adjacent_to`, and `all`/`any`/`not`. Use `mode: triangle` for part of a large
face.

## 2. Check

```bash
navier-ctl study check study.json          # or MCP: study_check {"file": ...} / {"definition": {...}}
```

The check imports every design into a temporary folder. It then:

- resolves the regions;
- checks equivalence against the first design: the lever arm from mounting centroid to load centroid, both region
  areas, and both normals;
- detects regions that span parallel planes;
- meshes the coarsest size to find unmapped regions and rigid-body motion;
- compares wall thickness with the element sizes.

It returns a `status`:

- **ready**: the study can run.
- **needs_input**: answer the blocking questions, or accept an acceptable one with the user's reason.
- **not_supported**: for example an open (non-watertight) STL.

## 3. Run

```bash
navier-ctl study run study.json --dir ~/NAVIER-Projects/my_study      # MCP: study_run, then job_status
```

The job runs every design at every element size, from coarse to fine, then the sensitivities at the chosen size. It
stops starting new analyses when a level would exceed `limits.max_elements`, the time budget is used up, or the
file system lacks the space for the next analysis's field; that is recorded as a failure with its recovery. Each analysis is a normal run in `designs/<name>/runs/<job_id>` with an
immutable, hashed `spec.json`.

## 4. Read the evidence

```bash
navier-ctl study report ~/NAVIER-Projects/my_study         # report.md
navier-ctl study evidence ~/NAVIER-Projects/my_study --full
```

Outcomes (`comparison.outcome`):

| Outcome | Meaning |
|---|---|
| `resolved` | every compared design has a discretisation-error estimate, and the difference between the best two exceeds the sum of their estimates. The statement says "lower predicted displacement under the modeled conditions", never "better" or "safe" |
| `ranking_consistent_on_tested_meshes` | a design has no estimate (for example a staircase chamfer whose shape changes with the mesh), but the ranking was the same on every tested mesh and the difference exceeds the designs' last changes between meshes. The size of the difference is not estimated beyond the tested meshes |
| `too_small_to_distinguish` | every design meets the convergence criterion, but the difference is within the estimates (or the last changes) |
| `not_resolved` | the difference is within the estimates or changes of designs that have not converged, the ranking changed between meshes, or a design has only one valid mesh; add a finer size |
| `more_refinement_required` | as above, and the planned refinement did not complete (element, time or storage budget) |
| `cannot_establish` | no mesh level is valid for every design (lost features, wrong mapping, failed checks), or the deformation is outside the small-deformation assumption |

Every record also reports, separately:
- the ranking on each tested mesh;
- each design's convergence criterion, met or not; a criterion that is not met stays in the statement;
- each design's discretisation-error estimate with its safety factor, or the reasons none is offered.

See `EVIDENCE.md`.

A level is **valid** only if all of these hold:

- force and moment balance and the energy identity close to 1e-6, and the solver converged;
- the load's line of action moved less than max(h/2, 1 % of the lever arm);
- the mounting region is on the mesh;
- the mesh is one connected region, and its volume is within 10 % of the geometry;
- the deformation is not outside the small-deformation assumption.

## 5. Replay

```bash
navier-ctl study replay ~/NAVIER-Projects/my_study           # MCP: study_replay
```

A replay reruns the study from `study.json`, `request.json` and the stored copies of the geometry (their SHA-256 is
verified), into `replays/<time>`. It then compares every quantity with the original: the outcome is `reproduced`
when all agree to a relative 1e-9, and bitwise identity is reported.

## Failure recovery

`study_run` refuses a study that needs input, returning the questions in `error.details`. A running study records
each failure with:

- `stage`: setup, mesh, validation, solve, quantities, schedule, storage or record;
- the design and element size;
- `failure_class`: invalid_input, unsupported_physics, numerical_failure, resource_exhaustion, io or cancelled;
- `code`, `message` and `recovery`;
- `partial_results_available`.

The failures appear in `job_status.error.details`, in `evidence.json` and in `report.md`. Completed levels are always
kept.

**Storage.**
- `study_check` returns `plan.storage` before anything runs:
  - the estimated retained and peak bytes for the chosen `retain_results`, at about 700 bytes per element plus 80 kB
    per analysis;
  - the free space;
  - whether it suffices, with 256 MB kept in reserve.

  When it does not suffice, the check warns (`INSUFFICIENT_STORAGE`) and `study_run` refuses with `RESOURCE_LIMIT`.
  Choose another directory, free space, or set `retain_results` to `none`.
- During the study, each analysis checks the free space first. If it is short, the study stops with
  `INSUFFICIENT_STORAGE` and keeps the completed levels.
- If the operations log cannot be opened, the study does not start.
- If `evidence.json` or `report.md` cannot be written, the job fails with `IO_ERROR`, names the file, and keeps the
  comparison in its error details.

**Retention.** `retain_results`:
- `refinement` (default) keeps each refinement level's field `results.nvr`;
- `all` also keeps the sensitivity runs;
- `none` keeps only quantities, checks and the SHA-256 of every field.

`evidence.json` → `reproduction.artifacts` lists what is retained as original, what is an extracted summary, and what
a replay regenerates. To inspect a failure, open the design's project: `project_open {"path": "<study>/designs/<name>"}`.

## Detailed operations

The study composes the ordinary operations, and `operations.jsonl` lists each one with its parameters. To inspect
or reproduce a step:

- `geometry_import`, `geometry_diagnostics`, `surfaces_list`, `selection_create` and `selection_preview`;
- `material_define` and `boundary_apply`, then `mesh_generate` and `mesh_inspect`;
- `setup_validate` and `analysis_run`;
- `results_quantities` (region-mean displacement, stiffness, mass, balance, deformation indicators), `results_query`
  and `results_render`.
