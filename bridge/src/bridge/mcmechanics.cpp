// Portalcraft bridge (bridge #32): Portal 2's chamber mechanics acting on the Minecraft player.
//
// While the Minecraft player is the player, Portal 2 moves a body (the puppet) to where Minecraft's player is, every tick, and
// overwrites the body's velocity with Minecraft's. Anything Portal 2 does to the BODY's motion (a faith plate's launch, a funnel's pull)
// is therefore lost, and Minecraft's player walks through it. The damage side (turret, laser, goo, ...) works because Portal 2 hurts the
// body and combat.cpp forwards the health lost. For the motion side this file does the same kind of forwarding, from the entities of the
// map, one mechanism per CLASS, so every instance in every map is covered:
//
//   trigger_catapult (aerial faith plate)      the Minecraft player's hull enters its volume -> Portal 2's launch velocity (research/specs/
//                                              test-elements.md 1: playerSpeed, launchDirection or launchTarget with the time-of-flight or
//                                              the fixed-speed solver) is sent as a kInMech launch; the mod gives it Portal 2's fling physics
//   prop_tractor_beam (excursion funnel)       inside the beam (the emitter's ray to the first wall, radius ~64) the velocity is the beam's:
//                                              linearForce along the beam, pulled to its axis, no gravity (kInMech beam, refreshed 20 Hz)
//   trigger_portal_cleanser (fizzler)          the hull enters its volume -> the player's portals (linkage group 0) fizzle and what the
//                                              player holds dissolves
//   env_portal_laser (thermal beam)            (bridge #37) the beam's sweep around the Minecraft player: 150 * frametime burn damage per
//                                              0.05 s update (as Portal 2's own sweep, which never reached the body) and the horizontal push
//   bounce / speed gel                         (bridge #37) Portal 2's paint power state on the body (PaintPowerUser::GetPaintPower, read after
//                                              every server move) says what the Minecraft player stands on: bounce gel gives Portal 2's bounce
//                                              velocity (trampoline mode, research/specs/test-elements.md 2.2), speed gel its raised ground speed
//
// Use / right click on Portal 2 buttons is in mc.cpp (a right click aimed at a Portal 2 entity is a +use); damage is in combat.cpp.
// The kInMech input type (16) and its codes are this file's; the mod side is dev.skycraft.portalcraft.HostMechanics.
#include "bridge.hpp"
#include "coords.hpp"
#include "link.hpp"

#include "Modules/Tier1.hpp"
#include "Offsets.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bridge {

using namespace skycraft::proto;

bool McModeActive();                                     // mc.cpp
Vector McFeetSrc();                                      // mc.cpp
Vector McVelocitySrc();                                  // mc.cpp
bool PlayerHolding();                                    // propcollide.cpp
bool McOnGround();                                       // mc.cpp
void CombatInjectHurt(int cls, float p2Damage);          // combat.cpp (bridge #37)
bool CombatRecentHurtClass(int cls, double within);      // combat.cpp (bridge #37)
int EntFieldOffset(void *ent, const char *field);        // ents.cpp
std::string EntKVx(void *ent, const char *key);          // ents.cpp (bridge #37): EntKV, then the entity's datamap when that is empty
bool EntDatamapRead(void *ent, const char *name, std::string &out);  // ents.cpp (bridge #37)
void TestCheck(bool ok, const char *what, const char *detail);  // testassert.cpp

namespace {

constexpr uint16_t kInMech = 16;  // input type (older mods ignore unknown types)
enum : uint16_t { kMechLaunch = 1, kMechBeam = 2, kMechBeamEnd = 3, kMechSpeed = 4, kMechPush = 5 };  // kMechPush (bridge #67): a trigger_push's base velocity, the displacement per second while inside  // kMechSpeed: a = the ground speed multiplier x 10000 (speed gel)

double NowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

bool g_on = true;           // pcb_mech
bool g_verbose = false;     // pcb_mech_log
int g_logLeft = 60;
int g_launches = 0, g_beamSends = 0, g_beamEntries = 0, g_fizzles = 0, g_portalsFizzled = 0, g_dropped = 0;
int g_laserHits = 0, g_bounces = 0, g_speedSends = 0;
Vector g_lastLaunch{0, 0, 0};
std::string g_lastLaunchBy;

std::unordered_map<void *, std::string> g_classOf;
std::vector<void *> g_catapults, g_cleansers, g_beams, g_beamTrigs, g_lasers, g_pushes;
double g_listAt = -1e9;

// ---- entity reads -------------------------------------------------------------------------------------------------
bool Flag(void *e, const char *field, int &off) {
	if (off == -2) off = EntFieldOffset(e, field);
	return off >= 0 && *((const unsigned char *)e + off) != 0;
}
bool Disabled(void *e) {
	static int off = -2;
	return Flag(e, "m_bDisabled", off);
}
bool BeamEnabled(void *e) {
	static int off = -2;
	if (off == -2) off = EntFieldOffset(e, "m_bEnabled");
	return off < 0 ? true : *((const unsigned char *)e + off) != 0;  // (no field: assume on)
}
bool WorldBox(void *e, Vector &mn, Vector &mx) {
	static int offMin = -2, offMax = -2, offAbs = -2;
	if (offMin == -2) offMin = EntFieldOffset(e, "m_vecMins"), offMax = EntFieldOffset(e, "m_vecMaxs"), offAbs = EntFieldOffset(e, "m_vecAbsOrigin");
	if (offMin < 0 || offMax < 0) return false;
	const Vector lmn = *(const Vector *)((const char *)e + offMin), lmx = *(const Vector *)((const char *)e + offMax);
	if (!std::isfinite(lmn.x + lmn.y + lmn.z + lmx.x + lmx.y + lmx.z) || !(lmx.x >= lmn.x) || (lmx - lmn).Length() > 20000.0f) return false;
	Vector o = EntVec(e, "origin");
	if (offAbs >= 0) o = *(const Vector *)((const char *)e + offAbs);
	mn = o + lmn, mx = o + lmx;
	return true;
}
// (bridge #67) a STRING key: IServerTools::GetKeyValue returns the four bytes of the string_t pointer for a non-empty string field ("launchTarget" came
// back as "\xa0I/]", so the plate looked unsolvable and a filter looked like noise), so the datamap is read first and a raw answer is taken only when printable
std::string StrKey(void *e, const char *key, const char *field) {
	std::string s;
	if (EntDatamapRead(e, key, s) || (field && EntDatamapRead(e, field, s))) return s;
	s = EntKV(e, key);
	for (unsigned char c : s)
		if (c < 32 || c > 126) return "";
	return s;
}
float KVf(void *e, const char *key, float def) {
	const std::string s = EntKV(e, key);
	return s.empty() ? def : (float)std::atof(s.c_str());
}
Vector Forward(const Vector &a) {
	const float p = a.x * (float)M_PI / 180.0f, y = a.y * (float)M_PI / 180.0f;
	return Vector(std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), -std::sin(p));
}

// ---- sending --------------------------------------------------------------------------------------------------------
void SendVel(uint16_t code, const Vector &v) {
	// Source u/s -> MC blocks/tick x 10000: MC (x, y, z) = Source (x, z, -y) / 40, 20 ticks a second
	const double k = 10000.0 / (kUnitsPerBlock * 20.0);
	link::PushInput(kInMech, code, (int32_t)std::lround(v.x * k), (int32_t)std::lround(v.z * k), (int32_t)std::lround(-v.y * k));
}

// the Minecraft player's body box (Portal 2's hull at its feet)
bool Overlaps(const Vector &mn, const Vector &mx, const Vector &feet) {
	const Vector lo = feet + Vector(-16, -16, 0), hi = feet + Vector(16, 16, 72);
	return hi.x >= mn.x && lo.x <= mx.x && hi.y >= mn.y && lo.y <= mx.y && hi.z >= mn.z && lo.z <= mx.z;
}

