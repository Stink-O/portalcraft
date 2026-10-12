// Portalcraft bridge (bridge #26): movers, one mechanism for every host solid that can move or animate.
//
// The CollisionField export (collfield.cpp) holds only what never moves: the world and static props. Everything else
// that is solid and can move or animate is a "mover", decided by class at listing (not by "has it moved"): doors, lifts,
// trains and elevators, moving brushes, solid animated props and their bone followers, buttons, the test chamber doors,
// and physics props (cubes, turrets). Each mover's collision is voxelised once in its own frame (5 u cells, coarse to
// fine, with traces that hit only that entity, wherever it stands then), merged into boxes, and kept by key (model +
// skin + body + bounds; a brush model by map and model), in memory and on disk (cache/bridge/movershapes). Minecraft
// gets each shape once per collision epoch (kColMoverShape) and each mover's shape by id (kColMoverBind); then, every
// host frame, the pose of every bound mover near the player (the McMovers block). Minecraft collides with each shape at
// its pose, carries a player that stands on a mover by the mover's pose change, and pushes a player a mover moves into
// (the mod's SkyMovers). Nothing here knows about elevators, lifts or any one map.
#include "bridge.hpp"
#include "growth.hpp"
#include "coords.hpp"
#include "dig.hpp"
#include "link.hpp"
#include "moverwalls.hpp"
#include "prof.hpp"

#include "Modules/Tier1.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>
#include "platform/platform.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bridge {

using namespace skycraft::proto;

int EntFieldOffset(void *ent, const char *field);           // ents.cpp
int EntMoveType(void *ent);                                 // ents.cpp
void EntLiveSet(std::unordered_set<void *> &out);           // ents.cpp
bool PlayerHolding();                                      // propcollide.cpp
const char *McMapName();                                   // mc.cpp
std::string MapVersionKey(const std::string &map);         // mapindex.cpp (bridge #34)
bool CollisionFieldCleared();                              // collfield.cpp: this epoch's kColClear has gone out
uint32_t CollisionFieldEpoch();                            // collfield.cpp
void TestCheck(bool ok, const char *what, const char *detail);  // testassert.cpp

static double NowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---- classes -------------------------------------------------------------------------------------------------------
// (bridge #65) prop_dynamic_override, func_wall, func_breakable and func_monitor are solid entities the map relies on for collision (the intro1 container
// and end wall, the glass floor, faith-plate floor panels, screens in walls). prop_dynamic_override is listed EXCEPT the plugin's own prop colliders on
// Minecraft blocks (propcollide.cpp / collider.cpp, named pcb_col_N): see IsOwnCollider. SolidNow drops a broken or non-solid one.
static const char *const kMoverClasses[] = {
	"func_door", "func_door_rotating", "func_movelinear", "func_tracktrain", "func_train", "func_tanktrain", "func_brush", "func_rotating",
	"func_platrot", "func_wall_toggle", "func_button", "func_rot_button", "func_physbox", "func_physbox_multiplayer", "prop_testchamber_door",
	"prop_dynamic", "prop_floor_button", "prop_under_floor_button", "prop_button", "prop_under_button", "phys_bone_follower", "prop_physics",
	"prop_physics_override", "prop_physics_multiplayer", "prop_weighted_cube", "prop_monster_box", "npc_portal_turret_floor",
	"prop_exploding_futbol", "prop_glass_futbol", "projected_wall_entity", "prop_dynamic_override", "func_wall", "func_breakable", "func_monitor"};
static bool IsOwnCollider(const std::string &name) { return name.rfind("pcb_col_", 0) == 0; }
static bool IsPhysicsClass(const std::string &c) {
	return c.rfind("prop_physics", 0) == 0 || c == "prop_weighted_cube" || c == "prop_monster_box" || c == "npc_portal_turret_floor" || c.rfind("func_physbox", 0) == 0 ||
	       c == "prop_exploding_futbol" || c == "prop_glass_futbol";
}

// (bridge #50) What cannot be mined, by what the entity IS, not where it is or whether it moves (a resting cube has velocity 0 like a closed door): physics
// props and NPC-like things, anything Chell can pick up, held things. The rule, one for every map:
//   * the class family: prop_physics*, physics_prop, simple_physics_prop, func_physbox*, prop_weighted_cube, prop_monster_box, prop_energy_ball, the futbols,
//     prop_paint_bomb, personality cores, npc_* (turrets, rattmann...), weapon_* and item_*;
//   * the engine's movetype: MOVETYPE_VPHYSICS (6) is simulated by the physics engine (every cube and prop a player can pick up), MOVETYPE_STEP/FLY/WALK
//     (NPCs) too;
//   * the model's family, for decorations built as props that stand for one of those (a turret model on a prop_dynamic): models/npcs/, turret, personality,
//     glados core and ball, metal_box, reflection_cube, weighted_cube, futbol, portalgun.
// Doors, panels, lifts, platforms, arms, elevator cars, buttons stay minable (movetype NONE / PUSH, solid animated models and brushes).
static bool HasWord(const std::string &s, const char *w) { return s.find(w) != std::string::npos; }
static const char *NoMineReason(void *e, const std::string &c, const std::string &model) {
	if (IsPhysicsClass(c)) return "physics class";
	static const char *const kClassPrefixes[] = {"prop_physics", "physics_prop", "simple_physics_prop", "func_physbox", "prop_weighted_cube", "prop_monster_box", "prop_energy_ball", "prop_exploding_futbol",
	                                             "prop_glass_futbol", "prop_paint_bomb", "prop_glados_core", "prop_personality", "npc_", "weapon_", "item_"};
	for (const char *p : kClassPrefixes)
		if (c.rfind(p, 0) == 0) return "physics prop / NPC class";
	// (a bone follower and the brush entities are the engine's own animated geometry: their movetype says nothing about being a prop)
	const bool geometry = c == "phys_bone_follower" || c.rfind("func_", 0) == 0 || c == "prop_testchamber_door" || c == "projected_wall_entity";
	const int mt = geometry ? 0 : EntMoveType(e);
	if (mt == 6 /* VPHYSICS */ || mt == 3 /* STEP */ || mt == 4 /* FLY */ || mt == 2 /* WALK */ || mt == 5 /* FLYGRAVITY */) return "physics / NPC movetype";
	std::string m = model;
	for (char &ch : m) ch = (char)std::tolower((unsigned char)ch);
	if (m.rfind("models/npcs/", 0) == 0 || HasWord(m, "turret") || HasWord(m, "personality") || HasWord(m, "glados_core") || HasWord(m, "glados_ball") || HasWord(m, "metal_box") ||
	    HasWord(m, "reflection_cube") || HasWord(m, "weighted_cube") || HasWord(m, "futbol") || HasWord(m, "portalgun") || HasWord(m, "portal_gun"))
		return "physics / NPC model";
	return nullptr;
}

// ---- entity fields -------------------------------------------------------------------------------------------------
static Vector AbsOrigin(void *e) {
	static int off = -2;
	if (off == -2) off = EntFieldOffset(e, "m_vecAbsOrigin");
	if (off < 0) return EntVec(e, "origin");
	const float *f = (const float *)((const char *)e + off);
	return Vector(f[0], f[1], f[2]);
}
// (bridge #37) no parent by keyvalue AND none in the engine (a projected wall is parented by code, with no parentname: its local angles are
// relative to the emitter, not absolute)
static bool IsUnparented(void *e) {
	if (!EntKV(e, "parentname").empty()) return false;
	static int off = -2;
	if (off == -2) off = EntFieldOffset(e, "m_pParent");
	if (off < 0) return true;
	const unsigned h = *(const unsigned *)((const char *)e + off);
	return h == 0xFFFFFFFFu || h == 0;
}
static QAngle AbsAngles(void *e, bool unparented) {
	static int off = -2, offLocal = -2;
	if (off == -2) off = EntFieldOffset(e, "m_angAbsRotation"), offLocal = EntFieldOffset(e, "m_angRotation");
	QAngle a{0, 0, 0};
	// (bridge #32) m_angAbsRotation is recomputed lazily, when something asks for GetAbsAngles: a func_door_rotating that turns by itself
	// kept 0 there (the testbed's tilting platform "never tilts"). An unparented entity's local angles (m_angRotation, kept current by
	// the entity's own movement) are its absolute ones.
	const int o = unparented && offLocal >= 0 ? offLocal : off;
	if (o >= 0) {
		const float *f = (const float *)((const char *)e + o);
		a = QAngle{f[0], f[1], f[2]};
	} else {
		const Vector v = EntVec(e, "angles");
		a = QAngle{v.x, v.y, v.z};
	}
	if (!std::isfinite(a.x + a.y + a.z)) a = QAngle{0, 0, 0};
	return a;
}
// world rotation (Source axes; columns = the local forward / left / up axes in world space, as the SDK's AngleMatrix)
static void AnglesMatrix(const QAngle &a, float m[3][3]) {
	const float d = 3.14159265f / 180.0f;
	const float sp = std::sin(a.x * d), cp = std::cos(a.x * d), sy = std::sin(a.y * d), cy = std::cos(a.y * d), sr = std::sin(a.z * d), cr = std::cos(a.z * d);
	m[0][0] = cp * cy, m[1][0] = cp * sy, m[2][0] = -sp;
	m[0][1] = sr * sp * cy - cr * sy, m[1][1] = sr * sp * sy + cr * cy, m[2][1] = sr * cp;
	m[0][2] = cr * sp * cy + sr * sy, m[1][2] = cr * sp * sy - sr * cy, m[2][2] = cr * cp;
}
static Vector Rot(const float m[3][3], const Vector &v) {
	return Vector(m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z, m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z);
}
static Vector RotT(const float m[3][3], const Vector &v) {
	return Vector(m[0][0] * v.x + m[1][0] * v.y + m[2][0] * v.z, m[0][1] * v.x + m[1][1] * v.y + m[2][1] * v.z, m[0][2] * v.x + m[1][2] * v.y + m[2][2] * v.z);
}
// the world AABB half extents of a local box with half extents h (|R| h)
static Vector AbsRot(const float m[3][3], const Vector &h) {
	return Vector(std::fabs(m[0][0]) * h.x + std::fabs(m[0][1]) * h.y + std::fabs(m[0][2]) * h.z, std::fabs(m[1][0]) * h.x + std::fabs(m[1][1]) * h.y + std::fabs(m[1][2]) * h.z,
	              std::fabs(m[2][0]) * h.x + std::fabs(m[2][1]) * h.y + std::fabs(m[2][2]) * h.z);
}
static bool BoundsLocal(void *ent, Vector &mn, Vector &mx) {
	static int offMin = -2, offMax = -2;
	if (offMin == -2) offMin = EntFieldOffset(ent, "m_vecMins"), offMax = EntFieldOffset(ent, "m_vecMaxs");
	if (offMin < 0 || offMax < 0) return false;
	mn = *(const Vector *)((const char *)ent + offMin), mx = *(const Vector *)((const char *)ent + offMax);
	return std::isfinite(mn.x + mx.x + mn.y + mx.y + mn.z + mx.z) && mx.x > mn.x && mx.y > mn.y && mx.z > mn.z && (mx - mn).Length() < 4096.0f;
}
// solid now: a solid type and not FSOLID_NOT_SOLID (a func_brush Disabled, a door's blocker turned off, a prop with collision off)
static bool SolidNow(void *e) {
	static int offFlags = -2, offType = -2;
	if (offFlags == -2) offFlags = EntFieldOffset(e, "m_usSolidFlags"), offType = EntFieldOffset(e, "m_nSolidType");
	const int type = offType >= 0 ? (int)*((const unsigned char *)e + offType) : 6;
	const int flags = offFlags >= 0 ? (int)*(const unsigned short *)((const char *)e + offFlags) : 0;
	return type != 0 && !(flags & 0x4);
}

// ---- traces against one entity --------------------------------------------------------------------------------------
struct OneFilter : public ITraceFilter {
	void *t = nullptr;
	virtual bool ShouldHitEntity(void *e, int) { return e == t; }
	virtual TraceType_t GetTraceType() const { return TRACE_ENTITIES_ONLY; }
};
static int g_traces = 0;
static bool BoxHitsEnt(void *ent, const Vector &centre, const Vector &half) {
	OneFilter filter;
	filter.t = ent;
	Ray_t ray;
	ray.m_Delta = VectorAligned(0, 0, 0);
	ray.m_IsSwept = false;
	ray.m_Extents = VectorAligned(half.x, half.y, half.z);
	ray.m_IsRay = half.x < 1e-3f && half.y < 1e-3f && half.z < 1e-3f;
	ray.m_StartOffset = VectorAligned(0, 0, 0);
	ray.m_Start = VectorAligned(centre.x, centre.y, centre.z);
	ray.m_pWorldAxisTransform = nullptr;
	CGameTrace tr;
	std::memset(&tr, 0, sizeof tr);
	engine.TraceRay(engine.traceServer->ThisPtr(), ray, MASK_PLAYERSOLID, &filter, &tr);
	++g_traces;
	return tr.startsolid || tr.allsolid;
}

