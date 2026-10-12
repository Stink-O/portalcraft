// Portalcraft bridge (bridge #19, Owner B): combat between the Minecraft player and Portal 2 (SkyCraft's Combat.cpp,
// with Portal 2 as the host). Each player's rules travel with them:
//
//   * Minecraft hits Portal 2 things. Turrets, cubes and other physics props near the Minecraft player are mirrored into
//     Minecraft as invisible hittable stand-ins (SkyCraft's actor table + SkyrimActorEntity), so Minecraft's own combat
//     (swords, crits, sweeps, knockback enchantments, bows, tridents) targets them. Each tick's hit comes back as
//     kEvHitActor; Portal 2 applies it the way Portal 2 knocks things: a physics push (an env_physexplosion aimed along
//     Minecraft's knockback direction, no damage), so a turret tips over and shuts down by its own logic and a cube is
//     knocked about. Minecraft explosions (TNT, creepers: kEvExplosion) push Portal 2's physics props the same way.
//   * Portal 2 hurts the Minecraft player; Minecraft owns its health. In Minecraft mode the body's Portal 2 health is held
//     at a large buffer (so Portal 2 never kills the body itself); each frame's loss (turret bullets, lasers, goo,
//     crushers: whatever Portal 2's own damage rules do to the player) is refunded and sent to Minecraft as a hit
//     (kInHurt, Portal 2 damage / 5 = Minecraft damage, as Portal 2's 100 health is Minecraft's 20).
//   * Death: Minecraft's own death screen (the HUD overlay carries it). When the player respawns (Minecraft's Respawn
//     button), Portal 2 holds the body and Minecraft is brought to it (it respawned at its world spawn), and Portal 2's
//     player dies so the chamber restarts from its checkpoint, as a Portal 2 death does; Minecraft mode survives the
//     reload and the Minecraft player is put at the new spawn (mc.cpp's level-change flow).
#include "bridge.hpp"
#include "growth.hpp"
#include "coords.hpp"
#include "link.hpp"

#include "Modules/Tier1.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bridge {

using namespace skycraft::proto;

bool McModeActive();                                 // mc.cpp
bool McModeHeld();                                   // mc.cpp (bridge #55): the Minecraft player is the player (mode Minecraft, in the world or not)
const McState &McStateNow();                         // mc.cpp
void McPuppetTransit(const Vector &origin, const Vector &vel);  // mc.cpp: Portal 2 holds the body until Minecraft is at it
void EntLiveSet(std::unordered_set<void *> &out);    // ents.cpp
int EntFieldOffset(void *ent, const char *field);    // ents.cpp
void *Spawn(const char *cls, std::initializer_list<std::pair<const char *, std::string>> kvs);  // collider.cpp
bool ClipHasRegion();                                // clip.cpp
bool ClipRegionContains(const Vector &p);            // clip.cpp
void TestCheck(bool ok, const char *what, const char *detail);  // testassert.cpp

static double NowS() {
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}
static std::string Vec3(const Vector &v) {
	char b[64];
	std::snprintf(b, sizeof b, "%.1f %.1f %.1f", v.x, v.y, v.z);
	return b;
}

// ---- Portal 2 things as Minecraft stand-ins -----------------------------------------------------------------
struct ClassInfo {
	const char *cls, *name;
	float w, h;  // fallback size (blocks) when the collision bounds are not found
	bool centred;  // the origin is the centre (cubes), not the feet (turrets)
	float push;    // push scale (b19_combat2: 490 moved a cube 8 blocks; three sword hits only tilted a turret 31 deg)
};
static const ClassInfo kClasses[] = {
    {"npc_portal_turret_floor", "Turret", 0.8f, 1.5f, false, 0.45f},
    {"prop_weighted_cube", "Cube", 0.85f, 0.85f, true, 0.35f},
    {"prop_monster_box", "Frankenturret", 0.8f, 0.8f, true, 0.5f},
    {"prop_physics", "Prop", 0.8f, 0.8f, true, 0.35f},
    {"prop_physics_override", "Prop", 0.8f, 0.8f, true, 0.35f},
    {"prop_glass_futbol", "Futbol", 0.5f, 0.5f, true, 0.25f},
    // (bridge #60) every other thing Chell can bump or pick up
    {"prop_physics_multiplayer", "Prop", 0.8f, 0.8f, true, 0.35f},
    {"prop_exploding_futbol", "Futbol", 0.5f, 0.5f, true, 0.25f},
    {"prop_personality_sphere", "Core", 0.6f, 0.6f, true, 0.3f},
    {"prop_glados_core", "Core", 0.6f, 0.6f, true, 0.3f},
};
struct P2Actor {
	void *ent;
	uint32_t formId;
	int cls;
	Vector lo, hi;  // world box (Source) this frame
};
static std::vector<P2Actor> g_actors;
PCB_GAUGE("combat.actors", g_actors.size());
static std::unordered_set<void *> g_live;
PCB_GAUGE("combat.live", g_live.size());
static double g_scanAt = 0;
static uint32_t g_nextForm = 1;
static bool g_actorsOn = true;     // pcb_combat
static float g_range = 1600.0f;    // u around the Minecraft player
static int g_hits = 0, g_pushes = 0, g_explosionPushes = 0, g_log = 40;
static int g_offMins = -2, g_offMaxs = -2;
static const uint32_t kP2FormBase = 0x0B000000;

static void Scan(const Vector &feet) {
	std::vector<P2Actor> keep;
	std::unordered_set<void *> have;
	for (auto &a : g_actors)
		if (g_live.count(a.ent)) keep.push_back(a), have.insert(a.ent);
	for (void *e : g_live) {
		if (have.count(e)) continue;
		const std::string cls = EntKV(e, "classname");
		for (int c = 0; c < (int)(sizeof kClasses / sizeof kClasses[0]); ++c) {
			if (cls != kClasses[c].cls) continue;
			if ((EntVec(e, "origin") - feet).Length() > g_range * 1.5f) break;
			keep.push_back({e, kP2FormBase + (g_nextForm++ & 0xFFFFFF), c, {}, {}});
			break;
		}
	}
	g_actors.swap(keep);
}

static void Bounds(P2Actor &a) {
	const ClassInfo &ci = kClasses[a.cls];
	const Vector o = EntVec(a.ent, "origin");
	if (g_offMins == -2) g_offMins = EntFieldOffset(a.ent, "m_vecMins"), g_offMaxs = EntFieldOffset(a.ent, "m_vecMaxs");
	if (g_offMins >= 0 && g_offMaxs >= 0) {
		const Vector mn = *(const Vector *)((const char *)a.ent + g_offMins), mx = *(const Vector *)((const char *)a.ent + g_offMaxs);
		// (a rotated prop's local box: its largest half-extent all round, so a tipped turret is still covered)
		const float r = std::fmax(std::fmax(std::fmax(std::fabs(mn.x), std::fabs(mx.x)), std::fmax(std::fabs(mn.y), std::fabs(mx.y))), 4.0f);
		if (mx.z > mn.z && r < 200.0f) {
			const float zr = std::fmax(std::fabs(mn.z), std::fabs(mx.z));
			const float rr = std::fmax(r, zr * (ci.centred ? 1.0f : 0.0f));
			a.lo = o + Vector(-rr, -rr, ci.centred ? -rr : mn.z), a.hi = o + Vector(rr, rr, ci.centred ? rr : mx.z);
			return;
		}
	}
	const float hw = ci.w * kUnitsPerBlock * 0.5f, h = ci.h * kUnitsPerBlock;
	a.lo = o + Vector(-hw, -hw, ci.centred ? -h * 0.5f : 0), a.hi = o + Vector(hw, hw, ci.centred ? h * 0.5f : h);
}

