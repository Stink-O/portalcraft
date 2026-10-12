// Portalcraft bridge: Minecraft's light on Portal 2's objects in the Minecraft region (protocol v16).
//
// Inside the hidden region Portal 2 lights its models (the portal gun's viewmodel, Chell's body seen
// through portals, cubes and other props) from the region's own baked ambient cubes, so a cube glows
// white at night and the gun stays bright in a cave. This replaces that lighting with Minecraft's,
// SkyCraft's WorldRender recipe in reverse:
//   * Probes. Every frame the plugin asks Minecraft for its light at Chell's eye (the viewmodel) and at
//     every other model drawn in the region (positions taken from the draws themselves, so any prop,
//     Chell's body or a cube flung in through a portal gets one). Minecraft answers with the lightmap
//     colour at each probe's block (its sky and block light levels through the lightmap, exactly as an
//     entity or a held item there is lit), whether the block sees the sky, the sun's direction and the
//     daylight factor (LightProbes.java).
//   * Ambient cube. Per axis: Minecraft's lightmap colour times Minecraft's own entity shading for that
//     direction (its two fixed item/entity lights: 1.0 up, 0.74 north/south, 0.5 east/west, 0.4 down),
//     converted to Portal 2's linear light (display ^2.2) and scaled (pc_mc_light_scale); plus a
//     directional sun term (moon at night) for the faces that see it, attenuated by sky visibility.
//     Block light (torches) is in the lightmap colour, warm as Minecraft makes it. Portal 2's local
//     lights are dropped for these models.
//   * Smoothing. Each probe's cube moves towards its target with a 150 ms time constant (walking into a
//     cave never pops), and a model that has just entered the region fades from Portal 2's lighting to
//     Minecraft's over 150 ms. Models outside the region (the chamber) are untouched.
// Hooks (vtable slots, this game build): VStudioRender026 DrawModel (28), whose model origin selects the
// probe, re-setting the ambient cube (SetAmbientLightColors 19) and local lights (SetLocalLights 21) just
// for that draw; VEngineModel016 ComputeLightingState (26), the array path props can take.
#include "bridge.hpp"
#include "slots.hpp"
#include "growth.hpp"
#include "coords.hpp"
#include "link.hpp"

#include "Modules/Tier1.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace bridge {

using namespace skycraft::proto;
bool ClipHasRegion();
bool ClipRegionContains(const Vector &p);
void TestCheck(bool ok, const char *what, const char *detail);

// ---- engine/studiorender slots (slots.hpp) and layouts ---------------------------
//   SetAmbientLightColors(const Vector4D[6]), SetLocalLights(int count, const LightDesc_t *),
//   DrawModel(results, info, bones, flex, flexDelayed, const Vector &origin, int flags),
//   ComputeLightingState(int n, const LightingQuery_t *, MaterialLightingState_t *, ITexture **)
// studiorender's own context (read to blend from and to restore after an overridden draw): byte offsets per operating system in slots.hpp
#define kRcAmbient (slots::Index(slots::kRcFieldAmbient))
#define kRcLights (slots::Index(slots::kRcFieldLights))
#define kRcLightCount (slots::Index(slots::kRcFieldLightCount))
static constexpr int kLightDescBytes = 0x58;
// LightingQuery_t: Vector origin, uint16 instance, ... (16 bytes); MaterialLightingState_t: Vector cube[6],
// Vector lightingOrigin, int localLightCount, LightDesc_t[4] (0x1b8 bytes)
static constexpr int kQueryBytes = 0x10, kStateBytes = 0x1b8, kStateLightCount = 0x54;

struct V4 {
	float x, y, z, w;
};

