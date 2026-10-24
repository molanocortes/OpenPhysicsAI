#!/usr/bin/env python3
"""remote.py - send work from this Mac to the compute nodes on the local network (docs/compute-nodes.md).

A node is a machine reachable by SSH with a key, where the engine is built and jobs run detached with their logs.
Machines and logins are kept out of the repository, in ~/.openphysicsai/nodes.json:

    {"bignode": {"host": "192.0.2.20", "mdns": "<name>.local", "user": "...", "key": "~/.ssh/openphysics_node",
                 "kind": "windows-wsl", "distro": "Ubuntu-24.04", "repo": "/home/node/openphysics", "threads": 8}}

kind "windows-wsl": Windows' own OpenSSH server (PowerShell as its shell) in front of Linux in WSL2; commands go to
Linux on stdin, files through rsync inside WSL, and a keep-alive process started outside any SSH session keeps the
distribution running between connections. kind "linux": a plain Linux machine.

    python3 tools/remote.py status [NODE]          reachable, Linux running, load, memory, jobs
    python3 tools/remote.py wake [NODE]            start the keep-alive if it is not running
    python3 tools/remote.py sync [NODE]            the repository's tracked files (no website, no gallery media)
    python3 tools/remote.py build [NODE] [TARGET]  make -jN TARGET in the node's copy (headless by default)
    python3 tools/remote.py run [NODE] [--name N] -- COMMAND...   a detached job in the node's copy; prints its id
    python3 tools/remote.py jobs [NODE]            every job: state, exit code, seconds, command
    python3 tools/remote.py log [NODE] JOB [--lines N]
    python3 tools/remote.py fetch [NODE] JOB PATH...   copy files from the node's copy to build/remote/NODE/JOB/
    python3 tools/remote.py sh [NODE] -- COMMAND...    a short command in the node's copy, in the foreground

NODE may be left out when only one node is configured.
"""
import json
import os
import pathlib
import shlex
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
CONFIG = pathlib.Path.home() / ".openphysicsai" / "nodes.json"
NOISE = "Failed to start the systemd user session"  # WSL's message when the per-user manager does not start; harmless


def load_nodes(planned=False):
    """the configured nodes; an entry with "planned": true is a machine not set up yet, left out unless asked for"""
    if not CONFIG.exists():
        sys.exit("remote.py: no nodes configured (%s); see docs/compute-nodes.md" % CONFIG)
    return {k: v for k, v in json.loads(CONFIG.read_text()).items() if planned or not v.get("planned")}


def pick(nodes, args):
    if args and args[0] in nodes:
        return args[0], nodes[args[0]], args[1:]
    if len(nodes) == 1:
        name = next(iter(nodes))
        return name, nodes[name], args
    sys.exit("remote.py: name the node, one of: %s" % ", ".join(nodes))


def ssh_base(n):
    key = os.path.expanduser(n.get("key", "~/.ssh/openphysics_node"))
    return ["ssh", "-i", key, "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
            "-o", "ServerAliveInterval=30", "%s@%s" % (n["user"], n["host"])]


def clean(text):
    return "\n".join(l for l in text.replace("\r", "").replace("\0", "").splitlines() if NOISE not in l and l.strip())


def linux(n, script, check=True, capture=True, root=False):
    """a bash script run in the node's Linux, sent on stdin (no quoting through Windows' shells); root=True runs it
    as root there (WSL's own -u root; on a plain Linux node, sudo without a password)"""
    if n["kind"] == "windows-wsl":
        cmd = ssh_base(n) + ["wsl.exe -d %s%s -- bash -s" % (n["distro"], " -u root" if root else "")]
    else:
        cmd = ssh_base(n) + ["sudo -n bash -s" if root else "bash -s"]
    r = subprocess.run(cmd, input=script, text=True, capture_output=capture)
    if capture:
        out = clean(r.stdout + ("\n" + r.stderr if r.returncode else ""))
        if check and r.returncode:
            sys.exit("remote.py: the node answered with exit %d:\n%s" % (r.returncode, out))
        return out
    return ""


def windows(n, command):
    """a PowerShell command on a windows-wsl node"""
    r = subprocess.run(ssh_base(n) + [command], text=True, capture_output=True)
    return r.returncode, clean(r.stdout + r.stderr)


