// Portalcraft bridge: the pit-flings clip (Chell, Portal 2's own gun, a portal into Minecraft).
//
//   * Region: maps with info_target "pc_mc_region" (+ two "pc_mc_region_bounds") get the
//     Minecraft terrain mapped into that hidden box, anchored at the chosen cliff.
//   * Redirect: whenever an active portal lies on a floor inside a redirect zone (info_target
//     "pc_redirect_zone", or pcb_redirect_zone; with none defined, any floor portal), its partner
//     is moved (the prop_portal's own NewLocation input, no cheats) onto the Minecraft cliff face,
//     fitted like a Portal 2 wall portal. Portal 2's native transit then carries Chell through.
//   * pc_reset: portals fizzled, Chell back at the chamber start, terrain kept.
#include "bridge.hpp"
#include "growth.hpp"
#include "coords.hpp"
#include "link.hpp"
#include "stage.hpp"
#include "worldconf.hpp"

#include "Modules/Tier1.hpp"

#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace bridge {

void TerrainSetRegion(double mcX, double mcY, double mcZ, const Vector &src);  // mc.cpp
bool TerrainIsSolid(int bx, int by, int bz);
void SetQuiet(bool quiet);

// The clip's cliff: bridge/world.conf (bridge #81: the seed, the cliff top, its facing and the pinned exit patch live there and nowhere else). The
// original cliff (seed 20260930) was top -328 114 424 facing west (-X), chosen from CliffFinder's real-block check (the mod's -Dskycraft.findCliff
// search, CLIFFEVAL lines in mc.log): a vertical face tall enough for the patch, open air in front and nothing above it, flat ground in front.
// (set from the mod's report by ClipWorldSync; 0 until Minecraft has opened a world)
static double g_cliffX = 0, g_cliffY = 0, g_cliffZ = 0;
static int g_cliffDx = -1, g_cliffDz = 0;

static bool g_regionMap = false;
// (bridge #61) The box of the last real region map (pit flings, the testbed) of this session, which the terrain mapping (mc.cpp) belongs to. It outlives the
// level: Minecraft's terrain there is still streamed and held while a campaign map is played, and the question "is this section / emitter in the
// terrain (not in a chamber)?" must keep the same answer on a map without a region box (ClipInTerrainBox). Never used as a region by itself.
static bool g_tbSet = false;
static Vector g_tbMin, g_tbMax, g_tbAnchor;  // (the mapping's anchor of that map: the floating origin may have moved g_map since)
static bool g_stageMode = false;  // (bridge #47) the region is the virtual one (stage.cpp): the exit is a cliff face of the Minecraft stage
static Vector g_regionMin, g_regionMax;
struct Zone {
	Vector centre;
	float radius;
};
static std::vector<Zone> g_zones;
PCB_GAUGE("clip.zones", g_zones.size());
static std::vector<void *> g_portals;
// (bridge #68) linked_portal_door (sp_a1_wakeup's incinerator, sp_a4_finale2's portal chambers): a model door that makes a hidden portal_base2D (Portal 2's
// own portal class, the one prop_portal is built on) at its pose. Portal 2 pairs and transits through those; the bridge reads them as one more kind of
// portal (ClipDoorPortals below).
int EntFieldOffset(void *ent, const char *field);  // ents.cpp
static std::vector<void *> g_doorPortals, g_doors;
static std::unordered_set<void *> g_knownEnts;  // (bridge #42) every entity pointer classified so far this level
PCB_GAUGE("clip.knownEnts", g_knownEnts.size());
static int g_sinceFullScan = 0;
// (bridge #32) LinkageGroupID -> portal slots (bridge.hpp kPortalSlots): group 0 is always slots 0/1, the others take the next pair of slots
// the first time they are seen (a map's pairs: pc_testbed's tb_pair_* are group 1, tb_fling_* group 2). -1: more groups than slots.
static std::vector<int> g_groupIds;  // index = group index (0 = LinkageGroupID 0)
static int g_slotSeen[kPortalSlots] = {};
static std::unordered_map<void *, int> g_doorPairId;    // (bridge #68) a linked pair of door portals -> its number (keyed by the lower pointer)
static std::unordered_map<void *, int> g_doorSlot;      // a door portal -> the slot it last filled
static std::unordered_map<void *, bool> g_doorOn;       // a door portal -> was open and linked when last logged
int PortalSlotOf(int group, bool two) {
	if (g_groupIds.empty()) g_groupIds.push_back(0);
	size_t gi = 0;
	while (gi < g_groupIds.size() && g_groupIds[gi] != group) ++gi;
	if (gi == g_groupIds.size()) {
		if ((int)gi * 2 + 1 >= kPortalSlots) return -1;
		g_groupIds.push_back(group);
	}
	return (int)gi * 2 + (two ? 1 : 0);
}
static bool SameGroup(void *a, void *b) { return std::atoi(EntKV(a, "LinkageGroupID").c_str()) == std::atoi(EntKV(b, "LinkageGroupID").c_str()); }

static int g_pollTick = 0;
static bool g_haveExit = false;
static Vector g_exitPos;
static QAngle g_exitAng;
static Vector g_startPos;
static QAngle g_startAng;
static int g_redirects = 0;

static bool OnFloor(const QAngle &a) { return a.x < -80.0f; }  // portal normal points up

static bool InZone(const Vector &p) {
	if (g_zones.empty()) return true;
	for (auto &z : g_zones)
		if (std::hypot(p.x - z.centre.x, p.y - z.centre.y) <= z.radius && std::fabs(p.z - z.centre.z) < 128) return true;
	return false;
}

// ---- the exit patch ------------------------------------------------------------------------------
// A 2 x 3 block patch of the cliff face for the wall portal (the 1.6 x 2.8 block opening fits inside
// with Portal 2's 2 u margin). It is described by its bottom wall block at along-face index 0 (wx, wy,
// wz), the facing direction (dx, dz) and the along-face axis (rx, rz) = (dz, -dx).
//
// The exit is pinned to the patch in world.conf (EXIT_WX/WY/WZ + the cliff's facing; the original, validated in the clip runs (val1e): wall blocks
// x -331, y 94..96, z 424..425, facing -X, so the portal sat at MC (-331.02 95.5 425)). The finder below ran on fresh
// Minecraft starts while the cliff was still streaming in and picked patches inside the rock (the
// user came out inside a block), so it is only a fallback for other cliffs (pcb_cliff), runs only
// once every section it scans has streamed, and its result must pass the same check as the pin.
struct Patch {
	int wx, wy, wz, dx, dz;
};
static Patch PinnedPatch() {
	const WorldConf &w = World();
	return Patch{w.exitWx, w.exitWy, w.exitWz, w.cliffDx, w.cliffDz};
}
static bool g_usePin = true;
static Patch g_exitPatch{};
enum PatchState { kPatchOk, kPatchNotStreamed, kPatchBad };
bool TerrainSectionKnown(int bx, int by, int bz);  // terrain.cpp

// Every wall cell solid, and the two cells in front of each one open air (so the whole opening has
// at least 2 blocks of air in front of it), all in sections that have streamed.
static PatchState CheckPatch(const Patch &p, char *why, size_t whyLen) {
	const int rx = p.dz, rz = -p.dx;
	for (int j = 0; j < 3; ++j)
		for (int i = 0; i < 2; ++i) {
			int x = p.wx + rx * i, y = p.wy + j, z = p.wz + rz * i;
			for (int f = 0; f <= 2; ++f)
				if (!TerrainSectionKnown(x + p.dx * f, y, z + p.dz * f)) {
					if (why) std::snprintf(why, whyLen, "section of %d %d %d not streamed", x + p.dx * f, y, z + p.dz * f);
					return kPatchNotStreamed;
				}
			if (!TerrainIsSolid(x, y, z)) {
				if (why) std::snprintf(why, whyLen, "no wall block at %d %d %d", x, y, z);
				return kPatchBad;
			}
			for (int f = 1; f <= 2; ++f)
				if (TerrainIsSolid(x + p.dx * f, y, z + p.dz * f)) {
					if (why) std::snprintf(why, whyLen, "block %d %d %d in front of the opening", x + p.dx * f, y, z + p.dz * f);
					return kPatchBad;
				}
		}
	if (why) std::snprintf(why, whyLen, "solid wall behind, 2 blocks of air in front");
	return kPatchOk;
}

// Portal centre on the patch's front face (+0.02 blocks out), Minecraft coordinates.
static void PatchCentre(const Patch &p, double &mx, double &my, double &mz) {
	const int rx = p.dz, rz = -p.dx;
	// (bridge #49) the stage's cliff patch starts at the ground: the opening sits flush on it (its bottom edge on the floor, as Portal 2 fits a portal), or the walker's
	// feet would be 4 u under the opening's bottom edge, in the wall (the hole is carved 1 u under it only). The pit's pinned patch keeps its 0.1 block margin.
	my = p.wy + (g_stageMode || StageUnified() ? 1.4 : 1.5);  // (bridge #78: the unified window's pinned patch too: a walker on the ground in front must reach the opening)
	mx = rx ? p.wx + (rx > 0 ? 1.0 : 0.0) : p.wx + (p.dx < 0 ? 0.0 : 1.0) + p.dx * 0.02;
	mz = rz ? p.wz + (rz > 0 ? 1.0 : 0.0) : p.wz + (p.dz < 0 ? 0.0 : 1.0) + p.dz * 0.02;
}

static void SetExit(const Patch &p, const char *how, const char *why) {
	double mx, my, mz;
	PatchCentre(p, mx, my, mz);
	g_exitPatch = p;
	g_exitPos = McToSrc(mx, my, mz);
	float mcYaw = (float)(std::atan2(-p.dx, p.dz) * 180.0 / M_PI);
	g_exitAng = QAngle{0.0f, McYawToSrc(mcYaw), 0.0f};
	g_haveExit = true;
	console->Print("[bridge] clip: cliff exit (%s) at MC (%.2f %.2f %.2f) facing (%d %d) = Portal 2 (%.1f %.1f %.1f) yaw %.1f: %s\n", how, mx, my, mz, p.dx, p.dz, g_exitPos.x,
	               g_exitPos.y, g_exitPos.z, g_exitAng.y, why);
}

