// Portalcraft bridge (bridge #79): Portal 2's light carried into the cuts (DESIGN-dig-render.md section 8).
//
// The user: "the light from Portal doesn't bleed into the Minecraft blocks" when digging into Portal 2's geometry. The band lights a
// block face from Portal 2's model lighting state half a block in front of it (terrain.cpp). Inside a cut that point is inside Portal 2's
// solid (the BSP does not know the cell was dug): the engine has no light there, the face took the section's mean or the last good state,
// and the contact AO traces started in solid (half strength everywhere). So the wall face at the mouth was lit and the first tunnel faces
// were black: a lighting seam at the mouth, and a black tunnel seen from outside (user shots 38, the a2_goo frames of #76).
//
// Here, as Minecraft lights a cave (and SkyCraft its digs), from the dug cells themselves:
//   * the MOUTHS: every dug cell with Minecraft air (no revealed block) that touches Portal 2's open air through one of its faces (3 u out,
//     the face centre and its quarter points, as cutvis.cpp finds the mouths), or whose own centre is open air (a cell the host's face
//     runs through). Each mouth samples Portal 2's lighting state in that open air, 16 u out (the band's own sample distance): the
//     ambient cube plus the local lights the engine picked there, dynamic lights included, our block lights left out (Minecraft's block
//     light carries them), evaluated per axis like the band does (BandLightCubes);
//   * a breadth-first walk over the dug air cells from all mouths at once: one level per block (Minecraft's light rule), the light of a
//     cell = the per-channel max of the mouths at its distance x Curve((15 - d) / 15) (Minecraft's brightness curve: 1 at the mouth,
//     0.78, 0.62, 0.5 ... 0 at 15 blocks);
//   * a band vertex whose sample point is in Portal 2's solid and that faces dug air takes the light of the cells in front of it,
//     interpolated over the four cells around the vertex (Minecraft's smooth lighting), for the face's normal; Minecraft's own sky light
//     (the same curve) and block light (the band's warm torch term) are the floor, Portal 2's contact AO is left out (its traces start in
//     solid there; Minecraft's own vertex AO stays in the vertex colour).
// (bridge #80) The harness (dh.py --scen seam,albedo on sp_a1_intro2) showed the mouth's light was right but the faces inside the hole
// took the cube's component for their OWN normal (a floor 0.0039 where the room-facing wall had 0.0072): the cap was drawn with 0.44 of
// the wall's light at the edge. A mouth now also keeps its INFLOW, the light arriving through its open face from outside (the component
// a surface facing out of that face gets: what the host's wall beside it is lit with), carried by the walk like a Minecraft light level;
// a face takes the brighter of its normal's component and the inflow. `pc_cut_light 2` is #79's rule.
// The field is rebuilt when the dug set, the Minecraft blocks in dug cells or the mapping change; the band sections whose cells' light
// changed are rebuilt (TerrainBandCellChanged). `pc_cut_light 0` restores the old lighting (A/B).
#include "bridge.hpp"
#include "coords.hpp"
#include "dig.hpp"
#include "growth.hpp"
#include "prof.hpp"

#include "Modules/Tier1.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace bridge {

bool TerrainIsSolid(int bx, int by, int bz);          // terrain.cpp
void TerrainBandCellChanged(int x, int y, int z);      // terrain.cpp
void TerrainBandRebuild();                             // terrain.cpp
uint32_t TerrainSolidsGen();                           // terrain.cpp
int DigRenderMode();                                   // cutrender.cpp
bool BandLightCubes(const std::vector<Vector> &pts, std::vector<std::array<Vector, 6>> &out, std::vector<char> &ok);  // bandlight.cpp

