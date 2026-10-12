// Portalcraft bridge: the Direct3D 9 backend's proof of concept and test commands (PLAN-vulkan.md V0).
//   pcb_d3d9_init          hook the game's device (only when it is running Direct3D 9: -vulkan on Linux, any Windows run)
//   pcb_d3d9_overlay 0|1   draw a red rectangle, a triangle and a translucent square over each frame, inside a state block
//   pcb_d3d9_stats         Present counts and threads, shader census (models seen, largest shader), failures
//   pcb_d3d9_shot <name>   the presented frame as cache/bridge/d3d9_<name>.png (what the player sees, overlays included)
//   pcb_d3d9_probe x y / pcb_assert_d3d9_pixel r g b [tol]   one presented pixel
#include "bridge/d3d9/d3d9hook.hpp"

#include "bridge/bridge.hpp"
#include "bridge/render_backend.hpp"
#include "Modules/Tier1.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

namespace bridge {

void TestCheck(bool ok, const char *what, const char *detail);  // testassert.cpp

using namespace d3d9;

namespace {

std::atomic<int> g_overlay{0};
bool g_registered = false;

struct Vtx { float x, y, z, rhw; uint32_t color; };

void DrawTestOverlay(void *dev, int w, int h) {
	if (!g_overlay.load()) return;
	OverlayScope scope(dev, w, h);
	if (!scope.ok()) return;
	// (1) a red rectangle with Clear and a rectangle list: top-left 40,40, 200 x 120
	D3DRECT_ r{40, 40, 240, 160};
	Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, const D3DRECT_ *, DWORD_, DWORD_, float, DWORD_)>(dev, kClear)(dev, 1, &r, D3DCLEAR_TARGET_, 0xFFFF0000u, 1.0f, 0);
	// (2) a coloured triangle and a half-transparent yellow square with DrawPrimitiveUP, fixed function, pre-transformed
	Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_)>(dev, kSetFVF)(dev, D3DFVF_XYZRHW_ | D3DFVF_DIFFUSE_);
	const float cx = w * 0.5f, cy = h * 0.5f;
	Vtx tri[3] = {{cx - 0.5f, cy - 150 - 0.5f, 0, 1, 0xFF00FF00u}, {cx + 130 - 0.5f, cy + 100 - 0.5f, 0, 1, 0xFF0000FFu}, {cx - 130 - 0.5f, cy + 100 - 0.5f, 0, 1, 0xFFFFFFFFu}};
	auto draw = Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, UINT_, const void *, UINT_)>(dev, kDrawPrimitiveUP);
	draw(dev, D3DPT_TRIANGLELIST_, 1, tri, sizeof(Vtx));
	const float x0 = 300, y0 = 40, x1 = 500, y1 = 160;
	const uint32_t yc = 0x80FFFF00u;  // alpha 128
	Vtx sq[4] = {{x0 - 0.5f, y0 - 0.5f, 0, 1, yc}, {x1 - 0.5f, y0 - 0.5f, 0, 1, yc}, {x0 - 0.5f, y1 - 0.5f, 0, 1, yc}, {x1 - 0.5f, y1 - 0.5f, 0, 1, yc}};
	draw(dev, D3DPT_TRIANGLESTRIP_, 2, sq, sizeof(Vtx));
}

// census (shader creation hook, render thread or loader thread)
std::mutex g_censusMx;
struct Census {
	uint64_t n = 0, tokens = 0, maxTokens = 0;
};
std::map<int, Census> g_vsModels, g_psModels;

void OnShader(void *, bool isPixel, const uint32_t *code, void *) {
	const int model = ShaderModel(code);
	const uint32_t t = ShaderTokens(code);
	std::lock_guard<std::mutex> lk(g_censusMx);
	Census &c = (isPixel ? g_psModels : g_vsModels)[model];
	++c.n, c.tokens += t;
	if (t > c.maxTokens) c.maxTokens = t;
}

void EnsureRegistered() {
	if (g_registered) return;
	g_registered = true;
	AddPresentListener(DrawTestOverlay);
	AddShaderListener(OnShader);
}

struct Boot {
	~Boot() { d3d9::Shutdown(); }  // plugin unload: the vtable slots go back before this code is unmapped
} g_boot;

}  // namespace

CON_COMMAND(pcb_d3d9_init, "pcb_d3d9_init - hook the game's Direct3D 9 device (-vulkan on Linux; Windows)\n") {
	if (d3d9::Init()) EnsureRegistered();
}

CON_COMMAND(pcb_d3d9_overlay, "pcb_d3d9_overlay <0|1> - (tests) draw a red rectangle, a triangle and a translucent square over every frame through the Direct3D 9 hook\n") {
	if (!d3d9::Active() && d3d9::Init()) EnsureRegistered();
	g_overlay = args.ArgC() > 1 ? std::atoi(args[1]) : 1;
	console->Print("[bridge] d3d9 overlay %s\n", g_overlay.load() ? "on" : "off");
}

