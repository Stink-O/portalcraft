// Portalcraft bridge: Portal 2 physics props (cubes, spheres, turrets) on Minecraft terrain.
//
// Chell collides with the streamed solid bitsets through the movement-trace hook, but vphysics props
// only know Portal 2's world. A pool of invisible one-block colliders (the Phase-0 collider: a
// prop_dynamic_override with SOLID_BBOX and a 39.6 u box hull, which props land and rest on; verified
// with a cube on a test block) is kept on the exposed terrain blocks around every physics prop in the
// region, and around where it is heading (its velocity, a few ticks ahead), so a flung cube lands
// too. Colliders stay on valid terrain until another prop needs them; none sits in an open portal's
// hole volume (a prop passes through the cliff portal). Scale: a few dozen colliders per moving prop,
// not one entity per block of the world.
#include "bridge.hpp"
#include "growth.hpp"
#include <algorithm>
#include "coords.hpp"
#include "stage.hpp"

#include "Modules/Tier1.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bridge {
Vector ClipRegionCentre();  // clip.cpp

bool ClipHasRegion();
bool ClipRegionContains(const Vector &p);
bool TerrainIsSolid(int bx, int by, int bz);
bool BoxInPortalHole(const Vector &mn, const Vector &mx);  // blockcollide.cpp
void *SpawnBlockCollider(const Vector &mins);              // collider.cpp
void SetColliderLog(bool on);                              // collider.cpp
void EntLiveSet(std::unordered_set<void *> &out);           // ents.cpp

bool EntTeleport(void *ent, const Vector &pos, const QAngle &ang);  // floatorigin.cpp

static bool g_enabled = true;
static bool g_pushOut = true;  // pcb_prop_pushout: a prop sunk into a block top is lifted onto it and put to sleep
static int g_pushes = 0, g_pushLog = 6;
static int g_radius = 2;           // blocks around each prop (and its predicted positions)
static int g_maxPool = 320;
static int g_tick = 0;
static int g_moves = 0, g_spawns = 0, g_wanted = 0, g_starved = 0;
static double g_msSum = 0, g_msMax = 0;
static int g_msN = 0;

struct Slot {
	void *ent;
	long long key;  // block it sits on, or 0 when parked
	int used;       // last tick it was wanted
	Vector at;      // where the entity was last put (its centre)
};
static std::vector<Slot> g_pool;
PCB_GAUGE("props.pool", g_pool.size());
static std::unordered_map<long long, int> g_byKey;
struct PropTrack {
	Vector last;
	Vector vel;
	int seen;
	bool pinned = false;  // push-out: held on a block top every tick until something moves it
	Vector pinAt;
	int still = 0;  // consecutive ticks under 5 u/s (waituntil still<name>)
};
static std::unordered_map<void *, PropTrack> g_props;
PCB_GAUGE("props.tracked", g_props.size());
static std::vector<void *> g_propList;

static const char *kPropClasses[] = {"prop_weighted_cube", "prop_physics", "prop_physics_override", "prop_physics_multiplayer", "npc_portal_turret_floor",
                                     "prop_monster_box"};

static inline long long Key(int x, int y, int z) { return ((long long)(x & 0x3FFFFF) << 42) | ((long long)(z & 0x3FFFFF) << 20) | (long long)(y & 0xFFFFF) | (1ll << 63); }
static inline void Unkey(long long k, int &x, int &y, int &z) {
	auto sx = [](long long v, int bits) { return (int)((v << (64 - bits)) >> (64 - bits)); };
	x = sx((k >> 42) & 0x3FFFFF, 22), z = sx((k >> 20) & 0x3FFFFF, 22), y = sx(k & 0xFFFFF, 20);
}
static Vector BlockMins(int x, int y, int z) {
	// Minecraft block (x, y, z) spans x..x+1, y..y+1, z..z+1; Source: +x, -z, +y
	Vector a = McToSrc(x, y, z + 1);
	return a;
}
// Where a collider for that block goes (the mins handed to the collider's box). The collider's box is 39.6 u
// (0.2 u in from every side of the block); it is raised 0.2 u so its top is the block's top: Portal 2's own ground
// trace stands Chell (and rests props) on it, and 0.2 u down was inside the blocks for the block hook (bridge #12).
static float g_colLift = 0.2f;  // pcb_prop_collide_lift
static Vector ColliderMins(int x, int y, int z) { return BlockMins(x, y, z) + Vector(0, 0, g_colLift); }
static bool Exposed(int x, int y, int z) {
	return !TerrainIsSolid(x + 1, y, z) || !TerrainIsSolid(x - 1, y, z) || !TerrainIsSolid(x, y + 1, z) || !TerrainIsSolid(x, y - 1, z) || !TerrainIsSolid(x, y, z + 1) ||
	       !TerrainIsSolid(x, y, z - 1);
}

