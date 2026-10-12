# Composite redesign: one Minecraft layer, same-frame (proposal for review, not built)

**Replaces:** the two modes in `composite.cpp`: the full-screen stream (camera in the region) and the portal-view stream, together with everything that patches the switch between them. That patchwork is the exit-frame detection (`ExitingPortal`, `g_exitFrame`), the "late main" and proximity main-view matching, the per-ray placement against the oblique far plane, the warm-up request and the frame-alternating stream scheduler.

**Kept:** the pinned cliff exit, noclip, Chell's gun on blocks, water, jumping, the black background (never the white sky), Minecraft's depth carried in the image alpha (GPU, full resolution), per-pixel alpha-tested occlusion layers, the frame and mask analysers, and asserts.

## Per frame
1. **OverrideView** (pose final, after input). We know the main camera and the live portal pair, which gives:
   - The views that can see the region. These are the main camera and its recursion through each visible portal pair, up to Portal 2's recursion depth (2). Virtual camera = main × portal transform; its clip plane is the exit portal's plane.
   - For each view, the Minecraft camera: one fixed rigid transform (`McToSrc`, 40 u/block), plus Minecraft's own projection with that clip plane.
   - The ones that look into the region: the camera, or its clip-plane side, lies in the region box, and its frustum meets it.
   - These requests (K ≤ 3) are written to shared memory right away, with the frame number. No overscan, because nothing is reprojected on a hit.
2. **Minecraft** renders all K cameras in one frame, one after another, each into its own target: colour plus depth in alpha. It ships them as K slots tagged with (frame, stream).
3. **DrawTranslucentRenderables** of each Portal 2 view:
   - Match the view to its request by camera pose (within 0.5 u and 0.1°, so no main-or-portal classification).
   - Wait for that slot with a bounded fence or futex: at most 3 ms in total per frame.
   - Upload only the view's screen box, at the view's resolution.
   - Draw the image as the background at the sky depth (depth test).
   - Draw the alpha-test occlusion layers over every Portal 2 object in the region (both portals, props), as now.
4. **Deadline miss:** use the stream's previous image with rotation plus position reprojection (today's path), and count it. Log per 300 frames: hit rate, wait times, Minecraft ms per camera, and upload ms.

## Stream keying (seamless crossing)
- Streams are keyed by Minecraft-space camera continuity, not by Portal 2 view type.
- Each request goes to the stream whose last pose is nearest: position under 2 blocks plus 30° × (frame gap), else a new stream (at most 3).
- On the crossing, the pre-teleport portal-view camera and the post-teleport main camera are the same Minecraft camera. They keep one stream, one target and one occlusion graph: no jump, no stale frame, no switch.
- Minecraft keeps one section graph per stream. Today a camera jump makes it render from the old graph for a frame or two; that's the white blocks seen after the exit.

## Depth
- Minecraft writes linear view depth, log-encoded (0.25–512 blocks, rounded toward farther), into alpha with its own projection, oblique plane included.
- Portal 2 compares per pixel through the alpha test: 255 materials, threshold "alpha ≥ level", measured exact.
- Chamber geometry is never behind the region: walls and floors block the view, and the region is only seen through portals, whose stencil and clip plane confine it. So depth testing against Portal 2 only matters for:
  - the region's sky walls: background at about 9000 u, in front of Portal 2's 2D sky cube (0.577 × zFar; measured NDC 0.9998);
  - Portal 2 objects inside the region: the occlusion layers.
- Writing a general per-pixel depth for the whole view would need a custom pixel shader. That's out of scope (no shader DLL changes).

## Budget (prof3 limits unchanged)
- Portal 2: hold 180 fps (mean ≤ 5.6 ms), worst frame ≤ 12 ms, ≤ 0.5 ms added on the render thread.
- Today's in-region frame: mean 5.60–5.64 ms, worst 9.7–12.6 ms; composite main-thread draw 0.13–0.30 ms.
- Today's texture regeneration: ~1.5 ms per 2000 × 1125 upload on the render thread. This is already over the 0.5 ms target for K > 0, so the redesign must cut it:
  - no overscan (1600 × 900 for the main view);
  - recursion views cropped to the portal's screen box;
  - upload rows only.

## Risks (measured where possible)
1. **Minecraft frame time:**
   - measured 130–170 fps delivered (6–7.7 ms per frame) at RD 24, not 2–5 ms;
   - K = 2–3 cameras per frame multiplies the level render;
   - a same-frame hit within 3 ms is unlikely at RD 24 without cheaper secondary cameras (lower RD, cropped viewports).
2. **Pacing:** Minecraft's frame loop follows the host's. In lock-step (Portal 2 waiting up to 25 ms for its own pose) 0 of 167 frames matched and Portal 2 fell to 37 fps.
   - Same-frame needs Minecraft to poll for requests and render on arrival, not on its own tick or vsync.
   - That's a change to the Fabric loop, with an unknown cost.
3. **Rendering several cameras per Minecraft frame:**
   - needs a re-entrant level render (several `renderLevel` calls), each with its own camera, section graph, culling and fog state;
   - the largest piece of Java work;
   - risks Minecraft state bugs (particles, entities extracted once per tick).
4. **Upload cost on the render thread:** see the budget.
5. **Portal 2 view order:**
   - the composite happens in each view's translucent pass;
   - recursion views render before the main view, so the deadline wait lands early in the frame and stalls Portal 2's render thread, not just the main thread;
   - this must be measured.
6. **Fallback quality:** on a miss the image is a frame late (today's reprojection). A miss rate above a few % will look like today's behaviour.

## Plan if approved
1. Minecraft: K-camera render with per-stream targets and graphs, and request polling (fakehost bench: ms per camera).
2. Protocol v13: request list and K slots.
3. Plugin:
   - per-view request and matching;
   - bounded wait;
   - cropped uploads;
   - delete the mode code.
4. Transition test: 20 crossings in each direction, walk and fling, gun-placed portals, with the camera
   aimed at the destination portal's centre every tick of the approach and fall (a per-tick view
   override, pitch 85-89 deg over the floor portal); assert `portal_in_view` (projected centre within the
   central 40% of the screen, opening >= N% of the screen); log pitch/yaw per tick. Glancing-angle
   crossings run as a separately labelled set. Every frame is checked for region background, staleness, and an image that doesn't match its pose (request id ≠ frame). Then prof3 and val2, then promotion.
