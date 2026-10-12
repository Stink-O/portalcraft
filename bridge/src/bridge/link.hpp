// Portalcraft bridge: host end of the SkyCraft shared-memory link (Linux). See link.cpp.
#pragma once

#include "skycraft_protocol.h"

#include <cstdint>

namespace bridge::link {

bool Create();   // shm_open + ftruncate + mmap, header initialised
void Destroy();  // unmap and unlink
uint8_t *Base();

uint64_t NowMs();  // CLOCK_MONOTONIC, the heartbeat clock
int64_t NowNs();   // CLOCK_MONOTONIC, the tick-timestamp clock (McState::tickQpc)

void Heartbeat();  // once per host frame
bool McAlive();    // Minecraft attached and its heartbeat is fresh (< 8 s)
uint32_t McPid();

void WriteSkyState(const skycraft::proto::SkyState &s);  // seqlock write (seq is managed here)
bool ReadMcState(skycraft::proto::McState &out);
const skycraft::proto::McTransit &LastMcTransit();  // (bridge #28) the McTransit block read with the last McState
void WriteActors(const skycraft::proto::ActorRecord *recs, uint32_t n);  // actor table (host actors as Minecraft stand-ins)
bool PopEvent(skycraft::proto::McEvent &out);
void WriteHostAudio(const skycraft::proto::HostAudio &in);              // Minecraft's audio listener (v14)                         // event ring (Minecraft -> host)         // seqlock read; false if torn or never written
void WriteHostView(const skycraft::proto::HostView &v);  // Portalcraft: the camera Minecraft renders
void WriteHostViewExt(const skycraft::proto::HostViewExt &v);  // v13: crop windows + the second camera (write before WriteHostView)
void WriteHostPortals(const skycraft::proto::HostPortals &v);
void WriteLightProbes(const skycraft::proto::LightProbes &p);  // v16: where the host wants Minecraft's light
bool ReadMcLight(skycraft::proto::McLight &out);                 // v16: Minecraft's light at those probes
uint32_t AnnouncedVersion();                                     // the protocol version in the header  // v15: the live pair drawn by Minecraft (write before WriteHostViewExt)
// Newest unread overlay frame (triple buffer, reader side); valid until the next call.
bool AcquireOverlay(const skycraft::proto::OverlaySlotHdr *&hdr, const uint8_t *&pixels);

// v17: the HUD image (render thread only) and the Minecraft player's host block.
bool AcquireHud(const skycraft::proto::HudSlotHdr *&hdr, const uint8_t *&pixels);
uint64_t HudFramesPublished();
void WriteHostPlayer(const skycraft::proto::HostPlayer &p);
void SetHostHandLight(bool on, float r, float g, float b, float tone);  // bridge #27: merged into every HostPlayer write
// v18 (bridge #26): the movers' poses, seqlock: fill the block between MoversBegin and MoversEnd (seq/epoch/frame are the caller's)
skycraft::proto::McMovers *MoversBegin();  // nullptr when unmapped
void MoversEnd();

void PushInput(uint16_t type, uint16_t code, int32_t a, int32_t b = 0, int32_t c = 0);
void SetInputTap(void (*fn)(uint16_t type, uint16_t code, int32_t a, int32_t b, int32_t c));  // (bridge #24) bugmark.cpp

// Collision ring message ({type, payloadBytes} + header + body). False if the ring is full.
bool WriteCollision(uint32_t type, const void *hdr, uint32_t hdrBytes, const void *body, uint32_t bodyBytes);
uint64_t CollisionBacklog();
void DiscardRender();

}  // namespace bridge::link
