// Portalcraft bridge: the Windows implementation of platform.hpp (MinGW-w64, 32-bit, system DLLs only: kernel32, user32, advapi32, psapi).
// Written without a Windows machine to run it on (PLAN-windows.md phase 1): everything marked UNVERIFIED has never executed.
#ifdef _WIN32
#include "platform.hpp"
#include "win_internal.hpp"

#include "skycraft_protocol.h"

#include <windows.h>
#include <psapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#undef near
#undef far

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern FILE *g_bridgeLogFile;

namespace bridge::plat {

// ---- UTF-8 <-> UTF-16 -----------------------------------------------------------------------------------------------------
std::wstring WidenUtf8(const char *s);  // (below; used by the C file wrappers at the end)
static std::wstring Wide(const std::string &s) {
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
	return w;
}
std::wstring WidenUtf8(const char *s) { return Wide(std::string(s)); }
static std::string Narrow(const wchar_t *w, int len = -1) {
	if (!w) return std::string();
	if (len < 0) len = (int)wcslen(w);
	if (len == 0) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
	std::string s((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, len, &s[0], n, nullptr, nullptr);
	return s;
}
static std::string Slashes(std::string s) {
	for (char &c : s)
		if (c == '\\') c = '/';
	return s;
}

// ---- time / process ---------------------------------------------------------------------------------------------------------
static std::atomic<int64_t> g_clockOff{0};
static std::atomic<bool> g_clockHave{false};
static int64_t RawMonotonicNs();
void LinkClockSample(int64_t hostMinusMcNs) {
	const int64_t cur = g_clockOff.load() ;
	// samples are in the adjusted clock's terms: fold them into the raw offset
	const int64_t raw = hostMinusMcNs + cur;
	if (!g_clockHave.load() || raw < cur) g_clockOff.store(raw), g_clockHave.store(true);
}
int64_t MonotonicNs() { return RawMonotonicNs() - g_clockOff.load(); }
static int64_t RawMonotonicNs() {
	static LARGE_INTEGER freq = [] {
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		return f;
	}();
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	const int64_t q = c.QuadPart / freq.QuadPart, r = c.QuadPart % freq.QuadPart;
	return q * 1000000000LL + r * 1000000000LL / freq.QuadPart;  // (no overflow: r < freq)
}
uint32_t ProcessId() { return (uint32_t)GetCurrentProcessId(); }
// Development under Wine/Proton (PCB_LINK_FILE): the mod checks the host pid with ProcessHandle, a Linux pid; Wine's own pid is a Windows one.
// /proc/self/stat read through the Z: drive is the Linux process that runs portal2.exe.
uint32_t LinkHostPid() {
	if (Getenv("PCB_LINK_FILE").empty()) return ProcessId();
	if (FILE *f = std::fopen("Z:/proc/self/stat", "r")) {
		unsigned long pid = 0;
		const int n = std::fscanf(f, "%lu", &pid);
		std::fclose(f);
		if (n == 1 && pid) return (uint32_t)pid;
	}
	return ProcessId();
}
void SleepUs(unsigned us) {
	if (us >= 1000) return Sleep((us + 999) / 1000);
	const int64_t until = MonotonicNs() + (int64_t)us * 1000;  // sub-millisecond waits spin (usleep(1) in a polling loop)
	while (MonotonicNs() < until) SwitchToThread();
}
void TerminateSelf() { TerminateProcess(GetCurrentProcess(), 143); }
std::string ProcessCommandLine(uint32_t) { return std::string(); }  // UNIMPLEMENTED on Windows: the world of the linked Minecraft stays "unknown"

// ---- files ------------------------------------------------------------------------------------------------------------------
static long long FileTimeToUnixNs(const FILETIME &ft) {
	ULARGE_INTEGER u;
	u.LowPart = ft.dwLowDateTime, u.HighPart = ft.dwHighDateTime;
	return ((long long)u.QuadPart - 116444736000000000LL) * 100LL;  // 100 ns units since 1601 -> ns since 1970
}
bool Stat(const std::string &path, FileInfo *out) {
	WIN32_FILE_ATTRIBUTE_DATA d;
	if (!GetFileAttributesExW(Wide(path).c_str(), GetFileExInfoStandard, &d)) return false;
	if (out) {
		const long long ns = FileTimeToUnixNs(d.ftLastWriteTime);
		out->size = ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
		out->mtimeSec = ns / 1000000000LL;
		out->mtimeNs = ns % 1000000000LL;
	}
	return true;
}
bool MakeDir(const std::string &path) { return CreateDirectoryW(Wide(path).c_str(), nullptr) != 0; }
bool MakeDirs(const std::string &path) {
	if (path.empty()) return false;
	std::string cur;
	for (size_t i = 0; i <= path.size(); ++i) {
		if (i == path.size() || path[i] == '/' || path[i] == '\\') {
			if (!cur.empty() && cur.back() != ':') CreateDirectoryW(Wide(cur).c_str(), nullptr);
		}
		if (i < path.size()) cur += path[i];
	}
	return Stat(path);
}
std::string ExecutablePath() {
	wchar_t buf[MAX_PATH * 4];
	DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof buf / sizeof buf[0]));
	return Slashes(Narrow(buf, (int)n));
}
std::string OwnModulePath() {
	HMODULE m = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&OwnModulePath, &m)) return "";
	wchar_t buf[MAX_PATH * 4];
	DWORD n = GetModuleFileNameW(m, buf, (DWORD)(sizeof buf / sizeof buf[0]));
	if (!n || n >= sizeof buf / sizeof buf[0]) return "";
	std::string p = Slashes(Narrow(buf, (int)n));
	if (p.compare(0, 4, "//?/") == 0) p.erase(0, 4);
	return p;
}
std::string Getenv(const char *name) {
	wchar_t buf[2048];
	DWORD n = GetEnvironmentVariableW(Wide(name).c_str(), buf, (DWORD)(sizeof buf / sizeof buf[0]));
	if (!n || n >= sizeof buf / sizeof buf[0]) return "";
	return Narrow(buf, (int)n);
}
std::string HomeOrTemp() {
	std::string h = Getenv("USERPROFILE");
	if (h.empty()) h = Getenv("TEMP");
	if (h.empty()) h = "C:/Windows/Temp";
	return Slashes(h);
}

