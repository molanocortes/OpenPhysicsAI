#!/usr/bin/env python3
"""breaks.py - real breaks of printed parts (GOALS.md G17 step 5): the parts, the model's prediction, the comparison.

The owner prints the parts of validation/breaks/parts/, breaks them with a bucket of water (validation/breaks/README.md)
and writes one record per break (validation/breaks/record_template.json). The records stay private, by default in
~/.openphysicsai/sealed/breaks/ (GOALS.md G17: measured breaks are a held-out set). This tool solves each record's part
as it was printed (the record's material, orientation, walls and infill) and compares the model's first-failure load and
place with the measured ones.

    python3 tools/breaks.py parts                 # write the STLs and scenarios of validation/breaks/parts/
    python3 tools/breaks.py predict [PART ...]    # the model's failure load of each part and print variant
    python3 tools/breaks.py compare [DIR]         # every record in DIR against the model (median error, places)
    python3 tools/breaks.py selftest              # the comparison's own check on synthetic records

Every part is an extruded profile without holes (no drilling, no inserts): heads that rest in a slotted plate, a hook
that takes a bucket handle, faces that a clamp holds. Lengths in mm, loads in N; the scenarios name each support and
load as the rig of README.md applies it.
"""
import json
import math
import os
import statistics
import struct
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PARTS = ROOT / "validation" / "breaks" / "parts"
SEALED = Path(os.environ.get("OPENPHYSICS_BREAKS") or os.environ.get("NAVIER_BREAKS")  # NAVIER_: the name before 2026-10-21
              or str(Path.home() / ".openphysicsai" / "sealed" / "breaks"))
G0 = 9.80665

# ------------------------------------------------------------------------------------------------ geometry


def arc(cx, cy, r, a0, a1, n):
    """n + 1 points from angle a0 to a1 (degrees) on a circle"""
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * k / n)), cy + r * math.sin(math.radians(a0 + (a1 - a0) * k / n))) for k in range(n + 1)]


def area2(poly):
    return sum(poly[i][0] * poly[(i + 1) % len(poly)][1] - poly[(i + 1) % len(poly)][0] * poly[i][1] for i in range(len(poly)))


def clean(poly, eps=1e-9):
    """drop repeated vertices; collinear ones stay, because a face split there can be selected on its own"""
    out = []
    for p in poly:
        if not out or abs(p[0] - out[-1][0]) > eps or abs(p[1] - out[-1][1]) > eps:
            out.append(p)
    if len(out) > 1 and abs(out[0][0] - out[-1][0]) <= eps and abs(out[0][1] - out[-1][1]) <= eps:
        out.pop()
    return out


def triangulate(poly):
    """ear clipping of a simple counter-clockwise polygon: index triples"""
    idx = list(range(len(poly)))

    def inside(p, a, b, c):
        def s(p1, p2, p3):
            return (p1[0] - p3[0]) * (p2[1] - p3[1]) - (p2[0] - p3[0]) * (p1[1] - p3[1])
        d1, d2, d3 = s(p, a, b), s(p, b, c), s(p, c, a)
        return d1 >= -1e-12 and d2 >= -1e-12 and d3 >= -1e-12

    tris = []
    guard = 0
    while len(idx) > 3 and guard < 100000:
        guard += 1
        for k in range(len(idx)):
            i0, i1, i2 = idx[k - 1], idx[k], idx[(k + 1) % len(idx)]
            a, b, c = poly[i0], poly[i1], poly[i2]
            if (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]) <= 1e-12:
                continue  # a reflex or flat corner is no ear
            if any(inside(poly[j], a, b, c) for j in idx if j not in (i0, i1, i2)):
                continue
            tris.append((i0, i1, i2))
            idx.pop(k)
            break
        else:
            raise ValueError("the profile is not a simple polygon")
    tris.append(tuple(idx))
    return tris


def extrude_stl(path, poly, depth, name):
    """binary STL of a counter-clockwise profile (x, y) extruded along z from 0 to depth, outward facets"""
    poly = clean(poly)
    if area2(poly) < 0:
        poly = poly[::-1]
    tris = []
    for i in range(len(poly)):
        (x0, y0), (x1, y1) = poly[i], poly[(i + 1) % len(poly)]
        a, b, c, d = (x0, y0, 0), (x1, y1, 0), (x1, y1, depth), (x0, y0, depth)
        tris += [(a, b, c), (a, c, d)]
    for i0, i1, i2 in triangulate(poly):
        p0, p1, p2 = poly[i0], poly[i1], poly[i2]
        tris.append(((p0[0], p0[1], depth), (p1[0], p1[1], depth), (p2[0], p2[1], depth)))
        tris.append(((p0[0], p0[1], 0), (p2[0], p2[1], 0), (p1[0], p1[1], 0)))
    with open(path, "wb") as f:
        f.write(name.encode()[:80].ljust(80, b" "))
        f.write(struct.pack("<I", len(tris)))
        for t in tris:
            u = [t[1][k] - t[0][k] for k in range(3)]
            v = [t[2][k] - t[0][k] for k in range(3)]
            n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
            ln = math.sqrt(sum(c * c for c in n)) or 1
            f.write(struct.pack("<3f", *[c / ln for c in n]))
            for p in t:
                f.write(struct.pack("<3f", *p))
            f.write(b"\0\0")
    return len(tris)


