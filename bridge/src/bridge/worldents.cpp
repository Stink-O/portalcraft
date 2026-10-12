// Portalcraft bridge (bridge #17): Minecraft's per-frame world things that Portal 2 draws in the chambers, from the
// mod's WorldEntities block (SkyCraft's WorldRender BuildEntities / BuildOutline, ported to the material system):
//   * the block outline (Minecraft's targeted block, or where a held block would go on a Portal 2 surface: SkyClip);
//   * the block-breaking cracks (a box of the crack stage's texture over the block being mined, blended);
//   * dropped items: blocks as small spinning cubes with their own faces, other items as a turning sprite.
// Only outside the region box (the chamber band): inside it Minecraft's own frame (the composite) shows them.
// Items are lit by Portal 2 at their position, like the band's blocks (terrain.cpp).
#include "bridge.hpp"
#include "coords.hpp"
#include "link.hpp"

#include "Modules/MaterialSystem.hpp"
#include "Modules/Tier1.hpp"
#include "Utils/SDK/MeshBuilder.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace bridge {

using namespace skycraft::proto;

const char *TerrainAtlasTexName(int &gen, float &vScale);  // terrain.cpp
bool ClipHasRegion();                                      // clip.cpp
bool ClipRegionContains(const Vector &p);
float RenderLightToScale(float linear);                    // render.cpp

static bool g_enabled = true;
static IMaterial *g_matSolid = nullptr, *g_matCrack = nullptr, *g_matLine = nullptr;
static int g_matGen = -1;
static float g_vScale = 1.0f;
static WorldEntities g_we;
static bool g_have = false;
static double g_lastRead = -1;
static int g_drawnOutline = 0, g_drawnCracks = 0, g_drawnItems = 0;

static double NowMs() {
	using namespace std::chrono;
	return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

static bool EnsureMaterials() {
	int gen = 0;
	float vs = 1.0f;
	const char *tex = TerrainAtlasTexName(gen, vs);
	if (!tex) return false;
	g_vScale = vs;
	if (gen == g_matGen && g_matSolid) return true;
	char name[64];
	auto kv = new KeyValues("UnlitGeneric");
	kv->SetString("$basetexture", tex);
	kv->SetInt("$vertexcolor", 1);
	kv->SetInt("$alphatest", 1);
	kv->SetInt("$nocull", 1);
	std::snprintf(name, sizeof name, "_pcb_we_solid%d", gen);
	g_matSolid = materialSystem->CreateMaterial(name, kv);
	kv = new KeyValues("UnlitGeneric");
	kv->SetString("$basetexture", tex);
	kv->SetInt("$vertexcolor", 1);
	kv->SetInt("$translucent", 1);
	std::snprintf(name, sizeof name, "_pcb_we_crack%d", gen);
	g_matCrack = materialSystem->CreateMaterial(name, kv);
	if (!g_matLine) {
		kv = new KeyValues("UnlitGeneric");
		kv->SetInt("$nocull", 1);
		kv->SetInt("$vertexcolor", 1);
		kv->SetInt("$vertexalpha", 1);
		kv->SetInt("$translucent", 1);
		g_matLine = materialSystem->CreateMaterial("_pcb_we_line", kv);
	}
	g_matGen = gen;
	return g_matSolid && g_matCrack && g_matLine;
}

// Seqlock read, once per frame (the views of one frame reuse it).
static bool Read() {
	const double now = NowMs();
	if (g_lastRead >= 0 && now - g_lastRead < 3.0) return g_have;
	g_lastRead = now;
	uint8_t *base = link::Base();
	if (!base || !link::McAlive()) return g_have = false;
	auto *s = (const WorldEntities *)(base + kOffWorldEntities);
	for (int i = 0; i < 50; ++i) {
		uint32_t a = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
		if (a & 1) continue;
		std::memcpy(&g_we, s, sizeof g_we);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(&s->seq, __ATOMIC_ACQUIRE) == a) {
			if (g_we.count > kMaxWorldEntities) g_we.count = kMaxWorldEntities;
			return g_have = a != 0;
		}
	}
	return g_have;  // (torn: last frame's)
}

bool ClipInTerrainBox(const Vector &p);  // clip.cpp (bridge #61)
static bool InChambers(const Vector &src) { return !ClipInTerrainBox(src); }

static Color Lit(const Vector &src, float shade, uint32_t tint) {
	Vector l = engine.LightAt(src + Vector(0, 0, 4), Vector(0, 0, 1));
	float r = RenderLightToScale(l.x) * shade, g = RenderLightToScale(l.y) * shade, b = RenderLightToScale(l.z) * shade;
	if (tint) r *= (tint & 0xFF) / 255.0f, g *= ((tint >> 8) & 0xFF) / 255.0f, b *= ((tint >> 16) & 0xFF) / 255.0f;
	auto c = [](float v) { return (uint8_t)std::fmin(255.0f, std::fmax(0.0f, v * 255.0f)); };
	return Color(c(r), c(g), c(b), 255);
}

