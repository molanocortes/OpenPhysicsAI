# Troubleshooting

Up: [README.md](../README.md) · install: [docs/release/INSTALL.md](release/INSTALL.md) · MCP clients:
[mcp-clients.md](mcp-clients.md)

Start with `make smoke` and `./openphysics-ctl doctor`: between them they say which part is wrong. Each entry below
gives the message you see, why, and what to do.

## `make` fails on Linux at `OpenGL/gl3.h`

You are on a version from before 2026-10-22, when `make` still meant the macOS app everywhere. Pull, or run
`make headless`, which builds everything that does not need the app.

## "the GPU engine needs Metal, which only macOS has"

The scenario chooses the GPU flow engine, which is written in Metal. Five examples do so by default. For
`cylinder_mounted_3d`, `sphere_wake_3d` and `wing_3d_10`, copy the scenario and add one line at its top level:

```json
"engine": "cpu",
```

The CPU engine is the one the GPU engine was verified against (`tools/lbm3dtest.c`, cases L1 and L4); it is slower,
so a 3D case can take many minutes on a laptop. `flag_3d` and `heart_valve` cannot: a sheet in the flow (the flag, the
valve's leaflets) is an immersed boundary that only the GPU engine has, and `labrun` says so. The water scenarios need nothing: without Metal they run on the CPU by
themselves. In the test suites the GPU cases are reported as not run on such a machine, not as failures.

## "the physics kernel needs macOS for now" (and the same for 3D magnetics)

The kernel ([docs/lab/kernel.md](lab/kernel.md)) and 3D magnetics solve with MFEM and with the sparse direct solvers of
Apple's Accelerate framework. MFEM builds on Linux (`make mfem`), but two of the lab's files still call Accelerate, so
on Linux the twelve examples that need them (the nine `kernel_*`, `pm_motor_3d`, `pm_motor_3d_drive`,
`induction_gear`) stop with this message. `make mfem` does not change that. A portable sparse solver in their place is
open work. On macOS, run `make mfem` once per clone, then `make lab` again.

## "the fluid's viscosity, diffusivity and heat capacity, and largest_speed_m_s, must be positive"

`examples/lab/crossflow_fan.json` and `examples/lab/seat_climate.json` stop with this on every system: their fluid has
no `thermal_diffusivity_m2_s` and no `volumetric_heat_capacity_j_m3k`, which the heat solver asks for. They are known
to be broken and will be fixed; use another example.

## `labfilm` shows only the outside of a 3D result

`labfilm` draws what a result holds as surfaces: 2D fields, slices, the surfaces of bodies, and of a 3D block of cells
its six outer faces, lit, each corner the mean of the cells around it. What happens inside a 3D block (the `*_volume`
examples, `shoebox_hall`) is therefore not in the picture. Open such a result in the app on macOS (`lab open FILE.lab`),
which draws volumes and cuts, or read numbers from it with `./build/labprobe` (for example `--line`, see
[docs/lab/README.md](lab/README.md)). `labfilm FILE.lab --info` lists what a result holds.

## The disk fills up

- A clone takes about 310 MB on disk and its build about 200 MB more. The CAD libraries, built only on request
  (`make cad`), take about 1.7 GB in `~/.openphysicsai/third_party`, once per machine.
- A lab result (`.lab`) holds every frame of every field: a few MB for a small 2D case, gigabytes for a large 3D one
  (`shoebox_hall` writes 2.2 GB); a film of it writes one PNG per frame. Check the size of a scenario's grid
  (`dx_m`, `cells`) and its number of frames before you run it on a small disk. Write results where you have room, and delete the ones you no longer need.
- Projects and lab results that the app or an agent runs go to `~/OpenPhysicsAI-Projects`. Delete old ones there.
- The test suites write into `$TMPDIR` and remove what they wrote when they pass; a suite that fails leaves its folder
  for inspection (`/tmp/navier-*` or `/tmp/openphysics-*`). Delete those when you are done.
- On Linux in WSL2, Linux's `df` reports the free space of its virtual disk, not of the Windows drive that holds it.
  When that drive fills, Linux turns its disk read-only. Check the Windows drive itself before long runs.

## An MCP client does not see the server, or sees no tools

- The command must be the absolute path of `openphysics-mcp` in your clone (or `navier-mcp`, its old name). Relative
  paths and `~` are not expanded by every client.
- Run `./openphysics-ctl doctor`: its `mcp_stdio_transport` line says whether the server answers `initialize` and how
  many tools it lists (75 by default; 88 with `--lab`).
- Nothing may be written to the server's standard output except protocol messages. Wrappers that print a banner break
  the connection; the server's own log goes to standard error.
- If the client's context is small, the 75 tool definitions are large (about 325 KB). `--lab REPO --lean` serves 16
  tools instead and reaches the engine's operations through three of them.
- The server reads files only below its read roots (`$HOME`, `/tmp`, `/Volumes`, the working directory). For STL files
  elsewhere add `--allow-read DIR`.
- Without `--embedded`, the server connects to a running `openphysics-server` (or one from before the rename) if one
  answers, and otherwise runs its engine in-process. Pass `--embedded` to be sure it does not.

[mcp-clients.md](mcp-clients.md) has the configuration for each client.

## The doctor's warnings

| Line | Meaning | What to do |
|---|---|---|
| `warn projects_folder ... ~/NAVIER-Projects is the projects folder from before the rename` | the project was called NAVIER until 2026-10-21; while only the old folder exists, it is used | rename `~/NAVIER-Projects` to `~/OpenPhysicsAI-Projects` when no older program needs it |
| `warn executables cannot determine the location of this executable` | the doctor could not find its own folder, so it could not look for `openphysics-mcp` beside it | run it by its path in the clone: `./openphysics-ctl doctor` |
| `info control_socket a server started before the rename answers at ...` | an old `navier-server` still runs; `openphysics-mcp` would connect to it | stop it and start `openphysics-server`, or pass `--embedded` |
| `fail executables ... MISSING` | the programs were not all built | `make am` (or `make`) |
| `fail workspace_writable` or `temp_writable` | the projects folder or `$TMPDIR` cannot be written | free space, fix the permissions, or `--workspace DIR` |

## The CAD tool: "CMake 3.18 or newer is needed"

`build/openphysics-cad` builds Open CASCADE and Manifold once per machine. On Linux it uses the distribution's CMake:
`sudo apt install cmake curl`, then `make cad`. Nothing was downloaded before the refusal. On macOS the build fetches
its own pinned CMake and needs nothing more. Everything else in the repository builds and runs without the CAD tool.

## Still stuck

Open an issue with the output of `make smoke` and `./openphysics-ctl doctor --json`, your system (`uname -a`, `clang
--version`) and the command that failed. [docs/release/REPORTING_FAILURES.md](release/REPORTING_FAILURES.md) says what
else helps.
