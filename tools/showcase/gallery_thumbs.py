#!/usr/bin/env python3
"""gallery_thumbs.py - the home screen's pictures: every library scenario computed and filmed in the app's own studio
look, one still each, into docs/media/thumbs/<scenario>.jpg (the app's gallery shows a scenario only when its picture
exists, so a scenario that does not look good is left out by leaving out its picture).

    python3 tools/showcase/gallery_thumbs.py                      # every scenario in examples/lab without a picture
    python3 tools/showcase/gallery_thumbs.py glass_bottle flag_3d # these, again
    python3 tools/showcase/gallery_thumbs.py --timeout 1800 fire  # a long one, detached or overnight
    python3 tools/showcase/gallery_thumbs.py --projects           # the computed parts in ~/OpenPhysicsAI-Projects (PROJECTS)

Each scenario runs with build/labrun (skipped past --timeout seconds, default 360, so a working session stays inside
its budget), is opened headless in ./openphysics with the panels hidden at its last frame, or at FRAME below, and saved as a
640 x 480 JPEG. The computed result is deleted afterwards; only the picture is kept.
"""
import argparse, json, os, subprocess, sys, tempfile, time
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "docs" / "media" / "thumbs"
FRAME = {"breaking_wave": 0.55, "glass_bottle": 0.7, "whipple_shield": 0.6, "crater_aluminium": 0.7, "dam_break": 0.45,
         "sloshing_tank": 0.5, "loudspeaker": 0.5, "shoebox_hall": 0.35, "rubber_sheet": 0.8}
PICTURES = {"black_hole"}


def autoframe(im, margin=0.12, max_zoom=2.6):
    """Frame the subject: the app's camera fits the whole domain, a picture wants the object. The studio backdrop and
    floor are smooth, so the subject is where the picture has edges (soft shadows have none); the box of the columns
    and rows with edges, with a margin, widened to 4:3, is what is kept."""
    import numpy as np
    from PIL import ImageFilter
    w, h = im.size
    e = np.array(im.convert("L").filter(ImageFilter.GaussianBlur(1.2)).filter(ImageFilter.FIND_EDGES)).astype(float)
    e[:3, :] = e[-3:, :] = 0
    e[:, :3] = e[:, -3:] = 0
    m = e > 18
    cols, rows = np.where(m.sum(0) >= 2)[0], np.where(m.sum(1) >= 2)[0]
    if len(cols) < 2 or len(rows) < 2:
        return im
    x0, x1, y0, y1 = cols[0], cols[-1], rows[0], rows[-1]
    bw, bh = (x1 - x0) * (1 + 2 * margin), (y1 - y0) * (1 + 2 * margin)
    if bw / max(bh, 1) < 4 / 3: bw = bh * 4 / 3
    else: bh = bw * 3 / 4
    bw, bh = max(bw, w / max_zoom), max(bh, h / max_zoom)
    if bw > w: bw, bh = w, w * 3 / 4
    if bh > h: bh, bw = h, h * 4 / 3
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    cx, cy = min(max(cx, bw / 2), w - bw / 2), min(max(cy, bh / 2), h - bh / 2)
    return im.crop((round(cx - bw / 2), round(cy - bh / 2), round(cx + bw / 2), round(cy + bh / 2)))


# the projects folder as the app finds it: ~/OpenPhysicsAI-Projects, or ~/NAVIER-Projects while only that one exists
PROJECTS_DIR = Path.home() / "OpenPhysicsAI-Projects"     # the finite-element projects the gallery shows when they have a picture
if not PROJECTS_DIR.is_dir() and (Path.home() / "NAVIER-Projects").is_dir():
    PROJECTS_DIR = Path.home() / "NAVIER-Projects"
RESULTS = PROJECTS_DIR / "lab"                            # labapp_results_dir(): where the app looks for a computed result
# computed parts (made by tools/art_gallery.py and tools/art_gallery_compute.py): the step, field and view each is shown at
PROJECTS = {"Thermal_Corolla": {"step": 11, "field": "temperature", "orbit": (30, 42)},
            "Helical_Lantern": {"step": 16, "field": "von_mises", "orbit": (30, 28)},
            "Helical_Flute": {"step": 31, "field": "temperature", "orbit": (30, 28)}}
KEEP_MAX_MB, KEEP_FREE_GB = 250, 0.8


def keep_result(name, lab):
    """Keep the computed result as the scenario's preloaded result unless it is very large or the disk is low."""
    import shutil
    mb = lab.stat().st_size / 1e6
    free_gb = shutil.disk_usage(Path.home()).free / 1e9
    return mb <= KEEP_MAX_MB and free_gb - mb / 1e3 > KEEP_FREE_GB


def frames_of(lab):
    out = subprocess.run([str(ROOT / "build/labfilm"), str(lab), "--info"], capture_output=True, text=True).stdout
    for line in out.splitlines():  # "frames: 61, time 0 to 0.02 s"
        if line.startswith("frames:"):
            return int(line.split(":")[1].split(",")[0])
    return 1


