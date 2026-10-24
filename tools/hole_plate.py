#!/usr/bin/env python3
"""hole_plate.py - GOALS.md G20, step 5's prerequisite (c): with the tet mesher's feature recovery, does a hole's edge
still carry a false peak where its curved wall meets the plate's flat face?

Criterion, written in GOALS.md before the first run (2026-10-08): a plate with a central hole in tension, meshed in
TET10, Poisson's ratio 0 so that the plane solution holds through the thickness: the largest von Mises stress anywhere,
the face edges of the hole included, within 3 % of Heywood's finite-width concentration on the net section,
Kt = 2 + (1 - d/W)^3.

The plate is a quarter (W = 40 mm, d = 10 mm, L = 120 mm, t = 4 mm) on its two symmetry planes (frictionless supports
through the hole's centre) and its lower face (frictionless: with nu = 0 nothing moves along z), pulled by a uniform
traction on its far end. Solved through the MCP server's embedded engine, as an agent would; the workspace is deleted
when the check passes.

    make openphysics-mcp && python3 tools/hole_plate.py [--size MM]
"""
import json
import shutil
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from mcptest import Client  # noqa: E402
from breaks import arc, extrude_stl  # noqa: E402

W, D, L, T = 40.0, 10.0, 120.0, 4.0  # mm
SIGMA = 10.0  # MPa, the traction on the far end (gross section)


def quarter_plate(path):
    """the quarter: x from 0 to L/2, y from 0 to W/2, minus the quarter hole of radius D/2 at the origin (1-degree facets)"""
    r = D / 2
    poly = [(r, 0.0), (L / 2, 0.0), (L / 2, W / 2), (0.0, W / 2), (0.0, r)]
    poly += arc(0.0, 0.0, r, 90, 0, 90)[1:-1]
    extrude_stl(path, poly, T, "hole_plate: a quarter plate with a hole")


def main():
    size = float(sys.argv[sys.argv.index("--size") + 1]) if "--size" in sys.argv else 0.5
    tmp = Path(tempfile.mkdtemp(prefix="openphysics-holeplate-", dir="/tmp"))
    stl = tmp / "quarter.stl"
    quarter_plate(stl)
    c = Client(["--embedded", "--workspace", str(tmp / "ws"), "--allow-read", str(tmp)])
    c.initialize()

    def call(op, args):
        r = c.call(op, args)["result"]["structuredContent"]
        if not r.get("ok"):
            raise SystemExit("%s: %s" % (op, json.dumps(r.get("error"))[:500]))
        return r.get("value", {})

    call("project_create", {"name": "hole_plate"})
    body = call("geometry_import", {"path": str(stl), "units": "mm", "name": "plate"})["body"]
    t = body["placement"]["translation_mm"]
    call("material_define", {"material": {"id": "iso_nu0", "name": "isotropic verification material, nu = 0", "family": "metal",
                                          "status": "user_supplied", "provenance": "hole_plate.py: verification values, not data for any material",
                                          "youngs_modulus_pa": {"value": 70e9}, "poisson_ratio": {"value": 0.0}, "density_kg_m3": {"value": 2700}}})
    call("material_assign", {"body": "plate", "material": "iso_nu0", "source": "user"})
    mesh = call("mesh_generate", {"method": "tet", "surface_size": "%g mm" % size, "order": 2})

    def plane(axis, at):  # a face on a plane of the part's own frame, moved to the build frame
        k = "xyz".index(axis)
        return {"plane": {"axis": axis, "at": at + t[k]}}

    for name, q in (("sym_x", plane("x", 0.0)), ("sym_y", plane("y", 0.0)), ("bottom", plane("z", 0.0)), ("end", plane("x", L / 2))):
        call("selection_create", {"name": name, "query": q, "source": "user"})
    for name in ("sym_x", "sym_y", "bottom"):
        call("boundary_apply", {"name": name + "_bc", "kind": "frictionless_support", "selection": name, "source": "user"})
    call("boundary_apply", {"name": "pull", "kind": "traction", "selection": "end", "traction": ["%g MPa" % SIGMA, 0, 0], "source": "user"})
    job = call("analysis_run", {"label": "hole plate"})["job_id"]
    st = {}
    for _ in range(120):
        st = call("job_status", {"job_id": job, "wait_seconds": 10})
        if st.get("state") not in ("queued", "running"):
            break
    ok = st.get("state") == "succeeded"
    vm = call("results_query", {"job_id": job, "quantity": "von_mises", "value_kind": "nodal_average", "top": 5}) if ok else {}
    scale = {"Pa": 1e-6, "kPa": 1e-3, "MPa": 1.0, "GPa": 1e3}.get(vm.get("unit"), float("nan"))
    top = vm.get("largest") or []
    smax = top[0]["value"] * scale if top else float("nan")
    where = ", ".join("%.2f" % (x - t[k]) for k, x in enumerate(top[0]["location_mm"])) if top else "nowhere"
    net = SIGMA * W / (W - D)
    kt = 2 + (1 - D / W) ** 3
    want = kt * net
    err = smax / want - 1
    print("== G20 step 5 (c): a hole's edge in TET10 with sharp edges kept")
    m = mesh.get("mesh", {})
    fe = m.get("sharp_edges", {})
    print("  mesh: %s elements, surface %g mm; sharp edges kept %s: nodes onto edges %s, onto corners %s, refused %s" % (
        m.get("elements"), size, fe.get("kept"), fe.get("nodes_on_edges"), fe.get("nodes_on_corners"), fe.get("moves_refused")))
    print("  largest von Mises %.4f MPa at (%s) mm in the part's frame; Heywood: Kt %.4f on the net section, %.4f MPa (%+.2f %%; criterion 3 %%)" % (
        smax, where, kt, want, 100 * err))
    passed = ok and abs(err) <= 0.03
    print("  %s" % ("ok" if passed else "FAIL"))
    if passed:
        shutil.rmtree(tmp)
    else:
        print("  workspace kept: %s" % tmp)
    print("\nHOLE PLATE: %d passed, %d failed" % (1 if passed else 0, 0 if passed else 1))
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
