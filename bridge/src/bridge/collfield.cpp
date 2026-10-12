// Portalcraft bridge: CollisionField export (SkyCraft DESIGN 5.1, stage A, Portal 2 edition).
//
// Portal 2's world collision around the player is sampled with the engine's own queries and
// streamed to Minecraft as 8x8x8-block regions of 1/8-block voxels (kColRegion / ColBlock),
// which the Fabric mod merges into Minecraft's block-collision queries. Per block (40 u):
//   1. one box test (the whole block, a zero-length box trace against the world): no solid
//      inside -> the block is empty and sends nothing;
//   2. otherwise the 512 sub-voxel centres are point-tested (world contents, MASK_PLAYERSOLID);
//   3. if the box touched solid but no centre is inside (geometry thinner than 5 u: glass,
//      thin panels), each sub-voxel gets its own box test instead.
// Regions are voxelised nearest-first around the player under a per-frame time budget, and
// each is sent once per collision epoch (a new epoch on every map load).
#include "bridge.hpp"
#include "growth.hpp"
#include "coords.hpp"
#include "stage.hpp"
#include "link.hpp"
#include "prof.hpp"
#include "colharvest.hpp"
#include "dig.hpp"

#include "Modules/Tier1.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <string>
#include <cmath>
#include <cstdlib>
#include <set>
#include <tuple>
#include <unordered_set>
#include <vector>
#include <map>
#include <array>
#include <sys/stat.h>
#include "platform/platform.hpp"
#include <unistd.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace bridge {

const char *McMapName();  // mc.cpp

using namespace skycraft::proto;

static const int kRegion = 8;  // blocks per region side (SkyCollision.REGION_SIZE)
static uint32_t g_epoch = 0;
static bool g_cleared = false;
static std::set<std::tuple<int, int, int>> g_sent;
PCB_GAUGE("col.sent", g_sent.size());
static std::set<std::tuple<int, int, int>> g_everSent;  // sent at least once this epoch (CollisionFieldFeetReady)
PCB_GAUGE("col.everSent", g_everSent.size());
static std::map<std::tuple<int, int, int>, std::vector<ColBlock>> g_lastSent;  // what Minecraft has, per region
PCB_GAUGE("col.lastSent", g_lastSent.size());
static std::map<std::tuple<int, int, int>, std::vector<std::pair<Vector, Vector>>> g_patches;  // region -> Source boxes to redo
PCB_GAUGE("col.patches", g_patches.size());
// Patch work, resumable over frames (a patch of a few hundred blocks next to a moving model is 20-50 ms of queries).
struct PatchWork {
	bool active = false;
	std::tuple<int, int, int> key;
	std::vector<std::pair<Vector, Vector>> boxes;
	std::vector<ColBlock> blocks;  // kept blocks + redone ones so far
	int next = 0;                  // block index 0..511
	bool dynNear = false;
};
static PatchWork g_pw;
static int g_radiusH = 3, g_radiusV = 2;  // regions around the player
static int g_traceSends = 0;  // pcb_colfield_trace <n>: log the next n region sends
static double g_nearBudgetMs = 20.0;
static double g_patchFeetBudgetMs = 6.0;
static bool g_boost = false;  // Portal 2 carries the body (CollisionFieldBoost): finish the feet's patches fast
void CollisionFieldBoost(bool on) { g_boost = on; }  // a moved solid's patch by the feet (old data stays in Minecraft meanwhile)
static double g_stepBudgetMs = 20.0;  // the ring one region further out (6 ms let an elevator ride outrun the floor)  // regions under/around the feet (see the frame loop)
static bool g_prefetchOn = true;  // pcb_colfield_prefetch (bridge #20)
static int g_prefetched = 0;
static double g_budgetMs = 1.5;  // per frame (lead, perf: was 4 ms, checked only between whole regions)
static int g_regionsSent = 0, g_blocksSent = 0;
static double g_msTotal = 0;
static std::vector<std::pair<std::tuple<int, int, int>, std::vector<Vector>>> g_carves;  // unused yet (portal holes)

// Terrain mode (the Minecraft player in pc_pit_flings, bridge #13): only the chambers are exported, i.e. Portal 2's
// world outside the region box (inside it Minecraft's own terrain is the ground), and never what lies in an open
// portal's hole volume, so the Minecraft player can walk or fall into a chamber portal; a region whose holes changed
// is voxelised again (the mod replaces a region on every send).
bool ClipRegionContains(const Vector &p);                    // clip.cpp
bool PortalHoleBox(int i, Vector &mn, Vector &mx);           // blockcollide.cpp
int ChellHoleBoxes(Vector *mn, Vector *mx, int max);          // blockcollide.cpp (bridge #45)
void ClipRegionBounds(Vector &mn, Vector &mx);               // clip.cpp
static void HarvestRegionDirty(const std::tuple<int, int, int> &k);  // (bridge #33) below
bool DigEnabled();  // dig.cpp (bridge #35)
static bool HarvestActive();
static void HarvestLevelReset();
static uint32_t g_hOptGen = 1;  // bumped when a voxelising option changes: results made under the old one are dropped
static bool g_terrainOnly = false;
static bool g_stageOn = false;  // (bridge #49) the virtual region (stage.cpp) is on: nothing of Portal 2's is exported inside its box (switching it on does not redo the export)
// (bridge #45) A portal's hole is the MINECRAFT player's collision rule, applied where its collision is asked (the mod's BlockCollisionsMixin carves
// the host's voxels and the blocks around each linked portal's opening, from kInHole). It is no longer baked into the exported field: the field is what
// Portal 2 has (the dig's reveal, the mining ray, the wall faces and the moved regions all read it), and a hole carved into it removed real wall (and the
// revealed blocks of a hole dug beside it) 128 u deep behind every portal, and re-voxelised the regions around it whenever a portal moved. g_carveExport
// (pcb_colfield_hole_export 1) brings the old behaviour back.
static bool g_carveExport = false;
static Vector g_holeMn[kPortalSlots], g_holeMx[kPortalSlots];  // (bridge #32: one per portal slot, every active pair) the holes baked into the export (g_carveExport only)
static bool g_holeOn[kPortalSlots] = {};
static Vector g_qMn[kPortalSlots], g_qMx[kPortalSlots];  // the holes now (every linked pair): not baked in, for the host's own rules
static bool g_qOn[kPortalSlots] = {};
static bool InHole(const Vector &p) {  // baked into the export
	for (int i = 0; i < kPortalSlots; ++i)
		if (g_holeOn[i] && p.x > g_holeMn[i].x && p.x < g_holeMx[i].x && p.y > g_holeMn[i].y && p.y < g_holeMx[i].y && p.z > g_holeMn[i].z && p.z < g_holeMx[i].z) return true;
	return false;
}
static bool InQueryHole(const Vector &p) {  // in a linked portal's hole now
	for (int i = 0; i < kPortalSlots; ++i)
		if (g_qOn[i] && p.x > g_qMn[i].x && p.x < g_qMx[i].x && p.y > g_qMn[i].y && p.y < g_qMx[i].y && p.z > g_qMn[i].z && p.z < g_qMx[i].z) return true;
	return false;
}
// (bridge #72) the support guard (movers.cpp) asks: feet inside a linked portal's hole are going through it, not falling out of the mover floor they stood on
bool CollisionFieldInPortalHole(const Vector &p) { return InQueryHole(p); }
// (bridge #23) Playerclip (CONTENTS_PLAYERCLIP: Valve's invisible brushes over ledges and around doors that stop Chell
// climbing) is not exported: the user's "invisible blocks" by the exit door ledge, which the Minecraft player (it jumps
// higher than Chell) could not get past, and which Minecraft's crosshair, mobs, items and fluids met too. Kept only where
// it is a floor over nothing (no solid within 160 u below): there it may be what keeps the player in the map.
static bool g_clipOn = false;  // pcb_colfield_playerclip 1: export playerclip as before
static int g_clipKept = 0, g_clipDropped = 0;
static bool ClipFloorOverVoid(const Vector &p) {
	CGameTrace tr;
	engine.Trace(p, p - Vector(0, 0, 160), Vector(0, 0, 0), MASK_SOLID, tr, true);
	return tr.fraction >= 1.0f && !tr.startsolid;
}
static bool WorldSolidAt(const Vector &p) {
	if (InHole(p)) return false;
	const int c = engine.GetPointContentsWorld(engine.traceServer->ThisPtr(), p, MASK_PLAYERSOLID);
	if (c & MASK_SOLID) return true;
	if (!(c & CONTENTS_PLAYERCLIP)) return false;
	if (g_clipOn) return true;
	const bool keep = ClipFloorOverVoid(p);
	++(keep ? g_clipKept : g_clipDropped);
	return keep;
}

// (a prefilter: playerclip included, so a block of clip is looked at and WorldSolidAt decides per sub-voxel)
static bool BoxHitsWorld(const Vector &centre, float half) {
	CGameTrace tr;
	engine.Trace(centre, centre, Vector(half, half, half), MASK_PLAYERSOLID, tr, true);
	return tr.startsolid || tr.allsolid;
}
// the thin-geometry fallback's per sub-voxel box: solid only (thin playerclip is never kept)
static bool BoxHitsWorldSolid(const Vector &centre, float half) {
	CGameTrace tr;
	engine.Trace(centre, centre, Vector(half, half, half), g_clipOn ? MASK_PLAYERSOLID : MASK_SOLID, tr, true);
	return tr.startsolid || tr.allsolid;
}

// ---- (bridge #26) the static field: the world and static props only ------------------------------------------------
// Everything that can move or animate (doors, lifts, elevators, panels, buttons, cubes...) is a mover (movers.cpp): its own
// shape, bound by id, posed every frame; Minecraft collides with it, rides it and is pushed by it there. The regions here
// never hold a mover, so they never change while a level runs and every region is cacheable (except a portal's hole).
// Static props (stairs, walkways, rails: prop_static) are not "world" for world-only traces and point contents, and are not
// server entities: a trace hit on something that is not in the server's entity list is a static prop.
void EntLiveSet(std::unordered_set<void *> &out);  // ents.cpp
static std::unordered_set<void *> g_serverEnts;
static double g_serverEntsAt = -1e9;
static bool g_staticOn = true;  // pcb_colfield_static
static bool IsStaticProp(void *e) { return g_staticOn && e && !g_serverEnts.count(e); }
static double NowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static bool g_dynNear = false;  // a static prop is in the region being voxelised
static bool BoxHitsDyn(const Vector &centre, float half) {
	if (!g_dynNear) return false;
	struct StaticFilter : public ITraceFilter {
		virtual bool ShouldHitEntity(void *e, int) { return IsStaticProp(e); }
		// (everything: static props are only enumerated with the world; the world itself only adds what is solid anyway)
		virtual TraceType_t GetTraceType() const { return TRACE_EVERYTHING; }
	} filter;
	Ray_t ray;
	ray.m_Delta = VectorAligned(0, 0, 0);
	ray.m_IsSwept = false;
	ray.m_Extents = VectorAligned(half, half, half);
	ray.m_IsRay = half < 1e-3f;
	ray.m_StartOffset = VectorAligned(0, 0, 0);
	ray.m_Start = VectorAligned(centre.x, centre.y, centre.z);
	ray.m_pWorldAxisTransform = nullptr;
	CGameTrace tr;
	std::memset(&tr, 0, sizeof tr);
	engine.TraceRay(engine.traceServer->ThisPtr(), ray, MASK_PLAYERSOLID, &filter, &tr);
	return tr.startsolid || tr.allsolid;
}
static void DirtyBox(const Vector &origin, float radius);

