#!/usr/bin/env python3
"""tetcompare.py - the same part solved on voxels and on 10-node tetrahedra, side by side (task T17, step 5).

Two parts, each meshed twice at comparable element counts and solved with the same material and conditions through
the operation layer (the MCP server in embedded mode):

  bracket   the L-bracket of tools/amflow.py (base 60 x 6 mm, upright 6 x 40 mm, 30 mm deep) with a 3 mm fillet in the
            inside corner, base fixed, 500 N sideways at the top of the upright
  assembly  the owner's Zero_Final_Assembly_Complete.stl (not in the repository; give its path in NAVIER_ASSEMBLY_STL),
            one end face fixed (patch 0), 500 N down on the other (patch 2)

Reported per mesh: elements, nodes, largest displacement, 99th-percentile nodal von Mises stress, the peak nodal von
Mises stress in the fillet corner (bracket only; the box around the fillet, away from the supports and the load) and the
wall time of meshing and solving. One PNG per part with the two results side by side goes to docs/app/.

    python3 tools/tetcompare.py [bracket] [assembly]

No dependencies beyond the standard library: the two rendered PNGs are decoded and joined here.
"""
import base64
import csv
import json
import math
import os
import struct
import sys
import tempfile
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from mcptest import Client  # noqa: E402

ASSEMBLY = Path(os.environ.get("NAVIER_ASSEMBLY_STL", "owner-assembly-not-set.stl"))  # a private part, not in the repository


# ---- geometry ----------------------------------------------------------------------------------------------------

def ear_clip(poly):
    """triangles of a simple counter-clockwise polygon"""
    idx = list(range(len(poly)))
    tris = []

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

    def inside(p, a, b, c):
        return cross(a, b, p) >= 0 and cross(b, c, p) >= 0 and cross(c, a, p) >= 0

    guard = 0
    while len(idx) > 3 and guard < 100000:
        guard += 1
        for i in range(len(idx)):
            a, b, c = idx[i - 1], idx[i], idx[(i + 1) % len(idx)]
            if cross(poly[a], poly[b], poly[c]) <= 1e-12:
                continue
            if any(inside(poly[j], poly[a], poly[b], poly[c]) for j in idx if j not in (a, b, c)):
                continue
            tris.append((a, b, c))
            idx.pop(i)
            break
    tris.append(tuple(idx))
    return tris


def filleted_bracket(path, r=3.0, n_arc=24):
    """the amflow L-bracket with a fillet of radius r in the inside corner (6, 6) of its XZ profile, 30 mm deep"""
    cx, cz = 6 + r, 6 + r
    prof = [(0, 0), (60, 0), (60, 6), (6 + r, 6)]
    for k in range(1, n_arc):
        th = math.radians(270 - 90 * k / n_arc)
        prof.append((cx + r * math.cos(th), cz + r * math.sin(th)))
    prof += [(6, 6 + r), (6, 40), (0, 40)]
    width = 30.0
    tris = []
    for a, b, c in ear_clip(prof):
        pa, pb, pc = prof[a], prof[b], prof[c]
        tris.append(((pa[0], 0, pa[1]), (pb[0], 0, pb[1]), (pc[0], 0, pc[1])))
        tris.append(((pa[0], width, pa[1]), (pc[0], width, pc[1]), (pb[0], width, pb[1])))
    for i in range(len(prof)):
        p, q = prof[i], prof[(i + 1) % len(prof)]
        a, b, c, d = (p[0], 0, p[1]), (p[0], width, p[1]), (q[0], width, q[1]), (q[0], 0, q[1])
        tris.append((a, b, c))
        tris.append((a, c, d))
    with open(path, "wb") as f:
        f.write(b"L bracket with a 3 mm inside fillet (tetcompare)".ljust(80, b" "))
        f.write(struct.pack("<I", len(tris)))
        for t in tris:
            f.write(struct.pack("<3f", 0, 0, 0))
            for v in t:
                f.write(struct.pack("<3f", *v))
            f.write(b"\0\0")


# ---- PNG decode and join (our renderer writes 8-bit RGB or RGBA, not interlaced) -----------------------------------

