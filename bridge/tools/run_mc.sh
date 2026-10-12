#!/bin/bash
# Start the SkyCraft-based Fabric mod (Minecraft 26.3) via Loom runClient, hidden, in the background.
#   tools/run_mc.sh [--visible] [--audio] [--terrain [seed] [render_distance] | --mirror]
# (bridge #78) The terrain world is the default (bridge #81: NO fixed seed: the first launch creates the save "Portalcraft" with a random seed; PORTALCRAFT_SEED=<n> forces one, save Portalcraft_<n>): the one world of every session, pit flings
# and the campaign (every chamber in its band, the plugin's pc_world auto follows it). --mirror: the void mirror world (save SkyCraft),
# the fallback with the per-chamber stage; test runs ask for it explicitly (locked_run.sh mirror).
# --audio (or SKYCRAFT_AUDIO=true): the host drives its sound (user sessions); test runs stay muted.
# Log: cache/bridge/mc.log. Minecraft maps /dev/shm/Portalcraft_v1 when the Portal 2 plugin creates it,
# opens its void world once linked, and quits 5 s after that Portal 2 exits.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LOG="$ROOT/cache/bridge/mc.log"
. "$ROOT/bridge/tools/world_conf.sh" || exit 1   # (bridge #81) RENDER_DISTANCE default; SEED_FORCED = $PORTALCRAFT_SEED
HIDDEN=true; WORLD=terrain; SEED=$SEED_FORCED; RD=$RENDER_DISTANCE; AUDIO=${SKYCRAFT_AUDIO:-false}
while [ $# -gt 0 ]; do
  case "$1" in
    --visible) HIDDEN=false ;;
    --audio) AUDIO=true ;;
    --mirror) WORLD=mirror ;;
    --terrain) WORLD=terrain; [ -n "${2:-}" ] && [ "${2#--}" = "$2" ] && { SEED=$2; shift; }; [ -n "${2:-}" ] && [ "${2#--}" = "$2" ] && { RD=$2; shift; } ;;
  esac; shift
done
if pgrep -f "java.*[K]notClient" >/dev/null; then echo "Minecraft (Loom) already running" >&2; exit 2; fi
mkdir -p "$(dirname "$LOG")"
rm -f "$ROOT/cache/bridge/portalcraft_world.env"   # (bridge #81) the new Minecraft reports its own world; never read the last one
cd "$ROOT/bridge/fabric"
PORTALCRAFT_ROOT="$ROOT" SKYCRAFT_AUDIO=$AUDIO SKYCRAFT_HIDDEN=$HIDDEN SKYCRAFT_QUIT_WITH_HOST=true SKYCRAFT_WORLD=$WORLD SKYCRAFT_SEED=$SEED SKYCRAFT_RENDER_DISTANCE=$RD SKYCRAFT_MIN_SECTION_Y=${SKYCRAFT_MIN_SECTION_Y:-$([ $WORLD = terrain ] && echo 3 || echo -2147483648)} nohup ./gradlew --no-daemon runClient > "$LOG" 2>&1 &
echo "[mc] runClient started (hidden=$HIDDEN audio=$AUDIO world=$WORLD seed=${SEED:-random} render distance=$RD), log $LOG"
