# Thin sheets: a rubber sheet drapes over a ball

Up: [the lab's documents](README.md) · code: [src/lab/sheet/](../../src/lab/sheet/sheet.h) · verification:
`tools/sheettest.c` (`./build/sheettest`, in `make test-lab`)

![A 0.5 mm natural-rubber sheet falls 20 cm onto a ball resting on a steel floor, stretches over the crown and pleats
into radial folds; colour is the stretch](../media/lab/rubber-sheet.gif)

A square of natural rubber 0.8 m across and 0.5 mm thick falls 20 cm onto a ball 30 cm across. It is stretched most
over the crown (yellow, 3 % at the moment it lands), the part that hangs pleats into radial folds, and the corners
spread on the floor and are held there by friction. Nothing places the folds: they are where a sheet that resists
stretching far more than bending has to buckle to hang round a sphere. 25 921 nodes, 51 200 triangles, 34 000 steps
in 60 s on the development laptop.

## The model

| Part | What is computed | Reference |
|---|---|---|
| Stretch | per triangle, the incompressible neo-Hookean membrane W = mu/2 (tr C + 1/det C - 3), C the in-plane Cauchy-Green tensor, the third stretch from incompressibility | Treloar 1975 |
| Bending | per triangle, the shape operator from the dihedral angles of its three edges (the mid-edge normal model), and the Kirchhoff plate's energy A D/2 (nu (tr M)^2 + (1 - nu) tr M^2), D = E H^3 / (12 (1 - nu^2)), E = 3 mu, nu = 1/2 | Grinspun et al. 2006; Chen et al. 2018 |
| Free edges | the free edge's mid-edge normal is minimised out (a Schur complement per triangle): the plate's anticlastic freedom | |
| Clamps | an edge against a fully pinned triangle takes that triangle's normal (the whole angle, not half) | sheettest C2 |
| Viscosity | Kelvin-Voigt: a stress 2 eta dE/dt, optional | |
| Self contact | when `self_contact_m` is set, each node and the triangles near it (found through a spatial hash) that were not its neighbours at rest keep that distance: a damped penalty, stiff enough to stop a pair within the gap at the stable step; folds and separate pieces in one mesh (valve leaflets) | Ericson 2004 (closest point) |
| Contact | nodes held a set distance outside rigid bodies (signed distance of the shape library) and a floor; the inward speed removed, Coulomb friction on the rest | |
| Time | velocity Verlet at the stable step of stretch and bending waves, threaded | |

The first bending model, a hinge energy per edge with the weights 3 |e|^2 / (A1 + A2) of discrete shells, was measured
on a bent cylinder: 1.46 times the plate's energy along a right-triangle grid and 4.4 times across it, so no single
factor calibrates it. A squared-Laplacian energy (isotropic) was blind to saddles: a clamped plate with free sides bent
anticlastically at no cost (+130 %). The mid-edge model is within 3.5 % of the plate on cylinders in three directions
and on a saddle, on right-triangle and equilateral grids (the shortfall is the free edges' relaxation); on badly
shaped triangles it is not (2.25 times on a grid of obtuse slivers), so sheets are meshed with right or equilateral
triangles.

Rubber: vulcanised natural rubber, shear modulus 0.42 MPa (the small-strain value of Ogden's 1972 fit to Treloar's
1944 tests), density 930 kg/m3. Demonstration values, stated in the scenario: the viscosity of 20 Pa s (a loss factor
of 0.1 at about 330 Hz; natural rubber's is 0.1 to 0.2 at audio frequencies), friction 0.8, damping 1/s for the air.
Not modelled: the air, the Mullins softening of rubber on first stretch. The film was made without self contact (it
came later, sheettest C5 and C6): where folds press together they may pass through each other.

## Verification (`tools/sheettest.c`)

Criteria written on 2026-09-26 before the first runs; every amendment is dated in the source with the runs that led
to it.

| Case | Against | Criterion | Result |
|---|---|---|---|
| C1 a rubber balloon blown up slowly | the neo-Hookean membrane's p(lambda) on its rising branch, at 0.5 and 0.9 of the peak pressure | 1 % | -0.009 %, -0.050 % |
| C2 a circular plate clamped at its rim under uniform pressure | Kirchhoff's q a^4 / (64 D) at the centre | 3 % on 24 rings | -0.94 % (-2.18, -1.44, -0.69 % on 8, 16, 32 rings) |
| C3 a free sheet ringing in its plane | membrane plus kinetic energy | 1 % over 2000 steps | 0.40 % |
| C4 a free sheet released from a bump (stretching and bending) | membrane plus bending plus kinetic energy | 1 % over 2000 steps | 0.32 % |
| C5 a sheet dropped onto another resting on a floor | the self-contact distance, 2 mm, as the gap once settled; never under 1 mm | 10 % | -0.0 %; closest 1.94 mm |
| C6 two sheets thrown at each other at 0.5 m/s | momentum; never closer than a quarter of the distance; kinetic energy not gained | 1e-9 | 3.5e-16; closest 1.97 mm; 93 % of the energy lost (an inelastic penalty) |

The stable step is taken from the shortest altitude of the triangles that can move (it was the square root of twice
the area, which a sliver's altitude falls far below: a valve's leaflets found it); the energy results above are at
that smaller step. C2 was first a clamped strip against w L^4 / (8 D); it failed three times (-57.6 %, -20.6 %, +15.3 %) and the record
shows why: the hinge model was anisotropic, and the strip itself has no closed form (it lies between cylindrical
bending and the beam). The circular plate is an exact plate solution with no free edges. Its first run, +7.06 %,
converged at first order: the clamp sat half a cell out until clamped edges took the whole angle.

## Running it

```bash
make lab
./build/sheettest
./build/labrun examples/lab/rubber_sheet.json /tmp/rubber.lab     # 34 000 steps, 1.2 s of fall and drape, about a minute
```

Keys ([rubber_sheet.json](../../examples/lab/rubber_sheet.json)): `material` {`shear_modulus_pa`, `density_kg_m3`,
`viscosity_pa_s`, `source`}, `sheet` {`size_m` [x, y], `centre_m`, `thickness_m`, `cell_m`, `contact_distance_m`},
`bodies` (labshape.h), `floor` {`z_m`, `source`}, `friction`, `damping_1_s`, `gravity_m_s2`, `body_look`, `run`
{`end_s`, `frames`}. The result has a part `sheet` (triangles, node fields `stretch` and `speed`), `body` and `floor`.
