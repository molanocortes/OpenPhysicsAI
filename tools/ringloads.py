#!/usr/bin/env python3
"""ringloads.py - loads on curved voxel faces, verified on a thin ring (GOALS.md G22; the case and its criterion were
written before the first run, 2026-10-08).

A quarter of a ring, radii 80 and 87 mm, two element layers long, its two cut planes and its bottom on frictionless
supports, meshed with voxels. Each load is one condition on one selection, the whole inner or outer surface, so no
grouping can hide a bias:
  P  6 MPa on the inner surface, the ends open: against Lame, the radial displacement at r = 81, 83.5 and 86 mm;
  T  convection inside (1000 W/(m^2 K) to 600 C) and outside (4000 W/(m^2 K) to 20 C), stepped to its steady state:
     against radial conduction, the temperature rise above the outside fluid at r = 81 and 86 mm and the drop between
     them (the local heat flow through the wall).
Every quantity at nine angles, 5 to 85 degrees, halfway along the ring, by probes 1 mm inside the surfaces.
Pass, at 0.25 mm: for every quantity the nine values' spread is at most 1 % of the closed form (2 % for the drop) and
their mean lies within 2 % of it. Recorded without a criterion: the same at 0.5 mm, and case T with G19's films
(2400 W/(m^2 K) to 2348 K inside, 50 000 W/(m^2 K) to 310 K outside).

The material is the check's own set of constants (E 72 GPa, nu 0.33, k 110 W/(m K), 2670 kg/m^3, 900 J/(kg K)), close
to AlSi10Mg and labelled as such; it is not a library material.

Usage, from the repository root after `make am`:
    python3 tools/ringloads.py            the criterion's runs, then the runs for the record
    python3 tools/ringloads.py --quick    the criterion's runs only (as `make test` runs it)
    python3 tools/ringloads.py --out DIR  also write ringloads.json there; --keep keeps the workspaces
    python3 tools/ringloads.py --placements   GOALS.md G22 step 5 (b): case P with the quarter turned by zero to three
                                              quarter turns on the grid, at 0.25 mm (judged) and 0.5 mm (recorded)
Exit status 0 when the criterion passes. The workspaces are removed when it passes and kept when it fails.
"""
import argparse
import json
import math
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mcptest import Client  # noqa: E402

A_MM, B_MM = 80.0, 87.0
E_PA, NU, K_W, RHO, CP = 72e9, 0.33, 110.0, 2670.0, 900.0
P_MPA = 6.0
FILMS = {"check": (1000.0, 600.0, 4000.0, 20.0),                       # h1 W/(m^2 K), T1 C, h2, T2 C
         "g19": (2400.0, 2348.0 - 273.15, 50000.0, 310.0 - 273.15)}
ANGLES = [5.0 + 10.0 * i for i in range(9)]
SEGMENTS = 360   # facets over the quarter: 0.25 degree each, a chord 0.2 um from the arc
SPREAD, SPREAD_DROP, MEAN = 0.01, 0.02, 0.02


# ---- closed forms ------------------------------------------------------------------------------------------------

def lame_u_mm(r):
    """radial displacement (mm) of a ring with open ends under internal pressure (plane stress, sigma_z = 0)"""
    a2, b2 = A_MM * A_MM, B_MM * B_MM
    A = P_MPA * a2 / (b2 - a2)                       # MPa
    return ((1 - NU) * A * r + (1 + NU) * A * b2 / r) / (E_PA * 1e-6)


def conduction(films):
    """steady radial conduction between two films: heat per metre of ring, and T(r) in C"""
    h1, t1, h2, t2 = films
    a, b = A_MM * 1e-3, B_MM * 1e-3
    q = (t1 - t2) / (1 / (2 * math.pi * a * h1) + math.log(b / a) / (2 * math.pi * K_W) + 1 / (2 * math.pi * b * h2))
    ta = t1 - q / (2 * math.pi * a * h1)
    return q, lambda r: ta - q * math.log(r / A_MM) / (2 * math.pi * K_W)


# ---- the quarter ring --------------------------------------------------------------------------------------------

