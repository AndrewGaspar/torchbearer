#!/usr/bin/env bash
# The sbs measurement battery (docs/stereo.md): every pair is the two halves of ONE composed
# frame, i.e. both eyes from the same game update. Needs output=full (3840-wide swapchain).
#   stereo_battery.sh INSTANCE
set -euo pipefail
inst=$1
here=$(dirname "$0")
export QUIVER_SOURCE=${QUIVER_SOURCE:-agent:torchbearer-stereo}
cfg() { quiver lab call "$inst" cfg "stereo.$1" "$2" >/dev/null; }
cfg convergence 0
cfg test_pan 0
cfg mode sbs
cfg separation 0;     "$here/stereo_shot.sh" "$inst" sep0 2 60
cfg separation 16.25; "$here/stereo_shot.sh" "$inst" sep1625 2 60
cfg separation 32.5;  "$here/stereo_shot.sh" "$inst" sep325 2 60
cfg separation 65;    "$here/stereo_shot.sh" "$inst" sep650 2 60
cfg separation 32.5
cfg convergence 4;    "$here/stereo_shot.sh" "$inst" conv4 2 60
cfg convergence 0
cfg test_pan 0.5;     "$here/stereo_shot.sh" "$inst" pansbs 4 60
# alternate (frame-sequential) for comparison, with the same temporal settings sbs forces
quiver lab call "$inst" tweak "TAA:Resolve" 0 >/dev/null
quiver lab call "$inst" tweak "TAA:Enable Object Velocity" 0 >/dev/null
cfg mode alternate;   "$here/stereo_shot.sh" "$inst" panalt 4 60
cfg test_pan 0
cfg separation 0;     "$here/stereo_shot.sh" "$inst" alt0 4 60
cfg separation 32.5
quiver lab call "$inst" tweak "TAA:Resolve" 1 >/dev/null
quiver lab call "$inst" tweak "TAA:Enable Object Velocity" 1 >/dev/null
cfg mode off;         "$here/stereo_shot.sh" "$inst" mono 2 60
