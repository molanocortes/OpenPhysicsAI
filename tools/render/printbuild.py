#!/usr/bin/env python3
"""printbuild.py - one part built layer by layer by the lab's inherent-strain LPBF build (lpbf_build_run), for the
metal printing film (tools/render/README.md). Any part: the scenario names its STL, material, strain, cut, supports and
the points to read, each value with its unit in its key and its source.

    python3 tools/render/printbuild.py SCENARIO.json OUTDIR [--element-size MM] [--element-size-z MM] [--stl PATH] [--story]

It drives navier-mcp (embedded, as an agent would) through project_create, geometry_import, geometry_place,
material_define, material_assign, mesh_generate and lpbf_build_run, waits for the job, reads the probes before and after
the cut (results_probe), and leaves in OUTDIR:
    results.nvt      the build's result file: one stored time per layer, then after the cut (and after support removal)
    summary.json     the build's own summary (stored times, tip, peak stress, provenance, scope)
    record.json      what this run computed, beside the public record the scenario quotes: the probes' deflection after
                     the cut, the largest displacement and von Mises stress, the mesh, the time it took
    part.stl         the part as it was built (the scenario's STL, mirrored when asked, minimum corner at the origin)
--element-size overrides the scenario's (a coarse run to measure the cost first); --stl reads the part from another
place (a copy on a compute node), checked against the scenario's SHA-256 all the same. --story reports the layers to
~/bin/story (the owner's phone) when it runs as a job on a compute node. The workspace is deleted when the run passes.
"""
import hashlib
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from mcptest import Client  # noqa: E402


def read_stl(path):
    """triangles of an ASCII or binary STL, as ((x, y, z) * 3) tuples in the file's units"""
    b = Path(path).read_bytes()
    tris = []
    if b[:5] == b"solid" and b"facet" in b[:400]:
        v = []
        for line in b.decode(errors="replace").splitlines():
            w = line.split()
            if w and w[0] == "vertex":
                v.append(tuple(float(x) for x in w[1:4]))
                if len(v) == 3:
                    tris.append(tuple(v))
                    v = []
        return tris
    n = struct.unpack("<I", b[80:84])[0]
    for i in range(n):
        f = struct.unpack("<12f", b[84 + 50 * i:84 + 50 * i + 48])
        tris.append((f[3:6], f[6:9], f[9:12]))
    return tris


def write_stl(path, tris, name):
    with open(path, "wb") as f:
        f.write(name.encode()[:80].ljust(80, b" "))
        f.write(struct.pack("<I", len(tris)))
        for a, b, c in tris:
            u = [b[k] - a[k] for k in range(3)]
            v = [c[k] - a[k] for k in range(3)]
            n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
            ln = math.sqrt(sum(x * x for x in n)) or 1
            f.write(struct.pack("<3f", *[x / ln for x in n]))
            for p in (a, b, c):
                f.write(struct.pack("<3f", *p))
            f.write(b"\0\0")


def bounds(tris):
    pts = [p for t in tris for p in t]
    return [min(p[k] for p in pts) for k in range(3)], [max(p[k] for p in pts) for k in range(3)]


class Mcp:
    """navier-mcp over stdio; a call that fails stops the run with the engine's own error"""

    def __init__(self, ws, allow):
        self.c = Client(["--embedded", "--workspace", str(ws), "--allow-read", str(allow)])
        self.c.initialize()

    def call(self, op, args, timeout=1800.0):
        rid = self.c.next_id
        self.c.next_id += 1
        self.c.send({"jsonrpc": "2.0", "id": rid, "method": "tools/call", "params": {"name": op, "arguments": args}})
        resp = self.c.recv(timeout=timeout)  # meshing a fine part takes longer than the client's 20 s default
        r = resp["result"]["structuredContent"]
        if not r.get("ok"):
            raise SystemExit("%s: %s" % (op, json.dumps(r.get("error"))[:1500]))
        return r.get("value", {})

    def close(self):
        return self.c.close()


def story(*args):
    exe = os.path.expanduser("~/bin/story")
    if os.path.exists(exe):
        subprocess.run([exe] + [str(a) for a in args], capture_output=True)


