# Beauty renders: lab results filmed in Blender

Up: [the lab](../../docs/lab/README.md) · files: [lab2ply.c](lab2ply.c), [beauty.py](beauty.py), [film.sh](film.sh)

The lab's own renderer (labfilm, `src/lab/labview.c`) draws a result the way an engineer reads it: a colour map on
flat-lit surfaces, a colour bar, the time. This pipeline films the same result the way a product is photographed:
machined steel in a studio, soft light and shadow, a slow camera move with a little depth of field. It is for the
README, posts and talks, where the lab should look as good as what it computes.

**The honesty rule.** The geometry and the field come from the simulation, untouched: every vertex is where the
solver put it in that frame, every colour is the field's value at that vertex through one fixed colour ramp, the same
for the whole film (labfilm's own range, the 0.5 and 99.5 percentiles over all frames, unless one is given). Materials,
light, the studio, the camera and its motion are presentation, chosen here. A frame between two of the result's
positions is never invented: `lab2ply --film N` may blend two neighbouring positions linearly (positions and field, the
cells of the nearer one), and the first film needs none, because its run computed one position per film frame. The
caption of every film says which is which.

## The pipeline

```
RESULT.lab  ->  build/lab2ply  ->  frame_NNNN/PART.ply, manifest.json  ->  blender beauty.py  ->  frame_NNNN.png  ->  ffmpeg  ->  film.mp4, film.gif
```

1. **[lab2ply.c](lab2ply.c)**, C on the lab's own reader (`src/lab/labio.c`): for each frame, the outer surface of
   every part as binary PLY: of hexahedral cells the faces used by one cell only (collapsed hexahedra give triangles;
   the format has no tetrahedra, a tetrahedral solver writes its surface as triangles and quadrilaterals, taken as they
   are), of 3D blocks the sides no other block covers, points parts as point clouds. Per vertex: position, a normal
   split at creases (35 degrees), the field's value, the value mapped to 0..1, and a rest position (where that material
   point is in the first frame, so a texture turns with its part). `--cut A0 A1` draws a section view: the wedge between
   two angles about the z axis taken out of the named parts, the cells cut along its half-planes, the field on the cut
   faces interpolated along the cells' edges. Unlike a solver's cutaway (whole cells left out by their centres, which
   pop in and out as a part turns), the cut is plane and stands still.
