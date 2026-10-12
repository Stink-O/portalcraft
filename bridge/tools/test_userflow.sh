#!/bin/bash
# Test of the user's flow through play_clip.sh (not run_game.sh): fresh launch, a `map` reload, and a
# Minecraft restart mid-session, driven by tools/cfg/userflow.txt.
#   tools/test_userflow.sh [run_name=userflow]
# Results: cache/bridge/shots/<run>/ (bridge.log, play*.out, PNGs). Never kills Portal 2; the only
# Minecraft it restarts is the one play_clip.sh started (play_clip.sh --restart-mc enforces that).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/bridge/tools/portal2_dir.sh" || exit 1   # P2 = the Portal 2 install
RUN="${1:-userflow}"; OUT="$ROOT/cache/bridge/shots/$RUN"; LOG="$ROOT/cache/bridge/bridge.log"
WD=1500
mkdir -p "$OUT"
pgrep -x portal2_linux >/dev/null && { echo "Portal 2 is running; not testing" >&2; exit 2; }
LOGSTART=$(wc -l < "$LOG")
SHOTDIR_EXISTED=0; [ -d "$P2/portal2/screenshots" ] && SHOTDIR_EXISTED=1

echo "[test] phase 1: play_clip.sh fresh launch"
"$ROOT/bridge/tools/play_clip.sh" ${PCB_SO:+--so "$PCB_SO"} --script "$ROOT/bridge/tools/cfg/userflow.txt" $WD 2>&1 | tee "$OUT/play1.out"
P2PID=$(pgrep -x portal2_linux | head -1)
[ -z "$P2PID" ] && { echo "[test] Portal 2 is not running after play_clip.sh" >&2; exit 3; }

echo "[test] waiting for PHASE3 (up to 1200 s)"
for i in $(seq 1 600); do
  tail -n +$((LOGSTART+1)) "$LOG" | grep -a -q 'PHASE3 restart Minecraft' && break
  kill -0 "$P2PID" 2>/dev/null || break
  sleep 2
done
if kill -0 "$P2PID" 2>/dev/null && tail -n +$((LOGSTART+1)) "$LOG" | grep -a -q 'PHASE3 restart Minecraft'; then
  echo "[test] phase 3: play_clip.sh --restart-mc (attach mode)"
  "$ROOT/bridge/tools/play_clip.sh" --restart-mc --mute 2>&1 | tee "$OUT/play3.out"
fi

echo "[test] waiting for the script to quit the game (up to $((WD+60)) s)"
t=0
while kill -0 "$P2PID" 2>/dev/null; do sleep 2; t=$((t+2)); [ $t -ge $((WD+60)) ] && { echo "[test] still running; leaving it" >&2; break; }; done
shopt -s nullglob
for s in "$P2/portal2/screenshots"/pcb_u*; do
  b=$(basename "$s"); mv "$s" "$OUT/$b"
  python3 -c "import sys;from PIL import Image;Image.open(sys.argv[1]).convert('RGB').save(sys.argv[2])" "$OUT/$b" "$OUT/${b%.*}.png" && rm -f "$OUT/$b"
done
[ "$SHOTDIR_EXISTED" = 0 ] && rmdir "$P2/portal2/screenshots" 2>/dev/null
tail -n +$((LOGSTART+1)) "$LOG" > "$OUT/bridge.log"
echo "[test] results:"
grep -a -E 'PHASE|ASSERT|RUN SUMMARY|clip: (cliff exit|no cliff exit|exit check)|link (up|down)|terrain: (region|Minecraft)' "$OUT/bridge.log" | sed 's/\[bridge\] //'
