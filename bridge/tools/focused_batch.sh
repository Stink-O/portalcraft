#!/bin/bash
# Focused-run batch (bridge #12): the timing runs that are only valid with Portal 2 focused at ~180 fps, back to
# back, for the lead to run with the user present. Each run opens Portal 2 (the user's KWin rule puts it on Desktop
# 2); the user switches there and clicks the window, and keeps it focused until the run says "done".
#
#   tools/focused_batch.sh [--m2c] [--dry-run]
#
# Runs (Minecraft is started fresh for each one, RD 24, muted; it quits with its Portal 2):
#   1 prof3_old   prof3 on d880bb01 (build/dev-mcportal/fo4.so, the old baseline)
#   2 prof3_dev   prof3 on build/dev (copied at the start, so a promotion during the batch does not change it)
#     -> did wob3 raise the worst frames (12.9-20.1 ms in shots/prof3_focus2)?
#   3 pc_off1  4 pc_on1  5 pc_off2  6 pc_on2   prof_mc (phases A-D, D = the moving camera) with Minecraft's pass
#     section cache off / on (-Dportalcraft.passCache=true), two A/B pairs, alternating
#   7 wob1_dev    wob1 on build/dev (walking-wobble jitter, p95 <= 0.5 px)
#   --m2c: 8 m2c_dev  trans_m2c (5 m2c_walk + 5 m2c_fling crossings): are trans11's m2c_fling failures throttling?
# About 20 minutes (23 with --m2c). Each run's HOST frame rate is checked: below 170 fps it is marked INVALID.
# Never kills anything: a run refuses to start over a running Portal 2 (run_game.sh) or Minecraft.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
B="$ROOT/bridge"; SHOTS="$ROOT/cache/bridge/shots"
. "$B/tools/world_conf.sh" || exit 1   # (bridge #81)
export PORTALCRAFT_SEED="${PORTALCRAFT_SEED:-$TEST_SEED}"   # the timing runs use the test seed, never the player's world
M2C=0; DRY=0
for a in "$@"; do case "$a" in --m2c) M2C=1 ;; --dry-run) DRY=1 ;; *) echo "unknown option $a" >&2; exit 2 ;; esac; done
STAMP=$(date +%m%d_%H%M)
OLD="$B/build/dev-mcportal/fo4.so"
[ "$(sha1sum < "$OLD" | cut -c1-8)" = d880bb01 ] || { echo "$OLD is not d880bb01; not running" >&2; exit 2; }
DEVSHA=$(sha1sum < "$B/build/dev/portalcraft_bridge.so" | cut -c1-8)
DEV="$B/build/dev-mcportal/batch_dev_$DEVSHA.so"

# label | build | script | watchdog s | pass cache
RUNS=(
  "prof3_old_$STAMP|$OLD|prof3.txt|300|0"
  "prof3_dev_$STAMP|$DEV|prof3.txt|300|0"
  "pc_off1_$STAMP|$DEV|prof_mc.txt|420|0"
  "pc_on1_$STAMP|$DEV|prof_mc.txt|420|1"
  "pc_off2_$STAMP|$DEV|prof_mc.txt|420|0"
  "pc_on2_$STAMP|$DEV|prof_mc.txt|420|1"
  "wob1_dev_$STAMP|$DEV|wob1.txt|360|0"
)
[ $M2C = 1 ] && RUNS+=("m2c_dev_$STAMP|$DEV|trans_m2c.txt|600|0")

mc_running() { pgrep -x java -a | grep -q fabric.dli; }
say() { echo "[batch $(date +%T)] $*"; }

say "build/dev = $DEVSHA (copied to $(basename "$DEV")), old baseline = d880bb01; ${#RUNS[@]} runs, about $(( ${#RUNS[@]} * 3 )) minutes"
say "For EVERY run: when it says GO, switch to Desktop 2 and click the Portal 2 window when it appears; keep it"
say "focused (no other window on top) until that run says done. Hands off the keyboard and mouse otherwise."
[ $DRY = 1 ] && { printf '  %s\n' "${RUNS[@]}"; exit 0; }
if [ ! -f "$DEV" ]; then cp "$B/build/dev/portalcraft_bridge.so" "$DEV.tmp" && mv "$DEV.tmp" "$DEV"; fi

i=0
for r in "${RUNS[@]}"; do
  i=$((i + 1))
  IFS='|' read -r LABEL SO CFG WD PC <<< "$r"
  if pgrep -x portal2_linux >/dev/null; then say "Portal 2 is running (not ours?): stopping the batch before $LABEL"; break; fi
  for k in $(seq 1 30); do mc_running || break; sleep 1; done  # (the last run's Minecraft quits 5 s after its host)
  if mc_running; then say "a Minecraft is still running: stopping the batch before $LABEL (nothing killed)"; break; fi
  if [ "$PC" = 1 ]; then export PORTALCRAFT_JVM_ARGS="-Dportalcraft.passCache=true"; else unset PORTALCRAFT_JVM_ARGS; fi
  "$B/tools/run_mc.sh" --terrain >/dev/null
  sleep 20
  echo
  say ">>> RUN $i/${#RUNS[@]} $LABEL ($CFG on $(basename "$SO"), pass cache $PC)"
  say ">>> GO: switch to Desktop 2 and click the Portal 2 window when it appears; keep it focused until 'done'"
  PCB_SO="$SO" "$B/tools/run_game.sh" "$B/tools/cfg/$CFG" pc_pit_flings "$WD" "$LABEL" | grep -E 'game exited|refus|not launching|did not start'
  unset PORTALCRAFT_JVM_ARGS
  cp "$ROOT/cache/bridge/mc.log" "$SHOTS/$LABEL/mc.log" 2>/dev/null
  # host frame rate of the run: the "host frames" windows (the first one covers loading; windows under 300 frames are
  # too short to judge); the slowest decides validity
  worst_fps=$(grep -ao 'host frames: [0-9]*, mean [0-9.]* ms ([0-9]* fps)' "$SHOTS/$LABEL/bridge.log" 2>/dev/null | tail -n +2 |
    awk '{n=$3; sub(",","",n); f=$7; sub("\\(","",f); if (n+0 >= 300) print f}' | sort -n | head -1)
  verdict="valid"; [ -z "$worst_fps" ] || [ "$worst_fps" -lt 170 ] && verdict="INVALID (host ${worst_fps:-?} fps: not focused?)"
  say "<<< done $LABEL: slowest phase ${worst_fps:-?} fps -> $verdict. You can look away until the next GO."
  echo "$LABEL $verdict" >> "$SHOTS/focused_batch_$STAMP.txt"
done

echo
say "summary (also in $SHOTS/focused_batch_$STAMP.txt):"
cat "$SHOTS/focused_batch_$STAMP.txt" 2>/dev/null
for r in "${RUNS[@]}"; do
  IFS='|' read -r LABEL SO CFG WD PC <<< "$r"
  [ -f "$SHOTS/$LABEL/bridge.log" ] || continue
  echo "== $LABEL"
  grep -a -E 'host frames:|RUN SUMMARY|wobble report|ASSERT FAIL' "$SHOTS/$LABEL/bridge.log" | sed 's/^\[bridge\] /  /' | head -20
  [ "$CFG" = prof_mc.txt ] && "$B/tools/mc_perf.sh" "$LABEL" 2>/dev/null | head -12
done
