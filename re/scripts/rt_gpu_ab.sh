#!/usr/bin/env bash
# GPU time of the engine's AO / shadow-buffer passes per variant (docs/raytracing.md), from the
# raytracing probe's timestamps (`rt gpu`, needs [dev] rt_capture = 1). Run under
# `quiver lab gpu --for INSTANCE --` so other instances are throttled.
#   rt_gpu_ab.sh INSTANCE [FRAMES=400] [VARIANTS="base ssao nohbao ssao+nohbao base"]
# Variants: base (the game's branches), ssao (SSAO forced on), nohbao (HBAO skipped), and joins.
set -euo pipefail
inst=$1 frames=${2:-400} variants=${3:-"base ssao nohbao ssao+nohbao base"}
export QUIVER_SOURCE=${QUIVER_SOURCE:-agent:torchbearer-rt}
call() { quiver lab call "$inst" "$@"; }
call rt gpu on >/dev/null
for v in $variants; do
  case $v in
    base) call rt flag ssao game >/dev/null; call rt flag hbao game >/dev/null ;;
    ssao) call rt flag ssao 1 >/dev/null; call rt flag hbao game >/dev/null ;;
    nohbao) call rt flag ssao game >/dev/null; call rt flag hbao 0 >/dev/null ;;
    ssao+nohbao) call rt flag ssao 1 >/dev/null; call rt flag hbao 0 >/dev/null ;;
    *) echo "unknown variant $v" >&2; exit 2 ;;
  esac
  call rt gpu reset >/dev/null
  t0=$(date +%s.%N)
  while :; do  # poll the probe's own frame counter (a real signal), not a fixed sleep
    s=$(call rt gpu status | tr -d '\n ')
    n=$(python3 -I -c 'import json,sys; print(json.loads(sys.argv[1])["frames"])' "$s")
    [[ $n -ge $frames ]] && break
  done
  t1=$(date +%s.%N)
  python3 -I -c '
import json, sys
s, v, dt = json.loads(sys.argv[1]), sys.argv[2], float(sys.argv[4]) - float(sys.argv[3])
p, n = s["passes"], s["frames"]
ms = "  ".join("%s %.3f" % (k, d["mean_ms"]) for k, d in p.items())
print("%-12s frames %d in %5.2fs (%6.1f fps)  %s" % (v, n, dt, n / dt, ms))' "$s" "$v" "$t0" "$t1"
done
call rt flag ssao game >/dev/null
call rt flag hbao game >/dev/null
call rt gpu off >/dev/null
