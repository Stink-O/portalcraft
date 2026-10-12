// Portalcraft bridge: the Minecraft player in Portal 2 (SkyCraft's PlayerPuppet, CameraDriver
// and InputBridge, with Portal 2 as the host).
//
// Per host frame (ClientModeShared::OverrideView, called once per rendered frame):
//   * heartbeat + SkyState: host feet/look in Minecraft coordinates, viewport, flags. Minecraft
//     takes its look direction from the host (Portal 2 keeps handling the mouse);
//   * McState: Minecraft's raw 20 Hz physics ticks, interpolated on the host clock exactly like
//     Minecraft's renderer does with partial ticks;
//   * CameraDriver: in Minecraft mode the view origin is Minecraft's eye and the FOV Minecraft's;
//   * InputBridge: the keyboard and mouse buttons (SDL scancodes, read from Portal 2's SDL) go to
//     Minecraft's input ring;
//   * CollisionField streaming (collfield.cpp).
// Per movement tick (ProcessMovement, puppet.cpp): PlayerPuppet writes Minecraft's position and
// velocity into Portal 2's player, so triggers, buttons and cameras still see a body there.
// ClientModeShared::CreateMove: in Minecraft mode Portal 2's own movement/use/fire buttons are
// cleared, so Chell's body neither walks, jumps, crouches nor fires the portal gun.
#include <map>
#include <chrono>
#include "bridge.hpp"
#include "growth.hpp"
#include "coords.hpp"
#include "link.hpp"
#include "prof.hpp"
#include "stage.hpp"

#include "Modules/Tier1.hpp"
#include "Offsets.hpp"
#include "Utils/Memory.hpp"
#include "Utils/SDK/UserCmd.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include "platform/platform.hpp"
#include <sys/stat.h>
#include <cstdio>

namespace bridge {

void DigHostFill(skycraft::proto::HostPlayer &hp);  // dig.cpp (bridge #33)

using namespace skycraft::proto;

void CollisionFieldFrame(const Vector &playerSrc, bool exportRegions);
bool SceneChamberEntities();  // scene.cpp (bridge #22)
void CollisionFieldLevelStart();
void CollisionFieldTerrainOnly(bool on);
void CollisionFieldStage(bool on);  // collfield.cpp (bridge #49)
void MoversFrame(const Vector &focus, bool active);  // movers.cpp (bridge #26)
void TerrainConsume(double budgetMs);
void TerrainSetMapReady(bool ready);
void ClipExitRecheck(const char *why);  // clip.cpp
int ClipArmKey();                       // clip.cpp

RegionMap g_map;
// Terrain mode: Minecraft's (held) player parks at the region anchor so its chunks load and
// mesh there, and no Portal 2 collision is exported (the Minecraft world has its own ground).
static bool g_terrainMode = false;
bool McTerrainModeNow() { return g_terrainMode; }  // (bridge #61) a region map set the terrain mapping this session (stage.cpp: campaign maps use the terrain window)

enum class Mode { Chell, Minecraft };
static Mode g_mode = Mode::Chell;
static bool g_wantMc = false;       // requested; becomes active once Minecraft is in its world
static uint32_t g_teleportSeq = 0;  // bumps to move the Minecraft player to Chell
static uint32_t g_worldId = 0;
static bool g_levelLive = false;
static bool g_startMc = false;  // pc_start_mode mc
static double g_levelLiveAt = 0;
static bool g_legacyBody = false;  // (test knob, cache/bridge/s40_legacy) the pre-#40 body on the drawn car
static bool g_arrDbg = false;
static bool g_arrShots = false;  // (bridge #40) arrdbg lines
static McState g_mc{};
static bool g_haveMc = false;
static bool g_linkWasUp = false;
static int g_logFrames = 0;

// ---- SDL (Portal 2 links SDL2; its scancodes are Minecraft/SDL3's) ------------------------
using _SDL_GetKeyboardState = const uint8_t *(*)(int *numkeys);
using _SDL_GetMouseState = uint32_t (*)(int *x, int *y);
using _SDL_GetKeyboardFocus = void *(*)();
static _SDL_GetKeyboardState SDL_GetKeyboardState_ = nullptr;
static _SDL_GetMouseState SDL_GetMouseState_ = nullptr;
static _SDL_GetKeyboardFocus SDL_GetKeyboardFocus_ = nullptr;
using _SDL_GetWindowSize = void (*)(void *window, int *w, int *h);
static _SDL_GetWindowSize SDL_GetWindowSize_ = nullptr;
static uint8_t g_keysSent[512];
static uint32_t g_buttonsSent = 0;
static bool g_toggleKeyDown = false;
static const int kScToggle = 13;  // J: switch Chell <-> Minecraft (Skate 3 style)
static const int kScEscape = 41;
static const int kScGrave = 53;  // console key stays Portal 2's
static const int kScUse = 10;    // G: Portal 2's use as the Minecraft player
static bool g_useHeld = false;

static void ResolveSdl() {
	if (SDL_GetKeyboardState_) return;
	SDL_GetKeyboardState_ = (_SDL_GetKeyboardState)plat::SdlSymbol("SDL_GetKeyboardState");
	SDL_GetMouseState_ = (_SDL_GetMouseState)plat::SdlSymbol("SDL_GetMouseState");
	SDL_GetKeyboardFocus_ = (_SDL_GetKeyboardFocus)plat::SdlSymbol("SDL_GetKeyboardFocus");
	SDL_GetWindowSize_ = (_SDL_GetWindowSize)plat::SdlSymbol("SDL_GetWindowSize");
	console->Print("[bridge] SDL input: keyboard %p mouse %p focus %p\n", (void *)SDL_GetKeyboardState_, (void *)SDL_GetMouseState_, (void *)SDL_GetKeyboardFocus_);
}

// ---- Minecraft screens (inventory, chat, every GUI): SkyCraft's input routing (DESIGN-mcplayer M2b) -------------
// An SDL event filter (Portal 2 sets none; any earlier one is chained) sees every input event before Portal 2's
// event loop and its input system's watcher. In Minecraft mode it takes the mouse wheel (the hotbar), and while a
// Minecraft screen is open it also takes the mouse motion (it moves our cursor instead of Portal 2's view, so the
// view stays put), text (chat, signs), key and button presses (Portal 2's binds stay quiet; releases always pass,
// so no Portal 2 bind is left held) and Escape (it closes the Minecraft screen instead of opening Portal 2's menu).
// The console key stays Portal 2's. The keyboard state Minecraft's keys are polled from is updated by SDL before
// the filter runs, so held keys still reach Minecraft through PumpInput.
using SDL_EventFilter_ = int (*)(void *userdata, void *event);
using _SDL_SetEventFilter = void (*)(SDL_EventFilter_ filter, void *userdata);
using _SDL_GetEventFilter = int (*)(SDL_EventFilter_ *filter, void **userdata);
static _SDL_SetEventFilter SDL_SetEventFilter_ = nullptr;
static SDL_EventFilter_ g_prevFilter = nullptr;
static void *g_prevFilterData = nullptr;
static bool g_filterInstalled = false;
static std::atomic<int> g_fMc{0}, g_fScreen{0};  // set each frame from the main loop
static std::atomic<int> g_fWheel{0}, g_fDx{0}, g_fDy{0}, g_fEsc{0};
static std::atomic<uint32_t> g_fText[32];
static std::atomic<int> g_fTextHead{0}, g_fTextTail{0};
// (bridge #21) pcb_mouse_log: every motion event seen by the filter (screen or not), for the "cursor recentres in the inventory" bug
static std::atomic<int> g_mouseLogLeft{0}, g_mouseLogHead{0}, g_mouseLogTail{0};
struct MouseRec { int which, x, y, xrel, yrel, screen, mc; };
static MouseRec g_mouseRing[256];
static std::atomic<int> g_warpsDropped{0};
// (lead, 2026-10-03) pc_input_lock 1 (test scripts turn it on): the real keyboard and mouse reach neither game; only the
// script's own input (pcb_mckey, pcb_sdl_event, console +commands) does. A person at the machine pressing keys during a
// run moved the Minecraft player off a test (cm_sp_a2_catapult_intro_1) and a stray J switched modes mid-test (full3).
static std::atomic<int> g_frameReqW{0}, g_frameReqH{0};  // McFrameRequested: the frame size last sent (the HUD render thread reads it)
void McFrameRequested(int &w, int &h) { w = g_frameReqW.load(), h = g_frameReqH.load(); }
static std::atomic<bool> g_inputLock{false};
static std::atomic<int> g_injecting{0};  // pcb_sdl_event pushing its own events through the filter
bool InputLocked() { return g_inputLock.load(); }
static int EventFilter(void *ud, void *ev) {
	const uint32_t type = *(const uint32_t *)ev;
	const uint8_t *e = (const uint8_t *)ev;
	if (g_inputLock.load() && !g_injecting.load() && (type == 0x300 || type == 0x301 || type == 0x303 || (type >= 0x400 && type <= 0x403))) return 0;
	if (type == 0x400 && g_mouseLogLeft.load() > 0) {
		g_mouseLogLeft.fetch_sub(1);
		const int h = g_mouseLogHead.load();
		g_mouseRing[h & 255] = {*(const int32_t *)(e + 12), *(const int32_t *)(e + 20), *(const int32_t *)(e + 24), *(const int32_t *)(e + 28), *(const int32_t *)(e + 32),
		                        g_fScreen.load(), g_fMc.load()};
		g_mouseLogHead = h + 1;
	}
	if (g_fMc.load()) {
		if (type == 0x403) {  // SDL_MOUSEWHEEL: y notches (flipped direction: the sign is SDL's)
			int y = *(const int32_t *)(e + 20);
			if (*(const uint32_t *)(e + 24) == 1) y = -y;  // SDL_MOUSEWHEEL_FLIPPED
			g_fWheel.fetch_add(y);
			return 0;
		}
		// (bridge #15) Minecraft mode, no screen: key presses are Minecraft's only (E inventory, 1-9 hotbar, Q drop,
		// F3...; SkyCraft's routing), so no Portal 2 bind fires; Minecraft polls the keyboard state. The console key
		// and Escape (Portal 2's pause menu) stay Portal 2's; releases always pass.
		if (type == 0x300 && !g_fScreen.load()) {
			const int sc = *(const int32_t *)(e + 16);
			if (sc != kScGrave && sc != kScEscape) return 0;
		}
		if (g_fScreen.load()) {
			if (type == 0x400) {  // SDL_MOUSEMOTION
				// (bridge #21) Portal 2 puts the pointer back at the window centre after reading it (a warp); SDL reports that warp
				// as a motion event of the opposite displacement, which cancelled the user's move and held the cursor at the
				// centre ("moving the mouse in the inventory recentres the cursor"). Such an event lands exactly on the centre,
				// from a position that was not the centre, with the displacement that closes the gap: it is not the user.
				static int lastX = -1, lastY = -1;
				const int px = *(const int32_t *)(e + 20), py = *(const int32_t *)(e + 24), rx = *(const int32_t *)(e + 28), ry = *(const int32_t *)(e + 32);
				int ww = 0, wh = 0;
				if (SDL_GetWindowSize_ && SDL_GetKeyboardFocus_) {
					if (void *win = SDL_GetKeyboardFocus_()) SDL_GetWindowSize_(win, &ww, &wh);
				}
				const int cx = ww / 2, cy = wh / 2;
				const bool warp = ww > 0 && std::abs(px - cx) <= 1 && std::abs(py - cy) <= 1 && (std::abs(lastX - cx) > 1 || std::abs(lastY - cy) > 1) && lastX >= 0 &&
				                  std::abs(rx - (px - lastX)) <= 2 && std::abs(ry - (py - lastY)) <= 2;
				lastX = px, lastY = py;
				if (warp) {
					++g_warpsDropped;
					return 0;
				}
				g_fDx.fetch_add(*(const int32_t *)(e + 28));
				g_fDy.fetch_add(*(const int32_t *)(e + 32));
				return 0;
			}
			if (type == 0x401) return 0;  // SDL_MOUSEBUTTONDOWN (Minecraft reads the buttons by polling)
			if (type == 0x303) {          // SDL_TEXTINPUT: UTF-8 -> code points
				const unsigned char *t = e + 12;
				for (int i = 0; i < 32 && t[i];) {
					uint32_t cp = t[i];
					int n = cp < 0x80 ? 1 : cp < 0xE0 ? 2 : cp < 0xF0 ? 3 : 4;
					cp = n == 1 ? cp : n == 2 ? cp & 0x1F : n == 3 ? cp & 0x0F : cp & 0x07;
					for (int k = 1; k < n && i + k < 32; ++k) cp = (cp << 6) | (t[i + k] & 0x3F);
					i += n;
					const int h = g_fTextHead.load();
					if (h - g_fTextTail.load() < 32) g_fText[h & 31] = cp, g_fTextHead = h + 1;
				}
				return 0;
			}
			if (type == 0x300) {  // SDL_KEYDOWN
				const int sc = *(const int32_t *)(e + 16);
				if (sc != kScGrave) {  // the console key stays Portal 2's
					if (sc == kScEscape && !e[13]) g_fEsc = 1;  // (not a repeat)
					return 0;
				}
			}
		}
	}
	return g_prevFilter ? g_prevFilter(g_prevFilterData, ev) : 1;
}
static void InstallFilter() {
	if (g_filterInstalled) return;
	SDL_SetEventFilter_ = (_SDL_SetEventFilter)plat::SdlSymbol("SDL_SetEventFilter");
	auto get = (_SDL_GetEventFilter)plat::SdlSymbol("SDL_GetEventFilter");
	if (!SDL_SetEventFilter_ || !get) return void(console->Print("[bridge] SDL event filter unavailable: no Minecraft screen input\n"));
	if (!get(&g_prevFilter, &g_prevFilterData)) g_prevFilter = nullptr, g_prevFilterData = nullptr;
	SDL_SetEventFilter_(EventFilter, nullptr);
	g_filterInstalled = true;
	console->Print("[bridge] SDL event filter installed (previous %p)\n", (void *)g_prevFilter);
}
static void RemoveFilter() {
	if (!g_filterInstalled) return;
	g_fMc = 0, g_fScreen = 0;
	SDL_SetEventFilter_(g_prevFilter, g_prevFilterData);
	g_filterInstalled = false;
}

// The cursor in Minecraft's window pixels (top-left origin), moved by the filtered mouse motion while a screen is open.
static int g_cursorX = 0, g_cursorY = 0;
static bool g_screenWas = false;
CON_COMMAND(pcb_mouse_log, "pcb_mouse_log <n> - log the next n mouse motion events the SDL filter sees (which, x, y, xrel, yrel, screen open, Minecraft mode)\n") {
	g_mouseLogLeft = args.ArgC() > 1 ? std::atoi(args[1]) : 100;
}
static void MouseLogFlush() {
	int t = g_mouseLogTail.load();
	const int h = g_mouseLogHead.load();
	if (h - t > 256) t = h - 256;
	for (; t != h; ++t) {
		const MouseRec &r = g_mouseRing[t & 255];
		console->Print("[bridge] mouse motion which %d pos (%d %d) rel (%d %d) screen %d mc %d; cursor (%d %d)\n", r.which, r.x, r.y, r.xrel, r.yrel, r.screen, r.mc, g_cursorX, g_cursorY);
	}
	g_mouseLogTail = t;
}
static void ScreenInput(bool forward, bool screen, int vw, int vh) {
	MouseLogFlush();
	int dx = g_fDx.exchange(0), dy = g_fDy.exchange(0), wheel = g_fWheel.exchange(0);
	const bool esc = g_fEsc.exchange(0) != 0;
	int h = g_fTextHead.load(), t = g_fTextTail.load();
	if (!forward) {  // (a paused or not-yet-leading frame: the screen state is kept, so it can't recentre the cursor)
		g_fTextTail = h;
		return;
	}
	if (wheel) link::PushInput(kInScroll, 0, wheel * 120);
	// Minecraft centres its cursor on a NEW screen only: the screen flag can drop for a frame or two while one stays open
	// (the user's "moving the mouse in the inventory recentres the cursor": bursts of recentres in their log), so a screen
	// counts as new only after it has been closed for 0.3 s
	static double s_screenSeenAt = -1e9;
	const double nowS = link::NowNs() * 1e-9;
	const bool newScreen = screen && !g_screenWas && nowS - s_screenSeenAt > 0.3;
	if (screen) s_screenSeenAt = nowS;
	if (newScreen) {
		static int s_recentres = 0;
		if (++s_recentres <= 40) console->Print("[bridge] cursor: recentred (screen opened or input resumed), %d so far, flags 0x%x\n", s_recentres, g_mc.flags);
		g_cursorX = vw / 2, g_cursorY = vh / 2, dx = dy = 0;
	}
	g_screenWas = screen;
	if (screen && (dx || dy)) {
		g_cursorX = std::max(0, std::min(vw - 1, g_cursorX + dx));
		g_cursorY = std::max(0, std::min(vh - 1, g_cursorY + dy));
		link::PushInput(kInCursor, 0, g_cursorX, g_cursorY);
	}
	for (; t != h; ++t) link::PushInput(kInText, 0, (int32_t)g_fText[t & 31].load());
	g_fTextTail = t;
	if (esc) link::PushInput(kInKey, (uint16_t)kScEscape, 1), link::PushInput(kInKey, (uint16_t)kScEscape, 0);
}

static void ReleaseAllKeys() {
	bool any = g_buttonsSent != 0;
	for (int i = 0; i < 512; ++i) any |= g_keysSent[i] != 0;
	std::memset(g_keysSent, 0, sizeof g_keysSent);
	g_buttonsSent = 0;
	if (any) link::PushInput(kInReleaseAll, 0, 0);
}

// G held (a shot fired while it is down is armed, clip.cpp); H, the way home; scripted holds of both (tests)
static bool g_armHeld = false;
static int g_homeKey = 11;  // SDL scancode (11 = H); 0 = off
static double g_simArmUntil = 0, g_simHomeUntil = 0;
bool McArmKeyHeld() { return g_armHeld; }
CON_COMMAND(pc_mc_home_key, "pc_mc_home_key [scancode|0] - the key held 0.5 s in Minecraft for pc_mc_home (SDL scancode, default 11 = H; 0 off)\n") {
	if (args.ArgC() > 1) g_homeKey = std::atoi(args[1]);
	console->Print("[bridge] mc home key: scancode %d\n", g_homeKey);
}
CON_COMMAND(pcb_key_hold, "pcb_key_hold <arm|home> <seconds> - (tests) the G / H key counts as held for that long, through the same polling as the real key\n") {
	if (args.ArgC() < 3) return;
	const double until = link::NowNs() * 1e-9 + std::atof(args[2]);
	if (!std::strcmp(args[1], "arm")) g_simArmUntil = until;
	else g_simHomeUntil = until;
}
// Injected by scripts (pcb_mckey): held on top of the physical keyboard.
static uint8_t g_scriptKeys[512];
static uint32_t g_scriptButtons = 0;

// ---- (bridge #32) right click as +use ---------------------------------------------------------------------------------
static int g_rmbUses = 0;
static float g_useReach = 96.0f;  // Portal 2's own use trace is shorter (80 u); this only decides whether to press
CON_COMMAND(pcb_use_reach, "pcb_use_reach [u] - (bridge #32) how far ahead of the eye a right click looks for a Portal 2 entity to +use (default 96)\n") {
	if (args.ArgC() > 1) g_useReach = (float)std::atof(args[1]);
	console->Print("[bridge] right click use: reach %.0f u, %d presses so far\n", g_useReach, g_rmbUses);
}
static std::string g_useTargetNow;
static Vector FeetRawSrc(bool body);
static bool UseTargetAhead() {
	struct NotPlayer : public ITraceFilter {
		void *skip = nullptr;
		virtual bool ShouldHitEntity(void *e, int) { return e != skip; }
		virtual TraceType_t GetTraceType() const { return TRACE_EVERYTHING; }
	} f;
	static void *player = nullptr;
	static double at = -1;
	if (link::NowNs() * 1e-9 - at > 1.0) {
		at = link::NowNs() * 1e-9;
		auto v = EntFind("player");
		player = v.empty() ? nullptr : v[0];
	}
	f.skip = player;
	const QAngle va = engine.ViewAngles();
	const float p = va.x * (float)M_PI / 180.0f, y = va.y * (float)M_PI / 180.0f;
	const Vector fwd(std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), -std::sin(p));
	const Vector from = FeetRawSrc(false) + Vector(0, 0, 1.62f * kUnitsPerBlock), to = from + fwd * g_useReach;
	Ray_t ray;
	ray.m_Delta = VectorAligned(to.x - from.x, to.y - from.y, to.z - from.z);
	ray.m_IsSwept = true, ray.m_IsRay = true;
	ray.m_Extents = VectorAligned(0, 0, 0);
	ray.m_StartOffset = VectorAligned(0, 0, 0);
	ray.m_Start = VectorAligned(from.x, from.y, from.z);
	ray.m_pWorldAxisTransform = nullptr;
	CGameTrace tr;
	std::memset(&tr, 0, sizeof tr);
	engine.TraceRay(engine.traceServer->ThisPtr(), ray, MASK_SOLID, &f, &tr);
	if (tr.fraction >= 1.0f || tr.startsolid || !tr.m_pEnt) return false;
	const std::string cls = EntKV(tr.m_pEnt, "classname");
	if (cls.empty() || cls == "worldspawn" || cls == "player" || cls.rfind("trigger_", 0) == 0 || cls.find("clip") != std::string::npos || cls.rfind("func_noportal", 0) == 0 ||
	    cls.rfind("prop_static", 0) == 0 || cls == "prop_portal" || cls.rfind("env_", 0) == 0)
		return false;
	g_useTargetNow = cls + " " + EntKV(tr.m_pEnt, "targetname");
	return true;
}
CON_COMMAND(pcb_assert_mc_use, "pcb_assert_mc_use <min presses> - (bridge #32) right clicks that were turned into a Portal 2 +use since the start of the run (last target in the log)\n") {
	const int mn = args.ArgC() > 1 ? std::atoi(args[1]) : 1;
	char buf[200];
	std::snprintf(buf, sizeof buf, "%d right click +use frames (want >= %d), last target: %s", g_rmbUses, mn, g_useTargetNow.empty() ? "-" : g_useTargetNow.c_str());
	void TestCheck(bool ok, const char *what, const char *detail);
	TestCheck(g_rmbUses >= mn, "right click as +use", buf);
}
static void PumpInput(bool forward) {
	ResolveSdl();
	if (!SDL_GetKeyboardState_) return;
	int n = 0;
	const uint8_t *ks = SDL_GetKeyboardState_(&n);
	bool focus = (!SDL_GetKeyboardFocus_ || SDL_GetKeyboardFocus_() != nullptr) && !g_inputLock.load();  // locked: script input only
	// The mode key works in both modes.
	bool t = ks && n > kScToggle && ks[kScToggle] && focus;
	// (not while a Minecraft screen is open: J is then a letter typed into chat or a sign)
	if (t && !g_toggleKeyDown && !(g_mode == Mode::Minecraft && (g_mc.flags & kMcScreenOpen))) engine.Cmd("bridge_mode toggle");
	g_toggleKeyDown = t;
	// pc_mc_portal's key (Chell only, not while the console or a menu is up; no bind is written)
	static bool armDown = false;
	const int ak = ClipArmKey();
	const bool menus = engine.Con_IsVisible(engine.client->ThisPtr()) || engine.IsPaused(engine.client->ThisPtr());
	const double now = link::NowNs() * 1e-9;
	bool a = ak > 0 && ((ks && n > ak && ks[ak] && focus) || now < g_simArmUntil) && g_mode == Mode::Chell && !menus;
	if (a && !armDown) engine.Cmd("pc_mc_portal");
	armDown = a;
	g_armHeld = a;
	// the way home (pc_mc_home): H held for half a second, in Minecraft (no bind is written)
	static double homeSince = 0;
	static bool homeFired = false;
	const bool h = g_homeKey > 0 && ((ks && n > g_homeKey && ks[g_homeKey] && focus) || now < g_simHomeUntil) && g_mode == Mode::Chell && !menus;
	if (!h) homeSince = 0, homeFired = false;
	else if (homeSince == 0) homeSince = now;
	else if (!homeFired && now - homeSince >= 0.5) homeFired = true, engine.Cmd("pc_mc_home");

	// (bridge #47) G as the Minecraft player with nothing to use under the crosshair: the portal onto Minecraft. The Minecraft player has no portal gun
	// (Portal 2's gun is Chell's), so this is the same as pressing J and then G: the body becomes Chell, with the Minecraft portal armed (the next portal
	// she fires opens onto the Minecraft stage; J goes back to the Minecraft player, who can walk through it as well). A press with something to use
	// (a button, a cube) stays Portal 2's use, as before.
	{
		static bool gPrev = false;
		const bool gNow = ks && n > kScUse && ks[kScUse] && focus;
		if (forward && gNow && !gPrev && g_mode == Mode::Minecraft && !(g_mc.flags & kMcScreenOpen) && !menus && ClipArmKey() > 0 && !UseTargetAhead()) {
			console->Print("[bridge] mc portal: G as the Minecraft player with nothing to use: the body becomes Chell with the portal onto Minecraft armed (J goes back)\n");
			engine.Cmd("bridge_mode chell");
			engine.Cmd("pc_mc_portal 1");
		}
		gPrev = gNow;
	}
	// G as the Minecraft player: Portal 2's use (buttons, cubes), SkyCraft's dedicated activate key (DESIGN-mcplayer 4's
	// fallback; the nearest-target arbitration on right click is later work). Not while a Minecraft screen is open.
	g_useHeld = forward && g_mode == Mode::Minecraft && !(g_mc.flags & kMcScreenOpen) && ((ks && n > kScUse && ks[kScUse] && focus) || g_scriptKeys[kScUse]);
	// (bridge #32) a right click aimed at a Portal 2 entity within use reach is Portal 2's +use as well (a pedestal button, a cube to pick up,
	// a door's button: whatever the player can use). Minecraft's own right click goes on as it is; on a wall (no entity) nothing is used.
	if (forward && g_mode == Mode::Minecraft && !(g_mc.flags & kMcScreenOpen) && !g_useHeld) {
		const uint32_t mb = (focus && SDL_GetMouseState_ ? SDL_GetMouseState_(nullptr, nullptr) : 0) | g_scriptButtons;
		if ((mb & 4) && UseTargetAhead()) g_useHeld = true, ++g_rmbUses;
	}
	// O as the Minecraft player: Minecraft's pause / options menu (Esc is Portal 2's), as SkyCraft's O (its Input.cpp): the key is not passed
	// on (Minecraft's own O is its Friends screen) and the mod opens the menu on kInOpenMenu. With a Minecraft screen up, O is a key like any
	// other (typing in chat). (2026-10-12)
	constexpr int kScMenu = 18;  // SDL_SCANCODE_O
	const bool screenUp = (g_mc.flags & kMcScreenOpen) != 0;
	{
		static bool oPrev = false;
		const bool oNow = focus && ks && n > kScMenu && ks[kScMenu];
		if (forward && oNow && !oPrev && g_mode == Mode::Minecraft && !screenUp && !menus) {
			ReleaseAllKeys();
			link::PushInput(kInOpenMenu, 0, 0);
			console->Print("[bridge] mc player: O opens Minecraft's menu\n");
		}
		oPrev = oNow;
	}
	if (!forward) {
		ReleaseAllKeys();
		return;
	}
	for (int sc = 1; sc < 512; ++sc) {
		if (sc == kScToggle || sc == kScGrave || sc == kScEscape || sc == kScUse || sc == 68 /* F11: Minecraft's hidden window must not go fullscreen */) continue;
		if (sc == kScMenu && !screenUp && !g_keysSent[sc]) continue;  // (O: the menu above, not Minecraft's Friends screen)
		uint8_t down = ((focus && ks && sc < n && ks[sc]) || g_scriptKeys[sc]) ? 1 : 0;
		if (down != g_keysSent[sc]) {
			g_keysSent[sc] = down;
			link::PushInput(kInKey, (uint16_t)sc, down);
		}
	}
	uint32_t buttons = (focus && SDL_GetMouseState_ ? SDL_GetMouseState_(nullptr, nullptr) : 0) | g_scriptButtons;
	for (int b = 1; b <= 5; ++b) {
		uint32_t bit = 1u << (b - 1);  // SDL_BUTTON(b)
		if ((buttons & bit) != (g_buttonsSent & bit)) link::PushInput(kInMouseButton, (uint16_t)b, (buttons & bit) ? 1 : 0);
	}
	g_buttonsSent = buttons;
}

