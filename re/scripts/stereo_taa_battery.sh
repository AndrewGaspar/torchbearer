#!/usr/bin/env bash
# Per-eye temporal state battery (docs/stereo.md "Temporal state per eye"): the same captures under
# five sbs variants. Needs output=full (3840-wide swapchain), the game's TAA
# on in its options, a static scene (fixture prologue-road).
#   stereo_taa_battery.sh INSTANCE            -> frames in the run folder; stereo_taa_analyse.py
# Variants (tags):
#   off     stereo.taa=0 object_velocity=0       TAA + object velocity off in sbs (old default)
#   shared  taa=1 object_velocity=1 per_eye=0    TAA on, history / previous view shared by the eyes
#   pereye  taa=1 object_velocity=1 per_eye=1    TAA on, per-eye history, index, previous matrices
#   mbshared  taa=0 object_velocity=1 per_eye=0  motion blur only: previous view matrices shared
#   mbpereye  taa=0 object_velocity=1 per_eye=1  motion blur only: per-eye previous view matrices
# Shot counts include the swallowed left-eye pass (one _L file per _SBS file).
set -euo pipefail
inst=$1
here=$(dirname "$0")
export QUIVER_SOURCE=${QUIVER_SOURCE:-agent:torchbearer-taa}
cfg() { quiver lab call "$inst" cfg "stereo.$1" "$2" >/dev/null; }
shot() { "$here/stereo_shot.sh" "$inst" "$@" >/dev/null; }
cfg convergence 0
cfg test_pan 0
cfg separation 32.5
cfg mode sbs
variant() {  # TAG TAA OBJVEL PER_EYE
  local t=$1
  cfg taa "$2"; cfg object_velocity "$3"; cfg per_eye "$4"
  cfg test_pan 0
  cfg separation 0;    shot "${t}sep0" 4 90
  cfg separation 32.5; shot "${t}sep325" 4 90
  quiver lab call "$inst" stereo status | tr -d '\n ' > "$(quiver lab logs "$inst" | head -1)/status-$t.json" 2>/dev/null || true
  cfg test_pan 0.5
  cfg separation 0;    shot "${t}pan0" 4 30
  cfg separation 32.5; shot "${t}pan325" 4 30
  cfg test_pan 0
}
# VARIANTS="tag:taa:object_velocity:per_eye ..." overrides the list (e.g. DLSS runs:
# VARIANTS="dlssshared:1:1:0 dlssreset:1:1:1" with the game's DLSS on).
for v in ${VARIANTS:-off:0:0:1 shared:1:1:0 pereye:1:1:1 mbshared:0:1:0 mbpereye:0:1:1}; do
  IFS=: read -r t a o e <<< "$v"
  variant "$t" "$a" "$o" "$e"
done
cfg taa 1; cfg object_velocity 1; cfg per_eye 1
cfg separation 32.5
