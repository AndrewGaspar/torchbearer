#!/usr/bin/env bash
# Per-eye DLSS battery (docs/stereo.md section 5 "DLSS"): per variant (game DLSS mode, stereo.taa,
# stereo.dlss_per_eye) the captures of stereo_taa_battery.sh (sep0, sep325, pan0, pan325) and a mono
# capture (mode off). Analyse with stereo_taa_analyse.py --variants <tags> [--eye-width 960].
#   stereo_dlss_battery.sh INSTANCE ["tag:dlss:taa:per_eye ..."]
# Captures are moved out of the run folder into $DEST (default: the run folder; set it when the
# lab disk is tight). Tags must be new for the run (stereo_shot.sh counts the mod's file list).
set -euo pipefail
inst=$1
vars=${2:-"ref:off:0:1 dq:quality:1:1 dqr:quality:1:0 dp:performance:1:1 dpr:performance:1:0"}
here=$(dirname "$0")
export QUIVER_SOURCE=${QUIVER_SOURCE:-agent:torchbearer-dlss}
c() { for _ in 1 2 3; do quiver lab call "$inst" cfg "$1" "$2" >/dev/null && return 0; done; return 1; }
shot() { "$here/stereo_shot.sh" "$inst" "$@" >/dev/null; }
run=$(quiver lab logs "$inst" 2>/dev/null || true); run=${run%%$'\n'*}
dest=${DEST:-$run}
mkdir -p "$dest"
park() {  # captures finished at least $1 s ago
  [[ $dest == "$run" ]] && return 0
  find "$run" -maxdepth 1 -name '*.bmp' ! -newermt "$(date -d "${1:-8} seconds ago" +%Y-%m-%dT%H:%M:%S)" \
    -exec mv {} "$dest/" \;
}
c stereo.convergence 0
c stereo.test_pan 0
c stereo.separation 32.5
for v in $vars; do
  park
  IFS=: read -r t dl taa pe <<< "$v"
  c stereo.mode sbs
  c game.dlss "$dl"
  if [[ $dl == off ]]; then c game.taa 1; fi
  c stereo.taa "$taa"; c stereo.object_velocity "$taa"; c stereo.dlss_per_eye "$pe"
  c stereo.separation 0;    shot "${t}sep0" 2 240
  c stereo.separation 32.5; shot "${t}sep325" 2 120
  quiver lab call "$inst" stereo status | tr -d '\n ' > "$dest/status-$t.json" || true
  c stereo.test_pan 0.5
  c stereo.separation 0;    shot "${t}pan0" 2 60
  c stereo.separation 32.5; shot "${t}pan325" 2 60
  c stereo.test_pan 0
  c stereo.mode off; shot "${t}mono" 2 120; c stereo.mode sbs
  echo "$t done"
done
c stereo.dlss_per_eye 1; c stereo.taa 1; c stereo.object_velocity 1
