// Portalcraft bridge (bridge #18, Owner A item 2): Minecraft's light-emitting blocks (torches, lanterns, glowstone, lava,
// fire, ...) placed in Portal 2's chambers light Portal 2's own world, SkyCraft's BlockLights.cpp ported to Portal 2's
// dynamic lights.
//
// The mod sends each section's emitters (kRenLights after its kRenSection: block, level 1-15, colour, kind, hazard).
// Every 0.25 s the emitters of the chamber band within 72 blocks of the camera are merged per 3x3x3-block cell (a lava pool
// is one light, not hundreds), weighted by level^2; the 24 cells nearest to the camera (minus their reach) keep a light,
// which stays on the same cell while it lasts. Each becomes a Portal 2 dynamic light (VEngineEffects001 CL_AllocDlight,
// slot 4: the engine lights the world's lightmaps and every model with it, so walls, props, Chell and the gun), placed a
// little above the cell's weighted centre, its radius the level's reach in blocks (a cluster a bit further), brighter
// emitters brighter. Flames flicker and lava glows slowly (SkyCraft's Animate). The placed blocks themselves already carry
// Minecraft's block light, so their own lighting leaves these lights out (BlockLightIsOurs, bandlight.cpp).
#include "bridge.hpp"
#include "slots.hpp"
#include "growth.hpp"
#include "coords.hpp"
#include "link.hpp"

#include "Modules/Tier1.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace bridge {

using namespace skycraft::proto;
bool ClipHasRegion();                      // clip.cpp
bool ClipRegionContains(const Vector &p);  // clip.cpp
void TestCheck(bool ok, const char *what, const char *detail);  // testassert.cpp

// public SDK dlight_t (dlight.h): flags, origin, radius, ColorRGBExp32, die, decay, minlight, key, style, direction, inner, outer
struct DLight {
	int flags;
	Vector origin;
	float radius;
	uint8_t r, g, b;
	int8_t exponent;
	float die, decay, minlight;
	int key, style;
	Vector dir;
	float inner, outer;
};
using _AllocDlight = DLight *(__rescall *)(void *thisptr, int key);
using _GetActiveDLights = int(__rescall *)(void *thisptr, DLight **list);
static Interface *g_efx = nullptr;
static _AllocDlight AllocDlight = nullptr;
static _AllocDlight AllocElight = nullptr;  // (bridge #27) slot 5: an entity light, models only
static _GetActiveDLights GetActiveDLights = nullptr;
static constexpr int kKeyBase = 0x5C180000;  // our dlight keys (no entity index is this large)

struct Emitter {
	int x, y, z;
	uint8_t level, kind;
	uint32_t rgb;
};
static std::unordered_map<long long, std::vector<Emitter>> g_bySection;
PCB_GAUGE("blocklights.sections", g_bySection.size());

static constexpr int kCellBlocks = 3, kMaxLights = 24;
static constexpr float kRangeBlocks = 72.0f, kUpdateS = 0.25f;

