// Portalcraft bridge (bridge #73): cut-away world, renderer-neutral half. Portal 2's world is not drawn inside dug cells, at the
// fragment level, by Portal 2's own shaders (DESIGN-dig-render.md sections 2 and 3). This file is the logic every renderer shares:
//   * the settings and counters, the commands (pc_dig_render, pc_cut_rule, pc_cut_audit, pc_cut_perf, pc_cut_dump);
//   * the dug cells as a sparse brick grid (BuildData, main thread: a brick index over the whole +-16384 u map and an atlas of
//     8x8x8-cell bricks), handed to the backend for upload on the render thread;
//   * the per-world-draw view data (ViewBegin: the inverse of the x,y,w rows of view*projection, from the view's real matrices,
//     which Portal 2's oblique portal clipping never changes: it only rewrites the depth row), the 3D skybox's view is never cut;
//   * when the backend is installed (CutInstall/CutFrame) and the callbacks around the engine's world draws (visclamp.cpp).
// The backend (render_backend.hpp rb::CutBackend*) turns that into API state: GL on Linux patches togl's GLSL (gl_cut.cpp), a
// Direct3D 9 backend patches the shader bytecode.
//
// (Split out of cutrender.cpp, now gl_cut.cpp; the code moved unchanged except for the backend calls.)
#include "bridge/bridge.hpp"
#include "bridge/prof.hpp"
#include "bridge/coords.hpp"
#include "bridge/dig.hpp"
#include "bridge/render_backend.hpp"
#include "bridge/slots.hpp"
#include "bridge/platform/platform.hpp"

#include "Modules/MaterialSystem.hpp"
#include "Modules/Tier1.hpp"
#include "Utils/SDK/MeshBuilder.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bridge {

void RunOnRenderThread(void (*fn)(void *), void *arg);  // terrain.cpp
const char *McMapName();                                  // mc.cpp

