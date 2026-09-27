# Assembly and study formats

## navier-assembly (version 1)

```json
{
  "format": "navier-assembly", "version": 1, "name": "arm_rig",
  "units": {"length": "mm", "angle": "deg", "mass": "g"},
  "gravity": ["0 m/s^2", "0 m/s^2", "-9.80665 m/s^2"],
  "bodies": [
    {"name": "arm", "part": "arm_part",
     "mass_properties": {"source": "mesh", "file": "arm.stl", "units": "mm",
                         "fill": {"model": "shell_infill", "density": "1240 kg/m^3", "shell_thickness": "0.8 mm",
                                  "infill_fraction": 0.2, "source": "user"}},
     "frames": [{"name": "elbow", "position": [120, 0, 0], "rpy": [0, 0, 0]}],
     "material": {"id": "pla_generic_demo", "source": "user"},
     "manufacturing": {"process": "FFF", "layer_height": "0.2 mm", "print_orientation": "flat"},
     "geometry": {"collision": [{"name": "arm_pad", "type": "box", "size": [120, 16, 10], "pose": {"position": [60, 0, 0]},
                                 "friction": 0.6, "friction_source": "user", "restitution": 0, "group": 0}]}},
    {"name": "payload",
     "mass_properties": {"provenance": "measured", "mass": "150 g", "com": [0, 0, 0],
                         "inertia": {"ixx": "20 kg*mm^2", "iyy": "20 kg*mm^2", "izz": "20 kg*mm^2", "ixy": 0, "ixz": 0, "iyz": 0}}}
  ],
  "joints": [
    {"name": "shoulder", "type": "revolute", "parent": "world", "child": "arm",
     "parent_frame": {"position": [0, 0, 0]}, "child_frame": {"position": [0, 0, 0]}, "axis": [0, -1, 0],
     "motion": "actuated", "initial": {"position": 0, "velocity": 0},
     "limits": {"lower": -30, "upper": 120, "restitution": 0, "effort": "2 N*m", "velocity": "180 deg/s"},
     "spring": {"stiffness": "0.1 N*m/rad", "reference": 0}, "damping": "0.001 N*m*s/rad",
     "friction": {"coulomb": "0.002 N*m", "regularization_velocity": "0.5 deg/s"}},
    {"name": "payload_mount", "type": "fixed", "parent": "arm", "child": "payload",
     "parent_frame": "arm.elbow", "child_frame": {}}
  ],
  "couplings": [{"name": "mimic", "follower": "finger_r", "driver": "finger_l", "ratio": 1, "offset": 0}],
  "environment": [{"name": "floor", "type": "plane", "friction": 0.5, "friction_source": "default"}]
}
```

Rules:

- **Units.** Plain numbers use the unit that `units` declares for their quantity; undeclared quantities are SI. Strings carry
  their own unit. Inline definitions sent over MCP need explicit units for every dimensional plain number except zero.
- **Frames.**
  - Every joint states `parent_frame` and `child_frame`. Only a `free` joint may omit both, and that is recorded as an assumption.
  - Revolute and prismatic joints state an `axis` in the joint frame.
  - A frame may reference a named frame of its own body: `"body.frame"`.
  - At q = 0 the two joint frames coincide.
- **Mass properties.**
  - Explicit form: mass, centre of mass, and inertia about the centre of mass in body axes. Inertia uses tensor components,
    so ixy = −∫xy dm. `inertia_rpy` rotates the given axes into the body axes.
  - Mesh form: the closed surface of an STL with a declared fill. There is no default fill.
  - Provenance is one of user, measured, cad, computed, inferred, default or calibrated.
- **Loops.** A joint between two bodies that are already connected closes a kinematic loop and is enforced as a constraint.
- **Contact geometry.**
  - Bodies declare `geometry.collision` shapes in body coordinates; `environment` holds static world shapes.
  - Shape types: `box` (`size` = full extents), `sphere` (`radius`), `capsule` (`radius`, `length` of the segment along
    the shape z axis) and `plane` (the half-space below the xy plane of its pose; its +z is the outward normal).
  - `mesh` and `cylinder` shapes are accepted in the document but refused when contact runs
    (`UNSUPPORTED_CONTACT_GEOMETRY`). A part is represented by the primitive pieces the user declares, never by a
    silent convex hull.
  - `friction` needs `friction_source` (measured, calibrated, user, cad, computed, inferred or default); without it
    the document is refused (`CONTACT_PROVENANCE_REQUIRED`). A shape without friction cannot take part in contact
    (`CONTACT_FRICTION_REQUIRED`). Default or inferred coefficients raise a warning.
  - `restitution` defaults to 0 (recorded). Pair values: friction = min of the two shapes, restitution = max.
  - Shapes with the same non-zero `group` never touch. Bodies connected by a joint never touch each other
    (a body jointed to the world never touches environment shapes).
  - Shape names must be unique: contact results and load attachments refer to them. Unnamed shapes are called
    `<body>.collision<k>` and `environment<k>`.