def png_read(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, w = 8, b"", 0
    while pos < len(data):
        ln = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        if kind == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            assert depth == 8 and ctype in (2, 6)
            bpp = 3 if ctype == 2 else 4
        elif kind == b"IDAT":
            idat += body
        pos += 12 + ln
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev, off = [], bytearray(stride), 0
    for _ in range(h):
        ft = raw[off]
        line = bytearray(raw[off + 1:off + 1 + stride])
        off += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ft == 1:
                line[i] = (line[i] + a) & 255
            elif ft == 2:
                line[i] = (line[i] + b) & 255
            elif ft == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif ft == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        rows.append(bytes(line[j:j + 3]) for j in range(0, stride, bpp))
        rows[-1] = b"".join(rows[-1])
        prev = line
    return w, h, rows


def png_join(images, gap=8):
    ims = [png_read(d) for d in images]
    H = max(h for _, h, _ in ims)
    W = sum(w for w, _, _ in ims) + gap * (len(ims) - 1)
    out = bytearray()
    for y in range(H):
        out.append(0)
        for i, (w, h, rows) in enumerate(ims):
            out += rows[y] if y < h else b"\x28\x2c\x36" * w
            if i < len(ims) - 1:
                out += b"\xff\xff\xff" * gap
    def chunk(k, b):
        return struct.pack(">I", len(b)) + k + b + struct.pack(">I", zlib.crc32(k + b) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(out), 9)) + chunk(b"IEND", b""))


# ---- the runs ------------------------------------------------------------------------------------------------------

def call(c, name, args):
    r = c.call(name, args)
    sc = r.get("result", {}).get("structuredContent") or {}
    if sc.get("ok") is not True:
        raise SystemExit(f"{name}: {json.dumps(sc.get('error'))[:900]}")
    return sc.get("value", {}), r


def solve(c, tmp, label, mesh_args, fillet_box=None):
    t0 = time.time()
    v, _ = call(c, "mesh_generate", mesh_args)
    tm = time.time() - t0
    m = v["mesh"]
    t1 = time.time()
    v, _ = call(c, "analysis_run", {"label": label, "allow_duplicate": True})
    job = v["job_id"]
    while True:
        st, _ = call(c, "job_status", {"job_id": job, "wait_seconds": 10})
        if st.get("state") not in ("queued", "running"):
            break
    ts = time.time() - t1
    if st.get("state") != "succeeded":
        raise SystemExit(f"{label}: {st.get('error')}")
    s = st["summary"]
    out = tmp / label
    call(c, "results_export", {"job_id": job, "formats": ["csv"], "directory": str(out)})
    peak = None
    with open(out / "nodes.csv") as f:
        rows = list(csv.DictReader(l for l in f if not l.startswith("#")))
    keys = rows[0].keys()
    kx = next(k for k in keys if k.startswith("x")), next(k for k in keys if k.startswith("y")), next(k for k in keys if k.startswith("z"))
    kv = next(k for k in keys if "von_mises" in k)
    if fillet_box:
        (x0, x1), (z0, z1) = fillet_box
        vals = [float(r[kv]) for r in rows if x0 <= float(r[kx[0]]) <= x1 and z0 <= float(r[kx[2]]) <= z1]
        peak = max(vals) if vals else float("nan")
    v, r = call(c, "results_render", {"job_id": job, "quantity": "von_mises", "width": 720, "height": 520, "show_undeformed": False,
                                      "title": f"{label}: {m.get('elements')} elements, von Mises (MPa)"})
    png = base64.b64decode(next(i for i in r["result"]["content"] if i.get("type") == "image")["data"])
    vm = s.get("von_mises", {})
    return {"label": label, "elements": m.get("elements"), "nodes": m.get("nodes"), "method": m.get("method"),
            "min_dihedral": (m.get("quality") or {}).get("min_dihedral_deg"),
            "umax_mm": s.get("displacement", {}).get("max_magnitude_mm"), "p99_mpa": vm.get("nodal_p99_mpa"),
            "peak_nodal_mpa": vm.get("nodal_average_max_mpa"), "fillet_peak_mpa": peak, "mesh_s": tm, "solve_s": ts,
            "solver": (s.get("solver") or {}).get("method"), "png": png}


def report(part, runs, path):
    print(f"\n{part}")
    print(f"  {'mesh':28} {'elements':>9} {'nodes':>8} {'max disp mm':>12} {'p99 MPa':>9} {'fillet peak':>12} {'mesh s':>7} {'solve s':>8}")
    for r in runs:
        fp = f"{r['fillet_peak_mpa']:.1f}" if r["fillet_peak_mpa"] is not None else "-"
        print(f"  {r['label']:28} {r['elements']:>9} {r['nodes']:>8} {r['umax_mm']:>12.5f} {r['p99_mpa']:>9.2f} {fp:>12} {r['mesh_s']:>7.1f} {r['solve_s']:>8.1f}")
    path.write_bytes(png_join([r["png"] for r in runs[:2]]))
    print(f"  figure: {path.relative_to(ROOT)}")


