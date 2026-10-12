// Portalcraft bridge (bridge #32): commands for the pc_testbed map that work WITHOUT sv_cheats (ent_fire and setpos need it).
//
//   pc_tb                       lists the bays: name, standing spot and facing (the map index / the live entities), whether both relays exist
//   pc_tb <bay> go|reset        fires tb_go_<bay> / tb_reset_<bay> ("Trigger") through the server's entity I/O (AcceptInput, as ent_fire does)
//   pc_tb_goto <bay>            puts the current player (Chell, or the Minecraft player) at tb_spot_<bay> with the spot's facing, and verifies it:
//                               "pc_tb_goto <bay>: OK ..." or "pc_tb_goto <bay>: FAILED ..." in the console and the log
//   pc_tb_goto_assert           (tests) an assert (pass/fail, ends a test run) on the last pc_tb_goto's verdict
//
// Nothing is typed in: the bays come from the map index (the named tb_go_<bay> relays), the spot from the live tb_spot_<bay> entity
// (else the BSP's entry), so a regenerated layout (maps/tools/gen_testbed.py) needs no change here. A spot is any tb_spot_* entity, so
// `pc_tb_goto fling_high` works as well. The teleport is Portal 2's own (point_teleport, floatorigin.cpp TeleportTo), not setpos.
// The Minecraft player: the body is teleported, then Minecraft is sent to it (mc.cpp McPlaceBody), as pcb_mc_place does.
#include "bridge.hpp"

#include "Modules/Tier1.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace bridge {

bool McModeActive();                                       // mc.cpp
bool McLevelIsLive();                                      // mc.cpp
Vector McFeetSrc();                                        // mc.cpp
bool McPlaced();                                           // mc.cpp
void McPlaceBody(const Vector &feet);                      // mc.cpp
bool TeleportTo(const std::string &target, const Vector &o, const QAngle &a);  // floatorigin.cpp
void MapIndexNames(const char *prefix, std::vector<std::string> &out);          // mapindex.cpp
bool MapIndexLookup(const char *name, Vector &origin, QAngle &angles);          // mapindex.cpp
void TestCheck(bool ok, const char *what, const char *detail);                  // testassert.cpp
int EntFieldOffset(void *ent, const char *field);                               // ents.cpp

namespace {

double NowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// the spot's origin and yaw: the live entity, else the map index
bool Spot(const std::string &name, Vector &origin, float &yaw) {
	auto live = EntFind(nullptr, name.c_str());
	if (!live.empty()) {
		origin = EntVec(live[0], "origin");
		yaw = EntVec(live[0], "angles").y;
		return true;
	}
	QAngle a{0, 0, 0};
	if (!MapIndexLookup(name.c_str(), origin, a)) return false;
	yaw = a.y;
	return true;
}

// the bays of this map: the <bay> of every tb_go_<bay> relay
std::vector<std::string> Bays() {
	std::vector<std::string> names, bays;
	MapIndexNames("tb_go_", names);
	for (const std::string &n : names) bays.push_back(n.substr(6));
	return bays;
}

struct Goto {
	bool active = false;
	std::string bay;
	Vector feet{0, 0, 0};
	float yaw = 0;
	bool mc = false;
	double since = 0, lastIssue = 0;
	int issues = 0;
} g_goto;
std::string g_lastGoto = "pc_tb_goto has not been run";
bool g_lastGotoOk = false;

bool YawOk(float want) { return std::fabs(std::remainder(engine.ViewAngles().y - want, 360.0f)) <= 10.0f; }

void Issue() {
	const QAngle a{0, g_goto.yaw, 0};
	TeleportTo("!player", g_goto.feet, a);
	engine.SetAngles(a);
	g_goto.lastIssue = NowS();
	++g_goto.issues;
}

void Finish(bool ok, const char *detail) {
	char buf[400];
	std::snprintf(buf, sizeof buf, "pc_tb_goto %s: %s %s as the %s", g_goto.bay.c_str(), ok ? "OK" : "FAILED", detail, g_goto.mc ? "Minecraft player" : "Chell");
	console->Print("[bridge] %s\n", buf);
	g_lastGoto = buf;
	g_lastGotoOk = ok;
	g_goto.active = false;
}

}  // namespace

