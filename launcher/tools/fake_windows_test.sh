#!/bin/bash
# Integration test of the WINDOWS launcher under wine, with fakes: no network, no Microsoft sign-in, no real Prism,
# Minecraft, Steam or Portal 2. Runs setup, the "no Windows plugin yet" Play message, a release that does have a plugin,
# the whole Play flow (fake steam.exe -> fake portal2.exe -> fake bridge log), the prismlauncher:// registry handling,
# self-update (rename aside) and the bug report.
#   launcher/build_windows.sh && launcher/tools/fake_windows_test.sh [path/to/portalcraft-launcher.exe]
# Needs: wine, xvfb-run, mingw-w64 gcc, python3. Uses a throwaway WINEPREFIX under /tmp (never ~/.wine).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
EXE=$(realpath "${1:-"$HERE/../../target-launcher-win/x86_64-pc-windows-gnu/release/portalcraft-launcher.exe"}")
[ -f "$EXE" ] || { echo "no $EXE: run launcher/build_windows.sh first" >&2; exit 2; }
for c in wine xvfb-run x86_64-w64-mingw32-gcc python3; do command -v $c >/dev/null || { echo "$c is missing" >&2; exit 2; }; done
# the whole test runs on its own virtual display so no window pops up on the real desktop
[ -z "${PC_XVFB:-}" ] && exec env PC_XVFB=1 xvfb-run -a "$0" "$@"

T=$(mktemp -d /tmp/pc-fakewin.XXXXXX)
export WINEPREFIX=$T/prefix WINEARCH=win64 WINEDEBUG=-all
[ "$WINEPREFIX" = "$HOME/.wine" ] && { echo "refusing to use ~/.wine" >&2; exit 2; }
wineboot -i >/dev/null 2>&1; wineserver -w
C=$WINEPREFIX/drive_c
D=$C/pcdata
export PORTALCRAFT_DATA='C:\pcdata'
mkdir -p "$T/dist" "$T/src" "$T/fakes" "$T/lcdir"
fail=0
check() { local t0=$SECONDS; if eval "$2"; then echo "PASS  $1 ($((SECONDS-t0))s)"; else echo "FAIL  $1 ($((SECONDS-t0))s)"; fail=1; fi; }
# A wine run, CR/LF cleaned. Output goes through a FILE, not a pipe: Prism and the games the launcher starts inherit the
# launcher's stdout, and a pipe would stay open until they exit (minutes).
W() { wine "$@" > "$T/w.$$.out" 2>&1; tr -d '\r' < "$T/w.$$.out"; }
L() { W "$EXE" "$@"; }                                # the launcher
# wine processes do not show up under their Windows names in ps: ask wine's own tasklist
count() { W tasklist | rg -ci "$1" || true; }
alive() { [ "$(count "$1")" -gt 0 ]; }

# ---- the fakes (C programs built with mingw-w64)
for n in fakeprism fakesteam fakeportal2 javaw; do x86_64-w64-mingw32-gcc -O1 -s -o "$T/fakes/$n.exe" "$HERE/winfakes/$n.c" || exit 2; done
python3 -I - "$T/src/fake-client.jar" <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1], "w")
z.writestr("assets/minecraft/textures/block/dirt.png", "png")
z.writestr("assets/minecraft/lang/en_us.json", "{}")
z.close()
PY
python3 -I - "$T" <<'PY'
import sys, zipfile
t = sys.argv[1]
z = zipfile.ZipFile(t + "/dist/prism.zip", "w", zipfile.ZIP_DEFLATED)
z.write(t + "/fakes/fakeprism.exe", "prismlauncher.exe")
z.write(t + "/fakes/javaw.exe", "javaw.exe")
z.write(t + "/src/fake-client.jar", "fake-client.jar")
z.writestr("portable.txt", "")
z.writestr("iconengines/", "")
z.writestr("jars/NewLaunch.jar", "x")
z.close()
PY
echo "plugin v1 pc_world" > "$T/dist/plugin1.dll"
echo "mod v1" > "$T/dist/mod1.jar"; echo "api" > "$T/dist/api.jar"
cp "$EXE" "$T/dist/launcher-new.exe"; echo "padding" >> "$T/dist/launcher-new.exe"
mkmanifest() {  # $1 launcher version  $2 with-plugin(0/1)  $3 prism sha override
python3 -I - "$T" "$1" "$2" "${3:-}" <<'PY'
import hashlib, json, sys
t, lv, with_plugin, override = sys.argv[1:5]
d = t + "/dist/"
def e(version, f, sha=None):
    b = open(d + f, "rb").read()
    return {"version": version, "url": "asset:" + f, "sha256": sha or hashlib.sha256(b).hexdigest(), "size": len(b)}
win = {"launcher": e(lv, "launcher-new.exe"), "prism": e("11.1.1", "prism.zip", override or None)}
if with_plugin == "1": win["plugin"] = e("1.0.0", "plugin1.dll")
# the top level is exactly what a Linux-only schema-1 release always had: a Windows launcher must ignore it
m = {"schema": 1, "release": "t", "launcher": {"version": "9.9.9", "url": "asset:nope", "sha256": "00", "size": 1},
     "plugin": {"version": "9.9.9", "url": "asset:nope", "sha256": "00", "size": 1},
     "mod": e("1.0.0", "mod1.jar"), "fabric_api": e("1.0+26.3", "api.jar"),
     "prism": {"version": "9.9.9", "url": "asset:nope.tar.gz", "sha256": "00", "size": 1},
     "platforms": {"windows-x86_64": win}}
json.dump(m, open(d + "launcher.json", "w"), indent=1)
PY
}
export PORTALCRAFT_MANIFEST_URL=$(winepath -w "$T/dist/launcher.json" 2>/dev/null | tr -d '\r')