# ------------------------------------------------------------------------------------------------ the parts


def bar_profile(wg=1.6, lg=20.0, rs=8.0, wn=6.4, ln=10.0, wh=14.4, lh=8.0, rf=1.0):
    """a tensile bar along x: a gauge wg x lg between shoulder arcs of radius rs up to a neck wn x ln, and a head wh x lh at
    each end; the step from neck to head is the face a slotted plate bears on (fillet rf). Symmetric about y = 0, its left
    end at x = 0. The widths are whole numbers of 0.4 mm voxels from the edge, so that a voxel mesh of 0.4 or 0.2 mm
    has the gauge's section exactly."""
    hg, hn, hh = wg / 2, wn / 2, wh / 2
    dy = hn - hg
    dx = math.sqrt(max(rs * rs - (rs - dy) ** 2, 0))  # the shoulder: an arc tangent to the gauge, meeting the neck
    a0 = math.degrees(math.asin(dx / rs))
    x_head, x_neck_end = lh, lh + ln
    x_gauge0 = x_neck_end + dx
    x_gauge1 = x_gauge0 + lg
    x_neck2 = x_gauge1 + dx
    x_head2 = x_neck2 + ln
    total = x_head2 + lh
    up = [(0, hh), (x_head, hh)]
    up += arc(x_head + rf, hn + rf, rf, 180, 270, 4)
    up += [(x_neck_end, hn)]
    up += arc(x_gauge0, hg + rs, rs, 270 - a0, 270, 12)
    up += arc(x_gauge1, hg + rs, rs, 270, 270 + a0, 12)
    up += [(x_head2 - rf, hn)]
    up += arc(x_head2 - rf, hn + rf, rf, 270, 360, 4)
    up += [(x_head2, hh), (total, hh)]
    low = [(x, -y) for (x, y) in up]
    poly = low + up[::-1]  # the lower edge left to right, the upper one back: counter-clockwise
    dims = {"x_head": x_head, "x_gauge": (x_gauge0, x_gauge1), "x_head2": x_head2, "total": total, "hh": hh, "hn": hn}
    return clean(poly), dims


def hook_profile(t=5.0, ri=12.0, shank=20.0, wh=24.0, hh=6.0, tip=45.0, rf=3.0, taper=1.5):
    """a J-hook in the xy plane: a shank that widens from t at the ring by `taper` on each side up to a head wh x hh whose
    underside rests on a slotted plate (fillets rf), and a ring of inner radius ri that carries a bucket handle at its
    lowest point; the ring runs on past its right end by `tip` degrees so that the handle stays in. The shank carries the
    same moment as the ring's end, so it widens and the bend is where a hook breaks."""
    rc, ro = ri + t / 2, ri + t
    xl, xr = -t / 2 - taper, t / 2 + taper  # the shank's edges where the fillets begin
    poly = [(xl, shank - rf), (-t / 2, 0)]
    poly += arc(rc, 0, ro, 180, 360 + tip, 48)
    poly += arc(rc, 0, ri, 360 + tip, 180, 48)
    poly += [(xr, shank - rf)]
    poly += arc(xr + rf, shank - rf, rf, 180, 90, 8)  # into the head's underside
    poly += [(wh / 2, shank), (wh / 2, shank + hh), (-wh / 2, shank + hh), (-wh / 2, shank), (xl - rf, shank)]
    poly += arc(xl - rf, shank - rf, rf, 90, 0, 8)
    dims = {"rc": rc, "ri": ri, "ro": ro, "t": t, "shank": shank, "wh": wh, "hh": hh, "bottom": (rc, -ri)}
    return clean(poly), dims


def bracket_profile(tb=6.0, ta=4.5, hb=70.0, la=45.0, rf=3.0, lip=3.0, liph=4.0, clamp=25.0, cord=6.0):
    """an L bracket in the xy plane: a back plate tb thick from y = 0 to hb against a wall (x = 0), an arm ta thick along
    x to la at the bottom, a fillet rf in the inner corner, and a lip lip x liph at the arm's end that keeps a cord on.
    The back face is split where the clamp's reach ends (clamp mm from the top) and the arm's top where the cord lies."""
    poly = [(0, 0), (la, 0), (la, ta + liph), (la - lip, ta + liph), (la - lip, ta), (la - lip - cord, ta), (tb + rf, ta)]
    poly += arc(tb + rf, ta + rf, rf, 270, 180, 8)
    poly += [(tb, hb), (0, hb), (0, hb - clamp)]
    dims = {"tb": tb, "ta": ta, "hb": hb, "la": la, "rf": rf, "lip": lip, "clamp": clamp, "cord": cord}
    return clean(poly), dims


