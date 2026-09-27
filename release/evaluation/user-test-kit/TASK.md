# Task: two mounting plates

You are choosing between two versions of an aluminium mounting plate. Use NAVIER to answer the questions at the end.
You may write the study definition by hand, following `doc/WORKFLOW.md`, or ask your own AI client to drive NAVIER
(`doc/AI_CLIENTS.md`). Write down which you did.

## The parts

Both plates were exported from CAD in **millimetres**, with z pointing up. They are in `task_geometry/`:

| File | Description |
|---|---|
| `plate_uniform.stl` | 120 mm long (x), 30 mm wide (y), 6 mm thick |
| `plate_stepped.stl` | 120 × 30 mm; 8 mm thick for the first 60 mm (x 0–60), 4 mm thick for the rest (x 60–120); the step is on the top side |

The two plates have the same volume. The underside of both is the plane z = 0.

## How they are used

- **Mounting:** the underside of the first 20 mm (x 0–20 mm, the full width) is bolted flat to a rigid steel base.
  Treat that patch as rigidly held.
- **Load:** a 2 kg instrument hangs under the free end. It is attached over the underside of the last 20 mm
  (x 100–120 mm, the full width) and pulls straight down.
- **Material:** aluminium 6082-T6, machined from plate. The supplier datasheet gives Young's modulus 70 GPa,
  Poisson's ratio 0.33 and density 2700 kg/m³.

## Questions

1. Which plate deflects less where the instrument hangs, under these conditions?
2. How do the two plates compare in mass?
3. According to NAVIER's record, how reliable is that comparison? Say what the record reports about mesh refinement,
   and anything it says is not evaluated.
4. What information, if it changed, could change your answer?

Write your answer in your own words: half a page is enough. Attach the study's `report.md`.

## Time

Plan for up to 90 minutes. Record on the feedback form when you started and finished each step. Stopping is an
acceptable result: record where and why.
