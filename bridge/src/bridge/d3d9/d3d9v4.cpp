// Portalcraft bridge: PLAN-vulkan.md phase V4, the Direct3D 9 conventions the Minecraft composite relies on, measured on the device the engine
// runs on (DXVK under -vulkan; Microsoft's d3d9 on Windows), with the backend's own draws on the engine's back buffer (no engine content is
// changed in a way that outlives the frame: every test draws inside an OverlayScope, i.e. a state block, at the next Present).
//   pcb_d3d9_v4 alpha                          the 8-bit alpha test: 256 alpha levels x 256 references, GREATEREQUAL and GREATER, fixed function and
//                                              a pixel shader; every texel against its reference
//   pcb_d3d9_v4 half                           half-pixel conventions: coverage of pre-transformed quads, a 1:1 bilinear copy with and without the
//                                              -0.5 offset, and a clip-space quad through a vertex shader (what offset the rasterizer applies)
//   pcb_d3d9_v4 depth <zlo> <zhi> [levels] [tag]  the depth of every pixel of the frame between zlo and zhi: `levels` full-screen quads at
//                                              increasing depth with the depth test, each adding 1/255 where it passes (so a pixel's red = how many
//                                              levels the engine's depth reaches); prints the histogram of the pixels, PNG d3d9_v4depth_<tag>.png
//   pcb_d3d9_v4 upload                         cost of replacing a texture every frame: dynamic default-pool (DISCARD) and managed (LockRect), 512x512,
//                                              1080x900, 1920x1080, the loop's wall time with and without a final GPU sync
// All results are console/log lines "[bridge] d3d9 v4 ...".
#include "bridge/d3d9/d3d9hook.hpp"
#include "bridge/d3d9/sm_asm.hpp"

#include "bridge/bridge.hpp"
#include "Modules/Tier1.hpp"
#include "Utils/lodepng.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace bridge {

using namespace d3d9;

