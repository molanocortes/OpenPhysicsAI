# Contract: the conforming tetrahedral mesh and the TET4 / TET10 solid elements (task T17, phase A)

Owner: the app session (`src/geom/tetmesh.[ch]`, `src/fem/tet.[ch]`, the additive lines in `src/fem/solid.[ch]`,
`src/ctl/ops_mesh.c`, `tools/tettest.c`). Written on 2026-09-19 before any code of this task; it changes only by
addition or dated amendment. Up: [ROADMAP-SOLVERS.md](../design/ROADMAP-SOLVERS.md), phase A.

The voxel mesh stays the default and stays untouched: every print analysis (LPBF build, FFF print, thermal) keeps
running on it, and the hex path of the solver is not edited, only dispatched around.

## 1. The method

**Geometry as a signed distance.** The mesher sees the part only through a signed distance function `f(p)`,
negative inside. For an STL, `|f|` is the distance to the closest triangle (the existing BVH, `src/geom/bvh.h`) and
the sign is the inside test the voxel mesher already uses: the parity of ray crossings along x, y and z, majority of
three. Points closer to the surface than 1e-9 of the part size are on it (`f = 0`). Tests may pass an analytic
distance instead (a sphere, a plate with a hole), which separates the mesher's error from the STL's facets.

**Background mesh: a graded body-centred-cubic lattice on an octree.** No octree exists in the repository (the
adaptive mesh of the AM-process session coarsens the voxel mesh in x and y and is not merged), so this task adds one
in `src/geom/tetmesh.c`. The root cube covers the part with a margin; a cell is split while it is larger than
`surface_size` and the surface passes within its reach (`|f(centre)| < cell diagonal`), and while it is larger than
`interior_size` anywhere inside. The tree is then balanced 2:1 across faces, edges and vertices, so neighbouring
leaves differ by at most one level. Lattice vertices are the leaves' corners and centres, plus the edge midpoints and
face centres that transitions need. Tetrahedra:

- between two leaves of equal size that share a face: the body-centred-cubic tetrahedra, each spanned by the two leaf
  centres and one edge (or half edge, where a smaller leaf has put a midpoint on it) of the shared face;
- where a leaf meets a larger leaf, or the root's boundary: the cone from the leaf centre over its face, the face
  split by a fan from the face centre (the larger side) and along the diagonal to that face centre (the smaller
  side), which makes both sides triangulate the face identically.

Every leaf is covered by cones or bipyramids over its faces, and every shared face is triangulated the same way from
both sides, so the background mesh is conforming by construction. In a region of equal leaves it is exactly the BCC
lattice of the paper below.

**Isosurface stuffing** (F. Labelle and J. R. Shewchuk, *Isosurface Stuffing: Fast Tetrahedral Meshes with Good
Dihedral Angles*, ACM Transactions on Graphics 26(3), SIGGRAPH 2007, article 57). Implemented from the paper's rules,
nothing copied:

1. `f` is evaluated at every lattice vertex. On every lattice edge whose ends have opposite strict signs, the
   **cut point** is the zero of `f` on that edge, found by bracketing (secant then bisection) on the true `f`, so it
   lies on the surface to the bracketing tolerance, not on a linear interpolant.
2. **Warping.** A vertex is violated when a cut point on one of its edges lies closer to it than `alpha` times the
   edge length. `alpha = 0.24999` on the long lattice edges (centre to centre and corner to corner, the "black"
   edges) and `alpha = 0.41189` on all others, the pair the paper gives for its 10.7 degree bound. Each violated
   vertex moves to its closest violating cut point and takes `f = 0`; cut points on its other edges are dropped.
3. **Stencils.** Every background tetrahedron is replaced by the part of it inside the surface, by the count of
   inside (-), on-surface (0) and outside (+) vertices: no inside vertex, dropped; no outside vertex, kept whole; one
   inside vertex, one tetrahedron; two inside and two outside, a prism of three; two inside, one on, one outside, a
   pyramid of two; three inside and one outside, a truncated tetrahedron of three.

