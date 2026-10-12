# Digging through Portal 2's geometry: rendering and visibility, redesigned (bridge #73)

The user, on the current build: *"The Minecraft breaking of the portal geometry feels underbaked and needs severe rethinking.
The parity between the two engines needs to be more closely aligned."* Six screenshots
(`cache/bridge/user_shots/dig_rethink/30..35`):

| # | what the user saw |
|---|---|
| 30 | where blocks were broken, a translucent copy of the Portal 2 wall is still drawn over the cut |
| 31 | looking through a corner cut-out into the same room: z-fighting, and white void where textures should be |
| 32 | standing in the chamber: doors, buttons, water and props all present (the reference) |
| 33 | standing INSIDE the cut: doors, buttons, many other objects and the water are gone |
| 34 | close to a hole: the Minecraft hole (stone walls, a torch) shows correctly |
| 35 | walked further away: the Portal 2 wall is drawn over the hole again, faintly showing the blocks behind it |

The ask: a data-driven design instead of "screenshot, look, patch". This document gives the diagnosis (section 1), the new model
(section 2), the techniques considered and the choice (section 3), the verification harness (section 4) and the rewrite plan
(section 5).

## 1. Why each symptom happens in the current design

Today a dug cell exists in four separate places, each with its own approximation:
- **collision**: the harvest export minus the dug cells (exact, cell by cell);
- **the picture**: a screen-space stencil "punch" (`digrender.cpp DigPunch`). After the engine has drawn the opaque world, the
  dug cells' boxes mark the stencil wherever the nearest drawn pixel lies inside a box (z-fail parity). Those pixels get a dark
  colour and the far depth. Then the "peel" (#44) redraws the engine's whole world list once per "exit plane", with a clip
  plane, so something appears behind the hole;
- **visibility**: `visclamp.cpp`. A camera in solid gets the vis origin of the last camera position that was in open air. Up to
  20 "sample points" around the nearest 24 dug boxes are merged into the main view's vis. Both are rebuilt per 20 u camera cell;
- **the hole's walls**: Minecraft blocks and wall quads (the mod's reveal and `DigWalls`), drawn as band meshes, every face
  pushed 0.06 u back.

Every approximation is tied to the main view, the camera's distance, caps, or history. The symptoms follow from these
mechanisms:

### 30, a translucent ghost of the wall over the cut
The punch runs once, at the start of the opaque renderables pass. Anything the engine draws after it is never punched:
- the **translucent world surfaces**: the engine draws them per leaf inside the translucent pass (IVRenderView slot 19,
  "draw translucent surfaces"). These are blended wall layers, glass, decals with alpha, and overlays on translucent materials;
- the **translucent renderables**;
- **projected-texture (flashlight) passes** that the engine applies after the opaque world.

They land in the punched pixels as a semi-transparent copy of the wall: the "ghost". The peel adds a second source. It redraws
the world through cluster-AABB exit planes. Where the cut is not one convex box (a column at a wall's foot, an L shape, two
cells meeting at an edge), an exit plane can keep part of the original wall face. That face is then redrawn into the hole.
*Harness metric:* the ghost fraction (section 4), the share of the cut's silhouette whose colour still matches the intact wall.

### 31, z-fighting and white void looking through a corner cut-out
- **Z-fighting.** A revealed block whose face lies on the wall's surface plane (its cell is inside the wall, and its outer face
  is where the wall's face is) is drawn by Minecraft at the same depth as Portal 2's face, minus the 0.06 u pull-back. 0.06 u is
  below the depth buffer's resolution beyond about 1500 to 2000 u (24-bit depth, near plane about 7 u). Mid-range views flicker,
  and so does every view where the punch box's 0.5 u growth half-covers such a face.
- **White void.** Pixels the punch cleared, with nothing of ours behind them and no peel plane that reaches the geometry beyond.
  Two causes:
  1. the peel's planes are the bounding box of the whole cut cluster. In a corner cut, the plane that would expose the room
     beyond is a face of the AABB, not of the cut, and is often culled by the "camera side" and "facing" tests;
  2. the leaves behind the cut are not in the view's visibility set. The vis samples cover only the 24 nearest boxes and 20
     points, rebuilt per 20 u camera cell. So the engine never draws them, and the frame's clear colour (white) shows.

  *Harness metrics:* the z-fight fraction (frame-to-frame instability at a still camera) and the void fraction (the depth is the
  far plane or clear where geometry is expected).

