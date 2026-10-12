#!/bin/bash
# Builds the two player downloads from dist/ (made by release.sh / make_release.py):
#   dist-zips/Portalcraft-<ver>-Windows/  and  .zip
#   dist-zips/Portalcraft-<ver>-Linux/    and  .zip
# Each holds the launcher plus a files/ folder with everything setup needs: the Portal 2 plugin, the mod, Fabric API,
# the portable Prism Launcher, the Prism profile template and the release list. The launcher uses files/ instead of
# downloading (engine.rs bundle_dir); only Minecraft and Java come from the internet at setup (they can't be shipped).
# Prism and Fabric API are the pinned files from dist/launcher.json, downloaded once into cache/release-pins/ and
# checked against their sha256.
#   launcher/tools/make_release_zips.sh            (after release.sh <ver>)
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
M=dist/launcher.json
[ -f "$M" ] || { echo "no $M: run ./release.sh <ver> first" >&2; exit 1; }
PINS=cache/release-pins
mkdir -p "$PINS"
VER=$(python3 -c "import json;print(json.load(open('$M'))['release'])")
OUT=dist-zips
rm -rf "$OUT"; mkdir -p "$OUT"

# the pinned third-party files (and Prism's licence: GPL-3.0, shipped unmodified)
python3 - "$M" "$PINS" <<'PY'
import hashlib, json, os, sys, urllib.request
m = json.load(open(sys.argv[1])); pins = sys.argv[2]
for a in (m["prism"], m["fabric_api"], m["platforms"]["windows-x86_64"]["prism"]):
    name = a["url"].rsplit("/", 1)[1].replace("%2B", "+"); p = os.path.join(pins, name)
    ok = lambda: os.path.exists(p) and hashlib.sha256(open(p, "rb").read()).hexdigest() == a["sha256"]
    if not ok():
        print("downloading", name); urllib.request.urlretrieve(a["url"], p)
    if not ok(): sys.exit("checksum mismatch: " + name)
for f in ("LICENSE", "COPYING.md"):
    p = os.path.join(pins, "prism-" + f)
    if not os.path.exists(p):
        urllib.request.urlretrieve("https://raw.githubusercontent.com/PrismLauncher/PrismLauncher/11.1.1/" + f, p)
PY
pin() { python3 -c "import json,sys;m=json.load(open('$M'));a=eval(sys.argv[1]);print(a['url'].rsplit('/',1)[1].replace('%2B','+'))" "$1"; }
PRISM_LIN=$(pin "m['prism']"); PRISM_WIN=$(pin "m['platforms']['windows-x86_64']['prism']"); FAPI=$(pin "m['fabric_api']")
JAR=$(python3 -c "import json;print(json.load(open('$M'))['mod']['url'].split(':',1)[1])")

notices() {  # $1 folder
  mkdir -p "$1/licences"
  cp LICENSE "$1/licences/Portalcraft-LICENSE.txt"; cp NOTICE "$1/licences/Portalcraft-NOTICE.txt"
  cp "$PINS/prism-LICENSE" "$1/licences/PrismLauncher-LICENSE.txt"; cp "$PINS/prism-COPYING.md" "$1/licences/PrismLauncher-COPYING.md"
  cat > "$1/licences/README.txt" <<EOT
Portalcraft is MIT licensed (Portalcraft-LICENSE.txt); its third-party notices are in Portalcraft-NOTICE.txt.
Source code: https://github.com/Stink-O/portalcraft

files/ also contains, unmodified:
- Prism Launcher 11.1.1 (GPL-3.0: PrismLauncher-LICENSE.txt, PrismLauncher-COPYING.md).
  Source code: https://github.com/PrismLauncher/PrismLauncher/tree/11.1.1
- Fabric API $FAPI (Apache-2.0; its licence is inside the jar). Source code: https://github.com/FabricMC/fabric
Minecraft and Java are not included: Prism downloads them from Mojang/Microsoft with your own account.
EOT
}

# ---- Windows
W="$OUT/Portalcraft-$VER-Windows"
mkdir -p "$W/files"
cp dist/portalcraft-launcher-windows-x86_64.exe "$W/Portalcraft.exe"
cp "$M" dist/portalcraft_bridge.dll "dist/$JAR" dist/instance-template.tar.gz "$W/files/"
cp "$PINS/$PRISM_WIN" "$PINS/$FAPI" "$W/files/"
notices "$W"
cat > "$W/README.txt" <<EOT
PORTALCRAFT $VER for Windows
============================