**Where this differs from the paper, and what that costs.** (a) The paper splits the quadrilateral faces of the
prisms by a parity rule its angle proof relies on; this implementation splits each quadrilateral by its shorter
diagonal (ties broken by the smaller vertex id), a rule that depends on the face alone and therefore keeps
neighbours conforming, and when the three quadrilaterals of a truncated tetrahedron happen to form a cycle it adds a
Steiner vertex at the centroid (eight tetrahedra) instead. (b) The grading and its transition tetrahedra are this
octree's, not the paper's. So **the paper's bound is not inherited**: the angles are measured on every mesh and the
floor in section 4 is enforced by refusal.

**Sharp edges are rounded at the cell scale.** A corner or crease of the part becomes a chamfer about one surface cell
wide, because the cut points lie on the surface but nothing forces a vertex onto the crease. Feature preservation is
a later step, not this one. Faces that lie on lattice planes are reproduced exactly. (Since 2026-10-08 the edges and
corners between the surface's patches are kept: section 12.)

**TET10.** Quadratic elements add a node at the middle of every edge. On edges that lie on the surface the middle
node is moved to the closest surface point, so the element follows a curved face; if that makes any Jacobian at a
quadrature point non-positive or smaller than a fifth of the straight element's, the node stays straight and is
counted.

## 2. Inputs (geometric choices, reported, no provenance needed)

- `surface_size`: the leaf size at the surface (m). Required.
- `interior_size`: the largest leaf inside (m). Default 4 x `surface_size`, clipped to a power-of-two multiple.
- `order`: 1 (TET4) or 2 (TET10). Default 2.
- the bodies: closed triangle surfaces in the build frame (the same bodies the voxel mesher takes), or, in tests, an
  analytic signed distance with a closest-point projection.

## 3. Refusals

- no body, a non-positive size, `interior_size < surface_size`, or an element estimate above the server's limit;
- a minimum dihedral angle below the floor (section 4), with the angle and where it is;
- any element with non-positive volume after warping;
- more than 5 % of the lattice vertices near the surface with an uncertain inside test (the voxel mesher's limit).

## 4. Quality reported per mesh, and the floor

Per mesh: elements, nodes, minimum and maximum dihedral angle (degrees), worst aspect ratio (circumradius over three
times inradius, 1 for a regular tetrahedron), distance of the surface nodes to the STL (max and mean) and of the
surface faces' centroids (max and mean, the chordal error), mesh volume against the STL volume (per cent),
face-connected regions, curved and straightened TET10 edges, seconds.

**Floor: a mesh whose smallest dihedral angle is below 5 degrees is refused.** Target on every verification shape of
section 5: at least 10 degrees and at most 165 degrees; a shape that misses the target is reported with its number.

## 5. Acceptance, fixed before the code

Mesher (M):

- **M1 cube.** An axis-aligned cube whose faces lie on lattice planes: volume exact to 1e-12 relative, every
  dihedral angle between 10 and 165 degrees, one region.
- **M2 sphere, analytic distance.** Surface cells r/4, r/8, r/16: the volume error falls with an observed order
  between 1.5 and 2.5 (faces are chords of the sphere), every surface node on the sphere to 1e-9 r, angles within the
  target. TET10 with curved faces: the error is reported with its order.
- **M3 thin plate with a hole.** Wall thickness t, surface cell t: one region, and every point of a grid on the
  plate's mid-surface outside the hole lies inside a tetrahedron (no lost wall).
- **M4 the owner's assembly** (a private thin-walled part, not in the repository) and **M5 the cylinder head**
  (the owner's part, removed from the repository on 2026-09-27) at 2 mm surface cells: elements, nodes, quality, time, and the head in **one**
  face-connected piece (the voxel mesh at 2 mm fell into 18).
- **M6 conformity.** Every interior triangle is shared by exactly two tetrahedra, every boundary triangle by one, and
  the boundary is a closed 2-manifold.

Elements (T), in `tools/tettest.c`, on meshes that do not depend on the mesher where possible:

- **T1 element.** TET4 and TET10 stiffness symmetric, six zero eigenvalues and no negative one, constant strain
  reproduced exactly (1e-13).
- **T2 patch test.** A patch with distorted interior nodes and a linear displacement on its boundary: interior
  displacements and the constant stress exact to 1e-10, both orders.
- **T3 cantilever.** End shear, L/h = 10, nu = 0, Timoshenko's deflection as for the hex path: TET4 locks (below 80 %
  on the coarse mesh), TET10 within 1 % at the mesh size stated in the report, successive TET10 differences shrink.
- **T4 Kirsch.** A plate with a circular hole in tension, quarter model, the exact infinite-plate stress applied as
  traction on the outer boundary, nu = 0 (then the plane-stress field is also the exact three-dimensional solution):
  the stress concentration factor at the hole equals 3.00 within 3 % on TET10 (meshed by this mesher, curved
  faces). Reported, not a criterion: TET4 on the same mesh, and nu = 0.3.
- **T5 fillet.** A stepped flat bar in tension with shoulder fillets against a value read from a published stress
  concentration chart (Peterson's, Pilkey and Pilkey eds.); the chart, figure, geometry and value are recorded here
  by dated amendment before the run, from a source that can be opened. Within 5 % on TET10. If no openable source is
  found, T5 is reported as not met, not replaced.
- **T6 balance.** Equilibrium error below 1e-9 and external work equal to twice the strain energy to 1e-8, gravity
  reaction equal to the weight, a free eigenstrain expansion stress-free, a restrained one at the exact hydrostatic
  stress, a free body refused by the constraint check: the checks the hex path has.

Operations and app (O): `mesh_generate` with `method: "tet"` returns the section 4 report; a static analysis on
TET10 reproduces a voxel result on a block to 1 %; `results_probe`, `results_render` and `results_export` accept tet
results; the app's MESH step offers VOXELS and TETRAHEDRA and draws tet results on their own faces with a working
section view.

## 6. What the result files carry

The element type (`hex8`, `tet4`, `tet10`) and the node count per element, beside the connectivity; readers that
only know hex8 refuse a tet file with that reason instead of misreading it.

## 7. Amendment 2026-09-19: what the first runs showed, and what was added

Recorded after the first runs, before any criterion was changed. No criterion of section 5 was changed.

- **Thin walls.** At a surface cell equal to the wall thickness the axis-aligned plate kept its wall, but the plate
  tilted by 30 degrees fell into 7 pieces (646 of 751 mid-surface points covered): stuffing warps every inside vertex
  of a wall thinner than its lattice onto the surface. Added: a surface leaf whose deepest corner is shallower than
  half its size has the wall's thickness measured along the surface normal from its centre, and below 1.5 leaf sizes
  it is split, up to two levels below `surface_size` (`thin_levels`, default 2). So "surface cell t" means cells of t
  where the wall allows it and down to t/4 where it does not; the report counts the split leaves (`thin_splits`).
- **Pinches.** The owner's assembly and cylinder head then came out conforming but with small islands (32 to 38
  elements, under a millimetre) held to the part only along edges. Added: a boundary edge shared by four or more
  boundary faces marks a pinch; the leaves around it are split and the part stuffed again, three rounds at most
  (`pinches`, `pinch_splits`).
- **Warps that flatten.** On the assembly a warp flattened a transition tetrahedron to zero volume (the paper's
  no-inversion argument covers the pure BCC lattice, not the cones at grading transitions). Added: after warping, a
  background tetrahedron below 5 % of its lattice volume gets its warped vertices back (`unwarped_vertices`).
- **Slivers.** With the face-local diagonal rule standing in for the paper's parity rule, the assembly had elements
  down to 0.03 degrees. Added: node smoothing that only accepts a move raising the smallest of the incident
  elements' dihedral angles and of their supplements; surface nodes are projected back onto the surface after every
  move; topology is unchanged, so conformity is too (`smoothed_nodes`). It runs on nodes of elements below 15 degrees.
- **M6** counts a boundary edge in an odd number of boundary faces as a crack (non-conforming); an even number above
  two is pieces touching along an edge, reported separately and removed by the pinch refinement.
- **T5 source.** Peterson's Stress Concentration Factors, 3rd ed. (Pilkey and Pilkey, Wiley 2008), treats opposite
  shoulder fillets in a flat bar in tension in section 3.3.1 (p. 137; charts from p. 151), as its table of contents
  at https://toc.library.ethz.ch/objects/pdf/e01_978-0-470-04824-5_01.pdf shows (opened 2026-09-19). The chart
  itself could not be opened. The value used is the curve fit for that case as reproduced, with its formula and
  ranges, at https://amesweb.info/stress-concentration-factor-calculator/shoulder-fillets-in-flat-bar.aspx (opened
  2026-09-19), which cites W. D. Pilkey, *Formulas for Stress, Strain, and Structural Matrices*, 2nd ed., Wiley 2005.
  Geometry: D/d = 1.5, r/d = 0.25 (h/r = 1.0, inside the fit's 0.1 to 2.0), sigma_nom = P/(t d), nu = 0 so the
  extruded plate is the plane-stress problem. Kt = 1.7448 from the fit. The figure number stays an open question.

## 8. Amendment 2026-09-19 (step 4): solver, thin walls as an input, the app

- **Two-level conjugate gradients for TET10.** Block-Jacobi PCG did not converge on the owner's assembly (126 522
  TET10 elements, about 690 000 equations: relative residual 0.0175 after 20 000 iterations), and a direct factor of
  that size does not fit the machine. Above the direct-solver size a TET10 model is now solved by PCG with a symmetric
  two-level preconditioner: damped block-Jacobi smoothing (damping from a power estimate of the largest eigenvalue),
  and an exact coarse correction on the TET4 problem of the corner nodes, factorised once by the existing sparse
  Cholesky. The same assembly converges in 137 iterations (40 s). T3's direct-against-PCG check covers it.
- **`thin_wall_levels`** (0, 1 or 2; default 2) is an input of `mesh_generate`, reported: on a part that is thin-walled
  throughout, two levels multiply the element count (the owner's assembly at 2 mm: 126 522 elements without, 522 764
  with). The app uses one level. With none, walls thinner than about a cell can be lost (the assembly at 3 mm: 81 pieces).
- **App.** The MESH step offers VOXELS and TETRAHEDRA; TETRAHEDRA meshes TET10 with one thin-wall level; CHECK MESH on a
  tetrahedral mesh goes to 0.8 of the surface cell, capped near 250 000 elements (voxels: 0.7, 60 000). Results are
  drawn on the mesh's own faces (a TET10 face as four triangles through its mid-edge nodes), without the STL mapping the
  voxel path needs. The print analyses (thermal, LPBF, FFF, contact) refuse a tetrahedral mesh by name through
  `project_mesh_current`; the static structural chain uses `project_mesh_current_any`.

## 9. Amendment 2026-09-20: perforated walls are said in words

The architect's answer to the open question of section 8: one thin-wall level stays the default, and the report must
say when walls are not resolved instead of leaving a perforated mesh to look sound. The thin-wall pass now also tests
the leaves at the finest cell (and runs even with `thin_wall_levels` 0, where it only counts): a wall still thinner
than 1.5 of the finest cell is counted in `thin_walls_not_resolved`, and the mesh report carries the sentence, with
the finest cell, the number of places, the volume error and the size that would resolve them. The app's MESH step
prints the same sentence. The owner's assembly at surface 4 mm with one level: 11 279 places, volume -3.9 %.

The parity rule of section 1 stays an open item by the architect's decision: the measured angles with the smoothing
pass are accepted, and the rule is not to be chased.

## 10. Amendment 2026-09-27: the part of M5 removed

The cylinder head of M5 was removed from the repository for copyright, by the owner's decision, so M5 can no longer
be run from it. What sections 5 and 7 record about it stays as the record; M4, on the owner's assembly, remains.

## 11. Amendment 2026-10-03: printing-mesh diagnostics and robustness

Printing remains on layer-aligned HEX8, with the staircase limitations described in
[src/geom/README.md](../../src/geom/README.md). This change does not extend TET4/TET10 to printing or alter their
acceptance criteria. `tools/meshtest.c` declares PM1 to PM5 before their first run: exact anisotropic box geometry and
25 printing layers; analytic multi-body ownership and overlap counts; deterministic threaded abstention counts;
an area-weighted boundary-distance check; refusal of nonfinite or unrepresentable grids; translation-invariant closed
source volume. All 52 mesh assertions pass, including the existing cylinder and topology cases. Address and undefined
behaviour sanitizers pass the same suite.

The original three-axis inside vote is now retained for body ownership, so an abstaining ray cannot be ignored by a
later, different inside test. Shared sweep counters use atomic reductions. Closed source volume is evaluated about a
local reference with compensated summation. The 25,000-element anisotropic fixture has 25 complete layers and minimum
det J 0.005 mm3; the translated closed-source volume differs from the exact box by 1.82e-12 relative. The boundary mean
offset fixture reports 0.0307692211 mm against the analytic area-weighted 0.0307692308 mm. Largest-spacing thickness
warnings explicitly depend on wall orientation. These checks verify mesh construction and diagnostics; they do not
establish printed-part convergence or experimental validation.

## 12. Amendment 2026-10-08: sharp edges and corners kept (GOALS.md G20 step 5)

Why. On TET10 a curved face read low where it met a flat one: G17 step 4 recorded the C-ring 12.1 % under the
Winkler-Bach closed form, which CalculiX reproduces to 0.7 %. Probing the mesh found the cause: the stuffed surface
rounds every edge about one cell wide, and at the C-ring's apex the top face lay 0.28 mm low beside the inner edge, so
the section that carries the bending was thinner than the part's.

Method (`recover_features` in `src/geom/tetmesh.c`, the STL side in `tet_stl_features`). The surface patches that
selections use (`src/geom/patches.c`) give each STL triangle a patch. An STL edge is sharp where the faces of two
patches turn by more than 30 degrees, and a vertex where three or more patches meet is a corner. After stuffing, a
boundary node whose faces lie on two patches is moved onto the sharp edge between them, one whose faces lie on three
onto their corner, and a crease of the stuffed surface (two faces at a node more than 25 degrees apart) is moved onto
its nearest sharp edge. A move is kept whole when every element around it stays above the 5 degree floor; otherwise
in part, or whole with its neighbours relaxed; failing both it is refused and counted. Recovery and the quality
smoothing alternate until a round moves no new node (six rounds at most); smoothing keeps edge nodes on their edge and
corners where they are. A TET10 mid-node goes onto the sharp edge when both its ends lie on it, otherwise onto its
face's own patch, never onto the patch across the edge. `mesh_generate` reports `mesh.sharp_edges` (kept, angle_deg,
nodes_on_edges, nodes_on_corners, moves_refused). Without patches the mesher rounds edges as before.

