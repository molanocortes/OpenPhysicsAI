#!/usr/bin/env python3
"""cadprinttest.py - GOALS.md G23 step 6: can a part be printed as it is, through build/openphysics-cad and openphysics-mcp.

    make build/openphysics-cad openphysics-mcp && python3 tools/cadprinttest.py      (make test-cad runs it; about three minutes)

Criteria, written 2026-10-08 before the first run (GOALS.md G23 step 6), amended only openly below them:
K1 a sphere of radius 10 mm resting on the plate, build direction +z, overhang angle 45 degrees: the overhanging area
   2 pi R^2 (1 - cos 45 deg) = 184.030 mm^2 within 1 per cent and the support under it pi R^3 (sqrt 2 - 1) / 6 =
   216.881 mm^3 within 1 per cent; a prism 10 mm deep whose two undersides slope at 30 and at 60 degrees: only the 30
   degree one overhangs, 200 mm^2, with 1000 sqrt 3 mm^3 of support, both within 1e-9 relative
K2 a 40 x 40 x 5 mm top on a 10 x 10 x 20 mm column: built along +z 1500 mm^2 of overhang and 30000 mm^3 of support,
   along +x, -x, +y and -y 200 mm^2 and 3000 mm^3, along -z none, all within 1e-9 relative (zero within 1e-9 mm^2 and
   mm^3), and -z named the best of the six
K3 a fin 0.5 mm thick, 15 mm long and 8 mm tall on a block, minimum wall 0.8 mm: the thinnest wall 0.5 mm within 1e-9
   mm, the thin area the fin's two sides, 240 mm^2 within 1e-9 relative, in two regions, nothing else thin; a tube of
   outer radius 10 mm with a 0.4 mm wall, 10 mm tall: the thinnest wall within 0.02 mm of 0.4 and the thin area its
   two cylindrical faces, 2 pi (10 + 9.6) 10 mm^2 within 1 per cent
K4 a 30 mm cube with a spherical void of radius 5 mm at its centre: one void, 4/3 pi 125 mm^3 within 0.5 per cent, its
   way out 10 mm through within 0.05 mm, an issue (trapped powder) for powder_bed_fusion and none for
   material_extrusion; step 3's L5 plate with its 1 mm skin and gyroid core: the outer surface's volume less the
   voids' equal to the written solid's volume within 1e-5 relative, the outer surface's volume within 0.5 per cent of
   the part's closed form; the same plate made again with drains of 1 mm radius placed by cad_lattice itself: its
   record says no enclosed void is left, the result is closed and manifold, and the check of that result finds no void
K5 a request without a process, without overhang_deg or without min_wall_mm, an unknown process and a surface that is
   not closed are each refused with the reason, nothing written
K6 K4's check of the latticed plate takes under 120 s on the laptop, its time reported
K7 through openphysics-mcp: cad_check offered in the default and lean profiles, and K2's part made with cad_part and
   checked with cad_check gives K2's numbers

Runs, 2026-10-08, no criterion changed: the first failed K4's drained plate (holes cut from the mesh with Manifold's
booleans ran tangent to the skin's flat inner faces and left slivers that the 32-bit STL joined into 12 wrong edges);
the holes were then drilled in the level set, and the second run left 4 tiny voids that touched the outside at one
point, taken as their way out at distance 0; with such points passed over, the third run passed every check.
"""
import json, math, os, pathlib, shutil, struct, subprocess, sys, tempfile, time

ROOT = pathlib.Path(__file__).resolve().parents[1]
CAD = ROOT / "build" / "openphysics-cad"
MCP = ROOT / "openphysics-mcp"
PI = math.pi
failures = []


def check(ok, what):
    print(("  ok    " if ok else "  FAIL  ") + what)
    if not ok:
        failures.append(what)
    return ok


def rel(a, b):
    return abs(a - b) / abs(b)


def last_record(text):
    for line in reversed(text.strip().splitlines()):
        if line.startswith("{"):
            return json.loads(line)
    return {"ok": False, "error": {"message": "no record: " + text[-300:]}}


