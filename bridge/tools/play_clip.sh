#!/bin/bash
# One-command user session for the pit-flings clip: hidden Minecraft + Portal 2 with the bridge.
#
#   tools/play_clip.sh [--stable | --so <plugin.so>] [--map <map>] [--restart-mc] [--mc] [--start mc|chell | --as-mc]
# --start mc (or --as-mc): start the level as the Minecraft player (down the arrival elevator as it), in the usual
#   pc_pit_flings terrain session; J still switches. --start chell (the default) starts as Chell.
#                      [--mute] [--script <file> [watchdog_s]]      (--script: test runs, Minecraft muted)
#
# --mc (bridge #14, DESIGN-mcplayer M1b): play Portal 2's campaign as the Minecraft player. The map defaults to
#   sp_a1_intro1, and `pc_start_mode mc` makes every level start as the Minecraft player (J still switches). READY once
#   the link is up and the Minecraft player leads.
#   (bridge #78) Minecraft runs the TERRAIN world (the one world of every session, the same as pit flings): every chamber
#   sits in the world's chamber band, keeping its own placed and revealed blocks per map, and G opens onto the real
#   generated terrain round the pinned cliff (endless, the floating origin). This needs a plugin with `pc_world` (bridge #78
#   and later); with an older plugin, or with --world mirror, it runs the old void "mirror" world and its per-chamber stage.
#   --world unified|mirror forces one.
#
# Launch mode (Portal 2 not running):
#   1. Minecraft: reuses a healthy hidden terrain Minecraft, waits for one that is quitting after the
#      last session to finish, starts one with run_mc.sh --terrain if none runs. A frozen or wrong
#      Minecraft is reported, never killed, unless this script started it (then it is restarted).
#   2. writes a temporary portal2/cfg/autoexec.cfg (plugin_load + map), launches Portal 2 via Steam,
#      removes the autoexec as soon as the level is in (and on any exit of this script).
#   3. waits (bounded) for `link up` and the pinned cliff exit line, the exit Minecraft reported for its world (cache/bridge/portalcraft_world.env), and
#      prints READY only when both hold.
# Attach mode (Portal 2 already running with this plugin loaded): only Minecraft is checked or
#   (re)started, then the same link + exit verification. Use it after Minecraft died mid-session;
#   --restart-mc restarts a Minecraft this script started (one it did not start is never touched).
#
# Never kills Portal 2. Never edits any game file except the temporary autoexec.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SO="$ROOT/bridge/build/dev/portalcraft_bridge.so"
MAP=pc_pit_flings
LOG="$ROOT/cache/bridge/bridge.log"
MCLOG="$ROOT/cache/bridge/mc.log"
PIDFILE="$ROOT/cache/bridge/play_clip.mc"   # "<pid> <starttime>" of the Minecraft this script started
. "$ROOT/bridge/tools/world_conf.sh" || exit 1   # (bridge #81) SAVE_NAME; the exit line to wait for is what Minecraft REPORTS for its world (world_report -> EXIT_MC)
RESTART_MC=0; SCRIPT=""; SCRIPT_WD=600; MCAUDIO=--audio; MCMODE=0; MAPSET=0; START=chell; WORLD=""
while [ $# -gt 0 ]; do
  case "$1" in
    --stable) SO="$ROOT/bridge/build/portalcraft_bridge.so" ;;
    --so) SO="$(realpath "$2")"; shift ;;
    --map) MAP="$2"; MAPSET=1; shift ;;
    --mc) MCMODE=1 ;;
    --start) case "${2:-}" in mc|chell) START=$2; shift ;; *) echo "--start needs mc or chell" >&2; exit 2 ;; esac ;;
    --as-mc) START=mc ;;
    --world) case "${2:-}" in unified|mirror) WORLD=$2; shift ;; *) echo "--world needs unified or mirror" >&2; exit 2 ;; esac ;;
    --restart-mc) RESTART_MC=1 ;;
    --mute) MCAUDIO="" ;;
    --script) MCAUDIO=""; SCRIPT="$(realpath "$2")"; shift; [ -n "${2:-}" ] && [ "${2#--}" = "$2" ] && { SCRIPT_WD=$2; shift; } ;;
    -h|--help) sed -n 2,22p "$0"; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 1 ;;
  esac; shift
done