// chellproxy.cpp: the actor table's Portal 2 entries this frame (Minecraft mode only: the Minecraft player is the one
// who fights them)
int CombatActors(ActorRecord *out, int max) {
	if (!g_actorsOn || !McModeActive() || !link::Base()) {
		g_actors.clear();
		return 0;
	}
	EntLiveSet(g_live);
	const Vector feet = PlayerOrigin();
	const double now = NowS();
	if (now - g_scanAt > 0.5) g_scanAt = now, Scan(feet);
	int n = 0;
	for (auto &a : g_actors) {
		if (n >= max) break;
		if (!g_live.count(a.ent)) continue;
		Bounds(a);
		const Vector c = (a.lo + a.hi) * 0.5f;
		if ((c - feet).Length() > g_range) continue;
		if (ClipHasRegion() && ClipRegionContains(c)) continue;
		ActorRecord &r = out[n++];
		std::memset(&r, 0, sizeof r);
		r.formId = a.formId;
		double x, y, z;
		SrcToMcD(Vector(c.x, c.y, a.lo.z), x, y, z);
		r.x = (float)x, r.y = (float)y, r.z = (float)z;
		r.yaw = SrcYawToMc(EntVec(a.ent, "angles").y);
		r.width = std::fmax(a.hi.x - a.lo.x, a.hi.y - a.lo.y) / kUnitsPerBlock;
		r.height = (a.hi.z - a.lo.z) / kUnitsPerBlock;
		r.healthFrac = 1.0f;
		r.level = 1;
		std::snprintf(r.name, sizeof r.name, "%s", kClasses[a.cls].name);
	}
	return n;
}

// A Portal 2 physics push at `at` (Source) of the given strength, on whatever physics objects are within `radius`.
static void Push(const Vector &at, float magnitude, float radius) {
	char mag[32], rad[32];
	std::snprintf(mag, sizeof mag, "%.0f", magnitude);
	std::snprintf(rad, sizeof rad, "%.0f", radius);
	// spawnflags 1: no damage (the push only); players are not pushed (no 2)
	void *e = Spawn("env_physexplosion", {{"origin", Vec3(at)}, {"magnitude", mag}, {"radius", rad}, {"spawnflags", "1"}});
	if (!e) return;
	EntInput(e, "Explode");
	EntInput(e, "Kill");
}

static float g_pushK = 1.0f;  // pcb_combat_push
// chellproxy.cpp: a Minecraft hit on one of our stand-ins. True if it was ours.
bool CombatOnHit(const McEvent &ev) {
	if ((ev.formId & 0xFF000000) != kP2FormBase) return false;
	auto it = std::find_if(g_actors.begin(), g_actors.end(), [&](const P2Actor &a) { return a.formId == ev.formId; });
	if (it == g_actors.end() || !g_live.count(it->ent)) return true;
	++g_hits;
	const Vector c = (it->lo + it->hi) * 0.5f;
	// Minecraft's knockback direction (MC x/z -> Source x/-y); none (a plain hit): away from the player
	Vector dir(ev.b, -ev.c, 0);
	if (dir.Length() < 0.1f) dir = c - PlayerOrigin(), dir.z = 0;
	const float len = dir.Length();
	dir = len > 1e-3f ? dir * (1.0f / len) : Vector(1, 0, 0);
	// A sword (7 damage) shoves a cube about a block; a sprint/knockback hit (d 0.9+) knocks a turret over
	// (bridge #60) ev.a is Minecraft's own damage (cooldown, weapon, crit already in it), ev.d the knockback (a sprint hit 0.9+); a bare-handed poke
	// (1 damage) now moves a cube a little instead of nothing
	const float magnitude = (120.0f + ev.a * 30.0f + ev.d * 500.0f) * g_pushK * kClasses[it->cls].push;
	// (a turret is pushed high on its body, above its centre of mass: a torque that tips it on a hard hit; Portal 2's own tipped-over check then kills it, a light poke only wobbles or slides it)
	const Vector at = it->cls == 0 ? Vector(c.x, c.y, it->lo.z + (it->hi.z - it->lo.z) * 0.85f) : c;
	Push(at - dir * 20.0f - Vector(0, 0, it->cls == 0 ? 0.0f : 6.0f), magnitude, 60.0f);
	++g_pushes;
	if (g_log > 0)
		--g_log, console->Print("[bridge] combat: Minecraft hit %s (form %08x) for %.1f, knockback %.2f along (%.2f %.2f): push %.0f at (%.0f %.0f %.0f)\n", kClasses[it->cls].name,
		                        ev.formId, ev.a, ev.d, dir.x, dir.y, magnitude, c.x, c.y, c.z);
	return true;
}

// chellproxy.cpp: a Minecraft explosion (TNT, creeper) pushes Portal 2's physics props in the chambers.
void CombatOnExplosion(const McEvent &ev) {
	const Vector at = McToSrc(ev.a, ev.b, ev.c);
	if (ClipHasRegion() && ClipRegionContains(at)) return;
	const float radius = std::fmax(ev.d, 1.0f) * 2.0f * kUnitsPerBlock;
	Push(at, (600.0f + ev.d * 400.0f) * g_pushK, radius);
	++g_explosionPushes;
	if (g_log > 0) --g_log, console->Print("[bridge] combat: Minecraft explosion (power %.1f) at (%.0f %.0f %.0f): Portal 2 physics pushed within %.0f u\n", ev.d, at.x, at.y, at.z, radius);
}

// ---- Portal 2 damage to the Minecraft player --------------------------------------------------------------------
static const int kBuffer = 100000;  // the body's Portal 2 health while the Minecraft player owns its health
static void *g_player = nullptr;
static bool g_buffered = false;
static int g_offHealth = -2, g_offMaxHealth = -2;
static float g_hurtSent = 0;  // Portal 2 damage forwarded, in total
static int g_hurtEvents = 0;
static bool g_hurtOn = true;  // pcb_combat_hurt
static float g_pending = 0;   // Portal 2 damage not yet sent (batched over 0.1 s, like Minecraft's i-frames batch hits)
// (bridge #32) the pending damage by its Portal 2 damage class (HostDamageClass): each class goes to Minecraft as its own damage source
enum { kHostClasses = 9 };
static float g_pendingBy[kHostClasses] = {};
static int g_offDmgType = -2;
static double g_pendingAt = 0;
static bool g_wasDead = false;
void CombatOnMcDied();
static int g_deaths = 0, g_respawns = 0, g_levelGen = 0, g_deathLevelGen = -1;
static double g_killAt = -1;
static int g_scriptKills = 0;      // (bridge #68) health writes that left the body at <= 0
static int g_hurtIgnored = 0;     // (bridge #34) health losses that were script/input writes, not damage
static int g_testHitFrames = 0;   // (bridge #34, tests) frames during which an untyped loss counts as damage (pcb_combat_p2hurt)
static const float kMaxHit = 1000.0f;  // (bridge #34) the most Portal 2 damage one forwarded hit carries
static bool g_p2Dying = false;

