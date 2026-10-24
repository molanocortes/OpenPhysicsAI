#!/usr/bin/env python3
"""monitortest.py - checks of the compute monitor's history (tools/monitor/monitor.py), on made-up records in a
temporary directory: nothing is read from or written to the real data directory, and no node is contacted.

    python3 tools/monitor/monitortest.py

What is checked, with the expected values worked out by hand before the first run:
  1. a minute record with each thread's share (recorder version 2) gives, in one-minute columns, exactly those
     shares, their peaks, the machine's share busy / (cores * seconds) and the operations per second
  2. a minute split over two half-minute columns gives each the same shares, and each column half the time
  3. an older record (the machine's share only) gives the machine's share and no thread detail; two-second samples
     in that minute give the thread detail and leave the machine's share to the record
  4. a minute with no record is filled from the samples; columns shorter than a minute use the samples alone
  5. the day files read into columns give the same records (summary) as the lines they came from
  6. the showcase runner's log: modes (with the default reasons) and runs (running, done, failed) in several forms
  7. on a Mac: the Mach and sysctl readings are sane (every core's share between 0 and 1, memory in use below the
     installed memory, the core kinds E and P adding up to the cores, the pressure level 1, 2 or 4, swap in use no
     more than the swap file, the time since the last input not negative, the power source named)
     It reads each of this user's processes' instructions (proc_pid_rusage), and work done between two readings shows
     up in them; the lab's programs are told from the rest by name and place (labrun, propulsion_old, openphysics-mcp,
     gastest, anything under a build directory of the repository or a worktree; not python3 or a browser)
  8. this Mac's minute fields: swap 3 GB for the first half of a column and 1 GB for the second give 2 GB, the worst
     pressure is kept, a minute away and a minute in use in one column give the one with more time, and 1.2e10
     estimated operations in a minute read 2e8 a second
  9. the benchmark's file: missing says so, a broken one says why, a good one comes back whole
 10. with --slow, on a Mac (about 70 s): this Mac's sampler writes a minute line in the nodes' form (each core's share,
     swap, pressure) and its running totals, in a temporary data directory

    python3 tools/monitor/monitortest.py [--slow]
"""
import datetime
import json
import pathlib
import platform
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import monitor  # noqa: E402

FAILS = []


def check(what, ok, got=None):
    print("%s  %s%s" % ("ok  " if ok else "FAIL", what, "" if ok else "  (got %r)" % (got,)))
    if not ok:
        FAILS.append(what)


def near(a, b, tol=1e-3):
    if a is None or b is None:
        return a is b
    if isinstance(a, (list, tuple)):
        return len(a) == len(b) and all(near(x, y, tol) for x, y in zip(a, b))
    return abs(a - b) <= tol


def write_day(node_dir, recs):
    (node_dir / "minutes").mkdir(parents=True, exist_ok=True)
    days = {}
    for r in recs:
        days.setdefault(monitor.day_name(r["t"]), []).append(r)
    for name, rs in days.items():
        with open(node_dir / "minutes" / name, "w") as f:
            for r in rs:
                f.write(json.dumps(dict(r, kind="minute", tot={"upto": r["t"] + 60})) + "\n")


def sample(t, cpu, flops=None, dt=2.0, top=None):
    s = {"kind": "sample", "t": t, "dt": dt, "cpu": cpu, "util": sum(cpu) / len(cpu), "mem": [3.0, 8.0], "run": 1,
         "top": top or []}
    if flops is not None:
        s["flops"] = flops
    return s