// ---- memory -----------------------------------------------------------------------------------------------------------------
bool SafeRead(const void *src, void *dst, size_t n) {
	SIZE_T got = 0;
	return src && ReadProcessMemory(GetCurrentProcess(), src, dst, n, &got) && got == n;
}
ProcessStats QueryProcessStats() {
	ProcessStats s;
	PROCESS_MEMORY_COUNTERS_EX pmc;
	pmc.cb = sizeof pmc;
	if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc)) {
		s.rssKb = (long long)(pmc.WorkingSetSize >> 10);
		s.sizeKb = (long long)(pmc.PrivateUsage >> 10);  // private commit, the closest to Linux's VmSize for the 4 GiB question
		s.peakSizeKb = (long long)(pmc.PeakPagefileUsage >> 10);
		s.swapKb = -1;
	}
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snap != INVALID_HANDLE_VALUE) {
		THREADENTRY32 te;
		te.dwSize = sizeof te;
		long long n = 0;
		const DWORD pid = GetCurrentProcessId();
		for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
			if (te.th32OwnerProcessID == pid) ++n;
		CloseHandle(snap);
		s.threads = n;
	}
	return s;
}
std::string AddressSpaceSummary() {
	// the largest free block and the total free address space of this 32-bit process: the 218 MiB link mapping needs one contiguous view
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	uintptr_t p = (uintptr_t)si.lpMinimumApplicationAddress, end = (uintptr_t)si.lpMaximumApplicationAddress;
	size_t largest = 0, total = 0;
	MEMORY_BASIC_INFORMATION mbi;
	while (p < end && VirtualQuery((LPCVOID)p, &mbi, sizeof mbi) == sizeof mbi) {
		if (mbi.State == MEM_FREE) {
			total += mbi.RegionSize;
			if (mbi.RegionSize > largest) largest = mbi.RegionSize;
		}
		const uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
		if (next <= p) break;
		p = next;
	}
	char buf[160];
	std::snprintf(buf, sizeof buf, "address space: user range up to %p, largest free block %u MB, free in total %u MB", (void *)end, (unsigned)(largest >> 20), (unsigned)(total >> 20));
	return buf;
}