// ---- (bridge #25) the fall guard's queries (mc.cpp FallGuardFrame) -----------------------------------------------
// What Minecraft holds as solid: the world (solid, playerclip only as a floor over nothing), static props, and the bound
// movers (where they are now, and a moving one in its own frame: movers.cpp). Not the player or the plugin's colliders.
bool MoverIsBound(void *e);  // movers.cpp
bool MoversFloorCrossed(const Vector &a, const Vector &b, double ageA, double ageB, Vector &at, std::string &what);
struct ExportFilter : public ITraceFilter {
	virtual bool ShouldHitEntity(void *e, int) { return IsStaticProp(e) || MoverIsBound(e); }
	virtual TraceType_t GetTraceType() const { return TRACE_EVERYTHING; }
};
bool FloorCrossedIn(const Vector &a, const Vector &b, ITraceFilter &filter, Vector &at, std::string &what);
// ageA / ageB: how long ago (s) Minecraft was at a / b
bool CollisionFieldFloorCrossed(const Vector &a, const Vector &b, double ageA, double ageB, Vector &at, std::string &what) {
	if (a.z - b.z < 1.0f || (b - a).Length() > 170.0f) return false;  // (a tick over 4 blocks is a teleport, not a fall)
	if (NowS() - g_serverEntsAt > 0.25) g_serverEntsAt = NowS(), EntLiveSet(g_serverEnts);
	ExportFilter filter;
	if (FloorCrossedIn(a, b, filter, at, what)) return true;
	return MoversFloorCrossed(a, b, ageA, ageB, at, what);
}
bool FloorCrossedIn(const Vector &a, const Vector &b, ITraceFilter &filter, Vector &at, std::string &what) {
	if (a.z - b.z < 1.0f) return false;
	Ray_t ray;
	// (straight down where the feet ended, from the height they left: Minecraft moves sideways then falls, and a diagonal
	// sweep clipped the top of the step it walked off; the middle of the feet only, 12 u of the 24, since the export's
	// 5 u voxels put a ledge's edge a few units off Portal 2's)
	const Vector s(b.x, b.y, a.z + 2.0f), e = b + Vector(0, 0, 2);
	ray.m_Delta = VectorAligned(e.x - s.x, e.y - s.y, e.z - s.z);
	ray.m_IsSwept = true;
	ray.m_Extents = VectorAligned(6, 6, 1);
	ray.m_IsRay = false;
	ray.m_StartOffset = VectorAligned(0, 0, 0);
	ray.m_Start = VectorAligned(s.x, s.y, s.z);
	ray.m_pWorldAxisTransform = nullptr;
	CGameTrace tr;
	std::memset(&tr, 0, sizeof tr);
	engine.TraceRay(engine.traceServer->ThisPtr(), ray, MASK_PLAYERSOLID, &filter, &tr);
	// (feet that left from inside a solid: start lower, out of it, down to b)
	for (float down = 8.0f; (tr.startsolid || tr.allsolid) && s.z - down > e.z + 1.0f; down += 8.0f) {
		const Vector s2(s.x, s.y, s.z - down);
		ray.m_Start = VectorAligned(s2.x, s2.y, s2.z);
		ray.m_Delta = VectorAligned(0, 0, e.z - s2.z);
		std::memset(&tr, 0, sizeof tr);
		engine.TraceRay(engine.traceServer->ThisPtr(), ray, MASK_PLAYERSOLID, &filter, &tr);
	}
	if (tr.startsolid || tr.allsolid || tr.fraction >= 1.0f || tr.plane.normal.z <= 0.7f) return false;
	at = Vector(tr.endpos.x, tr.endpos.y, tr.endpos.z - 1.0f);
	if (b.z > at.z - 8.0f) return false;  // (the export's 5 u voxels: landing a few units into the true floor is normal)
	if (InQueryHole(at - Vector(0, 0, 2)) || ((g_terrainOnly || g_stageOn) && ClipRegionContains(at))) return false;  // (a portal's hole, Minecraft's own terrain)
	if (!(tr.contents & MASK_SOLID) && !ClipFloorOverVoid(at - Vector(0, 0, 2))) return false;  // (playerclip that is not exported)
	what = tr.m_pEnt && g_serverEnts.count(tr.m_pEnt) ? EntKV(tr.m_pEnt, "classname") + " " + EntKV(tr.m_pEnt, "targetname") : "world";
	if (!tr.m_pEnt || !g_serverEnts.count(tr.m_pEnt)) DirtyBox(at, 48.0f);  // (the static field there is redone, by the feet budget)
	return true;
}
// Minecraft has a solid sub-voxel under these feet (a 3x3 sub-voxel patch, from 0.25 block above to 1 block below)
bool CollisionFieldSentFloorUnder(const Vector &feet) {
	const Vector mc = SrcToMc(feet);
	for (int sy = 2; sy >= -8; --sy)
		for (int sz = -1; sz <= 1; ++sz)
			for (int sx = -1; sx <= 1; ++sx) {
				const double px = mc.x + sx / 8.0, py = mc.y + sy / 8.0 - 1.0 / 16.0, pz = mc.z + sz / 8.0;
				const int bx = (int)std::floor(px), by = (int)std::floor(py), bz = (int)std::floor(pz);
				auto it = g_lastSent.find(std::make_tuple((int)std::floor(bx / (double)kRegion), (int)std::floor(by / (double)kRegion), (int)std::floor(bz / (double)kRegion)));
				if (it == g_lastSent.end()) continue;
				const int vx = (int)std::floor((px - bx) * 8), vy = (int)std::floor((py - by) * 8), vz = (int)std::floor((pz - bz) * 8);
				for (const ColBlock &c : it->second)
					if (c.x == bx && c.y == by && c.z == bz && (c.bits[vy] >> (vz * 8 + vx) & 1)) return true;
			}
	return false;
}

// collision work still queued (a static region redone after a fall guard catch)
bool CollisionFieldDynPending() { return !g_patches.empty() || g_pw.active; }
size_t CollisionFieldDynPendingCount() { return g_patches.size(); }

// One Minecraft block's 8x8x8 occupancy; false if empty.
static bool g_blockExtra = false;  // this block has static-prop / moving-solid geometry (else world contents alone)
static bool ClipFloorOverVoid(const Vector &p);
static bool SolidAt(const Vector &p) {
	if (WorldSolidAt(p)) return true;
	if (!g_blockExtra || InHole(p)) return false;
	return BoxHitsDyn(p, 0.0f);
}
static bool BoxHits(const Vector &centre, float half) { return BoxHitsWorldSolid(centre, half) || (g_blockExtra && BoxHitsDyn(centre, half)); }
static bool VoxeliseBlock(int bx, int by, int bz, ColBlock &out) {
	Vector centre = McToSrc(bx + 0.5, by + 0.5, bz + 0.5);
	if ((g_terrainOnly || g_stageOn) && ClipRegionContains(centre)) return false;
	g_blockExtra = g_dynNear && BoxHitsDyn(centre, kUnitsPerBlock / 2 - 0.05f);
	if (!g_blockExtra && !BoxHitsWorld(centre, kUnitsPerBlock / 2 - 0.05f)) return false;
	out.x = bx, out.y = by, out.z = bz, out.pad = 0;
	// Coarse to fine (lead, perf): a sub-voxel centre can only be solid where the box around it touches solid,
	// so the 8 octants (1/2 block) and then their 2x2x2 quarters (1/4 block) are box-tested first and only the
	// quarters that touch solid get their 8 centres point-tested. Same result, far fewer queries on surfaces.
	auto sub = [&](double i) { return (i + 0.5) / 8.0; };  // sub-voxel index -> block fraction (centre)
	bool quarter[4][4][4];
	const float octHalf = kUnitsPerBlock / 4 - 0.05f, qHalf = kUnitsPerBlock / 8 - 0.05f;
	for (int oy = 0; oy < 2; ++oy)
		for (int oz = 0; oz < 2; ++oz)
			for (int ox = 0; ox < 2; ++ox) {
				// (a block with static-prop geometry keeps the full test: box traces against models are
				// not trusted to report a box lying wholly inside one)
				const bool oct = g_blockExtra || BoxHitsWorld(McToSrc(bx + (ox * 4 + 2) / 8.0, by + (oy * 4 + 2) / 8.0, bz + (oz * 4 + 2) / 8.0), octHalf);
				for (int qy = 0; qy < 2; ++qy)
					for (int qz = 0; qz < 2; ++qz)
						for (int qx = 0; qx < 2; ++qx) {
							const int x = ox * 2 + qx, y = oy * 2 + qy, z = oz * 2 + qz;
							quarter[y][z][x] = oct && (g_blockExtra || BoxHitsWorld(McToSrc(bx + (x * 2 + 1) / 8.0, by + (y * 2 + 1) / 8.0, bz + (z * 2 + 1) / 8.0), qHalf));
						}
			}
	bool any = false;
	for (int y = 0; y < 8; ++y) {
		uint64_t layer = 0;
		for (int z = 0; z < 8; ++z)
			for (int x = 0; x < 8; ++x)
				if (quarter[y / 2][z / 2][x / 2] && SolidAt(McToSrc(bx + sub(x), by + sub(y), bz + sub(z)))) layer |= 1ull << (z * 8 + x);
		out.bits[y] = layer;
		any |= layer != 0;
	}
	if (!any) {  // thin geometry: box-test each sub-voxel (only inside quarters that touch solid)
		const float h = kUnitsPerBlock / 16 - 0.05f;
		for (int y = 0; y < 8; ++y) {
			uint64_t layer = 0;
			for (int z = 0; z < 8; ++z)
				for (int x = 0; x < 8; ++x) {
					if (!quarter[y / 2][z / 2][x / 2]) continue;
					const Vector p = McToSrc(bx + sub(x), by + sub(y), bz + sub(z));
					if (!InHole(p) && BoxHits(p, h)) layer |= 1ull << (z * 8 + x);
				}
			out.bits[y] = layer;
			any |= layer != 0;
		}
	}
	return any;
}

