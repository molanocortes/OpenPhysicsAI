#!/usr/bin/env python3
"""printhole.py - GOALS.md G17 step 8 (c): does the printed answer read a hole's peak on voxels?

Criterion, written in GOALS.md before the first run (2026-10-08): a plate with a hole in tension (W 40, d 10 and L 120 mm
as in tools/hole_plate.py, 2 mm thick, a quarter on its two symmetry planes and frictionless below), printed with step
1's isotropic fixture made with Poisson's ratio 0 so that the plane solution holds through the thickness, on voxels at
0.5 and 0.25 mm: the stated ratio within 5 % of Xt over Heywood's peak (Kt = 2 + (1 - d/W)^3 times the net-section
stress), and the stated point on the hole's wall within 1 mm of the net section.

The fixture's strengths are equal along and across the layers (50 MPa in tension and compression), so Tsai-Wu gives a
uniaxial stress the ratio Xt / stress. Solved through the MCP server's embedded engine, as an agent would; the
workspace is deleted when every check passes and kept, with its path printed, when one fails.

    make openphysics-mcp && python3 tools/printhole.py [--sizes 0.5,0.25] [--tet | --auto] [--keep]

--tet meshes TET10 at those surface sizes; --auto meshes voxels and lets the engine choose the mesh for the printed
answer (GOALS.md G17 step 9); without either, the voxels are solved as meshed (step 8's criterion).
"""
import json
import math
import shutil
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from mcptest import Client  # noqa: E402
from breaks import arc, extrude_stl  # noqa: E402

W, D, L, T = 40.0, 10.0, 120.0, 2.0  # mm
SIGMA = 10.0  # MPa, the traction on the far end (gross section)
XT = 50.0  # MPa, the fixture's strength
E_MPA = 2400.0

PRINTED = {"material": {
    "id": "printed_iso_nu0", "model": "orthotropic",
    "elastic": {"E1": "%g MPa" % E_MPA, "E2": "%g MPa" % E_MPA, "E3": "%g MPa" % E_MPA, "nu12": 0.0, "nu13": 0.0, "nu23": 0.0,
                "G12": "%g MPa" % (E_MPA / 2), "G23": "%g MPa" % (E_MPA / 2), "G13": "%g MPa" % (E_MPA / 2), "source": "assumed"},
    "strength": {"Xt": "%g MPa" % XT, "Xc": "%g MPa" % XT, "Yt": "%g MPa" % XT, "Yc": "%g MPa" % XT, "Zt": "%g MPa" % XT, "Zc": "%g MPa" % XT,
                 "S12": "30 MPa", "S23": "30 MPa", "S31": "30 MPa", "source": "assumed"},
    "note": "printstrengthtest's isotropic verification fixture with Poisson's ratio 0 (printhole.py), not data for any material"}}

results = []


def check(ok, what):
    results.append(bool(ok))
    print(("  ok   " if ok else "  FAIL ") + what, flush=True)
    return ok


def quarter_plate(path):
    """x from 0 to L/2, y from 0 to W/2, less the quarter hole of radius D/2 at the origin (1-degree facets), T thick"""
    r = D / 2
    poly = [(r, 0.0), (L / 2, 0.0), (L / 2, W / 2), (0.0, W / 2), (0.0, r)]
    poly += arc(0.0, 0.0, r, 90, 0, 90)[1:-1]
    extrude_stl(path, poly, T, "printhole: a quarter plate with a hole")