// ---- shapes ----------------------------------------------------------------------------------------------------------
static const float kCell = 5.0f;          // Source units
static const uint32_t kShapeMagic = 0x534D4350, kShapeVersion = 1;  // "PCMS"
struct Shape {
	uint32_t id = 0;
	std::string key;
	std::vector<ColMoverBox> boxes;  // MC blocks, mover-local
	Vector smn{0, 0, 0}, smx{0, 0, 0};  // the boxes' Source-local bounds
	uint32_t sentEpoch = 0;
	int traces = 0;
	double ms = 0;
	bool fromDisk = false;
	// (bridge #43) the dig lattice: a shape rebuilt after a dig (the original minus the dug cells) has `base` = the original and keeps its anchor
	uint32_t base = 0;
	float anchor[3] = {0, 0, 0};  // MC blocks, mover-local: the corner of the original's bounds
	bool hasAnchor = false;
};
static std::vector<Shape> g_shapes;                         // index = id - 1
PCB_GAUGE("movers.shapes", g_shapes.size());
static std::unordered_map<std::string, uint32_t> g_shapeByKey;
static std::unordered_set<std::string> g_diskMiss;          // keys looked up on disk and not there
PCB_GAUGE("movers.diskMiss", g_diskMiss.size());
static std::string ShapePath(const std::string &key) {
	uint64_t h = 1469598103934665603ull;
	for (unsigned char c : key) h = (h ^ c) * 1099511628211ull;
	char b[40];
	std::snprintf(b, sizeof b, "%016llx.bin", (unsigned long long)h);
	return CacheDir() + "/movershapes/" + b;
}
static void ShapeSave(const Shape &s) {
	if (s.boxes.empty()) return;  // (an empty shape is rebuilt each session: a transient miss must not stick in the cache)
	plat::MakeDir(CacheDir() + "/movershapes");
	const std::string path = ShapePath(s.key), tmp = path + ".tmp";
	FILE *f = std::fopen(tmp.c_str(), "wb");
	if (!f) return;
	const uint32_t hdr[3] = {kShapeMagic, kShapeVersion, (uint32_t)s.key.size()};
	std::fwrite(hdr, sizeof hdr, 1, f);
	std::fwrite(s.key.data(), 1, s.key.size(), f);
	const float b[6] = {s.smn.x, s.smn.y, s.smn.z, s.smx.x, s.smx.y, s.smx.z};
	std::fwrite(b, sizeof b, 1, f);
	const uint32_t n = (uint32_t)s.boxes.size();
	std::fwrite(&n, 4, 1, f);
	if (n) std::fwrite(s.boxes.data(), sizeof(ColMoverBox), n, f);
	std::fclose(f);
	std::rename(tmp.c_str(), path.c_str());
}
static uint32_t ShapeAdd(Shape s) {
	s.id = (uint32_t)g_shapes.size() + 1;
	g_shapeByKey[s.key] = s.id;
	g_shapes.push_back(std::move(s));
	return g_shapes.back().id;
}
static uint32_t ShapeLoad(const std::string &key) {
	if (g_diskMiss.count(key)) return 0;
	FILE *f = std::fopen(ShapePath(key).c_str(), "rb");
	if (!f) return g_diskMiss.insert(key), 0;
	uint32_t hdr[3] = {0, 0, 0}, n = 0;
	Shape s;
	bool ok = std::fread(hdr, sizeof hdr, 1, f) == 1 && hdr[0] == kShapeMagic && hdr[1] == kShapeVersion && hdr[2] < 4096;
	std::string k(ok ? hdr[2] : 0, '\0');
	float b[6];
	ok = ok && (k.empty() || std::fread(&k[0], 1, k.size(), f) == k.size()) && k == key && std::fread(b, sizeof b, 1, f) == 1 && std::fread(&n, 4, 1, f) == 1 && n < 200000;
	if (ok) {
		s.boxes.resize(n);
		ok = !n || std::fread(s.boxes.data(), sizeof(ColMoverBox), n, f) == n;
	}
	std::fclose(f);
	if (!ok || s.boxes.empty()) return g_diskMiss.insert(key), 0;
	s.key = key, s.smn = Vector(b[0], b[1], b[2]), s.smx = Vector(b[3], b[4], b[5]), s.fromDisk = true;
	return ShapeAdd(std::move(s));
}

