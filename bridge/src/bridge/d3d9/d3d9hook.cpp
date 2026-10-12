// Portalcraft bridge: Direct3D 9 hook layer. See d3d9hook.hpp.
#include <chrono>
#include "bridge/d3d9/d3d9hook.hpp"

#include "bridge/bridge.hpp"
#include "Modules/Tier1.hpp"
#include "Utils/lodepng.hpp"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#endif

namespace bridge::d3d9 {

void Printf(const char *fmt, ...) {
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (console) console->Print("%s", buf);
}

// ---- state -----------------------------------------------------------------------------------------------------------------------------------
namespace {

constexpr int kMaxPatches = 24;
struct Patch {
	void **vt = nullptr;
	int slot = -1;
	void *orig = nullptr, *hook = nullptr;
	const char *name = "";
};
Patch g_patches[kMaxPatches];
int g_nPatches = 0;
void **g_devVt = nullptr, **g_swapVt = nullptr;

std::atomic<int> g_active{0}, g_shutdown{0}, g_inHook{0};
std::atomic<void *> g_engineDev{nullptr};
void *g_throwDev = nullptr;  // never presented; only its vtable was used
Counters g_stats;
struct ThreadCount { std::atomic<uint32_t> id{0}; std::atomic<uint64_t> n{0}; };
ThreadCount g_threads[8];

std::mutex g_listMx;
std::vector<PresentFn> g_presentL;
std::vector<ResetFn> g_resetL;
std::vector<ShaderFn> g_shaderL;
std::atomic<BindFn> g_bindL{nullptr};
std::atomic<TextureFn> g_texL{nullptr};
std::atomic<DrawFn> g_drawL{nullptr};
std::atomic<ConstFn> g_constL{nullptr};
std::atomic<int> g_bindHooks{0};

thread_local int t_depth = 0;     // inside an engine Present (the device method may call the swap chain method)
thread_local int t_overlay = 0;   // inside an OverlayScope

// original entry points
using PresentT = HRESULT_(PCB_D3DAPI *)(void *, const RECT_ *, const RECT_ *, void *, const void *);
using SwapPresentT = HRESULT_(PCB_D3DAPI *)(void *, const RECT_ *, const RECT_ *, void *, const void *, DWORD_);
using ResetT = HRESULT_(PCB_D3DAPI *)(void *, D3DPRESENT_PARAMETERS_ *);
using CreateShaderT = HRESULT_(PCB_D3DAPI *)(void *, const uint32_t *, void **);
using SetShaderT = HRESULT_(PCB_D3DAPI *)(void *, void *);
using SetTextureT = HRESULT_(PCB_D3DAPI *)(void *, DWORD_, void *);
using DrawPrimT = HRESULT_(PCB_D3DAPI *)(void *, DWORD_, UINT_, UINT_);
using DrawIdxT = HRESULT_(PCB_D3DAPI *)(void *, DWORD_, int32_t, UINT_, UINT_, UINT_, UINT_);
using SetConstFT = HRESULT_(PCB_D3DAPI *)(void *, UINT_, const float *, UINT_);
PresentT o_Present;
SwapPresentT o_SwapPresent;
ResetT o_Reset;
CreateShaderT o_CreateVS, o_CreatePS;
SetShaderT o_SetVS, o_SetPS;
SetTextureT o_SetTexture;
DrawPrimT o_DrawPrim;
DrawIdxT o_DrawIdx;
SetConstFT o_SetPSConstF, o_SetVSConstF;

// per-device overlay resources (render thread)
void *g_sb = nullptr;        // IDirect3DStateBlock9 (D3DSBT_ALL), released at Reset
void *g_shotSurf = nullptr;  // system-memory copy target for screenshots
void *g_resolve = nullptr;   // non-multisampled copy of a multisampled back buffer
int g_shotW = 0, g_shotH = 0;
std::atomic<int> g_encoders{0};

std::mutex g_shotMx;
std::string g_shotPath;
std::atomic<int> g_shotReq{0};
std::atomic<int> g_infoReq{0};
std::atomic<int> g_probeReq{0}, g_probeX{0}, g_probeY{0}, g_probeDone{0};
std::atomic<uint32_t> g_probeRgba{0};

struct InFlight {
	InFlight() { g_inHook.fetch_add(1); }
	~InFlight() { g_inHook.fetch_sub(1); }
};
// times one listener call (bind/texture/draw/constants): pcb_d3d9_stats reports the render thread's cost per Present
struct ListenerTimer {
	std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
	~ListenerTimer() {
		const uint64_t ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
		g_stats.hookNs.fetch_add(ns, std::memory_order_relaxed);
		uint64_t m = g_stats.hookMaxNs.load(std::memory_order_relaxed);
		while (ns > m && !g_stats.hookMaxNs.compare_exchange_weak(m, ns)) {}
		g_stats.hookCalls.fetch_add(1, std::memory_order_relaxed);
	}
};

// ---- memory protection ---------------------------------------------------------------------------------------------------------------
bool WriteSlot(void **vt, int slot, void *value) {
	void **p = &vt[slot];
#ifdef _WIN32
	DWORD old;
	if (!VirtualProtect(p, sizeof(void *), PAGE_READWRITE, &old)) return false;
	*p = value;
	VirtualProtect(p, sizeof(void *), old, &old);
	return true;
#else
	const long ps = sysconf(_SC_PAGESIZE);
	void *page = (void *)((uintptr_t)p & ~(uintptr_t)(ps - 1));
	if (mprotect(page, (size_t)ps * 2, PROT_READ | PROT_WRITE) != 0) return false;  // (a slot can straddle a page boundary)
	*p = value;
	// the table lives in relro/rodata: back to read-only (the kernel does not keep the old flags for us)
	mprotect(page, (size_t)ps * 2, PROT_READ);
	return true;
#endif
}

bool PatchSlot(void **vt, int slot, void *hook, void **origOut, const char *name) {
	if (!vt || g_nPatches >= kMaxPatches) return false;
	void *orig = vt[slot];
	if (!orig) return false;
	if (!WriteSlot(vt, slot, hook)) return false;
	*origOut = orig;
	g_patches[g_nPatches++] = {vt, slot, orig, hook, name};
	return true;
}

// ---- engine-device helpers ---------------------------------------------------------------------------------------------------------
bool BackBufferSizeImpl(void *dev, int &w, int &h) {
	void *bb = nullptr;
	if (Failed(Method<HRESULT_(PCB_D3DAPI *)(void *, UINT_, UINT_, DWORD_, void **)>(dev, kGetBackBuffer)(dev, 0, 0, 0, &bb)) || !bb) return false;
	D3DSURFACE_DESC_ d{};
	const bool ok = !Failed(Method<HRESULT_(PCB_D3DAPI *)(void *, D3DSURFACE_DESC_ *)>(bb, kSurfGetDesc)(bb, &d));
	Release(bb);
	if (ok) w = (int)d.Width, h = (int)d.Height;
	return ok;
}

void ReleaseDeviceResources() {
	if (g_sb) Release(g_sb), g_sb = nullptr;
	if (g_shotSurf) Release(g_shotSurf), g_shotSurf = nullptr;
	if (g_resolve) Release(g_resolve), g_resolve = nullptr;
	g_shotW = g_shotH = 0;
}

// ---- screenshots / probes (after the listeners drew) --------------------------------------------------------------------------------------
void ReadbackFrame(void *dev, int W, int H) {
	if (!g_shotReq.load() && !g_probeReq.load()) return;
	(void)W, (void)H;
	std::vector<uint8_t> rgba;
	int w = 0, h = 0;
	if (!ReadBackBuffer(dev, rgba, w, h)) {
		g_stats.hrFail++;
		return;
	}
	if (g_probeReq.load()) {
		const int x = std::max(0, std::min(w - 1, g_probeX.load())), y = std::max(0, std::min(h - 1, g_probeY.load()));
		const uint8_t *p = &rgba[((size_t)y * w + x) * 4];
		g_probeRgba = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)255u << 24;
		g_probeReq = 0;
		g_probeDone = 1;
	}
	if (g_shotReq.load()) {
		std::string path;
		{
			std::lock_guard<std::mutex> lk(g_shotMx);
			path = g_shotPath;
		}
		g_shotReq = 0;
		g_encoders.fetch_add(1);
		std::thread([rgba = std::move(rgba), w, h, path]() mutable {
			lodepng::encode(path, rgba, (unsigned)w, (unsigned)h);
			g_encoders.fetch_sub(1);
		}).detach();
	}
}

// ---- Present --------------------------------------------------------------------------------------------------------------------------------
void NoteThread() {
#ifdef _WIN32
	const uint32_t id = (uint32_t)GetCurrentThreadId();
#else
	const uint32_t id = (uint32_t)gettid();
#endif
	for (auto &s : g_threads) {
		uint32_t v = s.id.load();
		if (v == id) {
			s.n++;
			return;
		}
		if (v == 0 && s.id.compare_exchange_strong(v, id)) {
			g_stats.presentThreads++;
			s.n++;
			return;
		}
	}
}

void OnPresent(void *dev) {
	if (dev == g_throwDev || g_shutdown.load()) return;
	void *expect = nullptr;
	g_engineDev.compare_exchange_strong(expect, dev);
	if (g_engineDev.load() != dev) return;  // a second device (not the game's): leave it alone
	NoteThread();
	int w = 0, h = 0;
	if (!BackBufferSizeImpl(dev, w, h)) return;
	std::vector<PresentFn> l;
	{
		std::lock_guard<std::mutex> lk(g_listMx);
		l = g_presentL;
	}
	if (g_infoReq.exchange(0)) {
		D3DVIEWPORT9_ vp{};
		Method<HRESULT_(PCB_D3DAPI *)(void *, D3DVIEWPORT9_ *)>(dev, kGetViewport)(dev, &vp);
		void *rt = nullptr, *bb = nullptr;
		Method<HRESULT_(PCB_D3DAPI *)(void *, UINT_, void **)>(dev, kGetRenderTarget)(dev, 0, &rt);
		Method<HRESULT_(PCB_D3DAPI *)(void *, UINT_, UINT_, DWORD_, void **)>(dev, kGetBackBuffer)(dev, 0, 0, 0, &bb);
		D3DSURFACE_DESC_ dr{}, db{};
		if (rt) Method<HRESULT_(PCB_D3DAPI *)(void *, D3DSURFACE_DESC_ *)>(rt, kSurfGetDesc)(rt, &dr);
		if (bb) Method<HRESULT_(PCB_D3DAPI *)(void *, D3DSURFACE_DESC_ *)>(bb, kSurfGetDesc)(bb, &db);
		D3DDEVICE_CREATION_PARAMETERS_ cp{};
		Method<HRESULT_(PCB_D3DAPI *)(void *, D3DDEVICE_CREATION_PARAMETERS_ *)>(dev, kGetCreationParameters)(dev, &cp);
		Printf("[bridge] d3d9 info: back buffer %p %ux%u fmt %u, render target 0 %p %ux%u fmt %u, viewport %u,%u %ux%u z %.2f..%.2f, behaviour 0x%x, focus window %p\n", bb, db.Width, db.Height,
		       db.Format, rt, dr.Width, dr.Height, dr.Format, vp.X, vp.Y, vp.Width, vp.Height, vp.MinZ, vp.MaxZ, (unsigned)cp.BehaviorFlags, cp.hFocusWindow);
		void *sc = nullptr;
		if (!Failed(Method<HRESULT_(PCB_D3DAPI *)(void *, UINT_, void **)>(dev, 14)(dev, 0, &sc)) && sc) {
			D3DPRESENT_PARAMETERS_ pp{};
			Method<HRESULT_(PCB_D3DAPI *)(void *, D3DPRESENT_PARAMETERS_ *)>(sc, 9)(sc, &pp);
			Printf("[bridge] d3d9 info: present parameters %ux%u fmt %u count %u msaa %u swap %u windowed %d vsync-interval 0x%x\n", pp.BackBufferWidth, pp.BackBufferHeight, pp.BackBufferFormat,
			       pp.BackBufferCount, pp.MultiSampleType, pp.SwapEffect, pp.Windowed, pp.PresentationInterval);
			Release(sc);
		}
#ifndef _WIN32
		if (cp.hFocusWindow) {
			void *h = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_NOLOAD);
			if (!h) h = RTLD_DEFAULT;
			auto getSize = (void (*)(void *, int *, int *))dlsym(h, "SDL_GetWindowSize");
			auto getVk = (void (*)(void *, int *, int *))dlsym(h, "SDL_Vulkan_GetDrawableSize");
			int a = 0, b = 0, c2 = 0, d2 = 0;
			if (getSize) getSize(cp.hFocusWindow, &a, &b);
			if (getVk) getVk(cp.hFocusWindow, &c2, &d2);
			Printf("[bridge] d3d9 info: SDL window %dx%d, Vulkan drawable %dx%d\n", a, b, c2, d2);
		}
#endif
		if (rt) Release(rt);
		if (bb) Release(bb);
	}
	for (PresentFn fn : l) fn(dev, w, h);
	ReadbackFrame(dev, w, h);
}