// ---- faith plates --------------------------------------------------------------------------------------------------
// Portal 2's launch velocity for a player at `feet` (research/specs/test-elements.md 1.3, 1.4). False: no launch.
bool CatapultVelocity(void *e, const Vector &feet, Vector &v, std::string &why) {
	const float speed = KVf(e, "playerSpeed", 0.0f);
	if (KVf(e, "onlyVelocityCheck", 0.0f) != 0.0f) return why = "onlyVelocityCheck", false;
	const float g = 600.0f;  // sv_gravity
	// (bridge #37) tbtest full4: the testbed's plate (launchTarget tb_faith_target) was launched in DIRECTION mode, (350 0 0): the key came back empty or
	// the target was not found, and a direction of 0 0 0 launched the player flat along +x (rose 5 u, 332 u short). The key is read through the datamap
	// too (EntKVx; the field m_strLaunchTarget), and a plate that has a target key but no entity for it is logged, not launched flat.
	std::string target = StrKey(e, "launchTarget", "m_strLaunchTarget");
	std::vector<void *> tg;
	if (!target.empty()) tg = EntFind(nullptr, target.c_str());
	if (!target.empty() && tg.empty()) return why = "launchTarget '" + target + "' names no entity", false;
	if (target.empty() && EntVec(e, "launchDirection").Length() < 1e-3f)
		return why = "no launchTarget (empty key and datamap) and launchDirection 0 0 0: the plate's aim is unreadable; run pcb_dump_fields trigger_catapult aunch", false;
	if (!tg.empty()) {
		if (speed <= 1.0f) return why = "playerSpeed 0", false;
		// the feet land 32 u under the target entity
		const Vector p = EntVec(tg[0], "origin") - Vector(0, 0, 32);
		Vector d = p - feet;
		if (KVf(e, "useExactVelocity", 0.0f) == 0.0f) {
			const float t = std::fmax(d.Length() / speed, 0.05f);  // time of flight
			v = d * (1.0f / t) + Vector(0, 0, 0.5f * g * t);
			return true;
		}
		// fixed speed: the ballistic solver, high or low arc (0: whichever is nearer the current velocity)
		const float dx = std::hypot(d.x, d.y), dz = d.z, v2 = speed * speed;
		const float disc = v2 * v2 - g * (g * dx * dx + 2.0f * dz * v2);
		if (disc <= 0.0f || dx < 1e-3f) {
			const float t = std::fmax(d.Length() / speed, 0.05f);
			v = d * (1.0f / t) + Vector(0, 0, 0.5f * g * t);
			return true;
		}
		const float hi = std::atan((v2 + std::sqrt(disc)) / (g * dx)), lo = std::atan((v2 - std::sqrt(disc)) / (g * dx));
		const Vector h(d.x / dx, d.y / dx, 0);
		auto vel = [&](float a) { return h * (speed * std::cos(a)) + Vector(0, 0, speed * std::sin(a)); };
		const int choice = (int)KVf(e, "exactVelocityChoiceType", 0.0f);
		if (choice == 1) v = vel(hi);
		else if (choice == 2) v = vel(lo);
		else {
			const Vector cur = McVelocitySrc(), a = vel(hi), b = vel(lo);
			v = Vector::DotProduct(a, cur) >= Vector::DotProduct(b, cur) ? a : b;
		}
		return true;
	}
	// direction mode
	if (speed <= 1.0f) return why = "playerSpeed 0 and no launch target", false;
	const Vector dir = Forward(EntVec(e, "launchDirection"));
	v = dir * speed;
	if (std::fabs(v.x) <= 0.001f && std::fabs(v.y) <= 0.001f) v.z = 1.5f * speed;  // straight-up plates launch 1.5x
	return true;
	// (bridge #67) useThresholdCheck and filtername are not computed here: a plate with either only launches when Portal 2's own launch of the body is
	// seen (CatapultsFrame); this velocity is then just the check that a velocity jump belongs to the plate.
}

struct Touch {
	bool inside = false;
	bool strict = false;       // (bridge #77) the body's hull overlaps the plate's volume with no margin: what Portal 2's own touch test uses
	double strictSince = -1e9; // (bridge #77) since when, for the "overlaps but Portal 2 did not touch it" diagnostic
	bool diagLogged = false;
	double lastFire = -1e9;
	double waitNative = -1e9;  // (bridge #37) the computed launch failed: Portal 2's own launch of the body, seen as a velocity jump, is used instead
	bool nativeOnly = false;   // (bridge #67) this wait is the plate's only way to launch (threshold or filter): the computed launch is checked against it
	bool haveExpect = false;   // (bridge #67) the computed launch for this entry, to tell Portal 2's own launch from some other jump of the body
	Vector expect{0, 0, 0};
};
// (bridge #67) which of Portal 2's rules a trigger_catapult applies to a player; read once per plate (the keys do not change)
struct PlateRules {
	bool known = false, clients = true, filtered = false, threshold = false;
	std::string filter;
};
std::unordered_map<void *, PlateRules> g_plateRules;
// read at every entry (a map can change a plate's keys with AddOutput; entries are rare)
const PlateRules &RulesOf(void *e) {
	PlateRules &r = g_plateRules[e];
	r.known = true;
	// spawnflags bit 1 "clients": the plate acts on players. A plate without it (physics objects only, flags 8) never touches a player in Portal 2
	// (bridge #67: intro5's slow_box_catapult at room_1_portal's exit launched the Minecraft player because this bit was never read).
	const std::string sf = EntKVx(e, "spawnflags");
	r.clients = sf.empty() ? true : (std::atoi(sf.c_str()) & 1) != 0;
	r.filter = StrKey(e, "filtername", "m_iFilterName");
	r.filtered = !r.filter.empty();
	r.threshold = KVf(e, "useThresholdCheck", 0.0f) != 0.0f;
	if (g_logLeft > 0)
		--g_logLeft, console->Print("[bridge] mech: faith plate %s: spawnflags %s (%s), filter '%s', threshold check %d -> %s\n", EntKV(e, "targetname").c_str(), sf.empty() ? "unreadable" : sf.c_str(),
		                            r.clients ? "acts on players" : "NOT for players", r.filter.c_str(), r.threshold ? 1 : 0,
		                            !r.clients ? "ignored: never launches the Minecraft player" : (r.filtered || r.threshold) ? "Portal 2 decides (its own launch of the body)" : "computed launch");
	return r;
}
Vector g_nativeVel{0, 0, 0};  // (bridge #37) the body's velocity at the start of a server move when something other than the puppet changed it
double g_nativeAt = -1e9;
std::unordered_map<void *, Touch> g_touch;

// (bridge #77) Portal 2 decides every plate's launch (default). The user's catapult_intro: plate catapult2a launches the player toward the landing platform,
// and its OnCatapulted output (-> catapult_target_relay) closes that platform 1 s later while the player is in the air. In Minecraft mode the platform never
// closed (the player fell into the goo: bridge.log "combat: ... goo" right after both "faith plate catapult2a launched" lines). The plate's outputs run from
// Portal 2's own StartTouch for the body. The body is moved to Minecraft's pose every tick and Portal 2 touch-links it there (the engine's own touch update
// runs after every player move, at the pose the puppet wrote), so the body touches a plate exactly where Chell would. But the launch decision used to be the
// mod's own hull test with a 2 u margin (the computed launch below): it launched the Minecraft player up to 2 u BEFORE the body's hull reached the plate's
// volume (both logged launches sit exactly on the margin: y 546 = 528 + 16 + 2), and the next pose (a fling rises ~44 u per Minecraft tick, the plate is
// 8 u thick) was already out of the volume, so Portal 2 never touched the plate: no OnCatapulted, no output. Now the launch is the velocity Portal 2 gave
// the body at its own touch (what the filter / threshold plates already did): its outputs, filters, entry rules and aim are all Chell's.
// pcb_mech_plates 0 brings the computed launch back (a diagnostic switch).
bool g_plateNative = true;
int g_plateDiag = 12;

