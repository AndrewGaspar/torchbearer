#!/usr/bin/env bash
# Renders/s and displayed frames (game updates)/s per stereo mode, from the mod's counters over a
# window of N status round trips. Run under `quiver lab gpu --` for numbers that compare.
#   stereo_rate.sh INSTANCE [N=40] [MODES="off sbs off sbs"]
set -euo pipefail
inst=$1 n=${2:-40} modes=${3:-"off sbs off sbs"}
export QUIVER_SOURCE=${QUIVER_SOURCE:-agent:torchbearer-stereo}
st() { quiver lab call "$inst" stereo status | tr -d '\n '; }
for m in $modes; do
  quiver lab call "$inst" cfg stereo.mode "$m" >/dev/null
  for _ in 1 2 3 4 5; do st >/dev/null; done  # settle
  a=$(st); t0=$(date +%s.%N)
  for _ in $(seq 1 "$n"); do st >/dev/null; done
  b=$(st); t1=$(date +%s.%N)
  python3 -I -c "
import json, sys
a, b, dt = json.loads(sys.argv[1]), json.loads(sys.argv[2]), float(sys.argv[4]) - float(sys.argv[3])
r = (b['render_frames'] - a['render_frames']) / dt
u = (b['sbs_updates'] - a['sbs_updates']) / dt
print(f'{sys.argv[5]:4s} window {dt:5.2f}s  renders/s {r:6.1f}  displayed frames/s {u if u else r:6.1f}')
" "$a" "$b" "$t0" "$t1" "$m"
done
quiver lab call "$inst" cfg stereo.mode off >/dev/null
