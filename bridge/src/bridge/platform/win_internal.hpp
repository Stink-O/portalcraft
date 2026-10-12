// Portalcraft bridge: declarations shared between platform_win.cpp and input_win.cpp only (Windows build).
#pragma once
#include <cstdint>

namespace bridge::plat {
void *WinGameWindow();     // Portal 2's main window (HWND) or nullptr
bool WinGameFocused();     // a window of this process is the foreground window
int WinScToVk(int sdlScancode);  // SDL scancode -> Windows virtual key (0 when none)
int WinSet1ToSc(unsigned scan, bool extended);  // a key message's physical scan code -> SDL scancode for letter/digit/punctuation keys (0: other)
// SDL_SetEventFilter / GetEventFilter / AddEventWatch / DelEventWatch / PushEvent (input_win.cpp); nullptr for any other name
void *WinEventSymbol(const char *name);
void WinInputPoll();       // retries the message hook while it is wanted but the window did not exist yet (called from the polling emulation)
void WinInputShutdown();
}  // namespace bridge::plat