def beam_profile(lb=30.0, hb=24.0, h=8.0, length=60.0, rf=3.0, lip=3.0, liph=4.0, clamp=20.0, cord=6.0):
    """a cantilever in the xy plane: a block lb x hb held by a clamp over its first `clamp` mm, a beam h deep and `length`
    long from the middle of its right face (fillets rf at the junction), and a lip at the beam's end for the cord"""
    y0, y1 = (hb - h) / 2, (hb + h) / 2
    xe = lb + length
    poly = [(0, 0), (clamp, 0), (lb, 0), (lb, y0 - rf)]
    poly += arc(lb + rf, y0 - rf, rf, 180, 90, 8)
    poly += [(xe, y0), (xe, y1 + liph), (xe - lip, y1 + liph), (xe - lip, y1), (xe - lip - cord, y1), (lb + rf, y1)]
    poly += arc(lb + rf, y1 + rf, rf, 270, 180, 8)
    poly += [(lb, hb), (clamp, hb), (0, hb)]
    dims = {"lb": lb, "hb": hb, "h": h, "y0": y0, "y1": y1, "xe": xe, "rf": rf, "lip": lip, "clamp": clamp, "cord": cord}
    return clean(poly), dims


def scenarios():
    """every part: its profile, depth, the supports and load of the rig (boxes in the part's own mm), the places a break
    is reported by, and the ways it is printed"""
    out = {}
    poly, d = bar_profile()
    depth = 1.6
    out["bar"] = {
        "title": "tensile bar, 1.6 x 1.6 mm gauge, heads resting in slotted plates",
        "profile": poly, "depth_mm": depth,
        "supports": [{"name": "top_head", "kind": "fixed", "box": [d["x_head"] - 0.01, -d["hh"] - 1, -1, d["x_head"] + 0.01, d["hh"] + 1, depth + 1],
                      "facing": "+x", "rig": "the head's step rests on a slotted plate hung from a beam"}],
        "load": {"name": "bucket", "box": [d["x_head2"] - 0.01, -d["hh"] - 1, -1, d["x_head2"] + 0.01, d["hh"] + 1, depth + 1], "facing": "-x",
                 "direction": [1, 0, 0], "rig": "a second slotted plate on the lower head carries the bucket"},
        "regions": {"gauge": [d["x_gauge"][0] - 2, -10, -1, d["x_gauge"][1] + 2, 10, depth + 1],  # with the first 2 mm of each shoulder
                    "shoulder": [d["x_head"] + 1, -10, -1, d["x_head2"] - 1, 10, depth + 1],
                    "head": [-1, -10, -1, d["total"] + 1, 10, depth + 1]},
        "prints": {"flat": {"build_direction": "z", "raster_reference": "x"},
                   "upright": {"build_direction": "x", "raster_reference": "y"}},
        "measure": "the gauge's width and thickness with calipers, to 0.01 mm",
    }
    poly, d = hook_profile()
    depth = 8.0
    bx, by = d["bottom"]
    out["hook"] = {
        "title": "J-hook, 5 mm thick and 8 mm deep, head resting in a slotted plate, bucket handle at the bottom of the ring",
        "profile": poly, "depth_mm": depth,
        "supports": [{"name": "head", "kind": "fixed", "box": [-d["wh"] / 2 - 1, d["shank"] - 0.01, -1, d["wh"] / 2 + 1, d["shank"] + 0.01, depth + 1],
                      "facing": "-y", "rig": "the head's underside rests on a slotted plate hung from a beam"}],
        "load": {"name": "bucket", "box": [bx - 2.5, by - 0.6, -1, bx + 2.5, by + 0.6, depth + 1], "facing": "+y", "max_angle_deg": 30, "direction": [0, -1, 0],
                 "rig": "the bucket's handle sits at the lowest point of the ring"},
        "regions": {"bend": [-d["t"] - 1, -d["ro"] - 1, -1, 2 * d["rc"] + d["ro"], 6, depth + 1],  # the ring and where it meets the shank
                    "shank": [-d["t"], 6, -1, d["t"], d["shank"] - 0.5, depth + 1],
                    "head": [-20, d["shank"] - 0.5, -1, 20, 60, depth + 1]},
        "prints": {"flat": {"build_direction": "z", "raster_reference": "x"}},
        "measure": "the ring's thickness at its lowest point, with calipers",
    }
    poly, d = bracket_profile()
    depth = 15.0
    out["bracket"] = {
        "title": "L bracket, clamped at the top of its back plate, a cord at the end of its arm",
        "profile": poly, "depth_mm": depth,
        "supports": [{"name": "clamp", "kind": "fixed", "box": [-0.01, d["hb"] - d["clamp"] - 0.01, -1, 0.01, d["hb"] + 1, depth + 1], "facing": "-x",
                      "rig": "the top 25 mm of the back plate clamped flat to the wall block"},
                     {"name": "wall", "kind": "frictionless_support", "box": [-0.01, -1, -1, 0.01, d["hb"] - d["clamp"] + 0.01, depth + 1], "facing": "-x",
                      "rig": "the rest of the back plate pressed against the wall block by the load"}],
        "load": {"name": "cord", "box": [d["la"] - d["lip"] - d["cord"] - 0.01, d["ta"] - 0.01, -1, d["la"] - d["lip"] + 0.01, d["ta"] + 0.01, depth + 1], "facing": "+y",
                 "direction": [0, -1, 0], "rig": "a cord over the arm against the lip, the bucket below"},
        "regions": {"corner": [-1, -1, -1, d["tb"] + d["rf"] + 6, d["ta"] + d["rf"] + 6, depth + 1],
                    "arm": [d["tb"], -1, -1, d["la"] + 1, d["ta"] + 5, depth + 1],
                    "back plate": [-1, -1, -1, d["tb"] + 1, d["hb"] + 1, depth + 1]},
        "prints": {"side": {"build_direction": "z", "raster_reference": "x"},
                   "standing": {"build_direction": "x", "raster_reference": "y"}},
        "measure": "the arm's thickness at the corner, with calipers",
    }
    poly, d = beam_profile()
    depth = 10.0
    out["beam"] = {
        "title": "cantilever 8 mm deep and 10 mm wide on a clamped block, for the three infill levels",
        "profile": poly, "depth_mm": depth,
        "supports": [{"name": "clamp", "kind": "fixed", "box": [-1, d["hb"] - 0.01, -1, d["clamp"] + 0.01, d["hb"] + 0.01, depth + 1], "facing": "+y",
                      "rig": "the block's top clamped to the table over its first 20 mm"},
                     {"name": "table", "kind": "fixed", "box": [-1, -0.01, -1, d["clamp"] + 0.01, 0.01, depth + 1], "facing": "-y",
                      "rig": "the block's bottom on the table under the clamp"}],
        "load": {"name": "cord", "box": [d["xe"] - d["lip"] - d["cord"] - 0.01, d["y1"] - 0.01, -1, d["xe"] - d["lip"] + 0.01, d["y1"] + 0.01, depth + 1], "facing": "+y",
                 "direction": [0, -1, 0], "rig": "a cord over the beam against the lip, the bucket below"},
        "regions": {"root": [d["lb"] - 1, -1, -1, d["lb"] + d["rf"] + 8, d["hb"] + 1, depth + 1],
                    "span": [d["lb"], -1, -1, d["xe"] + 1, d["hb"] + 1, depth + 1],
                    "block": [-1, -1, -1, d["lb"], d["hb"] + 1, depth + 1]},
        "prints": {"flat": {"build_direction": "z", "raster_reference": "x"}},
        "layouts": {"20 %": {"density": "20 %"}, "50 %": {"density": "50 %"}, "100 %": {"density": "100 %"}},
        "measure": "the beam's depth and width at the root, with calipers",
    }
    return out


