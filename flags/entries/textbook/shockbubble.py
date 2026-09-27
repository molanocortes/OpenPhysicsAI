#!/usr/bin/env python3
"""The textbook entry on flag gas-shock-bubble: one-dimensional gas dynamics, no simulation.

The cylinder is replaced by a slab. The incident shock meets the slab's upstream face: an exact Riemann problem between
the shocked air and the still gas (Toro, Riemann Solvers and Numerical Methods for Fluid Dynamics, chapter 4, with a
different ratio of specific heats on each side) gives the shock refracted into the gas and the speed of the face. The
refracted shock then meets the downstream face: a second Riemann problem between the shocked gas and still air gives
the transmitted shock. Curvature, focusing and the flow around the cylinder are ignored: this is the answer of the
back of an envelope.
"""
import json
import math
import os

P0, RHO0, G_AIR, R_AIR, MS = 101325.0, 1.20, 1.4, 287.05, 1.22


def f_side(p, rho, pk, g):
    """Toro's pressure function and its derivative for one side (shock if p > pk, rarefaction otherwise)."""
    c = math.sqrt(g * pk / rho)
    if p > pk:
        A, B = 2 / ((g + 1) * rho), (g - 1) / (g + 1) * pk
        q = math.sqrt(A / (p + B))
        return (p - pk) * q, q * (1 - 0.5 * (p - pk) / (p + B))
    r = (p / pk) ** ((g - 1) / (2 * g))
    return 2 * c / (g - 1) * (r - 1), 1 / (rho * c) * (p / pk) ** (-(g + 1) / (2 * g))


def riemann(L, R):
    """Star pressure and velocity between left and right states (rho, u, p, gamma)."""
    p = 0.5 * (L[2] + R[2])
    for _ in range(100):
        fl, dl = f_side(p, L[0], L[2], L[3])
        fr, dr = f_side(p, R[0], R[2], R[3])
        dp = (fl + fr + R[1] - L[1]) / (dl + dr)
        p = max(1e-6 * p, p - dp)
        if abs(dp) < 1e-12 * p:
            break
    u = 0.5 * (L[1] + R[1]) + 0.5 * (f_side(p, R[0], R[2], R[3])[0] - f_side(p, L[0], L[2], L[3])[0])
    return p, u


def right_shock(R, p, u):
    """Speed of the right-facing shock into the right state R, and the density behind it."""
    rho, ur, pr, g = R
    c = math.sqrt(g * pr / rho)
    W = ur + c * math.sqrt((g + 1) / (2 * g) * p / pr + (g - 1) / (2 * g))
    rho_s = rho * ((p / pr + (g - 1) / (g + 1)) / ((g - 1) / (g + 1) * p / pr + 1))
    return W, rho_s


def case(gamma_b, R_b):
    T0 = P0 / (RHO0 * R_AIR)
    rho_b = P0 / (R_b * T0)
    # the incident shock in air
    c0 = math.sqrt(G_AIR * P0 / RHO0)
    p2 = P0 * (2 * G_AIR * MS * MS - (G_AIR - 1)) / (G_AIR + 1)
    rho2 = RHO0 * (G_AIR + 1) * MS * MS / ((G_AIR - 1) * MS * MS + 2)
    u2 = MS * c0 * (1 - RHO0 / rho2)
    # the upstream face
    ps, us = riemann((rho2, u2, p2, G_AIR), (rho_b, 0.0, P0, gamma_b))
    v_r, rho_bs = right_shock((rho_b, 0.0, P0, gamma_b), ps, us)
    # the downstream face
    pt, ut = riemann((rho_bs, us, ps, gamma_b), (RHO0, 0.0, P0, G_AIR))
    v_t, _ = right_shock((RHO0, 0.0, P0, G_AIR), pt, ut)
    return {'v_refracted': round(v_r, 2), 'v_transmitted': round(v_t, 2), 'v_upstream_interface': round(us, 2)}


def main():
    cases = json.loads(os.environ['FLAG_CASES'])
    print(json.dumps({c['case']: case(c['inputs']['gamma'], c['inputs']['gas_constant_j_kgk']) for c in cases}))


if __name__ == '__main__':
    main()
