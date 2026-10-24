# Contribute a measurement

A measured print is the one thing no model can produce. This folder is where measurements from anyone enter the
project and become calibration profiles and challenges. Up: [challenges/README.md](../../challenges/README.md).

## What to submit

One folder per campaign: `validation/contrib/<year>-<lab-or-name>-<short>/` containing

- `measurement.json` (copy [measurement_template.json](measurement_template.json)): the machine, material, powder,
  process parameters, the specimen geometry (by reference to a geometry file in this repository, or a new one with
  every dimension), the orientation, how the specimen was separated from the plate (tool, height), the instrument and
  its resolution, and the measured values with the number of specimens behind each;
- photographs of the specimens before and after separation, and of the measurement set-up;
- optionally the raw instrument output.

Nothing is required that you do not have, but every field you leave empty is a field the project cannot claim. A
value without a source is not usable: "E = 70 GPa" needs "from the supplier's data sheet, <name>" or "measured by
ultrasound on printed coupons, n = 7".

## What happens with it

1. A maintainer checks the file against the template and opens a pull request if you did not.
2. The reference solver is calibrated on part of the values and predicts the rest, and the result goes on the
   leaderboard (`make leaderboard`) with your campaign named as the data source.
3. A calibration profile (material, machine, strains, provenance) is added to the profile registry so that others
   printing on the same machine and material can use it. The profile carries your name and the date.
4. If the campaign is large enough, it becomes a challenge with held-out values.

## Rights

By submitting you confirm you may publish the data. It is published under CC BY 4.0 (`LICENSE-DATA` at the repository
root), with your campaign credited. Personal data is never accepted.