// ---- interpolation of Minecraft's ticks ----------------------------------------------------
static double Alpha() {
	double tickNs = (g_mc.tickMs > 0 ? g_mc.tickMs : 50.0) * 1e6;
	double a = (double)(link::NowNs() - g_mc.tickQpc) / tickNs;
	return a < 0 ? 0 : a > 1.5 ? 1.5 : a;
}
// (bridge #26) the mover Minecraft's player rode in its latest tick (McState; movers.cpp has its pose now)
bool MoverAnchorMc(uint32_t id, double &x, double &y, double &z);  // movers.cpp
bool MoverRenderAnchorMc(uint32_t id, double &x, double &y, double &z);  // movers.cpp: as Portal 2 draws it this frame
bool MoverLiveAnchorMc(uint32_t id, double &x, double &y, double &z);  // movers.cpp (bridge #40): the entity's origin right now
static bool OnMover() { return g_mc.rideMover != 0; }
static Vector FeetRawSrc(bool body) {
	// (bridge #23/#26) riding a mover: the feet relative to it, interpolated as Minecraft simulated them, put on the mover
	// where it is now (Minecraft lags the host's pose by up to a tick: 10-20 u at an elevator's speed, the floor seen moving
	// under the camera). The Unity character controller's rule: platform and character interpolated consistently.
	double px, py, pz;
	// (bridge #40) the BODY (the puppet) rides the mover where the SERVER has it now: Portal 2's player is pushed by the server's car. The camera rides the
	// mover as Portal 2 draws it (cl_interp behind), which put the body 30 u under a car rising at 300 u/s (the 23..33 u jump at the switch; the pusher
	// then shoved it up again every tick)
	if (OnMover() && (body && !g_legacyBody ? MoverLiveAnchorMc(g_mc.rideMover, px, py, pz) : MoverRenderAnchorMc(g_mc.rideMover, px, py, pz))) {
		const double a = Alpha();
		const double lx0 = g_mc.prevX - g_mc.ridePrevX, ly0 = g_mc.prevY - g_mc.ridePrevY, lz0 = g_mc.prevZ - g_mc.ridePrevZ;
		const double lx1 = g_mc.curX - g_mc.rideCurX, ly1 = g_mc.curY - g_mc.rideCurY, lz1 = g_mc.curZ - g_mc.rideCurZ;
		if ((lx1 - lx0) * (lx1 - lx0) + (ly1 - ly0) * (ly1 - ly0) + (lz1 - lz0) * (lz1 - lz0) < 16.0)
			return McToSrc(px + lx0 + (lx1 - lx0) * a, py + ly0 + (ly1 - ly0) * a, pz + lz0 + (lz1 - lz0) * a);
	}
	// (bridge #20) a tick that spans a teleport (a portal transit followed, > 4 blocks): no sweep from the old spot
	// through the walls between (a frame of Portal 2's void: white), the new spot at once
	const double tx = g_mc.curX - g_mc.prevX, ty = g_mc.curY - g_mc.prevY, tz = g_mc.curZ - g_mc.prevZ;
	if (tx * tx + ty * ty + tz * tz > 16.0) return McToSrc(g_mc.curX, g_mc.curY, g_mc.curZ);
	double a = Alpha();
	return McToSrc(g_mc.prevX + (g_mc.curX - g_mc.prevX) * a, g_mc.prevY + (g_mc.curY - g_mc.prevY) * a, g_mc.prevZ + (g_mc.curZ - g_mc.prevZ) * a);
}
// (bridge #28) the body where it is drawn: behind a portal's plane on the latest tick's line (the tick of a transit, still behind
// the exit; an extrapolated frame past the entry) it is on the other side of the pair (mctransit.cpp)
Vector McTransitFoldFeet(const Vector &feet, const Vector &prev, const Vector &cur, Vector *vel);  // mctransit.cpp
Vector McTransitHoldBody(const Vector &feet);  // mctransit.cpp: the puppet only
static Vector FeetSrc(Vector *vel = nullptr, bool body = false) {
	return McTransitFoldFeet(FeetRawSrc(body), McToSrc(g_mc.prevX, g_mc.prevY, g_mc.prevZ), McToSrc(g_mc.curX, g_mc.curY, g_mc.curZ), vel);
}
static Vector VelocitySrc() {
	double s = 1000.0 / (g_mc.tickMs > 0 ? g_mc.tickMs : 50.0);  // blocks/tick -> blocks/s
	// a direction, not a point: MC (x, y, z) -> Source (x, -z, y) x 40, no mapping offset (McToSrc added the region
	// offset: 13120 16960 -17728 u/s, bridge #13); a tick that spans a teleport (> 4 blocks) gives no velocity
	const double dx = g_mc.curX - g_mc.prevX, dy = g_mc.curY - g_mc.prevY, dz = g_mc.curZ - g_mc.prevZ;
	if (dx * dx + dy * dy + dz * dz > 16.0) return Vector(0, 0, 0);
	return Vector((float)(dx * s * kUnitsPerBlock), (float)(-dz * s * kUnitsPerBlock), (float)(dy * s * kUnitsPerBlock));
}

void ChellProxyFrame();  // chellproxy.cpp
bool ChellProxyInChambers();  // chellproxy.cpp (bridge #19): the hidden player follows Chell in the chambers too
void AudioFrame(bool paused);  // audio.cpp
void McLightFrame(const Vector &eye);  // mclight.cpp
void FloatOriginViewFrame(const Vector &eye);  // floatorigin.cpp
bool ClipRegionContains(const Vector &p);
static bool ClipRegionContainsMc(const Vector &src) { return ClipRegionContains(src); }
void GlHudSetState(bool show, bool preload, bool cursorOn, int cursorX, int cursorY);  // glhud.cpp
void GlHudHideNow();  // glhud.cpp (bridge #56)
void HudGateLevelInit();  // hudgate.cpp (bridge #56)
uint32_t HudGateReadHide();
void HudGateOurHide(uint32_t before, uint32_t after);
void HudGateOurHideClear();
bool HudGateP2Shows(const char *&why);
void HudGateProbe(char *out, size_t n);  // hudgate.cpp (bridge #63)
void InputWatchInstall();  // ents.cpp (bridge #63)
void InputWatchLevelInit();
bool FadeNow(float &alpha, float rgb[3]);
void GlHudSetFade(float alpha, float r, float g, float b);  // glhud.cpp
bool HudGateScripted(bool &cam, bool &certain, const char *&why);  // hudgate.cpp (bridge #63)
bool CombatHudHidden();  // combat.cpp (bridge #19): no Minecraft HUD (its respawn's "Loading terrain") during Portal 2's death
bool GlHudDrawn();
bool GlHudReady();
static double g_wantSince = 0;
bool GlHudEnabled();
bool GlHudInit();
void GlHudShutdown();
static bool McActive() { return g_mode == Mode::Minecraft && g_haveMc && (g_mc.flags & kMcInWorld) && link::McAlive(); }

bool McModeActive() { return McActive(); }
bool McModeHeld() { return g_mode == Mode::Minecraft; }  // (bridge #55) the Minecraft player is the player, in the world or not yet (level loads, link gaps): the body's Portal 2 health stays held
bool McOnGround() { return (g_mc.flags & kMcOnGround) != 0; }
Vector McFeetSrc() { return FeetSrc(nullptr, true); }  // (bridge #40) where the player is in the server's frame (the body's), not on the car as it is DRAWN: digging, audio, mechanics, tests           // avatar.cpp (bridge #16)
Vector McFeetRawSrc() { return FeetRawSrc(false); }     // (bridge #28) avatar.cpp: where Minecraft's player is, not folded through a portal
const McState &McStateNow() { return g_mc; }        // avatar.cpp (bridge #16)
uint32_t AvatarHostFlags();                         // avatar.cpp: the body (kHpWantAvatar, kHpAvatarInMc)
void AvatarCameraFrame(CViewSetup *view);           // avatar.cpp: F5 third person
bool McSwitchPending() { return g_wantMc && g_mode == Mode::Chell; }

// ---- Minecraft-player mode in terrain sessions (bridge #13, DESIGN-mcplayer M1) ------------------
// The switch: Minecraft's player is teleported to Chell's feet (chamber band or region) with her velocity
// (kInVelocity, applied when the mod's hold releases), and the mode flips once Minecraft acks that teleport.
// (bridge #28) A portal transit is Minecraft's own (the mod's PlayerTransit; mctransit.cpp is the host's side): the Minecraft
// player walks into the carved hole (kInHole for Minecraft's terrain, the CollisionField export skips hole voxels in the chamber
// band) and goes through the tick its eye crosses the plane; the body follows it, no hand-over.
Vector PlayerOrigin();
Vector PlayerVelocity();
static bool g_awaitTeleport = false;
static void SendVelocity(const Vector &v) {
	// Source u/s -> MC blocks/tick: MC (x, y, z) = Source (x, z, -y) / 40, 20 ticks/s
	const double k = 10000.0 / (kUnitsPerBlock * 20.0);
	link::PushInput(kInVelocity, 0, (int32_t)std::lround(v.x * k), (int32_t)std::lround(v.z * k), (int32_t)std::lround(-v.y * k));
}
// Portal 2 holds the body until Minecraft has been sent to it (combat.cpp: the respawn hand-over; no longer portal transits)
void McPuppetTransit(const Vector &origin, const Vector &vel) {
	if (!McActive()) return;
	g_awaitTeleport = true;
	++g_teleportSeq;
	SendVelocity(vel);
	console->Print("[bridge] mc player: body handed to Portal 2 at (%.1f %.1f %.1f) velocity (%.0f %.0f %.0f): Minecraft follows (teleport %u)\n", origin.x, origin.y, origin.z, vel.x,
	               vel.y, vel.z, g_teleportSeq);
}

// Chell's body and Portal 2's HUD while the Minecraft player is the player: her model is not drawn (the entity's
// DisableDraw input: EF_NODRAW, networked, so her shadow and portal-view ghost go too) and Portal 2's HUD is hidden
// (the player's SetHUDVisibility input: crosshair, portal indicator); the viewmodel goes with r_drawviewmodel 0.
// Inputs, not raw field writes: a raw write is not flagged as changed and may never reach the client. No cvar is
// touched, nothing is archived. Restored on the way back. A raw-field fallback reports what the inputs left.
int EntFieldOffset(void *ent, const char *field);  // ents.cpp
static bool g_hidden = false;
static bool g_everHid = false;          // the hide ran in this session (a flag may ride through a level transition)
static void HideChell(bool hide) {
	if (hide == g_hidden) return;
	auto v = EntFind("player");
	if (v.empty()) return;
	const uint32_t hudBefore = HudGateReadHide();  // (bridge #56: which HIDEHUD bits are ours, so Portal 2's own hiding can be told apart)
	const bool a = EntInput(v[0], hide ? "DisableDraw" : "EnableDraw");
	const bool b = EntInput(v[0], "SetHUDVisibility", hide ? "0" : "1");
	if (hide) HudGateOurHide(hudBefore, HudGateReadHide());
	else HudGateOurHideClear();
	g_hidden = hide;
	if (hide) g_everHid = true;
	for (void *w : EntFind("weapon_portalgun")) EntInput(w, hide ? "DisableDraw" : "EnableDraw");  // (bridge #56b) the gun's world model too
	static int offFx = -2;
	if (offFx == -2) offFx = EntFieldOffset(v[0], "m_fEffects");
	console->Print("[bridge] mc player: Chell %s (inputs %d %d, effects 0x%x)\n", hide ? "hidden, Portal 2 HUD off" : "shown, Portal 2 HUD on", a, b,
	               offFx >= 0 ? *(int *)((char *)v[0] + offFx) : -1);
}

// (bridge #56b) A death, a reload or a respawn gives Chell a fresh body / weapon whose render state is the default again: the gun's world model (and the
// body) drew in front of Minecraft's low sneaking camera. While Minecraft is the player, four times a second: the player and every portal gun must
// carry EF_NODRAW, else the hide is applied again.
static void EnforceBodyHidden() {
	static double next = 0;
	const double now = link::NowNs() * 1e-9;
	if (!g_hidden || now < next) return;
	next = now + 0.25;
	bool bad = false;
	auto pl = EntFind("player");
	if (pl.empty()) return;
	const int o = EntFieldOffset(pl[0], "m_fEffects");
	if (o < 0) return;
	if (!(*(int *)((char *)pl[0] + o) & 32)) bad = true;  // EF_NODRAW
	for (void *w : EntFind("weapon_portalgun")) {
		const int ow = EntFieldOffset(w, "m_fEffects");
		if (ow >= 0 && !(*(int *)((char *)w + ow) & 32)) bad = true;
	}
	if (!bad) return;
	console->Print("[bridge] mc player: Chell's body or gun drawn again (respawn / reload): hidden again\n");
	g_hidden = false;
	HideChell(true);
}

// (bridge #61) Leaving Minecraft mode, and a level change while Chell is the player, restore the draw state by what exists NOW, not by what was
// hidden: the player, every portal gun and the viewmodel entities are checked for EF_NODRAW and given EnableDraw when they carry it. A level change
// makes a new player / gun / viewmodel (or carries the old flag through the transition), so a list remembered from the hide could be stale or empty,
// and a weapon (re)deployed by the map's own logic can come back hidden. Runs for a few seconds after the switch / the level start (a map's own logic
// may hold the gun back at its start: nothing is forced after the window), four times a second, and logs the gun's state once at the end.
static double g_restoreUntil = 0, g_restoreNext = 0;
static bool g_restoreReport = false;
static void RestoreChellDraw() {
	auto clear = [](void *e) {
		const int o = EntFieldOffset(e, "m_fEffects");
		if (o < 0 || !(*(int *)((char *)e + o) & 32)) return false;
		EntInput(e, "EnableDraw");
		return true;
	};
	int fixedPlayer = 0, fixedGun = 0, fixedVm = 0;
	for (void *e : EntFind("player")) fixedPlayer += clear(e);
	for (void *e : EntFind("weapon_portalgun")) fixedGun += clear(e);
	for (const char *cls : {"predicted_viewmodel", "viewmodel"})
		for (void *e : EntFind(cls)) fixedVm += clear(e);
	if (fixedPlayer || fixedGun || fixedVm) console->Print("[bridge] mc player: Chell mode, EF_NODRAW still set on %d player, %d portal gun, %d viewmodel entities: drawn again\n", fixedPlayer, fixedGun, fixedVm);
}
static void ChellGunReport() {
	void *pl = nullptr;
	for (void *e : EntFind("player")) pl = e;
	auto guns = EntFind("weapon_portalgun");
	int active = -1, drawVm = -1;
	if (pl) {
		const int oa = EntFieldOffset(pl, "m_hActiveWeapon"), od = EntFieldOffset(pl, "m_bDrawViewmodel");
		if (oa >= 0) active = (*(unsigned *)((char *)pl + oa) & 0xFFFFFFu) == 0xFFFFFFu ? 0 : 1;
		if (od >= 0) drawVm = *((unsigned char *)pl + od);
	}
	std::string g;
	for (void *w : guns) {
		const int o1 = EntFieldOffset(w, "m_bCanFirePortal1"), o2 = EntFieldOffset(w, "m_bCanFirePortal2"), of = EntFieldOffset(w, "m_fEffects");
		char b[96];
		std::snprintf(b, sizeof b, " [fire1 %d fire2 %d effects 0x%x]", o1 >= 0 ? *((unsigned char *)w + o1) : -1, o2 >= 0 ? *((unsigned char *)w + o2) : -1, of >= 0 ? *(int *)((char *)w + of) : -1);
		g += b;
	}
	ConVar *cv = (ConVar *)tier1->FindCommandBase(tier1->g_pCVar->ThisPtr(), "r_drawviewmodel");
	console->Print("[bridge] mc player: Chell's gun: %zu portal gun entities%s; active weapon %s, m_bDrawViewmodel %d, r_drawviewmodel %s\n", guns.size(), g.c_str(), active < 0 ? "?" : active ? "set" : "NONE", drawVm,
	               cv ? ((cv->m_pParent ? cv->m_pParent : cv)->m_nValue ? "on" : "OFF") : "?");
}
static void EnforceChellShown() {
	const double now = link::NowNs() * 1e-9;
	if (g_restoreUntil <= 0 || now < g_restoreNext) return;
	g_restoreNext = now + 0.25;
	RestoreChellDraw();
	if (now > g_restoreUntil) {
		g_restoreUntil = 0;
		if (g_restoreReport) g_restoreReport = false, ChellGunReport();
	}
}

// The open portals' hole volumes, carved out of Minecraft's own block collision for its player (the mod's
// BlockCollisionsMixin), so it can walk into a portal on Minecraft blocks (the cliff portal) as Chell does through
// the block hook. Sent when they change while the Minecraft player leads (or is about to); cleared otherwise.
bool PortalHoleBox(int i, Vector &mn, Vector &mx);  // blockcollide.cpp
static void SendHoles(bool on) {
	// (bridge #32) every portal slot (slot = group * 2 + colour): the code's bit 0 is the colour, bits 8..15 the linkage group index
	static Vector sentMn[kPortalSlots], sentMx[kPortalSlots];
	static bool sentOn[kPortalSlots] = {};
	for (int i = 0; i < kPortalSlots; ++i) {
		Vector mn, mx;
		const bool live = on && PortalHoleBox(i, mn, mx);
		if (live == sentOn[i] && (!live || ((mn - sentMn[i]).Length() < 0.5f && (mx - sentMx[i]).Length() < 0.5f))) continue;
		sentOn[i] = live, sentMn[i] = mn, sentMx[i] = mx;
		const uint16_t id = (uint16_t)((i & 1) | ((i >> 1) << 8));
		if (!live) {
			link::PushInput(kInHole, (uint16_t)(id | 4), 0, 0, 0);
			continue;
		}
		const Vector a = SrcToMc(mn), b = SrcToMc(mx);
		console->Print("[bridge] mc player: portal %d hole MC (%.2f %.2f %.2f)..(%.2f %.2f %.2f) sent\n", i, std::fmin(a.x, b.x), std::fmin(a.y, b.y), std::fmin(a.z, b.z), std::fmax(a.x, b.x),
		               std::fmax(a.y, b.y), std::fmax(a.z, b.z));
		link::PushInput(kInHole, id, (int32_t)std::lround(std::fmin(a.x, b.x) * 1000), (int32_t)std::lround(std::fmin(a.y, b.y) * 1000), (int32_t)std::lround(std::fmin(a.z, b.z) * 1000));
		link::PushInput(kInHole, (uint16_t)(id | 2), (int32_t)std::lround(std::fmax(a.x, b.x) * 1000), (int32_t)std::lround(std::fmax(a.y, b.y) * 1000), (int32_t)std::lround(std::fmax(a.z, b.z) * 1000));
	}
}

// ---- the linked pair for Minecraft's mobs and items (bridge #22) ------------------------------------------------------
// Both portals' poses in Minecraft coordinates (kInPortal), whenever both are open: the mod carries mobs, dropped items and
// projectiles through the pair (EntityTransit) and carves the blocks behind each opening out of their collision. Resent
// every second (a restarted Minecraft gets it again) and at once on a change.
static bool g_mobTransit = true;  // pc_mc_mob_transit
static void SendPortalPair(bool on) {
	// (bridge #32) every linked pair (kPortalSlots / 2 linkage groups): the code's bit 0 is the colour, bits 1..2 the part, bit 4 = clear that
	// group's pair, bits 8..15 the linkage group index (0 = the gun's pair, what older hosts sent)
	static bool sentOn[kPortalSlots / 2] = {};
	static Vector sentPos[kPortalSlots], sentN[kPortalSlots], sentU[kPortalSlots];
	static float sentHalf[kPortalSlots][2] = {};
	static double sentAt[kPortalSlots / 2] = {};
	const double now = link::NowNs() * 1e-9;
	for (int g = 0; g < kPortalSlots / 2; ++g) {
		const bool live = on && PortalLinked(2 * g);
		bool changed = live != sentOn[g];
		for (int i = 2 * g; live && i < 2 * g + 2; ++i)
			changed = changed || std::fabs(g_portalHalfW[i] - sentHalf[i][0]) > 0.01f || std::fabs(g_portalHalfH[i] - sentHalf[i][1]) > 0.01f || (g_portalPos[i] - sentPos[i]).Length() > 0.25f || (g_portalNormal[i] - sentN[i]).Length() > 1e-3f || (g_portalUp[i] - sentU[i]).Length() > 1e-3f;
		if (!changed && now - sentAt[g] < 1.0) continue;
		sentAt[g] = now;
		const uint16_t grp = (uint16_t)(g << 8);
		if (!live) {
			if (sentOn[g] || changed) link::PushInput(kInPortal, (uint16_t)(0x10 | grp), 0, 0, 0);
			sentOn[g] = false;
			continue;
		}
		for (int i = 2 * g; i < 2 * g + 2; ++i) {
			double x, y, z;
			SrcToMcD(g_portalPos[i], x, y, z);
			const Vector &n = g_portalNormal[i], &u = g_portalUp[i];
			const int c = i & 1;
			// a direction: MC (x, y, z) = Source (x, z, -y)
			link::PushInput(kInPortal, (uint16_t)(c | 0 << 1 | grp), (int32_t)std::lround(x * 1000), (int32_t)std::lround(y * 1000), (int32_t)std::lround(z * 1000));
			link::PushInput(kInPortal, (uint16_t)(c | 1 << 1 | grp), (int32_t)std::lround(n.x * 1e6), (int32_t)std::lround(n.z * 1e6), (int32_t)std::lround(-n.y * 1e6));
			link::PushInput(kInPortal, (uint16_t)(c | 2 << 1 | grp), (int32_t)std::lround(u.x * 1e6), (int32_t)std::lround(u.z * 1e6), (int32_t)std::lround(-u.y * 1e6));
			// (bridge #68) part 3: the opening's half size in thousandths of a block (a linked_portal_door is not the gun's 64 x 112)
			link::PushInput(kInPortal, (uint16_t)(c | 3 << 1 | grp), (int32_t)std::lround(g_portalHalfW[i] / kUnitsPerBlock * 1000), (int32_t)std::lround(g_portalHalfH[i] / kUnitsPerBlock * 1000), 0);
			sentHalf[i][0] = g_portalHalfW[i], sentHalf[i][1] = g_portalHalfH[i];
			if (changed) console->Print("[bridge] portal pair for Minecraft's mobs: portal %d (group %d) at MC (%.2f %.2f %.2f) normal (%.2f %.2f %.2f)\n", i, g, x, y, z, n.x, n.z, -n.y);
			sentPos[i] = g_portalPos[i], sentN[i] = n, sentU[i] = u;
		}
		sentOn[g] = true;
	}
}
CON_COMMAND(pc_mc_mob_transit, "pc_mc_mob_transit [0|1] - Minecraft's mobs, items and projectiles go through the portal pair (default 1)\n") {
	if (args.ArgC() > 1) g_mobTransit = std::atoi(args[1]) != 0;
	console->Print("[bridge] mob transit %d\n", g_mobTransit);
}

