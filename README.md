<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/branding/portalcraft-logo-dark.svg">
    <img alt="Portalcraft" width="640" src="docs/branding/portalcraft-logo-light.svg">
  </picture>
</p>

# Portalcraft

**Play [Portal 2](https://store.steampowered.com/app/620/Portal_2/)'s whole campaign as a
[Minecraft](https://www.minecraft.net) player.** Portalcraft runs the real Portal 2 and the real Minecraft:
Java Edition side by side and joins them: Minecraft's hand, hotbar, health, inventory, building and digging inside
Aperture's test chambers, and portals that open onto a real Minecraft world.

<p align="center">
  <a href="https://github.com/Stink-O/portalcraft/releases/latest"><b>Download (Windows and Linux)</b></a>
  &nbsp;·&nbsp;
  <a href="#help-wanted"><b>Help wanted</b></a>
</p>

<p align="center">
  <a href="https://github.com/Stink-O/portalcraft/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/Stink-O/portalcraft"></a>
  <img alt="Windows and Linux" src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux-informational">
  <a href="#help-wanted"><img alt="Help wanted" src="https://img.shields.io/badge/help-wanted-e8820c"></a>
  <a href="https://store.steampowered.com/app/620/Portal_2/"><img alt="Portal 2 on Steam" src="https://img.shields.io/badge/Portal%202-Steam-1b2838?logo=steam&logoColor=white"></a>
  <img alt="Minecraft 26.3" src="https://img.shields.io/badge/Minecraft-26.3%20Fabric-62b47a">
  <a href="https://github.com/Stink-O/portalcraft/discussions"><img alt="Discussions" src="https://img.shields.io/badge/GitHub-discussions-24292f?logo=github"></a>
  <a href="LICENSE"><img alt="MIT license" src="https://img.shields.io/badge/license-MIT-blue"></a>
</p>

<!-- VIDEO: drag the trailer (.mp4, under 100 MB) into this README in GitHub's web editor; GitHub uploads it and
inserts a https://github.com/user-attachments/assets/... link here, which plays inline. -->

Portal 2 draws everything you see and keeps running its own puzzles. Minecraft runs hidden in the background and
supplies the blocks, the player and the world. A small plugin links the two, and a launcher sets everything up.

> [!WARNING]
> **Early, experimental and still quite broken.** Expect bugs and visual glitches on every build.
>
> - **The Minecraft world shows visual artifacts on all builds** (Windows and Linux): smearing, swirling leaves and
>   stretched edges, most visible when you go through a portal into the Minecraft world and when you look or move
>   around in it. It is the main open problem; see [Help wanted](#help-wanted).
> - Many campaign maps have been played, but not every one, and it has been developed on one Linux PC and one Windows PC.
> - **Windows is brand new** in 0.2.0: see [Known issues](#known-issues).

> [!IMPORTANT]
> **Help wanted.** I can't keep working on Portalcraft on my own, so it is open for anyone who wants to pick it up:
> fixes, testing on other PCs, or taking over whole features. Pull requests are welcome, and
> [Discussions](https://github.com/Stink-O/portalcraft/discussions) is the place to ask questions or say what you'd
> like to work on. Start with [Help wanted](#help-wanted) and [CONTRIBUTING.md](CONTRIBUTING.md).

**Contents:** [Help wanted](#help-wanted) ·
[Features](#features) ·
[New in 0.2.0](#new-in-020) ·
[Getting started](#getting-started) ·
[How to play](#how-to-play) ·
[Keys](#keys) ·
[Troubleshooting](#if-something-goes-wrong) ·
[To-do list](#the-to-do-list-what-is-left-to-build) ·
[Building from source](#building-from-source-for-developers)

## Features

- **The whole Portal 2 campaign as a Minecraft player.** Minecraft's own hand and held items, health and hunger,
  hotbar, inventory, crafting and third-person view (F5). J switches between the Minecraft player and Chell at any
  time.
- **Build and dig inside the test chambers.** Place and break blocks anywhere. Mining into a chamber wall or floor
  reveals real Minecraft ground behind it, with stone, ore and caves, so you can tunnel through walls and floors.
- **Portals onto a real Minecraft world.** Hold G while Chell fires a portal and its partner opens in a Minecraft
  landscape generated from your own random seed, so no two players get the same world. Walk through, explore, come
  back (hold H to go home).
- **Portal 2's machinery still works on you.** Faith plates, light bridges, excursion funnels, lasers, turrets, goo,
  gels, fizzlers, moving platforms and scripted scenes. Cubes, buttons and doors work for the Minecraft player too.
  Falls and hazards hurt.
- **A launcher that does the setup.** It finds Portal 2, sets up Minecraft through
  [Prism Launcher](https://prismlauncher.org) with your own Microsoft account, keeps everything up to date, and
  **Continue** picks the campaign up where you left off.
- **Easy bug reports.** F8 in game marks the spot and saves a replay; the launcher's "Send a bug report" zips it with
  the logs, without passwords or tokens.

## Help wanted

Portalcraft needs other people now. Anything helps, from one bug report to taking over a whole feature. The places
where help would make the biggest difference:

1. **The visual artifacts in the Minecraft world (the biggest problem).** Portal 2 shows Minecraft's picture by
   reshaping Minecraft's last frame to Portal 2's camera (reprojection). Minecraft's own frame is clean, but the
   reshaped one smears at depth edges and near geometry: swirling leaves, a smeared player in third person, stretched
   edges at the side of the screen. It is not the frame rate (Minecraft delivers ~90-120 fps on Windows). The likely
   cause is the coarse 48 x 27 grid that warps the image across depth edges, plus the clamp at the image's edge. The
   probable fix is per-pixel reprojection in a pixel shader (both the OpenGL and the Direct3D 9 backend), with a
   margin that grows with how fast you turn. The code: `bridge/src/bridge/composite.cpp`.
2. **Windows testing.** 0.2.0 is the first Windows build and has only been played on one PC. Try it, report what
   breaks, and especially: the launcher starting Portal 2 from a normal Steam install, Windows SmartScreen and
   antivirus on the downloaded launcher, alt-tab, the Steam overlay, and AMD and Intel graphics.
3. **Portal 2's "portal behind a wall" outline** (the blue ring with corner brackets) does not show for portals
   hidden behind Minecraft blocks.
4. **Whole-campaign play-throughs**, map by map, on Windows and Linux, with F8 bug reports where things break.
5. Anything in [the to-do list](#the-to-do-list-what-is-left-to-build) below.

**How to start:** read [CONTRIBUTING.md](CONTRIBUTING.md), pick something, and say so in
[Discussions](https://github.com/Stink-O/portalcraft/discussions) or on an [issue](https://github.com/Stink-O/portalcraft/issues)
so two people don't do the same work. Issues marked
[help wanted](https://github.com/Stink-O/portalcraft/issues?q=is%3Aissue+is%3Aopen+label%3A%22help+wanted%22) are a
good place to begin. Pull requests are welcome.

## New in 0.2.0

- **Windows.** Portalcraft now runs on Windows 10 and 11 with the normal Windows version of Portal 2: the plugin
  hooks Windows' own Direct3D 9, Minecraft links through Windows shared memory, and the launcher sets everything up
  the same way as on Linux.
- **One zip per system, ready to go.** Each zip has the launcher plus everything setup needs (the plugin, the mod,
  Fabric API and a portable Prism Launcher), so setup only downloads Minecraft and Java.
- **Minecraft keeps pace on Windows** (it used to drop to ~20 fps because Windows throttled the hidden window).
- **Minecraft's render size adjusts by itself** to keep up with Portal 2 (`pc_mc_render_scale auto`).
- **O** opens Minecraft's pause menu; the G portal gives the chamber's own portal back when you leave the
  Minecraft world; chamber sounds fade properly in the Minecraft world on Windows.

## New in 0.1.0

- **First public release.** Linux only. The launcher, the Portal 2 plugin and the Minecraft mod are all in the
  release; Minecraft itself comes from your own account through Prism, and Portal 2 from your own Steam library.

## Getting started

**You need:**

| Thing | Notes |
|---|---|
| Windows 10 or 11 (64-bit; tested on 11) **or** Linux (64-bit), with Steam | Linux: Steam's normal package (Flatpak Steam can't load the plugin yet). macOS can't run Portal 2 any more. |
| [Portal 2](https://store.steampowered.com/app/620/Portal_2/) | You must own it. On Windows the normal Windows version; on Linux the Linux version (Steam's default; don't force it to run through Proton). |
| [Minecraft: Java Edition](https://www.minecraft.net/store/minecraft-java-bedrock-edition-pc) | On a Microsoft account. You must own it. The launcher downloads version 26.3 and Java for you. |
| About 3 GB of free disk space | For Prism, Minecraft, Java and your world. |
| A PC that can run two games at once | Portal 2 and a hidden Minecraft run side by side. Exact minimum specs have not been measured. |

**1. Download the zip for your system** from the [latest release](https://github.com/Stink-O/portalcraft/releases/latest)
(`Portalcraft-<version>-Windows.zip` or `Portalcraft-<version>-Linux.zip`) and unzip the whole folder somewhere you'll
keep it. Keep the launcher and the `files` folder next to each other: `files` holds everything setup needs, so it
doesn't have to download it. Each zip has a `README.txt` with these steps.

**2. Open the launcher.**

- **Windows:** double-click **`Portalcraft.exe`**. It is not code-signed yet, so Windows SmartScreen may say "Windows
  protected your PC": click **More info**, then **Run anyway**.
- **Linux:** run **`Portalcraft`** (double-click it, or `./Portalcraft` in a terminal). If it won't start, make it
  executable: right-click it, Properties, Permissions, tick "Allow executing file as program", or `chmod +x Portalcraft`.

**3. Press "Set up Portalcraft".** It finds Portal 2 in your Steam libraries, installs Prism Launcher (an
open-source Minecraft launcher, included in the zip), and sets up a Portalcraft profile in it with Fabric and the
Portalcraft mod. The
Portal 2 plugin goes into the launcher's own folder: nothing is added to your Portal 2 install.

**4. Sign in to Minecraft.** A Prism window opens. Click the account button at the top right, "Manage Accounts",
"Add Microsoft". Prism shows a short code: open [microsoft.com/link](https://www.microsoft.com/link), type the code
and sign in with the account that owns Minecraft. Portalcraft never sees your password. If your browser asks which
app should open a `prismlauncher://` link, press Cancel and use the code.

**5. Wait for Minecraft.** Prism downloads Minecraft and Java (a few minutes), then the launcher unpacks Minecraft's
textures from your own copy. Setup is done: press **Play the campaign**.

Settings has "Add to the application menu" for a menu entry. Everything lives in `~/.local/share/portalcraft` on
Linux and `%LOCALAPPDATA%\Portalcraft` on Windows (Prism, your Minecraft world, logs); deleting that folder removes Portalcraft completely, including your Portalcraft
world.

**Updates** arrive through the launcher: when a new release is out it says "A Portalcraft update is ready". Press the
button and it downloads and checks the new files itself. Your world, saves and sign-in are kept.

## How to play

Close Portal 2 first, then open the launcher.

- **Continue** goes back to the last campaign level you played with Portalcraft, from Portal 2's last save in that level if there is one.
- **New game** starts the campaign from the first level. It can also start a fresh Minecraft world with a new random seed; the old world is kept as a backup folder (Settings can delete old ones).
- **Start as** chooses who you are when a level loads: the Minecraft player or Chell. J switches in game.
- **Pit flings demo** is a changed copy of Valve's `sp_a2_pit_flings` with a hidden Minecraft region. It is built from your own copy of Portal 2 with Portal 2's own map tools (Settings, "Build the demo map"; needs python3). The campaign doesn't need it.

The launcher starts a hidden Minecraft, starts Portal 2 through Steam with the plugin, and says **Ready** when the two are linked. The first launch takes a few minutes while Minecraft creates your world. Minecraft stays hidden on purpose and closes by itself when Portal 2 does.

Settings: a fixed world seed (for filming or sharing a world), muting Minecraft, Minecraft's memory and view distance, a different campaign start level, Offline mode, "Check and repair installed files".

## Keys

No key binds are written to your Portal 2 settings.

| Key | What it does |
|---|---|
| J | Switch between the Minecraft player and Chell |
| G | Hold G while Chell fires a portal (or tap G, then fire): that portal's partner opens onto the Minecraft world. As the Minecraft player, G also uses things (buttons, cubes) |
| H | Hold for half a second in Minecraft: go back to the last chamber spot you stood on (a held cube comes too) |
| E | Minecraft inventory (the mouse moves Minecraft's cursor, Esc closes it) |
| O | As the Minecraft player: Minecraft's pause menu (sound, controls, Open to LAN, save and quit; Esc closes it) |
| Mouse wheel, 1 to 9 | Hotbar |
| Left click, right click | Mine and attack, place and use (right click also picks up cubes) |
| F5 | Third-person view |
| F8 | Mark a bug: saves a note and a replay; "Send a bug report" in the launcher includes them |
| ` (backtick) | Portal 2's console |

## Useful console commands

Open the console with the backtick key.

| Command | What it does |
|---|---|
| `crosshair 1` | Brings back Portal 2's portal reticle if it has gone missing |
| `bridge_mode mc` / `bridge_mode chell` / `bridge_mode toggle` | Same as pressing J |
| `pc_mc_home` | Same as holding H |
| `pc_mc_portal` | Same as G: arms the next portal shot to open on Minecraft |
| `pc_reset` | Clears your portals and puts Chell back at the chamber start |
| `pc_mc_render_scale auto` / `pc_mc_render_scale 0.5`..`1` | Minecraft's render size: automatic (default: lowered when Minecraft cannot keep up with Portal 2, raised when it can) or fixed |
| `pc_mc_target_fps 90` | The frame rate the automatic render size aims for from Minecraft |
| `pcb_dig_reset` | Forgets every dug hole in the current chamber (Minecraft's side too) |
| `pc_dig 0` / `pc_dig 1` | Turn wall digging off or on (it is on by default) |
| `pc_mc_hud 0` / `pc_mc_hud 1` | Hide or show the Minecraft hand and HUD |
| `pc_start_mode mc` / `pc_start_mode chell` | Choose who you are when a level loads |

The full list is in `bridge/README.md`.

## Reporting a bug

Press F8 in game at the spot where it happened, then press **Send a bug report** in the launcher. It saves `Portalcraft-bugreport-<date>.zip` in your home folder (logs, your F8 marks and system info; no passwords or sign-in tokens). Attach it to a [new issue](https://github.com/Stink-O/portalcraft/issues) and say what you did and what you expected.

## If something goes wrong

| What you see | What it usually means |
| --- | --- |
| "Portal 2 not found" | Portal 2 is in a Steam library the launcher didn't find. Set its folder in Settings (Steam: right-click Portal 2, Manage, Browse local files). |
| "Steam runs Portal 2 through Proton" | Steam is set to use Proton for Portal 2. Right-click Portal 2, Properties, Compatibility, untick the Proton box. |
| "Sign-in not finished" | The Prism sign-in was closed early. Press RETRY, then "Open Prism to sign in". |
| The browser asks which app should open `prismlauncher://` | Cancel it and use the code Prism shows, at microsoft.com/link. |
| Portal 2 doesn't start, or Play waits for ever | A Steam pop-up is in the way. Click it away and press RETRY. |
| "The plugin did not load" | Usually Flatpak Steam, which can't see the launcher's folder. Use the normal Steam package. |
| "/dev/shm is full" or stutter after hours of play | Steam leaks shared memory over long sessions. Restart Steam. |
| "Portal 2 is already running" | Close the Portal 2 you started yourself; Portalcraft never closes a game it did not start. |
| Something else | Press F8 in game, then "Send a bug report" in the launcher, and [open an issue](https://github.com/Stink-O/portalcraft/issues) with the zip. |

## Known issues

- **Visual artifacts in the Minecraft world, on every build** (Windows and Linux): smearing and swirling when going
  through a portal into the Minecraft world and when looking around in it. See [Help wanted](#help-wanted).
- **Windows (new in 0.2.0):** played on one PC only. The launcher's normal way of starting Portal 2 (through Steam)
  has not been tested on Windows yet; testing used a separate copy of Portal 2. SmartScreen and antivirus warnings
  on the unsigned launcher are expected. The Pit Flings demo map can't be built on Windows yet (it needs Portal 2's
  Linux map tools).
- Portals hidden behind Minecraft blocks don't get Portal 2's "portal behind a wall" outline.
- Steam sometimes shows a pop-up that stops the game from starting. Click it away and press RETRY in the launcher.
- Steam can leak shared memory over a long session. If the launcher says `/dev/shm` is full, or things stutter, restart Steam.
- Pulling a cube or other prop through a hole you dug in a wall (a glass wall, for example) does not work yet. Only the picture and Minecraft's side are cut; Portal 2's physics still see the old wall.
- Digging rough edges: pipes, signs and doors inside a cut are not cut yet. A thin white seam can show at one floor edge on `sp_a1_intro2`. Portal views are not cut. Explosions do not dig walls.
- Falling and other Portal 2 hazards hurt the Minecraft player. This is intended.
- Not every campaign map has been played all the way through as the Minecraft player. Cutscenes, the last boss chapters and the late gel and funnel puzzles are the least tested.
- Developed on one Linux machine with an NVIDIA GPU. Other setups may hit problems: please report them.

## The to-do list (what is left to build)

This is the author's list, in plain words. If you want to help, these are good places to start.

**Next up**

1. **Pull objects through dug walls.** If you mine a hole in a wall (say a glass wall) and try to grab a cube or pull it through, it does not work, because Portal 2's physics and its pick-up still see the original wall. The plan is to let physics objects ignore the original wall where a hole has been dug, using the same trick that already lets Chell's shadow follow holes. It must not let objects fall through floors next to a hole, and lasers, buttons and portals must keep working around it.
2. **Dig leftovers.** Pipes and signs (static props) and moving doors (brush entities) inside a hole are not cut yet. One thin seam strip on `sp_a1_intro2` at a floor edge. A white strip on the laser-over-goo room. Light inside holes has only been checked on `sp_a1_intro2`.
3. **Unified-world checks that have not been run.** A long walk with the world's moving origin (only teleport steps were tested). The fallback that builds a stone backing when the exit spot has no natural cliff.
4. **A full campaign walk-out check.** Fire a G portal in a campaign chamber, walk out into the Minecraft world, and come back, on several maps.
5. **Exit-spot quality.** The automatic exit finder was tried on three seeds. It needs checking on many more, and its scoring tuned.

**From the campaign risk list (done, but not yet seen working everywhere)**

- Portals that a map places itself (the first levels' ceiling and floor portals): checked on some maps, not all.
- Faith plates, pushes and gels in the later chapters: working in the test map, not re-checked across the whole campaign.
- Excursion funnels through portals: the first segment works, passing through a portal is untested.
- The portal doors in the final chapters (`sp_a4_finale2`), Wheatley in `sp_a1_wakeup` and `sp_a2_bts4`, and standing in a laser beam: not re-run.
- The last credits map never finishes loading some moving shapes (harmless).
- Housekeeping: memory-leak checks, crouching under low gaps.

**Older items that may still be open**

- Moving platforms: a few steep tilted panels still make the player slide off; rotating platforms only carry you in a straight line.
- Third-person cape lag after short hops.
- If something is built in the cliff opening where the G portal comes out, the portal cannot open (build elsewhere).

**Platforms**

- Windows: works since 0.2.0 but has been tested on one PC only. Needs testing on other PCs, with Portal 2 started
  through a normal Steam install, and on AMD and Intel graphics. Code signing for the launcher.

## Building from source (for developers)

You only need this to change Portalcraft. Players use the launcher above.

| Tool | Notes |
|---|---|
| g++ with 32-bit support | Portal 2 on Linux is a 32-bit program, so the plugin is built with `g++ -m32`. Fedora: `sudo dnf install gcc-c++ glibc-devel.i686 libstdc++-devel.i686`. Debian and Ubuntu: `sudo apt install g++-multilib`. |
| Java 25 (a JDK) | For the Fabric mod. Gradle is downloaded by `bridge/fabric/gradlew`. |
| Rust (stable) | For the launcher only. |
| make, python3, git | Build and helper tools. |

```sh
git clone https://github.com/Stink-O/portalcraft.git
cd portalcraft
bridge/tools/portal2_dir.sh                # prints your Portal 2 folder ($PORTAL2_DIR overrides it)
make -C bridge                             # the plugin -> bridge/build/dev/portalcraft_bridge.so
(cd bridge/fabric && ./gradlew build)      # the mod -> bridge/fabric/build/libs/portalcraft-<version>.jar
python3 bridge/tools/extract_mc_assets.py  # Minecraft's textures from your own 26.3 client jar, into cache/
launcher/build.sh                          # the launcher -> target-launcher/release/portalcraft-launcher
```

**On Windows**, build the plugin with [MSYS2](https://www.msys2.org)'s mingw32 shell (`pacman -S mingw-w64-i686-gcc make`,
then `make -C bridge -f Makefile.win CXX=g++`), or cross-build it from Linux with `bridge/tools/build_windows_plugin.sh`
(podman). The mod and the launcher build the same way as on Linux (`gradlew.bat build`; `cargo build --release` in
`launcher/`). `bridge/tools/run_game_win.ps1` is the Windows test harness.

The repository root is found through the `.portalcraft-root` file; keep it where it is. For development there is also a script launcher that runs the mod straight from Gradle (`bridge/tools/play_clip.sh --mc`, `--help` for every option) and a test harness (`bridge/TESTING.md`). Optional demo map: `maps/tools/build.sh --install` (reads Valve's map from your install and uses Portal 2's own Linux compilers; no Valve files are copied into the repository). Making a release: `launcher/RELEASE.md`.

## Project layout

| Folder | What is in it |
|---|---|
| `bridge/` | The Portal 2 plugin (`bridge/src`), the Fabric mod (`bridge/fabric`), the shared-memory protocol, developer launch scripts and tests (`bridge/tools`), design notes. Start with `bridge/README.md` |
| `launcher/` | The player launcher (Rust, egui): setup, Prism profile, updates, Continue, bug reports |
| `maps/` | Scripts that build the custom Portal 2 maps from your own copy of the game |
| `.portalcraft-root` | Marker file: the plugin and scripts use it to find this folder. Do not delete it |

## Credits and licences

Portalcraft's own code is under the MIT License (`LICENSE`). It stands on other people's work; full notices are in `NOTICE` and `bridge/NOTICE`.

- **SkyCraft** by chasmlol (MIT): the idea, the architecture and large parts of the Fabric mod, including digging. https://github.com/chasmlol/SkyCraft. Licence: `bridge/fabric/LICENSE-SkyCraft`.
- **SourceAutoRecord** by the SAR authors (MIT): the plugin framework that hooks into Portal 2. https://github.com/p2sr/SourceAutoRecord. Licence: `bridge/src/sar/LICENSE-SourceAutoRecord`. It includes LodePNG by Lode Vandevenne (zlib licence).
- **Create** by The Creators of Create (MIT): the way moving platforms carry the player is adapted from its contraption collision. https://github.com/Creators-of-Create/Create. Licence: `bridge/fabric/LICENSE-Create`. No Create art or sounds are used.
- **Immersive Portals** by qouteall (Apache-2.0): the approach to walking through portals. No code was copied.
- **Prism Launcher** (GPL-3.0): the launcher downloads it from its own GitHub release onto your computer; it is not included here.
- **Fabric**, the Fabric API (Apache-2.0) and Gradle: downloaded when you set up or build, not included.
- **Barlow** by The Barlow Project Authors (SIL Open Font License): the launcher's typeface.

Portal 2 and Portal are trademarks and works of Valve Corporation. Minecraft is a trademark and work of Mojang Studios and Microsoft. Portalcraft is a fan project, is not made by or affiliated with Valve, Mojang or Microsoft, and contains none of their game files, assets or code. You must own both games. Minecraft and its textures come from your own copy, downloaded through Prism with your own account, and Portal 2's maps are read from your own install.

> [!IMPORTANT]
> **Just want to play?** Download the Windows or Linux zip from the
> [latest release](https://github.com/Stink-O/portalcraft/releases/latest), unzip it, open **Portalcraft.exe**
> (Windows) or **Portalcraft** (Linux) and press **Set up Portalcraft**. Nothing else to install by hand.