// ---- modules ----------------------------------------------------------------------------------------------------------------
bool DescribeAddress(const void *addr, AddressInfo &out) {
	out = AddressInfo();
	MEMORY_BASIC_INFORMATION mbi;
	if (VirtualQuery(addr, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
		out.readable = true;
		out.executable = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
	}
	HMODULE m = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)addr, &m) || !m) return false;
	wchar_t buf[MAX_PATH * 2];
	DWORD n = GetModuleFileNameW(m, buf, (DWORD)(sizeof buf / sizeof buf[0]));
	std::string full = Slashes(Narrow(buf, (int)n));
	const size_t sl = full.find_last_of('/');
	out.module = sl == std::string::npos ? full : full.substr(sl + 1);
	out.base = (uintptr_t)m;
	out.offset = (uintptr_t)addr - out.base;
	return true;
}

void *ModuleSymbol(const char *module, const char *symbol) {
	HMODULE m = GetModuleHandleA((std::string(module) + ".dll").c_str());
	return m ? (void *)GetProcAddress(m, symbol) : nullptr;
}

// ---- input (SDL polling emulation) ------------------------------------------------------------------------------------------
// The Linux code polls SDL (scancode-indexed key array, button mask, focus window, window size). These emulate the polling half with the
// Win32 API, gated on a window of this process being in the foreground. The event half (filter/watch/push: relative mouse motion, the wheel,
// text, key events for Minecraft screens) is input_win.cpp (a message hook on the game window).
namespace {
struct EnumCtx {
	DWORD pid;
	HWND hwnd;
};
BOOL CALLBACK EnumProc(HWND h, LPARAM l) {
	EnumCtx *c = (EnumCtx *)l;
	DWORD pid = 0;
	GetWindowThreadProcessId(h, &pid);
	if (pid != c->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
	wchar_t cls[64] = L"";
	GetClassNameW(h, cls, 64);
	if (!wcscmp(cls, L"Valve001")) {  // Source's main window class (UNVERIFIED for Portal 2)
		c->hwnd = h;
		return FALSE;
	}
	if (!c->hwnd) c->hwnd = h;
	return TRUE;
}
HWND GameWindowNow() {
	EnumCtx ctx = {GetCurrentProcessId(), nullptr};
	EnumWindows(EnumProc, (LPARAM)&ctx);
	return ctx.hwnd;
}
HWND GameWindow() {
	static std::atomic<HWND> cached{nullptr};
	static std::atomic<int64_t> at{0};
	const int64_t now = MonotonicNs();
	HWND h = cached.load();
	if (!h || !IsWindow(h) || now - at.load() > 2000000000LL) {
		h = GameWindowNow();
		cached = h;
		at = now;
	}
	return h;
}
bool GameFocused() {
	HWND fg = GetForegroundWindow();
	if (!fg) return false;
	DWORD pid = 0;
	GetWindowThreadProcessId(fg, &pid);
	return pid == GetCurrentProcessId();
}
// SDL scancodes are physical key positions (USB usage ids); Windows virtual keys follow the keyboard layout. A fixed 'A'..'Z' / '0'..'9'
// table read the wrong physical keys on AZERTY / QWERTZ keyboards (WASD as the Minecraft player, G, H, J), so the letter, digit and
// punctuation positions go through the PC/AT scan code of that position and the current layout (2026-10-12).
static const uint8_t kSdlToSet1[57] = {
	0, 0, 0, 0,                                                                                  // 0-3
	0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19,  // 4-19: a..p
	0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,                                  // 20-29: q..z
	0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,                                  // 30-39: 1..9, 0
	0, 0, 0, 0, 0,                                                                               // 40-44: enter, esc, backspace, tab, space (fixed)
	0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0, 0x27, 0x28, 0x29, 0x33, 0x34, 0x35,                          // 45-56: - = [ ] \ (#) ; ' ` , . /
};
static int LayoutVk(int sc) {
	if (sc < 0 || sc >= 57 || !kSdlToSet1[sc]) return 0;
	static HKL cached = nullptr;
	static int vk[57];
	HKL hkl = GetKeyboardLayout(0);
	if (hkl != cached) {
		cached = hkl;
		for (int i = 0; i < 57; ++i) vk[i] = kSdlToSet1[i] ? (int)MapVirtualKeyExW(kSdlToSet1[i], MAPVK_VSC_TO_VK, hkl) : 0;
	}
	return vk[sc];
}
int Set1ToSc(unsigned scan, bool extended) {  // (WinSet1ToSc) input_win.cpp: a key message's physical scan code -> SDL scancode (0: not a layout key)
	if (extended || !scan) return 0;
	for (int i = 4; i < 57; ++i)
		if (kSdlToSet1[i] == scan) return i;
	return 0;
}
int ScToVk(int sc) {
	if (int v = LayoutVk(sc)) return v;
	if (sc >= 4 && sc <= 29) return 'A' + (sc - 4);
	if (sc >= 30 && sc <= 38) return '1' + (sc - 30);
	if (sc == 39) return '0';
	if (sc >= 58 && sc <= 69) return VK_F1 + (sc - 58);
	if (sc >= 89 && sc <= 97) return VK_NUMPAD1 + (sc - 89);
	switch (sc) {
	case 40: return VK_RETURN;   case 41: return VK_ESCAPE;   case 42: return VK_BACK;     case 43: return VK_TAB;
	case 44: return VK_SPACE;    case 45: return VK_OEM_MINUS; case 46: return VK_OEM_PLUS; case 47: return VK_OEM_4;
	case 48: return VK_OEM_6;    case 49: return VK_OEM_5;    case 51: return VK_OEM_1;     case 52: return VK_OEM_7;
	case 53: return VK_OEM_3;    case 54: return VK_OEM_COMMA; case 55: return VK_OEM_PERIOD; case 56: return VK_OEM_2;
	case 57: return VK_CAPITAL;  case 70: return VK_SNAPSHOT; case 71: return VK_SCROLL;   case 72: return VK_PAUSE;
	case 73: return VK_INSERT;   case 74: return VK_HOME;     case 75: return VK_PRIOR;     case 76: return VK_DELETE;
	case 77: return VK_END;      case 78: return VK_NEXT;     case 79: return VK_RIGHT;     case 80: return VK_LEFT;
	case 81: return VK_DOWN;     case 82: return VK_UP;       case 83: return VK_NUMLOCK;   case 84: return VK_DIVIDE;
	case 85: return VK_MULTIPLY; case 86: return VK_SUBTRACT; case 87: return VK_ADD;      case 98: return VK_NUMPAD0;
	case 99: return VK_DECIMAL;  case 224: return VK_LCONTROL; case 225: return VK_LSHIFT; case 226: return VK_LMENU;
	case 227: return VK_LWIN;    case 228: return VK_RCONTROL; case 229: return VK_RSHIFT; case 230: return VK_RMENU;
	case 231: return VK_RWIN;
	}
	return 0;
}
const uint8_t *Emu_GetKeyboardState(int *numkeys) {
	WinInputPoll();
	static uint8_t keys[512];
	if (numkeys) *numkeys = 512;
	std::memset(keys, 0, sizeof keys);
	if (!GameFocused()) return keys;
	for (int sc = 4; sc < 512; ++sc)
		if (int vk = ScToVk(sc)) keys[sc] = (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
	return keys;
}
uint32_t Emu_GetMouseState(int *x, int *y) {
	POINT p = {0, 0};
	GetCursorPos(&p);
	if (HWND w = GameWindow()) ScreenToClient(w, &p);
	if (x) *x = p.x;
	if (y) *y = p.y;
	if (!GameFocused()) return 0;
	uint32_t b = 0;
	if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) b |= 1;
	if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) b |= 2;
	if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) b |= 4;
	if (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) b |= 8;
	if (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) b |= 16;
	return b;
}
void *Emu_GetKeyboardFocus() { return GameFocused() ? (void *)GameWindow() : nullptr; }
void Emu_GetWindowSize(void *window, int *w, int *h) {
	RECT r = {0, 0, 0, 0};
	if (window) GetClientRect((HWND)window, &r);
	if (w) *w = r.right - r.left;
	if (h) *h = r.bottom - r.top;
}
void *Emu_GetWindowFromID(uint32_t id) { return id == 1 ? (void *)GameWindow() : nullptr; }
uint32_t Emu_GetWindowFlags(void *window) { return window && GameFocused() ? 0x200u : 0u; }  // SDL_WINDOW_INPUT_FOCUS
}  // namespace

