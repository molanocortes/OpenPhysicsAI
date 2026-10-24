#!/bin/bash
# printfilm.sh: the metal printing film from a build's results.nvt, run on a compute node (tools/render/README.md,
# "The metal printing film").
#
#   bash tools/render/printfilm.sh SCENARIO.json BUILDDIR OUTDIR [+ printfilm.py options]
#
#   BUILDDIR is what tools/render/printbuild.py wrote (results.nvt, record.json, summary.json).
#   1. build/nvt2ply (compiled here when missing) writes the surfaces of every stored time to OUTDIR/ply, with the states
#      between the last layer and each stored state after it (the cut) blended over CUT_FRAMES, displacement x SCALE
#   2. tools/render/nvt2ply_check.py checks every state against the .nvt (OUTDIR/check.txt); a failure stops the film
#   3. Blender renders OUTDIR/frames (tools/render/printfilm.py); frames already there are kept, so a stopped film resumes
#   4. ffmpeg encodes OUTDIR/film_1080p.mp4 (H.264, crf 18) and OUTDIR/film.gif (GIF_WIDTH px, GIF_FPS), each with the
#      captions: the title and the layer, the legend and its range, the factor, and what is computed and what is not
#
# Environment: SCALE (the scenario's film.scale), CUT_FRAMES (60), THREADS (4), FPS (30), GIF_FPS (10), GIF_WIDTH (800),
# GIF_FROM / GIF_TO (seconds of the film the GIF holds; all), NO_FILM=1 (surfaces and frames only), FILM_ONLY=1 (the
# encoding only), KEEP_PLY=1 (keep the surfaces; by default they are deleted once the frames exist).
set -euo pipefail  # pipefail: a failed check or render stops the film even though its output goes through tee or grep
cd "$(dirname "$0")/../.."
SCEN=$1 BUILD=$2 OUT=$3
shift 3
[ "${1:-}" = "+" ] && shift
THREADS=${THREADS:-4} FPS=${FPS:-30} GIF_FPS=${GIF_FPS:-10} GIF_WIDTH=${GIF_WIDTH:-800} CUT_FRAMES=${CUT_FRAMES:-60}
SCALE=${SCALE:-$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['film']['scale'])" "$SCEN")}
# the plate the scenario asks for (its margins around the part and its thickness, mm); options after + come later and win
PLATE=$(python3 -c "import json,sys; f=json.load(open(sys.argv[1])).get('film', {}); m=f.get('plate_margin_mm', [6, 13]); print(m[0], m[1], f.get('plate_thickness_mm', 12))" "$SCEN")
mkdir -p "$OUT"
[ -x build/nvt2ply ] || cc -O2 -std=c11 -D_GNU_SOURCE -Wall -Wextra -o build/nvt2ply tools/render/nvt2ply.c src/core/json.c src/core/sbuf.c -lm
if [ -z "${FILM_ONLY:-}" ] && [ ! -f "$OUT/ply/manifest.json" ]; then
  echo "== $(date '+%F %T') surfaces"
  # the stored states after the build (the cut, and a support removal when there is one): blends from the one before
  BL=$(python3 - "$BUILD/record.json" "$CUT_FRAMES" <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))
L, n, K = r["layers"], r["stored_times"], int(sys.argv[2])
print(" ".join("--blend %d %d %d" % (a, a + 1, K) for a in range(L - 1, n - 1)))
PY
)
  nice -n 19 ./build/nvt2ply "$BUILD/results.nvt" "$OUT/ply" --scale "$SCALE" $BL
  echo "== $(date '+%F %T') check"
  nice -n 19 python3 tools/render/nvt2ply_check.py "$BUILD/results.nvt" "$OUT/ply" | tee "$OUT/check.txt"
fi
[ -n "${FILM_ONLY:-}" ] || { echo "== $(date '+%F %T') frames"
nice -n 19 blender -b --factory-startup -noaudio -t "$THREADS" -P tools/render/printfilm.py -- "$OUT/ply" "$OUT/frames" \
  --threads "$THREADS" --fps "$FPS" --cut-frames "$CUT_FRAMES" --plate $PLATE "$@" 2>&1 | { grep --line-buffered -E "^printfilm.py|^beauty.py|Error|Traceback|line [0-9]+" || true; }; }
