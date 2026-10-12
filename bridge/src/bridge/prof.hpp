// Portalcraft bridge (bridge #25): scoped frame-attribution timers (the fresh-eyes review's quick win 1).
// PROF("name") times the rest of the enclosing block into this host frame's slot "name"; a frame over 20 ms logs its top
// contributors ("[bridge] spike ..."), and the run's totals print with RUN PERF ("[bridge] RUN PROF ..."). Names with a '.'
// are parts of the name before it (colfield.dyn is inside colfield) and are not added to "ours".
#pragma once
#include <chrono>

namespace bridge {
void ProfAdd(const char *name, double ms);
struct ProfScope {
	const char *n;
	std::chrono::steady_clock::time_point t0;
	explicit ProfScope(const char *name) : n(name), t0(std::chrono::steady_clock::now()) {}
	~ProfScope() { ProfAdd(n, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()); }
};
}  // namespace bridge
#define PROF_CAT2(a, b) a##b
#define PROF_CAT(a, b) PROF_CAT2(a, b)
#define PROF(name) ::bridge::ProfScope PROF_CAT(_prof_, __LINE__)(name)
