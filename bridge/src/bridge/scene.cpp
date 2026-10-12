// Portalcraft bridge (bridge #19, Owner B): Minecraft's entities, block entities and particles drawn by Portal 2 in the chambers.
//
// The mod's AvatarExporter captures, every Minecraft frame, everything Minecraft's own renderers submit for the entities
// within 64 blocks of its camera (mobs, animals, dropped items, XP orbs, arrows, minecarts, boats, armor stands, item frames,
// paintings, ...), the block entities within 48 (chests, beds, signs, banners, moving pistons, ...) and all particles, as
// one triangle mesh relative to a block near the camera (kRenScene: RenScene + RenBatch[] + RenVertex[]). Textures come as
// kRenTexture (avatar.cpp keeps them; texture 0 is the block/item atlas). This is SkyCraft's scene mesh (WorldRender.cpp
// OnMesh / DrawEntities), drawn here with the material system:
//   * only outside the region box (the chamber band): inside it Minecraft's own frame (the composite) shows them;
//   * solid batches in the opaque pass (render.cpp's DrawOpaqueRenderables hook, with the band's blocks), blended ones
//     (particles, translucent render types) in the translucent pass; every 3D view, so they are seen through portals at
//     any recursion depth and depth-tested against Portal 2's world;
//   * lit by Portal 2 as the body (avatar.cpp) and the band's blocks (terrain.cpp): each object (RenVertex flags bits
//     16-31 = the mod's object tag) gets an ambient cube at its own centre (ComputeLighting for the six axis normals,
//     floored by the world light at the point, x the blocks' scale), weighted per triangle by its normal; Minecraft's own
//     terms are kept: roofs darken (sky light, lerp(0.3, 1, Curve(sky))) and block light glows warm (max'd), so
//     emissive parts (spider eyes, a lit TNT's flash, glowing particles) stay bright. Particles have no normal: the cube's
//     average at their own block.
// The light cubes are cached for 0.3 s per 10 u spot (objects that stand still cost nothing); the lit vertices are built
// once per Minecraft mesh and reused by every view of the frame.
#include "bridge.hpp"
#include "platform/platform.hpp"
#include "growth.hpp"
#include "coords.hpp"
#include "link.hpp"

#include "Modules/MaterialSystem.hpp"
#include "Modules/Tier1.hpp"
#include "Utils/SDK/MeshBuilder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace bridge {

using namespace skycraft::proto;

bool ClipHasRegion();                       // clip.cpp
bool ClipRegionContains(const Vector &p);   // clip.cpp
float RenderLightToScale(float linear);     // render.cpp
bool AvatarTexture(uint32_t id, IMaterial *&mat, IMaterial *&matT, float &uScale, float &vScale);  // avatar.cpp
void TestCheck(bool ok, const char *what, const char *detail);  // testassert.cpp
void PortalThroughPose(int i, int j, const Vector &o, const QAngle &a, Vector &oo, QAngle &oa);  // composite.cpp
Vector RenderToneScaleSet(const Vector &v);                                                     // composite.cpp

