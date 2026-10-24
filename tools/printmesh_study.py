#!/usr/bin/env python3
"""printmesh_study.py - G17 step 4 (GOALS.md): does a printed part's strength ratio R settle as the voxel mesh is refined?

Criterion, written in GOALS.md before the first run: in pla_fff printed flat (build direction z) with Tsai-Wu, at element
sizes 1, 0.5 and 0.25 mm, for the sample bracket (foot held, 20 N on the top of its wall, as in printstrength_app.py)
and for a C-ring opened by its tips (inner radius 10 mm, outer 14 mm, 4 mm wide, a gap of 0.25 rad, 10 N on one tip
with the other held, turned so that its apex lies on a 45-degree staircase of the voxels): R at 0.5 and 0.25 mm differ
by at most 5 % and the governing point moves by at most 2 mm between them; for the C-ring, R at 0.25 mm is within 5 %
of the curved-beam closed form (Winkler-Bach at the inner fibre of the apex).

First run (GOALS.md): both parts failed on the voxel mesh. The rerun, also written before it ran, reads R as step 4 now
states it (the smallest strength ratio outside max(1 mm, 1.5 element sizes) of a sharp inside corner or a supported node,
the peak inside flagged) and solves three series: the C-ring on TET10 at surface sizes 1, 0.7 and 0.5 mm, the bracket on
voxels at 1, 0.5 and 0.25 mm and on TET10 at 1, 0.7 and 0.5 mm; the two finest sizes of each are compared.

The parts are solved through the MCP server's embedded engine, as an agent would. The workspace is deleted when every
check passes and kept, with its path printed, when one fails.

    make openphysics-mcp && python3 tools/printmesh_study.py [--series cring:voxel:1,0.5,0.25 ...] [--wait MINUTES] [--keep] [--turn DEG]

A series is part:kind:sizes with kind voxel (solved on the voxels as meshed), tet (TET10 at that surface size) or auto
(voxels of that size, solved on the mesh the engine chooses for a printed answer, GOALS.md G17 step 9).
"""
import json
import math
import shutil
import struct
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from mcptest import Client  # noqa: E402

MATERIAL = "pla_fff"
RI, RO, WIDTH, HALF_GAP = 10.0, 14.0, 4.0, 0.125  # mm, mm, mm, rad
TURN = -math.pi / 4  # the apex (90 degrees before the turn) lies at 45 degrees to the voxel axes
if "--turn" in sys.argv[:-1]:  # a diagnosis only: the criterion is the 45-degree turn
    TURN = math.radians(float(sys.argv[sys.argv.index("--turn") + 1]))
RING_FORCE_N = 10.0
BRACKET_FORCE_N = 20.0

results = []


def check(ok, what):
    results.append(bool(ok))
    print(("  ok   " if ok else "  FAIL ") + what)
    return ok


def turn(p):
    c, s = math.cos(TURN), math.sin(TURN)
    return (c * p[0] - s * p[1], s * p[0] + c * p[1], p[2])


def write_stl(path, tris, name):
    with open(path, "wb") as f:
        f.write(name.ljust(80, b" "))
        f.write(struct.pack("<I", len(tris)))
        for t in tris:
            f.write(struct.pack("<3f", 0, 0, 0))
            for v in t:
                f.write(struct.pack("<3f", *v))
            f.write(b"\0\0")


