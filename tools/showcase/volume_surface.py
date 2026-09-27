#!/usr/bin/env python3
"""Capture computed 3D fields and water surfaces through the native renderer."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--water', type=Path, required=True)
p.add_argument('--radar', type=Path, required=True)
p.add_argument('--output', type=Path, default=ROOT / 'build/volume-surface')
a = p.parse_args()
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
for path in (a.water, a.radar):
    if not path.is_file():
        p.error(f'Result does not exist: {path}')
subprocess.run(['make', '-j2', 'navier'], cwd=ROOT, check=True)
for name, result, field, limits, count, middle, axis in [
    ('water-surface', a.water, 'speed', '0 3', 24, 18, 'y'),
    ('radar-volume', a.radar, 'ez_scattered', '-0.15 0.15', 41, 23, 'z')]:
    commands = [f'lab open "{result.resolve()}"', 'lab pause', f'lab field {field}',
                f'lab range {limits}', 'lab orbit -60 35']
    if name == 'water-surface':
        commands += ['lab surface on']
    commands += [f'lab frame {middle}', 'frames 6', f'screenshot "{out / (name + ".png")}"']
    images = []
    # Stored-time playback, an orbit of one paused state, then a section in playback.
    for i in range(2*count+24):
        if i<count:
            commands += [f'lab frame {i}']
        elif i<count+24:
            commands += [f'lab frame {middle}', f'lab orbit {-60 + (i-count)*2} 35']
        else:
            commands += ['lab orbit -60 35', f'lab section {axis} 0.5', f'lab frame {i-count-24}']
        image = out / f'{name}-{i:03d}.png'
        commands += ['frames 2', f'screenshot "{image}"']
        images.append(str(image))
    commands += ['quit']
    script = out / f'{name}.nav'
    script.write_text('\n'.join(commands)+'\n')
    with (out / f'{name}.log').open('w') as log:
        subprocess.run([str(ROOT/'navier'), '--headless', '--size', '1200x800', '--exec', f'exec "{script}"'],
                       cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300)
    subprocess.run(['swift', str(ROOT/'tools/encode_gif.swift'), str(out/(name+'.gif')), '.083333', *images],
                   cwd=ROOT, check=True, timeout=180)
print(f'Unretouched native captures: {out}')