static Interface *g_studio = nullptr, *g_modelRender = nullptr;
using _SetAmbient4 = void(__rescall *)(void *thisptr, const V4 *cube);
using _SetLocalLights = void(__rescall *)(void *thisptr, int n, const void *lights);
using _DrawModel = void(__rescall *)(void *thisptr, void *results, const void *info, void *bones, float *flex, float *flexDelayed, const Vector &origin, int flags);
using _ComputeLightingState = void(__rescall *)(void *thisptr, int n, const void *queries, void *states, void **envCubemaps);
static _SetAmbient4 SetAmbient4 = nullptr;
static _SetLocalLights SetLocalLights = nullptr;
static _DrawModel DrawModel = nullptr;
static _ComputeLightingState ComputeLightingState = nullptr;

// ---- settings --------------------------------------------------------------------------------------
static bool g_enabled = true;      // pc_mc_light
static float g_scale = 0.55f;      // pc_mc_light_scale (calibrated, light1/light2): Portal 2 linear light for Minecraft's full light (lightmap 1, top face)
static float g_sunK = 0.35f;       // directional sun term (relative to g_scale), faces towards the sun, open sky only
static float g_moonK = 0.04f;      // the moon's, at night
static float g_tau = 0.15f;        // smoothing time constant (s)
static float g_matchDist = 40.0f;  // a draw uses a probe within this many units of its origin

// ---- probes ----------------------------------------------------------------------------------------
struct Slot {
	bool live = false;
	Vector pos{0, 0, 0};
	int lastUse = 0;
	uint32_t bornReq = 0;     // first request that carries this probe
	bool haveSample = false;
	LightSample s{};
	float target[6][3]{}, cur[6][3]{};
	bool curInit = false;
	float w = 0;              // Minecraft's share (0: Portal 2's own lighting, 1: Minecraft's)
	float engine[6][3]{};     // Portal 2's cube seen at the last draw (diagnostics)
	bool haveEngine = false;
	int draws = 0;
};
static Slot g_slots[kMaxLightProbes];
static int g_frame = 0;
static uint32_t g_req = 0;
static McLight g_light{};
static bool g_haveLight = false;
static uint32_t g_lightReq = 0;
static double g_lastT = 0;
static Vector g_eye{0, 0, 0};
static bool g_eyeIn = false;
// counters (pcb_light_status)
static long long g_drawCalls = 0, g_drawRegion = 0, g_drawOverridden = 0, g_clsCalls = 0, g_clsQueries = 0, g_clsRegion = 0, g_clsOverridden = 0;
static float g_maxStep = 0;  // largest per-frame change of the eye cube's luminance since the reset (smoothness)

static double NowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static float Lin(float c) { return std::pow(std::fmax(0.0f, c), 2.2f); }
static float Lum(const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }
static float CubeLum(const float cube[6][3]) {
	// what a viewer mostly sees of a model lit by this cube: the top and the sides, little of the bottom
	static const float wt[6] = {0.15f, 0.15f, 0.15f, 0.15f, 0.3f, 0.1f};
	float s = 0;
	for (int a = 0; a < 6; ++a) s += wt[a] * Lum(cube[a]);
	return s;
}

// Source ambient-cube axes (+X -X +Y -Y +Z -Z) in Minecraft coordinates (x, z, -y).
static const float kAxisMc[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, -1, 0}};

// Minecraft's entity/item shading for a face normal: its two fixed directional lights.
static float McShade(const float n[3]) {
	static float l0[3], l1[3];
	static bool init = false;
	if (!init) {
		float a[3] = {0.2f, 1.0f, -0.7f}, b[3] = {-0.2f, 1.0f, 0.7f};
		float la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
		for (int i = 0; i < 3; ++i) l0[i] = a[i] / la, l1[i] = b[i] / la;
		init = true;
	}
	float d0 = std::fmax(0.0f, l0[0] * n[0] + l0[1] * n[1] + l0[2] * n[2]);
	float d1 = std::fmax(0.0f, l1[0] * n[0] + l1[1] * n[1] + l1[2] * n[2]);
	return std::fmin(1.0f, (d0 + d1) * 0.6f + 0.4f);
}