def cad(command, request, out, timeout=900):
    out.mkdir(parents=True, exist_ok=True)
    f = out / ("model.json" if command == "part" else "request.json")
    f.write_text(json.dumps(request))
    r = subprocess.run([str(CAD), command, str(f), str(out)], capture_output=True, text=True, timeout=timeout)
    return last_record(r.stdout or r.stderr)


def part(features, out):
    r = cad("part", {"features": features}, out)
    assert r.get("ok"), r
    return out / "part.step"


def pbf(path, **kw):
    req = {"part": str(path), "process": "powder_bed_fusion", "overhang_deg": 45, "min_wall_mm": 0.4}
    req.update(kw)
    return req


TABLE = [{"id": "top", "op": "box", "size_mm": [40, 40, 5], "at_mm": [0, 0, 20]},
         {"id": "column", "op": "box", "size_mm": [10, 10, 20], "at_mm": [15, 15, 0]},
         {"id": "table", "op": "union", "of": ["top", "column"]}]
PLATE = [{"id": "plate", "op": "box", "size_mm": [60, 40, 8]},
         {"id": "boss", "op": "cylinder", "radius_mm": 8, "height_mm": 20, "at_mm": [30, 20, 0], "axis": "z"},
         {"id": "body", "op": "union", "of": ["plate", "boss"]},
         {"id": "bore", "op": "cylinder", "radius_mm": 4, "height_mm": 40, "at_mm": [30, 20, -10], "axis": "z"},
         {"id": "part", "op": "cut", "from": "body", "remove": ["bore"]}]


def k1(tmp):
    print("K1 a sphere, and a prism with undersides at 30 and 60 degrees")
    step = part([{"id": "ball", "op": "sphere", "radius_mm": 10, "centre_mm": [0, 0, 10]}], tmp / "k1s")
    r = cad("check", pbf(step), tmp / "k1s" / "check")
    o = r.get("overhang") or {}
    want_a, want_s = 2 * PI * 100 * (1 - math.cos(PI / 4)), PI * 1000 * (math.sqrt(2) - 1) / 6
    check(r.get("ok") and rel(o.get("area_mm2", 0), want_a) < 0.01,
          "sphere's overhang %.4f mm^2 against %.4f (%.3f %%) %s" % (o.get("area_mm2", float("nan")), want_a, 100 * rel(o.get("area_mm2", 0), want_a), (r.get("error") or {}).get("message", "")))
    check(r.get("ok") and rel(o.get("support_mm3", 0), want_s) < 0.01,
          "sphere's support %.4f mm^3 against %.4f (%.3f %%)" % (o.get("support_mm3", float("nan")), want_s, 100 * rel(o.get("support_mm3", 0), want_s)))
    a60, a30 = 30 + 10 / math.tan(PI / 3), 20 - 10 / math.tan(PI / 6)
    step = part([{"id": "profile", "op": "sketch", "plane": "xz",
                  "points_mm": [[20, 0], [30, 0], [30, 5], [a60, 15], [a60, 20], [a30, 20], [a30, 15], [20, 5]]},
                 {"id": "prism", "op": "extrude", "sketch": "profile", "distance_mm": 10}], tmp / "k1p")
    r = cad("check", pbf(step), tmp / "k1p" / "check")
    o = r.get("overhang") or {}
    regs = o.get("regions") or []
    check(r.get("ok") and rel(o.get("area_mm2", 0), 200) < 1e-9 and o.get("region_count") == 1 and abs(regs[0].get("lowest_angle_deg", 0) - 30) < 1e-6,
          "prism's overhang %.12g mm^2 against 200, %s region(s), at %s degrees %s" % (o.get("area_mm2", float("nan")), o.get("region_count"),
                                                                                     regs[0].get("lowest_angle_deg") if regs else None, (r.get("error") or {}).get("message", "")))
    check(r.get("ok") and rel(o.get("support_mm3", 0), 1000 * math.sqrt(3)) < 1e-9,
          "prism's support %.12g mm^3 against %.12g" % (o.get("support_mm3", float("nan")), 1000 * math.sqrt(3)))


