# Example: two versions of a wall bracket

The release scenario: *"Here are two versions of my bracket. They attach through these mounting regions and carry
approximately 3 kg here. Compare their stiffness and mass, explain the assumptions, and tell me what information
could change the comparison."*

| Design | Geometry | Volume |
|---|---|---|
| A | L bracket: 60 mm wall plate and 80 mm arm, both 8 mm thick, 40 mm wide, sharp inner corner | 42,240 mm³ |
| B | the same with a 24 mm inner chamfer (a triangular fill between wall plate and arm) | 53,760 mm³ |

Frame (mm): x runs along the arm away from the wall, y across the width, z up. The back face x = 0 is bolted to the
wall. The payload sits on a 16 × 16 mm pad on top of the arm tip (x 64–80, y 12–28, z 60). The STL faces are
triangulated on a 4 mm lattice, so the pad and bolt pads are unions of whole triangles, and the voxel mesh represents
every axis-aligned face exactly at 4, 2 and 1 mm. Only B's chamfer becomes a staircase.

`study.json` states:

- the material: nominal 6061-T6 values, source *database*;
- the process: machined;
- the mounting: back face fixed;
- the load: 3 kg × standard gravity downwards on the pad;
- element sizes 4, 2 and 1 mm;
- sensitivities: E ± 10 %, ν 0.30 and 0.36, mounting on only two 12 × 12 mm bolt pads, and self-weight.

## Run it

```bash
navier-ctl study check share/navier/examples/bracket_comparison/study.json
navier-ctl study run share/navier/examples/bracket_comparison/study.json --dir ~/NAVIER-Projects/bracket_ab
navier-ctl study report ~/NAVIER-Projects/bracket_ab
```

On an M2 with 8 GB the run takes about a minute: 16 analyses, the largest with about 53,000 elements. Run from the
source tree, the path is `examples/bracket_comparison/study.json` and the binary `./navier-ctl`.

## What to expect

`expected_output/` holds `evidence.json`, `report.md` and the region previews produced by the packaged binaries on
the build machine. On the same build and platform, a run elsewhere reproduces every quantity bitwise; the install
test checks this.

The outcome (`ranking_consistent_on_tested_meshes`):

- **B has the lower predicted load-region displacement under the modeled conditions on every tested mesh,** 60.5 %,
  63.9 % and 65.5 % less than A at 4, 2 and 1 mm.
- **B is also about 27 % heavier.**
- **Sensitivities:**
  - the ranking holds for every declared one;
  - holding only the bolt pads raises the displacements by up to about 25 %;
  - Poisson's ratio and self-weight change them by about 1 %.
- **Discretisation, reported as three separate findings:**
  - **Ranking:** the same on all three meshes.
  - **Convergence criterion:** A meets it (+0.85 % between 2 and 1 mm). B does **not** (−3.53 %, relative to the 1 mm
    value), because the chamfer's staircase approximation changes with the mesh. The record keeps that failed check.
  - **Estimate:** A has a discretisation-error estimate of 2.4 % (grid convergence index, safety factor 3). B has
    none, because its meshes discretise different shapes. So the size of the difference is not estimated beyond the
    tested meshes.
- During the independent evaluation, a 0.5 mm level (about 430,000 elements for B, `limits.max_elements` 450,000)
  kept the ranking (66.2 %). B's change fell to −1.70 %, within the criterion.

What the example does not show:

- whether either bracket is strong enough;
- how real bolts, washers and the wall behave;
- anything about a printed version of the part.

## Regenerate the geometry

```bash
python3 make_geometry.py geometry
```

This also writes the evaluation-set geometries: A with a small tip chamfer, and a thin-walled bracket.