static void NoteEnginePresent(std::chrono::steady_clock::time_point t1) {
	const uint64_t ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t1).count();
	g_stats.enginePresentNs += ns;
	uint64_t m = g_stats.enginePresentMaxNs.load();
	while (ns > m && !g_stats.enginePresentMaxNs.compare_exchange_weak(m, ns)) {}
}

HRESULT_ PCB_D3DAPI H_Present(void *self, const RECT_ *a, const RECT_ *b, void *hwnd, const void *dirty) {
	InFlight f;
	g_stats.presents++;
	if (t_depth == 0) {
		const auto t0 = std::chrono::steady_clock::now();
		++t_depth;
		OnPresent(self);
		--t_depth;
		g_stats.ourPresentNs += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
	}
	++t_depth;
	const auto t1 = std::chrono::steady_clock::now();
	const HRESULT_ hr = o_Present(self, a, b, hwnd, dirty);
	NoteEnginePresent(t1);
	--t_depth;
	if (Failed(hr)) g_stats.hrFail++;
	return hr;
}
HRESULT_ PCB_D3DAPI H_SwapPresent(void *self, const RECT_ *a, const RECT_ *b, void *hwnd, const void *dirty, DWORD_ flags) {
	InFlight f;
	g_stats.swapPresents++;
	if (t_depth == 0) {
		const auto t0 = std::chrono::steady_clock::now();
		struct Done {
			std::chrono::steady_clock::time_point t0;
			~Done() { g_stats.ourPresentNs += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(); }
		} done{t0};
		// swap chain 9 -> its device (GetDevice, slot 8)
		void *dev = nullptr;
		if (!Failed(Method<HRESULT_(PCB_D3DAPI *)(void *, void **)>(self, 8)(self, &dev)) && dev) {
			++t_depth;
			OnPresent(dev);
			--t_depth;
			Release(dev);
		}
	}
	++t_depth;
	const auto t1 = std::chrono::steady_clock::now();
	const HRESULT_ hr = o_SwapPresent(self, a, b, hwnd, dirty, flags);
	NoteEnginePresent(t1);
	--t_depth;
	if (Failed(hr)) g_stats.hrFail++;
	return hr;
}