static void MoveSlot(Slot &s, long long key, const Vector &mins) {
	const float h = kUnitsPerBlock / 2;
	char buf[96];
	std::snprintf(buf, sizeof buf, "%.2f %.2f %.2f", mins.x + h, mins.y + h, mins.z + h);
	EntInput(s.ent, "SetLocalOrigin", buf);
	s.at = Vector(mins.x + h, mins.y + h, mins.z + h);
	if (s.key) g_byKey.erase(s.key);
	s.key = key;
	if (key) g_byKey[key] = (int)(&s - g_pool.data());
	++g_moves;
}

// Floating origin: colliders sit on Minecraft blocks, so after the mapping moved they are all in the wrong
// Source place: forget their keys (they are re-placed on the next tick) and move the props' tracks.
// The keys are Minecraft blocks, so they stay valid: every collider moves by the shift with the world (bridge #12:
// they used to keep their old Source spot, which after the shift was some other place of the region, and only
// the ones wanted again were moved: invisible solid boxes in the air for props).
void PropCollideShift(const Vector &d) {
	for (Slot &s : g_pool) {
		if (!s.ent) continue;
		const float h = kUnitsPerBlock / 2;
		const Vector at = s.at + d;
		char buf[96];
		std::snprintf(buf, sizeof buf, "%.2f %.2f %.2f", at.x, at.y, at.z);
		EntInput(s.ent, "SetLocalOrigin", buf);
		s.at = at;
		(void)h;
	}
	for (auto &kv : g_props) kv.second.last += d, kv.second.pinAt += d;
	g_tick = 0;  // re-place this tick
}

void PropCollideLevelLive() {
	g_pool.clear();  // the entities went with the old level
	g_byKey.clear();
	g_props.clear();
	g_propList.clear();
}

// A prop was just spawned (pcb_spawn_cube): list the props again on the next tick, not up to 30 ticks later
// (a cube spawned a few units above the terrain fell through before its colliders were placed)
void PropCollideRescanSoon() { g_tick = 0; }