struct Cluster {
	long long cell = 0;
	double sx = 0, sy = 0, sz = 0, w = 0;
	float r = 0, g = 0, b = 0;
	int level = 0, count = 0;
	uint8_t kind = kLightSteady;
	float radius = 0, score = 0;  // Source units
	double mx = 0, my = 0, mz = 0;  // Minecraft position of the light
};
struct Slot {
	bool active = false;
	long long cell = 0;
	uint8_t kind = 0;
	float r = 0, g = 0, b = 0, radius = 0, base = 1, phase = 0;
	int level = 0;
	double mx = 0, my = 0, mz = 0;
	Vector at;  // Source position this frame
	// (bridge #58) steady light: what is shown eases to the target over ~0.2 s; a dropped light fades out before its slot is freed
	bool want = false;
	float fade = 0, curBright = -1, curRadius = 0, cr = 0, cg = 0, cb = 0;
};
static std::array<Slot, kMaxLights> g_slots;
static bool g_enabled = true;
static float g_bright = 0.4f;  // pc_mc_blocklights <0|1> [brightness]
static float g_radiusK = 1.0f;
// (bridge #22, item 7) dlight_t flags: 0 = world and models; 2 (DLIGHT_NO_MODEL_ILLUMINATION) = the lightmaps only.
// (bridge #27) "A cube beside a torch turns white": with flags 0 the engine puts our light into a model's lighting state as a local
// light, and a model gets only so many (2 in these states): the map's own light it pushed out is folded into the ambient cube, which
// lights every face of the model (b22_d, r27_b: +Y ambient 0.009 -> 0.031, +Z 0.018 -> 0.043 for a cube beside a torch), plus the
// torch's own inverse-square light. An elight (models only) does the same. So the dlight lights the world's lightmaps only (flags 2),
// and models get our lights from ComputeLightingState_Hook (mclight.cpp) after the engine has picked the map's lights: added as
// extra local lights (or into the ambient cube when the state is full), with Minecraft's block-light rule at the model (BlockLightsAt),
// which is what the placed blocks next to it get. pc_mc_blocklights_flags 0 = the old path.
static int g_dlFlags = 2;
static bool g_useElight = false;  // (pc_mc_blocklights_elight: the r27_a experiment, kept for comparison)
static bool g_suspended = false;
static bool g_modelLights = true;  // pc_mc_blocklights_models
static double g_nextRebuild = 0, g_clock = 0, g_lastT = 0;
static int g_frames = 0, g_activeLast = 0;

static double NowS() {
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}
static long long SecKey(int sx, int sy, int sz) { return ((long long)(sx & 0x3FFFFF) << 42) | ((long long)(sz & 0x3FFFFF) << 20) | (long long)(sy & 0xFFFFF); }
bool ClipInTerrainBox(const Vector &p);  // clip.cpp (bridge #61)
static bool InChambers(const Vector &src) { return !ClipInTerrainBox(src); }

static bool Ready() {
	if (AllocDlight) return true;
	if (!g_efx) g_efx = Interface::Create(MODULE_NAME("engine"), "VEngineEffects001", false);
	if (!g_efx) return false;
	AllocDlight = slots::Orig<_AllocDlight>(g_efx, slots::kEfxAllocDlight);
	AllocElight = slots::Orig<_AllocDlight>(g_efx, slots::kEfxAllocElight);
	GetActiveDLights = slots::Orig<_GetActiveDLights>(g_efx, slots::kEfxGetActiveDLights);
	return AllocDlight != nullptr;
}

void BlockLightsOnLights(const uint8_t *p, uint32_t bytes) {
	if (bytes < sizeof(RenLights)) return;
	auto *h = (const RenLights *)p;
	const long long key = SecKey(h->sx, h->sy, h->sz);
	const uint64_t count = std::min<uint64_t>(h->count, (bytes - sizeof(RenLights)) / sizeof(RenLight));
	if (count == 0) {
		g_bySection.erase(key);
		g_nextRebuild = 0;
		return;
	}
	auto &list = g_bySection[key];
	list.clear();
	auto *src = (const RenLight *)(p + sizeof(RenLights));
	for (uint64_t i = 0; i < count; ++i) {
		const RenLight &l = src[i];
		list.push_back({h->sx * 16 + l.x, h->sy * 16 + l.y, h->sz * 16 + l.z, l.level, (uint8_t)((l.color >> 24) & 0x0F), l.color & 0xFFFFFF});
	}
	g_nextRebuild = 0;  // a new torch shows at once
}

void BlockLightsClear() {
	g_bySection.clear();
	g_nextRebuild = 0;
}

static void Off(Slot &s, int i) {
	if (s.active && Ready()) {
		if (DLight *d = AllocDlight(g_efx->ThisPtr(), kKeyBase + i)) d->radius = 0, d->die = 0;
		if (AllocElight)
			if (DLight *d = AllocElight(g_efx->ThisPtr(), kKeyBase + i)) d->radius = 0, d->die = 0;
	}
	s.active = false;
	s.want = false;
	s.fade = 0;
	s.curBright = -1;
	s.cell = 0;
}

