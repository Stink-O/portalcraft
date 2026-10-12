// Portalcraft bridge (bridge #24): the map index and the targeting commands tests use instead of guessed coordinates.
//
// At level load the BSP's entity lump (lump 0) and model lump (lump 14, brush bounds) are read from the map file, and
// every named entity is indexed: targetname, classname, origin, angles and, for brush entities, the world bounds of
// its model. Once the level is live, each entity gets a floor sample (a downward trace) and a coarse grid of floor
// heights is traced across the map's world bounds (sliced over frames). Written to cache/bridge/mapindex/<map>.txt.
//
// Commands (see bridge/TESTING.md):
//   pcb_find <substring>                       index entries matching a name or class, with coordinates
//   pcb_goto <target> [dist=64] [side]         noclip setpos to a standable spot near the target (hull + floor traces)
//   pcb_face <target>                          exact yaw/pitch from the eye at the target (both modes; Minecraft follows)
//   pcb_assert_facing <target> [tol_deg=2]     the view points at the target
//   pcb_assert_past <target> <axis> [margin]   the player is beyond the target's plane / box on that axis
//   waituntil near <target> <dist> [ticks]     (script.cpp) the player is within dist of the target
// A target is a targetname (the live entity's current origin is used when it exists) or "x y z".
#include "bridge.hpp"
#include "coords.hpp"
#include "link.hpp"

#include "Modules/Tier1.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include "platform/platform.hpp"
#include <unistd.h>
#include <vector>