def slot_plate_profile(length=80.0, width=30.0, slots=((25.0, 10.0), (55.0, 15.0)), depth_in=18.0, notch=6.0, notch_in=8.0):
    """the rig's slotted plate (not a specimen): slots open at the front edge, 10 mm for the bar's neck and 15 mm for the
    hook's, and a notch at each end of the back edge for the cords it hangs by; print it solid, 10 mm thick"""
    poly = [(0, 0)]
    for cx, w in slots:
        poly += [(cx - w / 2, 0), (cx - w / 2, depth_in), (cx + w / 2, depth_in), (cx + w / 2, 0)]
    poly += [(length, 0), (length, width), (length - 4, width), (length - 4, width - notch_in), (length - 4 - notch, width - notch_in),
             (length - 4 - notch, width), (4 + notch, width), (4 + notch, width - notch_in), (4, width - notch_in), (4, width), (0, width)]
    return clean(poly)


def write_parts():
    PARTS.mkdir(parents=True, exist_ok=True)
    out = {}
    n = extrude_stl(PARTS / "slot_plate.stl", slot_plate_profile(), 10.0, "OpenPhysicsAI breaks: the rig's slotted plate, print solid")
    print("  %-8s %4d facets, 80.0 x 30.0 x 10.0 mm: the rig's slotted plate (10 and 15 mm slots), not a specimen" % ("plate", n))
    for name, sc in scenarios().items():
        stl = PARTS / (name + ".stl")
        n = extrude_stl(stl, sc["profile"], sc["depth_mm"], "OpenPhysicsAI breaks: " + sc["title"])
        spec = {k: v for k, v in sc.items() if k != "profile"}
        spec["stl"] = stl.name
        xs, ys = [p[0] for p in sc["profile"]], [p[1] for p in sc["profile"]]
        spec["file_min_mm"] = [min(xs), min(ys), 0.0]
        spec["size_mm"] = [max(xs) - min(xs), max(ys) - min(ys), sc["depth_mm"]]
        (PARTS / (name + ".json")).write_text(json.dumps(spec, indent=1) + "\n")
        out[name] = (n, spec["size_mm"])
        print("  %-8s %4d facets, %.1f x %.1f x %.1f mm: %s" % (name, n, *spec["size_mm"], sc["title"]))
    return out


# ------------------------------------------------------------------------------------------------ the model's prediction

# the mesh of each part: solid parts on conforming quadratic tetrahedra (no voxel steps on their curves); the infill beam
# on voxels, because walls and infill are laid out on the voxel grid
MESH = {"bar": {"method": "tet", "surface_size": "0.4 mm", "order": 2},
        "hook": {"method": "tet", "surface_size": "0.75 mm", "order": 2},
        "bracket": {"method": "tet", "surface_size": "1 mm", "order": 2},
        "beam": {"method": "voxel", "element_size": "1 mm"}}


