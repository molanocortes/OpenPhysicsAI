#!/usr/bin/env python3
"""heat_sink.py - showcase B, heat transfer: a printed steel heat sink heated through its pad, then left to cool.

    python3 tools/showcase/heat_sink.py [--out docs/media] [--keep]
    python3 tools/showcase/heat_sink.py --reuse WORKSPACE JOB_ID      # redraw a kept run without solving again

Prescribed inputs: 20 W into the 20 x 20 mm pad for 300 s (heat flux 5e4 W/m^2), then nothing for 300 s; convection
to air at 25 degC with a coefficient of 25 W/(m^2 K) on every other face; start at 25 degC. The convection coefficient
is prescribed, not computed: no air flow is solved here. Material: the library's published 316L record
(conductivity and heat capacity depend on temperature). Computed: the temperature field over 600 s, conduction only.
Geometry: base 60 x 60 x 6 mm, nine fins 2 mm thick and 28 mm tall, pad 20 x 20 x 2 mm under the centre; 1 mm cells.
The film keeps one colour range for all frames (the run's own minimum and maximum), so colours can be compared in time.
"""
import json, sys, tempfile, shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
from showcase import Engine, compose, write_film, write_stl  # noqa: E402

POWER_W, PAD_MM, T_ON, T_END, H_CONV, T_AIR, CELL = 20.0, 20, 300, 600, 25.0, 25.0, 1.0


def cells():
    c = set()
    for i in range(60):
        for j in range(60):
            for k in range(2, 8): c.add((i, j, k))                      # base, z 2..8
    for f in range(9):                                                   # nine fins along x, 2 mm thick, 7.25 mm pitch
        j0 = int(round(f * 7.25))
        for i in range(60):
            for j in range(j0, j0 + 2):
                for k in range(8, 36): c.add((i, j, k))
    for i in range(20, 40):
        for j in range(20, 40):
            for k in range(0, 2): c.add((i, j, k))                      # the pad the heat enters through
    return c


def main():
    out = Path(sys.argv[sys.argv.index("--out") + 1]) if "--out" in sys.argv else ROOT / "docs/media"
    reuse = sys.argv[sys.argv.index("--reuse") + 1:sys.argv.index("--reuse") + 3] if "--reuse" in sys.argv else None
    flux = POWER_W / (PAD_MM * 1e-3) ** 2
    times = list(range(15, T_END + 1, 15))
    if reuse:
        tmp = Path(reuse[0]).parent
        e = Engine(reuse[0])
        e.op("project_open", {"path": str(Path(reuse[0]) / "heat_sink")})
        job = {"job_id": reuse[1]}
        summ = e.op("job_status", {"job_id": reuse[1]})[0].get("summary") or {}
    else:
        tmp = Path(tempfile.mkdtemp(prefix="nvshow_heat"))
        stl = tmp / "heat_sink.stl"
        write_stl(stl, cells(), CELL, "showcase heat sink")
        e = Engine(tmp / "ws", allow_read=[tmp])
        e.op("project_create", {"name": "heat_sink", "description": "a printed 316L heat sink heated through its pad, then cooling in air"})
        e.op("geometry_import", {"path": str(stl), "units": "mm", "name": "sink"})
        e.op("material_assign", {"body": "sink", "material": "ss316l_lpbf", "source": "user"})
        e.op("mesh_generate", {"element_size": f"{CELL} mm"})
        e.op("selection_create", {"name": "pad", "query": {"plane": {"axis": "z", "at": "min"}}, "body": "sink"})
        e.op("selection_create", {"name": "air_side", "query": {"not": {"plane": {"axis": "z", "at": "min"}}}, "body": "sink"})
        e.op("boundary_apply", {"name": "chip", "kind": "heat_flux", "selection": "pad", "heat_flux": f"{flux:g} W/m^2", "source": "user",
                                "schedule": [{"time": "0 s", "factor": 1}, {"time": f"{T_ON} s", "factor": 1}, {"time": f"{T_ON + 1} s", "factor": 0}]})
        e.op("boundary_apply", {"name": "air", "kind": "convection", "selection": "air_side", "source": "user",
                                "convection": {"coefficient": H_CONV, "ambient": f"{T_AIR} degC"}})
        job, _ = e.op("analysis_run", {"analysis": "transient_thermal", "end_time": f"{T_END} s", "time_stepping": "adaptive",
                                       "initial_temperature": f"{T_AIR} degC", "output_times": [f"{t} s" for t in times]})
        e.op("project_save", {})
        summ = e.wait(job["job_id"]).get("summary") or {}
    q, _ = e.op("results_query", {"job_id": job["job_id"], "quantity": "temperature", "time_s": T_ON})
    peak = q["statistics"]["max"]
    lo, hi = T_AIR, 10 * (int(peak / 10) + 1)
    cam = {"eye_mm": [-165.0, -150.0, -62.0], "target_mm": [34.0, 30.0, 27.0], "up": [0, 0, 1], "fov_deg": 30}
    frames, record = [], []
    for t in [0] + times:
        args = {"job_id": job["job_id"], "quantity": "temperature", "colormap": "heat", "range": [lo, hi], "time_s": t,
                "camera": cam}
        v, im = e.picture("results_render", args)
        phase = f"heating, {POWER_W:g} W into the pad" if t <= T_ON else "power off, cooling in air"
        frames.append(compose(im, "Heat in a printed steel heat sink", f"t = {t:3d} s   ·   {phase}", "Temperature", "°C", lo, hi, "heat",
                              f"Prescribed: {POWER_W:g} W for {T_ON} s, convection {H_CONV:g} W/(m² K) to {T_AIR:g} °C air", job["job_id"]))
        record.append({"time_s": t, "max_c": v["max_value"]})
    out.mkdir(parents=True, exist_ok=True)
    size = write_film(frames, out / "showcase-heat.gif", ms=140, start=times.index(T_ON) + 1, cmap="heat")
    frames[times.index(T_ON) + 1].save(out / "showcase-heat-poster.png", optimize=True)   # the poster: the hottest state
    rec = {"job_id": job["job_id"], "prescribed": {"power_w": POWER_W, "pad_mm": PAD_MM, "flux_w_m2": flux, "on_s": T_ON, "end_s": T_END,
           "convection_w_m2k": H_CONV, "air_c": T_AIR}, "material": "ss316l_lpbf (published record)", "cell_mm": CELL,
           "elements": (summ.get("model") or {}).get("elements"), "computed": {"peak_c_at_power_off": peak, "frames": record},
           "colour_range_c": [lo, hi], "energy_balance": summ.get("energy_balance") or summ.get("heat_balance"), "gif_bytes": size}
    (out / "showcase-heat.json").write_text(json.dumps(rec, indent=1))
    print(json.dumps({k: rec[k] for k in ("job_id", "elements", "colour_range_c", "gif_bytes")}), "peak", round(peak, 1), "C; last frame max", round(record[-1]["max_c"], 1))
    e.close() if hasattr(e, "close") else None
    if "--keep" not in sys.argv and not reuse: shutil.rmtree(tmp, ignore_errors=True)
    else: print("kept", tmp)


if __name__ == "__main__":
    main()