namespace bridge {

bool McModeActive();                       // mc.cpp
Vector McFeetSrc();                        // mc.cpp
const skycraft::proto::McState &McStateNow();  // mc.cpp
bool PlayerNoclip();                       // puppet.cpp
void TestCheck(bool ok, const char *what, const char *detail);  // testassert.cpp
int EntFieldOffset(void *ent, const char *field);  // ents.cpp
// (bridge #70) the outputs of the map's logic_playerproxy entities that Portal 2 fires from the player's movement (OnJump, OnDuck, OnUnDuck), read from the BSP's
// entity lump: playerevents.cpp replays them when a relayed jump / duck did not reach Portal 2's own movement code.
static std::vector<PlayerProxyConn> g_proxyConnsStore;
const std::vector<PlayerProxyConn> &MapPlayerProxyConns() { return g_proxyConnsStore; }

namespace {

struct MapEnt {
	std::string name, cls, model;
	Vector origin{0, 0, 0};
	QAngle angles{0, 0, 0};
	bool hasBox = false;
	Vector lmn{0, 0, 0}, lmx{0, 0, 0};  // model bounds, local to the origin (brush entities)
	Vector mn{0, 0, 0}, mx{0, 0, 0};    // world bounds at the map's own origin/angles
	float floorZ = NAN;                 // the floor found under it (NAN: none)
};

std::vector<MapEnt> g_ents;
std::string g_mapName, g_bsp;
bool g_haveWorld = false;
Vector g_worldMn, g_worldMx;
int g_unnamed = 0;

// floor grid (sliced over frames once the level is live)
struct Column {
	float x, y;
	std::vector<float> floors;  // standable floors (>= 72 u of headroom), top first
};
std::vector<Column> g_grid;
size_t g_gridNext = 0;
float g_gridStep = 64;
bool g_live = false, g_entFloorsDone = false, g_gridDone = false, g_written = false;
double g_gridMs = 0;

double NowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---- Source angle helpers ----------------------------------------------------------------------------------
void AngleBasis(const QAngle &a, Vector &f, Vector &r, Vector &u) {
	const float p = a.x * (float)M_PI / 180, y = a.y * (float)M_PI / 180, ro = a.z * (float)M_PI / 180;
	const float sp = std::sin(p), cp = std::cos(p), sy = std::sin(y), cy = std::cos(y), sr = std::sin(ro), cr = std::cos(ro);
	f = Vector(cp * cy, cp * sy, -sp);
	r = Vector(-sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp);
	u = Vector(cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp);
}
// world AABB of a local box placed at origin o with angles a
void PlaceBox(const Vector &lmn, const Vector &lmx, const Vector &o, const QAngle &a, Vector &mn, Vector &mx) {
	if (std::fabs(a.x) < 0.01f && std::fabs(a.y) < 0.01f && std::fabs(a.z) < 0.01f) {
		mn = o + lmn, mx = o + lmx;
		return;
	}
	Vector f, r, u;
	AngleBasis(a, f, r, u);
	mn = Vector(1e9f, 1e9f, 1e9f), mx = Vector(-1e9f, -1e9f, -1e9f);
	for (int k = 0; k < 8; ++k) {
		const float x = (k & 1) ? lmx.x : lmn.x, y = (k & 2) ? lmx.y : lmn.y, z = (k & 4) ? lmx.z : lmn.z;
		const Vector p = o + f * x - r * y + u * z;  // (Source: local y is left = -right)
		for (int i = 0; i < 3; ++i) mn[i] = std::fmin(mn[i], p[i]), mx[i] = std::fmax(mx[i], p[i]);
	}
}

// ---- the BSP file ------------------------------------------------------------------------------------------
std::string FindBsp(const std::string &map) {
	char cwd[1024] = "";
	if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
	// the game's working directory, then the folder of the running portal2_linux (the Portal 2 install, wherever Steam put it)
	std::string exeDir = plat::ExecutablePath();
	exeDir = exeDir.substr(0, exeDir.find_last_of('/') == std::string::npos ? 0 : exeDir.find_last_of('/'));
	const std::string roots[] = {std::string(cwd), exeDir};
	const char *games[] = {"update", "portal2_dlc2", "portal2_dlc1", "portal2"};
	for (const std::string &r : roots)
		for (const char *g : games) {
			const std::string p = r + "/" + g + "/maps/" + map + ".bsp";
			struct stat st;
			if (stat(p.c_str(), &st) == 0 && st.st_size > 1036) return p;
		}
	return "";
}

bool ReadLump(FILE *f, const int lumps[64][2], int i, std::string &out) {
	if (lumps[i][0] <= 0 || lumps[i][1] <= 0 || lumps[i][1] > 64 * 1024 * 1024) return false;
	out.resize((size_t)lumps[i][1]);
	return std::fseek(f, lumps[i][0], SEEK_SET) == 0 && std::fread(&out[0], 1, out.size(), f) == out.size();
}

bool ParseVec(const std::string &s, float v[3]) { return std::sscanf(s.c_str(), "%f %f %f", &v[0], &v[1], &v[2]) == 3; }

void Load(const std::string &map) {
	g_ents.clear(), g_grid.clear(), g_gridNext = 0, g_unnamed = 0, g_proxyConnsStore.clear();
	g_haveWorld = false, g_live = false, g_entFloorsDone = false, g_gridDone = false, g_written = false, g_gridMs = 0;
	g_mapName = map, g_bsp = FindBsp(map);
	if (g_bsp.empty()) return void(console->Print("[bridge] mapindex: no BSP file found for %s\n", map.c_str()));
	FILE *f = std::fopen(g_bsp.c_str(), "rb");
	if (!f) return;
	char hdr[8 + 64 * 16];
	int lumps[64][2] = {};
	std::string ents, models;
	const bool ok = std::fread(hdr, 1, sizeof hdr, f) == sizeof hdr && !std::memcmp(hdr, "VBSP", 4);
	if (ok) {
		for (int i = 0; i < 64; ++i) std::memcpy(lumps[i], hdr + 8 + i * 16, 8);  // lump_t: fileofs, filelen, version, fourCC
		ReadLump(f, lumps, 0, ents);
		ReadLump(f, lumps, 14, models);
	}
	std::fclose(f);
	if (!ok || ents.empty()) return void(console->Print("[bridge] mapindex: %s is not a readable VBSP\n", g_bsp.c_str()));
	const size_t nModels = models.size() / 48;  // dmodel_t: mins, maxs, origin (floats), headnode, firstface, numfaces
	// the entity text: { "key" "value" ... } blocks
	size_t i = 0;
	const size_t n = ents.size();
	auto quoted = [&](std::string &s) {
		while (i < n && ents[i] != '"' && ents[i] != '}' && ents[i] != '{') ++i;
		if (i >= n || ents[i] != '"') return false;
		const size_t b = ++i;
		while (i < n && ents[i] != '"') ++i;
		s.assign(ents, b, i - b);
		if (i < n) ++i;
		return true;
	};
	while (i < n) {
		while (i < n && ents[i] != '{') ++i;
		if (i >= n) break;
		++i;
		MapEnt e;
		std::string k, v, origin, angles, angle;
		std::vector<std::pair<std::string, std::string>> outs;  // (bridge #70) OnJump / OnDuck / OnUnDuck of this entity
		for (;;) {
			while (i < n && std::isspace((unsigned char)ents[i])) ++i;
			if (i >= n || ents[i] == '}') break;
			if (!quoted(k) || !quoted(v)) break;
			if (k == "OnJump" || k == "OnDuck" || k == "OnUnDuck") outs.push_back({k, v});
			if (k == "targetname") e.name = v;
			else if (k == "classname") e.cls = v;
			else if (k == "model") e.model = v;
			else if (k == "origin") origin = v;
			else if (k == "angles") angles = v;
			else if (k == "angle") angle = v;
			else if (k == "world_mins" && e.name.empty()) ParseVec(v, &g_worldMn.x), g_haveWorld = true;
			else if (k == "world_maxs" && e.name.empty()) ParseVec(v, &g_worldMx.x);
		}
		if (i < n) ++i;
		if (e.cls == "logic_playerproxy")
			for (const auto &o : outs) {
				// "target<sep>input<sep>parameter<sep>delay<sep>times": the separator is ESC (0x1B) in Portal 2's maps, ',' in older ones
				const char sep = o.second.find('\x1b') != std::string::npos ? '\x1b' : ',';
				std::vector<std::string> f;
				size_t b = 0;
				for (;;) {
					const size_t e2 = o.second.find(sep, b);
					f.push_back(o.second.substr(b, e2 == std::string::npos ? std::string::npos : e2 - b));
					if (e2 == std::string::npos) break;
					b = e2 + 1;
				}
				if (f.size() < 2 || f[0].empty() || f[1].empty()) continue;
				PlayerProxyConn c;
				c.output = o.first, c.target = f[0], c.input = f[1];
				c.param = f.size() > 2 ? f[2] : "";
				c.delay = f.size() > 3 ? (float)std::atof(f[3].c_str()) : 0.0f;
				c.times = f.size() > 4 ? std::atoi(f[4].c_str()) : -1;
				g_proxyConnsStore.push_back(c);
			}
		if (e.name.empty()) {
			++g_unnamed;
			continue;
		}
		if (!origin.empty()) ParseVec(origin, &e.origin.x);
		if (!angles.empty()) ParseVec(angles, &e.angles.x);
		else if (!angle.empty()) e.angles.y = (float)std::atof(angle.c_str());
		if (e.model.size() > 1 && e.model[0] == '*') {
			const size_t m = (size_t)std::atoi(e.model.c_str() + 1);
			if (m < nModels) {
				const float *mm = (const float *)(models.data() + m * 48);
				e.lmn = Vector(mm[0], mm[1], mm[2]), e.lmx = Vector(mm[3], mm[4], mm[5]);
				e.hasBox = e.lmx.x >= e.lmn.x && e.lmx.y >= e.lmn.y && e.lmx.z >= e.lmn.z;
				if (e.hasBox) PlaceBox(e.lmn, e.lmx, e.origin, e.angles, e.mn, e.mx);
			}
		}
		g_ents.push_back(std::move(e));
	}
	console->Print("[bridge] mapindex: %s: %zu named entities (%d unnamed), %zu brush models, world %.0f %.0f %.0f .. %.0f %.0f %.0f (%s)\n", map.c_str(), g_ents.size(),
	               g_unnamed, nModels, g_worldMn.x, g_worldMn.y, g_worldMn.z, g_worldMx.x, g_worldMx.y, g_worldMx.z, g_bsp.c_str());
}

// ---- traces --------------------------------------------------------------------------------------------------
void *g_playerEnt = nullptr;
void *PlayerEnt() {
	static double at = -1;
	if (NowS() - at > 1.0) {
		at = NowS();
		auto v = EntFind("player");
		g_playerEnt = v.empty() ? nullptr : v[0];
	}
	return g_playerEnt;
}
// everything (world, static props, entities) but the player; or the world only
void TraceX(const Vector &s, const Vector &e, const Vector &half, unsigned mask, CGameTrace &tr, bool everything, void *ignore = nullptr) {
	struct Filter : public ITraceFilter {
		void *skip, *skip2;
		bool every;
		virtual bool ShouldHitEntity(void *ent, int) { return every && ent != skip && ent != skip2; }
		virtual TraceType_t GetTraceType() const { return every ? TRACE_EVERYTHING : TRACE_WORLD_ONLY; }
	} filter;
	filter.skip = PlayerEnt(), filter.skip2 = ignore, filter.every = everything;
	Ray_t ray;
	const Vector d = e - s;
	ray.m_Delta = VectorAligned(d.x, d.y, d.z);
	ray.m_IsSwept = d.SquaredLength() != 0;
	ray.m_Extents = VectorAligned(half.x, half.y, half.z);
	ray.m_IsRay = half.SquaredLength() < 1e-6f;
	ray.m_StartOffset = VectorAligned(0, 0, 0);
	ray.m_Start = VectorAligned(s.x, s.y, s.z);
	ray.m_pWorldAxisTransform = nullptr;
	std::memset(&tr, 0, sizeof tr);
	engine.TraceRay(engine.traceServer->ThisPtr(), ray, mask, &filter, &tr);
}

// Floors in a column, top first: up-facing surfaces under zTop with at least `headroom` u of open space above them.
// A ray that starts in solid continues from where it leaves it (fractionleftsolid; 8 u steps if not reported).
std::vector<float> ColumnFloors(float x, float y, float zTop, float zBot, bool everything, float headroom, int *traces = nullptr) {
	std::vector<float> out;
	float z = zTop, ceil = zTop;
	for (int it = 0; it < 96 && z > zBot; ++it) {
		CGameTrace tr;
		TraceX(Vector(x, y, z), Vector(x, y, zBot), Vector(0, 0, 0), MASK_PLAYERSOLID, tr, everything);
		if (traces) ++*traces;
		if (tr.startsolid) {
			const float left = tr.fractionleftsolid;
			const float exitZ = left > 0.0f && left < 1.0f ? z - left * (z - zBot) : z - 8.0f;
			z = std::fmin(exitZ, z - 0.25f) - 0.25f;
			ceil = z;
			continue;
		}
		if (tr.fraction >= 1.0f) break;
		const float hz = tr.endpos.z;
		if (tr.plane.normal.z > 0.7f && ceil - hz >= headroom) out.push_back(hz);
		z = hz - 0.5f;  // into the surface: the next trace starts in it and leaves it at its underside
		ceil = z;
	}
	return out;
}

// the player's standing hull is clear at these feet, and there is floor right under them
bool Standable(const Vector &feet, std::string *why = nullptr) {
	CGameTrace tr;
	TraceX(feet + Vector(0, 0, 36.5f), feet + Vector(0, 0, 36.5f), Vector(16, 16, 36), MASK_PLAYERSOLID, tr, true);
	const char *bad = nullptr;
	if (tr.startsolid || tr.allsolid) bad = "hull blocked";
	else if (engine.PointOutsideWorld(engine.traceServer->ThisPtr(), feet + Vector(0, 0, 36))) bad = "outside the map";
	else {
		CGameTrace fl;
		TraceX(feet + Vector(0, 0, 2), feet - Vector(0, 0, 24), Vector(16, 16, 1), MASK_PLAYERSOLID, fl, true);
		if (fl.startsolid || fl.fraction >= 1.0f || fl.plane.normal.z < 0.7f) bad = "no floor under the feet";
	}
	if (bad && why) *why = bad;
	return !bad;
}

// ---- floors (level live) -----------------------------------------------------------------------------------
void EntityFloors() {
	for (MapEnt &e : g_ents) {
		const Vector top = e.hasBox ? Vector((e.mn.x + e.mx.x) * 0.5f, (e.mn.y + e.mx.y) * 0.5f, e.mx.z + 2) : e.origin + Vector(0, 0, 8);
		CGameTrace tr;
		TraceX(top, top - Vector(0, 0, 4096), Vector(4, 4, 1), MASK_PLAYERSOLID, tr, true);
		e.floorZ = !tr.startsolid && tr.fraction < 1.0f && tr.plane.normal.z > 0.7f ? tr.endpos.z - 1.0f : NAN;
	}
	g_entFloorsDone = true;
}
void GridStart() {
	g_grid.clear(), g_gridNext = 0, g_gridDone = false;
	if (!g_haveWorld) return void(g_gridDone = true);
	// at most ~6000 columns: 64 u spacing, coarser on big maps
	const float w = g_worldMx.x - g_worldMn.x, h = g_worldMx.y - g_worldMn.y;
	g_gridStep = 64;
	while ((w / g_gridStep + 1) * (h / g_gridStep + 1) > 6000) g_gridStep *= 2;
	for (float y = g_worldMn.y + g_gridStep * 0.5f; y < g_worldMx.y; y += g_gridStep)
		for (float x = g_worldMn.x + g_gridStep * 0.5f; x < g_worldMx.x; x += g_gridStep) g_grid.push_back({x, y, {}});
}
void GridStep(double budgetMs) {
	const auto t0 = std::chrono::steady_clock::now();
	const auto dl = t0 + std::chrono::microseconds((long long)(budgetMs * 1000));
	while (g_gridNext < g_grid.size()) {
		Column &c = g_grid[g_gridNext++];
		c.floors = ColumnFloors(c.x, c.y, g_worldMx.z - 1, g_worldMn.z, false, 72.0f);
		if (std::chrono::steady_clock::now() > dl) break;
	}
	g_gridMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	if (g_gridNext >= g_grid.size()) g_gridDone = true;
}

void Write() {
	if (g_mapName.empty()) return;
	const std::string dir = CacheDir() + "/mapindex";
	plat::MakeDir(dir);
	const std::string path = dir + "/" + g_mapName + ".txt";
	FILE *f = std::fopen(path.c_str(), "w");
	if (!f) return;
	std::fprintf(f, "# map index (bridge #24): %s from %s\n", g_mapName.c_str(), g_bsp.c_str());
	std::fprintf(f, "# world bounds %.0f %.0f %.0f .. %.0f %.0f %.0f; %zu named entities (%d unnamed not listed)\n", g_worldMn.x, g_worldMn.y, g_worldMn.z, g_worldMx.x,
	             g_worldMx.y, g_worldMx.z, g_ents.size(), g_unnamed);
	std::fprintf(f, "# ent <targetname> <classname> origin <x y z> angles <p y r> [box <x0 y0 z0> <x1 y1 z1>] floor <z|->\n");
	std::fprintf(f, "#   origin/angles/box as in the BSP (pcb_goto/pcb_face use the live entity's origin when it exists); floor = a downward\n");
	std::fprintf(f, "#   trace from the origin (point entities) or from the top centre of the box (brush entities)\n");
	for (const MapEnt &e : g_ents) {
		std::fprintf(f, "ent %s %s origin %.1f %.1f %.1f angles %.1f %.1f %.1f", e.name.c_str(), e.cls.c_str(), e.origin.x, e.origin.y, e.origin.z, e.angles.x, e.angles.y, e.angles.z);
		if (e.hasBox) std::fprintf(f, " box %.1f %.1f %.1f %.1f %.1f %.1f", e.mn.x, e.mn.y, e.mn.z, e.mx.x, e.mx.y, e.mx.z);
		if (std::isnan(e.floorZ)) std::fprintf(f, " floor -\n");
		else std::fprintf(f, " floor %.1f\n", e.floorZ);
	}
	if (g_gridDone && !g_grid.empty()) {
		std::fprintf(f, "# floors: a %.0f u grid over the world bounds (world brushes only, no props or entities); floor <x> <y> <z top first...>\n", g_gridStep);
		std::fprintf(f, "#   only up-facing surfaces with >= 72 u of open space above them; columns with none are left out (%.0f ms to trace)\n", g_gridMs);
		for (const Column &c : g_grid) {
			if (c.floors.empty()) continue;
			std::fprintf(f, "floor %.0f %.0f", c.x, c.y);
			for (float z : c.floors) std::fprintf(f, " %.1f", z);
			std::fprintf(f, "\n");
		}
	} else {
		std::fprintf(f, "# floors: not traced yet\n");
	}
	std::fclose(f);
	console->Print("[bridge] mapindex: wrote %s (%zu entities%s)\n", path.c_str(), g_ents.size(), g_gridDone ? ", floor grid" : "");
}

// ---- targets ---------------------------------------------------------------------------------------------------
struct Target {
	std::string what;
	Vector point{0, 0, 0};          // the aim point: the box centre or the point
	bool box = false;
	Vector mn{0, 0, 0}, mx{0, 0, 0};  // box (or the point twice)
	QAngle ang{0, 0, 0};
	bool hasAng = false;
	void *ent = nullptr;            // the live entity, if any
	const MapEnt *idx = nullptr;
};
bool IsNum(const char *s) {
	char *end = nullptr;
	std::strtod(s, &end);
	return end && end != s && *end == 0;
}
const MapEnt *FindIdx(const char *name) {
	for (const MapEnt &e : g_ents)
		if (e.name == name) return &e;
	return nullptr;
}
bool LiveBounds(void *ent, Vector &mn, Vector &mx) {
	static int offMin = -2, offMax = -2;
	if (offMin == -2) offMin = EntFieldOffset(ent, "m_vecMins"), offMax = EntFieldOffset(ent, "m_vecMaxs");
	if (offMin < 0 || offMax < 0) return false;
	mn = *(const Vector *)((const char *)ent + offMin), mx = *(const Vector *)((const char *)ent + offMax);
	return std::isfinite(mn.x + mx.x + mn.y + mx.y + mn.z + mx.z) && mx.x >= mn.x && mx.y >= mn.y && mx.z >= mn.z && (mx - mn).Length() > 0.5f && (mx - mn).Length() < 8192.0f;
}
// args[i..]: a targetname or "x y z"; advances i
bool Resolve(const CCommand &args, int &i, Target &t, std::string &err) {
	if (i >= args.ArgC()) return err = "no target", false;
	if (i + 2 < args.ArgC() && IsNum(args[i]) && IsNum(args[i + 1]) && IsNum(args[i + 2])) {
		t.point = Vector((float)std::atof(args[i]), (float)std::atof(args[i + 1]), (float)std::atof(args[i + 2]));
		t.mn = t.mx = t.point;
		char b[96];
		std::snprintf(b, sizeof b, "(%.1f %.1f %.1f)", t.point.x, t.point.y, t.point.z);
		t.what = b;
		i += 3;
		return true;
	}
	const char *name = args[i++];
	t.what = name;
	t.idx = FindIdx(name);
	auto live = EntFind(nullptr, name);
	t.ent = live.empty() ? nullptr : live[0];
	if (!t.idx && !t.ent) return err = std::string("no entity named ") + name + " (pcb_find lists the index)", false;
	Vector o = t.idx ? t.idx->origin : Vector(0, 0, 0);
	QAngle a = t.idx ? t.idx->angles : QAngle{0, 0, 0};
	if (t.ent) {
		o = EntVec(t.ent, "origin");
		const std::string as = EntKV(t.ent, "angles");
		if (!as.empty()) std::sscanf(as.c_str(), "%f %f %f", &a.x, &a.y, &a.z);
	}
	t.ang = a, t.hasAng = true;
	if (t.idx && t.idx->hasBox) {
		PlaceBox(t.idx->lmn, t.idx->lmx, o, a, t.mn, t.mx);
		t.box = true;
	} else if (t.ent) {
		Vector lmn, lmx;
		if (LiveBounds(t.ent, lmn, lmx) && (lmx - lmn).Length() > 2.0f) PlaceBox(lmn, lmx, o, a, t.mn, t.mx), t.box = true;
	}
	if (t.box) t.point = (t.mn + t.mx) * 0.5f;
	else t.point = o, t.mn = t.mx = o;
	return true;
}

Vector EyeNow() {
	if (McModeActive()) {
		const float eye = McStateNow().tickEye;
		return McFeetSrc() + Vector(0, 0, (eye > 0.3f && eye < 3.0f ? eye : 1.62f) * kUnitsPerBlock);
	}
	return PlayerOrigin() + Vector(0, 0, 64);
}
void AnglesTo(const Vector &eye, const Vector &p, float &pitch, float &yaw) {
	const Vector d = p - eye;
	const float l = d.Length();
	pitch = l > 0.01f ? -std::asin(std::fmax(-1.0f, std::fmin(1.0f, d.z / l))) * 180.0f / (float)M_PI : 0;
	yaw = std::atan2(d.y, d.x) * 180.0f / (float)M_PI;
}
// distance from the player's standing hull to the target (0 when they overlap)
float HullDistance(const Target &t) {
	const Vector o = McModeActive() ? McFeetSrc() : PlayerOrigin();
	const Vector pmn = o + Vector(-16, -16, 0), pmx = o + Vector(16, 16, 72);
	float s = 0;
	for (int k = 0; k < 3; ++k) {
		float d = 0;
		if (pmx[k] < t.mn[k]) d = t.mn[k] - pmx[k];
		else if (t.mx[k] < pmn[k]) d = pmn[k] - t.mx[k];
		s += d * d;
	}
	return std::sqrt(s);
}

// pcb_goto in Minecraft mode: Chell is placed (the Minecraft player is switched out and back in)
int g_gotoStage = 0;
double g_gotoAt = 0;
char g_gotoCmd[256] = "";
bool g_gotoNoclipOff = false;
double g_noclipOffAt = -1;

}  // namespace

// (bridge #47) the level's BSP file (empty if none was found): stage.cpp reads the map's extent from it
const char *MapIndexBsp() { return g_bsp.c_str(); }

// ---- (bridge #34) the map's version: a hash of its BSP file ------------------------------------------------------
// The static collision cache (collfield.cpp, cache/bridge/colcache/<map>.bin) and the mover shapes of a map's brush models (movers.cpp) are
// kept on disk by map NAME; a rebuilt BSP (maps/tools/build_testbed.sh) kept serving the old geometry: after #32's rebuild the arrival shaft
// was still solid void in the cache (the column x 0, y -1824, z -900..20 solid, the fresh harvest empty above -835), so the Minecraft player
// stood embedded in it. A FNV-1a hash of the whole BSP file (size included; the pak lump too) changes with any rebuild and with nothing else.
// "" when the file is not found (callers then trust the cache, as before). Memoised per name + size + mtime: the BSP is read once per level load.
std::string MapVersionKey(const std::string &map) {
	static std::string s_map, s_key;
	static long long s_size = -1, s_mtime = -1;
	if (map.empty()) return "";
	const std::string path = FindBsp(map);
	struct stat st;
	if (path.empty() || stat(path.c_str(), &st) != 0) return "";
	if (map == s_map && s_size == (long long)st.st_size && s_mtime == (long long)st.st_mtime) return s_key;
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f) return "";
	uint64_t h = 1469598103934665603ull;
	std::vector<unsigned char> buf(1 << 20);
	size_t n;
	long long total = 0;
	while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
		for (size_t i = 0; i < n; ++i) h = (h ^ buf[i]) * 1099511628211ull;
		total += (long long)n;
	}
	std::fclose(f);
	char b[48];
	std::snprintf(b, sizeof b, "%016llx-%lld", (unsigned long long)h, total);
	s_map = map, s_key = b, s_size = (long long)st.st_size, s_mtime = (long long)st.st_mtime;
	return s_key;
}

