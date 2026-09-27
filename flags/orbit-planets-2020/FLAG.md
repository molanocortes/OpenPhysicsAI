# Trial T4 · Where the planets are ten and twenty years later

<img src="card.svg" alt="Trial T4, where the planets are ten and twenty years later, leading to flag 09" width="100%">

**A trial on the way to [flag 09, Apophis passes the Earth](../apophis-2029/FLAG.md).** Its physics is already three-dimensional (every body pulls on every other); its pictures and page are being rebuilt with the others. Its board below stands until the rebuild is scored ([GOALS.md](../../GOALS.md) G15).

**Category:** orbits · **cases:** 4 hidden, 1 practice · **definition:** [flag.json](flag.json) · **inputs:** [inputs.json](inputs.json) ·
**rules:** [flags/README.md](../README.md)

Given the positions and velocities of the Sun, the planets and the Moon on 1 January 2000 (inputs.json, from the
JPL Horizons system, with the DE440 masses), predict where Mercury, Mars and Jupiter are seen from the Sun on
1 January 2010 and 2020: their heliocentric ecliptic longitude in degrees, in the frame of the inputs.

The answers are the planets' positions on those dates as determined from decades of observations. They are scored at
about one arcsecond (0.0003 degrees): tight enough that a Newtonian model misses Mercury, whose orbit turns by 43
arcseconds a century through general relativity, and loose enough that the small asteroids left out of the inputs
do not decide the score.

Practice case (public): Venus on 2020-01-01, longitude 4.155859 degrees.

Why it is not trivial: twenty years are 83 revolutions of Mercury; the integrator must keep phase to a part in ten
million, the planets perturb each other, and relativity is not optional.

## Leaderboard

<!-- board:begin -->

**Attempts** 2 · **Challengers** 1 · **Record** 100 by @molanocortes · **First clear** @molanocortes, 2026-09-26

| Rank | Challenger | Score | Date |
| :---: | :--- | ---: | :--- |
| 🥇 | **[@molanocortes](https://github.com/molanocortes)**<br><sub>OpenPhysicsAI, using Claude Opus 5.5</sub> | **100** 🏆 | 2026-09-26 |
| baseline | *Textbook formulas*<br><sub>closed-form correlations, for reference, not ranked</sub> | 0 | 2026-09-26 |

<!-- board:end -->
