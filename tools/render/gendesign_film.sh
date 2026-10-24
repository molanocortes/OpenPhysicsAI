#!/bin/bash
# gendesign_film.sh: the generative-design film from a finished `build/gendesign run`, on a compute node with Blender
# (tools/render/README.md, "Generative design").
#
#   bash tools/render/gendesign_film.sh RUNDIR FILMDIR [topofilm.py options]
#
#   1. build/gendesign ply --check-only: every surface the film shows (the design space, the iterations: every one up to
#      DENSE_TO, then every SPARSE-th, and the last; the final design with its stress) is built once and checked for
#      open, non-manifold and misoriented edges; the film stops if one has any. Only FILMDIR/ply/manifest.json is kept
#      (a manifest made elsewhere, on the node that ran the optimisation, may be put there instead)
#   2. Blender (Cycles, tools/render/topofilm.py) renders FILMDIR/frame_NNNN.png, building each surface from the run's
#      densities just before its first frame and deleting it after its last (a surface at 0.8 mm voxels is 10 to 35 MB);
#      frames already there are kept, so a stopped film resumes
#   3. tools/render/topoencode.py: FILMDIR/generative_design.mp4 (H.264), .gif and _still.png, captioned from the run's
#      own numbers
#
# Environment: THREADS (4), DENSE_TO (the iteration where the run reached its volume, plus 20), SPARSE (4), TAUBIN
# (smoothing iterations of the surfaces, 20), NO_FILM=1 (frames only), FILM_ONLY=1 (the encoding only), KEEP_PLY=1 (keep
# the surfaces instead of deleting each after its last frame).
set -eu
cd "$(dirname "$0")/../.."
RUN=$1 FILM=$2
shift 2
THREADS=${THREADS:-4} SPARSE=${SPARSE:-4}
if [ -z "${DENSE_TO:-}" ]; then
  reached=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['target_reached_at'])" "$RUN/summary.json")
  DENSE_TO=$((reached + 20))
fi
mkdir -p "$FILM"
[ -x build/gendesign ] || nice -n 19 make -j"$THREADS" build/gendesign
if [ -z "${FILM_ONLY:-}" ]; then
  if [ ! -f "$FILM/ply/manifest.json" ]; then
    echo "== $(date '+%F %T') surfaces checked (dense to $DENSE_TO, then every $SPARSE)"
    nice -n 19 ./build/gendesign ply "$RUN" "$FILM/ply" --domain --final --dense-to "$DENSE_TO" --sparse "$SPARSE" \
      --taubin "${TAUBIN:-20}" --check-only > "$FILM/ply.log"
    tail -1 "$FILM/ply.log"
  fi
  echo "== $(date '+%F %T') frames"
  PRUNE=--prune
  [ -z "${KEEP_PLY:-}" ] || PRUNE=
  nice -n 19 blender -b --factory-startup -noaudio -t "$THREADS" -P tools/render/topofilm.py -- "$FILM/ply" "$FILM" \
    --threads "$THREADS" --dense-to "$DENSE_TO" --sparse "$SPARSE" --run "$RUN" $PRUNE "$@" 2>&1 | \
    { grep --line-buffered -E "^topofilm.py|Error|Traceback|line [0-9]+" || true; }
  n=$(python3 -c "import json,sys; print(len(json.load(open(sys.argv[1]))['frames']))" "$FILM/shots.json")
  have=$(ls "$FILM"/frame_*.png 2>/dev/null | wc -l)
  [ "$have" -ge "$n" ] || { echo "only $have of $n frames rendered"; exit 1; }
fi
[ -n "${NO_FILM:-}" ] && exit 0
echo "== $(date '+%F %T') film"
nice -n 19 python3 tools/render/topoencode.py "$RUN" "$FILM/ply" "$FILM" --threads "$THREADS"
echo "== $(date '+%F %T') done"
