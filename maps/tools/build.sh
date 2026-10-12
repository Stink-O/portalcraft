#!/bin/bash
# Rebuild pc_pit_flings.bsp from Valve's sp_a2_pit_flings.bsp.
#   maps/tools/build.sh [--install]
# Output: maps/build/pc_pit_flings.bsp (and the game copy with --install).
set -eu
cd "$(dirname "$0")"
. ../../bridge/tools/portal2_dir.sh || exit 1   # P2 = the Portal 2 install (PORTAL2_DIR, else found through Steam)
ORIG="$P2/portal2/maps/sp_a2_pit_flings.bsp"
B=../build; G=$B/game
mkdir -p "$G"
[ -f "$G/gameinfo.txt" ] || python3 - "$P2" "$G/gameinfo.txt" <<'PY'
import sys
p2, out = sys.argv[1], sys.argv[2]
open(out, "w").write('''"GameInfo"
{
	game	"PORTAL 2"
	title	"PORTAL 2"
	GameData	"portal2.fgd"
	FileSystem
	{
		SteamAppId	620
		ToolsAppId	211
		SearchPaths
		{
			Game	|gameinfo_path|.
			Game	"%s/update"
			Game	"%s/portal2_dlc2"
			Game	"%s/portal2_dlc1"
			Game	"%s/portal2"
		}
	}
}
''' % (p2, p2, p2, p2))
PY
python3 bsp2vmf.py "$ORIG" "$B/sp_a2_pit_flings_decomp.vmf" --report "$B/decomp_report.json" > /dev/null
python3 add_region.py "$B/sp_a2_pit_flings_decomp.vmf" "$G/pc_pit_flings.vmf"
./compile.sh pc_pit_flings all
for s in vbsp vvis vrad; do
  if grep -v "was never closed" "$G/pc_pit_flings.$s.txt" | grep -i -E "leak|error" ; then echo "!! $s reported problems"; fi
done
python3 relight.py "$G/pc_pit_flings.bsp" "$ORIG" "$B/pc_pit_flings.relit.bsp"
python3 pack.py "$B/pc_pit_flings.relit.bsp" "$ORIG" "$B/pc_pit_flings.bsp" --alias-script pc_map_alias.nut --compiled-name pc_pit_flings
if [ "${1:-}" = "--install" ]; then
  if pgrep -x portal2_linux >/dev/null; then echo "Portal 2 is running; not installing"; exit 1; fi
  cp "$B/pc_pit_flings.bsp" "$P2/portal2/maps/pc_pit_flings.bsp"
  echo "installed $P2/portal2/maps/pc_pit_flings.bsp"
fi
