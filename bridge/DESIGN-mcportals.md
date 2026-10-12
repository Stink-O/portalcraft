# Minecraft renders its own portal views (approved 2026-10-01; built by bridge #7, see README)

**Decisions (lead and user):**
- We write our own minimal implementation.
- Immersive Portals and SeamlessPortals (cloned in `bridge/refs/`) are reference only: none of their code is copied or run.
- Minecraft 26.3 has no stencil (the main target is D32_FLOAT and pipelines have no stencil state). Each portal pass therefore renders into its own target, reused from frame to frame, and is composited through a depth-tested mesh of the opening. That mesh is Portal 2's ellipse at the portal's current open scale.
- Recursion: the nested pass is composited into the depth-1 pass's target the same way.
- The depth-in-alpha pass carries the virtual camera's depth inside each opening, so Portal 2 objects seen through a portal (Chell, cubes) are hidden correctly by the occlusion layers.
- Cloud state is per frame: rotate CloudRenderer's UBO for every extra pass.

**Problem (measured, mcworld runs `mw1s`/`mw1s2`).** A chamber→Minecraft view needs one Minecraft camera, the primary. That camera is rendered every frame: image age is 2.0-2.4 host frames on average, max 3, with 0 failing frames (`mcchamber_p1_*`). A Minecraft↔Minecraft pair needs the main view, a portal view per visible portal, and recursion views. All of those except the main view share the second slot. Before the fix that slot was throttled (every 3rd frame) and its request was dropped when Minecraft read the shared view on an off frame. Portal-view images were 4.5-46 frames old on average, with a max of 13-204 (the facing tunnel was worst), and 241-1585 frames per pair were stale.

The scheduler fix (each view has a period, the most overdue view goes first, and a request stays in the shared view until Minecraft answers it) cut that to 2.5 average and 5-6 max for one portal view. The facing tunnel's portal views still reach 9-10, and its recursion views 15-29. The second slot still can't keep up when several views share it, and each image costs a texture Download on the render thread.

**Proposal.** When both portals of the live pair are in the region, Minecraft draws the portal views into its own frame, the way Immersive Portals does. Portal 2 then composites ONE Minecraft image for the main view, with correct, synchronous portal contents.

1. **Protocol v15.** HostViewExt carries the live pair in Minecraft coordinates (centre, normal, up, 32×56 u half-extents → blocks) and a flag "pair inside Minecraft". The mod still accepts v11-v14 hosts.
2. **Mod**, in the primary camera pass after the level:
   - For each pair portal facing the camera and inside the frustum:
     1. Write the opening (Portal 2's ellipse, 24 segments) into the stencil, depth-tested against the level just drawn, so terrain in front hides it.
     2. Clear depth inside the stencil.
     3. Render the level again from the virtual camera (the main camera × the portal transform), with an oblique near plane on the partner's plane, stencil-equal.
     4. Recurse to depth 2, Portal 2's default; past that, draw the portal's flat colour, as Portal 2 does.
   - Sky and fog are drawn per pass.
   - The depth-in-alpha pass runs last over the whole frame, so occlusion layers stay valid.
   - Reuses the existing second-level-pass machinery (GameRendererViewMixin, per-pass camera uniforms, FrustumOffsetGuard, smartCull off).
3. **Plugin.**
   - For a pair inside Minecraft, no candidates/streams are made for portal or recursion views.
   - Portal 2's own portal views (stencilled by Portal 2) draw the main stream's image, screen-mapped like the main view (the `over` path), because those pixels already show the portal's contents.
   - Portal 2 keeps drawing its rim, edge particles and open/close animation on top. The occlusion layers keep the rim hidden by terrain.
   - Mixed pairs (chamber↔Minecraft) keep today's multi-stream path unchanged.

**Immersive Portals.** Apache-2.0 (reuse is allowed with attribution in NOTICE), but the repo was archived on 2026-04-21 and never targeted 26.x. Its renderer is large and threads through many mixins (world rendering, chunk loading per dimension, entity sync, teleport) that we don't need. A minimal implementation of our own is simpler: it covers one pair, one dimension, and terrain already loaded around the camera. We would read IP's stencil/oblique-clip approach for reference and copy no code.

**Costs.**
- Minecraft: one extra level pass per visible pair portal, plus up to two recursion passes. The existing second camera measured 2.3-2.8 ms for two cameras at RD 12. Expect about +2-3 ms of Minecraft frame time per visible portal at RD 12-16 for portal passes.
- Portal 2: one upload instead of 2-6, so the render thread gets cheaper (each Download is ~4-6 ms today).
- Work: mod about 1.5-2 agent-days; plugin about 0.5; tests reuse mcworld1, where the facing tunnel and cliff↔cliff are the gates.

**Risks.**
1. Minecraft 26.3's main target needs a stencil attachment. Pass-local FBOs may be needed (probe first).
2. Re-entrant renderLevel: entities and particles are drawn once per pass (correct for portals, but it costs extra), and per-pass section culling has to start from the virtual camera.
3. The opening's shape and animation must match Portal 2's rim exactly. Open/close animations scale the opening, so the plugin must send the current scale.
4. A portal view in Minecraft is up to one host frame late (the primary's latency), the same as the main view, so the two stay consistent with each other.
5. Chell, cubes and Portal 2 props seen *through* a Minecraft-drawn portal are missing. Portal 2 can't draw them inside Minecraft's image, so we need a second occlusion or proxy path (Chell proxy model) or accept it.
6. A Minecraft frame budget at RD 24 + 3 passes may fall under 180 images/s. The fallback is reprojection, as now.
