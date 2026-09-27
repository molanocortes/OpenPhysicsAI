# Print jobs replicated from a slicer's file

Up: [validation/QUEUE.md](../QUEUE.md). A result here comes from `demo/run_gcode_print.py`: the process and the support
settings are read from the gcode a slicer wrote (`tools/gcode_read.py`), the part from its STL. These are predictions
without a measurement until someone prints the part and measures it; the file says so in `what_this_is`.

| Result | Part | What it shows |
|---|---|---|
| [drone_frame_2026-09-20.json](drone_frame_2026-09-20.json) | a drone frame in PLA, OrcaSlicer, 252 layers, 3 h 53 min, tree supports | warp 0.66 mm at the feet, 15 MPa on the bed and 9 MPa after release; supports 11.1 cm3 against the slicer's 22.7 cm3; one connected piece only at 1 mm voxels; pictures `docs/images/drone-print-warp.png` and `drone-print-supports.png`, shown in the app by `tools/demo_print_result.nav` |