// ---- (bridge #26) movers: Minecraft collides with, rides and is pushed by every moving solid itself ------------------
// Portal 2's movers (doors, lifts, elevators, arm panels, cubes...) are posed in Minecraft every frame (movers.cpp,
// SkyMovers in the mod): no Portal 2 takeover while one moves under or into the body. Portal 2 carries the body only
// after a fall guard catch (a bug: it logs as one) until Minecraft has the floor again.
bool CollisionFieldDynPending();  // collfield.cpp
static bool g_takeover = false;
static double g_lastPushAt = 0;
static int g_takeovers = 0;
const char *CollisionFieldLastMovingUnder();
size_t CollisionFieldDynPendingCount();
bool CollisionFieldFeetReady(const Vector &feetSrc);  // collfield.cpp
bool CollisionFieldFeetPending(const Vector &feetSrc);  // collfield.cpp
const char *CollisionFieldFeetWhy();
void CollisionFieldBoost(bool on);  // collfield.cpp
bool CollisionFieldFloorCrossed(const Vector &a, const Vector &b, double ageA, double ageB, Vector &at, std::string &what);
bool CollisionFieldSentFloorUnder(const Vector &feet);
bool MoversReadyNear(const Vector &p, float r, char *why, size_t n);  // movers.cpp
bool MoverFloorUnder(const Vector &feet, std::string *what);
bool MoverUprightYaw(uint32_t id, float &yaw);
std::string MoverName(uint32_t id);
void *MoverFind(const char *spec, uint32_t *idOut);
void MoversSetUrgent(bool on);
static void RideSample();  // below
static void McmFrame();  // below (bridge #31)
static double s_carrySince = 0;
// ---- (bridge #25) the fall guard: Minecraft never falls through a Portal 2 floor ---------------------------------------
// Per Minecraft tick, its feet's path is traced in Portal 2 against what Minecraft should hold (the static field and the
// movers). A fall through a floor is a bug (the static field late or wrong, a mover missing): it logs as one, puts the body
// back on top, and Portal 2 carries it until Minecraft has that floor. The last-resort net; tests assert 0 catches.
static Vector g_catchPos, g_catchVel;
static double g_catchUntil = 0, g_catchAt = 0;
static int g_catches = 0, g_catchLogs = 0;
static uint64_t g_guardTick = 0;
static bool g_guardOn = true;  // pc_mc_fall_guard
static float g_guardLowZ = 1e9f;  // (tests) the lowest feet z while the guard watched
static void FallGuardReset() { g_catchUntil = 0, g_catchAt = 0, g_guardTick = 0; }
static void FallGuardCatch(const Vector &at, const char *why, const std::string &what) {
	const double now = link::NowNs() * 1e-9;
	g_catchPos = at, g_catchVel = Vector(0, 0, 0), g_catchUntil = now + 0.08, g_catchAt = now;
	g_lastPushAt = now;
	++g_catches;
	if (!g_takeover) {
		g_takeover = true, g_awaitTeleport = true, ++g_takeovers;
		s_carrySince = now;
	}
	if (g_catchLogs++ < 40)
		console->Print("[bridge] BUG: fall guard: %s (%s): body to (%.1f %.1f %.1f), Portal 2 carries it until Minecraft has the floor; Minecraft feet (%.2f %.2f %.2f), riding %s\n", why,
		               what.c_str(), at.x, at.y, at.z, g_mc.curX, g_mc.curY, g_mc.curZ, MoverName(g_mc.rideMover).c_str());
}
// (bridge #34) the support guard: what the standing player is on (movers.cpp MoverSupportSample), and the catch when it is found under it
bool MoverSupportSample(const Vector &feet, uint32_t &id, Vector &local);
bool MoverSupportCheck(uint32_t id, const Vector &local, const Vector &feetNow, Vector &expected, std::string &name);
static uint32_t g_supId = 0;
static Vector g_supLocal;
static double g_supAt = 0;
static int g_supCatches = 0;
static void FallGuardFrame() {
	if (!g_guardOn || !McActive() || !g_levelLive || g_awaitTeleport || g_takeover) {
		g_supId = 0;  // (a hand-over, a teleport, a carry: whatever it stood on is not the reference any more)
		return;
	}
	if ((uint64_t)g_mc.tickQpc == g_guardTick) return;
	const bool first = g_guardTick == 0;
	g_guardTick = (uint64_t)g_mc.tickQpc;
	if (first) return;
	const Vector a = McToSrc(g_mc.prevX, g_mc.prevY, g_mc.prevZ), b = McToSrc(g_mc.curX, g_mc.curY, g_mc.curZ);
	g_guardLowZ = std::min(g_guardLowZ, b.z);
	Vector at;
	std::string what;
	const double tickS = (g_mc.tickMs > 0 ? g_mc.tickMs : 50.0) * 1e-3, ageB = (double)(link::NowNs() - g_mc.tickQpc) * 1e-9;
	if (CollisionFieldFloorCrossed(a, b, ageB + tickS, ageB, at, what)) return FallGuardCatch(at + Vector(0, 0, 1), "Minecraft fell through a Portal 2 floor", what);
	// (bridge #34) found below the floor it was standing on
	const double now = link::NowNs() * 1e-9;
	if (McOnGround()) {
		uint32_t id = 0;
		Vector local;
		if (MoverSupportSample(b, id, local)) g_supId = id, g_supLocal = local, g_supAt = now;
		else g_supId = 0;
	} else if (g_supId && now - g_supAt < 2.0) {
		Vector expected;
		std::string name;
		if (MoverSupportCheck(g_supId, g_supLocal, b, expected, name)) {
			++g_supCatches;
			g_supId = 0;
			FallGuardCatch(expected + Vector(0, 0, 1), "Minecraft was found below the floor it was standing on", name);
		}
	} else if (g_supId) {
		g_supId = 0;
	}
}
CON_COMMAND(pc_mc_fall_guard, "pc_mc_fall_guard [0|1] - (bridge #25) catch the Minecraft player falling through a Portal 2 floor (default 1); stats\n") {
	if (args.ArgC() > 1) g_guardOn = std::atoi(args[1]) != 0;
	console->Print("[bridge] fall guard %d: %d catches (%d of them \"found below the floor it stood on\"), lowest feet z %.1f\n", g_guardOn, g_catches, g_supCatches, g_guardLowZ);
}
CON_COMMAND(pcb_assert_mc_feet_above, "pcb_assert_mc_feet_above <min z> [max catches] | reset - (bridge #25) the Minecraft player's feet stayed above z since the reset\n") {
	static int base = 0;
	if (args.ArgC() > 1 && args[1][0] == 'r') return void((g_guardLowZ = 1e9f, base = g_catches));
	const float mn = args.ArgC() > 1 ? (float)std::atof(args[1]) : -1e9f;
	const int mxc = args.ArgC() > 2 ? std::atoi(args[2]) : 1000;
	const Vector f = FeetSrc();
	const float low = std::min(g_guardLowZ, f.z);
	char buf[200];
	std::snprintf(buf, sizeof buf, "lowest feet z %.1f (want >= %.1f), now (%.1f %.1f %.1f), %d fall guard catches (want <= %d)", low, mn, f.x, f.y, f.z, g_catches - base, mxc);
	void TestCheck(bool ok, const char *what, const char *detail);
	TestCheck(low >= mn && g_catches - base <= mxc, "the Minecraft player did not fall through", buf);
}
static bool g_placeArmed = false;  // (bridge #31) pcb_mc_place
static Vector g_placeTarget;
static float g_placeFall = 8.0f;
static int g_standTeleportIn = 0;  // (bridge #26) pcb_stand_on as the Minecraft player: frames until Minecraft is sent to the body
static Vector g_standTarget;
// (bridge #25, tests) stand on top of an entity (a lift panel): setpos onto the first floor a hull meets coming down through its
// box's middle. Chell mode (sv_cheats 1); in Minecraft mode switch to Chell first, then back (waituntil mcleads).
// (bridge #26) the entity may be a mover by "#id" or "@x,y,z" (the mover nearest that point) as well as a targetname.
CON_COMMAND(pcb_stand_on, "pcb_stand_on <targetname|#moverid|@x,y,z> [dx dy] - (tests, bridge #25) setpos onto the top of an entity (its box's middle + dx dy)\n") {
	if (args.ArgC() < 2) return console->Print("%s", pcb_stand_on.ThisPtr()->m_pszHelpString);
	void TestCheck(bool ok, const char *what, const char *detail);
	void *e = nullptr;
	if (args[1][0] == '#' || args[1][0] == '@') e = MoverFind(args[1], nullptr);
	else {
		std::vector<void *> v = EntFind(nullptr, args[1]);
		if (!v.empty()) e = v.front();
	}
	if (!e) return TestCheck(false, "stand on", (std::string("no entity ") + args[1]).c_str());
	bool MoverInvisible(void *e);  // movers.cpp
	if (MoverInvisible(e)) return console->Print("[bridge] stand on: %s is an invisible blocker (only its floor over nothing is a mover): skipped\n", args[1]);
	const int offA = EntFieldOffset(e, "m_vecAbsOrigin"), offMin = EntFieldOffset(e, "m_vecMins"), offMax = EntFieldOffset(e, "m_vecMaxs");
	if (offA < 0 || offMin < 0 || offMax < 0) return TestCheck(false, "stand on", "no bounds");
	const Vector o = *(const Vector *)((const char *)e + offA), mn = *(const Vector *)((const char *)e + offMin), mx = *(const Vector *)((const char *)e + offMax);
	const float dx = args.ArgC() > 3 ? (float)std::atof(args[2]) : 0.0f, dy = args.ArgC() > 3 ? (float)std::atof(args[3]) : 0.0f;
	// (the box is in the entity's frame: its middle turned by the entity's yaw)
	const int offAng = EntFieldOffset(e, "m_angAbsRotation");
	const float yaw = offAng >= 0 ? ((const float *)((const char *)e + offAng))[1] * 3.14159265f / 180.0f : 0.0f;
	const float lx = (mn.x + mx.x) * 0.5f, ly = (mn.y + mx.y) * 0.5f;
	Vector c(o.x + lx * std::cos(yaw) - ly * std::sin(yaw) + dx, o.y + lx * std::sin(yaw) + ly * std::cos(yaw) + dy, 0);
	// (bridge #31) "@x,y,z": stand exactly there (x y), off the pivot of a turntable or the middle of a long panel, not at the box's middle
	if (args[1][0] == '@') {
		float px = 0, py = 0, pz = 0;
		if (std::sscanf(args[1] + 1, "%f,%f,%f", &px, &py, &pz) >= 2) c = Vector(px + dx, py + dy, 0);
	}
	const float top = o.z + std::fmax(std::fabs(mn.z), std::fabs(mx.z)) + std::fmax(std::fabs(mn.x), std::fabs(mx.x));  // (a turned box: its corners)
	CGameTrace tr;
	engine.Trace(Vector(c.x, c.y, top + 40), Vector(c.x, c.y, o.z - std::fmax(std::fabs(mn.z), std::fabs(mx.z)) - 8), Vector(8, 8, 1), MASK_PLAYERSOLID, tr, false);
	char buf[256];
	std::snprintf(buf, sizeof buf, "%s %s origin (%.1f %.1f %.1f) box z %.1f..%.1f: top at (%.1f %.1f %.1f) on %s %s", EntKV(e, "classname").c_str(), args[1], o.x, o.y, o.z, o.z + mn.z,
	              o.z + mx.z, tr.endpos.x, tr.endpos.y, tr.endpos.z - 1, tr.m_pEnt ? EntKV(tr.m_pEnt, "classname").c_str() : "-", tr.m_pEnt ? EntKV(tr.m_pEnt, "targetname").c_str() : "");
	if (tr.startsolid || tr.fraction >= 1.0f) return console->Print("[bridge] stand on: not standable (no top reachable from above): %s\n", buf);
	// (bridge #26) the top found is another entity's or the world's: this entity has no top to stand on there (a wall panel)
	bool MoverEntOfTarget(void *hit, void *target);  // movers.cpp
	if (!MoverEntOfTarget(tr.m_pEnt, e)) return console->Print("[bridge] stand on: not standable: %s\n", buf);
	console->Print("[bridge] stand on: %s\n", buf);
	char cmd[128];
	std::snprintf(cmd, sizeof cmd, "setpos %.1f %.1f %.1f", tr.endpos.x, tr.endpos.y, tr.endpos.z + 1.0f);
	engine.Cmd(cmd);
	// (bridge #26) as the Minecraft player: the body is Portal 2's until Minecraft has been sent there (a few frames later,
	// once the setpos has run: the teleport takes the body's position of its frame)
	if (McActive()) g_awaitTeleport = true, g_standTeleportIn = 3, g_standTarget = Vector(tr.endpos.x, tr.endpos.y, tr.endpos.z + 1.0f);
}
// (bridge #25, tests) how far the Minecraft player's feet rose since the reset
CON_COMMAND(pcb_assert_mc_rise, "pcb_assert_mc_rise <min u> <max u> | reset - (tests, bridge #25) the Minecraft player's feet rose this far since the reset, and it stands\n") {
	static float base = 0;
	const Vector f = FeetSrc();
	if (args.ArgC() > 1 && args[1][0] == 'r') {
		base = f.z;
		return console->Print("[bridge] mc rise: reset at feet (%.1f %.1f %.1f)\n", f.x, f.y, f.z);
	}
	const float mn = args.ArgC() > 1 ? (float)std::atof(args[1]) : 0.0f, mx = args.ArgC() > 2 ? (float)std::atof(args[2]) : 1e9f;
	char buf[200];
	std::snprintf(buf, sizeof buf, "rose %.1f u (want %.0f..%.0f) to feet (%.1f %.1f %.1f), on ground %d, riding %s", f.z - base, mn, mx, f.x, f.y, f.z, McOnGround() ? 1 : 0,
	              MoverName(g_mc.rideMover).c_str());
	void TestCheck(bool ok, const char *what, const char *detail);
	TestCheck(f.z - base >= mn && f.z - base <= mx && McOnGround(), "the Minecraft player rode up", buf);
}
// (bridge #25) the level's body is known once Portal 2 has moved it once since the level started: a switch (or a level
// change in Minecraft mode) before that sent Minecraft to the old map's spot or (0 0 0); it is sent again to the spawn
static void StandTeleportFrame() {
	if (g_standTeleportIn <= 0) return;
	g_awaitTeleport = true;  // (the body is Portal 2's until Minecraft acks the teleport below: an old ack must not end it)
	if (--g_standTeleportIn > 0) return;
	// (bridge #31) the body must be at the target before Minecraft is sent to it: the setpos is a queued command, a slow frame can run it late
	static int s_late = 0;
	if (std::hypot(PlayerOrigin().x - g_standTarget.x, PlayerOrigin().y - g_standTarget.y) > 24.0f && s_late < 300) {
		++s_late, g_standTeleportIn = 1;
		if (s_late % 30 == 0) {  // (a setpos the engine dropped, e.g. on a player that was still respawning: issued again)
			console->Print("[bridge] stand on: the body is still at (%.1f %.1f %.1f), %.1f u from the target (%.1f %.1f %.1f) after %d frames: the teleport is issued again\n", PlayerOrigin().x, PlayerOrigin().y,
			               PlayerOrigin().z, std::hypot(PlayerOrigin().x - g_standTarget.x, PlayerOrigin().y - g_standTarget.y), g_standTarget.x, g_standTarget.y, g_standTarget.z, s_late);
			char cmd[128];
			std::snprintf(cmd, sizeof cmd, "setpos %.1f %.1f %.1f", g_standTarget.x, g_standTarget.y, g_standTarget.z);
			engine.Cmd(cmd);
			bool TeleportTo(const std::string &target, const Vector &o, const QAngle &a);  // floatorigin.cpp (bridge #32: also without sv_cheats)
			TeleportTo("!player", g_standTarget, QAngle{0, engine.ViewAngles().y, 0});
		}
		return;
	}
	s_late = 0;
	++g_teleportSeq;
	SendVelocity(Vector(0, 0, 0));
	const Vector f = PlayerOrigin();
	console->Print("[bridge] stand on: Minecraft sent to the body at (%.1f %.1f %.1f), %.1f u from the target (teleport %u)\n", f.x, f.y, f.z, (f - g_standTarget).Length(), g_teleportSeq);
}
static double g_scriptedEndAt = 0, g_bodyTeleportAt = 0, g_bodyBigTeleportAt = 0;  // (bridge #63) when Portal 2 last gave the player back / last teleported the body
static unsigned g_levelMoveBase = 0;
static bool g_bodyKnown = false;
// (bridge #36) the body was teleported by something other than the puppet (puppet.cpp: its origin jumped between two server moves): setpos, a map's
// trigger_teleport / point_teleport, SnapPlayer, a level script. The teleport wins over the puppet and over whatever carries Minecraft: Portal 2 owns
// the body until Minecraft has been sent to it and acked (g_awaitTeleport; the mod holds its player on the body while the collision around it comes in).
// Without this the next puppet write put the body back at Minecraft's old position, whenever Minecraft's last teleport had been acked already.
// pcb_mc_place / pc_tb_goto (g_standTeleportIn > 0) send Minecraft themselves once the body stands at their target.
void McBodyTeleported(const Vector &from, const Vector &to, const Vector &vel) {
	g_bodyTeleportAt = link::NowNs() * 1e-9;  // (bridge #63)
	if ((to - from).Length() > 400.0f) g_bodyBigTeleportAt = g_bodyTeleportAt;  // a cut to another place: a cutscene's, whose camera and freeze arrive a moment later
	if (!McActive() || !g_levelLive || !g_bodyKnown || g_takeover) return;
	g_awaitTeleport = true;
	if (g_standTeleportIn > 0) return;
	++g_teleportSeq;
	SendVelocity(vel);
	static int s_logs = 0;
	if (s_logs++ < 40)
		console->Print("[bridge] mc player: the body was teleported (%.1f %.1f %.1f) -> (%.1f %.1f %.1f), velocity (%.0f %.0f %.0f): Minecraft follows (teleport %u)\n", from.x, from.y, from.z,
		               to.x, to.y, to.z, vel.x, vel.y, vel.z, g_teleportSeq);
}
unsigned PuppetServerMoves();  // puppet.cpp
static void BodyKnownFrame() {
	if (g_bodyKnown || !g_levelLive) return;
	if (PuppetServerMoves() == g_levelMoveBase) return;
	g_bodyKnown = true;
	if (g_wantMc || g_awaitTeleport) {
		++g_teleportSeq;
		const Vector f = PlayerOrigin();
		console->Print("[bridge] mode: the level's body is at (%.1f %.1f %.1f): Minecraft is sent there (teleport %u)\n", f.x, f.y, f.z, g_teleportSeq);
	}
}
// (bridge #26) Minecraft takes the body over (a switch, a level change in Minecraft mode) only once it has the collision of
// every mover around the body (shapes built or loaded, bound, posed). If it had to wait, Minecraft is sent to the body where
// it is NOW once (the body may have ridden on meanwhile: the arrival elevator), and the hand-over completes on that ack.
static double g_handWaitSince = 0;
static bool g_levelChangeHandOver = false;  // the awaited teleport is a level change's (McLevelLive): it waits for the movers
static bool g_handWaited = false;
// (bridge #34) Minecraft has everything the body stands in or on: the collision of every solid mover around it (shape built or loaded, bound,
// posed) AND the static collision under the feet (the 3x3x2 regions around it sent, none of them waiting for a patch). Before, only the movers
// were asked: a body handed over where the static field was still on its way was released onto nothing.
bool CollisionFieldFeetReady(const Vector &feetSrc);  // collfield.cpp
const char *CollisionFieldFeetWhy();
// (bridge #71) Where Minecraft is released: the body's feet NOW, traced down to the real floor (HandOverSettleFrame, below), never a stale pose. When the
// body is inside solid or under the floor, the last pose it stood on a floor in. Used by the readiness checks and as the pose sent to the mod.
static bool g_safeValid = false;
static Vector g_safeFeet;
static bool g_goodValid = false;  // the last pose the body stood on a floor in (not in a sequence, not carried)
static Vector g_goodFeet;
static Vector HandOverFeet() { return g_safeValid ? g_safeFeet : PlayerOrigin(); }
// (bridge #40) how long a hand-over waits for the movers' collision. 6 s when nothing the body touches is missing; a mover touching the body (the arrival car
// it rides) that has no collision in Minecraft yet is waited for up to 30 s: releasing a player onto a car whose shape Minecraft does not have is the fall through
// the elevator (the body is Portal 2's meanwhile, so the wait costs nothing but the Minecraft-side physics)
bool MoversReadyNear(const Vector &p, float r, char *why, size_t n);  // movers.cpp
static bool HandOverGaveUp(double waited, char *why, size_t n) {
	// (bridge #63) after a Portal 2 sequence (the wake-up, the cuts of the Wheatley scene) the hand-over is bounded to 3 s, movers or not: a door under the
	// body that never gets a shape (or a region that is slow) held the pending switch for ever; Minecraft is released on the body's feet with the floor lift
	const bool afterSeq = g_scriptedEndAt > 0 && link::NowNs() * 1e-9 - g_scriptedEndAt < 30.0;
	if (afterSeq) return waited > 3.0;
	if (waited <= 6.0) return false;
	char w2[160];
	if (waited <= 30.0 && !MoversReadyNear(PlayerOrigin(), 72.0f, w2, sizeof w2)) {
		std::snprintf(why, n, "still waiting for the mover the body is on: %s", w2);
		return false;
	}
	return true;
}
static bool HandOverBodyReady(char *why, size_t n) {
	const Vector p = HandOverFeet();  // (bridge #71: the release point, not the body's raw origin)
	if (!MoversReadyNear(p, 320.0f, why, n)) return false;
	if (ClipRegionContains(p + Vector(0, 0, 36))) return true;  // (Minecraft's own terrain: the host exports no collision there)
	if (CollisionFieldFeetReady(p)) return true;
	std::snprintf(why, n, "the static collision under the body is not in Minecraft yet (%s)", CollisionFieldFeetWhy());
	return false;
}
static bool HandOverReady(const char *what) {
	const double now = link::NowNs() * 1e-9;
	if (g_handWaitSince == 0) g_handWaitSince = now;
	char why[200] = "";
	bool ready = HandOverBodyReady(why, sizeof why);
	if (!ready && HandOverGaveUp(now - g_handWaitSince, why, sizeof why)) {
		ready = true;
		console->Print("[bridge] BUG: %s: gave up waiting for the movers' collision after %.0f s (%s)\n", what, now - g_handWaitSince, why);
	}
	MoversSetUrgent(!ready);
	if (!ready) {
		if (!g_handWaited) console->Print("[bridge] mode: %s waits: %s\n", what, why);
		g_handWaited = true;
		return false;
	}
	if (g_handWaited) {
		g_handWaited = false;
		++g_teleportSeq;
		const Vector f = PlayerOrigin();
		console->Print("[bridge] mode: %s: the movers' collision is in Minecraft after %.2f s: Minecraft sent to the body at (%.1f %.1f %.1f) (teleport %u)\n", what,
		               now - g_handWaitSince, f.x, f.y, f.z, g_teleportSeq);
		return false;
	}
	g_handWaitSince = 0;
	return true;
}
// (bridge #31) HostPlayer kHpHandOver: the body is being handed to Minecraft (a switch, a level start as the Minecraft player, a level
// change, a respawn) and a mover around it (an elevator car it stands in, a lift panel) has no collision in Minecraft yet. The mod keeps
// a teleported player on the body (following it) however much static ground there is below: before, the hold released at once on the
// floor 6 blocks under the rising arrival car and Minecraft fell out of it to the shaft's bottom (the switch then took it from there).
// Same readiness as HandOverReady (every solid mover within 320 u bound and posed, Portal 2's pass-through learnt, and (#34) the static
// collision under the feet in), at most 6 s, plus 0.3 s once ready so the shapes and poses just sent have reached Minecraft.
// (bridge #34) why the grace cannot be longer: the host's hand-over completes on the mod's ack, which the mod sends only once it has released
// the player; so the flag has to drop by itself (0.3 s after the movers are in) for the hand-over to complete.
// (bridge #63) Portal 2 drives the player (a point_viewcontrol camera is the view, or the player is frozen: sp_a1_intro1's wake-up in the bed, the
// elevator videos, the final sequences). The Minecraft player must not lead then: its body is Portal 2's (g_awaitTeleport: the mod holds Minecraft on
// the body, the camera is Portal 2's, Minecraft's look is not driven), until Portal 2 gives the player back (plus 0.5 s: the sequence's last teleport
// and the look it sets land). Before, a Minecraft released onto the wake-up room's 1-voxel floor slab (feet 2 u inside it, the export's rounding) fell
// through it to the slab 28 u below, Minecraft's eye replaced the scripted camera, and J put Chell under the floor.
// Capped (a stuck flag must not hold Minecraft for ever): 240 s of camera, 30 s of the frozen flag alone.
static bool g_scriptedHold = false, g_scriptedCapped = false;
static double g_scriptedSince = 0, g_scriptedLastAt = 0, g_scriptedCamAt = 0;
static const char *g_scriptedWhy = "";
bool McScriptedRecently() { const double now = link::NowNs() * 1e-9; return g_scriptedSince != 0 || now - g_scriptedLastAt < 4.0 || now - g_bodyTeleportAt < 2.0; }  // mctransit.cpp: the view turned by the sequence itself
static bool MoveKeysDown() {  // W A S D / space held (the player is trying to walk)
	ResolveSdl();
	if (!SDL_GetKeyboardState_) return false;
	int n = 0;
	const uint8_t *ks = SDL_GetKeyboardState_(&n);
	const bool focus = (!SDL_GetKeyboardFocus_ || SDL_GetKeyboardFocus_() != nullptr) && !g_inputLock.load();
	for (int sc : {26, 22, 7, 4, 44})
		if ((focus && ks && sc < n && ks[sc]) || g_scriptKeys[sc]) return true;
	return false;
}
static double g_scriptedKeysSince = 0;
static bool g_scriptedSlow = false, g_scriptedBackToBack = false;  // (bridge #71) the debounce: 1.5 s, or 0.3 s for a lone camera
static const char *g_scriptedKind = nullptr;
static void ScriptedFrame() {
	const double now = link::NowNs() * 1e-9;
	bool cam = false, certain = false;
	const char *why = "";
	static double s_nextWatch = 0;
	if (now > s_nextWatch) s_nextWatch = now + 2.0, InputWatchInstall();  // env_fade / player_speedmod inputs (ents.cpp)
	bool on = g_levelLive && (McActive() || g_wantMc || g_awaitTeleport) && HudGateScripted(cam, certain, why);
	// (bridge #63) a cut to another place (the Wheatley scene's knockout teleport, 2900 u): the camera / freeze of the sequence arrives a moment after the
	// teleport, so for 2.5 s after it the body counts as Portal 2's (the sequence's own signals then take over)
	if (!on && g_levelLive && (McActive() || g_wantMc || g_awaitTeleport) && now - g_bodyBigTeleportAt < 2.5) on = true, why = "the level moved the body";
	// a screen fade (env_fade, ents.cpp) is Portal 2 directing a transition: the cryo wake-up's fades outlast the 2.5 s above and hide the camera behind
	// them; the body stays Portal 2's until the fade has cleared (1.5 s later the hold ends)
	if (!on && g_levelLive && (McActive() || g_wantMc || g_awaitTeleport)) {
		float fa = 0, fc[3];
		if (FadeNow(fa, fc) && fa > 0.02f) on = true, cam = certain = true, why = "a screen fade";
	}
	{  // the signals at the time (rate-limited): what to look at when a sequence is not detected
		static double s_probeAt = 0;
		static int s_probeN = 0;
		if (on && now - s_probeAt > 1.0 && s_probeN < 40) {
			char pb[200];
			HudGateProbe(pb, sizeof pb);
			s_probeAt = now, ++s_probeN;
			console->Print("[bridge] restraint probe (%s): %s\n", why, pb);
		}
	}
	if (on) {
		if (g_scriptedSince == 0) {
			g_scriptedSince = now, g_scriptedCapped = false;
			g_scriptedSlow = false, g_scriptedKind = nullptr;
			g_scriptedBackToBack = g_scriptedEndAt > 0 && now - g_scriptedEndAt < 4.0;  // (bridge #71) a new phase right after one: keep the long debounce
			console->Print("[bridge] mode: Portal 2 drives the player (%s): Minecraft is held on the body and the camera is Portal 2's until it gives the player back\n", why);
		}
		// (bridge #71) more than one kind of signal in the hold, a fade or a cut of the level: the phases may chain, so the long debounce
		if (g_scriptedKind && std::strcmp(g_scriptedKind, why)) g_scriptedSlow = true;
		g_scriptedKind = why;
		if (!std::strcmp(why, "a screen fade") || !std::strcmp(why, "the level moved the body")) g_scriptedSlow = true;
		g_scriptedLastAt = now, g_scriptedWhy = why;
		if (cam && certain) g_scriptedCamAt = now;  // (only a camera whose enabled state was read counts as one)
	} else if (g_scriptedSince != 0 && now - g_scriptedLastAt > ((g_scriptedSlow || g_scriptedBackToBack) ? 1.5 : 0.3)) {  // phases less than 1.5 s apart are one sequence (bridge #71: 0.3 s for a lone camera / freeze that Portal 2 really ended)
		console->Print("[bridge] mode: Portal 2 gave the player back after %.1f s (%s)\n", now - g_scriptedSince, g_scriptedWhy);
		g_scriptedSince = 0, g_scriptedEndAt = now;
		g_handWaitSince = 0;  // the bounded wait after a sequence counts from here (HandOverFlag restarts its own while held)
	}
	bool hold = g_scriptedSince != 0;
	if (hold && !g_scriptedCapped) {
		const bool camNow = now - g_scriptedCamAt < 0.6;
		// the player is trying to walk and no camera is certainly active: Portal 2 is not driving the player any more (a frozen flag or a view entity left
		// behind by a script), released after 2 s of keys
		if (!camNow && MoveKeysDown()) {
			if (g_scriptedKeysSince == 0) g_scriptedKeysSince = now;
		} else g_scriptedKeysSince = 0;
		const char *capWhy = nullptr;
		if (g_scriptedKeysSince != 0 && now - g_scriptedKeysSince > 2.0 && now - g_scriptedSince > 2.0) capWhy = "movement keys held for 2 s and no active camera";
		else if (camNow && now - g_scriptedSince > 240.0) capWhy = "camera active for 240 s";
		else if (!camNow && now - std::max(g_scriptedSince, g_scriptedCamAt) > 8.0) capWhy = "no active camera for 8 s";
		if (capWhy) {
			g_scriptedCapped = true;
			console->Print("[bridge] BUG: Portal 2 still reports driving the player (%s) but %s: Minecraft leads again\n", g_scriptedWhy, capWhy);
		}
	}
	g_scriptedHold = hold && !g_scriptedCapped;
	// Minecraft was leading when it began (a camera that starts after the hand-over): the body is Portal 2's again, Minecraft follows it
	if (g_scriptedHold && McActive() && !g_awaitTeleport && !g_takeover) {
		g_awaitTeleport = true;
		++g_teleportSeq;
	}
}
// ---- (bridge #71) no walking as Chell while a hand-over toward Minecraft is pending, and the release point -----------------------
// The bug: after Portal 2 gave the player back, the switch took a moment (debounce, collision, ack) and Chell walked on the user's keys; Minecraft was then
// released at a pose that was behind the body, or at a spot whose collision was not in yet (under the scene). Now Portal 2 never moves the body from the
// user's keys while Minecraft is wanted but does not lead (CreateMove_Hook zeroes move and jump / duck; the mouse look stays), and Minecraft is released at
// the body's feet as they are at release time, traced to the real floor.
bool MoverIsBound(void *e);  // movers.cpp
bool TeleportTo(const std::string &target, const Vector &o, const QAngle &a);  // floatorigin.cpp
bool PlayerNoclip();  // puppet.cpp
static bool g_inputBlock = false;
static double g_pendSince = 0;
// Minecraft is wanted and does not lead yet (the pending switch, an await of a teleport / level change / respawn, Portal 2's sequence): not a carry (g_takeover:
// Portal 2 moves the body from the real keys on purpose)
static bool HandOverPending() {
	return g_levelLive && !g_takeover && ((g_wantMc && g_mode == Mode::Chell && link::McAlive()) || g_awaitTeleport || g_scriptedHold);
}
static void HandOverInputFrame() {
	const double now = link::NowNs() * 1e-9;
	if (!HandOverPending()) {
		g_pendSince = 0, g_inputBlock = false;
		return;
	}
	if (g_pendSince == 0) g_pendSince = now;
	// (a sequence blocks as long as it holds; the rest at most 20 s: a Minecraft that never comes must not freeze Chell for ever)
	g_inputBlock = g_scriptedHold || now - g_pendSince < 20.0;
}
struct BodyFloor {
	bool solid, found;
	float top;
};
static BodyFloor TraceBodyFloor(const Vector &feet) {
	struct F : public ITraceFilter {
		virtual bool ShouldHitEntity(void *e, int) { return MoverIsBound(e); }  // (as the collision export: the world and the movers Minecraft holds)
		virtual TraceType_t GetTraceType() const { return TRACE_EVERYTHING; }
	} filter;
	const Vector s = feet + Vector(0, 0, 19.0f);
	Ray_t ray;
	ray.m_Delta = VectorAligned(0, 0, -99.0f);
	ray.m_IsSwept = true;
	ray.m_Extents = VectorAligned(4, 4, 1);
	ray.m_IsRay = false;
	ray.m_StartOffset = VectorAligned(0, 0, 0);
	ray.m_Start = VectorAligned(s.x, s.y, s.z);
	ray.m_pWorldAxisTransform = nullptr;
	CGameTrace tr;
	std::memset(&tr, 0, sizeof tr);
	engine.TraceRay(engine.traceServer->ThisPtr(), ray, MASK_PLAYERSOLID, &filter, &tr);
	BodyFloor r{tr.startsolid || tr.allsolid, false, 0.0f};
	if (!r.solid && tr.fraction < 1.0f) r.found = true, r.top = tr.endpos.z - 1.0f;
	return r;
}
static void HandOverSettleFrame() {
	g_safeValid = false;
	if (!g_levelLive || !g_bodyKnown || PlayerNoclip() || g_takeover) return;
	const double now = link::NowNs() * 1e-9;
	const Vector feet = PlayerOrigin();
	// a sequence moves the body its own way (a bed, a vehicle, a cut): neither judged nor remembered
	if (g_scriptedHold || now - g_bodyBigTeleportAt < 2.5) return;
	const BodyFloor fl = TraceBodyFloor(feet);
	const bool under = fl.solid || (fl.found && fl.top > feet.z + 6.0f);  // inside geometry, or the floor is above the feet
	const bool onFloor = !under && fl.found && feet.z - fl.top <= 2.5f;
	if (onFloor) g_goodValid = true, g_goodFeet = feet;
	if (!HandOverPending()) return;
	if (!under) {
		g_safeValid = true, g_safeFeet = feet;
		if (fl.found && feet.z - fl.top > 0.0f && feet.z - fl.top <= 6.0f) g_safeFeet.z = fl.top;  // the floor itself
		return;
	}
	// the body is in solid or under the floor: the last pose it stood on a floor in
	static double s_relocAt = 0;
	static int s_relocs = 0;
	if (!g_goodValid) return;
	g_safeValid = true, g_safeFeet = g_goodFeet;
	if (now - s_relocAt > 1.0 && s_relocs < 8) {
		s_relocAt = now, ++s_relocs;
		console->Print("[bridge] BUG: hand-over: the body is %s at (%.1f %.1f %.1f) (floor %s %.1f): back to its last pose on a floor (%.1f %.1f %.1f)\n", fl.solid ? "inside solid" : "under the floor", feet.x, feet.y,
		               feet.z, fl.found ? "top" : "-", fl.top, g_goodFeet.x, g_goodFeet.y, g_goodFeet.z);
		TeleportTo("!player", g_goodFeet, QAngle{0, engine.ViewAngles().y, 0});
	}
}
static double g_handFlagSince = 0, g_handFlagReadyAt = 0;
static bool g_handFlagCapped = false;
static bool HandOverFlag() {
	const double now = link::NowNs() * 1e-9;
	const bool want = g_levelLive && ((g_wantMc && g_mode == Mode::Chell) || g_awaitTeleport) && !g_takeover;
	if (!want) {
		g_handFlagSince = g_handFlagReadyAt = 0;
		g_handFlagCapped = false;
		return false;
	}
	if (g_scriptedHold) {  // (bridge #63) Portal 2 drives the player: held, whatever the collision around it
		g_handFlagSince = now, g_handFlagReadyAt = 0, g_handFlagCapped = false;
		return true;
	}
	if (g_handFlagSince == 0) g_handFlagSince = now;
	char why[200] = "";
	bool ready = HandOverBodyReady(why, sizeof why);
	if (!ready && HandOverGaveUp(now - g_handFlagSince, why, sizeof why)) {
		ready = true;
		if (!g_handFlagCapped) console->Print("[bridge] BUG: hand-over: gave up holding Minecraft for the movers' collision after %.0f s (%s)\n", now - g_handFlagSince, why);
		g_handFlagCapped = true;
	}
	if (!ready) {
		g_handFlagReadyAt = 0;
		MoversSetUrgent(true);
		return true;
	}
	if (g_handFlagReadyAt == 0) g_handFlagReadyAt = now;
	return now - g_handFlagReadyAt < 0.3;
}
static bool g_handOverNow = false;  // (bridge #34) this frame's kHpHandOver
static HostPlayer g_hpLast{};       // (bridge #34) the last HostPlayer written in full
static double g_carriedTotal = 0;  // seconds Portal 2 carried the body (pcb_assert_mc_carry)
static double g_carryLongest = 0;
static void TakeoverFrame() {
	RideSample();
	CollisionFieldBoost(g_takeover);  // (while Portal 2 carries the body, the feet's collision patches get the larger budget)
	if (!g_takeover) return;
	static int n = 0;
	if (++n % 4 == 0) ++g_teleportSeq;  // Minecraft follows the body
	const double now = link::NowNs() * 1e-9;
	if (g_mode != Mode::Minecraft) {
		g_takeover = false;
		return;
	}
	if (now - g_lastPushAt < 0.4) return;
	// (after a fall guard catch: until Minecraft has a floor under the body, the static field's or a mover's, at most 3 s)
	// (bridge #34: up to 8 s while a mover near the body still has no collision in Minecraft: the car it fell out of is the floor it waits for)
	if (g_catchAt > 0 && !CollisionFieldSentFloorUnder(PlayerOrigin()) && !MoverFloorUnder(PlayerOrigin(), nullptr)) {
		char why[160];
		if (now - g_catchAt < 3.0 || (now - g_catchAt < 8.0 && !MoversReadyNear(PlayerOrigin(), 320.0f, why, sizeof why))) return;
	}
	g_catchAt = 0;
	g_takeover = false;
	const double carried = now - s_carrySince;
	g_carriedTotal += carried, g_carryLongest = std::max(g_carryLongest, carried);
	++g_teleportSeq;  // Minecraft to the body, then it leads (g_awaitTeleport clears on the ack)
	SendVelocity(PlayerVelocity());
	const Vector f = PlayerOrigin();
	console->Print("[bridge] mc player: carried %.2f s after a fall guard catch: Minecraft leads again from (%.1f %.1f %.1f) (teleport %u)\n", carried, f.x, f.y, f.z, g_teleportSeq);
}
CON_COMMAND(pcb_assert_mc_carry, "pcb_assert_mc_carry <max total s> [max single s] | reset - Portal 2 carried the Minecraft player's body at most this long since the reset (bridge #23)\n") {
	static double base = 0;
	if (args.ArgC() > 1 && args[1][0] == 'r') return void((base = g_carriedTotal, g_carryLongest = 0));
	const double mx = args.ArgC() > 1 ? std::atof(args[1]) : 0.1, mx1 = args.ArgC() > 2 ? std::atof(args[2]) : mx;
	const double total = g_carriedTotal - base + (g_takeover ? 1e3 : 0);
	char buf[160];
	std::snprintf(buf, sizeof buf, "%.2f s carried in all, longest %.2f s, %s (want <= %.2f / %.2f)", g_carriedTotal - base, g_carryLongest, g_takeover ? "carried NOW" : "Minecraft leads", mx, mx1);
	void TestCheck(bool ok, const char *what, const char *detail);
	TestCheck(total <= mx && g_carryLongest <= mx1, "Portal 2 barely carried the Minecraft player", buf);
}
// (bridge #26) per Minecraft tick: on a mover (and carried by it: it moved), the walk relative to it, the view turned with it
static double g_platSpeedMax = 0;
static uint64_t g_rideTick = 0;
static int g_rideTicks = 0, g_onMoverTicks = 0;  // ticks carried by a moving mover / standing on any mover
static int g_pushTicks = 0;
static uint32_t g_rideTarget = 0;  // pcb_assert_mc_rode reset <target>
static int g_rideTargetTicks = 0;
static Vector g_rideTargetFrom(0, 0, 0), g_rideTargetAngFrom(0, 0, 0);
static std::string g_lastRideName;
static uint32_t g_yawMover = 0;
static float g_yawLast = 0;
// (bridge #26) the feet's height relative to the mover they ride, every host frame of a ride (constant if smooth)
static uint32_t g_rtMover = 0;
static double g_rtMin = 1e9, g_rtMax = -1e9;
static int g_rtFrames = 0, g_rtOther = 0;
static void RideTrackFrame() {
	if (!McActive() || !OnMover() || !(g_mc.rideFlags & kRideMoved)) return;
	double ax, ay, az;
	if (!MoverRenderAnchorMc(g_mc.rideMover, ax, ay, az)) return;
	if (!g_rtMover) g_rtMover = g_mc.rideMover;
	if (g_mc.rideMover != g_rtMover) return void(++g_rtOther);
	// (frames of a tick where the feet moved on the mover themselves, walking or settling at the hand-over, are not the ride's
	// smoothness: they are counted apart)
	const double dl = std::fabs((g_mc.curY - g_mc.rideCurY) - (g_mc.prevY - g_mc.ridePrevY));
	if (dl > 0.02) return void(++g_rtOther);
	const double rel = FeetSrc().z - McToSrc(ax, ay, az).z;
	g_rtMin = std::min(g_rtMin, rel), g_rtMax = std::max(g_rtMax, rel), ++g_rtFrames;
	if (g_rtFrames <= 160)
		console->Print("[bridge] ride track %d: rel %.2f alpha %.2f local0 %.3f local1 %.3f (blocks) tick %llu anchor y %.4f ridePrev y %.4f rideCur y %.4f\n", g_rtFrames, rel, Alpha(),
		               g_mc.prevY - g_mc.ridePrevY, g_mc.curY - g_mc.rideCurY, (unsigned long long)g_mc.tickQpc, ay, g_mc.ridePrevY, g_mc.rideCurY);
}
CON_COMMAND(pcb_ride_track, "pcb_ride_track reset | assert <max range u> [min frames] - (bridge #26) the feet's height relative to the ridden mover, every frame of the ride\n") {
	if (args.ArgC() > 1 && args[1][0] == 'r') return void((g_rtMover = 0, g_rtMin = 1e9, g_rtMax = -1e9, g_rtFrames = 0, g_rtOther = 0));
	const double mx = args.ArgC() > 2 ? std::atof(args[2]) : 0.5;
	const int mf = args.ArgC() > 3 ? std::atoi(args[3]) : 30;
	char buf[220];
	std::snprintf(buf, sizeof buf, "feet - mover origin z in %.2f..%.2f (range %.2f u, want <= %.2f) over %d frames riding %s (%d frames on another mover)", g_rtMin, g_rtMax,
	              g_rtMax - g_rtMin, mx, g_rtFrames, MoverName(g_rtMover).c_str(), g_rtOther);
	void TestCheck(bool ok, const char *what, const char *detail);
	TestCheck(g_rtFrames >= mf && g_rtMax - g_rtMin <= mx, "the feet stay put on the ridden mover every frame", buf);
}
// (bridge #31) a ride on a turning mover: per host frame, how far the rider's feet went round the mover's origin, how far the mover turned and how
// far the view turned, summed while the same mover is ridden (pcb_ride_spin)
static double g_spinOrbit = 0, g_spinMover = 0, g_spinView = 0, g_spinRadius = 0;
static int g_spinFrames = 0;
static uint32_t g_spinId = 0;
static double g_spinLastOrbit = 0, g_spinLastYaw = 0, g_spinLastView = 0;
static std::string g_spinName;
static void SpinSample() {
	bool MoverRenderAnchorMc(uint32_t id, double &x, double &y, double &z);
	float yaw;
	double ax, ay, az;
	if (!McActive() || !OnMover() || !MoverUprightYaw(g_mc.rideMover, yaw) || !MoverRenderAnchorMc(g_mc.rideMover, ax, ay, az)) return void(g_spinId = 0);
	const Vector rel = FeetSrc() - McToSrc(ax, ay, az);
	const double orbit = std::atan2(rel.y, rel.x) * 57.29578, view = engine.ViewAngles().y, radius = std::sqrt(rel.x * rel.x + rel.y * rel.y);
	if (g_spinId == g_mc.rideMover && radius > 4.0) {
		g_spinOrbit += std::remainder(orbit - g_spinLastOrbit, 360.0), g_spinMover += std::remainder(yaw - g_spinLastYaw, 360.0), g_spinView += std::remainder(view - g_spinLastView, 360.0);
		g_spinRadius = radius, ++g_spinFrames, g_spinName = MoverName(g_spinId);
	}
	g_spinId = g_mc.rideMover, g_spinLastOrbit = orbit, g_spinLastYaw = yaw, g_spinLastView = view;
}
CON_COMMAND(pcb_ride_spin, "pcb_ride_spin reset | assert <min deg> [tol deg=8] - (bridge #31) riding a turning mover: the feet went round its origin, and the view turned, as far as the mover turned (at least min)\n") {
	if (args.ArgC() > 1 && args[1][0] == 'r') return void((g_spinOrbit = g_spinMover = g_spinView = 0, g_spinFrames = 0, g_spinId = 0));
	const double mn = args.ArgC() > 2 ? std::atof(args[2]) : 20.0, tol = args.ArgC() > 3 ? std::atof(args[3]) : 8.0;
	char buf[300];
	std::snprintf(buf, sizeof buf, "mover turned %.1f deg, feet went round it %.1f deg (radius %.0f u), view turned %.1f deg over %d frames riding %s (want mover >= %.0f, orbit and view within %.0f of it)",
	              g_spinMover, g_spinOrbit, g_spinRadius, g_spinView, g_spinFrames, g_spinName.c_str(), mn, tol);
	void TestCheck(bool ok, const char *what, const char *detail);
	TestCheck(std::fabs(g_spinMover) >= mn && std::fabs(g_spinOrbit - g_spinMover) <= tol && std::fabs(g_spinView - g_spinMover) <= tol, "the rider goes round with the turning mover and the view turns with it", buf);
}
static void RideSample() {
	RideTrackFrame();
	SpinSample();
	McmFrame();
	// the view turns with a mover the player stands on (a turntable): Portal 2 owns the look, so the host turns it
	if (McActive() && OnMover()) {
		float yaw;
		if (MoverUprightYaw(g_mc.rideMover, yaw)) {
			if (g_yawMover == g_mc.rideMover) {
				const float d = std::remainder(yaw - g_yawLast, 360.0f);
				if (std::fabs(d) > 0.01f && std::fabs(d) < 30.0f) {
					QAngle va = engine.ViewAngles();
					va.y += d;
					engine.SetAngles(va);
				}
			}
			g_yawMover = g_mc.rideMover, g_yawLast = yaw;
		} else g_yawMover = 0;
	} else g_yawMover = 0;
	if (!McActive() || (uint64_t)g_mc.tickQpc == g_rideTick) return;
	g_rideTick = (uint64_t)g_mc.tickQpc;
	if (g_mc.rideFlags & kRidePushed) ++g_pushTicks;
	if (!OnMover()) return;
	++g_onMoverTicks;
	if (!(g_mc.rideFlags & kRideMoved)) return;
	++g_rideTicks;
	bool MoverOfTarget(uint32_t ride, uint32_t target);
	if (g_rideTarget && MoverOfTarget(g_mc.rideMover, g_rideTarget)) ++g_rideTargetTicks;
	static int logs = 0;
	const std::string name = MoverName(g_mc.rideMover);
	if (name != g_lastRideName && logs++ < 40) console->Print("[bridge] mc player: riding %s\n", name.c_str());
	g_lastRideName = name;
	const double dx = (g_mc.curX - g_mc.prevX) - (g_mc.rideCurX - g_mc.ridePrevX), dz = (g_mc.curZ - g_mc.prevZ) - (g_mc.rideCurZ - g_mc.ridePrevZ);
	const double v = std::sqrt(dx * dx + dz * dz) * 1000.0 / (g_mc.tickMs > 0 ? g_mc.tickMs : 50.0);
	if (v < 20.0) g_platSpeedMax = std::max(g_platSpeedMax, v);
}
CON_COMMAND(pcb_assert_mc_rode, "pcb_assert_mc_rode <min ticks> [max catches=0] | reset - (bridge #26) Minecraft was carried by a moving mover at least this many ticks, with at most this many fall guard catches\n") {
	static int base = 0, cbase = 0, obase = 0, pbase = 0;
	if (args.ArgC() > 1 && args[1][0] == 'r') {
		g_rideTarget = 0, g_rideTargetTicks = 0;
		if (args.ArgC() > 2) {
			void *te = MoverFind(args[2], &g_rideTarget);
			const int off = te ? EntFieldOffset(te, "m_vecAbsOrigin") : -1;
			g_rideTargetFrom = off >= 0 ? *(const Vector *)((const char *)te + off) : Vector(0, 0, 0);
			const int offR = te ? EntFieldOffset(te, "m_angAbsRotation") : -1;
			g_rideTargetAngFrom = offR >= 0 ? *(const Vector *)((const char *)te + offR) : Vector(0, 0, 0);
		}
		return void((base = g_rideTicks, cbase = g_catches, obase = g_onMoverTicks, pbase = g_pushTicks));
	}
	if (g_rideTarget) {
		const int mn = args.ArgC() > 1 ? std::atoi(args[1]) : 20, mxc = args.ArgC() > 2 ? std::atoi(args[2]) : 0;
		void *te = MoverFind(("#" + std::to_string(g_rideTarget)).c_str(), nullptr);
		const int off = te ? EntFieldOffset(te, "m_vecAbsOrigin") : -1;
		float moved = off >= 0 ? (*(const Vector *)((const char *)te + off) - g_rideTargetFrom).Length() : 0.0f;
		{  // (bridge #31) a mover that only turned (a turntable, a tilting panel) moved too: its angle change, in degrees, counts as u
			const int offR = te ? EntFieldOffset(te, "m_angAbsRotation") : -1;
			if (offR >= 0) {
				const Vector a = *(const Vector *)((const char *)te + offR);
				moved += std::fabs(std::remainder(a.x - g_rideTargetAngFrom.x, 360.0f)) + std::fabs(std::remainder(a.y - g_rideTargetAngFrom.y, 360.0f)) +
				         std::fabs(std::remainder(a.z - g_rideTargetAngFrom.z, 360.0f));
			}
		}
		char b2[300];
		void TestCheck(bool ok, const char *what, const char *detail);
		if (moved < 2.0f && g_rideTargetTicks == 0) {  // (its inputs did not move it: nothing to ride; a turning-only mover counts its ticks)
			std::snprintf(b2, sizeof b2, "the target %s did not move (%.1f u): not a ride", MoverName(g_rideTarget).c_str(), moved);
			return console->Print("[bridge] rode: %s\n", b2);
		}
		std::snprintf(b2, sizeof b2, "%d ticks carried by the target %s (want >= %d; %d by any moving mover), %d fall guard catches (want <= %d)", g_rideTargetTicks, MoverName(g_rideTarget).c_str(), mn,
		              g_rideTicks - base, g_catches - cbase, mxc);
		void TestCheck(bool ok, const char *what, const char *detail);
		return TestCheck(g_rideTargetTicks >= mn && g_catches - cbase <= mxc, "Minecraft rode the target mover itself", b2);
	}
	const int mn = args.ArgC() > 1 ? std::atoi(args[1]) : 20, mxc = args.ArgC() > 2 ? std::atoi(args[2]) : 0;
	char buf[260];
	std::snprintf(buf, sizeof buf, "%d ticks carried by a moving mover (want >= %d; %d on a mover, %d pushed; last %s), %d fall guard catches (want <= %d)", g_rideTicks - base, mn,
	              g_onMoverTicks - obase, g_pushTicks - pbase, g_lastRideName.c_str(), g_catches - cbase, mxc);
	void TestCheck(bool ok, const char *what, const char *detail);
	TestCheck(g_rideTicks - base >= mn && g_catches - cbase <= mxc, "Minecraft rode the mover itself", buf);
}
CON_COMMAND(pcb_assert_mc_platform_walk, "pcb_assert_mc_platform_walk <min b/s> <max b/s> | reset - the fastest walk on a moving mover, relative to it, since the reset (bridge #23)\n") {
	if (args.ArgC() > 1 && args[1][0] == 'r') return void(g_platSpeedMax = 0);
	const double mn = args.ArgC() > 1 ? std::atof(args[1]) : 3.5, mx = args.ArgC() > 2 ? std::atof(args[2]) : 6.0;
	char buf[120];
	std::snprintf(buf, sizeof buf, "%.2f blocks/s relative to the mover (want %.1f-%.1f; Minecraft walks 4.3, sprints 5.6)", g_platSpeedMax, mn, mx);
	void TestCheck(bool ok, const char *what, const char *detail);
	TestCheck(g_platSpeedMax >= mn && g_platSpeedMax <= mx, "Minecraft's walking speed on the mover", buf);
}
CON_COMMAND(pcb_assert_mc_platform, "pcb_assert_mc_platform [want 1|0] [target: name|#id|@x,y,z] - Minecraft stands on a mover (that one) now (bridge #23/#26)\n") {
	const int want = args.ArgC() > 1 ? std::atoi(args[1]) : 1;
	if (args.ArgC() > 2) {
		uint32_t id = 0;
		MoverFind(args[2], &id);
		char b2[260];
		std::snprintf(b2, sizeof b2, "riding %s, target %s = %s; Minecraft feet (%.3f %.3f %.3f) on ground %d", MoverName(g_mc.rideMover).c_str(), args[2], MoverName(id).c_str(), g_mc.curX,
		              g_mc.curY, g_mc.curZ, (g_mc.flags & kMcOnGround) ? 1 : 0);
		void TestCheck(bool ok, const char *what, const char *detail);
		bool MoverOfTarget(uint32_t ride, uint32_t target);
		return TestCheck(id && MoverOfTarget(g_mc.rideMover, id) == (want != 0), want ? "Minecraft stands on the target mover" : "Minecraft is not on the target mover", b2);
	}
	char buf[220];
	std::snprintf(buf, sizeof buf, "riding %s (flags %u); Minecraft feet (%.3f %.3f %.3f) mover origin (%.3f %.3f %.3f) on ground %d", MoverName(g_mc.rideMover).c_str(), g_mc.rideFlags, g_mc.curX,
	              g_mc.curY, g_mc.curZ, g_mc.rideCurX, g_mc.rideCurY, g_mc.rideCurZ, (g_mc.flags & kMcOnGround) ? 1 : 0);
	void TestCheck(bool ok, const char *what, const char *detail);
	TestCheck(OnMover() == (want != 0), want ? "Minecraft stands on a mover" : "Minecraft stands on no mover", buf);
}
CON_COMMAND(pcb_assert_mc_takeovers, "pcb_assert_mc_takeovers <min> [max] | reset - the body was carried by Portal 2 (bridge #26: only after a fall guard catch) min..max times since the reset\n") {
	static int base = 0;
	if (args.ArgC() > 1 && args[1][0] == 'r') return void(base = g_takeovers);
	const int want = args.ArgC() > 1 ? std::atoi(args[1]) : 1, mx = args.ArgC() > 2 ? std::atoi(args[2]) : 1 << 30;
	char buf[120];
	std::snprintf(buf, sizeof buf, "%d since the reset (want %d..%d), now %s", g_takeovers - base, want, mx, g_takeover ? "carried" : "Minecraft leads");
	void TestCheck(bool ok, const char *what, const char *detail);
	TestCheck(g_takeovers - base >= want && g_takeovers - base <= mx, "Portal 2 took the body over", buf);
}
// (bug marks) what the Minecraft player stands on
const char *CollisionFieldLastMovingUnder() {
	static std::string s;
	s = MoverName(g_mc.rideMover);
	return s.c_str();
}