def reachable(n):
    r = subprocess.run(["nc", "-z", "-G", "3", n["host"], "22"], capture_output=True)
    if r.returncode and n.get("mdns"):  # the address may have changed: ask for the machine by name
        r2 = subprocess.run(["nc", "-z", "-G", "3", n["mdns"], "22"], capture_output=True)
        if r2.returncode == 0:
            print("note: %s now answers at %s, not %s; update %s" % (n["mdns"], n["mdns"], n["host"], CONFIG))
            n["host"] = n["mdns"]
            return True
    return r.returncode == 0


KEEPALIVE = "sleep infinity"


def keepalive_running(n):
    code, out = windows(n, "(Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'wsl.exe' -and $_.CommandLine -like '*%s*' } "
                           "| Measure-Object).Count" % KEEPALIVE)
    return code == 0 and out.strip().isdigit() and int(out.strip()) > 0


def wake(n):
    if n["kind"] != "windows-wsl":
        return "a plain Linux node needs no keep-alive"
    if keepalive_running(n):
        return "keep-alive already running"
    code, out = windows(n, "$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = "
                           "'C:\\Windows\\System32\\wsl.exe -d %s --exec /usr/bin/%s' }; $r.ReturnValue" % (n["distro"], KEEPALIVE))
    return "keep-alive started" if code == 0 and out.strip().endswith("0") else "keep-alive could not start: %s" % out


def cmd_status(name, n, args):
    if not reachable(n):
        sys.exit("%s: not reachable on port 22 (%s); is it on and on this network?" % (name, n["host"]))
    print("%s: reachable at %s" % (name, n["host"]))
    if n["kind"] == "windows-wsl":
        code, out = windows(n, "$env:WSL_UTF8=1; (wsl.exe -l -v | Select-String '%s').ToString().Trim()" % n["distro"])
        print("  linux: %s" % (out or "?"))
        print("  keep-alive: %s" % ("running" if keepalive_running(n) else "NOT running (python3 tools/remote.py wake)"))
    print(linux(n, r'''
echo "  load: $(cut -d' ' -f1-3 /proc/loadavg) on $(nproc) threads; memory free $(free -g | awk '/Mem:/{print $7}') of $(free -g | awk '/Mem:/{print $2}') GB; disk free $(df -h ~ | awk 'NR==2{print $4}')"
n=0; r=0; for d in ~/jobs/*/; do [ -d "$d" ] || continue; n=$((n+1)); [ "$(cat "$d/state" 2>/dev/null)" = running ] && r=$((r+1)); done
echo "  jobs: $n, running $r"
'''))


def cmd_wake(name, n, args):
    print("%s: %s" % (name, wake(n)))


def cmd_sync(name, n, args):
    files = subprocess.run(["git", "ls-files"], cwd=ROOT, text=True, capture_output=True, check=True).stdout.splitlines()
    keep = [f for f in files if not f.startswith(("docs/images/", "docs/media/", "site/"))]
    lst = ROOT / "build" / "remote-sync-files.txt"
    lst.parent.mkdir(exist_ok=True)
    lst.write_text("\n".join(keep) + "\n")
    rsh = " ".join(shlex.quote(a) for a in ssh_base(n)[:-1])
    cmd = ["rsync", "-az", "--files-from=%s" % lst, "-e", rsh]
    if n["kind"] == "windows-wsl":
        cmd += ["--rsync-path=wsl -d %s rsync" % n["distro"]]
    cmd += ["./", "%s@%s:%s/" % (n["user"], n["host"], n["repo"])]
    r = subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True)
    lst.unlink()
    if r.returncode:
        sys.exit("remote.py: rsync failed (%d): %s" % (r.returncode, clean(r.stderr)))
    print("%s: %d files in sync with %s" % (name, len(keep), n["repo"]))


def cmd_build(name, n, args):
    target = " ".join(shlex.quote(a) for a in args) or "headless"
    print(linux(n, 'cd %s && start=$(date +%%s) && make -j%d %s > ~/build.log 2>&1; c=$?; echo "make %s: exit $c in $(( $(date +%%s) - start )) s"; '
                   '[ $c -eq 0 ] || tail -15 ~/build.log' % (shlex.quote(n["repo"]), n.get("threads", 4), target, target), check=False))


