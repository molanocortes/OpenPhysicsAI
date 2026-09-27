# src/lab: the physics lab

New physics domains, one directory each, plain C, no UI, no OpenGL, IEEE arithmetic (built with the headless core,
so they run on Linux and inside the servers). Up: [src/README.md](../README.md) · programme: [GOALS.md](../../GOALS.md)
· documentation: [docs/lab/](../../docs/lab/README.md).

Read first:

1. [labio.h](labio.h): the one result format every solver writes (frames of blocks, points and cells with fields).
2. [lab_domains.h](lab_domains.h): one entry point, `lab_run_domain(domain, scenario JSON, output)`, used by
   `build/labrun`, and later by the MCP server and the app.
3. [gas/gas.h](gas/gas.h): compressible flow on an adaptive mesh (supersonic, hypersonic, shocks, nozzles).

| Directory | What | Verified by | State |
|---|---|---|---|
| `labio.[ch]` | result format | `tools/labtest.c` (L1 to L3) | verified |
| `gas/gas3d.[ch]`, `gas3d_scenario.c` | double-precision 3D Euler, MC/MUSCL, HLLC, SSP-RK2, periodic volume | `tools/gas3dtest.c` | analytic diagonal sound wave and exact Sod refinement; no bodies/AMR yet |
| `gas/` | 2D planar and axisymmetric Euler, HLLC, block AMR, cut-cell bodies | `tools/gastest.c` (G1 to G8) | verified against closed forms; one flag entered |
| `acoustic/` | 3D FDTD acoustics, impedance walls, sources, receivers | `tools/actest.c` (A1 to A4) | verified against closed forms |
| `orbit/` | N-body, Gauss-Legendre order 12, 1PN, close-approach tracking | `tools/orbtest.c` (O1 to O4) | verified; against JPL on Apophis |
| `em/` | 3D Yee FDTD, CPML, TF/SF plane waves, scattering cross-sections | `tools/emtest.c` (E1 to E3) | verified against a cavity and Mie |
| `impact/` | explicit Lagrangian hex8, Johnson-Cook, Mie-Gruneisen, erosion, contact | `tools/imptest.c` (I1 to I5) | verified against closed forms and balances |
| `impact/sph.c` | particles for hypervelocity impact: SPH for solids, same material laws | `tools/sphtest.c` (S1 to S3) | verified against the Hugoniot, balances and the hex8 Taylor test |
| `flow/flow3d.[ch]`, `flow3d_scenario.c` | double-precision 3D periodic flow, full velocity and vorticity volumes | `tools/flow3dtest.c` | analytic Beltrami decay, mass and Poiseuille wall checks; finite voxel wings |
| `flow/` | 2D flow past cylinders and aerofoils on the app's lattice, dye streaklines, forces, Strouhal | `tools/flowtest.c` (F1, F2) | verified against the Schaefer-Turek benchmark; one flag entered |
| `heat/heat3d.[ch]`, `heat3d_scenario.c` | full 3D solid conduction, heterogeneous materials, insulated boundaries | `tools/heat3dtest.c` | analytic convergence and energy verification; native volume output |
| `heat/` | 2D heat and flow: D2Q9 TRT lattice with buoyancy, fans and porous media; conjugate heat transfer; a passive scalar | `tools/hftest.c` (H1 to H4) | verified against de Vahl Davis, closed forms and Shah and London |
| `magnet/` | 2D magnetostatics: vector potential, iron, windings, permanent magnets, Maxwell-stress torque; a motor that runs (`mg_drive.c`: rotor dynamics) and heats (`mthermal.c`: implicit conduction) | `tools/mgtest.c` (M1 to M7) | verified against closed forms |
| `relativity/` | light near a Schwarzschild black hole: null geodesics, the shadow, a lensed sky, a thin Keplerian disk with gravitational and Doppler shifts | `tools/rttest.c` (R1 to R4) | verified against closed forms; the disk's texture and the sky are pictures |
| `water/` | water with a free surface: weakly compressible SPH, Wendland kernel, delta-SPH, boundary particles for tanks and obstacles | `tools/wtest.c` (W0 to W2) | verified against hydrostatics and the sloshing period; not yet against measurements |
| `labview.[ch]` | software reference renderer for `tools/labfilm.c` and app fallback | output compared byte for byte with films before the split | integrated |
| `labscene.[ch]` | retained geometry from stored nodes and fields; sections expose interior hex faces | `tools/labscenetest.c`; native app `tools/uicheck.py --lab` | GPU integration in `src/labgpu.c`; points and unstructured cells |

Tools: `build/labrun` (run a scenario), `build/labfilm` (pictures and films), `build/labprobe` (numbers out of a
result), all built by `make lab`; `make test-lab` runs the verification suites.