void MapIndexLevelInit(const char *map) { Load(map ? map : ""); }

// (bridge #32, testbed.cpp) the index's named entities whose name starts with a prefix, and one entry's origin / angles as the BSP has them
void MapIndexNames(const char *prefix, std::vector<std::string> &out) {
	for (const MapEnt &e : g_ents)
		if (e.name.compare(0, std::strlen(prefix), prefix) == 0) out.push_back(e.name);
}
bool MapIndexLookup(const char *name, Vector &origin, QAngle &angles) {
	const MapEnt *e = FindIdx(name);
	if (!e) return false;
	origin = e->origin, angles = e->angles;
	return true;
}

// once per server frame (bugmark.cpp TestToolsFrame)
void TestbedFrame();  // testbed.cpp (bridge #32): pc_tb_goto's arrival check
void MapIndexFrame(bool levelLive) {
	TestbedFrame();
	if (!levelLive) return;
	if (!g_live) {
		g_live = true;
		if (!g_ents.empty() || g_haveWorld) {
			EntityFloors();
			Write();
			GridStart();
		}
	}
	if (!g_gridDone && !g_grid.empty()) {
		GridStep(1.0);
		if (g_gridDone) Write();
	}
	const double now = NowS();
	if (g_noclipOffAt > 0 && now >= g_noclipOffAt) {
		g_noclipOffAt = -1;
		engine.Cmd("pcb_noclip_set 0");
	}
	// Minecraft mode: 1 = switched to Chell, waiting; 2 = placed, waiting; then back to Minecraft
	if (g_gotoStage == 1 && !McModeActive() && now - g_gotoAt > 0.1) {
		engine.Cmd("pcb_noclip_set 1");
		engine.Cmd(g_gotoCmd);
		g_gotoStage = 2, g_gotoAt = now;
	} else if (g_gotoStage == 2 && now - g_gotoAt > 0.3) {
		if (g_gotoNoclipOff) engine.Cmd("pcb_noclip_set 0");
		engine.Cmd("bridge_mode mc");
		g_gotoStage = 0;
		console->Print("[bridge] goto: placed; back to the Minecraft player\n");
	} else if (g_gotoStage && now - g_gotoAt > 5.0) {
		g_gotoStage = 0;
		console->Print("[bridge] goto: gave up waiting for the switch to Chell\n");
	}
}

