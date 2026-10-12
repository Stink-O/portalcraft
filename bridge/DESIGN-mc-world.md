# How Minecraft is presented in every chamber: one world, or a stage per chamber? (bridge #74, 2026-10-09; proposal, nothing of it built)

The user's question: should there be one universal world across all chambers that the player can go back to, a typical generated world, instead of the synthesized per-chamber stage?

## What exists today (checked in the code, not remembered)

| | pit_flings session (`play_clip.sh --start mc`) | campaign session (`play_clip.sh --mc`) |
|---|---|---|
| Minecraft world | real generated overworld, seed 20260930, save `fabric/run/saves/Portalcraft_20260930` (`run_mc.sh --terrain`, min section y 3) | void "mirror" world, save `fabric/run/saves/SkyCraft` (`run_mc.sh` default, no height limit) |
| Where the chamber is | Portal 2's own coordinates mapped at 1 block = 40 u. The pinned cliff (MC -331 94 424) sits at Source z -13408, so the chamber (z about 0) is at Minecraft y about 440: above the generated terrain (top 320). Its collision is exported there (the "band") | the same plain mapping around Minecraft (0, 0, 0): the chamber is in the void at y -100..+100 |
| What G opens onto | the terrain at the cliff, shown in the map's hidden region box (a real hollow box compiled into pc_pit_flings) | the stage: a landscape made per chamber at y 640..767 (`StageTerrain.java`), shown in an empty solid box of the map's world (`stage.cpp`) |
| Next campaign map in the same session | #61: the same terrain, shown in the part of the old region box that is clear of the new map (`TerrainWindowEnsure`); the cliff is the same place every time | n/a (the stage is made again around the chamber's chunk columns, saved in the mirror world) |

So the "universal world with a home place" already exists for the terrain session: every later chamber's G-portal opens onto the same pinned cliff. What is not universal is `--mc`, and a terrain session that starts directly on a campaign map (it has no cliff streamed and no region box, so it falls into the stage code with no stage in a terrain world: the harness run `--mc terrain --map sp_a2_bridge_the_gap` waited for a cliff that never came).

## The three options

**(a) One persistent generated overworld for the whole campaign.** Every chamber's G-portal opens onto one shared home area (the pinned cliff, or a spawn-like meadow beside it). The terrain is real: biomes, caves, oceans, villages if the user digs far enough. Chambers keep the 1:40 mapping but each sits in its own slot of the world's high band (see below) so its collision and the blocks placed in it do not mix with another chamber's.
- Good: matches what the user asked for ("a typical normally generated world"). A base, a farm or a chest built at the home area is still there when the next chamber's portal opens (the place is the same). One world type for both session kinds: `run_mc.sh` and `play_clip.sh --mc` lose a branch, and the stage code, `StageTerrain.java`, the stage-only mod rules (`StageHost`, the barrier at the window edge, `sweepStage`) can retire.
- Cost: the box that shows the world is a window, not the whole world. The pit_flings window is a 160 x 120 x 160 block box (Source +-3200 u) round the cliff. The player cannot walk out of it and keep playing: the box edge is the plugin's invisible wall, the same as with the stage now. A true "walk anywhere" world needs the player to leave Portal 2's coordinates altogether (the Minecraft player standing in Minecraft's own world with Portal 2 only drawn as portals into it). That is a different product (the lead's route A) and not part of this proposal.

**(b) The current per-chamber stage.** Cheap in Minecraft (a void world and 600 flat chunks), works in 105 of 105 maps, nothing generated. Weaknesses: the landscape is synthetic (strips of plateau and lowland, one-block steps, no caves or biomes), it is a different place for each chamber (a base built in one is gone in the next), and it needs its own code in both the mod and the plugin.

**(c) Hybrid.** Terrain world for everything, with the stage kept only as a fallback if the terrain around the home place is not available. In practice (a) already needs a fallback while the terrain streams, so (c) is (a) plus the fallback, not a third design.

## How the mirror coexists with a real world (the chamber collision band)

