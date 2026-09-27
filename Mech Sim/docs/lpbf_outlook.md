# From plastic to metal: what the print machinery gives LPBF, and what is missing

Written 2026-09-18 by the AM-process session, after `fff_print` (see [`../../docs/contracts/print-results.md`](../../docs/contracts/print-results.md)).
No code was written for this page; everything below is a plan and a reading of the existing sources.

**Amended 2026-09-27.** This page was written when the repository held LPBF cantilever measurements, the strains a
commercial simulator calibrated against them and the published cantilever geometry (Gersch et al. 2025, CC BY 4.0).
The measurements and the simulator's results were removed before publication by the owner's decision; the geometry
remains as `samples/cantilever.stl`. Case 7 below is kept as the plan it was.

## 1. What carries over unchanged

| Piece | Where | Why it carries over |
|---|---|---|
| Element activation with a deposition plan | `src/mech/fffprint.c` (`fff_plan`) | Laser powder-bed fusion (LPBF) builds by layers too. The support rule changes (the powder bed carries overhangs, so every element of a layer is activated), but the bookkeeping, the counts of late and never-built elements and `elem_birth` in the result file are the same. |
| Heat with activation, convection, radiation and a prescribed plate | `src/fem/thermal.h`, used by the print | An LPBF build plate is a prescribed temperature (often 80-200 degC) exactly like the FFF bed, and the exposed faces radiate into the chamber. Phase change is already in the thermal material (`solidus`, `liquidus`, `latent_heat`). |
| Incremental thermo-elasticity with E(T), the alpha-weighted path modulus and stress relaxation above a temperature | `src/mech/fffprint.c` (`thermal_increment`, the relaxation pass) | The same structure describes a metal: the modulus falls with temperature, and above the annealing temperature the accumulated stress is released. `matlib` already carries `anneal_k` next to `glass_transition_k`. |
| Initial strain per element (`eps0`) in the solid solver | `src/fem/solid.c`, `hex8_initial_strain_load` | This is exactly the interface an **inherent-strain** LPBF model needs: a calibrated strain applied to each layer as it is activated. The mechanism exists; only the calibration and the scan-frame orientation of the strain are missing. |
| Release from the plate: accumulated support reactions reversed on an isostatic support | `fff_mech_release` | Cutting the part off the build plate is the distortion event of LPBF, and it is the same operation. |
| The result file and the operation plumbing | `results.nvt` with `elem_birth`, `mech_print_run`, the job kind, the `scope` block | An `lpbf_print` job would write the same file and be read by the same result operations and the same viewer in the application. |

## 2. What is missing, in the order it blocks work

1. **Plasticity in the solid solver.** `solid_solve` is linear elastic: one assembly, one solve. LPBF residual stresses
   reach the yield strength, so an elastic model over-predicts them and cannot produce the residual field that survives
   cutting. What is needed: J2 plasticity with temperature-dependent yield and isotropic (and ideally kinematic)
   hardening, a return mapping with a consistent tangent, a Newton loop per increment, per-Gauss-point state (plastic
   strain, hardening variable, back stress) carried between increments, and an **annealing reset** of that state above
   the annealing temperature. `matlib` already has `MATP_YIELD` and `MATP_HARDENING`; the solver has neither state nor
   iteration. This is the one blocking item: everything else is arrangement.
2. **Element death (support removal).** Cutting supports, wire-EDM removal and machining remove material that carries
   stress. The increment machinery can express it (deactivate elements, release their internal forces the way the
   relaxation pass already does), but nothing exposes it, and contact between the freed part and the plate is not
   modelled.
3. **Two material states per element (powder and solid).** LPBF conducts heat through the powder around the part
   (roughly 0.2-1 W/(m K) against 15-25 for solid steel at temperature). Today an element is either in the model or
   absent. A powder state needs its own property set and a conversion at activation.
