// Portalcraft bridge: the renderer-dependent features and the interface a renderer backend implements.
//
// Two features cannot go through the engine's material system and have to touch the graphics API that shaderapi/togl sits on:
//   1. the Minecraft hand/HUD overlay ("HUD"): one image per Minecraft frame, drawn over Portal 2's finished frame;
//   2. the cut-away world ("cut"): Portal 2's world shaders discard the fragments that lie in dug cells.
// Their logic is renderer-neutral (when to draw, which image, the dug-cell bricks, the per-view matrices, the test commands, the
// counters). Only the calls that need the API are in a backend. Backends:
//   * GL (Linux, togl):      render_backend/gl_hud.cpp, gl_cut.cpp (+ gl_api.*)      -- the reference; Linux behaviour must not change
//   * Direct3D 9:            d3d9/*.cpp  (Windows native, and Linux under Portal 2's -vulkan = DXVK)   -- owned by the D3D9 backend agent
//   * none (Windows today):  render_backend/stub_hud.cpp, stub_cut.cpp (+ stub_dig.cpp)  -- every entry point inert
// The neutral halves are render_backend/hud_logic.cpp and render_backend/cut_logic.cpp. Exactly one backend is compiled into a plugin
// (Makefile: RB=gl (default) | d3d9 (then src/bridge/d3d9/*.cpp replaces gl_*.cpp, keeping stub_dig.cpp); Makefile.win: stub_hud.cpp
// unless src/bridge/d3d9/hud_d3d9.cpp exists, stub_cut.cpp unless src/bridge/d3d9/cut_d3d9.cpp exists).
//
// Threads: "main thread" = the game thread (client frame); "render thread" = the material system's render thread (with
// mat_queue_mode 2) or the main thread when unqueued. The present/swap hook runs on the render thread.
//
// STABLE CONTRACT: the D3D9 agent implements the functions marked BACKEND; the neutral code calls them. Changes go through
// the lead.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct CViewSetup;