void CatapultsFrame(const std::unordered_set<void *> &live, const Vector &feet, double now) {
	for (void *e : g_catapults) {
		if (!live.count(e)) continue;
		Touch &t = g_touch[e];
		Vector mn, mx;
		// (bridge #67) 2 u of margin: Minecraft's feet rest 1 u above a floor-flush plate (catapult_intro's plates are 8 u slabs whose top is the floor) and Portal 2 touches it
		const bool boxOk = !Disabled(e) && WorldBox(e, mn, mx);
		const bool inside = boxOk && Overlaps(mn - Vector(2, 2, 2), mx + Vector(2, 2, 2), feet);
		const bool strict = boxOk && Overlaps(mn, mx, feet);  // (bridge #77) Portal 2's own rule: the hull in the volume, no margin
		const bool enter = inside && !t.inside;
		const bool enterStrict = strict && !t.strict;
		t.inside = inside;
		t.strict = strict;
		if (enterStrict) t.strictSince = now, t.diagLogged = false;
		if (!strict) t.strictSince = -1e9;
		if (t.waitNative > 0) {  // (bridge #37) fallback: Portal 2 touched the body itself and launched it; its velocity is the launch
			// (bridge #67) the jump must look like THIS plate's launch when we know what it should be (a portal transit or a gel bounce in the window is not)
			const bool looksRight = !t.haveExpect || (g_nativeVel.Length() > 1.0f && Vector::DotProduct(g_nativeVel, t.expect) > 0.6f * g_nativeVel.Length() * t.expect.Length());
			if (g_nativeAt >= t.waitNative - 0.15 && looksRight) {
				const Vector nv = g_nativeVel;
				t.waitNative = -1e9, t.lastFire = now, g_nativeAt = -1e9;
				SendVel(kMechLaunch, nv);
				++g_launches, g_lastLaunch = nv, g_lastLaunchBy = EntKV(e, "targetname");
				if (g_logLeft > 0)
					--g_logLeft, console->Print("[bridge] mech: faith plate %s: Portal 2's own launch of the body, (%.0f %.0f %.0f) u/s, handed to the Minecraft player\n", g_lastLaunchBy.c_str(), nv.x, nv.y, nv.z);
			} else if (now - t.waitNative > (t.nativeOnly ? 0.6 : 0.4)) {
				if (t.nativeOnly && g_logLeft > 0)
					--g_logLeft, console->Print("[bridge] mech: faith plate %s: the Minecraft player entered it and Portal 2 did not launch the body (its entry rules, threshold or filter): no launch\n", EntKV(e, "targetname").c_str());
				t.waitNative = -1e9;
			}
		}
		if (g_plateNative) {
			// (bridge #77) wait for Portal 2's own launch of the body. A wait starts when the hull gets into the volume (Portal 2's rule) or into the 2 u margin, or when a
			// launch of the body shows up while the hull is within the margin and nobody is waiting (the plate was enabled under the player, the entry fell between frames)
			if (t.waitNative <= 0 && now - t.lastFire >= 0.3 && (enter || enterStrict || (inside && g_nativeAt > t.lastFire + 0.05 && now - g_nativeAt < 0.3))) {
				const PlateRules &rules = RulesOf(e);
				if (rules.clients) {
					std::string why;
					const double at = (inside && g_nativeAt > t.lastFire + 0.05 && now - g_nativeAt < 0.3) ? g_nativeAt : now;
					t.waitNative = at, t.nativeOnly = true;
					t.haveExpect = CatapultVelocity(e, feet, t.expect, why);
					if (g_logLeft > 0)
						--g_logLeft, console->Print("[bridge] mech: %s: the Minecraft player is at the plate%s: waiting for Portal 2's own launch of the body (clients flag, filter '%s', threshold %d)\n", EntKV(e, "targetname").c_str(),
						                            strict ? "" : " (within the 2 u margin, the hull is not in the volume yet)", rules.filter.c_str(), rules.threshold ? 1 : 0);
				}
			}
			// diagnostic: the hull has been in the volume for 0.6 s and Portal 2 did not touch the plate (a launch would have reset lastFire)
			if (strict && !t.diagLogged && t.strictSince > 0 && now - t.strictSince > 0.6 && t.lastFire < t.strictSince && g_plateDiag > 0) {
				t.diagLogged = true;
				--g_plateDiag;
				const PlateRules &rules = RulesOf(e);
				console->Print("[bridge] mech: BUG? the Minecraft player's hull has been in plate %s for %.1f s and Portal 2 did not launch the body (clients flag %d, filter '%s', threshold %d, body velocity (%.0f %.0f %.0f)): "
				               "either Portal 2's rules say no (then Chell would not be launched either) or its touch did not reach the body (pcb_mech_plates 0 = the old computed launch)\n",
				               EntKV(e, "targetname").c_str(), now - t.strictSince, rules.clients ? 1 : 0, rules.filter.c_str(), rules.threshold ? 1 : 0, PlayerVelocity().x, PlayerVelocity().y, PlayerVelocity().z);
			}
			continue;
		}
		if (!enter || now - t.lastFire < 0.1) continue;
		// (bridge #67) Portal 2's rules for a player: the clients flag, the filter and the entry-speed window. The body is in the same volume, so Portal 2
		// runs its own StartTouch for it with all three; where one of them can say no, its answer (a velocity jump of the body) is the only launch.
		const PlateRules &rules = RulesOf(e);
		if (!rules.clients) continue;
		Vector v;
		std::string why;
		if (rules.filtered || rules.threshold) {
			t.waitNative = now, t.nativeOnly = true;
			if (g_logLeft > 0) { const Vector mv = McVelocitySrc(); console->Print("[bridge] mech: entry velocity of the Minecraft player (%.0f %.0f %.0f), speed %.0f\n", mv.x, mv.y, mv.z, mv.Length()); }
			t.haveExpect = CatapultVelocity(e, feet, t.expect, why);
			if (g_logLeft > 0) --g_logLeft, console->Print("[bridge] mech: %s: the Minecraft player entered it (%s): waiting for Portal 2's own launch of the body\n", EntKV(e, "targetname").c_str(), rules.threshold ? "entry-speed check" : "filter");
			continue;
		}
		if (!CatapultVelocity(e, feet, v, why)) {
			t.waitNative = now, t.nativeOnly = false, t.haveExpect = false;
			if (g_logLeft > 0) --g_logLeft, console->Print("[bridge] mech: %s: the Minecraft player entered it, no computed launch (%s); waiting for Portal 2's own\n", EntKV(e, "targetname").c_str(), why.c_str());
			continue;
		}
		const float cap = 3500.0f;  // sv_maxvelocity, per axis
		v.x = std::fmax(-cap, std::fmin(cap, v.x)), v.y = std::fmax(-cap, std::fmin(cap, v.y)), v.z = std::fmax(-cap, std::fmin(cap, v.z));
		t.lastFire = now;
		SendVel(kMechLaunch, v);
		++g_launches, g_lastLaunch = v, g_lastLaunchBy = EntKV(e, "targetname");
		if (g_logLeft > 0)
			--g_logLeft, console->Print("[bridge] mech: faith plate %s launched the Minecraft player from (%.0f %.0f %.0f): velocity (%.0f %.0f %.0f) u/s, speed %.0f\n", g_lastLaunchBy.c_str(), feet.x,
			                            feet.y, feet.z, v.x, v.y, v.z, v.Length());
	}
}

// ---- trigger_push (bridge #67, W11) ---------------------------------------------------------------------------------------------
// A trigger_push with the clients flag gives a player standing in it a BASE velocity: speed along pushdir, added to the player's motion for as long as it
// stays inside (it is not accumulated into the velocity; leaving the volume ends it). The body Portal 2 moves is overwritten by Minecraft's motion, so the
// push was lost (intro3's podium collapse, the a3 lifts' pushers, finale conveyors: 51 pushes in 22 maps). The same rule is applied from here: the
// Minecraft player's hull in the volume of an enabled push that has the clients flag and no filter (a filtered one is Portal 2's decision: it is applied
// only while the body itself shows the push, see McMechanicsAfterMove) -> the push is sent 20 times a second; the mod displaces the player by it every tick.
struct PushRules {
	bool known = false, clients = true, filtered = false;
	float speed = 0;
	Vector dir{1, 0, 0};
};
std::unordered_map<void *, PushRules> g_pushRules;
const PushRules &PushRulesOf(void *e) {
	PushRules &r = g_pushRules[e];
	if (r.known) return r;
	r.known = true;
	const std::string sf = EntKVx(e, "spawnflags");
	if (!sf.empty()) r.clients = (std::atoi(sf.c_str()) & 1) != 0;
	r.filtered = !StrKey(e, "filtername", "m_iFilterName").empty();
	std::string sp = EntKVx(e, "speed");
	r.speed = sp.empty() ? 40.0f : (float)std::atof(sp.c_str());
	std::string pd;
	if (!EntDatamapRead(e, "pushdir", pd)) pd = EntKV(e, "pushdir");  // (GetKeyValue gave junk for this vector key: a direction of (1, 0.02, 0) for a push the body shows as (-300 0 0))
	Vector a(0, 0, 0);
	if (!pd.empty()) std::sscanf(pd.c_str(), "%f %f %f", &a.x, &a.y, &a.z);
	r.dir = Forward(a);
	console->Print("[bridge] mech: push %s: spawnflags %s (%s), filter %d, speed %.0f along (%.2f %.2f %.2f)\n", EntKV(e, "targetname").c_str(), sf.empty() ? "unreadable" : sf.c_str(),
	               r.clients ? "acts on players" : "NOT for players", r.filtered ? 1 : 0, r.speed, r.dir.x, r.dir.y, r.dir.z);
	return r;
}
bool g_inPush = false;
double g_pushSentAt2 = -1e9;
int g_pushSends = 0, g_pushEntries = 0;
Vector g_baseVel{0, 0, 0};  // the body's own base velocity after its last server move (Portal 2's trigger_push on the body), and when it was non-zero
double g_baseVelAt = -1e9;
int g_baseOff = -2;  // the body's m_vecBaseVelocity offset (-1: not found)
void PushesFrame(const std::unordered_set<void *> &live, const Vector &feet, double now) {
	// The volume decides WHETHER the Minecraft player is in a push (an enabled, clients-flagged trigger_push whose box its hull is in); the push itself is what
	// Portal 2 gave the body there (its base velocity: the trigger's own pushdir, speed, filter and rotation, exactly Chell's), read after every server move. Without
	// the field, the push is computed from the keys (a filtered push is then not applied).
	void *in = nullptr;
	Vector push(0, 0, 0);
	bool inVolume = false;
	for (void *e : g_pushes) {
		if (!live.count(e) || Disabled(e)) continue;
		const PushRules &r = PushRulesOf(e);
		if (!r.clients) continue;
		Vector mn, mx;
		if (!WorldBox(e, mn, mx) || !Overlaps(mn - Vector(2, 2, 2), mx + Vector(2, 2, 2), feet)) continue;  // (2 u margin: see CatapultsFrame)
		inVolume = true;
		if (g_baseOff >= 0) {
			if (now - g_baseVelAt < 0.15) in = e, push = g_baseVel;
		} else if (!r.filtered)
			in = e, push = r.dir * r.speed;  // (the last toucher wins, as the base velocity is replaced)
	}
	(void)inVolume;
	if (in) {
		if (!g_inPush) {
			++g_pushEntries;
			if (g_logLeft > 0) --g_logLeft, console->Print("[bridge] mech: the Minecraft player is in push %s: (%.0f %.0f %.0f) u/s%s\n", EntKV(in, "targetname").c_str(), push.x, push.y, push.z, g_baseOff >= 0 ? " (Portal 2's own push of the body)" : " (computed)");
		}
		if (!g_inPush || now - g_pushSentAt2 >= 0.05) SendVel(kMechPush, push), g_pushSentAt2 = now, ++g_pushSends;
	} else if (g_inPush) {
		SendVel(kMechPush, Vector(0, 0, 0));
		if (g_logLeft > 0) --g_logLeft, console->Print("[bridge] mech: the Minecraft player left the push\n");
	}
	g_inPush = in != nullptr;
}
// ---- fizzlers --------------------------------------------------------------------------------------------------------
void FizzleThePlayer(const std::unordered_set<void *> &live, const Vector &feet) {
	int closed = 0;
	for (void *p : EntFind("prop_portal")) {
		if (!live.count(p)) continue;
		static int offAct = -2;
		if (offAct == -2) offAct = EntFieldOffset(p, "m_bActivated");
		const bool active = offAct >= 0 ? *((const unsigned char *)p + offAct) != 0 : EntKV(p, "Activated") == "1";
		if (!active) continue;
		const std::string grp = EntKV(p, "LinkageGroupID");
		if (!grp.empty() && std::atoi(grp.c_str()) != 0) continue;  // (only the player's pair: group 0; scripted pairs of other groups stay)
		if (EntInput(p, "Fizzle")) ++closed;
	}
	g_portalsFizzled += closed;
	// what the player holds dissolves (Portal 2's own cleanser takes a held cube that touches it; this covers the player touching it first)
	bool dropped = false;
	if (PlayerHolding()) {
		void *best = nullptr;
		float bd = 150.0f;
		const Vector eye = feet + Vector(0, 0, 64);
		for (void *c : live) {
			auto it = g_classOf.find(c);
			if (it == g_classOf.end()) continue;
			const std::string &cls = it->second;
			if (cls != "prop_weighted_cube" && cls != "prop_monster_box" && cls.rfind("prop_physics", 0) != 0 && cls != "npc_portal_turret_floor" && cls != "prop_glass_futbol") continue;
			const float d = (EntVec(c, "origin") - eye).Length();
			if (d < bd) bd = d, best = c;
		}
		if (best) {
			if (!EntInput(best, "Fizzle")) EntInput(best, "Dissolve");
			dropped = true;
			++g_dropped;
		}
	}
	++g_fizzles;
	if (g_logLeft > 0) --g_logLeft, console->Print("[bridge] mech: the Minecraft player touched a fizzler: %d portals closed%s\n", closed, dropped ? ", the held object dissolved" : "");
}
bool g_cleanserInside = false;
void CleansersFrame(const std::unordered_set<void *> &live, const Vector &feet) {
	bool inside = false;
	for (void *e : g_cleansers) {
		if (!live.count(e) || Disabled(e)) continue;
		Vector mn, mx;
		if (WorldBox(e, mn, mx) && Overlaps(mn - Vector(1, 1, 1), mx + Vector(1, 1, 1), feet)) inside = true;
	}
	if (inside && !g_cleanserInside) FizzleThePlayer(live, feet);
	g_cleanserInside = inside;
}