def c_ring_stl(path):
    """the ring from -90 + half gap to 270 - half gap degrees, outward winding, 2-degree facets, then turned"""
    a0, a1 = -math.pi / 2 + HALF_GAP, 1.5 * math.pi - HALF_GAP
    n = int(math.ceil((a1 - a0) / math.radians(2)))
    ring = []
    for k in range(n + 1):
        a = a0 + (a1 - a0) * k / n
        c, s = math.cos(a), math.sin(a)
        ring.append({"ib": (RI * c, RI * s, 0.0), "it": (RI * c, RI * s, WIDTH),
                     "ob": (RO * c, RO * s, 0.0), "ot": (RO * c, RO * s, WIDTH)})
    tris = []
    for k in range(n):
        p, q = ring[k], ring[k + 1]
        tris += [(p["ob"], q["ob"], q["ot"]), (p["ob"], q["ot"], p["ot"])]  # outer, facing away from the centre
        tris += [(p["ib"], q["it"], q["ib"]), (p["ib"], p["it"], q["it"])]  # inner, facing the centre
        tris += [(p["ib"], q["ob"], p["ob"]), (p["ib"], q["ib"], q["ob"])]  # bottom
        tris += [(p["it"], p["ot"], q["ot"]), (p["it"], q["ot"], q["it"])]  # top
    s, e = ring[0], ring[n]
    tris += [(s["ib"], s["ob"], s["ot"]), (s["ib"], s["ot"], s["it"])]      # the tip that is pulled
    tris += [(e["ib"], e["ot"], e["ob"]), (e["ib"], e["it"], e["ot"])]      # the tip that is held
    write_stl(path, [tuple(turn(v) for v in t) for t in tris], b"printmesh_study C-ring")
    rm = 0.5 * (RI + RO)
    pulled = {"point": list(turn((rm * math.sin(HALF_GAP), -rm * math.cos(HALF_GAP), WIDTH / 2))),
              "normal": list(turn((-math.cos(HALF_GAP), -math.sin(HALF_GAP), 0.0)))}
    held = {"point": list(turn((-rm * math.sin(HALF_GAP), -rm * math.cos(HALF_GAP), WIDTH / 2))),
            "normal": list(turn((math.cos(HALF_GAP), -math.sin(HALF_GAP), 0.0)))}
    return pulled, held


def ring_closed_form(xt_mpa, normal_n=None, moment_nmm=None):
    """Winkler-Bach: the inner fibre of the apex carries N = P and M = P (its distance from the line through the tips);
    given N and M (from the loads the mesh really carries), the same formula with them"""
    area = (RO - RI) * WIDTH
    rbar = 0.5 * (RI + RO)
    rn = (RO - RI) / math.log(RO / RI)
    e = rbar - rn
    normal = RING_FORCE_N if normal_n is None else normal_n
    moment = RING_FORCE_N * rbar * (1 + math.cos(HALF_GAP)) if moment_nmm is None else moment_nmm
    stress = normal / area + moment * (rn - RI) / (area * e * RI)
    return xt_mpa / stress, stress


def library_xt_mpa():
    data = json.loads((ROOT / "src/ctl/materials.json").read_text())
    recs = data["materials"] if isinstance(data, dict) else data
    rec = next(r for r in recs if r["id"] == MATERIAL)
    return rec["tensile_strength_along_layers_pa"]["value"] / 1e6


def find(o, key):
    if isinstance(o, dict):
        if key in o:
            return o[key]
        o = list(o.values())
    if isinstance(o, list):
        for v in o:
            r = find(v, key)
            if r is not None:
                return r
    return None