namespace bridge {

// ======================================================================================================================
// Entry points of the plugin for the rest of the bridge (implemented by the neutral halves; the stub implements them
// inertly). Names are historical (GlHud*/Cut*) and kept so callers did not change.
// ======================================================================================================================

// ---- HUD: main thread -----------------------------------------------------------------------------------------------
bool GlHudInit();                 // mc.cpp McInit: find the present function and hook it (calls HudBackendInstall). false: no HUD
void GlHudShutdown();             // McShutdown, before the link unmaps: stop hooking, wait for a present in flight
void GlHudSetState(bool show, bool preload, bool cursorOn, int cursorX, int cursorY);  // once per rendered world frame
void GlHudSetFade(float alpha, float r, float g, float b);   // Portal 2's screen fade, drawn over the HUD too
void GlHudHideNow();              // level load / disconnect: hide at once
bool GlHudEnabled();              // pc_mc_hud
bool GlHudReady();                // a screen-sized HUD image has been uploaded
bool GlHudDrawn();                // drawn on the last present
void GlHudPaint();                // composite.cpp, before Portal 2's own VGUI paint: queue the "under the UI" draw on the render thread
void GlHudShotTo(const std::string &fullPath);   // bugmark: save the presented frame (overlay included) at the next present

// ---- cut-away world: main thread ------------------------------------------------------------------------------------------
int DigRenderMode();              // pc_dig_render: 2 the cut is on (default), 1 off (the world is drawn uncut)
bool CutActive();                 // backend installed, textures exist
void CutInstall();                // once (CutFrame calls it): create the flush material, ask the render thread to install the backend
void CutFrame();                  // once a frame (visclamp OverrideView): install when on, rebuild the brick data when the dug set changed
void CutLevelStart();             // new level: the sky camera's area is never cut
void CutShutdown();               // McShutdown: backend uninstalled, active cleared
void CutWorldDrawBegin();         // visclamp hooks around DrawWorldLists / DrawTranslucentSurfaces (main thread; queues render-thread work)
void CutWorldDrawEnd();
void CutPerfDrawBegin();          // pc_cut_perf: GPU time around the world draws
void CutPerfDrawEnd();
void CutPerfFrame();

void CutNoteError(const std::string &e);   // a backend reports why it could not work (logged once, shown by pc_dig_render)

// ---- dig test harness (gl_dig.cpp only; inert elsewhere) -----------------------------------------------------------------
void DigHarnessMainView(const CViewSetup *view);
bool DigHarnessBusy();

// ======================================================================================================================
// The backend interface (namespace rb). BACKEND = the renderer provides it; NEUTRAL = provided by hud_logic.cpp / cut_logic.cpp
// ======================================================================================================================
namespace rb {

// ------------------------------------------------------------------------------------------------------------------------
// HUD
// ------------------------------------------------------------------------------------------------------------------------
// Counters the neutral code keeps and the backend adds to (render thread writes, torn reads only affect printouts).
struct HudStats {
	uint64_t swaps = 0, drawn = 0, uploads = 0, stateMismatch = 0, glErrors = 0, preErrors = 0;
	double upMs = 0, upMax = 0, drawMs = 0, drawMax = 0;
	int lastW = 0, lastH = 0, texW = 0, texH = 0;
	char mismatch[160] = "";
};

// What one overlay draw needs (all decided by the neutral code).
struct HudDraw {
	int W = 0, H = 0;               // drawable size in pixels
	bool intoEnginesFrame = false;  // true: the "under the UI" pass, draw into whatever target the engine has bound (its frame is stored upside down relative to the window);
	                                // false: at the present, draw into the window's back buffer
	bool testPattern = false;       // pcb_glhud_test: no fade, no cursor, no crosshair invert
	int frameW = 0, frameH = 0;     // size of the uploaded image (equal to W,H except around a resize)
	bool flipY = false;             // the uploaded rows are top-down (the image is flipped when sampled)
	int guiScale = 0;               // Minecraft GUI scale of the image (the crosshair rectangle)
	uint32_t mcFlags = 0;           // skycraft::proto::kMc* flags of the image
	uint32_t camera = 0;            // 0 first person (crosshair invert pass wanted when no screen is open)
	bool cursorOn = false;          // an MC screen is open: draw the arrow cursor
	int cursorX = 0, cursorY = 0;   // image pixels, top-left origin
	float fadeA = 0, fadeR = 1, fadeG = 1, fadeB = 1;   // Portal 2's screen fade
};

// -- BACKEND: install (main thread) ---------------------------------------------------------------------------------------
// Find the API's present function and hook it so that every present calls HudPresentBegin / HudOnPresent(window) / HudPresentEnd
// around the real one (see HudPresentBegin). `window` is any pointer that identifies the target for HudBackendWindowSize.
bool HudBackendInstall();
void HudBackendUninstall();   // stop hooking now (the hook may still be running on the render thread: the neutral code waits via HudPresentEnd)

// -- BACKEND: render thread -----------------------------------------------------------------------------------------------
bool HudBackendReady();       // create GPU resources on first use (shader, texture...); false: unavailable, nothing is drawn (the neutral code tries again every frame: be cheap after a failure)
bool HudBackendWindowSize(void *window, int *w, int *h);   // drawable size in pixels
void HudBackendBegin(HudStats &st);   // drain errors left by the engine (count st.preErrors), save every piece of API state the draw touches
void HudBackendEnd(HudStats &st, bool verify);   // count errors of ours (st.glErrors), restore the saved state exactly; verify: re-read and compare, count st.stateMismatch + st.mismatch
void HudBackendUpload(const uint8_t *rgba, int w, int h, HudStats &st);   // straight RGBA8, premultiplied alpha, rows as delivered; count uploads/time/texW/texH
void HudBackendDraw(const HudDraw &d);   // premultiplied-alpha blend of the uploaded image; cursor; crosshair invert pass; screen fade (see gl_hud.cpp's shader for the exact maths)
bool HudBackendReadPixel(int W, int H, int x, int y, uint8_t rgba[4]);   // the presented frame, x,y top-left origin
bool HudBackendReadFrame(int W, int H, std::vector<uint8_t> &raw, bool &bottomUp);   // the presented frame, RGBA8, W*H*4; bottomUp: first row is the bottom row (the neutral code flips)
bool HudBackendWantsFrame();  // the backend has its own reason to read frames this present (the GL recorder)
void HudBackendAfterOverlay(int W, int H);   // after the overlay and the readbacks (the GL recorder); may be empty
void HudBackendToast(int W, int H, const int (*rects)[4], const uint32_t *rgba, int n);   // bug-mark toast: solid rectangles x,y,w,h (top-left origin) with 0xAABBGGRR colour, drawn last

// -- NEUTRAL: called by the backend's present hook (render thread) -------------------------------------------------------------
// The hook must be:    const bool run = HudPresentBegin();  if (run) HudOnPresent(window);  <call the real present>;  HudPresentEnd();
// HudPresentBegin always counts the present in flight; it returns false once the plugin is shutting down (do not call HudOnPresent
// then, still call the real present and HudPresentEnd). The real function must stay callable from inside the hook.
bool HudPresentBegin();
void HudPresentEnd();
void HudOnPresent(void *window);   // the overlay decision and draw

// ------------------------------------------------------------------------------------------------------------------------
// Cut-away world
// ------------------------------------------------------------------------------------------------------------------------
// The dug cells as a sparse 3D grid, built on the main thread (cut_logic.cpp BuildData), uploaded on the render thread.
// Cells are Minecraft blocks. A brick is 8x8x8 cells. The index covers the whole +-16384 u map; the atlas is 16x16x16 bricks.
struct CutBricks {
	uint32_t gen = 0;
	int org[3] = {0, 0, 0};       // index origin (cells, a multiple of 8)
	int dims[3] = {0, 0, 0};      // index size (bricks)
	std::vector<uint8_t> index;   // dims[0]*dims[1]*dims[2] entries of 2 bytes (RG8): 0 = no brick, else atlas slot + 1 (low byte, high byte), x fastest
	std::vector<uint8_t> atlas;   // 128*128*128 bytes (R8), x fastest, then y, then z; 255 = dug cell. Brick slot s (the index stores s+1) holds its cell (lx,ly,lz) at
	                              // texel (tx,ty,tz) = ((s%16)*8+lx, ((s/16)%16)*8+ly, (s/256)*8+lz)
	int bricks = 0, cells = 0, dropped = 0;
	double map[12] = {0};         // world (Source units) -> cell coordinates, 3 rows of 4: cell = M * (x, y, z, 1)
};
constexpr int kCutBrick = 8, kCutAtlasBricks = 16, kCutAtlasTexels = 128;
constexpr int kCutUnitIdx = 30, kCutUnitAtl = 31;   // GL texture units (informational for other backends)

// Everything the cut needs for one world draw (computed by the neutral code from the view's matrices).
struct CutView {
	// world = worldFromClip * (clip.x, clip.y, clip.w, 1): 3x3 inverse of the x,y,w rows of view*projection, with translation (row-major 3x4)
	float worldFromClip[3][4] = {};
	float cellFromWorld[3][4] = {};   // the CutBricks::map of the uploaded data, as floats
	float eye[3] = {0, 0, 0};         // camera position (Source units)
	float eps = 0.25f;                // step along the surface normal (Source units)
	bool audit = false;               // pc_cut_audit: paint cut fragments magenta instead of discarding them
	bool viewRay = false;             // pc_cut_rule 0: step along the view ray instead of the surface normal
	int org[3] = {0, 0, 0}, dims[3] = {0, 0, 0}, brick = kCutBrick;   // of the uploaded data
};

// -- BACKEND: any thread ------------------------------------------------------------------------------------------------------
bool CutBackendAvailable();   // false: there is no cut in this build (DigRenderMode() then answers 1, "off", and nothing is installed)
// -- BACKEND: render thread ---------------------------------------------------------------------------------------------------
// Install: hook shader creation/use so that world shaders get the cut test. false (with a reason in err): the cut stays off for the run.
bool CutBackendInstall(std::string &err);
void CutBackendUpload(const CutBricks &d);      // create/replace the two volume textures
void CutBackendViewOn(const CutView &v);        // set the constants and bind the textures for the world draw that follows
void CutBackendViewOff();                       // the enable flag off (entities, our blocks, the HUD are never cut)
// -- BACKEND: main thread ---------------------------------------------------------------------------------------------------
void CutBackendUninstall();                     // undo the hooks (the render thread may still be inside one: make it harmless)
std::string CutBackendCounters();               // the pc_dig_render status line's backend part (links patched, failures...)
std::string CutBackendPatchTiming();            // a line about the patching time ("cut: patching at first use took ..."); "" if none
void CutBackendDump();                          // pc_cut_dump: write a sample of the patched shaders into the cache folder
// GPU timing of the world draws (pc_cut_perf). Render thread: Begin/End around one draw; any thread: Poll adds finished results.
void CutBackendPerfBegin();
void CutBackendPerfEnd();
void CutBackendPerfPoll(double &gpuNs, long &draws, long &lost);   // totals since PerfReset
void CutBackendPerfReset();
bool CutBackendHasPerf();                       // false: pc_cut_perf says unsupported

}  // namespace rb
}  // namespace bridge