[ -n "${NO_FILM:-}" ] && exit 0

echo "== $(date '+%F %T') film"
# the captions, timed from the film's own timeline: written as ffmpeg filter scripts for the film and for the GIF
python3 - "$SCEN" "$BUILD" "$OUT" "$FPS" "$GIF_FPS" "$GIF_WIDTH" "${GIF_FROM:-0}" "${GIF_TO:-0}" <<'PY'
import json, os, sys
scen, build, out = json.load(open(sys.argv[1])), sys.argv[2], sys.argv[3]
fps, gif_fps, gif_w, g0, g1 = int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6]), float(sys.argv[7]), float(sys.argv[8])
rec = json.load(open(os.path.join(build, "record.json")))
tl = json.load(open(os.path.join(out, "frames", "timeline.json")))
rows = tl["rows"]
first = lambda ph: next(r["time_s"] for r in rows if r["phase"] == ph)
last = lambda ph: max(r["time_s"] for r in rows if r["phase"] == ph) + 1.0 / fps
t_build, t_cut, t_turn, T = first("build"), first("cut"), first("turntable"), len(rows) / fps
lo, hi = tl["range_pa"]
L = tl["layers"]
per = sum(1 for r in rows if r["phase"] == "build") / L / fps  # seconds per layer
pr = {p["name"]: p for p in rec.get("probes", [])}
end = pr.get(scen.get("film", {}).get("probe", "ridge 6"))
film = scen.get("film", {})
texts = {
    "title": film.get("title") or scen["film"]["caption"],
    "build_note": "%d layers of %g mm, each shrinking on the part below it" % (L, scen["build"]["layer_thickness_mm"]),
    "cut": "cut from the plate: the bridge springs up",
    "cut_note": "the frames between the two computed states are interpolated",
    "scale": "distortion drawn %g times larger" % tl["scale"],
    "name": "von Mises stress, elastic", "lo": "0", "hi": "%.0f MPa" % (hi / 1e6),
    "credit": "computed by OpenPhysicsAI; light, materials, camera: Blender",
}
prov = scen.get("inherent_strain", {}).get("provenance", "")
if prov not in ("calibrated", "published", "measured"):  # a strain no measurement stands behind is said to be one
    texts["build_note"] += "; inherent strain %s, a demonstration value" % (prov or "without a source")
if end and "measured_mm" in end:
    texts["result"] = "free end: computed %.2f mm, NIST measured %.2f mm (%+.0f %%)" % (
        end["deflection_mm"], end["measured_mm"], end["this_run_against_measured_percent"])
    texts["result_note"] = "strain fitted on another bridge (IN625, 2018); blind median error %.0f %%, criterion %d %%" % (
        scen["record"]["median_absolute_error_percent"], scen["record"]["criterion_percent"])
# the layer counter, an expression drawtext expands every frame (in a text file, so no filter escaping): film time t,
# and the GIF's time, which starts at GIF_FROM
for k, off in (("layer_film", 0.0), ("layer_gif", g0)):
    texts[k] = "layer %%{eif:clip(floor((t+%.4f-%.4f)/%.6f)+1,1,%d):d} of %d" % (off, t_build, per, L, L)
for k, v in texts.items():  # drawtext expands %{...} in every text, and a backslash takes the next character as it is
    open(os.path.join(out, "text_%s.txt" % k), "w").write(v if k.startswith("layer_") else v.replace("\\", "\\\\").replace("%", "\\%"))