// waituntil near:<target>:<dist> (script.cpp)
bool MapIndexNear(const char *spec) {
	std::string s = spec;
	const size_t c = s.rfind(':');
	if (c == std::string::npos) return false;
	const float dist = (float)std::atof(s.c_str() + c + 1);
	std::string name = s.substr(0, c);
	// (a point target is written x,y,z)
	Target t;
	std::string err;
	float v[3];
	if (std::sscanf(name.c_str(), "%f,%f,%f", &v[0], &v[1], &v[2]) == 3) {
		t.point = t.mn = t.mx = Vector(v[0], v[1], v[2]);
	} else {
		t.idx = FindIdx(name.c_str());
		auto live = EntFind(nullptr, name.c_str());
		t.ent = live.empty() ? nullptr : live[0];
		if (!t.idx && !t.ent) return false;
		Vector o = t.ent ? EntVec(t.ent, "origin") : t.idx->origin;
		QAngle a = t.idx ? t.idx->angles : QAngle{0, 0, 0};
		if (t.idx && t.idx->hasBox) PlaceBox(t.idx->lmn, t.idx->lmx, o, a, t.mn, t.mx);
		else t.mn = t.mx = o;
	}
	return HullDistance(t) <= dist;
}

CON_COMMAND(pcb_find, "pcb_find <substring> - (tests) map-index entries whose targetname or classname contains it, with coordinates (cache/bridge/mapindex/<map>.txt)\n") {
	if (args.ArgC() < 2) return console->Print("%s", pcb_find.ThisPtr()->m_pszHelpString);
	std::string q = args[1];
	std::transform(q.begin(), q.end(), q.begin(), ::tolower);
	int shown = 0, total = 0;
	for (const MapEnt &e : g_ents) {
		std::string n = e.name + " " + e.cls;
		std::transform(n.begin(), n.end(), n.begin(), ::tolower);
		if (n.find(q) == std::string::npos) continue;
		++total;
		if (shown >= 40) continue;
		++shown;
		auto live = EntFind(nullptr, e.name.c_str());
		char box[128] = "";
		if (e.hasBox) std::snprintf(box, sizeof box, " box %.0f %.0f %.0f .. %.0f %.0f %.0f", e.mn.x, e.mn.y, e.mn.z, e.mx.x, e.mx.y, e.mx.z);
		char fl[32] = " floor -";
		if (!std::isnan(e.floorZ)) std::snprintf(fl, sizeof fl, " floor %.1f", e.floorZ);
		Vector lo = live.empty() ? e.origin : EntVec(live[0], "origin");
		console->Print("[bridge] find: %s (%s) origin %.1f %.1f %.1f angles %.0f %.0f %.0f%s%s%s\n", e.name.c_str(), e.cls.c_str(), e.origin.x, e.origin.y, e.origin.z,
		               e.angles.x, e.angles.y, e.angles.z, box, fl,
		               live.empty() ? " (not live)" : (lo - e.origin).Length() > 0.5f ? " (live: moved)" : "");
	}
	console->Print("[bridge] find '%s': %d matches%s in %s's index\n", args[1], total, total > shown ? " (first 40 shown)" : "", g_mapName.c_str());
}

