# Contract: the fused-filament print analysis (`fff_print`) and its results

Owner: the mechanics/AM-process session (`src/mech/**`, `src/ctl/print_analysis.[ch]`). This file is written before the
implementation and changes only by addition or dated amendment. It states what the analysis promises, what it refuses,
what it writes to disk, and the criteria its acceptance was judged by.

Related: [`Mech Sim/STATUS.md`](../../Mech%20Sim/STATUS.md) (state and verification numbers),
[`src/mech/fffprint.h`](../../src/mech/fffprint.h) (the physics and its limits),
[`docs/analysis.md`](../analysis.md) (how analyses are driven in general).

## 1. What it is

`fff_print` simulates a fused-filament print of one meshed part layer by layer: deposition, cooling, the residual
stress that accumulates while the part is held on the bed, and the warp when it is released. It is a **process
simulation on demonstration material data, not a forecast of a particular printer**. Every result carries that scope
in machine-readable form (section 5).

It is started by the typed operation `mech_print_run` and runs as a job of kind `fff_print` in the shared job manager,
so `job_status`, `job_cancel` and `job_list` work on it like any other analysis. Results are the time-indexed thermal
result file (`results.nvt`), so `results_query`, `results_probe`, `results_render` and `results_export` read a print
with `time_index` / `time_s` exactly as they read a transient thermal run, and the application's stored-time slider
moves through the build.

## 2. Inputs

All inputs are quantities with units (`"0.2 mm"`, `"210 degC"`, `"15 mm3/s"`) parsed by the shared unit reader, or
plain SI numbers. The process block must carry a `provenance` (`user`, `inferred`, `calibrated`); the analysis refuses
to guess where a printer setting came from.

| Input | Unit | Meaning |
|---|---|---|
| `body` | – | the meshed body that is printed (one body, one material) |
| `process.layer_height` | length | the **simulation** layer, which lumps several printed layers |
| `process.nozzle_temperature`, `bed_temperature`, `ambient_temperature` | temperature | deposition, build plate, chamber air |
| `process.convection` | W/(m²·K) | film coefficient on every exposed face |
| `process.radiation` | bool | grey radiation with the material's emissivity |
| `process.deposition_rate` | volume/time | how fast material is laid down; sets the time a layer cools |
| `process.min_layer_time` | time | lower bound on a layer's cooling time |
| `process.cooldown_bed_on`, `cooldown_bed_off` | time | after the last layer, with the bed heated and switched off |
| `process.thermal_substeps` | – | thermal steps per layer (≥ 2), geometric from a short first step |
| `process.relaxation_offset` | temperature difference | above the material's glass transition, where stress relaxes |
| `process.relaxation_time_at_tg` | time | creep below the relaxation temperature (GOALS.md G20 step 4): each element's stress relaxes as one Maxwell element, its time this value at the glass transition shifted by Williams, Landel and Ferry's universal constants (17.44, 51.6 K), none colder than Tg - 51.6 K; 0 (the default) leaves creep out |
| `provenance` | – | `user`, `inferred` or `calibrated`; refused when absent |
| `store` | – | `key_times` (default) or `substeps` (section 4) |
| `probes[]` | length | points whose temperature history is recorded in the summary |

## 3. Refusals (before any solve)

