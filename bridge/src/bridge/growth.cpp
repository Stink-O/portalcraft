// Portalcraft bridge (bridge #53): the periodic "growth" line (see growth.hpp). Command: pcb_growth [now | <seconds> | off].
#include "growth.hpp"

#include "bridge.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "platform/platform.hpp"

#include "Modules/Tier1.hpp"

extern unsigned long long g_logLines, g_logBytes, g_logDropped;  // Modules.cpp

namespace bridge {

uint64_t McFrameCounter();                                   // mc.cpp: Minecraft's frame counter (its fps = the difference over the period)
void FocusCounts(uint64_t &focused, uint64_t &unfocused);    // bugmark.cpp

namespace {
struct Gauge {
	const char *name;
	GaugeFn fn;
	long long first = -1, last = -1;
	int streak = 0;  // reports in a row in which it rose
};
const int kMaxGauges = 160;
Gauge *Gauges() {
	static Gauge g[kMaxGauges];
	return g;
}
int &GaugeCount() {
	static int n = 0;
	return n;
}
double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// this period's frames: 0.25 ms bins to 100 ms, the last bin takes everything longer (frames over 1 s are loading, kept out)
uint32_t g_hist[401];
uint64_t g_n = 0, g_over20 = 0;
double g_max = 0, g_sum = 0;
double g_period = 60.0;
double g_t0 = 0, g_lastReport = 0, g_start = 0;
uint64_t g_lastMcFrames = 0, g_lastFocus = 0, g_lastUnfocus = 0, g_lastLines = 0, g_lastBytes = 0, g_lastDrop = 0;
int g_reports = 0;

double Pct(double q) {
	if (!g_n) return 0;
	uint64_t want = (uint64_t)(q * g_n), acc = 0;
	for (int i = 0; i <= 400; ++i)
		if ((acc += g_hist[i]) > want) return (i + 0.5) * 0.25;
	return 100.0;
}
void Report(bool forced) {
	const double now = Now();
	const double span = now - g_lastReport;
	if (g_start == 0) g_start = g_lastReport;
	uint64_t foc = 0, unf = 0;
	FocusCounts(foc, unf);
	const uint64_t dF = foc - g_lastFocus, dU = unf - g_lastUnfocus;
	g_lastFocus = foc, g_lastUnfocus = unf;
	const uint64_t mcNow = McFrameCounter();
	const double mcFps = span > 0 && g_lastMcFrames && mcNow >= g_lastMcFrames ? (mcNow - g_lastMcFrames) / span : 0.0;
	g_lastMcFrames = mcNow;
	const plat::ProcessStats ps = plat::QueryProcessStats();  // Linux: /proc/self/status and /dev/shm; Windows: working set, private bytes, thread count
	const long long vmRss = ps.rssKb, vmSize = ps.sizeKb, vmSwap = ps.swapKb, threads = ps.threads, shmMb = ps.shmUsedMb;
	console->Print(
	    "[bridge] growth: t+%.0f s, %llu frames in %.0f s (%.0f fps; Minecraft %.0f fps), frame ms p50 %.2f p95 %.2f p99 %.2f max %.1f mean %.2f, %llu over 20 ms, focus %llu/%llu frames; "
	    "VmRSS %lld MB, VmSize %lld MB, VmSwap %lld MB, threads %lld, /dev/shm used %lld MB; log %llu lines %llu KB this period, %llu dropped by the limiter%s\n",
	    now - g_start, (unsigned long long)g_n, span, span > 0 ? g_n / span : 0.0, mcFps, Pct(0.5), Pct(0.95), Pct(0.99), g_max * 1000.0, g_n ? 1000.0 * g_sum / g_n : 0.0, (unsigned long long)g_over20,
	    (unsigned long long)dF, (unsigned long long)(dF + dU), vmRss >= 0 ? vmRss / 1024 : -1, vmSize >= 0 ? vmSize / 1024 : -1, vmSwap >= 0 ? vmSwap / 1024 : -1, threads, shmMb,
	    g_logLines - g_lastLines, (g_logBytes - g_lastBytes) >> 10, g_logDropped - g_lastDrop, forced ? " (asked)" : "");
	g_lastLines = g_logLines, g_lastBytes = g_logBytes, g_lastDrop = g_logDropped;
	// the gauges: name=value, +delta since the first report; those that rose in 3 reports in a row are named at the end
	std::string line = "[bridge] growth sizes:", rising;
	Gauge *g = Gauges();
	for (int i = 0; i < GaugeCount(); ++i) {
		const long long v = (long long)g[i].fn();
		if (g[i].first < 0) g[i].first = v;
		if (g[i].last >= 0) g[i].streak = v > g[i].last ? g[i].streak + 1 : 0;
		g[i].last = v;
		char buf[96];
		if (v != g[i].first) std::snprintf(buf, sizeof buf, " %s=%lld(%+lld)", g[i].name, v, v - g[i].first);
		else std::snprintf(buf, sizeof buf, " %s=%lld", g[i].name, v);
		if (line.size() + std::strlen(buf) > 1500) console->Print("%s\n", line.c_str()), line = "[bridge] growth sizes:";
		line += buf;
		if (g[i].streak >= 3) rising += std::string(" ") + g[i].name;
	}
	console->Print("%s\n", line.c_str());
	if (!rising.empty()) console->Print("[bridge] growth: RISING in 3 reports in a row:%s\n", rising.c_str());
	++g_reports;
	std::memset(g_hist, 0, sizeof g_hist);
	g_n = g_over20 = 0, g_max = g_sum = 0;
	g_lastReport = now;
}
}  // namespace

void GrowthRegister(const char *name, GaugeFn fn) {
	if (GaugeCount() >= kMaxGauges) return;
	Gauges()[GaugeCount()++] = Gauge{name, fn};
}

void GrowthDt(double dt) {
	if (dt >= 1.0) return;
	++g_hist[std::min(400, (int)(dt * 4000.0))];
	++g_n, g_sum += dt;
	if (dt > g_max) g_max = dt;
	if (dt > 0.020) ++g_over20;
}

// (bridge #61) A new level: the "+N since the first report" deltas and the RISING streaks start over (a level's own start-up fill is not a leak).
void GrowthLevelReset() {
	Gauge *g = Gauges();
	for (int i = 0; i < GaugeCount(); ++i) g[i].first = g[i].last = -1, g[i].streak = 0;
}

void GrowthFrame() {
	if (g_period <= 0) return;
	const double now = Now();
	if (g_lastReport == 0) {
		g_lastReport = g_start = now;
		return;
	}
	if (now - g_lastReport >= g_period) Report(false);
}

CON_COMMAND(pcb_growth, "pcb_growth [now | <seconds> | off] - (bridge #53) the periodic growth line: sizes of the queues and caches, memory, frame-time percentiles (default every 60 s)\n") {
	if (args.ArgC() > 1) {
		if (!std::strcmp(args[1], "now")) {
			if (g_lastReport == 0) g_lastReport = g_start = Now();
			Report(true);
			return;
		}
		if (!std::strcmp(args[1], "off")) g_period = 0;
		else if (std::atof(args[1]) >= 1.0) g_period = std::atof(args[1]);
	}
	console->Print("[bridge] growth: every %.0f s (0 = off), %d gauges, %d reports so far\n", g_period, GaugeCount(), g_reports);
}

}  // namespace bridge
