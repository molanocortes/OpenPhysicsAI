# 04 — three-domain demonstration: heated plate in an air channel

Built and run over MCP by `make_three_domain.py`. **Demonstration only:** both materials are uncalibrated `demonstration` records and nothing here is compared with a measurement.

## What is modelled

* **Flow:** steady laminar incompressible air (constant properties, no buoyancy) through a 60 × 20 × 10 mm channel at 0.02 m/s, from the lattice Boltzmann solver. It is computed once and used from t = 0 (one-way: temperature does not change it). Periodic across y, no-slip in z.
* **Energy:** the air's energy is advected (SUPG, BiCGSTAB/ILU(0)). The 40 × 20 × 4 mm steel plate conducts. The two exchange heat through their shared nodes, **with no convection coefficient**. Partitioned (iterated every step) or monolithic.
* **Structure:** the plate's thermal expansion, clamped at its upstream end (one-way, linear elastic, stored times).

## Reference run (partitioned, 2 mm, tolerance 1e-3 / 0.05 K, paused and resumed)

* paused at t = 4102.7 s and resumed from its checkpoint; 59 accepted steps
* plate peak 45.9495 °C, mean outlet face 37.9768 °C after 7200 s
* heat through the interface 417.3177 J of the 720 J dissipated; largest displacement 0.01328 mm
* discrete energy closure 1.5416179826633088e-11; enthalpy-flux closure 0.000237 (the mapped flow is not exactly divergence-free for the energy equation's elements; see stageE-evidence.md)
* flow: relaxation time 0.5280, lattice Mach 0.0430, Re (channel) 13.30, density 0.9970–1.0139, section scale 1.0964–1.1935
* monolithic at the same settings: plate peak 45.9495 °C, 59 steps, 4.2 s wall against 9.8 s for the partitioned run (which includes the pause)

## Temporal study (2 mm, monolithic)

| tolerance | steps | plate peak °C | outlet face °C | interface heat J |
|---|---|---|---|---|
| 1e-3 / 0.05 K | 59 | 45.9495 | 37.9768 | 417.3177 |
| 1e-4 / 0.005 K | 100 | 45.9887 | 38.0039 | 416.8549 |
| 1e-5 / 0.0005 K | 235 | 46.0152 | 38.0223 | 416.5422 |

## Spatial study at a fixed 60 s step (monolithic)

Every mesh takes the same 120 backward-Euler steps, so the temporal error, large at 60 s, is nearly the same on each and largely cancels in the differences between levels. These are the numbers to judge mesh convergence by.

| cells | steps | wall s | plate peak °C | outlet face °C | interface heat J | largest displacement mm | enthalpy-flux closure |
|---|---|---|---|---|---|---|---|
| 2 mm | 120 | 3.1 | 45.9686 | 37.9900 | 417.0917 | 0.013287 | 0.000236 |
| 1 mm | 120 | 38.7 | 46.0273 | 37.9665 | 416.4023 | 0.013281 | 0.000129 |
| 0.5 mm | 120 | 366.1 | 46.0545 | 37.9540 | 416.0821 | 0.013280 | 0.000035 |

| quantity | observed order (2 → 1 → 0.5 mm) | extrapolated | reading |
|---|---|---|---|
| plate_peak_c | 1.11 | 46.0782 | monotone, asymptotic range plausible |
| outlet_face_mean_c | 0.91 | 37.9396 | monotone, asymptotic range plausible |
| interface_heat_j | 1.11 | 415.8042 | monotone, asymptotic range plausible |
| max_displacement_mm | 2.50 | 0.013280 | monotone, asymptotic range plausible |
| enthalpy_flux_closure | 0.87, 1.90 (from ratios; exact limit 0) | 0 | converging towards zero |

## Does the temporal error of the 60 s step cancel between meshes?

The same three meshes at 30 s, and the 2 mm and 1 mm meshes at 15 s (monolithic, fields stored hourly; storing fewer fields leaves the solution unchanged: on 2 mm at 60 s the largest difference from the run stored every 300 s is 0.0e+00).

| cells | step s | wall s | plate peak °C | outlet face °C | interface heat J | largest displacement mm |
|---|---|---|---|---|---|---|
| 2 mm | 60 | 3.1 | 45.9686 | 37.9900 | 417.0917 | 0.013287 |
| 2 mm | 30 | 5.0 | 46.0018 | 38.0131 | 416.6997 | 0.013304 |
| 2 mm | 15 | 8.4 | 46.0185 | 38.0246 | 416.5033 | 0.013313 |
| 1 mm | 60 | 38.7 | 46.0273 | 37.9665 | 416.4023 | 0.013281 |
| 1 mm | 30 | 53.5 | 46.0606 | 37.9895 | 416.0089 | 0.013298 |
| 1 mm | 15 | 91.1 | 46.0773 | 38.0011 | 415.8119 | 0.013307 |
| 0.5 mm | 60 | 366.1 | 46.0545 | 37.9540 | 416.0821 | 0.013280 |
| 0.5 mm | 30 | 632.8 | 46.0879 | 37.9770 | 415.6881 | 0.013297 |

Backward Euler's error is `C Δt`, so the 60 s error of each mesh is −2 [T(30 s) − T(60 s)]. What does not cancel in a mesh difference is the change of that error between the two meshes; the last two columns give it as a fraction of the mesh difference it contaminates.

| quantity | temporal order 2 mm, 1 mm | T(30 s) − T(60 s) at 2 / 1 / 0.5 mm | contamination 2 → 1 mm | contamination 1 → 0.5 mm |
|---|---|---|---|---|
| plate_peak_c | 1.00, 1.00 | 0.0332 / 0.0333 / 0.0334 | 0.4% | 0.4% |
| outlet_face_mean_c | 1.00, 1.00 | 0.023 / 0.023 / 0.023 | 0.0% | 0.1% |
| interface_heat_j | 1.00, 1.00 | -0.392 / -0.393 / -0.394 | 0.4% | 0.4% |
| max_displacement_mm | 1.00, 1.00 | 1.71e-05 / 1.71e-05 / 1.71e-05 | 0.4% | 1.5% |

Spatial orders again, at 30 s and on each mesh's time-extrapolated value 2 T(30 s) − T(60 s):

| quantity (30 s) | observed order (2 → 1 → 0.5 mm) | extrapolated | fine mesh − extrapolated | reading |
|---|---|---|---|---|
| plate_peak_c | 1.11 | 46.1116 | -0.0237 | monotone, asymptotic range plausible |
| outlet_face_mean_c | 0.91 | 37.9626 | 0.0143 | monotone, asymptotic range plausible |
| interface_heat_j | 1.11 | 415.4097 | 0.278 | monotone, asymptotic range plausible |
| max_displacement_mm | 2.51 | 0.013297 | 2.16e-07 | monotone, asymptotic range plausible |

| quantity (time-extrapolated) | observed order (2 → 1 → 0.5 mm) | extrapolated | fine mesh − extrapolated | reading |
|---|---|---|---|---|
| plate_peak_c | 1.11 | 46.1450 | -0.0238 | monotone, asymptotic range plausible |
| outlet_face_mean_c | 0.91 | 37.9857 | 0.0143 | monotone, asymptotic range plausible |
| interface_heat_j | 1.11 | 415.0152 | 0.279 | monotone, asymptotic range plausible |
| max_displacement_mm | 2.51 | 0.013314 | 2.13e-07 | monotone, asymptotic range plausible |

**Reading.** The temporal error of a 60 s step is the same on all three meshes to within 0.4% of the mesh differences it could contaminate (1.5% for the displacement), and the observed spatial orders do not change once it is removed: the fixed-step study measures spatial error. That order is about 1 (1.11 for the plate peak, 1.11 for the interface heat, 0.91 for the outlet face), not the 2 of smooth solutions on trilinear elements, and this study does not isolate why. Candidates: the plate starts at the inlet, where the uniform 20 °C inflow meets the heated no-slip interface, and ends at an adiabatic floor, so gradients are singular at both edges; and the flow is re-solved on each mesh with a different relaxation time (0.528, 0.556, 0.612) and mapped to nodes with a section-flux correction that shrinks only about as fast as the cell size (largest factor 1.193, 1.080, 1.032). The extrapolated values rest on three levels in that regime and are indicative, not established.

Best estimates, extrapolated in time and space: plate peak 46.145 °C, interface heat 415.0 J, outlet face 37.986 °C. Against them the reference run (2 mm, tolerance 1e-3) is -0.196 K on the plate peak (-0.7% of the temperature rise) and +2.30 J (+0.6%) on the interface heat.

## Spatial study, adaptive (tolerance 1e-4 / 0.005 K, monolithic)

Each mesh chose its own step sequence (100, 100 and 239 steps). The temporal study above shows that this tolerance leaves errors of a few hundredths of a kelvin, the same size as the differences between meshes, so this table mixes spatial and temporal error and is kept only as a record.

| cells | steps | wall s | plate peak °C | outlet face °C | interface heat J | largest displacement mm | enthalpy-flux closure |
|---|---|---|---|---|---|---|---|
| 2 mm | 100 | 6.5 | 45.9887 | 38.0039 | 416.8549 | 0.013297 | 0.000236 |
| 1 mm | 100 | 76.1 | 46.0474 | 37.9804 | 416.1646 | 0.013292 | 0.000129 |
| 0.5 mm | 239 | 1636.2 | 46.1002 | 37.9855 | 415.5427 | 0.013304 | 0.000035 |

| quantity | observed order (2 → 1 → 0.5 mm) | extrapolated | reading |
|---|---|---|---|
| plate_peak_c | 0.15 | - | observed order 0.15 is outside [0.5, 4]: not in the asymptotic range, no extrapolation |
| outlet_face_mean_c | - | - | not monotone: no order can be estimated |
| interface_heat_j | 0.15 | - | observed order 0.15 is outside [0.5, 4]: not in the asymptotic range, no extrapolation |
| max_displacement_mm | - | - | not monotone: no order can be estimated |
| enthalpy_flux_closure | 0.87, 1.90 (from ratios; exact limit 0) | 0 | converging towards zero |

The mesh changes the flow as well as the energy equation: the lattice uses the same cells, so the flow's resolution (5, 10 and 20 cells across the channel), its relaxation time and its compressibility bound change with it. No extrapolation is offered unless the three values are monotone with an order between 0.5 and 4.

## What this study does not show

* agreement with an experiment: there is none, and the materials are demonstration values;
* the flow start-up, buoyancy, temperature-dependent air properties or turbulence;
* two-way coupling with the structure: the plate's deformation does not change the thermal problem.
