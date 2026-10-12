// Portalcraft bridge (bridge #24): the collision overlay. Draws what Minecraft's collision copy holds around the player
// as translucent, depth-tested wireframe boxes inside Portal 2's view, from what the export actually sent (collfield.cpp
// g_lastSent, patches merged): the truth Minecraft collides with, not a re-query.
//   pc_show_collision 0|1|2   1: the solid 1/8-block voxels within ~8 blocks, merged into boxes (white)
//                             2: coloured by what in Portal 2 is there now:
//                                world brush = cyan, static prop = green, movable entity = its own hue + its name drawn
//                                above it, playerclip = magenta, NOTHING there now (stale / invisible voxels) = red,
//                                not classified yet = white; region outlines: grey = not sent yet, yellow = patch
//                                pending; the live moving platform (sent separately) = blue
//   pcb_collision_shot <name> [mode=2]  (tests) the overlay on, then cache/bridge/glhud_<name>.png and a legend
//                                        cache/bridge/glhud_<name>.txt; the mode is restored afterwards
// Mode 2's classification only colours the boxes; it is per voxel (point contents, then entity traces), budgeted over
// frames and cached per block until the block's sent bits change.
#include "bridge.hpp"
#include "coords.hpp"
#include "link.hpp"

#include "Modules/MaterialSystem.hpp"
#include "Modules/Tier1.hpp"
#include "Utils/SDK/MeshBuilder.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bridge {

using namespace skycraft::proto;

bool McModeActive();                                 // mc.cpp
Vector McFeetSrc();                                  // mc.cpp
void CollisionFieldSentIn(int bx0, int by0, int bz0, int bx1, int by1, int bz1, std::vector<ColBlock> &out);  // collfield.cpp
int CollisionFieldRegionState(int rx, int ry, int rz);
int CollisionFieldRegionSize();
bool CollisionFieldPlatformBoxes(std::vector<std::pair<Vector, Vector>> &out);
void EntLiveSet(std::unordered_set<void *> &out);    // ents.cpp
bool FontPixel(char ch, int x, int y);               // bugmark.cpp

