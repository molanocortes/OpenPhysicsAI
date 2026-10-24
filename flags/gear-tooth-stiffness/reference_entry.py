#!/usr/bin/env python3
"""The reference entry on flag gear-tooth-stiffness: this repository's mechanism domain (src/lab/mechanism).

The gears are built from the flag's stated geometry as a generating rack makes them, meshed in hexahedra and solved as
two elastic parts in frictionless contact (src/lab/mechanism/mgearpair.h). The experiment's own reduction is then
repeated on the computed transmission error: a single tooth pair (the pinion's neighbouring teeth absent) and two
pairs sharing the load symmetrically about the pitch point, each at the case's two loads, half a base pitch from the
pitch point; delta1 and delta2 are the transmission errors' changes between the loads, and

    c_b = N / (2 delta2 - delta1),     c_t = N / (2 (delta1 - delta2)),

N the change of load per unit face width. The pitch point's stiffness is the single pair's there with the same c_b
taken out. Both sides of the pitch point are computed and averaged (the gears are alike, so they differ only by the
approach and recess of the contact).

Nothing here reads an answer: the scenario is built from the flag's conditions and the case's loads only. The values
the flag does not state are assumed and said so in the scenario: steel, the rack's tip radius, the crowning's
height, the blanks' bore. Environment: LAB_KEEP=1 keeps the result files in build/flags/.
"""
import json
import math
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
WORK = ROOT / 'build' / 'flags' / 'gear-tooth-stiffness'

# the flag's conditions (flag.json), in SI units
Z, M, ALPHA_DEG, FACE = 54, 0.003738, 18.0, 0.011
TIP_D, ROOT_D, THICK, CENTRE = 0.212344, 0.190575, 0.006220, 0.2032
# derived from them: the profile shift from the tooth thickness, the rack's addendum from the root diameter, the
# gears' addendum from the outside diameter
ALPHA = math.radians(ALPHA_DEG)
R_REF = M * Z / 2
X = (THICK - math.pi * M / 2) / (2 * M * math.tan(ALPHA))
HFP = (R_REF + X * M - ROOT_D / 2) / M
HA = (TIP_D / 2 - R_REF) / M - X
RB = R_REF * math.cos(ALPHA)
# assumed: not in the flag's conditions
E, NU = 207e9, 0.3           # steel
RHO_FP = 0.25                # the generating rack's tip radius, in modules (a full round would be 0.342)
CROWN = 3.5e-6               # lead crowning on each gear (the source says 3 to 4 um)
BORE_DEPTH = 3.0             # the blank solid to a bore this many modules below the root


def gear():
    return {
        'teeth': Z, 'module_m': M, 'pressure_angle_deg': ALPHA_DEG, 'profile_shift': round(X, 6), 'addendum_coefficient': round(HA, 6),
        'dedendum_coefficient': round(HFP, 6), 'cutter_tip_radius_coefficient': RHO_FP, 'tip_diameter_m': TIP_D, 'lead_crown_m': CROWN,
        'face_width_m': FACE, 'bore_diameter_m': ROOT_D - 2 * BORE_DEPTH * M,
        'material': {'youngs_modulus_pa': E, 'poisson_ratio': NU, 'source': 'assumed steel: the flag states no material'},
    }


def run(name, single, rolls_deg, torques):
    sc = {
        'domain': 'mechanism', 'model': 'gear_pair', 'title': 'flag gear-tooth-stiffness: %s' % name,
        'gears': [gear(), gear()], 'centre_distance_m': CENTRE, 'single_pinion_tooth': single, 'hold_sector_cuts': True,
        'mesh': {'teeth_modelled': 7, 'across': 14, 'flank': 28, 'fillet': 12, 'rim': 12, 'face': 8},
        'load': {'pinion_torque_nm': torques}, 'run': {'gauged_roll_deg': rolls_deg},
    }
    WORK.mkdir(parents=True, exist_ok=True)
    path, lab = WORK / ('%s.json' % name), WORK / ('%s.lab' % name)
    path.write_text(json.dumps(sc, indent=1))
    r = subprocess.run([str(ROOT / 'build' / 'labrun'), str(path), str(lab), '--quiet'], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit('labrun failed on %s: %s' % (name, (r.stderr or r.stdout)[-800:]))
    if os.environ.get('LAB_KEEP') != '1':
        lab.unlink(missing_ok=True)
    d = json.loads(r.stdout.strip().splitlines()[-1])['diagnostics']
    te, nt = d['te_elastic_um'], len(torques)
    # frames are position-major: te[position * nt + torque]
    return [[te[p * nt + k] for k in range(nt)] for p in range(len(rolls_deg))], d.get('wall_s')


def main():
    cases = json.loads(os.environ['FLAG_CASES'])
    loads = sorted({c['inputs'][k] for c in cases for k in ('load_low_n_per_mm', 'load_high_n_per_mm')})
    torques = [n * FACE * 1e3 * RB for n in loads]  # N/mm x mm x m
    # the pitch point and half a base pitch either side of it, as roll angles of the contact on the gear's tooth
    th_w = math.sqrt((CENTRE / 2 / RB) ** 2 - 1)
    dth = math.pi * M * math.cos(ALPHA) / 2 / RB
    rolls = [math.degrees(th_w - dth), math.degrees(th_w), math.degrees(th_w + dth)]
    one, w1 = run('single_pair', True, rolls, torques)
    two, w2 = run('two_pairs', False, [rolls[0], rolls[2]], torques)
    preds = {}
    for c in cases:
        lo, hi = loads.index(c['inputs']['load_low_n_per_mm']), loads.index(c['inputs']['load_high_n_per_mm'])
        N = loads[hi] - loads[lo]
        d1 = 0.5 * ((one[0][hi] - one[0][lo]) + (one[2][hi] - one[2][lo]))
        d2 = 0.5 * ((two[0][hi] - two[0][lo]) + (two[1][hi] - two[1][lo]))
        cb = N / (2 * d2 - d1)
        ct = N / (2 * (d1 - d2))
        dp = one[1][hi] - one[1][lo]
        cp = N / (dp - N / cb)
        preds[c['case']] = {'tooth_pair_stiffness_half_pitch_n_per_mm_um': round(ct, 3), 'tooth_pair_stiffness_pitch_point_n_per_mm_um': round(cp, 3)}
        print('%s: delta1 %.3f um, delta2 %.3f um, c_b %.2f, c_t %.3f, pitch point %.3f N/(mm um)' % (c['case'], d1, d2, cb, ct, cp), file=sys.stderr)
    print('runs: %s s and %s s' % (w1, w2), file=sys.stderr)
    print(json.dumps(preds))


if __name__ == '__main__':
    main()
