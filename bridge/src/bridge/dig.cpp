// Portalcraft bridge (bridge #33): digging through the chamber's walls and floors. See dig.hpp.
//
// Design: REVIEW-freshEyes 6, after SkyCraft's model (skse/src/Dig.h, Dig.cpp: Minecraft owns a per-world set of dug cells and sends
// them per section as kRenDug; the host leaves them out of its collision and its picture). The cell set arrives from the Fabric mod
// (SkyDig); `pcb_dig_cell` and `pcb_dig_box` dig cells on this side alone, to test everything here without the mod.
#include "dig.hpp"

#include "colharvest.hpp"
#include "coords.hpp"
#include "link.hpp"

#include "Modules/Tier1.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_set>

namespace bridge {
bool BridgeSkip(const char *name);  // plugin.cpp (PCB_SKIP bisect aid)
bool McLevelIsLive();             // mc.cpp

using namespace skycraft::proto;

const char *McMapName();           // mc.cpp
bool McModeActive();               // mc.cpp
Vector McFeetSrc();                // mc.cpp
void TestCheck(bool ok, const char *what, const char *detail);  // testassert.cpp
// collfield.cpp
void CollisionFieldSetDug(std::shared_ptr<const colharvest::DugSet> set);
void CollisionFieldDugRegionDirty(int rx, int ry, int rz);
void CollisionFieldHarvestWant(bool on);
bool CollisionFieldHarvestActive();
void CollisionFieldSentIn(int bx0, int by0, int bz0, int bx1, int by1, int bz1, std::vector<ColBlock> &out);
int CollisionFieldRegionState(int rx, int ry, int rz);  // 0 never sent, 1 in Minecraft, 2 in Minecraft + a patch queued
int CollisionFieldDugResent();                           // regions re-exported because of a dug cell
void TerrainBandCellChanged(int x, int y, int z);         // terrain.cpp (bridge #73)

namespace {

int g_on = 1;  // (lead, 2026-10-03: the user wants digging in the main build) on by default; pc_dig 0 turns it off
uint32_t g_world = 0;  // this map's dig world id (a hash of the map name; 0 = none)
std::string g_worldMap;

struct Snap {
	colharvest::DugSet set;
	std::vector<std::array<int, 3>> cells;
	uint32_t gen = 0;
};
std::mutex g_mu;
std::shared_ptr<const Snap> g_snap;  // read under g_mu by any thread
// main-thread truth
std::map<std::tuple<int, int, int>, std::array<uint8_t, 512>> g_sections;  // the mod's bitset per section (bit x + 16z + 256y)
std::set<std::array<int, 3>> g_local;                                       // cells dug by pcb_dig_cell (this side only)
uint32_t g_gen = 0;
bool g_dirty = false;                       // the set changed: publish the snapshot, fizzle portals, resend regions
std::vector<std::array<int, 3>> g_newCells;  // cells that became dug since the last publish
std::set<std::tuple<int, int, int>> g_dirtyRegions;
int g_msgs = 0, g_cellsFromMod = 0, g_fizzled = 0;
uint32_t g_material = 0;                    // HostPlayer::digMaterial
std::shared_ptr<const std::vector<std::pair<Vector, Vector>>> g_boxes;  // Source boxes of every dug cell (render thread)
uint32_t g_boxesGen = 0;
double g_boxesMapSig = 0;

std::shared_ptr<const Snap> Get() {
	std::lock_guard<std::mutex> lk(g_mu);
	return g_snap;
}
uint32_t Hash(const char *s) {
	uint32_t h = 2166136261u;
	for (; s && *s; ++s) h = (h ^ (uint8_t)*s) * 16777619u;
	return h ? h : 1;
}
void CellBox(int bx, int by, int bz, Vector &mn, Vector &mx) {
	const Vector p = McToSrc(bx, by, bz), q = McToSrc(bx + 1, by + 1, bz + 1);
	mn = Vector(std::fmin(p.x, q.x), std::fmin(p.y, q.y), std::fmin(p.z, q.z));
	mx = Vector(std::fmax(p.x, q.x), std::fmax(p.y, q.y), std::fmax(p.z, q.z));
}
void MarkRegion(int bx, int by, int bz) {
	auto rf = [](int b) { return (int)std::floor(b / 8.0); };
	g_dirtyRegions.insert(std::make_tuple(rf(bx), rf(by), rf(bz)));
}

// Rebuild the published snapshot from the mod's sections plus the local cells.
void Rebuild() {
	auto snap = std::make_shared<Snap>();
	for (const auto &e : g_sections) {
		const int sx = std::get<0>(e.first), sy = std::get<1>(e.first), sz = std::get<2>(e.first);
		for (int i = 0; i < 512; ++i)
			for (int b = 0; b < 8; ++b)
				if (e.second[i] >> b & 1) {
					const int bit = i * 8 + b;
					const int x = bit & 15, z = (bit >> 4) & 15, y = bit >> 8;
					snap->cells.push_back({sx * 16 + x, sy * 16 + y, sz * 16 + z});
				}
	}
	for (const auto &c : g_local) snap->cells.push_back(c);
	for (const auto &c : snap->cells) snap->set.insert(colharvest::BlockKey(c[0], c[1], c[2]));
	snap->gen = ++g_gen;
	std::lock_guard<std::mutex> lk(g_mu);
	g_snap = snap;
}

// A portal that sat on a cell that is gone fizzles (the surface it rested on has no collision any more).
void FizzlePortalsOn(const std::vector<std::array<int, 3>> &cells) {
	if (cells.empty()) return;
	for (void *p : EntFind("prop_portal")) {
		if (EntKV(p, "Activated") != "1") continue;
		const Vector o = EntVec(p, "origin");
		for (const auto &c : cells) {
			Vector mn, mx;
			CellBox(c[0], c[1], c[2], mn, mx);
			if (o.x > mn.x - 12 && o.x < mx.x + 12 && o.y > mn.y - 12 && o.y < mx.y + 12 && o.z > mn.z - 12 && o.z < mx.z + 12) {
				EntInput(p, "Fizzle");
				++g_fizzled;
				console->Print("[bridge] dig: a portal at (%.0f %.0f %.0f) rested on the dug cell MC %d %d %d: it fizzles\n", o.x, o.y, o.z, c[0], c[1], c[2]);
				break;
			}
		}
	}
}

int g_logged = 0;

// How a cell reads in what Minecraft was sent: 0 = no host content (air, or its region is not exported), 1 = diggable host content,
// 2 = host content at the dig limit (kColBlockLimit, the edge of what Portal 2 can hold: Minecraft reveals it as bedrock). pop = solid sub-voxels of 512.
int CellExport(int bx, int by, int bz, int *pop = nullptr) {
	std::vector<ColBlock> v;
	CollisionFieldSentIn(bx, by, bz, bx, by, bz, v);
	int n = 0, nd = 0;
	for (const ColBlock &b : v) {
		for (int y = 0; y < 8; ++y) n += __builtin_popcountll(b.bits[y]);
		nd |= (b.pad & kColBlockLimit) != 0;
	}
	if (pop) *pop = n;
	return n == 0 ? 0 : nd ? 2 : 1;
}
// The player the tests care about: Chell's feet, or the Minecraft player's.
Vector FocusFeet(float &halfWidth) {
	if (McModeActive()) return halfWidth = 12.0f, McFeetSrc();
	return halfWidth = 16.0f, PlayerOrigin();
}
void ClearLocal() {
	for (const auto &c : g_local) MarkRegion(c[0], c[1], c[2]);
	for (const auto &e : g_sections)
		for (int i = 0; i < 512; ++i)
			for (int b = 0; b < 8; ++b)
				if (e.second[i] >> b & 1) {
					const int bit = i * 8 + b;
					MarkRegion(std::get<0>(e.first) * 16 + (bit & 15), std::get<1>(e.first) * 16 + (bit >> 8), std::get<2>(e.first) * 16 + ((bit >> 4) & 15));
				}
	g_local.clear(), g_sections.clear(), g_newCells.clear();
	g_dirty = true;
}

// What a Portal 2 surface is made of, from its material name (the engine trace's surface name), as the block family it digs into.
uint32_t MaterialFromName(const char *name) {
	if (!name || !*name) return kChPanelWhite | kChPortalableBit;
	std::string s(name);
	for (char &c : s) c = (char)std::tolower((unsigned char)c);
	if (s.rfind("maps/", 0) == 0) {  // compile-time patched copies: maps/<map>/<material>_<x>_<y>_<z>
		const size_t a = s.find('/', 5);
		if (a != std::string::npos) s = s.substr(a + 1);
	}
	auto has = [&](const char *t) { return s.find(t) != std::string::npos; };
	if (has("glass") || has("window")) return kChGlass;
	if (has("white_") || has("whitewall") || has("white01")) return kChPanelWhite | kChPortalableBit;
	if (has("black")) return kChPanelBlack;
	if (has("concrete")) return kChConcrete;
	if (s.rfind("metal/", 0) == 0 || s.rfind("anim_wp/", 0) == 0 || has("grate") || has("hull")) return kChMetal;
	if (has("tile")) return kChTile;
	if (s.rfind("plastic/", 0) == 0 || has("plasticwall")) return kChPanelWhite | kChPortalableBit;
	if (s.rfind("tools/", 0) == 0) return kChPanelWhite | kChPortalableBit;
	return kChTile;
}

}  // namespace

bool DigEnabled() { return g_on != 0; }
uint32_t DigWorld() { return g_world; }
bool DigAny() {
	auto s = Get();
	return s && !s->cells.empty();
}
uint32_t DigGen() {
	auto s = Get();
	return s ? s->gen : 0;
}
bool DigIsDugMc(int bx, int by, int bz) {
	auto s = Get();
	return s && !s->set.empty() && s->set.count(colharvest::BlockKey(bx, by, bz));
}
bool DigIsDugSrc(const Vector &p) {
	auto s = Get();
	if (!s || s->set.empty()) return false;
	const Vector m = SrcToMc(p);
	return s->set.count(colharvest::BlockKey((int)std::floor(m.x), (int)std::floor(m.y), (int)std::floor(m.z))) != 0;
}
void DigCellsNear(const Vector &mn, const Vector &mx, float pad, std::vector<std::pair<Vector, Vector>> &out) {
	out.clear();
	auto s = Get();
	if (!s || s->cells.empty()) return;
	// the MC block range of the grown box, then a lookup per block (the set is a hash set; cells are few)
	const Vector a = SrcToMc(Vector(mn.x - pad, mn.y - pad, mn.z - pad)), b = SrcToMc(Vector(mx.x + pad, mx.y + pad, mx.z + pad));
	const int x0 = (int)std::floor(std::fmin(a.x, b.x)), x1 = (int)std::floor(std::fmax(a.x, b.x)), y0 = (int)std::floor(std::fmin(a.y, b.y)), y1 = (int)std::floor(std::fmax(a.y, b.y)),
	          z0 = (int)std::floor(std::fmin(a.z, b.z)), z1 = (int)std::floor(std::fmax(a.z, b.z));
	if ((long)(x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > 4096) {  // a huge box: walk the list instead
		for (const auto &c : s->cells) {
			Vector cmn, cmx;
			CellBox(c[0], c[1], c[2], cmn, cmx);
			if (cmx.x > mn.x - pad && cmn.x < mx.x + pad && cmx.y > mn.y - pad && cmn.y < mx.y + pad && cmx.z > mn.z - pad && cmn.z < mx.z + pad) out.push_back({cmn, cmx});
		}
		return;
	}
	for (int y = y0; y <= y1; ++y)
		for (int z = z0; z <= z1; ++z)
			for (int x = x0; x <= x1; ++x)
				if (s->set.count(colharvest::BlockKey(x, y, z))) {
					Vector cmn, cmx;
					CellBox(x, y, z, cmn, cmx);
					out.push_back({cmn, cmx});
				}
}
bool DigNear(const Vector &mn, const Vector &mx, float pad) {
	if (!DigAny()) return false;
	static thread_local std::vector<std::pair<Vector, Vector>> tmp;
	DigCellsNear(mn, mx, pad, tmp);
	return !tmp.empty();
}
void DigAllBoxes(std::vector<std::pair<Vector, Vector>> &out) {
	std::shared_ptr<const std::vector<std::pair<Vector, Vector>>> b;
	{
		std::lock_guard<std::mutex> lk(g_mu);
		b = g_boxes;
	}
	if (b) out = *b;
	else out.clear();
}

void DigOnMessage(const uint8_t *p, uint32_t len) {
	if (len < sizeof(RenDug)) return;
	RenDug h;
	std::memcpy(&h, p, sizeof h);
	if (!g_on || h.worldId != g_world) return;  // another map's cells, or digging off
	++g_msgs;
	const auto key = std::make_tuple(h.sx, h.sy, h.sz);
	std::array<uint8_t, 512> bits{};
	if (h.count > 0 && len >= sizeof(RenDug) + 512) std::memcpy(bits.data(), p + sizeof(RenDug), 512);
	std::array<uint8_t, 512> old{};
	auto it = g_sections.find(key);
	if (it != g_sections.end()) old = it->second;
	if (old == bits) return;
	for (int i = 0; i < 512; ++i) {
		const uint8_t diff = old[i] ^ bits[i];
		for (int b = 0; diff && b < 8; ++b)
			if (diff >> b & 1) {
				const int bit = i * 8 + b;
				const int x = h.sx * 16 + (bit & 15), z = h.sz * 16 + ((bit >> 4) & 15), y = h.sy * 16 + (bit >> 8);
				MarkRegion(x, y, z);
				if (bits[i] >> b & 1) g_newCells.push_back({x, y, z}), ++g_cellsFromMod;
			}
	}
	if (h.count > 0) g_sections[key] = bits;
	else g_sections.erase(key);
	g_dirty = true;
}

// (bridge #38) A tunnel can leave the map: the body (the Minecraft player's puppet, or Chell) is then inside Portal 2's solid void, where the engine's
// own world says "solid" and "outside the world". Everything that reads the engine for the body's surroundings (the movement hooks' DigEngineFilter, her
// ground, the puppet's stuck checks) already answers from the exported voxels and the blocks next to dug cells; this only says when it happens, once per
// change, so a run's log shows it (and `pcb_dig_status` prints the state).
static bool g_bodyInVoid = false, g_bodyOutside = false, g_bodyEmbedded = false;
static int g_bodyFrames = 0, g_bodyLogged = 0, g_bodyVoidFrames = 0;
static void DigBodyWatch() {
	if (!g_on || ++g_bodyFrames % 15) return;
#ifdef _WIN32
	// Windows (Proton run 2026-10-11): IEngineTrace::PointOutsideWorld called while no level is loaded (the menu's background map is loading)
	// makes the engine print "Cannot load corrupted map." and the map fail. Nothing to watch before the level is live anyway.
	if (!McLevelIsLive()) return;
#endif
	const Vector o = McModeActive() ? McFeetSrc() : PlayerOrigin();
	const Vector c = o + Vector(0, 0, 36);
	const bool solid = engine.GetPointContentsWorld && (engine.GetPointContentsWorld(engine.traceServer->ThisPtr(), c, MASK_SOLID) & CONTENTS_SOLID) != 0;
	const bool outside = engine.PointOutsideWorld && engine.PointOutsideWorld(engine.traceServer->ThisPtr(), c);
	if (solid) g_bodyVoidFrames += 15;
	// (bridge #38) is the point in a SOLID VOXEL of what was sent to Minecraft, outside every dug cell: the body is embedded in a wall (dig8: a test stood at x -1100,
	// inside the 30 u wall brush -1120..-1090, and could neither mine out of it nor walk; the engine's "inside solid" above says the same for a tunnel and a wall)
	bool embedded = false;
	{
		const Vector m = SrcToMc(c);
		const int bx = (int)std::floor(m.x), by = (int)std::floor(m.y), bz = (int)std::floor(m.z);
		if (!DigIsDugMc(bx, by, bz)) {
			std::vector<ColBlock> v;
			CollisionFieldSentIn(bx, by, bz, bx, by, bz, v);
			const int sx = std::min(7, std::max(0, (int)std::floor((m.x - bx) * 8))), sy = std::min(7, std::max(0, (int)std::floor((m.y - by) * 8))), sz = std::min(7, std::max(0, (int)std::floor((m.z - bz) * 8)));
			for (const ColBlock &b : v) embedded |= (b.bits[sy] >> (sz * 8 + sx) & 1) != 0;
		}
	}
	if (embedded && !g_bodyEmbedded && g_bodyLogged < 60) {
		++g_bodyLogged;
		console->Print("[bridge] dig: WARNING the %s at (%.0f %.0f %.0f) is EMBEDDED in a solid voxel of the host's export, outside every dug cell (inside a wall brush or the void, not in a tunnel): it cannot move or pick from there\n",
		               McModeActive() ? "Minecraft player's body" : "Chell", o.x, o.y, o.z);
	}
	g_bodyEmbedded = embedded;
	if ((solid != g_bodyInVoid || outside != g_bodyOutside) && g_bodyLogged < 40) {
		++g_bodyLogged;
		const bool lim = std::fabs(o.x) > 15500.0f || std::fabs(o.y) > 15500.0f || std::fabs(o.z) > 15500.0f;
		console->Print("[bridge] dig: the %s at (%.0f %.0f %.0f) is %s Portal 2's solid%s%s: the engine's world is no answer there (its trace starts solid); the exported voxels, the dug cells and the revealed blocks decide%s\n",
		               McModeActive() ? "Minecraft player's body" : "Chell", o.x, o.y, o.z, solid ? "inside" : "back out of", outside ? " (outside the world)" : "", lim ? " (near the dig limit)" : "",
		               solid ? " (a tunnel beyond the map)" : "");
	}
	g_bodyInVoid = solid, g_bodyOutside = outside;
}

// Once a frame (from DigHostFill): publish a changed set, tell the export, fizzle portals.
static void DigFrame() {
	if (!BridgeSkip("dig.body")) DigBodyWatch();
	if (g_on) {
		const uint32_t w = Hash(McMapName());
		if (w != g_world || g_worldMap != McMapName()) {  // another map: its own cells
			g_world = w, g_worldMap = McMapName();
			g_sections.clear(), g_local.clear(), g_newCells.clear(), g_dirtyRegions.clear();
			g_dirty = true;
		}
	}
	if (g_dirty) {
		g_dirty = false;
		const auto before = Get();
		Rebuild();
		const auto after = Get();
		CollisionFieldSetDug(std::shared_ptr<const colharvest::DugSet>(after, &after->set));
		for (const auto &r : g_dirtyRegions) CollisionFieldDugRegionDirty(std::get<0>(r), std::get<1>(r), std::get<2>(r));
		g_dirtyRegions.clear();
		FizzlePortalsOn(g_newCells);
		// (bridge #73) the band sections around changed cells are rebuilt: under the cut-away shaders their coplanar faces depend on the set
		{
			std::set<std::array<int, 3>> changed;
			if (before)
				for (const auto &c : before->cells)
					if (!after->set.count(colharvest::BlockKey(c[0], c[1], c[2]))) changed.insert(c);
			for (const auto &c : after->cells)
				if (!before || !before->set.count(colharvest::BlockKey(c[0], c[1], c[2]))) changed.insert(c);
			if (changed.size() < 4096)
				for (const auto &c : changed) TerrainBandCellChanged(c[0], c[1], c[2]);
		}
		if (g_logged < 40 && (!g_newCells.empty() || !before || before->cells.size() != after->cells.size())) {
			++g_logged;
			console->Print("[bridge] dig: %zu dug cells now (%d from Minecraft in %d messages), world %08x\n", after->cells.size(), g_cellsFromMod, g_msgs, g_world);
		}
		g_newCells.clear();
	}
	// the dug cells' Source boxes for the render thread; they move with the region mapping
	const auto s = Get();
	const double sig = g_map.src.x * 1.0 + g_map.src.y * 3.0 + g_map.src.z * 7.0 + g_map.mcX * 11.0 + g_map.mcY * 13.0 + g_map.mcZ * 17.0;
	if (s && (g_boxesGen != s->gen || g_boxesMapSig != sig)) {
		auto v = std::make_shared<std::vector<std::pair<Vector, Vector>>>();
		for (const auto &c : s->cells) {
			Vector mn, mx;
			CellBox(c[0], c[1], c[2], mn, mx);
			v->push_back({mn, mx});
		}
		g_boxesGen = s->gen, g_boxesMapSig = sig;
		std::lock_guard<std::mutex> lk(g_mu);
		g_boxes = v;
	}
}

void DigHostFill(HostPlayer &hp) {
	DigFrame();
	if (!g_on) return;
	hp.flags |= kHpDig;
	hp.digWorld = g_world;
	// What the host surface under Minecraft's crosshair is made of (the mod reads it when a mining starts).
	g_material = kChPanelWhite | kChPortalableBit;
	if (McModeActive()) {
		const QAngle a = engine.ViewAngles();
		const float pr = a.x * (float)M_PI / 180.0f, yw = a.y * (float)M_PI / 180.0f;
		const Vector dir(std::cos(pr) * std::cos(yw), std::cos(pr) * std::sin(yw), -std::sin(pr));
		const Vector eye = McFeetSrc() + Vector(0, 0, 64.8f);
		CGameTrace tr;
		if (engine.Trace(eye, eye + dir * 480.0f, Vector(0, 0, 0), MASK_SOLID, tr, true) && tr.fraction < 1.0f) g_material = MaterialFromName(tr.surface.name);
	}
	hp.digMaterial = g_material;
}

// ---- commands ---------------------------------------------------------------------------------------------------------------
CON_COMMAND(pc_dig, "pc_dig [0|1] - the Minecraft player can dig through chamber walls and floors (needs the harvest export: turned on with it); default 1\n") {
	if (args.ArgC() > 1) {
		g_on = std::atoi(args[1]) != 0;
		if (g_on) {
			CollisionFieldHarvestWant(true);
			g_world = Hash(McMapName()), g_worldMap = McMapName();
		}
		g_dirty = true;
	}
	auto s = Get();
	console->Print("[bridge] dig %s: world %08x (%s), %zu dug cells (%d messages from Minecraft, %d cells), harvest export %s, %d portals fizzled\n", g_on ? "on" : "off", g_world, g_worldMap.c_str(),
	               s ? s->cells.size() : 0, g_msgs, g_cellsFromMod, CollisionFieldHarvestActive() ? "ready" : "NOT ready (the point-query export ignores dug cells)", g_fizzled);
}
CON_COMMAND(pcb_dig_cell, "pcb_dig_cell <x y z> [0|1] - dig (1) or fill (0) the cell containing a Portal 2 point on this side only (Minecraft is not told; tests of the host side)\n") {
	if (args.ArgC() < 4) return;
	if (!g_on) return console->Print("[bridge] dig is off (pc_dig 1)\n");
	const Vector m = SrcToMc(Vector((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3])));
	const std::array<int, 3> c = {(int)std::floor(m.x), (int)std::floor(m.y), (int)std::floor(m.z)};
	const bool dig = args.ArgC() < 5 || std::atoi(args[4]) != 0;
	int pop = 0;
	const int st = CellExport(c[0], c[1], c[2], &pop);  // (before it goes: after, the export no longer has it)
	if (dig) g_local.insert(c), g_newCells.push_back(c);
	else g_local.erase(c);
	MarkRegion(c[0], c[1], c[2]);
	g_dirty = true;
	Vector mn, mx;
	CellBox(c[0], c[1], c[2], mn, mx);
	console->Print("[bridge] dig: cell MC %d %d %d (Source %.0f %.0f %.0f .. %.0f %.0f %.0f) %s; host content %d/512 sub-voxels%s\n", c[0], c[1], c[2], mn.x, mn.y, mn.z, mx.x, mx.y, mx.z,
	               dig ? "dug" : "filled", pop, !dig ? "" : st == 2 ? " -- NOTE: this cell is at the dig limit (the edge of what Portal 2 can hold): Minecraft makes it bedrock and never opens it" : st == 0 ? " -- NOTE: nothing to dig there (air, or not exported yet)" : "");
}
CON_COMMAND(pcb_dig_box, "pcb_dig_box <x0 y0 z0 x1 y1 z1> [0|1] - dig (1) or fill (0) every cell touching a Portal 2 box, on this side only\n") {
	if (args.ArgC() < 7) return;
	if (!g_on) return console->Print("[bridge] dig is off (pc_dig 1)\n");
	const Vector a = SrcToMc(Vector((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3]))), b = SrcToMc(Vector((float)std::atof(args[4]), (float)std::atof(args[5]), (float)std::atof(args[6])));
	const bool dig = args.ArgC() < 8 || std::atoi(args[7]) != 0;
	int n = 0;
	for (int y = (int)std::floor(std::fmin(a.y, b.y)); y <= (int)std::floor(std::fmax(a.y, b.y)); ++y)
		for (int z = (int)std::floor(std::fmin(a.z, b.z)); z <= (int)std::floor(std::fmax(a.z, b.z)); ++z)
			for (int x = (int)std::floor(std::fmin(a.x, b.x)); x <= (int)std::floor(std::fmax(a.x, b.x)) && n < 512; ++x) {
				const std::array<int, 3> c = {x, y, z};
				if (dig) g_local.insert(c), g_newCells.push_back(c);
				else g_local.erase(c);
				MarkRegion(x, y, z);
				++n;
			}
	g_dirty = true;
	console->Print("[bridge] dig: %d cells %s\n", n, dig ? "dug" : "filled");
}
CON_COMMAND(pcb_dig_clear, "pcb_dig_clear - forget every dug cell on this side (Minecraft's own set is not touched: pcb_dig_reset does both)\n") {
	ClearLocal();
	console->Print("[bridge] dig: cleared\n");
}
CON_COMMAND(pcb_dig_reset, "pcb_dig_reset - forget every dug cell here AND have Minecraft fill its set back in (/pcdig clear): a repeatable start for a test\n") {
	ClearLocal();
	engine.Cmd("pcb_mc_cmd pcdig clear");
	console->Print("[bridge] dig: reset (here, and /pcdig clear queued for Minecraft)\n");
}
CON_COMMAND(pcb_dig_under, "pcb_dig_under [cells=2] [0|1] - dig (1) or fill (0) the cells under the current player's whole hull footprint (Chell 32 u, Minecraft 24 u), from the one holding the feet down; this side only\n") {
	if (!g_on) return console->Print("[bridge] dig is off (pc_dig 1)\n");
	const int n = args.ArgC() > 1 ? std::max(1, std::min(8, std::atoi(args[1]))) : 2;
	const bool dig = args.ArgC() < 3 || std::atoi(args[2]) != 0;
	float hw;
	const Vector f = FocusFeet(hw);
	const Vector a = SrcToMc(Vector(f.x - hw, f.y - hw, f.z - 1.0f)), b = SrcToMc(Vector(f.x + hw, f.y + hw, f.z - 1.0f));
	const int x0 = (int)std::floor(std::fmin(a.x, b.x)), x1 = (int)std::floor(std::fmax(a.x, b.x)), z0 = (int)std::floor(std::fmin(a.z, b.z)), z1 = (int)std::floor(std::fmax(a.z, b.z));
	const int top = (int)std::floor(a.y);
	int cells = 0, nodig = 0, empty = 0;
	for (int y = top; y > top - n; --y)
		for (int z = z0; z <= z1; ++z)
			for (int x = x0; x <= x1; ++x) {
				const std::array<int, 3> c = {x, y, z};
				const int st = CellExport(x, y, z);
				nodig += st == 2, empty += st == 0;
				if (dig) g_local.insert(c), g_newCells.push_back(c);
				else g_local.erase(c);
				MarkRegion(x, y, z);
				++cells;
			}
	g_dirty = true;
	console->Print("[bridge] dig: %d cells %s under the %s at (%.0f %.0f %.0f): MC x %d..%d z %d..%d, %d down from y %d (%d of them at the dig limit, %d with no host content)\n", cells, dig ? "dug" : "filled",
	               McModeActive() ? "Minecraft player" : "Chell", f.x, f.y, f.z, x0, x1, z0, z1, n, top, nodig, empty);
}
CON_COMMAND(pcb_dig_status, "pcb_dig_status - why a dug cell does or does not change anything: the set, the harvest, the regions around the player, the cells under the hull\n") {
	auto s = Get();
	float hw;
	const Vector f = FocusFeet(hw);
	const Vector m = SrcToMc(f);
	const int prx = (int)std::floor(m.x / 8.0), pry = (int)std::floor(m.y / 8.0), prz = (int)std::floor(m.z / 8.0);
	console->Print("[bridge] dig status: pc_dig %s, world %08x, %zu dug cells (%d from Minecraft, %zu local), harvest %s, %d regions re-exported for dug cells, %s at (%.1f %.1f %.1f) = MC (%.2f %.2f %.2f)\n", g_on ? "on" : "OFF", g_world,
	               s ? s->cells.size() : 0, g_cellsFromMod, g_local.size(), CollisionFieldHarvestActive() ? "ready" : "NOT ready", CollisionFieldDugResent(), McModeActive() ? "Minecraft player" : "Chell", f.x, f.y, f.z, m.x, m.y, m.z);
	console->Print("[bridge]   the body is %s Portal 2's solid%s (%d frames inside so far)\n", g_bodyInVoid ? "INSIDE" : "not in", g_bodyOutside ? ", outside the world" : "", g_bodyVoidFrames);
	for (int dy = -1; dy <= 0; ++dy) {
		std::string row;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx) row += std::to_string(CollisionFieldRegionState(prx + dx, pry + dy, prz + dz)) + (dx == 1 ? "  " : "");
		console->Print("[bridge]   regions around the feet, layer %+d (x -1..1 per group, z -1..1 groups; 0 not sent, 1 in Minecraft, 2 patch queued): %s\n", dy, row.c_str());
	}
	// the cells the hull stands on, from the feet down
	const Vector a = SrcToMc(Vector(f.x - hw, f.y - hw, f.z - 1.0f)), b = SrcToMc(Vector(f.x + hw, f.y + hw, f.z - 1.0f));
	const int x0 = (int)std::floor(std::fmin(a.x, b.x)), x1 = (int)std::floor(std::fmax(a.x, b.x)), z0 = (int)std::floor(std::fmin(a.z, b.z)), z1 = (int)std::floor(std::fmax(a.z, b.z));
	const int top = (int)std::floor(a.y);
	for (int y = top; y > top - 3; --y) {
		std::string row;
		for (int z = z0; z <= z1; ++z)
			for (int x = x0; x <= x1; ++x) {
				int pop = 0;
				const int st = CellExport(x, y, z, &pop);
				char t[96];
				std::snprintf(t, sizeof t, " [%d %d %d: %s%s %d/512]", x, y, z, DigIsDugMc(x, y, z) ? "DUG " : "", st == 2 ? "LIMIT" : st == 1 ? "solid" : "empty", pop);
				row += t;
			}
		console->Print("[bridge]   cells under the hull, y %d:%s\n", y, row.c_str());
	}
	if (x1 > x0 || z1 > z0) console->Print("[bridge]   the hull spans %d x %d cells: it falls into a hole only once EVERY cell under it is dug (stand at a cell centre, or pcb_dig_under)\n", x1 - x0 + 1, z1 - z0 + 1);
}
CON_COMMAND(pcb_dig_list, "pcb_dig_list - the dug cells (Minecraft block coordinates and their Source boxes)\n") {
	auto s = Get();
	if (!s || s->cells.empty()) return console->Print("[bridge] dig: no dug cells\n");
	int n = 0;
	for (const auto &c : s->cells) {
		Vector mn, mx;
		CellBox(c[0], c[1], c[2], mn, mx);
		if (n++ < 40) console->Print("[bridge]   MC %d %d %d: Source %.0f %.0f %.0f .. %.0f %.0f %.0f\n", c[0], c[1], c[2], mn.x, mn.y, mn.z, mx.x, mx.y, mx.z);
	}
	console->Print("[bridge] dig: %zu dug cells\n", s->cells.size());
}
CON_COMMAND(pcb_assert_dug, "pcb_assert_dug <x y z> [1|0] - the cell containing a Portal 2 point is dug (1, default) / not dug (0)\n") {
	if (args.ArgC() < 4) return;
	const Vector p((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3]));
	const bool want = args.ArgC() < 5 || std::atoi(args[4]) != 0;
	const bool is = DigIsDugSrc(p);
	char buf[200];
	std::snprintf(buf, sizeof buf, "cell at (%.0f %.0f %.0f) is %s (want %s); %zu dug cells", p.x, p.y, p.z, is ? "dug" : "not dug", want ? "dug" : "not dug", Get() ? Get()->cells.size() : 0);
	TestCheck(is == want, "dug cell", buf);
}
CON_COMMAND(pcb_assert_dig_export, "pcb_assert_dig_export <x y z> <empty|solid> - the collision Minecraft was sent for the cell containing a Portal 2 point has no / some solid sub-voxels\n") {
	if (args.ArgC() < 5) return;
	const Vector m = SrcToMc(Vector((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3])));
	const int bx = (int)std::floor(m.x), by = (int)std::floor(m.y), bz = (int)std::floor(m.z);
	std::vector<ColBlock> v;
	CollisionFieldSentIn(bx, by, bz, bx, by, bz, v);
	int pop = 0;
	for (const ColBlock &b : v)
		for (int y = 0; y < 8; ++y) pop += __builtin_popcountll(b.bits[y]);
	const bool wantEmpty = !std::strcmp(args[4], "empty");
	char buf[200];
	std::snprintf(buf, sizeof buf, "cell MC %d %d %d: %d solid sub-voxels were sent (want %s)", bx, by, bz, pop, wantEmpty ? "none" : "some");
	TestCheck(wantEmpty ? pop == 0 : pop > 0, "dug cell's collision export", buf);
}

}  // namespace bridge
