# Known limitations

## Physics

- **Static, small-strain, linear elasticity with isotropic materials.** Nothing is modelled of plasticity, creep,
  contact, friction, bolt preload, geometric nonlinearity or buckling, dynamic loads, fatigue, temperature effects or
  residual stresses.
- **No strength or safety assessment.** Stress values are supporting information. Peaks at ideal supports and sharp
  inside corners are singular (they grow with refinement) and are labelled so. No failure criterion is applied.
- **Large deformation** is detected, not modelled. A result whose displacement exceeds 5 % of the model size, or
  whose rotation linearisation error exceeds 1e-2, is outside the assumption, and the comparison is not established.
  Between 1 % and 5 % (or 1e-3 and 1e-2) the result is labelled questionable. The thresholds are stated heuristics:
  the rotation criterion follows from the neglected θ²/2 term; the displacement ratio is a common engineering rule of
  thumb, not derived.
- **Supports** are ideal: fixed (all components) or frictionless on flat faces normal to x, y or z.

## Discretisation

- **Voxel hexahedral meshes.** The boundary is a staircase, so inclined and curved faces are approximated, and their
  volume error changes with the element size (reported per level). Features thinner than about two elements are
  poorly resolved, and a mesh that loses a connection, or whose volume differs by more than 10 %, is marked invalid.
  There is no conforming mesher.
- **Discretisation-error estimates.**
  - A grid convergence index is offered only when three or more valid meshes have a constant ratio of at least 1.3,
    the change is monotone, the observed order is in [0.5, 4], and the geometry is represented identically (voxel
    volume equal to the STL volume).
  - The safety factor is 3, or 1.25 when four meshes give consistent orders.
  - A staircase boundary (chamfer, slope, curve) never gets an estimate, because its shape changes with the mesh;
    such comparisons report the ranking on the tested meshes and the convergence criterion instead.
  - Estimates concern the model's discretisation, not its error against reality, and they are not bounds.
- **Resources.** The direct solver is used up to 250,000 equations; the iterative solver beyond. On an 8 GB machine,
  about 400,000 elements per analysis is the practical limit (`limits.max_elements`).

## Workflow

- **Two to four designs per study, one material per design, one load case, one payload region.** No load
  combinations, no multiple mounting regions per design beyond the declared alternatives, and no remote point loads
  or moments.
- **Region queries** are geometric (planes, boxes, facing directions, patches). Picks on rendered views cannot define
  a study region, because they are not reproducible.
- **Equivalence between designs** is checked on lever arm, areas and normals of the regions, within stated
  tolerances. It cannot detect every way two designs might be held differently.
- **The ambiguity check** flags regions that span parallel planes. It does not understand intent.
- **Keyword detection** of strength or safety requests is a heuristic that only adds a notice. It never changes the
  computation.
- **Replays** are bitwise reproducible on the same build and machine. Other compilers, CPUs or thread counts may
  change the last digits: the replay record compares with a relative tolerance of 1e-9 and reports bitwise identity
  separately.

## Evaluation status

- **Scripted MCP client:** the reference and evaluation cases in `REFERENCE_CASES.md` run automatically.
- **Real AI client:** evaluated on 2026-09-17 with Claude Code (model `claude-opus-4-8[1m]`), six cases with three
  repeats and only the NAVIER tools. **It is not dependable yet:**
  - The model ran studies competently and passed on failed checks.
  - It answered questions meant for the user: it inferred missing units, chose between ambiguous faces, and accepted
    open questions with its own reason.
  - It reported numbers when the comparison could not be established.
  - Once, it called a part safe from its own stress runs.

  Read an AI client's answer against `evidence.json`, and check `assumptions` and `accepted_questions` for anything
  the client, not you, decided. Codex is not installed and was not tested. Details are in the source tree
  (`release/evaluation/ai-client/RESULTS.md`).
- **External users:** none. No usability or user-testing claim is made.
- **Independent solver:** CalculiX 2.23 reproduced NAVIER's assembled problems (cantilever, brackets A and B) to about
  1e-7, and an independently built cantilever to 0.0015 %. The comparison covers these cases and meshes only.
  CalculiX's direct solver crashed on the largest bracket deck; its iterative solver was used for that one case (see
  `EVIDENCE.md`).
- **Validation against measurements:** none. A physical test protocol with a measurement template and analysis
  scripts is prepared in the source tree (`release/evaluation/physical/`); no measurement exists.

## Platform

The build was made and tested only on macOS arm64 (M2, 8 GB, macOS 15.7.3). See `INSTALL.md`.
