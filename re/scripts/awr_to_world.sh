#!/usr/bin/env bash
# From a fresh launch to in-world on the saved checkpoint (fixture prologue-road): press Return
# through the warning and the title until the main menu (the lighthouse, mean luma > 60, with
# "Continue Game" selected) shows, then Return once more and wait for the mod's in-world state.
# The title screen is flaky in the lab: sometimes a key press there leads back to the title or
# into the attract video; the script gives up after a while (relaunch and retry).
#   awr_to_world.sh INSTANCE [SCRATCH_DIR]
set -euo pipefail
inst=$1 dir=${2:-/tmp}
export QUIVER_SOURCE=${QUIVER_SOURCE:-agent:torchbearer-stereo}
mean() { python3 -I -c "
from PIL import Image; import numpy as np, sys
a = np.asarray(Image.open(sys.argv[1]).convert('L')).astype(float)
print(int(a.mean()), int(a[650:900, 540:800].mean()))" "$1"; }
for _ in $(seq 1 12); do
  quiver lab wait "$inst" frame --stable --timeout 60 >/dev/null 2>&1 || true
  quiver lab shot "$inst" "$dir/nav.png" >/dev/null
  read -r m menu < <(mean "$dir/nav.png")
  echo "screen mean $m (menu area $menu)"
  if (( m > 60 && m < 85 )); then
    quiver lab key "$inst" Return
    quiver lab wait "$inst" in-world --timeout 300
    exit 0
  fi
  quiver lab key "$inst" Return
done
echo "did not reach the main menu" >&2
exit 1