// ---- the static collision cache (lead, perf) ---------------------------------------------------------------------
// A chamber's world and static props never change, so a region voxelised without moving solids or a portal hole in
// it is kept (in memory and on disk, cache/bridge/colcache/<map>.bin) and sent again from there for free: after the
// first session Minecraft mode costs no voxelising except around moving solids and portal holes. Keys are Minecraft
// region keys at the mapping the cache was built with (g_cacheOff, Minecraft's block at Source 0 0 0); the floating
// origin shifts whole 16-block sections, so a later mapping differs by whole regions and keys/blocks translate.
static const uint32_t kCacheMagic = 0x43435043, kCacheVersion = 4;  // v4 (bridge #26): movers never in it, every region cacheable  // v3 (bridge #23): playerclip dropped except floors over nothing  // v2: static world + static props only (v1 baked movable parts where they stood)  // "PCCC"
static std::map<std::tuple<int, int, int>, std::vector<ColBlock>> g_cache;
PCB_GAUGE("col.cache", g_cache.size());
static int g_cacheOff[3] = {0, 0, 0};
static bool g_cacheHaveOff = false;
static std::string g_cacheMap;
static size_t g_cacheSaved = 0;
static int g_cacheHits = 0, g_cacheStores = 0;
static bool g_cacheOn = true;  // pcb_colfield_cache
static void CurOff(int o[3]) {
	const Vector m = SrcToMc(Vector(0, 0, 0));
	o[0] = (int)std::floor(m.x), o[1] = (int)std::floor(m.y), o[2] = (int)std::floor(m.z);
}
// delta (blocks) from the cache's mapping to the current one; false if it isn't whole regions
static bool CacheDelta(int d[3]) {
	if (!g_cacheHaveOff) return false;
	int o[3];
	CurOff(o);
	for (int i = 0; i < 3; ++i) {
		d[i] = o[i] - g_cacheOff[i];
		if (d[i] % kRegion) return false;
	}
	return true;
}
static std::string CachePath(const std::string &map) { return CacheDir() + "/colcache/" + map + ".bin"; }
// (bridge #34) the map's version, in a sidecar file next to the cache (colcache/<map>.key): the hash of the BSP the cache was built from
// (mapindex.cpp MapVersionKey). A cache whose key differs from the BSP now (or has no key: every cache before #34) is thrown away and rebuilt.
std::string MapVersionKey(const std::string &map);  // mapindex.cpp
static std::string g_cacheKey;
static std::string CacheKeyPath(const std::string &map) { return CacheDir() + "/colcache/" + map + ".key"; }
static void CacheSave() {
	if (g_cacheMap.empty() || !g_cacheHaveOff || g_cache.size() == g_cacheSaved) return;
	std::string dir = CacheDir() + "/colcache";
	plat::MakeDir(dir);
	const std::string path = CachePath(g_cacheMap), tmp = path + ".tmp";
	FILE *f = std::fopen(tmp.c_str(), "wb");
	if (!f) return;
	uint32_t hdr[2] = {kCacheMagic, kCacheVersion};
	std::fwrite(hdr, sizeof hdr, 1, f);
	std::fwrite(g_cacheOff, sizeof g_cacheOff, 1, f);
	uint32_t n = (uint32_t)g_cache.size();
	std::fwrite(&n, 4, 1, f);
	for (const auto &e : g_cache) {
		int32_t k[3] = {std::get<0>(e.first), std::get<1>(e.first), std::get<2>(e.first)};
		uint32_t c = (uint32_t)e.second.size();
		std::fwrite(k, sizeof k, 1, f);
		std::fwrite(&c, 4, 1, f);
		if (c) std::fwrite(e.second.data(), sizeof(ColBlock), c, f);
	}
	std::fclose(f);
	std::rename(tmp.c_str(), path.c_str());
	if (!g_cacheKey.empty())
		if (FILE *kf = std::fopen(CacheKeyPath(g_cacheMap).c_str(), "wb")) std::fwrite(g_cacheKey.data(), 1, g_cacheKey.size(), kf), std::fclose(kf);
	g_cacheSaved = g_cache.size();
	console->Print("[bridge] collision cache: %zu regions saved to %s\n", g_cache.size(), path.c_str());
}
static void CacheLoad(const std::string &map) {
	g_cache.clear(), g_cacheHaveOff = false, g_cacheSaved = 0, g_cacheMap = map;
	g_cacheKey = MapVersionKey(map);
	if (!g_cacheKey.empty()) {
		std::string have;
		if (FILE *kf = std::fopen(CacheKeyPath(map).c_str(), "rb")) {
			char kb[96];
			have.assign(kb, std::fread(kb, 1, sizeof kb, kf)), std::fclose(kf);
		}
		struct stat st;
		if (have != g_cacheKey && stat(CachePath(map).c_str(), &st) == 0) {
			std::remove(CachePath(map).c_str()), std::remove(CacheKeyPath(map).c_str());
			console->Print("[bridge] collision cache: %s was built from another version of the map (%s, the BSP now is %s): thrown away, rebuilt as it is needed\n", map.c_str(),
			               have.empty() ? "no key" : have.c_str(), g_cacheKey.c_str());
			return;
		}
	}
	FILE *f = std::fopen(CachePath(map).c_str(), "rb");
	if (!f) return;
	uint32_t hdr[2] = {0, 0}, n = 0;
	bool ok = std::fread(hdr, sizeof hdr, 1, f) == 1 && hdr[0] == kCacheMagic && hdr[1] == kCacheVersion && std::fread(g_cacheOff, sizeof g_cacheOff, 1, f) == 1 &&
	          std::fread(&n, 4, 1, f) == 1;
	for (uint32_t i = 0; ok && i < n; ++i) {
		int32_t k[3];
		uint32_t c;
		ok = std::fread(k, sizeof k, 1, f) == 1 && std::fread(&c, 4, 1, f) == 1 && c <= 512;
		if (!ok) break;
		std::vector<ColBlock> b(c);
		ok = !c || std::fread(b.data(), sizeof(ColBlock), c, f) == c;
		if (ok) g_cache[std::make_tuple(k[0], k[1], k[2])] = std::move(b);
	}
	std::fclose(f);
	if (!ok) {
		g_cache.clear();
		console->Print("[bridge] collision cache: %s unreadable, ignored\n", CachePath(map).c_str());
		return;
	}
	g_cacheHaveOff = true, g_cacheSaved = g_cache.size();
	console->Print("[bridge] collision cache: %zu regions loaded for %s\n", g_cache.size(), map.c_str());
}
// the region's Source box touches an open portal's hole box?
static bool RegionTouchesHole(const std::tuple<int, int, int> &k) {
	const Vector a = McToSrc(std::get<0>(k) * kRegion, std::get<1>(k) * kRegion, std::get<2>(k) * kRegion);
	const Vector b = McToSrc((std::get<0>(k) + 1) * kRegion, (std::get<1>(k) + 1) * kRegion, (std::get<2>(k) + 1) * kRegion);
	const Vector mn(std::fmin(a.x, b.x), std::fmin(a.y, b.y), std::fmin(a.z, b.z)), mx(std::fmax(a.x, b.x), std::fmax(a.y, b.y), std::fmax(a.z, b.z));
	for (int i = 0; i < kPortalSlots; ++i)
		if (g_holeOn[i] && g_holeMn[i].x < mx.x && g_holeMx[i].x > mn.x && g_holeMn[i].y < mx.y && g_holeMx[i].y > mn.y && g_holeMn[i].z < mx.z && g_holeMx[i].z > mn.z)
			return true;
	return false;
}
static bool CacheUsable(const std::tuple<int, int, int> &k) { return g_cacheOn && !RegionTouchesHole(k); }
// send region k from the cache; false if it isn't there (or can't be used: then it is voxelised)
static bool CacheTrySend(const std::tuple<int, int, int> &k) {
	int d[3];
	if (!g_cacheOn || !CacheDelta(d)) return false;
	const auto ck = std::make_tuple(std::get<0>(k) - d[0] / kRegion, std::get<1>(k) - d[1] / kRegion, std::get<2>(k) - d[2] / kRegion);
	auto it = g_cache.find(ck);
	if (it == g_cache.end() || !CacheUsable(k)) return false;
	std::vector<ColBlock> blocks = it->second;
	for (ColBlock &b : blocks) b.x += d[0], b.y += d[1], b.z += d[2];
	const int rx = std::get<0>(k), ry = std::get<1>(k), rz = std::get<2>(k);
	ColRegion hdr{rx * kRegion, ry * kRegion, rz * kRegion, rx * kRegion + kRegion - 1, ry * kRegion + kRegion - 1, rz * kRegion + kRegion - 1, g_epoch, (uint32_t)blocks.size()};
	if (!link::WriteCollision(kColRegion, &hdr, sizeof hdr, blocks.data(), (uint32_t)(blocks.size() * sizeof(ColBlock)))) return false;
	g_lastSent[k] = std::move(blocks);
	++g_regionsSent, ++g_cacheHits;
	g_blocksSent += (int)g_lastSent[k].size();
	return true;
}
static void CacheStore(const std::tuple<int, int, int> &k, const std::vector<ColBlock> &blocks) {
	if (!g_cacheOn || g_cacheMap.empty()) return;
	if (!g_cacheHaveOff) CurOff(g_cacheOff), g_cacheHaveOff = true;
	int d[3];
	if (!CacheDelta(d)) return;
	std::vector<ColBlock> b = blocks;
	for (ColBlock &c : b) c.x -= d[0], c.y -= d[1], c.z -= d[2];
	g_cache[std::make_tuple(std::get<0>(k) - d[0] / kRegion, std::get<1>(k) - d[1] / kRegion, std::get<2>(k) - d[2] / kRegion)] = std::move(b);
	++g_cacheStores;
	if (g_cache.size() >= g_cacheSaved + 512) CacheSave();  // (a full rewrite: rare; normally saved at level start)
}

// A region is voxelised a slice of blocks per frame under the frame budget (lead, perf: a whole region took
// ~25 ms, so one region per frame was a 25 ms hitch every frame while the field filled or moving solids
// re-sent). g_work is the region in progress; anything that invalidates a region restarts it.
struct RegionWork {
	bool active = false;
	std::tuple<int, int, int> key;
	int next = 0;  // block index 0..511 (x fastest, then z, then y)
	bool dynNear = false;
	bool cacheable = false;  // no portal hole in it when started: store the result
	std::vector<ColBlock> blocks;
};
static RegionWork g_work;
static void WorkInvalidate(const std::tuple<int, int, int> &k) {
	if (g_work.active && g_work.key == k) g_work.active = false;
}
static void WorkReset() { g_work.active = false; }

// Advance the region in progress until the deadline; true once it is complete and written.
static bool StepRegion(std::chrono::steady_clock::time_point deadline) {
	const int rx = std::get<0>(g_work.key), ry = std::get<1>(g_work.key), rz = std::get<2>(g_work.key);
	if (g_work.next == 0 && g_work.blocks.empty()) {
		// static props in this region at all? (one box trace for the whole region)
		const Vector rc = McToSrc((rx + 0.5) * kRegion, (ry + 0.5) * kRegion, (rz + 0.5) * kRegion);
		if (NowS() - g_serverEntsAt > 0.25) g_serverEntsAt = NowS(), EntLiveSet(g_serverEnts);
		g_dynNear = true;
		g_work.dynNear = BoxHitsDyn(rc, kRegion * kUnitsPerBlock * 0.5f);
		g_work.cacheable = g_cacheOn && !RegionTouchesHole(g_work.key);  // (bridge #26: the static field never holds a mover)
		g_work.blocks.reserve(128);
	}
	g_dynNear = g_work.dynNear;
	const int total = kRegion * kRegion * kRegion;
	while (g_work.next < total) {
		const int i = g_work.next++;
		const int x = i % kRegion, z = (i / kRegion) % kRegion, y = i / (kRegion * kRegion);
		ColBlock b;
		if (VoxeliseBlock(rx * kRegion + x, ry * kRegion + y, rz * kRegion + z, b)) g_work.blocks.push_back(b);
		if ((i & 7) == 7 && std::chrono::steady_clock::now() > deadline) break;
	}
	if (g_work.next < total) return false;
	ColRegion hdr{rx * kRegion, ry * kRegion, rz * kRegion, rx * kRegion + kRegion - 1, ry * kRegion + kRegion - 1, rz * kRegion + kRegion - 1, g_epoch,
	              (uint32_t)g_work.blocks.size()};
	if (!link::WriteCollision(kColRegion, &hdr, sizeof hdr, g_work.blocks.data(), (uint32_t)(g_work.blocks.size() * sizeof(ColBlock)))) return false;  // ring full: next frame
	++g_regionsSent;
	g_blocksSent += (int)g_work.blocks.size();
	g_lastSent[g_work.key] = g_work.blocks;
	g_patches.erase(g_work.key);  // a whole region sent now supersedes its patches
	if (g_pw.active && g_pw.key == g_work.key) g_pw = PatchWork();
	if (g_work.cacheable) CacheStore(g_work.key, g_work.blocks);
	return true;
}

// ---- block-level patches (lead, perf) ---------------------------------------------------------------------------------
// Only the blocks inside a box are voxelised again and merged into what Minecraft already has (bridge #26: used by the fall
// guard, which redoes the static field under a floor Minecraft fell through).
static int g_patchesSent = 0, g_patchBlocks = 0;
static void DirtyBox(const Vector &origin, float radius) {
	const float r = radius + 8.0f;
	const Vector bmn = origin - Vector(r, r, r), bmx = origin + Vector(r, r, r);
	const Vector a = SrcToMc(bmn), b = SrcToMc(bmx);
	for (int rx = (int)std::floor(std::fmin(a.x, b.x) / kRegion); rx <= (int)std::floor(std::fmax(a.x, b.x) / kRegion); ++rx)
		for (int ry = (int)std::floor(std::fmin(a.y, b.y) / kRegion); ry <= (int)std::floor(std::fmax(a.y, b.y) / kRegion); ++ry)
			for (int rz = (int)std::floor(std::fmin(a.z, b.z) / kRegion); rz <= (int)std::floor(std::fmax(a.z, b.z) / kRegion); ++rz) {
				const auto k = std::make_tuple(rx, ry, rz);
				WorkInvalidate(k);  // a region half voxelised starts over (it reads the entity where it is now)
				if (HarvestActive()) { HarvestRegionDirty(k); continue; }
				if (g_sent.count(k) && g_lastSent.count(k)) g_patches[k].push_back({bmn, bmx});
				// (a region not sent yet is voxelised whole in its turn, with the entity where it is then)
			}
}
static bool BlockTouches(int bx, int by, int bz, const std::vector<std::pair<Vector, Vector>> &boxes) {
	const Vector a = McToSrc(bx, by, bz), b = McToSrc(bx + 1, by + 1, bz + 1);
	const Vector mn(std::fmin(a.x, b.x), std::fmin(a.y, b.y), std::fmin(a.z, b.z)), mx(std::fmax(a.x, b.x), std::fmax(a.y, b.y), std::fmax(a.z, b.z));
	for (const auto &bx2 : boxes)
		if (bx2.first.x < mx.x && bx2.second.x > mn.x && bx2.first.y < mx.y && bx2.second.y > mn.y && bx2.first.z < mx.z && bx2.second.z > mn.z) return true;
	return false;
}
// Advance the patch in progress (or start the next one) until the deadline; true when one was sent.
static bool StepPatch(const std::tuple<int, int, int> &k, std::chrono::steady_clock::time_point deadline) {
	if (!g_pw.active || g_pw.key != k) {
		auto pit = g_patches.find(k);
		if (pit == g_patches.end()) return true;
		g_pw = PatchWork();
		g_pw.active = true, g_pw.key = k, g_pw.boxes = pit->second;
		g_patches.erase(pit);  // (new boxes arriving meanwhile start a new patch after this one)
		for (const ColBlock &c : g_lastSent[k])
			if (!BlockTouches(c.x, c.y, c.z, g_pw.boxes)) g_pw.blocks.push_back(c);
		const int rx = std::get<0>(k), ry = std::get<1>(k), rz = std::get<2>(k);
		const Vector rc = McToSrc((rx + 0.5) * kRegion, (ry + 0.5) * kRegion, (rz + 0.5) * kRegion);
		if (NowS() - g_serverEntsAt > 0.25) g_serverEntsAt = NowS(), EntLiveSet(g_serverEnts);
		g_dynNear = true;
		g_pw.dynNear = BoxHitsDyn(rc, kRegion * kUnitsPerBlock * 0.5f);
	}
	g_dynNear = g_pw.dynNear;
	const int rx = std::get<0>(k), ry = std::get<1>(k), rz = std::get<2>(k);
	const int total = kRegion * kRegion * kRegion;
	while (g_pw.next < total) {
		const int i = g_pw.next++;
		const int bx = rx * kRegion + i % kRegion, bz = rz * kRegion + (i / kRegion) % kRegion, by = ry * kRegion + i / (kRegion * kRegion);
		if (!BlockTouches(bx, by, bz, g_pw.boxes)) continue;
		++g_patchBlocks;
		ColBlock c;
		if (VoxeliseBlock(bx, by, bz, c)) g_pw.blocks.push_back(c);
		if (std::chrono::steady_clock::now() > deadline) break;
	}
	if (g_pw.next < total) return false;
	ColRegion hdr{rx * kRegion, ry * kRegion, rz * kRegion, rx * kRegion + kRegion - 1, ry * kRegion + kRegion - 1, rz * kRegion + kRegion - 1, g_epoch, (uint32_t)g_pw.blocks.size()};
	if (!link::WriteCollision(kColRegion, &hdr, sizeof hdr, g_pw.blocks.data(), (uint32_t)(g_pw.blocks.size() * sizeof(ColBlock)))) return false;
	g_lastSent[k] = std::move(g_pw.blocks);
	g_pw = PatchWork();
	++g_patchesSent;
	return true;
}

