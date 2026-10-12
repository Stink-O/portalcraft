// Portalcraft bridge: log a native backtrace to bridge.log when the game crashes, then end the process.
// The handlers themselves are OS code and live in platform/platform_linux.cpp (signals) and platform/platform_win.cpp
// (a vectored exception handler).
#include "bridge.hpp"
#include "platform/platform.hpp"

namespace bridge {

void CrashLogInstall() { plat::CrashHandlerInstall(); }
void CrashLogRemove() { plat::CrashHandlerRemove(); }

}  // namespace bridge