// ---- excursion funnels -------------------------------------------------------------------------------------------------
struct Beam {
	Vector from, dir;
	float length = 0, radius = 64, force = 150;
	double tracedAt = -1e9;
};
std::unordered_map<void *, Beam> g_beamGeom;
bool g_inBeam = false;
double g_beamSentAt = 0;

// The emitter's ray to the first wall (cached: the world does not move), its linearForce (signed: polarity) and radius.
const Beam *BeamOf(void *e, double now) {
	Beam &b = g_beamGeom[e];
	if (now - b.tracedAt > 1.0) {
		b.tracedAt = now;
		b.from = EntVec(e, "origin");
		b.dir = Forward(EntVec(e, "angles"));
		b.force = KVf(e, "linearForce", 150.0f);
		b.radius = KVf(e, "use128model", 0.0f) != 0.0f ? 64.0f : 48.0f;
		CGameTrace tr;
		engine.Trace(b.from + b.dir * 4.0f, b.from + b.dir * 8192.0f, Vector(0, 0, 0), MASK_SOLID_BRUSHONLY, tr, true);
		b.length = tr.fraction * 8192.0f;
	}
	return &b;
}
void BeamsFrame(const std::unordered_set<void *> &live, const Vector &feet, double now) {
	const Vector c = feet + Vector(0, 0, 36);
	const Beam *in = nullptr;
	void *inEnt = nullptr;
	Vector lateral(0, 0, 0);
	// (bridge #68) funnels through portals: Portal 2 makes one trigger_tractorbeam per beam SEGMENT (the emitter's, and one more for each portal the beam
	// enters: the child starts at the exit portal), each holding its own start, end, radius and signed force. The segments are what the player is carried by,
	// as with the lasers (#59). The emitter's own ray (below) is the fallback when the segment layout is not readable.
	Beam segBeam;
	static int offS = -2, offE = -2, offF = -2, offR = -2, offRev = -2, offDis = -2;
	bool segOk = false;
	if (!g_beamTrigs.empty()) {
		void *t0 = g_beamTrigs[0];
		if (offS == -2) offS = EntFieldOffset(t0, "m_vStart"), offE = EntFieldOffset(t0, "m_vEnd"), offF = EntFieldOffset(t0, "m_linearForce"), offR = EntFieldOffset(t0, "m_flRadius"), offRev = EntFieldOffset(t0, "m_bReversed"), offDis = EntFieldOffset(t0, "m_bDisabled");
		segOk = offS >= 0 && offE >= 0 && offF >= 0 && offR >= 0;
	}
	if (segOk) {
		for (void *e : g_beamTrigs) {
			if (!live.count(e) || (offDis >= 0 && *((const unsigned char *)e + offDis) != 0)) continue;
			const Vector a = *(const Vector *)((const char *)e + offS), z = *(const Vector *)((const char *)e + offE);
			const float len = (z - a).Length();
			const float force = *(const float *)((const char *)e + offF), radius = *(const float *)((const char *)e + offR);
			if (!std::isfinite(len + force + radius) || len < 8.0f || len > 20000.0f || radius < 8.0f || radius > 400.0f) continue;
			const Vector dir = (z - a) * (1.0f / len);
			const Vector d = c - a;
			const float along = Vector::DotProduct(d, dir);
			if (along < -16.0f || along > len + 8.0f) continue;
			const Vector lat = d - dir * along;
			if (lat.Length() > radius) continue;
			segBeam.from = a, segBeam.dir = dir, segBeam.length = len, segBeam.radius = radius, segBeam.force = force;
			in = &segBeam, inEnt = e, lateral = lat;
			static int segLogs = 0;
			if (segLogs < 12 && !g_inBeam) ++segLogs, console->Print("[bridge] mech: funnel segment %p: (%.0f %.0f %.0f) -> (%.0f %.0f %.0f), signed force %.0f, radius %.0f, reversed %d\n", e, a.x, a.y, a.z, z.x, z.y, z.z, force, radius, offRev >= 0 ? *((const unsigned char *)e + offRev) : -1);
			break;
		}
	}
	for (void *e : g_beams) {
		if (segOk) break;
		if (!live.count(e) || !BeamEnabled(e)) continue;
		const Beam *b = BeamOf(e, now);
		const Vector d = c - b->from;
		const float along = Vector::DotProduct(d, b->dir);
		if (along < -16.0f || along > b->length + 8.0f) continue;
		const Vector lat = d - b->dir * along;
		if (lat.Length() > b->radius) continue;
		in = b, inEnt = e, lateral = lat;
		break;
	}
	if (in) {
		// the beam's velocity: linearForce along it (signed), pulled to the axis (up to 150 u/s), no gravity (the mod's side)
		Vector pull = lateral * -4.0f;
		if (pull.Length() > 150.0f) pull = pull * (150.0f / pull.Length());
		const Vector v = in->dir * in->force + pull;
		if (!g_inBeam) {
			++g_beamEntries;
			if (g_logLeft > 0) --g_logLeft, console->Print("[bridge] mech: the Minecraft player is in funnel %s (force %.0f, %.0f u from its axis)\n", EntKV(inEnt, "targetname").c_str(), in->force, lateral.Length());
		}
		if (!g_inBeam || now - g_beamSentAt >= 0.05) SendVel(kMechBeam, v), g_beamSentAt = now, ++g_beamSends;
	} else if (g_inBeam) {
		SendVel(kMechBeamEnd, Vector(0, 0, 0));
		if (g_logLeft > 0) --g_logLeft, console->Print("[bridge] mech: the Minecraft player left the funnel\n");
	}
	g_inBeam = in != nullptr;
}


