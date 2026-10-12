# Releasing Portalcraft (for the maintainer)

What a release is: five files on a GitHub release (Linux), plus `launcher.json` (the manifest), plus, for Windows, the Windows
launcher `.exe` (and, once it exists, the Windows plugin `.dll`; see "Windows" below). Players download only the
launcher; it fetches everything else, checks every download against `launcher.json`, and updates itself.

```
portalcraft-launcher-linux-x86_64   the launcher (egui, one native binary)
portalcraft_bridge.so               the 32-bit Portal 2 plugin
portalcraft-<version>.jar           the Fabric mod
instance-template.tar.gz            the Prism profile template (instance.cfg.in + mmc-pack.json)
map-tools.tar.gz                    optional: scripts that build the pit_flings demo map on the player's machine
launcher.json                       the manifest: version, url, sha256, size of all of the above, plus the pinned
                                    Prism Launcher and Fabric API
```

Nothing from Valve or Mojang is in any of these. Prism Launcher and Fabric API are not re-hosted: the launcher downloads
them from their own release pages and refuses them unless the sha256 in `launcher.json` matches (pins in
`launcher/pins.json`). Minecraft and Java are downloaded by Prism on the player's machine, after the player signs in.

## The short way

`./release.sh <version>` at the repository root does sections 1 and 2 below in one go (portable plugin and launcher
builds in containers, the mod, the tests, `dist/` with `launcher.json`), and `./release.sh <version> --publish` then
creates the GitHub release after asking. The plugin is built by `bridge/tools/build_release_plugin.sh` in an Ubuntu 22.04
container with static libstdc++: a plugin built on a new distro (Fedora 44, glibc 2.43) is refused by Portal 2 on older
ones. The sections below are what it does, step by step.

## 1. Build the artifacts

From the repository root. Use a clean checkout of the commit you are tagging.

```sh
# a. the plugin (32-bit; needs g++ -m32). Use your own output name; never overwrite build/dev while someone is playing.
cd bridge
make DEVSO=build/release/portalcraft_bridge.so OBJDIR=build/objrelease
strip --strip-debug build/release/portalcraft_bridge.so     # 29 MB -> about 3 MB
cd ..

# b. the mod (Java 25; Gradle is downloaded by the wrapper). e4mc is NOT included (it is opt-in: PORTALCRAFT_E4MC=1).
(cd bridge/fabric && ./gradlew build)                       # -> bridge/fabric/build/libs/portalcraft-<version>.jar
#    (not the -sources.jar)

# c. the launcher. Build it in a container so it runs on older distributions: a binary built on a brand new distro
#    (Fedora 44, glibc 2.43) needs that glibc and will not start on Ubuntu 22.04 / Debian 12.
launcher/tools/build_release_container.sh                   # podman, rust:bookworm -> target-launcher-release/release/portalcraft-launcher
#    quick local build for testing only:  launcher/build.sh   ->  target-launcher/release/portalcraft-launcher

# d. the manifest and the release folder (dist/)
launcher/tools/make_release.py \
  --launcher target-launcher-release/release/portalcraft-launcher \
  --plugin   bridge/build/release/portalcraft_bridge.so \
  --mod      bridge/fabric/build/libs/portalcraft-0.1.0.jar \
  --release  0.1.0 \
  --check-pins            # downloads the pinned Prism and Fabric API once and checks their hashes (skip for a quick run)
```

`make_release.py` reads the launcher version from `launcher/Cargo.toml` and the mod version from
`bridge/fabric/gradle.properties`, writes the five files plus `launcher.json` into `dist/`, and prints their sizes. By
default the manifest uses `asset:<file>` urls, which the launcher resolves next to wherever `launcher.json` was fetched
from, so the same `dist/` works on GitHub and in a local test folder. (Use `--base-url` for absolute urls.)

### Windows (optional part of a release)

The same crate builds a native Windows program. Cross-compile it on Linux (mingw-w64, `rustup target add x86_64-pc-windows-gnu`):

```sh
launcher/build_windows.sh        # -> target-launcher-win/x86_64-pc-windows-gnu/release/portalcraft-launcher.exe (about 11 MB, system DLLs only)
launcher/tools/make_release.py ... --launcher-win target-launcher-win/x86_64-pc-windows-gnu/release/portalcraft-launcher.exe
#   when the Windows plugin exists:   ... --plugin-win path/to/portalcraft_bridge.dll
```

This adds `portalcraft-launcher-windows-x86_64.exe` (and `portalcraft_bridge.dll`) to `dist/` and a `platforms` entry to
`launcher.json`. **The schema stays 1**: the top-level `launcher`, `plugin` and `prism` are still the Linux files, so every old
Linux launcher reads the new manifest unchanged and ignores `platforms`. The Windows launcher reads only
`platforms["windows-x86_64"]` (`launcher`, `prism`, optional `plugin`); a release without a Windows entry, or an old schema-1
release, makes it say "This release has no files for your system". Without `plugin` the Windows launcher still sets
everything up (Prism, profile, sign-in, textures) and Play says "Portalcraft for Windows can't start the game yet". Adding the
plugin later is only `--plugin-win`; no launcher change and no schema bump. A new system (macOS, arm64) is another key.
Bump the schema only for a change an old launcher would misread.

Windows Prism: the pinned portable zip `PrismLauncher-Windows-MinGW-w64-Portable-<v>.zip` (self-contained Qt, no Visual C++ runtime
needed), in `pins.json` as `prism_windows`.

## 2. Test before uploading