// Fallback finder (other cliffs): flat patches nearest a third of the way down the face, accepted
// only once every section it scans has streamed, and only if the patch passes CheckPatch.
static PatchState FindPatch(Patch &out) {
	const int bx = (int)std::floor(g_cliffX), bz = (int)std::floor(g_cliffZ), top = (int)std::floor(g_cliffY);
	const int dx = g_cliffDx, dz = g_cliffDz;
	const int rx = dz, rz = -dx;
	for (int y = top - 50; y <= top - 6 + 15; y += 8)
		for (int r = -14; r <= 15; r += 8)
			for (int k = -20; k <= 31; k += 8)
				if (!TerrainSectionKnown(bx + rx * r + dx * k, y, bz + rz * r + dz * k)) return kPatchNotStreamed;
	auto depth = [&](int r, int y) -> int {
		int last = -1000;
		for (int k = -20; k < 28; ++k) {
			int x = bx + rx * r + dx * k, z = bz + rz * r + dz * k;
			if (TerrainIsSolid(x, y, z)) last = k;
			else if (last > -1000) {
				bool open = true;
				for (int f = 1; f <= 3; ++f) open &= !TerrainIsSolid(x + dx * f, y, z + dz * f);
				if (open) return last;
			}
		}
		return -1000;
	};
	struct Cand {
		int score;
		Patch p;
	};
	std::vector<Cand> cands;
	for (int y = top - 50; y <= top - 6; ++y)
		for (int r = -14; r <= 14; ++r) {
			int k = depth(r, y);
			if (k == -1000) continue;
			bool flat = true;
			for (int j = 0; j < 3 && flat; ++j)
				for (int i = 0; i < 2 && flat; ++i) flat = depth(r + i, y + j) == k;
			if (!flat) continue;
			bool clear = true;  // no overhang: open air in front up to 12 blocks above the patch
			for (int up = 3; up <= 12 && clear; ++up)
				for (int i = 0; i < 2 && clear; ++i)
					for (int f = 1; f <= 4 && clear; ++f)
						clear = !TerrainIsSolid(bx + rx * (r + i) + dx * (k + f), y + up, bz + rz * (r + i) + dz * (k + f));
			if (!clear) continue;
			cands.push_back({std::abs(y - (top - 20)) * 2 + std::abs(r) * 3, Patch{bx + rx * r + dx * k, y, bz + rz * r + dz * k, dx, dz}});
		}
	std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.score < b.score; });
	for (auto &c : cands)
		if (CheckPatch(c.p, nullptr, 0) == kPatchOk) {
			out = c.p;
			return kPatchOk;
		}
	return kPatchBad;
}

// The stage's exit (bridge #47): the cliff wall of the Minecraft stage nearest the window's centre. The stage (StageTerrain.java) has walls at
// x = k * 256 (+ 0..3 per 24-block run of z) facing -X: stone behind, open air in front, the lowland at their foot. The wall's face and the
// ground in front of it are read from the streamed blocks (the same check as the pinned cliff's: solid wall behind, 2 blocks of air in front,
// every cell of the 2 x 3 patch in a section that has streamed), so a mismatch with the mod's landscape is a refusal here, never a bad exit.
int TerrainFluid(int bx, int by, int bz);  // terrain.cpp
static PatchState FindStagePatch(Patch &out, char *why, size_t whyLen) {
	int wallX = 0, zc = 0;
	if (!StageWindow(wallX, zc)) {
		std::snprintf(why, whyLen, "the stage is not on");
		return kPatchBad;
	}
	for (int x = wallX - 16; x <= wallX + 16; x += 16)
		for (int z = zc - 16; z <= zc + 17; z += 16)
			for (int y = kStageLow - 16; y <= kStageLow + 16; y += 16)
				if (!TerrainSectionKnown(x, y, z)) {
					std::snprintf(why, whyLen, "stage section of %d %d %d not streamed", x, y, z);
					return kPatchNotStreamed;
				}
	const int row = kStageLow + 10;
	for (int k = 0; k < 41; ++k) {
		const int z0 = zc + ((k & 1) ? -(k + 1) : k);  // 0, -2, 2, -4, 4 ... (the patch is 2 wide: z0 and z0 + 1)
		const int a = (int)std::floor(z0 / (double)kStageRun), b = (int)std::floor((z0 + 1) / (double)kStageRun);
		if (a != b) continue;  // a face is flat inside a run only
		int face[2] = {0, 0}, ground[2] = {-1, -1};
		for (int c = 0; c < 2; ++c) {
			for (int x = wallX - 6; x <= wallX + 10; ++x)
				if (TerrainIsSolid(x, row, z0 + c) && !TerrainIsSolid(x - 1, row, z0 + c)) {
					face[c] = x;
					break;
				}
			if (!face[c]) break;
			for (int y = row; y > kStageLow - 12; --y)
				if (TerrainIsSolid(face[c] - 1, y, z0 + c)) {
					ground[c] = y;
					break;
				}
		}
		if (!face[0] || face[0] != face[1] || ground[0] < 0 || ground[0] != ground[1]) continue;
		Patch p{face[0], ground[0] + 1, z0, -1, 0};
		if (TerrainFluid(p.wx - 1, p.wy, z0) || TerrainFluid(p.wx - 1, p.wy, z0 + 1) || TerrainFluid(p.wx - 2, p.wy, z0) || TerrainFluid(p.wx - 2, p.wy, z0 + 1)) continue;  // not into a lake
		if (CheckPatch(p, why, whyLen) != kPatchOk) continue;
		out = p;
		return kPatchOk;
	}
	std::snprintf(why, whyLen, "no flat, dry cliff patch within 40 blocks of the window's z run %d (wall at x %d)", zc, wallX);
	return kPatchBad;
}

static int g_exitLogState = -1;
uint32_t TerrainSolidsGen();     // terrain.cpp
size_t TerrainSectionsKnown();   // terrain.cpp
// trigger: what asked for the check (logged); forceLog: log the "not yet" result even if unchanged.
static bool ComputeExit(const char *trigger = "query", bool forceLog = false) {
	if (g_usePin && !g_stageMode && !World().known) return false;  // (bridge #81) Minecraft has not reported its world's exit spot yet
	char why[160];
	PatchState st;
	if (g_stageMode) {
		Patch p{};
		st = FindStagePatch(p, why, sizeof why);
		if (st == kPatchOk) {
			SetExit(p, "stage", why);
			console->Print("[bridge] clip: exit check (%s): ok, %zu sections held\n", trigger, TerrainSectionsKnown());
			g_exitLogState = -1;
			return true;
		}
	} else if (g_usePin) {
		st = CheckPatch(PinnedPatch(), why, sizeof why);
		if (st == kPatchOk) {
			char how[96];
			std::snprintf(how, sizeof how, "pinned");
			SetExit(PinnedPatch(), how, why);
			console->Print("[bridge] clip: exit check (%s): ok, %zu sections held\n", trigger, TerrainSectionsKnown());
			g_exitLogState = -1;
			return true;
		}
	} else {
		Patch p{};
		st = FindPatch(p);
		if (st == kPatchOk) {
			CheckPatch(p, why, sizeof why);
			SetExit(p, "found", why);
			console->Print("[bridge] clip: exit check (%s): ok, %zu sections held\n", trigger, TerrainSectionsKnown());
			g_exitLogState = -1;
			return true;
		}
		std::snprintf(why, sizeof why, st == kPatchNotStreamed ? "cliff sections still streaming" : "no patch with a solid wall and 2 blocks of air");
	}
	if (forceLog || g_exitLogState != (int)st) {
		g_exitLogState = (int)st;
		console->Print("[bridge] clip: no cliff exit yet (%s exit: %s; check: %s, %zu sections held, link %s), the partner portal is not moved\n", g_stageMode ? "stage" : g_usePin ? "pinned" : "searched",
		               why, trigger, TerrainSectionsKnown(), link::McAlive() ? "up" : "down");
	}
	return false;
}

// Re-evaluation of the exit. Start-up order must not matter: the map may load before Minecraft
// links, Minecraft may restart mid-session, a level may reload. Each of those events (and every
// batch of sections that arrives while the exit is unresolved) re-checks the patch and logs it.
static std::string g_recheckWhy;  // the triggers since the last check, joined
static uint32_t g_exitGenChecked = ~0u;
static double g_exitLastTry = 0, g_exitLastPendingLog = 0;
static double ClipNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void ClipExitRecheck(const char *why) {
	if (g_recheckWhy.find(why) == std::string::npos) g_recheckWhy += g_recheckWhy.empty() ? why : std::string(" + ") + why;
}

void UnifiedFrame();  // unified.cpp (bridge #78)
void ClipFrame() {
	UnifiedFrame();
	if (!g_regionMap) return;
	const double now = ClipNow();
	if (!g_recheckWhy.empty()) {
		const std::string why = g_recheckWhy;
		g_recheckWhy.clear();
		g_haveExit = false;
		g_exitGenChecked = TerrainSolidsGen();
		g_exitLastTry = g_exitLastPendingLog = now;
		ComputeExit(why.c_str(), true);
		return;
	}
	if (g_haveExit) return;
	const uint32_t gen = TerrainSolidsGen();
	if (gen != g_exitGenChecked && now - g_exitLastTry >= 0.1) {
		g_exitGenChecked = gen, g_exitLastTry = now;
		if (ComputeExit("sections arrived")) return;
	}
	if (now - g_exitLastPendingLog >= 5.0) {
		g_exitLastPendingLog = now;
		ComputeExit("still waiting", true);
	}
}

