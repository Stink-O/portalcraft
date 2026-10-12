#!/bin/bash
# Run the WINDOWS Portal 2 copy under Proton 11 with the Windows bridge plugin and a test script
# (the Proton twin of run_game.sh; it takes the same cache/bridge/game.lock, so it never overlaps a Linux test).
#
#   tools/run_game_proton.sh <script|-> <map|-> [watchdog_s=150] [shots_subdir=run]
#
#   script "-"  : no pcb_script line (interactive-style probing; the run ends at the watchdog, 'quit' from PCB_POST, or PCB_HOLD)
#   map "-"     : no `map` line (the game stays in the main menu)
# Environment:
#   PCB_DLL=<path>            plugin dll (default bridge/build/win/portalcraft_bridge.dll); copied next to the run as-is (loaded by Z: path)
#   PCB_PRE="cmd1;cmd2"       console commands before the map line (e.g. pc_start_mode mc)
#   PCB_WIN_UNVERIFIED=...    held-back slot groups to enable (engine_light,net_dormant,rc_tone,rc_stencil,rc_fields,rc_callqueue,all)
#   PCB_SLOTS="Name=n,..."    slot overrides
#   PCB_LINK_FILE=<file>      dev link file for a Linux Minecraft (see platform_win.cpp)
#   PCB_NOPLUGIN=1            vanilla run (no plugin_load line)
#   PCB_GAME_ARGS="..."       extra engine args
#   PCB_HOLD=<s>              with script "-": quit this many seconds after the plugin loaded (default 60)
#   PCB_MC=1                  also start the dev Minecraft (tools/run_mc.sh --terrain) like locked_run.sh does (needs PCB_LINK_FILE)
# Writes <copy>/portal2/cfg/autoexec.cfg (refuses if one exists), removes it when the plugin reported "[bridge] loaded" or on exit.
# Stops ONLY the portal2.exe this run started (found by exact name, new pid); never uses pattern kills.
# Output: cache/bridge/shots/<subdir>/{bridge.log,console.log}, and pcb_* screenshots from <copy>/portal2/screenshots as PNG.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GAME="${PCB_WINGAME:-$HOME/Portal2-Windows}"   # the SteamCMD copy of the Windows Portal 2 (outside the Steam library)
P2="$GAME/portal2"
AUTO="$P2/cfg/autoexec.cfg"
LOG="$ROOT/cache/bridge/bridge.log"
LAUNCH="${PCB_PROTON_LAUNCH:-$HOME/portalcraft-wintest/2-run-windows-portal2-proton.sh}"   # starts that copy with Proton
DLL="$(realpath "${PCB_DLL:-$ROOT/bridge/build/win/portalcraft_bridge.dll}")"
SCRIPT="${1:--}"; MAP="${2:--}"; WD="${3:-150}"; SUB="${4:-run}"
[ "$SCRIPT" != "-" ] && SCRIPT="$(realpath "$SCRIPT")"
OUT="$ROOT/cache/bridge/shots/$SUB"

# The whole run happens under the shared game lock (re-exec once).
if [ -z "${PCB_LOCKED:-}" ]; then
  export PCB_LOCKED=1
  exec flock -o -w 1800 "$ROOT/cache/bridge/game.lock" "$0" "$@"
fi

[ -f "$GAME/portal2.exe" ] || { echo "no Windows Portal 2 at $GAME" >&2; exit 3; }
pgrep -x steam >/dev/null || { echo "Steam is not running" >&2; exit 3; }
if pgrep -x portal2_linux >/dev/null || pgrep -x portal2.exe >/dev/null; then echo "a Portal 2 is already running; not launching" >&2; exit 2; fi
[ -e "$AUTO" ] && { echo "$AUTO exists (someone else's run?); refusing" >&2; exit 2; }
[ "${PCB_NOPLUGIN:-}" = 1 ] || [ -f "$DLL" ] || { echo "no plugin dll at $DLL" >&2; exit 3; }

MYAUTO=""
rm_own_auto() { [ -n "$MYAUTO" ] && [ -f "$AUTO" ] && [ "$(cat "$AUTO")" = "$MYAUTO" ] && rm -f "$AUTO"; true; }
trap rm_own_auto EXIT INT TERM
rm -f "$P2/console.log"
LOGSTART=$(wc -l < "$LOG" 2>/dev/null || echo 0)
SHOTDIR_EXISTED=0; [ -d "$P2/screenshots" ] && SHOTDIR_EXISTED=1

WINDLL="Z:${DLL%.dll}"   # Source appends .dll itself when missing; Z: is the Wine view of /
PRE=""; [ -n "${PCB_PRE:-}" ] && PRE="$(printf '%s' "$PCB_PRE" | tr ';' '\n')
"
LINES=""
[ "${PCB_NOPLUGIN:-}" = 1 ] || LINES="plugin_load \"$WINDLL\"
$PRE"
LINES="engine_no_focus_sleep 0
$LINES"   # Source sleeps ~50 ms a frame while its window is unfocused (the user works elsewhere): never in a test run
[ "$SCRIPT" != "-" ] && LINES="${LINES}pcb_script \"Z:$SCRIPT\" $WD
"
[ "$MAP" != "-" ] && LINES="${LINES}map $MAP
"
MYAUTO="$(printf '%s// run %s' "$LINES" "$$")"
printf '%s\n' "$MYAUTO" > "$AUTO"