void PlayerEventsJumpRelayed();       // playerevents.cpp (bridge #70)
void PlayerEventsSneak(bool sneaking);
void PlayerEventsFrame();
void PlayerEventsLevelInit();
bool PuppetClientBody(Vector &feet, double &ageSec);  // puppet.cpp (bridge #70)
float PuppetViewOffsetZ();

uint32_t McTransitAck();
static bool PuppetProvider(Vector &pos, Vector &vel) {
	// (bridge #25) the fall guard caught the Minecraft player: the body goes back on top of the floor it fell through,
	// for a few ticks (server and client), then Portal 2 carries it (the takeover) until Minecraft has that floor
	if (g_catchUntil > 0 && link::NowNs() * 1e-9 < g_catchUntil) {
		pos = g_catchPos, vel = g_catchVel;
		return true;
	}
	if (!McActive() || g_awaitTeleport || g_takeover) return false;
	vel = VelocitySrc();
	const Vector rawFeet = FeetSrc(&vel, true);
	pos = McTransitHoldBody(rawFeet);  // (bridge #28: folded through the pair like the camera, never through a portal's plane)
	{  // (bridge #68, diagnosis) a jump of the body between two provider calls, with what each stage said (first 24)
		static Vector last;
		static int logs = 0;
		if (last.Length() > 0.1f && (pos - last).Length() > 100.0f && logs < 24)
			++logs, console->Print("[bridge] puppet jump: provider (%.1f %.1f %.1f) -> (%.1f %.1f %.1f); folded feet (%.1f %.1f %.1f), Minecraft feet (%.1f %.1f %.1f), transitSeq %u seen %u, vel (%.0f %.0f %.0f)\n", last.x,
			                       last.y, last.z, pos.x, pos.y, pos.z, rawFeet.x, rawFeet.y, rawFeet.z, FeetSrc().x, FeetSrc().y, FeetSrc().z, g_mc.transitSeq, McTransitAck(), vel.x, vel.y, vel.z);
		last = pos;
	}
	return true;
}