namespace {

enum : DWORD_ { kRsAlphaRef = 24, kRsAlphaFunc = 25, kCmpGreater = 5, kCmpGreaterEqual = 7, kCmpLessEqual = 4, kRsZFunc = 23, kFvfXyz = 0x002 };

struct Req {
	std::string what;
	double zlo = 0.999, zhi = 1.0;
	int levels = 200;
	std::string tag = "a";
};
std::atomic<int> g_req{0};
Req g_cfg;
bool g_registered = false;

struct V3 { float x, y, z, rhw; uint32_t c; };   // D3DFVF_XYZRHW | DIFFUSE
struct V4 { float x, y, z, rhw; float u, v; };               // XYZRHW | TEX1
struct VClip { float x, y, z; float u, v; };                 // XYZ | TEX1

using SetRS = HRESULT_(PCB_D3DAPI *)(void *, DWORD_, DWORD_);
double NowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void *MakeTex(void *dev, int w, int h, DWORD_ usage, DWORD_ pool, DWORD_ fmt = D3DFMT_A8R8G8B8_) {
	void *t = nullptr;
	if (Failed(Method<HRESULT_(PCB_D3DAPI *)(void *, UINT_, UINT_, UINT_, DWORD_, DWORD_, DWORD_, void **, void *)>(dev, kCreateTexture)(dev, (UINT_)w, (UINT_)h, 1, usage, fmt, pool, &t, nullptr))) return nullptr;
	return t;
}
// fills a texture (A8R8G8B8 as 0xAARRGGBB words), whatever the pitch
bool FillTex(void *tex, int w, int h, const uint32_t *px, DWORD_ flags) {
	D3DLOCKED_RECT_ lr{};
	if (Failed(Method<HRESULT_(PCB_D3DAPI *)(void *, UINT_, D3DLOCKED_RECT_ *, const RECT_ *, DWORD_)>(tex, kTexLockRect)(tex, 0, &lr, nullptr, flags)) || !lr.pBits) return false;
	for (int y = 0; y < h; ++y) std::memcpy((uint8_t *)lr.pBits + (size_t)y * lr.Pitch, px + (size_t)y * w, (size_t)w * 4);
	Method<HRESULT_(PCB_D3DAPI *)(void *, UINT_)>(tex, kTexUnlockRect)(tex, 0);
	return true;
}
void Clear(void *dev, int x0, int y0, int x1, int y1, uint32_t argb) {
	D3DRECT_ r{x0, y0, x1, y1};
	Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, const D3DRECT_ *, DWORD_, DWORD_, float, DWORD_)>(dev, kClear)(dev, 1, &r, D3DCLEAR_TARGET_, argb, 1.0f, 0);
}
void Sampler(void *dev, int filter) {
	auto ss = Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, DWORD_, DWORD_)>(dev, kSetSamplerState);
	ss(dev, 0, D3DSAMP_MINFILTER_, filter), ss(dev, 0, D3DSAMP_MAGFILTER_, filter), ss(dev, 0, D3DSAMP_MIPFILTER_, D3DTEXF_NONE_);
	ss(dev, 0, D3DSAMP_ADDRESSU_, D3DTADDRESS_CLAMP_), ss(dev, 0, D3DSAMP_ADDRESSV_, D3DTADDRESS_CLAMP_), ss(dev, 0, D3DSAMP_SRGBTEXTURE_, 0);
}
void FixedTexture(void *dev) {  // stage 0: colour and alpha come from the texture
	auto ts = Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, DWORD_, DWORD_)>(dev, kSetTextureStageState);
	ts(dev, 0, D3DTSS_COLOROP_, D3DTOP_SELECTARG1_), ts(dev, 0, D3DTSS_COLORARG1_, D3DTA_TEXTURE_);
	ts(dev, 0, D3DTSS_ALPHAOP_, D3DTOP_SELECTARG1_), ts(dev, 0, D3DTSS_ALPHAARG1_, D3DTA_TEXTURE_);
}
std::vector<uint32_t> PsCopy() {  // ps_2_0: oC0 = tex(t0)
	using namespace sm;
	Builder b(kPs20);
	b.DclTex(0).DclSampler(0, TT_2D);
	b.Ins(OP_TEXLD, {Dst(R_TEMP, 0), Src(R_TEXTURE, 0), Src(R_SAMPLER, 0)});
	b.Ins(OP_MOV, {Dst(R_COLOROUT, 0), Src(R_TEMP, 0)});
	b.End();
	return b.t;
}
std::vector<uint32_t> VsPass() {  // vs_2_0: oPos = v0; oT0 = v1
	using namespace sm;
	Builder b(kVs20);
	b.Ins(OP_DCL, {Usage_(U_POSITION, 0), Dst(R_INPUT, 0)});
	b.Ins(OP_DCL, {Usage_(U_TEXCOORD, 0), Dst(R_INPUT, 1)});
	b.Ins(OP_MOV, {Dst(R_RASTOUT, 0), Src(R_INPUT, 0)});
	b.Ins(OP_MOV, {Dst(R_OUTPUT, 0), Src(R_INPUT, 1)});   // (oT0)
	b.End();
	return b.t;
}

// pixel (x, y) of the read back frame
inline const uint8_t *Px(const std::vector<uint8_t> &f, int w, int x, int y) { return &f[((size_t)y * w + x) * 4]; }

