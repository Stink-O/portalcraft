// Portalcraft bridge: the Minecraft hand/HUD overlay, Direct3D 9 backend (PLAN-windows.md 2.4, PLAN-vulkan.md V2). Implements the rb::HudBackend*
// half of render_backend.hpp; the renderer-neutral half (render_backend/hud_logic.cpp) decides what to draw and when. Hooks Present through
// d3d9hook.cpp, so it runs the same on Microsoft's d3d9 (Windows) and on DXVK (Portal 2 -vulkan on Linux).
//
// The image: one default-pool dynamic A8R8G8B8 texture (the bytes arrive as R,G,B,A, so a ps_2_0 swaps red and blue), written with
// D3DLOCK_DISCARD. The draw: pre-transformed quads (no vertex shader) with premultiplied alpha, the same maths as the GL overlay's
// shader. Where the GL shader tests fragment positions (the crosshair rectangle where Minecraft's invert blend is drawn, the cursor arrow),
// this draws geometry: scissored strips around the rectangle for the normal pass, a scissored quad for the invert pass, two triangles for the arrow.
#include "bridge/render_backend.hpp"

#include "bridge/bridge.hpp"
#include "bridge/link.hpp"
#include "bridge/d3d9/d3d9hook.hpp"
#include "bridge/d3d9/sm_asm.hpp"
#include "bridge/platform/platform.hpp"

