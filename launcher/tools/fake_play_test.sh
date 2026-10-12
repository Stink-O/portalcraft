#!/bin/bash
# Integration test of the PLAY flow with fakes: no Microsoft account, no Minecraft, no Portal 2 is started.
#   launcher/tools/fake_play_test.sh [path/to/portalcraft-launcher]
# Builds a scratch world in a temp dir: a fake Portal 2 folder (with portal2_linux), a fake `steam` and a fake Portal 2 that
# reads the temporary autoexec.cfg and writes the plugin's log lines, a fake Prism that starts a fake "java", a stand-in
# Minecraft jar. Then runs `--cli play` and checks: the autoexec was written with plugin_load + map, is gone afterwards,
# Minecraft is started once and reused the second time, a leftover marker autoexec is cleaned up.
set -u
pgrep -x portal2_linux >/dev/null && { echo "a real Portal 2 is running: not running this test" >&2; exit 2; }
BIN=${1:-"$(dirname "$0")/../../target-launcher/release/portalcraft-launcher"}
BIN=$(realpath "$BIN")
T=$(mktemp -d /tmp/pc-fakeplay.XXXXXX)
export PORTALCRAFT_DATA=$T/data PORTAL2_DIR=$T/Portal\ 2 HOME_REAL=$HOME
export XDG_DATA_HOME=$T/xdg   # the app-menu entry setup adds goes here, never into your real menu
P2=$PORTAL2_DIR
D=$PORTALCRAFT_DATA
mkdir -p "$P2/portal2/cfg" "$P2/bin" "$D/plugin" "$D/cache/bridge" "$D/prism/instances/Portalcraft/.minecraft/mods" "$T/bin" "$D/prism/libraries/com/mojang/minecraft/26.3"
touch "$P2/portal2/gameinfo.txt"
touch "$D/.portalcraft-root" "$D/prism/instances/Portalcraft/instance.cfg"
A="$D/cache/minecraft-26.3/resourcepacks/local/minecraft-26.3/assets/minecraft"; mkdir -p "$A/textures/block"; touch "$A/.portalcraft-extracted" "$A/textures/block/dirt.png"
echo "fake plugin pc_world" > "$D/plugin/portalcraft_bridge.so"
# a fake signed-in Prism account (type + profile id only; no token) and a stand-in client jar
echo '{"accounts":[{"type":"MSA","profile":{"id":"fake","name":"Fake"}}],"formatVersion":3}' > "$D/prism/accounts.json"
python3 - "$D/prism/libraries/com/mojang/minecraft/26.3/minecraft-26.3-client.jar" <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1], "w")
z.writestr("assets/minecraft/textures/block/dirt.png", "png")
z.close()
PY
echo "[]" > "$D/state.json"; touch "$D/prism/PrismLauncher.keep"
cat > "$D/settings.json" <<EOS
{"portal2_dir":"$P2","seed_random":false,"seed":"4242","start_mode":"mc","offline":true}
EOS
# the fake game: Portal 2 loads the autoexec, then the plugin logs
cat > "$T/bin/portal2_linux" <<'EOS'
#!/bin/bash
LOG=$PORTALCRAFT_DATA/cache/bridge/bridge.log
echo $$ > "$PORTALCRAFT_DATA/../p2.pid"
sleep 2
MAP=$(sed -n 's/^map //p' "$PORTAL2_DIR/portal2/cfg/autoexec.cfg")
SO=$(sed -n 's/^plugin_load "\(.*\)"$/\1/p' "$PORTAL2_DIR/portal2/cfg/autoexec.cfg")
[ -f "$SO" ] || { echo "fake p2: plugin $SO missing" >> "$LOG"; exit 1; }
echo "[bridge] link: /dev/shm/Portalcraft_v1 created, 220 MB, host pid $$" >> "$LOG"
echo "[bridge] level init $MAP" >> "$LOG"
sleep 3
echo "[bridge] link up (Minecraft pid 1)" >> "$LOG"
echo "[bridge] mode: Minecraft player at (0 0 0)" >> "$LOG"
sleep 25
EOS
cp "$T/bin/portal2_linux" "$P2/portal2_linux"
cat > "$T/bin/steam" <<EOS
#!/bin/bash
nohup "$P2/portal2_linux" >/dev/null 2>&1 &
EOS
cat > "$D/prism/java" <<'EOS'
#!/bin/bash
echo $$ >> "$PORTALCRAFT_DATA/../java.pids"
sleep 120
EOS
cat > "$D/prism/PrismLauncher" <<EOS
#!/bin/bash
# fake Prism: "--launch Portalcraft" starts a fake java carrying the instance's JVM args
ARGS=\$(sed -n 's/^JvmArgs=//p' "$D/prism/instances/Portalcraft/instance.cfg" | tr -d '"')
nohup "$D/prism/java" \$ARGS >/dev/null 2>&1 &
EOS
chmod +x "$T/bin/"* "$P2/portal2_linux" "$D/prism/java" "$D/prism/PrismLauncher"
export PATH="$T/bin:$PATH"
fail=0
check() { if eval "$2"; then echo "PASS  $1"; else echo "FAIL  $1"; fail=1; fi; }