// ---- alpha -------------------------------------------------------------------------------------------------------------------------------------
void TestAlpha(void *dev, int W, int H) {
	const int OX = 64, OY = 64;
	void *tex = MakeTex(dev, 256, 1, D3DUSAGE_DYNAMIC_, D3DPOOL_DEFAULT_);
	void *ps = nullptr;
	const auto code = PsCopy();
	if (!tex || Failed(Method<HRESULT_(PCB_D3DAPI *)(void *, const uint32_t *, void **)>(dev, kCreatePixelShader)(dev, code.data(), &ps)) || !ps) {
		Printf("[bridge] d3d9 v4 alpha: could not create the test texture / shader\n");
		if (tex) Release(tex);
		return;
	}
	uint32_t px[256];
	for (int i = 0; i < 256; ++i) px[i] = ((uint32_t)i << 24) | 0x00FFFFFFu;   // white, alpha = i
	FillTex(tex, 256, 1, px, D3DLOCK_DISCARD_);
	long total = 0;
	for (int pass = 0; pass < 4; ++pass) {
		const bool useShader = pass >= 2;
		const DWORD_ func = (pass & 1) ? kCmpGreater : kCmpGreaterEqual;
		long bad = 0, tested = 0;
		int first[2] = {-1, -1};
		{
			OverlayScope scope(dev, W, H);
			if (!scope.ok()) return;
			auto rs = Method<SetRS>(dev, kSetRenderState);
			Clear(dev, OX, OY, OX + 256, OY + 256, 0xFF000000u);
			rs(dev, D3DRS_ALPHABLENDENABLE_, 0);
			rs(dev, D3DRS_ALPHATESTENABLE_, 1);
			rs(dev, kRsAlphaFunc, func);
			Sampler(dev, D3DTEXF_POINT_);
			FixedTexture(dev);
			Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, void *)>(dev, kSetTexture)(dev, 0, tex);
			Method<HRESULT_(PCB_D3DAPI *)(void *, void *)>(dev, kSetPixelShader)(dev, useShader ? ps : nullptr);
			Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_)>(dev, kSetFVF)(dev, D3DFVF_XYZRHW_ | D3DFVF_TEX1_);
			auto draw = Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, UINT_, const void *, UINT_)>(dev, kDrawPrimitiveUP);
			for (int ref = 0; ref < 256; ++ref) {
				rs(dev, kRsAlphaRef, (DWORD_)ref);
				const float x0 = OX - 0.5f, x1 = OX + 256 - 0.5f, y0 = OY + ref - 0.5f, y1 = OY + ref + 0.5f;
				const V4 q[4] = {{x0, y0, 0, 1, 0, 0}, {x1, y0, 0, 1, 1, 0}, {x0, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};
				draw(dev, D3DPT_TRIANGLESTRIP_, 2, q, sizeof(V4));
			}
			rs(dev, D3DRS_ALPHATESTENABLE_, 0);
			Method<HRESULT_(PCB_D3DAPI *)(void *, void *)>(dev, kSetPixelShader)(dev, nullptr);
			std::vector<uint8_t> f;
			int fw = 0, fh = 0;
			if (!ReadBackBuffer(dev, f, fw, fh)) {
				Printf("[bridge] d3d9 v4 alpha: readback failed\n");
				break;
			}
			for (int ref = 0; ref < 256; ++ref)
				for (int a = 0; a < 256; ++a) {
					const bool want = func == kCmpGreater ? a > ref : a >= ref;
					const uint8_t *p = Px(f, fw, OX + a, OY + ref);
					const bool got = p[0] > 128;
					++tested;
					if (got != want && ++bad == 1) first[0] = a, first[1] = ref;
				}
		}
		total += bad;
		Printf("[bridge] d3d9 v4 alpha: %s, %s: %ld of %ld (alpha, reference) pairs wrong%s", useShader ? "ps_2_0" : "fixed function", func == kCmpGreater ? "GREATER" : "GREATEREQUAL", bad, tested, bad ? "; first alpha " : "");
		if (bad) Printf("%d ref %d", first[0], first[1]);
		Printf("\n");
	}
	Printf("[bridge] d3d9 v4 alpha: %s (%ld wrong in all)\n", total == 0 ? "EXACT: every 8-bit alpha against every 8-bit reference passes or fails as the D3D9 rules say" : "NOT exact", total);
	Release(ps);
	Release(tex);
}