static void Redirect(void *partner, void *floorPortal) {
	if (!g_haveExit && !ComputeExit()) return;
	// never send Chell into rock: the patch is checked again before every move
	// (a section that has unloaded since keeps the validated result; a changed one does not)
	if (CheckPatch(g_exitPatch, nullptr, 0) == kPatchBad) {
		console->Print("[bridge] clip: the cliff exit patch changed (no longer a wall with air in front), redirect held\n");
		g_haveExit = false;
		return;
	}
	Vector cur = EntVec(partner, "origin");
	if ((cur - g_exitPos).Length() < 1.0f) {
		if (EntKV(partner, "Activated") == "1") return;
		// (2026-10-12) already at the exit but closed (Portal 2 closes it as Chell goes through): open it where it is. The full move below
		// replayed the portal's opening animation and sound on every crossing (the user's report; watch: prop_portal lines say who closed it).
		EntInput(partner, "SetActivatedState", "1");
		static int n = 0;
		if (++n <= 5 || n % 100 == 0) console->Print("[bridge] clip: partner portal %p re-opened in place at the exit (#%d)\n", partner, n);
		return;
	}
	char buf[160];
	std::snprintf(buf, sizeof buf, "%.2f %.2f %.2f %.1f %.1f %.1f", g_exitPos.x, g_exitPos.y, g_exitPos.z, g_exitAng.x, g_exitAng.y, g_exitAng.z);
	bool a = EntInput(partner, "SetActivatedState", "1");
	bool b = EntInput(partner, "NewLocation", buf);
	// re-run the floor portal's own linkage now that its partner is in place
	EntInput(floorPortal, "SetActivatedState", "1");
	++g_redirects;
	if (g_redirects <= 5 || g_redirects % 100 == 0)
		console->Print("[bridge] clip: partner portal %p -> cliff (%s), inputs %d/%d, now at (%s) active %s, redirect #%d\n", partner, buf, a, b, EntKV(partner, "origin").c_str(),
		               EntKV(partner, "Activated").c_str(), g_redirects);
}

static void McPortalLevelReset();
void ClipLevelLive() {
	g_regionMap = false;
	g_stageMode = false;
	StageLevelReset();  // (bridge #47) the virtual region is made again when it is needed
	g_zones.clear();
	g_portals.clear(), g_doorPortals.clear(), g_doors.clear();
	g_knownEnts.clear(), g_sinceFullScan = 0;
	g_groupIds.clear();
	g_doorPairId.clear(), g_doorSlot.clear(), g_doorOn.clear();
	g_haveExit = false;
	McPortalLevelReset();  // entities are new; an armed press stays armed
	auto region = EntFind("info_target", "pc_mc_region");
	if (!region.empty()) {
		Vector c = EntVec(region[0], "origin");
		auto bounds = EntFind("info_target", "pc_mc_region_bounds");
		g_regionMin = c - Vector(8000, 8000, 4000), g_regionMax = c + Vector(8000, 8000, 4000);
		if (bounds.size() >= 2) {
			Vector a = EntVec(bounds[0], "origin"), b = EntVec(bounds[1], "origin");
			g_regionMin = Vector(std::fmin(a.x, b.x), std::fmin(a.y, b.y), std::fmin(a.z, b.z));
			g_regionMax = Vector(std::fmax(a.x, b.x), std::fmax(a.y, b.y), std::fmax(a.z, b.z));
		}
		// Minecraft y 40 sits on the region floor; the cliff top at the region's x/y centre.
		Vector anchor(c.x, c.y, g_regionMin.z + (float)((g_cliffY - 40.0) * kUnitsPerBlock));
		TerrainSetRegion(g_cliffX, g_cliffY, g_cliffZ, anchor);
		g_regionMap = true;
		g_tbSet = true, g_tbMin = g_regionMin, g_tbMax = g_regionMax, g_tbAnchor = anchor;
		SetQuiet(true);
		console->Print("[bridge] clip: region (%.0f %.0f %.0f)..(%.0f %.0f %.0f), cliff MC (%.0f %.0f %.0f) at (%.0f %.0f %.0f)\n", g_regionMin.x, g_regionMin.y, g_regionMin.z,
		               g_regionMax.x, g_regionMax.y, g_regionMax.z, g_cliffX, g_cliffY, g_cliffZ, anchor.x, anchor.y, anchor.z);
	}
	{
		void UnifiedLevelLive(bool regionMap);  // unified.cpp (bridge #78): a campaign chamber's place in the unified world
		UnifiedLevelLive(g_regionMap);
	}
	for (void *z : EntFind("info_target", "pc_redirect_zone")) g_zones.push_back({EntVec(z, "origin"), 256});
	auto starts = EntFind("info_player_start");
	if (!starts.empty()) g_startPos = EntVec(starts[0], "origin"), g_startAng = [&] { Vector a = EntVec(starts[0], "angles"); return QAngle{a.x, a.y, a.z}; }();
	if (g_regionMap) ClipExitRecheck("level load");
}

// (bridge #61) LevelInit: the old level's region state goes at once, not on the first simulated frame (the loading frames of a campaign map after pit flings
// still saw its box, its exit and the stage), and the entity-keyed state with it. A region map finds its own box again in ClipLevelLive.
void ClipLevelInit() {
	{
		void UnifiedLevelInit();  // unified.cpp (bridge #78)
		UnifiedLevelInit();
	}
	g_regionMap = false;
	g_stageMode = false;
	g_haveExit = false;
	g_recheckWhy.clear();
	StageLevelReset();
	g_zones.clear();
	g_portals.clear(), g_doorPortals.clear(), g_doors.clear();
	g_knownEnts.clear(), g_sinceFullScan = 0;
	g_groupIds.clear();
	g_doorPairId.clear(), g_doorSlot.clear(), g_doorOn.clear();
	McPortalLevelReset();
}
bool WorldUnifiedChamber();  // unified.cpp (bridge #78)
bool ClipInTerrainBox(const Vector &p) {
	// (bridge #78) a chamber in the unified world's band: the terrain is what is in the window (when it is on); with it off, the terrain under the band maps
	// below Portal 2's world (Minecraft y < 320 is Source z < -17000), as does any terrain the window does not show (the slab reaches the world's sides)
	if (WorldUnifiedChamber()) return ClipRegionContains(p) || p.z < -16384.0f || std::fabs(p.x) > 16384.0f || std::fabs(p.y) > 16384.0f;
	if (!g_tbSet) return ClipRegionContains(p);
	return p.x > g_tbMin.x && p.x < g_tbMax.x && p.y > g_tbMin.y && p.y < g_tbMax.y && p.z > g_tbMin.z && p.z < g_tbMax.z;
}
bool ClipTerrainBox(Vector &mn, Vector &mx) {
	if (!g_tbSet) return false;
	mn = g_tbMin, mx = g_tbMax;
	return true;
}
// Minecraft's terrain around the pinned cliff has streamed (the exit can be validated): a terrain session, not a void mirror world.
bool ClipTerrainCliffStreamed() { return TerrainSectionKnown(PinnedPatch().wx + PinnedPatch().dx, PinnedPatch().wy, PinnedPatch().wz + PinnedPatch().dz); }
// (bridge #61) A campaign map in a TERRAIN session (stage.cpp): the terrain around the cliff is already mapped into Portal 2's world at the remembered box; the
// part of the box clear of this map is the level's region, with the pinned cliff as its exit. (The mirror world's stage is ClipStageBegin.)
void ClipTerrainWindowBegin(const Vector &mn, const Vector &mx) {
	// the floating origin may have left the mapping shifted when the region map was left: back to the region map's own anchor (a no-op when it is there)
	const Vector a = g_tbAnchor;
	if (g_map.mcX != g_cliffX || g_map.mcY != g_cliffY || g_map.mcZ != g_cliffZ || g_map.src.x != a.x || g_map.src.y != a.y || g_map.src.z != a.z) TerrainSetRegion(g_cliffX, g_cliffY, g_cliffZ, a);
	g_regionMin = mn, g_regionMax = mx;
	g_regionMap = true;
	g_stageMode = false;
	g_haveExit = false;
	g_exitLogState = -1;
	SetQuiet(true);
	ClipExitRecheck("terrain window on");
}

// (bridge #78) The unified world's window is on (stage.cpp UnifiedWindowEnsure): the level is a region map with the pinned cliff as its exit, as pit
// flings is; the chamber's own mapping (the band) is not touched.
void ClipUnifiedWindowBegin(const Vector &mn, const Vector &mx) {
	g_regionMin = mn, g_regionMax = mx;
	g_regionMap = true;
	g_stageMode = false;
	g_haveExit = false;
	g_exitLogState = -1;
	SetQuiet(true);
	ClipExitRecheck("unified window on");
}
// (bridge #78) The pinned cliff: its top (the region maps' anchor) and the exit patch's bottom wall block and facing.
void ClipCliffMc(double &x, double &y, double &z, int &faceX, int &faceY, int &faceZ, int &dx, int &dz) {
	x = g_cliffX, y = g_cliffY, z = g_cliffZ;
	faceX = PinnedPatch().wx, faceY = PinnedPatch().wy, faceZ = PinnedPatch().wz, dx = PinnedPatch().dx, dz = PinnedPatch().dz;
}

// (bridge #47) The virtual region is on (stage.cpp): from here the level is a region map as pit flings is: the box, the cliff exit (a face of the
// Minecraft stage, searched in the streamed blocks), the partner redirect, the way home, the composite. Called once per level.
void ClipStageBegin(const Vector &mn, const Vector &mx, int wallX, int zc) {
	(void)wallX, (void)zc;
	g_regionMin = mn, g_regionMax = mx;
	g_regionMap = true;
	g_stageMode = true;
	g_haveExit = false;
	g_exitLogState = -1;
	SetQuiet(true);
	ClipExitRecheck("stage on");
}

