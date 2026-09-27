# Open tasks, written for agents and people

Each task is a self-contained prompt with an acceptance test. Take one, work on a branch in a worktree, and open a
pull request that shows the acceptance test passing. Rules: [CONTRIBUTING.md](CONTRIBUTING.md). Map of the code:
[AGENTS.md](AGENTS.md). Tasks marked *reserved* are being done by the core sessions; the others are free.

| Task | Size | State |
|---|---|---|
| T1 Linux headless build | medium | done 2026-09-19 (architect): the engine, servers and all 21 suites pass on Ubuntu in CI (.github/workflows/linux.yml) |
| T2 Plasticity in the solid solver | large | reserved (solver session) |
| T3 Support structures for the LPBF build | large | reserved (solver session) |
| T4 Simple, Advanced and Agent modes in the app | large | reserved (app session) |
| T5 Measurement uncertainty carried into the score | medium | done 2026-09-19 (architect): scatter.json next to the measurements, within-scatter verdicts, tied ranks |
| T6 A second measured data set | any | free, needs a printer |
| T7 Streamable HTTP transport for the MCP server | medium | free |
| T8 Pieces below N elements dropped from a voxel mesh, counted and shown | small | free |
| T9 Strip absolute home paths from tracked files | small | done 2026-09-27: stage F of `tools/check.sh` reports none and has no exemptions |
| T11 Import the first NIST AM-Bench challenge | medium | free |
| T12 Adaptive voxel mesh (fine at the surface, coarse inside) | large | reserved (solver session) |
| T13 The physical measurement kit run on a second machine | any | free, needs a printer |
| T14 Materials library, batch 1: AM metals with sources | medium | reserved (app session, wave 9) |
| T15 Materials library, batches 2 to 5 (polymers, engineering metals, fluids, elements) | medium each | free after T14 |
| T16 Support structures as first-class physics with agent-controlled parameters | large | done 2026-09-19 (typed supports, `lpbf_supports_generate`); the homogenised tooth model is T23 |
| T17 Conforming tetrahedral mesh and TET4/TET10 elements (roadmap phase A) | large | done 2026-09-19, verified (`tools/tettest.c`, 91 checks) |
| T18 Large deformation and explicit dynamics with contact (phase B) | large | steps 1 to 3 done 2026-09-20 (`tools/dyntest.c`); open: the crush tube, the energy account once material yields, Hertz on a graded mesh |
| T19 Compressible flow with shocks (phase C) | large | free after T18 |
| T20 Assemblies and frictional contact in the FEM (phase D) | large | free after T19 |
| T21 Topology optimisation on the voxel grid (phase E) | medium | done 2026-09-20, verified (`tools/topotest.c`) |
| T22 Presentation: transparency, ambient occlusion, edges, exploded assemblies, mechanism demo (phase F) | medium | done 2026-09-20 except the mechanism demo in the app |
| T24 The print build on the conforming tetrahedral mesh: voxels cut thin walls into loose pieces (a drone frame: 35 at 2 mm), so its distortion cannot be compared | large | free |
| T25 Why the cantilever's build converges slowly and differently in X and Y: mesh and layer refined separately, a fourth level, then a remedy | medium | free |
| T23 Homogenised supports that hold for a comb a few pitches tall | medium | free |

## T1 Linux headless build

Prompt: The engine (`make headless`) is written to build without the macOS frameworks but has never been built on
Linux. On Ubuntu 22.04 or 24.04 with gcc or clang, make `make headless` and `make test` pass. Fix only what the
platform needs (a `src/platform_linux.c` for what `platform_macos.c` provides to the headless targets, byte-order
and `sysctl` calls, threads, timers). No new dependencies. Do not touch the GUI targets. Acceptance: `make headless`
and `make test` exit 0 on Linux; paste the final lines of every suite; the macOS build is unchanged (`make && make
test` still pass there). Add a `.github/workflows/linux.yml` that runs `make headless && make test` on ubuntu-latest.

## T5 Measurement uncertainty carried into the score

Prompt: A measurement set may carry no stated scatter. Add to
`validation/RESULT_FORMAT.md` an optional `uncertainty` block per result (a stated interval and its source) and to
the measurement CSV format an optional column for it; make `tools/leaderboard.py` report an
error as "within scatter" when both the prediction interval and the measurement interval are given and overlap,
and never rank two entries whose scores differ by less than the measurement scatter. Acceptance: a synthetic
result set with declared scatter produces the expected verdicts in a new test in `tools/`; existing outputs unchanged
when no uncertainty is declared.

## T6 A second measured data set

Prompt: Print the standard calibration cantilever (`samples/cantilever.stl`, from the open-access paper cited in
`samples/samples.json`) on any LPBF machine, in X and Y, cut it as the source describes, and measure the tip
deflection with a stated instrument. Submit it through `validation/contrib/`. Acceptance: a pull request with the
measurement file, the process record (machine, material, powder, layer thickness, laser power, scan speed, hatch,
scan strategy, cut height and tool), the instrument and its resolution, and photographs. This is the most valuable
contribution the project can receive.

## T7 Streamable HTTP transport for the MCP server

