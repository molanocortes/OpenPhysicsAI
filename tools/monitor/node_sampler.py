#!/usr/bin/env python3
"""node_sampler.py - the compute monitor's recorder, running on each compute node (docs/compute-nodes.md).

A system service in the node's Linux, installed by `python3 tools/monitor/monitor.py install NODE`. Every two seconds
it reads what the machine is doing and writes it down; the monitor on the Mac follows the lines and draws them.

  - each logical CPU's busy share, from /proc/stat (user, nice, system, irq, softirq; steal is kept apart)
  - floating-point operations, instructions and cycles, counted on every CPU by the processor's own counters
    (perf_event_open). Operations come from FP_ARITH_INST_RETIRED, each instruction weighted by the operations it
    does: 1 scalar, 2 for 128-bit double, 4 for 128-bit single or 256-bit double, 8 and 16 for the wider ones. The
    processor counts an FMA instruction twice, so a fused multiply-add is two operations, as usual.
    Checked on the first node against a loop with a known count: 2.0011e9 counted, 2.0010e9 expected, for a scalar
    and an AVX2-FMA build alike (tools/monitor/countertest.py).
  - memory, load, the processes using the CPUs, the GPU when nvidia-smi is there
  - the jobs that tools/remote.py started, and the CPU time each has used

Only the node's Linux is seen: that is where the jobs run. Windows' own work on a windows-wsl node is not counted.

Files:
  /run/openphysics-monitor/live.jsonl              one line per sample, a minute line each minute (the Mac follows it)
  /var/lib/openphysics-monitor/minutes/DAY.jsonl    one line per minute (UTC day), kept: since version 2 with each
                                                   thread's busy share and peak, and the processes that used the CPUs
  /var/lib/openphysics-monitor/totals.json          running totals since the recorder was first installed
  /var/lib/openphysics-monitor/info.json            the machine: processor, threads, memory, counters, GPU

    python3 node_sampler.py [--jobs DIR]     run (as root: counting on every CPU needs it)
    python3 node_sampler.py --once           one sample after two seconds, printed, then exit
"""
import ctypes
import datetime
import json
import os
import platform
import shutil
import signal
import socket
import struct
import subprocess
import sys
import threading
import time

VERSION = 2  # 2: a minute line carries each thread's busy share (cpu), its peak (cpk) and the processes (top)
PERIOD = 2.0
LIVE_DIR = "/run/openphysics-monitor"
LIVE = LIVE_DIR + "/live.jsonl"
LIVE_MAX_BYTES = 4 << 20  # then the live file starts again; the minutes keep the history
STORE = "/var/lib/openphysics-monitor"
CLK = os.sysconf("SC_CLK_TCK")

# FP_ARITH_INST_RETIRED (event 0xC7): umask groups with the operations each counted instruction does
FP_GROUPS = [(0x03, 1), (0x04, 2), (0x18, 4), (0x20, 8)]
FP_GROUPS_AVX512 = [(0x03, 1), (0x04, 2), (0x18, 4), (0x60, 8), (0x80, 16)]
# Intel cores whose counters have that event (the kernel's name for the core's PMU)
FP_PMUS = {"skylake", "icelake", "tigerlake", "sapphire_rapids", "emerald_rapids", "granite_rapids"}


