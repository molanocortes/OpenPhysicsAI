#!/usr/bin/env python3
"""The reference entry on flag gas-shock-bubble: this repository's compressible solver (src/lab/gas).

For each case (FLAG_CASES, set by tools/flags.py): a planar run of half the shock tube (the axis is a symmetry plane),
the cylinder given by the case's gamma and gas constant at the pressure and temperature of the air ahead of the shock.
Frames every 4 microseconds; along the axis, the cylinder is where gamma differs from air's, and a shock front is the
first point, coming from downstream, where the pressure exceeds the ambient by 10 per cent. The speeds are straight-line
fits of positions against time, the way an x-t diagram is read:

  v_refracted           the front inside the cylinder, while it crosses the middle 60 per cent of the diameter
  v_transmitted         the shock born where the refracted front reaches the cylinder's downstream edge, followed frame by
                        frame (the nearest front ahead of its last position), over the first diameter behind the cylinder
  v_upstream_interface  the cylinder's upstream edge, over its first 0.25 diameters of travel

Nothing here reads an answer. Environment: LAB_LEVEL (default 3) sets the finest level; LAB_KEEP=1 keeps results.
"""
import json
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
D = 0.050                      # cylinder diameter, m
HALF = 0.0445                  # half the tube's width, m
P0, RHO0, G_AIR, R_AIR = 101325.0, 1.20, 1.4, 287.05
MS = 1.22
X_SHOCK, X_C = 0.020, 0.060    # initial shock position and cylinder centre, m
LEVEL = int(os.environ.get('LAB_LEVEL', '3'))
WORK = ROOT / 'build' / 'flags' / 'gas-shock-bubble'
END, FRAMES = 400e-6, 100


def post_shock(M, rho1, p1, g=1.4):
    c1 = (g * p1 / rho1) ** 0.5
    p2 = p1 * (2 * g * M * M - (g - 1)) / (g + 1)
    rho2 = rho1 * (g + 1) * M * M / ((g - 1) * M * M + 2)
    u2 = M * c1 * (1 - rho1 / rho2)
    return {'density_kg_m3': rho2, 'velocity_m_s': [u2, 0.0], 'pressure_pa': p2, 'gamma': g}


def scenario(gamma_b, R_b):
    T0 = P0 / (RHO0 * R_AIR)
    air = {'density_kg_m3': RHO0, 'velocity_m_s': [0, 0], 'pressure_pa': P0, 'gamma': G_AIR}
    gas = {'density_kg_m3': P0 / (R_b * T0), 'velocity_m_s': [0, 0], 'pressure_pa': P0, 'gamma': gamma_b}
    ps = post_shock(MS, RHO0, P0)
    return {
        'domain': 'gas', 'title': 'shock-bubble interaction (flag gas-shock-bubble)', 'geometry': 'planar', 'gas_constant_j_kgk': R_AIR,
        'grid': {'x0_m': 0.0, 'y0_m': 0.0, 'length_m': 8 * HALF, 'root_blocks': [16, 2], 'max_level': LEVEL},
        'boundaries': {'x_low': 'inflow', 'x_high': 'outflow', 'y_low': 'wall', 'y_high': 'wall'},
        'initial': air, 'freestream': ps,
        'regions': [{'box_m': [-1, -1, X_SHOCK, 1], 'state': ps}, {'circle_m': [X_C, 0.0, D / 2], 'state': gas}],
        'run': {'end_time_s': END, 'frames': FRAMES, 'fields': 'p,gamma', 'cfl': 0.4},
    }


def probe(lab, field, frame, n=3000):
    out = subprocess.run([str(ROOT / 'build' / 'labprobe'), str(lab), '--field', field, '--frame', str(frame), '--line', '0', '2e-5',
                          str(8 * HALF), '2e-5', str(n)], capture_output=True, text=True, check=True).stdout
    xs, vs = [], []
    for line in out.splitlines():
        if line.startswith('#'):
            continue
        x, _, v = line.split()
        if v != 'nan':
            xs.append(float(x))
            vs.append(float(v))
    return xs, vs


def fit(ts, xs):
    n = len(ts)
    if n < 3:
        return None
    mt, mx = sum(ts) / n, sum(xs) / n
    den = sum((t - mt) ** 2 for t in ts)
    return sum((t - mt) * (x - mx) for t, x in zip(ts, xs)) / den if den > 0 else None


