// Portalcraft bridge: solid Minecraft blocks inside Portal 2 (Phase 0 stretch).
//
// Prototype of "a block Chell stands on and a Portal 2 cube lands on": the
// plugin spawns an invisible server entity per block through the engine's
// server-tools interface (CreateEntityByName / SetKeyValue / DispatchSpawn,
// SourceAutoRecord's offsets), a prop_dynamic_override with SOLID_BBOX whose
// bounding box is the block:
//   * engine traces (player movement, the portal gun, bullets) collide with a
//     SOLID_BBOX entity's box;
//   * a SOLID_BBOX prop creates a box-shaped vphysics shadow, so physics props
//     (cubes, spheres) land on it.
// The box comes from the model's hull; models/props_vac_anim/metal_box_lowres.mdl
// has a 39.6 u cube hull centred on its origin, i.e. one block (40 u) minus 0.2 u
// per side. Phase 1 replaces this with one static collision entity per chunk
// section (see README).
#include "bridge.hpp"

#include "Modules/Tier1.hpp"
#include "Offsets.hpp"

#include <cstdlib>
#include <cstring>

namespace bridge {

static Interface *g_serverTools = nullptr;
using _CreateEntityByName = void *(__rescall *)(void *, const char *);
using _DispatchSpawn = void(__rescall *)(void *, void *);
using _SetKeyValueChar = bool(__rescall *)(void *, void *, const char *, const char *);
static _CreateEntityByName CreateEntityByName = nullptr;
static _DispatchSpawn DispatchSpawn = nullptr;
static _SetKeyValueChar SetKeyValueChar = nullptr;

static const char *kColliderModel = "models/props_vac_anim/metal_box_lowres.mdl";
static int g_colliders = 0;
static bool g_colliderLog = true;
void SetColliderLog(bool on) { g_colliderLog = on; }

bool ColliderInit() {
	g_serverTools = Interface::Create(MODULE_NAME("server"), "VSERVERTOOLS001", false);
	if (!g_serverTools) return false;
	CreateEntityByName = g_serverTools->Original<_CreateEntityByName>(Offsets::CreateEntityByName);
	DispatchSpawn = g_serverTools->Original<_DispatchSpawn>(Offsets::DispatchSpawn);
	SetKeyValueChar = g_serverTools->Original<_SetKeyValueChar>(Offsets::SetKeyValueChar);
	return CreateEntityByName && DispatchSpawn && SetKeyValueChar;
}

void ColliderShutdown() {
	Interface::Delete(g_serverTools);
	g_serverTools = nullptr;
}

void *Spawn(const char *cls, std::initializer_list<std::pair<const char *, std::string>> kvs) {
	if (!g_serverTools) return nullptr;
	void *ent = CreateEntityByName(g_serverTools->ThisPtr(), cls);
	if (!ent) {
		console->Warn("[bridge] CreateEntityByName(%s) failed\n", cls);
		return nullptr;
	}
	for (auto &kv : kvs) SetKeyValueChar(g_serverTools->ThisPtr(), ent, kv.first, kv.second.c_str());
	DispatchSpawn(g_serverTools->ThisPtr(), ent);
	return ent;
}

// A keyvalue on a live entity (e.g. a targetname for an entity the plugin must address by name).
bool EntSetKV(void *ent, const char *key, const char *value) {
	if (!g_serverTools || !ent) return false;
	SetKeyValueChar(g_serverTools->ThisPtr(), ent, key, value);
	return true;
}

static std::string Vec(float x, float y, float z) {
	char b[96];
	std::snprintf(b, sizeof b, "%.2f %.2f %.2f", x, y, z);
	return b;
}

// A solid block with its min corner at mins.
void *SpawnBlockCollider(const Vector &mins) {
	float h = kUnitsPerBlock / 2;
	char name[32];
	std::snprintf(name, sizeof name, "pcb_col_%d", g_colliders++);
	void *e = Spawn("prop_dynamic_override", {{"targetname", name},
	                                          {"model", kColliderModel},
	                                          {"origin", Vec(mins.x + h, mins.y + h, mins.z + h)},
	                                          {"angles", "0 0 0"},
	                                          {"solid", "2"},         // SOLID_BBOX
	                                          {"rendermode", "10"},   // don't render; the bridge draws the block
	                                          {"disableshadows", "1"},
	                                          {"disablereceiveshadows", "1"}});
	if (g_colliderLog) console->Print("[bridge] collider %s at (%.1f %.1f %.1f) -> %p\n", name, mins.x, mins.y, mins.z, e);
	return e;
}

CON_COMMAND(pcb_solid_block, "pcb_solid_block <x> <y> <z> [kind] - a drawn block plus an invisible solid collider (min corner x y z)\n") {
	if (args.ArgC() < 4) return console->Print("%s", pcb_solid_block.ThisPtr()->m_pszHelpString);
	Vector mins((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3]));
	g_blocks.push_back({mins, args.ArgC() > 4 ? std::atoi(args[4]) : 0});
	SpawnBlockCollider(mins);
}

void PropCollideRescanSoon();  // propcollide.cpp
CON_COMMAND(pcb_spawn_cube, "pcb_spawn_cube <x> <y> <z> [name=pcb_cube] - spawn a Portal 2 weighted storage cube (prop_weighted_cube)\n") {
	if (args.ArgC() < 4) return console->Print("%s", pcb_spawn_cube.ThisPtr()->m_pszHelpString);
	void *e = Spawn("prop_weighted_cube", {{"targetname", args.ArgC() > 4 ? args[4] : "pcb_cube"}, {"origin", Vec((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3]))}});
	console->Print("[bridge] weighted cube at (%s %s %s) -> %p\n", args[1], args[2], args[3], e);
	PropCollideRescanSoon();
}