#include "Modules/Tier1.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bridge {
namespace rb {

using namespace bridge::d3d9;
using namespace skycraft::proto;

namespace {

void *g_tex = nullptr;  // IDirect3DTexture9, default pool, dynamic
int g_texW = 0, g_texH = 0;
void *g_psMain = nullptr, *g_psInvert = nullptr;
bool g_failed = false;
uint64_t g_retryAt = 0, g_calls = 0;
OverlayScope *g_scope = nullptr;
void *g_dev = nullptr;
int g_scopeW = 0, g_scopeH = 0;

struct Snap {
	bool have = false;
	D3DVIEWPORT9_ vp{};
	void *rt = nullptr, *vs = nullptr, *ps = nullptr, *tex0 = nullptr;
	DWORD_ rs[12] = {};
};
Snap g_snap;
const DWORD_ kSnapStates[12] = {D3DRS_ZENABLE_, D3DRS_ZWRITEENABLE_, D3DRS_ALPHABLENDENABLE_, D3DRS_SRCBLEND_, D3DRS_DESTBLEND_, D3DRS_CULLMODE_,
                                D3DRS_SCISSORTESTENABLE_, D3DRS_COLORWRITEENABLE_, D3DRS_STENCILENABLE_, D3DRS_ALPHATESTENABLE_, D3DRS_FOGENABLE_, D3DRS_SRGBWRITEENABLE_};

using SetRS = HRESULT_(PCB_D3DAPI *)(void *, DWORD_, DWORD_);
using GetRS = HRESULT_(PCB_D3DAPI *)(void *, DWORD_, DWORD_ *);

double NowMs() { return plat::MonotonicNs() * 1e-6; }

// ---- shaders ----------------------------------------------------------------------------------------------------------------------------------
// main:   out.rgb = tex.bgr * c0.x + c1.rgb * tex.a   (c0.x = 1 - fade, c1 = fade colour * fade), out.a = tex.a
// invert: out = (tex.bgr, 0)
// r[dst] = r[src].zyxw. ps_2_0 allows only a few fixed swizzles (any order needs the driver's arbitrary-swizzle cap, and Microsoft's d3d9
// rejects the shader with INVALIDCALL; DXVK accepts it), so the channels are copied one at a time with replicate swizzles.
void SwapRedBlue(sm::Builder &b, uint32_t dst, uint32_t src) {
	using namespace sm;
	b.Ins(OP_MOV, {Dst(R_TEMP, dst, kMaskX), Src(R_TEMP, src, kZZZZ)});
	b.Ins(OP_MOV, {Dst(R_TEMP, dst, kMaskY), Src(R_TEMP, src, kYYYY)});
	b.Ins(OP_MOV, {Dst(R_TEMP, dst, kMaskZ), Src(R_TEMP, src, kXXXX)});
	b.Ins(OP_MOV, {Dst(R_TEMP, dst, kMaskW), Src(R_TEMP, src, kWWWW)});
}
std::vector<uint32_t> MakePsMain() {
	using namespace sm;
	Builder b(kPs20);
	b.DclTex(0).DclSampler(0, TT_2D);
	b.Ins(OP_TEXLD, {Dst(R_TEMP, 2), Src(R_TEXTURE, 0), Src(R_SAMPLER, 0)});
	SwapRedBlue(b, 0, 2);                                                                           // bytes arrive R,G,B,A: A8R8G8B8 reads them as B,G,R,A
	b.Ins(OP_MUL, {Dst(R_TEMP, 1), Src(R_CONST, 1), Src(R_TEMP, 0, kWWWW)});                       // r1 = c1 * a
	b.Ins(OP_MAD, {Dst(R_TEMP, 0, kMaskXYZ), Src(R_TEMP, 0), Src(R_CONST, 0, kXXXX), Src(R_TEMP, 1)});
	b.Ins(OP_MOV, {Dst(R_COLOROUT, 0), Src(R_TEMP, 0)});
	b.End();
	return b.t;
}
std::vector<uint32_t> MakePsInvert() {
	using namespace sm;
	Builder b(kPs20);
	b.DclTex(0).DclSampler(0, TT_2D);
	b.Ins(OP_TEXLD, {Dst(R_TEMP, 2), Src(R_TEXTURE, 0), Src(R_SAMPLER, 0)});
	SwapRedBlue(b, 0, 2);
	b.Ins(OP_MOV, {Dst(R_TEMP, 0, kMaskW), Src(R_CONST, 0, kYYYY)});                               // c0.y = 0
	b.Ins(OP_MOV, {Dst(R_COLOROUT, 0), Src(R_TEMP, 0)});
	b.End();
	return b.t;
}

bool Create(void *dev) {
	InternalScope internal;
	auto createPs = Method<HRESULT_(PCB_D3DAPI *)(void *, const uint32_t *, void **)>(dev, kCreatePixelShader);
	const auto a = MakePsMain(), b = MakePsInvert();
	HRESULT_ hrA = createPs(dev, a.data(), &g_psMain), hrB = Failed(hrA) ? hrA : createPs(dev, b.data(), &g_psInvert);
	if (Failed(hrA) || Failed(hrB)) {
		console->Print("[bridge] d3d9 hud: could not create the overlay pixel shaders (main 0x%08x, invert 0x%08x)\n", (unsigned)hrA, (unsigned)hrB);
		g_psMain = g_psInvert = nullptr;
		return false;
	}
	console->Print("[bridge] d3d9 hud: ready (ps_2_0 main %p, invert %p)\n", g_psMain, g_psInvert);
	return true;
}

void OnReset(void *) {
	if (g_tex) Release(g_tex), g_tex = nullptr;
	g_texW = g_texH = 0;
}

void OnPresentListener(void *dev, int, int) {
	g_dev = dev;
	if (HudPresentBegin()) HudOnPresent(dev);
	HudPresentEnd();
}

struct Vtx { float x, y, z, rhw, u, v; };
struct VtxC { float x, y, z, rhw; uint32_t c; };

}  // namespace

// ---- install -----------------------------------------------------------------------------------------------------------------------------------
bool HudBackendInstall() {
	if (!d3d9::Init()) return false;
	static bool registered = false;
	if (!registered) {
		registered = true;
		AddPresentListener(OnPresentListener);
		AddResetListener(OnReset);
	}
	return true;
}
void HudBackendUninstall() { d3d9::Shutdown(); }

// ---- render thread ------------------------------------------------------------------------------------------------------------------------
bool HudBackendReady() {
	void *dev = EngineDevice();
	if (!dev) return false;
	if (g_psMain && g_psInvert) return true;
	if (g_failed && ++g_calls < g_retryAt) return false;
	g_failed = !Create(dev);
	g_retryAt = g_calls + 300;
	return !g_failed;
}

bool HudBackendWindowSize(void *window, int *w, int *h) {
	return BackBufferSize(window ? window : EngineDevice(), *w, *h);
}

void HudBackendBegin(HudStats &st) {
	(void)st;
	g_dev = EngineDevice();
	if (!g_dev) return;
	int w = 0, h = 0;
	if (!BackBufferSize(g_dev, w, h)) return;
	g_scopeW = w, g_scopeH = h;
	g_snap = Snap{};
	if (st.swaps % 64 == 1) {  // the cheap state check: a handful of Get calls before and after (pcb_assert_glhud counts mismatches)
		g_snap.have = true;
		Method<HRESULT_(PCB_D3DAPI *)(void *, D3DVIEWPORT9_ *)>(g_dev, kGetViewport)(g_dev, &g_snap.vp);
		Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, void **)>(g_dev, kGetRenderTarget)(g_dev, 0, &g_snap.rt);
		Method<HRESULT_(PCB_D3DAPI *)(void *, void **)>(g_dev, 93)(g_dev, &g_snap.vs);   // GetVertexShader
		Method<HRESULT_(PCB_D3DAPI *)(void *, void **)>(g_dev, 108)(g_dev, &g_snap.ps);  // GetPixelShader
		Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, void **)>(g_dev, 64)(g_dev, 0, &g_snap.tex0);  // GetTexture
		for (int i = 0; i < 12; ++i) Method<GetRS>(g_dev, 58)(g_dev, kSnapStates[i], &g_snap.rs[i]);
	}
	g_scope = new OverlayScope(g_dev, w, h);
	if (!g_scope->ok()) {
		delete g_scope;
		g_scope = nullptr;
	}
}