static void Rebuild(double px, double py, double pz) {
	std::unordered_map<long long, Cluster> cells;
	const double range2 = (double)kRangeBlocks * kRangeBlocks;
	auto fdiv = [](int v) { return v >= 0 ? v / kCellBlocks : (v - kCellBlocks + 1) / kCellBlocks; };
	for (auto &kv : g_bySection)
		for (const Emitter &e : kv.second) {
			const double dx = e.x + 0.5 - px, dy = e.y + 0.5 - py, dz = e.z + 0.5 - pz;
			if (dx * dx + dy * dy + dz * dz > range2) continue;
			if (!InChambers(McToSrc(e.x + 0.5, e.y + 0.5, e.z + 0.5))) continue;  // the region: Minecraft's own frame lights it
			const long long cell = SecKey(fdiv(e.x), fdiv(e.y), fdiv(e.z));
			Cluster &c = cells[cell];
			c.cell = cell;
			const double w = (double)e.level * e.level;
			c.sx += (e.x + 0.5) * w, c.sy += (e.y + 0.5) * w, c.sz += (e.z + 0.5) * w, c.w += w;
			c.r += (e.rgb & 0xFF) / 255.0f * (float)w, c.g += ((e.rgb >> 8) & 0xFF) / 255.0f * (float)w, c.b += ((e.rgb >> 16) & 0xFF) / 255.0f * (float)w;
			c.level = std::max<int>(c.level, e.level);
			c.kind = std::max<uint8_t>(c.kind, e.kind);
			++c.count;
		}
	std::vector<Cluster> chosen;
	const Vector eye = McToSrc(px, py, pz);
	for (auto &kv : cells) {
		Cluster c = kv.second;
		const float w = (float)c.w;
		c.r /= w, c.g /= w, c.b /= w;
		const float blocks = (float)(c.level + 1) * (1.0f + 0.12f * std::log2((float)c.count));
		c.radius = std::min(blocks, 20.0f) * (float)kUnitsPerBlock * 0.8f * g_radiusK;
		c.mx = c.sx / c.w, c.my = c.sy / c.w + 0.3, c.mz = c.sz / c.w;  // a little above the blocks' centre (lava lights its top)
		c.score = (McToSrc(c.mx, c.my, c.mz) - eye).Length() - c.radius;
		// (bridge #58) hysteresis: a light already shown keeps its place until another is clearly closer (4 blocks), so the set of
		// 24 does not alternate as the camera moves past the edge of the cut
		for (const Slot &o : g_slots)
			if (o.active && o.want && o.cell == c.cell) {
				c.score -= 4.0f * (float)kUnitsPerBlock;
				break;
			}
		chosen.push_back(c);
	}
	const size_t keep = std::min<size_t>(chosen.size(), kMaxLights);
	std::partial_sort(chosen.begin(), chosen.begin() + keep, chosen.end(), [](const Cluster &a, const Cluster &b) { return a.score < b.score; });
	chosen.resize(keep);
	std::array<bool, kMaxLights> used{};
	auto set = [&](Slot &s, const Cluster &c) {
		s.active = true, s.want = true, s.cell = c.cell, s.kind = c.kind;
		s.r = c.r, s.g = c.g, s.b = c.b, s.radius = c.radius;
		s.base = 0.75f + 0.65f * (c.level / 15.0f);
		s.level = c.level;
		s.phase = (float)(c.cell % 997) * 0.37f;
		s.mx = c.mx, s.my = c.my, s.mz = c.mz;
	};
	std::vector<const Cluster *> pending;
	for (auto &c : chosen) {
		bool placed = false;
		for (int i = 0; i < kMaxLights && !placed; ++i)
			if (!used[i] && g_slots[i].active && g_slots[i].cell == c.cell) set(g_slots[i], c), used[i] = placed = true;
		if (!placed) pending.push_back(&c);
	}
	for (auto *c : pending)
		for (int i = 0; i < kMaxLights; ++i)
			if (!used[i] && !g_slots[i].active) {  // (a fading-out slot is not reused until it is dark)
				set(g_slots[i], *c), used[i] = true;
				break;
			}
	for (int i = 0; i < kMaxLights; ++i)
		if (!used[i] && g_slots[i].active) g_slots[i].want = false;  // fades out (BlockLightsFrame), then Off
}

