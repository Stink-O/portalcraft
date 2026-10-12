// Portalcraft bridge: Windows input events (PLAN-windows.md phase 2).
// The Linux code gets mouse motion, the wheel, text and key events from Portal 2's SDL event queue (SDL_SetEventFilter, SDL_AddEventWatch,
// SDL_PushEvent). Portal 2 on Windows has no SDL, so those functions are emulated here: a WH_GETMESSAGE hook on the window's thread sees the Win32
// input messages as they are taken off the queue, builds the equivalent SDL_Event (same byte layout as SDL2: type @0, mouse motion x/y @20/24 and
// xrel/yrel @28/32, wheel y @20, button @16, key scancode @16 / repeat @13, text @12) and runs it through the registered watchers and filter.
// When the filter returns 0 the message is swallowed (turned into WM_NULL), which is what "SDL drops the event" meant for the engine: Portal 2's
// binds stay quiet while a Minecraft screen is open. Mouse look is stopped three ways while the filter drops motion: WM_MOUSEMOVE and WM_INPUT
// (raw input, m_rawinput 1) are swallowed, and for an engine that polls the cursor (m_rawinput 0) the cursor is clipped to the window's centre
// pixel (Pin/Unpin below). Raw deltas come from our own raw-mouse registration.
// UNVERIFIED: written without Windows; nothing here has run.
#ifdef _WIN32
#include "platform.hpp"
#include "win_internal.hpp"

#include <windows.h>
#undef near
#undef far

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

extern FILE *g_bridgeLogFile;