// (bridge #26) movers.cpp sends nothing in an epoch before its kColClear
bool CollisionFieldCleared() { return g_cleared; }
uint32_t CollisionFieldEpoch() { return g_epoch; }
void CollisionFieldStage(bool on) { g_stageOn = on; }
void CollisionFieldTerrainOnly(bool on) {
	if (on != g_terrainOnly) g_sent.clear(), WorkReset(), ++g_hOptGen;
	g_terrainOnly = on;
}
void MoversLevelStart();  // movers.cpp (bridge #26)
void CollisionFieldLevelStart() {
	CacheSave();
	++g_epoch;
	g_cleared = false;
	g_sent.clear();
	g_everSent.clear();
	g_lastSent.clear(), g_patches.clear(), g_pw = PatchWork();
	WorkReset();
	HarvestLevelReset();
	MoversLevelStart();  // (bridge #26: the mod drops its movers with the clear)
}


// ---- (bridge #33) the harvest-based export (colharvest.cpp, REVIEW-freshEyes 3.2) -----------------------------------------
// pc_colfield_harvest 1: a region is voxelised from the level file's geometry (the BSP's brushes, displacements, tree and the static
// props' .phy hulls, parsed once per map) on worker threads, not by point queries on the main thread; the main thread only picks the
// next region, hands it over and writes the finished ones to the ring. The ColBlock format is the same, so the mod is unchanged.
// tools/colharvest measures the equivalence to the point-query export offline (README of #33). While the geometry loads, or if it cannot
// be read, the point-query path above keeps exporting. The dug cells of the Minecraft player (dig.cpp) are left out here: one filter.
static int g_hMode = 1;  // (lead, 2026-10-03) the harvest export by default: digging (on by default) needs its dug-cell filter
static int g_hClip = 1;
static bool g_hProps = true, g_hLeaf = true;
enum { kHNone, kHLoading, kHReady, kHFailed };
static int g_hState = kHNone;
static std::string g_hMap;
static std::shared_ptr<const colharvest::World> g_hWorld;
struct HJob {
	bool load = false;
	std::string map;
	std::tuple<int, int, int> key;
	uint32_t epoch = 0, gen = 0, optGen = 0;
	colharvest::Mapping mapping;
	colharvest::Options opt;
	std::shared_ptr<const colharvest::World> world;
};
struct HRes {
	std::tuple<int, int, int> key;
	uint32_t epoch = 0, gen = 0, optGen = 0;
	std::vector<ColBlock> blocks;
	double ms = 0;
};
static std::mutex g_hMu;
static std::condition_variable g_hCv;
static std::deque<HJob> g_hJobs;
PCB_GAUGE("col.hJobs", [] { std::lock_guard<std::mutex> lk(g_hMu); return g_hJobs.size(); }());
PCB_GAUGE("col.ringBacklogKB", link::CollisionBacklog() >> 10);
static std::deque<HRes> g_hDone;
static std::vector<std::thread> g_hThreads;
static bool g_hStop = false;
static struct {
	bool ready = false, ok = false;
	std::string map, log, err;
	std::shared_ptr<colharvest::World> world;
	double ms = 0;
} g_hLoad;
static std::map<std::tuple<int, int, int>, uint32_t> g_regionGen;  // bumped when a region's inputs change (a dug cell, a portal hole)
PCB_GAUGE("col.regionGen", g_regionGen.size());
static std::set<std::tuple<int, int, int>> g_hInflight;             // handed to a worker, not yet written
static std::map<std::tuple<int, int, int>, double> g_hInflightAt;    // (bridge #63) when each was handed over: the watchdog in HarvestFrame
PCB_GAUGE("col.hInflight", g_hInflight.size());
static std::set<std::tuple<int, int, int>> g_hUrgent;               // dirty regions: picked before the rest
PCB_GAUGE("col.hUrgent", g_hUrgent.size());
static std::deque<HRes> g_hPending;                                  // finished, waiting for room in the ring
PCB_GAUGE("col.hPending", g_hPending.size());
static std::shared_ptr<const colharvest::DugSet> g_dugSnap;           // dig.cpp's current set (CollisionFieldSetDug)
static double g_hMsTotal = 0;
static int g_hRegions = 0, g_hStale = 0;
static const int kHInflightMax = 6;

static bool HarvestActive() { return g_hMode != 0 && g_hState == kHReady && g_hWorld; }
// (bridge #76) the map's parsed geometry (terrain.cpp's cut caps clip the hole walls to it), or null while it loads
std::shared_ptr<const colharvest::World> CollisionFieldHarvestWorld() { return g_hState == kHReady ? g_hWorld : nullptr; }
void TerrainBandRebuild();  // terrain.cpp
static void HarvestRegionDirty(const std::tuple<int, int, int> &k) {
	if (StageMcRegionSkip(std::get<1>(k))) return;  // (bridge #47: the stage is Minecraft's own, nothing of Portal 2's is exported there)
	++g_regionGen[k];
	g_sent.erase(k);
	g_hUrgent.insert(k);
}
static std::string HarvestFindBsp(const std::string &map, std::vector<std::string> &gameDirs) {
	char cwd[1024] = "";
	if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
	// the game's working directory, then the folder of the running portal2_linux (the Portal 2 install, wherever Steam put it)
	std::string exeDir = plat::ExecutablePath();
	exeDir = exeDir.substr(0, exeDir.find_last_of('/') == std::string::npos ? 0 : exeDir.find_last_of('/'));
	const std::string roots[] = {std::string(cwd), exeDir};
	const char *games[] = {"update", "portal2_dlc2", "portal2_dlc1", "portal2"};
	for (const std::string &r : roots) {
		std::string found;
		for (const char *g : games) {
			const std::string p = r + "/" + g + "/maps/" + map + ".bsp";
			struct stat st;
			if (found.empty() && stat(p.c_str(), &st) == 0 && st.st_size > 1036) found = p;
		}
		if (found.empty()) continue;
		for (const char *g : games) gameDirs.push_back(r + "/" + g);
		return found;
	}
	return "";
}
static void HarvestWorker() {
	for (;;) {
		HJob job;
		{
			std::unique_lock<std::mutex> lk(g_hMu);
			g_hCv.wait(lk, [] { return g_hStop || !g_hJobs.empty(); });
			if (g_hStop) return;
			job = std::move(g_hJobs.front());
			g_hJobs.pop_front();
		}
		try {
			if (job.load) {
				const auto t0 = std::chrono::steady_clock::now();
				auto w = std::make_shared<colharvest::World>();
				std::vector<std::string> dirs;
				const std::string bsp = HarvestFindBsp(job.map, dirs);
				std::string err, log;
				bool ok = !bsp.empty() && w->Load(bsp, dirs, err, &log);
				if (bsp.empty()) err = "no BSP file found for " + job.map;
				std::lock_guard<std::mutex> lk(g_hMu);
				g_hLoad.ready = true, g_hLoad.ok = ok, g_hLoad.map = job.map, g_hLoad.log = log, g_hLoad.err = err, g_hLoad.world = ok ? w : nullptr;
				g_hLoad.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			} else {
				HRes r;
				r.key = job.key, r.epoch = job.epoch, r.gen = job.gen, r.optGen = job.optGen;
				const auto t0 = std::chrono::steady_clock::now();
				job.world->VoxeliseRegion(job.mapping, job.opt, std::get<0>(job.key), std::get<1>(job.key), std::get<2>(job.key), r.blocks);
				r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
				std::lock_guard<std::mutex> lk(g_hMu);
				g_hDone.push_back(std::move(r));
			}
		} catch (...) {  // (an allocation failure in the 32-bit address space: the region is simply not delivered and is asked for again)
			if (!job.load) {
				HRes r;
				r.key = job.key, r.epoch = 0;
				std::lock_guard<std::mutex> lk(g_hMu);
				g_hDone.push_back(std::move(r));
			}
		}
	}
}
static void HarvestStartThreads() {
	if (!g_hThreads.empty()) return;
	g_hStop = false;
	for (int i = 0; i < 2; ++i) g_hThreads.emplace_back(HarvestWorker);
}
void CollisionFieldShutdown() {  // plugin unload: stop the workers
	{
		std::lock_guard<std::mutex> lk(g_hMu);
		g_hStop = true;
	}
	g_hCv.notify_all();
	for (auto &t : g_hThreads) t.join();
	g_hThreads.clear();
}
static void HarvestBeginLoad(const std::string &map) {
	if (!g_hMode || map.empty()) return;
	if (g_hMap == map && (g_hState == kHReady || g_hState == kHLoading)) return;  // the same map again: its geometry is still here
	g_hMap = map, g_hState = kHLoading, g_hWorld = nullptr;
	HarvestStartThreads();
	HJob j;
	j.load = true, j.map = map;
	{
		std::lock_guard<std::mutex> lk(g_hMu);
		g_hLoad.ready = false;
		g_hJobs.push_back(std::move(j));
	}
	g_hCv.notify_one();
}
static void HarvestLevelReset() {
	std::lock_guard<std::mutex> lk(g_hMu);
	for (auto it = g_hJobs.begin(); it != g_hJobs.end();) it = it->load ? std::next(it) : g_hJobs.erase(it);
	g_hDone.clear();
	g_hPending.clear();
	g_hInflight.clear();
	g_hUrgent.clear();
	g_regionGen.clear();
}
// dig.cpp: the set of dug cells changed (sections' worth): those regions are voxelised again
void CollisionFieldSetDug(std::shared_ptr<const colharvest::DugSet> set) { g_dugSnap = std::move(set); }
static std::set<std::tuple<int, int, int>> g_digWatch;  // (bridge #35) regions made dirty by a dug cell, until their new version is written (logged then)
static int g_digResent = 0, g_digWatchLogged = 0;
void CollisionFieldDugRegionDirty(int rx, int ry, int rz) {
	HarvestRegionDirty(std::make_tuple(rx, ry, rz));
	g_digWatch.insert(std::make_tuple(rx, ry, rz));
}
int CollisionFieldDugResent() { return g_digResent; }
static colharvest::Options HarvestOptions() {
	colharvest::Options o;
	o.clipMode = g_hClip, o.staticProps = g_hProps, o.leafPoint = g_hLeaf;
	if (g_terrainOnly || g_stageOn) {
		Vector mn, mx;
		ClipRegionBounds(mn, mx);
		if (ClipRegionContains((mn + mx) * 0.5f)) {
			o.hasExclude = true;
			for (int i = 0; i < 3; ++i) o.exclude.mn[i] = (&mn.x)[i], o.exclude.mx[i] = (&mx.x)[i];
		}
	}
	for (int i = 0; i < kPortalSlots; ++i)
		if (g_holeOn[i] && o.nHoles < kPortalSlots) {
			colharvest::Box &b = o.holes[o.nHoles++];
			for (int t = 0; t < 3; ++t) b.mn[t] = (&g_holeMn[i].x)[t], b.mx[t] = (&g_holeMx[i].x)[t];
		}
	o.dug = g_dugSnap;
	return o;
}
// Take what the workers finished (and the load), and write it to the ring.
static void HarvestCollect() {
	std::deque<HRes> done;
	{
		std::lock_guard<std::mutex> lk(g_hMu);
		if (g_hLoad.ready) {
			g_hLoad.ready = false;
			if (g_hLoad.map == g_hMap) {
				if (g_hLoad.ok) {
					g_hWorld = g_hLoad.world, g_hState = kHReady;
					g_sent.clear(), g_hInflight.clear(), WorkReset();  // everything is voxelised again, the harvest's way
					TerrainBandRebuild();  // (bridge #76) the band's cut caps are built from this geometry
					console->Print("[bridge] harvest: %s ready in %.0f ms: %zu world brushes, %zu displacements, %zu static props (%zu without a collision model)\n%s", g_hMap.c_str(), g_hLoad.ms,
					               g_hWorld->brushCount(), g_hWorld->dispCount(), g_hWorld->propCount(), g_hWorld->propsMissing(), g_hLoad.log.c_str());
				} else {
					g_hState = kHFailed;
					console->Print("[bridge] harvest: %s could not be loaded (%s): the point-query export stays\n", g_hMap.c_str(), g_hLoad.err.c_str());
				}
			}
			g_hLoad.world = nullptr;
		}
		done.swap(g_hDone);
	}
	for (HRes &r : done) g_hPending.push_back(std::move(r));
	const auto t0 = std::chrono::steady_clock::now();
	int n = 0;
	while (!g_hPending.empty()) {
		if (n >= 24 || std::chrono::steady_clock::now() > t0 + std::chrono::milliseconds(3)) break;
		HRes &r = g_hPending.front();
		g_hInflight.erase(r.key);
		const auto k = r.key;
		auto git = g_regionGen.find(k);
		const bool fresh = r.epoch == g_epoch && r.optGen == g_hOptGen && r.gen == (git == g_regionGen.end() ? 0u : git->second);
		if (!fresh) {
			if (r.epoch == g_epoch) ++g_hStale;  // (made under an older state: it is picked again, unsent)
			g_hPending.pop_front();
			continue;
		}
		const int rx = std::get<0>(k), ry = std::get<1>(k), rz = std::get<2>(k);
		ColRegion hdr{rx * kRegion, ry * kRegion, rz * kRegion, rx * kRegion + kRegion - 1, ry * kRegion + kRegion - 1, rz * kRegion + kRegion - 1, g_epoch, (uint32_t)r.blocks.size()};
		if (!link::WriteCollision(kColRegion, &hdr, sizeof hdr, r.blocks.data(), (uint32_t)(r.blocks.size() * sizeof(ColBlock)))) {
			g_hInflight.insert(k), g_hInflightAt[k] = NowS();  // ring full: next frame
			break;
		}
		++g_regionsSent, ++g_hRegions, ++n;
		g_blocksSent += (int)r.blocks.size();
		g_hMsTotal += r.ms;
		{
			static int slow = 0;
			if (r.ms > 700.0 && slow++ < 12) console->Print("[bridge] colfield: harvest of region (%d %d %d) took %.0f ms on a worker (%zu blocks)\n", rx, ry, rz, r.ms, r.blocks.size());
		}
		g_lastSent[k] = std::move(r.blocks);
		g_sent.insert(k), g_everSent.insert(k);
		g_hUrgent.erase(k);
		if (g_digWatch.erase(k)) {  // (bridge #35) a region a dug cell changed is in Minecraft again, without the dug cells
			++g_digResent;
			if (g_digWatchLogged < 40) {
				++g_digWatchLogged;
				const size_t nDug = g_dugSnap ? g_dugSnap->size() : 0;
				console->Print("[bridge] dig: region MC %d %d %d re-exported without the dug cells (%zu blocks sent; %zu dug cells in the set)\n", rx, ry, rz, g_lastSent[k].size(), nDug);
			}
		}
		g_hPending.pop_front();
	}
}
// Hand the nearest regions that are not sent yet to the workers (same order as the point-query path: the feet layers first).
static void HarvestFrame(int prx, int pry, int prz, bool pre, int qrx, int qry, int qrz) {
	HarvestCollect();
	if (!HarvestActive()) return;
	auto inRadius = [&](const std::tuple<int, int, int> &k) {
		return std::abs(std::get<0>(k) - prx) <= g_radiusH && std::abs(std::get<2>(k) - prz) <= g_radiusH && std::abs(std::get<1>(k) - pry) <= g_radiusV;
	};
	auto post = [&](const std::tuple<int, int, int> &k) {
		HJob j;
		j.key = k, j.epoch = g_epoch, j.optGen = g_hOptGen;
		auto git = g_regionGen.find(k);
		j.gen = git == g_regionGen.end() ? 0u : git->second;
		j.mapping.mcX = g_map.mcX, j.mapping.mcY = g_map.mcY, j.mapping.mcZ = g_map.mcZ;
		j.mapping.sx = g_map.src.x, j.mapping.sy = g_map.src.y, j.mapping.sz = g_map.src.z;
		j.mapping.upb = kUnitsPerBlock;
		j.opt = HarvestOptions();
		j.world = g_hWorld;
		g_hInflight.insert(k);
		g_hInflightAt[k] = NowS();
		{
			std::lock_guard<std::mutex> lk(g_hMu);
			g_hJobs.push_back(std::move(j));
		}
		g_hCv.notify_one();
	};
	// (bridge #63) watchdog: a job handed to a worker that has not come back in 3 s is forgotten and asked for again (bugmark 20261009-065600: "6 in flight,
	// 0 waiting, 0 jobs" for minutes: the inflight cap was reached by jobs that never returned, so no region was asked for any more and the hand-over
	// waited for a region "not sent" for ever). A late result of the forgotten job is still a fresh result and is written; a duplicate is harmless.
	{
		const double nowW = NowS();
		static int wdLogs = 0;
		// (bridge #69) jobs still WAITING for a worker are not lost: with 2 workers and 6 in flight a region that takes a second or two to voxelise (the wake-up's
		// stairwell) put the last jobs past 3 s, the watchdog posted each again, and the duplicates (24 queued) made every job slower (a teleport there
		// let Minecraft fall: "gave up", the fall guard). Only an idle queue means a job was lost.
		size_t queued = 0;
		{
			std::lock_guard<std::mutex> lk(g_hMu);
			queued = g_hJobs.size();
		}
		for (auto it = g_hInflight.begin(); it != g_hInflight.end();) {
			auto at = g_hInflightAt.find(*it);
			if (at != g_hInflightAt.end() && (nowW - at->second < 3.0 || (queued > 0 && nowW - at->second < 30.0))) { ++it; continue; }
			if (wdLogs++ < 20) console->Print("[bridge] colfield: harvest job for region (%d %d %d) did not return in 3 s: asked for again (%zu in flight)\n", std::get<0>(*it), std::get<1>(*it), std::get<2>(*it), g_hInflight.size());
			g_hInflightAt.erase(*it);
			it = g_hInflight.erase(it);
		}
	}
	while ((int)g_hInflight.size() < kHInflightMax) {
		int best = 1 << 30;
		std::tuple<int, int, int> pick{};
		for (auto it = g_hUrgent.begin(); it != g_hUrgent.end();) {  // dirty regions near the player first (dug cells, portal holes)
			if (g_sent.count(*it) || g_hInflight.count(*it)) { it = (g_sent.count(*it) ? g_hUrgent.erase(it) : std::next(it)); continue; }
			if (inRadius(*it)) {
				const int dx = std::get<0>(*it) - prx, dy = std::get<1>(*it) - pry, dz = std::get<2>(*it) - prz;
				const int d = -1000 + dx * dx + dz * dz + (dy == -1 ? 0 : 4 * dy * dy);
				if (d < best) best = d, pick = *it;
			} else {
				it = g_hUrgent.erase(it);  // out of range: voxelised in its turn when it comes back
				continue;
			}
			++it;
		}
		if (best == (1 << 30)) {
			for (int dy = -g_radiusV; dy <= g_radiusV; ++dy)
				for (int dz = -g_radiusH; dz <= g_radiusH; ++dz)
					for (int dx = -g_radiusH; dx <= g_radiusH; ++dx) {
						auto k = std::make_tuple(prx + dx, pry + dy, prz + dz);
						if (g_sent.count(k) || g_hInflight.count(k)) continue;
						const int d = dx * dx + dz * dz + (dy == -1 ? 0 : 4 * dy * dy);
						if (d < best) best = d, pick = k;
					}
			if (pre)
				for (int dy = -2; dy <= 1; ++dy)
					for (int dz = -1; dz <= 1; ++dz)
						for (int dx = -1; dx <= 1; ++dx) {
							auto k = std::make_tuple(qrx + dx, qry + dy, qrz + dz);
							const int d = 3 + dx * dx + dz * dz + (dy == -1 || dy == 0 ? 0 : 2);
							if (!g_sent.count(k) && !g_hInflight.count(k) && d < best) best = d, pick = k, ++g_prefetched;
						}
		}
		if (best == (1 << 30)) break;
		post(pick);
	}
}
bool CollisionFieldHarvestActive() { return HarvestActive(); }
bool CollisionFieldHarvestWanted() { return g_hMode != 0; }
void CollisionFieldHarvestWant(bool on) {  // dig.cpp: digging needs the harvest's dug-cell filter
	if (on && !g_hMode) g_hMode = 1, HarvestBeginLoad(McMapName());
}