Prompt: `navier-mcp` speaks MCP over stdio. Add the Streamable HTTP transport (MCP 2025 specification) on a loopback
address with token authentication, reusing `src/net/` (the control socket already has loopback TCP with a token).
No new dependencies: HTTP/1.1 parsing by hand, no TLS (loopback only, say so). Acceptance: `tools/mcptest.py`
extended to run its protocol suite over HTTP as well as stdio, all passing; refusals for a missing or wrong token
and for non-loopback binds tested.

## T8 Pieces below N elements dropped

Prompt: A voxel mesh of a thin-walled part can fall into several face-connected pieces. Add to `mesh_generate` an
optional `drop_pieces_below` (element count, with provenance) that removes pieces smaller than that, reports every
removed piece (elements, volume, bounding box) in the mesh report and the app's verdict line, and refuses if the
largest piece would be removed. Acceptance: opstest cases for a two-piece synthetic body; a thin-walled part that the
voxel mesh splits into several pieces reports every piece and, with the option, keeps the main body; nothing is silent.

## T9 Strip absolute home paths

Prompt: `bash tools/check.sh F` lists tracked files that contain an absolute home-folder path. Make them relative or
symbolic, keep every document readable, and bring the baseline in `tools/check.sh` to zero. Done 2026-09-27. Acceptance: stage F
reports 0 files; linkcheck passes.

## T11 Import the first NIST AM-Bench challenge

Prompt: Follow `challenges/nist-am-bench/CHALLENGE.md` exactly: read NIST's terms of use for the AM-Bench data and record
them with the access date; import only what may be redistributed, otherwise fetch instructions and hashes; write
`challenge.json` for one measured quantity our solver predicts today (distortion after the cut of the bridge part), with
geometry, process record, measurements with their stated uncertainty, the calibration split and the scoring rule,
pre-registered before any run; generate the geometry from the published dimensions into a parameter file with a source
per dimension; run the reference entry and let `make leaderboard` score it. Nothing from memory. Acceptance: `make
leaderboard` lists the new challenge with at least one entry; every number in the challenge files has a URL and access date.

## T13 The kit on a second machine

Prompt: Run `kit/README.md` on any LPBF machine, and submit the campaign
through `validation/contrib/`. Acceptance: a pull request with the campaign file complete, photographs, and the
instrument stated; the maintainers calibrate the reference solver on it and add the profile with your name.

## T14 and T15 Materials library batches

Prompt: Follow `materials/README.md`. For every material of the batch, add a record to `src/ctl/materials.json` in the
form of the published records (for example `alsi7mg_lpbf`): every property object with `value`, `provenance` and a `source` that a reader can
open (URL with access date, or a document in this repository with page or table). Prefer open sources in the order
the README lists; facts cited, tables never copied wholesale; nothing from memory. Where a source gives a range, record
the range in `note` and the value you chose and why. Temperature tables where the source gives them. Replace the
demonstration record of the same material and rename anything a test depends on to `*_demo`. Acceptance:
`python3 tools/matcheck.py` exit 0 with the new count; `make test` unchanged or its baselines updated with a stated
reason; a table in the pull request: material, properties, sources, licence of each source.

## T16 Support structures as first-class physics

Prompt: reserved for the solver session; see its wave-5 brief and `VISION.md`. In short: support types (block, wall,
cone, tree, lattice) with geometry-derived homogenised stiffness and conductivity, verified against explicit fine
models of a unit cell; overhang angle, spacing, wall thickness, interface teeth and removal as typed inputs with
provenance; supports in both the thermal and the mechanical build; every parameter reachable through MCP.

## T17 to T22 The solver roadmap

Prompt: see `docs/design/ROADMAP-SOLVERS.md` for each phase's scope and first verification cases. Each phase starts
with a contract in `docs/contracts/` (criteria before code), keeps every existing suite green, adds its own suite,
reaches the app through the existing operation and result-file machinery, and ends with a README status row.

## T23 Homogenised supports that hold for a comb a few pitches tall

Wave 5 (S10, `docs/contracts/supports.md`) measured the homogenised support model against the cantilever's own comb
teeth meshed explicitly. It is off by **+74 to +108 per cent**
when the homogenised cell is given the comb's own wall pattern, and by **-79 to -82 per cent** with block supports.
The unit cell says why, and the finding is worth more than the failure:

* A lattice of parallel walls has **no shear stiffness across the gaps**. Homogenisation replaces the teeth with a
  continuum that carries shear the real teeth cannot, or (with the block cell) with one far too soft in the
  direction that matters.
* A real tooth is **clamped at both ends** and is only about three pitches tall. Its stiffness comes from its ends,
  which is a boundary effect, and homogenisation is the operation that removes boundary effects. The model assumes a
  separation of scales that a support a few pitches tall does not have.

**Acceptance:** the cantilever's comb, represented as homogenised supports, predicts the build's deflection within
**10 per cent** of the same build with the teeth meshed explicitly, on the same mesh and the same protocol
(`docs/contracts/supports.md`). Until then the cantilever pipeline keeps explicit teeth, and no result may be quoted from
a homogenised support of that size. Candidate routes, none verified: a cell that carries the end clamping as a
boundary layer, a beam-lattice support element instead of a continuum, or a rule that refuses homogenisation below a
stated number of pitches (the honest short answer, and the one to implement first).
