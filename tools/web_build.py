#!/usr/bin/env python3
"""web_build.py - the physics lab as WebAssembly, and the scenarios of the lab page (site/lab/, src/web/README.md).

    python3 tools/web_build.py                 # from origin/main: build/web/site/lab/ (module, scenarios, provenance)
    python3 tools/web_build.py --ref .         # from the working tree instead, to try a change before it is public
    python3 tools/web_build.py --check         # and run every scenario of the page: in WebAssembly under Node, through
                                               # the page's own WASI layer, and natively from the same sources

The website describes the public branch, so by default the module is compiled from origin/main (a `git archive` of it
under build/web/), with this checkout's src/web/ added: the glue that turns the lab into a module a page can call.
The sources are the native lab's (src/lab/lab.mk) with the native core's arithmetic: C11, -O2, IEEE, no fast-math, no
floating-point contraction. What differs: one thread (src/web/threads_serial.c in place of src/threads.c), no GPU
engine (src/lab/flow/gpu_none.c: a scenario that asks for one says so), and no MFEM (the 3D magnetic backends).

The page's scenarios are the app's own (examples/lab/ on the same branch), made smaller where a phone needs it: each
entry of site/lab/catalogue.json names its source, what is changed and why, and its first view. The output, which
tools/site_build.py copies into the site:

    build/web/site/lab/lab.wasm          the module (debug information stripped)
    build/web/site/lab/scenarios.json    the catalogue the page reads
    build/web/site/lab/scenarios/*.json  the scenarios
    build/web/site/lab/build.json        what it was built from: branch, commit, glue, compiler

The compiler is wasi-sdk (Apache-2.0 WITH LLVM-exception; docs/THIRD_PARTY.md), unpacked under build/third_party/;
WASI_SDK names another copy. --check also needs Node 18 or later."""
import argparse
import concurrent.futures
import glob
import hashlib
import io
import json
import os
import resource
import shutil
import subprocess
import sys
import tarfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = os.path.join(ROOT, "build", "web")
OUT = os.path.join(WEB, "site", "lab")
CATALOGUE = os.path.join(ROOT, "site", "lab", "catalogue.json")

# the 3D magnetic backends need MFEM (C++), which this build does not carry; the headset's scene writer is not used
LEAVE_OUT = {"src/lab/magnet/spdirect.c", "src/lab/magnet/motor3d.c", "src/lab/magnet/induct3d.c", "src/lab/labxr.c"}

CFLAGS = ["-std=c11", "-O2", "-ffp-contract=off", "-D_GNU_SOURCE", "-DOPENPHYSICS_HAS_MFEM=0",
          "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-missing-field-initializers", "-Wno-pass-failed", "-MMD", "-MP"]
# clock() and getpid() only time a run and name a temporary file; WASI emulates them from the wall clock and a constant
# (no -msimd128: on the lab's loops it gained about 5 % on 2026-10-07, with the same results, and it would shut out
# browsers older than Safari 16.4)
WASM_CFLAGS = ["--target=wasm32-wasip1", "-D_WASI_EMULATED_PROCESS_CLOCKS", "-D_WASI_EMULATED_GETPID"]
# 8 MB of stack, as a desktop thread has; memory starts at 64 MB and grows to 4 GB at most
WASM_LDFLAGS = ["--target=wasm32-wasip1", "-Wl,-z,stack-size=8388608", "-Wl,--gc-sections", "-Wl,--strip-all",
                "-Wl,--initial-memory=67108864", "-Wl,--max-memory=4294967296",
                "-lwasi-emulated-process-clocks", "-lwasi-emulated-getpid", "-lm"]


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def sdk():
    d = os.environ.get("WASI_SDK") or next(iter(sorted(glob.glob(os.path.join(ROOT, "build", "third_party", "wasi-sdk-*")))), "")
    cc = os.path.join(d, "bin", "clang")
    if not os.path.isfile(cc):
        sys.exit("web_build: no wasi-sdk (build/third_party/wasi-sdk-*, or WASI_SDK); src/web/README.md says where to get it")
    version = open(os.path.join(d, "VERSION")).read().split()[0] if os.path.isfile(os.path.join(d, "VERSION")) else "?"
    return cc, version


