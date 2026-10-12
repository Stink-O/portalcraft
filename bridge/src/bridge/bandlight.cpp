// Portalcraft bridge (bridge #18, Owner A): Portal 2's light on the Minecraft blocks placed in its chambers, SkyCraft's
// WorldRender.cpp Lighting()/VoxelAO recipe with Portal 2 as the host, evaluated per vertex on the CPU.
//
//   * Host light. SkyCraft takes Skyrim's ambient cube and its 16 nearest lights. Here every vertex asks Portal 2 for the
//     lighting state a model standing there would get (VEngineModel016 ComputeLightingState, slot 26: the ambient cube
//     plus the up to 4 local lights the engine picked for that point, dynamic lights included) and evaluates it the way
//     Portal 2's vertex-lit model shader does: the ambient cube by the face normal, plus each local light's colour x
//     distance attenuation x spot cone x N.L at the vertex. So a placed block is lit exactly like a cube or Chell
//     standing on that spot, by the same lights.
//   * Minecraft's terms stay (SkyCraft): sky light darkens what Minecraft blocks roof over (lerp(0.3, 1, Curve(sky))), block
//     light (torches, glowstone) glows warm and is max'd with the host light; Minecraft's own vertex AO and tint are in the
//     vertex colour.
//   * Contact AO (SkyCraft VoxelAO, minecraft = false): a block face is darkened where Portal 2's geometry (world brushes
//     and static props) crowds it: short rays over the face's hemisphere, at about half strength (Minecraft's own AO
//     already darkens it next to other blocks).
//   * Contact shadows on Portal 2 (SkyCraft VoxelAO, minecraft = true): Portal 2's floor and walls are darkened where a
//     placed block meets them: blended skirts on the Portal 2 surface around the block's footprint (BandSkirts).
//   * The result is a linear light; the material system's own pipeline (UnlitGeneric: tonemap scale, sRGB, fog) turns it
//     into the pixel like Portal 2's own geometry (pcb_light_ramp checks that mapping).
#include "bridge.hpp"
#include "slots.hpp"
#include "platform/platform.hpp"
#include "growth.hpp"
#include "coords.hpp"
#include "link.hpp"

#include "Modules/MaterialSystem.hpp"
#include "Modules/Tier1.hpp"
#include "Utils/SDK/MeshBuilder.hpp"
#include "Utils/SDK/Trace.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bridge {

void EntLiveSet(std::unordered_set<void *> &out);  // ents.cpp
bool TerrainIsSolid(int bx, int by, int bz);       // terrain.cpp
QAngle MainViewAngles();                           // avatar.cpp
bool BlockLightIsOurs(const Vector &pos);          // blocklights.cpp

// ---- Portal 2's model lighting state ---------------------------------------------------------------------
// VEngineModel016 slot 26 ComputeLightingState(int n, const LightingQuery_t *, MaterialLightingState_t *, ITexture **).
// LightingQuery_t (16 bytes): Vector origin, uint16 instance (0xFFFF none), bool ambient boost. MaterialLightingState_t
// (0x1b8 bytes): Vector cube[6] (+X -X +Y -Y +Z -Z), Vector lighting origin, int local light count, LightDesc_t[4] (0x58
// each: type, colour, position, direction, range, falloff, attenuation 0/1/2, theta, phi, cos theta, cos phi, flags,
// 1/(cos theta - cos phi), range squared). Same layouts mclight.cpp uses.
static constexpr int kQueryBytes = 0x10, kStateBytes = 0x1b8, kStateCount = 0x54, kStateLights = 0x58, kLightBytes = 0x58;
using _ComputeLightingState = void(__rescall *)(void *thisptr, int n, const void *queries, void *states, void **envCubemaps);
static Interface *g_model = nullptr;
static _ComputeLightingState Cls = nullptr;

struct P2Light {
	int type;  // 1 point, 2 directional, 3 spot
	Vector col, pos, dir;
	float range, falloff, a0, a1, a2, cosTheta, cosPhi, oneOver;
};
struct P2State {
	float cube[6][3];
	int n;
	P2Light l[4];
};

static bool Ready() {
	if (Cls) return true;
	if (!g_model) g_model = Interface::Create(MODULE_NAME("engine"), "VEngineModel016", false);
	if (!g_model) return false;
	Cls = slots::Orig<_ComputeLightingState>(g_model, slots::kModelComputeLightingState);
	return Cls != nullptr;
}

