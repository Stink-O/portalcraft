// Portalcraft bridge #47: the Minecraft stage and the virtual region (stage.cpp).
//
// The portal onto Minecraft (G) must work in every campaign chamber, and those maps have no hidden Minecraft box (they are Valve's;
// pit_flings and the testbed have one built in, those keep it). So the Minecraft side makes real terrain, the STAGE, high above
// everything the chambers use (the mod, StageTerrain.java), and this side shows a window of it in an EMPTY box of Portal 2's world, the
// virtual region: Source space that no brush, entity or path of the map touches (chosen from the BSP, stagecalc.hpp). The mapping of
// the box and of the stage is the plain one plus a Source offset (coords.hpp StageMap), so Minecraft stays at Portal 2's own coordinates
// for the chamber and the stage is a translated copy. Everything else is the region maps' machinery unchanged (the region box, the
// cliff exit and the partner redirect, the composite, the way home): the virtual region simply IS a region once it is on.
//
// It is made the first time it is needed (G armed or a shot fired with G) and stays until the level changes.
#pragma once

#include "bridge.hpp"
#include "skycraft_protocol.h"

namespace bridge {

// The stage's landscape, in step with StageTerrain.java.
constexpr int kStageY0 = 640;       // the bedrock layer
constexpr int kStageLow = 704;      // the lowland's ground level
constexpr int kStageWallH = 24;     // plateaus stand this much above it
constexpr int kStagePeriod = 256;   // a plateau and a lowland; cliff faces (facing -X) at x = k * 256 (+ a 0..3 offset per 24-block run of z)
constexpr int kStageRun = 24;
constexpr int kStageTop = 768;
// (the window shown: 160 x 120 x 160 blocks around the cliff exit; the mod makes the landscape 12 chunks around it)
constexpr float kStageHalfX = 3200, kStageHalfY = 3200, kStageHalfZ = 2400;  // the virtual region's half extents (Source units)

bool StageActive();                              // the virtual region is on (this level)
bool StageEnsure(const char *why);               // turn it on if the level has no region and Minecraft is up; true when it is on
void StageLevelReset();                          // a new level: off
bool StageWindow(int &wallX, int &zc);           // the cliff wall (block x) and the exit's z run centre, once on
void StageAnchorMc(double &x, double &y, double &z);  // where Minecraft's hidden player waits while the stage is on (Minecraft coordinates)
void StageHostFill(skycraft::proto::HostPlayer &hp);  // HP_STAGE and the window's chunk, for the mod
Vector StageParkSpot();                          // a spot outside the box but inside Portal 2's world, where a spare collider can wait
bool StageMcRegionSkip(int ry);                  // a collision region (8 blocks) that lies in the stage: Portal 2 exports nothing there
bool StageUnified();                             // (bridge #78) the window on is the unified world's (the real terrain round the pinned cliff)
// Chell's hull in the virtual region: Portal 2's own world is void there, so the engine's answer is dropped (blockcollide.cpp)
bool StageHullNear(const Vector &lo, const Vector &hi, float pad);
int StageWallBoxes(const Vector &lo, const Vector &hi, Vector mn[6], Vector mx[6]);  // the box's six invisible walls that the swept hull reaches

}  // namespace bridge