echo "== leftover temporary autoexec from a crashed run"
printf '// portalcraft-launcher: temporary file, removed once the level has loaded\nmap x\n' > "$P2/portal2/cfg/autoexec.cfg"
echo "== play (first run: starts the fake Minecraft and the fake Portal 2)"
"$BIN" --cli play 2>&1 | sed 's/^/   /' | rg -v '^\s+\[20' 
check "READY reported" "rg -q 'level init sp_a1_intro1' $D/cache/bridge/bridge.log"
check "autoexec removed afterwards" "[ ! -e \"\$P2/portal2/cfg/autoexec.cfg\" ]"
check "no saved-cfg leftovers" "[ -z \"\$(ls \"\$P2/portal2/cfg\")\" ]"
check "minecraft started with the seed" "pgrep -af 'prism/java' | rg -q 'skycraft.seed=4242'"
sleep 31   # the fake Portal 2 exits
echo "== play again (Minecraft is still running: must be reused, not started twice)"
N1=$(pgrep -f "$D/prism/java" | wc -l)
"$BIN" --cli play 2>&1 | rg -i 'reusing|READY|NOT|error' | sed 's/^/   /'
N2=$(pgrep -f "$D/prism/java" | wc -l)
check "still exactly one fake Minecraft" "[ $N1 = 1 ] && [ $N2 = 1 ]"
check "autoexec removed again" "[ ! -e \"\$P2/portal2/cfg/autoexec.cfg\" ]"
echo "== a changed seed: the Minecraft this launcher started is restarted with it"
sleep 31
sed -i 's/"seed":"4242"/"seed":"999"/' "$D/settings.json"
"$BIN" --cli play 2>&1 | rg -i 'restarting|READY|NOT' | sed 's/^/   /'
check "restarted with the new seed" "pgrep -af 'prism/java' | rg -q 'skycraft.seed=999'"
check "old Minecraft is gone" "! pgrep -af 'prism/java' | rg -q 'skycraft.seed=4242'"
echo "== the player's own autoexec.cfg is kept"
sleep 31
echo 'sv_cheats 1' > "$P2/portal2/cfg/autoexec.cfg"
"$BIN" --cli play >/dev/null 2>&1
check "own autoexec restored" "[ \"\$(cat \"\$P2/portal2/cfg/autoexec.cfg\")\" = 'sv_cheats 1' ]"
kill $(cat "$T/java.pids" "$T/p2.pid" 2>/dev/null) 2>/dev/null
rm -rf "$T"
[ $fail = 0 ] && echo "ALL PASSED" || echo "FAILED"
exit $fail
