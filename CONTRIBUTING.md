# Contributing: people and agents

Four kinds of contribution keep this project alive, in this order of value:

1. **A measurement.** You printed something and measured it. That is the one thing no model can generate.
   See [validation/contrib/README.md](validation/contrib/README.md); a well-documented experiment can become a flag
   ([how](flags/CONTRIBUTING-FLAGS.md)).
2. **An attempt at a flag.** Download the lab, make it better with any AI model, and predict one of the thirteen flags
   or a trial; open a pull request and a machine reruns and scores it, with the score on your pull request and your
   name, your libraries and your model on the board. What wins is merged into the lab for everyone. See
   [how to take part](README.md#capture-a-flag) and [how the flags work](flags/README.md).
3. **A challenge entry.** Your solver, or this one with your idea, predicting a challenge's held-out cases.
   See [challenges/README.md](challenges/README.md).
4. **Code.** A task from [TASKS.md](TASKS.md), or something you found. Every task there is written as a prompt with an
   acceptance test, so an AI agent with an hour can take it as well as a person can.

## Rules that apply to everything

- **Provenance on every number.** A material value, a process setting, a measurement: say where it came from. The
  software refuses source-less inputs, and so do reviewers.
- **Criteria before results.** A new capability comes with a check against a closed form or an independent solution,
  and the pass criterion is written before the first run. If it fails, say so; never move the criterion to fit.
- **Five states, kept apart:** implemented, verified against theory, integrated, reachable through MCP, validated
  against measurement. Say which one your change reaches.
- **Build on existing work.** Open-source libraries and any fast compiled language are welcome under the conditions of
  rule 1 in [AGENTS.md](AGENTS.md): a compatible licence (permissive preferred, copyleft only by the owner's decision),
  pinned and vendored or fetched by the build, recorded in [docs/THIRD_PARTY.md](docs/THIRD_PARTY.md), and verified by
  the lab's own tests.
- **Build and test before a pull request:** `make && make test`, `make test-ui` if you touched the app,
  `make check` for the full gate, `make leaderboard` if you touched results.
- **Commit messages are plain** and carry no AI attribution lines. The author is you.
- **Prose without em dashes**, in files and commit messages alike.

## If you are an agent

Start at [AGENTS.md](AGENTS.md). Take one task from [TASKS.md](TASKS.md), do exactly its acceptance test, and put in
the pull request: what you ran with its final result lines, what you read but did not verify, and the commit. Do
not claim a state you did not reach. Do not edit files outside the task's ownership list. Work on a branch in a
worktree, never on main.

## Pull requests

One concern per pull request. The description says what changed, what was run, and what is not covered. Every pull
request runs the whole test suite on a clean machine; a flag entry is also rerun and scored there, with no one in the
loop ([how](flags/README.md#scored-by-a-machine)). For challenge entries, reviewers replay the result files from the
named commit.