namespace {

// ---- settings ------------------------------------------------------------------------------------------------------------------
std::atomic<int> g_renderMode{2};  // pc_dig_render: 2 the cut-away shaders (default since bridge #75), 1 off (the world is drawn uncut: for A/B only; the old punch is gone)
std::atomic<int> g_audit{0};       // pc_cut_audit
std::atomic<int> g_cutRule{1};     // pc_cut_rule (bridge #76): 1 the step along the surface's normal, 0 along the view ray (#73-#75)
std::atomic<int> g_active{0};      // backend installed and the textures exist
std::atomic<int> g_installState{0};  // 0 not tried, 1 installing, 2 ok, -1 failed
constexpr float kEps = 0.25f;

// ---- counters (any thread) -----------------------------------------------------------------------------------------------------
std::atomic<long> g_worldDraws{0}, g_worldCut{0}, g_skySkipped{0}, g_noMatrix{0}, g_uploads{0};
std::string g_lastError;
std::mutex g_errMu;

using slots::VCall;

// ---- the dug-cell bricks ---------------------------------------------------------------------------------------------------------
using CutData = rb::CutBricks;
constexpr int kBrick = rb::kCutBrick;
constexpr int kAtlasBricks = rb::kCutAtlasBricks;
constexpr int kMaxBricks = kAtlasBricks * kAtlasBricks * kAtlasBricks - 1;
std::mutex g_dataMu;
std::shared_ptr<const CutData> g_pending;   // built on the main thread, uploaded on the render thread
std::shared_ptr<const CutData> g_uploaded;  // render thread
uint32_t g_builtGen = 0xFFFFFFFFu;
double g_builtSig = 0;

// Main thread, once a frame: rebuild the textures' contents when the dug set (or the mapping) changed.
void BuildData() {
	const uint32_t gen = DigGen();
	const double sig = g_map.src.x * 1.0 + g_map.src.y * 3.0 + g_map.src.z * 7.0 + g_map.mcX * 11.0 + g_map.mcY * 13.0 + g_map.mcZ * 17.0;
	if (gen == g_builtGen && sig == g_builtSig) return;
	g_builtGen = gen, g_builtSig = sig;
	auto d = std::make_shared<CutData>();
	d->gen = gen;
	// world -> cells: x = mcX + (sx - src.x) / 40, y = mcY + (sz - src.z) / 40, z = mcZ - (sy - src.y) / 40
	const double k = 1.0 / kUnitsPerBlock;
	const double m[12] = {k, 0, 0, g_map.mcX - g_map.src.x * k, 0, 0, k, g_map.mcY - g_map.src.z * k, 0, -k, 0, g_map.mcZ + g_map.src.y * k};
	std::memcpy(d->map, m, sizeof m);
	// the index covers the cells of Portal 2's whole world (+-16384 u)
	int lo[3], hi[3];
	for (int a = 0; a < 3; ++a) lo[a] = 1 << 30, hi[a] = -(1 << 30);
	for (int c = 0; c < 8; ++c) {
		const double s[3] = {(c & 1) ? 16384.0 : -16384.0, (c & 2) ? 16384.0 : -16384.0, (c & 4) ? 16384.0 : -16384.0};
		for (int a = 0; a < 3; ++a) {
			const double v = m[a * 4] * s[0] + m[a * 4 + 1] * s[1] + m[a * 4 + 2] * s[2] + m[a * 4 + 3];
			lo[a] = std::min(lo[a], (int)std::floor(v) - 1), hi[a] = std::max(hi[a], (int)std::floor(v) + 1);
		}
	}
	for (int a = 0; a < 3; ++a) {
		d->org[a] = (int)std::floor(lo[a] / (double)kBrick) * kBrick;
		d->dims[a] = (hi[a] - d->org[a]) / kBrick + 1;
	}
	d->index.assign((size_t)d->dims[0] * d->dims[1] * d->dims[2] * 2, 0);
	d->atlas.assign((size_t)128 * 128 * 128, 0);
	std::vector<std::pair<Vector, Vector>> boxes;
	DigAllBoxes(boxes);
	std::unordered_map<long long, int> slotOf;
	for (const auto &b : boxes) {
		const Vector c = (b.first + b.second) * 0.5f;
		int cell[3];
		for (int a = 0; a < 3; ++a) cell[a] = (int)std::floor(m[a * 4] * c.x + m[a * 4 + 1] * c.y + m[a * 4 + 2] * c.z + m[a * 4 + 3]);
		int bi[3], l[3];
		bool in = true;
		for (int a = 0; a < 3; ++a) {
			const int r = cell[a] - d->org[a];
			bi[a] = r / kBrick, l[a] = r % kBrick;
			in = in && r >= 0 && bi[a] < d->dims[a];
		}
		if (!in) {
			++d->dropped;
			continue;
		}
		const long long key = ((long long)bi[2] * d->dims[1] + bi[1]) * d->dims[0] + bi[0];
		int slot;
		auto it = slotOf.find(key);
		if (it != slotOf.end()) slot = it->second;
		else {
			if (d->bricks >= kMaxBricks) {
				++d->dropped;
				continue;
			}
			slot = d->bricks++;
			slotOf[key] = slot;
			const int v = slot + 1;
			d->index[key * 2] = (uint8_t)(v & 255), d->index[key * 2 + 1] = (uint8_t)(v >> 8);
		}
		const int sx = slot % kAtlasBricks, sy = (slot / kAtlasBricks) % kAtlasBricks, sz = slot / (kAtlasBricks * kAtlasBricks);
		const int tx = sx * kBrick + l[0], ty = sy * kBrick + l[1], tz = sz * kBrick + l[2];
		d->atlas[((size_t)tz * 128 + ty) * 128 + tx] = 255;
		++d->cells;
	}
	if (d->dropped) console->Warn("[bridge] cut: %d dug cells could not be put in the cut textures\n", d->dropped);
	std::lock_guard<std::mutex> lk(g_dataMu);
	g_pending = d;
}

// ---- per view (render thread) ----------------------------------------------------------------------------------------------------
IMaterial *g_flushMat = nullptr;

// The material system batches dynamic-mesh draws: a batch still pending when the flag changes would be drawn with the wrong
// flag. Binding another material makes it flush them (an unbuffered dynamic mesh with our own material, never drawn).
void FlushBatches(IMatRenderContext *rc) {
	if (rc && g_flushMat) VCall<void *>(rc, slots::kRcGetDynamicMesh, false, (void *)nullptr, (void *)nullptr, (void *)g_flushMat);
}

// clip = C (world, 1). Source keeps VIEW / PROJECTION either in column-vector form (translation in the last column) or
// transposed; the view matrix's shape says which (its last row is 0 0 0 1 in column form).
bool ClipMatrix(IMatRenderContext *rc, double C[4][4], double eye[3]) {
	float v[4][4], p[4][4];
	VCall<void>(rc, slots::kRcGetMatrix, 0, (float *)v);
	VCall<void>(rc, slots::kRcGetMatrix, 1, (float *)p);
	const bool colForm = std::fabs(v[3][0]) + std::fabs(v[3][1]) + std::fabs(v[3][2]) < 1e-4f && std::fabs(v[3][3] - 1.0f) < 1e-4f;
	double V[4][4], P[4][4];
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j) V[i][j] = colForm ? v[i][j] : v[j][i], P[i][j] = colForm ? p[i][j] : p[j][i];
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j) {
			C[i][j] = 0;
			for (int k = 0; k < 4; ++k) C[i][j] += P[i][k] * V[k][j];
		}
	// the eye: V = [R t; 0 1] -> eye = -R^T t
	for (int a = 0; a < 3; ++a) eye[a] = -(V[0][a] * V[0][3] + V[1][a] * V[1][3] + V[2][a] * V[2][3]);
	return std::fabs(C[3][0]) + std::fabs(C[3][1]) + std::fabs(C[3][2]) > 1e-6;  // a perspective view (w depends on the point)
}

