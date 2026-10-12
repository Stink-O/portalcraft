// Portalcraft bridge: draws Minecraft blocks inside Portal 2's own frame.
//
// Hook point: CRendering3dView::DrawTranslucentRenderables in client.so, found
// with SourceAutoRecord's Linux signature (SAR's OverlayRender uses the same
// function). Every 3D scene view the engine renders goes through it: the main
// view, each recursive portal view (stencil portals), water reflection /
// refraction views and monitor views. It receives (inSkybox, shadowDepth), so
// the 3D-skybox pass and shadow-depth passes can be skipped. The blocks are
// drawn at the start of the translucent pass, i.e. after all opaque world
// geometry and opaque renderables, with depth test and depth write on, so the
// map occludes them and later translucent geometry (glass, portals' own
// overlays, particles) blends over them.
#include "bridge.hpp"
#include "prof.hpp"

#include "Hook.hpp"
#include "Modules/MaterialSystem.hpp"
#include "Modules/Tier1.hpp"
#include "Offsets.hpp"
#include "Utils/Memory.hpp"
#include "Utils/lodepng.hpp"

#include <chrono>
#include <cmath>
#include <cstring>

namespace bridge {

QAngle MainViewAngles();                // avatar.cpp (bridge #16)
bool IsMainCameraOrigin(const Vector &origin);  // visclamp.cpp (bridge #51)
void DigRenderShutdown();               // digrender.cpp
void DigMoverMark(const CViewSetup *view);     // digrender.cpp (bridge #43)
void MoverWallsDraw(const CViewSetup *view);   // moverwalls.cpp (bridge #50)
void DigMoverRestore(const CViewSetup *view);  // digrender.cpp (bridge #43)
void AvatarDraw(const CViewSetup *view);  // avatar.cpp
void StagePropsDraw(const CViewSetup *view);  // stageprops.cpp (bridge #74)
void SceneDraw(const CViewSetup *view, bool blendedPass);  // scene.cpp (bridge #19)
void SceneShadowsDraw(const CViewSetup *view);           // scene.cpp (bridge #19)
void ColOverlayDraw(const CViewSetup *view);             // coloverlay.cpp (bridge #24)
void DigHarnessMainView(const CViewSetup *view);         // digharness.cpp (bridge #73)
static IMaterial *g_blockMat = nullptr;
// Material variants for the lighting investigation (pcb_mat <n>).
static IMaterial *g_mats[4] = {};
static const char *g_matNames[4] = {"UnlitGeneric+tex+vertexcolor", "UnlitGeneric vertexcolor only (SAR solid)", "VertexLitGeneric+tex+vertexcolor", "UnlitGeneric+tex+vertexcolor+vertexalpha"};
static bool g_debugColor = false;

void RenderSelectMaterial(int n, bool debugColor) {
	if (n >= 0 && n < 4 && g_mats[n]) g_blockMat = g_mats[n];
	g_debugColor = debugColor;
	console->Print("[bridge] block material %d (%s), debug colour %d\n", n, n >= 0 && n < 4 ? g_matNames[n] : "?", debugColor);
}
static ITexture *g_atlasTex = nullptr;

// Atlas: 4 tiles of 16x16 in a row (64x16). Tile index per face kind.
enum Tile { kGrassTop = 0, kGrassSide = 1, kDirt = 2, kPlanks = 3 };
static const int kAtlasW = 64, kAtlasH = 16;

static int g_frame = 0;
static bool g_terrainMeshes = false;  // debug only (pcb_terrain_meshes)
static int g_traceViews = 0;
static ViewRecord g_lastMain;
static int g_viewsThisFrame = 0, g_lastFrameViews = 0;
static double g_lastViewTime = 0;

static double NowMs() {
	using namespace std::chrono;
	return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

const ViewRecord &LastMainView() { return g_lastMain; }
void RenderTraceViews(int n) { g_traceViews = n; }

// ---------------------------------------------------------------- atlas

static bool LoadTile(std::vector<uint8_t> &atlas, int tile, const char *name, uint32_t tint) {
	std::string path = MinecraftAssetsDir() + "/textures/block/" + name + ".png";
	std::vector<unsigned char> rgba;
	unsigned w = 0, h = 0;
	unsigned err = lodepng::decode(rgba, w, h, path);
	if (err || w != 16 || h < 16) {
		console->Warn("[bridge] could not load %s (%s)\n", path.c_str(), err ? lodepng_error_text(err) : "size");
		// magenta/black checker fallback
		for (int y = 0; y < 16; ++y)
			for (int x = 0; x < 16; ++x) {
				uint8_t *p = &atlas[(y * kAtlasW + tile * 16 + x) * 4];
				bool c = ((x / 4) ^ (y / 4)) & 1;
				p[0] = c ? 255 : 0; p[1] = 0; p[2] = c ? 255 : 0; p[3] = 255;  // BGRA
			}
		return false;
	}
	float tr = ((tint >> 16) & 255) / 255.0f, tg = ((tint >> 8) & 255) / 255.0f, tb = (tint & 255) / 255.0f;
	for (int y = 0; y < 16; ++y)
		for (int x = 0; x < 16; ++x) {
			const unsigned char *s = &rgba[(y * w + x) * 4];
			uint8_t *p = &atlas[(y * kAtlasW + tile * 16 + x) * 4];
			p[0] = (uint8_t)(s[2] * tb);
			p[1] = (uint8_t)(s[1] * tg);
			p[2] = (uint8_t)(s[0] * tr);
			p[3] = 255;
		}
	return true;
}

static bool BuildAtlas() {
	std::vector<uint8_t> atlas(kAtlasW * kAtlasH * 4, 255);
	// Minecraft's default (plains) grass colour, applied to the greyscale top.
	LoadTile(atlas, kGrassTop, "grass_block_top", 0x91BD59);
	LoadTile(atlas, kGrassSide, "grass_block_side", 0xFFFFFF);
	LoadTile(atlas, kDirt, "dirt", 0xFFFFFF);
	LoadTile(atlas, kPlanks, "oak_planks", 0xFFFFFF);
	g_atlasTex = materialSystem->CreateTexture("_pcb_block_atlas", kAtlasW, kAtlasH, atlas.data(),
	                                           TEXTUREFLAGS_POINTSAMPLE | TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT);
	return g_atlasTex != nullptr;
}

// ---------------------------------------------------------------- mesh

// Face order: +X -X +Y -Y +Z(top) -Z(bottom), Source axes (Z up).
static const Vector kNormals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

// Minecraft's fixed directional face shading: top 1.0, bottom 0.5, N/S 0.8, E/W 0.6.
// Source +X = MC +X (east), Source +Y = MC -Z (north).
static const float kFaceShade[6] = {0.6f, 0.6f, 0.8f, 0.8f, 1.0f, 0.5f};

static int TileFor(int kind, int face) {
	if (kind == 1) return kDirt;
	if (kind == 2) return kPlanks;
	if (face == 4) return kGrassTop;
	if (face == 5) return kDirt;
	return kGrassSide;
}

// Source lighting (linear, what ComputeLighting returns) -> vertex colour.
// Portal 2's UnlitGeneric treats the vertex colour as linear and applies the
// frame's own gamma/tonemap output, so no gamma is applied here (gamma 1); the
// scale (5) was calibrated by eye against the chamber (cache/bridge/shots/light).
// Tunable at runtime with pcb_light <scale> <gamma> <floor>.
static float g_lightScale = 5.0f, g_lightGamma = 1.0f, g_lightFloor = 0.0f;

void RenderSetLightParams(float scale, float gamma, float floor) {
	g_lightScale = scale;
	g_lightGamma = gamma > 0.1f ? gamma : 0.1f;
	g_lightFloor = floor;
	console->Print("[bridge] light: scale %.3f gamma %.2f floor %.3f\n", g_lightScale, g_lightGamma, g_lightFloor);
}

static float LightToScale(float linear) {
	float v = std::pow(std::fmax(linear * g_lightScale, 0.0f), 1.0f / g_lightGamma);
	if (v < g_lightFloor) v = g_lightFloor;
	if (v > 1.0f) v = 1.0f;
	return v;
}

// For other files (terrain.cpp band sections, worldents.cpp): the same Portal 2 light -> vertex colour mapping (bridge #17).
float RenderLightToScale(float linear) { return LightToScale(linear); }

static void DrawBlock(const Block &b) {
	const float s = kUnitsPerBlock;
	const Vector o = b.mins;
	// corners for each face, counter-clockwise seen from outside, starting bottom-left
	// with the texture's "up" being +Z on the side faces.
	Vector q[6][4] = {
	    {{o.x + s, o.y, o.z}, {o.x + s, o.y + s, o.z}, {o.x + s, o.y + s, o.z + s}, {o.x + s, o.y, o.z + s}},  // +X
	    {{o.x, o.y + s, o.z}, {o.x, o.y, o.z}, {o.x, o.y, o.z + s}, {o.x, o.y + s, o.z + s}},                  // -X
	    {{o.x + s, o.y + s, o.z}, {o.x, o.y + s, o.z}, {o.x, o.y + s, o.z + s}, {o.x + s, o.y + s, o.z + s}},  // +Y
	    {{o.x, o.y, o.z}, {o.x + s, o.y, o.z}, {o.x + s, o.y, o.z + s}, {o.x, o.y, o.z + s}},                  // -Y
	    {{o.x, o.y, o.z + s}, {o.x + s, o.y, o.z + s}, {o.x + s, o.y + s, o.z + s}, {o.x, o.y + s, o.z + s}},  // top
	    {{o.x, o.y + s, o.z}, {o.x + s, o.y + s, o.z}, {o.x + s, o.y, o.z}, {o.x, o.y, o.z}},                  // bottom
	};

	MeshBuilder mb(g_blockMat, PrimitiveType::QUADS, 6);
	const float inset = 0.01f / kAtlasW;  // keep point sampling inside the tile
	for (int f = 0; f < 6; ++f) {
		// Light the face the way the engine lights models: ambient cube + local
		// lights at a point just outside the face, for the face normal.
		Vector c = (q[f][0] + q[f][2]) * 0.5f + kNormals[f] * 2.0f;
		Vector l = engine.LightAt(c, kNormals[f]);
		float shade = kFaceShade[f];
		uint8_t r = (uint8_t)(255 * LightToScale(l.x) * shade);
		uint8_t g = (uint8_t)(255 * LightToScale(l.y) * shade);
		uint8_t bl = (uint8_t)(255 * LightToScale(l.z) * shade);
		Color col(r, g, bl, 255);
		if (g_debugColor) col = f == 4 ? Color(255, 40, 40, 255) : Color(40, 40, 255, 255);

		int tile = TileFor(b.kind, f);
		float u0 = tile * 16.0f / kAtlasW + inset, u1 = (tile + 1) * 16.0f / kAtlasW - inset;
		float v0 = 0.0f + 0.01f / kAtlasH, v1 = 1.0f - 0.01f / kAtlasH;
		// texture v grows downwards: bottom-left corner = (u0, v1)
		const float uv[4][2] = {{u0, v1}, {u1, v1}, {u1, v0}, {u0, v0}};
		for (int k = 0; k < 4; ++k) {
			mb.Position(q[f][k]);
			mb.Color(col);
			mb.TexCoord(0, uv[k][0], uv[k][1]);
			mb.AdvanceVertex();
		}
	}
	mb.Draw();
}

// ---------------------------------------------------------------- hook

using _DrawTranslucentRenderables = void(__rescall *)(void *thisptr, bool inSkybox, bool shadowDepth);
static _DrawTranslucentRenderables DrawTranslucentRenderables = nullptr;
static void __rescall DrawTranslucentRenderables_Hook(void *thisptr, bool inSkybox, bool shadowDepth);
static Hook g_drawTranslucentHook(&DrawTranslucentRenderables_Hook);

// CRendering3dView::DrawOpaqueRenderables (SAR's Linux signature): a view composited here (a lone region
// portal is opening, CompositeWantsEarly: the main camera only, so never a shadow-depth view) is skipped by
// the translucent hook of that pass. Its arguments are forwarded untouched (cdecl: extra words are harmless).
using _DrawOpaqueRenderables = void(__rescall *)(void *thisptr, int a, int b, int c);
static _DrawOpaqueRenderables DrawOpaqueRenderables = nullptr;
static void __rescall DrawOpaqueRenderables_Hook(void *thisptr, int a, int b, int c);
static Hook g_drawOpaqueHook(&DrawOpaqueRenderables_Hook);
static void *g_earlyView = nullptr;
static void __rescall DrawOpaqueRenderables_Hook(void *thisptr, int a, int b, int c) {
	auto *view = reinterpret_cast<CViewSetup *>((uintptr_t)thisptr + 8);
	const auto profT0 = std::chrono::steady_clock::now();  // (bridge #25: "rdraw" is ours, not Portal 2's pass below)
	// (not cleared by other views: the portal views' opaque passes run between this view's two passes)
	if (CompositeWantsEarly(view)) {
		QAngle va = MainViewAngles();  // (bridge #16: in front of the player in F5 the view looks back)
		bool isMain = std::fabs(va.x - view->angles.x) < 0.01f && std::fabs(va.y - view->angles.y) < 0.01f;
		CompositeView(view, isMain);
		g_earlyView = thisptr;
	}
	// bridge #17: the chamber band's opaque blocks with Portal 2's opaque world, before its portals and translucents: drawn in
	// the translucent pass they covered a portal fitted flush on a block face (b17_place6/7)
	// (bridge #75: the dug cells leave Portal 2's picture inside its own world shaders now, cutrender.cpp; the #33 stencil punch is gone)
	TerrainDraw(view->origin, kTerrainBandOpaque, &view->angles);
	SceneDraw(view, false);  // bridge #19: Minecraft entities and block entities in the chambers (solid batches)
	StagePropsDraw(view);  // bridge #74: Portal 2's props standing in the virtual region, in the opaque pass so a portal's stencil view is drawn around them (stageprops.cpp)
	MoverWallsDraw(view);  // bridge #50: the walls of those holes (Minecraft block faces), before the mark takes the depth over (moverwalls.cpp)
	DigMoverMark(view);  // bridge #43: the holes mined in movers: depth, so the engine's entities are not drawn inside them (digrender.cpp)
	ProfAdd("rdraw", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - profT0).count());
	g_drawOpaqueHook.Disable();
	DrawOpaqueRenderables(thisptr, a, b, c);
	g_drawOpaqueHook.Enable();
	DigMoverRestore(view);  // bridge #43: ... and the depth given back after them
}

static void __rescall DrawTranslucentRenderables_Hook(void *thisptr, bool inSkybox, bool shadowDepth) {
	// CRendering3dView starts with a vtable pointer and one more word, then its
	// CViewSetup (SAR: "CRendering3dView inherits CViewSetup!").
	auto *view = reinterpret_cast<CViewSetup *>((uintptr_t)thisptr + 8);

	double now = NowMs();
	if (now - g_lastViewTime > 4.0) {  // views of one frame are rendered back to back
		g_lastFrameViews = g_viewsThisFrame;
		g_viewsThisFrame = 0;
		++g_frame;
		MeshBuilder::NewFrame();  // (bridge #52) per-frame dynamic-mesh budget
	}
	g_lastViewTime = now;

	if (!inSkybox && !shadowDepth) {
		PROF("rdraw");  // (bridge #25)
		++g_viewsThisFrame;
		if (g_traceViews > 0) {
			--g_traceViews;
			console->Print("[bridge] view t=%.2fms f=%d #%d origin=(%.1f %.1f %.1f) ang=(%.1f %.1f %.1f) %dx%d fov=%.1f\n", now, g_frame,
			               g_viewsThisFrame, view->origin.x, view->origin.y, view->origin.z, view->angles.x, view->angles.y,
			               view->angles.z, view->width, view->height, view->fov);
		}
		QAngle va = MainViewAngles();  // (bridge #16: in front of the player in F5 the view looks back)
		bool isMain = std::fabs(va.x - view->angles.x) < 0.01f && std::fabs(va.y - view->angles.y) < 0.01f;
		// (bridge #51) a portal view of a facing pair has the player's angles too: only the view whose camera is the player's is the main one
		if (isMain && IsMainCameraOrigin(view->origin)) {
			g_lastMain = {view->origin, view->angles, g_frame};
		}
		// Minecraft's own frame where the view looks into the region; the old terrain meshes only
		// while compositing is off (pcb_composite 0).
		if (g_earlyView == thisptr) g_earlyView = nullptr;  // (composited in the opaque pass)
		else {
			PROF("rdraw.composite");
			CompositeView(view, isMain);
		}
		AvatarDraw(view);  // bridge #16: the Minecraft player's body (avatar.cpp)
		if (g_blockMat) {
			for (auto &b : g_blocks) DrawBlock(b);
		}
		// bridge #17: blocks placed in the chambers (outside the region box) are always Portal 2's meshes (opaque ones in the
		// opaque pass, below); the region's sections only with the debug switch (the composite draws Minecraft there)
		TerrainDraw(view->origin, kTerrainBandTranslucent | (g_terrainMeshes ? kTerrainRegion : 0), &view->angles);
		WorldEntsDraw(view->origin);  // bridge #17: dropped items, cracks and the block outline in the chambers
		ColOverlayDraw(view);  // bridge #24: pc_show_collision (coloverlay.cpp)
		SceneShadowsDraw(view);  // bridge #19: Minecraft's contact shadows under the chambers' entities
		SceneDraw(view, true);  // bridge #19: particles and translucent entity parts in the chambers
	} else if (g_traceViews > 0) {
		--g_traceViews;
		console->Print("[bridge] view t=%.2fms f=%d skipped (inSkybox=%d shadowDepth=%d)\n", now, g_frame, inSkybox, shadowDepth);
	}

	// bridge #43: the engine's translucent entities (a door drawn with alpha, glass parts, a model's translucent skin) are made around the holes mined in movers too:
	// the pieces' depth is put back for this draw only (ours, above, are already drawn)
	const bool holes = !inSkybox && !shadowDepth;
	if (holes) DigMoverMark(view);
	g_drawTranslucentHook.Disable();
	DrawTranslucentRenderables(thisptr, inSkybox, shadowDepth);
	g_drawTranslucentHook.Enable();
	if (holes) DigMoverRestore(view);
	// (bridge #73) the dig-render harness reads the main view back right after its translucent pass (digharness.cpp)
	if (holes && IsMainCameraOrigin(view->origin)) {
		const QAngle va = MainViewAngles();
		if (std::fabs(va.x - view->angles.x) < 0.01f && std::fabs(va.y - view->angles.y) < 0.01f) DigHarnessMainView(view);
	}
}

int RenderViewsLastFrame() { return g_lastFrameViews; }

bool TerrainMeshesDebug() { return g_terrainMeshes; }

CON_COMMAND(pcb_terrain_meshes, "pcb_terrain_meshes 0|1 - debug only: draw the old Source-mesh Minecraft terrain (default 0; compositing replaces it)\n") {
	if (args.ArgC() > 1) g_terrainMeshes = std::atoi(args[1]) != 0;
	console->Print("[bridge] terrain meshes %d\n", g_terrainMeshes);
}

bool RenderInit() {
	console->DevMsg("[bridge] building atlas\n");
	if (!BuildAtlas()) {
		console->Warn("[bridge] atlas texture creation failed\n");
		return false;
	}
	console->DevMsg("[bridge] atlas ok\n");
	auto kv = new KeyValues("UnlitGeneric");
	kv->SetString("$basetexture", "_pcb_block_atlas");
	kv->SetInt("$vertexcolor", 1);
	kv->SetInt("$nocull", 1);
	g_mats[0] = materialSystem->CreateMaterial("_pcb_block", kv);
	kv = new KeyValues("UnlitGeneric");
	kv->SetInt("$vertexcolor", 1);
	g_mats[1] = materialSystem->CreateMaterial("_pcb_block_solid", kv);
	kv = new KeyValues("VertexLitGeneric");
	kv->SetString("$basetexture", "_pcb_block_atlas");
	kv->SetInt("$vertexcolor", 1);
	kv->SetInt("$nocull", 1);
	g_mats[2] = materialSystem->CreateMaterial("_pcb_block_vlg", kv);
	kv = new KeyValues("UnlitGeneric");
	kv->SetString("$basetexture", "_pcb_block_atlas");
	kv->SetInt("$vertexcolor", 1);
	kv->SetInt("$vertexalpha", 1);
	kv->SetInt("$nocull", 1);
	g_mats[3] = materialSystem->CreateMaterial("_pcb_block_va", kv);
	g_blockMat = g_mats[0];
	if (!g_blockMat) {
		console->Warn("[bridge] material creation failed\n");
		return false;
	}

	console->DevMsg("[bridge] material ok\n");
	auto fn = Memory::Scan(MODULE_NAME("client"), Offsets::DrawTranslucentRenderables);
	if (!fn) {
		console->Warn("[bridge] DrawTranslucentRenderables signature not found\n");
		return false;
	}
	DrawTranslucentRenderables = (_DrawTranslucentRenderables)fn;
	g_drawTranslucentHook.SetFunc(fn);
	console->Print("[bridge] view hook installed at client.so+0x%x\n", (unsigned)(fn - Memory::Absolute(MODULE_NAME("client"), 0)));
	auto fo = Memory::Scan(MODULE_NAME("client"), Offsets::DrawOpaqueRenderables);
	if (fo) {
		DrawOpaqueRenderables = (_DrawOpaqueRenderables)fo;
		g_drawOpaqueHook.SetFunc(fo);
		console->Print("[bridge] opaque-pass hook installed at client.so+0x%x\n", (unsigned)(fo - Memory::Absolute(MODULE_NAME("client"), 0)));
	} else {
		console->Warn("[bridge] DrawOpaqueRenderables signature not found (a first shot in Minecraft pops in)\n");
	}
	return true;
}

void RenderShutdown() {
	DigRenderShutdown();
	g_drawTranslucentHook.Disable(true);
	if (DrawOpaqueRenderables) g_drawOpaqueHook.Disable(true);
	for (auto &m : g_mats) materialSystem->DestroyMaterial(m), m = nullptr;
	g_blockMat = nullptr;
	materialSystem->DestroyTexture(g_atlasTex);
	g_atlasTex = nullptr;
}

}  // namespace bridge
