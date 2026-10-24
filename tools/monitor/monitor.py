#!/usr/bin/env python3
"""monitor.py - the compute monitor: a page on this Mac that shows what the compute nodes do (docs/compute-nodes.md).

    python3 tools/monitor/monitor.py install [NODE]       put the recorder on a node (a system service in its Linux)
    python3 tools/monitor/monitor.py check [NODE]         do the node's counters count floating-point operations right?
    python3 tools/monitor/monitor.py serve [--port 8795]  the page and its data; open http://127.0.0.1:8795
    python3 tools/monitor/monitor.py summary [HOURS]      what each machine did in the last HOURS (default 12)

serve's other options (the defaults are what a service started at login runs):
    --read-only        follow no node and write nothing: serve the history already in the data directory, for a
                       second copy of the page beside the running one (this Mac is still sampled, in memory only)
    --mirror URL       with --read-only: take the nodes' live lines from the monitor at URL (its /api/state and
                       /api/live), so the copy shows live data without connecting to the nodes itself
    --data DIR         the data directory (default ~/.openphysicsai/monitor)
    --no-mac           leave this Mac off the page

Each node's recorder (node_sampler.py) keeps its own minute history, so nothing is lost while this Mac sleeps: the
server follows each node's live lines over one SSH connection and copies the history whenever it (re)connects.
Nodes come from ~/.openphysicsai/nodes.json; an entry with "planned": true is shown as waiting to be set up. This Mac
is a machine on the page too ("mac"), sampled here without SSH (MacProbe) and keeping its own minute history in the
same form. The server listens on this Mac only (127.0.0.1); the history stays in ~/.openphysicsai/monitor/.
"""
import array
import bisect
import collections
import ctypes
import datetime
import http.server
import json
import math
import os
import pathlib
import platform
import re
import shlex
import socket
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))
import remote  # noqa: E402
from node_sampler import Minute  # noqa: E402  (this Mac's minute records are written as the nodes' are)

DATA = pathlib.Path.home() / ".openphysicsai" / "monitor"
SHOWCASE_LOG = pathlib.Path.home() / ".openphysicsai" / "showcase-mac" / "log.jsonl"
LIVE = "/run/openphysics-monitor/live.jsonl"
STORE = "/var/lib/openphysics-monitor"
WEB = HERE / "web"
PORT = 8795
PERIOD = 2.0
OPTS = {"read_only": False, "mirror": None, "mac": True}
STATIC = {"index.html": "text/html; charset=utf-8", "app.js": "text/javascript; charset=utf-8",
          "style.css": "text/css; charset=utf-8"}
# one Apollo 11 mission of guidance computing: both computers (command and lunar module) doing their fastest
# instruction, one memory cycle of 11.72 us, without a pause for the whole 8 d 3 h 18 min 35 s (an upper bound)
APOLLO_INSTRUCTIONS = 2 * (1 / 11.72e-6) * (8 * 86400 + 3 * 3600 + 18 * 60 + 35)


def lab_checkouts():
    """this checkout and its git worktrees, read once at start: the lab's own programs are built in their build/"""
    root = HERE.parents[1]
    try:
        out = subprocess.run(["git", "-C", str(root), "worktree", "list", "--porcelain"], capture_output=True,
                             text=True, timeout=10).stdout
    except (OSError, subprocess.SubprocessError):
        out = ""
    return [ln[len("worktree "):] for ln in out.splitlines() if ln.startswith("worktree ")] or [str(root)]


UNIT = """[Unit]
Description=OpenPhysicsAI compute monitor recorder (tools/monitor/node_sampler.py)
After=local-fs.target

[Service]
ExecStart=/usr/bin/python3 /opt/openphysics-monitor/node_sampler.py --jobs %(jobs)s
Restart=always
RestartSec=5
Nice=5

[Install]
WantedBy=multi-user.target
"""


# ---------------------------------------------------------------- installing the recorder on a node

def cmd_install(name, n, args):
    src = (HERE / "node_sampler.py").read_text()
    jobs = str(pathlib.PurePosixPath(n["repo"]).parent / "jobs")  # where tools/remote.py keeps the node's jobs
    script = "\n".join([
        "set -e",
        "mkdir -p /opt/openphysics-monitor",
        "cat > /opt/openphysics-monitor/node_sampler.py <<'OPENPHYSICS_SAMPLER'", src.rstrip("\n"), "OPENPHYSICS_SAMPLER",
        "cat > /etc/systemd/system/openphysics-monitor.service <<'OPENPHYSICS_UNIT'", (UNIT % {"jobs": jobs}).rstrip("\n"),
        "OPENPHYSICS_UNIT",
        "python3 /opt/openphysics-monitor/node_sampler.py --once --jobs %s > /dev/null" % shlex.quote(jobs),
        "systemctl daemon-reload",
        "systemctl enable openphysics-monitor.service > /dev/null 2>&1",
        "systemctl restart openphysics-monitor.service",
        "sleep 7",
        'echo "recorder: $(systemctl is-active openphysics-monitor.service)"',
        "grep -c '\"kind\":\"sample\"' %s | sed 's/^/samples so far: /'" % LIVE,
        "tail -n 1 %s | cut -c1-300" % LIVE,
    ]) + "\n"
    print("%s:\n%s" % (name, remote.linux(n, script, root=True)))


def cmd_check(name, n, args):
    src = (HERE / "countertest.py").read_text()
    print(remote.linux(n, "python3 - <<'OPENPHYSICS_TEST'\n%s\nOPENPHYSICS_TEST\n" % src, check=False))


# ---------------------------------------------------------------- the minute records, kept in columns

