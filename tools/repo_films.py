#!/usr/bin/env python3
"""Capture real simulation sequences. Run from the root: python3 tools/repo_films.py flow|build|heat.
Requires the native macOS app and Swift/ImageIO. No solver or field edits.
"""
from pathlib import Path
import subprocess
import sys

kind = sys.argv[1]
if kind not in ('flow', 'build', 'heat'):
    raise SystemExit('Choose flow, build or heat')
root = Path(__file__).resolve().parents[1]
out = root / 'docs/media'
out.mkdir(exist_ok=True)
tmp = Path('/tmp/openphysicsai-film-' + kind)
tmp.mkdir(exist_ok=True)
if kind == 'flow':
    setup = """workspace fluid
scene glider
load models/showcase-sailplane.stl
quality draft
aoa 6
view speed
cmap turbo
linecmap hydro
surface solid
surface grid off
slice off
volume off
floorgrid off
box off
hud off
particles on
particles count 18000
particles size 0.45
particles emitter sheet
streamlines off
vortices off
bloom 0.22
exposure 1.1
camera model
camera orbit 210 32
camera zoom 1.2
frames 15
screenshot docs/media/aircraft-poster.png
start
wait 1600
"""
    commands = [('wait 18\nframes 2', i) for i in range(40)]
    delay = 0.08
elif kind == 'build':
    setup = (root / 'tools/repo_showcase.nav').read_text().split('fem field von_mises')[0]
    setup += '''fem field displacement
fem range all
fem deform auto
fem fit
fem edges on
floorgrid off
box off
hud off
frames 12
'''
    commands = [(f'fem step {i+1}\nframes 5', i) for i in range(10)]
    delay = 0.45
else:
    setup = (root / 'tools/demo_playback.nav').read_text().split('fem field von_mises')[0]
    lines = setup.splitlines()
    heater = next(i for i, line in enumerate(lines) if 'demo_switched_heater' in line)
    lines[heater:heater+1] = [
        'am selection_create name=heated_end query=\'{"extreme":{"direction":"+x"}}\' source=user replace=true',
        'am boundary_apply \'{"name":"end_heater","kind":"heat_flux","selection":"heated_end","heat_flux":150000,"schedule":[{"time":0,"factor":1},{"time":40,"factor":0}],"source":"user"}\'']
    setup = '\n'.join(lines).replace('thermomechanical 40 10', 'thermomechanical 100 10') + '\n'
    setup += '''fem field temperature
fem range all
fem deform true
fem fit
floorgrid off
box off
hud off
frames 12
'''
    commands = [(f'fem step {i+1}\nframes 6', i) for i in range(11)]
    delay = 0.35
frames = []
for command, i in commands:
    frame = tmp / f'{i:03}.png'
    frames.append(str(frame))
    setup += command + f'\nscreenshot {frame}\n'
setup += f'screenshot {out / (kind + "-poster.png")}\nfem status\nstatus\nquit\n'
script = tmp / 'capture.nav'
script.write_text(setup)
with (tmp / 'capture.log').open('w') as log:
    subprocess.run([str(root / 'navier'), '--headless', '--size', '1000x640',
                    '--workspace', str(tmp / 'workspace'), '--exec', f'exec {script}'],
                   cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=540)
if kind != 'flow':
    log_text = (tmp / 'capture.log').read_text()
    expected = 9 if kind == 'build' else 10
    if 'succeeded' not in log_text or f'stored index {expected}' not in log_text:
        raise SystemExit('Capture did not reach the successful final stored state; inspect the log')
subprocess.run(['swift', str(root / 'tools/encode_gif.swift'), str(out / (kind + '.gif')),
                str(delay), *frames], check=True)
print('Capture log:', tmp / 'capture.log')
