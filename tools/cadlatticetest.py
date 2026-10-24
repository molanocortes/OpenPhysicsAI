#!/usr/bin/env python3
"""cadlatticetest.py - GOALS.md G23 step 3: a printed part's skin and lattice core, through build/openphysics-cad.

    make build/openphysics-cad && python3 tools/cadlatticetest.py      (make test-cad runs it; a few minutes)

Criteria, written 2026-10-08 before the first run (GOALS.md G23 step 3), amended only openly below them:
L1 a 40 mm cube centred on the origin, no skin, its core the gyroid's network at level 0 (cell 10 mm): the core's solid
   fraction 0.5 within 0.005 (exactly 0.5 by the surface's symmetry p -> -p)
L2 the gyroid's sheet, cell 6 mm, wall 0.6 mm: the solid fraction measured from the mesh within 1 per cent of an
   independent count of the same field on a grid five times finer, and within 3 per cent of the thin-sheet value
   3.0919 x 0.6 / 6 (the gyroid's area per volume, 3.0919 / a for a cell of side a)
L3 a 40 mm cube with a 1 mm skin and nothing inside: 40^3 - 38^3 = 9128 mm^3 within 0.5 per cent
L4 every result is closed and manifold, by Manifold's own status and by a count of the written STL's edges (every edge
   in exactly two triangles, once each way)
L5 the plate of step 2's P1 from its STEP file, a 1 mm skin and a gyroid sheet core asked for at a solid fraction of
   0.3: the core measured at 0.3 within 0.02, every vertex of the result inside the part (within one grid step of its
   surface), the skin nowhere thinner than 0.9 mm where sampled
L6 a wall thinner than two grid steps, a solid fraction outside 0.05 to 0.95, an unknown kind and a missing part are
   each refused with the reason, nothing written
L7 L5's time on the laptop reported, a failure above 600 s

The first run (2026-10-08) failed three checks, each a fault of the program, fixed without changing a criterion: L2's
sheet came out 4.8 per cent under the thin-sheet value because the field took G / |grad G| at the point for the distance
to the surface, which made a 0.6 mm wall 0.5785 mm thick everywhere (measured along the normal); the field now projects
the point onto the surface (two Newton steps), and a 0.6 mm wall measures 0.5994 mm. L2's independent count counts the
same field, so it follows that change. L4 failed on L5's STL: 1459 points where two pieces of the solid touch, kept apart
by Manifold as copies of one vertex, were joined by an STL reader; each copy is now moved 0.1 micrometre into its own
piece before writing, and the program's own check reads the mesh as an STL reader does. L5's thinnest skin read
0.037 mm because samples within a grid step of a sharp edge started outside the edge the level set rounds; the depth is
now measured from where the solid begins under the sample, and that start is reported apart.
"""
import json, math, pathlib, shutil, subprocess, sys, tempfile, time

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]
CAD = ROOT / "build" / "openphysics-cad"
failures = []


def check(ok, what):
    print(("  ok    " if ok else "  FAIL  ") + what)
    if not ok:
        failures.append(what)
    return ok