int g_skyArea = -1;
Interface *g_engServer = nullptr;

void ViewBegin(void *) {
	if (!g_active.load()) return;
	IMatRenderContext *rc = materialSystem->GetRenderContext();
	if (!rc) return;
	FlushBatches(rc);
	{
		std::shared_ptr<const CutData> p;
		{
			std::lock_guard<std::mutex> lk(g_dataMu);
			p = g_pending;
			g_pending.reset();
		}
		if (p) rb::CutBackendUpload(*p), ++g_uploads, g_uploaded = p;
	}
	++g_worldDraws;
	double C[4][4], eye[3];
	const std::shared_ptr<const CutData> d = g_uploaded;
	if (!d || d->cells == 0 || !ClipMatrix(rc, C, eye)) {
		if (d && d->cells) ++g_noMatrix;
		rb::CutBackendViewOff();
		VCall<int>(rc, slots::kRcRelease);
		return;
	}
	// the 3D skybox's view draws the sky room at its own coordinates: never cut
	if (g_skyArea > 0 && g_engServer) {
		const Vector e((float)eye[0], (float)eye[1], (float)eye[2]);
		if (VCall<int>(g_engServer->ThisPtr(), slots::kEngGetArea, (const Vector *)&e) == g_skyArea) {
			++g_skySkipped;
			rb::CutBackendViewOff();
			VCall<int>(rc, slots::kRcRelease);
			return;
		}
	}
	// world = A^-1 (clip.xyw - b), A / b the x, y, w rows of C
	double A[3][3], b[3];
	const int rows[3] = {0, 1, 3};
	for (int r = 0; r < 3; ++r) {
		for (int c = 0; c < 3; ++c) A[r][c] = C[rows[r]][c];
		b[r] = C[rows[r]][3];
	}
	const double det = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) + A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
	if (std::fabs(det) < 1e-12) {
		++g_noMatrix;
		rb::CutBackendViewOff();
		VCall<int>(rc, slots::kRcRelease);
		return;
	}
	double I[3][3];
	I[0][0] = (A[1][1] * A[2][2] - A[1][2] * A[2][1]) / det, I[0][1] = (A[0][2] * A[2][1] - A[0][1] * A[2][2]) / det, I[0][2] = (A[0][1] * A[1][2] - A[0][2] * A[1][1]) / det;
	I[1][0] = (A[1][2] * A[2][0] - A[1][0] * A[2][2]) / det, I[1][1] = (A[0][0] * A[2][2] - A[0][2] * A[2][0]) / det, I[1][2] = (A[0][2] * A[1][0] - A[0][0] * A[1][2]) / det;
	I[2][0] = (A[1][0] * A[2][1] - A[1][1] * A[2][0]) / det, I[2][1] = (A[0][1] * A[2][0] - A[0][0] * A[2][1]) / det, I[2][2] = (A[0][0] * A[1][1] - A[0][1] * A[1][0]) / det;
	rb::CutView v;
	for (int r = 0; r < 3; ++r) {
		for (int c = 0; c < 3; ++c) v.worldFromClip[r][c] = (float)I[r][c];
		v.worldFromClip[r][3] = (float)-(I[r][0] * b[0] + I[r][1] * b[1] + I[r][2] * b[2]);
	}
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 4; ++c) v.cellFromWorld[r][c] = (float)d->map[r * 4 + c];
	for (int a = 0; a < 3; ++a) v.eye[a] = (float)eye[a], v.org[a] = d->org[a], v.dims[a] = d->dims[a];
	v.eps = kEps;
	v.audit = g_audit.load() != 0;
	v.viewRay = !g_cutRule.load();
	v.brick = kBrick;
	rb::CutBackendViewOn(v);
	++g_worldCut;
	VCall<int>(rc, slots::kRcRelease);
}

