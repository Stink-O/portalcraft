# Sourced by run_game.sh and play_clip.sh (bridge #24): Steam's launch of Portal 2 (app 620) stuck on a pop-up.
# Steam logs "LaunchApp waiting for user response to ShowInterstitials" in ~/.steam/steam/logs/console_log.txt; normally
# "continues with user response" follows in the same second. When it doesn't, a dialog is waiting for a click in the
# Steam window and no game starts (2026-10-02: 30 minutes lost to one).
#   steam_log_mark                 -> prints the log's current size (call before `steam steam://rungameid/620`)
#   steam_interstitial_blocked <mark> -> exit 0 (and prints the line) if a launch since <mark> waits on ShowInterstitials
#                                        for more than 3 s without a "continues"
STEAM_CONSOLE_LOG="${STEAM_CONSOLE_LOG:-$HOME/.steam/steam/logs/console_log.txt}"
steam_log_mark() { stat -c %s "$STEAM_CONSOLE_LOG" 2>/dev/null || echo 0; }
steam_interstitial_blocked() {
  local mark=${1:-0} size new wait_line id t now
  size=$(steam_log_mark)
  [ "$size" -lt "$mark" ] && mark=0  # (the log rotated)
  new=$(tail -c +$((mark + 1)) "$STEAM_CONSOLE_LOG" 2>/dev/null | grep -a 'AppID 620')
  wait_line=$(echo "$new" | grep -a 'waiting for user response to ShowInterstitials' | tail -1)
  [ -z "$wait_line" ] && return 1
  id=$(echo "$wait_line" | grep -o -E 'ActionID [0-9]+')
  echo "$new" | grep -a "$id" | grep -a -q 'continues with user response "ShowInterstitials"' && return 1
  t=$(date -d "$(echo "$wait_line" | grep -o -E '^\[[0-9: -]+\]' | tr -d '[]')" +%s 2>/dev/null || echo 0)
  now=$(date +%s)
  [ $((now - t)) -lt 3 ] && return 1
  echo "$wait_line"
  return 0
}
STEAM_INTERSTITIAL_MSG="Steam is waiting for a click on a pop-up (ShowInterstitials) before it starts Portal 2: open the Steam window and dismiss it (or wait for it), then run again. Nothing was launched."