def ring_stl(path, length, turns=0):
    """binary STL of the quarter ring (first quadrant, z from 0 to length, turned by `turns` quarter turns about z),
    outward normals; returns the triangle ids of the inner and outer surfaces in file order"""
    def p(r, th, z):
        if th == 0.5 * math.pi:   # the cut plane x = 0 exactly (cos would leave 5e-15), so its faces are normal to x
            return (0.0, r, z)
        return (r * math.cos(th), r * math.sin(th), z)

    tris, inner, outer = [], [], []

    def quad(q0, q1, q2, q3, tag=None):   # counter-clockwise seen from outside
        for t in ((q0, q1, q2), (q0, q2, q3)):
            if tag is not None:
                tag.append(len(tris))
            tris.append(t)

    th = [0.5 * math.pi * i / SEGMENTS for i in range(SEGMENTS)] + [0.5 * math.pi]
    for i in range(SEGMENTS):
        t0, t1 = th[i], th[i + 1]
        quad(p(A_MM, t0, 0), p(A_MM, t0, length), p(A_MM, t1, length), p(A_MM, t1, 0), inner)    # faces the axis
        quad(p(B_MM, t0, 0), p(B_MM, t1, 0), p(B_MM, t1, length), p(B_MM, t0, length), outer)    # faces away
        quad(p(A_MM, t0, length), p(B_MM, t0, length), p(B_MM, t1, length), p(A_MM, t1, length))  # top, +z
        quad(p(A_MM, t0, 0), p(A_MM, t1, 0), p(B_MM, t1, 0), p(B_MM, t0, 0))                      # bottom, -z
    quad(p(A_MM, 0, 0), p(B_MM, 0, 0), p(B_MM, 0, length), p(A_MM, 0, length))                    # cut y = 0, -y
    e = 0.5 * math.pi
    quad(p(A_MM, e, 0), p(A_MM, e, length), p(B_MM, e, length), p(B_MM, e, 0))                    # cut x = 0, -x
    for _ in range(turns % 4):   # (x, y) -> (-y, x): exact, so the turned quarter is the same solid on the grid
        tris = [tuple((-q[1], q[0], q[2]) for q in t) for t in tris]
    vol = 0.0
    for a, b, c in tris:
        vol += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6
    exact = 0.25 * math.pi * (B_MM ** 2 - A_MM ** 2) * length
    if not (0.999 * exact < vol < 1.001 * exact):
        raise RuntimeError(f"the quarter ring is not closed and outward ({vol:.3f} mm^3 against {exact:.3f})")
    with open(path, "wb") as f:
        f.write(b"OpenPhysicsAI G22 quarter ring".ljust(80, b" "))
        f.write(struct.pack("<I", len(tris)))
        for t in tris:
            f.write(struct.pack("<3f", 0, 0, 0))
            for v in t:
                f.write(struct.pack("<3f", *v))
            f.write(b"\0\0")
    return len(tris), inner, outer


# ---- one run through the operation layer -------------------------------------------------------------------------

class Failed(RuntimeError):
    pass


def call(c, name, args):
    r = c.call(name, args)
    sc = (r.get("result") or {}).get("structuredContent") or {}
    if (r.get("result") or {}).get("isError") or sc.get("ok") is not True:
        raise Failed(f"{name}: {json.dumps(sc.get('error') or r.get('error'))[:600]}")
    return sc.get("value") or {}


def wait_job(c, job_id, limit_s=540):
    t0 = time.time()
    while time.time() - t0 < limit_s:
        v = call(c, "job_status", {"job_id": job_id})
        if v.get("state") in ("succeeded", "failed", "cancelled"):
            if v.get("state") != "succeeded":
                raise Failed(f"job {job_id} {v.get('state')}: {json.dumps(v.get('error'))[:600]}")
            return v
        time.sleep(0.25)
    raise Failed(f"job {job_id} did not finish within {limit_s} s")


def turned(n, turns):
    for _ in range(turns % 4):
        n = (-n[1], n[0])
    return n


def plane_query(n):
    """the cut plane whose outward normal is n, a unit vector along x or y"""
    ax = "x" if n[0] else "y"
    pos = (n[0] or n[1]) > 0
    return {"all": [{"plane": {"axis": ax, "at": "max" if pos else "min"}},
                    {"facing": {"direction": ("+" if pos else "-") + ax, "max_angle_deg": 0.5}}]}


