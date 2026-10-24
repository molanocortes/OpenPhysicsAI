#!/usr/bin/env python3
"""run_queue.py - run the long jobs of a jobs file one after the other, unattended (rule 8 of AGENTS.md).

    nohup python3 tools/run_queue.py validation/queue.jobs --hours 10 > build/overnight/runner.log 2>&1 &

A jobs file has one job per line:   name | expected minutes | timeout minutes | shell command
Lines starting with # are comments. Each job runs in its own process group with its own log
(build/overnight/<name>.log, also in $JOB_LOG for the command itself); a job that passes its timeout is killed with
its children and the queue goes on. A job is skipped when its expected time no longer fits before the deadline, or
when less than 3 GB of disk is free. The machine is kept awake while the runner lives (caffeinate, mains power).
build/overnight/SUMMARY.md is rewritten after every job; build/overnight/DONE appears at the end. To stop after the
current job: touch build/overnight/STOP.
"""
import os, shutil, signal, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "build" / "overnight"


def main():
    jobs_file = Path(sys.argv[1])
    hours = float(sys.argv[sys.argv.index("--hours") + 1]) if "--hours" in sys.argv else 10.0
    only = sys.argv[sys.argv.index("--only") + 1].split(",") if "--only" in sys.argv else None
    OUT.mkdir(parents=True, exist_ok=True)
    for f in ("DONE", "STOP"):
        (OUT / f).unlink(missing_ok=True)
    jobs = []
    for ln in jobs_file.read_text().splitlines():
        if not ln.strip() or ln.lstrip().startswith("#"):
            continue
        name, exp, tmo, cmd = [p.strip() for p in ln.split("|", 3)]
        if only is None or name in only:
            jobs.append((name, float(exp), float(tmo), cmd))
    awake = subprocess.Popen(["caffeinate", "-i", "-m", "-s", "-w", str(os.getpid())])
    t_start, deadline, rows = time.time(), time.time() + hours * 3600, []

    def write_summary(final=False):
        head = (f"# Overnight queue, started {time.strftime('%Y-%m-%d %H:%M', time.localtime(t_start))}, "
                f"commit {subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], cwd=ROOT, capture_output=True, text=True).stdout.strip()}"
                f"{' (finished ' + time.strftime('%H:%M') + ')' if final else ' (running)'}\n\n"
                "| job | outcome | wall | log |\n|---|---|---|---|\n")
        (OUT / "SUMMARY.md").write_text(head + "\n".join(rows) + "\n")

    for name, exp, tmo, cmd in jobs:
        log = OUT / f"{name}.log"
        if (OUT / "STOP").exists():
            rows.append(f"| {name} | not started: STOP file | | |")
            continue
        if time.time() + exp * 60 > deadline:
            rows.append(f"| {name} | skipped: its {exp:.0f} expected minutes no longer fit before the deadline | | |")
            write_summary()
            continue
        free_gb = shutil.disk_usage(ROOT).free / 1e9
        if free_gb < 3.0:
            rows.append(f"| {name} | skipped: only {free_gb:.1f} GB of disk free | | |")
            write_summary()
            continue
        t0 = time.time()
        rows.append(f"| {name} | running since {time.strftime('%H:%M')} (expected {exp:.0f} min, killed at {tmo:.0f}) | | {log.name} |")
        write_summary()
        with open(log, "w") as lf:
            lf.write(f"$ {cmd}\n\n")
            lf.flush()
            p = subprocess.Popen(["/bin/bash", "-c", cmd], cwd=ROOT, stdout=lf, stderr=subprocess.STDOUT,
                                 start_new_session=True, env={**os.environ, "JOB_LOG": str(log), "PYTHONUNBUFFERED": "1"})
            try:
                rc = p.wait(timeout=tmo * 60)
                outcome = "finished, exit 0" if rc == 0 else f"finished, exit {rc}"
            except subprocess.TimeoutExpired:
                # the whole group, politely and then not; a group that is already gone answers with an OSError
                # (EPERM on macOS, ESRCH elsewhere), which must never take the queue down with it
                for sig in (signal.SIGTERM, signal.SIGKILL):
                    try:
                        os.killpg(p.pid, sig)
                    except OSError:
                        break
                    try:
                        p.wait(timeout=8)
                        break
                    except subprocess.TimeoutExpired:
                        continue
                outcome = f"KILLED at its {tmo:.0f} minute timeout"
            except Exception as e:  # one job's trouble is never the queue's
                outcome = f"runner error: {type(e).__name__}: {e}"
        wall = time.time() - t0
        rows[-1] = f"| {name} | {outcome} | {wall / 60:.1f} min | {log.name} |"
        write_summary()
    write_summary(final=True)
    (OUT / "DONE").write_text(time.strftime("%Y-%m-%d %H:%M\n"))
    awake.terminate()


if __name__ == "__main__":
    main()
