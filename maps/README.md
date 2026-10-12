# maps: pc_pit_flings (sp_a2_pit_flings + hidden Minecraft region)

`pc_pit_flings.bsp` is Valve's sp_a2_pit_flings rebuilt from its own BSP.
The chamber and its puzzle logic are unchanged. The only addition is a large
sealed hollow box below the chamber for the bridge plugin's Minecraft terrain.
The box is reachable only through portals.

## Region (for the bridge agent)

| what | value |
|---|---|
| interior min (x y z, Source units) | `-16128 -16128 -16128` |
| interior max | `16128 16128 -5632` |
| size | 32256 × 32256 × 10496 u = **806 × 806 × 262 blocks** at 40 u |
| walls | 128 u thick, outer faces at ±16256 / -16256 / -5504 |
| gap to the chamber | the chamber's lowest point is z -4592 (elevator shafts) |
| `pc_mc_region` | info_target at the centre, `0 0 -10880` |
| `pc_mc_region_bounds` | **two** info_targets with this same name: one at the min corner, one at the max corner. Take the componentwise min and max of their origins. |
| `pc_map_alias` | logic_script (see "Map name" below); not region-related |

- Wall material: `TOOLS/TOOLSSKYBOX` on all six walls. The walls render as
  the map's 2D sky (`sky_white`, plain white) and never as a textured wall.
  They are not portalable and have no lightmaps. The floor is solid, so
  Chell lands on an invisible sky-coloured floor at z -16128 if nothing is
  drawn there.
- Lighting: sky walls let the map's own light_environment light the box.
  vrad bakes its sky ambient into the region's leaf ambient cubes: a uniform,
  bluish ~0.33 / 0.47 / 0.66 (r g b) from every direction, measured with
  `pcb_probe`. The sun is added dynamically. The plugin's ×5 block-lighting
  calibration was made for the chamber's ~0.03–0.05 and saturates here, so
  it needs recalibrating. Black walls with point lights were tried and
  rejected: the engine applies only a few local lights per model, so block
  faces came out black (`maps_pc2` run).
- Verified in game (`cache/bridge/shots/maps_pc/`, from the first build, which
  had the same region): the region is not black-culled. `outsideWorld=0` and
  `contents=0` everywhere inside. `pcb_block_here` blocks on the region floor
  render and are lit (`pcb_region_floor_block.png`). A portal on the pit
  floor, `(200 -150 -767)` facing up, paired with one at `(20 20 -9000)`
  facing down, shows the region's blocks from above
  (`pcb_region_through_pit_portal.png`). That is the clip's shot.

## Map name

The file name differs from Valve's, and Valve's VScripts read
`GetMapName()`. That name picks GLaDOS's lines, the elevator videos and
music, the portal gun upgrade, and the next map at the exit. A missing name
would have given Chell the potato gun and broken the level end. The
pakfile carries `scripts/vscripts/portalcraft/pc_map_alias.nut` (source:
`tools/pc_map_alias.nut`). The first entity after worldspawn, the
logic_script `pc_map_alias`, runs it before any of Valve's scripts. It makes
`GetMapName()` return `sp_a2_pit_flings` in this map. This was verified in
game: `[maps] GetMapName=sp_a2_pit_flings` in `maps_pc/bridge.log`. The exit
therefore changelevels to sp_a2_fizzler_intro, as in Valve's map. Loading with
`map pc_pit_flings` starts in the arrival elevator, as `map sp_a2_pit_flings`
does.

## Method

Everything is in `tools/`. It is plain Python plus the game's own Linux compile tools.

1. `bsp2vmf.py`: our own BSP v21 → VMF decompiler (layouts from the public
   SDK bspfile.h, as in crates/source_assets). It reconstructs:
   - brushes exactly: plane intersections, bevels dropped, CONTENTS_DETAIL
     brushes → func_detail, stray areaportal brushes → their
     func_areaportalwindow;
   - brush entities from model space. vbsp only translates by the origin,
     and the engine applies `angles`, so no rotation is baked in;
   - texture axes, shift, scale and lightmapscale from texinfo, with cubemap
     and WVT patched materials unpatched via the pakfile;
   - tool materials that vbsp's texinfo compaction had remapped, recovered
     from brush contents (clip, blocklight, nodraw);
   - displacements (vbsp drops their brushes; a thin brush is rebuilt under
     each base face);
   - overlays: Valve's info_overlay_accessor keyvalues, side ids remapped via
     the face-id lump;
   - static props → prop_static, and cubemaps → env_cubemap with explicit
     `sides` taken from the patched materials, so the assignment is Valve's;
   - all other entities verbatim, with I/O kept as connections.
   Where two brushes have overlapping coplanar sides, the hidden side gets
   the visible side's surface. Otherwise the region's different BSP splits
   flip which one shows (one 16×64 u strip at x 256).