class Engine:
    """openphysics-mcp over stdio in its own workspace, as an AI host drives it"""

    def __init__(self, tmp):
        sys.path.insert(0, str(ROOT / "tools"))
        from mcptest import Client  # noqa: E402
        self.c = Client(["--embedded", "--workspace", str(Path(tmp) / "ws"), "--allow-read", str(PARTS)])
        self.c.initialize()

    def call(self, name, args):
        r = self.c.call(name, args)
        sc = r.get("result", {}).get("structuredContent") or {}
        if not sc.get("ok"):
            raise RuntimeError("%s: %s" % (name, json.dumps(sc.get("error"))[:600]))
        return sc.get("value", {})

    def wait(self, job, tries=200):
        for _ in range(tries):
            v = self.call("job_status", {"job_id": job, "wait_seconds": 10})  # under the client's 20 s read timeout
            if v.get("state") not in ("queued", "running"):
                return v
        raise RuntimeError("job %s did not finish" % job)

    def close(self):
        self.c.close()


def part_spec(name):
    return json.loads((PARTS / (name + ".json")).read_text())


def setup_part(eng, name, spec, material, project=None):
    """one project per part: geometry, the isotropic project material the static model needs, mesh, supports, load"""
    eng.call("project_create", {"name": project or "breaks_" + name})
    body = eng.call("geometry_import", {"path": str(PARTS / spec["stl"]), "units": "mm", "name": name})["body"]
    t = body["placement"]["translation_mm"]
    eng.call("material_assign", {"body": name, "material": material, "source": "user"})
    eng.call("mesh_generate", dict(MESH[name]))

    def sel(sname, item):
        b = item["box"]
        q = {"all": [{"box": {"min": [b[0] + t[0], b[1] + t[1], b[2] + t[2]], "max": [b[3] + t[0], b[4] + t[1], b[5] + t[2]]}},
                     {"facing": {"direction": item["facing"], "max_angle_deg": item.get("max_angle_deg", 10)}}]}
        eng.call("selection_create", {"name": sname, "query": q, "mode": "triangle", "source": "user", "description": item["rig"]})

    for i, sp in enumerate(spec["supports"]):
        sel("support_%d" % i, sp)
        eng.call("boundary_apply", {"name": sp["name"], "kind": sp["kind"], "selection": "support_%d" % i, "source": "user", "description": sp["rig"]})
    ld = spec["load"]
    sel("load", ld)
    F = 100.0  # the reference load: the answer scales it
    eng.call("boundary_apply", {"name": ld["name"], "kind": "force", "selection": "load", "force": ["%.17g N" % (F * c) for c in ld["direction"]],
                                "source": "user", "description": ld["rig"]})
    return t, F


def region_of(spec, loc_part):
    for rname, b in spec["regions"].items():
        if b[0] <= loc_part[0] <= b[3] and b[1] <= loc_part[1] <= b[4] and b[2] <= loc_part[2] <= b[5]:
            return rname
    return "elsewhere"


def predict_one(eng, name, spec, t, F, material, prt, layout=None):
    """the model's failure load (N, kg) and place for one way of printing the part"""
    mm = {"type": "orthotropic", "library": material, "build_direction": spec["prints"][prt]["build_direction"],
          "raster_reference": spec["prints"][prt]["raster_reference"]}
    if layout:
        mm["walls"] = layout["walls"]
        mm["infill"] = layout["infill"]
    job = eng.call("analysis_run", {"label": "%s %s" % (name, prt), "material_model": mm})["job_id"]
    st = eng.wait(job)
    if st.get("state") != "succeeded":
        raise RuntimeError("%s %s: job %s %s" % (name, prt, st.get("state"), json.dumps(st.get("error"))[:400]))
    summ = st.get("summary") or st.get("result") or {}
    S = summ.get("strength") or {}
    A = S.get("answer") or {}
    # the answer's stated strength ratio and place (GOALS.md G17 step 4); when every loaded point lies near a held face or
    # a sharp corner the answer states none, and the prediction says so instead of falling back on the peak
    if A and "strength_ratio" not in A:
        return {"part": name, "print": prt, "material": material, "not_stated": S.get("stated_strength_ratio_note", "no stated strength ratio"),
                "flags": [A["flag"]] if A.get("flag") else [], "job": job}
    R = A.get("strength_ratio", S.get("stated_strength_ratio", S.get("smallest_strength_ratio")))
    loc = A.get("where_mm", S.get("location_mm"))
    if R is None or loc is None:
        raise RuntimeError("%s %s: no strength in the summary (%s)" % (name, prt, ", ".join(sorted(summ))[:300]))
    part_loc = [loc[k] - t[k] for k in range(3)]
    flags = [A["flag"]] if A.get("flag") else []
    where = None if "between_layers" not in A else ("between the layers" if A["between_layers"] else "along the layers")
    return {"part": name, "print": prt, "material": material, "strength_ratio": R, "fails_at_n": R * F, "fails_at_kg": R * F / G0,
            "location_part_mm": part_loc, "region": region_of(spec, part_loc), "mode": A.get("governing_mode", S.get("governing_mode")),
            "where": where, "flags": flags, "answer": A.get("text"), "job": job}