Criteria, written in GOALS.md before the first run, and what the runs gave:

- (a) A box 1 x 0.7 x 0.5 turned 20, 30 and 10 degrees, TET10 at 0.1 through its STL (`tettest --features`, F1):
  every edge kept and the volume exact, both to 1e-6. **Not met** here; met since section 13. Edge samples lie within 5.27e-3 of the mesh's
  surface (4.63e-2 without the recovery), the volume within 1.51e-5 (1.24e-2), smallest dihedral 15.0 degrees; 110
  nodes went onto edges, 8 onto corners, 2 moves were refused. What remains is a few boundary faces that straddle an
  edge, which only a change of the mesh's topology (a flip or a split) removes. The criterion stays open; tettest pins
  the levels reached (a tenth of the cell, 1e-4, and five times better than without) as a regression guard. The first
  run, with one pass of recovery and no crease detection, gave 3.03e-2 and 1.61e-3 with 32 moves refused
  (`validation/print-strength/g20-tet-features-first-run.log`); the rounds, the crease detection and the relaxation of
  neighbours were added after it.
- (b) The C-ring of `tools/printmesh_study.py` on TET10 at 0.7 and 0.5 mm: R 2.0035 and 2.0042 (+0.0 %), the stated
  point 0.50 mm apart, -1.0 % from Winkler-Bach at 0.5 mm (it was -12.1 %). **Pass.**