2. `add_region.py`: appends the six wall brushes after Valve's world brushes,
   so Valve's brush order is unchanged. It adds the marker info_targets and
   puts the alias logic_script first.
3. `compile.sh`: vbsp → vvis (full) → vrad `-hdr -final -StaticPropLighting
   -StaticPropPolys -textureshadows` (HDR only, like Valve's). This takes
   about 50 s in total. Two workarounds for the 2026 Linux tools:
   - they refuse to write outside their `-game` dir, so `build/game/` is a
     small game whose gameinfo.txt mounts Portal 2 read-only;
   - they need `PORTAL2_NO_LAUNCHERMGR_REQUIRED_IN_MATERIAL_SYSTEM=1`,
     otherwise they fail with "Could not start the empty shader".
   vvis and vrad lower-case their input path, so paths go through the
   all-lower-case alias `/proc/self/cwd`.
4. `relight.py`: carries Valve's baked lighting over wherever the geometry
   matches. Our vrad differs from Valve's 2011 compile (different vrad build,
   random ambient sampling), and some rays from the chamber escape through
   nodraw faces and reach the region's sky.
   - HDR lightmaps per luxel: lightmap grids are world-aligned. 99.4% of
     luxels carried; the rest keep ours.
   - Leaf ambient: whole leaves where the bounds match, otherwise the nearest
     Valve sample per sample. Region samples stay ours.
5. `pack.py`: rewrites the pakfile. It keeps vbsp's patched materials and
   carries over Valve's built cubemaps (buildcubemaps needs the running
   game), Valve's static-prop vertex lighting (`sp_hdr_*.vhv`; props are in
   the same order) and Valve's simpleworldmodel, all renamed to the new map
   path. It also adds the alias script.

Rebuild (about 1 min, needs no GPU):

```sh
maps/tools/build.sh            # -> maps/build/pc_pit_flings.bsp
maps/tools/build.sh --install  # also copies it into the game (refuses while Portal 2 runs)
```

**Installed file:** `…/Portal 2/portal2/maps/pc_pit_flings.bsp` is the only
file this agent added to the game folder. Delete it to uninstall. Valve's
files were never modified.

Checks, all offline: `tools/cmp_bsp.py orig new` (areas, models, brush
contents, entities), `tools/cmp_faces.py` (material, texture alignment and
lightmap scale per face), `tools/cmp_brushes.py`, `tools/ambient_cmp.py`.

## Verification status

