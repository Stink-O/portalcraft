#!/bin/bash
# Run the real Portal 2 once with the bridge plugin and a test script.
#
#   tools/run_game.sh <script> <map> [watchdog_s=150] [shots_subdir=run]
#
# Writes portal2/cfg/autoexec.cfg (absent in a stock install; the script refuses
# to run if one exists) with: plugin_load <dev build>; pcb_script <script>; map <map>.
# The autoexec is deleted as soon as the plugin reports it has loaded (or on
# exit). The plugin runs the script after the map is in; the script ends with
# `quit`, and the plugin's watchdog shuts the game down cleanly if it is still
# running after watchdog_s. If it hangs anyway (20 s after the script ends or the plugin
# crashes, or watchdog_s + 60), this script stops the Portal 2 IT launched (never another one:
# it refuses to start while any Portal 2 is running). Then it waits up to 60 s for Minecraft to exit.
# Screenshots named pcb_* are moved out of portal2/screenshots into
# cache/bridge/shots/<subdir>/ and converted to PNG. No other game file is touched.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/bridge/tools/portal2_dir.sh" || exit 1   # P2 = the Portal 2 install (PORTAL2_DIR, else found through Steam)
SO="$(realpath "${PCB_SO:-$ROOT/bridge/build/dev/portalcraft_bridge.so}")"  # PCB_SO=<path> tests another build
AUTO="$P2/portal2/cfg/autoexec.cfg"
SCRIPT="$(realpath "$1")"; MAP="$2"; WD="${3:-150}"; SUB="${4:-run}"
OUT="$ROOT/cache/bridge/shots/$SUB"; LOG="$ROOT/cache/bridge/bridge.log"

if pgrep -x portal2_linux >/dev/null; then echo "Portal 2 is already running (the user may be playing); not launching" >&2; exit 2; fi
for f in "$AUTO" "$P2/update/cfg/autoexec.cfg"; do
  [ -e "$f" ] && { echo "$f exists (someone else's run?); refusing" >&2; exit 2; }
done
# remove the autoexec only if it is still the one this run wrote (another agent's run may own it by then)
MYAUTO=""
rm_own_auto() { [ -n "$MYAUTO" ] && [ -f "$AUTO" ] && [ "$(cat "$AUTO")" = "$MYAUTO" ] && rm -f "$AUTO"; true; }
trap rm_own_auto EXIT INT TERM
# A shared-memory link left behind by a crashed run (the plugin recreates it at load anyway).
rm -f /dev/shm/Portalcraft_v1
SHOTDIR_EXISTED=0; [ -d "$P2/portal2/screenshots" ] && SHOTDIR_EXISTED=1
LOGSTART=$(wc -l < "$LOG" 2>/dev/null || echo 0)
AUTOSTAMP="$ROOT/cache/bridge/.runstamp_$$"; touch "$AUTOSTAMP"  # frame dumps newer than this belong to this run
# (bridge #25) PCB_PRE="cmd1;cmd2": console commands before the map loads (pc_start_mode mc, as play_clip.sh does)
PRE=""; [ -n "${PCB_PRE:-}" ] && PRE="$(printf '%s' "$PCB_PRE" | tr ';' '\n')
"
MYAUTO="$(printf 'plugin_load "%s"\n%spcb_script "%s" %s\nmap %s\n// run %s' "$SO" "$PRE" "$SCRIPT" "$WD" "$MAP" "$$")"
printf '%s\n' "$MYAUTO" > "$AUTO"

# Steam drops a relaunch that comes seconds after the previous Portal 2 exited (the game starts without
# the autoexec and idles), so leave it a short settle time.
sleep ${PCB_SETTLE:-12}
stop_own_game() {
  echo "[run] $1: stopping our Portal 2 (pid $PID)" >&2
  kill -TERM "$PID" 2>/dev/null
  for j in $(seq 1 10); do kill -0 "$PID" 2>/dev/null || return; sleep 1; done
  echo "[run] still alive after SIGTERM; SIGKILL" >&2; kill -KILL "$PID" 2>/dev/null; sleep 1
}
echo "[run] launching Portal 2 ($(date +%T))"
. "$ROOT/bridge/tools/steam_check.sh"  # (bridge #24) a Steam pop-up holding the launch: fail fast
SMARK=$(steam_log_mark)
# PCB_GAME_ARGS="-vulkan": extra engine arguments for this launch (steam -applaunch adds them to the launch options without Steam's
# confirmation pop-up, which steam://run/620//<args> raises and which blocks every later launch until a user answers it);
# unset = the default launch
if [ -n "${PCB_GAME_ARGS:-}" ]; then steam -applaunch 620 ${PCB_GAME_ARGS} >/dev/null 2>&1 &
else steam steam://rungameid/620 >/dev/null 2>&1 &
fi
for i in $(seq 1 ${PCB_START_WAIT:-240}); do
  pgrep -x portal2_linux >/dev/null && break
  if [ $i -le 20 ] && blk=$(steam_interstitial_blocked "$SMARK"); then
    echo "[run] $STEAM_INTERSTITIAL_MSG" >&2; echo "[run] steam: $blk" >&2; exit 3
  fi
  sleep 1
