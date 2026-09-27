#!/usr/bin/env python3
"""gcode_read.py - what a sliced print job says about itself, read from the gcode a slicer wrote (OrcaSlicer, Bambu
Studio and PrusaSlicer dialects: '; FEATURE:' or ';TYPE:' comments, relative extrusion, G1/G2/G3).

    python3 tools/gcode_read.py job.gcode              # the summary as JSON on stdout
    python3 tools/gcode_read.py job.gcode --out DIR    # also DIR/gcode_summary.json

It reads, never guesses: the process values the slicer embedded (layer height, temperatures, material, support
settings), and what the toolpath itself deposits, split into part and support: filament length and volume per feature,
the bounding box of each, the layer count, the printing time the slicer estimated and the mean deposition rate that
follows from volume over time. The print simulation (mech_print_run) takes its process from this, and the supports the
simulator generates are compared with the supports the slicer printed.
"""
import json, math, re, sys
from pathlib import Path

SUPPORT = {"support", "support interface", "support transition"}
SKIP = {"custom", "skirt", "brim", "prime tower", "wipe tower"}


def read(path):
    fil_d, settings, feat = 1.75, {}, "custom"
    x = y = z = 0.0
    rel_e, e_abs = True, 0.0
    per = {}  # feature -> [length of filament mm, path length mm]
    box = {"part": [[1e9] * 3, [-1e9] * 3], "support": [[1e9] * 3, [-1e9] * 3]}
    layers, zs_support = 0, []
    want = ("layer_height", "initial_layer_print_height", "nozzle_temperature", "hot_plate_temp", "textured_plate_temp",
            "filament_type", "filament_density", "filament_diameter", "support_threshold_angle", "support_type",
            "tree_support_branch_diameter", "tree_support_tip_diameter", "tree_support_branch_angle",
            "tree_support_branch_distance", "support_top_z_distance", "support_object_xy_distance",
            "sparse_infill_density", "sparse_infill_pattern", "wall_loops", "line_width", "nozzle_diameter",
            "enable_support", "printer_model", "filament_settings_id", "chamber_temperatures")
    head = {}
    for ln in open(path, errors="replace"):
        if ln.startswith(";"):
            s = ln[1:].strip()
            m = re.match(r"(?:FEATURE|TYPE):\s*(.+)", s)
            if m:
                feat = m.group(1).strip().lower()
                continue
            if s.startswith(("CHANGE_LAYER", "LAYER_CHANGE")):
                layers += 1
                continue
            m = re.match(r"([\w \[\]]+?)\s*[=:]\s*(.+)", s)
            if m:
                k, v = m.group(1).strip(), m.group(2).strip()
                if k in want:
                    settings[k] = v
                elif k in ("model printing time", "total estimated time", "total layer number", "max_z_height",
                           "filament used [cm3]", "filament used [g]", "filament used [mm]"):
                    head[k] = v.split(";")[0].strip()
                if k == "filament_diameter":
                    try: fil_d = float(v.split(",")[0])
                    except ValueError: pass
            continue
        c = ln.split(";", 1)[0].split()
        if not c:
            continue
        g = c[0]
        if g == "M83": rel_e = True
        elif g == "M82": rel_e = False
        elif g in ("G92",):
            for t in c[1:]:
                if t[0] == "E": e_abs = float(t[1:])
        elif g in ("G0", "G1", "G2", "G3"):
            nx, ny, nz, de, i, j = x, y, z, 0.0, 0.0, 0.0
            for t in c[1:]:
                a, v = t[0], t[1:]
                try: v = float(v)
                except ValueError: continue
                if a == "X": nx = v
                elif a == "Y": ny = v
                elif a == "Z": nz = v
                elif a == "I": i = v
                elif a == "J": j = v
                elif a == "E":
                    de = v if rel_e else v - e_abs
                    if not rel_e: e_abs = v
            if g in ("G2", "G3"):
                cx, cy = x + i, y + j
                r = math.hypot(i, j)
                a0, a1 = math.atan2(y - cy, x - cx), math.atan2(ny - cy, nx - cx)
                d = (a0 - a1) if g == "G2" else (a1 - a0)
                d %= 2 * math.pi
                length = r * d
            else:
                length = math.hypot(nx - x, ny - y)
            if de > 0 and length > 0 and feat not in SKIP:
                p = per.setdefault(feat, [0.0, 0.0])
                p[0] += de
                p[1] += length
                b = box["support" if feat in SUPPORT else "part"]
                for k, (lo, hi) in enumerate(((x, nx), (y, ny), (nz, nz))):
                    b[0][k] = min(b[0][k], lo, hi)
                    b[1][k] = max(b[1][k], lo, hi)
                if feat in SUPPORT: zs_support.append(nz)
            x, y, z = nx, ny, nz
    area = math.pi * fil_d ** 2 / 4
    vol = {f: round(v[0] * area / 1000, 3) for f, v in per.items()}  # cm3
    v_sup = sum(v for f, v in vol.items() if f in SUPPORT)
    v_part = sum(v for f, v in vol.items() if f not in SUPPORT)

    def secs(t):
        return sum(int(n) * {"d": 86400, "h": 3600, "m": 60, "s": 1}[u] for n, u in re.findall(r"(\d+)([dhms])", t or ""))

    t_model = secs(head.get("model printing time", ""))
    out = {
        "file": Path(path).name, "header": head, "settings": settings, "layers": layers,
        "volume_cm3": {"part": round(v_part, 3), "support": round(v_sup, 3), "by_feature": vol},
        "box_mm": {k: {"min": [round(a, 2) for a in b[0]], "max": [round(a, 2) for a in b[1]],
                       "size": [round(h - l, 2) for l, h in zip(b[0], b[1])]} for k, b in box.items() if b[0][0] < 1e8},
        "support_top_z_mm": round(max(zs_support), 2) if zs_support else None,
        "model_printing_time_s": t_model,
        "mean_deposition_rate_mm3_s": round(1000 * (v_part + v_sup) / t_model, 3) if t_model else None,
    }
    return out


if __name__ == "__main__":
    o = read(sys.argv[1])
    if "--out" in sys.argv:
        d = Path(sys.argv[sys.argv.index("--out") + 1]); d.mkdir(parents=True, exist_ok=True)
        (d / "gcode_summary.json").write_text(json.dumps(o, indent=1))
    print(json.dumps(o, indent=1))