def tree(ref):
    """The sources to build: a git archive of ref under build/web/tree-COMMIT/, or the working tree for '.'.
    Returns (directory, commit, description)."""
    if ref == ".":
        head = run(["git", "-C", ROOT, "rev-parse", "HEAD"]).stdout.strip()
        dirty = run(["git", "-C", ROOT, "status", "--porcelain", "--", "src", "examples/lab", "tools/labrun.c"]).stdout.strip()
        return ROOT, head, "the working tree at %s%s" % (head[:12], " with local changes" if dirty else "")
    r = run(["git", "-C", ROOT, "rev-parse", "--verify", ref + "^{commit}"])
    if r.returncode:
        sys.exit("web_build: %s is not a commit here (git fetch origin first?)" % ref)
    commit = r.stdout.strip()
    d = os.path.join(WEB, "tree-" + commit[:12])
    if not os.path.isfile(os.path.join(d, ".complete")):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        data = subprocess.run(["git", "-C", ROOT, "archive", commit, "src", "tools/labrun.c", "examples/lab"],
                              capture_output=True, check=True).stdout
        with tarfile.open(fileobj=io.BytesIO(data)) as t:
            if sys.version_info >= (3, 12):
                t.extractall(d, filter="data")
            else:
                t.extractall(d)
        open(os.path.join(d, ".complete"), "w").write(commit + "\n")
    # the glue is this checkout's, whatever the branch: it is what turns the lab into a module; and the check's comparer
    glue = os.path.join(d, "src", "web")
    shutil.rmtree(glue, ignore_errors=True)
    shutil.copytree(os.path.join(ROOT, "src", "web"), glue)
    shutil.copyfile(os.path.join(ROOT, "tools", "labdiff.c"), os.path.join(d, "tools", "labdiff.c"))
    return d, commit, "%s at %s" % (ref, commit[:12])


def sources(src):
    rel = lambda p: os.path.relpath(p, src)
    lab = sorted(glob.glob(os.path.join(src, "src", "lab", "*.c")) + glob.glob(os.path.join(src, "src", "lab", "*", "*.c")))
    files = [rel(p) for p in lab if rel(p) not in LEAVE_OUT]
    for d in ("core", "render", "fem"):
        files += [rel(p) for p in sorted(glob.glob(os.path.join(src, "src", d, "*.c"))) if not p.endswith("xrlink.c")]
    files += ["src/geom/" + n for n in ("mesh.c", "bvh.c", "surface.c", "patches.c", "hexmesh.c", "repair.c", "tetmesh.c")]
    files += ["src/common.c", "src/lbm.c", "src/web/threads_serial.c"]
    return [f for f in files if os.path.isfile(os.path.join(src, f))]


def stale(src, path, obj):
    if not os.path.isfile(obj):
        return True
    t = os.path.getmtime(obj)
    deps = [os.path.join(src, path)]
    dep = obj[:-2] + ".d"
    if os.path.isfile(dep):
        text = open(dep).read().replace("\\\n", " ")
        deps += text.split("\n", 1)[0].split(":", 1)[-1].split()
    return any(not os.path.isfile(p) or os.path.getmtime(p) > t
               for p in (d if os.path.isabs(d) else os.path.join(src, d) for d in deps))


