# Compute nodes: other machines that run the lab's long and large jobs

Up: [AGENTS.md](../AGENTS.md)

The development machine is where people, the app and the headset work, and a laptop has little memory. A compute node
is another machine on the same network: it builds the engine and runs long or large jobs. Those jobs are sent, followed
and fetched from the development machine with [tools/remote.py](../tools/remote.py), and they keep running when it
sleeps or leaves the network.

Nodes and their logins are private. They live in `~/.openphysicsai/nodes.json` on the development machine, never in
the repository. One entry per node (the address is a placeholder from the range kept for documentation):

```json
{"bignode": {"host": "192.0.2.20", "mdns": "<name>.local", "user": "<login>", "key": "~/.ssh/openphysics_node",
             "kind": "windows-wsl", "distro": "Ubuntu-24.04", "repo": "/home/node/openphysics", "threads": 8}}
```

`kind` is `linux` for a plain Linux machine, or `windows-wsl` for Windows' own OpenSSH server in front of Linux in
WSL2 (below). `mdns` is optional: it is asked for when the address no longer answers. An entry with `"planned": true`
is a machine not set up yet.

```bash
python3 tools/remote.py status                     # reachable, Linux running, load, memory, jobs
python3 tools/remote.py sync && python3 tools/remote.py build      # send the tracked sources, make -jN headless
python3 tools/remote.py run --name tests -- 'make test-fast'       # a detached job; prints its id
python3 tools/remote.py jobs                       # every job: state, exit code, seconds
python3 tools/remote.py log JOB                    # the end of its log
python3 tools/remote.py fetch JOB path/in/repo ... # results to build/remote/NODE/JOB/ on this machine
```

## A Windows machine as a node: Linux in WSL2

This is how the first node was set up (8 October 2026). Every step can be undone.

1. **Windows' own OpenSSH server.**
   - The development machine's public key goes in `administrators_authorized_keys`. That file's permissions are set
     by SID, since group names differ between the languages of Windows.
   - The key line is restricted with `from=` to the local network, and a firewall rule opens TCP 22 to the local
     subnet only.
   - Password and keyboard-interactive login are switched off. The original `sshd_config` is kept as
     `sshd_config.before-openphysics`, and the new one is tested with `sshd -t` before the service restarts.
   - PowerShell is the default shell.