Vector PortalRightAxis(int i);  // composite.cpp
void PortalThroughPose(int i, int j, const Vector &o, const QAngle &a, Vector &oo, QAngle &oa);
void McTransitFrame(const McState &mc, bool active);  // mctransit.cpp (bridge #28)
uint32_t McTransitAck();

// ---- the per-frame link work -----------------------------------------------------------
uint64_t McFrameCounter() { return g_mc.frameCounter; }  // (bridge #53) growth.cpp
// (bridge #56b) Chell's viewmodel stays off for as long as the mode is Minecraft. r_drawviewmodel is a cheat cvar: Portal 2 puts cheat cvars
// back to their default on a level load (a death reload, a save load, a map restart) when cheats are off, so the one-off "r_drawviewmodel 0"
// of the switch was undone and the big portal gun came back beside Minecraft's hand. The value is now checked every frame and written straight
// into the cvar (no cheat check, no change notice) only when it differs; the user's own value is put back on leaving Minecraft mode.
static void EnforceViewmodel(bool hide) {
	static ConVar *cv = nullptr;
	static bool held = false;
	static int saved = 1;
	if (!cv) cv = (ConVar *)tier1->FindCommandBase(tier1->g_pCVar->ThisPtr(), "r_drawviewmodel");
	if (!cv) return;
	ConVar *v = cv->m_pParent ? cv->m_pParent : cv;
	if (hide) {
		if (!held) held = true, saved = v->m_nValue ? v->m_nValue : 1;
		if (v->m_nValue != 0) {
			v->m_nValue = 0, v->m_fValue = 0.0f;
			static int logged = 0;
			if (logged++ < 20) console->Print("[bridge] mc player: r_drawviewmodel was back on (a level load resets it): off again\n");
		}
	} else if (held) {
		held = false;
		v->m_nValue = saved, v->m_fValue = (float)saved;
	}
}

// (bridge #56) Minecraft's hand and HUD are drawn exactly when Portal 2 would draw Chell's viewmodel and HUD. One rule for the
// composite (glhud.cpp) and for HostPlayer kHpWantHud (Minecraft does not even render it otherwise). Evaluated per frame
// from engine state; a change is logged once ("hud: hidden (reason)" / "hud: shown"), at most 4 lines a second.
static bool HudShouldShow(bool paused, const char *&why) {
	why = "shown";
	if (!GlHudEnabled()) return why = "pc_mc_hud 0", false;
	if (!g_levelLive) return why = "loading", false;
	if (engine.IsPaused(engine.client->ThisPtr())) return why = "pause menu", false;
	if (paused) return why = "console", false;
	if (!McActive()) return why = "Minecraft not leading", false;
	if (g_levelChangeHandOver && g_awaitTeleport) return why = "level change", false;  // (bridge #69: with the await, a flag left over from a J during the level change hid the HUD for good)
	if (g_mc.flags & kMcDead) return why = "Minecraft death screen", false;
	if (CombatHudHidden()) return why = "Portal 2 death", false;
	const char *p2 = nullptr;
	// Portal 2 has no gun / HUD up, but a Minecraft screen (inventory, chat) the player opened still shows
	if (!(g_mc.flags & kMcScreenOpen) && !HudGateP2Shows(p2)) return why = p2, false;
	return true;
}
static void HudLogChange(bool show, const char *why) {
	static int last = -1;
	static double lastLog = -10;
	static const char *lastWhy = "";
	if (last == (show ? 1 : 0) && (show || lastWhy == why)) return;
	const double now = link::NowNs() * 1e-9;
	if (now - lastLog < 0.25) return;  // (rate limit: the change is logged on a later frame)
	lastLog = now;
	last = show ? 1 : 0, lastWhy = why;
	if (show) console->Print("[bridge] hud: shown\n");
	else console->Print("[bridge] hud: hidden (%s)\n", why);
}

#ifdef _WIN32
// PCB_FRAMESTOP=n: bisect aid, Frame() returns at stage n (Windows-only run diagnosis)
static int FrameStopAt() { static const int v = std::atoi(plat::Getenv("PCB_FRAMESTOP").c_str()); return v; }
#define FSTOP(n) do { if (FrameStopAt() && FrameStopAt() <= (n)) return; } while (0)
#else
#define FSTOP(n) do { } while (0)
#endif
static void Frame() {
	GrowthFrame();  // (bridge #53) the periodic growth line
	PlayerEventsFrame();  // (bridge #70)
	FSTOP(1);
	PROF("mcframe");  // (bridge #25) the whole per-frame link work; its parts are mc.*
	link::Heartbeat();
	bool up = link::McAlive();
	if (up != g_linkWasUp) {
		g_linkWasUp = up;
		console->Print("[bridge] link %s (Minecraft pid %u)\n", up ? "up" : "down", link::McPid());
		if (up) ++g_teleportSeq;  // put the Minecraft player where Chell is
		// A different Minecraft (restarted mid-session) resends its whole world once it is in it;
		// what the last one streamed is dropped so the exit is validated from the new one's data.
		static uint32_t lastPid = 0;
		if (up && lastPid && link::McPid() != lastPid) {
			console->Print("[bridge] link: a new Minecraft (pid %u, was %u): dropping the old one's terrain\n", link::McPid(), lastPid);
			TerrainClear();
		}
		if (up) lastPid = link::McPid();
		ClipExitRecheck(up ? "link up" : "link down");
	}
	g_haveMc = link::ReadMcState(g_mc) || g_haveMc;
	if (g_haveMc) McTransitFrame(g_mc, McActive());  // (bridge #28) Minecraft's own portal transits: the look turned once
	FSTOP(2);

	// Become the Minecraft player once Minecraft is actually in its world.
	// (bridge #14: and once Minecraft's hand and GUI are ready at the screen's size, so the switch frame swaps Portal 2's
	// gun for Minecraft's hand in one go; at most 1 s)
	const bool hudReady = !GlHudEnabled() || GlHudReady() || link::NowNs() * 1e-9 - g_wantSince > 1.0;
	// (lead, perf: the collision export is sliced over frames: Minecraft keeps following Chell's feet, without taking
	// over, until the regions she stands on have been sent; at most 3 s)
	static int s_followN = 0;
	(void)s_followN;  // (bridge #23: no teleports while the collision is on its way: the mod holds its player, as SkyCraft does)
	BodyKnownFrame();
	FSTOP(21);
	StandTeleportFrame();
	FSTOP(22);
	ScriptedFrame();  // (bridge #63)
	FSTOP(23);
	HandOverInputFrame();   // (bridge #71)
	FSTOP(24);
	HandOverSettleFrame();  // (bridge #71)
	FSTOP(25);
	if (g_wantMc && g_mode == Mode::Chell && up && g_haveMc && (g_mc.flags & kMcInWorld) && g_mc.teleportAck == g_teleportSeq && hudReady && g_bodyKnown && !g_scriptedHold &&
	    HandOverReady("switch")) {
		// (bridge #31, the net) Minecraft must be at the body when it takes it over: the take-over moves Portal 2's body to Minecraft's place, so
		// a Minecraft that is elsewhere (it fell out of an elevator car) drags the body there. Sent to the body again instead (a few tries).
		static int s_miss = 0;
		const float gap = (FeetSrc() - PlayerOrigin()).Length();
		if (gap > 64.0f && s_miss < 6) {
			++s_miss, ++g_teleportSeq;
			console->Print("[bridge] %shand-over: Minecraft is %.0f u from the body at the switch: sent to the body again (try %d, teleport %u)\n",
			               McScriptedRecently() ? "mode: " : "BUG: ", gap, s_miss, g_teleportSeq);  // (bridge #63: Portal 2's sequence moved the body, not a bug)
		} else {
			s_miss = 0;
			g_mode = Mode::Minecraft;
			engine.Cmd("r_drawviewmodel 0");
			Vector f = FeetSrc();
			console->Print("[bridge] mode: Minecraft player at MC (%.2f %.2f %.2f) = Portal 2 (%.1f %.1f %.1f)\n", g_mc.curX, g_mc.curY, g_mc.curZ, f.x, f.y, f.z);
		}
	}
	{
		PROF("mc.movers");  // (inside "mcframe": the ride sample, the fall guard)
		TakeoverFrame();
		FallGuardFrame();
	}
	FSTOP(4);
	// Minecraft is at the exit: it leads again (bridge #26: after a level change, once it has the movers around the body too)
	{
		// (bridge #68) how long a teleport kept the body Portal 2's (map teleports: wakeup's !player point_teleports, a2_intro's incinerator, a3_01's knockout): the
		// ack time, and a BUG line once if it takes more than 15 s (a destination whose collision never arrives)
		static unsigned seqSeen = 0;
		static double since = 0;
		static bool warned = false;
		const double nowT = link::NowNs() * 1e-9;
		if (g_awaitTeleport && (seqSeen != g_teleportSeq || since == 0)) seqSeen = g_teleportSeq, since = nowT, warned = false;
		const bool endsNow = g_awaitTeleport && !g_takeover && !g_scriptedHold && g_standTeleportIn <= 0 && (!McActive() || (g_mc.teleportAck == g_teleportSeq && (g_bodyKnown || !g_levelLive) && (!g_levelChangeHandOver || HandOverReady("level change"))));
		if (g_awaitTeleport && !endsNow && !warned && since > 0 && nowT - since > 15.0 && !g_scriptedHold) {
			warned = true;
			char w[200] = "";
			if (McActive()) HandOverBodyReady(w, sizeof w);
			console->Print("[bridge] BUG: teleport %u has kept the body Portal 2's for %.0f s: ack %u, body known %d, level change %d, feet ready: %s\n", g_teleportSeq, nowT - since, g_mc.teleportAck, g_bodyKnown, g_levelChangeHandOver, w[0] ? w : "-");
		}
		if (endsNow && since > 0) {
			static int logs = 0;
			if (logs++ < 40) console->Print("[bridge] mode: teleport %u ready after %.2f s: Minecraft leads again at (%.0f %.0f %.0f)\n", g_teleportSeq, nowT - since, PlayerOrigin().x, PlayerOrigin().y, PlayerOrigin().z);
			since = 0;
		}
	}
	if (g_awaitTeleport && !g_takeover && !g_scriptedHold && g_standTeleportIn <= 0 && (!McActive() || (g_mc.teleportAck == g_teleportSeq && (g_bodyKnown || !g_levelLive) && (!g_levelChangeHandOver || HandOverReady("level change")))))
		g_awaitTeleport = false, g_levelChangeHandOver = false;
	{
		bool PlayerVmGrab();   // ents.cpp (bridge #68)
		bool PlayerHolding();  // propcollide.cpp
		EnforceViewmodel(g_mode == Mode::Minecraft && !(PlayerVmGrab() && PlayerHolding()));  // (a forced viewmodel grab: Portal 2 draws the carried object in its viewmodel)
	}
	if (g_levelLive) EnforceBodyHidden();
	if (g_levelLive && !g_hidden && g_mode == Mode::Chell && !g_wantMc) EnforceChellShown();  // (bridge #61)
	if (g_levelLive) HideChell(McActive() || g_wantMc);  // (and while a switch is pending: no Portal 2 crosshair in the switch frames)
	else g_hidden = false;  // (a new level: the entity is new)
	if (up) SendHoles(g_levelLive && (McActive() || g_wantMc));
	if (up) SendPortalPair(g_levelLive && (g_mobTransit || McActive() || g_wantMc));  // (bridge #28: the player goes through it too)
	FSTOP(5);

	Vector feet = HandOverFeet();  // (bridge #71: while a hand-over is pending, Minecraft is sent to the release point: the body's feet traced to the floor)
	QAngle va = engine.ViewAngles();
	double mx, my, mz;
	SrcToMcD(feet, mx, my, mz);
	// (bridge #47) the virtual region (stage.cpp) is a region as well: the hidden player waits at the stage's cliff, so the stage's chunks stay loaded
	if (g_terrainMode || StageActive()) {
		// The hidden player is parked at the anchor (the cliff), or follows Chell while she is in the
		// region: Minecraft's server only sends a sound to players within ~16 blocks of it, and spawns
		// mobs around players. It is re-placed when she is more than 12 blocks from where it was put.
		static double px = 1e9, py = 1e9, pz = 1e9;
		double tx = g_map.mcX, ty = g_map.mcY, tz = g_map.mcZ;
		if (!g_terrainMode) StageAnchorMc(tx, ty, tz);
		if (g_mode == Mode::Minecraft && !g_awaitTeleport) {
			// the Minecraft player leads: its own position, no teleports
			tx = g_mc.curX, ty = g_mc.curY, tz = g_mc.curZ;
			px = tx, py = ty, pz = tz;
		} else if (g_levelLive && (g_wantMc || g_awaitTeleport)) {
			// switching to it, or following a transit: Chell's feet, anywhere (the chamber band too); the teleport was
			// bumped by the switch / the transit, the position goes with whatever frame Minecraft takes it in
			SrcToMcD(feet, tx, ty, tz);
			px = tx, py = ty, pz = tz;
		} else {
			// (bridge #19: in the chambers too, so Minecraft keeps their entities loaded, ticking and exported near her; the hidden
			// player is unseen and unhurt there, HiddenPlayerMixin, and mobs hunt Chell's stand-in)
			if (g_levelLive && (ClipRegionContainsMc(feet) || (ChellProxyInChambers() && !StageActive())) && g_mode == Mode::Chell) SrcToMcD(feet, tx, ty, tz);  // (bridge #49: with the stage on it waits at the stage: Minecraft draws the views of the stage seen through the portal around its player, which must be near them)
			if ((tx - px) * (tx - px) + (ty - py) * (ty - py) + (tz - pz) * (tz - pz) > 144.0) px = tx, py = ty, pz = tz, ++g_teleportSeq;
		}
		mx = px, my = py, mz = pz;
	}
	SkyState s{};
	bool paused = engine.IsPaused(engine.client->ThisPtr()) || engine.Con_IsVisible(engine.client->ThisPtr());
	s.flags = g_levelLive ? kSkyInGame : kSkyLoading;
	if (paused) s.flags |= kSkyMenuOpen;
	// Chell mode: Minecraft parks its player (the "loading" hold) until the next switch.
	if (g_mode == Mode::Chell && !g_wantMc && !g_terrainMode && !StageActive()) s.flags |= kSkyLoading;
	s.worldId = g_worldId;
	s.transitAck = McTransitAck();  // (bridge #28) the look below includes the turn of every Minecraft transit up to this one
	s.posX = mx, s.posY = my, s.posZ = mz;
	{
		// Minecraft's yaw is continuous (it is never wrapped); a wrapped yaw jumps 360 degrees where Minecraft's north
		// (+/-180) is, and Minecraft eases its hand sway and camera across that jump: the "jolt when looking north"
		// (lead, 2026-10-02). Send the wrapped yaw nearest the last one sent.
		static float s_lastYaw = 0.0f;
		static bool s_haveYaw = false;
		float y = SrcYawToMc(va.y);
		if (s_haveYaw) y = s_lastYaw + std::remainder(y - s_lastYaw, 360.0f);
		s_lastYaw = y, s_haveYaw = true;
		s.yaw = y;
	}
	s.pitch = va.x;
	s.teleportSeq = g_teleportSeq;
	int w = 0, h = 0;
	engine.GetScreenSize(engine.client->ThisPtr(), w, h);
	// Portalcraft: the composite camera is overscanned (wider FOV), so Minecraft renders
	// proportionally more pixels to keep the same detail per screen pixel.
	// (fitted to the composite's textures; the HUD image is this size too in Minecraft mode, and is scaled to the screen)
	int fw = 0, fh = 0;
	McFrameSize(w, h, CompositeOverscan(), fw, fh);
	s.viewportW = fw, s.viewportH = fh;
	g_frameReqW = fw, g_frameReqH = fh;
	s.gameHour = 12;
	// (bridge #34) the hand-over flag is written BEFORE the SkyState that carries the new teleport: the mod reads them separately, and a client tick that
	// saw the new teleport with the previous frame's flag (still 0) released its hold at once onto the static floor under the car (Minecraft then fell out
	// of the rising arrival car: the window was the rest of this frame between the two writes, a few ms of every 50 ms client tick = "only sometimes")
	g_handOverNow = HandOverFlag();
	{
		HostPlayer e = g_hpLast;
		e.flags = (e.flags & ~(uint32_t)kHpHandOver) | (g_handOverNow ? (uint32_t)kHpHandOver : 0u);
		link::WriteHostPlayer(e);
	}
	link::WriteSkyState(s);
	{
		// (2026-10-12) Minecraft paces its frames to these writes; on Windows it sat at ~25 fps waiting ~27 ms for the next one while Portal 2
		// drew ~280 fps in the region: how often they really happen, every 5 s, with the region flag
		static int writes = 0;
		static auto t0 = std::chrono::steady_clock::now();
		++writes;
		const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		if (el >= 5.0) {
			console->Print("[bridge] link: %d SkyState writes in %.1f s (%.0f a second), Minecraft player %d\n", writes, el, writes / el, (int)McActive());
			writes = 0, t0 = std::chrono::steady_clock::now();
		}
	}
	FSTOP(7);

	{
		PROF("mc.terrain");
		TerrainConsume(6.0);
	}
	{
		PROF("mc.chellproxy");
		ChellProxyFrame();
	}
	{
		PROF("mc.audio");
		AudioFrame(paused);
	}
	FSTOP(8);
	PumpInput(McActive() && !paused);
	// Minecraft screens and the hotbar wheel (the SDL event filter), the HUD request and the overlay's state (glhud.cpp)
	const bool screen = McActive() && (g_mc.flags & kMcScreenOpen);
	g_fMc = McActive() && !paused && g_levelLive ? 1 : 0;
	g_fScreen = screen ? 1 : 0;
	ScreenInput(McActive() && !paused, screen, s.viewportW, s.viewportH);
	FSTOP(9);
	const char *hudWhy = "";
	const bool hudShowNow = McActive() && HudShouldShow(paused, hudWhy);
	if (McActive()) HudLogChange(hudShowNow, hudWhy);
	{
		HostPlayer hp{};
		// (the HUD is asked for while a switch is pending too, so the switch frame has Minecraft's hand ready)
		// (bridge #56: asked for while the rule says show, and while a switch is pending so the switch frame has Minecraft's hand ready)
		hp.flags = (McActive() ? kHpMcMode : 0u) | ((McActive() ? hudShowNow : g_wantMc) && GlHudEnabled() ? kHpWantHud : 0u);
		if (McActive()) hp.flags |= kHpMcTransit;  // (bridge #28) Minecraft transits its player itself
		if (g_handOverNow) hp.flags |= kHpHandOver;  // (bridge #31) Minecraft stays on the body until the movers around it are in (bridge #34: computed before the SkyState)
		hp.flags |= AvatarHostFlags();  // bridge #16
		DigHostFill(hp);  // bridge #33: kHpDig, the dig world and the surface under the crosshair (dig.cpp; inert unless pc_dig 1)
		StageHostFill(hp);  // bridge #47: kHpStage and the window's chunk (stage.cpp; inert until the portal onto Minecraft is used)
		hp.screenW = (uint32_t)w, hp.screenH = (uint32_t)h;
		hp.cursorX = g_cursorX, hp.cursorY = g_cursorY;
		link::WriteHostPlayer(hp);
		g_hpLast = hp;
	}
	GlHudSetState(hudShowNow, g_wantMc && g_mode == Mode::Chell, screen, g_cursorX, g_cursorY);
	FSTOP(10);
	{  // (bridge #63) Portal 2's screen fade (env_fade) is drawn over the Minecraft hand/HUD too: it is faded the same way (glhud.cpp)
		float fa = 0, fc[3];
		FadeNow(fa, fc);
		GlHudSetFade(fa, fc[0], fc[1], fc[2]);
	}
	// terrain mode: Portal 2's collision (the chambers) is exported only for the Minecraft player (outside the region
	// box; Minecraft's own world is the ground inside it)
	CollisionFieldTerrainOnly(g_terrainMode);
	CollisionFieldStage(StageActive());  // (bridge #47/#49: nothing of Portal 2's is exported inside the virtual region either)
	FSTOP(11);
	// (around the body while Minecraft is on its way to it: a level start, a transit, a ride; Minecraft's hold waits for it)
	// (bridge #22: and in Chell mode while Minecraft's mobs/items are in the chambers or crossing the pair: they stand on the export)
	PROF("mc.colfield");
	if (g_levelLive && up) {
		const bool exportOn = !g_terrainMode || McActive() || g_wantMc || SceneChamberEntities();
		const Vector focus = McActive() && !g_awaitTeleport && !g_takeover ? FeetSrc() : feet;
		CollisionFieldFrame(focus, exportOn);
		void MoversSetPortal2MovesBody(bool on);
		bool PlayerNoclip();  // puppet.cpp
		MoversSetPortal2MovesBody(g_levelLive && g_bodyKnown && !PlayerNoclip() && (!McActive() || g_awaitTeleport) && !g_takeover);
		MoversFrame(focus, exportOn);  // (bridge #26: the movers' shapes, binds and poses; only the shapes while not exported)
	}

	// (bridge #40, instrumentation) the first seconds of a level start: the body, Minecraft's feet and the mover it rides, per frame
	if (g_levelLive && (g_wantMc || McActive()) && link::NowNs() * 1e-9 - g_levelLiveAt < 8.0 && g_arrDbg) {
		double ax = 0, ay = 0, az = 0, rx = 0, ry = 0, rz = 0;
		const bool haveA = g_haveMc && g_mc.rideMover && MoverAnchorMc(g_mc.rideMover, ax, ay, az), haveR = g_haveMc && g_mc.rideMover && MoverRenderAnchorMc(g_mc.rideMover, rx, ry, rz);
		console->Print("[bridge] arrdbg t %.3f mode %d want %d await %d ho %d body %.1f mcprev %.1f mccur %.1f a %.2f feet %.1f ride %u srv %.1f rnd %.1f ridePrev %.1f rideCur %.1f ack %u/%u ground %d\n",
		               link::NowNs() * 1e-9 - g_levelLiveAt, (int)g_mode, g_wantMc, g_awaitTeleport, g_handOverNow, PlayerOrigin().z, g_haveMc ? McToSrc(g_mc.prevX, g_mc.prevY, g_mc.prevZ).z : 0.0f,
		               g_haveMc ? McToSrc(g_mc.curX, g_mc.curY, g_mc.curZ).z : 0.0f, Alpha(), g_haveMc ? FeetSrc().z : 0.0f, g_haveMc ? g_mc.rideMover : 0, haveA ? McToSrc(ax, ay, az).z : 0.0f,
		               haveR ? McToSrc(rx, ry, rz).z : 0.0f, g_haveMc ? McToSrc(0, g_mc.ridePrevY, 0).z : 0.0f, g_haveMc ? McToSrc(0, g_mc.rideCurY, 0).z : 0.0f, g_mc.teleportAck, g_teleportSeq,
		               g_haveMc && (g_mc.flags & kMcOnGround) ? 1 : 0);
	}
	if (g_levelLive && g_arrDbg) {
		static double s_lastDump = -1;
		const double age = link::NowNs() * 1e-9 - g_levelLiveAt;
		void MoversDebugDump(const Vector &feet, float r);  // movers.cpp
		if (age > 0.3 && age < 2.6 && age - s_lastDump > 0.25) {
			s_lastDump = age;
			console->Print("[bridge] arrdbg movers at t %.3f (ho %d, body %.1f):\n", age, g_handOverNow, PlayerOrigin().z);
			MoversDebugDump(PlayerOrigin(), 250.0f);
		}
	}
	if (g_levelLive && g_arrDbg && g_arrShots) {
		static int s_shotIdx = 0, s_lastSlot = -1;
		const double age = link::NowNs() * 1e-9 - g_levelLiveAt;
		const int slot = (int)(age * 12.5);  // one every 80 ms between 0.6 s and 3 s
		if (age > 0.6 && age < 3.0 && slot != s_lastSlot) {
			s_lastSlot = slot;
			char cmd[96];
			std::snprintf(cmd, sizeof cmd, "pcb_glhud_shot s40_%02d_%s_%03d", s_shotIdx++, g_mode == Mode::Minecraft ? "mc" : "ch", (int)(age * 100));
			engine.Cmd(cmd);
		}
	}
	if (g_logFrames > 0 && g_haveMc) {
		--g_logFrames;
		Vector f = FeetSrc();
		console->Print("[bridge] mc flags 0x%x feet mc (%.3f %.3f %.3f) src (%.1f %.1f %.1f) yaw %.1f pitch %.1f fov %.1f ack %u/%u frame %llu\n", g_mc.flags, g_mc.x, g_mc.y,
		               g_mc.z, f.x, f.y, f.z, g_mc.yaw, g_mc.pitch, g_mc.fovDeg, g_mc.teleportAck, g_teleportSeq, (unsigned long long)g_mc.frameCounter);
	}
}

