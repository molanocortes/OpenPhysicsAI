# Material and manufacturing assumptions

## What the model uses

One isotropic, linear-elastic material per design: Young's modulus E, Poisson's ratio ν, and a density for mass and
self-weight. The values are evaluated at the reference temperature (20 °C by default). Nothing else about the
material enters the stiffness comparison.

## Where values come from

Every input carries a source, and the evidence record keeps it:

| Source | Meaning | Recorded as |
|---|---|---|
| `user` | stated by the user | an input |
| `measured` | measured on the real part or material batch | an input |
| `database` | a handbook, datasheet or library value | an input, with its reference |
| `inferred` | deduced by the assistant from context | an **assumption** |
| `default` | assumed without evidence | an **assumption** |

The built-in library (`materials_list`, `material_list`) holds, since 2026-09-19, records with a source on every value (status `measured` or `published`, 24 of 28) and a few **demonstration** records labelled as such: typical magnitudes that are not traceable to
a grade, supplier or test. A study using them says so, and treats absolute values as indicative only. For a real
decision, supply a record with its reference (for example the supplier certificate of the batch).

## What a material uncertainty does to the comparison

- **Young's modulus.** When every design uses the same material and the loads are forces, every displacement scales
  exactly with 1/E. The ranking and the ratio between designs therefore do not depend on E; absolute values scale
  with it. The study verifies this numerically with one run at the upper factor (to 1e-6). When designs use
  different materials, their moduli can change the ranking, and the record says that this was not assessed.
- **Poisson's ratio.** It is not a simple scale factor: declare values in `sensitivity.poisson_ratio` and each design
  is solved again.
- **Density** affects mass, and self-weight when it is included.

## Manufacturing

| Process | Treatment |
|---|---|
| unspecified | recorded assumption: homogeneous, isotropic, defect-free material with the values as given |
| machined, sheet metal, cast, molded | the values are used as given; the process is recorded. Residual stresses, casting porosity and forming anisotropy are not modelled |
| fff, sla, sls, mjf, lpbf (additive) | a **blocking question** unless `effective_properties` is `measured_for_this_process`. Printed parts are anisotropic, and their stiffness depends on orientation, infill, layer bonding and porosity. The user may accept isotropic bulk values with a stated reason: the result is then conditional on that, and interlayer failure and printed anisotropy are listed as not evaluated |

No printed-part **strength** prediction is made in any case.

## Mounting and loading are assumptions too

- **Mounting:** `fixed` makes every displacement of the mounting region zero, the stiffest idealisation of a bolted
  or bonded joint. Real joints (bolt preload, washers, wall flexibility, contact) are softer, and not equally so for
  every design. Declare a `mounting_alternatives` entry, such as only the bolt pads held, to bound the effect. An
  inferred or default mounting without an alternative is a blocking question.
- **Load:** a payload mass becomes a force m·g along the stated direction, with g recorded. The payload's own mass is
  distinct from the bracket's self-weight, which is included only on request. The load is a uniform traction over
  the load region. A payload bearing on part of the region, or with an offset centre of mass, would load the bracket
  differently, and this is not assessed.