static std::unordered_map<long long, int> g_hostLight;  // cell -> Portal 2 light level sent (HostLightFrame)
PCB_GAUGE("combat.hostLight", g_hostLight.size());
static int g_hlX = 0, g_hlLayer = 0;
static void *Player() {
	if (g_player && !g_live.count(g_player)) g_player = nullptr;
	if (!g_player) {
		auto v = EntFind("player");
		g_player = v.empty() ? nullptr : v[0];
		g_offHealth = g_offMaxHealth = g_offDmgType = -2;
	}
	return g_player;
}
static int *Field(void *e, int &off, const char *name) {
	if (off == -2) off = EntFieldOffset(e, name);
	return off >= 0 ? (int *)((char *)e + off) : nullptr;
}

static void SetBuffer(void *pl, bool on) {
	int *mh = Field(pl, g_offMaxHealth, "m_iMaxHealth"), *h = Field(pl, g_offHealth, "m_iHealth");
	if (!mh || !h) return;
	if (on) {
		*mh = kBuffer;
		EntInput(pl, "SetHealth", "100000");
		if (*h != kBuffer) *h = kBuffer;
	} else {
		// (raw writes: the SetHealth input lowers health through TakeDamage, which would kill the body from 100000)
		*h = 100;
		*mh = 100;
	}
	g_buffered = on;
	console->Print("[bridge] combat: Portal 2 health %s (health %d, max %d)\n", on ? "held for the Minecraft player (Minecraft owns its health)" : "back to Portal 2", *h, *mh);
}

void McMechanicsFrame(const std::unordered_set<void *> &live);  // mcmechanics.cpp
void McMechanicsLevelInit();
void CombatLevelInit() {
	McMechanicsLevelInit();
	g_player = nullptr, g_buffered = false, g_actors.clear(), g_live.clear(), ++g_levelGen, g_pending = 0, g_p2Dying = false, g_killAt = -1;
	g_hostLight.clear(), g_hlX = g_hlLayer = 0;
	if (link::Base()) link::PushInput(kInHostLight, 0x100, 0, 0, 0);
}

static void HostLightFrame();
static void HazardFrame();
int McLaserBeam(void *e, Vector &a, Vector &b, bool &lethal);  // mcmechanics.cpp (bridge #59)
// ---- (bridge #32) Portal 2 damage types -> a damage class -> the Minecraft damage source ---------------------------------------------
// DMG_* bits (Source). The class is what the mod maps to a DamageSource (SkyCombat.hurtPlayer): 0 generic (turret bullets, anything else),
// 1 crush -> cramming, 2 burn (laser, fire) -> onFire, 3 goo (radiation, acid, poison, dissolve: toxic goo) -> lava, 4 drown -> drown,
// 5 blast -> explosion, 6 shock -> lightningBolt, 7 fall -> fall, 8 a kill volume's plain lethal damage -> outOfWorld.
static const char *const kClassNames[kHostClasses] = {"generic", "crush", "burn", "goo", "drown", "blast", "shock", "fall", "void"};
static const int kDmgFlagBits = (1 << 11) | (1 << 12) | (1 << 13) | (1 << 22) | (1 << 23) | (1 << 28);  // prevent physics force, gib hints, no ragdoll, physgun, direct
static int EvidenceTypes(const Vector &feet, bool *inHazard = nullptr);
static int HostDamageClass(int types, float lost) {
	const int k = types & ~kDmgFlagBits;
	if (k & (1 << 5)) return 7;
	if (k & ((1 << 14) | (1 << 19))) return 4;
	if (k & (1 << 0)) return 1;
	if (k & ((1 << 6) | (1 << 27))) return 5;
	if (k & ((1 << 3) | (1 << 21) | (1 << 24))) return 2;
	if (k & ((1 << 15) | (1 << 16) | (1 << 17) | (1 << 18) | (1 << 20) | (1 << 26))) return 3;
	if (k & (1 << 8)) return 6;
	if (k) return 0;
	return lost >= 500.0f ? 8 : 0;
}
static int g_hurtByClass[kHostClasses] = {};
static double g_lastHurtAt[kHostClasses] = {};  // (bridge #37) when Portal 2's own damage of each class last reached the body
// (bridge #37) mcmechanics.cpp: the host's own laser sweep adds burn damage; not within `within` seconds of Portal 2's own
bool CombatRecentHurtClass(int cls, double within) { return cls >= 0 && cls < kHostClasses && NowS() - g_lastHurtAt[cls] < within; }
void CombatInjectHurt(int cls, float p2Damage) {
	if (!McModeActive() || cls < 0 || cls >= kHostClasses || p2Damage <= 0) return;
	if (g_pending <= 0) g_pendingAt = NowS();
	const float hit = std::fmin(p2Damage, kMaxHit);
	g_pending += hit, g_pendingBy[cls] += hit;
}
// mc.cpp (via chellproxy.cpp), every frame after the link work
void CombatFrame() {
	if (!link::Base()) return;
	EntLiveSet(g_live);  // every frame: entities freed since (a level change, a removed stand-in) must never be read
	void *pl = Player();
	if (!pl) return;
	const bool mc = McModeActive();
	const bool dead = mc && (McStateNow().flags & kMcDead);
	const double now = NowS();
	// Minecraft's death and respawn
	if (mc && dead && !g_wasDead) CombatOnMcDied();  // (also from kEvPlayerDied; the flag may last only a frame)
	if (mc && !dead && g_wasDead) ++g_respawns;
	g_wasDead = dead;
	HostLightFrame();
	HazardFrame();
	McMechanicsFrame(g_live);  // (bridge #32) faith plates, funnels, fizzlers on the Minecraft player
	if (g_killAt > 0 && now >= g_killAt) {
		g_killAt = -1, g_p2Dying = true;
		if (g_buffered) SetBuffer(pl, false);
		EntInput(pl, "SetHealth", "0");
		int *h = Field(pl, g_offHealth, "m_iHealth");
		if (h && *h > 0) engine.Cmd("kill");
		return;
	}
	if (g_killAt > 0 || g_p2Dying) return;  // (Portal 2's player is dying: its health is never touched until the reload)
	// the health buffer while the Minecraft player is the player
	// (bridge #55) held for the whole of Minecraft mode, not only while Minecraft is in the world and linked: in the gaps (level loads, a link
	// hiccup, the first frames of a hand-over) the body used to be back on Portal 2's 100 health and a laser / turret / goo killed it, with
	// Minecraft unhurt ("Chell dies"). Damage in such a gap is refunded and dropped (nothing to forward it to), never fatal.
	const bool want = (mc || McModeHeld()) && g_hurtOn;
	if (want != g_buffered) SetBuffer(pl, want);
	if (!g_buffered) return;
	int *h = Field(pl, g_offHealth, "m_iHealth");
	if (!h) return;
	// (bridge #32) Portal 2's own record of what hurt the body: CBasePlayer ORs the damage type of every hit into m_bitsDamageType; read and
	// cleared each frame, so a frame's loss carries the types of the hits that made it. Where the field is missing or says generic, the hazards
	// the body stands in decide (EvidenceTypes).
	int types = 0;
	if (int *dt = Field(pl, g_offDmgType, "m_bitsDamageType")) types = *dt, *dt = 0;
	if (*h < kBuffer) {
		const int lost = kBuffer - *h;
		// (bridge #68) a script's health write that leaves the body at 0 or below (SetPlayerHealth 0, `SetHealth -200`: sp_a2_intro's incinerator, bts3's fall relay,
		// speed_tb_catch's neurotoxin) is a kill. Real damage cannot do that: the buffer is 100000 and one hit forwards at most kMaxHit. Positive writes stay refunds.
		// (the input `SetHealth v` moves the buffered health only HALF way to v: measured, SetHealth 50 -> lost 49975, SetHealth 0 -> lost 50000, so a loss of
		// 50000 or more is a write of 0 or below; a direct write such as playerproxy's SetPlayerHealth lands the value itself, <= 0 here)
		const bool lethalWrite = *h <= 0 || lost >= kBuffer / 2;
		*h = kBuffer;  // refunded: Portal 2 never sees its body die
		if (!dead && mc && lost > 0) {
			// (bridge #34) only REAL damage goes to Minecraft. A health write by an input or a script (`ent_fire !player SetHealth 100`, a logic
			// entity, VScript) reaches the body through the same health field, untyped (m_bitsDamageType 0) and with the body in no hazard: that was
			// 49950 "void" damage = 9990 Minecraft damage, which killed the Minecraft player and Portal 2's body. Real damage is: a typed hit
			// (Portal 2 ORs the type of every hit into m_bitsDamageType: bullets, crush, burn, goo, fall...), or an untyped one while the body stands
			// in a damaging volume (a trigger_hurt, a laser beam; a kill volume's generic hit), or a test's own hit (pcb_combat_p2hurt).
			bool inHazard = false;
			if ((types & ~kDmgFlagBits) == 0) types |= EvidenceTypes(PlayerOrigin(), &inHazard);
			const bool real = (types & ~kDmgFlagBits) != 0 || inHazard || g_testHitFrames > 0 || lethalWrite;
			if (lethalWrite && (types & ~kDmgFlagBits) == 0 && !inHazard) {
				++g_scriptKills;
				console->Print("[bridge] combat: a script wrote the body's health to <= 0 (lost %d): a scripted kill, the Minecraft player dies\n", lost);
			}
			if (!real) {
				++g_hurtIgnored;
				if (g_hurtIgnored <= 10) console->Print("[bridge] combat: %d health taken off the body with no damage type and no hazard under it: a script/input health write, not damage (ignored, refunded)\n", lost);
			} else {
				if (g_pending <= 0) g_pendingAt = now;
				// one hit never forwards more than kMaxHit Portal 2 damage (= 200 Minecraft health, lethal many times over)
				const float hit = lethalWrite ? kMaxHit : std::fmin((float)lost, kMaxHit);
				const int cls = lethalWrite && (types & ~kDmgFlagBits) == 0 ? 8 : HostDamageClass(types, (float)lost);
				g_pending += hit, g_pendingBy[cls] += hit, g_lastHurtAt[cls] = now;
				if (g_log > 0) --g_log, console->Print("[bridge] combat: Portal 2 took %d health off the body: damage types 0x%x -> class %d (%s)\n", lost, types, cls, kClassNames[cls]);
			}
		}
	} else if (*h > kBuffer) {
		*h = kBuffer;
	}
	if (g_testHitFrames > 0) --g_testHitFrames;
	if (g_pending > 0 && now - g_pendingAt >= 0.1) {
		// kInHurt: a = host damage x 100; Minecraft takes host damage / 5 (SkyCombat.hurtPlayer), so Portal 2's 100 = 20. Flags bits 8..15 = the
		// damage class, which the mod turns into its damage source (bridge #32; class 0 = generic, what every older host sent)
		for (int c = 0; c < kHostClasses; ++c) {
			if (g_pendingBy[c] <= 0) continue;
			link::PushInput(kInHurt, (uint16_t)kHurtOther, (int32_t)std::lround(std::fmin(g_pendingBy[c], kMaxHit) * 100.0f), 0, (int32_t)(c << 8));
			++g_hurtEvents, ++g_hurtByClass[c];
			if (g_log > 0) --g_log, console->Print("[bridge] combat: Portal 2 hurt the Minecraft player: %.0f Portal 2 damage (%s) -> %.1f Minecraft\n", g_pendingBy[c], kClassNames[c], g_pendingBy[c] / 5.0f);
			g_pendingBy[c] = 0;
		}
		g_hurtSent += g_pending;
		g_pending = 0;
	}
}