- **Structural (final build):** identical brush contents counts (1630 brushes)
  and model bounds for all 155 models. Areas and areaportals match (plus the
  region's area). Entities match Valve's except the 4 added ones. All 26
  overlays have the same face counts. 3092 of 3127 faces have identical
  material, texture alignment and lightmap scale. The other 33 are split
  differently; none has a different material or alignment. There are no
  leaks, and no errors in the vbsp, vvis or vrad logs.
- **In game, first build** (before relight.py, the coplanar fix and the
  alias ordering were final; `cache/bridge/shots/maps_orig` vs `maps_pc`,
  `side_*.png` / `diff_*.png`): poses a, b, c, d, f are identical to the
  eye (mean pixel diff 0.1–1.7, mostly dynamic effects). Pose e (inside the
  departure elevator) was bluer and brighter in ours. That was the sky leak
  into the baked ambient, which relight.py now replaces with Valve's data
  (the leaf ambient at pose e is byte-identical to Valve's).
- **PENDING:** in-game side-by-side of the *final* build. Two attempts to
  launch the game failed: the GPU faulted under the headless gamescope
  launch option, and the lead then stopped all game runs.

## Next steps

1. When the GPU is back and the lead allows runs, repeat both captures with
   the bridge runner:
   `bridge/tools/run_game.sh maps/tools/cfg/orig.txt sp_a2_pit_flings 150 maps_orig`
   and `… maps/tools/cfg/pc.txt pc_pit_flings 200 maps_pc4`. Then diff the
   `pcb_pose_*` shots (the snippet that makes `side_*.png` / `diff_*.png` is
   in this agent's history; any per-pixel abs-diff will do). Check pose e
   especially, and the region shots.
2. Optional: a play-through of the puzzle and the exit changelevel. The
   entity lump is Valve's plus the markers, so this is expected to work.

## Known differences from Valve's map

- 33 faces are split slightly differently. They look the same, and their
  lightmaps are copied per luxel. 0.6% of luxels (edges of re-split faces)
  and chamber leaf-ambient samples that fall in Valve's solid use our vrad.
- One sliver face (x 256, z 382–383) takes the nearest cubemap, because
  Valve's compile had no face there.
- The static-prop "screen space fade" flag on 15 props is dropped. The
  current vbsp calls it obsolete, and those props have no fade distances.
- Brush order inside the world differs from Valve's: Valve's interleaves
  detail brushes, ours groups them. This only affects BSP splits.
- Two named lights gain a `defaultstyle` key, which the current vbsp adds.

# pc_testbed: one bay per mechanic (bridge #29)

A compact test chamber for the bridge's tests: a hall (interior x/y +-1120, z 0..384, black non-portalable metal) with 25 bays
(256 u wide inside, white portalable tile unless noted; the bright/dark/portals/mc bays differ) on its four sides. Each bay is one mechanic that a single input puts in
its active state, so no test has to solve a puzzle or walk far. The Minecraft region is the same hollow box as pc_pit_flings's
(`pc_mc_region`, two `pc_mc_region_bounds`), built by the same code (`add_region.region_solids/region_markers`).

**The generator is the single source of truth.** `tools/gen_testbed.py` writes the VMF and `build/pc_testbed_layout.json`
(copied to `bridge/tools/cfg/pc_testbed_layout.json`): per bay the bounds, standing spot and facing, each tb_ entity with origin,
size, top surface, portalable faces and normal, the relays, what each fires and the expected result. The bridge test cfgs are
generated from that JSON (`bridge/tools/gen_tb_smoke.py`) and never hold coordinates.

```sh
maps/tools/build_testbed.sh            # gen_testbed.py + vbsp/vvis/vrad + alias script -> maps/build/pc_testbed.bsp (about 50 s)
maps/tools/build_testbed.sh --install  # also copies it into the game (refuses while Portal 2 runs)
```

Needs `maps/tools/build.sh` to have run once (it writes `build/game/gameinfo.txt`). `build/game/instances` is a symlink to Portal 2's
`sdk_content/maps/instances` (vbsp finds `instances/p2editor/*.vmf` through it; nothing of Valve's is copied). **Installed file:**
`...Portal 2/portal2/maps/pc_testbed.bsp` (delete it to uninstall). Load with `map pc_testbed`.

- **Geometry:** walls are 64 u thick (two 32 u layers; pockets for the laser props are cut 32 deep). Slots are 320 u apart along
  each side; a bay's `u` runs across it, `v` into it from the hall wall's outer face (y/x = +-1184), `z` up from the floor (z=0).
  The hall is sealed (no leaks) and has no sky: it is lit only by `light` entities.
- **Start:** `info_player_start` is in the arrival car (Valve's `elevator_arrival_logic` instance, named `arrival_elevator`),
  which rises and opens like in pit_flings. `departure_elevator` is `elevator_departure_logic` under a 4500 u shaft.
  Valve's `global_ents` instance (far away in a void box at 3200 -3200 0) supplies `@command`, `@tonemapper` and the
  `environment_*` relays; `environment_testchamber` is fired at start (fog 40 53 64, auto exposure .5..3, as pit_flings).
  `pc_map_alias.nut` makes `GetMapName()` return `sp_a2_pit_flings` here too (so the elevator videos, music and the next map work).
- **Lighting (measured with `pcb_probe`, pit_flings in brackets):** hall 0.05-0.07 [arrival area 0.05-0.07], ordinary bays
  0.07-0.10 [open chamber 0.1-0.17], bright bay 0.14-0.20, dark bay 0.008-0.011 [darkest pit_flings spot 0.015]. Lights are
  `light` entities 183 204 218 like pit_flings's chamber lights (brightness 12 hall, 22 bays, 17 bright bay: Valve's are 10-30,
  300-900 saturates everything), and the tonemap is the same `environment_testchamber` one. The sky is not visible from anywhere
  (the hall is sealed; the region box's sky never reaches it). Side-by-side: `cache/bridge/shots/tb_smoke_final/side_by_side_lighting.png`.
- **Names:** every testable entity is `tb_*`; panels and spots are `info_target`s (portalable surfaces are world brushes, so they
  get a marker at the panel centre; sizes and normals are in the JSON). Instance-internal names are `tb_arm-*`,
  `arrival_elevator-*`, `departure_elevator-*` (Valve's names, kept so the bridge sees what it sees in pit_flings).
  `tb_go_<bay>` and `tb_reset_<bay>` are `logic_relay`s: `ent_fire tb_go_lift trigger`.
- **Not in the map (no usable asset or not testable by an input):** a gel dispenser prop (`tb_gel` is a bare `info_paint_sprayer`;
  the dropper model needs a ceiling cavity), a laser relay *powered by a redirected beam* (the receptacle and the reflective
  cube are there, placing the cube is a player action), faith plate and tractor-beam behaviour needs the player/cube to enter them.

| bay | side/slot | interior min .. max (x y z) | standing spot | main entity | go / reset relay | tb_ names |
|---|---|---|---|---|---|---|
| portals | S0+1 | -1088 -1696 0 .. -512 -1184 320 | `tb_spot_portals` (-800 -1024 8, yaw 270) | `tb_panel_wall_back` | `tb_go_portals` / `tb_reset_portals` | tb_panel_wall_a, tb_panel_wall_b, tb_nop_wall_back, tb_panel_floor, tb_nop_floor, tb_panel_ceiling, tb_pair_a, tb_pair_b |
| fling | S2 | -448 -1824 0 .. -192 -1184 832 | `tb_spot_fling` (-320 -1024 8, yaw 270) | `tb_panel_floor_fling` | `tb_go_fling` / `tb_reset_fling` | tb_fling_ledge, tb_panel_wall_fling, tb_fling_floor_portal, tb_fling_wall_portal, tb_spot_fling_high |
| arrival | S3 | -128 -1504 0 .. 128 -1184 256 | `tb_spot_arrival` (0 -1024 8, yaw 270) | `arrival_elevator-elevator_1` | `tb_go_arrival` / `tb_reset_arrival` | arrival_elevator, @entry_door |
| mc | S4 | 192 -1696 0 .. 448 -1184 320 | `tb_spot_mc` (320 -1024 8, yaw 270) | `tb_panel_wall_mc` | `tb_go_mc` / `tb_reset_mc` | tb_panel_floor_mc, tb_panel_wall_mc2, tb_mc_lamp |
| dark | S5 | 512 -1696 0 .. 768 -1184 256 | `tb_spot_dark` (640 -1024 8, yaw 270) | `tb_dark_wall` | `tb_go_dark` / `tb_reset_dark` | tb_dark_lamp |
| bright | S6 | 832 -1696 0 .. 1088 -1184 256 | `tb_spot_bright` (960 -1024 8, yaw 270) | `tb_bright_wall` | `tb_go_bright` / `tb_reset_bright` | tb_bright_lamp |
| lift | N0 | -1088 1184 0 .. -832 1696 384 | `tb_spot_lift` (-960 1024 8, yaw 90) | `tb_lift` | `tb_go_lift` / `tb_reset_lift` |  |
| doorh | N1 | -768 1184 0 .. -512 1696 256 | `tb_spot_doorh` (-640 1024 8, yaw 90) | `tb_door_h` | `tb_go_doorh` / `tb_reset_doorh` |  |
| mlinear | N2 | -448 1184 0 .. -192 1696 256 | `tb_spot_mlinear` (-320 1024 8, yaw 90) | `tb_movelinear` | `tb_go_mlinear` / `tb_reset_mlinear` |  |
| departure | N3 | -128 1184 0 .. 128 1504 256 | `tb_spot_departure` (0 1024 8, yaw 90) | `departure_elevator-elevator_1` | `tb_go_departure` / `tb_reset_departure` | departure_elevator, @exit_door |
| train | N4 | 192 1184 0 .. 448 1760 256 | `tb_spot_train` (320 1024 8, yaw 90) | `tb_train` | `tb_go_train` / `tb_reset_train` | tb_train_p1, tb_train_p2, tb_train_p3 |
| rotate | N5 | 512 1184 0 .. 768 1696 256 | `tb_spot_rotate` (640 1024 8, yaw 90) | `tb_turntable` | `tb_go_rotate` / `tb_reset_rotate` |  |
| tilt | N6 | 832 1184 0 .. 1088 1696 256 | `tb_spot_tilt` (960 1024 8, yaw 90) | `tb_tilt` | `tb_go_tilt` / `tb_reset_tilt` |  |
| doorpanel | W0 | -1696 -1088 0 .. -1184 -832 384 | `tb_spot_doorpanel` (-1024 -960 8, yaw 180) | `tb_doorpanel_lift` | `tb_go_doorpanel` / `tb_reset_doorpanel` | tb_doorpanel_model |
| arm | W1 | -1696 -768 0 .. -1184 -512 256 | `tb_spot_arm` (-1024 -640 8, yaw 180) | `tb_arm-model_arms` | `tb_go_arm` / `tb_reset_arm` | tb_arm |
| cubes | W2 | -1696 -448 0 .. -1184 -192 320 | `tb_spot_cubes` (-1024 -320 8, yaw 180) | `tb_btn_floor` | `tb_go_cubes` / `tb_reset_cubes` | tb_cube, tb_cube_template, tb_btn_pedestal, tb_btn_lamp, tb_btn_lamp2 |
| laser | W3+1 | -1696 -128 0 .. -1184 448 256 | `tb_spot_laser` (-1024 160 8, yaw 180) | `tb_laser_emitter` | `tb_go_laser` / `tb_reset_laser` | tb_laser_catcher, tb_laser_relay, tb_laser_cube, tb_laser_lamp |
| bridge | W5 | -1696 512 0 .. -1184 768 256 | `tb_spot_bridge` (-1024 640 8, yaw 180) | `tb_bridge` | `tb_go_bridge` / `tb_reset_bridge` | tb_bridge_pit |
| funnel | W6 | -1696 832 0 .. -1184 1088 384 | `tb_spot_funnel` (-1024 960 8, yaw 180) | `tb_funnel` | `tb_go_funnel` / `tb_reset_funnel` |  |
| faith | E0 | 1184 -1088 0 .. 1824 -832 384 | `tb_spot_faith` (1024 -960 8, yaw 0) | `tb_faith_arm` | `tb_go_faith` / `tb_reset_faith` | tb_faith, tb_faith_target |
| fizzler | E1 | 1184 -768 0 .. 1696 -512 256 | `tb_spot_fizzler` (1024 -640 8, yaw 0) | `tb_fizzler_model` | `tb_go_fizzler` / `tb_reset_fizzler` | tb_fizzler, tb_fizz_cube |
| turret | E2 | 1184 -448 0 .. 1696 -192 256 | `tb_spot_turret` (1024 -320 8, yaw 0) | `tb_turret` | `tb_go_turret` / `tb_reset_turret` |  |
| goo | E3 | 1184 -128 0 .. 1696 128 256 | `tb_spot_goo` (1024 0 8, yaw 0) | `tb_goo_surface` | `tb_go_goo` / `tb_reset_goo` | tb_goo_hurt |
| doors | E4 | 1184 192 0 .. 1696 448 256 | `tb_spot_doors` (1024 320 8, yaw 0) | `tb_door` | `tb_go_doors` / `tb_reset_doors` |  |
| gel | E5 | 1184 512 0 .. 1696 768 320 | `tb_spot_gel` (1024 640 8, yaw 0) | `tb_gel` | `tb_go_gel` / `tb_reset_gel` | tb_gel_floor |

All coordinates are in `build/pc_testbed_layout.json` (`bays.<bay>.entities.<name>`: origin, mins, maxs, size, top, normal,
portalable). `tb_spot_<bay>` is 160 u in front of the bay's opening, 8 u above the floor, facing the bay; `tb_spot_fling_high` is
on the fling bay's 640 u ledge.