font, bold, ink = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "0x23272e"
fade = "fade=t=in:st=0:d=0.5:color=0xd5d7db,fade=t=out:st=%.3f:d=0.7:color=0xd5d7db" % max(0.0, T - 0.7)
def chain(w, head, tail="", grow=1.0, t_off=0.0):
    s = grow * w / 1920.0
    r = lambda v: max(1, round(v * s))
    m, big, mid, small, bw, bh = r(56), r(40), r(30), r(24), r(340), max(4, r(13))
    t = lambda k: os.path.join(out, "text_%s.txt" % k)
    def dt(k, size, x, y, alpha="", en=None, f=font):
        e = "" if en is None else ":enable='between(t,%.3f,%.3f)'" % (en[0] - t_off, en[1] - t_off)
        return "drawtext=fontfile=%s:textfile=%s:fontsize=%d:fontcolor=%s%s:x=%s:y=%s%s" % (f, t(k), size, ink, alpha, x, y, e)
    def dl(size, x, y, en):
        return dt("layer_gif" if t_off > 0 else "layer_film", size, x, y, en=en)
    bar_y = "H-%d-%d" % (r(70), bh)
    y1, y2 = r(44) + big + r(14), r(44) + big + mid + r(26)  # the phase line and its note under the title
    parts = [dt("title", big, m, r(44), f=bold),
             dl(mid, m, y1, (t_build, t_cut)),
             dt("build_note", small, m, y2, "@0.85", en=(t_build, t_cut)),
             dt("cut", mid, m, y1, en=(t_cut, t_turn)),
             dt("cut_note", small, m, y2, "@0.85", en=(t_cut, t_turn)),
             dt("name", small, m, "%s-%d-%d" % (bar_y, r(10), small)),
             dt("lo", small, m, "H-%d+%d" % (r(70), r(8))),
             dt("hi", small, "%d+%d-tw" % (m, bw), "H-%d+%d" % (r(70), r(8))),
             dt("scale", small, "w-tw-%d" % m, "H-%d-%d" % (r(70), r(10) + small)),
             dt("credit", small, "w-tw-%d" % m, "H-%d+%d" % (r(70), r(8)), "@0.8")]
    if "result" in texts:
        parts += [dt("result", mid, m, y1, en=(t_turn, T + 1)), dt("result_note", small, m, y2, "@0.85", en=(t_turn, T + 1))]
    else:
        parts += [dt("scale", mid, m, y1, en=(t_turn, T + 1))]
    return "%s;[1:v]scale=%d:%d[lg];[v][lg]overlay=x=%d:y=%s,%s%s" % (head, bw, bh, m, bar_y, ",".join(parts), tail)
open(os.path.join(out, "filter_film.txt"), "w").write(chain(1920, "[0:v]null[v]", "," + fade))
gtrim = "trim=start=%.3f%s,setpts=PTS-STARTPTS," % (g0, (":end=%.3f" % g1) if g1 > 0 else "") if (g0 > 0 or g1 > 0) else ""
open(os.path.join(out, "filter_gif.txt"), "w").write(chain(gif_w, "[0:v]%sfps=%d,scale=%d:-2:flags=lanczos[v]" % (gtrim, gif_fps, gif_w),
    ",split[a][b];[a]palettegen=max_colors=128:stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=3:diff_mode=rectangle",
    grow=1.5, t_off=g0))
print("captions: build from %.2f s, cut from %.2f s, turntable from %.2f s, %.2f s in all" % (t_build, t_cut, t_turn, T))
PY
nice -n 19 ffmpeg -v error -y -framerate "$FPS" -i "$OUT/frames/frame_%04d.png" -i "$OUT/frames/legend.png" \
  -filter_complex_script "$OUT/filter_film.txt" -c:v libx264 -preset slow -crf 18 -pix_fmt yuv420p -movflags +faststart \
  -threads "$THREADS" "$OUT/film_1080p.mp4"
nice -n 19 ffmpeg -v error -y -framerate "$FPS" -i "$OUT/frames/frame_%04d.png" -i "$OUT/frames/legend.png" \
  -filter_complex_script "$OUT/filter_gif.txt" "$OUT/film.gif"
ls -la "$OUT/film_1080p.mp4" "$OUT/film.gif"
if [ -z "${KEEP_PLY:-}" ] && [ -f "$OUT/ply/manifest.json" ]; then rm -rf "$OUT/ply"/state_*; echo "surfaces deleted (the manifest kept)"; fi
echo "== $(date '+%F %T') done"