namespace {

using Cube = std::array<Vector, 6>;  // linear light for the Source axes +X -X +Y -Y +Z -Z

struct CellL {
	int d = 0;     // blocks from the nearest mouth cell (0 = a mouth)
	Cube cube{};   // the max of the mouth cubes reaching it at that distance (not yet attenuated)
	Vector in{};   // (bridge #80) the max of the mouths' inflow reaching it: the light that came in through a mouth's open face
};

int g_on = 1;  // pc_cut_light: 1 the inflow rule (bridge #80), 2 #79's directional rule only, 0 off
std::unordered_map<long long, CellL> g_field;
uint32_t g_dugGen = 0xFFFFFFFFu, g_solidsGen = 0xFFFFFFFFu;
double g_mapSig = 0;
uint64_t g_airSig = 0;
bool g_pending = false;   // some mouth's state came back empty: retry
int g_pendingTries = 0;
int g_retryWait = 0;
long g_builds = 0, g_mouths = 0, g_cellsChanged = 0, g_vertsLit = 0, g_queries = 0;
double g_buildMs = 0, g_buildMax = 0;
std::unordered_map<long long, Cube> g_mouthCache;     // mouth point (0.5 u grid) -> its light cube (static light: kept per level)
std::unordered_map<long long, char> g_openCache;      // point (0.5 u grid) -> open air (the world does not change)
PCB_GAUGE("cutlight.field", g_field.size());
PCB_GAUGE("cutlight.mouthCache", g_mouthCache.size());
PCB_GAUGE("cutlight.openCache", g_openCache.size());

constexpr int kMaxD = 15;

long long CellKey(int x, int y, int z) { return ((long long)(x + 1048576) << 42) | ((long long)(y + 1048576) << 21) | (long long)(z + 1048576); }
long long PtKey(const Vector &p) {
	return ((long long)std::lround(p.x * 2) & 0x1FFFFF) << 42 | ((long long)std::lround(p.y * 2) & 0x1FFFFF) << 21 | ((long long)std::lround(p.z * 2) & 0x1FFFFF);
}

bool OpenAir(const Vector &p) {
	const long long k = PtKey(p);
	auto it = g_openCache.find(k);
	if (it != g_openCache.end()) return it->second != 0;
	void *tr = engine.traceServer->ThisPtr();
	const bool open = !engine.PointOutsideWorld(tr, p) && (engine.GetPointContentsWorld(tr, p, 0x1 | 0x8) & (0x1 | 0x8)) == 0;
	if (g_openCache.size() > 400000) g_openCache.clear();
	g_openCache.emplace(k, open ? 1 : 0);
	return open;
}

float Curve(float l) { return l / (4.0f - 3.0f * l); }
float Falloff(int d) { return d >= kMaxD ? 0.0f : Curve((float)(kMaxD - d) / kMaxD); }

static const int kN6[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};  // Minecraft Direction ordinals
// Minecraft direction -> the Source cube axis of that normal (Source x = MC x, Source y = -MC z, Source z = MC y)
static const int kDirAxis[6] = {5, 4, 2, 3, 1, 0};

double MapSig() { return g_map.src.x * 1.0 + g_map.src.y * 3.0 + g_map.src.z * 7.0 + g_map.mcX * 11.0 + g_map.mcY * 13.0 + g_map.mcZ * 17.0; }

bool SameCell(const CellL &a, const CellL &b) {
	if (a.d != b.d) return false;
	if (std::fabs(a.in.x - b.in.x) > 1e-3f || std::fabs(a.in.y - b.in.y) > 1e-3f || std::fabs(a.in.z - b.in.z) > 1e-3f) return false;
	for (int i = 0; i < 6; ++i)
		if (std::fabs(a.cube[i].x - b.cube[i].x) > 1e-3f || std::fabs(a.cube[i].y - b.cube[i].y) > 1e-3f || std::fabs(a.cube[i].z - b.cube[i].z) > 1e-3f) return false;
	return true;
}

void Build() {
	const auto t0 = std::chrono::steady_clock::now();
	++g_builds;
	std::vector<std::pair<Vector, Vector>> boxes;
	DigAllBoxes(boxes);
	// the dug cells that are Minecraft air (a revealed block stops the light, as in Minecraft)
	std::vector<std::array<int, 3>> air;
	std::unordered_map<long long, int> idx;
	uint64_t sig = 1469598103934665603ull;
	for (const auto &b : boxes) {
		const Vector m = SrcToMc((b.first + b.second) * 0.5f);
		const int c[3] = {(int)std::floor(m.x), (int)std::floor(m.y), (int)std::floor(m.z)};
		if (TerrainIsSolid(c[0], c[1], c[2])) continue;
		idx[CellKey(c[0], c[1], c[2])] = (int)air.size();
		air.push_back({c[0], c[1], c[2]});
		sig = (sig ^ (uint64_t)CellKey(c[0], c[1], c[2])) * 1099511628211ull;
	}
	g_airSig = sig;
	// the mouths: open-air sample points per cell
	std::vector<std::vector<Vector>> pts(air.size());
	std::vector<std::vector<int>> ptDir(air.size());  // (bridge #80) the face the sample is out of (Minecraft direction), -1: the cell's own centre
	std::vector<Vector> want;
	std::unordered_map<long long, int> wantIdx;
	for (size_t i = 0; i < air.size(); ++i) {
		const auto &c = air[i];
		const Vector ctr = McToSrc(c[0] + 0.5, c[1] + 0.5, c[2] + 0.5);
		if (OpenAir(ctr)) pts[i].push_back(ctr), ptDir[i].push_back(-1);
		for (int d = 0; d < 6 && pts[i].size() < 3; ++d) {
			const int n[3] = {c[0] + kN6[d][0], c[1] + kN6[d][1], c[2] + kN6[d][2]};
			if (idx.count(CellKey(n[0], n[1], n[2])) || DigIsDugMc(n[0], n[1], n[2])) continue;  // another dug cell (air or a revealed block)
			for (int q = 0; q < 5; ++q) {
				double p[3] = {c[0] + 0.5, c[1] + 0.5, c[2] + 0.5};
				for (int a = 0; a < 3; ++a) p[a] += kN6[d][a] * (0.5 + 3.0 / kUnitsPerBlock);
				if (q) {
					int u = -1, v = -1;
					for (int a = 0; a < 3; ++a)
						if (!kN6[d][a]) (u < 0 ? u : v) = a;
					p[u] += (q & 1) ? 0.3 : -0.3, p[v] += (q & 2) ? 0.3 : -0.3;
				}
				const Vector s = McToSrc(p[0], p[1], p[2]);
				if (!OpenAir(s)) continue;
				// 16 u out along the face's normal (the band's sample distance) if that is still open air
				double p2[3] = {p[0], p[1], p[2]};
				for (int a = 0; a < 3; ++a) p2[a] += kN6[d][a] * (13.0 / kUnitsPerBlock);
				const Vector s2 = McToSrc(p2[0], p2[1], p2[2]);
				pts[i].push_back(OpenAir(s2) ? s2 : s), ptDir[i].push_back(d);
				break;
			}
		}
		for (const Vector &p : pts[i]) {
			const long long k = PtKey(p);
			if (!g_mouthCache.count(k) && !wantIdx.count(k)) wantIdx[k] = (int)want.size(), want.push_back(p);
		}
	}
	bool pending = false;
	if (!want.empty()) {
		std::vector<Cube> got;
		std::vector<char> ok;
		BandLightCubes(want, got, ok);
		g_queries += (long)want.size();
		for (size_t k = 0; k < want.size(); ++k) {
			if (ok.size() > k && ok[k]) {
				if (g_mouthCache.size() > 200000) g_mouthCache.clear();
				g_mouthCache[PtKey(want[k])] = got[k];
			} else
				pending = true;
		}
	}
	// the walk: all mouths at distance 0, one level per block
	std::unordered_map<long long, CellL> field;
	std::vector<int> frontier, next;
	std::vector<int> dist(air.size(), -1);
	std::vector<Cube> cube(air.size());
	std::vector<Vector> inflow(air.size());
	long mouths = 0;
	static int logsMouth = 0;
	for (size_t i = 0; i < air.size(); ++i) {
		Cube sum{};
		Vector in(0, 0, 0);
		int n = 0;
		for (size_t q = 0; q < pts[i].size(); ++q) {
			auto it = g_mouthCache.find(PtKey(pts[i][q]));
			if (it == g_mouthCache.end()) continue;
			for (int a = 0; a < 6; ++a) sum[a] += it->second[a];
			++n;
			// (bridge #80) the inflow: through an open face d, the light arriving from outside along d, which is the ambient cube's
			// component for a surface facing d (the host's own wall facing the same way is lit by it); from an open centre, the cube's mean
			Vector v(0, 0, 0);
			if (ptDir[i][q] >= 0) v = it->second[kDirAxis[ptDir[i][q]]];
			else
				for (int a = 0; a < 6; ++a) v += it->second[a] * (1.0f / 6.0f);
			in = Vector(std::fmax(in.x, v.x), std::fmax(in.y, v.y), std::fmax(in.z, v.z));
		}
		if (!n) continue;
		for (int a = 0; a < 6; ++a) sum[a] = sum[a] * (1.0f / n);
		if (logsMouth < 12)
			++logsMouth, console->Print("[bridge] cut light: mouth cell %d %d %d (%d samples, first at %.0f %.0f %.0f): +X %.4f -X %.4f +Y %.4f -Y %.4f +Z %.4f -Z %.4f, inflow %.4f (green)\n", air[i][0], air[i][1], air[i][2], n, pts[i][0].x, pts[i][0].y, pts[i][0].z,
			               sum[0].y, sum[1].y, sum[2].y, sum[3].y, sum[4].y, sum[5].y, in.y);
		dist[i] = 0, cube[i] = sum, inflow[i] = in, frontier.push_back((int)i), ++mouths;
	}
	for (int d = 1; d < kMaxD && !frontier.empty(); ++d) {
		next.clear();
		for (int i : frontier)
			for (const auto &o : kN6) {
				auto it = idx.find(CellKey(air[i][0] + o[0], air[i][1] + o[1], air[i][2] + o[2]));
				if (it == idx.end()) continue;
				const int j = it->second;
				if (dist[j] >= 0 && dist[j] < d) continue;
				if (dist[j] < 0) dist[j] = d, cube[j] = cube[i], inflow[j] = inflow[i], next.push_back(j);
				else {
					for (int a = 0; a < 6; ++a)
						cube[j][a] = Vector(std::fmax(cube[j][a].x, cube[i][a].x), std::fmax(cube[j][a].y, cube[i][a].y), std::fmax(cube[j][a].z, cube[i][a].z));
					inflow[j] = Vector(std::fmax(inflow[j].x, inflow[i].x), std::fmax(inflow[j].y, inflow[i].y), std::fmax(inflow[j].z, inflow[i].z));
				}
			}
		frontier.swap(next);
	}
	for (size_t i = 0; i < air.size(); ++i) {
		// no mouth within 15 blocks (or none known yet: a cut still being mined, a state the engine has not answered): not in the field,
		// so its faces keep the band's own light instead of going black
		if (dist[i] < 0) continue;
		CellL c;
		c.d = dist[i];
		c.cube = cube[i];
		c.in = inflow[i];
		field[CellKey(air[i][0], air[i][1], air[i][2])] = c;
	}
	// rebuild the band sections whose cells' light changed (also cells that left the field: a block placed back)
	long changed = 0;
	auto unkey = [](long long k, int &x, int &y, int &z) {
		x = (int)((k >> 42) & 0x1FFFFF) - 1048576, y = (int)((k >> 21) & 0x1FFFFF) - 1048576, z = (int)(k & 0x1FFFFF) - 1048576;
	};
	for (const auto &kv : field) {
		auto it = g_field.find(kv.first);
		if (it != g_field.end() && SameCell(it->second, kv.second)) continue;
		int x, y, z;
		unkey(kv.first, x, y, z);
		TerrainBandCellChanged(x, y, z), ++changed;
	}
	for (const auto &kv : g_field)
		if (!field.count(kv.first)) {
			int x, y, z;
			unkey(kv.first, x, y, z);
			TerrainBandCellChanged(x, y, z), ++changed;
		}
	g_field.swap(field);
	g_mouths = mouths, g_cellsChanged += changed;
	g_pending = pending;
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	g_buildMs += ms, g_buildMax = std::max(g_buildMax, ms);
	static int logs = 0;
	if (logs < 30 || g_builds % 50 == 0)
		++logs, console->Print("[bridge] cut light: %zu dug air cells, %ld mouth cells, %zu new mouth samples%s, %ld cells relit, %.2f ms\n", air.size(), mouths, want.size(),
		                       pending ? " (some empty: retried)" : "", changed, ms);
}

}  // namespace