namespace bridge::plat {
namespace {

using Filter = int (*)(void *userdata, void *event);
using Watch = int (*)(void *userdata, void *event);

std::mutex g_mx;
std::atomic<Filter> g_filter{nullptr};
std::atomic<void *> g_filterData{nullptr};
struct WatchRec {
	Watch fn;
	void *ud;
};
std::vector<WatchRec> g_watches;

HHOOK g_hook = nullptr;
DWORD g_hookThread = 0;
std::atomic<bool> g_wanted{false};

// SDL scancode for a Windows virtual key (built once from the forward table).
int VkToSc(int vk, bool extended) {
	static short table[256];
	static std::once_flag once;
	std::call_once(once, [] {
		std::memset(table, 0, sizeof table);
		for (int sc = 4; sc < 512; ++sc)
			if (int v = WinScToVk(sc))
				if (v < 256 && !table[v]) table[v] = (short)sc;
	});
	if (vk == VK_RETURN && extended) return 88;  // keypad enter
	return vk > 0 && vk < 256 ? table[vk] : 0;
}

void Put32(uint8_t *ev, int off, int32_t v) { std::memcpy(ev + off, &v, 4); }

// Runs one event through the watchers and then the filter. true = keep (the engine gets the message).
bool Dispatch(uint8_t *ev) {
	std::vector<WatchRec> watches;
	{
		std::lock_guard<std::mutex> lk(g_mx);
		watches = g_watches;
	}
	for (auto &w : watches) w.fn(w.ud, ev);
	if (Filter f = g_filter.load()) return f(g_filterData.load(), ev) != 0;
	return true;
}

std::atomic<DWORD> g_lastRawTick{0};

// ---- the cursor pin ----------------------------------------------------------------------------------------------------------
// Swallowing WM_MOUSEMOVE / WM_INPUT stops an engine that reads messages. An engine that reads the cursor position and recentres it
// (m_rawinput 0: GetCursorPos delta, then SetCursorPos) would still turn Chell, so while the filter is dropping mouse motion (a Minecraft
// screen is open, or pc_input_lock) the cursor is clipped to the window's centre pixel: the engine's delta is then always zero. Minecraft's
// own cursor keeps moving because the raw deltas (RegisterRaw) still arrive, and are dropped by the filter, which turns them into the screen
// cursor. The pin is lifted as soon as the filter keeps a motion event, the window loses focus, or the probe in WinInputPoll sees the filter
// keep a zero motion (screen closed). Behaviour with no Minecraft screen open: the filter keeps motion, nothing is ever pinned.
std::mutex g_pinMx;
bool g_pinned = false;
RECT g_prevClip = {0, 0, 0, 0};
bool g_prevClipValid = false;

void Pin() {
	std::lock_guard<std::mutex> lk(g_pinMx);
	if (g_pinned) return;
	HWND w = (HWND)WinGameWindow();
	if (!w || !WinGameFocused()) return;
	RECT cr;
	if (!GetClientRect(w, &cr)) return;
	POINT c = {(cr.right - cr.left) / 2, (cr.bottom - cr.top) / 2};
	ClientToScreen(w, &c);
	g_prevClipValid = GetClipCursor(&g_prevClip) != 0;
	RECT pin = {c.x, c.y, c.x + 1, c.y + 1};
	if (!ClipCursor(&pin)) return;
	g_pinned = true;
	SetCursorPos(c.x, c.y);
	if (g_bridgeLogFile) std::fprintf(g_bridgeLogFile, "[bridge] input: cursor pinned at %ld,%ld (the filter is dropping mouse motion)\n", c.x, c.y), std::fflush(g_bridgeLogFile);
}
void Unpin() {
	std::lock_guard<std::mutex> lk(g_pinMx);
	if (!g_pinned) return;
	g_pinned = false;
	// the clip the engine had (usually the window or nothing); a full-desktop rectangle means "no clip"
	ClipCursor(g_prevClipValid ? &g_prevClip : nullptr);
	if (g_bridgeLogFile) std::fprintf(g_bridgeLogFile, "[bridge] input: cursor pin lifted\n"), std::fflush(g_bridgeLogFile);
}
void NoteMotionVerdict(bool keep) {
	if (keep) Unpin();
	else Pin();
}

struct MouseState {
	int lastX = -1, lastY = -1;
	int wheelAcc = 0;
} g_mouse;

// Builds and dispatches the SDL event for one message; returns false when the filter dropped it.
bool Translate(const MSG &m) {
	alignas(8) uint8_t ev[56];
	std::memset(ev, 0, sizeof ev);
	Put32(ev, 4, (int32_t)GetTickCount());
	Put32(ev, 8, 1);  // windowID
	switch (m.message) {
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
	case WM_KEYUP:
	case WM_SYSKEYUP: {
		const bool down = m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN;
		const bool ext = (m.lParam & (1 << 24)) != 0;
		const UINT scan = (UINT)((m.lParam >> 16) & 0xFF);
		int vk = (int)m.wParam;
		if (vk == VK_SHIFT) vk = (int)MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX);
		else if (vk == VK_CONTROL) vk = ext ? VK_RCONTROL : VK_LCONTROL;
		else if (vk == VK_MENU) vk = ext ? VK_RMENU : VK_LMENU;
		int sc = WinSet1ToSc(scan, ext);  // (the physical position, as SDL reports it; the layout's virtual key for the rest)
		if (!sc) sc = VkToSc(vk, ext);
		if (!sc) return true;
		Put32(ev, 0, down ? 0x300 : 0x301);
		ev[12] = down ? 1 : 0;
		ev[13] = (down && (m.lParam & (1 << 30))) ? 1 : 0;  // auto-repeat
		Put32(ev, 16, sc);
		return Dispatch(ev);
	}
	case WM_CHAR:
	case WM_SYSCHAR: {
		// UTF-16: an astral code point arrives as two WM_CHAR (high, then low). SDL delivers one TEXTINPUT with the whole UTF-8 sequence,
		// so the pair is joined. The high half is held back, but the engine must not see half a pair: its fate is the filter's verdict on
		// a probe (filter only, no watchers: the verdict does not depend on the character), and the low half follows the high one's fate.
		static wchar_t high = 0;
		static bool highKept = true;
		const wchar_t c = (wchar_t)m.wParam;
		if (c >= 0xD800 && c <= 0xDBFF) {
			high = c;
			Put32(ev, 0, 0x303);
			ev[12] = '?';
			Filter f = g_filter.load();
			highKept = !f || f(g_filterData.load(), ev) != 0;
			return highKept;
		}
		wchar_t w[2] = {c, 0};
		int wl = 1;
		bool pairedKept = true;
		if (c >= 0xDC00 && c <= 0xDFFF) {
			if (!high) return true;  // a stray low half: nothing sensible to say, leave it to the engine
			w[0] = high, w[1] = c, wl = 2;
			pairedKept = highKept;
			high = 0;
		} else {
			high = 0;  // a high half followed by anything else was unpaired: forget it
			if (c < 32 || c == 127) return true;  // backspace, enter, tab: keys, not text
		}
		char utf8[8] = {};
		const int n = WideCharToMultiByte(CP_UTF8, 0, w, wl, utf8, sizeof utf8 - 1, nullptr, nullptr);
		if (n <= 0) return pairedKept;
		Put32(ev, 0, 0x303);
		std::memcpy(ev + 12, utf8, (size_t)n);
		return Dispatch(ev) && pairedKept;
	}
	case WM_MOUSEMOVE: {
		const int x = (int)(short)LOWORD(m.lParam), y = (int)(short)HIWORD(m.lParam);
		// While raw input is arriving (a WM_INPUT in the last 150 ms) it carries the deltas, so this event reports the position only (no
		// double counting in the filter, and a recentring warp of the cursor adds nothing).
		const bool rawLive = GetTickCount() - g_lastRawTick.load() < 150;
		const int rx = (g_mouse.lastX < 0 || rawLive) ? 0 : x - g_mouse.lastX, ry = (g_mouse.lastY < 0 || rawLive) ? 0 : y - g_mouse.lastY;
		g_mouse.lastX = x, g_mouse.lastY = y;
		Put32(ev, 0, 0x400);
		Put32(ev, 20, x), Put32(ev, 24, y), Put32(ev, 28, rx), Put32(ev, 32, ry);
		uint32_t st = 0;
		if (m.wParam & MK_LBUTTON) st |= 1;
		if (m.wParam & MK_MBUTTON) st |= 2;
		if (m.wParam & MK_RBUTTON) st |= 4;
		Put32(ev, 16, (int32_t)st);
		const bool keep = Dispatch(ev);
		NoteMotionVerdict(keep);
		return keep;
	}
	case WM_INPUT: {
		// Raw mouse (Portal 2's m_rawinput path, and our own registration, see RegisterRaw): relative motion becomes the SDL motion event
		// (xrel/yrel); when the filter drops it the message is swallowed, so the engine's raw accumulators stay at zero and Chell does not turn.
		RAWINPUT ri;
		UINT size = sizeof ri;
		if (GetRawInputData((HRAWINPUT)m.lParam, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) return true;
		if (ri.header.dwType != RIM_TYPEMOUSE) return true;  // keyboard raw input is not registered; leave anything else alone
		if (ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) return true;  // tablets / remote desktops: no deltas to report
		const int rx = (int)ri.data.mouse.lLastX, ry = (int)ri.data.mouse.lLastY;
		if (!rx && !ry) return true;  // button-only report
		g_lastRawTick = GetTickCount();
		POINT p = {0, 0};
		GetCursorPos(&p);
		ScreenToClient(m.hwnd, &p);
		Put32(ev, 0, 0x400);
		Put32(ev, 20, p.x), Put32(ev, 24, p.y), Put32(ev, 28, rx), Put32(ev, 32, ry);
		uint32_t st = 0;
		if (GetKeyState(VK_LBUTTON) & 0x8000) st |= 1;
		if (GetKeyState(VK_MBUTTON) & 0x8000) st |= 2;
		if (GetKeyState(VK_RBUTTON) & 0x8000) st |= 4;
		Put32(ev, 16, (int32_t)st);
		const bool keep = Dispatch(ev);
		NoteMotionVerdict(keep);
		return keep;
	}
	case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
	case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_XBUTTONDOWN: case WM_XBUTTONUP: {
		int button = 1;
		bool down = false;
		switch (m.message) {
		case WM_LBUTTONDOWN: down = true; [[fallthrough]];
		case WM_LBUTTONUP: button = 1; break;
		case WM_MBUTTONDOWN: down = true; [[fallthrough]];
		case WM_MBUTTONUP: button = 2; break;
		case WM_RBUTTONDOWN: down = true; [[fallthrough]];
		case WM_RBUTTONUP: button = 3; break;
		case WM_XBUTTONDOWN: down = true; [[fallthrough]];
		default: button = HIWORD(m.wParam) == XBUTTON1 ? 4 : 5; break;
		}
		Put32(ev, 0, down ? 0x401 : 0x402);
		ev[16] = (uint8_t)button;
		ev[17] = down ? 1 : 0;
		ev[18] = 1;  // clicks
		Put32(ev, 20, (int)(short)LOWORD(m.lParam)), Put32(ev, 24, (int)(short)HIWORD(m.lParam));
		return Dispatch(ev);
	}
	case WM_MOUSEWHEEL: {
		g_mouse.wheelAcc += (int)(short)HIWORD(m.wParam);
		const int notches = g_mouse.wheelAcc / WHEEL_DELTA;
		g_mouse.wheelAcc -= notches * WHEEL_DELTA;
		if (!notches) return true;
		Put32(ev, 0, 0x403);
		Put32(ev, 20, notches);  // y, positive = away from the user, as SDL (direction normal @24 = 0)
		return Dispatch(ev);
	}
	default: return true;
	}
}

LRESULT CALLBACK GetMsgProc(int code, WPARAM wp, LPARAM lp) {
	if (code == HC_ACTION && wp == PM_REMOVE) {
		MSG *m = (MSG *)lp;
		if (m && m->hwnd && m->hwnd == (HWND)WinGameWindow() && !Translate(*m)) {
			if (m->message == WM_INPUT) DefWindowProcW(m->hwnd, WM_INPUT, m->wParam, m->lParam);  // frees the raw input buffer the engine will not see
			m->message = WM_NULL;
			m->wParam = 0, m->lParam = 0;
		}
	}
	return CallNextHookEx(g_hook, code, wp, lp);
}

// Tests only (PCB_KEEP_ACTIVE=1, set by tools/run_game_proton.sh): Windows Portal 2 sleeps about 50 ms a frame while it is not the active
// app, and unlike Linux it has no engine_no_focus_sleep to turn that off, so an unattended test whose window lost focus ran at ~18 fps. The
// game window's WM_ACTIVATEAPP(FALSE) is passed on as TRUE. Players never set it: the game keeps its normal behaviour when they alt-tab.
WNDPROC g_keepActivePrev = nullptr;
LRESULT CALLBACK KeepActiveProc(HWND h, UINT m, WPARAM w, LPARAM l) {
	if (m == WM_ACTIVATEAPP && !w) w = TRUE;
	return IsWindowUnicode(h) ? CallWindowProcW(g_keepActivePrev, h, m, w, l) : CallWindowProcA(g_keepActivePrev, h, m, w, l);
}

// Raw mouse input for the game window, so relative deltas exist whether or not the engine uses m_rawinput. The same registration (usage 1/2,
// flags 0, this window) is what the engine makes in raw mode, so it replaces rather than competes with it; if the engine removes it, the
// WM_MOUSEMOVE deltas take over again (g_lastRawTick goes stale). PCB_NO_RAWREG=1 skips it.
void RegisterRaw(HWND w) {
	char v[8] = {};
	if (GetEnvironmentVariableA("PCB_NO_RAWREG", v, sizeof v) && v[0] == '1') return;
	RAWINPUTDEVICE d = {0x01, 0x02, 0, w};
	const BOOL ok = RegisterRawInputDevices(&d, 1, sizeof d);
	if (g_bridgeLogFile) std::fprintf(g_bridgeLogFile, "[bridge] input: raw mouse registration %s\n", ok ? "ok" : "failed"), std::fflush(g_bridgeLogFile);
}
bool InstallHook() {
	if (g_hook) return true;
	HWND w = (HWND)WinGameWindow();
	if (!w) return false;
	g_hookThread = GetWindowThreadProcessId(w, nullptr);
	g_hook = SetWindowsHookExW(WH_GETMESSAGE, GetMsgProc, nullptr, g_hookThread);
	if (g_hook) RegisterRaw(w);
	if (g_bridgeLogFile) {
		if (g_hook) std::fprintf(g_bridgeLogFile, "[bridge] input: message hook on the game window %p (thread %lu)\n", (void *)w, (unsigned long)g_hookThread);
		else std::fprintf(g_bridgeLogFile, "[bridge] input: SetWindowsHookEx failed (error %lu): no key/wheel/text events for Minecraft screens\n", (unsigned long)GetLastError());
		std::fflush(g_bridgeLogFile);
	}
	return g_hook != nullptr;
}
void RemoveHook() {
	Unpin();
	if (g_hook) UnhookWindowsHookEx(g_hook), g_hook = nullptr;
}
void Want() {
	g_wanted = true;
	InstallHook();
}
void MaybeUnwant() {
	if (!g_filter.load()) {
		std::lock_guard<std::mutex> lk(g_mx);
		if (!g_watches.empty()) return;
	}
	if (!g_filter.load()) g_wanted = false, RemoveHook();
}

// ---- the SDL functions -------------------------------------------------------------------------------------------------------
void Emu_SetEventFilter(Filter f, void *ud) {
	g_filterData = ud;
	g_filter = f;
	if (f) Want();
	else MaybeUnwant();
}
int Emu_GetEventFilter(Filter *f, void **ud) {
	if (f) *f = g_filter.load();
	if (ud) *ud = g_filterData.load();
	return g_filter.load() != nullptr;
}
void Emu_AddEventWatch(Watch w, void *ud) {
	{
		std::lock_guard<std::mutex> lk(g_mx);
		g_watches.push_back({w, ud});
	}
	Want();
}
void Emu_DelEventWatch(Watch w, void *ud) {
	{
		std::lock_guard<std::mutex> lk(g_mx);
		for (size_t i = 0; i < g_watches.size(); ++i)
			if (g_watches[i].fn == w && g_watches[i].ud == ud) {
				g_watches.erase(g_watches.begin() + (long)i);
				break;
			}
	}
	MaybeUnwant();
}
// Tests push events through the same filter; nothing is queued for the engine (it reads Win32 messages, which cannot be forged here).
int Emu_PushEvent(void *ev) {
	return Dispatch((uint8_t *)ev) ? 1 : 0;
}

}  // namespace

void *WinEventSymbol(const char *name) {
	if (!std::strcmp(name, "SDL_SetEventFilter")) return (void *)&Emu_SetEventFilter;
	if (!std::strcmp(name, "SDL_GetEventFilter")) return (void *)&Emu_GetEventFilter;
	if (!std::strcmp(name, "SDL_AddEventWatch")) return (void *)&Emu_AddEventWatch;
	if (!std::strcmp(name, "SDL_DelEventWatch")) return (void *)&Emu_DelEventWatch;
	if (!std::strcmp(name, "SDL_PushEvent")) return (void *)&Emu_PushEvent;
	return nullptr;
}

void WinInputPoll() {
	if (g_wanted.load() && !g_hook) InstallHook();  // the window did not exist when the filter was set
	// Probe the filter with a zero motion (the filter alone: no watchers): dropped = a Minecraft screen is open (pin before the engine reads the
	// cursor, so the first move does not turn the view), kept = release. Without focus never pinned.
	Filter f = g_filter.load();
	if (!f || !g_hook) return;
	if (!WinGameFocused()) return Unpin();
	alignas(8) uint8_t ev[56] = {};
	POINT p = {0, 0};
	GetCursorPos(&p);
	if (HWND w = (HWND)WinGameWindow()) ScreenToClient(w, &p);
	Put32(ev, 0, 0x400), Put32(ev, 8, 1), Put32(ev, 20, p.x), Put32(ev, 24, p.y);
	NoteMotionVerdict(f(g_filterData.load(), ev) != 0);
}

// (test harness) Ask for the keyboard focus for the game's window: Source throttles an inactive window, which the dev runs under Proton hit whenever
// the person at the machine works in another window.
bool FocusGameWindow() {
	HWND w = (HWND)WinGameWindow();
	if (!w) return false;
	ShowWindow(w, SW_SHOW);
	SetForegroundWindow(w);
	SetActiveWindow(w);
	SetFocus(w);
	return WinGameFocused();
}

void WinKeepActiveForTests() {
	char v[8] = {};
	if (!GetEnvironmentVariableA("PCB_KEEP_ACTIVE", v, sizeof v) || v[0] != '1' || g_keepActivePrev) return;
	HWND w = (HWND)WinGameWindow();
	if (!w) return;
	g_keepActivePrev = (WNDPROC)(IsWindowUnicode(w) ? SetWindowLongPtrW(w, GWLP_WNDPROC, (LONG_PTR)KeepActiveProc) : SetWindowLongPtrA(w, GWLP_WNDPROC, (LONG_PTR)KeepActiveProc));
	if (g_bridgeLogFile)
		std::fprintf(g_bridgeLogFile, "[bridge] input: PCB_KEEP_ACTIVE: the game window stays the active app for this test (%s)\n", g_keepActivePrev ? "ok" : "failed"), std::fflush(g_bridgeLogFile);
}

void WinInputShutdown() {
	g_wanted = false;
	RemoveHook();
	g_filter = nullptr;
	std::lock_guard<std::mutex> lk(g_mx);
	g_watches.clear();
}

}  // namespace bridge::plat
#endif  // _WIN32
