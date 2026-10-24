#!/bin/bash
# powertest.sh - can a node's power supply carry it? Every thread does the heaviest floating-point work there is
# (tools/monitor/loadtest.c: AVX2 fused multiply-adds held in registers) for 1 minute, then 15 minutes, then five
# times 30 s idle and 60 s full, the sudden jump that cut the first node's power on 2026-10-08. Watch it on the
# monitor (clock while busy, floating-point rate); a node that goes dark announces itself there. On the node:
#     python3 tools/remote.py run --name powertest -- 'bash tools/monitor/powertest.sh'
set -e
here=$(cd "$(dirname "$0")" && pwd)
n=${THREADS:-$(nproc)}
cc=$(command -v clang || command -v cc)
d=$(mktemp -d /tmp/powertest.XXXX)
"$cc" -O2 -mavx2 -mfma -o "$d/loadtest" "$here/loadtest.c"
run() { for i in $(seq 1 "$n"); do "$d/loadtest" "$1" > "$d/out.$i" & done; wait; }
echo "phase 0: $n threads for 60 s from $(date +%H:%M:%S)"; run 60
echo "phase 1, sustained: $n threads for 900 s from $(date +%H:%M:%S)"; run 900
for k in 1 2 3 4 5; do echo "phase 2, jump $k: 30 s idle, then $n threads for 60 s, from $(date +%H:%M:%S)"; sleep 30; run 60; done
echo "all phases done at $(date +%H:%M:%S); one thread's last count:"; cat "$d/out.1"
rm -rf "$d"