bool CutLightOn() { return g_on && DigRenderMode() == 2; }

// terrain.cpp, every client frame before band sections are rebuilt: keep the field in step with the dug cells.
void CutLightFrame() {
	PROF("cutlight");
	if (!CutLightOn() || !DigEnabled()) return;
	if (!DigAny()) {
		if (!g_field.empty()) {
			for (const auto &kv : g_field) {
				const long long k = kv.first;
				TerrainBandCellChanged((int)((k >> 42) & 0x1FFFFF) - 1048576, (int)((k >> 21) & 0x1FFFFF) - 1048576, (int)(k & 0x1FFFFF) - 1048576);
			}
			g_field.clear();
		}
		return;
	}
	const uint32_t gen = DigGen(), sg = TerrainSolidsGen();
	const double ms = MapSig();
	bool need = gen != g_dugGen || ms != g_mapSig;
	if (!need && sg != g_solidsGen) {
		// the Minecraft blocks changed somewhere: only a change in which dug cells are air matters
		std::vector<std::pair<Vector, Vector>> boxes;
		DigAllBoxes(boxes);
		uint64_t sig = 1469598103934665603ull;
		for (const auto &b : boxes) {
			const Vector m = SrcToMc((b.first + b.second) * 0.5f);
			const int c[3] = {(int)std::floor(m.x), (int)std::floor(m.y), (int)std::floor(m.z)};
			if (!TerrainIsSolid(c[0], c[1], c[2])) sig = (sig ^ (uint64_t)CellKey(c[0], c[1], c[2])) * 1099511628211ull;
		}
		need = sig != g_airSig;
	}
	g_solidsGen = sg;
	if (!need && g_pending && g_pendingTries < 20 && --g_retryWait <= 0) need = true, ++g_pendingTries, g_retryWait = 10;
	if (!need) return;
	if (gen != g_dugGen || ms != g_mapSig) g_pendingTries = 0;
	g_dugGen = gen, g_mapSig = ms;
	Build();
}