void HudBackendEnd(HudStats &st, bool verify) {
	if (g_scope) {
		delete g_scope;  // restores the engine's state (render target, then the state block)
		g_scope = nullptr;
	}
	if (verify && g_snap.have && g_dev) {
		const char *bad = nullptr;
		D3DVIEWPORT9_ vp{};
		void *rt = nullptr, *vs = nullptr, *ps = nullptr, *tex0 = nullptr;
		Method<HRESULT_(PCB_D3DAPI *)(void *, D3DVIEWPORT9_ *)>(g_dev, kGetViewport)(g_dev, &vp);
		Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, void **)>(g_dev, kGetRenderTarget)(g_dev, 0, &rt);
		Method<HRESULT_(PCB_D3DAPI *)(void *, void **)>(g_dev, 93)(g_dev, &vs);
		Method<HRESULT_(PCB_D3DAPI *)(void *, void **)>(g_dev, 108)(g_dev, &ps);
		Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, void **)>(g_dev, 64)(g_dev, 0, &tex0);
		if (std::memcmp(&vp, &g_snap.vp, sizeof vp)) bad = "viewport";
		else if (rt != g_snap.rt) bad = "render target 0";
		else if (vs != g_snap.vs) bad = "vertex shader";
		else if (ps != g_snap.ps) bad = "pixel shader";
		else if (tex0 != g_snap.tex0) bad = "texture 0";
		else
			for (int i = 0; i < 12 && !bad; ++i) {
				DWORD_ v = 0;
				Method<GetRS>(g_dev, 58)(g_dev, kSnapStates[i], &v);
				if (v != g_snap.rs[i]) bad = "render state";
			}
		if (bad) {
			++st.stateMismatch;
			std::snprintf(st.mismatch, sizeof st.mismatch, "%s", bad);
		}
		Release(rt), Release(vs), Release(ps), Release(tex0);
	}
	if (g_snap.have) Release(g_snap.rt), Release(g_snap.vs), Release(g_snap.ps), Release(g_snap.tex0);
	g_snap = Snap{};
}