| Condition | Error | What the message says |
|---|---|---|
| the process block has no `provenance`, or a required input is missing | `INVALID_PARAMS` | the typed schema names the missing property before the handler runs (amended 2026-09-17: the criteria first said `PRECONDITION`; the schema refuses earlier, which is better, so the code is the schema's) |
| the printed body has no material assignment, or the assignment's provenance is `default` | `PRECONDITION` | assign a material with `material_assign` and state its source |
| the part has no face on the build plate (no element with a whole face at the lowest node plane) | `PRECONDITION` | place the part on the plate, or add the supports the print needs |
| the mesh is stale with respect to geometry, placement or material (`project_mesh_current`) | `PRECONDITION` | run `mesh_generate` again |
| more than one material is assigned to the printed elements | `UNSUPPORTED` | the print model has one filament |
| the material record lacks a table the print needs (density, specific heat, conductivity, modulus, expansion) | `PRECONDITION` | the missing property is named |

Elements that cannot be reached from the bed through printed material are **not** a refusal: they are counted,
reported as `elements_never_deposited`, and left out of the print. Elements deposited after their own layer because
they only then rest on printed material are counted as `elements_deposited_late`.

## 4. What is stored, and when

The default `store: "key_times"` keeps one stored time for

- every simulation layer, twice: right after it is deposited and when it has cooled for its layer time,
- the end of the cool-down with the bed heated, and the end of the cool-down with the bed off,
- the state after release from the bed, which is always the **last** stored time.

`store: "substeps"` keeps every thermal substep of every layer and of both cool-down phases, each with its own
mechanical solve, plus the release: `thermal_substeps x (layers_deposited + 2) + 1` stored times (several times the cost
and the disk). Probe temperature histories are always kept at every thermal substep, in the summary, because they are a
few hundred numbers.

The two modes do not give exactly the same stresses; their difference measures mechanical increment lumping. The
original PLA wall record from `tools/printflow.py` was 7.791 MPa at `key_times` and 7.578 MPa at `substeps`. Amended
2026-10-03 after the deposition, stress-field and radiation corrections: the same workflow reports 8.506 MPa at
the nine key times and 9.028 MPa at 21 substep states (6.1 percent higher). Finer increments resolve temperature
paths, relaxation resets and stress redistribution differently; the change has no guaranteed sign. `substeps` is
the more resolved calculation, not a physical-accuracy certificate; `key_times` is cheaper.

## 5. The summary

`summary.json` in the run directory, and the `summary` block of the results file and of `job_status`, carry:

- `process` (the resolved inputs with units and their provenance), `mesh`, `deposition` (layers, late and never
  deposited elements), `timing` (machine time, compute seconds split into heat and stress),
- `results`: peak von Mises while on the bed and after release, the vertical warp range after release, the worst
  relative heat balance of a step, the largest bed reaction and the support reaction after release (which is zero for
  a self-equilibrated release), and `skin`, the exposed surface after the last layer that convection and radiation
  act on (section 10),
- `probes`: label, position, and the temperature history with its times,
- **`scope`**, machine-readable, with exactly these keys:

```json
"scope": {
  "statement": "process simulation on demonstration material data; not a forecast for a particular printer or filament",
  "is_forecast": false,
  "layer_lumping": {"simulation_layer_mm": 3.0, "printed_layers_per_simulation_layer": 15, "toolpath_within_a_layer": "not modelled"},
  "creep_below_relaxation_temperature": "not modelled",
  "constitutive_law": "temperature-dependent incremental stress accumulation (hypoelastic approximation); old stress is not rescaled when the modulus changes, and stress is reset above the relaxation temperature; no time-dependent viscoelastic law",
  "bed_stresses_are_upper_bound": false,
  "bed_stresses_note": "creep below the relaxation temperature is omitted; temperature-dependent stiffness and stress redistribution mean this approximation establishes no general local stress bound",
  "material": {"id": "pla_generic_demo", "status": "demonstration", "measured": false},
  "compared_with_measurement": false,
  "measured_accuracy": {"short": "model without creep: two published bars warped 400 to 800 times more", "statement": "...", "criteria_met": false,
                        "datasets": ["Syrlybayev et al. 2021 and Al Rashid and Koc 2022, their calibration bars"], "record": "GOALS.md G20 step 4"},
  "not_modelled": ["toolpath within a layer", "creep below the relaxation temperature", "plasticity", "raster anisotropy and interlayer strength", "crystallisation", "supports", "gravity", "adhesion failure"]
}
```

An agent that reads only `summary.scope` can tell that the numbers are a trend on demonstration data, not a prediction
to certify a part with.

**Amendment, 2026-10-08: the model's measured record (GOALS.md G20 step 6).** `compared_with_measurement` stays a
statement about this run. Beside it, `measured_accuracy` states how the model itself did against published prints,
measured blind in GOALS.md G20 step 4, and which datasets that was: with creep below the relaxation temperature
(`relaxation_time_at_tg` > 0), 21 cases from five studies at a median error of 38 %, the measured order of process
variants in 11 of 18 pairs, with `median_absolute_error`, `cases` and `process_pairs_in_measured_order`; without it,
the calibration failure of two published bars. `short` is the clause the app's print inspector shows; `criteria_met`
is false in both. `tools/printflow.py` checks the object.

**Amendment, 2026-10-08: the glass's own relaxation (GOALS.md G20 step 8).** `relaxation_activation_temperature`
(Ea / R, in K) replaces Williams, Landel and Ferry's universal shift below the glass transition by an Arrhenius law,
tau(T) = tau_g exp(T_act (1/T - 1/Tg)), with no cut-off; above Tg the WLF shift stays, and both give tau_g at Tg. It
exists because round one showed the universal constants make a 10 K change of bed temperature worth orders of magnitude
of relaxation below Tg, where glassy polymers shift far less (amorphous PLLA's measured apparent activation energy near
Tg is about 530 kJ/mol, 63,700 K: Macromolecules 58 (2025) 12966, CC BY 4.0). Default 0 keeps round one's law.
Verified in `mechtest --printing`: a held block keeps exp(-t / tau) at Tg - 20 K and at Tg - 60 K, below WLF's cut-off,
and WLF's tau at Tg + 5 K, all to round-off; the summary's `scope.creep_below_relaxation_temperature` names the law.

**Amendment, 2026-10-08: a bed that lets go (GOALS.md G20 step 8).** `bed_toughness` (J/m^2) gives the part's bond to
the bed a toughness. After every increment on the bed, a bonded bed node next to a peeled one is released when the
crack-closure estimate of the energy its release frees, G = -R . u_p / (2 A) (its accumulated bed reaction R, the
displacement u_p of the peeled neighbour behind it, the bed area A it carries), reaches the toughness; what it carried is
applied reversed and the part re-equilibrates, repeatedly within the increment, so an unstable peel runs on. The peel
starts from a flaw of one cell along the edge of the contact; a part left without three bonded nodes off one line pops
off the bed and is released at once. Peeled nodes carry no contact (the result says how deep one went). The result's
`results.bed_peel` gives the toughness, the bed nodes, the peeled ones and their fraction, whether the part came off
during the print, and that depth; `scope.not_modelled` drops "adhesion failure during the print". Default 0 keeps the bed
that never lets go. Verified in `mechtest --printing` against a bimetal strip's closed form, 7 E eps^2 t / 32 (+0.08 %),
with a tougher bond holding and a weaker one peeling to a short ligament whose free arms take the bimetal's curvature.

The implemented stress law accumulates increments evaluated along the temperature path. A changed modulus does not
rescale previously accumulated stress; above the relaxation temperature the stress is reset instantly. Exact
integration of `E(T) alpha(T) dT` verifies this chosen incremental law. It does not supply a time-dependent polymer
relaxation law or establish the general thermoelastic relation `sigma = D(T) (epsilon - epsilon_thermal)`.
Omitting creep also does not establish a general upper bound on local bed stresses: a spatially varying temperature
field changes stiffness and redistributes stress. The legacy scope key is retained with `false`; no stress bound is claimed.

## 6. File layout: `results.nvt` version 3

The print writes the existing time-indexed result file (`src/ctl/thermal_io.c`): the 8-byte magic `NVTHR001`, a
little-endian `uint64` header length, a JSON header, then the little-endian arrays listed in the header's `arrays`
table, in order. Version 3 adds **one** array and one header flag:

| Header key | Type | Meaning |
|---|---|---|
| `version` | int | `3` (files of version 1 and 2 still load) |
| `has_elem_birth` | bool | the `elem_birth` array is present |

| Array | Type | Count | Meaning |
|---|---|---|---|
| `elem_birth` | int32 | `nelems` | for every element, the index of the first stored time at which it exists; `-1` for an element that is never deposited. Written only when the field is present; a reader that does not know it sees a file it can still read, because the array table is self-describing |

`ThermalCase` carries the field as

```c
int *elem_birth; /* nelems: index of the first stored time at which the element exists; NULL = all from the start */
```

`NULL` means every element exists at every stored time, which is what a transient thermal or thermomechanical run
writes. Readers that draw a print must hide element `e` at stored time `i` when `elem_birth[e] < 0 || i < elem_birth[e]`.

Everything else in the file keeps its meaning: `times` (s), `T` (K per node per stored time), `mech_u` (m),
`mech_vm` (Pa), `tmin`/`tmax`, `mech_peak`/`mech_umax`, the mesh, the sets of the mapped selections. For a print,
`fixed`/`fixed_T` are the bed nodes and the bed temperature, and `elem_source` is zero.

## 7. Speed, measured

The cost is the structural increments: one solve per stored time, each over the whole printed part. Measured on the
truss bridge (516 x 96 x 105 mm, 681 cm3) on a fanless MacBook Air M2 with 8 GB, `build/amprint`, runs taken
back to back:

| Configuration | 4 mm elements, 9 928 elements, 55 increments | peak stress on the bed | warp (lowest point) |
|---|---|---|---|
| automatic solver, no thread pool (the state before this work) | 220.4 s (stress 183.5 s) | reference | reference |
| thread pool, direct | 195.1 s (stress 166.3 s) | 12.818340564 MPa | -0.491847557 mm |
| thread pool, iterative, tolerance 1e-10 | 143.6 s (stress 109.6 s) | 1.2e-10 relative | 1.2e-08 relative |
| thread pool, iterative, tolerance 1e-8 | 97.8 s (stress 71.6 s) | 2.6e-08 relative | 3.7e-06 relative |
| thread pool, iterative, tolerance 1e-6 | 78.7 s (stress 52.8 s) | 6.5e-06 relative | 7.5e-04 relative |

So the increments now assemble, solve and recover on the performance cores like every other structural solve (exact,
1.10x), and an agent that states a tolerance can trade accuracy for time deliberately (2.25x at 1e-8, with the peak
stress within 2.6e-8 and the warp within 3.7e-6 of the direct solve).

At 3 mm elements (25 234 elements, 110 553 equations) the same bridge did **not** reach the 180 s target:

- the direct factor is 633 MB per solve and needs 63.3 million entries at the release step, above the solver's own
  60-million guard, so the automatic solver falls back to the iterative one there;
- the fastest configuration measured (iterative, 1e-8, skipping increments below 1 K) printed the whole bridge in
  482 s in one run and 729 s in a repeat of the same command later in the session: on this passively cooled machine,
  with about 60 MB of free memory at the time, the spread between identical runs (1.5x) is larger than the remaining
  algorithmic headroom found inside the files this session owns;
- the levers that remain are in `src/fem`: a symbolic factorisation reused between increments with the same active set
  (the ordering and symbolic phase is part of the ~1 s per solve that is not the numeric factorisation, so at most
  about 10 %), and a preconditioner stronger than block Jacobi for the 3 000:1 stiffness contrast between molten and
  cold material, which is what makes the iterative solver lose its advantage as the model grows.

Skipping increments whose largest element temperature change is below a threshold is available and measured: at 1 K it
skipped 3 of 55 increments at 4 mm and 1 of 72 at 3 mm, so it is off by default. It is not an approximation of the
stress history: the skipped temperature change is integrated by the next increment that runs (the restrained-bar check
in `test_fff_print` shows one increment and five give the same stress), only the stored frame in between repeats the
previous mechanical state.

## 7. Acceptance

Amendments are dated and stated in the criterion itself; nothing is relaxed silently.

These criteria were written before the implementation ran. Each one is either met, with its measured value, or not.

| # | Criterion |
|---|---|
| A1 | `mech_print_run` starts a job of kind `fff_print`: `job_status` reports progress and a summary, `job_cancel` leaves it `CANCELLED`, an identical second call returns the first job with `deduplicated: true`, and the run directory holds a `spec.json` whose `spec_hash` covers every field except `job_id`, `created`, `project_revision` and `spec_hash` |
| A2 | Each refusal of section 3 is raised before any solve, with its error code and a hint naming the fix |
| A3 | The print's `results.nvt` loads, every stored time carries `T`, `mech_u` and `mech_vm`, the last stored time is the released state, and `results_query` / `results_probe` / `results_export` accept the print job id with `time_index` and `time_s` |
| A4 | `elem_birth` round-trips exactly; a version-2 file (no such field) still loads; a version-3 file without the field still loads |
| A5 | The summary carries `scope` with the keys of section 5, and the material status inside it is the library's own status string |
| A6 | (measured: `key_times` 9 stored times for 3 deposited layers, `substeps` 21 = 4 x (3 + 2) + 1; peaks 7.791 and 7.578 MPa) `key_times` stores 2 per **deposited** layer + 2 cool-down + 1 release and no more; `substeps` stores every thermal substep; the summary reports the count (amended 2026-09-17, first run: a simulation layer that holds no element centroid deposits nothing and is skipped, so the count follows `layers_deposited`, which the summary now reports next to `simulation_layers`; such a layer height also raises a warning) |
| A7 | `tools/printflow.py`, an independent MCP client, prints a small PLA wall end to end, provokes the refusals of section 3, reopens the project in a second engine session and reads the same stored times and the same peak von Mises (1e-9 relative), and checks from the results that the worst heat balance is ≤ 1e-6 and that the support reaction after release is ≤ 1e-6 of the largest bed reaction |
| A8 | `make test` runs `tools/printflow.py` and every suite passes |
| A9 | Speed: the truss-bridge print at 3 mm elements finishes in ≤ 180 s with its final warp and peak stress equal to the 519 s reference within 1e-6 relative for exact methods, or within a stated measured tolerance for iterative ones |

## 8. Numerical amendments, 2026-10-03

The following criteria are fixed before the new checks run. Synthetic material tables below are verification data,
not a measured filament or a printer forecast.

- F10: on a fully restrained hex, a piecewise linear modulus transition only 0.01 K wide and a piecewise linear
  expansion table give the closed-form stress integral to 1e-10 relative in one increment and in multiple increments.
  The integration splits at all material-table breakpoints, the relaxation temperature and every modulus-floor crossing;
  Simpson integration is exact on each resulting quadratic product. The previous fixed 0.5 K trapezoid is replaced.
- F11: on a bent two-layer specimen the reported element von Mises equals the mean of the eight Gauss-point von Mises
  values within 1e-12 relative. It is no longer computed from the mean stress tensor, whose opposite bending stresses
  can cancel before the invariant is evaluated. Uniform stress retains its previous value. This is an element mean,
  not a Gauss-point maximum and not a spatial convergence certificate.
- F12: for three deposited hex layers with no convection or radiation, the independently integrated final enthalpy
  plus reported heat into the bed equals the supplied nozzle enthalpy within 1e-7 relative, both for constant heat
  capacity and for a linear temperature-dependent heat capacity. The layer deposition correction must be nonzero;
  the reported deposition identity must close within 1e-12 relative. This checks deposition energy separately from
  the thermal solver's subsequent step balances. With bed and ambient at 300 K and deposition at 400 K, stored-frame
  nodal temperatures must stay in [300 - 1e-6, 400 + 1e-6] K.
- F13: repeat the constant-capacity three-layer print with a homogenised support bottom layer at capacity/stiffness
  fraction 0.25, then remove that support. The independent retained-part enthalpy plus removed-support enthalpy and
  bed heat equals supplied nozzle enthalpy within 1e-7 relative. The support-removal ledger equals the independent
  support enthalpy within 1e-10 of supplied energy.
- F14: the existing eight-layer demonstration PLA wall, with convection, radiation and both bed cool-down stages,
  closes its whole-print physical heat ledger within 1e-6 relative. This is a conservation check, not measured-print
  validation of the demonstration material.
- F15: after support removal, the stored frame's maximum temperature/displacement and final warp extrema equal the
  extrema independently computed over nodes of the remaining active elements, within 1e-10 K and 1e-12 m.

Conforming nodes at a new layer's interface already carry the previous layer's temperature. Setting only brand-new
nodes to the nozzle temperature loses some of the incoming material's energy before any cooling solve. The amended
layer model retains those interface temperatures and supplies the missing enthalpy during the first thermal substep
as a nodal heat pulse. At each new element's Gauss point the secant capacity between its interpolated initial
temperature and the nozzle temperature multiplies `N_a (T_nozzle - T_a) detJ`. Summing these nodal contributions is
exactly the missing Gauss-integrated enthalpy. A contribution on a prescribed bed node goes directly into the bed
heat ledger; free-node contributions enter the thermal solve. The pulse duration is the first substep, so substep
refinement is still required for local temperature-history convergence. This correction does not resolve individual
extruded roads, contact resistance, or a toolpath.

The result summary additionally reports supplied nozzle enthalpy above ambient, deposition correction, stored
enthalpy above ambient, heat into the air, enthalpy removed with supports, and the resulting whole-print heat balance.

## 9. Equilibrium diagnostic amendment, 2026-10-03

Before the first new diagnostic run, F16 requires the FDM increment's reported equilibrium residual to match an
independent direct structural solve of the same cooled bed-bonded hex to 1e-12 absolute, and to remain finite and
below 1e-9. Skipping an unchanged thermal increment must preserve that exact value and the solve count. Release
records its own successful solve residual; later solves or skips preserve that release value. F17 requires the
end-to-end MCP summary to carry finite last-solve and bed-release residuals below 1e-6 with their definition.
The first F16 harness incorrectly selected z=0 as the bed of a box centred on z=0, leaving no constrained nodes;
the solver correctly refused its rigid-body singularity. The harness now selects the bottom face by connectivity.
No acceptance tolerance or numerical solver changed for this fixture correction.

Amendment to A7: its original support-reaction/bed-reaction ratio is not a meaningful normalization for these
self-equilibrated thermal loads. `largest_bed_reaction_n` is the largest component of the net bed resultant, not
the largest individual bed force. Opposite bed forces can cancel even while the part carries substantial stress.
The first refinement study failed this ratio; its recorded failure is retained rather than labelled a physical
regression. The new criterion uses the recovered free-equation residual norm divided by the sum of the applied
nodal load norm, the individual prescribed-DOF reaction norm and the assembled free RHS norm including thermal
eigenstrain. This is the structural solver's existing equilibrium diagnostic; no force or state computation is
changed. With exactly zero force scale the solver reports its absolute residual, zero for an unloaded zero state.
Reaction forces remain reported in newtons and are not divided by the near-zero net bed resultant.

Observed after the fixture correction: F16's on-bed residual was 1.34e-16, bed-release 2.49e-16, and the later free
solve 1.69e-16; the five diagnostic checks passed with the full printing suite, 40/40. The MCP wall's last and
release residuals were both 6.29e-15, with 88/88 flow checks. These are last-increment equilibrium diagnostics,
not a measurement-validation claim or an independent convergence proof for the accumulated stress history.

## 10. The skin of inclined and curved walls, 2026-10-08 (GOALS.md G22 step 4)

Convection and grey radiation act on every exposed voxel face of the part that is not on the bed. Voxel steps over a
surface of unit normal n have |nx| + |ny| + |nz| times its area (1.41 at 45 degrees, up to 1.73), so each face now
exchanges the heat of the surface it stands for: its film coefficient and emissivity are multiplied by 1 over that
factor of the STL triangle the face is mapped to, the weight the static and thermal analyses use since G22 step 2
(`StaticModel.face_weight`, carried into the print as `FffMesh.face_weight`). A face normal to a grid axis, and a face
with no triangle (the top of the layer being printed, which a later layer covers), keeps the coefficient exactly, so
parts whose walls are all normal to the axes print bitwise as before. The supports' homogenised faces are unchanged:
their heat exchange is over their pattern's own surface per volume.

`results.skin` reports the part's exposed faces after the last layer, the bed's and the supports' excluded:
`area_mm2`, the surface convection and radiation act on (each face's area times its weight), and
`voxel_face_area_mm2`, the faces' own area. On a curved or inclined part the second is the larger.

Acceptance, written before the first run (GOALS.md G22 step 4, `tools/printskin.py`, in `make test-printing` and
`make test`): G16's 8 x 2 x 4 mm wall turned by 0, 30 and 45 degrees and meshed with 0.25 mm cubes, held at the bed's
temperature, loses to the air within 2 % of h A dT and of eps sigma A (T_bed^4 - T_air^4) over its 96 mm^2; at 0
degrees bitwise as before. Measured: 0 degrees -0.015 % and -0.049 % (the 0.015 K the wall sits below the bed), 30
degrees -0.99 % and -1.03 %, 45 degrees -1.37 % and -1.41 %; before the change the turned walls lost 29.1 and 32.8 %
too much. Records in `validation/g22-voxel-loads/print-before/` and `print-after/`.
