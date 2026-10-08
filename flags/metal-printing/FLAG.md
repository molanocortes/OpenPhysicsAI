# Flag 05 · Printing metal

<img src="card.svg" alt="Flag 05, printing metal: flag, stone phase change, difficulty ★★★★☆, in preparation" width="100%">

**Flag** · stone: **phase change** · difficulty **★★★★☆** · in preparation · [the thirteen flags](../../README.md#the-thirteen-flags) · [leaderboard](../LEADERBOARD.md) · [how the flags work](../README.md)

> A laser draws a line on a metal plate. Predict the pool it melts and the crystals it leaves behind.

<img src="../../docs/media/lab/melt-pool-flow.gif" alt="A laser crosses a steel plate; the melt pool, stirred by its surface, grows wide and shallow" width="100%">

<sub>The lab today: a laser track in 316L with Marangoni flow in the melt; the pool grows wider and shallower than conduction alone makes it. The keyhole and the crystals are what this flag adds.</sub>

## The flag

**Open since 8 October 2026** · **cases:** 11 hidden, 2 practice · **asked:** the melt pool's width and depth, in micrometres · **definition:** [flag.json](flag.json) · **rules:** [flags/README.md](../README.md)

The US National Institute of Standards and Technology runs AM-Bench, benchmark measurements for additive manufacturing. In its single-track experiments a laser scans lines across bare plates of nickel superalloy at stated power, speed and spot size. NIST measured the melt pool's width and depth in cross sections, its cooling with high-speed thermal cameras, and the solidified structure it left, and it collected predictions before releasing the measurements. The flag asks for the melt pool's width and depth on eleven tracks: four on IN625 plates on two machines (AMB2018-02) and seven on IN718 (AMB2022-03), from 137.9 to 325 W, 400 to 1200 mm/s and spots of 49 to 170 µm. The plates, the beam, the atmosphere and how NIST cut and measured the sections are stated in [flag.json](flag.json). Two practice tracks with public answers, AM-Bench's released calibration case on each machine of 2018, are there to calibrate and to check against: 133 µm wide and 91 µm deep on the CBM, 123 µm wide and 36 µm deep on the AMMT.

The cooling rate at solidification and the spacing of the solidification cells, which this flag also names, are not asked yet: their measurements are not yet curated into the same form, and NIST calls its 2018 cooling rates exemplar values, not to be used to calibrate or check models. They will join as a second sealed set, announced on this page.

## Why it is worth capturing

Printed metal flies in rocket engines and jet turbines and lives in hip implants, and its strength is decided in a pool of liquid metal a tenth of a millimetre wide that lives for a millisecond. A simulator that predicts the pool and the metal it leaves replaces months of trial prints and makes printed parts certifiable. This is where this project began, predicting how printed parts distort, and it is where heat, flow, surface tension and the growth of crystals meet.

## Why it is hard

The pool is stirred by its own surface tension at metres per second, may be dug into a keyhole by the recoil of evaporating metal, and solidifies at up to a million degrees per second, growing cells a micrometre apart. The scales run from micrometres to millimetres and from microseconds to milliseconds, and the properties of the liquid alloy, two and a half thousand degrees hot, are themselves hard to measure.

## What it takes, and where to start

- Heat with melting and solidification, flow in the melt with the Marangoni stress, evaporation and a free surface that can deform into a keyhole, the laser's absorption, then a model of the microstructure (phase field or cellular automaton) fed by the thermal history.
- In this repository: the melt pool solver with the enthalpy method and Marangoni flow (`src/lab/melt`, verified in `tools/melttest.c` against Rosenthal, Neumann and the thermocapillary return flow), and the distortion of printed parts, validated against measured cantilevers. The reference entry, [reference_entry.py](reference_entry.py), runs that solver in conduction on every track with sourced IN625 and IN718 properties ([reference_materials.json](reference_materials.json)) and one absorptivity fitted on a practice track; it is the method that predicted these tracks blind on 8 October 2026, before NIST's values were read and before this flag opened. Missing: the keyhole, the liquid alloys' measured properties, the crystals.
- Giants to stand on: MOOSE, PRISMS-PF (phase field).
- The [physics map](../../docs/map/PHYSICS.md) says, for every solver in this repository, what it computes, how it was verified and which files to read first.

## The measurement

The measured values come from a published experiment with stated conditions. The experiment is published, and anyone determined can find its numbers; what keeps the flag fair is that an entry is a simulator, rerun from its commit by a machine and read before it is merged, so code that looks the answer up is disqualified. The sealed file holds NIST's mean width and depth of each track and their combined standard uncertainty (k = 1), which is each case's scoring uncertainty (never below 2 per cent). It is kept out of this repository; its SHA-256 commitment, published in [flag.json](flag.json) on 8 October 2026 before any entry was scored, is

`e941cb95e71f12db95c3d00869599cb72a065a6fdae6c3249dc578e1138724aa`

so the answers cannot be changed afterwards, and anyone can check it when the flag is retired. Scores are published in bands of five points.

## Leaderboard

<!-- board:begin -->

**Attempts** 1 · **Challengers** 1 · **Record** 10 by @molanocortes · **First capture** still to be won

| Rank | Challenger | Score | Date |
| :---: | :--- | ---: | :--- |
| 🥇 | **[@molanocortes](https://github.com/molanocortes)**<br><sub>OpenPhysicsAI, using Claude Opus 5.5</sub> | **10** | 2026-10-08 |

<!-- board:end -->

## Capture it

Fork this repository or bring your own open-source simulator, build on any open-source library, work with any AI model: the board credits the person who captures the flag, the libraries and the model. Download the lab, make it better with your own AI agent, describe your entry in `flags/entries/<your GitHub name>/entry.json` and open a pull request: a machine reruns it from your commit, scores it against the sealed measurements and posts the score on your pull request, and a capture is merged into the lab for everyone once its code has been read ([how to take part](../../README.md#capture-a-flag)). The rules and the scoring: [how the flags work](../README.md). No prize and no money: the reward is your name on this board, and the first capture is kept for good.
