// Portalcraft bridge #47: the Minecraft stage and the virtual region (see stage.hpp for the idea).
#include "stage.hpp"

#include "coords.hpp"
#include "link.hpp"
#include "stagecalc.hpp"

#include "Modules/Tier1.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace bridge {

const char *MapIndexBsp();                      // mapindex.cpp
bool ClipHasRegion();                           // clip.cpp
void ClipStageBegin(const Vector &mn, const Vector &mx, int wallX, int zc);  // clip.cpp: the region flags, the exit search
void TestCheck(bool ok, const char *what, const char *detail);  // testassert.cpp
void TerrainSetMapReady(bool ready);            // terrain.cpp
bool McTerrainModeNow();                        // mc.cpp (bridge #61)
bool ClipTerrainBox(Vector &mn, Vector &mx);    // clip.cpp (bridge #61): the last real region map's box
bool ClipTerrainCliffStreamed();                // clip.cpp
void ClipTerrainWindowBegin(const Vector &mn, const Vector &mx);  // clip.cpp
bool WorldUnifiedChamber();                     // unified.cpp (bridge #78): this level's chamber is in the unified world's band
void ClipUnifiedWindowBegin(const Vector &mn, const Vector &mx);  // clip.cpp (bridge #78)
void ClipCliffMc(double &x, double &y, double &z, int &faceX, int &faceY, int &faceZ, int &dx, int &dz);  // clip.cpp (bridge #78): the pinned cliff

namespace {

bool g_enabled = true;  // pc_mc_stage: the portal onto Minecraft in campaign chambers (bridge #49: on by default)

struct Stage {
	bool computed = false, ok = false, active = false;
	stagecalc::Box world;
	float centre[3] = {0, 0, 0}, gap = 0;
	int wallX = 0, zc = 0;
	double wcx = 0, wcy = 0, wcz = 0;  // the window's centre (Minecraft)
	std::string note;
	bool noLink = false;
	bool terrain = false;  // (bridge #61) the window is the terrain session's own (no stage in the mod's world): the offset is 0
	bool unified = false;  // (bridge #78) the unified world's window: the real terrain round the pinned cliff, under the chamber band
	Vector bmn{0, 0, 0}, bmx{0, 0, 0};  // (bridge #78) its box
} g;

}  // namespace

bool StageActive() { return g.active && g_map.stage.on; }

// (bridge #74) Portal 2 culls a portal whose place is outside the camera's PVS (r_portal_use_pvs_optimization, default 1). A portal in the virtual region's
// solid void never is inside it, so it was never drawn at all: no view of the chamber from the Minecraft side (Minecraft drew a flat orange oval instead).
// With the culling off the engine draws such a portal and its view like any other, the chamber seen from the partner portal. Only while the stage is on
// (the cvar is not archived; the next level puts it back).
static bool g_pvsOff = false;
static void StagePortalCulling(bool off) {
	if (g_pvsOff == off) return;
	g_pvsOff = off;
	engine.Cmd(off ? "r_portal_use_pvs_optimization 0" : "r_portal_use_pvs_optimization 1");
}

void StageLevelReset() {
	StagePortalCulling(false);
	g_map.stage.on = false;
	g = Stage();
}

bool StageWindow(int &wallX, int &zc) {
	if (!StageActive()) return false;
	wallX = g.wallX, zc = g.zc;
	return true;
}

void StageAnchorMc(double &x, double &y, double &z) {
	if (g.unified) {  // (bridge #78) in front of the pinned cliff's exit, in the air
		double cx, cy, cz;
		int fx, fy, fz, dx, dz;
		ClipCliffMc(cx, cy, cz, fx, fy, fz, dx, dz);
		x = fx + 0.5 + dx * 4.0, y = fy + 4.0, z = fz + 1.0 + dz * 4.0;
		return;
	}
	x = g.wallX - 4.0, y = kStageLow + 8.0, z = g.zc + 1.0;
}

