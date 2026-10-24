#!/usr/bin/env python3
"""The reference entry on flag metal-printing: this repository's melt solver (src/lab/melt) in conduction, the method of
GOALS.md G20 step 2 (tools/g20_melt.py), which predicted these tracks blind on 2026-10-08 before NIST's values were read.

Each track runs in a half block on its plane of symmetry with 5 um cells (G20's refinement study), the beam a Gaussian of
1/e^2 radius half the stated D4 sigma diameter at the stated power, the plate at its stated temperature; the width and
depth are the mean of the transverse sections read by melt_track_section (melttest M11) between two and three pool
lengths behind the start. The alloys are in reference_materials.json with their sources. The absorptivity, 0.3085, is
G20's one free constant, fixed on the practice track amb2018-ammt-b (AM-Bench's public calibration case on the AMMT; the
CBM's, 91 um deep for 133 um wide, is a keyhole and was left out by King et al.'s shape rule). Reads no answer.

About 20 minutes for the eleven tracks and the two practice tracks on a laptop. LAB_MELT_CELL_UM overrides the cell."""
import json
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
WORK = ROOT / 'build' / 'flags' / 'metal-printing'
sys.path.insert(0, str(ROOT / 'tools'))
import g20_melt as g  # noqa: E402  (the method of G20 step 2: the block, the run and the reading of the sections)

ABSORPTIVITY = 0.3085
CELL = float(os.environ.get('LAB_MELT_CELL_UM', '5')) * 1e-6

r = subprocess.run(['make', '-s', 'build/g20melt'], cwd=ROOT, capture_output=True, text=True)
if r.returncode:
    sys.exit(r.stderr[-2000:])
g.RUNS = WORK / 'runs'
materials = json.loads((pathlib.Path(__file__).parent / 'reference_materials.json').read_text())['materials']
preds = {}
for c in json.loads(os.environ['FLAG_CASES']):
    i = c['inputs']
    track = {'id': c['case'], 'dataset': c['case'], 'laser_power_W': i['laser_power_w'],
             'scan_speed_mm_per_s': i['scan_speed_mm_per_s'], 'spot': {'D4sigma_diameter_um': i['spot_d4sigma_diameter_um']}}
    g.T0[c['case']] = float('%.2f' % (i['plate_temperature_c'] + 273.15))  # 298.15 and 296.65 K, as G20 states them
    o = g.solve(track, materials[i['alloy']], ABSORPTIVITY, CELL)
    preds[c['case']] = {'width': o['width_m'] * 1e6, 'depth': o['depth_m'] * 1e6}
    print('%s: width %.3f um, depth %.3f um, pool %.0f um long, %s' % (c['case'], o['width_m'] * 1e6, o['depth_m'] * 1e6,
                                                                       o['pool_length_at_end_m'] * 1e6, o['name']), file=sys.stderr, flush=True)
print(json.dumps(preds))
