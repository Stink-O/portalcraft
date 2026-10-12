// Portalcraft bridge #81: NO fixed seed and no hand-picked cliff. Every player's world has its own random seed; the mod (SpotFinder.java) finds the
// exit spot once per new world, stores it in the save (portalcraft.json) and reports it in cache/bridge/portalcraft_world.env (the keys of bridge/world.conf).
// The plugin, the shell tools and the test scripts take the exit and every anchor derived from it from that report.
#pragma once
#include <string>

namespace bridge {

struct WorldConf {
	bool known = false;  // false until the linked Minecraft has opened a world and reported its spot (cache/bridge/portalcraft_world.env)
	long long seed = 0;
	std::string save;                                  // the save directory's name
	double cliffX = 0, cliffY = 0, cliffZ = 0;         // the terrain mapping's anchor: 3 blocks behind the exit patch, 20 above it
	int cliffDx = -1, cliffDz = 0;                     // the direction the exit face looks
	int exitWx = 0, exitWy = 0, exitWz = 0;            // the pinned exit patch: its bottom wall block at along-face index 0
	double chamberX = 0, chamberY = 752, chamberZ = 0;  // Portal 2 (0 0 0) in Minecraft: the chamber band's origin (whole sections)
	std::string path;
};

// The current world, as the mod reports it. Polled (twice a second at most), so a world opened after the plugin loaded appears by itself.
const WorldConf &World();
// True once per change of the report for the caller that owns `seen` (start at 0): the world appeared, changed, or vanished.
bool WorldChanged(int &seen);

// "@NAME@", "@NAME+3@", "@NAME-4.5@" in a test script line (NAME = a world.conf key, upper case; SAVE_NAME is not a number and is not offered) are
// replaced by the value; anything else is left alone ("@p" selectors are lower case).
std::string WorldConfExpand(const std::string &line);

// "#legacy-cliff-shift" in a test script: its Minecraft coordinates were written for the original cliff (top -328 114 424: the pit region's floor, the
// chamber blocks round it) and are moved by (CLIFF_X + 328, 0, CLIFF_Z - 424) when the script is loaded. Only the commands whose coordinate
// arguments are known (pcb_mc_cmd fill/clone/setblock/tp/summon/particle/item replace block, pcb_mc_check .. block, pcb_aim_mc, pcb_p2light_mc,
// pcb_light_goto_mc, pcb_light_cube_mc, waituntil solid<x>,<y>,<z>); "~" and anything else stay as written. Opt-in per file, never for a script that uses @KEY@.
std::string WorldConfLegacyShift(const std::string &line);

}  // namespace bridge