void HudBackendUpload(const uint8_t *rgba, int w, int h, HudStats &st) {
	if (!g_dev || !g_scope) return;
	const double t0 = NowMs();
	if (!g_tex || g_texW != w || g_texH != h) {
		if (g_tex) Release(g_tex), g_tex = nullptr;
		InternalScope internal;
		// (UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *)
		if (Failed(Method<HRESULT_(PCB_D3DAPI *)(void *, UINT_, UINT_, UINT_, DWORD_, DWORD_, DWORD_, void **, void *)>(g_dev, kCreateTexture)(
		        g_dev, (UINT_)w, (UINT_)h, 1, D3DUSAGE_DYNAMIC_, D3DFMT_A8R8G8B8_, D3DPOOL_DEFAULT_, &g_tex, nullptr))) {
			g_tex = nullptr;
			g_texW = g_texH = 0;
			return;
		}
		g_texW = w, g_texH = h;
		st.texW = w, st.texH = h;
	}
	D3DLOCKED_RECT_ lr{};
	if (Failed(Method<HRESULT_(PCB_D3DAPI *)(void *, UINT_, D3DLOCKED_RECT_ *, const RECT_ *, DWORD_)>(g_tex, kTexLockRect)(g_tex, 0, &lr, nullptr, D3DLOCK_DISCARD_)) || !lr.pBits) return;
	if (lr.Pitch == w * 4) {
		std::memcpy(lr.pBits, rgba, (size_t)w * h * 4);
	} else {
		for (int y = 0; y < h; ++y) std::memcpy((uint8_t *)lr.pBits + (size_t)y * lr.Pitch, rgba + (size_t)y * w * 4, (size_t)w * 4);
	}
	Method<HRESULT_(PCB_D3DAPI *)(void *, UINT_)>(g_tex, kTexUnlockRect)(g_tex, 0);
	const double ms = NowMs() - t0;
	++st.uploads;
	st.upMs += ms;
	if (ms > st.upMax) st.upMax = ms;
}

