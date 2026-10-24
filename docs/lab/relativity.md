# Light near a black hole

Up: [the lab's documents](README.md) · code: [src/lab/relativity/](../../src/lab/relativity/blackhole.h) ·
verification: `tools/rttest.c` (`./build/rttest`)

![Light bends around a black hole](../media/lab/black-hole.gif)

What you see: a camera 55 hole masses away looks at a non-rotating black hole with a thin disk of gas around it, and
sinks from 72 to 86 degrees off the disk's axis while the gas turns. Nothing in the picture is drawn by hand; every
pixel is a ray of light traced backward from the camera through curved spacetime, and it ends in one of three places.

- **The shadow.** Rays aimed inside 3 sqrt(3) = 5.196 masses of the centre spiral into the horizon. That dark disk is
  2.6 times the horizon's size: the photon sphere, not the horizon, sets it.
- **The disk, seen three times.** The near side of the disk crosses in front of the hole. Light from the disk's far
  side, which would be hidden behind the hole, is bent over the top of it: the arch above. Light from the disk's
  underside is bent under the hole and reaches the camera as the smaller ring below. A thin bright line hugs the
  shadow: light that went round the hole once before escaping.
- **The sky.** Stars behind the hole are displaced and drawn into arcs around it. From 55 masses away the whole field of
  view lies inside the hole's Einstein radius (sqrt(4 M / D), 15 degrees here), so every star you see is lensed.

The colours carry physics that the eye does not see directly. The gas on the left moves toward the camera at up to half
the speed of light: its light is shifted to the blue and beamed (brightness as g^4), so that side is brighter and whiter;
the right side moves away and is dimmer and redder. Every photon also climbs out of the hole's gravity and loses
energy on the way, so the inner edge is redder than its temperature alone would make it.

## The model

Units: G = c = 1, lengths and times in the hole's mass M (the horizon at r = 2 M, the photon sphere at 3 M, the
innermost stable circular orbit at 6 M; for a hole of ten suns, M is 14.8 km and 49 microseconds).

| Part | What is computed | Status |
|---|---|---|
| Rays | null geodesics of the Schwarzschild metric: in each photon's plane, d2u/dphi2 + u = 3 u^2 with u = 1/r, fourth-order Runge-Kutta, a step that shrinks near the hole | verified (R1 to R3) |
| The camera | a static observer at the camera's radius; a pixel's direction gives the impact parameter b = r sin(psi) / sqrt(1 - 2/r) | verified (R4 uses it) |
| The disk | thin, in the equatorial plane from 6 M to 20 M, gas on circular Keplerian orbits (angular velocity r^(-3/2)) | model |
| Its temperature | the Shakura-Sunyaev profile with no torque at the inner edge, T proportional to r^(-3/4) (1 - sqrt(6 M / r))^(1/4), scaled by `temperature_k` | model |
| The shift | g = E_seen / E_emitted = sqrt(1 - 2/r_cam) / (u^t (1 - Omega L_z)): gravitational redshift and the Doppler shift of the orbit together | verified along the axis (R4) |
| Brightness | bolometric intensity as g^4 T^4 (I / nu^3 is invariant along a ray) | follows from the shift |
| Colour | a blackbody at the shifted temperature g T (Helland's fit of the Planckian locus in sRGB) | display |
| The disk's streaks | smooth noise carried round at the Keplerian rate, so inner streaks turn faster and wind up | a texture for the eye, not a measurement |
| The sky | point stars from a hash of the direction, a faint galactic band | a picture, not a catalogue |
| Exposure | the 99.8th percentile of the bright part maps to near white, a filmic curve (Narkowicz's fit of ACES), sRGB gamma; the first frame's exposure is kept for the whole film | display |

Not modelled: the hole's spin (Kerr), light travel time across the disk, the disk's thickness and its own absorption,
polarisation. The disk's colour depends on `temperature_k`, which is a demonstration value: a disk around a stellar-mass
hole shines at about ten million kelvin in X-rays, around a supermassive one at tens of thousands of kelvin.

## Verification (`./build/rttest`)

Criteria were written on 2026-09-26 before the first run; the one amendment is dated in the source and printed.

| Case | Against | Criterion | Result |
|---|---|---|---|
| R1 | weak-field bending of a ray with b = 1000 M against 4/b + 15 pi / (4 b^2) | 0.01 % | +0.00107 % |
| R2 | the edge between captured and escaping rays, by bisection, against 3 sqrt(3) | 1e-5 | 2e-13 |
| R3 | the first integral (du/dphi)^2 + u^2 - 2u^3 = 1/b^2 along a ray that winds 1.47 turns (b = 5.1965) | 1e-9 relative | 2.4e-14 |
| R4 | a photon from the disk at r = 10 M reaching an observer far up the axis: g = sqrt(1 - 3/r) | 1e-3 | 1.5e-11 |

R3 was amended after its first run: the ray first chosen (b = 5.3) bends by 3.56 rad, not by a full turn, so it did not
test what the case was written to test; it now uses b = 5.1965. The tolerance did not change.

## Running it

```bash
make lab
./build/labrun examples/lab/black_hole.json /tmp/black_hole.lab     # 60 frames, 640 x 360, four rays a pixel: 1 min 45 s
./build/labfilm /tmp/black_hole.lab --out /tmp/bh --size 960x540
```

In the app: open the library, pick *Light bends around a black hole*, and change the inclination, the camera's distance
or the disk's temperature, then RUN WITH THESE.

Scenario keys ([black_hole.json](../../examples/lab/black_hole.json)):

| Key | Meaning |
|---|---|
| `camera.distance_M`, `camera.inclination_deg`, `camera.azimuth_deg`, `camera.fov_deg` | where the camera is (inclination 0: on the axis, 90: in the disk's plane) and its vertical field of view |
| `image.width`, `image.height`, `image.samples_per_axis` | pixels, and rays per pixel along each axis (1 to 4) |
| `image.exposure_ref` | fix the light that maps to white (default: from the first frame) |
| `disk.inner_M`, `disk.outer_M`, `disk.temperature_k` | the disk; leave `disk` out for a bare hole in front of the stars |
| `stars` | the sky (default true) |
| `run.frames`, `run.time_from_M`, `run.time_to_M`, `run.inclination_from_deg`, `run.inclination_to_deg`, `run.azimuth_from_deg`, `run.azimuth_to_deg` | a film: the gas turns with time, the camera moves between the two angles |

The result is a `.lab` file whose one part, `image`, carries fields `r`, `g` and `b`; the viewer shows such a part as a
picture that fills the frame instead of placing it in the room.
