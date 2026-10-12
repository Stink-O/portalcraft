// Portalcraft bridge: Chell inside Minecraft (each player's rules travel with them).
//
// While Chell is in the Minecraft region she is mirrored into Minecraft as an invisible, hittable
// stand-in (SkyCraft's actor table and SkyrimActorEntity, reversed: the host's player instead of its
// NPCs): hostile mobs hunt her (SkyCombat.huntChell), pressure plates and tripwires trigger under her,
// arrows and melee hit her. Minecraft reports each tick's hit on her (kEvHitActor: damage after its own
// modifiers, knockback) and every explosion (kEvExplosion); she takes them as Portal 2 damage and a push.
#include "bridge.hpp"
#include "coords.hpp"
#include "link.hpp"

#include "Modules/Tier1.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bridge {

using namespace skycraft::proto;
bool ClipHasRegion();
bool ClipRegionContains(const Vector &p);
Vector PlayerVelocity();                 // puppet.cpp
void PuppetLaunch(const Vector &vel);    // puppet.cpp
bool McModeActive();                     // mc.cpp
int CombatActors(ActorRecord *out, int max);  // combat.cpp (bridge #19): Portal 2 turrets/cubes as stand-ins
bool CombatOnHit(const McEvent &ev);          // combat.cpp
void CombatOnExplosion(const McEvent &ev);    // combat.cpp
void CombatFrame();                           // combat.cpp
void CombatLevelInit();                       // combat.cpp
void CombatOnMcDied();                        // combat.cpp

static const uint32_t kChellFormId = 0x0C4E11;  // SkyCombat.CHELL_FORM_ID
static bool g_enabled = true;
static float g_damageScale = 5.0f;  // Minecraft health 20 -> Portal 2 health 100
static int g_hits = 0, g_explosions = 0, g_log = 30;
static float g_damageTaken = 0;
static bool g_mirrored = false;
// bridge #19: Chell is mirrored in the chambers too (mobs there hunt her), and the hidden Minecraft player follows her there
// (mc.cpp) so the chambers' entities stay loaded, ticking and drawn (scene.cpp) while she is the player
static bool g_inChambers = true;
bool ChellProxyInChambers() { return g_inChambers && link::McAlive(); }

// (cached: a full entity walk per frame is too slow; refreshed every 2 s, and on a level change)
static void *g_player = nullptr;
static int g_playerAge = 0;
void ChellProxyLevelLive() { g_player = nullptr, CombatLevelInit(); }
static void *PlayerEnt() {
	if (!g_player || ++g_playerAge > 240) {
		auto v = EntFind("player");
		g_player = v.empty() ? nullptr : v[0];
		g_playerAge = 0;
	}
	return g_player;
}

static void Damage(float p2, const char *what) {
	void *pl = PlayerEnt();
	if (!pl || !(p2 > 0) || p2 > 10000) return;  // (a removed stand-in reports an absurd hit)
	int hp = std::atoi(EntKV(pl, "health").c_str());
	int left = hp - (int)std::lround(p2);
	char buf[16];
	std::snprintf(buf, sizeof buf, "%d", left < 0 ? 0 : left);
	EntInput(pl, "SetHealth", buf);
	g_damageTaken += p2;
	if (g_log > 0) --g_log, console->Print("[bridge] chell proxy: %s, %.1f Portal 2 damage (health %d -> %s)\n", what, p2, hp, buf);
}

