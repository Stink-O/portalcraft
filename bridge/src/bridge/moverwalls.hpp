// Portalcraft bridge (bridge #50): the walls of the holes mined in movers, drawn as Minecraft block faces at the mover's drawn pose (moverwalls.cpp).
// The geometry is built by movers.cpp when a mover's cuts change (moverwalls_geom.hpp) and published every frame with the pose (like MoverHoles).
#pragma once

#include "bridge.hpp"
#include "moverwalls_geom.hpp"

#include <memory>
#include <vector>

namespace bridge {

// How much the mover punch (digrender.cpp DigMoverMark) grows each mined piece, Source units: the walls sit that far out. Keep equal to its `g`.
inline constexpr float kMoverPunchGrow = 3.0f;

struct MoverWalls {
	Vector origin;    // the mover's origin as Portal 2 draws it (Source)
	float rot[3][3];  // its rotation
	std::shared_ptr<const MoverWallSet> set;
	uint32_t id = 0;  // the mover
};
void MoverWallsSnapshot(std::vector<MoverWalls> &out);  // movers.cpp: render thread, published every frame
bool MoverWallsAny();                                    // movers.cpp
void MoverWallsSetStyle(int kind, const float rects[12]);  // moverwalls.cpp: the atlas rects (side, top, bottom) the mod sent for a material kind
void MoverWallsDraw(const CViewSetup *view);               // moverwalls.cpp: render.cpp's opaque hook, before the mover punch

}  // namespace bridge