// (bridge #58) The whole dlight for a slot. CL_AllocDlight clears the light it returns (a light with this key is reset to zero, die 0
// included), so every call must go on to write all of it: a call that only set the radius left the light dead until the next frame.
static bool WriteSlot(Slot &s, int i, float k) {
	DLight *d = AllocDlight(g_efx->ThisPtr(), kKeyBase + i);  // the same light for the same key, every frame
	if (!d) return false;
	s.at = McToSrc(s.mx, s.my, s.mz);
	// colour with an exponent (ColorRGBExp32: channel x 2^exp / 255): the brightest channel at ~255
	const float lin = s.curBright * k;
	const float mx = std::max(s.cr, std::max(s.cg, s.cb)) * lin;
	int e = 0;
	while (e > -8 && mx * std::ldexp(1.0f, -e) < 0.5f) --e;
	while (e < 8 && mx * std::ldexp(1.0f, -e) > 1.0f) ++e;
	auto ch = [&](float c) { return (uint8_t)std::min(255.0f, std::max(0.0f, c * lin * std::ldexp(1.0f, -e) * 255.0f)); };
	d->flags = g_dlFlags;
	d->origin = s.at;
	d->radius = s.curRadius;
	d->r = ch(s.cr), d->g = ch(s.cg), d->b = ch(s.cb), d->exponent = (int8_t)e;
	d->die = 1e9f;  // ours to switch off (Off)
	d->decay = 0;
	d->minlight = 0;
	d->style = 0;
	d->dir = Vector(0, 0, 0);
	d->inner = d->outer = 0;
	if (AllocElight) {
		if (DLight *el = AllocElight(g_efx->ThisPtr(), kKeyBase + i)) {
			if (g_useElight && g_dlFlags == 2) {
				*el = *d;  // the same light for models, once
				el->flags = 0;
				el->key = kKeyBase + i;
			} else {
				el->radius = 0, el->die = 0;
			}
		}
	}
	return true;
}

// Client frame (main thread): eye is the main view's origin.
void BlockLightsFrame(const Vector &eye) {
	const double now = NowS();
	const float dt = g_lastT > 0 ? (float)std::min(0.1, now - g_lastT) : 0.016f;
	g_lastT = now;
	g_clock += dt;
	++g_frames;
	if (!g_enabled || !Ready()) {
		for (int i = 0; i < kMaxLights; ++i) Off(g_slots[i], i);
		return;
	}
	if (now >= g_nextRebuild) {
		g_nextRebuild = now + kUpdateS;
		double px, py, pz;
		SrcToMcD(eye, px, py, pz);
		Rebuild(px, py, pz);
	}
	int active = 0;
	for (int i = 0; i < kMaxLights; ++i) {
		Slot &s = g_slots[i];
		if (!s.active) continue;
		// (bridge #58) no flicker: Minecraft's block light is steady (only its particles flicker), and so are SkyCraft's lights.
		// Everything eases to its target over ~0.2 s; a light that goes dark is Off once its fade has run out.
		const float ease = 1.0f - std::exp(-dt / 0.2f);
		const float tb = s.base * g_bright;
		if (s.curBright < 0) s.curBright = tb, s.curRadius = s.radius, s.cr = s.r, s.cg = s.g, s.cb = s.b;
		s.curBright += (tb - s.curBright) * ease, s.curRadius += (s.radius - s.curRadius) * ease;
		s.cr += (s.r - s.cr) * ease, s.cg += (s.g - s.cg) * ease, s.cb += (s.b - s.cb) * ease;
		s.fade = s.want ? std::min(1.0f, s.fade + dt / 0.2f) : s.fade - dt / 0.2f;
		if (s.fade <= 0 && !s.want) {
			Off(s, i);
			continue;
		}
		const float k = s.fade * s.fade * (3.0f - 2.0f * s.fade);
		if (WriteSlot(s, i, k)) ++active;
	}
	g_activeLast = active;
}