### 33, objects and water vanish when standing inside the cut
Not a rendering problem at all: **the server stops sending those entities.**
- Each tick the server builds the client's PVS in `ClientSetupVisibility` (ServerGameClients004 slot 9). It resets the PVS, then
  adds the player's eye with `AddOriginToPVS` (VEngineServer022 slot 59).
- With the eye inside a dug cell, the eye is inside Portal 2's solid. Its leaf is a solid leaf, cluster -1. The engine's
  cluster PVS for -1 is empty, and its area is 0.
- So `CheckTransmit` sends no entity that is not "always transmit". On the client, doors, buttons, props, `func_water_analog`
  water, indicator lights and every other networked entity go **dormant** and are not drawn.
- The world (brushes and static props) is client-side and still draws: the vis clamp gives the client's world vis a good
  origin. That matches the screenshot exactly: world and static props present, entities and the water entity gone.
- The client-side vis clamp never reached the server.

*Harness metric:* the number of entities that are in the frustum and in line of sight but dormant, or not visible by the
client's world vis (section 4.2).

### 34 and 35, correct when close, the wall layered back when walking away
- The punch, the peel and the vis samples all run on the nearest N things near the camera: punch boxes within 2600 u, the
  nearest 256 cells, peel clusters within 1800 u (only 2 of them, 6 planes), vis samples from the nearest 24 boxes, cached per
  20 u camera cell.
