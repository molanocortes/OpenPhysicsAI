#!/usr/bin/env python3
"""Compute small, reproducible FDM and LPBF open-cell demonstrations through MCP.

The material records and process/strain inputs are demonstrations, not calibrated
forecasts. FDM activates simulation layers; it does not resolve deposited roads.
LPBF uses elastic inherent strain with an assumed support option. The result
records state whether any supports actually exist. No measurement is invented.

After make: python3 tools/demo_printing.py --capture --film
The optional captures require the native macOS OpenGL app. --film saves the actual
stored states as PNG frames; encoding a GIF/video is a separate tooling choice.
"""
import argparse
import json
import struct
import subprocess
import time
from pathlib import Path

from mcptest import Client

ROOT = Path(__file__).resolve().parents[1]


def geometry(path):
    cells = {(x, y, z) for x in range(24) for y in range(6) for z in range(12)
             if z < 2 or z >= 10 or x < 2 or x >= 22 or 7 <= x < 9 or 15 <= x < 17
             or ((y < 2 or y >= 4) and abs((x % 8) - .8 * (z - 2)) <= 1)}
    faces = {(-1, 0, 0): ((0, 0, 0), (0, 0, 1), (0, 1, 1), (0, 1, 0)),
             (1, 0, 0): ((1, 0, 0), (1, 1, 0), (1, 1, 1), (1, 0, 1)),
             (0, -1, 0): ((0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)),
             (0, 1, 0): ((0, 1, 0), (0, 1, 1), (1, 1, 1), (1, 1, 0)),
             (0, 0, -1): ((0, 0, 0), (0, 1, 0), (1, 1, 0), (1, 0, 0)),
             (0, 0, 1): ((0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1))}
    tris = []
    for x, y, z in sorted(cells):
        for normal, corners in faces.items():
            if (x + normal[0], y + normal[1], z + normal[2]) in cells:
                continue
            p = [(x + a, y + b, z + c) for a, b, c in corners]
            tris += [(normal, p[0], p[1], p[2]), (normal, p[0], p[2], p[3])]
    with path.open('wb') as f:
        f.write(b'OpenPhysicsAI numerical demonstration'.ljust(80, b' ') + struct.pack('<I', len(tris)))
        for normal, a, b, c in tris:
            f.write(struct.pack('<12fH', *normal, *a, *b, *c, 0))
    return len(cells)