// ---- half pixel ------------------------------------------------------------------------------------------------------------------------------
void TestHalf(void *dev, int W, int H) {
	const int N = 64;
	void *tex = MakeTex(dev, N, N, D3DUSAGE_DYNAMIC_, D3DPOOL_DEFAULT_);
	void *ps = nullptr, *vs = nullptr;
	const auto pc = PsCopy(), vc = VsPass();
	if (!tex || Failed(Method<HRESULT_(PCB_D3DAPI *)(void *, const uint32_t *, void **)>(dev, kCreatePixelShader)(dev, pc.data(), &ps)) ||
	    Failed(Method<HRESULT_(PCB_D3DAPI *)(void *, const uint32_t *, void **)>(dev, kCreateVertexShader)(dev, vc.data(), &vs))) {
		Printf("[bridge] d3d9 v4 half: could not create the test resources\n");
		return;
	}
	std::vector<uint32_t> px((size_t)N * N);
	for (int j = 0; j < N; ++j)
		for (int i = 0; i < N; ++i) px[(size_t)j * N + i] = 0xFF000000u | ((uint32_t)(i * 4) << 16) | ((uint32_t)(j * 4) << 8) | 0x80u;   // R = 4 i, G = 4 j
	FillTex(tex, N, N, px.data(), D3DLOCK_DISCARD_);
	{
		OverlayScope scope(dev, W, H);
		if (!scope.ok()) return;
		auto rs = Method<SetRS>(dev, kSetRenderState);
		auto draw = Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, UINT_, const void *, UINT_)>(dev, kDrawPrimitiveUP);
		auto setTex = Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, void *)>(dev, kSetTexture);
		rs(dev, D3DRS_ALPHABLENDENABLE_, 0);
		FixedTexture(dev);
		Clear(dev, 0, 0, 640, 300, 0xFF000000u);
		// (1) coverage of solid quads, pre-transformed: (10,10)-(20,20) and (9.5,9.5)-(19.5,19.5)
		setTex(dev, 0, nullptr);
		Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_)>(dev, kSetFVF)(dev, D3DFVF_XYZRHW_ | D3DFVF_DIFFUSE_);
		auto quad = [&](float x0, float y0, float x1, float y1, uint32_t c) {
			const V3 q[4] = {{x0, y0, 0, 1, c}, {x1, y0, 0, 1, c}, {x0, y1, 0, 1, c}, {x1, y1, 0, 1, c}};
			draw(dev, D3DPT_TRIANGLESTRIP_, 2, q, sizeof(V3));
		};
		quad(10, 10, 20, 20, 0xFFFFFFFFu);
		quad(109.5f, 9.5f, 119.5f, 19.5f, 0xFFFFFFFFu);
		quad(209.25f, 9.25f, 219.25f, 19.25f, 0xFFFFFFFFu);   // a quad whose corners sit a quarter of a pixel off the pixel centres' integer grid
		// (2) 1:1 bilinear copies of an N x N texture
		Sampler(dev, D3DTEXF_LINEAR_);
		setTex(dev, 0, tex);
		Method<HRESULT_(PCB_D3DAPI *)(void *, void *)>(dev, kSetPixelShader)(dev, nullptr);
		Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_)>(dev, kSetFVF)(dev, D3DFVF_XYZRHW_ | D3DFVF_TEX1_);
		auto copy = [&](float ox, float oy, float off) {
			const float x0 = ox + off, y0 = oy + off, x1 = ox + N + off, y1 = oy + N + off;
			const V4 q[4] = {{x0, y0, 0, 1, 0, 0}, {x1, y0, 0, 1, 1, 0}, {x0, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};
			draw(dev, D3DPT_TRIANGLESTRIP_, 2, q, sizeof(V4));
		};
		copy(100, 100, -0.5f);   // pre-transformed with the D3D9 offset
		copy(200, 100, 0.0f);    // pre-transformed without it
		// (3) the same through a vertex shader (clip space), viewport N x N at (300, 100): no offset applied by the program
		Method<HRESULT_(PCB_D3DAPI *)(void *, void *)>(dev, kSetVertexShader)(dev, vs);
		Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_)>(dev, kSetFVF)(dev, kFvfXyz | D3DFVF_TEX1_);
		D3DVIEWPORT9_ vp{300, 100, (DWORD_)N, (DWORD_)N, 0.0f, 1.0f};
		Method<HRESULT_(PCB_D3DAPI *)(void *, const D3DVIEWPORT9_ *)>(dev, kSetViewport)(dev, &vp);
		const VClip cq[4] = {{-1, 1, 0.5f, 0, 0}, {1, 1, 0.5f, 1, 0}, {-1, -1, 0.5f, 0, 1}, {1, -1, 0.5f, 1, 1}};
		draw(dev, D3DPT_TRIANGLESTRIP_, 2, cq, sizeof(VClip));
		Method<HRESULT_(PCB_D3DAPI *)(void *, void *)>(dev, kSetVertexShader)(dev, nullptr);
		// (4) clip-space with a ps: same as (3) with the pixel shader (the usual path of the engine)
		D3DVIEWPORT9_ vp2{400, 100, (DWORD_)N, (DWORD_)N, 0.0f, 1.0f};
		Method<HRESULT_(PCB_D3DAPI *)(void *, const D3DVIEWPORT9_ *)>(dev, kSetViewport)(dev, &vp2);
		Method<HRESULT_(PCB_D3DAPI *)(void *, void *)>(dev, kSetVertexShader)(dev, vs);
		Method<HRESULT_(PCB_D3DAPI *)(void *, void *)>(dev, kSetPixelShader)(dev, ps);
		draw(dev, D3DPT_TRIANGLESTRIP_, 2, cq, sizeof(VClip));
		Method<HRESULT_(PCB_D3DAPI *)(void *, void *)>(dev, kSetVertexShader)(dev, nullptr);
		Method<HRESULT_(PCB_D3DAPI *)(void *, void *)>(dev, kSetPixelShader)(dev, nullptr);
		std::vector<uint8_t> f;
		int fw = 0, fh = 0;
		if (!ReadBackBuffer(dev, f, fw, fh)) {
			Printf("[bridge] d3d9 v4 half: readback failed\n");
		} else {
			auto cover = [&](int ox, int oy, const char *name) {
				int x0 = 1 << 20, x1 = -1, y0 = 1 << 20, y1 = -1;
				long n = 0;
				for (int y = oy - 4; y < oy + 20; ++y)
					for (int x = ox - 4; x < ox + 20; ++x)
						if (Px(f, fw, x, y)[0] > 128) x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y), ++n;
				Printf("[bridge] d3d9 v4 half: %s covers x %d..%d, y %d..%d (%ld pixels); D3D9 rule: pixel centres on integer coordinates, left/top edge inclusive\n", name, x0, x1, y0, y1, n);
			};
			cover(10, 10, "solid quad (10,10)-(20,20), pre-transformed");
			cover(110, 10, "solid quad (9.5,9.5)-(19.5,19.5) [x+100]");
			cover(210, 10, "solid quad (9.25,9.25)-(19.25,19.25) [x+200]");
			auto shift = [&](int ox, int oy, const char *name, const char *rule) {
				// mean of R - 4 i and G - 4 j over the interior, in texels (R = 4 i: a bilinear sample half a texel to the left reads 4 i - 2)
				double sr = 0, sg = 0;
				long n = 0;
				int rmin = 255, rmax = 0;
				for (int j = 2; j < N - 2; ++j)
					for (int i = 2; i < N - 2; ++i) {
						const uint8_t *p = Px(f, fw, ox + i, oy + j);
						sr += (p[0] - 4.0 * i) / 4.0, sg += (p[1] - 4.0 * j) / 4.0, ++n;
						rmin = std::min(rmin, (int)p[0] - 4 * i), rmax = std::max(rmax, (int)p[0] - 4 * i);
					}
				Printf("[bridge] d3d9 v4 half: %s: image shifted by %+.3f texel in x, %+.3f in y (R-4i range %d..%d); %s\n", name, sr / n, sg / n, rmin, rmax, rule);
			};
			shift(100, 100, "pre-transformed 1:1 bilinear copy, vertices at -0.5", "D3D9 rule: exact copy, shift 0");
			shift(200, 100, "pre-transformed 1:1 bilinear copy, vertices at 0", "D3D9 rule: shift -0.5 (a blur)");
			shift(300, 100, "clip-space quad through a vertex shader, no offset in the program", "D3D9 rule: shift -0.5 (the classic half-pixel blur)");
			shift(400, 100, "same with a pixel shader", "D3D9 rule: shift -0.5");
		}
	}
	Release(ps), Release(vs), Release(tex);
}