def compile_all(cc, src, files, objdir, flags, jobs):
    def one(path):
        obj = os.path.join(objdir, os.path.splitext(path)[0] + ".o")
        if not stale(src, path, obj):
            return obj, 0, ""
        os.makedirs(os.path.dirname(obj), exist_ok=True)
        r = run([cc] + flags + CFLAGS + ["-c", path, "-o", obj], cwd=src)
        return obj, r.returncode, r.stderr

    objs, failed = [], []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        for (obj, rc, err), path in zip(ex.map(one, files), files):
            err = "\n".join(l for l in err.splitlines() if "is deprecated, use --target" not in l)
            if err.strip():
                sys.stderr.write(err + "\n")
            if rc:
                failed.append(path)
            else:
                objs.append(obj)
    if failed:
        sys.exit("web_build: %d file(s) did not compile: %s" % (len(failed), " ".join(failed)))
    return objs


def link(cc, objs, out, flags, cwd):
    r = run([cc] + objs + flags + ["-o", out], cwd=cwd)
    if r.returncode:
        sys.stderr.write(r.stderr)
        sys.exit("web_build: linking %s failed" % os.path.relpath(out, ROOT))


def build_wasm(src, commit, jobs):
    cc, version = sdk()
    files = sources(src)
    objdir = os.path.join(WEB, "obj-" + (commit[:12] if src != ROOT else "worktree"))
    t0 = time.time()
    objs = compile_all(cc, src, files + ["src/web/labweb.c", "tools/labrun.c"], objdir, WASM_CFLAGS, jobs)
    lib = [o for o in objs if not o.endswith(("labweb.o", "labrun.o"))]
    page, command = os.path.join(objdir, "src/web/labweb.o"), os.path.join(objdir, "tools/labrun.o")
    os.makedirs(OUT, exist_ok=True)
    wasm = os.path.join(OUT, "lab.wasm")
    link(cc, lib + [page], wasm, ["-mexec-model=reactor"] + WASM_LDFLAGS, src)
    labrun = os.path.join(WEB, "labrun.wasm")
    link(cc, lib + [command], labrun, WASM_LDFLAGS, src)
    print("web_build: %d sources, %.0f s; %s %.2f MB" % (len(files) + 2, time.time() - t0, os.path.relpath(wasm, ROOT),
                                                       os.path.getsize(wasm) / 1e6))
    return wasm, labrun, version


def build_native(src, commit, jobs):
    """labrun from the same sources, natively and on one thread, for --check; and labdiff, which compares results."""
    files = sources(src) + ["tools/labrun.c", "tools/labdiff.c"]
    darwin = sys.platform == "darwin"
    objdir = os.path.join(WEB, "native-" + (commit[:12] if src != ROOT else "worktree"))
    objs = compile_all("clang", src, files, objdir, [], jobs)
    if darwin:  # the GPU engines (Metal) link on macOS, where gpu_none.c is empty; --check runs on the CPU
        for m in ("src/lab/flow/lbm3d_metal.m", "src/lab/water/water_metal.m"):
            o = os.path.join(objdir, m[:-2] + ".o")
            os.makedirs(os.path.dirname(o), exist_ok=True)
            r = run(["clang", "-O2", "-fobjc-arc", "-ffp-contract=off", "-w", "-c", m, "-o", o], cwd=src)
            if r.returncode:
                sys.exit(r.stderr)
            objs.append(o)
    exe, diff = os.path.join(WEB, "labrun-native"), os.path.join(WEB, "labdiff-native")
    # the 3D magnetic backends are weak references, absent without MFEM; the macOS linker must be told they may be
    ldflags = ["-lm"] + (["-framework", "Metal", "-framework", "Foundation", "-Wl,-U,_lab_run_induction",
                          "-Wl,-U,_lab_run_magnet3d"] if darwin else [])
    lib = [o for o in objs if not o.endswith(("labrun.o", "labdiff.o"))]
    link("clang", lib + [os.path.join(objdir, "tools", "labrun.o")], exe, ldflags, src)
    link("clang", lib + [os.path.join(objdir, "tools", "labdiff.o")], diff, ldflags, src)
    return exe, diff


def put(d, path, value):
    keys = path.split(".")
    for k in keys[:-1]:
        d = d[int(k)] if isinstance(d, list) else d.setdefault(k, {})
    if isinstance(d, list):
        d[int(keys[-1])] = value
    else:
        d[keys[-1]] = value