def solve(c, ws, name, stl, mesh, setup, about=None):
    def call(op, args):
        r = c.call(op, args)["result"]["structuredContent"]
        if not r.get("ok"):
            raise RuntimeError("%s: %s" % (op, r))
        return r.get("value", {})
    call("project_create", {"name": name, "description": "printmesh_study: does R settle as the mesh is refined (GOALS.md G17 step 4)"})
    call("geometry_import", {"path": str(stl), "units": "mm", "name": "part"})
    call("material_assign", {"body": "part", "material": MATERIAL, "source": "user"})
    t0 = time.time()
    built = call("mesh_generate", mesh)
    for sel, query in setup["selections"]:
        call("selection_create", {"name": sel, "query": query, "source": "user"})
    for bc in setup["conditions"]:
        call("boundary_apply", dict(bc, source="user"))
    model = {"type": "orthotropic", "library": MATERIAL, "build_direction": "z", "raster_reference": "x", "criterion": "tsai_wu"}
    if mesh.get("element_size") and not setup.get("auto"):  # a voxel series studies the voxels (GOALS.md G17 step 9's as_meshed)
        model["mesh"] = "as_meshed"
    run = call("analysis_run", {"label": name, "material_model": model})
    status = {}
    minutes = float(sys.argv[sys.argv.index("--wait") + 1]) if "--wait" in sys.argv[:-1] else 10  # a detached run may wait longer
    for _ in range(int(6 * minutes)):
        status = call("job_status", {"job_id": run["job_id"], "wait_seconds": 10})
        if status.get("state") not in ("queued", "running"):
            break
    if status.get("state") != "succeeded":
        raise RuntimeError("%s did not succeed: %s" % (name, status))
    summary = next((json.loads(p.read_text()) for p in (ws / name).rglob("summary.json")), None)
    st = (summary or {}).get("strength") or {}
    row = {"elements": built.get("mesh", {}).get("elements"), "seconds": time.time() - t0,
           "R": st.get("stated_strength_ratio"), "at": st.get("stated_location_mm"), "mode": st.get("stated_governing_mode"),
           "point_R": st.get("smallest_strength_ratio"), "flag": st.get("peak_flag"), "zone": st.get("singularity_zone_mm"),
           "stair": st.get("stated_near_staircase"), "tet_hint": "TET10" in ((st.get("answer") or {}).get("text") or ""),
           "choice": run.get("mesh_choice"), "element": st.get("element")}
    if about is not None:  # the force and moment the mesh really carries, for the closed form at the applied load
        q = call("results_quantities", {"job_id": run["job_id"], "about": about})
        row["applied_force_n"], row["applied_moment_nm"] = find(q, "applied_surface_force_n"), find(q, "applied_surface_moment_nm")
    return row


def mesh_args(kind, h):
    """tet: TET10 at surface size h; voxel: voxels of h solved as meshed; auto: voxels of h, the engine's choice (G17 step 9)"""
    return {"method": "tet", "surface_size": h, "order": 2} if kind == "tet" else {"element_size": "%g mm" % h}


