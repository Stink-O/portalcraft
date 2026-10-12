# Playing as the Minecraft player inside Portal 2 (proposal; M1 switch in progress, bridge #13)

## Decisions (2026-10-02, the user through the lead; reversible)

- **Scope: a plain, full Minecraft player.** HUD/GUI, the hand and held items, the body/character, inventory,
  placing and breaking blocks, the normal Minecraft things. **No Minecraft portal-gun item** (the user: "no minecraft
  portal gun, its ok, just the hud and the hand model and character and everything minecraft"). Portals are fired as
  Chell: switch with J. Everything about `portalcraft:portal_gun` / `kEvFirePortal` below is dropped.
- **SkyCraft parity (the user: "get the minecraft character working like how skycraft works").** The hand, held item
  and the full GUI come from Minecraft's own rendering: **option (a) of section 1** (SkyCraft's overlay: one image of
  hand + GUI, pixel-exact Minecraft, glint and every screen), not a re-drawn hand mesh. The body is the real Minecraft
  player model; input and inventory behave as in SkyCraft. (a)'s blocker is the ~5-6 ms per-image `Download`, so
  **M2 starts with a direct-GL upload path**: a PBO upload into the plugin's own GL texture, drawn at
  `SDL_GL_SwapWindow` with togl's GL state saved and restored (the composite can use it later: phase C). Then the
  overlay on it. Option (d) (hand mesh) is only the fallback if direct GL cannot be made to work. This supersedes
  section 1's recommendation of (d).
- **HUD option: overscan 1.0 in Minecraft mode** (crisp GUI: Minecraft's window equals the screen, so the GUI is not
  scaled; the composite's overscan margin is given up in that mode and phase B / reprojection covers the edges).
- **M1 as the lead scoped it (bridge #13):** a working J switch in pc_pit_flings (terrain mode): Minecraft's player
  teleported to Chell's feet (chamber band or region) with her velocity; the mod's hold released on Minecraft's own
  terrain without SkyCollision; the chamber band's Portal 2 collision exported in terrain mode (outside the region box,
  portal holes carved); the camera from Minecraft's eye; puppet and input engaged; Chell's model, the viewmodel and the
  crosshair hidden; portal transits of the body handed to Minecraft (teleport + velocity); switching back and forth
  keeps position, view and velocity, mid-air and mid-fling included. The hand, GUI, body and block placing are M2+ for
  a fresh agent. The chamber band is above the 320 build limit: the Minecraft player stands there on SkyCollision's
  shapes (BlockCollisionsMixin adds them at any height, build limit or not), so the datapack height raise is needed
  only once blocks are placed in the band (M2+).

**Goal.** In Minecraft mode (J / `bridge_mode mc`) the user is a full Minecraft player: Minecraft movement and physics, hand and held item, hotbar/HUD and inventory screens, placing and breaking blocks, combat, items. Portal 2 is still the renderer and the world. Built on: the puppet + CameraDriver + InputBridge (`mc.cpp`, `puppet.cpp`), CollisionField (`collfield.cpp`), the one-layer composite (`composite.cpp`, DESIGN-composite / -mcportals), the section mesh path (`terrain.cpp`) and mclight.

**Facts this design rests on (read in the code, 2026-10-01):**
- The mod already captures Minecraft geometry generically through a `SubmitNodeCollector` (`AvatarExporter`): the third-person body (kRenAvatar), every other entity, block entity and particle (kRenScene), and arrows, items, cracks and the block outline (WorldEntities). The mod still sends all of this every frame; the plugin ignores it.
- The world-view capture runs at `render3dHud` (`GameRendererViewMixin`). Minecraft then draws its hand and GUI into the same target. When no host view is active, SkyCraft's `FrameExporter` ships the main target (hand + GUI on a transparent background).
- Every image costs Portal 2's render thread **~5-6 ms in `ITexture::Download` whatever its size** (README, performance row). A per-frame image layer through the material system therefore breaks the 180 fps budget.
- In `pc_pit_flings` the map is rigid. The region floor is MC y 40, so the chamber (BSP verts z -4592..768, x -2560..832, y -2048..1920) maps to MC x -392..-307, z 376..472, **y 328..463**. That is directly above the cliff column and **above the 320 build limit**. Placing a block there fails in vanilla.
- Terrain mode exports **no** Portal 2 collision (`exportRegions = !g_terrainMode`), so in pc_pit_flings today a Minecraft-mode player in the chamber has no floor.
- Minecraft's window is the screen × overscan (1760×990 for 1600×900). A GUI drawn at that size and shown at 1600×900 is scaled by 0.91 and blurs.
- SkyCraft's shipped code uses a **dedicated key (G)** for Skyrim's activate. The "nearest target wins" rule appears only in its DESIGN.md.
- SkyCraft draws its overlay in a D3D11 Present hook with its own shaders. We have no shader of our own: only the material system, or raw GL under togl.

## 1. First-person hand, held item, HUD and GUI

| option | pros | cons / risks | cost |
|---|---|---|---|
| (a) SkyCraft overlay: one image of hand + GUI, read back and drawn over the frame | pixel-exact Minecraft: glint, maps, PiP player model, every screen; one mechanism; proven in SkyCraft | needs an image every Minecraft frame (the hand bobs and swings) = 5-6 ms per Download, so it is **blocked on a direct-GL upload path** (phase C); the hand is lit by Minecraft's light at the hidden player, which in the chamber band is full sky light (bright, or dark at night); it is drawn after post-processing, so it misses Portal 2's tonemap; the GUI is tied to the overscanned window | mod 0.5 d; plugin 0.5 d on the material path, plus ~2 d for direct GL |
| (b) hand inside the composite stream, with depth | no extra image where the region is the background | no stream exists in chambers, so Minecraft would have to render a full-screen level every frame just for the hand (6-7 ms of Minecraft time, plus an upload); reprojection drags a screen-locked hand; drawn at the background depth, so Portal 2 portals and props draw over it; tonemapped twice | high; reject |
| (c) GUI-only overlay plus the hand via the composite | the GUI changes less often than the hand | (b)'s problems for the hand | reject |
| **(d) GUI-only overlay plus the hand as a Portal 2-drawn viewmodel mesh** (new) | no texture traffic for the hand: ~1-3k vertices per frame over the render ring (dynamic mesh, main thread); locked to the view (camera-relative, so no reprojection); lit like Portal 2's gun (ComputeLighting at the eye in chambers, mclight's values in the region) and tonemapped with the scene; works in chambers and in the region alike; the GUI uploads only when it changes | MC's swing/equip/eat poses arrive one Minecraft frame late (≤ 8 ms; the motion is screen-space, so it is not visible as drift); enchant glint and map items need their own handling (v1: no glint; maps as their dynamic texture); the hand is drawn in our translucent hook with a near depth range (Portal 2's motion blur may touch it; hook DrawViewModels later for exact parity) | mod ~1 d (capture `ItemInHandRenderer` through the existing collector as a new kRenHand, view-space); plugin ~1 d |

