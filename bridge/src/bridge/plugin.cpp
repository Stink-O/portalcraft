// Portalcraft bridge: Portal 2 server plugin entry point and console commands.
//
// Loaded with `plugin_load <absolute path to portalcraft_bridge.so>`. The plugin
// framework (interface lookup, signature scans, hooks, offsets, ConCommand
// registration, material-system helpers) is a trimmed fork of
// SourceAutoRecord (MIT); see bridge/NOTICE.
#include "bridge.hpp"
#include "slots.hpp"
#include "worldconf.hpp"
#include "prof.hpp"

#include "Modules/MaterialSystem.hpp"
#include "Modules/Tier1.hpp"
#include "Offsets.hpp"
#include "Utils/SDK/ServerPlugin.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include "platform/platform.hpp"

extern FILE *g_bridgeLogFile;
void LoadOffsets();

namespace bridge {
void ConLogShutdown();  // conlog.cpp

std::vector<Block> g_blocks;

// The repo root: $PORTALCRAFT_ROOT if set, else the first directory above the .so that holds the marker file
// .portalcraft-root (walk up at most 6 levels), else ~/.cache/portalcraft (no repo found: caches only).
std::string RepoRoot() {
	static std::string cached;
	if (!cached.empty()) return cached;
	if (const char *env = std::getenv("PORTALCRAFT_ROOT")) {
		if (*env && plat::Stat(env)) return cached = env;
	}
	{
		std::string p = plat::OwnModulePath();  // (Windows: UTF-8 with '/' separators)
		if (!p.empty()) {
			for (int i = 0; i < 6; ++i) {
				auto s = p.find_last_of('/');
				if (s == std::string::npos || s == 0) break;
				p = p.substr(0, s);
				if (plat::Stat(p + "/.portalcraft-root")) return cached = p;
			}
		}
	}
	return cached = plat::HomeOrTemp() + "/.cache/portalcraft";
}

std::string CacheDir() { return RepoRoot() + "/cache/bridge"; }
std::string MinecraftAssetsDir() {
	return RepoRoot() + "/cache/minecraft-26.3/resourcepacks/local/minecraft-26.3/assets/minecraft";
}

// PCB_SKIP="a,b": bisect aid for init steps (see Load and McInit)
bool BridgeSkip(const char *name) {
	static const std::string skip = "," + plat::Getenv("PCB_SKIP") + ",";
	return skip.size() > 2 && skip.find(std::string(",") + name + ",") != std::string::npos;
}

static float ArgF(const CCommand &a, int i, float def = 0) { return i < a.ArgC() ? (float)std::atof(a[i]) : def; }

// ---------------------------------------------------------------- commands

static void PlacePortal(int which, const Vector &at, const Vector &nrm);

#ifdef _WIN32
namespace plat {
bool FocusGameWindow();
}
CON_COMMAND(pcb_focus, "pcb_focus - (Windows tests) ask for keyboard focus for the game window (an unfocused Source window runs at ~15 fps); logs whether it got it\n") {
	const bool ok = plat::FocusGameWindow();
	console->Print("[bridge] focus: %s\n", ok ? "the game window has the focus" : "asked, the window still has no focus");
}
#endif

CON_COMMAND(pcb_block, "pcb_block <x> <y> <z> [kind 0 grass|1 dirt|2 planks] - add a 1-block cube with its min corner at x y z (Source units)\n") {
	if (args.ArgC() < 4) return console->Print("%s", pcb_block.ThisPtr()->m_pszHelpString);
	g_blocks.push_back({Vector(ArgF(args, 1), ArgF(args, 2), ArgF(args, 3)), (int)ArgF(args, 4, 0)});
	console->Print("[bridge] block #%d at (%.1f %.1f %.1f)\n", (int)g_blocks.size() - 1, ArgF(args, 1), ArgF(args, 2), ArgF(args, 3));
}

CON_COMMAND(pcb_block_here, "pcb_block_here [distance=96] [kind] - drop a block on the floor in front of the camera\n") {
	auto &v = LastMainView();
	float d = ArgF(args, 1, 96);
	float yaw = v.angles.y * (float)M_PI / 180.0f;
	Vector p = v.origin + Vector(std::cos(yaw), std::sin(yaw), 0) * d;
	CGameTrace tr;
	engine.Trace(p, p - Vector(0, 0, 4096), Vector(0, 0, 0), MASK_SOLID_BRUSHONLY, tr, true);
	Vector mins(std::round(p.x - kUnitsPerBlock / 2), std::round(p.y - kUnitsPerBlock / 2), tr.endpos.z);
	g_blocks.push_back({mins, (int)ArgF(args, 2, 0)});
	console->Print("[bridge] block #%d at (%.1f %.1f %.1f) (floor trace fraction %.3f)\n", (int)g_blocks.size() - 1, mins.x, mins.y, mins.z, tr.fraction);
}

CON_COMMAND(pcb_clear, "pcb_clear - remove all blocks\n") {
	g_blocks.clear();
	console->Print("[bridge] blocks cleared\n");
}

CON_COMMAND(pcb_views, "pcb_views <n> - log the next n scene views the hook sees\n") {
	RenderTraceViews(args.ArgC() > 1 ? std::atoi(args[1]) : 20);
}

CON_COMMAND(pcb_status, "pcb_status - print hook, view and world-probe state\n") {
	auto &v = LastMainView();
	console->Print("[bridge] views last frame: %d; main view (%.1f %.1f %.1f) ang (%.1f %.1f)\n", RenderViewsLastFrame(), v.origin.x, v.origin.y, v.origin.z,
	               v.angles.x, v.angles.y);
	for (size_t i = 0; i < g_blocks.size(); ++i) {
		Vector c = g_blocks[i].mins + Vector(1, 1, 1) * (kUnitsPerBlock / 2);
		Vector l = engine.LightAt(c);
		bool outside = engine.PointOutsideWorld(engine.traceServer->ThisPtr(), c);
		int contents = engine.GetPointContents(engine.traceServer->ThisPtr(), c, MASK_ALL, nullptr);
		console->Print("[bridge] block #%d centre (%.1f %.1f %.1f): light (%.3f %.3f %.3f) outsideWorld=%d contents=0x%x\n", (int)i, c.x, c.y, c.z,
		               l.x, l.y, l.z, outside, contents);
		static const Vector n[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
		for (int k = 0; k < 6; ++k) {
			Vector fl = engine.LightAt(c + n[k] * (kUnitsPerBlock / 2 + 2), n[k]);
			console->Print("[bridge]   face %d normal (%g %g %g): ComputeLighting (%.3f %.3f %.3f)\n", k, n[k].x, n[k].y, n[k].z, fl.x, fl.y, fl.z);
		}
	}
}

CON_COMMAND(pcb_probe, "pcb_probe <x> <y> <z> - PointOutsideWorld / contents / light at a point\n") {
	Vector c(ArgF(args, 1), ArgF(args, 2), ArgF(args, 3));
	Vector l = engine.LightAt(c);
	bool outside = engine.PointOutsideWorld(engine.traceServer->ThisPtr(), c);
	int contents = engine.GetPointContents(engine.traceServer->ThisPtr(), c, MASK_ALL, nullptr);
	console->Print("[bridge] probe (%.1f %.1f %.1f): outsideWorld=%d contents=0x%x light (%.3f %.3f %.3f)\n", c.x, c.y, c.z, outside, contents, l.x, l.y, l.z);
}

// Stage-A CollisionField sample (SkyCraft DESIGN 5.1): vertical rays on a grid of
// 1/8-block cells around the camera, plus one hull trace, timed.
CON_COMMAND(pcb_trace_grid, "pcb_trace_grid [radius_blocks=4] [cells_per_block=8] - sample world collision around the camera with engine traces\n") {
	auto &v = LastMainView();
	int rb = args.ArgC() > 1 ? std::atoi(args[1]) : 4;
	int cpb = args.ArgC() > 2 ? std::atoi(args[2]) : 8;
	float cell = kUnitsPerBlock / cpb;
	int n = rb * cpb;
	std::string path = CacheDir() + "/trace_grid.txt";
	FILE *f = std::fopen(path.c_str(), "w");
	auto t0 = std::chrono::steady_clock::now();
	int hits = 0, total = 0, startsolid = 0;
	float zmin = 1e9f, zmax = -1e9f;
	for (int j = -n; j < n; ++j) {
		for (int i = -n; i < n; ++i) {
			Vector top(v.origin.x + (i + 0.5f) * cell, v.origin.y + (j + 0.5f) * cell, v.origin.z + 64);
			CGameTrace tr;
			bool hit = engine.Trace(top, top - Vector(0, 0, 512), Vector(0, 0, 0), MASK_PLAYERSOLID, tr, false);
			++total;
			if (tr.startsolid) ++startsolid;
			if (hit && !tr.startsolid) {
				++hits;
				zmin = std::fmin(zmin, tr.endpos.z);
				zmax = std::fmax(zmax, tr.endpos.z);
			}
			if (f) std::fprintf(f, "%.2f %.2f %.2f %d %d %.3f %.3f %.3f 0x%x\n", top.x, top.y, tr.endpos.z, hit, tr.startsolid, tr.plane.normal.x,
			                    tr.plane.normal.y, tr.plane.normal.z, tr.contents);
		}
	}
	// one player-sized hull trace straight down (72 u tall, 32 u wide)
	CGameTrace hull;
	engine.Trace(v.origin, v.origin - Vector(0, 0, 512), Vector(16, 16, 36), MASK_PLAYERSOLID, hull, false);
	double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	if (f) std::fclose(f);
	console->Print("[bridge] trace grid: %d rays (%dx%d, cell %.2f u), %d hits, %d start-solid, floor z %.1f..%.1f, %.2f ms (%.2f us/ray); hull trace endpos z %.1f frac %.3f -> %s\n",
	               total, 2 * n, 2 * n, cell, hits, startsolid, zmin, zmax, ms, 1000.0 * ms / total, hull.endpos.z, hull.fraction, path.c_str());
}

CON_COMMAND(pcb_portal, "pcb_portal <0|1> <x> <y> <z> <pitch> <yaw> <roll> - move portal N of linkage group 0 (Portal 2's own prop_portal)\n") {
	if (args.ArgC() < 8) return console->Print("%s", pcb_portal.ThisPtr()->m_pszHelpString);
	char buf[256];
	std::snprintf(buf, sizeof buf, "portal_place 0 %d %s %s %s %s %s %s", std::atoi(args[1]), args[2], args[3], args[4], args[5], args[6], args[7]);
	engine.Cmd(buf);
	console->Print("[bridge] %s\n", buf);
}

CON_COMMAND(pcb_portal_on_block, "pcb_portal_on_block <block#> <face 0..5 = +X -X +Y -Y +Z -Z> <0|1> - put portal N on a block face\n") {
	if (args.ArgC() < 4) return console->Print("%s", pcb_portal_on_block.ThisPtr()->m_pszHelpString);
	int bi = std::atoi(args[1]), face = std::atoi(args[2]), which = std::atoi(args[3]);
	if (bi < 0 || bi >= (int)g_blocks.size() || face < 0 || face > 5) return console->Print("bad block/face\n");
	static const Vector n[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
	static const float pitch[6] = {0, 0, 0, 0, -90, 90};
	static const float yaw[6] = {0, 180, 90, 270, 0, 0};
	float h = kUnitsPerBlock / 2;
	Vector c = g_blocks[bi].mins + Vector(h, h, h) + n[face] * (h + 0.5f);
	(void)pitch; (void)yaw;
	PlacePortal(which, c, n[face]);
}

CON_COMMAND(pcb_puppet, "pcb_puppet <x> <y> <z> [radius=64] [period=8] - drive the player around a circle (position every tick, view every frame); pcb_puppet off\n") {
	if (args.ArgC() == 2 && !std::strcmp(args[1], "off")) return PuppetStop();
	if (args.ArgC() < 4) return console->Print("%s", pcb_puppet.ThisPtr()->m_pszHelpString);
	PuppetStart(Vector(ArgF(args, 1), ArgF(args, 2), ArgF(args, 3)), ArgF(args, 4, 64), ArgF(args, 5, 8));
}

CON_COMMAND(pcb_log, "pcb_log <text> - write a line to the bridge log\n") {
	console->Print("[bridge] log: %s\n", args.ArgC() > 1 ? args.m_pArgSBuffer + args.m_nArgv0Size : "");
}

CON_COMMAND(pcb_light, "pcb_light <scale=5> [gamma=1] [floor=0] - block lighting: colour = clamp((light*scale)^(1/gamma))\n") {
	if (args.ArgC() < 2) return console->Print("%s", pcb_light.ThisPtr()->m_pszHelpString);
	RenderSetLightParams(ArgF(args, 1), ArgF(args, 2, 1.0f), ArgF(args, 3, 0.0f));
}

Vector g_portalPos[kPortalSlots], g_portalNormal[kPortalSlots], g_portalUp[kPortalSlots] = {Vector(0, 0, 1), Vector(0, 0, 1), Vector(0, 0, 1), Vector(0, 0, 1), Vector(0, 0, 1), Vector(0, 0, 1),
                                                                                          Vector(0, 0, 1), Vector(0, 0, 1), Vector(0, 0, 1), Vector(0, 0, 1), Vector(0, 0, 1), Vector(0, 0, 1),
                                                                                          Vector(0, 0, 1), Vector(0, 0, 1), Vector(0, 0, 1), Vector(0, 0, 1)};
bool g_portalSet[kPortalSlots] = {};
float g_portalHalfW[kPortalSlots] = {32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32};
float g_portalHalfH[kPortalSlots] = {56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56};

void PlacePortalAt(int which, const Vector &at, const Vector &nrm) { PlacePortal(which, at, nrm); }

static void PlacePortal(int which, const Vector &at, const Vector &nrm) {
	float pyaw = std::atan2(nrm.y, nrm.x) * 180.0f / (float)M_PI;
	float ppitch = -std::asin(std::fmax(-1.0f, std::fmin(1.0f, nrm.z))) * 180.0f / (float)M_PI;
	char buf[256];
	std::snprintf(buf, sizeof buf, "portal_place 0 %d %.2f %.2f %.2f %.1f %.1f 0", which & 1, at.x, at.y, at.z, ppitch, pyaw);
	engine.Cmd(buf);
	g_portalPos[which & 1] = at;
	g_portalNormal[which & 1] = nrm;
	g_portalSet[which & 1] = true;
	console->Print("[bridge] %s\n", buf);
}

int ClipRegionPortal();  // clip.cpp
CON_COMMAND(pcb_view_portal,
            "pcb_view_portal <0|1|r> [distance=128] [angle=0] [orbit=0] - noclip camera pose at portal N (r: the one in the Minecraft region).\n"
            "  orbit 0: the eye `distance` u in front of the centre, looking `angle` deg away from straight at it (80 = along the wall)\n"
            "  orbit 1: the eye `distance` u from the centre, `angle` deg off the portal's normal, looking at the centre\n") {
	int w = 0;
	if (args.ArgC() > 1 && args[1][0] == 'r') w = ClipRegionPortal();
	else w = args.ArgC() > 1 ? (std::atoi(args[1]) & 1) : 0;
	if (w < 0) return console->Print("[bridge] view_portal: no portal in the region\n");
	float d = ArgF(args, 2, 128), ang = ArgF(args, 3, 0) * (float)M_PI / 180.0f;
	bool orbit = args.ArgC() > 4 && std::atoi(args[4]) != 0;
	Vector n = g_portalNormal[w];
	auto rotZ = [](const Vector &v, float a) { return Vector(v.x * std::cos(a) - v.y * std::sin(a), v.x * std::sin(a) + v.y * std::cos(a), v.z); };
	Vector eye, look;
	if (orbit) {
		eye = g_portalPos[w] + rotZ(n, ang) * d;
		look = g_portalPos[w] - eye;
		look = look * (1.0f / look.Length());
	} else {
		eye = g_portalPos[w] + n * d;
		look = rotZ(n * -1.0f, ang);
	}
	float yaw = std::atan2(look.y, look.x) * 180.0f / (float)M_PI;
	float pitch = -std::asin(std::fmax(-1.0f, std::fmin(1.0f, look.z))) * 180.0f / (float)M_PI;
	char buf[256];
	std::snprintf(buf, sizeof buf, "setpos %.2f %.2f %.2f; setang %.1f %.1f 0", eye.x, eye.y, eye.z - 64, pitch, yaw);
	engine.Cmd(buf);
	console->Print("[bridge] view_portal %d d %.0f angle %.0f%s: %s\n", w, d, ang * 180 / M_PI, orbit ? " orbit" : "", buf);
}

CON_COMMAND(pcb_mat, "pcb_mat <0..3> [debug_colour 0|1] - pick the block material variant (lighting investigation)\n") {
	RenderSelectMaterial(args.ArgC() > 1 ? std::atoi(args[1]) : 0, args.ArgC() > 2 && std::atoi(args[2]));
}

// Put portal N on whatever wall a horizontal trace from a point hits.
CON_COMMAND(pcb_portal_wall, "pcb_portal_wall <0|1> <x> <y> <z> <yaw> - trace from x y z along yaw and put portal N on the wall it hits (z = portal centre)\n") {
	if (args.ArgC() < 6) return console->Print("%s", pcb_portal_wall.ThisPtr()->m_pszHelpString);
	int which = std::atoi(args[1]);
	Vector p(ArgF(args, 2), ArgF(args, 3), ArgF(args, 4));
	float yaw = ArgF(args, 5) * (float)M_PI / 180.0f;
	Vector dir(std::cos(yaw), std::sin(yaw), 0);
	CGameTrace tr;
	if (!engine.Trace(p, p + dir * 4096, Vector(0, 0, 0), MASK_SOLID_BRUSHONLY, tr, true)) return console->Print("[bridge] portal_wall: no hit\n");
	Vector nrm = tr.plane.normal;
	console->Print("[bridge] portal_wall hit at (%.1f %.1f %.1f) normal (%.2f %.2f %.2f) dist %.1f surface '%s'\n", tr.endpos.x, tr.endpos.y, tr.endpos.z,
	               nrm.x, nrm.y, nrm.z, tr.fraction * 4096, tr.surface.name ? tr.surface.name : "?");
	PlacePortal(which, tr.endpos + nrm * 1.0f, nrm);
}

// ---------------------------------------------------------------- plugin

// (bridge #61) What is keyed by the old map (positions, Portal 2's lighting, dynamic lights) and was never cleared at a level change.
void ClipLevelInit();          // clip.cpp
void TerrainLevelReset();      // terrain.cpp: lighting states, contact-AO cache, the band's rebuild
void SceneLevelReset();        // scene.cpp: light cubes and states of Minecraft's entities
void BlockLightsLevelInit();   // blocklights.cpp: the dynamic-light slots (the engine cleared its dlights)
void GrowthLevelReset();       // growth.cpp
static void LevelCachesReset() {
	TerrainLevelReset();
	SceneLevelReset();
	BlockLightsLevelInit();
	GrowthLevelReset();
}

class BridgePlugin : public IServerPluginCallbacks {
	bool loaded = false;

public:
	bool Load(CreateInterfaceFn, CreateInterfaceFn) override {
		if (this->loaded) return false;  // plugin_load twice
		plat::MakeDir(RepoRoot() + "/cache");
		plat::MakeDir(CacheDir());
		g_bridgeLogFile = std::fopen((CacheDir() + "/bridge.log").c_str(), "a");
		console = new Console();
		if (!console->Init()) return false;
		std::time_t now = std::time(nullptr);
		console->Print("[bridge] ==== portalcraft bridge loading %s", std::ctime(&now));

		CrashLogInstall();
		LoadOffsets();
		slots::LogPolicy();  // Windows: what the first-run safe-load policy holds back
		{
			const std::string area = plat::AddressSpaceSummary();
			if (!area.empty()) console->Print("[bridge] %s\n", area.c_str());
		}
		tier1 = new Tier1();
		materialSystem = new MaterialSystem();
		bool ok = true;
		if (!tier1->Init()) console->Warn("[bridge] tier1 init failed\n"), ok = false;
		console->DevMsg("[bridge] tier1 ok\n");
		if (!materialSystem->Init()) console->Warn("[bridge] materialsystem init failed\n"), ok = false;
		console->DevMsg("[bridge] materialsystem ok\n");
		if (!engine.Init()) console->Warn("[bridge] engine init failed\n"), ok = false;
		console->DevMsg("[bridge] engine ok\n");
		if (!ok) return false;

		// PCB_SKIP="render,puppet,collider,blockcollide,mc,fluids,portalgun,composite,mclight": bisect aid, skips those init steps
		auto skipped = [&](const char *n) { return BridgeSkip(n); };
		int n = Command::RegisterAll();
		console->DevMsg("[bridge] %d commands registered\n", n);
		if (skipped("render")) console->Print("[bridge] SKIPPED init render\n"); else if (!RenderInit()) return false;
		console->DevMsg("[bridge] render ok\n");
		if (skipped("puppet")) console->Print("[bridge] SKIPPED init puppet\n"); else if (!PuppetInit()) console->Warn("[bridge] puppet hooks failed (continuing)\n");
		if (skipped("collider")) console->Print("[bridge] SKIPPED init collider\n"); else if (!ColliderInit()) console->Warn("[bridge] server tools unavailable: no solid blocks (continuing)\n");
		if (skipped("blockcollide")) console->Print("[bridge] SKIPPED init blockcollide\n"); else if (!BlockCollideInit()) console->Warn("[bridge] movement trace hook failed: Chell walks through blocks (continuing)\n");
		if (skipped("mc")) console->Print("[bridge] SKIPPED init mc\n"); else if (!McInit()) console->Warn("[bridge] client mode hooks failed: no Minecraft mode (continuing)\n");
		if (skipped("fluids")) console->Print("[bridge] SKIPPED init fluids\n"); else if (!FluidsInit()) console->Warn("[bridge] point-contents hook failed: no Minecraft water for Chell (continuing)\n");
		if (skipped("portalgun")) console->Print("[bridge] SKIPPED init portalgun\n"); else if (!PortalGunInit()) console->Warn("[bridge] portal gun trace hook failed: no portals on Minecraft blocks (continuing)\n");
		if (skipped("composite")) console->Print("[bridge] SKIPPED init composite\n"); else if (!CompositeInit()) console->Warn("[bridge] composite texture/material failed: no Minecraft frame compositing (continuing)\n");
		if (skipped("mclight")) console->Print("[bridge] SKIPPED init mclight\n"); else if (!McLightInit()) console->Warn("[bridge] model lighting hooks failed: region objects keep Portal 2 lighting (continuing)\n");

#ifdef _WIN32
		plat::WinKeepActiveForTests();  // test harness only (PCB_KEEP_ACTIVE=1): no 50 ms unfocused sleep
		engine.Cmd("pcb_probe_slots");  // first-run check of every slot and signature (reads only), written to bridge.log and cache/bridge/slot_probe.txt
#endif
		console->Print("[bridge] loaded: %d commands, blocks are %.0f u, log %s/bridge.log\n", n, kUnitsPerBlock, CacheDir().c_str());
		console->Print("[bridge] world: waiting for Minecraft to report its world (cache/bridge/portalcraft_world.env)\n");
		this->loaded = true;
		return true;
	}

	void Unload() override {
		if (!this->loaded) return;
		console->Print("[bridge] unloading\n");
		ScriptShutdown();
		{
			void TestToolsShutdown();  // bugmark.cpp (bridge #24): the SDL event watch and the input tap off
			TestToolsShutdown();
		}
		{
			void CollisionFieldShutdown();  // collfield.cpp (bridge #33): the harvest workers stop
			CollisionFieldShutdown();
		}
		AudioShutdown();
		ConLogShutdown();
		PortalGunShutdown();
		{
			void BlockLightsShutdown();  // blocklights.cpp (bridge #18): switch our dynamic lights off
			void BandLightShutdown();    // bandlight.cpp (bridge #18)
			BlockLightsShutdown();
			BandLightShutdown();
			void CombatShutdown();  // combat.cpp (bridge #19): the body's Portal 2 health back to 100
			CombatShutdown();
		}
		McLightShutdown();
		CompositeShutdown();
		FluidsShutdown();
		McShutdown();
		plat::InputShutdown();  // Windows: the message hook goes before the DLL does
		BlockCollideShutdown();  // hooks the same vtable as the puppet: undo in reverse order
		PuppetShutdown();
		ColliderShutdown();
		EntsShutdown();
		RenderShutdown();
		Command::UnregisterAll();
		engine.Shutdown();
		materialSystem->Shutdown();
		tier1->Shutdown();
		this->loaded = false;
		CrashLogRemove();
		if (g_bridgeLogFile) std::fclose(g_bridgeLogFile), g_bridgeLogFile = nullptr;
	}

	void Pause() override {}
	void UnPause() override {}
	const char *GetPluginDescription() override { return "Portalcraft bridge (SourceAutoRecord-based)"; }
	void LevelInit(char const *map) override {
		ClipLevelInit();  // (bridge #61) the old level's region / stage state goes before the new map's first frame
		ChellProxyLevelLive();
		console->Print("[bridge] level init %s\n", map);
		// the launcher's "Continue": the last single-player campaign level played with Portalcraft (demo and test maps don't count)
		if (map && !std::strncmp(map, "sp_a", 4)) {
			if (FILE *f = std::fopen((CacheDir() + "/progress.txt").c_str(), "w")) {
				std::fprintf(f, "%s\n%lld\n", map, (long long)std::time(nullptr));
				std::fclose(f);
			}
		}
		this->levelStarted = false;
		for (bool &s : g_portalSet) s = false;
		McLevelInit(map);
		FluidsLevelInit();
		ScriptLevelInit();
		void MapIndexLevelInit(const char *map);  // mapindex.cpp (bridge #24)
		MapIndexLevelInit(map);
		LevelCachesReset();  // (bridge #61)
	}
	void ServerActivate(void *, int, int) override {}
	bool levelStarted = false;
	void GameFrame(bool simulating) override {
		if (simulating && !this->levelStarted) {
			this->levelStarted = true;
			BlockCollideLevelStart();
			McLevelLive();
			ClipLevelLive();
			PropCollideLevelLive();
			FloatOriginLevelLive();
			ChellProxyLevelLive();
		}
		PROF("gameframe");  // (bridge #25) frame attribution, prof.cpp
		PuppetFrame();
		{
			PROF("gameframe.clip");
			if (this->levelStarted) ClipFrame();  // the cliff exit re-check (also while paused)
			if (simulating) ClipTick();
		}
		{
			PROF("gameframe.gun");
			if (simulating) PortalGunTick();
		}
		{
			PROF("gameframe.props");
			if (simulating) PropCollideTick(), FloatOriginTick();
		}
		if (this->levelStarted) ScriptTick();  // also while paused (e.g. the window lost focus)
		void TestToolsFrame(bool levelLive);  // bugmark.cpp (bridge #24): bug marks, map index, collision overlay
		{
			PROF("gameframe.tools");
			TestToolsFrame(this->levelStarted);
		}
	}
	void LevelShutdown() override {
		LevelCachesReset();  // (bridge #61) the caches that are keyed by the old map's positions or lighting go with it
		void GlHudHideNow();  // glhud.cpp (bridge #56): the Minecraft hand/HUD goes at once, not over the next level's load
		GlHudHideNow();
	}
	void ClientFullyConnect(void *) override {}
	void ClientActive(void *) override {}
	void ClientDisconnect(void *) override {
		void GlHudHideNow();
		GlHudHideNow();
	}
	void ClientPutInServer(void *, char const *) override {}
	void SetCommandClient(int) override {}
	void ClientSettingsChanged(void *) override {}
	int ClientConnect(bool *, void *, const char *, const char *, char *, int) override { return 0; }
	int ClientCommand(void *, const void *&) override { return 0; }
	int NetworkIDValidated(const char *, const char *) override { return 0; }
	void OnQueryCvarValueFinished(int, void *, int, const char *, const char *) override {}
	void OnEdictAllocated(void *) override {}
	void OnEdictFreed(const void *) override {}
};

static BridgePlugin g_plugin;

}  // namespace bridge

using bridge::BridgePlugin;
EXPOSE_SINGLE_INTERFACE_GLOBALVAR(BridgePlugin, IServerPluginCallbacks, INTERFACEVERSION_ISERVERPLUGINCALLBACKS, bridge::g_plugin);