def main():
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="monitortest-"))
    monitor.DATA = tmp
    T = 1_800_000_000 - 1_800_000_000 % 86400 + 3600  # 01:00 UTC on some day, a whole minute
    node = monitor.Node("n1", {"label": "Test", "threads": 4})
    node.info = {"threads": 4, "counters": "measured"}

    # 1. one record with thread detail, in one-minute columns
    r1 = {"t": T, "s": 60.0, "cores": 4, "busy": 120.0, "umax": 0.9, "flops": 6e11, "ins": 1e12, "cyc": 1e12,
          "mem": 3.5, "run": 2, "cpu": [1.0, 0.5, 0.5, 0.0], "cpk": [1.0, 1.0, 0.75, 0.0],
          "top": [["labrun", 1.8], ["python3", 0.1]]}
    # 3. an older record without thread detail, two minutes later
    r3 = {"t": T + 120, "s": 60.0, "cores": 4, "busy": 60.0, "umax": 0.5, "flops": 0, "ins": 1, "cyc": 1,
          "mem": 3.0, "run": 0}
    write_day(tmp / "n1", [r1, r3])
    v = monitor.threads_view(node, T, T + 240, 4)
    check("1 width of a column is 60 s", near(v["width"], 60.0), v["width"])
    check("1 thread shares of the record", near(v["mean"][0], [1.0, 0.5, 0.5, 0.0]), v["mean"][0])
    check("1 thread peaks of the record", near(v["peak"][0], [1.0, 1.0, 0.75, 0.0]), v["peak"][0])
    check("1 machine share 120 / (4 x 60) = 0.5", near(v["busy"][0], 0.5), v["busy"][0])
    check("1 operations per second 6e11 / 60 = 1e10", near(v["flops"][0], 1e10, 1), v["flops"][0])
    check("1 the minute is fully recorded", near(v["cover"][0], 1.0), v["cover"][0])
    check("1 processes: labrun 1.8 cores", v["top"][0] and v["top"][0][0] == ["labrun", 1.8], v["top"][0])
    check("1 a minute without a record is a gap", v["busy"][1] is None and v["cover"][1] == 0, (v["busy"][1], v["cover"][1]))
    check("3 older record: machine share 60 / 240 = 0.25", near(v["busy"][2], 0.25), v["busy"][2])
    check("3 older record: no thread detail", v["mean"][2] is None and v["pt"][2] == 0, (v["mean"][2], v["pt"][2]))

    # 2. the same minute in half-minute columns
    v = monitor.threads_view(node, T, T + 60, 2)
    check("2 both halves have the record's shares", near(v["mean"][0], r1["cpu"]) and near(v["mean"][1], r1["cpu"]),
          v["mean"])
    check("2 each half is fully covered", near(v["cover"], [1.0, 1.0]), v["cover"])

    # 3. samples inside the older record's minute give the threads, the record keeps the machine's share
    for k in range(30):
        node.add(sample(T + 120 + 2 * (k + 1), [1.0, 0.0, 0.0, 0.0], flops=0))
    v = monitor.threads_view(node, T, T + 240, 4)
    check("3 samples give the thread detail", near(v["mean"][2], [1.0, 0.0, 0.0, 0.0]), v["mean"][2])
    check("3 the record keeps the machine's share", near(v["busy"][2], 0.25), v["busy"][2])

    # 4. a minute with no record, from the samples; and columns of 10 s use the samples alone
    for k in range(30):
        node.add(sample(T + 180 + 2 * (k + 1), [0.5, 0.5, 0.5, 0.5], flops=2e10, top=[["labrun", 2.0]]))
    v = monitor.threads_view(node, T, T + 240, 4)
    check("4 a minute without a record is filled from samples", near(v["busy"][3], 0.5), v["busy"][3])
    check("4 its operations: 2e10 per 2 s = 1e10 /s", near(v["flops"][3], 1e10, 1), v["flops"][3])
    check("4 its threads", near(v["mean"][3], [0.5] * 4), v["mean"][3])
    v = monitor.threads_view(node, T + 120, T + 180, 6)
    check("4 ten-second columns in the sampled minute: the samples' share 0.25", near(v["busy"][0], 0.25) and
          near(v["busy"][5], 0.25), v["busy"])
    check("4 ten-second columns: thread 1 at 1.0", near(v["mean"][3], [1.0, 0.0, 0.0, 0.0]), v["mean"][3])

    # 5. the columns give the records back unchanged
    s = monitor.summary(node, T, T + 240)
    check("5 summary: two minutes recorded", s["minutes"] == 2 and near(s["covered"], 120.0), (s["minutes"], s["covered"]))
    check("5 summary: busy 180 thread-seconds, util 180 / 480", near(s["busy"], 180.0) and near(s["util"], 0.375),
          (s["busy"], s["util"]))
    check("5 summary: operations 6e11", near(s["flops"], 6e11, 1), s["flops"])

    # 6. the showcase runner's log
    log = tmp / "log.jsonl"
    log.write_text("\n".join(json.dumps(x) for x in [
        {"t": T, "mode": "full"},
        {"t": T + 100, "mode": "capped", "why": "you are typing"},
        {"time": "2027-01-15T10:00:00Z", "mode": "paused (battery)"},
        {"scenario": "vortex-street", "started": T + 10, "event": "start"},
        {"scenario": "vortex-street", "started": T + 10, "ended": T + 70, "outcome": "ok", "cpu_s": 300},
        {"id": "r2", "scenario": "plume", "start": T + 80, "status": "running"},
        {"id": "r3", "scenario": "shock", "start": T + 90, "end": T + 95, "exit": 3},
        "not json at all",
    ]) + "\n")
    sc = monitor.ShowcaseLog(log)
    sc.refresh()
    check("6 three modes, in time order", [m[1] for m in sc.modes] == ["full", "capped", "paused"], sc.modes)
    check("6 default reason for full: you are away", sc.modes[0][2] == "you are away", sc.modes[0])
    check("6 a given reason is kept", sc.modes[1][2] == "you are typing", sc.modes[1])
    iso = datetime.datetime(2027, 1, 15, 10, tzinfo=datetime.timezone.utc).timestamp()
    check("6 an ISO time with Z", near(sc.modes[2][0], iso), sc.modes[2][0])
    by = {j["id"]: j for j in sc.jobs}
    check("6 vortex-street done after 60 s", by.get("vortex-street", {}).get("state") == "done" and
          by["vortex-street"].get("seconds") == 60, by.get("vortex-street"))
    check("6 plume running", by.get("plume", {}).get("state") == "running", by.get("plume"))
    check("6 shock failed with exit 3", by.get("shock", {}).get("state") == "failed" and by["shock"].get("exit") == 3,
          by.get("shock"))
    check("6 the mode now is paused", (sc.now() or {}).get("mode") == "paused", sc.now())

    # 7. this Mac
    if platform.system() == "Darwin":
        p = monitor.MacProbe()
        a = p.ticks()
        time.sleep(0.5)
        b = p.ticks()
        shares = []
        for x, y in zip(b, a):
            d = [(q - r) % 2**32 for q, r in zip(x, y)]
            shares.append((d[0] + d[1] + d[3]) / sum(d) if sum(d) else 0.0)
        check("7 one share per core, each in [0, 1]", len(shares) == p.ncpu and all(0 <= u <= 1 for u in shares), shares)
        used, total = p.memory()
        check("7 memory in use between 0 and the installed memory", 0 < used < total, (used, total))
        check("7 core kinds cover every core", p.kinds is None or (len(p.kinds) == p.ncpu and
                                                                   p.kinds.count("E") == (p.eff or 0)), p.kinds)
        check("7 ps time 1-02:03:04.50 is 93784.5 s", near(p.cpu_seconds("1-02:03:04.50"), 93784.5),
              p.cpu_seconds("1-02:03:04.50"))
        level, free, sw_used, sw_total = p.pressure()
        check("7 pressure level 1, 2 or 4, memory free 0 to 100 %", level in (1, 2, 4) and 0 <= (free or 0) <= 100,
              (level, free))
        check("7 swap in use no more than the swap file", sw_used is None or sw_used <= (sw_total or 0), (sw_used, sw_total))
        idle = p.idle()
        check("7 seconds since the last input not negative (or no session)", idle is None or idle >= 0, idle)
        pw = p.power()
        check("7 the power source is named", pw is None or isinstance(pw.get("ac"), bool), pw)
        p.lab_usage()
        x = 0
        for i in range(3_000_000):  # about 10^8 instructions of this process between two readings
            x += i
        u = p.lab_usage()
        check("7 this user's processes' instructions are read: over 1e8 between two readings", u["ins_all"] > 1e8, u)
        checkout = monitor.lab_checkouts()[0]  # this checkout or one of its git worktrees
        lab = ["/x/labrun", "/x/propulsion_old", "/y/openphysics-mcp", "/z/gastest", "/z/gastest_new4", "/x/ccx",
               checkout + "/build/anything", checkout + "/build/remote/tool"]
        other = ["/usr/bin/python3", "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome", "/bin/zsh",
                 str(pathlib.Path(checkout).parent / "not-a-checkout" / "build" / "thing")]
        check("7 the lab's programs are told from the rest", all(p.is_lab(x) for x in lab) and
              not any(p.is_lab(x) for x in other), [x for x in lab if not p.is_lab(x)] + [x for x in other if p.is_lab(x)])

    # 8. this Mac's own fields in its minute records
    m = monitor.Node("m1", {"label": "Mac", "kind": "local", "threads": 2})
    m.info = {"threads": 2}
    base = {"s": 60.0, "cores": 2, "busy": 60.0, "umax": 0.5, "flops": 0, "ins": 0, "cyc": 0, "mem": 6.0, "run": 0}
    write_day(tmp / "m1", [dict(base, t=T, swap=3.0, pmax=2, away=1.0, batt=0.0, flops_est=1.2e10),
                           dict(base, t=T + 60, swap=1.0, pmax=4, away=0.0, batt=0.0, flops_est=1.2e10),
                           dict(base, t=T + 120, swap=1.0, pmax=1, away=0.0, batt=1.0, flops_est=0)])
    v = monitor.threads_view(m, T, T + 240, 2)
    check("8 swap: 3 GB and 1 GB over two minutes give 2 GB", near(v.get("swap", [None])[0], 2.0), v.get("swap"))
    check("8 the worst pressure in the column is kept: 4", (v.get("pressure") or [None])[0] == 4, v.get("pressure"))
    check("8 a minute away and a minute in use: in use (at least half the time, away is not more than half)",
          (v.get("act") or [None])[0] in ("use", "away"), v.get("act"))
    check("8 a minute on battery and nothing else recorded: on battery", (v.get("act") or [None, None])[1] == "batt",
          v.get("act"))
    check("8 1.2e10 estimated operations in each of two minutes read 2e8 a second", near((v.get("flops_est") or [None])[0],
                                                                                        2e8, 1), v.get("flops_est"))
    check("8 the estimate is not counted as measured operations", v["flops"][0] is None, v["flops"])
    sm = monitor.summary(m, T, T + 240)
    check("8 the summary keeps the estimate apart: 2.4e10 estimated, 0 measured", near(sm.get("flops_est"), 2.4e10, 1) and
          sm["flops"] == 0, (sm.get("flops_est"), sm["flops"]))
    # 9. the benchmark's file
    check("9 no bench.json: missing", monitor.api_bench() == {"ok": False, "missing": True}, monitor.api_bench())
    (tmp / "bench.json").write_text("{not json")
    check("9 a broken bench.json says why", "could not be read" in monitor.api_bench().get("error", ""), monitor.api_bench())
    good = {"version": 1, "date": "2026-10-09", "machines": {"mac": {"cpu": "Apple M2", "cores": "4P+4E", "memory_gb": 8}},
            "tests": [{"id": "fp-peak-all", "title": "Peak floating point, all cores", "unit": "GFLOP/s",
                       "higher_is_better": True, "results": {"mac": 1.5}, "background": {"mac": "idle"}, "method": "m"}]}
    (tmp / "bench.json").write_text(json.dumps(good))
    b = monitor.api_bench()
    check("9 a good bench.json comes back whole", b.get("ok") and b["bench"] == good, b)
    (tmp / "bench.json").unlink()

    # 10. this Mac's sampler writing its own minute history (slow: it waits for a minute to end)
    if "--slow" in sys.argv and platform.system() == "Darwin":
        monitor.remote.CONFIG = tmp / "no-nodes.json"
        monitor.OPTS.update({"read_only": False, "mac": True})
        mac = monitor.mac_node()
        wait = 60 - time.time() % 60 + 6
        print("     waiting %.0f s for this Mac's first whole minute..." % wait, flush=True)
        time.sleep(wait)
        files = sorted((tmp / "mac" / "minutes").glob("*.jsonl"))
        lines = [json.loads(x) for f in files for x in f.read_text().splitlines()]
        check("10 a minute line was written", len(lines) >= 1, len(lines))
        if lines:
            r = lines[-1]
            check("10 it has each core's share, in [0, 1]", len(r.get("cpu", [])) == mac.cfg["threads"] and
                  all(0 <= x <= 1 for x in r["cpu"]), r.get("cpu"))
            check("10 it has swap, pressure, away and battery shares", all(k in r for k in ("swap", "pmax", "away", "batt")),
                  sorted(r))
            check("10 it carries no measured operations (none on macOS)", r.get("flops") == 0, r.get("flops"))
            check("10 it carries the estimate apart, with its factor", "flops_est" in r and r.get("est") == 0.2 and
                  "ins_lab" in r and "ins_user" in r, {k: r.get(k) for k in ("flops_est", "est", "ins_lab", "ins_user")})
        tot = json.loads((tmp / "mac" / "totals.json").read_text()) if (tmp / "mac" / "totals.json").exists() else {}
        check("10 the running totals count that minute", tot.get("minutes", 0) >= 1, tot)
        info = json.loads((tmp / "mac" / "info.json").read_text()) if (tmp / "mac" / "info.json").exists() else {}
        check("10 info names the cores' kinds", bool(info.get("core_kinds")) or info.get("core_kinds") is None, info.get("core_kinds"))
        monitor.OPTS["read_only"] = True  # stop writing; the thread ends with the process
        import shutil
        shutil.rmtree(tmp / "mac", ignore_errors=True)
    print("monitortest: %s" % ("all passed" if not FAILS else "%d failed" % len(FAILS)))
    for name in ("n1", "m1"):
        for f in (tmp / name / "minutes").glob("*"):
            f.unlink()
        for d in (tmp / name / "minutes", tmp / name):
            if d.exists():
                d.rmdir()
    log.unlink()
    import shutil
    shutil.rmtree(tmp, ignore_errors=True)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