static void TargetCube(const LightSample &s, float out[6][3]) {
	const float *sun = g_light.sunDir;
	float day = std::fmax(0.0f, std::fmin(1.0f, g_light.daylight));
	float open = s.skyVisible > 0.5f ? 1.0f : 0.0f;
	// the sky's share of this probe's light (sky level through Minecraft's curve)
	float sl = s.sky / 15.0f, skyCurve = sl / (4.0f - 3.0f * sl);
	float lm[3] = {s.r, s.g, s.b};
	float slc[3] = {Lin(g_light.skyLightColor[0]), Lin(g_light.skyLightColor[1]), Lin(g_light.skyLightColor[2])};
	static const float moonCol[3] = {0.55f, 0.65f, 1.0f};
	for (int a = 0; a < 6; ++a) {
		const float *n = kAxisMc[a];
		float sh = McShade(n);
		float ds = n[0] * sun[0] + n[1] * sun[1] + n[2] * sun[2];
		float sunF = std::fmax(0.0f, ds) * day * open * skyCurve, moonF = std::fmax(0.0f, -ds) * (1 - day) * open * skyCurve;
		for (int c = 0; c < 3; ++c) out[a][c] = g_scale * (Lin(lm[c] * sh) + g_sunK * sunF * slc[c] + g_moonK * moonF * moonCol[c]);
	}
}

static int FindSlot(const Vector &o, bool alloc) {
	if (g_eyeIn && (o - g_eye).Length() < 48.0f) return 0;
	int best = -1;
	float bd = g_matchDist;
	for (int i = 1; i < (int)kMaxLightProbes; ++i) {
		if (!g_slots[i].live) continue;
		float d = (g_slots[i].pos - o).Length();
		if (d < bd) bd = d, best = i;
	}
	if (best >= 0 || !alloc) return best;
	for (int i = 1; i < (int)kMaxLightProbes; ++i) {
		Slot &s = g_slots[i];
		if (s.live) continue;
		s = Slot();
		s.live = true, s.pos = o, s.lastUse = g_frame, s.bornReq = g_req + 1;
		return i;
	}
	return -1;
}

// The slot's cube blended with Portal 2's own (engine) cube; false: leave Portal 2's lighting.
static bool Blend(Slot &s, const float engine[6][3], float out[6][3]) {
	std::memcpy(s.engine, engine, sizeof s.engine);
	s.haveEngine = true;
	++s.draws;
	if (!s.curInit || s.w <= 0) return false;
	for (int a = 0; a < 6; ++a)
		for (int c = 0; c < 3; ++c) out[a][c] = engine[a][c] + (s.cur[a][c] - engine[a][c]) * s.w;
	return true;
}

static Slot *SlotFor(const Vector &origin) {
	if (!g_enabled || !g_haveLight || !ClipHasRegion() || !ClipRegionContains(origin)) return nullptr;
	int i = FindSlot(origin, true);
	if (i < 0) return nullptr;
	Slot &s = g_slots[i];
	if (i > 0) s.pos = origin;
	s.lastUse = g_frame;
	return &s;
}