// ---- Portal 2's light and hazards for Minecraft's mobs (the mod's HostWorld) ------------------------------------------
bool TerrainIsSolid(int bx, int by, int bz);  // terrain.cpp (Minecraft block coords)
float RenderLightToScale(float linear);       // render.cpp
static int g_hlSent = 0, g_hlCells = 0, g_hlForce = -1;
static int g_hlCx = 0, g_hlCy = 0, g_hlCz = 0;
static bool g_hlOn = true;
static long long CellKey(int x, int y, int z) { return ((long long)(x & 0x3FFFFF) << 42) | ((long long)(z & 0x3FFFFF) << 20) | (long long)(y & 0xFFFFF); }
// Every frame in Minecraft mode, sliced: the cells above blocks placed in the chambers within 40 blocks get Portal 2's light
// there as a Minecraft light level (kInHostLight), sent when it changes. Natural spawning then follows Portal 2's light
// under its roofs (MonsterDarknessMixin). Needs a client frame (ComputeLighting), as the band's lighting does.
static void HostLightFrame() {
	if (!g_hlOn || !McModeActive()) return;
	const int R = 40, H = 16;
	if (g_hlX == 0 && g_hlLayer == 0) {
		double x, y, z;
		SrcToMcD(PlayerOrigin(), x, y, z);
		g_hlCx = (int)std::floor(x), g_hlCy = (int)std::floor(y), g_hlCz = (int)std::floor(z);
	}
	int lights = 0;
	for (int rows = 0; rows < 3 && lights < 48; ++rows) {
		const int x = g_hlCx - R + g_hlX;
		for (int z = g_hlCz - R; z <= g_hlCz + R; ++z)
			for (int y = g_hlCy - H; y <= g_hlCy + H; ++y) {
				if (!TerrainIsSolid(x, y, z) || TerrainIsSolid(x, y + 1, z)) continue;
				const Vector p = McToSrc(x + 0.5, y + 1.1, z + 0.5);
				if (ClipHasRegion() && ClipRegionContains(p)) continue;
				// (outside Portal 2's map there is no light and no roof: Minecraft's own rules; b19_mobs4 spawned mobs on a
				// platform out in the void)
				if (engine.PointOutsideWorld(engine.traceServer->ThisPtr(), p + Vector(0, 0, 20))) continue;
				++g_hlCells;
				const long long key = CellKey(x, y + 1, z);
				if (g_hostLight.count(key) && (lights >= 48 || (g_hlLayer & 3))) continue;  // (known cells re-lit every 4th sweep)
				if (lights >= 48) continue;
				++lights;
				Vector l = engine.LightAt(p + Vector(0, 0, 4), Vector(0, 0, 1));
				const Vector w = engine.LightAt(p + Vector(0, 0, 4));
				const float lin = std::fmax(std::fmax(l.x, w.x), std::fmax(std::fmax(l.y, w.y), std::fmax(l.z, w.z)));
				int level = std::max(0, std::min(15, (int)std::lround(RenderLightToScale(lin) * 15.0f)));
				if (g_hlForce >= 0) level = g_hlForce;
				auto it = g_hostLight.find(key);
				if (it != g_hostLight.end() && it->second == level) continue;
				if (g_hostLight.size() > 400000) g_hostLight.clear();  // (bridge #53) bounded: the sweep sends the levels again
				g_hostLight[key] = level;
				link::PushInput(kInHostLight, (uint16_t)level, x, y + 1, z);
				++g_hlSent;
			}
		if (++g_hlX > 2 * R) g_hlX = 0, ++g_hlLayer;
	}
}