// Called once per frame while Minecraft is linked.
// exportRegions false: only (re)send the clear, e.g. in terrain mode where Minecraft's own
// world is the ground and Portal 2 geometry must not appear in it.
// The regions the Minecraft player stands on and steps into next (3x3 around the feet, the feet layer and the one
// under it) have all been sent: until then Minecraft must not lead (lead, perf: the export is now sliced over frames,
// so right after a switch, a respawn or a ride the floor can still be on its way; Portal 2's body leads meanwhile).
// Collision work still queued for the regions around the feet (moved solids' patches, a patch in progress, dirty
// regions): a carried player waits for these only (lead: waiting for every patch in range kept the Minecraft player
// "carried" in the arrival elevator for many seconds after the ride, 172 patches queued from the static cache)
bool CollisionFieldFeetPending(const Vector &feetSrc) {
	const Vector mc = SrcToMc(feetSrc);
	const int prx = (int)std::floor(mc.x / kRegion), pry = (int)std::floor(mc.y / kRegion), prz = (int)std::floor(mc.z / kRegion);
	if (StageMcRegionSkip(pry)) return false;  // (bridge #47)
	auto near = [&](const std::tuple<int, int, int> &k) {
		return std::abs(std::get<0>(k) - prx) <= 1 && std::abs(std::get<2>(k) - prz) <= 1 && std::get<1>(k) - pry >= -1 && std::get<1>(k) - pry <= 0;
	};
	if (g_pw.active && near(g_pw.key)) return true;
	for (const auto &e : g_patches)
		if (near(e.first)) return true;
	return false;
}

static char g_feetWhy[128] = "";
const char *CollisionFieldFeetWhy() { return g_feetWhy; }
bool CollisionFieldFeetReady(const Vector &feetSrc) {
	if (!link::McAlive()) return true;
	const Vector mc = SrcToMc(feetSrc);
	const int prx = (int)std::floor(mc.x / kRegion), pry = (int)std::floor(mc.y / kRegion), prz = (int)std::floor(mc.z / kRegion);
	if (StageMcRegionSkip(pry)) return true;  // (bridge #47: Minecraft's own ground)
	// (bridge #63) a neighbour region only counts when the feet are within 3 blocks of it: the 3x3 around the feet kept a hand-over waiting for a
	// region 4 blocks away that was never sent (Wheatley's room: "region -1 +0 +1 not sent", the pending switch never completed). The rest
	// of the ring is exported as the player walks, as in every other case.
	const double fx = mc.x - prx * kRegion, fz = mc.z - prz * kRegion;
	auto reaches = [&](int dx, int dz) {
		return (dx == 0 || (dx < 0 ? fx : kRegion - fx) < 3.0) && (dz == 0 || (dz < 0 ? fz : kRegion - fz) < 3.0);
	};
	for (int dy = -1; dy <= 0; ++dy)
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx) {
				if (!reaches(dx, dz)) continue;
				const auto k = std::make_tuple(prx + dx, pry + dy, prz + dz);
				if (!g_everSent.count(k))
					return std::snprintf(g_feetWhy, sizeof g_feetWhy, "region %+d %+d %+d not sent (%zu in flight, %zu waiting for the ring, %zu jobs)", dx, dy, dz, g_hInflight.size(), g_hPending.size(), [] { std::lock_guard<std::mutex> lk(g_hMu); return g_hJobs.size(); }()), false;  // (a moved solid's re-send keeps the old data in Minecraft meanwhile)
				// (lead: a movable solid not yet patched into the region the feet stand in or on, which was sent from the
				// static cache, may be the floor; only those two regions hold the player, so a door patch nearby doesn't)
				if (dx == 0 && dz == 0 && ((g_pw.active && g_pw.key == k) || g_patches.count(k)))
					return std::snprintf(g_feetWhy, sizeof g_feetWhy, "patch pending in region %+d %+d %+d", dx, dy, dz), false;
			}
	return true;
}

