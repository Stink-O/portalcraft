// Portalcraft bridge: the host end of SkyCraft's shared-memory link, ported to Linux.
//
// SkyCraft's Skyrim plugin creates a Win32 file mapping "Local\SkyCraft_v1"; here the Portal 2
// plugin creates POSIX shared memory "/Portalcraft_v1" (the file /dev/shm/Portalcraft_v1), sized and
// laid out exactly as protocol/skycraft_protocol.h says, and the Fabric mod maps that file.
// Heartbeats use CLOCK_MONOTONIC milliseconds and tick timestamps CLOCK_MONOTONIC nanoseconds
// on both sides (Java's System.nanoTime() is CLOCK_MONOTONIC on Linux).
#include "link.hpp"
#include <cstdlib>
#include <cstdio>
#include <string>

#include "Modules/Console.hpp"

#include <cerrno>
#include <cstring>

#include "platform/platform.hpp"

namespace bridge { std::string CacheDir(); }
namespace bridge::link {

using namespace skycraft::proto;

static uint8_t *g_base = nullptr;
static uint32_t g_skySeq = 0;
static int64_t g_mcPidSeen = 0;

template <typename T> static T *At(uint64_t off) { return reinterpret_cast<T *>(g_base + off); }

uint64_t NowMs() {
	const int64_t ns = plat::MonotonicNs();  // (Linux: CLOCK_MONOTONIC; Windows: QueryPerformanceCounter)
	return (uint64_t)(ns / 1000000000LL) * 1000 + (uint64_t)(ns % 1000000000LL) / 1000000;
}
int64_t NowNs() { return plat::MonotonicNs(); }

uint8_t *Base() { return g_base; }

bool Create() {
	if (g_base) return true;
	// A fresh object every run: an old Minecraft still mapping the previous one sees our new
	// file (different inode) and relinks (Linux); the platform layer does the OS part.
	std::string what, err;
	uint8_t *p = plat::ShmCreate(kMappingBytes, what, err);
	if (!p) {
		console->Warn("[bridge] link: %s\n", err.c_str());
		return false;
	}
	g_base = p;
	if (plat::ShmWasExisting()) {
		// (Windows) a Minecraft from the previous run still holds the named mapping, so it is the same object with the old contents:
		// clear the header page and the rings (everything before the overlay pixels); the new host pid below makes the mod relink.
		std::memset(g_base, 0, (size_t)kOffOverlayPixels);
		console->Print("[bridge] link: the mapping already existed (a Minecraft from the previous run holds it); cleared the header page and the rings\n");
	}
	// tmpfs pages start zeroed: every ring and seqlock starts empty (a Windows mapping is zeroed as well).
	auto *h = At<Header>(kOffHeader);
	h->version = kVersion;
	// (A/B tests against an older mod: a file cache/bridge/proto_version holding e.g. 14 announces that older
	// protocol, so a mod built before v15 links; nothing newer is read by it then)
	if (FILE *pf = std::fopen((CacheDir() + "/proto_version").c_str(), "r")) {
		int v = 0;
		if (std::fscanf(pf, "%d", &v) == 1 && v >= 11 && v <= (int)kVersion) h->version = (uint32_t)v;
		std::fclose(pf);
		console->Print("[bridge] link: announcing protocol %u (cache/bridge/proto_version)\n", h->version);
	}
	h->skyrimPid = plat::LinkHostPid();
	h->mcPid = 0;
	__atomic_store_n(&h->skyrimHeartbeatMs, NowMs(), __ATOMIC_RELEASE);
	__atomic_store_n(&h->magic, kMagic, __ATOMIC_RELEASE);  // last: the mod checks magic first
	console->Print("[bridge] link: %s created, %llu MB, host pid %d\n", what.c_str(), (unsigned long long)(kMappingBytes >> 20), (int)plat::LinkHostPid());
	return true;
}

void Destroy() {
	if (!g_base) return;
	plat::ShmDestroy();
	g_base = nullptr;
}

void Heartbeat() {
	if (!g_base) return;
	__atomic_store_n(&At<Header>(kOffHeader)->skyrimHeartbeatMs, NowMs(), __ATOMIC_RELEASE);
}

bool McAlive() {
	if (!g_base) return false;
	auto *h = At<Header>(kOffHeader);
	uint32_t pid = __atomic_load_n(&h->mcPid, __ATOMIC_ACQUIRE);
	if (!pid) return false;
	uint64_t beat = __atomic_load_n(&h->mcHeartbeatMs, __ATOMIC_ACQUIRE);
#ifdef _WIN32
	{  // PCB_LINK_FILE under Wine: bring the host clock onto the native Minecraft's CLOCK_MONOTONIC (see platform.hpp)
		static const bool dev = !plat::Getenv("PCB_LINK_FILE").empty();
		static uint64_t lastBeat = 0;
		if (dev && beat && beat != lastBeat) lastBeat = beat, plat::LinkClockSample((int64_t)NowNs() - (int64_t)beat * 1000000LL);
	}
#endif
	uint64_t now = NowMs();
	if (pid != g_mcPidSeen) {
		g_mcPidSeen = pid;
		console->Print("[bridge] link: Minecraft pid %u attached\n", pid);
	}
	return now - beat < 8000;
}

uint32_t McPid() { return g_base ? At<Header>(kOffHeader)->mcPid : 0; }

void WriteSkyState(const SkyState &in) {
	if (!g_base) return;
	auto *s = At<SkyState>(kOffSkyState);
	uint32_t seq = g_skySeq;
	__atomic_store_n(&s->seq, seq + 1, __ATOMIC_RELEASE);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	SkyState copy = in;
	copy.seq = seq + 1;
	std::memcpy((uint8_t *)s + 4, (const uint8_t *)&copy + 4, sizeof(SkyState) - 4);
	__atomic_store_n(&s->seq, seq + 2, __ATOMIC_RELEASE);
	g_skySeq = seq + 2;
}

static uint32_t g_viewSeq = 0;
void WriteHostView(const HostView &in) {
	if (!g_base) return;
	auto *v = At<HostView>(kOffHostView);
	uint32_t seq = g_viewSeq;
	__atomic_store_n(&v->seq, seq + 1, __ATOMIC_RELEASE);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	HostView copy = in;
	copy.seq = seq + 1;
	std::memcpy((uint8_t *)v + 4, (const uint8_t *)&copy + 4, sizeof(HostView) - 4);
	__atomic_store_n(&v->seq, seq + 2, __ATOMIC_RELEASE);
	g_viewSeq = seq + 2;
}

static uint32_t g_extSeq = 0;
void WriteHostViewExt(const HostViewExt &in) {
	if (!g_base) return;
	auto *v = At<HostViewExt>(kOffHostViewExt);
	uint32_t seq = g_extSeq;
	__atomic_store_n(&v->seq, seq + 1, __ATOMIC_RELEASE);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	HostViewExt copy = in;
	copy.seq = seq + 1;
	std::memcpy((uint8_t *)v + 4, (const uint8_t *)&copy + 4, sizeof(HostViewExt) - 4);
	__atomic_store_n(&v->seq, seq + 2, __ATOMIC_RELEASE);
	g_extSeq = seq + 2;
}

// v15: the live portal pair Minecraft draws itself (write before WriteHostViewExt / WriteHostView).
static uint32_t g_portalsSeq = 0;
void WriteHostPortals(const HostPortals &in) {
	if (!g_base) return;
	auto *v = At<HostPortals>(kOffHostPortals);
	uint32_t seq = g_portalsSeq;
	__atomic_store_n(&v->seq, seq + 1, __ATOMIC_RELEASE);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	HostPortals copy = in;
	copy.seq = seq + 1;
	std::memcpy((uint8_t *)v + 4, (const uint8_t *)&copy + 4, sizeof(HostPortals) - 4);
	__atomic_store_n(&v->seq, seq + 2, __ATOMIC_RELEASE);
	g_portalsSeq = seq + 2;
}

// Overlay triple buffer, reader side: take the middle slot when it holds an unread frame.
// Initial ownership (SkyLink.java): writer's back slot 1, middle 0 (state 0), the reader's front 2.
static uint32_t g_overlayFront = 2;
bool AcquireOverlay(const OverlaySlotHdr *&hdr, const uint8_t *&pixels) {
	if (!g_base) return false;
	auto *ctl = At<OverlayCtl>(kOffOverlayCtl);
	uint32_t st = __atomic_load_n(&ctl->state, __ATOMIC_ACQUIRE);
	if (!(st & kOverlayDirty)) return false;
	uint32_t old = __atomic_exchange_n(&ctl->state, g_overlayFront, __ATOMIC_ACQ_REL);
	g_overlayFront = old & 3;
	hdr = At<OverlaySlotHdr>(kOffOverlaySlotHdr + g_overlayFront * sizeof(OverlaySlotHdr));
	pixels = g_base + kOffOverlayPixels + g_overlayFront * kOverlaySlotBytes;
	return true;
}

// v17: the Minecraft player's HUD triple buffer, reader side (glhud.cpp, on the render thread only).
static uint32_t g_hudFront = 2;
bool AcquireHud(const HudSlotHdr *&hdr, const uint8_t *&pixels) {
	if (!g_base) return false;
	auto *ctl = At<HudCtl>(kOffHudCtl);
	uint32_t st = __atomic_load_n(&ctl->state, __ATOMIC_ACQUIRE);
	if (!(st & kOverlayDirty)) return false;
	uint32_t old = __atomic_exchange_n(&ctl->state, g_hudFront, __ATOMIC_ACQ_REL);
	g_hudFront = old & 3;
	hdr = &ctl->slot[g_hudFront];
	pixels = g_base + kOffHudPixels + g_hudFront * kHudSlotBytes;
	return true;
}
uint64_t HudFramesPublished() { return g_base ? __atomic_load_n(&At<HudCtl>(kOffHudCtl)->framesPublished, __ATOMIC_ACQUIRE) : 0; }

static uint32_t g_playerSeq = 0;
static float g_handLight[3] = {1, 1, 1}, g_handTone = 1;
static uint32_t g_handLightSeq = 0;
void SetHostHandLight(bool on, float r, float g, float b, float tone) {
	if (!on) {
		g_handLightSeq = 0;
		return;
	}
	g_handLight[0] = r, g_handLight[1] = g, g_handLight[2] = b, g_handTone = tone;
	if (++g_handLightSeq == 0) g_handLightSeq = 1;
}
void WriteHostPlayer(const HostPlayer &in) {
	if (!g_base) return;
	auto *v = At<HostPlayer>(kOffHostPlayer);
	uint32_t seq = g_playerSeq;
	__atomic_store_n(&v->seq, seq + 1, __ATOMIC_RELEASE);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	HostPlayer copy = in;
	copy.seq = seq + 1;
	// bridge #27: the hand light comes from bandlight.cpp (SetHostHandLight), not from the writer's struct
	copy.handLight[0] = g_handLight[0], copy.handLight[1] = g_handLight[1], copy.handLight[2] = g_handLight[2];
	copy.handLightSeq = g_handLightSeq;
	copy.handTone = g_handTone;
	std::memcpy((uint8_t *)v + 4, (const uint8_t *)&copy + 4, sizeof(HostPlayer) - 4);
	__atomic_store_n(&v->seq, seq + 2, __ATOMIC_RELEASE);
	g_playerSeq = seq + 2;
}

static uint32_t g_moversSeq = 0;
McMovers *MoversBegin() {
	if (!g_base) return nullptr;
	auto *v = At<McMovers>(kOffMcMovers);
	__atomic_store_n(&v->seq, g_moversSeq + 1, __ATOMIC_RELEASE);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	return v;
}
void MoversEnd() {
	if (!g_base) return;
	auto *v = At<McMovers>(kOffMcMovers);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	__atomic_store_n(&v->seq, g_moversSeq + 2, __ATOMIC_RELEASE);
	g_moversSeq += 2;
}

static McTransit g_mcTransit{};  // (bridge #28) read with McState, under its seqlock
const McTransit &LastMcTransit() { return g_mcTransit; }
bool ReadMcState(McState &out) {
	if (!g_base) return false;
	auto *s = At<McState>(kOffMcState);
	for (int i = 0; i < 1000; ++i) {
		uint32_t a = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
		if (a & 1) continue;
		std::memcpy(&out, s, sizeof out);
		std::memcpy(&g_mcTransit, At<McTransit>(kOffMcTransit), sizeof g_mcTransit);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(&s->seq, __ATOMIC_ACQUIRE) == a) return a != 0;
	}
	return false;
}

