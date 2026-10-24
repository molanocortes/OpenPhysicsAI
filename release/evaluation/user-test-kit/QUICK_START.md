# Quick start

This takes about 10 minutes. Write down the start and end time of each step on the feedback form.

## 1. Install

```bash
shasum -a 256 -c navier-0.3.0-rc2-macos-arm64.tar.gz.sha256      # must print OK
mkdir -p ~/navier-test && tar -xzf navier-0.3.0-rc2-macos-arm64.tar.gz -C ~/navier-test
cd ~/navier-test/navier-0.3.0-rc2-macos-arm64
```

If macOS refuses to open the programs ("cannot be verified"), see `doc/INSTALL.md`, section on quarantine.

## 2. Check the installation

```bash
bin/navier-ctl doctor
```

Every line should say `ok`, and the command should exit with status 0. Otherwise, follow `DIAGNOSTICS.md`.

## 3. Run the packaged example

```bash
bin/navier-ctl study check share/navier/examples/bracket_comparison/study.json
bin/navier-ctl study run share/navier/examples/bracket_comparison/study.json --dir ~/navier-test/example
bin/navier-ctl study report ~/navier-test/example | less
```

The run takes one to two minutes. Read the report's Outcome section and compare it with
`share/navier/examples/bracket_comparison/README.md`.

## 4. Where to read

- `README.md` and `doc/WORKFLOW.md`: how a study definition is written, and what the outcomes mean.
- `doc/EVIDENCE.md`: the checks, the three refinement conclusions, and what an estimate is.
- `doc/ASSUMPTIONS.md` and `doc/LIMITATIONS.md`: what the model does not cover.
- `doc/AI_CLIENTS.md`: connecting an AI client, if you use one.

Then continue with `TASK.md`.