void CollisionFieldFrame(const Vector &playerSrc, bool exportRegions) {
	if (!link::McAlive()) return;
	if (!g_cleared) {
		if (!link::WriteCollision(kColClear, &g_epoch, sizeof g_epoch, nullptr, 0)) return;
		g_cleared = true;
		console->Print("[bridge] collision: epoch %u\n", g_epoch);
		if (g_cacheMap != McMapName()) CacheSave(), CacheLoad(McMapName());
		HarvestBeginLoad(McMapName());
	}
	for (int i = 0; i < kPortalSlots; ++i) g_qOn[i] = PortalHoleBox(i, g_qMn[i], g_qMx[i]);  // (bridge #45) the holes now, for the host's own rules
	// (bridge #35) digging: Chell's hull next to a dug cell reads the exported voxels (blockcollide.cpp), and the harvest is collected here, so
	// with pc_dig on the harvest keeps exporting around her in Chell mode too (dig2: it never did, so the voxels were never "complete" and the
	// engine's world, which still has the dug floor, stayed her answer)
	if (!exportRegions && !(g_hMode && DigEnabled())) return;
	// the open portals' holes baked into the export (g_carveExport only): a change sends the regions around the old and the new hole again
	for (int i = 0; i < kPortalSlots; ++i) {
		Vector mn, mx;
		const bool on = g_carveExport && PortalHoleBox(i, mn, mx);
		if (on == g_holeOn[i] && (!on || ((mn - g_holeMn[i]).Length() < 0.5f && (mx - g_holeMx[i]).Length() < 0.5f))) continue;
		for (int k = 0; k < 2; ++k) {
			if (k == 0 ? !g_holeOn[i] : !on) continue;
			const Vector a = SrcToMc(k == 0 ? g_holeMn[i] : mn), b = SrcToMc(k == 0 ? g_holeMx[i] : mx);
			for (int rx = (int)std::floor(std::fmin(a.x, b.x) / kRegion); rx <= (int)std::floor(std::fmax(a.x, b.x) / kRegion); ++rx)
				for (int ry = (int)std::floor(std::fmin(a.y, b.y) / kRegion); ry <= (int)std::floor(std::fmax(a.y, b.y) / kRegion); ++ry)
					for (int rz = (int)std::floor(std::fmin(a.z, b.z) / kRegion); rz <= (int)std::floor(std::fmax(a.z, b.z) / kRegion); ++rz) WorkInvalidate(std::make_tuple(rx, ry, rz)), HarvestRegionDirty(std::make_tuple(rx, ry, rz));
		}
		g_holeOn[i] = on, g_holeMn[i] = mn, g_holeMx[i] = mx;
	}
	Vector mc = SrcToMc(playerSrc);
	int prx = (int)std::floor(mc.x / kRegion), pry = (int)std::floor(mc.y / kRegion), prz = (int)std::floor(mc.z / kRegion);
	if (StageMcRegionSkip(pry)) return;  // (bridge #47) the player is on the stage: Minecraft's own ground, nothing of Portal 2's to export
	// (bridge #20) the far side of a portal the player is about to go through: the regions where it comes out are sent
	// ahead, on the ordinary per-frame budget, once everything around the player is in. The regions around an open
	// portal's hole are never in the static cache, so arriving there voxelised them under the feet budget (20 ms frames:
	// the hitch right after a transit, c20_smooth1m) and Portal 2 carried the body until they were in.
	bool pre = false;
	int qrx = 0, qry = 0, qrz = 0;
	if (g_prefetchOn)
		for (int i = 0; i < kPortalSlots && !pre; ++i)  // (bridge #68: every linked pair, not only the gun's: the map's linked_portal_doors send the player 4000 u away)
			if (PortalLinked(i) && (playerSrc - g_portalPos[i]).Length() < 280.0f) {
				const int j = i ^ 1;
				// the exit feet: under a wall portal's centre on its floor, or on a floor portal
				const Vector exitFeet = g_portalPos[j] + g_portalNormal[j] * 20.0f - Vector(0, 0, std::fabs(g_portalNormal[j].z) > 0.7f ? 0.0f : g_portalHalfH[j]);
				const Vector q = SrcToMc(exitFeet);
				qrx = (int)std::floor(q.x / kRegion), qry = (int)std::floor(q.y / kRegion), qrz = (int)std::floor(q.z / kRegion);
				pre = (std::abs(qrx - prx) > 1 || std::abs(qrz - prz) > 1 || std::abs(qry - pry) > 1) && !StageMcRegionSkip(qry);  // (not the player's own neighbourhood; not the stage, bridge #47)
			}
	auto inPre = [&](const std::tuple<int, int, int> &k) {
		return pre && std::abs(std::get<0>(k) - qrx) <= 1 && std::abs(std::get<2>(k) - qrz) <= 1 && std::get<1>(k) - qry >= -2 && std::get<1>(k) - qry <= 1;
	};
	const auto t0 = std::chrono::steady_clock::now();
	const auto deadline = t0 + std::chrono::microseconds((long long)(g_budgetMs * 1000.0));
	int cacheSends = 0;
	PROF("colfield.export");  // (bridge #25: patches + regions below)
	if (g_hMode) {
		HarvestFrame(prx, pry, prz, pre, qrx, qry, qrz);
		if (HarvestActive() || !exportRegions) return;  // (the workers voxelise; nothing else to do on the main thread)
	}
	// moved solids' patches first (only the blocks the entity covers, resumable): the ones by the feet get the feet
	// budget, the rest the normal one; far ones are dropped and redone whole when they come back in range
	{
		auto inRange = [&](const std::tuple<int, int, int> &k) {
			return std::abs(std::get<0>(k) - prx) <= g_radiusH && std::abs(std::get<2>(k) - prz) <= g_radiusH && std::abs(std::get<1>(k) - pry) <= g_radiusV;
		};
		auto byFeet = [&](const std::tuple<int, int, int> &k) {
			return std::abs(std::get<0>(k) - prx) <= 1 && std::abs(std::get<2>(k) - prz) <= 1 && std::get<1>(k) - pry >= -1 && std::get<1>(k) - pry <= 0;
		};
		if (g_pw.active && !inRange(g_pw.key)) g_sent.erase(g_pw.key), g_pw = PatchWork();
		for (auto it = g_patches.begin(); it != g_patches.end();)
			if (!inRange(it->first)) g_sent.erase(it->first), it = g_patches.erase(it);
			else ++it;
		for (int guard = 0; guard < 64; ++guard) {
			std::tuple<int, int, int> k;
			if (g_pw.active) k = g_pw.key;
			else if (!g_patches.empty()) {
				// nearest first (lead: in map order, a moved floor panel ahead of a walking player was patched after it
				// had been walked onto, and the Minecraft player sank through it)
				int best = 1 << 30;
				for (const auto &e : g_patches) {
					const int dx = std::get<0>(e.first) - prx, dy = std::get<1>(e.first) - pry, dz = std::get<2>(e.first) - prz;
					const int d = dx * dx + dz * dz + (dy == -1 ? 0 : 4 * dy * dy);
					if (d < best) best = d, k = e.first;
				}
			} else break;
			const auto dl = t0 + std::chrono::microseconds((long long)((byFeet(k) ? (g_boost ? g_nearBudgetMs : g_patchFeetBudgetMs) : g_budgetMs) * 1000.0));
			if (std::chrono::steady_clock::now() > dl) break;
			if (!StepPatch(k, dl)) break;  // out of time (or the ring is full)
		}
	}
	for (;;) {
		if (g_work.active) {
			// a region that left the export radius is dropped (it is picked again when it comes back)
			const auto &k = g_work.key;
			if ((std::abs(std::get<0>(k) - prx) > g_radiusH || std::abs(std::get<2>(k) - prz) > g_radiusH || std::abs(std::get<1>(k) - pry) > g_radiusV) && !inPre(k)) g_work.active = false;
			if (g_sent.count(k)) g_work.active = false;
		}
		if (!g_work.active) {
			// nearest unsent region
			int best = 1 << 30;
			std::tuple<int, int, int> pick;
			for (int dy = -g_radiusV; dy <= g_radiusV; ++dy)
				for (int dz = -g_radiusH; dz <= g_radiusH; ++dz)
					for (int dx = -g_radiusH; dx <= g_radiusH; ++dx) {
						auto k = std::make_tuple(prx + dx, pry + dy, prz + dz);
						if (g_sent.count(k)) continue;
						int d = dx * dx + dz * dz + (dy == -1 ? 0 : 4 * dy * dy);  // the layer under the feet (the floor) as soon as the feet layer
						if (d < best) best = d, pick = k;
					}
			if (pre)  // the far side of the portal it is near: right after the player's own neighbours (d 0..2)
				for (int dy = -2; dy <= 1; ++dy)
					for (int dz = -1; dz <= 1; ++dz)
						for (int dx = -1; dx <= 1; ++dx) {
							auto k = std::make_tuple(qrx + dx, qry + dy, qrz + dz);
							const int d = 3 + dx * dx + dz * dz + (dy == -1 || dy == 0 ? 0 : 2);
							if (!g_sent.count(k) && d < best) best = d, pick = k, ++g_prefetched;
						}
			if (best == (1 << 30)) return;
			if (CacheTrySend(pick)) {  // from the cache: no voxelising
				g_sent.insert(pick), g_everSent.insert(pick);
				// (cached regions are cheap: limited by count and a 4 ms cap, not by the budget the patches above used up;
				// starving them left the region ahead unsent, and Portal 2 carried the player mid-walk, lead 2026-10-02)
				if (++cacheSends >= 96 || std::chrono::steady_clock::now() > t0 + std::chrono::milliseconds(4) + std::chrono::microseconds((long long)(g_nearBudgetMs * 1000.0))) return;
				continue;
			}
			g_work.active = true, g_work.key = pick, g_work.next = 0, g_work.blocks.clear();
		}
		// the regions right around and under the feet are what the Minecraft player stands on: those get a
		// larger budget (a short hitch right after a switch or a teleport beats falling through an unsent floor)
		const auto &wk = g_work.key;
		const int adx = std::abs(std::get<0>(wk) - prx), adz = std::abs(std::get<2>(wk) - prz), ddy = std::get<1>(wk) - pry;
		const bool feetLayers = ddy >= -1 && ddy <= 0;
		// (bridge #20: a moved solid's region only gets the feet budget when it is by the feet; anywhere in the export
		// radius it made every frame a 20 ms frame while a ramp panel animated 10 m away: c20_smooth1n frames 67-77)
		const bool underFeet = feetLayers && adx <= 1 && adz <= 1;  // what the player stands on now
		const bool nearFeet = underFeet || (feetLayers && adx <= 2 && adz <= 2);                  // where the next steps land
		const auto nearDeadline = t0 + std::chrono::microseconds((long long)((underFeet ? g_nearBudgetMs : g_stepBudgetMs) * 1000.0));
		auto s = std::chrono::steady_clock::now();
		if (s > (nearFeet ? nearDeadline : deadline)) return;  // this frame's share is spent
		const bool done = StepRegion(nearFeet ? nearDeadline : deadline);
		g_msTotal += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s).count();
		if (!done) return;  // out of time (or the ring is full): continue next frame
		g_sent.insert(g_work.key);
		g_everSent.insert(g_work.key);
		if (g_traceSends > 0) {
			--g_traceSends;
			console->Print("[bridge] colfield sent (%d %d %d) rel (%d %d %d) near %d, %zu blocks, player region (%d %d %d)\n", std::get<0>(wk), std::get<1>(wk), std::get<2>(wk),
			               std::get<0>(wk) - prx, std::get<1>(wk) - pry, std::get<2>(wk) - prz, nearFeet ? 1 : 0, g_work.blocks.size(), prx, pry, prz);
		}
		g_work.active = false;
	}
}

CON_COMMAND(pcb_colfield_hole_export, "pcb_colfield_hole_export [0|1] - (bridge #45) bake the portal holes into the exported collision field again (the old way; default 0: the mod carves the Minecraft player's collision at query time)\n") {
	if (args.ArgC() > 1) g_carveExport = std::atoi(args[1]) != 0;
	console->Print("[bridge] portal holes baked into the export: %d\n", g_carveExport);
}
CON_COMMAND(pcb_colfield_prefetch, "pcb_colfield_prefetch [0|1] - send the regions on the far side of a portal the player is near ahead (default 1)\n") {
	if (args.ArgC() > 1) g_prefetchOn = std::atoi(args[1]) != 0;
	console->Print("[bridge] collision prefetch through portals %s, %d regions picked so far\n", g_prefetchOn ? "on" : "off", g_prefetched);
}
CON_COMMAND(pcb_colfield_block, "pcb_colfield_block <x> <y> <z> - voxelise the Minecraft block at a Portal 2 point now and print its 8 layers (solid sub-voxels per layer)\n") {
	if (args.ArgC() < 4) return;
	const Vector mc = SrcToMc(Vector((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3])));
	const int bx = (int)std::floor(mc.x), by = (int)std::floor(mc.y), bz = (int)std::floor(mc.z);
	if (NowS() - g_serverEntsAt > 0.25) g_serverEntsAt = NowS(), EntLiveSet(g_serverEnts);
	g_dynNear = true;
	ColBlock b{};
	const bool any = VoxeliseBlock(bx, by, bz, b);
	char buf[200];
	int n = 0;
	for (int y = 0; y < 8; ++y) n += std::snprintf(buf + n, sizeof buf - n, " %d", __builtin_popcountll(b.bits[y]));
	console->Print("[bridge] block MC %d %d %d (from %s %s %s): %s, layers bottom->top:%s\n", bx, by, bz, args[1], args[2], args[3], any ? "solid" : "empty", any ? buf : "");
}
CON_COMMAND(pcb_colfield_static, "pcb_colfield_static [0|1] - static props (stairs, walkways) in the Minecraft player's collision\n") {
	if (args.ArgC() > 1) g_staticOn = std::atoi(args[1]) != 0, g_sent.clear(), WorkReset();
	console->Print("[bridge] collision field: static props %s\n", g_staticOn ? "on" : "off");
}
// (bridge #26) the old name: the movers now (movers.cpp)
CON_COMMAND(pcb_colfield_dyn, "pcb_colfield_dyn - (bridge #26) the movers near the player (pcb_movers)\n") { engine.Cmd("pcb_movers near 600"); }