def cmd_run(name, n, args):
    label = "job"
    if args[:1] == ["--name"] and len(args) > 2:
        label, args = args[1], args[2:]
    if args[:1] == ["--"]:
        args = args[1:]
    if not args:
        sys.exit("remote.py run: give the command after --")
    if n["kind"] == "windows-wsl" and not keepalive_running(n):
        print(wake(n))
    command = " ".join(args) if len(args) == 1 else " ".join(shlex.quote(a) for a in args)
    script = r'''
set -e
id=$(date +%%Y%%m%%d-%%H%%M%%S)-%(label)s
d=~/jobs/$id; mkdir -p "$d"
cat > "$d/cmd" <<'OPENPHYSICS_CMD'
cd %(repo)s
%(command)s
OPENPHYSICS_CMD
date -Is > "$d/started"; echo running > "$d/state"
D="$d" nohup setsid bash -c 'echo $$ > "$D/pid"; start=$(date +%%s); bash "$D/cmd" > "$D/log" 2>&1; code=$?; echo $code > "$D/exit"; echo $(( $(date +%%s) - start )) > "$D/seconds"; echo done > "$D/state"' > /dev/null 2>&1 < /dev/null &
# WSL ends what has not detached when the connection closes: wait until the job has written its pid
for i in $(seq 1 100); do [ -s "$d/pid" ] && break; sleep 0.1; done
[ -s "$d/pid" ] || { echo "the job did not start" >&2; echo failed > "$d/state"; exit 1; }
echo "$id"
''' % {"label": "".join(c for c in label if c.isalnum() or c in "-_") or "job", "repo": shlex.quote(n["repo"]), "command": command}
    job = linux(n, script).strip().splitlines()[-1]
    print("%s: started %s" % (name, job))
    print("  follow it: python3 tools/remote.py log %s %s" % (name, job))


def cmd_jobs(name, n, args):
    print(linux(n, r'''
for d in ~/jobs/*/; do [ -d "$d" ] || continue
  id=$(basename "$d"); st=$(cat "$d/state" 2>/dev/null); ex=$(cat "$d/exit" 2>/dev/null); s=$(cat "$d/seconds" 2>/dev/null)
  printf "%-36s %-8s exit %-4s %6s s  %s\n" "$id" "$st" "${ex:--}" "${s:--}" "$(sed -n 2p "$d/cmd" | cut -c1-70)"
done
''') or "%s: no jobs yet" % name)


def cmd_log(name, n, args):
    lines = 40
    if "--lines" in args:
        i = args.index("--lines")
        lines = int(args[i + 1])
        args = args[:i] + args[i + 2:]
    if not args:
        sys.exit("remote.py log: give the job id")
    print(linux(n, 'd=~/jobs/%s; echo "state: $(cat $d/state) exit: $(cat $d/exit 2>/dev/null || echo -)"; tail -n %d "$d/log"' % (shlex.quote(args[0]), lines), check=False))


def cmd_fetch(name, n, args):
    if len(args) < 2:
        sys.exit("remote.py fetch: give the job id and one or more paths inside the node's copy")
    job, paths = args[0], args[1:]
    dest = ROOT / "build" / "remote" / name / job
    dest.mkdir(parents=True, exist_ok=True)
    rsh = " ".join(shlex.quote(a) for a in ssh_base(n)[:-1])
    for p in paths:
        cmd = ["rsync", "-az", "-e", rsh]
        if n["kind"] == "windows-wsl":
            cmd += ["--rsync-path=wsl -d %s rsync" % n["distro"]]
        cmd += ["%s@%s:%s/%s" % (n["user"], n["host"], n["repo"], p), str(dest) + "/"]
        r = subprocess.run(cmd, text=True, capture_output=True)
        print("%s %s -> %s" % ("fetched" if r.returncode == 0 else "FAILED", p, dest) + ("" if r.returncode == 0 else ": " + clean(r.stderr)))


def cmd_sh(name, n, args):
    if args[:1] == ["--"]:
        args = args[1:]
    command = " ".join(args) if len(args) == 1 else " ".join(shlex.quote(a) for a in args)
    print(linux(n, "cd %s && %s" % (shlex.quote(n["repo"]), command), check=False))


COMMANDS = {"status": cmd_status, "wake": cmd_wake, "sync": cmd_sync, "build": cmd_build, "run": cmd_run, "jobs": cmd_jobs,
            "log": cmd_log, "fetch": cmd_fetch, "sh": cmd_sh}


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS:
        print(__doc__)
        return 2
    nodes = load_nodes()
    name, n, rest = pick(nodes, sys.argv[2:])
    COMMANDS[sys.argv[1]](name, n, rest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