- (c) A plate with a central hole in tension, Poisson's ratio 0 (`tools/hole_plate.py`, W 40, d 10, t 4 mm, a quarter
  on its symmetry planes, TET10 at 0.5 mm, 323160 elements): the largest von Mises stress anywhere is 32.457 MPa, on
  the hole's wall at the net section, +0.51 % from Heywood's 2 + (1 - d/W)^3 on the net section (32.292 MPa); the
  criterion was 3 %. **Pass.** The mesher before this change gives +2.58 % on the same case, its peak on the rounded
  edge where the hole meets the lower face, 1.5 mm off the net section.

What it changes for a user: a sharp inside edge is no longer rounded, so the stress there is singular and grows with
refinement, as linear elasticity says; the strength answer already reads its ratio outside such a corner's zone
(G17 step 4 (b)), and `results_query` says so when its largest value lies at one.


## 13. Amendment 2026-10-08: sharp edges and corners closed where moves cannot reach them (GOALS.md G17 step 9)

Why. After section 12, two TET10 meshes still read a false peak at a sharp edge: the C-ring of
`tools/printmesh_study.py` with its apex along a grid axis at 1 mm (12.1 % under Winkler-Bach) and the 2 mm plate with a
hole of `tools/printhole.py` at 0.75 mm (9.5 % under Heywood). Measured with the part's own patches and sharp edges: at
the ring, two boundary faces stood 51 and 38 degrees off the part's surface where its lower face meets the outer
cylinder, the walls of an empty wedge beside nodes already on the edge (a notch); at the plate, the corner where the
hole's wall, the lower face and the symmetry plane meet had no node, and faces 0.07 mm off the surface cut it off. The
turned box of section 12 (a) showed the same: its edge samples 5.27e-3 off the mesh, its volume 1.51e-5 short.