def scenarios(src):
    """The page's scenarios from the catalogue and the branch's examples/lab; returns the catalogue the page reads."""
    cat = json.load(open(CATALOGUE))
    out_dir = os.path.join(OUT, "scenarios")
    shutil.rmtree(out_dir, ignore_errors=True)
    os.makedirs(out_dir)
    entries = []
    for e in cat["scenarios"]:
        path = os.path.join(src, e["from"])
        if not os.path.isfile(path):
            sys.exit("web_build: %s names %s, which is not on the branch built from" % (e["id"], e["from"]))
        d = json.load(open(path))
        for key, value in e.get("change", {}).items():
            put(d, key, value)
        text = json.dumps(d, indent=1, ensure_ascii=False) + "\n"
        open(os.path.join(out_dir, e["id"] + ".json"), "w").write(text)
        page = {k: v for k, v in e.items() if k not in ("from", "change")}
        page["file"] = "scenarios/%s.json?v=%s" % (e["id"], hashlib.sha256(text.encode()).hexdigest()[:10])
        page["source"] = e["from"]
        entries.append(page)
    return {"scenarios": entries}


def differ(a, b):
    """The largest relative difference between two JSON values' numbers (inf when their shapes differ)."""
    if isinstance(a, dict) and isinstance(b, dict):
        return max([differ(a[k], b[k]) if k in b else float("inf") for k in a] + [0.0])
    if isinstance(a, list) and isinstance(b, list):
        return max([differ(x, y) for x, y in zip(a, b)] + [0.0 if len(a) == len(b) else float("inf")])
    if isinstance(a, (int, float)) and isinstance(b, (int, float)) and not isinstance(a, bool):
        return 0.0 if a == b else abs(a - b) / max(abs(a), abs(b))
    return 0.0 if a == b else float("inf")


def poster(entry, result, work):
    """The scenario's card picture: a frame of its result drawn by the page's own module, 480 by 270, as a JPEG where
    macOS's sips can make one (a PNG elsewhere); returns its path in the site, named by its content."""
    out = os.path.join(work, entry["id"] + "-poster")
    cam = entry.get("camera", [-55, 30])
    r = run(["node", os.path.join(ROOT, "tools", "web_run.mjs"), os.path.join(OUT, "lab.wasm"), "-", out, "--result", result,
             "--view", json.dumps(entry.get("view", {})), "--size", "480x270", "--azim", str(cam[0]), "--elev", str(cam[1]),
             "--frames", "1", "--at", str(entry.get("poster_at", 0.6))])
    png = os.path.join(out, "frame_00.png")
    if r.returncode or not os.path.isfile(png):
        sys.exit("web_build: the poster of %s did not render: %s" % (entry["id"], r.stderr.strip()[-300:]))
    os.makedirs(os.path.join(OUT, "posters"), exist_ok=True)
    jpg = os.path.join(work, entry["id"] + ".jpg")
    made = shutil.which("sips") and run(["sips", "-s", "format", "jpeg", "-s", "formatOptions", "82", png, "--out", jpg]).returncode == 0
    src, ext = (jpg, ".jpg") if made else (png, ".png")
    data = open(src, "rb").read()
    name = "posters/%s%s" % (entry["id"], ext)
    open(os.path.join(OUT, name), "wb").write(data)
    return "%s?v=%s" % (name, hashlib.sha256(data).hexdigest()[:10])


def timed(cmd):
    """Run cmd; return the result and the processor time it took (user and system, s). Processor time, not the time
    on the clock, so that other work on the machine does not inflate the page's estimates."""
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    r = run(cmd)
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    return r, (after.ru_utime - before.ru_utime) + (after.ru_stime - before.ru_stime)


