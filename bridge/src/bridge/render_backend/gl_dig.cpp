// Portalcraft bridge (bridge #73): the dig-render verification harness (DESIGN-dig-render.md section 4).
//
// One probe = one frame of the main view, judged from data:
//   * what the frame SHOWS: the depth and colour of a grid of sample pixels, read back on the render thread right after the main
//     view's translucent pass (before the HUD and the view model), with the view and projection matrices the frame was really
//     drawn with (IMatRenderContext::GetMatrix on the render thread);
//   * what it SHOULD show, from geometry: each sample's ray (from those matrices) is walked through Portal 2's world with the dug
//     cells removed (the engine's own world trace, the hits inside or on a dug cell skipped as DESIGN section 2 defines), against the
//     Minecraft solids (TerrainRay), water, and entities (an entity trace); and the same ray without the cut (where the old wall
//     was);
//   * which entities the client has: every networked server entity, dormant or not on the client (dormant = the server does not
//     send it), with its class, name and origin;
//   * the camera's leaf (contents, cluster, area) and the visibility origins this view handed the engine.
// The result goes to cache/bridge/digharness/<dir>/<name>.json plus one summary line in the log; tools/digharness/judge.py compares
// probes (A/B colour for ghosts, two frames for z-fighting, open vs inside for entities).
//
// Commands: pc_dh_probe <dir> <name> [gridW gridH], pc_dh_mine <x y z> [x1 y1 z1] (mine the cells of a Source box through the mod,
// as a player would: Minecraft reveals around them), pc_dh_status.
#include "bridge/bridge.hpp"
#include "bridge/slots.hpp"
#include "bridge/coords.hpp"
#include "bridge/dig.hpp"
#include "gl_api.hpp"

#include "Modules/MaterialSystem.hpp"
#include "Modules/Tier1.hpp"
#include "Utils/Memory.hpp"
#include "Utils/SDK/MeshBuilder.hpp"
#include "Utils/lodepng.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <set>
#include <array>

namespace bridge {

void RunOnRenderThread(void (*fn)(void *), void *arg);  // terrain.cpp
bool TerrainRay(const double s[3], const double d[3], double &tHit, int &axis, int &sign);  // terrain.cpp
void EntLiveSet(std::unordered_set<void *> &out);  // ents.cpp
int VisLastMainOrigins(Vector *out, int max);       // visclamp.cpp: the vis origins the main view's last ViewSetupVisEx passed on
const char *McMapName();                            // mc.cpp
bool McModeActive();                                // mc.cpp
Vector McFeetSrc();                                 // mc.cpp

namespace {

using namespace gl;

// ---- engine pieces ---------------------------------------------------------------------------------------------------------
Interface *g_entList = nullptr;   // VClientEntityList003
Interface *g_engServer = nullptr; // VEngineServer022
// slot numbers per operating system: bridge/slots.hpp (IClientNetworkable 9 reads the dormant byte, 10 the index; IServerUnknown GetRefEHandle returns
// &m_RefEHandle, index = low 16 bits; IVEngineServer GetClusterForOrigin / GetArea)
constexpr int kGetHighestEntityIndex = 6;

using slots::VCall;

bool EnsureInterfaces() {
	if (!g_entList) g_entList = Interface::Create(MODULE_NAME("client"), "VClientEntityList003", false);
	if (!g_engServer) g_engServer = Interface::Create(MODULE_NAME("engine"), "VEngineServer022", false);
	return g_entList && g_engServer;
}

// ---- the capture (render thread) -------------------------------------------------------------------------------------------
struct Capture {
	float view[4][4] = {}, proj[4][4] = {};
	int W = 0, H = 0, vx = 0, vy = 0;
	int fbo = 0, samples = 0;
	bool resolved = false;
	std::vector<float> depth;
	std::vector<uint8_t> rgba;
	std::string err;
};
std::mutex g_capMu;
std::shared_ptr<Capture> g_capDone;  // set by the render thread
std::atomic<int> g_capPending{0};

GLuint g_rFbo = 0, g_rCol = 0, g_rDep = 0;
int g_rW = 0, g_rH = 0;

void CaptureOnRenderThread(void *) {
	auto cap = std::make_shared<Capture>();
	IMatRenderContext *rc = materialSystem->GetRenderContext();
	if (rc) {
		VCall<void>(rc, slots::kRcFlush, false);
		VCall<void>(rc, slots::kRcGetMatrix, 0, (float *)cap->view);
		VCall<void>(rc, slots::kRcGetMatrix, 1, (float *)cap->proj);
	}
	if (!gl::Load()) {
		cap->err = "GL entry points missing";
	} else {
		GLint vp[4] = {0, 0, 0, 0}, drawFbo = 0, readFbo = 0, rb = 0, packBuf = 0, align = 4, rowLen = 0, skipR = 0, skipP = 0, sampleBufs = 0, samples = 0;
		GetIntegerv(VIEWPORT, vp);
		GetIntegerv(DRAW_FRAMEBUFFER_BINDING, &drawFbo);
		GetIntegerv(READ_FRAMEBUFFER_BINDING, &readFbo);
		GetIntegerv(RENDERBUFFER_BINDING, &rb);
		GetIntegerv(PIXEL_PACK_BUFFER_BINDING, &packBuf);
		GetIntegerv(PACK_ALIGNMENT, &align);
		GetIntegerv(PACK_ROW_LENGTH, &rowLen);
		GetIntegerv(PACK_SKIP_ROWS, &skipR);
		GetIntegerv(PACK_SKIP_PIXELS, &skipP);
		GetIntegerv(SAMPLE_BUFFERS, &sampleBufs);
		GetIntegerv(SAMPLES, &samples);
		GetError();
		cap->vx = vp[0], cap->vy = vp[1], cap->W = vp[2], cap->H = vp[3], cap->fbo = drawFbo, cap->samples = sampleBufs ? samples : 0;
		const int W = vp[2], H = vp[3];
		if (W > 0 && H > 0 && W <= 8192 && H <= 8192) {
			BindBuffer(PIXEL_PACK_BUFFER, 0);
			PixelStorei(PACK_ALIGNMENT, 1), PixelStorei(PACK_ROW_LENGTH, 0), PixelStorei(PACK_SKIP_ROWS, 0), PixelStorei(PACK_SKIP_PIXELS, 0);
			GLuint src = (GLuint)drawFbo;
			int sx = vp[0], sy = vp[1];
			if (sampleBufs) {  // a multisampled target cannot be read: resolve it into ours first
				if (!g_rFbo || g_rW != W || g_rH != H) {
					if (g_rFbo) DeleteFramebuffers(1, &g_rFbo), DeleteRenderbuffers(1, &g_rCol), DeleteRenderbuffers(1, &g_rDep);
					GenFramebuffers(1, &g_rFbo), GenRenderbuffers(1, &g_rCol), GenRenderbuffers(1, &g_rDep);
					BindRenderbuffer(RENDERBUFFER, g_rCol), RenderbufferStorage(RENDERBUFFER, RGBA8, W, H);
					BindRenderbuffer(RENDERBUFFER, g_rDep), RenderbufferStorage(RENDERBUFFER, DEPTH24_STENCIL8, W, H);
					BindFramebuffer(DRAW_FRAMEBUFFER, g_rFbo);
					FramebufferRenderbuffer(DRAW_FRAMEBUFFER, COLOR_ATTACHMENT0, RENDERBUFFER, g_rCol);
					FramebufferRenderbuffer(DRAW_FRAMEBUFFER, DEPTH_STENCIL_ATTACHMENT, RENDERBUFFER, g_rDep);
					g_rW = W, g_rH = H;
				}
				BindFramebuffer(READ_FRAMEBUFFER, (GLuint)drawFbo);
				BindFramebuffer(DRAW_FRAMEBUFFER, g_rFbo);
				BlitFramebuffer(vp[0], vp[1], vp[0] + W, vp[1] + H, 0, 0, W, H, COLOR_BUFFER_BIT | DEPTH_BUFFER_BIT, NEAREST);
				const GLenum e = GetError();
				if (e != NO_ERROR) cap->err = "resolve blit failed (GL error " + std::to_string(e) + ")";
				else src = g_rFbo, sx = 0, sy = 0, cap->resolved = true;
			}
			BindFramebuffer(READ_FRAMEBUFFER, src);
			cap->depth.resize((size_t)W * H);
			cap->rgba.resize((size_t)W * H * 4);
			ReadPixels(sx, sy, W, H, DEPTH_COMPONENT, FLOAT, cap->depth.data());
			ReadPixels(sx, sy, W, H, RGBA, UNSIGNED_BYTE, cap->rgba.data());
			const GLenum e = GetError();
			if (e != NO_ERROR) cap->err += (cap->err.empty() ? "" : "; ") + std::string("readback GL error ") + std::to_string(e);
			// put back what togl expects
			BindFramebuffer(READ_FRAMEBUFFER, (GLuint)readFbo);
			BindFramebuffer(DRAW_FRAMEBUFFER, (GLuint)drawFbo);
			BindRenderbuffer(RENDERBUFFER, (GLuint)rb);
			BindBuffer(PIXEL_PACK_BUFFER, (GLuint)packBuf);
			PixelStorei(PACK_ALIGNMENT, align), PixelStorei(PACK_ROW_LENGTH, rowLen), PixelStorei(PACK_SKIP_ROWS, skipR), PixelStorei(PACK_SKIP_PIXELS, skipP);
		} else {
			cap->err = "bad viewport";
		}
	}
	if (rc) VCall<int>(rc, slots::kRcRelease);
	std::lock_guard<std::mutex> lk(g_capMu);
	g_capDone = cap;
}

// ---- matrices -----------------------------------------------------------------------------------------------------------------
struct M4 {
	double m[4][4];
};
M4 Mul(const M4 &a, const M4 &b) {
	M4 r{};
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j)
			for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
	return r;
}
M4 Transpose(const M4 &a) {
	M4 r{};
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j) r.m[i][j] = a.m[j][i];
	return r;
}
bool Invert(const M4 &a, M4 &out) {
	double m[4][8];
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 8; ++j) m[i][j] = j < 4 ? a.m[i][j] : (j - 4 == i ? 1 : 0);
	for (int c = 0; c < 4; ++c) {
		int p = c;
		for (int r = c + 1; r < 4; ++r)
			if (std::fabs(m[r][c]) > std::fabs(m[p][c])) p = r;
		if (std::fabs(m[p][c]) < 1e-12) return false;
		if (p != c)
			for (int j = 0; j < 8; ++j) std::swap(m[p][j], m[c][j]);
		const double d = m[c][c];
		for (int j = 0; j < 8; ++j) m[c][j] /= d;
		for (int r = 0; r < 4; ++r)
			if (r != c) {
				const double f = m[r][c];
				for (int j = 0; j < 8; ++j) m[r][j] -= f * m[c][j];
			}
	}
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j) out.m[i][j] = m[i][j + 4];
	return true;
}
void Apply(const M4 &a, const double v[4], double o[4]) {
	for (int i = 0; i < 4; ++i) o[i] = a.m[i][0] * v[0] + a.m[i][1] * v[1] + a.m[i][2] * v[2] + a.m[i][3] * v[3];
}
M4 FromF(const float f[4][4]) {
	M4 r{};
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j) r.m[i][j] = f[i][j];
	return r;
}

