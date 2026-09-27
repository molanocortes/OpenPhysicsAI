# Mechanics layer: integration contract

Owner: the robotics/mechanics session. Counterpart: the thermal session, which owns adaptive thermal stepping,
rejection and rollback, durable checkpoints, thermal contact, fluid energy transport, conjugate heat transfer, and
the **shared multiphysics orchestrator**. This file changes only by addition or dated amendment.

Base: snapshot 2026-09-16T12:15:12+02:00 (`baseline/SNAPSHOT.md`). **Amended 2026-09-16:** re-baselined onto the
snapshot of 2026-09-16T16:35:48+02:00, taken after the thermal session's Stages C–E (`baseline/REBASELINE_2.md`). Base
hashes of the consumed interfaces are in `baseline/consumed_interfaces.sha256`; the first base is kept in
`baseline/2026-09-16T1215/`. Run `tools/drift_check.sh` before every merge.
**Amended 2026-09-27:** `baseline/` and `tools/drift_check.sh` were removed from the repository before publication
(the layer has been merged since 2026-09-18); the drift-check rules in this file no longer apply.

## 1. New modules owned by the mechanics session

| Path | Content |
|---|---|
| `src/mech/**` | math and spatial algebra, mass properties, assembly model and native format, URDF subset import, rigid multibody dynamics, joints/constraints, contact, actuators, transmissions, controllers, sensors, load transfer to FEM, orthotropic elasticity, modal/transient structural response, flexible bodies, MCP handlers (`mech_ops.c`), the operation schema fragment (`mech_ops_schema.json`) and the build fragment (`mech.mk`) |
| `tools/mechtest.c` | numerical verification (analytic references) |
| `tools/mechflow.py` | end-to-end MCP workflow test using an independent client |
| `Mech Sim/**` | status, this contract, decisions, verification reports, study specs, demonstrations |

Build outputs use distinct names: `build/mechtest`, `build/obj/mech/*`, `build/gen/mech_ops_schema.c`. Test runs
write only into `mkdtemp` workspaces.

## 2. Shared interfaces consumed (read-only; base hashes recorded)

| Interface | Used for |
|---|---|
| `core/json.h`, `core/jschema.h`, `core/units.h`, `core/errors.h`, `core/sha256.h`, `core/paths.h` | native format, schemas, unit parsing (`quantity_from_json`), error codes, spec hashes, path checks |
| `ctl/ops.h`, `ctl/ops_internal.h`, `ctl/engine_internal.h` | handler signature, `op_fail`/`op_succeed`/`op_quantity`, `engine_touch` (revisions and journal), engine lock, path resolution |
| `ctl/project.h`, `ctl/setup.h` (`Provenance`, `Selection`), `ctl/matlib.h` | bodies and STL geometry, placement into the build frame, named selections as attachment regions, material records |
| `ctl/jobs.h` | background dynamics and assessment jobs, progress, cancellation, checkpoint requests |
| `fem/solid.h`, `fem/hex8.h`, `fem/sparse.h`, `fem/dense.h` | static structural assessment of load snapshots; later mass matrices and modal analysis |
| `geom/mesh.h`, `geom/bvh.h`, `geom/surface.h` | closed-surface checks, mass properties, collision geometry |
| `ctl/static_analysis.h` (added at re-baseline 2) | mesh and selection extraction for load assessments, the `static_structural` job and its results |
| `fem/orchestrator.h`, `fem/thermal_participant.h`, `fem/thermal_integrator.h`, `fem/timestep.h` (added at re-baseline 2) | the mechanics participant adapter: trial / commit / export / import lifecycle, stage-resolved interface fields |
| `render/swrender.h` (added at re-baseline 2) | headless images of mechanics results |

## 3. Proposed shared-file changes (additive; none applied to the shared checkout yet)