void StageHostFill(skycraft::proto::HostPlayer &hp) {
	if (!StageActive() || g.terrain) return;  // (the terrain world has no stage: the mod is not asked for one)
	hp.flags |= skycraft::proto::kHpStage;
	hp.stageChunkX = (int)std::floor(g.wcx / 16.0);
	hp.stageChunkZ = (int)std::floor(g.wcz / 16.0);
	hp.stageRadius = 12;  // (192 blocks around the window: the horizon of the Minecraft view past the box)
}

Vector StageParkSpot() {
	const Vector m = g_map.stage.mx + Vector(60, 60, 60);
	return Vector(std::fmin(m.x, 16350.0f), std::fmin(m.y, 16350.0f), std::fmin(m.z, 16350.0f));
}

bool StageMcRegionSkip(int ry) {
	const StageMap &st = g_map.stage;
	if (!st.on) return false;
	if (st.yMax < 1e29) return ry * 8 < (int)st.yMax + 8;  // (bridge #78) the unified window: the terrain under the chamber band
	return ry * 8 >= (int)st.yMin - 8;
}
bool StageUnified() { return StageActive() && g.unified; }

bool StageHullNear(const Vector &lo, const Vector &hi, float pad) {
	const StageMap &st = g_map.stage;
	if (!st.on) return false;
	return hi.x > st.mn.x - pad && lo.x < st.mx.x + pad && hi.y > st.mn.y - pad && lo.y < st.mx.y + pad && hi.z > st.mn.z - pad && lo.z < st.mx.z + pad;
}

// The virtual region's six walls: slabs just outside the box (Portal 2's world ends there for Chell: past +-16384 u it kills, and the stage
// goes on past the window), kept as boxes the hull sweep treats like blocks.
int StageWallBoxes(const Vector &lo, const Vector &hi, Vector mn[6], Vector mx[6]) {
	const StageMap &st = g_map.stage;
	if (!st.on) return 0;
	const float t = 400.0f;
	int n = 0;
	auto add = [&](const Vector &a, const Vector &b) {
		if (hi.x > a.x && lo.x < b.x && hi.y > a.y && lo.y < b.y && hi.z > a.z && lo.z < b.z) mn[n] = a, mx[n] = b, ++n;
	};
	const Vector lo3 = st.mn - Vector(t, t, t), hi3 = st.mx + Vector(t, t, t);
	add(Vector(lo3.x, lo3.y, lo3.z), Vector(st.mn.x, hi3.y, hi3.z));  // -x
	add(Vector(st.mx.x, lo3.y, lo3.z), Vector(hi3.x, hi3.y, hi3.z));  // +x
	add(Vector(lo3.x, lo3.y, lo3.z), Vector(hi3.x, st.mn.y, hi3.z));  // -y
	add(Vector(lo3.x, st.mx.y, lo3.z), Vector(hi3.x, hi3.y, hi3.z));  // +y
	add(Vector(lo3.x, lo3.y, lo3.z), Vector(hi3.x, hi3.y, st.mn.z));  // floor
	add(Vector(lo3.x, lo3.y, st.mx.z), Vector(hi3.x, hi3.y, hi3.z));  // ceiling
	return n;
}