// once per server frame (mapindex.cpp MapIndexFrame)
void TestbedFrame() {
	if (!g_goto.active) return;
	const double now = NowS();
	const Vector body = PlayerOrigin();
	const float dh = std::hypot(body.x - g_goto.feet.x, body.y - g_goto.feet.y);
	const bool bodyAt = dh <= 8.0f && body.z <= g_goto.feet.z + 4.0f && body.z >= g_goto.feet.z - 16.0f;
	bool arrived = bodyAt;
	char detail[300];
	if (g_goto.mc) {
		const Vector mcf = McFeetSrc();
		const float dm = std::hypot(mcf.x - g_goto.feet.x, mcf.y - g_goto.feet.y);
		arrived = McPlaced() && dm <= 24.0f && dh <= 24.0f;
		std::snprintf(detail, sizeof detail, "at (%.1f %.1f %.1f), %.1f u from tb_spot_%s (%.1f %.1f %.1f), facing yaw %.0f", mcf.x, mcf.y, mcf.z, dm, g_goto.bay.c_str(), g_goto.feet.x,
		              g_goto.feet.y, g_goto.feet.z, engine.ViewAngles().y);
	} else {
		std::snprintf(detail, sizeof detail, "at (%.1f %.1f %.1f), %.1f u from tb_spot_%s (%.1f %.1f %.1f), facing yaw %.0f", body.x, body.y, body.z, dh, g_goto.bay.c_str(), g_goto.feet.x,
		              g_goto.feet.y, g_goto.feet.z, engine.ViewAngles().y);
	}
	if (arrived && YawOk(g_goto.yaw)) return Finish(true, detail);
	if (arrived) engine.SetAngles(QAngle{0, g_goto.yaw, 0});  // (the body is there: only the view is left to turn)
	// Chell: a teleport the engine dropped is issued again (the Minecraft player's body is re-sent by mc.cpp's StandTeleportFrame)
	if (!g_goto.mc && !bodyAt && now - g_goto.lastIssue > 0.5 && g_goto.issues < 8) Issue();
	if (now - g_goto.since > 8.0) {
		std::string why = detail;
		if (!arrived) why += g_goto.mc ? " (Minecraft did not reach the spot or does not stand on the ground there)" : " (the body is not at the spot)";
		else why += " (the view did not turn to the spot's facing)";
		Finish(false, why.c_str());
	}
}

CON_COMMAND(pc_tb, "pc_tb | pc_tb <bay> go|reset - the testbed's bays; fire a bay's tb_go_<bay> / tb_reset_<bay> relay (works without sv_cheats)\n") {
	const std::vector<std::string> bays = Bays();
	if (args.ArgC() < 2) {
		if (bays.empty()) return console->Print("[bridge] pc_tb: this map has no testbed bays (no tb_go_<bay> relays in its entity lump)\n");
		console->Print("[bridge] pc_tb: %zu bays (pc_tb <bay> go|reset, pc_tb_goto <bay>)\n", bays.size());
		for (const std::string &b : bays) {
			Vector o(0, 0, 0);
			float yaw = 0;
			const bool spot = Spot("tb_spot_" + b, o, yaw);
			const bool live = !EntFind(nullptr, ("tb_go_" + b).c_str()).empty() && !EntFind(nullptr, ("tb_reset_" + b).c_str()).empty();
			if (spot) console->Print("[bridge]   %-10s spot (%.0f %.0f %.0f) yaw %.0f%s\n", b.c_str(), o.x, o.y, o.z, yaw, live ? "" : "  (relays not live)");
			else console->Print("[bridge]   %-10s (no tb_spot_%s)%s\n", b.c_str(), b.c_str(), live ? "" : "  (relays not live)");
		}
		return;
	}
	const std::string bay = args[1];
	if (std::find(bays.begin(), bays.end(), bay) == bays.end()) return console->Print("[bridge] pc_tb: FAILED no bay '%s' (pc_tb lists them)\n", bay.c_str());
	if (args.ArgC() < 3) {
		Vector o(0, 0, 0);
		float yaw = 0;
		if (Spot("tb_spot_" + bay, o, yaw)) console->Print("[bridge] pc_tb: %s: spot (%.0f %.0f %.0f) yaw %.0f; pc_tb %s go | reset\n", bay.c_str(), o.x, o.y, o.z, yaw, bay.c_str());
		return;
	}
	const std::string what = args[2];
	if (what != "go" && what != "reset") return console->Print("[bridge] pc_tb: FAILED unknown action '%s' (go or reset)\n", what.c_str());
	const std::string relay = "tb_" + what + "_" + bay;
	auto live = EntFind(nullptr, relay.c_str());
	if (live.empty()) return console->Print("[bridge] pc_tb: FAILED no live entity %s (is the level loaded?)\n", relay.c_str());
	const bool ok = EntInput(live[0], "Trigger");
	console->Print("[bridge] pc_tb: %s %s (Trigger %s)\n", ok ? "fired" : "FAILED to fire", relay.c_str(), ok ? "accepted" : "refused by the entity");
}

