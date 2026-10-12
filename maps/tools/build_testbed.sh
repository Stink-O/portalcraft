#!/bin/bash
# Build pc_testbed.bsp (bridge #29): generate the VMF + layout JSON, vbsp/vvis/vrad, add the alias script.
#   maps/tools/build_testbed.sh [--install]
# Output: maps/build/pc_testbed.bsp, maps/build/pc_testbed_layout.json (also copied to bridge/tools/cfg/).
# --install also copies the bsp into the game (refuses while Portal 2 runs).
set -eu
cd "$(dirname "$0")"
. ../../bridge/tools/portal2_dir.sh || exit 1   # P2 = the Portal 2 install (PORTAL2_DIR, else found through Steam)
B=../build; G=$B/game
mkdir -p "$G"
if [ ! -f "$G/gameinfo.txt" ]; then
  echo "run maps/tools/build.sh once first (it writes $G/gameinfo.txt)"; exit 1
fi
# vbsp finds instances/p2editor/*.vmf here (a link to Valve's sdk_content; nothing is copied)
ln -sfn "$P2/sdk_content/maps/instances" "$G/instances"
python3 gen_testbed.py "$G/pc_testbed.vmf" "$B/pc_testbed_layout.json"
cp "$B/pc_testbed_layout.json" ../../bridge/tools/cfg/pc_testbed_layout.json
./compile.sh pc_testbed all
for s in vbsp vvis vrad; do
  if grep -v "was never closed" "$G/pc_testbed.$s.txt" | grep -i -E "leak|error" ; then echo "!! $s reported problems"; fi
done
python3 pack_testbed.py "$G/pc_testbed.bsp" "$B/pc_testbed.bsp" --alias-script pc_map_alias.nut
if [ "${1:-}" = "--install" ]; then
  if pgrep -x portal2_linux >/dev/null; then echo "Portal 2 is running; not installing"; exit 1; fi
  cp "$B/pc_testbed.bsp" "$P2/portal2/maps/pc_testbed.bsp"
  echo "installed $P2/portal2/maps/pc_testbed.bsp"
fi
