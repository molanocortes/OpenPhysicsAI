# Study 1 — heated component cooled by a prescribed convection coefficient

A 100 x 10 x 10 mm bar held at 300 degC on one end face and cooled by `h = 60 W/(m^2 K)` to 20 degC air on the other,
run to 20 000 s (ten diffusion times `L^2 rho cp / k = 2000 s`), so the field is effectively steady.

## Reference

One-dimensional series resistance:

    q = (T_hot - T_amb) / (L/(k A) + 1/(h A)) = 1.2923 W
    T_cold_face = T_amb + q/(h A) = 235.385 degC

## Result

| quantity | value |
|---|---|
| cold-face temperature | **235.385 degC** (analytic 235.385 degC) |
| energy closure error | 1.20e-11 |
| worst per-step balance error | 5.75e-12 |
| stored vs integrated enthalpy | 0.00e+00 |
| specification hash | `fb7fc2e8bf861a60bdb2e0f6efa7c14e53639c0104e6e5e38646bc829b2ccdec` |

The voxel mesh represents this box exactly (no staircase), so the agreement is a check of the discretisation and the
convection boundary term, not of the mesher.

## What this does not model

The cooling is a **prescribed heat-transfer coefficient**, not resolved airflow. No fluid is solved, so this is not
conjugate heat transfer and `h` carries all the uncertainty of whatever correlation it came from. The material is a
made-up constant-property alloy chosen for the closed-form reference; it is not a real material.

## Reproduce

```bash
make am
python3 "Thermal Sim/examples/make_examples.py"
```

The resolved setup is in `workspace/heated_bar/runs/<job>/spec.json` and can be replayed without any AI.