// ---- movers ----------------------------------------------------------------------------------------------------------
struct Mover {
	void *ent = nullptr;
	uint32_t id = 0;
	std::string cls, name, key;
	bool physics = false;
	bool invisible = false, classified = false;  // a clip-only / tool-material brush entity: never exported (bridge #23's rule)
	int classifyTries = 0;                        // (a ray through its box that misses it: tried on 3 frames, then taken as visible)
	uint32_t shape = 0;  // its shape once built or loaded (0: not yet)
	uint32_t bound = 0;  // the shape Minecraft has for it (the last kColMoverBind sent)
	uint32_t boundEpoch = 0;
	bool solid = true, held = false, listed = false;
	bool unparented = true;    // (bridge #32) no parentname: its local angles are its absolute ones (AbsAngles)
	bool passable = false;     // Portal 2's player stands inside its solid (a train brush enclosing an elevator car): not exported
	double insideSince = -1;   // since when the player's hull (Portal 2 moving it) has been inside it
	double clearSince = -1;    // (lead) since when the hull has been clear of a passable mover (exported again after 1 s)
	Vector origin{0, 0, 0}, lastOrigin{0, 0, 0}, vel{0, 0, 0};
	QAngle ang{0, 0, 0}, lastAng{0, 0, 0};
	float rot[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
	Vector lmn{-16, -16, -16}, lmx{16, 16, 16};
	float radius = 32.0f;
	double movedAt = -1e9, posAt = -1e9, seenAt = 0;
	std::vector<std::pair<double, Vector>> hist;  // (bridge #26) the server origin at each change (host time): its render pose
	// (bridge #39) a slab class's pose, resolved by probing the entity itself (ResolveSlabPose): the engine's own angle fields do not always say how its
	// collision is turned (the light bridge: Chell stands on it, the fields said yaw 270, the slab lies along yaw 0)
	bool rotFixed = false;       // m.rot is the probed one, not the engine's angles
	QAngle rotFixedFor{0, 0, 0};  // the engine angles it was probed for (a change re-probes)
	double slabCheckAt = -1;     // next probe time
	int slabHits = 0;            // the chosen pose's probe hits (of kSlabProbes)
	// (bridge #43) holes the Minecraft player mined in this mover: cells of its shape's dig lattice (1-block cubes of its own frame). They live as long as
	// this entity does (a new level starts a new list), and move with it. dugShape is its shape minus them (0: none dug).
	// (bridge #43, 2nd) the holes are CUTS: boxes of its own frame (MC blocks, in thousandths: lo xyz, hi xyz). A mined cell of the lattice is one; a cell mined in
	// a mover that shares the space (a door's brush, its model, its clip brushes) is carried into every other mover that moves with it as the same
	// world box in that mover's own frame, so the space opens through all of them.
	std::set<std::array<int, 6>> dug;
	uint32_t dugShape = 0;
	bool dugStale = false;  // dug changed (or the shape did): dugShape and the picture's boxes are rebuilt
	std::vector<std::pair<Vector, Vector>> holeBoxes;  // what the dug cells took out of the shape (Source local boxes): the picture's hole
	// (bridge #50) what the hole's walls are made of (the material kind each cut was mined as) and the walls themselves (moverwalls_geom.hpp)
	std::map<std::array<int, 6>, int> cutKind;
	std::shared_ptr<const MoverWallSet> walls;
	// (bridge #50) never minable: a physics prop, an NPC, anything Chell can pick up (see NoMineReason)
	bool noMine = false;
};
static void DugReset(Mover &m);  // (bridge #43, below)
static std::vector<Mover> g_movers;
PCB_GAUGE("movers.movers", g_movers.size());
static std::unordered_map<void *, size_t> g_moverByEnt;
static std::unordered_set<void *> g_live;
static uint32_t g_nextMoverId = 1;
static double g_listAt = -1e9, g_levelAt = 0, g_firstListAt = -1e9;
static double g_s40BindDelay = 0;
static bool g_on = true;              // pc_movers
static double g_buildMs = 1.5, g_buildUrgentMs = 8.0;  // (lead: never a long frame mid-play; most shapes come from the disk cache)
static bool g_urgent = false;
static double g_p2MovesSince = -1, g_p2Watched = 0;  // (this epoch: how long Portal 2 has moved the body while movers were checked)
static float g_poseRadius = 960.0f;  // ~3 regions
static uint64_t g_frame = 0;
static int g_shapesBuilt = 0, g_shapesLoaded = 0, g_bindsSent = 0, g_shapesSent = 0, g_posesLast = 0;
static double g_buildMsTotal = 0;
static Vector g_focus(0, 0, 0);

// (bridge #37) entities whose collision is a thin slab (a vphysics mesh a box trace barely finds: the hard light bridge, 64 u wide and 1/64 u
// thick; the testbed's build found one 40x35 u patch of its 64x248 u slab by tracing, and the Minecraft player fell through the rest). The
// shape is the entity's own bounds, as one box, with no traces.
static bool IsSlabClass(const std::string &cls) { return cls == "projected_wall_entity"; }

// the entity's model name (m_ModelName is a pooled string: the keyvalue route returns its pointer's bytes)
static std::string ModelName(void *e) {
	static int off = -2;
	if (off == -2) off = EntFieldOffset(e, "m_ModelName");
	if (off < 0) return "";
	const char *p = *(const char *const *)((const char *)e + off);
	if (!p) return "";
	std::string s;
	for (int i = 0; i < 200 && p[i]; ++i) {
		if ((unsigned char)p[i] < 32 || (unsigned char)p[i] > 126) return "";
		s += p[i];
	}
	return s;
}
static std::string ShapeKey(void *e, const std::string &cls, const Vector &mn, const Vector &mx) {
	const std::string model = ModelName(e);
	char b[160];
	std::snprintf(b, sizeof b, "|%.0f %.0f %.0f %.0f %.0f %.0f", mn.x, mn.y, mn.z, mx.x, mx.y, mx.z);
	// (bridge #34) a brush model's shape is the map's: keyed by the map's version too, so a rebuilt BSP (the same model numbers and bounds, other geometry)
	// does not get the old build's shape from cache/bridge/movershapes
	if (!model.empty() && model[0] == '*') {
		std::string ver = MapVersionKey(McMapName());
		return std::string(McMapName()) + (ver.empty() ? "" : "@" + ver.substr(0, 8)) + ":" + model + b;
	}
	if (IsSlabClass(cls)) return "slab:" + cls + b;  // (bridge #37) analytic: the same bounds, the same shape
	if (model.empty()) return cls + "|" + std::to_string((uintptr_t)e) + b;  // (no model name: not shared, not cached usefully)
	std::string k = (model.empty() ? cls : model) + "|s" + EntKV(e, "skin") + "|b" + EntKV(e, "body");
	if (cls == "phys_bone_follower") {
		static int off = -2;
		if (off == -2) off = EntFieldOffset(e, "m_solidIndex");
		k += "|f" + std::to_string(off >= 0 ? *(const int *)((const char *)e + off) : -1);
	}
	for (char &c : k) c = (char)std::tolower((unsigned char)c);
	return k + b;
}

// (bridge #23's rule) a brush entity whose contents are playerclip only, or both of whose faces are tool materials other than
// nodraw (toolsclip, toolsinvisible, toolsplayerclip...): Valve's invisible blocker, not geometry. Never exported.
static void Classify(Mover &m) {
	if (m.classified) return;
	// (only brush entities are drawn with tool materials or made of clip; a model's collision is its model's)
	if (m.key.find(":*") == std::string::npos || ++m.classifyTries > 3) return void(m.classified = true);
	Vector mn, mx;
	if (!BoundsLocal(m.ent, mn, mx)) return void(m.classified = true);
	OneFilter filter;
	filter.t = m.ent;
	const Vector o = m.origin;
	Vector wmn(1e9, 1e9, 1e9), wmx(-1e9, -1e9, -1e9);
	const Vector c = o + Rot(m.rot, (mn + mx) * 0.5f), h = AbsRot(m.rot, (mx - mn) * 0.5f);
	const Vector a = c - h - Vector(1, 1, 1), b = c + h + Vector(1, 1, 1);
	Ray_t ray;
	ray.m_Delta = VectorAligned(b.x - a.x, b.y - a.y, b.z - a.z);
	ray.m_IsSwept = true, ray.m_IsRay = true;
	ray.m_Extents = VectorAligned(0, 0, 0);
	ray.m_StartOffset = VectorAligned(0, 0, 0);
	ray.m_Start = VectorAligned(a.x, a.y, a.z);
	ray.m_pWorldAxisTransform = nullptr;
	CGameTrace tr;
	std::memset(&tr, 0, sizeof tr);
	engine.TraceRay(engine.traceServer->ThisPtr(), ray, MASK_ALL, &filter, &tr);
	if (tr.fraction >= 1.0f) return;  // (not solid now: looked at again on the next listing)
	m.classified = true;
	auto isTool = [](std::string s) {
		for (char &ch : s) ch = (char)std::tolower((unsigned char)ch);
		return s.rfind("tools/", 0) == 0 && s.find("nodraw") == std::string::npos;
	};
	const bool clipOnly = !(tr.contents & MASK_SOLID) && (tr.contents & (CONTENTS_PLAYERCLIP | 0x20000 /* CONTENTS_MONSTERCLIP */));
	bool tool = isTool(tr.surface.name ? tr.surface.name : "");
	std::string surf = tr.surface.name ? tr.surface.name : "-";
	if (tool) {
		Ray_t back = ray;
		back.m_Start = VectorAligned(b.x, b.y, b.z);
		back.m_Delta = VectorAligned(a.x - b.x, a.y - b.y, a.z - b.z);
		CGameTrace tr2;
		std::memset(&tr2, 0, sizeof tr2);
		engine.TraceRay(engine.traceServer->ThisPtr(), back, MASK_ALL, &filter, &tr2);
		tool = tr2.fraction < 1.0f && isTool(tr2.surface.name ? tr2.surface.name : "");
	}
	m.invisible = clipOnly || tool;
	if (m.invisible) {
		// (#23's rule, as for the world's playerclip: an invisible blocker is kept only where it is a floor over nothing, the
		// floor Portal 2's player walks on there: pc_pit_flings's arrival_elevator-floor_clip outside the elevator door)
		m.key += "|floor-over-nothing", m.shape = 0, DugReset(m);
		console->Print("[bridge] movers: %s %s is invisible (contents 0x%x, surface %s): only its floor over nothing is exported\n", m.cls.c_str(), m.name.c_str(), tr.contents,
		               surf.c_str());
	}
}

static void UpdatePose(Mover &m, double now) {
	const Vector o = AbsOrigin(m.ent);
	const QAngle a = AbsAngles(m.ent, m.unparented);
	const bool moved = (o - m.origin).Length() > 0.01f || std::fabs(a.x - m.ang.x) + std::fabs(a.y - m.ang.y) + std::fabs(a.z - m.ang.z) > 0.01f;
	if (moved) {
		m.vel = now - m.posAt < 0.5 && now > m.posAt ? (o - m.origin) * (float)(1.0 / (now - m.posAt)) : Vector(0, 0, 0);
		m.lastOrigin = m.origin, m.lastAng = m.ang;
		if (m.hist.empty() || now - m.hist.back().first > 0.5) m.hist.push_back({now - 1e-3, m.origin});  // (it rested here until now)
		m.hist.push_back({now, o});
		if (m.hist.size() > 64) m.hist.erase(m.hist.begin(), m.hist.begin() + 32);
		m.origin = o, m.ang = a, m.movedAt = m.posAt = now;
		// (bridge #39) a probed pose stays while the engine's angles are the ones it was probed for
		if (m.rotFixed && std::fabs(a.x - m.rotFixedFor.x) + std::fabs(a.y - m.rotFixedFor.y) + std::fabs(a.z - m.rotFixedFor.z) > 0.01f) m.rotFixed = false, m.slabCheckAt = -1;
		if (!m.rotFixed) AnglesMatrix(a, m.rot);
	} else if (now - m.posAt > 0.5) {
		m.vel = Vector(0, 0, 0);
	}
}

// (bridge #39) The light bridge's pose, found by looking at the entity instead of trusting its angle fields. Chell walks on the bridge by Portal 2's own collision;
// the Minecraft player fell through it because the slab was posed along the wrong axis: tbtest full4's first shape build (traces in the pose from m_angRotation)
// found only a 32 x 32 u patch of the 64 x 248 u slab, near its origin = the overlap of two slabs 90 degrees apart (the fields said the emitter's yaw 270,
// the collision lies along yaw 0 from the same origin), and after #37 (m_angAbsRotation when m_pParent is set) the player still fell. So for a slab class the
// pose is the one under which the entity's own collision answers: a few small boxes along the slab's long centre line are traced against the entity for each
// candidate turn (the engine's absolute angles, its local angles, the world axes, the three other quarter turns about up), and the candidate with the most
// hits is the pose (the engine's first on a tie). It is looked at again whenever the entity is new, its bounds change, its angles change, or the pose
// stops hitting; a pose that hits is re-verified every 2 s.
static const int kSlabProbes = 5;
static int SlabProbeHits(const Mover &m, const float R[3][3]) {
	// the long axis of the local bounds is the one the probes run along; the thin axis is probed at its middle with a 3 u half extent
	const Vector ext = m.lmx - m.lmn;
	int longAxis = 0, thinAxis = 0;
	for (int i = 1; i < 3; ++i) {
		if (ext[i] > ext[longAxis]) longAxis = i;
		if (ext[i] < ext[thinAxis]) thinAxis = i;
	}
	if (longAxis == thinAxis) return 0;
	int hits = 0;
	for (int k = 0; k < kSlabProbes; ++k) {
		const float t = (k + 0.5f) / kSlabProbes;
		Vector local = (m.lmn + m.lmx) * 0.5f;
		local[longAxis] = m.lmn[longAxis] + ext[longAxis] * t;
		const Vector c = m.origin + Rot(R, local);
		if (BoxHitsEnt(m.ent, c, Vector(3, 3, 3))) ++hits;
	}
	return hits;
}
static void ResolveSlabPose(Mover &m, double now) {
	if (now < m.slabCheckAt || !m.solid) return;
	const int cur = SlabProbeHits(m, m.rot);
	if (cur >= kSlabProbes - 1) {  // (one probe may fall on a gap in the beam where a cube cuts it)
		m.slabHits = cur, m.slabCheckAt = now + 2.0;
		return;
	}
	struct Cand {
		const char *what;
		QAngle a;
	};
	const QAngle abs = AbsAngles(m.ent, false), loc = AbsAngles(m.ent, true);
	const Cand cands[] = {{"engine absolute angles", abs}, {"engine local angles", loc}, {"world axes (yaw 0)", QAngle{0, 0, 0}}, {"yaw 90", QAngle{0, 90, 0}},
	                      {"yaw 180", QAngle{0, 180, 0}}, {"yaw 270", QAngle{0, 270, 0}}};
	int best = -1, bestHits = cur;
	float bestR[3][3];
	for (int i = 0; i < (int)(sizeof cands / sizeof cands[0]); ++i) {
		float R[3][3];
		AnglesMatrix(cands[i].a, R);
		const int h = SlabProbeHits(m, R);
		if (h > bestHits) best = i, bestHits = h, std::memcpy(bestR, R, sizeof R);
	}
	// nothing hits yet (a beam still growing out of its emitter, a beam off): look again soon; a pose that hits better is taken
	m.slabCheckAt = now + (bestHits >= kSlabProbes - 1 ? 2.0 : 0.25);
	if (best < 0) return void(m.slabHits = cur);
	std::memcpy(m.rot, bestR, sizeof bestR);
	m.rotFixed = true, m.rotFixedFor = m.ang, m.slabHits = bestHits;
	static int off = -2;
	if (off == -2) off = EntFieldOffset(m.ent, "m_pParent");
	console->Print(
	    "[bridge] movers: %s %s #%u pose resolved by probing: %s (%d of %d probe boxes hit it; the pose in use hit %d); engine absolute angles (%.1f %.1f %.1f), local (%.1f %.1f %.1f), "
	    "parent handle %s, origin (%.1f %.1f %.1f), local bounds (%.1f %.1f %.1f)..(%.1f %.1f %.1f)\n",
	    m.cls.c_str(), m.name.c_str(), m.id, cands[best].what, bestHits, kSlabProbes, cur, abs.x, abs.y, abs.z, loc.x, loc.y, loc.z,
	    off < 0 ? "unknown" : (*(const unsigned *)((const char *)m.ent + off) == 0xFFFFFFFFu || *(const unsigned *)((const char *)m.ent + off) == 0 ? "none" : "set"), m.origin.x,
	    m.origin.y, m.origin.z, m.lmn.x, m.lmn.y, m.lmn.z, m.lmx.x, m.lmx.y, m.lmx.z);
}

static std::unordered_map<void *, std::string> g_classOf;  // (a pointer's classname, asked once: the listing was 2+ ms a second)
PCB_GAUGE("movers.classOf", g_classOf.size());
static void Refresh(double now) {
	PROF("movers.list");
	std::unordered_set<void *> seen;
	for (auto it = g_classOf.begin(); it != g_classOf.end();)
		if (!g_live.count(it->first)) it = g_classOf.erase(it);
		else ++it;
	for (void *e : g_live) {
		auto ci = g_classOf.find(e);
		if (ci == g_classOf.end()) ci = g_classOf.emplace(e, EntKV(e, "classname")).first;
		const std::string &c = ci->second;
		bool want = false;
		for (const char *k : kMoverClasses) want |= c == k;
		// (the user: "only map real geometry instead of hitboxes": clips and triggers never collide in Minecraft)
		if (c.find("clip") != std::string::npos || c.rfind("trigger_", 0) == 0) want = false;
		if (want && c == "prop_dynamic_override" && IsOwnCollider(EntKV(e, "targetname"))) want = false;  // (bridge #65)
		if (!want) continue;
		seen.insert(e);
		auto it = g_moverByEnt.find(e);
		if (it != g_moverByEnt.end() && g_movers[it->second].cls == c) {
			Mover &em = g_movers[it->second];
			em.name = EntKV(e, "targetname");
			em.unparented = IsUnparented(e);
			// (bridge #31) a solid whose own bounds change (a light bridge growing out of its emitter, a resized brush): a new shape for the new bounds
			Vector bmn, bmx;
			if (!em.physics && BoundsLocal(e, bmn, bmx) && ((bmn - em.lmn).Length() > 2.0f || (bmx - em.lmx).Length() > 2.0f)) {
				em.lmn = bmn, em.lmx = bmx, em.radius = std::fmax(bmn.Length(), bmx.Length());
				em.key = ShapeKey(e, c, bmn, bmx);
				em.shape = 0, em.classified = false, em.classifyTries = 0;
				DugReset(em);  // (bridge #43) other bounds, other lattice
				em.slabCheckAt = -1;  // (bridge #39) a new length: the pose is probed again
			}
			continue;
		}
		if (it != g_moverByEnt.end()) g_movers[it->second].ent = nullptr;  // (the pointer is a new entity now: a new mover)
		Mover m;
		m.ent = e, m.id = g_nextMoverId++, m.cls = c, m.name = EntKV(e, "targetname"), m.physics = IsPhysicsClass(c), m.seenAt = now;
		{
			const char *why = NoMineReason(e, c, ModelName(e));
			m.noMine = why != nullptr;
			if (why) console->Print("[bridge] movers: %s %s #%u is not minable (%s)\n", c.c_str(), m.name.c_str(), m.id, why);
		}
		m.unparented = IsUnparented(e);
		m.origin = m.lastOrigin = AbsOrigin(e), m.ang = m.lastAng = AbsAngles(e, m.unparented);
		AnglesMatrix(m.ang, m.rot);
		Vector mn, mx;
		if (BoundsLocal(e, mn, mx)) m.lmn = mn, m.lmx = mx;
		else m.lmn = Vector(-128, -128, -128), m.lmx = Vector(128, 128, 128);
		m.radius = std::fmax(m.lmn.Length(), m.lmx.Length());
		m.key = ShapeKey(e, c, m.lmn, m.lmx);
		g_moverByEnt[e] = g_movers.size();
		g_movers.push_back(std::move(m));
	}
	for (Mover &m : g_movers)
		if (m.ent && !seen.count(m.ent)) m.ent = nullptr;
	if (g_firstListAt < g_levelAt) g_firstListAt = now;
	g_listAt = now;
}

// ---- shape building (coarse to fine, sliced over frames) -----------------------------------------------------------
struct Build {
	bool active = false;
	std::string key;
	void *ent = nullptr;
	uint32_t moverId = 0;
	Vector gmn;            // grid min, Source local
	int n[3] = {0, 0, 0};
	int up = 2, upSign = 1;  // the local axis nearest world up when the build started (tops are refined along it)
	std::vector<uint8_t> occ;  // 0 empty, 1 full, 2.. a top cell solid up to (v - 1) * 0.25 u along up
	std::vector<std::array<int, 4>> stack;
	std::vector<size_t> tops;
	size_t topNext = 0;
	int phase = 0;  // 0 fill, 1 refine tops, 3 floor-over-nothing filter (invisible blockers), 2 done
	bool floorOnly = false;
	int column = 0;
	int traces = 0;
	double ms = 0, startedAt = 0;
};
static Build g_build;
static size_t At(const Build &b, int x, int y, int z) { return ((size_t)z * b.n[1] + y) * b.n[0] + x; }
static Mover *MoverById(uint32_t id) {
	for (Mover &m : g_movers)
		if (m.id == id && m.ent) return &m;
	return nullptr;
}
static void BuildStart(Mover &m) {
	Build b;
	// (bridge #32) the "floor over nothing" filter is for invisible blockers that stand ON a floor (a playerclip floor, pc_pit_flings's
	// arrival_elevator-floor_clip). A train, door or lift whose own brush is invisible (the departure car's toolsinvisible shell) IS the
	// geometry it carries: its lowest cell lies well under the walking surface, and a shaft under 160 u deep put a world floor "under" it, so
	// the filter emptied the shape and the Minecraft player fell through the car (pc_testbed departure, tbtest full1).
	const bool carriesGeometry = m.cls == "func_tracktrain" || m.cls == "func_train" || m.cls == "func_tanktrain" || m.cls == "func_movelinear" || m.cls == "func_door" ||
	                             m.cls == "func_door_rotating" || m.cls == "func_platrot" || m.cls == "func_rotating";
	b.active = true, b.key = m.key, b.ent = m.ent, b.moverId = m.id, b.startedAt = NowS(), b.floorOnly = m.invisible && !carriesGeometry;
	b.gmn = m.lmn - Vector(1, 1, 1);
	const Vector ext = m.lmx - m.lmn + Vector(2, 2, 2);
	for (int i = 0; i < 3; ++i) b.n[i] = std::max(1, (int)std::ceil(ext[i] / kCell));
	b.occ.assign((size_t)b.n[0] * b.n[1] * b.n[2], 0);
	for (int z = 0; z < b.n[2]; z += 8)
		for (int y = 0; y < b.n[1]; y += 8)
			for (int x = 0; x < b.n[0]; x += 8) b.stack.push_back({x, y, z, 8});
	float best = -1;
	for (int k = 0; k < 3; ++k)
		if (std::fabs(m.rot[2][k]) > best) best = std::fabs(m.rot[2][k]), b.up = k, b.upSign = m.rot[2][k] >= 0 ? 1 : -1;
	g_build = std::move(b);
}
// the greedy merge: rows in x, rectangles in y, stacks in z; equal cell values only
static void BuildFinish(Build &b) {
	struct Rect {
		int x0, x1, y0, y1, z0, z1;
		uint8_t v;
	};
	std::vector<Rect> done;
	std::map<std::array<int, 5>, size_t> open;
	std::vector<uint8_t> used(b.occ.size(), 0);
	for (int z = 0; z < b.n[2]; ++z) {
		std::map<std::array<int, 5>, size_t> next;
		for (int y = 0; y < b.n[1]; ++y)
			for (int x = 0; x < b.n[0]; ++x) {
				const uint8_t v = b.occ[At(b, x, y, z)];
				if (!v || used[At(b, x, y, z)]) continue;
				int x1 = x;
				while (x1 + 1 < b.n[0] && b.occ[At(b, x1 + 1, y, z)] == v && !used[At(b, x1 + 1, y, z)]) ++x1;
				int y1 = y;
				for (;;) {
					if (y1 + 1 >= b.n[1]) break;
					bool row = true;
					for (int xx = x; xx <= x1 && row; ++xx) row = b.occ[At(b, xx, y1 + 1, z)] == v && !used[At(b, xx, y1 + 1, z)];
					if (!row) break;
					++y1;
				}
				for (int yy = y; yy <= y1; ++yy)
					for (int xx = x; xx <= x1; ++xx) used[At(b, xx, yy, z)] = 1;
				const std::array<int, 5> key{x, x1, y, y1, v};
				auto it = open.find(key);
				if (it != open.end() && v == 1) done[it->second].z1 = z, next[key] = it->second;
				else done.push_back({x, x1, y, y1, z, z, v}), next[key] = done.size() - 1;
			}
		open.swap(next);
	}
	Shape s;
	s.key = b.key, s.traces = b.traces, s.ms = b.ms;
	s.smn = Vector(1e9, 1e9, 1e9), s.smx = Vector(-1e9, -1e9, -1e9);
	const float k = 1.0f / kUnitsPerBlock;
	for (const Rect &r : done) {
		Vector a = b.gmn + Vector(r.x0 * kCell, r.y0 * kCell, r.z0 * kCell), c = b.gmn + Vector((r.x1 + 1) * kCell, (r.y1 + 1) * kCell, (r.z1 + 1) * kCell);
		if (r.v >= 2) {  // a refined top cell: solid only this far along up
			const float e = (r.v - 1) * 0.25f;
			if (b.upSign > 0) c[b.up] = a[b.up] + e;
			else a[b.up] = c[b.up] - e;
		}
		for (int i = 0; i < 3; ++i) s.smn[i] = std::fmin(s.smn[i], a[i]), s.smx[i] = std::fmax(s.smx[i], c[i]);
		// Source local (x, y, z) -> Minecraft local (x, z, -y)
		s.boxes.push_back(ColMoverBox{a.x * k, a.z * k, -c.y * k, c.x * k, c.z * k, -a.y * k});
	}
	if (s.boxes.empty()) s.smn = s.smx = Vector(0, 0, 0);
	ShapeSave(s);
	const uint32_t id = ShapeAdd(std::move(s));
	++g_shapesBuilt;
	const Shape &sh = g_shapes[id - 1];
	const Mover *from = MoverById(b.moverId);
	console->Print("[bridge] movers: shape %u (%s) built from %s %s: %zu boxes from %d traces, %.1f ms over %.2f s\n", id, sh.key.c_str(), from ? from->cls.c_str() : "?",
	               from ? from->name.c_str() : "?", sh.boxes.size(), sh.traces, sh.ms, NowS() - b.startedAt);
}
static void BuildStep(double budgetMs) {
	Build &b = g_build;
	if (!b.active) return;
	PROF("movers.build");
	const auto t0 = std::chrono::steady_clock::now();
	const auto dl = t0 + std::chrono::microseconds((long long)(budgetMs * 1000.0));
	Mover *m = MoverById(b.moverId);
	if (!m || !g_live.count(m->ent)) {  // (it died: built from another mover with this key later)
		b.active = false;
		return;
	}
	const int t0traces = g_traces;
	// cells are placed with the mover's pose NOW (a mover moving while it is built stays in its own frame)
	auto world = [&](const Vector &local) { return m->origin + Rot(m->rot, local); };
	const Vector cellHalf(kCell * 0.5f, kCell * 0.5f, kCell * 0.5f);
	int n = 0;
	while (b.phase == 0 && !b.stack.empty()) {
		const auto c = b.stack.back();
		b.stack.pop_back();
		const int s = c[3];
		if (c[0] >= b.n[0] || c[1] >= b.n[1] || c[2] >= b.n[2]) continue;
		const Vector lc = b.gmn + Vector((c[0] + s * 0.5f) * kCell, (c[1] + s * 0.5f) * kCell, (c[2] + s * 0.5f) * kCell);
		const float h = s * kCell * 0.5f - 0.05f;
		if (BoxHitsEnt(m->ent, world(lc), AbsRot(m->rot, Vector(h, h, h)))) {
			bool full = false;
			if (s > 1) {  // wholly inside (its centre and 8 corners are in solid: brushes are convex): no need to split
				full = BoxHitsEnt(m->ent, world(lc), Vector(0, 0, 0));
				int k = 0;
				for (; k < 8 && full && std::chrono::steady_clock::now() < dl; ++k) full = BoxHitsEnt(m->ent, world(lc + Vector(k & 1 ? h - 0.2f : 0.2f - h, k & 2 ? h - 0.2f : 0.2f - h, k & 4 ? h - 0.2f : 0.2f - h)), Vector(0, 0, 0));
				if (full && k < 8) {  // (out of time mid-test: the cell again next frame)
					b.stack.push_back(c);
					break;
				}
			}
			if (s == 1 || full) {
				for (int z = c[2]; z < std::min(c[2] + s, b.n[2]); ++z)
					for (int y = c[1]; y < std::min(c[1] + s, b.n[1]); ++y)
						for (int x = c[0]; x < std::min(c[0] + s, b.n[0]); ++x) b.occ[At(b, x, y, z)] = 1;
			} else
				for (int k = 0; k < 8; ++k) b.stack.push_back({c[0] + (k & 1) * s / 2, c[1] + ((k >> 1) & 1) * s / 2, c[2] + ((k >> 2) & 1) * s / 2, s / 2});
		}
		++n;
		if (std::chrono::steady_clock::now() > dl) break;  // (every cell: a trace against a detailed model can take milliseconds)
	}
	if (b.phase == 0 && b.stack.empty()) {
		// the top cells (nothing above them along up): their solid part is found to 1/4 u (5 u cells put a floor up to 5 u
		// above Portal 2's: the Minecraft player stood in the air, or sank into a floor it was put on)
		b.phase = 1;
		for (int z = 0; z < b.n[2]; ++z)
			for (int y = 0; y < b.n[1]; ++y)
				for (int x = 0; x < b.n[0]; ++x) {
					if (!b.occ[At(b, x, y, z)]) continue;
					int q[3] = {x, y, z};
					q[b.up] += b.upSign;
					if (q[b.up] < 0 || q[b.up] >= b.n[b.up] || !b.occ[At(b, q[0], q[1], q[2])]) b.tops.push_back(At(b, x, y, z));
				}
	}
	while (b.phase == 1 && b.topNext < b.tops.size()) {
		const size_t i = b.tops[b.topNext++];
		const int x = (int)(i % b.n[0]), y = (int)((i / b.n[0]) % b.n[1]), z = (int)(i / ((size_t)b.n[0] * b.n[1]));
		const Vector base = b.gmn + Vector(x * kCell, y * kCell, z * kCell);
		float lo = 0, hi = kCell;  // solid reaches up to hi (from the cell's low side along up)
		for (int it = 0; it < 4; ++it) {
			const float mid = (lo + hi) * 0.5f;
			// the slab above mid (along up, inside the cell): any solid there?
			Vector smn = base, smx = base + Vector(kCell, kCell, kCell);
			if (b.upSign > 0) smn[b.up] = base[b.up] + mid;
			else smx[b.up] = base[b.up] + kCell - mid;
			Vector half = (smx - smn) * 0.5f - Vector(0.05f, 0.05f, 0.05f);
			for (int k = 0; k < 3; ++k) half[k] = std::fmax(half[k], 0.01f);
			if (BoxHitsEnt(m->ent, world((smn + smx) * 0.5f), AbsRot(m->rot, half))) lo = mid;
			else hi = mid;
		}
		const int q = std::max(1, std::min(20, (int)std::ceil(hi / 0.25f - 1e-3f)));
		b.occ[i] = q >= 20 ? 1 : (uint8_t)(1 + q);
		++n;
		if (std::chrono::steady_clock::now() > dl) break;
	}
	b.traces += g_traces - t0traces;
	b.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	g_buildMsTotal += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	if (b.phase == 1 && b.topNext >= b.tops.size()) b.phase = b.floorOnly ? 3 : 2;
	// (an invisible blocker) a column of cells is kept only if no world solid lies within 160 u under its lowest cell
	while (b.phase == 3 && b.column < b.n[0] * b.n[1]) {
		const int x = b.column % b.n[0], y = b.column / b.n[0];
		++b.column;
		int z0 = -1;
		for (int z = 0; z < b.n[2] && z0 < 0; ++z)
			if (b.occ[At(b, x, y, z)]) z0 = z;
		if (z0 < 0) continue;
		const Vector p = world(b.gmn + Vector((x + 0.5f) * kCell, (y + 0.5f) * kCell, z0 * kCell - 0.5f));
		CGameTrace tr;
		engine.Trace(p, p - Vector(0, 0, 160), Vector(0, 0, 0), MASK_SOLID, tr, true);
		++g_traces;
		if (!(tr.fraction >= 1.0f && !tr.startsolid))
			for (int z = 0; z < b.n[2]; ++z) b.occ[At(b, x, y, z)] = 0;
		if (std::chrono::steady_clock::now() > dl) break;
	}
	if (b.phase == 3 && b.column >= b.n[0] * b.n[1]) b.phase = 2;
	if (b.phase == 2) {
		BuildFinish(b);
		b.active = false;
		for (Mover &mm : g_movers)
			if (mm.ent && !mm.shape && mm.key == b.key) mm.shape = g_shapeByKey[b.key];
	}
}

// (bridge #37) one box over the entity's bounds (Source local), 2 u deep under its top face so a fast fall cannot tunnel it; the top is the entity's own top
static uint32_t SlabShape(const Mover &m) {
	Vector a = m.lmn, c = m.lmx;
	if (!(c.x > a.x && c.y > a.y && c.z > a.z) || (c - a).Length() > 4096.0f) return 0;
	if (c.z - a.z < 2.0f) a.z = c.z - 2.0f;
	Shape s;
	s.key = m.key, s.smn = a, s.smx = c;
	const float k = 1.0f / kUnitsPerBlock;
	s.boxes.push_back(ColMoverBox{a.x * k, a.z * k, -c.y * k, c.x * k, c.z * k, -a.y * k});  // Source local (x, y, z) -> Minecraft local (x, z, -y)
	const uint32_t id = ShapeAdd(std::move(s));
	++g_shapesBuilt;
	console->Print("[bridge] movers: shape %u (%s) is one box over the bounds of %s %s: Source local (%.1f %.1f %.1f) .. (%.1f %.1f %.1f)\n", id, m.key.c_str(), m.cls.c_str(), m.name.c_str(), a.x,
	               a.y, a.z, c.x, c.y, c.z);
	return id;
}

// ---- (bridge #43) holes mined in a mover ---------------------------------------------------------------------------------
// One mechanism for every mover (door, wall panel, lift, car, platform, cube): its shape has a dig lattice of 1-block cubes in its own frame, anchored at the
// corner of the shape's bounds (so a thin door sits inside one layer of cells). The Minecraft player mines a cell (kRenMoverDig); the mover keeps the set,
// its collision becomes its shape minus those cells (a new shape, bound in place of the old one: Minecraft walks and looks through the hole, and the hole moves
// with the mover), and the picture punches the removed pieces at the mover's drawn pose (digrender.cpp). Nothing here knows any class or map.
static uint32_t Eff(const Mover &m) { return m.dugShape ? m.dugShape : m.shape; }
static void ShapeAnchor(uint32_t id, float out[3]) {
	Shape &s = g_shapes[id - 1];
	if (!s.hasAnchor) {
		float lo[3] = {1e30f, 1e30f, 1e30f};
		for (const ColMoverBox &b : s.boxes) lo[0] = std::fmin(lo[0], b.minX), lo[1] = std::fmin(lo[1], b.minY), lo[2] = std::fmin(lo[2], b.minZ);
		for (int i = 0; i < 3; ++i) s.anchor[i] = s.boxes.empty() ? 0.0f : lo[i];
		s.hasAnchor = true;
	}
	for (int i = 0; i < 3; ++i) out[i] = s.anchor[i];
}
// the box [lo, hi] taken out of a box: the (up to six) pieces that are left
static void SubtractBox(const ColMoverBox &b, const float cl[3], const float ch[3], std::vector<ColMoverBox> &out, std::vector<ColMoverBox> *removed) {
	const float eps = 1e-5f;
	float mn[3] = {b.minX, b.minY, b.minZ}, mx[3] = {b.maxX, b.maxY, b.maxZ};
	for (int a = 0; a < 3; ++a)
		if (cl[a] >= mx[a] - eps || ch[a] <= mn[a] + eps) return out.push_back(b);  // no overlap: kept whole
	auto mk = [](const float a[3], const float c[3]) { return ColMoverBox{a[0], a[1], a[2], c[0], c[1], c[2]}; };
	for (int a = 0; a < 3; ++a) {
		if (cl[a] > mn[a] + eps) {
			float c[3] = {mx[0], mx[1], mx[2]};
			c[a] = cl[a];
			out.push_back(mk(mn, c));
		}
		if (ch[a] < mx[a] - eps) {
			float d[3] = {mn[0], mn[1], mn[2]};
			d[a] = ch[a];
			out.push_back(mk(d, mx));
		}
		mn[a] = std::fmax(mn[a], cl[a]), mx[a] = std::fmin(mx[a], ch[a]);
	}
	if (removed) removed->push_back(mk(mn, mx));  // what is left in the middle is what the box took
}
static std::array<int, 6> CutKey(const float lo[3], const float hi[3]) {
	return {(int)std::lround(lo[0] * 1000), (int)std::lround(lo[1] * 1000), (int)std::lround(lo[2] * 1000), (int)std::lround(hi[0] * 1000), (int)std::lround(hi[1] * 1000), (int)std::lround(hi[2] * 1000)};
}
static void DugReset(Mover &m) {
	if (m.dug.empty() && !m.dugShape) return;
	m.dug.clear(), m.dugShape = 0, m.dugStale = false, m.holeBoxes.clear(), m.cutKind.clear(), m.walls.reset();
}
// dugShape and the picture's pieces from m.dug and the original shape (the shape must be known)
static void RebuildDug(Mover &m) {
	m.dugStale = false;
	m.holeBoxes.clear();
	m.walls.reset();
	if (!m.shape || m.dug.empty()) return void(m.dugShape = 0);
	float a[3];
	ShapeAnchor(m.shape, a);
	std::vector<ColMoverBox> cur = g_shapes[m.shape - 1].boxes, nxt, removed;
	std::vector<WallCut> wallCuts;  // (bridge #50) per cut: what it took out, for the walls
	const std::string baseKey = g_shapes[m.shape - 1].key;
	const Vector smn = g_shapes[m.shape - 1].smn, smx = g_shapes[m.shape - 1].smx;
	std::string key = baseKey + "|dug";
	for (const auto &c : m.dug) {
		char t[96];
		std::snprintf(t, sizeof t, ":%d,%d,%d,%d,%d,%d", c[0], c[1], c[2], c[3], c[4], c[5]);
		key += t;
		const float lo[3] = {c[0] * 0.001f, c[1] * 0.001f, c[2] * 0.001f}, hi[3] = {c[3] * 0.001f, c[4] * 0.001f, c[5] * 0.001f};
		nxt.clear();
		WallCut wc;
		for (int i = 0; i < 3; ++i) wc.lo[i] = lo[i], wc.hi[i] = hi[i];
		auto ck = m.cutKind.find(c);
		wc.kind = ck != m.cutKind.end() ? ck->second : 0;
		for (const ColMoverBox &b : cur) SubtractBox(b, lo, hi, nxt, &wc.removed);
		removed.insert(removed.end(), wc.removed.begin(), wc.removed.end());
		wallCuts.push_back(std::move(wc));
		cur.swap(nxt);
	}
	{  // (bridge #50) the hole's walls: the faces of the cuts where the rest of the shape continues, as Minecraft block faces
		auto set = std::make_shared<MoverWallSet>();
		BuildMoverWalls(cur, wallCuts, kMoverPunchGrow, kUnitsPerBlock, *set);
		if (!set->quads.empty()) m.walls = set;
	}
	for (const ColMoverBox &b : removed)  // MC local (x, y, z) -> Source local (x, -z, y)
		m.holeBoxes.push_back({Vector(b.minX * kUnitsPerBlock, -b.maxZ * kUnitsPerBlock, b.minY * kUnitsPerBlock), Vector(b.maxX * kUnitsPerBlock, -b.minZ * kUnitsPerBlock, b.maxY * kUnitsPerBlock)});
	auto it = g_shapeByKey.find(key);
	if (it != g_shapeByKey.end()) return void(m.dugShape = it->second);
	Shape sh;
	sh.key = key, sh.boxes = std::move(cur), sh.smn = smn, sh.smx = smx, sh.base = m.shape, sh.hasAnchor = true;
	for (int i = 0; i < 3; ++i) sh.anchor[i] = a[i];
	m.dugShape = ShapeAdd(std::move(sh));
	console->Print("[bridge] movers: %s %s #%u has %zu mined cells: shape %u = shape %u minus them (%zu boxes), %zu pieces out of the picture\n", m.cls.c_str(), m.name.c_str(), m.id, m.dug.size(),
	               m.dugShape, m.shape, g_shapes[m.dugShape - 1].boxes.size(), m.holeBoxes.size());
}

static std::mutex g_holeMu;
static std::shared_ptr<const std::vector<MoverHoles>> g_holes;
static std::atomic<bool> g_holesNonEmpty{false};
static std::shared_ptr<const std::vector<MoverWalls>> g_wallSnap;  // (bridge #50) the walls of the holes, with each mover's drawn pose
static std::atomic<bool> g_wallsNonEmpty{false};
static int g_moverDigs = 0, g_moverDigsRefused = 0;
static Vector RenderOriginSrc(const Mover &m);

// the picture's side (digrender.cpp): every mover with holes, at the pose Portal 2 draws it
static void PublishHoles() {
	{  // (bridge #50) the walls first: they exist with the holes, and are published (and cleared) on their own
		auto w = std::make_shared<std::vector<MoverWalls>>();
		for (const Mover &m : g_movers) {
			if (!m.ent || !m.walls || m.holeBoxes.empty()) continue;
			MoverWalls mw;
			mw.origin = RenderOriginSrc(m);
			std::memcpy(mw.rot, m.rot, sizeof mw.rot);
			mw.set = m.walls, mw.id = m.id;
			w->push_back(std::move(mw));
		}
		if (!w->empty() || g_wallsNonEmpty) {
			g_wallsNonEmpty = !w->empty();
			std::lock_guard<std::mutex> lk(g_holeMu);
			g_wallSnap = w;
		}
	}
	bool any = false;
	for (const Mover &m : g_movers) any |= m.ent && !m.holeBoxes.empty();
	if (!any && !g_holesNonEmpty) return;
	auto v = std::make_shared<std::vector<MoverHoles>>();
	for (const Mover &m : g_movers) {
		if (!m.ent || m.holeBoxes.empty()) continue;
		MoverHoles h;
		h.origin = RenderOriginSrc(m);
		std::memcpy(h.rot, m.rot, sizeof h.rot);
		h.boxes = m.holeBoxes;
		v->push_back(std::move(h));
	}
	g_holesNonEmpty = !v->empty();
	std::lock_guard<std::mutex> lk(g_holeMu);
	g_holes = v;
}
void MoverHolesSnapshot(std::vector<MoverHoles> &out) {
	std::shared_ptr<const std::vector<MoverHoles>> h;
	{
		std::lock_guard<std::mutex> lk(g_holeMu);
		h = g_holes;
	}
	if (h) out = *h;
	else out.clear();
}
bool MoverHolesAny() { return g_holesNonEmpty; }
void MoverWallsSnapshot(std::vector<MoverWalls> &out) {
	std::shared_ptr<const std::vector<MoverWalls>> w;
	{
		std::lock_guard<std::mutex> lk(g_holeMu);
		w = g_wallSnap;
	}
	if (w) out = *w;
	else out.clear();
}
bool MoverWallsAny() { return g_wallsNonEmpty; }

// MC-local box (blocks) of mover `from` -> the AABB of the same world box in mover `to`'s MC-local frame
static void CarryBox(const Mover &from, const float lo[3], const float hi[3], const Mover &to, float outLo[3], float outHi[3]) {
	for (int i = 0; i < 3; ++i) outLo[i] = 1e30f, outHi[i] = -1e30f;
	for (int k = 0; k < 8; ++k) {
		const float x = (k & 1) ? hi[0] : lo[0], y = (k & 2) ? hi[1] : lo[1], z = (k & 4) ? hi[2] : lo[2];
		const Vector sl(x * kUnitsPerBlock, -z * kUnitsPerBlock, y * kUnitsPerBlock);  // MC local (x, y, z) -> Source local (x, -z, y)
		const Vector w = from.origin + Rot(from.rot, sl);
		const Vector t = RotT(to.rot, w - to.origin);  // Source local of `to`
		const float m[3] = {t.x / kUnitsPerBlock, t.z / kUnitsPerBlock, -t.y / kUnitsPerBlock};
		for (int i = 0; i < 3; ++i) outLo[i] = std::fmin(outLo[i], m[i]), outHi[i] = std::fmax(outHi[i], m[i]);
	}
}
// does the box [lo, hi] (MC local of m) touch what m is made of: any box of its shape, else (not built yet) its bounds
static bool BoxTouches(const Mover &m, const float lo[3], const float hi[3]) {
	const float e = 0.02f;
	auto over = [&](float a0, float a1, float b0, float b1, float c0, float c1) { return lo[0] < a1 - e && hi[0] > a0 + e && lo[1] < b1 - e && hi[1] > b0 + e && lo[2] < c1 - e && hi[2] > c0 + e; };
	if (m.shape && !g_shapes[m.shape - 1].boxes.empty()) {
		for (const ColMoverBox &b : g_shapes[m.shape - 1].boxes)
			if (over(b.minX, b.maxX, b.minY, b.maxY, b.minZ, b.maxZ)) return true;
		return false;
	}
	const float k = 1.0f / kUnitsPerBlock;  // Source local bounds -> MC local
	return over(m.lmn.x * k, m.lmx.x * k, m.lmn.z * k, m.lmx.z * k, -m.lmx.y * k, -m.lmn.y * k);
}

// kRenMoverDig: the Minecraft player mined (or, for tests, filled back) a cell of a mover. The cell opens that space through EVERY mover that occupies it and moves
// with the one mined (a door is a brush, a model, clip brushes...: one hole through all of them): the same world box is recorded in each such mover's own frame.
void MoverDigOnMessage(const uint8_t *p, uint32_t len) {
	if (len < sizeof(RenMoverDig)) return;
	RenMoverDig h;
	std::memcpy(&h, p, sizeof h);
	if (h.flags & kMoverDigStyle) {  // (bridge #50) not a cell: the atlas rects of a material kind (cx), for the walls of holes
		if (len >= sizeof(RenMoverDig) + sizeof(RenMoverStyle)) MoverWallsSetStyle(h.cx, (const float *)(p + sizeof h));
		return;
	}
	const int kind = (int)((h.flags >> kMoverDigKindShift) & 0xFFu);
	if (!DigEnabled() || h.worldId != DigWorld()) return;  // digging off, or a message of another map
	Mover *m = MoverById(h.moverId);
	const bool fill = (h.flags & kMoverDigFill) != 0;
	const char *why = !m ? "no such mover (gone)" : !m->shape ? "its shape is not built" : m->invisible ? "invisible geometry has nothing to mine" : (m->noMine || m->held) ? "a physics prop / NPC / held object is not minable" : std::abs(h.cx) > 4096 || std::abs(h.cy) > 4096 || std::abs(h.cz) > 4096 ? "cell out of range" : nullptr;
	float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
	if (!why) {
		float a[3];
		ShapeAnchor(m->shape, a);
		lo[0] = a[0] + h.cx, lo[1] = a[1] + h.cy, lo[2] = a[2] + h.cz, hi[0] = lo[0] + 1, hi[1] = lo[1] + 1, hi[2] = lo[2] + 1;
		// the cell must hold some of the mover (a stale pick, or a message from a shape that has changed since)
		if (!fill && !BoxTouches(*m, lo, hi)) why = "the cell holds nothing of the mover";
		else if (!fill && m->dug.size() >= 400) why = "too many mined cells in one mover";
	}
	if (why) {
		if (g_moverDigsRefused++ < 20) console->Print("[bridge] mover dig #%u cell (%d %d %d) refused: %s\n", h.moverId, h.cx, h.cy, h.cz, why);
		return;
	}
	auto apply = [&](Mover &t, const float l[3], const float u[3]) {
		const auto key = CutKey(l, u);
		if (fill ? t.dug.erase(key) == 0 : !t.dug.insert(key).second) return false;  // already so
		if (fill) t.cutKind.erase(key);
		else t.cutKind[key] = kind;  // (bridge #50) its walls are made of what the cell was mined as
		t.dugStale = true;
		return true;
	};
	const bool first = apply(*m, lo, hi);
	++g_moverDigs;
	console->Print("[bridge] mover dig: %s %s #%u cell (%d %d %d) %s (%zu now)\n", m->cls.c_str(), m->name.c_str(), m->id, h.cx, h.cy, h.cz, fill ? "filled" : "mined", m->dug.size());
	(void)first;
	// the others: every mover that moves with it (same velocity now: a door's model, brush and clip brushes; not a cube that happens to lie in the doorway) and holds some of
	// the space. Their cut is the same world box in their own frame (exact when they are turned alike, as a door's parts are).
	for (Mover &o : g_movers) {
		if (!o.ent || o.id == m->id || o.physics || o.noMine || o.held) continue;  // (bridge #50: by what it is: a resting cube or turret is as still as a closed door)
		if ((o.vel - m->vel).Length() > 12.0f) continue;
		if ((o.origin - m->origin).Length() > o.radius + m->radius + 80.0f) continue;
		float ol[3], oh[3];
		CarryBox(*m, lo, hi, o, ol, oh);
		if (!fill && !BoxTouches(o, ol, oh)) continue;
		if (!fill && o.dug.size() >= 400) continue;
		if (apply(o, ol, oh))
			console->Print("[bridge] mover dig: ... and through %s %s #%u (it shares the space and moves with it)%s\n", o.cls.c_str(), o.name.c_str(), o.id, o.invisible ? " [invisible]" : "");
	}
}

// ---- level / per frame -------------------------------------------------------------------------------------------------
void MoversLevelStart() {
	// (the shapes stay: they are the models'; Minecraft gets them again in the new epoch)
	for (Shape &s : g_shapes) s.sentEpoch = 0;
	g_movers.clear(), g_moverByEnt.clear(), g_classOf.clear();
	g_build = Build();
	g_listAt = -1e9, g_firstListAt = -1e9, g_levelAt = NowS();
	g_p2MovesSince = -1, g_p2Watched = 0;
	// (test knob, bridge #40) cache/bridge/s40_bind holds the seconds shapes and binds are withheld from Minecraft after the level start: a slow collision thread
	g_s40BindDelay = 0;
	if (FILE *f = std::fopen((CacheDir() + "/s40_bind").c_str(), "r")) {
		double d = 0;
		if (std::fscanf(f, "%lf", &d) == 1) g_s40BindDelay = d;
		std::fclose(f);
		console->Print("[bridge] movers: test knob: shapes and binds withheld for %.1f s\n", g_s40BindDelay);
	}
}
void MoversSetUrgent(bool on) { g_urgent = on; }
static bool g_p2MovesBody = false;
void MoversSetPortal2MovesBody(bool on) {
	const double now = NowS();
	if (on && g_p2MovesSince < 0) g_p2MovesSince = now;
	if (!on) g_p2MovesSince = -1;
	if (on) g_p2Watched = std::max(g_p2Watched, now - g_p2MovesSince);
	g_p2MovesBody = on;
}  // mc.cpp: Chell leads, or the body is Portal 2's (a hand-over)

static void Quat(const float r[3][3], double &qx, double &qy, double &qz, double &qw) {
	// M = P r P^T in Minecraft's axes, P: rows (1 0 0) (0 0 1) (0 -1 0)
	const double m00 = r[0][0], m01 = r[0][2], m02 = -r[0][1];
	const double m10 = r[2][0], m11 = r[2][2], m12 = -r[2][1];
	const double m20 = -r[1][0], m21 = -r[1][2], m22 = r[1][1];
	const double tr = m00 + m11 + m22;
	if (tr > 0) {
		const double S = std::sqrt(tr + 1.0) * 2;
		qw = 0.25 * S, qx = (m21 - m12) / S, qy = (m02 - m20) / S, qz = (m10 - m01) / S;
	} else if (m00 > m11 && m00 > m22) {
		const double S = std::sqrt(1.0 + m00 - m11 - m22) * 2;
		qw = (m21 - m12) / S, qx = 0.25 * S, qy = (m01 + m10) / S, qz = (m02 + m20) / S;
	} else if (m11 > m22) {
		const double S = std::sqrt(1.0 + m11 - m00 - m22) * 2;
		qw = (m02 - m20) / S, qx = (m01 + m10) / S, qy = 0.25 * S, qz = (m12 + m21) / S;
	} else {
		const double S = std::sqrt(1.0 + m22 - m00 - m11) * 2;
		qw = (m10 - m01) / S, qx = (m02 + m20) / S, qy = (m12 + m21) / S, qz = 0.25 * S;
	}
	if (qw < 0) qw = -qw, qx = -qx, qy = -qy, qz = -qz;
	const double n = std::sqrt(qw * qw + qx * qx + qy * qy + qz * qz);
	if (n > 0) qw /= n, qx /= n, qy /= n, qz /= n;
}

// focus: where the Minecraft player is (or is about to be), Source
// active: Minecraft's collision is exported (the Minecraft player, or a switch to it, or its entities in the chambers): shapes,
// binds and poses go out. Otherwise (Chell plays a terrain session) the movers are only listed and their shapes built in the
// background, so a later switch finds them in the cache instead of building them during the hand-over.
void MoversFrame(const Vector &focus, bool active) {
	if (!g_on || !link::McAlive() || !CollisionFieldCleared()) return;
	PROF("movers");
	const double now = NowS();
	g_focus = focus, ++g_frame;
	EntLiveSet(g_live);
	if (now - g_listAt > 1.0) Refresh(now);
	// the dead (Kill, a fizzled cube, a bone follower gone with its model): unbound, forgotten
	for (Mover &m : g_movers)
		if (m.ent && !g_live.count(m.ent)) m.ent = nullptr;
	const uint32_t epoch = CollisionFieldEpoch();
	for (Mover &m : g_movers)
		if (!m.ent && m.bound && m.boundEpoch == epoch) {
			ColMoverBind bind{m.id, 0};
			if (link::WriteCollision(kColMoverBind, &bind, sizeof bind, nullptr, 0)) m.bound = 0, ++g_bindsSent;
		}
	if (std::any_of(g_movers.begin(), g_movers.end(), [](const Mover &m) { return !m.ent && !m.bound; })) {
		g_movers.erase(std::remove_if(g_movers.begin(), g_movers.end(), [](const Mover &m) { return !m.ent && !m.bound; }), g_movers.end());
		g_moverByEnt.clear();
		for (size_t i = 0; i < g_movers.size(); ++i)
			if (g_movers[i].ent) g_moverByEnt[g_movers[i].ent] = i;
	}
	// the prop the player holds (Portal 2's hold controller carries it in front of the eye): not a solid for Minecraft
	void *heldEnt = nullptr;
	if (PlayerHolding()) {
		float best = 150.0f;
		const Vector eye = PlayerOrigin() + Vector(0, 0, 64);
		for (const Mover &m : g_movers)
			if (m.ent && m.physics && (m.origin - eye).Length() < best) best = (m.origin - eye).Length(), heldEnt = m.ent;
	}
	{
		PROF("movers.pose");
		for (Mover &m : g_movers) {
			if (!m.ent) continue;
			UpdatePose(m, now);
			m.solid = SolidNow(m.ent);
			m.held = m.ent == heldEnt;
			if (m.solid && !m.classified) Classify(m);
			if (IsSlabClass(m.cls)) ResolveSlabPose(m, now);  // (bridge #39) before its shape and bind go out
			if (!m.shape) {
				auto it = g_shapeByKey.find(m.key);
				if (it != g_shapeByKey.end()) m.shape = it->second;
				else if (IsSlabClass(m.cls) && m.solid) m.shape = SlabShape(m);
				else if ((m.shape = ShapeLoad(m.key))) ++g_shapesLoaded;
			}
			if (m.dugStale && m.shape) RebuildDug(m);  // (bridge #43) the original shape minus the mined cells
		}
	}
	// (bridge #26) what Portal 2's own player movement passes through: a solid the player's hull stays inside for 0.25 s while
	// Portal 2 moves the player (Chell leads, or the body is Portal 2's during a hand-over). The game's rule is not in a class,
	// flag or material (pc_pit_flings's arrival train: a plain solid nodraw block around the car, Chell rides inside it).
	// (lead) not learned while a scripted sequence or a level teleport moves the body (sp_a1_intro1's vault: the script put the body inside
	// container_collision, the dock room's whole floor, and the Minecraft player fell through it for the rest of the level), and not for ever:
	// a passable mover the hull has stayed clear of for 1 s is exported again.
	bool McScriptedRecently();  // mc.cpp
	{
		const Vector hullC = PlayerOrigin() + Vector(0, 0, 36);
		for (Mover &m : g_movers) {
			if (!m.ent || !m.passable) continue;
			if ((m.origin - hullC).Length() <= m.radius + 64.0f && BoxHitsEnt(m.ent, hullC, Vector(12, 12, 28))) {
				m.clearSince = -1;
				continue;
			}
			if (m.clearSince < 0) m.clearSince = now;
			if (now - m.clearSince < 1.0) continue;
			m.passable = false, m.insideSince = -1, m.clearSince = -1;
			console->Print("[bridge] movers: %s %s #%u: the player is clear of it again: exported again\n", m.cls.c_str(), m.name.c_str(), m.id);
		}
	}
	if (g_p2MovesBody && !McScriptedRecently()) {
		const Vector hullC = PlayerOrigin() + Vector(0, 0, 36);
		for (Mover &m : g_movers) {
			if (!m.ent || m.passable || !m.solid || m.physics || (m.origin - hullC).Length() > m.radius + 64.0f) continue;
			if (!BoxHitsEnt(m.ent, hullC, Vector(12, 12, 28))) {
				m.insideSince = -1;
				continue;
			}
			if (m.insideSince < 0) m.insideSince = now;
			if (now - m.insideSince < 0.25) continue;
			m.passable = true;
			console->Print("[bridge] movers: %s %s #%u: Portal 2's player stands inside it: passable for players, not exported\n", m.cls.c_str(), m.name.c_str(), m.id);
		}
	}
	// the next shape to build: the unbuilt solid visible mover nearest the focus
	if (!g_build.active) {
		Mover *pick = nullptr;
		float best = 1e30f;
		for (Mover &m : g_movers) {
			if (!m.ent || m.shape || !m.solid || m.passable || !m.classified) continue;
			const float d = (m.origin - focus).Length() - m.radius;
			if (d < best) best = d, pick = &m;
		}
		if (pick) BuildStart(*pick);
	}
	BuildStep(g_urgent ? g_buildUrgentMs : g_buildMs);
	PublishHoles();  // (bridge #43) the picture's holes, at each mover's drawn pose
	if (!active) return;
	// shapes and binds (the shape before its first bind in an epoch). (Not before Portal 2 has moved the body for 0.3 s this
	// level: a mover its player passes through, bound before that, held Minecraft inside it and pushed it out of the car.)
	for (Mover &m : g_movers) {
		if (!m.ent || g_p2Watched < 0.3 || now - g_levelAt < g_s40BindDelay) continue;
		const uint32_t eff = Eff(m);  // (bridge #43) its shape minus the cells mined out of it
		const uint32_t want = m.solid && !m.passable && eff && !g_shapes[eff - 1].boxes.empty() ? eff : 0;
		if (want == m.bound && (m.boundEpoch == epoch || !want)) continue;
		if (want) {
			Shape &s = g_shapes[want - 1];
			if (s.sentEpoch != epoch) {
				ColMoverShape hdr{s.id, (uint32_t)s.boxes.size()};
				if (!link::WriteCollision(kColMoverShape, &hdr, sizeof hdr, s.boxes.data(), (uint32_t)(s.boxes.size() * sizeof(ColMoverBox)))) break;  // ring full: next frame
				// its dig lattice (bridge #43): the anchor of the ORIGINAL shape, and whether there is anything to mine in it
				ColMoverLattice lat{s.id, (m.invisible ? kLatticeNoPick : 0u) | (m.noMine ? kLatticeNoMine : 0u), 0, 0, 0, 0};
				float a[3];
				ShapeAnchor(s.base ? s.base : s.id, a);
				lat.ax = a[0], lat.ay = a[1], lat.az = a[2];
				if (!link::WriteCollision(kColMoverLattice, &lat, sizeof lat, nullptr, 0)) break;  // (the shape goes again next frame)
				s.sentEpoch = epoch, ++g_shapesSent;
			}
		}
		ColMoverBind bind{m.id, want};
		if (!link::WriteCollision(kColMoverBind, &bind, sizeof bind, nullptr, 0)) break;
		m.bound = want, m.boundEpoch = epoch, ++g_bindsSent;
	}
	// the poses of the bound movers near the focus, nearest first
	PROF("movers.stream");
	static std::vector<std::pair<float, Mover *>> near;
	near.clear();
	for (Mover &m : g_movers) {
		m.listed = false;
		if (!m.ent || !m.bound || m.boundEpoch != epoch || m.held) continue;
		const float d = (m.origin - focus).Length() - m.radius;
		if (d < g_poseRadius) near.push_back({d, &m});
	}
	if (near.size() > kMaxMovers) std::nth_element(near.begin(), near.begin() + kMaxMovers, near.end(), [](const auto &a, const auto &b) { return a.first < b.first; }), near.resize(kMaxMovers);
	McMovers *v = link::MoversBegin();
	if (!v) return;
	double bx, by, bz;
	SrcToMcD(focus, bx, by, bz);
	bx = std::floor(bx), by = std::floor(by), bz = std::floor(bz);
	v->count = 0, v->epoch = epoch, v->flags = 0, v->frame = g_frame, v->baseX = bx, v->baseY = by, v->baseZ = bz;
	for (const auto &p : near) {
		Mover &m = *p.second;
		double x, y, z, qx, qy, qz, qw;
		SrcToMcD(m.origin, x, y, z);
		Quat(m.rot, qx, qy, qz, qw);
		MoverPose &r = v->movers[v->count++];
		r.moverId = m.id, r.x = (float)(x - bx), r.y = (float)(y - by), r.z = (float)(z - bz);
		r.qx = (float)qx, r.qy = (float)qy, r.qz = (float)qz, r.qw = (float)qw;
		m.listed = true;
	}
	g_posesLast = (int)v->count;
	link::MoversEnd();
}

// ---- queries (mc.cpp, collfield.cpp, the overlay, bug marks, tests) ----------------------------------------------------
// Minecraft has the collision of every mover around p: the list has been read this epoch and every solid visible mover whose
// bounds come within r of p is bound (its shape built or loaded and sent)
bool MoversReadyNear(const Vector &p, float r, char *why, size_t n) {
	if (!g_on) return true;
	if (g_firstListAt < g_levelAt) return std::snprintf(why, n, "the movers are not listed yet"), false;
	// (what Portal 2's player passes through is learnt while Portal 2 moves the body: 0.3 s of it first)
	if (g_p2Watched < 0.3) return std::snprintf(why, n, "learning which movers Portal 2's player passes through (%.2f s)", g_p2Watched), false;
	int waiting = 0;
	const Mover *first = nullptr;
	for (const Mover &m : g_movers) {
		if (!m.ent || !m.solid || m.held || m.passable) continue;
		if ((m.origin - p).Length() - m.radius > r) continue;
		const bool ready = m.classified && m.shape && (g_shapes[Eff(m) - 1].boxes.empty() || (m.bound == Eff(m) && m.boundEpoch == CollisionFieldEpoch() && m.listed));
		if (!ready) ++waiting, first = first ? first : &m;
	}
	if (waiting) return std::snprintf(why, n, "%d movers near the body have no collision in Minecraft yet (%s %s)", waiting, first->cls.c_str(), first->name.c_str()), false;
	return true;
}
// the mover's origin now (MC) and its yaw (Source degrees, valid when it stands upright)
bool MoverAnchorMc(uint32_t id, double &x, double &y, double &z) {
	const Mover *m = MoverById(id);
	if (!m) return false;
	SrcToMcD(m->origin, x, y, z);
	return true;
}
// (bridge #26) the mover's origin as Portal 2 DRAWS it this frame: the client draws entities at their server poses interpolated
// at (now - its interpolation delay), linearly between the poses it received. The camera of a rider is built on this, so the
// eye and the drawn car can never be out of phase (the server origin steps 10 u per server tick: the camera on it jittered
// against the smoothly drawn car).
#include "Modules/Tier1.hpp"
static float CvarF(const char *name, float def) {
	if (!tier1 || !tier1->FindCommandBase) return def;
	auto *v = (ConVar *)tier1->FindCommandBase(tier1->g_pCVar->ThisPtr(), name);
	if (!v) return def;
	v = v->m_pParent ? v->m_pParent : v;
	return v->m_fValue;
}
static double g_lerpOverride = -1;  // pc_movers_lerp
double MoversRenderLerp() {
	if (g_lerpOverride >= 0) return g_lerpOverride;
	const float interp = CvarF("cl_interp", 0.1f), ratio = CvarF("cl_interp_ratio", 2.0f), rate = CvarF("cl_updaterate", 20.0f);
	return std::max((double)interp, rate > 0 ? ratio / rate : 0.0);
}
// the mover's origin as Portal 2 draws it (Source)
static Vector RenderOriginSrc(const Mover &m) {
	Vector o = m.origin;
	const double t = NowS() - MoversRenderLerp();
	const auto &h = m.hist;
	if (!h.empty() && t < h.back().first) {
		o = h.front().second;
		for (size_t i = 1; i < h.size(); ++i)
			if (h[i].first > t) {
				const double a = (t - h[i - 1].first) / std::max(1e-6, h[i].first - h[i - 1].first);
				o = h[i - 1].second + (h[i].second - h[i - 1].second) * (float)std::max(0.0, std::min(1.0, a));
				break;
			}
	}
	return o;
}
bool MoverRenderAnchorMc(uint32_t id, double &x, double &y, double &z) {
	const Mover *m = MoverById(id);
	if (!m) return false;
	SrcToMcD(RenderOriginSrc(*m), x, y, z);
	return true;
}
// (bridge #40) the camera of a hand-over: Portal 2's own eye is the body, which rides the SERVER's car; the car is DRAWN a cl_interp behind that (the 30 u of a
// car rising at 300 u/s). When the Minecraft player takes over its camera rides the drawn car, so the view stepped down by that lag at the switch. The pending
// switch draws the same camera in advance: this is the (server - drawn) offset of the moving solid the body stands on (a box just under the feet touches it).
bool MoverUnderFeetLag(const Vector &feet, Vector &lag) {
	if (!g_on) return false;
	const double now = NowS(), t = now - MoversRenderLerp();
	for (Mover &m : g_movers) {
		if (!m.ent || !m.solid || m.physics || m.held || now - m.movedAt > 0.3 || (m.origin - feet).Length() - m.radius > 96.0f) continue;
		if (!BoxHitsEnt(m.ent, feet - Vector(0, 0, 2), Vector(10, 10, 3))) continue;
		Vector o = AbsOrigin(m.ent), r = m.origin;
		const auto &h = m.hist;
		if (!h.empty() && t < h.back().first) {
			r = h.front().second;
			for (size_t i = 1; i < h.size(); ++i)
				if (h[i].first > t) {
					const double a = (t - h[i - 1].first) / std::max(1e-6, h[i].first - h[i - 1].first);
					r = h[i - 1].second + (h[i].second - h[i - 1].second) * (float)std::max(0.0, std::min(1.0, a));
					break;
				}
		}
		lag = o - r;
		return lag.Length() < 100.0f;
	}
	return false;
}
// (bridge #40) the mover's origin as the server has it right now (the entity itself, not the last frame's sample): what the body rides
bool MoverLiveAnchorMc(uint32_t id, double &x, double &y, double &z) {
	const Mover *m = MoverById(id);
	if (!m) return false;
	SrcToMcD(m->ent && g_live.count(m->ent) ? AbsOrigin(m->ent) : m->origin, x, y, z);
	return true;
}
CON_COMMAND(pc_movers_lerp, "pc_movers_lerp [s|-1] - (bridge #26) the render delay a rider's camera uses (-1: Portal 2's cl_interp rule)\n") {
	if (args.ArgC() > 1) g_lerpOverride = std::atof(args[1]);
	console->Print("[bridge] movers render lerp %.3f s (cl_interp %.3f, cl_interp_ratio %.2f, cl_updaterate %.0f)\n", MoversRenderLerp(), CvarF("cl_interp", -1), CvarF("cl_interp_ratio", -1),
	               CvarF("cl_updaterate", -1));
}
bool MoverUprightYaw(uint32_t id, float &yaw) {
	const Mover *m = MoverById(id);
	if (!m || std::fabs(m->rot[2][2]) < 0.9f) return false;
	yaw = std::atan2(m->rot[1][0], m->rot[0][0]) * 57.29578f;
	return true;
}
std::string MoverName(uint32_t id) {
	const Mover *m = MoverById(id);
	return m ? m->cls + " " + m->name + " #" + std::to_string(id) : (id ? "mover #" + std::to_string(id) + " (gone)" : "-");
}
// the bound movers' boxes near p (Source, world AABBs of the boxes where they are now)
static void BoxesNear(const Vector &p, float r, std::vector<std::pair<Vector, Vector>> &out, std::vector<const Mover *> *who = nullptr) {
	for (const Mover &m : g_movers) {
		if (!m.ent || !m.bound || m.held || (m.origin - p).Length() - m.radius > r) continue;
		const Shape &s = g_shapes[m.bound - 1];
		for (const ColMoverBox &b : s.boxes) {
			// MC local -> Source local: (x, y, z)_mc = (x, z, -y)_src, so Source (x, -z, y)
			const Vector a(b.minX * kUnitsPerBlock, -b.maxZ * kUnitsPerBlock, b.minY * kUnitsPerBlock), c(b.maxX * kUnitsPerBlock, -b.minZ * kUnitsPerBlock, b.maxY * kUnitsPerBlock);
			const Vector centre = m.origin + Rot(m.rot, (a + c) * 0.5f), h = AbsRot(m.rot, (c - a) * 0.5f);
			out.push_back({centre - h, centre + h});
			if (who) who->push_back(&m);
		}
	}
}
void MoversBoxesNear(const Vector &p, float r, std::vector<std::pair<Vector, Vector>> &out) {
	out.clear();
	BoxesNear(p, r, out);
}
// a bound mover's box top under the feet (within 8 u below to 4 u above), over the middle of the feet
bool MoverFloorUnder(const Vector &feet, std::string *what) {
	static std::vector<std::pair<Vector, Vector>> boxes;
	static std::vector<const Mover *> who;
	boxes.clear(), who.clear();
	BoxesNear(feet, 200.0f, boxes, &who);
	for (size_t i = 0; i < boxes.size(); ++i) {
		const auto &b = boxes[i];
		if (feet.x > b.first.x - 6 && feet.x < b.second.x + 6 && feet.y > b.first.y - 6 && feet.y < b.second.y + 6 && b.second.z > feet.z - 8 && b.second.z < feet.z + 4) {
			if (what) *what = who[i]->cls + " " + who[i]->name;
			return true;
		}
	}
	return false;
}
// (tests) the ride counts as the target: the same mover, or one of the target model's bone followers (an arm's collision)
bool MoverOfTarget(uint32_t ride, uint32_t target) {
	if (!ride || !target) return false;
	if (ride == target) return true;
	const Mover *r = MoverById(ride), *t = MoverById(target);
	return r && t && r->cls == "phys_bone_follower" && (r->origin - t->origin).Length() < t->radius + 16.0f;
}
bool MoverEntOfTarget(void *hit, void *target) {
	if (hit == target) return true;
	auto a = g_moverByEnt.find(hit), b = g_moverByEnt.find(target);
	return a != g_moverByEnt.end() && b != g_moverByEnt.end() && MoverOfTarget(g_movers[a->second].id, g_movers[b->second].id);
}
bool MoverInvisible(void *e) {
	auto it = g_moverByEnt.find(e);
	return it != g_moverByEnt.end() && g_movers[it->second].invisible;
}
bool MoverIsBound(void *e) {
	auto it = g_moverByEnt.find(e);
	return it != g_moverByEnt.end() && g_movers[it->second].bound && !g_movers[it->second].held;
}
// (the fall guard) a moving mover tested in its own frame: the feet's path as it was relative to it
bool FloorCrossedIn(const Vector &a, const Vector &b, ITraceFilter &filter, Vector &at, std::string &what);  // collfield.cpp
bool MoversFloorCrossed(const Vector &a, const Vector &b, double ageA, double ageB, Vector &at, std::string &what) {
	const double now = NowS();
	for (const Mover &m : g_movers) {
		if (!m.ent || m.held || (m.origin - b).Length() > m.radius + 200.0f) continue;
		// (bridge #34) a visible solid mover Minecraft has NO collision for (its shape is not built or not bound yet: the arrival car at a level start)
		// is a Portal 2 floor all the same: a fall through it is the bug this guard exists for. (Before, only bound moving movers were watched,
		// so the one case where the floor was missing was the one case not caught.) Physics props, invisible blockers and what Portal 2's player
		// passes through are not floors Minecraft is owed.
		const bool owed = m.solid && !m.bound && !m.passable && !m.physics && !m.invisible && m.classified && (!m.shape || !g_shapes[Eff(m) - 1].boxes.empty());
		if (owed) {
			OneFilter one;
			one.t = m.ent;
			if (FloorCrossedIn(a, b, one, at, what)) return what += " (no collision bound in Minecraft yet)", true;
			continue;
		}
		if (!m.bound || now - m.movedAt > 0.3 || m.vel.Length() < 20.0f) continue;
		const float ka = (float)std::fmin(0.5, std::fmax(0.0, ageA - (now - m.posAt))), kb = (float)std::fmin(0.5, std::fmax(0.0, ageB - (now - m.posAt)));
		OneFilter one;
		one.t = m.ent;
		if (FloorCrossedIn(a + m.vel * ka, b + m.vel * kb, one, at, what)) return true;
	}
	return false;
}
// ---- (bridge #34) the support guard: a Minecraft player found below the floor it was standing on -------------------------------------------
// The fall guard above traces the feet's path against Portal 2's floors. This one remembers WHAT the standing player was on (by Portal 2's own
// geometry: a downward hull trace against each solid mover, whether Minecraft has its collision or not) and where on it, in that mover's frame.
// If the player is later found not on the ground, 40+ u under the spot that mover carries now, with the mover right above it, it fell out of
// the floor (the arrival car at a level start with its shape not bound yet; any mover whose collision came late or was dropped).
// The mover must still be solid, alive and not one Portal 2's player passes through. Invisible blockers count (the arrival floor_clip is what
// Portal 2's player stands on in the car).
bool MoverSupportSample(const Vector &feet, uint32_t &id, Vector &local) {
	for (const Mover &m : g_movers) {
		if (!m.ent || !m.solid || m.held || m.passable || m.physics) continue;
		if ((m.origin - feet).Length() - m.radius > 40.0f) continue;
		OneFilter one;
		one.t = m.ent;
		Ray_t ray;
		ray.m_Delta = VectorAligned(0, 0, -16.0f);
		ray.m_IsSwept = true;
		ray.m_Extents = VectorAligned(6, 6, 1);
		ray.m_IsRay = false;
		ray.m_StartOffset = VectorAligned(0, 0, 0);
		ray.m_Start = VectorAligned(feet.x, feet.y, feet.z + 6.0f);
		ray.m_pWorldAxisTransform = nullptr;
		CGameTrace tr;
		std::memset(&tr, 0, sizeof tr);
		engine.TraceRay(engine.traceServer->ThisPtr(), ray, MASK_PLAYERSOLID, &one, &tr);
		++g_traces;
		if (tr.startsolid || tr.allsolid || tr.fraction >= 1.0f || tr.plane.normal.z < 0.7f) continue;
		id = m.id, local = RotT(m.rot, feet - m.origin);
		return true;
	}
	return false;
}
// true: the feet are under the spot the mover carries now by more than 40 u and the mover is above them
bool CollisionFieldInPortalHole(const Vector &p);  // collfield.cpp (bridge #72)
bool MoverSupportCheck(uint32_t id, const Vector &local, const Vector &feetNow, Vector &expected, std::string &name) {
	const Mover *m = MoverById(id);
	if (!m || !m->solid || m->held || m->passable) return false;
	name = m->cls + " " + m->name + " #" + std::to_string(m->id) + (m->bound ? " (bound)" : " (NOT bound in Minecraft)");
	expected = m->origin + Rot(m->rot, local);
	if (feetNow.z > expected.z - 40.0f || std::fabs(feetNow.x - expected.x) > 200.0f || std::fabs(feetNow.y - expected.y) > 200.0f) return false;
	if (CollisionFieldInPortalHole(feetNow + Vector(0, 0, 2))) return false;  // (bridge #72) down a portal's hole through the slab it stood on: the Minecraft player's rule cuts movers there too
	OneFilter one;
	one.t = m->ent;
	Ray_t ray;
	ray.m_Delta = VectorAligned(0, 0, 320.0f);
	ray.m_IsSwept = true;
	ray.m_Extents = VectorAligned(6, 6, 1);
	ray.m_IsRay = false;
	ray.m_StartOffset = VectorAligned(0, 0, 0);
	ray.m_Start = VectorAligned(feetNow.x, feetNow.y, feetNow.z + 72.0f);
	ray.m_pWorldAxisTransform = nullptr;
	CGameTrace tr;
	std::memset(&tr, 0, sizeof tr);
	engine.TraceRay(engine.traceServer->ThisPtr(), ray, MASK_PLAYERSOLID, &one, &tr);
	++g_traces;
	return tr.startsolid || tr.allsolid || tr.fraction < 1.0f;
}
// tracked movers for bug marks: entity, origin now, origin before its last move
void MoversTracked(std::vector<std::tuple<void *, Vector, Vector>> &out) {
	out.clear();
	for (const Mover &m : g_movers)
		if (m.ent && m.bound) out.emplace_back(m.ent, m.origin, m.lastOrigin);
}
// a mover by "#id", "@x,y,z" (the nearest to that point) or targetname (the nearest to the player)
void *MoverFind(const char *spec, uint32_t *idOut) {
	const Mover *best = nullptr;
	if (spec[0] == '#') {
		best = MoverById((uint32_t)std::atoi(spec + 1));
	} else {
		Vector p = PlayerOrigin();
		const bool at = spec[0] == '@';
		if (at) std::sscanf(spec + 1, "%f,%f,%f", &p.x, &p.y, &p.z);
		float bd = 1e30f;
		for (const Mover &m : g_movers) {
			if (!m.ent || (!at && m.name != spec)) continue;
			const float d = (m.origin - p).Length();
			if (d < bd) bd = d, best = &m;
		}
	}
	if (idOut) *idOut = best ? best->id : 0;
	return best ? best->ent : nullptr;
}

// ---- commands ------------------------------------------------------------------------------------------------------------
static void PrintMover(const Mover &m, FILE *f) {
	const Shape *s = m.shape ? &g_shapes[Eff(m) - 1] : nullptr;
	static int offG = -2, offT = -2, offF = -2;
	if (offG == -2) offG = EntFieldOffset(m.ent, "m_CollisionGroup"), offT = EntFieldOffset(m.ent, "m_nSolidType"), offF = EntFieldOffset(m.ent, "m_usSolidFlags");
	char extra[96];
	std::snprintf(extra, sizeof extra, "[group %d solidtype %d flags 0x%x] ", offG >= 0 ? *(const int *)((const char *)m.ent + offG) : -1, offT >= 0 ? (int)*((const unsigned char *)m.ent + offT) : -1,
	              offF >= 0 ? (int)*(const unsigned short *)((const char *)m.ent + offF) : -1);
	char line[700];
	char probed[48] = "";
	if (m.rotFixed) std::snprintf(probed, sizeof probed, " [pose probed, %d/%d hits]", m.slabHits, kSlabProbes);
	std::snprintf(line, sizeof line, "%smover #%u %s '%s' at (%.1f %.1f %.1f) ang (%.1f %.1f %.1f)%s r %.0f %s%s%s%s shape %u (%zu boxes%s) bound %u%s%s moved %.1f s ago; key %s\n", extra, m.id,
	              m.cls.c_str(), m.name.c_str(), m.origin.x, m.origin.y, m.origin.z, m.ang.x, m.ang.y, m.ang.z, probed, m.radius, m.solid ? "solid" : "NOT SOLID",
	              m.invisible ? " INVISIBLE" : "", m.held ? " HELD" : "", m.listed ? " posed" : "", m.shape, s ? s->boxes.size() : 0, s && s->fromDisk ? ", disk" : "", m.bound,
	              m.physics ? " physics" : "", m.noMine ? " NOT-MINABLE" : "", m.movedAt > 0 ? NowS() - m.movedAt : -1.0, m.key.c_str());
	if (f) std::fputs(line, f);
	else console->Print("[bridge]   %s", line);
}
// (bridge #40, diagnostics) the movers around the body and whether the body's feet touch each: the car at a level start
void MoversDebugDump(const Vector &feet, float r) {
	for (const Mover &m : g_movers) {
		if (!m.ent || (m.origin - feet).Length() - m.radius > r) continue;
		const bool under = BoxHitsEnt(m.ent, feet - Vector(0, 0, 2), Vector(10, 10, 3)), inside = BoxHitsEnt(m.ent, feet + Vector(0, 0, 36), Vector(12, 12, 28));
		char tag[64];
		std::snprintf(tag, sizeof tag, "%s%s%s", under ? "[FEET ON] " : "", inside ? "[BODY INSIDE] " : "", m.passable ? "[PASSABLE] " : "");
		console->Print("[bridge] arrdbg mover %s", tag);
		PrintMover(m, nullptr);
	}
}
// (bridge #32) pcb_assert_mover_bound <targetname|classname> [min boxes=1]: that mover's collision has reached Minecraft: solid now, a shape with at least
// that many boxes, bound in this epoch (Minecraft has the shape) and posed in the latest pose block (Minecraft collides with it). A light bridge, a lift, a door.
CON_COMMAND(pcb_assert_mover_bound, "pcb_assert_mover_bound <targetname|classname> [min boxes=1] - (bridge #32) a mover with that name or class has its collision in Minecraft (shape bound and posed)\n") {
	if (args.ArgC() < 2) return;
	const size_t minBoxes = args.ArgC() > 2 ? (size_t)std::atoi(args[2]) : 1;
	const uint32_t epoch = CollisionFieldEpoch();
	char buf[400];
	int seen = 0;
	std::string detail;
	for (const Mover &m : g_movers) {
		if (!m.ent || (m.name != args[1] && m.cls != args[1])) continue;
		++seen;
		const size_t boxes = m.shape ? g_shapes[Eff(m) - 1].boxes.size() : 0;
		// (bridge #39) and the shape sits where the entity's collision is: small boxes at the middles of up to 8 of its boxes, in the pose Minecraft is sent, traced against the entity
		int probes = 0, hits = 0;
		if (m.shape && m.bound == Eff(m))
			for (const ColMoverBox &b : g_shapes[Eff(m) - 1].boxes) {
				if (probes >= 8) break;
				const Vector lc((b.minX + b.maxX) * 0.5f * kUnitsPerBlock, -(b.minZ + b.maxZ) * 0.5f * kUnitsPerBlock, (b.minY + b.maxY) * 0.5f * kUnitsPerBlock);  // MC local (x, y, z) -> Source (x, -z, y)
				++probes;
				if (BoxHitsEnt(m.ent, m.origin + Rot(m.rot, lc), Vector(2, 2, 2))) ++hits;
			}
		const bool placed = probes > 0 && hits * 2 >= probes;
		const bool ok = m.solid && m.shape && boxes >= minBoxes && m.bound == Eff(m) && m.boundEpoch == epoch && m.listed && placed;
		std::snprintf(buf, sizeof buf,
		              "%s %s #%u: solid %d, shape %u with %zu boxes (want >= %zu), bound %u, bound this epoch %d, posed %d, passable %d, held %d, invisible %d, bounds (%.1f %.1f %.1f)..(%.1f %.1f %.1f), "
		              "posed at (%.1f %.1f %.1f) ang (%.1f %.1f %.1f)%s: %d of %d shape probes hit the entity (%s)",
		              m.cls.c_str(), m.name.c_str(), m.id, m.solid, m.shape, boxes, minBoxes, m.bound, m.boundEpoch == epoch, m.listed, m.passable, m.held, m.invisible, m.lmn.x, m.lmn.y, m.lmn.z, m.lmx.x,
		              m.lmx.y, m.lmx.z, m.origin.x, m.origin.y, m.origin.z, m.ang.x, m.ang.y, m.ang.z, m.rotFixed ? " (pose probed)" : "", hits, probes, placed ? "placed" : "NOT where the entity is");
		if (ok) return TestCheck(true, "mover collision reached Minecraft", buf);
		detail = buf;
	}
	if (!seen) std::snprintf(buf, sizeof buf, "no mover named or of class %s (movers are listed once a second; a light bridge exists only while enabled)", args[1]);
	else std::snprintf(buf, sizeof buf, "%s", detail.c_str());
	TestCheck(false, "mover collision reached Minecraft", buf);
}
// (bridge #43) pcb_mover_dig <#id|name|@x,y,z> <cx cy cz> [0|1]: mine (1) or fill back (0) a cell of a mover's dig lattice from here, as the mod's message would
CON_COMMAND(pcb_mover_dig, "pcb_mover_dig <#id|targetname|@x,y,z> <cx cy cz> [0|1] [kind] - (bridge #43) mine (1) or fill (0) a cell of a mover's dig lattice on this side (the mod is not asked); with no cell: print the mover's lattice\n") {
	if (args.ArgC() < 2) return;
	uint32_t id = 0;
	if (!MoverFind(args[1], &id) || !id) return console->Print("[bridge] mover dig: no mover '%s'\n", args[1]);
	const Mover *m = MoverById(id);
	if (!m) return;
	if (args.ArgC() < 5) {
		float a[3] = {0, 0, 0};
		if (m->shape) ShapeAnchor(m->shape, a);
		return console->Print("[bridge] mover dig: %s %s #%u shape %u (%zu boxes), effective %u, lattice anchor (%.3f %.3f %.3f) MC blocks, %zu cells mined, %zu pieces out of the picture\n", m->cls.c_str(), m->name.c_str(), id, m->shape,
		                      m->shape ? g_shapes[m->shape - 1].boxes.size() : 0, Eff(*m), a[0], a[1], a[2], m->dug.size(), m->holeBoxes.size());
	}
	const uint32_t kind = args.ArgC() > 6 ? (uint32_t)std::atoi(args[6]) & 0xFFu : 1u;  // (bridge #50) the material of the walls (ChamberMaterial 1..6), default panel white
	RenMoverDig h{id, std::atoi(args[2]), std::atoi(args[3]), std::atoi(args[4]), DigWorld(), (args.ArgC() > 5 && !std::atoi(args[5]) ? kMoverDigFill : 0u) | (kind << kMoverDigKindShift)};
	MoverDigOnMessage((const uint8_t *)&h, sizeof h);
}
CON_COMMAND(pcb_movers, "pcb_movers [near <r>|all|write] - (bridge #26) the movers: shapes, binds, poses; write: cache/bridge/movers_<map>.txt\n") {
	const char *a = args.ArgC() > 1 ? args[1] : "near";
	console->Print("[bridge] movers %s: %zu listed, %zu shapes (%d built, %d from disk, %.0f ms building), %d shapes / %d binds sent, %d posed last frame, building %s\n", g_on ? "on" : "off",
	               g_movers.size(), g_shapes.size(), g_shapesBuilt, g_shapesLoaded, g_buildMsTotal, g_shapesSent, g_bindsSent, g_posesLast,
	               g_build.active ? g_build.key.c_str() : "-");
	if (!std::strcmp(a, "write")) {
		const std::string path = CacheDir() + "/movers_" + McMapName() + ".txt";
		FILE *f = std::fopen(path.c_str(), "w");
		if (!f) return;
		std::fprintf(f, "# movers in %s (bridge #26): one line per mover\n", McMapName());
		for (const Mover &m : g_movers)
			if (m.ent) PrintMover(m, f);
		std::fclose(f);
		return console->Print("[bridge] movers written to %s\n", path.c_str());
	}
	const float r = !std::strcmp(a, "all") ? 1e9f : args.ArgC() > 2 ? (float)std::atof(args[2]) : 400.0f;
	for (const Mover &m : g_movers)
		if (m.ent && (m.origin - PlayerOrigin()).Length() - m.radius < r) PrintMover(m, nullptr);
}
// (bridge #68) every solid, classified mover of the level has its shape (built or loaded from the disk cache) and none is being built: the prebuild
// scripts (tools/cfg/w68_prebuild_all.txt) wait for this on each map so the shapes of the whole campaign are in cache/bridge/movershapes before a player gets there
bool MoversAllBuilt() {
	static double lastLog = 0;
	const double nowT = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	const bool logIt = nowT - lastLog > 10.0;
	if (g_build.active) {
		if (logIt) lastLog = nowT, console->Print("[bridge] movers: prebuild waits: a shape is being built\n");
		return false;
	}
	bool any = false;
	for (const Mover &m : g_movers) {
		if (!m.ent) continue;
		any = true;
		if (m.classified && m.solid && !m.passable && !m.shape) {
			if (logIt) lastLog = nowT, console->Print("[bridge] movers: prebuild waits for %s %s (shape none, solid %d passable %d)\n", m.cls.c_str(), m.name.c_str(), m.solid, m.passable);
			return false;
		}  // (the pick rule of the build queue: what it would still build)
	}
	return any;
}
CON_COMMAND(pc_movers, "pc_movers [0|1] [build ms] [urgent build ms] [pose radius u] - (bridge #26) movers on/off and budgets\n") {
	if (args.ArgC() > 1) g_on = std::atoi(args[1]) != 0;
	if (args.ArgC() > 2) g_buildMs = std::atof(args[2]);
	if (args.ArgC() > 3) g_buildUrgentMs = std::atof(args[3]);
	if (args.ArgC() > 4) g_poseRadius = (float)std::atof(args[4]);
	console->Print("[bridge] movers %d, build %.1f ms/frame (%.1f urgent), pose radius %.0f u\n", g_on, g_buildMs, g_buildUrgentMs, g_poseRadius);
}
CON_COMMAND(pcb_assert_movers_ready, "pcb_assert_movers_ready [radius=320] - (tests, bridge #26) every mover around the player has its collision in Minecraft\n") {
	char why[200] = "ready";
	const bool ok = MoversReadyNear(PlayerOrigin(), args.ArgC() > 1 ? (float)std::atof(args[1]) : 320.0f, why, sizeof why);
	TestCheck(ok, "the movers around the player are in Minecraft", why);
}

}  // namespace bridge
namespace bridge {
CON_COMMAND(pcb_mover_probe, "pcb_mover_probe <#id|name|@x,y,z> [x y z half] - (bridge #26) one-entity box test against a mover (default: Portal 2's player hull)\n") {
	if (args.ArgC() < 2) return;
	uint32_t id = 0;
	void *e = MoverFind(args[1], &id);
	if (!e) return console->Print("[bridge] mover probe: no mover %s\n", args[1]);
	Vector p = PlayerOrigin() + Vector(0, 0, 36), h(12, 12, 28);
	if (args.ArgC() > 4) p = Vector((float)std::atof(args[2]), (float)std::atof(args[3]), (float)std::atof(args[4]));
	if (args.ArgC() > 5) h = Vector((float)std::atof(args[5]), (float)std::atof(args[5]), (float)std::atof(args[5]));
	Vector mn(0, 0, 0), mx(0, 0, 0);
	BoundsLocal(e, mn, mx);
	const Vector o = AbsOrigin(e);
	console->Print("[bridge] mover probe #%u %s at (%.1f %.1f %.1f) bounds (%.0f %.0f %.0f)..(%.0f %.0f %.0f): box (%.1f %.1f %.1f) half (%.0f %.0f %.0f) hits %d, centre point %d; player at (%.1f %.1f %.1f)\n", id,
	               EntKV(e, "classname").c_str(), o.x, o.y, o.z, mn.x, mn.y, mn.z, mx.x, mx.y, mx.z, p.x, p.y, p.z, h.x, h.y, h.z, BoxHitsEnt(e, p, h) ? 1 : 0,
	               BoxHitsEnt(e, p, Vector(0, 0, 0)) ? 1 : 0, PlayerOrigin().x, PlayerOrigin().y, PlayerOrigin().z);
}
}  // namespace bridge
