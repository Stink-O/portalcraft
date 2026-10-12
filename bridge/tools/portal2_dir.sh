#!/bin/bash
# Finds the player's Portal 2 install (the folder that holds portal2_linux and portal2/).
#
#   . "$ROOT/bridge/tools/portal2_dir.sh"     # sets and exports P2 (and PORTAL2_DIR); returns 1 with a message if not found
#   bridge/tools/portal2_dir.sh               # prints the path, exit 1 with a message on stderr if not found
#
# Order: $PORTAL2_DIR if set (and valid), else Steam's libraryfolders.vdf (native, ~/.local/share/Steam, Flatpak),
# looking for the library that holds appid 620 (steamapps/common/<installdir from appmanifest_620.acf, default "Portal 2">).
# Python twin: bridge/tools/portal2_dir.py (same rules).
_p2_valid() { [ -d "$1/portal2" ] && [ -e "$1/portal2_linux" -o -d "$1/bin" ]; }
_p2_find() {
  if [ -n "${PORTAL2_DIR:-}" ]; then
    if _p2_valid "$PORTAL2_DIR"; then printf '%s\n' "${PORTAL2_DIR%/}"; return 0; fi
    echo "portal2_dir: PORTAL2_DIR=$PORTAL2_DIR is set but does not look like a Portal 2 folder (no portal2/ inside it)." >&2
    return 1
  fi
  local steamapps libs lib inst d tried=""
  local roots=("$HOME/.steam/steam/steamapps" "$HOME/.local/share/Steam/steamapps" "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps")
  libs=()
  for steamapps in "${roots[@]}"; do
    [ -d "$steamapps" ] || continue
    libs+=("$steamapps/..")           # the Steam install's own library
    if [ -r "$steamapps/libraryfolders.vdf" ]; then
      # "path" entries (current format) and "<n>" "<path>" entries (old format)
      while IFS= read -r lib; do [ -n "$lib" ] && libs+=("$lib"); done < <(awk -F'"' '
        $2=="path" && $4!="" { print $4; next }
        $2 ~ /^[0-9]+$/ && substr($4,1,1)=="/" { print $4 }' "$steamapps/libraryfolders.vdf")
    fi
  done
  local seen=" "
  for lib in "${libs[@]}"; do
    lib=$(cd "$lib" 2>/dev/null && pwd -P) || continue
    case "$seen" in *" $lib "*) continue ;; esac; seen="$seen$lib "
    tried="$tried $lib"
    inst="Portal 2"
    if [ -r "$lib/steamapps/appmanifest_620.acf" ]; then
      d=$(awk -F'"' '$2=="installdir" { print $4 }' "$lib/steamapps/appmanifest_620.acf")
      [ -n "$d" ] && inst=$d
    fi
    if _p2_valid "$lib/steamapps/common/$inst"; then printf '%s\n' "$lib/steamapps/common/$inst"; return 0; fi
  done
  echo "portal2_dir: could not find Portal 2 (Steam app 620). Install it in Steam, start it once, or set PORTAL2_DIR to the folder" >&2
  echo "  that contains portal2_linux (Steam: right-click Portal 2, Manage, Browse local files)." >&2
  [ -n "$tried" ] && echo "  Steam libraries checked:$tried" >&2 || echo "  No Steam library was found under ~/.steam, ~/.local/share/Steam or the Flatpak path." >&2
  return 1
}
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  _p2_find
  exit $?
else
  P2="$(_p2_find)" || { unset P2; return 1; }
  export P2 PORTAL2_DIR="$P2"
fi