// (bridge #61) A campaign map in a TERRAIN session (the pit flings world: real generated terrain, no stage in the mod's world). The terrain around the pinned
// cliff is already mapped into Portal 2's world at the box of the region map the session started on (g_map is untouched, so the Minecraft player's place
// in the chamber and everything the mapping carries stay as they are). On this map that box is empty space apart from whatever of the map reaches into
// it: its top is cut below the map's lowest brush (stagecalc::BspExtent), and what is left is the virtual region, exactly as the mirror world's stage is
// (the same StageMap with a zero offset: the box's invisible walls, Portal 2's void dropped there, the composite's portal rules). The exit is the
// pinned cliff patch, as in pit flings.
static bool TerrainWindowEnsure(const char *why) {
	Vector bmn, bmx;
	if (!ClipTerrainBox(bmn, bmx)) {
		if (!g.computed) g.computed = true, console->Print("[bridge] stage: the terrain session has no region box to reuse (%s)\n", why);
		return false;
	}
	if (!g.computed) {
		g.computed = true;
		const char *bsp = MapIndexBsp();
		float top = bmx.z;
		if (bsp && *bsp && stagecalc::BspExtent(bsp, g.world)) {
			top = std::fmin(top, g.world.mn[2] - 512.0f);
			char b[200];
			std::snprintf(b, sizeof b, "map extent (%.0f %.0f %.0f)..(%.0f %.0f %.0f), the terrain box (%.0f %.0f %.0f)..(%.0f %.0f %.0f) is cut at z %.0f", g.world.mn[0], g.world.mn[1], g.world.mn[2], g.world.mx[0],
			              g.world.mx[1], g.world.mx[2], bmn.x, bmn.y, bmn.z, bmx.x, bmx.y, bmx.z, top);
			g.note = b;
		} else {
			top = std::fmin(top, -5632.0f);
			g.note = "no readable BSP: the terrain box is cut at z -5632";
		}
		g.centre[2] = top;
		g.ok = top - bmn.z >= 6000.0f;  // the cliff's anchor sits 2960 u above the box floor and its face rises 900 u: room for them and the sky above
		console->Print("[bridge] stage: terrain session: %s%s\n", g.note.c_str(), g.ok ? "" : " (no room: no portal onto Minecraft in this map)");
	}
	if (!g.ok) return false;
	StageMap st;
	st.mn = bmn;
	st.mx = Vector(bmx.x, bmx.y, g.centre[2]);
	st.d = Vector(0, 0, 0);
	st.yMin = 1e8;  // (no stage heights: nothing maps by offset)
	st.on = true;
	g_map.stage = st;
	g.active = true;
	g.terrain = true;
	ClipTerrainWindowBegin(st.mn, st.mx);
	StagePortalCulling(true);
	console->Print("[bridge] stage: ON (%s): the terrain session's region (%.0f %.0f %.0f)..(%.0f %.0f %.0f) is this map's Minecraft region, the pinned cliff its exit\n", why, st.mn.x, st.mn.y, st.mn.z, st.mx.x, st.mx.y, st.mx.z);
	return true;
}