class Day:
    """the minute records of one day file in columns (the array module: a few hundred kB a day, where the parsed
    lines took megabytes); a record's threads sit in cpu and cpk from off[i], nt[i] of them, and up to three
    processes in topn (an index into names, 65535 for none) and topc (their cores)"""
    COLS = ("t", "s", "cores", "busy", "umax", "flops", "ins", "cyc", "mem", "run", "gpu", "swap", "pmax", "away", "batt",
            "flops_est")
    OPTIONAL = ("gpu", "swap", "pmax", "away", "batt", "flops_est")  # NaN where a record has none (only this Mac has the rest)
    __slots__ = COLS + ("off", "nt", "cpu", "cpk", "names", "topn", "topc", "ordered")

    def __init__(self):
        for k in self.COLS:
            setattr(self, k, array.array("d"))
        self.off, self.nt = array.array("I"), array.array("B")
        self.cpu, self.cpk = array.array("f"), array.array("f")
        self.names, self.topn, self.topc = [], array.array("H"), array.array("f")
        self.ordered = True

    def add(self, r):
        t = float(r["t"])
        if self.t and t < self.t[-1]:
            self.ordered = False
        self.t.append(t)
        for k in ("s", "cores", "busy", "umax", "flops", "ins", "cyc", "mem", "run"):
            v = r.get(k)
            getattr(self, k).append(float(v) if isinstance(v, (int, float)) else 0.0)
        for k in self.OPTIONAL:
            v = r.get(k)
            getattr(self, k).append(float(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else math.nan)
        cpu, cpk = r.get("cpu"), r.get("cpk")
        self.off.append(len(self.cpu))
        if isinstance(cpu, list) and cpu and all(isinstance(x, (int, float)) for x in cpu):
            cpu = cpu[:255]
            cpk = cpk[:len(cpu)] if isinstance(cpk, list) and len(cpk) >= len(cpu) else cpu
            self.nt.append(len(cpu))
            self.cpu.extend(float(x) for x in cpu)
            self.cpk.extend(float(x) if isinstance(x, (int, float)) else 0.0 for x in cpk)
        else:
            self.nt.append(0)
        top = r.get("top") if isinstance(r.get("top"), list) else []
        for k in range(3):
            e = top[k] if k < len(top) else None
            if isinstance(e, list) and len(e) == 2 and isinstance(e[1], (int, float)):
                name = str(e[0])[:48]
                try:
                    j = self.names.index(name)
                except ValueError:
                    j = len(self.names)
                    self.names.append(name)
                self.topn.append(min(j, 65534))
                self.topc.append(float(e[1]))
            else:
                self.topn.append(65535)
                self.topc.append(0.0)

    def indices(self, t0, t1):
        if not self.ordered:
            return [i for i, t in enumerate(self.t) if t0 <= t < t1]
        return range(bisect.bisect_left(self.t, t0), bisect.bisect_left(self.t, t1))

    def record(self, i):
        """one record as the recorder wrote it (without its running totals)"""
        r = {"kind": "minute", "t": self.t[i]}
        for k in ("s", "busy", "umax", "flops", "ins", "cyc", "mem"):
            r[k] = getattr(self, k)[i]
        r["cores"], r["run"] = int(self.cores[i]), int(self.run[i])
        r["gpu"] = None if math.isnan(self.gpu[i]) else self.gpu[i]
        for k in ("swap", "pmax", "away", "batt", "flops_est"):
            if not math.isnan(getattr(self, k)[i]):
                r[k] = getattr(self, k)[i]
        for k in ("t", "flops", "ins", "cyc"):
            if r[k] == int(r[k]):
                r[k] = int(r[k])
        n, o = self.nt[i], self.off[i]
        if n:
            r["cpu"], r["cpk"] = list(self.cpu[o:o + n]), list(self.cpk[o:o + n])
        top = self.top(i)
        if top:
            r["top"] = top
        return r

    def top(self, i):
        return [[self.names[self.topn[3 * i + k]], self.topc[3 * i + k]] for k in range(3)
                if self.topn[3 * i + k] != 65535]


_days, _days_lock = collections.OrderedDict(), threading.Lock()


def read_day(path):
    """the minute records of one day file (a Day), parsed once while the file stays the same; None if there is none"""
    try:
        st = path.stat()
    except OSError:
        return None
    key, stamp = str(path), (st.st_mtime_ns, st.st_size)
    with _days_lock:
        hit = _days.get(key)
        if hit and hit[0] == stamp:
            _days.move_to_end(key)
            return hit[1]
    d = Day()
    with open(path) as f:
        for line in f:
            try:
                r = json.loads(line)
            except ValueError:
                continue
            if isinstance(r, dict) and r.get("kind") == "minute" and isinstance(r.get("t"), (int, float)):
                d.add(r)
    with _days_lock:
        _days[key] = (stamp, d)
        _days.move_to_end(key)
        while len(_days) > 256:  # a month of three machines, about 20 MB at most
            _days.popitem(last=False)
    return d


def read_json(path):
    try:
        return json.loads(pathlib.Path(path).read_text())
    except (OSError, ValueError):
        return None


def write_json(path, obj):
    tmp = str(path) + ".tmp"
    with open(tmp, "w") as f:
        json.dump(obj, f, separators=(",", ":"))
    os.replace(tmp, path)


def day_name(t):
    return time.strftime("%Y-%m-%d", time.gmtime(t)) + ".jsonl"


class Node:
    def __init__(self, name, cfg):
        self.name, self.cfg = name, cfg
        self.planned = bool(cfg.get("planned"))
        self.local = cfg.get("kind") == "local"
        self.lock = threading.Lock()
        self.state = "planned" if self.planned else "connecting"
        self.note = "not set up yet" if self.planned else ""
        self.since = time.time()
        self.last_line = self.stream_started = 0.0
        self.info = None
        self.samples = collections.deque(maxlen=1800)  # two-second samples, the last hour
        self.minutes = collections.deque(maxlen=720)  # minute records not yet in a day file on this Mac
        self.jobs = []
        self.totals = None
        self.showcase = None  # this Mac: the showcase runner's mode now, and its changes
        self.modes = []
        self.activity = None  # this Mac: seconds since the last input, on power or on battery
        self.estimate = None  # this Mac: how its floating-point operations are estimated (factor, source, lab cores)
        self.proc = None
        self.gw = None  # this Mac's network gateway while the node answered (None: not seen online in this run)
        self.gw_checked = 0.0
        self.bad_since = None
        self.alerted = False

    @property
    def store(self):
        return DATA / self.name

    def set(self, state, note=""):
        with self.lock:
            if state != self.state:
                self.since = time.time()
            self.state, self.note = state, note

    def add(self, rec):
        k = rec.get("kind")
        with self.lock:
            self.last_line = time.time()
            if k == "sample":
                if not self.samples or rec["t"] > self.samples[-1]["t"]:
                    self.samples.append(rec)
            elif k == "minute":
                tot = rec.get("tot")
                rec = {key: v for key, v in rec.items() if key != "tot"}  # the totals are kept once, not per minute
                if not self.minutes or rec["t"] > self.minutes[-1]["t"]:
                    self.minutes.append(rec)
                self._take_totals(tot)
            elif k == "info":
                self.info = rec
            elif k == "jobs":
                self.jobs = rec.get("jobs", [])

    def take_totals(self, tot):
        with self.lock:
            self._take_totals(tot)

    def _take_totals(self, tot):
        if tot and (not self.totals or (tot.get("upto") or 0) >= (self.totals.get("upto") or 0)):
            self.totals = tot

    def day_files(self, t0, t1):
        day = int(t0 // 86400) * 86400
        while day < t1:
            d = read_day(self.store / "minutes" / day_name(day))
            if d is not None:
                yield d
            day += 86400

    def minutes_range(self, t0, t1):
        recs = {}
        for d in self.day_files(t0, t1):
            for i in d.indices(t0, t1):
                recs[d.t[i]] = d.record(i)
        with self.lock:
            for r in self.minutes:
                if t0 <= r["t"] < t1:
                    recs[r["t"]] = r
        return [recs[k] for k in sorted(recs)]

    def minute_obs(self, t0, t1):
        """(t, s, cores, busy, umax, flops, mem, run, cpu, cpk, top, extra) for each minute record in [t0, t1), from
        the day files and the records not yet in one (extra: this Mac's swap, pmax, away, batt and estimated operations,
        or None); a minute recorded twice counts once"""
        seen = set()
        for d in self.day_files(t0, t1):
            for i in d.indices(t0, t1):
                t = d.t[i]
                if t in seen:
                    continue
                seen.add(t)
                n, o = d.nt[i], d.off[i]
                extra = None if math.isnan(d.swap[i]) and math.isnan(d.flops_est[i]) else tuple(
                    0.0 if math.isnan(v) else v for v in (d.swap[i], d.pmax[i], d.away[i], d.batt[i], d.flops_est[i]))
                yield (t, d.s[i], d.cores[i], d.busy[i], d.umax[i], d.flops[i], d.mem[i], d.run[i],
                       d.cpu[o:o + n] if n else None, d.cpk[o:o + n] if n else None, d.top(i), extra)
        with self.lock:
            extra = [r for r in self.minutes if t0 <= r["t"] < t1 and r["t"] not in seen]
        for r in extra:
            cpu = r.get("cpu") if isinstance(r.get("cpu"), list) and r.get("cpu") else None
            cpk = r.get("cpk") if cpu and isinstance(r.get("cpk"), list) and len(r["cpk"]) == len(cpu) else cpu
            extra = (r.get("swap") or 0, r.get("pmax") or 0, r.get("away") or 0, r.get("batt") or 0,
                     r.get("flops_est") or 0) if "swap" in r or "flops_est" in r else None
            yield (r["t"], r.get("s") or 0, r.get("cores") or 0, r.get("busy") or 0, r.get("umax") or 0,
                   r.get("flops") or 0, r.get("mem") or 0, r.get("run") or 0, cpu, cpk, r.get("top") or [], extra)

    def first_minute(self):
        try:
            days = sorted(p.name for p in (self.store / "minutes").glob("*.jsonl"))
        except OSError:
            days = []
        for name in days:
            d = read_day(self.store / "minutes" / name)
            if d is not None and len(d.t):
                return min(d.t)
        with self.lock:
            return self.minutes[0]["t"] if self.minutes else None


NODES = []


def find(name):
    return next((n for n in NODES if n.name == name), None)


# ---------------------------------------------------------------- following the nodes

def copy_history(node):
    """the node's minute files and totals, copied to this Mac (rsync only sends what is new)"""
    dest = DATA / node.name
    dest.mkdir(parents=True, exist_ok=True)
    rsh = " ".join(shlex.quote(a) for a in remote.ssh_base(node.cfg)[:-1])
    cmd = ["rsync", "-az", "--exclude=*.tmp", "-e", rsh]
    if node.cfg["kind"] == "windows-wsl":
        cmd.append("--rsync-path=wsl -d %s rsync" % node.cfg["distro"])
    cmd += ["%s@%s:%s/" % (node.cfg["user"], node.cfg["host"], STORE), str(dest) + "/"]
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=180).returncode == 0
    except subprocess.TimeoutExpired:
        return False


def load_history(node):
    """what this Mac already has of a node: its machine and its running totals (the minutes stay in their files)"""
    info = read_json(DATA / node.name / "info.json")
    with node.lock:
        if info and not node.info:
            node.info = info
    node.take_totals(read_json(DATA / node.name / "totals.json"))


def stream_cmd(cfg):
    base = remote.ssh_base(cfg)
    target = "wsl.exe -d %s -- bash -s" % cfg["distro"] if cfg["kind"] == "windows-wsl" else "bash -s"
    return base[:-1] + ["-o", "ServerAliveCountMax=3"] + [base[-1], target]


def follow(node):
    """one SSH connection that follows the node's live lines; reconnects after a sleep, an outage or a restart"""
    wait = 5
    errlog = DATA / node.name / "stream.err"
    while True:
        if not remote.reachable(node.cfg):
            node.set("offline", "not reachable on the network")
            time.sleep(wait)
            wait = min(2 * wait, 60)
            continue
        if node.state != "online":
            node.set("connecting")
        copy_history(node)
        load_history(node)
        errlog.parent.mkdir(parents=True, exist_ok=True)
        with open(errlog, "w") as err:
            p = subprocess.Popen(stream_cmd(node.cfg), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=err,
                                 text=True, bufsize=1)
            node.proc, node.stream_started = p, time.time()
            p.stdin.write("exec tail -n 1800 -F %s 2>/dev/null\n" % LIVE)
            p.stdin.flush()
            last_copy = time.time()
            for line in p.stdout:
                if not line.startswith("{"):
                    continue
                try:
                    rec = json.loads(line)
                except ValueError:
                    continue
                node.add(rec)
                if node.state != "online" and rec.get("kind") == "sample" and time.time() - rec["t"] < 30:
                    node.set("online")
                    wait = 5
                if time.time() - last_copy > 600:
                    last_copy = time.time()
                    threading.Thread(target=copy_history, args=(node,), daemon=True).start()
            p.wait()
        node.proc = None
        why = remote.clean(errlog.read_text()) if errlog.exists() else ""
        if node.state != "silent":
            node.set("offline", why[-240:] or "the connection closed")
        time.sleep(wait)
        wait = min(2 * wait, 60)


def notify(title, text):
    """a notification on this Mac, in Notification Center"""
    try:
        subprocess.Popen(["osascript", "-e", "display notification %s with title %s" % (json.dumps(text), json.dumps(title))],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError:
        pass


def gateway():
    r = subprocess.run(["route", "-n", "get", "default"], capture_output=True, text=True)
    return next((l.split()[-1] for l in r.stdout.splitlines() if "gateway:" in l), None)


def watchdog():
    """a connection that has gone quiet (this Mac slept, the network dropped) is closed, so that it reconnects; a node
    that stops answering for over two and a half minutes while this Mac stays awake on the same network is announced
    once, and again when it is back (so a Mac that wakes elsewhere, or after a sleep, raises no alarm)"""
    while True:
        time.sleep(5)
        now = time.time()
        for n in list(NODES):
            if n.local:  # this Mac is sampled here; it cannot stop answering to itself
                continue
            p = n.proc
            if p and p.poll() is None and now - max(n.last_line, n.stream_started) > 30:
                n.set("silent", "connected, but nothing from its recorder for 30 s")
                p.kill()
            label = n.cfg.get("label", n.name)
            if n.state == "online":
                n.bad_since = None
                if now - n.gw_checked > 60:
                    n.gw, n.gw_checked = gateway(), now
                if n.alerted:
                    notify("OpenPhysicsAI compute", "%s is back and answering." % label)
                    n.alerted = False
            elif not n.planned and n.gw:
                n.bad_since = n.bad_since or now
                if not n.alerted and now - n.bad_since > 150 and now - n.last_line > 150 and gateway() == n.gw:
                    with n.lock:
                        last = n.samples[-1] if n.samples else None
                    busy = ", %d%% busy just before" % round(100 * last["util"]) if last else ""
                    notify("OpenPhysicsAI compute", "%s stopped answering at %s%s." % (
                        label, time.strftime("%H:%M", time.localtime(n.last_line)), busy))
                    n.alerted = True


# ---------------------------------------------------------------- this Mac, sampled here

class MacProbe:
    """this Mac's own counters, read through documented macOS interfaces with ctypes, without SSH and without root:
    each core's ticks (user, system, idle, nice) from the Mach call host_processor_info(PROCESSOR_CPU_LOAD_INFO);
    memory in use from host_statistics64(HOST_VM_INFO64), counted as Activity Monitor counts it (app memory, wired,
    compressed); the sizes from sysctlbyname(3) (hw.memsize, hw.pagesize, hw.perflevel0 and 1, the processor's name);
    the load average from getloadavg(3); each core's kind (efficiency or performance) once from the device tree
    (ioreg); and the processes using the cores from ps(1) every ten seconds. macOS gives ordinary programs no
    floating-point or instruction counters, so the Mac's operations are not counted, and the page says so."""

    def __init__(self):
        L = self.lib = ctypes.CDLL("/usr/lib/libSystem.B.dylib", use_errno=True)
        u32, p32 = ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)
        L.mach_host_self.restype = u32
        L.host_processor_info.argtypes = [u32, ctypes.c_int, p32, ctypes.POINTER(ctypes.POINTER(ctypes.c_int32)), p32]
        L.host_processor_info.restype = ctypes.c_int
        L.host_statistics64.argtypes = [u32, ctypes.c_int, ctypes.c_void_p, p32]
        L.host_statistics64.restype = ctypes.c_int
        L.vm_deallocate.argtypes = [u32, ctypes.c_size_t, ctypes.c_size_t]
        L.sysctlbyname.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t), ctypes.c_void_p,
                                   ctypes.c_size_t]
        self.host = L.mach_host_self()
        self.task = ctypes.c_uint32.in_dll(L, "mach_task_self_").value
        self.page = self.sysctl_int("hw.pagesize") or 16384
        self.memsize = self.sysctl_int("hw.memsize") or 0
        self.ncpu = len(self.ticks())
        self.perf = self.sysctl_int("hw.perflevel0.logicalcpu")
        self.eff = self.sysctl_int("hw.perflevel1.logicalcpu") if (self.sysctl_int("hw.nperflevels") or 1) > 1 else 0
        self.cpu = self.sysctl_str("machdep.cpu.brand_string") or platform.processor() or "this Mac"
        self.kinds = self.core_kinds()
        self.lab_init()

    def sysctl_raw(self, name):
        size = ctypes.c_size_t(0)
        if self.lib.sysctlbyname(name.encode(), None, ctypes.byref(size), None, 0) != 0 or not size.value:
            return None
        buf = ctypes.create_string_buffer(size.value)
        if self.lib.sysctlbyname(name.encode(), buf, ctypes.byref(size), None, 0) != 0:
            return None
        return buf.raw[:size.value]

    def sysctl_int(self, name):
        b = self.sysctl_raw(name)
        return int.from_bytes(b, "little") if b else None

    def sysctl_str(self, name):
        b = self.sysctl_raw(name)
        return b.split(b"\0")[0].decode(errors="replace").strip() if b else None

    def core_kinds(self):
        """E or P for each logical CPU, from the device tree's cluster-type; None where it does not say"""
        kinds = {}
        try:
            out = subprocess.run(["ioreg", "-l", "-p", "IODeviceTree", "-w0", "-r", "-n", "cpus"], capture_output=True,
                                 text=True, timeout=10).stdout
            cur = None
            for line in out.splitlines():
                m = re.search(r"\+-o cpu(\d+)@", line)
                if m:
                    cur = {"id": int(m.group(1))}
                    continue
                if cur is None:
                    continue
                m = re.search(r'"cluster-type" = <"(\w)">', line)
                if m:
                    cur["kind"] = m.group(1)
                m = re.search(r'"logical-cpu-id" = (\d+)', line)
                if m:
                    cur["id"] = int(m.group(1))
                if "kind" in cur:
                    kinds[cur["id"]] = cur["kind"]
        except (OSError, subprocess.SubprocessError):
            pass
        if len(kinds) == self.ncpu:
            return [kinds.get(i) for i in range(self.ncpu)]
        if self.eff and self.perf and self.eff + self.perf == self.ncpu:  # Apple silicon numbers its E cores first
            return ["E"] * self.eff + ["P"] * self.perf
        return None

    def ticks(self):
        n, cnt = ctypes.c_uint32(0), ctypes.c_uint32(0)
        info = ctypes.POINTER(ctypes.c_int32)()
        kr = self.lib.host_processor_info(self.host, 2, ctypes.byref(n), ctypes.byref(info), ctypes.byref(cnt))
        if kr != 0:
            raise OSError("host_processor_info returned %d" % kr)
        try:
            return [tuple(info[4 * i + k] & 0xFFFFFFFF for k in range(4)) for i in range(n.value)]
        finally:
            self.lib.vm_deallocate(self.task, ctypes.cast(info, ctypes.c_void_p).value, cnt.value * 4)

    class VM(ctypes.Structure):  # struct vm_statistics64, <mach/vm_statistics.h>
        _fields_ = ([(k, ctypes.c_uint32) for k in ("free", "active", "inactive", "wire")]
                    + [(k, ctypes.c_uint64) for k in ("zero_fill", "reactivations", "pageins", "pageouts", "faults",
                                                       "cow_faults", "lookups", "hits", "purges")]
                    + [(k, ctypes.c_uint32) for k in ("purgeable", "speculative")]
                    + [(k, ctypes.c_uint64) for k in ("decompressions", "compressions", "swapins", "swapouts")]
                    + [(k, ctypes.c_uint32) for k in ("compressor", "throttled", "external", "internal")]
                    + [("uncompressed_in_compressor", ctypes.c_uint64)])

    def memory(self):
        vm, cnt = self.VM(), ctypes.c_uint32(ctypes.sizeof(self.VM) // 4)
        if self.lib.host_statistics64(self.host, 4, ctypes.byref(vm), ctypes.byref(cnt)) != 0:
            raise OSError("host_statistics64 failed")
        used = (max(0, vm.internal - vm.purgeable) + vm.wire + vm.compressor) * self.page
        return used, self.memsize

    def sysctl_i32(self, name):
        v, n = ctypes.c_int32(0), ctypes.c_size_t(4)
        return v.value if self.lib.sysctlbyname(name.encode(), ctypes.byref(v), ctypes.byref(n), None, 0) == 0 else None

    class XSW(ctypes.Structure):  # struct xsw_usage, <sys/sysctl.h>
        _fields_ = [("total", ctypes.c_uint64), ("avail", ctypes.c_uint64), ("used", ctypes.c_uint64),
                    ("pagesize", ctypes.c_uint32), ("encrypted", ctypes.c_int32)]

    def pressure(self):
        """memory pressure as macOS judges it: (level, share of memory free, swap used, swap size, compressed); level
        is kern.memorystatus_vm_pressure_level (1 normal, 2 warning, 4 critical), the share free is
        kern.memorystatus_level (what memory_pressure(1) prints), swap from vm.swapusage, in bytes"""
        x, n = self.XSW(), ctypes.c_size_t(ctypes.sizeof(self.XSW))
        ok = self.lib.sysctlbyname(b"vm.swapusage", ctypes.byref(x), ctypes.byref(n), None, 0) == 0
        return (self.sysctl_i32("kern.memorystatus_vm_pressure_level"), self.sysctl_i32("kern.memorystatus_level"),
                x.used if ok else None, x.total if ok else None)

    def idle(self):
        """seconds since the last keyboard, mouse or trackpad input, from CoreGraphics'
        CGEventSourceSecondsSinceLastEventType (the session's HID state); None where there is no session"""
        try:
            if not hasattr(self, "_idle"):
                cg = ctypes.CDLL("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")
                self._idle = cg.CGEventSourceSecondsSinceLastEventType
                self._idle.restype, self._idle.argtypes = ctypes.c_double, [ctypes.c_int32, ctypes.c_uint32]
            v = self._idle(1, 0xFFFFFFFF)
            return v if 0 <= v < 1e9 else None
        except (OSError, AttributeError):
            return None

    @staticmethod
    def power():
        """on mains or on battery, and the battery's charge, from pmset(1)"""
        try:
            out = subprocess.run(["pmset", "-g", "batt"], capture_output=True, text=True, timeout=5).stdout
        except (OSError, subprocess.SubprocessError):
            return None
        m = re.search(r"(\d+)%", out)
        return {"ac": "AC Power" in out, "pct": int(m.group(1)) if m else None,
                "charging": bool(re.search(r"; charging", out))}

    # this user's processes, each with its own instruction count (proc_pid_rusage, RUSAGE_INFO_V6, no root): the
    # lab's programs are the ones whose instructions the estimate of floating-point operations takes
    LAB_STEMS = ("labrun", "propulsion", "openphysics", "navier", "ccx")  # and their variants: openphysics-mcp, labrun_diag, ...
    # (navier: the programs' names before the rename of 2026-10-21, still run through their aliases for one release)
    RU = {"user": 0, "system": 1, "start": 8, "ins": 29, "cyc": 30, "pins": 38}  # uint64 fields after the 16-byte uuid

    def lab_init(self):
        L = self.lib
        L.proc_listallpids.argtypes, L.proc_listallpids.restype = [ctypes.c_void_p, ctypes.c_int], ctypes.c_int
        L.proc_pid_rusage.argtypes, L.proc_pid_rusage.restype = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p], ctypes.c_int
        L.proc_pidpath.argtypes, L.proc_pidpath.restype = [ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32], ctypes.c_int

        class Timebase(ctypes.Structure):
            _fields_ = [("numer", ctypes.c_uint32), ("denom", ctypes.c_uint32)]
        tb = Timebase()
        L.mach_timebase_info(ctypes.byref(tb))
        self.tick_s = tb.numer / tb.denom / 1e9 if tb.denom else 1e-9  # Mach ticks to seconds (125/3 ns on Apple silicon)
        self.rbuf = (ctypes.c_uint64 * 128)()  # more than struct rusage_info_v6 needs
        self.pbuf = ctypes.create_string_buffer(4096)
        self.lab_root = re.compile(r"^(%s)/build/" % "|".join(re.escape(c) for c in lab_checkouts()))
        self.kinds_of = {}  # (pid, start): is it one of the lab's programs
        self.seen = {}      # pid: (start, cpu ticks, instructions, on performance cores)

    def is_lab(self, path):
        name = os.path.basename(path)
        if any(name == x or name.startswith(x + "-") or name.startswith(x + "_") for x in self.LAB_STEMS):
            return True
        return bool(re.search(r"test(_[\w.-]+)?$", name) or self.lab_root.match(path))

    def lab_usage(self):
        """instructions and CPU time of this user's processes since the last call, one pass over the process list:
        {ins_all, ins_lab, pins_lab, cpu_lab (s)}. A process is counted from its second sighting on, and an exited one
        up to its last, so what is not seen is left out (the estimate built on it can only fall short)"""
        L, r, k = self.lib, self.rbuf, self.RU
        n = L.proc_listallpids(None, 0)
        pids = (ctypes.c_int * (max(n, 0) + 64))()
        n = L.proc_listallpids(pids, ctypes.sizeof(pids))
        out = {"ins_all": 0, "ins_lab": 0, "pins_lab": 0, "cpu_lab": 0.0}
        seen = {}
        for i in range(max(0, n)):
            pid = pids[i]
            if pid <= 0 or L.proc_pid_rusage(pid, 6, r) != 0:  # RUSAGE_INFO_V6; another user's process says no
                continue
            start, cpu, ins, pins = r[2 + k["start"]], r[2 + k["user"]] + r[2 + k["system"]], r[2 + k["ins"]], r[2 + k["pins"]]
            seen[pid] = (start, cpu, ins, pins)
            q = self.seen.get(pid)
            if not q or q[0] != start or ins < q[2]:
                continue
            d = ins - q[2]
            out["ins_all"] += d
            if not d:
                continue
            lab = self.kinds_of.get((pid, start))
            if lab is None:
                m = L.proc_pidpath(pid, self.pbuf, 4096)
                lab = self.kinds_of[(pid, start)] = self.is_lab(self.pbuf.value.decode(errors="replace")) if m > 0 else False
            if lab:
                out["ins_lab"] += d
                out["pins_lab"] += max(0, pins - q[3])
                out["cpu_lab"] += max(0, cpu - q[1]) * self.tick_s
        self.seen = seen
        if len(self.kinds_of) > 4 * len(seen) + 256:
            self.kinds_of = {key: v for key, v in self.kinds_of.items() if seen.get(key[0], (None,))[0] == key[1]}
        return out

    @staticmethod
    def cpu_seconds(s):
        """ps(1)'s accumulated CPU time, [[dd-]hh:]mm:ss.ss, in seconds"""
        days, _, s = s.rpartition("-")
        v = 0.0
        for part in s.split(":"):
            v = 60 * v + float(part)
        return v + (86400 * int(days) if days.isdigit() else 0)

    def processes(self):
        """pid: (name, CPU seconds so far)"""
        out = {}
        try:
            r = subprocess.run(["ps", "-Ao", "pid=,time=,comm="], capture_output=True, text=True, timeout=10)
        except (OSError, subprocess.SubprocessError):
            return out
        for line in r.stdout.splitlines():
            p = line.split(None, 2)
            if len(p) == 3:
                try:
                    out[int(p[0])] = (os.path.basename(p[2].strip()) or p[2].strip(), self.cpu_seconds(p[1]))
                except ValueError:
                    pass
        return out

    def about(self):
        parts = [self.cpu]
        if self.perf and self.eff:
            parts.append("%dP + %dE cores" % (self.perf, self.eff))  # performance and efficiency cores
        else:
            parts.append("%d cores" % self.ncpu)
        if self.memsize:
            parts.append("%d GB" % round(self.memsize / 2**30))
        return " · ".join(parts + ["this Mac"])

    def info(self):
        return {"kind": "info", "version": 1, "host": socket.gethostname(), "cpu": self.cpu, "threads": self.ncpu,
                "cores": self.ncpu, "perf_cores": self.perf, "eff_cores": self.eff, "core_kinds": self.kinds,
                "memory": self.memsize, "gpu": None, "kernel": platform.release(),
                "os": "macOS %s" % platform.mac_ver()[0], "local": True,
                "counters": "not counted: macOS gives ordinary programs no floating-point counters; estimated from the "
                            "lab programs' own instruction counts (proc_pid_rusage)"}