// clip = M * (world, 1) for the frame. The material system's matrices are taken as they come; which product / transpose maps a point
// in front of the camera to the screen centre is found from the data (a point 200 u ahead must land at x = y = 0, 0 < z < w).
bool CalibrateClip(const float view[4][4], const float proj[4][4], const Vector &eye, const QAngle &ang, M4 &clip, std::string &how) {
	const float pr = ang.x * (float)M_PI / 180.0f, yr = ang.y * (float)M_PI / 180.0f;
	const Vector fwd(std::cos(pr) * std::cos(yr), std::cos(pr) * std::sin(yr), -std::sin(pr));
	const Vector q = eye + fwd * 200.0f;
	const double qv[4] = {q.x, q.y, q.z, 1.0};
	const M4 V = FromF(view), P = FromF(proj);
	const M4 cands[4] = {Mul(P, V), Transpose(Mul(V, P)), Mul(Transpose(P), Transpose(V)), Transpose(Mul(P, V))};
	const char *names[4] = {"P*V", "(V*P)^T", "P^T*V^T", "(P*V)^T"};
	double best = 1e30;
	int bi = -1;
	for (int i = 0; i < 4; ++i) {
		double o[4];
		Apply(cands[i], qv, o);
		if (o[3] <= 0) continue;
		const double err = std::fabs(o[0] / o[3]) + std::fabs(o[1] / o[3]) + (o[2] / o[3] < 0 || o[2] / o[3] > 1 ? 1 : 0);
		if (err < best) best = err, bi = i;
	}
	if (bi < 0 || best > 0.05) {
		how = "no matrix arrangement puts the view direction at the screen centre";
		return false;
	}
	clip = cands[bi];
	how = names[bi];
	return true;
}

// ---- expected hits ---------------------------------------------------------------------------------------------------------
enum Kind { kNone = 0, kHost, kWater, kSky, kMcBlock, kMcCover, kEntity };
const char *kKindNames[] = {"none", "host", "water", "sky", "mc", "mccover", "entity"};
struct Hit {
	float d = 1e30f;
	int kind = kNone;
	std::string what;  // surface or entity class
};

constexpr int kMaskVisSolid = 0x1 | 0x8;     // CONTENTS_SOLID | CONTENTS_GRATE
constexpr int kMaskWater = 0x20 | 0x10;      // CONTENTS_WATER | CONTENTS_SLIME
constexpr unsigned kSurfSky2D = 0x2, kSurfSky = 0x4, kSurfNodraw = 0x80, kSurfHint = 0x100, kSurfSkip = 0x200;
constexpr float kEps = 0.25f;  // DESIGN 2: the owner-cell step along the ray

bool SolidAt(const Vector &p) { return (engine.GetPointContentsWorld(engine.traceServer->ThisPtr(), p, kMaskVisSolid) & kMaskVisSolid) != 0; }

// The cut's rule (DESIGN 2): a host surface point is removed if the cell behind or in front of it is dug, a step e along the surface's
// normal (bridge #76; was along the ray, as the shaders were).
bool CutRemoves(const Vector &hp, const Vector &normal) { return DigIsDugSrc(hp + normal * kEps) || DigIsDugSrc(hp - normal * kEps); }

// Walk the ray through dug cells from distance t. Returns >= 0: the distance where it left the dug cells into OPEN air (continue
// the world trace there); sets `hit` when it left them into host solid (that boundary is covered by Minecraft: a revealed block or
// a hole wall), or reached maxD.
float MarchCut(const Vector &eye, const Vector &dir, float t, float maxD, Hit &hit) {
	const float step = 2.0f;
	float prev = t;
	for (; t < maxD; prev = t, t += step) {
		const Vector p = eye + dir * t;
		if (DigIsDugSrc(p)) continue;
		// left the dug cells between prev and t: find the boundary
		float a = prev, b = t;
		for (int i = 0; i < 8; ++i) {
			const float m = 0.5f * (a + b);
			if (DigIsDugSrc(eye + dir * m)) a = m;
			else b = m;
		}
		if (SolidAt(eye + dir * (b + 0.1f))) {
			hit.d = b, hit.kind = kMcCover, hit.what = "cut boundary";
			return -1;
		}
		return b + 0.05f;
	}
	hit.d = 1e30f, hit.kind = kNone;
	return -1;
}

Hit HostWalk(const Vector &eye, const Vector &dir, float maxD, bool cut) {
	Hit h;
	float t = 0;
	if (cut && DigIsDugSrc(eye)) {
		t = MarchCut(eye, dir, 0, maxD, h);
		if (t < 0) return h;
	}
	for (int iter = 0; iter < 24 && t < maxD; ++iter) {
		CGameTrace tr;
		const Vector s = eye + dir * t;
		engine.Trace(s, eye + dir * maxD, Vector(0, 0, 0), kMaskVisSolid | kMaskWater, tr, true);
		if (tr.startsolid) {
			// inside host solid (not dug): walk on until open air or a dug cell (the camera embedded, or a skipped no-draw face)
			float u = t;
			bool out = false;
			for (int k = 0; k < 200 && u < maxD; ++k, u += 4.0f) {
				const Vector p = eye + dir * u;
				if (cut && DigIsDugSrc(p)) {
					u = MarchCut(eye, dir, u, maxD, h);
					if (u < 0) return h;
					out = true;
					break;
				}
				if (!SolidAt(p)) {
					out = true;
					break;
				}
			}
			if (!out) return Hit();
			t = u + 0.1f;
			continue;
		}
		if (tr.fraction >= 1.0f) return Hit();
		const float d = t + tr.fraction * (maxD - t);
		const Vector hp = eye + dir * d;
		const unsigned fl = tr.surface.flags;
		if (fl & (kSurfSky | kSurfSky2D)) {
			h.d = d, h.kind = kSky, h.what = tr.surface.name ? tr.surface.name : "";
			return h;
		}
		if (fl & (kSurfNodraw | kSurfHint | kSurfSkip)) {  // not drawn: look through it
			t = d + 0.5f;
			continue;
		}
		if (cut && CutRemoves(hp, tr.plane.normal)) {
			t = MarchCut(eye, dir, d + kEps, maxD, h);
			if (t < 0) return h;
			continue;
		}
		h.d = d, h.kind = (tr.contents & kMaskWater) ? kWater : kHost, h.what = tr.surface.name ? tr.surface.name : "";
		return h;
	}
	return h;
}

struct SkipFilter : public ITraceFilter {
	void *skip = nullptr;
	virtual bool ShouldHitEntity(void *ent, int) { return ent != skip; }
	virtual TraceType_t GetTraceType() const { return TRACE_EVERYTHING; }
};

// (entity index from a server entity: its handle's low 16 bits)
int ServerEntIndex(void *ent) {
	if (!ent) return -1;
	const unsigned *h = VCall<const unsigned *>(ent, slots::kServerGetRefEHandle);
	return h ? (int)(*h & 0xFFFF) : -1;
}

Hit EntityHit(const Vector &eye, const Vector &dir, float maxD, void *player) {
	Hit h;
	SkipFilter f;
	f.skip = player;
	Ray_t ray;
	const Vector end = eye + dir * maxD;
	const Vector delta = end - eye;
	ray.m_Delta = VectorAligned(delta.x, delta.y, delta.z);
	ray.m_IsSwept = true;
	ray.m_Extents = VectorAligned(0, 0, 0);
	ray.m_IsRay = true;
	ray.m_StartOffset = VectorAligned(0, 0, 0);
	ray.m_Start = VectorAligned(eye.x, eye.y, eye.z);
	ray.m_pWorldAxisTransform = nullptr;
	CGameTrace tr;
	std::memset(&tr, 0, sizeof tr);
	engine.TraceRay(engine.traceServer->ThisPtr(), ray, 0x1 | 0x4000 | 0x2000000 | 0x8 | kMaskWater /* SOLID MOVEABLE MONSTER GRATE WATER */, &f, &tr);
	if (tr.fraction >= 1.0f || tr.startsolid || !tr.m_pEnt) return h;
	if (ServerEntIndex(tr.m_pEnt) == 0) return h;  // the world
	h.d = tr.fraction * maxD, h.kind = kEntity, h.what = EntKV(tr.m_pEnt, "classname") + ":" + EntKV(tr.m_pEnt, "targetname") + "#" + std::to_string(ServerEntIndex(tr.m_pEnt));
	return h;
}

Hit BlockHit(const Vector &eye, const Vector &dir, float maxD) {
	Hit h;
	double s[3], e[3];
	SrcToMcD(eye, s[0], s[1], s[2]);
	SrcToMcD(eye + dir * maxD, e[0], e[1], e[2]);
	const double d[3] = {e[0] - s[0], e[1] - s[1], e[2] - s[2]};
	double t = 0;
	int axis = 0, sign = 0;
	if (TerrainRay(s, d, t, axis, sign)) h.d = (float)(t * maxD), h.kind = kMcBlock, h.what = "block";
	return h;
}

// ---- the probe ---------------------------------------------------------------------------------------------------------------
struct Sample {
	int px, py;
	Vector dir;
	Hit cut, uncut, block, ent;
	int expKind = kNone;
	float expD = 1e30f;
	float actD = -1, depth = 1;
	uint8_t rgb[3] = {0, 0, 0};
	const char *cls = "";
	int near = 0;  // (bridge #76, seam probes) the expected hit lies within one cell of a dug cell (1), the ray aims at a cap point (2)
	int lp = -1, side = 0;  // (bridge #79) a light pair across a mouth's edge: its index, 0 = the host's wall face, 1 = the cap / block beside it
	Vector target;          // (bridge #79) the point the pair's ray was aimed at
};

