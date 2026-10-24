# Real breaks: print these parts, break them with a bucket of water

This is the kit of GOALS.md G17 step 5. You print a handful of plastic parts, hang a bucket from each one, add water
until it breaks, and write down the weight and where it broke. `tools/breaks.py` then solves every part as you printed
it and compares the model's answer ("holds about R times the load") with what really happened. The step passes when, over
at least 10 breaks, the median error of the predicted load is at most 25 % and the predicted place of the break matches
the real one in at least 8 of 10.

Your records and photos stay private: by default in `~/.openphysicsai/sealed/breaks/`, outside the repository. Nothing
from them is published without your approval.

## What you need

- A printer with PLA, or another filament whose library record has values along and across the layers
  (`materials_list`; PLA is `pla_fff`). Part of one roll is enough.
- A bucket of 15 to 20 litres with a strong handle (1 litre of water weighs 1 kg), a jug, and a luggage scale reading to
  10 g up to at least 25 kg, with a peak-hold function if it has one.
- Cord or rope rated well over 30 kg, two carabiners or S-hooks, and something firm to hang from (a pull-up bar, a
  beam, a ladder laid across two chairs).
- For the bracket and the beam: a table edge, a block of wood, a C-clamp, and a small spacer that sets how much of the
  part the clamp holds (25 mm for the bracket, 20 mm for the beam).
- Safety glasses. Printed plastic breaks into sharp pieces.

## What you print

The STL files are in `parts/`, already placed for the "flat" or "side" print. Print the bar, the hook and the bracket
**solid (100 % infill)** with 2 perimeters, 4 top and 4 bottom layers, and 0.2 mm layers. Print the beam three times
with **gyroid** infill at 20, 50 and 100 %, same walls. Other infill patterns are not modelled yet.

| Part | File | Prints | How to place it | Model's first-failure load (PLA) |
|---|---|---|---|---|
| Tensile bar, 1.6 x 1.6 mm gauge | `bar.stl` | 2 flat, 2 upright | flat as it is; upright: standing on one head, with a 5 mm brim | about 13 kg flat, 10 kg upright |
| J-hook | `hook.stl` | 2 flat | flat as it is | about 10 kg |
| L bracket | `bracket.stl` | 2 on its side, 2 standing | side: as it is; standing: back face down on the bed, arm pointing up | about 7 kg on its side, 5 kg standing |
| Cantilever beam | `beam.stl` | 20, 50 and 100 % gyroid, one each | flat as it is | about 3, 6 and 9 kg |
| Slotted plate (the rig, not a specimen) | `slot_plate.stl` | 2, solid | flat as it is | |

These loads are the model's **first failure**, the load at which the most stressed point first reaches the strength in
the material's data sheet. A blind comparison with published tests (GOALS.md G17 step 5's note) found real bending
breaks at about 1.7 times that, so a bracket or a beam can take more than the table says. Have enough water ready.
On quadratic tetrahedra a curved surface used to read low where it met a flat face (step 4's record: 12 % on a ring);
since the mesher keeps sharp edges (GOALS.md G20 step 5) the ring reads 1 % low and the hook's load rose from 9.5 to
10.0 kg (`predictions-2026-10-08.txt` before, `predictions-2026-10-08-sharp-edges.txt` after). Every corner where a part is meant to break has a real radius (3 mm on the
hook, the bracket and the beam), so the answer reads it rather than flagging a sharp corner. Run
`python3 tools/breaks.py predict` for the current numbers.

## How to hang each part

- **Bar and hook.** Hang one slotted plate level from your support by two cords through its end notches. Slide the
  part's neck into the slot (10 mm for the bar, 15 mm for the hook) so that its head rests on top of the plate.
  - Bar: slide the second plate onto the bar's lower neck, under the lower head, and hang the bucket from that plate's
    end notches.
  - Hook: hang the bucket's handle at the lowest point of the hook's ring.
- **Bracket.** Lay the bracket's back plate against the wood block, arm horizontal and pointing away. Clamp only the top
  25 mm of the back plate, with the 25 mm spacer under the clamp's pad. Loop the cord over the arm against the lip and
  hang the bucket from it.
- **Beam.** Put the block on the table with the beam sticking out over the edge. Clamp the first 20 mm of the block
  (spacer under the pad). Loop the cord over the beam against the lip and hang the bucket from it.

## How to load it

1. Weigh everything that will hang below the part (bucket, cord, carabiner, for the bar also the lower plate). Hang it.
2. Put a box with a towel under the bucket, about 3 cm below it, so that it drops a short way and nothing spills.
3. Add water steadily: about a litre every 15 seconds at first, then smaller amounts once the part visibly bends. Aim for
   a break within 2 to 5 minutes of the first water. Plastic creeps, and a slower test breaks lower.
4. If the part whitens, creaks or stays bent before it breaks, note the water level then (it gives `first_damage_kg`).
5. When it breaks, weigh the bucket with its water and everything that hung below the part. That total is
   `break.mass_kg`.
6. Look at where it broke and take two photos, one of each broken face.

## What you record

One file per break, a copy of [record_template.json](record_template.json) saved as
`~/.openphysicsai/sealed/breaks/<date>-<part>-<print>-<n>.json`:

- the part and the way it was printed (`print`: one of the part's prints above);
- the printer, slicer, filament (brand, material, colour) and the library record that matches it (`pla_fff` for PLA);
- the slicer settings: layer height, line width, perimeters, top and bottom layers, infill density and pattern;
- the printed mass in grams (a check on the infill), and the dimension named in the part's file measured with calipers;
- the break: the mass, how it was weighed, the first-damage mass if you saw one, the time from the first water to the
  break, and the place, as one of the part's regions:

| Part | Regions |
|---|---|
| bar | gauge (the narrow middle and the first 2 mm of each shoulder), shoulder, head |
| hook | bend (the ring, and where it meets the shank), shank, head |
| bracket | corner, arm, back plate |
| beam | root, span, block |

## Comparing

    python3 tools/breaks.py compare                        # every record in ~/.openphysicsai/sealed/breaks
    python3 tools/breaks.py compare path/to/folder         # or another folder

For each record the tool sets up the part with the record's material, orientation, walls and infill. It solves it,
and prints the predicted and measured loads, the error (negative when the model is on the safe side) and both break
places. It writes `score.json` beside the records. A record the model cannot judge is listed with the reason: an
infill pattern other than gyroid, infill below 10 %, or a missing break mass. `python3 tools/breaks.py selftest` checks
the comparison itself.

## Why these parts

- The **bars** test the material data: a gauge in plain tension breaks at its section times the strength, along the
  layers flat and across them upright.
- The **hook** and the **bracket** are the everyday case: bending at a curve and at a filleted corner. The bracket's
  two prints put the corner's stress along the layers or across them.
- The **beam** at three infill levels tests the walls-and-infill model of step 3.

None of the parts needs a drilled hole. The places a person holds or pulls (heads, clamped faces, the cord's patch) are
faces of the part, and the analysis applies its supports and loads to exactly those faces (`breaks.py`, `scenarios()`).
