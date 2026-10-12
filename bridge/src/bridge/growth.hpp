// Portalcraft bridge (bridge #53): the "growth" log line. Every 60 s one line (a few, when long) names the sizes of every queue and cache
// that could grow with the length of a session, the process's memory, the frame-time percentiles of that minute and how many of its frames
// had no window focus, so a long session shows what grows and when frames slow down. A gauge is one line in the file that owns the
// container:   PCB_GAUGE("terrain.sections", g_sections.size());
// Cheap by construction: gauges are read once a minute, on the client frame thread; the per-frame cost is one histogram increment.
#pragma once
#include <cstddef>

namespace bridge {
using GaugeFn = size_t (*)();
void GrowthRegister(const char *name, GaugeFn fn);
struct GrowthReg {
	GrowthReg(const char *n, GaugeFn f) { GrowthRegister(n, f); }
};
void GrowthDt(double dtSeconds);  // composite.cpp CountFrame: one host frame ended after dtSeconds
void GrowthFrame();               // mc.cpp Frame(): prints when a period has passed
void GrowthLevelReset();          // (bridge #61) plugin.cpp LevelInit
}  // namespace bridge
#define PCB_GAUGE_CAT2(a, b) a##b
#define PCB_GAUGE_CAT(a, b) PCB_GAUGE_CAT2(a, b)
#define PCB_GAUGE(name, expr) static ::bridge::GrowthReg PCB_GAUGE_CAT(pcb_gauge_, __LINE__)(name, []() -> size_t { return (size_t)(expr); })
