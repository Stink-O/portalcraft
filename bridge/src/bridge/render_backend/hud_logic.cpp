// Portalcraft bridge: the Minecraft hand/HUD overlay, renderer-neutral half (DESIGN-mcplayer M2a/M2b, "phase C").
//
// The Minecraft player's hand, held item and GUI come from Minecraft's own rendering (SkyCraft's overlay): one image per Minecraft
// frame, hand + GUI on a transparent background, shipped through the v17 HUD triple buffer. Going through the material system
// would cost ITexture::Download's ~5-6 ms per image, so the image is drawn straight through the graphics API instead, right
// before the frame is presented: the backend (render_backend.hpp) hooks the present function and calls HudOnPresent from the
// render thread. This file decides what to draw and when; the backend uploads and draws (GL on Linux: gl_hud.cpp).
//
// Visibility is decided on the main thread (mc.cpp: Minecraft mode, not paused, console closed, level live) and read here through
// atomics. Test commands: pcb_glhud_test (a test pattern, M2a's probe), pcb_glhud_probe / pcb_assert_glhud_pixel (read back the
// presented pixel), pcb_glhud_shot (a PNG of the presented frame: Portal 2's own screenshots are taken before this overlay),
// pcb_glhud_stats, pcb_assert_glhud.
//
// (This file was split out of glhud.cpp, now gl_hud.cpp; the code moved unchanged except for the backend calls.)
#include "bridge/bridge.hpp"
#include "bridge/link.hpp"
#include "bridge/render_backend.hpp"
#include "bridge/platform/platform.hpp"

#include "Modules/Tier1.hpp"
#include "Utils/lodepng.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace bridge {

using namespace skycraft::proto;

void RunOnRenderThread(void (*fn)(void *), void *arg);  // terrain.cpp
bool BugMarkToastActive();                                // bugmark.cpp
int BugMarkToastRects(int W, int H, int (*rects)[4], uint32_t *rgba, int max);
void TestCheck(bool ok, const char *what, const char *detail);  // testassert.cpp