def run(tmp, name, h_mm, case, films=None, turns=0):
    """returns the run's record: per quantity the closed form and the nine probed values"""
    length = 2 * h_mm
    stl = tmp / f"{name}.stl"
    ntri, inner, outer = ring_stl(stl, length, turns)
    rot = 0.5 * math.pi * (turns % 4)   # the angle a local angle is turned by on the grid
    ws = tmp / f"ws_{name}"
    c = Client(["--embedded", "--workspace", str(ws), "--allow-read", str(tmp)])
    t0 = time.time()
    try:
        c.initialize()
        call(c, "project_create", {"name": name, "description": "GOALS.md G22: loads on curved voxel faces, a quarter ring"})
        body = call(c, "geometry_import", {"path": str(stl), "units": "mm", "name": "ring", "units_source": "user"}).get("body", {})
        if body.get("triangles") not in (None, ntri):
            raise Failed(f"the import holds {body.get('triangles')} triangles, the file {ntri}: the ids would not match")
        tr = (body.get("placement") or {}).get("translation_mm") or [0.0, 0.0, 0.0]
        call(c, "material_define", {"material": {
            "id": "ring_check", "name": "constants of the G22 check (near AlSi10Mg)", "family": "metal", "status": "user_supplied",
            "processes": ["lpbf"], "provenance": "the verification case's stated constants, GOALS.md G22",
            "density_kg_m3": {"value": RHO}, "conductivity_w_per_mk": {"value": K_W}, "specific_heat_j_per_kgk": {"value": CP},
            "youngs_modulus_pa": {"value": E_PA}, "poisson_ratio": {"value": NU}, "expansion_1_per_k": {"value": 21e-6}}})
        call(c, "material_assign", {"body": "ring", "material": "ring_check", "source": "user"})
        mesh = call(c, "mesh_generate", {"element_size": f"{h_mm} mm", "max_elements": 400000}).get("mesh", {})
        sel = {}
        for nm, q in (("inner", {"triangles": inner}), ("outer", {"triangles": outer}),
                      ("cut_x", plane_query(turned((-1, 0), turns))), ("cut_y", plane_query(turned((0, -1), turns))),
                      ("bottom", {"all": [{"plane": {"axis": "z", "at": "min"}}, {"facing": {"direction": "-z", "max_angle_deg": 0.5}}]})):
            s = call(c, "selection_create", {"name": nm, "body": "ring", "query": q, "mode": "triangle", "source": "user",
                                             "description": f"the quarter ring's {nm.replace('_', ' ')}"}).get("selection", {})
            sel[nm] = {"stl_area_mm2": s.get("area_mm2"), "mesh_faces": (s.get("mesh_projection") or {}).get("faces"),
                       "mesh_area_mm2": (s.get("mesh_projection") or {}).get("mesh_area_mm2")}
        if case == "P":
            for nm in ("cut_x", "cut_y", "bottom"):
                call(c, "boundary_apply", {"name": f"sym_{nm}", "kind": "frictionless_support", "selection": nm, "source": "user",
                                           "description": "the quarter's symmetry planes and its bottom"})
            call(c, "boundary_apply", {"name": "p_inner", "kind": "pressure", "selection": "inner", "source": "user",
                                       "description": "internal pressure, one condition on the whole inner surface", "pressure": f"{P_MPA} MPa"})
            job = call(c, "analysis_run", {"analysis": "static_structural"})
        else:
            h1, t1, h2, t2 = films
            call(c, "boundary_apply", {"name": "film_inner", "kind": "convection", "selection": "inner", "source": "user",
                                       "description": "the inner film, one condition on the whole inner surface",
                                       "convection": {"coefficient": h1, "ambient": f"{t1} degC"}})
            call(c, "boundary_apply", {"name": "film_outer", "kind": "convection", "selection": "outer", "source": "user",
                                       "description": "the outer film, one condition on the whole outer surface",
                                       "convection": {"coefficient": h2, "ambient": f"{t2} degC"}})
            job = call(c, "analysis_run", {"analysis": "transient_thermal", "end_time": "100 s", "time_step": "2 s",
                                           "time_stepping": "fixed", "theta": 1, "initial_temperature": "25 degC",
                                           "output_every": 50})
        st = wait_job(c, job["job_id"])
        rec = {"name": name, "case": case, "element_mm": h_mm, "length_mm": length, "turns": turns % 4, "elements": mesh.get("elements"),
               "nodes": mesh.get("nodes"), "selections": sel}
        if case == "P":
            radii = [A_MM + 1.0, 0.5 * (A_MM + B_MM), B_MM - 1.0]
        else:
            radii = [A_MM + 1.0, B_MM - 1.0]
        pts = [[r * math.cos(math.radians(a) + rot) + tr[0], r * math.sin(math.radians(a) + rot) + tr[1], 0.5 * length + tr[2]]
               for r in radii for a in ANGLES]
        probes = call(c, "results_probe", {"job_id": job["job_id"], "points_mm": pts}).get("probes", [])
        if len(probes) != len(pts) or not all(p.get("inside") for p in probes):
            raise Failed(f"a probe fell outside the mesh: {json.dumps([p for p in probes if not p.get('inside')])[:400]}")
        quantities = []
        if case == "P":
            for i, r in enumerate(radii):
                vals = []
                for j, a in enumerate(ANGLES):
                    d = probes[i * len(ANGLES) + j]["displacement_mm"]
                    vals.append(d["x"] * math.cos(math.radians(a) + rot) + d["y"] * math.sin(math.radians(a) + rot))
                quantities.append({"quantity": f"radial displacement at r = {r:g} mm (mm)", "closed_form": lame_u_mm(r), "values": vals,
                                   "spread_limit": SPREAD})
            summary = st.get("summary") or st.get("result") or {}
            rec["load_report"] = summary.get("boundary_conditions") or summary.get("loads")
        else:
            q, T = conduction(films)
            hot = [probes[j]["temperature_c"] for j in range(len(ANGLES))]
            cold = [probes[len(ANGLES) + j]["temperature_c"] for j in range(len(ANGLES))]
            t2 = films[3]
            quantities.append({"quantity": f"rise above the outside fluid at r = {radii[0]:g} mm (K)", "closed_form": T(radii[0]) - t2,
                               "values": [v - t2 for v in hot], "spread_limit": SPREAD})
            quantities.append({"quantity": f"rise above the outside fluid at r = {radii[1]:g} mm (K)", "closed_form": T(radii[1]) - t2,
                               "values": [v - t2 for v in cold], "spread_limit": SPREAD})
            quantities.append({"quantity": f"drop from r = {radii[0]:g} to {radii[1]:g} mm, the local heat flow (K)",
                               "closed_form": T(radii[0]) - T(radii[1]), "values": [x - y for x, y in zip(hot, cold)],
                               "spread_limit": SPREAD_DROP})
            rec["films"] = {"inner_w_m2k": films[0], "inner_c": films[1], "outer_w_m2k": films[2], "outer_c": films[3]}
            rec["closed_form_heat_w_per_m"] = q
            summary = st.get("summary") or st.get("result") or {}
            rec["energy"] = summary.get("energy")
            rec["conditions"] = summary.get("conditions")
        for qd in quantities:
            cf, v = qd["closed_form"], qd["values"]
            mean = sum(v) / len(v)
            qd["mean"], qd["spread"] = mean, max(v) - min(v)
            qd["mean_error_rel"], qd["spread_rel"] = (mean - cf) / cf, (max(v) - min(v)) / abs(cf)
            qd["worst_angle_error_rel"] = max(abs(x - cf) for x in v) / abs(cf)
            qd["pass"] = qd["spread_rel"] <= qd["spread_limit"] and abs(qd["mean_error_rel"]) <= MEAN
        rec["quantities"] = quantities
        rec["warnings"] = sorted({w.get("code") for w in (st.get("warnings") or []) if isinstance(w, dict)})
        rec["seconds"] = round(time.time() - t0, 1)
        return rec
    finally:
        c.close()


