// Portalcraft bridge: Minecraft's own frame composited into Portal 2 (DESIGN-composite.md, phase A).
//
// One Minecraft layer. Every Portal 2 view whose camera is in the Minecraft region shows Minecraft
// rendered for that camera, through the one fixed region transform (McToSrc, 40 u per block):
//   * the camera itself once Chell is in the region;
//   * the virtual camera of a portal view that looks into the region (from the chamber through the
//     pit portal, or region to region through two portals on terrain), with the exit portal as its
//     oblique near plane.
// Per frame (OverrideView, pose final) the plugin predicts these cameras from the main camera and the
// live portal pair, keeps at most two (the largest on screen first; with none on screen, the view
// through the pit portal stays warm), assigns each to a stream by Minecraft-camera continuity and
// asks Minecraft for them (HostView + the v13 HostViewExt: crop window = the view's screen box,
// second camera rendered cheaper in the same Minecraft frame). Minecraft ships each image tagged
// with its request; the image lands in its stream's texture (cropped upload on the render thread,
// straight from shared memory).
// Each Portal 2 view is matched to a stream by pose (this frame's predicted camera, else stream
// continuity, else the nearest image), and draws that stream's newest image reprojected to the view
// (rotation, plus position against a proxy plane at the image's depth). A camera that crosses a
// portal is one continuous camera, so it keeps its stream and image: no switch, no stale frame.
// Placement: a camera behind a region portal's plane (a portal view) gets world-space geometry just
// beyond that plane (Portal 2's stencil and clip plane confine it); any other camera gets the image
// at the background depth with the depth test, so it fills exactly what Portal 2 left as the
// region's sky, and the per-pixel occlusion layers (Minecraft's depth in the image alpha) hide
// Portal 2 objects behind terrain. A region view never shows the region's background: no image yet
// means black, and only before the first Minecraft frame.
#include "bridge.hpp"
#include "slots.hpp"
#include "growth.hpp"
#include "prof.hpp"
#include <map>
#include "coords.hpp"
#include "link.hpp"

#include "Modules/MaterialSystem.hpp"
#include "Modules/Tier1.hpp"
#include "Offsets.hpp"
#include "Utils/Memory.hpp"
#include "Utils/SDK/ITexture.hpp"
#include "Utils/SDK/ImageFormat.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "platform/platform.hpp"
#include <vector>

namespace bridge {

using namespace skycraft::proto;

void RunOnRenderThread(void (*fn)(void *), void *arg);  // terrain.cpp
bool ClipRegionContains(const Vector &p);              // clip.cpp
bool ClipHasRegion();
bool ClipExitHint(Vector &pos, QAngle &ang);
int ClipRegionPortal();  // index of the portal inside the region, or -1
void TestCheck(bool ok, const char *what, const char *detail);  // testassert.cpp
bool TerrainRayBlocked(const Vector &a, const Vector &b);       // portalgun.cpp
int TransitApproach(float &pitch, float &yaw, float &areaPct);  // transit.cpp

static double NowS() {
	return (double)plat::MonotonicNs() * 1e-9;
}

// ---- options -----------------------------------------------------------------------------------
static bool g_enabled = true;
static float g_overscan = 1.1f;      // request frustum / view frustum (the reprojection margin)
static float g_margin = 0.1f;        // primary camera's crop box margin (NDC of the view, per side)
static int g_secondRd = 12;          // second camera's render distance (chunks)
static int g_secondEvery = 2;        // a portal view's second-camera image every N host frames, recursion views every 3N
                                     // (each image costs the material system's texture Download on the render
                                     // thread until phase C; 3 left portal views 2-7 frames old, mcworld runs)
static int g_maxCams = 6;            // region cameras tracked per frame (main + 2 portals x recursion depth 2)
static bool g_crop = true;           // crop requests to the view's screen box
static bool g_toneOverride = true;   // Minecraft's colours not exposure-scaled by Portal 2
static bool g_reproject = true;      // rotation reprojection
static bool g_reprojectPos = true;   // position reprojection against the proxy plane
static bool g_predict = true;        // rotation prediction per stream
static bool g_predictPos = true;     // position prediction per stream
static float g_predictPosMax = 64.0f;
static const float kPredictTauS = 0.03f;  // the smoothing of the camera's speed and turn rate (s)
static const float kPredictMaxS = 0.1f;   // the longest prediction (s)
static int g_srgb = 1;
static bool g_upload = true;
static int g_stage = 2;              // 2: inside the 3D view
// HUD option A (DESIGN-mcplayer Decisions): in Minecraft mode the request frustum is the view's own (overscan 1.0),
// so Minecraft's window equals the screen and its GUI is drawn 1:1 (the reprojection margin is given up there).
bool McModeActive();  // mc.cpp
static float g_mcOverscan = 1.0f;
bool McSwitchPending();  // mc.cpp: a J switch to the Minecraft player is pending (its window resizes first)
static inline float Overscan() { return McModeActive() || McSwitchPending() ? g_mcOverscan : g_overscan; }
float CompositeOverscan() { return g_enabled ? Overscan() : 1.0f; }
// v15 (DESIGN-mcportals.md): a portal pair with both portals in the region is drawn by Minecraft
// itself, into its own frame; every region view then draws that one image.
static bool g_mcPortals = true;
static bool g_pairInMc = false;          // this frame
static int g_mcPortalRd = 12;            // portal passes' render distance (chunks)
static float g_mcPortalRes = 1.0f;       // portal passes' target scale
static int g_mcPortalDepth = 2;          // recursion (Portal 2's r_portal_stencil_depth)
// Portal 2's visible opening when fully open: measured on Portal 2's own rendering (pcb_measure_hole, run
// mcp6: the pixels of its portal views inside the rim, both colours, 3 blocks straight on and at 45 deg):
// 27 x 47 u half extents (0.84 of the 32 x 56 u portal quad; the rim's animated inner edge varies 0.80-0.87)
// Portal 2's own opening, exactly: its stencil-hole pass (PortalRefract stage 1) keeps the pixels of the 32 x 56 u
// portal quad with |p| <= s^2, p the quad position scaled to [-1, 1] on both axes and s = smoothstep(open amount)
// (3a^2 - 2a^3); everything Portal 2 shows of a portal's view lies inside that ellipse and its rim (particles,
// the refracting edge pass, the static overlay) is drawn over its outer part. So Minecraft's opening is that
// ellipse at that scale: the image then reaches exactly as far as Portal 2's own view would, under the same rim.
// (#7's 27 x 47 was the visible inner edge of the animated rim: the view stopped short of it, a gap at the top.)
static float g_openW = 29.1f, g_openH = 50.3f;  // (set from edge5: the stencil ellipse is not the full 32 x 56 quad; see the README row)
static double g_portalMovedT[2] = {-1e9, -1e9};
static Vector g_portalLastPos[2];
static bool g_portalLastSet[2] = {false, false};
static float g_portalOpen[2] = {1, 1};
static float g_portalOpenScale[2] = {1, 1};  // the opening scale sent to Minecraft (the open curve, predicted to the image's display)
static int g_pairFrames = 0, g_pairViews = 0, g_pairRemaps = 0;
static int g_pairDebug = 0;  // 1: pair views without the depth test, 2: at the main view's constant background depth
static bool g_occlPropsMain = false;  // props' occlusion layers in the main view outside pair mode too
  // mc.cpp sizes Minecraft's frame by it

// ---- textures, streams --------------------------------------------------------------------------
static const int kTexW = 2048, kTexH = 1024;  // the largest image: the view x overscan (1760 x 990 at 1600 x 900)

// Minecraft's frame for a sw x sh screen: the screen x overscan, scaled down (same shape) until it fits kTexW x kTexH. mc.cpp sends it,
// the composite crops it and the HUD expects it: one rule for all three. (2026-10-12: only the width was capped, twice; a 2560 x 1440
// screen gave 2048 x 1152, taller than the textures, and raising the cap in mc.cpp alone smeared every Minecraft view.)
// The automatic render scale (2026-10-12, the user: "everything like that needs to be calculated dynamically ... has to look good"): Minecraft's
// world is a picture rendered apart and reprojected to Portal 2's camera every Portal 2 frame; the older it is, the more a fast turn shows
// its stretched edge and a fast move smears block edges. Every second RenderScaleUpdate compares the frames Minecraft delivered (its frame
// numbers) with Portal 2's own, and steps the scale of Minecraft's frame down 0.1 (to kMinScale) after 2 short seconds, up 0.05 after 5
// good ones; a step up that is undone within 6 s holds the scale for 30 s (no seesaw). "Short" is below 85% of the wanted rate: Portal 2's
// rate, at most pc_mc_target_fps (90). pc_mc_render_scale <0.5..1> fixes it, "auto" (the default) lets it move.
static const float kMinScale = 0.5f;
static std::atomic<float> g_renderScale{1.0f};
static float g_scaleFixed = 0;  // 0: auto
static int g_targetFps = 90;
void McFrameSize(int sw, int sh, float overscan, int &w, int &h) {
	w = (int)std::lround(sw * overscan), h = (int)std::lround(sh * overscan);
	if (w > kTexW || h > kTexH) {
		const double s = std::min((double)kTexW / w, (double)kTexH / h);
		w = std::max(1, (int)(w * s)), h = std::max(1, (int)(h * s));
	}
	const float rs = g_renderScale.load(std::memory_order_relaxed);
	if (rs < 0.999f) w = std::max(64, (int)std::lround(w * rs)), h = std::max(36, (int)std::lround(h * rs));
}
static const int kStreams = 6;  // one per distinct region camera (MC<->MC pairs: main + 2 x depth 2)
static IMaterial *g_matBlack = nullptr;
static IMaterial *g_matMagenta = nullptr;  // (debug: Portal 2's own portal-view pixels, for measuring its opening)
static IMaterial *g_matStatic[2] = {};  // a portal's own colour (Portal 2's look for a view it cannot show)
static Interface *g_engineVgui = nullptr;

struct ReqInfo {
	uint32_t id = 0;
	int stream = -1;
	uint32_t camGen = 0;
	uint64_t frame = 0;  // host frame of the request
	Vector origin;       // the (predicted) camera Minecraft renders
	QAngle angles;
	float tanX = 1, tanY = 1;            // the request frustum at distance 1
	float crop[4] = {-1, -1, 1, 1};      // the rendered window of that frustum (NDC)
	int clip = -1;
	float openScale[2] = {0, 0};         // v15: the opening scale Minecraft drew each pair portal at (0 = not drawn)
	Vector openPos[2];                   // ... and where
};
static ReqInfo g_req[128];
static uint32_t g_requestId = 0;
static ReqInfo *Req(uint32_t id) { return g_req[id % 128].id == id && id ? &g_req[id % 128] : nullptr; }

// A texture (plain and sRGB views of the same pixels) with its materials. Each stream has two sizes:
// the material system's Download converts and uploads the whole texture whatever the rectangle
// (measured ~4 ms after the regeneration for 2048 x 1024, independent of the image size), so a crop
// that fits goes into the small one.
static const int kSmallW = 1024, kSmallH = 512;
struct TexSet {
	int w = kTexW, h = kTexH;
	ITexture *tex = nullptr, *texSrgb = nullptr;
	IMaterial *mat = nullptr, *matSrgb = nullptr;
	IMaterial *matTint = nullptr;  // debug (pair views, pcb_mc_portals_shape ... 5): the image x [0 1 0], for pcb_measure_gap
	IMaterial *occl[256] = {};  // [a]: alpha test passes texels with alpha >= a (8-bit)
};
struct Stream {
	int index = 0;
	TexSet set[2];
	int drawSet = 0;  // the set the newest queued upload writes (what this frame's draws sample)
	int upSet = 0;    // render thread: the set the current Download writes
	double regenT0 = 0, regenT1 = 0;  // render thread: the regeneration inside the current Download
	// the camera this stream follows (Minecraft-camera continuity)
	bool camValid = false;
	uint32_t camGen = 0;  // bumped when the stream takes a new (discontinuous) camera
	Vector camO, camVel;
	QAngle camA, prevA;
	bool havePrevA = false;
	float angVel[3] = {0, 0, 0};  // degrees a second (pitch, yaw, roll), smoothed over kPredictTauS
	uint64_t prevAFrame = 0;
	uint64_t camFrame = 0;
	int camClip = -1;
	int camDepth = 0;  // Portal 2's recursion level of its camera (0 the camera, 1 a portal view, 2+ recursion)
	float camBox[4] = {-1, -1, 1, 1};  // its screen box (portal views)
	float latencyFrames = 2.0f;
	int newCams = 0;
	uint64_t genFrame = 0;  // the host frame the current camera chain started
	// the newest image (texture contents once its upload has run)
	bool haveImage = false;
	ReqInfo img;           // the request it answers
	int imgW = 0, imgH = 0;
	bool depthAlpha = false;
	float proxyDepth = 9000;  // Source units: the plane used for position reprojection
	uint64_t imageArrived = 0;
	// The image in our memory, in the texture's own layout (rows top-down, stride kTexW, the texture's
	// channel order): a worker thread copies it out of the shared-memory slot, the render thread's
	// texture regeneration copies it into the texture, and the frame probes read it. Two buffers:
	// the worker fills one while the other may still be uploading.
	uint8_t *buf[2] = {};
	uint32_t bufReq[2] = {};
	std::atomic<uint32_t> bufDone[2]{};  // request id the worker finished writing into buf[i]
	std::atomic<int> inflight[2]{};      // uploads queued from buf[i] and not yet run
	int next = 0, cur = -1;              // the buffer the next image goes to / the newest image's buffer
	int upBuf = 0;                       // the buffer the queued upload reads (render thread)
	uint32_t upReq = 0;
	int upW = 0, upH = 0;
	Rect_t upRect{};
	ITexture *upTex[2] = {};
	int skippedBusy = 0;
	uint64_t heldFrame = 0;  // the host frame its image was last drawn held (not reprojected)
	uint64_t reqFrame = 0;   // the host frame it was last requested (the secondary slot round-robin)
	bool small = false;      // a secondary stream (portal and recursion views): 1024 x 512 at most, no occlusion layers
};
// streams 0-2 hold full-screen images (the camera's own view needs one, with occlusion layers); the
// others serve MC<->MC portal views and their recursion with crops of at most 1024 x 512 (~12 MB each
// in the game's 32-bit address space instead of ~36 MB)
static const int kBigStreams = 3;  // the camera + both portal views of an MC<->MC pair (a crossing keeps its stream)
static int g_vpW = 1760, g_vpH = 990;  // Minecraft's frame size for a host view (the screen x overscan)
static Stream g_streams[kStreams];

// RGBA (Minecraft's readback) -> the texture's format, rows flipped (the slot is bottom-up).
__attribute__((target("ssse3"))) static void SwizzleRowSsse3(const uint8_t *src, uint8_t *dst, int w) {
	typedef char v16 __attribute__((vector_size(16)));
	const v16 mask = {2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15};
	int x = 0;
	for (; x + 4 <= w; x += 4) {
		v16 v;
		std::memcpy(&v, src + x * 4, 16);
		v = __builtin_ia32_pshufb128(v, mask);
		std::memcpy(dst + x * 4, &v, 16);
	}
	for (; x < w; ++x) dst[x * 4] = src[x * 4 + 2], dst[x * 4 + 1] = src[x * 4 + 1], dst[x * 4 + 2] = src[x * 4], dst[x * 4 + 3] = src[x * 4 + 3];
}
static bool g_ssse3 = false;
static ImageFormat g_texFmt = IMAGE_FORMAT_BGRA8888;  // what the textures' bits are (measured: BGRA8888)

// ---- the copy worker: shared-memory slot -> a stream's buffer ----------------------------------------
struct CopyJob {
	Stream *st;
	int buf;
	uint32_t req;
	const uint8_t *src;  // rows bottom-up, RGBA
	int w, h;
};
static std::mutex g_jobMx;
static std::condition_variable g_jobCv;
static std::vector<CopyJob> g_jobs;
PCB_GAUGE("composite.copyJobs", g_jobs.size());
static std::atomic<int> g_jobsPending{0};
static std::thread g_worker;
static bool g_workerQuit = false;
static double g_copyS = 0;
static std::atomic<int> g_copyN{0};
static void DoCopy(const CopyJob &j) {
	double t0 = NowS();
	uint8_t *dst = j.st->buf[j.buf];
	for (int y = 0; y < j.h; ++y) {
		const uint8_t *s = j.src + (size_t)(j.h - 1 - y) * j.w * 4;
		uint8_t *d = dst + (size_t)y * kTexW * 4;
		if (g_texFmt == IMAGE_FORMAT_BGRA8888) {
			if (g_ssse3) SwizzleRowSsse3(s, d, j.w);
			else
				for (int x = 0; x < j.w; ++x) d[x * 4] = s[x * 4 + 2], d[x * 4 + 1] = s[x * 4 + 1], d[x * 4 + 2] = s[x * 4], d[x * 4 + 3] = s[x * 4 + 3];
		} else {
			std::memcpy(d, s, (size_t)j.w * 4);
		}
	}
	j.st->bufDone[j.buf].store(j.req, std::memory_order_release);
	g_copyS += NowS() - t0, ++g_copyN;
}
static void WorkerMain() {
	std::unique_lock<std::mutex> lk(g_jobMx);
	for (;;) {
		g_jobCv.wait(lk, [] { return g_workerQuit || !g_jobs.empty(); });
		if (g_workerQuit) return;
		std::vector<CopyJob> jobs;
		jobs.swap(g_jobs);
		lk.unlock();
		for (auto &j : jobs) DoCopy(j), g_jobsPending.fetch_sub(1, std::memory_order_acq_rel);
		lk.lock();
	}
}
static inline float TW(const Stream &S) { return (float)S.set[S.drawSet].w; }
static inline float TH(const Stream &S) { return (float)S.set[S.drawSet].h; }
// The texel at texture coordinates (tu, tv) of the stream's newest image, as R, G, B, A.
static void ImagePx(const Stream &S, float tu, float tv, uint8_t out[4]) {
	if (S.cur < 0) return void(out[0] = out[1] = out[2] = out[3] = 0);
	int x = std::max(0, std::min(S.imgW - 1, (int)(tu * TW(S)))), y = std::max(0, std::min(S.imgH - 1, (int)(tv * TH(S))));
	const uint8_t *p = S.buf[S.cur] + ((size_t)y * kTexW + x) * 4;
	bool bgra = g_texFmt == IMAGE_FORMAT_BGRA8888;
	out[0] = bgra ? p[2] : p[0], out[1] = p[1], out[2] = bgra ? p[0] : p[2], out[3] = p[3];
}
static double g_regenS = 0;
static int g_regenN = 0, g_regenFmt = -1, g_log = 3;
class ViewRegen : public ITextureRegenerator {
public:
	Stream *st = nullptr;
	void RegenerateTextureBits(ITexture *, IVTFTexture *vtf, Rect_t *) override {
		auto t0 = NowS();
		st->regenT0 = t0;
		auto ImageFormat_fn = Memory::VMT<ImageFormat(__rescall *)(IVTFTexture *)>(vtf, Offsets::ImageFormat);
		auto ImageData = Memory::VMT<uint8_t *(__rescall *)(IVTFTexture *)>(vtf, Offsets::ImageData);
		ImageFormat fmt = ImageFormat_fn(vtf);
		uint8_t *dst = ImageData(vtf);
		const uint8_t *src = st->buf[st->upBuf];
		if (!dst || !src) return;
		// the worker's copy of this image (it finishes long before this runs; wait at most 4 ms)
		for (int i = 0; i < 4000 && st->bufDone[st->upBuf].load(std::memory_order_acquire) != st->upReq; ++i) plat::SleepUs(1);
		const int tw = st->set[st->upSet].w;
		int w = std::min(st->upW, tw), h = std::min(st->upH, st->set[st->upSet].h);
		for (int y = 0; y < h; ++y) std::memcpy(dst + (size_t)y * tw * 4, src + (size_t)y * kTexW * 4, (size_t)w * 4);
		if (fmt != g_texFmt && g_log > 0) --g_log, console->Print("[bridge] composite: texture format %d, images were prepared as %d\n", (int)fmt, (int)g_texFmt);
		g_texFmt = fmt;
		st->regenT1 = NowS();
		g_regenS += st->regenT1 - t0, ++g_regenN;
		g_regenFmt = (int)fmt;
	}
	void Release() override {}
};
static ViewRegen g_regens[kStreams];

bool CompositeHookPaint();
bool CompositeInit() {
	g_ssse3 = __builtin_cpu_supports("ssse3");
	auto CreateProceduralTexture = (ITexture * (__rescall *)(void *, const char *, const char *, int, int, ImageFormat, int)) materialSystem->materials->Original(
	    Offsets::CreateProceduralTexture);
	const int flags = TEXTUREFLAGS_NOMIP | TEXTUREFLAGS_NOLOD | TEXTUREFLAGS_SINGLECOPY | TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT;
	for (int k = 0; k < kStreams; ++k) {
		Stream &S = g_streams[k];
		S.index = k;
		g_regens[k].st = &S;
		S.small = k >= kBigStreams;
		if (S.small) S.drawSet = 1;
		const size_t rows = S.small ? kSmallH : kTexH;
		for (auto &b : S.buf) b = new uint8_t[(size_t)kTexW * rows * 4], std::memset(b, 0, (size_t)kTexW * rows * 4);
		for (int j = S.small ? 1 : 0; j < 2; ++j) {
			TexSet &T = S.set[j];
			T.w = j ? kSmallW : kTexW, T.h = j ? kSmallH : kTexH;
			char name[64], nameS[64], mname[80], mnameS[80];
			std::snprintf(name, sizeof name, "_pcb_mcview%d_%d", k, j);
			std::snprintf(nameS, sizeof nameS, "_pcb_mcview%d_%d_srgb", k, j);
			std::snprintf(mname, sizeof mname, "%s_mat", name);
			std::snprintf(mnameS, sizeof mnameS, "%s_mat", nameS);
			T.tex = CreateProceduralTexture(materialSystem->materials->ThisPtr(), name, "Portalcraft textures", T.w, T.h, IMAGE_FORMAT_RGBA8888, flags);
			if (!T.tex) return false;
			T.tex->SetTextureRegenerator(&g_regens[k], false);
			// The same pixels flagged sRGB (TEXTUREFLAGS_SRGB 0x40): sampled as display colours and
			// linearised, so the scene's sRGB write gives them back unchanged.
			T.texSrgb = CreateProceduralTexture(materialSystem->materials->ThisPtr(), nameS, "Portalcraft textures", T.w, T.h, IMAGE_FORMAT_RGBA8888, flags | 0x40);
			if (T.texSrgb) {
				T.texSrgb->SetTextureRegenerator(&g_regens[k], false);
				auto kvs = new KeyValues("UnlitGeneric");
				kvs->SetString("$basetexture", nameS);
				kvs->SetInt("$nocull", 1);
				kvs->SetInt("$nofog", 1);
				T.matSrgb = materialSystem->CreateMaterial(mnameS, kvs);
			}
			auto kv = new KeyValues("UnlitGeneric");
			kv->SetString("$basetexture", name);
			kv->SetInt("$nocull", 1);
			kv->SetInt("$nofog", 1);
			T.mat = materialSystem->CreateMaterial(mname, kv);
			{
				auto kt = new KeyValues("UnlitGeneric");
				kt->SetString("$basetexture", name);
				kt->SetString("$color", "[0 1 0]");
				kt->SetInt("$nocull", 1);
				kt->SetInt("$nofog", 1);
				char tn[96];
				std::snprintf(tn, sizeof tn, "%s_tint", name);
				T.matTint = materialSystem->CreateMaterial(tn, kt);
			}
			// occlusion layers: this texture's colour, alpha-tested against its per-pixel depth (alpha)
			for (int a = 1; a < 256 && !S.small; ++a) {
				auto ko = new KeyValues("UnlitGeneric");
				ko->SetString("$basetexture", T.texSrgb ? nameS : name);
				ko->SetInt("$nocull", 1);
				ko->SetInt("$nofog", 1);
				ko->SetInt("$alphatest", 1);
				char ref[32];
				// Portal 2's alpha test compares 8-bit alpha against an integer reference, alpha >= ref
				// (measured: a reference of (a - 0.5) / 255 passed level a - 1 too); (a + 0.25) / 255
				// gives exactly level a whether the reference is truncated or rounded
				std::snprintf(ref, sizeof ref, "%.6f", (a + 0.25f) / 255.0f);
				ko->SetString("$alphatestreference", ref);
				char nm[48];
				std::snprintf(nm, sizeof nm, "_pcb_occl%d_%d_%d", k, j, a);
				T.occl[a] = materialSystem->CreateMaterial(nm, ko);
			}
		}
	}
	auto kvb = new KeyValues("UnlitGeneric");
	kvb->SetString("$color", "[0 0 0]");
	kvb->SetInt("$nocull", 1);
	kvb->SetInt("$nofog", 1);
	g_matBlack = materialSystem->CreateMaterial("_pcb_mcview_black", kvb);
	{
		auto km = new KeyValues("UnlitGeneric");
		km->SetString("$color", "[1 0 1]");
		km->SetInt("$nocull", 1);
		km->SetInt("$nofog", 1);
		g_matMagenta = materialSystem->CreateMaterial("_pcb_mcview_magenta", km);
	}
	for (int c = 0; c < 2; ++c) {
		auto kc = new KeyValues("UnlitGeneric");
		kc->SetString("$color", c ? "[0.55 0.27 0.04]" : "[0.06 0.24 0.55]");  // orange / blue, dimmed
		kc->SetInt("$nocull", 1);
		kc->SetInt("$nofog", 1);
		g_matStatic[c] = materialSystem->CreateMaterial(c ? "_pcb_mcview_static1" : "_pcb_mcview_static0", kc);
	}
	g_workerQuit = false;
	g_worker = std::thread(WorkerMain);
	bool paint = CompositeHookPaint();
	bool ok = g_streams[0].set[1].mat && g_streams[1].set[1].mat;
	console->Print("[bridge] composite: %d streams x textures %dx%d + %dx%d, materials %s, probe hook %s, ssse3 %d\n", kStreams, kTexW, kTexH, kSmallW, kSmallH, ok ? "ok" : "FAILED", paint ? "ok" : "FAILED", g_ssse3);
	return ok;
}

void CompositeShutdown() {
	{
		std::lock_guard<std::mutex> lk(g_jobMx);
		g_workerQuit = true;
	}
	g_jobCv.notify_all();
	if (g_worker.joinable()) g_worker.join();
	if (g_engineVgui) Interface::Delete(g_engineVgui), g_engineVgui = nullptr;
	HostView off{};
	link::WriteHostView(off);
	if (g_matBlack) materialSystem->DestroyMaterial(g_matBlack), g_matBlack = nullptr;
	for (auto &S : g_streams) {
		for (TexSet &T : S.set) {
			for (auto &m : T.occl)
				if (m) materialSystem->DestroyMaterial(m), m = nullptr;
			if (T.mat) materialSystem->DestroyMaterial(T.mat), T.mat = nullptr;
			if (T.matSrgb) materialSystem->DestroyMaterial(T.matSrgb), T.matSrgb = nullptr;
			if (T.texSrgb) T.texSrgb->SetTextureRegenerator(nullptr, false), materialSystem->DestroyTexture(T.texSrgb), T.texSrgb = nullptr;
			if (T.tex) T.tex->SetTextureRegenerator(nullptr, false), materialSystem->DestroyTexture(T.tex), T.tex = nullptr;
		}
		for (auto &b : S.buf) delete[] b, b = nullptr;
	}
}

bool CompositeEnabled() { return g_enabled; }

// ---- render context (IMatRenderContext slots for Portal 2, Linux): the public SDK 2013 order up
// to LoadIdentity, checked against CMatQueuedRenderContext's vtable in materialsystem.so (slot 29
// copies a whole 4x4 matrix stack entry, slot 30 a 3x4; 20 and 31 forward to the base context's
// MatrixMode(mode) / LoadIdentity()). The order shifts by one after that (CreateStaticMesh 55).
// render-context slots: slots.hpp
enum { kMatView = 0, kMatProjection = 1, kMatModel = 10 };
// A held-back slot (Windows safe-load) does nothing and returns a zero value.
template <typename F, typename... A>
static auto RcCall(IMatRenderContext *rc, slots::Id slot, A... a) {
	using R = decltype(F(nullptr)(rc, a...));
	F f = slots::Vtbl<F>(rc, slot);
	if (!f) return R();
	return f(rc, a...);
}
using _RcVoid = void(__rescall *)(IMatRenderContext *);
using _RcInt = void(__rescall *)(IMatRenderContext *, int);
using _RcGetMatrix = void(__rescall *)(IMatRenderContext *, int, VMatrix *);
using _RcSetTM = void(__rescall *)(IMatRenderContext *, const Vector &);
#ifdef _WIN32
// MSVC passes `this` in ECX and the hidden struct-return pointer as the first stack argument; GCC does the reverse for a thiscall pointer
// that returns a struct, so the return pointer is spelled out (WINDOWS-VERIFY F2).
using _RcGetTM = Vector *(__rescall *)(IMatRenderContext *, Vector *);
static Vector RcGetTone(IMatRenderContext *rc) {
	Vector v(1, 1, 1);
	if (auto f = slots::Vtbl<_RcGetTM>(rc, slots::kRcGetToneMappingScaleLinear)) f(rc, &v);
	return v;
}
#else
using _RcGetTM = Vector(__rescall *)(IMatRenderContext *);
static Vector RcGetTone(IMatRenderContext *rc) {
	Vector v(1, 1, 1);
	if (auto f = slots::Vtbl<_RcGetTM>(rc, slots::kRcGetToneMappingScaleLinear)) v = f(rc);
	return v;
}
#endif
using _RcReadPixels = void(__rescall *)(IMatRenderContext *, int, int, int, int, unsigned char *, ImageFormat);

static VMatrix g_mainView, g_mainProj;
static bool g_haveMainMatrices = false;
static void Mul(const VMatrix &a, const VMatrix &b, VMatrix &o) {
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j) o.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
}
// World point -> normalised device coordinates of the main view (D3D: y up, z 0..1). False if behind.
static bool Project(const Vector &p, float &x, float &y, float &z) {
	VMatrix vp;
	Mul(g_mainProj, g_mainView, vp);
	float c[4];
	for (int i = 0; i < 4; ++i) c[i] = vp.m[i][0] * p.x + vp.m[i][1] * p.y + vp.m[i][2] * p.z + vp.m[i][3];
	if (c[3] <= 0.1f) return false;
	x = c[0] / c[3], y = c[1] / c[3], z = c[2] / c[3];
	return true;
}

// Portal 2 draws its 2D sky as a cube of half-size 0.577 x zFar around the camera, writing depth
// (the region's toolsskybox walls show it), so the cleared depth never survives: measured, sky pixels
// sit at NDC z ~0.9998 with zFar 28374. The background goes at 0.55 of the cube's half-size
// (~9000 u): in front of every sky pixel, behind every Portal 2 object nearer than that.
static float kFarZ = 0.9995f;
static float g_bgDepth = 9000.0f;  // the background's view depth (Source units) in the main view
static float g_farZOverride = 0;
static void BackgroundZ() {
	if (g_farZOverride > 0) return void(kFarZ = g_farZOverride);
	if (!g_haveMainMatrices) return;
	float a = g_mainProj.m[2][2], b = g_mainProj.m[2][3];
	if (std::fabs(a + 1) < 1e-7f) return;
	float zf = b / (a + 1);
	if (!(zf > 1000 && zf < 1e6f)) return;
	float d = 0.55f * 0.57735f * zf;
	g_bgDepth = d;
	kFarZ = -a + b / d;
}

