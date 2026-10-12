// Portalcraft bridge #81: the world the linked Minecraft opened, as the mod reports it (see worldconf.hpp).
#include "worldconf.hpp"

#include "bridge.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include "platform/platform.hpp"
#include <vector>

namespace bridge {
namespace {
std::map<std::string, std::string> g_kv;  // the report's keys (SEED, SAVE, CLIFF_*, EXIT_W*, CHAMBER_*, SPOT_*)
WorldConf g_conf;
int g_generation = 0;
long long g_lastStamp = -1;
double g_lastPoll = -1e9;

double NowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

double Num(const char *key, double def) {
	auto it = g_kv.find(key);
	return it == g_kv.end() ? def : std::atof(it->second.c_str());
}

std::string ReportPath() {
	if (const char *e = std::getenv("PCB_WORLD_ENV")) return e;
	return CacheDir() + "/portalcraft_world.env";
}

// Re-reads the report when it changed (at most twice a second); a vanished file (a new Minecraft is starting) makes the world unknown again.
void Poll() {
	const double now = NowSec();
	if (now - g_lastPoll < 0.5) return;
	g_lastPoll = now;
	const std::string path = ReportPath();
	plat::FileInfo st;
	long long stamp = plat::Stat(path, &st) ? st.mtimeSec * 1000000000LL + st.mtimeNs + st.size : -1;
	if (stamp == g_lastStamp) return;
	g_lastStamp = stamp;
	++g_generation;
	g_kv.clear();
	g_conf = WorldConf();
	std::ifstream f(path);
	if (stamp < 0 || !f) return;
	std::string s;
	while (std::getline(f, s)) {
		auto b = s.find_first_not_of(" \t\r");
		if (b == std::string::npos || s[b] == '#') continue;
		auto eq = s.find('=', b);
		if (eq == std::string::npos) continue;
		g_kv[s.substr(b, eq - b)] = s.substr(eq + 1, s.find_last_not_of(" \t\r") - eq);
	}
	if (!g_kv.count("EXIT_WX")) return;  // half written / not a report
	WorldConf &w = g_conf;
	w.known = true, w.path = path;
	w.seed = std::atoll(g_kv.count("SEED") ? g_kv["SEED"].c_str() : "0");
	w.save = g_kv.count("SAVE") ? g_kv["SAVE"] : "";
	w.cliffX = Num("CLIFF_X", 0), w.cliffY = Num("CLIFF_Y", 0), w.cliffZ = Num("CLIFF_Z", 0);
	w.cliffDx = (int)Num("CLIFF_DX", -1), w.cliffDz = (int)Num("CLIFF_DZ", 0);
	w.exitWx = (int)Num("EXIT_WX", 0), w.exitWy = (int)Num("EXIT_WY", 0), w.exitWz = (int)Num("EXIT_WZ", 0);
	w.chamberX = Num("CHAMBER_X", 0), w.chamberY = Num("CHAMBER_Y", 752), w.chamberZ = Num("CHAMBER_Z", 0);
}
}  // namespace

const WorldConf &World() {
	Poll();
	return g_conf;
}

bool WorldChanged(int &seen) {
	Poll();
	if (seen == g_generation) return false;
	seen = g_generation;
	return true;
}

std::string WorldConfExpand(const std::string &line) {
	if (line.find('@') == std::string::npos) return line;
	World();  // (polls the report)
	std::string out;
	for (size_t i = 0; i < line.size();) {
		if (line[i] == '@' && i + 1 < line.size() && std::isupper((unsigned char)line[i + 1])) {
			size_t j = i + 1;
			while (j < line.size() && (std::isupper((unsigned char)line[j]) || std::isdigit((unsigned char)line[j]) || line[j] == '_')) ++j;
			std::string name = line.substr(i + 1, j - i - 1);
			double off = 0;
			size_t k = j;
			if (k < line.size() && (line[k] == '+' || line[k] == '-')) {
				char *end = nullptr;
				off = std::strtod(line.c_str() + k, &end);
				k = end - line.c_str();
			}
			auto it = g_kv.find(name);
			if (k < line.size() && line[k] == '@' && it != g_kv.end()) {
				char buf[64];
				std::snprintf(buf, sizeof buf, "%.4f", std::atof(it->second.c_str()) + off);
				std::string n = buf;
				while (n.back() == '0') n.pop_back();
				if (n.back() == '.') n.pop_back();
				out += n;
				i = k + 1;
				continue;
			}
		}
		out += line[i++];
	}
	return out;
}

std::string WorldConfLegacyShift(const std::string &line) {
	if (!World().known) return line;
	const double sx = World().cliffX + 328.0, sz = World().cliffZ - 424.0;
	if (sx == 0 && sz == 0) return line;
	std::vector<std::string> t;
	for (size_t i = 0; i < line.size();) {
		size_t j = line.find(' ', i);
		if (j == std::string::npos) j = line.size();
		if (j > i) t.push_back(line.substr(i, j - i));
		i = j + 1;
	}
	if (t.empty()) return line;
	std::vector<size_t> triples;  // token index of a triple's x (its z is x + 2)
	if (t[0] == "pcb_mc_cmd" && t.size() > 2) {
		const std::string &c = t[1];
		if (c == "fill") triples = {2, 5};
		else if (c == "clone") triples = {2, 5, 8};
		else if (c == "setblock") triples = {2};
		else if (c == "tp" || c == "summon" || c == "particle") triples = {3};
		else if (c == "item" && t.size() > 6 && t[2] == "replace" && t[3] == "block") triples = {4};
	} else if (t[0] == "pcb_mc_check" && t.size() > 5 && t[2] == "block") {
		triples = {3};
	} else if (t[0] == "waituntil" && t.size() > 1 && t[1].rfind("solid", 0) == 0) {  // waituntil solid<x>,<y>,<z> [ticks]
		int x, y, z, n = 0;
		if (std::sscanf(t[1].c_str(), "solid%d,%d,%d%n", &x, &y, &z, &n) == 3 && n == (int)t[1].size()) {
			char buf[96];
			std::snprintf(buf, sizeof buf, "solid%d,%d,%d", x + (int)sx, y, z + (int)sz);
			t[1] = buf;
			std::string out;
			for (size_t i = 0; i < t.size(); ++i) out += (i ? " " : "") + t[i];
			return out;
		}
		return line;
	} else if (t[0] == "pcb_aim_mc" || t[0] == "pcb_p2light_mc" || t[0] == "pcb_light_goto_mc" || t[0] == "pcb_light_cube_mc") {
		triples = {1};
	}
	bool changed = false;
	for (size_t first : triples) {
		for (int axis : {0, 2}) {
			size_t i = first + axis;
			if (i >= t.size()) continue;
			char *end = nullptr;
			double d = std::strtod(t[i].c_str(), &end);
			if (end == t[i].c_str() || *end) continue;  // "~", "~1": relative, as written
			size_t dot = t[i].find('.');
			int dec = dot == std::string::npos ? 0 : (int)(t[i].size() - dot - 1);
			char buf[64];
			std::snprintf(buf, sizeof buf, "%.*f", dec, d + (axis == 0 ? sx : sz));
			t[i] = buf;
			changed = true;
		}
	}
	if (t[0] == "pcb_mc_cmd" || t[0] == "pcb_mc_check") {  // target selectors: @e[x=..,z=..,dx=..] (the volume's size dx/dz stays)
		for (std::string &tok : t) {
			if (tok.find('[') == std::string::npos) continue;
			std::string o;
			for (size_t i = 0; i < tok.size(); ++i) {
				bool arg = (tok[i] == 'x' || tok[i] == 'z') && i + 1 < tok.size() && tok[i + 1] == '=' && i > 0 && (tok[i - 1] == '[' || tok[i - 1] == ',');
				if (!arg) {
					o += tok[i];
					continue;
				}
				char *end = nullptr;
				double d = std::strtod(tok.c_str() + i + 2, &end);
				if (end == tok.c_str() + i + 2) {
					o += tok[i];
					continue;
				}
				std::string num(tok.c_str() + i + 2, (size_t)(end - (tok.c_str() + i + 2)));
				size_t dot = num.find('.');
				char buf[64];
				std::snprintf(buf, sizeof buf, "%c=%.*f", tok[i], dot == std::string::npos ? 0 : (int)(num.size() - dot - 1), d + (tok[i] == 'x' ? sx : sz));
				o += buf;
				i = end - tok.c_str() - 1;
				changed = true;
			}
			tok = o;
		}
	}
	if (!changed) return line;
	std::string out;
	for (size_t i = 0; i < t.size(); ++i) out += (i ? " " : "") + t[i];
	return out;
}

}  // namespace bridge