void OnReset(void *dev) {
	g_stats.resets++;
	std::vector<ResetFn> l;
	{
		std::lock_guard<std::mutex> lk(g_listMx);
		l = g_resetL;
	}
	for (ResetFn fn : l) fn(dev);
	ReleaseDeviceResources();
}
HRESULT_ PCB_D3DAPI H_Reset(void *self, D3DPRESENT_PARAMETERS_ *pp) {
	InFlight f;
	if (self != g_throwDev) OnReset(self);
	return o_Reset(self, pp);
}

// ---- shaders ---------------------------------------------------------------------------------------------------------------------------------
HRESULT_ CreateShaderCommon(void *self, const uint32_t *code, void **out, bool isPixel) {
	InFlight f;
	const HRESULT_ hr = (isPixel ? o_CreatePS : o_CreateVS)(self, code, out);
	if (Failed(hr) || !out || !*out || self == g_throwDev || t_overlay > 0) return hr;
	(isPixel ? g_stats.psCreated : g_stats.vsCreated)++;
	std::vector<ShaderFn> l;
	{
		std::lock_guard<std::mutex> lk(g_listMx);
		l = g_shaderL;
	}
	for (ShaderFn fn : l) fn(self, isPixel, code, *out);
	return hr;
}
HRESULT_ PCB_D3DAPI H_CreateVS(void *self, const uint32_t *code, void **out) { return CreateShaderCommon(self, code, out, false); }
HRESULT_ PCB_D3DAPI H_CreatePS(void *self, const uint32_t *code, void **out) { return CreateShaderCommon(self, code, out, true); }