// ---- Source view <-> Minecraft camera -------------------------------------------------------------
static void AngleBasis(const QAngle &a, Vector &f, Vector &r, Vector &u) {
	float sp = std::sin(a.x * (float)M_PI / 180), cp = std::cos(a.x * (float)M_PI / 180);
	float sy = std::sin(a.y * (float)M_PI / 180), cy = std::cos(a.y * (float)M_PI / 180);
	float sr = std::sin(a.z * (float)M_PI / 180), cr = std::cos(a.z * (float)M_PI / 180);
	f = Vector(cp * cy, cp * sy, -sp);
	r = Vector(-sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp);
	u = Vector(cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp);
}
// Forward + up -> angles (the inverse of AngleBasis).
static QAngle BasisAngles(const Vector &f, const Vector &u) {
	QAngle a;
	a.x = -std::asin(std::fmax(-1.0f, std::fmin(1.0f, f.z))) * 180.0f / (float)M_PI;
	a.y = std::atan2(f.y, f.x) * 180.0f / (float)M_PI;
	a.z = 0;
	Vector f0, r0, u0;
	AngleBasis(a, f0, r0, u0);
	a.z = std::atan2(Vector::DotProduct(u, r0), Vector::DotProduct(u, u0)) * 180.0f / (float)M_PI;
	return a;
}
static inline void DirToMc(const Vector &s, double out[3]) { out[0] = s.x, out[1] = s.z, out[2] = -s.y; }
static float AngleBetween(const QAngle &a, const QAngle &b) {
	Vector fa, ra, ua, fb, rb, ub;
	AngleBasis(a, fa, ra, ua);
	AngleBasis(b, fb, rb, ub);
	float d = std::fmax(-1.0f, std::fmin(1.0f, Vector::DotProduct(fa, fb)));
	return std::acos(d) * 180.0f / (float)M_PI;
}
static Vector Cross(const Vector &a, const Vector &b) { return Vector(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static Vector PortalRight(int i) { return Cross(g_portalUp[i], g_portalNormal[i]); }

// The portal transform: a pose seen through portal i comes out of its partner j (rotated half a turn
// about the portals' up axes). The virtual camera of the view through i.
static void ThroughPortal(int i, int j, const Vector &o, const QAngle &a, Vector &oo, QAngle &oa) {
	const Vector &ni = g_portalNormal[i], &ui = g_portalUp[i], &nj = g_portalNormal[j], &uj = g_portalUp[j];
	Vector ri = PortalRight(i), rj = PortalRight(j);
	auto pt = [&](const Vector &v) {
		return nj * -Vector::DotProduct(v, ni) + rj * -Vector::DotProduct(v, ri) + uj * Vector::DotProduct(v, ui);
	};
	oo = g_portalPos[j] + pt(o - g_portalPos[i]);
	Vector f, r, u;
	AngleBasis(a, f, r, u);
	oa = BasisAngles(pt(f), pt(u));
}

// (bridge #22) scene.cpp: Minecraft-lit copies of region entities (ghosts through the pair) are drawn at Portal 2's tonemap
// scale 1, as the composite draws Minecraft's own image: their colours are Minecraft's. Returns the previous scale.
Vector RenderToneScaleSet(const Vector &v) {
	IMatRenderContext *rc = materialSystem->GetRenderContext();
	if (!rc) return v;
	const Vector old = RcGetTone(rc);
	RcCall<_RcSetTM>(rc, slots::kRcSetToneMappingScaleLinear, v);
	RcCall<_RcVoid>(rc, slots::kRcRelease);
	return old;
}

// (bridge #27) bandlight.cpp's hand light: Portal 2's current tonemap scale (the placed blocks' vertex light is multiplied by it)
Vector RenderToneScaleGet() {
	IMatRenderContext *rc = materialSystem->GetRenderContext();
	if (!rc) return Vector(1, 1, 1);
	const Vector v = RcGetTone(rc);
	RcCall<_RcVoid>(rc, slots::kRcRelease);
	return v;
}

// (bridge #20) the same transform for mc.cpp: the Minecraft player's eye that has passed a portal's plane, inside
// its opening, before Portal 2 has carried the body through (Minecraft leads by a tick)
void PortalThroughPose(int i, int j, const Vector &o, const QAngle &a, Vector &oo, QAngle &oa) { ThroughPortal(i, j, o, a, oo, oa); }
Vector PortalRightAxis(int i) { return PortalRight(i); }

// A camera behind a region portal's plane looks out of that portal (its clip plane): within a wide
// cone behind the opening. -1: none (a camera in the open air of the region).
static int PortalBehind(const Vector &o) {
	int best = -1;
	float bestD = 1e30f;
	for (int i = 0; i < 2; ++i) {
		if (!g_portalSet[i] || !ClipRegionContains(g_portalPos[i])) continue;
		const Vector &n = g_portalNormal[i], &c = g_portalPos[i];
		float d = Vector::DotProduct(o - c, n);
		if (d >= -0.5f || d < -6000.0f) continue;
		Vector q = o - c - n * d;
		if (q.Length() > std::fmax(96.0f, 6.0f * -d)) continue;
		float score = -d + 0.5f * q.Length();
		if (score < bestD) bestD = score, best = i;
	}
	return best;
}
// The main camera just behind a region portal, inside its opening (the frame on which Chell comes out
// of it is rendered from her eye there): that portal's plane clips its view.
static int InOpeningBehind(const Vector &o) {
	for (int i = 0; i < 2; ++i) {
		if (!g_portalSet[i] || !ClipRegionContains(g_portalPos[i])) continue;
		const Vector &n = g_portalNormal[i], &c = g_portalPos[i];
		float d = Vector::DotProduct(o - c, n);
		if (d >= 0 || d < -96.0f) continue;
		Vector q = o - c - n * d, u = g_portalUp[i], r = PortalRight(i);
		float lx = Vector::DotProduct(q, r) / 40.0f, ly = Vector::DotProduct(q, u) / 72.0f;
		if (lx * lx + ly * ly <= 1.0f) return i;
	}
	return -1;
}

// ---- the pair drawn by Minecraft (v15) --------------------------------------------------------------
// Portal 2's open animation: 0 -> 1 in 0.5 s from the portal's (re)placement (portal-rendering.md 2.1).
// The plugin sees placements through the server entities (clip.cpp polls them every 4 ticks).
// Portal 2's own open amount of each portal colour: the value its PortalOpenAmount material proxy put on
// the colour's static-overlay material the last time a portal of that colour was drawn (the value the
// portal shaders use). IMaterial::FindVar is vtable slot 11; IMaterialVar keeps its float at +12 (the
// public Source SDK layouts).
static void *g_openVar[2] = {nullptr, nullptr};
static bool g_openVarTried = false;
static float ReadOpenVar(int i) {
	if (!g_openVarTried) {
		g_openVarTried = true;
		for (int c = 0; c < 2; ++c) {
			IMaterial *m = materialSystem->FindMaterial(c ? "models/portals/portalstaticoverlay_2" : "models/portals/portalstaticoverlay_1", "Model textures");
			if (!m) continue;
			using _FindVar = void *(__rescall *)(IMaterial *, const char *, bool *, bool);
			bool found = false;
			void *v = slots::VCall<void *>(m, slots::kMaterialFindVar, "$PortalOpenAmount", &found, false);
			if (found && v) g_openVar[c] = v;
		}
		console->Print("[bridge] composite: Portal 2's open amount read from its materials: blue %s, orange %s\n", g_openVar[0] ? "ok" : "no", g_openVar[1] ? "ok" : "no");
	}
	if (!g_openVar[i]) return -1;
	float f = *(const float *)((const uint8_t *)g_openVar[i] + 12);
	return f == f && f >= 0 && f <= 1.001f ? f : -1;
}
static float g_openRead[2] = {-1, -1};
static bool g_awaitReset[2] = {false, false};
static int g_pairLog = 64;  // placed again: P2's open amount has not been seen restart yet
// Portal 2's visible opening (its rim's inner edge) against its open amount, measured frame by frame on its
// own portals (run mcp8, pcb_measure_open; the scale of the fully open 27 x 47 u opening): nothing until
// ~0.47, then it grows almost linearly and settles by ~0.95. (Each sample's hole is the frame before its
// open amount, so the table lags Portal 2 by about a frame: it never leads.)
static int g_openCurveMode = 0;  // 0: smoothstep(open)^2 (the shader's growth), 1: #7's measured curve of the visible inner edge
static float OpenCurveMeasured(float o);
static float OpenCurve(float o) {
	if (g_openCurveMode == 1) return OpenCurveMeasured(o);
	o = std::fmax(0.0f, std::fmin(1.0f, o));
	const float sm = o * o * (3 - 2 * o);
	return sm * sm;
}
static float OpenCurveMeasured(float o) {  // (#7's measured curve of the visible inner edge, kept for reference)
	static const float k[][2] = {{0.45f, 0.0f},    {0.484f, 0.055f}, {0.517f, 0.115f}, {0.55f, 0.17f},  {0.587f, 0.27f}, {0.62f, 0.355f}, {0.653f, 0.455f},
	                             {0.688f, 0.53f},  {0.72f, 0.65f},   {0.753f, 0.72f},  {0.789f, 0.79f}, {0.824f, 0.865f}, {0.859f, 0.935f}, {0.898f, 0.97f},
	                             {0.931f, 0.99f},  {0.964f, 1.0f},   {1.0f, 1.0f}};
	const int n = sizeof k / sizeof k[0];
	if (o <= k[0][0]) return 0.0f;
	for (int i = 1; i < n; ++i)
		if (o <= k[i][0]) return k[i - 1][1] + (k[i][1] - k[i - 1][1]) * (o - k[i - 1][0]) / (k[i][0] - k[i - 1][0]);
	return 1.0f;
}
static double MeanFrameS();
static int g_openPredFrames = 2;  // frames ahead the opening is predicted (0: Portal 2's last value; images are on screen 2+ frames after their request, so the image's opening trails Portal 2's by those frames and never leads it)
static void UpdatePair() {
	const double now = NowS();
	for (int i = 0; i < 2; ++i) {
		// placed somewhere else (a set/unset change alone is not a placement: Portal 2 reports a portal that is
		// still opening as not activated)
		const bool moved = g_portalSet[i] && g_portalLastSet[i] && (g_portalPos[i] - g_portalLastPos[i]).Length() > 1.0f;
		const bool appeared = g_portalSet[i] && !g_portalLastSet[i];
		if ((moved || appeared || (!g_portalSet[i] && g_portalLastSet[i])) && g_pairLog > 0)
			--g_pairLog, console->Print("[bridge] composite: portal %d %s at (%.1f %.1f %.1f), Portal 2 open %.3f\n", i, moved ? "moved" : appeared ? "activated" : "deactivated", g_portalPos[i].x,
			                           g_portalPos[i].y, g_portalPos[i].z, ReadOpenVar(i));
		if (moved || (appeared && now - g_portalMovedT[i] > 1.0)) g_portalMovedT[i] = now;
		if (moved) g_awaitReset[i] = true;
		g_portalLastSet[i] = g_portalSet[i], g_portalLastPos[i] = g_portalPos[i];
		// (the plugin's own clock since it saw the portal move: only a bound while Portal 2's value catches up)
		g_portalOpen[i] = g_portalSet[i] ? (float)std::fmin(1.0, std::fmax(0.0, 2.0 * (now - g_portalMovedT[i]))) : 0.0f;
		// Portal 2's own open amount for the portal (its material, set when Portal 2 last drew it)
		const float var = InStageBox(g_portalPos[i]) ? -1.0f : ReadOpenVar(i);  // (bridge #49: Portal 2 never draws a portal in the virtual region's void, so its material never opens: the plugin's own clock)
		g_openRead[i] = var;
		if (g_awaitReset[i] && ((var >= 0 && var < 0.99f) || now - g_portalMovedT[i] > 0.6)) g_awaitReset[i] = false;
		float open = var >= 0 ? var : g_portalOpen[i];
		if (g_awaitReset[i]) open = std::fmin(open, g_portalOpen[i]);  // the material still holds the old portal's value
		// predicted for when this request's image is on screen (Portal 2 opens at 2 per second)
		const float pred = std::fmin(1.0f, open + 2.0f * (float)MeanFrameS() * (float)g_openPredFrames - 0.015f);
		g_portalOpenScale[i] = g_portalSet[i] ? OpenCurve(pred) : 0.0f;
	}
	g_pairInMc = g_mcPortals && g_portalSet[0] && g_portalSet[1] && ClipRegionContains(g_portalPos[0]) && ClipRegionContains(g_portalPos[1]);
	if (g_pairInMc) ++g_pairFrames;
}
static Vector g_ovOrigin;  // (declared here: WritePortals reads it)
static bool g_stageFlat = false;  // (bridge #74) experiment: the old flat-colour opening for a portal in the stage
static bool g_mcFlatDebug = false;
static float g_openUpOff = 1.36f;  // the opening's centre along the portal's up axis (Source units at full open)  // pcb_mc_portals_flat: Minecraft fills its openings with flat magenta (edge measurements)
static void WritePortals(uint32_t requestId) {
	HostPortals hp{};
	hp.primaryRequestId = requestId;
	hp.flags = g_pairInMc ? kPortalsInMc : 0u;
	hp.maxDepth = g_mcFlatDebug ? 0u : (uint32_t)g_mcPortalDepth;
	// (bridge #49) the camera is in the virtual region (stage.cpp): Portal 2 does not draw anything that stands in its void (an entity in no leaf is never drawn,
	// the way-home portal included), so Minecraft draws that portal's opening in its own frame, as a flat colour (Portal 2's static look: what the chamber behind
	// it looks like is not shown); the portal in the chamber is not asked for (nothing there to see from here)
	const bool stageCam = g_stageFlat && g_map.stage.on && InStageBox(g_ovOrigin);
	if (stageCam) hp.flags = kPortalsInMc, hp.maxDepth = 0u;
	hp.renderDistance = (uint32_t)g_mcPortalRd;
	hp.resScale = g_mcPortalRes;
	// flat colours past the recursion limit: Portal 2's static look, as display values (the materials'
	// linear [0.06 0.24 0.55] / [0.55 0.27 0.04])
	static const uint32_t colour[2] = {0x4785C2, 0xC28D3B};
	for (int i = 0; i < 2; ++i) {
		HostPortal &o = hp.portal[i];
		SrcToMcD(g_portalPos[i] + g_portalUp[i] * (g_openUpOff * g_portalOpenScale[i]), o.cx, o.cy, o.cz);
		double n[3], u[3];
		DirToMc(g_portalNormal[i], n);
		DirToMc(g_portalUp[i], u);
		o.nx = (float)n[0], o.ny = (float)n[1], o.nz = (float)n[2];
		o.ux = (float)u[0], o.uy = (float)u[1], o.uz = (float)u[2];
		const float sc = g_portalOpenScale[i];
		o.halfW = g_openW * sc / (float)kUnitsPerBlock, o.halfH = g_openH * sc / (float)kUnitsPerBlock;
		if (stageCam && !InStageBox(g_portalPos[i])) o.halfW = o.halfH = 0.0f;
		if (ReqInfo *q = Req(requestId)) q->openScale[i] = g_pairInMc ? sc : 0.0f, q->openPos[i] = g_portalPos[i];
		o.open = g_portalOpen[i];
		o.colour = g_mcFlatDebug ? 0xFF00FFu : colour[i];
	}
	link::WriteHostPortals(hp);
}
// A camera that crossed a portal of the pair is the same Minecraft camera on the other side: the
// stream that followed it is carried through the portal (its pose, its image's pose and its pending
// requests), so the image rendered just before the crossing (whose opening fills the view there)
// reprojects into the first frames after it instead of a new camera starting from nothing.
static void CarryThrough(int i, Vector &o, QAngle &a) {
	Vector oo;
	QAngle oa;
	ThroughPortal(i, 1 - i, o, a, oo, oa);
	o = oo, a = oa;
}
// ---- host frame clock -------------------------------------------------------------------------------
static uint64_t g_hostFrame = 0, g_frames = 0;
static double g_lastFrameT = 0, g_frameSum = 0, g_frameMax = 0;
static int g_frameN = 0;
static double g_frameEma = 0;  // (bridge #53) a running mean of the last ~64 frames: the run-wide mean drifted to the unfocused 54 ms frames and stayed there
static double MeanFrameS() { return g_frameN > 30 && g_frameEma > 0 ? std::fmin(0.05, std::fmax(0.002, g_frameEma)) : 1.0 / 120; }
// Run-wide frame-time histogram (lead: performance tracked in every test run, printed with the RUN SUMMARY;
// never reset by pcb_composite_lag). 0.25 ms bins to 100 ms; frames over 1 s (loading) are left out.
static uint32_t g_runHist[401];
static uint64_t g_runN = 0;
static double g_runSum = 0, g_runMax = 0;
static void CountFrame() {
	double t = NowS();
	if (g_lastFrameT > 0) {
		double dt = t - g_lastFrameT;
		g_frameSum += dt, ++g_frameN;
		if (dt < 0.25) g_frameEma = g_frameEma > 0 ? g_frameEma + (dt - g_frameEma) * (1.0 / 64.0) : dt;  // (a stall of 0.25 s or more is not a frame time)
		GrowthDt(dt);
		if (dt > g_frameMax) g_frameMax = dt;
		if (dt < 1.0) {
			void ProfFrameEnd(double dtMs);  // prof.cpp (bridge #25): the scopes timed in this frame; spikes over 20 ms logged
			ProfFrameEnd(dt * 1000.0);
			++g_runHist[std::min(400, (int)(dt * 4000.0))];
			++g_runN, g_runSum += dt;
			if (dt > g_runMax) g_runMax = dt;
		}
	}
	g_lastFrameT = t;
}
static double RunPercentileMs(double q) {
	uint64_t want = (uint64_t)(q * g_runN), acc = 0;
	for (int i = 0; i <= 400; ++i)
		if ((acc += g_runHist[i]) > want) return (i + 0.5) * 0.25;
	return 100.0;
}
void CompositeRunPerf() {
	if (!g_runN) return;
	uint64_t over12 = 0, over20 = 0;
	for (int i = 48; i <= 400; ++i) over12 += g_runHist[i];
	for (int i = 80; i <= 400; ++i) over20 += g_runHist[i];
	const double mean = 1000 * g_runSum / g_runN;
	const char *RunFocusNote();  // bugmark.cpp (bridge #24): the window's SDL focus over the run
	console->Print("[bridge] RUN PERF: %llu frames, mean %.2f ms (%.0f fps), p50 %.2f, p95 %.2f, p99 %.2f, worst %.1f ms, %llu over 12 ms, %llu over 20 ms%s%s\n",
	               (unsigned long long)g_runN, mean, 1000.0 / mean, RunPercentileMs(0.5), RunPercentileMs(0.95), RunPercentileMs(0.99), 1000 * g_runMax,
	               (unsigned long long)over12, (unsigned long long)over20, mean > 40 ? " (THROTTLED? window not focused: timing not valid)" : "", RunFocusNote());
	void ProfRunReport();  // prof.cpp (bridge #25)
	ProfRunReport();
}

// ---- this frame's Minecraft cameras ------------------------------------------------------------------
// A camera that can see the region this frame, predicted from the main camera (OverrideView).
struct Cand {
	Vector o;          // the camera, as Portal 2 will render it this frame
	QAngle a;
	int clip = -1;     // the region portal it looks out of (its clip plane)
	float box[4] = {-1, -1, 1, 1};  // its pixels on screen (NDC of the main view), clipped to the screen
	float prio = 0;    // screen share; 2 = the camera itself; 0 = not on screen (kept warm)
	int stream = -1;
	uint32_t req = 0;
	const char *what = "";
	bool skipReq = false;  // not requested this frame (the second slot is throttled)
	int depth = 0;     // Portal 2's recursion level of the view (0 the camera, 1 a portal view, 2 a view in a portal view)
};
static const int kMaxCands = 8;
static Cand g_cands[kMaxCands];
static int g_nCands = 0;
static QAngle g_ovAngles;
static float g_ovTanX = 1.3333f, g_ovTanY = 0.75f;

// Screen box of portal i's opening seen from the main camera: false if it is off screen. A portal
// with any point behind the camera covers the whole screen (the camera is at or in it).
static bool PortalBox(int i, const Vector &o, const Vector &f, const Vector &r, const Vector &u, float box[4]) {
	const Vector c = g_portalPos[i], n = g_portalNormal[i], pu = g_portalUp[i], pr = PortalRight(i);
	if (Vector::DotProduct(o - c, n) <= 0) return false;  // behind the portal: it shows nothing here
	float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
	for (int k = 0; k < 24; ++k) {
		float t = 2 * (float)M_PI * k / 24;
		Vector p = c + pr * (34.0f * std::cos(t)) + pu * (58.0f * std::sin(t)) - o;
		float z = Vector::DotProduct(p, f);
		if (z < 1.0f) {
			box[0] = box[1] = -1, box[2] = box[3] = 1;
			return true;
		}
		float x = Vector::DotProduct(p, r) / (z * g_ovTanX), y = Vector::DotProduct(p, u) / (z * g_ovTanY);
		x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
	}
	box[0] = std::max(-1.0f, x0), box[1] = std::max(-1.0f, y0), box[2] = std::min(1.0f, x1), box[3] = std::min(1.0f, y1);
	return box[0] < box[2] && box[1] < box[3];
}

// Every camera Portal 2 renders this frame that looks into the region: the main camera, and the
// virtual camera behind each visible portal, recursively up to Portal 2's stencil depth (2), as
// Portal 2 does: a view through portal i comes out of its partner j, and inside that view the other
// portals visible from that camera recurse again. A camera outside the region is not requested but
// still recurses (a chamber camera sees the region through a chamber portal). A portal is only seen
// from its own side of the region boundary (the chamber's walls hide the region and vice versa).
static int g_recursion = 2;
struct Node {
	Vector o;
	QAngle a;
	int clip;        // the portal plane this camera sits behind (-1: the main camera)
	float box[4];    // its pixels on screen (main view NDC)
	int depth;
};
// bridge #18 (tests): for a few frames the main camera is requested from Minecraft even outside the region, so Minecraft's
// own view of the chamber band at the player's pose can be saved (pcb_mc_native); Portal 2 does not draw it.
static int g_nativeMainFrames = 0;
void CompositeNativeMain(int frames) { g_nativeMainFrames = frames; }
static void BuildCandidates(const CViewSetup *main) {
	g_nCands = 0;
	const bool nativeMain = g_nativeMainFrames > 0;
	if (nativeMain) --g_nativeMainFrames;
	Node nodes[16];
	int nn = 0;
	nodes[nn++] = Node{g_ovOrigin, g_ovAngles, -1, {-1, -1, 1, 1}, 0};
	int warm = -1;
	float warmD = 1e30f;
	// (bridge #45) a Minecraft image only ever goes into a portal that opens onto the Minecraft REGION (pit flings, the testbed). A campaign chamber (the mirror
	// world) has no region: its gun portals are Portal 2's alone (it draws both ends and what is seen through them), and the portal slots (clip.cpp, tracked on
	// every map since #42, for the Minecraft player's transit) must not make the composite chase views through them (nothing of Minecraft's to show there).
	const bool pairsMatter = ClipHasRegion();
	for (int q = 0; q < nn; ++q) {
		const Node nd = nodes[q];
		const bool in = ClipRegionContains(nd.o);
		if ((in || (nativeMain && nd.depth == 0)) && g_nCands < kMaxCands) {
			Cand &c = g_cands[g_nCands++];
			c = Cand{};
			c.o = nd.o, c.a = nd.a;
			c.clip = nd.depth == 0 ? InOpeningBehind(nd.o) : nd.clip;
			std::memcpy(c.box, nd.box, sizeof c.box);
			c.prio = nd.depth == 0 ? 2.0f : (nd.box[2] - nd.box[0]) * (nd.box[3] - nd.box[1]) / 4 / (float)nd.depth;
			c.what = nd.depth == 0 ? "camera" : nd.depth == 1 ? "portal view" : "recursion view";
			c.depth = nd.depth;
		}
		if (nd.depth >= g_recursion) continue;
		Vector f, r, u;
		AngleBasis(nd.a, f, r, u);
		for (int i = 0; i < 2 && !g_pairInMc && pairsMatter; ++i) {  // (a pair inside Minecraft: Minecraft draws its views)
			const int j = 1 - i;
			if (!g_portalSet[i] || !g_portalSet[j] || i == nd.clip) continue;
			if (ClipRegionContains(g_portalPos[i]) != in) continue;
			float box[4];
			if (!PortalBox(i, nd.o, f, r, u, box)) {
				float d = (g_portalPos[i] - nd.o).Length();
				if (nd.depth == 0 && !in && ClipRegionContains(g_portalPos[j]) && d < warmD) warmD = d, warm = i;
				continue;
			}
			// inside the parent's opening on screen
			box[0] = std::max(box[0], nd.box[0]), box[1] = std::max(box[1], nd.box[1]);
			box[2] = std::min(box[2], nd.box[2]), box[3] = std::min(box[3], nd.box[3]);
			if (box[0] >= box[2] || box[1] >= box[3] || nn >= 16) continue;
			Node &ch = nodes[nn++];
			ThroughPortal(i, j, nd.o, nd.a, ch.o, ch.a);
			ch.clip = j, ch.depth = nd.depth + 1;
			std::memcpy(ch.box, box, sizeof box);
		}
	}
	if (g_nCands == 0 && warm >= 0) {
		// nothing of the region on screen: keep the view through the nearest such portal rendering,
		// so it has an image (and a continuous stream) the moment it comes into view
		Cand &c = g_cands[g_nCands++];
		c = Cand{};
		ThroughPortal(warm, 1 - warm, g_ovOrigin, g_ovAngles, c.o, c.a);
		c.clip = 1 - warm, c.prio = 0, c.what = "warm portal view", c.depth = 1;
	}
	std::stable_sort(g_cands, g_cands + g_nCands, [](const Cand &a, const Cand &b) { return a.prio > b.prio; });
	if (g_nCands > std::min(g_maxCams, kStreams)) g_nCands = std::min(g_maxCams, kStreams);
}

// ---- streams keyed by Minecraft-camera continuity ---------------------------------------------------
// Each camera goes to the stream whose camera was nearest last time, within 2 blocks plus its own
// motion, and 45 deg, per frame of gap; else a new camera takes the stream unused longest. So the
// view through the pit portal before Chell falls in and her own camera after it are one stream.
static int g_streamLog = 0;
static float PosTol(const Stream &st, uint64_t gap) { return (80.0f + 1.5f * st.camVel.Length() * (float)MeanFrameS()) * (float)gap; }
static int AssignStream(const Cand &c, bool taken[kStreams]) {
	int best = -1;
	float bestScore = 1e30f;
	const bool needBig = c.clip < 0;  // a camera that is not a portal view: full screen, occlusion layers
	for (int k = 0; k < kStreams; ++k) {
		Stream &st = g_streams[k];
		if (taken[k] || !st.camValid || (needBig && st.small)) continue;
		uint64_t gap = g_hostFrame - st.camFrame;
		if (gap < 1 || gap > 30) continue;
		float d = (c.o - st.camO).Length(), ang = AngleBetween(c.a, st.camA);
		// position decides: a camera that crosses a portal can turn abruptly (Portal 2 renders Chell's
		// camera after a floor-to-wall transit up to ~80 deg off the portal view's camera just before),
		// so the angle limit only applies to cameras that are not clearly the same point
		bool samePoint = d <= 60.0f * gap;
		if (d > PosTol(st, gap) || (!samePoint && ang > 45.0f * gap)) continue;
		float score = d + (samePoint ? 0.0f : 4 * ang);
		if (score < bestScore) bestScore = score, best = k;
	}
	if (best < 0 && g_pairInMc && c.depth == 0) {
		// the camera crossed a portal of a pair Minecraft draws: the stream it had goes with it
		for (int k = 0; k < kStreams && best < 0; ++k) {
			Stream &st = g_streams[k];
			if (taken[k] || !st.camValid || st.small) continue;
			uint64_t gap = g_hostFrame - st.camFrame;
			if (gap < 1 || gap > 30) continue;
			for (int i = 0; i < 2 && best < 0; ++i) {
				if (std::fabs(Vector::DotProduct(st.camO - g_portalPos[i], g_portalNormal[i])) > 160.0f) continue;
				Vector o = st.camO;
				QAngle a = st.camA;
				CarryThrough(i, o, a);
				if ((c.o - o).Length() > PosTol(st, gap)) continue;
				Vector v0 = g_portalPos[i] + st.camVel, vz = g_portalPos[i];
				QAngle dummy{0, 0, 0};
				CarryThrough(i, v0, dummy), CarryThrough(i, vz, dummy);
				st.camVel = v0 - vz;
				st.camO = o, st.camA = a;
				if (st.havePrevA) {
					Vector po = g_portalPos[i];
					CarryThrough(i, po, st.prevA);
				}
				if (st.haveImage && st.img.camGen == st.camGen) CarryThrough(i, st.img.origin, st.img.angles);
				for (auto &q : g_req)
					if (q.id && q.stream == k && q.camGen == st.camGen && q.frame + 64 > g_hostFrame) CarryThrough(i, q.origin, q.angles);
				++g_pairRemaps;
				if (g_streamLog > 0) --g_streamLog, console->Print("[bridge] composite: camera crossed portal %d: stream %d carried through\n", i, k);
				best = k;
			}
		}
	}
	if (best < 0) {
		best = -1;
		// a new camera: the stream unused longest. The camera and portal views (which become the camera
		// when Chell crosses) take full-screen streams first; recursion views take secondary ones first.
		const bool preferSmall = c.depth >= 2;
		for (int pass = 0; pass < 2 && best < 0; ++pass)
			for (int k = 0; k < kStreams; ++k) {
				if (taken[k] || (needBig && g_streams[k].small) || (pass == 0 && g_streams[k].small != preferSmall)) continue;
				if (best < 0 || !g_streams[k].camValid || (g_streams[best].camValid && g_streams[k].camFrame < g_streams[best].camFrame)) best = k;
			}
		if (best < 0) return -1;
		Stream &st = g_streams[best];
		++st.camGen, ++st.newCams;
		st.genFrame = g_hostFrame;
		st.camVel = Vector(0, 0, 0);
		st.havePrevA = false;
		st.angVel[0] = st.angVel[1] = st.angVel[2] = 0;
		if (g_streamLog > 0) --g_streamLog, console->Print("[bridge] composite: new camera (%s) at (%.0f %.0f %.0f) -> stream %d\n", c.what, c.o.x, c.o.y, c.o.z, best);
	} else {
		Stream &st = g_streams[best];
		float dt = (float)((g_hostFrame - st.camFrame) * MeanFrameS());
		if (dt > 1e-4f) {
			const float k = 1.0f - std::exp(-dt / kPredictTauS);  // (a time constant, not a share per frame: the same smoothing at any frame rate)
			st.camVel = st.camVel + ((c.o - st.camO) * (1.0f / dt) - st.camVel) * k;
		}
	}
	Stream &st = g_streams[best];
	st.camValid = true, st.camO = c.o, st.camA = c.a, st.camFrame = g_hostFrame, st.camClip = c.clip, st.camDepth = c.depth;
	std::memcpy(st.camBox, c.box, sizeof st.camBox);
	taken[best] = true;
	return best;
}

// Prediction: Minecraft's frame lands latencyFrames host frames after the request, so the request
// asks for where this stream's camera will be by then (rotation and position from its own history).
// (2026-10-12) In time, not frames: the horizon is the measured delay in seconds (at most kPredictMaxS), the rates are smoothed with a time
// constant. It was "one frame's change x min(delay, 4 frames)": at Windows' ~280 fps the delay was ~7 frames (25 ms), so it predicted about
// half of it, and one frame's mouse step (uneven at 280 fps) times 4 made the prediction itself shake; the reprojection then had to carry
// 10-20 ms of a fast turn (the swirls around leaves and the player in third person). Linux at ~180 fps, 1-3 frames old, barely changes.
static float PredictHorizonS(const Stream &st) { return (float)std::fmin(st.latencyFrames * MeanFrameS(), kPredictMaxS); }
static QAngle PredictAngles(Stream &st, const QAngle &a) {
	QAngle out = a;
	if (g_predict && st.havePrevA) {
		const float dt = (float)((g_hostFrame - st.prevAFrame) * MeanFrameS());
		if (dt > 1e-4f) {
			const float d[3] = {a.x - st.prevA.x, std::remainder(a.y - st.prevA.y, 360.0f), std::remainder(a.z - st.prevA.z, 360.0f)};
			const float k = 1.0f - std::exp(-dt / kPredictTauS);
			for (int i = 0; i < 3; ++i) st.angVel[i] += (d[i] / dt - st.angVel[i]) * k;
		}
		const float h = PredictHorizonS(st);
		auto lim = [](float v) { return std::fmax(-30.0f, std::fmin(30.0f, v)); };
		out.x = std::fmax(-89.9f, std::fmin(89.9f, a.x + lim(st.angVel[0] * h)));
		out.y = a.y + lim(st.angVel[1] * h), out.z = a.z + lim(st.angVel[2] * h);
	}
	st.prevA = a, st.havePrevA = true, st.prevAFrame = g_hostFrame;
	return out;
}

static int g_shotFrames = 0;
static HostView g_lastHostView{};
static HostViewExt g_lastExt{};
static int g_secKept = 0;  // frames a throttled second-camera request was kept in the shared view until answered
static bool g_haveLastRequest = false;
static int g_reqLog = 0;
// The request for candidate c (its stream assigned): predicted pose, frustum, crop window, clip plane.
static void FillRequest(Cand &c, bool primary, HostView &v, float crop[4]) {
	Stream &st = g_streams[c.stream];
	QAngle angles = PredictAngles(st, c.a);
	Vector origin = c.o;
	if (g_predictPos) {
		Vector dv = st.camVel * PredictHorizonS(st);
		float l = dv.Length();
		if (l > g_predictPosMax) dv = dv * (g_predictPosMax / l);
		origin = c.o + dv;
	}
	v = HostView{};
	v.flags = kViewActive | (c.clip < 0 ? kViewFullScreen : 0u);
	if (primary && g_shotFrames > 0) --g_shotFrames, v.flags |= kViewScreenshot;
	SrcToMcD(origin, v.x, v.y, v.z);
	Vector f, r, u;
	AngleBasis(angles, f, r, u);
	double fm[3], um[3];
	DirToMc(f, fm);
	DirToMc(u, um);
	double pitch = std::asin(std::fmax(-1.0, std::fmin(1.0, -fm[1])));
	double yaw = std::atan2(-fm[0], fm[2]);
	double right0[3] = {-std::cos(yaw), 0, -std::sin(yaw)};
	double up0[3] = {-std::sin(pitch) * std::sin(yaw), std::cos(pitch), std::sin(pitch) * std::cos(yaw)};
	double roll = std::atan2(-(um[0] * right0[0] + um[1] * right0[1] + um[2] * right0[2]), um[0] * up0[0] + um[1] * up0[1] + um[2] * up0[2]);
	v.pitch = (float)(pitch * 180 / M_PI), v.yaw = (float)(yaw * 180 / M_PI), v.roll = (float)(roll * 180 / M_PI);
	const float os = Overscan();
	float tanX = g_ovTanX * os, tanY = g_ovTanY * os;
	v.fovY = (float)(2.0 * std::atan(tanY) * 180.0 / M_PI);
	if (c.clip >= 0) {
		double p[3], n[3];
		SrcToMcD(g_portalPos[c.clip], p[0], p[1], p[2]);
		DirToMc(g_portalNormal[c.clip], n);
		v.plane[0] = (float)n[0], v.plane[1] = (float)n[1], v.plane[2] = (float)n[2];
		v.plane[3] = (float)-(n[0] * p[0] + n[1] * p[1] + n[2] * p[2]);
		v.flags |= kViewClipPlane;
	}
	// crop window: the screen box (plus the margin on the primary camera), in the request's NDC
	float m = primary ? g_margin : 0.0f;
	crop[0] = -1, crop[1] = -1, crop[2] = 1, crop[3] = 1;
	if (g_crop) {
		crop[0] = std::max(-1.0f, (c.box[0] - m) / os), crop[1] = std::max(-1.0f, (c.box[1] - m) / os);
		crop[2] = std::min(1.0f, (c.box[2] + m) / os), crop[3] = std::min(1.0f, (c.box[3] + m) / os);
		if (crop[2] - crop[0] < 0.02f || crop[3] - crop[1] < 0.02f) crop[0] = crop[1] = -1, crop[2] = crop[3] = 1;
	}
	if (st.small) {
		// at most 1024 x 512 pixels of Minecraft's frame, centred on the view's box (a larger opening
		// shows its centre; its edges are the reprojection's uncovered margin)
		const float mw = 2.0f * (kSmallW - 2) / (float)g_vpW, mh = 2.0f * (kSmallH - 2) / (float)g_vpH;
		auto clampAxis = [](float &a, float &b, float m) {
			if (b - a <= m) return;
			float c = std::fmax(-1 + m / 2, std::fmin(1 - m / 2, (a + b) / 2));
			a = c - m / 2, b = c + m / 2;
		};
		clampAxis(crop[0], crop[2], mw);
		clampAxis(crop[1], crop[3], mh);
	}
	v.requestId = ++g_requestId;
	c.req = v.requestId;
	ReqInfo &q = g_req[v.requestId % 128];
	q = ReqInfo{};
	q.id = v.requestId, q.stream = c.stream, q.camGen = st.camGen, q.frame = g_hostFrame;
	q.origin = origin, q.angles = angles, q.tanX = tanX, q.tanY = tanY, q.clip = c.clip;
	std::memcpy(q.crop, crop, sizeof q.crop);
	if (g_reqLog > 0 || (g_requestId % 600) == 0) {
		if (g_reqLog > 0) --g_reqLog;
		console->Print("[bridge] composite: request %u (%s, %s) stream %d mc (%.2f %.2f %.2f) yaw %.1f pitch %.1f roll %.1f fovY %.1f plane %d crop (%.2f %.2f %.2f %.2f)\n", v.requestId,
		               primary ? "primary" : "second", c.what, c.stream, v.x, v.y, v.z, v.yaw, v.pitch, v.roll, v.fovY, (v.flags & kViewClipPlane) != 0, crop[0], crop[1], crop[2], crop[3]);
	}
}

static void MakeRequests() {
	bool taken[kStreams] = {};
	for (int k = 0; k < g_nCands; ++k) g_cands[k].stream = AssignStream(g_cands[k], taken);
	HostView v1{}, v2{};
	HostViewExt ext{};
	float crop1[4], crop2[4];
	FillRequest(g_cands[0], true, v1, crop1);
	if (g_cands[0].stream >= 0) g_streams[g_cands[0].stream].reqFrame = g_hostFrame;
	// Minecraft renders two cameras per frame: the largest view every frame; the other region views
	// (MC<->MC portal views and their recursion) share the second slot. Each has a period (a portal view
	// every g_secondEvery host frames, a recursion view every 3x that) and the most overdue one (frames
	// since its last request / its period; one with no image of its own first) takes the slot when it
	// is due. (A global "every Nth frame" with a longest-waiting pick starved portal views when several
	// shared the slot: 40-200 frames old in run mw1s.) Each view only ever shows its own stream's images.
	{
		int bestK = -1;
		float bestU = -1;
		for (int k = 1; k < g_nCands; ++k) {
			if (g_cands[k].stream < 0) continue;
			const Stream &st = g_streams[g_cands[k].stream];
			const float period = (float)(g_cands[k].depth >= 2 ? 3 * g_secondEvery : g_secondEvery);
			float u = (float)(g_hostFrame - st.reqFrame) / period;
			if (!(st.haveImage && st.img.camGen == st.camGen)) u += 1e6f;
			u += 1e-3f * g_cands[k].prio;  // (ties: the larger view)
			if (u > bestU) bestU = u, bestK = k;
		}
		if (bestK > 1) std::swap(g_cands[1], g_cands[bestK]);
		if (g_nCands > 1 && g_cands[1].stream >= 0) {
			if (bestU < 1.0f) g_cands[1].skipReq = true;  // nobody due (still matched by pose)
			else g_streams[g_cands[1].stream].reqFrame = g_hostFrame;
		}
	}
	ext.flags = kExtValid;
	std::memcpy(ext.crop, crop1, sizeof ext.crop);
	// A throttled second camera keeps its last request in the shared view until Minecraft has answered
	// it: Minecraft reads the view once per its own frame, and at a lower frame rate than Portal 2's it
	// could read only the frames in between (aliasing: the portal view's image went 40 frames stale
	// with a still camera, run mw0).
	bool keepSec = false;
	if (g_nCands > 1 && g_cands[1].stream >= 0 && g_cands[1].skipReq && g_haveLastRequest && g_lastExt.secRequestId &&
	    g_lastExt.secStream == (uint32_t)g_cands[1].stream) {
		const Stream &st = g_streams[g_cands[1].stream];
		const ReqInfo *q = Req(g_lastExt.secRequestId);
		keepSec = q && q->camGen == st.camGen && (!st.haveImage || st.img.id < g_lastExt.secRequestId);
		if (keepSec) ++g_secKept;
	}
	ext.primaryRequestId = v1.requestId;
	ext.primaryStream = (uint32_t)g_cands[0].stream;
	if (g_nCands > 1 && g_cands[1].stream >= 0 && !g_cands[1].skipReq) {
		FillRequest(g_cands[1], false, v2, crop2);
		ext.sx = v2.x, ext.sy = v2.y, ext.sz = v2.z;
		ext.syaw = v2.yaw, ext.spitch = v2.pitch, ext.sroll = v2.roll, ext.sfovY = v2.fovY;
		std::memcpy(ext.splane, v2.plane, sizeof ext.splane);
		std::memcpy(ext.scrop, crop2, sizeof ext.scrop);
		ext.secRequestId = v2.requestId, ext.secStream = (uint32_t)g_cands[1].stream;
		ext.secFlags = v2.flags & (kViewActive | kViewClipPlane);
		ext.secRenderDistance = (uint32_t)g_secondRd;
	} else if (keepSec) {
		ext.sx = g_lastExt.sx, ext.sy = g_lastExt.sy, ext.sz = g_lastExt.sz;
		ext.syaw = g_lastExt.syaw, ext.spitch = g_lastExt.spitch, ext.sroll = g_lastExt.sroll, ext.sfovY = g_lastExt.sfovY;
		std::memcpy(ext.splane, g_lastExt.splane, sizeof ext.splane);
		std::memcpy(ext.scrop, g_lastExt.scrop, sizeof ext.scrop);
		ext.secRequestId = g_lastExt.secRequestId, ext.secStream = g_lastExt.secStream;
		ext.secFlags = g_lastExt.secFlags, ext.secRenderDistance = g_lastExt.secRenderDistance;
	}
	WritePortals(v1.requestId);
	link::WriteHostViewExt(ext);
	link::WriteHostView(v1);
	g_lastHostView = v1, g_lastExt = ext, g_haveLastRequest = true;
}

// ---- Minecraft's images ---------------------------------------------------------------------------
static double g_uploadS = 0, g_dlPreS = 0, g_dlPostS = 0;
static int g_uploadN = 0;
static bool g_pingpong = true;
static double g_pullS = 0;
static int g_pullN = 0, g_noDepthFrames = 0, g_lostImages = 0;
static uint64_t g_skippedPulls = 0;
static float g_lagSum = 0, g_lagMax = 0;
static int g_lagN = 0;

static inline float DecodeDepthAlpha(int a) {  // Source units
	float t = (255.0f - a) / 254.0f;
	return kDepthAlphaMin * std::exp(t * std::log(kDepthAlphaMax / kDepthAlphaMin)) * kUnitsPerBlock;
}
static inline int EncodeDepthAlpha(float units) {  // the level whose decoded depth is >= units
	float d = units / kUnitsPerBlock;
	if (d <= kDepthAlphaMin) return 255;
	if (d >= kDepthAlphaMax) return 1;
	float t = std::log(d / kDepthAlphaMin) / std::log(kDepthAlphaMax / kDepthAlphaMin);
	return std::max(1, std::min(255, (int)std::floor(255.0f - t * 254.0f)));
}

static int g_shiftTrace = 0;  // frames to trace after a floating-origin shift (pulls, requests, frame times)
static double g_shiftTraceT = 0;
static std::atomic<uint64_t> g_mcFrameIdMax{0};  // Minecraft's newest frame number seen (RenderScaleUpdate: its frame rate)
static void TakeImage(const uint8_t *px, int w, int h, uint32_t requestId, uint32_t flags, uint64_t frameId) {
	if (frameId > g_mcFrameIdMax.load(std::memory_order_relaxed)) g_mcFrameIdMax.store(frameId, std::memory_order_relaxed);
	ReqInfo *q = Req(requestId);
	if (!q || q->stream < 0 || w <= 0 || h <= 0 || w > kTexW || h > kTexH) {
		++g_lostImages;
		return;
	}
	Stream &S = g_streams[q->stream];
	if (S.small && (w > kSmallW || h > kSmallH)) {
		++g_lostImages;
		return;
	}
	if (g_shiftTrace > 0)
		console->Print("[bridge] composite trace: host %llu image for request %u (stream %d, requested at host %llu, %llu frames ago), %dx%d\n", (unsigned long long)g_hostFrame, requestId, q->stream,
		               (unsigned long long)q->frame, (unsigned long long)(g_hostFrame - q->frame), w, h);
	if (S.haveImage && S.img.id > requestId) return;  // older than what it shows
	S.haveImage = true;
	S.img = *q;
	S.imgW = w, S.imgH = h;
	S.imageArrived = g_hostFrame;
	if (g_hostFrame >= q->frame) S.latencyFrames = 0.9f * S.latencyFrames + 0.1f * (float)(g_hostFrame - q->frame);
	S.depthAlpha = (flags & kSlotDepthAlpha) != 0;
	// proxy depth for position reprojection: the median of the depths near the image centre, and a
	// check that the alpha really carries depth (all 255: the pass did not land)
	int levels[49], nl = 0, n255 = 0;
	for (int j = -3; j <= 3; ++j)
		for (int i = -3; i <= 3; ++i) {
			int x = std::max(0, std::min(w - 1, w / 2 + i * w / 16)), y = std::max(0, std::min(h - 1, h / 2 + j * h / 16));
			int a = px[((size_t)y * w + x) * 4 + 3];
			n255 += a == 255;
			if (a > 0) levels[nl++] = a;
		}
	if (S.depthAlpha && n255 == 49) {
		bool any = false;
		for (int k = 0; k < 64 && !any; ++k) any = px[((size_t)((k * 431) % h) * w + (k * 977) % w) * 4 + 3] != 255;
		if (!any) {
			S.depthAlpha = false;
			if (++g_noDepthFrames <= 3) console->Print("[bridge] composite: frame %llu claims depth in alpha but its alpha is all 255; occlusion skipped\n", (unsigned long long)frameId);
		}
	}
	if (S.depthAlpha && nl > 24) {
		std::nth_element(levels, levels + nl / 2, levels + nl);
		S.proxyDepth = std::fmax(80.0f, std::fmin(9000.0f, DecodeDepthAlpha(levels[nl / 2])));
	} else {
		S.proxyDepth = 9000.0f;
	}
	// the worker copies it out of the slot into the stream's free buffer; the render thread then
	// uploads that buffer (queued now, so it runs before this frame's draws)
	int b = S.next;
	if (S.inflight[b].load(std::memory_order_acquire) > 0) {
		++S.skippedBusy;  // (never seen: an upload from two images ago still queued)
		return;
	}
	S.next ^= 1;
	S.cur = b, S.bufReq[b] = requestId;
	S.bufDone[b].store(0, std::memory_order_release);
	g_jobsPending.fetch_add(1, std::memory_order_acq_rel);
	{
		std::lock_guard<std::mutex> lk(g_jobMx);
		g_jobs.push_back(CopyJob{&S, b, requestId, px, w, h});
	}
	g_jobCv.notify_one();
	++g_frames;
	if (!g_upload) return;
	struct Upload {
		Stream *st;
		int buf;
		uint32_t req;
		int w, h;
		ITexture *tex;
		int set;
	};
	const int set = S.small || (w <= kSmallW && h <= kSmallH) ? 1 : 0;
	const TexSet &T = S.set[set];
	S.drawSet = set;
	auto *u = new Upload{&S, b, requestId, w, h, (g_srgb && T.texSrgb) ? T.texSrgb : T.tex, set};
	S.inflight[b].fetch_add(1, std::memory_order_acq_rel);
	RunOnRenderThread(
	    [](void *arg) {
		    auto *u = static_cast<Upload *>(arg);
		    Stream *st = u->st;
		    double t0 = NowS();
		    st->upBuf = u->buf, st->upReq = u->req, st->upW = u->w, st->upH = u->h, st->upSet = u->set;
		    st->upRect = Rect_t{0, 0, u->w, u->h};
		    st->regenT0 = st->regenT1 = 0;
		    u->tex->Download(&st->upRect);
		    double t1 = NowS();
		    g_uploadS += t1 - t0, ++g_uploadN;
		    if (st->regenT0 > 0) g_dlPreS += st->regenT0 - t0, g_dlPostS += t1 - st->regenT1;
		    st->inflight[u->buf].fetch_sub(1, std::memory_order_acq_rel);
		    delete u;
	    },
	    u);
}

static void PullFrame() {
	double t0 = NowS();
	if (g_jobsPending.load(std::memory_order_acquire) > 0) {
		++g_skippedPulls;  // the worker still reads the slot we hold (never seen: it takes ~1 ms)
		return;
	}
	const OverlaySlotHdr *h;
	const uint8_t *px;
	if (!link::AcquireOverlay(h, px)) return;
	if (h->flags & kSlotWorldView) {
		if (h->width && h->height) TakeImage(px, (int)h->width, (int)h->height, h->viewRequestId, h->flags, h->frameId);
		if (h->img2Offset && h->img2W && h->img2H && h->img2Offset + (uint64_t)h->img2W * h->img2H * 4 <= kOverlaySlotBytes)
			TakeImage(px + h->img2Offset, h->img2W, h->img2H, h->img2RequestId, h->img2Flags, h->frameId);
	}
	g_pullS += NowS() - t0, ++g_pullN;
}

// ---- per frame -------------------------------------------------------------------------------------
struct CurView {
	Vector origin;
	Vector f, r, u;
	float tx = 1, ty = 1;
	bool valid = false;
};
static CurView g_cur;
static CurView g_mainCur;  // the main camera this frame (OverrideView): a pair view's image mapping (v15)
// What this frame's region views showed (for the transit checks): the worst of them.
struct FrameViewInfo {
	int views = 0, missing = 0, stale = 0, mismatch = 0, reprojected = 0, exact = 0, held = 0, over = 0;
	int fallbackLate = 0;   // views drawn without their own image (fallback fill) after their camera chain's first 3 frames
	int fallbackEarly = 0;  // ... within them (Minecraft's first image for a new camera is 2-3 frames out)
	int stream = -1, set = -1, imgW = 0, imgH = 0;
	uint64_t age = 0;          // host frames since the drawn image's request
	float poseErr = 0, angErr = 0, uncovered = 0;
};
static FrameViewInfo g_fvi;
static int g_drawStream = -1;  // the stream drawn for the camera's own (background) view this frame
static bool g_regionCamThisFrame = false;
static bool g_anyImageEver = false;
static int g_missingViews = 0, g_missingAfterFirst = 0, g_staleViews = 0, g_mismatchViews = 0, g_reprojViews = 0, g_exactViews = 0, g_regionViews = 0;
static int g_ageLimit = 6;
static float g_posErrLimit = 80.0f;
// match statistics: rendered views against this frame's predicted cameras
static float g_matchPosSum = 0, g_matchPosMax = 0, g_matchAngMax = 0;
static int g_matchN = 0;
static float g_ovDiffMax = 0, g_ovDiffSum = 0;
static int g_ovDiffN = 0;

static void ResetPortalViewSnaps();  // (validation probes, below)
// Floating origin (floatorigin.cpp): every Source position the composite keeps for region cameras moves by
// d in the same frame as Chell and the mapping, so streams keep their camera continuity (no new camera,
// no stale-image matching) and images in flight still map to their requests. Minecraft-side positions
// are unchanged, so the images themselves stay valid.
static uint64_t g_shiftHostFrame = 0;  // the host frame of the last floating-origin shift (the watch's shift window)
void CompositeShiftTraceArm(int frames) { g_shiftTrace = frames, g_shiftTraceT = NowS(); }
void CompositeShift(const Vector &d) {
	g_shiftHostFrame = g_hostFrame + 1;
	for (Stream &st : g_streams) {
		st.camO += d;
		st.img.origin += d;
		for (int i = 0; i < 2; ++i) st.img.openPos[i] += d;
	}
	for (ReqInfo &r : g_req) {
		r.origin += d;
		for (int i = 0; i < 2; ++i) r.openPos[i] += d;
	}
	for (int i = 0; i < 2; ++i)
		if (g_portalLastSet[i] && ClipRegionContains(g_portalLastPos[i] + d)) g_portalLastPos[i] += d;
}

// once a second (see g_renderScale above McFrameSize)
static void RenderScaleUpdate() {
	static double t0 = 0, lastUp = -1e9, upLockUntil = 0;
	static uint64_t host0 = 0, mc0 = 0;
	static int lowSecs = 0, okSecs = 0;
	const double now = NowS();
	const uint64_t mcNow = g_mcFrameIdMax.load(std::memory_order_relaxed);
	if (t0 == 0 || mcNow < mc0) {  // (first call, or a new Minecraft whose frame numbers start again)
		t0 = now, host0 = g_hostFrame, mc0 = mcNow;
		return;
	}
	const double el = now - t0;
	if (el < 1.0) return;
	const double hostFps = (g_hostFrame - host0) / el, mcFps = (mcNow - mc0) / el;
	t0 = now, host0 = g_hostFrame, mc0 = mcNow;
	if (g_scaleFixed > 0) {
		g_renderScale = g_scaleFixed;
		return;
	}
	if (mcFps < 1.0 || hostFps < 1.0) {  // no Minecraft view (or a paused host): nothing to judge
		lowSecs = okSecs = 0;
		return;
	}
	const double want = std::min(hostFps, (double)g_targetFps);
	if (mcFps < 0.85 * want) ++lowSecs, okSecs = 0;
	else if (mcFps >= 0.97 * want) ++okSecs, lowSecs = 0;
	else lowSecs = okSecs = 0;
	const float s = g_renderScale.load();
	if (lowSecs >= 2 && s > kMinScale + 0.001f) {
		const float n = std::max(kMinScale, s - 0.1f);
		g_renderScale = n, lowSecs = 0;
		if (now - lastUp < 6.0) upLockUntil = now + 30.0;  // the last step up did not hold
		console->Print("[bridge] composite: render scale %.2f -> %.2f (Minecraft %.0f fps, Portal 2 %.0f fps, wanted %.0f)%s\n", s, n, mcFps, hostFps, want,
		               upLockUntil > now ? "; held there for 30 s" : "");
	} else if (okSecs >= 5 && s < 0.999f && now >= upLockUntil) {
		const float n = std::min(1.0f, s + 0.05f);
		g_renderScale = n, okSecs = 0, lastUp = now;
		console->Print("[bridge] composite: render scale %.2f -> %.2f (Minecraft %.0f fps, Portal 2 %.0f fps, wanted %.0f)\n", s, n, mcFps, hostFps, want);
	}
}

void CompositeFrameBegin(const CViewSetup *main) {
	CountFrame();  // host frame times are measured whether or not compositing is on (baselines)
	PROF("compbegin");  // (bridge #25)
	if (!g_enabled || !g_streams[0].set[0].mat) return;
	++g_hostFrame;
	RenderScaleUpdate();
	g_cur.valid = false;
	g_fvi = FrameViewInfo{};
	ResetPortalViewSnaps();
	g_drawStream = -1;
	g_regionCamThisFrame = false;
	PullFrame();
	UpdatePair();
	g_ovOrigin = main->origin, g_ovAngles = main->angles;
	// OverrideView's fov is the 4:3 horizontal value; the rendered view's is widened to the aspect
	int sw = 0, sh = 0;
	engine.GetScreenSize(engine.client->ThisPtr(), sw, sh);
	float aspect = sw > 0 && sh > 0 ? (float)sw / sh : 16.0f / 9;
	if (sw > 0 && sh > 0) {  // as mc.cpp sizes Minecraft's frame
		McFrameSize(sw, sh, Overscan(), g_vpW, g_vpH);
	}
	g_ovTanX = std::tan(main->fov * (float)M_PI / 360.0f) * aspect / (4.0f / 3.0f), g_ovTanY = g_ovTanX / aspect;
	AngleBasis(g_ovAngles, g_mainCur.f, g_mainCur.r, g_mainCur.u);
	g_mainCur.origin = g_ovOrigin, g_mainCur.tx = g_ovTanX, g_mainCur.ty = g_ovTanY, g_mainCur.valid = true;
	BuildCandidates(main);
	if (g_nCands > 0) MakeRequests();
	if (g_shiftTrace > 0) {
		--g_shiftTrace;
		const double t = NowS();
		console->Print("[bridge] composite trace: host %llu at +%.1f ms, eye (%.1f %.1f %.1f), %d candidates (first: stream %d, %s), request id now %u, pair in Minecraft %d\n", (unsigned long long)g_hostFrame,
		               1000 * (t - g_shiftTraceT), main->origin.x, main->origin.y, main->origin.z, g_nCands, g_nCands > 0 ? g_cands[0].stream : -2, g_nCands > 0 ? g_cands[0].what : "-", g_requestId, g_pairInMc);
	}
	if (g_nCands > 0) return;
	if (ClipHasRegion() && g_haveLastRequest) {
		// nothing links into the region (no portal pair): keep Minecraft rendering its last camera, so
		// its chunk meshes stay compiled and warm
		HostView v = g_lastHostView;
		v.flags &= ~kViewScreenshot;
		ReqInfo *q = Req(v.requestId);
		v.requestId = ++g_requestId;
		if (q) {
			ReqInfo copy = *q;
			copy.id = v.requestId, copy.frame = g_hostFrame;
			g_req[v.requestId % 128] = copy;
		}
		HostViewExt ext = g_lastExt;
		ext.primaryRequestId = v.requestId;
		ext.secFlags = 0;
		WritePortals(v.requestId);
		link::WriteHostViewExt(ext);
		link::WriteHostView(v);
	} else if (ClipHasRegion()) {
		Vector ep;
		QAngle ea;
		if (ClipExitHint(ep, ea)) {
			// before the first view of the region: the cliff exit looking out
			Vector out(std::cos(ea.y * (float)M_PI / 180), std::sin(ea.y * (float)M_PI / 180), 0);
			g_nCands = 1;
			g_cands[0] = Cand{};
			g_cands[0].o = ep + out * 80.0f, g_cands[0].a = QAngle{10, ea.y, 0}, g_cands[0].what = "exit hint";
			MakeRequests();
			g_nCands = 0;
		}
	}
}

// ---- drawing ---------------------------------------------------------------------------------------
static void RecordCurrent(const CViewSetup *view) {
	AngleBasis(view->angles, g_cur.f, g_cur.r, g_cur.u);
	g_cur.origin = view->origin;
	float aspect = view->m_flAspectRatio > 0.1f ? view->m_flAspectRatio : (float)view->width / (float)(view->height ? view->height : 1);
	g_cur.tx = std::tan(view->fov * (float)M_PI / 360.0f), g_cur.ty = g_cur.tx / aspect;
	g_cur.valid = true;
}

// Screen NDC of the current view -> texture coordinates in stream S's image: the view's ray, carried
// to the proxy plane (the image's depth) and seen from the pose the image was rendered for (rotation
// and position reprojection), then into the image's crop window.
static int g_uncovered = 0;
static bool g_holdView = false;  // this view: the image as it was rendered (no reprojection)
// Position reprojection per pixel (bridge #11, the user's "the world warps slightly while I walk"): the ray of
// each vertex is carried to the depth Minecraft wrote at the point it lands on in the image (its depth alpha),
// by fixed-point iteration from the proxy plane, instead of to one plane at the image centre's median depth.
// With one plane every surface off that depth shifted by (pose residual) x (1/z - 1/D): near ground swam
// ~5 px against the far terrain at walking speed, and D was re-picked per image (the warp changed with every
// image). The residual (the image's pose against the drawn camera's) changes every frame as image age
// alternates between 1-3 frames (Minecraft at 110-150 images/s vs 180), so that error was a jitter, while a
// pure rotation has no parallax and stayed exact (why turning on the spot looked fine).
// 0 iterations = the old single proxy plane (A/B: pcb_reproject_pixel <iterations>).
static int g_reprojIters = 3;
static int g_reprojFallback = 0;  // views drawn with the proxy plane because the image's depth was not ready
static float g_depthLut[256];
static bool g_depthLutInit = false;
static inline float DepthLevelCentre(int a) {  // Source units at the centre of depth level a (0: sky, far away)
	if (!g_depthLutInit) {
		const float L = std::log(kDepthAlphaMax / kDepthAlphaMin);
		for (int k = 0; k < 256; ++k) {
			// level k holds depths in (dec(k+1), dec(k)] (rounded towards farther): its geometric centre
			float t = std::fmax(0.0f, ((255.0f - k) - 0.5f) / 254.0f);
			g_depthLut[k] = k == 0 ? 1e5f : kDepthAlphaMin * std::exp(t * L) * kUnitsPerBlock;
		}
		g_depthLutInit = true;
	}
	return g_depthLut[a & 255];
}
static bool ImageUVCore(const CurView &cur, const ReqInfo &q, int imgW, int imgH, float tw, float th, float proxyDepth, bool held, float x, float y, float &u, float &v,
                        bool *outside, const uint8_t *depthBuf = nullptr) {
	float u1 = (float)imgW / tw, v1 = (float)imgH / th;
	const float os = Overscan();
	// no pose: the same view, at its own frustum (by the request's own tangents: an image requested before an overscan
	// change, e.g. a J switch, still maps right)
	float X = cur.valid && q.tanX > 0 ? x * cur.tx / q.tanX : x / os, Y = cur.valid && q.tanY > 0 ? y * cur.ty / q.tanY : y / os;
	if (cur.valid && g_reproject && !held) {
		Vector d = cur.f + cur.r * (x * cur.tx) + cur.u * (y * cur.ty);
		// (the image's basis: cached, the same request is mapped thousands of times per frame)
		static QAngle s_a{1e9f, 0, 0};
		static Vector pf, pr, pu;
		if (s_a.x != q.angles.x || s_a.y != q.angles.y || s_a.z != q.angles.z) AngleBasis(q.angles, pf, pr, pu), s_a = q.angles;
		Vector w = d;
		if (g_reprojectPos) {
			const Vector oc = cur.origin - q.origin;
			const float den = Vector::DotProduct(d, pf), ocf = Vector::DotProduct(oc, pf);
			float D = proxyDepth;
			const int iters = depthBuf ? g_reprojIters : 0;
			for (int it = 0; den > 1e-3f; ++it) {
				float t = (D - ocf) / den;
				if (!(t > 1.0f)) break;  // (that depth lies behind this camera: rotation only, as before)
				w = oc + d * t;
				if (it >= iters) break;
				// the depth Minecraft wrote where this point lands in the image
				float a = Vector::DotProduct(w, pf);
				if (a <= 1e-4f) break;
				float ix = Vector::DotProduct(w, pr) / a / q.tanX, iy = Vector::DotProduct(w, pu) / a / q.tanY;
				float cu = (ix - q.crop[0]) / (q.crop[2] - q.crop[0]), cv = (q.crop[3] - iy) / (q.crop[3] - q.crop[1]);
				int px = std::max(0, std::min(imgW - 1, (int)(cu * imgW))), py = std::max(0, std::min(imgH - 1, (int)(cv * imgH)));
				float Dn = DepthLevelCentre(depthBuf[((size_t)py * kTexW + px) * 4 + 3]);
				if (std::fabs(Dn - D) <= 0.002f * D) break;
				D = Dn;
			}
		}
		float a = Vector::DotProduct(w, pf);
		if (a <= 1e-4f) return false;
		X = Vector::DotProduct(w, pr) / a / q.tanX, Y = Vector::DotProduct(w, pu) / a / q.tanY;
	}
	float cu = (X - q.crop[0]) / (q.crop[2] - q.crop[0]), cv = (q.crop[3] - Y) / (q.crop[3] - q.crop[1]);
	bool out = cu < 0 || cu > 1 || cv < 0 || cv > 1;
	if (outside) *outside = out;
	if (out) ++g_uncovered;
	u = std::fmax(0.5f / tw, std::fmin(u1 - 0.5f / tw, cu * u1)), v = std::fmax(0.5f / th, std::fmin(v1 - 0.5f / th, cv * v1));
	return true;
}
// The CPU copy of the stream's newest image (its depth alpha), once the copy worker has written it: it starts
// at the pull (OverrideView) and takes ~1 ms, the draws come later in the frame. Bounded wait (1.5 ms), else
// the proxy plane for this frame (counted).
static const Stream *g_depthS = nullptr;
static uint32_t g_depthReq = 0;
static const uint8_t *g_depthPtr = nullptr;
static uint64_t g_depthFailFrame = ~0ull;
static const uint8_t *DepthBuf(const Stream &S) {
	if (g_reprojIters <= 0 || !S.depthAlpha || S.cur < 0 || S.bufReq[S.cur] != S.img.id) return nullptr;
	if (g_depthS == &S && g_depthReq == S.img.id && (g_depthPtr || g_depthFailFrame == g_hostFrame)) return g_depthPtr;
	const uint8_t *p = nullptr;
	for (int i = 0; i < 300; ++i) {
		if (S.bufDone[S.cur].load(std::memory_order_acquire) == S.img.id) {
			p = S.buf[S.cur];
			break;
		}
		plat::SleepUs(5);
	}
	if (!p) ++g_reprojFallback, g_depthFailFrame = g_hostFrame;  // (not waited for again this frame)
	g_depthS = &S, g_depthReq = S.img.id, g_depthPtr = p;
	return p;
}
static bool ImageUV(const Stream &S, float x, float y, float &u, float &v, bool *outside = nullptr) {
	return ImageUVCore(g_cur, S.img, S.imgW, S.imgH, TW(S), TH(S), S.proxyDepth, g_holdView, x, y, u, v, outside, DepthBuf(S));
}

// Black at the far plane (depth tested), in NDC: covers exactly what Portal 2 left as the region's
// sky in this view, never Portal 2's own pixels.
static void DrawFarBlack() {
	IMatRenderContext *rc = materialSystem->GetRenderContext();
	if (!rc || !g_matBlack) return;
	const int modes[3] = {kMatView, kMatProjection, kMatModel};
	for (int m : modes) RcCall<_RcInt>(rc, slots::kRcMatrixMode, m), RcCall<_RcVoid>(rc, slots::kRcPushMatrix), RcCall<_RcVoid>(rc, slots::kRcLoadIdentity);
	{
		MeshBuilder mb(g_matBlack, PrimitiveType::QUADS, 1);
		mb.ForceDepth(false, true);
		Color black(0, 0, 0, 255);
		const float q[4][2] = {{-1.01f, 1.01f}, {1.01f, 1.01f}, {1.01f, -1.01f}, {-1.01f, -1.01f}};
		for (auto &c : q) mb.Position(Vector(c[0], c[1], kFarZ)), mb.Color(black), mb.TexCoord(0, 0, 0), mb.AdvanceVertex();
		mb.Draw();
	}
	for (int m : modes) RcCall<_RcInt>(rc, slots::kRcMatrixMode, m), RcCall<_RcVoid>(rc, slots::kRcPopMatrix);
	RcCall<_RcVoid>(rc, slots::kRcRelease);
}

// The image over the whole view in NDC (identity matrices pushed by the caller), tessellated so the
// reprojected texture coordinates stay accurate.
struct ViewGeo;
static float ZAt(const ViewGeo &g, float x, float y, float D);
static Vector GeoPos(const ViewGeo &g, float x, float y, float D);
// The grid is fine enough that the per-pixel reprojection holds between vertices: for any plane 1/z is affine
// on screen, so a cell spanning one surface interpolates its parallax nearly exactly; only cells across a
// silhouette blend (33 x 33 px at 1600 x 900). 16 x 9 with the proxy plane before bridge #11.
static int g_gridX = 48, g_gridY = 27;
static const int kGridMaxX = 64, kGridMaxY = 36;
// the last background grid drawn for the camera's own view (the wobble probe interpolates it as drawn)
static float g_lastGridUV[(kGridMaxX + 1) * (kGridMaxY + 1)][2];
static int g_lastGridX = 0, g_lastGridY = 0;
static uint64_t g_lastGridFrame = 0;
static uint32_t g_lastGridImg = 0;
static void FullScreenImage(const Stream &S, IMaterial *mat, float zq, bool depthTest, const ViewGeo *geo = nullptr) {
	const int gx = std::max(4, std::min(kGridMaxX, g_gridX)), gy = std::max(3, std::min(kGridMaxY, g_gridY));
	static float uv[(kGridMaxX + 1) * (kGridMaxY + 1)][2];
	static Vector pos[(kGridMaxX + 1) * (kGridMaxY + 1)];
	for (int j = 0; j <= gy; ++j)
		for (int i = 0; i <= gx; ++i) {
			const float x = -1 + 2.0f * i / gx, y = 1 - 2.0f * j / gy;
			float tu, tv;
			if (!ImageUV(S, x, y, tu, tv)) tu = tv = 0;
			const int k = j * (gx + 1) + i;
			uv[k][0] = tu, uv[k][1] = tv;
			pos[k] = geo ? GeoPos(*geo, x, y, g_bgDepth) : Vector(x, y, zq);
		}
	if (!geo && !g_holdView) {
		std::memcpy(g_lastGridUV, uv, sizeof uv);
		g_lastGridX = gx, g_lastGridY = gy, g_lastGridFrame = g_hostFrame, g_lastGridImg = S.img.id;
	}
	MeshBuilder mb(mat, PrimitiveType::QUADS, gx * gy);
	mb.ForceDepth(false, depthTest);
	const Color white(255, 255, 255, 255);
	auto V = [&](int k) {
		mb.Position(pos[k]);
		mb.Color(white);
		mb.TexCoord(0, uv[k][0], uv[k][1]);
		mb.AdvanceVertex();
	};
	for (int j = 0; j < gy; ++j)
		for (int i = 0; i < gx; ++i) {
			const int k = j * (gx + 1) + i;
			V(k), V(k + 1), V(k + gx + 2), V(k + gx + 1);
		}
	mb.Draw();
}

// A view with a clip plane (a portal view): world-space geometry along the view's rays, each vertex
// just beyond where its ray crosses the plane of the portal it looks out of, so Portal 2's user clip
// plane keeps it to the portal and the oblique far plane cannot cut it away (a fixed 2000 u quad
// vanished when the virtual camera was within a few units of the portal: white frames).
static bool g_portalViewDepth = true;  // pcb_portal_view_depth
static void RayGrid(const CViewSetup *view, const Stream *S, IMaterial *mat, int pi) {
	const int gx = 32, gy = 20;  // (16 x 10 before the per-pixel reprojection)
	const float lo = -1.25f, hi = 1.25f, d = 2000.0f;
	Vector pn = pi >= 0 ? g_portalNormal[pi] : Vector(0, 0, 0), pp = pi >= 0 ? g_portalPos[pi] : Vector(0, 0, 0);
	float dp = pi >= 0 ? Vector::DotProduct(pp - view->origin, pn) : 0;  // > 0: the camera is behind the plane
	MeshBuilder mb(mat, PrimitiveType::QUADS, gx * gy);
	// (bridge #21) a portal view's image goes on the exit plane (a few units behind it) WITH the depth test: what Portal 2 itself drew
	// in front of the plane in this view (a cube or Chell coming out of the exit, the half of an object that has passed the
	// entry) stays in front. It was drawn without the test and covered all of that: objects cut off at the portal.
	mb.ForceDepth(false, S != nullptr && pi >= 0 && g_portalViewDepth);
	Color c = S ? Color(255, 255, 255, 255) : Color(0, 0, 0, 255);
	Vector cf, cr, cu;
	AngleBasis(view->angles, cf, cr, cu);
	float aspect = view->m_flAspectRatio > 0.1f ? view->m_flAspectRatio : (float)view->width / (float)(view->height ? view->height : 1);
	float ctx = std::tan(view->fov * (float)M_PI / 360.0f), cty = ctx / aspect;
	const float nearMin = std::fmax(12.0f, view->zNear * 2.0f + 4.0f);
	auto V = [&](float x, float y) {
		Vector dir = cf + cr * (x * ctx) + cu * (y * cty);
		float t = d, dn = Vector::DotProduct(dir, pn);
		// just beyond the plane, but never inside the view's near plane (a camera within a few units of
		// the plane: Chell's eye right at the pit portal; the geometry was clipped away, a white frame)
		if (pi >= 0 && dp > 0 && dn > 1e-4f) t = std::fmin(d, std::fmax(dp / dn * 1.01f + 4.0f, nearMin));
		else if (pi >= 0 && dp <= 0 && dp > -64.0f) t = dn < -1e-4f ? std::fmax(8.0f, dp / dn * 0.98f) : 16.0f;
		float tu = 0, tv = 0;
		if (S && !ImageUV(*S, x, y, tu, tv)) tu = tv = 0;
		mb.Position(view->origin + dir * t);
		mb.Color(c);
		mb.TexCoord(0, tu, tv);
		mb.AdvanceVertex();
	};
	for (int j = 0; j < gy; ++j)
		for (int i = 0; i < gx; ++i) {
			float x0 = lo + (hi - lo) * i / gx, x1 = lo + (hi - lo) * (i + 1) / gx, y0 = hi - (hi - lo) * j / gy, y1 = hi - (hi - lo) * (j + 1) / gy;
			V(x0, y0), V(x1, y0), V(x1, y1), V(x0, y1);
		}
	mb.Draw();
}

// ---- occlusion: Portal 2 objects behind Minecraft terrain ---------------------------------------
// Minecraft's image carries its per-pixel depth in alpha (log-encoded, rounded towards farther). Over
// the screen box of each Portal 2 object in the region (both portals with their rim and edge
// particles), quads of Minecraft's colour are drawn from far to near, one per depth level present
// in the object's depth span: quad a lies at depth dec(a) x (1 + eps), alpha-tested to the pixels
// whose Minecraft depth level is at least a (at least as near), depth-tested against Portal 2's
// depth and writing depth. A pixel thus shows Minecraft wherever its terrain is nearer than the
// Portal 2 surface there by more than eps (per screen pixel, at the image's own resolution and
// through the same reprojection as the colour, so Minecraft's own edges are untouched), and the
// depth written there hides the rim and particles Portal 2 draws next.
static bool g_occlusion = true;
static int g_occDebug = 0, g_occLog = 0;
static float g_occEps = 0.01f, g_occEpsOpening = 0.05f;
static int g_occLayers = 0, g_occObjects = 0;
static double g_drawS = 0;
static int g_drawN = 0;
static void OccluderTris(const Stream &S, IMaterial *mat, const float (*v)[2], int nTris, float z, bool depthTest) {
	if (nTris <= 0) return;
	MeshBuilder mb(mat, PrimitiveType::TRIANGLES, nTris);
	mb.ForceDepth(true, depthTest);
	Color white(255, 255, 255, 255);
	for (int k = 0; k < nTris * 3; ++k) {
		float tu, tv;
		if (!ImageUV(S, v[k][0], v[k][1], tu, tv)) tu = tv = 0;
		mb.Position(Vector(v[k][0], v[k][1], z));
		mb.Color(white);
		mb.TexCoord(0, tu, tv);
		mb.AdvanceVertex();
	}
	mb.Draw();
}
// The portal's opening does not hold the portal surface's depth in Portal 2's depth buffer (its
// pixels keep the depth of the view through it), so inside the opening the depth test is replaced
// by the analytic depth of the portal's plane: layer a covers the part of the opening where the
// plane lies beyond D_a (1/depth of a plane is linear in screen space: a straight cut).
static void DrawOccluders(const Stream &S) {
	if (!g_occlusion || !S.depthAlpha || !g_haveMainMatrices || !g_cur.valid) return;
	// the image's depths are as seen from where it was rendered: off by up to that distance from this
	// camera, so terrain must be nearer than a portal by more than that to hide it
	const float poseErr = (S.img.origin - g_cur.origin).Length();
	const float m22 = g_mainProj.m[2][2], m23 = g_mainProj.m[2][3];
	const int kE = 32;
	for (int i = 0; i < 2; ++i) {
		if (!g_portalSet[i] || !ClipRegionContains(g_portalPos[i])) continue;
		Vector c = g_portalPos[i], n = g_portalNormal[i], u = g_portalUp[i];
		Vector r = PortalRight(i);
		if (Vector::DotProduct(g_cur.origin - c, n) <= 1.0f) continue;  // behind or in the portal plane
		auto toNdc = [&](const Vector &p, float &sx, float &sy, float &depth) {
			Vector v = p - g_cur.origin;
			depth = Vector::DotProduct(v, g_cur.f);
			if (depth < 8.0f) return false;
			sx = Vector::DotProduct(v, g_cur.r) / (depth * g_cur.tx), sy = Vector::DotProduct(v, g_cur.u) / (depth * g_cur.ty);
			return true;
		};
		// the opening (scale 1) and the rim/particle box (scale 1.5, +-8 u off the plane)
		float E[kE][2], ex0 = 2, ey0 = 2, ex1 = -2, ey1 = -2, dmin = 1e30f, dmax = 0;
		bool ok = true;
		for (int k = 0; k < kE && ok; ++k) {
			float t = 2 * (float)M_PI * k / kE, dd;
			ok = toNdc(c + r * (g_openW * std::cos(t)) + u * (g_openH * std::sin(t)), E[k][0], E[k][1], dd);
			for (float off = -8; off <= 8 && ok; off += 16) {
				float sx, sy;
				ok = toNdc(c + r * (48.0f * std::cos(t)) + u * (84.0f * std::sin(t)) + n * off, sx, sy, dd);
				ex0 = std::min(ex0, sx), ex1 = std::max(ex1, sx), ey0 = std::min(ey0, sy), ey1 = std::max(ey1, sy);
				dmin = std::min(dmin, dd), dmax = std::max(dmax, dd);
			}
		}
		if (!ok) continue;  // the camera is right at the portal: nothing of the terrain can be in front of it
		ex0 = std::max(-1.02f, ex0), ey0 = std::max(-1.02f, ey0), ex1 = std::min(1.02f, ex1), ey1 = std::min(1.02f, ey1);
		if (ex0 >= ex1 || ey0 >= ey1) continue;
		++g_occObjects;
		// ring: the box minus the opening, as kE quads from the opening's edge to the box's edge
		float cx = 0, cy = 0;
		for (auto &e : E) cx += e[0], cy += e[1];
		cx /= kE, cy /= kE;
		static float ring[kE * 6][2];
		for (int k = 0; k < kE; ++k) {
			auto outer = [&](int kk, float &ox, float &oy) {
				float dx = E[kk % kE][0] - cx, dy = E[kk % kE][1] - cy;
				float sxp = dx > 0 ? (ex1 - cx) / dx : dx < 0 ? (ex0 - cx) / dx : 1e9f, syp = dy > 0 ? (ey1 - cy) / dy : dy < 0 ? (ey0 - cy) / dy : 1e9f;
				float sc = std::max(1.0f, std::min(sxp, syp));
				ox = cx + dx * sc, oy = cy + dy * sc;
			};
			float o0x, o0y, o1x, o1y;
			outer(k, o0x, o0y), outer(k + 1, o1x, o1y);
			const float tri[6][2] = {{E[k][0], E[k][1]}, {o0x, o0y}, {o1x, o1y}, {E[k][0], E[k][1]}, {o1x, o1y}, {E[(k + 1) % kE][0], E[(k + 1) % kE][1]}};
			for (int q = 0; q < 6; ++q) ring[k * 6 + q][0] = tri[q][0], ring[k * 6 + q][1] = tri[q][1];
		}
		// the plane's 1/depth over the screen: 1/z = A x + B y + C (from three projected points)
		float P[3][3];
		{
			const Vector pts[3] = {c, c + r * 32.0f, c + u * 56.0f};
			for (int q = 0; q < 3; ++q) toNdc(pts[q], P[q][0], P[q][1], P[q][2]);
		}
		float A, B, Cc;
		{
			float x1 = P[1][0] - P[0][0], y1 = P[1][1] - P[0][1], w1 = 1 / P[1][2] - 1 / P[0][2];
			float x2 = P[2][0] - P[0][0], y2 = P[2][1] - P[0][1], w2 = 1 / P[2][2] - 1 / P[0][2];
			float det = x1 * y2 - x2 * y1;
			if (std::fabs(det) < 1e-9f) continue;
			A = (w1 * y2 - w2 * y1) / det, B = (x1 * w2 - x2 * w1) / det;
			Cc = 1 / P[0][2] - A * P[0][0] - B * P[0][1];
		}
		int aFar = EncodeDepthAlpha(dmax / (1 + g_occEps)), aNear = std::min(255, EncodeDepthAlpha(dmin / (1 + g_occEps)) + 1);
		if (g_occLog > 0) {
			--g_occLog;
			console->Print("[bridge] occl portal %d (stream %d): box ndc (%.3f %.3f)-(%.3f %.3f) depth %.1f..%.1f, levels %d..%d\n", i, S.index, ex0, ey0, ex1, ey1, dmin, dmax, aFar, aNear);
		}
		for (int a = aFar; a <= aNear; ++a) {
			float D = DecodeDepthAlpha(a) * (1 + g_occEps) + poseErr;
			IMaterial *occl = S.set[S.drawSet].occl[a];
			if (D > dmax + 1 || !occl) continue;
			float z = -m22 + m23 / D;
			if (!(z > 0 && z < 1)) continue;
			// ring: Portal 2's depth there is trustworthy (rock behind the portal shows as the region's sky)
			OccluderTris(S, occl, ring, kE * 2, z, true);
			// opening: the part where the portal plane lies beyond D (1/z <= 1/D), depth test off; with
			// a larger margin: the portal sits on terrain, which lies at the plane's own depth, and a
			// reprojected image's depths are a few units off (stripes at grazing views)
			float poly[kE * 2 + 4][2];
			int np = 0;
			const float lim = 1 / (DecodeDepthAlpha(a) * (1 + g_occEpsOpening) + 8.0f + poseErr);
			for (int k = 0; k < kE; ++k) {
				const float *p0 = E[k], *p1 = E[(k + 1) % kE];
				float w0 = A * p0[0] + B * p0[1] + Cc - lim, w1 = A * p1[0] + B * p1[1] + Cc - lim;
				if (w0 <= 0) poly[np][0] = p0[0], poly[np][1] = p0[1], ++np;
				if ((w0 <= 0) != (w1 <= 0)) {
					float t = w0 / (w0 - w1);
					poly[np][0] = p0[0] + (p1[0] - p0[0]) * t, poly[np][1] = p0[1] + (p1[1] - p0[1]) * t, ++np;
				}
			}
			if (np >= 3) {
				static float fan[(kE * 2 + 4) * 3][2];
				int nt = 0;
				for (int k = 1; k + 1 < np; ++k, ++nt) {
					fan[nt * 3][0] = poly[0][0], fan[nt * 3][1] = poly[0][1];
					fan[nt * 3 + 1][0] = poly[k][0], fan[nt * 3 + 1][1] = poly[k][1];
					fan[nt * 3 + 2][0] = poly[k + 1][0], fan[nt * 3 + 2][1] = poly[k + 1][1];
				}
				OccluderTris(S, occl, fan, nt, z, false);
			}
			++g_occLayers;
		}
	}
}

// Drift probe: each region portal's centre on screen against where the composite shows Minecraft's
// rendering of that spot (the block face the portal rests on). Pixels at 1600 x 900. The offset is
// the misregistration of Portal 2's portal against Minecraft's terrain; its change from one frame to
// the next is the TWITCH (pcb_twitch): ~0 with a still camera, small and smooth while moving.
static double g_driftSum = 0, g_driftMax = 0;
static int g_driftN = 0;
struct TwitchStats {
	int n = 0, frames = 0, over1 = 0, over2 = 0, newImg = 0;
	double sum = 0, max = 0, sumNew = 0, driftSum = 0, driftMax = 0;
	std::vector<float> all;  // per-frame jitter (for percentiles)
	uint64_t maxFrame = 0;
};
static TwitchStats g_tw;
struct TwitchPrev {
	uint64_t frame = 0;
	float dx = 0, dy = 0;
	uint32_t img = 0;
};
static TwitchPrev g_twPrev[2];
static uint64_t g_twLastFrame[2] = {};
static int g_twSkip[8] = {};  // why a frame's portal was not measured (pcb_twitch report)
static void DriftProbe(const Stream &S) {
	if (!g_cur.valid) return void(++g_twSkip[0]);
	for (int pi = 0; pi < 2; ++pi) {
		if (!g_portalSet[pi] || !ClipRegionContains(g_portalPos[pi]) || g_twLastFrame[pi] == g_hostFrame) continue;
		g_twLastFrame[pi] = g_hostFrame;
		Vector c = g_portalPos[pi];
		{
			// only a portal resting on a block: one in open air (pcb_portal) has no terrain at its centre to compare with
			double bx, by, bz;
			SrcToMcD(c - g_portalNormal[pi] * 2.0f, bx, by, bz);
			if (!TerrainIsSolid((int)std::floor(bx), (int)std::floor(by), (int)std::floor(bz))) { ++g_twSkip[1]; continue; }
		}
		auto screenOf = [&](const Vector &dir, float &sx, float &sy) {
			float a = Vector::DotProduct(dir, g_cur.f);
			if (a <= 1e-3f) return false;
			sx = Vector::DotProduct(dir, g_cur.r) / (a * g_cur.tx), sy = Vector::DotProduct(dir, g_cur.u) / (a * g_cur.ty);
			return true;
		};
		float tx, ty;
		// (the portal facing the camera: its front side; a portal seen from behind is inside the rock)
		if (Vector::DotProduct(g_cur.origin - c, g_portalNormal[pi]) < 1.0f) { ++g_twSkip[2]; continue; }
		if (!screenOf(c - g_cur.origin, tx, ty) || std::fabs(tx) > 0.95f || std::fabs(ty) > 0.95f) { ++g_twSkip[3]; continue; }
		// hidden behind terrain from here: nothing to compare
		if (TerrainRayBlocked(g_cur.origin, c + g_portalNormal[pi] * 2.0f)) { ++g_twSkip[4]; continue; }
		// where the image shows that point: its direction from the image's pose, back through ImageUV's mapping
		Vector pf, pr, pu;
		AngleBasis(S.img.angles, pf, pr, pu);
		Vector w = c - S.img.origin;
		float a = Vector::DotProduct(w, pf);
		if (a < 1) { ++g_twSkip[5]; continue; }
		float X = Vector::DotProduct(w, pr) / a / S.img.tanX, Y = Vector::DotProduct(w, pu) / a / S.img.tanY;
		// the screen point whose ImageUV lands there: Newton steps
		float sx = tx, sy = ty;
		for (int it = 0; it < 4; ++it) {
			float u0, v0, u1, v1, u2, v2;
			auto img = [&](float x, float y, float &ix, float &iy) {
				float tu, tv;
				ImageUV(S, x, y, tu, tv);
				ix = tu, iy = tv;
			};
			img(sx, sy, u0, v0), img(sx + 1e-3f, sy, u1, v1), img(sx, sy + 1e-3f, u2, v2);
			float tu = ((X - S.img.crop[0]) / (S.img.crop[2] - S.img.crop[0])) * S.imgW / TW(S), tv = ((S.img.crop[3] - Y) / (S.img.crop[3] - S.img.crop[1])) * S.imgH / TH(S);
			float j00 = (u1 - u0) / 1e-3f, j01 = (u2 - u0) / 1e-3f, j10 = (v1 - v0) / 1e-3f, j11 = (v2 - v0) / 1e-3f, det = j00 * j11 - j01 * j10;
			if (std::fabs(det) < 1e-12f) break;
			float du = tu - u0, dv = tv - v0;
			sx += (j11 * du - j01 * dv) / det, sy += (-j10 * du + j00 * dv) / det;
		}
		float ddx = (sx - tx) * 800, ddy = (sy - ty) * 450;
		double dr = std::hypot(ddx, ddy);
		if (!(dr < 1e4)) { ++g_twSkip[6]; continue; }
		g_driftSum += dr, g_driftMax = std::max(g_driftMax, dr), ++g_driftN;
		g_tw.driftSum += dr, g_tw.driftMax = std::max(g_tw.driftMax, dr), ++g_tw.frames;
		TwitchPrev &pv = g_twPrev[pi];
		if (pv.frame + 1 == g_hostFrame) {
			float j = (float)std::hypot(ddx - pv.dx, ddy - pv.dy);
			bool newImg = pv.img != S.img.id;
			++g_tw.n, g_tw.sum += j, g_tw.over1 += j > 1.0f, g_tw.over2 += j > 2.0f, g_tw.newImg += newImg;
			if (newImg) g_tw.sumNew += j;
			if (j > g_tw.max) g_tw.max = j, g_tw.maxFrame = g_hostFrame;
			if (g_tw.all.size() < 200000) g_tw.all.push_back(j);
		}
		pv.frame = g_hostFrame, pv.dx = ddx, pv.dy = ddy, pv.img = S.img.id;
	}
}

// One region view, drawn from stream S (or black without one).
static int g_exitViews = 0, g_heldViews = 0, g_entrySideViews = 0, g_intoPortalViews = 0;
static int g_foreignAvoided = 0, g_fallbackViews = 0, g_foreignDrawn = 0, g_regionViewsAtReset = 0;  // per-view stream ownership
static int g_fallbackLateViews = 0;
// image age per view (host frames since the drawn image's request): the camera's own view (primary)
// and portal views (throttled secondary cameras), for pcb_assert_image_age
static uint64_t g_agePriMax = 0, g_ageSecMax = 0;
static double g_agePriSum = 0, g_ageSecSum = 0;
static int g_agePriN = 0, g_ageSecN = 0;
static int g_ageHist[2][12] = {};
static uint64_t g_ageRecMax = 0;
static int g_ageRecN = 0;
struct ViewGeo;
static void DrawPairView(const CViewSetup *view, const Stream *S);
static void MainViewObjectOccluders(const Stream &S);
// A lone portal (no partner) that is still opening: Portal 2 draws its growing opening (static disc and
// rim) without writing the portal plane's depth (the pixels keep the sky's), so the region background drawn
// in the translucent pass at the sky depth covered it, and a first shot in Minecraft popped in at open 1
// instead of growing (runs first5-first8; redrawing only its stage-2 overlay gave a disc a third the size:
// most of the opening is its stencil/rim stages). While a lone region portal opens, the main region view
// is composited at the start of Portal 2's opaque pass instead (render.cpp), before Portal 2 draws its
// portals, so the opening lands on Minecraft's terrain as it lands on a chamber wall. Fully open or
// linked portals take Portal 2's normal path, which the late background never covered.
static bool g_earlyOpening = true;
static int g_earlyViews = 0;
static bool LoneOpening(int i) {
	const float open = ReadOpenVar(i);
	return g_portalSet[i] && !g_portalSet[1 - i] && ClipRegionContains(g_portalPos[i]) && open > 0.0f && open < 0.999f;
}
bool CompositeWantsEarly(const CViewSetup *view) {
	if (!g_earlyOpening || !g_enabled || g_pairInMc || !ClipRegionContains(view->origin) || (view->origin - g_ovOrigin).Length() >= 2.0f) return false;
	if (!(LoneOpening(0) || LoneOpening(1))) return false;
	++g_earlyViews;
	return true;
}
CON_COMMAND(pcb_stage_flat, "pcb_stage_flat [0|1] - (bridge #74) a portal in the virtual region is drawn by Minecraft as a flat colour (1) or left to Portal 2 / the pair path (0)\n") {
	if (args.ArgC() > 1) g_stageFlat = std::atoi(args[1]) != 0;
	console->Print("[bridge] stage flat %d\n", g_stageFlat);
}
CON_COMMAND(pcb_early_opening, "pcb_early_opening [0|1] - while a lone region portal opens, composite the main region view before Portal 2's portals (default 1)\n") {
	if (args.ArgC() > 1) g_earlyOpening = std::atoi(args[1]) != 0;
	console->Print("[bridge] early composite while a lone portal opens %d (%d views so far)\n", g_earlyOpening, g_earlyViews);
}
// ---- wobble probe (bridge #11): Minecraft's world against where it should be, as drawn ------------------
// N fixed points on Minecraft block faces (rays from the camera to the terrain bisected by the block march:
// exact surfaces, not the image's quantised depth). Every frame, for each point visible in both the image
// (the image's depth there agrees) and on screen: where the background grid AS DRAWN shows it (the drawn
// grid's texture coordinates, interpolated as the GPU does, inverted by Newton steps) against its true
// projection. That misregistration e(t) in screen pixels is how far Minecraft's world sits from where a
// Portal 2-native object at the same spot would be. Reported:
//   * jitter |e(t) - e(t-1)|: the world swimming against Portal 2's from frame to frame;
//   * the drawn position's second difference |p(t) - 2p(t-1) + p(t-2)| (p = true + e) against the native
//     reference's |s(t) - 2s(t-1) + s(t-2)| (s = the true projection: what Portal 2 itself would draw there).
// Per-frame log (pcb_wobble log N): image age, the reprojection translation (drawn camera - image pose),
// OverrideView's pose against the drawn camera, mean |e|.
bool TerrainRayHit(const Vector &a, const Vector &dir, float maxDist, Vector &point);  // portalgun.cpp
struct WobblePt {
	Vector p;
	bool valid = false;
	uint64_t f1 = 0, f2 = 0;  // frames of the two previous samples (consecutive frames only)
	float e1[2] = {}, e2[2] = {}, s1[2] = {}, s2[2] = {};
};
static const int kWobbleMax = 96;
static WobblePt g_wob[kWobbleMax];
static int g_wobN = 0, g_wobWant = 0, g_wobLog = 0, g_wobFrames = 0, g_wobRearms = 0;
static bool g_wobOn = false;
static std::vector<float> g_wobJit, g_wobAcc, g_wobNat, g_wobMis;
static double g_wobTransSum = 0, g_wobOvSum = 0, g_wobAgeSum = 0;
static int g_wobAgeHist[8] = {};
static void WobbleArm(const Stream &S) {
	g_wobN = 0;
	// a grid over the lower 3/4 of the screen (the ground walked over, near to far) and some higher rows
	const int cols = 12, rows = std::max(1, g_wobWant / cols);
	for (int j = 0; j < rows && g_wobN < kWobbleMax; ++j)
		for (int i = 0; i < cols && g_wobN < kWobbleMax; ++i) {
			float x = -0.85f + 1.7f * (i + 0.5f) / cols, y = 0.5f - 1.4f * (j + 0.5f) / rows;
			Vector d = g_cur.f + g_cur.r * (x * g_cur.tx) + g_cur.u * (y * g_cur.ty);
			d = d * (1.0f / d.Length());
			Vector hit;
			if (!TerrainRayHit(g_cur.origin, d, 4000.0f, hit)) continue;
			WobblePt &w = g_wob[g_wobN++];
			w = WobblePt{};
			w.p = hit, w.valid = true;
		}
	++g_wobRearms;
	(void)S;
}
// the drawn grid's texture coordinates at screen (x, y): the quad's two triangles (TL TR BR) and (TL BR BL)
static bool DrawnUV(float x, float y, float &u, float &v) {
	const int gx = g_lastGridX, gy = g_lastGridY;
	if (gx <= 0) return false;
	float fx = (x + 1) * 0.5f * gx, fy = (1 - y) * 0.5f * gy;
	int i = (int)std::floor(fx), j = (int)std::floor(fy);
	if (i < 0 || j < 0 || i >= gx || j >= gy) return false;
	fx -= i, fy -= j;
	const float *TL = g_lastGridUV[j * (gx + 1) + i], *TR = g_lastGridUV[j * (gx + 1) + i + 1], *BL = g_lastGridUV[(j + 1) * (gx + 1) + i], *BR = g_lastGridUV[(j + 1) * (gx + 1) + i + 1];
	for (int c = 0; c < 2; ++c)
		(c ? v : u) = fx >= fy ? TL[c] + fx * (TR[c] - TL[c]) + fy * (BR[c] - TR[c]) : TL[c] + fy * (BL[c] - TL[c]) + fx * (BR[c] - BL[c]);
	return true;
}
static void WobbleProbe(const Stream &S, const CViewSetup *view) {
	if (!g_wobOn || !g_cur.valid || g_lastGridFrame != g_hostFrame || g_lastGridImg != S.img.id) return;
	// re-arm when most points went off screen or out of the image
	int live = 0;
	Vector pf, pr, pu;
	AngleBasis(S.img.angles, pf, pr, pu);
	const uint8_t *dbuf = S.depthAlpha && S.cur >= 0 && S.bufDone[S.cur].load(std::memory_order_acquire) == S.img.id ? S.buf[S.cur] : nullptr;
	if (!dbuf) return;
	const float pxX = 800.0f, pxY = 450.0f;  // NDC -> pixels at 1600 x 900
	float misSum = 0;
	int misN = 0;
	for (int k = 0; k < g_wobN; ++k) {
		WobblePt &w = g_wob[k];
		if (!w.valid) continue;
		// true projection in the drawn camera
		Vector v = w.p - g_cur.origin;
		float z = Vector::DotProduct(v, g_cur.f);
		if (z < 16.0f) { w.f1 = w.f2 = 0; continue; }
		float sx = Vector::DotProduct(v, g_cur.r) / (z * g_cur.tx), sy = Vector::DotProduct(v, g_cur.u) / (z * g_cur.ty);
		if (std::fabs(sx) > 0.92f || std::fabs(sy) > 0.92f) { w.f1 = w.f2 = 0; continue; }
		// the point in the image, and the image's own depth there (hidden in the image / water / leaves: skip)
		Vector wi = w.p - S.img.origin;
		float zi = Vector::DotProduct(wi, pf);
		if (zi < 16.0f) { w.f1 = w.f2 = 0; continue; }
		float X = Vector::DotProduct(wi, pr) / zi / S.img.tanX, Y = Vector::DotProduct(wi, pu) / zi / S.img.tanY;
		float cu = (X - S.img.crop[0]) / (S.img.crop[2] - S.img.crop[0]), cv = (S.img.crop[3] - Y) / (S.img.crop[3] - S.img.crop[1]);
		if (cu < 0.01f || cu > 0.99f || cv < 0.01f || cv > 0.99f) { w.f1 = w.f2 = 0; continue; }
		const int ipx = std::min(S.imgW - 1, (int)(cu * S.imgW)), ipy = std::min(S.imgH - 1, (int)(cv * S.imgH));
		const float Dimg = DepthLevelCentre(dbuf[((size_t)ipy * kTexW + ipx) * 4 + 3]);
		if (std::fabs(Dimg - zi) > 0.04f * zi + 8.0f) { w.f1 = w.f2 = 0; continue; }
		++live;
		const float tu = cu * S.imgW / TW(S), tv = cv * S.imgH / TH(S);
		// the screen point whose drawn texture coordinates are (tu, tv): Newton from the true projection
		float x = sx, y = sy;
		bool ok = true;
		for (int it = 0; it < 6 && ok; ++it) {
			float u0, v0, u1, v1, u2, v2;
			const float h = 2e-3f;
			ok = DrawnUV(x, y, u0, v0) && DrawnUV(x + h, y, u1, v1) && DrawnUV(x, y + h, u2, v2);
			if (!ok) break;
			float j00 = (u1 - u0) / h, j01 = (u2 - u0) / h, j10 = (v1 - v0) / h, j11 = (v2 - v0) / h, det = j00 * j11 - j01 * j10;
			if (std::fabs(det) < 1e-12f) { ok = false; break; }
			float du = tu - u0, dv = tv - v0;
			x += (j11 * du - j01 * dv) / det, y += (-j10 * du + j00 * dv) / det;
			if (std::fabs(du) * TW(S) < 0.02f && std::fabs(dv) * TH(S) < 0.02f) break;
		}
		if (!ok || std::fabs(x - sx) > 0.2f || std::fabs(y - sy) > 0.2f) { w.f1 = w.f2 = 0; continue; }
		const float e[2] = {(x - sx) * pxX, (y - sy) * pxY}, s[2] = {sx * pxX, sy * pxY};
		misSum += std::hypot(e[0], e[1]), ++misN;
		g_wobMis.push_back(std::hypot(e[0], e[1]));
		if (w.f1 + 1 == g_hostFrame) {
			g_wobJit.push_back(std::hypot(e[0] - w.e1[0], e[1] - w.e1[1]));
			if (w.f2 + 2 == g_hostFrame) {
				// drawn (s + e) and native (s) second differences
				float ax = (s[0] + e[0]) - 2 * (w.s1[0] + w.e1[0]) + (w.s2[0] + w.e2[0]), ay = (s[1] + e[1]) - 2 * (w.s1[1] + w.e1[1]) + (w.s2[1] + w.e2[1]);
				float nx = s[0] - 2 * w.s1[0] + w.s2[0], ny = s[1] - 2 * w.s1[1] + w.s2[1];
				g_wobAcc.push_back(std::hypot(ax, ay)), g_wobNat.push_back(std::hypot(nx, ny));
			}
		}
		w.f2 = w.f1, w.f1 = g_hostFrame;
		std::memcpy(w.e2, w.e1, sizeof w.e2), std::memcpy(w.s2, w.s1, sizeof w.s2);
		std::memcpy(w.e1, e, sizeof w.e1), std::memcpy(w.s1, s, sizeof w.s1);
	}
	++g_wobFrames;
	const uint64_t age = g_hostFrame - S.img.frame;
	++g_wobAgeHist[std::min<uint64_t>(age, 7)];
	const float trans = (g_cur.origin - S.img.origin).Length(), ovd = (view->origin - g_ovOrigin).Length();
	g_wobTransSum += trans, g_wobOvSum += ovd, g_wobAgeSum += (double)age;
	if (g_wobLog > 0) {
		--g_wobLog;
		console->Print("[bridge] wobble f%llu: image %u age %llu, reprojection translation %.2f u (image pose -> drawn camera), OverrideView vs drawn %.3f u, %d points, misregistration mean %.2f px\n",
		               (unsigned long long)g_hostFrame, S.img.id, (unsigned long long)age, trans, ovd, misN, misN ? misSum / misN : 0.0f);
	}
	if (live < g_wobN / 3 || g_wobN < 6) WobbleArm(S);
}
static void WobbleReport(const char *label, float limP95, float limRatio) {
	auto pct = [](std::vector<float> v, double q) {
		if (v.empty()) return 0.0;
		std::sort(v.begin(), v.end());
		return (double)v[std::min(v.size() - 1, (size_t)(q * (v.size() - 1) + 0.5))];
	};
	const double j50 = pct(g_wobJit, 0.5), j95 = pct(g_wobJit, 0.95), j99 = pct(g_wobJit, 0.99), jmax = pct(g_wobJit, 1.0);
	const double a95 = pct(g_wobAcc, 0.95), n95 = pct(g_wobNat, 0.95), a50 = pct(g_wobAcc, 0.5), n50 = pct(g_wobNat, 0.5);
	const double m50 = pct(g_wobMis, 0.5), m95 = pct(g_wobMis, 0.95);
	char buf[700];
	std::snprintf(buf, sizeof buf,
	              "[%s] %d frames, %zu point-frame pairs: jitter |e(t)-e(t-1)| p50 %.2f p95 %.2f p99 %.2f max %.2f px; drawn 2nd difference p50 %.2f p95 %.2f px vs Portal 2-native p50 %.2f p95 %.2f px; "
	              "misregistration p50 %.2f p95 %.2f px; mean image age %.2f (ages 1..7+: %d %d %d %d %d %d %d), mean reprojection translation %.2f u, OverrideView vs drawn %.3f u; %d re-arms; "
	              "per-pixel fallbacks %d",
	              label, g_wobFrames, g_wobJit.size(), j50, j95, j99, jmax, a50, a95, n50, n95, m50, m95, g_wobFrames ? g_wobAgeSum / g_wobFrames : 0.0, g_wobAgeHist[1], g_wobAgeHist[2],
	              g_wobAgeHist[3], g_wobAgeHist[4], g_wobAgeHist[5], g_wobAgeHist[6], g_wobAgeHist[7], g_wobFrames ? g_wobTransSum / g_wobFrames : 0.0,
	              g_wobFrames ? g_wobOvSum / g_wobFrames : 0.0, g_wobRearms, g_reprojFallback);
	if (limP95 > 0) {
		// the gate: the drawn world's frame-to-frame jitter is within noise (p95 under the limit), and its
		// second difference is that of a Portal 2-native object at the same points (within the ratio)
		TestCheck(g_wobJit.size() >= 100 && j95 <= limP95 && a95 <= n95 * limRatio + limP95, "walking wobble", buf);
	} else {
		console->Print("[bridge] wobble %s\n", buf);
	}
}
CON_COMMAND(pcb_wobble, "pcb_wobble start [points=72] | log <frames> | report <label> [max_p95_px] [max_ratio=1.5] | stop - Minecraft's world against Portal 2's while moving (misregistration jitter of fixed terrain points, as drawn)\n") {
	const char *op = args.ArgC() > 1 ? args[1] : "report";
	if (!std::strcmp(op, "start")) {
		g_wobWant = args.ArgC() > 2 ? std::max(12, std::min(kWobbleMax, std::atoi(args[2]))) : 72;
		g_wobN = 0, g_wobFrames = 0, g_wobRearms = 0, g_wobOn = true;
		g_wobJit.clear(), g_wobAcc.clear(), g_wobNat.clear(), g_wobMis.clear();
		g_wobTransSum = g_wobOvSum = g_wobAgeSum = 0;
		std::memset(g_wobAgeHist, 0, sizeof g_wobAgeHist);
		g_reprojFallback = 0;
	} else if (!std::strcmp(op, "log")) {
		g_wobLog = args.ArgC() > 2 ? std::atoi(args[2]) : 60;
	} else if (!std::strcmp(op, "stop")) {
		g_wobOn = false;
	} else {
		WobbleReport(args.ArgC() > 2 ? args[2] : "wobble", args.ArgC() > 3 ? (float)std::atof(args[3]) : 0.0f, args.ArgC() > 4 ? (float)std::atof(args[4]) : 1.5f);
	}
}

static void DrawView(const CViewSetup *view, const Stream *S, int clipPortal, bool over) {
	if (!S) {
		if (clipPortal < 0 && !over) DrawFarBlack();
		else if (clipPortal < 0) RayGrid(view, nullptr, g_matBlack, -1);
		else {
			// the view out of portal clipPortal is seen through its partner: that portal's colour
			IMaterial *m = g_matStatic[1 - clipPortal] ? g_matStatic[1 - clipPortal] : g_matBlack;
			RayGrid(view, nullptr, m, clipPortal);
		}
		return;
	}
	float lag = AngleBetween(S->img.angles, view->angles);
	g_lagSum += lag, ++g_lagN;
	if (lag > g_lagMax) g_lagMax = lag;
	IMatRenderContext *rc = materialSystem->GetRenderContext();
	if (!rc) return;
	Vector tm(1, 1, 1);
	if (g_toneOverride) {
		tm = RcGetTone(rc);
		RcCall<_RcSetTM>(rc, slots::kRcSetToneMappingScaleLinear, Vector(1, 1, 1));
	}
	const TexSet &T = S->set[S->drawSet];
	IMaterial *m0 = g_srgb && T.matSrgb ? T.matSrgb : T.mat;
	if (clipPortal < 0) {
		const int modes[3] = {kMatView, kMatProjection, kMatModel};
		for (int m : modes) RcCall<_RcInt>(rc, slots::kRcMatrixMode, m), RcCall<_RcVoid>(rc, slots::kRcPushMatrix), RcCall<_RcVoid>(rc, slots::kRcLoadIdentity);
		RcCall<_RcVoid>(rc, slots::kRcBeginRender);
		FullScreenImage(*S, m0, over ? 0.5f : kFarZ, !over);
		if (!over) DrawOccluders(*S);
		if (!over && (g_pairInMc || g_occlPropsMain) && (g_pairDebug & 7) != 5) MainViewObjectOccluders(*S);
		if (g_occDebug > 0 && T.occl[g_occDebug]) {
			// debug: black, then only the pixels whose depth level passes the alpha test
			MeshBuilder mb(g_matBlack, PrimitiveType::QUADS, 1);
			mb.ForceDepth(false, false);
			Color black(0, 0, 0, 255);
			const float q[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
			for (auto &c : q) mb.Position(Vector(c[0], c[1], 0.1f)), mb.Color(black), mb.TexCoord(0, 0, 0), mb.AdvanceVertex();
			mb.Draw();
			const float tri[6][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, 1}, {1, -1}, {-1, -1}};
			OccluderTris(*S, T.occl[g_occDebug], tri, 2, 0.1f, false);
		}
		RcCall<_RcVoid>(rc, slots::kRcEndRender);
		for (int m : modes) RcCall<_RcInt>(rc, slots::kRcMatrixMode, m), RcCall<_RcVoid>(rc, slots::kRcPopMatrix);
		DriftProbe(*S);
		if (!over) WobbleProbe(*S, view);
	} else {
		RayGrid(view, S, (g_pairDebug & 7) == 3 && g_matMagenta ? g_matMagenta : (g_pairDebug & 7) == 5 && T.matTint ? T.matTint : m0, clipPortal);  // (debug 3: magenta, Portal 2's opening; 5: tinted, as the pair views)
	}
	if (g_toneOverride) RcCall<_RcSetTM>(rc, slots::kRcSetToneMappingScaleLinear, tm);
	RcCall<_RcVoid>(rc, slots::kRcRelease);
}

// Fraction of the screen box whose rays fall outside the image (stretched edge texels there).
static float Uncovered(const Stream &S, const float box[4]) {
	int out = 0, n = 0;
	for (int j = 0; j < 9; ++j)
		for (int i = 0; i < 16; ++i) {
			float x = box[0] + (box[2] - box[0]) * (i + 0.5f) / 16, y = box[1] + (box[3] - box[1]) * (j + 0.5f) / 9, tu, tv;
			bool o = true;
			if (ImageUV(S, x, y, tu, tv, &o) && !o) {
			} else {
				++out;
			}
			++n;
		}
	return n ? (float)out / n : 0;
}

static int g_traceComposite = 0;
static int g_matchLog = 0;
static void RecordPortalView(const Stream &S);  // (validation probes, below)
// Every non-skybox 3D view, at the start of its translucent pass (render.cpp). One rule for all of
// them: a view whose camera is in the region shows Minecraft for that camera. True when it did.
bool CompositeView(const CViewSetup *view, bool isMainByAngles) {
	if (!g_enabled || !g_streams[0].set[0].mat) return false;
	const bool isMainCam = isMainByAngles && (view->origin - g_ovOrigin).Length() < 0.5f;
	if (isMainByAngles) {
		// Portal 2's main camera: its matrices, for the background depth and the probes
		IMatRenderContext *rc = materialSystem->GetRenderContext();
		if (rc) {
			RcCall<_RcGetMatrix>(rc, slots::kRcGetMatrix, (int)kMatView, &g_mainView);
			RcCall<_RcGetMatrix>(rc, slots::kRcGetMatrix, (int)kMatProjection, &g_mainProj);
			RcCall<_RcVoid>(rc, slots::kRcRelease);
			g_haveMainMatrices = true;
			BackgroundZ();
		}
		float dv = (view->origin - g_ovOrigin).Length();
		g_ovDiffMax = std::fmax(g_ovDiffMax, dv), g_ovDiffSum += dv, ++g_ovDiffN;
	}
	// Chell's eye at (within a unit of) or behind the plane of a portal that leads into the region, the
	// tick before she is teleported: Portal 2 renders that view as the void (white, only the viewmodel
	// on top). It is the region camera on the far side of the portal: that camera's
	// image goes over the whole view.
	CViewSetup through;
	bool intoPortal = false;
	// (Portal 2's own camera only, recognised by position: at this frame's eye, which is outside the
	// region. A portal view's virtual camera also sits behind a portal's plane, e.g. the view through
	// the cliff portal has its camera inside the pit floor, but only while Chell is in the region; the
	// rendered angles can differ from the engine's on this tick, so they are not used)
	const bool atEye = (view->origin - g_ovOrigin).Length() < 2.0f && !ClipRegionContains(g_ovOrigin);
	if (atEye && !ClipRegionContains(view->origin)) {
		for (int i = 0; i < 2 && !intoPortal; ++i) {
			const int j = 1 - i;
			if (!g_portalSet[i] || !g_portalSet[j] || ClipRegionContains(g_portalPos[i]) || !ClipRegionContains(g_portalPos[j])) continue;
			const Vector &n = g_portalNormal[i], &c = g_portalPos[i];
			float d = Vector::DotProduct(view->origin - c, n);
			// (within a unit of the plane already: the gun's floor portal sits at -768.0 and the white
			// frames had the eye 0.1-0.5 u above it)
			if (d >= 1.0f || d < -64.0f) continue;
			Vector q = view->origin - c - n * d;
			float lx = Vector::DotProduct(q, PortalRight(i)) / 40.0f, ly = Vector::DotProduct(q, g_portalUp[i]) / 72.0f;
			if (lx * lx + ly * ly > 1.0f) continue;
			through = *view;
			ThroughPortal(i, j, view->origin, view->angles, through.origin, through.angles);
			intoPortal = true, ++g_intoPortalViews;
		}
		if (intoPortal) view = &through;
	}
	if (!ClipRegionContains(view->origin)) {
		if (g_traceComposite > 0)
			--g_traceComposite, console->Print("[bridge] cview f%llu: origin (%.1f %.1f %.1f) ang (%.1f %.1f %.1f) fov %.1f main %d (byAngles %d) | outside the region\n", (unsigned long long)g_hostFrame,
			                                view->origin.x, view->origin.y, view->origin.z, view->angles.x, view->angles.y, view->angles.z, view->fov, isMainCam, isMainByAngles);
		return false;
	}
	// (the main view by its origin: the gun's view kick or a punch turns the rendered angles away from the
	// engine's for a few frames; no portal view's camera sits at the main camera)
	if (g_pairInMc && (view->origin - g_ovOrigin).Length() >= 2.0f) {
		// a view through a portal of the pair Minecraft draws itself: the main camera's image, whose
		// opening already shows this view (same age, same stream; never a stream of its own)
		int sIdx = g_nCands > 0 && g_cands[0].depth == 0 ? g_cands[0].stream : -1;
		const Stream *S = sIdx >= 0 && g_streams[sIdx].haveImage && g_streams[sIdx].img.camGen == g_streams[sIdx].camGen ? &g_streams[sIdx] : nullptr;
		++g_regionViews, ++g_fvi.views, ++g_pairViews;
		if (!S) {
			++g_fvi.missing, ++g_missingViews;
			if (g_anyImageEver) ++g_missingAfterFirst, ++g_fallbackViews;
			const uint64_t chainAge = sIdx >= 0 ? g_hostFrame - g_streams[sIdx].genFrame : ~0ull;
			if (chainAge > 3) ++g_fvi.fallbackLate, ++g_fallbackLateViews;
			else ++g_fvi.fallbackEarly;
		} else {
			uint64_t age = g_hostFrame - S->img.frame;
			float pe = (S->img.origin - g_ovOrigin).Length();
			bool stale = age > (uint64_t)g_ageLimit, mismatch = pe > g_posErrLimit;
			g_fvi.stale += stale, g_staleViews += stale;
			g_fvi.mismatch += mismatch, g_mismatchViews += mismatch;
			if (!stale && !mismatch) ++g_fvi.reprojected, ++g_reprojViews;
			if (age > g_fvi.age) g_fvi.age = age;
			++g_ageHist[1][std::min<uint64_t>(age, 11)];
			g_ageSecMax = std::max(g_ageSecMax, age), g_ageSecSum += age, ++g_ageSecN;
		}
		if (g_traceComposite > 0) {
			--g_traceComposite;
			console->Print("[bridge] cview f%llu: origin (%.1f %.1f %.1f) ang (%.1f %.1f %.1f) | pair view: main stream %d image %u (%llu frames old)\n", (unsigned long long)g_hostFrame,
			               view->origin.x, view->origin.y, view->origin.z, view->angles.x, view->angles.y, view->angles.z, sIdx, S ? S->img.id : 0,
			               S ? (unsigned long long)(g_hostFrame - S->img.frame) : 0ull);
		}
		double t0 = NowS();
		if (g_stage & 2) DrawPairView(view, S);
		g_drawS += NowS() - t0, ++g_drawN;
		return true;
	}
	RecordCurrent(view);
	// 1) this frame's predicted camera for this view (exact)
	int how = 0;  // 1 exact, 2 continuity, 3 another camera's image, 0 none
	int sIdx = -1, clip = -1;
	const Cand *cand = nullptr;
	{
		float best = 1e30f;
		for (int k = 0; k < g_nCands; ++k) {
			const Cand &c = g_cands[k];
			if (c.stream < 0) continue;
			float d = (view->origin - c.o).Length(), ang = AngleBetween(view->angles, c.a);
			if (d > 24.0f + 2.0f * g_streams[c.stream].camVel.Length() * (float)MeanFrameS() || ang > 20.0f) continue;
			if (d + 4 * ang < best) best = d + 4 * ang, cand = &c;
		}
		if (cand) {
			how = 1, sIdx = cand->stream;
			float d = (view->origin - cand->o).Length(), ang = AngleBetween(view->angles, cand->a);
			g_matchPosSum += d, ++g_matchN, g_matchPosMax = std::max(g_matchPosMax, d), g_matchAngMax = std::max(g_matchAngMax, ang);
		}
	}
	// 2) stream continuity (the camera moved after OverrideView: a transit between the two)
	if (sIdx < 0) {
		float best = 1e30f;
		for (int k = 0; k < kStreams; ++k) {
			const Stream &st = g_streams[k];
			if (!st.camValid) continue;
			uint64_t gap = g_hostFrame - st.camFrame + 1;
			if (gap > 30) continue;
			float d = (view->origin - st.camO).Length(), ang = AngleBetween(view->angles, st.camA);
			bool samePoint = d <= 60.0f * gap;
			if (d > PosTol(st, gap) || (!samePoint && ang > 60.0f * gap)) continue;
			float score = d + (samePoint ? 0.0f : 4 * ang);
			if (score < best) best = score, sIdx = k;
		}
		if (sIdx >= 0) how = 2;
		if (g_matchLog > 0) --g_matchLog, console->Print("[bridge] composite: view at (%.1f %.1f %.1f) ang (%.1f %.1f %.1f) not predicted: continuity stream %d, clip %d\n", view->origin.x, view->origin.y,
		                                              view->origin.z, view->angles.x, view->angles.y, view->angles.z, sIdx, clip);
	}
	// placement by the view's own geometry: a camera behind a region portal's plane looks out of it (a
	// portal view, or Chell's camera on the frame she comes out of it); any other region camera gets
	// the background. (Not from the matched camera: on a crossing frame the view through the pit portal
	// matches Chell's camera after the teleport, a few units away on the other side of the plane.)
	// (a pair inside Minecraft: the main camera is never a portal view, even where it stands behind the other
	// portal's plane, two portals on parallel walls; only inside an opening, the frame Chell comes out of it)
	clip = g_pairInMc && (view->origin - g_ovOrigin).Length() < 2.0f ? InOpeningBehind(view->origin) : PortalBehind(view->origin);
	if (clip < 0 && !isMainByAngles) {
		// a portal view whose camera is just in front of the plane, inside the opening (Chell's eye a
		// fraction of a unit past the pit portal's plane, the tick before she is teleported): still a
		// view through that portal (Portal 2's clip plane and stencil), never the background path
		for (int i = 0; i < 2 && clip < 0; ++i) {
			if (!g_portalSet[i] || !ClipRegionContains(g_portalPos[i])) continue;
			const Vector &n = g_portalNormal[i], &c = g_portalPos[i];
			float d = Vector::DotProduct(view->origin - c, n);
			if (d < -1.0f || d > 16.0f) continue;  // (overlaps PortalBehind's 0.5 u)
			Vector q = view->origin - c - n * d;
			float lx = Vector::DotProduct(q, PortalRight(i)) / 40.0f, ly = Vector::DotProduct(q, g_portalUp[i]) / 72.0f;
			if (lx * lx + ly * ly <= 1.0f) clip = i, ++g_entrySideViews;
		}
	}
	// the image: only this view's own stream's, from its own camera chain. Another camera's image
	// reprojected into this view smears (MC<->MC pairs have more region views than Minecraft renders
	// per frame). Without one: Portal 2's own recursion-limit look (the portal's static colour) for a
	// portal view, black for the camera itself; never the region's background.
	const Stream *S = sIdx >= 0 && g_streams[sIdx].haveImage && g_streams[sIdx].img.camGen == g_streams[sIdx].camGen ? &g_streams[sIdx] : nullptr;
	if (!S && sIdx >= 0 && g_streams[sIdx].haveImage) ++g_foreignAvoided;
	// per-view checks
	++g_regionViews, ++g_fvi.views;
	if (S && sIdx >= 0 && S != &g_streams[sIdx]) ++g_foreignDrawn;  // (cannot happen: asserted)
	if (!S && g_anyImageEver) ++g_fallbackViews;
	if (!S) {
		++g_fvi.missing, ++g_missingViews;
		if (g_anyImageEver) ++g_missingAfterFirst;
		const uint64_t chainAge = sIdx >= 0 ? g_hostFrame - g_streams[sIdx].genFrame : ~0ull;
		if (chainAge > 3) ++g_fvi.fallbackLate, ++g_fallbackLateViews;
		else ++g_fvi.fallbackEarly;
	} else {
		g_anyImageEver = true;
		uint64_t age = g_hostFrame - S->img.frame;
		float pe = (S->img.origin - view->origin).Length(), ae = AngleBetween(S->img.angles, view->angles);
		// a portal view only needs its opening's box (its stencil hides the rest)
		float box[4] = {-1, -1, 1, 1};
		if (clip >= 0) std::memcpy(box, cand ? cand->box : S->camBox, sizeof box);
		float unc = Uncovered(*S, box);
		// the camera turned further than the image covers (Chell's camera after a floor-to-wall transit
		// turns up to ~80 deg at once): hold the view as rendered until its own image arrives
		if (unc > 0.5f) {
			g_holdView = true, ++g_heldViews, ++g_fvi.held;
			const_cast<Stream *>(S)->heldFrame = g_hostFrame;
			unc = Uncovered(*S, box);
		}
		const int depth = cand ? cand->depth : S->camDepth;
		bool stale = age > (uint64_t)(depth >= 2 ? 2 * g_ageLimit : g_ageLimit);  // (recursion views are refreshed 3x less often)
		// mismatch: another camera's image, or one rendered more than 2 blocks away; reprojection
		// gaps (uncovered edges) are reprojection error, reported with the reprojected frames
		bool mismatch = how == 3 || pe > g_posErrLimit;
		g_fvi.stale += stale, g_staleViews += stale;
		g_fvi.mismatch += mismatch, g_mismatchViews += mismatch;
		if (!stale && !mismatch) {
			if (how == 1) ++g_fvi.exact, ++g_exactViews;
			else ++g_fvi.reprojected, ++g_reprojViews;
		}
		if (age > g_fvi.age) g_fvi.age = age;
		{
			const bool pri = depth == 0;  // (the camera's own view; 1 a portal view; recursion views counted apart)
			if (depth <= 1) {
				const int k = pri ? 0 : 1;
				++g_ageHist[k][std::min<uint64_t>(age, 11)];
				if (pri) g_agePriMax = std::max(g_agePriMax, age), g_agePriSum += age, ++g_agePriN;
				else g_ageSecMax = std::max(g_ageSecMax, age), g_ageSecSum += age, ++g_ageSecN;
			} else {
				g_ageRecMax = std::max(g_ageRecMax, age), ++g_ageRecN;
			}
		}
		if (pe >= g_fvi.poseErr) g_fvi.poseErr = pe, g_fvi.stream = S->index, g_fvi.set = S->drawSet, g_fvi.imgW = S->imgW, g_fvi.imgH = S->imgH;
		g_fvi.angErr = std::max(g_fvi.angErr, ae);
		g_fvi.uncovered = std::max(g_fvi.uncovered, unc);
	}
	if (g_traceComposite > 0) {
		--g_traceComposite;
		console->Print("[bridge] cview f%llu: origin (%.1f %.1f %.1f) ang (%.1f %.1f %.1f) fov %.1f main %d | match %d stream %d clip %d image %d (request %u, %llu frames old)\n",
		               (unsigned long long)g_hostFrame, view->origin.x, view->origin.y, view->origin.z, view->angles.x, view->angles.y, view->angles.z, view->fov, isMainCam, how, sIdx,
		               clip, S ? S->index : -1, S ? S->img.id : 0, S ? (unsigned long long)(g_hostFrame - S->img.frame) : 0ull);
	}
	if (clip < 0) g_regionCamThisFrame = true;
	else if (S && !intoPortal) RecordPortalView(*S);
	if (S && (clip < 0 || g_drawStream < 0)) g_drawStream = S->index;  // the probes' image: the camera's own, else a portal view's
	const bool over = intoPortal;
	if (intoPortal) clip = -1;
	double t0 = NowS();
	if (g_stage & 2) DrawView(view, S, clip, over);
	g_holdView = false;
	g_drawS += NowS() - t0, ++g_drawN;
	return true;
}

// ---- views of a pair Minecraft draws itself (v15) -----------------------------------------------
// Minecraft's image already shows what each portal of the pair shows, at the main camera's pose, so
// every region view draws that one image screen-mapped (the main camera's reprojection): Portal 2's
// stencil keeps a portal view's draw to its opening, where the image's pixels are exactly that
// portal's contents. In a portal view the image goes at the background depth of THAT view's own
// projection (its oblique near plane tilts the depth range), with the depth test, so what Portal 2
// draws in it (Chell's body, cubes) stays in front; the occlusion layers hide those objects where
// Minecraft's terrain is nearer (the image's alpha inside an opening is the virtual camera's depth).
struct ViewGeo {
	CurView cur;  // the view's own camera
	VMatrix vp;   // its projection x view (Portal 2's matrices for this view)
	bool haveVp = false;
	// world-space vertices drawn with the view's own matrices (a portal view: Portal 2 clips it with
	// a user clip plane in world space, which would cut NDC geometry drawn with identity matrices);
	// else NDC vertices under identity matrices with the depth from vp
	bool world = false;
};
static void TakeViewGeo(const CViewSetup *view, ViewGeo &g) {
	AngleBasis(view->angles, g.cur.f, g.cur.r, g.cur.u);
	g.cur.origin = view->origin;
	float aspect = view->m_flAspectRatio > 0.1f ? view->m_flAspectRatio : (float)view->width / (float)(view->height ? view->height : 1);
	g.cur.tx = std::tan(view->fov * (float)M_PI / 360.0f), g.cur.ty = g.cur.tx / aspect;
	g.cur.valid = true;
	g.haveVp = false;
	IMatRenderContext *rc = materialSystem->GetRenderContext();
	if (!rc) return;
	VMatrix v, p;
	RcCall<_RcGetMatrix>(rc, slots::kRcGetMatrix, (int)kMatView, &v);
	RcCall<_RcGetMatrix>(rc, slots::kRcGetMatrix, (int)kMatProjection, &p);
	RcCall<_RcVoid>(rc, slots::kRcRelease);
	Mul(p, v, g.vp);
	g.haveVp = true;
}
// NDC depth, in this view's own projection, of the point at view depth D along the ray of screen (x, y).
static float ZAt(const ViewGeo &g, float x, float y, float D) {
	if (!g.haveVp) return kFarZ;
	Vector w = g.cur.origin + (g.cur.f + g.cur.r * (x * g.cur.tx) + g.cur.u * (y * g.cur.ty)) * D;
	float c[4];
	for (int i = 0; i < 4; ++i) c[i] = g.vp.m[i][0] * w.x + g.vp.m[i][1] * w.y + g.vp.m[i][2] * w.z + g.vp.m[i][3];
	if (c[3] <= 1e-3f) return 0.99999f;
	float z = c[2] / c[3];
	if (!(z == z)) return 0.99999f;
	return std::fmax(-1.0f, std::fmin(0.99999f, z));
}
static Vector GeoPos(const ViewGeo &g, float x, float y, float D) {
	if (g.world) return g.cur.origin + (g.cur.f + g.cur.r * (x * g.cur.tx) + g.cur.u * (y * g.cur.ty)) * D;
	return Vector(x, y, ZAt(g, x, y, D));
}
// Portal 2 objects in the region the occlusion layers cover: physics props, and Chell's body in
// views where Portal 2 draws it (portal views).
int PropCentresInRegion(Vector *out, int max);  // propcollide.cpp
static int g_objOccl = 0, g_objLayers = 0;
static void DrawObjectOccluders(const Stream &S, const ViewGeo &geo, bool chell) {
	if (!g_occlusion || !S.depthAlpha || S.small || !geo.haveVp) return;
	Vector c[24];
	float half[24][3];
	int n = PropCentresInRegion(c, 20);
	for (int k = 0; k < n; ++k) half[k][0] = half[k][1] = half[k][2] = 26.0f;  // a cube (36 u) at any orientation
	if (chell && n < 24) {
		Vector o = PlayerOrigin();
		if (ClipRegionContains(o)) c[n] = o + Vector(0, 0, 36), half[n][0] = half[n][1] = 20.0f, half[n][2] = 38.0f, ++n;
	}
	const float poseErr = (S.img.origin - g_mainCur.origin).Length();
	for (int k = 0; k < n; ++k) {
		float x0 = 2, y0 = 2, x1 = -2, y1 = -2, dmin = 1e30f, dmax = 0;
		bool ok = true;
		for (int q = 0; q < 8 && ok; ++q) {
			Vector p = c[k] + Vector((q & 1) ? half[k][0] : -half[k][0], (q & 2) ? half[k][1] : -half[k][1], (q & 4) ? half[k][2] : -half[k][2]) - geo.cur.origin;
			float d = Vector::DotProduct(p, geo.cur.f);
			if (d < 8.0f) ok = false;
			float sx = Vector::DotProduct(p, geo.cur.r) / (d * geo.cur.tx), sy = Vector::DotProduct(p, geo.cur.u) / (d * geo.cur.ty);
			x0 = std::min(x0, sx), x1 = std::max(x1, sx), y0 = std::min(y0, sy), y1 = std::max(y1, sy);
			dmin = std::min(dmin, d), dmax = std::max(dmax, d);
		}
		if (!ok) continue;
		x0 = std::max(-1.02f, x0 - 0.01f), y0 = std::max(-1.02f, y0 - 0.01f), x1 = std::min(1.02f, x1 + 0.01f), y1 = std::min(1.02f, y1 + 0.01f);
		if (x0 >= x1 || y0 >= y1) continue;
		++g_objOccl;
		int aFar = EncodeDepthAlpha(dmax / (1 + g_occEps)), aNear = std::min(255, EncodeDepthAlpha(dmin / (1 + g_occEps)) + 1);
		for (int a = aFar; a <= aNear; ++a) {
			IMaterial *occl = S.set[S.drawSet].occl[a];
			float D = DecodeDepthAlpha(a) * (1 + g_occEps) + poseErr;
			if (D > dmax + 1 || !occl) continue;
			const float q[4][2] = {{x0, y1}, {x1, y1}, {x1, y0}, {x0, y0}};
			float z[4];
			bool zok = true;
			for (int v = 0; v < 4; ++v) z[v] = ZAt(geo, q[v][0], q[v][1], D), zok = zok && (geo.world || z[v] > 0);
			if (!zok) continue;
			MeshBuilder mb(occl, PrimitiveType::QUADS, 1);
			mb.ForceDepth(true, true);
			for (int v = 0; v < 4; ++v) {
				float tu, tv;
				if (!ImageUV(S, q[v][0], q[v][1], tu, tv)) tu = tv = 0;
				mb.Position(geo.world ? GeoPos(geo, q[v][0], q[v][1], D) : Vector(q[v][0], q[v][1], z[v]));
				mb.Color(Color(255, 255, 255, 255));
				mb.TexCoord(0, tu, tv);
				mb.AdvanceVertex();
			}
			mb.Draw();
			++g_objLayers;
		}
	}
}
// A portal view of the pair (or any non-main region view while the pair is in Minecraft).
static void DrawPairView(const CViewSetup *view, const Stream *S) {
	ViewGeo geo;
	TakeViewGeo(view, geo);
	g_cur = g_mainCur;  // the image's own screen mapping: the main camera's
	if (!S) {
		IMaterial *m = g_matBlack;
		RayGrid(view, nullptr, m, -1);
		return;
	}
	IMatRenderContext *rc = materialSystem->GetRenderContext();
	if (!rc) return;
	Vector tm(1, 1, 1);
	if (g_toneOverride) {
		tm = RcGetTone(rc);
		RcCall<_RcSetTM>(rc, slots::kRcSetToneMappingScaleLinear, Vector(1, 1, 1));
	}
	const TexSet &T = S->set[S->drawSet];
	IMaterial *m0 = g_srgb && T.matSrgb ? T.matSrgb : T.mat;
	geo.world = true;
	if (g_traceComposite > 0 || g_pairDebug >= 8) {
		if (g_pairDebug >= 8) --g_pairDebug;
		console->Print("[bridge] pair view: origin (%.1f %.1f %.1f) vp %d, z at centre %.6f / corners %.6f %.6f, bg depth %.0f, kFarZ %.6f; vp row2 (%.4f %.4f %.4f %.4f) row3 (%.4f %.4f %.4f %.4f)\n",
		               view->origin.x, view->origin.y, view->origin.z, geo.haveVp, ZAt(geo, 0, 0, g_bgDepth), ZAt(geo, -1, -1, g_bgDepth), ZAt(geo, 1, 1, g_bgDepth), g_bgDepth, kFarZ,
		               geo.vp.m[2][0], geo.vp.m[2][1], geo.vp.m[2][2], geo.vp.m[2][3], geo.vp.m[3][0], geo.vp.m[3][1], geo.vp.m[3][2], geo.vp.m[3][3]);
	}
	if ((g_pairDebug & 7) == 3 && g_matMagenta) FullScreenImage(*S, g_matMagenta, kFarZ, false, &geo);  // (debug 3: Portal 2's portal-view pixels in magenta)
	else if ((g_pairDebug & 7) == 5 && T.matTint) FullScreenImage(*S, T.matTint, kFarZ, true, &geo);  // (debug 5: tinted green: Portal 2's stencil, for pcb_measure_gap)
	else FullScreenImage(*S, m0, kFarZ, (g_pairDebug & 7) != 1, &geo);  // (debug 1: no depth test)
	if ((g_pairDebug & 7) != 5) DrawObjectOccluders(*S, geo, true);  // (debug 5: none; Chell's layer is the image untinted, read as overshoot)
	if (g_toneOverride) RcCall<_RcSetTM>(rc, slots::kRcSetToneMappingScaleLinear, tm);
	RcCall<_RcVoid>(rc, slots::kRcRelease);
}

static void MainViewObjectOccluders(const Stream &S) {
	if (!g_haveMainMatrices || !g_cur.valid) return;
	ViewGeo geo;
	geo.cur = g_cur;
	Mul(g_mainProj, g_mainView, geo.vp);
	geo.haveVp = true;
	const CurView keep = g_cur;
	DrawObjectOccluders(S, geo, false);
	g_cur = keep;
}

// ---- validation probes (read the finished frame at the paint stage) ----------------------------
static const Stream *MainImage() { return g_drawStream >= 0 ? &g_streams[g_drawStream] : nullptr; }
static bool ImageReady(const Stream *S) { return S && S->cur >= 0 && S->bufDone[S->cur].load(std::memory_order_acquire) == S->img.id; }
static bool ReadPixel(IMatRenderContext *rc, int x, int y, uint8_t out[4]) {
	if (x < 0 || y < 0 || x >= 1600 || y >= 900) return false;
	RcCall<_RcReadPixels>(rc, slots::kRcReadPixels, x, y, 1, 1, out, IMAGE_FORMAT_RGBA8888);
	return true;
}
static void NdcToPixel(float x, float y, int &px, int &py) { px = (int)std::lround((x + 1) * 0.5f * 1600), py = (int)std::lround((1 - y) * 0.5f * 900); }

// The floor portal's opening on screen: brightness of the half towards the portal's "up" edge vs
// the other half. Looking down with the view aligned to that up, Minecraft's sky must be on top.
static void SkyUpProbe(int expectSkyUp) {
	int pi = ClipRegionPortal();
	int near = pi < 0 ? -1 : 1 - pi;
	char buf[256];
	if (near < 0 || !g_portalSet[near] || !g_haveMainMatrices) return TestCheck(false, "sky up", "no portal pair / matrices");
	Vector c = g_portalPos[near], nrm = g_portalNormal[near];
	Vector f, r, u;
	for (void *p : EntFind("prop_portal"))
		if ((EntVec(p, "origin") - c).Length() < 2) {
			Vector a = EntVec(p, "angles");
			AngleBasis(QAngle{a.x, a.y, a.z}, f, r, u);
		}
	IMatRenderContext *rc = materialSystem->GetRenderContext();
	if (!rc) return;
	double top = 0, bot = 0;
	int nt = 0, nb = 0;
	for (int k = -4; k <= 4; ++k)
		for (int side = -1; side <= 1; side += 2) {
			Vector p = c + nrm * 1.0f + u * (side * 30.0f) + r * (k * 3.0f);
			float x, y, z;
			if (!Project(p, x, y, z)) continue;
			int px, py;
			NdcToPixel(x, y, px, py);
			uint8_t px4[4];
			if (!ReadPixel(rc, px, py, px4)) continue;
			double b = px4[0] + px4[1] + px4[2];
			(side > 0 ? top : bot) += b, ++(side > 0 ? nt : nb);
		}
	RcCall<_RcVoid>(rc, slots::kRcRelease);
	top /= nt ? nt : 1, bot /= nb ? nb : 1;
	std::snprintf(buf, sizeof buf, "brightness towards the portal's up edge %.0f, opposite edge %.0f (sky expected %s)", top, bot, expectSkyUp ? "up" : "down");
	TestCheck(expectSkyUp ? top > bot + 30 : bot > top + 30, "sky orientation in the floor portal", buf);
}

// From inside the region facing the cliff portal: the opening must show Portal 2's chamber, not
// Minecraft. Compares the frame with Minecraft's image at the same screen points.
static void HoleProbe() {
	int pi = ClipRegionPortal();
	const Stream *S = MainImage();
	if (pi < 0 || !g_haveMainMatrices || !S) return TestCheck(false, "cliff portal shows the chamber", "no region portal / matrices / image");
	IMatRenderContext *rc = materialSystem->GetRenderContext();
	if (!rc) return;
	int same = 0, n = 0;
	char buf[200];
	for (int k = -2; k <= 2; ++k)
		for (int j = -2; j <= 2; ++j) {
			Vector p = g_portalPos[pi] + g_portalNormal[pi] * 1.0f + Vector(0, 0, 10.0f * j) + Vector(g_portalNormal[pi].y, -g_portalNormal[pi].x, 0) * (5.0f * k);
			float x, y, z;
			if (!Project(p, x, y, z) || std::fabs(x) > 1 || std::fabs(y) > 1) continue;
			int px, py;
			NdcToPixel(x, y, px, py);
			uint8_t f4[4];
			float tu, tv;
			if (!ReadPixel(rc, px, py, f4) || !ImageUV(*S, x, y, tu, tv)) continue;
			uint8_t m[4];
			ImagePx(*S, tu, tv, m);
			int diff = std::abs(f4[0] - m[0]) + std::abs(f4[1] - m[1]) + std::abs(f4[2] - m[2]);
			same += diff < 30, ++n;
		}
	RcCall<_RcVoid>(rc, slots::kRcRelease);
	std::snprintf(buf, sizeof buf, "%d of %d points inside the opening look like Minecraft's image", same, n);
	TestCheck(n >= 10 && same * 4 < n, "cliff portal shows the chamber", buf);
}

// ---- whole-frame analysis (mask correctness, white flashes) --------------------------------------
// The finished frame is sampled every 4th pixel. Each sample's camera ray is tested against the
// region (cliff) portal's opening: well inside it the frame must show Portal 2's chamber (differ from
// Minecraft's image there), well outside it Minecraft. White samples (the region's sky_white walls)
// must never show. Portal 2's render context is queued: a ReadPixels issued from the main thread
// returns before the render thread has run it (the buffer then still held the previous frame: frame
// N's pixels were judged with frame N+1's camera). The per-frame watch therefore reads the frame on
// the render thread, in order after the frame's own draws, and analyses it at the next paint against
// a snapshot of that frame's state (camera, images, view checks).
struct FrameStats {
	int occluded = 0;
	int samples = 0, white = 0;
	int inside = 0, insideNotMc = 0;
	int outside = 0, outsideNotMc = 0;
	bool opening = false;
	bool mainInRegion = false;
	float eyeDist = 0;
};
struct ImgSnap {
	bool ok = false;
	ReqInfo img;
	int imgW = 0, imgH = 0;
	float tw = 1, th = 1, proxyDepth = 9000;
	const uint8_t *buf = nullptr;
	const uint8_t *depth = nullptr;  // buf when it holds this image's own depth alpha (per-pixel reprojection)
	bool held = false;
};
struct PvSnap {  // a portal view drawn this frame: its camera and its image (white-sample checks)
	CurView cur;
	ImgSnap img;
};
struct FrameSnap {
	uint64_t frame = 0;
	PvSnap pv[4];
	int nPv = 0;
	CurView cur;
	CurView mainCam;  // Portal 2's own camera this frame (OverrideView)
	bool regionCam = false;
	ImgSnap img;
	FrameViewInfo fvi;
	Vector ov;
	float pitch = 0, yaw = 0;
	int approach = -1;
};
static PvSnap g_pvThis[4];
static int g_nPvThis = 0;
static FrameSnap TakeSnap() {
	FrameSnap f;
	f.nPv = g_nPvThis;
	for (int k = 0; k < g_nPvThis; ++k) f.pv[k] = g_pvThis[k];
	f.frame = g_hostFrame, f.cur = g_cur, f.regionCam = g_regionCamThisFrame, f.fvi = g_fvi, f.ov = g_ovOrigin;
	AngleBasis(g_ovAngles, f.mainCam.f, f.mainCam.r, f.mainCam.u);
	f.mainCam.origin = g_ovOrigin, f.mainCam.tx = g_ovTanX, f.mainCam.ty = g_ovTanY, f.mainCam.valid = true;
	float area;
	f.approach = TransitApproach(f.pitch, f.yaw, area);
	const Stream *S = MainImage();
	if (S && ImageReady(S)) {
		ImgSnap &i = f.img;
		i.ok = true, i.img = S->img, i.imgW = S->imgW, i.imgH = S->imgH, i.tw = TW(*S), i.th = TH(*S), i.proxyDepth = S->proxyDepth, i.buf = S->buf[S->cur];
		i.depth = DepthBuf(*S);
		i.held = S->heldFrame == g_hostFrame;
	} else if (S && S->cur >= 0 && S->bufReq[S->cur ^ 1] != 0 && S->bufDone[S->cur ^ 1].load(std::memory_order_acquire) == S->bufReq[S->cur ^ 1]) {
		// the newest image is still being copied out: the previous one (a frame older, the newest one's pose;
		// only the white check's "is Minecraft bright there" uses it)
		ImgSnap &i = f.img;
		i.ok = true, i.img = S->img, i.imgW = S->imgW, i.imgH = S->imgH, i.tw = TW(*S), i.th = TH(*S), i.proxyDepth = S->proxyDepth, i.buf = S->buf[S->cur ^ 1];
		i.held = S->heldFrame == g_hostFrame;
	}
	return f;
}
static void ResetPortalViewSnaps() { g_nPvThis = 0; }
static void RecordPortalView(const Stream &S) {
	if (g_nPvThis >= 4 || !ImageReady(&S)) return;
	PvSnap &p = g_pvThis[g_nPvThis++];
	p.cur = g_cur;
	ImgSnap &i = p.img;
	i.ok = true, i.img = S.img, i.imgW = S.imgW, i.imgH = S.imgH, i.tw = TW(S), i.th = TH(S), i.proxyDepth = S.proxyDepth, i.buf = S.buf[S.cur];
	i.depth = DepthBuf(S);
	i.held = false;
}
static bool SnapUV(const FrameSnap &f, float x, float y, float &u, float &v) {
	const ImgSnap &i = f.img;
	return i.ok && ImageUVCore(f.cur, i.img, i.imgW, i.imgH, i.tw, i.th, i.proxyDepth, i.held, x, y, u, v, nullptr, i.depth);
}
static void SnapPx(const ImgSnap &i, float tu, float tv, uint8_t out[4]) {
	int x = std::max(0, std::min(i.imgW - 1, (int)(tu * i.tw))), y = std::max(0, std::min(i.imgH - 1, (int)(tv * i.th)));
	const uint8_t *p = i.buf + ((size_t)y * kTexW + x) * 4;
	bool bgra = g_texFmt == IMAGE_FORMAT_BGRA8888;
	out[0] = bgra ? p[2] : p[0], out[1] = p[1], out[2] = bgra ? p[0] : p[2], out[3] = p[3];
}
// The frame being analysed (a synchronous read, or a render-thread readback).
static const uint8_t *g_aPx = nullptr;
static int g_aW = 0, g_aH = 0;
static std::vector<uint8_t> g_frameBuf;
static bool ReadFrame() {  // (one-shot probes with a still camera)
	int w = 0, h = 0;
	engine.GetScreenSize(engine.client->ThisPtr(), w, h);
	if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return false;
	g_frameBuf.resize((size_t)w * h * 4);
	IMatRenderContext *rc = materialSystem->GetRenderContext();
	if (!rc) return false;
	RcCall<_RcReadPixels>(rc, slots::kRcReadPixels, 0, 0, w, h, g_frameBuf.data(), IMAGE_FORMAT_RGBA8888);
	RcCall<_RcVoid>(rc, slots::kRcRelease);
	g_aPx = g_frameBuf.data(), g_aW = w, g_aH = h;
	return true;
}
struct ReadBack {
	std::vector<uint8_t> px;
	int w = 0, h = 0;
	std::atomic<int> state{0};  // 0 free, 1 queued, 2 read
	FrameSnap snap;
};
static ReadBack g_rb[2];
static int g_rbNext = 0, g_rbSkipped = 0;
static void QueueReadBack() {
	ReadBack &rb = g_rb[g_rbNext];
	if (rb.state.load() != 0) return void(++g_rbSkipped);  // (the previous read of this buffer not done yet)
	int w = 0, h = 0;
	engine.GetScreenSize(engine.client->ThisPtr(), w, h);
	if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return;
	rb.px.resize((size_t)w * h * 4), rb.w = w, rb.h = h;
	rb.snap = TakeSnap();
	rb.state = 1;
	g_rbNext ^= 1;
	RunOnRenderThread(
	    [](void *arg) {
		    auto *r = static_cast<ReadBack *>(arg);
		    IMatRenderContext *rc = materialSystem->GetRenderContext();
		    if (rc) RcCall<_RcReadPixels>(rc, slots::kRcReadPixels, 0, 0, r->w, r->h, r->px.data(), IMAGE_FORMAT_RGBA8888), RcCall<_RcVoid>(rc, slots::kRcRelease);
		    r->state = 2;
	    },
	    &rb);
}
static float OpeningScale(const CurView &cur, int pi, float x, float y) {
	Vector d = cur.f + cur.r * (x * cur.tx) + cur.u * (y * cur.ty);
	Vector c = g_portalPos[pi], n = g_portalNormal[pi];
	float denom = Vector::DotProduct(d, n);
	if (denom > -1e-5f) return -1;
	float t = Vector::DotProduct(c - cur.origin, n) / denom;
	if (t <= 0) return -1;
	Vector q = cur.origin + d * t - c;
	// the opening's own axes (its long axis is the portal's up)
	float lx = Vector::DotProduct(q, PortalRight(pi)) / g_openW, ly = Vector::DotProduct(q, g_portalUp[pi]) / g_openH;
	return std::sqrt(lx * lx + ly * ly);
}
static inline const uint8_t *Px(int x, int y) { return &g_aPx[((size_t)y * g_aW + x) * 4]; }
static inline bool IsWhite(const uint8_t *f) { return f[0] >= 250 && f[1] >= 250 && f[2] >= 250; }
static inline bool IsChamber(const uint8_t *f) {
	int mx = std::max(f[0], std::max(f[1], f[2])), mn = std::min(f[0], std::min(f[1], f[2]));
	return mx < 110 && mx - mn < 40;
}
static bool g_markInside = false;
static std::vector<int> g_marks;
static void AnalyzeFrame(const FrameSnap &sn, FrameStats &st) {
	st = FrameStats{};
	g_marks.clear();
	if (!g_aPx) return;
	const int w = g_aW, h = g_aH;
	const CurView &cur = sn.cur;
	int pi = ClipRegionPortal();
	// the opening shows the chamber only when the other portal is in the chamber
	if (pi >= 0 && g_portalSet[1 - pi] && ClipRegionContains(g_portalPos[1 - pi])) pi = -1;
	st.mainInRegion = sn.regionCam;
	bool geo = st.mainInRegion && cur.valid && pi >= 0;
	if (geo) st.eyeDist = Vector::DotProduct(cur.origin - g_portalPos[pi], g_portalNormal[pi]);
	geo = geo && st.eyeDist > 0;
	// white is the region's background only where a region view drew: no region view this frame, no
	// check; and only for samples whose ray goes into the region: from Portal 2's camera in the region,
	// everywhere but the openings of portals that lead to the chamber; from the chamber, inside the
	// openings of portals that lead into the region (Portal 2's own white lights stay out of it)
	const bool whiteCheck = sn.fvi.views > 0;
	const bool camIn = ClipRegionContains(sn.mainCam.origin);
	auto intoRegion = [&](float x, float y) {
		bool inOpening = false;
		for (int i = 0; i < 2 && !inOpening; ++i) {
			if (!g_portalSet[i] || !g_portalSet[1 - i] || ClipRegionContains(g_portalPos[i]) != camIn) continue;
			if (ClipRegionContains(g_portalPos[1 - i]) == camIn) continue;  // leads to the same side
			float sc = OpeningScale(sn.mainCam, i, x, y);
			inOpening = sc >= 0 && sc < 1.0f;
		}
		return camIn ? !inOpening : inOpening;
	};
	for (int py = 4; py < h - 4; py += 4)
		for (int px = 4; px < w - 4; px += 4) {
			const uint8_t *f = Px(px, py);
			++st.samples;
			float x = (px + 0.5f) / w * 2 - 1, y = 1 - (py + 0.5f) / h * 2;
			const bool gun = px > w * 0.45f && py > h * 0.52f;  // the portal gun is white
			if (whiteCheck && !gun && IsWhite(f) && intoRegion(x, y) && IsWhite(Px(px - 3, py)) && IsWhite(Px(px + 3, py)) && IsWhite(Px(px, py - 3)) && IsWhite(Px(px, py + 3))) {
				// not if Minecraft's image is bright around there (clouds, sun; Portal 2's bloom whitens them)
				bool cloud = false;
				float tu, tv;
				auto bright = [&](const ImgSnap &im) {
					// (bloom spreads a bright sun or haze well beyond its own texels)
					for (int dy = -15; dy <= 15; dy += 5)
						for (int dx = -15; dx <= 15; dx += 5) {
							uint8_t m[4];
							SnapPx(im, tu + dx / im.tw, tv + dy / im.th, m);
							// (Minecraft's clouds, sun and bright sky: Portal 2's bloom takes them to white; inside a
							// cloud the image is a ~160 grey fog that Portal 2's tonemap lifts to 255, run float3f)
							if (m[0] + m[1] + m[2] > 3 * 140) return true;
						}
					return false;
				};
				if (SnapUV(sn, x, y, tu, tv)) cloud = bright(sn.img);
				// (inside a portal opening: that portal view's own image, e.g. the sun seen through a floor portal)
				for (int k = 0; k < sn.nPv && !cloud; ++k) {
					bool o = true;
					const PvSnap &pv = sn.pv[k];
					if (ImageUVCore(pv.cur, pv.img.img, pv.img.imgW, pv.img.imgH, pv.img.tw, pv.img.th, pv.img.proxyDepth, false, x, y, tu, tv, &o, pv.img.depth) && !o) cloud = bright(pv.img);
				}
				if (!cloud) {
					++st.white;
					static int whiteDbg = 0;
					if (whiteDbg < 12) {
						++whiteDbg;
						float u2 = -1, v2 = -1;
						uint8_t m[4] = {0, 0, 0, 0};
						bool okUV = SnapUV(sn, x, y, u2, v2);
						if (okUV) SnapPx(sn.img, u2, v2, m);
						console->Print("[bridge] white sample at (%d %d) of %dx%d: screen %d %d %d; image uv %d (%.3f %.3f) of %dx%d texel %d %d %d a %d, img ok %d held %d\n", px, py, w, h, f[0], f[1], f[2], okUV, u2, v2,
						               sn.img.imgW, sn.img.imgH, m[0], m[1], m[2], m[3], sn.img.ok, sn.img.held);
					}
				}
			}
			if (!geo) continue;
			float sc = OpeningScale(cur, pi, x, y);
			bool in = sc >= 0 && sc < 0.75f, out = sc < 0 || sc > 1.6f;
			if (!in && !out) continue;
			if (in) {
				Vector dd = cur.f + cur.r * (x * cur.tx) + cur.u * (y * cur.ty);
				float t = Vector::DotProduct(g_portalPos[pi] - cur.origin, g_portalNormal[pi]) / Vector::DotProduct(dd, g_portalNormal[pi]);
				if (TerrainRayBlocked(cur.origin, cur.origin + dd * t + g_portalNormal[pi] * 2.0f)) {
					++st.occluded;
					continue;
				}
			}
			if (gun) continue;
			bool ch = IsChamber(f);
			if (out && ch) {
				float tu, tv;
				if (SnapUV(sn, x, y, tu, tv)) {
					uint8_t m[4];
					SnapPx(sn.img, tu, tv, m);
					ch = std::abs(f[0] - m[0]) + std::abs(f[1] - m[1]) + std::abs(f[2] - m[2]) > 60;
				}
			}
			if (in && g_markInside) g_marks.push_back(py * w + px);
			if (in) ++st.inside, st.insideNotMc += ch;
			else ++st.outside, st.outsideNotMc += ch;
		}
	st.opening = st.inside >= 12;
}
static void DumpFrame(const char *label, int n) {
	if (!g_aPx) return;
	char path[512];
	std::snprintf(path, sizeof path, "%s/shots/frames/%s_%03d.ppm", CacheDir().c_str(), label, n);
	FILE *fp = std::fopen(path, "wb");
	if (!fp) return;
	const int w = g_aW / 2, h = g_aH / 2;
	std::fprintf(fp, "P6\n%d %d\n255\n", w, h);
	std::vector<uint8_t> row((size_t)w * 3);
	std::vector<char> mark;
	if (!g_marks.empty()) {
		mark.assign((size_t)g_aW * g_aH, 0);
		for (int m : g_marks) mark[m] = 1;
	}
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			size_t i = (size_t)(y * 2) * g_aW + x * 2;
			bool mk = !mark.empty() && (mark[i] || (i >= 2 && mark[i - 2]) || (i >= (size_t)g_aW * 2 && mark[i - g_aW * 2]));
			for (int c = 0; c < 3; ++c) row[(size_t)x * 3 + c] = mk ? (c == 1 ? 0 : 255) : g_aPx[i * 4 + c];
		}
		std::fwrite(row.data(), 1, row.size(), fp);
	}
	std::fclose(fp);
}

