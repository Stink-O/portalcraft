// Portalcraft bridge (bridge #25): frame attribution for the frames over 20 ms (see prof.hpp).
#include "bridge.hpp"
#include "prof.hpp"

#include "Modules/Tier1.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bridge {

namespace {
struct Slot {
	const char *n = nullptr;
	double frame = 0, spikeSum = 0, allSum = 0, worst = 0;
	int spikeTop = 0;  // frames over 20 ms where this was the largest top-level scope
	int calls = 0;
};
const int kMax = 64;
Slot g_s[kMax];
int g_n = 0;
uint64_t g_frames = 0, g_spikes = 0, g_spikeLogs = 0;
double g_spikeMs = 0, g_spikeOurs = 0;
bool g_on = true;
int g_logMax = 60;
bool Child(const char *n) { return std::strchr(n, '.') != nullptr; }
}  // namespace

void ProfAdd(const char *name, double ms) {
	if (!g_on) return;
	for (int i = 0; i < g_n; ++i)
		if (g_s[i].n == name || !std::strcmp(g_s[i].n, name)) {
			g_s[i].frame += ms, ++g_s[i].calls;
			return;
		}
	if (g_n >= kMax) return;
	g_s[g_n].n = name, g_s[g_n].frame = ms, g_s[g_n].calls = 1;
	++g_n;
}

// composite.cpp CountFrame: one host frame ended after dtMs; everything timed since the last call was in it
void ProfFrameEnd(double dtMs) {
	if (!g_on) return;
	++g_frames;
	double ours = 0;
	for (int i = 0; i < g_n; ++i) {
		g_s[i].allSum += g_s[i].frame;
		g_s[i].worst = std::max(g_s[i].worst, g_s[i].frame);
		if (!Child(g_s[i].n)) ours += g_s[i].frame;
	}
	if (dtMs > 20.0) {
		++g_spikes, g_spikeMs += dtMs, g_spikeOurs += ours;
		int order[kMax];
		for (int i = 0; i < g_n; ++i) order[i] = i;
		std::sort(order, order + g_n, [](int a, int b) { return g_s[a].frame > g_s[b].frame; });
		int top = -1;
		for (int k = 0; k < g_n && top < 0; ++k)
			if (!Child(g_s[order[k]].n)) top = order[k];
		if (top >= 0 && g_s[top].frame > 0.5) ++g_s[top].spikeTop;
		for (int i = 0; i < g_n; ++i) g_s[i].spikeSum += g_s[i].frame;
		// (bridge #53) after the first g_logMax spikes, one more every 10 s: a long session shows how its frames slow down, not only the first minute
		static double lastLogT = 0;
		const double nowT = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		if (g_spikeLogs < (uint64_t)g_logMax || nowT - lastLogT > 10.0) {
			lastLogT = nowT;
			++g_spikeLogs;
			char buf[512];
			int len = std::snprintf(buf, sizeof buf, "[bridge] spike %.1f ms (frame %llu): ours %.1f ms:", dtMs, (unsigned long long)g_frames, ours);
			for (int k = 0; k < g_n && k < 5 && g_s[order[k]].frame > 0.3 && len < (int)sizeof buf - 40; ++k)
				len += std::snprintf(buf + len, sizeof buf - len, " %s %.1f (x%d)", g_s[order[k]].n, g_s[order[k]].frame, g_s[order[k]].calls);
			console->Print("%s\n", buf);
		}
	}
	for (int i = 0; i < g_n; ++i) g_s[i].frame = 0, g_s[i].calls = 0;
}

void ProfRunReport() {
	if (!g_frames) return;
	int order[kMax];
	for (int i = 0; i < g_n; ++i) order[i] = i;
	std::sort(order, order + g_n, [](int a, int b) { return g_s[a].spikeSum > g_s[b].spikeSum; });
	console->Print("[bridge] RUN PROF: %llu frames, %llu over 20 ms (mean %.1f ms; ours %.1f ms of it); per scope: ms in those frames / frames it led / worst frame ms / mean ms per frame\n",
	               (unsigned long long)g_frames, (unsigned long long)g_spikes, g_spikes ? g_spikeMs / g_spikes : 0.0, g_spikes ? g_spikeOurs / g_spikes : 0.0);
	for (int k = 0; k < g_n && k < 24; ++k) {
		const Slot &s = g_s[order[k]];
		console->Print("[bridge]   %-22s %8.1f  %4d  %6.1f  %6.3f\n", s.n, s.spikeSum, s.spikeTop, s.worst, s.allSum / g_frames);
	}
}

CON_COMMAND(pcb_prof, "pcb_prof [0|1|reset|print] [log spikes] - (bridge #25) frame attribution: scoped timers, spikes over 20 ms logged with their top scopes\n") {
	if (args.ArgC() > 1) {
		if (!std::strcmp(args[1], "reset")) {
			for (int i = 0; i < g_n; ++i) g_s[i] = Slot{g_s[i].n};
			g_frames = g_spikes = g_spikeLogs = 0, g_spikeMs = g_spikeOurs = 0;
		} else if (!std::strcmp(args[1], "print")) return ProfRunReport();
		else g_on = std::atoi(args[1]) != 0;
	}
	if (args.ArgC() > 2) g_logMax = std::atoi(args[2]);
	console->Print("[bridge] prof %d, %llu frames, %llu spikes, %d scopes\n", g_on, (unsigned long long)g_frames, (unsigned long long)g_spikes, g_n);
}

}  // namespace bridge
