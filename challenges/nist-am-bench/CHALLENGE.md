# Challenge: NIST AM-Bench (being imported)

Up: [challenges/README.md](../README.md). State: **defined, no data imported yet.** Task: [TASKS.md](../../TASKS.md) T11.

NIST's Additive Manufacturing Benchmark Test Series (AM-Bench, 2018 and 2022 rounds) printed and measured parts on
documented machines and published the measurements for blind prediction: bridge-shaped parts in nickel alloy with
measured distortion after partial cutting and residual strain, plus single-track and microstructure measurements.
It is the closest existing thing to this project's challenges, and its data is the natural second and third challenge.

## What the import must do (and must not)

1. Read the terms of use of the AM-Bench data on NIST's site and record them here with the access date. Import only
   what the terms allow to be redistributed; otherwise store the fetch instructions and the file hashes, never the data.
2. For each measured quantity that our solver can predict today (distortion after the cut, residual strain where a
   comparable quantity exists), write a `challenge.json` in this folder with the geometry (from NIST's published CAD,
   with its source), the process record, the measurements with their stated uncertainty (AM-Bench states scatter),
   the calibration split and the scoring rule, pre-registered before any run.
3. Generate the geometry for the solver from the published dimensions, in a parameter file with every dimension and
   its source.
4. Run the reference entry and CalculiX where feasible, write the result files, and let `make leaderboard` score them.

Nothing from memory: every number comes from a NIST document opened during the import, with its URL and access date.