// ---- hooks -------------------------------------------------------------------------------------------
// (bridge #27) what studiorender actually holds for one model draw near a point: its ambient cube and local lights, as the
// draw uses them (pcb_model_light_dump x y z [radius]). The lighting-state query (pcb_p2light_mc) is a different path.
static std::atomic<int> g_dumpWant{0};
static Vector g_dumpAt{0, 0, 0};
static float g_dumpR = 48.0f;
static void DumpDraw(const uint8_t *rc, const Vector &origin) {
	const V4 *amb = (const V4 *)(rc + kRcAmbient);
	const int n = *(const int *)(rc + kRcLightCount);
	console->Print("[bridge] model draw at (%.0f %.0f %.0f): ambient +X (%.3f %.3f %.3f) -X (%.3f %.3f %.3f) +Y (%.3f %.3f %.3f) -Y (%.3f %.3f %.3f) +Z (%.3f %.3f %.3f) -Z (%.3f %.3f %.3f), %d local lights\n",
	               origin.x, origin.y, origin.z, amb[0].x, amb[0].y, amb[0].z, amb[1].x, amb[1].y, amb[1].z, amb[2].x, amb[2].y, amb[2].z, amb[3].x, amb[3].y, amb[3].z, amb[4].x, amb[4].y,
	               amb[4].z, amb[5].x, amb[5].y, amb[5].z, n);
	for (int i = 0; i < n && i < 4; ++i) {
		const uint8_t *d = rc + kRcLights + i * kLightDescBytes;
		const float *f = (const float *)d;
		console->Print("[bridge]   draw light %d type %d colour (%.3f %.3f %.3f) at (%.0f %.0f %.0f) atten %.4f %.6f %.8f range %.0f\n", i, *(const int *)d, f[1], f[2], f[3], f[4], f[5], f[6], f[12],
		               f[13], f[14], f[10]);
	}
}
CON_COMMAND(pcb_model_light_dump, "pcb_model_light_dump <x> <y> <z> [radius=48] [mc] - (diagnostics) print the ambient cube and local lights of the next model draws (up to 3) near a Source point (mc: a Minecraft point)\n") {
	if (args.ArgC() < 4) return;
	const bool mc = args.ArgC() > 5 && !std::strcmp(args[5], "mc");
	g_dumpAt = mc ? McToSrc(std::atof(args[1]), std::atof(args[2]), std::atof(args[3])) : Vector((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3]));
	g_dumpR = args.ArgC() > 4 ? (float)std::atof(args[4]) : 48.0f;
	g_dumpWant = 3;
}

static void __rescall DrawModel_Hook(void *thisptr, void *results, const void *info, void *bones, float *flex, float *flexDelayed, const Vector &origin, int flags) {
	++g_drawCalls;
	if (g_dumpWant.load() > 0 && (origin - g_dumpAt).Length() < g_dumpR) {
		--g_dumpWant;
		DumpDraw((const uint8_t *)thisptr, origin);
	}
	Slot *s = SlotFor(origin);
	if (!s) return DrawModel(thisptr, results, info, bones, flex, flexDelayed, origin, flags);
	++g_drawRegion;
	uint8_t *rc = (uint8_t *)thisptr;
	V4 saved[6];
	uint8_t savedLights[4 * kLightDescBytes];
	int savedCount = *(int *)(rc + kRcLightCount);
	std::memcpy(saved, rc + kRcAmbient, sizeof saved);
	std::memcpy(savedLights, rc + kRcLights, sizeof savedLights);
	float eng[6][3], out[6][3];
	for (int a = 0; a < 6; ++a) eng[a][0] = saved[a].x, eng[a][1] = saved[a].y, eng[a][2] = saved[a].z;
	if (!Blend(*s, eng, out)) return DrawModel(thisptr, results, info, bones, flex, flexDelayed, origin, flags);
	++g_drawOverridden;
	V4 cube[6];
	for (int a = 0; a < 6; ++a) cube[a] = {out[a][0], out[a][1], out[a][2], 1.0f};
	SetAmbient4(thisptr, cube);
	if (s->w > 0.5f) SetLocalLights(thisptr, 0, nullptr);
	DrawModel(thisptr, results, info, bones, flex, flexDelayed, origin, flags);
	SetAmbient4(thisptr, saved);
	if (s->w > 0.5f) SetLocalLights(thisptr, savedCount < 0 ? 0 : savedCount > 4 ? 4 : savedCount, savedLights);
}