namespace {

double NowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

int g_mode = 0;
int g_radH = 8, g_radV = 6;  // blocks
IMaterial *g_mat = nullptr;

// voxel classes (a cell's value in the grid; movable entities from kMov on, one per entity)
enum : uint8_t { kEmpty = 0, kWorld = 1, kStatic = 2, kClip = 3, kNothing = 4, kUnclassified = 5, kMov = 6 };

struct Box {
	Vector mn, mx;  // Source
	uint8_t cls;
};
std::vector<Box> g_boxes;
struct RegionBox {
	Vector mn, mx;
	int state;
};
std::vector<RegionBox> g_regions;
std::vector<std::pair<Vector, Vector>> g_platBoxes;
struct Label {
	Vector at;
	std::string text;
	uint8_t cls;
};
std::vector<Label> g_labels;

// movable entities seen by the classifier: index -> entity, name
std::vector<std::pair<void *, std::string>> g_movEnts;
uint8_t MovIndex(void *e) {
	for (size_t i = 0; i < g_movEnts.size(); ++i)
		if (g_movEnts[i].first == e) return (uint8_t)(kMov + i);
	if (g_movEnts.size() >= 249) return kNothing;
	std::string n = EntKV(e, "targetname");
	if (n.empty()) n = EntKV(e, "classname");
	g_movEnts.push_back({e, n});
	return (uint8_t)(kMov + g_movEnts.size() - 1);
}

// per-block classification cache: key = block, value = the bits it was made for and a class per solid voxel (512)
struct BlockClass {
	uint64_t bits[8];
	std::vector<uint8_t> cls;  // 512, kEmpty where not solid
	bool done = false;
};
struct Key {
	int x, y, z;
	bool operator==(const Key &o) const { return x == o.x && y == o.y && z == o.z; }
};
struct KeyHash {
	size_t operator()(const Key &k) const { return ((size_t)k.x * 73856093u) ^ ((size_t)k.y * 19349663u) ^ ((size_t)k.z * 83492791u); }
};
std::unordered_map<Key, BlockClass, KeyHash> g_classes;
std::vector<Key> g_queue;  // blocks to classify, nearest first
std::unordered_set<void *> g_serverEnts;
double g_serverEntsAt = -1;
int g_classified = 0;
double g_classMs = 0;

void *g_playerEnt = nullptr;
bool PointEntity(const Vector &p, float half, void *&hit) {
	struct Filter : public ITraceFilter {
		void *skip;
		virtual bool ShouldHitEntity(void *e, int) { return e != skip; }
		virtual TraceType_t GetTraceType() const { return TRACE_EVERYTHING; }
	} filter;
	filter.skip = g_playerEnt;
	Ray_t ray;
	ray.m_Delta = VectorAligned(0, 0, 0);
	ray.m_IsSwept = false;
	ray.m_Extents = VectorAligned(half, half, half);
	ray.m_IsRay = half < 1e-3f;
	ray.m_StartOffset = VectorAligned(0, 0, 0);
	ray.m_Start = VectorAligned(p.x, p.y, p.z);
	ray.m_pWorldAxisTransform = nullptr;
	CGameTrace tr;
	std::memset(&tr, 0, sizeof tr);
	engine.TraceRay(engine.traceServer->ThisPtr(), ray, MASK_PLAYERSOLID, &filter, &tr);
	hit = tr.m_pEnt;
	return tr.startsolid || tr.allsolid;
}
// what is at a voxel now: world contents first (brushes), then entities (a server entity = movable / brush entity, otherwise a
// static prop); a point first, then the voxel's box (geometry thinner than a voxel)
uint8_t ClassifyVoxel(const Vector &p) {
	for (int pass = 0; pass < 2; ++pass) {
		const float h = pass == 0 ? 0.0f : 2.45f;
		int c = 0;
		if (pass == 0) c = engine.GetPointContentsWorld(engine.traceServer->ThisPtr(), p, MASK_ALL);
		else {
			CGameTrace tr;
			engine.Trace(p, p, Vector(h, h, h), MASK_SOLID, tr, true);
			if (tr.startsolid || tr.allsolid) c |= CONTENTS_SOLID;
			else {
				engine.Trace(p, p, Vector(h, h, h), CONTENTS_PLAYERCLIP, tr, true);
				if (tr.startsolid || tr.allsolid) c |= CONTENTS_PLAYERCLIP;
			}
		}
		if (c & MASK_SOLID) return kWorld;
		if (c & CONTENTS_PLAYERCLIP) return kClip;
		void *e = nullptr;
		if (PointEntity(p, h, e)) {
			if (!e || !g_serverEnts.count(e)) return kStatic;
			// a playerclip-only brush entity (a door's player clip) reads as clip
			const std::string cls = EntKV(e, "classname");
			if (cls == "worldspawn") return kWorld;
			CGameTrace tr;
			engine.Trace(p, p, Vector(h, h, h), MASK_SOLID, tr, false);
			if (!(tr.startsolid || tr.allsolid)) return kClip;
			return MovIndex(e);
		}
	}
	return kNothing;
}
void ClassifyStep(double budgetMs) {
	if (g_queue.empty()) return;
	const auto t0 = std::chrono::steady_clock::now(), dl = t0 + std::chrono::microseconds((long long)(budgetMs * 1000));
	if (NowS() - g_serverEntsAt > 1.0) {
		g_serverEntsAt = NowS(), EntLiveSet(g_serverEnts);
		auto v = EntFind("player");
		g_playerEnt = v.empty() ? nullptr : v[0];
	}
	while (!g_queue.empty() && std::chrono::steady_clock::now() < dl) {
		const Key k = g_queue.back();
		g_queue.pop_back();
		auto it = g_classes.find(k);
		if (it == g_classes.end() || it->second.done) continue;
		BlockClass &bc = it->second;
		bc.cls.assign(512, kEmpty);
		for (int y = 0; y < 8; ++y)
			for (int z = 0; z < 8; ++z)
				for (int x = 0; x < 8; ++x)
					if ((bc.bits[y] >> (z * 8 + x)) & 1) bc.cls[(y * 8 + z) * 8 + x] = ClassifyVoxel(McToSrc(k.x + (x + 0.5) / 8.0, k.y + (y + 0.5) / 8.0, k.z + (z + 0.5) / 8.0));
		bc.done = true;
		++g_classified;
	}
	g_classMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// greedy merge of a dense grid of class values into boxes (x runs, then y rows, then z stacks)
void Merge(std::vector<uint8_t> &g, int nx, int ny, int nz, std::vector<std::array<int, 7>> &out) {
	auto at = [&](int x, int y, int z) { return ((size_t)z * ny + y) * nx + x; };
	for (int z = 0; z < nz; ++z)
		for (int y = 0; y < ny; ++y)
			for (int x = 0; x < nx; ++x) {
				const uint8_t v = g[at(x, y, z)];
				if (!v) continue;
				int x1 = x;
				while (x1 + 1 < nx && g[at(x1 + 1, y, z)] == v) ++x1;
				int y1 = y;
				for (;;) {
					if (y1 + 1 >= ny) break;
					bool ok = true;
					for (int xx = x; xx <= x1 && ok; ++xx) ok = g[at(xx, y1 + 1, z)] == v;
					if (!ok) break;
					++y1;
				}
				int z1 = z;
				for (;;) {
					if (z1 + 1 >= nz) break;
					bool ok = true;
					for (int yy = y; yy <= y1 && ok; ++yy)
						for (int xx = x; xx <= x1 && ok; ++xx) ok = g[at(xx, yy, z1 + 1)] == v;
					if (!ok) break;
					++z1;
				}
				for (int zz = z; zz <= z1; ++zz)
					for (int yy = y; yy <= y1; ++yy)
						for (int xx = x; xx <= x1; ++xx) g[at(xx, yy, zz)] = 0;
				out.push_back({x, y, z, x1, y1, z1, v});
			}
}

// ---- the build (main thread, at most every 0.25 s; the grid is in Minecraft voxels: x, y (up), z) ------------------------
uint64_t g_lastHash = 0;
int g_lastAnchor[3] = {1 << 30, 0, 0};
int g_lastMode = -1;
double g_lastBuild = -1;
int g_lastPending = -1;
size_t g_sentBlocks = 0;
std::vector<ColBlock> g_sent;
uint64_t Hash(const std::vector<ColBlock> &v) {
	uint64_t h = 1469598103934665603ull;
	auto mix = [&](uint64_t x) { h = (h ^ x) * 1099511628211ull; };
	for (const ColBlock &b : v) {
		mix((uint64_t)(uint32_t)b.x << 32 | (uint32_t)b.y), mix((uint64_t)(uint32_t)b.z);
		for (int i = 0; i < 8; ++i) mix(b.bits[i]);
	}
	return h;
}
Vector Feet() { return McModeActive() ? McFeetSrc() : PlayerOrigin(); }

void Build(bool force) {
	const double now = NowS();
	double fx, fy, fz;
	SrcToMcD(Feet(), fx, fy, fz);
	const int ax = (int)std::floor(fx), ay = (int)std::floor(fy), az = (int)std::floor(fz);
	const int bx0 = ax - g_radH, bx1 = ax + g_radH, by0 = ay - g_radV, by1 = ay + g_radV, bz0 = az - g_radH, bz1 = az + g_radH;
	// (the floating origin moved the mapping: Minecraft block coordinates now sit elsewhere in Portal 2)
	static double mapKey[6] = {0, 0, 0, 0, 0, 0};
	const double mk[6] = {g_map.mcX, g_map.mcY, g_map.mcZ, g_map.src.x, g_map.src.y, g_map.src.z};
	if (std::memcmp(mk, mapKey, sizeof mk)) std::memcpy(mapKey, mk, sizeof mk), g_classes.clear(), g_queue.clear();
	CollisionFieldSentIn(bx0, by0, bz0, bx1, by1, bz1, g_sent);
	g_sentBlocks = g_sent.size();
	const uint64_t h = Hash(g_sent);
	const int pending = (int)g_queue.size();
	const bool moved = ax != g_lastAnchor[0] || ay != g_lastAnchor[1] || az != g_lastAnchor[2];
	if (!force && h == g_lastHash && !moved && g_mode == g_lastMode && !(g_mode == 2 && pending != g_lastPending) && now - g_lastBuild < 2.0) return;
	g_lastHash = h, g_lastAnchor[0] = ax, g_lastAnchor[1] = ay, g_lastAnchor[2] = az, g_lastMode = g_mode, g_lastBuild = now;
	// classification bookkeeping (mode 2): new or changed blocks are queued, nearest last (popped first)
	if (g_mode == 2) {
		std::vector<std::pair<int, Key>> add;
		for (const ColBlock &b : g_sent) {
			const Key k{b.x, b.y, b.z};
			auto it = g_classes.find(k);
			if (it != g_classes.end() && !std::memcmp(it->second.bits, b.bits, sizeof b.bits)) continue;
			BlockClass &bc = g_classes[k];
			std::memcpy(bc.bits, b.bits, sizeof b.bits);
			bc.done = false, bc.cls.clear();
			const int d = (b.x - ax) * (b.x - ax) + (b.y - ay) * (b.y - ay) + (b.z - az) * (b.z - az);
			add.push_back({d, k});
		}
		if (!add.empty()) {
			for (const Key &k : g_queue) add.push_back({(k.x - ax) * (k.x - ax) + (k.y - ay) * (k.y - ay) + (k.z - az) * (k.z - az), k});
			std::sort(add.begin(), add.end(), [](const std::pair<int, Key> &a, const std::pair<int, Key> &b) { return a.first > b.first; });
			g_queue.clear();
			for (auto &a : add) g_queue.push_back(a.second);
		}
		if (g_classes.size() > 40000) g_classes.clear(), g_queue.clear();  // (a long session: start over)
	}
	g_lastPending = (int)g_queue.size();
	// the dense voxel grid
	const int nx = (bx1 - bx0 + 1) * 8, ny = (by1 - by0 + 1) * 8, nz = (bz1 - bz0 + 1) * 8;
	std::vector<uint8_t> grid((size_t)nx * ny * nz, kEmpty);
	for (const ColBlock &b : g_sent) {
		const BlockClass *bc = nullptr;
		if (g_mode == 2) {
			auto it = g_classes.find(Key{b.x, b.y, b.z});
			if (it != g_classes.end() && it->second.done) bc = &it->second;
		}
		for (int y = 0; y < 8; ++y)
			for (int z = 0; z < 8; ++z)
				for (int x = 0; x < 8; ++x) {
					if (!((b.bits[y] >> (z * 8 + x)) & 1)) continue;
					const int gx = (b.x - bx0) * 8 + x, gy = (b.y - by0) * 8 + y, gz = (b.z - bz0) * 8 + z;
					uint8_t v = g_mode == 2 ? kUnclassified : kWorld;
					if (bc) v = bc->cls[(y * 8 + z) * 8 + x];
					if (!v) v = kUnclassified;
					grid[((size_t)gz * ny + gy) * nx + gx] = v;
				}
	}
	std::vector<std::array<int, 7>> merged;
	Merge(grid, nx, ny, nz, merged);
	g_boxes.clear();
	g_labels.clear();
	std::map<uint8_t, std::pair<Vector, Vector>> movBounds;
	for (const auto &m : merged) {
		const Vector a = McToSrc(bx0 + m[0] / 8.0, by0 + m[1] / 8.0, bz0 + m[2] / 8.0), b = McToSrc(bx0 + (m[3] + 1) / 8.0, by0 + (m[4] + 1) / 8.0, bz0 + (m[5] + 1) / 8.0);
		Box bx{Vector(std::fmin(a.x, b.x), std::fmin(a.y, b.y), std::fmin(a.z, b.z)), Vector(std::fmax(a.x, b.x), std::fmax(a.y, b.y), std::fmax(a.z, b.z)), (uint8_t)m[6]};
		g_boxes.push_back(bx);
		if (bx.cls >= kMov) {
			auto it = movBounds.find(bx.cls);
			if (it == movBounds.end()) movBounds[bx.cls] = {bx.mn, bx.mx};
			else
				for (int i = 0; i < 3; ++i) it->second.first[i] = std::fmin(it->second.first[i], bx.mn[i]), it->second.second[i] = std::fmax(it->second.second[i], bx.mx[i]);
		}
	}
	for (auto &mb : movBounds) {
		const size_t i = mb.first - kMov;
		if (i < g_movEnts.size())
			g_labels.push_back({Vector((mb.second.first.x + mb.second.second.x) * 0.5f, (mb.second.first.y + mb.second.second.y) * 0.5f, mb.second.second.z + 10.0f), g_movEnts[i].second,
			                    mb.first});
	}
	// regions (mode 2): not sent / patch pending
	g_regions.clear();
	if (g_mode == 2) {
		const int R = CollisionFieldRegionSize();
		auto rf = [R](int v) { return (int)std::floor(v / (double)R); };
		for (int ry = rf(by0); ry <= rf(by1); ++ry)
			for (int rz = rf(bz0); rz <= rf(bz1); ++rz)
				for (int rx = rf(bx0); rx <= rf(bx1); ++rx) {
					const int s = CollisionFieldRegionState(rx, ry, rz);
					if (s == 1) continue;
					const Vector a = McToSrc(rx * R, ry * R, rz * R), b = McToSrc((rx + 1) * R, (ry + 1) * R, (rz + 1) * R);
					g_regions.push_back({Vector(std::fmin(a.x, b.x), std::fmin(a.y, b.y), std::fmin(a.z, b.z)), Vector(std::fmax(a.x, b.x), std::fmax(a.y, b.y), std::fmax(a.z, b.z)), s});
				}
	}
}

// ---- drawing -------------------------------------------------------------------------------------------------------
Color ClassColor(uint8_t c, uint8_t alpha) {
	switch (c) {
	case kWorld: return g_mode == 1 ? Color(235, 235, 235, alpha) : Color(40, 220, 255, alpha);
	case kStatic: return Color(60, 235, 60, alpha);
	case kClip: return Color(255, 60, 255, alpha);
	case kNothing: return Color(255, 30, 30, alpha);
	case kUnclassified: return Color(235, 235, 235, alpha);
	default: {
		// movable: a hue per entity, away from the fixed colours (orange .. yellow-brown .. pink-orange)
		static const Color kHues[6] = {Color(255, 150, 20), Color(255, 110, 60), Color(230, 180, 40), Color(255, 130, 130), Color(200, 120, 255), Color(255, 200, 120)};
		Color k = kHues[(c - kMov) % 6];
		k.a = alpha;
		return k;
	}
	}
}
bool EnsureMaterial() {
	if (g_mat) return true;
	auto kv = new KeyValues("UnlitGeneric");
	kv->SetInt("$nocull", 1);
	kv->SetInt("$vertexcolor", 1);
	kv->SetInt("$vertexalpha", 1);
	kv->SetInt("$translucent", 1);
	g_mat = materialSystem->CreateMaterial("_pcb_coloverlay", kv);
	return g_mat != nullptr;
}

// quads are buffered and flushed in batches (a dynamic mesh holds a few tens of thousands of vertices)
struct QuadBatch {
	std::vector<Vector> p;
	std::vector<Color> c;
	void Add(const Vector &a, const Vector &b, const Vector &d, const Vector &e, Color col) {
		p.push_back(a), p.push_back(b), p.push_back(d), p.push_back(e);
		c.push_back(col);
	}
	void Flush() {
		const size_t quads = c.size();
		for (size_t s = 0; s < quads; s += 4000) {
			const size_t n = std::min<size_t>(4000, quads - s);
			MeshBuilder mb(g_mat, PrimitiveType::QUADS, (int)n);
			mb.ForceDepth(false, true);  // depth-tested, no depth writes (it reads correctly against the walls)
			for (size_t q = s; q < s + n; ++q)
				for (int k = 0; k < 4; ++k) {
					mb.Position(p[q * 4 + k]);
					mb.Color(c[q]);
					mb.AdvanceVertex();
				}
			mb.Draw();
		}
		p.clear(), c.clear();
	}
};
QuadBatch g_batch;
void Edge(const Vector &a, const Vector &b, const Vector &eye, float wk, Color col) {
	const Vector mid = (a + b) * 0.5f, toEye = eye - mid, d = b - a;
	Vector side(d.y * toEye.z - d.z * toEye.y, d.z * toEye.x - d.x * toEye.z, d.x * toEye.y - d.y * toEye.x);
	const float sl = side.Length();
	if (sl < 1e-4f) return;
	const float w = std::fmax(0.15f, toEye.Length() * wk);
	side = side * (w / sl);
	g_batch.Add(a - side, b - side, b + side, a + side, col);
}
void WireBox(const Vector &mn0, const Vector &mx0, const Vector &eye, Color edge, Color face, float wk, bool faces) {
	const Vector mn = mn0 - Vector(0.3f, 0.3f, 0.3f), mx = mx0 + Vector(0.3f, 0.3f, 0.3f);  // (just outside the surfaces: no z-fighting)
	auto C = [&](int k) { return Vector((k & 1) ? mx.x : mn.x, (k & 2) ? mx.y : mn.y, (k & 4) ? mx.z : mn.z); };
	static const int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
	for (auto &e : kEdges) Edge(C(e[0]), C(e[1]), eye, wk, edge);
	if (!faces) return;
	static const int kFaces[6][4] = {{0, 2, 6, 4}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 5, 7, 6}};
	for (auto &f : kFaces) g_batch.Add(C(f[0]), C(f[1]), C(f[2]), C(f[3]), face);
}
void Text3D(const Vector &at, const std::string &s, const Vector &eye, Color col) {
	// billboarded: right = horizontal, perpendicular to the view; up = world up; ~ 2.5 u font pixels at 200 u
	Vector to = eye - at;
	const float dist = to.Length();
	Vector right(-to.y, to.x, 0);
	const float rl = right.Length();
	if (rl < 1e-3f) return;
	right = right * (1.0f / rl);
	const float px = std::fmax(1.0f, dist * 0.008f);
	const int len = (int)std::min<size_t>(s.size(), 48);
	const Vector o = at - right * (len * 6 * px * 0.5f) + Vector(0, 0, 7 * px);
	Color back(0, 0, 0, 150);
	{
		const Vector a = o - right * px + Vector(0, 0, px), b = o + right * (len * 6 * px) + Vector(0, 0, px);
		g_batch.Add(a, b, b - Vector(0, 0, 9 * px), a - Vector(0, 0, 9 * px), back);
	}
	for (int i = 0; i < len; ++i)
		for (int y = 0; y < 7; ++y)
			for (int x = 0; x < 5; ++x) {
				if (!FontPixel(s[i], x, y)) continue;
				const Vector p = o + right * ((i * 6 + x) * px) - Vector(0, 0, y * px);
				g_batch.Add(p, p + right * px, p + right * px - Vector(0, 0, px), p - Vector(0, 0, px), col);
			}
}

// pcb_collision_shot
int g_shotStage = 0, g_shotPrevMode = 0, g_shotFrames = 0;
double g_shotAt = 0;
std::string g_shotName;
int g_drawnViews = 0;

void WriteLegend(const std::string &path) {
	FILE *f = std::fopen(path.c_str(), "w");
	if (!f) return;
	int counts[256] = {};
	for (const Box &b : g_boxes) ++counts[b.cls];
	const Vector p = Feet();
	std::fprintf(f, "# collision overlay (bridge #24) mode %d, %d x %d x %d blocks around feet %.1f %.1f %.1f; %zu sent blocks -> %zu boxes; %zu blocks still to classify\n", g_mode,
	             2 * g_radH + 1, 2 * g_radV + 1, 2 * g_radH + 1, p.x, p.y, p.z, g_sentBlocks, g_boxes.size(), g_queue.size());
	std::fprintf(f, "world brush (cyan%s): %d boxes\nstatic prop (green): %d\nplayerclip (magenta): %d\nnothing in Portal 2 there now: stale / invisible (RED): %d\nnot classified yet (white): %d\n",
	             g_mode == 1 ? "; white in mode 1" : "", counts[kWorld], counts[kStatic], counts[kClip], counts[kNothing], counts[kUnclassified]);
	for (size_t i = 0; i < g_movEnts.size(); ++i)
		if (counts[kMov + i]) {
			const Color c = ClassColor((uint8_t)(kMov + i), 255);
			std::fprintf(f, "movable %s (rgb %d %d %d): %d boxes\n", g_movEnts[i].second.c_str(), c.r, c.g, c.b, counts[kMov + i]);
		}
	int unsent = 0, pending = 0;
	for (const RegionBox &r : g_regions) ++(r.state == 0 ? unsent : pending);
	std::fprintf(f, "regions not sent (grey outline): %d, patch pending (yellow outline): %d\nlive platform boxes (blue): %zu\n", unsent, pending, g_platBoxes.size());
	std::fprintf(f, "# boxes: <class> <x0 y0 z0> <x1 y1 z1>\n");
	for (const Box &b : g_boxes)
		std::fprintf(f, "box %s %.1f %.1f %.1f %.1f %.1f %.1f\n",
		             b.cls == kWorld ? "world" : b.cls == kStatic ? "static" : b.cls == kClip ? "clip" : b.cls == kNothing ? "NOTHING" : b.cls == kUnclassified ? "unclassified" :
		                                                                                                      (b.cls - kMov < (int)g_movEnts.size() ? g_movEnts[b.cls - kMov].second.c_str() : "movable"),
		             b.mn.x, b.mn.y, b.mn.z, b.mx.x, b.mx.y, b.mx.z);
	std::fclose(f);
}

}  // namespace