- **Flexible bodies.** A body may carry `"flexible"`, the reduced elastic model written by `mech_flexible_attach` from a
  `mech_flexible_reduce` job. It is always SI and in body axes, whatever `units` declares, and is not meant to be edited:
  ```json
  "flexible": {"method": "craig_bampton", "units": "SI",
               "omega2": [n], "zeta": [n], "ell": [6n], "dJ": [9n],
               "interfaces": [{"joint": "payload_mount", "point": [0.12, 0, 0], "phi": [3n], "psi": [3n]}],
               "fe_mass_properties": {"mass": 0.0238, "com": [3], "inertia": [9]},
               "provenance": {"body": "arm", "part": "arm_part", "root_selection": "hinge", "mesh_hash": "...", "damping": {...}, "job_id": "..."}}
  ```
  - `omega2` are the squared angular frequencies of the mass-normalised elastic coordinates, ascending; `zeta` their
    modal damping ratios. `ell` holds, per coordinate, [∫ρ x × φ dV; ∫ρ φ dV] about the body origin; `dJ` the
    first-order change of the inertia tensor about the body origin (row-major 3×3).
  - Each interface is ridden by the named child joint, whose `parent_frame` origin must equal `point`. `phi` and `psi`
    are the interface translation and rotation per unit coordinate.
  - The body's mass properties must equal `fe_mass_properties` (`FLEXIBLE_MASS_PROPERTIES` otherwise): rigid and
    elastic inertia come from the same mesh. A renamed or re-parented interface joint is refused
    (`FLEXIBLE_INTERFACE_JOINT`), a moved one fails compilation (`INTERFACE_FRAME_MISMATCH`), and a massless link fixed
    to a flexible body is refused (`FLEXIBLE_MERGED_LINK`).
  - Other joints of the body ride its reference frame, which is held at the root region (warned). Contact shapes on a
    flexible body are refused when contact runs (`CONTACT_ON_FLEXIBLE_BODY`): mount contact geometry on a body attached
    to an interface.
- **Unknown keys** are reported.
- **Canonical form.** `asm_to_json` writes SI numbers and quaternions, provenance, carried assumptions and unsupported content.
  Reloading it gives the same model, and dumping it again gives identical text (tested).

## navier-mech-study (version 1)

```json
{
  "format": "navier-mech-study", "version": 1, "name": "lift", "plain_numbers": "require_units",
  "settings": {"end_time": "2 s", "max_step": "0.5 ms", "record_period": "2 ms"},
  "snapshots": ["0.4 s"],
  "actuators": [{"name": "motor", "type": "dc_motor", "joint": "shoulder", "gear_ratio": 150, "efficiency": 0.6,
                 "rotor_inertia": "1.5 g*cm^2", "resistance": "4 ohm", "torque_constant": "10 mN*m/A",
                 "back_emf_constant": "0.01 V*s/rad", "voltage_limit": "12 V", "current_limit": "1.5 A"}],
  "sensors": [{"name": "encoder", "type": "joint_position", "joint": "shoulder", "period": "1 ms", "latency_samples": 1,
               "resolution": "0.09 deg", "noise_std": "0.01 deg", "seed": 11}],
  "controllers": [{"name": "pid", "type": "pid", "actuator": "motor", "feedback": "encoder", "loop": "position",
                   "period": "1 ms", "delay_samples": 1, "kp": 40, "ki": 80, "kd": 0.5, "derivative_filter": "3 ms",
                   "output_limits": ["-12 V", "12 V"], "antiwindup": "clamp",
                   "reference": {"type": "min_jerk", "start": "0.1 s", "from": "0 deg", "to": "60 deg", "duration": "0.6 s"}}]
}
```

- **Quantity dimensions** follow the joint:
  - revolute: N·m, rad, rad/s, N·m/rad
  - prismatic: N, m, m/s, N/m

  Controller gains are plain SI numbers (command units per error unit).