bool PlayerHolding();  // below
// Every server tick.
void PropCollideTick() {
	// (lead) every map with a Minecraft mapping, not only the region maps: in campaign chambers (the unified world, the stage) cubes and
	// turrets must rest on and bump into the blocks the Minecraft player places there (the lookups find nothing where there are none)
	bool TerrainMapReady();  // terrain.cpp
	if (!g_enabled || !(ClipHasRegion() || TerrainMapReady())) return;
	++g_tick;
	auto t0 = std::chrono::steady_clock::now();
	struct Timer {
		std::chrono::steady_clock::time_point t0;
		~Timer() {
			double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			g_msSum += ms, ++g_msN;
			if (ms > g_msMax) g_msMax = ms;
		}
	} timer{t0};
	if (g_tick % 30 == 1) {
		g_propList.clear();
		for (void *e : EntFind(nullptr)) {
			std::string c = EntKV(e, "classname");
			for (const char *k : kPropClasses)
				if (c == k) {
					g_propList.push_back(e);
					break;
				}
		}
	}
	// A prop removed since the list was built (a cube fizzled, dissolved or killed) left a dangling
	// pointer: reading it crashed the game (run mw1s3, ent_fire <cube> kill). Only live entities.
	{
		static std::unordered_set<void *> live;
		EntLiveSet(live);
		g_propList.erase(std::remove_if(g_propList.begin(), g_propList.end(), [](void *e) { return !live.count(e); }), g_propList.end());
		for (auto it = g_props.begin(); it != g_props.end();) it = live.count(it->first) ? std::next(it) : g_props.erase(it);
	}
	std::unordered_set<long long> want;
	for (void *e : g_propList) {
		Vector o = EntVec(e, "origin");
		PropTrack &t = g_props[e];
		if (t.seen) t.vel = (o - t.last) * 60.0f;
		t.still = t.seen && t.vel.Length() < 5.0f ? t.still + 1 : 0;
		t.last = o, t.seen = g_tick;
		// near or in the region (a cube flung in arrives within a tick of the boundary)
		const Vector reach = o + t.vel * 0.1f;
		// (bridge #17: also in the chambers, near blocks the Minecraft player placed there: the band's solids; elsewhere in
		// the chambers the lookups find nothing)
		(void)reach;
		const int nsteps = 3;
		for (int st = 0; st < nsteps; ++st) {
			Vector c = o + t.vel * (0.05f * st);
			double mx, my, mz;
			SrcToMcD(c, mx, my, mz);
			const int bx = (int)std::floor(mx), by = (int)std::floor(my), bz = (int)std::floor(mz);
			for (int dy = -g_radius; dy <= g_radius; ++dy)
				for (int dz = -g_radius; dz <= g_radius; ++dz)
					for (int dx = -g_radius; dx <= g_radius; ++dx) {
						int x = bx + dx, y = by + dy, z = bz + dz;
						if (!TerrainIsSolid(x, y, z) || !Exposed(x, y, z)) continue;
						Vector mn = BlockMins(x, y, z), mxv = mn + Vector(kUnitsPerBlock, kUnitsPerBlock, kUnitsPerBlock);
						if (BoxInPortalHole(mn, mxv)) continue;
						want.insert(Key(x, y, z));
					}
		}
	}
	// Push-out. Near an open portal Portal 2 simulates a prop in that portal's own physics environment, which
	// holds a carved copy of the map's world and nothing of ours: the colliders do not count there and a cube
	// set down on blocks within ~2 blocks of a portal fell through them (float2: corridor floor, MC y 89 -> 63;
	// run diagcube: a cube 80 u before the portal fell, ones 200 u away rested). So a prop whose bottom sank
	// more than 0.5 u into the top of a solid block (outside every portal hole) while not rising is lifted onto
	// that top with Portal 2's own teleport and held there: every tick it is put back (gravity sinks it < 0.1 u
	// per tick; "Sleep" stops its velocity, but it does not stay asleep there) until something moves it more
	// than 3 u (picked up, pushed) or the block under it is gone. Where the colliders work a prop never sinks
	// into a block, so this does not fire.
	// the prop Chell holds: the tracked prop nearest her eye while she holds something
	const bool holding = PlayerHolding();
	void *held = nullptr;
	if (holding) {
		float best = 150.0f;
		const Vector eye = PlayerOrigin() + Vector(0, 0, 64);
		for (void *e : g_propList) {
			auto it = g_props.find(e);
			if (it == g_props.end()) continue;
			const float dd = (it->second.last - eye).Length();
			if (dd < best) best = dd, held = e;
		}
	}
	if (g_pushOut)
		for (void *e : g_propList) {
			auto it = g_props.find(e);
			if (it == g_props.end() || it->second.seen != g_tick) continue;
			PropTrack &t = it->second;
			const Vector o = t.last, v = t.vel;
			if (holding && e == held) {
				t.pinned = false;  // (a held prop: Portal 2's hold controller moves it; teleporting it fought the carry)
				continue;
			}
			const float half = 18.0f;  // (a weighted cube lying flat; others are close enough)
			// The block under the prop's centre only (its centre of mass decides what holds it). Bridge #13: it also took
			// four points 17 u out along x and y; a cube sliding off a one-block step (centre already over the lower
			// column) had one of them 0.2 u inside the step's column, sank 1.5 u below the step's top as it went over
			// the edge and was lifted 40 u back onto the step and pinned there, half off it (props1: cube_fast and
			// cube_fling resting 58 u above their own column's top)
			float top = -1e30f;
			{
				const Vector p = o + Vector(0, 0, -half);
				double mx, my, mz;
				SrcToMcD(p, mx, my, mz);
				const int x = (int)std::floor(mx), y = (int)std::floor(my), z = (int)std::floor(mz);
				// (the block under the bottom, or the one just below it when the bottom is right on a top)
				for (int dy = 0; dy >= -1; --dy) {
					if (!TerrainIsSolid(x, y + dy, z)) continue;
					Vector mn = BlockMins(x, y + dy, z), mxv = mn + Vector(kUnitsPerBlock, kUnitsPerBlock, kUnitsPerBlock);
					if (BoxInPortalHole(mn, mxv) || mxv.z > o.z) continue;  // (a hole; or a block beside the prop, not under it)
					top = std::fmax(top, mxv.z);
				}
			}
			if (t.pinned) {
				// released when something moved it (picked up, pushed, hit) or the block under it is gone (a portal hole)
				const Vector dxy(o.x - t.pinAt.x, o.y - t.pinAt.y, 0);
				if (dxy.Length() > 3.0f || o.z > t.pinAt.z + 3.0f || top < -1e29f || std::fabs(top + half + 0.05f - t.pinAt.z) > 1.0f) {
					t.pinned = false;
					EntInput(e, "Wake");
					continue;
				}
				if (o.z < t.pinAt.z - 0.02f) {
					const Vector a = EntVec(e, "angles");
					EntTeleport(e, t.pinAt, QAngle{a.x, a.y, a.z});
					EntInput(e, "Sleep");
					t.last = t.pinAt, t.vel = Vector(0, 0, 0);
				}
				continue;
			}
			if (v.z > 1.0f || top < -1e29f || o.z - half > top - 0.5f) continue;
			const Vector a = EntVec(e, "angles");
			const Vector to(o.x, o.y, top + half + 0.05f);
			const bool ok = EntTeleport(e, to, QAngle{a.x, a.y, a.z});
			EntInput(e, "Sleep");
			t.last = to, t.vel = Vector(0, 0, 0), t.pinned = true, t.pinAt = to;
			++g_pushes;
			if (g_pushLog > 0) --g_pushLog, console->Print("[bridge] prop push-out: %s sank to z %.1f (block top %.1f, falling %.0f u/s): lifted onto it and held there (teleport %d)\n", EntKV(e, "targetname").c_str(), o.z - half, top, -v.z, ok);
		}
	// colliders in an open portal's hole volume leave it (the portal may have opened since)
	if (g_tick % 10 == 0)
		for (Slot &s : g_pool) {
			if (!s.key) continue;
			int x, y, z;
			Unkey(s.key, x, y, z);
			Vector mn = BlockMins(x, y, z);
			if (BoxInPortalHole(mn, mn + Vector(kUnitsPerBlock, kUnitsPerBlock, kUnitsPerBlock)) || !TerrainIsSolid(x, y, z)) {
				// out of reach, independent of the mapping: the region box's far corner, near its bottom (>= 330 blocks from
				// anywhere Chell can be, past the 128-block collision export). (It was Minecraft y 44 under the cliff, "never
				// exported"; bridge #11 exports deep sections' collision, and after a vertical shift that spot can be Chell's cave.)
				Vector park = StageActive() ? StageParkSpot() : ClipRegionCentre() + Vector(-15000.0f, -15000.0f, -5000.0f);  // (bridge #47: the virtual region's corner is far from the centre of Portal 2's world)
				MoveSlot(s, 0, park);
			}
		}
	g_wanted = (int)want.size();
	for (long long k : want) {
		auto it = g_byKey.find(k);
		if (it != g_byKey.end()) {
			g_pool[it->second].used = g_tick;
			continue;
		}
	}
	for (long long k : want) {
		if (g_byKey.count(k)) continue;
		int x, y, z;
		Unkey(k, x, y, z);
		Vector mn = ColliderMins(x, y, z);
		// a free slot: one not wanted this tick (it stays valid terrain until moved)
		Slot *free = nullptr;
		for (Slot &s : g_pool)
			if (s.used != g_tick && (!free || s.used < free->used)) free = &s;
		if (free && (int)g_pool.size() >= g_maxPool) {
			MoveSlot(*free, k, mn);
			free->used = g_tick;
		} else if ((int)g_pool.size() < g_maxPool) {
			SetColliderLog(false);
			void *e = SpawnBlockCollider(mn);
			if (!e) break;
			g_pool.push_back(Slot{e, k, g_tick, mn + Vector(kUnitsPerBlock / 2, kUnitsPerBlock / 2, kUnitsPerBlock / 2)});
			g_byKey[k] = (int)g_pool.size() - 1;
			++g_spawns;
		} else {
			++g_starved;
		}
	}
}