# ---- a fake Steam in the registry, with Portal 2 in a second library
mkdir -p "$C/FakeSteam/steamapps" "$C/Games/SteamLib/steamapps/common/Portal 2/portal2/cfg"
touch "$C/Games/SteamLib/steamapps/common/Portal 2/portal2/gameinfo.txt"
cp "$T/fakes/fakesteam.exe" "$C/FakeSteam/steam.exe"
cp "$T/fakes/fakeportal2.exe" "$C/Games/SteamLib/steamapps/common/Portal 2/portal2.exe"
printf '"AppState"\n{\n\t"installdir"\t\t"Portal 2"\n}\n' > "$C/Games/SteamLib/steamapps/appmanifest_620.acf"
printf '"libraryfolders"\n{\n\t"0"\n\t{\n\t\t"path"\t\t"C:\\\\FakeSteam"\n\t}\n\t"1"\n\t{\n\t\t"path"\t\t"C:\\\\Games\\\\SteamLib"\n\t}\n}\n' > "$C/FakeSteam/steamapps/libraryfolders.vdf"
wine reg add 'HKCU\Software\Valve\Steam' /v SteamPath /d 'c:/fakesteam' /f >/dev/null 2>&1
wine reg add 'HKCU\Software\Valve\Steam' /v SteamExe /d 'c:/fakesteam/steam.exe' /f >/dev/null 2>&1
export FAKE_P2_EXE='C:\Games\SteamLib\steamapps\common\Portal 2\portal2.exe'

echo "[$(date +%T)] == 0. Steam found through the registry and the second library; data folder is where PORTALCRAFT_DATA says"
L --cli detect | sed 's/^/   /'
check "Portal 2 found" "L --cli detect | rg -q 'Portal 2: C:.Games.SteamLib.steamapps.common.Portal 2'"

echo "[$(date +%T)] == 1. a bad checksum is refused and nothing half-written stays"
mkmanifest 0.1.0 0 0000000000000000000000000000000000000000000000000000000000000000
L --cli setup | rg -A1 'damaged' | head -3 | sed 's/^/   /'
check "no Prism installed" "[ ! -e '$D/prism/prismlauncher.exe' ]"
check "no .part left" "[ -z \"\$(ls $D/downloads | rg '\\.part$')\" ]"