static void Parse(const uint8_t *s, P2State &out) {
	std::memcpy(out.cube, s, sizeof out.cube);
	int n = *(const int *)(s + kStateCount);
	out.n = n < 0 ? 0 : n > 4 ? 4 : n;
	for (int i = 0; i < out.n; ++i) {
		const uint8_t *d = s + kStateLights + i * kLightBytes;
		const float *f = (const float *)d;
		P2Light &l = out.l[i];
		l.type = *(const int *)d;
		l.col = Vector(f[1], f[2], f[3]);
		l.pos = Vector(f[4], f[5], f[6]);
		l.dir = Vector(f[7], f[8], f[9]);
		l.range = f[10], l.falloff = f[11], l.a0 = f[12], l.a1 = f[13], l.a2 = f[14];
		l.cosTheta = f[17], l.cosPhi = f[18], l.oneOver = f[20];
	}
}

static int g_queries = 0, g_queryCalls = 0;
// Portal 2's lighting state at each point (main thread, in a client frame: from a console command it reads 0).
bool BandLightQuery(const std::vector<Vector> &pts, std::vector<P2State> &out) {
	out.assign(pts.size(), P2State{});
	if (!Ready() || pts.empty()) return false;
	const int kBatch = 64;
	static std::vector<uint8_t> q(kBatch * kQueryBytes), st(kBatch * kStateBytes);
	static std::vector<void *> env(kBatch);
	for (size_t first = 0; first < pts.size(); first += kBatch) {
		const int n = (int)std::min<size_t>(kBatch, pts.size() - first);
		std::memset(q.data(), 0, q.size());
		std::memset(st.data(), 0, st.size());
		for (int i = 0; i < n; ++i) {
			uint8_t *e = q.data() + i * kQueryBytes;
			std::memcpy(e, &pts[first + i], sizeof(Vector));
			*(uint16_t *)(e + 12) = 0xFFFF;
		}
		Cls(g_model->ThisPtr(), n, q.data(), st.data(), env.data());
		for (int i = 0; i < n; ++i) Parse(st.data() + i * kStateBytes, out[first + i]);
		g_queries += n, ++g_queryCalls;
	}
	return true;
}

// The vertex-lit model shader's sum: ambient cube by the normal, plus each local light (attenuation, spot cone, N.L).
// hasNormal false (plants, torches): the cube's average with a lifted top (SkyCraft) and the lights at 0.75.
Vector BandLightEval(const P2State &s, const Vector &p, const Vector &n, bool hasNormal) {
	Vector c(0, 0, 0);
	if (hasNormal) {
		const float n2[3] = {n.x * n.x, n.y * n.y, n.z * n.z};
		const int ax[3] = {n.x >= 0 ? 0 : 1, n.y >= 0 ? 2 : 3, n.z >= 0 ? 4 : 5};
		for (int a = 0; a < 3; ++a) c += Vector(s.cube[ax[a]][0], s.cube[ax[a]][1], s.cube[ax[a]][2]) * n2[a];
	} else {
		for (int a = 0; a < 6; ++a) c += Vector(s.cube[a][0], s.cube[a][1], s.cube[a][2]) * (a == 4 ? 2.0f / 6 : 1.0f / 6);
		c = c * (6.0f / 7.0f);
	}
	for (int i = 0; i < s.n; ++i) {
		const P2Light &l = s.l[i];
		if (l.type != 2 && BlockLightIsOurs(l.pos)) continue;  // Minecraft's own block light is in the vertex data already
		float k = 1.0f;
		Vector L;
		if (l.type == 2) {
			L = l.dir * -1.0f;
		} else {
			Vector d = l.pos - p;
			const float dist = std::fmax(1.0f, d.Length());
			if (l.range > 0 && dist > l.range) continue;
			L = d * (1.0f / dist);
			const float den = l.a0 + l.a1 * dist + l.a2 * dist * dist;
			k = den > 1e-6f ? 1.0f / den : 0.0f;
			if (l.type == 3) {
				const float cd = -(L.x * l.dir.x + L.y * l.dir.y + L.z * l.dir.z);
				if (cd <= l.cosPhi) continue;
				if (cd < l.cosTheta) {
					float t = (cd - l.cosPhi) * l.oneOver;
					t = t < 0 ? 0 : t > 1 ? 1 : t;
					k *= l.falloff > 0 && l.falloff != 1.0f ? std::pow(t, l.falloff) : t;
				}
			}
		}
		const float ndl = hasNormal ? std::fmax(0.0f, n.x * L.x + n.y * L.y + n.z * L.z) : 0.75f;
		c += l.col * (k * ndl);
	}
	return c;
}

