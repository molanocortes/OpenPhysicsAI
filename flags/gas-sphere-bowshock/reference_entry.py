#!/usr/bin/env python3
"""The reference entry on flag gas-sphere-bowshock: this repository's compressible solver (src/lab/gas).

For each case (FLAG_CASES, set by tools/flags.py): an axisymmetric run of a sphere in a uniform stream at the given
Mach number, started impulsively and run until the shock has stopped moving; then the shock is located as the
steepest pressure rise along the axis and along the lines y = 0.4 d and 0.8 d. Prints one JSON line of predictions.

Nothing here reads an answer: the scenario is built from the case's Mach number and the flag's conditions only.
Environment: LAB_LEVEL (default 3) sets the finest level; LAB_KEEP=1 keeps the result files in build/flags/.
"""
import json
import math
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
D = 0.0127           # sphere diameter, m (the lengths asked for are over d, and the flow is inviscid)
T0 = 295.0           # stagnation temperature of the jet, K (it does not enter a length over d)
P = 101325.0         # the jet exhausts at atmospheric static pressure
GAMMA, R = 1.4, 287.05
LEVEL = int(os.environ.get('LAB_LEVEL', '3'))
WORK = ROOT / 'build' / 'flags' / 'gas-sphere-bowshock'


def scenario(mach, end_time, frames):
    T = T0 / (1 + 0.5 * (GAMMA - 1) * mach * mach)
    return {
        'domain': 'gas', 'title': 'sphere at Mach %.2f (flag gas-sphere-bowshock)' % mach,
        'geometry': 'axisymmetric', 'gas_constant_j_kgk': R,
        'grid': {'x0_m': -5 * D, 'y0_m': 0.0, 'length_m': 10 * D, 'root_blocks': [8, 4], 'max_level': LEVEL},
        'boundaries': {'x_low': 'inflow', 'x_high': 'outflow', 'y_low': 'axis', 'y_high': 'outflow'},
        'freestream': {'mach': mach, 'pressure_pa': P, 'temperature_k': T, 'gamma': GAMMA},
        'bodies': [{'circle_m': [0.0, 0.0, D / 2]}],
        'run': {'end_time_s': end_time, 'frames': frames, 'fields': 'p', 'cfl': 0.4},
    }


def probe(lab, y, x0, x1, n=4000, frame=-1):
    out = subprocess.run([str(ROOT / 'build' / 'labprobe'), str(lab), '--field', 'p', '--frame', str(frame), '--line', str(x0), str(y),
                          str(x1), str(y), str(n)], capture_output=True, text=True, check=True).stdout
    xs, ps = [], []
    for line in out.splitlines():
        if line.startswith('#'):
            continue
        x, _, p = line.split()
        if p != 'nan':
            xs.append(float(x))
            ps.append(float(p))
    return xs, ps


def shock_x(xs, ps):
    """The steepest pressure rise of the first compression met coming from upstream, refined by a parabola."""
    p_inf = ps[0]
    i_start = next(i for i, p in enumerate(ps) if p > p_inf * 1.01)
    # the rise ends where the pressure stops increasing by more than a trace
    i_end = i_start
    while i_end + 1 < len(ps) and ps[i_end + 1] > ps[i_end] - 1e-9 * p_inf and ps[i_end + 1] < 1e30:
        i_end += 1
        if ps[i_end] > p_inf * 1.02 and (ps[i_end] - ps[i_end - 1]) < 1e-4 * (ps[i_end] - p_inf) and i_end - i_start > 40:
            break
    lo = max(1, i_start - 20)
    g = [(ps[i + 1] - ps[i - 1]) for i in range(lo, min(i_end + 20, len(ps) - 1))]
    k = max(range(len(g)), key=lambda i: g[i])
    i = lo + k
    if 0 < k < len(g) - 1:
        a, b, c = g[k - 1], g[k], g[k + 1]
        den = a - 2 * b + c
        off = 0.5 * (a - c) / den if den != 0 else 0.0
    else:
        off = 0.0
    return xs[i] + off * (xs[1] - xs[0])


def measure(lab, frame=-1):
    xs, ps = probe(lab, 1e-5 * D, -5 * D, -0.5 * D, frame=frame)
    x_nose = shock_x(xs, ps)
    res = {'standoff_center_over_d': (0.0 - x_nose) / D}
    for Y in (0.4, 0.8):
        xs, ps = probe(lab, Y * D, -5 * D, 1.0 * D, frame=frame)
        res['shape_x_over_d_at_y_%.1f' % Y] = (shock_x(xs, ps) - x_nose) / D
    return res


def main():
    cases = json.loads(os.environ['FLAG_CASES'])
    WORK.mkdir(parents=True, exist_ok=True)
    preds, notes = {}, {}
    for c in cases:
        mach = c['inputs']['mach']
        u = mach * math.sqrt(GAMMA * R * T0 / (1 + 0.5 * (GAMMA - 1) * mach * mach))
        end = 60 * D / u  # sixty sphere diameters of travel: the shock has settled (checked below)
        sc = WORK / ('%s.json' % c['case'])
        lab = WORK / ('%s.lab' % c['case'])
        sc.write_text(json.dumps(scenario(mach, end, 6)))
        r = subprocess.run([str(ROOT / 'build' / 'labrun'), str(sc), str(lab), '--quiet'], capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit('labrun failed on %s: %s' % (c['case'], r.stderr[-500:]))
        m = measure(lab)
        before = measure(lab, frame=-2)  # ten diameters of travel earlier
        drift = abs(m['standoff_center_over_d'] - before['standoff_center_over_d'])
        preds[c['case']] = m
        notes[c['case']] = {'standoff_drift_last_10_diameters': round(drift, 5), 'run': json.loads(r.stdout.strip().splitlines()[-1])['wall_s']}
        print('%s: %s, drift %.4f' % (c['case'], json.dumps(m), drift), file=sys.stderr)
        if os.environ.get('LAB_KEEP') != '1':
            lab.unlink()
    print(json.dumps(preds))
    (WORK / 'notes.json').write_text(json.dumps(notes, indent=1))


if __name__ == '__main__':
    main()