struct HazardClass {
	const char *cls;
	int kind;  // HostWorld: 1 lethal, 2 laser, 3 fizzler, 4 hurt
};
static const HazardClass kHazards[] = {{"trigger_hurt", 4}, {"trigger_portal_cleanser", 3}, {"env_portal_laser", 2}};
static double g_hzAt = 0;
static int g_hzSent = 0, g_hzLast = 0;
static bool g_hzOn = true;
static int g_offDisabled = -2, g_offLaserOn = -2;
struct SentHazard {
	int kind;
	Vector a, b;  // Source: box corners, or a laser's two ends
	std::string cls;
	int dmg = 0;  // (bridge #32) a trigger_hurt's damagetype bits
};
static std::vector<SentHazard> g_hzList, g_hzBuild;
static void SendBox(int slot, int kind, const Vector &a, const Vector &b, int dmg = 0) {
	g_hzBuild.push_back({kind, a, b, "", dmg});
	double ax, ay, az, bx, by, bz;
	SrcToMcD(a, ax, ay, az), SrcToMcD(b, bx, by, bz);
	if (kind != 2) {  // (a box: min and max corners in Minecraft's axes; a laser keeps its two ends)
		double mn[3] = {std::fmin(ax, bx), std::fmin(ay, by), std::fmin(az, bz)}, mx[3] = {std::fmax(ax, bx), std::fmax(ay, by), std::fmax(az, bz)};
		ax = mn[0], ay = mn[1], az = mn[2], bx = mx[0], by = mx[1], bz = mx[2];
	}
	const uint16_t code = (uint16_t)(kind | (slot << 8));
	link::PushInput(kInHazard, code, (int32_t)std::lround(ax * 1000), (int32_t)std::lround(ay * 1000), (int32_t)std::lround(az * 1000));
	link::PushInput(kInHazard, (uint16_t)(code | 0x10), (int32_t)std::lround(bx * 1000), (int32_t)std::lround(by * 1000), (int32_t)std::lround(bz * 1000));
}
// Twice a second: Portal 2's hazards near the player as boxes for Minecraft's mobs (the mod expires a box not re-sent for 3 s).
static void HazardFrame() {
	const double now = NowS();
	if (!g_hzOn || now - g_hzAt < 0.5) return;
	g_hzAt = now;
	EntLiveSet(g_live);  // (lead: entities freed since the last refill crashed EntKV below, bridge #20's report)
	const Vector feet = PlayerOrigin();
	int slot = 0;
	g_hzBuild.clear();
	link::PushInput(kInHazard, 0x20, 0, 0, 0);
	for (void *e : g_live) {
		if (slot >= 255) break;
		const std::string cls = EntKV(e, "classname");
		const HazardClass *hc = nullptr;
		for (auto &h : kHazards)
			if (cls == h.cls) hc = &h;
		if (!hc) continue;
		const Vector o = EntVec(e, "origin");
		if ((o - feet).Length() > 4000.0f) continue;
		if (hc->kind == 2) {
			{  // (bridge #59) the beam segment Portal 2 itself holds for this laser (mcmechanics.cpp): ends at a cube / a placed block / a portal like the real beam
				Vector sa, sb;
				bool lethal = false;
				const int st = McLaserBeam(e, sa, sb, lethal);
				if (st == 0) continue;
				if (st > 0) {
					SendBox(slot++, 2, sa, sb);
					continue;
				}
			}
			if (g_offLaserOn == -2) g_offLaserOn = EntFieldOffset(e, "m_bLaserOn");
			if (g_offLaserOn >= 0 && !*((const unsigned char *)e + g_offLaserOn)) continue;
			const Vector an = EntVec(e, "angles");
			const float p = an.x * (float)M_PI / 180.0f, y = an.y * (float)M_PI / 180.0f;
			const Vector fwd(std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), -std::sin(p));
			CGameTrace tr;
			const Vector from = o + fwd * 24.0f;
			engine.Trace(from, from + fwd * 8192.0f, Vector(0, 0, 0), MASK_SOLID_BRUSHONLY, tr, true);
			SendBox(slot++, 2, from, tr.endpos);
			continue;
		}
		if (g_offDisabled == -2) g_offDisabled = EntFieldOffset(e, "m_bDisabled");
		if (g_offDisabled >= 0 && *((const unsigned char *)e + g_offDisabled)) continue;
		if (g_offMins == -2) g_offMins = EntFieldOffset(e, "m_vecMins"), g_offMaxs = EntFieldOffset(e, "m_vecMaxs");
		if (g_offMins < 0 || g_offMaxs < 0) continue;
		const Vector mn = *(const Vector *)((const char *)e + g_offMins), mx = *(const Vector *)((const char *)e + g_offMaxs);
		if (!(mx.x > mn.x) || (mx - mn).Length() > 20000.0f) continue;
		if (ClipHasRegion() && ClipRegionContains((o + mn + o + mx) * 0.5f)) continue;
		int kind = hc->kind;
		if (kind == 4 && std::atof(EntKV(e, "damage").c_str()) >= 100.0) kind = 1;
		SendBox(slot++, kind, o + mn, o + mx, (hc->kind == 4) ? std::atoi(EntKV(e, "damagetype").c_str()) : 0);
	}
	g_hzSent += slot, g_hzLast = slot;
	g_hzList.swap(g_hzBuild);
}