HRESULT_ PCB_D3DAPI H_SetVS(void *self, void *shader) {
	InFlight f;
	if (t_overlay == 0) {
		g_stats.vsSet++;
		if (BindFn fn = g_bindL.load()) {
			ListenerTimer lt;
			if (void *r = fn(self, false, shader)) shader = r;
		}
	}
	return o_SetVS(self, shader);
}
HRESULT_ PCB_D3DAPI H_SetPS(void *self, void *shader) {
	InFlight f;
	if (t_overlay == 0) {
		g_stats.psSet++;
		if (BindFn fn = g_bindL.load()) {
			ListenerTimer lt;
			if (void *r = fn(self, true, shader)) shader = r;
		}
	}
	return o_SetPS(self, shader);
}
HRESULT_ PCB_D3DAPI H_SetTexture(void *self, DWORD_ stage, void *tex) {
	InFlight f;
	const HRESULT_ hr = o_SetTexture(self, stage, tex);
	if (t_overlay == 0) {
		if (TextureFn fn = g_texL.load()) {
			ListenerTimer lt;
			fn(self, stage, tex);
		}
	}
	return hr;
}

HRESULT_ PCB_D3DAPI H_DrawPrim(void *self, DWORD_ type, UINT_ start, UINT_ count) {
	InFlight f;
	if (t_overlay == 0) {
		if (DrawFn fn = g_drawL.load()) {
			ListenerTimer lt;
			fn(self);
		}
	}
	return o_DrawPrim(self, type, start, count);
}
HRESULT_ PCB_D3DAPI H_DrawIdx(void *self, DWORD_ type, int32_t baseVertex, UINT_ minVertex, UINT_ numVertices, UINT_ startIndex, UINT_ primCount) {
	InFlight f;
	if (t_overlay == 0) {
		if (DrawFn fn = g_drawL.load()) {
			ListenerTimer lt;
			fn(self);
		}
	}
	return o_DrawIdx(self, type, baseVertex, minVertex, numVertices, startIndex, primCount);
}
HRESULT_ PCB_D3DAPI H_SetPSConstF(void *self, UINT_ start, const float *data, UINT_ count) {
	InFlight f;
	if (t_overlay == 0) {
		if (ConstFn fn = g_constL.load()) {
			ListenerTimer lt;
			fn(self, true, start, count);
		}
	}
	return o_SetPSConstF(self, start, data, count);
}
HRESULT_ PCB_D3DAPI H_SetVSConstF(void *self, UINT_ start, const float *data, UINT_ count) {
	InFlight f;
	if (t_overlay == 0) {
		if (ConstFn fn = g_constL.load()) {
			ListenerTimer lt;
			fn(self, false, start, count);
		}
	}
	return o_SetVSConstF(self, start, data, count);
}

// ---- discovery -------------------------------------------------------------------------------------------------------------------------------
using Direct3DCreate9T = void *(PCB_D3DAPI *)(UINT_);
using CreateDeviceT = HRESULT_(PCB_D3DAPI *)(void *, UINT_, DWORD_, void *, DWORD_, D3DPRESENT_PARAMETERS_ *, void **);

Direct3DCreate9T FindCreate9(std::string &which) {
#ifdef _WIN32
	HMODULE m = GetModuleHandleA("d3d9.dll");
	which = "d3d9.dll";
	if (!m) return nullptr;
	return (Direct3DCreate9T)GetProcAddress(m, "Direct3DCreate9");
#else
	// -vulkan: shaderapivk.so links libdxvk_d3d9.so. Without it (togl/OpenGL mode) there is no Direct3D 9 device to reach.
	void *h = dlopen("libdxvk_d3d9.so", RTLD_NOW | RTLD_NOLOAD);
	which = "libdxvk_d3d9.so";
	if (!h) return nullptr;
	return (Direct3DCreate9T)dlsym(h, "Direct3DCreate9");
#endif
}

// the throw-away window the throw-away device needs
struct ThrowWindow {
	void *handle = nullptr;
#ifndef _WIN32
	void (*destroy)(void *) = nullptr;
#endif
	bool Create() {
#ifdef _WIN32
		handle = CreateWindowExA(0, "STATIC", "pcb_d3d9", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
		return handle != nullptr;
#else
		void *h = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_NOLOAD);
		if (!h) h = RTLD_DEFAULT;
		auto create = (void *(*)(const char *, int, int, int, int, uint32_t))dlsym(h, "SDL_CreateWindow");
		destroy = (void (*)(void *))dlsym(h, "SDL_DestroyWindow");
		if (!create || !destroy) return false;
		// SDL_WINDOWPOS_UNDEFINED 0x1FFF0000, SDL_WINDOW_HIDDEN 0x8, SDL_WINDOW_VULKAN 0x10000000
		handle = create("pcb_d3d9", 0x1FFF0000, 0x1FFF0000, 64, 64, 0x8 | 0x10000000);
		return handle != nullptr;
#endif
	}
	void Destroy() {
		if (!handle) return;
#ifdef _WIN32
		DestroyWindow((HWND)handle);
#else
		destroy(handle);
#endif
		handle = nullptr;
	}
};