CON_COMMAND(pcb_colfield_cache, "pcb_colfield_cache [0|1|save|clear] - the static collision cache (cache/bridge/colcache)\n") {
	if (args.ArgC() > 1) {
		if (!std::strcmp(args[1], "save")) CacheSave();
		else if (!std::strcmp(args[1], "clear")) g_cache.clear(), g_cacheHaveOff = false, g_cacheSaved = 0, std::remove(CachePath(g_cacheMap).c_str()), std::remove(CacheKeyPath(g_cacheMap).c_str());
		else g_cacheOn = std::atoi(args[1]) != 0;
	}
	console->Print("[bridge] collision cache %s: map %s, %zu regions (%zu on disk), %d hits, %d stores\n", g_cacheOn ? "on" : "off", g_cacheMap.c_str(), g_cache.size(), g_cacheSaved,
	               g_cacheHits, g_cacheStores);
}

// Voxelise every region in a Portal 2 box now (no budget) into the cache, then save it: run once per map.
CON_COMMAND(pcb_colfield_prebuild, "pcb_colfield_prebuild <x0 y0 z0 x1 y1 z1> - voxelise every region in this Portal 2 box into the collision cache now\n") {
	if (args.ArgC() < 7) return console->Print("%s", pcb_colfield_prebuild.ThisPtr()->m_pszHelpString);
	if (g_cacheMap.empty()) CacheLoad(McMapName());
	const Vector a = SrcToMc(Vector((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3])));
	const Vector b = SrcToMc(Vector((float)std::atof(args[4]), (float)std::atof(args[5]), (float)std::atof(args[6])));
	int lo[3] = {(int)std::floor(std::fmin(a.x, b.x) / kRegion), (int)std::floor(std::fmin(a.y, b.y) / kRegion), (int)std::floor(std::fmin(a.z, b.z) / kRegion)};
	int hi[3] = {(int)std::floor(std::fmax(a.x, b.x) / kRegion), (int)std::floor(std::fmax(a.y, b.y) / kRegion), (int)std::floor(std::fmax(a.z, b.z) / kRegion)};
	const auto t0 = std::chrono::steady_clock::now();
	int done = 0, skipped = 0, skHole = 0, skMoving = 0;
	int d[3];
	for (int ry = lo[1]; ry <= hi[1]; ++ry)
		for (int rz = lo[2]; rz <= hi[2]; ++rz)
			for (int rx = lo[0]; rx <= hi[0]; ++rx) {
				const auto k = std::make_tuple(rx, ry, rz);
				if (CacheDelta(d) && g_cache.count(std::make_tuple(rx - d[0] / kRegion, ry - d[1] / kRegion, rz - d[2] / kRegion))) { ++skipped; continue; }
				if (RegionTouchesHole(k)) { ++skHole; ++skipped; continue; }
				const auto keep = g_work;
				g_work = RegionWork();
				g_work.active = true, g_work.key = k;
				const Vector rc = McToSrc((rx + 0.5) * kRegion, (ry + 0.5) * kRegion, (rz + 0.5) * kRegion);
				if (NowS() - g_serverEntsAt > 0.25) g_serverEntsAt = NowS(), EntLiveSet(g_serverEnts);
				g_dynNear = true;
				g_work.dynNear = BoxHitsDyn(rc, kRegion * kUnitsPerBlock * 0.5f);
				g_dynNear = g_work.dynNear;
				for (int i = 0; i < kRegion * kRegion * kRegion; ++i) {
					ColBlock c;
					if (VoxeliseBlock(rx * kRegion + i % kRegion, ry * kRegion + i / (kRegion * kRegion), rz * kRegion + (i / kRegion) % kRegion, c)) g_work.blocks.push_back(c);
				}
				CacheStore(k, g_work.blocks);
				g_work = keep;
				++done;
			}
	CacheSave();
	console->Print("[bridge] collision prebuild: %d regions voxelised, %d skipped (%d portal hole, %d moving solid, the rest cached), %.1f s\n", done, skipped, skHole, skMoving,
	               std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
}

CON_COMMAND(pcb_colfield_playerclip, "pcb_colfield_playerclip [0|1] - export Valve's playerclip brushes as solid (1, the old behaviour) or only floors over nothing (0); stats\n") {
	if (args.ArgC() > 1) {
		g_clipOn = std::atoi(args[1]) != 0;
		g_sent.clear(), WorkReset(), g_cacheOn = !g_clipOn;  // (the cache never stores the old rule's clip)
	}
	console->Print("[bridge] collision: playerclip %s; clip sub-voxels kept (floor over nothing) %d, dropped %d\n", g_clipOn ? "exported" : "dropped", g_clipKept, g_clipDropped);
}
CON_COMMAND(pcb_contents, "pcb_contents <x> <y> <z> - world contents at a Portal 2 point (playerclip 0x10000) and whether the export makes it solid\n") {
	if (args.ArgC() < 4) return;
	const Vector p((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3]));
	const int c = engine.GetPointContentsWorld(engine.traceServer->ThisPtr(), p, MASK_ALL);
	console->Print("[bridge] contents at (%.0f %.0f %.0f): 0x%x (solid %d, playerclip %d), exported solid %d, floor over nothing %d\n", p.x, p.y, p.z, c, (c & MASK_SOLID) != 0,
	               (c & CONTENTS_PLAYERCLIP) != 0, WorldSolidAt(p), ClipFloorOverVoid(p));
}
CON_COMMAND(pcb_colfield_trace, "pcb_colfield_trace <n> - log the next n collision regions sent (key, offset from the player, near-feet budget)\n") {
	g_traceSends = args.ArgC() > 1 ? std::atoi(args[1]) : 40;
}

CON_COMMAND(pcb_colfield, "pcb_colfield [radiusH regions] [radiusV] [budget ms] - CollisionField export stats / settings\n") {
	if (args.ArgC() > 1) g_radiusH = std::atoi(args[1]);
	if (args.ArgC() > 2) g_radiusV = std::atoi(args[2]);
	if (args.ArgC() > 3) g_budgetMs = std::atof(args[3]);
	console->Print("[bridge] collision field: epoch %u, %d regions (%d solid blocks) sent, %.1f ms voxelising (%.1f ms/region), radius %d/%d regions, budget %.1f ms/frame, "
	               "ring backlog %llu bytes\n",
	               g_epoch, g_regionsSent, g_blocksSent, g_msTotal, g_regionsSent ? g_msTotal / g_regionsSent : 0.0, g_radiusH, g_radiusV, g_budgetMs,
	               (unsigned long long)link::CollisionBacklog());
	console->Print("[bridge] collision patches: %d sent (%d blocks redone), %zu pending\n", g_patchesSent, g_patchBlocks, g_patches.size());
}

// ---- (bridge #24) read-only accessors for the collision overlay (coloverlay.cpp) and bug marks (bugmark.cpp) -----------
// What Minecraft has (g_lastSent, patches merged) for the blocks in an inclusive Minecraft block box.
void CollisionFieldSentIn(int bx0, int by0, int bz0, int bx1, int by1, int bz1, std::vector<ColBlock> &out) {
	out.clear();
	auto rf = [](int b) { return (int)std::floor(b / (double)kRegion); };
	for (int ry = rf(by0); ry <= rf(by1); ++ry)
		for (int rz = rf(bz0); rz <= rf(bz1); ++rz)
			for (int rx = rf(bx0); rx <= rf(bx1); ++rx) {
				auto it = g_lastSent.find(std::make_tuple(rx, ry, rz));
				if (it == g_lastSent.end()) continue;
				for (const ColBlock &b : it->second)
					if (b.x >= bx0 && b.x <= bx1 && b.y >= by0 && b.y <= by1 && b.z >= bz0 && b.z <= bz1) out.push_back(b);
			}
}
// 0: never sent this epoch; 1: Minecraft has it; 2: it has it, but a moved solid's patch / re-send is still queued
int CollisionFieldRegionState(int rx, int ry, int rz) {
	const auto k = std::make_tuple(rx, ry, rz);
	if (!g_lastSent.count(k)) return 0;
	if (g_patches.count(k) || (g_pw.active && g_pw.key == k)) return 2;
	return 1;
}
int CollisionFieldRegionSize() { return kRegion; }
// (bridge #26) the bound movers' boxes near the player (Source, world AABBs where they are now): blue in the overlay
void MoversBoxesNear(const Vector &p, float r, std::vector<std::pair<Vector, Vector>> &out);  // movers.cpp
bool CollisionFieldPlatformBoxes(std::vector<std::pair<Vector, Vector>> &out) {
	MoversBoxesNear(PlayerOrigin(), 480.0f, out);
	return !out.empty();
}
// voxelise one block now, as the export would (pcb_colfield_block)
bool CollisionFieldVoxeliseNow(int bx, int by, int bz, ColBlock &out) {
	if (NowS() - g_serverEntsAt > 0.25) g_serverEntsAt = NowS(), EntLiveSet(g_serverEnts);
	const bool dn = g_dynNear;
	g_dynNear = true;
	out = ColBlock{};
	const bool any = VoxeliseBlock(bx, by, bz, out);
	g_dynNear = dn;
	return any;
}
// the movers: entity, origin now, origin before its last move (movers.cpp)
void MoversTracked(std::vector<std::tuple<void *, Vector, Vector>> &out);
void CollisionFieldDynTracked(std::vector<std::tuple<void *, Vector, Vector>> &out) { MoversTracked(out); }
// pcb_dyn_probe's answer as values: a mover's box (bridge #26), the world (as exported), everything
CON_COMMAND(pcb_dyn_probe, "pcb_dyn_probe <x> <y> <z> [half=0] - is there a mover's box (Minecraft's copy) there, and what the world says\n") {
	if (args.ArgC() < 4) return;
	const Vector p((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3]));
	const float h = args.ArgC() > 4 ? (float)std::atof(args[4]) : 0.0f;
	int dyn, world, all;
	std::string what;
	void CollisionFieldDynProbe(const Vector &p, float h, int &dyn, int &world, int &all, std::string &what);
	CollisionFieldDynProbe(p, h, dyn, world, all, what);
	console->Print("[bridge] dyn probe (%.0f %.0f %.0f) half %.1f: mover box %d, world %d, everything %d (%s)\n", p.x, p.y, p.z, h, dyn, world, all, what.c_str());
}
void CollisionFieldDynProbe(const Vector &p, float h, int &dyn, int &world, int &all, std::string &what) {
	EntLiveSet(g_serverEnts), g_serverEntsAt = NowS();
	const bool dn = g_dynNear;
	g_dynNear = true;
	dyn = 0;  // (bridge #26) a bound mover's box (where it is now) holds the point
	static std::vector<std::pair<Vector, Vector>> mb;
	MoversBoxesNear(p, 64.0f, mb);
	for (const auto &bb : mb)
		if (p.x > bb.first.x - h && p.x < bb.second.x + h && p.y > bb.first.y - h && p.y < bb.second.y + h && p.z > bb.first.z - h && p.z < bb.second.z + h) dyn = 1;
	g_dynNear = dn;
	world = WorldSolidAt(p) ? 1 : 0;
	CGameTrace tr;
	engine.Trace(p, p, Vector(h, h, h), MASK_PLAYERSOLID, tr, false);
	all = tr.startsolid ? 1 : 0;
	what = tr.m_pEnt ? EntKV(tr.m_pEnt, "classname") + " " + EntKV(tr.m_pEnt, "targetname") : "-";
}



