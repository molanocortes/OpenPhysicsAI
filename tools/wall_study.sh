#!/bin/sh
# Sphere drag against resolution for both model-wall treatments, at Re = 100.
#   tools/wall_study.sh [steps_per_diameter_cell] [tunnel_width_in_D] [D ...]
# Cd is averaged over the last 40% of each run, so give enough steps for the wake to settle (800 per cell of
# diameter is a reasonable minimum). The tunnel is W*D on a side, so blockage falls as W grows; both treatments
# share it, which makes the comparison between them fair even before a blockage correction.
set -e
cd "$(dirname "$0")/.."
SPD=${1:-800}
W=${2:-5}
[ $# -gt 2 ] && shift 2 || shift $#
DS=${*:-8 12 16 24}
printf 'sphere Re=100, %sD x %sD tunnel, %s steps per diameter cell, RR3 + bulk viscosity\n' "$W" "$W" "$SPD"
printf '%4s %8s %10s %10s\n' D steps staircase curved
for D in $DS; do
    S=$((SPD * D))
    st=$(LBM_TAUB=1 ./build/lbmbench sphere 100 "$D" "$S" rr 0 1 800 "$W" 2>&1 | sed -nE 's/mean Cd \(last 40%\) = ([0-9.]+).*/\1/p')
    cu=$(LBM_TAUB=1 LBM_CURVED=1 ./build/lbmbench sphere 100 "$D" "$S" rr 0 1 800 "$W" 2>&1 | sed -nE 's/mean Cd \(last 40%\) = ([0-9.]+).*/\1/p')
    printf '%4s %8s %10s %10s\n' "$D" "$S" "$st" "$cu"
done