namespace {

// ---- shared with the main thread -------------------------------------------------------------------
std::atomic<int64_t> g_stateAtNs{0};  // (bridge #56) when mc.cpp last set the state: it runs once per rendered world frame, so no update = no world is on screen
bool StateFresh() {
	const int64_t now = plat::MonotonicNs();
	return now - g_stateAtNs.load() < 250000000ll;  // 250 ms: a loading screen, the main menu, a video, any frame without the world view
}
std::atomic<int> g_show{0};       // draw the Minecraft HUD (mc.cpp decides)
std::atomic<int> g_cursorOn{0};   // an MC screen is open: draw the cursor
std::atomic<int> g_preload{0};    // a switch to the Minecraft player is pending: upload, don't draw
std::atomic<int> g_drawnFlag{0};  // the HUD was drawn on the last present
std::atomic<int> g_readyFlag{0};  // a screen-sized HUD image is uploaded (the J switch waits for it)
std::atomic<int> g_cursorX{0}, g_cursorY{0};  // HUD pixels, top-left origin
std::atomic<int> g_testMode{0};   // 1: the test pattern instead of the HUD
std::atomic<int> g_enabled{1};    // pc_mc_hud
std::atomic<int> g_shutdown{0};
std::atomic<int> g_inHook{0};
std::atomic<int> g_probeReq{0}, g_probeX{0}, g_probeY{0}, g_probeDone{0};
std::atomic<uint32_t> g_probeRgba{0};
std::atomic<int> g_shotReq{0};
std::string g_shotName;  // set before g_shotReq
std::atomic<int> g_burstLeft{0}, g_burstIdx{0};
std::atomic<int> g_encoders{0};  // PNG threads still running (unload waits for them: their code is in this library)
std::string g_burstName;

rb::HudStats g_st;  // render thread writes, main thread reads; torn reads only affect printouts

// ---- the last image (render thread) --------------------------------------------------------------------
bool g_haveFrame = false, g_flipY = false;
int g_frameW = 0, g_frameH = 0, g_guiScale = 0;
uint32_t g_frameMcFlags = 0, g_frameCamera = 0;
std::atomic<float> g_fadeA{0.0f}, g_fadeR{1.0f}, g_fadeG{1.0f}, g_fadeB{1.0f};  // (bridge #63) Portal 2's screen fade over our hand/HUD
std::vector<uint8_t> g_pattern;
int g_patW = 0, g_patH = 0;
uint64_t g_patFrame = 0;

double NowMs() { return plat::MonotonicNs() * 1e-6; }

// M2a's probe image: premultiplied colour bars and squares at known places (pcb_assert_glhud_pixel), plus a
// small block that moves every frame so a live view shows the upload is per frame.
void MakePattern(int w, int h) {
	if (g_patW != w || g_patH != h) {
		g_patW = w, g_patH = h;
		g_pattern.assign((size_t)w * h * 4, 0);
		auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {  // top-left origin, premultiplied
			uint8_t *p = &g_pattern[((size_t)(h - 1 - y) * w + x) * 4];
			p[0] = (uint8_t)(r * a / 255), p[1] = (uint8_t)(g * a / 255), p[2] = (uint8_t)(b * a / 255), p[3] = a;
		};
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x) {
				if (x >= 64 && x < 192 && y >= 64 && y < 192) put(x, y, 255, 0, 0, 255);                     // opaque red
				else if (x >= 224 && x < 352 && y >= 64 && y < 192) put(x, y, 0, 255, 0, 255);              // opaque green
				else if (x >= 384 && x < 512 && y >= 64 && y < 192) put(x, y, 0, 0, 255, 128);              // half blue
				else if (y >= h - 48 && y < h - 8 && x >= w / 2 - 182 && x < w / 2 + 182) put(x, y, 40, 40, 40, 200);  // a hotbar-sized bar
				else if (((x / 32) + (y / 32)) % 2 == 0 && y >= 224 && y < 288) put(x, y, 255, 255, 255, 64);  // a checker strip
			}
	}
	// the moving block: 24 x 24, along the strip
	static int lastX = -1;
	auto block = [&](int bx, bool on) {
		for (int y = 300; y < 324 && y < h; ++y)
			for (int x = bx; x < bx + 24 && x < w; ++x) {
				uint8_t *p = &g_pattern[((size_t)(h - 1 - y) * w + x) * 4];
				p[0] = on ? 255 : 0, p[1] = on ? 200 : 0, p[2] = 0, p[3] = on ? 255 : 0;
			}
	};
	if (lastX >= 0) block(lastX, false);
	lastX = (int)((g_patFrame * 4) % (uint64_t)std::max(1, w - 24));
	block(lastX, true);
	++g_patFrame;
}

void Readbacks(int W, int H) {
	if (!g_probeReq.load() && !g_shotReq.load() && !g_burstLeft.load()) return;
	if (g_probeReq.load()) {
		uint8_t px[4] = {0, 0, 0, 0};
		const int x = std::max(0, std::min(W - 1, g_probeX.load())), y = std::max(0, std::min(H - 1, g_probeY.load()));
		rb::HudBackendReadPixel(W, H, x, y, px);
		g_probeRgba = (uint32_t)px[0] | (uint32_t)px[1] << 8 | (uint32_t)px[2] << 16 | (uint32_t)px[3] << 24;
		g_probeReq = 0;
		g_probeDone = 1;
	}
	const bool burst = g_burstLeft.load() > 0;
	if (g_shotReq.load() || burst) {
		std::vector<uint8_t> raw;
		bool bottomUp = true;
		rb::HudBackendReadFrame(W, H, raw, bottomUp);
		if (raw.size() < (size_t)W * H * 4) raw.assign((size_t)W * H * 4, 0);
		std::string path, shotPath;
		// (bridge #24: a name starting with '/' is a full path, GlHudShotTo)
		const std::string shotFile = !g_shotName.empty() && g_shotName[0] == '/' ? g_shotName : CacheDir() + "/glhud_" + g_shotName + ".png";
		if (burst && g_shotReq.load()) shotPath = shotFile, g_shotReq = 0;
		if (burst) {
			char nb[64];
			const int k = g_burstIdx.fetch_add(1);
			std::snprintf(nb, sizeof nb, "_%02d", k);
			path = CacheDir() + "/glhud_" + g_burstName + nb + ".png";
			console->Print("[bridge] glhud burst %s frame %d: swap %llu, HUD %s\n", g_burstName.c_str(), k, (unsigned long long)g_st.swaps, g_show.load() ? "shown" : "hidden");
			g_burstLeft.fetch_sub(1);
		} else {
			path = shotFile;
			g_shotReq = 0;
		}
		g_encoders.fetch_add(1);
		std::thread([raw = std::move(raw), bottomUp, W, H, path, shotPath]() mutable {
			std::vector<uint8_t> top((size_t)W * H * 4);
			for (int y = 0; y < H; ++y) std::memcpy(&top[(size_t)y * W * 4], &raw[(size_t)(bottomUp ? H - 1 - y : y) * W * 4], (size_t)W * 4);
			for (size_t i = 3; i < top.size(); i += 4) top[i] = 255;
			lodepng::encode(path, top, (unsigned)W, (unsigned)H);
			if (!shotPath.empty()) lodepng::encode(shotPath, top, (unsigned)W, (unsigned)H);
			g_encoders.fetch_sub(1);
		}).detach();
	}
}

