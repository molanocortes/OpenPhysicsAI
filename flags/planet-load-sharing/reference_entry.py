#!/usr/bin/env python3
"""The reference entry on trial planet-load-sharing: this repository's gear trains (src/lab/mechanism/mtrain.h).

The four-planet helical set is built from the trial's stated geometry as cutters make it (a rack for the sun and the
planets, a shaper for the ring), every gear elastic in hexahedra, all in frictionless contact on both flanks of every
mesh: the sun floating, each planet on a bearing spring about its pin, the pin of planet 1 out of place by the case's
error, the ring held at 11 lugs and free to move radially there, the carrier held while the sun's torque is applied.
Each case is the mean, over six positions through one mesh cycle, of the ring meshes' forces of planets 1 and 3 over
the sum of the four (the share the ring's root strain measures).

Nothing here reads an answer: the scenario is built from the trial's conditions and the case's inputs only. What the
trial does not state is assumed and said so in the scenario: the 22.5 deg normal pressure angle (the stated 23.04 deg
being its transverse value), the profile shifts' split, standard addenda and a 1.25 / 0.38 rack, a 31-tooth shaper for
the ring, 25 um of thinning on every flank (backlash), the bores, steel, the lugs' width (6 deg), and the needle
bearings' stiffness: 2e5 N/mm, the value from the bearing maker's data that the source's own model used (a later
model of the same rig used 1.3e5 N/mm; docs/lab/mechanism.md shows both). Environment: LAB_KEEP=1 keeps the result
files in build/flags/.
"""
import json
import math
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
WORK = ROOT / 'build' / 'flags' / 'planet-load-sharing'

# the trial's conditions (flag.json)
ZS, ZP, ZR, MN, BETA, CENTRE, FACE = 73, 26, 125, 0.00181, 13.12, 0.09212, 0.025
SHIFT_SUM = 0.0685                                  # x_sun + x_planet = x_ring - x_planet, from the centre distance
PLANET_DEG = [0, 360 * 50 / 198, 180, 360 * 149 / 198]
# assumed: not in the trial's conditions
ALPHA_N = 22.5                                      # the stated 23.04 deg is its transverse value at this helix
X_SUN, X_PLANET, X_RING = SHIFT_SUM, 0.0, SHIFT_SUM
THINNING = 25e-6                                    # every flank: a normal backlash of about 0.1 mm a mesh
BEARING = 2e8                                       # N/m, the bearing maker's value the source's model used
E, NU = 207e9, 0.3
STEEL = {'youngs_modulus_pa': E, 'poisson_ratio': NU, 'source': 'assumed steel: the trial states no material'}


def gear(z, x, helix, bore_d, mesh, **extra):
    g = {'teeth': z, 'module_m': MN, 'pressure_angle_deg': ALPHA_N, 'helix_angle_deg': helix, 'profile_shift': x,
         'addendum_coefficient': 1.0, 'dedendum_coefficient': 1.25, 'cutter_tip_radius_coefficient': 0.38, 'flank_thinning_m': THINNING,
         'face_width_m': FACE, 'bore_diameter_m': bore_d, 'material': STEEL, 'mesh': mesh}
    g.update(extra)
    return g