- On top of that, the band draw selects sections nearest first, within a vertex budget (#52). A hole's revealed blocks can drop
  out of the budget while the hole is still in view.
- Each limit changes with the distance and with what else is near the camera. So a hole that is right when close turns into
  the dark paint, then the peel's partial redraw, then nothing: the wall comes back, faintly showing what is behind.
- A further distance effect is the punch box itself. It is grown 0.5 u, and the wall face must lie inside it. Walls sit on the
  cell grid only to within the map's 0.2 block offset. When the wall's face lies just outside a dug cell, the punch misses it
  entirely, from every distance; when it lies just inside, depth precision decides it, and from far away it loses.

*Harness metric:* the ghost and void fractions at 2, 5, 10, 20 and 40 blocks from the same hole.

### The common root
The picture of a hole is assembled **after the fact, in screen space, for one view**, from heuristics with distance and count
limits. Visibility is patched **on the client only, from the camera's history**. Nothing derives both from the one thing that
is exact: the dug cells. So every new view type (portal views, water reflections, shadow maps, the server's PVS) and every new
distance needs its own patch.

## 2. The new model: one set of dug cells, every consumer derives from it

**The authority.** The dug-cell set per map, in Minecraft block coordinates (`dig.cpp`, unchanged: the mod's SkyDig sends it
per section, kRenDug). With the region mapping (an affine map, `coords.hpp`) a cell is an axis-aligned 40 u box in Source
space.

From it, deterministically, with no camera history, distance or "nearest N":

| consumer | rule | where |
|---|---|---|
| collision (Minecraft, Chell, gun) | the harvest export minus the dug cells (unchanged) | collfield/colharvest, blockcollide, portalgun |
| **Portal 2's world picture** | a world fragment is not drawn if the cell **behind** it along the view ray is dug, or the cell **in front** of it is dug: `dug(cell(p + e*d)) or dug(cell(p - e*d))`, with `p` the fragment's world position, `d` the view direction, `e` = 0.25 u. The first part removes surfaces that belong to the dug solid; the second removes host faces seen from inside the hole on its boundary, which Minecraft's revealed blocks or hole walls cover. | the GPU, inside the engine's own world shaders: `cutrender.cpp` (section 3) |
| what the hole shows | whatever is behind, drawn by the engine's own depth test in the same pass: the next room, the chamber, the revealed blocks | nothing extra |
| **the revealed blocks** | drawn as Minecraft sends them, except faces that coincide with a visible host surface (the block's own cell is host-solid right behind the face, the outward cell is host-empty and not dug): those are dropped, so there are no coplanar pairs and no z-fighting by construction | `terrain.cpp` band build |
| **visibility (every view)** | a connected group of dug cells, a "cut", is a new portal between the open leaves it touches. Its **mouths** are open-air points just outside its faces, one per cluster. For any vis origin: add the mouths of every cut that the origin's cluster can see. An origin inside a cut (or in solid touching one) is replaced by its cut's mouths. | `cutvis.cpp`, the ViewSetupVisEx hook, for every view (portal and reflection views included) |
| **the server's PVS** (entities, water entity, areas) | the same origins as the main camera's vis, added with the engine's own `AddOriginToPVS` after the game's `ClientSetupVisibility`: entities seen through a cut, or from inside one, are sent and drawn | `cutvis.cpp`, ServerGameClients004 slot 9 hook |
| Chell's physics shadow, light | unchanged (shadow off near dug cells, #44); band light as today | blockcollide, bandlight |
| holes in movers | unchanged (#43/#50 mover punch and walls, in the mover's frame) | digrender (mover part), moverwalls |

What disappears:
- the stencil punch and the dark paint;
- the peel (the world-list replay);
- the union boxes and the exit planes;
- the "last good camera position";
- the 20 u camera-cell sample cache;
- the 2600 u, 1800 u and 256-cell limits.

## 3. Techniques considered, and the choice

**(a) Remove host surfaces at the source by rebuilding them.** This is SkyCraft's way (`DigMesh.cpp`): it clones each
BSTriShape minus the dug cubes, hides the original, and Skyrim's own shaders draw the clone. In Portal 2 that means:
- clipping each BSP face, displacement triangle and static prop against the dug boxes;
- flagging the originals not to draw (writing the engine's surface flags);
- drawing the remainders ourselves with the right material, lightmap page, bumped-lightmap coordinates, paint map and
  env-map.

It is exact geometry. But it re-implements the world's shading (LightmappedGeneric with bump, WorldVertexTransition blends,
Portal 2's paint), and any mismatch shows as a seam around every hole. It also needs separate paths for faces, displacements,
overlays, decals and static props, and engine-internal surface layouts. That is weeks of work and a permanent risk of seams.

**(b) Correct visibility.** Needed whatever draws the hole: chosen (`cutvis`), as the deterministic mouth rule above, on the
client vis and the server PVS.

**(c) Depth-correct CSG compositing with fixed-function stencil and depth.** Removing a fragment inside a volume is a
point-in-volume test per fragment. The stencil parity count can do that only for the nearest layer. Showing the next layer
needs depth peeling against the previous layer's depth, and that needs a shader. Every fixed-function variant is an
approximation of the kind we have now (clip planes, AABBs).

**(d) SkyCraft's port, done at the fragment level: cut-away world shaders. CHOSEN.** Portal 2 on Linux renders through
togl, which translates every D3D shader into GLSL text at run time and links each vertex/pixel pair into a GL program
(`libtogl.so`: `#version 120`, `glShaderSourceARB`, `glLinkProgramARB`). We intercept the link (togl's GL entry points are a
table of function pointers in libtogl's data, patched in place) and add a few lines to the pair:
- **vertex shader:** keep the D3D clip position (togl's own variable, captured just before togl's flip and depth fix-up:
  the line `vTempPos.y = -vTempPos.y;` is in every vertex shader togl writes) as a varying.
- **fragment shader:** at the top of `main`, rebuild the world position from that clip position, step `e` along the view
  ray each way, look both cells up in a 3D texture of dug cells, and `discard` if either is dug.

The parameters live in global GL state that togl never touches:
- the world-from-clip transform: three rows of view x projection, which are unaffected by Portal 2's oblique portal clipping;
- the eye, the enable flag and the texture's frame, in `gl_TextureMatrix[4..7]`, set with `glMatrixLoadfEXT`;
- a sparse 3D texture on unit 31: a brick index over the whole ±16384 u map plus a brick atlas, so there is no window and no
  distance limit.

A functor on the render thread sets them at the start of each world draw (DrawWorldLists, slot 13, and translucent surfaces,
slot 19), and clears the flag after. So only Portal 2's world is cut:
- brushes, displacements, decals, overlays, water and translucent world surfaces;
- their flashlight passes;
- every view type: the main view, portal views, reflections, shadow maps.

Entities, our own blocks and the HUD are not cut. The parameters come from that view's real matrices, read on the render
thread.

Why this is the right trade:
- **Fidelity.** What stays visible is drawn by Portal 2's own shaders, with its lighting, bump maps, paint and decals. Nothing
  is re-implemented, so there are no seams.
- **Correctness.** It is per fragment and exact, in every view, at every distance. What is behind a hole is drawn by the
  engine's depth test in the same pass, so there is nothing to peel and no stencil.
- **Ghosts.** Translucent world surfaces and decals are cut too: the ghost of 30 cannot happen.
- **Cost.** One varying, about 30 ALU instructions and two texture fetches per world fragment, only while a cut exists near
  enough to matter (the enable flag is off when no cell is dug). `discard` turns off early-Z for world shaders: at
  1600x900 that is far below a millisecond on this GPU.
- **Shader compiles.** Two extra compiles per program, once, at the program's first link. Programs linked before the plugin's
  hook are relinked once with the engine's own `gl_flushpaircache` (a togl console command).
- **Robustness.**
  - A program whose text does not match (the anchor line missing) is linked unpatched and counted. It then draws the wall
    over the hole, and the harness's coverage check catches it.
  - Compile or link failures fall back to the original shaders.
  - Without togl's GLSL (another renderer), everything falls back to `pc_dig_render 1`, the old path.
- **Behind a switch.** `pc_dig_render 2`: nothing is hooked until the switch is on.

Known limits of the choice:
- **Static props** are drawn in the opaque renderables pass, not the world list. Version 1 cuts the world list only, so a
  static prop inside a dug cell stays. Version 2: gate the static-prop draw (StaticPropMgrClient005) the same way.
- **Brush entities** (`func_brush`, doors) are entities and not cut, as before. Movers keep #43's lattice.
- **Physics props and cubes** still rest on the original surface. That is a vphysics gap, unchanged.
- **Minecraft's own picture through a Portal 2 portal** is unchanged.

### Performance budget (per frame, 1600x900)
| item | budget |
|---|---|
| fragment cut test | under 0.3 ms GPU (world fragments only) |
| brick texture updates | under 0.1 ms CPU per changed cell (one 8x8x8 brick upload) |
| mouth rebuild | under 2 ms, only when the dug set changes |
| per-view vis augmentation | a few PVS bit tests per cut: microseconds |
| server PVS | under 10 AddOriginToPVS calls per tick |

## 4. The verification harness (data, not screenshots)

`digharness.cpp` (plugin) plus `tools/digharness/` (runner and judge). Each probe works on one frame, the main view:

1. **Ground truth from geometry.**
   - **Rays.** A grid of 64 x 36 sample pixels. The rays come from the view and projection matrices that the frame was really
     drawn with, read on the render thread.
   - **The expected first hit** is found by marching each ray against the engine's world contents with the dug cells removed:
     an empty-to-solid transition, not inside a dug cell, refined by bisection. The march also tests the Minecraft solids, a
     world-only trace for water, and an entity trace.
   - **What the old wall would show:** the same march without the cut.
2. **What the frame shows.**
   - **Depth and colour** at the samples, read back on the render thread right after the main view's translucent pass, before
     the HUD. A multisampled target is resolved first.
   - Each actual hit is classified by the candidate distance it matches:

     | class | the pixel shows |
     |---|---|
     | `host` | the cut host surface: correct |
     | `ghost_wall` | the old wall face where the cut host lies deeper |
     | `mc` | a Minecraft block |
     | `entity` | an entity |
     | `void` | the far plane, or the clear colour |
     | `other` | none of these |
3. **Entities (4.2).**
   - The **expected set**: for every networked entity whose box is in the frustum and that some sample ray reaches first
     (line of sight with the cut), plus everything the same probe finds from a reference open pose.
   - The **actual state**, per entity: dormant or not (the client did or did not receive it), and the client's own
     `IsBoxVisible`.
   - **Water:** expected water pixels (the ray meets a water surface first) against actual pixels at the water's depth.
4. **Visibility data.** The camera's leaf: contents, cluster and area, from the engine; the vis origins actually passed to the
   engine for this view; the cut groups and their mouths.
5. **Coverage (new path only).** An audit mode makes the patched shaders paint inside-cut fragments magenta instead of
   discarding them. Any pixel of the cut's silhouette that is not magenta is a world draw path the cut does not reach.
6. **Frame stability.** Two consecutive frames at a still camera. A colour change above a threshold in host or Minecraft
   pixels counts as z-fighting.

Output: `cache/bridge/digharness/<tag>/<probe>.json`, one line of numbers per probe in the log, and `judge.py` →
`report.txt` / `results.json`. These are the reproductions, pass or fail by numbers. Each runs on `pc_testbed` and on
`sp_a1_intro3`; the cells are mined through the mod (`pcdig mine`), so the Minecraft reveal is real:

| id | reproduction | pass when |
|---|---|---|
| S30 ghost | dig 2 cells at a wall's foot; stand 3 blocks away facing it | ghost_wall at most 1% of the cut silhouette; colour-ghost at most 1% |
| S31 corner | dig a 2x2x2 cut through a room corner; look through it at the room from the side | void at most 0.5%, z-fight at most 0.2% of the samples |
| S32 open (reference) | stand in the chamber facing the same way as S33 | every expected entity drawn (dormant 0) |
| S33 inside | stand inside the cut, same direction | dormant expected entities = 0; water pixels within 10% of S32's |
| S34/35 distance | the S30 hole from 2, 5, 10, 20, 40 blocks | ghost_wall and void at most 1% at every distance |

Screenshots are taken too: as evidence, not as the test.

## 5. The rewrite plan

1. **The harness** (digharness.cpp, tools/digharness). Run on the current build for the "before" numbers.
2. **Cut-away world shaders** (`cutrender.cpp`):
   - the togl GL-table hook, link-time patching, the brick textures, and the per-view functors from the DrawWorldLists and
     translucent-surfaces hooks (visclamp's render interface);
   - `pc_dig_render 2` turns it on and turns the punch and peel off;
   - audit mode for coverage.
3. **Deterministic visibility** (`cutvis.cpp`):
   - cuts, mouths, vis origins for every view;
   - the server PVS hook;
   - it replaces visclamp's last-good origin and its samples under `pc_dig_render 2`.
4. **No coplanar Minecraft faces** (`terrain.cpp`): drop band faces that coincide with a visible host surface.
5. **Static props** (v2, if the harness shows props in cuts matter): gate the static-prop draw.
6. **Make it the default** once every symptom passes, then delete:
   - `DigPunch`, `PeelPlanes`, `UnionBoxes`, `NearPunchBoxes` and the world-list replay (digrender.cpp, visclamp.cpp);
   - visclamp's `g_good` fallback and its sample builder.

   The mover-hole punch (`DigMoverMark/Restore`) and `moverwalls.cpp` stay: they serve movers, not the world.

Every step is checked with the harness;.

## 6. Status (bridge #75)
Steps 1-4 and 6 are done and `pc_dig_render 2` is the default; the punch, peel, union boxes, nearest-cells cache, world-list replay and
dug-cell vis samples are deleted (mover punch, moverwalls and mover vis samples stay). Added beyond section 2: an eye inside a cut takes
its fog volume / water from the cut's nearest mouth (IVRenderView slot 10). Static props in cuts (step 5) are not built: no probe has
shown one.

## 7. Cut caps (bridge #76)
The hole's walls where a dug cell meets a cell the host's surface runs through were the mod's sub-voxel walls (5 u steps), which left a strip of up to 2.5 u of nothing (or a lip) beside Portal 2's cut wall face: the user's "white void at the seam" (38). They are now exact: each such face is clipped to the BSP's visible solid (colharvest `World::SolidParts`) in the band build (`terrain.cpp CutCaps`, `pc_dig_caps`), textured and lit as the mod's wall, like SkyCraft's exact land caps. The cut's owner-cell step is along the surface normal (`pc_cut_rule`). The harness's seam probe (`pc_dh_seam`, `dh.py --scen seam,user38`) measures it.

## 8. Light inside the cuts (bridge #79)
The user: *"the light from Portal doesn't bleed into the Minecraft blocks"* when digging into Portal 2's geometry. A band face is lit from Portal 2's model lighting state half a block in front of it. Inside a cut that point is inside Portal 2's solid (the BSP never learns a cell was dug): the engine has no light there, so the face took the section's mean or the last good state, and the contact-AO traces started in solid (half strength on every face). The result was a lit wall face at the mouth beside black caps and blocks, and a black tunnel seen from the room.

The cut's light now comes from the dug cells themselves, the way Minecraft lights a cave (`cutlight.cpp`, `pc_cut_light`, default on):
- **Mouths.** A dug cell with Minecraft air (no revealed block) is a mouth when Portal 2's open air touches it, either through one of its faces (3 u out at the face centre or its quarter points, as cutvis finds mouths) or because its own centre is open air (the host's face runs through it). Each mouth samples Portal 2's lighting state in that open air, 16 u out. The state is the ambient cube plus the engine's local lights, dynamic lights included. Our block lights are left out, because Minecraft's block light carries them. The state is evaluated per axis (`BandLightCubes`).
- **Propagation.** A breadth-first walk over the dug air cells starts from every mouth at once. The light drops one level per block with Minecraft's brightness curve, Curve((15 - d) / 15): 1 at the mouth, then 0.78, 0.62, 0.5, down to 0 at 15 blocks. At each distance a cell takes the per-channel maximum of the mouths that reach it. Revealed blocks stop the light.
- **Faces.** A band vertex whose sample point is in Portal 2's solid and that faces dug air takes the cube along its normal. It is interpolated over the four cells in front of the vertex, which is Minecraft's smooth lighting. Minecraft's sky light (on the same mouth light) and block light (the band's torch term) are the floor. Portal 2's contact AO and the band's roof term do not apply there; Minecraft's own vertex AO stays.
- **Rebuilds.** The field is rebuilt when the dug set changes, when the Minecraft blocks in dug cells change, or when the mapping changes. The band sections whose cells' light changed are rebuilt. A mouth whose state came back empty is retried.

The harness's seam probe now also measures **luminance across the mouth's edge**. Every face between a dug cell and a non-dug cell is scanned with a 2 u lattice that reaches 10 u past the face. Each edge point between Portal 2's solid and open air gives a pair of rays: one hits the host's wall face 2 u out from the edge and 4 u along it, the other hits the cap or block 4 u inside the edge. A pair counts when both rays hit what they aimed at, and the judge compares the mean linear luminance of the two sides (`light` verdict: within one stop). The probe also has an **edge test**: a seam sample that fails exactly on a silhouette edge is re-traced 1.5 px to each side, and if it matches the surface there it is classed `edge` (MSAA aliasing), not missing.

**Bridge #80.** The harness's new `albedo` scenario (`_AL` probes: Portal 2 fullbright, the band unlit; verdict `light_albedo`) separates light from texture. It showed the mouth's light was right, but each face in the cut took the cube's component for its own normal, and caps along the mouth's edge (sample point out in the room's air) took the room path with Portal 2's contact AO: 0.42 of the wall's light at the edge. Now a mouth also carries its **inflow** (the cube component for a surface facing out of its open face, the light the host wall facing that way gets), propagated like a Minecraft light level; a face takes max(its normal's component, inflow) x falloff, and every band face looking into a dug-air cell of the field takes the cut light (`pc_cut_light 2`, `pc_cut_light_faces 0` restore #79's rules). sp_a1_intro2: light_albedo 0.42 -> 0.70 (within one stop).
