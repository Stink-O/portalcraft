// Portalcraft bridge: assertions for scripted test runs.
//
// A run that continues after a missed portal shot produces meaningless numbers, so scripts check
// their preconditions with pcb_assert_* commands. A failed assertion logs "ASSERT FAIL", prints
// the run summary and quits the game at once (through the console, never a kill). Every script
// ends with pcb_run_summary: "RUN SUMMARY: PASS" or "FAIL". Only PASS runs are reported.
#include <cstring>
#include "bridge.hpp"

#include "Modules/Tier1.hpp"
#include "coords.hpp"
#include "platform/platform.hpp"

#include <cmath>
#include <cstdlib>

namespace bridge {

bool ClipExitPose(Vector &pos, QAngle &ang);  // clip.cpp
int ClipRegionPortal();

float PuppetApexGain();  // puppet.cpp
float FluidLastSurfaceZ();  // fluids.cpp
int FluidAt(const Vector &p);
static int g_asserts = 0, g_fails = 0;
static bool g_soft = false;  // pcb_assert_soft 1: failures are counted, the run goes on
static uint64_t g_lastFrames = 0;

void CompositeRunPerf();  // composite.cpp: the run's frame-time stats
static void Summary() {
	CompositeRunPerf();
	console->Print("[bridge] RUN SUMMARY: %s (%d assertions, %d failed)\n", g_fails ? "FAIL" : "PASS", g_asserts, g_fails);
}

void TestCheck(bool ok, const char *what, const char *detail);
static void Check(bool ok, const char *what, const char *detail) { TestCheck(ok, what, detail); }
// (bridge #31) a test block whose placement failed: every later assert of the block is reported as NOT RUN and counts as a failure, never a pass
static bool g_blockSkip = false;
void TestBlockSkip(bool on) { g_blockSkip = on; }
void TestCheck(bool ok, const char *what, const char *detail) {
	char skipped[400];
	if (g_blockSkip && std::strcmp(what, "placement") != 0) {
		std::snprintf(skipped, sizeof skipped, "NOT RUN (the block's placement failed): %s", what);
		what = skipped, ok = false;
	}
	++g_asserts;
	console->Print("[bridge] ASSERT %s: %s %s\n", ok ? "pass" : "FAIL", what, detail);
	if (ok) return;
	++g_fails;
	if (g_soft) return;
	console->Print("[bridge] RUN FAIL: aborting the run\n");
	Summary();
	engine.Cmd("quit");
}

static float Arg(const CCommand &a, int i, float d) { return a.ArgC() > i ? (float)std::atof(a[i]) : d; }

CON_COMMAND(pcb_assert_portals, "pcb_assert_portals - both portals of the pair exist and are active\n") {
	auto ps = EntFind("prop_portal");
	int active = 0;
	char buf[512];
	int n = 0;
	for (void *p : ps) {
		bool a = EntKV(p, "Activated") == "1";
		active += a;
		Vector o = EntVec(p, "origin");
		n += std::snprintf(buf + n, sizeof buf - n, "[PortalTwo %s active %d at %.1f %.1f %.1f] ", EntKV(p, "PortalTwo").c_str(), a, o.x, o.y, o.z);
	}
	Check(ps.size() >= 2 && active >= 2, "two active portals", buf);
}

CON_COMMAND(pcb_assert_portal, "pcb_assert_portal <portalTwo 0|1> <x> <y> <z> [tol=8] - that portal is active at that point\n") {
	int two = (int)Arg(args, 1, 0);
	Vector want(Arg(args, 2, 0), Arg(args, 3, 0), Arg(args, 4, 0));
	float tol = Arg(args, 5, 8);
	bool ok = false;
	char buf[256] = "not found";
	for (void *p : EntFind("prop_portal")) {
		if (std::atoi(EntKV(p, "PortalTwo").c_str()) != two) continue;
		Vector o = EntVec(p, "origin");
		float d = (o - want).Length();
		ok = EntKV(p, "Activated") == "1" && d <= tol;
		std::snprintf(buf, sizeof buf, "at %.1f %.1f %.1f, %.1f u from %.1f %.1f %.1f, active %s", o.x, o.y, o.z, d, want.x, want.y, want.z, EntKV(p, "Activated").c_str());
	}
	Check(ok, "portal placement", buf);
}

CON_COMMAND(pcb_assert_redirect, "pcb_assert_redirect - a portal of the pair sits at the cliff exit in the Minecraft region\n") {
	Vector ep;
	QAngle ea;
	bool have = ClipExitPose(ep, ea);
	int pi = ClipRegionPortal();
	char buf[256];
	float d = pi >= 0 && have ? (g_portalPos[pi] - ep).Length() : -1;
	std::snprintf(buf, sizeof buf, "exit %s, region portal %d, %.1f u from the exit", have ? "found" : "missing", pi, d);
	Check(have && pi >= 0 && d >= 0 && d < 4, "redirect to the cliff", buf);
}

CON_COMMAND(pcb_assert_player, "pcb_assert_player <x> <y> <z> [tol=16] - the player is near that point\n") {
	Vector want(Arg(args, 1, 0), Arg(args, 2, 0), Arg(args, 3, 0));
	Vector o = PlayerOrigin();
	float tol = Arg(args, 4, 16), d = (o - want).Length();
	char buf[200];
	std::snprintf(buf, sizeof buf, "at %.1f %.1f %.1f, %.1f u from %.1f %.1f %.1f", o.x, o.y, o.z, d, want.x, want.y, want.z);
	Check(d <= tol, "player position", buf);
}

CON_COMMAND(pcb_assert_player_below, "pcb_assert_player_below <z> - the player's feet are below that height (e.g. in the region)\n") {
	Vector o = PlayerOrigin();
	char buf[120];
	std::snprintf(buf, sizeof buf, "z %.1f, limit %.1f", o.z, Arg(args, 1, 0));
	Check(o.z < Arg(args, 1, 0), "player below", buf);
}

CON_COMMAND(pcb_assert_player_axis, "pcb_assert_player_axis <x|y|z> <lt|gt> <value> - one coordinate of the player's origin is below / above a value\n") {
	if (args.ArgC() < 4) return console->Print("%s", pcb_assert_player_axis.ThisPtr()->m_pszHelpString);
	Vector o = PlayerOrigin();
	int a = args[1][0] == 'x' ? 0 : args[1][0] == 'y' ? 1 : 2;
	float v = (&o.x)[a], lim = Arg(args, 3, 0);
	bool lt = args[2][0] == 'l';
	char buf[160];
	std::snprintf(buf, sizeof buf, "%s %.1f (origin %.1f %.1f %.1f), want %s %.1f", args[1], v, o.x, o.y, o.z, lt ? "<" : ">", lim);
	Check(lt ? v < lim : v > lim, "player coordinate", buf);
}

CON_COMMAND(pcb_assert_frames, "pcb_assert_frames - Minecraft composite frames arrived since the last call\n") {
	uint64_t f = CompositeFramesReceived();
	char buf[120];
	std::snprintf(buf, sizeof buf, "%llu new frames (%llu total)", (unsigned long long)(f - g_lastFrames), (unsigned long long)f);
	Check(f > g_lastFrames, "composite frames arriving", buf);
	g_lastFrames = f;
}

CON_COMMAND(pcb_assert_counting, "pcb_assert_counting - the host frame-time counters are collecting\n") {
	int n = CompositeHostFramesCounted();
	char buf[80];
	std::snprintf(buf, sizeof buf, "%d frames counted", n);
	Check(n > 0, "frame counters", buf);
}

CON_COMMAND(pcb_assert_apex, "pcb_assert_apex <min_u> [max_u=60] - jump height since pcb_apex_begin\n") {
	float g = PuppetApexGain();
	char buf[80];
	std::snprintf(buf, sizeof buf, "rose %.1f u", g);
	Check(g >= Arg(args, 1, 40) && g <= Arg(args, 2, 60), "jump apex", buf);
}

CON_COMMAND(pcb_assert_float, "pcb_assert_float - the player is in Minecraft water near its surface (not on the bed)\n") {
	Vector o = PlayerOrigin();
	float sz = FluidLastSurfaceZ();
	bool wet = FluidAt(o + Vector(0, 0, 8)) == 1 || FluidAt(o + Vector(0, 0, 40)) == 1 || FluidAt(o - Vector(0, 0, 8)) == 1;
	char buf[160];
	std::snprintf(buf, sizeof buf, "feet z %.1f, surface z %.1f, in water %d", o.z, sz, wet);
	Check(wet && o.z > sz - 72, "floating in water", buf);
}

CON_COMMAND(pcb_assert_roll, "pcb_assert_roll [max_deg=1] - the main view is upright (camera roll near 0), e.g. after a transit\n") {
	const ViewRecord &v = LastMainView();
	float roll = std::remainder(v.angles.z, 360.0f), lim = Arg(args, 1, 1);
	char buf[120];
	std::snprintf(buf, sizeof buf, "roll %.2f deg (pitch %.1f yaw %.1f), limit %.1f", roll, v.angles.x, v.angles.y, lim);
	Check(std::fabs(roll) <= lim, "camera upright", buf);
}

bool PlayerNoclip();  // puppet.cpp
CON_COMMAND(pcb_noclip_set, "pcb_noclip_set <0|1> - toggle noclip only if the player's state differs (scripts)\n") {
	bool want = Arg(args, 1, 1) != 0;
	if (PlayerNoclip() != want) engine.Cmd("noclip");
}

CON_COMMAND(pcb_assert_noclip, "pcb_assert_noclip <0|1> - the player's movetype is (1) or is not (0) noclip\n") {
	bool want = Arg(args, 1, 1) != 0;
	char buf[60];
	std::snprintf(buf, sizeof buf, "noclip %d, expected %d", PlayerNoclip(), want);
	Check(PlayerNoclip() == want, "noclip state", buf);
}

CON_COMMAND(pcb_assert_soft, "pcb_assert_soft <0|1> - 1: a failed assertion is counted but does not abort the run (exploratory runs)\n") {
	g_soft = args.ArgC() > 1 && std::atoi(args[1]) != 0;
}

// Deep caves (bridge #11): Chell's feet in Minecraft y within [min, max] and the block under them is solid in the
// collision export (sections below y 48 ship collision now). A drop into a carved pocket under the terrain lands on
// its floor instead of falling through.
bool TerrainIsSolid(int bx, int by, int bz);  // terrain.cpp
CON_COMMAND(pcb_assert_feet_mcy, "pcb_assert_feet_mcy <min_y> [max_y=min_y+0.5] - Chell's feet are at that Minecraft height, on a block the collision export holds as solid\n") {
	if (args.ArgC() < 2) return;
	const Vector o = PlayerOrigin();
	double x, y, z;
	SrcToMcD(o, x, y, z);
	const double lo = std::atof(args[1]), hi = args.ArgC() > 2 ? std::atof(args[2]) : lo + 0.5;
	const bool solid = TerrainIsSolid((int)std::floor(x), (int)std::floor(y - 0.05), (int)std::floor(z));
	char buf[200];
	std::snprintf(buf, sizeof buf, "feet MC (%.2f %.3f %.2f), expected y %.2f..%.2f, block below solid %d", x, y, z, lo, hi, solid);
	Check(y >= lo && y <= hi && solid, "feet at Minecraft height", buf);
}

bool TerrainSectionKnown(int bx, int by, int bz);  // terrain.cpp
// pcb_find_cave <mcX> <mcZ> <ymin> <ymax> [radius=24] [dirX dirZ]: a natural cave floor near (x z) in the streamed collision:
// a solid block with 3 x 3 x 2 air above it (sections streamed), nearest first; Chell's feet go there (setpos) facing the
// given direction (the cave's longest air run along x or z if none is given). Tests: drop and walk in real caves.
static double g_caveY = -1000;
CON_COMMAND(pcb_find_cave, "pcb_find_cave <mcX> <mcZ> <ymin> <ymax> [radius=24] - put Chell's feet on a natural cave floor near there (tests)\n") {
	if (args.ArgC() < 5) return;
	const int cx = std::atoi(args[1]), cz = std::atoi(args[2]), y0 = std::atoi(args[3]), y1 = std::atoi(args[4]), r = args.ArgC() > 5 ? std::atoi(args[5]) : 24;
	auto air = [](int x, int y, int z) { return TerrainSectionKnown(x, y, z) && !TerrainIsSolid(x, y, z); };
	int best = 1 << 30, bx = 0, by = 0, bz = 0, bestRun = 0, bdx = 1, bdz = 0;
	for (int y = y1; y >= y0; --y)
		for (int dz = -r; dz <= r; ++dz)
			for (int dx = -r; dx <= r; ++dx) {
				const int x = cx + dx, z = cz + dz;
				if (!TerrainSectionKnown(x, y - 1, z) || !TerrainIsSolid(x, y - 1, z)) continue;
				bool ok = true;
				for (int a = -1; a <= 1 && ok; ++a)
					for (int b = -1; b <= 1 && ok; ++b) ok = air(x + a, y, z + b) && air(x + a, y + 1, z + b);
				if (!ok) continue;
				// the longest floor run (air above solid) along +-x / +-z from here, up to 8 blocks
				int run = 0, rdx = 1, rdz = 0;
				for (auto d : {std::pair<int, int>{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
					int k = 1;
					while (k <= 8 && air(x + d.first * k, y, z + d.second * k) && air(x + d.first * k, y + 1, z + d.second * k) && TerrainIsSolid(x + d.first * k, y - 1, z + d.second * k)) ++k;
					if (k - 1 > run) run = k - 1, rdx = d.first, rdz = d.second;
				}
				const int cost = dx * dx + dz * dz - 40 * run;
				if (cost < best) best = cost, bx = x, by = y, bz = z, bestRun = run, bdx = rdx, bdz = rdz;
			}
	if (best == (1 << 30)) {
		g_caveY = -1000;
		return TestCheck(false, "cave found", "no cave floor in the streamed collision there");
	}
	g_caveY = by;
	Vector p = McToSrc(bx + 0.5, by + 0.0, bz + 0.5);
	char c[200];
	const float yaw = std::atan2(-(float)bdz, (float)bdx) * 180.0f / (float)M_PI;  // MC +x = Source +x, MC +z = Source -y
	std::snprintf(c, sizeof c, "setpos %.2f %.2f %.2f; setang 0 %.1f 0", p.x, p.y, p.z + 1.0f, yaw);
	engine.Cmd(c);
	char buf[200];
	std::snprintf(buf, sizeof buf, "cave floor MC (%d %d %d), a %d-block floor run towards (%d %d), %s", bx, by, bz, bestRun, bdx, bdz, c);
	TestCheck(true, "cave found", buf);
}
// pcb_assert_cave_feet [drop=0.5]: Chell's feet are on the floor pcb_find_cave chose (within drop below .. 0.3 above)
// and on a block the collision export holds as solid.
CON_COMMAND(pcb_assert_cave_feet, "pcb_assert_cave_feet [max_drop_blocks=0.5] - Chell stands on the cave floor pcb_find_cave chose (or at most that far below it, on solid)\n") {
	const double drop = args.ArgC() > 1 ? std::atof(args[1]) : 0.5;
	const Vector o = PlayerOrigin();
	double x, y, z;
	SrcToMcD(o, x, y, z);
	const bool solid = TerrainIsSolid((int)std::floor(x), (int)std::floor(y - 0.05), (int)std::floor(z)) || TerrainIsSolid((int)std::floor(x + 0.4), (int)std::floor(y - 0.05), (int)std::floor(z)) ||
	                   TerrainIsSolid((int)std::floor(x - 0.4), (int)std::floor(y - 0.05), (int)std::floor(z)) || TerrainIsSolid((int)std::floor(x), (int)std::floor(y - 0.05), (int)std::floor(z + 0.4)) ||
	                   TerrainIsSolid((int)std::floor(x), (int)std::floor(y - 0.05), (int)std::floor(z - 0.4));
	char buf[220];
	std::snprintf(buf, sizeof buf, "feet MC (%.2f %.3f %.2f), cave floor y %.0f (allowed %.2f..%.2f), block below solid %d", x, y, z, g_caveY, g_caveY - drop, g_caveY + 0.3, solid);
	Check(g_caveY > -999 && y >= g_caveY - drop && y <= g_caveY + 0.3 && solid && std::fabs(y - std::round(y)) < 0.06, "standing on the cave floor", buf);
}

CON_COMMAND(pcb_run_summary, "pcb_run_summary - PASS/FAIL line for the run\n") { Summary(); }

}  // namespace bridge

namespace bridge {
// Portal 2 is a 32-bit process: its address space is the budget for our textures and buffers.
CON_COMMAND(pcb_assert_vmsize, "pcb_assert_vmsize [max_mb=3200] - the game's address space (VmSize) is below the limit\n") {
	int maxMb = args.ArgC() > 1 ? std::atoi(args[1]) : 3200;
	const plat::ProcessStats ps = plat::QueryProcessStats();
	long kb = (long)ps.sizeKb, peak = (long)ps.peakSizeKb;
	char buf[128];
	std::snprintf(buf, sizeof buf, "VmSize %ld MB (peak %ld MB), limit %d MB", kb / 1024, peak / 1024, maxMb);
	TestCheck(kb > 0 && kb / 1024 < maxMb, "address space", buf);
}

// Persistence: the pair as it stands now, and later "still the same, still open".
static Vector g_markPos[2];
static bool g_markSet[2] = {false, false};
CON_COMMAND(pcb_portal_mark, "pcb_portal_mark - remember both portals' positions (for pcb_assert_portals_unchanged)\n") {
	g_markSet[0] = g_markSet[1] = false;
	for (void *p : EntFind("prop_portal")) {
		int two = std::atoi(EntKV(p, "PortalTwo").c_str()) & 1;
		if (EntKV(p, "Activated") != "1") continue;
		g_markPos[two] = EntVec(p, "origin"), g_markSet[two] = true;
	}
	console->Print("[bridge] portals marked: 0 %s (%.1f %.1f %.1f), 1 %s (%.1f %.1f %.1f)\n", g_markSet[0] ? "open" : "closed", g_markPos[0].x, g_markPos[0].y, g_markPos[0].z,
	               g_markSet[1] ? "open" : "closed", g_markPos[1].x, g_markPos[1].y, g_markPos[1].z);
}
CON_COMMAND(pcb_assert_portals_unchanged, "pcb_assert_portals_unchanged [label] - both portals marked by pcb_portal_mark are still open at the same place\n") {
	char buf[400];
	int n = std::snprintf(buf, sizeof buf, "[%s] ", args.ArgC() > 1 ? args[1] : "");
	bool ok = g_markSet[0] && g_markSet[1];
	for (int two = 0; two < 2; ++two) {
		bool found = false, active = false;
		Vector o;
		for (void *p : EntFind("prop_portal")) {
			if ((std::atoi(EntKV(p, "PortalTwo").c_str()) & 1) != two) continue;
			found = true, active = EntKV(p, "Activated") == "1", o = EntVec(p, "origin");
		}
		float d = (o - g_markPos[two]).Length();
		ok = ok && found && active && d < 1.0f;
		n += std::snprintf(buf + n, sizeof buf - n, "portal %d %s at (%.1f %.1f %.1f), %.2f u from the mark; ", two, !found ? "MISSING" : active ? "open" : "CLOSED", o.x, o.y, o.z, d);
	}
	TestCheck(ok, "portals still open where they were", buf);
}
}  // namespace bridge