void BlockLightsLevelInit() {
	for (auto &s : g_slots) s.active = false, s.want = false, s.fade = 0, s.curBright = -1, s.cell = 0;  // (the engine cleared its dlights)
	g_nextRebuild = 0;
}

void BlockLightsShutdown() {
	for (int i = 0; i < kMaxLights; ++i) Off(g_slots[i], i);
	if (g_efx) Interface::Delete(g_efx);
	g_efx = nullptr;
	AllocDlight = nullptr;
}

// While the placed blocks' lighting is queried, our lights are off: Portal 2 folds dynamic lights into a model's ambient cube
// too (a18_light3: the cube's sides 3-4x brighter beside a torch), and the blocks carry Minecraft's block light already.
// BlockLightsResume switches them on again right after.
static int g_suspendDepth = 0;
static bool NeedsEngineSuspend() { return g_dlFlags != 2 || g_useElight; }  // flags 2 lights lightmaps only: models never see the dlight
void BlockLightsSuspend() {
	++g_suspendDepth;
	g_suspended = true;
	// (bridge #58) With the default flags 2 the engine does not fold our dlights into a model's lighting, so nothing needs switching
	// off. The old code re-allocated every light here and again in Resume; each CL_AllocDlight call clears the light, and Resume put
	// back only the radius, so any frame that built a section or lit Chell's body left the lights dead (die 0, colour 0) = flicker.
	if (!NeedsEngineSuspend() || !Ready()) return;
	for (int i = 0; i < kMaxLights; ++i)
		if (g_slots[i].active)
			if (DLight *d = AllocDlight(g_efx->ThisPtr(), kKeyBase + i)) {
				d->radius = 0, d->die = 0;
				if (AllocElight)
					if (DLight *e = AllocElight(g_efx->ThisPtr(), kKeyBase + i)) e->radius = 0, e->die = 0;
			}
}
void BlockLightsResume() {
	if (g_suspendDepth > 0 && --g_suspendDepth > 0) return;
	g_suspended = false;
	if (!NeedsEngineSuspend() || !Ready()) return;
	for (int i = 0; i < kMaxLights; ++i)
		if (g_slots[i].active) {
			const float f = std::min(1.0f, std::max(0.0f, g_slots[i].fade));
			WriteSlot(g_slots[i], i, f * f * (3.0f - 2.0f * f));
		}
}

// One of our lights at this Source position (bandlight.cpp leaves them out of the placed blocks' own lighting).
bool BlockLightIsOurs(const Vector &pos) {
	for (auto &s : g_slots)
		if (s.active && (s.at - pos).Length() < 2.0f) return true;
	return false;
}

int BlockLightsActive() { return g_activeLast; }

// (bridge #27) Our lights on a model at p (Source), for mclight.cpp's ComputeLightingState_Hook. Minecraft's block light there, as the
// placed blocks are lit (terrain.cpp: Curve(level / 15) x pcb_band_light's block_k, warm): the level is the light's emission less the
// Manhattan distance in blocks from its cell to p's, as Minecraft spreads block light through open air. Each light is returned with
// its Source position and that linear colour; none while the placed blocks' own lighting is being queried (they carry Minecraft's
// block light already) or in the region (Minecraft lights it).
float BandBlockK();  // terrain.cpp
int BlockLightsAt(const Vector &p, Vector *pos, Vector *col, int max) {
	if (!g_modelLights || g_suspended || !g_enabled || !InChambers(p)) return 0;
	double px, py, pz;
	SrcToMcD(p, px, py, pz);
	const int bx = (int)std::floor(px), by = (int)std::floor(py), bz = (int)std::floor(pz);
	int n = 0;
	for (const Slot &s : g_slots) {
		if (!s.active || n >= max) continue;
		const int d = std::abs((int)std::floor(s.mx) - bx) + std::abs((int)std::floor(s.my - 0.3) - by) + std::abs((int)std::floor(s.mz) - bz);
		const int l = s.level - d;
		if (l <= 0) continue;
		const float lv = l / 15.0f, curve = lv / (4.0f - 3.0f * lv);
		const float k = curve * BandBlockK() * std::min(1.0f, std::max(0.0f, s.fade));
		pos[n] = s.at, col[n] = Vector(k, k * 0.85f, k * 0.65f);
		++n;
	}
	return n;
}
bool BlockLightsSuspended() { return g_suspended; }