void *SdlSymbol(const char *name) {
	if (!std::strcmp(name, "SDL_GetKeyboardState")) return (void *)&Emu_GetKeyboardState;
	if (!std::strcmp(name, "SDL_GetMouseState")) return (void *)&Emu_GetMouseState;
	if (!std::strcmp(name, "SDL_GetKeyboardFocus")) return (void *)&Emu_GetKeyboardFocus;
	if (!std::strcmp(name, "SDL_GetWindowSize")) return (void *)&Emu_GetWindowSize;
	if (!std::strcmp(name, "SDL_GetWindowFromID")) return (void *)&Emu_GetWindowFromID;
	if (!std::strcmp(name, "SDL_GetWindowFlags")) return (void *)&Emu_GetWindowFlags;
	return WinEventSymbol(name);  // SDL_SetEventFilter/GetEventFilter/AddEventWatch/DelEventWatch/PushEvent: input_win.cpp
}

void *WinGameWindow() { return (void *)GameWindow(); }
bool WinGameFocused() { return GameFocused(); }
int WinScToVk(int sc) { return ScToVk(sc); }
int WinSet1ToSc(unsigned scan, bool extended) { return Set1ToSc(scan, extended); }
void InputShutdown() { WinInputShutdown(); }

// ---- shared memory (Local\Portalcraft_v1) -----------------------------------------------------------------------------------
namespace {
HANDLE g_map = nullptr, g_file = INVALID_HANDLE_VALUE;
uint8_t *g_base = nullptr;
bool g_existing = false;

bool Elevated() {
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
	TOKEN_ELEVATION e{};
	DWORD size = 0;
	const bool ok = GetTokenInformation(token, TokenElevation, &e, sizeof e, &size);
	CloseHandle(token);
	return ok && e.TokenIsElevated;
}
// Who may open the mapping: this user, SYSTEM and administrators, at normal integrity, so an elevated Portal 2 and an unelevated Minecraft
// (and the other way round) can both open it. From SkyCraft's Link.cpp (MIT, the user's own project). Free with LocalFree.
PSECURITY_DESCRIPTOR SharedWithThisUser() {
	std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)";
	HANDLE token = nullptr;
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		DWORD size = 0;
		GetTokenInformation(token, TokenUser, nullptr, 0, &size);
		std::vector<uint8_t> buffer(size);
		if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
			LPWSTR sid = nullptr;
			if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &sid)) {
				sddl += std::wstring(L"(A;;GA;;;") + sid + L")";
				LocalFree(sid);
			}
		}
		CloseHandle(token);
	}
	sddl += L"S:(ML;;NW;;;ME)";
	PSECURITY_DESCRIPTOR d = nullptr;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &d, nullptr)) return nullptr;
	return d;
}
std::string Err(const char *what, DWORD code) {
	char buf[160];
	std::snprintf(buf, sizeof buf, "%s failed (Win32 error %lu)", what, (unsigned long)code);
	return buf;
}
}  // namespace