# ---------------------------------------------------------------- the showcase runner's log on this Mac

MODE_WHY = {"full": "you are away", "capped": "you are using it", "paused": "on battery"}
AWAY_AFTER = 120  # seconds without keyboard, mouse or trackpad: the owner is away from this Mac


def when(r, *keys):
    """a moment from the first of keys present: Unix seconds (milliseconds are recognised) or an ISO 8601 time"""
    for k in keys:
        v = r.get(k)
        if isinstance(v, bool):
            continue
        if isinstance(v, (int, float)) and v > 0:
            return v / 1000.0 if v > 1e12 else float(v)
        if isinstance(v, str) and v.strip():
            s = v.strip()
            try:
                d = datetime.datetime.fromisoformat(s[:-1] + "+00:00" if s.endswith("Z") else s)
                return d.timestamp()
            except ValueError:
                try:
                    return float(s)
                except ValueError:
                    continue
    return None


def first_text(r, *keys):
    for k in keys:
        v = r.get(k)
        if isinstance(v, (str, int, float)) and not isinstance(v, bool) and str(v).strip():
            return str(v).strip()
    return None


def norm_mode(m):
    m = m.strip().lower()
    if "full" in m:
        return "full"
    if "cap" in m or "limit" in m or "throttl" in m:
        return "capped"
    if "pause" in m or "battery" in m or "stop" in m:
        return "paused"
    return m[:24]