CON_COMMAND(pcb_spawn_ent, "pcb_spawn_ent <classname> <x> <y> <z> <yaw> <targetname> [key value]... - spawn any server entity (tests: a pedestal button)\n") {
	if (args.ArgC() < 7) return console->Print("%s", pcb_spawn_ent.ThisPtr()->m_pszHelpString);
	char ang[48];
	std::snprintf(ang, sizeof ang, "0 %s 0", args[5]);
	void *e = nullptr;
	if (args.ArgC() >= 9)
		e = Spawn(args[1], {{"targetname", args[6]}, {"origin", Vec((float)std::atof(args[2]), (float)std::atof(args[3]), (float)std::atof(args[4]))}, {"angles", ang}, {args[7], args[8]}});
	else e = Spawn(args[1], {{"targetname", args[6]}, {"origin", Vec((float)std::atof(args[2]), (float)std::atof(args[3]), (float)std::atof(args[4]))}, {"angles", ang}});
	console->Print("[bridge] %s '%s' at (%s %s %s) yaw %s -> %p\n", args[1], args[6], args[2], args[3], args[4], args[5], e);
}

// What is directly under a point: a ray down against everything (world + entities).
// Put portal N flat on whatever floor is below x y z (test helper).
CON_COMMAND(pcb_portal_floor, "pcb_portal_floor <0|1> <x> <y> <z> - trace down from x y z and put portal N on that floor\n") {
	if (args.ArgC() < 5) return console->Print("%s", pcb_portal_floor.ThisPtr()->m_pszHelpString);
	Vector p((float)std::atof(args[2]), (float)std::atof(args[3]), (float)std::atof(args[4]));
	CGameTrace tr;
	engine.Trace(p, p - Vector(0, 0, 4096), Vector(0, 0, 0), MASK_SOLID_BRUSHONLY, tr, true);
	char buf[160];
	std::snprintf(buf, sizeof buf, "portal_place 0 %d %.2f %.2f %.2f -90 0 0", std::atoi(args[1]), tr.endpos.x, tr.endpos.y, tr.endpos.z + 0.5f);
	engine.Cmd(buf);
	console->Print("[bridge] floor at z %.2f (surface '%s') -> %s\n", tr.endpos.z, tr.surface.name ? tr.surface.name : "?", buf);
}

CON_COMMAND(pcb_probe_down, "pcb_probe_down <x> <y> <z> - trace straight down (world and entities) and print the hit height\n") {
	if (args.ArgC() < 4) return console->Print("%s", pcb_probe_down.ThisPtr()->m_pszHelpString);
	Vector p((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3]));
	CGameTrace tr;
	engine.Trace(p, p - Vector(0, 0, 2048), Vector(0, 0, 0), MASK_SOLID, tr, false);
	console->Print("[bridge] probe_down (%.1f %.1f %.1f): hit z %.2f, entity %p, normal z %.2f, contents 0x%x\n", p.x, p.y, p.z, tr.endpos.z, tr.m_pEnt, tr.plane.normal.z,
	               tr.contents);
}

CON_COMMAND(pcb_ents_near, "pcb_ents_near <x y z> [radius=400] - server entities whose origin is within radius (classname, name, origin; debug)\n") {
	if (args.ArgC() < 4) return;
	Vector p((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3]));
	const float r = args.ArgC() > 4 ? (float)std::atof(args[4]) : 400.0f;
	int n = 0;
	for (void *e : EntFind(nullptr)) {
		const Vector o = EntVec(e, "origin");
		if ((o - p).Length() > r) continue;
		++n;
		console->Print("[bridge] near: %s '%s' at (%.0f %.0f %.0f) model '%s'\n", EntKV(e, "classname").c_str(), EntKV(e, "targetname").c_str(), o.x, o.y, o.z, EntKV(e, "model").c_str());
	}
	console->Print("[bridge] near (%.0f %.0f %.0f) r %.0f: %d entities\n", p.x, p.y, p.z, r, n);
}
CON_COMMAND(pcb_trace_box, "pcb_trace_box <x y z> <x2 y2 z2> [half=18] [mask=solid|player|npc] - sweep a box (a cube's hull) and print what it hits (debug)\n") {
	if (args.ArgC() < 7) return console->Print("%s", pcb_trace_box.ThisPtr()->m_pszHelpString);
	Vector a((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3])), b((float)std::atof(args[4]), (float)std::atof(args[5]), (float)std::atof(args[6]));
	const float h = args.ArgC() > 7 ? (float)std::atof(args[7]) : 18.0f;
	const char *m = args.ArgC() > 8 ? args[8] : "solid";
	const unsigned mask = !std::strcmp(m, "player") ? MASK_PLAYERSOLID : !std::strcmp(m, "npc") ? MASK_NPCSOLID : MASK_SOLID;
	CGameTrace tr;
	engine.Trace(a, b, Vector(h, h, h), mask, tr, false);
	std::string who = tr.m_pEnt ? EntKV(tr.m_pEnt, "classname") + " '" + EntKV(tr.m_pEnt, "targetname") + "'" : "-";
	console->Print("[bridge] trace_box (%.0f %.0f %.0f)->(%.0f %.0f %.0f) half %.0f %s: frac %.3f ss %d end (%.1f %.1f %.1f) n (%.2f %.2f %.2f) ent %s surface '%s' contents 0x%x\n", a.x, a.y, a.z, b.x, b.y, b.z,
	               h, m, tr.fraction, tr.startsolid, tr.endpos.x, tr.endpos.y, tr.endpos.z, tr.plane.normal.x, tr.plane.normal.y, tr.plane.normal.z, who.c_str(), tr.surface.name ? tr.surface.name : "", tr.contents);
}

}  // namespace bridge
