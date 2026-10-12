// Portalcraft bridge: the Linux implementation of platform.hpp. The code here used to live in the bridge files named at each
// function and was moved unchanged.
#ifndef _WIN32
#include "platform.hpp"

#include "skycraft_protocol.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>
#include <time.h>

extern FILE *g_bridgeLogFile;
namespace bridge { std::string RepoRoot(); }

namespace bridge::plat {

int64_t MonotonicNs() {  // was link.cpp NowNs
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
uint32_t ProcessId() { return (uint32_t)getpid(); }
uint32_t LinkHostPid() { return ProcessId(); }
void SleepUs(unsigned us) { usleep(us); }
void TerminateSelf() { kill(getpid(), SIGTERM); }  // was script.cpp's watchdog

std::string ProcessCommandLine(uint32_t pid) {  // was unified.cpp
	char path[64];
	std::snprintf(path, sizeof path, "/proc/%u/cmdline", pid);
	std::string cmd;
	if (FILE *f = std::fopen(path, "rb")) {
		char buf[4096];
		size_t n;
		while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) cmd.append(buf, n);
		std::fclose(f);
	}
	return cmd;
}

bool Stat(const std::string &path, FileInfo *out) {
	struct stat st;
	if (::stat(path.c_str(), &st) != 0) return false;
	if (out) {
		out->size = (long long)st.st_size;
		out->mtimeSec = (long long)st.st_mtim.tv_sec;
		out->mtimeNs = (long long)st.st_mtim.tv_nsec;
	}
	return true;
}
bool MakeDir(const std::string &path) { return ::mkdir(path.c_str(), 0755) == 0; }
bool MakeDirs(const std::string &path) {
	if (path.empty()) return false;
	std::string cur;
	for (size_t i = 0; i <= path.size(); ++i) {
		if (i == path.size() || path[i] == '/') {
			if (!cur.empty() && cur != "/") ::mkdir(cur.c_str(), 0755);
		}
		if (i < path.size()) cur += path[i];
	}
	struct stat st;
	return ::stat(path.c_str(), &st) == 0;
}
std::string ExecutablePath() {  // was mapindex.cpp / collfield.cpp
	char exe[1024] = "";
	ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
	exe[n > 0 ? n : 0] = 0;
	return exe;
}
std::string OwnModulePath() {  // was plugin.cpp RepoRoot
	Dl_info info;
	if (dladdr((void *)&OwnModulePath, &info) && info.dli_fname) return info.dli_fname;
	return "";
}
std::string HomeOrTemp() {
	const char *home = std::getenv("HOME");
	return home ? home : "/tmp";
}
std::string Getenv(const char *name) {
	const char *v = std::getenv(name);
	return v ? v : "";
}

bool SafeRead(const void *src, void *dst, size_t n) {  // was visclamp.cpp ReadableVec
	struct iovec local = {dst, n}, remote = {const_cast<void *>(src), n};
	return src && process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)n;
}

static long long ProcField(const char *file, const char *key) {  // was growth.cpp
	FILE *f = std::fopen(file, "r");
	if (!f) return -1;
	char line[256];
	long long v = -1;
	const size_t kl = std::strlen(key);
	while (std::fgets(line, sizeof line, f))
		if (!std::strncmp(line, key, kl)) {
			v = std::atoll(line + kl);
			break;
		}
	std::fclose(f);
	return v;
}
ProcessStats QueryProcessStats() {
	ProcessStats s;
	s.rssKb = ProcField("/proc/self/status", "VmRSS:");
	s.sizeKb = ProcField("/proc/self/status", "VmSize:");
	s.peakSizeKb = ProcField("/proc/self/status", "VmPeak:");
	s.swapKb = ProcField("/proc/self/status", "VmSwap:");
	s.threads = ProcField("/proc/self/status", "Threads:");
	struct statvfs sv;
	if (!statvfs("/dev/shm", &sv)) s.shmUsedMb = (long long)((sv.f_blocks - sv.f_bfree) * (unsigned long long)sv.f_frsize >> 20);
	return s;
}

std::string AddressSpaceSummary() { return std::string(); }

bool DescribeAddress(const void *addr, AddressInfo &out) {
	out = AddressInfo();
	uint8_t probe = 0;
	out.readable = SafeRead(addr, &probe, 1);
	// executable: the /proc/self/maps line that contains the address
	if (FILE *f = std::fopen("/proc/self/maps", "r")) {
		char line[512];
		while (std::fgets(line, sizeof line, f)) {
			unsigned long lo = 0, hi = 0;
			char perms[8] = {};
			if (std::sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) == 3 && (uintptr_t)addr >= lo && (uintptr_t)addr < hi) {
				out.executable = perms[2] == 'x';
				break;
			}
		}
		std::fclose(f);
	}
	Dl_info di;
	if (!dladdr(addr, &di) || !di.dli_fname) return false;
	std::string full = di.dli_fname;
	const size_t sl = full.find_last_of('/');
	out.module = sl == std::string::npos ? full : full.substr(sl + 1);
	out.base = (uintptr_t)di.dli_fbase;
	out.offset = (uintptr_t)addr - out.base;
	return true;
}

void InputShutdown() {}