def cad(args, timeout=900):
    r = subprocess.run([str(CAD)] + [str(a) for a in args], capture_output=True, text=True, timeout=timeout)
    try:
        return json.loads(r.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        return {"ok": False, "error": {"message": "no record: %s" % (r.stderr or r.stdout)[-300:]}}


def part(tmp, name, features):
    out = tmp / name
    out.mkdir()
    (out / "model.json").write_text(json.dumps({"features": features}))
    r = cad(["part", out / "model.json", out])
    if not r.get("ok"):
        raise SystemExit("cadlatticetest: the part %s was not made: %s" % (name, r.get("error")))
    return out / "part.step"


def lattice(tmp, name, req):
    out = tmp / name
    out.mkdir()
    (out / "request.json").write_text(json.dumps(req))
    t = time.time()
    r = cad(["lattice", out / "request.json", out])
    r["_seconds"] = time.time() - t
    r["_dir"] = out
    return r


def stl_closed(path):
    """every edge of the written STL in exactly two triangles, once each way (vertices joined where their floats agree)"""
    b = np.fromfile(path, dtype=np.uint8)
    n = int(np.frombuffer(b[80:84].tobytes(), dtype="<u4")[0])
    rec = np.frombuffer(b[84:84 + 50 * n].tobytes(), dtype=np.dtype([("n", "<f4", 3), ("v", "<f4", (3, 3)), ("a", "<u2")]))
    verts = rec["v"].reshape(-1, 3)
    _, ids = np.unique(verts, axis=0, return_inverse=True)
    tri = ids.reshape(-1, 3)
    edges = np.concatenate([tri[:, [0, 1]], tri[:, [1, 2]], tri[:, [2, 0]]])
    if np.any(edges[:, 0] == edges[:, 1]):
        return False
    key = edges[:, 0].astype(np.int64) * (ids.max() + 1) + edges[:, 1]
    rev = edges[:, 1].astype(np.int64) * (ids.max() + 1) + edges[:, 0]
    if len(np.unique(key)) != len(key):  # an edge twice in the same direction
        return False
    return bool(np.all(np.isin(rev, key)))


def gyroid_sheet_fraction(cell, wall, step):
    """the same field as lattice.cpp (the distance to the surface G = 0 by two Newton steps onto it), counted
    independently on a grid of the given step over one periodic cell"""
    n = int(round(cell / step))
    k = 2 * math.pi / cell
    c = (np.arange(n) + 0.5) * cell / n
    x, y = np.meshgrid(c, c, indexing="ij")

    def g_and_grad(px, py, pz):
        sx, cx, sy, cy, sz, cz = np.sin(k * px), np.cos(k * px), np.sin(k * py), np.cos(k * py), np.sin(k * pz), np.cos(k * pz)
        return sx * cy + sy * cz + sz * cx, k * (cx * cy - sz * sx), k * (-sx * sy + cy * cz), k * (-sy * sz + cz * cx)
    inside = 0
    for zz in c:  # one slab at a time: the laptop's memory
        z = np.full_like(x, zz)
        g0, gx, gy, gz = g_and_grad(x, y, z)
        floor = (0.2 * k) ** 2
        far = np.abs(g0) / np.sqrt(np.maximum(gx * gx + gy * gy + gz * gz, floor)) > 0.5 * cell
        qx, qy, qz = x.copy(), y.copy(), z.copy()
        for _ in range(2):
            g, gx, gy, gz = g_and_grad(qx, qy, qz)
            s = g / np.maximum(gx * gx + gy * gy + gz * gz, floor)
            qx, qy, qz = qx - s * gx, qy - s * gy, qz - s * gz
        d = np.sqrt((x - qx) ** 2 + (y - qy) ** 2 + (z - qz) ** 2)
        d = np.where(far, np.inf, d)
        inside += int(np.count_nonzero(wall / 2 - d > 0))
    return inside / float(n ** 3)


PLATE = [
    {"id": "plate", "op": "box", "size_mm": [60, 40, 8]},
    {"id": "boss", "op": "cylinder", "radius_mm": 8, "height_mm": 20, "at_mm": [30, 20, 0], "axis": "z"},
    {"id": "body", "op": "union", "of": ["plate", "boss"]},
    {"id": "bore", "op": "cylinder", "radius_mm": 4, "height_mm": 40, "at_mm": [30, 20, -10], "axis": "z"},
    {"id": "part", "op": "cut", "from": "body", "remove": ["bore"]},
]


def main():
    if not CAD.exists():
        print("cadlatticetest: build/openphysics-cad is not built (make build/openphysics-cad)")
        return 2
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="openphysics-cadlattice-"))
    results = []
    try:
        cube40 = part(tmp, "cube40", [{"id": "c", "op": "box", "size_mm": [40, 40, 40], "at_mm": [-20, -20, -20]}])
        cube30 = part(tmp, "cube30", [{"id": "c", "op": "box", "size_mm": [30, 30, 30], "at_mm": [-15, -15, -15]}])
        plate = part(tmp, "plate", PLATE)

        print("L1 the gyroid's network in a cube centred on the origin")
        r = lattice(tmp, "l1", {"part": str(cube40), "skin_mm": 0, "resolution_mm": 0.2,
                                "core": {"kind": "gyroid", "form": "network", "level": 0, "cell_mm": 10}})
        results.append(("L1", r))
        f = (r.get("core") or {}).get("solid_fraction", float("nan"))
        check(r.get("ok") and abs(f - 0.5) < 0.005, "solid fraction %.5f against 0.5 (%s)" % (f, r.get("error") or "%.0f s" % r["_seconds"]))

        print("L2 the gyroid's sheet: the mesh against a finer count and the thin-sheet value")
        h = 0.15
        r = lattice(tmp, "l2", {"part": str(cube30), "skin_mm": 0, "resolution_mm": h,
                                "core": {"kind": "gyroid", "form": "sheet", "cell_mm": 6, "wall_mm": 0.6}})
        results.append(("L2", r))
        f = (r.get("core") or {}).get("solid_fraction", float("nan"))
        count = gyroid_sheet_fraction(6, 0.6, h / 5)
        thin = 3.0919 * 0.6 / 6
        check(r.get("ok") and abs(f - count) / count < 0.01, "mesh %.5f against the count on a %.3f mm grid %.5f (%.2f %%)" % (f, h / 5, count, 100 * abs(f - count) / count))
        check(r.get("ok") and abs(f - thin) / thin < 0.03, "mesh %.5f against the thin-sheet value %.5f (%.2f %%)" % (f, thin, 100 * abs(f - thin) / thin))

        print("L3 a hollow cube: its 1 mm skin only")
        r = lattice(tmp, "l3", {"part": str(cube40), "skin_mm": 1, "resolution_mm": 0.2, "core": {"kind": "none"}})
        results.append(("L3", r))
        v = (r.get("result") or {}).get("volume_mm3", float("nan"))
        check(r.get("ok") and abs(v - 9128) / 9128 < 0.005, "%.2f mm^3 against 9128 (%.3f %%)" % (v, 100 * abs(v - 9128) / 9128))

        print("L5 the plate: a 1 mm skin and a gyroid sheet core at a solid fraction of 0.3")
        r = lattice(tmp, "l5", {"part": str(plate), "skin_mm": 1, "resolution_mm": 0.15,
                                "core": {"kind": "gyroid", "form": "sheet", "cell_mm": 6, "solid_fraction": 0.3}})
        results.append(("L5", r))
        core, res, skin = r.get("core") or {}, r.get("result") or {}, r.get("skin") or {}
        f = core.get("solid_fraction", float("nan"))
        check(r.get("ok") and abs(f - 0.3) <= 0.02, "core %.4f against 0.3 (wall %.3f mm, %s)" % (f, core.get("wall_mm", float("nan")), r.get("error") or ""))
        check(r.get("ok") and res.get("outside_part_mm", 1e9) <= 0.15, "the furthest vertex outside the part: %.4f mm (one grid step 0.15)" % res.get("outside_part_mm", float("nan")))
        check(r.get("ok") and skin.get("thinnest_sampled_mm", 0) >= 0.9, "the thinnest skin where sampled: %.3f mm" % skin.get("thinnest_sampled_mm", float("nan")))
        print("L7 its time: %.1f s (the record says %.1f s)" % (r["_seconds"], r.get("seconds", float("nan"))))
        check(r["_seconds"] < 600, "under 600 s")

        print("L4 every result closed and manifold, twice")
        for name, rr in results:
            stl = rr["_dir"] / "infilled.stl"
            own = stl_closed(stl) if stl.exists() else False
            check(rr.get("ok") and (rr.get("result") or {}).get("closed_manifold") and (rr.get("result") or {}).get("stl_closed_manifold") and own,
                  "%s: Manifold's status, the program's count and this test's count of the STL's edges agree (%s triangles)"
                  % (name, (rr.get("result") or {}).get("triangles")))

        print("L6 refusals")
        cases = [
            ("thin", {"part": str(cube30), "skin_mm": 1, "resolution_mm": 0.15, "core": {"kind": "gyroid", "cell_mm": 6, "wall_mm": 0.2}}, "two grid steps"),
            ("fraction", {"part": str(cube30), "core": {"kind": "gyroid", "cell_mm": 6, "solid_fraction": 0.99}}, "0.05 to 0.95"),
            ("kind", {"part": str(cube30), "core": {"kind": "honeycomb", "cell_mm": 6, "wall_mm": 1}}, "not known"),
            ("missing", {"part": str(tmp / "missing.step"), "core": {"kind": "gyroid", "cell_mm": 6, "wall_mm": 1}}, "cannot read"),
        ]
        for name, req, word in cases:
            rr = lattice(tmp, "refuse-" + name, req)
            msg = (rr.get("error") or {}).get("message", "")
            wrote = sorted(p.name for p in rr["_dir"].iterdir() if p.name != "request.json")
            check(rr.get("ok") is False and word in msg and not wrote, "%s refused: %s; files written %s" % (name, msg, wrote))
    finally:
        if not failures:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print("kept for a look: %s" % tmp)
    print("cadlatticetest: %s" % ("all checks passed" if not failures else "%d failed" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
