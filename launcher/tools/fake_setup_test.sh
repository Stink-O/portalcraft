#!/bin/bash
# Integration test of the SETUP flow, update, repair, self-update and offline handling, with fakes: no network, no Microsoft
# sign-in (the "sign-in" is a fake accounts.json written by this script), no real Prism, Minecraft or Portal 2.
#   launcher/tools/fake_setup_test.sh [path/to/portalcraft-launcher]
set -u
pgrep -x portal2_linux >/dev/null && { echo "a real Portal 2 is running: not running this test" >&2; exit 2; }
BIN=$(realpath "${1:-"$(dirname "$0")/../../target-launcher/release/portalcraft-launcher"}")
T=$(mktemp -d /tmp/pc-fakesetup.XXXXXX)
export PORTALCRAFT_DATA=$T/data
export XDG_DATA_HOME=$T/xdg   # the app-menu entry setup adds goes here, never into your real menu
D=$PORTALCRAFT_DATA
export PORTAL2_DIR="$T/Portal 2"
mkdir -p "$PORTAL2_DIR/portal2/cfg" "$T/dist" "$T/src/prism" "$T/lcdir"
touch "$PORTAL2_DIR/portal2_linux" "$PORTAL2_DIR/portal2/gameinfo.txt"
fail=0
check() { if eval "$2"; then echo "PASS  $1"; else echo "FAIL  $1"; fail=1; fi; }

# ---- fake release: a Prism tarball whose PrismLauncher stub creates the client jar and starts a fake java
cat > "$T/src/prism/PrismLauncher" <<EOS
#!/bin/bash
DATA="$D"
case "\$1" in
  --launch)
    ( sleep 2
      J=\$DATA/prism/libraries/com/mojang/minecraft/26.3; mkdir -p \$J
      python3 -c "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1],'w'); z.writestr('assets/minecraft/textures/block/dirt.png','png'); z.writestr('assets/minecraft/lang/en_us.json','{}'); z.close()" \$J/minecraft-26.3-client.jar
      sleep 1
      ARGS=\$(sed -n 's/^JvmArgs=//p' \$DATA/prism/instances/Portalcraft/instance.cfg | tr -d '"')
      exec -a java bash -c 'echo \$\$ >> $T/java.pids; sleep 300; true' java \$ARGS ) >/dev/null 2>&1 &
    ;;
esac
EOS
chmod +x "$T/src/prism/PrismLauncher"
echo "portable" > "$T/src/prism/portable.txt"
tar czf "$T/dist/prism.tar.gz" -C "$T/src/prism" PrismLauncher portable.txt
echo "plugin v1 pc_world" > "$T/dist/plugin1.so"; echo "plugin v2 pc_world" > "$T/dist/plugin2.so"
echo "mod v1" > "$T/dist/mod1.jar"; echo "mod v2" > "$T/dist/mod2.jar"; echo "api" > "$T/dist/api.jar"
cp "$BIN" "$T/dist/launcher-new"; echo "padding" >> "$T/dist/launcher-new"   # a "newer launcher": same program, different hash
mkmanifest() {  # $1 plugin file, $2 mod file, $3 launcher version, $4 plugin sha override
python3 - "$T" "$1" "$2" "$3" "${4:-}" <<'PY'
import hashlib, json, os, sys
t, plug, mod, lv, override = sys.argv[1:6]
d = t + "/dist/"
def e(version, f, sha=None):
    b = open(d + f, "rb").read()
    return {"version": version, "url": "asset:" + f, "sha256": sha or hashlib.sha256(b).hexdigest(), "size": len(b)}
m = {"schema": 1, "release": "t", "launcher": e(lv, "launcher-new"), "plugin": e(plug[6:7] + ".0.0", plug, override or None),
     "mod": e(mod[3:4] + ".0.0", mod), "fabric_api": e("1.0+26.3", "api.jar"), "prism": e("11.1.1", "prism.tar.gz")}
json.dump(m, open(d + "launcher.json", "w"), indent=1)
PY
}
mkmanifest plugin1.so mod1.jar 0.1.0
export PORTALCRAFT_MANIFEST_URL=$T/dist/launcher.json
echo '{"portal2_dir":"","offline":false}' > /dev/null

echo "== 1. a bad checksum is refused and nothing half-written stays"
mkmanifest plugin1.so mod1.jar 0.1.0 0000000000000000000000000000000000000000000000000000000000000000
"$BIN" --cli setup 2>&1 | rg -A1 'damaged' | head -3 | sed 's/^/   /'
check "no plugin installed" "[ ! -e '$D/plugin/portalcraft_bridge.so' ]"
check "no .part left" "[ -z \"\$(ls $D/downloads | rg '\\.part$')\" ]"