// ---- pc_mc_portal: send the next shot's partner into Minecraft, from anywhere ---------------------
// Armed by `pc_mc_portal` (or its key, G by default: polled through SDL like J, so no bind is written to
// the user's config). The next portal Chell fires (either colour, any portalable surface, any chamber)
// keeps Portal 2's own placement; once it has landed it becomes the anchor and its partner is held at the
// validated cliff exit (the same Redirect, with the same patch check) instead of linking to her other
// portal. One shot disarms; pressing again before firing cancels. The anchor is released when the anchor
// portal moves or closes, or when Chell fires the partner's colour herself (a normal pair again).
// pc_pit_redirect_mode: 0 off, 1 G only (default: a pit floor portal goes to Minecraft only when fired with G held or
// armed, through the anchor rule below, like a shot anywhere else), 2 always (the old pit-floor rule; test suites).
// The user: "the floor in that level still no matter what spawns the minecraft portal, i only really want it to if
// im holding g" (bridge #12).
static int g_pitMode = 1;
static bool g_mcArmed = false;
static int g_armShot = -1;          // colour fired while armed, waiting for it to land
static int g_armShotTick = 0;
static Vector g_armShotPos;         // where Portal 2's placement put it
static void *g_anchor = nullptr;    // the portal whose partner is held at the exit
static int g_anchorColour = -1;
static Vector g_anchorPos;
static int g_armKey = 10;           // SDL scancode (10 = G); 0 = off
static bool g_armSound = false;
static int ColourOf(void *p) { return EntKV(p, "PortalTwo") == "1" ? 1 : 0; }
static void *PortalOfColour(int c) {
	for (void *p : EntFind("prop_portal"))
		if (ColourOf(p) == c) return p;
	return nullptr;
}
// (2026-10-12, the user: one-gun chambers lost their map-placed orange portal to the Minecraft link, "it breaks most puzzles") Every portal the
// link takes as the partner is remembered as it was (place, open or not, or made by us); when the link is released it goes back, unless
// it has left the exit since (Chell fired that colour, or the map moved it): then it is theirs and stays.
struct TakenPortal {
	Vector pos;
	std::string ang;
	bool active = false;
	bool spawned = false;  // made by the link (no partner existed): closed again
};
static std::unordered_map<void *, TakenPortal> g_taken;
static void RestoreTaken(const char *why) {
	if (g_taken.empty()) return;
	const std::vector<void *> live = EntFind("prop_portal");
	for (auto &kv : g_taken) {
		void *p = kv.first;
		const TakenPortal &t = kv.second;
		if (std::find(live.begin(), live.end(), p) == live.end()) continue;  // gone
		if ((EntVec(p, "origin") - g_exitPos).Length() > 1.0f) continue;      // moved since: Chell's or the map's now
		if (t.spawned) {
			EntInput(p, "SetActivatedState", "0");
			console->Print("[bridge] mc portal: the partner made for the link closed again (%s)\n", why);
			continue;
		}
		char buf[160];
		std::snprintf(buf, sizeof buf, "%.2f %.2f %.2f %s", t.pos.x, t.pos.y, t.pos.z, t.ang.c_str());
		EntInput(p, "NewLocation", buf);
		EntInput(p, "SetActivatedState", t.active ? "1" : "0");
		console->Print("[bridge] mc portal: partner portal %p back where it was (%s, %s) (%s)\n", p, buf, t.active ? "open" : "closed", why);
	}
	g_taken.clear();
}
static void ReleaseAnchor(const char *why, bool restore = true) {
	if (!g_anchor) return;
	console->Print("[bridge] mc portal: anchor (portal %d) released: %s\n", g_anchorColour, why);
	g_anchor = nullptr, g_anchorColour = -1;
	if (restore) RestoreTaken(why);
}
int ClipArmKey() { return g_armKey; }
static void McHomeLevelReset();
static void McPortalLevelReset() {
	g_anchor = nullptr, g_anchorColour = -1, g_armShot = -1;
	g_taken.clear();  // (a new level: those entities are gone)
	McHomeLevelReset();
}

// ---- the way back: G in Minecraft, and the H hold (bridge #12) -----------------------------------------
// A shot fired with G (armed or held) that lands in the region is a home anchor: its partner is put back in the
// chamber, at the last wall Portal 2 had that colour on in this chamber (or the other colour's), or failing that the
// default spot (the pit flings test wall, else a wall in front of the level's start), so Chell can walk back through
// it. (Not a floor: out of a floor portal she pops up and falls straight back in.) The anchor is
// released like the Minecraft one (moved, closed, the partner's colour fired).
static void *g_homeAnchor = nullptr;
static int g_homeColour = -1, g_homeShot = -1, g_homeShotTick = 0;
static Vector g_homeAnchorPos, g_homeShotPos;
struct ChamberSpot {
	bool ok = false;
	Vector pos;
	QAngle ang;
};
static ChamberSpot g_chamberPortal[2];  // the last wall each colour was on outside the region (this level)
static ChamberSpot g_chamberFeet;       // the last spot Chell stood on in the chamber (pc_mc_home), away from portals
static int g_groundTicks = 0;
bool McArmKeyHeld();       // mc.cpp
const char *McMapName();   // mc.cpp
static void McHomeLevelReset() {
	g_homeAnchor = nullptr, g_homeColour = -1, g_homeShot = -1;
	g_chamberPortal[0] = g_chamberPortal[1] = ChamberSpot();
	g_chamberFeet = ChamberSpot();
}
static void ReleaseHome(const char *why) {
	if (!g_homeAnchor) return;
	console->Print("[bridge] mc home: anchor (portal %d) released: %s\n", g_homeColour, why);
	g_homeAnchor = nullptr, g_homeColour = -1;
}

// portalgun.cpp: every TraceFirePortal (Chell's shot), with the colour and whether it can place.
void ClipOnGunShot(int colour, bool placed, const Vector &pos) {
	if (g_anchor && placed && colour != g_anchorColour) ReleaseAnchor("Chell fired the partner's colour", false), g_taken.clear();  // (she places it herself)
	if (g_homeAnchor && placed && colour != g_homeColour) ReleaseHome("Chell fired the partner's colour");
	const bool held = McArmKeyHeld();
	if (!g_mcArmed && !held) return;
	if (!g_regionMap) StageEnsure("a portal fired for Minecraft");  // (bridge #47) a campaign chamber: the virtual region
	if (placed && ClipRegionContains(pos)) {  // a shot in Minecraft with G: its partner goes home to the chamber
		g_mcArmed = false;
		g_homeShot = colour, g_homeShotTick = g_pollTick, g_homeShotPos = pos;
		console->Print("[bridge] mc home: portal %d fired with G in Minecraft at (%.1f %.1f %.1f)%s; its partner goes back to the chamber once it lands\n", colour, pos.x, pos.y, pos.z,
		               held ? " (key held)" : "");
		return;
	}
	if (!placed) {
		console->Print("[bridge] mc portal: portal %d shot cannot place a portal; still armed\n", colour);
		return;
	}
	g_mcArmed = false;
	g_armShot = colour, g_armShotTick = g_pollTick, g_armShotPos = pos;
	console->Print("[bridge] mc portal: portal %d fired %s at (%.1f %.1f %.1f) (disarmed); its partner goes to Minecraft once it lands\n", colour, held ? "with G held" : "while armed", pos.x,
	               pos.y, pos.z);
}

static void Redirect(void *partner, void *floorPortal);
// Returns true when the anchor rule handled this poll.
static bool AnchorTick() {
	if (g_armShot >= 0) {
		void *p = PortalOfColour(g_armShot);
		if (p) {
			Vector o = EntVec(p, "origin");
			bool active = EntKV(p, "Activated") == "1";
			if ((o - g_armShotPos).Length() < 2.0f && active) {
				ReleaseAnchor("a new anchor", false);  // (the partner goes straight back to the exit: no restore in between)
				g_anchor = p, g_anchorColour = g_armShot, g_anchorPos = o;
				g_armShot = -1;
				console->Print("[bridge] mc portal: portal %d landed at (%.1f %.1f %.1f): its partner is linked to the Minecraft cliff exit\n", g_anchorColour, o.x, o.y, o.z);
				if (ClipRegionContains(o)) ReleaseAnchor("it landed inside the Minecraft region (a normal portal there)");
			}
		}
		if (g_armShot >= 0 && g_pollTick - g_armShotTick > 180) {  // 3 s
			console->Print("[bridge] mc portal: the armed shot (portal %d) did not land a portal\n", g_armShot);
			g_armShot = -1;
		}
	}
	if (!g_anchor) return false;
	bool alive = false;
	for (void *p : g_portals) alive |= p == g_anchor;
	if (!alive) g_portals = EntFind("prop_portal");
	alive = false;
	for (void *p : g_portals) alive |= p == g_anchor;
	if (!alive) return ReleaseAnchor("the portal is gone"), false;
	Vector o = EntVec(g_anchor, "origin");
	if ((o - g_anchorPos).Length() > 1.0f) return ReleaseAnchor("the portal moved"), false;
	if (EntKV(g_anchor, "Activated") != "1" && g_pollTick - g_armShotTick > 120) return ReleaseAnchor("the portal closed"), false;
	// (lead) the partner is the portal Portal 2 actually pairs with the anchor: an ACTIVE one of the other colour in its group. Maps hold several
	// portals of a colour (sp_a1_intro3's map-placed oranges, mostly inactive); taking the last in the list moved an inactive one to the exit while the
	// active one stayed in the chamber, and a map that re-places its portal afterwards was never followed. Of several active ones, one not yet at the
	// exit wins (the map has just activated it), so the redirect follows the map every poll.
	void *partner = nullptr, *partnerInactive = nullptr;
	for (void *p : g_portals) {
		if (p == g_anchor || ColourOf(p) == g_anchorColour || !SameGroup(p, g_anchor)) continue;
		if (EntKV(p, "Activated") != "1") {
			partnerInactive = p;
			continue;
		}
		if (!partner || (EntVec(partner, "origin") - g_exitPos).Length() < 1.0f) partner = p;
	}
	if (!partner) partner = partnerInactive;
	if (!partner && (g_haveExit || ComputeExit("mc portal"))) {
		partner = Spawn("prop_portal", {{"PortalTwo", g_anchorColour ? "0" : "1"}, {"LinkageGroupID", EntKV(g_anchor, "LinkageGroupID")}, {"Activated", "0"}});
		console->Print("[bridge] mc portal: created the partner portal (PortalTwo %d) -> %p\n", !g_anchorColour, partner);
		g_portals = EntFind("prop_portal");
		if (partner) g_taken[partner].spawned = true;
	}
	if (partner && !g_taken.count(partner) && (EntVec(partner, "origin") - g_exitPos).Length() > 1.0f) {
		TakenPortal &t = g_taken[partner];
		t.pos = EntVec(partner, "origin"), t.ang = EntKV(partner, "angles"), t.active = EntKV(partner, "Activated") == "1";
		console->Print("[bridge] mc portal: partner portal %p taken for the link from (%.1f %.1f %.1f) angles %s, %s; it goes back when the link is released\n", partner, t.pos.x,
		               t.pos.y, t.pos.z, t.ang.c_str(), t.active ? "open" : "closed");
	}
	if (partner) Redirect(partner, g_anchor);
	return true;
}

