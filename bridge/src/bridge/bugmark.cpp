// Portalcraft bridge (bridge #24): "mark bug" for the user, and the per-frame dispatcher of the test tools.
//
// F8 (pc_bugmark_key; polled through SDL like J/G/H in mc.cpp, no bind is written) writes a bundle to
// cache/bridge/bugmarks/<timestamp>/:
//   pose.txt     map, mode, Portal 2 and Minecraft poses, both crosshair targets, velocity, switch/carry state
//   screen.png   the presented frame (glhud readback, the Minecraft overlay included)
//   colfield.txt what Minecraft's collision copy holds for the 5x5x4 blocks around the feet, next to a fresh
//                voxelisation of the same blocks (pcb_colfield_block), with the differences counted
//   dyn.txt      the tracked moving solids within 6 blocks, probed (pcb_dyn_probe)
//   ring.txt     the last ~10 s: per frame the pose, frame time and carry flags; per input what Minecraft received
//                (the shared-memory input ring: keys, buttons, wheel, cursor) and the raw SDL events Portal 2 saw
//   replay.txt   a pcb_script cfg that recreates the moment (see bridge/TESTING.md)
// One console line confirms it, and a short "BUG MARKED" toast is drawn at the swap after the screenshot and video
// readbacks (glhud.cpp), so neither a bug-mark screenshot nor a pcb_record video has it.
//
// Also here: the window-focus count behind pcb_run_summary's timing note (RunFocusNote), pcb_assert_pose,
// pcb_setview, and TestToolsFrame / TestToolsShutdown, which drive mapindex.cpp and coloverlay.cpp.
#include "bridge.hpp"
#include "growth.hpp"
#include "coords.hpp"
#include "link.hpp"

#include "Modules/Tier1.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include "platform/platform.hpp"
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <tuple>
#include <vector>

extern bool g_bridgeQuiet;  // Modules.cpp: quiet mode (output to the log only)

