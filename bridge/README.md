# bridge: Minecraft inside the real Portal 2 (route B)

A Portal 2 server plugin (native Linux, 32-bit) that draws Minecraft blocks in
Portal 2's own frame, makes them solid for Chell, puts Portal 2's real portals
on them, and can puppet the player. It follows chasm's SkyCraft (Skyrim + a
hidden Minecraft 26.3 over shared memory) with Portal 2 as the host. The
plugin framework is a trimmed fork of SourceAutoRecord (see `NOTICE`).

Phase 0 (this) proves the host side. Minecraft is not connected yet: blocks
are placed with console commands and textured from `cache/minecraft-26.3`.

## Phase 0 results

All numbers come from scripted runs in `sp_a2_bridge_intro`. Screenshots and
per-run logs are in `cache/bridge/shots/<run>/`.

| test | result | evidence |
|---|---|---|
| 1. `plugin_load` from outside the game folder, console + log | works | `cache/bridge/bridge.log` |
| 2. textured block in the chamber, depth-tested, drawn through the material system | works | `final/pcb_final_chamber.png`, `user/test2_block_in_chamber.png` |
| 3. block seen through a portal (every recursion level) | works; confirmed by the user in a live session | `user/test3_blocks_through_portal.png`, `final/pcb_final_through_portal.png`, view log below |
| lighting per face from the engine's model lighting | works, calibrated by eye | `light/grid.png` |
| a. engine traces from the plugin | 4096 rays in 9.2 ms (2.25 µs/ray), plus hull traces | `p0a/bridge.log`, `cache/bridge/trace_grid.txt` |
| b. plugin moves Portal 2's prop_portal pair | works; read-back matches to 0.005 u | `pwalk3/bridge.log` |
| c. player position every tick and view every frame | works (circle path) | `p0a/pcb_d_puppet*.png` |
| Chell stands on, walks off, jumps onto blocks, is stopped by block walls | works (movement-trace hook, no entities) | `collide/bridge.log` |
| portal on a block wall: low shot fitted flush to the floor, walk straight in | works, script input only | `pwalk3/bridge.log` (a) |
| portal higher on a block wall: lip blocks walking, jump goes through | works, script input only | `pwalk3/bridge.log` (b) |
| Portal 2 cube lands and rests on a block | works with an invisible SOLID_BBOX collider entity per block | `p0a/pcb_c_solid.png`: cube top at z 75.86 = block 39.8 + cube 36.06 |

View log from `final`, looking into a portal: each frame the hook runs for the
second recursion (origin x −914.9), the first (x −405.0) and the main view
(x 105.0), so blocks appear at every portal depth.

Collision log (`collide`): dropped onto a block, z 40.03; walked off, z 0.03;
jumped back on, z 40.01; walked into a block wall, stopped at x −63.97 (wall
face −80, plus the 16 u half-hull, plus the 1/32 u engine epsilon).

Portal log (`pwalk3`): aim (−80, 310, 20) was fitted to (−79.50, 309.88, 56.03)
and floor-snapped with its bottom edge at z 0.03. Walking in from x 40, the
player came out of the far portal at (77.2, 320.1, 25.5). Aim (120, 320, 90)
kept its centre, so the bottom edge sits at z 34. Walking in stopped at
x 103.97; jumping went through to (−61.96, 309.88). The input log shows only
the script's buttons during both walks. (An earlier run's transit is not
evidence: the user pressed jump in it.)

## Phase 1 status (in progress)

