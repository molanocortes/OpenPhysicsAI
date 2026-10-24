# Orbits (`src/lab/orbit`)

Planets, moons, asteroids and spacecraft under gravity, with the first relativistic correction around the Sun.
Up: [docs/lab/README.md](README.md) · code: [orbit.h](../../src/lab/orbit/orbit.h) · tests: [tools/orbtest.c](../../tools/orbtest.c) ·
initial conditions: [tools/horizons.py](../../tools/horizons.py)

## Model

- **Gravity:** Newtonian, every body on every body (test particles, GM 0, feel but do not pull), plus the first
  post-Newtonian term of the Sun for every other body (Anderson et al. 1975): Mercury's 43 arcseconds a century.
- **Integrator:** Gauss-Legendre implicit Runge-Kutta, s stages, order 2s (default 6 stages, order 12): symplectic
  and symmetric, so the energy error stays bounded; nodes and coefficients computed at start-up from the Legendre
  roots; stages iterated to round-off; compensated summation. Fixed step, or adaptive by step doubling for close
  encounters, with the closest approach of a tracked pair refined between steps by integrating to trial times.
- **Not modelled:** the asteroids (DE440 carries 343), the Sun's oblateness, non-gravitational forces on small
  bodies (the Yarkovsky effect on Apophis), tides, the relativistic terms between planets.
- **Initial conditions:** `tools/horizons.py` fetches barycentric state vectors from JPL Horizons and uses the DE440
  GM values of JPL's astrodynamic parameters page; the query and its date go into the scenario.

## Verification (tools/orbtest.c, criteria written before the first run)

| Case | Against | Criterion | Result |
|---|---|---|---|
| O1 Kepler orbit, e = 0.6, 100 revolutions | the closed form | 1e-9 of a; energy 1e-12 | 5.6e-10; 3.8e-15 |
| O2 Mercury about the Sun, 1PN | 6 pi GM / (c^2 a (1 - e^2)) a revolution | 1 % | 0.000 %: 42.98 arcseconds a century |
| O3 Sun, Jupiter, Saturn, 10 000 years | conservation | 1e-11, 1e-12 | 7e-14, 3e-14 |
| O4 hyperbolic flyby of the Earth | two-body periapsis | 1e-7 | 7e-15 (the tracker first gave 3e-6 with cubic interpolation alone; now refined by integration) |

## Against JPL

- **Apophis, 13 April 2029** ([apophis_2029.json](../../examples/lab/apophis_2029.json)): from the Horizons state of
  1 March 2029, the closest approach to the Earth's centre comes out at 38 011.4 km at 21:46 TDB; JPL's close-approach
  solution (orbit 220) gives 38 011.5 km at 21:46. A comparison with another model's prediction, not a measurement.
- **Flag [orbit-planets-2020](../../flags/orbit-planets-2020/FLAG.md):** planet positions ten and twenty years ahead,
  scored at one arcsecond against the observed positions; practice case Venus 2020 within 0.004 arcseconds.

## Scenario keys

`domain` = "orbit", `title`, `relativity`, `stages`, `step_days`, `adaptive`, `tolerance`, `bodies` [{`name`,
`gm_km3_s2`, `radius_km`, `position_km`, `velocity_km_s`}], `track` {`a`, `b`}, `run` {`end_days`, `frames`,
`paths`, `frame_origin` (a body's name: pictures centred on it)}.
