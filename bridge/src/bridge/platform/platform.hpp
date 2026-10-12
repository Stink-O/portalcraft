// Portalcraft bridge: the operating-system layer. Everything the bridge needs from the OS that is not portable C++ goes through
// here, so the rest of the plugin has no #ifdef _WIN32 and no POSIX/Win32 headers (PLAN-windows.md section 4.3).
//   platform_linux.cpp : the Linux implementation (the code that used to be spread over the bridge files, moved here unchanged)
//   platform_win.cpp   : the Windows implementation (MinGW-w64, 32-bit)
// Declarations only; each function says what it does on both systems.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace bridge::plat {

// ---- time / process -------------------------------------------------------------------------------------------------------
int64_t MonotonicNs();                    // Linux CLOCK_MONOTONIC; Windows QueryPerformanceCounter (same origin as Java's System.nanoTime())
uint32_t ProcessId();                     // getpid / GetCurrentProcessId
#ifdef _WIN32
// PCB_LINK_FILE dev mode under Wine: Wine's QueryPerformanceCounter and a native Minecraft's CLOCK_MONOTONIC have different epochs. The link feeds
// (host clock - Minecraft's heartbeat) samples here; the smallest is the epoch difference and MonotonicNs() subtracts it. Real Windows: never called.
void LinkClockSample(int64_t hostMinusMcNs);
#endif
uint32_t LinkHostPid();                   // the pid written into the link header (Windows with PCB_LINK_FILE under Wine: the Linux pid, which a native Minecraft can check)
void SleepUs(unsigned us);                // usleep / Sleep (rounded up to the millisecond on Windows)
void TerminateSelf();                     // (script watchdog) SIGTERM to ourselves / TerminateProcess
// The command line of another process as raw bytes (arguments NUL-separated on Linux); empty when it is not readable.
// Windows: not readable from a 32-bit process without NtQueryInformationProcess; returns empty.
std::string ProcessCommandLine(uint32_t pid);

// ---- files ----------------------------------------------------------------------------------------------------------------
struct FileInfo {
	long long size = 0;
	long long mtimeSec = 0;
	long long mtimeNs = 0;  // nanoseconds part (0 where the file system has no finer stamp)
};
bool Stat(const std::string &path, FileInfo *out = nullptr);  // false when the path does not exist (file or directory)
bool MakeDir(const std::string &path);                        // one level, mode 0755; false if it exists or fails (callers ignore the result)
bool MakeDirs(const std::string &path);                       // mkdir -p
std::string ExecutablePath();                                 // /proc/self/exe / the .exe of this process (UTF-8)
std::string OwnModulePath();                                  // the path of the plugin file itself (dladdr / GetModuleFileNameW), UTF-8
std::string HomeOrTemp();                                     // $HOME else /tmp; Windows: %USERPROFILE% else %TEMP% (UTF-8, forward slashes)
std::string Getenv(const char *name);                         // "" when unset

// ---- memory ---------------------------------------------------------------------------------------------------------------
// Copies n bytes from src if the whole range is readable; no fault on a bad guess.
bool SafeRead(const void *src, void *dst, size_t n);

struct ProcessStats {
	long long rssKb = -1, sizeKb = -1, peakSizeKb = -1, swapKb = -1, threads = -1;
	long long shmUsedMb = -1;  // used space of /dev/shm; -1 on Windows
};
ProcessStats QueryProcessStats();

// A line describing the free address space of the process (largest free block); "" on Linux. Logged at load on Windows (32-bit).
std::string AddressSpaceSummary();

// ---- modules --------------------------------------------------------------------------------------------------------------
// A function exported by a module that is already loaded in the process (never loads one). `module` is the bare name,
// e.g. "tier0" (libtier0.so / tier0.dll). nullptr when absent.
void *ModuleSymbol(const char *module, const char *symbol);

// ---- input --------------------------------------------------------------------------------------------------------------
// The bridge reads keyboard/mouse state through the SDL2 function set Portal 2 uses on Linux (SDL scancodes, SDL button masks,
// SDL event layouts). SdlSymbol resolves one such function: on Linux from the already-loaded libSDL2 (else the global scope), on
// Windows from a small emulation in platform_win.cpp (GetAsyncKeyState gated on the foreground window) for the polling functions
// and nullptr for the event-filter/watch/push functions (those need the window-procedure hook of phase 2). nullptr when absent.
void *SdlSymbol(const char *name);

// ---- shared-memory link (the host end of protocol/skycraft_protocol.h) -----------------------------------------------------
// Creates a fresh object (the previous one is unlinked/replaced so an old Minecraft relinks), maps `bytes` of it read/write.
// On success returns the base; on failure nullptr and `err` says why. `what` receives a description for the log line
// ("/dev/shm/Portalcraft_v1" or "Local\Portalcraft_v1").
uint8_t *ShmCreate(uint64_t bytes, std::string &what, std::string &err);
// True when a mapping with this name already existed (Windows: ERROR_ALREADY_EXISTS; Linux: always false, the old object is unlinked).
bool ShmWasExisting();
void ShmDestroy();

// ---- address description (pcb_probe_slots) ---------------------------------------------------------------------------------
struct AddressInfo {
	std::string module;       // bare file name of the module (library) containing the address; "" when it is in none
	uintptr_t base = 0;       // load address of that module
	uintptr_t offset = 0;     // address - base
	bool readable = false;    // the page(s) at the address are readable
	bool executable = false;  // ... and executable
};
// Describes where an address lies. Returns false when it lies in no module (the other fields are still filled where known).
bool DescribeAddress(const void *addr, AddressInfo &out);

// ---- input events (the event half of the SDL set above) ------------------------------------------------------------------------
// Windows: SdlSymbol also resolves SDL_SetEventFilter/GetEventFilter/AddEventWatch/DelEventWatch/PushEvent, backed by a message hook on
// Portal 2's window that turns Win32 input messages into SDL_Event structs (platform/input_win.cpp). Linux: libSDL2 itself.
// Removes the hook and forgets the filter/watchers (plugin unload; no-op on Linux).
void InputShutdown();

// ---- crash handler (writes to g_bridgeLogFile, then ends the process) ------------------------------------------------------
void CrashHandlerInstall();
void CrashHandlerRemove();

#ifdef _WIN32
void WinKeepActiveForTests();  // PCB_KEEP_ACTIVE=1 (test harness only): WM_ACTIVATEAPP(FALSE) passed on as TRUE, no 50 ms unfocused sleep (input_win.cpp)
#endif
}  // namespace bridge::plat
