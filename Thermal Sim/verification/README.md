# Verification logs

Raw output of the runs quoted in `../docs/theory.md` and `../docs/capability-matrix.md`, captured on the machine
recorded in `../STATUS.md` (macOS 15.7.3, Apple M2, 8 GB, Apple clang 17.0.0).

| Log | Command | Checks |
|---|---|---|
| `thermtest.log` | `./build/thermtest` | 72 — analytical and manufactured solutions for the thermal core |
| `thermflow.log` | `python3 tools/thermflow.py` | 95 — the complete thermal workflow over MCP stdio |
| `femtest.log` | `./build/femtest` | 42 — structural FEM, including free and constrained thermal expansion |
| `amtest.log` | `./build/amtest` | 102 — the operation layer for static, transient thermal and thermomechanical runs |
| `tsteptest.log` | `./build/tsteptest` | 54 — adaptive time integration (Stage A) |
| `transientflow.log` | `python3 tools/transientflow.py` | 81 — adaptive settings, schedules and failures over MCP (Stage A) |
| `restartflow.log` | `python3 tools/restartflow.py` | 154 — checkpoint, pause, resume, SIGKILL, corruption (Stage B) |
| `contactflow.log` | `python3 tools/contactflow.py` | 104 — thermal contact over MCP (Stage C) |
| `orchtest.log` | `./build/orchtest` | 24 — the shared orchestrator, partitioned conduction (Stage D) |
| `advtest.log` | `./build/advtest` | 43 — nonsymmetric solver, SUPG advection, lattice Boltzmann flow, conjugate heat transfer in the core (Stage E) |
| `chtflow.log` | `python3 tools/chtflow.py` | 102 — the fluid–thermal–solid study over MCP (Stage E) |

Regenerate them with `make test` from the repository root, or individually with the commands above. For each stage of
the 2026-09-16 release, the acceptance criteria (`stageX-criteria.md`, written before the first run and amended only
with the observed values recorded) and the evidence tables (`stageX-evidence.md`) sit next to the logs.

Every case here is an **analytical or manufactured solution**, or a comparison between two numerical paths
(partitioned and monolithic, resumed and uninterrupted). None is a comparison against physical measurement, and every
library material, including `air_demo`, holds demonstration values. See `../docs/capability-matrix.md` for what that means for each
feature.