CON_COMMAND(pcb_d3d9_stats, "pcb_d3d9_stats [reset] - Present counters, threads and the shader census of the Direct3D 9 hook\n") {
	Counters &s = d3d9::Stats();
	console->Print("[bridge] d3d9: %s, device %p, Present %llu (swap chain %llu, Ex %llu) on %u thread(s), Reset %llu, overlays %llu, HRESULT failures %llu, "
	               "shaders created VS %llu PS %llu\n",
	               d3d9::Active() ? "hooked" : "off", d3d9::EngineDevice(), (unsigned long long)s.presents.load(), (unsigned long long)s.swapPresents.load(),
	               (unsigned long long)s.presentExs.load(), s.presentThreads.load(), (unsigned long long)s.resets.load(), (unsigned long long)s.overlays.load(),
	               (unsigned long long)s.hrFail.load(), (unsigned long long)s.vsCreated.load(), (unsigned long long)s.psCreated.load());
	{
		const double n = (double)(s.presents.load() + s.swapPresents.load());
		console->Print("[bridge] d3d9: per Present: our listeners %.2f ms, the engine's own Present %.2f ms (worst %.1f ms) over %.0f calls\n", n ? s.ourPresentNs.load() / n * 1e-6 : 0.0,
		               n ? s.enginePresentNs.load() / n * 1e-6 : 0.0, s.enginePresentMaxNs.load() * 1e-6, n);
	}
	{
		const double n = (double)(s.presents.load() + s.swapPresents.load());
		console->Print("[bridge] d3d9: per Present: draw/bind/texture/constant listeners %.2f ms over %.0f calls (%.2f us a call, slowest call %.1f ms)\n", n ? s.hookNs.load() / n * 1e-6 : 0.0,
		               n ? s.hookCalls.load() / n : 0.0, s.hookCalls.load() ? s.hookNs.load() * 1e-3 / s.hookCalls.load() : 0.0, s.hookMaxNs.load() * 1e-6);
		console->Print("%s", rb::CutBackendPatchTiming().c_str());
		if (args.ArgC() > 1 && !std::strcmp(args[1], "reset"))
			s.presents = 0, s.swapPresents = 0, s.ourPresentNs = 0, s.enginePresentNs = 0, s.enginePresentMaxNs = 0, s.hookNs = 0, s.hookCalls = 0, s.hookMaxNs = 0;
	}
	console->Print("[bridge] d3d9: Present threads (id:count) %s\n", d3d9::ThreadSummary().c_str());
	std::lock_guard<std::mutex> lk(g_censusMx);
	for (int pass = 0; pass < 2; ++pass)
		for (auto &kv : pass ? g_psModels : g_vsModels)
			console->Print("[bridge] d3d9 census: %s %d_%d: %llu shaders, mean %.0f tokens, max %llu\n", pass ? "ps" : "vs", kv.first >> 8, kv.first & 255,
			               (unsigned long long)kv.second.n, kv.second.n ? (double)kv.second.tokens / kv.second.n : 0.0, (unsigned long long)kv.second.maxTokens);
}

CON_COMMAND(pcb_d3d9_shot, "pcb_d3d9_shot <name> - (tests) save the presented frame, overlays included, as cache/bridge/d3d9_<name>.png (a name starting with / is a full path)\n") {
	const std::string n = args.ArgC() > 1 ? args[1] : "shot";
	d3d9::RequestShot(n[0] == '/' ? n : CacheDir() + "/d3d9_" + n + ".png");
}

CON_COMMAND(pcb_d3d9_info, "pcb_d3d9_info - (tests) log the engine device's back buffer, render target and viewport sizes at the next Present\n") {
	d3d9::RequestInfo();
}

CON_COMMAND(pcb_d3d9_probe, "pcb_d3d9_probe <x> <y> - (tests) read the presented pixel at x, y (top-left origin) at the next Present\n") {
	if (args.ArgC() < 3) return;
	d3d9::RequestProbe(std::atoi(args[1]), std::atoi(args[2]));
}

CON_COMMAND(pcb_assert_d3d9_pixel, "pcb_assert_d3d9_pixel <r> <g> <b> [tol=8] - the pixel pcb_d3d9_probe read\n") {
	if (args.ArgC() < 4) return;
	const int r = std::atoi(args[1]), g = std::atoi(args[2]), b = std::atoi(args[3]), tol = args.ArgC() > 4 ? std::atoi(args[4]) : 8;
	uint32_t c = 0;
	const bool done = d3d9::ProbeResult(c);
	const int pr = c & 255, pg = (c >> 8) & 255, pb = (c >> 16) & 255;
	char buf[160];
	std::snprintf(buf, sizeof buf, "read %d %d %d (%s), want %d %d %d +-%d", pr, pg, pb, done ? "done" : "NOT READ", r, g, b, tol);
	TestCheck(done && std::abs(pr - r) <= tol && std::abs(pg - g) <= tol && std::abs(pb - b) <= tol, "d3d9 presented pixel", buf);
}

}  // namespace bridge
