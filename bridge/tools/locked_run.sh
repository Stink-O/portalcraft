#!/bin/bash
# One test run under the shared game lock (several agents share this machine; only one Portal 2 at a time).
#   tools/locked_run.sh <mc mode: terrain|mirror> <script> <map> <watchdog_s> <shots_subdir>   (PCB_SO=<plugin.so>)
# Waits (bounded) for no Minecraft before starting one and after the game exits, so the next holder never sees one.
# The Minecraft this run started (its fabric.dli java, recorded by pid) is stopped before the lock is released when
# the game never started / the plugin never loaded (run_game.sh exit 3/4) or it is still up 60 s after the game
# (it quits with its host only once linked): SIGTERM, then SIGKILL after 20 s (bridge #20, lead's request).
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
exec flock -o -w 1800 "$ROOT/cache/bridge/game.lock" bash -c '
  cd "'"$ROOT"'/bridge"
  mcpids() { pgrep -x java -a | grep fabric.dli | cut -d" " -f1; }
  nomc() { for i in $(seq 1 ${1:-60}); do [ -z "$(mcpids)" ] && return 0; sleep 1; done; return 1; }
  nomc || { echo "[lock] a Minecraft is still running after 60 s" >&2; exit 2; }
  # (bridge #19: SKYCRAFT_TEST=1: the mod respawns a dead player and heals it once in the world: tests start alive)
  ROOT="$(cd .. && pwd)"; . tools/world_conf.sh; export PORTALCRAFT_SEED="${PORTALCRAFT_SEED:-$TEST_SEED}"  # (bridge #81) test runs use the test seed (a throwaway save), never the players own world
  if [ "$1" = terrain ]; then SKYCRAFT_TEST=1 tools/run_mc.sh --terrain >/dev/null; else SKYCRAFT_TEST=1 tools/run_mc.sh --mirror >/dev/null; fi  # (bridge #78: run_mc.sh defaults to the terrain world)
  OWN=""
  for i in $(seq 1 120); do OWN=$(mcpids); [ -n "$OWN" ] && break; sleep 1; done
  echo "[lock] own Minecraft pid(s): ${OWN:-none found}" >&2
  stop_own_mc() {
    # never while any Portal 2 runs: the user may have started a session that reuses this Minecraft (2026-10-02)
    pgrep -x portal2_linux >/dev/null && { echo "[lock] a Portal 2 is running (the user?): own Minecraft left alone" >&2; return 0; }
    local alive=""
    for p in $OWN; do kill -0 "$p" 2>/dev/null && alive="$alive $p"; done
    [ -z "$alive" ] && return 0
    echo "[lock] stopping own Minecraft ($1):$alive" >&2
    kill -TERM $alive 2>/dev/null
    for i in $(seq 1 20); do local any=""; for p in $alive; do kill -0 "$p" 2>/dev/null && any=1; done; [ -z "$any" ] && return 0; sleep 1; done
    kill -KILL $alive 2>/dev/null; sleep 1
  }
  for i in $(seq 1 80); do grep -q "game window hidden" ../cache/bridge/mc.log 2>/dev/null && break; sleep 5; done
  SHM_BEFORE=$(ls /dev/shm 2>/dev/null | grep "^u$(id -u)-Shm_" | sort)
  tools/run_game.sh "$2" "$3" "$4" "$5"; r=$?
  # (bridge #25) the Minecraft log of this run goes next to its bridge.log, before the next lock holder overwrites it
  mkdir -p "../cache/bridge/shots/$5" && cp -f ../cache/bridge/mc.log "../cache/bridge/shots/$5/mc.log" 2>/dev/null
  if [ $r -eq 2 ]; then echo "[lock] run_game refused (a Portal 2 was already running): own Minecraft left alone" >&2
  elif [ $r -eq 3 ] || [ $r -eq 4 ]; then stop_own_mc "the game never linked (run_game exit $r)"
  else nomc 60 || stop_own_mc "still running 60 s after the game"; fi
  nomc 30 || echo "[lock] a Minecraft is still running" >&2
  # Steam leaks one 25 MB /dev/shm/u1000-Shm_* per game launch (971 had filled RAM and swap, 2026-10-02):
  # remove the ones this run created that no process holds open
  for f in $(ls /dev/shm 2>/dev/null | grep "^u$(id -u)-Shm_" | sort | comm -13 <(echo "$SHM_BEFORE") -); do
    fuser -s "/dev/shm/$f" 2>/dev/null || rm -f "/dev/shm/$f"
  done
  exit $r' _ "$@"
