# Study 3 — melting and solidification

A 10 mm cube of the `ss316l_lpbf_demo` material (solidus 1375 degC, liquidus 1400 degC, latent heat 270 kJ/kg) heated
uniformly at 4e8 W/m^3 for 20 s, which carries it through the solidus and part way through the mushy zone.

## Result

| quantity | value |
|---|---|
| largest molten volume | **716.815 mm^3** of 1000 mm^3 |
| peak temperature, latent heat applied | **1392.9 degC** |
| peak temperature, `phase_change: false` | 1669.4 degC |
| held back by the latent heat | 276.5 K |
| energy closure error | 4.49e-14 |
| stored vs integrated enthalpy | 2.61e-15 |
| setup warnings | PHASE_CHANGE_ACTIVE, DEMONSTRATION_MATERIAL |
| specification hash | `382ea5a1c8ce1a58c99aef29022b20aa0a5af0921fb7e4192773c39dd4c795d5` |

The second run is the same study with the phase change switched off; the difference is the latent heat doing its job.
Both runs report which choice was made — the module never applies or drops latent heat silently.

The sharp verification of this model is in `build/thermtest`: a melt/re-freeze cycle returns to its starting
temperature with an enthalpy mismatch of 8.7e-14, and a one-phase Stefan problem matches the similarity solution
`s(t) = 2 lambda sqrt(alpha t)` to **-0.12 %**.

## What this does not model

The liquid fraction is **linear between solidus and liquidus and assumed to be at local equilibrium**. There is no
undercooling, no nucleation, no hysteresis (freezing retraces the melting curve) and no solute redistribution. The
mushy interval is a model parameter that changes the answer, so a study that depends on it should be repeated with a
different interval. Molten material conducts like a solid: there is no melt-pool convection. The material values are
demonstration data, not calibrated 316L.

## Solver note

Latent heat makes the step stiff: inside the mushy zone `dH/dT` is about 8.6e7 J/(m^3 K), so a temperature tolerance
is a much looser energy tolerance. This study uses `temperature_tolerance: 1e-9` and `max_iterations: 1500`. Without
Aitken relaxation the Picard iteration for this case **diverges** — the fixed-point map has derivative `-B/Q` with
`B` the latent heat still to be absorbed and `Q` the heat delivered in the step.
