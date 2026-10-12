#!/bin/bash
# Integration test: setup from a release zip's bundled files (the files/ folder next to the launcher) with the server unreachable.
# sign-in (the "sign-in" is a fake accounts.json written by this script), no real Prism, Minecraft or Portal 2.
#   launcher/tools/fake_setup_test.sh [path/to/portalcraft-launcher]
set -u
pgrep -x portal2_linux >/dev/null && { echo "a real Portal 2 is running: not running this test" >&2; exit 2; }
BIN=$(realpath "${1:-"$(dirname "$0")/../../target-launcher/release/portalcraft-launcher"}")
T=$(mktemp -d /tmp/pc-fakebundle.XXXXXX)
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
# the release zip: the launcher with files/ next to it; the release list on the server cannot be reached
mkdir -p "$T/app/files"; cp "$BIN" "$T/app/portalcraft-launcher"
cp "$T/dist/"{launcher.json,plugin1.so,mod1.jar,api.jar,prism.tar.gz} "$T/app/files/"
export PORTALCRAFT_MANIFEST_URL=http://127.0.0.1:9/launcher.json

echo "== setup with no server: everything comes from files/"
( sleep 6; echo '{"accounts":[{"type":"MSA","profile":{"id":"fake","name":"Fake"}}],"formatVersion":3}' > "$D/prism/accounts.json" ) &
timeout 120 "$T/app/portalcraft-launcher" --cli setup 2>&1 | rg -i 'came with the launcher|Failed' | head -8 | sed 's/^/   /'
check "plugin installed from the bundle" "[ -f '$D/plugin/portalcraft_bridge.so' ] && rg -q 'plugin v1' '$D/plugin/portalcraft_bridge.so'"
check "mod + fabric api in the profile" "[ -f '$D/prism/instances/Portalcraft/.minecraft/mods/portalcraft-1.0.0.jar' ] && [ -f '$D/prism/instances/Portalcraft/.minecraft/mods/fabric-api-1.0+26.3.jar' ]"
check "Prism unpacked" "[ -x '$D/prism/PrismLauncher' ]"
check "setup_done in state" "rg -q '\"setup_done\": true' '$D/state.json'"
sleep 2
for p in $(cat "$T/java.pids" 2>/dev/null); do kill "$p" 2>/dev/null; done
[ $fail = 0 ] && echo "ALL PASSED" || echo "SOME FAILED"
rm -rf "$T"
exit $fail