struct EntRow {
	int index;
	std::string cls, name;
	Vector origin;
	int clientState;  // -1 no client copy, 0 drawn-eligible (not dormant), 1 dormant
};

struct Probe {
	std::string dir, name;
	int gw = 64, gh = 36;
	Vector eye;
	QAngle ang;
	std::vector<EntRow> ents;
	int camContents = 0, camCluster = -2, camArea = -2;
	bool camOutside = false;
	std::vector<Vector> visOrigins;
	int frame = 0;
	double t0 = 0;
	bool seam = false;  // (bridge #76) pc_dh_seam: rays aimed at the cut's seams instead of the grid
};
std::unique_ptr<Probe> g_want, g_inFlight;
std::vector<std::string> g_cmdQueue;  // Minecraft commands, one per frame (pcb_mc_cmd keeps one per frame)
int g_cmdWait = 0;
int g_done = 0;
std::string g_lastSummary;

void ScanEntities(Probe &pr) {
	pr.ents.clear();
	std::unordered_set<void *> live;
	EntLiveSet(live);
	std::unordered_map<int, void *> byIndex;
	for (void *e : live) {
		const int i = ServerEntIndex(e);
		if (i > 0 && i < 2048) byIndex[i] = e;
	}
	int verified = 0, mismatched = 0;
	for (auto &kv : byIndex) {
		EntRow r;
		r.index = kv.first;
		r.cls = EntKV(kv.second, "classname");
		r.name = EntKV(kv.second, "targetname");
		r.origin = EntVec(kv.second, "origin");
		r.clientState = -1;
		void *net = g_entList ? VCall<void *>(g_entList->ThisPtr(), slots::kEntListGetClientNetworkable, kv.first) : nullptr;
		if (net && slots::Use(slots::kNetEntIndex) && slots::Use(slots::kNetIsDormant)) {
			const int idx = VCall<int>(net, slots::kNetEntIndex);
			if (idx == kv.first) {
				++verified;
				r.clientState = VCall<bool>(net, slots::kNetIsDormant) ? 1 : 0;
			} else {
				++mismatched;
			}
		}
		pr.ents.push_back(r);
	}
	if (mismatched && verified == 0) console->Warn("[bridge] digharness: the client networkable's index slot does not match (%d mismatches): dormancy unread\n", mismatched);
}

void BeginProbe(std::unique_ptr<Probe> pr, const CViewSetup *view) {
	pr->eye = view->origin;
	pr->ang = view->angles;
	EnsureInterfaces();
	ScanEntities(*pr);
	void *tr = engine.traceServer->ThisPtr();
	pr->camContents = engine.GetPointContentsWorld(tr, pr->eye, 0xFFFFFFFF);
	pr->camOutside = engine.PointOutsideWorld(tr, pr->eye);
	if (g_engServer) {
		pr->camCluster = VCall<int>(g_engServer->ThisPtr(), slots::kEngClusterForOrigin, (const Vector *)&pr->eye);
		pr->camArea = VCall<int>(g_engServer->ThisPtr(), slots::kEngGetArea, (const Vector *)&pr->eye);
	}
	Vector o[32];
	const int n = VisLastMainOrigins(o, 32);
	pr->visOrigins.assign(o, o + n);
	{
		std::lock_guard<std::mutex> lk(g_capMu);
		g_capDone.reset();
	}
	g_inFlight = std::move(pr);
	RunOnRenderThread(CaptureOnRenderThread, nullptr);
}

void MkDirs(const std::string &p) {
	std::string cur;
	for (size_t i = 0; i < p.size(); ++i) {
		cur += p[i];
		if (p[i] == '/' && cur.size() > 1) mkdir(cur.c_str(), 0755);
	}
	mkdir(p.c_str(), 0755);
}

std::string Esc(const std::string &s) {
	std::string o;
	for (char c : s) {
		if (c == '"' || c == '\\') o += '\\';
		if ((unsigned char)c >= 32) o += c;
	}
	return o;
}

