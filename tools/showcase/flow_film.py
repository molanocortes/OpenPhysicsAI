#!/usr/bin/env python3
"""flow_film.py - showcase C, fluid dynamics: tracer particles in the wake of a sailplane, from the native application.

    python3 tools/showcase/flow_film.py [--out docs/media] [--preview] [--aoa 6] [--frames 60]

The lattice Boltzmann tunnel runs until the wake is developed, is paused, and is then advanced by exactly STEPS
solver steps per film frame with the solver stopped while each frame is drawn and saved. The particles are advected by
the computed velocity field for those same steps, so every frame is the same interval of simulated time. (The earlier
film let the solver run on while screenshots were written: the interval varied from frame to frame, the particle
backlog was clipped, and the motion stuttered. That was the capture, not the flow.) Nothing is interpolated, reversed
or retimed; the GIF restarts at its end and the flow is not periodic.
The application frames are placed in the same frame as every other showcase picture (showcase.compose).
"""
import subprocess, sys, tempfile, json, shutil
from pathlib import Path
from PIL import Image

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
from showcase import compose, write_film, PRESET  # noqa: E402

STEPS, DEVELOP = 10, 2600


def main():
    a = sys.argv
    out = Path(a[a.index("--out") + 1]) if "--out" in a else ROOT / "docs/media"
    aoa = float(a[a.index("--aoa") + 1]) if "--aoa" in a else 8.0
    preview = "--preview" in a
    n = 4 if preview else int(a[a.index("--frames") + 1]) if "--frames" in a else 44
    gap = 12 if preview else 1                       # a preview looks at four states far apart
    opt = lambda k, d: a[a.index(k) + 1] if k in a else d
    count, emitter, orbit, zoom = opt("--count", "14000"), opt("--emitter", "sheet"), opt("--orbit", "140 38").replace(",", " "), opt("--zoom", "0.8")
    tmp = Path(tempfile.mkdtemp(prefix="nvshow_flow"))
    w, h = PRESET["picture"]
    nav = f"""workspace fluid
scene glider
load models/showcase-sailplane.stl
quality draft
aoa {aoa:g}
view speed
cmap hydro
linecmap hydro
surface solid
surface grid off
slice off
volume off
floorgrid off
box off
hud off
backdrop neutral
particles on
particles count {count}
particles size 0.7
particles emitter {emitter}
streamlines off
vortices off
bloom 0.12
exposure 1.0
camera model
camera orbit {orbit}
camera zoom {zoom}
start
wait {DEVELOP}
pause
frames 4
status
"""
    for i in range(n):
        nav += f"step {STEPS * gap}\nframes {3 * gap}\nscreenshot {tmp}/f{i:03d}.png\n"
    nav += "status\nquit\n"
    (tmp / "capture.nav").write_text(nav)
    log = subprocess.run([str(ROOT / "navier"), "--headless", "--size", f"{w}x{h}", "--workspace", str(tmp / "ws"), "--exec", f"exec {tmp}/capture.nav"],
                         cwd=ROOT, capture_output=True, text=True, timeout=590)
    text = log.stdout + log.stderr
    (tmp / "capture.log").write_text(text)
    steps = [ln for ln in text.splitlines() if "step" in ln.lower() and ("MLUPS" in ln or "steps" in ln)][-2:]
    frames = []
    for i in range(n):
        im = Image.open(tmp / f"f{i:03d}.png").convert("RGB").resize((w, h))
        frames.append(compose(im, "Flow: the wake of a sailplane", f"a smoke sheet of tracer particles in the computed velocity field   ·   angle of attack {aoa:g}°",
                              note=f"Lattice Boltzmann tunnel, draft lattice. Each frame advances the solver by {STEPS} steps; nothing is interpolated.", run="scene glider"))
    out.mkdir(parents=True, exist_ok=True)
    if preview:
        sheet = Image.new("RGB", (1600, 900))
        for i, f in enumerate(frames): sheet.paste(f.resize((800, 450)), ((i % 2) * 800, (i // 2) * 450))
        sheet.save(out / f"flow-preview-{emitter}-{orbit.replace(chr(32), chr(95))}.png")
        print("preview written;", steps)
    else:
        size = write_film(frames, out / "showcase-flow.gif", ms=80, hold_last=80, hold_first=80, colors=40)
        frames[n // 2].save(out / "showcase-flow-poster.png", optimize=True)
        (out / "showcase-flow.json").write_text(json.dumps({"scene": "glider, models/showcase-sailplane.stl", "quality": "draft", "aoa_deg": aoa, "develop_steps": DEVELOP,
                                                           "steps_per_frame": STEPS, "frames": n, "particles": int(count), "emitter": emitter, "camera_orbit": orbit, "zoom": float(zoom), "gif_bytes": size, "status": steps}, indent=1))
        print("film", size, "bytes;", steps)
    shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
