# Overnight queue: long runs that are not worth a session's waiting time

Rule 8 of [AGENTS.md](../AGENTS.md): no run over ten minutes inside a working session. Runs that belong here are
launched by the owner when the machine is free, one at a time, with the command below, and their outcome is written
back into the row. A run is added only when the model it exercises has passed its cheap verification cases; the
column "answers" says which single number or figure the run exists for.

## How the queue is run

`nohup python3 tools/run_queue.py validation/queue.jobs --hours 10 > build/overnight/runner.log 2>&1 &` runs the jobs
of [queue.jobs](queue.jobs) one after the other without anyone watching: each in its own process group with its own
log and its own timeout (a job that hangs is killed with its children and the queue goes on), the laptop kept awake
while it runs, a job skipped when it no longer fits before the deadline or when the disk is nearly full.
`build/overnight/SUMMARY.md` is rewritten after every job; `touch build/overnight/STOP` ends the queue after the
current job. A row below becomes a line of `queue.jobs` once its command exists; studies run with `--no-write`, so the
queue never makes a leaderboard entry by itself. First run: the night of 2026-09-20, eleven jobs, about nine and a
half hours expected.

| Added | Run | Command | Expected time | Answers | Outcome |
|---|---|---|---|---|---|
| 2026-09-20 | Hertz on a mesh graded at the pole (D12 of docs/contracts/dynamics.md) | a ball mesh refined toward the contact pole (four levels, target element at the pole about a fifth of the Hertz patch radius, roughly 25 000 elements), pressed to 1.5 mm at penalty scale 50, run as a new case of `tools/dyntest.c` behind `D12_FINE=1` | about 40 min on this laptop (the 1 728 element ball took 89 s and the step falls with the smallest element) | whether the contact radius and the peak pressure meet Hertz to 5 per cent once the patch is several elements across, which is what D12 asks and no uniform mesh here can answer | not run: the case behind `D12_FINE=1` is not in `tools/dyntest.c` yet |
| 2026-09-27 | The methane fire's plume averaged over two minutes instead of 30 s | `fire_plume_long_average` in `queue.jobs` (`FIRE_T_END=128 ./build/firetest`) | about 80 min (the 38 s test takes about 25) | whether the plume's shift since the room engine (rise at 2.0 m -18 % to -40 %, velocity -20 % to -56 %, against +15 % to +37 % before) is real or an unconverged average: the block scatter falls with the longer average, and V2 and V3 are read again ([docs/lab/fire.md](../docs/lab/fire.md)) | not run yet |

## The first night, 2026-09-20 (commit 16e5ba9, 7 h 40 min, logs in `build/overnight/`)

Eleven jobs, ten finished, one killed at its timeout. The rows and findings of that night that used the LPBF
validation data were removed with that data on 2026-09-27 (docs/map/HISTORY.md). On an idle machine: `make test-fast` and `make test` green,
`make test-ui` 133 + 53 + 11 + 15 with no failure, the Simple, build, structural and tetrahedral walkthroughs green.
The stop walkthrough failed 4 of 13 that night and the
cause was a defect in the application, fixed the next morning (a simulation that was still ending was adopted again
every frame after "start over").