def measure(lab, gamma_b):
    x_ui0, x_di0 = X_C - D / 2, X_C + D / 2
    thr_g = 0.5 * abs(gamma_b - G_AIR)
    rec = []  # (t, x_ui, x_di, x_front_inside, fronts behind the cylinder)
    for f in range(FRAMES + 1):
        t = END * f / FRAMES
        xs, gs = probe(lab, 'gamma', f)
        inside = [x for x, g in zip(xs, gs) if abs(g - G_AIR) > thr_g]
        if not inside:
            continue
        x_ui, x_di = min(inside), max(inside)
        xs, ps = probe(lab, 'p', f)
        hot = 1.10 * P0
        # the front inside the cylinder: scanning from its downstream edge upstream
        x_in = next((x for x, p in zip(reversed(xs), reversed(ps)) if x_ui <= x <= x_di and p > hot), None)
        # downstream-facing shock fronts behind the cylinder: local extremes of the pressure drop, steeper than 5 % of the
        # ambient pressure per millimetre
        fronts = []
        for k in range(1, len(xs) - 1):
            if xs[k] <= x_di + 1e-3:
                continue
            g = (ps[k + 1] - ps[k - 1]) / (xs[k + 1] - xs[k - 1])
            if g < -0.05 * P0 / 1e-3 and (not fronts or xs[k] - fronts[-1][0] > 2e-3):
                fronts.append((xs[k], g))
            elif fronts and xs[k] - fronts[-1][0] <= 2e-3 and g < fronts[-1][1]:
                fronts[-1] = (xs[k], g)
        rec.append((t, x_ui, x_di, x_in, [f[0] for f in fronts]))
    # refracted shock: the front inside, while it crosses the middle 60 % of the initial diameter
    a, b = x_ui0 + 0.2 * D, x_ui0 + 0.8 * D
    pts = [(t, xi) for t, _, _, xi, _ in rec if xi is not None and a <= xi <= b]
    v_r = fit([p[0] for p in pts], [p[1] for p in pts])
    # transmitted shock: born where the refracted front reaches the cylinder's downstream edge; followed frame by frame as
    # the front ahead of its last position that is nearest to it, over the first diameter behind the cylinder
    t_reach = next((t for t, _, x_di, xi, _ in rec if xi is not None and xi >= x_di - 1e-3), None)
    ex = None
    if t_reach is not None:  # the birth: the first frame, from then on, with a front within 3 mm of the downstream edge
        ex = next(((t, min(x for x in fr if x <= x_di + 3e-3)) for t, _, x_di, _, fr in rec
                   if t >= t_reach and any(x <= x_di + 3e-3 for x in fr)), None)
    pts = []
    if ex:
        t_prev, x_prev = ex
        pts.append(ex)
        for t, _, _, _, fr in rec:
            if t <= ex[0]:
                continue
            ahead = [x for x in fr if x_prev - 0.5e-3 <= x <= x_prev + 1500.0 * (t - t_prev)]
            if not ahead:
                continue
            x = min(ahead)
            pts.append((t, x))
            t_prev, x_prev = t, x
            if x > x_di0 + D:
                break
    pts = [p for p in pts if p[1] <= x_di0 + D]
    v_t = fit([p[0] for p in pts], [p[1] for p in pts])

    # upstream interface: over its first quarter diameter of travel, once it has started to move
    pts = [(t, x_ui) for t, x_ui, _, _, _ in rec if x_ui0 + 0.01 * D <= x_ui <= x_ui0 + 0.25 * D]
    v_u = fit([p[0] for p in pts], [p[1] for p in pts])
    return {'v_refracted': v_r, 'v_transmitted': v_t, 'v_upstream_interface': v_u}


def main():
    cases = json.loads(os.environ['FLAG_CASES'])
    WORK.mkdir(parents=True, exist_ok=True)
    preds = {}
    for c in cases:
        g, R = c['inputs']['gamma'], c['inputs']['gas_constant_j_kgk']
        sc, lab = WORK / ('%s.json' % c['case']), WORK / ('%s.lab' % c['case'])
        sc.write_text(json.dumps(scenario(g, R)))
        r = subprocess.run([str(ROOT / 'build' / 'labrun'), str(sc), str(lab), '--quiet'], capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit('labrun failed on %s: %s' % (c['case'], r.stderr[-500:]))
        m = measure(lab, g)
        preds[c['case']] = {k: (round(v, 2) if v is not None else None) for k, v in m.items()}
        print('%s: %s' % (c['case'], json.dumps(preds[c['case']])), file=sys.stderr)
        if os.environ.get('LAB_KEEP') != '1':
            lab.unlink()
    print(json.dumps(preds))


if __name__ == '__main__':
    main()
