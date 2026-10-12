// Portalcraft bridge: endless terrain through a floating origin.
//
// The Minecraft region is a fixed box in the Portal 2 map (about +-400 blocks across at 40 u per block, and
// Portal 2 cannot work past +-16384 u). Minecraft is endless, so when Chell walks far from the box's centre the
// region<->Minecraft mapping is moved by whole chunks, and everything of Portal 2's inside the region moves by
// the same Source offset d in the same server tick, so nothing changes where it is in Minecraft:
//   * the mapping (g_map: the Minecraft anchor moves by -d, the Source anchor stays);
//   * Chell (SnapPlayer + her velocity kept: PuppetLaunch);
//   * both portals when they are in the region (NewLocation; one that would leave the box is fizzled);
//   * physics props in the region (cubes, turrets: a point_teleport each, velocity kept);
//   * the composite's streams and requests (camera continuity, no new camera), the light probes, the prop
//     collider pool (re-placed next tick), pending gun placements, the cliff exit (recomputed; unavailable
//     while it is outside the box), Minecraft's collision epoch for the chamber.
// Audio, the Chell proxy, fluids, block collision and the terrain solids work in Minecraft coordinates (or
// convert every frame) and follow the mapping by themselves.
//
// When: Chell is in the region, more than pc_mc_float_dist (default 200 blocks) from its centre horizontally,
// not within 3 blocks of a portal, not in noclip-free fall through a portal (no teleport in the last 10 ticks),
// and the last shift was at least 2 s ago. The shift is the nearest whole-chunk offset that brings her back to
// the centre. Back in the chamber with no portal open in the region, the mapping returns to its anchor (so the
// pit redirect has its cliff exit again).
#include "bridge.hpp"
#include "growth.hpp"
#include "coords.hpp"
#include "stage.hpp"

#include "Modules/Tier1.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace bridge {

bool ClipHasRegion();
bool ClipRegionContains(const Vector &p);
Vector ClipRegionCentre();
void ClipRegionBounds(Vector &mn, Vector &mx);
void ClipShift(const Vector &d);
void CompositeShift(const Vector &d);
void CompositeShiftTraceArm(int frames);
void McLightShift(const Vector &d);
void PropCollideShift(const Vector &d);
void PortalGunShift(const Vector &d);
void PuppetLaunch(const Vector &vel);
void PuppetShiftOrigin(const Vector &d);
bool PlayerNoclip();
bool McModeActive();  // mc.cpp
bool EntSetKV(void *ent, const char *key, const char *value);
void TerrainClearMeshes();
void CollisionFieldLevelStart();
void ClipExitRecheck(const char *why);
void TestCheck(bool ok, const char *what, const char *detail);

bool FloatAtAnchor();
static bool g_enabled = true;          // pc_mc_float (on by default since bridge #10: gate float4)
static float g_dist = 200 * kUnitsPerBlock;  // pc_mc_float_dist (blocks)
static double g_anchorMcX = 0, g_anchorMcY = 0, g_anchorMcZ = 0;
static Vector g_anchorD;  // (bridge #78) the unified window's offset at the anchor
// (bridge #78) The unified world's window floats by its own offset (g_map.stage.d): the chamber's mapping (the band) never moves.
static bool FloatsWindow() { return StageUnified(); }
// How far the terrain has moved in Portal 2 since the anchor (Source units).
static Vector DriftNow() {
	if (FloatsWindow()) return g_map.stage.d - g_anchorD;
	return Vector((float)((g_anchorMcX - g_map.mcX) * kUnitsPerBlock), (float)((g_map.mcZ - g_anchorMcZ) * kUnitsPerBlock), (float)((g_anchorMcY - g_map.mcY) * kUnitsPerBlock));
}
static float g_marginV = 16 * kUnitsPerBlock;  // pc_mc_float_v: shift vertically when Chell's feet come this close to the box's top or bottom
static bool g_haveAnchor = false;
static int g_tick = 0, g_lastShiftTick = -100000, g_lastTeleportTick = -100000;
static Vector g_lastFeet;
static int g_shifts = 0, g_returns = 0, g_fizzled = 0, g_propsMoved = 0;
static Vector g_lastD;
static int g_tpSerial = 0;
static int g_tripSkip = 0;  // ticks whose step is not travel (a shift: PlayerOrigin() is the pre-shift origin until the next move)


static const char *kPropClasses[] = {"prop_weighted_cube", "prop_physics", "prop_physics_override", "prop_physics_multiplayer", "npc_portal_turret_floor", "prop_monster_box"};

static std::string V3(const Vector &v) {
	char b[96];
	std::snprintf(b, sizeof b, "%.3f %.3f %.3f", v.x, v.y, v.z);
	return b;
}