class ShowcaseLog:
    """the showcase runner's log on this Mac (~/.openphysicsai/showcase-mac/log.jsonl), read when it is there and
    again when it changes: one JSON object a line. A line with "mode" is a change of the runner's mode (full speed
    while the owner is away and on mains, capped while he uses the Mac, paused on battery), with its time ("t",
    "time", "ts" or "at") and its reason ("why" or "reason"). A line naming a run ("id", "run", "job", "scenario" or
    "name") gives its start ("started"/"start"), end ("ended"/"end"), outcome ("state", "status", "event", "outcome",
    "result", "exit") and command ("cmd"/"command"). The runner is written separately, so the keys are read
    tolerantly and a line not understood is skipped."""

    def __init__(self, path):
        self.path, self.stamp = path, None
        self.modes, self.jobs = [], []

    def refresh(self):
        try:
            st = self.path.stat()
        except OSError:
            self.stamp, self.modes, self.jobs = None, [], []
            return
        stamp = (st.st_mtime_ns, st.st_size)
        if stamp == self.stamp:
            return
        self.stamp = stamp
        modes, runs, order = [], {}, []
        try:
            with open(self.path, errors="replace") as f:
                if st.st_size > 4 << 20:  # the last 4 MB are plenty
                    f.seek(st.st_size - (4 << 20))
                    f.readline()
                for line in f:
                    try:
                        r = json.loads(line)
                    except ValueError:
                        continue
                    if isinstance(r, dict):
                        self.take(r, modes, runs, order)
        except OSError:
            return
        modes.sort(key=lambda m: m[0])
        jobs = [runs[k] for k in order]
        running = [j for j in jobs if j["state"] == "running"]
        self.modes = modes[-5000:]
        self.jobs = running + [j for j in jobs if j["state"] != "running"][-10:]

    @staticmethod
    def take(r, modes, runs, order):
        t = when(r, "t", "time", "ts", "at", "timestamp", "date")
        m = r.get("mode")
        if isinstance(m, str) and m.strip() and t:
            mode = norm_mode(m)
            modes.append((t, mode, first_text(r, "why", "reason", "because", "note") or MODE_WHY.get(mode, "")))
        name = first_text(r, "scenario", "name", "title", "job", "run", "id")
        if not name or (isinstance(m, str) and not any(k in r for k in ("started", "start", "ended", "end", "outcome",
                                                                          "status", "state", "exit", "result"))):
            return
        start = when(r, "started", "start", "begin", "t0")
        key = first_text(r, "id", "run", "job") or "%s@%s" % (name, start or t)
        j = runs.get(key)
        if j is None:
            j = runs[key] = {"id": name, "lane": "showcase", "state": "running", "started": start or t, "cmd": ""}
            order.append(key)
        if start:
            j["started"] = start
        end = when(r, "ended", "end", "finished", "t1", "stop", "stopped")
        words = " ".join(str(r.get(k, "")) for k in ("state", "status", "event", "outcome", "result")).lower()
        code = r.get("exit", r.get("exit_code", r.get("code", r.get("rc", r.get("returncode")))))
        if isinstance(code, bool) or not isinstance(code, int):
            code = None
        failed = code not in (None, 0) or re.search(
            r"\b(fail(ed|ure)?|error|crash(ed)?|time(d)? ?out|kill(ed)?|abort(ed)?|lost)\b", words)
        finished = end or code is not None or failed or re.search(
            r"\b(done|ok|okay|pass(ed)?|finish(ed)?|complete(d)?|success(ful)?|succeeded|end(ed)?|exit(ed)?)\b", words)
        if finished:
            j["state"] = "failed" if failed else "done"
            j["ended"] = end or t or j.get("ended")
            j["exit"] = code if code is not None else (1 if failed else 0)
        elif re.search(r"start|run|begin", words):
            j["state"] = "running"
        secs = r.get("seconds", r.get("duration", r.get("elapsed", r.get("wall_s"))))
        if isinstance(secs, (int, float)) and not isinstance(secs, bool):
            j["seconds"] = round(secs)
        elif j.get("ended") and j.get("started"):
            j["seconds"] = round(j["ended"] - j["started"])
        cpu = r.get("cpu_s", r.get("cpu_time"))
        if isinstance(cpu, (int, float)) and not isinstance(cpu, bool):
            j["cpu_s"] = cpu
        cmd = first_text(r, "cmd", "command", "scenario", "what")
        if cmd:
            j["cmd"] = cmd[:160]

    def now(self):
        if not self.modes:
            return None
        t, mode, why = self.modes[-1]
        return {"mode": mode, "since": t, "why": why}


