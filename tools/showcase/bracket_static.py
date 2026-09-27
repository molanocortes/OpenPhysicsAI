#!/usr/bin/env python3
"""bracket_static.py - showcase D, solid mechanics: where an L-bracket carries its load.

    python3 tools/showcase/bracket_static.py [--out docs/media]

The bracket of tools/tetcompare.py (base 60 x 6 mm, upright 6 x 40 mm, 30 mm deep, a 3 mm fillet in the inside corner),
on a conforming mesh of 10-node tetrahedra. Prescribed: the underside of the base fixed, 500 N pushing the top of the
upright sideways. Material: the library's published 316L record. Computed: the static displacement and the von Mises
stress. One still: the stress on the deformed shape, the undeformed outline behind it, the deformation scale stated.
The colour scale ends at the 99th percentile of the nodal stress, which is stated on the picture: the largest nodal value
sits at the fixed edge, where a rigid support makes the stress singular, and would otherwise own the scale.
"""
import json, sys, tempfile, shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE)); sys.path.insert(0, str(ROOT / "tools"))
from showcase import Engine, compose  # noqa: E402
from tetcompare import filleted_bracket  # noqa: E402

FORCE_N, SCALE = 500.0, 40


def main():
    out = Path(sys.argv[sys.argv.index("--out") + 1]) if "--out" in sys.argv else ROOT / "docs/media"
    tmp = Path(tempfile.mkdtemp(prefix="nvshow_bracket"))
    stl = tmp / "bracket.stl"
    filleted_bracket(stl)
    e = Engine(tmp / "ws", allow_read=[tmp])
    e.op("project_create", {"name": "bracket", "description": "an L-bracket pushed sideways at the top of its upright"})
    e.op("geometry_import", {"path": str(stl), "units": "mm", "name": "bracket"})
    e.op("material_assign", {"body": "bracket", "material": "ss316l_lpbf", "source": "user"})
    mesh, _ = e.op("mesh_generate", {"method": "tet", "surface_size": "1.5 mm", "order": 2})
    mesh = mesh.get("mesh", mesh)
    e.op("selection_create", {"name": "base_bottom", "query": {"plane": {"axis": "z", "at": "min"}}, "source": "user"})
    e.op("selection_create", {"name": "upright_top", "query": {"extreme": {"direction": "+z"}}, "source": "user"})
    e.op("boundary_apply", {"name": "bolted", "kind": "fixed", "selection": "base_bottom", "source": "user"})
    e.op("boundary_apply", {"name": "push", "kind": "force", "selection": "upright_top", "force": [f"{FORCE_N} N", 0, 0], "source": "user"})
    job, _ = e.op("analysis_run", {"label": "showcase bracket"})
    st = e.wait(job["job_id"])
    qs, _ = e.op("results_query", {"job_id": job["job_id"], "quantity": "von_mises"})
    qd, _ = e.op("results_query", {"job_id": job["job_id"], "quantity": "displacement"})
    stats = qs["statistics"]
    p99 = (stats.get("percentiles") or {}).get("p99") or stats.get("p99")
    hi = float(int(p99 / 5) * 5 + 5) if p99 else float(int(stats["max"]))
    v, im = e.picture("results_render", {"job_id": job["job_id"], "quantity": "von_mises", "colormap": "viridis", "range": [0, hi],
                                         "deformation_scale": SCALE, "show_undeformed": True, "camera": {"preset": "iso"}})
    pic = compose(im, "Solid mechanics: the load path in a bracket", f"base fixed   ·   {FORCE_N:g} N sideways at the top of the upright", "Von Mises stress", "MPa", 0, hi, "viridis",
                  f"Deformation ×{SCALE}, undeformed outline behind. Colour scale ends at the 99th percentile; the fixed edge is singular.", job["job_id"])
    out.mkdir(parents=True, exist_ok=True)
    pic.save(out / "showcase-solid.png", optimize=True)
    rec = {"job_id": job["job_id"], "mesh": {"method": "tet", "order": 2, "elements": mesh.get("elements"), "nodes": mesh.get("nodes")},
           "prescribed": {"force_n": FORCE_N, "fixed": "underside of the base"}, "material": "ss316l_lpbf (published record)",
           "computed": {"max_displacement_mm": qd["statistics"]["max"], "von_mises_p99_mpa": p99, "von_mises_max_mpa": stats["max"]},
           "colour_range_mpa": [0, hi], "deformation_scale": SCALE, "equilibrium": (st.get("summary") or {}).get("equilibrium")}
    (out / "showcase-solid.json").write_text(json.dumps(rec, indent=1))
    print(json.dumps(rec)[:700])
    shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