**Recommendation: (d).** *(Superseded 2026-10-02 by the user's SkyCraft-parity direction: (a) on a direct-GL path; (d) only as the fallback. See Decisions.)*
- **Hand:** a mesh, drawn in the main view only (depth 0), as Portal 2 does its viewmodel (`player-in-portals.md` §1), with `DepthRange(0, 0.05)` and depth write on, so later translucents stay behind it. Hidden in third person (F5), where Minecraft sends no hand.
- **GUI:** one image, captured by clearing colour to transparent after the level capture, so the hand never lands in it (the mod skips the hand pass in Minecraft mode). It is published to a new HUD triple buffer **only when its hash changes**, and drawn in the existing `VEngineVGui::Paint` hook *before* Portal 2's own paint, so the console and menus stay on top. It is hidden while Portal 2 is paused or the console is open (SkyCraft's rule).
- **v1 transport:** Download on change, measured. Hotbar, health and XP change a few times a second, so a 5 ms hitch stays under the 12 ms worst frame. The attack-cooldown meter (crosshair), heart blink and hunger shake change every tick. Mitigations, in order: draw MC's crosshair and attack indicator ourselves as a mesh (SkyCraft also gives them a pass of their own); throttle the GUI to ≤ 10 Hz outside screens; direct GL (phase C).
- **Phase C** (shared with the composite): a PBO upload into our own GL texture, drawn at `SDL_GL_SwapWindow` with togl's GL state saved and restored. That brings the GUI to full rate (~0.3 ms for 1600×900, less with dirty rects). EXT_memory_object_fd zero-copy needs a Vulkan exporter in one process, so it is a later probe.