static int g_maskProbe = 0, g_holeProbe = 0, g_skyUpProbe = -1, g_occProbe = 0, g_probeWait = 0;
static char g_maskLabel[64] = "mask";
static void MaskProbe() {
	FrameStats st;
	if (!ReadFrame()) return TestCheck(false, "mask", "no frame");
	FrameSnap sn = TakeSnap();
	g_markInside = true;
	AnalyzeFrame(sn, st);
	g_markInside = false;
	float whiteF = st.samples ? (float)st.white / st.samples : 1;
	float inF = st.inside ? (float)st.insideNotMc / st.inside : 1;
	float outF = st.outside ? (float)st.outsideNotMc / st.outside : 0;
	int openPx = st.inside * 16, maskedPx = (st.insideNotMc + st.outsideNotMc) * 16;
	char buf[400];
	std::snprintf(buf, sizeof buf,
	              "[%s] eye %.1f u from the plane, white %.2f%%, opening %d px visible (%.0f%% chamber-like) + %d px behind terrain, outside %d px (%.1f%% chamber-like), masked %d px = %.2f x the opening",
	              g_maskLabel, st.eyeDist, 100 * whiteF, openPx, 100 * inF, st.occluded * 16, st.outside * 16, 100 * outF, maskedPx, openPx ? (float)maskedPx / openPx : 0.0f);
	DumpFrame(g_maskLabel, 0);
	bool ok = st.mainInRegion && whiteF <= 0.002f && outF <= 0.04f;
	if (st.opening && openPx >= 1500) ok = ok && inF >= 0.5f;
	TestCheck(ok, "mask (no white, opening = chamber, Minecraft elsewhere)", buf);
}

