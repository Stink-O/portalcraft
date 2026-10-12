// Portalcraft bridge: the Direct3D 9 backend's hook layer (PLAN-windows.md section 2.2, PLAN-vulkan.md V1).
// Finds the IDirect3DDevice9 vtable (through a throw-away device), patches slots in place, and offers the features
// (hud, cut) listeners for Present / Reset / shader creation / shader binding. Works unchanged on native Windows d3d9 and
// on DXVK (Portal 2 -vulkan on Linux).
#pragma once

#include "bridge/d3d9/d3d9_min.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace bridge::d3d9 {

// Lifecycle (main thread). Init is idempotent; it returns false (with a console line) when the game is not running a D3D9 device
// we can reach (e.g. togl/OpenGL mode on Linux). Shutdown restores every patched slot and waits for hooks in flight.
bool Init();
void Shutdown();
bool Active();

// ---- listeners (render thread, the engine's own device) ----------------------------------------------------------------------------------
// Present: called at the outermost Present, before the engine's own Present runs (w, h = the back buffer size).
using PresentFn = void (*)(void *dev, int w, int h);
void AddPresentListener(PresentFn fn);
// Reset: called before the engine's Reset/ResetEx. Default-pool resources must be released here.
using ResetFn = void (*)(void *dev);
void AddResetListener(ResetFn fn);
// Shader creation: called after a successful create with the original bytecode (dword tokens) and the new shader object.
using ShaderFn = void (*)(void *dev, bool isPixel, const uint32_t *code, void *shader);
void AddShaderListener(ShaderFn fn);
// Shader binding (SetVertexShader / SetPixelShader), called before the engine's call goes through; the listener may change which
// object is bound by returning a replacement (nullptr: keep). Not called for the backend's own calls.
using BindFn = void *(*)(void *dev, bool isPixel, void *shader);
void SetBindListener(BindFn fn);
// SetTexture, called after the engine's call went through (the cut re-binds its own samplers). Not called for the backend's own calls.
using TextureFn = void (*)(void *dev, uint32_t stage, void *tex);
void SetTextureListener(TextureFn fn);
// A draw call (DrawPrimitive / DrawIndexedPrimitive), called before the engine's call goes through. Not called for the backend's own calls.
using DrawFn = void (*)(void *dev);
void SetDrawListener(DrawFn fn);
// SetPixelShaderConstantF / SetVertexShaderConstantF by the engine (start register, number of registers), called before the call goes through.
using ConstFn = void (*)(void *dev, bool isPixel, uint32_t start, uint32_t count);
void SetConstListener(ConstFn fn);
// Enable the hooks the cut needs (SetVertexShader/SetPixelShader/SetTexture are off until a feature asks, to cost nothing otherwise).
void EnableBindHooks(bool on);

// The back buffer size of the engine's device (false when unknown).
bool BackBufferSize(void *dev, int &w, int &h);
void *EngineDevice();  // the engine's device (null until the first Present)

// A state block capture around everything a listener draws (the engine's shader API caches device state and assumes the device
// matches it): ctor captures and switches the render target to the back buffer with flat 2D state, dtor restores. Inside the scope the
// backend's own hooks ignore the calls. ok() false: nothing was set up, do not draw.
class OverlayScope {
public:
	OverlayScope(void *dev, int w, int h);
	~OverlayScope();
	bool ok() const { return ok_; }

private:
	void *dev_;
	bool ok_ = false;
	void *rt_ = nullptr, *bb_ = nullptr;
};
bool InOverlay();  // the calling thread is inside an OverlayScope or an InternalScope
// The backend's own device calls outside an overlay (creating its shaders and textures): the hooks ignore them (no census, no listeners).
struct InternalScope {
	InternalScope();
	~InternalScope();
};
// The presented frame (back buffer) as RGBA8, top row first. Render thread, device = the engine's.
bool ReadBackBuffer(void *dev, std::vector<uint8_t> &rgba, int &w, int &h);

// Presented-frame screenshot: written at the next Present after the listeners drew (name starting with '/' = a full path).
void RequestInfo();  // print the engine device's back buffer / render target / viewport sizes at the next Present
void RequestShot(const std::string &path);
// One pixel of the presented frame (top-left origin), RGBA in the low bytes; done is set when read.
void RequestProbe(int x, int y);
bool ProbeResult(uint32_t &rgba);

struct Counters {
	std::atomic<uint64_t> presents{0}, swapPresents{0}, presentExs{0}, resets{0}, vsCreated{0}, psCreated{0}, vsSet{0}, psSet{0}, hrFail{0}, overlays{0};
	std::atomic<uint64_t> ourPresentNs{0}, enginePresentNs{0}, enginePresentMaxNs{0};  // wall time in our Present listeners / in the engine's own Present (blocks on vsync, the compositor, the GPU)
	std::atomic<uint32_t> presentThreads{0};  // distinct threads seen calling the outermost Present
	std::atomic<uint64_t> hookNs{0}, hookCalls{0}, hookMaxNs{0};  // wall time inside our bind/texture/draw/constant listeners (the render thread's per-call cost)
};
Counters &Stats();
std::string ThreadSummary();  // "tid:presents, ..." of the threads that called the outermost Present

// Shader model of a token stream: 0x0200 for 2_0, 0x0300 for 3_0 ...; vertex = high word 0xFFFE, pixel = 0xFFFF. Token count includes the end token.
int ShaderModel(const uint32_t *code, bool *isPixel = nullptr);
uint32_t ShaderTokens(const uint32_t *code);

void Printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));  // the bridge console (main thread) / log

}  // namespace bridge::d3d9
