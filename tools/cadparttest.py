#!/usr/bin/env python3
"""cadparttest.py - GOALS.md G23 step 2: exact parts from a feature list, through build/openphysics-cad as the engine runs it.

    make build/openphysics-cad && python3 tools/cadparttest.py      (make test-cad runs it)

Criteria, written 2026-10-08 before the first run (GOALS.md G23 step 2), amended only openly below them:
P1 a 60 x 40 x 8 mm plate, a boss of radius 8 mm standing 20 mm from the plate's floor, both through a hole of radius
   4 mm: every feature's measured volume is its closed form (19200, 1280 pi, the union 19200 + 768 pi, the hole's tool
   640 pi, the part 19200 + 448 pi mm^3) within 1e-9 relative
P2 the plate's four vertical outer edges, chosen by a selector (edges along z between two planes) and not by index,
   rounded at 5 mm: 18400 + 648 pi mm^3 within 1e-7 relative, and the selector picks exactly those four
P3 an L-shaped profile sketched in the xz plane and revolved a full turn about z: the sum of 2 pi R A over its two
   rectangles (Pappus) within 1e-9 relative
P4 a 40 x 40 x 20 mm box shelled to 2 mm walls, open at its top face (chosen by a selector): 8672 mm^3 within 1e-9
   relative
P5 the part of P1 as STEP read back: one valid solid of the same volume within 1e-9 relative, and as STL: closed and
   manifold, its volume within 0.1 per cent
P6 an unknown operation, a missing size and a rounding radius larger than the part allows are each refused with the
   feature's id and the reason, and no file is written
P7 the final record names every face with its kind and parameters (P1's hole: a cylinder of radius 4 mm along z through
   x 30, y 20)
"""
import json, math, pathlib, shutil, struct, subprocess, sys, tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
CAD = ROOT / "build" / "openphysics-cad"
failures = []
PI = math.pi


def check(ok, what):
    print(("  ok    " if ok else "  FAIL  ") + what)
    if not ok:
        failures.append(what)
    return ok


def rel(a, b):
    return abs(a - b) / abs(b)


