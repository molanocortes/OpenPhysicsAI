# Solver roadmap: from printing to the pictures on every simulation vendor's front page

Owner's decision, 2026-09-19: everything the commercial tools show is within reach except billion-cell LES and
geospatial twins; build all of it, step by step, with AI sessions doing the work and verification before results.
Up: [AGENTS.md](../../AGENTS.md). Rules for every phase: criteria first, closed forms or published benchmarks,
provenance on inputs, no new dependencies, the five states kept apart.

| Phase | Capability | Why it matters | First verification cases | Session |
|---|---|---|---|---|
| A | **Conforming tetrahedral mesh** (isosurface stuffing on the octree we have, then TET4 and quadratic TET10 elements in the solid solver, mesh quality report) | every solid-mechanics picture has a mesh that follows the geometry; voxels lose thin walls and fillet stresses | patch test; cantilever convergence; Kirsch hole (stress concentration 3.0); fillet factor against a published chart; the bracket and a thin-walled casting at 2 mm without losing walls | app session, now |
| B | **Large deformation and explicit dynamics** (total-Lagrangian nonlinear elements with Newton; central-difference explicit with lumped mass and hourglass control; node-to-surface contact including self-contact) | crash, forming, buckling; the crash-tube picture | elastica of a cantilever; Euler buckling; the Abramowicz-Jones mean crushing force of a square tube; a dropped ball's contact impulse (exists in the mechanics layer) | solver session, after wave 5 |
| C | **Compressible flow** (finite volume Euler then Navier-Stokes on the voxel grid with an immersed boundary from the STL; HLLC flux, MUSCL, RK3; shocks) | supersonic bodies, re-entry, nozzles | Sod shock tube; oblique shock on a wedge against the exact relations; bow-shock standoff on a sphere against Billig's correlation; a cylinder's drag at Mach 2 against published data | solver session, after B |
| D | **Assemblies and contact in the FEM** (many bodies in one analysis, node-to-surface frictional contact, bolted joints, gear tooth contact) | the gearbox picture; real assemblies instead of one part | Hertz contact (sphere on plane, cylinder on plane); a bolted flange; two gear teeth in contact against Hertz | solver session, after C, with the mechanics layer's contact code reused |
| E | **Topology optimisation** (density method on the voxel grid, compliance objective with a volume constraint, sensitivity filter, optimality criteria) | the optimised-bracket picture; a small addition on the mesh we already have | the MBB beam and the cantilever benchmark of the 99-line code, compared with the published layouts and compliance | app session, after A |
| F | **Presentation** (transparent bodies with correct depth ordering, ambient occlusion, edge lines, exploded assemblies, a mechanism demo in the app, image export at print resolution) | the pictures | pixel checks as today; a gallery in the README rendered by the software | app session, alongside E |

Not on the roadmap: billion-cell LES (a cluster, not a feature), geospatial twins (not simulation). Each phase gets
its own contract file in `docs/contracts/` before code, in the form of `lpbf-build.md`, and a row in the honest
status table of the README when it reaches a state.