def cmd_predict(names, material="pla_fff"):
    tmp = tempfile.mkdtemp(prefix="openphysics-breaks-", dir="/tmp")
    eng = Engine(tmp)
    rows = []
    try:
        for name in names or [p.stem for p in sorted(PARTS.glob("*.json"))]:
            spec = part_spec(name)
            t, F = setup_part(eng, name, spec, material)
            for prt in spec["prints"]:
                layouts = spec.get("layouts") or {None: None}
                for lname, lay in layouts.items():
                    layout = None
                    if lay:
                        layout = {"walls": {"perimeters": 2, "line_width": "0.45 mm", "top_layers": 4, "bottom_layers": 4, "layer_height": "0.2 mm"},
                                  "infill": {"density": lay["density"], "pattern": "gyroid"}}
                    r = predict_one(eng, name, spec, t, F, material, prt, layout)
                    r["layout"] = lname
                    rows.append(r)
                    if "not_stated" in r:
                        print("  %-8s %-9s %-6s no stated strength ratio: %s" % (name, prt, lname or "solid", r["not_stated"]))
                        continue
                    print("  %-8s %-9s %-6s fails at %7.1f N = %5.2f kg, %-9s %s %s" % (name, prt, lname or "solid", r["fails_at_n"], r["fails_at_kg"],
                          r["region"], r["mode"], ",".join(r["flags"]) if r["flags"] else ""))
    finally:
        eng.close()
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)
    return rows


# ------------------------------------------------------------------------------------------------ records and the comparison

REQUIRED = ["part", "print", "filament", "settings", "break"]


def read_record(path):
    """a break record, or a reason it cannot be scored"""
    try:
        r = json.loads(Path(path).read_text())
    except (OSError, ValueError) as e:
        return None, "not readable as JSON: %s" % e
    missing = [k for k in REQUIRED if k not in r]
    if missing:
        return None, "missing " + ", ".join(missing)
    b = r["break"]
    if not isinstance(b.get("mass_kg"), (int, float)) or not b["mass_kg"] > 0:
        return None, "break.mass_kg (bucket, water and everything hanging below the part) is missing"
    if not (PARTS / (r["part"] + ".json")).exists():
        return None, "unknown part '%s'" % r["part"]
    spec = part_spec(r["part"])
    if r["print"] not in spec["prints"]:
        return None, "part '%s' has no print '%s' (%s)" % (r["part"], r["print"], ", ".join(spec["prints"]))
    if b.get("region") and b["region"] not in spec["regions"]:
        return None, "break.region '%s' is not one of %s" % (b["region"], ", ".join(spec["regions"]))
    st = r["settings"]
    dens = str(st.get("infill_density", "100 %")).replace("%", "").strip()
    try:
        d = float(dens) / (100 if float(dens) > 1 else 1)
    except ValueError:
        return None, "settings.infill_density '%s' is not a percentage" % st.get("infill_density")
    if d < 1 - 1e-9:
        if str(st.get("infill_pattern", "")).lower() != "gyroid":
            return None, "infill pattern '%s' is not modelled (only gyroid, or 100 %%)" % st.get("infill_pattern")
        if d < 0.1:
            return None, "infill below 10 %% has no strength model"
    return r, None


def layout_of(r):
    """the record's walls and infill, or None for a solid print"""
    st = r["settings"]
    dens = str(st.get("infill_density", "100 %")).replace("%", "").strip()
    d = float(dens) / (100 if float(dens) > 1 else 1)
    if d >= 1 - 1e-9:
        return None
    return {"walls": {"perimeters": int(st.get("perimeters", 2)), "line_width": "%g mm" % st.get("line_width_mm", 0.45),
                      "top_layers": int(st.get("top_layers", 4)), "bottom_layers": int(st.get("bottom_layers", 4)),
                      "layer_height": "%g mm" % st.get("layer_height_mm", 0.2)},
            "infill": {"density": d, "pattern": "gyroid"}}


def score(rows):
    """the step's numbers: median absolute error of the failure load, its signed median, places matched"""
    errs = [x["error"] for x in rows]
    placed = [x for x in rows if x.get("observed_region")]
    out = {"records": len(rows), "median_abs_error": statistics.median([abs(e) for e in errs]) if errs else None,
           "median_error": statistics.median(errs) if errs else None,
           "places_matched": sum(1 for x in placed if x["observed_region"] == x["predicted_region"]), "places_reported": len(placed)}
    out["passes"] = bool(len(rows) >= 10 and out["median_abs_error"] <= 0.25 and len(placed) >= 10
                         and out["places_matched"] >= 0.8 * len(placed))
    return out


def model_predictions(keys):
    """the model's prediction for every (part, print, material, layout) asked, one project per part and material"""
    out = {}
    tmp = tempfile.mkdtemp(prefix="openphysics-breaks-", dir="/tmp")
    eng = Engine(tmp)
    try:
        for part, mat in sorted({(k[0], k[2]) for k in keys}):
            spec = part_spec(part)
            t, F = setup_part(eng, part, spec, mat, project="breaks_%s_%s" % (part, mat))
            for k in sorted(k for k in keys if k[0] == part and k[2] == mat):
                out[k] = predict_one(eng, part, spec, t, F, mat, k[1], json.loads(k[3]))
    finally:
        eng.close()
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)
    return out