// ---- (bridge #33) the exported voxels as geometry for Portal 2's own movement and shots (dig.cpp, blockcollide.cpp, portalgun.cpp) --------
// Around a dug hole the engine's world still has the wall that was dug away, so Chell's hull and the portal gun's shots use what Minecraft
// has instead: the exported voxel field (the host's solid minus the dug cells). Boxes are Source AABBs of x-runs of solid sub-voxels.
static std::tuple<int, int, int> g_vbKey{1 << 30, 0, 0};
static std::tuple<int, int, int> g_vbMissing{0, 0, 0};  // (bridge #35) the last region CollisionFieldVoxelBoxes found not exported
void CollisionFieldVoxelMissing(int &rx, int &ry, int &rz) { rx = std::get<0>(g_vbMissing), ry = std::get<1>(g_vbMissing), rz = std::get<2>(g_vbMissing); }
static const std::vector<ColBlock> *g_vbVec = nullptr;
static void VoxCacheReset() { g_vbKey = {1 << 30, 0, 0}, g_vbVec = nullptr; }  // (g_lastSent changes between calls)
static const ColBlock *g_vbIdx[kRegion * kRegion * kRegion];  // (bridge #35) the cached region's blocks by position (the linear scan per sub-voxel cost ms per trace)
static const ColBlock *VoxelBlockAt(int bx, int by, int bz) {
	auto &lastKey = g_vbKey;
	auto &lastVec = g_vbVec;
	auto rf = [](int b) { return (int)std::floor(b / (double)kRegion); };
	const auto rk = std::make_tuple(rf(bx), rf(by), rf(bz));
	if (rk != lastKey || !lastVec) {
		auto it = g_lastSent.find(rk);
		lastKey = rk;
		lastVec = it == g_lastSent.end() ? nullptr : &it->second;
		std::fill(std::begin(g_vbIdx), std::end(g_vbIdx), nullptr);
		if (lastVec)
			for (const ColBlock &b : *lastVec) {
				const int ix = ((b.x % kRegion) + kRegion) % kRegion, iy = ((b.y % kRegion) + kRegion) % kRegion, iz = ((b.z % kRegion) + kRegion) % kRegion;
				g_vbIdx[(iy * kRegion + iz) * kRegion + ix] = &b;
			}
	}
	if (!lastVec) return nullptr;
	const int ix = ((bx % kRegion) + kRegion) % kRegion, iy = ((by % kRegion) + kRegion) % kRegion, iz = ((bz % kRegion) + kRegion) % kRegion;
	return g_vbIdx[(iy * kRegion + iz) * kRegion + ix];
}
// true if every region the box touches has been exported (else the caller keeps the engine's answer)
bool CollisionFieldVoxelBoxes(const Vector &lo, const Vector &hi, std::vector<std::pair<Vector, Vector>> &out, bool *complete) {
	VoxCacheReset();
	out.clear();
	const Vector a = SrcToMc(lo), b = SrcToMc(hi);
	const double mnx = std::fmin(a.x, b.x), mxx = std::fmax(a.x, b.x), mny = std::fmin(a.y, b.y), mxy = std::fmax(a.y, b.y), mnz = std::fmin(a.z, b.z), mxz = std::fmax(a.z, b.z);
	const int x0 = (int)std::floor(mnx * 8), x1 = (int)std::floor(mxx * 8), y0 = (int)std::floor(mny * 8), y1 = (int)std::floor(mxy * 8), z0 = (int)std::floor(mnz * 8), z1 = (int)std::floor(mxz * 8);
	if ((long)(x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > 60000) {
		if (complete) *complete = false;
		return false;
	}
	bool ok = true;
	auto fl8 = [](int v) { return (int)std::floor(v / 8.0); };
	// (bridge #36) headroom over a dug cell: the lowest sub-voxel row of a solid cell directly above a dug one is no collision (the mod's SkyCollision.shapeAt
	// does the same for the Minecraft player). The export's 5 u rows put a floor up to 2.5 u above its true height and the 40 u cells start 8 u under the
	// chamber floors, so a tunnel dug 2 cells high (80 u: exactly the hull's 72 above the true floor) left the standing hull 2 u short of its ceiling
	// and nothing could enter it. The row (5 u) is not drawn by anybody: the walls of the hole come from the dug cells' boxes.
	const bool anyDug = DigAny();
	// (bridge #45) Chell's hole volumes (the field is no longer carved by them): her hull next to a dug cell may enter a portal's opening
	Vector hMn[kPortalSlots], hMx[kPortalSlots];
	const int nHoles = ChellHoleBoxes(hMn, hMx, kPortalSlots);
	auto inChellHole = [&](int x, int y, int z) {
		const Vector c = McToSrc((x + 0.5) / 8.0, (y + 0.5) / 8.0, (z + 0.5) / 8.0);
		for (int k = 0; k < nHoles; ++k)
			if (c.x > hMn[k].x && c.x < hMx[k].x && c.y > hMn[k].y && c.y < hMx[k].y && c.z > hMn[k].z && c.z < hMx[k].z) return true;
		return false;
	};
	int hkx = 0, hky = 0, hkz = 0;
	bool hkv = false, hkHave = false;
	auto liftOver = [&](int bx, int by, int bz) {
		if (!hkHave || hkx != bx || hky != by || hkz != bz) hkHave = true, hkx = bx, hky = by, hkz = bz, hkv = DigIsDugMc(bx, by - 1, bz);
		return hkv;
	};
	for (int y = y0; y <= y1; ++y)
		for (int z = z0; z <= z1; ++z) {
			int runStart = 0;
			bool inRun = false;
			auto flush = [&](int endX) {
				if (!inRun) return;
				inRun = false;
				const Vector p = McToSrc(runStart / 8.0, y / 8.0, z / 8.0), q = McToSrc(endX / 8.0, (y + 1) / 8.0, (z + 1) / 8.0);
				out.push_back({Vector(std::fmin(p.x, q.x), std::fmin(p.y, q.y), std::fmin(p.z, q.z)), Vector(std::fmax(p.x, q.x), std::fmax(p.y, q.y), std::fmax(p.z, q.z))});
			};
			for (int x = x0; x <= x1; ++x) {
				const int bx = fl8(x), by = fl8(y), bz = fl8(z);
				const ColBlock *cb = VoxelBlockAt(bx, by, bz);
				if (!cb && !g_vbVec) ok = false, g_vbMissing = std::make_tuple(fl8(bx), fl8(by), fl8(bz));
				const int sx = x - bx * 8, sy = y - by * 8, sz = z - bz * 8;
				const bool solid = cb && (cb->bits[sy] >> (sz * 8 + sx) & 1) && !(anyDug && sy == 0 && liftOver(bx, by, bz)) && !(nHoles && inChellHole(x, y, z));
				if (solid && !inRun) inRun = true, runStart = x;
				if (!solid) flush(x);
			}
			flush(x1 + 1);
		}
	if (complete) *complete = ok;
	return true;
}
// First solid sub-voxel along a Source ray (3D DDA over the exported voxels); true with the distance and the entered face's normal.
bool CollisionFieldVoxelRay(const Vector &start, const Vector &dir, float maxDist, float &dist, Vector &normal) {
	VoxCacheReset();
	// MC voxel space: u = 8 per block; x east, y up, z south (Source y reversed)
	const Vector s = SrcToMc(start);
	const double o[3] = {s.x * 8, s.y * 8, s.z * 8};
	const double d[3] = {dir.x, dir.z, -dir.y};  // per Source unit; one voxel is 5 u
	const double k = 8.0 / kUnitsPerBlock;
	int v[3] = {(int)std::floor(o[0]), (int)std::floor(o[1]), (int)std::floor(o[2])};
	double tMax[3], tDelta[3];
	int step[3];
	for (int i = 0; i < 3; ++i) {
		const double di = d[i] * k;  // voxels per Source unit
		step[i] = di > 0 ? 1 : -1;
		if (std::fabs(di) < 1e-12) tMax[i] = tDelta[i] = 1e30;
		else {
			tDelta[i] = 1.0 / std::fabs(di);
			const double next = di > 0 ? v[i] + 1 - o[i] : o[i] - v[i];
			tMax[i] = next * tDelta[i];
		}
	}
	auto fl8 = [](int q) { return (int)std::floor(q / 8.0); };
	auto solidAt = [&](int x, int y, int z) {
		const ColBlock *cb = VoxelBlockAt(fl8(x), fl8(y), fl8(z));
		return cb && (cb->bits[y - fl8(y) * 8] >> ((z - fl8(z) * 8) * 8 + (x - fl8(x) * 8)) & 1);
	};
	double t = 0;
	int axis = -1;
	for (int n = 0; n < 6000 && t <= maxDist; ++n) {
		if (solidAt(v[0], v[1], v[2])) {
			if (axis < 0) return false;  // starts inside
			dist = (float)t;
			const double sg = -step[axis];
			double nm[3] = {0, 0, 0};
			nm[axis] = sg;
			normal = Vector((float)nm[0], (float)-nm[2], (float)nm[1]);  // MC (x, y, z) -> Source (x, -z, y)
			return true;
		}
		axis = tMax[0] < tMax[1] ? (tMax[0] < tMax[2] ? 0 : 2) : (tMax[1] < tMax[2] ? 1 : 2);
		t = tMax[axis];
		tMax[axis] += tDelta[axis];
		v[axis] += step[axis];
	}
	return false;
}

// ---- (bridge #33) harvest commands -------------------------------------------------------------------------------------
CON_COMMAND(pc_colfield_harvest, "pc_colfield_harvest [0|1] - voxelise the chamber collision from the level file on worker threads instead of point queries (default 1; falls back while loading)\n") {
	if (args.ArgC() > 1) {
		const int v = std::atoi(args[1]);
		if (v && !g_hMode) {
			g_hMode = 1;
			HarvestBeginLoad(McMapName());
		} else if (!v && g_hMode) {
			g_hMode = 0;
			g_sent.clear(), WorkReset();  // the point-query path takes over again
			HarvestLevelReset();
		}
	}
	const char *st[] = {"not loaded", "loading", "ready", "FAILED"};
	console->Print("[bridge] harvest export %s: %s (%s); %d regions written (%.1f ms/region on the workers), %zu in flight, %zu waiting for the ring, %d stale results dropped; playerclip %s, static props %s, leaf contents %s\n",
	               g_hMode ? "on" : "off", g_hMap.c_str(), st[g_hState], g_hRegions, g_hRegions ? g_hMsTotal / g_hRegions : 0.0, g_hInflight.size(), g_hPending.size(), g_hStale,
	               g_hClip ? "all solid" : "floors over nothing only", g_hProps ? "on" : "off", g_hLeaf ? "on" : "off");
}
CON_COMMAND(pc_colfield_harvest_opt, "pc_colfield_harvest_opt <clip 0|1> <props 0|1> <leaf 0|1> - harvest voxelising options (default 1 1 1: the point-query export's behaviour); resends everything\n") {
	if (args.ArgC() > 3) {
		g_hClip = std::atoi(args[1]) != 0, g_hProps = std::atoi(args[2]) != 0, g_hLeaf = std::atoi(args[3]) != 0;
		++g_hOptGen;
		g_sent.clear();
		g_hUrgent.clear();
	}
	console->Print("[bridge] harvest options: playerclip %s, static props %s, leaf contents %s\n", g_hClip ? "all solid" : "floors over nothing only", g_hProps ? "on" : "off", g_hLeaf ? "on" : "off");
}
// pcb_colfield_compare <x y z> [radius blocks]: the live check of the offline equivalence. Every block in a cube around the Portal 2 point is
// voxelised both ways now (the point queries on this thread, the harvest in place) and the sub-voxels compared.
CON_COMMAND(pcb_colfield_compare, "pcb_colfield_compare <x y z> [radius_blocks=6] - voxelise the blocks around a Portal 2 point with the point queries and with the harvest and count the differing sub-voxels\n") {
	if (args.ArgC() < 4) return;
	if (!g_hWorld) return console->Print("[bridge] harvest: no geometry loaded (pc_colfield_harvest 1, wait for 'ready')\n");
	const Vector c((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3]));
	const int R = args.ArgC() > 4 ? std::atoi(args[4]) : 6;
	const Vector mc = SrcToMc(c);
	const int cx = (int)std::floor(mc.x), cy = (int)std::floor(mc.y), cz = (int)std::floor(mc.z);
	colharvest::Mapping m;
	m.mcX = g_map.mcX, m.mcY = g_map.mcY, m.mcZ = g_map.mcZ, m.sx = g_map.src.x, m.sy = g_map.src.y, m.sz = g_map.src.z, m.upb = kUnitsPerBlock;
	colharvest::Options o = HarvestOptions();
	std::map<std::tuple<int, int, int>, std::vector<ColBlock>> regs;
	long long oldV = 0, newV = 0, both = 0;
	int blocksOld = 0, blocksNew = 0, diffBlocks = 0, shown = 0;
	auto rf = [](int b) { return (int)std::floor(b / (double)kRegion); };
	for (int by = cy - R; by <= cy + R; ++by)
		for (int bz = cz - R; bz <= cz + R; ++bz)
			for (int bx = cx - R; bx <= cx + R; ++bx) {
				const auto rk = std::make_tuple(rf(bx), rf(by), rf(bz));
				auto it = regs.find(rk);
				if (it == regs.end()) {
					std::vector<ColBlock> v;
					g_hWorld->VoxeliseRegion(m, o, std::get<0>(rk), std::get<1>(rk), std::get<2>(rk), v);
					it = regs.emplace(rk, std::move(v)).first;
				}
				ColBlock a{}, b{};
				const bool ha = CollisionFieldVoxeliseNow(bx, by, bz, a);
				bool hb = false;
				for (const ColBlock &x : it->second)
					if (x.x == bx && x.y == by && x.z == bz) b = x, hb = true;
				blocksOld += ha, blocksNew += hb;
				int diff = 0;
				for (int y = 0; y < 8; ++y) {
					const uint64_t oa = ha ? a.bits[y] : 0, ob = hb ? b.bits[y] : 0;
					oldV += __builtin_popcountll(oa), newV += __builtin_popcountll(ob), both += __builtin_popcountll(oa & ob);
					diff += __builtin_popcountll(oa ^ ob);
				}
				if (diff) {
					++diffBlocks;
					if (shown++ < 6) console->Print("[bridge]   block MC %d %d %d differs in %d sub-voxels (point queries %s, harvest %s)\n", bx, by, bz, diff, ha ? "solid" : "empty", hb ? "solid" : "empty");
				}
			}
	const int side = 2 * R + 1;
	console->Print("[bridge] harvest compare %d^3 blocks around MC %d %d %d: point queries %lld sub-voxels in %d blocks, harvest %lld in %d blocks, both %lld; %d blocks differ (agreement %.3f%%)\n", side, cx, cy, cz, oldV,
	               blocksOld, newV, blocksNew, both, diffBlocks, oldV + newV - both ? 100.0 * both / (double)(oldV + newV - both) : 100.0);
}

}  // namespace bridge