echo "== 2. full setup; the 'sign-in' appears 6 s in"
mkmanifest plugin1.so mod1.jar 0.1.0
( sleep 6; echo '{"accounts":[{"type":"MSA","profile":{"id":"fake","name":"Fake"}}],"formatVersion":3}' > "$D/prism/accounts.json" ) &
timeout 120 "$BIN" --cli setup 2>&1 | rg -v '^\[20' | tail -14 | sed 's/^/   /'
check "plugin installed" "[ -f '$D/plugin/portalcraft_bridge.so' ]"
check "marker file for the plugin" "[ -f '$D/.portalcraft-root' ]"
check "mod + fabric api in the profile" "[ -f '$D/prism/instances/Portalcraft/.minecraft/mods/portalcraft-1.0.0.jar' ] && [ -f '$D/prism/instances/Portalcraft/.minecraft/mods/fabric-api-1.0+26.3.jar' ]"
check "textures unpacked from the jar" "[ -f '$D/cache/minecraft-26.3/resourcepacks/local/minecraft-26.3/assets/minecraft/textures/block/dirt.png' ] && [ -f '$D/cache/minecraft-26.3/resourcepacks/local/minecraft-26.3/assets/minecraft/lang/en_us.json' ]"
check "setup_done in state" "rg -q '\"setup_done\": true' '$D/state.json'"
check "app-menu entry added (in the test's own XDG_DATA_HOME), with the icon" "rg -q '^Icon=$D/portalcraft.png' '$T/xdg/applications/portalcraft.desktop' && [ -s '$D/portalcraft.png' ] && rg -q 'shortcut_made\": true' '$D/state.json'"
sleep 2
check "the Minecraft that setup started was closed again" "! pgrep -af 'java' | rg -q 'portalcraft.root=$D'"
check "JVM args carry the data folder" "rg -q 'portalcraft.root=$D' '$D/prism/instances/Portalcraft/instance.cfg'"
touch "$D/prism/instances/Portalcraft/.minecraft/mods/e4mc-fabric-6.2.2.jar"

echo "== 3. second run is a no-op; then a new release (plugin v2, mod v2) updates, old files go"
"$BIN" --cli setup 2>&1 | rg -c 'Done|Skipped' | sed 's/^/   steps done: /'
mkmanifest plugin2.so mod2.jar 0.1.0
"$BIN" --cli setup 2>&1 | rg 'Install the Portal 2 plugin|Set up the Minecraft' | tail -2 | sed 's/^/   /'
check "plugin is v2" "rg -q 'plugin v2' '$D/plugin/portalcraft_bridge.so'"
check "mod v2 in, mod v1 and e4mc out" "[ -f '$D/prism/instances/Portalcraft/.minecraft/mods/portalcraft-2.0.0.jar' ] && [ ! -e '$D/prism/instances/Portalcraft/.minecraft/mods/portalcraft-1.0.0.jar' ] && [ -z \"\$(ls $D/prism/instances/Portalcraft/.minecraft/mods | rg e4mc)\" ]"

echo "== 4. repair: a damaged plugin is found by verify and fetched again"
echo "garbage" > "$D/plugin/portalcraft_bridge.so"
"$BIN" --cli verify 2>&1 | rg 'repair' | sed 's/^/   /'
"$BIN" --cli setup >/dev/null 2>&1
check "plugin repaired" "rg -q 'plugin v2' '$D/plugin/portalcraft_bridge.so'"

echo "== 5. offline: saved list is used when the server is gone; offline mode with no list says so"
export PORTALCRAFT_MANIFEST_URL=$T/dist/nonexistent.json
"$BIN" --cli setup 2>&1 | rg 'Check for the newest' | tail -1 | sed 's/^/   /'
check "setup still passes from the saved list" "rg -q 'setup complete' <("$BIN" --cli setup 2>&1)"
rm -f "$D/launcher.json" "$D/launcher.base"
echo '{"offline":true}' > "$D/settings.json"
"$BIN" --cli setup 2>&1 | rg -A1 'Offline mode is on' | head -2 | sed 's/^/   /'
echo '{}' > "$D/settings.json"
export PORTALCRAFT_MANIFEST_URL=$T/dist/launcher.json

echo "== 6. self-update replaces the program, hash-checked"
cp "$BIN" "$T/lcdir/portalcraft-launcher"
mkmanifest plugin2.so mod2.jar 0.9.0
"$T/lcdir/portalcraft-launcher" --cli selfupdate 2>&1 | tail -2 | sed 's/^/   /'
check "launcher file is the new one" "cmp -s '$T/dist/launcher-new' '$T/lcdir/portalcraft-launcher'"
check "no temp file left" "[ -z \"\$(ls -A $T/lcdir | rg '\\.new')\" ]"

echo "== 7. bug report"
HOME=$T "$BIN" --cli bugreport | tail -1 | sed 's/^/   /'
check "zip exists in HOME with the logs" "unzip -l $T/Portalcraft-bugreport-*.zip | rg -q 'launcher.log' && unzip -l $T/Portalcraft-bugreport-*.zip | rg -q 'system.txt'"
check "no account file in the zip" "! unzip -l $T/Portalcraft-bugreport-*.zip | rg -q accounts"

kill $(cat "$T/java.pids" 2>/dev/null) 2>/dev/null
rm -rf "$T"
[ $fail = 0 ] && echo "ALL PASSED" || echo "FAILED"
exit $fail