static double NowS() {
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ---- the mesh as Minecraft sent it -------------------------------------------------------------------
static double g_ox = 0, g_oy = 0, g_oz = 0;
static std::vector<RenBatch> g_batches;
static std::vector<RenVertex> g_verts;
static double g_meshAt = 0;
static uint64_t g_meshes = 0;

static bool g_enabled = true;      // pc_mc_scene
static bool g_particles = true;    // pc_mc_scene_particles
static float g_worldK = 1.0f;      // the world light floor under the ambient cube (as terrain.cpp's pcb_band_light)
static int g_lightBudget = 48;     // new light cubes per build

// terrain.cpp: kRenScene. 0 batches = nothing to show.
void SceneOnMesh(const uint8_t *p, uint32_t bytes) {
	if (bytes < sizeof(RenScene)) return;
	const auto *h = (const RenScene *)p;
	const uint64_t need = sizeof(RenScene) + (uint64_t)h->batchCount * sizeof(RenBatch) + (uint64_t)h->vertexCount * sizeof(RenVertex);
	if (bytes < need || h->batchCount > 1024 || h->vertexCount > 600000) return;
	g_ox = h->originX, g_oy = h->originY, g_oz = h->originZ;
	const auto *b = (const RenBatch *)(p + sizeof(RenScene));
	g_batches.assign(b, b + h->batchCount);
	const auto *v = (const RenVertex *)(b + h->batchCount);
	g_verts.assign(v, v + h->vertexCount);
	g_meshAt = NowS();
	++g_meshes;
}

static bool Fresh() { return g_enabled && g_meshAt > 0 && NowS() - g_meshAt < 0.5; }
// worldents.cpp: dropped items are drawn from the scene (Minecraft's own item models), not as WorldEntities sprites
bool SceneDrawsItems() { return Fresh(); }

// ---- Portal 2's light ---------------------------------------------------------------------------------
struct Cube {
	Vector c[6];  // +X -X +Y -Y +Z -Z (Source), already scaled to vertex-colour range
	Vector avg;
	double at = 0;
};
static std::unordered_map<long long, Cube> g_cubes;
PCB_GAUGE("scene.cubes", g_cubes.size());
static int g_lightCalls = 0, g_lightCallsTotal = 0, g_cubeMisses = 0;
static const Vector kAxis[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

static long long SpotKey(const Vector &p, float cell) {
	const long long x = (long long)std::floor(p.x / cell), y = (long long)std::floor(p.y / cell), z = (long long)std::floor(p.z / cell);
	return ((x & 0x1FFFFF) << 42) | ((y & 0x1FFFFF) << 21) | (z & 0x1FFFFF);
}

static Vector Scaled(const Vector &l) { return Vector(RenderLightToScale(l.x), RenderLightToScale(l.y), RenderLightToScale(l.z)); }

// The ambient cube at p (Source), cached per spot. Null when over this build's budget and nothing cached.
static const Cube *CubeAt(const Vector &p, float cell, float reach, double now) {
	const long long key = SpotKey(p, cell) * 4 + (cell > 20.0f ? 1 : 0);
	auto it = g_cubes.find(key);
	if (it != g_cubes.end() && now - it->second.at < 0.3) return &it->second;
	if (g_lightCalls >= g_lightBudget) {
		++g_cubeMisses;
		return it != g_cubes.end() ? &it->second : nullptr;
	}
	++g_lightCalls, ++g_lightCallsTotal;
	Cube cu;
	const Vector w = engine.LightAt(p) * g_worldK;
	cu.avg = Vector(0, 0, 0);
	for (int i = 0; i < 6; ++i) {
		Vector l = engine.LightAt(p + kAxis[i] * reach, kAxis[i]);
		l.x = std::fmax(l.x, w.x), l.y = std::fmax(l.y, w.y), l.z = std::fmax(l.z, w.z);
		cu.c[i] = Scaled(l);
		cu.avg = cu.avg + cu.c[i] * (1.0f / 6.0f);
	}
	cu.at = now;
	return &(g_cubes[key] = cu);
}

static float Curve(float l) { return l / (4.0f - 3.0f * l); }

// ---- the lit mesh, built once per Minecraft mesh ----------------------------------------------------------
struct LitVert {
	Vector p;
	float u, v;
	uint8_t r, g, b, a;
};
struct LitBatch {
	IMaterial *mat;
	bool blended, particle;
	bool daylight = false;  // (bridge #22) Minecraft-lit copies of region entities: drawn at tonemap scale 1
	std::vector<LitVert> v;
};
static std::vector<LitBatch> g_lit;
PCB_GAUGE("scene.lit", g_lit.size());
static uint64_t g_litMesh = ~0ull;
static int g_objects = 0, g_objectsRegion = 0, g_trisLit = 0, g_trisRegion = 0;
static int g_ghosts = 0, g_ghostTris = 0, g_ghostsTotal = 0;  // (bridge #22) objects drawn again through the portal pair
static bool g_ghostOn = true;                                 // pc_mc_scene_ghosts
// mc.cpp (bridge #22): Minecraft entities are in the chambers or on their way through the pair: the chambers' collision must
// be exported (in Chell mode too) so they stand on Portal 2's floors instead of being held where it is unknown
bool SceneChamberEntities() { return Fresh() && (g_objects > 0 || g_ghosts > 0); }
static double g_buildMs = 0, g_buildMsMax = 0;
static uint64_t g_drawsOpaque = 0, g_drawsBlended = 0, g_trisDrawn = 0;
static Vector g_firstObject;  // (diagnostics) the nearest object's centre to the main view
static float g_firstObjectD = 1e9f;

// #18's Portal 2 model lighting (bandlight.cpp): the lighting state a model standing at a point gets (ambient cube + up
// to 4 local lights), evaluated as Portal 2's vertex-lit model shader does. (Same declarations as terrain.cpp.)
struct P2Light {
	int type;
	Vector col, pos, dir;
	float range, falloff, a0, a1, a2, cosTheta, cosPhi, oneOver;
};
struct P2State {
	float cube[6][3];
	int n;
	P2Light l[4];
};
bool BandLightQuery(const std::vector<Vector> &pts, std::vector<P2State> &out);
Vector BandLightEval(const P2State &s, const Vector &p, const Vector &n, bool hasNormal);
float BandLinearToVertex(float l);  // bandlight.cpp (#18): linear light -> vertex colour for the sRGB atlas
void BlockLightsSuspend();          // blocklights.cpp (#18): our own dlights (Minecraft's torches) out of the query: their
void BlockLightsResume();           //   light is Minecraft's block light in the vertex data already

static int g_lightMode = 2;      // pc_mc_scene_light: 2 Portal 2's model lighting state (as a cube or Chell there), 1 ComputeLighting x5
static float g_stateK = 1.0f;    // linear light scale (mode 2)
static float g_stateFill = 0.0f; // never darker than this share of the state's mean ambient (bounce); 0 = off (#18's recipe)
static float g_blockK = 0.3f;    // Minecraft block light (torches) at full level, linear: as the placed blocks (terrain.cpp g_bandBlockK)
static float g_gamma = 2.2f;     // vertex colour = linear light ^ (1/gamma): the shader decodes vertex colours (#18, pcb_light_ramp)
struct StateEntry {
	P2State st;
	double at = 0;
};
static std::unordered_map<long long, StateEntry> g_states;
PCB_GAUGE("scene.states", g_states.size());
static int g_stateQueries = 0;

struct Obj {
	Vector lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
	const Cube *cube = nullptr;
	const P2State *st = nullptr;
	bool region = false;
};

// Portal 2's states at many points at once, cached per spot for 0.3 s: one batched engine call per build for what is new.
static void States(const std::vector<std::pair<Vector, float>> &spots, std::vector<const P2State *> &out, double now) {
	out.assign(spots.size(), nullptr);
	std::vector<Vector> ask;
	std::vector<long long> askKey;
	for (size_t i = 0; i < spots.size(); ++i) {
		const long long key = SpotKey(spots[i].first, spots[i].second) * 4 + (spots[i].second > 20.0f ? 1 : 0);
		auto it = g_states.find(key);
		if (it != g_states.end() && now - it->second.at < 0.3) continue;
		if ((int)ask.size() >= g_lightBudget * 4) { ++g_cubeMisses; continue; }
		if (std::find(askKey.begin(), askKey.end(), key) != askKey.end()) continue;
		ask.push_back(spots[i].first), askKey.push_back(key);
	}
	if (!ask.empty()) {
		std::vector<P2State> got;
		BlockLightsSuspend();
		const bool ok = BandLightQuery(ask, got);
		BlockLightsResume();
		if (ok)
			for (size_t k = 0; k < ask.size(); ++k) g_states[askKey[k]] = {got[k], now};
		g_stateQueries += (int)ask.size(), g_lightCalls += (int)ask.size(), g_lightCallsTotal += (int)ask.size();
	}
	for (size_t i = 0; i < spots.size(); ++i) {
		auto it = g_states.find(SpotKey(spots[i].first, spots[i].second) * 4 + (spots[i].second > 20.0f ? 1 : 0));
		if (it != g_states.end()) out[i] = &it->second.st;
	}
}

static void Build(const Vector &eye) {
	const auto t0 = std::chrono::steady_clock::now();
	const double now = NowS();
	g_lit.clear();
	g_lightCalls = 0;
	g_objects = g_objectsRegion = g_trisLit = g_trisRegion = 0;
	g_firstObjectD = 1e9f;
	const Vector org = McToSrc(g_ox, g_oy, g_oz);
	const size_t n = g_verts.size();
	static std::vector<Vector> pos;
	pos.resize(n);
	for (size_t i = 0; i < n; ++i) {
		const RenVertex &rv = g_verts[i];
		pos[i] = org + Vector(rv.x * kUnitsPerBlock, -rv.z * kUnitsPerBlock, rv.y * kUnitsPerBlock);
	}
	// the objects: bounds per tag, then their light at the centre (nearest to the eye first gets the budget)
	std::unordered_map<uint32_t, Obj> objs;
	for (size_t i = 0; i < n; ++i) {
		const uint32_t tag = g_verts[i].flags >> 16;
		if (!tag) continue;
		Obj &o = objs[tag];
		const Vector &p = pos[i];
		o.lo.x = std::fmin(o.lo.x, p.x), o.lo.y = std::fmin(o.lo.y, p.y), o.lo.z = std::fmin(o.lo.z, p.z);
		o.hi.x = std::fmax(o.hi.x, p.x), o.hi.y = std::fmax(o.hi.y, p.y), o.hi.z = std::fmax(o.hi.z, p.z);
	}
	const bool region = ClipHasRegion();
	std::vector<std::pair<float, Obj *>> order;
	order.reserve(objs.size());
	for (auto &kv : objs) {
		Obj &o = kv.second;
		const Vector c = (o.lo + o.hi) * 0.5f;
		o.region = region && ClipRegionContains(c);
		if (o.region) {
			++g_objectsRegion;
			continue;
		}
		++g_objects;
		const float d = (c - eye).Length();
		if (d < g_firstObjectD) g_firstObjectD = d, g_firstObject = c;
		order.push_back({d, &o});
	}
	std::sort(order.begin(), order.end(), [](const std::pair<float, Obj *> &a, const std::pair<float, Obj *> &b) { return a.first < b.first; });
	if (g_lightMode == 2) {
		std::vector<std::pair<Vector, float>> spots;
		for (auto &e : order) spots.push_back({(e.second->lo + e.second->hi) * 0.5f, 10.0f});
		std::vector<const P2State *> got;
		States(spots, got, now);
		for (size_t k = 0; k < order.size(); ++k) order[k].second->st = got[k];
	} else {
		for (auto &e : order) {
			Obj &o = *e.second;
			const Vector c = (o.lo + o.hi) * 0.5f;
			const float half = std::fmax(std::fmax(o.hi.x - o.lo.x, o.hi.y - o.lo.y), o.hi.z - o.lo.z) * 0.5f;
			// reach: half the object's size, at least half a block (sample outside the body, not inside Portal 2's walls)
			o.cube = CubeAt(c, 10.0f, std::fmin(std::fmax(half, 12.0f), 40.0f), now);
		}
	}
	// particles: one state per block they are in
	std::unordered_map<long long, const P2State *> partSt;
	if (g_lightMode == 2 && g_particles) {
		std::vector<std::pair<Vector, float>> spots;
		std::vector<long long> keys;
		for (const RenBatch &b : g_batches) {
			const uint32_t cnt = b.count - b.count % 3;
			if (!cnt || b.first + (uint64_t)cnt > n || ((g_verts[b.first].flags >> 4) & 7) != 0) continue;
			for (uint32_t i = b.first; i < b.first + cnt; i += 3) {
				if (g_verts[i].flags >> 16) continue;
				const Vector c = (pos[i] + pos[i + 1] + pos[i + 2]) * (1.0f / 3.0f);
				const long long key = SpotKey(c, 40.0f);
				if (partSt.count(key)) continue;
				partSt[key] = nullptr;
				spots.push_back({Vector((std::floor(c.x / 40.0f) + 0.5f) * 40.0f, (std::floor(c.y / 40.0f) + 0.5f) * 40.0f, (std::floor(c.z / 40.0f) + 0.5f) * 40.0f), 40.0f});
				keys.push_back(key);
			}
		}
		std::vector<const P2State *> got;
		States(spots, got, now);
		for (size_t k = 0; k < keys.size(); ++k) partSt[keys[k]] = got[k];
	}
	// (bridge #22) an object across a linked portal's plane (a mob or item on its way through, either side, the region's too)
	// is drawn a second time through the pair: the part past the plane comes out of the partner, as Portal 2 draws its props'
	// clones. What lies behind the partner's plane is in its wall (or under Portal 2's portal surface): hidden by depth.
	struct Ghost {
		Vector o, ex, ey, ez;  // affine: p' = o + ex p.x + ey p.y + ez p.z
		const P2State *st = nullptr;
		bool region = false;
	};
	std::unordered_map<uint32_t, std::vector<Ghost>> ghosts;
	g_ghosts = g_ghostTris = 0;
	if (g_ghostOn && (PortalLinked(0) || PortalLinked(2) || PortalLinked(4) || PortalLinked(6) || PortalLinked(8) || PortalLinked(10) || PortalLinked(12) || PortalLinked(14))) {
		std::vector<std::pair<Vector, float>> spots;
		std::vector<std::pair<uint32_t, size_t>> spotOf;
		for (auto &kv : objs) {
			const Obj &o = kv.second;
			const Vector c = (o.lo + o.hi) * 0.5f, half = (o.hi - o.lo) * 0.5f;
			for (int i = 0; i < kPortalSlots; ++i) {
				if (!PortalLinked(i)) continue;
				const Vector &n = g_portalNormal[i];
				const float d = Vector::DotProduct(c - g_portalPos[i], n);
				const float r = std::fabs(n.x) * half.x + std::fabs(n.y) * half.y + std::fabs(n.z) * half.z;
				if (d - r >= 0.0f || d + r <= 0.0f) continue;  // not across the plane
				const Vector q = c - g_portalPos[i] - n * d, up = g_portalUp[i], right = Vector(n).Cross(up);
				const float ext = std::fmax(std::fmax(half.x, half.y), half.z);
				if (std::fabs(Vector::DotProduct(q, right)) > g_portalHalfW[i] + ext || std::fabs(Vector::DotProduct(q, up)) > g_portalHalfH[i] + ext) continue;
				Ghost g;
				QAngle a{0, 0, 0}, oa;
				Vector px, py, pz;
				PortalThroughPose(i, i ^ 1, Vector(0, 0, 0), a, g.o, oa);
				PortalThroughPose(i, i ^ 1, Vector(1, 0, 0), a, px, oa);
				PortalThroughPose(i, i ^ 1, Vector(0, 1, 0), a, py, oa);
				PortalThroughPose(i, i ^ 1, Vector(0, 0, 1), a, pz, oa);
				g.ex = px - g.o, g.ey = py - g.o, g.ez = pz - g.o;
				const Vector gc = g.o + g.ex * c.x + g.ey * c.y + g.ez * c.z;
				// lit as the object itself is (Portal 2 lights a prop across a portal by its own origin, both halves alike): a
				// region object's copy in the chamber keeps Minecraft's daylight, a chamber object's copy keeps Portal 2's light
				// there; the whole entity changes light when it is carried through
				g.region = o.region;
				auto &list = ghosts[kv.first];
				list.push_back(g);
				++g_ghosts;
				if (!g.region && g_lightMode == 2) spots.push_back({c, 10.0f}), spotOf.push_back({kv.first, list.size() - 1});
			}
		}
		if (!spots.empty()) {
			std::vector<const P2State *> got;
			States(spots, got, now);
			for (size_t k = 0; k < spots.size(); ++k) ghosts[spotOf[k].first][spotOf[k].second].st = got[k];
		}
		g_ghostsTotal += g_ghosts;
	}
	static Cube neutral;
	if (neutral.at == 0) {
		for (int i = 0; i < 6; ++i) neutral.c[i] = Vector(0.6f, 0.6f, 0.6f);
		neutral.avg = Vector(0.6f, 0.6f, 0.6f), neutral.at = 1;
	}
	for (const RenBatch &b : g_batches) {
		const uint32_t cnt = b.count - b.count % 3;
		if (!cnt || b.first + (uint64_t)cnt > n) continue;
		IMaterial *mat = nullptr, *matT = nullptr;
		float us = 1, vs = 1;
		if (!AvatarTexture(b.texture, mat, matT, us, vs)) continue;
		const bool particle = ((g_verts[b.first].flags >> 4) & 7) == 0;
		if (particle && !g_particles) continue;
		LitBatch lb, day;
		lb.blended = day.blended = (b.flags & 1) != 0;
		lb.particle = day.particle = particle;
		lb.mat = day.mat = lb.blended ? matT : mat;
		day.daylight = true;
		lb.v.reserve(cnt);
		// one triangle, lit: Portal 2's state (st), the ambient cube (cu), or daylight (a ghost in the region, where Minecraft
		// lights its own world)
		auto emit = [&](uint32_t i, const Vector *P, const Cube *cu, const P2State *st, bool daylight) {
			const Vector &p0 = P[0], &p1 = P[1], &p2 = P[2];
			const Vector c = (p0 + p1 + p2) * (1.0f / 3.0f);
			Vector nrm(0, 0, 1);
			bool hasN = false;
			if (!particle) {
				nrm = (p1 - p0).Cross(p2 - p0);
				const float len = nrm.Length();
				if (len > 1e-6f) nrm = nrm * (1.0f / len), hasN = true;
			}
			Vector light;
			bool encode = false;
			if (daylight) {
				// Minecraft's own colours (drawn at tonemap scale 1, the vertex colour applied in display space as Minecraft's
				// shader does): full daylight x Minecraft's face shade (top 1, sides 0.8 / 0.6, bottom 0.5), x roof below
				light = Vector(1, 1, 1);
				if (hasN) light = light * (nrm.z > 0.5f ? 1.0f : nrm.z < -0.5f ? 0.5f : std::fabs(nrm.x) > std::fabs(nrm.y) ? 0.6f : 0.8f);
				encode = false;
			} else if (g_lightMode == 2 && !st) {
				light = Vector(0.05f, 0.05f, 0.05f) * g_stateK;  // (no state yet: dim, encoded like the rest)
				encode = true;
			} else if (g_lightMode == 2 && st) {
				// as Portal 2 lights a model there; a face never darker than a share of the mean ambient (bounce)
				light = BandLightEval(*st, c + nrm * 1.0f, nrm, hasN);
				float mean[3] = {0, 0, 0};
				for (int a = 0; a < 6; ++a)
					for (int k = 0; k < 3; ++k) mean[k] += st->cube[a][k] / 6.0f;
				light = Vector(std::fmax(light.x, mean[0] * g_stateFill), std::fmax(light.y, mean[1] * g_stateFill), std::fmax(light.z, mean[2] * g_stateFill)) * g_stateK;
				encode = true;
			} else {
				if (!cu) cu = &neutral;
				light = cu->avg;
				if (hasN) {
					// the ambient cube, Source's way: squared normal components pick each axis' side
					const Vector &cx = cu->c[nrm.x >= 0 ? 0 : 1], &cy = cu->c[nrm.y >= 0 ? 2 : 3], &cz = cu->c[nrm.z >= 0 ? 4 : 5];
					light = cx * (nrm.x * nrm.x) + cy * (nrm.y * nrm.y) + cz * (nrm.z * nrm.z);
				}
			}
			++g_trisLit;
			for (int k = 0; k < 3; ++k) {
				const RenVertex &rv = g_verts[i + k];
				const float block = Curve((rv.light & 0xF) / 15.0f), sky = Curve(((rv.light >> 8) & 0xF) / 15.0f);
				const float roof = 0.3f + 0.7f * sky;
				float lr, lg, lbl;
				if (encode) {
					// #18's recipe (bandlight.cpp): Portal 2's linear light x roof, max'd with warm block light at 0.3, each channel
					// through BandLinearToVertex (the shader decodes the vertex colour as sRGB and applies the tonemap scale)
					const float bk = block * g_blockK;
					lr = BandLinearToVertex(std::fmax(light.x * roof, bk));
					lg = BandLinearToVertex(std::fmax(light.y * roof, bk * 0.85f));
					lbl = BandLinearToVertex(std::fmax(light.z * roof, bk * 0.65f));
				} else {
					lr = std::fmax(light.x * roof, block), lg = std::fmax(light.y * roof, block * 0.85f), lbl = std::fmax(light.z * roof, block * 0.65f);
				}
				const uint32_t col = rv.color;
				auto ch = [](uint32_t v, float l) { return (uint8_t)std::fmin(255.0f, v * l); };
				LitVert lv;
				lv.p = P[k];
				lv.u = rv.u * us, lv.v = rv.v * vs;
				lv.r = ch(col & 0xFF, lr), lv.g = ch((col >> 8) & 0xFF, lg), lv.b = ch((col >> 16) & 0xFF, lbl), lv.a = (uint8_t)(col >> 24);
				(daylight ? day : lb).v.push_back(lv);
			}
		};
		for (uint32_t i = b.first; i < b.first + cnt; i += 3) {
			const Vector P[3] = {pos[i], pos[i + 1], pos[i + 2]};
			const Vector c = (P[0] + P[1] + P[2]) * (1.0f / 3.0f);
			const uint32_t tag = g_verts[i].flags >> 16;
			const Cube *cu = nullptr;
			const P2State *st = nullptr;
			if (tag) {
				auto gh = ghosts.find(tag);
				if (gh != ghosts.end())
					for (const Ghost &g : gh->second) {
						Vector Q[3];
						for (int k = 0; k < 3; ++k) Q[k] = g.o + g.ex * P[k].x + g.ey * P[k].y + g.ez * P[k].z;
						// (the mesh's winding is kept: a half turn is a rotation, no mirror)
						emit(i, Q, nullptr, g.st, g.region);
						++g_ghostTris;
					}
				auto it = objs.find(tag);
				if (it == objs.end() || it->second.region) {
					++g_trisRegion;
					continue;
				}
				cu = it->second.cube, st = it->second.st;
			} else {
				if (region && ClipRegionContains(c)) {
					++g_trisRegion;
					continue;
				}
				if (g_lightMode == 2) {
					auto it = partSt.find(SpotKey(c, 40.0f));
					st = it != partSt.end() ? it->second : nullptr;
				} else {
					cu = CubeAt(c, 40.0f, 12.0f, now);
				}
			}
			emit(i, P, cu, st, false);
		}
		if (!lb.v.empty()) g_lit.push_back(std::move(lb));
		if (!day.v.empty()) g_lit.push_back(std::move(day));
	}
	if (g_cubes.size() > 4096) {
		for (auto it = g_cubes.begin(); it != g_cubes.end();) it = now - it->second.at > 2.0 ? g_cubes.erase(it) : std::next(it);
	}
	if (g_states.size() > 4096) {
		for (auto it = g_states.begin(); it != g_states.end();) it = now - it->second.at > 2.0 ? g_states.erase(it) : std::next(it);
		if (g_states.size() > 16384) g_states.clear();  // (bridge #61) a hard bound: entries are only good for 0.3 s anyway
	}
	g_buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	g_buildMsMax = std::fmax(g_buildMsMax, g_buildMs);
}

// render.cpp: the opaque pass (solid batches) and the translucent pass (blended ones) of every 3D view.
void SceneDraw(const CViewSetup *view, bool blendedPass) {
	if (!Fresh() || g_verts.empty()) return;
	if (g_litMesh != g_meshes) g_litMesh = g_meshes, Build(view->origin);
	bool any = false;
	for (const LitBatch &lb : g_lit) {
		if (lb.blended != blendedPass || !lb.mat) continue;
		const uint32_t n = (uint32_t)lb.v.size();
		Vector tm(1, 1, 1);
		if (lb.daylight) tm = RenderToneScaleSet(Vector(1, 1, 1));
		for (uint32_t done = 0; done < n;) {
			const uint32_t cnt = std::min<uint32_t>(n - done, 9000);
			MeshBuilder mb(lb.mat, PrimitiveType::TRIANGLES, (int)(cnt / 3));
			for (uint32_t i = done; i < done + cnt; ++i) {
				const LitVert &v = lb.v[i];
				mb.Position(v.p);
				mb.Color(Color(v.r, v.g, v.b, v.a));
				mb.TexCoord(0, v.u, v.v);
				mb.AdvanceVertex();
			}
			mb.Draw();
			done += cnt;
			g_trisDrawn += cnt / 3;
		}
		if (lb.daylight) RenderToneScaleSet(tm);
		any = true;
	}
	if (any) ++(blendedPass ? g_drawsBlended : g_drawsOpaque);
}

// ---- contact shadows: Minecraft's soft round shadow under each mob, player and item (kWeShadow in WorldEntities) -------------
static bool g_shadows = true;  // pc_mc_scene_shadows
static IMaterial *g_shadowMat = nullptr;
static WorldEntities g_we;
static double g_weAt = -1;
static int g_shadowsDrawn = 0;
static bool ReadWe() {
	const double now = NowS();
	if (now - g_weAt < 0.003) return true;
	g_weAt = now;
	uint8_t *base = link::Base();
	if (!base || !link::McAlive()) return false;
	auto *src = (const WorldEntities *)(base + kOffWorldEntities);
	for (int i = 0; i < 50; ++i) {
		const uint32_t a = __atomic_load_n(&src->seq, __ATOMIC_ACQUIRE);
		if (a & 1) continue;
		std::memcpy(&g_we, src, sizeof g_we);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(&src->seq, __ATOMIC_ACQUIRE) == a) {
			if (g_we.count > kMaxWorldEntities) g_we.count = kMaxWorldEntities;
			return a != 0;
		}
	}
	return true;
}
static bool ShadowMaterial() {
	if (g_shadowMat) return true;
	// Minecraft's shadow.png is a soft disc: a radial alpha ramp, black
	const int n = 64;
	std::vector<uint8_t> bgra(n * n * 4, 0);
	for (int y = 0; y < n; ++y)
		for (int x = 0; x < n; ++x) {
			const float dx = (x + 0.5f) / n * 2 - 1, dy = (y + 0.5f) / n * 2 - 1, r = std::sqrt(dx * dx + dy * dy);
			const float a = r >= 1 ? 0.0f : std::pow(1.0f - r, 0.7f);
			bgra[(y * n + x) * 4 + 3] = (uint8_t)std::lround(a * 255);
		}
	if (!materialSystem->CreateTexture("_pcb_scene_shadow", n, n, bgra.data(), 0)) return false;
	auto kv = new KeyValues("UnlitGeneric");
	kv->SetString("$basetexture", "_pcb_scene_shadow");
	kv->SetInt("$vertexcolor", 1);
	kv->SetInt("$vertexalpha", 1);
	kv->SetInt("$translucent", 1);
	kv->SetInt("$nocull", 1);
	g_shadowMat = materialSystem->CreateMaterial("_pcb_scene_shadow_mat", kv);
	return g_shadowMat != nullptr;
}
// render.cpp (translucent pass, after SceneDraw): Minecraft draws a shadow under every entity; the chambers' entities get one
// on Portal 2's floor (or the placed block they stand on). Alpha 0.5 at the feet, fading as the ground falls away (1.5 blocks).
bool McViewRedirect(Vector &t, float r[3][3]);  // mc.cpp (bridge #70)
Vector McFeetRawSrc();                          // mc.cpp
void SceneShadowsDraw(const CViewSetup *view) {
	if (!g_shadows || !Fresh() || !ReadWe() || !ShadowMaterial()) return;
	const bool region = ClipHasRegion();
	int n = 0;
	for (uint32_t i = 0; i < g_we.count; ++i) n += g_we.entities[i].kind == kWeShadow;
	if (!n) return;
	MeshBuilder mb(g_shadowMat, PrimitiveType::QUADS, n);
	int drawn = 0;
	for (uint32_t i = 0; i < g_we.count; ++i) {
		const WorldEntity &e = g_we.entities[i];
		if (e.kind != kWeShadow) continue;
		const Vector feet = McToSrc(e.x, e.y, e.z);
		if (region && ClipRegionContains(feet)) continue;
		// (bridge #70) the Minecraft player's own shadow (F5) while the view is redirected (a point_viewproxy): drawn where the camera sees the body, on the
		// ground under it in the redirected frame
		Vector rt;
		float rr[3][3];
		const Vector mcFeet = McFeetRawSrc();
		const bool redirected = McViewRedirect(rt, rr) && std::fabs(feet.x - mcFeet.x) < 8.0f && std::fabs(feet.y - mcFeet.y) < 8.0f && std::fabs(feet.z - mcFeet.z) < 24.0f;
		auto xf = [&](const Vector &p) {
			return Vector(rr[0][0] * p.x + rr[0][1] * p.y + rr[0][2] * p.z + rt.x, rr[1][0] * p.x + rr[1][1] * p.y + rr[1][2] * p.z + rt.y,
			              rr[2][0] * p.x + rr[2][1] * p.y + rr[2][2] * p.z + rt.z);
		};
		CGameTrace tr;
		float ground, drop;
		if (redirected) {
			engine.Trace(xf(feet + Vector(0, 0, 4)), xf(feet - Vector(0, 0, 60)), Vector(0, 0, 0), MASK_SOLID_BRUSHONLY, tr, true);
			drop = tr.fraction < 1.0f && !tr.startsolid ? 64.0f * tr.fraction - 4.0f : 0.0f;  // along the (rotated) down axis
			ground = feet.z - drop;
		} else {
			engine.Trace(feet + Vector(0, 0, 4), feet - Vector(0, 0, 60), Vector(0, 0, 0), MASK_SOLID_BRUSHONLY, tr, true);
			ground = tr.fraction < 1.0f && !tr.startsolid ? std::fmax(tr.endpos.z, feet.z - 60) : feet.z;
			drop = feet.z - ground;
		}
		const float a = 0.5f * std::fmax(0.0f, 1.0f - drop / 60.0f);
		if (a <= 0.01f) continue;
		const float r = std::fmax(e.scale, 0.4f) * 0.5f * kUnitsPerBlock * 1.1f;
		const float z = (drop < 1.0f ? feet.z : ground) + 0.6f;
		const Vector c(feet.x, feet.y, z);
		Vector q[4] = {c + Vector(-r, -r, 0), c + Vector(r, -r, 0), c + Vector(r, r, 0), c + Vector(-r, r, 0)};
		if (redirected)
			for (Vector &v : q) v = xf(v);
		const float uv[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
		for (int k = 0; k < 4; ++k) {
			mb.Position(q[k]);
			mb.Color(Color(0, 0, 0, (uint8_t)std::lround(a * 255)));
			mb.TexCoord(0, uv[k][0], uv[k][1]);
			mb.AdvanceVertex();
		}
		++drawn;
	}
	// (unused quads are degenerate at the origin with alpha 0)
	for (int k = drawn; k < n; ++k)
		for (int j = 0; j < 4; ++j) mb.Position(Vector(0, 0, 0)), mb.Color(Color(0, 0, 0, 0)), mb.TexCoord(0, 0, 0), mb.AdvanceVertex();
	mb.Draw();
	g_shadowsDrawn += drawn;
}
CON_COMMAND(pc_mc_scene_shadows, "pc_mc_scene_shadows 0|1 - Minecraft's round contact shadows under entities in the chambers (default 1)\n") {
	if (args.ArgC() > 1) g_shadows = std::atoi(args[1]) != 0;
	console->Print("[bridge] scene shadows %d (%d drawn so far)\n", g_shadows, g_shadowsDrawn);
}

// ---- commands -------------------------------------------------------------------------------------------------
CON_COMMAND(pc_mc_scene, "pc_mc_scene 0|1 - draw Minecraft's entities, block entities and particles in the chambers (default 1)\n") {
	if (args.ArgC() > 1) g_enabled = std::atoi(args[1]) != 0;
	console->Print("[bridge] Minecraft scene (entities, particles) %s\n", g_enabled ? "on" : "off");
}
CON_COMMAND(pc_mc_scene_ghosts, "pc_mc_scene_ghosts 0|1 - (bridge #22) a Minecraft entity across a portal's plane is drawn again through the pair (default 1)\n") {
	if (args.ArgC() > 1) g_ghostOn = std::atoi(args[1]) != 0, g_litMesh = ~0ull;
	console->Print("[bridge] scene ghosts %d: %d objects in the last build (%d triangles), %d builds' worth in all\n", g_ghostOn, g_ghosts, g_ghostTris, g_ghostsTotal);
}
// (tests) pcb_assert_scene_ghosts reset | <min>: entities drawn through the pair (across a portal's plane) since the reset
CON_COMMAND(pcb_assert_scene_ghosts, "pcb_assert_scene_ghosts reset | <min_builds> - (bridge #22) builds with an entity drawn through the portal pair since the reset\n") {
	static int base = 0;
	if (args.ArgC() > 1 && !std::strcmp(args[1], "reset")) return void(base = g_ghostsTotal);
	const int want = args.ArgC() > 1 ? std::atoi(args[1]) : 1;
	char buf[160];
	std::snprintf(buf, sizeof buf, "%d entity copies through the pair since the reset (want >= %d), last build %d objects %d triangles", g_ghostsTotal - base, want, g_ghosts, g_ghostTris);
	TestCheck(g_ghostsTotal - base >= want, "Minecraft entities drawn through the portal pair", buf);
}
CON_COMMAND(pc_mc_scene_particles, "pc_mc_scene_particles 0|1 - draw Minecraft's particles in the chambers (default 1)\n") {
	if (args.ArgC() > 1) g_particles = std::atoi(args[1]) != 0, g_litMesh = ~0ull;
	console->Print("[bridge] Minecraft particles %s\n", g_particles ? "on" : "off");
}
CON_COMMAND(pc_mc_scene_light, "pc_mc_scene_light <mode 2|1> [k] [fill] [block_k] - Portal 2's light on Minecraft entities: 2 its model lighting state (default; k 1, fill 0, block 0.3), 1 ComputeLighting x5 (k = world floor)\n") {
	if (args.ArgC() > 1) g_lightMode = std::atoi(args[1]) == 1 ? 1 : 2;
	if (args.ArgC() > 2) (g_lightMode == 2 ? g_stateK : g_worldK) = (float)std::atof(args[2]);
	if (args.ArgC() > 3) g_stateFill = (float)std::atof(args[3]);
	if (args.ArgC() > 4) g_blockK = (float)std::atof(args[4]);
	g_cubes.clear(), g_states.clear(), g_litMesh = ~0ull;
	console->Print("[bridge] scene light: mode %d, k %.2f, fill %.2f, block %.2f, world floor x%.2f\n", g_lightMode, g_stateK, g_stateFill, g_blockK, g_worldK);
}
// (bridge #61) A new level: the cached light of the old map's cubes and states is gone (they are keyed by position and lit by Portal 2's lighting there).
void SceneLevelReset() { g_cubes.clear(), g_states.clear(), g_litMesh = ~0ull; }
static void Status() {
	size_t tris = 0, blendedTris = 0, particleTris = 0;
	for (auto &b : g_lit) (b.blended ? blendedTris : tris) += b.v.size() / 3, particleTris += b.particle ? b.v.size() / 3 : 0;
	const Vector org = McToSrc(g_ox, g_oy, g_oz);
	console->Print("[bridge] scene: %s, mesh %.2f s old (%llu meshes, %zu batches, %zu vertices), origin MC (%.0f %.0f %.0f) = (%.0f %.0f %.0f); objects %d drawn + %d in the region, "
	               "lit triangles %d (solid %zu, blended %zu, particles %zu), region-skipped %d; light cubes %zu cached, %d calls last build (%d total, %d over budget), build %.2f ms (max %.2f); "
	               "draws opaque %llu blended %llu, %llu triangles drawn; nearest object at (%.0f %.0f %.0f) %.0f u from the eye\n",
	               Fresh() ? "live" : "stale/off", g_meshAt > 0 ? NowS() - g_meshAt : -1.0, (unsigned long long)g_meshes, g_batches.size(), g_verts.size(), g_ox, g_oy, g_oz, org.x, org.y,
	               org.z, g_objects, g_objectsRegion, g_trisLit, tris, blendedTris, particleTris, g_trisRegion, g_cubes.size(), g_lightCalls, g_lightCallsTotal, g_cubeMisses, g_buildMs,
	               g_buildMsMax, (unsigned long long)g_drawsOpaque, (unsigned long long)g_drawsBlended, (unsigned long long)g_trisDrawn, g_firstObject.x, g_firstObject.y, g_firstObject.z,
	               g_firstObjectD);
}
CON_COMMAND(pcb_scene_status, "pcb_scene_status - the Minecraft scene mesh (entities, particles) as drawn in the chambers\n") { Status(); }
// pcb_assert_scene <min_objects> [min_draws_since_reset] | reset
CON_COMMAND(pcb_assert_scene, "pcb_assert_scene reset | <min_objects> [min_opaque_draws] - Minecraft entities drawn in the chambers\n") {
	static uint64_t base = 0;
	if (args.ArgC() > 1 && !std::strcmp(args[1], "reset")) return void(base = g_drawsOpaque);
	const int minObj = args.ArgC() > 1 ? std::atoi(args[1]) : 1;
	const int minDraws = args.ArgC() > 2 ? std::atoi(args[2]) : 1;
	char buf[220];
	std::snprintf(buf, sizeof buf, "%d objects lit in the chambers (%d in the region), %d triangles, %llu opaque view draws since the reset, mesh %s", g_objects, g_objectsRegion,
	              g_trisLit, (unsigned long long)(g_drawsOpaque - base), Fresh() ? "live" : "stale");
	TestCheck(Fresh() && g_objects >= minObj && (int)(g_drawsOpaque - base) >= minDraws, "Minecraft entities in the chamber", buf);
	Status();
}

// (tests) pcb_scene_hang <item_frame|painting|glow_item_frame> <left> <up> <fwd>: a hanging entity facing the player, left/up/fwd
// blocks from its feet (along the view's yaw, snapped to a side), on an oak planks block placed behind it
CON_COMMAND(pcb_scene_hang, "pcb_scene_hang <item_frame|painting> <left> <up> <fwd> - (tests) hang an item frame / painting facing the player\n") {
	if (args.ArgC() < 5) return;
	const float yaw = engine.ViewAngles().y;
	// snap the forward direction to a side: Source +X = MC east, +Y = MC north
	int q = (int)std::lround(yaw / 90.0f) & 3;  // 0 +X, 1 +Y, 2 -X, 3 -Y
	static const int fx[4] = {1, 0, -1, 0}, fy[4] = {0, 1, 0, -1};
	const Vector f((float)fx[q], (float)fy[q], 0), l(-f.y, f.x, 0);
	const Vector at = PlayerOrigin() + (f * (float)std::atof(args[4]) + l * (float)std::atof(args[2]) + Vector(0, 0, (float)std::atof(args[3]))) * kUnitsPerBlock + Vector(0, 0, 4);
	double x, y, z;
	SrcToMcD(at, x, y, z);
	const int bx = (int)std::floor(x), by = (int)std::floor(y), bz = (int)std::floor(z);
	// MC: forward (+X east, +Y = -Z north); the entity faces back towards the player
	const int mfx = fx[q], mfz = -fy[q];
	const int sx = bx + mfx, sz = bz + mfz;
	// facing toward the player = -forward. 3D ids: 2 north(-Z) 3 south(+Z) 4 west(-X) 5 east(+X); 2D: 0 south 1 west 2 north 3 east
	int id3 = 0, id2 = 0;
	if (-mfx == 1) id3 = 5, id2 = 3;
	else if (-mfx == -1) id3 = 4, id2 = 1;
	else if (-mfz == 1) id3 = 3, id2 = 0;
	else id3 = 2, id2 = 2;
	char buf[400];
	std::snprintf(buf, sizeof buf, "pcb_mc_cmd setblock %d %d %d minecraft:oak_planks", sx, by, sz);
	engine.Cmd(buf);
	if (!std::strcmp(args[1], "painting"))
		std::snprintf(buf, sizeof buf, "pcb_mc_cmd summon minecraft:painting %d %d %d {facing:%db,variant:\"minecraft:kebab\"}", bx, by, bz, id2);
	else
		std::snprintf(buf, sizeof buf, "pcb_mc_cmd summon minecraft:%s %d %d %d {Facing:%db,Fixed:1b,Item:{id:\"minecraft:diamond_sword\",count:1}}", args[1], bx, by, bz, id3);
	engine.Cmd(buf);
	console->Print("[bridge] scene hang: %s at MC %d %d %d facing %d/%d, support %d %d %d\n", args[1], bx, by, bz, id3, id2, sx, by, sz);
}

// (tests, parity sheets) Minecraft's own view of the level from the player's camera: #18's NativeShot in the mod saves
// cache/bridge/mcnative/mcnative_<name>.png within ~10 Minecraft frames. Pair it with Portal 2's `screenshot <name>`.
CON_COMMAND(pcb_scene_native, "pcb_scene_native <name> - (tests) Minecraft saves its own view as cache/bridge/mcnative/mcnative_<name>.png\n") {
	if (args.ArgC() < 2) return;
	char dir[512], path[600];
	std::snprintf(dir, sizeof dir, "%s/mcnative", CacheDir().c_str());
	if (!plat::MakeDirs(dir)) return;
	std::snprintf(path, sizeof path, "%s/request", dir);
	if (FILE *f = std::fopen(path, "w")) std::fprintf(f, "%s\n", args[1]), std::fclose(f);
	console->Print("[bridge] Minecraft native view requested: %s\n", args[1]);
}

}  // namespace bridge