// A quad from MC-space corners (TL, TR, BR, BL) with an atlas rect {u0, v0, u1, v1}.
static void Quad(MeshBuilder &mb, const double p[4][3], const float *uv, Color col) {
	const float u[4] = {uv[0], uv[2], uv[2], uv[0]}, v[4] = {uv[1], uv[1], uv[3], uv[3]};
	for (int k = 0; k < 4; ++k) {
		mb.Position(McToSrc(p[k][0], p[k][1], p[k][2]));
		mb.Color(col);
		mb.TexCoord(0, u[k], v[k] * g_vScale);
		mb.AdvanceVertex();
	}
}

// SkyCraft's Box: an axis-aligned box rotated yaw (radians) about its vertical centre line, MC coords; side / top /
// bottom textures; shade per face like Minecraft's fixed face shading (when shaded).
static void Box(MeshBuilder &mb, const double mn[3], const float sz[3], float yaw, const float *side, const float *top, const float *bottom, Color base,
                uint32_t topTint, bool shaded) {
	const double cx = mn[0] + sz[0] * 0.5, cz = mn[2] + sz[2] * 0.5;
	const double c = std::cos(yaw), s = std::sin(yaw);
	auto corner = [&](int i, double out[3]) {
		const double lx = ((i & 1) ? 0.5 : -0.5) * sz[0], lz = ((i & 4) ? 0.5 : -0.5) * sz[2];
		out[0] = cx + lx * c - lz * s;
		out[1] = mn[1] + ((i & 2) ? sz[1] : 0.0);
		out[2] = cz + lx * s + lz * c;
	};
	static const int kFaces[6][4] = {{6, 7, 5, 4}, {3, 2, 0, 1}, {7, 3, 1, 5}, {2, 6, 4, 0}, {2, 3, 7, 6}, {4, 5, 1, 0}};
	static const float kShade[6] = {0.8f, 0.8f, 0.6f, 0.6f, 1.0f, 0.5f};
	for (int f = 0; f < 6; ++f) {
		double p[4][3];
		for (int k = 0; k < 4; ++k) corner(kFaces[f][k], p[k]);
		const float *uv = f == 4 ? top : f == 5 ? bottom : side;
		Color col = base;
		if (shaded) {
			float k = kShade[f];
			float r = col.r * k, g = col.g * k, b = col.b * k;
			if (f == 4 && topTint) r *= (topTint & 0xFF) / 255.0f, g *= ((topTint >> 8) & 0xFF) / 255.0f, b *= ((topTint >> 16) & 0xFF) / 255.0f;
			col = Color((uint8_t)r, (uint8_t)g, (uint8_t)b, 255);
		}
		Quad(mb, p, uv, col);
	}
}