// ---- lasers (bridge #37, #59) ---------------------------------------------------------------------------------------------------
// tbtest full4 laser.beam.mc: the body stood in the beam for 2.5 s and Portal 2's health never changed (Chell, in the same beam, lost 2 health and was
// pushed out of it): Portal 2's own sweep (research/specs/test-elements.md 3) does not reach the puppet body. The same rule is applied from here to
// the Minecraft player, per beam segment (each env_portal_laser entity: a reflected or portal-continued segment is its own entity): the body is hit
// when its origin is within 16 u (horizontally) of the segment and the segment is inside its hull. A hit is 150 * frametime burn damage (1000 for a
// lethal emitter), sent as burn (Minecraft: onFire), and the player is pushed horizontally away from the beam (120 u/s, horizontal only, bridge #57).
// Portal 2's own hit, if it ever lands on the body, wins: nothing is added for 0.5 s after a burn it forwarded (combat.cpp).
//
// (bridge #59) WHICH segment: the one Portal 2 itself computed. Every env_portal_laser keeps its current start, its end (the first thing its beam
// trace met: wall, cube, turret, door, a Minecraft block our trace hook clips it at, a portal, a reflection cube, a catcher) and an on flag; the
// bridge #37 sweep re-traced from the emitter against brushes only, so a cube or a placed block in the way did not stop it and the Minecraft player
// kept burning behind it. Now the stored segment is the beam: whatever Portal 2 lets through (glass and grates are not in the beam's trace mask, a
// fizzler is not solid, a light bridge is on its ignore list) is through here too, and a portal hop / a reflection is the child entity's own segment.
// The three fields are read at their offsets in the server's entity (found by name in the datamap when it lists them; else this build's layout, but
// only when two other datamap fields of the class sit where that layout puts them). Without a usable layout the old brush-only sweep still runs.
struct LaserLayout {
	bool resolved = false, ok = false;
	int on = -1, start = -1, end = -1, lethal = -1;
};
LaserLayout g_ll;
void ResolveLaserLayout(void *e) {
	if (g_ll.resolved) return;
	g_ll.resolved = true;
	g_ll.on = EntFieldOffset(e, "m_bLaserOn"), g_ll.start = EntFieldOffset(e, "m_vStartPoint"), g_ll.end = EntFieldOffset(e, "m_vEndPoint"), g_ll.lethal = EntFieldOffset(e, "m_bIsLethal");
	const int child = EntFieldOffset(e, "m_pChildLaser"), refl = EntFieldOffset(e, "m_bFromReflectedCube");  // (anchors: datamap fields of the class)
	const bool anchored = (child == 0x4d0 || refl == 0x775) && (child < 0 || child == 0x4d0) && (refl < 0 || refl == 0x775);
	if (anchored) {
		if (g_ll.start < 0) g_ll.start = 0x780;
		if (g_ll.end < 0) g_ll.end = 0x78c;
		if (g_ll.on < 0) g_ll.on = 0x798;
		if (g_ll.lethal < 0) g_ll.lethal = 0x799;
	}
	g_ll.ok = g_ll.start >= 0 && g_ll.end >= 0 && g_ll.on >= 0;
	console->Print("[bridge] mech: laser layout %s (anchors child %d reflected %d; on +%d start +%d end +%d lethal +%d)\n", g_ll.ok ? "ok" : "UNKNOWN, the old brush-only sweep is used", child, refl, g_ll.on, g_ll.start, g_ll.end, g_ll.lethal);
}
bool LaserOn(void *e) {
	ResolveLaserLayout(e);
	return g_ll.on < 0 ? true : *((const unsigned char *)e + g_ll.on) != 0;
}
float CvarF(const char *name, float def) {
	if (!tier1 || !tier1->FindCommandBase) return def;
	auto *v = (ConVar *)tier1->FindCommandBase(tier1->g_pCVar->ThisPtr(), name);
	if (!v) return def;
	v = v->m_pParent ? v->m_pParent : v;
	return v->m_fValue;
}
double g_laserAt = -1e9, g_pushAt = -1e9;
int g_laserLog = 20, g_laserLayoutBad = 0;
// 0: this laser is off (no beam), 1: a = start and b = end of its beam segment as Portal 2 holds them, -1: the layout is unknown / the data is not usable
int RealBeam(void *e, Vector &a, Vector &b, bool &lethal) {
	ResolveLaserLayout(e);
	if (!g_ll.ok) return -1;
	if (!*((const unsigned char *)e + g_ll.on)) return 0;
	a = *(const Vector *)((const char *)e + g_ll.start), b = *(const Vector *)((const char *)e + g_ll.end);
	const bool finite = std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z) && std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.z);
	if (!finite || (a - b).Length() > 60000.0f) return -1;
	if (a.x == 0.0f && a.y == 0.0f && a.z == 0.0f && b.x == 0.0f && b.y == 0.0f && b.z == 0.0f) return 0;  // (on, but its first update has not run)
	// the emitter and a child both sit at their segment's start: a start far from the entity means the three numbers are not what they were taken for
	if ((a - EntVec(e, "origin")).Length() > 400.0f) return -1;
	lethal = g_ll.lethal >= 0 ? *((const unsigned char *)e + g_ll.lethal) != 0 : KVf(e, "LethalDamage", 0.0f) != 0.0f;
	return 1;
}
// the thing that stopped the beam at `b` is a player (the puppet): a short ray through the end, with entities, in the beam's own mask
bool BeamEndsOnPlayer(const Vector &a, const Vector &b) {
	const Vector d = b - a;
	const float l = d.Length();
	if (l < 1.0f) return false;
	const Vector dir = d * (1.0f / l);
	CGameTrace tr;
	engine.Trace(b - dir * 2.0f, b + dir * 3.0f, Vector(0, 0, 0), 0x46004001u, tr, false);
	return tr.m_pEnt && EntKV(tr.m_pEnt, "classname") == "player";
}
// the closest approach of the segment a..b to the body's axis; true when it is a hit; `away` is the horizontal direction from the beam to the body.
// (bridge #59) A beam that ENDS short of the body (the closest point is its end) hurts only when the thing that ended it was the body itself: Portal 2's
// hull keeps a character 16 u from whatever blocks the beam, the Minecraft player's is 12 u wide (0.3 block), so "within 16 u of the end" burned a
// player standing flush behind a block or a cube.
bool BeamHitsBody(const Vector &a, const Vector &b, const Vector &feet, Vector &away) {
	const Vector d = b - a;
	const float l2 = d.x * d.x + d.y * d.y;
	Vector cp = a;
	bool hit;
	float hd;
	if (l2 < 1.0f) {  // (a vertical beam)
		hd = std::hypot(feet.x - a.x, feet.y - a.y);
		hit = hd <= 16.0f && std::fmax(a.z, b.z) >= feet.z && std::fmin(a.z, b.z) <= feet.z + 72.0f;
	} else {
		const float tu = ((feet.x - a.x) * d.x + (feet.y - a.y) * d.y) / l2;
		const float t = std::fmax(0.0f, std::fmin(1.0f, tu));
		cp = a + d * t;
		hd = std::hypot(feet.x - cp.x, feet.y - cp.y);
		hit = hd <= 16.0f && cp.z >= feet.z - 4.0f && cp.z <= feet.z + 76.0f;  // (the hull, with the few units the Minecraft body's feet can differ by)
		if (hit && tu > 1.0f) hit = BeamEndsOnPlayer(a, b);
	}
	if (hd > 1.0f) away = Vector((feet.x - cp.x) / hd, (feet.y - cp.y) / hd, 0);
	else {
		const float l = std::hypot(d.x, d.y);
		away = l > 1e-3f ? Vector(-d.y / l, d.x / l, 0) : Vector(1, 0, 0);
	}
	return hit;
}
void LasersFrame(const std::unordered_set<void *> &live, const Vector &feet, double now) {
	if (now - g_laserAt < 0.05) return;
	const double prevAt = g_laserAt;
	g_laserAt = now;
	if (CvarF("sv_player_collide_with_laser", 1.0f) == 0.0f) return;
	// (bridge #55) a real elapsed time per update (the sweep runs at most every 0.05 s, often later on a slow frame), so the damage rate is Portal 2's
	// 150 per second whatever the update rate; each emitter counts once
	const double dt = std::fmax(0.016, std::fmin(0.25, now - prevAt));
	std::unordered_set<void *> seen;
	for (void *e : g_lasers) {
		if (!live.count(e) || !seen.insert(e).second) continue;
		Vector from, to;
		bool lethal = false;
		const int st = RealBeam(e, from, to, lethal);
		if (st == 0) continue;
		if (st < 0) {  // (no usable layout: the bridge #37 sweep, emitter to the first brush or open portal)
			if (!LaserOn(e)) continue;
			if (g_laserLayoutBad++ == 0) console->Print("[bridge] mech: the laser %s has no readable beam segment: brush-only sweep for it\n", EntKV(e, "targetname").c_str());
			const Vector o = EntVec(e, "origin"), fwd = Forward(EntVec(e, "angles"));
			from = o + fwd * 24.0f;
			CGameTrace tr;
			engine.Trace(from, from + fwd * 8192.0f, Vector(0, 0, 0), MASK_SOLID_BRUSHONLY, tr, true);
			float len = tr.fraction * 8192.0f;
			for (int i = 0; i < kPortalSlots; ++i) {  // (an open portal ends the segment: the beam goes on from the linked portal, as its own entity)
				if (!PortalLinked(i)) continue;
				const float dn = Vector::DotProduct(fwd, g_portalNormal[i]);
				if (dn > -0.05f) continue;
				const float t = Vector::DotProduct(g_portalPos[i] - from, g_portalNormal[i]) / dn;
				if (t > 0.0f && t < len && (from + fwd * t - g_portalPos[i]).Length() < 48.0f) len = t;
			}
			to = from + fwd * len;
			lethal = KVf(e, "LethalDamage", 0.0f) != 0.0f;
		}
		Vector away;
		if (!BeamHitsBody(from, to, feet, away)) continue;
		const float dmg = lethal ? 1000.0f : (float)(150.0 * (prevAt > now - 1.0 ? dt : 0.05));  // (150 per second of contact: the first update after a gap counts 0.05 s)
		++g_laserHits;
		if (!CombatRecentHurtClass(2, 0.5)) CombatInjectHurt(2, dmg);
		if (g_laserLog > 0) --g_laserLog, console->Print("[bridge] mech: the laser %s hit the Minecraft player at (%.0f %.0f %.0f): %.1f burn damage%s, segment (%.0f %.0f %.0f) -> (%.0f %.0f %.0f)\n", EntKV(e, "targetname").c_str(), feet.x, feet.y, feet.z, dmg, lethal ? " (lethal)" : "", from.x, from.y, from.z, to.x, to.y, to.z);
		const Vector fwd = (to - from).Length() > 1.0f ? (to - from) * (1.0f / (to - from).Length()) : Vector(1, 0, 0);
		const bool steep = std::fabs(fwd.z) >= 0.2f;
		if (now - g_pushAt >= 0.4 && !(steep && !McOnGround())) {  // (steep beams do not push an airborne player)
			g_pushAt = now;
			SendVel(kMechLaunch, away * 120.0f);  // (bridge #57: a gentle horizontal nudge out of the beam, never vertical)
		}
	}
}

