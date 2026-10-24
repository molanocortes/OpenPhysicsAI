# Rigid-multibody and contact backend decision

**Decision (2026-09-16):** implement a native C11 multibody core (`src/mech`) behind a narrow backend interface.
MuJoCo stays a **candidate cross-check backend**, not a dependency. Adding it later needs (1) your permission to
download its prebuilt release, (2) disclosure of the C++ parts it brings, and (3) results labelled with MuJoCo's
soft-contact regime. The FEM stays the structural solver; no soft-body approximation replaces it.

## Candidates

| | Native C core (this repo) | MuJoCo | ODE | RBDL / Pinocchio / Drake / Chrono |
|---|---|---|---|---|
| C API | yes (the application language) | yes: "C/C++ library with a C API" | yes | C++ APIs only |
| Implementation languages | C11 | C engine; model compiler, XML and mesh tooling in C++ with vendored third-party libraries (to be itemised if adopted) | C/C++ | C++ (Eigen, Boost, Bazel/CMake stacks) |
| License | project's own | Apache-2.0 (docs CC-BY-4.0) | BSD / LGPL | zlib / BSD |
| macOS arm64 + headless Linux | yes, same Makefile and `CORE_CFLAGS` | prebuilt: Linux x86-64 and AArch64, Windows x86-64, macOS universal | builds from source | yes, heavy toolchains |
| Build on this machine | yes (clang only) | no CMake here; prebuilt binary needs a download (needs permission) | needs a source download | no (no CMake/Bazel/Homebrew) |
| Joint constraints | exact bilateral constraints (Lagrange multipliers) with measured drift and explicit projection | soft equality constraints (solref/solimp regularisation) | ERP/CFM stabilisation (arbitrary stabilisation parameters) | exact (RBDL/Pinocchio: no contact) |
| Contact | primitives + controlled mesh approximations, rigid complementarity with reported impulses, penetration and friction state | "soft, convex and analytically-invertible", optimisation-based; penetration inherent; mesh geoms collide through convex hulls | LCP (Dantzig / QuickStep) | Drake: hydroelastic; Chrono: SMC/NSC |
| Reaction diagnostics | constraint rank, redundancy, non-uniqueness of reactions, energy ledger designed in | regularised solution is unique but reflects the regularisation, not structural compliance | limited | good in Drake |
| State serialisation / rollback | whole state is a flat, versioned struct; trial advance and rollback do not consume random samples twice | `mjData` state get/set; sensor noise and controller state are the application's job | manual | varies |
| Thread safety | model read-only, one state per thread | `mjModel` shared, one `mjData` per thread | per-world | varies |
| Determinism | IEEE, no FMA, no fast-math: bitwise repeatable on one platform (tested) | not stated in the documentation read; must be tested | not guaranteed across builds | varies |
| FEM / viewer compatibility | shares units, frames, selections, materials and job/results plumbing | separate model format; frames and units need mapping | same | same |

MuJoCo facts are from its official overview and repository README, read 2026-09-16: C API, generalised coordinates,
optimisation-based soft contact, Newton/CG/PGS solvers, MJCF/URDF/MJB, the `mjModel`/`mjData` split, Apache-2.0,
and the platform list for prebuilt binaries. Claims marked "to be itemised" or "must be tested" have not been checked.

## Why native first

1. **The brief's diagnostics are central, not optional.** Redundant or inconsistent loop constraints, singular
   configurations, reaction non-uniqueness, energy and work accounting, and constraint drift must be reported
   rather than absorbed by regularisation. A native core exposes the constraint Jacobian, its rank and the
   multiplier solution directly.
2. **Orchestrator semantics:** trial advance, rollback, and accepted-state checkpoints that keep controller,
   sensor-noise and filter state bit-exact are easiest when the whole state is ours.
3. **Toolchain and permissions:** no CMake or package manager on this machine, and downloading binaries needs
   explicit user permission. The native core builds with the existing clang flags on macOS and headless Linux.
4. **Engineering loads:** rigid-motion loads feeding the FEM must preserve resultants and moments exactly. Contact
   impulses must stay distinguishable from averaged forces. Soft-contact force histories would carry regularisation
   artefacts into structural assessment.

## Risks of the native choice, with mitigations

| Risk | Mitigation |
|---|---|
| Bugs in new dynamics code | Analytic verification suite (`tools/mechtest.c`): free fall, torque-free rotation against the Jacobi-elliptic solution, compound pendulum against the elliptic-integral period, damped oscillator, two-link inverse dynamics against closed-form Lagrange equations, joint limits, Freudenstein four-bar. Convergence orders are measured, not assumed. |
| Contact robustness on general meshes | Restricted, inspectable contact geometry (plane, sphere, box, capsule, convex hulls, and approximations built only on request and reported). Sensitivity to time step and contact parameters is reported with every contact result. |
| Performance in contact-rich scenes | Targets are engineering test rigs (tens of bodies, tens of contacts), not large robot fleets. Measure first; optimise only after verification. |
| Duplicated effort versus a mature engine | Keep a narrow backend interface (`MechBackend`: compile, trial step, get/set state, outputs) so a MuJoCo adapter can be added as a cross-check once download permission is given. |

## Narrow backend interface (planned)

```c
typedef struct MechBackendOps {
    const char *name;                 /* "navier-mb" or "mujoco" */
    bool (*compile)(const Assembly *a, void **model, MechDiag *d);
    bool (*trial_advance)(void *model, const MbState *in, double dt, const MbInputs *u, MbState *out, MbStepReport *r);
    size_t (*state_size)(void *model);
    bool (*serialize)(void *model, const MbState *s, uint8_t *buf, size_t cap);
    bool (*deserialize)(void *model, const uint8_t *buf, size_t len, MbState *s);
    void (*destroy)(void *model);
} MechBackendOps;
```

`trial_advance` is pure: it writes a new state and never modifies `in`. Commit and rollback therefore only choose
which state to keep.