CON_COMMAND(pcb_prop_collide_lift, "pcb_prop_collide_lift [u=0.2] - raise new colliders so their top is the block top (0 = the old 0.2 u low top)\n") {
	if (args.ArgC() > 1) g_colLift = (float)std::atof(args[1]);
	int n = 0;
	for (Slot &sl : g_pool)
		if (sl.key) {
			int x, y, z;
			Unkey(sl.key, x, y, z);
			MoveSlot(sl, sl.key, ColliderMins(x, y, z)), ++n;
		}
	console->Print("[bridge] prop collider lift %.2f u (%d colliders re-placed)\n", g_colLift, n);
}
CON_COMMAND(pcb_prop_pushout, "pcb_prop_pushout [0|1] - lift a prop that sank into a Minecraft block top onto it (props near a portal, whose colliders do not count)\n") {
	if (args.ArgC() > 1) g_pushOut = std::atoi(args[1]) != 0;
	console->Print("[bridge] prop push-out %s, %d so far\n", g_pushOut ? "on" : "off", g_pushes);
}
CON_COMMAND(pcb_prop_collide, "pcb_prop_collide [0|1] [radius_blocks=2] [max_colliders=320] - Portal 2 props collide with Minecraft terrain (block colliders around each prop)\n") {
	if (args.ArgC() > 1) g_enabled = std::atoi(args[1]) != 0;
	if (args.ArgC() > 2) g_radius = std::max(1, std::min(4, std::atoi(args[2])));
	if (args.ArgC() > 3) g_maxPool = std::max(16, std::min(800, std::atoi(args[3])));
	int unkeyed = 0;
	for (const Slot &sl : g_pool) unkeyed += !sl.key;
	console->Print("[bridge] prop collide: %d of the colliders have no block (parked or left after a shift)\n", unkeyed);
	console->Print("[bridge] prop collide %d: radius %d, %zu colliders (max %d), %d wanted now, %d spawned, %d moved, %d starved, %zu props tracked, %d push-outs; %.3f ms/tick mean, %.2f ms worst\n",
	               g_enabled, g_radius, g_pool.size(), g_maxPool, g_wanted, g_spawns, g_moves, g_starved, g_propList.size(), g_pushes, g_msN ? g_msSum / g_msN : 0.0, g_msMax);
	g_msSum = 0, g_msN = 0, g_msMax = 0;
}

