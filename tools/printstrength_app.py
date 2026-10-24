#!/usr/bin/env python3
"""printstrength_app.py - G17 step 1 d (GOALS.md): a printed part's failure index through the app and the headset.

Criterion, written in GOALS.md before the first run: a part set up by script in the app shows the "failure index"
field; the largest value on the drawn surface equals the job summary's within 1e-6 relative; the headset scene check
(tools/xr/xr_check.py) passes on it. The scene's own values (its _VALUE attribute, what the headset reads under the
pointer) are compared with the summary in the same way.

The part is the shipped sample bracket (an L-bracket of 20 triangles, the kind of coarse STL most parts arrive as),
printed flat, its foot held and 20 N pushing the top of its wall. The material is a verification fixture, not data for
any plastic. The window runs off-screen (--headless); the workspace is deleted when every check passes.

    make openphysics && python3 tools/printstrength_app.py [--shot window.png]
"""
import json
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APP = ROOT / "openphysics"
STL = ROOT / "samples" / "bracket.stl"

results = []


def check(ok, what):
    results.append(bool(ok))
    print(("  ok   " if ok else "  FAIL ") + what)
    return ok


ISO = {"material": {"id": "fixture_iso", "name": "isotropic verification fixture", "family": "polymer", "status": "user_supplied",
                    "provenance": "printstrength_app: E = 2.4 GPa, nu = 0.35, rho = 1240 kg/m3, verification values, not data for any material",
                    "youngs_modulus_pa": {"value": 2.4e9}, "poisson_ratio": {"value": 0.35}, "density_kg_m3": {"value": 1240}}}
PRINTED = {"material": {"id": "printed_ti", "model": "orthotropic",
                        "elastic": {"E1": "3200 MPa", "E2": "3200 MPa", "E3": "2600 MPa", "nu12": 0.35, "nu13": 0.32, "nu23": 0.32,
                                    "G12": "%.17g Pa" % (3.2e9 / 2.7), "G23": "1100 MPa", "G13": "1100 MPa", "source": "assumed"},
                        "strength": {"Xt": "50 MPa", "Xc": "60 MPa", "Yt": "50 MPa", "Yc": "60 MPa", "Zt": "30 MPa", "Zc": "55 MPa",
                                     "S12": "30 MPa", "S23": "25 MPa", "S31": "25 MPa", "source": "assumed"},
                        "note": "transversely isotropic verification fixture (axis 3 the build direction), not data for any material"}}


def am(op, params):
    text = json.dumps(params)
    assert "'" not in text, "the script quotes operation parameters with single quotes"
    return "am %s '%s'" % (op, text)


def glb_values(path):
    """the _VALUE attribute of every primitive of a GLB (what the headset reads under the pointer)"""
    b = path.read_bytes()
    jlen = struct.unpack_from("<I", b, 12)[0]
    g = json.loads(b[20:20 + jlen])
    blen = struct.unpack_from("<I", b, 20 + jlen)[0]
    binc = b[28 + jlen:28 + jlen + blen]
    vals = []
    for mesh in g.get("meshes", []):
        for p in mesh["primitives"]:
            a = p["attributes"].get("_VALUE")
            if a is None:
                continue
            acc = g["accessors"][a]
            view = g["bufferViews"][acc["bufferView"]]
            vals += struct.unpack_from("<%df" % acc["count"], binc, view.get("byteOffset", 0) + acc.get("byteOffset", 0))
    return vals, g


