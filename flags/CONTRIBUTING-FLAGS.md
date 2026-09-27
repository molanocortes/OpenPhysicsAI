# Proposing a flag

A flag is only as good as the experiment behind it. Up: [README.md](README.md)

## What qualifies

- **A measurement of the physical world**, not the output of another simulation (a code-to-code comparison belongs
  in [challenges/](../challenges/README.md) as a reference board, not here).
- **Controlled conditions that are stated**: geometry, material or gas, boundary conditions, the quantities held
  fixed, and how the measured quantity was obtained. If a simulator would have to guess a condition, the flag is not
  ready.
- **A stated uncertainty**, or enough information to estimate one (instrument resolution, repeatability, scatter
  between repeats). The scoring uncertainty is never below 2 per cent.
- **Published and citable**, so the sealed file can name its source; a measurement of your own is welcome with its
  raw data and set-up ([validation/contrib/](../validation/contrib/README.md)).
- **Several cases** where the experiment offers them, one of them public as a practice case.

## What to write

1. `flags/<id>/flag.json`: id, title, the question, the conditions, the outputs with units, the hidden cases, the
   practice cases with their answers, the metric (`log_ratio` for positive quantities, `absolute` for quantities that
   change sign), and its place: a proposed flag enters as a **trial** (`"tier": "trial"`, `"leads_to"`: the flag
   whose stone it tests, a `label` such as `T5`) or, by the owner's decision, as one of the numbered flags (`number`,
   `tier` of `flag`, `semi-holy-grail` or `holy-grail`, `stone`, `difficulty` from 1 to 5 or 6 for a holy grail,
   `subtitle`, `short`, `status`). A trial does not name its source in the public files; the thirteen are famous
   experiments and name theirs.
2. `flags/<id>/FLAG.md`: the same for people: the flag, why it is worth capturing, why it is hard, what it takes, and
   the markers `<!-- board:begin -->` and `<!-- board:end -->` where `tools/flags.py board` draws its leaderboard.
3. A draft of the sealed file, sent to a maintainer privately, never committed: `{"id", "source": {"citation", "url",
   "where", "uncertainty"}, "answers": {case: {output: [value, uncertainty]}}}`. The uncertainty is relative for
   `log_ratio` and absolute for `absolute`.
4. The maintainer runs `python3 tools/flags.py seal DRAFT.json --remove-draft`, which stores it, adds a salt and
   writes the SHA-256 commitment into flag.json.
5. Optionally, a reference entry: how this repository's solvers answer it (`flags/<id>/reference_entry.py`), built
   from the conditions only.

## Retiring a flag

A flag is retired when its source becomes too easy to look up for the question as posed, or when a better
measurement replaces it. Its sealed file is then published next to it, the commitment can be checked by anyone, and
the flag moves to a public benchmark.