// once per server frame (bugmark.cpp TestToolsFrame)
void ColOverlayFrame() {
	if (g_shotStage == 1) {
		// wait until classified (or 4 s), then two more frames
		if (g_mode == 2 && !g_queue.empty() && NowS() - g_shotAt < 4.0) {
		} else if (++g_shotFrames >= 3) {
			Build(true);
			engine.Cmd(("pcb_glhud_shot " + g_shotName).c_str());
			WriteLegend(CacheDir() + "/glhud_" + g_shotName + ".txt");
			console->Print("[bridge] collision shot %s: mode %d, %zu boxes, %zu sent blocks, %zu to classify; legend glhud_%s.txt\n", g_shotName.c_str(), g_mode, g_boxes.size(), g_sentBlocks,
			               g_queue.size(), g_shotName.c_str());
			g_shotStage = 2, g_shotAt = NowS();
		}
	} else if (g_shotStage == 2 && NowS() - g_shotAt > 0.5) {
		g_mode = g_shotPrevMode, g_shotStage = 0;
	}
	if (!g_mode) return;
	if (g_mode == 2) ClassifyStep(2.0);
	if (NowS() - g_lastBuild >= 0.25) Build(false);
	CollisionFieldPlatformBoxes(g_platBoxes);
}

// per 3D view, translucent pass (render.cpp)
void ColOverlayDraw(const CViewSetup *view) {
	if (!g_mode || !EnsureMaterial()) return;
	const Vector eye = view->origin;
	const uint8_t faceA = g_mode == 1 ? 18 : 24;
	for (const Box &b : g_boxes) WireBox(b.mn, b.mx, eye, ClassColor(b.cls, 230), ClassColor(b.cls, faceA), 0.0011f, true);
	for (const RegionBox &r : g_regions) WireBox(r.mn, r.mx, eye, r.state == 2 ? Color(255, 230, 0, 230) : Color(140, 140, 140, 160), Color(0, 0, 0, 0), 0.0015f, false);
	for (const auto &p : g_platBoxes) WireBox(p.first, p.second, eye, Color(60, 90, 255, 230), Color(60, 90, 255, 40), 0.0011f, true);
	for (const Label &l : g_labels) Text3D(l.at, l.text, eye, ClassColor(l.cls, 255));
	g_batch.Flush();
	++g_drawnViews;
}