// ---- for other files (scene.cpp, worldents.cpp, avatar.cpp): the same light as the placed blocks -----------------------
// Portal 2's model lighting at p (Source) as a 6-axis cube (+X -X +Y -Y +Z -Z), each axis = ambient cube + the local lights
// for that normal, linear; our Minecraft block lights left out (the Minecraft side carries block light). Client frame only.
// Apply by squared normal components (Source's ambient cube rule), then BandLinearToVertex for the vertex colour.
void BlockLightsSuspend();
void BlockLightsResume();
bool BandLightCube(const Vector &p, Vector out[6]) {
	std::vector<Vector> pts{p};
	std::vector<P2State> st;
	BlockLightsSuspend();
	const bool ok = BandLightQuery(pts, st);
	BlockLightsResume();
	static const Vector ax[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
	for (int a = 0; a < 6; ++a) out[a] = ok ? BandLightEval(st[0], p, ax[a], true) : Vector(0, 0, 0);
	return ok;
}
// (bridge #79) the same for many points in one batched query (cutlight.cpp: the cuts' mouths). ok[i] = 0 where the engine returned
// an empty state (no ambient cube and no light: a point it could not place yet); the caller retries those.
bool BandLightCubes(const std::vector<Vector> &pts, std::vector<std::array<Vector, 6>> &out, std::vector<char> &ok) {
	out.assign(pts.size(), {});
	ok.assign(pts.size(), 0);
	std::vector<P2State> st;
	BlockLightsSuspend();  // (our block lights are Minecraft's block light, carried by the mod's own light levels)
	const bool any = BandLightQuery(pts, st);
	BlockLightsResume();
	if (!any) return false;
	static const Vector ax[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
	for (size_t i = 0; i < pts.size(); ++i) {
		float sum = 0;
		for (int a = 0; a < 6; ++a) sum += st[i].cube[a][0] + st[i].cube[a][1] + st[i].cube[a][2];
		if (sum <= 0 && st[i].n == 0) continue;
		ok[i] = 1;
		for (int a = 0; a < 6; ++a) out[i][a] = BandLightEval(st[i], pts[i], ax[a], true);
	}
	return true;
}
// Linear light -> vertex colour (0-1) for UnlitGeneric with the sRGB atlas: the shader decodes vertex colours to linear and
// multiplies by the tonemap scale, as Portal 2's own models get their light (pcb_light_ramp, a18_light1).
float BandLinearToVertex(float l) { return std::pow(std::fmin(1.0f, std::fmax(0.0f, l)), 1.0f / 2.2f); }

// ---- contact AO from Portal 2's geometry ---------------------------------------------------------------
static float g_aoStrength = 0.5f;  // pcb_band_ao: on the blocks' own faces (SkyCraft 0.42; Minecraft's AO is there too)
static float g_skirtAlpha = 0.35f;  // the Portal 2 surface right at a block's edge (SkyCraft's 0.85 x its sphere term)
static float g_skirtWidth = 0.55f;  // blocks
static std::unordered_map<long long, float> g_aoCache;
PCB_GAUGE("bandlight.ao", g_aoCache.size());
static std::unordered_set<void *> g_serverEnts;
static double g_entsAt = -10;
static int g_aoTraces = 0;

static double NowS() {
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// World brushes and static props (anything not in the server's entity list): what stays put.
static bool TraceStatic(const Vector &a, const Vector &b, CGameTrace &tr) {
	struct StaticFilter : public ITraceFilter {
		virtual bool ShouldHitEntity(void *e, int) { return e && !g_serverEnts.count(e); }
		virtual TraceType_t GetTraceType() const { return TRACE_EVERYTHING; }
	} filter;
	Ray_t ray;
	const Vector d = b - a;
	ray.m_Delta = VectorAligned(d.x, d.y, d.z);
	ray.m_IsSwept = true;
	ray.m_Extents = VectorAligned(0, 0, 0);
	ray.m_IsRay = true;
	ray.m_StartOffset = VectorAligned(0, 0, 0);
	ray.m_Start = VectorAligned(a.x, a.y, a.z);
	ray.m_pWorldAxisTransform = nullptr;
	std::memset(&tr, 0, sizeof tr);
	engine.TraceRay(engine.traceServer->ThisPtr(), ray, MASK_SOLID & ~CONTENTS_MONSTER, &filter, &tr);
	++g_aoTraces;
	return tr.fraction < 1.0f || tr.startsolid;
}

static void RefreshEnts() {
	const double now = NowS();
	if (now - g_entsAt > 2.0) g_entsAt = now, EntLiveSet(g_serverEnts);
}

static long long PosKey(const Vector &p, int dir) {
	return (((long long)std::lround(p.x * 2) & 0xFFFFF) << 41 | ((long long)std::lround(p.y * 2) & 0xFFFFF) << 21 | ((long long)std::lround(p.z * 2) & 0xFFFFF) << 1) * 8 + dir;
}

// 1 = open, lower = Portal 2 geometry in front of the face near this vertex. p on the face, n its normal (Source).
float BandContactAO(const Vector &p, const Vector &n, int dir) {
	if (g_aoStrength <= 0) return 1.0f;
	const long long key = PosKey(p, dir);
	auto it = g_aoCache.find(key);
	if (it != g_aoCache.end()) return it->second;
	RefreshEnts();
	// two tangents
	Vector t1 = std::fabs(n.z) > 0.9f ? Vector(1, 0, 0) : Vector(0, 0, 1);
	Vector t2(n.y * t1.z - n.z * t1.y, n.z * t1.x - n.x * t1.z, n.x * t1.y - n.y * t1.x);
	t1 = Vector(t2.y * n.z - t2.z * n.y, t2.z * n.x - t2.x * n.z, t2.x * n.y - t2.y * n.x);
	const float R = 40.0f;  // one block
	const Vector o = p + n * 1.0f;
	// the normal, 4 at 45 degrees and 4 grazing (60 degrees): cosine-weighted
	static const float kDirs[9][3] = {{1, 0, 0}, {0.707f, 0.707f, 0}, {0.707f, -0.707f, 0}, {0.707f, 0, 0.707f}, {0.707f, 0, -0.707f},
	                                  {0.5f, 0.612f, 0.612f}, {0.5f, -0.612f, 0.612f}, {0.5f, 0.612f, -0.612f}, {0.5f, -0.612f, -0.612f}};
	float occ = 0, wsum = 0;
	for (auto &d : kDirs) {
		const Vector r = n * d[0] + t1 * d[1] + t2 * d[2];
		const float w = d[0];
		wsum += w;
		CGameTrace tr;
		if (!TraceStatic(o, o + r * R, tr)) continue;
		if (tr.startsolid) {
			occ += w;
			continue;
		}
		const float f = 1.0f - tr.fraction;
		occ += w * f * f;
	}
	float ao = 1.0f - g_aoStrength * (wsum > 0 ? occ / wsum : 0.0f);
	if (g_aoCache.size() > 600000) g_aoCache.clear();  // (bridge #53) bounded: a long session walks past ever more band vertices; a miss only costs the traces again
	g_aoCache.emplace(key, ao);
	return ao;
}

// ---- contact shadows on Portal 2's surfaces -------------------------------------------------------------
// MC directions (Direction ordinal: down, up, north, south, west, east) as MC block offsets
static const int kMcDir[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};

// Distance (blocks, along MC direction d) from the block's centre to the Portal 2 surface in front of that face, or -1.
static float SurfaceAlong(double cx, double cy, double cz, int d) {
	const Vector a = McToSrc(cx, cy, cz), b = McToSrc(cx + kMcDir[d][0] * 1.25, cy + kMcDir[d][1] * 1.25, cz + kMcDir[d][2] * 1.25);
	CGameTrace tr;
	if (!TraceStatic(a, b, tr) || tr.startsolid) return -1;
	return 1.25f * tr.fraction;
}

// Blended black quads on the Portal 2 surface around block (bx, by, bz)'s footprint where it meets that surface.
// Appends quads as 4 vertices each (Source positions, alpha 0-255).
struct SkirtVert {
	Vector p;
	uint8_t a;
};
void BandSkirts(int bx, int by, int bz, std::vector<SkirtVert> &out) {
	if (g_skirtAlpha <= 0) return;
	RefreshEnts();
	const double cx = bx + 0.5, cy = by + 0.5, cz = bz + 0.5;
	for (int d = 0; d < 6; ++d) {
		if (TerrainIsSolid(bx + kMcDir[d][0], by + kMcDir[d][1], bz + kMcDir[d][2])) continue;
		const float h = SurfaceAlong(cx, cy, cz, d);
		// the Portal 2 surface touches this face: from inside the block (sunk in) to a sliver beyond it
		static int logged = 0;
		if (h >= 0 && logged < 12) ++logged, console->Print("[bridge] skirt: block %d %d %d face %d: Portal 2 surface %.3f blocks from the centre%s\n", bx, by, bz, d, h, h < 0.02f || h > 0.55f ? " (no skirt)" : "");
		if (h < 0.02f || h > 0.55f) continue;
		const float a0 = g_skirtAlpha;
		const float lift = 0.25f / kUnitsPerBlock;  // off the surface (no z-fighting)
		const double sh = h - lift;  // plane offset from the centre along d
		int tang[4], nt = 0;
		for (int e = 0; e < 6; ++e)
			if (e / 2 != d / 2) tang[nt++] = e;
		auto onPlane = [&](double u, double v, int eu, int ev) {  // u along tangent eu, v along ev, from the centre
			return McToSrc(cx + kMcDir[d][0] * sh + kMcDir[eu][0] * u + kMcDir[ev][0] * v, cy + kMcDir[d][1] * sh + kMcDir[eu][1] * u + kMcDir[ev][1] * v,
			               cz + kMcDir[d][2] * sh + kMcDir[eu][2] * u + kMcDir[ev][2] * v);
		};
		const double W = g_skirtWidth;
		bool open[6] = {};
		for (int k = 0; k < 4; ++k) {
			const int e = tang[k];
			const int nx = bx + kMcDir[e][0], ny = by + kMcDir[e][1], nz = bz + kMcDir[e][2];
			if (TerrainIsSolid(nx, ny, nz)) continue;
			// the surface carries on under the skirt at the same height
			const float h2 = SurfaceAlong(nx + 0.5, ny + 0.5, nz + 0.5, d);
			if (h2 < 0 || std::fabs(h2 - h) > 0.1f) continue;
			open[e] = true;
			// the other in-plane axis (axes: 0 y, 1 z, 2 x; ordinal 2a is its minus side, 2a+1 its plus side)
			const int fa = 3 - d / 2 - e / 2;
			const int fp = 2 * fa + 1, fm = 2 * fa;
			// edge strip: from the block's side (0.5) out to 0.5 + W along e, spanning -0.5..0.5 along the other axis
			const Vector p0 = onPlane(0.5, 0.5, e, fp), p1 = onPlane(0.5 + W, 0.5, e, fp), p2 = onPlane(0.5 + W, 0.5, e, fm), p3 = onPlane(0.5, 0.5, e, fm);
			const uint8_t A = (uint8_t)std::lround(255 * a0);
			out.push_back({p0, A}), out.push_back({p1, 0}), out.push_back({p2, 0}), out.push_back({p3, A});
		}
		// corner pieces where both edges are open and the diagonal neighbour is not a block
		for (int i = 0; i < 4; ++i)
			for (int j = 0; j < 4; ++j) {
				const int e1 = tang[i], e2 = tang[j];
				if (e1 / 2 >= e2 / 2 || !open[e1] || !open[e2]) continue;
				if (TerrainIsSolid(bx + kMcDir[e1][0] + kMcDir[e2][0], by + kMcDir[e1][1] + kMcDir[e2][1], bz + kMcDir[e1][2] + kMcDir[e2][2])) continue;
				const uint8_t A = (uint8_t)std::lround(255 * a0);
				out.push_back({onPlane(0.5, 0.5, e1, e2), A}), out.push_back({onPlane(0.5 + W, 0.5, e1, e2), 0});
				out.push_back({onPlane(0.5 + W, 0.5 + W, e1, e2), 0}), out.push_back({onPlane(0.5, 0.5 + W, e1, e2), 0});
			}
	}
}

static IMaterial *g_skirtMat = nullptr, *g_rampMat = nullptr;
IMaterial *BandSkirtMaterial() {
	if (!g_skirtMat) {
		auto kv = new KeyValues("UnlitGeneric");
		kv->SetInt("$vertexcolor", 1);
		kv->SetInt("$vertexalpha", 1);
		kv->SetInt("$translucent", 1);
		kv->SetInt("$nocull", 1);
		g_skirtMat = materialSystem->CreateMaterial("_pcb_band_skirt", kv);
	}
	return g_skirtMat;
}

void BandLightCacheClear() { g_aoCache.clear(); }

// ---- calibration: the vertex colour -> pixel mapping -----------------------------------------------------
static int g_ramp = 0;
// Main thread, in a view: 8 unlit squares in front of the eye with vertex colour 1/32 .. 1 (doubling), then a white one.
void BandLightRampDraw(const Vector &eye) {
	if (!g_ramp) return;
	if (!g_rampMat) {
		auto kv = new KeyValues("UnlitGeneric");
		kv->SetInt("$vertexcolor", 1);
		kv->SetInt("$nocull", 1);
		g_rampMat = materialSystem->CreateMaterial("_pcb_light_ramp", kv);
	}
	QAngle a = MainViewAngles();
	const float yaw = a.y * (float)M_PI / 180.0f, pitch = a.x * (float)M_PI / 180.0f;
	const Vector fwd(std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), -std::sin(pitch));
	const Vector right(std::sin(yaw), -std::cos(yaw), 0);
	const Vector up(right.y * fwd.z - right.z * fwd.y, right.z * fwd.x - right.x * fwd.z, right.x * fwd.y - right.y * fwd.x);
	const Vector c = eye + fwd * 60.0f + up * 8.0f;
	MeshBuilder mb(g_rampMat, PrimitiveType::QUADS, 9);
	for (int i = 0; i < 9; ++i) {
		const float v = i < 8 ? std::pow(2.0f, (float)i - 7.0f) : 1.0f;  // 1/128 .. 1, then 1
		const uint8_t b = (uint8_t)std::lround(255 * v);
		const Vector o = c + right * (-36.0f + 8.0f * i);
		const Vector q[4] = {o + up * 4, o + right * 7 + up * 4, o + right * 7 - up * 4, o - up * 4};
		for (auto &p : q) {
			mb.Position(p);
			mb.Color(Color(b, b, b, 255));
			mb.AdvanceVertex();
		}
	}
	mb.Draw();
}

// ---- commands -----------------------------------------------------------------------------------------------
CON_COMMAND(pcb_light_ramp, "pcb_light_ramp 0|1 - (calibration) 9 unlit squares in front of the eye, vertex colour 1/128 .. 1 doubling, then 1\n") {
	if (args.ArgC() > 1) g_ramp = std::atoi(args[1]);
	console->Print("[bridge] light ramp %d\n", g_ramp);
}

void TerrainBandRebuild();  // terrain.cpp
CON_COMMAND(pcb_band_ao, "pcb_band_ao [strength=0.5] [skirt_alpha=0.42] [skirt_width=0.55] - contact AO on placed blocks from Portal 2's geometry, and their contact shadows on it; rebuilds\n") {
	if (args.ArgC() > 1) g_aoStrength = (float)std::atof(args[1]);
	if (args.ArgC() > 2) g_skirtAlpha = (float)std::atof(args[2]);
	if (args.ArgC() > 3) g_skirtWidth = (float)std::atof(args[3]);
	g_aoCache.clear();
	TerrainBandRebuild();
	console->Print("[bridge] band AO %.2f, skirt alpha %.2f width %.2f\n", g_aoStrength, g_skirtAlpha, g_skirtWidth);
}

CON_COMMAND(pcb_band_light_stats, "pcb_band_light_stats - lighting-state queries and AO traces so far\n") {
	console->Print("[bridge] band light: %d lighting-state queries in %d calls, %d AO traces, %zu AO cached\n", g_queries, g_queryCalls, g_aoTraces, g_aoCache.size());
}

// Minecraft's own view at the player's camera (NativeShot.java): cache/bridge/mcnative/mcnative_<name>.png. Minecraft only
// renders its level for a host camera, so the main camera is requested for a while (CompositeNativeMain) and the request
// file is written once that has had time to arrive.
void CompositeNativeMain(int frames);  // composite.cpp
static std::string g_nativeName;
static int g_nativeWait = 0;
CON_COMMAND(pcb_mc_native, "pcb_mc_native <name> - (tests) Minecraft saves its own view of the level from the player's camera as cache/bridge/mcnative/mcnative_<name>.png\n") {
	if (args.ArgC() < 2) return;
	g_nativeName = args[1];
	g_nativeWait = 12;
	CompositeNativeMain(45);
	console->Print("[bridge] native Minecraft view requested: %s\n", args[1]);
}
static void NativeFrame() {
	if (g_nativeWait <= 0 || --g_nativeWait > 0) return;
	const std::string dir = CacheDir() + "/mcnative";
	if (!plat::MakeDirs(dir)) return;
	if (FILE *f = std::fopen((dir + "/request").c_str(), "w")) {
		std::fprintf(f, "%s\n", g_nativeName.c_str());
		std::fclose(f);
	}
}

// Lighting state at a point (diagnostics): queued to the next client frame (ComputeLightingState reads 0 from a command)
static bool g_probeWant = false;
static Vector g_probeAt;
CON_COMMAND(pcb_p2light_at, "pcb_p2light_at <x> <y> <z> - (diagnostics) Portal 2's model lighting state there, printed in the next frame\n") {
	if (args.ArgC() < 4) return;
	g_probeAt = Vector((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3]));
	g_probeWant = true;
}
CON_COMMAND(pcb_p2light_mc, "pcb_p2light_mc <x> <y> <z> - (diagnostics) Portal 2's model lighting state at a Minecraft position, printed in the next frame\n") {
	if (args.ArgC() < 4) return;
	g_probeAt = McToSrc(std::atof(args[1]), std::atof(args[2]), std::atof(args[3]));
	g_probeWant = true;
}
// ---- (bridge #27) the Minecraft player's hand: lit like a placed block at that spot ----------------------------------------
// The hand and held item are drawn by Minecraft (glhud.cpp shows its image). In a chamber its own light there is the chamber
// band's open sky, so the hand was too bright (day) or too dark (night) next to the blocks Portal 2 lights. Every client frame
// in Minecraft mode outside the region: Portal 2's model lighting state a little in front of the eye (where the hand is),
// evaluated like a placed block without a normal (BandLightEval: the cube's mean with a lifted top, its lights), with the
// band's bounce floor; our own block lights left out (Minecraft adds its block light, as for the blocks); times the
// tonemap scale. The mod (HandLight.java) applies the band's roof and block-light max and picks the lightmap texel.
float TerrainRenderTone();    // terrain.cpp: the frame's tonemap scale, read on the render thread
bool McModeActive();          // mc.cpp
bool ClipHasRegion();                      // clip.cpp
bool ClipRegionContains(const Vector &p);  // clip.cpp
static bool g_handOn = true;
static Vector g_handLight(0, 0, 0);
static bool g_handHave = false;
static double g_handAt = 0;
static int g_handLogs = 0;
static Vector g_handRaw(0, 0, 0);
static float g_handTone = 1;
static void HandLightFrame() {
	const ViewRecord &v = LastMainView();
	const float pr = v.angles.x * (float)M_PI / 180, yr = v.angles.y * (float)M_PI / 180;
	const Vector f(std::cos(pr) * std::cos(yr), std::cos(pr) * std::sin(yr), -std::sin(pr));
	const Vector r(std::sin(yr), -std::cos(yr), 0);  // (no roll)
	const Vector u(std::sin(pr) * std::cos(yr), std::sin(pr) * std::sin(yr), std::cos(pr));
	Vector p = v.origin + f * 14.0f + r * 6.0f - u * 8.0f;  // about where the right hand is
	const bool on = g_handOn && McModeActive() && !(ClipHasRegion() && ClipRegionContains(v.origin));
	if (!on) {
		link::SetHostHandLight(false, 0, 0, 0, 1);
		g_handHave = false;
		return;
	}
	if (engine.GetPointContentsWorld && (engine.GetPointContentsWorld(engine.traceServer->ThisPtr(), p, MASK_SOLID) & CONTENTS_SOLID)) p = v.origin;
	std::vector<Vector> pts{p};
	std::vector<P2State> st;
	BlockLightsSuspend();
	const bool ok = BandLightQuery(pts, st);
	BlockLightsResume();
	float sum = 0;
	if (ok)
		for (int a = 0; a < 6; ++a) sum += st[0].cube[a][0] + st[0].cube[a][1] + st[0].cube[a][2];
	if (!ok || (sum <= 0 && st[0].n == 0)) {
		if (!g_handHave) return;  // nothing yet: Minecraft's own until the first good state
	} else {
		Vector L = BandLightEval(st[0], p, Vector(0, 0, 1), false);
		float mean[3] = {0, 0, 0};
		for (int a = 0; a < 6; ++a)
			for (int c = 0; c < 3; ++c) mean[c] += st[0].cube[a][c] / 6.0f;
		L.x = std::fmax(L.x, mean[0] * 0.1f), L.y = std::fmax(L.y, mean[1] * 0.1f), L.z = std::fmax(L.z, mean[2] * 0.1f);  // terrain.cpp g_bandFill
		g_handRaw = L;
		const double now = NowS();
		const float dt = g_handHave ? (float)std::fmin(0.25, now - g_handAt) : 1.0f;
		g_handAt = now;
		const float k = g_handHave ? 1.0f - std::exp(-dt / 0.15f) : 1.0f;  // 150 ms, as mclight.cpp
		g_handLight = g_handLight + (L - g_handLight) * k;
		g_handHave = true;
	}
	g_handTone = std::fmax(0.01f, TerrainRenderTone());
	link::SetHostHandLight(true, g_handLight.x * g_handTone, g_handLight.y * g_handTone, g_handLight.z * g_handTone, g_handTone);
	if (g_handLogs < 6 || (g_handLogs < 200 && g_handLogs % 50 == 0))
		console->Print("[bridge] hand light at (%.0f %.0f %.0f): linear (%.3f %.3f %.3f), tonemap %.3f, %d local lights\n", p.x, p.y, p.z, g_handLight.x, g_handLight.y, g_handLight.z, g_handTone,
		               ok ? st[0].n : -1);
	++g_handLogs;
}
CON_COMMAND(pc_mc_hand_light, "pc_mc_hand_light [0|1] - (bridge #27) light the Minecraft player's hand with Portal 2's light in chambers, as placed blocks are\n") {
	if (args.ArgC() > 1) g_handOn = std::atoi(args[1]) != 0;
	console->Print("[bridge] hand light %d: linear (%.3f %.3f %.3f) raw (%.3f %.3f %.3f) tonemap %.3f\n", g_handOn, g_handLight.x, g_handLight.y, g_handLight.z, g_handRaw.x, g_handRaw.y, g_handRaw.z,
	               g_handTone);
}

void BandLightFrame() {
	NativeFrame();
	HandLightFrame();
	if (!g_probeWant) return;
	g_probeWant = false;
	std::vector<Vector> p{g_probeAt};
	std::vector<P2State> s;
	BandLightQuery(p, s);
	const P2State &t = s[0];
	console->Print("[bridge] p2light at (%.0f %.0f %.0f): cube +X (%.3f %.3f %.3f) -X (%.3f %.3f %.3f) +Y (%.3f %.3f %.3f) -Y (%.3f %.3f %.3f) +Z (%.3f %.3f %.3f) -Z (%.3f %.3f %.3f), %d lights\n",
	               g_probeAt.x, g_probeAt.y, g_probeAt.z, t.cube[0][0], t.cube[0][1], t.cube[0][2], t.cube[1][0], t.cube[1][1], t.cube[1][2], t.cube[2][0], t.cube[2][1], t.cube[2][2], t.cube[3][0],
	               t.cube[3][1], t.cube[3][2], t.cube[4][0], t.cube[4][1], t.cube[4][2], t.cube[5][0], t.cube[5][1], t.cube[5][2], t.n);
	for (int i = 0; i < t.n; ++i)
		console->Print("[bridge]   light %d type %d colour (%.3f %.3f %.3f) at (%.0f %.0f %.0f) atten %.4f %.6f %.8f range %.0f\n", i, t.l[i].type, t.l[i].col.x, t.l[i].col.y, t.l[i].col.z, t.l[i].pos.x,
		               t.l[i].pos.y, t.l[i].pos.z, t.l[i].a0, t.l[i].a1, t.l[i].a2, t.l[i].range);
	const Vector up = BandLightEval(t, g_probeAt, Vector(0, 0, 1), true);
	console->Print("[bridge]   evaluated: top (%.3f %.3f %.3f); ComputeLighting top (%.3f %.3f %.3f)\n", up.x, up.y, up.z, engine.LightAt(g_probeAt, Vector(0, 0, 1)).x,
	               engine.LightAt(g_probeAt, Vector(0, 0, 1)).y, engine.LightAt(g_probeAt, Vector(0, 0, 1)).z);
}

void BandLightShutdown() {
	if (g_model) Interface::Delete(g_model);
	g_model = nullptr;
	Cls = nullptr;
}

}  // namespace bridge