// (bridge #32) the damage types of the hazards the body stands in right now (the 2 Hz list HazardFrame sent to Minecraft): a trigger_hurt
// its keyvalue's damagetype, a laser beam within its sweep DMG_BURN. Used when Portal 2's own record (m_bitsDamageType) says nothing.
static int EvidenceTypes(const Vector &feet, bool *inHazard) {
	int types = 0;
	const Vector lo = feet + Vector(-16, -16, 0), hi = feet + Vector(16, 16, 72), c = feet + Vector(0, 0, 36);
	for (const SentHazard &h : g_hzList) {
		if (h.kind == 4 || h.kind == 1) {
			const Vector mn(std::fmin(h.a.x, h.b.x), std::fmin(h.a.y, h.b.y), std::fmin(h.a.z, h.b.z)), mx(std::fmax(h.a.x, h.b.x), std::fmax(h.a.y, h.b.y), std::fmax(h.a.z, h.b.z));
			if (hi.x >= mn.x && lo.x <= mx.x && hi.y >= mn.y && lo.y <= mx.y && hi.z >= mn.z && lo.z <= mx.z) types |= h.dmg, inHazard ? (void)(*inHazard = true) : (void)0;
		} else if (h.kind == 2) {
			const Vector d = h.b - h.a;
			const float l2 = d.x * d.x + d.y * d.y + d.z * d.z;
			float t = l2 > 1e-3f ? ((c.x - h.a.x) * d.x + (c.y - h.a.y) * d.y + (c.z - h.a.z) * d.z) / l2 : 0.0f;
			t = std::fmax(0.0f, std::fmin(1.0f, t));
			if ((h.a + d * t - c).Length() <= 16.0f + std::sqrt(l2) / 256.0f + 20.0f) types |= (1 << 3), inHazard ? (void)(*inHazard = true) : (void)0;
		}
	}
	return types;
}
// (tests) the hazards last sent, and a Minecraft entity summoned into one: pcb_combat_hazard_summon <kind> <entity> [nbt]
CON_COMMAND(pcb_combat_hazards, "pcb_combat_hazards - the Portal 2 hazard boxes last sent to Minecraft\n") {
	static const char *names[5] = {"?", "lethal", "laser", "fizzler", "hurt"};
	for (auto &h : g_hzList) {
		double ax, ay, az, bx, by, bz;
		SrcToMcD(h.a, ax, ay, az), SrcToMcD(h.b, bx, by, bz);
		console->Print("[bridge] hazard %s: Source (%.0f %.0f %.0f) - (%.0f %.0f %.0f) = MC (%.1f %.1f %.1f) - (%.1f %.1f %.1f)\n", names[h.kind >= 1 && h.kind <= 4 ? h.kind : 0], h.a.x, h.a.y, h.a.z, h.b.x,
		               h.b.y, h.b.z, ax, ay, az, bx, by, bz);
	}
}
CON_COMMAND(pcb_combat_hazard_summon, "pcb_combat_hazard_summon <kind 1-4> <entity> [nbt] - (tests) summon a Minecraft entity into the nearest hazard of that kind\n") {
	if (args.ArgC() < 3) return;
	const int kind = std::atoi(args[1]);
	const Vector feet = PlayerOrigin();
	const SentHazard *best = nullptr;
	float bd = 1e9f;
	for (auto &h : g_hzList) {
		if (h.kind != kind) continue;
		const float d = ((h.a + h.b) * 0.5f - feet).Length();
		if (d < bd) bd = d, best = &h;
	}
	if (!best) return void(console->Print("[bridge] no hazard of kind %d\n", kind));
	Vector at = (best->a + best->b) * 0.5f;
	if (kind == 2) at.z -= 40.0f;  // a beam at chest height: the mob's feet a block below it
	else at.z = std::fmin(best->a.z, best->b.z) + 8.0f;  // a field or a pit: the bottom of the box
	double x, y, z;
	SrcToMcD(at, x, y, z);
	// (the console splits an NBT argument at its braces and colons: joined back without spaces)
	std::string nbt;
	for (int i = 3; i < args.ArgC(); ++i) nbt += args[i];
	char buf[300];
	std::snprintf(buf, sizeof buf, "pcb_mc_cmd summon %s %.2f %.2f %.2f %s", args[2], x, y, z, nbt.c_str());
	engine.Cmd(buf);
	console->Print("[bridge] hazard summon: %s\n", buf);
}

// chellproxy.cpp: kEvPlayerDied. The host owns death (SkyCraft; the user's decision for Portalcraft): the Minecraft player's
// death is Portal 2's player death, with Portal 2's fade and its checkpoint reload; Minecraft respawns at once at the body
// (mod: immediate respawn + RespawnAtBodyMixin), Minecraft mode survives the reload and the level-change flow puts the
// Minecraft player at the reload spot. No Minecraft death screen.
static double g_lastDeathAt = -10;
static int g_p2Deaths = 0;
void CombatOnMcDied() {
	if (!McModeActive()) return;
	const double now = NowS();
	if (now - g_lastDeathAt < 3.0) return;  // (one death: the event and a stray second one)
	g_lastDeathAt = now;
	++g_deaths, ++g_p2Deaths, g_deathLevelGen = g_levelGen, g_pending = 0;
	McPuppetTransit(PlayerOrigin(), Vector(0, 0, 0));  // Portal 2 holds the body (Minecraft respawns there)
	g_killAt = now;  // CombatFrame: un-buffer the health and kill Portal 2's player
	console->Print("[bridge] combat: the Minecraft player died: Portal 2's player dies (its fade, then the checkpoint reload)\n");
}

// mc.cpp: Minecraft's HUD image is not drawn while Portal 2's player dies (Minecraft's instant respawn shows its "Loading
// terrain" screen for a moment: b19_death1 dt_b_fade1); it is back after the checkpoint reload.
bool CombatHudHidden() { return g_p2Dying || NowS() - g_lastDeathAt < 1.5; }

bool CombatMcDead() { return McModeActive() && (McStateNow().flags & kMcDead) != 0; }  // script.cpp: waituntil mcdead

void CombatShutdown() {
	if (g_buffered && g_player && g_live.count(g_player)) SetBuffer(g_player, false);
}

