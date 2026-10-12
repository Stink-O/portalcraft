#!/bin/bash
# Makes a Portalcraft release in one go: sets the version everywhere, builds the plugin, the mod and the launcher for
# old and new distributions, runs the launcher's tests, writes dist/ with launcher.json, and (with --publish) creates
# the GitHub release. Players' launchers then offer the update by themselves.
#
#   ./release.sh 0.1.1                 build and test into dist/, publish nothing
#   ./release.sh 0.1.1 --publish       ... then create the GitHub release v0.1.1 with every file in dist/
#   options: --notes <file>  release notes (default: a short text you can edit on GitHub)
#            --windows       also build Windows: the launcher and the plugin (PLUGIN_WIN=<dll> ships a DLL built on Windows instead)
#            --no-tests      skip the launcher tests (not recommended)
#
# Needs: podman (or CONTAINER=docker) for the portable builds, Java 25 for the mod, Rust, python3, gh (for --publish).
set -euo pipefail
cd "$(dirname "$0")"

VER=${1:-}
[[ $VER =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "usage: ./release.sh <version, e.g. 0.1.1> [--publish] [--notes file] [--windows] [--no-tests]" >&2; exit 2; }
shift
PUBLISH=0 WINDOWS=0 TESTS=1 NOTES=""
while [ $# -gt 0 ]; do
  case $1 in
    --publish) PUBLISH=1 ;;
    --windows) WINDOWS=1 ;;
    --no-tests) TESTS=0 ;;
    --notes) NOTES=$2; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
step() { printf '\n== %s\n' "$*"; }

if [ $PUBLISH = 1 ]; then
  command -v gh >/dev/null || { echo "gh (GitHub CLI) is needed for --publish" >&2; exit 1; }
  if git rev-parse --git-dir >/dev/null 2>&1 && [ -n "$(git status --porcelain)" ]; then
    echo "The working tree has uncommitted changes. Commit (and push) first, so the release matches the code on GitHub." >&2
    exit 1
  fi
  gh release view "v$VER" -R Stink-O/portalcraft >/dev/null 2>&1 && { echo "release v$VER already exists on GitHub" >&2; exit 1; }
fi

step "version $VER"
# The launcher compares versions to offer self-updates; the mod and plugin carry the release name in launcher.json.
sed -i "0,/^version = \".*\"/s//version = \"$VER\"/" launcher/Cargo.toml
sed -i "s/^version=.*/version=$VER/" bridge/fabric/gradle.properties
grep -q "^version = \"$VER\"" launcher/Cargo.toml && grep -q "^version=$VER" bridge/fabric/gradle.properties

step "plugin (Ubuntu 22.04 container: loads on glibc 2.35 and newer)"
bridge/tools/build_release_plugin.sh

step "mod"
(cd bridge/fabric && ./gradlew build -q)
JAR=bridge/fabric/build/libs/portalcraft-$VER.jar
[ -f "$JAR" ] || { echo "mod jar $JAR missing" >&2; exit 1; }

step "launcher (Debian 12 container: runs on glibc 2.35 and newer)"
# the container build updates Cargo.lock's own version line; --locked needs it current first
(cd launcher && cargo update --workspace --offline -q)
launcher/tools/build_release_container.sh
LAUNCHER=target-launcher-release/release/portalcraft-launcher
WINARGS=()
if [ $WINDOWS = 1 ]; then
  step "Windows launcher"
  launcher/build_windows.sh
  # the Windows plugin: PLUGIN_WIN=<dll> to ship one built on Windows, else cross-built here (MinGW, podman)
  if [ -z "${PLUGIN_WIN:-}" ]; then bridge/tools/build_windows_plugin.sh >/dev/null; PLUGIN_WIN=bridge/build/win/portalcraft_bridge.stripped.dll; fi
  WINARGS=(--launcher-win target-launcher-win/x86_64-pc-windows-gnu/release/portalcraft-launcher.exe --plugin-win "$PLUGIN_WIN")
fi

if [ $TESTS = 1 ]; then
  step "launcher tests"
  CARGO_TARGET_DIR=target-launcher cargo test --release -q --manifest-path launcher/Cargo.toml
  if pgrep -x portal2_linux >/dev/null; then
    echo "Portal 2 is running: skipping the two end-to-end launcher tests (they never touch a real game)"
  else
    launcher/tools/fake_setup_test.sh "$LAUNCHER"
    launcher/tools/fake_play_test.sh "$LAUNCHER"
  fi
fi

step "dist/"
rm -rf dist
python3 launcher/tools/make_release.py --launcher "$LAUNCHER" --plugin bridge/build/release/portalcraft_bridge.so \
  --mod "$JAR" --release "$VER" --check-pins "${WINARGS[@]}"

if [ $PUBLISH = 0 ]; then
  step "done (not published)"
  echo "Everything is in dist/. To publish: ./release.sh $VER --publish (after committing and pushing)."
  exit 0
fi

step "publish v$VER"
# Two places: the main repository's release holds ONLY the launchers (what players download); the files the launcher
# fetches by itself (plugin, mod, templates, launcher.json) go to the separate portalcraft-updates repository, whose
# latest release is the launcher's default release list. Players never need to look there.
MAIN=Stink-O/portalcraft
FEED=Stink-O/portalcraft-updates
# the release page carries the two ready-to-go zips (launcher + files/ with everything setup needs)
launcher/tools/make_release_zips.sh >/dev/null
PLAYER_FILES=(dist-zips/Portalcraft-$VER-Windows.zip dist-zips/Portalcraft-$VER-Linux.zip)
for f in "${PLAYER_FILES[@]}"; do [ -f "$f" ] || { echo "missing $f (release.sh needs --windows for the Windows zip)" >&2; exit 1; }; done
if [ -z "$NOTES" ]; then
  NOTES=$(mktemp)
  cat > "$NOTES" <<EOT
<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://github.com/Stink-O/portalcraft/raw/main/docs/branding/portalcraft-logo-dark.svg">
    <img alt="Portalcraft" width="520" src="https://github.com/Stink-O/portalcraft/raw/main/docs/branding/portalcraft-logo-light.svg">
  </picture>
</p>

Portalcraft $VER

**New players:** download the Windows or Linux zip below, unzip it and open the launcher inside (see its README.txt).
**Already playing?** Open the launcher; it offers the update by itself.
EOT
fi
gh release view "v$VER" -R "$FEED" >/dev/null 2>&1 && { echo "release v$VER already exists in $FEED" >&2; exit 1; }
read -r -p "Publish v$VER: launchers to $MAIN, all $(ls dist | wc -l) files to $FEED? [y/N] " ok
[ "$ok" = y ] || { echo "not published"; exit 1; }
gh release create "v$VER" -R "$FEED" dist/* --title "Launcher files $VER" --notes "Files the Portalcraft launcher downloads by itself. To play, get the launcher from https://github.com/$MAIN/releases/latest"
gh release create "v$VER" -R "$MAIN" "${PLAYER_FILES[@]}" --title "Portalcraft $VER" --notes-file "$NOTES"
step "published"
echo "Check: curl -sIL https://github.com/$FEED/releases/latest/download/launcher.json | grep -i '^HTTP'"