echo "[$(date +%T)] == 2. full setup (release has no Windows plugin yet); the 'sign-in' appears 8 s in; no /dev/shm step"
mkmanifest 0.1.0 0
( sleep 8; echo '{"accounts":[{"type":"MSA","profile":{"id":"fake","name":"Fake"}}],"formatVersion":3}' > "$D/prism/accounts.json" ) &
timeout 180 wine "$EXE" --cli setup > "$T/setup.out" 2>&1; tr -d '\r' < "$T/setup.out" | rg -v '^\[20' | tail -14 | sed 's/^/   /'
check "Prism unpacked from the zip" "[ -f '$D/prism/prismlauncher.exe' ] && [ -f '$D/prism/portable.txt' ] && [ -f '$D/prism/jars/NewLaunch.jar' ]"
check "no plugin installed, setup not failed by it" "[ ! -e '$D/plugin/portalcraft_bridge.dll' ] && rg -q 'setup_done.: true' '$D/state.json'"
check "no shared-memory step" "! rg -q 'dev/shm' '$D/logs/cli-output.txt'"
check "mod + fabric api in the profile" "[ -f '$D/prism/instances/Portalcraft/.minecraft/mods/portalcraft-1.0.0.jar' ] && [ -f '$D/prism/instances/Portalcraft/.minecraft/mods/fabric-api-1.0+26.3.jar' ]"
check "textures unpacked from the jar" "[ -f '$D/cache/minecraft-26.3/resourcepacks/local/minecraft-26.3/assets/minecraft/textures/block/dirt.png' ]"
sleep 2
check "the Minecraft that setup started was closed again" "! alive 'javaw.exe'"
check "JVM args use forward slashes and the data folder" "rg -q 'portalcraft.root=C:/pcdata' '$D/prism/instances/Portalcraft/instance.cfg'"
check "marker file for the plugin" "[ -f '$D/.portalcraft-root' ]"

echo "[$(date +%T)] == 3. second run is a no-op"
L --cli setup | rg -c 'Done|Skipped' | sed 's/^/   steps done or skipped: /'
check "status: everything ok" "! L --cli status | rg -q MISSING"

echo "[$(date +%T)] == 4. Play with no Windows plugin in the release: a friendly message, nothing started"
L --cli play | tail -4 | sed 's/^/   /'
check "says Windows cannot start the game yet" "L --cli play | rg -q \"can't start the game yet\""
check "nothing was started and Portal 2's folder is untouched" "! alive 'javaw.exe' && ! alive 'portal2.exe' && [ -z \"\$(ls '$C/Games/SteamLib/steamapps/common/Portal 2/portal2/cfg')\" ]"

echo "[$(date +%T)] == 5. a release WITH a Windows plugin: installed as .dll, hash-checked"
mkmanifest 0.1.0 1
L --cli setup | rg 'Install the Portal 2 plugin' | tail -1 | sed 's/^/   /'
check "plugin dll installed" "rg -q 'plugin v1' '$D/plugin/portalcraft_bridge.dll'"
echo "garbage" > "$D/plugin/portalcraft_bridge.dll"
L --cli verify | rg 'repair' | sed 's/^/   /'
L --cli setup >/dev/null
check "damaged plugin repaired" "rg -q 'plugin v1' '$D/plugin/portalcraft_bridge.dll'"

echo "[$(date +%T)] == 6. Play: fake steam.exe starts fake portal2.exe; the temporary autoexec is written, used and removed"
echo 'sv_cheats 1' > "$C/Games/SteamLib/steamapps/common/Portal 2/portal2/cfg/autoexec.cfg"
echo '{"seed_random":false,"seed":"4242","start_mode":"mc","offline":true}' > "$D/settings.json"
L --cli play | rg -v '^\s+\[20' | tail -8 | sed 's/^/   /'
check "READY (level init + link up in the bridge log)" "rg -q 'level init sp_a1_intro1' '$D/cache/bridge/bridge.log' && rg -q 'link up' '$D/cache/bridge/bridge.log'"
check "the game saw plugin_load with a quoted forward-slash .dll path" "rg -q 'link: Local' '$D/cache/bridge/bridge.log'"
check "own autoexec.cfg is back, ours is gone" "[ \"\$(tr -d '\r' < '$C/Games/SteamLib/steamapps/common/Portal 2/portal2/cfg/autoexec.cfg')\" = 'sv_cheats 1' ] && [ -z \"\$(ls '$C/Games/SteamLib/steamapps/common/Portal 2/portal2/cfg' | rg -v '^autoexec.cfg\$')\" ]"
check "Minecraft (javaw.exe) is running and the profile carries the fixed seed" "alive 'javaw.exe' && rg -q 'skycraft.seed=4242' '$D/prism/instances/Portalcraft/instance.cfg'"
check "status sees both games" "L --cli status | rg -q 'portal2 running: \\[[0-9]' && L --cli status | rg -q 'minecraft running: \\[[0-9]'"
echo "[$(date +%T)] == 6b. Play again while Portal 2 is running with the plugin: attaches, Minecraft reused"
N1=$(count javaw.exe)
L --cli play | rg -i 'reusing|READY|already|healthy' | sed 's/^/   /'
check "still exactly one Minecraft (reused, not started twice)" "[ $N1 = 1 ] && [ \$(count javaw.exe) = 1 ]"
W taskkill /F /IM portal2.exe >/dev/null; W taskkill /F /IM javaw.exe >/dev/null; sleep 2

