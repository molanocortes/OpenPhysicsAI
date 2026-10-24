#!/usr/bin/env python3
"""hero_collage.py - the front page's opening picture: seven of the lab's films in one animated bento.

Each tile is a film of docs/media/lab/, made by the native app from a solver's own output, cropped to its subject and
scaled; nothing inside a tile is retouched. The tiles play together in one loop (each film stretched or compressed to
the loop's length), with a short label on a soft shade at the bottom. The gaps and the outer corners are transparent,
so the page's own colour shows between the tiles in the light and the dark theme alike.

    python3 tools/showcase/hero_collage.py            # writes docs/media/lab-hero.webp, about three minutes
    python3 tools/showcase/hero_collage.py --frame    # one still in build/lab-hero-check.png, to check the crops

Needs Pillow with WebP; the labels use the system's SF Pro (macOS), else Helvetica Neue, else Pillow's own font.
"""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont, ImageSequence

ROOT = Path(__file__).resolve().parents[2]
LAB = ROOT / "docs" / "media" / "lab"
OUT = ROOT / "docs" / "media" / "lab-hero.webp"

W, H, GAP, RADIUS = 1280, 720, 8, 14
FRAMES, FRAME_MS = 96, 84            # an eight-second loop
# film, crop box in the film's pixels (left, top, right, bottom), tile (x, y, w, h), label
TILES = [
    ("heart-valve.gif",       (40, 60, 680, 540),   (0, 0, 636, 477),     "A heart valve opens and closes"),
    ("fire.gif",              (186, 30, 574, 620),  (644, 0, 314, 477),   "A methane fire"),
    ("capsule-3d-view.gif",   (140, 120, 700, 539), (966, 0, 314, 235),   "A capsule at Mach 6"),
    ("loudspeaker.gif",       (60, 134, 700, 614),  (966, 243, 314, 234), "Sound fills a room"),
    ("glass-bottle-view.gif", (60, 200, 600, 604),  (0, 485, 314, 235),   "A bottle shatters"),
    ("black-hole.gif",        (36, 76, 684, 316),   (322, 485, 636, 235), "Light around a black hole"),
    ("flag-3d.gif",           (140, 90, 740, 539),  (966, 485, 314, 235), "A flag in the wind"),
]


def label_font(size):
    for path, variation in (("/System/Library/Fonts/SFNS.ttf", "Semibold"), ("/System/Library/Fonts/HelveticaNeue.ttc", None)):
        try:
            f = ImageFont.truetype(path, size, index=10 if path.endswith(".ttc") else 0)
            if variation:
                f.set_variation_by_name(variation)
            return f
        except (OSError, ValueError):
            continue
    return ImageFont.load_default()


def load(name, box, size):
    im = Image.open(LAB / name)
    frames = []
    for fr in ImageSequence.Iterator(im):
        frames.append(fr.convert("RGB").crop(box).resize(size, Image.LANCZOS))
    return frames


def shade(w, h, height=86, strength=150):
    """A soft dark ramp at the bottom of a tile, so a white label reads on any picture."""
    g = Image.new("L", (1, height))
    for y in range(height):
        g.putpixel((0, y), int(strength * (y / (height - 1)) ** 1.6))
    return g.resize((w, height))


def main():
    font = label_font(19)
    tiles = []
    for name, box, (x, y, w, h), text in TILES:
        frames = load(name, box, (w, h))
        mask = Image.new("L", (w, h), 0)
        ImageDraw.Draw(mask).rounded_rectangle((0, 0, w - 1, h - 1), RADIUS, fill=255)
        tiles.append((frames, (x, y, w, h), text, mask, shade(w, h)))
        print("%-24s %3d frames, tile %dx%d" % (name, len(frames), w, h))
    only = [FRAMES // 2] if "--frame" in sys.argv else range(FRAMES)   # --frame: one still to check the crops, no film
    out = []
    for k in only:
        canvas = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        for frames, (x, y, w, h), text, mask, sh in tiles:
            tile = frames[k * len(frames) // FRAMES].copy()
            dark = Image.new("RGB", (w, sh.size[1]), (0, 0, 0))
            tile.paste(dark, (0, h - sh.size[1]), sh)
            d = ImageDraw.Draw(tile)
            d.text((16, h - 16), text, font=font, fill=(245, 245, 247), anchor="ls")
            canvas.paste(tile, (x, y), mask)
        out.append(canvas)
    out[len(out) // 2].save(ROOT / "build" / "lab-hero-check.png")
    if len(out) == 1:
        print("wrote build/lab-hero-check.png")
        return 0
    out[0].save(OUT, save_all=True, append_images=out[1:], duration=FRAME_MS, loop=0, quality=78, method=6)
    print("wrote %s, %.1f MB, %d frames of %d ms" % (OUT.relative_to(ROOT), OUT.stat().st_size / 1e6, FRAMES, FRAME_MS))


if __name__ == "__main__":
    sys.exit(main())
