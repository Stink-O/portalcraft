# Writing accurate bridge tests (bridge #24)

Tests used to guess coordinates and facing directions, and bugs the user saw could not be located. Use these tools instead.

## 1. Name things; don't guess coordinates

Every level load reads the BSP's entity lump and writes `cache/bridge/mapindex/<map>.txt`. It lists every named entity with
its classname, origin, angles, the world box of brush entities, and the floor under it. Once the level is live, a grid of floor
heights across the map follows (world brushes only, 64 u columns, every floor with at least 72 u of headroom).

| command | what it does |
|---|---|
| `pcb_find <substring>` | lists index entries whose name or class contains it, with origin, box and floor; marks entities that moved or are gone |
| `pcb_goto <target> [dist=64] [side]` | noclip setpos to a spot `dist` u from the target's box, facing it. A hull trace checks that the spot is clear and a floor trace checks that it is standable. With no side, it picks the nearest spot to the player that has a line of sight. `front/back/left/right` go along the target's angles; `low/high` pick the lowest or highest floor around the target. In Minecraft mode it switches to Chell, places her, and switches back (follow it with `waituntil mcleads`) |
| `pcb_face <target>` | exact pitch and yaw from the eye to the target's centre. Minecraft's look follows in both modes |
| `pcb_assert_facing <target> [tol=2]` | asserts that the view points at the target |
| `pcb_assert_past <target> <+x\|-x\|+y\|-y\|+z\|-z> [margin]` | asserts that the player's origin is beyond the target's box on that axis: `+x` means past its max x, `-x` means before its min x |
| `waituntil near <target> <dist> [ticks] [abort]` | waits until the player's standing hull is within `dist` u of the target |
| `pcb_mapindex [write\|reload]` | prints the index status, or writes or reloads it |

A target is a targetname or `x y z`; in `waituntil near`, write a point as `x,y,z`. When the entity exists live, its current
origin is used, so a door that has opened is aimed at where it stands now. These commands need `sv_cheats 1`.

```
pcb_goto exit_ledge_player_clip 80 low
wait 60
pcb_face exit_ledge_player_clip
pcb_assert_facing exit_ledge_player_clip 2
pcb_glhud_shot my_ledge
```

## 2. See what Minecraft collides with

`pc_show_collision 1` draws the voxels the export actually sent to Minecraft within about 8 blocks, as merged, translucent
wireframe boxes. They are depth-tested. The data comes from collfield.cpp's record of what was sent, with patches merged; nothing
is re-queried.

`pc_show_collision 2` colours each voxel by what Portal 2 has there now:

| colour | meaning |
|---|---|
| cyan | a world brush |
| green | a static prop |
| own hue, name drawn above | a movable entity |
| magenta | playerclip |
| **red** | **nothing in Portal 2 is there**: stale voxels, or invisible blocks |
| white | not classified yet |
| grey outline | a region that has not been sent yet (a gap) |
| yellow outline | a region whose patch is still pending |
| blue | the movers' boxes where they are now (bridge #26: what Minecraft collides with for doors, lifts, panels, cubes; never in the static voxels) |

Any of these three in a screenshot points at the bug:
- red boxes;
- a floor slab at the wrong height;
- a missing slab.

`pcb_collision_shot <name> [mode=2]` is for tests. It turns the overlay on, waits for classification (up to 4 s), and saves
`cache/bridge/glhud_<name>.png` plus `glhud_<name>.txt`. The text file has the legend counts, the movable entity names with their
colours, and every box with its class. The previous mode is restored afterwards. `pcb_collision_probe x y z` prints the
classifier's answer for one point.

Arguments: `pc_show_collision 2 [radius_blocks] [vertical_blocks]`.

## 3. Turn a user's bug into a test

The user presses **F8** (`pc_bugmark_key <scancode>`; no bind is written). It works in either mode. This writes
`cache/bridge/bugmarks/<timestamp>/`:

