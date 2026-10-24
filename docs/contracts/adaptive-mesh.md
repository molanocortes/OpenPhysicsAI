# Adaptive voxel mesh for the layer-by-layer build (TASKS.md T12)

Written 2026-09-19, **before the code**, by the solver session. The criteria below change only by dated amendment.

## Why

A uniform voxel mesh of the cylinder head at 2 mm has 316 723 part elements before supports; a commercial 2 mm voxel
model of the same part was several times smaller, because its voxels are coarse inside the part. The build cost grows faster than the element
count, so 2 mm, and plasticity at 4 mm, were out of reach on this machine in wave 4.

## What is built

**Fine at the surface, coarse inside, 2:1 balanced.** The mesh starts from the uniform voxel mesh at the fine size `h`
(the mesher's inside test, supports and the cut already decided on it) and merges aligned blocks of 2 x 2 fine elements
into one, recursively, up to a largest size `2^L h`. A block is merged only if every fine element in it is the same
kind (part or support; the cut band and removed islands are never merged), and no element in it, nor any element that
touches it along a face, edge or corner in its own layer or the layers above and below, is missing: the free surface
and one fine element beyond it stay fine. Neighbouring elements, across faces, edges and corners, differ in size by at
most a factor of two (a **2:1 balanced** forest, enforced by lowering levels until no pair violates it).

**Layers stay whole, so coarsening is in x and y only.** An element belongs to exactly one simulation layer. With the
layer thickness equal to the fine size, which is how every build here runs, no element may be taller than one fine
element: the coarse elements are `2^L h x 2^L h x h`. **The cost:** the reduction is at most `4^L` instead of `8^L`, and
the coarse elements are flat (aspect ratio up to 4 at 2 mm surface and 8 mm inside). Coarsening in z would need layers
thicker than the surface voxel, which changes the physics (wave 3 measured the layer thickness as the dominant size).

**Coupling: hanging-node constraints.** A node of a fine element that lies inside an edge or a face of a coarser
neighbour is a hanging node. Its displacement is the coarse element's interpolation: the mean of the two edge ends for
a mid-edge node, of the four face corners for a mid-face node (the only positions a 2:1 mesh allows). The constraint
is eliminated in assembly: a hanging node carries no equation, and the element matrices and forces that touch it are
distributed to its masters with those weights; a master that is itself hanging is resolved in turn. After the solve
the hanging values are interpolated and written like any other node, so the result file keeps plain hex8 elements and
the application draws it unchanged. The alternative, transition elements, would need element shapes the solver does
not have.

**In the build.** A constraint belongs to the coarse element whose edge or face creates it, and it is active only once
that element is: a layer is activated on the deformed part below it, and the constraint then ties the **increments**
of the hanging node to its masters from that step on, the same stress-free activation as every other element.

## Criteria, fixed before the first run

| ID | Criterion | Tolerance |
|---|---|---|
| **M1** | patch test across a 2:1 interface in x and y and between layers (mid-edge and mid-face hanging nodes): a free body under a uniform eigenstrain, held 3-2-1, displaces exactly `eps . x` at every node, hanging nodes included, and carries no stress | 1e-12 relative on displacement, stress below 1e-6 of `E |eps|` |
| **M2** | the same with a hanging node whose master is itself hanging (a chain) | as M1 |
| **M3** | a conforming mesh is solved exactly as before: every existing verification case unchanged | exactly as recorded |
| **M4** | the cantilever built on an adaptive mesh (0.5 mm at the surface, up to 2 mm inside) against the uniform 0.5 mm mesh, same layers | tip deflection after the cut within **1 %**; element count and wall time reported |
| **M5** | the result file of an adaptive build loads, holds hex8 elements only, and the hanging nodes carry displacements equal to their interpolation | to rounding |
| **M6** | the cylinder head at "2 mm at the surface, up to 8 mm inside": elements and memory reported against the uniform 2 mm mesh (316 723 part elements) | reported, no tolerance |

## Results (2026-09-19)

| ID | Measured | Verdict |
|---|---|---|
| M1 | 11 hanging nodes, mid-edge and mid-face, across x and between layers: displacement error 1.4e-14 of `eps L`, stress 1.2e-14 of `E eps` | pass |
| M1b | added: the eigenstrain in the fine upper elements only, so coarse and fine must push on each other through the constraints: support reactions of the free body 1.7e-15 of `E eps dA`, equilibrium error 1.9e-16, while carrying 0.72 `E eps` of stress | pass |
| M2 | the mid-face node written as a chain through two hanging mid-edge nodes: 1.4e-14 | pass |
| M3 | mechtest 258 (the 255 earlier cases unchanged), lpbfflow unchanged before the new checks, `make test` | pass |
| M4 | Calibrated-P17 at 0.5 mm, 0.5 mm layers, coarse up to 2 mm, fine band 2: X 1.6689 against 1.6780 uniform (**-0.54 %**), Y 3.9488 against 3.9715 (**-0.57 %**); 29 820 elements against 36 000; 19 to 21 s against 56 to 62 s for the uniform mesh on the direct solver and 27 s on the iterative one | pass |
| M5 | an adaptive slab (1 056 elements against 1 440 uniform, 464 hanging nodes): the file holds hex8 only, and the 200 hanging nodes on coarse top edges carry the mean of their edge ends to 2.1e-16 of the largest displacement | pass |
| M6 | the cylinder head, 2 mm surface, up to 8 mm inside, fine band 2, with the wave-4 support rule: **237 041 elements** (146 194 part, 90 847 support), 313 216 nodes, 188 064 hanging, against **529 190** uniform; the engine holds 308 MB once the case is built | reported |

**Amendment, 2026-09-19, the fine band.** The contract first kept one fine voxel along the surface. M4 measured it:
coarse up to 2 mm with a one-voxel band, the cantilever came out 3.3 per cent stiff (X 1.6224, Y 3.8501). The
constraints stiffen a thin member, and the cantilever's top beam is only six layers tall. The band is now an input,
**two voxels by default**, and M4 passes with it (-0.54 and -0.57 per cent). The criterion itself is unchanged. On the
cylinder head the band costs little: 220 283 elements with one voxel against 237 041 with two.

**Amendment, 2026-09-19, the solver.** With hanging nodes the masters couple across coarse elements, and the direct
factor fills in far more: the adaptive cantilever took 145 s on the direct solver and 22 s on the iterative one, against
56 s and 27 s uniform. The automatic choice now sends any mesh with hanging nodes to the iterative solver; conforming
meshes are unchanged.

**Amendment, 2026-09-19, a newly used hanging node.** A node on a coarse element's top face is first used when the layer
above is laid. It lies on that face as the face is then, deformed, so it starts at its masters' interpolation; on a
uniform mesh it would have moved with the layer below. Without this its written displacement disagreed with the
constraint by up to 71 per cent of the largest displacement (M5 caught it); the tip deflection of M4 did not change.

**Why the cylinder head is still larger than a commercial voxel model.** Coarsening is in x and y only, because an element must
stay within one 2 mm layer, so at most 16 voxels merge instead of 64; the head's walls are thin, so much of it is
within two voxels of a surface; and the wave-4 support rule fills every downward-facing cavity. Step B of this wave
changes the last of these.

**Amendment, 2026-09-19 (evening): released forces on unused hanging nodes.** When elements are removed (supports,
the cut), the forces they carried are released onto their nodes. A node that hangs on a coarse element and was used
only by the removed elements (the underside of a coarse part element standing on fine supports) carries no equation
and, with nothing active using it any more, its constraint was skipped and its released force was lost. The cylinder
head showed it: at 4 mm, adaptive against uniform, the same largest displacement on the plate (1.7556 mm both) but
5.93 against 1.62 mm separated, and 2.57 against 1.81 mm once the rigid motion is removed. Fixed in `src/mech/lpbf.c`:
before every step a released force on an unused hanging node goes to its masters with the constraint's weights. After
the fix: 1.66 mm separated, 1.809 mm rigid-free (uniform 1.811). A flow test now holds it (a 24 x 24 x 5 mm slab on
supports: adaptive within 1 % of uniform after removal; without the fix 15.9 times too far). M4 re-run with the fix:
X 1.6698 against 1.6780 (-0.49 %), Y 3.9504 against 3.9715 (-0.53 %), pass. Two guards came with it: the 3-2-1 hold
never takes a hanging node, and `solid.c` refuses a component held on a hanging node whose masters are free in it
(it was ignored before). The 2 mm cylinder-head build made before the fix is superseded for its separated state; its
state on the plate is unaffected.

**Amendment, 2026-09-27: the cylinder head removed.** The part and its runs were removed from the repository for
copyright, by the owner's decision. M6 and the numbers on the head above stay as the record of what was measured on
2026-09-19; they can no longer be rerun from this repository.
