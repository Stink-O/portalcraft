// Portalcraft bridge (bridge #56): Portal 2's own state that decides whether Chell's viewmodel and HUD are on screen,
// read from the local player's server datamap (the same route as ents.cpp). mc.cpp's HudShouldShow() combines this with
// the engine's pause/console/loading state and Minecraft's own state, and gates the Minecraft hand/HUD with it.
//
// Every field is looked up by name once; a field the datamap does not have is simply not used (the rule then errs on
// the side of showing, as before). Reads happen only while a level is live and the cached player pointer is still a
// live entity (re-checked about once a second and on every level start).
#include "bridge.hpp"

#include "Hook.hpp"
#include "Modules/Tier1.hpp"

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace bridge {

int EntFieldOffset(void *ent, const char *field);                 // ents.cpp
bool PlayerVmGrab();                                              // ents.cpp (bridge #68)
bool PlayerHolding();                                             // propcollide.cpp
void EntLiveSet(std::unordered_set<void *> &out);                  // ents.cpp
int EntMoveType(void *ent);                                        // ents.cpp
bool PlayerSpeedmodZero();                                         // ents.cpp (bridge #63)

static void *g_player = nullptr;
static double g_checkedAt = -10;
static int g_offHide = -2, g_offWeapon = -2, g_offViewEnt = -2, g_offDrawVm = -2, g_offLife = -2, g_offFlags = -2;
static int g_offCamState = -2, g_offProxyState = -2, g_offLagged = -2;
static double g_camAt = -10;
static bool g_camActive = false, g_camCertain = false;
static char g_camDbg[160] = "";  // the cameras' states, for the probe line
static int ViewScripted(const char *b);  // below (bridge #63)
static uint32_t g_ourMask = 0;  // HIDEHUD bits our own SetHUDVisibility 0 sets (not Portal 2's own hiding)
static int g_hideBitsCfg = 4 | 16;  // HIDEHUD_ALL | HIDEHUD_PLAYERDEAD

static double NowSec() {
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void HudGateLevelInit() {
	g_player = nullptr, g_checkedAt = -10;
	g_offHide = g_offWeapon = g_offViewEnt = g_offDrawVm = g_offLife = g_offFlags = -2;
	g_camAt = -10, g_camActive = g_camCertain = false, g_offCamState = g_offProxyState = g_offLagged = -2;
	g_ourMask = 0;
}

static void *Player() {
	const double now = NowSec();
	if (g_player && now - g_checkedAt < 1.0) return g_player;
	std::unordered_set<void *> live;
	if (g_player) {
		EntLiveSet(live);
		g_checkedAt = now;
		if (live.count(g_player)) return g_player;
		g_player = nullptr, g_offHide = g_offWeapon = g_offViewEnt = g_offDrawVm = g_offLife = g_offFlags = -2;
	}
	if (now - g_checkedAt < 0.25) return nullptr;
	g_checkedAt = now;
	auto v = EntFind("player", nullptr);
	if (v.empty()) return nullptr;
	g_player = v[0];
	return g_player;
}

static int Off(int &cache, const char *name) {
	if (cache == -2) {
		cache = g_player ? EntFieldOffset(g_player, name) : -1;
		console->Print("[bridge] hud gate: %s at +%d in the player's datamap\n", name, cache);
	}
	return cache;
}

// The HIDEHUD bits mc.cpp's own SetHUDVisibility input sets: measured around the input (HideChell), so only Portal 2's
// own hiding counts afterwards.
uint32_t HudGateReadHide() {
	void *p = Player();
	const int o = p ? Off(g_offHide, "m_iHideHUD") : -1;
	return o < 0 ? 0u : *(const uint32_t *)((const char *)p + o);
}
void HudGateOurHide(uint32_t before, uint32_t after) {
	g_ourMask = after & ~before;
	if (g_ourMask == 0) g_ourMask = 4;  // (an input that changed nothing measurable: HIDEHUD_ALL is what it sets in the SDK)
	console->Print("[bridge] hud gate: our SetHUDVisibility 0 sets m_iHideHUD bits 0x%x (before 0x%x, after 0x%x)\n", g_ourMask, before, after);
}
void HudGateOurHideClear() { g_ourMask = 0; }

// false = Portal 2 is not showing the gun / HUD; `why` names the first reason.
bool HudGateP2Shows(const char *&why) {
	void *p = Player();
	if (!p) return true;
	const char *b = (const char *)p;
	int o = Off(g_offLife, "m_lifeState");
	if (o >= 0 && *(const unsigned char *)(b + o) != 0) return why = "dead (Portal 2 life state)", false;
	o = Off(g_offHide, "m_iHideHUD");
	if (o >= 0) {
		const uint32_t bits = *(const uint32_t *)(b + o) & ~g_ourMask;
		if (bits & (uint32_t)g_hideBitsCfg) return why = "hidehud flags", false;
	}
	if (ViewScripted(b)) return why = "scripted camera", false;  // (bridge #63: the cameras' enabled state, not a stale view entity)
	{  // (bridge #69) sitting in a vehicle (sp_a4_finale4's ending): Portal 2 draws no weapon and no HUD, and the Minecraft hand must not sit over the seat's view
		static int offVeh2 = -2;
		if (offVeh2 == -2) offVeh2 = EntFieldOffset(p, "m_hVehicle");
		if (offVeh2 >= 0) {
			const uint32_t h = *(const uint32_t *)(b + offVeh2);
			if (h != 0xFFFFFFFFu && h != 0) return why = "in a vehicle", false;
		}
	}
	// (lead) no "no weapon" rule: the Minecraft player has its own hand and hotbar whether or not Chell holds the gun
	// (the user: the hand and HUD must show in sp_a1_intro1 after the wake-up white screen)
	o = Off(g_offDrawVm, "m_bDrawViewmodel");
	if (o >= 0 && *(const unsigned char *)(b + o) == 0) return why = "viewmodel off", false;
	// (bridge #68) a forced viewmodel grab (ForceVMGrabController: intro7's Wheatley, bts4) carries the held object in Portal 2's own viewmodel: that is drawn
	// (mc.cpp EnforceViewmodel leaves it on) and the Minecraft hand and HUD give way to it, as Chell's HUD would
	if (PlayerVmGrab() && PlayerHolding()) return why = "Portal 2's viewmodel carries the held object", false;
	return true;
}

// (bridge #63; this is the PlayerRestrained() of the campaign survey, item 10) Portal 2 is driving the player: a point_viewcontrol camera is the view (the wake-up, the elevator videos, the final
// sequences) or the player is frozen (FL_FROZEN). mc.cpp keeps the Minecraft player held on the body and the camera Portal 2's while this
// holds. m_hViewEntity alone is NOT trusted: Portal 2's scripts leave it set (to the player itself or to a camera that has been disabled) after the
// sequence (bugmark 20261009-065600: no HUD, no hand, no walking for minutes after the white screen). The cameras' own enabled state decides:
// `certain` = the state field was read. Without it (field not found) the view entity is the fallback, uncertain.
static void CameraRefresh() {
	const double now = NowSec();
	if (now - g_camAt < 0.2) return;
	g_camAt = now;
	g_camActive = false, g_camCertain = false;
	std::vector<void *> cams = EntFind("point_viewcontrol", nullptr);
	// (bridge #69) a point_viewproxy (the wake-up's first seconds, the bts5 / bts6 pod rides) is a scripted view too: its own m_state is 1 while it drives
	// the view, with every camera's m_state still 0 (measured on sp_a1_wakeup: proxy 1 for ~4 s, then the viewcontrol takes over)
	bool proxyOn = false;
	for (void *px : EntFind("point_viewproxy", nullptr)) {
		if (g_offProxyState == -2) {
			g_offProxyState = EntFieldOffset(px, "m_state");
			console->Print("[bridge] hud gate: point_viewproxy m_state at +%d\n", g_offProxyState);
		}
		if (g_offProxyState >= 0 && *(const int *)((const char *)px + g_offProxyState) == 1) proxyOn = true;
	}
	// (lead) a proxy alone is NOT a restraint: sp_a1_intro1's vault after the bed keeps a proxy on while Chell walks freely (bridge #69's rule held the
	// Minecraft player for 33 s there: no walking, no HUD). A proxy is a view redirect, handled by mc.cpp OverrideView; real restraint in a proxy scene
	// (wake-up claws, the pods) shows up as a camera, a freeze, a parent or speedmod 0.
	(void)proxyOn;
	if (cams.empty()) {
		g_camCertain = true, g_camActive = false;
		return;
	}
	// (the line above: no camera in the map)  // no camera in the map: none active (but a proxy may be)
	if (g_offCamState == -2) {
		g_offCamState = EntFieldOffset(cams[0], "m_state");
		console->Print("[bridge] hud gate: point_viewcontrol m_state at +%d (%zu cameras)\n", g_offCamState, cams.size());
	}
	if (g_offCamState < 0) return;
	g_camCertain = true;
	g_camActive = false;
	int n = 0;
	g_camDbg[0] = 0;
	for (void *c : cams) {
		const int st = *(const int *)((const char *)c + g_offCamState);
		if (st == 1) g_camActive = true;  // USE_ON
		if (n < 8) n += std::snprintf(g_camDbg + n, sizeof g_camDbg - n, "%s%d", n ? "," : "", st);
	}
}
// 1 = a camera is the view (certain), 2 = the view entity is set and the cameras' state is unknown (uncertain), 0 = not
static int ViewScripted(const char *b) {
	CameraRefresh();
	if (g_camCertain) return g_camActive ? 1 : 0;
	const int o = Off(g_offViewEnt, "m_hViewEntity");
	if (o >= 0) {
		const uint32_t h = *(const uint32_t *)(b + o);
		if (h != 0xFFFFFFFFu && h != 0) return 2;
	}
	return 0;
}
// One line of every restraint signal (the sequence's diagnosis when none of them fired): the view entity's raw handle, the cameras' m_state, flags, movetype.
void HudGateProbe(char *out, size_t n) {
	out[0] = 0;
	void *p = Player();
	if (!p) return;
	const char *b = (const char *)p;
	CameraRefresh();
	const int ov = Off(g_offViewEnt, "m_hViewEntity"), of = Off(g_offFlags, "m_fFlags");
	std::snprintf(out, n, "viewent 0x%x, %zu cameras' m_state [%s], flags 0x%x, movetype %d", ov >= 0 ? *(const uint32_t *)(b + ov) : 0u, EntFind("point_viewcontrol", nullptr).size(), g_camDbg,
	              of >= 0 ? *(const uint32_t *)(b + of) : 0u, EntMoveType(p));
}
bool HudGateScripted(bool &cam, bool &certain, const char *&why) {
	cam = certain = false;
	void *p = Player();
	if (!p) return false;
	const char *b = (const char *)p;
	const int v = ViewScripted(b);
	if (v) return cam = true, certain = v == 1, why = v == 1 ? "scripted camera" : "view entity set, camera state unknown", true;
	if (PlayerSpeedmodZero()) return cam = true, certain = true, why = "player_speedmod 0", true;  // (the input is watched: ents.cpp)
	// (bridge #69) the field itself: a level's logic_auto fires ModifySpeed 0 at map spawn, before the input watch is installed (sp_a3_00, sp_a4_finale4)
	{
		const int ol = Off(g_offLagged, "m_flLaggedMovementValue");
		if (ol >= 0 && *(const float *)(b + ol) < 0.05f) return cam = true, certain = true, why = "player_speedmod 0 (the player's lagged movement value)", true;
	}
	int o = Off(g_offFlags, "m_fFlags");
	if (o >= 0 && (*(const uint32_t *)(b + o) & ((1u << 5) | (1u << 6))) != 0) return why = "player frozen or at controls", true;  // FL_FROZEN, FL_ATCONTROLS
	if (EntMoveType(p) == 0) return why = "movetype none", true;
	// a vehicle (the ending's seat) or a move parent (a prop the player is attached to: a pod, a bed)
	static int offVeh = -2, offPar = -2;
	if (offVeh == -2) offVeh = EntFieldOffset(p, "m_hVehicle"), console->Print("[bridge] hud gate: m_hVehicle at +%d\n", offVeh);
	if (offPar == -2) offPar = EntFieldOffset(p, "m_pParent"), console->Print("[bridge] hud gate: m_pParent at +%d\n", offPar);
	for (int off : {offVeh, offPar})
		if (off >= 0) {
			const uint32_t h = *(const uint32_t *)(b + off);
			// (bridge #69) a vehicle is the view too (the ending's seat: Portal 2 draws the seat's own eyes and arms): it counts as a camera (no 8 s release, no Minecraft hand)
			if (h != 0xFFFFFFFFu && h != 0) return off == offVeh ? (cam = certain = true, why = "in a vehicle") : (why = "attached to a prop"), true;
		}
	return false;
}

CON_COMMAND(pc_mc_hud_hidebits, "pc_mc_hud_hidebits [mask] - m_iHideHUD bits (Portal 2's own) that hide the Minecraft hand/HUD (default 20 = ALL|PLAYERDEAD)\n") {
	if (args.ArgC() > 1) g_hideBitsCfg = (int)std::strtol(args[1], nullptr, 0);
	console->Print("[bridge] hud hide bits 0x%x\n", g_hideBitsCfg);
}

}  // namespace bridge