// A new level: the mouths' light was the old map's.
void CutLightReset() {
	g_mouthCache.clear();
	g_openCache.clear();
	g_field.clear();
	g_dugGen = 0xFFFFFFFFu;
}

// The light a band vertex takes from the cut (terrain.cpp): the vertex (Minecraft block coordinates), the face's Minecraft direction
// (0..5), the vertex's Minecraft sky level (0..15). Linear light for the face's normal, interpolated over the four cells in front of the
// vertex that are dug air (Minecraft's smooth light), floored by Minecraft's sky light on the same mouth light. False: no dug air cell in
// front of the vertex (the caller keeps its own light). *mean: the 6-axis mean of the cube (the band's bounce floor).
bool CutLightVertex(double x, double y, double z, int dir, int sky, Vector &out, float *meanOut) {
	if (!CutLightOn() || g_field.empty() || dir < 0 || dir > 5) return false;
	const int ax = kN6[dir][0] ? 0 : kN6[dir][1] ? 1 : 2, sg = kN6[dir][ax];
	const double v[3] = {x, y, z};
	const int a1 = (ax + 1) % 3, a2 = (ax + 2) % 3;
	const int front = (int)std::floor(v[ax] + sg * 0.5);
	const double u1 = v[a1] - 0.5, u2 = v[a2] - 0.5;
	const int i1 = (int)std::floor(u1), i2 = (int)std::floor(u2);
	const double f1 = u1 - i1, f2 = u2 - i2;
	Vector L(0, 0, 0), C(0, 0, 0);
	double wsum = 0, mean = 0;
	const int axis = kDirAxis[dir];
	for (int a = 0; a < 2; ++a)
		for (int b = 0; b < 2; ++b) {
			const double w = (a ? f1 : 1 - f1) * (b ? f2 : 1 - f2);
			if (w <= 1e-6) continue;
			int c[3];
			c[ax] = front, c[a1] = i1 + a, c[a2] = i2 + b;
			auto it = g_field.find(CellKey(c[0], c[1], c[2]));
			if (it == g_field.end()) continue;
			const Cube &q = it->second.cube;
			// (bridge #80) Minecraft's light level has no direction: what came in through the mouth reaches every face of the cave. A face
			// takes its own normal's component or the inflow, whichever is brighter (#79's rule, the normal's component alone: pc_cut_light 2)
			const Vector &in = it->second.in;
			const Vector qa = g_on == 2 ? q[axis] : Vector(std::fmax(q[axis].x, in.x), std::fmax(q[axis].y, in.y), std::fmax(q[axis].z, in.z));
			L += qa * (float)(w * Falloff(it->second.d));
			C += qa * (float)w;
			float m = 0;
			for (int k = 0; k < 6; ++k) m += (q[k].x + q[k].y + q[k].z) / 18.0f;
			mean += w * m;
			wsum += w;
		}
	if (wsum <= 1e-6) return false;
	L = L * (float)(1.0 / wsum), C = C * (float)(1.0 / wsum);
	const float sk = Curve(std::fmin(15, std::fmax(0, sky)) / 15.0f);
	out = Vector(std::fmax(L.x, C.x * sk), std::fmax(L.y, C.y * sk), std::fmax(L.z, C.z * sk));
	if (meanOut) *meanOut = (float)(mean / wsum);
	++g_vertsLit;
	return true;
}