uint8_t *ShmCreate(uint64_t bytes, std::string &what, std::string &err) {
	g_existing = false;
	// Development under Wine/Proton: PCB_LINK_FILE=<path> backs the mapping with a file that a native Linux Minecraft can map too.
	const std::string devFile = Getenv("PCB_LINK_FILE");
	if (!devFile.empty()) {
		g_file = CreateFileW(Wide(devFile).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (g_file == INVALID_HANDLE_VALUE) { err = Err("CreateFileW(PCB_LINK_FILE)", GetLastError()); return nullptr; }
		g_map = CreateFileMappingW(g_file, nullptr, PAGE_READWRITE, (DWORD)(bytes >> 32), (DWORD)(bytes & 0xFFFFFFFFu), nullptr);
		if (!g_map) { err = Err("CreateFileMapping(file)", GetLastError()); CloseHandle(g_file), g_file = INVALID_HANDLE_VALUE; return nullptr; }
		what = devFile + " (PCB_LINK_FILE)";
	} else {
		if (Elevated()) if (g_bridgeLogFile) std::fprintf(g_bridgeLogFile, "[bridge] link: Portal 2 is running as administrator\n");
		SECURITY_ATTRIBUTES sa{sizeof sa, SharedWithThisUser(), FALSE};
		g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, sa.lpSecurityDescriptor ? &sa : nullptr, PAGE_READWRITE, (DWORD)(bytes >> 32), (DWORD)(bytes & 0xFFFFFFFFu),
		                           skycraft::proto::kMappingName);
		const DWORD created = GetLastError();
		if (sa.lpSecurityDescriptor) LocalFree(sa.lpSecurityDescriptor);
		if (!g_map) { err = Err("CreateFileMappingW(Local\\Portalcraft_v1)", created); return nullptr; }
		g_existing = created == ERROR_ALREADY_EXISTS;
		what = "Local\\Portalcraft_v1";
	}
	g_base = (uint8_t *)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
	if (!g_base) {
		const DWORD e = GetLastError();
		err = Err("MapViewOfFile of the whole mapping", e) + " (" + AddressSpaceSummary() + ")";
		CloseHandle(g_map), g_map = nullptr;
		if (g_file != INVALID_HANDLE_VALUE) CloseHandle(g_file), g_file = INVALID_HANDLE_VALUE;
		return nullptr;
	}
	return g_base;
}
bool ShmWasExisting() { return g_existing; }
void ShmDestroy() {
	if (g_base) UnmapViewOfFile(g_base), g_base = nullptr;
	if (g_map) CloseHandle(g_map), g_map = nullptr;
	if (g_file != INVALID_HANDLE_VALUE) CloseHandle(g_file), g_file = INVALID_HANDLE_VALUE;
}