def cmd_compare(folder, predictor=model_predictions, quiet=False):
    """every record in folder against the model; writes score.json beside them. predictor maps a set of keys (part,
    print, material, layout as JSON) to predictions; the self-test passes its own."""
    folder = Path(folder)
    recs, refused = [], []
    for path in sorted(folder.glob("*.json")):
        if path.name == "score.json":
            continue
        r, why = read_record(path)
        if not r:
            refused.append({"record": path.name, "why": why})
            continue
        mat = r["filament"].get("library", "pla_fff")
        recs.append((path.name, r, (r["part"], r["print"], mat, json.dumps(layout_of(r), sort_keys=True))))
    preds = predictor({k for _, _, k in recs}) if recs else {}
    rows = []
    for name, r, k in recs:
        p = preds[k]
        if "not_stated" in p:  # the model makes no statement for this print: listed, not scored
            refused.append({"record": name, "why": "the analysis states no strength ratio: %s" % p["not_stated"]})
            continue
        meas = r["break"]["mass_kg"]
        lay = json.loads(k[3])
        row = {"record": name, "part": r["part"], "print": r["print"], "material": k[2],
               "layout": "solid" if not lay else "%g %% gyroid" % (100 * lay["infill"]["density"]),
               "predicted_kg": p["fails_at_kg"], "measured_kg": meas, "error": p["fails_at_kg"] / meas - 1,
               "predicted_region": p["region"], "observed_region": r["break"].get("region"), "mode": p.get("mode"),
               "flags": p.get("flags", [])}  # a flagged answer is read away from a held face or a sharp corner
        fd = r["break"].get("first_damage_kg")
        if isinstance(fd, (int, float)) and fd > 0:
            row["first_damage_kg"] = fd
            row["error_against_first_damage"] = p["fails_at_kg"] / fd - 1
        rows.append(row)
    out = {"format": "openphysicsai-break-score", "format_version": 1, "rows": rows, "refused": refused, "score": score(rows),
           "reading": "predicted: the stated first-failure load of the analysis (strength.answer); measured: the hanging mass at the break; "
                      "error = predicted / measured - 1, negative when the model is conservative"}
    (folder / "score.json").write_text(json.dumps(out, indent=1) + "\n")
    if not quiet:
        for x in rows:
            print("  %-26s %-8s %-9s %-10s predicted %6.2f kg, measured %6.2f kg, %+6.1f %%, place %s / %s" % (
                x["record"][:26], x["part"], x["print"], x["layout"], x["predicted_kg"], x["measured_kg"], 100 * x["error"],
                x["predicted_region"], x["observed_region"] or "-"))
        for x in refused:
            print("  %-26s not scored: %s" % (x["record"][:26], x["why"]))
        sc = out["score"]
        if rows:
            print("  %d records: median absolute error %.1f %% (signed %+.1f %%), places %d of %d; GOALS G17 step 5 %s" % (
                sc["records"], 100 * sc["median_abs_error"], 100 * sc["median_error"], sc["places_matched"], sc["places_reported"],
                "passes" if sc["passes"] else "does not pass (10 records or more, at most 25 %, places 8 in 10)"))
    return out


# ------------------------------------------------------------------------------------------------ the self-test


def record(part, prt, mass, region=None, density="100 %", pattern="gyroid", library="pla_fff", drop=None):
    r = {"format": "openphysicsai-break-record", "format_version": 1, "part": part, "print": prt, "specimen": "selftest",
         "filament": {"brand": "selftest", "material": "PLA", "library": library},
         "settings": {"layer_height_mm": 0.2, "line_width_mm": 0.45, "perimeters": 2, "top_layers": 4, "bottom_layers": 4,
                      "infill_density": density, "infill_pattern": pattern},
         "break": {"mass_kg": mass, "region": region, "how_weighed": "selftest"}}
    if drop:
        del r["break"][drop]
    return r