bool SceneDrawsItems();  // scene.cpp (bridge #19): dropped items come with Minecraft's own models in the scene mesh
void WorldEntsDraw(const Vector &eye) {
	if (!g_enabled || !Read() || !EnsureMaterials()) return;
	// solid things: dropped blocks and item sprites
	int solids = 0, cracks = 0;
	const bool sceneItems = SceneDrawsItems();
	for (uint32_t i = 0; i < g_we.count; ++i) {
		const WorldEntity &e = g_we.entities[i];
		if (!InChambers(McToSrc(e.x, e.y, e.z))) continue;
		if (sceneItems && (e.kind == kWeBlock || e.kind == kWeItem)) continue;
		if (e.kind == kWeBlock) solids += 6;
		else if (e.kind == kWeItem) solids += 1;
		else if (e.kind == kWeCrack) cracks += 6;
	}
	if (solids) {
		MeshBuilder mb(g_matSolid, PrimitiveType::QUADS, solids);
		for (uint32_t i = 0; i < g_we.count; ++i) {
			const WorldEntity &e = g_we.entities[i];
			const Vector at = McToSrc(e.x, e.y, e.z);
			if (!InChambers(at)) continue;
			if (sceneItems && (e.kind == kWeBlock || e.kind == kWeItem)) continue;
			if (e.kind == kWeBlock) {
				const float s = e.scale, sz[3] = {s, s, s};
				const double mn[3] = {e.x - s * 0.5, e.y - s * 0.5, e.z - s * 0.5};
				Box(mb, mn, sz, e.yaw * (float)M_PI / 180.0f, e.uv[0], e.uv[1], e.uv[2], Lit(at, 1.0f, 0), e.tint, true);
			} else if (e.kind == kWeItem) {
				const double spin = e.yaw * M_PI / 180.0, half = e.scale * 0.5;
				const double rx = std::cos(spin) * half, rz = std::sin(spin) * half;
				const double p[4][3] = {{e.x - rx, e.y + half, e.z - rz}, {e.x + rx, e.y + half, e.z + rz}, {e.x + rx, e.y - half, e.z + rz}, {e.x - rx, e.y - half, e.z - rz}};
				Quad(mb, p, e.uv[0], Lit(at, 1.0f, 0));
			}
		}
		mb.Draw();
		++g_drawnItems;
	}
	if (cracks) {
		MeshBuilder mb(g_matCrack, PrimitiveType::QUADS, cracks);
		for (uint32_t i = 0; i < g_we.count; ++i) {
			const WorldEntity &e = g_we.entities[i];
			if (e.kind != kWeCrack || !InChambers(McToSrc(e.x, e.y, e.z))) continue;
			const double mn[3] = {e.x, e.y, e.z};
			Box(mb, mn, e.ext, 0.0f, e.uv[0], e.uv[0], e.uv[0], Color(255, 255, 255, 255), 0, false);
		}
		mb.Draw();
		++g_drawnCracks;
	}
	if (g_we.hasSelection) {
		const float g = 0.002f;
		const double lo[3] = {g_we.selMin[0] - g, g_we.selMin[1] - g, g_we.selMin[2] - g}, hi[3] = {g_we.selMax[0] + g, g_we.selMax[1] + g, g_we.selMax[2] + g};
		if (InChambers(McToSrc((lo[0] + hi[0]) * 0.5, (lo[1] + hi[1]) * 0.5, (lo[2] + hi[2]) * 0.5))) {
			// Minecraft's outline is ~2 px wide at any distance: each edge is a ribbon facing the eye, its width scaled with
			// the distance (Source lines are 1 px and vanished on the dark chamber floors, b17_place2)
			static const int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
			MeshBuilder mb(g_matLine, PrimitiveType::QUADS, 12);
			const Color col(0, 0, 0, 0x73);  // black, 45% (Minecraft's outline)
			auto corner = [&](int k) { return McToSrc((k & 1) ? hi[0] : lo[0], (k & 2) ? hi[1] : lo[1], (k & 4) ? hi[2] : lo[2]); };
			for (auto &edge : kEdges) {
				const Vector a = corner(edge[0]), b = corner(edge[1]);
				const Vector mid = (a + b) * 0.5f, toEye = eye - mid, d = b - a;
				Vector side(d.y * toEye.z - d.z * toEye.y, d.z * toEye.x - d.x * toEye.z, d.x * toEye.y - d.y * toEye.x);
				const float sl = side.Length();
				if (sl < 1e-4f) continue;
				const float w = std::fmax(0.25f, toEye.Length() * 0.0012f);
				side = side * (w / sl);
				const Vector q[4] = {a - side, b - side, b + side, a + side};
				for (auto &p : q) {
					mb.Position(p);
					mb.Color(col);
					mb.AdvanceVertex();
				}
			}
			mb.Draw();
			++g_drawnOutline;
		}
	}
}

CON_COMMAND(pcb_worldents, "pcb_worldents [0|1] - Minecraft's outline, cracks and dropped items drawn by Portal 2 in the chambers (default 1); prints counts\n") {
	if (args.ArgC() > 1) g_enabled = std::atoi(args[1]) != 0;
	g_lastRead = -1;
	Read();
	int items = 0, cracks = 0;
	for (uint32_t i = 0; i < g_we.count; ++i) items += g_we.entities[i].kind == kWeItem || g_we.entities[i].kind == kWeBlock, cracks += g_we.entities[i].kind == kWeCrack;
	console->Print("[bridge] worldents %d: %u entities (%d items, %d cracks), selection %u (%.2f %.2f %.2f)-(%.2f %.2f %.2f); drawn outline %d cracks %d items %d views\n", g_enabled, g_we.count, items,
	               cracks, g_we.hasSelection, g_we.selMin[0], g_we.selMin[1], g_we.selMin[2], g_we.selMax[0], g_we.selMax[1], g_we.selMax[2], g_drawnOutline, g_drawnCracks, g_drawnItems);
}

