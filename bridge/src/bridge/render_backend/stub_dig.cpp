// Portalcraft bridge: the dig test harness (gl_dig.cpp) reads depth and colour back through OpenGL; builds without it get these.
#include "bridge/bridge.hpp"
#include "bridge/render_backend.hpp"

namespace bridge {
void DigHarnessMainView(const CViewSetup *) {}
bool DigHarnessBusy() { return false; }
}  // namespace bridge
