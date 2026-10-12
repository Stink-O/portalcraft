// Portalcraft bridge: a self-test of the Direct3D 9 HUD backend (hud_d3d9.cpp) against a CPU model of the GL overlay's shader maths
// (gl_hud.cpp's fragment shader: premultiplied alpha, the cursor arrow, Minecraft's invert blend, the screen fade). It needs no Minecraft:
//   pcb_d3d9_hudtest [flip=1] [fade=0] [cursor=1] [invert=1] [tag=a]
// At the next Present it clears the back buffer to a flat colour, uploads a synthetic HUD image the size of the screen, draws it
// through rb::HudBackend*, reads the frame back and compares every pixel with the model. The result is a console line and a PNG
// cache/bridge/d3d9_hudtest_<tag>.png of what was presented.
#include "bridge/render_backend.hpp"

#include "bridge/bridge.hpp"
#include "bridge/link.hpp"
#include "bridge/d3d9/d3d9hook.hpp"
#include "Modules/Tier1.hpp"
#include "Utils/lodepng.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace bridge {

void TestCheck(bool ok, const char *what, const char *detail);  // testassert.cpp

namespace {

struct Req {
	bool flip = true;  // rows top-down
	float fade = 0.0f;
	bool cursor = true, invert = true;
	std::string tag = "a";
};
std::atomic<int> g_req{0};
Req g_cfg;
bool g_registered = false;
std::atomic<int> g_done{0}, g_ok{0};

using namespace d3d9;

void Run(void *dev, int W, int H) {
	if (!g_req.exchange(0)) return;
	const Req rq = g_cfg;
	if (!rb::HudBackendReady()) {
		Printf("[bridge] d3d9 hudtest: backend not ready\n");
		g_done = 1;
		return;
	}
	const uint8_t bg[3] = {77, 102, 128};
	{
		OverlayScope scope(dev, W, H);
		if (!scope.ok()) {
			g_done = 1;
			return;
		}
		Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, const D3DRECT_ *, DWORD_, DWORD_, float, DWORD_)>(dev, kClear)(dev, 0, nullptr, D3DCLEAR_TARGET_,
		                                                                                                         0xFF000000u | (bg[0] << 16) | (bg[1] << 8) | bg[2], 1.0f, 0);
	}
	// the synthetic image, premultiplied RGBA, rows as delivered (top-down when rq.flip)
	std::vector<uint8_t> img((size_t)W * H * 4, 0);
	auto set = [&](int x, int yTop, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
		const int row = rq.flip ? yTop : (H - 1 - yTop);
		uint8_t *p = &img[((size_t)row * W + x) * 4];
		p[0] = (uint8_t)(r * a / 255), p[1] = (uint8_t)(g * a / 255), p[2] = (uint8_t)(b * a / 255), p[3] = a;
	};
	for (int y = 0; y < H; ++y)
		for (int x = 0; x < W; ++x) {
			if (x >= 40 && x < 168 && y >= 40 && y < 168) set(x, y, 255, 0, 0, 255);                          // opaque red
			else if (x >= 200 && x < 328 && y >= 40 && y < 168) set(x, y, 0, 255, 0, 128);                    // half green
			else if (x >= 360 && x < 488 && y >= 40 && y < 168) set(x, y, 0, 0, 255, 255);                    // opaque blue
			else if (y >= H - 60 && y < H - 20 && x >= W / 2 - 182 && x < W / 2 + 182) set(x, y, 40, 40, 40, 200);  // hotbar-ish
			else if (y >= 200 && y < 264 && ((x / 16) + (y / 16)) % 2 == 0) set(x, y, 255, 255, 255, 64);     // checker
			else if (y >= 280 && y < 300) set(x, y, (uint8_t)(x * 255 / std::max(1, W - 1)), 128, 255 - (uint8_t)(x * 255 / std::max(1, W - 1)), 255);  // gradient strip
			else if (std::abs(x - W / 2) < 30 && std::abs(y - H / 2) < 30) set(x, y, 255, 255, 0, 255);          // yellow in the crosshair rectangle
			else if (std::abs(x - W / 2) < 60 && std::abs(y - H / 2) < 60) set(x, y, 255, 255, 255, 255);       // white around it (outside the rectangle part)
		}
	rb::HudStats st;
	st.swaps = 1;  // (the state check runs on swaps % 64 == 1)
	rb::HudDraw d;
	d.W = W, d.H = H;
	d.frameW = W, d.frameH = H;
	d.flipY = rq.flip;
	d.guiScale = rq.invert ? 2 : 0;
	d.camera = 0;
	d.mcFlags = 0;
	d.cursorOn = rq.cursor;
	d.cursorX = 700, d.cursorY = 400;
	d.fadeA = rq.fade, d.fadeR = 0.2f, d.fadeG = 0.0f, d.fadeB = 0.6f;
	rb::HudBackendBegin(st);
	rb::HudBackendUpload(img.data(), W, H, st);
	rb::HudBackendDraw(d);
	rb::HudBackendEnd(st, true);
	std::vector<uint8_t> got;
	int gw = 0, gh = 0;
	if (!ReadBackBuffer(dev, got, gw, gh) || gw != W || gh != H) {
		Printf("[bridge] d3d9 hudtest: readback failed (%d x %d)\n", gw, gh);
		g_done = 1;
		return;
	}
	lodepng::encode(CacheDir() + "/d3d9_hudtest_" + rq.tag + ".png", got, (unsigned)W, (unsigned)H);