#ifdef _WIN32
// Microsoft's d3d9 gives every device its own copy of the device function table (on the heap; every copy points at the same code in
// d3d9.dll), where DXVK shares one table in its image. With private tables, patching the throw-away device's copy never reaches the
// engine's device (seen on Windows 11: Present 0 for the whole run), so the engine's device is looked up and its own table patched.
// The lookup compares pointers only (no calls): a device is an object whose table holds the same Present / Reset / QueryInterface /
// AddRef code as the throw-away device. It searches the writable data of the engine's renderer DLL for a pointer to such an object,
// then (if none) one level deeper, inside the objects that data points to.
struct ReadableMap {
	std::vector<std::pair<uintptr_t, uintptr_t>> r;  // committed, readable, not guard: [begin, end), sorted
	void Build() {
		r.clear();
		MEMORY_BASIC_INFORMATION mi;
		for (uintptr_t a = 0x10000; VirtualQuery((void *)a, &mi, sizeof mi) == sizeof mi; a = (uintptr_t)mi.BaseAddress + mi.RegionSize) {
			const DWORD rd = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
			if (mi.State == MEM_COMMIT && (mi.Protect & rd) && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
				const uintptr_t b = (uintptr_t)mi.BaseAddress, e = b + mi.RegionSize;
				if (!r.empty() && r.back().second == b) r.back().second = e;
				else r.push_back({b, e});
			}
			if ((uintptr_t)mi.BaseAddress + mi.RegionSize <= a) break;  // (wrapped at the top of the address space)
		}
	}
	bool Ok(uintptr_t p, size_t n) const {
		auto it = std::upper_bound(r.begin(), r.end(), std::make_pair(p, ~(uintptr_t)0));
		if (it == r.begin()) return false;
		--it;
		return p >= it->first && p + n <= it->second && p + n >= p;
	}
};

std::string ToHex(uintptr_t v) {
	char b[20];
	std::snprintf(b, sizeof b, "%lx", (unsigned long)v);
	return b;
}

bool LooksLikeDevice(const ReadableMap &m, uintptr_t obj, void **probeVt) {
	if (!obj || (obj & 3) || !m.Ok(obj, sizeof(void *))) return false;
	void **vt = *(void ***)obj;
	if (!vt || vt == probeVt || ((uintptr_t)vt & 3) || !m.Ok((uintptr_t)vt, (kCreatePixelShader + 1) * sizeof(void *))) return false;
	if (vt[kPresent] != probeVt[kPresent]) return false;
	const int same = (vt[kQueryInterface] == probeVt[kQueryInterface]) + (vt[kAddRef] == probeVt[kAddRef]) + (vt[kReset] == probeVt[kReset]) +
	                 (vt[kCreateVertexShader] == probeVt[kCreateVertexShader]) + (vt[kCreatePixelShader] == probeVt[kCreatePixelShader]);
	return same >= 4;
}

// the engine's device, or nullptr; `how` says where it was found
void *FindEngineDevice(void **probeVt, std::string &how) {
	HMODULE mod = GetModuleHandleA("shaderapidx9.dll");
	if (!mod) return how = "no shaderapidx9.dll", nullptr;
	auto *dos = (IMAGE_DOS_HEADER *)mod;
	auto *nt = (IMAGE_NT_HEADERS *)((uint8_t *)mod + dos->e_lfanew);
	ReadableMap m;
	m.Build();
	std::vector<std::pair<uintptr_t, uintptr_t>> data;
	IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
	for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s)
		if (s->Characteristics & IMAGE_SCN_MEM_WRITE) data.push_back({(uintptr_t)mod + s->VirtualAddress, (uintptr_t)mod + s->VirtualAddress + s->Misc.VirtualSize});
	void *found = nullptr;
	int hits = 0;
	for (auto &d : data)
		for (uintptr_t a = d.first; a + sizeof(void *) <= d.second; a += sizeof(void *)) {
			const uintptr_t v = *(uintptr_t *)a;
			if (LooksLikeDevice(m, v, probeVt)) {
				if (!found) found = (void *)v, how = "shaderapidx9.dll+0x" + ToHex(a - (uintptr_t)mod);
				else if ((void *)v != found) ++hits;
			}
		}
	if (!found) {
		for (auto &d : data)
			for (uintptr_t a = d.first; a + sizeof(void *) <= d.second && !found; a += sizeof(void *)) {
				const uintptr_t o = *(uintptr_t *)a;
				if (!o || (o & 3) || !m.Ok(o, 64 * sizeof(void *))) continue;
				for (int k = 0; k < 64; ++k) {
					const uintptr_t v = ((uintptr_t *)o)[k];
					if (LooksLikeDevice(m, v, probeVt)) {
						found = (void *)v, how = "inside the object at shaderapidx9.dll+0x" + ToHex(a - (uintptr_t)mod) + ", field +" + std::to_string(k * 4);
						break;
					}
				}
			}
	}
	if (found && hits) how += " (" + std::to_string(hits) + " other device pointer(s) seen; the first one is used)";
	if (!found) how = "no pointer to a device in shaderapidx9.dll's data";
	return found;
}

bool InModuleOf(const void *addr, const char *name) {
	HMODULE m = nullptr;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m)) return false;
	return m == GetModuleHandleA(name);
}
#endif