CON_COMMAND(pcb_mapindex, "pcb_mapindex [write|reload] - (tests) the map index: status; write the file now; reload it from the BSP\n") {
	if (args.ArgC() > 1 && !std::strcmp(args[1], "reload")) {
		const std::string m = g_mapName;
		Load(m);
		g_live = false;
		return;
	}
	if (args.ArgC() > 1 && !std::strcmp(args[1], "write")) return Write();
	console->Print("[bridge] mapindex %s: %zu named entities, entity floors %s, floor grid %zu/%zu columns (%.0f u, %.0f ms)\n", g_mapName.c_str(), g_ents.size(), g_entFloorsDone ? "done" : "pending",
	               g_gridNext, g_grid.size(), g_gridStep, g_gridMs);
}

CON_COMMAND(pcb_goto,
            "pcb_goto <targetname | x y z> [dist=64] [side] [front|back|left|right|low|high] - (tests) noclip setpos to a standable spot dist u from the target (hull and floor\n"
            "  traces verify it), facing it. No side: the nearest standable spot with a line of sight. front/back/left/right: along the target's angles. low/high: the\n"
            "  lowest / highest floor around it. In Minecraft mode Chell is placed and the Minecraft player switched back in (then: waituntil mcleads).\n") {
	if (args.ArgC() < 2) return console->Print("%s", pcb_goto.ThisPtr()->m_pszHelpString);
	int i = 1;
	Target t;
	std::string err;
	if (!Resolve(args, i, t, err)) return TestCheck(false, "goto target", err.c_str());
	float dist = 64;
	std::string side;
	for (; i < args.ArgC(); ++i) {
		if (IsNum(args[i])) dist = (float)std::atof(args[i]);
		else if (!std::strcmp(args[i], "side")) continue;
		else side = args[i];
	}
	if (!side.empty() && side != "front" && side != "back" && side != "left" && side != "right" && side != "low" && side != "high")
		return TestCheck(false, "goto side", ("unknown side " + side).c_str());
	// candidate directions (horizontal unit vectors)
	std::vector<float> yaws;
	const float baseYaw = t.ang.y;
	if (side == "front" || side == "back" || side == "left" || side == "right") {
		const float s = side == "front" ? 0.0f : side == "back" ? 180.0f : side == "left" ? 90.0f : -90.0f;
		for (float off : {0.0f, 10.0f, -10.0f, 20.0f, -20.0f, 35.0f, -35.0f}) yaws.push_back(baseYaw + s + off);
	} else {
		for (int k = 0; k < 24; ++k) yaws.push_back(k * 15.0f);
	}
	const Vector from = McModeActive() ? McFeetSrc() : PlayerOrigin();
	const float startZ = std::fmax(t.mx.z, t.point.z) + 80.0f;
	const float botZ = std::fmin(t.mn.z, t.point.z) - 1024.0f;
	struct Cand {
		Vector feet;
		float yaw;
		bool los;
		float score;
	};
	std::vector<Cand> cands;
	int rejected = 0;
	std::string lastWhy;
	for (float yd : yaws) {
		const float yr = yd * (float)M_PI / 180.0f;
		const Vector d(std::cos(yr), std::sin(yr), 0);
		const float ext = std::fabs(d.x) * (t.mx.x - t.mn.x) * 0.5f + std::fabs(d.y) * (t.mx.y - t.mn.y) * 0.5f;
		const Vector c((t.mn.x + t.mx.x) * 0.5f, (t.mn.y + t.mx.y) * 0.5f, 0);
		const float r = ext + 16.0f + dist;
		const float x = c.x + d.x * r, y = c.y + d.y * r;
		std::vector<float> floors = ColumnFloors(x, y, startZ, botZ, true, 72.0f);
		if (floors.empty()) {
			++rejected, lastWhy = "no floor";
			continue;
		}
		// which floor: high (and the default) = the first under the start (on top of the target if it is solid); low = the first
		// floor at least 16 u under the target's bottom (at the foot of a ledge, not the pit beyond it)
		float fz = floors.front();
		if (side == "low") {
			fz = NAN;
			for (float z : floors)
				if (z <= t.mn.z - 16.0f) {
					fz = z;
					break;
				}
			if (std::isnan(fz)) {
				++rejected, lastWhy = "no floor under the target's bottom";
				continue;
			}
		}
		const Vector feet(x, y, fz + 1.0f);
		std::string why;
		if (!Standable(feet, &why)) {
			++rejected, lastWhy = why;
			continue;
		}
		// a line of sight from the eye to the target (playerclip is see-through)
		CGameTrace los;
		const Vector eye = feet + Vector(0, 0, 64);
		TraceX(eye, t.point, Vector(0, 0, 0), MASK_SOLID, los, true, t.ent);
		const bool see = los.fraction > 0.98f || (los.endpos.x >= t.mn.x - 2 && los.endpos.x <= t.mx.x + 2 && los.endpos.y >= t.mn.y - 2 && los.endpos.y <= t.mx.y + 2 &&
		                                         los.endpos.z >= t.mn.z - 2 && los.endpos.z <= t.mx.z + 2);
		float score;
		if (side == "low") score = -fz * 4.0f + (feet - from).Length() * 0.01f;  // (the floor closest under the target)
		else if (side == "high") score = -fz;
		else if (!side.empty()) score = std::fabs(std::remainder(yd - (yaws[0]), 360.0f));
		else score = (feet - from).Length();
		cands.push_back({feet, yd, see, score});
	}
	if (cands.empty()) {
		char b[200];
		std::snprintf(b, sizeof b, "%s: no standable spot %.0f u away (%d candidates rejected, last: %s)", t.what.c_str(), dist, rejected, lastWhy.c_str());
		return TestCheck(false, "goto", b);
	}
	std::stable_sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.los != b.los ? a.los : a.score < b.score; });
	const Cand &best = cands.front();
	float pitch, yaw;
	AnglesTo(best.feet + Vector(0, 0, 64), t.point, pitch, yaw);
	char cmd[256];
	std::snprintf(cmd, sizeof cmd, "setpos %.2f %.2f %.2f; setang %.2f %.2f 0; pcb_stop", best.feet.x, best.feet.y, best.feet.z, pitch, yaw);
	const bool wasNoclip = PlayerNoclip();
	if (McModeActive()) {
		std::snprintf(g_gotoCmd, sizeof g_gotoCmd, "%s", cmd);
		g_gotoNoclipOff = !wasNoclip;
		g_gotoStage = 1, g_gotoAt = NowS();
		engine.Cmd("bridge_mode chell");
	} else {
		engine.Cmd("pcb_noclip_set 1");
		engine.Cmd(cmd);
		if (!wasNoclip) g_noclipOffAt = NowS() + 0.25;  // (the drop is 1 u: feet on the floor)
	}
	char b[400];
	std::snprintf(b, sizeof b, "%s%s: feet (%.1f %.1f %.1f) on the floor at %.1f, %.0f u from the %s, bearing %.0f, line of sight %s, %zu standable of %zu; %s", t.what.c_str(),
	              side.empty() ? "" : (" " + side).c_str(), best.feet.x, best.feet.y, best.feet.z, best.feet.z - 1, dist, t.box ? "box" : "point", best.yaw, best.los ? "yes" : "NO",
	              cands.size(), yaws.size(), cmd);
	console->Print("[bridge] goto %s\n", b);
	if (!best.los) console->Print("[bridge] goto: WARNING no standable spot sees %s; the chosen one is blocked\n", t.what.c_str());
}