// bridge #18: the user saw Minecraft's hand and hotbar drawn over Portal 2's closed captions (the overlay was drawn at the
// present, after everything). Now it is drawn in the render thread's command stream right before Portal 2's VGUI paint
// (GlHudPaint, queued from the VEngineVGui::Paint hook ahead of Portal 2's own paint), into the engine's frame: captions, the
// console, menus and the pause screen go on top. The present only draws it when that path did not run this frame (fallback),
// and does the test readbacks.
std::atomic<uint64_t> g_paintDrawFor{0};  // the present number the paint pass drew for
std::atomic<void *> g_window{nullptr};
std::atomic<int> g_paintPath{1};          // pc_mc_hud_under_ui
uint64_t g_paintDraws = 0;

// Upload the newest HUD image and draw it (render thread, API state saved/restored by the caller).
void UploadAndDraw(int W, int H, bool show, bool preload, int test, bool keepFb) {
	if (show || preload) {
		if (test) {
			MakePattern(W, H);
			rb::HudBackendUpload(g_pattern.data(), W, H, g_st);
			g_frameW = W, g_frameH = H, g_flipY = false, g_guiScale = 0, g_frameMcFlags = 0, g_frameCamera = 0;
			g_haveFrame = true;
		} else {
			const HudSlotHdr *hdr = nullptr;
			const uint8_t *px = nullptr;
			// only an image of the screen's size (Minecraft's window follows the screen in Minecraft mode, HUD option A;
			// one rendered before a resize would show the GUI at the wrong scale for a frame)
			// (or the frame size mc.cpp asked for, when the screen is larger than the composite's textures: McFrameSize scales it down with
			// the screen's shape, and the backend scales the image back up; before 2026-10-12 a screen above 2048 wide had no HUD at all)
			auto sizeOk = [&](int iw, int ih) {
				int rw = 0, rh = 0;
				McFrameRequested(rw, rh);
				const bool screenSize = std::abs(iw - W) <= 2 && std::abs(ih - H) <= 2;
				const bool askedSize = rw > 0 && std::abs(iw - rw) <= 2 && std::abs(ih - rh) <= 2 &&
				                       std::abs((long long)rw * H - (long long)rh * W) <= 2LL * std::max(W, H);
				return screenSize || askedSize;
			};
			if (link::AcquireHud(hdr, px) && hdr->width > 0 && hdr->height > 0 && hdr->width <= kMaxHudW && hdr->height <= kMaxHudH &&
			    sizeOk((int)hdr->width, (int)hdr->height)) {
				rb::HudBackendUpload(px, (int)hdr->width, (int)hdr->height, g_st);
				g_frameW = (int)hdr->width, g_frameH = (int)hdr->height;
				g_flipY = !(hdr->flags & kSlotBottomUp);
				g_guiScale = (int)hdr->guiScale, g_frameMcFlags = hdr->mcFlags, g_frameCamera = hdr->cameraMode;
				g_haveFrame = true;
			}
		}
		if (g_haveFrame && show) {
			rb::HudDraw d;
			d.W = W, d.H = H, d.intoEnginesFrame = keepFb;
			d.testPattern = test != 0;
			d.frameW = g_frameW, d.frameH = g_frameH, d.flipY = g_flipY, d.guiScale = g_guiScale, d.mcFlags = g_frameMcFlags, d.camera = g_frameCamera;
			d.cursorOn = g_cursorOn.load() != 0, d.cursorX = g_cursorX.load(), d.cursorY = g_cursorY.load();
			d.fadeA = g_fadeA.load(), d.fadeR = g_fadeR.load(), d.fadeG = g_fadeG.load(), d.fadeB = g_fadeB.load();
			const double t0 = NowMs();
			rb::HudBackendDraw(d);
			const double ms = NowMs() - t0;
			++g_st.drawn;
			g_drawnFlag = 1;
			g_st.drawMs += ms;
			if (ms > g_st.drawMax) g_st.drawMax = ms;
		}
	}
	if (!(g_haveFrame && show)) g_drawnFlag = 0;
	g_readyFlag = g_haveFrame && !test ? 1 : 0;
}