def check(labrun_wasm, native, entries):
    """Every scenario in WebAssembly (Node, the page's WASI layer) and natively; the records must agree, and the
    fields when the files are not identical. Each passing scenario gets its card picture, drawn from the WebAssembly
    result, and its time: the processor time of the WebAssembly run here."""
    native, labdiff = native
    work = os.path.join(WEB, "check")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    shutil.rmtree(os.path.join(OUT, "posters"), ignore_errors=True)
    bad = 0
    for e in entries:
        sc = os.path.join(OUT, e["file"].split("?")[0])
        res_w, res_n = os.path.join(work, e["id"] + ".wasm.lab"), os.path.join(work, e["id"] + ".native.lab")
        w, tw = timed(["node", os.path.join(ROOT, "tools", "web_run.mjs"), labrun_wasm, sc, res_w])
        n, tn = timed([native, sc, res_n, "--quiet"])
        if w.returncode or n.returncode:
            print("  %-12s FAILED: %s" % (e["id"], ((w.stderr if w.returncode else n.stderr).strip().splitlines() or ["?"])[-1]))
            bad += 1
            continue
        rw, rn = json.loads(w.stdout.strip().splitlines()[-1]), json.loads(n.stdout.strip().splitlines()[-1])
        same = open(res_w, "rb").read() == open(res_n, "rb").read()
        worst = differ(rw.get("diagnostics"), rn.get("diagnostics"))
        fields = 0.0
        if not same:  # how far apart the fields are, relative to each field's largest magnitude
            d = run([labdiff, res_w, res_n])
            fields = json.loads(d.stdout.strip().splitlines()[-1])["largest_relative_difference"] if d.returncode == 0 else float("inf")
        e["seconds"] = round(tw, 1)
        e["mb"] = round(os.path.getsize(res_w) / 1e6, 1)
        e["check"] = "identical" if same else "diagnostics differ by %.1g, fields by %.1g" % (worst, fields)
        verdict = ("the same result, byte for byte" if same else
                   "diagnostics differ by at most %.1g, fields by %.1g of their largest magnitude" % (worst, fields))
        ok = same or (worst < 1e-6 and fields < 1e-4)
        if not ok:
            verdict = "DIFFERENT: " + verdict
        bad += not ok
        print("  %-12s %5.1f s in WebAssembly, %5.1f s native (processor time), %5.1f MB: %s" % (e["id"], tw, tn, e["mb"], verdict))
        sys.stdout.flush()
        e["poster"] = poster(e, res_w, work)
        os.remove(res_w)
        os.remove(res_n)
    shutil.rmtree(work, ignore_errors=True)
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ref", default="origin/main", help="the branch or commit to build from; '.' for the working tree")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    src, commit, described = tree(a.ref)
    wasm, labrun, version = build_wasm(src, commit, a.jobs)
    cat = scenarios(src)
    glue = hashlib.sha256()  # the glue's code: its README may change without a rebuild
    for p in sorted(glob.glob(os.path.join(ROOT, "src", "web", "*.[ch]"))):
        glue.update(open(p, "rb").read())
    bad = 0
    if a.check:
        native = build_native(src, commit, a.jobs)
        print("web_build: every scenario of the page, from %s:" % described)
        bad = check(labrun, native, cat["scenarios"])
    json.dump(cat, open(os.path.join(OUT, "scenarios.json"), "w"), indent=1, ensure_ascii=False)
    info = {"ref": a.ref, "commit": commit, "built_from": described, "glue_sha256": glue.hexdigest(),
            "catalogue_sha256": hashlib.sha256(open(CATALOGUE, "rb").read()).hexdigest(),
            "wasi_sdk": version, "module_sha256": hashlib.sha256(open(wasm, "rb").read()).hexdigest(),
            "checked": bool(a.check and not bad), "built": time.strftime("%Y-%m-%d")}
    json.dump(info, open(os.path.join(OUT, "build.json"), "w"), indent=1)
    tail = "" if not a.check else ", %d scenario(s) failed the check" % bad if bad else ", every scenario checked"
    print("web_build: %s written from %s%s" % (os.path.relpath(OUT, ROOT), described, tail))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