// Every frame for a while: white frames, frames whose opening shows Minecraft, and (transit) the
// region views' images: missing, stale, of another pose, or reprojected.
static int g_watchLeft = 0, g_watchFrames = 0, g_watchWhite = 0, g_watchHole = 0, g_watchDumps = 0, g_watchRegion = 0;
static int g_watchMissing = 0, g_watchStale = 0, g_watchPose = 0, g_watchReproj = 0, g_watchFail = 0;
static float g_watchPoseMax = 0;
static uint64_t g_watchAgeMax = 0;
static char g_watchLabel[64] = "watch";
static bool g_watchTransit = false;
static bool g_watchMcWorld = false;  // "mcworld" watch: transit checks, a missing image only fails after a chain's first 3 frames
static int g_watchLate = 0, g_watchEarly = 0, g_setLate = 0, g_setEarly = 0;
static char g_setLabel[64] = "set";
static int g_setFrames = 0, g_setRegion = 0, g_setWhite = 0, g_setHole = 0, g_setStale = 0, g_setPose = 0, g_setMissing = 0, g_setReproj = 0, g_setFail = 0, g_setCrossings = 0;
static int g_setViews = 0, g_setExact = 0, g_setDumps = 0, g_setHeld = 0, g_setOver = 0;
static float g_setPoseMax = 0, g_setUncMax = 0;
static uint64_t g_setAgeMax = 0;
struct WorstFrame {
	float score = -1;
	char line[360] = "";
	char file[96] = "";
};
static WorstFrame g_worst[3];
static void WatchSummary();
// Preview stability with a still camera: the mean per-pixel change between consecutive frames inside
// the openings of portals whose far side is Minecraft (clouds re-centred between camera passes made
// them twitch; a stale or alternating image shows up the same way). Only frames whose camera pose
// equals the previous frame's are compared.
static std::vector<uint8_t> g_stPrev;
static std::vector<int> g_stPrevIdx;
static Vector g_stPrevO;
static QAngle g_stPrevA;
static uint64_t g_stPrevFrame = 0;
static int g_stFrames = 0, g_stOver = 0;
static double g_stSum = 0, g_stMax = 0;
static uint64_t g_stMaxFrame = 0;
static void StillStability(const FrameSnap &sn) {
	if (!g_aPx) return;
	std::vector<int> idx;
	std::vector<uint8_t> px;
	for (int py = 6; py < g_aH - 6; py += 6)
		for (int x6 = 6; x6 < g_aW - 6; x6 += 6) {
			const float x = (x6 + 0.5f) / g_aW * 2 - 1, y = 1 - (py + 0.5f) / g_aH * 2;
			if (x6 > g_aW * 0.45f && py > g_aH * 0.52f) continue;  // (the gun)
			bool in = false;
			for (int i = 0; i < 2 && !in; ++i) {
				if (!g_portalSet[i] || !g_portalSet[1 - i] || !ClipRegionContains(g_portalPos[1 - i])) continue;
				float sc = OpeningScale(sn.mainCam, i, x, y);
				in = sc >= 0 && sc < 0.85f;
			}
			if (!in) continue;
			idx.push_back(py * g_aW + x6);
			const uint8_t *f = Px(x6, py);
			px.insert(px.end(), f, f + 3);
		}
	QAngle a = g_ovAngles;
	bool still = g_stPrevFrame + 1 == sn.frame && (sn.mainCam.origin - g_stPrevO).Length() < 0.05f && std::fabs(a.x - g_stPrevA.x) < 0.02f &&
	             std::fabs(std::remainder(a.y - g_stPrevA.y, 360.0f)) < 0.02f && idx == g_stPrevIdx && !idx.empty();
	if (still) {
		double sum = 0;
		for (size_t k = 0; k < px.size(); ++k) sum += std::abs((int)px[k] - (int)g_stPrev[k]);
		double mean = sum / px.size();
		++g_stFrames, g_stSum += mean, g_stOver += mean > 4.0;
		if (mean > g_stMax) g_stMax = mean, g_stMaxFrame = sn.frame;
	}
	g_stPrev.swap(px), g_stPrevIdx.swap(idx), g_stPrevO = sn.mainCam.origin, g_stPrevA = a, g_stPrevFrame = sn.frame;
}
static int g_watchShiftFrames = 0, g_watchShiftFail = 0;  // frames within 10 host frames after a floating-origin shift, and failing ones
static void WatchFrame(const FrameSnap &sn) {
	StillStability(sn);
	FrameStats st;
	g_markInside = g_watchTransit;
	AnalyzeFrame(sn, st);
	g_markInside = false;
	const FrameViewInfo &fv = sn.fvi;
	++g_watchFrames;
	g_watchRegion += st.mainInRegion;
	float whiteF = st.samples ? (float)st.white / st.samples : 0;
	float inF = st.inside ? (float)st.insideNotMc / st.inside : 1;
	// (an opening under ~1500 px is mostly Portal 2's rim and glow: its inside is not classified, as in
	// the mask probe)
	bool white = whiteF > 0.002f, hole = st.opening && st.inside * 16 >= 1500 && inF < 0.5f;
	g_watchWhite += white, g_watchHole += hole;
	if (g_watchTransit) {
		bool missing = g_watchMcWorld ? fv.fallbackLate > 0 : fv.missing > 0, stale = fv.stale > 0, pose = fv.mismatch > 0, reproj = fv.views > 0 && fv.missing == 0 && !stale && !pose;
		g_watchLate += fv.fallbackLate > 0, g_watchEarly += fv.fallbackEarly > 0, g_setLate += fv.fallbackLate > 0, g_setEarly += fv.fallbackEarly > 0;
		g_watchMissing += missing, g_watchStale += stale, g_watchPose += pose, g_watchReproj += reproj;
		g_watchPoseMax = std::max(g_watchPoseMax, fv.poseErr);
		g_watchAgeMax = std::max(g_watchAgeMax, fv.age);
		bool fail = white || hole || missing || stale || pose;
		g_watchFail += fail;
		if (g_shiftHostFrame && sn.frame >= g_shiftHostFrame && sn.frame < g_shiftHostFrame + 10) ++g_watchShiftFrames, g_watchShiftFail += fail;
		++g_setFrames, g_setRegion += fv.views > 0, g_setWhite += white, g_setHole += hole, g_setStale += stale, g_setPose += pose, g_setMissing += missing, g_setReproj += reproj, g_setFail += fail;
		g_setViews += fv.views, g_setExact += fv.exact, g_setHeld += fv.held > 0, g_setOver += fv.over > 0;
		g_setPoseMax = std::max(g_setPoseMax, fv.poseErr), g_setUncMax = std::max(g_setUncMax, fv.uncovered), g_setAgeMax = std::max(g_setAgeMax, fv.age);
		char line[360];
		std::snprintf(line, sizeof line,
		              "transit %s f%d (host %llu): eye (%.1f %.1f %.1f) pitch %.1f yaw %.1f%s | views %d (exact %d held %d) stream %d (texture %d, image %dx%d) age %llu pose %.1f u %.1f deg uncovered %.1f%% | white %.2f%% hole %d (opening %d px, %.0f%% chamber) missing %d stale %d mismatch %d%s",
		              g_watchLabel, g_watchFrames, (unsigned long long)sn.frame, sn.ov.x, sn.ov.y, sn.ov.z, sn.pitch, sn.yaw,
		              sn.approach < 0 ? "" : sn.approach ? " [aimed, in view]" : " [aimed, NOT IN VIEW]", fv.views, fv.exact, fv.held, fv.stream, fv.set, fv.imgW, fv.imgH,
		              (unsigned long long)fv.age, fv.poseErr, fv.angErr, 100 * fv.uncovered, 100 * whiteF, hole, st.inside * 16, 100 * inF, fv.missing, fv.stale, fv.mismatch, fail ? "  FAIL" : "");
		if (!g_watchMcWorld || fail || fv.fallbackEarly > 0) console->Print("[bridge] %s%s\n", line, fv.fallbackEarly > 0 && !fail ? " (new camera chain: fallback within its first 3 frames)" : "");
		// the set's worst frames (failures first, then pose error, uncovered area, age) are kept as images
		float score = (fail ? 1e6f : 0) + fv.poseErr + 1000 * fv.uncovered + 10 * (float)fv.age;
		int slot = -1;
		for (int k = 0; k < 3; ++k)
			if (slot < 0 || g_worst[k].score < g_worst[slot].score) slot = k;
		if (fv.views > 0 && score > g_worst[slot].score) {
			WorstFrame &wf = g_worst[slot];
			wf.score = score;
			std::snprintf(wf.line, sizeof wf.line, "%s", line);
			std::snprintf(wf.file, sizeof wf.file, "worst_%s_%d", g_setLabel, slot);
			DumpFrame(wf.file, 0);
		}
		if (fail && g_setDumps < 16) {
			char nm[96];
			std::snprintf(nm, sizeof nm, "%s_c%d", g_watchLabel, g_setCrossings);
			DumpFrame(nm, g_watchFrames), ++g_setDumps;
		}
	} else if (white || hole) {
		console->Print("[bridge] watch %s frame %d: white %.1f%%, opening %d samples %.0f%% chamber, camera in region %d, eye %.1f u from the portal plane\n", g_watchLabel, g_watchFrames,
		               100 * whiteF, st.inside, 100 * inF, st.mainInRegion, st.eyeDist);
		if (g_watchDumps < 12) DumpFrame(g_watchLabel, g_watchFrames), ++g_watchDumps;
	}
}
static void WatchSummary() {
	console->Print("[bridge] watch %s: %d frames (%d with the camera in the region), %d white, %d with Minecraft in the opening, %d missing, %d stale, %d mismatch, %d reprojected-only (max age %llu, max pose error %.1f u); fallback fill %d late / %d in a new chain's first 3 frames; failing %d; readbacks skipped %d; floating-origin shifts: %d frames within 10 of one, %d failing\n",
	               g_watchLabel, g_watchFrames, g_watchRegion, g_watchWhite, g_watchHole, g_watchMissing, g_watchStale, g_watchPose, g_watchReproj, (unsigned long long)g_watchAgeMax,
	               g_watchPoseMax, g_watchLate, g_watchEarly, g_watchFail, g_rbSkipped, g_watchShiftFrames, g_watchShiftFail);
}
// The readbacks the render thread has finished, oldest first.
static void ProcessReadBacks() {
	for (int pass = 0; pass < 2; ++pass) {
		ReadBack *best = nullptr;
		for (auto &rb : g_rb)
			if (rb.state.load() == 2 && (!best || rb.snap.frame < best->snap.frame)) best = &rb;
		if (!best) return;
		g_aPx = best->px.data(), g_aW = best->w, g_aH = best->h;
		WatchFrame(best->snap);
		g_aPx = nullptr;
		best->state = 0;
	}
}