| goal | status | where |
|---|---|---|
| 1. Linux shared-memory link | **works**. The plugin `shm_open`s `/dev/shm/Portalcraft_v1` (191 MB). SkyLink maps the file (no kernel32). Clocks are CLOCK_MONOTONIC on both sides. The mod ignores mappings left by a crashed host and relinks when a new one appears. Protocol **v13** (phase A): HostViewExt at 0x188 (crop windows, a second camera) and a second image in the overlay slot header; the mod still links to v11/v12 hosts (no ext: one full-frame camera, as before). Layout test: `tools/layout_test.sh`, 207 constants, -m32 == -m64 == Proto.java | `src/bridge/link.*`, `protocol/`, `fabric/.../link/` |
| 2. Fabric mod built + run hidden | **works**. `tools/run_mc.sh [--terrain seed rd]` runs Loom `runClient` hidden and muted, and quits with the host. `build/fakehost` stands in for Portal 2 when testing the mod alone | `fabric/`, `tools/run_mc.sh`, `tools/fakehost/` |
| 3. MC player drives Portal 2 | **works; J in pc_pit_flings since bridge #13 (M1, gate `mcplayer1`, `shots/b13_mcp4`)**: J puts Minecraft's player at Chell's feet (chamber band or region) with her velocity, the body follows it, Chell and Portal 2's HUD hidden, the camera is Minecraft's eye (before the composite predicts cameras); the chamber collision is exported in terrain mode outside the region box with portal holes left out, Minecraft's own collision has the holes carved (`kInHole`), a portal transit of the body teleports Minecraft to the exit with the velocity (pit portal mid-fall, the cliff portal walked into); J back keeps position, view and velocity. Hand/HUD/body: M2 (DESIGN-mcplayer). Milestone run `mc1b` (mirror world). PlayerPuppet: ProcessMovement write-back, interpolated on Minecraft's tick timestamps. CameraDriver: ClientModeShared::OverrideView (Minecraft eye + FOV). InputBridge: SDL scancodes; Portal 2's usercmd buttons cleared in CreateMove. CollisionField: world-only voxelisation, 8 ms/region under a 4 ms/frame budget. Minecraft walked through the chamber and stopped at the wall (x 243 vs wall 256 − 12) | `src/bridge/mc.cpp`, `puppet.cpp`, `collfield.cpp` |
| terrain meshes (render ring) | **works but superseded**. kRenSection → static meshes built on the render thread (call-queue functors). CreateStaticMesh takes 4 arguments in Portal 2 | `src/bridge/terrain.cpp` |
| pit-flings redirect | **works**. A floor portal in the pit moves its partner onto a flat patch of the Minecraft cliff face through the prop_portal's own NewLocation input, so no sv_cheats is needed. If Chell has fired only one colour, the partner is spawned. Transit not yet verified | `src/bridge/clip.cpp`, `ents.cpp` |
| Chell on Minecraft terrain | coded (kRenSolids feeds the TracePlayerBBox hook) and untested | `blockcollide.cpp` |
| cliff exit | **pinned and validated**: the exit is the patch the mod's SpotFinder found for this save (bridge #81: no fixed seed, no hand-picked cliff; cache/bridge/portalcraft_world.env), a natural 2x3 vertical face with clear landing ground. It is accepted only once its sections have streamed, every wall cell is solid and two blocks in front of the whole opening are air; checked again before every redirect (the old finder ran mid-streaming and put the exit inside rock). The finder is a validated fallback for `pcb_cliff` only. Assert `pcb_assert_exit` | `clip.cpp`, `shots/val2b` |
| exit re-check + user launcher | **verified (run `userflow1`)**. The pinned exit is re-checked and logged on level load, link up/down, Minecraft resending its world and each batch of sections while unresolved (`clip: exit check (<trigger>)`, plus a "still waiting" line every 5 s), so start-up order does not matter. A level reload with the same region mapping keeps the streamed solids (they are keyed by Minecraft section; Minecraft sends each section once per link, and the old clear on reload lost the cliff for the session); a different Minecraft linking drops the old one's terrain. `tools/play_clip.sh` is the one-command user session (see Build and load); `tools/test_userflow.sh` (+ `cfg/userflow.txt`) runs it through a fresh launch with portals placed before the link, a `map` reload and a Minecraft restart: 12/12 asserts | `clip.cpp`, `terrain.cpp`, `mc.cpp`, `tools/play_clip.sh`, `shots/userflow1` |
| pc_mc_portal (portal into Minecraft from anywhere) | **verified (run `mcportal4`, 16/16)**. `pc_mc_portal [1/0]` (key G, polled through SDL like J, so no bind is written; `pc_mc_portal_key <scancode/0>`) arms the next shot; pressing again cancels. The next portal Chell fires (either colour, any portalable surface) keeps Portal 2's placement; once it has landed (at the spot Portal 2's own TraceFirePortal chose) its partner is spawned if needed and held at the pinned cliff exit through the same Redirect and patch check as the pit rule. One shot disarms; the anchor is released when it moves or closes or when Chell fires the partner's colour. `pc_pit_redirect 0/1` (a command; the trimmed SAR has no ConVars) toggles the pit-floor rule. `pc_mc_portal_sound 1` plays a quiet game sound (off by default). Log-only (quiet mode). Tested with the pit rule off: cancel, arm + wall shot, walk in, back out through the cliff portal (block collision off for that approach, as in trans2), arm + floor shot and fall in **Bridge #12 (the user):** the pit floor goes to Minecraft only with G now: `pc_pit_redirect_mode 0` off / **1 G only (default)** / 2 always (the old rule; every suite that relies on it sets 2; `pc_pit_redirect 0/1` = 0/2). A shot fired while G is held counts as armed (as well as tap-then-fire). **Ways back:** in Minecraft a portal fired with G puts its partner back in the chamber (the last chamber wall a portal was on, else a default wall: pit flings' (-560 -60 64), else a wall ahead of the start), so she walks back through it; **H held 0.5 s** in Minecraft (`pc_mc_home`, key `pc_mc_home_key`, SDL scancode 11, unbound in Portal 2) teleports her to the last chamber spot she stood on (on the ground 0.5 s, away from portals; else the start), velocity zeroed, a held cube with her (re-taken with a +use if the hold broke). Chell mode only (Minecraft-player mode: not done). Gate `home1` (`shots/g12_home1`): no-G pit pair stays in the chamber; G pit pair goes to the cliff; G-in-Minecraft pair walks her home; H from ~1500 blocks out with a cube: home within 0.6 u, holding, every frame clean, ambience back, floating origin at its anchor. play_clip.sh prints the keys | `clip.cpp`, `portalgun.cpp`, `mc.cpp`, `floatorigin.cpp`, `tools/cfg/mcportal1.txt`, `home1.txt` |
| G portal in every campaign chamber (bridge #49) | **works on bridge_the_gap, Chell and the Minecraft player, there and back** (`tbtest -k campaign_stage`). No hidden box in Valve's maps: the mod makes a real landscape (the stage, y 640..767) and the plugin shows a window of it in an empty box of Portal 2's world chosen from the BSP (`stage.cpp`); on from the first G, off with the level; `pc_mc_stage 0` disables. Way-home portal drawn flat by Minecraft (Portal 2 draws nothing in the void). | `stage.cpp`, `StageTerrain.java` |
| Minecraft<->Minecraft portal pairs | **verified (run `mcmc2`, 21/21)**. Region cameras follow Portal 2's recursion: from the main camera every visible portal on the camera's side of the region boundary recurses through its partner up to depth 2, boxed inside the parent opening; each region camera gets its own stream (6: three full-screen for the camera and portal views, three 1024x512 for recursion views, crop clamped). Minecraft renders the largest view every frame; the others share the second slot (longest waiting first, every 3rd host frame unless a view has no image, `pcb_composite_second_every`). A view draws only its own stream's image from its own camera chain; with none it shows the portal's colour (black for the camera's own view). `pcb_assert_own_streams` (0 foreign draws in every phase: recursion tunnel, turning, side by side on the cliff, repeated walk-through, the gun's ground + cliff pair). Tunnel: 5.57 ms mean, worst 14 ms (`mcmcprof`); VmSize 2.17 GB | `composite.cpp`, `tools/cfg/mcmc1.txt`, `mcmc_prof.txt` |
| Minecraft hang in host views | **fixed**: vanilla `Frustum.offsetToFullyIncludeCameraCube` looped forever for an oblique (portal-view) frustum in the second-camera pass, freezing the hidden Minecraft mid-session (and it then never quit with its host). `FrustumOffsetGuardMixin` bounds it. `pcb_assert_mc_alive` fails a run whose Minecraft stops rendering | `FrustumOffsetGuardMixin.java`, `mc.cpp` |
| Minecraft freeze (render thread parked, never quits with its host, ignores TERM) | **fixed (bridge #9, mod)**: a pipeline first used mid-frame is compiled by `PipelineCache.get` on the shared background pool while the render thread blocks on it; with the pool busy rebuilding sections whose uploads only the render thread drains (a burst after a teleport at render distance 24), the compile never ran (jstack `shots/edge7/jstack_mc.txt`). `PipelineCacheInlineMixin` compiles it on the render thread itself. No freeze and a clean quit with the host in every run since (edge7b, edge8, edge8b, edge9-9e, float2) | `PipelineCacheInlineMixin.java` |
| first portal shot in Minecraft animates | **fixed (bridge #9)**, frame sheet `shots/first9/sheet_first9.png` (chamber / old / fixed by Portal 2's open amount): (1) Portal 2 re-tests a new portal's resting surface 0.1 s after placement with short rays straight back from its corners; on Minecraft blocks they missed and it closed the portal at open ~0.2 (the gun's re-apply restarted it 0.77 s later). A server TraceRay hook (`fluids.cpp`, slot 5) makes a short ray in the region that ends in a solid Minecraft block hit it (`pcb_surface_trace`); removing the block still fizzles the portal. (2) While a lone portal opens, Portal 2 draws the opening without writing depth, and the region background (translucent pass, sky depth) covered it until open 1. While a lone region portal opens, the main region view is now composited at the start of Portal 2's opaque pass (`DrawOpaqueRenderables` hook, SAR's Linux signature), before its portals (`pcb_early_opening`). Normalised disc width at open 0.5/0.6/0.7/0.8: chamber 0.34/0.46/0.55/0.69, fixed 0.39/0.52/0.64/0.80, before 0.17 (nothing) until open 1 | `fluids.cpp`, `render.cpp`, `composite.cpp`, `tools/cfg/first9.txt`, `tools/open_sheet.py` |
| Portal 2 props on Minecraft terrain | **verified (run `props2`, 6/6)**: cubes rest on terrain after a drop and a 10-block fall (centre 18.1 u above the ground top, still), and a cube that falls through the pit portal is flung out of the cliff portal and lands on the terrain below. A pool of invisible one-block colliders (the Phase-0 SOLID_BBOX collider, up to 320) follows the physics props in the region: the exposed solid blocks within 2 blocks of each prop and of where it will be 0.05 / 0.1 s ahead, none in an open portal's hole. 0.1-0.2 ms per tick. **Near an open portal** (within ~2 blocks) Portal 2 simulates a prop in that portal's own physics environment, which holds a carved copy of the map and none of our colliders, so a cube set down there fell through placed blocks (float2: the corridor floor, MC y 89 -> 63; run `propfloor`: a cube 80 u before the portal fell, 200 u away it rested). Push-out (bridge #10, `pcb_prop_pushout`): a prop sunk more than 0.5 u into a block top (outside portal holes) is lifted onto it with point_teleport and held there every tick (Sleep stops its velocity but it does not stay asleep) until something moves it more than 3 u (pickup, a push) or the block goes. Push-out takes the block under the prop's centre only (bridge #13: a cube sliding off a one-block step was lifted back onto it and pinned half off it, props1's cube_fast / cube_fling; `waituntil still<name>`). Gate `propfloor` 9/9 (`shots/propfloor2`): cubes rest on the placed floor at y 89 beside, between and before the portals, dropped from 3 blocks too; a held cube is picked up, lifted 66 u and dropped back onto the floor. Gaps: only block tops (a prop pushed sideways into a block wall near a portal still passes in); colliders are SOLID_BBOX entities, so a gun shot at terrain right next to a cube may stop on a collider; no stacking test yet **Carrying a cube (bridge #12, the user's "invisible blocks")**: the colliders' 39.6 u boxes sat 0.2 u below the block tops; Portal 2's own ground trace stood Chell on them, 0.17 u inside the blocks for the block hook, so every seam between two floor blocks was a 0.17 u wall (she stopped dead; colliders exist only around props, and a held cube is always in front of her). Fixed twice over: colliders are raised 0.2 u (top = block top, `pcb_prop_collide_lift`), and the block sweep takes a hull resting up to the 0.5 u skin below a block top as standing on it (`pcb_collide_seams`). Also: a floating-origin shift now moves every collider with the world (they kept their old Source spot), and push-out never pins the prop Chell holds. Gate `carry1` 23/23 (`shots/g12_carry1`): a cube carried through the pit portal, over a stone course (flat, a jump up a step, past pillars, back), dropped and picked up again (pool capped so colliders are moved), a shift, and through a Minecraft<->Minecraft pair; `pcb_blockers` attributes every wall Chell's traces hit (block / world / collider / portalenv / other) and every ground speed cut: only real blocks. Control/fix pair `shots/carry_iso3` (old: stopped after 4 u, feet -14168.17, 112 block wall hits; new: 175 u/s, 0) | `propcollide.cpp`, `blockcollide.cpp`, `tools/cfg/props1.txt`, `carry1.txt`, `carry_iso3.txt` |
| normal Minecraft world + Chell inside Minecraft | **verified (run `chell2`, 4/4)**. The terrain world runs as a normal world: day/night, weather, natural mob spawning, difficulty normal (the hidden player stays creative, so mobs ignore it). While Chell is in the region she is mirrored into Minecraft through SkyCraft's actor table as an invisible hittable stand-in (`chellproxy.cpp`, SkyrimActorEntity): monsters within 32 blocks that see her take her as their target (`SkyCombat.huntChell`; naturally spawned skeletons at night did), pressure plates and tripwires under her trigger (SkyCraft's stepOnTriggers), and Minecraft's hits (melee, arrows: kEvHitActor) and explosions (kEvExplosion, Minecraft's exposure formula) reach her as Portal 2 damage (x5 by default, `pcb_chell_proxy 1 <scale>`) plus a push. Tested: a zombie (8 hits of 3), skeletons, a creeper explosion (15.9 at scale 1). Test hook: `pcb_mc_cmd` / `pcb_mc_summon_near` queue Minecraft server commands through cache/bridge/mc_commands.txt | `chellproxy.cpp`, `link.cpp`, fabric `SkyCraft.java`, `SkyCombat.java`, `tools/cfg/chell1.txt` |
| Minecraft sound | **verified for listener and level (run `audio2`, 7/7); not listened to.** Protocol v14 HostAudio (0x80; the mod accepts v11-v14): every frame the plugin drives Minecraft's OpenAL listener and overall gain (`audio.cpp`, mod `SoundEngineListenerMixin`), never the hidden player's camera: in the region, Chell's head (full level); in a chamber near a portal whose partner is in the region, her head carried through the pair (the portal view's camera), the level falling off from 2 to 10 blocks (smoothstep); otherwise, paused or in a menu, silent. Smoothed (80 ms, at most 0.08 per frame): asserted steps 0.066 entering the portal's range, 0.038 over the fall through the pit portal. `pc_mc_volume` (default 0.6) sets Minecraft against Portal 2. The hidden player follows Chell in the region (re-placed every 12 blocks) because Minecraft's server sends a sound only to players within ~16 blocks; it is parked at the cliff otherwise. Chell's stand-in plays Minecraft's step (every 1.7 blocks), fall/landing and splash sounds; Portal 2's footsteps are off (sv_footsteps' value set directly, no notice) while she stands on Minecraft blocks. Muting: Minecraft is audible only when started with `run_mc.sh --audio` (play_clip.sh does, unless `--mute` or `--script`), and every scripted run sends it gain 0 (`pcb_audio_test_mute`). Not done: an audio capture (it would be audible), tuning `pc_mc_volume` by ear | `audio.cpp`, `chellproxy.cpp`, fabric `SoundEngineListenerMixin`, `SkyCombat.chellSounds`, `tools/cfg/audio1.txt` |
| pit transit (natural play) | **verified**. Chell's own gun: orange on the pit wall, blue on the pit floor; the redirect moves orange to the cliff; she falls in at 900 u/s and exits the cliff portal at 919 u/s horizontal, lands on terrain 1.5 s later. Also with a portal_place pair | `shots/pit6`, `pit7`, `pit9` (`pcb_track` log) |
| Chell on Minecraft terrain | **walks, stops (friction), lands**; collision matches Minecraft's real blocks (pcb_column vs CliffFinder profile). Fixed: resting contact sank 0.2 u per tick into blocks (stuck); Portal 2's own ground check never saw blocks (now SetGroundEntity hook, slot 70). **Jump on terrain still does not fire** | `blockcollide.cpp`, `shots/pit9` |
| level end / map-name shim | verified: `GetMapName=sp_a2_pit_flings`; the departure elevator changelevels to sp_a2_fizzler_intro | `shots/pit6`, `pit7` |
| compositing: one Minecraft layer | **phase A of DESIGN-composite.md, validated (see transition test)**. Per frame (OverrideView, pose final) the plugin predicts every camera that can see the region: Portal 2's own camera when it is in the region, the virtual camera of each on-screen portal whose partner is in the region (the portal transform of the live pair), or, with nothing on screen, the view through the pit portal kept warm. At most 2 (K), largest on screen first; each goes to a stream by Minecraft-camera continuity (position decides: within 2 blocks plus its own motion per frame of gap; Portal 2 turns Chell's camera up to ~80 deg in one tick after a floor-to-wall transit) and is requested with its screen box as crop window (primary + 0.1 NDC margin, 10% overscan frustum; secondary no margin, RD 12, rendered as a second level pass in the same Minecraft frame). Each Portal 2 view is matched to its predicted camera by pose (exact for 93% of region views), else by stream continuity, else the nearest image; it draws its stream's newest image reprojected (rotation, and position against a proxy plane at the image's centre depth), or holds it unreprojected when the camera turned past it. Placement by the view's geometry only: behind a region portal's plane (or a portal view just in front of it) = world-space geometry beyond the plane, at least 2x the near plane out; anything else = background at the sky depth with the depth test, plus per-pixel occlusion layers (their margin grows with the image's pose error); Chell's eye within a unit of the pit portal's plane = the region camera beyond it, over the whole (void) view. No region view ever shows the region's background after the first image (asserted over 39k region views). Removed: ExitingPortal / g_exitFrame, late-main and proximity matching, the alternating stream scheduler, the depth mesh, lock-step and the after-post-processing paint overlay | `composite.cpp`, fabric `portalcraft/`, `DESIGN-composite.md` |
| position reprojection per pixel (walking wobble) | **fixed (bridge #11), run `wob1`** (host 180 fps). The user saw Minecraft's world swim against Portal 2's while walking, never while turning: position reprojection against one proxy plane (the image centre's median depth, re-picked per image) was wrong by residual x (1/z - 1/D) off that depth, with a residual changing every frame. Each vertex's ray now goes to the depth Minecraft wrote where it lands in the image (depth alpha, 3 fixed-point steps), 48 x 27 grid (portal views 32 x 20), shared by background, occlusion layers and probes. Jitter of fixed block-face points against where Portal 2 would draw them, p95: walk 1.80 -> 0.15 px, strafe 4.82 -> 0.13, fly 1.22 -> 0.05, turn ~0 both; drawn second difference = Portal 2-native. Minecraft's walk bob / hurt tilt are off while the host drives the camera (`HostViewNoBobMixin`). `pcb_reproject_pixel [iterations] [gx gy]` (0 = the old plane), probe `pcb_wobble`, gate `tools/cfg/wob1.txt` (p95 <= 0.5 px) | `composite.cpp`, fabric `HostViewNoBobMixin`, `tools/cfg/wob1.txt` |
| Portal 2's chamber ambience in Minecraft | **fixed (bridge #11), per channel since bridge #12: `amb2` 8/8**. While Chell's eye is in the region the chamber soundscape fades out over 0.5 s and comes back on the way out: every playing channel whose sound is under `ambient/` has its master volume scaled (`snd_set_master_volume <guid> <0..255>`; channels read with `snd_print_channels`, replies parsed from the console: on entry, every 0.25 s and when an `ambient/` sound starts, from `snd_report_start_sound`); the mix group Music 0.6 -> 0 (`snd_setmixer`, no cheat); `dsp_room` held at 0. Bridge #11 faded the mix group "Ambient", whose rule matches any name containing "ambien": that silenced the portals' hum (`weapon_ambient/wpn_portal_ambient_lp_01.wav`; measured, `shots/sndprobe2`: speaker gains 2/2/2/14/14 -> 0); the user heard no hum in Minecraft. Now the hum 64 u from the cliff portal reads 23 (chamber reference 28; 74 after a real fall), the soundscape channels 0, Music 0 (`pcb_assert_hum`, `pcb_assert_ambience`). Every sound command runs silently since bridge #13 (`ConRunSilent`: the command's callback in a thread-local logging state holding only the plugin's listener; no `snd_report_start_sound`, a silent channel scan every 0.05 s instead): nothing reaches the console or the screen even with developer 1 (`quiet1`, `shots/b13_quiet1`). `pc_mc_ambience 0/1` | `audio.cpp`, `conlog.cpp`, `tools/cfg/amb2.txt` |
| occlusion: Portal 2 objects behind Minecraft terrain | **portals, per pixel**: Minecraft writes its linear depth into the image's alpha on the GPU (log-encoded, 0.25-512 blocks, rounded towards farther); alpha-tested layers of its colour over each region portal's screen box (rim and particles included; the opening against the portal plane's analytic depth) hide portals behind terrain without touching Minecraft's own edges. Props and Chell: not yet. Assert `pcb_assert_occluded` | `DepthAlphaPass.java`, `composite.cpp` |
| performance | prof3 (all features on, RD 24, 180 fps cap): mean 5.55-5.56 ms in every phase (look-down, occluded view, turning, walking, near the cliff portal at 1/2/4 blocks straight on and at 45 deg, slow walk-through). Worst frame 7.3 ms looking down at the floor portal, 10-14.8 ms in the region phases (composite off: 7.7-9.1 ms). Minecraft 107-180 images/s, latency 2-3 host frames. Main thread: pull 0.006 ms, composite draw 0.1-1.0 ms (occlusion layers). Render thread: the material system's Download costs **~5-6 ms per image whatever its size** (the regeneration copy itself 0.05 ms cropped / 1.0 ms full screen; the rest is a fixed cost after it: ping-pong textures and a 4x smaller texture did not change it, so it is not a stall on the texture in use); the old code had the same cost unmeasured. The 0.5 ms render-thread budget needs phase C (direct GL upload) | `shots/trans2` |
| render distance | 24 by default (`run_mc.sh`, forced at startup); collision export capped at 8 chunks (`skycraft.exportRadius`), all heights (sections below `minSectionY` ship collision only, no mesh); `PORTALCRAFT_MC_FPS` caps Minecraft. Budget runs are valid only with the game on DP-3 (180 Hz) and focused: read each run's HOST frame stats; never script window placement | `run_mc.sh`, `SkyClient.java` |
| jump on blocks/terrain | **fixed**: apex 43.1 u on Portal 2 floors and Minecraft terrain (Portal 2's CheckJumpButton leaves the ground before adding the jump velocity; the block-ground override is off inside it) | `blockcollide.cpp`, `shots/comp9`, `val1e` |
| Minecraft water / lava | water is CONTENTS_WATER for Chell (point-contents hook; fluid bitsets after the solids): Source swimming (hold jump to rise, sinks otherwise, as in Portal 2); lava kills | `fluids.cpp`, `WorldExporter.java` |
| test runs | `pcb_assert_*` (soft mode `pcb_assert_soft`), `waituntil`, `pcb_run_summary`; frame analysis (`pcb_assert_mask`, `pcb_frame_watch [transit]`, `pcb_assert_occluded`): the per-frame watch reads each frame back on the render thread and analyses it one frame later against that frame's snapshot (a main-thread ReadPixels on the queued context returned the previous frame); poses (`pcb_view_portal r d angle orbit`, `pcb_view_occluded`, `pcb_aim`, `pcb_aim_flat`, `pcb_goto_portal`, `pcb_goto_aim`); `pcb_aim_track` (eased, <= 180 deg/s, released on the teleport tick; unreachable targets not aimed) + `pcb_aim_check`, `pcb_launch_to`, `pcb_stop`, `pcb_transit_set`, `pcb_assert_no_missing`, `pcb_assert_sections_mc`; validation `tools/cfg/val2.txt`, transition test generated by `tools/gen_trans.py` (`tools/cfg/trans2.txt`, short `trans2s.txt`); `run_game.sh` moves a run's frame dumps to `shots/<run>/frames/` | `testassert.cpp`, `transit.cpp`, `composite.cpp`, `tools/` |
| portals on Minecraft blocks (Chell's gun) | **works**: `TraceFirePortal` hook (server.so, SAR signature) marches the shot through Minecraft solids; Portal 2's fitting rules on the block face (bump <= 91.2 u, floor snap, partner overlap, floor portals along the shot); Portal 2's FirePortal places the real portal (re-applied through NewLocation for 1 s, Portal 2's resting-surface test would close it). Both colours, cliff and ground, linking to each other or back to the pit portal, transit both ways (asserts `pcb_assert_gun_block`, `pcb_assert_teleports`). A partner placed on terrain is not redirected | `portalgun.cpp`, `shots/val2b` |
| noclip | flies through Minecraft blocks (block collision, block ground and block jumps are off while the player's movetype is noclip, read from the server datamap) | `blockcollide.cpp`, `ents.cpp` |
| transition test | **trans2 (06:17 build)**: 100 gun-portal crossings (20 + 20 chamber->Minecraft walk / fling, 20 + 20 Minecraft->chamber walk / fling, 10 + 10 glancing), every frame checked: 0 missing, 0 stale, 0 pose-mismatched frames in every set, 0 region views without an image (of 39,331), portal_in_view 2,730 approach frames all in view; 7 single white frames (5 at the pit portal's plane the tick before the teleport, fixed in the 06:27 build; 2 bloom around Minecraft's sun, exclusion widened). Final gate on the candidate build: see the phase A report | `tools/gen_trans.py`, `shots/trans2` |
| Minecraft: second camera, crop, cliff streaming | v13 hosts: the second camera is a second level pass in the same Minecraft frame (keeps the first camera's view area and section graph, culled to its RD, fresh fog buffer and camera uniforms), the crop is an NDC translation (1:1 pixels into the target's corner) with a sub-rectangle readback, both images in one overlay slot. Measured with `build/fakehost` (v13 options): 513x360 + 640x433 images per frame, 2.3-2.8 ms Minecraft work per frame. Section export no longer drops sections outside the export radius (they wait for the player; the cliff exit was sometimes never streamed after a fresh start); `pcb_assert_sections_mc` checks the exit's sections by id before a run uses them | `GameRendererViewMixin`, `CameraHostViewMixin`, `WorldViewExporter`, `WorldExporter`, `tools/fakehost/` |
| entering the cliff portal with block collision on | **gap**: Chell launched at the cliff portal (region side) stops with her hull at the rock face around it in many tries; Portal 2's transit never takes her. The transition test turns block collision off for those approaches (`pcb_collide 0`). Walking out of the cliff portal and falling into the pit portal are unaffected | `blockcollide.cpp` |
| mcworld suite (portals shot inside Minecraft) | **in progress (bridge #6)**. `tools/gen_mcworld.py` writes `tools/cfg/mcworld1.txt` (`--short` gives `mcworld1s.txt`). It covers: the placement matrix with the real gun (ground, cliff, ceiling, slope, edge bump, water/leaves/glass rejected, re-fire, a shot next to a cube); preview stability per pair at 2/6/15/40 blocks, still/walk/strafe/180 deg/s turn, with every frame watched (`pcb_frame_watch N label mcworld`), image age per view (`pcb_assert_image_age`) and twitch (`pcb_twitch`); transits per pair (walk/jump/fling, `pcb_assert_transits`, never stuck: `pcb_assert_stuck`); persistence (30 s, night, 150 and 500 block trips: `pcb_portal_mark` / `pcb_assert_portals_unchanged`); frame-time windows; one Chell-proxy-on case (the proxy is off in the rest of the suite, because mob hits knocked Chell off her scripted drops). Helpers: `pcb_find_surface <kind> [radius] [like] [cube \| place=<block>]` (with `waituntil placed` and `pcb_place_clear`), `pcb_view_from`, `pcb_stand_before`, `pcb_cam_turn`, `pcb_fly_to`, `waituntil near`. Fixed by it: a throttled second-camera request was lost when Minecraft read the view on an off frame (portal views up to 204 frames stale), and the second slot is now scheduled per view period; a gun shot beside a cube stopped on a prop collider; water was not rejected; leaves, glass and partial blocks are now non-portalable through a fourth bitset in kRenSolids; killing a cube in the region crashed the game (dangling prop pointer); Chell got stuck 3 u in the ground beside a floor portal | `portalgun.cpp`, `composite.cpp`, `puppet.cpp`, `propcollide.cpp`, `blockcollide.cpp`, `tools/gen_mcworld.py`, `DESIGN-mcportals.md` |
| Minecraft draws portal pairs inside Minecraft (v15) | **built (bridge #7), gate `mcportals1` 98/99** (`shots/mcp_after4`; the one failure is a twitch view whose portal centre is behind terrain, unmeasurable in the baseline too). When both portals of the live pair are in the region the plugin sends the pair (HostPortals @0xE00, protocol v15; the mod still links to v11-v14 hosts and a v15 host can announce 14 through `cache/bridge/proto_version` for A/B runs) and makes no portal-view streams; Minecraft renders, in the primary camera's pass, the view through each visible portal (virtual camera = primary x portal transform, oblique near plane on the partner, culled to the opening's screen box, render distance 12) into its own reused target and composites it through Portal 2's opening (an ellipse, depth-tested; colour and depth-in-alpha copied, so inside an opening the alpha is the virtual camera's depth), recursing to depth 2, then the portal's flat colour; portals hidden behind terrain are skipped (raycast). Every Portal 2 view in the region draws that one image screen-mapped (portal views as world-space geometry at the background depth of their own projection, so Chell and props in them stay in front), with occlusion layers for props (all views) and Chell (portal views). Each extra level pass has its own CloudRenderer / FogRenderer / projection buffer (vanilla's per-frame cloud buffer was overwritten by the second pass: the cloud twitch), the second camera of mixed pairs included. Crossing a pair portal carries the main stream through it (no new camera). Opening (bridge #9, gate `edge9e`/`edge8b`): 29.1 x 50.3 u centred 1.36 u up the portal, growing with smoothstep(open)^2 predicted 2 frames ahead. Gate `pcb_measure_gap` (same frame: Portal 2's portal views tinted x[0 1 0] with `pcb_mc_portals_shape w h 0 5`, Minecraft's opening flat magenta; green = gap, magenta beyond the opening's edge = overshoot, colour-aware through the rim): 0 px overshoot on every side, both colours, 1/3/6 blocks straight on, 45 deg, and through the re-fire open animation (open 0.71-1); the control 32 x 56 overshoots 16-20 px at 3 blocks, 9-11 at 6. Gaps lie under Portal 2's rim, whose refraction samples what we draw there, so they are judged in close-ups (`shots/edge8b/closeups_top_OLD_A_SMALL.png`): #7's 27 x 47 centred shows a thin wall band at the top (the user's report), 24 x 42 a band all round, 29.1 x 50.3 none. Known: a re-fire on the exact same spot shows the previous opening for up to 2 frames (image latency). `pcb_mc_portals [0/1] [rd] [res] [depth]`, `pcb_mc_portals_shape` | `PortalPasses.java`, `PortalComposite.java`, `composite.cpp`, `tools/gen_mcportals.py`, `DESIGN-mcportals.md` |
| test placements are real | every placement in mcworld1/cloud1/mcmc1/transitspeed/mcportals1 is a gun shot on a block face checked by `pcb_assert_portal_face` (solid behind, air in front, at 9 points; unstreamed sections count as air only once streaming has settled, `waituntil terrain_idle`); the facing pair is `pcb_build_corridor` (two stone walls built in open air with fill commands and removed with `pcb_build_corridor remove`). The old mid-air `pcb_portal` pairs were not on faces (facing pair floating, mcmc1's side-by-side partly inside the cliff) | `portalgun.cpp`, `tools/cfg/*.txt` |
| Minecraft lighting on Portal 2 objects in the region | **verified and promoted (bridge #8): light1 6/6** at the calibrated scale (`shots/light3`, sheet `light3/light_sheet.png`; first run at scale 1.0 `shots/light1`). Gun shell on screen (0-255): noon 222, overhang 161, cave 18, midnight 87 (Portal 2's own lighting: 218), torch at night 210; held-item lightmap ratios would give 135 / 21 / 69 / 217, so overhang and midnight stay ~20% brighter (Portal 2's tonemap compresses the white shell). Protocol v16: the plugin asks for Minecraft's light at Chell's eye and at every model drawn in the region (LightProbes @0x16100); the mod answers with the lightmap colour at each probe's block (CPU copy of `lightmap.fsh` with the inputs Minecraft last used), sky/block levels, sky visibility, sun direction and daylight (McLight @0x16400; v11-v15 hosts still link). Hooks: VStudioRender026 DrawModel (28) sets the ambient cube (SetAmbientLightColors 19) and drops local lights (SetLocalLights 21) for a model whose origin is in the region, restoring afterwards; VEngineModel016 ComputeLightingState (26) for the array path (cubes). Cube per axis = Minecraft's lightmap colour x its entity shading for that direction, ^2.2, x `pc_mc_light_scale` (0.55, fitted so the gun's on-screen ratios follow the held-item lightmap ratios), + sun/moon on the faces that see it under open sky; 150 ms smoothing and a 150 ms fade in from Portal 2's lighting. Measured (eye cube luminance, scale 1.0): noon 0.638, overhang 0.179, cave 0.003, midnight 0.046, torch at night 0.522; a cube at night 0.047 vs Portal 2's 0.384. Chamber: 0 overridden draws, gun brightness 223.5 vs 224.0 off/on. `pc_mc_light`, `pc_mc_light_scale`, `pcb_light_status`, `pcb_light_mark`, `pcb_assert_light_*` | `mclight.cpp`, fabric `LightProbes.java`, `LightmapCaptureMixin`, `tools/cfg/light1.txt` |
| never stuck in blocks | **verified (bridge #8, `shots/tspeed4` 35/35, promoted)**: a move that ends with Chell's hull inside Minecraft blocks moves her to the nearest free spot (up to a block up or aside, free of blocks and of Portal 2's world), as Source does for brushes. It fixes tspeed3's "beside the ground portal" case (a setpos onto a one-block step). `pcb_unstick` Portal holes (bridge #12, the user's "collision breaks if I go low enough"): the user's log showed Chell, after transits through two floor portals on blocks, behind a floor portal's plane with no transit; the hole carved behind a portal for her went 500 u deep, and she fell 12 blocks down it into the rock (40 UNSTUCK lines). The hole is 128 u now (`pcb_collide_hole_depth`) and a Chell stuck behind an open portal's plane over its opening, or behind it for 4 ticks, is put back in front of it (`pcb_unstick_portal`); `pcb_assert_behind_portal`. Gate `floorloop2` 30/30 (old behaviour: 464 u sink, `shots/floorloop2c`). The cause in the user's session (bridge #13): no skipped transit; the exit put her hull 44 u off the floor portal's centre against the block rim (Portal 2's exit check does not see blocks); `pcb_exit_recentre` (default on) moves such a hull onto the exit's axis, Portal 2's own rule (gate `floorfloor1` written, not run). Depth itself is fine: pockets every 8 blocks y 90 -> -60 and natural caves at y -50..-30 (`deep1`), a 144-block shaft fall and a staircase walk across vertical shifts with collision on (`deep2`) | `puppet.cpp`, `blockcollide.cpp`, `tools/gen_deep.py`, `cfg/floorloop2.txt` |
| endless terrain (floating origin) | **on by default (bridge #10, promoted d880bb01); gate `float4` 333/333** (`shots/float4c`, written by `tools/gen_float.py`). Past 200 blocks from the region box's centre horizontally (`pc_mc_float [0/1] [blocks=200] [margin_v=16]`), or within 16 blocks of its top or bottom, the mapping moves by whole chunks (sections vertically) and, in the same tick, everything of Portal 2's in the region moves by the same Source offset: Chell through point_teleport's TeleportToCurrentPos (the client snaps in the next frame; SnapPlayer made it swing over ~12 frames, float2's 4.5 px twitch), her velocity kept; the portals (NewLocation); physics props (TeleportToCurrentPos: the old Teleport input used the origin saved in Activate(), so every moved prop went to 0 0 0, float2's lost cube); the composite's streams, light probes, collider pool, push-out pins, pending gun placements, the cliff exit. **Portals and props a shift would carry out of the box are kept in Minecraft coordinates**: the portal is closed (Fizzle) and placed again (same entity: colour and linkage; SetActivatedState + NewLocation) when its spot is back inside the box; a prop is frozen at a hold spot under the box and put back (EnableMotion). Chell's gun firing a parked colour drops its record. Back in the chamber with nothing open in the region, the mapping returns to its anchor (x, y and z). Gate: two test-command shifts watching the pair (twitch 0.00 px, light step 0.000, kept 0.00 blocks); a ~40-block continuous flight across one shift boundary per direction (+x, -x, +z, -z, up, down: `pcb_float_near_edge`, every frame watched, the 10 frames after each shift clean: `pcb_assert_watch_shifts`); 1520 blocks out and back on each horizontal axis and up past y 330 / down to y -50 by teleport steps (`pcb_float_step_to`: box-safe, each through the shift logic; after every step: no white or region-background frame, Chell inside the box, the pair and cube intact open or parked: 51 steps); the pair placed again works for transit after 1500 blocks away and back. Earlier gate `float3` (continuous 1522-block legs at 800 u/s): 0 failing of 4474 / 4526 frames, 0 ticks outside the box. Known: a moving Chell's view holds for one frame at a shift (Portal 2 resets its interpolation for a teleport); a parked prop's hold spot (Minecraft y ~46 under the box, 375 blocks off) could be seen from far away in rare views; deep caves below Minecraft y 48 had no collision (`minSectionY` 3): since bridge #11 those sections ship their collision bitsets without a mesh (`WorldExporter`; assert `pcb_assert_feet_mcy`, test `tools/cfg/cave1.txt`: 4/4, `shots/cave1`) | `floatorigin.cpp`, `composite.cpp` (CompositeShift), `clip.cpp` (ClipShift), `propcollide.cpp`, `tools/gen_float.py`, `tools/cfg/float3.txt`, `float4.txt` |
| terrain meshes in the region | off by default everywhere (debug only: `pcb_terrain_meshes 1`); solids still stream for collision | `render.cpp` |

## Build and load

```sh
cd bridge
make            # -> build/dev/portalcraft_bridge.so (test runs load this)
make stable     # -> build/portalcraft_bridge.so (refuses while Portal 2 runs)
```

Only `g++ -m32` is needed: no SDK, no libraries beyond libdl/libpthread. In the
game console:

```
plugin_load /path/to/portalcraft/bridge/build/portalcraft_bridge.so
sv_cheats 1
pcb_block_here            # a grass block on the floor in front of you
```

Absolute paths work, so the plugin stays outside the game folder. It writes
`cache/bridge/bridge.log`. The repo root is found by walking up from the `.so`
to the `.portalcraft-root` marker file (or set `PORTALCRAFT_ROOT`).

### Playing the clip (user session)

```sh
tools/play_clip.sh            # dev build; --stable for build/portalcraft_bridge.so
```

Starts (or reuses) the hidden terrain Minecraft, launches Portal 2 through Steam with a temporary
autoexec (`plugin_load` + `map pc_pit_flings`) that it removes once the level is in, and prints
READY only when the log shows `link up` and the pinned cliff exit Minecraft reported for its world (bridge #81). Run it
again while Portal 2 is up (attach mode) to check or restart Minecraft; `--restart-mc` restarts a
Minecraft that play_clip.sh started. A frozen, wrong-world or foreign Minecraft is reported, never
killed; Portal 2 is never touched. Bounded waits throughout (exit codes: 2 refused, 3 Minecraft
problem, 5 Minecraft start, 6 Portal 2 start, 7 link/exit not confirmed).

### Scripted test runs

```sh
tools/run_game.sh tools/cfg/final.txt sp_a2_bridge_intro 90 final
```

The script writes a temporary `portal2/cfg/autoexec.cfg` containing
`plugin_load <dev build>; pcb_script <script> <watchdog>; map <map>`, and
deletes it as soon as the plugin logs that it has loaded. It refuses to run if
Portal 2 is already running (the user may be playing) or if an autoexec
already exists. It never kills the game.

The plugin runs the script on the first server tick after the map loads. Each
line is a console command; `wait N` means N/60 s of wall clock. Every script
ends with `quit`. A watchdog SIGTERMs the game (a clean shutdown) after the
given number of seconds. Screenshots named `pcb_*` are moved to
`cache/bridge/shots/<run>/` and converted to PNG, next to that run's log. While
a script runs, player input changes are logged, which shows whether anyone
touched the controls.

Portal 2 does not exec `listenserver.cfg` in singleplayer, and `wait` in
autoexec expires while the map loads. The plugin-driven script replaces both.

### Commands

| command | what |
|---|---|
| `pcb_block x y z [kind]` / `pcb_block_here [dist] [kind]` / `pcb_clear` | add a block (min corner, Source units; kind 0 grass, 1 dirt, 2 planks) / remove all |
| `pcb_solid_block x y z [kind]` | block + invisible SOLID_BBOX collider (props land on it) |
| `pcb_collide 0/1 [dbg_hits] [dbg_traces]` | Chell-vs-blocks movement-trace hook (on by default) |
| `pcb_portal_blocks n x y z yaw [pitch]` | shoot portal n at the blocks: aim, fit, floor snap, place |
| `pcb_portal_check` | read the prop_portals back, compare with the request (PASS/FAIL) |
| `pcb_portal n x y z p y r`, `pcb_portal_wall n x y z yaw`, `pcb_portal_on_block`, `pcb_view_portal n [d]` | raw placement helpers and a camera pose |
| `pcb_puppet x y z [r] [period]` / `pcb_puppet off` | drive the player around a circle |
| `pcb_trace_grid [radius_blocks] [cells_per_block]` | stage-A CollisionField sample, timed |
| `pcb_status`, `pcb_probe x y z`, `pcb_probe_down x y z`, `pcb_where`, `pcb_views n` | state, lighting, contents, player origin, view log |
| `pcb_light scale [gamma] [floor]`, `pcb_mat n [debug]` | lighting calibration (default 5 / 1 / 0), material variants |
| `pcb_spawn_cube x y z`, `pcb_script file [s]`, `pcb_log text` | test helpers |

## Hook points and interfaces

Everything below was verified against this install (build 10090 per SAR's
tables). The game runs the OpenGL path: `shaderapidx9.so` over `libtogl.so`,
window title "PORTAL 2 - OpenGL", not DXVK or Vulkan. The plugin only talks to
the material system (IMaterialSystem, IMatRenderContext, IMesh), so it does
not depend on the backend.

| what | where | how it was found |
|---|---|---|
| scene draw, every view | `CRendering3dView::DrawTranslucentRenderables(inSkybox, shadowDepth)` in client.so (+0x727b80), inline JMP hook | SAR's Linux signature (SAR's OverlayRender uses it) |
| materials / texture / mesh | `VMaterialSystem080`: CreateMaterial 81, CreateProceduralTexture 91, GetRenderContext 113; dynamic mesh via SAR's MeshBuilder | SAR |
| lighting at a point for a normal | `VEngineClient015` slot 69 (ComputeLighting: leaf ambient cube + local lights, as models are lit) | slot 1 (GetLightForPoint, SAR) forwards to slot 69 in engine.so's disassembly |
| console commands | `VEngineClient015` ExecuteClientCmd 104. ClientCmd (7) silently drops anything without FCVAR_CLIENTCMD_CAN_EXECUTE, e.g. setpos | SAR offsets, tested |
| traces | `EngineTraceServer004` / `EngineTraceClient004`: TraceRay 5, PointOutsideWorld 19, GetPointContents 0 | SAR |
| player position / velocity | `GameMovement001` (server.so and client.so) slot 2 ProcessMovement: write CMoveData origin/velocity after the game's move | SAR |
| view angles | `VEngineClient015` SetViewAngles 19 | SAR |
| Chell vs blocks | `GameMovement001` slot 14 TracePlayerBBox (slot 13 SetupMovementBounds, 15 PlayerSolidMask), server and client | the decomp agent's vtable listing (5-argument trace override; slot 15 returns MASK_PLAYERSOLID) |
| entities | `VSERVERTOOLS001`: FirstEntity 8, NextEntity 9, GetKeyValue 11, SetKeyValue 12, CreateEntityByName 15, DispatchSpawn 16 | SAR (12/15/16); 8/9/11 follow from the public IServerTools order around SAR's slots |

### Rendering

Blocks are drawn at the start of the translucent pass of every 3D view: the
main view, every recursive portal view, and water and monitor views. The
skybox pass and shadow-depth passes are skipped. Opaque world and props are
already in the depth buffer at that point, so the map occludes blocks;
translucent surfaces drawn afterwards (glass, portal overlays, particles)
blend over them. Portal views clip them correctly with Portal 2's own oblique
clip plane.

- Material: `UnlitGeneric`, `$basetexture` = a 64×16 point-sampled procedural
  atlas (grass top tinted #91BD59, grass side, dirt, oak planks) built from the
  Minecraft PNGs, `$vertexcolor`.
- Lighting: per face, ComputeLighting at the face centre for the face normal,
  × Minecraft's face shade (top 1.0, N/S 0.8, E/W 0.6, bottom 0.5), × 5.
  Portal 2 treats the vertex colour as linear and applies its own output
  gamma/tonemap. A gamma-encoded version looked flat and too bright (the
  user's first report). `VertexLitGeneric` renders black without a lighting
  state (`mats/grid.png`).
- Blocks are drawn into the HDR scene before post-processing, so Portal 2's
  colour correction and bloom apply. Tonemap-scale readback is not wired: the
  ×5 is a by-eye calibration.

## World limits (investigated)

Run `void`, blocks at x 1200 behind the east wall and at x 17000:

- **Inside the chamber**, blocks behind a wall are correctly hidden by depth
  (`pcb_v1`).
- **Camera outside the map** (noclip into the void): the engine draws no
  world at all (the frame clears to white). Our blocks still draw, because the
  plugin does no leaf culling, but black: ComputeLighting there returns about
  0.011 against 0.045 in the chamber (`pcb_v2`).
  `PointOutsideWorld` = 1 and contents = SOLID for every void point.
- **A portal whose far side is in the void**: the view through it is solid
  black, even though our hook ran for that view (`pcb_v3`). Portal 2 does not
  render portal views from solid space.
- **Beyond ±16384**: `setpos 16900 0 0` is accepted, and the world vanishes
  as it does in the void (`pcb_v4`). Portals, traces and physics are
  unusable there.

So everything Minecraft must stay inside the empty space of a compiled map.
Under the lead's current scope (the world is Portal 2's chambers plus placed
blocks, like SkyCraft's void mirror world), that holds with no custom map. If
a generated Minecraft landscape comes back later, it needs a large hollow box
attached to a test chamber, compiled with the game's own
`bin/linux32/vbsp_linux` / `vvis_linux` / `vrad_linux` (present in the
install; vvis gives the box its own visleafs, vrad a base ambient). Minecraft
terrain would be recentred around the player, and all content kept well
inside ±16384 u, which is ±409 blocks at 40 u.

## Phase 1 plan

1. **Link on Linux.** Port `protocol/skycraft_protocol.h` unchanged in layout:
   POSIX shared memory (`shm_open("/SkyCraft_v1")` + `mmap` in the plugin;
   `FileChannel.map` of `/dev/shm/Portalcraft_v1` in Java, replacing SkyLink's
   `OpenFileMappingW`). Keep the seqlock slots, byte rings and heartbeats. Use
   polling or a futex word instead of Win32 named events, and
   `CLOCK_MONOTONIC` ms for `GetTickCount64`. Add a layout test on both
   sides.
2. **Minecraft.** Build SkyCraft's Fabric mod with its gradle wrapper (javac
   25 is now installed). Run it via Loom `runClient` with an offline dev
   account, hidden, and point it at the Linux mapping.
3. **Scale.** 40 Source units per block. Portal 2's 72 u standing player is
   then 1.8 blocks, Minecraft's player height. MC (x, y, z) → Source
   (x·40, −z·40, y·40), since MC +Z is south and Source +Y is north
   (right-handed on both sides).
4. **Meshes.** Consume `kRenAtlas` into the procedural atlas texture, and
   `kRenSection` into one static mesh per 16³ section (IMatRenderContext
   static meshes instead of the per-frame dynamic mesh used now). Split into
   opaque, cutout and translucent batches. `kRenLights` / `kRenSolids`
   feed lighting and collision. Relight with SkyCraft's WorldRender recipe:
   host ambient cube by normal (ComputeLighting), sky light
   `l/(4−3l)` darkening under roofs, block light max'd in warm, all in the HDR
   scene.
5. **Player (Minecraft mode).** PlayerState → ProcessMovement write-back and
   SetViewAngles (both proven in c). Camera from Minecraft's view matrix,
   including bob, with the FOV converted. `kRenAvatar` is drawn only in portal
   views, so you see yourself through
   portals.
6. **CollisionField.** Stage-A grid of engine traces around the player
   (2.25 µs/ray, so a 48-block radius at 1/8 block is amortised over frames).
   Portal 2 dynamic solids (doors, cubes, platforms) come from entity traces.
   The wall behind an open portal is carved out, using the same hole volume as
   below.
7. **Portals.** One shared prop_portal pair. Placement is ours on blocks (the
   fitting above) and Portal 2's on its own surfaces. Move the portals with
   `portal_place` now, and call the entity's NewLocation input directly later.
   Minecraft-side transit follows Portal 2's own transit rules.
8. **Hand and HUD.** CPU readback from Minecraft's offscreen layers into a
   procedural texture drawn after the 3D views. GL↔Vulkan interop is a later
   optimisation.

## Modes (Minecraft player ⇄ Chell)

`bridge_mode mc|chell|toggle`, bound to one key.

- **Handoff state:** position (feet), view angles, velocity and crouch state
  are copied across in both directions in the same tick. That covers
  mid-fling: velocity survives because both sides write it (ProcessMovement
  write-back into Portal 2, a PlayerState-style message into Minecraft).
- **Minecraft mode:** Minecraft is authoritative. Portal 2's player is a
  puppet (its movement input is swallowed and its origin written each tick),
  so triggers, buttons and cube pickups still see a body. The camera follows
  Minecraft. Portal 2's viewmodel and HUD are hidden; Minecraft's hand and
  HUD overlay are shown.
- **Chell mode:** puppeting stops and Portal 2's movement, gun, viewmodel and
  HUD run untouched. Minecraft's player is frozen at the handoff point (its
  client paused like SkyCraft's "Skyrim menu open" state), and its camera no
  longer drives Portal 2. The hand and HUD overlay are hidden. Blocks stay
  drawn and solid (the trace hook is mode-independent).
- **Shared:** blocks, portals, held objects and Portal 2 entity state live in
  one world. A held cube stays held across the switch: Portal 2's own pickup
  in Chell mode, puppet-driven pickup in Minecraft mode.
- **Risks:** prediction error on the frame of the switch (client prediction
  vs server); Minecraft's server rejecting the teleport-back as "moved too
  quickly" (use its own teleport path); a stuck state if the handoff position
  is inside a block for the other side's hull (Minecraft 0.6×1.8 blocks vs
  Portal 2 32×72 u, 0.8×1.8 blocks; resolve with a FindClosestPassableSpace
  step).

## Interop matrix

MC = Minecraft player. "Phase 0" says what this phase proved for the row.

| capability | MC | Chell | mechanism | phase | Phase 0 |
|---|---|---|---|---|---|
| portals: one shared pair, placed by either | portal item → plugin | real gun | move prop_portal (portal_place, then NewLocation) | 1 | move + read-back proven |
| portals on Portal 2 surfaces | ours, with P2 validation | P2 gun | placement spec §3–4 | 1 | – |
| portals on blocks | ours | hook the gun's placement trace so blocks are hit and validated | spec §3–4 + block fit | MC 1, Chell 2 | fit + floor snap on blocks proven |
| transit: players | MC teleport + view/velocity rotate | P2 native | portal-transit.md | 1 | Chell transit through a block-wall portal proven |
| transit: P2 cubes, MC mobs, items, arrows | – | – | P2 native for cubes; MC entities teleported by the mod | 2 | – |
| players collide with blocks | MC native | TracePlayerBBox hook | kRenSolids → AABBs | 1 | Chell proven |
| P2 props collide with / rest on blocks | – | – | fallback proven: SOLID_BBOX collider entity per block; target: per-section static collider or SkyCraft-style push-out | 1–2 | collider fallback proven |
| hole behind a portal on a block wall | CollisionField carve | carve in the trace hook | hole volume (spec §4) | 1 | Chell proven |
| MC collides with P2 world + dynamic solids | CollisionField (traces) | – | stage A traces | 1 | traces proven |
| buttons (floor, pedestal) | puppet body occupies space; use arbitration | native | puppet + nearest-target use | 1 | puppet proven |
| pick up P2 cubes | use arbitration → P2 pickup via puppet view | native | object-pickup.md | 2 | – |
| fizzlers fizzle MC portals / held cubes | yes | native | trigger overlap on the puppet | 2 | – |
| lasers, turrets, goo damage | forwarded as MC damage | native | HitBridge-style events, test-elements.md numbers | 2 | – |
| faith plates, gels | forwarded launch / bounce / speed | native | test-elements.md | 2 | – |
| blocks press floor buttons | – | – | block AABB vs button trigger | 2 | – |
| breaking a portal's surface fizzles it | – | – | on BlockChange, re-validate portals | 2 | – |
| TNT pushes P2 cubes | – | – | explosion → vphysics impulse | 3 | – |
| mode switch keeps position / view / velocity | both | both | handoff above | 1 | – |

## Risks

- **Prop physics on blocks.** Proven only with one entity per block. The
  edict limit (2048) and its cost rule that out for terrain. Candidates:
  merged per-section colliders via a physics collide model on one entity
  (needs IPhysicsCollision), or SkyCraft's push-out (NpcBlocks.cpp) applied
  to props each tick, damping velocity into blocks.
- **Chell's portal gun on blocks** needs the placement trace hooked; the
  function isn't identified yet (portal-placement.md names the steps).
- **Hole rule** uses the portals the bridge placed. It must read the live
  prop_portal pair, so that portals placed by Chell's gun count too.
- **Offsets** are build-specific. SAR signatures cover most of them. Slots
  69 (ComputeLighting) and 14 (TracePlayerBBox) are ours and must be
  rechecked after any game update.
- **Performance:** one dynamic mesh per block per view is fine for tens of
  blocks. Terrain needs static section meshes, and every portal view redraws.
- **Test hygiene:** the user is at the machine. `run_game.sh` refuses to start
  over a running game, and scripts log input.

## Known gaps

- Only the standing hull (32×72) is swept against blocks. Crouched Chell is
  treated as standing, so she won't fit under 1-block gaps.
- For wall portals the collision hole is carved 1 u below the opening's
  bottom edge instead of the 0.98 × opening of the spec, so a floor-snapped
  portal has no lip.
- Fitted portal centres can sit about 0.1 u off the aim line (seen: y
  309.88 for aim 310). The fit is a grid search, not Portal 2's edge-push
  loop.
- Lighting calibration is by eye (×5). No tonemap readback, and no projected
  texture or shadowing.
- Block textures are a fixed 4-tile atlas until `kRenAtlas` streams.