// Move an entity to a spot with Portal 2's own teleport (point_teleport TeleportToCurrentPos: the
// entity's Teleport(origin, angles, no velocity), so a physics prop keeps its motion and the client does not
// interpolate across it). (Its "Teleport" input uses the origin saved in Activate(), which a point_teleport
// spawned at run time never gets: everything it moved went to (0 0 0), the cube floats 1/2 lost.)
static void *g_tp = nullptr;  // one point_teleport, reused (re-made after a level change)
bool TeleportTo(const std::string &target, const Vector &o, const QAngle &a) {
	char ang[96];
	std::snprintf(ang, sizeof ang, "%.3f %.3f %.3f", a.x, a.y, a.z);
	if (!g_tp) g_tp = Spawn("point_teleport", {{"targetname", "pcb_fo_tp"}, {"target", target}, {"origin", V3(o)}, {"angles", ang}});
	if (!g_tp) return false;
	EntSetKV(g_tp, "target", target.c_str());
	EntSetKV(g_tp, "origin", V3(o).c_str());
	EntSetKV(g_tp, "angles", ang);
	return EntInput(g_tp, "TeleportToCurrentPos");
}
// (also propcollide.cpp's push-out) An entity to a spot through point_teleport, named first if it has no name.
bool EntTeleport(void *ent, const Vector &pos, const QAngle &ang) {
	std::string name = EntKV(ent, "targetname");
	if (name.empty()) {
		char b[40];
		std::snprintf(b, sizeof b, "pcb_fo_%d", ++g_tpSerial);
		name = b;
		EntSetKV(ent, "targetname", b);
	}
	return TeleportTo(name, pos, ang);
}
static bool TeleportBy(void *ent, const Vector &d) {
	std::string name = EntKV(ent, "targetname");
	if (name.empty()) {
		char b[40];
		std::snprintf(b, sizeof b, "pcb_fo_%d", ++g_tpSerial);
		name = b;
		EntSetKV(ent, "targetname", b);
	}
	Vector o = EntVec(ent, "origin"), a = EntVec(ent, "angles");
	bool ok = TeleportTo(name, o + d, QAngle{a.x, a.y, a.z});
	Vector now = EntVec(ent, "origin");
	if (!ok || (now - (o + d)).Length() > 1.0f) {
		console->Print("[bridge] floating origin: prop %s not moved (input %d): at (%.1f %.1f %.1f), wanted (%.1f %.1f %.1f)\n", name.c_str(), ok, now.x, now.y, now.z, o.x + d.x, o.y + d.y, o.z + d.z);
		return false;
	}
	return true;
}

// Portals a shift would carry out of the region box: the Portal 2 entity is closed and the portal is kept in
// Minecraft coordinates; when a later shift brings that spot back inside the box it is placed again there,
// same entity (colour and linkage), same angles. Chell's gun firing that colour meanwhile drops the record.
struct Parked {
	bool on = false;
	void *ent = nullptr;
	double mc[3] = {0, 0, 0};
	QAngle ang{0, 0, 0};
	int since = 0;
};
static Parked g_parked[2];
static int g_parkedN = 0, g_restoredN = 0, g_parkDropped = 0;
static int g_tpMode = 1;    // pcb_float_tpmode: Chell moved by 0 SnapPlayer, 1 point_teleport (TeleportToCurrentPos)
static int g_eyeLog = 0;    // client frames to log after a shift (diagnostics)
static int g_traceN = 0;    // pcb_float_trace: frames of the eye / composite trace after each shift
static uint64_t g_viewFrame = 0, g_shiftViewFrame = 0;
static Vector g_eyePrev;

// Physics props a shift would carry out of the box: frozen (DisableMotion) at a hold spot deep under the
// terrain inside the box, in its far corner (>= 330 blocks from anywhere Chell can be, past the 128-block collision
// export: no colliders or push-out there, though deep sections now export collision; drawn behind rock), and put
// back at their Minecraft spot, motion enabled, when it is inside the box again.
struct ParkedProp {
	std::string name;
	double mc[3];
	QAngle ang;
};
static std::vector<ParkedProp> g_parkedProps;
PCB_GAUGE("floatorigin.parked", g_parkedProps.size());
static int g_propsParked = 0, g_propsRestored = 0;
static Vector HoldSpot(int k) {
	Vector v = ClipRegionCentre() + Vector(15000.0f - 120.0f * (k % 20), 15000.0f - 120.0f * (k / 20), -5000.0f);
	if (StageUnified()) {  // (bridge #78) the unified window is a slab that need not be centred on Portal 2's world: its far corner, near its floor
		Vector mn, mx;
		ClipRegionBounds(mn, mx);
		v = Vector(mx.x - 400.0f - 120.0f * (k % 20), mx.y - 400.0f - 120.0f * (k / 20), mn.z + 200.0f);
	}
	return v;
}
static bool PropParked(void *e) {
	const std::string n = EntKV(e, "targetname");
	for (auto &q : g_parkedProps)
		if (q.name == n) return true;
	return false;
}
static void ParkedPropsTick() {
	for (size_t k = 0; k < g_parkedProps.size();) {
		ParkedProp &q = g_parkedProps[k];
		const Vector at = McToSrc(q.mc[0], q.mc[1], q.mc[2]);
		void *e = nullptr;
		for (const char *cls : kPropClasses)
			for (void *x : EntFind(cls, q.name.c_str())) e = x;
		if (!e) {
			console->Print("[bridge] floating origin: parked prop %s is gone\n", q.name.c_str());
			g_parkedProps.erase(g_parkedProps.begin() + k);
			continue;
		}
		if (!ClipRegionContains(at)) {
			++k;
			continue;
		}
		bool ok = TeleportTo(q.name, at, q.ang);
		EntInput(e, "EnableMotion");
		EntInput(e, "Wake");
		++g_propsRestored;
		console->Print("[bridge] floating origin: prop %s put back at its Minecraft spot (%.2f %.2f %.2f) (teleport %d)\n", q.name.c_str(), q.mc[0], q.mc[1], q.mc[2], ok);
		g_parkedProps.erase(g_parkedProps.begin() + k);
	}
}

