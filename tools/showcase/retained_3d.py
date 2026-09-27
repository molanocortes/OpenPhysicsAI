#!/usr/bin/env python3
"""Compute a small water result and capture the native retained renderer, without retouching frames."""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, default=ROOT / 'build' / 'retained-3d')
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
subprocess.run(['make', '-j2', 'navier', 'build/labrun'], cwd=ROOT, check=True)
scenario = json.loads((ROOT / 'examples/lab/dam_break.json').read_text())
scenario['title'] = '3D dam break: retained viewer study'
scenario['particle_spacing_m'] = .03
scenario['run'].update(end_s=.8, frames=24)
# Match the existing scenario schema, including its particle-spacing key.
original = json.loads((ROOT / 'examples/lab/dam_break.json').read_text())
if 'particle_spacing_m' not in original:
    raise SystemExit('Water scenario schema changed: review spacing before running')
source, result = out / 'water.json', out / 'water.lab'
source.write_text(json.dumps(scenario, indent=2) + '\n')
subprocess.run([str(ROOT / 'build/labrun'), str(source), str(result)], cwd=ROOT, check=True, timeout=300)
commands = [f'lab open "{result}"', 'lab pause', 'lab frame 18', 'lab field speed',
            'lab surface off', 'lab range 0 3', 'lab orbit -60 35', 'lab renderer cpu', 'frames 6',
            f'screenshot "{out / "before.png"}"', 'lab bench 60', 'frames 65',
            'lab renderer gpu', 'frames 6', f'screenshot "{out / "after.png"}"',
            'lab bench 60', 'frames 65']
images = []
for i in range(80):
    if i < 24:
        commands += [f'lab frame {i}']
    elif i < 56:
        commands += ['lab frame 18', f'lab orbit {-60 + (i-24)*2} 35']
    else:
        commands += ['lab orbit -60 35', 'lab section y 0.5', f'lab frame {i-56}']
    path = out / f'frame-{i:03d}.png'
    images.append(str(path))
    commands += ['frames 2', f'screenshot "{path}"']
commands += ['lab info', 'quit']
script = out / 'capture.nav'
script.write_text('\n'.join(commands) + '\n')
with (out / 'capture.log').open('w') as log:
    subprocess.run([str(ROOT / 'navier'), '--headless', '--size', '1440x900', '--exec', f'exec "{script}"'],
                   cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300)
subprocess.run(['swift', str(ROOT / 'tools/encode_gif.swift'), str(out / 'viewer.gif'), '0.083333', *images],
               cwd=ROOT, check=True, timeout=180)
print(f'Native captures, equal-path benchmarks and film: {out}')
