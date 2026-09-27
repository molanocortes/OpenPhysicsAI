# Brittle fracture: a glass bottle shatters

Up: [the lab's documents](README.md) · code: [src/lab/fracture/](../../src/lab/fracture/peri.h) · verification:
`tools/peritest.c` (`./build/peritest`, in `make test-lab`)

![A glass bottle falls 1.3 m onto concrete: cracks run up from the edge of its base, branch round the body and the
shoulder, and the base breaks into fragments](../media/lab/glass-bottle.gif)

A glass bottle dropped from 1.3 m lands on the edge of its base. Cracks start where the glass is stretched past what it
can bear, run up the wall at hundreds of metres a second, branch, circle the shoulder, and the base breaks into
fragments. Nothing tells the cracks where to go: they are where bonds broke. Colour is damage, the share of a point's
bonds that have broken: bottle green where the glass is whole, white along the cracks and in the fragments.

## The model

Bond-based peridynamics (Silling 2000), the method made for cracks that start, run and branch on their own. The glass
is 116 000 points on a 1 mm lattice (the wall is 3 mm), each bonded to every point within a horizon of 3 mm; a bond is
a spring along its line, and it breaks for good when stretched past a critical stretch.

| Part | What is computed | Reference |
|---|---|---|
| Material | the prototype microelastic brittle material: the micromodulus from the bulk modulus (Poisson's ratio 1/4, the bond-based model's own), normalised by the lattice's own sum so that an interior point is exact | Silling and Askari 2005 |
| Breaking | the critical stretch s0 = sqrt(5 G0 / (9 K delta)), which makes the energy to open a unit area of crack the fracture energy G0 = K_Ic^2 / E | Silling and Askari 2005 |
| Partial volumes | points on the horizon's edge count by the share of their cell inside it | Bobaru and co-workers |
| Time | velocity Verlet at 0.8 of the stable step (0.14 microsecond) | |
| Floor | a rigid plane pushing back by a penalty stiff enough for a point to stop within a step | |

Glass: soda-lime, Young's modulus 72 GPa, density 2.5 g/cm3, fracture toughness 0.75 MPa m^0.5 (Varshneya and Mauro,
Fundamentals of Inorganic Glasses, 3rd ed., 2019); its Poisson's ratio of 0.22 is taken as the model's 1/4. Not
modelled: the flaws on a real bottle's surface that decide where its cracks start (the lattice here is flawless, so
cracks start where the stress is highest), residual stress from forming, the concrete's own give, air.

## Verification (`tools/peritest.c`)

Criteria written on 2026-09-26 before the first run; the amendments are dated in the source with the first runs.

| Case | Against | Criterion | Result |
|---|---|---|---|
| P1 a cube stretched uniformly by 1e-4 | the continuum's energy density 9/2 K eps^2 at interior points | 5 % | exact, after the micromodulus was normalised by the lattice (first run +6.22 %) |
| P2 the bonds crossing a plane, broken at the critical stretch | the fracture energy G0 | 10 % | +0.85 % |
| P3 a free cube set ringing | kinetic plus elastic energy | 0.5 % | 0.31 % |
| P4 a notched plate pre-stretched to 0.8 of critical and held | a crack runs at least half the plate, slower than Rayleigh waves | | 39.5 mm at 950 m/s, the Rayleigh speed 3077 m/s |

P4's first set-ups did not test what they meant to: grips set moving at once broke the plate at the grips, and a
uniform stretching rate brought the whole plate to critical at once. The pre-stretched plate is the set-up of Ha and
Bobaru (2010) for dynamic brittle fracture.

## Running it

```bash
make lab
./build/peritest
./build/labrun examples/lab/glass_bottle.json /tmp/bottle.lab     # 18 000 steps, 2.5 ms of impact, about 2.5 minutes
```

Keys ([glass_bottle.json](../../examples/lab/glass_bottle.json)): `material` {`youngs_modulus_pa`, `density_kg_m3`,
`fracture_toughness_pa_sqrt_m`, `source`}, `spacing_m`, `bodies` (labshape.h), `box_m` (where the lattice is laid),
`velocity_m_s`, `floor` {`z_m`, `source`}, `gravity_m_s2`, `palette`, `run` {`end_s`, `frames`}. The result has a part
`glass` (points, fields `damage` and `speed`) and `floor`.
