# Stage C — acceptance criteria, fixed before the first comparison

Written 2026-09-16 before `tools/contactflow.py` was run for the first time. All steps go through MCP only.

**Reference problem.** Two bodies meeting face to face: A = 20 × 10 × 10 mm (k = 50 W/(m·K)), B = 20 × 10 × 10 mm
(k = 10 W/(m·K)), both ρ = 8000 kg/m³, c_p = 500 J/(kg·K); 5000 W/m² into A's far face, convection 100 W/(m²·K) to
20 °C on B's far face, interface conductance 2000 W/(m²·K); adaptive run to 30 000 s (≈ 16 dominant time constants, so
the remaining transient is below 1e-5 K). Series resistance, cross-section 1e-4 m²:

    Q = 0.5 W,   R_A = 4 K/W,   R_contact = 5 K/W,   R_B = 20 K/W,   R_conv = 100 K/W
    T_B,far = 70.00 °C,  T_B,interface = 80.00 °C,  T_A,interface = 82.50 °C,  T_A,far = 84.50 °C,  jump = 2.5 K

| # | Test | Criterion |
|---|---|---|
| C1 | conductance interface | both interface side temperatures and the far-face temperatures within 1e-3 K of the series-resistance values; jump within 1e-3 K of 2.5 K; heat rate from B into A within 1e-5 W of -0.5 W |
| C2 | independent energy check | each body's own balance (stored - sources - boundary - prescribed - interface) closes to 1e-9 relative, and both bodies report `independently_closed` |
| C3 | reproducibility | the project saved, reopened in a new MCP server process, meshed again and solved again reproduces the side temperatures and heat rate to 1e-12 relative |
| C4 | model variants | thin_layer with k/d = 2000 W/(m²·K) equals the conductance result to 1e-9 K; perfect contact has zero jump and T_A,far within 1e-3 K of 82.00 °C; insulated carries exactly 0 W and leaves B at exactly 20 °C |
| C5 | refused topologies | separated bodies (gap), the same body twice, a second interface on the same pair, and overlapping bodies are each refused with a diagnostic naming the problem |
| C6 | exterior condition on the interface | a convection condition on all faces of A is reported as reaching the interface with 100 mm² ± 1 %, and its mesh area excludes that area |
| C7 | mechanical bonding kept | a thermomechanical run with only A's far face fixed succeeds (B is held only through the bond; a mechanically split B would be a rigid-body mode and fail) |
| C8 | preview | `interface_preview` returns a PNG and a coverage of 16 faces and 100 mm² for a 2.5 mm mesh |

## Amendments

**C4, insulated interface — "leaves B at exactly 20 °C" amended to "B within 1e-9 K of 20 °C, with less than 1e-9 J
stored and exactly 0 J through the interface"** (2026-09-16, after the first run). Observed: B at 20.00000000000864 °C
after 600 s while the interface heat was exactly 0 J. A control run with no condition at all on B drifted by the same
order (20.000000000006366 °C). The drift is floating-point round-off of the conduction operator applied to a uniform
293.15 K field (its row sums vanish only to ~1e-16 relative), turned into a correction by the linear solve; it is not
heat crossing the interface. Exact equality was a criterion floating point cannot meet. The same run exposed a
reporting defect that was fixed rather than accepted: B's `balance_error` was normalised by B's own picojoule-sized
energies and read 1.29; body balances are now normalised by the model's energy scale and also report `residual_j`.