// Render thread, queued just before Portal 2's VGUI paint of this frame.
void PaintPass(void *) {
	if (g_shutdown.load() || !g_paintPath.load()) return;
	void *window = g_window.load();
	if (!window || !rb::HudBackendReady()) return;
	const int test = g_testMode.load();
	const bool show = test || (g_enabled.load() && g_show.load() && StateFresh());
	const bool preload = !show && g_enabled.load() && g_preload.load() && StateFresh();
	if (!show && !preload) return;  // (the present clears the state)
	int W = 0, H = 0;
	rb::HudBackendWindowSize(window, &W, &H);
	if (W <= 0 || H <= 0) return;
	rb::HudBackendBegin(g_st);
	UploadAndDraw(W, H, show, preload, test, true);
	rb::HudBackendEnd(g_st, false);
	++g_paintDraws;
	g_paintDrawFor = g_st.swaps + 1;
}

void PrintStats() {
	const rb::HudStats s = g_st;
	console->Print("[bridge] glhud: %llu swaps, %llu drawn, %llu uploads (%dx%d) mean %.3f ms max %.3f ms, draw mean %.3f ms max %.3f ms, screen %dx%d, state mismatches %llu%s%s, "
	               "GL errors %llu (togl's own before ours %llu), HUD frames published %llu\n",
	               (unsigned long long)s.swaps, (unsigned long long)s.drawn, (unsigned long long)s.uploads, s.texW, s.texH, s.uploads ? s.upMs / s.uploads : 0.0, s.upMax,
	               s.drawn ? s.drawMs / s.drawn : 0.0, s.drawMax, s.lastW, s.lastH, (unsigned long long)s.stateMismatch, s.stateMismatch ? ": last " : "", s.mismatch,
	               (unsigned long long)s.glErrors, (unsigned long long)s.preErrors, (unsigned long long)link::HudFramesPublished());
}

}  // namespace