bool Discover() {
	std::string lib;
	Direct3DCreate9T create = FindCreate9(lib);
	if (!create) {
		Printf("[bridge] d3d9: no %s in this process (the game is not running Direct3D 9 / -vulkan); the Direct3D 9 backend stays off\n", lib.c_str());
		return false;
	}
	ThrowWindow win;
	if (!win.Create()) {
		Printf("[bridge] d3d9: could not create the throw-away window\n");
		return false;
	}
	void *d3d = create(D3D_SDK_VERSION_);
	if (!d3d) {
		Printf("[bridge] d3d9: Direct3DCreate9 failed\n");
		win.Destroy();
		return false;
	}
	D3DPRESENT_PARAMETERS_ pp{};
	pp.BackBufferWidth = 64, pp.BackBufferHeight = 64;
	pp.BackBufferFormat = D3DFMT_X8R8G8B8_;
	pp.BackBufferCount = 1;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD_;
	pp.hDeviceWindow = win.handle;
	pp.Windowed = 1;
	void *dev = nullptr;
	const HRESULT_ hr = Method<CreateDeviceT>(d3d, kD3dCreateDevice)(d3d, D3DADAPTER_DEFAULT_, D3DDEVTYPE_HAL_, win.handle, D3DCREATE_SOFTWARE_VERTEXPROCESSING_, &pp, &dev);
	if (Failed(hr) || !dev) {
		Printf("[bridge] d3d9: throw-away CreateDevice failed (0x%08x)\n", (unsigned)hr);
		Release(d3d);
		win.Destroy();
		return false;
	}
	g_throwDev = dev;
	g_devVt = VTable(dev);
	void *swap = nullptr;
	if (!Failed(Method<HRESULT_(PCB_D3DAPI *)(void *, UINT_, void **)>(dev, 14)(dev, 0, &swap)) && swap) g_swapVt = VTable(swap);
	Printf("[bridge] d3d9: device vtable %p, swap chain vtable %p (%s)\n", (void *)g_devVt, (void *)g_swapVt, lib.c_str());
#ifdef _WIN32
	if (!InModuleOf(g_devVt, lib.c_str())) {
		// private per-device tables (Microsoft's d3d9): patch the engine device's own table, never the throw-away one (freed below)
		std::string how;
		void *eng = FindEngineDevice(g_devVt, how);
		if (!eng) {
			Printf("[bridge] d3d9: each device has its own function table here and the engine's device was not found (%s); the Direct3D 9 backend stays off\n", how.c_str());
			g_devVt = nullptr;
			if (swap) Release(swap);
			Release(dev), Release(d3d), win.Destroy();
			g_throwDev = nullptr;
			return false;
		}
		g_devVt = VTable(eng);
		Printf("[bridge] d3d9: each device has its own function table here; the engine's device %p (found at %s), its table %p\n", eng, how.c_str(), (void *)g_devVt);
	}
#endif

	bool ok = true;
	ok &= PatchSlot(g_devVt, kPresent, (void *)H_Present, (void **)&o_Present, "Present");
	ok &= PatchSlot(g_devVt, kReset, (void *)H_Reset, (void **)&o_Reset, "Reset");
	ok &= PatchSlot(g_devVt, kCreateVertexShader, (void *)H_CreateVS, (void **)&o_CreateVS, "CreateVertexShader");
	ok &= PatchSlot(g_devVt, kCreatePixelShader, (void *)H_CreatePS, (void **)&o_CreatePS, "CreatePixelShader");
	// (PresentEx / ResetEx are left alone: Portal 2 creates a plain IDirect3DDevice9, and on Microsoft's d3d9 the Ex slots do not exist in its table)
	if (g_swapVt) PatchSlot(g_swapVt, kSwapPresent, (void *)H_SwapPresent, (void **)&o_SwapPresent, "SwapChain::Present");
	if (!ok) Printf("[bridge] d3d9: a vtable patch failed\n");

	if (swap) Release(swap);
	Release(dev);  // the vtables outlive the object (they belong to the library's class)
	Release(d3d);
	win.Destroy();
	g_throwDev = nullptr;  // (the address may be reused by the engine's device)
	return ok;
}

}  // namespace

// ---- public API -------------------------------------------------------------------------------------------------------------------------
bool Init() {
	if (g_active.load()) return true;
	g_shutdown = 0;
	if (!Discover()) return false;
	g_active = 1;
	Printf("[bridge] d3d9: hooked %d slots (Present, Reset, CreateVertexShader, CreatePixelShader, swap chain Present)\n", g_nPatches);
	return true;
}

void Shutdown() {
	if (!g_active.load()) return;
	g_shutdown = 1;
	for (int i = g_nPatches - 1; i >= 0; --i) WriteSlot(g_patches[i].vt, g_patches[i].slot, g_patches[i].orig);
	g_nPatches = 0;
	for (int i = 0; i < 500 && g_inHook.load(); ++i) {  // a call that entered a hook before the table was restored
#ifdef _WIN32
		Sleep(1);
#else
		timespec t{0, 1000000};
		nanosleep(&t, nullptr);
#endif
	}
	for (int i = 0; i < 500 && g_encoders.load(); ++i) {
#ifdef _WIN32
		Sleep(10);
#else
		timespec t{0, 10000000};
		nanosleep(&t, nullptr);
#endif
	}
	g_active = 0;
	g_engineDev = nullptr;
	// the state block / shot surface belong to the engine's device; they are released by the device (or leaked with it at exit)
}

bool Active() { return g_active.load() != 0; }
void AddPresentListener(PresentFn fn) { std::lock_guard<std::mutex> lk(g_listMx); g_presentL.push_back(fn); }
void AddResetListener(ResetFn fn) { std::lock_guard<std::mutex> lk(g_listMx); g_resetL.push_back(fn); }
void AddShaderListener(ShaderFn fn) { std::lock_guard<std::mutex> lk(g_listMx); g_shaderL.push_back(fn); }
void SetBindListener(BindFn fn) { g_bindL = fn; }
void SetTextureListener(TextureFn fn) { g_texL = fn; }
void SetDrawListener(DrawFn fn) { g_drawL = fn; }
void SetConstListener(ConstFn fn) { g_constL = fn; }
void *EngineDevice() { return g_engineDev.load(); }
Counters &Stats() { return g_stats; }
std::string ThreadSummary() {
	std::string s;
	for (auto &t : g_threads)
		if (t.id.load()) s += (s.empty() ? "" : ", ") + std::to_string(t.id.load()) + ":" + std::to_string((unsigned long long)t.n.load());
	return s;
}
bool InOverlay() { return t_overlay > 0; }