def selftest():
    """the comparison's own check (criteria in GOALS.md G17 step 5): synthetic records against a stand-in predictor whose
    values are known, so that every error, the median and the places are known exactly; three records it must refuse;
    and one pass through the model, which must give what `predict` gives"""
    ok = []

    def check(c, what):
        ok.append(bool(c))
        print(("  ok   " if c else "  FAIL ") + what)

    print("== the comparison against a stand-in predictor")
    tmp = Path(tempfile.mkdtemp(prefix="openphysics-breaks-selftest-", dir="/tmp"))
    known = {("bar", "flat"): (13.0, "gauge"), ("bar", "upright"): (10.0, "gauge"), ("hook", "flat"): (9.5, "bend"),
             ("bracket", "side"): (7.2, "corner"), ("bracket", "standing"): (5.3, "corner"), ("beam", "flat"): (9.2, "root")}

    def standin(keys):
        out = {}
        for k in keys:
            kg, reg = known[(k[0], k[1])]
            lay = json.loads(k[3])
            if lay:
                kg *= lay["infill"]["density"]  # a stand-in rule: what matters is that the comparison uses it as given
            out[k] = {"fails_at_kg": kg, "region": reg, "mode": "standin"}
        return out

    # measured = predicted x factor: the error of each is 1 / factor - 1; two places wrong, one not reported
    plan = [("bar", "flat", "100 %", 1.10, "gauge"), ("bar", "flat", "100 %", 0.95, "gauge"), ("bar", "upright", "100 %", 1.30, "gauge"),
            ("hook", "flat", "100 %", 1.60, "bend"), ("bracket", "side", "100 %", 1.45, "corner"), ("bracket", "side", "100 %", 1.50, "arm"),
            ("bracket", "standing", "100 %", 1.05, "corner"), ("bracket", "standing", "100 %", 0.90, "corner"),
            ("beam", "flat", "20 %", 2.00, "root"), ("beam", "flat", "50 %", 1.70, "root"), ("beam", "flat", "100 %", 1.20, "span"),
            ("beam", "flat", "100 %", 1.25, None)]
    for i, (part, prt, dens, f, reg) in enumerate(plan):
        kg, _ = known[(part, prt)]
        d = float(dens.replace("%", "")) / 100
        (tmp / ("r%02d.json" % i)).write_text(json.dumps(record(part, prt, kg * d * f, reg, dens)))
    (tmp / "x1.json").write_text(json.dumps(record("beam", "flat", 5.0, "root", "20 %", "grid")))
    (tmp / "x2.json").write_text(json.dumps(record("bracket", "side", 5.0, "corner", drop="mass_kg")))
    (tmp / "x3.json").write_text(json.dumps(record("hook", "flat", 5.0, "the middle")))
    out = cmd_compare(tmp, predictor=standin, quiet=True)
    rows = {x["record"]: x for x in out["rows"]}
    worst = max(abs(rows["r%02d.json" % i]["error"] - (1 / f - 1)) for i, (_, _, _, f, _) in enumerate(plan))
    check(len(rows) == len(plan) and worst <= 1e-9, "every synthetic record's error equals 1 / factor - 1 (%d records, worst %.1e)" % (len(rows), worst))
    want = statistics.median([abs(1 / f - 1) for (_, _, _, f, _) in plan])
    sc = out["score"]
    check(abs(sc["median_abs_error"] - want) <= 1e-12, "the median absolute error is the median of |1 / factor - 1|: %.6f" % sc["median_abs_error"])
    check(sc["places_reported"] == 11 and sc["places_matched"] == 9, "places: %d of %d reported match (9 of 11 built so)" % (sc["places_matched"], sc["places_reported"]))
    why = {x["record"]: x["why"] for x in out["refused"]}
    check("x1.json" in why and "not modelled" in why["x1.json"] and "x2.json" in why and "mass_kg" in why["x2.json"] and "x3.json" in why
          and "region" in why["x3.json"], "refused, with their reasons: a grid infill, a record without the break mass, an unknown place (%s)" %
          "; ".join("%s: %s" % kv for kv in sorted(why.items())))
    check((tmp / "score.json").exists() and json.loads((tmp / "score.json").read_text())["score"] == sc, "score.json is written beside the records")
    print("== one pass through the model")
    tmp2 = Path(tempfile.mkdtemp(prefix="openphysics-breaks-selftest-", dir="/tmp"))
    (tmp2 / "m1.json").write_text(json.dumps(record("bracket", "side", 10.0, "corner")))
    (tmp2 / "m2.json").write_text(json.dumps(record("beam", "flat", 6.0, "root", "20 %")))
    out2 = cmd_compare(tmp2, quiet=True)
    direct = model_predictions({("bracket", "side", "pla_fff", json.dumps(None)),
                                ("beam", "flat", "pla_fff", json.dumps(layout_of(record("beam", "flat", 1, density="20 %")), sort_keys=True))})
    rows2 = {x["part"]: x for x in out2["rows"]}
    dk = {k[0]: v for k, v in direct.items()}
    check(len(rows2) == 2 and all(abs(rows2[p]["predicted_kg"] - dk[p]["fails_at_kg"]) <= 1e-9 * dk[p]["fails_at_kg"] for p in ("bracket", "beam"))
          and rows2["beam"]["layout"] == "20 % gyroid", "the model's predictions in the comparison equal a direct run (bracket %.4f kg, beam at 20 %% %.4f kg)" %
          (rows2.get("bracket", {}).get("predicted_kg", float("nan")), rows2.get("beam", {}).get("predicted_kg", float("nan"))))
    passed = all(ok)
    import shutil
    if passed:
        shutil.rmtree(tmp), shutil.rmtree(tmp2)
    else:
        print("  kept for a look: %s %s" % (tmp, tmp2))
    print("\nBREAKS COMPARISON SELF-TEST: %d passed, %d failed" % (sum(ok), len(ok) - sum(ok)))
    return 0 if passed else 1


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "parts":
        write_parts()
    elif cmd == "predict":
        cmd_predict(sys.argv[2:])
    elif cmd == "compare":
        cmd_compare(sys.argv[2] if len(sys.argv) > 2 else SEALED)
    elif cmd == "selftest":
        sys.exit(selftest())
    else:
        print(__doc__)
        sys.exit(2)
