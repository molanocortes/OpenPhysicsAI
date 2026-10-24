#!/usr/bin/env python3
"""The reference entry on flag orbit-planets-2020: this repository's N-body solver (src/lab/orbit), Gauss-Legendre with
6 stages, half-day steps, the 1PN term around the Sun, on the bodies of inputs.json and nothing else.

For each case: run from the epoch to days_after_epoch, take the planet's position minus the Sun's, its longitude in the
ecliptic frame of the inputs. Prints one JSON line of predictions. Reads no answer.
"""
import json
import math
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
HERE = pathlib.Path(__file__).resolve().parent
WORK = ROOT / 'build' / 'flags' / 'orbit-planets-2020'


def main():
    cases = json.loads(os.environ['FLAG_CASES'])
    inp = json.loads((HERE / 'inputs.json').read_text())
    WORK.mkdir(parents=True, exist_ok=True)
    preds = {}
    for c in cases:
        days = c['inputs']['days_after_epoch']
        sc = {'domain': 'orbit', 'title': 'flag orbit-planets-2020, %s' % c['case'], 'relativity': True, 'stages': 6, 'step_days': 0.5,
              'bodies': inp['bodies'], 'run': {'end_days': days, 'frames': 1, 'paths': False}}
        p = WORK / ('%s.json' % c['case'])
        p.write_text(json.dumps(sc))
        r = subprocess.run([str(ROOT / 'build' / 'labrun'), str(p), str(WORK / 'run.lab'), '--quiet'], capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stderr)
        fin = {b['name']: b['position_km'] for b in json.loads(r.stdout.strip().splitlines()[-1])['diagnostics']['final']}
        x, s = fin[c['inputs']['body']], fin['sun']
        preds[c['case']] = {'longitude_deg': math.degrees(math.atan2(x[1] - s[1], x[0] - s[0])) % 360}
    print(json.dumps(preds))


if __name__ == '__main__':
    main()