**Rest of the hand and GUI design:**
- **Lighting:** the hand mesh carries Minecraft's vertex colour/AO only. The plugin multiplies it by Portal 2's light at the eye (× the gun-calibrated scale), or by McLight's eye sample inside the region (mclight already computes it).
- **Window size and scaling:** in Minecraft mode, the window is the screen exactly (the GUI scale and cursor are then 1:1). The composite's overscan then needs the level pass to render into its own larger target (mixin on the level target's size), or overscan 1.0 in Minecraft mode, relying on phase B. **Decided (lead, 2026-10-02): overscan 1.0 in Minecraft mode.**
- **Mouse cursor in screens:** with `kMcScreenOpen` set, the plugin freezes Portal 2's view (restores the angles in CreateMove and OverrideView), turns the angle delta back into mouse counts (Δyaw / (m_yaw × sensitivity)), moves its own cursor clamped to the screen, sends `kInCursor` and draws the arrow itself (SkyCraft's shader cursor, as a quad). The wheel and text come from the key hook (§4).
- **Hiding Portal 2's HUD and viewmodel:** `r_drawviewmodel 0` (as now). For the crosshair and portal indicators, set HIDEHUD_CROSSHAIR in the player's `m_Local.m_iHideHUD` through the server datamap; no archived cvar changes, so config.cfg is never touched. Restore it on Chell mode, level shutdown and plugin unload. Portal 2's damage flash stays off because Portal 2 never takes the damage (§5). Captions stay.
- **Latency:** the GUI is ≤ 1 Minecraft frame plus 1 host frame behind (hotbar scroll ≈ 10-15 ms), the same as SkyCraft.

## 2. The player's 3D body (third person, seen through portals)

| option | pros | cons |
|---|---|---|
| (a) kRenAvatar mesh drawn by Portal 2 (Portal 2 lighting) | already exported; follows Portal 2's draw rules (portal views, ghosts at the opening); correct in chambers | the skin and armour need kRenTexture uploads (one-time Downloads); in the region, the mesh must be lit with mclight and occluded with the occlusion layers |
| (b) Minecraft renders its own player into the composite | exact Minecraft lighting and occlusion in the region; free | only where Minecraft is the background; host-view and portal passes must render the local player (`Camera.isDetached` for entity rendering in host passes) |
| **(c) hybrid by location** | each side draws where it is native | a body straddling a region portal is drawn twice, each clipped by its side's plane, which is what Portal 2's ghosts do |

**Recommendation:** (a) first, then (c). The mod sends kRenAvatar in first person too while the host asks (a HostPlayer flag). The plugin draws it in portal views (depth ≥ 1), never in the main view in first person (`player-in-portals.md` §2), and in the main view in third person. Chell's model is hidden in Minecraft mode (EF_NODRAW on the puppet's client entity, so her shadow goes too). With the feet in the region, Minecraft draws the player (mixin in the host/portal passes) and the plugin draws nothing there.

## 3. Blocks placed inside Portal 2 chambers