// (bridge #24) the bug mark's input record (bugmark.cpp): every input Minecraft is sent
static void (*g_inputTap)(uint16_t, uint16_t, int32_t, int32_t, int32_t) = nullptr;
void SetInputTap(void (*fn)(uint16_t, uint16_t, int32_t, int32_t, int32_t)) { g_inputTap = fn; }
void PushInput(uint16_t type, uint16_t code, int32_t a, int32_t b, int32_t c) {
	if (!g_base) return;
	if (g_inputTap) g_inputTap(type, code, a, b, c);
	uint8_t *ring = g_base + kOffInputRing;
	auto *head = (uint64_t *)(ring + kInputRingHeadOff);
	uint64_t h = __atomic_load_n(head, __ATOMIC_RELAXED);
	auto *e = (InputEvent *)(ring + kInputRingDataOff + (h & (kInputRingEntries - 1)) * sizeof(InputEvent));
	e->type = type;
	e->code = code;
	e->a = a;
	e->b = b;
	e->c = c;
	__atomic_store_n(head, h + 1, __ATOMIC_RELEASE);  // the consumer drops the oldest if lapped
}

// Byte ring producer shared by the collision ring (same layout as the render ring).
static bool RingWrite(uint64_t ringOff, uint64_t ringBytes, uint32_t type, const void *hdr, uint32_t hdrBytes, const void *body, uint32_t bodyBytes) {
	uint8_t *ring = g_base + ringOff;
	auto *head = (uint64_t *)(ring + 0x00);
	auto *tail = (uint64_t *)(ring + 0x40);
	const uint64_t dataBytes = ringBytes - 0x80;
	uint8_t *data = ring + 0x80;
	uint32_t payload = hdrBytes + bodyBytes;
	uint64_t msg = (8 + payload + 7) & ~7ull;
	if (msg > dataBytes / 2) return false;
	uint64_t h = __atomic_load_n(head, __ATOMIC_RELAXED);
	uint64_t t = __atomic_load_n(tail, __ATOMIC_ACQUIRE);
	uint64_t pos = h % dataBytes;
	uint64_t pad = pos + msg > dataBytes ? dataBytes - pos : 0;
	if (dataBytes - (h - t) < msg + pad) return false;  // consumer behind: caller retries later
	if (pad) {
		*(uint32_t *)(data + pos) = 0;  // kColPad / kRenPad
		*(uint32_t *)(data + pos + 4) = 0;
		h += pad;
		pos = 0;
	}
	*(uint32_t *)(data + pos) = type;
	*(uint32_t *)(data + pos + 4) = payload;
	std::memcpy(data + pos + 8, hdr, hdrBytes);
	if (bodyBytes) std::memcpy(data + pos + 8 + hdrBytes, body, bodyBytes);
	__atomic_store_n(head, h + msg, __ATOMIC_RELEASE);
	return true;
}