CON_COMMAND(pc_mc_blocklights_models, "pc_mc_blocklights_models [0|1] - (bridge #27) Minecraft's lights on Portal 2's models (cubes, Chell, props) in chambers, added to their lighting state\n") {
	if (args.ArgC() > 1) g_modelLights = std::atoi(args[1]) != 0;
	console->Print("[bridge] block lights on models %d\n", g_modelLights);
}

CON_COMMAND(pc_mc_blocklights_elight, "pc_mc_blocklights_elight [0|1] - (bridge #27, experiment) models get the block lights through an elight (0, default: ComputeLightingState_Hook adds them)\n") {
	if (args.ArgC() > 1) g_useElight = std::atoi(args[1]) != 0;
	console->Print("[bridge] block light elights %d (dlight flags %d)\n", g_useElight, g_dlFlags);
}

CON_COMMAND(pc_mc_blocklights_flags, "pc_mc_blocklights_flags [n] - (bridge #22) dlight_t flags of the block lights: 0 world + models, 2 world only\n") {
	if (args.ArgC() > 1) g_dlFlags = std::atoi(args[1]);
	console->Print("[bridge] block light dlight flags %d\n", g_dlFlags);
}
CON_COMMAND(pc_mc_blocklights, "pc_mc_blocklights [0|1] [brightness=1] [radius_k=1] - Minecraft's torches, lava, glowstone... light Portal 2's chambers (dynamic lights)\n") {
	if (args.ArgC() > 1) g_enabled = std::atoi(args[1]) != 0;
	if (args.ArgC() > 2) g_bright = (float)std::atof(args[2]);
	if (args.ArgC() > 3) g_radiusK = (float)std::atof(args[3]);
	g_nextRebuild = 0;
	console->Print("[bridge] Minecraft block lights %d, brightness %.2f, radius x%.2f\n", g_enabled, g_bright, g_radiusK);
}

CON_COMMAND(pcb_blocklights, "pcb_blocklights - Minecraft emitters held, lights shown, the engine's active dynamic lights\n") {
	size_t n = 0;
	for (auto &kv : g_bySection) n += kv.second.size();
	int engineN = -1;
	if (Ready() && GetActiveDLights) {
		DLight *list[32] = {};
		engineN = GetActiveDLights(g_efx->ThisPtr(), list);
	}
	console->Print("[bridge] block lights: %zu emitters in %zu sections, %d lights shown, engine reports %d active dlights\n", n, g_bySection.size(), g_activeLast, engineN);
	for (int i = 0; i < kMaxLights; ++i) {
		const Slot &s = g_slots[i];
		if (s.active)
			console->Print("[bridge]   light %d: MC (%.1f %.1f %.1f) src (%.0f %.0f %.0f) radius %.0f colour (%.2f %.2f %.2f) kind %d\n", i, s.mx, s.my, s.mz, s.at.x, s.at.y, s.at.z, s.radius, s.r, s.g, s.b, s.kind);
	}
}

CON_COMMAND(pcb_assert_blocklights, "pcb_assert_blocklights <min> - (tests) at least that many Minecraft block lights are shown in the chambers\n") {
	const int want = args.ArgC() > 1 ? std::atoi(args[1]) : 1;
	char buf[96];
	std::snprintf(buf, sizeof buf, "%d shown (want >= %d)", g_activeLast, want);
	TestCheck(g_activeLast >= want, "Minecraft block lights in the chambers", buf);
}

}  // namespace bridge