def show(rec):
    films = f", films {rec['films']['inner_w_m2k']:g}/{rec['films']['outer_w_m2k']:g} W/(m^2 K)" if rec.get("films") else ""
    print(f"== {rec['name']}: case {rec['case']}, voxels {rec['element_mm']} mm, {rec.get('elements')} elements{films}, {rec['seconds']} s",
          flush=True)
    for qd in rec["quantities"]:
        vals = " ".join(f"{x:.6g}" for x in qd["values"])
        print(f"   {qd['quantity']}: closed form {qd['closed_form']:.6g}; at {', '.join(f'{a:g}' for a in ANGLES)} degrees: {vals}")
        print(f"     mean {100 * qd['mean_error_rel']:+.2f} %, spread {100 * qd['spread_rel']:.2f} % (limit {100 * qd['spread_limit']:g} %),"
              f" worst angle {100 * qd['worst_angle_error_rel']:.2f} %{'' if qd['pass'] else '   [outside the criterion]'}", flush=True)


def lobes(vals):
    """the mean and the two- and four-lobed terms over the nine angles, on which cos 2t and cos 4t are orthogonal"""
    c0 = sum(vals) / len(vals)
    c2 = 2 / len(vals) * sum(v * math.cos(2 * math.radians(t)) for v, t in zip(vals, ANGLES))
    c4 = 2 / len(vals) * sum(v * math.cos(4 * math.radians(t)) for v, t in zip(vals, ANGLES))
    return c0, c2 / c0, c4 / c0