bool WriteCollision(uint32_t type, const void *hdr, uint32_t hdrBytes, const void *body, uint32_t bodyBytes) {
	if (!g_base) return false;
	return RingWrite(kOffCollisionRing, kCollisionRingBytes, type, hdr, hdrBytes, body, bodyBytes);
}

uint64_t CollisionBacklog() {
	if (!g_base) return 0;
	uint8_t *ring = g_base + kOffCollisionRing;
	return __atomic_load_n((uint64_t *)ring, __ATOMIC_ACQUIRE) - __atomic_load_n((uint64_t *)(ring + 0x40), __ATOMIC_ACQUIRE);
}

}  // namespace bridge::link

namespace bridge { std::string CacheDir(); }
namespace bridge::link {
// Render ring consumer stand-in: until the mesh renderer consumes it, drop everything so
// Minecraft's producer never blocks on a full ring.
void DiscardRender() {
	if (!g_base) return;
	uint8_t *ring = g_base + kOffRenderRing;
	uint64_t h = __atomic_load_n((uint64_t *)(ring + kRenRingHeadOff), __ATOMIC_ACQUIRE);
	__atomic_store_n((uint64_t *)(ring + kRenRingTailOff), h, __ATOMIC_RELEASE);
}

// Actor table (host -> Minecraft, seqlock): the host's actors mirrored as hittable stand-ins.
static uint32_t g_actorSeq = 0;
void WriteActors(const ActorRecord *recs, uint32_t n) {
	if (!g_base) return;
	if (n > kMaxActors) n = kMaxActors;
	auto *t = At<ActorTable>(kOffActorTable);
	uint32_t seq = g_actorSeq;
	__atomic_store_n(&t->seq, seq + 1, __ATOMIC_RELEASE);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	t->count = n;
	if (n) std::memcpy(t->actors, recs, sizeof(ActorRecord) * n);
	__atomic_store_n(&t->seq, seq + 2, __ATOMIC_RELEASE);
	g_actorSeq = seq + 2;
}

// Event ring (Minecraft -> host): one event, or false when empty.
bool PopEvent(McEvent &out) {
	if (!g_base) return false;
	uint8_t *ring = g_base + kOffEventRing;
	auto *headp = (uint64_t *)(ring + kEventRingHeadOff);
	auto *tailp = (uint64_t *)(ring + kEventRingTailOff);
	uint64_t head = __atomic_load_n(headp, __ATOMIC_ACQUIRE), tail = __atomic_load_n(tailp, __ATOMIC_RELAXED);
	if (tail >= head) return false;
	if (head - tail > kEventRingEntries) tail = head - kEventRingEntries;  // lapped: drop the oldest
	std::memcpy(&out, ring + kEventRingDataOff + (tail & (kEventRingEntries - 1)) * sizeof(McEvent), sizeof(McEvent));
	__atomic_store_n(tailp, tail + 1, __ATOMIC_RELEASE);
	return true;
}
// Minecraft's audio listener (v14, seqlock).
static uint32_t g_audioSeq = 0;
void WriteHostAudio(const HostAudio &in) {
	if (!g_base) return;
	auto *a = At<HostAudio>(kOffHostAudio);
	uint32_t seq = g_audioSeq;
	__atomic_store_n(&a->seq, seq + 1, __ATOMIC_RELEASE);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	HostAudio copy = in;
	copy.seq = seq + 1;
	std::memcpy((uint8_t *)a + 4, (const uint8_t *)&copy + 4, sizeof(HostAudio) - 4);
	__atomic_store_n(&a->seq, seq + 2, __ATOMIC_RELEASE);
	g_audioSeq = seq + 2;
}
}  // namespace bridge::link

// ---- v16 light probes ----------------------------------------------------------------------
namespace bridge::link {
using namespace skycraft::proto;
static uint32_t g_probeSeq = 0;
void WriteLightProbes(const LightProbes &in) {
	if (!g_base) return;
	auto *p = At<LightProbes>(kOffLightProbes);
	uint32_t seq = g_probeSeq;
	__atomic_store_n(&p->seq, seq + 1, __ATOMIC_RELEASE);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	std::memcpy((uint8_t *)p + 4, (const uint8_t *)&in + 4, sizeof(LightProbes) - 4);
	__atomic_store_n(&p->seq, seq + 2, __ATOMIC_RELEASE);
	g_probeSeq = seq + 2;
}
bool ReadMcLight(McLight &out) {
	if (!g_base) return false;
	auto *s = At<McLight>(kOffMcLight);
	for (int i = 0; i < 64; ++i) {
		uint32_t a = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
		if (a & 1) continue;
		std::memcpy(&out, s, sizeof out);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(&s->seq, __ATOMIC_ACQUIRE) == a) return a != 0;
	}
	return false;
}
uint32_t AnnouncedVersion() { return g_base ? At<Header>(kOffHeader)->version : 0; }
}  // namespace bridge::link