// ---- commands -------------------------------------------------------------------------------------------------
CON_COMMAND(pcb_combat, "pcb_combat [0|1] [range_u] - Portal 2 turrets, cubes and props as Minecraft stand-ins in Minecraft mode\n") {
	if (args.ArgC() > 1) g_actorsOn = std::atoi(args[1]) != 0;
	if (args.ArgC() > 2) g_range = (float)std::atof(args[2]);
	console->Print("[bridge] combat: stand-ins %d (range %.0f u): %zu tracked, %d hits, %d pushes, %d explosion pushes; Portal 2 health %s, %d hurts sent (%.0f Portal 2 damage), "
	               "deaths %d, respawns %d\n",
	               g_actorsOn, g_range, g_actors.size(), g_hits, g_pushes, g_explosionPushes, g_buffered ? "held" : "Portal 2's", g_hurtEvents, g_hurtSent, g_deaths, g_respawns);
	console->Print("[bridge] combat: host light %d cells seen, %d levels sent (%zu known); hazards %d boxes last send (%d in total)\n", g_hlCells, g_hlSent, g_hostLight.size(), g_hzLast,
	               g_hzSent);
	for (auto &a : g_actors) {
		const Vector c = (a.lo + a.hi) * 0.5f;
		console->Print("[bridge]   %s form %08x at (%.0f %.0f %.0f) box %.0fx%.0fx%.0f\n", kClasses[a.cls].name, a.formId, c.x, c.y, c.z, a.hi.x - a.lo.x, a.hi.y - a.lo.y, a.hi.z - a.lo.z);
	}
}
CON_COMMAND(pcb_combat_hurt, "pcb_combat_hurt [0|1] - Portal 2 damage to the Minecraft player goes to Minecraft (default 1)\n") {
	if (args.ArgC() > 1) g_hurtOn = std::atoi(args[1]) != 0;
	console->Print("[bridge] combat: Portal 2 -> Minecraft damage %d\n", g_hurtOn);
}
CON_COMMAND(pcb_combat_push, "pcb_combat_push <k> - scale of Minecraft hits' physics push on Portal 2 props (default 1)\n") {
	if (args.ArgC() > 1) g_pushK = (float)std::atof(args[1]);
	console->Print("[bridge] combat: push x%.2f\n", g_pushK);
}
// (tests) Portal 2 damage on the body, as a turret or a laser would deal it (DMG_GENERIC through Portal 2's own health)
CON_COMMAND(pcb_combat_p2hurt, "pcb_combat_p2hurt <damage> [crush|bullet|burn|fall|blast|shock|drown|poison|acid|radiation|dissolve|<bits>] - (tests) take Portal 2 health off the player's body, as that damage type (bridge #32: the type picks the Minecraft damage source)\n") {
	void *pl = Player();
	int *h = pl ? Field(pl, g_offHealth, "m_iHealth") : nullptr;
	if (!h || args.ArgC() < 2) return;
	*h -= std::atoi(args[1]);
	g_testHitFrames = 3;  // (bridge #34) a test's hit is damage whatever its type (untyped losses are otherwise script writes)
	if (args.ArgC() > 2) {
		static const struct { const char *n; int bits; } kT[] = {{"crush", 1 << 0}, {"bullet", 1 << 1}, {"burn", 1 << 3}, {"fall", 1 << 5}, {"blast", 1 << 6}, {"shock", 1 << 8}, {"drown", 1 << 14},
		                                                       {"poison", 1 << 17}, {"radiation", 1 << 18}, {"acid", 1 << 20}, {"dissolve", 1 << 26}};
		int bits = (int)std::strtol(args[2], nullptr, 0);
		for (auto &t : kT)
			if (!std::strcmp(args[2], t.n)) bits = t.bits;
		if (int *dt = Field(pl, g_offDmgType, "m_bitsDamageType")) *dt |= bits;
		else console->Print("[bridge] combat: the player has no m_bitsDamageType field: the type is judged by the hazards the body stands in\n");
	}
	console->Print("[bridge] combat: body health now %d\n", *h);
}
// pcb_assert_combat <hits|pushes|hurts|deaths|respawns|tracked> <min> | reset
CON_COMMAND(pcb_assert_combat, "pcb_assert_combat reset | <hits|pushes|explosions|hurts|deaths|respawns|tracked> <min> - combat counters since the reset\n") {
	static int base[6] = {};
	int cur[6] = {g_hits, g_pushes, g_explosionPushes, g_hurtEvents, g_deaths, g_respawns};
	static const char *names[6] = {"hits", "pushes", "explosions", "hurts", "deaths", "respawns"};
	if (args.ArgC() < 2 || !std::strcmp(args[1], "reset")) return void(std::memcpy(base, cur, sizeof base));
	const int want = args.ArgC() > 2 ? std::atoi(args[2]) : 1;
	char buf[200];
	if (!std::strcmp(args[1], "tracked")) {
		std::snprintf(buf, sizeof buf, "%zu Portal 2 stand-ins tracked (want %d)", g_actors.size(), want);
		return TestCheck((int)g_actors.size() >= want, "Portal 2 stand-ins", buf);
	}
	for (int i = 0; i < 6; ++i)
		if (!std::strcmp(args[1], names[i])) {
			std::snprintf(buf, sizeof buf, "%s since the reset: %d (want >= %d); Portal 2 damage forwarded %.0f in total", names[i], cur[i] - base[i], want, g_hurtSent);
			return TestCheck(cur[i] - base[i] >= want, "combat", buf);
		}
}
// (tests) the nearest stand-in's Portal 2 entity origin and angles
CON_COMMAND(pcb_combat_nearest, "pcb_combat_nearest [classname] - (tests) the nearest tracked Portal 2 stand-in: origin, angles\n") {
	const Vector feet = PlayerOrigin();
	const P2Actor *best = nullptr;
	float bd = 1e9f;
	for (auto &a : g_actors) {
		if (args.ArgC() > 1 && std::strcmp(kClasses[a.cls].cls, args[1])) continue;
		const float d = (EntVec(a.ent, "origin") - feet).Length();
		if (d < bd) bd = d, best = &a;
	}
	if (!best) return void(console->Print("[bridge] combat: no stand-in\n"));
	const Vector o = EntVec(best->ent, "origin"), an = EntVec(best->ent, "angles");
	console->Print("[bridge] combat: nearest %s form %08x at (%.1f %.1f %.1f) angles (%.1f %.1f %.1f), %.0f u away\n", kClasses[best->cls].name, best->formId, o.x, o.y, o.z, an.x, an.y,
	               an.z, bd);
}

CON_COMMAND(pcb_combat_hostlight, "pcb_combat_hostlight [0|1] [force_level|-1] - Portal 2's light on placed chamber blocks for Minecraft's monster spawning\n") {
	if (args.ArgC() > 1) g_hlOn = std::atoi(args[1]) != 0;
	if (args.ArgC() > 2) g_hlForce = std::atoi(args[2]), g_hostLight.clear();
	console->Print("[bridge] combat: host light %d (forced %d): %d cells seen, %d levels sent, %zu known\n", g_hlOn, g_hlForce, g_hlCells, g_hlSent, g_hostLight.size());
}

// (tests) the Minecraft player alive again: respawned if dead, full health and food, survival (the mod's TestReset, which also
// runs by itself at the start of every locked_run.sh test: SKYCRAFT_TEST=1). Tests that kill the player end with it.
CON_COMMAND(pcb_mc_reset_player, "pcb_mc_reset_player - (tests) respawn the Minecraft player if dead; full health and food; survival\n") {
	char path[512];
	std::snprintf(path, sizeof path, "%s/mc_reset_request", CacheDir().c_str());
	if (FILE *f = std::fopen(path, "w")) std::fclose(f);
	console->Print("[bridge] Minecraft player reset requested\n");
}
CON_COMMAND(pcb_assert_mc_living, "pcb_assert_mc_living [0|1] - (tests) the Minecraft player is alive (1, default) or dead (0)\n") {
	const bool want = args.ArgC() < 2 || std::atoi(args[1]) != 0;
	const bool alive = !(McStateNow().flags & kMcDead);
	char buf[120];
	std::snprintf(buf, sizeof buf, "Minecraft player %s (want %s), Minecraft flags 0x%x", alive ? "alive" : "dead", want ? "alive" : "dead", McStateNow().flags);
	TestCheck(alive == want, "Minecraft player alive", buf);
}