CON_COMMAND(pc_tb_goto, "pc_tb_goto <bay> - put the current player (Chell or the Minecraft player) at tb_spot_<bay> facing as the spot does; prints OK or FAILED (works without sv_cheats)\n") {
	if (args.ArgC() < 2) return console->Print("%s", pc_tb_goto.ThisPtr()->m_pszHelpString);
	g_goto = Goto();
	g_goto.bay = args[1];
	g_lastGotoOk = false;
	if (!McLevelIsLive()) {
		g_lastGoto = std::string("pc_tb_goto ") + args[1] + ": FAILED the level is not live yet";
		return console->Print("[bridge] %s\n", g_lastGoto.c_str());
	}
	Vector o(0, 0, 0);
	float yaw = 0;
	if (!Spot("tb_spot_" + g_goto.bay, o, yaw)) {
		g_lastGoto = std::string("pc_tb_goto ") + args[1] + ": FAILED no tb_spot_" + args[1] + " in this map (pc_tb lists the bays)";
		return console->Print("[bridge] %s\n", g_lastGoto.c_str());
	}
	g_goto.feet = o, g_goto.yaw = yaw, g_goto.mc = McModeActive(), g_goto.since = NowS(), g_goto.active = true;
	Issue();
	if (g_goto.mc) McPlaceBody(o);
	console->Print("[bridge] pc_tb_goto %s: teleporting the %s to (%.1f %.1f %.1f) yaw %.0f\n", args[1], g_goto.mc ? "Minecraft player" : "player", o.x, o.y, o.z, yaw);
}

// pc_tb_gun: Chell's dual portal gun the way Valve's flow gives it (the map's logic_auto does this 1.5 s after the level starts): the console
// commands give_portalgun and upgrade_portalgun through the map's point_servercommand `@command`. Also lists the weapon_portalgun entities.
CON_COMMAND(pc_tb_gun, "pc_tb_gun - give Chell the dual portal gun (give_portalgun + upgrade_portalgun through @command, no sv_cheats); lists the portal gun entities\n") {
	auto cmd = EntFind("point_servercommand", "@command");
	if (cmd.empty()) console->Print("[bridge] pc_tb_gun: FAILED no point_servercommand @command in this map (global_ents)\n");
	else {
		const bool a = EntInput(cmd[0], "Command", "give_portalgun"), b = EntInput(cmd[0], "Command", "upgrade_portalgun");
		console->Print("[bridge] pc_tb_gun: give_portalgun %s, upgrade_portalgun %s\n", a ? "sent" : "REFUSED", b ? "sent" : "REFUSED");
	}
	for (void *g : EntFind("weapon_portalgun")) {
		int o1 = EntFieldOffset(g, "m_bCanFirePortal1"), o2 = EntFieldOffset(g, "m_bCanFirePortal2");
		const Vector o = EntVec(g, "origin");
		console->Print("[bridge] pc_tb_gun: weapon_portalgun %p at (%.0f %.0f %.0f): can fire portal 1: %s, portal 2: %s\n", g, o.x, o.y, o.z,
		               o1 < 0 ? "?" : *((const unsigned char *)g + o1) ? "yes" : "no", o2 < 0 ? "?" : *((const unsigned char *)g + o2) ? "yes" : "no");
	}
}