void EnableBindHooks(bool on) {
	if (!g_active.load()) return;
	const int want = on ? 1 : 0;
	if (g_bindHooks.exchange(want) == want) return;
	if (on) {
		PatchSlot(g_devVt, kSetVertexShader, (void *)H_SetVS, (void **)&o_SetVS, "SetVertexShader");
		PatchSlot(g_devVt, kSetPixelShader, (void *)H_SetPS, (void **)&o_SetPS, "SetPixelShader");
		PatchSlot(g_devVt, kSetTexture, (void *)H_SetTexture, (void **)&o_SetTexture, "SetTexture");
		PatchSlot(g_devVt, kDrawPrimitive, (void *)H_DrawPrim, (void **)&o_DrawPrim, "DrawPrimitive");
		PatchSlot(g_devVt, kDrawIndexedPrimitive, (void *)H_DrawIdx, (void **)&o_DrawIdx, "DrawIndexedPrimitive");
		PatchSlot(g_devVt, kSetPixelShaderConstantF, (void *)H_SetPSConstF, (void **)&o_SetPSConstF, "SetPixelShaderConstantF");
		PatchSlot(g_devVt, kSetVertexShaderConstantF, (void *)H_SetVSConstF, (void **)&o_SetVSConstF, "SetVertexShaderConstantF");
	}
	// (switching them off again is left to Shutdown: the hooks pass straight through when no listener is set)
}

int ShaderModel(const uint32_t *code, bool *isPixel) {
	if (!code) return 0;
	const uint32_t hi = code[0] >> 16;
	if (isPixel) *isPixel = hi == 0xFFFF;
	if (hi != 0xFFFE && hi != 0xFFFF) return 0;
	return (int)(code[0] & 0xFFFF);
}
uint32_t ShaderTokens(const uint32_t *code) {
	uint32_t n = 1;  // the version token
	for (;;) {
		const uint32_t t = code[n];
		++n;
		if (t == 0x0000FFFF) return n;
		const uint32_t op = t & 0xFFFF;
		if (op == 0xFFFE) n += (t >> 16) & 0x7FFF;  // comment: length in dwords
		else n += (t >> 24) & 0xF;                  // instruction: parameter count
		if (n > (1u << 20)) return n;               // unterminated
	}
}

InternalScope::InternalScope() { ++t_overlay; }
InternalScope::~InternalScope() { --t_overlay; }

bool ReadBackBuffer(void *dev, std::vector<uint8_t> &rgba, int &w, int &h) {
	void *bb = nullptr;
	if (Failed(Method<HRESULT_(PCB_D3DAPI *)(void *, UINT_, UINT_, DWORD_, void **)>(dev, kGetBackBuffer)(dev, 0, 0, 0, &bb)) || !bb) return false;
	D3DSURFACE_DESC_ d{};
	Method<HRESULT_(PCB_D3DAPI *)(void *, D3DSURFACE_DESC_ *)>(bb, kSurfGetDesc)(bb, &d);
	void *src = bb;
	// a multisampled back buffer (the engine's anti-aliasing setting) cannot be read directly: resolve it into a plain render target first
	if (d.MultiSampleType != 0) {
		if (!g_resolve || g_shotW != (int)d.Width || g_shotH != (int)d.Height) {
			if (g_resolve) Release(g_resolve), g_resolve = nullptr;
			// (UINT w, UINT h, D3DFORMAT, D3DMULTISAMPLETYPE, DWORD quality, BOOL lockable, IDirect3DSurface9 **, HANDLE *)
			if (Failed(Method<HRESULT_(PCB_D3DAPI *)(void *, UINT_, UINT_, DWORD_, DWORD_, DWORD_, int32_t, void **, void *)>(dev, kCreateRenderTarget)(dev, d.Width, d.Height, d.Format, 0, 0, 0, &g_resolve, nullptr))) {
				g_resolve = nullptr;
				Release(bb);
				return false;
			}
		}
		if (Failed(Method<HRESULT_(PCB_D3DAPI *)(void *, void *, const RECT_ *, void *, const RECT_ *, DWORD_)>(dev, kStretchRect)(dev, bb, nullptr, g_resolve, nullptr, D3DTEXF_NONE_))) {
			Release(bb);
			return false;
		}
		src = g_resolve;
	}
	if (!g_shotSurf || g_shotW != (int)d.Width || g_shotH != (int)d.Height) {
		if (g_shotSurf) Release(g_shotSurf), g_shotSurf = nullptr;
		if (Failed(Method<HRESULT_(PCB_D3DAPI *)(void *, UINT_, UINT_, DWORD_, DWORD_, void **, void *)>(dev, kCreateOffscreenPlainSurface)(dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM_, &g_shotSurf, nullptr))) {
			g_shotSurf = nullptr;
			Release(bb);
			return false;
		}
	}
	g_shotW = (int)d.Width, g_shotH = (int)d.Height;
	const bool got = !Failed(Method<HRESULT_(PCB_D3DAPI *)(void *, void *, void *)>(dev, kGetRenderTargetData)(dev, src, g_shotSurf));
	Release(bb);
	if (!got) return false;
	D3DLOCKED_RECT_ lr{};
	if (Failed(Method<HRESULT_(PCB_D3DAPI *)(void *, D3DLOCKED_RECT_ *, const RECT_ *, DWORD_)>(g_shotSurf, kSurfLockRect)(g_shotSurf, &lr, nullptr, D3DLOCK_READONLY_))) return false;
	w = (int)d.Width, h = (int)d.Height;
	rgba.resize((size_t)w * h * 4);
	for (int y = 0; y < h; ++y) {
		const uint8_t *s = (const uint8_t *)lr.pBits + (size_t)y * lr.Pitch;
		uint8_t *o = &rgba[(size_t)y * w * 4];
		for (int x = 0; x < w; ++x) o[x * 4 + 0] = s[x * 4 + 2], o[x * 4 + 1] = s[x * 4 + 1], o[x * 4 + 2] = s[x * 4 + 0], o[x * 4 + 3] = 255;
	}
	Method<HRESULT_(PCB_D3DAPI *)(void *)>(g_shotSurf, kSurfUnlockRect)(g_shotSurf);
	return true;
}