done
PID=$(pgrep -x portal2_linux | head -1)
[ -z "$PID" ] && { echo "[run] game did not start" >&2; exit 3; }
echo "[run] pid $PID"
for i in $(seq 1 90); do tail -n +$((LOGSTART+1)) "$LOG" 2>/dev/null | grep -q "\[bridge\] loaded" && break; sleep 1; done
rm_own_auto; echo "[run] autoexec removed"
tail -n +$((LOGSTART+1)) "$LOG" 2>/dev/null | grep -q "\[bridge\] loaded" || { stop_own_game "the plugin never loaded (90 s)"; echo "[run] game exited without the plugin"; exit 4; }
grep -o -E "lib(dxvk_d3d9|togl)\.so|shaderapi[a-z0-9]*\.so" /proc/$PID/maps 2>/dev/null | sort -u | sed 's/^/[run] mapped: /'
# Wait for the game to exit. It is OUR Portal 2 (none was running at launch), so a hang is ended here:
# 20 s after the script finished (done/unloading) or the plugin logged a crash, or at watchdog + 60 s,
# SIGTERM, then SIGKILL 10 s later. A hung test must never leave a window up or hold the test lock.
t=0; ended=-1
while kill -0 "$PID" 2>/dev/null; do
  sleep 2; t=$((t+2))
  if [ $ended -lt 0 ] && tail -n +$((LOGSTART+1)) "$LOG" 2>/dev/null | grep -q -E "\[bridge\] (script: done|unloading)|^CRASH signal"; then ended=$t; fi
  [ $ended -ge 0 ] && [ $((t-ended)) -ge 20 ] && { stop_own_game "no exit 20 s after the script ended or crashed"; break; }
  [ $t -ge $((WD+60)) ] && { stop_own_game "still running after ${t}s (watchdog ${WD}s)"; break; }
done
echo "[run] game exited after ~${t}s"
# Its Minecraft quits with the host; wait (bounded) so the next run never finds one still exiting.
for j in $(seq 1 60); do pgrep -x java -a | grep -q fabric.dli || break; sleep 1; done
pgrep -x java -a | grep -q fabric.dli && echo "[run] warning: Minecraft still running 60 s after the game exited" >&2
mkdir -p "$OUT"
shopt -s nullglob
for s in "$P2/portal2/screenshots"/pcb_*; do
  b=$(basename "$s"); mv "$s" "$OUT/$b"
  python3 -c "import sys;from PIL import Image;Image.open(sys.argv[1]).convert('RGB').save(sys.argv[2])" "$OUT/$b" "$OUT/${b%.*}.png" && rm -f "$OUT/$b"
  echo "[run] shot $OUT/${b%.*}.png"
done
[ "$SHOTDIR_EXISTED" = 0 ] && rmdir "$P2/portal2/screenshots" 2>/dev/null
# the plugin's frame dumps (frame watch failures, a transition set's worst frames, mask probes) of
# this run: moved next to its log, as PNG
FR="$ROOT/cache/bridge/shots/frames"
n=0
for f in "$FR"/*.ppm; do
  [ "$f" -nt "$AUTOSTAMP" ] || continue
  mkdir -p "$OUT/frames"; b=$(basename "$f" .ppm)
  python3 -c "import sys;from PIL import Image;Image.open(sys.argv[1]).save(sys.argv[2])" "$f" "$OUT/frames/$b.png" && rm -f "$f" && n=$((n+1))
done
[ $n -gt 0 ] && echo "[run] $n frame dumps in $OUT/frames"
rm -f "$AUTOSTAMP"
tail -n +$((LOGSTART+1)) "$LOG" > "$OUT/bridge.log"
grep -a -E "\[bridge\] RUN (PERF|SUMMARY)" "$OUT/bridge.log" | tail -2 | sed "s/^\[bridge\] /[run] /"
exit 0