def k2(tmp):
    print("K2 a table: its overhang and support along each axis")
    step = part(TABLE, tmp / "k2")
    r = cad("check", pbf(step), tmp / "k2" / "check")
    o = r.get("overhang") or {}
    check(r.get("ok") and rel(o.get("area_mm2", 0), 1500) < 1e-9 and rel(o.get("support_mm3", 0), 30000) < 1e-9,
          "along +z: %.12g mm^2, %.12g mm^3 against 1500 and 30000 %s" % (o.get("area_mm2", float("nan")), o.get("support_mm3", float("nan")), (r.get("error") or {}).get("message", "")))
    want = {"+x": (200, 3000), "-x": (200, 3000), "+y": (200, 3000), "-y": (200, 3000), "+z": (1500, 30000), "-z": (0, 0)}
    got = {x["build_direction"]: (x["overhang_mm2"], x["support_mm3"]) for x in r.get("orientations", [])}
    for k, (a, s) in want.items():
        ga, gs = got.get(k, (float("nan"), float("nan")))
        ok = (abs(ga) < 1e-9 and abs(gs) < 1e-9) if a == 0 else (rel(ga, a) < 1e-9 and rel(gs, s) < 1e-9)
        check(ok, "along %s: %.12g mm^2, %.12g mm^3 against %g and %g" % (k, ga, gs, a, s))
    check(r.get("best_orientation") == "-z", "the best of the six: %s" % r.get("best_orientation"))
    return step, r


def k3(tmp):
    print("K3 a thin fin, and a thin tube")
    step = part([{"id": "block", "op": "box", "size_mm": [20, 20, 10]},
                 {"id": "fin", "op": "box", "size_mm": [15, 0.5, 8], "at_mm": [2.5, 9.75, 10]},
                 {"id": "both", "op": "union", "of": ["block", "fin"]}], tmp / "k3f")
    r = cad("check", pbf(step, min_wall_mm=0.8), tmp / "k3f" / "check")
    w = r.get("walls") or {}
    check(r.get("ok") and abs(w.get("thinnest_mm", 0) - 0.5) < 1e-9, "fin: thinnest %.12g mm against 0.5 %s" % (w.get("thinnest_mm", float("nan")), (r.get("error") or {}).get("message", "")))
    check(r.get("ok") and rel(w.get("thin_area_mm2", 0), 240) < 1e-9 and w.get("region_count") == 2,
          "fin: thin area %.12g mm^2 against 240, in %s regions" % (w.get("thin_area_mm2", float("nan")), w.get("region_count")))
    step = part([{"id": "outer", "op": "cylinder", "radius_mm": 10, "height_mm": 10, "at_mm": [0, 0, 0], "axis": "z"},
                 {"id": "inner", "op": "cylinder", "radius_mm": 9.6, "height_mm": 20, "at_mm": [0, 0, -5], "axis": "z"},
                 {"id": "tube", "op": "cut", "from": "outer", "remove": ["inner"]}], tmp / "k3t")
    r = cad("check", pbf(step, min_wall_mm=0.8), tmp / "k3t" / "check")
    w = r.get("walls") or {}
    want = 2 * PI * (10 + 9.6) * 10
    check(r.get("ok") and abs(w.get("thinnest_mm", 0) - 0.4) <= 0.02, "tube: thinnest %.6f mm against 0.4 %s" % (w.get("thinnest_mm", float("nan")), (r.get("error") or {}).get("message", "")))
    check(r.get("ok") and rel(w.get("thin_area_mm2", 0), want) < 0.01, "tube: thin area %.4f mm^2 against %.4f (%.3f %%)" % (w.get("thin_area_mm2", float("nan")), want, 100 * rel(w.get("thin_area_mm2", 0), want)))