| option | pros | cons |
|---|---|---|
| **(a) Portal 2-lit section meshes** (terrain.cpp + SkyCraft's WorldRender recipe), Chell via the block hook, props via the collider pool | zero latency, locked to the world; Portal 2 depth, portals and recursion for free (Phase 0, verified); Chell, props and the gun on blocks already work (blockcollide, propcollide, portalgun); small memory (chamber builds) | Portal 2 lighting approximates Minecraft's (no MC AO lightmap; block light needs our own term); a section re-meshes on every change (render-thread static mesh rebuild) |
| (b) a chamber-mirror world drawn by Minecraft and composited with depth | Minecraft's exact look | per-pixel depth against Portal 2 needs a shader we don't have (255 alpha-test layers full screen ≈ 3-4 ms of fill); 2-3 frames of latency, so nearby blocks swim while strafing (today's 0.7-12 px drift); K cameras for portal views; lighting mismatch with the chamber |
| (c) mixed (meshes in chambers, composite in the region) | what we have today | none beyond (a) |

**Recommendation:** (c), meaning (a) for everything in the chamber band and the composite for the region.

**Mesh path:** meshes for the chamber band only (terrain meshes stay off), lit by ComputeLighting by face normal × Minecraft face shade × the calibration, max'd with Minecraft's block light (warm, `l/(4-3l)` as in WorldRender). Translucent water and glass go in a later batch; cutout uses alpha test.

**Interactions:**
- **Placing on Portal 2 surfaces:** CollisionField shapes aren't blocks, so Minecraft's crosshair never hits them. Port SkyClip to the voxel field: a DDA through the 1/8-block ColBlocks gives the hit face and the placement cell. Exact triangles (kColTris from the map's BSP, read at level load) come later, for projectiles.
- **Breaking:** Minecraft's own rules. The plugin draws cracks and the outline from WorldEntities (kWeCrack, the selection box).
- **Block entities and pistons:** chests, beds, signs and moving pistons come from kRenScene (the existing capture), drawn as dynamic meshes; doors, redstone and rails are ordinary section geometry (re-meshed on change); redstone logic is Minecraft's own.
- **Water and lava on Portal 2 floors:** FlowingFluidMixin (from SkyCraft, already in the mod) needs triangles. It needs a voxel variant (a fluid rests on the top of a CollisionField cell). Chell already swims in it (fluids.cpp).
- **Portals on placed blocks:** already work (portalgun.cpp).
- **Breaking a portal's surface:** re-validate the pair on the section update, then Fizzle.
- **Blocks pressing floor buttons:** a block AABB against the button's trigger, phase 3.

## 4. Input routing and arbitration

- **Swallowing Portal 2's binds:** hook `IBaseClientDLL::IN_KeyEvent` (VClient016; slot to pin from the vtable); returning 0 drops a bind's command. Portal 2 keeps J (mode), ` (console), Esc (its menu, or closing an MC screen) and F6/F9 (save/load); everything else goes to Minecraft. E is Minecraft's inventory; Portal 2's +use moves to the arbitration below. The same hook supplies MOUSE_WHEEL (`kInScroll`) and key-to-char for `kInText` (chat, signs). Movement buttons stay cleared in CreateMove, as now.
- **Use arbitration (right click):** Minecraft reports its crosshair target in McState (block or entity, and its distance; WorldEntities already carries the block box). The plugin finds Portal 2's use target (a use trace, or the held cube). Nearest wins: a Portal 2 target turns the click into one tick of IN_USE on the puppet, and the click is not forwarded. While Portal 2 holds a cube, right click only drops it. Fallback if the user finds this ambiguous: G as an explicit "Portal 2 use" key, like SkyCraft's shipped code.
- ~~The Minecraft portal-gun item~~ **dropped (the user, 2026-10-02):** portals are fired as Chell (J).

## 5. Combat and damage, both ways

- **Minecraft hits Portal 2:** turrets, cubes and props within 32 blocks go into SkyCraft's actor table, as Chell's proxy does now (`SkyrimActorEntity`): invisible, hittable, boxed by the entity's bounds. kEvHitActor → the entity's TakeDamage with DMG_CLUB and a force: vphysics applies the impulse, and a turret tips and dies by its own logic. Arrows and TNT go the same way (kEvExplosion is wired already).
- **Portal 2 hurts Minecraft:** hook the player's OnTakeDamage (server vtable; the decomp agent pins the slot). In Minecraft mode, cancel it and send `kInHurt` with Portal 2 damage / 5 (SkyCraft's ratio, 100 HP ↔ 20). New HurtKinds: laser burn (150/s, so ~30 MC/s), turret bullet, goo / lethal trigger_hurt (MC `/kill` semantics), crush. The laser push is forwarded as a velocity (below). Minecraft's health, armour and i-frames decide; MC fall damage applies (Portal 2 has none).
- **Death:** kEvPlayerDied lets lethal damage through to the puppet, so Portal 2's own death and checkpoint reload run. Gamerules `keepInventory` and `doImmediateRespawn`. The level-live teleport puts Minecraft at Chell's spawn. Blocks are not rolled back by Portal 2 saves (accepted; SkyCraft's saveId snapshots if ever needed).
- **Velocity setters (catapults, laser push, bounce gel, transit):** the server ProcessMovement hook sees the velocity at entry differ from what we wrote last tick. Transit and pushes send `kInVelocity` (Minecraft sets its delta, rotated by the portal for transit). **Catapults and bounce use a host takeover:** Portal 2 movement drives the puppet until it lands, Minecraft follows by `kInHostDrive`, then gets the landing velocity (SkyCraft's furniture takeover). Minecraft's gravity is 0.08 b/t² with 2% drag per tick against Portal 2's 600 u/s², so Minecraft physics would land a faith-plate arc short. Speed gel: a Minecraft speed modifier while the feet are on paint; this needs the paint colour at the feet (no trace field gives it; decomp work), so it comes last.

## 6. World mapping

| option | pros | cons |
|---|---|---|
| separate void dimension for chambers (SkyCraft's interiors) | clean void; any height | **the client holds one level**: in a chamber the terrain (needed for the composite through the pit portal) would be unloaded; a dimension change is a level reload, so it can't happen mid-fling |
| same world, chambers in a disjoint far range | void by a generator mixin | the region's chunks unload (they are > RD from the player), so the chamber→region views go blank |
| **same world, today's rigid mapping: chambers in a band above the build limit over the region** | the band is void by construction (no generator change); the cliff's chunks stay within RD (they share a column); one transform, so transit and composite need no change | needs the overworld `dimension_type` raised (datapack: min_y -64, height 576 → max 511) on an existing world; more sections per column for the section graph and lighting (+50%: watch the Minecraft frame, already the bottleneck) |

**Recommendation:** the band, with two RegionMaps in `coords.hpp`: the region map, which floats, and a chamber map anchored per map name. For pc_pit_flings the chamber map is the anchor (rigid, as today); a map without a region gets its own slot (band y ≥ 330, x = 4096 × slot, keyed by map name).

**Consequences:**
- **Persistence:** blocks are real Minecraft blocks in the saved world, so returning to a map finds them; a changelevel moves Minecraft to the new map's slot (`McLevelLive` teleport).
- **Floating origin:** off in Minecraft mode for v1 (the region box gives ±400 blocks). v2: on a shift, copy the band's non-air sections by the same chunk delta (server side, block entities included) and re-key the meshes.
- **Collision in terrain mode:** export collision for the chamber, i.e. everything outside the region box.
- **First action:** test the height change on a copy of `Portalcraft_20260930` (the world hash of the 13×13 cliff chunks must not change).

## 7. Performance and memory (32-bit, 180 fps)

- **Plugin per frame:** hand mesh ≤ 3k verts (~0.05 ms main thread); avatar and scene meshes only when visible; chamber sections as static meshes (render thread, ≤ 1 rebuild per frame, budgeted); GUI upload on change only (v1), then ~0.3 ms (phase C). Gate: ≤ 0.5 ms added mean, worst ≤ 12 ms.
- **Memory:** HUD triple buffer 3 × 2048×1152×4 = 28 MB, so the mapping goes 191 → 219 MB (address space ~2.2 → ~2.25 GB); GUI texture 9 MB; skins and the atlas (exists); chamber meshes < 10 MB for thousands of blocks; terrain meshes stay off.
- **Minecraft:** the hand pass is skipped; the GUI pass is unchanged; +50% sections per column from the height change. The level render is unavoidable while a host view is up; with no host view in Minecraft mode, cull the level to nothing (LevelRendererMixin) without dropping the compile requests.

## 8. Better or extra ideas

- Reuse the SkyCraft code already in the mod as-is: AvatarExporter (hand, body, scene), WorldEntities (cracks, items, arrows), SkyCombat and the actor table (Portal 2 entities as proxies), SkyClip (needs only voxels or triangles), FlowingFluidMixin, the tick interpolation (exists).
- Feeding SkyCraft's exact-triangle pipeline (kColTris from the map's BSP brushes at level load) re-enables TriCollider and SkyClip unchanged. That is better than traces for thin geometry.
- Rejected: capturing the GUI itself as a 2D mesh (sprites + glyph atlases) to avoid image uploads. Item icons and the inventory's player model are per-frame offscreen renders, so the capture surface is too large.

## Protocol v17 (the mod keeps accepting v11-v16 hosts)

| addition | where |
|---|---|
| `HostPlayer` (Host→MC, seqlock): mcMode, wantAvatarFirstPerson, wantHand, wantGui, cursor x/y, screen size, eye light RGB | 0x16800 |
| `HudCtl` + HUD triple buffer (w, h, flags, frameId, hash, dirty rect) | ctl 0x16900; pixels after the render ring (+28 MB) |
| McState spare (0xC8..0xFF): targetKind, targetDist, selectedSlot, heldItem, handVisible | 0x200 block |
| kRenHand = 12 (RenAvatar layout, view space); kRenAvatar also in first person on request | render ring |
| Input: kInVelocity = 9 (MC blocks/tick), kInHostDrive = 10 (start/stop + pos/vel); HurtKind laser/bullet/lethal/crush | input ring |
| Events: kEvRespawned = 7 (kEvFirePortal dropped with the portal-gun item) | event ring |
| ActorFlags: kActorP2Turret, kActorP2Prop (entity index + serial in formId) | actor table |

Layout test (`tools/layout_test.sh`) is extended; Proto.java is mirrored.

## Files touched

- **Plugin:** `mc.cpp` (mode, HideHUD, cursor, IN_KeyEvent, arbitration); new `hud.cpp` (GUI draw in Paint, later direct GL) and `hand.cpp` (hand / avatar / scene meshes); `terrain.cpp` (consume kRenHand/Avatar/Scene/Texture, band meshes on); `collfield.cpp` (chamber export in terrain mode); new `damage.cpp` (OnTakeDamage hook, MC hits → TakeDamage); `chellproxy.cpp` → actors; `puppet.cpp` (velocity setters, takeover); `coords.hpp`, `clip.cpp`, `floatorigin.cpp` (chamber map); `link.*`, the protocol header, `layout_test.sh`.
- **Mod:** SkyClient + new `HudExporter` (clear after level, hash, publish on change); AvatarExporter (hand capture); InputBridge (velocity, drive); SkyClip voxel variant; FlowingFluidMixin (voxel); SkyCombat (Portal 2 actors, hurt kinds); CameraHostViewMixin (local player in host passes); a datapack `dimension_type`; gamerules.

## Phased plan (each phase gated)

1. **M1, smallest playable:** in sp_a2_bridge_intro and pc_pit_flings, walk as the Minecraft player with the hand and hotbar visible, and place a block in a chamber. Needs: the chamber collision export, the dimension height (copy-world test first), the voxel SkyClip, band meshes, outline and cracks, kRenHand, the GUI on change, HideHUD, IN_KeyEvent.
2. **M2:** screens (cursor, wheel, text), use arbitration (buttons, cubes). (No portal-gun item: dropped.)
3. **M3:** the avatar in portal views, Chell hidden, Minecraft drawing the player in the region.
4. **M4:** combat and hazards both ways, death and respawn, catapult and laser-push takeover.
5. **M5:** direct-GL GUI (phase C); block entities, water and button polish; per-map slots; floating origin with band relocation; gels.

## Gates (in the style of the existing suites; `tools/gen_mcplayer.py` → `cfg/mcplayer1.txt`)

- **M1:** `bridge_mode mc`, `pcb_assert_mode mc`; walk 5 blocks with `pcb_mckey` (`pcb_assert_moved`) and land on the floor (`pcb_assert_on_ground`); place a block on a Portal 2 floor, wall and ceiling, on a placed block, and through a portal. Each is checked with `pcb_assert_mc_block` (the solids bitset at the expected Source cell) and a frame probe (block pixels at the projected centre); J back to Chell: she stands on it and is stopped by it, a cube rests on it, a portal lands on it (the existing asserts); `pcb_assert_hand` (hand pixels in the main view's lower right, none in portal views); `pcb_assert_gui` (hotbar pixels at bottom centre match Minecraft's own screenshot within 2 levels per channel; the window equals the screen, so no scaling); `pcb_assert_no_p2_hud` (no crosshair pixels); persistence (a `map` reload and a changelevel round trip keep the block); perf window (mean ≤ 5.6 ms, worst ≤ 12 ms, render thread +≤ 0.5 ms, VmSize ≤ 2.4 GB); every run checks the input log for live input.
- **M2:** inventory open, then the cursor moves an item between slots (an MC-side inventory assert); scroll changes the slot; a pedestal button pressed by right click; a cube picked up and dropped.
- **M3:** the avatar is visible through a portal (`pcb_assert_seen_through`), absent in the main view, Chell's model absent; the region case through the cliff portal.
- **M4:** a laser does 30 ± 5 MC damage/s; a turret hits; goo kills, then Portal 2 reloads and Minecraft respawns at Chell; a punched cube moves > 0.5 blocks; a hit turret tips; a faith-plate landing within 1 block of Chell's on the same plate (`pcb_track`).

## Risks (top three first)

1. **Image upload cost.** Download costs 5-6 ms per image. Option (d) keeps the hand off it, but the GUI's per-tick animations and screens need throttling until phase C. Direct GL under togl must restore togl's GL state exactly, or rendering corrupts.
2. **Build height in the chamber band.** It needs an overworld dimension_type override on an existing world. Unknown: chunk compatibility, and the section-graph cost on a Minecraft frame that is already 6-7 ms. Every alternative breaks the composite, because the client holds one level.
3. **Authority conflicts.** Portal 2 entities that set the player's velocity (catapults, laser push, gels, transit) against Minecraft's authoritative physics. MC gravity and drag differ from Portal 2's, so puzzles need the takeover. The detection is heuristic (an entry-velocity mismatch).
4. Also: the overscan vs GUI window size decision; unpinned slots (IN_KeyEvent, OnTakeDamage, TakeDamage), which are decomp work; paint at the feet, for gels.