def native_script(out, runs, film):
    q = lambda p: json.dumps(str(p))
    lines = ['mode manual', 'workspace solid', 'floorgrid off', 'box off']
    for run in runs:
        kind, n = run['kind'], run['status']['summary']['results']['stored_times']
        project = run['project']['project']['directory']
        lines += [f'solid open {q(project)}', 'frames 8', f"fem job {run['run']['job_id']}", 'frames 12',
                  'fem pause', 'fem deform true', 'fem range all', 'fem fit', 'uiclick "6 RESULTS"', 'frames 4']
        if kind == 'fdm':
            lines += [f'fem step {n // 2 + 1}', 'fem field temperature']
        else:
            lines += ['fem step last', 'fem field von_mises']
        lines += ['frames 8', f'screenshot {q(out / (kind + "-inspector.png"))}', 'hud clean', 'frames 30',
                  'fem fit', 'frames 12', f'screenshot {q(out / (kind + "-clean.png"))}']
        if film and kind == 'fdm':
            frames = out / 'fdm-frames'
            frames.mkdir(exist_ok=True)
            for i in range(1, n + 1):
                lines += [f'fem step {i}', 'frames 3', f'screenshot {q(frames / (f"{i:03d}.png"))}']
        lines += ['fem step last', 'fem field displacement', 'frames 8',
                  f'screenshot {q(out / (kind + "-displacement.png"))}']
        if kind == 'lpbf':
            lines += ['fem field von_mises', 'fem section y 0.5', 'frames 8',
                      f'screenshot {q(out / "lpbf-section.png")}', 'fem section off']
        lines += ['hud on', 'frames 8']
    lines += ['quit', '']
    path = out / 'capture.nav'
    path.write_text('\n'.join(lines))
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/demo-printing')
    parser.add_argument('--capture', action='store_true')
    parser.add_argument('--film', action='store_true', help='capture FDM stored states as PNGs (implies --capture)')
    args = parser.parse_args()
    out = args.output.expanduser().resolve()
    out.mkdir(parents=True, exist_ok=True)
    ws = out / 'workspace'
    ws.mkdir(exist_ok=True)
    stl = out / 'open-cell-structure.stl'
    count = geometry(stl)
    deadline = time.monotonic() + 480
    c = Client(['--embedded', '--workspace', str(ws), '--allow-read', str(out)])
    runs = []

    def call(name, params):
        response = c.call(name, params).get('result', {}).get('structuredContent', {})
        if not response.get('ok'):
            raise RuntimeError(f'{name}: {json.dumps(response.get("error"))}')
        return response.get('value', {})

    try:
        c.initialize()
        for kind in ('fdm', 'lpbf'):
            name = 'lattice_' + kind
            if (ws / name).exists():
                project = call('project_open', {'path': str(ws / name)})
            else:
                project = call('project_create', {'name': name, 'description': 'Numerical demonstration, inferred inputs, no measurement comparison'})
            call('geometry_import', {'path': str(stl), 'units': 'mm', 'name': 'structure', 'replace': True})
            call('material_assign', {'body': 'structure', 'material': 'pla_generic_demo' if kind == 'fdm' else 'ss316l_lpbf_demo', 'source': 'inferred'})
            mesh = call('mesh_generate', {'element_size': '1 mm'})
            call('project_save', {})
            if kind == 'fdm':
                inputs = {'body': 'structure', 'label': 'FDM numerical demonstration, inferred settings', 'process': {
                    'layer_height': '1 mm', 'printed_layer_height': '0.2 mm', 'nozzle_temperature': '210 degC',
                    'bed_temperature': '60 degC', 'ambient_temperature': '30 degC', 'deposition_rate': '8 mm^3/s',
                    'min_layer_time': '8 s', 'cooldown_bed_on': '120 s', 'cooldown_bed_off': '300 s',
                    'thermal_substeps': 4, 'provenance': 'inferred'}}
                op = 'mech_print_run'
            else:
                inputs = {'body': 'structure', 'build_orientation': 'X', 'layer_thickness_sim': '1 mm',
                    'inherent_strain': {'exx': -.001, 'eyy': -.002, 'ezz': -.01, 'provenance': 'inferred', 'source': 'demonstration tensor, not a calibration'},
                    'material': {'youngs_modulus': '215000 MPa', 'poissons_ratio': .3, 'provenance': 'inferred'},
                    'supports': {'stiffness_fraction': .1, 'provenance': 'assumed', 'source': 'demonstration stiffness, not measured', 'remove': True},
                    'label': 'LPBF elastic numerical demonstration, inferred inputs'}
                op = 'lpbf_build_run'
            started = time.monotonic()
            job = call(op, inputs)
            while True:
                status = call('job_status', {'job_id': job['job_id'], 'wait_seconds': 10})
                if status['state'] not in ('queued', 'running'):
                    break
                if time.monotonic() >= deadline:
                    call('job_cancel', {'job_id': job['job_id']})
                    raise RuntimeError('demonstration exceeded its eight-minute solve budget')
            if status['state'] != 'succeeded':
                raise RuntimeError(json.dumps(status))
            runs.append({'kind': kind, 'project': project, 'mesh': mesh, 'inputs': inputs, 'run': job,
                         'status': status, 'elapsed_seconds': time.monotonic() - started})
            (out / 'runs.json').write_text(json.dumps(runs, indent=2))
            r = status['summary']['results']
            model = status['summary']['model']
            print(f"{kind}: {job['job_id']} succeeded, {model['elements']} elements, {r['stored_times']} stored states, {runs[-1]['elapsed_seconds']:.3f} s", flush=True)
            if kind == 'fdm':
                print(f"  released peak {r['peak_von_mises_released_mpa']:.6g} MPa; z warp [{r['warp_z_min_mm']:.6g}, {r['warp_z_max_mm']:.6g}] mm; worst heat balance {r['worst_heat_balance_relative']:.3e}", flush=True)
            else:
                print(f"  elastic peak {r['peak_von_mises_before_cut_mpa']:.6g} MPa; maximum displacement {r['max_displacement_before_cut_mm']:.6g} mm; equilibrium error {r['equilibrium_error_last_solve']:.3e}", flush=True)
    finally:
        c.close()
    script = native_script(out, runs, args.film)
    print(f'{count} geometry cells; native capture script: {script}', flush=True)
    if args.capture or args.film:
        with (out / 'capture.log').open('w') as log:
            subprocess.run([str(ROOT / 'navier'), '--headless', '--size', '1440x900', '--workspace', str(ws),
                            '--exec', 'exec ' + json.dumps(str(script))], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                           timeout=120, check=True)
        expected = [out / (kind + suffix + '.png') for kind in ('fdm', 'lpbf')
                    for suffix in ('-inspector', '-clean', '-displacement')]
        expected.append(out / 'lpbf-section.png')
        if args.film:
            count = runs[0]['status']['summary']['results']['stored_times']
            expected.extend(out / 'fdm-frames' / f'{i:03d}.png' for i in range(1, count + 1))
        missing = [str(path) for path in expected if not path.exists()]
        if missing:
            raise RuntimeError('native capture omitted expected files: ' + ', '.join(missing))
        print(f'native captures saved in {out}', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
