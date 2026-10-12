// Portalcraft bridge: the slot table's data and the first-run "safe-load" policy (see slots.hpp).
#include "slots.hpp"

#include "bridge.hpp"
#include "platform/platform.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace bridge::slots {

static const Entry kTable[] = {
#define PCB_X(Name, iface, module, group, lin, win, basis, kind, note) {#Name, iface, module, group, lin, win, Basis::basis, Kind::kind, note},
    PCB_SLOT_TABLE(PCB_X)
#undef PCB_X
};
static_assert(sizeof kTable / sizeof kTable[0] == kCount, "slot table and enum out of step");

const Entry &Info(Id id) { return kTable[id]; }

#if defined(_WIN32) || defined(PCB_SLOTS_FORCE_WINDOWS)  // (the second form lets a Linux unit test exercise the Windows policy)
static constexpr bool kWindows = true;
#else
static constexpr bool kWindows = false;
#endif

namespace {
struct Policy {
	bool all = false;
	std::set<std::string> enabled;        // lower-case group or slot names
	std::map<std::string, int> overrides;  // lower-case slot name -> number
	std::string sources;                   // what was read, for the log
	bool skipped[kCount] = {};
};

std::string Lower(std::string s) {
	for (char &c : s) c = (char)std::tolower((unsigned char)c);
	return s;
}

void ParseTokens(Policy &p, const std::string &text, bool numbers) {
	std::string tok;
	auto flush = [&] {
		if (tok.empty()) return;
		if (numbers) {
			size_t eq = tok.find('=');
			if (eq != std::string::npos) {
				// base 0: decimal, 0x.. hex (the offset rows are written in hex), and a malformed number is ignored instead of becoming 0
				char *end = nullptr;
				const long v = std::strtol(tok.c_str() + eq + 1, &end, 0);
				if (end && end != tok.c_str() + eq + 1 && *end == '\0') p.overrides[Lower(tok.substr(0, eq))] = (int)v;
			}
		} else {
			const std::string t = Lower(tok);
			if (t == "all") p.all = true;
			else p.enabled.insert(t);
		}
		tok.clear();
	};
	bool comment = false;
	for (char c : text) {
		if (c == '\n') comment = false;
		if (comment) continue;
		if (c == '#') {
			flush();
			comment = true;
		} else if (c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ';') flush();
		else tok += c;
	}
	flush();
}

std::string ReadFile(const std::string &path) {
	std::string out;
	if (FILE *f = std::fopen(path.c_str(), "rb")) {
		char buf[1024];
		size_t n;
		while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
		std::fclose(f);
	}
	return out;
}

Policy &Pol() {
	static Policy p;
	static std::once_flag once;
	std::call_once(once, [] {
		const std::string dir = CacheDir();
		std::string env = plat::Getenv("PCB_WIN_UNVERIFIED");
		if (!env.empty()) ParseTokens(p, env, false), p.sources += " PCB_WIN_UNVERIFIED";
		std::string file = ReadFile(dir + "/win_unverified.txt");
		if (!file.empty()) ParseTokens(p, file, false), p.sources += " win_unverified.txt";
		std::string oenv = plat::Getenv("PCB_SLOTS");
		if (!oenv.empty()) ParseTokens(p, oenv, true), p.sources += " PCB_SLOTS";
		std::string ofile = ReadFile(dir + "/slot_overrides.txt");
		if (!ofile.empty()) ParseTokens(p, ofile, true), p.sources += " slot_overrides.txt";
	});
	return p;
}

bool Overridden(const Policy &p, Id id) { return !p.overrides.empty() && p.overrides.count(Lower(kTable[id].name)) != 0; }
}  // namespace

int Index(Id id) {
	const Entry &e = kTable[id];
	Policy &p = Pol();
	if (p.overrides.empty()) return kWindows ? e.win : e.linux_;  // fast path: no overrides (always on Linux), no string/map work per call
	auto it = p.overrides.find(Lower(e.name));
	if (it != p.overrides.end()) return it->second;
	return kWindows ? e.win : e.linux_;
}

bool EnabledAll() { return Pol().all; }

bool Usable(Id id) {
	if (!kWindows) return true;
	const Entry &e = kTable[id];
	if (e.basis != Basis::Unverified) return true;
	Policy &p = Pol();
	if (p.all || Overridden(p, id)) return true;
	return p.enabled.count(Lower(e.name)) || p.enabled.count(Lower(e.group));
}

bool Use(Id id) {
	if (Usable(id)) return true;
	Policy &p = Pol();
	if (!p.skipped[id]) {
		p.skipped[id] = true;
		const Entry &e = kTable[id];
		if (console)
			console->Print("[bridge] safe-load: skipped %s (%s slot %d, Windows number unverified; %s): enable with PCB_WIN_UNVERIFIED=%s or win_unverified.txt\n", e.name, e.iface, e.win,
			               e.note[0] ? e.note : "Portal 2 only", e.group);
	}
	return false;
}

int Count(Basis b) {
	int n = 0;
	for (const Entry &e : kTable) n += e.basis == b;
	return n;
}

void LogPolicy() {
	if (!kWindows) return;
	Policy &p = Pol();
	std::string groups;
	std::set<std::string> seen;
	int held = 0;
	for (int i = 0; i < kCount; ++i)
		if (!Usable((Id)i)) {
			++held;
			if (seen.insert(kTable[i].group).second) groups += std::string(groups.empty() ? "" : ", ") + kTable[i].group;
		}
	console->Print("[bridge] safe-load: %d slot entries (%d SAR, %d derived from the SDK header, %d proton-verified, %d windows-verified, %d Portal-2-only unverified); policy read from:%s\n", (int)kCount, Count(Basis::Sar),
	               Count(Basis::Derived), Count(Basis::ProtonVerified), Count(Basis::WindowsVerified), Count(Basis::Unverified), p.sources.empty() ? " nothing (defaults)" : p.sources.c_str());
	if (held)
		console->Print("[bridge] safe-load: %d unverified entries held back, groups: %s. Features that need them stay off and log what they skipped. After pcb_probe_slots, enable "
		               "a group with PCB_WIN_UNVERIFIED=<group> (or all) or cache/bridge/win_unverified.txt\n", held, groups.c_str());
	else
		console->Print("[bridge] safe-load: nothing held back (policy enables all unverified entries)\n");
	for (auto &o : p.overrides) {
		bool known = false;
		for (const Entry &e : kTable) known = known || Lower(e.name) == o.first;
		console->Print("[bridge] safe-load: slot override %s = %d%s\n", o.first.c_str(), o.second, known ? "" : "  (NO SUCH SLOT: ignored, check the spelling against slots.hpp)");
	}
}

}  // namespace bridge::slots