// ---- depth ---------------------------------------------------------------------------------------------------------------------------------------
void TestDepth(void *dev, int W, int H, const Req &rq) {
	const int K = std::max(2, std::min(250, rq.levels));
	const double step = (rq.zhi - rq.zlo) / K;
	std::vector<uint8_t> f;
	int fw = 0, fh = 0;
	{
		OverlayScope scope(dev, W, H);
		if (!scope.ok()) return;
		auto rs = Method<SetRS>(dev, kSetRenderState);
		auto draw = Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, UINT_, const void *, UINT_)>(dev, kDrawPrimitiveUP);
		Clear(dev, 0, 0, W, H, 0xFF000000u);
		Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, void *)>(dev, kSetTexture)(dev, 0, nullptr);
		Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_)>(dev, kSetFVF)(dev, D3DFVF_XYZRHW_ | D3DFVF_DIFFUSE_);
		rs(dev, D3DRS_ZENABLE_, 1), rs(dev, D3DRS_ZWRITEENABLE_, 0), rs(dev, kRsZFunc, kCmpLessEqual);
		rs(dev, D3DRS_ALPHABLENDENABLE_, 1), rs(dev, D3DRS_SRCBLEND_, D3DBLEND_ONE_), rs(dev, D3DRS_DESTBLEND_, D3DBLEND_ONE_);
		const uint32_t c = 0x00010101u;
		for (int k = 1; k <= K; ++k) {
			const float z = (float)(rq.zlo + k * step);
			const V3 q[4] = {{-0.5f, -0.5f, z, 1, c}, {W - 0.5f, -0.5f, z, 1, c}, {-0.5f, H - 0.5f, z, 1, c}, {W - 0.5f, H - 0.5f, z, 1, c}};
			draw(dev, D3DPT_TRIANGLESTRIP_, 2, q, sizeof(V3));
		}
		rs(dev, D3DRS_ZENABLE_, 0);
		if (!ReadBackBuffer(dev, f, fw, fh)) {
			Printf("[bridge] d3d9 v4 depth: readback failed\n");
			return;
		}
	}
	// count c (red) = number of levels k with zlo + k*step <= depth: depth in [zlo + c*step, zlo + (c+1)*step)
	std::map<int, long> hist;
	std::vector<uint8_t> img((size_t)fw * fh * 4, 255);
	for (int y = 0; y < fh; ++y)
		for (int x = 0; x < fw; ++x) {
			const int c = Px(f, fw, x, y)[0];
			++hist[c];
			uint8_t *o = &img[((size_t)y * fw + x) * 4];
			o[0] = o[1] = o[2] = (uint8_t)(c * 255 / K);
			if (c == 0) o[0] = 60, o[1] = 0, o[2] = 0;
			else if (c >= K) o[0] = 0, o[1] = 0, o[2] = 160;
		}
	lodepng::encode(CacheDir() + "/d3d9_v4depth_" + rq.tag + ".png", img, (unsigned)fw, (unsigned)fh);
	Printf("[bridge] d3d9 v4 depth %s: frame %d x %d, z in [%.7f, %.7f] in %d levels of %.2e: pixels below the range %ld, at or above it %ld\n", rq.tag.c_str(), fw, fh, rq.zlo, rq.zhi, K, step,
	       hist.count(0) ? hist[0] : 0L, hist.count(K) ? hist[K] : 0L);
	std::vector<std::pair<long, int>> top;
	for (auto &kv : hist)
		if (kv.first > 0 && kv.first < K) top.push_back({kv.second, kv.first});
	std::sort(top.rbegin(), top.rend());
	for (size_t i = 0; i < top.size() && i < 14; ++i)
		Printf("[bridge] d3d9 v4 depth %s: %ld pixels at depth %.7f .. %.7f\n", rq.tag.c_str(), top[i].first, rq.zlo + top[i].second * step, rq.zlo + (top[i].second + 1) * step);
	int lo = K, hi = 0;
	for (auto &kv : hist)
		if (kv.first > 0 && kv.first < K) lo = std::min(lo, kv.first), hi = std::max(hi, kv.first);
	if (hi >= lo) Printf("[bridge] d3d9 v4 depth %s: depths inside the range run from %.7f to %.7f\n", rq.tag.c_str(), rq.zlo + lo * step, rq.zlo + (hi + 1) * step);
}