void ViewEnd(void *) {
	if (!g_active.load()) return;
	IMatRenderContext *rc = materialSystem->GetRenderContext();
	FlushBatches(rc);
	rb::CutBackendViewOff();
	if (rc) VCall<int>(rc, slots::kRcRelease);
}

void InstallOnRenderThread(void *) {
	if (g_installState.load() != 1) return;
	std::string err;
	if (!rb::CutBackendInstall(err)) {
		g_installState = -1;
		if (!err.empty()) CutNoteError(err);
		return;
	}
	g_installState = 2;
	g_active = 1;
}

// ---- (bridge #75) the GPU cost of Portal 2's world draws (DESIGN 3's budget): timestamps around every world draw call
// (DrawWorldLists and the translucent world surfaces, every view), summed per frame over a window of frames. Run it at the same pose
// in pc_dig_render 1 and 2 (and with no dug cell) to read the cut test's cost; the old path's punch and peel are separate draws and
// are timed by pc_dig_perf in digrender.cpp's own scope (PROF), not here.
std::atomic<long> g_perfFrame{0};
std::atomic<int> g_perfOn{0};
long g_perfFrames = 0, g_perfFirst = 0, g_perfLast = -1;
std::string g_perfLabel;
void PerfStampBegin(void *) { rb::CutBackendPerfBegin(); }
void PerfStampEnd(void *) { rb::CutBackendPerfEnd(); }

}  // namespace

void CutNoteError(const std::string &e) {
	std::lock_guard<std::mutex> lk(g_errMu);
	if (g_lastError.empty()) console->Warn("[bridge] cut: %s\n", e.c_str());
	g_lastError = e;
}

// ---- the interface for the rest of the plugin (main thread) --------------------------------------------------------------------
int DigRenderMode() { return rb::CutBackendAvailable() ? g_renderMode.load() : 1; }
bool CutActive() { return g_active.load() != 0; }

// visclamp.cpp's DrawWorldLists / DrawTranslucentSurfaces hooks, around the engine's own call
void CutWorldDrawBegin() {
	if (g_renderMode.load() == 2 && g_active.load()) RunOnRenderThread(ViewBegin, nullptr);
}
void CutWorldDrawEnd() {
	if (g_renderMode.load() == 2 && g_active.load()) RunOnRenderThread(ViewEnd, nullptr);
}

// Install the backend (once): the render thread does it (or this thread, when the material system is not queued yet: before a map).
void CutInstall() {
	if (g_installState.load() == 0) {
		if (!g_flushMat) {
			auto kv = new KeyValues("UnlitGeneric");
			kv->SetInt("$vertexcolor", 1);
			g_flushMat = materialSystem->CreateMaterial("_pcb_cut_flush", kv);
		}
		if (!g_engServer) g_engServer = Interface::Create(MODULE_NAME("engine"), "VEngineServer022", false);
		g_installState = 1;
		RunOnRenderThread(InstallOnRenderThread, nullptr);
	}
}

// Once a frame (visclamp.cpp, from OverrideView): install when switched on, keep the textures in step.
void CutFrame() {
	PROF("cutrender");
	if (DigRenderMode() != 2) return;
	static std::string s_map;
	if (s_map != McMapName()) s_map = McMapName(), CutLevelStart();
	CutInstall();
	if (!g_active.load()) return;
	BuildData();
}

// A new level: the sky camera's area (its view is never cut).
void CutLevelStart() {
	g_skyArea = -1;
	auto sky = EntFind("sky_camera");
	if (!sky.empty() && g_engServer) {
		const Vector o = EntVec(sky[0], "origin");
		g_skyArea = VCall<int>(g_engServer->ThisPtr(), slots::kEngGetArea, (const Vector *)&o);
	}
	g_builtGen = 0xFFFFFFFFu;
}

void CutShutdown() {
	rb::CutBackendUninstall();
	g_active = 0;
}