// One of the pool's collider entities (the blocker log in blockcollide.cpp).
bool PropColliderIs(void *ent) {
	for (const Slot &s : g_pool)
		if (s.ent == ent) return true;
	return false;
}
// A point on or in a collider's box (client traces: client entities are not the server's pointers).
bool PropColliderNear(const Vector &p) {
	for (const Slot &s : g_pool) {
		if (!s.ent) continue;
		const Vector o = s.at;
		if (std::fabs(p.x - o.x) < 21 && std::fabs(p.y - o.y) < 21 && std::fabs(p.z - o.z) < 21) return true;
	}
	return false;
}

// The tracked physics props in the region (centres, last server tick), for the composite's occlusion layers.
int PropCentresInRegion(Vector *out, int max) {
	int n = 0;
	for (void *e : g_propList) {
		auto it = g_props.find(e);
		if (it == g_props.end() || !ClipRegionContains(it->second.last)) continue;
		if (n < max) out[n++] = it->second.last;
	}
	return n;
}

// pcb_prop_debug <targetname>: the blocks under a prop (solid, exposed, in a portal's hole, collider placed).
CON_COMMAND(pcb_prop_debug, "pcb_prop_debug [targetname=pcb_cube] - the Minecraft blocks under a prop and the colliders on them\n") {
	const char *name = args.ArgC() > 1 ? args[1] : "pcb_cube";
	auto v = EntFind(nullptr, name);
	if (v.empty()) return console->Print("[bridge] prop debug: no %s\n", name);
	Vector o = EntVec(v[0], "origin");
	double mx, my, mz;
	SrcToMcD(o, mx, my, mz);
	bool listed = std::find(g_propList.begin(), g_propList.end(), v[0]) != g_propList.end();
	console->Print("[bridge] prop debug %s at (%.1f %.1f %.1f) = MC (%.2f %.2f %.2f), listed %d, %zu colliders, %d wanted\n", name, o.x, o.y, o.z, mx, my, mz, listed, g_pool.size(), g_wanted);
	const int bx = (int)std::floor(mx), by = (int)std::floor(my), bz = (int)std::floor(mz);
	for (int y = by; y >= by - 3; --y)
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx) {
				int x = bx + dx, z = bz + dz;
				Vector mn = BlockMins(x, y, z), mxv = mn + Vector(kUnitsPerBlock, kUnitsPerBlock, kUnitsPerBlock);
				auto it = g_byKey.find(Key(x, y, z));
				Vector co(0, 0, 0);
				if (it != g_byKey.end()) co = EntVec(g_pool[it->second].ent, "origin");
				if (TerrainIsSolid(x, y, z) || it != g_byKey.end())
					console->Print("[bridge]   block (%d %d %d): solid %d exposed %d hole %d collider %d at (%.1f %.1f %.1f), block centre (%.1f %.1f %.1f)\n", x, y, z, TerrainIsSolid(x, y, z), Exposed(x, y, z),
					               BoxInPortalHole(mn, mxv), it != g_byKey.end(), co.x, co.y, co.z, mn.x + 20, mn.y + 20, mn.z + 20);
			}
}