# env for the plugin inside Wine
# Windows Portal 2 has no engine_no_focus_sleep (the line above is for the Linux twin): the plugin keeps the window "active" instead
export PCB_KEEP_ACTIVE="${PCB_KEEP_ACTIVE:-1}"
export PCB_WIN_UNVERIFIED="${PCB_WIN_UNVERIFIED:-}" PCB_SLOTS="${PCB_SLOTS:-}" PCB_LINK_FILE="${PCB_LINK_FILE:-}"
[ -z "$PCB_WIN_UNVERIFIED" ] && unset PCB_WIN_UNVERIFIED
[ -z "$PCB_SLOTS" ] && unset PCB_SLOTS
[ -z "$PCB_LINK_FILE" ] && unset PCB_LINK_FILE

MCPID=""
if [ "${PCB_MC:-}" = 1 ]; then
  . "$ROOT/bridge/tools/world_conf.sh"; export PORTALCRAFT_SEED="${PORTALCRAFT_SEED:-$TEST_SEED}"
  ( cd "$ROOT/bridge" && SKYCRAFT_TEST=1 PCB_LINK_FILE="${PCB_LINK_FILE:-}" tools/run_mc.sh --terrain >/dev/null 2>&1 )
  for i in $(seq 1 60); do MCPID=$(pgrep -x java -a | rg fabric.dli | cut -d" " -f1 | head -1); [ -n "$MCPID" ] && break; sleep 1; done
  echo "[run] own Minecraft pid: ${MCPID:-none}"
fi

PID=""
stop_own_game() {
  echo "[run] $1: stopping our portal2.exe (pid ${PID:-?})" >&2
  [ -z "$PID" ] && return
  kill -TERM "$PID" 2>/dev/null
  for j in $(seq 1 10); do kill -0 "$PID" 2>/dev/null || return; sleep 1; done
  echo "[run] still alive after SIGTERM; SIGKILL" >&2; kill -KILL "$PID" 2>/dev/null; sleep 1
}
stop_own_mc() {
  [ -n "$MCPID" ] && kill -0 "$MCPID" 2>/dev/null || return 0
  pgrep -x portal2_linux >/dev/null && return 0
  echo "[run] stopping own Minecraft $MCPID" >&2
  kill -TERM "$MCPID" 2>/dev/null; for i in $(seq 1 20); do kill -0 "$MCPID" 2>/dev/null || return 0; sleep 1; done
  kill -KILL "$MCPID" 2>/dev/null
}

echo "[run] launching Windows Portal 2 under Proton ($(date +%T))"
nohup "$LAUNCH" -novid -windowed -w 1600 -h 900 -console -condebug ${PCB_GAME_ARGS:-} >"$ROOT/cache/bridge/proton_launch.log" 2>&1 &
LPID=$!
for i in $(seq 1 ${PCB_START_WAIT:-240}); do
  PID=$(pgrep -x portal2.exe | head -1); [ -n "$PID" ] && break
  kill -0 $LPID 2>/dev/null || break
  sleep 1
done
[ -z "$PID" ] && { echo "[run] game did not start (see cache/bridge/proton_launch.log)" >&2; stop_own_mc; exit 3; }
echo "[run] pid $PID"
if [ "${PCB_NOPLUGIN:-}" != 1 ]; then
  for i in $(seq 1 ${PCB_LOAD_WAIT:-60}); do tail -n +$((LOGSTART+1)) "$LOG" 2>/dev/null | rg -q "\[bridge\] loaded" && break; kill -0 "$PID" 2>/dev/null || break; sleep 1; done
fi
rm_own_auto; echo "[run] autoexec removed"
if [ "${PCB_NOPLUGIN:-}" != 1 ] && ! tail -n +$((LOGSTART+1)) "$LOG" 2>/dev/null | rg -q "\[bridge\] loaded"; then
  stop_own_game "the plugin never loaded"; stop_own_mc; exit 4
fi
t=0; ended=-1
while kill -0 "$PID" 2>/dev/null; do
  sleep 2; t=$((t+2))
  if [ $ended -lt 0 ] && tail -n +$((LOGSTART+1)) "$LOG" 2>/dev/null | rg -q "\[bridge\] (script: done|unloading)|^CRASH signal|EXCEPTION 0x"; then ended=$t; fi
  [ $ended -ge 0 ] && [ $((t-ended)) -ge 20 ] && { stop_own_game "no exit 20 s after the script ended or crashed"; break; }
  if [ "$SCRIPT" = "-" ] && [ $t -ge ${PCB_HOLD:-60} ]; then stop_own_game "hold time over"; break; fi
  [ $t -ge $((WD+60)) ] && { stop_own_game "still running after ${t}s (watchdog ${WD}s)"; break; }
done
echo "[run] game exited after ~${t}s"
# wineserver and the launcher wrapper wind down on their own; wait briefly for the proton script
for j in $(seq 1 20); do kill -0 $LPID 2>/dev/null || break; sleep 1; done
for j in $(seq 1 40); do [ -z "$MCPID" ] && break; kill -0 "$MCPID" 2>/dev/null || break; sleep 1; done
stop_own_mc
mkdir -p "$OUT"
shopt -s nullglob
for s in "$P2/screenshots"/pcb_*; do
  b=$(basename "$s"); mv "$s" "$OUT/$b"
  python3 -c "import sys;from PIL import Image;Image.open(sys.argv[1]).convert('RGB').save(sys.argv[2])" "$OUT/$b" "$OUT/${b%.*}.png" && rm -f "$OUT/$b"
  echo "[run] shot $OUT/${b%.*}.png"
done
[ "$SHOTDIR_EXISTED" = 0 ] && rmdir "$P2/screenshots" 2>/dev/null
tail -n +$((LOGSTART+1)) "$LOG" > "$OUT/bridge.log"
cp -f "$P2/console.log" "$OUT/console.log" 2>/dev/null
rm_own_auto
echo "[run] logs in $OUT"
exit 0
