// Portalcraft bridge: testbed traces (lead, 2026-10-03).
//
// The testbed suite (tools/tb/) does not assert in game. Each test records what happened, and tools/tb/judge.py
// decides afterwards from the recording, so a verdict can be re-checked or corrected without another run.
//
//   pcb_tb_run <dir>                          traces go to <dir>/<test>.jsonl (the dir must exist)
//   pcb_tb_begin <test>                       start a trace: one JSON line per game frame (at most 120 Hz)
//   pcb_tb_watch <label> <class|*> <name> <field> <i|f|b|o|a|n>
//                                             also record a server field of the first matching entity each frame:
//                                             i int, f float, b bool byte, o origin (m_vecAbsOrigin), a angles
//                                             (m_angAbsRotation), n = how many live entities have that name,
//                                             c = how many live entities of that class, v = a named 3-float field
//                                             (m_angRotation: m_angAbsRotation is computed lazily and can stay 0)
//   pcb_tb_event <label>                      a marker line (the judge measures phases between markers)
//   pcb_tb_end                                close the trace; Minecraft's damage events since begin are appended
//
// A frame line: {"t":s since begin,"m":"mc"|"chell","p2":{"o":[..],"v":[..],"a":[pitch,yaw],"hp":n,"hold":0|1},
// "mc":{"f":[..] Source u,"g":0|1,"fl":flags,"ride":id,"tr":transitSeq} (only while Minecraft leads),
// "por":[[portalTwo,active,x,y,z,linkage],...] (every prop_portal), "w":{label:value}}
#include "bridge.hpp"
#include "coords.hpp"
#include "link.hpp"

#include "Modules/Tier1.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace bridge {

using namespace skycraft::proto;

int EntFieldOffset(void *ent, const char *field);  // ents.cpp
bool McModeActive();                               // mc.cpp
Vector McFeetSrc();                                // mc.cpp
const McState &McStateNow();                       // mc.cpp
bool PlayerHolding();                              // propcollide.cpp

namespace {
struct Watch {
	std::string label, cls, name, field;
	char type;
};
std::string g_dir = ".";
std::string g_test;
FILE *g_f = nullptr;
std::vector<Watch> g_watch;
double g_t0 = 0, g_last = -1;
long g_mcLogMark = 0;
int g_frames = 0;

double Now() {
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}
std::string McLogPath() {
	return CacheDir() + "/mc.log";
}
long McLogSize() {
	FILE *f = std::fopen(McLogPath().c_str(), "r");
	if (!f) return 0;
	std::fseek(f, 0, SEEK_END);
	long n = std::ftell(f);
	std::fclose(f);
	return n;
}
// JSON string escaping for the few free-text fields (labels, names, log lines)
std::string Q(const std::string &s) {
	std::string o = "\"";
	for (char c : s) {
		if (c == '"' || c == '\\') o += '\\', o += c;
		else if ((unsigned char)c < 0x20) o += ' ';
		else o += c;
	}
	return o + "\"";
}
void *Player() {
	auto v = EntFind("player");
	return v.empty() ? nullptr : v.front();
}
std::string WatchValue(const Watch &w) {
	char b[128];
	if (w.type == 'n') return std::to_string(EntFind(nullptr, w.name.c_str()).size());
	if (w.type == 'c') return std::to_string(EntFind(w.cls.c_str()).size());
	auto v = EntFind(w.cls == "*" ? nullptr : w.cls.c_str(), w.name == "*" ? nullptr : w.name.c_str());
	if (v.empty()) return "null";
	const char *field = w.type == 'o' ? "m_vecAbsOrigin" : w.type == 'a' ? "m_angAbsRotation" : w.field.c_str();  // 'v': a named 3-float field
	const int off = EntFieldOffset(v.front(), field);
	if (off < 0) return "null";
	const char *p = (const char *)v.front() + off;
	switch (w.type) {
	case 'f': std::snprintf(b, sizeof b, "%.3f", *(const float *)p); break;
	case 'b': std::snprintf(b, sizeof b, "%d", (int)*(const unsigned char *)p); break;
	case 'o':
	case 'a':
	case 'v': {
		const float *x = (const float *)p;
		std::snprintf(b, sizeof b, "[%.1f,%.1f,%.1f]", x[0], x[1], x[2]);
		break;
	}
	default: std::snprintf(b, sizeof b, "%d", *(const int *)p); break;
	}
	return b;
}
void Close() {
	if (!g_f) return;
	// Minecraft's damage events since begin (the mod's "Portalcraft: player hurt: <source> <dmg> at x y z" lines in mc.log)
	if (FILE *f = std::fopen(McLogPath().c_str(), "r")) {
		std::fseek(f, g_mcLogMark, SEEK_SET);
		char buf[1024];
		while (std::fgets(buf, sizeof buf, f))
			if (const char *p = std::strstr(buf, "Portalcraft: player hurt: ")) {
				char src[64] = "?", clock[16] = "";
				double dmg = 0, x = 0, y = 0, z = 0;
				std::sscanf(p + 26, "%63s %lf at %lf %lf %lf", src, &dmg, &x, &y, &z);
				std::sscanf(buf, "[%15[^]]", clock);
				const Vector s = McToSrc(x, y, z);
				std::fprintf(g_f, "{\"hurt\":{\"src\":%s,\"dmg\":%.2f,\"at\":[%.1f,%.1f,%.1f],\"clock\":%s}}\n", Q(src).c_str(), dmg > 1e9 ? 1e9 : dmg, s.x, s.y, s.z, Q(clock).c_str());
			}
		std::fclose(f);
	}
	std::fprintf(g_f, "{\"end\":%.3f,\"frames\":%d}\n", Now() - g_t0, g_frames);
	std::fclose(g_f);
	g_f = nullptr;
	console->Print("[bridge] TB END %s (%d frames)\n", g_test.c_str(), g_frames);
}
}  // namespace

