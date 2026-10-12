#!/bin/bash
# Compile a VMF with Portal 2's own Linux tools.
#   compile.sh <name> [vbsp|vvis|vrad|all] [extra vrad args]
# The VMF must be at maps/build/game/<name>.vmf. Output: maps/build/game/<name>.bsp
# Notes:
#  - The 2026 Linux tools refuse to write outside their -game directory, so
#    maps/build/game/ is a tiny "game" whose gameinfo.txt mounts Portal 2's
#    content read-only (see gameinfo.txt).
#  - PORTAL2_NO_LAUNCHERMGR_REQUIRED_IN_MATERIAL_SYSTEM=1 lets the material
#    system start without the game launcher's SDL manager (vbsp/vrad abort
#    with "Could not start the empty shader" otherwise).
set -eu
NAME="$1"; STEP="${2:-all}"; shift 2 || true
. "$(dirname "$0")/../../bridge/tools/portal2_dir.sh" || exit 1   # P2 = the Portal 2 install (PORTAL2_DIR, else found through Steam)
T="$P2/bin/linux32"
G="$(cd "$(dirname "$0")/../build/game" && pwd)"
export LD_LIBRARY_PATH="$T"
export PORTAL2_NO_LAUNCHERMGR_REQUIRED_IN_MATERIAL_SYSTEM=1
cd "$G"
run() { echo "== $*"; "$@"; }
# vvis/vrad lower-case their input path; /proc/self/cwd is an all-lower-case
# alias of the game dir (we cd into it), so the paths survive that.
C=/proc/self/cwd
TH="-threads $(nproc)"
if [ "$STEP" = vbsp ] || [ "$STEP" = all ]; then run "$T/vbsp_linux" $TH -game "$C" "$C/$NAME.vmf" > "$G/$NAME.vbsp.txt" 2>&1; tail -3 "$G/$NAME.vbsp.txt"; fi
if [ "$STEP" = vvis ] || [ "$STEP" = all ]; then run "$T/vvis_linux" $TH ${VVIS_ARGS:-} -game "$C" "$C/$NAME.bsp" > "$G/$NAME.vvis.txt" 2>&1; tail -3 "$G/$NAME.vvis.txt"; fi
if [ "$STEP" = vrad ] || [ "$STEP" = all ]; then run "$T/vrad_linux" ${VRAD_ARGS:--hdr -final -StaticPropLighting -StaticPropPolys -textureshadows} $TH "$@" -game "$C" "$C/$NAME.bsp" > "$G/$NAME.vrad.txt" 2>&1; tail -3 "$G/$NAME.vrad.txt"; fi
