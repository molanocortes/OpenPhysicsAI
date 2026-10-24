# Physical comparison protocol: bracket A against bracket B

**Status: validation pending. No specimen has been made and no measurement exists.** This protocol, its template and
its scripts were written before any test. `predict.py` produced the predictions in `predictions.json`; `analyze.py`
refuses to report anything when the measurement file holds no readings. Measurements must never be invented or
completed by estimation.

## 1. Purpose and claim tested

- **Primary claim:** the ratio of the displacement at the load pad of bracket B to that of bracket A under the same
  static load. The ratio does not depend on Young's modulus, so it avoids the least certain input of a printed
  specimen.
- **Secondary claim:** each bracket's absolute compliance, using a modulus measured on coupons printed with the
  specimens.

The protocol does not test strength, fatigue, creep or failure, and it gives NAVIER no safety or strength claim.

## 2. Specimens

| Item | Specification |
|---|---|
| Geometry | `examples/bracket_comparison/geometry/bracket_a_plain.stl` and `bracket_b_chamfer.stl`, 1:1, in mm (SHA-256 in the example's `expected_output/evidence.json`) |
| Number | 3 of A, 3 of B; 5 flexural coupons 80 × 10 × 4 mm |
| Material | one PLA (or PETG) spool for all parts; record the brand, colour, lot and datasheet tensile strength and modulus |
| Printer | one printer for all parts; record the model, nozzle diameter and firmware |
| Orientation | profile (XZ plane) flat on the build plate, width (y) vertical; coupons flat, with their 4 mm thickness along the build direction, so their layers lie like the brackets' layers |
| Settings | 0.2 mm layers, 100 % rectilinear infill at ±45°, 4 perimeters, extrusion multiplier calibrated beforehand; nozzle and bed temperatures from the filament datasheet; the same slicer profile for every part (store the profile file) |
| Order | print A and B alternately (A1, B1, A2, B2, A3, B3), coupons in the same job as A2 and B2 |
| Conditioning | at least 48 h at 21 ± 2 °C before testing; record the humidity |
| Acceptance | mass and six thicknesses recorded (template rows `specimen`). A specimen whose mean plate thickness deviates from 8.00 mm by more than 0.15 mm is kept, but flagged by `analyze.py`: displacement scales roughly with t⁻³, so 0.15 mm is about 5.6 % |

## 3. Mounting

| Item | Specification |
|---|---|
| Base | steel angle or block, at least 10 mm thick, bolted to a rigid table |
| Clamp | the wall plate is clamped between the steel base (against the back face x = 0) and a 10 mm steel clamp plate on the front face x = 8 mm, over z 0–24 mm, across the full width. This region is identical for A and B, because B's chamfer starts at z = 28 mm |
| Bolts | four M6 bolts through the base and the clamp plate, outside the specimen width, torqued to 4 N·m in a cross pattern |
| Predicted model | the same regions held fixed: the back face and the clamped front strip (`predict.py` writes this definition). An ideal rigid clamp is stiffer than a real one, so measured compliance is expected to be somewhat higher; the base indicator measures only the base's own movement |

## 4. Load

| Item | Specification |
|---|---|
| Load pad | a 16 × 16 × 5 mm steel pad centred on x 64–80, y 12–28 mm of the arm's top face, located by a printed template; its mass (about 10 g) is part of the preload |
| Steps | preload 0.2 kg (hanger), then 1.0, 2.0 and 3.0 kg total, calibrated masses (±0.1 g) |
| Direction | vertical, by gravity; a plumb check on the hanger |
| Non-destructive range | see section 6. Do not exceed 3.0 kg |

## 5. Measurement

| Item | Specification |
|---|---|
| Pad displacement | a digital indicator, 0.001 mm resolution, calibration certificate recorded; probe vertical under the arm's underside at x = 72, y = 20 mm (below the pad centre) |
| Base movement | a second indicator on the clamp plate's top edge above the specimen centre line |
| Net displacement | (pad reading − pad zero) − (base reading − base zero) |
| Cycle | zero both indicators at the preload; apply each step and read 10 s after the load settles; unload to the preload and read after 60 s. Three cycles per specimen, removing and re-mounting between cycles |
| Test order | A1, B1, B2, A2, A3, B3 |
| Temperature | 21 ± 2 °C at the specimen, recorded for each cycle; readings outside that range are not used |
| Record | every reading in `measurement_template.csv` (a copy per test day), with the operator code; nothing is averaged by hand |

## 6. Non-destructive range and linearity

Before testing, `predict.py` gives for 3 kg:
- the predicted displacement;
- the nodal 99th-percentile von Mises stress, as a planning indicator only. Stresses at the sharp inside corner of A
  are singular, so the peak is not used.

Testing proceeds when that indicator is below 10 % of the datasheet tensile strength (about 50 MPa for PLA gives a
limit of 5 MPa). During testing:
- **linearity:** the net displacement at 1, 2 and 3 kg must lie within 3 % of a straight line through the preload
  point;
- **return to zero:** after unloading, the reading must return within 2 % of the 3 kg displacement.

A cycle that fails either condition is recorded, marked in `notes`, and excluded from the compliance fit. If two
cycles of a specimen fail, that specimen is excluded and the reason is reported.

## 7. Analysis (fixed before testing)

`analyze.py measurements.csv predictions.json` does the following.

**1. Compliance per cycle.** Least-squares slope of net displacement against load (N) through the steps, in mm/N.

**2. Per specimen and per design.** The mean compliance of the valid cycles.

**3. Standard uncertainty per design.** Combined in quadrature:
- **Type A:** the standard deviation of the specimen means divided by √n;
- **indicator resolution:** 0.001/√12 mm per reading, four readings per net value;
- **indicator calibration:** 0.3 % of the reading, or the certificate value;
- **load position:** ±0.5 mm on an 80 mm arm, 0.4 %;
- **temperature:** 0.35 %;
- **plate thickness:** 3 × (standard deviation of the thicknesses) / 8 mm.

**4. Ratio.** r = C_B / C_A, with relative uncertainties combined in quadrature. Shared terms (calibration, temperature)
are counted as independent, which is conservative.

**5. Prediction uncertainty used for the comparison.** An allowance, not an estimate: the last relative change between
the two finest meshes of each design, combined in quadrature. It is labelled as such in the output.

**6. Primary agreement.** E_n = |r_meas − r_pred| / √(U_meas² + U_pred²), with expanded uncertainties (k = 2).
- E_n ≤ 1: **consistent**.
- E_n > 1: **inconsistent**.

**7. Secondary agreement.** The absolute compliance of each design against the prediction scaled by E_study / E_coupon:
- E_coupon is the flexural modulus from the coupons (three-point bending, span 64 mm, E = L³ m / (4 b h³), with m the
  initial load–deflection slope);
- its uncertainty is the Type A spread of the five coupons, combined with the terms above.

The same E_n rule applies. A printed part is not isotropic, and NAVIER assumes it is, so a secondary inconsistency is
reported as such, together with that known model difference.

**8. Reporting.** Every specimen and cycle, the exclusions with their reasons, the uncertainty budget, and both
readings. A consistent result is evidence for this bracket pair, material, mounting and load range only.

## 8. Sources of disagreement to report

- Clamp compliance and slip, beyond the base movement that is measured.
- Printed anisotropy, porosity, perimeter and infill differences between the thick and thin regions.
- Creep during the 10 s hold.
- Probe location: underside below the pad centre, against the model's pad-mean displacement. The plate compression
  under 3 kg is about 3e-4 mm, negligible.
- The staircase representation of B's chamfer in the model: B's last mesh change was 3.5 % at 1 mm and 1.7 % at
  0.5 mm.