// ---- crash handler ----------------------------------------------------------------------------------------------------------
// A vectored exception handler that logs the faulting address and a stack scan as module+offset, then lets the chain continue (the engine's
// own handlers, Steam's minidump writer). It only logs exceptions that are normally fatal and at most 12 of them. UNVERIFIED.
namespace {
PVOID g_veh = nullptr;
std::atomic<int> g_logged{0};

void LogAddr(void *a) {
	HMODULE m = nullptr;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m) && m) {
		wchar_t buf[MAX_PATH * 2];
		DWORD n = GetModuleFileNameW(m, buf, (DWORD)(sizeof buf / sizeof buf[0]));
		std::string full = Slashes(Narrow(buf, (int)n));
		const size_t sl = full.find_last_of('/');
		std::fprintf(g_bridgeLogFile, "  %p %s+0x%lx\n", a, sl == std::string::npos ? full.c_str() : full.c_str() + sl + 1, (unsigned long)((char *)a - (char *)m));
	} else {
		std::fprintf(g_bridgeLogFile, "  %p ?\n", a);
	}
}
bool Fatalish(DWORD c) {
	return c == EXCEPTION_ACCESS_VIOLATION || c == EXCEPTION_ILLEGAL_INSTRUCTION || c == EXCEPTION_PRIV_INSTRUCTION || c == EXCEPTION_INT_DIVIDE_BY_ZERO ||
	       c == EXCEPTION_STACK_OVERFLOW || c == EXCEPTION_IN_PAGE_ERROR || c == EXCEPTION_ARRAY_BOUNDS_EXCEEDED || c == EXCEPTION_BREAKPOINT;
}
LONG CALLBACK Veh(EXCEPTION_POINTERS *ep) {
	if (!g_bridgeLogFile || !ep || !ep->ExceptionRecord || !Fatalish(ep->ExceptionRecord->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
	if (g_logged.fetch_add(1) >= 12) return EXCEPTION_CONTINUE_SEARCH;
	const EXCEPTION_RECORD *er = ep->ExceptionRecord;
	std::fprintf(g_bridgeLogFile, "EXCEPTION 0x%08lx at %p (thread %lu), first chance; access %s address %p\n", (unsigned long)er->ExceptionCode, er->ExceptionAddress,
	             (unsigned long)GetCurrentThreadId(), er->NumberParameters >= 1 ? (er->ExceptionInformation[0] == 0 ? "read" : er->ExceptionInformation[0] == 1 ? "write" : "execute") : "-",
	             er->NumberParameters >= 2 ? (void *)er->ExceptionInformation[1] : nullptr);
	LogAddr(er->ExceptionAddress);
	if (ep->ContextRecord) {
		const CONTEXT *c = ep->ContextRecord;
		std::fprintf(g_bridgeLogFile, "  eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx\n", (unsigned long)c->Eax, (unsigned long)c->Ebx, (unsigned long)c->Ecx,
		             (unsigned long)c->Edx, (unsigned long)c->Esi, (unsigned long)c->Edi, (unsigned long)c->Ebp, (unsigned long)c->Esp);
		std::fprintf(g_bridgeLogFile, "stack scan (words that point into a loaded module):\n");
		const void *const *sp = (const void *const *)c->Esp;
		for (int i = 0, found = 0; i < 512 && found < 24; ++i) {
			void *w = nullptr;
			if (!SafeRead(sp + i, &w, sizeof w)) break;
			HMODULE m = nullptr;
			if (w && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)w, &m) && m) LogAddr(w), ++found;
		}
	}
	std::fflush(g_bridgeLogFile);
	return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

void CrashHandlerInstall() {
	if (!g_veh) g_veh = AddVectoredExceptionHandler(1, Veh);
}
void CrashHandlerRemove() {
	if (g_veh) RemoveVectoredExceptionHandler(g_veh), g_veh = nullptr;
}

}  // namespace bridge::plat