void HudBackendDraw(const HudDraw &d) {
	if (!g_dev || !g_scope || !g_tex || !g_psMain) return;
	void *dev = g_dev;
	const int W = d.W, H = d.H;
	auto rs = Method<SetRS>(dev, kSetRenderState);
	auto ss = Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, DWORD_, DWORD_)>(dev, kSetSamplerState);
	auto setScissor = Method<HRESULT_(PCB_D3DAPI *)(void *, const RECT_ *)>(dev, kSetScissorRect);
	auto draw = Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, UINT_, const void *, UINT_)>(dev, kDrawPrimitiveUP);
	auto setPsC = Method<HRESULT_(PCB_D3DAPI *)(void *, UINT_, const float *, UINT_)>(dev, kSetPixelShaderConstantF);

	// the sampler: 1:1 with the screen -> nearest, else linear (bridge #15 of the GL overlay), clamped, no sRGB decode
	const int filt = (d.frameW == W && d.frameH == H) ? D3DTEXF_POINT_ : D3DTEXF_LINEAR_;
	ss(dev, 0, D3DSAMP_MINFILTER_, filt), ss(dev, 0, D3DSAMP_MAGFILTER_, filt), ss(dev, 0, D3DSAMP_MIPFILTER_, D3DTEXF_NONE_);
	ss(dev, 0, D3DSAMP_ADDRESSU_, D3DTADDRESS_CLAMP_), ss(dev, 0, D3DSAMP_ADDRESSV_, D3DTADDRESS_CLAMP_), ss(dev, 0, D3DSAMP_SRGBTEXTURE_, 0);
	Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, void *)>(dev, kSetTexture)(dev, 0, g_tex);
	Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_)>(dev, kSetFVF)(dev, D3DFVF_XYZRHW_ | D3DFVF_TEX1_);

	const float fa = d.testPattern ? 0.0f : d.fadeA;
	const float c0[4] = {1.0f - fa, 0.0f, 0.0f, 0.0f};
	const float c1[4] = {d.fadeR * fa, d.fadeG * fa, d.fadeB * fa, 0.0f};

	// the full-screen quad. Pixel centres sit on integer coordinates in D3D9, hence the -0.5. Top-down rows (flipY): v 0 at the top.
	const float v0 = d.flipY ? 0.0f : 1.0f, v1 = d.flipY ? 1.0f : 0.0f;
	const Vtx quad[4] = {{-0.5f, -0.5f, 0, 1, 0, v0}, {W - 0.5f, -0.5f, 0, 1, 1, v0}, {-0.5f, H - 0.5f, 0, 1, 0, v1}, {W - 0.5f, H - 0.5f, 0, 1, 1, v1}};

	const float sx = d.frameW > 0 ? (float)W / d.frameW : 1.0f, sy = d.frameH > 0 ? (float)H / d.frameH : 1.0f;
	// the crosshair rectangle (Minecraft's invert blend): around the screen centre, 15 GUI pixels + the attack indicator below (SkyCraft's rectangle)
	const bool invert = !d.testPattern && d.guiScale > 0 && d.camera == 0 && !(d.mcFlags & kMcScreenOpen);
	int ix0 = 0, iy0 = 0, ix1 = 0, iy1 = 0;
	if (invert) {
		const float g = d.guiScale * sx, cx = W * 0.5f, cy = H * 0.5f;
		auto first = [](float v) { return (int)std::ceil(v - 0.5f); };  // the first pixel whose centre is >= v
		ix0 = std::max(0, std::min(W, first(cx - 12 * g))), ix1 = std::max(0, std::min(W, first(cx + 12 * g)));
		iy0 = std::max(0, std::min(H, first(cy - 12 * g))), iy1 = std::max(0, std::min(H, first(cy + 28 * g)));
	}

	// pass 0: premultiplied alpha; the rectangle (when there is one) is left to the invert pass
	rs(dev, D3DRS_ALPHABLENDENABLE_, 1);
	rs(dev, D3DRS_SRCBLEND_, D3DBLEND_ONE_), rs(dev, D3DRS_DESTBLEND_, D3DBLEND_INVSRCALPHA_);
	rs(dev, D3DRS_SEPARATEALPHABLENDENABLE_, 1), rs(dev, D3DRS_SRCBLENDALPHA_, D3DBLEND_ONE_), rs(dev, D3DRS_DESTBLENDALPHA_, D3DBLEND_INVSRCALPHA_);
	Method<HRESULT_(PCB_D3DAPI *)(void *, void *)>(dev, kSetPixelShader)(dev, g_psMain);
	setPsC(dev, 0, c0, 1), setPsC(dev, 1, c1, 1);
	if (!invert) {
		draw(dev, D3DPT_TRIANGLESTRIP_, 2, quad, sizeof(Vtx));
	} else {
		rs(dev, D3DRS_SCISSORTESTENABLE_, 1);
		const RECT_ strips[4] = {{0, 0, W, iy0}, {0, iy1, W, H}, {0, iy0, ix0, iy1}, {ix1, iy0, W, iy1}};
		for (const RECT_ &r : strips) {
			if (r.right <= r.left || r.bottom <= r.top) continue;
			setScissor(dev, &r);
			draw(dev, D3DPT_TRIANGLESTRIP_, 2, quad, sizeof(Vtx));
		}
		rs(dev, D3DRS_SCISSORTESTENABLE_, 0);
	}

	// the arrow cursor (an MC screen is open): black outline triangle, white inside; both faded like the image
	if (d.cursorOn && !d.testPattern) {
		Method<HRESULT_(PCB_D3DAPI *)(void *, void *)>(dev, kSetPixelShader)(dev, nullptr);
		Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, void *)>(dev, kSetTexture)(dev, 0, nullptr);
		Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_)>(dev, kSetFVF)(dev, D3DFVF_XYZRHW_ | D3DFVF_DIFFUSE_);
		auto col = [&](float v, float fadeC) {  // premultiplied, alpha 1: v * (1 - fade) + fade colour * fade
			const float o = v * (1.0f - fa) + fadeC * fa;
			return (uint32_t)(std::max(0.0f, std::min(1.0f, o)) * 255.0f + 0.5f);
		};
		auto argb = [&](float v) { return 0xFF000000u | (col(v, d.fadeR) << 16) | (col(v, d.fadeG) << 8) | col(v, d.fadeB); };
		const float ox = d.cursorX * sx - 0.5f, oy = d.cursorY * sy - 0.5f;
		const VtxC outer[3] = {{ox, oy, 0, 1, argb(0)}, {ox, oy + 18, 0, 1, argb(0)}, {ox + 10.8f, oy + 18, 0, 1, argb(0)}};
		const VtxC inner[3] = {{ox + 1.5f, oy + 5, 0, 1, argb(1)}, {ox + 1.5f, oy + 16.5f, 0, 1, argb(1)}, {ox + 8.4f, oy + 16.5f, 0, 1, argb(1)}};
		draw(dev, D3DPT_TRIANGLELIST_, 1, outer, sizeof(VtxC));
		draw(dev, D3DPT_TRIANGLELIST_, 1, inner, sizeof(VtxC));
		Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_)>(dev, kSetFVF)(dev, D3DFVF_XYZRHW_ | D3DFVF_TEX1_);
		Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, void *)>(dev, kSetTexture)(dev, 0, g_tex);
	}

	// pass 1: Minecraft's BlendFunction.INVERT inside the rectangle; the back buffer's alpha is left alone
	if (invert && ix1 > ix0 && iy1 > iy0) {
		Method<HRESULT_(PCB_D3DAPI *)(void *, void *)>(dev, kSetPixelShader)(dev, g_psInvert);
		rs(dev, D3DRS_SRCBLEND_, D3DBLEND_INVDESTCOLOR_), rs(dev, D3DRS_DESTBLEND_, D3DBLEND_INVSRCCOLOR_);
		rs(dev, D3DRS_SRCBLENDALPHA_, D3DBLEND_ZERO_), rs(dev, D3DRS_DESTBLENDALPHA_, D3DBLEND_ONE_);
		rs(dev, D3DRS_SCISSORTESTENABLE_, 1);
		const RECT_ r = {ix0, iy0, ix1, iy1};
		setScissor(dev, &r);
		setPsC(dev, 0, c0, 1);
		draw(dev, D3DPT_TRIANGLESTRIP_, 2, quad, sizeof(Vtx));
		rs(dev, D3DRS_SCISSORTESTENABLE_, 0);
	}
}

