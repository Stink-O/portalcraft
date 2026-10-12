// Portalcraft bridge #78: ONE Minecraft world for every session (bridge/DESIGN-mc-world.md, option (a)).
//
// The generated terrain world (the seed of bridge/world.conf, run_mc.sh's default) is the world of pit flings AND of every campaign chamber:
//   * a campaign chamber is mapped into the world's CHAMBER BAND, above everything the generator makes: Portal 2's (0 0 0) is Minecraft
//     (world.conf CHAMBER_X/Y/Z; originally -320 752 432), 1 block = 40 u, so the whole +-16384 u of Portal 2's world is Minecraft y 342..1162, over the columns of the pinned cliff
//     (the mod's overworld is 1232 blocks high for it: y -64..1167). Every campaign map has the same place; what a chamber holds in Minecraft
//     (placed blocks, the blocks digging reveals) is kept per map by the mod (ChamberSlots.java: the band's blocks of the old map are stored
//     and the new map's put back on every map change), so nothing of one chamber shows in another and a chamber's blocks are there again
//     when its map comes back;
//   * the portal onto Minecraft (G) opens onto the REAL terrain round the pinned cliff: the same home for every chamber, shown in the biggest
//     empty slab of Portal 2's world under the map (stage.cpp UnifiedWindowEnsure), with the floating origin moving it (endless terrain);
//   * pit flings (and the testbed: maps with their own pc_mc_region) keep their own mapping exactly as before.
// The mirror world and its per-chamber stage stay available as the fallback: `pc_world legacy`, or a Minecraft started with the mirror
// world (run_mc.sh --mirror; `pc_world auto` follows the world the linked Minecraft was started with).
#include "bridge.hpp"
#include "coords.hpp"
#include "link.hpp"
#include "platform/platform.hpp"
#include "stage.hpp"
#include "worldconf.hpp"

#include "Modules/Tier1.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace bridge {

bool WorldUnified();
void McSetChamberMapping(double mcX, double mcY, double mcZ, const Vector &src, const char *why);  // mc.cpp
void TestCheck(bool ok, const char *what, const char *detail);                                    // testassert.cpp

// The chamber band's place: Portal 2's origin in Minecraft. Whole sections (the collision cache translates by whole 8-block regions; the
// floating origin and the terrain sections by 16), over the pinned cliff's columns (world.conf; originally MC -331 94 424) so the home terrain stays loaded round a
// Minecraft player in any chamber.
// (bridge #81: world.conf CHAMBER_X/Y/Z, derived from the pinned cliff by tools/world_pick.py; originally -320 752 432)
#define kChamberX (World().chamberX)
#define kChamberY (World().chamberY)
#define kChamberZ (World().chamberZ)

namespace {
int g_setting = 0;  // pc_world: 0 auto (the linked Minecraft's world), 1 unified, 2 legacy
uint32_t g_detPid = 0;
int g_detWorld = -1;  // the linked Minecraft's world: -1 unknown, 0 mirror, 1 terrain
bool g_levelSeen = false, g_levelRegion = false;
bool g_applied = false;  // this level's chamber is in the unified world's band
std::string g_lastWhy;

// The world the linked Minecraft was started with: its command line says (-Dskycraft.world=terrain); same machine, same pid namespace.
int LinkedWorld() {
	const uint32_t pid = link::McPid();
	if (!pid) return -1;
	if (pid == g_detPid) return g_detWorld;
	g_detPid = pid, g_detWorld = -1;
	{
		const std::string cmd = plat::ProcessCommandLine(pid);  // empty when unreadable (always on Windows: the world is then "unknown")
		if (!cmd.empty()) {
		g_detWorld = cmd.find("-Dskycraft.world=terrain") != std::string::npos ? 1 : 0;
		// (bridge #81) the seed the linked Minecraft was started with must be world.conf's: the pinned cliff is only there in that seed's world
		auto sp = cmd.find("-Dskycraft.seed=");
		if (g_detWorld == 1 && sp != std::string::npos) {
			if (cmd[sp + 16] == '-' || (cmd[sp + 16] >= '0' && cmd[sp + 16] <= '9')) console->Print("[bridge] world: the linked Minecraft was started with the forced seed %lld\n", std::atoll(cmd.c_str() + sp + 16));
		}
		}
	}
	console->Print("[bridge] world: the linked Minecraft (pid %u) runs %s\n", pid, g_detWorld == 1 ? "the terrain world" : g_detWorld == 0 ? "the mirror world" : "an unknown world (its command line is not readable)");
	return g_detWorld;
}

void Apply(const char *why) {
	const bool want = !g_levelRegion && WorldUnified() && World().known;  // (bridge #81: once Minecraft has reported its world)
	if (want && !g_applied) {
		McSetChamberMapping(kChamberX, kChamberY, kChamberZ, Vector(0, 0, 0), why);
		console->Print("[bridge] world: unified: this chamber is in the terrain world's band (%s); G opens onto the terrain round the pinned cliff\n", why);
	} else if (!want && g_applied && !g_levelRegion) {
		McSetChamberMapping(0, 0, 0, Vector(0, 0, 0), why);  // back to the mirror world's plain mapping
		console->Print("[bridge] world: legacy (%s): the chamber at Minecraft's origin, G opens onto the stage\n", why);
	}
	g_applied = want;
}
}  // namespace

