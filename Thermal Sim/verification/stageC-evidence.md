# Stage C evidence — thermal contact through MCP

Criteria were fixed before the first run (`stageC-criteria.md`, including one recorded amendment). Raw output:
`contactflow.log` (`tools/contactflow.py`, 104 checks, MCP only).

**Path exercised end to end:** STL import of two bodies → explicit placement into one assembly → materials → mesh →
`interface_list` (undeclared contact reported as perfect bonding) → `interface_define` (topology checked, coverage and
resistance reported) → conditions → adaptive transient job → `results_interface` + per-body balances → save → new
server process → reopen → re-mesh → identical result.

| # | Governing model | Reference problem | Mesh / time settings | Error metric | Observed | Criterion | Known limitations |
|---|---|---|---|---|---|---|---|
| C1 | conduction + lumped interface conductance `q = h (T_b − T_a)` | two 20 × 10 × 10 mm blocks, k = 50 / 10 W/(m·K), 5000 W/m² in, 100 W/(m²·K) out, h = 2000 W/(m²·K): series resistance | 2.5 mm voxels (16 interface faces, 25 node pairs), adaptive BE to 30 000 s | temperature and heat-rate deviation from the series solution | side A 82.499941 °C (82.5), side B 79.999942 °C (80.0), jump 2.499999 K (2.5), far faces 84.499941 / 69.999950 °C, heat rate −0.49999974 W (−0.5) — within 6e-5 K and 2.6e-7 W | 1e-3 K; 1e-5 W | 1-D steady problem: linear elements are exact, so this verifies the interface integration and accounting, not spatial convergence of a curved interface |
| C2 | per-body energy balance | same | same | each body's `stored − sources − boundary − prescribed − interface` | both bodies close below 1e-9 relative and report `independently_closed` | 1e-9 | perfectly bonded bodies cannot close independently (their exchange is not a measured quantity) and say so |
| C3 | reproducibility | same, saved, reopened in a new `navier-server` process, re-meshed, re-solved | same | relative difference of side temperature and heat rate | ≤ 1e-12 | 1e-12 | same build and machine |
| C4 | model variants | thin layer 0.5 mm × 1 W/(m·K) (= 2000 W/(m²·K)); perfect; insulated | same (insulated run to 600 s) | equality with the conductance run; jump; heat | thin layer equal to < 1e-9 K; perfect: no jump, far face 82.00 °C; insulated: exactly 0 W and 0 J, B within 1e-9 K of 20 °C with < 1e-9 J stored (amended criterion) | as stated | a thin layer has no heat capacity |
| C5 | topology checks | the same body twice; a second interface on the pair; a 30 mm gap; a 7 mm overlap | 2.5 mm | refusal and diagnostic | each refused; gap named with its size; overlap named as an overlap | refused with diagnosis | only coincident, face-matching interfaces of two bodies are supported |
| C6 | exterior conditions on internal faces | convection on every face of A | same | interface area of the selection; mesh area | 100 mm² reported on the interface; mesh area excludes it (no double counting) | 100 mm² ± 1 % | the probe steps half an element outward from each STL triangle |
| C7 | thermal split, mechanical bond | thermomechanical run fixed only at A's far face | same | the structural solve | succeeds: B is carried through the bond | succeeds | elastic, bonded: no mechanical contact or separation |
| C8 | preview | `interface_preview` | same | image and coverage | PNG; 16 faces, 100 mm² | as stated | — |

## Design notes that the evidence depends on

* **Only the thermal model is split.** Nodes used by both bodies get a thermal twin for body B's elements; the
  structural model keeps the shared mesh nodes and maps displacements back to every thermal node (C7).
* **Area-lumped pairs.** Each interface face gives a quarter of its area to each of its four node pairs, so the total
  conductance is exactly `h · A` regardless of node count (C1 matches the series resistance to 2.6e-7 W).
* **Two independent accounts of the interface heat.** The pairs report the heat crossing; each body's own stored,
  source and boundary energies are summed separately from its elements and faces. They agree to 1e-9 (C2).
* **Overlaps are detected, not guessed.** The mesher now counts cells claimed by two bodies (ownership unchanged), so an
  overlap is refused instead of producing an interface at an arbitrary cell boundary.