def shoot(name, scenario, timeout, work):
    t0 = time.time()
    stored = RESULTS / (name + ".lab")
    if stored.exists() and stored.stat().st_mtime >= scenario.stat().st_mtime:
        return film(name, stored, None, t0, work)  # already computed (and newer than its scenario): film it
    lab = work / (name + ".lab")
    try:
        r = subprocess.run([str(ROOT / "build/labrun"), str(scenario), str(lab), "--quiet"], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        lab.unlink(missing_ok=True)
        return "skipped: longer than %d s" % timeout
    if r.returncode or not lab.exists():
        return "failed: " + (r.stderr.strip().splitlines() or ["no result"])[-1][:120]
    return film(name, lab, keep_result(name, lab), t0, work)


def film(name, lab, keep, t0, work):
    """one still of a computed result; keep None: the result is the stored one and stays where it is"""
    n = frames_of(lab)
    frame = int(round((n - 1) * FRAME.get(name, 1.0)))
    png = work / (name + ".png")
    nav = work / (name + ".nav")
    nav.write_text("hud off\nlab open %s\nlab frame %d\nframes 45\nscreenshot %s\n" % (lab, frame, png))
    subprocess.run([(os.environ.get("OPENPHYSICS_BIN") or os.environ.get("NAVIER_BIN") or str(ROOT / "openphysics")), "--headless", "--size", "960x720", "--workspace", str(work / "ws"), "--exec", "exec %s" % nav],
                   capture_output=True, text=True, timeout=180)
    if keep:  # preloaded: the app opens this result at once when the scenario is chosen (labapp_select)
        RESULTS.mkdir(parents=True, exist_ok=True)
        lab.replace(RESULTS / (name + ".lab"))
    elif keep is not None:
        lab.unlink(missing_ok=True)
    if not png.exists():
        return "failed: no screenshot"
    from PIL import Image, ImageStat
    im = Image.open(png).convert("RGB")
    if name in PICTURES:  # a picture result (the black hole) is drawn flat in the room: keep the picture, fill the card
        g = im.convert("L")
        rows = [y for y in range(im.height) if ImageStat.Stat(g.crop((0, y, im.width, y + 1))).mean[0] < 120]
        if rows:
            im = im.crop((0, rows[0], im.width, rows[-1] + 1))
        s = max(640 / im.width, 480 / im.height)
        im = im.resize((round(im.width * s), round(im.height * s)), Image.LANCZOS)
        x0, y0 = (im.width - 640) // 2, (im.height - 480) // 2
        im = im.crop((x0, y0, x0 + 640, y0 + 480))
    else:
        im = autoframe(im)
    im.resize((640, 480), Image.LANCZOS).save(OUT / (name + ".jpg"), quality=86, optimize=True)
    png.unlink()
    return "ok, %d frames, %.0f s%s" % (n, time.time() - t0, ", preloaded" if keep is not False else "")


def film_project(name, spec, work):
    """one still of a computed finite-element project, in the app's studio look, read from the projects folder (the
    project is only opened, never recomputed)"""
    proj = PROJECTS_DIR / name
    if not (proj / "project.json").exists():
        return "skipped: not in %s" % PROJECTS_DIR
    png, nav = work / (name + ".png"), work / (name + ".nav")
    nav.write_text("\n".join([
        "mode manual", "workspace solid", 'solid open "%s"' % proj, "frames 30", "fem follow last", "frames 30", "fem pause",
        "fem step %d" % spec["step"], "fem deform true", "fem field %s" % spec["field"], "fem edges off", "fem outline off",
        "fem marker off", "box off", "hud off", "fem fit", "camera orbit %d %d" % spec["orbit"], "camera zoom 0.76",
        "frames 90", "screenshot %s" % png, ""]))
    subprocess.run([(os.environ.get("OPENPHYSICS_BIN") or os.environ.get("NAVIER_BIN") or str(ROOT / "openphysics")), "--headless", "--size", "960x720", "--workspace", str(work / "ws"),
                    "--exec", "exec %s" % nav], capture_output=True, text=True, timeout=240)
    if not png.exists():
        return "failed: no screenshot"
    autoframe(Image.open(png).convert("RGB")).resize((640, 480), Image.LANCZOS).save(OUT / (name + ".jpg"), quality=86, optimize=True)
    png.unlink()
    return "ok"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("names", nargs="*")
    ap.add_argument("--timeout", type=int, default=360)
    ap.add_argument("--projects", action="store_true", help="the computed parts in PROJECTS instead of lab scenarios")
    a = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    if a.projects:
        with tempfile.TemporaryDirectory(prefix="openphysics-thumbs-") as tmp:
            for n in a.names or PROJECTS:
                print("%-28s %s" % (n, film_project(n, PROJECTS[n], Path(tmp)) if n in PROJECTS else "unknown project"), flush=True)
        return 0
    scen = {p.stem: p for p in sorted((ROOT / "examples/lab").glob("*.json"))}
    names = a.names or [n for n in scen if not (OUT / (n + ".jpg")).exists()]
    with tempfile.TemporaryDirectory(prefix="openphysics-thumbs-") as tmp:
        work = Path(tmp)
        for n in names:
            if n not in scen:
                print("%-28s unknown scenario" % n, flush=True)
                continue
            print("%-28s %s" % (n, shoot(n, scen[n], a.timeout, work)), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