	// ---- the model (gl_hud.cpp's shader, pixel centres at x + 0.5) ----
	const float sx = 1.0f, sy = 1.0f;
	const float cxs = d.cursorX * sx, cys = d.cursorY * sy;
	const float g = d.guiScale * sx, cx = W * 0.5f, cy = H * 0.5f;
	const float ir[4] = {cx - 12 * g, cy - 12 * g, cx + 12 * g, cy + 28 * g};
	long bad = 0, edgeBad = 0, tested = 0, maxd = 0;
	int firstBad[2] = {-1, -1};
	for (int y = 0; y < H; ++y)
		for (int x = 0; x < W; ++x) {
			const float px = x + 0.5f, py = y + 0.5f;
			const bool inInv = d.guiScale > 0 && px >= ir[0] && py >= ir[1] && px < ir[2] && py < ir[3];
			const int row = rq.flip ? y : (H - 1 - y);
			const uint8_t *s = &img[((size_t)row * W + x) * 4];
			float c[4] = {s[0] / 255.0f, s[1] / 255.0f, s[2] / 255.0f, s[3] / 255.0f};
			float dst[3] = {bg[0] / 255.0f, bg[1] / 255.0f, bg[2] / 255.0f};
			bool nearArrow = false;
			if (inInv) {
				c[0] = c[1] = c[2] = c[3] = 0;
			} else if (d.cursorOn) {
				const float qx = px - cxs, qy = py - cys;
				nearArrow = qx >= -1.5f && qy >= -1.5f && qy < 19.5f && qx <= qy * 0.6f + 1.5f;
				if (qx >= 0 && qy >= 0 && qy < 18 && qx <= qy * 0.6f) {
					const bool edge = qx < 1.5f || qx > qy * 0.6f - 1.5f || qy > 16.5f;
					c[0] = c[1] = c[2] = edge ? 0.0f : 1.0f, c[3] = 1.0f;
				}
			}
			const float fa = d.fadeA;
			float o[3];
			for (int k = 0; k < 3; ++k) {
				const float fc = k == 0 ? d.fadeR : k == 1 ? d.fadeG : d.fadeB;
				const float cc = c[k] * (1.0f - fa) + fc * (fa * c[3]);
				o[k] = cc + dst[k] * (1.0f - c[3]);
			}
			if (inInv) {  // pass 1: src = overlay rgb; result = src * (1 - dst) + dst * (1 - src)
				const float sr[3] = {s[0] / 255.0f, s[1] / 255.0f, s[2] / 255.0f};
				for (int k = 0; k < 3; ++k) o[k] = sr[k] * (1.0f - dst[k]) + dst[k] * (1.0f - sr[k]);
			}
			const uint8_t *q = &got[((size_t)y * W + x) * 4];
			long dm = 0;
			for (int k = 0; k < 3; ++k) dm = std::max(dm, (long)std::labs((long)std::lround(o[k] * 255.0f) - (long)q[k]));
			++tested;
			maxd = std::max(maxd, dm);
			if (dm > 2) {
				if (nearArrow) ++edgeBad;
				else if (++bad == 1) firstBad[0] = x, firstBad[1] = y;
			}
		}
	char buf[300];
	std::snprintf(buf, sizeof buf, "%s: %ld px tested, %ld off by > 2/255 (max %ld) away from the cursor edge, %ld on its edge, state mismatches %llu %s%s", rq.tag.c_str(), tested, bad, maxd, edgeBad,
	              (unsigned long long)st.stateMismatch, st.mismatch, bad ? "; first at" : "");
	Printf("[bridge] d3d9 hudtest %s", buf);
	if (bad) Printf(" %d,%d", firstBad[0], firstBad[1]);
	Printf("\n");
	g_ok = bad == 0 && st.stateMismatch == 0;
	g_done = 1;
}

}  // namespace

CON_COMMAND(pcb_d3d9_hudtest, "pcb_d3d9_hudtest [flip=1] [fade=0] [cursor=1] [invert=1] [tag=a] - (tests) draw a synthetic HUD through the Direct3D 9 backend and compare it with the GL shader's maths\n") {
	if (!rb::HudBackendInstall()) return;
	if (!g_registered) {
		g_registered = true;
		d3d9::AddPresentListener(Run);
	}
	g_cfg = Req{};
	if (args.ArgC() > 1) g_cfg.flip = std::atoi(args[1]) != 0;
	if (args.ArgC() > 2) g_cfg.fade = (float)std::atof(args[2]);
	if (args.ArgC() > 3) g_cfg.cursor = std::atoi(args[3]) != 0;
	if (args.ArgC() > 4) g_cfg.invert = std::atoi(args[4]) != 0;
	if (args.ArgC() > 5) g_cfg.tag = args[5];
	g_done = 0, g_ok = 0;
	g_req = 1;
}

CON_COMMAND(pcb_assert_d3d9_hudtest, "pcb_assert_d3d9_hudtest - the last pcb_d3d9_hudtest matched the model\n") {
	TestCheck(g_done.load() && g_ok.load(), "d3d9 HUD self-test", g_done.load() ? (g_ok.load() ? "matches the model" : "differs from the model (see the log)") : "did not run");
}

}  // namespace bridge