void ColOverlayShutdown() {
	g_mode = 0;
	if (g_mat) materialSystem->DestroyMaterial(g_mat), g_mat = nullptr;
}

CON_COMMAND(pc_show_collision, "pc_show_collision [0|1|2] [radius_blocks=8] [vertical=6] - draw Minecraft's collision copy around the player (what was sent): 1 merged voxels,\n"
                               "  2 coloured by source (cyan world, green static prop, own hue + name movable, magenta playerclip, RED nothing there now, white unclassified;\n"
                               "  region outlines grey = not sent, yellow = patch pending; blue = live platform)\n") {
	if (args.ArgC() > 1) g_mode = std::max(0, std::min(2, std::atoi(args[1])));
	if (args.ArgC() > 2) g_radH = std::max(1, std::min(16, std::atoi(args[2])));
	if (args.ArgC() > 3) g_radV = std::max(1, std::min(12, std::atoi(args[3])));
	g_lastBuild = -1;
	console->Print("[bridge] show collision %d (radius %d / %d blocks): %zu boxes from %zu sent blocks, %zu blocks to classify (%d classified, %.0f ms), %d views drawn\n", g_mode, g_radH,
	               g_radV, g_boxes.size(), g_sentBlocks, g_queue.size(), g_classified, g_classMs, g_drawnViews);
}
CON_COMMAND(pcb_collision_shot, "pcb_collision_shot <name> [mode=2] - (tests) collision overlay on, cache/bridge/glhud_<name>.png + glhud_<name>.txt (legend, boxes); mode restored\n") {
	if (args.ArgC() < 2) return console->Print("%s", pcb_collision_shot.ThisPtr()->m_pszHelpString);
	g_shotName = args[1];
	g_shotPrevMode = g_shotStage ? g_shotPrevMode : g_mode;
	g_mode = args.ArgC() > 2 ? std::max(1, std::min(2, std::atoi(args[2]))) : 2;
	g_shotStage = 1, g_shotFrames = 0, g_shotAt = NowS();
	g_lastBuild = -1;
}
CON_COMMAND(pcb_collision_probe, "pcb_collision_probe <x> <y> <z> - (tests) what the overlay's classifier says at a Portal 2 point\n") {
	if (args.ArgC() < 4) return;
	const Vector p((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3]));
	g_serverEntsAt = NowS(), EntLiveSet(g_serverEnts);
	auto v = EntFind("player");
	g_playerEnt = v.empty() ? nullptr : v[0];
	const uint8_t c = ClassifyVoxel(p);
	console->Print("[bridge] collision probe (%.1f %.1f %.1f): %s\n", p.x, p.y, p.z,
	               c == kWorld ? "world" : c == kStatic ? "static prop" : c == kClip ? "playerclip" : c == kNothing ? "nothing" : c >= kMov && c - kMov < (int)g_movEnts.size() ? g_movEnts[c - kMov].second.c_str() : "?");
}

}  // namespace bridge