. "$ROOT/bridge/tools/portal2_dir.sh" || exit 1   # P2 = the Portal 2 install (PORTAL2_DIR, else found through Steam)
AUTO="$P2/portal2/cfg/autoexec.cfg"
[ "$MCMODE" = 1 ] && [ "$MAPSET" = 0 ] && MAP=sp_a1_intro1
# (bridge #78) the campaign's world: the terrain world when the plugin knows it (pc_world), else the old mirror world
if [ "$MCMODE" = 1 ] && [ -z "$WORLD" ]; then
  if [ -f "$SO" ] && grep -a -q 'pc_world' "$SO"; then WORLD=unified; else WORLD=mirror; fi
fi
[ "$MCMODE" = 0 ] && WORLD=unified   # (pit flings: the terrain world, as always)
WANT_TERRAIN=1; [ "$WORLD" = mirror ] && WANT_TERRAIN=0
say() { echo "[play $(date +%T)] $*"; }
fail() { say "NOT READY: $*"; exit "${2:-4}"; }

# ---- Minecraft helpers (pgrep -x on the process name never matches this shell) ----
mc_pids() { pgrep -x java -a | grep fabric.dli | awk '{print $1}'; }
stat_field() { awk -v f="$2" '{ sub(/^[0-9]+ \([^)]*\) /, ""); split($0, a, " "); print a[f-2] }' "/proc/$1/stat" 2>/dev/null; }
starttime() { stat_field "$1" 22; }
cpu_ticks() { local u s; u=$(stat_field "$1" 14); s=$(stat_field "$1" 15); echo $(( ${u:-0} + ${s:-0} )); }
is_ours() { [ -f "$PIDFILE" ] && [ "$(cat "$PIDFILE")" = "$1 $(starttime "$1")" ]; }
mclog_age() { [ -f "$MCLOG" ] && echo $(( $(date +%s) - $(stat -c %Y "$MCLOG") )) || echo 99999; }
is_terrain() { tr '\0' ' ' < "/proc/$1/cmdline" 2>/dev/null | grep -q -- '-Dskycraft.world=terrain'; }
# alive and making progress: not stopped/zombie, and CPU time advancing over 3 s (a hidden
# Minecraft renders continuously, linked or not)
mc_progressing() {
  local st a b; st=$(stat_field "$1" 3)
  case "$st" in T|t|Z|X|"") return 1 ;; esac
  a=$(cpu_ticks "$1"); sleep 3; b=$(cpu_ticks "$1")
  [ "$b" -gt "$a" ]
}
stop_ours() {  # only ever called for a pid is_ours() confirmed
  say "stopping the Minecraft this script started (pid $1)"
  kill -TERM "$1" 2>/dev/null
  for _ in $(seq 1 30); do kill -0 "$1" 2>/dev/null || { say "Minecraft $1 exited"; return 0; }; sleep 1; done
  say "Minecraft $1 ignored SIGTERM for 30 s (frozen); SIGKILL (it is ours)"
  kill -KILL "$1" 2>/dev/null; sleep 2
  kill -0 "$1" 2>/dev/null && fail "Minecraft $1 would not exit" 5
  return 0
}
start_mc() {
  local before; before=" $(mc_pids | tr '\n' ' ') "
  if [ "$WANT_TERRAIN" = 0 ]; then "$ROOT/bridge/tools/run_mc.sh" --mirror $MCAUDIO | sed 's/^/[play]   /'
  else "$ROOT/bridge/tools/run_mc.sh" --terrain $MCAUDIO | sed 's/^/[play]   /'; fi
  for _ in $(seq 1 120); do
    for p in $(mc_pids); do
      case "$before" in *" $p "*) ;; *) echo "$p $(starttime "$p")" > "$PIDFILE"; MCPID=$p; say "Minecraft started (pid $p, hidden, $([ "$WANT_TERRAIN" = 0 ] && echo mirror || echo terrain) world)"; return 0 ;; esac
    done
    sleep 1
  done
  fail "Minecraft did not start within 120 s (see $MCLOG)" 5
}