// Occlusion: the region portal is behind terrain from this pose, so the pixels where it projects must
// show Minecraft's terrain (match Minecraft's image there), not the chamber or the portal's rim.
static void OcclusionProbe() {
	int pi = ClipRegionPortal();
	if (pi < 0 || !g_cur.valid || !ReadFrame()) return TestCheck(false, "portal hidden by terrain", "no region portal / view / image");
	FrameSnap sn = TakeSnap();
	if (!sn.img.ok) return TestCheck(false, "portal hidden by terrain", "no image");
	int n = 0, mc = 0, chamber = 0;
	for (int py = 2; py < g_aH; py += 2)
		for (int px = 2; px < g_aW; px += 2) {
			float x = (px + 0.5f) / g_aW * 2 - 1, y = 1 - (py + 0.5f) / g_aH * 2;
			float sc = OpeningScale(sn.cur, pi, x, y);
			if (sc < 0 || sc > 1.1f) continue;
			const uint8_t *f = Px(px, py);
			float tu, tv;
			if (!SnapUV(sn, x, y, tu, tv)) continue;
			uint8_t m[4];
			SnapPx(sn.img, tu, tv, m);
			++n;
			mc += std::abs(f[0] - m[0]) + std::abs(f[1] - m[1]) + std::abs(f[2] - m[2]) < 75;
			chamber += IsChamber(f);
		}
	g_marks.clear();
	DumpFrame("occluded", 0);
	char buf[200];
	std::snprintf(buf, sizeof buf, "%d samples where the portal projects: %.0f%% match Minecraft's terrain, %.0f%% chamber-like", n, n ? 100.0 * mc / n : 0.0, n ? 100.0 * chamber / n : 0.0);
	TestCheck(n >= 20 && mc * 10 >= n * 7 && chamber * 10 < n, "portal hidden by terrain", buf);
}