// ---- upload cost ------------------------------------------------------------------------------------------------------------------------------
void TestUpload(void *dev, int W, int H) {
	struct Sz { int w, h; } sizes[3] = {{512, 512}, {1080, 900}, {1920, 1080}};
	for (int mode = 0; mode < 2; ++mode)
		for (const Sz &s : sizes) {
			void *tex = mode == 0 ? MakeTex(dev, s.w, s.h, D3DUSAGE_DYNAMIC_, D3DPOOL_DEFAULT_) : MakeTex(dev, s.w, s.h, 0, D3DPOOL_MANAGED_);
			if (!tex) {
				Printf("[bridge] d3d9 v4 upload: %s %dx%d: texture not created\n", mode == 0 ? "dynamic default-pool" : "managed", s.w, s.h);
				continue;
			}
			std::vector<uint32_t> px((size_t)s.w * s.h);
			for (size_t i = 0; i < px.size(); ++i) px[i] = 0xFF000000u | (uint32_t)(i * 2654435761u);
			OverlayScope scope(dev, W, H);
			if (!scope.ok()) {
				Release(tex);
				return;
			}
			auto rs = Method<SetRS>(dev, kSetRenderState);
			auto draw = Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, UINT_, const void *, UINT_)>(dev, kDrawPrimitiveUP);
			rs(dev, D3DRS_ALPHABLENDENABLE_, 0);
			Sampler(dev, D3DTEXF_POINT_);
			FixedTexture(dev);
			Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, void *)>(dev, kSetTexture)(dev, 0, tex);
			Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_)>(dev, kSetFVF)(dev, D3DFVF_XYZRHW_ | D3DFVF_TEX1_);
			const V4 q[4] = {{0, 0, 0, 1, 0, 0}, {64, 0, 0, 1, 1, 0}, {0, 64, 0, 1, 0, 1}, {64, 64, 0, 1, 1, 1}};
			const int iters = 40;
			FillTex(tex, s.w, s.h, px.data(), mode == 0 ? D3DLOCK_DISCARD_ : 0);
			draw(dev, D3DPT_TRIANGLESTRIP_, 2, q, sizeof(V4));   // (warm up)
			double lockMs = 0;
			const double t0 = NowMs();
			for (int it = 0; it < iters; ++it) {
				px[0] ^= (uint32_t)it;
				const double a = NowMs();
				FillTex(tex, s.w, s.h, px.data(), mode == 0 ? D3DLOCK_DISCARD_ : 0);
				lockMs += NowMs() - a;
				draw(dev, D3DPT_TRIANGLESTRIP_, 2, q, sizeof(V4));
			}
			const double t1 = NowMs();
			std::vector<uint8_t> f;
			int fw = 0, fh = 0;
			ReadBackBuffer(dev, f, fw, fh);   // (waits for the GPU)
			const double t2 = NowMs();
			Printf("[bridge] d3d9 v4 upload: %s %dx%d (%.1f MB): lock+copy+unlock %.2f ms, loop with a draw %.2f ms per frame, with the GPU caught up %.2f ms per frame\n",
			       mode == 0 ? "dynamic default-pool DISCARD" : "managed LockRect", s.w, s.h, s.w * s.h * 4 / 1048576.0, lockMs / iters, (t1 - t0) / iters, (t2 - t0) / iters);
			Release(tex);
		}
}

