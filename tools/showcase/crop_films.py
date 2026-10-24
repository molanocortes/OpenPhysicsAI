#!/usr/bin/env python3
"""crop_films.py - the front page's films cut to their scene: the app's panel, the burned-in captions and colour bars
of older renders are cropped away; nothing inside the scene is retouched, and every frame keeps its palette and timing.

    python3 tools/showcase/crop_films.py            # writes every *-view.gif below from its source film
    python3 tools/showcase/crop_films.py --tiles    # the front page gallery's tiles, docs/media/tile-*.gif
"""
import sys
from pathlib import Path
from PIL import GifImagePlugin, Image, ImageSequence

GifImagePlugin.LOADING_STRATEGY = GifImagePlugin.LoadingStrategy.RGB_AFTER_DIFFERENT_PALETTE_ONLY
ROOT = Path(__file__).resolve().parents[2]
M = ROOT / "docs" / "media"
# source film, crop box (left, top, right, bottom) in its pixels, output
CROPS = [
    (M / "lab/cylinder-mounted-3d.gif", (0, 84, 752, 760), M / "lab/cylinder-mounted-3d-view.gif"),
    (M / "lab/capsule-3d.gif", (0, 84, 752, 760), M / "lab/capsule-3d-view.gif"),
    (M / "lab/glass-bottle.gif", (0, 84, 752, 760), M / "lab/glass-bottle-view.gif"),
    (M / "lab/radar-aircraft.gif", (0, 84, 752, 760), M / "lab/radar-aircraft-view.gif"),
    (M / "lab/motor-3d.gif", (0, 84, 752, 760), M / "lab/motor-3d-view.gif"),
    (M / "lab/induction-gear.gif", (0, 84, 752, 760), M / "lab/induction-gear-view.gif"),
]


# the gallery's tiles: every film cut around its subject, padded with its own background to 4:3 and scaled to one size,
# so the grid's cells line up
TILE = (480, 360)
TILES = [
    (M / "showcase-polymer-print.gif", (185, 84, 725, 480), M / "tile-drone-print.gif"),
    (M / "showcase-flow.gif", (150, 84, 710, 480), M / "tile-sailplane.gif"),
    (M / "showcase-heat.gif", (114, 74, 654, 478), M / "tile-heat-sink.gif"),
    (M / "lab/water-surface.gif", (30, 160, 730, 685), M / "tile-dam-break.gif"),
    (M / "lab/loudspeaker.gif", (30, 140, 730, 665), M / "tile-loudspeaker.gif"),
    (M / "lab/rubber-sheet.gif", (70, 190, 690, 638), M / "tile-rubber-sheet.gif"),
    (M / "lab/motor-3d.gif", (30, 150, 730, 675), M / "tile-motor.gif"),
    (M / "lab/induction-gear.gif", (30, 150, 730, 675), M / "tile-induction.gif"),
    (M / "lab/black-hole.gif", (36, 40, 684, 352), M / "tile-black-hole.gif"),
    (M / "lab/shoebox-hall.gif", (140, 28, 660, 418), M / "tile-concert-hall.gif"),
    # the trials
    (M / "lab/cylinder-re105.gif", (90, 100, 690, 420), M / "tile-trial-shedding.gif"),
    (M / "lab/sphere-bowshock.gif", (0, 90, 752, 650), M / "tile-trial-bowshock.gif"),
    (M / "lab/helium-bubble.gif", (20, 36, 640, 400), M / "tile-trial-bubble.gif"),
    (M / "lab/planets.gif", (138, 190, 698, 610), M / "tile-trial-planets.gif"),
]


def tile(src, box, out):
    im = Image.open(src)
    frames, durs = [], []
    for fr in ImageSequence.Iterator(im):
        durs.append(fr.info.get("duration", 80))
        c = fr.convert("RGB").crop(box)
        w, h = c.size
        W = max(w, round(h * TILE[0] / TILE[1]))
        H = max(h, round(w * TILE[1] / TILE[0]))
        canvas = Image.new("RGB", (W, H), c.getpixel((2, 2)))   # padded with the film's own background
        canvas.paste(c, ((W - w) // 2, (H - h) // 2))
        q = canvas.resize(TILE, Image.LANCZOS).quantize(colors=255, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
        q.info = {}
        frames.append(q)
    frames[0].save(out, save_all=True, append_images=frames[1:], duration=durs, loop=0, optimize=False)
    print("%-36s %3d frames -> %s (%d KB)" % (src.name, len(frames), out.name, out.stat().st_size // 1024))


def crop(src, box, out):
    im = Image.open(src)
    frames, durs = [], []
    for fr in ImageSequence.Iterator(im):
        durs.append(fr.info.get("duration", 80))
        c = fr.crop(box)
        c.info = {}
        frames.append(c if c.mode == "P" else c.convert("RGB").quantize(colors=255, dither=Image.Dither.NONE))
    frames[0].save(out, save_all=True, append_images=frames[1:], duration=durs, loop=0, optimize=False)
    print("%-36s %3d frames -> %s (%d KB)" % (src.name, len(frames), out.name, out.stat().st_size // 1024))


if __name__ == "__main__":
    only = [a for a in sys.argv[1:] if a != "--tiles"]
    tiles = "--tiles" in sys.argv
    for src, box, out in (TILES if tiles else CROPS):
        if not only or any(o in src.name for o in only):
            (tile if tiles else crop)(src, box, out)
