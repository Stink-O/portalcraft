// Portalcraft bridge: scripted test runs.
//
// Portal 2 does not exec listenserver.cfg in singleplayer, and `wait` in
// autoexec.cfg expires during loading, so the plugin runs test scripts itself:
//
//   pcb_script <file> [watchdog_seconds=150]      (from autoexec.cfg, before `map`)
//
// The script starts on the first simulated server tick after a map loads.
// Lines are console commands, except `wait <ticks>` (server ticks, 60/s) and
// `#` comments. Scripts should end with `quit`; the watchdog sends SIGTERM to
// the game (a clean shutdown: the plugin unloads normally) if one is still
// running after the given time, so a test run can never be left open.
#include "bridge.hpp"
#include "growth.hpp"
#include "coords.hpp"
#include "worldconf.hpp"

#include "Modules/Tier1.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>
#include "platform/platform.hpp"

namespace bridge {

extern bool g_logInput;  // puppet.cpp: log player input changes
extern bool g_audioTestMute;  // audio.cpp

struct Line {
	int wait;
	std::string cmd;
	std::string until;  // `waituntil <cond> <timeout_ticks>`: hold until the condition holds
	bool late = false, legacy = false;  // (bridge #81) @KEY@ values / the original cliff's coordinates: expanded when the line RUNS (the world is reported by Minecraft after the script is loaded)
	bool abort = false;  // `waituntil <cond> <ticks> abort`: a timeout ends the run (to its pcb_run_summary) (bridge #20)
};
bool ClipExitPose(Vector &pos, QAngle &ang);  // clip.cpp
bool ClipHaveExit();                          // clip.cpp
void TestCheck(bool ok, const char *what, const char *detail);  // testassert.cpp
namespace link { bool McAlive(); }
static double g_untilDeadline = -1;
bool TerrainSectionKnown(int bx, int by, int bz);  // terrain.cpp
bool FindPlacedReady();                            // portalgun.cpp
double TerrainIdleS();                             // terrain.cpp
bool CorridorReady();                              // portalgun.cpp
// Every column of sections within one section of the player (x/z) has streamed something within 4 sections
// of the player's height (all-air sections are never sent).
static bool TerrainAround(const Vector &feet) {
	double x, y, z;
	SrcToMcD(feet, x, y, z);
	int bx = (int)std::floor(x), by = (int)std::floor(y), bz = (int)std::floor(z);
	for (int dx = -1; dx <= 1; ++dx)
		for (int dz = -1; dz <= 1; ++dz) {
			bool any = false;
			for (int dy = -4; dy <= 4 && !any; ++dy) any = TerrainSectionKnown(bx + 16 * dx, by + 16 * dy, bz + 16 * dz);
			if (!any) return false;
		}
	return true;
}
// Conditions for `waituntil`: link (Minecraft attached), exit (terrain around the cliff streamed,
// exit found), frames (Minecraft composite frames arriving), spawned (the player exists).
double FloatTripNet();  // floatorigin.cpp
bool PlayerHolding();   // propcollide.cpp
bool PropStill(const char *name);  // propcollide.cpp
bool McWaitMode(bool mc);          // mc.cpp
const char *McMapName();           // mc.cpp
bool McLevelIsLive();              // mc.cpp
bool McLeads();                    // mc.cpp
bool McPlayerAlive();              // mc.cpp
bool CombatMcDead();               // combat.cpp (bridge #19)
int PuppetTransits();   // puppet.cpp
bool FloatShiftedSinceStep();
bool MapIndexNear(const char *spec);  // mapindex.cpp (bridge #24): "<target>:<dist>"
static bool Condition(const std::string &c) {
	if (c.compare(0, 8, "mcyabove") == 0 || c.compare(0, 8, "mcybelow") == 0) {  // Chell's feet above / below a Minecraft y
		double x, y, z;
		SrcToMcD(PlayerOrigin(), x, y, z);
		const double v = std::atof(c.c_str() + 8);
		return c[3] == 'a' ? y > v : y < v;
	}
	if (c.compare(0, 8, "mcxabove") == 0 || c.compare(0, 8, "mcxbelow") == 0) {  // Chell's feet east / west of a Minecraft x
		double x, y, z;
		SrcToMcD(PlayerOrigin(), x, y, z);
		const double v = std::atof(c.c_str() + 8);
		return c[3] == 'a' ? x > v : x < v;
	}
	if (c.compare(0, 5, "still") == 0) return PropStill(c.c_str() + 5);  // a prop (by targetname) has settled
	if (c.compare(0, 5, "solid") == 0) {  // solid<x>,<y>,<z>: that Minecraft block has streamed as solid (a fill landed)
		int x, y, z;
		return std::sscanf(c.c_str() + 5, "%d,%d,%d", &x, &y, &z) == 3 && TerrainIsSolid(x, y, z);
	}
	if (c.compare(0, 3, "ent") == 0) return !EntFind(nullptr, c.c_str() + 3).empty();  // ent<targetname>: such an entity exists
	if (c.compare(0, 3, "map") == 0) return McLevelIsLive() && !std::strcmp(McMapName(), c.c_str() + 3);  // map<name>: that level is in and live
	if (c == "mcdead") return CombatMcDead();  // bridge #19: the Minecraft player is dead (its death screen is up)
	if (c == "mcalive") return McPlayerAlive();  // (bridge #20) Minecraft's player is in its world and not dead (no death screen)
	if (c == "mcleads") return McLeads();  // the Minecraft player leads (not on its way after a level start / transit / ride)
	if (c == "mcmode") return McWaitMode(true);    // the Minecraft player leads (bridge_mode mc took effect)
	if (c == "chellmode") return McWaitMode(false);
	if (c == "moversbuilt") {
		bool MoversAllBuilt();  // movers.cpp (bridge #68)
		return MoversAllBuilt();
	}
	if (c == "holding") return PlayerHolding();
	if (c == "transit") return PuppetTransits() > 0;  // a portal transit since pcb_assert_transits reset
	if (c == "notholding") return !PlayerHolding();
	if (c == "streamidle") return TerrainIdleS() > 1.5;  // no terrain section arrived for 1.5 s (streaming settled; anywhere, also high in the air)
	if (c == "floatshift") return FloatShiftedSinceStep();  // a floating-origin shift since the last pcb_float_step_to / near_edge
	if (c.compare(0, 4, "trip") == 0) return FloatTripNet() >= std::atof(c.c_str() + 4);  // the float trip's net Minecraft distance (blocks)
	if (c == "link") return link::McAlive();
	if (c == "exit") {
		Vector p;
		QAngle a;
		return ClipExitPose(p, a);
	}
	if (c == "gpair") return g_portalSet[0] && g_portalSet[1] && ClipRegionPortal() >= 0;  // (bridge #49) a pair with one portal in Minecraft is open (the portal onto Minecraft, G)
	if (c == "haveexit") return ClipHaveExit();  // resolved by the plugin itself (no check forced)
	if (c == "nolink") return !link::McAlive();
	if (c == "probes_idle") return CompositeProbesIdle();  // every per-frame probe has finished (they run slower than wall-clock waits)
	if (c == "frames") return CompositeFramesReceived() > 0;
	if (c == "spawned") return PlayerOrigin().Length() > 0.1f;
	if (c == "mcplaced") {  // (bridge #31) pcb_mc_place: Minecraft has been sent to the point, stands on the ground there
		bool McPlaced();  // mc.cpp
		return McPlaced();
	}
	if (c == "placed") return FindPlacedReady();  // a test block placed in Minecraft has streamed
	if (c.compare(0, 5, "near:") == 0) return MapIndexNear(c.c_str() + 5);  // (bridge #24) waituntil near <target> <dist>
	if (c == "near") return TerrainAround(PlayerOrigin());  // Minecraft's terrain around the player has streamed
	if (c == "corridor") return CorridorReady();  // the test corridor's walls have streamed
	if (c == "terrain_idle") return TerrainAround(PlayerOrigin()) && TerrainIdleS() > 1.0;  // and no section arrived for 1 s (streaming settled)
	return true;
}
static std::vector<Line> g_lines;
PCB_GAUGE("script.lines", g_lines.size());
static size_t g_pc = 0;
static double g_resumeAt = 0;  // wall clock: `wait N` = N/60 s (server ticks can run faster while loading)

static double NowS() {
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}
static bool g_armed = false, g_running = false;
static std::atomic<int> g_watchdogGen{0};

void ScriptLevelInit() {
	if (g_armed && !g_running) console->Print("[bridge] script: map loading, will start on first tick\n");
}

void TbTraceTick();  // tbtrace.cpp (lead): testbed traces, every game frame
void ScriptTick() {
	TbTraceTick();
	if (!g_armed) return;
	if (!g_running) {
		g_running = true;
		console->Print("[bridge] script: start (%d lines)\n", (int)g_lines.size());
	}
	if (NowS() < g_resumeAt) return;
	while (g_pc < g_lines.size()) {
		auto &u = g_lines[g_pc];
		if (!u.until.empty()) {
			if ((u.late || u.legacy) && !World().known) return;  // the world is not reported yet
			if ((u.late || u.legacy) && g_untilDeadline < 0) {
				std::string full = "waituntil " + u.until + " 0";
				if (u.legacy) full = WorldConfLegacyShift(full);
				full = WorldConfExpand(full);
				u.until = full.substr(10, full.size() - 12), u.late = u.legacy = false;
			}
			if (g_untilDeadline < 0) g_untilDeadline = NowS() + u.wait / 60.0, console->Print("[bridge] script> waituntil %s\n", u.until.c_str());
			if (!Condition(u.until)) {
				if (NowS() < g_untilDeadline) return;
				char buf[96];
				std::snprintf(buf, sizeof buf, "%s not reached within %d ticks", u.until.c_str(), u.wait);
				g_untilDeadline = -1;
				++g_pc;
				TestCheck(false, "waituntil", buf);
				if (u.abort) {
					// (bridge #20) fail fast: nothing after this can test anything; on to the run summary (or the end)
					size_t to = g_lines.size();
					for (size_t k = g_lines.size(); k-- > g_pc;)
						if (g_lines[k].cmd.rfind("pcb_run_summary", 0) == 0) { to = k; break; }
					if (to == g_lines.size())
						for (size_t k = g_lines.size(); k-- > g_pc;)
							if (g_lines[k].cmd == "quit") { to = k; break; }
					console->Print("[bridge] script: ABORT: %s; skipping to the end of the script\n", buf);
					g_pc = to;
				}
				return;
			}
			console->Print("[bridge] script: %s ready\n", u.until.c_str());
#ifdef _WIN32
			// Dev runs under Proton (dev link set): ask for the window focus once the level is up; an unfocused Source window runs at ~15 fps
			if (u.until == "spawned" && std::getenv("PCB_LINK_FILE")) engine.Cmd("pcb_focus");
#endif
			g_untilDeadline = -1;
			++g_pc;
			continue;
		}
		if ((g_lines[g_pc].late || g_lines[g_pc].legacy) && !World().known) return;  // wait for the world report (bounded by the watchdog)
		auto &l = g_lines[g_pc++];
		if (l.late || l.legacy) {
			if (l.legacy) l.cmd = WorldConfLegacyShift(l.cmd);
			l.cmd = WorldConfExpand(l.cmd);
			l.late = l.legacy = false;
		}
		if (l.wait > 0) {
			g_resumeAt = NowS() + l.wait / 60.0;
			return;
		}
		console->Print("[bridge] script> %s\n", l.cmd.c_str());
		engine.Cmd(l.cmd.c_str());
	}
	g_armed = false;
	console->Print("[bridge] script: done\n");
}

CON_COMMAND(pcb_script, "pcb_script <file> [watchdog_seconds=150] - run a test script after the next map load (see script.cpp)\n") {
	if (args.ArgC() < 2) return console->Print("%s", pcb_script.ThisPtr()->m_pszHelpString);
	std::ifstream f(args[1]);
	if (!f) return console->Warn("[bridge] script: cannot open %s\n", args[1]);
	g_lines.clear();
	std::string s;
	bool legacyShift = false;  // (bridge #81) "#legacy-cliff-shift": a script written for the original cliff (worldconf.hpp)
	while (std::getline(f, s)) {
		auto b = s.find_first_not_of(" \t\r");
		if (b != std::string::npos && s[b] == '#' && s.compare(b, 19, "#legacy-cliff-shift") == 0) legacyShift = true;
		if (b == std::string::npos || s[b] == '#') continue;
		s = s.substr(b);
		while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
		// (bridge #24) waituntil near <targetname|x,y,z> <dist> [ticks=600] [abort]: the player within dist u of the target
		char nt[128] = {}, nd[32] = {}, nflag[16] = {};
		int nticks = 600;
		if (s.rfind("waituntil near ", 0) == 0 && std::sscanf(s.c_str() + 15, "%127s %31s %d %15s", nt, nd, &nticks, nflag) >= 2 && std::strtod(nd, nullptr) > 0 &&
		    std::string(nt).find_first_not_of("0123456789") != std::string::npos) {
			g_lines.push_back({nticks, "", std::string("near:") + nt + ":" + nd, !std::strcmp(nflag, "abort")});
		} else if (s.rfind("waituntil ", 0) == 0) {
			char cond[32] = {}, flag[16] = {};
			int t = 600;
			std::sscanf(s.c_str() + 10, "%31s %d %15s", cond, &t, flag);
			g_lines.push_back({t, "", cond, !std::strcmp(flag, "abort")});
		} else if (s.rfind("wait ", 0) == 0 || s == "wait") {
			g_lines.push_back({s == "wait" ? 1 : std::atoi(s.c_str() + 5), "", ""});
		} else {
			g_lines.push_back({0, s, ""});
		}
		{
			Line &ln = g_lines.back();
			for (size_t q = s.find('@'); q != std::string::npos && !ln.late; q = s.find('@', q + 1)) ln.late = q + 1 < s.size() && s[q + 1] >= 'A' && s[q + 1] <= 'Z';
			ln.legacy = legacyShift;
		}
	}
	g_pc = 0;
	g_resumeAt = 0;
	g_armed = true;
	g_running = false;
	g_logInput = true;  // so a run's log shows whether anyone touched the controls
	g_audioTestMute = true;  // test runs never make Minecraft audible
	int secs = args.ArgC() > 2 ? std::atoi(args[2]) : 150;
	int gen = ++g_watchdogGen;
	std::thread([secs, gen]() {
		for (int i = 0; i < secs * 10; ++i) {
			plat::SleepUs(100000);
			if (g_watchdogGen.load() != gen) return;  // re-armed or plugin unloading
		}
		if (g_watchdogGen.load() != gen) return;
		console->Warn("[bridge] script watchdog: %d s elapsed, terminating the test run\n", secs);
		plat::TerminateSelf();
	}).detach();
	console->Print("[bridge] script: loaded %s (%d lines), watchdog %d s\n", args[1], (int)g_lines.size(), secs);
}

void ScriptShutdown() { ++g_watchdogGen; }

}  // namespace bridge