def main():
    which = [a for a in sys.argv[1:] if not a.startswith("-")] or ["bracket", "assembly"]
    tmp = Path(tempfile.mkdtemp(prefix="tetcompare"))
    results = {}
    if "bracket" in which:
        stl = tmp / "bracket_fillet.stl"
        filleted_bracket(stl)
        c = Client(["--embedded", "--workspace", str(tmp / "wsb"), "--allow-read", str(tmp), "--allow-write", str(tmp)])
        c.initialize()
        call(c, "project_create", {"name": "bracket_fillet"})
        call(c, "geometry_import", {"path": str(stl), "units": "mm", "name": "bracket"})
        call(c, "selection_create", {"name": "base_bottom", "query": {"all": [{"facing": {"direction": "down"}}, {"plane": {"axis": "z", "at": "min"}}]},
                                    "source": "user"})
        call(c, "selection_create", {"name": "upright_top", "query": {"extreme": {"direction": "+z"}}, "source": "user"})
        call(c, "material_assign", {"body": "bracket", "material": "ss316l_lpbf_demo", "source": "user"})
        call(c, "boundary_apply", {"name": "bolted", "kind": "fixed", "selection": "base_bottom", "source": "user"})
        call(c, "boundary_apply", {"name": "push", "kind": "force", "selection": "upright_top", "force": ["0.5 kN", 0, 0], "source": "user"})
        # the fillet box in the build frame: the part is placed with its footprint centred, so read the offset from the mesh
        v, _ = call(c, "geometry_place", {"body": "bracket"})
        lo = v["body"]["bounds_mm"]["min"]
        box = ((lo[0] + 5.0, lo[0] + 10.0), (lo[2] + 5.0, lo[2] + 10.0))
        runs = [solve(c, tmp, "voxels 0.65 mm", {"method": "voxel", "element_size": "0.65 mm"}, box),
                solve(c, tmp, "TET10 surface 1.5 mm", {"method": "tet", "surface_size": "1.5 mm", "order": 2}, box)]
        # a finer TET10 mesh as the reference the two are read against (not in the figure)
        runs.append(solve(c, tmp, "TET10 surface 1 mm (reference)", {"method": "tet", "surface_size": "1 mm", "order": 2}, box))
        c.close()
        report("bracket with a 3 mm fillet, 500 N sideways at the top", runs, ROOT / "docs/app/compare-bracket.png")
        results["bracket"] = runs
    if "assembly" in which and ASSEMBLY.exists():
        c = Client(["--embedded", "--workspace", str(tmp / "wsa"), "--allow-read", str(ASSEMBLY.parent), "--allow-write", str(tmp)])
        c.initialize()
        call(c, "project_create", {"name": "assembly"})
        call(c, "geometry_import", {"path": str(ASSEMBLY), "units": "mm", "name": "assembly"})
        call(c, "selection_create", {"name": "end_fixed", "query": {"patches": [0]}, "source": "user"})
        call(c, "selection_create", {"name": "end_loaded", "query": {"patches": [2]}, "source": "user"})
        call(c, "material_assign", {"body": "assembly", "material": "ss316l_lpbf_demo", "source": "user"})
        call(c, "boundary_apply", {"name": "fixed_end", "kind": "fixed", "selection": "end_fixed", "source": "user"})
        call(c, "boundary_apply", {"name": "load", "kind": "force", "selection": "end_loaded", "force": [0, 0, "-500 N"], "source": "user"})
        runs = [solve(c, tmp, "voxels 0.8 mm", {"method": "voxel", "element_size": "0.8 mm", "max_elements": 400000}),
                solve(c, tmp, "TET10 surface 4 mm", {"method": "tet", "surface_size": "4 mm", "order": 2, "thin_wall_levels": 1})]
        c.close()
        report("owner's assembly, one end fixed, 500 N down on the other", runs, ROOT / "docs/app/compare-assembly.png")
        results["assembly"] = runs
    for part in results.values():
        for r in part:
            r.pop("png", None)
    (tmp / "compare.json").write_text(json.dumps(results, indent=1))
    print(f"\nnumbers: {tmp / 'compare.json'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