| file | what it holds |
|---|---|
| `pose.txt` | the map; the mode; Portal 2's feet, eye, view and velocity; Minecraft's feet, look, eye and flags; both crosshair targets (the Portal 2 trace, and Minecraft's outlined block and face); switch, carry and takeover state; the nearest named entity |
| `screen.png` | the frame as presented, with the Minecraft overlay |
| `colfield.txt` | for the 5x5x4 blocks around the feet: what Minecraft holds next to a fresh voxelisation, flagged `DIFFERS`, plus each region's sent or pending state |
| `dyn.txt` | the tracked moving solids within 6 blocks, with probe results, flagged `MOVED SINCE` when their export is stale |
| `ring.txt` | the last 12 s. `F` lines hold per-frame pose, frame time and flags. `I` lines hold inputs: `mc` lines are what Minecraft received; `sdl` lines are raw SDL events Portal 2 saw |
| `replay.txt` | a ready `pcb_script` cfg; see below |

A one-line `BUG MARKED -> <dir>` confirmation goes to the console. A 2.5 s toast is drawn after the screenshot and pcb_record
readbacks, so neither captures it; an external screen recorder does. `cache/bridge/bugmarks/index.txt` lists every mark.

`replay.txt` runs as:

```
PCB_SO=/abs/path.so tools/locked_run.sh terrain|mirror <bundle>/replay.txt <map> 400 <run>
```

The exact line is in its header. It has two parts:

1. **The lead-up.** It starts at the pose 12 s before the mark.
   - In Minecraft mode, it replays what Minecraft received: `pcb_mckey` keys and buttons, `pcb_sdl_event` wheel and cursor
     moves, and the view as `pcb_setview`.
   - In Chell mode, it replays a pose track (`setpos` and `pcb_setview`), because Portal 2's own keys cannot be replayed through
     these commands; they appear as comments.
   - `pcb_pose_diff leadup` logs how close this part ended to the mark. This is for information only, since physics and timing
     differ.
2. **The moment.** It puts the player at the marked pose in the same mode (`setpos`, `setang` and `bridge_mode`), then runs
   `pcb_assert_pose <feet> 1 <view> 1`.

To make a regression test from a mark:
1. Copy `replay.txt` to `tools/cfg/<name>.txt`.
2. Usually, delete part 1.
3. After part 2, add the check that fails today. Examples:
   - `pcb_assert_body_clear`
   - `pcb_collision_shot` followed by reading the `.txt` for `NOTHING` boxes
   - `pcb_assert_mc_flag ground 1`
   - `pcb_assert_body_z`
4. Replace raw coordinates with `pcb_goto <name>` where an entity is nearby. `pose.txt` gives the nearest named entity.

Tests can press the key through the real polling path with `pcb_bugmark press [label]`. `pcb_bugmark now [label]` marks at
once, and `pcb_bugmark last` prints the newest bundle.

## 3b. Movers (bridge #26)

Every solid that can move is a mover (doors, lifts, trains, arm panels, buttons, cubes...): its own shape in Minecraft, posed every frame.

| command | what it does |
|---|---|
| `pcb_movers [near <r>\|all\|write]` | lists movers (id, class, name, pose, shape, bound, posed); `write` saves `cache/bridge/movers_<map>.txt` |
| `pcb_stand_on <name\|#id\|@x,y,z>` | puts the player on the top under the entity's box middle; as the Minecraft player, Minecraft is sent there too |
| `pcb_assert_mc_platform [1\|0]` | Minecraft stands on a mover now |
| `pcb_assert_mc_rode <min ticks> [max catches]` | ticks carried by a moving mover since `reset`, and fall guard catches |
| `pcb_assert_movers_ready [r]` | every mover within r of the player has its collision in Minecraft |
| `pc_movers [0\|1] [build ms] [urgent ms] [radius]` | switch / budgets |

A sweep over every mover of a map, from the BSP (classes, parents, the map's own inputs that move each one):

```
tools/movers_sweep.py <map> > tools/cfg/<map>_movers26.txt
PCB_SO=/abs/path.so tools/locked_run.sh mirror tools/cfg/<map>_movers26.txt <map> 1500 <run>
tools/movers_table.py ../cache/bridge/shots/<run>/bridge.log
```

## 4. Timing and launching

- `pcb_run_summary`'s `RUN PERF` line says `TIMING NOT VALID: the window was not focused for N of M frames` whenever SDL's window
  flags showed no input focus. It does this as well as the old mean-frame-time heuristic. Use per-feature CPU ms in unfocused runs.
- `tools/run_game.sh` and `play_clip.sh` watch Steam's `console_log.txt` for 20 s after a launch. If Steam waits on its
  `ShowInterstitials` pop-up, they stop at once with a clear message: run_game exits 3 and play_clip exits 6. Nothing is left
  launched, and locked_run stops its own Minecraft. The check is `tools/steam_check.sh`.
- Building a second copy at the same time (for example a test build while you play): `make OBJDIR=build/obj-<you> DEVSO=build/dev-mcportal/<you>-x.so`. This keeps the object
  files apart.

## 5. The testbed map (bridge #29)

`pc_testbed` is a compact chamber with one bay per mechanic, so tests reach a state with one input instead of solving a puzzle
(maps/README.md has the bay table; `cache/bridge/mapindex/pc_testbed.txt` lists every entity). A central hall (x/y +-1120,
384 high) has bays on its four sides. Every testable thing is named `tb_*`; every bay has an `info_target tb_spot_<bay>` in the
hall in front of it, and two relays: `tb_go_<bay>` (reach the mechanic's active state) and `tb_reset_<bay>` (back).

```
ent_fire tb_go_lift trigger                    # the lift rises 192 u
pcb_goto tb_spot_laser 64 ; pcb_face tb_laser_catcher
pcb_stand_on tb_turntable                      # as Chell or the Minecraft player
```

The layout is generated: `maps/tools/gen_testbed.py` writes the VMF and `maps/build/pc_testbed_layout.json` (copied to
`tools/cfg/pc_testbed_layout.json`: bay bounds, standing spots and facing, every tb_ entity with origin, size, top surface,
portalable faces and normals, each relay with what it fires and the expected result). **Test cfgs name entities, never
coordinates**; `tools/gen_tb_smoke.py` writes them from the JSON, so a layout change regenerates the tests:

```
maps/tools/build_testbed.sh                    # generate + vbsp/vvis/vrad + alias script -> maps/build/pc_testbed.bsp
cp maps/build/pc_testbed.bsp "<Portal 2>/portal2/maps/"   # (build_testbed.sh --install does it when no Portal 2 runs)
tools/gen_tb_smoke.py                          # -> tools/cfg/tb_smoke.txt (Chell phase, then the Minecraft player) + tb_startmc.txt
tools/gen_tb_smoke.py --chell                  # -> tb_look.txt (Chell only, shots before/after each relay)
tools/gen_tb_smoke.py --probe                  # -> tb_probe.txt (lighting probes + one shot per bay)
PCB_SO=<abs .so> tools/locked_run.sh mirror tools/cfg/tb_smoke.txt pc_testbed 1800 tb_smoke
PCB_PRE="pc_start_mode mc" PCB_SO=<abs .so> tools/locked_run.sh mirror tools/cfg/tb_startmc.txt pc_testbed 400 tb_startmc
tools/tb_check.py                              # every layout entity is in cache/bridge/mapindex/pc_testbed.txt
tools/movers_sweep.py pc_testbed > tools/cfg/pc_testbed_movers26.txt   # the movers sweep works on it unchanged
```

The map starts in the arrival elevator car (Valve's `elevator_arrival_logic` instance, named `arrival_elevator`): the car rises
and opens by itself, as in pit_flings. `departure_elevator` is Valve's departure logic with the 4500 u shaft above bay `departure`.
Valve's `global_ents` instance supplies `@command`, `@tonemapper` and the environment relays; `environment_testchamber` is fired
at start, like pit_flings. The Minecraft region is the same box as pit_flings's (`pc_mc_region`, `pc_mc_region_bounds`).
