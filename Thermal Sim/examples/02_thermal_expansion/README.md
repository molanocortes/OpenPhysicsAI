# Study 2 — thermally loaded solid with deformation

The bar of study 1, anchored on the heated end face, solved as a **one-way thermomechanical** analysis: the
temperature history drives the thermal strain and the temperature-dependent stiffness, and the structure does not feed
back into the temperature field.

## Result

| quantity | value |
|---|---|
| temperature range | 235.38 .. 300.00 degC |
| largest displacement | **0.3020 mm** |
| estimate from the mean temperature | `alpha * (T_mean - T_0) * L` = 0.2972 mm |
| energy closure error | 1.20e-11 |
| specification hash | `92a3a96db1185311fa0083574fc7ff00946cdcff97e09f2bab547b9c587da9f8` |

The estimate uses the mean of the end temperatures; the real profile is not linear, so the two are close rather than
equal. It is a sanity bound, not a verification case — the verification of thermal strain is in `build/femtest`
(free and constrained expansion against closed-form values).

## What this does not model

**This is not a residual-stress prediction.** The solid is linear elastic: with no plasticity or creep the thermal
stresses vanish again when the part returns to a uniform temperature, so nothing is left behind. Residual stress needs
a history-dependent constitutive model, which is not implemented. There is also no two-way coupling: the deformation
does not move the thermal boundaries.