// ---- UTF-8 aware C file functions (WINDOWS-VERIFY F6) --------------------------------------------------------------------------
// The bridge builds UTF-8 paths (CacheDir etc.) and calls fopen/stat/remove/rename with them everywhere, including libstdc++'s file streams.
// msvcrt's narrow versions read the ANSI code page, so a data folder with non-ASCII characters in it could not be opened. The link line has
// -Wl,--wrap=fopen,--wrap=stat,--wrap=remove,--wrap=rename: every reference lands here, and a path that is not plain ASCII goes through the
// wide-character function instead.
extern "C" {
FILE *__real_fopen(const char *path, const char *mode);
int __real_stat(const char *path, struct _stat32 *st);
int __real_remove(const char *path);
int __real_rename(const char *from, const char *to);
}
static bool PlainAscii(const char *s) {
	for (; s && *s; ++s)
		if ((unsigned char)*s >= 0x80) return false;
	return true;
}
static std::wstring WideUtf8(const char *s) {
	if (!s) return std::wstring();
	return bridge::plat::WidenUtf8(s);
}
extern "C" FILE *__wrap_fopen(const char *path, const char *mode) {
	if (PlainAscii(path)) return __real_fopen(path, mode);
	return _wfopen(WideUtf8(path).c_str(), WideUtf8(mode).c_str());
}
extern "C" int __wrap_stat(const char *path, struct _stat32 *st) {
	if (PlainAscii(path)) return __real_stat(path, st);
	return _wstat32(WideUtf8(path).c_str(), st);
}
extern "C" int __wrap_remove(const char *path) {
	if (PlainAscii(path)) return __real_remove(path);
	return _wremove(WideUtf8(path).c_str());
}
// rename replaces an existing destination on Linux (the caches write <file>.tmp and rename it over <file>); Windows' rename refuses, so
// every cache was written once and never updated (the collision cache lost each session's harvest, 2026-10-12): MoveFileExW replaces it.
extern "C" int __wrap_rename(const char *from, const char *to) {
	(void)__real_rename;
	if (MoveFileExW(WideUtf8(from).c_str(), WideUtf8(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) return 0;
	errno = GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND ? ENOENT : EACCES;
	return -1;
}
#endif  // _WIN32