def placements(a, tmp, commit):
    """GOALS.md G22 step 5 (b), written before its first run: at 0.25 mm in all four placements the spread at most 1 %
    and the mean within 2 % of Lame, the two-lobed term within 0.1 %, and the four placements' radial displacements
    within 0.1 % of each other (of Lame's value); at 0.5 mm the two-lobed term is recorded"""
    out = {"check": "GOALS.md G22 step 5 (b): the ring in four placements on the grid", "engine": commit, "angles_deg": ANGLES,
           "runs": []}
    ok = True
    for h in (0.25, 0.5):
        judged = h == 0.25
        recs = []
        for t in range(4):
            name = f"p_{int(h * 100):03d}_turn{t}"
            try:
                rec = run(tmp, name, h, "P", None, t)
            except Failed as e:
                print(f"== {name}: FAILED TO RUN: {e}", flush=True)
                out["runs"].append({"name": name, "error": str(e)})
                ok &= not judged
                continue
            for qd in rec["quantities"]:
                qd["c0"], qd["two_lobed"], qd["four_lobed"] = lobes(qd["values"])
                if judged:
                    qd["pass"] = qd["pass"] and abs(qd["two_lobed"]) <= 0.001
            rec["judged"] = judged
            show(rec)
            print("     two-lobed terms " + ", ".join(f"{100 * qd['two_lobed']:+.3f} %" for qd in rec["quantities"]) +
                  "; four-lobed " + ", ".join(f"{100 * qd['four_lobed']:+.3f} %" for qd in rec["quantities"]), flush=True)
            if judged:
                ok &= all(qd["pass"] for qd in rec["quantities"])
            recs.append(rec)
            out["runs"].append(rec)
        if len(recs) == 4:
            worst = max(abs(r["quantities"][q]["values"][j] - recs[0]["quantities"][q]["values"][j]) / abs(recs[0]["quantities"][q]["closed_form"])
                        for r in recs[1:] for q in range(3) for j in range(len(ANGLES)))
            out[f"placements_differ_rel_{h}"] = worst
            print(f"== at {h} mm the four placements differ by at most {100 * worst:.4f} % of Lame", flush=True)
            if judged:
                ok &= worst <= 0.001
        elif judged:
            ok = False
    out["pass"] = ok
    if a.out:
        Path(a.out).mkdir(parents=True, exist_ok=True)
        (Path(a.out) / "ringloads_placements.json").write_text(json.dumps(out, indent=1))
    print("PASS" if ok else "FAIL", f"(engine {commit})")
    if ok and not a.keep:
        shutil.rmtree(tmp, ignore_errors=True)
    else:
        print(f"workspaces kept in {tmp}")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--quick", action="store_true", help="the criterion's runs only")
    ap.add_argument("--out", help="directory for ringloads.json")
    ap.add_argument("--keep", action="store_true", help="keep the workspaces")
    ap.add_argument("--placements", action="store_true", help="G22 step 5 (b): case P in four placements on the grid")
    a = ap.parse_args()
    root = Path(__file__).resolve().parent.parent
    try:
        commit = subprocess.run(["git", "-C", str(root), "describe", "--always", "--dirty"], capture_output=True, text=True).stdout.strip()
    except OSError:
        commit = ""
    tmp = Path(tempfile.mkdtemp(prefix="nvringloads"))
    if a.placements:
        return placements(a, tmp, commit)
    plan = [("p_025", 0.25, "P", None, True), ("t_025", 0.25, "T", FILMS["check"], True)]
    if not a.quick:
        plan += [("p_050", 0.5, "P", None, False), ("t_050", 0.5, "T", FILMS["check"], False),
                 ("t_025_g19", 0.25, "T", FILMS["g19"], False)]
    out = {"check": "GOALS.md G22: loads on curved voxel faces", "engine": commit, "angles_deg": ANGLES,
           "criterion": "at 0.25 mm, every quantity's spread over the nine angles at most 1 % of the closed form (2 % for the drop) "
                        "and their mean within 2 % of it", "runs": []}
    ok = True
    for name, h, case, films, judged in plan:
        try:
            rec = run(tmp, name, h, case, films)
        except Failed as e:
            print(f"== {name}: FAILED TO RUN: {e}", flush=True)
            rec = {"name": name, "error": str(e)}
            ok &= not judged
            out["runs"].append(rec)
            continue
        rec["judged"] = judged
        show(rec)
        if judged:
            ok &= all(qd["pass"] for qd in rec["quantities"])
        out["runs"].append(rec)
    out["pass"] = ok
    if a.out:
        Path(a.out).mkdir(parents=True, exist_ok=True)
        (Path(a.out) / "ringloads.json").write_text(json.dumps(out, indent=1))
    print("PASS" if ok else "FAIL", f"(engine {commit})")
    if ok and not a.keep:
        shutil.rmtree(tmp, ignore_errors=True)
    else:
        print(f"workspaces kept in {tmp}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