2. **[beauty.py](beauty.py)**, a Blender script (`blender -b -P tools/render/beauty.py -- PLYDIR OUTDIR`): a
   seamless round studio (a floor that curves up into a wall all around, so the camera can orbit), six area lights
   (key, fill, two strip rims, an overhead softbox and a band behind that the gears' faces mirror), machined steel for the
   gears with the field as a colour ramp blended into the base colour (steel where the field is low, then the house's
   "ember" sequence: gold, orange, red; the stressed metal turns from a mirror into a tinted surface so the colour reads
   in any light, with a faint glow at the top of the range), a darker ground steel for rings, black anodised aluminium
   for what the solver draws but does not compute (carriers, pins, shafts). The camera orbits slowly and pushes in,
   eased at both ends, with light depth of field. Cycles on the CPU with adaptive sampling.
3. **[film.sh](film.sh)** runs both and encodes the frames with ffmpeg: an H.264 MP4 (crf 18) and a GIF for READMEs
   (palettegen and paletteuse), each with a caption: the result's title, the field's legend and range, and a line
   that says what is computed and what is Blender's.

**Denoising.** Ubuntu's Blender (4.0.2 from the noble archive) is built without OpenImageDenoise, so Cycles has no
denoiser of its own. beauty.py filters each frame itself, on Cycles' passes: the edge-avoiding a-trous wavelet filter of
Dammertz, Sewtz, Hanika and Lensch (High Performance Graphics 2010), five passes of a 5 x 5 B3 spline with holes of 1 to
16 pixels, its edges stopped by the normal, the depth and the luminance of each pixel, the luminance weights scaled by
the local noise and carried through the passes as in SVGF (Schied et al., High Performance Graphics 2017). It filters
the light divided by the albedo and multiplies the albedo back, so the field's colours and every geometric edge stay
sharp, and it keeps the emission out of the filter. Lone fireflies (a pixel over four times its neighbours' median) are
brought down first. `--denoise nlm` uses ffmpeg's non-local means instead, `--denoise none` none.

## Commands

On a compute node (the renders are long; never on the shared laptop), from the repository's root:

```bash
cc -O2 -std=c11 -Wall -Wextra -o build/lab2ply tools/render/lab2ply.c src/lab/labio.c -lm    # or let film.sh do it
build/lab2ply RESULT.lab PLYDIR --field stress --check | tail        # every frame, a report per part
blender -b --factory-startup -noaudio -t 6 -P tools/render/beauty.py -- PLYDIR STILLS --still 150   # one still
bash tools/render/film.sh RESULT.lab OUTDIR --field stress --cut -110 -10 --cut-parts ring,plate + --story 10
```

Everything after `+` goes to beauty.py; the head of [beauty.py](beauty.py) lists its options (frames, size, samples,
threads, the camera's path, lens, depth of field, look, exposure). Blender comes from Ubuntu's archive
(`apt-get install blender`; 4.0.2 on Ubuntu 24.04), ffmpeg likewise. From the Mac, through the node's lane:

```bash
python3 tools/remote.py run desktop --name beauty-film -- 'bash tools/render/film.sh RESULT.lab $TMPDIR/beauty/film --field stress + --story 10'
```

## Verification of the exporter

On the 69:1 Wolfrom stage (`examples/lab/robot_wolfrom_stage.json` as it is, 36 positions, the solver's own cutaway;
14 parts, 504 part frames):

- every surface closed and consistently oriented: 0 open, 0 non-manifold and 0 misoriented edges in all 504, Euler
  characteristic 0 for every part with a bore (a torus) and 2 for the cut rings and the carrier plate (a ball);
- the volume inside each surface equals the volume of its cells (the divergence theorem over each cell's faces) to all
  nine digits printed, in every part: no face missing, none extra, none turned inward;
- the surface's bounds equal those of the part's nodes (the sun: plus and minus 9.99990 by 9.90592 mm, 0 to 8 mm);
- the field: the range lab2ply chooses, 0 to 3.65263e+07 Pa, is the one labfilm chooses for the same result; on the last
  frame the surface carries the part's whole nodal maximum (the sun 8.52485e+06 Pa, the fixed ring 4.63444e+07 Pa, the
  numbers `labfilm --info` prints), its minimum lying inside the part.

The section view (`--cut -110 -10 --cut-parts ring,plate`) on the same gearbox run with whole rings (300 positions,
4,200 part frames): every cut surface closed (0 open, non-manifold or misoriented edges; Euler characteristic 2), the
volume inside every uncut part's surface equal to its cells' in every frame, and the cut against the closed form of a
wedge: the 72-tooth fixed ring keeps 0.722222 of its volume in all 300 frames, 260/360 = 0.722222 exactly, because the
100 degree wedge holds exactly 20 of its tooth pitches; the 69-tooth output ring, whose wedge holds 19.17 pitches,
0.721705 to 0.722189; the carrier plate 0.722221 to 0.722224. On the fixed ring the plane cut also keeps exactly the
volume the solver's own cutaway keeps (6.89510664e-06 m^3 both): the wedge's planes fall on its cells' boundaries.

## Time

On the desktop node (Intel i7-7740X, 4 cores, 8 threads; Blender on 6 threads at nice 19, while the node ran two
other solvers and another session's test suite), a 1920 x 1080 frame of the Wolfrom stage (14 parts, 280,000 triangles)
with at most 40 samples per pixel (adaptive, threshold 0.05): 110 s of path tracing and 20 s of filtering; with 24
samples, 70 s and 20 s. The film uses at most 32: the helical gearbox (20 parts, 400,000 triangles) took 93 s in all
for a 1920 x 1080 frame with the node otherwise quiet. A still at 960 x 540 takes 30 to 35 s, a good way to try a look.
lab2ply writes the 300 frames of that gearbox (3.2 GB of PLY) in 2 minutes.

## Films

The first film is ready to render and not rendered yet: the desktop node's disk failed under it (below). Its recipe,
one job on a node, about 1.5 hours of solving and 8 hours of rendering:

```bash
python3 tools/remote.py run desktop --name beauty-film-helical -- 'O=$TMPDIR/beauty/helical && mkdir -p $O &&
  python3 -c "import json,sys; s=json.load(open(sys.argv[1])); s[\"picture\"].pop(\"cutaway_deg\",None); s[\"run\"][\"positions\"]=300; json.dump(s,open(sys.argv[2],\"w\"))" examples/lab/robot_gearbox_two_stage_helical.json $O/scenario.json &&
  nice -n 19 ./build/labrun $O/scenario.json $O/result.lab &&
  CAPTION="A two-stage helical planetary gearbox, 25:1, for a robot joint" LEGEND="von Mises stress" \
  bash tools/render/film.sh $O/result.lab $O/film --field stress --cut -110 -10 --cut-parts ring,plate + --dist 6.4 5.6 --story 6'
```

That is [robot_gearbox_two_stage_helical.json](../../examples/lab/robot_gearbox_two_stage_helical.json) (two helical
planetary stages, 14 gears, 2 N m in) at 300 positions over 105 degrees of input, the solver's cutaway taken out so that
the rings stay whole: one simulated position per frame of a 10 second film at 30 frames per second, nothing blended, and
the plane section through both rings and both carrier plates.

What the runs showed, and what a film of them shows with them. The train's output torque should be the input's torque
times the ratio at every position, since each stage is statically determinate:

- the two-stage helical gearbox (25:1, 2 N m in): 50.008 N m over its first 13 positions (to 4.2 degrees), then a
  sawtooth between 45.6 and 49.2 N m with a period of 2.8 degrees of input, through the 181 positions computed before
  the node failed;
- the 69:1 Wolfrom stage ([robot_wolfrom_stage.json](../../examples/lab/robot_wolfrom_stage.json), 300 positions): below
  99 % of 34.5 N m at 152 of them, and in seven windows of 1 to 2 degrees, every 12.6 degrees of input (32 positions),
  jammed: the sun meshes carry 304 N on both flanks, the fixed ring nothing, the output only the 0.5 N m that goes in. Its
  stress then leaves the rings for the sun and comes back, a blink in a film every 1.3 seconds, which is why the helical
  gearbox goes first.

Both are questions for the gear-train solver (`src/lab/mechanism/mtrain.c`), not for the renderer. Answered on
2026-10-22 (docs/lab/mechanism.md, geartest M22): the solver took the teeth's contacts at the train's kinematic pose,
while a high-ratio train turns its input degrees further before it carries its load; it now finds them again where the
train settles. The gearbox's output then stays at 50 N m within 0.08 %; the Wolfrom stage's jams are gone, and it keeps
a ripple of up to 2 % where a tooth's sharp corner comes into mesh.

**The helical gearbox's film (2026-10-23 and 24).** The recipe above, with the fixed solver: the scenario at 300
positions over 105 degrees on a compute node (2.8 hours, 1117 s of it building the 14 gears' models side by side on 3
cores, 11.7 GB at most), the output torque 49.98 to 50.01 N m at every frame (computed ratio 24.990 to 25.007), the peak
von Mises stress in a tooth 49 to 57 MPa (the legend's 0 to 30 MPa is lab2ply's range of the nodal stress). lab2ply wrote
4.5 GB of surfaces; Blender rendered 1920 x 1080 frames, the first 180 on four CPU threads (about 2.5 minutes a frame)
and the rest on an RTX 3050 Ti with `OPENPHYSICS_RENDER_DEVICE=CUDA` (25 to 40 s a frame; the same frame from both
differs by 1.2 of 255 on average). `film.sh` (FILM_ONLY) encoded the MP4 and the README's GIF (640 px, 8 frames a
second).

**The node's disk.** Inside the desktop's Linux, `df` shows the free space of its virtual disk (900 GB); the real limit
is the Windows drive the virtual disk lives on. On 2026-10-21 at 22:46 that drive (C:, 475 GB) filled up, the virtual
disk could not grow, Linux's root went emergency read-only, and the two runs on the node (the Wolfrom HD flagship and the
helical gearbox for this film) stopped. This pipeline's scratch had grown to about 13 GB that day: a 300-position
gearbox result is 4 to 5 GB, its surfaces 3 to 5 GB, the frames about 1 GB. Check the Windows drive first
(`Get-PSDrive C` over the node's SSH), and delete each result's surfaces once its film is encoded.

## Generative design

A topology optimisation computed by the engine, filmed. Files: [../gendesign.c](../gendesign.c) (the case, the run, the
surfaces), [topofilm.py](topofilm.py) (Blender), [topoencode.py](topoencode.py) (captions, MP4, GIF, still); the
optimiser's contract with the additions this needed: [topology-optimisation.md](../../docs/contracts/topology-optimisation.md),
amendments 3 and 4.

**The case** (drawn for the film, not copied): an engine-mount bracket of Ti-6Al-4V as built by laser powder bed fusion
(`ti6al4v_lpbf` from the library: E 110 GPa from the EOS Ti64 data sheet, Poisson's ratio 0.342 from TIMET). A design
space of 160 x 100 x 72 mm; four M10 bolts at x = +-62, y = +-34 mm, with bosses of 24 mm held solid and clamped on the
mounting face, and room kept free above them for the bolt heads; a clevis of two lugs held solid, with a 12 mm pin along
y at 54 mm height, room for the rod end between the lugs and for the pin's head and nut outside them. The load: 25 kN
through the pin, 30 degrees from vertical towards +x, spread over the half of each bore the pin presses on. Case and
load are symmetric about y = 0, so half is computed with that plane held in y, and the other half is its mirror image.
The angle was 45 degrees first: the coarse run at 2 mm voxels then left the two bosses on the far side unconnected
(criterion 4 below), since at that angle their bolts carry almost no axial load; at 20 and 30 degrees all four bosses
are connected.

**The optimiser**: modified SIMP (p = 3, E_min 1e-9), the sensitivity filter, optimality criteria with a move limit of
0.2, a volume fraction of 12 %, started from the whole design space solid and taken down by a volume continuation of
0.5 % per update (contract amendment 3), so that the film shows the part emerging from the solid block; then the
iterations at 12 % until the change criterion or the iteration limit. Every solve is the engine's static analysis of the
voxel mesh (incompatible-mode hexahedra), by conjugate gradients preconditioned with the new geometric multigrid
(amendment 4), warm started from the previous iteration's displacements.

**Acceptance criteria, written on 2026-10-22 before the first full run** (and never loosened afterwards):

1. *Compliance falls and converges.* After the volume reaches its target, the compliance at the end is lower than at the
   first iteration at the target, and over the last ten iterations it varies by less than 0.1 %.
2. *Volume.* The final volume fraction is 12 % within 0.5 % of itself (0.1194 to 0.1206).
3. *Energy.* In every solve `f^T u` and twice the strain energy agree to 1e-6 (the optimiser reports the worst).
4. *One part.* The elements above half density form one face-connected piece that holds every bolt boss and both lugs
   (in the half model: one piece holding its two bosses and its lug, and reaching the symmetry plane, so that with its
   mirror image it is one part).
5. *Every solve converged*: true relative residual at most 1e-9 (the solver stops at 1e-10 on its recursive residual).
6. *Stiffness.* The part is at least twice as stiff as the same volume spread evenly: its compliance at most half that
   of the design space filled uniformly with 12 % of the material's stiffness (one more solve).
7. *Surfaces.* Every surface written for the film is closed and consistently oriented: no open, non-manifold or
   misoriented edge.
8. *Budget.* The run ends within 12 hours on 4 threads, below 20 GB of memory.

**The recipe**, from the repository's root. The optimisation on a node with four threads to spare, the film on a node
with Blender (here the Surface for the first and the desktop for the second; the densities moved between them):

```bash
make build/gendesign
# the optimisation: the density of every iteration in RUN/snap, the criteria printed and in RUN/summary.json, pictures
# for the owner's phone every 10 minutes (--story)
nice -n 19 ./build/gendesign run RUN --h 0.8 --rmin 2.5 --vstep 0.005 --iters 350 --threads 4 --story
# criterion 7: every surface the film shows built once and checked; only the manifest is kept
./build/gendesign ply RUN FILM/ply --domain --final --dense-to 196 --sparse 4 --taubin 20 --check-only
./build/gendesign pic RUN look.png          # a quick look at the final design through the software renderer
# the film: Blender builds each surface from RUN's densities just before its first frame and deletes it after its
# last; then the MP4, the GIF and the still, captioned from RUN's own numbers
DENSE_TO=196 THREADS=4 bash tools/render/gendesign_film.sh RUN FILM
```

`build/gendesign solve RUN --iter K` solves one kept design again (solver, threads and tolerance as options; the
multigrid's smoother can be varied through VMG_DEGREE, VMG_SAFETY, VMG_RATIO, VMG_STEPS and VMG_COARSE, documented in
`src/fem/vmg.c`), to compare solvers on the real problem.

**What the runs showed** (2026-10-22; the Surface node for the optimisations, the desktop for the film; both nodes
shared with other lanes the whole time, so every time below is a time under load).

*The solver.* On the bracket at 2 mm voxels (62,249 elements in the half model, 186,000 equations) a solve took 965
block-Jacobi conjugate-gradient iterations and 8.7 s, and 14 to 18 multigrid iterations and 1.5 to 2.5 s. At 1 mm
(496,936 elements) multigrid needed 13 to 23 iterations, about 18 s for a whole optimisation iteration. Block-Jacobi
iterations grow with the inverse voxel size; multigrid's do not, which is what made 0.8 mm affordable. During the
continuation, while the first voids are half emptied (densities falling 0.2 per update towards the floor), multigrid
needs more for a few iterations: on the coarse run 14 rising to 69 at iteration 14 and back to 16 two iterations later;
on the full run 16 rising to 272 at iteration 17, then 94, then 21 to 25 for the rest of the run. Tried on the coarse
run's hardest solve: a larger eigenvalue safety factor (1.3) or 30 Lanczos steps change nothing (78 and 76 iterations
against 75), a cubic smoother 61; so the spike is the coarse levels' trilinear interpolation meeting half-empty cells,
not the smoother's eigenvalue estimate.

*The warm start.* The 30 degree coarse run solved cold (each solve from zero) needs 4,737 multigrid iterations in its 208 solves, warm 3,843, 19 % fewer, for the identical design (final compliance 0.490536 J both). On the small box of topotest T5, whose design moves far in every iteration, it saves 1 %.

*The coarse runs* (2 mm voxels, a 5 mm filter, the same continuation):

| load | iterations | stiffness against the even spread | pieces | criteria failed |
|---|---|---|---|---|
| 45 degrees | 204, converged | 2.14 | one boss pair unconnected | 4 |
| 30 degrees | 207, converged | 1.99 | one part | 6 |
| 20 degrees | 203, converged | 1.98 | one part | 6 |

*The full run* (Surface, 4 threads, 0.8 mm voxels: 200 x 63 x 90 cells in the computed half, 980,505 elements,
1,029,021 nodes, about 3.1 million equations; 1.96 million voxels in the whole part; filter 2.5 mm). 350 iterations in
5.67 h (20,425 s, 96 % of it in the solver), peak memory 5.9 GB. The volume reached its target at update 176; the run
then stopped at its iteration limit of 350 with a largest density change of 0.014, so the optimiser's own change test
(0.01) was not met, while the compliance had settled (criterion 1 below). Multigrid: 9,746 iterations in 351 solves,
28 on average, 21 to 35 outside the spike, at most 272. Compliance: the solid design space 0.20974 J, at the first
iteration at the target 0.47919 J, final 0.45749 J; the same volume spread evenly 1.00567 J.

| criterion | result | |
|---|---|---|
| 1 compliance falls and converges | 0.47919 J at the target, 0.45749 J at the end; the last ten within 0.006 % | PASS |
| 2 volume | 0.12000, 0.000 % off | PASS |
| 3 energy | f^T u and 2 U at most 3.4e-12 apart | PASS |
| 4 one part | 114,480 elements above half density in one piece, holding both bosses and the lug and reaching the symmetry plane | PASS |
| 5 every solve converged | worst true residual 1.0e-10 | PASS |
| 6 stiffness | 2.20 times as stiff as the even spread (45.8 % of the solid block's stiffness with 12 % of its material) | PASS |
| 7 surfaces | 220 surfaces checked, no open, non-manifold or misoriented edge | PASS |
| 8 budget | 5.67 h, 5.9 GB | PASS |

The largest von Mises stress in the final design, element means of the solid's stress, is 195 MPa under the 25 kN load,
against the 1130 MPa yield strength of the library's record: this design minimises compliance and has no stress
constraint.

*The surfaces.* Every surface the film shows, 220 of them, closed and consistently oriented. The final part: 421,960
triangles, one piece, Euler characteristic -12, enclosing 117,789 mm^3 against 117,228 mm^3 of voxels above half
density (0.5 % more, the smoothing of the voxel steps). The solid block of iteration 0: 1.23 million triangles.

*The film.* 444 frames at 1920 x 1080 and 24 frames per second, 18.5 s: the brief (36 frames, the solid block with
the load's arrow), the carving (216: every iteration up to 196, then every 8th to 350), the turn (120) and the stress
(72). Cycles on the desktop's CPU, 12 samples per pixel (adaptive, threshold 0.05) and the a-trous filter: about 78 s
a frame on 6 threads once the node was quiet (90 to 110 s on 4 threads while it was shared, 20 s of it the filter),
plus 20 to 40 s to build each carving surface from its density; about 11 hours of rendering in all. The first 213
frames were rendered while the optimisation was still running. Files (in the owner's highlights folder):
`generative_design_1080p.mp4` (H.264, crf 18, 10.7 MB), `generative_design.gif` (800 x 450, 10 frames per second,
every 4th carving frame and every 6th of the turn, 128 colours, 5.6 MB) and `generative_design_still.png` (1200 px, a
frame of the turn). Not made: a 4K version, about four times the render time on this CPU.

## The metal printing film

Files: [printbuild.py](printbuild.py) (a part built by the engine), [print_amb2022_bridge.json](print_amb2022_bridge.json)
(its scenario), [nvt2ply.c](nvt2ply.c) (the build's result file as surfaces), [nvt2ply_check.py](nvt2ply_check.py) (the
exporter's check), [printfilm.py](printfilm.py) (the film in Blender), [printfilm.sh](printfilm.sh) (all of it on a node).

A part grows layer by layer on its build plate, the residual stress building up as it grows; then it is cut from the
plate and springs back, and a slow turntable closes the film. It is the engine's inherent-strain LPBF build
(`lpbf_build_run`, [docs/contracts/lpbf-build.md](../../docs/contracts/lpbf-build.md)): every layer born stress-free on
the deformed part below it, its inherent strain an eigenstrain, then the cut. Every vertex is where the build put its
node, the displacement multiplied by a factor the caption states, and every colour is the computed von Mises stress at
that node through one fixed scale. The plate, the studio, the light, the materials, the camera, the timing and the turn
are presentation, and so are the frames between the stored state before the cut and the one after. The method computes
no temperature, so nothing in the film suggests one: the stress scale is a cool one (steel, steel blue, blue, indigo,
violet; a warm scale on a fresh layer reads as heat) and nothing glows.

### The part: NIST AM-Bench's 2022 bridge

The bridge (AMB2022-01, Inconel 718, 75 x 5 x 12.5 mm, twelve legs on a 19 mm base) is AM-Bench's distortion experiment:
NIST printed it, cut its legs from the plate with a wire, and measured how far its ridges rose. Its geometry is NIST's
own CAD file, `AMB2022-01-AMMT-PartCAD.STL` (a US government work, fetched with the owner's approval for GOALS.md G20
step 3, SHA-256 d92f368a...; not held by this repository: `printbuild.py --stl PATH` reads it from wherever it is, and
the scenario's hash is checked). The scenario is the blind prediction of GOALS.md G20 step 3 (`tools/g20_bridge.py
predict`) as it stands, but for the element size:

- the inherent strain: -3.790e-3 in x, y and z, *calibrated*: fitted on a published measurement, the free-end ridge of
  NIST's 2018 IN625 bridge (AMB2018-01, 1.276 mm after the cut), at 0.5 mm layers and elements. Nothing from any
  private dataset;
- the material: IN718's room-temperature datasheet constants (Special Metals, E = 29.0e6 psi, nu = 0.294); the
  displacement of a body loaded only by an eigenstrain does not depend on E, the stress does;
- 25 simulation layers of 0.5 mm, the layer the strain was calibrated at (halving the element size at the same layer is
  a discretisation change, lpbf-build.md V6; halving the layer would change the physics);
- the cut: the bottom 0.5 mm of all twelve legs from 20 mm from the pointed end on, the base staying on the plate, as
  GOALS.md G20 step 3 inferred it ("as close to base plate as possible", no height stated);
- no supports: NIST built the bridge without, and the overhang rule (45 degrees) finds none under its arches.

**The coarse run reproduces the public record.** At the record's own 0.5 mm elements (29,356 hexahedra, 7.4 minutes on
the desktop node with 4 CPUs while it ran other jobs), the six measured ridges rise -0.0057, 0.0229, 0.1518, 0.3851,
0.7421 and 1.2873 mm after the cut, the frozen blind prediction's -0.006, 0.023, 0.152, 0.385, 0.742 and 1.287 mm to
every digit it gives. NIST measured -0.0025, 0.0154, 0.1826, 0.5232, 1.0583 and 1.8088 mm (expanded uncertainty
0.0043 mm); the blind comparison's median absolute error is 29.4 % against the 15 % criterion, which it fails, and the
free end reads 29 % low (GOALS.md G20 step 3, docs/map/PHYSICS.md).

**The film's run**, at 0.25 mm elements and the same 0.5 mm layers: 235,072 hexahedra and 263,268 nodes, 25 layers and
the cut in 26 solves (by the shared solver's own rule a direct factorisation up to 250,000 unknowns and conjugate
gradients beyond, where the upper layers are), 38.6 minutes on the Surface node with 4 CPUs at nice 19 while it ran
other jobs, a 240.7 MB `results.nvt`, the last solve in equilibrium to 3.4e-11. The ridges rise -0.0059, 0.0227, 0.1528, 0.3892, 0.7506 and 1.3012 mm: the free end 1.1 %
above the coarse run, inside the 5 % that V6 allows a halved element, and 28 % below NIST's 1.81 mm (median absolute
error over the six ridges 28.6 %). Before the cut the part's largest displacement is 0.129 mm; after it 1.414 mm, the
free end's top rising 1.397 mm. Drawn ten times larger, that is a 14 mm curl on a 12.5 mm tall part, and the caption
says so.

**The stress is the elastic model's.** The blind prediction is elastic, and so is the film: the stress is the elastic
response to the calibrated strain, about E times the strain over (1 - nu), 1.1 GPa, through the bulk of the built part,
2.34 GPa at the 99.5 percentile (the top of the colour scale) and 7.1 GPa at the corners where the part meets the rigid
plate, a singularity that grows as the mesh refines (5.1 GPa at 0.5 mm). That is above what the alloy can carry: a real
part yields there. The colours show where this model loads the part, not the stress a printed part keeps, and the legend
says "elastic".

### The exporter and its check

[nvt2ply.c](nvt2ply.c) reads `results.nvt` (a JSON header, then the arrays it lists) and writes, per state, one PLY per
element group (`part`, `support`, `plate`): the outer faces of the elements alive at that stored time (alive as
lpbf-build.md section 5 defines it; outer: shared with no other alive element, of any group), one vertex per node and
face direction (so every voxel edge stays sharp and a bent face stays smooth), each vertex carrying its deformed
position (rest plus F times u), normal, von Mises stress and its 0..1 value, its rest position, its true displacement and
its node's index. A state is a stored time or a blend between two (`--blend A B K`); the film blends the last layer into
the cut over as many states as the cut has frames (48 in the bridge's film). In this linear model the displacement blend is the equilibrium under the fraction s of the force
the cut releases; the blended stress is an interpolation between two computed fields.

[nvt2ply_check.py](nvt2ply_check.py) checks every state by a second implementation (numpy, from the `.nvt`), with its
criteria written before the exporter's first run: C1 the triangles are twice the group's outer faces; C2 the vertices
are exactly the distinct (node, direction) pairs of those faces; C3 every vertex's rest position, displacement, position,
stress and colour value equal the node's to float32 rounding (1e-6 of the state's largest); C4 the union of the groups is
closed and consistently oriented; C5 the volume inside it equals the alive elements' to 1e-9 and is positive (outward);
C6 at a probe node, the part's surface node that rises most at the end, the PLY's displacement and stress are printed
beside the `.nvt`'s. On the coarse run: 85 states (26 stored times and 59 blends), all six criteria in every state; at
the probe (node 442, the free end's lower corner) after the cut, PLY u_z 1.384418 mm against the file's 1.384418 mm. On
the film's run: 73 states (26 stored times and the 47 blends of a 48-frame cut), 4.13 million vertices and 6.91 million
triangles in all (323 MB), all six criteria in every state; at the probe (node 17867, on the free end) after the cut, PLY
u_z 1.397405 mm against the file's 1.397405 mm and von Mises 398.79 MPa against 398.79 MPa.

### The film

[printfilm.py](printfilm.py) reuses the first films' studio, lights, PLY reader and denoiser ([beauty.py](beauty.py)) and
adds what a build needs: the plate under the part (surface-ground steel, the grinding along x, its top a hair below
z = 0), the part in a satin printed metal whose colour is the stress through a cool scale (the metal's own grey up to a
tenth of the range, then steel blue, blue, indigo and violet, the metal turning from a mirror into a tinted satin as the
stress rises), supports in matte grey, and a timeline: the empty plate, every stored layer held for a few frames while
the camera turns slowly round the part (an eased orbit from 74 to 50 degrees of azimuth, 22 to 17 degrees high), the
finished part, the cut blended over the exporter's states, a pause, and a turntable of the plate and the part (360
degrees, eased) under fixed lights. [printfilm.sh](printfilm.sh) exports, checks, renders and encodes, with captions
timed from the film's own timeline: the title and the layer being built, what the cut is and that its frames are
interpolated, the legend with its range and "elastic", the factor ("distortion drawn 10 times larger"), the credit, and
over the turntable this run's free end against NIST's measurement with the blind record's median error and criterion.

The bridge's film runs at 24 frames per second: 18 frames of empty plate, 25 layers of 6 frames, 18 frames on the
finished part, 48 frames of cut, 18 after it and 192 of turntable, 444 frames or 18.5 seconds.

Looked at before the long render, at 640 x 360 and 1280 x 720: the first look read as pale blue glass on a blown-out
plate, small in the frame. The metal was made darker and kept metallic, the plate darker and rougher with its top light
dimmed, the camera brought closer, and the stress scale muted so that the part reads as metal first: the stressed deck
steel blue, the legs back to bare metal once the cut has freed them, violet where the base is still held by the plate.
A 1280 x 720 frame at 24 samples took 144 to 157 s on the desktop node with 4 threads at nice 19 while the node ran at a
load of 20 to 22 (19 to 24 s of it the denoiser). The film itself, 1920 x 1080 at up to 24 samples: about 95 s a
frame on the desktop node with 4 threads and little else running (16 s of it the denoiser), so about 12 hours for the
444 frames; the hero still, 2400 x 1350 at 64 samples and scaled to 1200 px, 20 minutes on the busy node.

### The recipe

On the nodes, each in its own lane (`tools/remote.py --lane NAME`), the build where the solver is welcome and the film
where Blender is:

```bash
# the build: 39 minutes on the Surface node at 0.25 mm (7.4 minutes at 0.5 mm with --element-size 0.5, the check that
# the pipeline gives the public record's numbers); NIST's STL copied to the node first, its hash checked by the scenario
python3 tools/remote.py run surface --name bridge025 -- 'ulimit -v 8000000; nice -n 19 taskset -c 0-3 python3 \
  tools/render/printbuild.py tools/render/print_amb2022_bridge.json $TMPDIR/bridge025 --stl $TMPDIR/AMB2022-01-AMMT-PartCAD.STL --story'
# the film, from that build directory (results.nvt, record.json) on the node that has Blender
python3 tools/remote.py run desktop --name printfilm -- 'FPS=24 CUT_FRAMES=48 bash tools/render/printfilm.sh \
  tools/render/print_amb2022_bridge.json $TMPDIR/bridge025 $TMPDIR/film + --intro 18 --layer-frames 6 --hold 18 --turn-frames 192 --story 12'
```

`printfilm.sh` stops if `nvt2ply_check.py` fails, keeps the frames it already has when it is run again, and deletes the
surfaces once the film is encoded (`KEEP_PLY=1` keeps them).

### Any part

The scenario names everything a build needs, each number with its unit in its key and each value with its source:
`part` (the STL, its SHA-256, its units, `mirror_x`, `z_offset_mm` and an optional `place` passed to `geometry_place`:
`up_axis`, `rotate_z`, `rotations`), `material`, `build` (layer, element sizes, orientation, optional `plasticity`),
`inherent_strain` (three numbers, a provenance and a source of at most 159 characters), `cut` (or null), `supports`
(passed to `lpbf_build_run` as it is, with its type and parameters and their provenance, docs/contracts/supports.md; or
null), `probes` (points in the part's own frame, read before and after the cut), `record` (a public measurement to
show beside the run, or nothing) and `film` (the displacement factor, the plate's margins, the caption). The exporter
writes supports as their own `support.ply`, drawn matte grey, and the support removal after the cut as one more
stored state the film blends into like the cut. A part on supports needs `z_offset_mm` (its height above the plate)
and a strain whose provenance it states; without a published one, the strain is a demonstration value and the caption
says so. The exporter draws uniform voxel meshes only (`adaptive_mesh` is refused: a coarse face meeting four fine ones
would be drawn inside the part). Checked on a part that needs supports: a 20 x 8 x 2 mm slab 3 mm above the plate on
homogenised supports (stiffness fraction 0.3 and the bridge's strain, both assumed for the test), cut free at 0.25 mm,
6,400 elements at 0.5 mm, 72 s on the Surface node. Its first stored times hold supports only, the part's face on them is
not drawn while they stand, the cut and the supports go in one step (lpbf-build.md section 12), and all 22 states (11
stored times and 11 blends) meet the six criteria.

## Not done yet

- Points parts (particles) and blocks are exported but beauty.py films surfaces only: spheres for particles and a
  look for 3D blocks (a melt pool) are the next things to add.
- The cut is a wedge about the z axis only.
- No motion blur; beauty.py has no turntable of the part itself (printfilm.py has) and one look ("light"; "dark"
  exists, untuned).
- Each frame is filtered alone; a filter that also uses the frames before it (SVGF's temporal half) would let a film
  use fewer samples per frame.