Play Portal 2's campaign as a Minecraft player.

You need:
  - Portal 2, installed through Steam (you must own it)
  - Minecraft: Java Edition, on your Microsoft account (you must own it)
  - Steam running

HOW TO START
  1. Unzip this whole folder somewhere you'll keep it (for example Documents).
     Keep Portalcraft.exe and the "files" folder together.
  2. Double-click Portalcraft.exe.
     If Windows says "Windows protected your PC", click "More info", then "Run anyway".
     (The launcher isn't code-signed yet.)
  3. Press "Set up Portalcraft".
     A Prism Launcher window opens: click the account button at the top right,
     "Manage Accounts", "Add Microsoft", and sign in with the account that owns Minecraft.
     Setup then downloads Minecraft and Java (a few minutes).
  4. Press "Play the campaign".

Keys in game: J switches between the Minecraft player and Chell, hold G while firing a
portal to open it onto the Minecraft world, E inventory, O Minecraft's menu, F8 marks a bug.

This is an early, experimental release: expect bugs and visual glitches, especially in the
Minecraft world. Problems: press F8 in game, then "Send a bug report" in the launcher, and
post the zip at https://github.com/Stink-O/portalcraft/issues

More help: https://github.com/Stink-O/portalcraft
EOT

sed -i 's/$/\r/' "$W/README.txt" "$W/licences/README.txt"   # Windows line endings for Notepad

# ---- Linux
L="$OUT/Portalcraft-$VER-Linux"
mkdir -p "$L/files"
cp dist/portalcraft-launcher-linux-x86_64 "$L/Portalcraft"; chmod +x "$L/Portalcraft"
cp "$M" dist/portalcraft_bridge.so "dist/$JAR" dist/instance-template.tar.gz dist/map-tools.tar.gz "$L/files/"
cp "$PINS/$PRISM_LIN" "$PINS/$FAPI" "$L/files/"
notices "$L"
cat > "$L/README.txt" <<EOT
PORTALCRAFT $VER for Linux
==========================

Play Portal 2's campaign as a Minecraft player.

You need:
  - Steam (the normal package, not Flatpak) with Portal 2 installed (the Linux version,
    Steam's default; don't force Proton for it)
  - Minecraft: Java Edition on your Microsoft account
  - 64-bit Linux with glibc 2.35 or newer (Ubuntu 22.04, Fedora 36, Debian 12 or later)

HOW TO START
  1. Extract this whole folder somewhere you'll keep it. Keep "Portalcraft" and the
     "files" folder together.
  2. Run Portalcraft: double-click it, or in a terminal:
         cd Portalcraft-$VER-Linux
         ./Portalcraft
     If it won't start, make it executable first:
         chmod +x Portalcraft
     (or right-click it, Properties, Permissions, "Allow executing file as program").
  3. Press "Set up Portalcraft". In the Prism window: account button (top right),
     "Manage Accounts", "Add Microsoft". Prism shows a code: open microsoft.com/link,
     enter it and sign in. Setup then downloads Minecraft and Java.
     Settings has "Add to the application menu".
  4. Press "Play the campaign".

Everything Portalcraft installs lives in ~/.local/share/portalcraft (delete it to remove
Portalcraft, including its Minecraft world).

Keys: J Minecraft player / Chell, hold G while firing a portal for the Minecraft world,
E inventory, O Minecraft's menu, F8 marks a bug ("Send a bug report" in the launcher).

Early and experimental: expect bugs and visual glitches in the Minecraft world.
Issues: https://github.com/Stink-O/portalcraft/issues
EOT

# ---- zips (Linux: zip keeps the executable bit for unzip/Ark/File Roller)
( cd "$OUT" && for d in Portalcraft-$VER-Windows Portalcraft-$VER-Linux; do rm -f "$d.zip"; zip -qr -X "$d.zip" "$d"; done )
ls -la "$OUT"
for d in "$OUT"/*/; do echo "== $d"; (cd "$d" && find . -type f | sort | xargs -d '\n' ls -la | awk '{printf "  %10s  %s\n", $5, $9}'); done