void FinishProbe(Probe &pr, const Capture &cap) {
	const std::string dir = CacheDir() + "/digharness/" + pr.dir;
	MkDirs(dir);
	const std::string base = dir + "/" + pr.name;
	FILE *f = std::fopen((base + ".json").c_str(), "w");
	if (!f) return console->Warn("[bridge] digharness: cannot write %s.json\n", base.c_str());
	M4 clip, inv;
	std::string how;
	const bool okM = cap.err.empty() && CalibrateClip(cap.view, cap.proj, pr.eye, pr.ang, clip, how) && Invert(clip, inv);
	std::vector<Sample> S;
	int rowFlipVotes[2] = {0, 0};
	double fitA = 0, fitB = 0;
	int fitN = 0;
	void *player = nullptr;
	{
		auto pl = EntFind("player");
		if (!pl.empty()) player = pl[0];
	}
	const float maxD = 8000.0f;
	// (bridge #75) the eye the frame was really drawn from: the point the clip transform sends to x = y = w = 0. The capture runs on the
	// render thread, which can be a frame behind the main view this probe started on: a moving or settling camera then put the rays a
	// few units off and every distance 1-3 % wrong (sp_a1_intro2: whole walls classed "missing"). Rays and distances start here.
	Vector E = pr.eye;
	float eyeDelta = -1;
	if (okM) {
		const double z[4] = {0, 0, 1, 0};
		double w[4];
		Apply(inv, z, w);
		if (std::fabs(w[3]) > 1e-12) {
			const Vector m((float)(w[0] / w[3]), (float)(w[1] / w[3]), (float)(w[2] / w[3]));
			eyeDelta = (m - pr.eye).Length();
			if (eyeDelta < 128.0f) E = m;
		}
	}
	int seamTargets = 0, seamFaces = 0;
	std::vector<std::pair<Vector, Vector>> lightTargets;  // (bridge #79) (wall point, cap point) per edge point
	if (okM) {
		const int W = cap.W, H = cap.H;
		std::vector<std::array<int, 3>> pix;  // (px, py, aimed at a cap point)
		if (!pr.seam) {
			for (int j = 0; j < pr.gh; ++j)
				for (int i = 0; i < pr.gw; ++i) pix.push_back({(int)((i + 0.5) * W / pr.gw), (int)((j + 0.5) * H / pr.gh), 0});
		} else {
			// (bridge #76) the seams: every face between a dug cell and a neighbour cell that is part open air and part Portal 2 solid (the
			// ring of cells the host's wall face runs through: the mouth and the tunnel's walls within one cell of the host face). Points on
			// that face with host solid right behind them are where a cap must be drawn; each is projected and the pixels around it
			// (+-6 px, 3 px apart) are sampled, so the wall face beside the mouth's edge is sampled too.
			std::vector<std::pair<Vector, Vector>> boxes;
			DigAllBoxes(boxes);
			std::set<std::pair<int, int>> seen;
			auto project = [&](const Vector &p, int &px, int &py) {
				const double v[4] = {p.x, p.y, p.z, 1.0};
				double c[4];
				Apply(clip, v, c);
				if (c[3] <= 1.0) return false;
				const double xn = c[0] / c[3], yn = c[1] / c[3];
				if (std::fabs(xn) >= 1.0 || std::fabs(yn) >= 1.0) return false;
				px = (int)((xn + 1.0) * 0.5 * W), py = (int)((1.0 - yn) * 0.5 * H);
				return true;
			};
			for (const auto &bx : boxes) {
				const Vector cen = (bx.first + bx.second) * 0.5f;
				if ((cen - E).Length() > 900.0f) continue;
				const Vector mc = SrcToMc(cen);
				const int D[3] = {(int)std::floor(mc.x), (int)std::floor(mc.y), (int)std::floor(mc.z)};
				static const int kStep[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
				for (int dir = 0; dir < 6; ++dir) {
					const int N[3] = {D[0] + kStep[dir][0], D[1] + kStep[dir][1], D[2] + kStep[dir][2]};
					if (DigIsDugMc(N[0], N[1], N[2])) continue;
					int open = 0, solid = 0;
					for (int a = 0; a < 3; ++a)
						for (int b = 0; b < 3; ++b)
							for (int c = 0; c < 3; ++c) {
								const Vector q = McToSrc(N[0] + (a + 0.5) / 3.0, N[1] + (b + 0.5) / 3.0, N[2] + (c + 0.5) / 3.0);
								if (SolidAt(q)) ++solid;
								else ++open;
							}
					if (!open || !solid) continue;  // wholly open (nothing to cap) or wholly solid (a revealed block's place, deep in)
					++seamFaces;
					const Vector into = (McToSrc(N[0] + 0.5, N[1] + 0.5, N[2] + 0.5) - McToSrc(D[0] + 0.5, D[1] + 0.5, D[2] + 0.5)) * (1.0f / kUnitsPerBlock);
					const int axis = dir >> 1 == 0 ? 1 : dir >> 1 == 1 ? 2 : 0, a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
					const double plane = D[axis] + (kStep[dir][axis] > 0 ? 1.0 : 0.0);
					// a 1 u lattice over the face: "solid behind" per point; the points on the edge of the solid part (where a cap meets
					// Portal 2's cut face: the seam itself, within 2 u) and a coarse 5 u lattice of the rest
					const int G = 40;
					std::vector<char> sb((size_t)G * G);
					auto at = [&](int i, int j) {
						double m[3];
						m[axis] = plane, m[a1] = D[a1] + (i + 0.5) / G, m[a2] = D[a2] + (j + 0.5) / G;
						return McToSrc(m[0], m[1], m[2]);
					};
					if (Vector::DotProduct(E - at(G / 2, G / 2), into) > -0.5f) continue;  // the eye must be on D's side of the face
					for (int i = 0; i < G; ++i)
						for (int j = 0; j < G; ++j) sb[(size_t)i * G + j] = SolidAt(at(i, j) + into * 0.3f);
					for (int i = 0; i < G; ++i)
						for (int j = 0; j < G; ++j) {
							if (!sb[(size_t)i * G + j]) continue;
							bool edge = false;
							for (int k = -2; k <= 2 && !edge; k += 4)
								edge = (i + k >= 0 && i + k < G && !sb[(size_t)(i + k) * G + j]) || (j + k >= 0 && j + k < G && !sb[(size_t)i * G + j + k]);
							if (!edge && (i % 5 != 2 || j % 5 != 2)) continue;
							int px, py;
							if (!project(at(i, j), px, py)) continue;
							++seamTargets;
							const int r = edge ? 4 : 0;
							for (int dy = -r; dy <= r; dy += 2)
								for (int dx = -r; dx <= r; dx += 2) {
									const int x = px + dx, y = py + dy;
									if (x < 0 || y < 0 || x >= W || y >= H || !seen.insert({x, y}).second) continue;
									pix.push_back({x, y, dx == 0 && dy == 0 ? 1 : 0});
								}
						}
				}
			}
			// (bridge #79) LIGHT PAIRS across the mouth's edge: on every face between a dug cell and a cell that is not dug, the lattice
			// (extended a quarter block past the face, so a wall face lying on the cell boundary is found too) has points with Portal 2's
			// solid behind them next to points without: the edge where a cap (or a revealed block's face) meets Portal 2's cut wall
			// face. Each edge point gives two rays: one at the wall face 2 u out of the edge and 4 u along it into the neighbour cell,
			// one at the cap 4 u inside the edge. Their colours give the luminance step across the mouth (judged when both rays hit
			// what they aimed at).
			for (const auto &bx : boxes) {
				const Vector cen = (bx.first + bx.second) * 0.5f;
				if ((cen - E).Length() > 700.0f || lightTargets.size() > 1600) continue;
				const Vector mc = SrcToMc(cen);
				const int D[3] = {(int)std::floor(mc.x), (int)std::floor(mc.y), (int)std::floor(mc.z)};
				static const int kStep[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
				for (int dir = 0; dir < 6; ++dir) {
					const int N[3] = {D[0] + kStep[dir][0], D[1] + kStep[dir][1], D[2] + kStep[dir][2]};
					if (DigIsDugMc(N[0], N[1], N[2])) continue;
					const Vector into = (McToSrc(N[0] + 0.5, N[1] + 0.5, N[2] + 0.5) - McToSrc(D[0] + 0.5, D[1] + 0.5, D[2] + 0.5)) * (1.0f / kUnitsPerBlock);
					const int axis = dir >> 1 == 0 ? 1 : dir >> 1 == 1 ? 2 : 0, a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
					const double plane = D[axis] + (kStep[dir][axis] > 0 ? 1.0 : 0.0);
					const int G = 20, X = 5;  // 2 u lattice, 10 u past the face each way
					auto at = [&](int i, int j) {
						double m[3];
						m[axis] = plane, m[a1] = D[a1] + (i + 0.5) / G, m[a2] = D[a2] + (j + 0.5) / G;
						return McToSrc(m[0], m[1], m[2]);
					};
					if (Vector::DotProduct(E - at(G / 2, G / 2), into) > -0.5f) continue;  // the eye on D's side
					const int S2 = G + 2 * X;
					std::vector<char> sb((size_t)S2 * S2);
					for (int i = -X; i < G + X; ++i)
						for (int j = -X; j < G + X; ++j) sb[(size_t)(i + X) * S2 + (j + X)] = SolidAt(at(i, j) + into * 0.3f);
					auto solid = [&](int i, int j) { return i >= -X && i < G + X && j >= -X && j < G + X && sb[(size_t)(i + X) * S2 + (j + X)]; };
					for (int i = 0; i < G; ++i)
						for (int j = 0; j < G; ++j) {
							if (!solid(i, j) || (i + j) % 2) continue;
							static const int kG[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
							for (const auto &g : kG) {
								if (solid(i + g[0], j + g[1]) || !solid(i - 2 * g[0], j - 2 * g[1])) continue;  // an edge with 4 u of solid behind it
								const Vector P = at(i, j);
								const Vector gS = (at(i + g[0], j + g[1]) - P) * (1.0f / std::max(1e-3f, (at(i + g[0], j + g[1]) - P).Length()));
								if (Vector::DotProduct(E - P, gS) < 2.0f) continue;  // the wall face must face the eye
								const Vector W = P + gS * 2.0f + into * 4.0f, C = P - gS * 4.0f;
								int pw[2], pc[2];
								if (!project(W, pw[0], pw[1]) || !project(C, pc[0], pc[1])) continue;
								lightTargets.push_back({W, C});
								break;
							}
						}
				}
			}
			if (lightTargets.size() > 1600) {
				std::vector<std::pair<Vector, Vector>> keep;
				const size_t st = (lightTargets.size() + 1599) / 1600;
				for (size_t k = 0; k < lightTargets.size(); k += st) keep.push_back(lightTargets[k]);
				lightTargets.swap(keep);
			}
			if (pix.size() > 30000) {  // (bounded: a stride over the set)
				std::vector<std::array<int, 3>> keep;
				const size_t st = (pix.size() + 29999) / 30000;
				for (size_t k = 0; k < pix.size(); k += st) keep.push_back(pix[k]);
				pix.swap(keep);
			}
		}
		for (const auto &px3 : pix) {
				Sample s;
				s.px = px3[0], s.py = px3[1];
				if (px3[2]) s.near = 2;
				const double xn = 2.0 * (s.px + 0.5) / W - 1.0, yn = 1.0 - 2.0 * (s.py + 0.5) / H;
				double a[4] = {xn, yn, 0.0, 1.0}, b[4] = {xn, yn, 1.0, 1.0}, wa[4], wb[4];
				Apply(inv, a, wa), Apply(inv, b, wb);
				const Vector pa((float)(wa[0] / wa[3]), (float)(wa[1] / wa[3]), (float)(wa[2] / wa[3])), pb((float)(wb[0] / wb[3]), (float)(wb[1] / wb[3]), (float)(wb[2] / wb[3]));
				Vector d = pb - pa;
				d = d * (1.0f / std::max(d.Length(), 1e-6f));
				s.dir = d;
				s.cut = HostWalk(E, d, maxD, true);
				s.uncut = HostWalk(E, d, maxD, false);
				s.block = BlockHit(E, d, std::min(maxD, s.cut.d + 1.0f));
				s.ent = EntityHit(E, d, std::min(maxD, std::min(s.cut.d, s.block.d) + 1.0f), player);
				const Hit *best = &s.cut;
				if (s.block.d < best->d) best = &s.block;
				if (s.ent.d < best->d) best = &s.ent;
				s.expKind = best->kind, s.expD = best->d;
				if (pr.seam && !s.near && best->d < 1e29f) {  // within one cell of a dug cell
					const Vector hp = E + d * best->d;
					for (int k = 0; k < 27 && !s.near; ++k)
						if (DigIsDugSrc(hp + Vector((k % 3 - 1) * 30.0f, (k / 3 % 3 - 1) * 30.0f, (k / 9 - 1) * 30.0f))) s.near = 1;
				}
				S.push_back(s);
			}
			// (bridge #79) the light pairs: a ray at each target's pixel (the pixel centre nearest the projected point)
			for (size_t k = 0; k < lightTargets.size(); ++k)
				for (int side = 0; side < 2; ++side) {
					const Vector T = side ? lightTargets[k].second : lightTargets[k].first;
					const double v4[4] = {T.x, T.y, T.z, 1.0};
					double c4[4];
					Apply(clip, v4, c4);
					if (c4[3] <= 1.0) continue;
					Sample s;
					s.px = std::min(W - 1, std::max(0, (int)((c4[0] / c4[3] + 1.0) * 0.5 * W))), s.py = std::min(H - 1, std::max(0, (int)((1.0 - c4[1] / c4[3]) * 0.5 * H)));
					s.lp = (int)k, s.side = side, s.target = T;
					const double xn = 2.0 * (s.px + 0.5) / W - 1.0, yn = 1.0 - 2.0 * (s.py + 0.5) / H;
					double a[4] = {xn, yn, 0.0, 1.0}, b[4] = {xn, yn, 1.0, 1.0}, wa[4], wb[4];
					Apply(inv, a, wa), Apply(inv, b, wb);
					const Vector pa((float)(wa[0] / wa[3]), (float)(wa[1] / wa[3]), (float)(wa[2] / wa[3])), pb((float)(wb[0] / wb[3]), (float)(wb[1] / wb[3]), (float)(wb[2] / wb[3]));
					Vector d = pb - pa;
					d = d * (1.0f / std::max(d.Length(), 1e-6f));
					s.dir = d;
					s.cut = HostWalk(E, d, maxD, true);
					s.uncut.d = 1e30f;
					s.block = BlockHit(E, d, std::min(maxD, s.cut.d + 1.0f));
					s.ent = EntityHit(E, d, std::min(maxD, std::min(s.cut.d, s.block.d) + 1.0f), player);
					const Hit *best = &s.cut;
					if (s.block.d < best->d) best = &s.block;
					if (s.ent.d < best->d) best = &s.ent;
					s.expKind = best->kind, s.expD = best->d;
					S.push_back(s);
				}
		// GL rows run bottom-up in the framebuffer; togl draws D3D's picture with y flipped, so D3D row r is GL row r. Both readings are
		// tried against the geometry and the one that agrees more is used (logged).
		auto actualAt = [&](const Sample &s, bool flip, float &depth, float &dist) {
			const int row = flip ? (H - 1 - s.py) : s.py;
			depth = cap.depth[(size_t)row * W + s.px];
			const double xn = 2.0 * (s.px + 0.5) / W - 1.0, yn = 1.0 - 2.0 * (s.py + 0.5) / H;
			double v[4] = {xn, yn, depth, 1.0}, w[4];
			Apply(inv, v, w);
			const Vector p((float)(w[0] / w[3]), (float)(w[1] / w[3]), (float)(w[2] / w[3]));
			dist = (p - E).Length();
		};
		for (int flip = 0; flip < 2; ++flip)
			for (const Sample &s : S) {
				if (s.expKind == kNone || s.expD > 3000) continue;
				float dep, dist;
				actualAt(s, flip != 0, dep, dist);
				if (std::fabs(dist - s.expD) < std::max(2.0f, 0.01f * s.expD)) ++rowFlipVotes[flip];
			}
		const bool flip = rowFlipVotes[1] > rowFlipVotes[0];
		for (Sample &s : S) actualAt(s, flip, s.depth, s.actD);
		// The depth -> distance mapping, checked against the geometry: the depth buffer did not follow the reported projection to
		// the last digit in the first runs (2-3 % at 1000 u). Window depth = a + b / viewDepth for any perspective projection: a and b
		// are fitted on the samples that agree within 15 %, then every sample is mapped with them (logged as depthFit).
		{
			const float pp = pr.ang.x * (float)M_PI / 180.0f, yy = pr.ang.y * (float)M_PI / 180.0f;
			const Vector fwd(std::cos(pp) * std::cos(yy), std::cos(pp) * std::sin(yy), -std::sin(pp));
			// (bridge #75) fitted in rounds with a tightening gate (15 %, 3 %, 1 %, 0.5 % of the distance the last fit gives): one round at
			// 15 % let props and decals in front of the expected wall pull the fit (sp_a1_intro2: a = 0.99943, every distance 2-3 % long)
			auto mapped = [&](const Sample &s, double A, double B) -> double {
				const double c = Vector::DotProduct(s.dir, fwd), den2 = s.depth - A;
				if (s.depth >= 0.99985f || c < 1e-3 || std::fabs(den2) < 1e-12) return -1;
				const double vd = B / den2;
				return vd > 0 ? vd / c : -1;
			};
			const double gates[4] = {0.15, 0.03, 0.01, 0.005};
			bool have = false;
			for (int round = 0; round < 4; ++round) {
				double sx = 0, sy = 0, sxx = 0, sxy = 0;
				int cnt = 0;
				for (const Sample &s : S) {
					if ((s.expKind != kHost && s.expKind != kMcBlock) || s.expD > 4000 || s.depth >= 0.99985f) continue;
					const double d = have ? mapped(s, fitA, fitB) : (double)s.actD;
					if (d < 0 || std::fabs(d - s.expD) > gates[round] * s.expD) continue;
					const double vd = s.expD * Vector::DotProduct(s.dir, fwd);
					if (vd < 1) continue;
					const double x = 1.0 / vd, y = s.depth;
					sx += x, sy += y, sxx += x * x, sxy += x * y, ++cnt;
				}
				if (cnt < 30) break;
				const double den = cnt * sxx - sx * sx;
				if (std::fabs(den) <= 1e-18) break;
				fitB = (cnt * sxy - sx * sy) / den, fitA = (sy - fitB * sx) / cnt, fitN = cnt, have = true;
			}
			// keep whichever mapping agrees with more of the geometry: the projection matrix's (actD as read) or the fit. A frame of props
			// and one wall at one distance makes the two-parameter fit ill-conditioned (sp_a1_intro2 s35_d200: b -8.1 for a near plane of 7)
			if (have) {
				int agreeM = 0, agreeF = 0;
				for (const Sample &s : S) {
					if ((s.expKind != kHost && s.expKind != kMcBlock) || s.expD > 4000) continue;
					const double tol = std::max(1.5, 0.006 * s.expD), d = mapped(s, fitA, fitB);
					if (s.depth < 0.99985f && std::fabs(s.actD - s.expD) < tol) ++agreeM;
					if (d > 0 && std::fabs(d - s.expD) < tol) ++agreeF;
				}
				if (agreeF > agreeM)  // b is negative for a + b / vd increasing with depth: depth = a + b / vd -> vd = b / (depth - a)
					for (Sample &s : S) {
						const double d = mapped(s, fitA, fitB);
						if (d > 0) s.actD = (float)d;
					}
				else
					fitN = -agreeM;  // (logged as depthFit [a, b, -n]: the matrix's mapping was kept, n samples agreeing)
			}
		}
		for (Sample &s : S) {
			const int row = flip ? (H - 1 - s.py) : s.py;
			const uint8_t *c = &cap.rgba[((size_t)row * W + s.px) * 4];
			s.rgb[0] = c[0], s.rgb[1] = c[1], s.rgb[2] = c[2];
		}
	}
	// (bridge #79) THE EDGE TEST. A seam sample that fails right on a silhouette edge (the mouth's edge line seen edge-on from inside the
	// cut: seam_inside_out2/3 of #76) is the depth resolve taking one MSAA sample of a pixel the edge crosses. Its expected hit is
	// re-traced 1.5 px to each side: if the expected surface jumps there and the drawn depth matches the surface on the other side,
	// the sample is classed "edge" (aliasing), not missing.
	int nEdge = 0;
	if (okM && pr.seam) {
		const int W = cap.W, H = cap.H;
		for (Sample &s : S) {
			if (!s.near || s.lp >= 0 || s.expKind == kNone || s.expKind == kEntity) continue;
			const float tol = std::max(1.5f, 0.006f * std::min(s.expD, 1e5f));
			const bool far = s.depth >= 0.99985f;
			if (!far && std::fabs(s.actD - s.expD) < tol) continue;  // ok
			if (!far && s.actD < s.expD - tol) continue;               // something nearer: not an edge question
			static const float kOff[4][2] = {{1.5f, 0}, {-1.5f, 0}, {0, 1.5f}, {0, -1.5f}};
			for (const auto &o : kOff) {
				const double xn = 2.0 * (s.px + 0.5 + o[0]) / W - 1.0, yn = 1.0 - 2.0 * (s.py + 0.5 + o[1]) / H;
				double a[4] = {xn, yn, 0.0, 1.0}, b[4] = {xn, yn, 1.0, 1.0}, wa[4], wb[4];
				Apply(inv, a, wa), Apply(inv, b, wb);
				const Vector pa((float)(wa[0] / wa[3]), (float)(wa[1] / wa[3]), (float)(wa[2] / wa[3])), pb((float)(wb[0] / wb[3]), (float)(wb[1] / wb[3]), (float)(wb[2] / wb[3]));
				Vector d = pb - pa;
				d = d * (1.0f / std::max(d.Length(), 1e-6f));
				const Hit c = HostWalk(E, d, maxD, true);
				const Hit b2 = BlockHit(E, d, std::min(maxD, c.d + 1.0f));
				const float e2 = std::min(c.d, b2.d);
				if (std::fabs(e2 - s.expD) < tol) continue;  // no jump that way
				const float tol2 = std::max(1.5f, 0.006f * std::min(e2, 1e5f));
				if ((far && e2 > 1e29f) || (!far && std::fabs(s.actD - e2) < std::max(tol2, std::fabs(e2 - s.expD) * 0.5f))) {
					s.cls = "edge";
					++nEdge;
					break;
				}
			}
		}
	}
	// classification
	int n = 0, nOk = 0, nGhost = 0, nVoid = 0, nNear = 0, nMissing = 0, nSil = 0, silGhost = 0, silVoid = 0, silOk = 0, nExpEnt = 0, nWaterExp = 0, nWaterOk = 0;
	int seamN = 0, seamOk = 0, seamVoid = 0, seamMissing = 0, seamNear = 0, seamAim = 0, seamAimBad = 0;  // (bridge #76)
	for (Sample &s : S) {
		++n;
		// (bridge #75) a water surface is drawn up to ~1 u below its brush's top (the goo of sp_a2_laser_over_goo): at a grazing angle that
		// is several units along the ray, so water gets a wider tolerance
		const float tol = s.expKind == kWater ? std::max(4.0f, 0.015f * std::min(s.expD, 1e5f)) : std::max(1.5f, 0.006f * std::min(s.expD, 1e5f));
		const bool far = s.depth >= 0.99985f;  // the far plane, or the old punch's paint (drawn at 0.9999)
		const bool sil = s.uncut.kind == kHost && s.uncut.d < s.cut.d - tol && s.uncut.d < std::min(s.block.d, s.ent.d) - tol;  // the old wall lies in front of what the cut shows
		if (sil) ++nSil;
		if (s.expKind == kEntity) ++nExpEnt;
		if (s.expKind == kWater) ++nWaterExp;
		const bool wasEdge = std::string(s.cls) == "edge";
		if (s.expKind == kNone) s.cls = far ? "ok_none" : "extra";
		else if (wasEdge) s.cls = "edge";
		else if (far) s.cls = "void";
		else if (std::fabs(s.actD - s.expD) < tol) s.cls = "ok";
		else if (sil && std::fabs(s.actD - s.uncut.d) < tol) s.cls = "ghost_wall";
		else if (s.actD < s.expD - tol) s.cls = "near_other";
		else s.cls = "missing";
		if (s.lp >= 0) continue;  // (bridge #79) light pairs: judged below, not part of the depth summary
		const std::string c = s.cls;
		if (c == "ok" || c == "ok_none") ++nOk;
		else if (c == "edge") ++nOk;
		else if (c == "ghost_wall") ++nGhost;
		else if (c == "void") ++nVoid;
		else if (c == "near_other" || c == "extra") ++nNear;
		else ++nMissing;
		if (sil) {
			if (c == "ok") ++silOk;
			else if (c == "ghost_wall") ++silGhost;
			else if (c == "void") ++silVoid;
		}
		if (s.expKind == kWater && c == "ok") ++nWaterOk;
		if (s.near && s.expKind != kNone && s.expKind != kEntity) {  // (an entity's collision hull is not its drawn model: not judged here)
			++seamN;
			if (c == "ok" || c == "edge") ++seamOk;
			else if (c == "void") ++seamVoid;
			else if (c == "missing") ++seamMissing;
			else if (c == "near_other") ++seamNear;
			if (s.near == 2) ++seamAim, seamAimBad += c == "void" || c == "missing";
		}
	}
	int entDormant = 0, entLive = 0;
	for (const EntRow &e : pr.ents) entDormant += e.clientState == 1, entLive += e.clientState == 0;
	// (bridge #79) luminance across the mouth's edge: pairs whose two rays both hit what they aimed at (the wall: Portal 2's surface within
	// 6 u of the wall point; the cap: a Minecraft surface within 6 u of the cap point), linear luminance of the captured sRGB colour
	auto lum = [](const uint8_t *c) {
		auto lin = [](float v) { v /= 255.0f; return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
		return 0.2126f * lin(c[0]) + 0.7152f * lin(c[1]) + 0.0722f * lin(c[2]);
	};
	int lightPairs = 0, lightAimed = (int)lightTargets.size(), lightDark = 0, lightSat = 0, lightTooDark = 0;
	double lumWall = 0, lumCap = 0;
	std::vector<double> ratios;
	{
		std::vector<const Sample *> wall(lightTargets.size(), nullptr), capS(lightTargets.size(), nullptr);
		for (const Sample &s : S) {
			if (s.lp < 0 || std::string(s.cls) != "ok") continue;
			const Vector hp = E + s.dir * s.expD;
			if ((hp - s.target).Length() > 6.0f) continue;
			if (s.side == 0 && s.expKind == kHost) wall[s.lp] = &s;
			if (s.side == 1 && (s.expKind == kMcBlock || s.expKind == kMcCover)) capS[s.lp] = &s;
		}
		for (size_t k = 0; k < lightTargets.size(); ++k) {
			if (!wall[k] || !capS[k]) continue;
			const float lw = lum(wall[k]->rgb), lc = lum(capS[k]->rgb);
			// judged only where the wall's pixel says something: not saturated (an emissive sign, a light strip) and not near black
			// (sRGB under ~14/255, where a step of one code value is a third of the signal)
			if (std::max({wall[k]->rgb[0], wall[k]->rgb[1], wall[k]->rgb[2]}) >= 250) {
				++lightSat;
				continue;
			}
			if (lw < 0.004f) {
				++lightTooDark;
				continue;
			}
			++lightPairs, lumWall += lw, lumCap += lc;
			ratios.push_back((lc + 0.002) / (lw + 0.002));
			if (lc < 0.25f * lw) ++lightDark;
		}
	}
	std::sort(ratios.begin(), ratios.end());
	const double ratioMed = ratios.empty() ? -1 : ratios[ratios.size() / 2];
	const double ratioMean = lightPairs && lumWall > 0 ? lumCap / lumWall : -1;
	if (lightPairs) lumWall /= lightPairs, lumCap /= lightPairs;
	// and the cut's Minecraft surfaces in the whole probe against the host's (every ok sample near the cut)
	double tunLum = 0, hostLum = 0;
	int tunN = 0, hostN = 0;
	for (const Sample &s : S) {
		if (s.lp >= 0 || !s.near || std::string(s.cls) != "ok") continue;
		if (s.expKind == kMcBlock || s.expKind == kMcCover) tunLum += lum(s.rgb), ++tunN;
		else if (s.expKind == kHost) hostLum += lum(s.rgb), ++hostN;
	}
	if (tunN) tunLum /= tunN;
	if (hostN) hostLum /= hostN;
	// JSON
	std::fprintf(f, "{\n \"probe\": \"%s\", \"map\": \"%s\", \"mode\": \"%s\", \"frame\": %d,\n", Esc(pr.name).c_str(), Esc(McMapName()).c_str(), McModeActive() ? "mc" : "chell", pr.frame);
	std::fprintf(f, " \"eye\": [%.2f, %.2f, %.2f], \"angles\": [%.3f, %.3f, %.3f], \"drawEye\": [%.2f, %.2f, %.2f], \"eyeDelta\": %.2f,\n", pr.eye.x, pr.eye.y, pr.eye.z, pr.ang.x, pr.ang.y, pr.ang.z, E.x, E.y, E.z, eyeDelta);
	std::fprintf(f, " \"capture\": {\"W\": %d, \"H\": %d, \"vx\": %d, \"vy\": %d, \"fbo\": %d, \"samples\": %d, \"resolved\": %s, \"err\": \"%s\", \"matrix\": \"%s\", \"rowVotes\": [%d, %d], \"depthFit\": [%.9g, %.9g, %d]},\n", cap.W, cap.H, cap.vx,
	             cap.vy, cap.fbo, cap.samples, cap.resolved ? "true" : "false", Esc(cap.err).c_str(), Esc(how).c_str(), rowFlipVotes[0], rowFlipVotes[1], fitA, fitB, fitN);
	std::fprintf(f, " \"clip\": [");
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j) std::fprintf(f, "%s%.9g", i + j ? ", " : "", okM ? clip.m[i][j] : 0.0);
	std::fprintf(f, "],\n");
	std::fprintf(f, " \"camera\": {\"contents\": %d, \"cluster\": %d, \"area\": %d, \"outside\": %s, \"dug\": %s, \"visOrigins\": [", pr.camContents, pr.camCluster, pr.camArea, pr.camOutside ? "true" : "false",
	             DigIsDugSrc(pr.eye) ? "true" : "false");
	for (size_t i = 0; i < pr.visOrigins.size(); ++i) std::fprintf(f, "%s[%.1f, %.1f, %.1f]", i ? ", " : "", pr.visOrigins[i].x, pr.visOrigins[i].y, pr.visOrigins[i].z);
	std::fprintf(f, "]},\n");
	{
		std::vector<std::pair<Vector, Vector>> boxes;
		DigAllBoxes(boxes);
		std::fprintf(f, " \"dug\": %zu,\n", boxes.size());
	}
	std::fprintf(f, " \"summary\": {\"n\": %d, \"ok\": %d, \"ghost_wall\": %d, \"void\": %d, \"near_other\": %d, \"missing\": %d, \"silhouette\": %d, \"sil_ok\": %d, \"sil_ghost\": %d, \"sil_void\": %d, "
	             "\"exp_entity\": %d, \"exp_water\": %d, \"water_ok\": %d, \"ents_live\": %d, \"ents_dormant\": %d},\n",
	             n, nOk, nGhost, nVoid, nNear, nMissing, nSil, silOk, silGhost, silVoid, nExpEnt, nWaterExp, nWaterOk, entLive, entDormant);
	std::fprintf(f, " \"seam\": {\"on\": %s, \"faces\": %d, \"targets\": %d, \"n\": %d, \"ok\": %d, \"void\": %d, \"missing\": %d, \"near_other\": %d, \"aimed\": %d, \"aimed_bad\": %d},\n", pr.seam ? "true" : "false",
	             seamFaces, seamTargets, seamN, seamOk, seamVoid, seamMissing, seamNear, seamAim, seamAimBad);
	std::fprintf(f, " \"edge\": %d,\n", nEdge);
	std::fprintf(f, " \"light\": {\"aimed\": %d, \"pairs\": %d, \"lum_wall\": %.5f, \"lum_cap\": %.5f, \"ratio_mean\": %.4f, \"ratio_median\": %.4f, \"dark_pairs\": %d, \"tunnel_lum\": %.5f, \"tunnel_n\": %d, \"host_lum\": %.5f, \"host_n\": %d, \"saturated\": %d, \"too_dark\": %d},\n",
	             lightAimed, lightPairs, lumWall, lumCap, ratioMean, ratioMed, lightDark, tunLum, tunN, hostLum, hostN, lightSat, lightTooDark);
	std::fprintf(f, " \"grid\": [%d, %d],\n \"samples\": [\n", pr.gw, pr.gh);
	for (size_t k = 0; k < S.size(); ++k) {
		const Sample &s = S[k];
		auto dd = [](float d) { return d > 1e29f ? -1.0f : d; };
		if (s.lp >= 0) {  // (bridge #79) a light-pair ray
			std::fprintf(f, "  {\"p\": [%d, %d], \"lp\": %d, \"side\": %d, \"t\": [%.1f, %.1f, %.1f], \"exp\": \"%s\", \"expD\": %.2f, \"act\": %.2f, \"rgb\": [%d, %d, %d], \"cls\": \"%s\", \"near\": 0}%s\n", s.px, s.py, s.lp, s.side,
			             s.target.x, s.target.y, s.target.z, kKindNames[s.expKind], dd(s.expD), s.actD, s.rgb[0], s.rgb[1], s.rgb[2], s.cls, k + 1 < S.size() ? "," : "");
			continue;
		}
		std::fprintf(f, "  {\"p\": [%d, %d], \"exp\": \"%s\", \"expD\": %.2f, \"cut\": [\"%s\", %.2f], \"uncut\": [\"%s\", %.2f], \"blk\": %.2f, \"ent\": [%.2f, \"%s\"], \"act\": %.2f, \"z\": %.7f, \"rgb\": [%d, %d, %d], \"cls\": \"%s\", \"surf\": \"%s\", \"near\": %d}%s\n",
		             s.px, s.py, kKindNames[s.expKind], dd(s.expD), kKindNames[s.cut.kind], dd(s.cut.d), kKindNames[s.uncut.kind], dd(s.uncut.d), dd(s.block.d), dd(s.ent.d), Esc(s.ent.what).c_str(), s.actD, s.depth,
		             s.rgb[0], s.rgb[1], s.rgb[2], s.cls, Esc(s.cut.what).c_str(), s.near, k + 1 < S.size() ? "," : "");
	}
	std::fprintf(f, " ],\n \"entities\": [\n");
	for (size_t k = 0; k < pr.ents.size(); ++k) {
		const EntRow &e = pr.ents[k];
		std::fprintf(f, "  {\"i\": %d, \"cls\": \"%s\", \"name\": \"%s\", \"o\": [%.1f, %.1f, %.1f], \"client\": %d}%s\n", e.index, Esc(e.cls).c_str(), Esc(e.name).c_str(), e.origin.x, e.origin.y, e.origin.z, e.clientState,
		             k + 1 < pr.ents.size() ? "," : "");
	}
	std::fprintf(f, " ]\n}\n");
	std::fclose(f);
	// the captured colour as a PNG (evidence only), top row first
	if (!cap.rgba.empty() && cap.W > 0) {
		const bool flip = rowFlipVotes[1] > rowFlipVotes[0];
		std::vector<uint8_t> img((size_t)cap.W * cap.H * 4);
		for (int r = 0; r < cap.H; ++r) {
			const int src = flip ? (cap.H - 1 - r) : r;
			std::memcpy(&img[(size_t)r * cap.W * 4], &cap.rgba[(size_t)src * cap.W * 4], (size_t)cap.W * 4);
		}
		for (size_t i = 3; i < img.size(); i += 4) img[i] = 255;
		lodepng::encode(base + ".png", img, cap.W, cap.H);
	}
	char line[600];
	std::snprintf(line, sizeof line,
	              "digharness %s/%s: %d samples, ok %d, ghost_wall %d, void %d, near_other %d, missing %d; cut silhouette %d (ok %d, ghost %d, void %d); entities live %d dormant %d; camera contents 0x%x cluster %d area %d%s; %s%s",
	              pr.dir.c_str(), pr.name.c_str(), n, nOk, nGhost, nVoid, nNear, nMissing, nSil, silOk, silGhost, silVoid, entLive, entDormant, pr.camContents, pr.camCluster, pr.camArea, DigIsDugSrc(pr.eye) ? " (in a dug cell)" : "",
	              okM ? how.c_str() : "NO MATRICES", cap.err.empty() ? "" : (" capture error: " + cap.err).c_str());
	g_lastSummary = line;
	console->Print("[bridge] %s\n", line);
	if (pr.seam)
		console->Print("[bridge] digharness %s/%s SEAM: %d seam faces, %d cap points in view; %d samples near the cut: ok %d (%d on an edge), VOID %d, MISSING %d, near_other %d; aimed at a cap point %d, of them void/missing %d\n", pr.dir.c_str(),
		               pr.name.c_str(), seamFaces, seamTargets, seamN, seamOk, nEdge, seamVoid, seamMissing, seamNear, seamAim, seamAimBad);
	if (pr.seam)
		console->Print("[bridge] digharness %s/%s LIGHT: %d of %d edge pairs seen; luminance wall %.4f cap %.4f, ratio %.3f (median %.3f), %d pairs under a quarter; tunnel %.4f (%d) vs host %.4f (%d); not judged: %d saturated walls, %d too dark\n", pr.dir.c_str(), pr.name.c_str(), lightPairs,
		               lightAimed, lumWall, lumCap, ratioMean, ratioMed, lightDark, tunLum, tunN, hostLum, hostN, lightSat, lightTooDark);
}

}  // namespace

// render.cpp, after the main view's translucent pass (main thread): start a wanted probe, finish a captured one.
void DigHarnessMainView(const CViewSetup *view) {
	if (!g_cmdQueue.empty() && --g_cmdWait <= 0) {
		engine.Cmd(g_cmdQueue.front().c_str());
		g_cmdQueue.erase(g_cmdQueue.begin());
		g_cmdWait = 3;
	}
	if (g_inFlight) {
		std::shared_ptr<Capture> cap;
		{
			std::lock_guard<std::mutex> lk(g_capMu);
			cap = g_capDone;
		}
		if (cap) {
			FinishProbe(*g_inFlight, *cap);
			g_inFlight.reset();
			++g_done;
			std::lock_guard<std::mutex> lk(g_capMu);
			g_capDone.reset();
		} else if (++g_inFlight->frame > 600) {
			console->Warn("[bridge] digharness: the capture of %s never came back from the render thread\n", g_inFlight->name.c_str());
			g_inFlight.reset();
		}
		return;
	}
	if (g_want) BeginProbe(std::move(g_want), view);
}

bool DigHarnessBusy() { return g_want || g_inFlight || !g_cmdQueue.empty(); }

CON_COMMAND(pc_dh_probe, "pc_dh_probe <dir> <name> [gridW gridH] - (bridge #73) one data probe of the main view: expected vs drawn depth per sample ray, the client's entity states, the camera's leaf; cache/bridge/digharness/<dir>/<name>.json\n") {
	if (args.ArgC() < 3) return console->Print("usage: pc_dh_probe <dir> <name> [gridW gridH]\n");
	if (g_want || g_inFlight) return console->Print("[bridge] digharness: a probe is already running\n");
	auto p = std::make_unique<Probe>();
	p->dir = args[1], p->name = args[2];
	if (args.ArgC() > 4) p->gw = std::max(4, std::min(256, std::atoi(args[3]))), p->gh = std::max(4, std::min(256, std::atoi(args[4])));
	g_want = std::move(p);
}

CON_COMMAND(pc_dh_seam, "pc_dh_seam <dir> <name> - (bridge #76) a seam probe: rays aimed at the faces between dug cells and the cells the host's surface runs through (the mouth's edges, the tunnel's walls within one cell of the host face); fails on void or missing surface\n") {
	if (args.ArgC() < 3) return console->Print("usage: pc_dh_seam <dir> <name>\n");
	if (g_want || g_inFlight) return console->Print("[bridge] digharness: a probe is already running\n");
	auto p = std::make_unique<Probe>();
	p->dir = args[1], p->name = args[2], p->seam = true;
	g_want = std::move(p);
}

CON_COMMAND(pc_dh_status, "pc_dh_status - (bridge #73) the harness: probes done, busy, the last summary\n") {
	console->Print("[bridge] digharness: %d probes done, %s; last: %s\n", g_done, DigHarnessBusy() ? "busy" : "idle", g_lastSummary.c_str());
}

// ---- scenario helpers: a wall found by tracing, a cut dug into it through Minecraft, the player put at a distance or inside --------
// Map-agnostic: the scripts name a direction, never a coordinate.
namespace {
struct Wall {
	bool ok = false;
	Vector hit, n, dir;   // the wall point hit, its normal (horizontal, toward the player), the horizontal direction into the wall
	float floorZ = 0;     // the floor in front of the wall
	float thickness = 0;  // solid along dir from hit (capped)
	bool openBehind = false;
	int wide = 1, high = 2, deep = 2;
	std::string surf;
	bool cut = false;
	Vector cutCentre;  // the mined cells' centre (Source): placements line up with it, so a hull fits the opening
	std::vector<std::array<int, 4>> cells;  // (bridge #75) the mined cells (Minecraft x y z, depth index): pc_dh_enter stands in the deepest
} g_wall;

float FloorBelow(const Vector &p) {
	CGameTrace tr;
	if (engine.Trace(p, p - Vector(0, 0, 400), Vector(0, 0, 0), 0x1 | 0x8, tr, true) && !tr.startsolid) return tr.endpos.z;
	return p.z - 64;
}
// solid thickness along dir from p (stepping 2 u), and whether open, inside-the-world air follows within `cap`
float Thickness(const Vector &p, const Vector &dir, float cap, bool &openBehind) {
	void *tr = engine.traceServer->ThisPtr();
	openBehind = false;
	for (float t = 1.0f; t < cap; t += 2.0f) {
		const Vector q = p + dir * t;
		if (!(engine.GetPointContentsWorld(tr, q, 0x1) & 0x1)) {
			openBehind = !engine.PointOutsideWorld(tr, q);
			return t;
		}
	}
	return cap;
}
void PlaceAt(const Vector &feet, float yaw, float pitch) {
	char c[256];
	if (McModeActive()) {
		std::snprintf(c, sizeof c, "pcb_mc_place %.1f %.1f %.1f %.1f 24", feet.x, feet.y, feet.z, yaw);
		engine.Cmd(c);
	} else {
		std::snprintf(c, sizeof c, "setpos %.1f %.1f %.1f", feet.x, feet.y, feet.z);
		engine.Cmd(c);
	}
	std::snprintf(c, sizeof c, "pcb_setview %.1f %.1f", pitch, yaw);
	engine.Cmd(c);
}
}  // namespace

CON_COMMAND(pc_dh_aimwall, "pc_dh_aimwall <yaw|-999> [minD=60] [maxD=900] [through=0] [maxThick=130] [surface substring|-] - (bridge #73) find a flat wall from the eye, scanning yaw +-60 deg (5 deg steps, nearest to <yaw> first; -999: all round); through=1: only a wall with open air behind it\n") {
	if (args.ArgC() < 2) return;
	const float yaw0 = (float)std::atof(args[1]);
	const float minD = args.ArgC() > 2 ? (float)std::atof(args[2]) : 60.0f, maxD = args.ArgC() > 3 ? (float)std::atof(args[3]) : 900.0f;
	const bool through = args.ArgC() > 4 && std::atoi(args[4]) != 0;
	const float maxThick = args.ArgC() > 5 ? (float)std::atof(args[5]) : 130.0f;
	const std::string want = args.ArgC() > 6 && std::strcmp(args[6], "-") ? args[6] : "";  // a surface name must contain this (e.g. white)
	const bool any = yaw0 < -360.0f;  // -999: every direction
	const Vector feet = McModeActive() ? McFeetSrc() : PlayerOrigin();
	g_wall = Wall();
	for (int k = 0; k <= (any ? 71 : 24) && !g_wall.ok; ++k) {
		const float yaw = any ? k * 5.0f : yaw0 + (k % 2 ? 1 : -1) * 5.0f * ((k + 1) / 2);
		const float yr = yaw * (float)M_PI / 180.0f;
		const Vector dir(std::cos(yr), std::sin(yr), 0);
		for (float h : {44.0f, 20.0f}) {
			const Vector eye = feet + Vector(0, 0, h);
			CGameTrace tr;
			if (!engine.Trace(eye, eye + dir * maxD, Vector(0, 0, 0), 0x1, tr, true) || tr.startsolid || tr.fraction >= 1.0f) continue;
			const float d = tr.fraction * maxD;
			if (d < minD || std::fabs(tr.plane.normal.z) > 0.1f) continue;
			if (!want.empty()) {
				std::string nm = tr.surface.name ? tr.surface.name : "";
				for (char &ch : nm) ch = (char)std::tolower((unsigned char)ch);
				if (nm.find(want) == std::string::npos) continue;
			}
			Vector n = tr.plane.normal;
			n.z = 0;
			n = n * (1.0f / std::max(n.Length(), 1e-4f));
			if (n.x * dir.x + n.y * dir.y > -0.8f) continue;  // a wall square to the view (within ~37 deg)
			bool open = false;
			const Vector into = n * -1.0f;
			const float th = Thickness(tr.endpos, into, through ? maxThick + 2 : 200.0f, open);
			if (through && (!open || th > maxThick)) continue;
			g_wall.surf = tr.surface.name ? tr.surface.name : "";
			g_wall.ok = true, g_wall.hit = tr.endpos, g_wall.n = n, g_wall.dir = into, g_wall.thickness = th, g_wall.openBehind = open;
			g_wall.floorZ = FloorBelow(tr.endpos + n * 24.0f + Vector(0, 0, 4));
			break;
		}
	}
	if (!g_wall.ok) return console->Print("[bridge] digharness: no wall found from (%.0f %.0f %.0f) toward yaw %.0f\n", feet.x, feet.y, feet.z, yaw0);
	const float yaw = std::atan2(g_wall.dir.y, g_wall.dir.x) * 180.0f / (float)M_PI;
	console->Print("[bridge] digharness: wall at (%.1f %.1f %.1f) [%s], normal (%.2f %.2f), %.0f u thick%s, floor z %.1f; facing yaw %.1f\n", g_wall.hit.x, g_wall.hit.y, g_wall.hit.z, g_wall.surf.c_str(), g_wall.n.x, g_wall.n.y, g_wall.thickness,
	               g_wall.openBehind ? " with open air behind" : "", g_wall.floorZ, yaw);
}

CON_COMMAND(pc_dh_wallcut, "pc_dh_wallcut [wide=1] [high=2] [deep=2|0=through] - (bridge #73) mine a cut into the wall pc_dh_aimwall found, from its floor up, through Minecraft (/pcdig mine)\n") {
	if (!g_wall.ok) return console->Print("[bridge] digharness: no wall (pc_dh_aimwall first)\n");
	g_wall.wide = args.ArgC() > 1 ? std::max(1, std::atoi(args[1])) : 1;
	g_wall.high = args.ArgC() > 2 ? std::max(1, std::atoi(args[2])) : 2;
	const int deepArg = args.ArgC() > 3 ? std::atoi(args[3]) : 2;
	g_wall.deep = deepArg > 0 ? deepArg : (int)std::ceil((g_wall.thickness + 2.0f) / kUnitsPerBlock) + 1;
	const Vector side(-g_wall.dir.y, g_wall.dir.x, 0);
	std::set<std::array<int, 3>> cells;
	// (bridge #75) the cut's lowest row is the Minecraft block row nearest the room's floor (the floor's height rounded to the block
	// grid), not the row that contains a point 1 u above it: with the map's grid offset that row could start up to a block below the
	// floor, and the opening above the floor was then one block high (s33 could not walk in)
	const int floorRow = (int)std::lround(SrcToMc(Vector(g_wall.hit.x, g_wall.hit.y, g_wall.floorZ)).y);
	g_wall.cells.clear();
	for (int d = 0; d < g_wall.deep; ++d)
		for (int w = 0; w < g_wall.wide; ++w)
			for (int h = 0; h < g_wall.high; ++h) {
				const Vector p = Vector(g_wall.hit.x, g_wall.hit.y, g_wall.floorZ) + g_wall.dir * (1.0f + d * kUnitsPerBlock) + side * ((w - (g_wall.wide - 1) * 0.5f) * kUnitsPerBlock);
				const Vector m = SrcToMc(p);
				const std::array<int, 3> c{(int)std::floor(m.x), floorRow + h, (int)std::floor(m.z)};
				if (cells.insert(c).second) g_wall.cells.push_back({c[0], c[1], c[2], d});
			}
	Vector sum(0, 0, 0);
	for (const auto &c : cells) sum += McToSrc(c[0] + 0.5, c[1] + 0.5, c[2] + 0.5);
	if (!cells.empty()) g_wall.cutCentre = sum * (1.0f / cells.size()), g_wall.cut = true;
	for (const auto &c : cells) {
		char cmd[128];
		std::snprintf(cmd, sizeof cmd, "pcb_mc_cmd pcdig mine %d %d %d", c[0], c[1], c[2]);
		g_cmdQueue.push_back(cmd);
	}
	console->Print("[bridge] digharness: mining %zu cells (%d wide, %d high, %d deep) into the wall at (%.0f %.0f %.0f)\n", cells.size(), g_wall.wide, g_wall.high, g_wall.deep, g_wall.hit.x, g_wall.hit.y, g_wall.hit.z);
}

CON_COMMAND(pc_dh_standoff, "pc_dh_standoff <dist> [pitch=8] [yawOffset=0] [lateral=0] - (bridge #73) put the current player <dist> u in front of the wall cut, facing it\n") {
	if (!g_wall.ok || args.ArgC() < 2) return console->Print("[bridge] digharness: no wall (pc_dh_aimwall first)\n");
	const float dist = (float)std::atof(args[1]), pitch = args.ArgC() > 2 ? (float)std::atof(args[2]) : 8.0f, yawOff = args.ArgC() > 3 ? (float)std::atof(args[3]) : 0.0f,
	            lat = args.ArgC() > 4 ? (float)std::atof(args[4]) : 0.0f;
	const Vector side(-g_wall.dir.y, g_wall.dir.x, 0);
	Vector base(g_wall.hit.x, g_wall.hit.y, g_wall.floorZ);
	if (g_wall.cut) {  // line up with the cut's middle along the wall
		const float off = Vector::DotProduct(g_wall.cutCentre - base, side);
		base = base + side * off;
	}
	Vector feet = base + g_wall.n * dist + side * lat;
	feet.z = FloorBelow(feet + Vector(0, 0, 40)) + 1.0f;
	PlaceAt(feet, std::atan2(g_wall.dir.y, g_wall.dir.x) * 180.0f / (float)M_PI + yawOff, pitch);
}

CON_COMMAND(pc_dh_inside, "pc_dh_inside [pitch=0] [yawOffset=180] [depthCells=0.6] - (bridge #73) put the current player inside the wall cut (on its floor), by default facing back out\n") {
	if (!g_wall.ok) return console->Print("[bridge] digharness: no wall (pc_dh_aimwall first)\n");
	const float pitch = args.ArgC() > 1 ? (float)std::atof(args[1]) : 0.0f, yawOff = args.ArgC() > 2 ? (float)std::atof(args[2]) : 180.0f, dc = args.ArgC() > 3 ? (float)std::atof(args[3]) : 0.6f;
	Vector feet = Vector(g_wall.hit.x, g_wall.hit.y, g_wall.floorZ) + g_wall.dir * (dc * kUnitsPerBlock);
	// the cut's floor: the bottom of the lowest mined cell
	const Vector m = SrcToMc(feet + Vector(0, 0, 1));
	feet.z = McToSrc(m.x, std::floor(m.y), m.z).z + 0.5f;
	PlaceAt(feet, std::atan2(g_wall.dir.y, g_wall.dir.x) * 180.0f / (float)M_PI + yawOff, pitch);
}

// (bridge #75) stand INSIDE the cut: Minecraft's own teleport to the floor of the deepest column of the lowest row (the cell is air for
// Minecraft; the host's placement refuses it, the body being in Portal 2's solid there). The body follows Minecraft (the puppet), so
// the server's eye is inside the wall as when a player walks in. Then pc_dh_where says whether the eye is in a dug cell.
CON_COMMAND(pc_dh_enter, "pc_dh_enter [depthIndex=-1: deepest] - (bridge #75) teleport the Minecraft player onto the floor of the cut (its lowest row, the given depth), through Minecraft's /tp\n") {
	if (!g_wall.cut || g_wall.cells.empty()) return console->Print("[bridge] digharness: no cut (pc_dh_wallcut first)\n");
	int want = args.ArgC() > 1 ? std::atoi(args[1]) : -1;
	int maxD = 0, minY = 1 << 30;
	for (const auto &c : g_wall.cells) maxD = std::max(maxD, c[3]), minY = std::min(minY, c[1]);
	if (want < 0 || want > maxD) {
		// the deepest column whose floor-row and eye-row cells are both dug (a through-cut's last column can be open air behind a thin
		// wall: not inside anything)
		want = -1;
		for (int d = maxD; d >= 0 && want < 0; --d)
			for (const auto &c : g_wall.cells)
				if (c[3] == d && c[1] == minY && DigIsDugSrc(McToSrc(c[0] + 0.5, c[1] + 0.5, c[2] + 0.5)) && DigIsDugSrc(McToSrc(c[0] + 0.5, c[1] + 1.5, c[2] + 0.5))) {
					want = d;
					break;
				}
		for (int d = maxD; d >= 0 && want < 0; --d)  // else the deepest column with a dug floor cell (a thin panel: its own column)
			for (const auto &c : g_wall.cells)
				if (c[3] == d && c[1] == minY && DigIsDugSrc(McToSrc(c[0] + 0.5, c[1] + 0.5, c[2] + 0.5))) {
					want = d;
					break;
				}
		if (want < 0) want = maxD;
	}
	double sx = 0, sz = 0;
	int n = 0;
	for (const auto &c : g_wall.cells)
		if (c[3] == want && c[1] == minY) sx += c[0] + 0.5, sz += c[2] + 0.5, ++n;
	if (!n) return console->Print("[bridge] digharness: no cell at depth %d\n", want);
	char cmd[160];
	std::snprintf(cmd, sizeof cmd, "pcb_mc_cmd tp @a %.3f %.3f %.3f", sx / n, minY + 0.02, sz / n);
	engine.Cmd(cmd);
	const Vector s = McToSrc(sx / n, minY, sz / n);
	console->Print("[bridge] digharness: into the cut: Minecraft (%.2f %d %.2f) = Source (%.1f %.1f %.1f), depth %d of %d\n", sx / n, minY, sz / n, s.x, s.y, s.z, want, maxD);
}

CON_COMMAND(pc_dh_where, "pc_dh_where - (bridge #75) where the Minecraft player's feet and eye are, and whether they are in a dug cell\n") {
	const Vector f = McModeActive() ? McFeetSrc() : PlayerOrigin();
	const Vector eye = f + Vector(0, 0, 64.8f);
	const bool fd = DigIsDugSrc(f + Vector(0, 0, 2)), ed = DigIsDugSrc(eye);
	void *tr = engine.traceServer->ThisPtr();
	const int ec = engine.GetPointContentsWorld(tr, eye, 0x1);
	console->Print("[bridge] digharness: where: feet (%.1f %.1f %.1f) dug %d, eye (%.1f %.1f %.1f) dug %d, eye in Portal 2 solid %d; %s\n", f.x, f.y, f.z, fd ? 1 : 0, eye.x, eye.y, eye.z, ed ? 1 : 0, (ec & 1) ? 1 : 0,
	               ed ? "INSIDE the cut" : "NOT inside the cut");
}

CON_COMMAND(pc_dh_face, "pc_dh_face <pitch> <yawOffset> - (bridge #73) turn the view to the wall's direction plus an offset (180 = back out of the cut)\n") {
	if (!g_wall.ok || args.ArgC() < 3) return;
	char c[96];
	std::snprintf(c, sizeof c, "pcb_setview %.1f %.1f", (float)std::atof(args[1]), std::atan2(g_wall.dir.y, g_wall.dir.x) * 180.0f / (float)M_PI + (float)std::atof(args[2]));
	engine.Cmd(c);
}

CON_COMMAND(pc_dh_mine, "pc_dh_mine <x y z> [x1 y1 z1] - (bridge #73) mine every cell of a Source box through Minecraft (/pcdig mine: as a player would, the reveal follows)\n") {
	if (args.ArgC() < 4) return console->Print("usage: pc_dh_mine <x y z> [x1 y1 z1]\n");
	const Vector a((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3]));
	const Vector b = args.ArgC() >= 7 ? Vector((float)std::atof(args[4]), (float)std::atof(args[5]), (float)std::atof(args[6])) : a;
	const Vector ma = SrcToMc(a), mb = SrcToMc(b);
	int n = 0;
	for (int y = (int)std::floor(std::fmin(ma.y, mb.y)); y <= (int)std::floor(std::fmax(ma.y, mb.y)); ++y)
		for (int z = (int)std::floor(std::fmin(ma.z, mb.z)); z <= (int)std::floor(std::fmax(ma.z, mb.z)); ++z)
			for (int x = (int)std::floor(std::fmin(ma.x, mb.x)); x <= (int)std::floor(std::fmax(ma.x, mb.x)) && n < 64; ++x) {
				char cmd[128];
				std::snprintf(cmd, sizeof cmd, "pcb_mc_cmd pcdig mine %d %d %d", x, y, z);
				g_cmdQueue.push_back(cmd);
				++n;
			}
	console->Print("[bridge] digharness: asked Minecraft to mine %d cells\n", n);
}

}  // namespace bridge