echo "[$(date +%T)] == 7. prismlauncher:// handler: borrowed during sign-in, handed back after"
rm -f "$D/prism/accounts.json"; rm -f "$D/state.json"
wine reg delete 'HKCU\Software\Classes\prismlauncher' /f >/dev/null 2>&1
L --cli prism | sed 's/^/   /'
check "our Prism is the prismlauncher:// handler while signing in" "W reg query 'HKCU\\Software\\Classes\\prismlauncher\\shell\\open\\command' | rg -qi 'pcdata.prism.prismlauncher.exe'"
echo '{"accounts":[{"type":"MSA","profile":{"id":"fake","name":"Fake"}}],"formatVersion":3}' > "$D/prism/accounts.json"
L --cli status >/dev/null
check "handler gone again after sign-in (there was none before)" "! W reg query 'HKCU\\Software\\Classes\\prismlauncher' | rg -qi 'prismlauncher.exe'"
rm -f "$D/prism/accounts.json"
wine reg add 'HKCU\Software\Classes\prismlauncher\shell\open\command' /ve /d '"C:\Their\Prism\prismlauncher.exe" "%1"' /f >/dev/null 2>&1
L --cli prism >/dev/null
echo '{"accounts":[{"type":"MSA","profile":{"id":"fake","name":"Fake"}}],"formatVersion":3}' > "$D/prism/accounts.json"
L --cli status >/dev/null
check "the player's own handler is put back" "W reg query 'HKCU\\Software\\Classes\\prismlauncher\\shell\\open\\command' | rg -qi 'Their.Prism'"
W taskkill /F /IM prismlauncher.exe >/dev/null

echo "[$(date +%T)] == 8. self-update: the running .exe is renamed aside, the new one moves in, the .old.exe goes at the next start"
cp "$EXE" "$T/lcdir/portalcraft-launcher.exe"
mkmanifest 0.9.0 1
W "$(winepath -w "$T/lcdir/portalcraft-launcher.exe")" --cli selfupdate | tail -2 | sed 's/^/   /'
check "launcher file is the new one" "cmp -s '$T/dist/launcher-new.exe' '$T/lcdir/portalcraft-launcher.exe'"
check "the old program was kept aside as .old.exe" "cmp -s '$EXE' '$T/lcdir/portalcraft-launcher.old.exe'"
W "$(winepath -w "$T/lcdir/portalcraft-launcher.exe")" --cli status >/dev/null
check "the .old.exe is deleted at the next start" "[ ! -e '$T/lcdir/portalcraft-launcher.old.exe' ]"

echo "[$(date +%T)] == 9. a second launcher on the same data folder is refused"
W taskkill /F /IM prismlauncher.exe | head -2 | sed 's/^/   /'; sleep 2   # the stand-in Prism of section 7
rm -f "$D/prism/accounts.json"; echo '{}' > "$D/state.json"
( wine "$EXE" --cli setup > "$T/holder.out" 2>&1 & ) ; sleep 6        # waits for a sign-in that never comes: holds the lock
W "$EXE" --cli setup > "$T/second.out"
head -3 "$T/second.out" | sed 's/^/   /'
tail -3 "$T/holder.out" | sed 's/^/   holder: /'
check "the second one says Portalcraft is already open" "rg -q 'already open' '$T/second.out'"
wineserver -k 2>/dev/null; sleep 1
echo '{"accounts":[{"type":"MSA","profile":{"id":"fake","name":"Fake"}}],"formatVersion":3}' > "$D/prism/accounts.json"

echo "[$(date +%T)] == 10. bug report"
W "$EXE" --cli bugreport | tail -1 | sed 's/^/   /'
Z=$(ls $C/users/*/Portalcraft-bugreport-*.zip 2>/dev/null | head -1)
check "zip with the logs and system info" "[ -n '$Z' ] && unzip -l '$Z' | rg -q 'launcher.log' && unzip -l '$Z' | rg -q 'system.txt'"
check "no account file in the zip" "! unzip -l '$Z' | rg -q accounts"

wineserver -k 2>/dev/null
rm -rf "$T"
[ $fail = 0 ] && echo "ALL PASSED" || echo "FAILED"
exit $fail