# ---- mode ----
P2PID=$(pgrep -x portal2_linux | head -1)
MODE=launch
if [ -n "$P2PID" ]; then
  # attach only to a Portal 2 whose bridge is loaded now: its log's newest "created" line names it
  host=$(grep -a -o -E 'link: /dev/shm/Portalcraft_v1 created, [0-9]+ MB, host pid [0-9]+' "$LOG" 2>/dev/null | tail -1 | awk '{print $NF}')
  lastload=$(grep -a -n -E '\[bridge\] (link: /dev/shm/Portalcraft_v1 created|unloading)' "$LOG" | tail -1)
  if [ "$host" = "$P2PID" ] && ! echo "$lastload" | grep -q unloading; then
    MODE=attach
    say "Portal 2 is running (pid $P2PID) with the bridge loaded: attach mode (only Minecraft is checked; the game is not touched)"
  else
    fail "Portal 2 is running (pid $P2PID) without the bridge loaded by a launcher. Not touching it; close it first, then run this again." 2
  fi
else
  # our own maps (pc_*) are not part of Portal 2: they are built and installed once (maps/tools/build.sh --install)
  case "$MAP" in pc_*) [ -f "$P2/portal2/maps/$MAP.bsp" ] || {
    say "NOT READY: the map $MAP is not installed in your Portal 2 ($P2/portal2/maps/)."
    case "$MAP" in pc_testbed) say "  Install it once with Portal 2 closed:  maps/tools/build_testbed.sh --install  (needs the Portal 2 Authoring Tools)" ;;
      *) say "  Install it once with Portal 2 closed:  maps/tools/build.sh --install" ;; esac
    say "  Or play the campaign as the Minecraft player now, no install needed:  bridge/tools/play_clip.sh --mc"
    exit 2; } ;; esac
  for f in "$AUTO" "$P2/update/cfg/autoexec.cfg"; do
    [ -e "$f" ] && fail "$f exists (another run in progress, or left over?); not launching. Remove it if no run is in progress." 2
  done
  [ -f "$SO" ] || fail "plugin $SO not built (cd bridge && make)" 2
fi
say "plugin: $SO (built $(date -r "$SO" '+%Y-%m-%d %H:%M'))"

# ---- Minecraft ----
MCPID=""; MCSTARTED=0
pids=$(mc_pids)
if [ "$(echo "$pids" | grep -c .)" -gt 1 ]; then
  fail "several Minecraft (Fabric) instances are running: $(echo $pids). Not touching them; close the extra ones." 3
fi
if [ -n "$pids" ]; then
  p=$pids
  if [ "$RESTART_MC" = 1 ]; then
    is_ours "$p" || fail "--restart-mc: Minecraft pid $p was not started by play_clip.sh; not touching it. Close it yourself, then run this again." 3
    stop_ours "$p"; p=""
  elif [ "$MODE" = launch ] && { tail -n 5 "$MCLOG" 2>/dev/null | grep -q -E 'has closed; saving and quitting|Stopping!' || {
         # linked to a Portal 2 that is gone: it quits with its host (5 s after it notices), so it
         # cannot be reused even if it has not started quitting yet
         lasthost=$(grep -a -o -E 'linked to the host game \(pid [0-9]+\)' "$MCLOG" 2>/dev/null | tail -1 | grep -o -E '[0-9]+');
         [ -n "$lasthost" ] && ! kill -0 "$lasthost" 2>/dev/null; }; }; then
    say "Minecraft $p belongs to the last Portal 2 session and quits with it; waiting for it to exit (up to 60 s)"
    for _ in $(seq 1 60); do kill -0 "$p" 2>/dev/null || break; sleep 1; done
    if kill -0 "$p" 2>/dev/null; then
      is_ours "$p" && { stop_ours "$p"; p=""; } || fail "Minecraft $p has been quitting for over 60 s (stuck?). It was not started by play_clip.sh, so it is not touched: close it (kill $p), then run this again." 3
    else p=""; fi
  fi
  if [ -n "$p" ]; then
    healthy=1; why=""
    if [ "$WANT_TERRAIN" = 1 ] && ! is_terrain "$p"; then healthy=0; why="the mirror world (this session needs the terrain world)"
    elif [ "$WANT_TERRAIN" = 0 ] && is_terrain "$p"; then healthy=0; why="a terrain world (--world mirror needs the void mirror world)"
    elif ! mc_progressing "$p"; then healthy=0; why="frozen (state $(stat_field "$p" 3), no CPU progress in 3 s)"
    elif [ "$MODE" = attach ] && [ "$(grep -a -E '\[bridge\] link (up|down)' "$LOG" | tail -1 | grep -c ' up ')" = 1 ] && [ "$(mclog_age)" -gt 15 ]; then
      healthy=0; why="linked but mc.log has not advanced for $(mclog_age) s"
    fi
    # a test run holds the game lock: its Minecraft (test mode) is not the user's to reuse
    if [ "$healthy" = 1 ] && ! flock -n "$ROOT/cache/bridge/game.lock" true 2>/dev/null; then
      fail "a test run is using Minecraft $p right now (cache/bridge/game.lock is held). Wait for that run to finish, then run this again." 3
    fi
    if [ "$healthy" = 1 ]; then
      MCPID=$p; say "reusing the running Minecraft (pid $p$(is_ours "$p" && echo ', started by play_clip.sh'))"
    elif is_ours "$p"; then
      say "Minecraft $p is $why; it was started by play_clip.sh, restarting it"
      stop_ours "$p"
    else
      fail "Minecraft pid $p is $why. It was not started by play_clip.sh, so it is not touched: close it (kill $p), then run this again." 3
    fi
  fi
