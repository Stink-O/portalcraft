// Stand-in for the Portal 2 plugin when developing the Fabric mod: creates /dev/shm/Portalcraft_v1,
// beats the host heartbeat, writes a SkyState each frame (in game, the player parked at the given
// Minecraft position) and drains the render/collision rings, printing what arrives.
//   fakehost <seconds> [mcX mcY mcZ [camX camY camZ yaw pitch roll fovY [nx ny nz d]]]
// With a camera, it also asks Minecraft for its own frame from there (Portalcraft HostView), and
// for a screenshot of it 8 s and 16 s in (cache/bridge/mcview/).
// v13 (protocol HostViewExt): FAKEHOST_CROP="x0 y0 x1 y1" crops the camera; FAKEHOST_SECOND="dx dy dz
// dyaw x0 y0 x1 y1 rd" adds a second camera (offset from the first, its crop, its render distance).
// (bridge #49) FAKEHOST_STAGE="chunkX chunkZ radius": the host asks for the Minecraft stage (HostPlayer kHpStage) around that chunk, as the plugin does for the portal onto
// Minecraft in a campaign chamber; the mod's log says "Portalcraft stage: chunk ... made" and the camera (second to fourth arguments) can look at it.
// The overlay slots are read back (both images) and saved once as PPMs to FAKEHOST_DUMP (a prefix).
#include "../../protocol/skycraft_protocol.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <initializer_list>

using namespace skycraft::proto;
static uint64_t Ms() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000ull + t.tv_nsec / 1000000; }

