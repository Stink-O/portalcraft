// Portalcraft bridge (bridge #73): what can be seen through dug cells, for every view and for the server (DESIGN-dig-render.md 2).
//
// Portal 2 decides what to draw (the client's world visibility) and what to send (the server's PVS: entities, the water entity,
// areas) from the potentially visible set of the viewer's cluster. A dug hole is a new opening the level's precomputed visibility
// knows nothing about, and a viewer standing inside one is inside a solid leaf (cluster -1: nothing visible, no entity sent).
//
// The rule, deterministic and the same for every view: a connected group of dug cells (a "cut") is a new portal between the open
// leaves it touches. Its MOUTHS are open-air points just outside its faces, one per cluster. For a visibility origin:
//   * inside a cut (its cell is dug), or in solid next to one: the origin is replaced by that cut's mouths;
//   * anywhere else: the mouths of every cut that the origin's cluster can see (any mouth's cluster in its PVS) are added.
// The client's ViewSetupVisEx (visclamp.cpp, every view: main, portal, reflection) and the server's ClientSetupVisibility (hooked
// here: the engine's own AddOriginToPVS for each added origin) use the same function. No camera history, distance or count of
// "nearest" cells is involved; the cuts and mouths are rebuilt when the dug set changes.
#include "bridge.hpp"
#include "slots.hpp"
#include "prof.hpp"
#include "coords.hpp"
#include "dig.hpp"

#include "Modules/Tier1.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