void CutPerfDrawBegin() {
	if (g_perfOn.load()) RunOnRenderThread(PerfStampBegin, nullptr);
}
void CutPerfDrawEnd() {
	if (g_perfOn.load()) RunOnRenderThread(PerfStampEnd, nullptr);
}
// once a frame (visclamp.cpp)
void CutPerfFrame() {
	if (!g_perfOn.load()) return;
	const long f = ++g_perfFrame;
	if (f < g_perfFirst) return;
	if (f == g_perfFirst) rb::CutBackendPerfReset();  // (the first frames of a window are skipped: queued draws of the frames before)
	if (f >= g_perfLast) {
		double gpuNs = 0;
		long draws = 0, lost = 0;
		rb::CutBackendPerfPoll(gpuNs, draws, lost);
		g_perfFrames = g_perfLast - g_perfFirst;
		g_perfOn = 0;
		console->Print("[bridge] CUT PERF %s (pc_dig_render %d, dug cells %s): %ld frames, world draws on the GPU %.3f ms per frame (%ld timed draws, %.1f per frame, %ld lost)\n", g_perfLabel.c_str(), g_renderMode.load(),
		               DigAny() ? "yes" : "no", g_perfFrames, g_perfFrames ? gpuNs * 1e-6 / g_perfFrames : 0.0, draws, g_perfFrames ? (double)draws / g_perfFrames : 0.0, lost);
	}
}

CON_COMMAND(pc_cut_perf, "pc_cut_perf <frames> [label] - (bridge #75) GPU time of Portal 2's world draws (every view) per frame over the next <frames> frames (GL timestamps); prints CUT PERF\n") {
	if (!rb::CutBackendHasPerf()) return console->Print("[bridge] pc_cut_perf: GPU timing is not available in this renderer backend\n");
	const long n = args.ArgC() > 1 ? std::max(10, std::atoi(args[1])) : 120;
	g_perfLabel = args.ArgC() > 2 ? args[2] : "-";
	const long f = g_perfFrame.load();
	g_perfFirst = f + 8, g_perfLast = f + 8 + n;
	g_perfOn = 1;
}

CON_COMMAND(pc_dig_render, "pc_dig_render [1|2] - how dug cells leave Portal 2's picture: 2 cut-away world shaders and visibility through cuts (default; bridge #73/#75, DESIGN-dig-render.md), 1 off (A/B only: the world is drawn uncut; the old stencil punch was removed in #75)\n") {
	if (args.ArgC() > 1) {
		const int m = std::atoi(args[1]) == 2 ? 2 : 1;
		g_renderMode = m;
		if (m != 2 && g_active.load()) RunOnRenderThread(ViewEnd, nullptr);
		if (m == 2 && rb::CutBackendAvailable()) CutInstall();  // (now: before a map loads, its programs are linked through the hook)
	}
	{
		const std::string t = rb::CutBackendPatchTiming();
		if (!t.empty()) console->Print("%s", t.c_str());
	}
	console->Print("[bridge] dig render %d (%s); cut: %s, %s; %ld world draws, %ld cut, %ld sky, %ld without matrices; %ld uploads%s%s\n",
	               g_renderMode.load(), g_renderMode.load() == 2 ? "cut-away shaders" : "OFF: the world is not cut", g_installState.load() == 2 ? "installed" : g_installState.load() < 0 ? "FAILED" : "not installed",
	               rb::CutBackendCounters().c_str(), g_worldDraws.load(), g_worldCut.load(), g_skySkipped.load(), g_noMatrix.load(), g_uploads.load(), g_lastError.empty() ? "" : "; last error: ",
	               g_lastError.c_str());
}

CON_COMMAND(pc_cut_rule, "pc_cut_rule [1|0] - (bridge #76) the cut's owner-cell step: 1 along the surface's normal (default), 0 along the view ray (bridges #73-#75, for A/B)\n") {
	if (args.ArgC() > 1) g_cutRule = std::atoi(args[1]) != 0;
	console->Print("[bridge] cut rule %d (%s)\n", g_cutRule.load(), g_cutRule.load() ? "surface normal" : "view ray");
}

CON_COMMAND(pc_cut_audit, "pc_cut_audit [0|1] - (bridge #73, tests) the cut-away shaders paint the cut fragments magenta instead of removing them\n") {
	if (args.ArgC() > 1) g_audit = std::atoi(args[1]) != 0;
	console->Print("[bridge] cut audit %d\n", g_audit.load());
}

CON_COMMAND(pc_cut_dump, "pc_cut_dump - (bridge #73) write one patched vertex / fragment shader pair to cache/bridge/cut_sample_{vs,fs}.glsl (generated text, for reading)\n") {
	rb::CutBackendDump();
}

}  // namespace bridge