int main(int argc, char **argv) {
	int secs = argc > 1 ? atoi(argv[1]) : 60;
	double px = argc > 4 ? atof(argv[2]) : 0, py = argc > 4 ? atof(argv[3]) : 100, pz = argc > 4 ? atof(argv[4]) : 0;
	shm_unlink(kShmName);
	int fd = shm_open(kShmName, O_CREAT | O_RDWR, 0600);
	if (fd < 0 || ftruncate(fd, kMappingBytes) != 0) { perror("shm"); return 1; }
	auto *b = (uint8_t *)mmap(nullptr, kMappingBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	auto *h = (Header *)(b + kOffHeader);
	h->version = kVersion; h->skyrimPid = getpid(); h->skyrimHeartbeatMs = Ms(); __atomic_store_n(&h->magic, kMagic, __ATOMIC_RELEASE);
	auto *ss = (SkyState *)(b + kOffSkyState);
	auto *hv = (HostView *)(b + kOffHostView);
	auto *hpl = (HostPlayer *)(b + kOffHostPlayer);
	int stageCx = 0, stageCz = 0, stageR = 0;
	const bool stage = getenv("FAKEHOST_STAGE") && sscanf(getenv("FAKEHOST_STAGE"), "%d %d %d", &stageCx, &stageCz, &stageR) == 3;
	uint32_t hseq = 0;
	bool view = argc > 11;
	HostView cam{};
	if (view) {
		cam.flags = kViewActive | (getenv("FAKEHOST_FLAGS") ? (std::uint32_t)atoi(getenv("FAKEHOST_FLAGS")) : 0u);  // (bridge #49: FAKEHOST_FLAGS=4 asks as the host's main view)
		cam.x = atof(argv[5]), cam.y = atof(argv[6]), cam.z = atof(argv[7]);
		cam.yaw = atof(argv[8]), cam.pitch = atof(argv[9]), cam.roll = atof(argv[10]), cam.fovY = atof(argv[11]);
		if (argc > 15) {
			for (int i = 0; i < 4; ++i) cam.plane[i] = atof(argv[12 + i]);
			cam.flags |= kViewClipPlane;
		}
	}
	auto *hx = (HostViewExt *)(b + kOffHostViewExt);
	float crop[4] = {-1, -1, 1, 1}, scrop[4] = {-1, -1, 1, 1}, sd[4] = {0, 0, 0, 0};
	int srd = 0;
	bool second = false;
	if (const char *c = getenv("FAKEHOST_CROP")) sscanf(c, "%f %f %f %f", &crop[0], &crop[1], &crop[2], &crop[3]);
	if (const char *c = getenv("FAKEHOST_SECOND")) second = sscanf(c, "%f %f %f %f %f %f %f %f %d", &sd[0], &sd[1], &sd[2], &sd[3], &scrop[0], &scrop[1], &scrop[2], &scrop[3], &srd) == 9;
	const char *dump = getenv("FAKEHOST_DUMP");
	uint32_t front = 2, xseq = 0;
	int dumped = 0;
	uint64_t images1 = 0, images2 = 0;
	uint64_t t0 = Ms(), framesSeen = 0;
	auto *octl = (OverlayCtl *)(b + kOffOverlayCtl);
	uint64_t end = Ms() + secs * 1000ull; uint32_t seq = 0; uint64_t sections = 0, verts = 0, msgs = 0, lastPrint = 0;
	while (Ms() < end) {
		h->skyrimHeartbeatMs = Ms();
		__atomic_store_n(&ss->seq, seq + 1, __ATOMIC_RELEASE);
		ss->flags = kSkyInGame; ss->posX = px; ss->posY = py; ss->posZ = pz; ss->teleportSeq = 1; ss->viewportW = 1280; ss->viewportH = 720; ss->gameHour = 12;
		__atomic_store_n(&ss->seq, seq + 2, __ATOMIC_RELEASE); seq += 2;
		if (stage) {
			__atomic_store_n(&hpl->seq, hseq + 1, __ATOMIC_RELEASE);
			hpl->flags = kHpStage, hpl->stageChunkX = stageCx, hpl->stageChunkZ = stageCz, hpl->stageRadius = (uint32_t)stageR;
			__atomic_store_n(&hpl->seq, hseq + 2, __ATOMIC_RELEASE);
			hseq += 2;
		}
		if (view) {
			uint64_t t = Ms() - t0;
			HostView v = cam;
			v.requestId = seq / 2;
			static const char *delay = getenv("FAKEHOST_VIEW_DELAY_MS");
			if (delay && t < (uint64_t)atoi(delay)) v.flags = 0;
			static int shotAt[8] = {8, 16, 24, 0, 0, 0, 0, 0};  // seconds after the start (FAKEHOST_SHOTS="s1 s2 ..." (bridge #49): a slow Minecraft needs later ones)
			static bool shotsRead = false;
			if (!shotsRead) {
				shotsRead = true;
				if (const char *e = getenv("FAKEHOST_SHOTS")) {
					int n = sscanf(e, "%d %d %d %d %d %d %d %d", &shotAt[0], &shotAt[1], &shotAt[2], &shotAt[3], &shotAt[4], &shotAt[5], &shotAt[6], &shotAt[7]);
					for (int i = n < 0 ? 0 : n; i < 8; ++i) shotAt[i] = 0;
				}
			}
			for (int i = 0; i < 8; ++i)
				if (shotAt[i] > 0 && t > (uint64_t)shotAt[i] * 1000 && t < (uint64_t)shotAt[i] * 1000 + 300) v.flags |= kViewScreenshot;
			v.requestId = seq;  // odd ids: primary; the second camera gets seq + 1
			HostViewExt x{};
			x.flags = kExtValid;
			memcpy(x.crop, crop, sizeof crop);
			x.primaryRequestId = v.requestId;
			if (second && (v.flags & kViewActive)) {
				x.sx = v.x + sd[0], x.sy = v.y + sd[1], x.sz = v.z + sd[2];
				x.syaw = v.yaw + sd[3], x.spitch = v.pitch, x.sroll = 0, x.sfovY = v.fovY;
				memcpy(x.scrop, scrop, sizeof scrop);
				x.secRequestId = seq + 1, x.secStream = 1, x.secFlags = kViewActive, x.secRenderDistance = srd;
			}
			__atomic_store_n(&hx->seq, xseq + 1, __ATOMIC_RELEASE);
			std::memcpy((uint8_t *)hx + 4, (uint8_t *)&x + 4, sizeof x - 4);
			__atomic_store_n(&hx->seq, xseq + 2, __ATOMIC_RELEASE);
			xseq += 2;
			__atomic_store_n(&hv->seq, seq + 1, __ATOMIC_RELEASE);
			std::memcpy((uint8_t *)hv + 4, (uint8_t *)&v + 4, sizeof v - 4);
			__atomic_store_n(&hv->seq, seq + 2, __ATOMIC_RELEASE);
			framesSeen = octl->framesPublished;
			uint32_t st = __atomic_load_n(&octl->state, __ATOMIC_ACQUIRE);
			if (st & kOverlayDirty) {
				front = __atomic_exchange_n(&octl->state, front, __ATOMIC_ACQ_REL) & 3;
				auto *sh = (OverlaySlotHdr *)(b + kOffOverlaySlotHdr + front * sizeof(OverlaySlotHdr));
				const uint8_t *px = b + kOffOverlayPixels + front * kOverlaySlotBytes;
				++images1, images2 += sh->img2Offset != 0;
				if ((images1 % 300) == 1)
					printf("[fakehost] slot: image %ux%u request %u flags 0x%x | image2 offset %u %ux%u request %u flags 0x%x\n", sh->width, sh->height, sh->viewRequestId, sh->flags, sh->img2Offset,
					       sh->img2W, sh->img2H, sh->img2RequestId, sh->img2Flags);
				if (dump && t > 12000 && dumped < 1 && (!second || sh->img2Offset)) {
					++dumped;
					auto save = [&](const char *sfx, const uint8_t *p, int w, int hh) {
						char path[512];
						snprintf(path, sizeof path, "%s_%s.ppm", dump, sfx);
						FILE *fp = fopen(path, "wb");
						if (!fp) return;
						fprintf(fp, "P6\n%d %d\n255\n", w, hh);
						for (int y = hh - 1; y >= 0; --y)
							for (int x2 = 0; x2 < w; ++x2) fwrite(p + ((size_t)y * w + x2) * 4, 1, 3, fp);
						fclose(fp);
						printf("[fakehost] saved %s (%dx%d)\n", path, w, hh);
					};
					save("img1", px, sh->width, sh->height);
					if (sh->img2Offset) save("img2", px + sh->img2Offset, sh->img2W, sh->img2H);
				}
			}
		}
		for (uint64_t ringOff : {kOffRenderRing, kOffCollisionRing}) {
			uint64_t bytes = ringOff == kOffRenderRing ? kRenderRingBytes : kCollisionRingBytes, dataBytes = bytes - 0x80;
			uint8_t *ring = b + ringOff; uint64_t head = __atomic_load_n((uint64_t *)ring, __ATOMIC_ACQUIRE), tail = *(uint64_t *)(ring + 0x40);
			while (tail < head) {
				uint64_t pos = tail % dataBytes; uint32_t type = *(uint32_t *)(ring + 0x80 + pos), pl = *(uint32_t *)(ring + 0x80 + pos + 4);
				if (type == 0) { tail += dataBytes - pos; continue; }
				if (ringOff == kOffRenderRing) { ++msgs; if (type == kRenSection) { ++sections; verts += ((RenSection *)(ring + 0x88 + pos))->vertexCount; } }
				tail += (8 + pl + 7) & ~7ull;
			}
			__atomic_store_n((uint64_t *)(ring + 0x40), tail, __ATOMIC_RELEASE);
		}
		if (Ms() - lastPrint > 5000) {
			lastPrint = Ms();
			auto *mc = (McState *)(b + kOffMcState);
			printf("[fakehost] mc pid %u flags 0x%x feet (%.2f %.2f %.2f) | render msgs %llu sections %llu vertices %llu | overlay frames %llu, read %llu (with a second image %llu)\n", h->mcPid, mc->flags, mc->x,
			       mc->y, mc->z, (unsigned long long)msgs, (unsigned long long)sections, (unsigned long long)verts, (unsigned long long)framesSeen, (unsigned long long)images1, (unsigned long long)images2);
			fflush(stdout);
		}
		usleep(16000);
	}
	shm_unlink(kShmName);
	return 0;
}
