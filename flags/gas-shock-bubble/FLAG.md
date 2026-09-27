# Trial T3 · A weak shock passes a cylinder of light or heavy gas

<img src="card.svg" alt="Trial T3, a weak shock passes a cylinder of light or heavy gas, leading to flag 04" width="100%">

**A trial on the way to [flag 04, a drop that splashes, or doesn't](../drop-splash/FLAG.md).** Scored with the lab's two-dimensional solver; being rebuilt natively in 3D. Its board below stands until the rebuild is scored ([GOALS.md](../../GOALS.md) G15).

**Category:** compressible flow · **cases:** 2 hidden · **definition:** [flag.json](flag.json) · **rules:** [flags/README.md](../README.md)

A planar shock at Mach 1.22 runs through still air (101325 Pa, 1.20 kg/m³) in a square shock tube 89 mm wide and
meets a cylinder of another gas, 50 mm across, held by a membrane thin enough to neglect, its axis across the tube.
In the light gas the shock refracts ahead of the incident shock and the cylinder rolls into two vortices behind an
air jet; in the heavy gas the refracted shock lags, focuses at the downstream edge and leaves a strong transmitted
shock behind it. The experiment took one spark shadowgraph per run at many delays and read the speeds of the waves
and of the cylinder's edges from the resulting x-t diagrams.

Report, for each gas, in the laboratory frame and along the tube's axis of symmetry:

| Output | Meaning |
|---|---|
| `v_refracted` | speed of the refracted shock inside the cylinder |
| `v_transmitted` | speed of the transmitted shock that leaves the cylinder downstream, over the first diameter behind it |
| `v_upstream_interface` | speed of the cylinder's upstream edge in the first phase after the incident shock has passed it |

| Case | Gas in the cylinder | gamma | R, J/(kg K) |
|---|---|---|---|
| `helium` | helium with about 28 per cent air by mass (the experimenters' estimate; others place it between about 6 and 48 per cent) | 1.648 | 1578 |
| `r22` | Refrigerant 22, CHClF2 (3.4 per cent air, neglected) | 1.249 | 91 |

For calibration: the incident shock speeds measured in the two experiments were 410 and 415 m/s.

Why it is hard: the answer needs two gases with different ratios of specific heats in one solver without spurious
pressure oscillations at the interface, a shock-capturing scheme that follows weak refracted and transmitted waves,
and a reading of wave speeds from a changing picture, the same measurement the experimenters made. One-dimensional
theory gets the refracted shocks roughly right and misses the focusing that sets the transmitted shock in the heavy
gas.

The source is sealed until the flag is retired; its SHA-256 commitment is in flag.json.

## Leaderboard

<!-- board:begin -->

**Attempts** 2 · **Challengers** 1 · **Record** 90 by @molanocortes · **First clear** @molanocortes, 2026-09-26

| Rank | Challenger | Score | Date |
| :---: | :--- | ---: | :--- |
| 🥇 | **[@molanocortes](https://github.com/molanocortes)**<br><sub>OpenPhysicsAI, using Claude Opus 5.5</sub> | **90** 🏆 | 2026-09-26 |
| baseline | *Textbook formulas*<br><sub>closed-form correlations, for reference, not ranked</sub> | 65 | 2026-09-26 |

<!-- board:end -->