def readf(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return ""


class Counters:
    """the processor's own event counters on every CPU: floating-point operations, instructions, cycles"""

    def __init__(self, ncpu, fp_groups):
        self.rows, self.status = [], "measured"
        if platform.machine() != "x86_64":
            self.status = "unavailable: perf_event_open is set up here for x86_64 only"
            return
        self.libc = ctypes.CDLL(None, use_errno=True)
        self.libc.syscall.restype = ctypes.c_long
        try:
            for cpu in range(ncpu):
                row = [("flops", w, self.open(4, (umask << 8) | 0xC7, cpu)) for umask, w in fp_groups]
                row.append(("ins", 1, self.open(0, 1, cpu)))  # PERF_TYPE_HARDWARE, instructions
                row.append(("cyc", 1, self.open(0, 0, cpu)))  # PERF_TYPE_HARDWARE, cycles
                self.rows.append(row)
        except OSError as e:
            for row in self.rows:
                for _, _, fd in row:
                    os.close(fd)
            self.rows, self.status = [], "unavailable: %s" % e
        if self.rows and not fp_groups:
            self.status = "instructions and cycles measured; this processor's floating-point event is not known here"

    def open(self, type_, config, cpu):
        attr = bytearray(64)  # perf_event_attr, first version; read_format: time enabled and running, to scale
        struct.pack_into("<IIQQQQQIIQ", attr, 0, type_, 64, config, 0, 0, 3, 0, 0, 0, 0)
        buf = (ctypes.c_char * 64).from_buffer(attr)
        fd = self.libc.syscall(ctypes.c_long(298), buf, ctypes.c_int(-1), ctypes.c_int(cpu), ctypes.c_int(-1),
                               ctypes.c_ulong(8))
        if fd < 0:
            e = ctypes.get_errno()
            raise OSError(e, "perf_event_open(type %d, config %#x, cpu %d): %s" % (type_, config, cpu, os.strerror(e)))
        return fd

    def read(self):
        tot = {"flops": 0.0, "ins": 0.0, "cyc": 0.0}
        for row in self.rows:
            for kind, w, fd in row:
                v, enabled, running = struct.unpack("<QQQ", os.read(fd, 24))
                tot[kind] += w * (v * enabled / running if running else 0.0)
        return tot


class Gpu(threading.Thread):
    """nvidia-smi's own loop, one line every two seconds; the latest is kept"""

    def __init__(self, exe):
        super().__init__(daemon=True)
        self.exe, self.latest, self.name = exe, None, None

    def run(self):
        q = ["--query-gpu=name,utilization.gpu,memory.used,memory.total,temperature.gpu,power.draw",
             "--format=csv,noheader,nounits", "-l", "2"]
        while True:
            try:
                p = subprocess.Popen([self.exe] + q, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
                for line in p.stdout:
                    f = [x.strip() for x in line.split(",")]
                    if len(f) < 6:
                        continue
                    num = lambda s: float(s) if s.replace(".", "", 1).isdigit() else None  # noqa: E731
                    self.name = f[0]
                    self.latest = {"t": time.time(), "util": num(f[1]), "mem": num(f[2]), "mem_total": num(f[3]),
                                   "temp": num(f[4]), "power": num(f[5])}
                p.wait()
            except OSError:
                pass
            time.sleep(30)


def machine_info(counters_status, gpu_name):
    model, flags, cores = "", set(), set()
    phys = core = None
    with open("/proc/cpuinfo") as f:
        for line in f:
            k, _, v = line.partition(":")
            k, v = k.strip(), v.strip()
            if k == "model name" and not model:
                model = v
            elif k == "flags" and not flags:
                flags = set(v.split())
            elif k == "physical id":
                phys = v
            elif k == "core id":
                core = v
                cores.add((phys, core))
    mem = 0
    for line in readf("/proc/meminfo").splitlines():
        if line.startswith("MemTotal:"):
            mem = int(line.split()[1]) * 1024
    return {"kind": "info", "version": VERSION, "host": socket.gethostname(), "cpu": " ".join(model.split()),
            "threads": os.cpu_count(), "cores": len(cores) or None, "memory": mem, "avx2": "avx2" in flags,
            "avx512": "avx512f" in flags, "pmu": readf("/sys/bus/event_source/devices/cpu/caps/pmu_name"),
            "counters": counters_status, "gpu": gpu_name, "kernel": platform.release(),
            "wsl": "microsoft" in platform.release().lower()}


def read_stat():
    rows = {}
    with open("/proc/stat") as f:
        for line in f:
            if line.startswith("cpu") and line[3].isdigit():
                p = line.split()
                v = [int(x) for x in p[1:9]]  # user nice system idle iowait irq softirq steal
                rows[int(p[0][3:])] = (v[0] + v[1] + v[2] + v[5] + v[6], v[3] + v[4], v[7])
    return [rows[k] for k in sorted(rows)]


def read_mem():
    m = {}
    for line in readf("/proc/meminfo").splitlines():
        k, _, v = line.partition(":")
        if k in ("MemTotal", "MemAvailable"):
            m[k] = int(v.split()[0]) * 1024
    return m.get("MemTotal", 0) - m.get("MemAvailable", 0), m.get("MemTotal", 0)


def read_procs():
    """pid: (name, session, own CPU ticks, waited-for children's ticks)"""
    out = {}
    for d in os.listdir("/proc"):
        if not d.isdigit():
            continue
        s = readf("/proc/%s/stat" % d)
        r = s.rfind(")")
        if r < 0:
            continue
        f = s[r + 2:].split()
        try:
            out[int(d)] = (s[s.find("(") + 1:r], int(f[3]), int(f[11]) + int(f[12]), int(f[13]) + int(f[14]))
        except (IndexError, ValueError):
            pass
    return out


def iso_time(s):
    try:
        return datetime.datetime.fromisoformat(s).timestamp()
    except ValueError:
        return None


def job_dirs(jobs_dir):
    """the shared copy's jobs and each lane's (tools/remote.py keeps a lane's jobs in ~/lanes/LANE/jobs beside ~/jobs)"""
    out = [(None, jobs_dir)]
    lanes = os.path.join(os.path.dirname(jobs_dir.rstrip("/")), "lanes")
    try:
        out += [(lane, os.path.join(lanes, lane, "jobs")) for lane in sorted(os.listdir(lanes))
                if os.path.isdir(os.path.join(lanes, lane, "jobs"))]
    except OSError:
        pass
    return out


def read_jobs(jobs_dir, procs):
    """the jobs tools/remote.py started, in every lane: the running ones and the last finished, with the CPU time
    each used"""
    entries = []
    for lane, base in job_dirs(jobs_dir):
        try:
            entries += [(jid, lane, os.path.join(base, jid)) for jid in os.listdir(base)
                        if os.path.isdir(os.path.join(base, jid))]
        except OSError:
            continue
    entries.sort()  # a job's id begins with its start time
    jobs = []
    for jid, lane, d in entries[-40:]:
        state = readf(d + "/state")
        if not state:
            continue
        cmd = readf(d + "/cmd").splitlines()
        j = {"id": jid, "state": state, "started": iso_time(readf(d + "/started")),
             "cmd": " ".join(cmd[1:])[:160] if len(cmd) > 1 else ""}
        if lane:
            j["lane"] = lane
        ex, sec = readf(d + "/exit"), readf(d + "/seconds")
        if ex.lstrip("-").isdigit():
            j["exit"] = int(ex)
            try:
                j["ended"] = os.path.getmtime(d + "/exit")
            except OSError:
                pass
        if sec.isdigit():
            j["seconds"] = int(sec)
        pid = readf(d + "/pid")
        if state == "running" and pid.isdigit():
            sid = int(pid)
            if sid in procs:  # every live process of the job's session, with what its finished children used
                j["cpu_s"] = round(sum(p[2] + p[3] for p in procs.values() if p[1] == sid) / CLK, 1)
            else:
                j["state"] = "lost"  # its processes are gone and it never wrote an exit code
        jobs.append(j)
    running = [j for j in jobs if j["state"] == "running"]
    return running + [j for j in jobs if j["state"] != "running"][-10:]


def write_json(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(obj, f, separators=(",", ":"))
    os.replace(tmp, path)


def emit(obj):
    line = json.dumps(obj, separators=(",", ":")) + "\n"
    try:
        if os.path.getsize(LIVE) > LIVE_MAX_BYTES:
            os.truncate(LIVE, 0)
    except OSError:
        pass
    with open(LIVE, "a") as f:
        f.write(line)


class Minute:
    """the samples of one minute, summed; written as one line when the minute is over"""

    def __init__(self, t, cores):
        self.t, self.cores = t, cores
        self.s = self.busy = self.flops = self.ins = self.cyc = self.mem = self.umax = 0.0
        self.run, self.gpu = 0, []
        self.cpu, self.cpk = [0.0] * cores, [0.0] * cores  # each thread's busy seconds, and its busiest sample
        self.top = {}  # process name: CPU seconds

    def add(self, dt, busy, sample):
        self.s += dt
        self.busy += busy
        self.flops += sample.get("flops", 0)
        self.ins += sample.get("ins", 0)
        self.cyc += sample.get("cyc", 0)
        self.mem += sample["mem"][0] * dt
        self.umax = max(self.umax, sample["util"])
        for i, u in enumerate(sample["cpu"][:len(self.cpu)]):
            self.cpu[i] += u * dt
            self.cpk[i] = max(self.cpk[i], u)
        for name, cores in sample.get("top") or []:  # the processes of the last ten seconds, for each sample's share
            self.top[name] = self.top.get(name, 0.0) + cores * dt
        if sample.get("gpu") and sample["gpu"].get("util") is not None:
            self.gpu.append(sample["gpu"]["util"])

    def record(self, running):
        s = self.s or 1.0
        return {"kind": "minute", "t": self.t, "s": round(self.s, 2), "cores": self.cores,
                "busy": round(self.busy, 2), "umax": round(self.umax, 3), "flops": round(self.flops),
                "ins": round(self.ins), "cyc": round(self.cyc), "mem": round(self.mem / self.s if self.s else 0, 3),
                "run": running, "gpu": round(sum(self.gpu) / len(self.gpu), 1) if self.gpu else None,
                "cpu": [round(c / s, 3) for c in self.cpu], "cpk": [round(c, 3) for c in self.cpk],
                "top": [[k, round(v / s, 2)] for k, v in sorted(self.top.items(), key=lambda x: -x[1])[:3]]}


def main():
    jobs_dir = sys.argv[sys.argv.index("--jobs") + 1] if "--jobs" in sys.argv else "/home/node/jobs"
    once = "--once" in sys.argv
    ncpu = len(read_stat())
    flags = readf("/proc/cpuinfo")
    pmu = readf("/sys/bus/event_source/devices/cpu/caps/pmu_name")
    groups = (FP_GROUPS_AVX512 if " avx512f" in flags else FP_GROUPS) if pmu in FP_PMUS else []
    counters = Counters(ncpu, groups)
    smi = shutil.which("nvidia-smi") or ("/usr/lib/wsl/lib/nvidia-smi" if os.path.exists("/usr/lib/wsl/lib/nvidia-smi") else None)
    gpu = Gpu(smi) if smi else None
    if gpu:
        gpu.start()
    info = machine_info(counters.status, None)

    if not once:
        os.makedirs(LIVE_DIR, exist_ok=True)
        os.makedirs(STORE + "/minutes", exist_ok=True)
        os.chmod(STORE, 0o755)
        write_json(STORE + "/info.json", info)
        emit(info)
    totals = None
    try:
        with open(STORE + "/totals.json") as f:
            totals = json.load(f)
    except (OSError, ValueError):
        pass
    if not totals:
        totals = {"since": time.time(), "upto": None, "s": 0.0, "busy": 0.0, "flops": 0.0, "ins": 0.0, "cyc": 0.0,
                  "peak_flops": 0.0, "minutes": 0}

    def stop(*_):
        raise SystemExit(0)
    signal.signal(signal.SIGTERM, stop)

    prev_stat, prev_cnt, prev_t = read_stat(), counters.read(), time.time()
    procs_prev, procs_t, top, jobs, running = read_procs(), time.time(), [], [], 0
    minute = None
    next_wake = time.monotonic() + PERIOD
    n = 0
    try:
        while True:
            time.sleep(max(0.0, next_wake - time.monotonic()))
            next_wake += PERIOD
            if next_wake < time.monotonic():  # the machine paused (a suspended VM): start the rhythm again
                next_wake = time.monotonic() + PERIOD
            t, stat, cnt = time.time(), read_stat(), counters.read()
            dt = t - prev_t
            if dt <= 0 or dt > 10 * PERIOD:  # a gap: count nothing across it
                prev_stat, prev_cnt, prev_t = stat, cnt, t
                continue
            cpu, steal, busy_s = [], 0.0, 0.0
            for (b1, i1, s1), (b0, i0, s0) in zip(stat, prev_stat):
                db, di, ds = b1 - b0, i1 - i0, s1 - s0
                tot = db + di + ds
                u = db / tot if tot > 0 else 0.0
                cpu.append(round(u, 3))
                steal += ds / tot if tot > 0 else 0.0
                busy_s += u * dt
            used, total = read_mem()
            sample = {"kind": "sample", "t": round(t, 2), "dt": round(dt, 3), "util": round(sum(cpu) / len(cpu), 4),
                      "cpu": cpu, "steal": round(steal / len(cpu), 4), "mem": [round(used / 2**30, 3), round(total / 2**30, 3)],
                      "load": float(readf("/proc/loadavg").split()[0] or 0)}
            if counters.rows:
                for k in ("flops", "ins", "cyc"):
                    sample[k] = max(0, round(cnt[k] - prev_cnt[k]))
            if gpu and gpu.latest and t - gpu.latest["t"] < 10:
                sample["gpu"] = {k: v for k, v in gpu.latest.items() if k != "t"}
            n += 1
            if n % 5 == 1:  # the processes and jobs every ten seconds
                procs = read_procs()
                span = max(t - procs_t, 1e-3)
                by_name = {}
                for pid, p in procs.items():
                    q = procs_prev.get(pid)
                    if q and q[0] == p[0]:
                        use = (p[2] - q[2]) / CLK / span
                        if use > 0.02:
                            by_name[p[0]] = by_name.get(p[0], 0.0) + use
                top = sorted(([k, round(v, 2)] for k, v in by_name.items()), key=lambda x: -x[1])[:4]
                procs_prev, procs_t = procs, t
                jobs = read_jobs(jobs_dir, procs)
                running = sum(1 for j in jobs if j["state"] == "running")
                if not once:
                    emit({"kind": "jobs", "t": round(t, 2), "jobs": jobs})
            sample["top"] = top
            sample["run"] = running
            if once:
                print(json.dumps({"info": info, "sample": sample, "jobs": jobs}, indent=1))
                return 0
            emit(sample)
            m0 = int(t // 60) * 60
            if minute and minute.t != m0:
                rec = minute.record(running)
                for k in ("s", "busy", "flops", "ins", "cyc"):
                    totals[k] += rec[k]
                totals["minutes"] += 1
                totals["upto"] = minute.t + 60
                if rec["s"] > 30:
                    totals["peak_flops"] = max(totals["peak_flops"], rec["flops"] / rec["s"])
                rec["tot"] = dict(totals)
                with open(STORE + "/minutes/%s.jsonl" % time.strftime("%Y-%m-%d", time.gmtime(minute.t)), "a") as f:
                    f.write(json.dumps(rec, separators=(",", ":")) + "\n")
                write_json(STORE + "/totals.json", totals)
                emit(rec)
                minute = None
            if minute is None:
                minute = Minute(m0, len(cpu))
            minute.add(dt, busy_s, sample)
            if n % 30 == 0:
                if gpu and gpu.name and not info["gpu"]:
                    info["gpu"] = gpu.name
                    write_json(STORE + "/info.json", info)
                emit(info)
            prev_stat, prev_cnt, prev_t = stat, cnt, t
    except SystemExit:
        return 0


if __name__ == "__main__":
    sys.exit(main())