def main():
    shot = sys.argv[sys.argv.index("--shot") + 1] if "--shot" in sys.argv[:-1] else None  # a picture of the window, optional
    print("== d. the failure index in the app and the headset")
    if not APP.exists():
        print("  build the app first: make openphysics")
        return 2
    tmp = Path(tempfile.mkdtemp(prefix="openphysics-printstrength-app-"))
    ws, glb = tmp / "ws", tmp / "scene.glb"
    ws.mkdir()
    lines = ["workspace solid", "frames 4",
             am("project_create", {"name": "strength_app", "description": "G17 1d: the failure index through the app"}),
             am("geometry_import", {"path": "samples/bracket.stl", "units": "mm", "name": "bracket"}),
             am("material_define", ISO),
             am("material_assign", {"body": "bracket", "material": "fixture_iso", "source": "user"}),
             am("mech_material_define", PRINTED),
             am("mesh_generate", {"element_size": "0.5 mm"}),
             am("selection_create", {"name": "foot", "query": {"all": [{"facing": {"direction": "down"}}, {"plane": {"axis": "z", "at": "min"}}]},
                                     "source": "user"}),
             am("selection_create", {"name": "top", "query": {"extreme": {"direction": "+z"}}, "source": "user"}),
             am("boundary_apply", {"name": "hold", "kind": "fixed", "selection": "foot", "source": "user"}),
             am("boundary_apply", {"name": "push", "kind": "force", "selection": "top", "force": ["20 N", 0, 0], "source": "user"}),
             am("analysis_run", {"label": "printed bracket", "material_model": {"type": "orthotropic", "material": "printed_ti",
                                                                                  "build_direction": "z", "raster_reference": "x",
                                                                                  "criterion": "tsai_wu"}}),
             "frames 10", "fem follow", "solid wait 120", "frames 20",
             "fem field failure_index", "frames 8", "fem status",
             "fem deform 0", "frames 6"] + (["fem fit", "frames 10", "screenshot %s" % shot] if shot else []) + ["xr export %s" % glb,
             "echo PRINTSTRENGTH_APP done", "quit"]
    script = tmp / "strength.nav"
    script.write_text("\n".join(lines) + "\n")
    run = subprocess.run([str(APP), "--headless", "--size", "960x600", "--workspace", str(ws), "--exec", "exec %s" % script],
                         capture_output=True, text=True, timeout=600, cwd=str(ROOT))
    log = run.stdout + run.stderr
    (tmp / "app.log").write_text(log)
    check("PRINTSTRENGTH_APP done" in log, "the script ran to the end in the app")
    summaries = [json.loads(p.read_text()) for p in ws.rglob("summary.json")]
    strength = next((s["strength"] for s in summaries if "strength" in s), None)
    check(strength is not None and "largest_failure_index" in strength, "the job summary carries the strength result")
    want = strength["largest_failure_index"] if strength else float("nan")
    if strength:
        loc = strength.get("location_mm", [float("nan")] * 3)
        print("  job summary: largest failure index %.9g (%s, %s) at (%.2f, %.2f, %.2f) mm, strength ratio %.4g" %
              (want, strength.get("criterion"), strength.get("governing_mode"), loc[0], loc[1], loc[2], strength.get("smallest_strength_ratio")))
    m = re.search(r"drawn peak ([0-9.eE+-]+) .*\((\w+)\)", log)
    check(m is not None and m.group(2) == "failure_index", "the window draws the failure index field")
    if m:
        drawn = float(m.group(1))
        check(abs(drawn / want - 1) <= 1e-6, "the largest drawn value %.9g equals the summary's %.9g (%.2e relative)" % (drawn, want, abs(drawn / want - 1)))
    surf = re.search(r"result surface built for (\S+) \(([^)]*)\): (\d+) triangles", log)
    if surf:
        print("  surface built for %s: %s triangles" % (surf.group(1), surf.group(3)))
    if glb.exists():
        xr = subprocess.run([sys.executable, str(ROOT / "tools/xr/xr_check.py"), str(glb), "--stl", str(STL)], capture_output=True, text=True)
        check(xr.returncode == 0 and "XR SCENE CHECK PASSED" in xr.stdout, "the headset scene check passes on the exported scene")
        if xr.returncode:
            print(xr.stdout + xr.stderr)
        vals, g = glb_values(glb)
        res = ((g.get("nodes") or [{}])[0].get("extras") or {}).get("result") or {}
        finite = [v for v in vals if abs(v) < 1e30]
        check(res.get("field_name") == "failure_index", "the scene says it shows the failure index (%s)" % res.get("field_name"))
        if finite:
            check(abs(max(finite) / want - 1) <= 1e-6, "the scene's largest value %.9g equals the summary's (%.2e relative)" % (max(finite), abs(max(finite) / want - 1)))
    else:
        check(False, "the window exported the headset scene")
    ok = all(results)
    if ok:
        shutil.rmtree(tmp)
    else:
        print("  kept for diagnosis: %s (app.log, strength.nav, scene.glb)" % tmp)
    print("\nFAILURE INDEX THROUGH THE APP (G17 step 1 d): %d passed, %d failed" % (sum(results), len(results) - sum(results)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