def k4(tmp):
    print("K4 enclosed voids: a cube with a hidden sphere, and the latticed plate")
    step = part([{"id": "cube", "op": "box", "size_mm": [30, 30, 30]},
                 {"id": "ball", "op": "sphere", "radius_mm": 5, "centre_mm": [15, 15, 15]},
                 {"id": "hollow", "op": "cut", "from": "cube", "remove": ["ball"]}], tmp / "k4c")
    r = cad("check", pbf(step), tmp / "k4c" / "check")
    v = r.get("voids") or {}
    one = (v.get("list") or [{}])[0]
    want = 4 / 3 * PI * 125
    check(r.get("ok") and v.get("count") == 1 and rel(v.get("volume_mm3", 0), want) < 0.005,
          "one void: %s, %.4f mm^3 against %.4f %s" % (v.get("count"), v.get("volume_mm3", float("nan")), want, (r.get("error") or {}).get("message", "")))
    th = (one.get("way_out") or {}).get("through_mm", float("nan"))
    check(abs(th - 10) <= 0.05, "its way out: %.4f mm through against 10" % th)
    check(r.get("traps_material") is True and any("powder" in s for s in r.get("issues", [])), "powder_bed_fusion: an issue, %s" % r.get("issues"))
    r = cad("check", dict(pbf(step), process="material_extrusion"), tmp / "k4c" / "check-fff")
    check(r.get("ok") and r.get("traps_material") is False and not any("void" in s for s in r.get("issues", [])),
          "material_extrusion: no issue (%s; notes %s)" % (r.get("issues"), r.get("notes")))

    plate = part(PLATE, tmp / "k4p")
    lat = {"part": str(plate), "skin_mm": 1, "resolution_mm": 0.15, "core": {"kind": "gyroid", "form": "sheet", "cell_mm": 6, "solid_fraction": 0.3}}
    lr = cad("lattice", lat, tmp / "k4p" / "gyroid")
    if not check(lr.get("ok"), "the latticed plate made (%s s) %s" % (lr.get("seconds"), (lr.get("error") or {}).get("message", ""))):
        return
    t0 = time.time()
    r = cad("check", pbf(tmp / "k4p" / "gyroid" / "infilled.stl"), tmp / "k4p" / "gyroid" / "check")
    wall = time.time() - t0
    v = r.get("voids") or {}
    solid = (r.get("part") or {}).get("volume_mm3", float("nan"))
    written = (lr.get("result") or {}).get("written_volume_mm3", float("nan"))
    outer = v.get("outer_volume_mm3", float("nan"))
    closed_form = 19200 + 448 * PI
    print("    %s voids, %.2f mm^3, keeping %s; the lattice's own count %s" % (v.get("count"), v.get("volume_mm3", float("nan")), v.get("keeps"), (lr.get("result") or {}).get("enclosed_voids")))
    check(r.get("ok") and rel(outer - v.get("volume_mm3", 0), written) < 1e-5,
          "outer %.6f less voids %.6f = %.6f against the written solid %.6f %s" % (outer, v.get("volume_mm3", float("nan")), outer - v.get("volume_mm3", 0), written, (r.get("error") or {}).get("message", "")))
    check(r.get("ok") and rel(outer, closed_form) < 0.005, "the outer surface's volume %.4f against the part's %.4f (%.3f %%)" % (outer, closed_form, 100 * rel(outer, closed_form)))
    print("K6 its time")
    check(r.get("ok") and r.get("seconds", 1e9) < 120, "the check took %.1f s (the record: %s s), under 120" % (wall, r.get("seconds")))

    lat["drains"] = {"radius_mm": 1}
    dr = cad("lattice", lat, tmp / "k4p" / "drained")
    res, d = dr.get("result") or {}, dr.get("drains") or {}
    check(dr.get("ok") and res.get("enclosed_voids") == 0 and d.get("enclosed_voids_after") == 0 and res.get("closed_manifold") and res.get("stl_closed_manifold"),
          "drained: %s holes in %s rounds, voids %s before and %s after, closed and manifold %s/%s %s" % (
              d.get("count"), d.get("rounds"), d.get("enclosed_voids_before"), d.get("enclosed_voids_after"), res.get("closed_manifold"),
              res.get("stl_closed_manifold"), (dr.get("error") or {}).get("message", "")))
    r = cad("check", pbf(tmp / "k4p" / "drained" / "infilled.stl"), tmp / "k4p" / "drained" / "check")
    check(r.get("ok") and (r.get("voids") or {}).get("count") == 0, "the check of the drained plate: %s voids %s" % ((r.get("voids") or {}).get("count"), (r.get("error") or {}).get("message", "")))