// waituntil still<name>: that prop has moved under 5 u/s for 30 ticks (a cube tumbling down a slope settles at
// its own pace; throttled runs ended fixed waits while it still rolled, bridge #13)
bool PropStill(const char *name) {
	auto v = EntFind(nullptr, name);
	if (v.empty()) return false;
	auto it = g_props.find(v[0]);
	return it != g_props.end() && it->second.still >= 30;
}

void TestCheck(bool ok, const char *what, const char *detail);
int EntFieldOffset(void *ent, const char *field);  // ents.cpp
// Chell holds an object (Portal 2's own flag on the player).
bool PlayerHolding() {
	auto v = EntFind("player");
	if (v.empty()) return false;
	static int off = -2;
	if (off == -2) off = EntFieldOffset(v[0], "m_bIsHoldingSomething");
	return off >= 0 && *((const unsigned char *)v[0] + off) != 0;
}
CON_COMMAND(pcb_assert_holding, "pcb_assert_holding [1|0] [targetname] - Chell holds an object (1) or not (0); with a name, that prop is within 120 u of her eye\n") {
	const bool want = args.ArgC() < 2 || std::atoi(args[1]) != 0;
	const bool h = PlayerHolding();
	char buf[220];
	float d = -1;
	if (args.ArgC() > 2) {
		auto v = EntFind(nullptr, args[2]);
		if (!v.empty()) d = (EntVec(v[0], "origin") - (PlayerOrigin() + Vector(0, 0, 64))).Length();
	}
	std::snprintf(buf, sizeof buf, "holding %d (want %d)%s%s, %.0f u from the eye", h, want, args.ArgC() > 2 ? ", prop " : "", args.ArgC() > 2 ? args[2] : "", d);
	TestCheck(h == want && (args.ArgC() < 3 || !want || (d >= 0 && d < 120)), "holding", buf);
}
CON_COMMAND(pcb_aim_prop, "pcb_aim_prop <targetname> - turn Chell's view to a prop's centre (tests: pick it up with +use)\n") {
	if (args.ArgC() < 2) return;
	auto v = EntFind(nullptr, args[1]);
	if (v.empty()) return console->Print("[bridge] aim_prop: no %s\n", args[1]);
	const Vector d = EntVec(v[0], "origin") - (PlayerOrigin() + Vector(0, 0, 64));
	const float l = d.Length();
	if (l < 1) return;
	char buf[96];
	std::snprintf(buf, sizeof buf, "setang %.2f %.2f 0", -std::asin(d.z / l) * 180.0f / (float)M_PI, std::atan2(d.y, d.x) * 180.0f / (float)M_PI);
	engine.Cmd(buf);
	console->Print("[bridge] aim_prop %s: %.0f u away, %s\n", args[1], l, buf);
}
static Vector g_propMark;
CON_COMMAND(pcb_prop_mark, "pcb_prop_mark [targetname=pcb_cube] - remember a prop's position (pcb_assert_prop_moved)\n") {
	auto v = EntFind(nullptr, args.ArgC() > 1 ? args[1] : "pcb_cube");
	if (!v.empty()) g_propMark = EntVec(v[0], "origin");
}
CON_COMMAND(pcb_assert_prop_moved, "pcb_assert_prop_moved <targetname> <min_u> [min_up_u] - the prop moved at least that far (and up) since pcb_prop_mark: e.g. picked up\n") {
	const char *name = args.ArgC() > 1 ? args[1] : "pcb_cube";
	auto v = EntFind(nullptr, name);
	const float want = args.ArgC() > 2 ? (float)std::atof(args[2]) : 10.0f, up = args.ArgC() > 3 ? (float)std::atof(args[3]) : -1e9f;
	char buf[200];
	if (v.empty()) {
		std::snprintf(buf, sizeof buf, "no entity named %s", name);
		return TestCheck(false, "prop moved", buf);
	}
	const Vector o = EntVec(v[0], "origin");
	std::snprintf(buf, sizeof buf, "%s from (%.1f %.1f %.1f) to (%.1f %.1f %.1f): %.1f u, %.1f u up (want >= %.0f, up >= %.0f)", name, g_propMark.x, g_propMark.y, g_propMark.z, o.x, o.y, o.z, (o - g_propMark).Length(),
	              o.z - g_propMark.z, want, up);
	TestCheck((o - g_propMark).Length() >= want && o.z - g_propMark.z >= up, "prop moved", buf);
}
// The prop rests on Minecraft terrain: in the region, (nearly) still, its bottom within a block above
// the terrain top under it.
CON_COMMAND(pcb_assert_prop_rest, "pcb_assert_prop_rest <targetname> [ground_top_y] - that prop rests on Minecraft terrain (in the region, still, on the ground; on that block top if given)\n") {
	const char *name = args.ArgC() > 1 ? args[1] : "pcb_cube";
	auto v = EntFind(nullptr, name);
	char buf[400];
	if (v.empty()) {
		std::snprintf(buf, sizeof buf, "no entity named %s", name);
		return TestCheck(false, "prop rests on terrain", buf);
	}
	Vector o = EntVec(v[0], "origin");
	auto it = g_props.find(v[0]);
	float speed = it != g_props.end() ? it->second.vel.Length() : -1;
	double mx, my, mz;
	SrcToMcD(o, mx, my, mz);
	int x = (int)std::floor(mx), z = (int)std::floor(mz), top = -1000;
	for (int y = (int)std::floor(my) + 1; y > (int)std::floor(my) - 40; --y)
		if (TerrainIsSolid(x, y, z)) {
			top = y + 1;
			break;
		}
	float above = top > -1000 ? (float)((my - top) * kUnitsPerBlock) : 1e9f;  // prop origin (its centre) above the ground, Source units
	// (diagnostics: the highest top under the cube's footprint, its angles, whether push-out holds it)
	int ftop = -1000;
	for (int k = 0; k < 4; ++k) {
		const int fx = (int)std::floor(mx + (k & 1 ? 0.45 : -0.45)), fz = (int)std::floor(mz + (k & 2 ? 0.45 : -0.45));
		for (int y = (int)std::floor(my) + 1; y > (int)std::floor(my) - 40; --y)
			if (TerrainIsSolid(fx, y, fz)) {
				ftop = std::max(ftop, y + 1);
				break;
			}
	}
	const Vector ang = EntVec(v[0], "angles");
	std::snprintf(buf, sizeof buf, "%s at (%.1f %.1f %.1f) = MC (%.2f %.2f %.2f), ground top y %d, centre %.1f u above it, speed %.1f u/s (footprint top y %d, angles %.0f %.0f %.0f, pinned %d)", name, o.x, o.y, o.z,
	              mx, my, mz, top, above, speed, ftop, ang.x, ang.y, ang.z, it != g_props.end() && it->second.pinned);
	const bool onTop = args.ArgC() < 3 || top == std::atoi(args[2]);
	if (args.ArgC() > 2) std::snprintf(buf + std::strlen(buf), sizeof buf - std::strlen(buf), " (want ground top y %s)", args[2]);
	// on its own column's top, or (not held up by push-out) wedged on the highest top under its footprint: a cube
	// tipped into a one-block notch rests on the notch's rim (props1 bridge #13: pitch 53, 11.5 u above the rim)
	const bool pinned = it != g_props.end() && it->second.pinned;
	const float aboveRim = ftop > -1000 ? (float)((my - ftop) * kUnitsPerBlock) : 1e9f;
	const bool supported = (above > 0 && above < 40) || (!pinned && aboveRim > 0 && aboveRim < 40);
	TestCheck(ClipRegionContains(o) && supported && speed >= 0 && speed < 20 && onTop, "prop rests on terrain", buf);
}

}  // namespace bridge