static bool PortalAlive(void *e) {
	if (!e) return false;
	for (void *p : EntFind("prop_portal"))
		if (p == e) return true;
	return false;
}
static bool PortalActive(void *e) { return EntKV(e, "Activated") == "1"; }

// Every tick: parked portals whose Minecraft spot is back inside the box are placed again.
static void ParkedTick() {
	for (int i = 0; i < 2; ++i) {
		Parked &k = g_parked[i];
		if (!k.on) continue;
		if (!PortalAlive(k.ent)) {
			k.on = false, ++g_parkDropped;
			console->Print("[bridge] floating origin: parked portal %d dropped (its entity is gone)\n", i);
			continue;
		}
		if (PortalActive(k.ent) && g_tick - k.since > 2) {
			// placed again by Portal 2 (the gun fired this colour): the new placement wins
			k.on = false, ++g_parkDropped;
			console->Print("[bridge] floating origin: parked portal %d dropped (placed again by Portal 2)\n", i);
			continue;
		}
		const Vector at = McToSrc(k.mc[0], k.mc[1], k.mc[2]);
		if (!ClipRegionContains(at)) continue;
		char buf[160];
		std::snprintf(buf, sizeof buf, "%.2f %.2f %.2f %.1f %.1f %.1f", at.x, at.y, at.z, k.ang.x, k.ang.y, k.ang.z);
		bool a = EntInput(k.ent, "SetActivatedState", "1");
		bool b = EntInput(k.ent, "NewLocation", buf);
		g_portalPos[i] = at, g_portalSet[i] = true;
		k.on = false, ++g_restoredN;
		console->Print("[bridge] floating origin: portal %d placed again at its Minecraft spot (%.2f %.2f %.2f) = Portal 2 (%s) (inputs %d %d)\n", i, k.mc[0], k.mc[1], k.mc[2], buf, a, b);
	}
}