void Run(void *dev, int W, int H) {
	if (!g_req.exchange(0)) return;
	const Req rq = g_cfg;
	if (rq.what == "alpha") TestAlpha(dev, W, H);
	else if (rq.what == "half") TestHalf(dev, W, H);
	else if (rq.what == "depth") TestDepth(dev, W, H, rq);
	else if (rq.what == "upload") TestUpload(dev, W, H);
}

}  // namespace

CON_COMMAND(pcb_d3d9_v4, "pcb_d3d9_v4 alpha | half | upload | depth <zlo> <zhi> [levels=200] [tag] - (tests) PLAN-vulkan V4: D3D9 alpha-test exactness, half-pixel conventions, upload cost, depth of the frame's pixels, measured at the next Present on the engine's device\n") {
	if (args.ArgC() < 2) return;
	if (!d3d9::Active() && !d3d9::Init()) return;
	if (!g_registered) {
		g_registered = true;
		d3d9::AddPresentListener(Run);
	}
	Req r;
	r.what = args[1];
	if (args.ArgC() > 2) r.zlo = std::atof(args[2]);
	if (args.ArgC() > 3) r.zhi = std::atof(args[3]);
	if (args.ArgC() > 4) r.levels = std::atoi(args[4]);
	if (args.ArgC() > 5) r.tag = args[5];
	g_cfg = r;
	g_req = 1;
}

}  // namespace bridge