// Called every game frame (from ScriptTick).
void TbTraceTick() {
	if (!g_f) return;
	const double t = Now();
	if (g_last >= 0 && t - g_last < 1.0 / 120) return;
	g_last = t;
	++g_frames;
	std::string s;
	char b[512];
	const bool mc = McModeActive();
	std::snprintf(b, sizeof b, "{\"t\":%.3f,\"m\":\"%s\"", t - g_t0, mc ? "mc" : "chell");
	s += b;
	const Vector o = PlayerOrigin(), v = PlayerVelocity();
	const ViewRecord &view = LastMainView();
	int hp = -1;
	if (void *pl = Player()) {
		const int off = EntFieldOffset(pl, "m_iHealth");
		if (off >= 0) hp = *(const int *)((const char *)pl + off);
	}
	std::snprintf(b, sizeof b, ",\"p2\":{\"o\":[%.1f,%.1f,%.1f],\"v\":[%.1f,%.1f,%.1f],\"a\":[%.1f,%.1f],\"hp\":%d,\"hold\":%d}", o.x, o.y, o.z, v.x, v.y, v.z, view.angles.x, view.angles.y, hp,
	              PlayerHolding() ? 1 : 0);
	s += b;
	if (mc) {
		const McState &m = McStateNow();
		const Vector f = McFeetSrc();
		std::snprintf(b, sizeof b, ",\"mc\":{\"f\":[%.1f,%.1f,%.1f],\"g\":%d,\"fl\":%u,\"ride\":%u,\"tr\":%u}", f.x, f.y, f.z, (m.flags & kMcOnGround) ? 1 : 0, m.flags, m.rideMover, m.transitSeq);
		s += b;
	}
	// every prop_portal (the map's own pairs too): [portalTwo, active, x, y, z, linkage]
	s += ",\"por\":[";
	int np = 0;
	for (void *p : EntFind("prop_portal")) {
		const Vector o = EntVec(p, "origin");
		std::snprintf(b, sizeof b, "%s[%d,%d,%.1f,%.1f,%.1f,%d]", np++ ? "," : "", std::atoi(EntKV(p, "PortalTwo").c_str()) & 1, EntKV(p, "Activated") == "1" ? 1 : 0, o.x, o.y, o.z,
		              std::atoi(EntKV(p, "LinkageGroupID").c_str()));
		s += b;
	}
	s += "]";
	if (!g_watch.empty()) {
		s += ",\"w\":{";
		for (size_t i = 0; i < g_watch.size(); ++i) s += (i ? "," : "") + Q(g_watch[i].label) + ":" + WatchValue(g_watch[i]);
		s += "}";
	}
	s += "}\n";
	std::fputs(s.c_str(), g_f);
}

CON_COMMAND(pcb_tb_run, "pcb_tb_run <dir> - (testbed) traces go to <dir>/<test>.jsonl\n") {
	if (args.ArgC() > 1) g_dir = args[1];
}
CON_COMMAND(pcb_tb_begin, "pcb_tb_begin <test> - (testbed) start recording a trace\n") {
	if (args.ArgC() < 2) return;
	Close();
	g_test = args[1];
	g_watch.clear();
	g_f = std::fopen((g_dir + "/" + g_test + ".jsonl").c_str(), "w");
	if (!g_f) return console->Warn("[bridge] TB cannot write %s/%s.jsonl\n", g_dir.c_str(), g_test.c_str());
	g_t0 = Now(), g_last = -1, g_frames = 0;
	g_mcLogMark = McLogSize();
	std::fprintf(g_f, "{\"begin\":%s}\n", Q(g_test).c_str());
	console->Print("[bridge] TB BEGIN %s\n", g_test.c_str());
}
CON_COMMAND(pcb_tb_watch, "pcb_tb_watch <label> <class|*> <name|*> <field> <i|f|b|o|a|n|c|v> - (testbed) record a server field each frame\n") {
	if (args.ArgC() < 6) return;
	g_watch.push_back({args[1], args[2], args[3], args[4], args[5][0]});
}
CON_COMMAND(pcb_tb_event, "pcb_tb_event <label> - (testbed) a marker in the trace\n") {
	if (!g_f || args.ArgC() < 2) return;
	std::fprintf(g_f, "{\"ev\":%s,\"t\":%.3f}\n", Q(args[1]).c_str(), Now() - g_t0);
	console->Print("[bridge] TB EVENT %s %s\n", g_test.c_str(), args[1]);
}
CON_COMMAND(pcb_tb_end, "pcb_tb_end - (testbed) close the trace\n") { Close(); }

}  // namespace bridge
