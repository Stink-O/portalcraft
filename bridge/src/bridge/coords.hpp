// Portalcraft bridge: Minecraft <-> Source (Portal 2) coordinates.
//
// 1 block = 40 Source units. Source is Z-up with +X east and +Y north; Minecraft is Y-up with
// +X east and +Z south. Both are right-handed, so, around a pair of anchor points (the region
// mapping: Minecraft block position mcOrigin sits at Source position srcOrigin):
//   src = srcOrigin + ((mc.x - o.x) * 40, -(mc.z - o.z) * 40, (mc.y - o.y) * 40)
// Yaw: Minecraft 0 faces south (+Z) and grows clockwise seen from above; Source 0 faces +X and
// grows counter-clockwise:  src_yaw = -mc_yaw - 90.  Pitch is positive looking down in both.
#pragma once

#include "bridge.hpp"

#include <cmath>

namespace bridge {

// (bridge #47) The Minecraft STAGE: a landscape high above everything the chambers use (blocks y >= yMin), shown in an empty box of
// Portal 2's world (the virtual region, stage.cpp). It is the same mapping moved by a Source offset d: a stage point's Source
// position is the plain mapping's plus d, and a Source point inside the box maps back to the stage. The two ranges never overlap
// (Portal 2's world is +-16384 u = +-410 blocks of y; the stage starts at y 640).
struct StageMap {
	bool on = false;
	Vector mn{0, 0, 0}, mx{0, 0, 0};  // the virtual region's box (Source units)
	Vector d{0, 0, 0};                 // Source offset of the stage
	double yMin = 600;                 // Minecraft y from which a point belongs to the stage
	double yMax = 1e30;                // (bridge #78) ... and below which: the unified world's window is the terrain UNDER the chamber band (y < 320)
};
// (bridge #78) a Minecraft height that belongs to the stage / window (the offset mapping), not to the chamber (the plain one)
inline bool InStageY(const StageMap &st, double y) { return st.on && y >= st.yMin && y < st.yMax; }

struct RegionMap {
	double mcX = 0, mcY = 0, mcZ = 0;  // Minecraft anchor (blocks)
	Vector src{0, 0, 0};              // where it sits in Portal 2 (units)
	StageMap stage;                   // bridge #47
};
extern RegionMap g_map;

inline bool InStageBox(const Vector &s) {
	const StageMap &st = g_map.stage;
	return st.on && s.x >= st.mn.x && s.x <= st.mx.x && s.y >= st.mn.y && s.y <= st.mx.y && s.z >= st.mn.z && s.z <= st.mx.z;
}
inline Vector McToSrc(double x, double y, double z) {
	Vector v(g_map.src.x + (float)((x - g_map.mcX) * kUnitsPerBlock), g_map.src.y + (float)(-(z - g_map.mcZ) * kUnitsPerBlock),
	         g_map.src.z + (float)((y - g_map.mcY) * kUnitsPerBlock));
	if (InStageY(g_map.stage, y)) v = v + g_map.stage.d;
	return v;
}
inline Vector SrcToMc(const Vector &s0) {
	const Vector s = InStageBox(s0) ? s0 - g_map.stage.d : s0;
	return Vector((float)(g_map.mcX + (s.x - g_map.src.x) / kUnitsPerBlock), (float)(g_map.mcY + (s.z - g_map.src.z) / kUnitsPerBlock),
	              (float)(g_map.mcZ - (s.y - g_map.src.y) / kUnitsPerBlock));
}
// Double precision variant for anchors far from the Minecraft origin.
inline void SrcToMcD(const Vector &s0, double &x, double &y, double &z) {
	const Vector s = InStageBox(s0) ? s0 - g_map.stage.d : s0;
	x = g_map.mcX + (s.x - g_map.src.x) / kUnitsPerBlock;
	y = g_map.mcY + (s.z - g_map.src.z) / kUnitsPerBlock;
	z = g_map.mcZ - (s.y - g_map.src.y) / kUnitsPerBlock;
}
inline float McYawToSrc(float mcYaw) { return std::remainder(-mcYaw - 90.0f, 360.0f); }
inline float SrcYawToMc(float srcYaw) { return std::remainder(-srcYaw - 90.0f, 360.0f); }

}  // namespace bridge