| File | Change | Size |
|---|---|---|
| `Makefile` | `-include src/mech/mech.mk` directly after the `CORE_OBJ :=` line (at re-baseline 2: after upstream's extended `CORE_OBJ` line, before the `GUI_OBJ` comment). `mech.mk` appends to `CORE_SRC`/`CORE_OBJ`, embeds the schema fragment, adds `build/mechtest` to `TESTS`, and adds `test: test-mech` as a prerequisite, so the existing `test` recipe is not edited | 1 line |
| `src/ctl/ops.c` | registry: merge the embedded fragment `NAVIER_MECH_OPS_SCHEMA` (`operations` + `$defs`, collisions are errors) into the root before validation; add `OPS_MECH_BINDINGS` to `ALL_BINDINGS` (after upstream's `OPS_CONTACT_BINDINGS`) | ~30 lines |
| `src/ctl/ops_internal.h` | `extern const OpBinding OPS_MECH_BINDINGS[];` | 1 line |
| `src/ctl/ops_project.c` | `capabilities_get`: `mech_capabilities_json(v)` adds a `mechanics` section (fidelity levels with status and assumptions); `project_open` / `project_save`: call `mech_project_opened` / `mech_project_saved` so `mechanics/assembly.json` is loaded and saved with the project | ~6 lines |

Until merged, the working copy carries exactly these changes, each marked `/* mech-integration */`. No other shared
file is edited. `STATUS.md` and `README.md` get one pointer section each at merge time, never a rewrite.

## 4. Schema additions

- **Fragment:** `src/mech/mech_ops_schema.json`, with its own `contract: "navier-mech"` and `contract_version`
  (0.1.0 at the first vertical slice; 0.2.0 adds contact: `mech_environment_define`, `collision` on
  `mech_body_define`, `settings.contact`, and `contact_shape` attachments on `mech_fem_assess`; all additive). The
  shared `navier-am` version is not bumped by mechanics work. `capabilities_get` reports both. **0.3.0** adds
  `mech_material_define`, `mech_structure_query` and `material_model` on `mech_fem_assess` (additive). **0.4.0** adds
  `mech_modal_run`, `mech_transient_assess` and `mech_vibration_query` (additive). **0.5.0** adds
  `mech_flexible_reduce`, `mech_flexible_attach`, `mech_flexible_detach`, `mech_flexible_stress` and `settings.flexible_initial_state`
  (additive; the `flexible` body block of the assembly format is new, and assemblies without it are unchanged).
- **Job kinds:** `mech_dynamics`, `mech_structural` (orthotropic assessments), `mech_modal`, `mech_transient` and
  `mech_flexible` (Craig-Bampton reductions) and `mech_flexible_stress` (stresses from coupled elastic coordinates) are
  mechanics-owned. The shared result
  operations accept only `static_structural` and refuse the others, which is the intended behaviour: their strain query
  assumes an isotropic material. Isotropic assessments keep the shared `static_structural` kind and all shared result
  operations.
- **Naming and routing:** all operation names start with `mech_`. Mutations go through the shared revision,
  idempotency and journal machinery.
- **Operation families**, added stage by stage:
  - assembly import and inspection
  - body properties
  - joints
  - collision configuration
  - actuators
  - sensors
  - controllers
  - trajectories
  - study validation
  - dynamics runs
  - load-history export
  - FEM assessment
  - modal analysis
  - flexible bodies (reduction, attach, detach)
  - parameter studies
  - result comparison
- **Persisted state:** `<project>/mechanics/assembly.json` holds the native format (`navier-assembly` v1). Run
  directories reuse `<project>/runs/<job_id>/` with a hashed `spec.json`.

## 5. Physics state exchanged

Every exchanged quantity carries: SI unit, frame, timestamp (simulation time, s), spatial association (body, joint,
selection or point), and sign convention. Frames are the right-handed build frame: metres, +Z up, gravity along −Z by
default. Quaternions are Hamilton (w, x, y, z), mapping body to parent. Spatial vectors are ordered [angular; linear].

| Exported by mechanics | Unit | Frame / association | Convention |
|---|---|---|---|
| Body pose and twist | m, rad/s, m/s | build frame; body frame origin | twist of the body frame origin |
| Joint position, velocity, effort | rad or m, rad/s or m/s, N·m or N | joint axis | positive about/along the joint axis |
| Joint reaction wrench | N, N·m | joint frame, at the joint origin | wrench applied **by the parent on the child** |
| Contact point, normal, gap, normal/tangential force or impulse, stick/slip | m, N or N·s | build frame | normal points from shape B to shape A; gap < 0 is penetration. Rows carry impulses (N·s) and the step (s); average forces (N) are published only for persistent contact, and impact steps are flagged `impact` / `contact_impulsive` and never exported as forces |
| Structural load snapshot | N/m² tractions on selections; N/m³ body force field | body frame at the snapshot time | d'Alembert inertial loads included; resultant and moment reported |
| Flexible body state | kg^½·m (mass-normalised elastic coordinates), m and rad (interface motion), J (strain energy) | body axes of the flexible body, relative to its reference frame held at the root region | first-order floating frame; interface motion is the translation of the interface point and the rotation vector of the rigid interface |
| Motor electrical losses, frictional heat | W | actuator body / contact region | **exported only by mechanics**; thermal must not recompute them (no double counting). Today they are integrated energies per study (J: copper, motor friction, gearbox, contact friction, contact impacts); per-step power per actuator and contact pair comes with the participant adapter |
| Runtime checkpoint | bytes (versioned `NVMSCKP1`) | whole mechanical state | includes controller, sensor (random generator) and contact solver warm-start state, so a restored checkpoint replays bitwise |

| Imported by mechanics | Unit | From | Use |
|---|---|---|---|
| Temperature | K | thermal (field sample at body/sensor locations) | temperature sensors; temperature-dependent material data |
| Fluid force and moment | N, N·m | fluid solver, one-way | external wrench at body frame origin (build frame) |
| Prescribed motion | m, rad (with time) | study spec | rheonomic constraints (the reaction is the required effort) |

## 6. Integration dependencies

| Dependency | Owner | Status at contract time | Mechanics side until ready |
|---|---|---|---|
| Shared multiphysics orchestrator (participant API) | thermal | **available** (its Stage D is done; `src/fem/orchestrator.h`, in the working copy since re-baseline 2) | `mech_participant.[ch]` implements the real `OrchParticipant` lifecycle (trial from the accepted state, commit, export, import). It is tested against the shared orchestrator itself, so no test double is needed. One-way coupling comes first; there are no FSI claims |
| Durable checkpoint format | thermal | Stage B done (thermal jobs) | mechanics checkpoints use their own versioned file in the run directory; converge on the shared format when it is generalised |
| Thermal contact through MCP, conjugate heat transfer | thermal | pending | export motor losses and frictional heat only |
| Temperature-dependent material records | shared material system (`matlib`) | isotropic reference-temperature data | orthotropic printed-material records proposed as an additive `matlib` extension, reviewed before merge |

## 7. Tests required before merging

1. At the merge revision, the full shared suite passes unchanged (`make test`, including the thermal session's
   latest flows), and so do `make navier` and `make test-ui`, because the Makefile changes.
2. `build/mechtest`: every verification case passes, with measured convergence orders.
3. `tools/mechflow.py`: the MCP vertical slice (assembly → joints → actuator → controller → dynamics → recorded
   loads → structural assessment → result query) runs through `navier-mcp` with an independent client.
4. The schema fragment passes `jschema_check`. `capabilities_get` lists only operations that have handlers, and
   the mechanics section matches what is reachable.
5. `tools/drift_check.sh` shows no shared-file differences other than section 3. Every file both sessions changed
   is merged hunk by hunk.

Merges are serial. If a merge is blocked, the working copy keeps tested, reviewable commits, and the blocker is
recorded in `Mech Sim/STATUS.md`.