static void Shift(const Vector &d, const char *why) {
	// 1. the mapping: the same Minecraft place appears d further along in Portal 2
	if (FloatsWindow()) {
		g_map.stage.d = g_map.stage.d + d;  // (bridge #78) only the terrain's offset: the chamber band stays
	} else {
		g_map.mcX -= d.x / kUnitsPerBlock;
		g_map.mcY -= d.z / kUnitsPerBlock;
		g_map.mcZ += d.y / kUnitsPerBlock;
	}
	// 2. Chell, keeping her velocity
	const Vector feet = PlayerOrigin(), vel = PlayerVelocity();
	const bool chellIn = ClipRegionContains(feet);
	if (chellIn) {
		const QAngle va = engine.ViewAngles();
		if (g_tpMode != 1 || !TeleportTo("!player", feet + d, va)) SnapPlayer(feet + d, va);
		PuppetLaunch(vel);
		PuppetShiftOrigin(d);  // (PlayerOrigin() users this tick and next frame: the hidden player, the trip meter)
	}
	// 3. the portals in the region
	int fizz = 0, moved = 0;
	for (void *p : EntFind("prop_portal")) {
		Vector o = EntVec(p, "origin"), a = EntVec(p, "angles");
		if (!ClipRegionContains(o)) continue;
		int two = EntKV(p, "PortalTwo") == "1";
		// a closed portal stays where it is (NewLocation would open it again), a parked one too
		if (!PortalActive(p) || (g_parked[two].on && g_parked[two].ent == p)) continue;
		if (!ClipRegionContains(o + d)) {
			// out of the box: closed in Portal 2, kept in Minecraft coordinates (ParkedTick places it again)
			Parked &k = g_parked[two];
			k.on = true, k.ent = p, k.ang = QAngle{a.x, a.y, a.z}, k.since = g_tick;
			SrcToMcD(o + d, k.mc[0], k.mc[1], k.mc[2]);  // (the new mapping: o + d is the same Minecraft spot)
			EntInput(p, "Fizzle");
			++fizz, ++g_parkedN;
			g_portalSet[two] = false;
			console->Print("[bridge] floating origin: portal %d parked at its Minecraft spot (%.2f %.2f %.2f)\n", two, k.mc[0], k.mc[1], k.mc[2]);
			continue;
		}
		char buf[160];
		std::snprintf(buf, sizeof buf, "%.2f %.2f %.2f %.1f %.1f %.1f", o.x + d.x, o.y + d.y, o.z + d.z, a.x, a.y, a.z);
		EntInput(p, "NewLocation", buf);
		g_portalPos[two] = o + d;  // this frame's composite and the collision hole see it at once
		++moved;
	}
	// 4. physics props in the region
	int props = 0, propsParked = 0;
	for (const char *cls : kPropClasses)
		for (void *e : EntFind(cls)) {
			const Vector o = EntVec(e, "origin");
			if (!ClipRegionContains(o) || PropParked(e)) continue;
			if (ClipRegionContains(o + d)) {
				if (TeleportBy(e, d)) ++props;
				continue;
			}
			std::string name = EntKV(e, "targetname");
			if (name.empty()) {
				char b[40];
				std::snprintf(b, sizeof b, "pcb_fo_%d", ++g_tpSerial);
				name = b;
				EntSetKV(e, "targetname", b);
			}
			ParkedProp q;
			q.name = name;
			const Vector a = EntVec(e, "angles");
			q.ang = QAngle{a.x, a.y, a.z};
			SrcToMcD(o + d, q.mc[0], q.mc[1], q.mc[2]);
			EntInput(e, "DisableMotion");
			TeleportTo(name, HoldSpot((int)g_parkedProps.size()), q.ang);
			g_parkedProps.push_back(q);
			++propsParked, ++g_propsParked;
			console->Print("[bridge] floating origin: prop %s parked at its Minecraft spot (%.2f %.2f %.2f)\n", name.c_str(), q.mc[0], q.mc[1], q.mc[2]);
		}
	// 5. everything that keeps Source positions of region things
	CompositeShift(d);
	McLightShift(d);
	PropCollideShift(d);
	PortalGunShift(d);
	ClipShift(d);
	TerrainClearMeshes();        // (debug meshes are built in Source space)
	if (!FloatsWindow()) CollisionFieldLevelStart();  // Minecraft drops the chamber voxels exported through the old mapping (bridge #78: the unified band does not move)
	g_lastShiftTick = g_tick, g_lastD = d, g_fizzled += fizz, g_propsMoved += props;
	g_shiftViewFrame = g_viewFrame, g_eyeLog = g_traceN, g_tripSkip = 2;
	CompositeShiftTraceArm(g_traceN);
	console->Print("[bridge] floating origin (%s): shifted by (%.0f %.0f %.0f) u = %d x %d chunks, %d sections up; Minecraft anchor now (%.1f %.1f %.1f); Chell %s (%.0f %.0f %.0f) -> (%.0f %.0f %.0f), "
	               "%d portals moved, %d parked, %d props moved, %d parked\n",
	               why, d.x, d.y, d.z, (int)std::lround(-d.x / (16 * kUnitsPerBlock)), (int)std::lround(d.y / (16 * kUnitsPerBlock)), (int)std::lround(-d.z / (16 * kUnitsPerBlock)), g_map.mcX,
	               g_map.mcY, g_map.mcZ, chellIn ? "moved" : "stays", feet.x, feet.y, feet.z, chellIn ? feet.x + d.x : feet.x, chellIn ? feet.y + d.y : feet.y, chellIn ? feet.z + d.z : feet.z, moved, fizz, props, propsParked);
	ClipExitRecheck("floating origin");
}

static Vector ChunkSnap(float x, float y) {
	const float c = 16 * kUnitsPerBlock;
	return Vector(-std::round(x / c) * c, -std::round(y / c) * c, 0);
}

// pcb_float_trip: a long trip through the shifts (tests). Every tick: Chell inside the region box (she must
// never leave it: past it Portal 2 shows nothing), her largest offset from the box's centre, and the
// distance she has covered in Minecraft (feet in Minecraft coordinates, so the shifts do not count).
static bool g_tripOn = false;
static int g_tripTicks = 0, g_tripOutside = 0, g_tripShifts0 = 0, g_tripFizzled0 = 0;
static double g_tripMc = 0, g_tripMaxOff = 0, g_tripLast[3] = {0, 0, 0}, g_tripStart[3] = {0, 0, 0};
static bool g_tripHaveLast = false;
static void TripTick() {
	if (!g_tripOn) return;
	const Vector feet = PlayerOrigin();
	++g_tripTicks;
	if (!ClipRegionContains(feet)) ++g_tripOutside;
	const Vector c = ClipRegionCentre();
	g_tripMaxOff = std::fmax(g_tripMaxOff, std::fmax(std::fabs(feet.x - c.x), std::fabs(feet.y - c.y)) / kUnitsPerBlock);
	double m[3];
	SrcToMcD(feet, m[0], m[1], m[2]);
	if (g_tripSkip > 0) {
		--g_tripSkip;
		return;  // (the last position stays the pre-shift one, in its own mapping)
	}
	if (g_tripHaveLast) {
		const double dx = m[0] - g_tripLast[0], dz = m[2] - g_tripLast[2], step = std::sqrt(dx * dx + dz * dz);
		if (step < 50) g_tripMc += step;  // (a portal transit is not travel)
	} else {
		for (int i = 0; i < 3; ++i) g_tripStart[i] = m[i];
	}
	for (int i = 0; i < 3; ++i) g_tripLast[i] = m[i];
	g_tripHaveLast = true;
}
double FloatTripNet() {
	if (!g_tripOn || !g_tripHaveLast) return 0;
	return std::sqrt((g_tripLast[0] - g_tripStart[0]) * (g_tripLast[0] - g_tripStart[0]) + (g_tripLast[2] - g_tripStart[2]) * (g_tripLast[2] - g_tripStart[2]));
}
CON_COMMAND(pcb_float_trip, "pcb_float_trip start | assert <min_blocks> - a long trip through the floating origin: Chell never outside the region box, Minecraft distance covered (tests)\n") {
	if (args.ArgC() > 1 && !std::strcmp(args[1], "start")) {
		g_tripOn = true, g_tripTicks = g_tripOutside = 0, g_tripMc = g_tripMaxOff = 0, g_tripHaveLast = false, g_tripSkip = 0, g_tripShifts0 = g_shifts, g_tripFizzled0 = g_fizzled;
		return console->Print("[bridge] float trip: started\n");
	}
	const double want = args.ArgC() > 2 ? std::atof(args[2]) : 1500;
	g_tripOn = false;
	const double net = std::sqrt((g_tripLast[0] - g_tripStart[0]) * (g_tripLast[0] - g_tripStart[0]) + (g_tripLast[2] - g_tripStart[2]) * (g_tripLast[2] - g_tripStart[2]));
	char buf[400];
	std::snprintf(buf, sizeof buf, "%.0f blocks covered in Minecraft (net %.0f, from MC %.0f %.0f to %.0f %.0f) over %d ticks, %d shifts, %d ticks outside the region box, largest offset from its centre %.0f blocks, %d portals parked leaving the box",
	              g_tripMc, net, g_tripStart[0], g_tripStart[2], g_tripLast[0], g_tripLast[2], g_tripTicks, g_shifts - g_tripShifts0, g_tripOutside, g_tripMaxOff, g_fizzled - g_tripFizzled0);
	TestCheck(net >= want && g_tripOutside == 0 && g_shifts > g_tripShifts0, "long trip through the floating origin", buf);
}