static Vector AimPart(const Target &t, const CCommand &args, int i) {
	if (i >= args.ArgC() || !t.box) return t.point;
	if (!std::strcmp(args[i], "bottom")) return Vector(t.point.x, t.point.y, t.mn.z);
	if (!std::strcmp(args[i], "top")) return Vector(t.point.x, t.point.y, t.mx.z);
	return t.point;
}
CON_COMMAND(pcb_face, "pcb_face <targetname | x y z> [center|bottom|top] - (tests) turn the view to the target (a box's centre, or its bottom / top face's centre) from the eye\n"
                      "  (exact yaw/pitch; Minecraft mode too: its view follows)\n") {
	int i = 1;
	Target t;
	std::string err;
	if (!Resolve(args, i, t, err)) return TestCheck(false, "face target", err.c_str());
	t.point = AimPart(t, args, i);
	const Vector eye = EyeNow();
	float pitch, yaw;
	AnglesTo(eye, t.point, pitch, yaw);
	engine.SetAngles(QAngle{pitch, yaw, 0});
	console->Print("[bridge] face %s at (%.1f %.1f %.1f) from the eye (%.1f %.1f %.1f), %.0f u: pitch %.2f yaw %.2f\n", t.what.c_str(), t.point.x, t.point.y, t.point.z, eye.x, eye.y,
	               eye.z, (t.point - eye).Length(), pitch, yaw);
}