// ---- tests ------------------------------------------------------------------------------------
int TerrainBandSolidCount();               // terrain.cpp
bool TerrainIsSolid(int bx, int by, int bz);
void TestCheck(bool ok, const char *what, const char *detail);  // testassert.cpp
static int g_bandMark = 0;
void McNoteScriptedTurn();  // mctransit.cpp
CON_COMMAND(pcb_aim_mc, "pcb_aim_mc <mcX> <mcY> <mcZ> [eye_u=64] - (tests) turn the view to a Minecraft point from the player's eye\n") {
	if (args.ArgC() < 4) return;
	const float eyeH = args.ArgC() > 4 ? (float)std::atof(args[4]) : 64.0f;
	const Vector d = McToSrc(std::atof(args[1]), std::atof(args[2]), std::atof(args[3])) - (PlayerOrigin() + Vector(0, 0, eyeH));
	const float l = d.Length();
	if (l < 1) return;
	char buf[96];
	std::snprintf(buf, sizeof buf, "setang %.2f %.2f 0", -std::asin(d.z / l) * 180.0f / (float)M_PI, std::atan2(d.y, d.x) * 180.0f / (float)M_PI);
	McNoteScriptedTurn();
	engine.Cmd(buf);
	console->Print("[bridge] aim_mc: %.0f u away, %s\n", l, buf);
}
CON_COMMAND(pcb_band_mark, "pcb_band_mark - (tests) remember the chamber band's solid block count (pcb_assert_band_delta)\n") {
	g_bandMark = TerrainBandSolidCount();
	console->Print("[bridge] band: %d solid blocks\n", g_bandMark);
}
CON_COMMAND(pcb_assert_band_delta, "pcb_assert_band_delta <delta> - the chamber band's solid block count changed by exactly delta since pcb_band_mark\n") {
	const int want = args.ArgC() > 1 ? std::atoi(args[1]) : 1, now = TerrainBandSolidCount();
	char buf[128];
	std::snprintf(buf, sizeof buf, "%d -> %d (want %+d)", g_bandMark, now, want);
	TestCheck(now - g_bandMark == want, "chamber-band blocks placed / broken", buf);
	g_bandMark = now;
}
// Solid Minecraft blocks in the 9x9x9 around the player's feet (terrain placement checks)
static int NearSolids() {
	double x, y, z;
	SrcToMcD(PlayerOrigin(), x, y, z);
	const int bx = (int)std::floor(x), by = (int)std::floor(y), bz = (int)std::floor(z);
	int n = 0;
	for (int dy = -4; dy <= 4; ++dy)
		for (int dz = -4; dz <= 4; ++dz)
			for (int dx = -4; dx <= 4; ++dx) n += TerrainIsSolid(bx + dx, by + dy, bz + dz);
	return n;
}
static int g_nearMark = 0;
CON_COMMAND(pcb_near_mark, "pcb_near_mark - (tests) remember the solid block count around the player (pcb_assert_near_delta)\n") {
	g_nearMark = NearSolids();
	console->Print("[bridge] near: %d solid blocks\n", g_nearMark);
}
CON_COMMAND(pcb_assert_near_delta, "pcb_assert_near_delta <delta> - the solid block count around the player changed by delta since pcb_near_mark\n") {
	const int want = args.ArgC() > 1 ? std::atoi(args[1]) : 1, now = NearSolids();
	char buf[128];
	std::snprintf(buf, sizeof buf, "%d -> %d (want %+d)", g_nearMark, now, want);
	TestCheck(now - g_nearMark == want, "blocks placed / broken around the player", buf);
	g_nearMark = now;
}

CON_COMMAND(pcb_assert_outline, "pcb_assert_outline <0|1> - Minecraft's block outline is (1) or is not (0) shown in the chambers\n") {
	g_lastRead = -1;
	Read();
	const bool want = args.ArgC() > 1 && std::atoi(args[1]) != 0;
	const bool on = g_have && g_we.hasSelection && InChambers(McToSrc((g_we.selMin[0] + g_we.selMax[0]) * 0.5, (g_we.selMin[1] + g_we.selMax[1]) * 0.5, (g_we.selMin[2] + g_we.selMax[2]) * 0.5));
	char buf[160];
	std::snprintf(buf, sizeof buf, "selection %u (%.2f %.2f %.2f)-(%.2f %.2f %.2f)", g_we.hasSelection, g_we.selMin[0], g_we.selMin[1], g_we.selMin[2], g_we.selMax[0], g_we.selMax[1], g_we.selMax[2]);
	TestCheck(on == want, "block outline in the chambers", buf);
}
CON_COMMAND(pcb_assert_we, "pcb_assert_we <crack|item> <min> - (tests) Minecraft world entities of that kind in the chambers\n") {
	if (args.ArgC() < 3) return;
	g_lastRead = -1;
	Read();
	const uint32_t kind = !std::strcmp(args[1], "crack") ? kWeCrack : kWeItem;
	int n = 0;
	for (uint32_t i = 0; i < g_we.count; ++i) {
		const WorldEntity &e = g_we.entities[i];
		if ((e.kind == kind || (kind == kWeItem && e.kind == kWeBlock)) && InChambers(McToSrc(e.x, e.y, e.z))) ++n;
	}
	char buf[96];
	std::snprintf(buf, sizeof buf, "%d %s(s) of %u entities", n, args[1], g_we.count);
	TestCheck(n >= std::atoi(args[2]), "Minecraft world entities in the chambers", buf);
}

}  // namespace bridge