static bool PlaceHome(void *partner, void *anchor) {
	ChamberSpot sp = g_chamberPortal[g_homeColour ? 0 : 1];
	const char *src = "its last chamber wall";
	if (!sp.ok) sp = g_chamberPortal[g_homeColour ? 1 : 0], src = "the anchor colour's last chamber wall";
	if (!sp.ok) {
		// the default: pit flings' test wall; else the first wall straight ahead of the level's start
		if (!std::strcmp(McMapName(), "pc_pit_flings")) sp.ok = true, sp.pos = Vector(-560, -60, 64), sp.ang = QAngle{0, 0, 0}, src = "the default wall spot";
		else {
			const Vector eye = g_startPos + Vector(0, 0, 64);
			Vector f(std::cos(g_startAng.y * (float)M_PI / 180), std::sin(g_startAng.y * (float)M_PI / 180), 0);
			CGameTrace tr;
			engine.Trace(eye, eye + f * 2048, Vector(0, 0, 0), MASK_SOLID_BRUSHONLY, tr, true);
			if (tr.fraction < 1 && std::fabs(tr.plane.normal.z) < 0.3f) {
				const Vector n = tr.plane.normal;
				sp.ok = true, sp.pos = tr.endpos + n * 1.0f, sp.ang = QAngle{0, std::atan2(n.y, n.x) * 180.0f / (float)M_PI, 0}, src = "a wall in front of the start";
			}
		}
	}
	if (!sp.ok) return false;
	if ((EntVec(partner, "origin") - sp.pos).Length() < 1.0f && EntKV(partner, "Activated") == "1") return true;
	char buf[160];
	std::snprintf(buf, sizeof buf, "%.2f %.2f %.2f %.1f %.1f %.1f", sp.pos.x, sp.pos.y, sp.pos.z, sp.ang.x, sp.ang.y, sp.ang.z);
	EntInput(partner, "SetActivatedState", "1");
	EntInput(partner, "NewLocation", buf);
	EntInput(anchor, "SetActivatedState", "1");
	static int n = 0;
	if (++n <= 8) console->Print("[bridge] mc home: partner portal -> %s (%s)\n", src, buf);
	return true;
}
// Returns true when the home rule handled this poll.
static bool HomeTick() {
	if (g_homeShot >= 0) {
		void *p = PortalOfColour(g_homeShot);
		if (p && (EntVec(p, "origin") - g_homeShotPos).Length() < 2.0f && EntKV(p, "Activated") == "1") {
			ReleaseHome("a new anchor");
			ReleaseAnchor("a home anchor", false);  // (the home rule puts the partner back in the chamber itself)
			g_taken.clear();
			g_homeAnchor = p, g_homeColour = g_homeShot, g_homeAnchorPos = EntVec(p, "origin");
			g_homeShot = -1;
			console->Print("[bridge] mc home: portal %d landed in Minecraft at (%.1f %.1f %.1f): its partner is linked back to the chamber\n", g_homeColour, g_homeAnchorPos.x, g_homeAnchorPos.y,
			               g_homeAnchorPos.z);
		}
		if (g_homeShot >= 0 && g_pollTick - g_homeShotTick > 180) console->Print("[bridge] mc home: the shot (portal %d) did not land a portal\n", g_homeShot), g_homeShot = -1;
	}
	if (!g_homeAnchor) return false;
	g_portals = EntFind("prop_portal");
	bool alive = false;
	for (void *p : g_portals) alive |= p == g_homeAnchor;
	if (!alive) return ReleaseHome("the portal is gone"), false;
	if ((EntVec(g_homeAnchor, "origin") - g_homeAnchorPos).Length() > 1.0f) {
		// (a floating-origin shift moves it with the region: follow it)
		if (!ClipRegionContains(EntVec(g_homeAnchor, "origin"))) return ReleaseHome("the portal moved"), false;
		g_homeAnchorPos = EntVec(g_homeAnchor, "origin");
	}
	if (EntKV(g_homeAnchor, "Activated") != "1" && g_pollTick - g_homeShotTick > 120) return ReleaseHome("the portal closed"), false;
	void *partner = nullptr;
	for (void *p : g_portals)
		if (p != g_homeAnchor && ColourOf(p) != g_homeColour && SameGroup(p, g_homeAnchor)) partner = p;
	if (!partner) {
		partner = Spawn("prop_portal", {{"PortalTwo", g_homeColour ? "0" : "1"}, {"LinkageGroupID", EntKV(g_homeAnchor, "LinkageGroupID")}, {"Activated", "0"}});
		console->Print("[bridge] mc home: created the partner portal (PortalTwo %d) -> %p\n", !g_homeColour, partner);
		g_portals = EntFind("prop_portal");
	}
	if (partner) PlaceHome(partner, g_homeAnchor);
	return true;
}

// pc_mc_home: Chell back to the chamber from Minecraft (the H hold): the last spot she stood on in this chamber, or
// the level's start; velocity zeroed; a held cube comes with her.
bool PlayerHolding();                                            // propcollide.cpp
bool FloatTeleportPlayer(const Vector &pos, const QAngle &ang);  // floatorigin.cpp
bool EntTeleport(void *ent, const Vector &pos, const QAngle &ang);
static int g_homes = 0;
static Vector g_homeTo;
// After the jump Portal 2 may have let go of the cube (its hold breaks on a teleport): it is put at her hold point
// and +use pressed for a moment to take it again.
static void *g_regrab = nullptr;
static int g_regrabTick = 0;
static void RegrabTick() {
	if (!g_regrab) return;
	const int t = g_pollTick - g_regrabTick;
	if (t == 3 && PlayerHolding()) {
		g_regrab = nullptr;
		return;
	}
	if (t == 3) {
		const QAngle va = engine.ViewAngles();
		const float y = va.y * (float)M_PI / 180;
		const Vector at = PlayerOrigin() + Vector(0, 0, 50) + Vector(std::cos(y), std::sin(y), 0) * 60.0f;
		const Vector ca = EntVec(g_regrab, "angles");
		EntTeleport(g_regrab, at, QAngle{ca.x, ca.y, ca.z});
		engine.SetAngles(QAngle{std::atan2(14.0f, 60.0f) * 180.0f / (float)M_PI, va.y, 0});  // (her eye is 64 u up, the cube 50)
	}
	if (t == 19) {  // (aimed at where it is now: it has been falling)
		const Vector d = EntVec(g_regrab, "origin") - (PlayerOrigin() + Vector(0, 0, 64));
		const float l = d.Length();
		if (l > 1) engine.SetAngles(QAngle{-std::asin(d.z / l) * 180.0f / (float)M_PI, std::atan2(d.y, d.x) * 180.0f / (float)M_PI, 0});
	}
	if (t == 20) engine.Cmd("+use");
	if (t >= 32) {
		engine.Cmd("-use");
		engine.SetAngles(QAngle{0.0f, engine.ViewAngles().y, 0});
		console->Print("[bridge] mc home: the cube %s\n", PlayerHolding() ? "is in her hands again" : "was not taken again");
		g_regrab = nullptr;
	}
}
CON_COMMAND(pc_mc_home, "pc_mc_home - from Minecraft, back to the last chamber spot Chell stood on (or the level's start); key: hold H (pc_mc_home_key)\n") {
	const Vector feet = PlayerOrigin(), eye = feet + Vector(0, 0, 64);
	if (!g_regionMap || !ClipRegionContains(eye)) return console->Print("[bridge] mc home: not in Minecraft\n");
	const ChamberSpot sp = g_chamberFeet.ok ? g_chamberFeet : ChamberSpot{true, g_startPos, g_startAng};
	// the held cube: the tracked prop nearest her eye
	void *cube = nullptr;
	Vector cubeOff;
	if (PlayerHolding()) {
		float best = 150;
		for (const char *cls : {"prop_weighted_cube", "prop_physics", "prop_physics_override", "prop_monster_box"})
			for (void *e : EntFind(cls)) {
				const float d = (EntVec(e, "origin") - eye).Length();
				if (d < best) best = d, cube = e, cubeOff = EntVec(e, "origin") - feet;
			}
	}
	FloatTeleportPlayer(sp.pos, sp.ang);
	if (cube) {
		// the same offset turned to her new yaw
		const QAngle va = engine.ViewAngles();
		const float a = (sp.ang.y - va.y) * (float)M_PI / 180, c = std::cos(a), s = std::sin(a);
		const Vector off(cubeOff.x * c - cubeOff.y * s, cubeOff.x * s + cubeOff.y * c, cubeOff.z);
		const Vector ca = EntVec(cube, "angles");
		EntTeleport(cube, sp.pos + off, QAngle{ca.x, ca.y, ca.z});
	}
	if (cube) g_regrab = cube, g_regrabTick = g_pollTick;
	++g_homes, g_homeTo = sp.pos;
	console->Print("[bridge] mc home: Chell from (%.0f %.0f %.0f) back to %s (%.0f %.0f %.0f)%s\n", feet.x, feet.y, feet.z, g_chamberFeet.ok ? "her last chamber spot" : "the level's start", sp.pos.x,
	               sp.pos.y, sp.pos.z, cube ? ", with the held cube" : "");
}

