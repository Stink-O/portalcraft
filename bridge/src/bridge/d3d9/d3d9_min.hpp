// Portalcraft bridge: the small part of Direct3D 9 the D3D9 backend needs, declared here so the build needs no DirectX or MinGW headers
// (the same file builds with g++ -m32 on Linux, against DXVK's native Direct3DCreate9, and with MinGW-w64 on Windows).
// Direct3D 9 is a documented COM interface: every device method is a slot in a table of function pointers whose order is fixed by
// the public d3d9.h. The slot numbers below follow that header; nothing in this file comes from Portal 2.
#pragma once

#include <cstdint>
#include <cstddef>

namespace bridge::d3d9 {

#if defined(_WIN32)
#define PCB_D3DAPI __stdcall  // COM methods of Microsoft's d3d9 (and of DXVK built for Windows) are stdcall
#else
#define PCB_D3DAPI            // Valve's native DXVK build on Linux declares them without a calling convention: plain cdecl (verified on
                              // libdxvk_d3d9.so: its COM methods end in a plain "ret", not "ret N")
#endif

using HRESULT_ = int32_t;
inline bool Failed(HRESULT_ hr) { return hr < 0; }
using DWORD_ = uint32_t;
using UINT_ = uint32_t;

struct RECT_ { int32_t left, top, right, bottom; };
struct D3DRECT_ { int32_t x1, y1, x2, y2; };

// ---- slot numbers: IUnknown / IDirect3D9 / IDirect3DDevice9(Ex) / IDirect3DSwapChain9 / surfaces / textures --------------------------
enum DevSlot : int {
	kQueryInterface = 0, kAddRef = 1, kRelease = 2,
	kTestCooperativeLevel = 3, kGetCreationParameters = 9,
	kReset = 16, kPresent = 17, kGetBackBuffer = 18,
	kCreateTexture = 23, kCreateVolumeTexture = 24, kCreateRenderTarget = 28,
	kUpdateSurface = 30, kUpdateTexture = 31, kGetRenderTargetData = 32, kStretchRect = 34, kColorFill = 35, kCreateOffscreenPlainSurface = 36,
	kSetRenderTarget = 37, kGetRenderTarget = 38,
	kBeginScene = 41, kEndScene = 42, kClear = 43, kSetViewport = 47, kGetViewport = 48,
	kSetRenderState = 57, kCreateStateBlock = 59,
	kGetTexture = 64, kSetTexture = 65, kSetTextureStageState = 67, kGetSamplerState = 68, kSetSamplerState = 69,
	kSetScissorRect = 75,
	kDrawPrimitive = 81, kDrawIndexedPrimitive = 82, kDrawPrimitiveUP = 83,
	kSetVertexDeclaration = 87, kSetFVF = 89,
	kCreateVertexShader = 91, kSetVertexShader = 92, kGetVertexShader = 93, kSetVertexShaderConstantF = 94, kSetVertexShaderConstantB = 98,
	kSetStreamSource = 100,
	kCreatePixelShader = 106, kSetPixelShader = 107, kGetPixelShader = 108, kSetPixelShaderConstantF = 109, kSetPixelShaderConstantB = 113,
	kPresentEx = 121, kResetEx = 132,
};
enum D3dSlot : int { kD3dCreateDevice = 16 };
enum SwapSlot : int { kSwapPresent = 3 };
enum SurfSlot : int { kSurfGetDesc = 12, kSurfLockRect = 13, kSurfUnlockRect = 14 };
enum TexSlot : int { kTexGetLevelDesc = 17, kTexGetSurfaceLevel = 18, kTexLockRect = 19, kTexUnlockRect = 20 };
enum ShaderSlot : int { kShaderGetFunction = 4 };
enum VolSlot : int { kVolLockBox = 19, kVolUnlockBox = 20 };
enum StateBlockSlot : int { kSbCapture = 4, kSbApply = 5 };

// ---- constants ------------------------------------------------------------------------------------------------------------------------
enum : DWORD_ {
	D3D_SDK_VERSION_ = 32,
	D3DADAPTER_DEFAULT_ = 0,
	D3DDEVTYPE_HAL_ = 1,
	D3DCREATE_SOFTWARE_VERTEXPROCESSING_ = 0x20, D3DCREATE_MULTITHREADED_ = 0x4,
	D3DSWAPEFFECT_DISCARD_ = 1,
	D3DFMT_UNKNOWN_ = 0, D3DFMT_A8R8G8B8_ = 21, D3DFMT_X8R8G8B8_ = 22, D3DFMT_A8B8G8R8_ = 32, D3DFMT_L8_ = 50, D3DFMT_A8L8_ = 51, D3DFMT_D24S8_ = 75,
	D3DPOOL_DEFAULT_ = 0, D3DPOOL_MANAGED_ = 1, D3DPOOL_SYSTEMMEM_ = 2,
	D3DUSAGE_DYNAMIC_ = 0x200,
	D3DLOCK_READONLY_ = 0x10, D3DLOCK_DISCARD_ = 0x2000,
	D3DCLEAR_TARGET_ = 1,
	D3DSBT_ALL_ = 1,
	D3DPT_TRIANGLELIST_ = 4, D3DPT_TRIANGLESTRIP_ = 5,
	D3DFVF_XYZRHW_ = 0x004, D3DFVF_DIFFUSE_ = 0x040, D3DFVF_TEX1_ = 0x100,
	// render states
	D3DRS_ZENABLE_ = 7, D3DRS_FILLMODE_ = 8, D3DRS_ZWRITEENABLE_ = 14, D3DRS_ALPHATESTENABLE_ = 15, D3DRS_SRCBLEND_ = 19, D3DRS_DESTBLEND_ = 20,
	D3DRS_CULLMODE_ = 22, D3DRS_ZFUNC_ = 23, D3DRS_ALPHABLENDENABLE_ = 27, D3DRS_FOGENABLE_ = 28, D3DRS_STENCILENABLE_ = 52, D3DRS_CLIPPING_ = 136,
	D3DRS_LIGHTING_ = 137, D3DRS_COLORWRITEENABLE_ = 168, D3DRS_BLENDOP_ = 171, D3DRS_SCISSORTESTENABLE_ = 174, D3DRS_SRGBWRITEENABLE_ = 194,
	D3DRS_SEPARATEALPHABLENDENABLE_ = 206, D3DRS_SRCBLENDALPHA_ = 207, D3DRS_DESTBLENDALPHA_ = 208, D3DRS_BLENDOPALPHA_ = 209,
	D3DBLEND_ZERO_ = 1, D3DBLEND_ONE_ = 2, D3DBLEND_SRCCOLOR_ = 3, D3DBLEND_INVSRCCOLOR_ = 4, D3DBLEND_SRCALPHA_ = 5, D3DBLEND_INVSRCALPHA_ = 6,
	D3DBLEND_DESTCOLOR_ = 9, D3DBLEND_INVDESTCOLOR_ = 10,
	D3DBLENDOP_ADD_ = 1, D3DCULL_NONE_ = 1, D3DFILL_SOLID_ = 3,
	// sampler states
	D3DSAMP_ADDRESSU_ = 1, D3DSAMP_ADDRESSV_ = 2, D3DSAMP_ADDRESSW_ = 3, D3DSAMP_MAGFILTER_ = 5, D3DSAMP_MINFILTER_ = 6, D3DSAMP_MIPFILTER_ = 7, D3DSAMP_SRGBTEXTURE_ = 11, D3DSAMP_MAXMIPLEVEL_ = 9,
	D3DTADDRESS_CLAMP_ = 3, D3DTEXF_NONE_ = 0, D3DTEXF_POINT_ = 1, D3DTEXF_LINEAR_ = 2,
	// texture stage states (fixed function)
	D3DTSS_COLOROP_ = 1, D3DTSS_COLORARG1_ = 2, D3DTSS_COLORARG2_ = 3, D3DTSS_ALPHAOP_ = 4, D3DTSS_ALPHAARG1_ = 5, D3DTSS_ALPHAARG2_ = 6,
	D3DTOP_DISABLE_ = 1, D3DTOP_SELECTARG1_ = 2, D3DTOP_MODULATE_ = 4, D3DTA_DIFFUSE_ = 0, D3DTA_TEXTURE_ = 2,
};

struct D3DPRESENT_PARAMETERS_ {
	UINT_ BackBufferWidth, BackBufferHeight;
	DWORD_ BackBufferFormat;
	UINT_ BackBufferCount;
	DWORD_ MultiSampleType, MultiSampleQuality;
	DWORD_ SwapEffect;
	void *hDeviceWindow;
	int32_t Windowed;
	int32_t EnableAutoDepthStencil;
	DWORD_ AutoDepthStencilFormat;
	DWORD_ Flags;
	UINT_ FullScreen_RefreshRateInHz, PresentationInterval;
};
struct D3DSURFACE_DESC_ {
	DWORD_ Format, Type, Usage, Pool, MultiSampleType, MultiSampleQuality;
	UINT_ Width, Height;
};
struct D3DLOCKED_RECT_ { int32_t Pitch; void *pBits; };
struct D3DLOCKED_BOX_ { int32_t RowPitch, SlicePitch; void *pBits; };
struct D3DVIEWPORT9_ { DWORD_ X, Y, Width, Height; float MinZ, MaxZ; };
struct D3DDEVICE_CREATION_PARAMETERS_ { UINT_ AdapterOrdinal; DWORD_ DeviceType; void *hFocusWindow; DWORD_ BehaviorFlags; };

// ---- calling a COM method by slot ---------------------------------------------------------------------------------------------------
inline void **VTable(void *obj) { return obj ? *reinterpret_cast<void ***>(obj) : nullptr; }
template <class F> inline F Method(void *obj, int slot) { return reinterpret_cast<F>(VTable(obj)[slot]); }

inline uint32_t Release(void *obj) { return obj ? Method<uint32_t(PCB_D3DAPI *)(void *)>(obj, kRelease)(obj) : 0; }
inline uint32_t AddRef(void *obj) { return obj ? Method<uint32_t(PCB_D3DAPI *)(void *)>(obj, kAddRef)(obj) : 0; }

}  // namespace bridge::d3d9