Method (`cap_features` in `src/geom/tetmesh.c`, after the feature rounds of section 12). An apex is a node on a sharp
edge or corner, or a new node on a true corner that has none (three or more distinct sharp edges meet there; a vertex
where one sharp edge only passes from one patch of a smooth surface to the next is no corner; the STL side's
`corner_get`). A boundary face near the apex that sees it from outside (by more than 0.02 of a cell) gets the element
from the apex to it, kept only when: its dihedral angles stay above the floor; its centroid lies inside the part and its
three new faces on the part's surface (within a tenth of a cell); no boundary node lies inside it and none of its edges
crosses a boundary face; no element already there (two rings around its nodes) holds one of five points inside it or
has its centroid in it; no face it makes is already held by two elements; and every edge it touches stays in two
boundary faces or none, counted over all the elements one apex adds. Rounds repeat until one adds nothing.
`mesh_generate` reports `sharp_edges.elements_added_at_features` and `corners_given_a_node`. The environment variable
`TETMESH_NO_CAPS` turns the pass off, for diagnostic comparisons only; `TETMESH_DEBUG` prints each candidate and why it
was kept or refused. Tried first and dropped: splitting a boundary edge where it crosses a feature and moving the new
node onto it, which the angle floor refused almost every time.

Criteria, written in GOALS.md before the first run, and what the runs gave (all **met**): (i) section 12 (a), the turned
box: every edge to 4.4e-16, the volume to 1.9e-14, smallest dihedral 15.0 degrees (`tettest`, F1); (ii) the two parts
(`tettest`, F2, with their stated ratios through the engine): the ring at 1 mm 2.0038, -1.1 % (was 1.781), 8 of 8 true
corners on a node, no face more than 30 degrees off the surface (the worst 6.9); the plate at 0.75 mm 1.5329, -1.0 %
(was 1.4018), 10 of 10 corners on a node, the worst face 4.4 degrees; (iii) nothing lost: the ring on its 45-degree
staircase 2.0121, 2.0035 and 2.0034 at 1, 0.7 and 0.5 mm, the ring along an axis 2.0009 at 0.5 mm, the plate -0.7 %
at 1 mm and -0.5 % at 0.5 mm, section 12's (b) and (c) as before (`hole_plate.py` +0.51 %), `tettest` 105 passed. The
added elements per mesh are few: 2 on the ring, 4 and 2 corners on the plate, 10 and 4 corners on the 45-degree ring.