// (bridge #78) THE UNIFIED WORLD'S WINDOW. One Minecraft world for every session: the generated terrain world (the seed of world.conf). Every chamber is in
// its band (Minecraft y >= 320, above anything the generator makes, unified.cpp), and the portal onto Minecraft opens onto the real terrain round the
// pinned cliff, the same home for every chamber, shown in the biggest empty slab of Portal 2's world under (or over) the map: the pinned cliff at the
// slab's x/y centre, Minecraft y 48 on its floor (the lowest section the host is sent), the slab's top cut where it maps to y 320. The mapping is the
// stage's (a Source offset for the terrain), only the range test differs: terrain is y < 320 (coords.hpp StageMap::yMax). The exit is the pinned
// cliff patch (as in pit flings), the floating origin moves the offset (endless terrain), the mod is asked for no stage.
static bool UnifiedWindowEnsure(const char *why) {
	if (!g.computed) {
		g.computed = true;
		const char *bsp = MapIndexBsp();
		float mn[3] = {0, 0, 0}, mx[3] = {0, 0, 0};
		bool above = false;
		if (!bsp || !*bsp) {
			g.note = "no BSP file found for this map";
		} else if (!stagecalc::BspExtent(bsp, g.world)) {
			g.note = "the BSP is not readable";
		} else if (stagecalc::ChooseSlab(g.world, 16128.0f, 512.0f, 2.0f * kStageHalfZ, (320.0f - 48.0f) * kUnitsPerBlock, mn, mx, above)) {
			g.ok = true;
			char b[240];
			std::snprintf(b, sizeof b, "map extent (%.0f %.0f %.0f)..(%.0f %.0f %.0f): the window is the slab %s it, (%.0f %.0f %.0f)..(%.0f %.0f %.0f)", g.world.mn[0], g.world.mn[1], g.world.mn[2], g.world.mx[0],
			              g.world.mx[1], g.world.mx[2], above ? "over" : "under", mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
			g.note = b;
		} else {
			const float half[3] = {kStageHalfX, kStageHalfY, kStageHalfZ};
			g.ok = stagecalc::ChooseRegion(g.world, half, 16300.0f, g.centre, g.gap) && g.gap >= 1500.0f;
			for (int i = 0; i < 3; ++i) mn[i] = g.centre[i] - half[i], mx[i] = g.centre[i] + half[i];
			char b[240];
			std::snprintf(b, sizeof b, "map extent (%.0f %.0f %.0f)..(%.0f %.0f %.0f) leaves no slab: the window is the empty box centred at (%.0f %.0f %.0f), %.0f u clear of it", g.world.mn[0], g.world.mn[1], g.world.mn[2],
			              g.world.mx[0], g.world.mx[1], g.world.mx[2], g.centre[0], g.centre[1], g.centre[2], g.gap);
			g.note = b;
		}
		g.bmn = Vector(mn[0], mn[1], mn[2]), g.bmx = Vector(mx[0], mx[1], mx[2]);
		console->Print("[bridge] world: %s%s\n", g.note.c_str(), g.ok ? "" : " (no room: no portal onto Minecraft in this map)");
	}
	if (!g.ok) return false;
	double cx, cy, cz;
	int fx, fy, fz, dx, dz;
	ClipCliffMc(cx, cy, cz, fx, fy, fz, dx, dz);
	const double floorY = 48.0;
	StageMap st;
	st.mn = g.bmn;
	st.mx = g.bmx;
	st.mx.z = std::fmin(st.mx.z, st.mn.z + (float)((320.0 - floorY) * kUnitsPerBlock));  // (its top maps to y 320 at most: above it is the chamber band)
	// the plain (chamber) mapping of the cliff's column at the floor height, and where it goes: the box's x/y centre, its floor
	const Vector plain(g_map.src.x + (float)((cx - g_map.mcX) * kUnitsPerBlock), g_map.src.y + (float)(-(cz - g_map.mcZ) * kUnitsPerBlock), g_map.src.z + (float)((floorY - g_map.mcY) * kUnitsPerBlock));
	const Vector want((st.mn.x + st.mx.x) * 0.5f, (st.mn.y + st.mx.y) * 0.5f, st.mn.z);
	st.d = want - plain;
	st.yMin = -1e30;
	st.yMax = 320.0;
	st.on = true;
	g_map.stage = st;
	g.active = true;
	g.terrain = true;  // (the mod is asked for no stage)
	g.unified = true;
	TerrainSetMapReady(true);
	ClipUnifiedWindowBegin(st.mn, st.mx);
	StagePortalCulling(true);
	console->Print("[bridge] stage: ON (%s): the unified world's window: the terrain round the pinned cliff (MC %.0f %.0f %.0f) is shown in (%.0f %.0f %.0f)..(%.0f %.0f %.0f), offset (%.0f %.0f %.0f); the chamber stays in the band\n", why, cx, cy, cz,
	               st.mn.x, st.mn.y, st.mn.z, st.mx.x, st.mx.y, st.mx.z, st.d.x, st.d.y, st.d.z);
	return true;
}

bool StageEnsure(const char *why) {
	if (g.active) return true;
	if (!g_enabled) return false;
	if (ClipHasRegion()) return false;  // pit flings and the testbed have their own (real) region
	if (!link::McAlive()) {
		if (!g.noLink) console->Print("[bridge] stage: Minecraft is not linked: the portal onto Minecraft needs it (%s)\n", why), g.noLink = true;
		return false;
	}
	g.noLink = false;
	if (WorldUnifiedChamber()) return UnifiedWindowEnsure(why);  // (bridge #78) the unified world: the real terrain round the pinned cliff
	if (McTerrainModeNow() && ClipTerrainCliffStreamed()) return TerrainWindowEnsure(why);  // (bridge #61) a terrain session: Minecraft's world has real terrain, not the stage
	if (!g.computed) {
		g.computed = true;
		const char *bsp = MapIndexBsp();
		if (!bsp || !*bsp) {
			g.note = "no BSP file found for this map";
		} else if (!stagecalc::BspExtent(bsp, g.world)) {
			g.note = "the BSP is not readable";
		} else {
			const float half[3] = {kStageHalfX, kStageHalfY, kStageHalfZ};
			g.ok = stagecalc::ChooseRegion(g.world, half, 16300.0f, g.centre, g.gap) && g.gap >= 1500.0f;
			char b[200];
			std::snprintf(b, sizeof b, "map extent (%.0f %.0f %.0f)..(%.0f %.0f %.0f), the empty box is centred at (%.0f %.0f %.0f), %.0f u clear of it", g.world.mn[0], g.world.mn[1], g.world.mn[2], g.world.mx[0],
			              g.world.mx[1], g.world.mx[2], g.centre[0], g.centre[1], g.centre[2], g.gap);
			g.note = b;
		}
		console->Print("[bridge] stage: %s%s\n", g.note.c_str(), g.ok ? "" : " (no room: no portal onto Minecraft in this map)");
	}
	if (!g.ok) return false;
	// the window: the cliff wall nearest the body (walls at x = k * 256) and the 24-block run of z the body is in
	const Vector mc = SrcToMc(PlayerOrigin());
	g.wallX = (int)std::lround(mc.x / kStagePeriod) * kStagePeriod;
	g.zc = (int)std::floor(mc.z / kStageRun) * kStageRun + kStageRun / 2 - 1;
	g.wcx = g.wallX - 32.0, g.wcy = kStageY0 + 60.0, g.wcz = g.zc + 1.0;
	const Vector plain = McToSrc(g.wcx, g.wcy, g.wcz);  // (the stage is off: the plain mapping)
	const Vector c(g.centre[0], g.centre[1], g.centre[2]);
	StageMap st;
	st.mn = c - Vector(kStageHalfX, kStageHalfY, kStageHalfZ);
	st.mx = c + Vector(kStageHalfX, kStageHalfY, kStageHalfZ);
	st.d = c - plain;
	st.yMin = kStageY0 - 40.0;
	st.on = true;
	g_map.stage = st;
	g.active = true;
	TerrainSetMapReady(true);  // (the mirror world's streamed sections are taken in from the first use of the stage: its cliff must be found in them)
	ClipStageBegin(st.mn, st.mx, g.wallX, g.zc);
	StagePortalCulling(true);
	console->Print("[bridge] stage: ON (%s): the window around the cliff wall at Minecraft x %d, z run %d..%d is shown in the empty box (%.0f %.0f %.0f)..(%.0f %.0f %.0f) of Portal 2's world, offset (%.0f %.0f %.0f)\n",
	               why, g.wallX, g.zc - 11, g.zc + 12, st.mn.x, st.mn.y, st.mn.z, st.mx.x, st.mx.y, st.mx.z, st.d.x, st.d.y, st.d.z);
	return true;
}

// ---- commands ------------------------------------------------------------------------------------------------------------------
CON_COMMAND(pc_mc_stage, "pc_mc_stage [0|1] - the portal onto Minecraft in campaign chambers (the Minecraft stage in an empty box of the map's world); 0 turns it off for the next level (default 1)\n") {
	if (args.ArgC() > 1) g_enabled = std::atoi(args[1]) != 0;
	console->Print("[bridge] stage: %s, %s\n", g_enabled ? "enabled" : "disabled", StageActive() ? "ON for this level" : "not on yet (made when G is armed)");
}

CON_COMMAND(pcb_stage, "pcb_stage [on] - the virtual region's state (and, with 'on', turn it on now: tests)\n") {
	if (args.ArgC() > 1 && !std::strcmp(args[1], "on")) StageEnsure("pcb_stage on");
	const StageMap &st = g_map.stage;
	console->Print("[bridge] stage: %s, enabled %d, computed %d, room %d; box (%.0f %.0f %.0f)..(%.0f %.0f %.0f) offset (%.0f %.0f %.0f); window wall x %d, z run centre %d; %s\n", g.active ? "ON" : "off", g_enabled, g.computed, g.ok,
	               st.mn.x, st.mn.y, st.mn.z, st.mx.x, st.mx.y, st.mx.z, st.d.x, st.d.y, st.d.z, g.wallX, g.zc, g.note.c_str());
}

CON_COMMAND(pcb_assert_stage, "pcb_assert_stage <1|0> - the virtual region is on (1) or off (0)\n") {
	const bool want = args.ArgC() > 1 && std::atoi(args[1]) != 0;
	char b[200];
	std::snprintf(b, sizeof b, "stage %s (want %s); %s", g.active ? "on" : "off", want ? "on" : "off", g.note.c_str());
	TestCheck(g.active == want, "virtual region", b);
}


// (tests) Turn Chell towards the nearest wall that takes a portal, so a script can fire at a campaign chamber without a number typed for it: rays all round at
// eye level; a hit counts when the surface is upright, its material name says WHITE, it is at least 110 u away, and a 48 x 48 u patch round the hit is flat (the
// same plane at the four corners), the size a portal needs. The nearest such wall.
CON_COMMAND(pcb_aim_portalable, "pcb_aim_portalable [range=700] - (tests) turn towards the nearest upright white wall with a flat 48 x 48 u patch (a portal fits)\n") {
	const float range = args.ArgC() > 1 ? (float)std::atof(args[1]) : 700.0f;
	const Vector eye = PlayerOrigin() + Vector(0, 0, 60);
	float best = 1e9f, byaw = 0;
	const char *bname = "";
	int hits = 0, upright = 0, white = 0, far = 0;
	std::string seen;
	auto trace = [&](const Vector &from, const Vector &dir, CGameTrace &tr) {
		return engine.Trace(from, from + dir * range, Vector(0, 0, 0), MASK_SOLID, tr, true) && tr.fraction < 1.0f && !tr.startsolid;
	};
	for (int yaw = 0; yaw < 360; yaw += 3) {
		const float y = yaw * (float)M_PI / 180;
		const Vector dir(std::cos(y), std::sin(y), 0);
		CGameTrace tr;
		if (!trace(eye, dir, tr) || !tr.surface.name) continue;
		++hits;
		if (std::fabs(tr.plane.normal.z) > 0.1f) continue;
		++upright;
		std::string n = tr.surface.name;
		for (char &c : n) c = (char)std::toupper((unsigned char)c);
		const float d = tr.fraction * range;
		if (seen.size() < 300 && seen.find(tr.surface.name) == std::string::npos) seen += std::string(tr.surface.name) + " ";
		if (n.find("WHITE") == std::string::npos) continue;
		++white;
		if (d < 110.0f) continue;
		++far;
		if (d >= best) continue;
		const Vector side(-dir.y, dir.x, 0);
		bool flat = true;
		for (int k = 0; k < 4 && flat; ++k) {
			const Vector from = eye + side * ((k & 1) ? 24.0f : -24.0f) + Vector(0, 0, (k & 2) ? 24.0f : -24.0f);
			CGameTrace t2;
			flat = trace(from, dir, t2) && std::fabs(t2.plane.normal.Dot(tr.plane.normal) - 1.0f) < 0.01f && std::fabs(t2.fraction * range - d) < 2.0f;
		}
		if (flat) best = d, byaw = (float)yaw, bname = tr.surface.name;
	}
	if (best > 1e8f) return console->Print("[bridge] aim: no flat white wall within %.0f u (%d rays hit, %d upright, %d white, %d of those 110+ u away; materials: %s)\n", range, hits, upright, white, far, seen.c_str());
	engine.SetAngles(QAngle{0, byaw, 0});
	console->Print("[bridge] aim: a flat white wall %.0f u away (%s): yaw %.0f\n", best, bname, byaw);
}

}  // namespace bridge
