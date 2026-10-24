#!/usr/bin/env python3
"""The reference entry on flag flow-cylinder-shedding: this repository's lattice Boltzmann solver (src/lbm.c through
src/lab/flow), two-dimensional, cylinder of 16 cells, tunnel 24 diameters wide with far-field walls, 150 convective
times; the Strouhal number from the zero crossings of the lift over the second half. Reads no answer."""
import json, os, pathlib, subprocess, sys
ROOT = pathlib.Path(__file__).resolve().parents[2]
WORK = ROOT / 'build' / 'flags' / 'flow-cylinder-shedding'
D = int(os.environ.get('LAB_BODY_CELLS', '16'))
preds = {}
WORK.mkdir(parents=True, exist_ok=True)
for c in json.loads(os.environ['FLAG_CASES']):
    sc = {'domain': 'flow', 'title': 'cylinder at Re %g (flag flow-cylinder-shedding)' % c['inputs']['reynolds'],
          'reynolds': c['inputs']['reynolds'], 'shape': 'cylinder', 'body_cells': D, 'lattice_velocity': 0.08,
          'tunnel': {'width_d': 24, 'length_d': 30, 'upstream_d': 8}, 'run': {'convective_times': 150, 'frames': 1}}
    p = WORK / ('%s.json' % c['case'])
    p.write_text(json.dumps(sc))
    r = subprocess.run([str(ROOT / 'build' / 'labrun'), str(p), str(WORK / 'run.lab'), '--quiet'], capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stderr)
    d = json.loads(r.stdout.strip().splitlines()[-1])['diagnostics']
    print('%s: %s' % (c['case'], json.dumps(d)), file=sys.stderr)
    preds[c['case']] = {'strouhal': d['strouhal']}
print(json.dumps(preds))
