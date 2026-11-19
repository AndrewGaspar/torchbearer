#!/usr/bin/env bash
# Capture N consecutive presented frames under the current stereo settings, after SKIP presents
# (let TAA/exposure settle after a change). Polls the mod's `stereo files` until they exist.
#   stereo_shot.sh INSTANCE TAG [N=1] [SKIP=60]
set -euo pipefail
inst=$1 tag=$2 n=${3:-1} skip=${4:-60}
export QUIVER_SOURCE=${QUIVER_SOURCE:-agent:torchbearer-stereo}
quiver lab call "$inst" stereo shots "$n" "$tag" "$skip" >/dev/null
for _ in $(seq 1 200); do
  got=$(quiver lab call "$inst" stereo files | grep -c "_${tag}_" || true)
  if (( got >= n )); then quiver lab call "$inst" stereo files | grep "_${tag}_" | tr -d ' ",' ; exit 0; fi
done
echo "capture $tag timed out" >&2
exit 1