// ---- the present (render thread), called by the backend's hook --------------------------------------------------------------------
namespace rb {

bool HudPresentBegin() {
	g_inHook.fetch_add(1);
	return !g_shutdown.load();
}
void HudPresentEnd() { g_inHook.fetch_sub(1); }

void HudOnPresent(void *window) {
	++g_st.swaps;
	g_window = window;
	const int test = g_testMode.load();
	const bool show = test || (g_enabled.load() && g_show.load() && StateFresh());
	// a J switch is pending: Minecraft already ships the HUD; keep the newest uploaded, so the switch frame has it
	const bool preload = !show && g_enabled.load() && g_preload.load() && StateFresh();
	const bool toast = BugMarkToastActive();  // (bridge #24)
	const bool wantRead = g_probeReq.load() || g_shotReq.load() || g_burstLeft.load() > 0 || HudBackendWantsFrame() || toast;
	const bool paintDid = g_paintPath.load() && g_paintDrawFor.load() == g_st.swaps;
	if (!show) {
		if (!preload) g_haveFrame = false, g_readyFlag = 0;  // a stale frame never flashes when the HUD comes back
		if (!wantRead && !preload) return;
	}
	if (paintDid && !wantRead) return;
	if (!HudBackendReady()) return;
	int W = 0, H = 0;
	HudBackendWindowSize(window, &W, &H);
	if (W <= 0 || H <= 0) return;
	g_st.lastW = W, g_st.lastH = H;
	HudBackendBegin(g_st);  // drains the engine's own errors first, so ours are counted alone
	if (!paintDid) UploadAndDraw(W, H, show, preload, test, false);
	Readbacks(W, H);
	HudBackendAfterOverlay(W, H);
	if (toast) {  // (bridge #24: after both readbacks)
		static int r[1200][4];
		static uint32_t c[1200];
		const int n = BugMarkToastRects(W, H, r, c, 1200);
		if (n > 0) HudBackendToast(W, H, r, c, n);
	}
	HudBackendEnd(g_st, g_st.swaps % 64 == 1 || test);
}

}  // namespace rb

// Main thread, VEngineVGui::Paint hook (composite.cpp), before Portal 2's own paint, once per frame.
void GlHudPaint() {
	if (g_shutdown.load() || !g_paintPath.load() || !g_window.load()) return;
	RunOnRenderThread(PaintPass, nullptr);
}

void GlHudSetFade(float a, float r, float g, float b) { g_fadeA = a, g_fadeR = r, g_fadeG = g, g_fadeB = b; }

// ---- main thread ---------------------------------------------------------------------------------------
bool GlHudDrawn() { return g_drawnFlag.load() != 0; }
bool GlHudReady() { return g_readyFlag.load() != 0; }
void GlHudHideNow() {  // (bridge #56) a level load / disconnect: hidden at once, the stale frame dropped by the next present
	g_show = 0, g_preload = 0, g_readyFlag = 0;
}
void GlHudSetState(bool show, bool preload, bool cursorOn, int cursorX, int cursorY) {
	g_stateAtNs = plat::MonotonicNs();
	g_show = show ? 1 : 0;
	g_preload = preload ? 1 : 0;
	g_cursorOn = cursorOn ? 1 : 0;
	g_cursorX = cursorX, g_cursorY = cursorY;
}
bool GlHudEnabled() { return g_enabled.load() != 0; }

bool GlHudInit() { return rb::HudBackendInstall(); }

void GlHudShutdown() {
	g_shutdown = 1;
	rb::HudBackendUninstall();
	// let a present in flight on the render thread leave the hook before the link (the HUD pixels) is unmapped;
	// the GPU objects are left to the device/context (they cannot be deleted from this thread)
	for (int i = 0; i < 200 && g_inHook.load(); ++i) plat::SleepUs(1000);
	for (int i = 0; i < 500 && g_encoders.load(); ++i) plat::SleepUs(10000);  // PNG encodes in flight (bounded 5 s)
}

CON_COMMAND(pc_mc_hud, "pc_mc_hud [0|1] - draw the Minecraft player's hand, held item and GUI (Minecraft's own image) in Minecraft mode\n") {
	if (args.ArgC() > 1) g_enabled = std::atoi(args[1]) ? 1 : 0;
	console->Print("[bridge] Minecraft HUD %s\n", g_enabled.load() ? "on" : "off");
}

CON_COMMAND(pc_mc_hud_under_ui, "pc_mc_hud_under_ui [0|1] - draw the Minecraft hand/HUD before Portal 2's 2D UI (captions, console, menus on top; default 1) or at the swap (0)\n") {
	if (args.ArgC() > 1) g_paintPath = std::atoi(args[1]) ? 1 : 0;
	console->Print("[bridge] Minecraft HUD under Portal 2's UI %d (%llu paint-pass draws)\n", g_paintPath.load(), (unsigned long long)g_paintDraws);
}

