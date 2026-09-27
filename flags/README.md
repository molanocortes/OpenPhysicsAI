# How the flags work

Up: [the front page: the thirteen flags and their boards](../README.md) · [the whole leaderboard](LEADERBOARD.md) ·
[proposing a flag](CONTRIBUTING-FLAGS.md) · [AGENTS.md](../AGENTS.md)

A **flag** is a measurement of the physical world, taken in a controlled experiment, that a simulator must predict from
first principles. Thirteen flags, each from a different branch of physics, stand around this laboratory; four smaller
trials lead to them. A challenger captures a flag by predicting its measurements within their uncertainty with a
simulator anyone can rerun and read. Anyone can take part: download the lab, make it better with any AI agent, open a
pull request, and a machine scores it; what wins is merged into the lab for everyone. There is no prize and no money: the reward is a name on the board, and the first
capture of a flag is kept in its history for good.

## The thirteen

| # | Flag | Stone | Tier |
|---|---|---|---|
| 01 | [The wake of a car](car-wake/FLAG.md) | turbulence | flag |
| 02 | [Re-entry from space](reentry-fire-ii/FLAG.md) | hypersonics | flag |
| 03 | [A turbulent jet flame](jet-flame/FLAG.md) | combustion | flag |
| 04 | [A drop that splashes, or doesn't](drop-splash/FLAG.md) | interfaces | flag |
| 05 | [Printing metal](metal-printing/FLAG.md) | phase change | flag |
| 06 | [A metal part tearing apart](metal-fracture/FLAG.md) | solids | flag |
| 07 | [The radar signature of a stealth shape](radar-almond/FLAG.md) | electromagnetic waves | flag |
| 08 | [A spark in air](spark-streamer/FLAG.md) | electric fields and plasma | flag |
| 09 | [Apophis passes the Earth](apophis-2029/FLAG.md) | gravity | semi holy grail |
| 10 | [The Sun's corona at the eclipse of 2027](corona-2027/FLAG.md) | magnetic fields of a star | semi holy grail |
| 11 | [A star in a bottle](fusion-shot/FLAG.md) | energy | holy grail |
| 12 | [The storm that surprised everyone](hurricane-otis/FLAG.md) | the atmosphere and the ocean | holy grail |
| 13 | [A human heartbeat](heartbeat/FLAG.md) | life | holy grail |

Trials: [vortex shedding behind a cylinder](flow-cylinder-shedding/FLAG.md) (to 01), [the bow shock ahead of a
sphere](gas-sphere-bowshock/FLAG.md) (to 02), [a shock through a bubble of gas](gas-shock-bubble/FLAG.md) (to 04),
[the planets ten and twenty years ahead](orbit-planets-2020/FLAG.md) (to 09). They were answered first with the lab's
two-dimensional solvers and are being rebuilt natively in 3D.

**Tiers.** A **flag** is a hard problem of today: the best methods, pushed hard, can capture it. A **semi holy grail** has
an answer that does not exist yet; nature reveals it on a known date, and only predictions registered before it count.
A **holy grail** is beyond the reach of any simulator today; it may be within reach in about ten years, and capturing
it would change the world.

**Difficulty** is our estimate, from one star to five; the holy grails are marked with the infinity sign.

**Status.** A flag is *in preparation* until its cases, the outputs asked and their units are written into its
`flag.json` and its answers are sealed; it is then *open*, and *captured* once a challenger scores 90 or more. A semi
holy grail is scored after its event.

## The credit

Every row on a board names a person, not a program: the **challenger** (the GitHub name that submitted the entry), the
open-source **libraries** the entry is built on (the first is its main one) and the **AI model** that helped, as the
entry declares them. Baselines (textbook formulas, for instance) are shown for reference and are not ranked.

The **hall of fame** ranks challengers across the thirteen: first by flags captured, then by **power**, the sum of each
flag's best score weighted by its tier (once for a flag, twice for a semi holy grail, three times for a holy grail), out
of 2100. **Attempts** count scored submissions: one entry's predictions at one commit are one attempt, however often they
are re-scored.

## An entry

An entry is a directory `flags/entries/<name>/` with an `entry.json`:

```json
{
 "title": "a short name for the simulator",
 "who": "your GitHub name",
 "ai": "the AI model that helped, or \"none\"",
 "libraries": ["the open-source libraries it is built on, the main one first"],
 "open": true,
 "baseline": false,
 "description": "what it is, in a few sentences",
 "commands": {"<flag id>": "a command that prints one JSON line of predictions for that flag"}
}
```

`python3 tools/flags.py run --entry <name>` runs every command with the flag's cases in the environment (`FLAG_ID`,
`FLAG_CASES`, `FLAG_DIR`) and writes `predictions.json`, printing how close you are on the practice cases. The reference
entry of this repository shows the form: [reference/entry.json](entries/reference/entry.json).

## Scored by a machine

A pull request that adds or changes an entry is scored with no one in the loop, in two steps:

1. **The rerun** ([flag-entry.yml](../.github/workflows/flag-entry.yml)) builds the lab on a clean machine and reruns
   the entry from the pull request's commit, with the flags' files and `tools/flags.py` as they are on main, so a
   question cannot be changed by the answer to it. It has no secret and cannot see an answer. The lab's whole test
   suite runs on every pull request beside it ([linux.yml](../.github/workflows/linux.yml)).
2. **The score** ([flag-score.yml](../.github/workflows/flag-score.yml)) runs main's code only. It reads the rerun's
   predictions as data, scores them against the sealed answers (a repository secret), logs the attempt in
   [attempts.jsonl](attempts.jsonl) under the GitHub name that opened the pull request, redraws every board
   (`tools/flags.py judge`), and answers on the pull request with the banded scores and a label: `flag: capture`,
   `flag: record`, `flag: scored`, `flag: tomorrow` (one scored submission per entry per day) or `flag: not scored`.

A pull request labelled `flag: capture` or `flag: record` is read before it is merged, for the rule below; then its code
is part of the lab for everyone. An entry is a directory named after its challenger: a pull request cannot score under
someone else's entry. Pull requests that touch the workflows, the scorer or the attempts are told so and are not
merged as entries.

## Rules

- **An entry is a simulator, not a set of numbers.** It must be rerunnable from its commit and readable. Code that
  recognises a flag's inputs and returns an answer, or that reads the measurements, is disqualified.
- **Open source ranks.** Entries built on closed simulators may be listed, marked, and do not rank.
- **One scored submission per entry per day.**
- **A semi holy grail counts only predictions committed before its deadline**; the commit's date is the registration.
- **Nothing is tuned to the hidden answers.** Practice cases, with public answers, exist so that nobody needs to probe.

## Public and sealed

| Public, in `flags/<id>/` | Sealed, outside the repository |
|---|---|
| the question, in plain words and SI units | the measured values |
| the conditions of every case (what the experimenters controlled) | the measurement uncertainty used for scoring |
| what to report, with units | the source's exact tables and figures |
| practice cases, with their answers, for development | a random salt |
| a SHA-256 commitment of the sealed file | |

The thirteen flags are famous experiments, named on their pages, and a determined person can find their numbers in the
literature. What keeps a flag fair is not secrecy but the rule above: a score is recorded only for a simulator that we
can rerun from its commit and whose code we can read. The commitment is the SHA-256 of the sealed file's canonical JSON,
published before any entry is scored, so the answers cannot be changed afterwards; when a flag is retired its sealed
file is published and anyone can check the hash. The trials do not name their sources until they are retired.

## How a score is formed

For one case, with prediction `p`, measurement `m` and scoring uncertainty `s` (the stated measurement uncertainty,
never below 2 per cent):

    e = |ln(p / m)| / ln(1 + s)        for positive quantities
    e = |p - m| / s                     for quantities that can change sign
    case score = exp(-e^2 / 2)         (1 within the scatter, 0.61 at one uncertainty, 0.01 at three)

A **flag score** is 100 times the mean case score over its cases and outputs, published **rounded to a band of 5
points**. 90 or more captures the flag (about half an uncertainty off on average). Only banded flag scores are
published, never a case score or an error, and never the direction of an error, so one banded number carries at most a
few bits about several hidden values at once; together with the cost of a rerun and one submission a day, that keeps
the score from being inverted into the answers.

## Tools

`tools/flags.py` (standard library only): `seal` (a maintainer moves a draft of answers into the sealed store and
publishes the commitment), `run`, `verify`, `score`, `judge` (CI: one pull request, above), `export-sealed` (a maintainer
sets the CI secret: `python3 tools/flags.py export-sealed | gh secret set OPAI_SEALED_JSON`), `board` (the front page's tables, every flag's page and card,
[LEADERBOARD.md](LEADERBOARD.md), [thirteen.svg](thirteen.svg), [podium.svg](podium.svg)) and `check-commitments`.
`board --check` draws nothing and fails when a drawn file no longer agrees with the attempts and the flags' files; CI
and `make check` run it, so a board cannot be edited by hand. Each flag's glyph and colour are drawn by the same tool
(`GLYPHS` and `STONE` in it). The sealed store is `$OPAI_SEALED` or `~/.openphysicsai/sealed/`, one file per flag,
never inside the repository.