// Every server tick (plugin.cpp, after the clip rules).
void FloatOriginTick() {
	++g_tick;
	if (!ClipHasRegion() || (StageActive() && !StageUnified())) return;  // (bridge #47: the virtual region is a fixed box in the stage: the mapping never floats; bridge #78: the unified window does)
	TripTick();
	if (!g_haveAnchor) g_anchorMcX = g_map.mcX, g_anchorMcY = g_map.mcY, g_anchorMcZ = g_map.mcZ, g_anchorD = g_map.stage.d, g_haveAnchor = true;
	const Vector feet = PlayerOrigin();
	if ((feet - g_lastFeet).Length() > 100.0f) g_lastTeleportTick = g_tick;  // a portal transit or a setpos
	g_lastFeet = feet;
	ParkedTick();
	ParkedPropsTick();
	if (!g_enabled || g_tick - g_lastShiftTick < 120 || g_tick - g_lastTeleportTick < 10) return;
	// the Minecraft player (bridge #13, DESIGN-mcplayer 6): no shifts while it leads; Minecraft's coordinates are the
	// truth then, and the region box gives it +-200 blocks around wherever the mapping stood at the switch
	if (McModeActive()) return;
	const Vector c = ClipRegionCentre();
	if (ClipRegionContains(feet)) {
		// horizontally: past pc_mc_float_dist from the centre, by whole chunks back to it; vertically: within
		// pc_mc_float_v of the box's top or bottom, by whole sections back to its middle height
		Vector mn, mx;
		ClipRegionBounds(mn, mx);
		const float ox = feet.x - c.x, oy = feet.y - c.y, oz = feet.z - c.z;
		const float dist = FloatsWindow() ? std::fmin(g_dist, 0.6f * std::fmin(mx.x - mn.x, mx.y - mn.y) * 0.5f) : g_dist;  // (bridge #78: a smaller window recentres sooner)
		const bool needH = std::fmax(std::fabs(ox), std::fabs(oy)) >= dist;
		const bool needV = !FloatsWindow() && (feet.z < mn.z + g_marginV || feet.z + 72.0f > mx.z - g_marginV);  // (bridge #78: the unified window holds y 48..320, no vertical shifts)
		if (!needH && !needV) return;
		for (int i = 0; i < 2; ++i)
			if (g_portalSet[i] && (g_portalPos[i] - (feet + Vector(0, 0, 36))).Length() < 3 * kUnitsPerBlock) return;
		Vector d = needH ? ChunkSnap(ox, oy) : Vector(0, 0, 0);
		if (needV) d.z = -std::round(oz / (16 * kUnitsPerBlock)) * (16 * kUnitsPerBlock);
		++g_shifts;
		Shift(d, needV ? (needH ? "Chell far from the region's centre and near its top or bottom" : "Chell near the region's top or bottom") : "Chell far from the region's centre");
		return;
	}
	// Chell in the chamber: bring the mapping home once nothing open is in the region
	if (FloatAtAnchor()) return;
	for (int i = 0; i < 2; ++i)
		if (g_portalSet[i] && ClipRegionContains(g_portalPos[i])) return;
	const Vector d = DriftNow() * -1.0f;
	++g_returns;
	Shift(d, "back to the anchor");
}

void FloatOriginLevelLive() {
	// a new level sets the mapping again (TerrainSetRegion): that is the anchor
	g_haveAnchor = false;
	for (Parked &k : g_parked) k = Parked();
	g_parkedProps.clear();
	g_tp = nullptr;  // (the old level's entity is gone)
	g_lastShiftTick = -100000;
}