2. **Power.** Never sleep or hibernate on mains power. A power supply that cannot hold every thread at full load cuts
   out without a crash code (Windows logs Kernel-Power 41); capping the processor (Windows' maximum processor state)
   lowers the peak draw, and [tools/monitor/powertest.sh](../tools/monitor/powertest.sh) checks the result: every
   thread at full floating-point load for minutes, and jumps from idle to full.
3. **Linux.**
   - Ubuntu 24.04's WSL image from Canonical, with its checksum checked, imported with `wsl --import` into a folder
     on a data disk.
   - Memory, threads and the swap file's place set in `.wslconfig`.
   - A user `node` without a password.
   - systemd on, and lingering enabled.
   - The build tools from Ubuntu's archive: clang, make, python3-numpy, rsync.
   - The engine builds with `make headless`, as in the Linux CI.
4. **A keep-alive.** WSL stops a distribution when its last connection closes, and ends whatever has not detached
   from it. So `wsl.exe --exec sleep infinity` runs as a process started through WMI, outside any SSH session.
   `remote.py run` starts it if it is missing, and `remote.py wake` does so on demand. A scheduled task that starts
   it at boot brings it back after Windows restarts.

The CAD libraries are built on a node like anywhere else: `make cad` once (about an hour on 4 cores; on Linux the
distribution's CMake), into the node's own `~/.openphysicsai/third_party/`. The pinned source archives can be copied
from the development machine instead of fetched again; their checksums are checked either way. On the first node it
took 19.5 minutes on 8 threads, with Open CASCADE's X11 code switched off (`USE_XLIB=OFF`: a headless machine has no
X11), and `cadtest`, `cellstifftest` and `specimentest` pass there.

What bit us, so that nobody pays for it twice:

- Commands for Linux go to `wsl.exe -- bash -s` on stdin. Quoting through PowerShell, `wsl.exe` and bash in one
  string breaks.
- Multi-line PowerShell runs from a `.ps1` copied with scp and run with `-File`. Piped to `-Command -`, its blocks
  are skipped.
- A detached job must have started before the connection closes. `remote.py` waits until the job has written its
  pid file.
- rsync works through it with `--rsync-path="wsl -d DISTRO rsync"`; this was checked bit for bit.
- A node that changes address (Wi-Fi and cable give it different ones) is best named by its mDNS name. By name,
  macOS may connect over IPv6 link-local, which a key line restricted with `from=` to IPv4 addresses refuses as
  "Permission denied (publickey)"; `AddressFamily inet` for that name in `~/.ssh/config` avoids it.
- A job that takes more memory than the node has makes Linux swap and, on a failing disk, can stall the whole node:
  the WSL disk image went read-only, every later `wsl.exe -- bash -s` hung, and the hung sessions used up Windows'
  socket buffers until even SSH timed out. Jobs that can take much memory run under a ceiling (`ulimit -v`), a memory
  guard such as `earlyoom` in the node's Linux ends the largest process before Linux freezes, the swap file belongs on
  a sound disk, and a scheduled task in Windows can end orphaned `wsl.exe` sessions and restart the keep-alive.
- WSL warns "Failed to start the systemd user session" on every call. It is harmless, and `remote.py` hides it.
  Long jobs do not depend on the per-user service manager.

To undo it all:

1. `wsl --unregister DISTRO`, and delete its folder.
2. Copy `sshd_config.before-openphysics` back over `sshd_config`.
3. Remove "OpenSSH Server" under Optional features, and delete the `.wslconfig` that was written.
4. Set the processor back to 100 per cent if it was capped: `powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR
   PROCTHROTTLEMAX 100`, then `powercfg /setactive SCHEME_CURRENT`.

## The monitor

A page on the development machine shows what the machines are doing and what they have done: how busy they are now,
in one number; each machine as a ring whose segments are its threads (violet idle, gold busy), with its last hour, its
operations a second, clock, memory and the processes using it; the history over any range, the last five minutes live,
an hour, a day, a week, a month or everything recorded, stepped back a window at a time (busy share, floating-point
operations, memory, and every thread as a row, each by its average or its peak, nights shaded, a readout under the
pointer, the finger or the arrow keys); the jobs lane by lane; the work done since the recorders started, as Apollo 11
missions and Cray-1 days; and the machines compared. A Mac running the page is a machine on it too, `mac`.

```bash
python3 tools/monitor/monitor.py install NODE      # the recorder, once per node (a system service in its Linux)
python3 tools/monitor/monitor.py check NODE        # do its counters count floating-point operations right?
python3 tools/monitor/monitor.py serve &           # the page: http://127.0.0.1:8795
python3 tools/monitor/monitor.py summary 12        # the last 12 hours, in the terminal
python3 tools/monitor/monitortest.py               # its history, the Mac's readings, the benchmark file (--slow: the Mac's minute lines)
python3 tools/monitor/monitor.py serve --port 8796 --read-only --mirror http://127.0.0.1:8795   # a copy beside it
```

- **The recorder** ([tools/monitor/node_sampler.py](../tools/monitor/node_sampler.py)) reads, every two seconds,
  each thread's busy share from `/proc/stat` and the processor's own counters on every CPU (`perf_event_open`):
  floating-point operations retired (FP_ARITH_INST_RETIRED, each instruction weighted by the operations it does, an
  FMA counted twice by the processor), instructions and cycles. It writes a live file that the page follows, a minute
  file per day that it keeps, and running totals. So nothing is lost while the development machine sleeps: the page
  copies the minutes when it reconnects. Since version 2 each minute line also carries each thread's busy share
  (`cpu`), its busiest two-second sample (`cpk`) and the processes that used the cores (`top`); a node keeps them once
  its recorder is installed again (`monitor.py install NODE`). Older minutes hold the machine's share only, and the
  page draws them hatched across its thread rows; the last hour is always drawn thread by thread from the live samples.
- **What it counts**: everything in the node's Linux, which is where the jobs run. Windows' own work on a windows-wsl
  node is not counted. WSL2 on the first node passes the counters through (its `cpu` PMU is `skylake`); it does not
  pass the energy counters, so there is no power reading.
- **Checked**: [tools/monitor/countertest.py](../tools/monitor/countertest.py) runs a loop with a known count,
  2 n x 1000 + n = 2.001e9 operations, in a scalar and an AVX2-FMA build; the criterion is 1%. On the first node
  (8 October 2026) both counted 2.0010e9, ratio 1.00000.
- **The server** ([tools/monitor/monitor.py](../tools/monitor/monitor.py)) listens on 127.0.0.1:8795 only. To keep it
  running, start it at login with the system's service manager (on macOS a LaunchAgent in `~/Library/LaunchAgents/`
  that runs `monitor.py serve` and restarts it if it stops); the page's own files are read fresh on every load. A node
  that stops answering for over two and a half minutes, while the development machine stays awake on the same
  network, is announced in a notification on the Mac, and again when it is back. The server follows each node over
  one SSH connection and copies the history to `~/.openphysicsai/monitor/` (private: it names the machines). A
  machine marked `"planned": true` in `nodes.json` is shown as waiting to be set up.