namespace bridge {

int DigRenderMode();  // cutrender.cpp
Vector VisMainCamera();  // visclamp.cpp: this frame's main camera

namespace {

Interface *g_engServer = nullptr;   // VEngineServer022
Interface *g_gameClients = nullptr; // ServerGameClients004
// IVEngineServer and IServerGameClients slots: slots.hpp (ClientSetupVisibility resets the PVS, adds the view entity's eye, then the player's own)
constexpr int kMaxMouthsPerCut = 12, kMaxOrigins = 24;

using slots::VCall;
int Cluster(const Vector &p) { return g_engServer ? VCall<int>(g_engServer->ThisPtr(), slots::kEngClusterForOrigin, (const Vector *)&p) : -1; }

struct Cut {
	std::vector<std::array<int, 3>> cells;
	std::vector<Vector> mouths;
	std::vector<int> clusters;
};
std::vector<Cut> g_cuts;
std::unordered_map<long long, int> g_cellCut;  // cell key -> cut index
uint32_t g_gen = 0xFFFFFFFFu;
double g_sig = 0;
long g_rebuilds = 0, g_viewsTouched = 0, g_serverAdds = 0, g_replaced = 0;
int g_lastOrigins = 0;

long long Key(int x, int y, int z) { return ((long long)(x + 1048576) << 42) | ((long long)(y + 1048576) << 21) | (long long)(z + 1048576); }

bool OpenAir(const Vector &p) {
	void *tr = engine.traceServer->ThisPtr();
	if (engine.PointOutsideWorld(tr, p)) return false;
	return (engine.GetPointContentsWorld(tr, p, 0x1) & 0x1) == 0;
}

void Rebuild() {
	++g_rebuilds;
	g_cuts.clear();
	g_cellCut.clear();
	std::vector<std::pair<Vector, Vector>> boxes;
	DigAllBoxes(boxes);
	std::vector<std::array<int, 3>> cells;
	std::unordered_map<long long, int> idx;
	for (const auto &b : boxes) {
		const Vector m = SrcToMc((b.first + b.second) * 0.5f);
		const std::array<int, 3> c = {(int)std::floor(m.x), (int)std::floor(m.y), (int)std::floor(m.z)};
		idx[Key(c[0], c[1], c[2])] = (int)cells.size();
		cells.push_back(c);
	}
	// connected groups (face neighbours)
	std::vector<int> grp(cells.size(), -1);
	static const int N6[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
	for (size_t i = 0; i < cells.size(); ++i) {
		if (grp[i] >= 0) continue;
		const int g = (int)g_cuts.size();
		g_cuts.emplace_back();
		std::vector<int> st = {(int)i};
		grp[i] = g;
		while (!st.empty()) {
			const int k = st.back();
			st.pop_back();
			g_cuts[g].cells.push_back(cells[k]);
			g_cellCut[Key(cells[k][0], cells[k][1], cells[k][2])] = g;
			for (auto &d : N6) {
				auto it = idx.find(Key(cells[k][0] + d[0], cells[k][1] + d[1], cells[k][2] + d[2]));
				if (it != idx.end() && grp[it->second] < 0) grp[it->second] = g, st.push_back(it->second);
			}
		}
	}
	// mouths: open air 3 u outside each face that does not touch another dug cell, one point per cluster
	for (Cut &c : g_cuts) {
		for (const auto &cell : c.cells) {
			if ((int)c.mouths.size() >= kMaxMouthsPerCut) break;
			for (auto &d : N6) {
				if (idx.count(Key(cell[0] + d[0], cell[1] + d[1], cell[2] + d[2]))) continue;
				// the face centre and its four quarter points, in Minecraft block coordinates, 3/40 of a block outside the face
				for (int q = 0; q < 5; ++q) {
					double p[3] = {cell[0] + 0.5, cell[1] + 0.5, cell[2] + 0.5};
					for (int a = 0; a < 3; ++a) p[a] += d[a] * (0.5 + 3.0 / kUnitsPerBlock);
					if (q) {
						int u = -1, v = -1;
						for (int a = 0; a < 3; ++a)
							if (!d[a]) (u < 0 ? u : v) = a;
						p[u] += (q & 1) ? 0.25 : -0.25, p[v] += (q & 2) ? 0.25 : -0.25;
					}
					const Vector s = McToSrc(p[0], p[1], p[2]);
					if (!OpenAir(s)) continue;
					const int cl = Cluster(s);
					if (cl < 0 || std::find(c.clusters.begin(), c.clusters.end(), cl) != c.clusters.end()) continue;
					c.clusters.push_back(cl);
					c.mouths.push_back(s);
					break;
				}
				if ((int)c.mouths.size() >= kMaxMouthsPerCut) break;
			}
		}
	}
	static int logged = 0;
	if (logged < 20) {
		++logged;
		size_t m = 0;
		for (const Cut &c : g_cuts) m += c.mouths.size();
		console->Print("[bridge] cut vis: %zu dug cells in %zu cuts with %zu mouths (open air on the far side of a hole, one per cluster)\n", cells.size(), g_cuts.size(), m);
	}
}

void Ensure() {
	if (!g_engServer) g_engServer = Interface::Create(MODULE_NAME("engine"), "VEngineServer022", false);
	const uint32_t gen = DigGen();
	const double sig = g_map.src.x * 1.0 + g_map.src.y * 3.0 + g_map.src.z * 7.0 + g_map.mcX * 11.0 + g_map.mcY * 13.0 + g_map.mcZ * 17.0;
	if (gen == g_gen && sig == g_sig) return;
	g_gen = gen, g_sig = sig;
	Rebuild();
}

// The cut a point belongs to: its own cell dug, or (in solid) a dug cell within one cell of it.
int CutOf(const Vector &p, bool inSolid) {
	const Vector m = SrcToMc(p);
	const int x = (int)std::floor(m.x), y = (int)std::floor(m.y), z = (int)std::floor(m.z);
	auto it = g_cellCut.find(Key(x, y, z));
	if (it != g_cellCut.end()) return it->second;
	if (!inSolid) return -1;
	for (int dy = -1; dy <= 1; ++dy)
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx) {
				auto jt = g_cellCut.find(Key(x + dx, y + dy, z + dz));
				if (jt != g_cellCut.end()) return jt->second;
			}
	return -1;
}

}  // namespace

// The origins one visibility origin stands for (DESIGN 2). Returns how many were written to out (at least 1 unless out is full);
// *replaced: the origin itself was replaced (it was inside a cut).
int CutVisExpand(const Vector &o, Vector *out, int max, bool *replaced) {
	PROF("cutvis");
	if (replaced) *replaced = false;
	if (max <= 0) return 0;
	if (DigRenderMode() != 2 || !DigEnabled() || !DigAny()) return out[0] = o, 1;
	Ensure();
	if (g_cuts.empty()) return out[0] = o, 1;
	void *tr = engine.traceServer->ThisPtr();
	const bool solid = (engine.GetPointContentsWorld(tr, o, 0x1) & 0x1) != 0 || engine.PointOutsideWorld(tr, o);
	int n = 0;
	std::vector<int> seen;
	auto push = [&](const Vector &p, int cl) {
		if (n >= max) return;
		if (cl >= 0 && std::find(seen.begin(), seen.end(), cl) != seen.end()) return;
		if (cl >= 0) seen.push_back(cl);
		out[n++] = p;
	};
	const int own = CutOf(o, solid);
	int oc = -1;
	if (solid && own >= 0) {
		if (replaced) *replaced = true;
		++g_replaced;
		for (size_t i = 0; i < g_cuts[own].mouths.size(); ++i) push(g_cuts[own].mouths[i], g_cuts[own].clusters[i]);
		if (n == 0) push(o, -1);  // a cut with no mouth (closed all round): nothing better to give
		oc = n ? Cluster(out[0]) : -1;
	} else {
		oc = Cluster(o);
		push(o, oc);
		if (own >= 0)
			for (size_t i = 0; i < g_cuts[own].mouths.size(); ++i) push(g_cuts[own].mouths[i], g_cuts[own].clusters[i]);
	}
	// every other cut whose mouths this origin's cluster can see
	if (oc >= 0 && g_engServer) {
		static uint8_t pvs[16384];
		const int bytes = VCall<int>(g_engServer->ThisPtr(), slots::kEngPVSForCluster, oc, (int)sizeof pvs, (unsigned char *)pvs);
		if (bytes > 0)
			for (size_t k = 0; k < g_cuts.size() && n < max; ++k) {
				if ((int)k == own) continue;
				bool vis = false;
				for (int cl : g_cuts[k].clusters) vis = vis || (cl >> 3) < bytes && (pvs[cl >> 3] >> (cl & 7) & 1);
				if (!vis) continue;
				for (size_t i = 0; i < g_cuts[k].mouths.size(); ++i) push(g_cuts[k].mouths[i], g_cuts[k].clusters[i]);
			}
	}
	++g_viewsTouched;
	g_lastOrigins = n;
	return n;
}

// (bridge #75) The origin the engine should take a view's FOG VOLUME / WATER from (IVRenderView slot 10, visclamp.cpp): an eye inside
// a cut is in a solid leaf, which has no water data, so the client decides there is no water to draw (sp_a2_laser_over_goo, inside
// a cut looking back out over the goo: 0 of 28 expected water samples drawn). The cut's mouth nearest the eye stands in for it, as
// for the visibility. Returns false (keep the eye) unless the eye is in solid inside a cut.
long g_fogSwaps = 0;
bool CutVisFogOrigin(const Vector &eye, Vector &out) {
	if (DigRenderMode() != 2 || !DigEnabled() || !DigAny()) return false;
	Ensure();
	if (g_cuts.empty()) return false;
	void *tr = engine.traceServer->ThisPtr();
	const bool solid = (engine.GetPointContentsWorld(tr, eye, 0x1) & 0x1) != 0 || engine.PointOutsideWorld(tr, eye);
	if (!solid) return false;
	const int own = CutOf(eye, true);
	if (own < 0 || g_cuts[own].mouths.empty()) return false;
	float best = 1e30f;
	for (const Vector &m : g_cuts[own].mouths) {
		const float d = (m - eye).Length();
		if (d < best) best = d, out = m;
	}
	++g_fogSwaps;
	return true;
}

namespace {
// ---- the server's PVS ----------------------------------------------------------------------------------------------------------
using _ClientSetupVisibility = void(__rescall *)(void *thisptr, void *viewEnt, void *client, unsigned char *pvs, int pvssize);
_ClientSetupVisibility g_origCSV = nullptr;
void __rescall ClientSetupVisibility_Hook(void *thisptr, void *viewEnt, void *client, unsigned char *pvs, int pvssize) {
	g_origCSV(thisptr, viewEnt, client, pvs, pvssize);
	PROF("cutvis.server");
	if (DigRenderMode() != 2 || !DigEnabled() || !DigAny() || !g_engServer) return;
	// singleplayer: the client's camera is what this player sees (Minecraft's camera in Minecraft mode, F5 included)
	const Vector cam = VisMainCamera();
	Vector o[kMaxOrigins];
	bool rep = false;
	const int n = CutVisExpand(cam, o, kMaxOrigins, &rep);
	for (int i = 0; i < n; ++i) {
		if (!rep && i == 0) continue;  // the camera itself: the game already added the player's eye
		VCall<void>(g_engServer->ThisPtr(), slots::kEngAddOriginToPVS, (const Vector *)&o[i]);
		++g_serverAdds;
	}
}
}  // namespace

bool CutVisInit() {
	if (!g_engServer) g_engServer = Interface::Create(MODULE_NAME("engine"), "VEngineServer022", false);
	g_gameClients = Interface::Create(MODULE_NAME("server"), "ServerGameClients004");
	if (!g_gameClients || !slots::Hook(g_gameClients, slots::kGcClientSetupVisibility, ClientSetupVisibility_Hook, g_origCSV)) {
		console->Warn("[bridge] cut vis: the server's ClientSetupVisibility was not hooked (entities seen through holes may not be sent)\n");
		if (g_gameClients) Interface::Delete(g_gameClients);
		g_gameClients = nullptr;
		return false;
	}
	return true;
}
void CutVisShutdown() {
	if (g_gameClients) Interface::Delete(g_gameClients);
	g_gameClients = nullptr;
}

CON_COMMAND(pc_cut_vis, "pc_cut_vis - (bridge #73) the cuts (connected dug cells), their mouths, and what the visibility hooks did\n") {
	Ensure();
	console->Print("[bridge] cut vis (pc_dig_render %d): %zu cuts, %ld rebuilds, %ld origins expanded (%ld replaced: the viewer inside a cut), %ld server PVS additions, %ld fog-volume origins from a mouth, last expansion %d origins; server hook %s\n", DigRenderMode(),
	               g_cuts.size(), g_rebuilds, g_viewsTouched, g_replaced, g_serverAdds, g_fogSwaps, g_lastOrigins, g_gameClients ? "installed" : "NOT installed");
	for (size_t k = 0; k < g_cuts.size() && k < 12; ++k) {
		std::string m;
		for (size_t i = 0; i < g_cuts[k].mouths.size(); ++i) {
			char t[80];
			std::snprintf(t, sizeof t, " (%.0f %.0f %.0f c%d)", g_cuts[k].mouths[i].x, g_cuts[k].mouths[i].y, g_cuts[k].mouths[i].z, g_cuts[k].clusters[i]);
			m += t;
		}
		console->Print("[bridge]   cut %zu: %zu cells, mouths%s\n", k, g_cuts[k].cells.size(), m.empty() ? " none" : m.c_str());
	}
}

}  // namespace bridge
