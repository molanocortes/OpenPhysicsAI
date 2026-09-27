# Validation result format

One JSON file per solver run, written by whoever ran it, read by the challenge scorer
[`tools/leaderboard.py`](../tools/leaderboard.py). It exists so that a number can be compared without asking the person
who produced it what it means. Put files in the results directory the challenge names, named
`<solver>_<strategy>_<orientation>_<element>mm.json`, or `<solver>_<strategy>_<orientation>_<element>mm_l<layer>mm.json`
when two runs share an element size and differ in the simulation layer. The name is for people; the scorer reads the
fields, never the name. The example below is illustrative: the LPBF cantilever results this format was written for were
removed from the repository with their measurements on 2026-09-27.

Contract: **`format_version` 1**. The scorer refuses a file it cannot read rather than guessing.

```json
{
  "format": "openphysicsai-validation-result",
  "format_version": 1,
  "case": { "strategy": "P17", "orientation": "X", "condition": "AS" },
  "solver": { "name": "navier-lpbf", "version": "0.4.0-dev", "git_commit": "1f7162b",
              "element_type": "voxel-hex8", "notes": "" },
  "geometry": { "file": "<the challenge's geometry parameter file>",
                "sha256": "<of that file, exactly as read>",
                "overrides": {} },
  "mesh": { "element_size_mm": 1.0, "elements": 5544, "nodes": 9412, "equations": 28236 },
  "material": { "youngs_modulus_mpa": 70000, "poissons_ratio": 0.33 },
  "inherent_strain": { "exx": -0.0012226, "eyy": -0.0034059, "ezz": -0.03,
                       "source": "the elastic strain of the P17 preset in src/ctl/print_profiles.json" },
  "layer_scheme": { "mode": "layer_by_layer", "layer_thickness_mm": 1.0, "layers": 9,
                    "activation": "strain_free" },
  "cut": { "height_mm": 2.5, "kerf_mm": 1.0, "elements_removed": 372 },
  "result": { "tip_uz_before_cut_mm": -0.1789, "tip_uz_after_cut_mm": -0.2106,
              "springback_mm": -0.0317,
              "tip_definition": "max u_z over the nodes at x = total_length_x_mm" },
  "run": { "wall_s": 35.2, "peak_rss_mb": 210, "started": "2026-09-17T23:55:00Z", "host": "m2-air" },
  "provisional": true,
  "provisional_reason": "geometry unconfirmed: total_width_y_mm is an unverified recollection"
}
```

Rules, for every scorer that reads the format:

- **`geometry.sha256` is mandatory.** A result whose hash does not match the geometry file in the working tree is
  reported as computed from a different geometry, and is not compared.
- **`layer_scheme.mode`** is `whole_part` or `layer_by_layer`. They answer different questions and are never averaged
  or plotted together.
- **`layer_scheme.layer_thickness_mm` is required for `layer_by_layer`, and it is part of the discretisation**, not a
  description of it. An inherent-strain build has **two** independent sizes: the layer thickness sets how many strained
  layers make up the curvature, and the element size resolves each layer. They move the answer in opposite directions
  and by different amounts, so a mesh-convergence criterion is applied to each of them **alone**: a scorer compares two
  results only when exactly one of the two sizes differs, and prints a pair in which both differ without a verdict. A run that
  reports only `mesh.element_size_mm` cannot be used for such a criterion.
- **`case.orientation`** is `X` or `Y`, the axis of the machine the part's long axis lay along. A model that lays the
  part along its own x and applies the tensor unrotated **is the X case** and says `X`; `n/a` is read as `X` for
  compatibility with the first CalculiX files.
- **`case.strategy`** may be spelled `P17`, `Calibrated-P17`, `Ot`, `Original (OT)` or `OriginalOT`; `leaderboard.py`
  reduces all of them to the spelling the measurement file uses.
- **`inherent_strain.source`** names where the tensor came from. A tensor that this project calibrated itself says so,
  with the case it was fitted on, because a number fitted on a case may not be reported as a prediction of that case.
- **All three of `tip_uz_before_cut_mm`, `tip_uz_after_cut_mm`, `springback_mm`** are required. The sources disagree on
  which one they mean by "deflection", so all three travel together. Sign convention: +z is up, away from the plate.
- **`provisional`** stays `true` while any parameter in the geometry file has a status other than `confirmed`. Every
  table built from such a result carries the label.
- Unknown fields are allowed and ignored. Missing required fields are an error, not a default.
