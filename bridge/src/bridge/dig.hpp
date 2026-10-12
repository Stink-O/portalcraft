// Portalcraft bridge (bridge #33): the Minecraft player digs through Portal 2's chamber walls and floors (REVIEW-freshEyes 6).
//
// Minecraft keeps the dug cells (a per-world set, saved with its world; the mod's SkyDig, a port of SkyCraft's) and sends them here per
// section (kRenDug). Here they are:
//   * left out of the collision export (collfield.cpp, one filter in the harvest worker),
//   * cut out of the host's picture inside Portal 2's own world shaders, every view (cutrender.cpp, bridge #73/#75), and made seen through
//     by every view's visibility and the server's PVS (cutvis.cpp),
//   * seen through by Chell's hull (blockcollide.cpp) and by the portal gun's shots (portalgun.cpp), both of which use the exported voxels
//     around a hole instead of the engine's world, which still has the wall that was dug away,
//   * a reason to fizzle a portal that sat on a cell that is gone.
// Everything is inert until `pc_dig 1` (which also turns the harvest export on) and while no cell is dug.
#pragma once

#include "bridge.hpp"
#include "skycraft_protocol.h"

#include <vector>

namespace bridge {

bool DigEnabled();   // pc_dig is on
bool DigAny();       // any cell is dug (cheap; any thread)
uint32_t DigGen();   // changes whenever the set does (any thread)
// The cell containing a Minecraft block position / a Source point (main thread or the server/client movement hooks).
bool DigIsDugMc(int bx, int by, int bz);
bool DigIsDugSrc(const Vector &p);
// True if a dug cell's box touches the Source box [mn, mx] grown by pad.
bool DigNear(const Vector &mn, const Vector &mx, float pad);
// The dug cells' Source boxes touching [mn, mx] grown by pad (any thread: the render hook uses it).
void DigCellsNear(const Vector &mn, const Vector &mx, float pad, std::vector<std::pair<Vector, Vector>> &out);
// Every dug cell's Source box (render thread; a snapshot).
void DigAllBoxes(std::vector<std::pair<Vector, Vector>> &out);

void DigOnMessage(const uint8_t *p, uint32_t len);   // a kRenDug payload (terrain.cpp's ring consumer)
void DigHostFill(skycraft::proto::HostPlayer &hp);   // mc.cpp: kHpDig, the dig world, the material under the crosshair

uint32_t DigWorld();  // this map's dig world id (0 = none)

// (bridge #43) Holes the Minecraft player mined in movers (doors, panels, lifts, cars: any mover; movers.cpp). A mover keeps its own set of mined cells (1-block
// cubes of its own frame); its collision is its shape minus them, and its picture is punched where they were (digrender.cpp, at the mover's drawn pose).
struct MoverHoles {
	Vector origin;           // the mover's origin as Portal 2 draws it (Source)
	float rot[3][3];         // its rotation (columns = local forward / left / up in world space)
	std::vector<std::pair<Vector, Vector>> boxes;  // Source local boxes: the pieces of the mover that were mined out
};
void MoverHolesSnapshot(std::vector<MoverHoles> &out);  // render thread: a snapshot, published every frame
bool MoverHolesAny();
void MoverDigOnMessage(const uint8_t *p, uint32_t len);  // a kRenMoverDig payload (terrain.cpp's ring consumer)

// collfield.cpp
bool CollisionFieldVoxelBoxes(const Vector &lo, const Vector &hi, std::vector<std::pair<Vector, Vector>> &out, bool *complete);
bool CollisionFieldVoxelRay(const Vector &start, const Vector &dir, float maxDist, float &dist, Vector &normal);

}  // namespace bridge
