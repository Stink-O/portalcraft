// Prints protocol/skycraft_protocol.h's layout under Proto.java's names, for layout_test.sh.
// Built -m32 (like the Portal 2 plugin) so i386 struct alignment is what gets checked.
#include "../../protocol/skycraft_protocol.h"
#include <cstddef>
#include <cstdio>

using namespace skycraft::proto;
#define P(name, v) std::printf("%s %lld\n", name, (long long)(v))
#define O(name, S, f) P(name, offsetof(S, f))

int main() {
	P("MAGIC", kMagic); P("VERSION", kVersion); P("UNITS_PER_BLOCK", (long long)kUnitsPerBlock);
	P("OFF_HEADER", kOffHeader); P("OFF_SKY_STATE", kOffSkyState); P("OFF_MC_STATE", kOffMcState);
	P("OFF_OVERLAY_CTL", kOffOverlayCtl); P("OFF_OVERLAY_SLOT_HDR", kOffOverlaySlotHdr); P("OFF_WATER_GRID", kOffWaterGrid);
	P("OFF_INPUT_RING", kOffInputRing); P("OFF_COLLISION_RING", kOffCollisionRing); P("COLLISION_RING_BYTES", kCollisionRingBytes);
	P("OFF_OVERLAY_PIXELS", kOffOverlayPixels); P("MAX_OVERLAY_W", kMaxOverlayW); P("MAX_OVERLAY_H", kMaxOverlayH);
	P("OVERLAY_SLOT_BYTES", kOverlaySlotBytes); P("OVERLAY_SLOTS", kOverlaySlots); P("OFF_ACTOR_TABLE", kOffActorTable);
	P("OFF_EVENT_RING", kOffEventRing); P("OFF_WORLD_ENTITIES", kOffWorldEntities); P("OFF_RENDER_RING", kOffRenderRing);
	P("RENDER_RING_BYTES", kRenderRingBytes); P("MAPPING_BYTES", kMappingBytes);
	O("H_MAGIC", Header, magic); O("H_VERSION", Header, version); O("H_SKYRIM_PID", Header, skyrimPid); O("H_MC_PID", Header, mcPid);
	O("H_SKYRIM_HEARTBEAT", Header, skyrimHeartbeatMs); O("H_MC_HEARTBEAT", Header, mcHeartbeatMs);
	O("WG_SEQ", WaterGrid, seq); O("WG_ORIGIN_X", WaterGrid, originX); O("WG_ORIGIN_Z", WaterGrid, originZ); O("WG_WORLD_ID", WaterGrid, worldId);
	O("WG_SURFACE", WaterGrid, surface); P("WATER_GRID_SIZE", kWaterGridSize);
	O("SS_SEQ", SkyState, seq); O("SS_FLAGS", SkyState, flags); O("SS_WORLD_ID", SkyState, worldId); O("SS_TRANSIT_ACK", SkyState, transitAck);
	O("SS_POS_X", SkyState, posX); O("SS_POS_Y", SkyState, posY); O("SS_POS_Z", SkyState, posZ); O("SS_YAW", SkyState, yaw); O("SS_PITCH", SkyState, pitch);
	O("SS_TELEPORT_SEQ", SkyState, teleportSeq); O("SS_VIEWPORT_W", SkyState, viewportW); O("SS_VIEWPORT_H", SkyState, viewportH); O("SS_GAME_HOUR", SkyState, gameHour);
	P("SKY_IN_GAME", kSkyInGame); P("SKY_MENU_OPEN", kSkyMenuOpen); P("SKY_LOADING", kSkyLoading);
	O("MS_SEQ", McState, seq); O("MS_FLAGS", McState, flags); O("MS_X", McState, x); O("MS_Y", McState, y); O("MS_Z", McState, z);
	O("MS_YAW", McState, yaw); O("MS_PITCH", McState, pitch); O("MS_EYE_HEIGHT", McState, eyeHeight); O("MS_SENSITIVITY", McState, sensitivity);
	O("MS_TELEPORT_ACK", McState, teleportAck); O("MS_GUI_SCALE", McState, guiScale); O("MS_FRAME_COUNTER", McState, frameCounter);
	O("MS_FOV", McState, fovDeg); O("MS_BOB_PHASE", McState, bobPhase); O("MS_BOB_AMOUNT", McState, bobAmount);
	O("MS_EYE_X", McState, eyeX); O("MS_EYE_Y", McState, eyeY); O("MS_EYE_Z", McState, eyeZ); O("MS_TICK_QPC", McState, tickQpc);
	O("MS_PREV_X", McState, prevX); O("MS_CUR_X", McState, curX); O("MS_EYE_HEIGHT_O", McState, tickEyeO); O("MS_EYE_HEIGHT_T", McState, tickEye);
	O("MS_WALK_O", McState, walkDistO); O("MS_WALK", McState, walkDist); O("MS_BOB_O", McState, bobO); O("MS_BOB", McState, bob);
	O("MS_TICK_MS", McState, tickMs); O("MS_CAMERA_MODE", McState, cameraMode); O("MS_CAMERA_DISTANCE", McState, cameraDistance);
	O("MS_RIDE_MOVER", McState, rideMover); O("MS_RIDE_FLAGS", McState, rideFlags); O("MS_RIDE_PREV_X", McState, ridePrevX); O("MS_RIDE_CUR_X", McState, rideCurX); P("RIDE_MOVED", kRideMoved); P("RIDE_PUSHED", kRidePushed);
	O("MS_TRANSIT_SEQ", McState, transitSeq); O("MS_TRANSIT_PORTALS", McState, transitPortals);
	P("OFF_MC_TRANSIT", kOffMcTransit); O("MT_Q", McTransit, q); O("MT_OFF", McTransit, off); O("MT_FRAC", McTransit, frac); O("MT_SEQ", McTransit, seq); P("MC_TRANSIT_BYTES", sizeof(McTransit));
	P("MC_STATE_BYTES", sizeof(McState));
	P("MC_IN_WORLD", kMcInWorld); P("MC_SCREEN_OPEN", kMcScreenOpen); P("MC_ON_GROUND", kMcOnGround); P("MC_SNEAKING", kMcSneaking);
	P("MC_SPRINTING", kMcSprinting); P("MC_DEAD", kMcDead); P("MC_SWIMMING", kMcSwimming); P("MC_FLYING", kMcFlying);
	O("OC_STATE", OverlayCtl, state); O("OC_FRAMES_PUBLISHED", OverlayCtl, framesPublished); P("OVERLAY_DIRTY", kOverlayDirty);
	P("SLOT_HDR_SIZE", sizeof(OverlaySlotHdr)); O("SH_WIDTH", OverlaySlotHdr, width); O("SH_HEIGHT", OverlaySlotHdr, height);
	O("SH_FLAGS", OverlaySlotHdr, flags); O("SH_FRAME_ID", OverlaySlotHdr, frameId);
	O("SH_VIEW_REQUEST_ID", OverlaySlotHdr, viewRequestId); O("SH_DEPTH_OFFSET", OverlaySlotHdr, depthOffset);
	O("SH_Z_NEAR", OverlaySlotHdr, zNear); O("SH_Z_FAR", OverlaySlotHdr, zFar); O("SH_DEPTH_W", OverlaySlotHdr, depthW); O("SH_DEPTH_H", OverlaySlotHdr, depthH); P("SLOT_BOTTOM_UP", kSlotBottomUp); P("SLOT_WORLD_VIEW", kSlotWorldView); P("SLOT_DEPTH_ALPHA", kSlotDepthAlpha);
	P("OFF_HOST_VIEW", kOffHostView); P("HOST_VIEW_BYTES", sizeof(HostView));
	O("HV_SEQ", HostView, seq); O("HV_FLAGS", HostView, flags); O("HV_X", HostView, x); O("HV_Y", HostView, y); O("HV_Z", HostView, z);
	O("HV_YAW", HostView, yaw); O("HV_PITCH", HostView, pitch); O("HV_ROLL", HostView, roll); O("HV_FOV_Y", HostView, fovY);
	O("HV_PLANE", HostView, plane); O("HV_REQUEST_ID", HostView, requestId);
	P("OFF_HOST_VIEW_EXT", kOffHostViewExt); P("HOST_VIEW_EXT_BYTES", sizeof(HostViewExt)); P("EXT_VALID", kExtValid);
	O("HX_SEQ", HostViewExt, seq); O("HX_FLAGS", HostViewExt, flags); O("HX_CROP", HostViewExt, crop); O("HX_PRIMARY_REQUEST_ID", HostViewExt, primaryRequestId);
	O("HX_PRIMARY_STREAM", HostViewExt, primaryStream); O("HX_SX", HostViewExt, sx); O("HX_SY", HostViewExt, sy); O("HX_SZ", HostViewExt, sz);
	O("HX_SYAW", HostViewExt, syaw); O("HX_SPITCH", HostViewExt, spitch); O("HX_SROLL", HostViewExt, sroll); O("HX_SFOV_Y", HostViewExt, sfovY);
	O("HX_SPLANE", HostViewExt, splane); O("HX_SCROP", HostViewExt, scrop); O("HX_SEC_REQUEST_ID", HostViewExt, secRequestId); O("HX_SEC_STREAM", HostViewExt, secStream);
	O("HX_SEC_FLAGS", HostViewExt, secFlags); O("HX_SEC_RENDER_DISTANCE", HostViewExt, secRenderDistance);
	P("OFF_HOST_PORTALS", kOffHostPortals); P("HOST_PORTALS_BYTES", sizeof(HostPortals)); P("HOST_PORTAL_BYTES", sizeof(HostPortal)); P("PORTALS_IN_MC", kPortalsInMc);
	O("HP_SEQ", HostPortals, seq); O("HP_FLAGS", HostPortals, flags); O("HP_PRIMARY_REQUEST_ID", HostPortals, primaryRequestId); O("HP_MAX_DEPTH", HostPortals, maxDepth);
	O("HP_RENDER_DISTANCE", HostPortals, renderDistance); O("HP_RES_SCALE", HostPortals, resScale); O("HP_PORTAL", HostPortals, portal);
	O("PO_CX", HostPortal, cx); O("PO_CY", HostPortal, cy); O("PO_CZ", HostPortal, cz); O("PO_NX", HostPortal, nx); O("PO_NY", HostPortal, ny); O("PO_NZ", HostPortal, nz);
	O("PO_UX", HostPortal, ux); O("PO_UY", HostPortal, uy); O("PO_UZ", HostPortal, uz); O("PO_HALF_W", HostPortal, halfW); O("PO_HALF_H", HostPortal, halfH);
	O("PO_OPEN", HostPortal, open); O("PO_COLOUR", HostPortal, colour);
	P("MAX_LIGHT_PROBES", kMaxLightProbes); P("OFF_LIGHT_PROBES", kOffLightProbes); P("OFF_MC_LIGHT", kOffMcLight);
	P("LIGHT_PROBES_BYTES", sizeof(LightProbes)); P("LIGHT_SAMPLE_BYTES", sizeof(LightSample)); P("MC_LIGHT_BYTES", sizeof(McLight));
	O("LP_SEQ", LightProbes, seq); O("LP_COUNT", LightProbes, count); O("LP_REQUEST_ID", LightProbes, requestId); O("LP_POS", LightProbes, pos);
	O("LS_R", LightSample, r); O("LS_G", LightSample, g); O("LS_B", LightSample, b); O("LS_SKY", LightSample, sky); O("LS_BLOCK", LightSample, block); O("LS_SKY_VISIBLE", LightSample, skyVisible);
	O("ML_SEQ", McLight, seq); O("ML_REQUEST_ID", McLight, requestId); O("ML_COUNT", McLight, count); O("ML_FLAGS", McLight, flags); O("ML_SUN_DIR", McLight, sunDir);
	O("ML_DAYLIGHT", McLight, daylight); O("ML_SKY_COLOR", McLight, skyColor); O("ML_DAY_TIME", McLight, dayTime); O("ML_SKY_LIGHT_COLOR", McLight, skyLightColor); O("ML_SAMPLES", McLight, s);
	P("MAX_HUD_W", kMaxHudW); P("MAX_HUD_H", kMaxHudH); P("HUD_SLOT_BYTES", kHudSlotBytes); P("HUD_SLOTS", kHudSlots); P("OFF_HUD_PIXELS", kOffHudPixels);
	P("OFF_HOST_PLAYER", kOffHostPlayer); P("HOST_PLAYER_BYTES", sizeof(HostPlayer)); P("HP_MC_MODE", kHpMcMode); P("HP_WANT_HUD", kHpWantHud); P("HP_WANT_AVATAR", kHpWantAvatar); P("HP_AVATAR_IN_MC", kHpAvatarInMc); P("HP_MC_TRANSIT", kHpMcTransit); P("HP_HAND_OVER", kHpHandOver); P("HP_DIG", kHpDig); P("HP_STAGE", kHpStage); O("HPL_STAGE_CX", HostPlayer, stageChunkX); O("HPL_STAGE_CZ", HostPlayer, stageChunkZ); O("HPL_STAGE_R", HostPlayer, stageRadius);
	O("HPL_SEQ", HostPlayer, seq); O("HPL_FLAGS", HostPlayer, flags); O("HPL_SCREEN_W", HostPlayer, screenW); O("HPL_SCREEN_H", HostPlayer, screenH);
	O("HPL_CURSOR_X", HostPlayer, cursorX); O("HPL_CURSOR_Y", HostPlayer, cursorY);
	O("HPL_DIG_MATERIAL", HostPlayer, digMaterial); O("HPL_DIG_WORLD", HostPlayer, digWorld); P("COL_BLOCK_NO_DIG", kColBlockNoDig); P("COL_BLOCK_LIMIT", kColBlockLimit); P("COL_BLOCK_VIS_KNOWN", kColBlockVisKnown); P("COL_BLOCK_VIS_ANY", kColBlockVisAny); P("COL_BLOCK_VIS_SAMPLES_SHIFT", kColBlockVisSamplesShift); P("COL_BLOCK_VIS_SAMPLES_MASK", kColBlockVisSamplesMask); P("CH_NONE", kChNone); P("CH_PANEL_WHITE", kChPanelWhite); P("CH_PANEL_BLACK", kChPanelBlack); P("CH_METAL", kChMetal); P("CH_TILE", kChTile); P("CH_GLASS", kChGlass); P("CH_CONCRETE", kChConcrete); P("CH_PORTALABLE_BIT", kChPortalableBit);  // bridge #33
	O("HPL_HAND_LIGHT", HostPlayer, handLight); O("HPL_HAND_LIGHT_SEQ", HostPlayer, handLightSeq); O("HPL_HAND_TONE", HostPlayer, handTone);  // bridge #27
	P("OFF_HUD_CTL", kOffHudCtl); P("HUD_CTL_BYTES", sizeof(HudCtl)); P("HUD_SLOT_HDR_BYTES", sizeof(HudSlotHdr));
	O("HC_STATE", HudCtl, state); O("HC_FRAMES_PUBLISHED", HudCtl, framesPublished); O("HC_SLOT", HudCtl, slot);
	O("HS_WIDTH", HudSlotHdr, width); O("HS_HEIGHT", HudSlotHdr, height); O("HS_FLAGS", HudSlotHdr, flags); O("HS_GUI_SCALE", HudSlotHdr, guiScale);
	O("HS_FRAME_ID", HudSlotHdr, frameId); O("HS_MC_FLAGS", HudSlotHdr, mcFlags); O("HS_CAMERA_MODE", HudSlotHdr, cameraMode);
	P("OFF_HOST_AUDIO", kOffHostAudio); P("HOST_AUDIO_BYTES", sizeof(HostAudio)); P("AUDIO_ACTIVE", kAudioActive);
	O("HA_SEQ", HostAudio, seq); O("HA_FLAGS", HostAudio, flags); O("HA_X", HostAudio, x); O("HA_Y", HostAudio, y); O("HA_Z", HostAudio, z);
	O("HA_YAW", HostAudio, yaw); O("HA_PITCH", HostAudio, pitch); O("HA_GAIN", HostAudio, gain); O("HA_LEVEL", HostAudio, level); P("AUDIO_CHAMBER", kAudioChamber); P("AUDIO_LOG_SOUNDS", kAudioLogSounds);
	O("SH_IMG2_OFFSET", OverlaySlotHdr, img2Offset); O("SH_IMG2_W", OverlaySlotHdr, img2W); O("SH_IMG2_H", OverlaySlotHdr, img2H);
	O("SH_IMG2_REQUEST_ID", OverlaySlotHdr, img2RequestId); O("SH_IMG2_FLAGS", OverlaySlotHdr, img2Flags);
	P("VIEW_ACTIVE", kViewActive); P("VIEW_CLIP_PLANE", kViewClipPlane); P("VIEW_FULL_SCREEN", kViewFullScreen); P("VIEW_SCREENSHOT", kViewScreenshot);
	P("INPUT_RING_ENTRIES", kInputRingEntries); P("IR_HEAD", kInputRingHeadOff); P("IR_TAIL", kInputRingTailOff); P("IR_DATA", kInputRingDataOff);
	P("IN_KEY", kInKey); P("IN_MOUSE_BUTTON", kInMouseButton); P("IN_SCROLL", kInScroll); P("IN_CURSOR", kInCursor); P("IN_TEXT", kInText);
	P("IN_RELEASE_ALL", kInReleaseAll); P("IN_HURT", kInHurt); P("IN_OPEN_MENU", kInOpenMenu); P("IN_PORTAL", kInPortal);
	P("MAX_ACTORS", kMaxActors); O("AT_SEQ", ActorTable, seq); O("AT_COUNT", ActorTable, count); O("AT_RECORDS", ActorTable, actors); P("ACTOR_RECORD_BYTES", sizeof(ActorRecord));
	P("EVENT_RING_ENTRIES", kEventRingEntries); P("ER_HEAD", kEventRingHeadOff); P("ER_TAIL", kEventRingTailOff); P("ER_DATA", kEventRingDataOff); P("EVENT_BYTES", sizeof(McEvent));
	P("MAX_WORLD_ENTITIES", kMaxWorldEntities); O("WE_SEQ", WorldEntities, seq); O("WE_COUNT", WorldEntities, count); O("WE_HAS_SELECTION", WorldEntities, hasSelection);
	O("WE_SEL_MIN", WorldEntities, selMin); O("WE_SEL_MAX", WorldEntities, selMax); O("WE_RECORDS", WorldEntities, entities); P("WORLD_ENTITY_BYTES", sizeof(WorldEntity));
	P("RR_HEAD", kRenRingHeadOff); P("RR_TAIL", kRenRingTailOff); P("RR_DATA", kRenRingDataOff); P("RR_DATA_BYTES", kRenRingDataBytes);
	P("REN_PAD", kRenPad); P("REN_ATLAS", kRenAtlas); P("REN_SECTION", kRenSection); P("REN_CLEAR_ALL", kRenClearAll); P("REN_TEXTURE", kRenTexture);
	P("REN_AVATAR", kRenAvatar); P("REN_SCENE", kRenScene); P("REN_ATLAS_REGION", kRenAtlasRegion); P("REN_LIGHTS", kRenLights); P("REN_RAGDOLL", kRenRagdoll);
	P("REN_SOLIDS", kRenSolids); P("REN_DUG", kRenDug); P("REN_DUG_BYTES", sizeof(RenDug)); P("REN_MOVER_DIG", kRenMoverDig); P("REN_MOVER_DIG_BYTES", sizeof(RenMoverDig)); P("COL_MOVER_LATTICE", kColMoverLattice); P("COL_MOVER_LATTICE_BYTES", sizeof(ColMoverLattice)); P("LATTICE_NO_PICK", kLatticeNoPick); P("LATTICE_NO_MINE", kLatticeNoMine); P("MOVER_DIG_STYLE", kMoverDigStyle); P("MOVER_DIG_KIND_SHIFT", kMoverDigKindShift); P("REN_MOVER_STYLE_BYTES", sizeof(RenMoverStyle)); P("REN_VERTEX_BYTES", sizeof(RenVertex));
	P("CR_HEAD", kColRingHeadOff); P("CR_TAIL", kColRingTailOff); P("CR_DATA", kColRingDataOff); P("CR_DATA_BYTES", kColRingDataBytes);
	P("COL_PAD", kColPad); P("COL_CLEAR", kColClear); P("COL_REGION", kColRegion); P("COL_TRIS", kColTris); P("COL_MOVER_SHAPE", kColMoverShape); P("COL_MOVER_BIND", kColMoverBind); P("COL_MOVER_SHAPE_BYTES", sizeof(ColMoverShape)); P("COL_MOVER_BOX_BYTES", sizeof(ColMoverBox)); P("COL_MOVER_BIND_BYTES", sizeof(ColMoverBind));
	P("OFF_MC_MOVERS", kOffMcMovers); P("MAX_MOVERS", kMaxMovers); O("MV_SEQ", McMovers, seq); O("MV_COUNT", McMovers, count); O("MV_EPOCH", McMovers, epoch); O("MV_FLAGS", McMovers, flags);
	O("MV_FRAME", McMovers, frame); O("MV_BASE_X", McMovers, baseX); O("MV_POSES", McMovers, movers); P("MOVER_POSE_BYTES", sizeof(MoverPose));
	O("MP_ID", MoverPose, moverId); O("MP_X", MoverPose, x); O("MP_QX", MoverPose, qx); O("MP_QW", MoverPose, qw); P("COL_TRI_BYTES", sizeof(ColTri));
	P("TRI_STAIR_HELPER", kTriStairHelper); P("COL_REGION_HEADER_BYTES", sizeof(ColRegion)); P("COL_BLOCK_BYTES", sizeof(ColBlock));
	return 0;
}