// ---- gels (bridge #37) ---------------------------------------------------------------------------------------------------------------
// Portal 2 keeps what paint a player stands on as PaintPowerInfo per power (0 bounce, 2 speed, 3 portal; state 1 = active), read through
// PaintPowerUser::GetPaintPower on the body after every server move (puppet.cpp calls McMechanicsAfterMove). The body stands where Minecraft's
// player does, so its paint state is the Minecraft player's. What the powers do (research/specs/test-elements.md 2.2, 2.3):
//   bounce: touching bounce gel and (landing / falling into it, or a jump from it): v' = tangential + the bounce (500 u/s along the normal on a
//           floor; mirrored when the impact is faster; the wall jump 365 out + 275 up on a wall), sent as a launch (the fling physics)
//   speed:  the ground speed limit grows from 175 to 800 u/s at 500 u/s^2 while moving on it and decays at 500 u/s^2 off it; the mod scales the
//           Minecraft player's ground speed by limit/175 (kMechSpeed)
// Not done: the 0.375 forward bonus, the ledge-fall boost, the wall-jump help, the auto-bounce above 500 u/s, conversion (white) gel (portal
// placement on it is the portal gun's), speed gel funnelling into floor portals.
struct Paint {
	int state = 0, type = -1;
	Vector n{0, 0, 0};
	double readAt = -1e9, activeAt = -1e9, touchAt = -1e9;  // (activeAt: state 1; touchAt: state 1 or 2, any power in use)
};
Paint g_paint[4];
bool g_paintLogAll = false, g_paintBad = false;
int g_paintLog = 24;
void ReadPaint(void *player, double now) {
	if (g_paintBad || !player) return;
	if (Offsets::GetPaintPower <= 0) {
		g_paintBad = true;
		console->Print("[bridge] mech: no GetPaintPower offset for this Portal 2 build: gels are off for the Minecraft player\n");
		return;
	}
	using _GetPaintPower = const char *(__rescall *)(void *thisptr, unsigned paintId);
#ifdef _WIN32
	// Windows: GetPaintPower is called on the embedded paint-power user at player + 0x1250 (upstream SourceAutoRecord TasPlayer.cpp), not on the player.
	void *paintUser = (char *)player + 0x1250;
#else
	void *paintUser = player;
#endif
	auto fn = Memory::VMT<_GetPaintPower>(paintUser, Offsets::GetPaintPower);
	for (unsigned id = 0; id < 4; ++id) {
		const char *p = fn(paintUser, id);
		if (!p) return;
		// PaintPowerInfo_t (32-bit): vtable, surface normal (3 floats), contact point (3), power type, other's handle, state, thin flag
		const float *nf = (const float *)(p + 4);
		const int type = *(const int *)(p + 28), state = *(const int *)(p + 36);
		if (state < 0 || state > 8) {
			g_paintBad = true;
			console->Print("[bridge] mech: the paint power record of the body reads state %d: the gel code is off (check Offsets::GetPaintPower %d)\n", state, Offsets::GetPaintPower);
			return;
		}
		Paint &pt = g_paint[id];
		if ((state != pt.state || g_paintLogAll) && g_paintLog > 0 && McModeActive()) {
			--g_paintLog;
			console->Print("[bridge] mech: paint power %u (type %d) state %d -> %d, normal (%.2f %.2f %.2f)\n", id, type, pt.state, state, nf[0], nf[1], nf[2]);
		}
		pt.state = state, pt.type = type, pt.n = Vector(nf[0], nf[1], nf[2]), pt.readAt = now;
		if (state == 1) pt.activeAt = now;
		if (state == 1 || state == 2) pt.touchAt = now;
	}
}
// Portal 2's bounce (research/specs/test-elements.md 2.2) for a velocity v against a surface of normal n
Vector BounceVelocity(const Vector &v, const Vector &n) {
	const Vector up(0, 0, 1);
	const float vn = Vector::DotProduct(v, n);
	auto wallJump = [&](float s) {
		float d = Vector::DotProduct(n, up);
		Vector extra(0, 0, 0);
		if (d > -0.1f) extra = up * (275.0f * (1.0f - d));
		else {
			Vector h = n - up * d;
			const float hl = h.Length();
			h = hl > 1e-4f ? h * (1.0f / hl) : Vector(0, 0, 0);
			extra = h * (275.0f * Vector::DotProduct(n, h));
			d = 1.0f - Vector::DotProduct(n, h);
		}
		const float k = (1.0f - 0.73f) * d + 0.73f;
		return n * (s * k) + extra;
	};
	const Vector tangential = v - n * vn;
	if (std::fabs(vn) >= 500.0f) {  // reflection mode
		if (std::fabs(n.z) <= 0.001f) {
			Vector r = tangential + wallJump(std::fabs(vn));
			if (r.z < 10.0f) r.z = 10.0f;
			return r;
		}
		return tangential - n * vn;
	}
	return tangential + wallJump(500.0f);
}
double g_gelAt = -1e9, g_bounceAt = -1e9, g_speedSentAt = -1e9, g_nativeBounceAt = -1e9;
Vector g_nativeBounce{0, 0, 0};
bool g_gPrevGround = false, g_onSpeed = false, g_speedActive = false;
float g_maxSpeed = 175.0f, g_fallSpeed = 0.0f;
Vector g_prevV{0, 0, 0};
void GelFrame(const Vector &feet, double now) {
	const Vector v = McVelocitySrc();
	const bool ground = McOnGround();
	const double dt = std::fmin(0.1, std::fmax(0.0, now - g_gelAt));
	g_gelAt = now;
	// --- bounce gel
	const Paint &b = g_paint[0];
	const bool touching = now - b.touchAt < 0.12;
	if (!ground && v.z < 0) g_fallSpeed = std::fmax(g_fallSpeed, -v.z);
	else if (ground && g_gPrevGround) g_fallSpeed = 0.0f;
	if (now - g_nativeBounceAt < 0.15 && now - g_bounceAt > 0.3) {  // Portal 2 gave the body the bounce itself: that is the answer
		g_bounceAt = now, g_nativeBounceAt = -1e9;
		++g_bounces;
		SendVel(kMechLaunch, g_nativeBounce);
		if (g_logLeft > 0) --g_logLeft, console->Print("[bridge] mech: bounce gel (Portal 2's own move of the body): velocity (%.0f %.0f %.0f) u/s\n", g_nativeBounce.x, g_nativeBounce.y, g_nativeBounce.z);
		g_fallSpeed = 0.0f;
	} else if (touching && now - g_bounceAt > 0.3) {
		Vector n = b.n;
		const float nl = n.Length();
		n = nl > 0.5f ? n * (1.0f / nl) : Vector(0, 0, 1);
		Vector vin = v;
		bool go = false;
		const char *why = "";
		if (ground && !g_gPrevGround && g_fallSpeed >= 4.0f) go = true, why = "landed", vin = Vector(v.x, v.y, -g_fallSpeed);      // landing: the speed it arrived with
		else if (!ground && v.z < -20.0f && n.z > 0.5f) go = true, why = "fell into it";                            // falling into the contact
		else if (v.z > 100.0f && (g_gPrevGround || ground) && n.z > 0.5f) go = true, why = "jumped from it", vin = Vector(v.x, v.y, 0);  // jump pressed on it
		else if (std::fabs(n.z) <= 0.001f && std::fmax(-Vector::DotProduct(v, n), -Vector::DotProduct(g_prevV, n)) >= 4.0f) go = true, why = "ran into it", vin = (-Vector::DotProduct(v, n) >= -Vector::DotProduct(g_prevV, n)) ? v : g_prevV;  // a wall (Minecraft zeroes the velocity into it: the sample before counts)
		if (go) {
			// (the landing's own velocity is along the normal for a floor: the fall speed; for a wall the velocity into it)
			const Vector r = BounceVelocity(vin, n);
			g_bounceAt = now;
			++g_bounces;
			SendVel(kMechLaunch, r);
			if (g_logLeft > 0)
				--g_logLeft, console->Print("[bridge] mech: bounce gel (%s): normal (%.2f %.2f %.2f), velocity (%.0f %.0f %.0f) -> (%.0f %.0f %.0f) u/s\n", why, n.x, n.y, n.z, vin.x, vin.y, vin.z, r.x, r.y, r.z);
			g_fallSpeed = 0.0f;
		}
	}
	g_gPrevGround = ground, g_prevV = v;
	// --- speed gel
	const Paint &sp = g_paint[2];
	const bool onSpeed = now - sp.activeAt < 0.12 && ground;
	const float hv = std::hypot(v.x, v.y);
	if (onSpeed) {
		if (!g_onSpeed) g_maxSpeed = std::fmax(175.0f, std::fmin(800.0f, hv));
		else if (hv > 100.0f) g_maxSpeed = std::fmin(800.0f, g_maxSpeed + 500.0f * (float)dt);
		else g_maxSpeed = std::fmax(175.0f, g_maxSpeed - 500.0f * (float)dt);
	} else if (ground) g_maxSpeed = std::fmax(175.0f, g_maxSpeed - 500.0f * (float)dt);
	g_onSpeed = onSpeed;
	const bool want = g_maxSpeed > 180.0f && (onSpeed || ground);
	if ((want || g_speedActive) && now - g_speedSentAt >= 0.05) {
		g_speedSentAt = now;
		const int m = (int)std::lround((want ? g_maxSpeed / 175.0f : 1.0f) * 10000.0f);
		link::PushInput(kInMech, kMechSpeed, m, 0, 0);
		++g_speedSends;
		if (!g_speedActive && want && g_logLeft > 0) --g_logLeft, console->Print("[bridge] mech: speed gel: the Minecraft player's ground speed limit %.0f u/s (x%.2f)\n", g_maxSpeed, g_maxSpeed / 175.0f);
		g_speedActive = want;
	}
}