// pcb_assert_seen_through <portal> <chell|targetname> [min_fraction=0.25]: a Portal 2 object seen THROUGH
// portal i is drawn there. The object (Chell's body, or a named entity's centre) as seen through portal i
// appears at its position carried from the partner's side to portal i's side; at that point on screen,
// inside the opening, the frame must differ from Minecraft's image (Portal 2 drew the object over it).
static int g_seenProbe = 0, g_seenPortal = 0;
static char g_seenWhat[64] = "chell";
static float g_seenMin = 0.25f;
static void SeenThroughProbe() {
	const int i = g_seenPortal, j = 1 - i;
	char buf[300];
	if (!g_portalSet[i] || !g_portalSet[j]) return TestCheck(false, "object seen through the portal", "no pair");
	Vector obj;
	float halfW = 12, halfH = 30;
	if (!std::strcmp(g_seenWhat, "chell")) obj = PlayerOrigin() + Vector(0, 0, 36);
	else {
		auto v = EntFind(nullptr, g_seenWhat);
		if (v.empty()) return TestCheck(false, "object seen through the portal", "no such entity");
		obj = EntVec(v[0], "origin"), halfW = halfH = 12;
	}
	if (!ReadFrame()) return TestCheck(false, "object seen through the portal", "no frame");
	FrameSnap sn = TakeSnap();
	if (!sn.img.ok) return TestCheck(false, "object seen through the portal", "no Minecraft image");
	// the object near the partner j, seen through i: carried from j's side to i's side
	Vector seen;
	QAngle dummy{0, 0, 0};
	ThroughPortal(j, i, obj, dummy, seen, dummy);
	const CurView &cam = sn.mainCam;
	Vector v = seen - cam.origin;
	float d = Vector::DotProduct(v, cam.f);
	if (d < 8) return TestCheck(false, "object seen through the portal", "behind the camera");
	float cx = Vector::DotProduct(v, cam.r) / (d * cam.tx), cy = Vector::DotProduct(v, cam.u) / (d * cam.ty);
	float sx = halfW / (d * cam.tx), sy = halfH / (d * cam.ty);
	int n = 0, diff = 0, inOpening = 0;
	for (int gy = -6; gy <= 6; ++gy)
		for (int gx = -4; gx <= 4; ++gx) {
			float x = cx + sx * gx / 4.0f, y = cy + sy * gy / 6.0f;
			if (std::fabs(x) > 0.98f || std::fabs(y) > 0.98f) continue;
			float sc = OpeningScale(cam, i, x, y);
			if (sc < 0 || sc > 0.95f) continue;  // only what is seen inside the opening
			++inOpening;
			float tu, tv;
			if (!SnapUV(sn, x, y, tu, tv)) continue;
			int px, py;
			NdcToPixel(x, y, px, py);
			if (px < 0 || py < 0 || px >= g_aW || py >= g_aH) continue;
			const uint8_t *f = Px(px, py);
			uint8_t m[4];
			SnapPx(sn.img, tu, tv, m);
			++n;
			diff += std::abs(f[0] - m[0]) + std::abs(f[1] - m[1]) + std::abs(f[2] - m[2]) > 60;
		}
	g_marks.clear();
	char nm[64];
	std::snprintf(nm, sizeof nm, "seen_%s_p%d", g_seenWhat, i);
	DumpFrame(nm, 0);
	std::snprintf(buf, sizeof buf, "%s through portal %d: at screen (%.2f %.2f), %d samples inside the opening, %d compared, %.0f%% differ from Minecraft's image (min %.0f%%)", g_seenWhat, i, cx,
	              cy, inOpening, n, n ? 100.0 * diff / n : 0.0, 100 * g_seenMin);
	TestCheck(n >= 10 && diff >= g_seenMin * n, "object seen through the portal", buf);
}