def main():
    a = sys.argv[1:]
    opts = {"--element-size": None, "--element-size-z": None, "--stl": None}
    flags = {"--story": False, "--keep": False}
    pos = []
    i = 0
    while i < len(a):
        if a[i] in opts:
            opts[a[i]] = a[i + 1] if a[i] == "--stl" else float(a[i + 1])
            i += 2
        elif a[i] in flags:
            flags[a[i]] = True
            i += 1
        else:
            pos.append(a[i])
            i += 1
    if len(pos) != 2:
        print(__doc__)
        return 2
    sc = json.loads(Path(pos[0]).read_text())
    out = Path(pos[1]).resolve()
    out.mkdir(parents=True, exist_ok=True)
    part, build, mat = sc["part"], sc["build"], sc["material"]
    h = opts["--element-size"] or build["element_size_mm"]
    hz = opts["--element-size-z"] or (opts["--element-size"] or build.get("element_size_z_mm", h))

    # the part as built: the scenario's STL, checked against its hash, mirrored when asked, minimum corner at the origin
    stl = Path(os.path.expanduser(opts["--stl"] or part["stl"]))
    sha = hashlib.sha256(stl.read_bytes()).hexdigest()
    if part.get("stl_sha256") and sha != part["stl_sha256"]:
        raise SystemExit("%s: SHA-256 %s, the scenario names %s" % (stl, sha, part["stl_sha256"]))
    tris = read_stl(stl)
    lo, _ = bounds(tris)
    tris = [tuple(tuple(p[k] - lo[k] for k in range(3)) for p in t) for t in tris]
    if part.get("mirror_x"):
        _, hi = bounds(tris)
        tris = [((hi[0] - a_[0], a_[1], a_[2]), (hi[0] - c_[0], c_[1], c_[2]), (hi[0] - b_[0], b_[1], b_[2])) for a_, b_, c_ in tris]
    _, size = bounds(tris)
    write_stl(out / "part.stl", tris, "printbuild %s" % part["name"])
    print("part: %s, %d facets, %.3f x %.3f x %.3f %s, SHA-256 %s" % (stl.name, len(tris), size[0], size[1], size[2], part["units"], sha[:16]))

    ws = out / "ws"
    if ws.exists():
        shutil.rmtree(ws)
    m = Mcp(ws, out)
    t0 = time.time()
    rec = {"what": "printbuild.py: %s" % sc["title"], "scenario": sc, "element_size_mm": [h, h, hz],
           "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "stl_sha256": sha,
           "host": os.uname().nodename, "cpus_allowed": len(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None}
    try:
        m.call("project_create", {"name": "printbuild_%s" % part["name"]})
        body = m.call("geometry_import", {"path": str(out / "part.stl"), "units": part["units"], "name": part["name"]})["body"]
        zoff = float(part.get("z_offset_mm", 0.0))
        if zoff > 0 or part.get("place"):
            pl = dict(part.get("place") or {})
            pl.update({"body": part["name"], "z_offset": "%g mm" % zoff})
            body = m.call("geometry_place", pl).get("body", body)
        tr = (body.get("placement") or {}).get("translation_mm") or [-0.5 * size[0], -0.5 * size[1], zoff]
        m.call("material_define", {"material": {
            "id": mat["id"], "name": mat["name"], "family": "metal", "status": "user_supplied", "processes": ["lpbf"],
            "provenance": mat["source"], "density_kg_m3": {"value": mat["density_kg_m3"]},
            "youngs_modulus_pa": {"value": mat["youngs_modulus_mpa"] * 1e6}, "poisson_ratio": {"value": mat["poissons_ratio"]}}})
        m.call("material_assign", {"body": part["name"], "material": mat["id"], "source": "user"})
        tm = time.time()
        mesh = m.call("mesh_generate", {"element_size": "%g mm" % h, "element_size_z": "%g mm" % hz}, timeout=3600)
        rec["mesh_seconds"] = round(time.time() - tm, 1)
        st = sc["inherent_strain"]
        args = {"body": part["name"], "build_orientation": build.get("build_orientation", "X"),
                "layer_thickness_sim": "%g mm" % build["layer_thickness_mm"],
                "inherent_strain": {k: st[k] for k in ("exx", "eyy", "ezz", "provenance", "source")},
                "material": {"youngs_modulus": "%.9g MPa" % mat["youngs_modulus_mpa"], "poissons_ratio": mat["poissons_ratio"],
                             "provenance": "user"},
                "label": sc["title"][:120]}
        if sc.get("cut"):
            c = sc["cut"]
            args["cut"] = {"height": "%g mm" % c["height_mm"], "kerf": "%g mm" % c["kerf_mm"], "from_x": "%g mm" % c.get("from_x_mm", 0.0),
                           "provenance": c["provenance"]}
        if sc.get("supports"):
            args["supports"] = sc["supports"]
        if build.get("plasticity"):
            args["plasticity"] = build["plasticity"]
        run = m.call("lpbf_build_run", args)
        job = run["job_id"]
        model = run.get("model", {})
        print("build: job %s, %s elements, %s nodes, layers of %g mm, elements %g x %g x %g mm" % (
            job, model.get("elements"), model.get("nodes"), build["layer_thickness_mm"], h, h, hz), flush=True)
        if flags["--story"]:
            story("progress", 0, 1, "--unit", "layers", "--note", "meshed: %s elements" % model.get("elements"))
        tb, last, s = time.time(), None, {}
        while True:
            s = m.call("job_status", {"job_id": job, "wait_seconds": 20}, timeout=120)
            stage = s.get("stage") or s.get("message") or ""
            if stage != last:
                print("  %6.0f s  %s  %s" % (time.time() - tb, s.get("state"), stage), flush=True)
                last = stage
                if flags["--story"] and stage.startswith("layer "):
                    k, n = stage.split()[1], stage.split()[3]
                    story("progress", k, n, "--unit", "layers")
            if s.get("state") not in ("queued", "running"):
                break
        rec["build_seconds"] = round(time.time() - tb, 1)
        if s.get("state") != "succeeded":
            raise SystemExit("the build did not succeed: %s" % json.dumps(s)[:1500])
        summ = s.get("summary") or {}
        res = summ.get("results") or {}
        n = int(res.get("stored_times"))
        layers = int(res.get("layers") or model.get("layers") or (n - 1))
        before, after = layers - 1, n - 1  # the last layer; the last stored time (after the cut, or the supports' removal)
        probes = sc.get("probes") or []
        if probes:
            pts = [[tr[0] + p["x_mm"], tr[1] + p["y_mm"], tr[2] + p["z_mm"]] for p in probes]
            pb = m.call("results_probe", {"job_id": job, "points_mm": pts, "time_index": before})["probes"]
            pa = m.call("results_probe", {"job_id": job, "points_mm": pts, "time_index": after})["probes"]
            if not all(q.get("inside", True) for q in pb + pa):
                raise SystemExit("a probe lies outside the mesh: %s" % json.dumps([q.get("inside") for q in pb + pa]))
            rec["probes"] = []
            for p, x, qb, qa in zip(probes, pts, pb, pa):
                d = qa["displacement_mm"]["z"] - qb["displacement_mm"]["z"]
                rec["probes"].append({"name": p["name"], "point_build_frame_mm": x, "uz_before_cut_mm": qb["displacement_mm"]["z"],
                                      "uz_after_mm": qa["displacement_mm"]["z"], "deflection_mm": d,
                                      "von_mises_before_mpa": qb.get("von_mises_mpa"), "von_mises_after_mpa": qa.get("von_mises_mpa")})
        run_dir = Path(run["run_directory"])
        rec.update({"job_id": job, "stored_times": n, "layers": layers, "time_index_before_cut": before, "time_index_after": after,
                    "translation_mm": tr, "model": model, "mesh": {k: mesh.get(k) for k in ("elements", "nodes", "element_size_mm") if k in mesh},
                    "results": res, "seconds": round(time.time() - t0, 1)})
        shutil.move(str(run_dir / "results.nvt"), str(out / "results.nvt"))
        shutil.copy(str(run_dir / "summary.json"), str(out / "summary.json"))
    finally:
        code, err = m.close()
        if err.strip():
            (out / "navier-mcp.stderr.txt").write_text(err)
    rec["results_nvt_bytes"] = (out / "results.nvt").stat().st_size
    # beside this run's numbers, the public record the scenario quotes, read as it is written there
    r = sc.get("record") or {}
    if rec.get("probes") and r.get("measured"):
        for p in rec["probes"]:
            mv, pv = r["measured"].get(p["name"]), (r.get("predicted_blind_at_0p5mm") or {}).get(p["name"])
            if mv is not None:
                p["measured_mm"] = mv
                p["this_run_against_measured_percent"] = 100.0 * (p["deflection_mm"] - mv) / abs(mv)
            if pv is not None:
                p["public_blind_prediction_mm"] = pv
    (out / "record.json").write_text(json.dumps(rec, indent=1) + "\n")
    print("results: %d stored times (%d layers), %.1f MB, build %.0f s, all %.0f s" % (
        n, layers, rec["results_nvt_bytes"] / 1e6, rec["build_seconds"], rec["seconds"]))
    for k in ("tip_uz_before_cut_mm", "tip_uz_after_cut_mm", "springback_mm", "max_displacement_before_cut_mm",
              "max_displacement_after_cut_mm", "peak_von_mises_before_cut_mpa", "peak_von_mises_after_cut_mpa", "equilibrium_error_last_solve"):
        if k in res:
            print("  %s %s" % (k, res[k]))
    for p in rec.get("probes", []):
        print("  %-8s deflection after the cut %.5f mm%s" % (p["name"], p["deflection_mm"], "" if "measured_mm" not in p else
              "  (measured %.4f mm, this run %+.1f %%; public blind prediction %s mm)" % (
                  p["measured_mm"], p["this_run_against_measured_percent"], p.get("public_blind_prediction_mm"))))
    if not flags["--keep"]:
        shutil.rmtree(ws, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