namespace bridge {

using namespace skycraft::proto;

bool McModeActive();                           // mc.cpp
bool McSwitchPending();                        // mc.cpp
bool McLeads();                                // mc.cpp
bool McOnGround();                             // mc.cpp
Vector McFeetSrc();                            // mc.cpp
const McState &McStateNow();                   // mc.cpp
const char *McMapName();                       // mc.cpp
bool McLevelIsLive();                          // mc.cpp
bool PlayerNoclip();                           // puppet.cpp
bool PlayerHolding();                          // propcollide.cpp
bool ClipHaveExit();                           // clip.cpp
bool ClipHasRegion();                          // clip.cpp
bool ClipRegionContains(const Vector &p);      // clip.cpp
const char *CollisionFieldLastMovingUnder();   // collfield.cpp
const char *CollisionFieldFeetWhy();           // collfield.cpp
size_t CollisionFieldDynPendingCount();        // collfield.cpp
void CollisionFieldSentIn(int bx0, int by0, int bz0, int bx1, int by1, int bz1, std::vector<ColBlock> &out);  // collfield.cpp (#24)
int CollisionFieldRegionState(int rx, int ry, int rz);
int CollisionFieldRegionSize();
bool CollisionFieldVoxeliseNow(int bx, int by, int bz, ColBlock &out);
void CollisionFieldDynTracked(std::vector<std::tuple<void *, Vector, Vector>> &out);
void CollisionFieldDynProbe(const Vector &p, float h, int &dyn, int &world, int &all, std::string &what);
void GlHudShotTo(const std::string &path);     // glhud.cpp (#24)
void TestCheck(bool ok, const char *what, const char *detail);  // testassert.cpp
void MapIndexFrame(bool levelLive);            // mapindex.cpp
std::string MapIndexNearest(const Vector &p, float *distOut);  // mapindex.cpp
void ColOverlayFrame();                        // coloverlay.cpp
void ColOverlayShutdown();                     // coloverlay.cpp


// ---- a 5x7 bitmap font (the toast here, the overlay's 3D labels in coloverlay.cpp) --------------------------------
// 7 rows, the low 5 bits each, bit 4 = the leftmost column. Lower case is drawn as upper case; unknown: a box.
bool FontPixel(char ch, int x, int y) {
	static const struct {
		char c;
		uint8_t r[7];
	} kGlyphs[] = {
	    {'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}}, {'C', {14, 17, 16, 16, 16, 17, 14}}, {'D', {28, 18, 17, 17, 17, 18, 28}},
	    {'E', {31, 16, 16, 30, 16, 16, 31}}, {'F', {31, 16, 16, 30, 16, 16, 16}}, {'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}},
	    {'I', {14, 4, 4, 4, 4, 4, 14}},      {'J', {7, 2, 2, 2, 2, 18, 12}},      {'K', {17, 18, 20, 24, 20, 18, 17}}, {'L', {16, 16, 16, 16, 16, 16, 31}},
	    {'M', {17, 27, 21, 21, 17, 17, 17}}, {'N', {17, 17, 25, 21, 19, 17, 17}}, {'O', {14, 17, 17, 17, 17, 17, 14}}, {'P', {30, 17, 17, 30, 16, 16, 16}},
	    {'Q', {14, 17, 17, 17, 21, 18, 13}}, {'R', {30, 17, 17, 30, 20, 18, 17}}, {'S', {15, 16, 16, 14, 1, 1, 30}},   {'T', {31, 4, 4, 4, 4, 4, 4}},
	    {'U', {17, 17, 17, 17, 17, 17, 14}}, {'V', {17, 17, 17, 17, 17, 10, 4}},  {'W', {17, 17, 17, 21, 21, 21, 10}}, {'X', {17, 17, 10, 4, 10, 17, 17}},
	    {'Y', {17, 17, 17, 10, 4, 4, 4}},    {'Z', {31, 1, 2, 4, 8, 16, 31}},     {'0', {14, 17, 19, 21, 25, 17, 14}}, {'1', {4, 12, 4, 4, 4, 4, 14}},
	    {'2', {14, 17, 1, 2, 4, 8, 31}},     {'3', {31, 2, 4, 2, 1, 17, 14}},     {'4', {2, 6, 10, 18, 31, 2, 2}},     {'5', {31, 16, 30, 1, 1, 17, 14}},
	    {'6', {6, 8, 16, 30, 17, 17, 14}},   {'7', {31, 1, 2, 4, 8, 8, 8}},       {'8', {14, 17, 17, 14, 17, 17, 14}}, {'9', {14, 17, 17, 15, 1, 2, 12}},
	    {'-', {0, 0, 0, 31, 0, 0, 0}},       {'_', {0, 0, 0, 0, 0, 0, 31}},       {'.', {0, 0, 0, 0, 0, 12, 12}},      {':', {0, 12, 12, 0, 12, 12, 0}},
	    {'/', {0, 1, 2, 4, 8, 16, 0}},       {'(', {2, 4, 8, 8, 8, 4, 2}},        {')', {8, 4, 2, 2, 2, 4, 8}},        {'+', {0, 4, 4, 31, 4, 4, 0}},
	    {'*', {0, 4, 21, 14, 21, 4, 0}},     {'#', {10, 10, 31, 10, 31, 10, 10}}, {',', {0, 0, 0, 0, 12, 4, 8}},       {'=', {0, 0, 31, 0, 31, 0, 0}},
	};
	if (x < 0 || x > 4 || y < 0 || y > 6) return false;
	if (ch == ' ') return false;
	if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
	for (const auto &g : kGlyphs)
		if (g.c == ch) return (g.r[y] >> (4 - x)) & 1;
	return x == 0 || x == 4 || y == 0 || y == 6;
}

namespace {

double NowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---- SDL (Portal 2's libSDL2) ----------------------------------------------------------------------------------
using SdlWatch = int (*)(void *userdata, void *event);
struct Sdl {
	const uint8_t *(*GetKeyboardState)(int *) = nullptr;
	void *(*GetKeyboardFocus)() = nullptr;
	void (*AddEventWatch)(SdlWatch, void *) = nullptr;
	void (*DelEventWatch)(SdlWatch, void *) = nullptr;
	void *(*GetWindowFromID)(uint32_t) = nullptr;
	uint32_t (*GetWindowFlags)(void *) = nullptr;
	bool tried = false;
} g_sdl;
void ResolveSdl() {
	if (g_sdl.tried) return;
	g_sdl.tried = true;
	g_sdl.GetKeyboardState = (const uint8_t *(*)(int *))plat::SdlSymbol("SDL_GetKeyboardState");
	g_sdl.GetKeyboardFocus = (void *(*)())plat::SdlSymbol("SDL_GetKeyboardFocus");
	g_sdl.AddEventWatch = (void (*)(SdlWatch, void *))plat::SdlSymbol("SDL_AddEventWatch");
	g_sdl.DelEventWatch = (void (*)(SdlWatch, void *))plat::SdlSymbol("SDL_DelEventWatch");
	g_sdl.GetWindowFromID = (void *(*)(uint32_t))plat::SdlSymbol("SDL_GetWindowFromID");
	g_sdl.GetWindowFlags = (uint32_t(*)(void *))plat::SdlSymbol("SDL_GetWindowFlags");
}
void *GameWindow() {
	static void *w = nullptr;
	static double at = -1;
	if (!g_sdl.GetWindowFromID) return nullptr;
	if (!w || NowS() - at > 5.0) {
		at = NowS();
		w = nullptr;
		for (uint32_t id = 1; id <= 8 && !w; ++id) w = g_sdl.GetWindowFromID(id);
	}
	return w;
}

// ---- the rolling record ------------------------------------------------------------------------------------------
enum : uint8_t { kFNoclip = 1, kFHolding = 2, kFP2Carry = 4, kFMcGround = 8, kFFocus = 16, kFPaused = 32 };
struct FrameRec {
	double t;
	float dt;
	uint8_t mode;  // 0 Chell, 1 Minecraft, 2 switching to Minecraft
	uint8_t flags;
	Vector feet, mcFeet, vel;
	QAngle view;
};
struct InRec {
	double t;
	uint8_t src;  // 0 what Minecraft received (input ring), 1 an SDL event Portal 2 saw
	uint16_t type, code;
	int32_t a, b, c;
};
const double kKeep = 12.0;
std::deque<FrameRec> g_frames;
std::deque<InRec> g_inputs;
std::mutex g_inMx;  // (the SDL watch may run on another thread than the frame)

void InputTap(uint16_t type, uint16_t code, int32_t a, int32_t b, int32_t c) {
	if (type != kInKey && type != kInMouseButton && type != kInScroll && type != kInCursor && type != kInText && type != kInReleaseAll) return;
	std::lock_guard<std::mutex> lk(g_inMx);
	g_inputs.push_back({NowS(), 0, type, code, a, b, c});
}
int SdlWatchFn(void *, void *ev) {
	const uint8_t *e = (const uint8_t *)ev;
	const uint32_t type = *(const uint32_t *)e;
	InRec r{NowS(), 1, (uint16_t)0, 0, 0, 0, 0};
	switch (type) {
	case 0x300:  // SDL_KEYDOWN / UP: state @12, repeat @13, scancode @16
	case 0x301:
		if (e[13]) return 1;  // (repeats)
		r.type = 1, r.code = (uint16_t)*(const int32_t *)(e + 16), r.a = type == 0x300;
		break;
	case 0x400:  // SDL_MOUSEMOTION: xrel @28, yrel @32
		r.type = 4, r.a = *(const int32_t *)(e + 28), r.b = *(const int32_t *)(e + 32);
		break;
	case 0x401:  // SDL_MOUSEBUTTONDOWN / UP: button @16
	case 0x402:
		r.type = 2, r.code = e[16], r.a = type == 0x401;
		break;
	case 0x403:  // SDL_MOUSEWHEEL: y @20
		r.type = 3, r.a = *(const int32_t *)(e + 20);
		break;
	default: return 1;
	}
	std::lock_guard<std::mutex> lk(g_inMx);
	g_inputs.push_back(r);
	return 1;
}
bool g_watchOn = false;

// mode switches (for "last switch")
int g_lastMode = -1, g_switches = 0;
double g_lastSwitchAt = -1;

// window focus over the whole session (RunFocusNote)
uint64_t g_focusFrames = 0, g_unfocusFrames = 0;
PCB_GAUGE("bugmark.frames", g_frames.size());
PCB_GAUGE("bugmark.inputs", [] { std::lock_guard<std::mutex> lk(g_inMx); return g_inputs.size(); }());

// the key
int g_key = 65;  // SDL scancode: 65 = F8
bool g_keyDown = false;
double g_simUntil = 0;
std::string g_lastBundle;
std::string g_pendingLabel;

// the toast (drawn by glhud.cpp on the render thread)
std::atomic<double> g_toastUntil{0};
char g_toastText[64] = "";

// ---- helpers ------------------------------------------------------------------------------------------------------
const char *ModeName(int m) { return m == 1 ? "minecraft" : m == 2 ? "switching-to-minecraft" : "chell"; }
int ModeNow() { return McModeActive() ? 1 : McSwitchPending() ? 2 : 0; }
std::string Ts() {
	std::time_t now = std::time(nullptr);
	char b[32];
	std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", std::localtime(&now));
	return b;
}
void *PlayerEntity() {
	auto v = EntFind("player");
	return v.empty() ? nullptr : v[0];
}
void TraceIgnoringPlayer(const Vector &s, const Vector &e, unsigned mask, CGameTrace &tr) {
	struct Filter : public ITraceFilter {
		void *skip;
		virtual bool ShouldHitEntity(void *ent, int) { return ent != skip; }
		virtual TraceType_t GetTraceType() const { return TRACE_EVERYTHING; }
	} filter;
	filter.skip = PlayerEntity();
	Ray_t ray;
	const Vector d = e - s;
	ray.m_Delta = VectorAligned(d.x, d.y, d.z);
	ray.m_IsSwept = true;
	ray.m_Extents = VectorAligned(0, 0, 0);
	ray.m_IsRay = true;
	ray.m_StartOffset = VectorAligned(0, 0, 0);
	ray.m_Start = VectorAligned(s.x, s.y, s.z);
	ray.m_pWorldAxisTransform = nullptr;
	std::memset(&tr, 0, sizeof tr);
	engine.TraceRay(engine.traceServer->ThisPtr(), ray, mask, &filter, &tr);
}
Vector Forward(const QAngle &a) {
	const float p = a.x * (float)M_PI / 180, y = a.y * (float)M_PI / 180;
	return Vector(std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), -std::sin(p));
}
std::string EntName(void *e) {
	if (!e) return "-";
	std::string c = EntKV(e, "classname"), n = EntKV(e, "targetname");
	if (c.empty()) return "static prop / world";
	return n.empty() ? c : c + " " + n;
}
// Minecraft's targeted block (the outline the mod exports): seqlock copy of WorldEntities' header
bool McSelection(float mn[3], float mx[3]) {
	uint8_t *base = link::Base();
	if (!base || !link::McAlive()) return false;
	auto *s = (const WorldEntities *)(base + kOffWorldEntities);
	for (int i = 0; i < 50; ++i) {
		const uint32_t a = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
		if (a & 1) continue;
		const uint32_t has = s->hasSelection;
		float m0[3], m1[3];
		std::memcpy(m0, s->selMin, sizeof m0), std::memcpy(m1, s->selMax, sizeof m1);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(&s->seq, __ATOMIC_ACQUIRE) != a) continue;
		if (!has) return false;
		std::memcpy(mn, m0, sizeof m0), std::memcpy(mx, m1, sizeof m1);
		return true;
	}
	return false;
}

// ---- the bundle ---------------------------------------------------------------------------------------------------
void WriteBlockLayers(FILE *f, const char *what, const ColBlock *b) {
	if (!b) return void(std::fprintf(f, "  %-6s empty\n", what));
	int n = 0;
	for (int y = 0; y < 8; ++y) n += __builtin_popcountll(b->bits[y]);
	std::fprintf(f, "  %-6s %3d voxels, per layer bottom->top:", what, n);
	for (int y = 0; y < 8; ++y) std::fprintf(f, " %2d", __builtin_popcountll(b->bits[y]));
	std::fprintf(f, "   bits:");
	for (int y = 0; y < 8; ++y) std::fprintf(f, " %016llx", (unsigned long long)b->bits[y]);
	std::fprintf(f, "\n");
}

std::string WriteBundle(const std::string &label) {
	const double now = NowS();
	const std::string root = CacheDir() + "/bugmarks";
	plat::MakeDir(root);
	std::string name = Ts() + (label.empty() ? "" : "_" + label), dir = root + "/" + name;
	for (int k = 2; k < 50; ++k) {
		struct stat st;
		if (stat(dir.c_str(), &st) != 0) break;
		dir = root + "/" + name + "-" + std::to_string(k);
	}
	if (!plat::MakeDir(dir)) return "";
	// the screenshot first: it is taken at the next swap
	GlHudShotTo(dir + "/screen.png");

	const int mode = ModeNow();
	const Vector feet = PlayerOrigin(), vel = PlayerVelocity();
	const QAngle va = engine.ViewAngles();
	const ViewRecord &mv = LastMainView();
	const McState &mc = McStateNow();
	const bool mcOn = mode == 1;
	const Vector mcFeet = McFeetSrc();
	const char *map = McMapName();

	// ---- pose.txt
	if (FILE *f = std::fopen((dir + "/pose.txt").c_str(), "w")) {
		std::fprintf(f, "# bug mark (bridge #24) %s\n", name.c_str());
		std::fprintf(f, "map %s\n", map);
		std::fprintf(f, "mode %s\n", ModeName(mode));
		std::fprintf(f, "p2_feet %.3f %.3f %.3f\n", feet.x, feet.y, feet.z);
		std::fprintf(f, "p2_view %.3f %.3f %.3f\n", va.x, va.y, va.z);
		std::fprintf(f, "p2_eye %.3f %.3f %.3f   # the rendered main view (frame %d), angles %.2f %.2f %.2f\n", mv.origin.x, mv.origin.y, mv.origin.z, mv.frame, mv.angles.x,
		             mv.angles.y, mv.angles.z);
		std::fprintf(f, "p2_velocity %.1f %.1f %.1f   # %.1f u/s\n", vel.x, vel.y, vel.z, vel.Length());
		std::fprintf(f, "p2_noclip %d\np2_holding %d\n", PlayerNoclip() ? 1 : 0, PlayerHolding() ? 1 : 0);
		double fx, fy, fz;
		SrcToMcD(feet, fx, fy, fz);
		std::fprintf(f, "p2_feet_in_mc %.3f %.3f %.3f   # the Portal 2 feet in Minecraft coordinates (mapping anchor MC %.1f %.1f %.1f = src %.1f %.1f %.1f)\n", fx, fy, fz, g_map.mcX,
		             g_map.mcY, g_map.mcZ, g_map.src.x, g_map.src.y, g_map.src.z);
		std::fprintf(f, "mc_linked %d (pid %u)\n", link::McAlive() ? 1 : 0, link::McPid());
		std::fprintf(f, "mc_feet %.4f %.4f %.4f   # Minecraft's latest tick (prev %.4f %.4f %.4f)\n", mc.curX, mc.curY, mc.curZ, mc.prevX, mc.prevY, mc.prevZ);
		std::fprintf(f, "mc_feet_src %.3f %.3f %.3f   # interpolated, in Portal 2 units\n", mcFeet.x, mcFeet.y, mcFeet.z);
		std::fprintf(f, "mc_look %.3f %.3f   # yaw pitch (Minecraft degrees); Portal 2 yaw %.2f\n", mc.yaw, mc.pitch, McYawToSrc(mc.yaw));
		std::fprintf(f, "mc_eye %.4f %.4f %.4f   # eye height %.3f blocks\n", mc.eyeX, mc.eyeY, mc.eyeZ, mc.tickEye);
		std::fprintf(f, "mc_flags 0x%x  in_world %d screen %d ground %d sneak %d sprint %d dead %d swim %d flying %d camera %u\n", mc.flags, !!(mc.flags & kMcInWorld),
		             !!(mc.flags & kMcScreenOpen), !!(mc.flags & kMcOnGround), !!(mc.flags & kMcSneaking), !!(mc.flags & kMcSprinting), !!(mc.flags & kMcDead),
		             !!(mc.flags & kMcSwimming), !!(mc.flags & kMcFlying), mc.cameraMode);
		std::fprintf(f, "mc_teleport_ack %u  ride mover %u flags %u (mover origin %.3f %.3f %.3f)\n", mc.teleportAck, mc.rideMover, mc.rideFlags, mc.rideCurX, mc.rideCurY, mc.rideCurZ);
		// crosshair: Portal 2
		const Vector eye = mv.origin, fw = Forward(va);
		for (int k = 0; k < 2; ++k) {
			CGameTrace tr;
			TraceIgnoringPlayer(eye, eye + fw * 8192.0f, k == 0 ? MASK_SHOT : MASK_PLAYERSOLID, tr);
			std::fprintf(f, "p2_crosshair_%s %s at %.1f %.1f %.1f, %.1f u, normal %.2f %.2f %.2f, surface '%s', contents 0x%x, entity %s\n", k == 0 ? "shot" : "playersolid",
			             tr.fraction < 1.0f ? "hit" : "nothing", tr.endpos.x, tr.endpos.y, tr.endpos.z, tr.fraction * 8192.0f, tr.plane.normal.x, tr.plane.normal.y, tr.plane.normal.z,
			             tr.surface.name ? tr.surface.name : "", tr.contents, EntName(tr.m_pEnt).c_str());
		}
		// crosshair: Minecraft (the outline block; the face from Minecraft's eye along its look; entities are not exported)
		float smn[3], smx[3];
		if (McSelection(smn, smx)) {
			const double yr = mc.yaw * M_PI / 180, pr = mc.pitch * M_PI / 180;
			const double d[3] = {-std::sin(yr) * std::cos(pr), -std::sin(pr), std::cos(yr) * std::cos(pr)}, o[3] = {mc.eyeX, mc.eyeY, mc.eyeZ};
			double tmin = -1e30, tmax = 1e30;
			int face = -1;
			for (int i = 0; i < 3; ++i) {
				if (std::fabs(d[i]) < 1e-9) continue;
				double t0 = (smn[i] - o[i]) / d[i], t1 = (smx[i] - o[i]) / d[i];
				int f0 = i * 2, f1 = i * 2 + 1;  // entering the min plane = facing -axis
				if (t0 > t1) std::swap(t0, t1), std::swap(f0, f1);
				if (t0 > tmin) tmin = t0, face = f0;
				tmax = std::min(tmax, t1);
			}
			static const char *kFaces[6] = {"west (-x)", "east (+x)", "down (-y)", "up (+y)", "north (-z)", "south (+z)"};
			const Vector a = McToSrc(smn[0], smn[1], smn[2]), b = McToSrc(smx[0], smx[1], smx[2]);
			std::fprintf(f, "mc_crosshair block %d %d %d (outline %.3f %.3f %.3f .. %.3f %.3f %.3f) face %s at %.2f blocks; Portal 2 box %.1f %.1f %.1f .. %.1f %.1f %.1f\n",
			             (int)std::floor((smn[0] + smx[0]) * 0.5), (int)std::floor((smn[1] + smx[1]) * 0.5), (int)std::floor((smn[2] + smx[2]) * 0.5), smn[0], smn[1], smn[2], smx[0], smx[1],
			             smx[2], face >= 0 && tmin <= tmax ? kFaces[face] : "? (the look misses the outline)", tmin, std::fmin(a.x, b.x), std::fmin(a.y, b.y), std::fmin(a.z, b.z),
			             std::fmax(a.x, b.x), std::fmax(a.y, b.y), std::fmax(a.z, b.z));
		} else {
			std::fprintf(f, "mc_crosshair none (no outlined block)\n");
		}
		std::fprintf(f, "mc_crosshair_entity n/a (the mod exports no hit entity; see p2_crosshair_shot)\n");
		// switch / carry / takeover
		std::fprintf(f, "last_switch %s (%d switches seen since load)\n",
		             g_lastSwitchAt < 0 ? "none" : (std::to_string(now - g_lastSwitchAt).substr(0, 6) + " s ago").c_str(), g_switches);
		std::fprintf(f, "minecraft_leads %d   # 0 in Minecraft mode: Portal 2 carries the body (a takeover) or a teleport is awaited\n", McLeads() ? 1 : 0);
		double carried = 0;
		for (const FrameRec &r : g_frames)
			if (r.flags & kFP2Carry) carried += r.dt;
		std::fprintf(f, "p2_carry_seconds_in_ring %.2f\n", carried);
		std::fprintf(f, "moving_solid_under '%s'\nfeet_collision_why '%s'\ncollision_patches_pending %zu\n", CollisionFieldLastMovingUnder(), CollisionFieldFeetWhy(),
		             CollisionFieldDynPendingCount());
		std::fprintf(f, "region_map %d in_region %d have_exit %d\n", ClipHasRegion() ? 1 : 0, ClipHasRegion() && ClipRegionContains(feet) ? 1 : 0, ClipHaveExit() ? 1 : 0);
		float nd = 0;
		const std::string near = MapIndexNearest(feet, &nd);
		std::fprintf(f, "nearest_named %s, %.0f u\n", near.empty() ? "-" : near.c_str(), nd);
		std::fclose(f);
	}

	// ---- colfield.txt: 5x5x4 blocks around the feet (x, z +-2; y from the block under the feet up 4)
	const Vector at = mcOn ? mcFeet : feet;
	double ax, ay, az;
	SrcToMcD(at, ax, ay, az);
	const int bx = (int)std::floor(ax), by = (int)std::floor(ay), bz = (int)std::floor(az);
	if (FILE *f = std::fopen((dir + "/colfield.txt").c_str(), "w")) {
		std::vector<ColBlock> sent;
		CollisionFieldSentIn(bx - 2, by - 1, bz - 2, bx + 2, by + 2, bz + 2, sent);
		std::fprintf(f, "# collision around the feet (MC block %d %d %d, Portal 2 %.1f %.1f %.1f): 'sent' = what Minecraft holds (the export's last send, patches merged),\n", bx, by,
		             bz, at.x, at.y, at.z);
		std::fprintf(f, "# 'fresh' = voxelised now (pcb_colfield_block). Bits: one uint64 per layer bottom->top, bit (z*8+x), 1/8-block voxels (5 u).\n");
		const int R = CollisionFieldRegionSize();
		auto rf = [R](int b) { return (int)std::floor(b / (double)R); };
		std::fprintf(f, "# regions (%d blocks): ", R);
		for (int ry = rf(by - 1); ry <= rf(by + 2); ++ry)
			for (int rz = rf(bz - 2); rz <= rf(bz + 2); ++rz)
				for (int rx = rf(bx - 2); rx <= rf(bx + 2); ++rx) {
					const int s = CollisionFieldRegionState(rx, ry, rz);
					std::fprintf(f, "(%d %d %d) %s; ", rx, ry, rz, s == 0 ? "NOT SENT" : s == 2 ? "sent, PATCH PENDING" : "sent");
				}
		std::fprintf(f, "\n");
		int diffs = 0;
		for (int y = by - 1; y <= by + 2; ++y)
			for (int z = bz - 2; z <= bz + 2; ++z)
				for (int x = bx - 2; x <= bx + 2; ++x) {
					const ColBlock *s = nullptr;
					for (const ColBlock &b : sent)
						if (b.x == x && b.y == y && b.z == z) s = &b;
					ColBlock fr{};
					const bool any = CollisionFieldVoxeliseNow(x, y, z, fr);
					int onlySent = 0, onlyFresh = 0;
					for (int l = 0; l < 8; ++l) {
						const uint64_t a = s ? s->bits[l] : 0, b = any ? fr.bits[l] : 0;
						onlySent += __builtin_popcountll(a & ~b), onlyFresh += __builtin_popcountll(b & ~a);
					}
					const Vector c = McToSrc(x + 0.5, y + 0.5, z + 0.5);
					std::fprintf(f, "block %d %d %d (dx %+d dy %+d dz %+d) centre %.1f %.1f %.1f%s\n", x, y, z, x - bx, y - by, z - bz, c.x, c.y, c.z,
					             onlySent || onlyFresh ? "   DIFFERS" : "");
					WriteBlockLayers(f, "sent", s);
					WriteBlockLayers(f, "fresh", any ? &fr : nullptr);
					if (onlySent || onlyFresh) std::fprintf(f, "  diff   %d voxels only in what was sent (stale or missing now), %d only in the fresh voxelisation (not sent)\n", onlySent, onlyFresh), ++diffs;
				}
		std::fprintf(f, "# %d of 100 blocks differ\n", diffs);
		std::fclose(f);
	}

	// ---- dyn.txt
	if (FILE *f = std::fopen((dir + "/dyn.txt").c_str(), "w")) {
		std::vector<std::tuple<void *, Vector, Vector>> dyn;
		CollisionFieldDynTracked(dyn);
		std::fprintf(f, "# moving solids the export tracks within 6 blocks (240 u) of the feet; probe = pcb_dyn_probe at the origin, half 4 u\n");
		int n = 0;
		for (auto &d : dyn) {
			const Vector o = std::get<1>(d), ex = std::get<2>(d);
			if ((o - at).Length() > 240.0f && (ex - at).Length() > 240.0f) continue;
			int dl = 0, w = 0, all = 0;
			std::string what;
			CollisionFieldDynProbe(o, 4.0f, dl, w, all, what);
			std::fprintf(f, "%s at %.1f %.1f %.1f (exported at %.1f %.1f %.1f%s), %.0f u away: probe moving-solid %d world %d everything %d (%s)\n", EntName(std::get<0>(d)).c_str(), o.x,
			             o.y, o.z, ex.x, ex.y, ex.z, (o - ex).Length() > 0.5f ? ", MOVED SINCE" : "", (o - at).Length(), dl, w, all, what.c_str());
			++n;
		}
		std::fprintf(f, "# %d of %zu tracked\n", n, dyn.size());
		std::fclose(f);
	}

	// ---- ring.txt
	std::vector<InRec> ins;
	{
		std::lock_guard<std::mutex> lk(g_inMx);
		ins.assign(g_inputs.begin(), g_inputs.end());
	}
	if (FILE *f = std::fopen((dir + "/ring.txt").c_str(), "w")) {
		std::fprintf(f, "# the last %.0f s before the mark (t in s, 0 = the mark)\n", kKeep);
		std::fprintf(f, "# F t dt_ms mode feet(x y z) view(pitch yaw) vel(x y z) mc_feet(x y z) flags[noclip holding p2carry mcground focus paused]\n");
		std::fprintf(f, "# I t src(mc = Minecraft received it, sdl = Portal 2's SDL event) type code a b c\n");
		size_t ii = 0;
		static const char *kTypes[] = {"?", "key", "button", "wheel", "cursor/motion", "text", "release-all"};
		for (const FrameRec &r : g_frames) {
			while (ii < ins.size() && ins[ii].t <= r.t) {
				const InRec &e = ins[ii++];
				std::fprintf(f, "I %.3f %s %s %u %d %d %d\n", e.t - now, e.src ? "sdl" : "mc", e.type < 7 ? kTypes[e.type] : "?", e.code, e.a, e.b, e.c);
			}
			std::fprintf(f, "F %.3f %.1f %s %.2f %.2f %.2f %.2f %.2f %.1f %.1f %.1f %.2f %.2f %.2f %d%d%d%d%d%d\n", r.t - now, r.dt * 1000, ModeName(r.mode), r.feet.x, r.feet.y,
			             r.feet.z, r.view.x, r.view.y, r.vel.x, r.vel.y, r.vel.z, r.mcFeet.x, r.mcFeet.y, r.mcFeet.z, !!(r.flags & kFNoclip), !!(r.flags & kFHolding),
			             !!(r.flags & kFP2Carry), !!(r.flags & kFMcGround), !!(r.flags & kFFocus), !!(r.flags & kFPaused));
		}
		for (; ii < ins.size(); ++ii) {
			const InRec &e = ins[ii];
			std::fprintf(f, "I %.3f %s %s %u %d %d %d\n", e.t - now, e.src ? "sdl" : "mc", e.type < 7 ? kTypes[e.type] : "?", e.code, e.a, e.b, e.c);
		}
		std::fclose(f);
	}

	// ---- replay.txt
	if (FILE *f = std::fopen((dir + "/replay.txt").c_str(), "w")) {
		const bool linked = link::McAlive();
		const FrameRec *first = g_frames.empty() ? nullptr : &g_frames.front();
		std::fprintf(f, "# replay of bug mark %s (bridge #24): %s, %s mode\n", name.c_str(), map, ModeName(mode));
		std::fprintf(f, "# run: tools/locked_run.sh %s %s %s 400 bm_%s   (PCB_SO=<absolute .so>)\n", ClipHasRegion() ? "terrain" : "mirror", (dir + "/replay.txt").c_str(), map,
		             name.c_str());
		std::fprintf(f, "# part 1 replays the ~%.0f s before the mark (pose track in Chell mode, Minecraft's received input in Minecraft mode);\n", kKeep);
		std::fprintf(f, "# part 2 puts the player at the marked pose (noclip setpos/setang, same mode) and checks it within 1 u.\n");
		std::fprintf(f, "pcb_assert_soft 1\nsv_cheats 1\nwaituntil spawned 600\nwaituntil map%s 1200 abort\n", map);
		if (linked) std::fprintf(f, "waituntil link 9000\n");
		if (ClipHaveExit()) std::fprintf(f, "waituntil haveexit 3600\n");
		std::fprintf(f, "pcb_log REPLAY %s\n", name.c_str());
		auto place = [&](const Vector &p, const QAngle &v, int m, bool keepNoclip) {
			std::fprintf(f, "bridge_mode chell\nwaituntil chellmode 600\npcb_noclip_set 1\nsetpos %.3f %.3f %.3f\nsetang %.3f %.3f 0\npcb_setview %.3f %.3f\npcb_stop\nwait 30\n", p.x, p.y,
			             p.z, v.x, v.y, v.x, v.y);
			// (Minecraft mode: Chell stays in noclip through the switch, so Minecraft is teleported to the exact feet, not to where
			// her hull would drop to: Portal 2's floor and Minecraft's 5 u voxel top can differ by a voxel)
			if (!keepNoclip && m == 0) std::fprintf(f, "pcb_noclip_set 0\nwait 30\n");
			if (m != 0) std::fprintf(f, "bridge_mode mc\nwaituntil mcmode 1200\nwaituntil mcleads 600\npcb_noclip_set 0\npcb_setview %.3f %.3f\nwait 10\n", v.x, v.y);
		};
		if (first) {
			std::fprintf(f, "# ---- part 1: the lead-up from t %.2f s\n", first->t - now);
			// (a Chell pose track keeps noclip on between its setpos steps; part 2 sets the marked noclip state)
			place(first->feet, first->view, first->mode == 1 ? 1 : 0, first->mode != 1);
			std::fprintf(f, "pcb_pos_mark\n");
			// events in 1/60 s steps
			struct Ev {
				int tick;
				std::string cmd;
			};
			std::vector<Ev> evs;
			const double t0 = first->t;
			auto tick = [&](double t) { return (int)std::lround((t - t0) * 60.0); };
			// the view (and in Chell mode the pose: a pose track, Portal 2's keys can't be replayed through these commands)
			QAngle lastV = first->view;
			Vector lastP = first->feet;
			int lastPoseTick = 0;
			int lastMode = first->mode == 1 ? 1 : 0;
			for (const FrameRec &r : g_frames) {
				const int k = tick(r.t);
				char b[160];
				const int m = r.mode == 1 ? 1 : 0;
				if (m != lastMode) {
					evs.push_back({k, m ? "pcb_noclip_set 0" : "# (back to Chell)"});
					evs.push_back({k, m ? "bridge_mode mc" : "bridge_mode chell"});
					if (!m) evs.push_back({k, "pcb_noclip_set 1"});
					lastMode = m;
				}
				if (std::fabs(std::remainder(r.view.x - lastV.x, 360.0f)) > 0.05f || std::fabs(std::remainder(r.view.y - lastV.y, 360.0f)) > 0.05f) {
					std::snprintf(b, sizeof b, "pcb_setview %.3f %.3f", r.view.x, r.view.y);
					evs.push_back({k, b}), lastV = r.view;
				}
				if (r.mode != 1 && (r.feet - lastP).Length() > 0.5f && k - lastPoseTick >= 3) {
					std::snprintf(b, sizeof b, "setpos %.3f %.3f %.3f", r.feet.x, r.feet.y, r.feet.z);
					evs.push_back({k, b}), lastP = r.feet, lastPoseTick = k;
				}
			}
			bool held[512] = {};
			int lastCx = -1, lastCy = -1;
			for (const InRec &e : ins) {
				if (e.t < t0) continue;
				const int k = tick(e.t);
				char b[160];
				if (e.src == 1) {
					std::snprintf(b, sizeof b, "# sdl %s %u %d %d", e.type == 1 ? "key" : e.type == 2 ? "button" : e.type == 3 ? "wheel" : "motion", e.code, e.a, e.b);
					if (e.type != 4) evs.push_back({k, b});  // (raw motion is in the view track)
					continue;
				}
				switch (e.type) {
				case kInKey:
					if (e.code == g_key) continue;  // (the mark key itself)
					if (e.code < 512) held[e.code] = e.a != 0;
					std::snprintf(b, sizeof b, "pcb_mckey %u %d", e.code, e.a ? 1 : 0);
					break;
				case kInMouseButton:
					if (e.code == 1 || e.code == 3) std::snprintf(b, sizeof b, "pcb_mckey %s %d", e.code == 1 ? "lmb" : "rmb", e.a ? 1 : 0);
					else std::snprintf(b, sizeof b, "# mouse button %u %d (no replay command)", e.code, e.a);
					break;
				case kInScroll: std::snprintf(b, sizeof b, "pcb_sdl_event wheel %d", e.a / 120); break;
				case kInCursor:
					if (lastCx >= 0) std::snprintf(b, sizeof b, "pcb_sdl_event motion %d %d", e.a - lastCx, e.b - lastCy);
					else std::snprintf(b, sizeof b, "# cursor at %d %d (first in the record: no delta)", e.a, e.b);
					lastCx = e.a, lastCy = e.b;
					break;
				case kInText: std::snprintf(b, sizeof b, "# text U+%04X (no replay command)", (unsigned)e.a); break;
				case kInReleaseAll:
					b[0] = 0;
					for (int s = 1; s < 512; ++s)
						if (held[s]) {
							char r[32];
							std::snprintf(r, sizeof r, "pcb_mckey %d 0", s);
							evs.push_back({k, r}), held[s] = false;
						}
					std::snprintf(b, sizeof b, "# release all");
					break;
				default: b[0] = 0;
				}
				if (b[0]) evs.push_back({k, b});
			}
			std::stable_sort(evs.begin(), evs.end(), [](const Ev &a, const Ev &b) { return a.tick < b.tick; });
			int cur = 0;
			for (const Ev &e : evs) {
				if (e.tick > cur) std::fprintf(f, "wait %d\n", e.tick - cur), cur = e.tick;
				std::fprintf(f, "%s\n", e.cmd.c_str());
			}
			const int end = tick(now);
			if (end > cur) std::fprintf(f, "wait %d\n", end - cur);
			for (int s = 1; s < 512; ++s)
				if (held[s]) std::fprintf(f, "pcb_mckey %d 0\n", s);
			std::fprintf(f, "pcb_mckey lmb 0\npcb_mckey rmb 0\n");
			std::fprintf(f, "# where the lead-up ended vs the mark (information only: physics and timing differ)\n");
			std::fprintf(f, "pcb_pose_diff leadup %.3f %.3f %.3f\n", feet.x, feet.y, feet.z);
			std::fprintf(f, "pcb_glhud_shot bm_%s_leadup\nwait 20\n", name.c_str());
		}
		std::fprintf(f, "# ---- part 2: the marked moment\n");
		place(feet, va, mode == 1 ? 1 : 0, mode != 1 && PlayerNoclip());
		std::fprintf(f, "pcb_assert_pose %.3f %.3f %.3f 1 %.3f %.3f 1\n", feet.x, feet.y, feet.z, va.x, va.y);
		std::fprintf(f, "pcb_glhud_shot bm_%s_moment\nwait 30\n", name.c_str());
		std::fprintf(f, "pcb_run_summary\nquit\n");
		std::fclose(f);
	}
	// an index of all marks
	if (FILE *f = std::fopen((root + "/index.txt").c_str(), "a")) {
		std::fprintf(f, "%s %s %s feet %.1f %.1f %.1f view %.1f %.1f\n", name.c_str(), map, ModeName(mode), feet.x, feet.y, feet.z, va.x, va.y);
		std::fclose(f);
	}
	return dir;
}

void Mark(const std::string &label) {
	if (!McLevelIsLive()) return void(console->Print("[bridge] bug mark: no level is live\n"));
	const std::string dir = WriteBundle(label);
	if (dir.empty()) return void(console->Warn("[bridge] bug mark: could not create a directory under %s/bugmarks\n", CacheDir().c_str()));
	g_lastBundle = dir;
	const Vector p = PlayerOrigin();
	const QAngle v = engine.ViewAngles();
	char line[512];
	std::snprintf(line, sizeof line, "[bridge] BUG MARKED -> %s (%s, %s, feet %.1f %.1f %.1f, view %.1f %.1f)\n", dir.c_str(), McMapName(), ModeName(ModeNow()), p.x, p.y, p.z, v.x, v.y);
	console->Print("%s", line);
	if (g_bridgeQuiet && console->ColorMsg) console->ColorMsg(Color(255, 210, 80), "%s", line);  // (quiet maps: still to the console)
	const std::string base = dir.substr(dir.find_last_of('/') + 1);
	std::snprintf(g_toastText, sizeof g_toastText, "BUG MARKED %s", base.substr(0, 40).c_str());
	g_toastUntil = NowS() + 2.5;
}

}  // namespace

// ---- glhud.cpp's toast (render thread): rectangles in window pixels, top-left origin; 0 when none ------------------------
bool BugMarkToastActive() { return NowS() < g_toastUntil.load(); }
int BugMarkToastRects(int W, int H, int (*rects)[4], uint32_t *rgba, int max) {
	if (!BugMarkToastActive() || W <= 0 || H <= 0) return 0;
	char text[64];
	std::memcpy(text, g_toastText, sizeof text);
	text[63] = 0;
	const int len = (int)std::strlen(text);
	const int s = std::max(2, H / 360);  // font pixel
	const int tw = len * 6 * s, th = 7 * s, pad = 3 * s;
	const int x0 = (W - tw) / 2, y0 = H / 8;
	int n = 0;
	if (n < max) rects[n][0] = x0 - pad, rects[n][1] = y0 - pad, rects[n][2] = tw + 2 * pad, rects[n][3] = th + 2 * pad, rgba[n++] = 0xE0201810;  // dark bar (ABGR)
	if (n < max) rects[n][0] = x0 - pad, rects[n][1] = y0 + th + pad - s / 2, rects[n][2] = tw + 2 * pad, rects[n][3] = std::max(1, s / 2), rgba[n++] = 0xFF30C0FF;  // underline
	for (int i = 0; i < len; ++i)
		for (int y = 0; y < 7; ++y) {
			int run = -1;
			for (int x = 0; x <= 5; ++x) {
				const bool on = x < 5 && FontPixel(text[i], x, y);
				if (on && run < 0) run = x;
				if (!on && run >= 0) {
					if (n < max) rects[n][0] = x0 + (i * 6 + run) * s, rects[n][1] = y0 + y * s, rects[n][2] = (x - run) * s, rects[n][3] = s, rgba[n++] = 0xFF40D8FF;  // amber
					run = -1;
				}
			}
		}
	return n;
}

// pcb_run_summary (composite.cpp's RUN PERF line): "" when the window had focus, else why timing is not valid
const char *RunFocusNote() {
	static char buf[160];
	const uint64_t all = g_focusFrames + g_unfocusFrames;
	if (!all || !g_unfocusFrames) return "";
	std::snprintf(buf, sizeof buf, " (TIMING NOT VALID: the window was not focused for %llu of %llu frames, SDL window flags)", (unsigned long long)g_unfocusFrames,
	              (unsigned long long)all);
	return buf;
}

// Once per server frame (plugin.cpp GameFrame). levelLive: the level has started.
void TestToolsFrame(bool levelLive) {
	ResolveSdl();
	static bool tapOn = false;
	if (!tapOn) tapOn = true, link::SetInputTap(InputTap);
	if (!g_watchOn && g_sdl.AddEventWatch) g_sdl.AddEventWatch(SdlWatchFn, nullptr), g_watchOn = true;
	const double now = NowS();
	// focus (SDL window flags: SDL_WINDOW_INPUT_FOCUS 0x200)
	bool focus = true;
	if (g_sdl.GetWindowFlags && GameWindow()) {
		focus = false;
		for (uint32_t id = 1; id <= 8 && !focus; ++id)
			if (void *w = g_sdl.GetWindowFromID(id)) focus = (g_sdl.GetWindowFlags(w) & 0x200) != 0;
	}
	++(focus ? g_focusFrames : g_unfocusFrames);
	MapIndexFrame(levelLive);
	if (!levelLive) return;
	// the rolling record
	static double last = 0;
	const int mode = ModeNow();
	if (mode != g_lastMode) {
		if (g_lastMode >= 0) ++g_switches, g_lastSwitchAt = now;
		g_lastMode = mode;
	}
	FrameRec r{};
	r.t = now, r.dt = last > 0 ? (float)(now - last) : 0.0f, last = now;
	r.mode = (uint8_t)mode;
	r.feet = PlayerOrigin(), r.vel = PlayerVelocity(), r.view = engine.ViewAngles();
	r.mcFeet = mode == 1 ? McFeetSrc() : Vector(0, 0, 0);
	r.flags = (PlayerNoclip() ? kFNoclip : 0) | (PlayerHolding() ? kFHolding : 0) | (mode == 1 && !McLeads() ? kFP2Carry : 0) | (mode == 1 && McOnGround() ? kFMcGround : 0) |
	          (focus ? kFFocus : 0) | (engine.IsPaused(engine.client->ThisPtr()) ? kFPaused : 0);
	if (r.feet.Length() > 0.1f) g_frames.push_back(r);  // (not before the player has spawned: a pose at 0 0 0)
	while (!g_frames.empty() && now - g_frames.front().t > kKeep) g_frames.pop_front();
	{
		std::lock_guard<std::mutex> lk(g_inMx);
		while (!g_inputs.empty() && now - g_inputs.front().t > kKeep) g_inputs.pop_front();
		while (g_inputs.size() > 20000) g_inputs.pop_front();
	}
	// the key (no bind: polled, like J; the test hook holds it for 0.1 s through the same path)
	int n = 0;
	const uint8_t *ks = g_sdl.GetKeyboardState ? g_sdl.GetKeyboardState(&n) : nullptr;
	const bool kfocus = !g_sdl.GetKeyboardFocus || g_sdl.GetKeyboardFocus() != nullptr;
	const bool down = g_key > 0 && ((ks && g_key < n && ks[g_key] && kfocus) || now < g_simUntil);
	if (down && !g_keyDown) {
		const std::string label = g_pendingLabel;
		g_pendingLabel.clear();
		Mark(label);
	}
	g_keyDown = down;
	ColOverlayFrame();
}

void TestToolsShutdown() {
	link::SetInputTap(nullptr);
	if (g_watchOn && g_sdl.DelEventWatch) g_sdl.DelEventWatch(SdlWatchFn, nullptr);
	g_watchOn = false;
	g_toastUntil = 0;
	ColOverlayShutdown();
}

CON_COMMAND(pc_bugmark_key, "pc_bugmark_key [scancode|0] - the \"mark bug\" key (SDL scancode, default 65 = F8; 0 off). No bind is written\n") {
	if (args.ArgC() > 1) g_key = std::atoi(args[1]);
	console->Print("[bridge] bug mark key: scancode %d\n", g_key);
}
CON_COMMAND(pcb_bugmark, "pcb_bugmark press [label] | now [label] | last - (tests) press the bug-mark key through its real polling path, or mark at once; last: the newest bundle\n") {
	const char *a = args.ArgC() > 1 ? args[1] : "now";
	if (!std::strcmp(a, "last")) return console->Print("[bridge] bug mark last: %s\n", g_lastBundle.empty() ? "(none this session)" : g_lastBundle.c_str());
	g_pendingLabel = args.ArgC() > 2 ? args[2] : "";
	if (!std::strcmp(a, "press")) {
		g_simUntil = NowS() + 0.1;
		return;
	}
	const std::string label = g_pendingLabel;
	g_pendingLabel.clear();
	Mark(label);
}
void McNoteScriptedTurn();  // mctransit.cpp
CON_COMMAND(pcb_setview, "pcb_setview <pitch> <yaw> - (tests) set the view angles on the client now (no cheats; Minecraft's look follows)\n") {
	if (args.ArgC() < 3) return;
	McNoteScriptedTurn();
	engine.SetAngles(QAngle{(float)std::atof(args[1]), (float)std::atof(args[2]), 0});
}
CON_COMMAND(pcb_assert_pose, "pcb_assert_pose <x> <y> <z> [tol_u=1] [pitch yaw [tol_deg=1]] - (tests) the player's feet (the body; Minecraft's in its mode) and view\n") {
	if (args.ArgC() < 4) return console->Print("%s", pcb_assert_pose.ThisPtr()->m_pszHelpString);
	const Vector want((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3]));
	const float tol = args.ArgC() > 4 ? (float)std::atof(args[4]) : 1.0f;
	const Vector body = PlayerOrigin(), mcf = McFeetSrc();
	const bool mc = McModeActive();
	const float d = (body - want).Length(), dmc = mc ? (mcf - want).Length() : 0.0f;
	bool ok = d <= tol && (!mc || dmc <= tol);
	char buf[300];
	int k = std::snprintf(buf, sizeof buf, "body %.2f %.2f %.2f: %.2f u from %.2f %.2f %.2f", body.x, body.y, body.z, d, want.x, want.y, want.z);
	if (mc) k += std::snprintf(buf + k, sizeof buf - k, "; Minecraft feet %.2f %.2f %.2f: %.2f u", mcf.x, mcf.y, mcf.z, dmc);
	if (args.ArgC() > 6) {
		const float p = (float)std::atof(args[5]), y = (float)std::atof(args[6]), td = args.ArgC() > 7 ? (float)std::atof(args[7]) : 1.0f;
		const QAngle va = engine.ViewAngles();
		const float dp = std::fabs(std::remainder(va.x - p, 360.0f)), dy = std::fabs(std::remainder(va.y - y, 360.0f));
		ok = ok && dp <= td && dy <= td;
		std::snprintf(buf + k, sizeof buf - k, "; view %.2f %.2f (off %.2f %.2f deg)", va.x, va.y, dp, dy);
	}
	TestCheck(ok, "pose reproduced", buf);
}
CON_COMMAND(pcb_pose_diff, "pcb_pose_diff <label> <x> <y> <z> - (tests) log how far the player's feet are from a point (no assertion)\n") {
	if (args.ArgC() < 5) return;
	const Vector want((float)std::atof(args[2]), (float)std::atof(args[3]), (float)std::atof(args[4])), p = PlayerOrigin();
	console->Print("[bridge] pose diff %s: feet %.1f %.1f %.1f, %.1f u from %.1f %.1f %.1f\n", args[1], p.x, p.y, p.z, (p - want).Length(), want.x, want.y, want.z);
}

void FocusCounts(uint64_t &focused, uint64_t &unfocused) { focused = g_focusFrames, unfocused = g_unfocusFrames; }  // (bridge #53) growth.cpp
}  // namespace bridge
