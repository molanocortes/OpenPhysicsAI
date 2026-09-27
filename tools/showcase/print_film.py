#!/usr/bin/env python3
"""print_film.py - showcase E: a finished printing-process simulation replayed from its stored states.

    python3 tools/showcase/print_film.py polymer WORKSPACE PROJECT JOB_ID [--out docs/media]

Nothing is solved here. Each frame is one stored state of the run, drawn with the elements that exist at that state
(material activation, then the removal of supports), from one camera (the camera that frames the finished part) and
with one colour range for the whole film. The shape is drawn undeformed; the colour carries the field.

polymer: von Mises stress in the part, MPa, one range for the whole film (0 to the run's peak on the bed, rounded up).
         The supports are simulated and hidden (groups: part), so the part can be seen. The stored states of this
         run are the cooled states of each layer, so its temperature never exceeds the bed's and is not filmed.
         The run: demo/run_gcode_print.py on a slicer's job; record validation/prints/.

The metal film (showcase A) was removed on 2026-09-27 with the part it showed, for copyright; polymer is the one film.
"""
import json, math, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
from showcase import Engine, compose, write_film  # noqa: E402


def locked_camera(e, base, ti):
    v, _ = e.picture("results_render", dict(base, time_index=ti))
    c = v["camera"]
    return {"eye_mm": c["eye_mm"], "target_mm": c["target_mm"], "up": c["up"], "fov_deg": c["vertical_fov_deg"]}


def main():
    kind, ws, project, job = sys.argv[1:5]
    if kind != "polymer":
        sys.exit(f"print_film.py: no film named {kind!r}; the one film is 'polymer'")
    out = Path(sys.argv[sys.argv.index("--out") + 1]) if "--out" in sys.argv else ROOT / "docs/media"
    out.mkdir(parents=True, exist_ok=True)
    e = Engine(ws)
    e.op("project_open", {"path": str(Path(ws) / project)})
    st, _ = e.op("job_status", {"job_id": job})
    summ = st.get("summary") or {}
    frames, record = [], []
    proc = (summ.get("model") or {}).get("process") or {}
    res = summ.get("results") or {}
    q_last, _ = e.op("results_query", {"job_id": job, "quantity": "von_mises"})
    last = q_last["time_index"]
    hi = float(math.ceil(res.get("peak_von_mises_on_bed_mpa", q_last["statistics"]["max"])))
    base = {"job_id": job, "quantity": "von_mises", "colormap": "viridis", "range": [0, hi], "groups": ["part"], "camera": {"preset": "iso"}}
    cam = locked_camera(e, base, last)
    for ti in list(range(1, last - 1, 3)) + [last - 1, last - 1, last, last]:
        v, im = e.picture("results_render", dict(base, camera=cam, time_index=ti))
        state = (f"t = {v['time_s'] / 60:5.1f} min   ·   deposition and cooling, layer by layer" if ti < last - 1 else
                 "print finished and cooled   ·   still on the bed" if ti == last - 1 else "released from the bed   ·   supports removed")
        frames.append(compose(im, "Polymer printing: stress in a drone frame", state, "Von Mises stress", "MPa", 0, hi, "viridis",
                              f"PLA, process read from the slicer's job file (nozzle {proc.get('nozzle_c', 0):g} °C, bed {proc.get('bed_c', 0):g} °C). Supports are simulated and hidden here.", job))
        record.append({"time_index": ti, "time_s": v["time_s"], "max_mpa": v["max_value"]})
    name = "showcase-polymer-print"
    rec = {"job_id": job, "field": "von Mises stress in the part, MPa", "colour_range_mpa": [0, hi],
           "peak_on_bed_mpa": res.get("peak_von_mises_on_bed_mpa"), "peak_released_mpa": res.get("peak_von_mises_released_mpa"),
           "warp_z_mm": [res.get("warp_z_min_mm"), res.get("warp_z_max_mm")], "frames": record}
    size = write_film(frames, out / f"{name}.gif", ms=110, start=len(frames) - 1, cmap="viridis")
    rec["gif_bytes"] = size
    (out / f"{name}.json").write_text(json.dumps(rec, indent=1))
    print(json.dumps({k: v for k, v in rec.items() if k != "frames"}), "frames", len(frames))


if __name__ == "__main__":
    main()