// ---- portal link watch (bridge #37) ------------------------------------------------------------------------------------------------------
// tbtest full4 fling.fling.chell: the testbed's static floor+wall pair (group 2) is open (m_bActivated) and Chell still lands on the floor portal: no
// transit, no hole. If the engine left it unlinked (m_hLinkedPortal empty) that is exactly what it looks like. Every 0.5 s, in both modes: an active
// portal of a group other than 0 (the gun's pair is never touched) whose linkage group has another active portal but whose own link handle is empty gets SetActivatedState 1 again (Portal 2 re-runs its linkage
// on it, as clip.cpp's Redirect already does for a floor portal whose partner came later), at most 3 times per portal and 2 s apart; every action and the
// first sight of each linked pair is logged with the raw handles, so the next run says what the engine really held.
std::vector<void *> g_portals;
std::unordered_map<void *, double> g_relinkAt;
std::unordered_map<void *, int> g_relinks;
std::unordered_set<void *> g_linkSeen;
double g_linkWatchAt = -1e9;
int g_linkLog = 24;
bool PortalActiveNow(void *p) {
	static int off = -2;
	if (off == -2) off = EntFieldOffset(p, "m_bActivated");
	return off >= 0 ? *((const unsigned char *)p + off) != 0 : EntKV(p, "Activated") == "1";
}
unsigned PortalLinkHandle(void *p) {
	static int off = -2;
	if (off == -2) off = EntFieldOffset(p, "m_hLinkedPortal");
	return off >= 0 ? *(const unsigned *)((const char *)p + off) : 0xFFFFFFFDu;
}
// (bridge #39) Chell standing still on a linked floor portal of a map-placed pair: the transit never came (tbtest full5 fling.fling.chell: the map's group 2 pair
// sat exactly on the floor face and Chell stood on the open portal at z 0.0 for 4 s, while the same pair put there by the placement command, 0.5 u above the floor,
// flung her). Looked at every 0.5 s in Chell mode, for any active, linked portal of a group other than 0 whose normal points up: her feet within 3 u of its plane,
// within 30 u of its centre, not moving, for 0.75 s. Then the portal is placed again 0.5 u out along its normal (NewLocation, the input the placement command
// ends in), at most twice per portal; a line says so. Gun portals are Portal 2's own and are never touched.
std::unordered_map<void *, double> g_floorStandSince;
std::unordered_map<void *, int> g_floorNudges;
void FloorPortalWatch(void *p, double now) {
	if (McModeActive()) return;
	const Vector o = EntVec(p, "origin"), a = EntVec(p, "angles");
	const float pp = a.x * (float)M_PI / 180.0f, py = a.y * (float)M_PI / 180.0f;
	const Vector n(std::cos(pp) * std::cos(py), std::cos(pp) * std::sin(py), -std::sin(pp));
	if (n.z < 0.7f) return;
	const Vector feet = PlayerOrigin(), vel = PlayerVelocity();
	if (std::fabs(feet.z - o.z) > 3.0f || std::hypot(feet.x - o.x, feet.y - o.y) > 30.0f || vel.Length() > 5.0f) {
		g_floorStandSince.erase(p);
		return;
	}
	const double since = g_floorStandSince.emplace(p, now).first->second;
	if (now - since < 0.75 || g_floorNudges[p] >= 2) return;
	++g_floorNudges[p];
	g_floorStandSince[p] = now;
	char buf[160];
	std::snprintf(buf, sizeof buf, "%.2f %.2f %.2f %.1f %.1f %.1f", o.x + n.x * 0.5f, o.y + n.y * 0.5f, o.z + n.z * 0.5f, a.x, a.y, a.z);
	const bool ok = EntInput(p, "NewLocation", buf);
	console->Print("[bridge] linkwatch: Chell stands still on the linked floor portal '%s' (group %s) at feet (%.1f %.1f %.1f), portal (%.2f %.2f %.2f): no transit; the portal is placed again 0.5 u out (NewLocation \"%s\", input %s, #%d)\n",
	               EntKV(p, "targetname").c_str(), EntKV(p, "LinkageGroupID").c_str(), feet.x, feet.y, feet.z, o.x, o.y, o.z, buf, ok ? "sent" : "FAILED", g_floorNudges[p]);
}
void PortalLinkWatch(const std::unordered_set<void *> &live, double now) {
	if (now - g_linkWatchAt < 0.5) return;
	g_linkWatchAt = now;
	for (void *p : g_portals) {
		if (!live.count(p) || !PortalActiveNow(p)) continue;
		const std::string grp = EntKV(p, "LinkageGroupID");
		void *partner = nullptr;
		for (void *q : g_portals)
			if (q != p && live.count(q) && PortalActiveNow(q) && EntKV(q, "LinkageGroupID") == grp) partner = q;
		if (!partner) continue;
		const unsigned h = PortalLinkHandle(p);
		if (h != 0xFFFFFFFFu && h != 0u && h != 0xFFFFFFFDu) {
			if (g_linkSeen.insert(p).second && g_linkLog > 0)
				--g_linkLog, console->Print("[bridge] linkwatch: portal '%s' (group %s) is linked: m_hLinkedPortal 0x%x\n", EntKV(p, "targetname").c_str(), grp.c_str(), h);
			if (std::atoi(grp.c_str()) != 0) FloorPortalWatch(p, now);  // (bridge #39)
			continue;
		}
		if (h == 0xFFFFFFFDu) continue;  // (no such field: nothing to judge by)
		if (std::atoi(grp.c_str()) == 0) continue;  // (the gun's pair is Portal 2's own: never touched here)
		if (g_relinks[p] >= 3 || now - g_relinkAt[p] < 2.0) continue;
		g_relinkAt[p] = now;
		++g_relinks[p];
		EntInput(p, "SetActivatedState", "1");
		if (g_linkLog > 0)
			--g_linkLog, console->Print("[bridge] linkwatch: portal '%s' (group %s, PortalTwo %s) is active and so is '%s', but its m_hLinkedPortal is 0x%x: SetActivatedState sent again (#%d)\n",
			                           EntKV(p, "targetname").c_str(), grp.c_str(), EntKV(p, "PortalTwo").c_str(), EntKV(partner, "targetname").c_str(), h, g_relinks[p]);
	}
}

void Refresh(const std::unordered_set<void *> &live, double now) {
	g_listAt = now;
	for (auto it = g_classOf.begin(); it != g_classOf.end();)
		if (!live.count(it->first)) it = g_classOf.erase(it);
		else ++it;
	g_catapults.clear(), g_cleansers.clear(), g_beams.clear(), g_beamTrigs.clear(), g_lasers.clear(), g_pushes.clear(), g_portals.clear();  // (bridge #61: g_portals was never cleared here, so every refresh (2-10 a second) appended every prop_portal again and PortalLinkWatch's pair loop grew quadratically with the session: 150-220 ms frames after ~40 minutes; the first #55 note is the same bug for g_lasers) (bridge #55: g_lasers was not cleared here, so every refresh appended every laser again: after 7 s a beam hit counted 144 times = 360 Portal 2 damage in one flush, the "one-shot")
	for (void *e : live) {
		auto ci = g_classOf.find(e);
		if (ci == g_classOf.end()) ci = g_classOf.emplace(e, EntKV(e, "classname")).first;
		const std::string &c = ci->second;
		if (c == "trigger_catapult") g_catapults.push_back(e);
		else if (c == "trigger_push") g_pushes.push_back(e);
		else if (c == "trigger_portal_cleanser") g_cleansers.push_back(e);
		else if (c == "prop_tractor_beam") g_beams.push_back(e);
		else if (c == "trigger_tractorbeam") g_beamTrigs.push_back(e);
		else if (c == "env_portal_laser") g_lasers.push_back(e);
		else if (c == "prop_portal") g_portals.push_back(e);
	}
	for (auto it = g_touch.begin(); it != g_touch.end();)
		if (!live.count(it->first)) it = g_touch.erase(it);
		else ++it;
	for (auto it = g_plateRules.begin(); it != g_plateRules.end();)
		if (!live.count(it->first)) it = g_plateRules.erase(it);
		else ++it;
	for (auto it = g_pushRules.begin(); it != g_pushRules.end();)
		if (!live.count(it->first)) it = g_pushRules.erase(it);
		else ++it;
	for (auto it = g_beamGeom.begin(); it != g_beamGeom.end();)
		if (!live.count(it->first)) it = g_beamGeom.erase(it);
		else ++it;
}

}  // namespace

// combat.cpp HazardFrame (bridge #59): the beam segment of an env_portal_laser as Portal 2 holds it. 0 off, 1 a..b valid, -1 unknown
int McLaserBeam(void *e, Vector &a, Vector &b, bool &lethal) { return RealBeam(e, a, b, lethal); }