```sh
# launcher unit tests + both fake end-to-end tests (no network, no sign-in, no real game is started)
CARGO_TARGET_DIR=target-launcher cargo test --release --manifest-path launcher/Cargo.toml
launcher/tools/fake_setup_test.sh target-launcher-release/release/portalcraft-launcher
launcher/tools/fake_play_test.sh  target-launcher-release/release/portalcraft-launcher

# Windows launcher under wine (throwaway WINEPREFIX in /tmp, fake Steam registry, fake Prism/Minecraft/Portal 2; ~10 minutes)
launcher/tools/fake_windows_test.sh

# every screen, headless (writes PNGs)
xvfb-run -a target-launcher-release/release/portalcraft-launcher --screenshot /tmp/shots

# the real thing, on a machine with Steam + Portal 2 and a Microsoft account that owns Minecraft:
PORTALCRAFT_DATA=/tmp/pc-test PORTALCRAFT_MANIFEST_URL=$PWD/dist/launcher.json \
  target-launcher-release/release/portalcraft-launcher
```

The real test needs a human: press "Set up Portalcraft", sign in inside Prism, wait for Minecraft to download, press
"Play the campaign".

## 3. Upload

`./release.sh <version> --publish` does this. Two repositories, so the release players see stays simple:
1. `Stink-O/portalcraft`, release `v<version>`: **only the launchers** (`portalcraft-launcher-linux-x86_64`, and the
   Windows `.exe` once Windows can play). This is the page players download from.
2. `Stink-O/portalcraft-updates`, release `v<version>`: **every file in `dist/`**, including `launcher.json`. The
   launcher's default release list is `https://github.com/Stink-O/portalcraft-updates/releases/latest/download/launcher.json`,
   so the newest non-prerelease release there is what players' launchers install and update to. Players never visit it.
3. Check the three urls resolve (`curl -sIL` should end in 200) and that the sha256 of the uploaded launcher equals the
   one in `launcher.json` (`sha256sum`).
4. README: tell players to download `portalcraft-launcher-linux-x86_64`, `chmod +x` it (or right-click, Properties,
   Permissions, "Allow executing as program") and run it. Nothing else.

## What changes the manifest

| Change | What to do |
|---|---|
| New launcher | bump `version` in `launcher/Cargo.toml`, rebuild, rerun `make_release.py`. Old launchers see "A Portalcraft update is ready" and update themselves (`launcher.version` newer than the running one). |
| New plugin / mod | rebuild, rerun `make_release.py`. The launcher compares `version + sha256` with what it installed and reinstalls (never while Portal 2 or Minecraft runs). |
| Newer Minecraft, Fabric Loader or Fabric API | edit `launcher/prism/instance/mmc-pack.json` (`net.minecraft`, `net.fabricmc.intermediary`, `net.fabricmc.fabric-loader`), `launcher/pins.json` (Fabric API: url, sha256, size), `MC_VERSION` in `launcher/src/paths.rs`, and `bridge/fabric/gradle.properties` + `fabric.mod.json`. |
| Newer Prism | edit `launcher/pins.json` (url, sha256, size from the GitHub release page; the asset is `PrismLauncher-Linux-Qt6-Portable-<v>.tar.gz`), rerun `--check-pins`. |

## Where things end up on the player's machine

`~/.local/share/portalcraft/` on Linux, `%LOCALAPPDATA%\Portalcraft\` on Windows (`$PORTALCRAFT_DATA` overrides both; a test uses a
temp folder). On Windows `plugin\portalcraft_bridge.dll` replaces the `.so`, `prism\prismlauncher.exe` replaces `PrismLauncher`,
`launcher.lock` is a file opened with no sharing, and the launcher keeps `portalcraft-launcher.old.exe` next to itself for one start after a self-update:

```
.portalcraft-root                 marker: the plugin finds this folder by walking up from its .so (RepoRoot())
plugin/portalcraft_bridge.so      what `plugin_load` is given (absolute path, written into the temporary autoexec.cfg)
prism/                            portable Prism Launcher (portable.txt inside): its accounts, Java and Minecraft stay here
prism/instances/Portalcraft/      the profile; instance.cfg carries the -D properties of bridge/tools/run_mc.sh
prism/instances/Portalcraft/.minecraft/mods/{portalcraft-*.jar,fabric-api-*.jar}
prism/instances/Portalcraft/.minecraft/saves/Portalcraft   the player's world
cache/bridge/                     bridge.log, portalcraft_world.env, bugmarks/ (the plugin's and the mod's CacheDir())
cache/minecraft-26.3/resourcepacks/local/minecraft-26.3/assets/minecraft/   unpacked from the player's own client jar
downloads/  logs/  tools/  settings.json  state.json  launcher.json
```

Remove everything with `rm -rf ~/.local/share/portalcraft` (this also deletes the world and Prism's saved account).
Portal 2's own folder only ever gets `portal2/cfg/autoexec.cfg` (temporary) and, if the player builds the demo map,
`portal2/maps/pc_pit_flings.bsp`.

## Command line (for support)

```
portalcraft-launcher --cli detect | status | procs | setup | verify | bugreport | selfupdate | maps | prism | play
portalcraft-launcher --screenshot <dir>
```

On Windows the release program has no console of its own: `--cli`, `--help` and `--screenshot` attach to the console they were started
from, and `--cli` also writes everything it prints to `<data folder>\logs\cli-output.txt` (cmd.exe does not wait for a window
program, so the text file is the dependable copy). `--cli procs` lists the Portal 2 / Java / Prism processes the launcher sees.
