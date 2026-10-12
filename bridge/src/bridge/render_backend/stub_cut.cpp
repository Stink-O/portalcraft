// Portalcraft bridge: the cut-away world backend that does nothing (PLAN-windows.md section 2.3). Used where no renderer backend exists
// for it: Windows until d3d9/cut_d3d9.cpp is written. CutBackendAvailable() is false, so DigRenderMode() answers 1 ("off, the world is
// drawn uncut"): digging works for collision and on Minecraft's side, and Portal 2's walls are drawn over the holes.
#include "bridge/bridge.hpp"
#include "bridge/render_backend.hpp"

namespace bridge::rb {

bool CutBackendAvailable() { return false; }
bool CutBackendInstall(std::string &err) {
	err = "no renderer backend for the cut in this build";
	return false;
}
void CutBackendUpload(const CutBricks &) {}
void CutBackendViewOn(const CutView &) {}
void CutBackendViewOff() {}
void CutBackendUninstall() {}
std::string CutBackendCounters() { return "no backend"; }
std::string CutBackendPatchTiming() { return std::string(); }
void CutBackendDump() { console->Print("[bridge] cut: no renderer backend, nothing to dump\n"); }
void CutBackendPerfBegin() {}
void CutBackendPerfEnd() {}
void CutBackendPerfPoll(double &gpuNs, long &draws, long &lost) { gpuNs = 0, draws = 0, lost = 0; }
void CutBackendPerfReset() {}
bool CutBackendHasPerf() { return false; }

}  // namespace bridge::rb