// Portal 2's visible opening, measured (pcb_measure_hole): with the pair views filled magenta
// (pcb_mc_portals_shape ... 3), the pixels Portal 2 shows of a portal view are magenta where nothing of its
// own (rim, edge particles, static) covers them. Along 72 directions in the portal's plane, the last
// contiguous magenta point from the centre is the opening's visible edge (its rim's inner edge); the
// radii are accumulated over several frames (the rim is animated) and summarised as an ellipse.
static int g_holeMeasure = 0, g_holeMeasurePortal = 0, g_holeMeasureFrames = 0;
static bool g_holeUseMax = false;  // the largest radius over the frames (the stencil hole under the animated rim)
static char g_holeLabel[48] = "hole";
static std::vector<float> g_holeR[72];
static std::vector<float> g_holePx[72];  // the edge's distance from the opening's centre on screen (pixels)
static bool g_holeLenient = false;       // magenta-tinted counts (blended under the rim's translucent edge)
static std::map<std::string, std::vector<float>> g_holeSaved;
struct HoleFit { float cx, cy, hw, hh; };
static std::map<std::string, HoleFit> g_holeFits;  // label -> centred ellipse fit (Source units in the portal's plane)  // label -> per-direction edge (px), -1 none
static void HoleMeasureFrame() {
	const int pi = g_holeMeasurePortal;
	if (!g_portalSet[pi] || !ReadFrame()) return;
	FrameSnap sn = TakeSnap();
	const CurView &cam = sn.mainCam;
	const Vector c = g_portalPos[pi], n = g_portalNormal[pi], u = g_portalUp[pi], r = PortalRight(pi);
	// 1 magenta, 0 not, -1 off screen (or behind the camera)
	auto magenta = [&](const Vector &p) {
		Vector v = p - cam.origin;
		float d = Vector::DotProduct(v, cam.f);
		if (d < 1) return -1;
		float x = Vector::DotProduct(v, cam.r) / (d * cam.tx), y = Vector::DotProduct(v, cam.u) / (d * cam.ty);
		int px, py;
		NdcToPixel(x, y, px, py);
		if (px < 3 || py < 3 || px >= g_aW - 3 || py >= g_aH - 3) return -1;
		const uint8_t *f = Px(px, py);
		if (g_holeLenient) return f[0] > f[1] + 50 && f[2] > f[1] + 50 ? 1 : 0;
		return f[0] > 170 && f[1] < 90 && f[2] > 170 ? 1 : 0;
	};
	auto toPx = [&](const Vector &p, float &px, float &py) {
		Vector v = p - cam.origin;
		float d = Vector::DotProduct(v, cam.f);
		if (d < 1) return false;
		float x = Vector::DotProduct(v, cam.r) / (d * cam.tx), y = Vector::DotProduct(v, cam.u) / (d * cam.ty);
		px = (x + 1) * 0.5f * g_aW, py = (1 - y) * 0.5f * g_aH;
		return true;
	};
	for (int k = 0; k < 72; ++k) {
		float t = 2 * (float)M_PI * k / 72, cs = std::cos(t), sn2 = std::sin(t);
		// the outermost magenta sample with a mostly magenta run inside it (the crosshair, HUD text or
		// the gun may interrupt the run nearer the centre); a direction that leaves the screen first
		// does not count
		const int N = 400;
		int m[N];
		bool off = false;
		for (int q = 0; q < N && !off; ++q) {
			float s = 0.2f + q * 0.004f;
			m[q] = magenta(c + n * 0.5f + r * (s * 32.0f * cs) + u * (s * 56.0f * sn2));
			off = m[q] < 0;
		}
		if (off) continue;
		float edge = -1;
		for (int q = N - 1; q >= 25 && edge < 0; --q) {
			if (m[q] != 1) continue;
			int run = 0;
			for (int j = q - 25; j <= q; ++j) run += m[j] == 1;
			if (run >= 22) edge = 0.2f + q * 0.004f;
		}
		if (edge > 0) {
			g_holeR[k].push_back(edge);
			float cx, cy, ex, ey;
			if (toPx(c + n * 0.5f, cx, cy) && toPx(c + n * 0.5f + r * (edge * 32.0f * cs) + u * (edge * 56.0f * sn2), ex, ey)) g_holePx[k].push_back(std::hypot(ex - cx, ey - cy));
		}
	}
	++g_holeMeasureFrames;
}
// pcb_measure_open <portal> <frames> [label]: every frame of a portal's open animation (fire it just
// before), Portal 2's visible opening against what Minecraft's image drawn this frame shows: Portal 2's
// open amount (its material), the plugin's timer, the opening scale in the drawn image's request, and the
// hole Portal 2 shows (pair views magenta, pcb_mc_portals_shape ... 3; an orange portal: its static
// overlay mixes orange into the magenta, which stays blue-heavy), as the median radius over 16 directions
// in units of the full-open opening.
static int g_openMeasure = 0, g_openMeasurePortal = 1, g_openMeasureN = 0;
static int g_openCmpN = 0, g_openLeadN = 0, g_openOldSpotN = 0;
static float g_openLeadMax = 0;
static void OpenMeasureFrame() {
	const int pi = g_openMeasurePortal;
	if (!g_portalSet[pi] || !ReadFrame()) return;
	FrameSnap sn = TakeSnap();
	const CurView &cam = sn.mainCam;
	const Vector c = g_portalPos[pi], n = g_portalNormal[pi], u = g_portalUp[pi], r = PortalRight(pi);
	auto hole = [&](const Vector &p) {
		Vector v = p - cam.origin;
		float d = Vector::DotProduct(v, cam.f);
		if (d < 1) return -1;
		float x = Vector::DotProduct(v, cam.r) / (d * cam.tx), y = Vector::DotProduct(v, cam.u) / (d * cam.ty);
		int px, py;
		NdcToPixel(x, y, px, py);
		if (px < 3 || py < 3 || px >= g_aW - 3 || py >= g_aH - 3) return -1;
		const uint8_t *f = Px(px, py);
		return f[2] - f[1] > 50 && f[0] - f[1] > 50 ? 1 : 0;  // magenta, or magenta under the orange static
	};
	std::vector<float> radii;
	for (int k = 0; k < 16; ++k) {
		float t = 2 * (float)M_PI * (k + 0.5f) / 16, cs = std::cos(t), sn2 = std::sin(t);
		float edge = 0;
		int miss = 0;
		for (float s = 0.0f; s < 1.3f; s += 0.005f) {
			int h = hole(c + n * 0.5f + r * (s * g_openW * cs) + u * (s * g_openH * sn2));
			if (h < 0) break;
			if (h) edge = s, miss = 0;
			else if (++miss > 8) break;
		}
		radii.push_back(edge);
	}
	std::sort(radii.begin(), radii.end());
	const float holeScale = radii[radii.size() / 2];
	const Stream *S = MainImage();
	// (ReadFrame returns the previous frame: compare with the image drawn in it)
	static float prevScale = -1;
	static Vector prevPos;
	static uint64_t prevAge = 0;
	float imgScale = prevScale;
	const float imgMoved = (prevPos - g_portalPos[pi]).Length();
	if (imgMoved > 1.0f && imgScale > 0) imgScale = -2;  // (that image's opening is at the portal's old spot)
	prevScale = S ? S->img.openScale[pi] : -1, prevPos = S ? S->img.openPos[pi] : Vector(0, 0, 0);
	uint64_t age = prevAge;
	prevAge = S ? g_hostFrame - S->img.frame : 0;
	if (imgScale >= 0 && holeScale < 1.2f) {
		++g_openCmpN;
		if (imgScale > holeScale + 0.02f) ++g_openLeadN, g_openLeadMax = std::max(g_openLeadMax, imgScale - holeScale);
	}
	if (imgScale == -2) ++g_openOldSpotN;
	console->Print("[bridge] open f%d (host %llu): Portal 2 open %.3f (timer %.3f) -> hole scale %.3f (p25 %.3f p75 %.3f); Minecraft image (age %llu) opening scale %.3f%s%s\n", g_openMeasureN,
	               (unsigned long long)g_hostFrame, g_openRead[pi], g_portalOpen[pi], holeScale, radii[4], radii[11], (unsigned long long)age, imgScale,
	               imgScale > holeScale + 0.02f ? "  LEADS" : "", imgScale == -2 ? "  (image: the opening at the old spot)" : "");
	if (g_openMeasureN < 60 && g_openMeasureN % 3 == 0) {
		char nm[64];
		std::snprintf(nm, sizeof nm, "open_p%d_%s", pi, g_pairDebug == 3 ? "magenta" : "normal");
		g_marks.clear();
		DumpFrame(nm, g_openMeasureN);
	}
	++g_openMeasureN;
}
static void HoleMeasureReport() {
	// per direction: the median radius (scale of the 32 x 56 ellipse); the ellipse fit by least squares
	double sxx = 0, syy = 0, sxy = 0, sx = 0, sy = 0;  // fit 1 = A x^2 + B y^2 over the median points
	std::string tab;
	float rmin = 9, rmax = 0;
	int nd = 0;
	for (int k = 0; k < 72; ++k) {
		auto v = g_holeR[k];
		if (v.empty()) continue;
		std::sort(v.begin(), v.end());
		float m = g_holeUseMax ? v.back() : v[v.size() / 2], lo = v[v.size() / 10], hi = v[v.size() * 9 / 10];
		float t = 2 * (float)M_PI * k / 72;
		double X = m * 32.0 * std::cos(t), Y = m * 56.0 * std::sin(t);
		double a = X * X, b = Y * Y;
		sxx += a * a, syy += b * b, sxy += a * b, sx += a, sy += b;
		rmin = std::min(rmin, m), rmax = std::max(rmax, m), ++nd;
		if (k % 6 == 0) {
			char buf[64];
			std::snprintf(buf, sizeof buf, " %d:%.3f(%.3f-%.3f)", k * 5, m, lo, hi);
			tab += buf;
		}
	}
	// general axis-aligned fit with a centre: A X^2 + B Y^2 + C X + D Y = 1 (least squares, 4x4 normal equations)
	{
		double M[4][5] = {};
		int np = 0;
		for (int k = 0; k < 72; ++k) {
			auto v = g_holeR[k];
			if (v.empty()) continue;
			std::sort(v.begin(), v.end());
			float m = g_holeUseMax ? v.back() : v[v.size() / 2];
			float t = 2 * (float)M_PI * k / 72;
			double X = m * 32.0 * std::cos(t), Y = m * 56.0 * std::sin(t), f[4] = {X * X, Y * Y, X, Y};
			for (int i = 0; i < 4; ++i) {
				for (int j = 0; j < 4; ++j) M[i][j] += f[i] * f[j];
				M[i][4] += f[i];
			}
			++np;
		}
		for (int i = 0; i < 4; ++i) {  // Gauss-Jordan
			int piv = i;
			for (int r = i + 1; r < 4; ++r)
				if (std::fabs(M[r][i]) > std::fabs(M[piv][i])) piv = r;
			for (int c = 0; c < 5; ++c) std::swap(M[i][c], M[piv][c]);
			if (std::fabs(M[i][i]) < 1e-12) break;
			for (int r = 0; r < 4; ++r) {
				if (r == i) continue;
				double fct = M[r][i] / M[i][i];
				for (int c = 0; c < 5; ++c) M[r][c] -= fct * M[i][c];
			}
		}
		double A4 = M[0][4] / M[0][0], B4 = M[1][4] / M[1][1], C4 = M[2][4] / M[2][2], D4 = M[3][4] / M[3][3];
		if (np >= 12 && A4 > 0 && B4 > 0) {
			double cx = -C4 / (2 * A4), cy = -D4 / (2 * B4), rhs = 1 + C4 * C4 / (4 * A4) + D4 * D4 / (4 * B4);
			console->Print("[bridge] hole [%s] centred fit: centre right %+.2f up %+.2f u, half extents %.2f x %.2f u\n", g_holeLabel, cx, cy, std::sqrt(rhs / A4), std::sqrt(rhs / B4));
			g_holeFits[g_holeLabel] = HoleFit{(float)cx, (float)cy, (float)std::sqrt(rhs / A4), (float)std::sqrt(rhs / B4)};
		}
		std::string all;
		for (int k = 0; k < 72; ++k) {
			auto v = g_holeR[k];
			if (v.empty()) continue;
			std::sort(v.begin(), v.end());
			char b[24];
			std::snprintf(b, sizeof b, " %d:%.3f", k * 5, g_holeUseMax ? v.back() : v[v.size() / 2]);
			all += b;
		}
		console->Print("[bridge] hole [%s] all directions (deg:scale):%s\n", g_holeLabel, all.c_str());
	}
	double det = sxx * syy - sxy * sxy, A = det != 0 ? (sx * syy - sy * sxy) / det : 0, B = det != 0 ? (sy * sxx - sx * sxy) / det : 0;
	double hw = A > 0 ? 1 / std::sqrt(A) : 0, hh = B > 0 ? 1 / std::sqrt(B) : 0;
	console->Print("[bridge] hole [%s] portal %d over %d frames, %d directions: fitted ellipse %.2f x %.2f u (half extents), scale of 32x56 %s %.3f..%.3f; deg:scale(p10-p90)%s\n", g_holeLabel,
	               g_holeMeasurePortal, g_holeMeasureFrames, nd, hw, hh, g_holeUseMax ? "max" : "median", rmin, rmax, tab.c_str());
	char b2[200];
	std::snprintf(b2, sizeof b2, "[%s] portal %d: %.2f x %.2f u over %d frames", g_holeLabel, g_holeMeasurePortal, hw, hh, g_holeMeasureFrames);
	TestCheck(nd >= 24 && hw > 5 && hh > 5, "portal opening measured", b2);
	std::vector<float> px(72, -1.0f);
	for (int k = 0; k < 72; ++k) {
		auto v = g_holePx[k];
		if (v.empty()) continue;
		std::sort(v.begin(), v.end());
		px[k] = g_holeUseMax ? v.back() : v[v.size() / 2];
	}
	g_holeSaved[g_holeLabel] = px;
}

// pcb_measure_gap: the same-frame gate for Minecraft's opening against Portal 2's own (run with Minecraft's
// pair openings flat magenta, pcb_mc_portals_flat 1, and the pair views tinted, pcb_mc_portals_shape w h 0 5).
// Portal 2's portal view (its stencil: what it shows of the view through the portal) then draws Minecraft's
// image x [0 1 0]: magenta inside Minecraft's opening turns black, Minecraft's wall turns green. Outside the
// stencil the main view shows the image untinted. So, with the rim drawn on top as the player sees it:
//   green   (G > R+30, G > B+20) = a GAP: Portal 2's opening shows wall where Minecraft's opening ended;
//   magenta (R, B > G+60)        = an OVERSHOOT: Minecraft's opening where Portal 2 shows no view.
// Portal 2's blue/orange rim never passes either test (blue has G < B, orange G < R), and a gap or overshoot
// under an opaque part of the rim is not counted (it cannot be seen). (The old classifier, magenta run ->
// neutral -> rim, counted magenta blended under the rim as opening and the far wall's tint as overshoot.)
// Along 72 rays from the centre (0.2 .. 1.4 of the 32 x 56 quad) per frame: the screen length of gap and of
// overshoot samples, per direction; reported per side (p95 and max over the frames and the 18 directions
// of each side) and asserted: p95 <= limit_px on every side and both kinds. Frames are counted only when
// the tint is visible (black opening pixels near the centre), else the mode is not on.
static int g_gapLeft = 0, g_gapPortal = 0, g_gapFrames = 0, g_gapBadFrames = 0;
static char g_gapLabel[48] = "gap";
static std::vector<float> g_gapPx[72], g_overPx[72];
static float g_gapOpenMin = 2, g_gapOpenMax = -1;
static int g_gapWaited = 0;
static void GapMeasureFrame() {
	const int pi = g_gapPortal;
	// (a re-fired portal reads as not set for part of its opening: wait for it, at most 300 frames)
	if (!g_portalSet[pi] && g_gapWaited < 300) {
		++g_gapWaited, ++g_gapLeft;
		return;
	}
	if (!g_portalSet[pi] || !ReadFrame()) return;
	FrameSnap sn = TakeSnap();
	const CurView &cam = sn.mainCam;
	const Vector c = g_portalPos[pi], n = g_portalNormal[pi], u = g_portalUp[pi], r = PortalRight(pi);
	auto proj = [&](const Vector &p, float &px, float &py) {
		Vector v = p - cam.origin;
		float d = Vector::DotProduct(v, cam.f);
		if (d < 1) return false;
		float x = Vector::DotProduct(v, cam.r) / (d * cam.tx), y = Vector::DotProduct(v, cam.u) / (d * cam.ty);
		px = (x + 1) * 0.5f * g_aW, py = (1 - y) * 0.5f * g_aH;
		return px >= 2 && py >= 2 && px < g_aW - 2 && py < g_aH - 2;
	};
	// (colour-aware: the rim blends over what is under it; each test uses the channel the rim lacks, so a
	// gap or overshoot seen through the rim's translucent part counts, and the bare rim never does)
	auto cls = [&](float px, float py) {  // 0 other, 1 gap (green), 2 overshoot (magenta), 3 opening (black)
		const uint8_t *f = Px((int)px, (int)py);
		const float R = f[0], G = f[1], B = f[2];
		if (R < 40 && G < 40 && B < 40) return 3;
		// gap: green dominating both other channels (the rim's own gradient runs to cyan at the blue portal's
		// bottom and to yellow at the orange one's, which a ratio test took for green); visible through the
		// rim up to about half its opacity
		if (G > R + 20 && G > B + 30) return 1;
		if (pi == 0 ? R > G + 40 : B > G + 40) return 2;  // overshoot: the channel the rim lacks (blue rim R < G, orange rim B < G)
		return 0;
	};
	// the tint is on: most samples around the centre are black
	// (Portal 2's visible opening scale now: the nested portal at the centre of a facing pair and the
	// recursion views inside it are skipped; the rays cover the outer part of the opening, its edge and the rim)
	const float open = ReadOpenVar(pi);
	const float vis = std::max(0.08f, OpenCurve(open >= 0 ? open : 1.0f));
	int black = 0, tot = 0;
	for (int k = 0; k < 16; ++k) {
		float t = 2 * (float)M_PI * k / 16, X, Y;
		if (proj(c + n * 0.5f + r * (0.6f * vis * 32.0f * std::cos(t)) + u * (0.6f * vis * 56.0f * std::sin(t)), X, Y)) ++tot, black += cls(X, Y) == 3;
	}
	if (tot < 4 || black * 2 < tot) {
		++g_gapBadFrames;
		return;
	}
	for (int k = 0; k < 72; ++k) {
		float t = 2 * (float)M_PI * k / 72, cs = std::cos(t), sn2 = std::sin(t);
		const int N = 600;
		static float SX[N], SY[N];
		static int SC[N];
		bool ok = true;
		int lastOpen = -1;
		for (int q = 0; q < N && ok; ++q) {
			float s = 0.55f * vis + q * (1.4f - 0.55f * vis) / N;  // 0.55 of the visible opening .. 1.4 of the quad
			ok = proj(c + n * 0.5f + r * (s * 32.0f * cs) + u * (s * 56.0f * sn2), SX[q], SY[q]);
			if (!ok) break;
			SC[q] = cls(SX[q], SY[q]);
			if (SC[q] == 3) lastOpen = q;
		}
		// only past the opening's edge (the ray's last black sample): inside it a facing pair shows the nested
		// portal, which the flat debug colour fills with magenta past the recursion limit
		float gap = 0, over = 0;
		for (int q = std::max(1, lastOpen + 1); ok && q < N; ++q) {
			float step = std::hypot(SX[q] - SX[q - 1], SY[q] - SY[q - 1]);
			if (SC[q] == 1) gap += step;
			else if (SC[q] == 2) over += step;
		}
		if (!ok) continue;  // (a ray that leaves the screen is not measured)
		g_gapPx[k].push_back(gap);
		g_overPx[k].push_back(over);
	}
	g_gapOpenMin = std::min(g_gapOpenMin, open), g_gapOpenMax = std::max(g_gapOpenMax, open);
	if (std::strstr(g_gapLabel, "open")) {  // (the open animation: each frame's worst gap / overshoot)
		float gm = 0, om = 0;
		for (int k = 0; k < 72; ++k) {
			if (!g_gapPx[k].empty()) gm = std::max(gm, g_gapPx[k].back());
			if (!g_overPx[k].empty()) om = std::max(om, g_overPx[k].back());
		}
		console->Print("[bridge] gap [%s] f%d (host %llu): Portal 2 open %.3f, image opening scale %.3f: gap max %.1f px, overshoot max %.1f px\n", g_gapLabel, g_gapFrames, (unsigned long long)g_hostFrame, open,
		               MainImage() ? MainImage()->img.openScale[pi] : -1.0f, gm, om);
	}
	if (g_gapFrames == 0 || g_gapLeft == 1) {
		char nm[80];
		std::snprintf(nm, sizeof nm, "gap_%s_%s", g_gapLabel, g_gapFrames == 0 ? "first" : "last");
		g_marks.clear();
		DumpFrame(nm, 0);
	}
	++g_gapFrames;
}
static void GapReport(float lim) {
	auto side = [&](std::vector<float> *v, int q, float &mx, int &n) {
		std::vector<float> all;
		for (int dk = -9; dk < 9; ++dk)
			for (float x : v[(q * 18 + dk + 72) % 72]) all.push_back(x);
		std::sort(all.begin(), all.end());
		n = (int)all.size();
		mx = all.empty() ? -1 : all.back();
		return all.empty() ? -1.0f : all[all.size() * 95 / 100];
	};
	const char *names[4] = {"right", "top", "left", "bottom"};
	float worst = 0;
	int measuredSides = 0;
	std::string t;
	for (int q = 0; q < 4; ++q) {
		float gmx, omx;
		int gn, on;
		float g95 = side(g_gapPx, q, gmx, gn), o95 = side(g_overPx, q, omx, on);
		char b[160];
		if (gn == 0) std::snprintf(b, sizeof b, " %s off screen;", names[q]);
		else std::snprintf(b, sizeof b, " %s gap p95 %.1f max %.1f, overshoot p95 %.1f max %.1f (%d);", names[q], g95, gmx, o95, omx, gn), ++measuredSides;
		t += b;
		worst = std::max(worst, std::max(g95, o95));
	}
	char buf[800];
	std::snprintf(buf, sizeof buf, "[%s] portal %d over %d frames (open %.2f..%.2f, %d frames without the tint):%s (limit %.1f px at p95)", g_gapLabel, g_gapPortal, g_gapFrames, g_gapOpenMin,
	              g_gapOpenMax, g_gapBadFrames, t.c_str(), lim);
	TestCheck(g_gapFrames >= 5 && measuredSides >= 2 && worst <= lim, "Minecraft's opening meets Portal 2's (no gap, no overshoot)", buf);
}
static float g_gapLim = 1.0f;
CON_COMMAND(pcb_measure_gap, "pcb_measure_gap <portal> [frames=40] [label] [limit_px=1] - same-frame gap / overshoot between Minecraft's opening (pcb_mc_portals_flat 1) and Portal 2's rim, asserted at the end\n") {
	g_gapPortal = args.ArgC() > 1 ? (std::atoi(args[1]) & 1) : 0;
	g_gapLeft = args.ArgC() > 2 ? std::max(1, std::atoi(args[2])) : 40;
	std::snprintf(g_gapLabel, sizeof g_gapLabel, "%s", args.ArgC() > 3 ? args[3] : "gap");
	g_gapLim = args.ArgC() > 4 ? (float)std::atof(args[4]) : 1.0f;
	g_gapFrames = 0, g_gapBadFrames = 0, g_gapOpenMin = 2, g_gapOpenMax = -1, g_gapWaited = 0;
	for (auto &v : g_gapPx) v.clear();
	for (auto &v : g_overPx) v.clear();
}

// pcb_frame_dump: every frame for N frames, full resolution inside a pixel box (frame-by-frame captures)
static int g_dumpLeft = 0, g_dumpN = 0, g_dumpBox[4] = {0, 0, 0, 0};
static char g_dumpLabel[48] = "dump";
static void DumpCropFrame() {
	if (!ReadFrame()) return;
	int x0 = std::max(0, g_dumpBox[0]), y0 = std::max(0, g_dumpBox[1]), x1 = g_dumpBox[2] > 0 ? std::min(g_aW, g_dumpBox[2]) : g_aW, y1 = g_dumpBox[3] > 0 ? std::min(g_aH, g_dumpBox[3]) : g_aH;
	if (x1 <= x0 || y1 <= y0) return;
	char path[512];
	std::snprintf(path, sizeof path, "%s/shots/frames/%s_%03d.ppm", CacheDir().c_str(), g_dumpLabel, g_dumpN);
	FILE *fp = std::fopen(path, "wb");
	if (!fp) return;
	std::fprintf(fp, "P6\n%d %d\n255\n", x1 - x0, y1 - y0);
	std::vector<uint8_t> row((size_t)(x1 - x0) * 3);
	for (int y = y0; y < y1; ++y) {
		const int sy = y;  // (rows as DumpFrame reads them)
		for (int x = x0; x < x1; ++x)
			for (int c = 0; c < 3; ++c) row[(size_t)(x - x0) * 3 + c] = g_aPx[((size_t)sy * g_aW + x) * 4 + c];
		std::fwrite(row.data(), 1, row.size(), fp);
	}
	std::fclose(fp);
	console->Print("[bridge] frame dump %s %d (host frame %llu): portal open vars %.3f %.3f, set %d %d, early views %d\n", g_dumpLabel, g_dumpN, (unsigned long long)g_hostFrame, ReadOpenVar(0), ReadOpenVar(1), g_portalSet[0], g_portalSet[1], g_earlyViews);
	++g_dumpN;
}
CON_COMMAND(pcb_frame_dump, "pcb_frame_dump <frames> <label> [x0 y0 x1 y1] - dump every frame (full resolution, optionally a pixel box) to shots/frames\n") {
	g_dumpLeft = args.ArgC() > 1 ? std::atoi(args[1]) : 30;
	std::snprintf(g_dumpLabel, sizeof g_dumpLabel, "%s", args.ArgC() > 2 ? args[2] : "dump");
	for (int i = 0; i < 4; ++i) g_dumpBox[i] = args.ArgC() > 6 ? std::atoi(args[3 + i]) : 0;
	g_dumpN = 0;
}

CON_COMMAND(pcb_probes, "pcb_probes - per-frame probe counters (what waituntil probes_idle waits for)\n") {
	console->Print("[bridge] probes: hole %d open %d gap %d dump %d watch %d holeProbe %d mask %d occ %d seen %d\n", g_holeMeasure, g_openMeasure, g_gapLeft, g_dumpLeft, g_watchLeft, g_holeProbe, g_maskProbe, g_occProbe, g_seenProbe);
}
bool CompositeProbesIdle() {
	return g_holeMeasure <= 0 && g_openMeasure <= 0 && g_gapLeft <= 0 && g_dumpLeft <= 0 && g_watchLeft <= 0 && g_holeProbe <= 0 && g_maskProbe <= 0 && g_occProbe <= 0 && g_seenProbe <= 0;
}

// ---- CEngineVGui::Paint (VEngineVGui001 slot 15 on Linux, SourceAutoRecord's offsets): the probes ----
using _Paint = void(__rescall *)(void *thisptr, int mode);
static _Paint VguiPaint = nullptr;
static uint64_t g_paintFrame = 0;
void GlHudPaint();  // glhud.cpp (bridge #18): the Minecraft hand/HUD, queued before Portal 2's 2D UI
static void __rescall VguiPaint_Hook(void *thisptr, int mode) {
	if (g_paintFrame != g_hostFrame) {
		PROF("paint");  // (bridge #25)
		g_paintFrame = g_hostFrame;
		GlHudPaint();
		// the image probes compare with Minecraft's image: wait (at most 30 frames) for it
		const Stream *S = MainImage();
		bool cpuReady = !S || ImageReady(S) || ++g_probeWait > 30;
		if (g_skyUpProbe >= 0) SkyUpProbe(g_skyUpProbe), g_skyUpProbe = -1;
		if (cpuReady) {
			g_probeWait = 0;
			if (g_holeProbe > 0) --g_holeProbe, HoleProbe();
			if (g_maskProbe > 0) --g_maskProbe, MaskProbe();
			if (g_occProbe > 0) --g_occProbe, OcclusionProbe();
			if (g_seenProbe > 0) --g_seenProbe, SeenThroughProbe();
			if (g_openMeasure > 0) --g_openMeasure, OpenMeasureFrame();
			if (g_holeMeasure > 0 && --g_holeMeasure >= 0) {
				HoleMeasureFrame();
				if (g_holeMeasure == 0) HoleMeasureReport();
			}
		}
		if (g_dumpLeft > 0) --g_dumpLeft, DumpCropFrame();
		if (g_gapLeft > 0) {
			GapMeasureFrame();
			if (--g_gapLeft == 0) GapReport(g_gapLim);
		}
		ProcessReadBacks();
		if (g_watchLeft > 0) {
			QueueReadBack();
			if (--g_watchLeft == 0) WatchSummary();
		}
	}
	VguiPaint(thisptr, mode);
}

bool CompositeHookPaint() {
	g_engineVgui = Interface::Create(MODULE_NAME("engine"), "VEngineVGui001");
	if (!g_engineVgui) return false;
	return g_engineVgui->Hook(VguiPaint_Hook, VguiPaint, Offsets::Paint);
}