def run(features, out, result=None):
    model = {"features": features}
    if result:
        model["result"] = result
    f = out / "model.json"
    f.write_text(json.dumps(model))
    r = subprocess.run([str(CAD), "part", str(f), str(out)], capture_output=True, text=True, timeout=300)
    try:
        return json.loads(r.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        return {"ok": False, "error": {"message": "no record: %s" % (r.stderr or r.stdout)[-300:]}}


def stl_volume(path):
    b = pathlib.Path(path).read_bytes()
    n = struct.unpack("<I", b[80:84])[0]
    v = 0
    for i in range(n):
        x = struct.unpack("<12f", b[84 + 50 * i:84 + 50 * i + 48])
        a, p, q = x[3:6], x[6:9], x[9:12]
        v += (a[0] * (p[1] * q[2] - p[2] * q[1]) - a[1] * (p[0] * q[2] - p[2] * q[0]) + a[2] * (p[0] * q[1] - p[1] * q[0])) / 6
    return v


PLATE = [
    {"id": "plate", "op": "box", "size_mm": [60, 40, 8]},
    {"id": "boss", "op": "cylinder", "radius_mm": 8, "height_mm": 20, "at_mm": [30, 20, 0], "axis": "z"},
    {"id": "body", "op": "union", "of": ["plate", "boss"]},
    {"id": "bore", "op": "cylinder", "radius_mm": 4, "height_mm": 40, "at_mm": [30, 20, -10], "axis": "z"},
    {"id": "part", "op": "cut", "from": "body", "remove": ["bore"]},
]


def main():
    if not CAD.exists():
        print("cadparttest: build/openphysics-cad is not built (make build/openphysics-cad)")
        return 2
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="openphysics-cadpart-"))
    try:
        print("P1 a plate, a boss and a hole")
        r = run(PLATE, tmp)
        want = {"plate": 19200, "boss": 1280 * PI, "body": 19200 + 768 * PI, "bore": 640 * PI, "part": 19200 + 448 * PI}
        got = {f["id"]: f["volume_mm3"] for f in r.get("features", [])}
        for k, v in want.items():
            check(k in got and rel(got[k], v) < 1e-9, "%s: %.10g mm^3 against %.10g" % (k, got.get(k, float("nan")), v))
        part = r.get("part") or {}
        check(r.get("ok") and rel(part.get("volume_mm3", 0), want["part"]) < 1e-9 and part.get("valid") and part.get("solids") == 1,
              "the part: ok, one valid solid of %.10g mm^3" % part.get("volume_mm3", float("nan")))

        print("P7 the faces named")
        holes = [f for f in part.get("named_faces", []) if f.get("kind") == "cylinder" and abs(f.get("radius_mm", 0) - 4) < 1e-9]
        h = holes[0] if holes else {}
        ax, th = h.get("axis", [0, 0, 0]), h.get("through_mm", [0, 0, 0])
        check(len(holes) == 1 and abs(abs(ax[2]) - 1) < 1e-12 and abs(th[0] - 30) < 1e-9 and abs(th[1] - 20) < 1e-9,
              "the hole: a cylinder of radius 4 mm along z through (30, 20): %s" % json.dumps(h)[:160])
        kinds = sorted({f.get("kind") for f in part.get("named_faces", [])})
        check(all(f.get("name") and f.get("kind") and f.get("area_mm2", 0) > 0 for f in part.get("named_faces", [])) and kinds == ["cylinder", "plane"],
              "every face named with its kind and area (%d faces, kinds %s)" % (len(part.get("named_faces", [])), kinds))

        print("P5 STEP and STL")
        step, stl = tmp / "part.step", tmp / "part.stl"
        m = subprocess.run([str(CAD), "measure", str(step)], capture_output=True, text=True, timeout=120)
        mp = (json.loads(m.stdout.strip().splitlines()[-1]) if m.stdout.strip() else {}).get("part", {})
        check(mp.get("solids") == 1 and mp.get("valid") and rel(mp.get("volume_mm3", 0), want["part"]) < 1e-9,
              "STEP read back: %s solid(s), valid %s, %.10g mm^3" % (mp.get("solids"), mp.get("valid"), mp.get("volume_mm3", float("nan"))))
        sr = r.get("stl") or {}
        sv = stl_volume(stl) if stl.exists() else 0
        check(sr.get("closed_manifold") and rel(sv, want["part"]) < 1e-3,
              "STL: %s triangles, closed and manifold %s, %.6g mm^3 (relative %.2e)" % (sr.get("triangles"), sr.get("closed_manifold"), sv, rel(sv, want["part"]) if sv else 1))

        print("P2 four vertical edges rounded, chosen by a selector")
        r = run(PLATE + [{"id": "round", "op": "fillet", "of": "part", "edges": {"parallel": "z", "between": "planes"}, "radius_mm": 5}], tmp)
        rf = [f for f in r.get("features", []) if f["id"] == "round"]
        v = rf[0]["volume_mm3"] if rf else float("nan")
        check(r.get("ok") and rf and rf[0].get("chosen") == 4 and rel(v, 18400 + 648 * PI) < 1e-7,
              "chose %s edges; %.10g mm^3 against %.10g" % (rf[0].get("chosen") if rf else None, v, 18400 + 648 * PI))

        print("P3 an L-shaped profile revolved (Pappus)")
        r = run([{"id": "L", "op": "sketch", "plane": "xz", "points_mm": [[10, 0], [30, 0], [30, 5], [15, 5], [15, 20], [10, 20]]},
                 {"id": "ring", "op": "revolve", "sketch": "L", "axis": "z"}], tmp)
        e = 2 * PI * (20 * 100 + 12.5 * 75)
        v = (r.get("part") or {}).get("volume_mm3", float("nan"))
        check(r.get("ok") and rel(v, e) < 1e-9, "%.10g mm^3 against %.10g" % (v, e))

        print("P4 a box shelled, open at the top")
        r = run([{"id": "box", "op": "box", "size_mm": [40, 40, 20]},
                 {"id": "cup", "op": "shell", "of": "box", "open_faces": {"extreme": "+z", "kind": "plane"}, "thickness_mm": 2}], tmp)
        cf = [f for f in r.get("features", []) if f["id"] == "cup"]
        v = (r.get("part") or {}).get("volume_mm3", float("nan"))
        check(r.get("ok") and cf and cf[0].get("chosen") == 1 and rel(v, 8672) < 1e-9, "%.10g mm^3 against 8672, %s face(s) open" % (v, cf[0].get("chosen") if cf else None))

        print("P6 refusals")
        cases = [
            ([{"id": "w", "op": "wedge", "size_mm": [1, 1, 1]}], "w", "not known"),
            ([{"id": "b", "op": "box"}], "b", "size_mm"),
            ([{"id": "b", "op": "box", "size_mm": [10, 10, 10]},
              {"id": "r", "op": "fillet", "of": "b", "edges": {"parallel": "z"}, "radius_mm": 50}], "r", "round"),
        ]
        for features, fid, word in cases:
            out = tmp / ("refuse-" + fid)
            out.mkdir()
            r = run(features, out)
            err = r.get("error") or {}
            wrote = sorted(p.name for p in out.iterdir() if p.name != "model.json")
            check(r.get("ok") is False and err.get("feature") == fid and word in err.get("message", "") and not wrote,
                  "refused at %s: %s; files written %s" % (err.get("feature"), err.get("message"), wrote))
    finally:
        if not failures:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print("kept for a look: %s" % tmp)
    print("cadparttest: %s" % ("all checks passed" if not failures else "%d failed" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