# The Mac's floating-point operations, estimated so as to fall short rather than over: FLOP_PER_INSTRUCTION of every
# instruction its lab programs retire. The nodes' own counters give 0.78 operations per instruction on the desktop (the
# geometry check) and 0.41 on the Surface (finite elements); the M2's vectors are 128 bits against the nodes' 256, so
# vector code needs up to twice the instructions for the same operations: 0.41 / 2 = 0.2. Python and everything else
# are left out. A ratio the benchmark measures on the Mac (bench.json, machines.mac.flop_per_instruction_min) wins.
FLOP_PER_INSTRUCTION = 0.2


def mac_factor():
    b = read_json(DATA / "bench.json")
    try:
        v = float(b["machines"]["mac"]["flop_per_instruction_min"])
        if 0 < v < 64:
            return v, "bench"
    except (TypeError, KeyError, ValueError):
        pass
    return FLOP_PER_INSTRUCTION, "rule"


def mac_loop(node, probe, showcase):
    """this Mac's samples every two seconds and its minute records, written like a node's (minutes/DAY.jsonl,
    totals.json, info.json), so its days and weeks show as theirs do; in read-only mode they stay in memory"""
    store = DATA / node.name
    writes = not OPTS["read_only"]
    info = probe.info()
    totals = read_json(store / "totals.json")
    if not isinstance(totals, dict) or "s" not in totals:
        totals = {"since": time.time(), "upto": None, "s": 0.0, "busy": 0.0, "flops": 0.0, "ins": 0.0, "cyc": 0.0,
                  "peak_flops": 0.0, "minutes": 0}
    if writes:
        (store / "minutes").mkdir(parents=True, exist_ok=True)
        write_json(store / "info.json", info)
    with node.lock:
        node.info = info
    node.take_totals(dict(totals))
    node.set("online")
    prev, prev_t = probe.ticks(), time.time()
    procs_prev, procs_t, top, running = probe.processes(), time.time(), [], 0
    minute, n, idle, power, mx = None, 0, None, None, None
    factor, source = mac_factor()
    probe.lab_usage()  # the first sighting of every process: counting starts with the next sample
    next_wake = time.monotonic() + PERIOD
    while True:
        time.sleep(max(0.0, next_wake - time.monotonic()))
        next_wake += PERIOD
        if next_wake < time.monotonic():  # this Mac slept: start the rhythm again
            next_wake = time.monotonic() + PERIOD
        try:
            t, ticks = time.time(), probe.ticks()
            dt = t - prev_t
            if dt <= 0 or dt > 10 * PERIOD:  # a gap (a sleep): count nothing across it
                prev, prev_t = ticks, t
                continue
            cpu = []
            for a, b in zip(ticks, prev):
                d = [(x - y) % 2**32 for x, y in zip(a, b)]  # user, system, idle, nice
                tot = sum(d)
                cpu.append(round((d[0] + d[1] + d[3]) / tot, 3) if tot > 0 else 0.0)
            used, total = probe.memory()
            level, free_pct, swap_used, swap_total = probe.pressure()
            sample = {"kind": "sample", "t": round(t, 2), "dt": round(dt, 3), "util": round(sum(cpu) / len(cpu), 4),
                      "cpu": cpu, "steal": 0.0, "mem": [round(used / 2**30, 3), round(total / 2**30, 3)],
                      "load": round(os.getloadavg()[0], 2)}
            if swap_used is not None:
                sample["swap"] = [round(swap_used / 2**30, 3), round(swap_total / 2**30, 3)]
            if level is not None:
                sample["pressure"] = level
            if free_pct is not None:
                sample["free_pct"] = free_pct
            if n % 5 == 0:
                idle = probe.idle()
            if n % 15 == 0:
                power = probe.power() or power
            if idle is not None:
                sample["idle"] = round(idle)
            if power:
                sample["power"] = power
            if n % 30 == 0:
                factor, source = mac_factor()
            u = probe.lab_usage()
            est = factor * u["ins_lab"]
            sample.update({"flops_est": round(est), "ins_lab": u["ins_lab"], "ins_user": u["ins_all"],
                           "lab_cores": round(u["cpu_lab"] / dt, 2),
                           "lab_p": round(u["pins_lab"] / u["ins_lab"], 2) if u["ins_lab"] else None})
            n += 1
            if n % 5 == 1:  # the processes, the runner's log and its jobs every ten seconds
                procs = probe.processes()
                span = max(t - procs_t, 1e-3)
                by_name = {}
                for pid, (name, sec) in procs.items():
                    q = procs_prev.get(pid)
                    if q and q[0] == name and sec >= q[1]:
                        use = (sec - q[1]) / span
                        if use > 0.02:
                            by_name[name] = by_name.get(name, 0.0) + use
                top = sorted(([k, round(v, 2)] for k, v in by_name.items()), key=lambda x: -x[1])[:4]
                procs_prev, procs_t = procs, t
                showcase.refresh()
                running = sum(1 for j in showcase.jobs if j["state"] == "running")
                with node.lock:
                    node.jobs = list(showcase.jobs)
                    node.showcase = showcase.now()
                    node.modes = list(showcase.modes)
            sample["top"] = top
            sample["run"] = running
            node.add(sample)
            with node.lock:
                node.activity = {"idle": sample.get("idle"), "power": power, "pressure": sample.get("pressure"),
                                 "free_pct": free_pct, "swap": sample.get("swap"), "t": sample["t"]}
                node.estimate = {"factor": factor, "source": source, "lab_cores": sample["lab_cores"],
                                 "lab_p": sample["lab_p"]}
            if node.state != "online":
                node.set("online")
            m0 = int(t // 60) * 60
            if minute and minute.t != m0:
                rec = minute.record(running)
                if mx and mx["s"] > 0:  # this Mac's own: swap in use, the worst pressure, time away, time on battery,
                    rec.update({"swap": round(mx["swap"] / mx["s"], 3), "pmax": mx["pmax"],  # and the estimate
                                "away": round(mx["away"] / mx["s"], 2), "batt": round(mx["batt"] / mx["s"], 2),
                                "flops_est": round(mx["est"]), "ins_lab": mx["ins_lab"], "ins_user": mx["ins_user"],
                                "est": factor})
                for k in ("s", "busy"):
                    totals[k] += rec[k]
                for k in ("flops_est", "ins_lab"):
                    totals[k] = totals.get(k, 0) + rec.get(k, 0)
                if rec["s"] > 30 and rec.get("flops_est"):
                    totals["peak_flops_est"] = max(totals.get("peak_flops_est", 0), rec["flops_est"] / rec["s"])
                totals["minutes"] += 1
                totals["upto"] = minute.t + 60
                rec["tot"] = dict(totals)
                if writes:
                    with open(store / "minutes" / day_name(minute.t), "a") as f:
                        f.write(json.dumps(rec, separators=(",", ":")) + "\n")
                    write_json(store / "totals.json", totals)
                node.add(rec)
                minute = None
            if minute is None:
                minute = Minute(m0, len(cpu))
                mx = {"s": 0.0, "swap": 0.0, "pmax": 0, "away": 0.0, "batt": 0.0, "est": 0.0, "ins_lab": 0, "ins_user": 0}
            minute.add(dt, sum(cpu) * dt, sample)
            mx["s"] += dt
            mx["swap"] += (sample.get("swap") or [0])[0] * dt
            mx["pmax"] = max(mx["pmax"], sample.get("pressure") or 0)
            mx["away"] += dt if (sample.get("idle") or 0) >= AWAY_AFTER else 0.0
            mx["batt"] += dt if power and not power.get("ac", True) else 0.0
            mx["est"] += est
            mx["ins_lab"] += u["ins_lab"]
            mx["ins_user"] += u["ins_all"]
            prev, prev_t = ticks, t
        except Exception as e:  # keep sampling; say what went wrong on the page
            node.set("offline", "this Mac's sampler: %s" % e)
            time.sleep(10)
            try:
                prev, prev_t, minute = probe.ticks(), time.time(), None
            except Exception:
                pass


def mac_node():
    try:
        probe = MacProbe()
    except (OSError, AttributeError, ValueError) as e:
        print("monitor: this Mac is left off the page (%s)" % e, flush=True)
        return None
    cfg = {"label": "MacBook", "about": probe.about(), "kind": "local", "threads": probe.ncpu,
           "memory_gb": round(probe.memsize / 2**30) if probe.memsize else None}
    node = Node("mac", cfg)
    load_history(node)
    threading.Thread(target=mac_loop, args=(node, probe, ShowcaseLog(SHOWCASE_LOG)), daemon=True).start()
    return node


# ---------------------------------------------------------------- what each machine did, from its records

def summary(node, t0, t1):
    recs = node.minutes_range(t0, t1)
    s = sum(r["s"] for r in recs)
    cap = sum(r["cores"] * r["s"] for r in recs)
    out = {"since": t0, "until": t1, "covered": s, "minutes": len(recs), "cores": recs[-1]["cores"] if recs else None,
           "upto": recs[-1]["t"] + 60 if recs else t0}
    for k in ("busy", "flops", "ins", "cyc"):
        out[k] = sum(r[k] for r in recs)
    out["util"] = out["busy"] / cap if cap else None
    out["peak_flops"] = max((r["flops"] / r["s"] for r in recs if r["s"] > 30), default=0)
    out["ghz"] = out["cyc"] / out["busy"] / 1e9 if out["busy"] > 1 else None
    out["ipc"] = out["ins"] / out["cyc"] if out["cyc"] else None
    with node.lock:
        jobs = list(node.jobs)
    out["jobs_ended"] = [j["id"] for j in jobs if j.get("ended") and t0 <= j["ended"] < t1]
    if any("flops_est" in r for r in recs):  # this Mac: estimated, never added to the measured operations
        out["flops_est"] = sum(r.get("flops_est", 0) for r in recs)
    return out


def series(node, t0, t1, points=480):
    """minute records in buckets: [middle, busy share, share of the bucket recorded, operations/s, instructions/s,
    busiest sample]; a bucket with no record is a gap (the machine off, or its recorder down)"""
    width = max(60, int(math.ceil((t1 - t0) / points / 60.0)) * 60)
    n = int(math.ceil((t1 - t0) / width))
    acc = [None] * n
    for r in node.minutes_range(t0, t1):
        i = int((r["t"] - t0) // width)
        if 0 <= i < n and r["s"] > 0:
            a = acc[i] or [0.0] * 7
            a[0] += r["s"]
            a[1] += r["busy"]
            a[2] += r["flops"]
            a[3] += r["ins"]
            a[4] = max(a[4], r["umax"])
            a[5] += r["cores"] * r["s"]
            acc[i] = a
    pts = []
    for i, a in enumerate(acc):
        mid = round(t0 + (i + 0.5) * width)
        if not a:
            pts.append([mid, None, 0, None, None, None])
        else:
            pts.append([mid, round(a[1] / a[5], 4), round(min(1.0, a[0] / width), 3), round(a[2] / a[0]),
                        round(a[3] / a[0]), round(a[4], 3)])
    return {"width": width, "points": pts}


def heat(samples):
    """each thread's busy share in ten-second columns over the last hour"""
    if not samples:
        return []
    start = samples[-1]["t"] - 3600
    cols = {}
    for s in samples:
        if s["t"] >= start:
            c = cols.setdefault(int((s["t"] - start) // 10), [0, [0.0] * len(s["cpu"])])
            c[0] += 1
            for i, u in enumerate(s["cpu"][:len(c[1])]):
                c[1][i] += u
    return [[round(start + (k + 0.5) * 10), [round(v / c[0], 2) for v in c[1]]] for k, c in sorted(cols.items())]


def counted(node):
    """does this machine count floating-point operations? (the nodes' processor counters; not this Mac)"""
    info = node.info or {}
    if node.local:
        return False
    if info.get("counters"):
        return info["counters"] == "measured"
    return True


def threads_view(node, t0, t1, bins):
    """each thread's busy share from t0 to t1 in `bins` columns (the page's pixel columns), with the machine's busy
    share, operations, memory and processes in the same columns. A column takes its share of every record that
    overlaps it, weighted by the time they share: the minute records the machine keeps (each thread's share and
    peak since the recorder's version 2, before that the machine's share alone) and the two-second samples of the
    last hour, which fill in the minutes not yet in a record, give each thread where an older record does not, and
    are used alone where a column is shorter than a minute. Peaks are the busiest two-second sample (or minute)."""
    t1 = max(t1, t0 + 10)
    bins = int(max(1, min(bins, 2000, (t1 - t0) // PERIOD)))
    w = (t1 - t0) / bins
    fine = w < 60
    with node.lock:
        samples = [s for s in node.samples if s["t"] > t0 and s["t"] - s.get("dt", PERIOD) < t1]
        info = dict(node.info or {})
        modes = list(node.modes)
    count_flops = counted(node)

    sec, cap, busy, bmax = [0.0] * bins, [0.0] * bins, [0.0] * bins, [None] * bins
    fl, flsec, flmax = [0.0] * bins, [0.0] * bins, [None] * bins
    mem, memsec, run = [0.0] * bins, [0.0] * bins, [None] * bins
    swap, swsec, pres, away, batt = [0.0] * bins, [0.0] * bins, [None] * bins, [0.0] * bins, [0.0] * bins
    est, estsec, estmax = [0.0] * bins, [0.0] * bins, [None] * bins
    ptsec, tm, tp, tops = [0.0] * bins, [None] * bins, [None] * bins, [None] * bins

    def spread(a, b):
        if b <= t0 or a >= t1 or b <= a:
            return ()
        k0, k1 = max(0, int((a - t0) // w)), min(bins - 1, int(math.ceil((b - t0) / w)) - 1)
        if k0 == k1:
            return ((k0, (min(b, t1) - max(a, t0)) / (b - a)),)
        out = []
        for k in range(k0, k1 + 1):
            lo, hi = max(a, t0 + k * w), min(b, t0 + (k + 1) * w)
            if hi > lo:
                out.append((k, (hi - lo) / (b - a)))
        return out

    def machine(parts, rec_s, cores, busy_ts, peak, flops, fps, memgb, nrun, top):
        for k, f in parts:
            sec[k] += rec_s * f
            cap[k] += cores * rec_s * f
            busy[k] += busy_ts * f
            bmax[k] = peak if bmax[k] is None or peak > bmax[k] else bmax[k]
            if count_flops:
                fl[k] += flops * f
                flsec[k] += rec_s * f
                flmax[k] = fps if flmax[k] is None or fps > flmax[k] else flmax[k]
            if memgb:
                mem[k] += memgb * rec_s * f
                memsec[k] += rec_s * f
            run[k] = nrun if run[k] is None or nrun > run[k] else run[k]
            if top:
                d = tops[k] = tops[k] or {}
                for name, cores_used in top:
                    d[name] = d.get(name, 0.0) + cores_used * rec_s * f

    def mac(parts, rec_s, extra):
        sw, pm, aw, bt, fe = extra
        rate = fe / rec_s if rec_s > 0 else 0.0
        for k, f in parts:
            est[k] += fe * f
            estsec[k] += rec_s * f
            estmax[k] = rate if estmax[k] is None or rate > estmax[k] else estmax[k]
            swap[k] += sw * rec_s * f
            swsec[k] += rec_s * f
            pres[k] = pm if pres[k] is None or pm > pres[k] else pres[k]
            away[k] += aw * rec_s * f
            batt[k] += bt * rec_s * f

    def threads(parts, rec_s, shares, peaks):
        n = len(shares)
        for k, f in parts:
            ptsec[k] += rec_s * f
            m, p = tm[k], tp[k]
            if m is None or len(m) < n:
                m = tm[k] = (m or []) + [0.0] * (n - len(m or []))
                p = tp[k] = (p or []) + [0.0] * (n - len(p or []))
            for i in range(n):
                m[i] += shares[i] * rec_s * f
                if peaks[i] > p[i]:
                    p[i] = peaks[i]

    def minute_of(s):
        return int((s["t"] - s.get("dt", PERIOD) / 2) // 60) * 60

    smp_cov = collections.Counter()
    for s in samples:
        smp_cov[minute_of(s)] += s.get("dt", PERIOD)
    rec_s, rec_pt = {}, set()
    for (t, s, cores, b, umax, flops, memgb, nrun, cpu, cpk, top, extra) in node.minute_obs(t0 - 60, t1):
        if s <= 0:
            continue
        rec_s[t] = s
        if cpu:
            rec_pt.add(t)
        if fine and smp_cov.get(t, 0) >= 0.8 * min(60.0, s):
            continue  # the samples draw this minute in finer columns
        parts = spread(t, t + 60)
        machine(parts, s, cores, b, umax, flops, flops / s if s > 30 else 0.0, memgb, nrun, top)
        if extra:
            mac(parts, s, extra)
        if cpu:
            threads(parts, s, cpu, cpk or cpu)
    for s in samples:
        m, dt = minute_of(s), s.get("dt", PERIOD)
        parts = spread(s["t"] - dt, s["t"])
        if not parts:
            continue
        mine = m not in rec_s or (fine and smp_cov.get(m, 0) >= 0.8 * min(60.0, rec_s[m]))
        if mine:
            cpu = s.get("cpu") or []
            machine(parts, dt, len(cpu), sum(cpu) * dt, s.get("util", 0.0), s.get("flops") or 0,
                    (s.get("flops") or 0) / dt, (s.get("mem") or [0])[0], s.get("run") or 0,
                    [(name, cores) for name, cores in s.get("top") or []])
            if "swap" in s or "flops_est" in s:
                pw = s.get("power") or {}
                mac(parts, dt, ((s.get("swap") or [0])[0], s.get("pressure") or 0,
                                1.0 if (s.get("idle") or 0) >= AWAY_AFTER else 0.0, 0.0 if pw.get("ac", True) else 1.0,
                                s.get("flops_est") or 0))
        if mine or m not in rec_pt:
            cpu = s.get("cpu")
            if cpu:
                threads(parts, dt, cpu, cpu)

    r3 = lambda x: None if x is None else round(x, 3)  # noqa: E731
    out = {"node": node.name, "t0": t0, "t1": t1, "bins": bins, "width": w, "now": time.time(),
           "threads": info.get("threads") or node.cfg.get("threads") or max((len(m) for m in tm if m), default=0),
           "kinds": info.get("core_kinds"), "counted": count_flops,
           "cover": [round(min(1.0, x / w), 3) for x in sec],
           "busy": [round(busy[k] / cap[k], 3) if cap[k] > 0 else None for k in range(bins)],
           "busy_max": [r3(x) for x in bmax],
           "flops": [round(fl[k] / flsec[k]) if flsec[k] > 0 else None for k in range(bins)],
           "flops_max": [None if x is None else round(x) for x in flmax],
           "mem": [round(mem[k] / memsec[k], 2) if memsec[k] > 0 else None for k in range(bins)],
           "run": [None if x is None else int(x) for x in run], "pt": [round(min(1.0, x / w), 3) for x in ptsec],
           "mean": [[round(v / ptsec[k], 3) for v in tm[k]] if tm[k] and ptsec[k] > 0 else None for k in range(bins)],
           "peak": [[round(v, 3) for v in tp[k]] if tp[k] and ptsec[k] > 0 else None for k in range(bins)],
           "top": [[[name, round(v / sec[k], 2)] for name, v in sorted(d.items(), key=lambda x: -x[1])[:3]
                    if v / sec[k] >= 0.05] if d and sec[k] > 0 else None for k, d in enumerate(tops)],
           "first": node.first_minute()}
    if any(x > 0 for x in estsec):  # this Mac: floating-point operations a second, estimated (see FLOP_PER_INSTRUCTION)
        out["flops_est"] = [round(est[k] / estsec[k]) if estsec[k] > 0 else None for k in range(bins)]
        out["flops_est_max"] = [None if x is None else round(x) for x in estmax]
    if any(x > 0 for x in swsec):  # this Mac: swap in use (GB), the worst memory pressure, and away, in use or on battery
        out["swap"] = [round(swap[k] / swsec[k], 2) if swsec[k] > 0 else None for k in range(bins)]
        out["pressure"] = [None if x is None else int(x) for x in pres]
        out["act"] = [None if swsec[k] <= 0 else "batt" if batt[k] >= 0.5 * swsec[k] else
                      "away" if away[k] >= 0.5 * swsec[k] else "use" for k in range(bins)]
    if modes:  # the showcase runner's modes on this Mac, as [start, end, mode, why] within the window
        segs = []
        for i, (t, mode, why) in enumerate(modes):
            end = modes[i + 1][0] if i + 1 < len(modes) else time.time()
            if end > t0 and t < t1:
                segs.append([max(t, t0), min(end, t1), mode, why])
        out["modes"] = segs
    return out


def node_view(n, now, full):
    with n.lock:
        samples = list(n.samples)
        v = {"name": n.name, "label": n.cfg.get("label", n.name), "about": n.cfg.get("about", ""),
             "planned": n.planned, "state": n.state, "note": n.note, "since": n.since, "last_line": n.last_line,
             "info": n.info, "jobs": n.jobs, "totals": n.totals,
             "threads": (n.info or {}).get("threads") or n.cfg.get("threads"), "memory_gb": n.cfg.get("memory_gb"),
             "local": n.local, "counted": counted(n), "showcase": n.showcase, "activity": n.activity,
             "estimate": n.estimate}
    if v["state"] == "online" and now - v["last_line"] > 15:
        v["state"] = "silent"
    if full:
        v["samples"] = samples[-180:]  # the last six minutes at full rate
        v["heat"] = heat(samples)
    return v


def api_state():
    now = time.time()
    return {"now": now, "apollo_instructions": APOLLO_INSTRUCTIONS, "nodes": [node_view(n, now, True) for n in NODES],
            "mode": "read-only" if OPTS["read_only"] else "live", "mirror": OPTS["mirror"]}


def api_live(since):
    now = time.time()
    out = {"now": now, "nodes": []}
    for n in NODES:
        v = node_view(n, now, False)
        with n.lock:
            v["samples"] = [s for s in n.samples if s["t"] > since][-120:]
        out["nodes"].append(v)
    return out


def api_history(hours):
    now = time.time()
    t1 = math.ceil(now / 60) * 60
    t0 = t1 - hours * 3600
    return {"now": now, "since": t0, "until": t1, "nodes": {n.name: series(n, t0, t1) for n in NODES if not n.planned}}


def api_summary(t0, t1):
    return {"now": time.time(), "nodes": {n.name: summary(n, t0, t1) for n in NODES if not n.planned}}


def api_threads(q):
    node = find(q["node"])
    if node is None or node.planned:
        raise KeyError("no machine named %r is recorded" % q["node"])
    now = time.time()
    t1 = float(q.get("t1", now))
    t0 = float(q.get("t0", t1 - 3600))
    if not (t1 > t0 and t1 - t0 <= 5 * 366 * 86400):
        raise ValueError("t0 must be before t1, and the window at most five years")
    return threads_view(node, t0, t1, int(float(q.get("bins", 600))))


def api_bench():
    """the machines' benchmark (~/.openphysicsai/monitor/bench.json, written by the benchmark), if it has run"""
    path = DATA / "bench.json"
    if not path.exists():
        return {"ok": False, "missing": True}
    try:
        b = json.loads(path.read_text())
    except (OSError, ValueError) as e:
        return {"ok": False, "error": "bench.json could not be read: %s" % e}
    if not isinstance(b, dict) or not isinstance(b.get("tests"), list):
        return {"ok": False, "error": "bench.json has no list of tests"}
    return {"ok": True, "bench": b, "mtime": path.stat().st_mtime}


# ---------------------------------------------------------------- a read-only copy beside the running monitor

def get_json(url, timeout=5):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.loads(r.read().decode())


def mirror(url):
    """read-only: the nodes' state, jobs, totals and two-second samples from the monitor at url (what its own page
    reads), so this copy shows live data without connecting to the nodes; the minute history is read from the data
    directory, which that monitor keeps up to date"""
    last_state = 0.0
    while True:
        try:
            mirrored = [n for n in NODES if not n.local and not n.planned]
            if time.time() - last_state > 15:
                views, last_state = get_json(url + "/api/state")["nodes"], time.time()
            else:
                ts = [n.samples[-1]["t"] for n in mirrored if n.samples]
                since = min(ts) if ts else time.time() - 600
                views = get_json(url + "/api/live?since=%.2f" % since)["nodes"]
            for v in views:
                n = find(v.get("name"))
                if n is None or n.local or n.planned:
                    continue
                for s in v.get("samples") or []:
                    n.add(s)
                with n.lock:
                    n.state, n.note, n.since = v.get("state", n.state), v.get("note", ""), v.get("since", n.since)
                    n.last_line = v.get("last_line") or n.last_line
                    n.info = v.get("info") or n.info
                    n.jobs = v.get("jobs", n.jobs)
                n.take_totals(v.get("totals"))
        except Exception as e:  # the main monitor restarts, or is gone: say so on the nodes
            for n in NODES:
                if not n.local and not n.planned:
                    n.set("offline", "the main monitor (%s) is not answering: %s" % (url, e))
            last_state = 0.0
        time.sleep(PERIOD)


def refresh_from_disk():
    """read-only without a mirror: the totals and the machine as the running monitor last copied them"""
    while True:
        for n in NODES:
            if not n.local and not n.planned:
                load_history(n)
        time.sleep(60)


# ---------------------------------------------------------------- the server

class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "openphysics-monitor"

    def log_message(self, *args):
        pass

    def send(self, code, body, ctype):
        b = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = {k: v[0] for k, v in urllib.parse.parse_qs(u.query).items()}
        try:
            data = None
            if u.path == "/api/state":
                data = api_state()
            elif u.path == "/api/live":
                data = api_live(float(q.get("since", 0)))
            elif u.path == "/api/history":
                data = api_history(min(24 * 90, max(1, float(q.get("hours", 24)))))
            elif u.path == "/api/summary":
                data = api_summary(float(q["since"]), float(q.get("until", time.time())))
            elif u.path == "/api/threads":
                data = api_threads(q)
            elif u.path == "/api/bench":
                data = api_bench()
            if data is not None:
                return self.send(200, json.dumps(data, separators=(",", ":"), allow_nan=False), "application/json")
            name = "index.html" if u.path in ("", "/") else u.path.lstrip("/")
            if name in STATIC:
                return self.send(200, (WEB / name).read_bytes(), STATIC[name])
            self.send(404, "not found", "text/plain")
        except (BrokenPipeError, ConnectionResetError):
            pass
        except (KeyError, ValueError) as e:
            self.send(400, json.dumps({"error": "bad request: %s" % e}), "application/json")


def take_config():
    """the nodes in nodes.json: a new one, or a planned one that has been set up, starts being followed (unless
    read-only); this Mac is added after them"""
    cfg = remote.load_nodes(planned=True) if remote.CONFIG.exists() else {}
    old = {n.name: n for n in NODES}
    nodes = []
    for name, c in cfg.items():
        if name == "mac" and OPTS["mac"]:
            continue  # the name of this Mac on the page
        n = old.get(name)
        if n is None or (n.planned and not c.get("planned")):
            n = Node(name, c)
            if OPTS["read_only"]:
                load_history(n)
                if not n.planned:
                    n.set("offline", "this read-only copy does not follow the nodes")
            elif not n.planned:
                threading.Thread(target=follow, args=(n,), daemon=True).start()
        nodes.append(n)
    if OPTS["mac"]:
        mac = old.get("mac") if old.get("mac") and old["mac"].local else mac_node()
        if mac:
            nodes.append(mac)
    NODES[:] = nodes


def watch_config():
    seen = remote.CONFIG.stat().st_mtime if remote.CONFIG.exists() else 0
    while True:
        time.sleep(30)
        try:
            mtime = remote.CONFIG.stat().st_mtime
        except OSError:
            continue
        if mtime != seen:
            seen = mtime
            take_config()


def option(args, name, default=None):
    if name in args:
        i = args.index(name)
        if i + 1 >= len(args):
            sys.exit("monitor.py: %s needs a value" % name)
        return args[i + 1]
    return default


def cmd_serve(args):
    global DATA
    port = int(option(args, "--port", PORT))
    DATA = pathlib.Path(option(args, "--data", str(DATA))).expanduser()
    OPTS["read_only"] = "--read-only" in args
    OPTS["mirror"] = (option(args, "--mirror") or "").rstrip("/") or None
    OPTS["mac"] = "--no-mac" not in args and platform.system() == "Darwin"
    if OPTS["mirror"] and not OPTS["read_only"]:
        sys.exit("monitor.py: --mirror is for a read-only copy; add --read-only")
    take_config()
    if not OPTS["read_only"]:
        threading.Thread(target=watchdog, daemon=True).start()
    threading.Thread(target=watch_config, daemon=True).start()
    if OPTS["mirror"]:
        threading.Thread(target=mirror, args=(OPTS["mirror"],), daemon=True).start()
    elif OPTS["read_only"]:
        threading.Thread(target=refresh_from_disk, daemon=True).start()
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
    srv.daemon_threads = True
    what = ("read-only, live lines from %s" % OPTS["mirror"] if OPTS["mirror"] else "read-only") \
        if OPTS["read_only"] else "following the nodes"
    print("monitor: serving http://127.0.0.1:%d (%s; data in %s)" % (port, what, DATA), flush=True)
    srv.serve_forever()


def cmd_summary(args):
    hours = float(args[0]) if args else 12.0
    now = time.time()
    machines = list(remote.load_nodes().items())
    if (DATA / "mac" / "minutes").is_dir():
        machines.append(("mac", {"label": "MacBook", "kind": "local"}))
    for name, cfg in machines:
        node = Node(name, cfg)
        if not node.local and not copy_history(node):
            print("%s: could not copy its history; showing what this Mac already has" % name)
        s = summary(node, now - hours * 3600, now)
        if not s["minutes"]:
            print("%s: nothing recorded in the last %g h" % (name, hours))
            continue
        if node.local:
            print("%s: %.1f of %g h recorded, busy %.0f%% of %d cores (macOS gives no floating-point counters)" % (
                name, s["covered"] / 3600, hours, 100 * (s["util"] or 0), s["cores"]))
            continue
        print("%s: %.1f of %g h recorded, busy %.0f%% of %d threads, %.3g floating-point operations (%.1f GFLOP/s on "
              "average, fastest minute %.1f), %.3g instructions = %s Apollo 11 missions; clock %.2f GHz while busy, "
              "%.2f instructions per cycle" % (
                  name, s["covered"] / 3600, hours, 100 * (s["util"] or 0), s["cores"], s["flops"],
                  s["flops"] / max(s["covered"], 1) / 1e9, s["peak_flops"] / 1e9, s["ins"],
                  "{:,.0f}".format(s["ins"] / APOLLO_INSTRUCTIONS), s["ghz"] or 0, s["ipc"] or 0))


def main():
    cmds = {"install": cmd_install, "check": cmd_check}
    if len(sys.argv) < 2 or sys.argv[1] not in list(cmds) + ["serve", "summary"]:
        print(__doc__)
        return 2
    if sys.argv[1] == "serve":
        return cmd_serve(sys.argv[2:])
    if sys.argv[1] == "summary":
        return cmd_summary(sys.argv[2:])
    nodes = remote.load_nodes()
    name, n, rest = remote.pick(nodes, sys.argv[2:])
    cmds[sys.argv[1]](name, n, rest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