def main():
    series = [a.split(":") for a in sys.argv[sys.argv.index("--series") + 1:] if ":" in a] if "--series" in sys.argv else \
        [["cring", "tet", "1,0.7,0.5"], ["bracket", "voxel", "1,0.5,0.25"], ["bracket", "tet", "1,0.7,0.5"]]
    tmp = Path(tempfile.mkdtemp(prefix="openphysics-printmesh-"))
    ws = tmp / "projects"
    ws.mkdir()
    ring_stl, bracket_stl = tmp / "cring.stl", tmp / "bracket.stl"
    pulled, held = c_ring_stl(ring_stl)
    shutil.copyfile(ROOT / "samples" / "bracket.stl", bracket_stl)
    cos_t, sin_t = math.cos(TURN), math.sin(TURN)
    parts = {
        "bracket": (bracket_stl, {
            "selections": [("foot", {"all": [{"facing": {"direction": "down"}}, {"plane": {"axis": "z", "at": "min"}}]}),
                           ("top", {"extreme": {"direction": "+z"}})],
            "conditions": [{"name": "hold", "kind": "fixed", "selection": "foot"},
                           {"name": "push", "kind": "force", "selection": "top", "force": ["%g N" % BRACKET_FORCE_N, 0, 0]}]}),
        "cring": (ring_stl, {
            "selections": [("held", {"plane": dict(held, tolerance=0.01)}), ("pulled", {"plane": dict(pulled, tolerance=0.01)})],
            "conditions": [{"name": "hold", "kind": "fixed", "selection": "held"},
                           {"name": "pull", "kind": "force", "selection": "pulled",
                            "force": ["%.9g N" % (RING_FORCE_N * cos_t), "%.9g N" % (RING_FORCE_N * sin_t), 0]}]}),
    }
    xt = library_xt_mpa()
    r_cf, s_cf = ring_closed_form(xt)
    apex = list(turn((0.0, 0.5 * (RI + RO), WIDTH / 2)))
    hoop = turn((1.0, 0.0, 0.0))  # the direction of the apex section's normal force
    c = Client(["--embedded", "--workspace", str(ws), "--allow-read", str(tmp)])
    c.initialize()
    rows = {}
    try:
        for part, kind, sizes in series:
            stl, setup = parts[part]
            for h in [float(x) for x in sizes.split(",")]:
                name = "%s_%s_%s" % (part, kind, ("%g" % h).replace(".", "p"))
                r = rows[(part, kind, h)] = solve(c, ws, name, stl, mesh_args(kind, h), dict(setup, auto=kind == "auto"), apex if part == "cring" else None)
                at = r["at"] or [float("nan")] * 3
                extra = ""
                if part == "cring":  # where the stated point is on the ring, and the closed form at the load the mesh carries
                    x, y = at[0] * cos_t + at[1] * sin_t, -at[0] * sin_t + at[1] * cos_t
                    f, mo = r["applied_force_n"], r["applied_moment_nm"]
                    n_app = sum(f[k] * hoop[k] for k in range(3))
                    r_app = ring_closed_form(xt, n_app, mo[2] * 1e3)[0]
                    extra = "  radius %.2f mm, %.1f deg from the apex; closed form at the applied load %.5g" % (
                        math.hypot(x, y), math.degrees(math.atan2(x, y)), r_app)
                print("  %-8s %-5s h %-5g %7s elements  R %-9.5g (point %.5g, peak flag %s, zone %.3g mm)  %-24s at (%7.2f %7.2f %6.2f) mm%s  [%.1f s]" %
                      (part, kind, h, r["elements"], r["R"] or float("nan"), r["point_R"] or float("nan"), r["flag"], r["zone"] or float("nan"),
                       r["mode"], at[0], at[1], at[2], extra, r["seconds"]), flush=True)
                print("           at a voxel staircase (GOALS.md G17 step 8): %s; the answer names TET10: %s; solved on %s" % (
                    {True: "yes", False: "no", None: "not reported"}[r["stair"]], "yes" if r["tet_hint"] else "no",
                    "%s at %g mm (%s elements; mesh_choice, GOALS.md G17 step 9)" % (r["choice"].get("solved_on"), r["choice"].get("surface_size_mm", 0),
                                                                                       r["choice"].get("elements")) if r["choice"] else
                    "the mesh as made (%s)" % (r["element"] or "?")), flush=True)
    finally:
        c.close()
    print("  C-ring closed form (Winkler-Bach): inner-fibre stress %.4g MPa at the apex under the requested 10 N, Xt %.4g MPa, R %.5g" % (s_cf, xt, r_cf))
    for part, kind, sizes in series:
        hs = [float(x) for x in sizes.split(",")]
        if len(hs) < 2:
            continue
        a, b = rows[(part, kind, hs[-2])], rows[(part, kind, hs[-1])]
        if a["R"] is None or b["R"] is None:
            check(False, "%s on %s: a ratio is stated at %g and %g mm" % (part, kind, hs[-2], hs[-1]))
            continue
        change = b["R"] / a["R"] - 1
        moved = math.dist(a["at"], b["at"])
        check(abs(change) <= 0.05, "%s on %s: R moves %+.1f %% from %g to %g mm (criterion 5 %%)" % (part, kind, 100 * change, hs[-2], hs[-1]))
        check(moved <= 2.0, "%s on %s: the governing point moves %.2f mm (criterion 2 mm)" % (part, kind, moved))
        if part == "cring":
            off = b["R"] / r_cf - 1
            check(abs(off) <= 0.05, "C-ring on %s: R at %g mm is %+.1f %% from the closed form (criterion 5 %%)" % (kind, hs[-1], 100 * off))
    ok = all(results)
    print("PRINT MESH STUDY: %d passed, %d failed" % (sum(results), len(results) - sum(results)))
    if ok and "--keep" not in sys.argv:
        shutil.rmtree(tmp)
    else:
        print("  workspace kept: %s" % tmp)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
