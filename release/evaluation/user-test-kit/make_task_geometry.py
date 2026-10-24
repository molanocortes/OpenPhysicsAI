#!/usr/bin/env python3
"""make_task_geometry.py - the two plates of the user-test task, as closed binary STL in millimetres

    python3 make_task_geometry.py [OUTPUT_DIR]      (default: task_geometry/ next to this script)

Both plates are 120 mm long (x), 30 mm wide (y) and have the same volume (21,600 mm3):
  plate_uniform.stl   6 mm thick over the whole length
  plate_stepped.stl   8 mm thick for x 0-60 mm, 4 mm thick for x 60-120 mm (the step is on the top side)
The underside is the plane z = 0 for both. Every face is axis-aligned on a 2 mm lattice, so voxel meshes of 2, 1 and
0.5 mm represent the plates exactly. Uses the extrusion of examples/bracket_comparison/make_geometry.py.
"""
import importlib.util
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
candidates = [HERE.parents[2] / "examples/bracket_comparison/make_geometry.py", HERE.parents[1] / "examples/bracket_comparison/make_geometry.py"]
src = next((c for c in candidates if c.exists()), None)
if src is None:
    raise SystemExit("make_geometry.py of the bracket example not found next to this kit")
spec = importlib.util.spec_from_file_location("make_geometry", src)
mg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mg)
mg.S = 2.0  # lattice spacing of these plates, mm

PLATES = {
    "plate_uniform.stl": ([(0, 0), (60, 0), (60, 3), (0, 3)], 30, "task plate U: 120 x 30 x 6 mm"),
    "plate_stepped.stl": ([(0, 0), (60, 0), (60, 2), (30, 2), (30, 4), (0, 4)], 30, "task plate S: 8 mm then 4 mm thick"),
}

out = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "task_geometry"
out.mkdir(parents=True, exist_ok=True)
for name, (poly, width, label) in PLATES.items():
    tris = mg.extrude(poly, width)
    vol = mg.write_stl(out / name, tris, label)
    print(f"{name}: {len(tris)} triangles, volume {vol:.1f} mm3")