def solve(c, ws, size):
    def call(op, args):
        r = c.call(op, args)["result"]["structuredContent"]
        if not r.get("ok"):
            raise RuntimeError("%s: %s" % (op, json.dumps(r.get("error"))[:500]))
        return r.get("value", {})
    name = "hole_%s" % ("%g" % size).replace(".", "p")
    call("project_create", {"name": name, "description": "printhole.py: a hole's peak in the printed answer on voxels (GOALS.md G17 step 8)"})
    body = call("geometry_import", {"path": str(ws.parent / "quarter.stl"), "units": "mm", "name": "plate"})["body"]
    t = body["placement"]["translation_mm"]
    call("material_define", {"material": {"id": "iso_nu0", "name": "isotropic verification material, nu = 0", "family": "polymer",
                                          "status": "user_supplied", "provenance": "printhole.py: verification values, not data for any material",
                                          "youngs_modulus_pa": {"value": E_MPA * 1e6}, "poisson_ratio": {"value": 0.0},
                                          "density_kg_m3": {"value": 1240}}})
    call("material_assign", {"body": "plate", "material": "iso_nu0", "source": "user"})
    call("mech_material_define", PRINTED)
    t0 = time.time()
    tet = "--tet" in sys.argv  # TET10 at this surface size instead of voxels (a measurement; the criterion is the voxel one)
    auto = "--auto" in sys.argv  # voxels of this size, solved on the mesh the engine chooses (GOALS.md G17 step 9)
    mesh = call("mesh_generate", {"method": "tet", "surface_size": "%g mm" % size, "order": 2} if tet else
                {"element_size": "%g mm" % size, "max_elements": 400000})

    def plane(axis, at):  # a face on a plane of the part's own frame, moved to the build frame
        k = "xyz".index(axis)
        return {"plane": {"axis": axis, "at": at + t[k]}}

    for nm, q in (("sym_x", plane("x", 0.0)), ("sym_y", plane("y", 0.0)), ("bottom", plane("z", 0.0)), ("end", plane("x", L / 2))):
        call("selection_create", {"name": nm, "query": q, "source": "user"})
    for nm in ("sym_x", "sym_y", "bottom"):
        call("boundary_apply", {"name": nm + "_bc", "kind": "frictionless_support", "selection": nm, "source": "user"})
    call("boundary_apply", {"name": "pull", "kind": "traction", "selection": "end", "traction": ["%g MPa" % SIGMA, 0, 0], "source": "user"})
    model = {"type": "orthotropic", "material": "printed_iso_nu0", "build_direction": "z", "raster_reference": "x", "criterion": "tsai_wu"}
    if not tet and not auto:
        model["mesh"] = "as_meshed"  # step 8 studies the voxels themselves
    run = call("analysis_run", {"label": name, "material_model": model})
    job = run["job_id"]
    st = {}
    for _ in range(60):
        st = call("job_status", {"job_id": job, "wait_seconds": 10})
        if st.get("state") not in ("queued", "running"):
            break
    if st.get("state") != "succeeded":
        raise RuntimeError("%s did not succeed: %s" % (name, json.dumps(st)[:600]))
    summary = next((json.loads(p.read_text()) for p in (ws / name).rglob("summary.json")), None)
    s = (summary or {}).get("strength") or {}
    at = s.get("stated_location_mm") or [float("nan")] * 3
    return {"elements": mesh.get("mesh", {}).get("elements"), "seconds": time.time() - t0, "R": s.get("stated_strength_ratio"),
            "at": [at[k] - t[k] for k in range(3)], "mode": s.get("stated_governing_mode"), "point_R": s.get("smallest_strength_ratio"),
            "peak_R": s.get("peak_surface_strength_ratio"), "flag": s.get("peak_flag"), "stair": s.get("stated_near_staircase"),
            "tet_hint": "TET10" in ((s.get("answer") or {}).get("text") or ""), "choice": run.get("mesh_choice")}


def main():
    sizes = [float(x) for x in sys.argv[sys.argv.index("--sizes") + 1].split(",")] if "--sizes" in sys.argv else [0.5, 0.25]
    tmp = Path(tempfile.mkdtemp(prefix="openphysics-printhole-"))
    ws = tmp / "projects"
    ws.mkdir()
    quarter_plate(tmp / "quarter.stl")
    net = SIGMA * W / (W - D)
    kt = 2 + (1 - D / W) ** 3
    r_cf = XT / (kt * net)
    print("== G17 step 8 (c): a hole's peak in the printed answer on voxels")
    print("  Heywood: Kt %.4f on the net section, peak %.4f MPa, R = Xt / peak = %.5f" % (kt, kt * net, r_cf), flush=True)
    c = Client(["--embedded", "--workspace", str(ws), "--allow-read", str(tmp)])
    c.initialize()
    try:
        for h in sizes:
            r = solve(c, ws, h)
            x, y, z = r["at"]
            off = math.hypot(x, y - D / 2)  # from the hole's wall at the net section, (0, d/2) in the part's frame
            print("  h %-5g %7s elements  R %.5g (%+.1f %% from Heywood; Gauss-point minimum %.4g, peak at the surface %.4g, flag %s), at (%.2f, "
                  "%.2f, %.2f) mm in the part's frame; at a voxel staircase: %s, the answer names TET10: %s  [%.1f s]" % (
                      h, r["elements"], r["R"] or float("nan"), 100 * ((r["R"] or float("nan")) / r_cf - 1), r["point_R"] or float("nan"),
                      r["peak_R"] or float("nan"), r["flag"], x, y, z, {True: "yes", False: "no", None: "not reported"}[r["stair"]],
                      "yes" if r["tet_hint"] else "no", r["seconds"]), flush=True)
            if r["choice"]:
                print("           solved on %s at %g mm, %s elements (mesh_choice, GOALS.md G17 step 9)" % (
                    r["choice"].get("solved_on"), r["choice"].get("surface_size_mm", 0), r["choice"].get("elements")), flush=True)
            check(r["R"] is not None and abs(r["R"] / r_cf - 1) <= 0.05, "h %g: the stated ratio within 5 %% of Heywood's (%+.1f %%)" % (
                h, 100 * ((r["R"] or float("nan")) / r_cf - 1)))
            check(off <= 1.0, "h %g: the stated point %.2f mm from the hole's wall at the net section (criterion 1 mm)" % (h, off))
    finally:
        c.close()
    ok = all(results)
    print("PRINT HOLE: %d passed, %d failed" % (sum(results), len(results) - sum(results)))
    if ok and "--keep" not in sys.argv:
        shutil.rmtree(tmp)
    else:
        print("  workspace kept: %s" % tmp)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