// (digharness) the light level a cell has from the cut: d (blocks from a mouth), -1 when it is not dug air
int CutLightCellDist(int x, int y, int z) {
	auto it = g_field.find(CellKey(x, y, z));
	return it == g_field.end() ? -1 : it->second.d;
}

CON_COMMAND(pc_cut_light, "pc_cut_light [0|1|2] - (bridge #79) light the cuts' faces from Portal 2's light at their mouths, carried in one level per block: 1 (default) every face takes the brighter of its normal's light and the light that came in through the mouth (bridge #80), 2 its normal's light only (#79), 0 the old per-vertex samples inside the host's solid; rebuilds the band\n") {
	if (args.ArgC() > 1) {
		g_on = std::max(0, std::min(2, std::atoi(args[1])));
		g_dugGen = 0xFFFFFFFFu;
		if (!g_on) g_field.clear();
		TerrainBandRebuild();
	}
	int hist[kMaxD + 1] = {};
	for (const auto &kv : g_field) ++hist[std::min(kMaxD, kv.second.d)];
	std::string h;
	for (int d = 0; d <= kMaxD; ++d)
		if (hist[d]) h += " d" + std::to_string(d) + ":" + std::to_string(hist[d]);
	console->Print("[bridge] cut light %s: %zu dug air cells (%s), %ld mouth cells, %ld builds (%.1f ms, worst %.2f), %ld mouth queries, %ld cells relit, %ld vertices lit; mouth cache %zu%s\n", g_on == 1 ? "ON (inflow)" : g_on == 2 ? "ON (directional)" : "off",
	               g_field.size(), h.empty() ? "none" : h.c_str() + 1, g_mouths, g_builds, g_buildMs, g_buildMax, g_queries, g_cellsChanged, g_vertsLit, g_mouthCache.size(), g_pending ? " (pending empty states)" : "");
}

}  // namespace bridge
