// Portalcraft bridge: the HUD overlay backend that does nothing (PLAN-windows.md section 2.4). Used where no renderer backend exists for
// it: Windows until d3d9/hud_d3d9.cpp is written. The plugin then runs without the Minecraft hand/HUD overlay (hud_logic.cpp's
// GlHudInit fails, mc.cpp continues, GlHudEnabled() stays true but nothing is ever drawn).
#include "bridge/bridge.hpp"
#include "bridge/render_backend.hpp"

namespace bridge::rb {

bool HudBackendInstall() {
	console->Print("[bridge] hud: no renderer backend in this build (the Minecraft hand/HUD overlay is unavailable)\n");
	return false;
}
void HudBackendUninstall() {}
bool HudBackendReady() { return false; }
bool HudBackendWindowSize(void *, int *w, int *h) { return *w = 0, *h = 0, false; }
void HudBackendBegin(HudStats &) {}
void HudBackendEnd(HudStats &, bool) {}
void HudBackendUpload(const uint8_t *, int, int, HudStats &) {}
void HudBackendDraw(const HudDraw &) {}
bool HudBackendReadPixel(int, int, int, int, uint8_t[4]) { return false; }
bool HudBackendReadFrame(int, int, std::vector<uint8_t> &, bool &) { return false; }
bool HudBackendWantsFrame() { return false; }
void HudBackendAfterOverlay(int, int) {}
void HudBackendToast(int, int, const int (*)[4], const uint32_t *, int) {}

}  // namespace bridge::rb