- **A Mac** is sampled by the server itself, without SSH (`MacProbe` in monitor.py): each core's ticks from the
  Mach call `host_processor_info` (its efficiency and performance cores named from the device tree), memory in use
  from `host_statistics64` as Activity Monitor counts it, memory pressure and swap from `sysctl`
  (`kern.memorystatus_vm_pressure_level`, `kern.memorystatus_level`, `vm.swapusage`), the time since the last input
  from CoreGraphics (away after two minutes), the power source from `pmset`, the processes from `ps`. macOS gives
  ordinary programs no floating-point counters, so the Mac's operations are an estimate, shown as one and never added
  to the nodes' measured totals: 0.2 operations per instruction of the lab's own programs, whose instructions each
  process reports without root (`proc_pid_rusage`, one pass over the process list a sample; the programs are labrun,
  propulsion, openphysics and its servers (and `navier*`, their names before 2026-10-21), ccx, the `*test` tools and their variants, and anything under the build
  directory of this checkout or one of its git worktrees). The first two nodes' own counters gave 0.78 and 0.41
  operations per instruction, and the M2's 128-bit vectors need up to twice the instructions of 256-bit ones, so
  0.41 / 2 = 0.2; Python and everything else are left out, so the real value is likely higher. A ratio the benchmark
  measures on the Mac (`machines.mac.flop_per_instruction_min` in bench.json) replaces 0.2. Its minute lines, in the
  nodes' form with swap, pressure, time away and on battery and the estimate added, go to
  `~/.openphysicsai/monitor/mac/`. A showcase runner's log on the Mac (`~/.openphysicsai/showcase-mac/log.jsonl`) is
  read when it is there: a line with `mode` (full, capped, paused) is shown beside the Mac's busy share and as a band
  under its history, and a line naming a run (`scenario`, `name` or `id`, with `start`, `end`, `outcome` or `exit`)
  is a job in the Mac's showcase lane.
- **Machines compared** reads `~/.openphysicsai/monitor/bench.json` (written by the benchmark: `machines`, and `tests`
  each with `title`, `unit`, `higher_is_better`, `results`, `background` and `method`), a bar for each machine; until
  that file exists the section says the benchmark is pending and shows no number.
- **The data**, for any other client: `/api/threads?node=NODE&t0=T0&t1=T1&bins=N` gives each thread's average and peak
  busy share in N columns from T0 to T1 (Unix seconds), with the machine's busy share and peak, operations a second
  and the fastest, memory, jobs running and the processes in the same columns (and for a Mac swap, pressure and
  away, in use or on battery); `/api/bench` gives the benchmark's file. `/api/state`, `/api/live`, `/api/history` and
  `/api/summary` are as before. `serve --read-only` follows no node and writes nothing; with `--mirror URL` it takes
  the live lines from the monitor at URL, which is how a second copy of the page runs beside the first.
- **Apollo 11**, as the page counts it: the two guidance computers needed at least one memory cycle of 11.72 us per
  instruction, so over the mission's 8 d 3 h 18 min 35 s they could execute at most 1.2e11 instructions. One mission
  is that many of our instructions, one for one, which favours Apollo.

## Rules

- A node runs what the development machine sends. Nothing else on a node is read, copied or sent anywhere: not its
  owners' files, and on a machine that belongs to someone else, none of its owner's data. Only the repository's
  tracked files go there, and only the jobs' results come back.
- Long runs belong on a node: AGENTS.md rule 8 limits runs on the shared laptop, and a node's jobs are detached by
  construction. The laptop still runs the cheap verification cases first; a node runs the long last step.
- A node is reached with a key only, from the local network only.