void *ModuleSymbol(const char *module, const char *symbol) {  // was conlog.cpp
	std::string lib = std::string("lib") + module + ".so";
	void *h = dlopen(lib.c_str(), RTLD_NOW | RTLD_NOLOAD);
	if (!h) return nullptr;
	void *p = dlsym(h, symbol);
	dlclose(h);
	return p;
}

void *SdlSymbol(const char *name) {  // was mc.cpp / bugmark.cpp
	void *h = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_NOLOAD);
	if (!h) h = RTLD_DEFAULT;
	return dlsym(h, name);
}

// ---- shared memory (was link.cpp) -------------------------------------------------------------------------------------------
static uint8_t *g_shmBase = nullptr;
static uint64_t g_shmBytes = 0;
static int g_shmFd = -1;

uint8_t *ShmCreate(uint64_t bytes, std::string &what, std::string &err) {
	char buf[256];
	const char *name = skycraft::proto::kShmName;
	// A fresh object every run: an old Minecraft still mapping the previous one sees our new
	// file (different inode) and relinks.
	shm_unlink(name);
	g_shmFd = shm_open(name, O_CREAT | O_RDWR, 0600);
	if (g_shmFd < 0) {
		std::snprintf(buf, sizeof buf, "shm_open(%s) failed: %s", name, strerror(errno));
		err = buf;
		return nullptr;
	}
	if (ftruncate(g_shmFd, (off_t)bytes) != 0) {
		std::snprintf(buf, sizeof buf, "ftruncate(%llu) failed: %s", (unsigned long long)bytes, strerror(errno));
		err = buf;
		close(g_shmFd), g_shmFd = -1;
		return nullptr;
	}
	void *p = mmap(nullptr, (size_t)bytes, PROT_READ | PROT_WRITE, MAP_SHARED, g_shmFd, 0);
	if (p == MAP_FAILED) {
		std::snprintf(buf, sizeof buf, "mmap of %llu MB failed: %s", (unsigned long long)(bytes >> 20), strerror(errno));
		err = buf;
		close(g_shmFd), g_shmFd = -1;
		return nullptr;
	}
	g_shmBase = (uint8_t *)p;
	g_shmBytes = bytes;
	what = std::string("/dev/shm") + name;
	return g_shmBase;
}
bool ShmWasExisting() { return false; }
void ShmDestroy() {
	if (!g_shmBase) return;
	munmap(g_shmBase, (size_t)g_shmBytes);
	g_shmBase = nullptr;
	close(g_shmFd), g_shmFd = -1;
	shm_unlink(skycraft::proto::kShmName);
}

// ---- crash handler (was crashlog.cpp) ---------------------------------------------------------------------------------------
static struct sigaction g_old[32];
static const int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};

static void LogAddr(void *a) {
	Dl_info info;
	if (dladdr(a, &info) && info.dli_fname) {
		const char *base = std::strrchr(info.dli_fname, '/');
		std::fprintf(g_bridgeLogFile, "  %p %s+0x%lx %s\n", a, base ? base + 1 : info.dli_fname, (unsigned long)((char *)a - (char *)info.dli_fbase),
		             info.dli_sname ? info.dli_sname : "");
	} else {
		std::fprintf(g_bridgeLogFile, "  %p ?\n", a);
	}
}

static void Handler(int sig, siginfo_t *si, void *uctx) {
	if (g_bridgeLogFile) {
		auto *uc = (ucontext_t *)uctx;
		void *eip = (void *)uc->uc_mcontext.gregs[REG_EIP];
		std::fprintf(g_bridgeLogFile, "CRASH signal %d, fault address %p, eip:\n", sig, si ? si->si_addr : nullptr);
		LogAddr(eip);
		void *frames[48];
		int n = backtrace(frames, 48);
		std::fprintf(g_bridgeLogFile, "backtrace (%d):\n", n);
		for (int i = 0; i < n; ++i) LogAddr(frames[i]);
		// raw stack words that look like code addresses (frame pointers are often omitted)
		std::fprintf(g_bridgeLogFile, "stack scan:\n");
		auto *sp = (void **)uc->uc_mcontext.gregs[REG_ESP];
		for (int i = 0, found = 0; i < 512 && found < 24; ++i) {
			Dl_info info;
			if (dladdr(sp[i], &info) && info.dli_fname && std::strstr(info.dli_fname, ".so")) {
				LogAddr(sp[i]);
				++found;
			}
		}
		std::fflush(g_bridgeLogFile);
	}
	// (bridge #14: exit at once. The previous handler, Steam's crash reporter, could leave the window hanging forever,
	// holding the test lock and the user's screen; the backtrace is in bridge.log)
	if (g_bridgeLogFile) std::fprintf(g_bridgeLogFile, "[bridge] crash: exiting the game (signal %d)\n", sig), std::fflush(g_bridgeLogFile);
	_exit(128 + sig);
}

void CrashHandlerInstall() {
	for (int s : kSignals) {
		struct sigaction sa;
		std::memset(&sa, 0, sizeof sa);
		sa.sa_sigaction = Handler;
		sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
		sigemptyset(&sa.sa_mask);
		sigaction(s, &sa, &g_old[s]);
	}
}

void CrashHandlerRemove() {
	for (int s : kSignals) sigaction(s, &g_old[s], nullptr);
}

}  // namespace bridge::plat
#endif  // !_WIN32