def scenario(error_um, torques_nm):
    mt = MN / math.cos(math.radians(BETA))
    ring_root = mt * ZR / 2 + (1.25 + X_RING) * MN          # about where the shaper's corner leaves the root
    cycle = (360 / ZS) / (1 - 1 / (1 + ZR / ZS))           # the sun's turn through one mesh cycle against the carrier
    coarse = {'across': 4, 'flank': 6, 'fillet': 3, 'rim': 4, 'face': 4}
    return {
        'domain': 'mechanism', 'model': 'gear_train',
        'title': 'trial planet-load-sharing: pin %+g um' % error_um,
        'provenance': {'source': 'the trial planet-load-sharing (flags/planet-load-sharing/flag.json)',
                       'assumed': 'see flags/planet-load-sharing/reference_entry.py'},
        'gears': {
            'sun': gear(ZS, X_SUN, BETA, 0.090, dict(coarse, rim=3)),
            'planet': gear(ZP, X_PLANET, -BETA, 0.028, coarse),
            'ring': gear(ZR, X_RING, -BETA, round(2 * (ring_root + 0.013), 6), coarse, internal=True,
                         cutter={'teeth': 31, 'addendum_coefficient': 1.25, 'dedendum_coefficient': 1.25, 'cutter_tip_radius_coefficient': 0.25},
                         hold={'lugs': 11, 'lug_width_deg': 6, 'phase_deg': 0, 'radial_free': True}),
        },
        'shafts': [
            {'name': 'sun', 'axis_m': [0, 0], 'torque_nm': -torques_nm[0], 'floating': True},
            {'name': 'carrier', 'axis_m': [0, 0], 'locked': True},
            {'name': 'planet', 'axis_m': [CENTRE, 0], 'carrier': 'carrier', 'angles_deg': PLANET_DEG,
             'pin_errors_m': [[error_um * 1e-6, 0], [0, 0], [0, 0], [0, 0]], 'support_n_per_m': BEARING},
            {'name': 'housing', 'axis_m': [0, 0], 'fixed': True},
        ],
        'parts': [{'name': 'sun', 'gear': 'sun', 'shaft': 'sun'}, {'name': 'planet', 'gear': 'planet', 'shaft': 'planet'},
                  {'name': 'ring', 'gear': 'ring', 'shaft': 'housing'}],
        'meshes': [['sun', 'planet'], ['planet', 'ring']],
        'input': 'sun',
        # the sun's torque clockwise: with a positive error planet 1 then leads and is pressed into mesh first
        'run': {'positions': 6, 'input_from_deg': 0, 'input_to_deg': -cycle * 5 / 6, 'torques_nm': [-t for t in torques_nm], 'frames': False},
    }


def run(error_um, torques_nm):
    WORK.mkdir(parents=True, exist_ok=True)
    name = 'pin_%+g' % error_um
    path, lab = WORK / (name + '.json'), WORK / (name + '.lab')
    path.write_text(json.dumps(scenario(error_um, torques_nm), indent=1))
    r = subprocess.run([str(ROOT / 'build' / 'labrun'), str(path), str(lab), '--quiet'], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit('labrun failed on %s: %s' % (name, (r.stderr or r.stdout)[-800:]))
    rec = json.loads(pathlib.Path(str(lab) + '.train.json').read_text())
    if os.environ.get('LAB_KEEP') != '1':
        lab.unlink(missing_ok=True)
        pathlib.Path(str(lab) + '.train.json').unlink(missing_ok=True)
    shares = {}
    for t, f in zip(rec['input_torque_nm'], rec['mesh_force_n']):
        ring = f[4:8]  # meshes: the sun with planets 1 to 4, then planets 1 to 4 with the ring
        shares.setdefault(-t, []).append((ring[0] + ring[2]) / 2 / sum(ring))
    return {t: sum(v) / len(v) for t, v in shares.items()}, rec.get('wall_s')


def main():
    cases = json.loads(os.environ['FLAG_CASES'])
    errors = sorted({c['inputs']['pin_error_um'] for c in cases})
    preds = {}
    for e in errors:
        torques = sorted({c['inputs']['sun_torque_nm'] for c in cases if c['inputs']['pin_error_um'] == e})
        share, wall = run(e, torques)
        for c in cases:
            if c['inputs']['pin_error_um'] == e:
                preds[c['case']] = {'overloaded_pair_share': round(share[c['inputs']['sun_torque_nm']], 4)}
        print('pin %+g um: %s (%s s)' % (e, ', '.join('%g N m %.4f' % (t, share[t]) for t in torques), wall), file=sys.stderr)
    print(json.dumps(preds))


if __name__ == '__main__':
    main()