CON_COMMAND(pcb_assert_facing, "pcb_assert_facing <targetname | x y z> [center|bottom|top] [tol_deg=2] - (tests) the view points at the target from the eye\n") {
	int i = 1;
	Target t;
	std::string err;
	if (!Resolve(args, i, t, err)) return TestCheck(false, "facing target", err.c_str());
	t.point = AimPart(t, args, i);
	if (i < args.ArgC() && !IsNum(args[i])) ++i;
	const float tol = i < args.ArgC() ? (float)std::atof(args[i]) : 2.0f;
	const Vector eye = EyeNow();
	float pitch, yaw;
	AnglesTo(eye, t.point, pitch, yaw);
	const QAngle va = engine.ViewAngles();
	const float dp = std::fabs(std::remainder(va.x - pitch, 360.0f)), dy = std::fabs(std::remainder(va.y - yaw, 360.0f));
	char b[220];
	std::snprintf(b, sizeof b, "%s: view %.2f %.2f, wanted %.2f %.2f (off %.2f / %.2f deg, tol %.1f)", t.what.c_str(), va.x, va.y, pitch, yaw, dp, dy, tol);
	TestCheck(dp <= tol && dy <= tol, "facing the target", b);
}

CON_COMMAND(pcb_assert_past, "pcb_assert_past <targetname | x y z> <axis +x|-x|+y|-y|+z|-z> [margin=0] - (tests) the player's origin is beyond the target's box (or the plane\n"
                             "  through the point) on that axis: +x = past its max x, -x = before its min x\n") {
	int i = 1;
	Target t;
	std::string err;
	if (!Resolve(args, i, t, err)) return TestCheck(false, "past target", err.c_str());
	if (i >= args.ArgC()) return TestCheck(false, "past axis", "no axis given");
	const char *ax = args[i++];
	const bool neg = ax[0] == '-';
	const char c = (ax[0] == '-' || ax[0] == '+') ? ax[1] : ax[0];
	const int k = c == 'x' ? 0 : c == 'y' ? 1 : c == 'z' ? 2 : -1;
	if (k < 0) return TestCheck(false, "past axis", ax);
	const float margin = i < args.ArgC() ? (float)std::atof(args[i]) : 0.0f;
	const Vector o = McModeActive() ? McFeetSrc() : PlayerOrigin();
	const float plane = neg ? t.mn[k] - margin : t.mx[k] + margin;
	const bool ok = neg ? o[k] < plane : o[k] > plane;
	char b[220];
	std::snprintf(b, sizeof b, "%s %s: player %c %.1f (origin %.1f %.1f %.1f), wanted %s %.1f", t.what.c_str(), ax, c, o[k], o.x, o.y, o.z, neg ? "<" : ">", plane);
	TestCheck(ok, "player past the target", b);
}

// for bug marks (bugmark.cpp): the nearest indexed entity to a point, and a target's aim point by name
std::string MapIndexNearest(const Vector &p, float *distOut) {
	const MapEnt *best = nullptr;
	float bd = 1e9f;
	for (const MapEnt &e : g_ents) {
		Vector c = e.hasBox ? (e.mn + e.mx) * 0.5f : e.origin;
		const float d = (c - p).Length();
		if (d < bd) bd = d, best = &e;
	}
	if (distOut) *distOut = bd;
	return best ? best->name + " (" + best->cls + ")" : "";
}

}  // namespace bridge
