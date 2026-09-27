# The measurement kit: print this, measure it, and your machine joins the board

The one contribution no model can make is a measured print. This kit is the standard way to make one. It is the
specimen and procedure of a published open-access study, so a result from your machine is directly comparable
with every other entry. Up: [challenges/README.md](../challenges/README.md) · submit: [validation/contrib](../validation/contrib/README.md).

## What you print

The calibration cantilever of Gersch, Schulz and Bagdahn 2025 (TMS 2025 Supplemental Proceedings, pp. 170-181,
doi:10.1007/978-3-031-80748-0_15, open access under CC BY 4.0): a comb 70 x 10 x 9 mm, webs and gaps 1 mm, a 3 mm beam
on top, a 10 mm solid block at the fixed end. The STL is `samples/cantilever.stl`; its source line is in
`samples/samples.json`.

Print **at least one along the machine X axis and one along Y** (the beam's long side along the axis), on the plate
without extra supports (the teeth are the supports). More specimens per orientation give a scatter value, which is
worth more than the mean.

## What you record (all of it goes into the campaign file)

Machine and laser configuration; material, supplier and powder size; layer thickness; laser power, scan speed, hatch
spacing and scan strategy per specimen; shielding gas; plate preheating; build position on the plate; any parameter
the machine changed by itself. The template lists every field: [measurement_template.json](../validation/contrib/measurement_template.json).

## How you measure

1. Before separation, with the part still on the plate: measure the top surface height of the free end relative to the
   plate (a coordinate measuring machine with a small ball tip, a dial indicator on a surface plate, or a structured
   light scanner; record the instrument and its resolution).
2. Separate the specimen from the plate **through the comb**, by wire erosion or a saw, and record the height above
   the plate at which you cut and the kerf. Keep the time between cutting and measuring short (the source warns that
   creep changes the deflection over days).
3. Measure the free-end height again, same reference. The reported quantity is the difference: the tip rise after
   the cut, in millimetres, positive upward.
4. Photograph the specimens before and after, and the measurement set-up.

## What you get

Your campaign becomes a calibration profile (material, machine, strains, your name, the date) that anyone printing
on the same machine and material can use, and a row on the leaderboard as a data source. If your set is large enough
it becomes its own challenge with held-out values, and every solver on the board has to predict your machine.

## Cost

One build plate hour and one hour of measurement. If you have access to a printer and cannot justify the material,
open an issue: coordinating campaigns so that several labs print the same day is part of the plan.