// ---- ClientModeShared hooks ------------------------------------------------------------------
static Interface *g_clientMode = nullptr;
using _OverrideView = void(__rescall *)(void *thisptr, CViewSetup *view);
using _CreateMove = bool(__rescall *)(void *thisptr, float sampleTime, CUserCmd *cmd);
static _OverrideView OverrideView = nullptr;
static _CreateMove CreateMove = nullptr;

// ---- pcb_eye_watch (bridge #20): the main view's eye outside Portal 2's world (the screen clears to white) or in
// solid, per frame, while the Minecraft player is (or is about to be) the player. The user's "the world goes white"
// after a chamber-to-chamber portal transit as the Minecraft player.
static int g_ewFrames = 0, g_ewOut = 0, g_ewSolid = 0, g_ewLogged = 0, g_ewTrace = 0;
static Vector g_ewLastBad;
static bool g_ewOn = false;  // (tests only: two traces a frame) on from the first pcb_eye_watch
static void EyeWatch(const CViewSetup *view, const char *who) {
	if (!g_ewOn || !g_levelLive || !(McActive() || g_wantMc || g_awaitTeleport)) return;
	++g_ewFrames;
	const bool out = engine.PointOutsideWorld(engine.traceServer->ThisPtr(), view->origin);
	CGameTrace tr;
	engine.Trace(view->origin, view->origin, Vector(0, 0, 0), MASK_SOLID_BRUSHONLY, tr, true);
	const bool solid = tr.startsolid;
	if (out) ++g_ewOut;
	if (solid) ++g_ewSolid;
	if (out || solid) g_ewLastBad = view->origin;
	if (((out || solid) && g_ewLogged < 60) || g_ewTrace > 0) {
		if (out || solid) ++g_ewLogged;
		if (g_ewTrace > 0) --g_ewTrace;
		const Vector body = PlayerOrigin();
		console->Print("[bridge] eye watch%s: eye (%.1f %.1f %.1f) %s%s by %s; body (%.1f %.1f %.1f); mc prev (%.2f %.2f %.2f) cur (%.2f %.2f %.2f) a %.2f; await %d takeover %d ack %u/%u\n",
		               out || solid ? " BAD" : "", view->origin.x, view->origin.y, view->origin.z, out ? "OUTSIDE " : "", solid ? "SOLID " : "", who, body.x, body.y, body.z, g_mc.prevX,
		               g_mc.prevY, g_mc.prevZ, g_mc.curX, g_mc.curY, g_mc.curZ, Alpha(), g_awaitTeleport, g_takeover, g_mc.teleportAck, g_teleportSeq);
	}
}
CON_COMMAND(pcb_eye_watch, "pcb_eye_watch reset | off | trace <frames> | assert <max_bad_frames> - the main view's eye outside Portal 2's world (white screen) or in solid, as the Minecraft player\n") {
	if (args.ArgC() > 1 && !std::strcmp(args[1], "trace")) return void(g_ewTrace = args.ArgC() > 2 ? std::atoi(args[2]) : 60);
	if (args.ArgC() > 1 && !std::strcmp(args[1], "assert")) {
		const int mx = args.ArgC() > 2 ? std::atoi(args[2]) : 0;
		char buf[220];
		std::snprintf(buf, sizeof buf, "%d frames watched: eye outside the world %d, in solid %d (want <= %d), last bad at (%.1f %.1f %.1f)", g_ewFrames, g_ewOut, g_ewSolid, mx, g_ewLastBad.x,
		              g_ewLastBad.y, g_ewLastBad.z);
		void TestCheck(bool ok, const char *what, const char *detail);
		return TestCheck(g_ewFrames > 0 && g_ewOut + g_ewSolid <= mx, "eye inside Portal 2's world", buf);
	}
	g_ewFrames = g_ewOut = g_ewSolid = g_ewLogged = 0;
	g_ewOn = !(args.ArgC() > 1 && !std::strcmp(args[1], "off"));
}

// (bridge #28) the camera through a portal and pcb_view_track are in mctransit.cpp
void McTransitCamera(CViewSetup *view, const Vector &eye, const Vector &eye0, const Vector &eye1);  // mctransit.cpp
void ViewTrackFrame(const CViewSetup *view);                                                        // mctransit.cpp

void VisClampFrame(const Vector &eye, bool live);  // visclamp.cpp
bool VisClampInit();
void VisClampShutdown();

int McTransits();  // mctransit.cpp
extern bool g_mcCarryUse;  // mctransit.cpp (bridge #66)

// ---- (bridge #70) the view redirect's rigid transform ----
struct Mat3 {
	float m[3][3];
};
static Mat3 MatId() { return Mat3{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}; }
// local -> world for Source angles (pitch about y, yaw about z, roll about x; x forward, y left, z up)
static Mat3 AngMat(const QAngle &a) {
	const float d = (float)M_PI / 180.0f;
	const float sp = std::sin(a.x * d), cp = std::cos(a.x * d), sy = std::sin(a.y * d), cy = std::cos(a.y * d), sr = std::sin(a.z * d), cr = std::cos(a.z * d);
	Mat3 r;
	r.m[0][0] = cp * cy, r.m[0][1] = sr * sp * cy - cr * sy, r.m[0][2] = cr * sp * cy + sr * sy;
	r.m[1][0] = cp * sy, r.m[1][1] = sr * sp * sy + cr * cy, r.m[1][2] = cr * sp * sy - sr * cy;
	r.m[2][0] = -sp, r.m[2][1] = sr * cp, r.m[2][2] = cr * cp;
	return r;
}
static Mat3 MatMulT(const Mat3 &a, const Mat3 &b) {  // a * transpose(b)
	Mat3 r;
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[i][0] * b.m[j][0] + a.m[i][1] * b.m[j][1] + a.m[i][2] * b.m[j][2];
	return r;
}
static Vector MatVec(const Mat3 &a, const Vector &v) {
	return Vector(a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z, a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z, a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z);
}
// the rotation's angle in degrees; 999 when the matrix is not finite (angles of an unset view)
static float MatAngleDeg(const Mat3 &a) {
	const float tr = a.m[0][0] + a.m[1][1] + a.m[2][2];
	if (!(tr == tr) || std::fabs(tr) > 3.5f) return 999.0f;
	return std::acos(std::fmax(-1.0f, std::fmin(1.0f, (tr - 1.0f) * 0.5f))) * 180.0f / (float)M_PI;
}
static struct {
	bool active = false;
	Mat3 r = MatId();
	Vector t{0, 0, 0};
} g_redirect;
// avatar.cpp: where the redirect puts what is drawn at the Minecraft player's place (p' = R p + t) this frame; false: no redirect
bool McViewRedirect(Vector &t, float r[3][3]) {
	if (!g_redirect.active) return false;
	t = g_redirect.t;
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j) r[i][j] = g_redirect.r.m[i][j];
	return true;
}
bool BridgeSkip(const char *name);  // plugin.cpp
static void __rescall OverrideView_Hook(void *thisptr, CViewSetup *view) {
	OverrideView(thisptr, view);
	static const bool skipFrame = BridgeSkip("mc.frame");
	if (skipFrame) return;
	Frame();
	const bool mcView = McActive() && !g_awaitTeleport;
	// (bridge #40) the hand-over of the camera: Portal 2's eye (the body: the car where the server has it) to Minecraft's (the car as Portal 2 draws it, a
	// cl_interp behind: 30 u lower on a rising car) is a step the user sees as the view dropping into the floor; the step is eased out over 0.35 s
	// ...and the pending switch draws that camera in advance (the step is then only the rest)
	if (!mcView && g_levelLive && (McActive() || (g_wantMc && g_mode == Mode::Chell))) {
		bool MoverUnderFeetLag(const Vector &feet, Vector &lag);  // movers.cpp
		Vector lag;
		static Vector s_lag(0, 0, 0);
		if (MoverUnderFeetLag(PlayerOrigin(), lag)) s_lag = lag;
		else s_lag = s_lag * 0.8f;
		view->origin = view->origin - s_lag;
	}
	static bool s_prevMcView = false;
	static Vector s_lastP2Eye(0, 0, 0), s_camDelta(0, 0, 0);
	static double s_camT0 = -1;
	const Vector p2Eye = view->origin;
	const QAngle p2Ang = view->angles;
	g_redirect.active = false;
	if (mcView) {
		// CameraDriver: Minecraft's eye (interpolated feet + its smoothed eye height), Portal 2's
		// look (the mouse), Minecraft's vertical FOV as the 4:3 horizontal FOV Source expects here.
		// Applied before the composite predicts its cameras (bridge #13): every camera the composite asks Minecraft
		// for is then Minecraft's own eye, the one drawn (it used Chell's body eye, a few units off: the "fight").
		double a = Alpha();
		float eye = g_mc.tickEyeO + (g_mc.tickEye - g_mc.tickEyeO) * (float)(a > 1 ? 1 : a);
		// (bridge #14: only a sane player FOV; a mod that reported its host-driven camera's FOV fed the composite's
		// request FOV x overscan back in here every frame, up to 180 degrees: the "stretched, broken" switch)
		if (g_mc.fovDeg >= 30.0f && g_mc.fovDeg <= 120.0f) view->fov = 2.0f * std::atan(std::tan(g_mc.fovDeg * (float)M_PI / 360.0f) * 4.0f / 3.0f) * 180.0f / (float)M_PI;
		// (bridge #28) the eye on the latest tick's line; drawn through the pair while it is behind a portal's plane on that line
		// (the frames of a transit's tick still behind the exit, or an extrapolated frame past the entry): continuous, no blend
		McTransitCamera(view, FeetRawSrc(false) + Vector(0, 0, eye * kUnitsPerBlock), McToSrc(g_mc.prevX, g_mc.prevY, g_mc.prevZ) + Vector(0, 0, g_mc.tickEyeO * kUnitsPerBlock),
		                McToSrc(g_mc.curX, g_mc.curY, g_mc.curZ) + Vector(0, 0, g_mc.tickEye * kUnitsPerBlock));
		// (lead, reworked in bridge #70) a view redirect (point_viewproxy and the like: sp_a1_intro1's vault after the bed, the pods): the body stands in a
		// blockout box and Portal 2 draws the view from somewhere else. Portal 2 computed that view from the CLIENT's predicted body (the proxy's target
		// transform applied to the body's eye relative to the reference; the client's interpolated entities, the same frame the vault is drawn in), so the
		// redirect is a rigid transform M, measured as "Portal 2's view" against "Portal 2's own eye before the redirect" of the same prediction:
		//   M(p) = R (p - E) + E + gap, E = the client body's eye, gap = Portal 2's view origin - E, R = Portal 2's view angles composed with the inverse of
		//   the player's own view angles (the proxy's rotation: the vault turns while Wheatley carries it).
		// It was measured against the SERVER body (up to a tick older than the client's, PlayerOrigin()): out of phase by the walking speed x the tick phase,
		// every frame, applied to the camera: the "jittery, blurry room" only when moving with WASD (static it was exact). The client body is what the
		// puppet's client move wrote (puppet.cpp PuppetClientBody); the eye height is the server player's m_vecViewOffset (64 standing, 28 ducked). The
		// fallback (no client move this frame: Portal 2 carries the body) uses the server body, low-passed (0.12 s).
		const Vector mcEye0 = view->origin;  // Minecraft's eye before the redirect
		bool redirOn = false;
		Mat3 redirR = MatId();
		{
			static bool s_redir = false;
			static Vector s_gapLP(0, 0, 0);
			static double s_gapLPAt = 0;
			float vo = PuppetViewOffsetZ();
			if (vo < 0) vo = (g_mc.flags & kMcSneaking) ? 28.0f : 64.0f;
			Vector clientBody;
			double bodyAge = 1e9;
			const bool haveClient = PuppetClientBody(clientBody, bodyAge) && bodyAge < 0.1 && !g_takeover;
			const Vector bodyEye = (haveClient ? clientBody : PlayerOrigin()) + Vector(0, 0, vo);
			const Vector redir = p2Eye - bodyEye;
			// (bridge #66) Not for ~0.8 s after the body jumped (a Minecraft transit through a portal pair, a teleport): the client's view of the body lags the jump by
			// a few frames, at the old place by the pair's offset, which read as a "redirect" when the portals are > 128 u apart (map-placed pairs are, in other
			// rooms) and drew the camera back at the entry for two frames and forth again: the flash on walking through.
			static Vector s_prevBody(0, 0, 0);
			static double s_jumpUntil = 0;
			static int s_prevTransits = -1;
			const Vector body = PlayerOrigin();
			const double nowR = link::NowNs() * 1e-9;
			bool nearPortal = false;
			for (int k = 0; k < kPortalSlots; ++k) nearPortal |= PortalLinked(k) && (g_portalPos[k] - (s_prevBody + Vector(0, 0, 36))).Length() < 150.0f;
			if (((body - s_prevBody).Length() > 96.0f && nearPortal) || (s_prevTransits >= 0 && McTransits() != s_prevTransits)) s_jumpUntil = nowR + 0.8;
			s_prevBody = body, s_prevTransits = McTransits();
			const bool on = g_levelLive && redir.Length() > 128.0f && nowR > s_jumpUntil;
			Vector gap = redir;
			Mat3 R = MatId();
			float rotDeg = 0.0f;
			if (on) {
				if (haveClient || !s_redir) s_gapLP = redir;
				else s_gapLP = s_gapLP + (redir - s_gapLP) * (1.0f - (float)std::exp(-std::fmin(0.1, std::fmax(0.0, nowR - s_gapLPAt)) / 0.12));
				s_gapLPAt = nowR;
				if (!haveClient) gap = s_gapLP;
				const Mat3 r = MatMulT(AngMat(p2Ang), AngMat(engine.ViewAngles()));
				rotDeg = MatAngleDeg(r);
				if (rotDeg >= 1.0f && rotDeg < 170.0f) R = r;  // (below 1 degree: Portal 2's view angles and the player's differ by a frame of mouse, not a turn)
				else rotDeg = 0.0f;
				view->origin = bodyEye + gap + MatVec(R, view->origin - bodyEye);
			}
			redirOn = on, redirR = R;
			if (on != s_redir)
				console->Print("[bridge] camera: Portal 2's view is %s (%.0f %.0f %.0f u from the body's eye, measured against the %s body, eye height %.0f, view turned %.1f deg): Minecraft's eye %s\n",
				               on ? "redirected" : "back on the body", redir.x, redir.y, redir.z, haveClient ? "client" : "server", vo, rotDeg, on ? "follows the redirect" : "is its own again");
			s_redir = on;
		}
		const double now = link::NowNs() * 1e-9;
		if (!s_prevMcView) {
			const Vector d = s_lastP2Eye - view->origin;
			// (only the hand-over of a body Portal 2 was moving: within a few dozen units; a portal transit or a teleport is a cut)
			s_camDelta = d.Length() < 64.0f && g_levelLive ? d : Vector(0, 0, 0);
			s_camT0 = now;
			if (s_camDelta.Length() > 1.0f)
				console->Print("[bridge] camera: hand-over step from Portal 2's eye to Minecraft's: (%.1f %.1f %.1f) u, eased out over 0.35 s\n", s_camDelta.x, s_camDelta.y, s_camDelta.z);
		}
		const double k = 1.0 - (now - s_camT0) / 0.35;
		if (k > 0.0 && s_camDelta.Length() > 0.0f) view->origin = view->origin + s_camDelta * (float)(k * k);
		// (bridge #70) the redirect as the final camera has it, for what is drawn at the Minecraft player's place (avatar.cpp): M(p) = R p + t maps Minecraft's
		// eye onto the camera exactly (the hand-over ease included)
		if (redirOn) g_redirect.active = true, g_redirect.r = redirR, g_redirect.t = view->origin - MatVec(redirR, mcEye0);
	}
	s_prevMcView = mcView;
	if (!mcView) s_lastP2Eye = p2Eye;
	FloatOriginViewFrame(view->origin);
	TransitFrame(view);  // test runs: camera aimed at a portal (transit.cpp)
	AvatarCameraFrame(view);  // bridge #16: F5 third person (before the composite predicts its cameras)
	CompositeFrameBegin(view);
	McLightFrame(view->origin);  // mclight.cpp: Minecraft's light at the eye and the region's models
	EyeWatch(view, mcView ? "minecraft" : "portal2");
	ViewTrackFrame(view);
	VisClampFrame(view->origin, g_levelLive);  // bridge #41: the world stays drawn with the camera in a tunnel beyond the map (visclamp.cpp)
}