// (bridge #81) Minecraft reported a (new) world: this level's chamber band origin follows it.
void UnifiedWorldChanged() {
	if (!g_levelSeen) return;
	g_applied = false;
	Apply("world reported");
}

// The unified world is in use: pc_world unified, or auto with a Minecraft that runs the terrain world.
bool WorldUnified() { return g_setting == 1 || (g_setting == 0 && LinkedWorld() == 1); }
// This level's chamber is mapped into the unified world's band (not a region map, unified world).
bool WorldUnifiedChamber() { return g_applied && !g_levelRegion; }

// clip.cpp ClipLevelInit: a new map. The chamber mapping goes in now, before the level's first frame moves the Minecraft player to the body (every
// campaign map has the same one; a region map sets its own at its first frame, UnifiedLevelLive).
void UnifiedLevelInit() {
	g_levelSeen = true, g_levelRegion = false;
	Apply("level init");
}
// clip.cpp ClipLevelLive: the level's first frame, after a region map set its own mapping.
void UnifiedLevelLive(bool regionMap) {
	g_levelSeen = true, g_levelRegion = regionMap;
	if (regionMap) g_applied = false;  // (its own mapping; nothing to undo)
	Apply("level start");
}
// Every frame (clip.cpp ClipFrame): Minecraft may link after the level started, or the setting change.
void UnifiedFrame() {
	if (g_levelSeen) Apply("the Minecraft world became known");
}

CON_COMMAND(pc_world, "pc_world [auto|unified|legacy] - the Minecraft world of campaign chambers: unified = the terrain world (every chamber in its band, G opens onto the real terrain round the pinned cliff); legacy = the mirror world and its stage; auto (default) = the world the linked Minecraft runs\n") {
	if (args.ArgC() > 1) {
		const char *a = args[1];
		g_setting = !std::strcmp(a, "unified") ? 1 : !std::strcmp(a, "legacy") ? 2 : 0;
		if (g_levelSeen) Apply("pc_world");
	}
	console->Print("[bridge] world: pc_world %s; the linked Minecraft runs %s; this level: %s\n", g_setting == 1 ? "unified" : g_setting == 2 ? "legacy" : "auto",
	               g_detWorld == 1 ? "the terrain world" : g_detWorld == 0 ? "the mirror world" : "(not known yet)",
	               g_levelRegion ? "a region map (its own mapping)" : g_applied ? "chamber in the unified band" : "legacy mapping");
}

CON_COMMAND(pcb_assert_world, "pcb_assert_world <unified|legacy> - (tests) this level's chamber is in the unified world's band, or not\n") {
	const bool want = args.ArgC() > 1 && !std::strcmp(args[1], "unified");
	const Vector m = SrcToMc(Vector(0, 0, 0));
	char b[200];
	std::snprintf(b, sizeof b, "chamber %s (want %s); Portal 2 (0 0 0) is Minecraft (%.1f %.1f %.1f); pc_world %d, linked world %d", WorldUnifiedChamber() ? "unified" : "legacy", want ? "unified" : "legacy", m.x, m.y, m.z, g_setting,
	              g_detWorld);
	TestCheck(WorldUnifiedChamber() == want, "world", b);
}

}  // namespace bridge