// The map's prop_portal entities, kept up to date cheaply every tick (bridge #42): entities are listed by pointer (no key-value reads),
// only a pointer not seen before is asked for its class, and a pointer that left the list is dropped (a stale one must never be read).
// A full class scan every 240 ticks backs up an address reused by another entity between two ticks.
void EntLiveSet(std::unordered_set<void *> &out);  // ents.cpp
static void RefreshPortalList() {
	static std::unordered_set<void *> live;
	std::unordered_set<void *> &known = g_knownEnts;
	int &sinceFull = g_sinceFullScan;
	if (++sinceFull >= 240 || known.empty()) {
		sinceFull = 0;
		g_portals = EntFind("prop_portal");
		g_doorPortals = EntFind("portal_base2D");
		g_doors = EntFind("linked_portal_door");
		EntLiveSet(known);
		return;
	}
	EntLiveSet(live);
	g_portals.erase(std::remove_if(g_portals.begin(), g_portals.end(), [&](void *e) { return !live.count(e); }), g_portals.end());
	g_doorPortals.erase(std::remove_if(g_doorPortals.begin(), g_doorPortals.end(), [&](void *e) { return !live.count(e); }), g_doorPortals.end());
	g_doors.erase(std::remove_if(g_doors.begin(), g_doors.end(), [&](void *e) { return !live.count(e); }), g_doors.end());
	for (void *e : live) {
		if (known.count(e)) continue;
		const std::string cls = EntKV(e, "classname");
		if (cls == "prop_portal" && std::find(g_portals.begin(), g_portals.end(), e) == g_portals.end()) g_portals.push_back(e);
		else if (cls == "portal_base2D" && std::find(g_doorPortals.begin(), g_doorPortals.end(), e) == g_doorPortals.end()) g_doorPortals.push_back(e);
		else if (cls == "linked_portal_door" && std::find(g_doors.begin(), g_doors.end(), e) == g_doors.end()) g_doors.push_back(e);
	}
	known.swap(live);
}


// (bridge #68) The doors' portals. A portal_base2D is Portal 2's portal class (m_bActivated, m_hLinkedPortal, m_fNetworkHalfWidth / Height); the linked_portal_door
// that made it holds its handle (m_hPortal). A portal's partner is the portal whose handle is its m_hLinkedPortal, so the pair is read from Portal 2 itself
// (a map links doors by name, by SetPartner at run time, or not at all: wakeup's incinerator is opened and linked by one relay). Each linked pair takes the
// slots of a group of its own (PortalSlotOf, group numbers from 100000, the lower pointer being colour 0); a pair that is not open and linked fills no slot,
// like a lone prop_portal. The size comes from the portal itself (the doors are 64 x 64 and 64 x 128, not the gun's 64 x 112).
static void ClipDoorPortals() {
	static int offAct = -2, offLink = -2, offHW = -2, offHH = -2, offDoorH = -2;
	std::unordered_map<unsigned, void *> byHandle;  // a portal's handle (from its door) -> the portal
	for (void *d : g_doors) {
		if (offDoorH == -2) offDoorH = EntFieldOffset(d, "m_hPortal");
		if (offDoorH < 0) break;
		const unsigned h = *(const unsigned *)((const char *)d + offDoorH);
		if (!h || h == 0xFFFFFFFFu) continue;
		const Vector o = EntVec(d, "origin");
		void *best = nullptr;
		float bd = 3.0f;
		for (void *p : g_doorPortals) {
			const float dd = (EntVec(p, "origin") - o).Length();
			if (dd < bd) bd = dd, best = p;
		}
		if (best) byHandle[h] = best;
	}
	std::unordered_set<void *> filled;
	for (void *p : g_doorPortals) {
		if (offAct == -2) offAct = EntFieldOffset(p, "m_bActivated"), offLink = EntFieldOffset(p, "m_hLinkedPortal"), offHW = EntFieldOffset(p, "m_fNetworkHalfWidth"), offHH = EntFieldOffset(p, "m_fNetworkHalfHeight");
		if (offAct < 0 || offLink < 0) return;
		const bool active = *((const unsigned char *)p + offAct) != 0;
		const unsigned lh = *(const unsigned *)((const char *)p + offLink);
		auto it = lh && lh != 0xFFFFFFFFu ? byHandle.find(lh) : byHandle.end();
		void *q = it == byHandle.end() ? nullptr : it->second;
		if (!q || q == p) continue;
		const bool qActive = *((const unsigned char *)q + offAct) != 0;
		void *lo = p < q ? p : q;
		auto pid = g_doorPairId.find(lo);
		if (pid == g_doorPairId.end()) pid = g_doorPairId.emplace(lo, (int)g_doorPairId.size()).first;
		const int slot = PortalSlotOf(100000 + pid->second, p > q);
		if (slot < 0) continue;
		const Vector o = EntVec(p, "origin"), a = EntVec(p, "angles");
		const float py = a.y * (float)M_PI / 180, pp = a.x * (float)M_PI / 180;
		g_portalPos[slot] = o;
		g_portalNormal[slot] = Vector(std::cos(pp) * std::cos(py), std::cos(pp) * std::sin(py), -std::sin(pp));
		{
			const float pr = a.z * (float)M_PI / 180, sr = std::sin(pr), cr = std::cos(pr), sp = std::sin(pp), cp = std::cos(pp), sy = std::sin(py), cy = std::cos(py);
			g_portalUp[slot] = Vector(cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp);
		}
		const float hw = offHW >= 0 ? *(const float *)((const char *)p + offHW) : 0.0f, hh = offHH >= 0 ? *(const float *)((const char *)p + offHH) : 0.0f;
		g_portalHalfW[slot] = hw > 4.0f && hw < 400.0f ? hw : 32.0f;
		g_portalHalfH[slot] = hh > 4.0f && hh < 400.0f ? hh : 56.0f;
		const bool on = active && qActive;
		g_portalSet[slot] = on;
		if (on) g_slotSeen[slot] = g_pollTick;
		g_doorSlot[p] = slot;
		filled.insert(p);
		auto lo2 = g_doorOn.find(p);
		if (lo2 == g_doorOn.end() || lo2->second != on) {
			g_doorOn[p] = on;
			console->Print("[bridge] doors: linked_portal_door portal %p '%s' %s: slot %d (pair %d), at (%.0f %.0f %.0f) angles (%.0f %.0f %.0f), opening %.0f x %.0f\n", p, EntKV(p, "targetname").c_str(), on ? "OPEN and linked" : "linked, not open yet", slot,
			               pid->second, o.x, o.y, o.z, a.x, a.y, a.z, g_portalHalfW[slot] * 2, g_portalHalfH[slot] * 2);
		}
	}
	// a door portal that lost its link or its partner no longer fills its slot
	for (auto &kv : g_doorSlot)
		if (!filled.count(kv.first) && kv.second >= 0 && g_portalSet[kv.second]) {
			g_portalSet[kv.second] = false;
			console->Print("[bridge] doors: portal %p lost its link: slot %d cleared\n", kv.first, kv.second);
		}
}

void McTransitCarryTick();  // mctransit.cpp (bridge #66)
// Every server tick.
void UnifiedWorldChanged();  // unified.cpp
// (bridge #81) The exit spot is found by the mod once per new world and reported (cache/bridge/portalcraft_world.env): every anchor derives from it. It appears
// after the plugin has loaded (Minecraft opens its world once linked), so the cliff, the terrain mapping's anchor and the chamber band follow it here.
static void ClipWorldSync() {
	static int seen = 0;
	if (!WorldChanged(seen)) return;
	const WorldConf &w = World();
	g_haveExit = false;
	g_exitLogState = -1;
	if (!w.known) {
		console->Print("[bridge] world: no world reported by Minecraft (yet)\n");
		return;
	}
	g_cliffX = w.cliffX, g_cliffY = w.cliffY, g_cliffZ = w.cliffZ, g_cliffDx = w.cliffDx, g_cliffDz = w.cliffDz;
	g_usePin = true;
	console->Print("[bridge] world: Minecraft reports %s (seed %lld): exit patch wall block (%d %d %d) facing (%d %d), cliff anchor MC (%.0f %.0f %.0f), chamber band origin MC (%.0f %.0f %.0f)\n", w.save.c_str(), w.seed, w.exitWx,
	               w.exitWy, w.exitWz, w.cliffDx, w.cliffDz, w.cliffX, w.cliffY, w.cliffZ, w.chamberX, w.chamberY, w.chamberZ);
	if (g_regionMap && g_tbSet) {  // a region map is loaded: its terrain mapping was made without the anchor
		g_tbAnchor.z = g_tbMin.z + (float)((g_cliffY - 40.0) * kUnitsPerBlock);
		TerrainSetRegion(g_cliffX, g_cliffY, g_cliffZ, g_tbAnchor);
	}
	UnifiedWorldChanged();
}