// Main thread, every frame (mc.cpp, after the link work).
void ChellProxyFrame() {
	if (!link::Base()) return;
	static ActorRecord recs[64];
	ActorRecord &rec = recs[0];
	rec = ActorRecord{};
	uint32_t n = 0;
	const Vector feet = PlayerOrigin();
	if (g_enabled && ClipHasRegion() && !McModeActive() && (ClipRegionContains(feet) || g_inChambers)) {
		double x, y, z;
		SrcToMcD(feet, x, y, z);
		rec.formId = kChellFormId;
		rec.flags = kActorHostile;
		rec.x = (float)x, rec.y = (float)y, rec.z = (float)z;
		rec.yaw = SrcYawToMc(engine.ViewAngles().y);
		rec.width = 32.0f / kUnitsPerBlock, rec.height = 72.0f / kUnitsPerBlock;
		void *pl = PlayerEnt();
		rec.healthFrac = pl ? std::atoi(EntKV(pl, "health").c_str()) / 100.0f : 1.0f;
		rec.level = 1;
		std::snprintf(rec.name, sizeof rec.name, "Chell");
		n = 1;
	}
	const bool chell = n != 0;
	// bridge #19: Portal 2's turrets, cubes and props near the Minecraft player (combat.cpp), after Chell's slot
	static uint32_t lastTotal = 0;
	const uint32_t total = n + (uint32_t)CombatActors(recs + n, 63);
	if (total || lastTotal) link::WriteActors(recs, total);
	lastTotal = total;
	if (chell != g_mirrored) console->Print("[bridge] chell proxy: %s\n", chell ? "Chell mirrored into Minecraft" : "Chell's stand-in removed");
	g_mirrored = chell;
	CombatFrame();

	McEvent ev;
	for (int guard = 0; guard < 64 && link::PopEvent(ev); ++guard) {
		if (ev.type == kEvExplosion) CombatOnExplosion(ev);  // bridge #19: pushes Portal 2's props in the chambers
		if (ev.type == kEvPlayerDied) CombatOnMcDied();       // bridge #19: the host owns death
		if (ev.type == kEvHitActor && CombatOnHit(ev)) {
			continue;  // bridge #19: a Portal 2 turret / cube / prop (combat.cpp)
		} else if (ev.type == kEvHitActor && ev.formId == kChellFormId) {
			++g_hits;
			Damage(ev.a * g_damageScale, ev.flags & kHitProjectile ? "hit by a projectile" : "hit");
			if (ev.d > 0) {
				// Minecraft knockback (strength ~0.4 = 8 blocks/s): direction (b, c) in Minecraft x/z
				float s = ev.d * 20.0f * kUnitsPerBlock * 0.6f;
				Vector push(ev.b * s, -ev.c * s, std::fmin(ev.d, 0.4f) * 20.0f * kUnitsPerBlock * 0.5f);
				PuppetLaunch(PlayerVelocity() + push);
			}
		} else if (ev.type == kEvExplosion && g_mirrored) {
			// Minecraft's explosion damage with full exposure: impact = 1 - distance / (2 radius)
			double x, y, z;
			SrcToMcD(PlayerOrigin() + Vector(0, 0, 36), x, y, z);
			double dx = x - ev.a, dy = y - ev.b, dz = z - ev.c, dist = std::sqrt(dx * dx + dy * dy + dz * dz);
			double impact = 1.0 - dist / (2.0 * ev.d);
			if (impact <= 0) continue;
			++g_explosions;
			float mc = (float)((impact * impact + impact) / 2.0 * 7.0 * (2.0 * ev.d) + 1.0);
			Damage(mc * g_damageScale, "caught in a Minecraft explosion");
			if (dist > 1e-3) {
				float s = (float)(impact * 20.0 * kUnitsPerBlock * 0.8);
				PuppetLaunch(PlayerVelocity() + Vector((float)(dx / dist) * s, (float)(-dz / dist) * s, (float)(std::fmax(0.3, dy / dist)) * s));
			}
		}
	}
}

CON_COMMAND(pcb_chell_proxy, "pcb_chell_proxy [0|1] [damage_scale=5] [in_chambers 0|1] - Chell mirrored into Minecraft (mobs, plates, arrows, explosions hit her)\n") {
	if (args.ArgC() > 1) g_enabled = std::atoi(args[1]) != 0;
	if (args.ArgC() > 2) g_damageScale = (float)std::atof(args[2]);
	if (args.ArgC() > 3) g_inChambers = std::atoi(args[3]) != 0;
	console->Print("[bridge] chell proxy %d (mirrored %d): %d hits, %d explosions, %.0f damage taken, scale %.1f\n", g_enabled, g_mirrored, g_hits, g_explosions, g_damageTaken,
	               g_damageScale);
}

void TestCheck(bool ok, const char *what, const char *detail);
CON_COMMAND(pcb_assert_chell_hit, "pcb_assert_chell_hit [min_hits=1] | reset - Minecraft hit Chell's stand-in at least min times since the reset\n") {
	static int base = 0;
	if (args.ArgC() > 1 && !std::strcmp(args[1], "reset")) return void(base = g_hits + g_explosions);
	int minHits = args.ArgC() > 1 ? std::atoi(args[1]) : 1;
	char buf[160];
	std::snprintf(buf, sizeof buf, "%d hits and explosions since the reset, %.0f damage taken in total, mirrored %d", g_hits + g_explosions - base, g_damageTaken, g_mirrored);
	TestCheck(g_hits + g_explosions - base >= minHits, "Minecraft hits Chell", buf);
}

}  // namespace bridge

namespace bridge {
// Test hook: Minecraft server commands for the mod (SkyCombat.runTestCommands reads the file).
static void QueueMcCommand(const char *line) {
	char path[512];
	std::snprintf(path, sizeof path, "%s/mc_commands.txt", CacheDir().c_str());
	if (FILE *f = std::fopen(path, "a")) std::fprintf(f, "%s\n", line), std::fclose(f);
	console->Print("[bridge] mc command queued: %s\n", line);
}
CON_COMMAND(pcb_mc_cmd, "pcb_mc_cmd <command...> - run a Minecraft server command (tests)\n") {
	if (args.ArgC() < 2) return;
	QueueMcCommand(args.m_pArgSBuffer + args.m_nArgv0Size);
}
CON_COMMAND(pcb_mc_profile, "pcb_mc_profile - start / stop Minecraft's own profiler (F3+L metrics; report in fabric/run/debug/profiling)\n") {
	char path[512];
	std::snprintf(path, sizeof path, "%s/mc_profile_request", CacheDir().c_str());
	if (FILE *f = std::fopen(path, "w")) std::fclose(f);
	console->Print("[bridge] Minecraft profiler toggle requested\n");
}
CON_COMMAND(pcb_mc_summon_near, "pcb_mc_summon_near <entity> <dx> <dz> - summon a Minecraft entity dx/dz blocks from Chell's feet (tests)\n") {
	if (args.ArgC() < 4) return;
	double x, y, z;
	SrcToMcD(PlayerOrigin(), x, y, z);
	char line[160];
	std::snprintf(line, sizeof line, "summon %s %.1f %.1f %.1f", args[1], x + std::atof(args[2]), y + 1.0, z + std::atof(args[3]));
	QueueMcCommand(line);
}
}  // namespace bridge
