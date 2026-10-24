#!/usr/bin/env python3
"""topology.py - showcase F, design exploration: the design space and what the optimiser keeps of it.

    python3 tools/showcase/topology.py [--out docs/media]

Design space: a block 80 x 12 x 40 mm, 1 mm cells (38 400 elements), the library's published 316L record. Prescribed:
the face at x = 0 fixed, 1 kN downwards on the face at x = 80 mm. Objective and constraint: least compliance (the work
of the load, a measure of flexibility) with 30 % of the volume kept; density method with penalty 3, sensitivity filter
1.5 cells, the elements carrying the support and the load held solid. Shown: the cells whose density ends at or above
0.5. Optimisation iterations are not time, and the picture is two states, not a film.
The densities come back in element order (x fastest, then y, then z); the script asserts the mirror symmetry in y that
this load case must have, so a wrong mapping cannot produce a picture.
"""
import base64, json, struct, sys, tempfile, shutil
from pathlib import Path
from PIL import Image

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
from showcase import Engine, compose, PRESET, write_stl  # noqa: E402

NX, NY, NZ, VF, LOAD_N = 80, 12, 40, 0.30, 1000.0


def main():
    out = Path(sys.argv[sys.argv.index("--out") + 1]) if "--out" in sys.argv else ROOT / "docs/media"
    tmp = Path(tempfile.mkdtemp(prefix="nvshow_topo"))
    space = {(i, j, k) for i in range(NX) for j in range(NY) for k in range(NZ)}
    write_stl(tmp / "space.stl", space, 1.0, "design space")
    e = Engine(tmp / "ws", allow_read=[tmp])
    e.op("project_create", {"name": "design_space"})
    e.op("geometry_import", {"path": str(tmp / "space.stl"), "units": "mm", "name": "block"})
    e.op("material_assign", {"body": "block", "material": "ss316l_lpbf", "source": "user"})
    e.op("mesh_generate", {"element_size": "1 mm"})
    e.op("selection_create", {"name": "wall", "query": {"plane": {"axis": "x", "at": "min"}}, "source": "user"})
    e.op("selection_create", {"name": "free_end", "query": {"plane": {"axis": "x", "at": "max"}}, "source": "user"})
    e.op("boundary_apply", {"name": "fixed", "kind": "fixed", "selection": "wall", "source": "user"})
    e.op("boundary_apply", {"name": "load", "kind": "force", "selection": "free_end", "force": [0, 0, f"-{LOAD_N} N"], "source": "user"})
    w, h = PRESET["picture"][0] // 2, PRESET["picture"][1]
    v0, im0 = e.op("view_render", {"style": "showcase", "camera": {"preset": "iso"}, "width": w, "height": h, "show_build_plate": False, "label_patches": False})
    c = v0["camera"]
    cam = {"eye_mm": c["eye_mm"], "target_mm": c["target_mm"], "up": c["up"], "fov_deg": c["vertical_fov_deg"]}
    job, _ = e.op("topology_optimize", {"volume_fraction": VF, "max_iterations": 80, "passive_layers": 1, "label": "showcase design exploration"})
    e.wait(job["job_id"])
    r, _ = e.op("topology_result", {"job_id": job["job_id"], "include_density": True})
    raw = base64.b64decode(r["density_base64"])
    rho = struct.unpack(f"<{len(raw) // 4}f", raw)
    assert len(rho) == NX * NY * NZ, (len(rho), NX * NY * NZ)
    at = lambda i, j, k: rho[(k * NY + j) * NX + i]
    worst = max(abs(at(i, j, k) - at(i, NY - 1 - j, k)) for i in range(0, NX, 3) for j in range(NY // 2) for k in range(0, NZ, 3))
    assert worst < 0.02, f"the densities are not mirror symmetric in y ({worst:.3f}): the element order is not the one assumed"
    kept = {(i, j, k) for (i, j, k) in space if at(i, j, k) >= 0.5}
    write_stl(tmp / "result.stl", kept, 1.0, "optimised")
    e.op("project_create", {"name": "result"})
    e.op("geometry_import", {"path": str(tmp / "result.stl"), "units": "mm", "name": "kept"})
    _, im1 = e.op("view_render", {"style": "showcase", "camera": cam, "width": w, "height": h, "show_build_plate": False, "label_patches": False})
    both = Image.new("RGB", tuple(PRESET["picture"]))
    both.paste(im0, (0, 0)); both.paste(im1, (w, 0))
    c0, c1 = r["compliance_initial_j"], r["compliance_j"]
    pic = compose(both, "Design exploration: what a load path needs",
                  f"left: the design space   ·   right: the {100 * len(kept) / len(space):.0f} % of it the optimiser keeps",
                  note=f"Left face fixed, {LOAD_N / 1000:g} kN down on the right face. Compliance {c0:.3g} J to {c1:.3g} J at {100 * VF:.0f} % volume. Iterations are not time.",
                  run=job["job_id"])
    out.mkdir(parents=True, exist_ok=True)
    pic.save(out / "showcase-topology.png", optimize=True)
    rec = {"job_id": job["job_id"], "design_space_mm": [NX, NY, NZ], "elements": len(space), "volume_fraction": r["volume_fraction"], "kept_cells": len(kept),
           "compliance_initial_j": c0, "compliance_j": c1, "iterations": r["iterations"], "stop_reason": r["stop_reason"], "settings": r["settings"],
           "symmetry_check_max_difference": worst, "prescribed": {"load_n": LOAD_N, "fixed": "face x = 0"}, "material": "ss316l_lpbf (published record)"}
    (out / "showcase-topology.json").write_text(json.dumps(rec, indent=1))
    print(json.dumps({k: rec[k] for k in ("job_id", "kept_cells", "compliance_initial_j", "compliance_j", "iterations", "stop_reason", "symmetry_check_max_difference")}))
    shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
