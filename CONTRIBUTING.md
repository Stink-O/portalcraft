# Contributing to Portalcraft

Thanks for looking. Portalcraft was built by one person, who can't keep working on it alone. It works, but it is
rough. Help of any size is welcome: a bug report, a test on your PC, a fix, or taking over a whole feature.

## Where to talk

- **[Discussions](https://github.com/Stink-O/portalcraft/discussions):** questions, ideas, and "I'd like to work on X".
- **[Issues](https://github.com/Stink-O/portalcraft/issues):** bugs and concrete tasks. Ones marked **help wanted** are
  ready to pick up. Say on the issue that you're taking it, so two people don't do the same work.
- **Pull requests:** welcome, including small ones and drafts. A PR that fixes part of a problem is still useful.

## What needs doing most

See [Help wanted in the README](README.md#help-wanted). The biggest open problem is the visual artifacts in the
Minecraft world, which appear on every build. Briefly:

- **What happens:** Portal 2 shows Minecraft's picture by reprojecting Minecraft's latest frame to Portal 2's camera.
  This is `bridge/src/bridge/composite.cpp`: `ImageUVCore`, the warp grid and `RenderScaleUpdate`.
- **What's already known:**
  - Minecraft's own frame is clean.
  - The damage appears in the reshaping, at depth edges and near geometry. It shows as swirling leaves, a smeared
    player in third person, and stretched bark at the screen edge.
  - It isn't the frame rate (Minecraft gives about 90-120 fps), the occlusion pass, or the render scale.
  - Images arrive about 25 ms old.
- **Most likely cause:** a 48 x 27 grid interpolates texture coordinates across depth edges. Its cells are about 53 px
  wide at 2560 px, so the mapping folds at silhouettes. The clamp at the image edge also stretches the margin.
- **Suggested next steps:**
  1. Reproduce it offline: a saved image, its depth alpha and two poses, run through `ImageUVCore` and the grid.
  2. Move to per-pixel reprojection in a pixel shader, in both render backends (OpenGL on Linux, Direct3D 9 on Windows).
  3. Make the image margin grow with the turn rate.
- **Useful commands:** `pcb_mcshot` and `pcb_d3d9_shot <name>` save Minecraft's own frame and the composited frame for
  comparison. `pcb_wobble` measures how well points line up.

## How it fits together

- **The Portal 2 plugin** (`bridge/src`, C++, 32-bit). Built on a trimmed fork of
  [SourceAutoRecord](https://github.com/p2sr/SourceAutoRecord). It loads into the real Portal 2, draws Minecraft's
  blocks, hand, HUD and world inside Portal 2's frame, cuts dug holes out of Portal 2's walls, and runs the Minecraft
  player inside Portal 2's physics and triggers.
  - Linux Portal 2 draws through OpenGL (togl). Windows Portal 2 draws through Direct3D 9.
  - The few parts that touch the graphics API are behind `render_backend.hpp`: `render_backend/` for OpenGL,
    `d3d9/` for Direct3D 9.
  - Operating-system calls are behind `platform/`.
  - Engine addresses for both systems are in one table: `slots.hpp`.
- **The Fabric mod** (`bridge/fabric`, Java, Minecraft 26.3; derived from
  [SkyCraft](https://github.com/chasmlol/SkyCraft)). It runs a hidden Minecraft that supplies the world, the player and
  the blocks.
- **The link:** one shared-memory block. Its layout is `bridge/protocol/skycraft_protocol.h`. It is called
  `/dev/shm/Portalcraft_v1` on Linux and `Local\Portalcraft_v1` on Windows.
- **The launcher** (`launcher/`, Rust and egui). It finds Portal 2, sets up Minecraft through a portable Prism
  Launcher, keeps everything up to date, and starts both games.

**Read next:**
- `bridge/README.md`: the plugin's features and every console command.
- `bridge/TESTING.md`: the test harness and test scripts.
- `bridge/DESIGN-*.md`: how the composite, digging, the Minecraft player, portals and the unified world work.
- `bridge/WINDOWS-VERIFY.md`: Windows diagnostics.
- `launcher/RELEASE.md`: how releases are made.

## Building and testing

The README has [the build steps](README.md#building-from-source-for-developers) for Linux and Windows.

Test runs are driven by small console scripts in `bridge/tools/cfg/`. They are loaded with
`pcb_script "<file>" <watchdog seconds>`; `waituntil`, `wait`, `screenshot` and `pcb_*` checks do the work.
- **Linux harness:** `bridge/tools/run_game.sh` (and `locked_run.sh`, which also starts the dev Minecraft).
- **Windows harness:** `bridge/tools/run_game_win.ps1`.

Each harness writes a temporary `autoexec.cfg`, starts the game, waits for the script to finish, collects `bridge.log`
and the screenshots, and cleans up.

## Ground rules

- **It must look and play like Portal 2.** Minecraft parts appear inside Portal 2's own rendering and rules.
- **Fix things generally, for the whole campaign,** not with special cases for one map.
- **No game files and no decompiled code in the repository.** Nothing from Valve or Mojang: no maps, textures,
  models, sounds or code. Everything is read from the player's own installs at run time. Engine signatures and
  offsets that come from SourceAutoRecord (MIT) are fine. Describe engine behaviour in plain words.
- **Keep both systems building.**
  - Windows-only code goes behind `#ifdef _WIN32` or the platform layer.
  - Graphics code goes behind the render backend.
  - If you can only test one system, say so in the PR.
- **Show evidence.** For anything visual, attach a screenshot or the relevant `bridge.log` lines to the PR.
- **Use plain English** in user-facing text (launcher, README): Portalcraft is meant for players who aren't technical.

## Licence

Portalcraft is MIT licensed (`LICENSE`). By contributing you agree that your contribution is released under the same
licence. Third-party notices are in `NOTICE` and `bridge/NOTICE`.
