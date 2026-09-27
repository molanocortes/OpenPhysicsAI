#!/usr/bin/env python3
"""Each planet on its own Kepler orbit about the Sun (mu = GM_sun + GM_planet), from its heliocentric state in
inputs.json; no perturbations, no relativity. Universal-variable propagation."""
import json, math, os, pathlib
inp = json.loads((pathlib.Path(__file__).resolve().parents[2] / 'orbit-planets-2020' / 'inputs.json').read_text())
B = {b['name']: b for b in inp['bodies']}

def stumpff(z):
    if z > 1e-8:
        s = math.sqrt(z); return (1 - math.cos(s)) / z, (s - math.sin(s)) / s ** 3
    if z < -1e-8:
        s = math.sqrt(-z); return (math.cosh(s) - 1) / -z, (math.sinh(s) - s) / s ** 3
    return 0.5, 1 / 6

def propagate(r0, v0, mu, t):
    R0 = math.sqrt(sum(x * x for x in r0)); V0 = math.sqrt(sum(x * x for x in v0))
    vr0 = sum(a * b for a, b in zip(r0, v0)) / R0; alpha = 2 / R0 - V0 * V0 / mu
    chi = math.sqrt(mu) * abs(alpha) * t
    for _ in range(200):
        z = alpha * chi * chi; C, S = stumpff(z)
        F = R0 * vr0 / math.sqrt(mu) * chi * chi * C + (1 - alpha * R0) * chi ** 3 * S + R0 * chi - math.sqrt(mu) * t
        dF = R0 * vr0 / math.sqrt(mu) * chi * (1 - alpha * chi * chi * S) + (1 - alpha * R0) * chi * chi * C + R0
        d = F / dF; chi -= d
        if abs(d) < 1e-12: break
    z = alpha * chi * chi; C, S = stumpff(z)
    f = 1 - chi * chi / R0 * C; g = t - chi ** 3 / math.sqrt(mu) * S
    return [f * a + g * b for a, b in zip(r0, v0)]

out = {}
sun = B['sun']
for c in json.loads(os.environ['FLAG_CASES']):
    p = B[c['inputs']['body']]
    r0 = [(a - b) * 1e3 for a, b in zip(p['position_km'], sun['position_km'])]
    v0 = [(a - b) * 1e3 for a, b in zip(p['velocity_km_s'], sun['velocity_km_s'])]
    mu = (sun['gm_km3_s2'] + p['gm_km3_s2']) * 1e9
    r = propagate(r0, v0, mu, c['inputs']['days_after_epoch'] * 86400)
    out[c['case']] = {'longitude_deg': math.degrees(math.atan2(r[1], r[0])) % 360}
print(json.dumps(out))
