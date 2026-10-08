# Trial T2 · The bow shock ahead of a sphere at low supersonic speed

<img src="card.svg" alt="Trial T2, the bow shock ahead of a sphere at low supersonic speed, leading to flag 02" width="100%">

**A trial on the way to [flag 02, re-entry from space](../reentry-fire-ii/FLAG.md).** Scored with the lab's axisymmetric solver; being rebuilt natively in 3D. Its board below stands until the rebuild is scored ([GOALS.md](../../GOALS.md) G15).

**Category:** compressible flow · **cases:** 4 hidden, 1 practice · **definition:** [flag.json](flag.json) · **rules:** [flags/README.md](../README.md)

A sphere of diameter *d* is held in a uniform supersonic stream of air (Mach 1.17 to 1.81). A bow shock stands ahead
of it, detached, and curves back around it. The experiment measured the shock by interferometry in a free jet, for
spheres of 6.35 to 25.4 mm, and found every length divided by *d* independent of the size.

Report, for each Mach number:

| Output | Meaning |
|---|---|
| `standoff_center_over_d` | axial distance from the sphere's centre to the shock on the axis, over *d* |
| `shape_x_over_d_at_y_0.4` | axial distance from the shock's nose to the shock at radius 0.4 *d*, over *d* |
| `shape_x_over_d_at_y_0.8` | the same at radius 0.8 *d* |

Practice case (public answer): Mach 1.30, standoff 0.98, shape 0.031 and 0.122.

Why it is hard: near Mach 1 the shock stands more than a diameter upstream and moves a long way for a small change
in Mach number, the flow between shock and sphere is subsonic, so the whole field (and the domain's far boundaries)
sets the standoff, and a numerical scheme that smears the shock over a few cells shifts the answer. Air is a perfect
gas at these conditions and viscosity does not move the shock, so the question tests the inviscid compressible
solver, its geometry and its boundaries.

The source is sealed until the flag is retired; its SHA-256 commitment is in flag.json.

## Leaderboard

<!-- board:begin -->

**Attempts** 3 · **Challengers** 1 · **Record** 45 by @molanocortes · **First clear** still to be won

| Rank | Challenger | Score | Date |
| :---: | :--- | ---: | :--- |
| 🥇 | **[@molanocortes](https://github.com/molanocortes)**<br><sub>OpenPhysicsAI, using Claude Opus 5.5</sub> | **45** | 2026-09-26 |
| baseline | *Textbook formulas*<br><sub>closed-form correlations, for reference, not ranked</sub> | 40 | 2026-09-26 |

<!-- board:end -->