fi
# (bridge #78) the first session of the unified world carries the campaign player (inventory, armour, XP) over from the mirror
# world's save, once (tools/world78/migrate.py: both saves backed up first; a marker in the terrain save; never while a Minecraft runs)
if [ -z "$MCPID" ] && [ "$WANT_TERRAIN" = 1 ] && grep -a -q 'pc_world' "$SO" && [ ! -e "$ROOT/bridge/fabric/run/saves/$SAVE_NAME/portalcraft_chambers/migrated78.txt" ]; then
  say "first session of the unified world: carrying the campaign player over from the mirror world (tools/world78/migrate.py)"
  python3 -I "$ROOT/bridge/tools/world78/migrate.py" 2>&1 | sed 's/^/[play]   /'
fi
[ -z "$MCPID" ] && { start_mc; MCSTARTED=1; }

# ---- Portal 2 ----
if [ "$MODE" = launch ]; then
  LOGSTART=$(wc -l < "$LOG" 2>/dev/null || echo 0)
  trap 'rm -f "$AUTO"' EXIT INT TERM
  {
    printf 'plugin_load "%s"\n' "$SO"
    { [ "$MCMODE" = 1 ] || [ "$START" = mc ]; } && printf 'pc_start_mode mc\n'
    grep -a -q 'pc_world' "$SO" && printf 'pc_world %s\n' "$([ "$WORLD" = mirror ] && echo legacy || echo unified)"
    [ -n "$SCRIPT" ] && printf 'pcb_script "%s" %s\n' "$SCRIPT" "$SCRIPT_WD"
    printf 'map %s\n' "$MAP"
  } > "$AUTO"
  say "launching Portal 2 (temporary autoexec: plugin_load + map $MAP)"
  . "$ROOT/bridge/tools/steam_check.sh"  # (bridge #24) a Steam pop-up holding the launch: fail fast
  SMARK=$(steam_log_mark)
  steam steam://rungameid/620 >/dev/null 2>&1 &
  for i in $(seq 1 240); do
    P2PID=$(pgrep -x portal2_linux | head -1); [ -n "$P2PID" ] && break
    if [ $i -le 20 ] && blk=$(steam_interstitial_blocked "$SMARK"); then say "steam: $blk"; fail "$STEAM_INTERSTITIAL_MSG" 6; fi
    sleep 1
  done
  [ -z "$P2PID" ] && fail "Portal 2 did not start within 240 s" 6
  say "Portal 2 running (pid $P2PID); waiting for the level"
  ok=0
  for _ in $(seq 1 180); do
    tail -n +$((LOGSTART+1)) "$LOG" 2>/dev/null | grep -a -q "\[bridge\] level init $MAP" && { ok=1; break; }
    kill -0 "$P2PID" 2>/dev/null || break
    sleep 1
  done
  rm -f "$AUTO"; trap - EXIT INT TERM
  say "temporary autoexec removed"
  [ "$ok" = 1 ] || fail "the plugin did not log 'level init $MAP' within 180 s (is the game at the main menu? see $LOG)" 6
