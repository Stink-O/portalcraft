# Sourced by the shell tools (bridge #81). ROOT = the portalcraft checkout.
#   . "$ROOT/bridge/tools/world_conf.sh"
# Defaults from bridge/world.conf (RENDER_DISTANCE, TEST_SEED), then
#   SEED_FORCED  PORTALCRAFT_SEED (tests, filming) or empty: the player's world has a random seed picked by the mod on the first launch
#   SAVE_NAME    the save directory this launch uses: Portalcraft_<forced seed>, else Portalcraft
#   world_report loads cache/bridge/portalcraft_world.env (what the mod reports for the OPEN save: SEED SAVE CLIFF_* EXIT_W* CHAMBER_* ...) and sets
#                EXIT_MC, the portal centre on the exit patch as the plugin logs it; returns 1 while Minecraft has not reported
WORLD_CONF="${PCB_WORLD_CONF:-$ROOT/bridge/world.conf}"
[ -r "$WORLD_CONF" ] || { echo "world_conf.sh: $WORLD_CONF not found" >&2; return 1 2>/dev/null || exit 1; }
. "$WORLD_CONF"
WORLD_ENV="${PCB_WORLD_ENV:-$ROOT/cache/bridge/portalcraft_world.env}"
SEED_FORCED="${PORTALCRAFT_SEED:-}"
SAVE_NAME="Portalcraft${SEED_FORCED:+_$SEED_FORCED}"
world_report() {
  [ -r "$WORLD_ENV" ] || return 1
  grep -q '^EXIT_WX=' "$WORLD_ENV" || return 1
  . "$WORLD_ENV"
  # the same formula as clip.cpp PatchCentre (the pinned patch is never a stage patch): rx = dz, rz = -dx
  EXIT_MC=$(awk -v wx="$EXIT_WX" -v wy="$EXIT_WY" -v wz="$EXIT_WZ" -v dx="$CLIFF_DX" -v dz="$CLIFF_DZ" 'BEGIN {
    rx = dz; rz = -dx; my = wy + 1.5
    if (rx != 0) mx = wx + (rx > 0 ? 1.0 : 0.0); else mx = wx + (dx < 0 ? 0.0 : 1.0) + dx * 0.02
    if (rz != 0) mz = wz + (rz > 0 ? 1.0 : 0.0); else mz = wz + (dz < 0 ? 0.0 : 1.0) + dz * 0.02
    printf "(%.2f %.2f %.2f)", mx, my, mz }')
}