// (bridge #27) Minecraft's lights (torches, lava, glowstone...) on a model in the chambers: added to the state the engine made with
// the map's own lights, so they never push one of those out (blocklights.cpp: the dlights light the lightmaps only). A light is
// appended as a point light of constant strength (Minecraft's block light at the model, BlockLightsAt; the shader's N.L gives the
// side facing it), or, when the state's 4 local lights are taken, added to the ambient cube by direction, as Source folds lights.
int BlockLightsAt(const Vector &p, Vector *pos, Vector *col, int max);  // blocklights.cpp
static long long g_clsMcLights = 0, g_clsMcFolded = 0;
static void AddBlockLights(uint8_t *st, const Vector &o) {
	Vector pos[8], col[8];
	const int m = BlockLightsAt(o, pos, col, 8);
	for (int k = 0; k < m; ++k) {
		int &count = *(int *)(st + kStateLightCount);
		Vector L = pos[k] - o;
		const float len = L.Length();
		L = len > 1e-3f ? L * (1.0f / len) : Vector(0, 0, 1);
		if (count >= 0 && count < 4) {
			// LightDesc_t (public SDK lightdesc.h): type, colour, position, direction, range, falloff, attenuation 0/1/2, theta, phi,
			// cos theta, cos phi, flags (attenuation0 1 | derived values 8), 1/(cos theta - cos phi), range squared
			uint8_t *d = st + 0x58 + count * kLightDescBytes;
			std::memset(d, 0, kLightDescBytes);
			float *f = (float *)d;
			*(int *)d = 1;  // point
			f[1] = col[k].x, f[2] = col[k].y, f[3] = col[k].z;
			f[4] = pos[k].x, f[5] = pos[k].y, f[6] = pos[k].z;
			f[10] = 0;  // no range cut (the strength is already the value at the model)
			f[12] = 1.0f;
			*(int *)(d + 19 * 4) = 1 | 8;
			++count;
			++g_clsMcLights;
		} else {
			float *cube = (float *)st;
			static const Vector ax[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
			for (int a = 0; a < 6; ++a) {
				const float w = std::fmax(0.0f, ax[a].x * L.x + ax[a].y * L.y + ax[a].z * L.z);
				cube[a * 3] += col[k].x * w, cube[a * 3 + 1] += col[k].y * w, cube[a * 3 + 2] += col[k].z * w;
			}
			++g_clsMcFolded;
		}
	}
}

static void __rescall ComputeLightingState_Hook(void *thisptr, int n, const void *queries, void *states, void **envCubemaps) {
	ComputeLightingState(thisptr, n, queries, states, envCubemaps);
	++g_clsCalls;
	g_clsQueries += n;
	if (!queries || !states) return;
	for (int i = 0; i < n; ++i) {
		const Vector &o = *(const Vector *)((const uint8_t *)queries + i * kQueryBytes);
		Slot *s = SlotFor(o);
		if (!s) {
			AddBlockLights((uint8_t *)states + i * kStateBytes, o);
			continue;
		}
		++g_clsRegion;
		float *cube = (float *)((uint8_t *)states + i * kStateBytes);
		float eng[6][3], out[6][3];
		std::memcpy(eng, cube, sizeof eng);
		if (!Blend(*s, eng, out)) continue;
		++g_clsOverridden;
		std::memcpy(cube, out, sizeof out);
		if (s->w > 0.5f) *(int *)((uint8_t *)states + i * kStateBytes + kStateLightCount) = 0;
	}
}

// ---- per frame (main thread, OverrideView) ---------------------------------------------------------------
void McLightFrame(const Vector &eye) {
	++g_frame;
	double now = NowS();
	float dt = g_lastT > 0 ? (float)std::fmin(0.1, now - g_lastT) : 0.016f;
	g_lastT = now;
	if (!link::Base() || link::AnnouncedVersion() < 16) return;
	McLight l;
	if (link::ReadMcLight(l) && l.requestId != 0) {
		g_light = l, g_haveLight = true, g_lightReq = l.requestId;
		for (int i = 0; i < (int)kMaxLightProbes && i < (int)l.count; ++i) {
			Slot &s = g_slots[i];
			if (s.live && l.requestId >= s.bornReq) s.s = l.s[i], s.haveSample = true;
		}
	}
	g_eye = eye;
	bool in = g_enabled && ClipHasRegion() && ClipRegionContains(eye);
	if (in && !g_eyeIn) {
		g_slots[0] = Slot();
		g_slots[0].live = true, g_slots[0].bornReq = g_req + 1;
	}
	g_eyeIn = in;
	if (!in) g_slots[0].live = false;
	if (in) g_slots[0].pos = eye, g_slots[0].lastUse = g_frame;
	float k = 1.0f - std::exp(-dt / g_tau);
	for (int i = 0; i < (int)kMaxLightProbes; ++i) {
		Slot &s = g_slots[i];
		if (s.live && i > 0 && g_frame - s.lastUse > 30) s.live = false;
		if (!s.live || !s.haveSample || !g_haveLight) continue;
		TargetCube(s.s, s.target);
		float before = s.curInit ? CubeLum(s.cur) : 0;
		if (!s.curInit) {
			std::memcpy(s.cur, s.target, sizeof s.cur);
			s.curInit = true;
		} else {
			for (int a = 0; a < 6; ++a)
				for (int c = 0; c < 3; ++c) s.cur[a][c] += (s.target[a][c] - s.cur[a][c]) * k;
		}
		if (i == 0 && s.w >= 1) g_maxStep = std::fmax(g_maxStep, std::fabs(CubeLum(s.cur) - before) / std::fmax(1e-4f, before));
		s.w = std::fmin(1.0f, s.w + dt / g_tau);
	}
	LightProbes p{};
	int n = 0;
	for (int i = 0; i < (int)kMaxLightProbes; ++i) {
		if (!g_slots[i].live) continue;
		double x, y, z;
		SrcToMcD(g_slots[i].pos, x, y, z);
		p.pos[i][0] = x, p.pos[i][1] = y, p.pos[i][2] = z;
		n = i + 1;
	}
	p.count = n;
	p.requestId = ++g_req;
	link::WriteLightProbes(p);
}

// Floating origin: probes keep their Minecraft position (their samples stay valid), their Source one moves.
void McLightShift(const Vector &d) {
	for (Slot &s : g_slots)
		if (s.live) s.pos += d;
	g_eye += d;
}

bool McLightInit() {
	g_studio = Interface::Create(MODULE_NAME("studiorender"), "VStudioRender026");
	g_modelRender = Interface::Create(MODULE_NAME("engine"), "VEngineModel016");
	if (!g_studio || !g_modelRender) return false;
	// the hooks read studiorender's context at fixed byte offsets: held back on Windows until those are enabled (rc_fields)
	if (!slots::Use(slots::kRcFieldAmbient) || !slots::Use(slots::kRcFieldLights) || !slots::Use(slots::kRcFieldLightCount)) return false;
	SetAmbient4 = slots::Orig<_SetAmbient4>(g_studio, slots::kStudioSetAmbient4);
	SetLocalLights = slots::Orig<_SetLocalLights>(g_studio, slots::kStudioSetLocalLights);
	slots::Hook(g_studio, slots::kStudioDrawModel, DrawModel_Hook, DrawModel);
	slots::Hook(g_modelRender, slots::kModelComputeLightingState, ComputeLightingState_Hook, ComputeLightingState);
	return true;
}

void McLightShutdown() {
	Interface::Delete(g_studio);
	Interface::Delete(g_modelRender);
	g_studio = g_modelRender = nullptr;
}

// ---- commands -------------------------------------------------------------------------------------------
CON_COMMAND(pc_mc_light, "pc_mc_light [0|1] - Minecraft's light on Portal 2 objects in the region (default 1)\n") {
	if (args.ArgC() > 1) g_enabled = std::atoi(args[1]) != 0;
	console->Print("[bridge] Minecraft lighting on region objects %s\n", g_enabled ? "on" : "off");
}
CON_COMMAND(pc_mc_light_scale, "pc_mc_light_scale [scale] [sun] [moon] [tau_s] - Portal 2 light for Minecraft's full light, the sun's and moon's directional share, smoothing\n") {
	if (args.ArgC() > 1) g_scale = (float)std::atof(args[1]);
	if (args.ArgC() > 2) g_sunK = (float)std::atof(args[2]);
	if (args.ArgC() > 3) g_moonK = (float)std::atof(args[3]);
	if (args.ArgC() > 4) g_tau = std::fmax(0.01f, (float)std::atof(args[4]));
	console->Print("[bridge] Minecraft light: scale %.3f, sun %.3f, moon %.3f, tau %.3f s\n", g_scale, g_sunK, g_moonK, g_tau);
}

static void PrintSlot(int i) {
	const Slot &s = g_slots[i];
	double x, y, z;
	SrcToMcD(s.pos, x, y, z);
	console->Print("[bridge] light slot %d %s: MC (%.1f %.1f %.1f) sky %.0f block %.0f open %.0f lightmap (%.3f %.3f %.3f) w %.2f draws %d | cube top (%.3f %.3f %.3f) side (%.3f %.3f %.3f) "
	               "bottom (%.3f %.3f %.3f) lum %.4f | Portal 2's top (%.3f %.3f %.3f) lum %.4f\n",
	               i, i == 0 ? "(eye)" : "", x, y, z, s.s.sky, s.s.block, s.s.skyVisible, s.s.r, s.s.g, s.s.b, s.w, s.draws, s.cur[4][0], s.cur[4][1], s.cur[4][2], s.cur[0][0], s.cur[0][1],
	               s.cur[0][2], s.cur[5][0], s.cur[5][1], s.cur[5][2], CubeLum(s.cur), s.engine[4][0], s.engine[4][1], s.engine[4][2], s.haveEngine ? CubeLum(s.engine) : -1.0f);
}

CON_COMMAND(pcb_light_status, "pcb_light_status - Minecraft lighting: probes, cubes, hook counters\n") {
	console->Print("[bridge] light: %s, protocol %u, answer %u/%u (%s), sun (%.2f %.2f %.2f) daylight %.3f time %.0f, eye in region %d (%.0f %.0f %.0f)\n", g_enabled ? "on" : "off",
	               link::Base() ? link::AnnouncedVersion() : 0, g_lightReq, g_req, g_haveLight ? "have" : "none", g_light.sunDir[0], g_light.sunDir[1], g_light.sunDir[2], g_light.daylight,
	               g_light.dayTime, g_eyeIn, g_eye.x, g_eye.y, g_eye.z);
	console->Print("[bridge] light hooks: DrawModel %lld (region %lld, overridden %lld), ComputeLightingState %lld calls %lld queries (region %lld, overridden %lld)\n", g_drawCalls, g_drawRegion,
	               g_drawOverridden, g_clsCalls, g_clsQueries, g_clsRegion, g_clsOverridden);
	console->Print("[bridge] Minecraft block lights on chamber models: %lld added as local lights, %lld folded into the ambient cube\n", g_clsMcLights, g_clsMcFolded);
	for (int i = 0; i < (int)kMaxLightProbes; ++i)
		if (g_slots[i].live) PrintSlot(i);
}

// Test marks: the eye cube's luminance (what the gun is lit with) under a label, for ordering asserts.
static std::map<std::string, float> g_marks;
PCB_GAUGE("mclight.marks", g_marks.size());
static float SlotLum(int i) { return g_slots[i].live && g_slots[i].curInit ? CubeLum(g_slots[i].cur) * g_slots[i].w : -1.0f; }

CON_COMMAND(pcb_light_mark, "pcb_light_mark <label> [slot=0|cube] - remember the probe's current Minecraft light (eye: what lights the gun; cube: the probe nearest the newest cube)\n") {
	if (args.ArgC() < 2) return;
	int slot = 0;
	if (args.ArgC() > 2 && !std::strcmp(args[2], "cube")) {
		slot = -1;
		auto cubes = EntFind("prop_weighted_cube");
		if (!cubes.empty()) slot = FindSlot(EntVec(cubes.back(), "origin"), false);
		if (slot < 0) {
			for (int i = 1; i < (int)kMaxLightProbes; ++i)
				if (g_slots[i].live && g_slots[i].draws > 0) slot = i;
		}
	} else if (args.ArgC() > 2) {
		slot = std::atoi(args[2]);
	}
	float v = slot >= 0 ? SlotLum(slot) : -1;
	g_marks[args[1]] = v;
	// Portal 2's own lighting of that model (what it would be without Minecraft's)
	g_marks[std::string(args[1]) + "_p2"] = slot >= 0 && g_slots[slot].haveEngine ? CubeLum(g_slots[slot].engine) : -1.0f;
	console->Print("[bridge] light mark %s: slot %d luminance %.4f\n", args[1], slot, v);
	if (slot >= 0) PrintSlot(slot);
}

CON_COMMAND(pcb_light_goto_mc, "pcb_light_goto_mc <x> <y> <z> [mcYaw] [pitch] - setpos Chell's feet at a Minecraft position (tests)\n") {
	if (args.ArgC() < 4) return;
	Vector p = McToSrc(std::atof(args[1]), std::atof(args[2]), std::atof(args[3]));
	char c[160];
	std::snprintf(c, sizeof c, "setpos %.2f %.2f %.2f", p.x, p.y, p.z);
	engine.Cmd(c);
	if (args.ArgC() > 4) {
		std::snprintf(c, sizeof c, "setang %.2f %.2f 0", args.ArgC() > 5 ? std::atof(args[5]) : 0.0, (double)McYawToSrc((float)std::atof(args[4])));
		engine.Cmd(c);
	}
}
CON_COMMAND(pcb_light_cube_mc, "pcb_light_cube_mc <x> <y> <z> - spawn a weighted cube at a Minecraft position (tests)\n") {
	if (args.ArgC() < 4) return;
	Vector p = McToSrc(std::atof(args[1]), std::atof(args[2]), std::atof(args[3]));
	char c[160];
	std::snprintf(c, sizeof c, "pcb_spawn_cube %.2f %.2f %.2f", p.x, p.y, p.z);
	engine.Cmd(c);
}

CON_COMMAND(pcb_assert_light_order, "pcb_assert_light_order <brighter> <darker> [min_ratio=1.15] - the first mark is brighter than the second by the ratio\n") {
	if (args.ArgC() < 3) return;
	float a = g_marks.count(args[1]) ? g_marks[args[1]] : -1, b = g_marks.count(args[2]) ? g_marks[args[2]] : -1;
	float r = args.ArgC() > 3 ? (float)std::atof(args[3]) : 1.15f;
	char buf[200];
	std::snprintf(buf, sizeof buf, "%s %.4f vs %s %.4f (ratio %.2f, want >= %.2f)", args[1], a, args[2], b, b > 0 ? a / b : 0.0f, r);
	TestCheck(a > 0 && b >= 0 && a >= b * r, "Minecraft light ordering", buf);
}

CON_COMMAND(pcb_assert_light_max, "pcb_assert_light_max <mark> <max> - the mark's luminance is at most max (e.g. a cube at night is not lit like day)\n") {
	if (args.ArgC() < 3) return;
	float a = g_marks.count(args[1]) ? g_marks[args[1]] : -1, m = (float)std::atof(args[2]);
	char buf[160];
	std::snprintf(buf, sizeof buf, "%s %.4f (max %.4f)", args[1], a, m);
	TestCheck(a >= 0 && a <= m, "Minecraft light level", buf);
}

CON_COMMAND(pcb_assert_light_smooth, "pcb_assert_light_smooth <max_rel_step> | reset - the eye cube never changed by more than that fraction in one frame\n") {
	if (args.ArgC() > 1 && !std::strcmp(args[1], "reset")) return void(g_maxStep = 0);
	float m = args.ArgC() > 1 ? (float)std::atof(args[1]) : 0.15f;
	char buf[160];
	std::snprintf(buf, sizeof buf, "largest per-frame change %.3f (max %.3f)", g_maxStep, m);
	TestCheck(g_maxStep <= m, "Minecraft light smoothing", buf);
}

}  // namespace bridge
