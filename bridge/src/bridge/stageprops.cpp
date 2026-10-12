// Portalcraft bridge (#74): Portal 2's props in the virtual region (the Minecraft stage, stage.cpp) are drawn by us.
//
// The stage box is solid void in Portal 2's world. The engine draws a client entity only through the leaves its bounds touch, and a prop in
// solid void touches none, so a cube, a turret or a carried prop standing on the stage was never submitted to the studio renderer (#49's gap,
// measured by the studio-draw count in mclight.cpp). Portals are drawn by a different path (fixed by StagePortalCulling, stage.cpp).
// So, for every 3D view, the props whose origin is in the box are drawn through their own renderable's DrawModel, at the start of the
// translucent pass, after Minecraft's image is in the depth buffer: terrain in front hides them, and the studio draw hook of mclight.cpp
// lights them with Minecraft's light as it does every model in a region. Views through portals take the same call, so a cube is seen from
// the chamber through the portal and the other way round.
#include "bridge.hpp"
#include "slots.hpp"
#include "coords.hpp"

#include "Modules/Tier1.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace bridge {

bool StageActive();  // stage.cpp

namespace {

Interface *g_entList = nullptr;  // VClientEntityList003
// slot numbers and the renderable offset: slots.hpp. IClientRenderable sits one pointer after IClientUnknown in a C_BaseEntity and
// IClientNetworkable one after it (SDK order).
constexpr int kStudioRender = 1;

using slots::VCall;

const char *const kClasses[] = {"prop_weighted_cube", "npc_portal_turret_floor", "prop_monster_box", "prop_physics", "prop_physics_override", "prop_physics_multiplayer",
                                "prop_glass_futbol", "prop_exploding_futbol", "prop_personality_sphere", "prop_glados_core", "prop_energy_ball", "prop_dynamic_override"};

struct Prop {
	void *server;
	int index;
	const char *cls;
};
std::vector<Prop> g_props;
double g_scanT = -1;
int g_drawn = 0, g_checked = 0, g_bad = 0, g_logViews = 0;
bool g_on = true;


// The server sends a client only the entities its PVS check passes, and a prop in the box has no cluster: the client never had the entity at
// all (measured: the client's networkable for the cube was null). The engine's own "always transmit" edict state fixes that. IServerUnknown
// slot 5 is GetNetworkable, IServerNetworkable slot 2 GetEdict and slot 3 GetClassName (checked against the entity's own classname below),
// and the edict's first word holds its state flags (FL_EDICT_ALWAYS = 1 << 3 among FL_EDICT_PVSCHECK 1 << 5 and FL_EDICT_DONTSEND 1 << 4).
bool g_netOk = true;
int g_alwaysSet = 0;
void TransmitAlways(void *ent, const char *cls) {
	if (!g_netOk) return;
	void *net = VCall<void *>(ent, slots::kServerGetNetworkable);
	const char *nm = net ? VCall<const char *>(net, slots::kServerNetGetClassName) : nullptr;
	if (!nm || std::strcmp(nm, cls)) {
		console->Print("[bridge] stage props: the server entity's networkable does not answer with its classname (%s): not forcing transmit\n", nm ? nm : "null");
		g_netOk = false;
		return;
	}
	int *ed = VCall<int *>(net, slots::kServerNetGetEdict);
	if (!ed) return;
	const int want = (*ed & ~((1 << 4) | (1 << 5))) | (1 << 3);
	if (*ed != want) {
		*ed = want;
		if (g_alwaysSet++ < 4) console->Print("[bridge] stage props: %s %p set to always transmit (edict flags now 0x%x)\n", cls, ent, want);
	}
}

double NowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void Rescan() {
	g_props.clear();
	if (!g_entList) g_entList = Interface::Create(MODULE_NAME("client"), "VClientEntityList003", false);
	if (!g_entList) return;
	for (const char *cls : kClasses) {
		for (void *e : EntFind(cls)) {
			const Vector o = EntVec(e, "origin");
			if (!InStageBox(o)) continue;
			const unsigned *h = VCall<const unsigned *>(e, slots::kServerGetRefEHandle);
			if (!h) continue;
			g_props.push_back({e, (int)(*h & 0xFFFF), cls});
			TransmitAlways(e, cls);
		}
	}
}

}  // namespace

void StagePropsDraw(const CViewSetup *view) {
	if (!g_on || !StageActive()) return;
	if (!slots::Use(slots::kNetIsDormant) || !slots::Use(slots::kNetEntIndex)) return;  // Windows safe-load: Portal 2-only slots
	const int drawnBefore = g_drawn;
	const double now = NowS();
	if (g_scanT < 0 || now - g_scanT > 0.2) g_scanT = now, Rescan();  // (cheap: the lists are small; a freed entity is out within 0.2 s)
	{ static int l2 = 0; if (l2 < 1 && !g_props.empty()) ++l2, console->Print("[bridge] stage props: %zu props in the box\n", g_props.size()); }
	for (const Prop &p : g_props) {
		TransmitAlways(p.server, p.cls);
		void *net = VCall<void *>(g_entList->ThisPtr(), slots::kEntListGetClientNetworkable, p.index);
		{
			static int logged = 0;
			if (logged < 2) ++logged, console->Print("[bridge] stage props: entity %d: client networkable %p, index %d, dormant %d\n", p.index, net, net ? VCall<int>(net, slots::kNetEntIndex) : -1, net ? (int)VCall<bool>(net, slots::kNetIsDormant) : -1);
		}
		if (!net || VCall<int>(net, slots::kNetEntIndex) != p.index || VCall<bool>(net, slots::kNetIsDormant)) continue;
		void *ren = (uint8_t *)net + slots::Index(slots::kRenderableOffsetFromNet);
		if (g_checked < 3) {  // (the layout is checked on the first props: the renderable's own origin must be the entity's)
			++g_checked;
			const Vector *ro = VCall<const Vector *>(ren, slots::kRenderableGetRenderOrigin);
			const Vector so = EntVec(p.server, "origin");
			const bool ok = ro && (*ro - so).Length() < 40.0f;
			console->Print("[bridge] stage props: renderable origin %s (%.0f %.0f %.0f) against the entity's (%.0f %.0f %.0f)\n", ok ? "matches" : "DOES NOT MATCH", ro ? ro->x : 0, ro ? ro->y : 0,
			               ro ? ro->z : 0, so.x, so.y, so.z);
			if (!ok) g_bad = 1;
		}
		if (g_bad) {
			g_on = false;
			return;
		}
		VCall<int>(ren, slots::kRenderableDrawModel, kStudioRender);
		++g_drawn;
	}
	if (g_logViews > 0 && !g_props.empty()) --g_logViews, console->Print("[bridge] stage props view: origin (%.0f %.0f %.0f) ang (%.0f %.0f %.0f), %d drawn of %zu\n", view->origin.x, view->origin.y, view->origin.z, view->angles.x, view->angles.y, view->angles.z, g_drawn - drawnBefore, g_props.size());
}

CON_COMMAND(pcb_stage_props, "pcb_stage_props [0|1] - (bridge #74) draw Portal 2's props that stand in the virtual region (the engine does not: the box is solid void)\n") {
	if (args.ArgC() > 1) g_on = std::atoi(args[1]) != 0, g_bad = 0, g_checked = 0;
	if (args.ArgC() > 2) g_logViews = std::atoi(args[2]);
	console->Print("[bridge] stage props: %s, %zu in the box, %d drawn so far\n", g_on ? "on" : "off", g_props.size(), g_drawn);
}

}  // namespace bridge
