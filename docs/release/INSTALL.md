# Installing OpenPhysicsAI from a clone

Up: [README.md](../../README.md) · [AGENTS.md](../../AGENTS.md) · problems: [TROUBLESHOOTING.md](../TROUBLESHOOTING.md)

OpenPhysicsAI is built from source with clang and make. There is no installer and nothing is installed outside the
clone and two folders in your home (see [Where things go](#where-things-go)). Packaged builds are not published yet.

## Platforms

| Platform | State |
|---|---|
| macOS 11 or later on Apple silicon | developed and tested here (macOS 15, M2, 8 GB): the app, the servers, the lab, every test tier |
| macOS on Intel | written to build (the Makefile tunes for the processor it finds), never built or tested |
| Linux on x86-64 | built and tested on Ubuntu 24.04 with clang 18 (in WSL2) on 2026-10-22: `make` with no warning, `make smoke` 5 of 5, `make test-fast` all 52 commands, `make test-printing`, the MCP checks 521 of 521 and 190 of 190, `openphysics-ctl doctor`. Everything except the macOS parts below |
| Linux on arm64 | written to build, not tested |
| Windows | not supported directly; Linux in WSL2 is how the Linux release test ran |

### What works where

| Part | macOS | Linux |
|---|---|---|
| The engine and its three programs: `openphysics-ctl`, `openphysics-mcp` (the MCP server), `openphysics-server` | yes | yes |
| The physics lab's tools: `build/labrun`, `build/labfilm`, `build/labprobe` | yes | yes |
| The native app, `./openphysics` and `OpenPhysicsAI.app` (Cocoa, OpenGL, Core Text) | yes | no |
| The headset link ([docs/xr.md](../xr.md)), served by the app | yes | no |
| The GPU flow and water engines (Metal) | yes | no. Five examples choose the GPU flow engine: `cylinder_mounted_3d`, `sphere_wake_3d` and `wing_3d_10` run on the CPU with `"engine": "cpu"` added at the top of the scenario; `flag_3d` and `heart_valve` need the GPU engine, because the sheet in their flow (its immersed boundary) lives there. The water examples (`breaking_wave`, `dam_break`, `sloshing_tank`) run on the CPU by themselves |
| The physics kernel (MFEM) and 3D magnetics | yes, after `make mfem` once per clone | not yet: MFEM builds, but two of the lab's files use Apple's Accelerate for their sparse direct solvers. Twelve examples need them: the nine `kernel_*`, `pm_motor_3d`, `pm_motor_3d_drive`, `induction_gear`; `labrun` says so and stops |
| The CAD tool, `build/openphysics-cad` ([docs/cad.md](../cad.md)) | yes: `make cad build/openphysics-cad` builds its libraries (about an hour, once) and then the tool; `make` builds the tool when the libraries are there | written to build with the distribution's CMake; not part of the Linux release test |
| The test tiers below | yes | yes |

Of the 77 examples in `examples/lab/`, 58 run on Linux as shipped and 61 with `"engine": "cpu"` added to three of them
(the 12 that need the kernel or 3D magnetics and `flag_3d` and `heart_valve` do not), and two run nowhere at the
moment: `crossflow_fan` and `seat_climate` are refused on every
system because their fluid lacks two properties the solver asks for (`thermal_diffusivity_m2_s` and
`volumetric_heat_capacity_j_m3k`).

## Prerequisites

| Needed for | macOS | Linux (Ubuntu or Debian) |
|---|---|---|
| building | the Xcode Command Line Tools: `xcode-select --install` | `sudo apt install clang make python3 git` |
| the test suites | Python 3 (part of the Command Line Tools) | python3, above |
| the CAD tool (optional) | nothing more: the build fetches its own pinned CMake | `sudo apt install cmake curl` |
| a few tools (optional) | NumPy: `python3 -m pip install numpy` | `sudo apt install python3-numpy` |

NumPy is used only by `tools/labread.py`, `tools/ccx_crosscheck.py`, `tools/cadlatticetest.py`,
`tools/frame_subject.py` and `tools/showcase/gallery_thumbs.py`. No Homebrew, no package manager on macOS and no
library outside the operating system are needed for the build; libraries the lab builds on are fetched at pinned
versions with their checksums by the targets that need them ([THIRD_PARTY.md](../THIRD_PARTY.md)).

## Get it and build it

```bash
git clone https://github.com/molanocortes/OpenPhysicsAI.git
cd OpenPhysicsAI
make            # see the table below for what this builds on each system
make lab        # the lab's tools: labrun, labfilm, labprobe
make smoke      # five checks, about 20 seconds after the build
```

A clone downloads about 140 MB and takes about 310 MB on disk (170 MB of files and their history). The build adds about 200 MB in `build/`; the CAD libraries take about
1.7 GB in `~/.openphysicsai/third_party` (once per machine, shared by every clone), MFEM about 65 MB in
`build/third_party`. A lab result holds every frame of every field: a few MB for a small 2D case, gigabytes for a large
3D one (`shoebox_hall`, 5.9 million cells and 46 frames, writes 2.2 GB).

| Target | What it builds | Time |
|---|---|---|
| `make` on macOS | the app (`./openphysics`), the three programs, and the `navier*` links ([below](#renamed-from-navier)); the CAD tool too once its libraries are built | minutes |
| `make` on Linux | the headless build: the three programs, the test programs and the lab's tools | 45 s on four cores |
| `make headless` | the same as `make` on Linux, on either system | |
| `make am` | only the three programs | |
| `make lab` | the lab's tools and tests | |
| `make cad build/openphysics-cad` | the CAD libraries (Open CASCADE 7.9.3, Manifold 3.5.4), then the CAD tool | about an hour, once |
| `make mfem` | MFEM 4.8 for the physics kernel; the kernel itself is macOS only for now | about 20 minutes |
| `make clean` | removes what the build made; keeps `OpenPhysicsAI.app/Contents/Info.plist`, which is tracked | |

`PORTABLE=1` (for example `make PORTABLE=1`) builds for the processor family's baseline instead of tuning for this
machine (`-mcpu=native` or `-march=native`), for binaries that must run on other computers. Numerical code never
builds with fast-math, so results do not depend on the tuning.

## Check it

`make smoke` builds what it needs and runs five checks, each one line with PASS or FAIL: the build, the doctor, a
lab scenario (`examples/lab/lisle_tooth_root.json`), a picture of its result drawn by `labfilm`, and the MCP server
answering `initialize` and `tools/list`. It is the first thing to run after a clone, on either system.

`./openphysics-ctl doctor` is the engine's own self-diagnosis:

- `openphysics-mcp` and `openphysics-server` sit next to it;
- on macOS, only system libraries are loaded;
- the embedded operation contract and the material library parse;
- which projects folder is used (a warning when it is the one from before the rename, below);
- the projects folder and the temporary folder are writable, and which folders the engine may read;
- `openphysics-mcp` answers an MCP `initialize` and lists its tools over stdio;
- whether a control server is running;
- a reference solve: two cantilevers through the comparison workflow, the tip deflection checked against beam theory
  within 2 % (-0.79 % on the Linux release test).

It ends with `ALL DIAGNOSTICS PASSED` (exit 0) or `DIAGNOSTICS FAILED` (exit 7). `--json` prints the checks as JSON,
`--no-solve` skips the solve. `./openphysics-ctl --embedded call capabilities_get` lists what the engine can and cannot
do, machine-readable.

### The test tiers

| Command | What it covers | Time on a four-core Linux laptop |
|---|---|---|
| `make smoke` | the first check after a clone | about 20 s |
| `make test` | the engineering workflow suite: the core, meshing, finite elements, the operations, studies and evidence, the MCP flows, the doctor | about 80 min |
| `make test-fast` | the session tier: the C verification suites, the lab's suites (with session subsets of the long statistical ones) and the MCP protocol checks | about 4.5 h (`geartest --quick` alone 55 min) |
| `make test-printing` | the printing suite: analytic FDM and LPBF checks, mesh checks, complete MCP printing workflows | about 13 min |
| `make test-ui` | clicks every control of the app off-screen (macOS) | |
| `make check` | the quality gate: warnings, sanitizers, `make test`, links, schema coverage, hygiene | |

What continuous integration runs: on every pull request, on Linux, `make headless`, `make test`, `make smoke` and the
doctor ([linux.yml](../../.github/workflows/linux.yml)), and the link and leaderboard checks
([leaderboard.yml](../../.github/workflows/leaderboard.yml)); every night, on Linux, `make test-fast`. There is no
macOS job: the app and the Metal engines are tested only on a developer's Mac.

## Where things go

| Place | What |
|---|---|
| the clone | the programs (`./openphysics-*`), `build/` (the lab's tools, tests, MFEM) |
| `~/OpenPhysicsAI-Projects` | projects, studies and exports; lab results the app or an agent runs go to `lab/` in it. `--workspace DIR` changes it |
| `~/.openphysicsai/` | per-user state: the control socket (`run/`), the CAD libraries (`third_party/`), the app's settings |

The programs read input files only below their read roots (`$HOME`, `/tmp`, `/Volumes` and the working directory) and
write only below the projects folder and `$TMPDIR`; `--allow-read DIR` and `--allow-write DIR` add roots.
To uninstall, delete the clone and these two folders.

## Connect an AI client

[docs/mcp-clients.md](../mcp-clients.md) gives the configuration for Claude Code, Claude Desktop, Codex and any
client that starts an MCP server on stdio; the [README](../../README.md#connect-an-ai-agent) has the short version.

## Renamed from NAVIER

Until 2026-10-21 the project was called NAVIER. For one release the old names keep working:

| Before | Now | What still works |
|---|---|---|
| `navier`, `navier-ctl`, `navier-mcp`, `navier-server`, `build/navier-cad` | `openphysics`, `openphysics-ctl`, `openphysics-mcp`, `openphysics-server`, `build/openphysics-cad` | `make` makes the old names as links to the new programs, so an MCP client configuration or a script that names `navier-mcp` keeps working; `make clean` removes them |
| `NAVIER_*` environment variables | `OPENPHYSICS_*` | a `NAVIER_` variable is read when its `OPENPHYSICS_` one is not set |
| `~/NAVIER-Projects` | `~/OpenPhysicsAI-Projects` | the old folder is used while it is the only one; the doctor warns. Rename it when no older program needs it |
| `~/.navier/` | `~/.openphysicsai/` | read where the new place has nothing, never written |
| `NAVIER.app` | `OpenPhysicsAI.app` | |
| MCP server name `navier` | `openphysics` | rename the entry in your client's configuration when convenient |

A server started before the rename still answers at its old socket; the doctor says so, and `openphysics-mcp
--embedded` avoids connecting to it.

## Older pages

The 0.3.0-rc2 macOS archive of September 2026 had its own pages, kept as they were:
[its install page](archive/INSTALL-0.3.0-rc2.md) and [its front page](archive/README-0.3.0-rc2.md).