static bool __rescall CreateMove_Hook(void *thisptr, float sampleTime, CUserCmd *cmd) {
	bool r = CreateMove(thisptr, sampleTime, cmd);
	// (bridge #71) Minecraft is wanted but does not lead yet (the pending switch, the debounce after a sequence, the teleport / level-change / respawn waits):
	// the user's keys must not walk Chell away from where Minecraft will be released. Move axes, jump and duck off; the mouse look and Portal 2's own
	// scripted movement stay (the gun and use buttons too). When Minecraft leads, the keys still held reach it at once (PumpInput sends the state).
	if (cmd && !McActive() && g_inputBlock) {
		cmd->forwardmove = cmd->sidemove = cmd->upmove = 0;
		cmd->buttons &= ~((1 << 1) | (1 << 2));  // IN_JUMP, IN_DUCK
	}
	if (McActive() && cmd) {
		// Minecraft has the controls: Chell's body only keeps the look direction (and G, Portal 2's use).
		cmd->buttons = (g_useHeld || g_mcCarryUse) ? (1 << 5) /* IN_USE */ : 0;  // (bridge #66: g_mcCarryUse, mctransit.cpp)
		cmd->forwardmove = cmd->sidemove = cmd->upmove = 0;
		cmd->impulse = 0;
		// (bridge #65) Portal 2's own logic that tests the player's buttons (logic_playerproxy OnJump / OnDuck / OnUnDuck: the sp_a1_intro1 jump and crouch
		// tutorial) sees them from the usercmd, which is zeroed above. While Minecraft leads (the puppet: Apply() overwrites the body's origin and velocity after
		// every server move, so the body's own jump or duck moves nothing), the buttons are relayed from Minecraft's state: IN_JUMP for one command on each take-off
		// (jump key down while Minecraft is on the ground; Portal 2 needs the body on the ground at that command, which it is before Minecraft leaves it), IN_DUCK for as
		// long as Minecraft sneaks. Not while Portal 2 carries the body (the takeover block below sends the real keys) or waits for a teleport.
		if (!g_takeover && !g_awaitTeleport && g_levelLive && !(g_mc.flags & kMcScreenOpen)) {
			static bool s_jumpPrev = false, s_duckPrev = false;
			int n = 0;
			const uint8_t *ks = SDL_GetKeyboardState_ ? SDL_GetKeyboardState_(&n) : nullptr;
			const bool focus = !SDL_GetKeyboardFocus_ || SDL_GetKeyboardFocus_() != nullptr;
			const bool space = (focus && ks && 44 < n && ks[44]) || g_scriptKeys[44];
			const bool jumpNow = space && (g_mc.flags & kMcOnGround);
			// (bridge #70) IN_JUMP is HELD for 0.15 s from the take-off, not sent for one command: the engine makes a usercmd every frame but only the one at
			// the network send is kept (the next frame writes the same slot again), so a one-frame pulse reached Portal 2's movement about one time in three
			// (the log: 12 "relayed" lines in the apple scene, no jump check on the server). Held, it is in the sent command; the game takes the press once
			// (no repeat while held), and playerevents.cpp fires the map's OnJump when it still did not arrive.
			static double s_jumpHoldUntil = 0;
			const double nowJ = link::NowNs() * 1e-9;
			if (jumpNow && !s_jumpPrev) {
				s_jumpHoldUntil = nowJ + 0.15;
				PlayerEventsJumpRelayed();
				static int s_log = 0;
				if (++s_log <= 12) console->Print("[bridge] mode: jump relayed to Portal 2 (IN_JUMP held 0.15 s)\n");
			}
			if (nowJ < s_jumpHoldUntil) cmd->buttons |= (1 << 1);  // IN_JUMP
			s_jumpPrev = jumpNow;
			const bool duckNow = (g_mc.flags & kMcSneaking) != 0;
			PlayerEventsSneak(duckNow);
			if (duckNow) cmd->buttons |= (1 << 2);  // IN_DUCK
			if (duckNow != s_duckPrev) console->Print("[bridge] mode: %s relayed to Portal 2 (IN_DUCK)\n", duckNow ? "sneak" : "stand");
			s_duckPrev = duckNow;
		}
		// (bridge #20) while Portal 2 carries the body (a moving solid under it, the collision on its way: TakeoverFrame), the
		// player's own walk goes into Portal 2's move, so the body keeps walking instead of sliding to a stop (the user:
		// "not as smooth as Chell"; c20_smooth1m: the camera fell to 44% speed after a transit next to a moving panel)
		if (g_takeover && !(g_mc.flags & kMcScreenOpen)) {
			auto key = [](int sc) {
				int n = 0;
				const uint8_t *ks = SDL_GetKeyboardState_ ? SDL_GetKeyboardState_(&n) : nullptr;
				const bool focus = !SDL_GetKeyboardFocus_ || SDL_GetKeyboardFocus_() != nullptr;
				return (focus && ks && sc < n && ks[sc]) || g_scriptKeys[sc];
			};
			const float f = (key(26) ? 1.0f : 0.0f) - (key(22) ? 1.0f : 0.0f), sd = (key(7) ? 1.0f : 0.0f) - (key(4) ? 1.0f : 0.0f);
			cmd->forwardmove = 175.0f * f, cmd->sidemove = 175.0f * sd;
			if (key(44)) cmd->buttons |= (1 << 1);   // IN_JUMP
			if (key(225)) cmd->buttons |= (1 << 2);  // IN_DUCK
		}
	}
	return r;
}

bool BridgeSkip(const char *name);  // plugin.cpp
bool McInit() {
	auto clientDll = Interface::Create(MODULE_NAME("client"), "VClient016", false);
	if (!clientDll) return false;
	// SourceAutoRecord's route to the client mode: HudProcessInput -> GetClientMode -> g_pClientMode.
	auto HudProcessInput = clientDll->Original(Offsets::HudProcessInput);
	auto GetClientMode = Memory::Read<uintptr_t>(HudProcessInput + Offsets::GetClientMode);
	auto g_pClientMode = Memory::Deref<uintptr_t>(GetClientMode + Offsets::g_pClientMode);
	void *clientMode = Memory::Deref<void *>(g_pClientMode);
	Interface::Delete(clientDll);
	g_clientMode = Interface::Create(clientMode);
	if (!g_clientMode) return false;
	if (!BridgeSkip("mc.hooks")) {
		if (!BridgeSkip("mc.overrideview")) g_clientMode->Hook(OverrideView_Hook, OverrideView, Offsets::OverrideView);
		if (!BridgeSkip("mc.createmove")) g_clientMode->Hook(CreateMove_Hook, CreateMove, Offsets::CreateMove);
	}
	PuppetSetProvider(PuppetProvider);
	if (!BridgeSkip("mc.link") && !link::Create()) console->Warn("[bridge] link unavailable: Minecraft mode disabled\n");
	if (!BridgeSkip("mc.hud") && !GlHudInit()) console->Warn("[bridge] direct-GL overlay unavailable: no Minecraft HUD (continuing)\n");
	if (!BridgeSkip("mc.filter")) InstallFilter();
	if (!BridgeSkip("mc.visclamp")) VisClampInit();
	return true;
}

// pcb_vtdump <object> [from=0] [to=40]: the genuine vtable entries of an engine object (module+offset, first 16 bytes, the first `ret imm16`
// within 700 bytes = the callee-cleaned argument bytes, and the function's length up to its int3 padding). Windows slot hunting without a
// disassembler: a getter is a few bytes with no ret imm, TracePlayerBBox is `ret 0x14`. Objects: clientmode, movement (server), movementclient,
// or an interface "<module>/<version>", e.g. engine/VEngineClient015.
CON_COMMAND(pcb_vtdump, "pcb_vtdump <clientmode|movement|movementclient|module/Version> [from=0] [to=40] - an object's original vtable, slot by slot\n") {
	if (args.ArgC() < 2) return console->Print("%s", pcb_vtdump.ThisPtr()->m_pszHelpString);
	const std::string which = args[1];
	uintptr_t *vt = nullptr;
	int size = 0;
	Interface *tmp = nullptr;
	if (which == "clientmode") {
		if (g_clientMode) vt = g_clientMode->vtable, size = g_clientMode->vtableSize;
	} else if (which.rfind("ent/", 0) == 0) {  // ent/<classname>: the first server entity of that class (a map must be running)
		std::vector<void *> v = EntFind(which.substr(4).c_str(), nullptr);
		if (!v.empty() && v[0]) {
			vt = *reinterpret_cast<uintptr_t **>(v[0]);
			if (uintptr_t *orig = Interface::OriginalVtable(v[0])) vt = orig;
			size = 400;
		}
	} else {
		std::string mod, ver;
		if (which == "movement") mod = MODULE_NAME("server"), ver = "GameMovement001";
		else if (which == "movementclient") mod = MODULE_NAME("client"), ver = "GameMovement001";
		else {
			size_t c = which.find('/');
			if (c != std::string::npos) mod = which.substr(0, c) + MODULE_EXTENSION, ver = which.substr(c + 1);
		}
		if (!mod.empty()) tmp = Interface::Create(mod.c_str(), ver.c_str(), false);
		if (tmp) vt = tmp->vtable, size = tmp->vtableSize;
	}
	if (!vt) return console->Print("[bridge] vtdump: no such object\n");
	int from = args.ArgC() > 2 ? std::atoi(args[2]) : 0, to = args.ArgC() > 3 ? std::atoi(args[3]) : 40;
	for (int i = from; i <= to && i < size; ++i) {
		uintptr_t e = vt[i];
		plat::AddressInfo ai;
		plat::DescribeAddress((const void *)e, ai);
		uint8_t b[700] = {};
		plat::SafeRead((const void *)e, b, sizeof b);
		char hex[64] = "";
		for (int k = 0; k < 16; ++k) std::snprintf(hex + k * 3, 4, "%02x ", b[k]);
		int len = -1, ret = -1;
		for (int k = 0; k < 698; ++k) {
			if (ret < 0 && b[k] == 0xc2 && b[k + 2] == 0) ret = b[k + 1];
			if (b[k] == 0xcc && b[k + 1] == 0xcc) { len = k; break; }
		}
		// a forwarding thunk (engine.dll's IVRenderView: `mov ecx,[global]; mov eax,[ecx]; mov edx,[eax+d]; jmp edx`): follow it to the real method
		char fol[160] = "";
		for (int k = 0; k + 12 < 24; ++k) {
			if (b[k] != 0x8b || b[k + 1] != 0x0d || b[k + 6] != 0x8b || b[k + 7] != 0x01 || b[k + 8] != 0x8b) continue;
			const uint8_t modrm = b[k + 9];
			int disp = -1;
			if ((modrm & 0xc0) == 0x40) disp = b[k + 10];
			else if ((modrm & 0xc0) == 0x80) disp = *(const int *)&b[k + 10];
			else if ((modrm & 0xc0) == 0x00) disp = 0;
			uintptr_t obj = 0, tvt = 0, tgt = 0;
			if (disp < 0 || !plat::SafeRead(*(const void *const *)&b[k + 2], &obj, 4) || !plat::SafeRead((const void *)obj, &tvt, 4) || !plat::SafeRead((const void *)(tvt + disp), &tgt, 4)) break;
			uint8_t tb[700] = {};
			plat::SafeRead((const void *)tgt, tb, sizeof tb);
			int tret = -1;
			for (int q = 0; q < 698; ++q) {
				if (tret < 0 && tb[q] == 0xc2 && tb[q + 2] == 0) tret = tb[q + 1];
				if (tb[q] == 0xcc && tb[q + 1] == 0xcc) break;
			}
			plat::AddressInfo ti;
			plat::DescribeAddress((const void *)tgt, ti);
			std::snprintf(fol, sizeof fol, " -> thunk to %s+0x%x (slot %d of the target) ret %d", ti.module.c_str(), (unsigned)ti.offset, disp / 4, tret);
			break;
		}
		console->Print("[bridge] vtdump %s %d: %s+0x%x  len %d ret %d  %s%s\n", which.c_str(), i, ai.module.c_str(), (unsigned)ai.offset, len, ret, hex, fol);
	}
	if (tmp) Interface::Delete(tmp);
}

void McShutdown() {
	VisClampShutdown();
	RemoveFilter();
	GlHudShutdown();  // before the link (the HUD pixels) goes
	PuppetSetProvider(nullptr);
	Interface::Delete(g_clientMode);
	g_clientMode = nullptr;
	link::Destroy();
}

void AudioLevelInit();  // audio.cpp
static char g_mapName[64] = "";
const char *McMapName() { return g_mapName; }
bool McLevelIsLive() { return g_levelLive; }
void McLevelInit(const char *map) {
	std::snprintf(g_mapName, sizeof g_mapName, "%s", map ? map : "");
	AudioLevelInit();
	uint32_t h = 2166136261u;
	for (const char *p = map; *p; ++p) h = (h ^ (uint8_t)*p) * 16777619u;
	g_worldId = h;
	g_levelLive = false;
	HudGateLevelInit();
	GlHudHideNow();  // (bridge #56) the load: no stale Minecraft frame over it
	g_hidden = false;  // a new level's player entity: hide it again (a reload goes straight to live, Frame() may never see it not live)
	CollisionFieldLevelStart();
	g_levelMoveBase = PuppetServerMoves(), g_bodyKnown = false;  // (bridge #25)
	g_handWaitSince = 0, g_handWaited = false;
	InputWatchLevelInit();
	g_scriptedKeysSince = 0, g_scriptedEndAt = g_bodyTeleportAt = g_bodyBigTeleportAt = 0, g_scriptedSince = g_scriptedLastAt = g_scriptedCamAt = 0, g_scriptedHold = false, g_scriptedCapped = false;  // (bridge #63)
	g_safeValid = g_goodValid = false, g_inputBlock = false, g_pendSince = 0, g_scriptedSlow = g_scriptedBackToBack = false, g_scriptedKind = nullptr;  // (bridge #71)
	FallGuardReset();
	PlayerEventsLevelInit();  // (bridge #70)
}

static void SetMode(bool mc);
// M1b (the user: "should be able to play the game from the start as a minecraft character"): pc_start_mode mc makes
// every level start as the Minecraft player; a level change in Minecraft mode keeps it (Portal 2's body leads until
// Minecraft has been teleported to the new spawn). Not archived: play_clip.sh --mc sets it in its temporary autoexec.
CON_COMMAND(pc_start_mode, "pc_start_mode mc|chell - start every level as the Minecraft player (mc) or as Chell\n") {
	if (args.ArgC() > 1) g_startMc = !std::strcmp(args[1], "mc");
	console->Print("[bridge] start mode: %s\n", g_startMc ? "Minecraft player" : "Chell");
}

void McLevelLive() {
	g_levelLive = true;
	g_levelLiveAt = link::NowNs() * 1e-9;
	{  // (bridge #40) arrival diagnostics: only while cache/bridge/s40_dbg exists (test runs of the arrival ride)
		struct stat st;
		g_arrDbg = ::stat((CacheDir() + "/s40_dbg").c_str(), &st) == 0;
		g_arrShots = g_arrDbg && ::stat((CacheDir() + "/s40_shots").c_str(), &st) == 0;
		g_legacyBody = ::stat((CacheDir() + "/s40_legacy").c_str(), &st) == 0;
		// (test knob) cache/bridge/s40_fps holds a frame rate cap for the level start: a slow machine's longer host frames and Minecraft ticks
		if (FILE *f = std::fopen((CacheDir() + "/s40_fps").c_str(), "r")) {
			int n = 0;
			if (std::fscanf(f, "%d", &n) == 1 && n > 0) {
				char cmd[48];
				std::snprintf(cmd, sizeof cmd, "fps_max %d", n);
				engine.Cmd(cmd);
				console->Print("[bridge] arrival test knob: %s\n", cmd);
			}
			std::fclose(f);
		}
	}
	++g_teleportSeq;  // new map: Minecraft follows Chell's spawn
	if (g_mode == Mode::Minecraft) {
		g_awaitTeleport = true;  // until Minecraft acks: the body is Portal 2's (Minecraft is still at the old map's spot)
		g_levelChangeHandOver = true;
		console->Print("[bridge] mode: Minecraft player across the level change (teleport %u)\n", g_teleportSeq);
	} else if (g_startMc) {
		SetMode(true);
	} else if (g_everHid) {
		g_restoreUntil = link::NowNs() * 1e-9 + 6.0, g_restoreNext = 0, g_restoreReport = true;  // (bridge #61) Chell mode in a new level: a hide flag may have ridden through the transition
	}
	// Sanity check of the world-only contents query the collision export relies on.
	Vector below = PlayerOrigin() - Vector(0, 0, 8);
	console->Print("[bridge] contents 8 u under the player: world-only 0x%x, all 0x%x\n", engine.GetPointContentsWorld(engine.traceServer->ThisPtr(), below, MASK_ALL),
	               engine.GetPointContents(engine.traceServer->ThisPtr(), below, MASK_ALL, nullptr));
}

static void SetMode(bool mc) {
	if (mc) {
		if (g_mode == Mode::Minecraft || g_wantMc) return;
		g_wantMc = true;
		g_wantSince = link::NowNs() * 1e-9;
		TerrainSetMapReady(true);  // mirror world: blocks the player places use the default mapping
		++g_teleportSeq;  // Minecraft player appears where Chell stands, moving as she moves
		SendVelocity(PlayerVelocity());
		const Vector f = PlayerOrigin(), v = PlayerVelocity();
		console->Print("[bridge] mode: switching to the Minecraft player (teleport %u) at Portal 2 (%.1f %.1f %.1f) velocity (%.0f %.0f %.0f)\n", g_teleportSeq, f.x, f.y, f.z, v.x, v.y, v.z);
	} else {
		g_wantMc = false;
		if (g_mode == Mode::Chell) return;
		g_mode = Mode::Chell;
		g_awaitTeleport = false;
		g_levelChangeHandOver = false;  // (bridge #69: the level change's wait ends with the mode)
		g_takeover = false;
		ReleaseAllKeys();
		HideChell(false);
		engine.Cmd("r_drawviewmodel 1");
		RestoreChellDraw();  // (bridge #61) by the entities that exist now, then again for a few seconds (EnforceChellShown)
		g_restoreUntil = link::NowNs() * 1e-9 + 4.0, g_restoreNext = 0, g_restoreReport = true;
		const Vector f = PlayerOrigin(), v = PlayerVelocity();
		console->Print("[bridge] mode: Chell at (%.1f %.1f %.1f) velocity (%.0f %.0f %.0f)\n", f.x, f.y, f.z, v.x, v.y, v.z);
	}
}

CON_COMMAND(bridge_mode, "bridge_mode mc|chell|toggle - switch between Chell and the Minecraft player (key: J)\n") {
	const char *a = args.ArgC() > 1 ? args[1] : "toggle";
	if (!std::strcmp(a, "mc")) SetMode(true);
	else if (!std::strcmp(a, "chell")) SetMode(false);
	else SetMode(!(g_mode == Mode::Minecraft || g_wantMc));
}

void TerrainClearMeshes();         // terrain.cpp
size_t TerrainSectionsKnown();      // terrain.cpp
void TerrainSetRegion(double mcX, double mcY, double mcZ, const Vector &src) {
	// A level (re)load with the same mapping keeps everything Minecraft has streamed: Minecraft sends
	// each section once per link and does not resend on a level change, so clearing here lost the
	// cliff (and Chell's terrain collision) for the rest of the session after a `map` reload. The
	// solids are keyed by Minecraft section and survive a new mapping too; only meshes move with it.
	const bool same = g_terrainMode && g_map.mcX == mcX && g_map.mcY == mcY && g_map.mcZ == mcZ && g_map.src.x == src.x && g_map.src.y == src.y && g_map.src.z == src.z;
	if (!same) TerrainClearMeshes();
	console->Print("[bridge] terrain: region mapping %s, %zu streamed sections kept\n", same ? "unchanged" : g_terrainMode ? "changed (meshes cleared)" : "set", TerrainSectionsKnown());
	g_map.mcX = mcX, g_map.mcY = mcY, g_map.mcZ = mcZ;
	g_map.src = src;
	g_terrainMode = true;
	TerrainSetMapReady(true);
	CollisionFieldLevelStart();  // a new, empty collision epoch: Minecraft drops chamber voxels
	++g_teleportSeq;  // park Minecraft's player at the anchor
	console->Print("[bridge] terrain region: MC (%.1f %.1f %.1f) at Portal 2 (%.1f %.1f %.1f)\n", g_map.mcX, g_map.mcY, g_map.mcZ, g_map.src.x, g_map.src.y, g_map.src.z);
}

// (bridge #78) The unified world (unified.cpp): a chamber's own mapping, the plain one at a fixed place of the chamber band (no region box, no terrain
// mode: the chamber is exported and the hidden player follows Chell as in the mirror world; the terrain is a window of its own, stage.cpp).
void McSetChamberMapping(double mcX, double mcY, double mcZ, const Vector &src, const char *why) {
	const bool same = !g_terrainMode && g_map.mcX == mcX && g_map.mcY == mcY && g_map.mcZ == mcZ && g_map.src.x == src.x && g_map.src.y == src.y && g_map.src.z == src.z;
	g_terrainMode = false;
	TerrainSetMapReady(true);
	if (same) return;
	TerrainClearMeshes();
	g_map.mcX = mcX, g_map.mcY = mcY, g_map.mcZ = mcZ;
	g_map.src = src;
	CollisionFieldLevelStart();
	++g_teleportSeq;
	console->Print("[bridge] world: chamber mapping (%s): Portal 2 (%.0f %.0f %.0f) is Minecraft (%.1f %.1f %.1f), %zu streamed sections kept\n", why, src.x, src.y, src.z, mcX, mcY, mcZ, TerrainSectionsKnown());
}

CON_COMMAND(pcb_terrain_region, "pcb_terrain_region <mcX> <mcY> <mcZ> <srcX> <srcY> <srcZ> - show Minecraft terrain around block mcX mcY mcZ at Portal 2 position srcX srcY srcZ\n") {
	if (args.ArgC() < 7) return console->Print("%s", pcb_terrain_region.ThisPtr()->m_pszHelpString);
	TerrainSetRegion(std::atof(args[1]), std::atof(args[2]), std::atof(args[3]), Vector((float)std::atof(args[4]), (float)std::atof(args[5]), (float)std::atof(args[6])));
}

void TestCheck(bool ok, const char *what, const char *detail);  // testassert.cpp
// Minecraft's render loop keeps producing frames (a hung render thread, e.g. an endless frustum loop in
// a host-view pass, otherwise freezes the composite silently).
CON_COMMAND(pcb_assert_mc_alive, "pcb_assert_mc_alive [min_frames=30] - Minecraft rendered at least min_frames frames since the last call (or since reset)\n") {
	static uint64_t last = 0;
	McState tmp{};
	if (link::ReadMcState(tmp)) g_mc = tmp, g_haveMc = true;
	uint64_t now = g_mc.frameCounter;
	if (args.ArgC() > 1 && !std::strcmp(args[1], "reset")) return void(last = now);
	int minFrames = args.ArgC() > 1 ? std::atoi(args[1]) : 30;
	char buf[160];
	std::snprintf(buf, sizeof buf, "Minecraft frame %llu, %lld since the last check, link %s", (unsigned long long)now, (long long)(now - last), link::McAlive() ? "up" : "down");
	TestCheck(link::McAlive() && now >= last + (uint64_t)minFrames, "Minecraft render loop advancing", buf);
	last = now;
}

CON_COMMAND(pcb_mclog, "pcb_mclog <frames> - log the Minecraft state for the next frames\n") {
	g_logFrames = args.ArgC() > 1 ? std::atoi(args[1]) : 30;
	console->Print("[bridge] link %s, Minecraft pid %u, mode %s\n", link::McAlive() ? "up" : "down", link::McPid(), g_mode == Mode::Minecraft ? "mc" : g_wantMc ? "mc (pending)" : "chell");
}