bool HudBackendReadPixel(int, int, int x, int y, uint8_t rgba[4]) {
	std::vector<uint8_t> raw;
	int w = 0, h = 0;
	if (!ReadBackBuffer(g_dev ? g_dev : EngineDevice(), raw, w, h)) return false;
	x = std::max(0, std::min(w - 1, x)), y = std::max(0, std::min(h - 1, y));
	std::memcpy(rgba, &raw[((size_t)y * w + x) * 4], 4);
	return true;
}

bool HudBackendReadFrame(int W, int H, std::vector<uint8_t> &raw, bool &bottomUp) {
	int w = 0, h = 0;
	bottomUp = false;
	if (!ReadBackBuffer(g_dev ? g_dev : EngineDevice(), raw, w, h)) return false;
	(void)W, (void)H;
	return true;
}

bool HudBackendWantsFrame() { return false; }
void HudBackendAfterOverlay(int, int) {}

void HudBackendToast(int W, int H, const int (*r)[4], const uint32_t *c, int n) {
	(void)W, (void)H;
	if (!g_dev || !g_scope) return;
	auto clear = Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, const D3DRECT_ *, DWORD_, DWORD_, float, DWORD_)>(g_dev, kClear);
	for (int i = 0; i < n; ++i) {
		if (r[i][2] <= 0 || r[i][3] <= 0) continue;
		const D3DRECT_ rect = {r[i][0], r[i][1], r[i][0] + r[i][2], r[i][1] + r[i][3]};
		const uint32_t k = c[i];  // 0xAABBGGRR -> D3DCOLOR_XRGB
		clear(g_dev, 1, &rect, D3DCLEAR_TARGET_, 0xFF000000u | ((k & 255u) << 16) | (k & 0xFF00u) | ((k >> 16) & 255u), 1.0f, 0);
	}
}

}  // namespace rb
}  // namespace bridge
