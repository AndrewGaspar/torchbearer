#!/usr/bin/env bash
# Screenshots of one scene per ambient-occlusion variant (docs/raytracing.md): the game's (HBAO),
# SSAO forced with HBAO kept (HBAO's apply overwrites it), SSAO instead of HBAO, no AO, and the
# game's again (drift control). Needs [dev] rt_capture = 1 (the `rt flag` verb).
#   rt_ao_shots.sh INSTANCE OUT_DIR TAG
set -euo pipefail
inst=$1 out=$2 tag=$3
export QUIVER_SOURCE=${QUIVER_SOURCE:-agent:torchbearer-rt}
mkdir -p "$out"
flag() { quiver lab call "$inst" rt flag "$1" "$2" >/dev/null; }
shot() { echo "$1: $(quiver lab shot "$inst" "$out/$tag-$1.png" --settle 2 --metrics | tail -1)"; }
flag ssao game; flag hbao game; shot hbao
flag ssao 1; shot ssao+hbao
flag hbao 0; shot ssao
flag ssao game; shot none
flag hbao game; shot hbao2