// Chell to a Source spot with Portal 2's own teleport (the client snaps the next frame), velocity zeroed (pc_mc_home).
bool FloatTeleportPlayer(const Vector &pos, const QAngle &ang) {
	const Vector from = PlayerOrigin();
	const bool ok = TeleportTo("!player", pos, ang);
	if (!ok) SnapPlayer(pos, ang);
	PuppetLaunch(Vector(0, 0, 0));
	PuppetShiftOrigin(pos - from);
	return ok;
}
// The mapping is at the level's anchor (nothing shifted, or brought home).
bool FloatAtAnchor() {
	if (!g_haveAnchor) return true;
	if (FloatsWindow()) return DriftNow().Length() < 0.5f;
	return g_map.mcX == g_anchorMcX && g_map.mcY == g_anchorMcY && g_map.mcZ == g_anchorMcZ;
}
void TestCheck(bool ok, const char *what, const char *detail);
CON_COMMAND(pcb_assert_float_home, "pcb_assert_float_home - the floating origin's mapping is back at the level's anchor\n") {
	char buf[200];
	std::snprintf(buf, sizeof buf, "mapping MC (%.1f %.1f %.1f), anchor MC (%.1f %.1f %.1f), %d shifts, %d returns", g_map.mcX, g_map.mcY, g_map.mcZ, g_anchorMcX, g_anchorMcY, g_anchorMcZ, g_shifts, g_returns);
	TestCheck(FloatAtAnchor(), "floating origin at its anchor", buf);
}

// ---- teleport steps (tests): far points reached through the shift logic, a box-safe step at a time -------
static int g_stepShifts0 = 0;  // shifts before the last step (waituntil floatshift)
bool FloatShiftedSinceStep() { return g_shifts > g_stepShifts0; }
// Chell's feet to a Source point clamped inside the region box (margins: 2 blocks inside the sides, 4 above the
// bottom, her height + 4 below the top), with Portal 2's own teleport; velocity zeroed.
static Vector StepTo(Vector want, const char *what) {
	Vector mn, mx;
	ClipRegionBounds(mn, mx);
	const float m = 2 * kUnitsPerBlock;
	want.x = std::fmax(mn.x + m, std::fmin(mx.x - m, want.x));
	want.y = std::fmax(mn.y + m, std::fmin(mx.y - m, want.y));
	want.z = std::fmax(mn.z + 4 * kUnitsPerBlock, std::fmin(mx.z - 72.0f - 4 * kUnitsPerBlock, want.z));
	const Vector from = PlayerOrigin();
	if (!TeleportTo("!player", want, engine.ViewAngles())) SnapPlayer(want, engine.ViewAngles());
	PuppetLaunch(Vector(0, 0, 0));
	PuppetShiftOrigin(want - from);
	g_stepShifts0 = g_shifts;
	double a[3], b[3];
	SrcToMcD(from, a[0], a[1], a[2]);
	SrcToMcD(want, b[0], b[1], b[2]);
	console->Print("[bridge] float step (%s): Chell MC (%.1f %.1f %.1f) -> (%.1f %.1f %.1f) = Portal 2 (%.0f %.0f %.0f)\n", what, a[0], a[1], a[2], b[0], b[1], b[2], want.x, want.y, want.z);
	return want;
}
CON_COMMAND(pcb_float_step_to, "pcb_float_step_to <mcX|~> <mcY|~> <mcZ|~> - teleport Chell towards a Minecraft point, as far as the region box allows (tests: far points through the shift logic)\n") {
	if (args.ArgC() < 4 || !ClipHasRegion()) return;
	double c[3];
	SrcToMcD(PlayerOrigin(), c[0], c[1], c[2]);
	double t[3];
	for (int i = 0; i < 3; ++i) t[i] = args[1 + i][0] == '~' ? c[i] : std::atof(args[1 + i]);
	StepTo(McToSrc(t[0], t[1], t[2]), "towards a point");
}
CON_COMMAND(pcb_float_near_edge, "pcb_float_near_edge <+x|-x|+z|-z|up|down> <blocks> - teleport Chell that many blocks short of the shift boundary on that axis (tests)\n") {
	if (args.ArgC() < 3 || !ClipHasRegion()) return;
	const char *ax = args[1];
	const float b = (float)std::atof(args[2]) * kUnitsPerBlock;
	Vector mn, mx, p = PlayerOrigin();
	ClipRegionBounds(mn, mx);
	const Vector c = ClipRegionCentre();
	// (Minecraft +z is Source -y)
	if (!std::strcmp(ax, "+x")) p.x = c.x + g_dist - b;
	else if (!std::strcmp(ax, "-x")) p.x = c.x - g_dist + b;
	else if (!std::strcmp(ax, "+z")) p.y = c.y - g_dist + b;
	else if (!std::strcmp(ax, "-z")) p.y = c.y + g_dist - b;
	else if (!std::strcmp(ax, "up")) p.z = mx.z - g_marginV - 72.0f - b;
	else if (!std::strcmp(ax, "down")) p.z = mn.z + g_marginV + b;
	StepTo(p, ax);
}
CON_COMMAND(pcb_assert_float_inbox, "pcb_assert_float_inbox - Chell is inside the region box (tests)\n") {
	const Vector f = PlayerOrigin();
	double x, y, z;
	SrcToMcD(f, x, y, z);
	char buf[200];
	std::snprintf(buf, sizeof buf, "Chell at Portal 2 (%.0f %.0f %.0f) = MC (%.1f %.1f %.1f), %d shifts so far", f.x, f.y, f.z, x, y, z, g_shifts);
	TestCheck(ClipRegionContains(f) && ClipRegionContains(f + Vector(0, 0, 72)), "Chell inside the region box", buf);
}

