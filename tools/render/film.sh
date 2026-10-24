#!/bin/bash
# film.sh: a physics-lab result as a beauty film, run on a compute node (tools/render/README.md).
#
#   bash tools/render/film.sh RESULT.lab OUTDIR [lab2ply options] [+ beauty.py options]
#
#   1. build/lab2ply (compiled here when it is missing) writes the result's surfaces to OUTDIR/ply
#   2. Blender (Cycles) renders OUTDIR/frames/frame_NNNN.png; frames already there are kept, so a stopped film resumes
#   3. ffmpeg encodes OUTDIR/film.mp4 (H.264, crf 18, FPS frames per second) and OUTDIR/film.gif (GIF_WIDTH px wide,
#      GIF_FPS frames per second, palettegen and paletteuse), both with a caption and the field's colour legend
#
# Environment: THREADS (6), FPS (30), GIF_FPS (12), GIF_WIDTH (720), CAPTION (the result's title), LEGEND (the field's
# name as the legend says it), CREDIT (the line at the top right), NO_FILM=1 (surfaces and frames only), FILM_ONLY=1 (the
# encoding only, from the frames there are).
# Example (the first film, 2026-10-21):
#   bash tools/render/film.sh $TMPDIR/beauty/helical_full/result.lab $TMPDIR/beauty/helical_film --field stress \
#       --cut -110 -10 --cut-parts ring,plate + --story 10
set -eu
cd "$(dirname "$0")/../.."
RESULT=$1 OUT=$2
shift 2
L2P=() BEAUTY=()
while [ $# -gt 0 ]; do
  if [ "$1" = "+" ]; then shift; BEAUTY=("$@"); break; fi
  L2P+=("$1"); shift
done
THREADS=${THREADS:-6} FPS=${FPS:-30} GIF_FPS=${GIF_FPS:-12} GIF_WIDTH=${GIF_WIDTH:-720}
mkdir -p "$OUT"
[ -x build/lab2ply ] || cc -O2 -std=c11 -Wall -Wextra -o build/lab2ply tools/render/lab2ply.c src/lab/labio.c -lm
if [ -z "${FILM_ONLY:-}" ] && [ ! -f "$OUT/ply/manifest.json" ]; then
  echo "== $(date '+%F %T') surfaces"
  nice -n 19 ./build/lab2ply "$RESULT" "$OUT/ply" "${L2P[@]}"
fi
[ -n "${FILM_ONLY:-}" ] || { echo "== $(date '+%F %T') frames"
nice -n 19 blender -b --factory-startup -noaudio -t "$THREADS" -P tools/render/beauty.py -- "$OUT/ply" "$OUT/frames" \
  --threads "$THREADS" "${BEAUTY[@]}" 2>&1 | { grep --line-buffered -E "^beauty.py|Error|Traceback|line [0-9]+" || true; }; }
[ -n "${NO_FILM:-}" ] && exit 0

echo "== $(date '+%F %T') film"
# the caption: the result's title, the field's legend with its range, and what is simulation and what is not; written as
# ffmpeg filter scripts for the full film and for the GIF, laid out in proportion to the width
NFR=$(ls "$OUT"/frames/frame_*.png | wc -l)
python3 - "$OUT" "$GIF_FPS" "$GIF_WIDTH" "$FPS" "$NFR" <<'PY'
import json, os, sys
out, gif_fps, gif_w, fps, nfr = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
# a short fade from and to the studio's grey at both ends, so that a looping GIF does not jump back
T = nfr / fps
fade = "fade=t=in:st=0:d=0.4:color=0xd8dade,fade=t=out:st=%.3f:d=0.6:color=0xd8dade" % max(0.0, T - 0.6)
m = json.load(open(os.path.join(out, "ply", "manifest.json")))
h = m.get("header", {})
unit = h.get("fields", {}).get(m["field"], "")
lo, hi = m["range"]
if unit == "Pa" and max(abs(lo), abs(hi)) >= 1e5: lo, hi, unit = lo / 1e6, hi / 1e6, "MPa"
name = os.environ.get("LEGEND") or m["field"]
texts = {"caption": os.environ.get("CAPTION") or h.get("title", ""), "name": name, "lo": "%.3g" % lo,
         "hi": "%.3g %s" % (hi, unit), "credit": os.environ.get("CREDIT") or
         "Geometry and %s computed by OpenPhysicsAI; light, materials and camera by Blender" % name}
for k, v in texts.items():
    open(os.path.join(out, "text_%s.txt" % k), "w").write(v)
font, ink = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "0x2b2f36"
def chain(w, head, tail="", grow=1.0):
    s = grow * w / 1920.0  # the layout in proportion to the width; a small picture gets larger type
    r = lambda v: max(1, round(v * s))
    m, big, small, bw, bh, yb, gap = r(56), r(34), r(22), r(300), max(4, r(12)), r(78), r(8)
    t = lambda k: os.path.join(out, "text_%s.txt" % k)
    dt = lambda k, size, x, y, alpha="": "drawtext=fontfile=%s:textfile=%s:fontsize=%d:fontcolor=%s%s:x=%s:y=%s" % (
        font, t(k), size, ink, alpha, x, y)
    bar_y = "H-%d-%d" % (yb, bh)
    parts = [dt("caption", big, m, "%s-%d-%d-%d" % (bar_y, gap, small, big + r(14))),
             dt("name", small, m, "%s-%d-%d" % (bar_y, gap, small)),
             dt("lo", small, m, "H-%d+%d" % (yb, gap)),
             dt("hi", small, "%d+%d-tw" % (m, bw), "H-%d+%d" % (yb, gap)),
             dt("credit", small, "w-tw-%d" % m, "%d" % r(44), "@0.75")]  # top right, over the studio's empty wall
    return "%s;[1:v]scale=%d:%d[lg];[v][lg]overlay=x=%d:y=%s,%s%s" % (head, bw, bh, m, bar_y, ",".join(parts), tail)
open(os.path.join(out, "filter_film.txt"), "w").write(chain(1920, "[0:v]null[v]", "," + fade))
open(os.path.join(out, "filter_gif.txt"), "w").write(chain(gif_w, "[0:v]fps=%d,scale=%d:-2:flags=lanczos[v]" % (gif_fps, gif_w),
    "," + fade + ",split[a][b];[a]palettegen=max_colors=192:stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle",
    grow=1.4))
PY
nice -n 19 ffmpeg -v error -y -framerate "$FPS" -i "$OUT/frames/frame_%04d.png" -i "$OUT/frames/legend.png" \
  -filter_complex_script "$OUT/filter_film.txt" -c:v libx264 -preset slow -crf 18 -pix_fmt yuv420p -movflags +faststart \
  -threads "$THREADS" "$OUT/film.mp4"
nice -n 19 ffmpeg -v error -y -framerate "$FPS" -i "$OUT/frames/frame_%04d.png" -i "$OUT/frames/legend.png" \
  -filter_complex_script "$OUT/filter_gif.txt" "$OUT/film.gif"
ls -la "$OUT/film.mp4" "$OUT/film.gif"
echo "== $(date '+%F %T') done"
