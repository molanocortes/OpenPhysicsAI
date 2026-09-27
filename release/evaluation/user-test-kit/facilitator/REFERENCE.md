# Facilitator reference (do not hand to participants)

## The developer's reference set-up

`reference_study.json` shows one correct definition. Other definitions can also be correct: for example, a
`patches` or `plane` query combined with a box, with or without sensitivities.

**Result with the rc2 development build (2026-09-17, M2 8 GB, 261 s including sensitivities):**
`reference_comparison.json` and `reference_report.md`.

| | Uniform U | Stepped S |
|---|---|---|
| Region areas (mounting / load) | 600 / 600 mm² | 600 / 600 mm² |
| Lever arm (mounting centroid to load centroid) | 100 mm | 100 mm |
| Load-region displacement at 2 / 1 / 0.5 mm | 0.13140 / 0.13254 / 0.13311 mm | 0.12455 / 0.12599 / 0.12670 mm |
| Last change (relative to the finer mesh) | +0.43 %, criterion met | +0.56 %, criterion met |
| Discretisation-error estimate (safety factor 3) | 0.0017 mm (1.3 %) | 0.0020 mm (1.6 %) |
| Mass (geometry) | 0.05832 kg | 0.05832 kg |

**Outcome: `resolved`.** S deflects 4.8 % less than U at 0.5 mm. The difference (0.0064 mm) exceeds the sum of the
estimates (0.0037 mm), and S ranked first on all three meshes. The masses are equal.

## Known pitfall: region boxes and triangle centroids

A box query in `triangle` mode selects the triangles whose **centroid** lies in the box. On these plates' 2 mm
triangulation, a box that is too generous also takes neighbouring triangles. The developer's own first two reference
set-ups were wrong in exactly this way. The areas reported by `study check` and in the report exposed both errors.

| Set-up | Region areas | Outcome |
|---|---|---|
| box only, z within ±1 mm (`first_reference_run_box_only.json`) | 700 / 704 mm²: strips of the end and side faces included | `ranking_consistent_on_tested_meshes`, S 2.9–5.2 % lower; U's refinement not monotone, so no estimate |
| box and `facing -z`, x from −1 to 21 mm (`second_reference_run_margin.json`) | 630 / 630 mm²: one extra row of underside triangles | `ranking_consistent_on_tested_meshes`, S 3.4–5.4 % lower; U not monotone |
| box and `facing -z`, x from −0.5 to 20.5 mm (reference) | 600 / 600 mm² | `resolved`, S 4.8 % lower |

The ranking was the same in all three set-ups. The size of the difference and the refinement reading were not.

## Grading checklist

Record for each participant:

| Item | Observed |
|---|---|
| Installed and ran `doctor` without help | yes / with help / no |
| Ran the packaged example | yes / no |
| Units stated as mm (not left to a default) | yes / no |
| Material values as given (70 GPa, 0.33, 2700 kg/m³) | yes / partly / no |
| Mounting region: underside x 0–20 mm only (check area 600 mm² in the report) | yes / area ___ mm² |
| Load region: underside x 100–120 mm only (area 600 mm²), 2 kg downwards | yes / area ___ mm² / wrong direction |
| Looked at previews or region areas to confirm the set-up | yes / no / unknown |
| Element sizes chosen, and whether the check warned about thin walls | ___ |
| Answer 1: S deflects less under the modeled conditions | yes / no / not answered |
| Answer 2: equal mass | yes / no |
| Answer 3: refinement reported (outcome, convergence criterion, estimates or their absence) | complete / partial / missing |
| Answer 3: exclusions reported (no strength or safety assessment, ideal rigid base, uniform load) | yes / partly / no |
| Answer 4: sensible factors named (bolt and base stiffness, load location, material batch, fillets not modelled) | yes / partly / no |
| Unsupported claims (for example "safe", "strong enough", a precise percentage presented as certain) | count ___ |
| Interventions by the facilitator | count ___, what |
| Total time; where the participant stopped, if they did | ___ |

A participant's set-up that differs from the reference is not wrong in itself. Judge it against the task text. A
different outcome is expected when the regions differ, as the table above shows.