def k5(tmp, table_step):
    print("K5 refusals")
    base = pbf(table_step)
    stl = table_step.parent / "part.stl"
    b = stl.read_bytes()
    n = struct.unpack("<I", b[80:84])[0]
    opened = tmp / "open.stl"
    opened.write_bytes(b[:80] + struct.pack("<I", n - 1) + b[84:84 + 50 * (n - 1)])
    cases = [("process", {k: v for k, v in base.items() if k != "process"}, "process"),
             ("overhang", {k: v for k, v in base.items() if k != "overhang_deg"}, "overhang_deg"),
             ("wall", {k: v for k, v in base.items() if k != "min_wall_mm"}, "min_wall_mm"),
             ("unknown", dict(base, process="fdm"), "not one checked here"),
             ("open", dict(base, part=str(opened)), "not closed")]
    for name, req, word in cases:
        out = tmp / ("refuse-" + name)
        r = cad("check", req, out)
        wrote = sorted(p.name for p in out.iterdir() if p.name != "request.json")
        check(r.get("ok") is False and word in (r.get("error") or {}).get("message", "") and not wrote,
              "%s: %s; files written %s" % (name, (r.get("error") or {}).get("message"), wrote))


class Mcp:
    def __init__(self, tmp, lean):
        args = [str(MCP), "--embedded", "--workspace", str(tmp / "ws"), "--lab", str(ROOT), "--lab-results", str(tmp / "res")]
        self.p = subprocess.Popen(args + (["--lean"] if lean else []), cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True)
        self.n = 0
        self.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "cadprinttest", "version": "1"}})
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
        self.p.stdin.flush()

    def rpc(self, method, params=None):
        self.n += 1
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.n, "method": method, "params": params or {}}) + "\n")
        self.p.stdin.flush()
        while True:
            m = json.loads(self.p.stdout.readline())
            if m.get("id") == self.n:
                return m

    def call(self, name, args):
        r = self.rpc("tools/call", {"name": name, "arguments": args})["result"]
        return r.get("structuredContent") or json.loads(r["content"][-1]["text"])

    def stop(self):
        self.p.kill()
        self.p.wait()


def k7(tmp):
    print("K7 through openphysics-mcp")
    servers = []
    try:
        for lean in (False, True):
            m = Mcp(tmp, lean)
            servers.append(m)
            names = {t["name"] for t in m.rpc("tools/list")["result"]["tools"]}
            check("cad_check" in names, "%s profile offers cad_check" % ("lean" if lean else "default"))
        m = servers[0]
        r = m.call("cad_part", {"name": "table", "model": {"features": TABLE}})
        check(r.get("ok"), "cad_part made the table (%s)" % (r.get("error") or {}).get("message", ""))
        r = m.call("cad_check", {"name": "table_check", "part": "table", "process": "powder_bed_fusion", "overhang_deg": 45, "min_wall_mm": 0.4})
        o = (r.get("value") or {}).get("overhang") or {}
        check(r.get("ok") and rel(o.get("area_mm2", 0), 1500) < 1e-9 and rel(o.get("support_mm3", 0), 30000) < 1e-9 and (r.get("value") or {}).get("best_orientation") == "-z",
              "cad_check: %s mm^2, %s mm^3, best %s (%s)" % (o.get("area_mm2"), o.get("support_mm3"), (r.get("value") or {}).get("best_orientation"), (r.get("error") or {}).get("message", "")))
    finally:
        for s in servers:
            s.stop()


def main():
    if not CAD.exists():
        print("cadprinttest: build/openphysics-cad is not built (make build/openphysics-cad)")
        return 2
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="openphysics-cadprint-"))
    try:
        k1(tmp)
        table_step, _ = k2(tmp)
        k3(tmp)
        k5(tmp, table_step)
        if MCP.exists():
            k7(tmp)
        else:
            check(False, "K7 needs openphysics-mcp (make openphysics-mcp)")
        k4(tmp)
    finally:
        if not failures:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print("kept for a look: %s" % tmp)
    print("cadprinttest: %s" % ("all checks passed" if not failures else "%d failed" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