// Every client frame (OverrideView, before the composite): diagnostics of how the client's camera follows a shift.
void FloatOriginViewFrame(const Vector &eye) {
	++g_viewFrame;
	if (g_eyeLog > 0) {
		--g_eyeLog;
		const Vector s = eye - g_eyePrev;
		console->Print("[bridge] floating origin: view frame +%llu after the shift: eye (%.1f %.1f %.1f), step (%.1f %.1f %.1f) = %.1f u\n", (unsigned long long)(g_viewFrame - g_shiftViewFrame), eye.x,
		               eye.y, eye.z, s.x, s.y, s.z, s.Length());
	}
	g_eyePrev = eye;
}
CON_COMMAND(pcb_float_trace, "pcb_float_trace [frames=12] - log the eye and the composite's pulls/requests for that many frames after each shift (0 off)\n") {
	g_traceN = args.ArgC() > 1 ? std::atoi(args[1]) : 12;
	console->Print("[bridge] floating origin trace: %d frames after each shift\n", g_traceN);
}
CON_COMMAND(pcb_float_tpmode, "pcb_float_tpmode [0|1] - how a shift moves Chell: 0 SnapPlayer, 1 point_teleport (default)\n") {
	if (args.ArgC() > 1) g_tpMode = std::atoi(args[1]);
	console->Print("[bridge] floating origin: Chell moved by %s\n", g_tpMode == 1 ? "point_teleport" : "SnapPlayer");
}
CON_COMMAND(pcb_assert_float_parked, "pcb_assert_float_parked <parked> <placed_again> - at least that many portals parked out of the box and placed again (tests)\n") {
	int wp = args.ArgC() > 1 ? std::atoi(args[1]) : 1, wr = args.ArgC() > 2 ? std::atoi(args[2]) : 1;
	char buf[200];
	std::snprintf(buf, sizeof buf, "%d parked (want >= %d), %d placed again (want >= %d), %d dropped, now parked: %d %d", g_parkedN, wp, g_restoredN, wr, g_parkDropped, g_parked[0].on, g_parked[1].on);
	TestCheck(g_parkedN >= wp && g_restoredN >= wr && !g_parked[0].on && !g_parked[1].on, "portals kept in Minecraft coordinates out of the box", buf);
	if (args.ArgC() > 3) {
		std::snprintf(buf, sizeof buf, "%d props parked, %d put back, %d still parked (want >= %s put back)", g_propsParked, g_propsRestored, (int)g_parkedProps.size(), args[3]);
		TestCheck(g_propsRestored >= std::atoi(args[3]) && g_parkedProps.empty(), "props kept in Minecraft coordinates out of the box", buf);
	}
}
CON_COMMAND(pc_mc_float, "pc_mc_float [0|1] [dist_blocks=200] [margin_v_blocks=16] - endless terrain: recentre the region on Chell by whole chunks (and sections vertically)\n") {
	if (args.ArgC() > 1) g_enabled = std::atoi(args[1]) != 0;
	if (args.ArgC() > 2) g_dist = std::fmax(32.0f, (float)std::atof(args[2])) * kUnitsPerBlock;
	if (args.ArgC() > 3) g_marginV = std::fmax(4.0f, (float)std::atof(args[3])) * kUnitsPerBlock;
	console->Print("[bridge] floating origin %s at %.0f blocks: %d shifts, %d returns, %d portals fizzled, %d props moved; anchor MC (%.1f %.1f), now (%.1f %.1f)\n", g_enabled ? "on" : "off",
	               g_dist / kUnitsPerBlock, g_shifts, g_returns, g_fizzled, g_propsMoved, g_anchorMcX, g_anchorMcZ, g_map.mcX, g_map.mcZ);
}
CON_COMMAND(pcb_float_shift, "pcb_float_shift <chunksX> <chunksZ> [sectionsY] - shift the floating origin now (tests): Minecraft chunks the mapping moves by\n") {
	if (args.ArgC() < 3 || !ClipHasRegion()) return;
	const float c = 16 * kUnitsPerBlock;
	++g_shifts;
	Shift(Vector(-(float)std::atof(args[1]) * c, (float)std::atof(args[2]) * c, args.ArgC() > 3 ? -(float)std::atof(args[3]) * c : 0.0f), "test command");
}
CON_COMMAND(pcb_assert_float_origin, "pcb_assert_float_origin <min_shifts> [mcX mcZ tol=2] - at least that many shifts; optionally Chell's feet are at that Minecraft x/z\n") {
	int want = args.ArgC() > 1 ? std::atoi(args[1]) : 1;
	double x, y, z;
	SrcToMcD(PlayerOrigin(), x, y, z);
	bool posOk = true;
	char buf[220];
	if (args.ArgC() > 3) {
		double tx = std::atof(args[2]), tz = std::atof(args[3]), tol = args.ArgC() > 4 ? std::atof(args[4]) : 2.0;
		posOk = std::hypot(x - tx, z - tz) <= tol;
	}
	std::snprintf(buf, sizeof buf, "%d shifts (want >= %d), %d returns, Chell at MC (%.2f %.2f %.2f), Source (%.0f %.0f %.0f)", g_shifts, want, g_returns, x, y, z, PlayerOrigin().x,
	              PlayerOrigin().y, PlayerOrigin().z);
	TestCheck(g_shifts >= want && posOk, "floating origin", buf);
}