// ---- test helpers -----------------------------------------------------------------------------------------------
static void *g_spawned[8] = {};
static Vector g_markPos[8], g_markAng[8];
// pcb_combat_spawn_front <slot 0-7> <classname> <dist_u> [side_u] [face: toward|away] [key value]...: an entity on the floor in
// front of the player (along the view's yaw), facing the player or away from it
CON_COMMAND(pcb_combat_spawn_front, "pcb_combat_spawn_front <slot> <classname> <dist_u> [side_u] [toward|away] [key value]... - (tests) spawn in front of the player\n") {
	if (args.ArgC() < 4) return;
	const int slot = std::atoi(args[1]) & 7;
	const float dist = (float)std::atof(args[3]), side = args.ArgC() > 4 ? (float)std::atof(args[4]) : 0.0f;
	const bool toward = !(args.ArgC() > 5 && !std::strcmp(args[5], "away"));
	const float yaw = engine.ViewAngles().y, r = yaw * (float)M_PI / 180.0f;
	const Vector fwd(std::cos(r), std::sin(r), 0), left(-std::sin(r), std::cos(r), 0);
	Vector at = PlayerOrigin() + fwd * dist + left * side + Vector(0, 0, 24);
	CGameTrace tr;
	engine.Trace(at, at - Vector(0, 0, 400), Vector(0, 0, 0), MASK_SOLID_BRUSHONLY, tr, true);
	at = tr.endpos + Vector(0, 0, std::strcmp(args[2], "npc_portal_turret_floor") ? 24.0f : 1.0f);
	char ang[48];
	std::snprintf(ang, sizeof ang, "0 %.1f 0", toward ? yaw + 180.0f : yaw);
	std::vector<std::pair<const char *, std::string>> kv = {{"origin", Vec3(at)}, {"angles", ang}};
	for (int i = 6; i + 1 < args.ArgC(); i += 2) kv.push_back({args[i], args[i + 1]});
	void *e = nullptr;
	switch (kv.size()) {  // (Spawn takes an initializer list)
	case 2: e = Spawn(args[2], {kv[0], kv[1]}); break;
	case 3: e = Spawn(args[2], {kv[0], kv[1], kv[2]}); break;
	default: e = Spawn(args[2], {kv[0], kv[1], kv[2], kv[3]}); break;
	}
	g_spawned[slot] = e;
	if (e) g_markPos[slot] = EntVec(e, "origin"), g_markAng[slot] = EntVec(e, "angles");
	console->Print("[bridge] combat: spawned %s in slot %d at (%.0f %.0f %.0f) facing %s the player -> %p\n", args[2], slot, at.x, at.y, at.z, toward ? "toward" : "away from", e);
}
// pcb_combat_face_open <dist_u> [step_deg=15]: turn the view to the direction (all round, from the current yaw) with the
// longest run of level (within 24 u of the feet), clear floor, up to dist: room for the test props and mobs
CON_COMMAND(pcb_combat_face_open, "pcb_combat_face_open <dist_u> [step_deg=15] - (tests) face the direction with the most open level floor\n") {
	const float dist = args.ArgC() > 1 ? (float)std::atof(args[1]) : 300.0f, step = args.ArgC() > 2 ? (float)std::atof(args[2]) : 15.0f;
	const Vector feet = PlayerOrigin();
	const float yaw0 = engine.ViewAngles().y;
	float bestYaw = yaw0, bestRun = -1;
	for (float a = 0; a < 360.0f; a += step) {
		const float yaw = yaw0 + a, r = yaw * (float)M_PI / 180.0f;
		const Vector fwd(std::cos(r), std::sin(r), 0);
		float run = 0;
		for (float d = 40.0f; d <= dist; d += 20.0f) {
			const Vector at = feet + fwd * d;
			CGameTrace down, hull;
			engine.Trace(at + Vector(0, 0, 30), at - Vector(0, 0, 60), Vector(0, 0, 0), MASK_SOLID_BRUSHONLY, down, true);
			engine.Trace(at + Vector(0, 0, 40), at + Vector(0, 0, 40), Vector(16, 16, 30), MASK_SOLID_BRUSHONLY, hull, true);
			if (down.startsolid || down.fraction >= 1.0f || std::fabs(down.endpos.z - feet.z) >= 24.0f || hull.startsolid) break;
			run = d;
		}
		if (run > bestRun + 1.0f) bestRun = run, bestYaw = yaw;
	}
	char buf[64];
	std::snprintf(buf, sizeof buf, "setang 0 %.1f 0", std::remainder(bestYaw, 360.0f));
	engine.Cmd(buf);
	console->Print("[bridge] combat: the most open level floor: %.0f u along yaw %.1f (%s)\n", bestRun, bestYaw, buf);
}
// pcb_turn <dyaw> <pitch>: (tests) turn the view by dyaw degrees and set its pitch (Portal 2's view drives Minecraft's)
CON_COMMAND(pcb_turn, "pcb_turn <dyaw> <pitch> - (tests) turn the view (yaw relative, pitch absolute)\n") {
	const QAngle a = engine.ViewAngles();
	char buf[64];
	std::snprintf(buf, sizeof buf, "setang %.1f %.1f 0", args.ArgC() > 2 ? (float)std::atof(args[2]) : a.x, a.y + (args.ArgC() > 1 ? (float)std::atof(args[1]) : 0.0f));
	engine.Cmd(buf);
}
static void *Slot(int slot) {
	void *e = g_spawned[slot & 7];
	if (e && !g_live.count(e)) {
		EntLiveSet(g_live);
		if (!g_live.count(e)) e = g_spawned[slot & 7] = nullptr;
	}
	return e;
}
// pcb_combat_aim <slot> [up_u]: the view turned to the spawned entity's centre (the Minecraft player attacks with lmb)
CON_COMMAND(pcb_combat_aim, "pcb_combat_aim <slot> [up_u=20] - (tests) aim at a pcb_combat_spawn_front entity\n") {
	void *e = Slot(args.ArgC() > 1 ? std::atoi(args[1]) : 0);
	if (!e) return void(console->Print("[bridge] combat aim: nothing in that slot\n"));
	const Vector d = EntVec(e, "origin") + Vector(0, 0, args.ArgC() > 2 ? (float)std::atof(args[2]) : 20.0f) - (PlayerOrigin() + Vector(0, 0, 64));
	const float l = d.Length();
	char buf[96];
	std::snprintf(buf, sizeof buf, "setang %.2f %.2f 0", -std::asin(d.z / l) * 180.0f / (float)M_PI, std::atan2(d.y, d.x) * 180.0f / (float)M_PI);
	engine.Cmd(buf);
	console->Print("[bridge] combat aim: %.0f u away, %s\n", l, buf);
}
CON_COMMAND(pcb_combat_mark, "pcb_combat_mark <slot> - (tests) remember the slot entity's origin and angles\n") {
	const int slot = args.ArgC() > 1 ? std::atoi(args[1]) & 7 : 0;
	void *e = Slot(slot);
	if (e) g_markPos[slot] = EntVec(e, "origin"), g_markAng[slot] = EntVec(e, "angles");
}
// pcb_assert_moved <slot> <min_u> [min_tilt_deg]: the entity moved at least min_u (and tilted at least min_tilt from upright)
CON_COMMAND(pcb_assert_moved, "pcb_assert_moved <slot> <min_u> [min_tilt_deg] - (tests) a spawned entity was pushed (and tipped)\n") {
	const int slot = args.ArgC() > 1 ? std::atoi(args[1]) & 7 : 0;
	const float minU = args.ArgC() > 2 ? (float)std::atof(args[2]) : 8.0f, minTilt = args.ArgC() > 3 ? (float)std::atof(args[3]) : -1.0f;
	void *e = Slot(slot);
	char buf[220];
	if (!e) {
		std::snprintf(buf, sizeof buf, "slot %d: the entity is gone (dissolved or removed)", slot);
		return TestCheck(minTilt >= 0, "Portal 2 prop pushed", buf);
	}
	const Vector o = EntVec(e, "origin"), a = EntVec(e, "angles");
	const float moved = (o - g_markPos[slot]).Length();
	const float tilt = std::fmax(std::fabs(std::remainder(a.x, 360.0f)), std::fabs(std::remainder(a.z, 360.0f)));
	std::snprintf(buf, sizeof buf, "slot %d %s: moved %.1f u (want %.0f), angles (%.0f %.0f %.0f), tilt %.0f deg (want %.0f)", slot, EntKV(e, "classname").c_str(), moved, minU, a.x, a.y, a.z,
	              tilt, minTilt);
	TestCheck(moved >= minU && tilt >= minTilt, "Portal 2 prop pushed", buf);
}

}  // namespace bridge