4. **Scale.** A useful LPBF part at 0.5-1 mm lumped layers is 10^5-10^6 elements. The direct factorisation already needs
   633 MB per solve at 110 553 equations on the truss bridge, and the iterative solver loses its advantage at that size
   (see the speed section of the print contract). LPBF needs a reusable symbolic factorisation, a stronger
   preconditioner than block Jacobi, and probably the inherent-strain shortcut (one mechanical solve per layer group,
   no thermal history) as the default fidelity.
5. **Calibration inputs.** Inherent strain is a calibrated quantity, not a material property: it comes from a small
   thermo-mechanical model or from a measured cantilever. The project has no place to store a calibrated process
   record with its provenance, and the honesty rules forbid inventing one.

## 3. The smallest verification cases I would register, in this order

Each is a closed form or a published measurement, and each is small enough to run in seconds.

| # | Case | Reference value | What it catches |
|---|---|---|---|
| 1 | **Built-up block with no temperature change**: activate layer after layer at a constant temperature | zero displacement and zero stress to solver tolerance | activation bookkeeping, birth at the programmed position, force redistribution |
| 2 | **Restrained bar, elastic-plastic, cooled by dT** | `sigma = min(E alpha dT, sigma_y)` and, after release, a residual of `sigma_y - E alpha dT` in the elastic case | the return mapping, the yield surface and the temperature-dependent yield |
| 3 | **Two-bar (Bree) thermal ratcheting** | the classic elastic / shakedown / ratcheting regime boundaries in the Bree diagram | cyclic plasticity with hardening, which is what layer reheating does to metal |
| 4 | **Bimetal strip** (already registered for FFF) | Timoshenko `kappa = 1.5 alpha dT / h`; the current model matches to 0.004 % | the layer-pair mechanism that produces warp |
| 5 | **Annealing reset**: heat a plastically strained bar above the annealing temperature and cool it | stress-free at the annealing temperature, and the stress on cooling equal to case 2 from that temperature | the annealing rule, the metal counterpart of the relaxation pass already verified for PLA |
| 6 | **Inherent-strain cantilever, cut from the plate** | the deflection follows from the calibrated strain in closed form for a uniform strip (`kappa = 6 eps_i t_layer / t^2` for a thin layer on a thick strip) | the inherent-strain path end to end |
| 7 | **A measured calibration cantilever** (the 70 x 10 x 9 mm AlSi10Mg comb cantilever of `samples/cantilever.stl`, cut from the plate by wire erosion, tip deflection measured on a coordinate measuring machine) | measured deflections of printed specimens; the set this case was planned on is not public | the first validation against a measurement: the inherent-strain path, the cut-from-the-plate step and the orientation dependence at once. The cut height and kerf are not published, so they must be fixed before anything is claimed |
| 8 | **NIST AM-Bench LPBF bridge distortion** (the AMB2018/AMB2022 bridge specimens, distortion and diffraction residual strain published by NIST) | the published measured values | a second, fully public validation case, independent of the owner's data. Its case numbers and uncertainties must be read from the AM-Bench publication first; nothing from it is in this repository |

Cases 1-6 can be written against closed forms today (1, 4 and the relaxation half of 5 already exist for FFF). Case 7 is
what the repository is now equipped for, and it is also what decides the feature order: the dataset is an
**inherent-strain** calibration, so the inherent-strain mode (case 6, `eps0` per activated layer, which the solid solver
already accepts) is worth more than a full thermal history for metal, and plasticity is needed for the residual stress
that the same report measures, not for the deflection. Until a case of this kind passes, an `lpbf` analysis carries the
same honest `scope` statement the FFF print carries now.

## 4. What I would not do

Do not start with a melt-pool model. A moving-source, keyhole-resolved melt pool is a different discipline and a
different mesh; the distortion and residual stress that a part owner asks about are captured by layer lumping plus
inherent strain or a lumped thermal history, which is what the industrial tools do. The melt pool belongs to a later
stage, feeding the inherent strain that the layer model consumes.