else
  # the session's log starts at its plugin load
  LOGSTART=$(grep -a -n -E 'link: /dev/shm/Portalcraft_v1 created' "$LOG" | tail -1 | cut -d: -f1); LOGSTART=$((LOGSTART-1))
  [ "$MCSTARTED" = 1 ] && LOGSTART=$(wc -l < "$LOG")  # verify against the new Minecraft only
fi

# ---- verification: link up and the pinned cliff exit, both current ----
session() { tail -n +$((LOGSTART+1)) "$LOG" 2>/dev/null; }
if [ "$MCMODE" = 1 ]; then
  # --mc: the link is up and the Minecraft player leads (no cliff in the campaign maps)
  say "waiting for the Minecraft link and the Minecraft player (up to 300 s)"
  t=0
  while [ $t -lt 300 ]; do
    kill -0 "$P2PID" 2>/dev/null || fail "Portal 2 exited" 6
    [ -n "$(mc_pids)" ] || fail "Minecraft exited (see $MCLOG)" 5
    session | grep -a -q -E '\[bridge\] mode: Minecraft player at' && break
    sleep 2; t=$((t+2))
  done
  [ $t -ge 300 ] && { session | grep -a -E 'link (up|down)|mode:|start mode' | tail -8 | sed 's/^/[play]   /'; fail "the Minecraft player did not take over within 300 s; nothing was killed" 7; }
  say "READY: Portal 2 pid $P2PID, Minecraft pid $(mc_pids | head -1) ($([ $MCSTARTED = 1 ] && echo started now || echo reused)), map $MAP, as the Minecraft player, $([ "$WORLD" = mirror ] && echo 'the mirror world (stage)' || echo 'the terrain world (unified)')."
  [ "$WORLD" != mirror ] && say "G (hold while Chell fires, or J then G): the portal opens onto the real Minecraft world at the pinned cliff, the same place from every chamber."
  say "keys (no binds are written): J  Chell / Minecraft player.  G  use (buttons, cubes) as the Minecraft player."
  say "  E  inventory (the mouse moves Minecraft's cursor; Esc closes it).  Wheel / 1-9  hotbar."
  exit 0
fi
link_state() { session | grep -a -E '\[bridge\] link (up|down) \(Minecraft pid' | tail -1; }
exit_state() { session | grep -a -E '\[bridge\] clip: (cliff exit \(|no cliff exit yet)' | tail -1; }
say "waiting for the Minecraft link and the cliff exit (up to 300 s)"
good=0; t=0; last=""
while [ $t -lt 300 ]; do
  kill -0 "$P2PID" 2>/dev/null || fail "Portal 2 exited" 6
  [ -n "$(mc_pids)" ] || fail "Minecraft exited (see $MCLOG)" 5
  ls=$(link_state); es=$(exit_state)
  if world_report && echo "$ls" | grep -q ' up ' && echo "$es" | grep -q -F "cliff exit (pinned) at MC $EXIT_MC"; then
    good=$((good+1)); [ $good -ge 2 ] && break
  else
    good=0
    now="link: ${ls:-none yet} | exit: ${es:-not checked yet}"
    [ "$now" != "$last" ] && { say "${now//\[bridge\] /}"; last=$now; }
  fi
  sleep 2; t=$((t+2))
done
if [ $good -lt 2 ]; then
  say "last bridge lines:"; session | grep -a -E 'link (up|down)|clip:|terrain: (region|Minecraft)' | tail -8 | sed 's/^/[play]   /'
  fail "link and cliff exit not confirmed within 300 s; nothing was killed" 7
fi
say "link: $(link_state | sed 's/\[bridge\] //')"
say "exit: $(exit_state | sed 's/\[bridge\] //')"
say "READY: Portal 2 pid $P2PID, Minecraft pid $(mc_pids | head -1) ($([ $MCSTARTED = 1 ] && echo started now || echo reused)), map $MAP."
say "keys (no binds are written):"
say "  G  hold while firing (or tap, then fire): that portal's partner opens on the Minecraft cliff. The pit floor"
say "     only goes to Minecraft this way now (pc_pit_redirect_mode 2 = the old always-on pit rule)."
say "     In Minecraft, a portal fired with G puts its partner back in the chamber: walk back through it."
say "  H  hold 0.5 s in Minecraft: back to the last chamber spot you stood on (a held cube comes too)."
say "  J  toggle Chell / Minecraft player."
exit 0