The mirror requirement: while the player is in a chamber, Portal 2's collision must exist in the Minecraft world around the Minecraft player (so it can stand on the floors, mobs walk, blocks sit on them). It is exported as host collision (`SkyCollision`, 8-block regions), not as blocks, so it needs no terrain to be absent, only that the Minecraft world does not already have solid blocks there.
- The terrain session already does this by height: the chamber is at y about 440, above the 320 limit, where nothing generates. The host code treats everything from y 320 up as the chamber band (`BAND_MIN_Y = 320` in `HostWorld`).
- Per-chamber slot: the pit_flings anchor maps one chamber. For the next maps the plain mapping puts every chamber at the same place, which is why a block placed in one chamber is still there in the next (#61's open item). The fix is a Source-to-Minecraft x/z offset per map, `slot(map) * 2048 blocks` (a chamber is under 130 blocks wide, Portal 2's world is +-410 blocks), applied where `g_map` anchors are set (`TerrainSetRegion`, the same call the floating origin uses). Placed blocks then persist per chamber: coming back to a chamber restores what was built in it, and the home area is untouched. 2048 blocks is inside the generated world's chunk range, but nothing is generated there: the band is above 320, the terrain below, so the slots can be kilometres from the home area without cost (only chunks near the player are loaded).

## Persistence

- The world save is already persistent (`saves/Portalcraft_20260930`, 88 MB; the mirror save is 47 MB) and is not wiped per session.
- Inventory, health, XP: the Minecraft player's own data in that save, so it carries across chambers and sessions as long as one world is used. In the mirror world the same holds. Nothing to build.
- Placed blocks: at the home area, they persist (same place). In a chamber they persist per slot once slots exist (above). Without slots they leak into the next chamber at the same coordinates, as now.
- Dug cells (the SkyCraft-style wall digging) are keyed by `worldId` and by map version: unchanged.

## Performance

- Terrain world: the generator runs near the player. The home window is small. I have no terrain-session frame numbers of my own; the harness runs of the mirror stage this session gave RUN PERF mean 5.8 ms (about 170 fps, unfocused). Costs that exist only with real terrain: chunk generation and light for the 160-block window (done once, saved), RD 24 sections held by the host (the log shows 2000+ sections held).
- Mirror stage: about 600 chunks generated once, then nothing.
- Memory: Steam's /dev/shm leak and swap were the real crash source in earlier sessions (#49), not either world.
- Unmeasured: a terrain session started on a campaign map. It needs a measurement before it is the default.

## What changes in `run_mc.sh` / `play_clip.sh`

- `run_mc.sh`: `--terrain` becomes the default and the only mode; `--mirror` stays one release as a fallback. `SKYCRAFT_MIN_SECTION_Y` follows the world (3 for terrain).
- `play_clip.sh`: `--mc` stops asking for the mirror world (`is_terrain` health check inverted at lines 141-142); the "pit_flings terrain session" and "campaign session" become one: `--start mc|chell` and `--map` only.
- Plugin: `StageEnsure` today needs a session that began on a region map (`g_tbSet`, a remembered box) for the terrain window. For a session that begins on a campaign map it must make that box itself: `stagecalc::ChooseRegion` already finds an empty solid box in the BSP; the terrain around the pinned cliff has to be mapped into it (a Source offset, as the stage does, but the "belongs to the stage" test in `coords.hpp` is `y >= yMin`; the terrain is `y < 320`, the chambers above it, so the test has to become a range). Streaming and the exit search (`ClipTerrainCliffStreamed`) then work as in pit_flings.
- Mod: `StageTerrain` retires from the default path; nothing else changes. A per-map slot offset needs the mapping change only (the mod sees plain Minecraft coordinates).

## Recommendation

(a), reached in two steps, with (b) kept as the fallback until the step-2 session type has been run for a few chambers:
1. Today: make the pair views work in the stage (this report's item 1) and leave the world types as they are. Nothing in step 2 is needed for the release.
2. Next: one terrain world for both session kinds, the pinned cliff as the home place of every chamber, a per-map slot for the chamber band. The pieces that are not trivial: the empty box for a session that starts on a campaign map, the range test in `coords.hpp` (shared by every hot mapping function, so it needs the mapping tests and a full `tbtest` run), and per-map slots (changes where Minecraft places the chamber, which the collision cache keys must follow).

## What was implemented

Only item 1 (the pair/reverse views), which does not depend on the world type. The recommendation is not implemented: it touches the coordinate mapping used by every system and has never run on a session that begins on a campaign map; that is not low-risk for a release today.

## Bridge #78: built (2026-10-09/10)

Option (a), with one change of detail: the per-map "slot" is a per-map STORE, not a per-map place.
- Far-apart slots (2048 blocks per map) cannot keep the pinned cliff as every chamber's home: Minecraft only holds chunks within the render distance (24 chunks = 384 blocks) of its one player, so a Minecraft player standing in a chamber 2048 blocks from the cliff could not see the cliff through its G portal. Vertical slots over the cliff do not fit either (the campaign's chambers need far more height than the world can have).
- So every campaign chamber has the same place, over the cliff's columns: Portal 2 (0 0 0) = Minecraft (-320 752 432), the whole +-16384 u at y 342..1162 (the overworld is now 1232 high: y -64..1167). The mod's `ChamberSlots` keeps what each map's chamber holds: on every map change the band's blocks (y >= 320, with block entities) and entities of the loaded chunks are written to `<save>/portalcraft_chambers/<mapid>.dat` before anything is removed, and the new map's are put back; chunks that load later are swapped as they load (`owners.dat`). Blocks from before #78 belong to pc_pit_flings. SkyDig keeps every map's dug cells in that world.
- G opens onto the real terrain round the pinned cliff from every chamber (`stage.cpp UnifiedWindowEnsure`): the biggest empty slab under (or over) the map, the stage's offset mapping with a range test (terrain = y < 320), the pinned exit, the floating origin moving only the terrain's offset.
- Switch `pc_world auto|unified|legacy` (auto follows the linked Minecraft's world); the mirror world + stage stays as the fallback (`play_clip.sh --mc --world mirror`, `run_mc.sh --mirror`).