// puppet.cpp, at the start of every server move in Minecraft mode: `velIn` is the body's velocity as the game hands it to the move, `lastSet` what the puppet
// left there at the end of the previous move. A jump between the two is something Portal 2 did to the body in between (a faith plate's launch)
void McMechanicsBodyMove(const Vector &velIn, const Vector &lastSet) {
	if (!g_on) return;
	static int jumpLogs = 0;
	if ((velIn - lastSet).Length() > 150.0f && velIn.Length() > 150.0f && jumpLogs < 30) ++jumpLogs, console->Print("[bridge] mech: Portal 2 changed the body's velocity by %.0f between two moves: (%.0f %.0f %.0f) -> (%.0f %.0f %.0f)\n", (velIn - lastSet).Length(), lastSet.x, lastSet.y, lastSet.z, velIn.x, velIn.y, velIn.z);
	if ((velIn - lastSet).Length() > 150.0f && velIn.Length() > 150.0f) g_nativeVel = velIn, g_nativeAt = NowS();
}

void McMechanicsLevelInit() {
	g_classOf.clear(), g_catapults.clear(), g_cleansers.clear(), g_beams.clear(), g_beamTrigs.clear(), g_lasers.clear(), g_pushes.clear(), g_plateRules.clear(), g_pushRules.clear(), g_inPush = false, g_portals.clear(), g_relinkAt.clear(), g_relinks.clear(), g_linkSeen.clear(), g_floorStandSince.clear(), g_floorNudges.clear(), g_touch.clear(), g_beamGeom.clear();
	g_listAt = -1e9, g_inBeam = false, g_cleanserInside = false, g_logLeft = 60, g_laserLog = 20, g_paintLog = 24;
	for (Paint &p : g_paint) p = Paint();
	g_maxSpeed = 175.0f, g_onSpeed = g_speedActive = false, g_bounceAt = g_pushAt = -1e9, g_gPrevGround = false, g_fallSpeed = 0.0f;
}

// puppet.cpp, after every server move in Minecraft mode: the body's paint powers (what the Minecraft player stands on)
void McMechanicsAfterMove(void *player, const Vector &velIn, const Vector &velOut) {
	if (!g_on) return;
	const double now = NowS();
	ReadPaint(player, now);
	{  // (bridge #67) Portal 2's own trigger_push on the body: its base velocity
		static int logs = 0;
		if (g_baseOff == -2) g_baseOff = EntFieldOffset(player, "m_vecBaseVelocity"), console->Print("[bridge] mech: the body's m_vecBaseVelocity is at %d\n", g_baseOff);
		if (g_baseOff >= 0) {
			const Vector bv = *(const Vector *)((const char *)player + g_baseOff);
			if (std::isfinite(bv.x + bv.y + bv.z) && bv.Length() > 1.0f && bv.Length() < 5000.0f) {
				if (now - g_baseVelAt > 1.0 && logs < 30) ++logs, console->Print("[bridge] mech: Portal 2 pushes the body itself: base velocity (%.0f %.0f %.0f)\n", bv.x, bv.y, bv.z);
				g_baseVel = bv, g_baseVelAt = now;
			}
		}
	}
	// a bounce Portal 2's own move gave the body (it landed on bounce gel in Portal 2's eyes: the paint power fired): the velocity out is Portal 2's bounce
	if (now - g_paint[0].touchAt < 0.12 && velOut.z - velIn.z > 250.0f && velOut.z >= 350.0f) g_nativeBounce = velOut, g_nativeBounceAt = now;
}

// combat.cpp CombatFrame, every frame (live: the server's entities now)
void McMechanicsFrame(const std::unordered_set<void *> &live) {
	if (!g_on) return;
	{  // (both modes) the entity lists, and the portal link watch
		const double t = NowS();
		if (t - g_listAt > (g_lasers.empty() ? 0.5 : 0.1)) Refresh(live, t);  // (bridge #59: a beam's reflected / portal child is a new entity: found within 0.1 s)
		PortalLinkWatch(live, t);
	}
	if (!McModeActive()) {
		if (g_inBeam) g_inBeam = false;  // (the mod drops the beam by itself 250 ms after the last update)
		g_cleanserInside = false, g_inPush = false;
		for (auto &t : g_touch) t.second.inside = false;
		return;
	}
	const double now = NowS();
	if (now - g_listAt > (g_lasers.empty() ? 0.5 : 0.1)) Refresh(live, now);
	const Vector feet = McFeetSrc();
	CatapultsFrame(live, feet, now);
	PushesFrame(live, feet, now);
	CleansersFrame(live, feet);
	BeamsFrame(live, feet, now);
	LasersFrame(live, feet, now);
	GelFrame(feet, now);
}

CON_COMMAND(pcb_mech, "pcb_mech [0|1] - (bridge #32) Portal 2's faith plates, funnels, fizzlers, lasers and gels act on the Minecraft player (default 1); counters\n") {
	if (args.ArgC() > 1) g_on = std::atoi(args[1]) != 0;
	console->Print("[bridge] mech %s: %zu faith plates, %zu fizzlers, %zu funnels, %zu lasers known; launches %d (last %s: %.0f %.0f %.0f), funnel entries %d (%d updates), fizzler touches %d (%d portals closed, %d held objects), "
	               "laser hits %d, gel bounces %d, speed gel updates %d (limit %.0f u/s), %zu pushes known, push entries %d (%d updates)\n",
	               g_on ? "on" : "off", g_catapults.size(), g_cleansers.size(), g_beams.size(), g_lasers.size(), g_launches, g_lastLaunchBy.c_str(), g_lastLaunch.x, g_lastLaunch.y, g_lastLaunch.z, g_beamEntries,
	               g_beamSends, g_fizzles, g_portalsFizzled, g_dropped, g_laserHits, g_bounces, g_speedSends, g_maxSpeed, g_pushes.size(), g_pushEntries, g_pushSends);
}
CON_COMMAND(pcb_mech_plates, "pcb_mech_plates [0|1] - (bridge #77) 1 (default): a faith plate launches the Minecraft player when Portal 2's own touch launches the body (its outputs run: OnCatapulted); 0: the old computed launch from the mod's own volume test\n") {
	if (args.ArgC() > 1) g_plateNative = std::atoi(args[1]) != 0;
	console->Print("[bridge] mech plates: %s\n", g_plateNative ? "Portal 2's own launch of the body" : "computed launch (old)");
}
// pcb_mech_paint [log 0|1]: the body's paint power records (what Portal 2 thinks the Minecraft player stands on)
CON_COMMAND(pcb_mech_paint, "pcb_mech_paint [log 0|1] - (bridge #37) the body's paint powers (0 bounce, 2 speed, 3 portal; state 1 = active); log: print every change while the Minecraft player leads\n") {
	if (args.ArgC() > 2 && !std::strcmp(args[1], "log")) g_paintLogAll = std::atoi(args[2]) != 0, g_paintLog = 400;
	static const char *names[4] = {"bounce", "reflect", "speed", "portal"};
	const double now = NowS();
	console->Print("[bridge] mech paint: GetPaintPower slot %d%s\n", Offsets::GetPaintPower, g_paintBad ? " (BAD: the record read nonsense, gels off)" : "");
	for (int i = 0; i < 4; ++i)
		console->Print("[bridge]   power %d (%s): type %d state %d normal (%.2f %.2f %.2f), read %.2f s ago, active %.2f s ago\n", i, names[i], g_paint[i].type, g_paint[i].state, g_paint[i].n.x, g_paint[i].n.y, g_paint[i].n.z,
		               now - g_paint[i].readAt, now - g_paint[i].activeAt);
}
CON_COMMAND(pcb_mech_log, "pcb_mech_log [n] - (bridge #32) log the next n mechanic events (default 60)\n") { g_logLeft = args.ArgC() > 1 ? std::atoi(args[1]) : 60; }

// pcb_assert_mech reset | <launches|funnel|fizzles|portals_closed|laser|bounces> <min> [max]  (counts since the reset)
CON_COMMAND(pcb_assert_mech, "pcb_assert_mech reset | <launches|funnel|fizzles|portals_closed|laser|bounces> <min> [max] - (bridge #32, #37) mechanic counters since the reset: faith plate launches, funnel entries, fizzler touches, portals the fizzler closed, laser hits, gel bounces\n") {
	static int base[7] = {};
	const int cur[7] = {g_launches, g_beamEntries, g_fizzles, g_portalsFizzled, g_laserHits, g_bounces, g_pushEntries};
	static const char *names[7] = {"launches", "funnel", "fizzles", "portals_closed", "laser", "bounces", "pushes"};
	if (args.ArgC() < 2 || !std::strcmp(args[1], "reset")) return void(std::memcpy(base, cur, sizeof base));
	const int mn = args.ArgC() > 2 ? std::atoi(args[2]) : 1, mx = args.ArgC() > 3 ? std::atoi(args[3]) : 1 << 30;
	for (int i = 0; i < 7; ++i)
		if (!std::strcmp(args[1], names[i])) {
			char buf[240];
			std::snprintf(buf, sizeof buf, "%s since the reset: %d (want %d..%d); last launch (%.0f %.0f %.0f) by %s", names[i], cur[i] - base[i], mn, mx, g_lastLaunch.x, g_lastLaunch.y, g_lastLaunch.z,
			              g_lastLaunchBy.c_str());
			return TestCheck(cur[i] - base[i] >= mn && cur[i] - base[i] <= mx, "mechanic", buf);
		}
	TestCheck(false, "mechanic", "unknown counter");
}

}  // namespace bridge