- **Where units live.** In `settings` and in each component, never globally. That way, adding a component cannot
  reinterpret another component's numbers.
- **Noisy sensors** need a seed.
- **Contact.** `settings.contact = {"enabled": true, "margin": "1 mm", "max_iterations": 500, "tolerance": 1e-10,
  "restitution_speed": "1 mm/s"}` switches the study to first-order time stepping with impulses. Each contact pair
  gets histories (`contact.<A>.<B>.normal_force`, `friction_force`, `sliding`, `min_gap`, `impact`) and a
  `peak_contact:<A>.<B>` snapshot. Steps with an impact are reported as impulses and kept out of force peaks, peak
  snapshots and the joint load envelope. When collision shapes exist and contact is off, validation warns that
  bodies pass through each other.
- **Flexible bodies.** `settings.flexible_initial_state` is `static_equilibrium` (default: the elastic coordinates start
  in equilibrium with gravity and the initial motion while the joints are held, so a held part does not ring at t = 0)
  or `undeformed` (the loads act suddenly at t = 0). Steps are limited to 0.5 / ω_max of the elastic coordinates
  (`flexible_step_limit_s` in the summary). Each flexible body records `<body>.elastic[k]`, `<body>.strain_energy` and,
  per interface, `<body>.<joint>.dx/dy/dz/deflection` (m) and `rx/ry/rz/rotation` (rad) in body axes relative to the
  reference frame; the summary reports peak interface deflection and rotation and peak strain energy with their times.
- **Feedback.** A PID states its feedback: a sensor name, or `ideal`, which is reported as an idealisation.
- **Run specification.** Every run stores `spec.json` (`navier-mech-run-spec`): the canonical assembly and study in SI,
  the integrator settings, and the SHA-256 of all of it. That file alone reproduces the run.

## URDF subset

The subset is documented in `src/mech/urdf.c`. The root link's connection to the world is an explicit option (`fixed` or
`free`). A link without an inertial becomes a frame of its parent when it is fixed to that parent, and is an error when it
moves. `<transmission>`, `<gazebo>`, `<safety_controller>`, `<calibration>`, planar joints and unknown elements are listed
as not imported.

## navier-mech-materials (version 1)

`<project>/mechanics/materials.json` holds orthotropic printed-part materials. The shared material library is
isotropic and is not edited.

```json
{"format": "navier-mech-materials", "version": 1, "materials": [
  {"id": "pla_demo", "model": "orthotropic", "density": "1240 kg/m^3",
   "elastic": {"E1": "3.2e9 Pa", "E2": "3.0e9 Pa", "E3": "2.4e9 Pa", "nu12": 0.35, "nu13": 0.30, "nu23": 0.32,
               "G12": "1.15e9 Pa", "G23": "0.85e9 Pa", "G13": "0.9e9 Pa", "source": "assumed", "reference": "..."},
   "strength": {"Xt": "5e7 Pa", "Xc": "6e7 Pa", "Yt": "4.5e7 Pa", "Yc": "5.5e7 Pa", "Zt": "2.5e7 Pa", "Zc": "5e7 Pa",
                "S12": "2.5e7 Pa", "S23": "1.5e7 Pa", "S31": "1.5e7 Pa", "f12": -0.5, "f13": -0.5, "f23": -0.5,
                "source": "assumed", "reference": "..."},
   "print": {"process": "FFF", "layer_height": "0.2 mm", "raster": "0/90"}}
]}
```

- **Axes.** Axis 3 is the build direction (normal to the layers). Axis 1 is the raster reference, rotated by the raster
  angle in the layer plane. A +/-45 degree raster is usually transversely isotropic: E1 = E2 and G12 = E1/(2(1 + ν12)).
- **Poisson ratios.** They are major ratios, ν_ij = −ε_j/ε_i under stress along i. The compliance must be positive
  definite.
- **Sources.** They are one of measured, datasheet, literature, calibrated, user or assumed. Assessments repeat them,
  and assumed data raise warnings.
- **Canonical form.** Stored values are SI strings with units, and records are replaced by id.
- **In an assessment.** `mech_fem_assess` with `material_model {type: orthotropic, material, build_direction,
  raster_reference, raster_angle, criterion}` gives the directions in the build frame of the placed part. The run
  directory receives `mech_structure.bin` (mesh, displacements, Gauss-point stresses in global and material axes,
  strains, failure indices, axes) and `summary.json`.