// Tests: Minecraft positions of the portals and the newest cube must not change across shifts.
static double g_mark[3][3];
static bool g_markSet[3] = {};
static bool McPosOf(int which, double out[3]) {
	void *e = nullptr;
	if (which < 2) {
		for (void *p : EntFind("prop_portal"))
			if ((EntKV(p, "PortalTwo") == "1") == (which == 1) && EntKV(p, "Activated") == "1") e = p;
	} else {
		auto c = EntFind("prop_weighted_cube");
		if (!c.empty()) e = c.back();
	}
	if (!e) return false;
	SrcToMcD(EntVec(e, "origin"), out[0], out[1], out[2]);
	return true;
}
CON_COMMAND(pcb_float_mark, "pcb_float_mark - remember the Minecraft positions of both portals and the newest cube (tests)\n") {
	for (int i = 0; i < 3; ++i) {
		g_markSet[i] = McPosOf(i, g_mark[i]);
		if (g_markSet[i]) console->Print("[bridge] float mark %s at MC (%.2f %.2f %.2f)\n", i == 2 ? "cube" : i ? "portal 1" : "portal 0", g_mark[i][0], g_mark[i][1], g_mark[i][2]);
	}
}
CON_COMMAND(pcb_assert_float_intact, "pcb_assert_float_intact [tol_blocks=0.3] - the marked portals and cube are at their Minecraft positions, open or parked out of the box (tests)\n") {
	double tol = args.ArgC() > 1 ? std::atof(args[1]) : 0.3;
	char buf[260];
	for (int i = 0; i < 3; ++i) {
		if (!g_markSet[i]) continue;
		double now[3];
		const char *state = "parked";
		bool have = false;
		if (i < 2 && g_parked[i].on) {
			for (int k = 0; k < 3; ++k) now[k] = g_parked[i].mc[k];
			have = true;
		}
		if (i == 2)
			for (auto &q : g_parkedProps)
				if (q.name == "pcb_cube") {
					for (int k = 0; k < 3; ++k) now[k] = q.mc[k];
					have = true;
				}
		if (!have) have = McPosOf(i, now), state = "open";
		double d = have ? std::sqrt((now[0] - g_mark[i][0]) * (now[0] - g_mark[i][0]) + (now[1] - g_mark[i][1]) * (now[1] - g_mark[i][1]) + (now[2] - g_mark[i][2]) * (now[2] - g_mark[i][2])) : -1;
		std::snprintf(buf, sizeof buf, "%s: was MC (%.2f %.2f %.2f), now %s (%.2f %.2f %.2f), %.2f blocks away (tol %.2f)", i == 2 ? "cube" : i ? "portal 1" : "portal 0", g_mark[i][0], g_mark[i][1], g_mark[i][2],
		              have ? state : "gone", have ? now[0] : 0, have ? now[1] : 0, have ? now[2] : 0, d, tol);
		TestCheck(have && d <= tol, "intact (open or parked) at its Minecraft position", buf);
	}
}
CON_COMMAND(pcb_assert_float_kept, "pcb_assert_float_kept [tol_blocks=0.3] - the marked portals and cube are still at the same Minecraft positions (tests)\n") {
	double tol = args.ArgC() > 1 ? std::atof(args[1]) : 0.3;
	for (int i = 0; i < 3; ++i) {
		if (!g_markSet[i]) continue;
		double now[3];
		bool have = McPosOf(i, now);
		double d = have ? std::sqrt((now[0] - g_mark[i][0]) * (now[0] - g_mark[i][0]) + (now[1] - g_mark[i][1]) * (now[1] - g_mark[i][1]) + (now[2] - g_mark[i][2]) * (now[2] - g_mark[i][2])) : -1;
		char buf[200];
		std::snprintf(buf, sizeof buf, "%s: was MC (%.2f %.2f %.2f), now %s (%.2f %.2f %.2f), moved %.2f blocks (tol %.2f)", i == 2 ? "cube" : i ? "portal 1" : "portal 0", g_mark[i][0], g_mark[i][1],
		              g_mark[i][2], have ? "" : "gone", have ? now[0] : 0, have ? now[1] : 0, have ? now[2] : 0, d, tol);
		TestCheck(have && d <= tol, "kept its Minecraft position across the shift", buf);
	}
}

}  // namespace bridge
