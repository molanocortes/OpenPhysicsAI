#!/usr/bin/env python3
"""Billig's correlations for a sphere (J. Spacecraft Rockets 4, 1967): standoff Delta / R = 0.143 exp(3.24 / M^2),
shock radius of curvature Rc / R = 1.143 exp(0.54 / (M - 1)^1.2), and the hyperbolic shape
x = Rc cot^2(mu) [ sqrt(1 + y^2 tan^2(mu) / Rc^2) - 1 ] measured from the shock's nose, mu the Mach angle."""
import json, math, os
out = {}
for c in json.loads(os.environ['FLAG_CASES']):
    M = c['inputs']['mach']
    R = 0.5  # in units of d
    delta = R * 0.143 * math.exp(3.24 / M ** 2)
    Rc = R * 1.143 * math.exp(0.54 / (M - 1) ** 1.2)
    mu = math.asin(1 / M)
    def x(y):
        return Rc / math.tan(mu) ** 2 * (math.sqrt(1 + y * y * math.tan(mu) ** 2 / (Rc * Rc)) - 1)
    out[c['case']] = {'standoff_center_over_d': R + delta, 'shape_x_over_d_at_y_0.4': x(0.4), 'shape_x_over_d_at_y_0.8': x(0.8)}
print(json.dumps(out))