// (bridge #34) every prop_portal with what Portal 2 itself holds about it: the diagnostic for a map-placed portal that does not transit Chell
// (the testbed's floor portal: full1..full3). Field names are tried by datamap name; "?" = the datamap has no such field.
CON_COMMAND(pc_tb_portals, "pc_tb_portals - (bridge #34) list every prop_portal: name, group, colour, activated, origin, angles, Portal 2's own linked-portal handle and flags\n") {
	for (void *p : EntFind("prop_portal")) {
		const Vector o = EntVec(p, "origin"), a = EntVec(p, "angles");
		auto rd = [&](const char *f, int bytes) -> long long {
			const int off = EntFieldOffset(p, f);
			if (off < 0) return -1;
			return bytes == 1 ? (long long)*((const unsigned char *)p + off) : (long long)*(const unsigned int *)((const char *)p + off);
		};
		console->Print("[bridge] pc_tb_portals: %p '%s' group %s PortalTwo %s Activated %s origin (%.2f %.2f %.2f) angles (%.1f %.1f %.1f) | m_bActivated %lld m_bIsPortal2 %lld m_hLinkedPortal 0x%llx m_iLinkageGroupID %lld\n", p,
		               EntKV(p, "targetname").c_str(), EntKV(p, "LinkageGroupID").c_str(), EntKV(p, "PortalTwo").c_str(), EntKV(p, "Activated").c_str(), o.x, o.y, o.z, a.x, a.y, a.z,
		               rd("m_bActivated", 1), rd("m_bIsPortal2", 1), rd("m_hLinkedPortal", 4), rd("m_iLinkageGroupID", 4));
	}
}

// (bridge #68) every entity whose class or name contains the substring: class, name, origin, angles, parent. A probe for classes the bridge does not know.
CON_COMMAND(pc_ents, "pc_ents <substring> - (bridge #68) list every entity whose classname or targetname contains the text\n") {
	if (args.ArgC() < 2) return;
	int n = 0;
	for (void *e : EntFind(nullptr)) {
		const std::string c = EntKV(e, "classname"), t = EntKV(e, "targetname");
		if (c.find(args[1]) == std::string::npos && t.find(args[1]) == std::string::npos) continue;
		const Vector o = EntVec(e, "origin"), a = EntVec(e, "angles");
		console->Print("[bridge] pc_ents: %p %s '%s' origin (%.1f %.1f %.1f) angles (%.1f %.1f %.1f) group '%s' activated '%s'\n", e, c.c_str(), t.c_str(), o.x, o.y, o.z, a.x, a.y, a.z, EntKV(e, "LinkageGroupID").c_str(), EntKV(e, "Activated").c_str());
		if (++n >= 60) break;
	}
}

CON_COMMAND(pc_ent_handles, "pc_ent_handles - (bridge #68, probe) for each linked_portal_door and portal_base2D: its handle fields, and which entity each resolves to\n") {
	auto all = EntFind(nullptr);
	auto resolve = [&](unsigned h, int off) -> void * {
		for (void *e : all)
			if (*(const unsigned *)((const char *)e + off) == h) return e;
		return nullptr;
	};
	for (const char *cls : {"linked_portal_door", "portal_base2D"})
		for (void *e : EntFind(cls)) {
			console->Print("[bridge] pc_ent_handles: %s %p '%s' self+4 0x%x self+8 0x%x\n", cls, e, EntKV(e, "targetname").c_str(), *(const unsigned *)((const char *)e + 4), *(const unsigned *)((const char *)e + 8));
			for (const char *f : {"m_hPortal", "m_hPartner", "m_hLinkedPortal", "m_hLocalPortal"}) {
				const int off = EntFieldOffset(e, f);
				if (off < 0) continue;
				const unsigned h = *(const unsigned *)((const char *)e + off);
				void *r4 = resolve(h, 4);
				console->Print("[bridge] pc_ent_handles:   %s +%d = 0x%x -> via +4 %p (%s)\n", f, off, h, r4, r4 ? EntKV(r4, "classname").c_str() : "-");
			}
		}
}
std::string MapVersionKey(const std::string &map);  // mapindex.cpp
const char *McMapName();                           // mc.cpp
CON_COMMAND(pc_map_version, "pc_map_version - (bridge #34) the loaded map's version key (a hash of its BSP): what the collision cache and the mover shapes are keyed by\n") {
	console->Print("[bridge] pc_map_version: %s = %s\n", McMapName(), MapVersionKey(McMapName()).c_str());
}

CON_COMMAND(pc_tb_goto_assert, "pc_tb_goto_assert - (tests) the last pc_tb_goto arrived (pass) or FAILED / is still going (fail; a failed assert ends a test run)\n") {
	const std::string v = g_goto.active ? "pc_tb_goto is still going: " + g_lastGoto : g_lastGoto;
	TestCheck(!g_goto.active && g_lastGotoOk, "pc_tb_goto", v.c_str());
}

}  // namespace bridge
