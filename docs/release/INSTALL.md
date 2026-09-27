# Installation

## Supported platform

| Platform | Status |
|---|---|
| macOS 15 on Apple silicon (arm64) | **tested**: built and tested on an M2 with macOS 15.7.3. Binaries are built for `-mcpu=apple-m1`, so they run on every Apple-silicon Mac |
| macOS on Intel | not built or tested |
| Linux (x86-64, arm64) | the headless core is written to build there (`make headless`), but this release was neither built nor tested on Linux |
| Windows | not supported |

## Requirements

- **Runtime:** none beyond the operating system. The binaries use only macOS system libraries; `navier-ctl doctor`
  checks this.
- **Optional:**
  - Python 3.9+ to run the scripted evaluation in `share/navier/tests`;
  - NumPy and CalculiX (`ccx`) for the independent cross-check tool;
  - an MCP client (Claude Code, Codex, or another MCP host) to drive the workflow with an AI model.

## Install

```bash
tar -xzf navier-0.3.0-rc2-macos-arm64.tar.gz -C /opt       # or any folder you own
/opt/navier-0.3.0-rc2-macos-arm64/bin/navier-ctl doctor
```

Nothing is installed outside that folder. On first use the tools create `~/NAVIER-Projects`, the default workspace
(change it with `--workspace DIR`), and `~/.navier/run` only if you start `navier-server`.

macOS may quarantine binaries downloaded with a browser. If `doctor` is blocked, remove the attribute for this
folder only:

```bash
xattr -dr com.apple.quarantine /opt/navier-0.3.0-rc2-macos-arm64
```

## The diagnostic command

`navier-ctl doctor` checks:

- that `navier-mcp` and `navier-server` sit next to it;
- that only system libraries are loaded;
- that the embedded operation contract and material library parse;
- that the workspace and the temporary folder are writable;
- that `navier-mcp` answers an MCP `initialize` and lists its tools over stdio;
- whether a control server is running;
- a reference solve: two cantilevers through the comparison workflow, with the tip deflection checked against beam
  theory within 2 %.

`--json` prints machine-readable results and `--no-solve` skips the solve. The exit code is 0 when every check
passes and 7 otherwise.

## File access

The tools read input files only below their read roots: `$HOME`, `/tmp`, `/Volumes` and the working directory. They
write projects and exports only below the workspace and `$TMPDIR`. Add roots with `--allow-read DIR` and
`--allow-write DIR` (up to 16 of each kind; a folder inside an existing root needs none, and a write root is also
readable). `capabilities_get` lists the roots in effect.

## Uninstall

Delete the extracted folder. Your studies stay in the workspace until you delete them.