// ---- commands ----------------------------------------------------------------------------------------
static void ArmCpu(int = 0) {}  // (the probes read the streams' own buffers)
static int g_bgFrames = 0;  // (pcb_assert_no_flash) region views without an image after Minecraft's first frame
CON_COMMAND(pcb_assert_sky_up, "pcb_assert_sky_up <1|0> - at the next paint: Minecraft's sky is towards the floor portal's up edge (1) or away (0)\n") {
	g_skyUpProbe = args.ArgC() > 1 ? std::atoi(args[1]) : 1;
}
CON_COMMAND(pcb_assert_no_flash, "pcb_assert_no_flash [reset] - no region view went without a Minecraft image since the last reset (after the first one)\n") {
	if (args.ArgC() > 1) return void(g_bgFrames = g_missingAfterFirst);
	char buf[120];
	int n = g_missingAfterFirst - g_bgFrames;
	std::snprintf(buf, sizeof buf, "%d region views without a Minecraft image", n);
	TestCheck(n == 0, "no region-background flash", buf);
}
static int g_missingMark = 0;
CON_COMMAND(pcb_assert_no_missing, "pcb_assert_no_missing [reset] - hard invariant: no region view showed background (no image) since the reset, after Minecraft's first frame\n") {
	if (args.ArgC() > 1) return void(g_missingMark = g_missingAfterFirst);
	char buf[160];
	std::snprintf(buf, sizeof buf, "%d region views missing an image (of %d region views since load; first image %s)", g_missingAfterFirst - g_missingMark, g_regionViews,
	              g_anyImageEver ? "arrived" : "not yet");
	TestCheck(g_missingAfterFirst == g_missingMark, "region views never show background", buf);
}
CON_COMMAND(pcb_assert_portal_view, "pcb_assert_portal_view - at the next paint: the cliff portal's opening shows the chamber, not Minecraft\n") { g_holeProbe = 1, ArmCpu(); }
CON_COMMAND(pcb_assert_occluded, "pcb_assert_occluded - at the next paint: the region portal's projected pixels show Minecraft terrain (it is behind a ridge)\n") { g_occProbe = 1, ArmCpu(); }
CON_COMMAND(pcb_assert_mask, "pcb_assert_mask [label] - at the next paint: no white, the cliff portal's opening shows the chamber, Minecraft everywhere else\n") {
	g_maskProbe = 1, ArmCpu();
	std::snprintf(g_maskLabel, sizeof g_maskLabel, "%s", args.ArgC() > 1 ? args[1] : "mask");
}
CON_COMMAND(pcb_frame_watch, "pcb_frame_watch <frames> [label] [transit] - analyse every frame (white, Minecraft in the cliff opening; transit: the region views' images); bad frames are logged and dumped; 0 stops the current watch\n") {
	if (args.ArgC() > 1 && std::atoi(args[1]) == 0) {
		if (g_watchLeft > 0) g_watchLeft = 0, WatchSummary();
		return;
	}
	ProcessReadBacks();  // (a previous watch's last frames)
	g_watchLeft = args.ArgC() > 1 ? std::atoi(args[1]) : 240;
	g_watchFrames = g_watchWhite = g_watchHole = g_watchDumps = g_watchRegion = 0;
	g_watchMissing = g_watchStale = g_watchPose = g_watchReproj = g_watchFail = 0, g_watchPoseMax = 0, g_watchAgeMax = 0;
	g_watchMcWorld = args.ArgC() > 3 && args[3][0] == 'm';
	g_watchTransit = args.ArgC() > 3 && (args[3][0] == 't' || g_watchMcWorld);
	g_watchLate = g_watchEarly = 0, g_watchShiftFrames = g_watchShiftFail = 0;
	std::snprintf(g_watchLabel, sizeof g_watchLabel, "%s", args.ArgC() > 2 ? args[2] : "watch");
	ArmCpu(g_watchLeft + 10);
}
CON_COMMAND(pcb_transit_set, "pcb_transit_set <reset label|cross|report|assert> - totals of the transit frame checks over a set of crossings\n") {
	const char *op = args.ArgC() > 1 ? args[1] : "report";
	if (op[0] == 'r' && op[2] == 's') {
		std::snprintf(g_setLabel, sizeof g_setLabel, "%s", args.ArgC() > 2 ? args[2] : "set");
		g_setFrames = g_setRegion = g_setWhite = g_setHole = g_setStale = g_setPose = g_setMissing = g_setReproj = g_setFail = g_setCrossings = 0;
		g_setViews = g_setExact = g_setDumps = g_setHeld = g_setOver = 0, g_setPoseMax = g_setUncMax = 0, g_setAgeMax = 0, g_setLate = g_setEarly = 0;
		for (auto &w : g_worst) w = WorstFrame{};
		return;
	}
	if (op[0] == 'c') return void(++g_setCrossings);
	char buf[480];
	std::snprintf(buf, sizeof buf,
	              "[%s] %d crossings, %d frames (%d with a region view): %d failing = %d missing, %d stale, %d mismatch, %d white, %d Minecraft in the opening; %d reprojected-only (%d held after a turn, %d portal-exit frames drawn over); fallback fill %d late / %d early; worst age %llu frames, pose error %.1f u, uncovered %.1f%%",
	              g_setLabel, g_setCrossings, g_setFrames, g_setRegion, g_setFail, g_setMissing, g_setStale, g_setPose, g_setWhite, g_setHole, g_setReproj, g_setHeld, g_setOver, g_setLate,
	              g_setEarly, (unsigned long long)g_setAgeMax, g_setPoseMax, 100 * g_setUncMax);
	for (auto &w : g_worst)
		if (w.score >= 0) console->Print("[bridge] transit set %s worst frame (%s.ppm): %s\n", g_setLabel, w.file, w.line);
	if (op[0] == 'a' && args.ArgC() > 2 && args[2][0] == 'm') {
		// (mcworld sets: missing = fallback after a chain's first 3 frames, counted in the failing frames)
		TestCheck(g_setFrames > 0 && g_setFail == 0, "preview frames clean", buf);
	} else if (op[0] == 'a') {
		TestCheck(g_setFrames > 0 && g_setMissing == 0, "transition: no region view without an image", buf);
		TestCheck(g_setFrames > 0 && g_setFail == 0, "transition frames clean", buf);
	} else {
		console->Print("[bridge] transit set %s\n", buf);
	}
}
CON_COMMAND(pcb_assert_watch_nowhite, "pcb_assert_watch_nowhite - the last pcb_frame_watch saw no white (region background) frame and no Minecraft in a chamber opening (teleports: camera cuts)\n") {
	char buf[200];
	std::snprintf(buf, sizeof buf, "[%s] %d frames (%d in the region): %d white, %d Minecraft in a chamber opening (also %d stale, %d late fallback: a camera cut)", g_watchLabel, g_watchFrames, g_watchRegion,
	              g_watchWhite, g_watchHole, g_watchStale, g_watchLate);
	TestCheck(g_watchFrames > 0 && g_watchWhite == 0 && g_watchHole == 0, "no white or region background frame", buf);
}
CON_COMMAND(pcb_assert_watch_shifts, "pcb_assert_watch_shifts [min_frames=1] - the last pcb_frame_watch saw floating-origin shifts and every frame within 10 host frames after one was clean\n") {
	const int want = args.ArgC() > 1 ? std::atoi(args[1]) : 1;
	char buf[200];
	std::snprintf(buf, sizeof buf, "[%s] %d frames within 10 host frames after a shift (want >= %d), %d failing", g_watchLabel, g_watchShiftFrames, want, g_watchShiftFail);
	TestCheck(g_watchShiftFrames >= want && g_watchShiftFail == 0, "clean frames at the floating-origin shifts", buf);
}
CON_COMMAND(pcb_assert_watch, "pcb_assert_watch [min_region_frames=1] - the last pcb_frame_watch saw no white frame and no frame with Minecraft in the opening\n") {
	int minRegion = args.ArgC() > 1 ? std::atoi(args[1]) : 1;
	char buf[480];
	std::snprintf(buf, sizeof buf, "[%s] %d frames (%d in the region, %d still to go), %d white, %d with Minecraft in the opening", g_watchLabel, g_watchFrames, g_watchRegion, g_watchLeft,
	              g_watchWhite, g_watchHole);
	if (g_watchMcWorld) {
		std::snprintf(buf, sizeof buf, "[%s] %d frames (%d in the region, %d still to go): %d failing = %d white, %d Minecraft in a chamber opening, %d fallback after a chain's first 3 frames, %d stale, %d mismatch (%d early fallback frames)",
		              g_watchLabel, g_watchFrames, g_watchRegion, g_watchLeft, g_watchFail, g_watchWhite, g_watchHole, g_watchLate, g_watchStale, g_watchPose, g_watchEarly);
		return TestCheck(g_watchFrames > 0 && g_watchFail == 0, "every frame clean", buf);
	}
	TestCheck(g_watchFrames > 0 && g_watchRegion >= minRegion && g_watchWhite == 0 && g_watchHole == 0, "every frame clean", buf);
}
CON_COMMAND(pcb_composite_farz, "pcb_composite_farz <ndc_z> - depth of the region background quad; logs the main projection\n") {
	if (args.ArgC() > 1) g_farZOverride = (float)std::atof(args[1]), BackgroundZ();
	const VMatrix &p = g_mainProj;
	float a = p.m[2][2], b = p.m[2][3];
	console->Print("[bridge] composite far z %.6f; main projection row2 (%g %g %g %g), near/far estimate %.2f / %.1f\n", kFarZ, p.m[2][0], p.m[2][1], p.m[2][2], p.m[2][3], b / a, b / (a + 1));
}
CON_COMMAND(pcb_composite_mask, "pcb_composite_mask - composite statistics (views, matching, streams)\n") {
	console->Print("[bridge] composite: %d region views: %d exact, %d reprojected, %d stale, %d mismatch, %d missing (%d after the first image); %d held (turned past the image), %d portal views from just in front of the plane, %d views from inside a portal; new cameras %d / %d; images lost %d; pulls skipped %llu\n",
	               g_regionViews, g_exactViews, g_reprojViews, g_staleViews, g_mismatchViews, g_missingViews, g_missingAfterFirst, g_heldViews, g_entrySideViews, g_intoPortalViews, g_streams[0].newCams,
	               g_streams[1].newCams, g_lostImages, (unsigned long long)g_skippedPulls);
	console->Print("[bridge] composite: rendered views vs predicted cameras: %d matched, mean %.2f u, max %.2f u / %.2f deg; rendered main vs OverrideView: mean %.2f u, max %.2f u over %d frames\n",
	               g_matchN, g_matchN ? g_matchPosSum / g_matchN : 0.0f, g_matchPosMax, g_matchAngMax, g_ovDiffN ? g_ovDiffSum / g_ovDiffN : 0.0f, g_ovDiffMax, g_ovDiffN);
	g_matchN = 0, g_matchPosSum = g_matchPosMax = g_matchAngMax = 0, g_ovDiffN = 0, g_ovDiffSum = g_ovDiffMax = 0;
}
CON_COMMAND(pcb_composite_streams, "pcb_composite_streams [log_n] - Minecraft camera streams; log the next n new-camera assignments and requests\n") {
	if (args.ArgC() > 1) g_streamLog = std::atoi(args[1]), g_reqLog = std::min(20, g_streamLog);
	for (const Stream &st : g_streams)
		console->Print("[bridge] stream %d: camera %s at (%.0f %.0f %.0f), %llu frames ago, velocity %.0f u/s, image %d (request %u, %dx%d, proxy depth %.0f u), latency %.2f, new cameras %d\n",
		               st.index, st.camValid ? "set" : "none", st.camO.x, st.camO.y, st.camO.z, (unsigned long long)(g_hostFrame - st.camFrame), st.camVel.Length(), st.haveImage, st.img.id,
		               st.imgW, st.imgH, st.proxyDepth, st.latencyFrames, st.newCams);
}
CON_COMMAND(pcb_composite_cams, "pcb_composite_cams [K=2] [second_rd=12] [crop 0|1] [margin=0.1] - Minecraft cameras per frame, the second one's render distance, crop to screen boxes\n") {
	if (args.ArgC() > 1) g_maxCams = std::max(1, std::min(kStreams, std::atoi(args[1])));
	if (args.ArgC() > 2) g_secondRd = std::max(0, std::atoi(args[2]));
	if (args.ArgC() > 3) g_crop = std::atoi(args[3]) != 0;
	if (args.ArgC() > 4) g_margin = (float)std::atof(args[4]);
	console->Print("[bridge] composite cameras: K %d, second render distance %d, crop %d, margin %.2f\n", g_maxCams, g_secondRd, g_crop, g_margin);
}
CON_COMMAND(pcb_drift, "pcb_drift - screen drift of the cliff portal's centre against Minecraft's terrain there since the last call\n") {
	console->Print("[bridge] drift: %d frames, mean %.2f px, max %.2f px; position prediction %s, position reprojection %s\n", g_driftN, g_driftN ? g_driftSum / g_driftN : 0.0, g_driftMax,
	               g_predictPos ? "on" : "off", g_reprojectPos ? "on" : "off");
	g_driftSum = g_driftMax = 0, g_driftN = 0;
}
CON_COMMAND(pcb_assert_image_age, "pcb_assert_image_age reset | <primary_max> <secondary_max> - the drawn images' age in host frames since the reset (camera's own view / portal views)\n") {
	if (args.ArgC() > 1 && args[1][0] == 'r') {
		g_agePriMax = g_ageSecMax = 0, g_agePriSum = g_ageSecSum = 0, g_agePriN = g_ageSecN = 0, g_ageRecMax = 0, g_ageRecN = 0;
		std::memset(g_ageHist, 0, sizeof g_ageHist);
		return;
	}
	int pm = args.ArgC() > 1 ? std::atoi(args[1]) : 3, sm = args.ArgC() > 2 ? std::atoi(args[2]) : 6;
	char buf[640];
	int n = std::snprintf(buf, sizeof buf, "camera view: %d draws, mean %.2f, max %llu (limit %d); portal views: %d draws, mean %.2f, max %llu (limit %d); histograms 0..11+:", g_agePriN,
	                      g_agePriN ? g_agePriSum / g_agePriN : 0.0, (unsigned long long)g_agePriMax, pm, g_ageSecN, g_ageSecN ? g_ageSecSum / g_ageSecN : 0.0,
	                      (unsigned long long)g_ageSecMax, sm);
	n += std::snprintf(buf + n, sizeof buf - n, " recursion views %d draws, max %llu;", g_ageRecN, (unsigned long long)g_ageRecMax);
	for (int k = 0; k < 2; ++k) {
		n += std::snprintf(buf + n, sizeof buf - n, " %s", k ? "| portal" : "camera");
		for (int a = 0; a < 12 && n < (int)sizeof buf - 8; ++a) n += std::snprintf(buf + n, sizeof buf - n, " %d", g_ageHist[k][a]);
	}
	// (within the limit in >= 98% of the draws and never past twice it: a Minecraft frame that takes
	// longer than usual is a single late image, not a stale preview)
	int priOver = 0, secOver = 0;
	for (int a = pm + 1; a < 12; ++a) priOver += g_ageHist[0][a];
	for (int a = sm + 1; a < 12; ++a) secOver += g_ageHist[1][a];
	n += std::snprintf(buf + n, sizeof buf - n, "; over the limit: camera %d, portal %d", priOver, secOver);
	bool ok = g_agePriN + g_ageSecN > 0 && priOver * 50 <= g_agePriN && secOver * 50 <= g_ageSecN && (int)g_agePriMax <= 2 * pm && (int)g_ageSecMax <= 2 * sm;
	TestCheck(ok, "image age bounded", buf);
}
CON_COMMAND(pcb_assert_still_stable, "pcb_assert_still_stable reset | <max_mean_change> [max_frame_change] - with a still camera, the openings of portals onto Minecraft change little from frame to frame (clouds, stale images); needs a running pcb_frame_watch\n") {
	if (args.ArgC() > 1 && args[1][0] == 'r') { g_stFrames = g_stOver = 0, g_stSum = g_stMax = 0; return; }
	float lim = args.ArgC() > 1 ? (float)std::atof(args[1]) : 1.0f, limMax = args.ArgC() > 2 ? (float)std::atof(args[2]) : 1e9f;
	char buf[260];
	std::snprintf(buf, sizeof buf, "%d still frame pairs: mean change %.2f levels/channel (limit %.2f), max %.2f (frame %llu, limit %.1f), %d frames over 4", g_stFrames,
	              g_stFrames ? g_stSum / g_stFrames : 0.0, lim, g_stMax, (unsigned long long)g_stMaxFrame, limMax, g_stOver);
	TestCheck(g_stFrames >= 10 && g_stSum / std::max(1, g_stFrames) <= lim && g_stMax <= limMax, "still preview stable", buf);
}
CON_COMMAND(pcb_twitch, "pcb_twitch reset | report <label> [max_p99_px] [max_px] - per-frame screen jitter of each region portal's opening against Minecraft's terrain (camera in the region)\n") {
	const char *op = args.ArgC() > 1 ? args[1] : "report";
	if (op[0] == 'r' && op[2] == 's') {
		g_tw = TwitchStats{};
		std::memset(g_twSkip, 0, sizeof g_twSkip);
		return;
	}
	const char *label = args.ArgC() > 2 ? args[2] : "twitch";
	std::vector<float> v = g_tw.all;
	std::sort(v.begin(), v.end());
	auto pct = [&](double q) { return v.empty() ? 0.0 : (double)v[std::min(v.size() - 1, (size_t)(q * (v.size() - 1) + 0.5))]; };
	char buf[400];
	std::snprintf(buf, sizeof buf,
	              "[%s] %d frame pairs (%d with a new image): jitter mean %.2f px, p50 %.2f, p95 %.2f, p99 %.2f, max %.2f (host frame %llu), %d over 1 px, %d over 2 px; mean on new-image frames %.2f px; misregistration mean %.2f px, max %.2f px over %d frames",
	              label, g_tw.n, g_tw.newImg, g_tw.n ? g_tw.sum / g_tw.n : 0.0, pct(0.5), pct(0.95), pct(0.99), g_tw.max, (unsigned long long)g_tw.maxFrame, g_tw.over1, g_tw.over2,
	              g_tw.newImg ? g_tw.sumNew / g_tw.newImg : 0.0, g_tw.frames ? g_tw.driftSum / g_tw.frames : 0.0, g_tw.driftMax, g_tw.frames);
	if (g_tw.n == 0)
		console->Print("[bridge] twitch [%s]: nothing measured: no view %d, not on a block %d, not facing %d, off screen %d, behind terrain %d, behind the image %d, no solution %d\n", label,
		               g_twSkip[0], g_twSkip[1], g_twSkip[2], g_twSkip[3], g_twSkip[4], g_twSkip[5], g_twSkip[6]);
	std::memset(g_twSkip, 0, sizeof g_twSkip);
	if (args.ArgC() > 3) {
		float lim99 = (float)std::atof(args[3]), limMax = args.ArgC() > 4 ? (float)std::atof(args[4]) : 1e9f;
		TestCheck(g_tw.n > 0 && pct(0.99) <= lim99 && g_tw.max <= limMax, "portal twitch", buf);
	} else {
		console->Print("[bridge] twitch %s\n", buf);
	}
	g_tw = TwitchStats{};
}
CON_COMMAND(pcb_composite_depth, "pcb_composite_depth <unused> [predict_position 0|1] [reproject_position 0|1] - position prediction / reprojection\n") {
	if (args.ArgC() > 2) g_predictPos = std::atoi(args[2]) != 0;
	if (args.ArgC() > 3) g_reprojectPos = std::atoi(args[3]) != 0;
	console->Print("[bridge] composite position prediction %d, position reprojection %d\n", g_predictPos, g_reprojectPos);
}
CON_COMMAND(pcb_reproject_pixel, "pcb_reproject_pixel [iterations=3 (0: the old single proxy plane)] [grid_x=48 grid_y=27] - per-pixel position reprojection from the image's depth alpha (bridge #11)\n") {
	if (args.ArgC() > 1) g_reprojIters = std::max(0, std::min(8, std::atoi(args[1])));
	if (args.ArgC() > 3) g_gridX = std::atoi(args[2]), g_gridY = std::atoi(args[3]);
	console->Print("[bridge] per-pixel reprojection: %d iterations (%d views fell back to the proxy plane), background grid %d x %d\n", g_reprojIters, g_reprojFallback, g_gridX, g_gridY);
}
CON_COMMAND(pcb_occl_debug, "pcb_occl_debug <level 0..255> - debug: black screen, then Minecraft only where its depth level >= level (0 = off)\n") {
	g_occDebug = args.ArgC() > 1 ? std::max(0, std::min(255, std::atoi(args[1]))) : 0;
}
CON_COMMAND(pcb_alpha_stats, "pcb_alpha_stats [stream=0] - histogram of the stream's CPU image alpha (per-pixel depth levels)\n") {
	const Stream &st = g_streams[args.ArgC() > 1 ? (std::atoi(args[1]) & 1) : 0];
	if (!ImageReady(&st)) return console->Print("[bridge] alpha: no image\n");
	int h[8] = {}, n = 0, zero = 0, full = 0;
	for (int y = 0; y < st.imgH; y += 4)
		for (int x = 0; x < st.imgW; x += 4) {
			int a = st.buf[st.cur][((size_t)y * kTexW + x) * 4 + 3];
			++h[a >> 5], ++n, zero += a == 0, full += a == 255;
		}
	console->Print("[bridge] alpha (stream %d, depth flag %d, %dx%d): 0: %d, 255: %d, bins/32: %d %d %d %d %d %d %d %d of %d\n", st.index, st.depthAlpha, st.imgW, st.imgH, zero, full, h[0], h[1],
	               h[2], h[3], h[4], h[5], h[6], h[7], n);
}
CON_COMMAND(pcb_composite_occlusion, "pcb_composite_occlusion <0|1> [eps=0.01] - hide Portal 2 objects behind Minecraft terrain (per pixel, from the image's depth alpha)\n") {
	if (args.ArgC() > 1 && args[1][0] == 'l') return void(g_occLog = 2);
	if (args.ArgC() > 1) g_occlusion = std::atoi(args[1]) != 0;
	if (args.ArgC() > 2) g_occEps = (float)std::atof(args[2]);
	if (args.ArgC() > 3) g_occEpsOpening = (float)std::atof(args[3]);
	console->Print("[bridge] occlusion %d (eps %.3f, in openings %.3f): %d objects, %d layers drawn since the last call; composite draw %.3f ms/frame main thread over %d frames\n", g_occlusion, g_occEps, g_occEpsOpening, g_occObjects,
	               g_occLayers, g_drawN ? 1000 * g_drawS / g_drawN : 0.0, g_drawN);
	g_occObjects = g_occLayers = 0, g_drawS = 0, g_drawN = 0;
}
void CompositeTraceViews(int n) { g_traceComposite = std::max(g_traceComposite, n), g_matchLog = std::max(g_matchLog, n); }
CON_COMMAND(pcb_composite_trace, "pcb_composite_trace <n> - log the next n composite view decisions (and unpredicted views)\n") {
	g_traceComposite = args.ArgC() > 1 ? std::atoi(args[1]) : 20;
	g_matchLog = g_traceComposite;
}
CON_COMMAND(pcb_composite, "pcb_composite [0|1] - Minecraft's own frame in every region view (default 1)\n") {
	if (args.ArgC() > 1) g_enabled = std::atoi(args[1]) != 0;
	if (!g_enabled) {
		HostView off{};
		link::WriteHostView(off);
		for (auto &st : g_streams) st.haveImage = false, st.camValid = false;
	}
	console->Print("[bridge] composite %s: %llu images received, request %u, region portal %d; portals set %d %d at (%.1f %.1f %.1f) (%.1f %.1f %.1f)\n", g_enabled ? "on" : "off",
	               (unsigned long long)g_frames, g_requestId, ClipRegionPortal(), g_portalSet[0], g_portalSet[1], g_portalPos[0].x, g_portalPos[0].y, g_portalPos[0].z, g_portalPos[1].x,
	               g_portalPos[1].y, g_portalPos[1].z);
}
CON_COMMAND(pcb_portal_view_depth, "pcb_portal_view_depth [0|1] - a portal view's Minecraft image is depth-tested against what Portal 2 drew in that view (objects coming out of the exit stay visible; default 1)\n") {
	if (args.ArgC() > 1) g_portalViewDepth = std::atoi(args[1]) != 0;
	console->Print("[bridge] portal view depth test %d\n", g_portalViewDepth);
}
CON_COMMAND(pcb_composite_stage, "pcb_composite_stage <2|0> - 2: inside the 3D view; 0: draw nothing (probes and requests keep running; debugging)\n") {
	if (args.ArgC() > 1) g_stage = std::atoi(args[1]);
	console->Print("[bridge] composite stage %d\n", g_stage);
}
CON_COMMAND(pcb_composite_paint, "pcb_composite_paint - removed in phase A (the in-view image is the only stage)\n") { console->Print("[bridge] composite paint overlay: removed\n"); }

uint64_t CompositeFramesReceived() { return g_frames; }
int CompositeHostFramesCounted() { return g_frameN; }

CON_COMMAND(pcb_composite_opts, "pcb_composite_opts <srgb 0|1> <tonemap_override 0|1> [overscan] [reprojection 0|1] [prediction 0|1] [upload 0|1] - in-view composite options\n") {
	if (args.ArgC() > 1) g_srgb = std::atoi(args[1]);
	if (args.ArgC() > 2) g_toneOverride = std::atoi(args[2]) != 0;
	if (args.ArgC() > 3) g_overscan = std::fmax(1.0f, std::fmin(1.25f, (float)std::atof(args[3])));
	if (args.ArgC() > 4) g_reproject = std::atoi(args[4]) != 0;
	if (args.ArgC() > 5) g_predict = std::atoi(args[5]) != 0;
	if (args.ArgC() > 6) g_upload = std::atoi(args[6]) != 0;
	console->Print("[bridge] composite opts: srgb %d, tonemap override %d, overscan %.2f, reprojection %d, prediction %d, upload %d\n", g_srgb, g_toneOverride, g_overscan, g_reproject, g_predict,
	               g_upload);
}
CON_COMMAND(pcb_composite_lag, "pcb_composite_lag - pose lag, host frame times, Minecraft images and composite costs since the last call\n") {
	console->Print("[bridge] composite lag: %d draws, pose error mean %.2f deg, max %.2f deg, latency %.2f / %.2f host frames (streams 0 / 1), %d uncovered samples\n", g_lagN,
	               g_lagN ? g_lagSum / g_lagN : 0.0f, g_lagMax, g_streams[0].latencyFrames, g_streams[1].latencyFrames, g_uncovered);
	console->Print("[bridge] host frames: %d, mean %.2f ms (%.0f fps), worst %.2f ms\n", g_frameN, g_frameN ? 1000 * g_frameSum / g_frameN : 0.0, g_frameN ? g_frameN / g_frameSum : 0.0,
	               1000 * g_frameMax);
	static uint64_t lastFrames = 0;
	static double lastT = 0;
	double now = NowS();
	if (lastT > 0)
		console->Print("[bridge] Minecraft images received: %llu in %.1f s (%.0f/s)\n", (unsigned long long)(g_frames - lastFrames), now - lastT, (g_frames - lastFrames) / std::max(0.001, now - lastT));
	lastFrames = g_frames, lastT = now;
	int cn = g_copyN.load();
	console->Print("[bridge] composite cost: main-thread pull %.3f ms/frame; render thread upload %.3f ms each (%d; before the regeneration %.3f, texture regeneration %.3f, after it %.3f; format %d, ping-pong %d); worker copy %.3f ms each (%d); pulls skipped %llu\n",
	               g_pullN ? 1000 * g_pullS / g_pullN : 0.0, g_uploadN ? 1000 * g_uploadS / g_uploadN : 0.0, g_uploadN, g_uploadN ? 1000 * g_dlPreS / g_uploadN : 0.0,
	               g_regenN ? 1000 * g_regenS / g_regenN : 0.0, g_uploadN ? 1000 * g_dlPostS / g_uploadN : 0.0, g_regenFmt, g_pingpong, cn ? 1000 * g_copyS / cn : 0.0, cn,
	               (unsigned long long)g_skippedPulls);
	g_pullS = g_regenS = g_uploadS = g_copyS = g_dlPreS = g_dlPostS = 0, g_pullN = g_regenN = g_uploadN = 0, g_copyN = 0, g_skippedPulls = 0;
	g_frameSum = g_frameMax = 0, g_frameN = 0;
	g_lagSum = g_lagMax = 0, g_lagN = 0, g_uncovered = 0;
}
CON_COMMAND(pcb_composite_pingpong, "pcb_composite_pingpong <0|1> - upload into two textures per stream in turn (default 1)\n") {
	if (args.ArgC() > 1) g_pingpong = std::atoi(args[1]) != 0;
	console->Print("[bridge] composite ping-pong textures %d\n", g_pingpong);
}
CON_COMMAND(pcb_mcshot, "pcb_mcshot - Minecraft saves its next composited view as cache/bridge/mcview/mcview_<request>.png\n") {
	g_shotFrames = 8;
	console->Print("[bridge] composite: Minecraft screenshot requested at request %u\n", g_requestId + 1);
}

CON_COMMAND(pcb_composite_second_every, "pcb_composite_second_every [N=3] - request the second camera every N host frames (1 = every frame)\n") {
	if (args.ArgC() > 1) g_secondEvery = std::max(1, std::atoi(args[1]));
	console->Print("[bridge] composite: second camera every %d host frames\n", g_secondEvery);
}

CON_COMMAND(pcb_mc_portals, "pcb_mc_portals [0|1] [render_distance=12] [res_scale=1] [depth=2] - a portal pair with both portals in Minecraft is drawn by Minecraft itself (v15)\n") {
	if (args.ArgC() > 1) g_mcPortals = std::atoi(args[1]) != 0;
	if (args.ArgC() > 2) g_mcPortalRd = std::max(0, std::min(32, std::atoi(args[2])));
	if (args.ArgC() > 3) g_mcPortalRes = std::max(0.25f, std::min(1.0f, (float)std::atof(args[3])));
	if (args.ArgC() > 4) g_mcPortalDepth = std::max(1, std::min(2, std::atoi(args[4])));
	console->Print("[bridge] mc portals %d: render distance %d, resolution x%.2f, depth %d; pair frames %d, pair views %d, crossings carried %d; object occlusion %d boxes / %d layers; open %.2f %.2f\n",
	               g_mcPortals, g_mcPortalRd, g_mcPortalRes, g_mcPortalDepth, g_pairFrames, g_pairViews, g_pairRemaps, g_objOccl, g_objLayers, g_portalOpen[0], g_portalOpen[1]);
}
CON_COMMAND(pcb_mc_portals_shape, "pcb_mc_portals_shape [half_width=32] [half_height=56] [props_occlusion_everywhere 0|1] - the opening Minecraft draws (Source units at full open)\n") {
	if (args.ArgC() > 1) g_openW = (float)std::atof(args[1]);
	if (args.ArgC() > 2) g_openH = (float)std::atof(args[2]);
	if (args.ArgC() > 3) g_occlPropsMain = std::atoi(args[3]) != 0;
	if (args.ArgC() > 4) g_pairDebug = std::atoi(args[4]);
	console->Print("[bridge] mc portals opening %.1f x %.1f u, props occlusion in every main view %d\n", g_openW, g_openH, g_occlPropsMain);
}
CON_COMMAND(pcb_mc_portals_centre, "pcb_mc_portals_centre [up_offset_u=0] - the opening Minecraft draws is centred this far up the portal's up axis\n") {
	if (args.ArgC() > 1) g_openUpOff = (float)std::atof(args[1]);
	console->Print("[bridge] mc portals opening centre %+.2f u up\n", g_openUpOff);
}
CON_COMMAND(pcb_mc_portals_fit, "pcb_mc_portals_fit <label...> - set the opening Minecraft draws to the mean centred fit of those pcb_measure_hole runs (Portal 2's visible opening)\n") {
	float cy = 0, hw = 0, hh = 0;
	int n = 0;
	for (int i = 1; i < args.ArgC(); ++i) {
		auto it = g_holeFits.find(args[i]);
		if (it == g_holeFits.end()) continue;
		cy += it->second.cy, hw += it->second.hw, hh += it->second.hh, ++n;
	}
	if (!n) return console->Print("[bridge] mc portals fit: no fits\n");
	g_openW = hw / n, g_openH = hh / n, g_openUpOff = cy / n;
	console->Print("[bridge] mc portals opening set from %d fits: %.2f x %.2f u, centre %+.2f u up\n", n, g_openW, g_openH, g_openUpOff);
}
CON_COMMAND(pcb_mc_portals_curve, "pcb_mc_portals_curve [0|1] [predict_frames] - the opening's growth: 0 smoothstep(open)^2, 1 the measured visible-edge curve; frames of image latency predicted\n") {
	if (args.ArgC() > 1) g_openCurveMode = std::atoi(args[1]);
	if (args.ArgC() > 2) g_openPredFrames = std::atoi(args[2]);
	console->Print("[bridge] mc portals open curve %d, predicted %d frames\n", g_openCurveMode, g_openPredFrames);
}
CON_COMMAND(pcb_mc_portals_flat, "pcb_mc_portals_flat [0|1] - Minecraft fills its pair openings with flat magenta (measuring where its opening ends)\n") {
	if (args.ArgC() > 1) g_mcFlatDebug = std::atoi(args[1]) != 0;
	console->Print("[bridge] mc portals flat magenta %d\n", g_mcFlatDebug);
}
CON_COMMAND(pcb_measure_hole, "pcb_measure_hole <portal> [frames=40] [label] [max|median] [lenient] - measure the magenta opening (Portal 2's: pcb_mc_portals_shape ... 3; Minecraft's: pcb_mc_portals_flat 1); lenient counts magenta blended under the rim\n") {
	g_holeMeasurePortal = args.ArgC() > 1 ? (std::atoi(args[1]) & 1) : 0;
	g_holeMeasure = args.ArgC() > 2 ? std::max(1, std::atoi(args[2])) : 40;
	std::snprintf(g_holeLabel, sizeof g_holeLabel, "%s", args.ArgC() > 3 ? args[3] : "hole");
	g_holeUseMax = args.ArgC() > 4 && !std::strcmp(args[4], "max");
	g_holeLenient = args.ArgC() > 5 && !std::strcmp(args[5], "lenient");
	g_holeMeasureFrames = 0;
	for (auto &v : g_holeR) v.clear();
	for (auto &v : g_holePx) v.clear();
}
CON_COMMAND(pcb_assert_edge_gap, "pcb_assert_edge_gap <image_label> <portal2_label> [max_px=1] - the image's opening reaches Portal 2's visible opening (two pcb_measure_hole runs): gap <= max_px in every direction\n") {
	if (args.ArgC() < 3 || !g_holeSaved.count(args[1]) || !g_holeSaved.count(args[2])) return TestCheck(false, "edge gap", "labels not measured");
	const auto &a = g_holeSaved[args[1]], &b = g_holeSaved[args[2]];
	const float lim = args.ArgC() > 3 ? (float)std::atof(args[3]) : 1.0f;
	std::vector<float> gaps;
	float worst = -1e9f;
	int wk = -1;
	for (int k = 0; k < 72; ++k) {
		if (a[k] < 0 || b[k] < 0) continue;
		float g = b[k] - a[k];
		gaps.push_back(g);
		if (g > worst) worst = g, wk = k;
	}
	std::sort(gaps.begin(), gaps.end());
	// per side (in the portal's own frame: right 0 deg, up 90, left 180, down 270), mean over +-15 deg
	float side[4] = {0, 0, 0, 0};
	for (int q = 0; q < 4; ++q) {
		int n = 0;
		for (int dk = -3; dk <= 3; ++dk) {
			int k = (q * 18 + dk + 72) % 72;
			if (a[k] >= 0 && b[k] >= 0) side[q] += b[k] - a[k], ++n;
		}
		side[q] = n ? side[q] / n : -99;
	}
	console->Print("[bridge] edge gap %s vs %s: right %.1f, top %.1f, left %.1f, bottom %.1f px\n", args[1], args[2], side[0], side[1], side[2], side[3]);
	char buf[260];
	std::snprintf(buf, sizeof buf, "%s vs %s: %zu directions, gap (Portal 2's opening minus the image's, px) median %.2f, p90 %.2f, max %.2f at %d deg (limit %.1f)", args[1], args[2], gaps.size(),
	              gaps.empty() ? 0.0f : gaps[gaps.size() / 2], gaps.empty() ? 0.0f : gaps[gaps.size() * 9 / 10], worst, wk * 5, lim);
	const float least = gaps.empty() ? 0 : gaps.front();
	TestCheck(gaps.size() >= 24 && worst <= lim && least >= -lim, "the image reaches Portal 2's rim exactly (gap and overshoot within the limit)", buf);
}
CON_COMMAND(pcb_assert_seen_through, "pcb_assert_seen_through <portal> <chell|targetname> [min_fraction=0.25] - at the next paint: that Portal 2 object, seen through the portal, is drawn there\n") {
	g_seenPortal = args.ArgC() > 1 ? (std::atoi(args[1]) & 1) : 0;
	std::snprintf(g_seenWhat, sizeof g_seenWhat, "%s", args.ArgC() > 2 ? args[2] : "chell");
	g_seenMin = args.ArgC() > 3 ? (float)std::atof(args[3]) : 0.25f;
	g_seenProbe = 1;
}
CON_COMMAND(pcb_measure_open, "pcb_measure_open <portal> [frames=150] | reset | assert [max_lead_frames=0] - log Portal 2's opening against Minecraft's every frame of an open animation (fire the portal just before; pair views magenta)\n") {
	if (args.ArgC() > 1 && !std::strcmp(args[1], "reset")) {
		g_openCmpN = g_openLeadN = g_openOldSpotN = 0, g_openLeadMax = 0;
		return;
	}
	if (args.ArgC() > 1 && !std::strcmp(args[1], "assert")) {
		const int lim = args.ArgC() > 2 ? std::atoi(args[2]) : 0;
		char buf[240];
		std::snprintf(buf, sizeof buf, "%d frames compared: the image's opening led Portal 2's visible opening in %d (by up to %.3f of the full opening; limit %d frames); %d frames showed the opening at the portal's previous spot",
		              g_openCmpN, g_openLeadN, g_openLeadMax, lim, g_openOldSpotN);
		return TestCheck(g_openCmpN >= 20 && g_openLeadN <= lim, "Minecraft's opening never leads Portal 2's", buf);
	}
	g_openMeasurePortal = args.ArgC() > 1 ? (std::atoi(args[1]) & 1) : 1;
	g_openMeasure = args.ArgC() > 2 ? std::max(1, std::atoi(args[2])) : 150;
	g_openMeasureN = 0;
}
CON_COMMAND(pcb_assert_own_streams, "pcb_assert_own_streams [reset] - no region view was drawn with another camera's image since the reset (views without their own image show the fallback)\n") {
	if (args.ArgC() > 1 && !std::strcmp(args[1], "reset")) {
		g_foreignAvoided = g_fallbackViews = g_foreignDrawn = 0;
		g_regionViewsAtReset = g_regionViews;
		return;
	}
	char buf[200];
	int views = g_regionViews - g_regionViewsAtReset;
	std::snprintf(buf, sizeof buf, "%d region views: %d drawn with another stream's image, %d fell back (no own image yet), %d old-camera images refused", views, g_foreignDrawn, g_fallbackViews,
	              g_foreignAvoided);
	TestCheck(g_foreignDrawn == 0 && views > 0, "every region view drawn from its own stream", buf);
}

}  // namespace bridge

namespace bridge {
CON_COMMAND(pc_mc_overscan, "pc_mc_overscan [1.0-1.25] - the composite's overscan in Minecraft mode (1.0: Minecraft's window = the screen, its GUI 1:1)\n") {
	if (args.ArgC() > 1) g_mcOverscan = std::fmax(1.0f, std::fmin(1.25f, (float)std::atof(args[1])));
	console->Print("[bridge] Minecraft-mode overscan %.2f\n", g_mcOverscan);
}
CON_COMMAND(pc_mc_render_scale, "pc_mc_render_scale [auto|0.5-1] - the scale of Minecraft's rendered frame: auto (default) follows how fast Minecraft keeps up with Portal 2\n") {
	if (args.ArgC() > 1) {
		if (!std::strcmp(args[1], "auto")) g_scaleFixed = 0;
		else g_scaleFixed = std::fmax(kMinScale, std::fmin(1.0f, (float)std::atof(args[1]))), g_renderScale = g_scaleFixed;
	}
	console->Print("[bridge] Minecraft render scale %.2f (%s), target %d fps\n", g_renderScale.load(), g_scaleFixed > 0 ? "fixed" : "auto", g_targetFps);
}
CON_COMMAND(pc_mc_target_fps, "pc_mc_target_fps [n=90] - the frame rate the automatic render scale wants from Minecraft (at most Portal 2's own)\n") {
	if (args.ArgC() > 1) g_targetFps = std::max(20, std::min(400, std::atoi(args[1])));
	console->Print("[bridge] Minecraft render scale target %d fps\n", g_targetFps);
}
}  // namespace bridge
