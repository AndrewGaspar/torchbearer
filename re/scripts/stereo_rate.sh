#!/usr/bin/env bash
# Renders/s (scene renders, i.e. eyes) and displayed frames (game updates)/s per stereo variant,
# from the mod's counters over a window of N status round trips. Run under
# `quiver lab gpu --for INSTANCE --` for numbers that compare.
#   stereo_rate.sh INSTANCE [N=40] [VARIANTS="off frame view view+skip off frame view view+skip"]
# Variants: off (mono), sbs (sbs with the current sbs_path), frame / view (sbs with that path),
# view+skip (view path, second eye reuses shadows and the sky probe), view+shadows, view+probe.
set -euo pipefail
inst=$1 n=${2:-40} variants=${3:-"off frame view view+skip off frame view view+skip"}
export QUIVER_SOURCE=${QUIVER_SOURCE:-agent:torchbearer-stereo}
st() { quiver lab call "$inst" stereo status | tr -d '\n '; }
cfg() { quiver lab call "$inst" cfg "stereo.$1" "$2" >/dev/null; }
for v in $variants; do
  sh=0 pr=0
  case $v in
    off) cfg mode off ;;
    sbs) cfg mode sbs ;;
    frame) cfg mode sbs; cfg sbs_path frame ;;
    view) cfg mode sbs; cfg sbs_path view ;;
    view+skip) cfg mode sbs; cfg sbs_path view; sh=1 pr=1 ;;
    view+shadows) cfg mode sbs; cfg sbs_path view; sh=1 ;;
    view+probe) cfg mode sbs; cfg sbs_path view; pr=1 ;;
    *) echo "unknown variant $v" >&2; exit 2 ;;
  esac
  cfg skip_shadows $sh
  cfg skip_probe $pr
  for _ in 1 2 3 4 5; do st >/dev/null; done  # settle
  a=$(st); t0=$(date +%s.%N)
  for _ in $(seq 1 "$n"); do st >/dev/null; done
  b=$(st); t1=$(date +%s.%N)
  python3 -I -c "
import json, sys
a, b, dt = json.loads(sys.argv[1]), json.loads(sys.argv[2]), float(sys.argv[4]) - float(sys.argv[3])
r = (b['render_frames'] - a['render_frames']) / dt
u = (b['sbs_updates'] - a['sbs_updates']) / dt
n = max(b['timed_updates'] - a['timed_updates'], 1)
ms = lambda k: (b[k] - a[k]) / n / 1000
print(f'{sys.argv[5]:12s} window {dt:5.2f}s  renders/s {r:6.1f}  displayed frames/s {u if u else r:6.1f}  '
      f'CPU ms/update: update {ms(\"us_update\"):5.2f} eye1 {ms(\"us_eye1\"):5.2f} eye2 {ms(\"us_eye2\"):5.2f} '
      f'endFrame {ms(\"us_endframe\"):5.2f}')
" "$a" "$b" "$t0" "$t1" "$v"
done
cfg skip_shadows 0
cfg skip_probe 0
cfg mode off