CON_COMMAND(pcb_glhud_test, "pcb_glhud_test <0|1> - (tests) draw a test pattern through the direct-GL overlay, uploaded every frame\n") {
	g_testMode = args.ArgC() > 1 ? std::atoi(args[1]) : 1;
	console->Print("[bridge] glhud test pattern %s\n", g_testMode.load() ? "on" : "off");
}

CON_COMMAND(pcb_glhud_stats, "pcb_glhud_stats [reset] - direct-GL overlay counters and timings (render thread CPU time)\n") {
	if (args.ArgC() > 1 && !std::strcmp(args[1], "reset")) {
		g_st.drawn = g_st.uploads = 0, g_st.upMs = g_st.upMax = g_st.drawMs = g_st.drawMax = 0;
		return;
	}
	PrintStats();
}

CON_COMMAND(pcb_assert_glhud, "pcb_assert_glhud <min_drawn> - the overlay drew at least min_drawn frames since the last stats reset, with togl's GL state restored and no GL error of ours\n") {
	const uint64_t want = args.ArgC() > 1 ? (uint64_t)std::atoll(args[1]) : 30;
	PrintStats();
	char buf[320];
	std::snprintf(buf, sizeof buf, "drawn %llu (want >= %llu), state mismatches %llu %s, GL errors %llu", (unsigned long long)g_st.drawn, (unsigned long long)want,
	              (unsigned long long)g_st.stateMismatch, g_st.mismatch, (unsigned long long)g_st.glErrors);
	TestCheck(g_st.drawn >= want && g_st.stateMismatch == 0 && g_st.glErrors == 0, "direct-GL overlay", buf);
}

CON_COMMAND(pcb_glhud_probe, "pcb_glhud_probe <x> <y> - (tests) read the presented pixel at x, y (top-left origin) at the next swap\n") {
	if (args.ArgC() < 3) return;
	g_probeX = std::atoi(args[1]), g_probeY = std::atoi(args[2]);
	g_probeDone = 0;
	g_probeReq = 1;
}
CON_COMMAND(pcb_assert_glhud_pixel, "pcb_assert_glhud_pixel <r> <g> <b> [tol=8] - the pixel pcb_glhud_probe read\n") {
	if (args.ArgC() < 4) return;
	const int r = std::atoi(args[1]), g = std::atoi(args[2]), b = std::atoi(args[3]), tol = args.ArgC() > 4 ? std::atoi(args[4]) : 8;
	const uint32_t c = g_probeRgba.load();
	const int pr = c & 255, pg = (c >> 8) & 255, pb = (c >> 16) & 255;
	char buf[160];
	std::snprintf(buf, sizeof buf, "probe (%d %d) read %d %d %d (%s), want %d %d %d +-%d", g_probeX.load(), g_probeY.load(), pr, pg, pb, g_probeDone.load() ? "done" : "NOT READ", r, g, b, tol);
	TestCheck(g_probeDone.load() && std::abs(pr - r) <= tol && std::abs(pg - g) <= tol && std::abs(pb - b) <= tol, "presented pixel", buf);
}
CON_COMMAND(pcb_glhud_burst, "pcb_glhud_burst <name> <frames> - (tests) save that many consecutive presented frames as cache/bridge/glhud_<name>_NN.png\n") {
	if (args.ArgC() < 3) return;
	g_burstName = args[1];
	g_burstIdx = 0;
	g_burstLeft = std::max(1, std::min(120, std::atoi(args[2])));
}

// (bridge #24) the bug mark's screenshot: the presented frame at the next present, to a full path
void GlHudShotTo(const std::string &path) {
	g_shotName = path;
	g_shotReq = 1;
}

CON_COMMAND(pcb_glhud_shot, "pcb_glhud_shot <name> - (tests) save the presented frame, overlay included, as cache/bridge/glhud_<name>.png\n") {
	g_shotName = args.ArgC() > 1 ? args[1] : "shot";
	g_shotReq = 1;
}

}  // namespace bridge
