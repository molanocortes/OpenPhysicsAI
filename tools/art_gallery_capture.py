#!/usr/bin/env python3
"""Capture real gallery results with the native renderer. No solver or image synthesis.

Run after art_gallery_compute.py. Each movie uses fixed full-range colour limits.
FDM and thermal films replay stored fields, without inventing intermediate states.
The LPBF orbit is a single pre-cut stress state; a separate film replays build/cut.
Geometry, lighting, background, palette and camera are presentation choices.
"""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PRESETS = {
    'fdm': dict(field='temperature', cmap='inferno', elevation=28, hero=31, frames=105),
    'lpbf': dict(field='von_mises', cmap='viridis', elevation=28, hero=16, frames=144),
    'thermal': dict(field='temperature', cmap='inferno', elevation=42, hero=11, frames=122),
}

def capture(kind, repo, out, size, stills_only=False):
    record = json.loads((out / f'{kind}-run.json').read_text())
    if not record.get('acceptance_passed'):
        raise RuntimeError('Run did not pass its predeclared criteria')
    project = out / 'workspace' / Path(record['project']['project']['directory']).name
    job = record['run']['job_id']
    result = project / 'runs' / job / 'results.nvt'
    result_hash = hashlib.sha256(result.read_bytes()).hexdigest()
    if result_hash != record['results_sha256']:
        raise RuntimeError('Saved result differs from the accepted computation record')
    preset = PRESETS[kind]
    folder = out / 'frames' / kind
    folder.mkdir(parents=True, exist_ok=True)
    q = lambda p: json.dumps(str(p))
    commands = ['mode manual', 'workspace solid', f'solid open {q(project)}', 'frames 30',
                f'fem job {job}', 'frames 30', 'fem pause', 'fem deform true', 'fem range all',
                'fem range peak', 'fem surface on', 'fem edges off', 'fem outline off', 'fem marker off',
                'fem shadows on', f'fem field {preset["field"]}', f'fem cmap {preset["cmap"]}',
                'floorgrid off', 'box off', 'bloom 0.06', 'exposure 1',
                'backdrop 0.008 0.012 0.020 0.002 0.003 0.007', 'camera fov 30', 'hud clean',
                'fem fit', f'camera orbit 30 {preset["elevation"]}', 'camera zoom 0.76', 'frames 120',
                f'fem step {preset["hero"]}', 'frames 60',
                f'screenshot {q(out / (kind + ".png"))}', 'frames 8', 'hud off', 'frames 8',
                f'fem export {q(out / (kind + "-art.png"))} 2', 'frames 8', 'hud clean', 'frames 8']
    frames = []
    for i in range(0 if stills_only else preset['frames']):
        if kind == 'fdm':
            step = 1 + i // 3
            yaw = 20 + 55 * i / (preset['frames'] - 1)
        elif kind == 'thermal':
            step = 1 + i // 2
            yaw = 30 + 55 * i / (preset['frames'] - 1)
        else:
            step = 16
            yaw = 30 + 360 * i / preset['frames']
        path = folder / f'{i:04d}.png'
        commands += [f'fem step {step}', f'camera orbit {yaw:.6f} {preset["elevation"]}',
                     'frames 16', f'screenshot {q(path)}']
        frames.append(dict(path=str(path.relative_to(out)), stored_step_one_based=step,
                           requested_yaw_degrees=yaw))
    if kind == 'lpbf':
        commands += ['camera orbit 30 28', 'frames 120']
        for step in ([] if stills_only else range(1, 18)):
            commands += [f'fem step {step}', 'frames 12', f'screenshot {q(folder / ("build-%02d.png" % step))}']
        commands += ['fem step 16', 'fem section y 0.55', 'frames 30',
                     f'screenshot {q(out / "lpbf-section.png")}', 'fem section off', 'frames 12',
                     'fem step 17', 'frames 12', f'screenshot {q(out / "lpbf-cut.png")}']
    commands += ['fem status', 'uitext', 'quit']
    label = f'{kind}-stills' if stills_only else f'{kind}-capture'
    script = out / f'{label}.nav'
    script.write_text('\n'.join(commands) + '\n')
    log = out / f'{label}.log'
    with log.open('w') as stream:
        subprocess.run([str(repo / 'navier'), '--headless', '--size', f'{size}x{size}', '--workspace',
                        str(out / 'workspace'), '--exec', 'exec ' + q(script)],
                       cwd=repo, stdout=stream, stderr=subprocess.STDOUT, check=True, timeout=480)
    text = log.read_text()
    if '[err!]' in text:
        raise RuntimeError('Native input reported an error; inspect ' + str(log))
    for frame in frames:
        path = out / frame['path']
        if not path.exists():
            raise RuntimeError('Missing native frame: ' + str(path))
        frame['sha256'] = hashlib.sha256(path.read_bytes()).hexdigest()
    artifacts = [out / (kind + '.png'), out / (kind + '-art.png')]
    if kind == 'lpbf':
        artifacts += [out / 'lpbf-section.png', out / 'lpbf-cut.png']
        if not stills_only:
            artifacts += [folder / ('build-%02d.png' % i) for i in range(1, 18)]
    artifact_hashes = {str(path.relative_to(out)): hashlib.sha256(path.read_bytes()).hexdigest() for path in artifacts}
    if hashlib.sha256(result.read_bytes()).hexdigest() != result_hash:
        raise RuntimeError('Result file changed during capture')
    evidence = dict(kind=kind, result_sha256=result_hash, artifacts=artifact_hashes,
                    renderer_sha256=hashlib.sha256((repo / 'navier').read_bytes()).hexdigest(),
                    pixels='Native OpenGL output; no painted fields or fabricated time interpolation',
                    deformation_scale=0 if kind == 'thermal' else 1,
                    range='all stored states, full peak (not percentile clipped)',
                    presentation='Designed geometry; artificial studio lighting, ambient occlusion, bloom and backdrop',
                    preset=preset, fps=20, frames=frames)
    (out / f'{label}.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(f'{kind}: {len(frames)} native frames, annotated still and double-resolution artwork')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kind', choices=PRESETS)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--repo', type=Path, default=REPO)
    parser.add_argument('--size', type=int, default=1000)
    parser.add_argument('--stills-only', action='store_true', help='recapture stills without recording another movie')
    args = parser.parse_args()
    if not 600 <= args.size <= 2000:
        parser.error('--size must be between 600 and 2000')
    capture(args.kind, args.repo.resolve(), args.output_dir.expanduser().resolve(), args.size, args.stills_only)