void ClipTick() {
	ClipWorldSync();
	// (bridge #42) the portal slots below are tracked on EVERY map. They were tracked on region maps only (this function returned at once
	// without a pc_mc_region), so in a campaign chamber the gun's portals never reached the arrays that carve the holes and carry the
	// Minecraft player through: it walked into a portal on the mirror world and nothing happened.
	// Minecraft lava burns like Portal 2's goo: Chell dies in it.
	static int lavaCooldown = 0;
	if (lavaCooldown > 0) --lavaCooldown;
	else if (g_regionMap && FluidAt(PlayerOrigin() + Vector(0, 0, 4)) == 2) {
		for (void *pl : EntFind("player")) EntInput(pl, "SetHealth", "0");
		console->Print("[bridge] clip: Chell is in Minecraft lava\n");
		lavaCooldown = 120;
	}
	// (every tick: the composite follows placements and closings at once, see composite.cpp UpdatePair)
	++g_pollTick;
	RegrabTick();
	McTransitCarryTick();  // (bridge #66)
	RefreshPortalList();
	void *floorPortal = nullptr, *partner = nullptr;
	// (bridge #32) portals are paired by LinkageGroupID, as Portal 2 pairs them: one portal per colour per group. Slot = group * 2 + colour
	// (PortalSlotOf; group 0, the gun's pair, is slots 0 and 1), so every pre-placed pair of a map has slots of its own and the arrays that
	// carve holes and carry the Minecraft player through (mc.cpp, mctransit.cpp, the mod's PortalPair) know every active pair.
	// (Two portals of one colour in one group is not a pair Portal 2 knows: the active one is kept.)
	for (void *p : g_portals) {
		bool two = EntKV(p, "PortalTwo") == "1";
		bool active = EntKV(p, "Activated") == "1";
		const int group = std::atoi(EntKV(p, "LinkageGroupID").c_str());
		const int slot = PortalSlotOf(group, two);
		Vector o = EntVec(p, "origin"), a = EntVec(p, "angles");
		if (slot < 0) continue;
		if (!active && g_portalSet[slot] && g_slotSeen[slot] == g_pollTick) continue;
		QAngle ang{a.x, a.y, a.z};
		// the collision hole rule follows the real portals
		float py = ang.y * (float)M_PI / 180, pp = ang.x * (float)M_PI / 180;
		g_portalPos[slot] = o;
		g_portalNormal[slot] = Vector(std::cos(pp) * std::cos(py), std::cos(pp) * std::sin(py), -std::sin(pp));
		{
			float pr = ang.z * (float)M_PI / 180, sr = std::sin(pr), cr = std::cos(pr), sp = std::sin(pp), cp = std::cos(pp), sy = std::sin(py), cy = std::cos(py);
			g_portalUp[slot] = Vector(cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp);
		}
		g_portalSet[slot] = active;
		g_portalHalfW[slot] = 32.0f, g_portalHalfH[slot] = 56.0f;
		if (active) g_slotSeen[slot] = g_pollTick;
		if (group != 0 || !g_regionMap) continue;  // (the pit / cliff / home rules below are the gun's, on maps with a Minecraft region: group 0 only)
		// (wall portals only: a floor portal as the way back pops Chell up and drops her straight back in)
		if (active && !ClipRegionContains(o) && std::fabs(std::sin(pp)) < 0.7f) g_chamberPortal[two] = ChamberSpot{true, o, ang};
		// floor portals in the chamber only (a floor portal on Minecraft terrain is a normal portal)
		if (active && OnFloor(ang) && InZone(o) && !ClipRegionContains(o)) floorPortal = p;
	}
	ClipDoorPortals();
	if (!g_regionMap) return;  // (the rest is the region maps' pit / cliff / home rules)
	// the last chamber spot Chell stood on (for pc_mc_home): on the ground for half a second, away from open portals
	{
		const Vector feet = PlayerOrigin(), vel = PlayerVelocity();
		bool nearPortal = false;
		for (int i = 0; i < 2; ++i) nearPortal |= g_portalSet[i] && (g_portalPos[i] - (feet + Vector(0, 0, 36))).Length() < 120.0f;
		if (!ClipRegionContains(feet + Vector(0, 0, 64)) && std::fabs(vel.z) < 1.0f && !nearPortal && feet.Length() > 1.0f) {
			if (++g_groundTicks >= 30) g_chamberFeet = ChamberSpot{true, feet, engine.ViewAngles()};
		} else {
			g_groundTicks = 0;
		}
	}
	if (g_pollTick % 4) return;  // (the portal state above every tick; the rules below every 4th, as before)
	if (HomeTick()) return;
	if (AnchorTick()) return;
	if (!floorPortal || g_pitMode != 2) return;
	std::string two = EntKV(floorPortal, "PortalTwo");
	for (void *p : g_portals)
		if (p != floorPortal && EntKV(p, "PortalTwo") != two && SameGroup(p, floorPortal)) partner = p;
	if (!partner && (g_haveExit || ComputeExit())) {
		// Chell has only fired one colour so far: the other portal of the pair is created.
		partner = Spawn("prop_portal", {{"PortalTwo", two == "1" ? "0" : "1"}, {"LinkageGroupID", EntKV(floorPortal, "LinkageGroupID")}, {"Activated", "0"}});
		console->Print("[bridge] clip: created the partner portal (PortalTwo %s) -> %p\n", two == "1" ? "0" : "1", partner);
		g_portals = EntFind("prop_portal");
	}
	// A partner Chell has shot onto Minecraft terrain herself stays there (it links back to the
	// chamber's floor portal); one anywhere in the chamber is sent to the cliff.
	// (Portal 2 reports a portal that is still opening as not activated, so "moved within the last
	// 2 s" counts as placed too.)
	if (partner) {
		static void *lastPartner = nullptr;
		static Vector lastPos;
		static int movedTick = -100000;
		Vector po = EntVec(partner, "origin");
		if (partner != lastPartner || (po - lastPos).Length() > 1.0f) movedTick = g_pollTick, lastPartner = partner, lastPos = po;
		bool placed = EntKV(partner, "Activated") == "1" || g_pollTick - movedTick < 120;
		if (placed && ClipRegionContains(po) && (po - g_exitPos).Length() > 1.0f) return;
		if (placed && ClipRegionContains(po) && g_haveExit) return;
	}
	if (partner) Redirect(partner, floorPortal);
}

bool ClipHasRegion() { return g_regionMap; }
Vector ClipRegionCentre() { return (g_regionMin + g_regionMax) * 0.5f; }
void ClipRegionBounds(Vector &mn, Vector &mx) { mn = g_regionMin, mx = g_regionMax; }
// Floating origin (floatorigin.cpp): the mapping moved by whole chunks. The exit is a Minecraft place, so it
// is recomputed through the new mapping; it is only usable while it is well inside the region.
void ClipShift(const Vector &d) {
	if (!g_haveExit) return;
	SetExit(g_exitPatch, g_usePin ? "pinned" : "found", "floating origin shift");
	const Vector m(200, 200, 200);
	bool inside = g_exitPos.x > g_regionMin.x + m.x && g_exitPos.x < g_regionMax.x - m.x && g_exitPos.y > g_regionMin.y + m.y && g_exitPos.y < g_regionMax.y - m.y &&
	              g_exitPos.z > g_regionMin.z + m.z && g_exitPos.z < g_regionMax.z - m.z;
	if (!inside) {
		g_haveExit = false;
		console->Print("[bridge] clip: the cliff exit is outside the region after the shift (%.0f %.0f %.0f): unavailable until the mapping comes back\n", g_exitPos.x, g_exitPos.y, g_exitPos.z);
	}
}
bool ClipExitInRegion() { return g_haveExit; }
bool ClipHaveExit() { return g_regionMap && g_haveExit; }
// The cliff exit (where the partner portal goes), once the terrain around the cliff has streamed.
bool ClipExitPose(Vector &pos, QAngle &ang) {
	if (!g_regionMap || (!g_haveExit && !ComputeExit())) return false;
	pos = g_exitPos, ang = g_exitAng;
	return true;
}

// Where the exit will be, before its terrain has streamed (camera warm-up only; never a redirect).
bool ClipExitHint(Vector &pos, QAngle &ang) {
	if (!g_regionMap) return false;
	if (g_haveExit) return pos = g_exitPos, ang = g_exitAng, true;
	if (g_stageMode) {
		int wallX = 0, zc = 0;
		if (!StageWindow(wallX, zc)) return false;
		double mx, my, mz;
		PatchCentre(Patch{wallX + 1, kStageLow + 1, zc, -1, 0}, mx, my, mz);
		pos = McToSrc(mx, my, mz);
		ang = QAngle{0.0f, McYawToSrc((float)(std::atan2(1.0, 0.0) * 180.0 / M_PI)), 0.0f};
		return true;
	}
	if (!g_usePin) return false;
	double mx, my, mz;
	PatchCentre(PinnedPatch(), mx, my, mz);
	pos = McToSrc(mx, my, mz);
	ang = QAngle{0.0f, McYawToSrc((float)(std::atan2(-PinnedPatch().dx, PinnedPatch().dz) * 180.0 / M_PI)), 0.0f};
	return true;
}

bool ClipRegionContains(const Vector &p) {
	return g_regionMap && p.x > g_regionMin.x && p.x < g_regionMax.x && p.y > g_regionMin.y && p.y < g_regionMax.y && p.z > g_regionMin.z && p.z < g_regionMax.z;
}
// The portal of the shared pair that sits inside the region (the cliff portal), or -1.
int ClipRegionPortal() {
	for (int i = 0; i < 2; ++i)
		if (g_portalSet[i] && ClipRegionContains(g_portalPos[i])) return i;
	return -1;
}

CON_COMMAND(pc_reset, "pc_reset - portals cleared, Chell back at the chamber start (Minecraft terrain stays)\n") {
	for (void *p : EntFind("prop_portal")) EntInput(p, "Fizzle");
	SnapPlayer(g_startPos, g_startAng);
	console->Print("[bridge] clip: reset to (%.0f %.0f %.0f)\n", g_startPos.x, g_startPos.y, g_startPos.z);
}

CON_COMMAND(pcb_redirect_zone, "pcb_redirect_zone <x> <y> <z> [radius=256] - floor portals here send their partner to the cliff\n") {
	if (args.ArgC() < 4) return console->Print("%s", pcb_redirect_zone.ThisPtr()->m_pszHelpString);
	g_zones.push_back({Vector((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3])), args.ArgC() > 4 ? (float)std::atof(args[4]) : 256.0f});
}

CON_COMMAND(pcb_cliff, "pcb_cliff <mcX> <mcY> <mcZ> <dx> <dz> - choose the Minecraft cliff (top block, facing direction)\n") {
	if (args.ArgC() < 6) {
		return console->Print("[bridge] cliff MC (%.0f %.0f %.0f) facing (%d %d), exit %s (%.1f %.1f %.1f), %d redirects\n", g_cliffX, g_cliffY, g_cliffZ, g_cliffDx, g_cliffDz,
		                      g_haveExit ? "found" : "not yet", g_exitPos.x, g_exitPos.y, g_exitPos.z, g_redirects);
	}
	g_cliffX = std::atof(args[1]), g_cliffY = std::atof(args[2]), g_cliffZ = std::atof(args[3]);
	g_cliffDx = std::atoi(args[4]), g_cliffDz = std::atoi(args[5]);
	g_haveExit = false;
	g_usePin = false;  // another cliff: the validated finder picks its patch
	g_exitLogState = -1;
}