// ---- Minecraft-player tests (bridge #13, mcplayer1) ----
static Vector g_markPos, g_markVel;
static QAngle g_markAng;
CON_COMMAND(pcb_pos_mark, "pcb_pos_mark - remember the player's position, velocity and view (pcb_assert_pos_kept)\n") {
	g_markPos = PlayerOrigin(), g_markVel = PlayerVelocity(), g_markAng = engine.ViewAngles();
	console->Print("[bridge] pos mark (%.1f %.1f %.1f) velocity (%.0f %.0f %.0f) view (%.1f %.1f)\n", g_markPos.x, g_markPos.y, g_markPos.z, g_markVel.x, g_markVel.y, g_markVel.z, g_markAng.x, g_markAng.y);
}
CON_COMMAND(pcb_assert_pos_kept, "pcb_assert_pos_kept <max_u> [max_vel_u_s=-1] [max_view_deg=5] - position (and velocity, view) still where pcb_pos_mark took them (a mode switch keeps them)\n") {
	const float maxU = args.ArgC() > 1 ? (float)std::atof(args[1]) : 8, maxV = args.ArgC() > 2 ? (float)std::atof(args[2]) : -1, maxA = args.ArgC() > 3 ? (float)std::atof(args[3]) : 5;
	const Vector p = PlayerOrigin(), v = PlayerVelocity();
	const QAngle a = engine.ViewAngles();
	float dy = std::fabs(std::remainder(a.y - g_markAng.y, 360.0f)), dp = std::fabs(a.x - g_markAng.x);
	char buf[300];
	std::snprintf(buf, sizeof buf, "now (%.1f %.1f %.1f) vel (%.0f %.0f %.0f) view (%.1f %.1f) vs mark (%.1f %.1f %.1f) vel (%.0f %.0f %.0f) view (%.1f %.1f): %.1f u, vel diff %.0f, view %.1f deg (want <= %.0f u%s, <= %.0f deg)",
	              p.x, p.y, p.z, v.x, v.y, v.z, a.x, a.y, g_markPos.x, g_markPos.y, g_markPos.z, g_markVel.x, g_markVel.y, g_markVel.z, g_markAng.x, g_markAng.y, (p - g_markPos).Length(),
	              (v - g_markVel).Length(), std::fmax(dy, dp), maxU, maxV >= 0 ? ", velocity within the limit" : "", maxA);
	TestCheck((p - g_markPos).Length() <= maxU && (maxV < 0 || (v - g_markVel).Length() <= maxV) && std::fmax(dy, dp) <= maxA, "position kept", buf);
}
CON_COMMAND(pcb_assert_pos_moved, "pcb_assert_pos_moved <min_u> [max_dz=1e9] - the player moved at least min_u horizontally since pcb_pos_mark (and its height changed at most max_dz)\n") {
	const float minU = args.ArgC() > 1 ? (float)std::atof(args[1]) : 40, maxDz = args.ArgC() > 2 ? (float)std::atof(args[2]) : 1e9f;
	const Vector p = PlayerOrigin(), d = p - g_markPos;
	const float h = std::sqrt(d.x * d.x + d.y * d.y);
	char buf[200];
	std::snprintf(buf, sizeof buf, "from (%.1f %.1f %.1f) to (%.1f %.1f %.1f): %.1f u horizontally, %.1f u up (want >= %.0f, |dz| <= %.0f)", g_markPos.x, g_markPos.y, g_markPos.z, p.x, p.y, p.z, h, d.z, minU, maxDz);
	TestCheck(h >= minU && std::fabs(d.z) <= maxDz, "player moved", buf);
}
CON_COMMAND(pcb_assert_mc_mode, "pcb_assert_mc_mode <1|0> - the Minecraft player leads (1) or Chell (0); with 1, Portal 2's body is at Minecraft's player\n") {
	const bool want = args.ArgC() < 2 || std::atoi(args[1]) != 0;
	char buf[300];
	const Vector f = FeetSrc(), p = PlayerOrigin();
	std::snprintf(buf, sizeof buf, "mode %s (want %s), Minecraft player MC (%.2f %.2f %.2f) = Portal 2 (%.1f %.1f %.1f), body at (%.1f %.1f %.1f): %.1f u apart, ack %u/%u",
	              McActive() ? "minecraft" : g_wantMc ? "minecraft (pending)" : "chell", want ? "minecraft" : "chell", g_mc.curX, g_mc.curY, g_mc.curZ, f.x, f.y, f.z, p.x, p.y, p.z,
	              (f - p).Length(), g_mc.teleportAck, g_teleportSeq);
	TestCheck(want ? (McActive() && (f - p).Length() < 48.0f) : !McActive() && !g_wantMc, "player mode", buf);
}
// (bridge #28: pcb_assert_mc_transits is in mctransit.cpp)
bool McWaitMode(bool mc) { return mc ? McActive() : !McActive() && !g_wantMc; }
bool McPlayerAlive() { return g_haveMc && link::McAlive() && (g_mc.flags & kMcInWorld) && !(g_mc.flags & kMcDead); }  // waituntil mcalive (bridge #20)
bool McLeads() { return McActive() && !g_awaitTeleport && !g_takeover; }  // waituntil mcleads
// (bridge #20) Minecraft-side block/entity checks: the cfg queues
//   pcb_mc_cmd execute if <cond> run say PCB_OK <label>    and    pcb_mc_cmd execute unless <cond> run say PCB_FAIL <label>
// (or pcb_mc_check <label> <cond...>, which queues both); the server logs the say line to cache/bridge/mc.log, read here.
CON_COMMAND(pcb_mc_check, "pcb_mc_check <label> <execute condition...> - (tests) queue a Minecraft-side check; pcb_assert_mc_say <label> reads its verdict\n") {
	if (args.ArgC() < 3) return;
	const char *cond = args.m_pArgSBuffer + args.m_nArgv0Size;
	while (*cond == ' ') ++cond;
	cond += std::strlen(args[1]);
	while (*cond == ' ') ++cond;
	char line[600];
	std::snprintf(line, sizeof line, "pcb_mc_cmd execute if %s run say PCB_OK %s\n", cond, args[1]);
	engine.Cmd(line);
	std::snprintf(line, sizeof line, "pcb_mc_cmd execute unless %s run say PCB_FAIL %s\n", cond, args[1]);
	engine.Cmd(line);
}
CON_COMMAND(pcb_assert_mc_say, "pcb_assert_mc_say <label> [fail] - (tests) the Minecraft-side check <label> said PCB_OK (with fail: PCB_FAIL) (cache/bridge/mc.log)\n") {
	if (args.ArgC() < 2) return;
	char path[512];
	std::snprintf(path, sizeof path, "%s/mc.log", CacheDir().c_str());
	FILE *f = std::fopen(path, "r");
	int ok = 0, fail = 0;
	if (f) {
		char buf[1024], okTag[160], failTag[160];
		std::snprintf(okTag, sizeof okTag, "PCB_OK %s", args[1]);
		std::snprintf(failTag, sizeof failTag, "PCB_FAIL %s", args[1]);
		while (std::fgets(buf, sizeof buf, f)) {
			if (std::strstr(buf, "test command:")) continue;  // (the queued command's own echo ends with the tag too)
			char *e = buf + std::strlen(buf);
			while (e > buf && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
			const size_t lo = std::strlen(okTag), lf = std::strlen(failTag), n = std::strlen(buf);
			if (n >= lo && !std::strcmp(buf + n - lo, okTag)) ++ok;
			if (n >= lf && !std::strcmp(buf + n - lf, failTag)) ++fail;
		}
		std::fclose(f);
	}
	char d[200];
	const bool wantFail = args.ArgC() > 2 && !std::strcmp(args[2], "fail");  // the condition must NOT hold
	std::snprintf(d, sizeof d, "%s: %d ok, %d fail in Minecraft's log%s", args[1], ok, fail, wantFail ? " (want: fail)" : "");
	void TestCheck(bool ok, const char *what, const char *detail);
	TestCheck(wantFail ? (fail > 0 && ok == 0) : (ok > 0 && fail == 0), "Minecraft-side check", d);
}
// (bridge #20) Minecraft damage to the player (the mod's PlayerHurtLogMixin lines in mc.log) since a mark
static long g_hurtMark = 0;
CON_COMMAND(pcb_mc_hurt, "pcb_mc_hurt mark | assert <max> [source] [near] | atleast <min> [source] [near] - (tests) the Minecraft player's damage events since the mark (mc.log), optionally of one damage source (generic, fall, ...) and/or within 12 blocks of the last pcb_mc_place point\n") {
	char path[512];
	std::snprintf(path, sizeof path, "%s/mc.log", CacheDir().c_str());
	FILE *f = std::fopen(path, "r");
	if (!f) return;
	if (args.ArgC() > 1 && !std::strcmp(args[1], "mark")) {
		std::fseek(f, 0, SEEK_END), g_hurtMark = std::ftell(f), std::fclose(f);
		return;
	}
	const bool atleast = args.ArgC() > 1 && !std::strcmp(args[1], "atleast");
	std::string src;
	bool nearOnly = false;
	for (int i = 3; i < args.ArgC(); ++i) {
		if (!std::strcmp(args[i], "near")) nearOnly = true;
		else src = args[i];
	}
	double tx, ty, tz;
	SrcToMcD(g_placeTarget, tx, ty, tz);
	std::fseek(f, g_hurtMark, SEEK_SET);
	char buf[1024], last[300] = "";
	int n = 0, all = 0;
	std::map<std::string, int> bySource;
	while (std::fgets(buf, sizeof buf, f))
		if (const char *p = std::strstr(buf, "Portalcraft: player hurt: ")) {
			char sname[64] = "?";
			double dmg = 0, x = 0, y = 0, z = 0;
			std::sscanf(p + 26, "%63s %lf at %lf %lf %lf", sname, &dmg, &x, &y, &z);
			++all, ++bySource[sname];
			if (!src.empty() && src != sname) continue;
			if (nearOnly && std::hypot(std::hypot(x - tx, y - ty), z - tz) > 12.0) continue;
			++n;
			std::snprintf(last, sizeof last, "%s", p + 26);
			console->Print("[bridge] mc hurt: %s", p + 26);
		}
	std::fclose(f);
	std::string sum;
	for (const auto &kv : bySource) sum += " " + kv.first + "=" + std::to_string(kv.second);
	const int lim = args.ArgC() > 2 ? std::atoi(args[2]) : 0;
	char d[600];
	std::snprintf(d, sizeof d, "%d damage events%s%s%s since the mark (want %s %d); all sources:%s%s%s", n, src.empty() ? "" : " of source ", src.c_str(), nearOnly ? " near the placement" : "", atleast ? ">=" : "<=", lim,
	              sum.empty() ? " none" : sum.c_str(), n ? ", last: " : "", last);
	if (char *e = std::strchr(d, '\n')) *e = 0;
	void TestCheck(bool ok, const char *what, const char *detail);
	TestCheck(atleast ? n >= lim : n <= lim, atleast ? "the Minecraft player was hurt" : "the Minecraft player not hurt", d);
}
// ---- (bridge #31) mechanics tests: what the Minecraft player's motion measured, and what Portal 2 did about it -----------------------------
// pcb_mcm reset starts a recording of the Minecraft player's feet (Source u) every host frame and its per-tick speed;
// pcb_assert_mcm <metric> <min> [max] reads it: rise, drop, dist_h (the farthest it went horizontally from the start), dx dy dz (now - start),
// spd_h / spd_v (the fastest per-tick speed, u/s), air / ground (ticks), peak_t (s from the start to the highest point)
static struct Mcm {
	bool on = false;
	Vector start, minp, maxp, now;
	double spdH = 0, spdV = 0, distH = 0, peakAt = 0, t0 = 0;
	int air = 0, ground = 0;
	uint64_t tick = 0;
} g_mcm;
static void McmFrame() {
	if (!g_mcm.on || !McActive()) return;
	const Vector f = FeetSrc();
	const double t = link::NowNs() * 1e-9;
	if (g_mcm.t0 == 0) g_mcm.t0 = t, g_mcm.start = g_mcm.minp = g_mcm.maxp = f;
	g_mcm.now = f;
	g_mcm.minp = Vector(std::min(g_mcm.minp.x, f.x), std::min(g_mcm.minp.y, f.y), std::min(g_mcm.minp.z, f.z));
	if (f.z > g_mcm.maxp.z) g_mcm.peakAt = t - g_mcm.t0;
	g_mcm.maxp = Vector(std::max(g_mcm.maxp.x, f.x), std::max(g_mcm.maxp.y, f.y), std::max(g_mcm.maxp.z, f.z));
	g_mcm.distH = std::max(g_mcm.distH, (double)std::hypot(f.x - g_mcm.start.x, f.y - g_mcm.start.y));
	if ((uint64_t)g_mc.tickQpc != g_mcm.tick) {
		g_mcm.tick = (uint64_t)g_mc.tickQpc;
		const Vector a = McToSrc(g_mc.prevX, g_mc.prevY, g_mc.prevZ), b = McToSrc(g_mc.curX, g_mc.curY, g_mc.curZ);
		const double dt = (g_mc.tickMs > 0 ? g_mc.tickMs : 50.0) * 1e-3;
		g_mcm.spdH = std::max(g_mcm.spdH, std::hypot(b.x - a.x, b.y - a.y) / dt), g_mcm.spdV = std::max(g_mcm.spdV, std::fabs(b.z - a.z) / dt);
		if (g_mc.flags & kMcOnGround) ++g_mcm.ground;
		else ++g_mcm.air;
	}
}
CON_COMMAND(pcb_mcm, "pcb_mcm reset - (bridge #31) start recording the Minecraft player's motion for pcb_assert_mcm\n") {
	g_mcm = Mcm();
	g_mcm.on = true;
}
CON_COMMAND(pcb_assert_mcm, "pcb_assert_mcm <rise|drop|dist_h|dx|dy|dz|spd_h|spd_v|air|ground|peak_t> <min> [max] - (bridge #31) the Minecraft player's motion since pcb_mcm reset\n") {
	if (args.ArgC() < 3) return;
	const std::string m = args[1];
	const Mcm &c = g_mcm;
	double v = 0;
	if (m == "rise") v = c.maxp.z - c.start.z;
	else if (m == "drop") v = c.start.z - c.minp.z;
	else if (m == "dist_h") v = c.distH;
	else if (m == "dx") v = c.now.x - c.start.x;
	else if (m == "dy") v = c.now.y - c.start.y;
	else if (m == "dz") v = c.now.z - c.start.z;
	else if (m == "spd_h") v = c.spdH;
	else if (m == "spd_v") v = c.spdV;
	else if (m == "air") v = c.air;
	else if (m == "ground") v = c.ground;
	else if (m == "peak_t") v = c.peakAt;
	const double mn = std::atof(args[2]), mx = args.ArgC() > 3 ? std::atof(args[3]) : 1e18;
	char buf[300];
	std::snprintf(buf, sizeof buf, "%s = %.1f (want %.1f..%.1f); start (%.0f %.0f %.0f) now (%.0f %.0f %.0f) peak z %.0f low z %.0f, %d air / %d ground ticks", m.c_str(), v, mn, mx, c.start.x, c.start.y,
	              c.start.z, c.now.x, c.now.y, c.now.z, c.maxp.z, c.minp.z, c.air, c.ground);
	TestCheck(c.on && v >= mn && v <= mx, "the Minecraft player's measured motion", buf);
}
CON_COMMAND(pcb_assert_mc_near, "pcb_assert_mc_near <targetname> <tol_u> [max_dz=1e9] - (bridge #31) the Minecraft player's feet are within tol u (horizontally) of that entity's origin\n") {
	if (args.ArgC() < 3) return;
	std::vector<void *> v = EntFind(nullptr, args[1]);
	const int off = v.empty() ? -1 : EntFieldOffset(v.front(), "m_vecAbsOrigin");
	if (off < 0) return TestCheck(false, "near entity", (std::string("no entity ") + args[1]).c_str());
	const Vector o = *(const Vector *)((const char *)v.front() + off), f = FeetSrc();
	const double d = std::hypot(f.x - o.x, f.y - o.y), tol = std::atof(args[2]), mdz = args.ArgC() > 3 ? std::atof(args[3]) : 1e9;
	char buf[220];
	std::snprintf(buf, sizeof buf, "feet (%.0f %.0f %.0f) are %.0f u (h) and dz %.0f from %s (%.0f %.0f %.0f), want <= %.0f / %.0f", f.x, f.y, f.z, d, f.z - o.z, args[1], o.x, o.y, o.z, tol, mdz);
	TestCheck(d <= tol && std::fabs(f.z - o.z) <= mdz, "the Minecraft player is near the entity", buf);
}
CON_COMMAND(pcb_assert_ent_count, "pcb_assert_ent_count <targetname> <min> [max=min] - (bridge #31) live entities with that targetname\n") {
	if (args.ArgC() < 3) return;
	const int n = (int)EntFind(nullptr, args[1]).size(), mn = std::atoi(args[2]), mx = args.ArgC() > 3 ? std::atoi(args[3]) : mn;
	char buf[160];
	std::snprintf(buf, sizeof buf, "%d entities named %s (want %d..%d)", n, args[1], mn, mx);
	TestCheck(n >= mn && n <= mx, "entity count", buf);
}
// ---- verified placement: pcb_mc_place + waituntil mcplaced + pcb_assert_placed. A block whose placement failed fails loudly and its other asserts are NOT RUN.
void TestBlockSkip(bool on);  // testassert.cpp
bool McPlaced() {
	if (!g_placeArmed) return true;
	if (!McActive() || g_awaitTeleport || g_standTeleportIn > 0) return false;
	const Vector f = FeetSrc();
	return (g_mc.flags & kMcOnGround) && std::hypot(f.x - g_placeTarget.x, f.y - g_placeTarget.y) < 24.0f;
}
CON_COMMAND(pcb_test_block, "pcb_test_block <name> - (bridge #31) a new test block: asserts run again (after a failed placement they are NOT RUN until the next block)\n") {
	TestBlockSkip(false);
	g_placeArmed = false;
	console->Print("[bridge] TEST BLOCK %s\n", args.ArgC() > 1 ? args[1] : "?");
}
CON_COMMAND(pcb_mc_place, "pcb_mc_place <x> <y> <z> <yaw> [max fall u=8] - (bridge #31) put the Minecraft player at a Portal 2 point (then: waituntil mcplaced <n>, pcb_assert_placed)\n") {
	if (args.ArgC() < 5) return;
	char cmd[128];
	std::snprintf(cmd, sizeof cmd, "setpos %s %s %s", args[1], args[2], args[3]);
	engine.Cmd(cmd);
	std::snprintf(cmd, sizeof cmd, "setang 0 %s 0", args[4]);
	engine.Cmd(cmd);
	g_placeTarget = Vector((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3]));
	g_placeFall = args.ArgC() > 5 ? (float)std::atof(args[5]) : 8.0f;
	g_placeArmed = true;
	if (McActive()) g_awaitTeleport = true, g_standTeleportIn = 3, g_standTarget = g_placeTarget;
}
// (bridge #32, testbed.cpp: pc_tb_goto, no sv_cheats) the body has been put at `feet` (Portal 2's own teleport): as the Minecraft player,
// Minecraft is sent to it a few frames later (StandTeleportFrame), and waituntil mcplaced / pcb_assert_placed judge the arrival.
void McPlaceBody(const Vector &feet) {
	g_placeTarget = feet;
	g_placeFall = 16.0f;
	g_placeArmed = true;
	if (McActive()) g_awaitTeleport = true, g_standTeleportIn = 3, g_standTarget = feet;
}
// the Minecraft player's velocity in Portal 2's frame (u/s), from the latest tick (mcmechanics.cpp: a launch is judged against it)
Vector McVelocitySrc() { return VelocitySrc(); }
CON_COMMAND(pcb_assert_placed, "pcb_assert_placed [tol_xy=8] - (bridge #31) the Minecraft player is where pcb_mc_place put it (on the ground, within tol, at most max-fall below); else the block's asserts are NOT RUN\n") {
	const float tol = args.ArgC() > 1 ? (float)std::atof(args[1]) : 8.0f;
	const Vector f = FeetSrc();
	const float dh = std::hypot(f.x - g_placeTarget.x, f.y - g_placeTarget.y);
	const bool ground = (g_mc.flags & kMcOnGround) != 0, zok = f.z <= g_placeTarget.z + 4.0f && f.z >= g_placeTarget.z - g_placeFall - 4.0f;
	const bool ok = g_placeArmed && McActive() && dh <= tol && ground && zok;
	char buf[300];
	if (ok) std::snprintf(buf, sizeof buf, "Minecraft stands at (%.1f %.1f %.1f) on ground, %.1f u from (%.1f %.1f %.1f)", f.x, f.y, f.z, dh, g_placeTarget.x, g_placeTarget.y, g_placeTarget.z);
	else std::snprintf(buf, sizeof buf, "placement failed: at (%.1f %.1f %.1f) ground %d mc %d, wanted (%.1f %.1f %.1f) within %.0f u and at most %.0f u below", f.x, f.y, f.z, ground, McActive() ? 1 : 0,
	                   g_placeTarget.x, g_placeTarget.y, g_placeTarget.z, tol, g_placeFall);
	TestCheck(ok, "placement", buf);
	TestBlockSkip(!ok);
}
CON_COMMAND(pcb_assert_mc_flag, "pcb_assert_mc_flag <ground|sneak|sprint|screen|flying|swim|dead> <1|0> - a Minecraft player state flag (McState)\n") {
	if (args.ArgC() < 3) return;
	const char *n = args[1];
	const uint32_t bit = !std::strcmp(n, "ground") ? kMcOnGround : !std::strcmp(n, "sneak") ? kMcSneaking : !std::strcmp(n, "sprint") ? kMcSprinting
	                     : !std::strcmp(n, "screen") ? kMcScreenOpen : !std::strcmp(n, "flying") ? kMcFlying
	                     : !std::strcmp(n, "swim") ? kMcSwimming : !std::strcmp(n, "dead") ? kMcDead : 0;
	const bool want = std::atoi(args[2]) != 0, have = (g_mc.flags & bit) != 0;
	char buf[120];
	std::snprintf(buf, sizeof buf, "%s %d (want %d), flags 0x%x", n, have, want, g_mc.flags);
	TestCheck(bit && have == want, "Minecraft player flag", buf);
}
CON_COMMAND(pcb_assert_body_z, "pcb_assert_body_z <min_z> <max_z> - the player's feet height (stairs, floors)\n") {
	if (args.ArgC() < 3) return;
	const Vector p = PlayerOrigin();
	const float lo = (float)std::atof(args[1]), hi = (float)std::atof(args[2]);
	char buf[120];
	std::snprintf(buf, sizeof buf, "feet at (%.1f %.1f %.1f), want z %.0f..%.0f", p.x, p.y, p.z, lo, hi);
	TestCheck(p.z >= lo && p.z <= hi, "feet height", buf);
}
// The body (Portal 2's player hull at the Minecraft player's feet) is not inside Portal 2's world: walls held it.
CON_COMMAND(pcb_assert_body_clear, "pcb_assert_body_clear - the player's hull is not in Portal 2's world solid and its centre is inside the map\n") {
	const Vector p = PlayerOrigin();
	CGameTrace tr;
	// (the Minecraft player's core: its collision is Portal 2's world in 1/8-block voxels, so it may stand up to ~5 u
	// into a wall or step a voxel's centre test missed; walking through a wall puts the core in it or past it)
	engine.Trace(p + Vector(0, 0, 40), p + Vector(0, 0, 40), Vector(6, 6, 30), MASK_PLAYERSOLID, tr, true);
	const bool outside = engine.PointOutsideWorld(engine.traceServer->ThisPtr(), p + Vector(0, 0, 36));
	char buf[160];
	std::snprintf(buf, sizeof buf, "body at (%.1f %.1f %.1f): in solid %d, outside the map %d", p.x, p.y, p.z, tr.startsolid ? 1 : 0, outside ? 1 : 0);
	TestCheck(!tr.startsolid && !outside, "body clear of walls", buf);
}

CON_COMMAND(pc_input_lock, "pc_input_lock [0|1] - 1: the real keyboard and mouse are ignored (test runs: only scripted input); 0: normal\n") {
	if (args.ArgC() > 1) g_inputLock = std::atoi(args[1]) != 0;
	console->Print("[bridge] input lock: %s\n", g_inputLock.load() ? "ON (real keyboard and mouse ignored)" : "off");
}
CON_COMMAND(pcb_mckey, "pcb_mckey <scancode|lmb|rmb> <0|1> - scripted Minecraft input (held until released)\n") {
	if (args.ArgC() < 3) return console->Print("%s", pcb_mckey.ThisPtr()->m_pszHelpString);
	int down = std::atoi(args[2]) ? 1 : 0;
	if (!std::strcmp(args[1], "lmb")) g_scriptButtons = down ? (g_scriptButtons | 1) : (g_scriptButtons & ~1u);
	else if (!std::strcmp(args[1], "rmb")) g_scriptButtons = down ? (g_scriptButtons | 4) : (g_scriptButtons & ~4u);
	else {
		int sc = std::atoi(args[1]);
		if (sc > 0 && sc < 512) g_scriptKeys[sc] = (uint8_t)down;
	}
}

// (bridge #15, tests) real SDL events pushed into Portal 2's queue: they go through the same event filter as the
// mouse and wheel (the filter runs at push time), so the screen cursor, the hotbar wheel and the view freeze are
// tested on the real path. SDL2 event layouts: wheel y @20; motion xrel/yrel @28/@32; key scancode @16.
CON_COMMAND(pcb_sdl_event, "pcb_sdl_event <wheel <notches> | motion <dx> <dy> | key <scancode>> - (tests) push a real SDL event through Portal 2's queue\n") {
	if (args.ArgC() < 3) return console->Print("%s", pcb_sdl_event.ThisPtr()->m_pszHelpString);
	using _SDL_PushEvent = int (*)(void *ev);
	static _SDL_PushEvent push = nullptr;
	if (!push) {
		push = (_SDL_PushEvent)plat::SdlSymbol("SDL_PushEvent");
		if (!push) return console->Print("[bridge] SDL_PushEvent missing\n");
	}
	alignas(8) uint8_t ev[56] = {};
	auto put = [&](int off, int32_t v) { std::memcpy(ev + off, &v, 4); };
	if (!std::strcmp(args[1], "wheel")) put(0, 0x403), put(20, std::atoi(args[2]));
	else if (!std::strcmp(args[1], "motion")) put(0, 0x400), put(28, std::atoi(args[2])), put(32, args.ArgC() > 3 ? std::atoi(args[3]) : 0);
	else if (!std::strcmp(args[1], "motionwarp")) {
		// (tests) a real pointer move of dx dy from the window centre, then Portal 2's warp back to the centre (what the real mouse does)
		int ww = 1600, wh = 900;
		if (SDL_GetWindowSize_ && SDL_GetKeyboardFocus_)
			if (void *win = SDL_GetKeyboardFocus_()) SDL_GetWindowSize_(win, &ww, &wh);
		const int dx = std::atoi(args[2]), dy = args.ArgC() > 3 ? std::atoi(args[3]) : 0, cx = ww / 2, cy = wh / 2;
		put(0, 0x400), put(20, cx), put(24, cy);  // (back at the centre first: the previous warp)
		put(28, 0), put(32, 0);
		++g_injecting; push(ev); --g_injecting;
		put(20, cx + dx), put(24, cy + dy), put(28, dx), put(32, dy);
		++g_injecting; push(ev); --g_injecting;
		put(20, cx), put(24, cy), put(28, -dx), put(32, -dy);
		++g_injecting; const int r = push(ev); --g_injecting;
		console->Print("[bridge] sdl motion %d %d + warp back pushed (%d warps dropped so far)\n", dx, dy, g_warpsDropped.load());
		return void(r);
	}
	else if (!std::strcmp(args[1], "key")) put(0, 0x300), ev[12] = 1 /* pressed */, put(16, std::atoi(args[2]));
	else return;
	++g_injecting; const int r = push(ev); --g_injecting;
	console->Print("[bridge] sdl event %s pushed (%s)\n", args[1], r == 1 ? "passed the filter" : r == 0 ? "taken by the filter" : "error");
}
CON_COMMAND(pcb_screen_cursor, "pcb_screen_cursor - (tests) print the Minecraft screen cursor and Portal 2's view angles\n") {
	const QAngle va = engine.ViewAngles();
	console->Print("[bridge] screen cursor %d %d (screen %s), view %.2f %.2f\n", g_cursorX, g_cursorY, (g_mc.flags & kMcScreenOpen) ? "open" : "closed", va.x, va.y);
}

}  // namespace bridge