bool BackBufferSize(void *dev, int &w, int &h) { return BackBufferSizeImpl(dev, w, h); }

void RequestInfo() { g_infoReq = 1; }
void RequestShot(const std::string &path) {
	{
		std::lock_guard<std::mutex> lk(g_shotMx);
		g_shotPath = path;
	}
	g_shotReq = 1;
}
void RequestProbe(int x, int y) {
	g_probeX = x, g_probeY = y, g_probeDone = 0, g_probeReq = 1;
}
bool ProbeResult(uint32_t &rgba) {
	rgba = g_probeRgba.load();
	return g_probeDone.load() != 0;
}

// ---- overlay scope -----------------------------------------------------------------------------------------------------------------------
OverlayScope::OverlayScope(void *dev, int w, int h) : dev_(dev) {
	using SetRS = HRESULT_(PCB_D3DAPI *)(void *, DWORD_, DWORD_);
	++t_overlay;
	if (!g_sb) {
		if (Failed(Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, void **)>(dev, kCreateStateBlock)(dev, D3DSBT_ALL_, &g_sb))) {
			g_sb = nullptr;
			g_stats.hrFail++;
			--t_overlay;
			return;
		}
	}
	Method<HRESULT_(PCB_D3DAPI *)(void *)>(g_sb, kSbCapture)(g_sb);
	// the render target: the game's back buffer (the engine may have another one bound at Present)
	Method<HRESULT_(PCB_D3DAPI *)(void *, UINT_, void **)>(dev, kGetRenderTarget)(dev, 0, &rt_);
	if (Failed(Method<HRESULT_(PCB_D3DAPI *)(void *, UINT_, UINT_, DWORD_, void **)>(dev, kGetBackBuffer)(dev, 0, 0, 0, &bb_)) || !bb_) {
		bb_ = nullptr;
	} else if (rt_ != bb_) {
		Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, void *)>(dev, kSetRenderTarget)(dev, 0, bb_);
	}
	D3DVIEWPORT9_ vp{0, 0, (DWORD_)w, (DWORD_)h, 0.0f, 1.0f};
	Method<HRESULT_(PCB_D3DAPI *)(void *, const D3DVIEWPORT9_ *)>(dev, kSetViewport)(dev, &vp);
	auto rs = Method<SetRS>(dev, kSetRenderState);
	rs(dev, D3DRS_ZENABLE_, 0);
	rs(dev, D3DRS_ZWRITEENABLE_, 0);
	rs(dev, D3DRS_CULLMODE_, D3DCULL_NONE_);
	rs(dev, D3DRS_FILLMODE_, D3DFILL_SOLID_);
	rs(dev, D3DRS_ALPHATESTENABLE_, 0);
	rs(dev, D3DRS_STENCILENABLE_, 0);
	rs(dev, D3DRS_FOGENABLE_, 0);
	rs(dev, D3DRS_LIGHTING_, 0);
	rs(dev, D3DRS_CLIPPING_, 1);
	rs(dev, D3DRS_SCISSORTESTENABLE_, 0);
	rs(dev, D3DRS_COLORWRITEENABLE_, 0xF);
	rs(dev, D3DRS_SRGBWRITEENABLE_, 0);
	rs(dev, D3DRS_SEPARATEALPHABLENDENABLE_, 0);
	rs(dev, D3DRS_BLENDOP_, D3DBLENDOP_ADD_);
	rs(dev, D3DRS_ALPHABLENDENABLE_, 1);
	rs(dev, D3DRS_SRCBLEND_, D3DBLEND_SRCALPHA_);
	rs(dev, D3DRS_DESTBLEND_, D3DBLEND_INVSRCALPHA_);
	{
		auto ts = Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, DWORD_, DWORD_)>(dev, kSetTextureStageState);  // stage 0 passes the vertex colour; stage 1 off
		ts(dev, 0, D3DTSS_COLOROP_, D3DTOP_SELECTARG1_), ts(dev, 0, D3DTSS_COLORARG1_, D3DTA_DIFFUSE_);
		ts(dev, 0, D3DTSS_ALPHAOP_, D3DTOP_SELECTARG1_), ts(dev, 0, D3DTSS_ALPHAARG1_, D3DTA_DIFFUSE_);
		ts(dev, 1, D3DTSS_COLOROP_, D3DTOP_DISABLE_), ts(dev, 1, D3DTSS_ALPHAOP_, D3DTOP_DISABLE_);
	}
	Method<HRESULT_(PCB_D3DAPI *)(void *, void *)>(dev, kSetVertexShader)(dev, nullptr);
	Method<HRESULT_(PCB_D3DAPI *)(void *, void *)>(dev, kSetPixelShader)(dev, nullptr);
	Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, void *)>(dev, kSetTexture)(dev, 0, nullptr);
	ok_ = true;
	g_stats.overlays++;
}

OverlayScope::~OverlayScope() {
	if (ok_) {
		if (bb_ && rt_ != bb_ && rt_) Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, void *)>(dev_, kSetRenderTarget)(dev_, 0, rt_);
		Method<HRESULT_(PCB_D3DAPI *)(void *)>(g_sb, kSbApply)(g_sb);
		--t_overlay;
	}
	if (rt_) Release(rt_);
	if (bb_) Release(bb_);
}

}  // namespace bridge::d3d9