CON_COMMAND(pcb_cliff_pin, "pcb_cliff_pin - back to the pinned cliff exit of world.conf\n") {
	if (World().known) g_cliffX = World().cliffX, g_cliffY = World().cliffY, g_cliffZ = World().cliffZ, g_cliffDx = World().cliffDx, g_cliffDz = World().cliffDz;
	g_usePin = true;
	g_haveExit = false;
	g_exitLogState = -1;
}

void TestCheck(bool ok, const char *what, const char *detail);  // testassert.cpp
CON_COMMAND(pcb_assert_exit, "pcb_assert_exit - the cliff exit is the pinned patch and it has a solid wall behind and 2 blocks of air in front\n") {
	char why[160], buf[320];
	PatchState st = CheckPatch(PinnedPatch(), why, sizeof why);
	double mx, my, mz;
	PatchCentre(PinnedPatch(), mx, my, mz);
	Vector want = McToSrc(mx, my, mz);
	float d = g_haveExit ? (g_exitPos - want).Length() : -1;
	std::snprintf(buf, sizeof buf, "exit %s, %.2f u from the pinned MC (%.2f %.2f %.2f); pinned patch: %s", g_haveExit ? "set" : "missing", d, mx, my, mz, why);
	TestCheck(g_haveExit && d >= 0 && d < 0.1f && st == kPatchOk, "cliff exit pinned and valid", buf);
}

CON_COMMAND(pcb_cliff_scan, "pcb_cliff_scan - print the terrain solidity around the cliff (debug)\n") {
	int bx = (int)std::floor(g_cliffX), bz = (int)std::floor(g_cliffZ);
	for (int y = (int)g_cliffY + 4; y >= (int)g_cliffY - 40; y -= 4) {
		char row[64];
		int n = 0;
		for (int k = -8; k < 24 && n < 60; ++k) row[n++] = TerrainIsSolid(bx + g_cliffDx * k, y, bz + g_cliffDz * k) ? '#' : '.';
		row[n] = 0;
		console->Print("[bridge] cliff y %3d: %s\n", y, row);
	}
}

CON_COMMAND(pcb_clip_exit, "pcb_clip_exit - (re)compute the cliff exit and log it\n") {
	g_haveExit = false;
	if (!ComputeExit("pcb_clip_exit", true)) console->Print("[bridge] clip: cliff face not found yet (terrain not streamed?)\n");
}

CON_COMMAND(pc_mc_portal, "pc_mc_portal [1|0] - arm (or cancel; no argument toggles): the next portal Chell fires links its partner to the Minecraft cliff exit (key: G, pc_mc_portal_key)\n") {
	g_mcArmed = args.ArgC() > 1 ? std::atoi(args[1]) != 0 : !g_mcArmed;
	console->Print("[bridge] mc portal: %s\n", g_mcArmed ? "armed (the next portal shot links to Minecraft)" : "disarmed (cancelled)");
	if (g_mcArmed && !g_regionMap) StageEnsure("armed");  // (bridge #47) a campaign chamber: the virtual region and the stage start now (the mod needs a moment to make it)
	if (g_armSound) engine.Cmd(g_mcArmed ? "playvol weapons/portalgun/portal_fizzle2.wav 0.4" : "playvol weapons/portalgun/portal_fizzle2.wav 0.2");
}

CON_COMMAND(pc_mc_portal_key, "pc_mc_portal_key [scancode|0] - the key that runs pc_mc_portal (SDL scancode, default 10 = G; 0 off)\n") {
	if (args.ArgC() > 1) g_armKey = std::atoi(args[1]);
	console->Print("[bridge] mc portal key: scancode %d\n", g_armKey);
}

CON_COMMAND(pc_mc_portal_sound, "pc_mc_portal_sound [0|1] - a quiet Portal 2 sound when arming/cancelling (default 0)\n") {
	if (args.ArgC() > 1) g_armSound = std::atoi(args[1]) != 0;
	console->Print("[bridge] mc portal sound: %d\n", g_armSound);
}

CON_COMMAND(pc_pit_redirect_mode, "pc_pit_redirect_mode [0|1|2] - the pit floor: 0 a normal portal, 1 Minecraft only when fired with G held or armed (default), 2 always Minecraft (the old rule; tests)\n") {
	if (args.ArgC() > 1) g_pitMode = std::max(0, std::min(2, std::atoi(args[1])));
	console->Print("[bridge] pit redirect mode %d (%s)\n", g_pitMode, g_pitMode == 0 ? "off" : g_pitMode == 1 ? "G only" : "always");
}
CON_COMMAND(pc_pit_redirect, "pc_pit_redirect [0|1] - old switch: 0 = pc_pit_redirect_mode 0, 1 = pc_pit_redirect_mode 2 (always)\n") {
	if (args.ArgC() > 1) g_pitMode = std::atoi(args[1]) != 0 ? 2 : 0;
	console->Print("[bridge] pit redirect mode %d\n", g_pitMode);
}

CON_COMMAND(pcb_mc_portal_state, "pcb_mc_portal_state - armed / pending shot / anchor (tests)\n") {
	console->Print("[bridge] mc portal state: armed %d, pending shot %d, anchor portal %d at (%.1f %.1f %.1f), pit redirect mode %d, region portal %d, home anchor %d\n", g_mcArmed, g_armShot, g_anchor ? g_anchorColour : -1,
	               g_anchorPos.x, g_anchorPos.y, g_anchorPos.z, g_pitMode, ClipRegionPortal(), g_homeAnchor ? g_homeColour : -1);
}

void TestCheck(bool ok, const char *what, const char *detail);
CON_COMMAND(pcb_assert_portal_region, "pcb_assert_portal_region <colour 0|1> <1 in Minecraft | 0 in the chamber> - that portal is open and on that side\n") {
	const int c = args.ArgC() > 1 ? std::atoi(args[1]) & 1 : 0;
	const bool want = args.ArgC() > 2 && std::atoi(args[2]) != 0;
	void *p = PortalOfColour(c);
	char buf[200];
	const Vector o = p ? EntVec(p, "origin") : Vector(0, 0, 0);
	const bool on = p && EntKV(p, "Activated") == "1";
	std::snprintf(buf, sizeof buf, "portal %d %s at (%.1f %.1f %.1f): %s (want %s)", c, on ? "open" : "not open", o.x, o.y, o.z, p && ClipRegionContains(o) ? "in Minecraft" : "in the chamber",
	              want ? "in Minecraft" : "in the chamber");
	TestCheck(on && ClipRegionContains(o) == want, "portal side", buf);
}
CON_COMMAND(pcb_assert_player_region, "pcb_assert_player_region <1 in Minecraft | 0 in the chamber> [x y z tol] - Chell's eye is on that side (and her feet within tol of x y z)\n") {
	const bool want = args.ArgC() > 1 && std::atoi(args[1]) != 0;
	const Vector feet = PlayerOrigin();
	char buf[220];
	float d = -1;
	if (args.ArgC() > 5) d = (feet - Vector((float)std::atof(args[2]), (float)std::atof(args[3]), (float)std::atof(args[4]))).Length();
	const float tol = args.ArgC() > 5 ? (float)std::atof(args[5]) : 0;
	std::snprintf(buf, sizeof buf, "Chell at (%.1f %.1f %.1f): %s (want %s)%s, velocity %.0f u/s, %d homes, last to (%.0f %.0f %.0f), %.1f u from the expected spot", feet.x, feet.y, feet.z,
	              ClipRegionContains(feet + Vector(0, 0, 64)) ? "in Minecraft" : "in the chamber", want ? "in Minecraft" : "in the chamber", "", PlayerVelocity().Length(), g_homes, g_homeTo.x, g_homeTo.y,
	              g_homeTo.z, d);
	TestCheck(ClipRegionContains(feet + Vector(0, 0, 64)) == want && (d < 0 || d <= tol), "Chell's side", buf);
}
CON_COMMAND(pcb_assert_mc_home, "pcb_assert_mc_home <colour> - that portal is the home anchor (in Minecraft) and its partner is open in the chamber\n") {
	const int c = args.ArgC() > 1 ? std::atoi(args[1]) & 1 : 0;
	void *q = PortalOfColour(!c);
	const Vector qo = q ? EntVec(q, "origin") : Vector(0, 0, 0);
	char buf[220];
	std::snprintf(buf, sizeof buf, "home anchor %d (want %d) at (%.1f %.1f %.1f); partner %s at (%.1f %.1f %.1f) %s", g_homeAnchor ? g_homeColour : -1, c, g_homeAnchorPos.x, g_homeAnchorPos.y,
	              g_homeAnchorPos.z, q && EntKV(q, "Activated") == "1" ? "open" : "not open", qo.x, qo.y, qo.z, q && ClipRegionContains(qo) ? "in Minecraft" : "in the chamber");
	TestCheck(g_homeAnchor && g_homeColour == c && ClipRegionContains(g_homeAnchorPos) && q && EntKV(q, "Activated") == "1" && !ClipRegionContains(qo), "mc home portal pair", buf);
}
CON_COMMAND(pcb_assert_mc_anchor, "pcb_assert_mc_anchor <colour 0|1|-1> - (-1: no anchor, nothing armed or pending) that portal is the mc-portal anchor, outside the region, and its partner sits at the cliff exit\n") {
	int c = args.ArgC() > 1 ? std::atoi(args[1]) : 0;
	char buf[256];
	int pi = ClipRegionPortal();
	float d = pi >= 0 && g_haveExit ? (g_portalPos[pi] - g_exitPos).Length() : -1;
	std::snprintf(buf, sizeof buf, "anchor %d (want %d) at (%.1f %.1f %.1f), region portal %d %.1f u from the exit", g_anchor ? g_anchorColour : -1, c, g_anchorPos.x, g_anchorPos.y, g_anchorPos.z, pi, d);
	if (c < 0) return TestCheck(!g_anchor && !g_mcArmed && g_armShot < 0, "no mc portal anchor (not armed)", buf);
	TestCheck(g_anchor && g_anchorColour == c && !ClipRegionContains(g_anchorPos) && pi == !c && d >= 0 && d < 4, "mc portal anchor linked to the cliff", buf);
}

}  // namespace bridge
