// Portalcraft bridge: player puppet test (Phase 0 stretch c).
//
// Minecraft mode will make Minecraft authoritative for the player: every tick
// the plugin writes Minecraft's position/velocity into Portal 2's player and
// drives the view angles from Minecraft's camera. This file proves the two
// mechanisms with a scripted path instead of Minecraft:
//   * position/velocity: a vtable hook on CGameMovement::ProcessMovement
//     (interface "GameMovement001", in both server.so and client.so so client
//     prediction agrees) that overwrites the move data's origin and velocity
//     after the game's own movement ran. That is the same place SourceAutoRecord
//     writes its trace-teleport positions.
//   * view: IVEngineClient::SetViewAngles each frame.
#include "bridge.hpp"
#include "prof.hpp"

#include "Modules/Tier1.hpp"
#include "Offsets.hpp"
#include "Utils/SDK/GameMovement.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace bridge {
bool McLeads();  // mc.cpp

static Interface *g_serverMovement = nullptr;
static Interface *g_clientMovement = nullptr;
using _ProcessMovement = int(__rescall *)(void *thisptr, void *player, CMoveData *move);
static _ProcessMovement ServerProcessMovement = nullptr;
static _ProcessMovement ClientProcessMovement = nullptr;

static bool g_active = false;
static Vector g_centre;
static float g_radius = 64, g_period = 8;
static double g_t0 = 0;
static int g_logTicks = 0;

static double Now() {
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Scripted stand-in for Minecraft's PlayerState: a circle around the centre,
// facing along the direction of travel.
static void Target(double t, Vector &pos, Vector &vel, QAngle &ang) {
	double w = 2 * M_PI / g_period;
	double a = w * (t - g_t0);
	pos = Vector(g_centre.x + g_radius * std::cos(a), g_centre.y + g_radius * std::sin(a), g_centre.z);
	vel = Vector(-g_radius * w * std::sin(a), g_radius * w * std::cos(a), 0);
	ang = QAngle{10.0f, (float)std::fmod(a * 180 / M_PI + 90.0, 360.0), 0.0f};
}

// Minecraft mode installs a provider: the Minecraft player's position/velocity (Source units).
static bool (*g_provider)(Vector &pos, Vector &vel) = nullptr;
void PuppetSetProvider(bool (*fn)(Vector &pos, Vector &vel)) { g_provider = fn; }

// (bridge #28) The Minecraft player goes through portals in Minecraft (the mod's PlayerTransit; mctransit.cpp): the body is put
// where Minecraft's player is, folded through the pair like the camera, and Portal 2's own move never carries it across a portal's
// plane (McTransitGuardMove), so Portal 2 never transits the puppet itself. Removed: the body held in front of the plane for
// Portal 2's transit (HoldAtPortal), the forced transit and pc_mc_transit_carry.
void TestCheck(bool ok, const char *what, const char *detail);  // testassert.cpp
void McTransitGuardMove(const Vector &origin, Vector &vel, float dt);  // mctransit.cpp
void McTransitCarryHeld(const Vector &before, int i);                   // mctransit.cpp (bridge #66)
void McBodyTeleported(const Vector &from, const Vector &to, const Vector &vel);  // mc.cpp (bridge #36)
void McMechanicsBodyMove(const Vector &velIn, const Vector &lastSet);  // mcmechanics.cpp (bridge #37)
void McMechanicsAfterMove(void *player, const Vector &velIn, const Vector &velOut);  // mcmechanics.cpp (bridge #37)

bool McModeActive();
static int EnteredPortal(const Vector &a, const Vector &b);
// (bridge #70) where the CLIENT's predicted body was put by the last client move, and when: Portal 2 computes its view (including a point_viewproxy
// redirect) from that body, so mc.cpp's redirect is measured against it, not against the server body (which is up to a tick out of phase: the
// jitter while walking).
static Vector g_clientBody(0, 0, 0);
static double g_clientBodyAt = -1;
bool PuppetClientBody(Vector &feet, double &ageSec) {
	if (g_clientBodyAt < 0) return false;
	feet = g_clientBody, ageSec = Now() - g_clientBodyAt;
	return true;
}
static void Apply(CMoveData *move, const char *side) {
	Vector pos, vel;
	QAngle ang;
	if (g_provider) {
		if (!g_provider(pos, vel)) return;
		if (side[0] == 'c') g_clientBody = pos, g_clientBodyAt = Now();
		// (bridge #66) the body jumps through a pair (Minecraft transited its player): the held object goes through the same pair now, before Portal 2's grab
		// (which runs after this move) sees the player at the exit and pulls the object there through the world
		if (side[0] == 's' && McModeActive() && (pos - move->m_vecAbsOrigin).Length() > 100.0f) McTransitCarryHeld(move->m_vecAbsOrigin, EnteredPortal(move->m_vecAbsOrigin, pos));
		move->m_vecAbsOrigin = pos;
		move->m_vecVelocity = vel;
		return;
	}
	Target(Now(), pos, vel, ang);
	move->m_vecAbsOrigin = pos;
	move->m_vecVelocity = vel;
	if (g_logTicks > 0 && side[0] == 's') {
		--g_logTicks;
		console->Print("[bridge] puppet %s origin=(%.1f %.1f %.1f)\n", side, pos.x, pos.y, pos.z);
	}
}

static Vector g_lastServerOrigin, g_lastServerVel;
static unsigned g_serverMoves = 0;  // (bridge #25) PuppetServerMoves
// ---- pcb_body_watch: where the Minecraft player puts the body vs where Portal 2's own player would stand ----------------
// Each server tick in Minecraft mode: the body's core in Portal 2's solid (walls, steps), or the feet more than 6 u above
// or below Portal 2's ground under them while Minecraft says it stands (a missing or extra piece of the export).
bool McModeActive();
void TestCheck(bool ok, const char *what, const char *detail);  // testassert.cpp
static bool g_bodyWatchNoclip = false;
static bool g_bodyWatch = false;
static int g_bwTicks = 0, g_bwSolid = 0, g_bwFloat = 0, g_bwSunk = 0, g_bwLogged = 0;
bool McOnGround();  // mc.cpp
static void BodyWatch(const Vector &o, void *player) {
	if (!g_bodyWatch || !McModeActive() || g_bodyWatchNoclip) return;
	++g_bwTicks;
	struct NotPlayer : public ITraceFilter {
		void *player;
		virtual bool ShouldHitEntity(void *e, int) { return e != player; }
		virtual TraceType_t GetTraceType() const { return TRACE_EVERYTHING; }
	} f;
	f.player = player;
	auto trace = [&](const Vector &a, const Vector &b, const Vector &h, CGameTrace &tr) {
		Ray_t ray;
		ray.m_Delta = VectorAligned(b.x - a.x, b.y - a.y, b.z - a.z);
		ray.m_IsSwept = (b - a).Length() > 0;
		ray.m_Extents = VectorAligned(h.x, h.y, h.z);
		ray.m_IsRay = h.Length() < 1e-3f;
		ray.m_StartOffset = VectorAligned(0, 0, 0);
		ray.m_Start = VectorAligned(a.x, a.y, a.z);
		ray.m_pWorldAxisTransform = nullptr;
		std::memset(&tr, 0, sizeof tr);
		engine.TraceRay(engine.traceServer->ThisPtr(), ray, MASK_PLAYERSOLID, &f, &tr);
	};
	CGameTrace core, down;
	trace(o + Vector(0, 0, 40), o + Vector(0, 0, 40), Vector(6, 6, 30), core);
	// Portal 2's ground under the feet: a 24 u wide box down from 40 u above (steps up to 18 u count as ground)
	trace(o + Vector(0, 0, 40), o - Vector(0, 0, 64), Vector(12, 12, 1), down);
	const float ground = down.fraction < 1.0f && !down.startsolid ? down.endpos.z - 1.0f : -1e9f;
	const char *bad = nullptr;
	if (core.startsolid) ++g_bwSolid, bad = "core in solid";
	else if (McOnGround() && ground > -1e8f && o.z > ground + 6.0f) ++g_bwFloat, bad = "standing above Portal 2's ground";
	else if (ground > -1e8f && o.z < ground - 6.0f) ++g_bwSunk, bad = "below Portal 2's ground";
	if (bad && g_bwLogged < 20) {
		++g_bwLogged;
		console->Print("[bridge] body watch: %s at (%.1f %.1f %.1f), Portal 2 ground %.1f\n", bad, o.x, o.y, o.z, ground);
	}
}
CON_COMMAND(pcb_body_watch, "pcb_body_watch <1|0> | assert <max_bad_ticks> - the Minecraft player's body against Portal 2's own collision, per tick\n") {
	if (args.ArgC() > 1 && !std::strcmp(args[1], "assert")) {
		const int mx = args.ArgC() > 2 ? std::atoi(args[2]) : 0;
		char buf[200];
		std::snprintf(buf, sizeof buf, "%d ticks watched: core in solid %d, standing above Portal 2's ground %d, below it %d (want <= %d bad ticks)", g_bwTicks, g_bwSolid, g_bwFloat, g_bwSunk, mx);
		TestCheck(g_bwTicks > 0 && g_bwSolid + g_bwFloat + g_bwSunk <= mx, "body on Portal 2's collision", buf);
		return;
	}
	g_bodyWatch = args.ArgC() > 1 && std::atoi(args[1]) != 0;
	g_bwTicks = g_bwSolid = g_bwFloat = g_bwSunk = g_bwLogged = 0;
}
bool McModeActive();                                      // mc.cpp
static int g_trackTicks = 0, g_trackEvery = 3;
static bool g_apexOn = false;
static float g_apexStart = 0, g_apexMax = 0, g_apexBest = 0;
CON_COMMAND(pcb_apex_begin, "pcb_apex_begin - start recording the highest player z (jump tests)\n") {
	g_apexOn = true;
	g_apexStart = g_apexMax = g_lastServerOrigin.z;
}
// best rise over the attempts so far (a user moving during the run can spoil one attempt)
float PuppetApexGain() {
	if (g_apexMax - g_apexStart > g_apexBest) g_apexBest = g_apexMax - g_apexStart;
	return g_apexBest;
}
CON_COMMAND(pcb_apex_note, "pcb_apex_note - fold the current attempt into the best jump rise\n") {
	float g = PuppetApexGain();
	console->Print("[bridge] jump attempt rose %.1f u (best %.1f)\n", g_apexMax - g_apexStart, g);
}

CON_COMMAND(pcb_track, "pcb_track <ticks> [every=3] - log the player's origin and velocity every 3rd movement tick, and every teleport\n") {
	g_trackTicks = args.ArgC() > 1 ? std::atoi(args[1]) : 300;
	g_trackEvery = args.ArgC() > 2 ? std::max(1, std::atoi(args[2])) : 3;
}
static int g_teleports = 0;
static Vector g_lastTeleportTo;
void TestCheck(bool ok, const char *what, const char *detail);  // testassert.cpp
CON_COMMAND(pcb_assert_teleports, "pcb_assert_teleports [min=1] | reset - the player teleported (a portal transit or setpos) at least min times since the reset\n") {
	if (args.ArgC() > 1 && args[1][0] == 'r') return void(g_teleports = 0);
	int mn = args.ArgC() > 1 ? std::atoi(args[1]) : 1;
	char buf[160];
	std::snprintf(buf, sizeof buf, "%d teleports since the reset, last to (%.1f %.1f %.1f)", g_teleports, g_lastTeleportTo.x, g_lastTeleportTo.y, g_lastTeleportTo.z);
	TestCheck(g_teleports >= mn, "teleported", buf);
}
static int g_lastButtons = -1;
static float g_lastFwd = 0, g_lastSide = 0;
bool g_logInput = false;  // on during scripted runs (pcb_script)

int EntMoveType(void *ent);  // ents.cpp
static bool g_noclip = false;
bool PlayerNoclip() { return g_noclip; }  // the player's movetype is MOVETYPE_NOCLIP (last server move)

// pcb_launch_to / pcb_stop: the velocity goes into the next move on both sides (the server's move
// alone was undone by the client's prediction: the launch never showed)
static bool g_launchPending = false, g_launchPendingClient = false;
static Vector g_launchVel;
void PuppetLaunch(const Vector &vel) { g_launchVel = vel, g_launchPending = true, g_launchPendingClient = false; }

// Portal transits (not setpos): the origin jumped from near one portal of the pair to near the other,
// inside a move or between two moves. And "stuck in a block": with block collision in force, the hull
// penetrates a Minecraft block (more than the 0.5 u contact skin) outside an open portal's hole.
bool BlockHullStuck(const Vector &origin);  // blockcollide.cpp
float BehindPortalDepth(const Vector &origin);  // blockcollide.cpp
static float g_behindMax = 0, g_behindNow = 0;
static Vector g_behindAt;
static int g_behindLog = 0;
static int g_transits = 0, g_transitLog = 0, g_stuckTicks = 0, g_stuckRun = 0, g_stuckRunMax = 0, g_stuckLog = 0;
static Vector g_stuckAt, g_prevTickOrigin;
static float g_lastTransitSpeed = 0, g_lastTransitEntry = 0;
static Vector g_prevTickVel;
static int g_lastTransitFrom = -1;
// (bridge #22) a jump of the hull from a to b through the linked pair: the portal entered (-1 none). Of the two assignments the
// one that fits best: two portals within 150 u of each other (the pit's floor and wall portals) passed the old test both ways.
static int g_p2PuppetTransits = 0;  // (bridge #28) Portal 2's own transits of the Minecraft player's puppet (a bug)
static int EnteredPortal(const Vector &a, const Vector &b) {
	int entered = -1;
	float best = 300.0f;
	for (int i = 0; i < kPortalSlots; ++i) {  // (bridge #32: every linked slot; the partner is i ^ 1)
		if (!PortalLinked(i)) continue;
		const float da = (a + Vector(0, 0, 36) - g_portalPos[i]).Length(), db = (b + Vector(0, 0, 36) - g_portalPos[i ^ 1]).Length();
		if (da < 150.0f && db < 150.0f && da + db < best) best = da + db, entered = i;
	}
	return entered;
}
static void PuppetTransitAndStuck(const Vector &before, CMoveData *move) {
	const Vector &after = move->m_vecAbsOrigin;
	for (int pass = 0; pass < 2; ++pass) {
		const Vector a = pass ? before : g_prevTickOrigin, b = pass ? after : before;
		if (a.Length() < 0.1f || (b - a).Length() < 100.0f) continue;
		// (the hull centre near the openings: within 2 hull lengths)
		for (int i = EnteredPortal(a, b); i >= 0; i = -1) {
			const int j = i ^ 1;
			{
				++g_transits, g_lastTransitFrom = i, g_lastTransitSpeed = move->m_vecVelocity.Length(), g_lastTransitEntry = g_prevTickVel.Length();
				if (g_transitLog < 400)
					++g_transitLog, console->Print("[bridge] transit %d: portal %d -> %d, (%.1f %.1f %.1f) -> (%.1f %.1f %.1f), speed %.0f in, %.0f out\n", g_transits, i, j, a.x, a.y, a.z, b.x, b.y,
					                               b.z, g_lastTransitEntry, g_lastTransitSpeed);
				break;
			}
		}
	}
	g_prevTickOrigin = after;
	g_prevTickVel = move->m_vecVelocity;
	if (!g_noclip) {
		const float bd = BehindPortalDepth(after);
		g_behindNow = bd;
		if (bd > g_behindMax) g_behindMax = bd, g_behindAt = after;
		if (bd > 48.0f && g_behindLog < 20)
			++g_behindLog, console->Print("[bridge] BEHIND PORTAL: Chell's centre %.1f u behind an open portal's plane at (%.1f %.1f %.1f), velocity (%.0f %.0f %.0f)\n", bd, after.x, after.y, after.z,
			                              move->m_vecVelocity.x, move->m_vecVelocity.y, move->m_vecVelocity.z);
	}
	if (!g_noclip && BlockHullStuck(after)) {
		++g_stuckTicks, ++g_stuckRun, g_stuckAt = after;
		g_stuckRunMax = std::max(g_stuckRunMax, g_stuckRun);
		if (g_stuckLog < 40)
			++g_stuckLog, console->Print("[bridge] STUCK in a block at (%.1f %.1f %.1f) velocity (%.0f %.0f %.0f), %d ticks in a row\n", after.x, after.y, after.z, move->m_vecVelocity.x,
			                             move->m_vecVelocity.y, move->m_vecVelocity.z, g_stuckRun);
	} else {
		g_stuckRun = 0;
	}
}

// Never stuck in Minecraft blocks: Source nudges a player out of brushes it starts inside of; blocks get
// the same. When the hull ends a move inside blocks (a setpos or teleport onto a step, a block placed or
// streamed in around Chell), she is moved to the nearest free spot: up to a block up, or up to a block
// aside, smallest move first (up preferred), free of blocks and of Portal 2's world.
static bool g_unstick = true;  // pcb_unstick
static int g_unstuck = 0;
// Portal 2's own exit check, for blocks (bridge #13, research/specs/portal-transit.md 1.5): after a transit it
// traces the hull at the exit and, if it starts solid, retries with the centre projected onto the exit portal's
// axis. Its trace does not see Minecraft blocks, so a hull put out near the rim of an opening on blocks stayed
// there: the user's session (bridge.log ~230900): transit 26 into a floor portal 44.6 u off its centre, out of the
// other floor portal 44 u off centre and 26 u behind its plane at 750 u/s up; the 32 u hull reached past the
// 56 u half-length of the opening into the block rim, the rise stopped against it and she sank down the hole behind
// the portal, with no transit due (she was moving away from the plane). Same rule here: when this move was a transit
// and the hull at the exit overlaps blocks or is blocked within 36 u out along the exit's normal, the centre goes
// onto the exit's axis (its distance from the plane and the velocity kept), if that spot is free.
bool BlockHullBlocked(const Vector &origin, const Vector &d);  // blockcollide.cpp
static bool g_exitRecentre = true;  // pcb_exit_recentre
static int g_exitRecentred = 0;
static void ExitRecentre(const Vector &before, CMoveData *move) {
	if (!g_exitRecentre) return;
	const Vector o = move->m_vecAbsOrigin;
	if ((o - before).Length() < 100.0f) return;
	for (int i = EnteredPortal(before, o); i >= 0; i = -1) {
		const int j = i ^ 1;
		const Vector bc = o + Vector(0, 0, 36);
		const Vector n = g_portalNormal[j];
		if (!BlockHullBlocked(o, n * 36.0f)) return;
		const float along = Vector::DotProduct(bc - g_portalPos[j], n);
		const Vector p = g_portalPos[j] + n * along - Vector(0, 0, 36);
		const bool free = !BlockHullBlocked(p, n * 36.0f);
		++g_exitRecentred;
		if (g_exitRecentred <= 40)
			console->Print("[bridge] EXIT RECENTRE portal %d: hull at (%.1f %.1f %.1f) %.1f u off the exit's axis, %.1f u from its plane, blocked by blocks; %s\n", j, o.x, o.y, o.z,
			               (bc - (g_portalPos[j] + n * along)).Length(), along, free ? "moved onto the axis" : "the axis is blocked too: left");
		if (free) move->m_vecAbsOrigin = p;
		return;
	}
}
CON_COMMAND(pcb_exit_recentre, "pcb_exit_recentre [0|1] - after a transit, a hull put out against Minecraft blocks moves onto the exit portal's axis (Portal 2's own exit check; default 1)\n") {
	if (args.ArgC() > 1) g_exitRecentre = std::atoi(args[1]) != 0;
	console->Print("[bridge] exit recentre %s, %d so far\n", g_exitRecentre ? "on" : "off", g_exitRecentred);
}
int BehindPortalWhich(const Vector &origin, float &depth);  // blockcollide.cpp
static int g_unstuckPortal = 0;
static bool PortalHoleRescue(CMoveData *move, const char *why);
static bool UnstickNearest(CMoveData *move, const Vector &o);
static bool g_unstickPortal = true;  // pcb_unstick_portal
CON_COMMAND(pcb_unstick_portal, "pcb_unstick_portal [0|1] - stuck behind an open portal's plane over its opening: back out in front of it (default 1)\n") {
	if (args.ArgC() > 1) g_unstickPortal = std::atoi(args[1]) != 0;
	console->Print("[bridge] unstick out of portal holes %s, %d so far\n", g_unstickPortal ? "on" : "off", g_unstuckPortal);
}
static bool Unstick(CMoveData *move) {
	const Vector o = move->m_vecAbsOrigin;
	if (!BlockHullStuck(o)) return false;
	// Stuck behind an open portal's plane, over its opening (in the hole carved out of the blocks, with no transit:
	// the user's session, bridge #12): back out in front of that portal, the velocity into it removed. Nudging her
	// aside inside the hole column let her fall on through the rock.
	if (PortalHoleRescue(move, "stuck")) return true;
	return UnstickNearest(move, o);
}
// Behind an open portal's plane over its opening for more than a few ticks (Portal 2 teleports Chell the tick her
// centre crosses the plane: 36 u + a tick's travel at most), or stuck there: out in front of that portal, the
// velocity into it removed. The hole carved for her behind a portal is a shaft into the blocks; without a transit
// she fell down it (the user's session) or, with the shaft cut to 128 u, stood at its bottom, trapped.
static int g_behindTicks = 0;
static bool PortalHoleRescue(CMoveData *move, const char *why) {
	const Vector o = move->m_vecAbsOrigin;
	float depth = 0;
	const int pi = BehindPortalWhich(o, depth);
	if (g_unstickPortal && pi >= 0 && depth > 40.0f) {
		const Vector n = g_portalNormal[pi];
		const float reach = 16 * std::fabs(n.x) + 16 * std::fabs(n.y) + 36 * std::fabs(n.z);  // the hull's half-extent along n
		Vector p = o + n * (depth + reach + 1.0f);
		if (!BlockHullStuck(p)) {
			move->m_vecAbsOrigin = p;
			const float vn = Vector::DotProduct(move->m_vecVelocity, n);
			if (vn < 0) move->m_vecVelocity = move->m_vecVelocity - n * vn;
			++g_unstuck, ++g_unstuckPortal;
			if (g_unstuckPortal <= 20)
				console->Print("[bridge] UNSTUCK out of portal %d's hole (%s): centre was %.1f u behind its plane at (%.1f %.1f %.1f), moved %.1f u out along its normal\n", pi, why, depth, o.x, o.y,
				               o.z, depth + reach + 1.0f);
			g_behindTicks = 0;
			return true;
		}
	}
	return false;
}
static bool UnstickNearest(CMoveData *move, const Vector &o) {
	struct Cand {
		Vector d;
		float cost;
	};
	std::vector<Cand> cands;
	static const float steps[] = {1, 2, 4, 8, 12, 16, 20, 24, 28, 32, 36, 41};
	for (float dz : steps) cands.push_back({Vector(0, 0, dz), dz * 0.9f});
	for (int k = 0; k < 8; ++k) {
		float a = k * (float)M_PI / 4;
		for (float r : {4.0f, 8.0f, 12.0f, 16.0f, 20.0f, 24.0f, 32.0f, 41.0f})
			for (float dz : {0.0f, 1.0f})
				cands.push_back({Vector(std::cos(a) * r, std::sin(a) * r, dz), r + dz});
	}
	std::sort(cands.begin(), cands.end(), [](const Cand &x, const Cand &y) { return x.cost < y.cost; });
	for (const Cand &c : cands) {
		Vector p = o + c.d;
		if (BlockHullStuck(p)) continue;
		CGameTrace tr;
		engine.Trace(p + Vector(0, 0, 36), p + Vector(0, 0, 36), Vector(16, 16, 36), MASK_PLAYERSOLID, tr, true);
		if (tr.startsolid) continue;
		move->m_vecAbsOrigin = p;
		if (move->m_vecVelocity.z < 0 && c.d.z > 0) move->m_vecVelocity.z = 0;
		++g_unstuck;
		if (g_unstuck <= 40)
			console->Print("[bridge] UNSTUCK from blocks at (%.1f %.1f %.1f): moved (%.1f %.1f %.1f)\n", o.x, o.y, o.z, c.d.x, c.d.y, c.d.z);
		return true;
	}
	return false;
}
CON_COMMAND(pcb_unstick, "pcb_unstick [0|1] - move Chell out of Minecraft blocks she ends a move inside of (default 1)\n") {
	if (args.ArgC() > 1) g_unstick = std::atoi(args[1]) != 0;
	console->Print("[bridge] unstick %s, %d moves so far\n", g_unstick ? "on" : "off", g_unstuck);
}

void BlockWatchTickBegin();  // blockcollide.cpp
void ChellShadowFrame(void *player, const Vector &origin, bool noclip);  // blockcollide.cpp (bridge #44)
void BlockWatchTickEnd(const Vector &velIn, CMoveData *mv, const Vector &before, const Vector &lastOut, bool noclip);
// (bridge #70) the server player's eye height above its origin (64 standing, 28 ducked, easing between) and its flags (FL_ONGROUND 1, FL_DUCKING 2):
// the redirect in mc.cpp needs Portal 2's own eye, and the jump / duck relay checks that Portal 2 took the buttons. -1: not in the datamap.
int EntFieldOffset(void *ent, const char *field);  // ents.cpp
static float g_svViewOffZ = -1;
static int g_svFlags = -1;
static void ReadPlayerState(void *player) {
	static int offVO = -2, offFl = -2;
	if (!player) return;
	if (offVO == -2) {
		offVO = EntFieldOffset(player, "m_vecViewOffset"), offFl = EntFieldOffset(player, "m_fFlags");
		console->Print("[bridge] puppet: player m_vecViewOffset at +%d, m_fFlags at +%d in the server datamap\n", offVO, offFl);
	}
	if (offVO >= 0) {
		const float z = *(const float *)((const char *)player + offVO + 8);
		g_svViewOffZ = z > 8.0f && z < 100.0f ? z : -1.0f;
	}
	if (offFl >= 0) g_svFlags = *(const int *)((const char *)player + offFl);
}
float PuppetViewOffsetZ() { return g_svViewOffZ; }
int PuppetPlayerFlags() { return g_svFlags; }
static int __rescall ServerProcessMovement_Hook(void *thisptr, void *player, CMoveData *move) {
	PROF("svmove");  // (bridge #25: Portal 2's player move with our hooks in it)
	g_noclip = player && EntMoveType(player) == MOVETYPE_NOCLIP;
	// Log input changes, so a test run shows whether anyone touched the controls.
	if (g_logInput && (move->m_nButtons != g_lastButtons || move->m_flForwardMove != g_lastFwd || move->m_flSideMove != g_lastSide)) {
		console->Print("[bridge] input buttons 0x%x fwd %.0f side %.0f at (%.1f %.1f %.1f)\n", move->m_nButtons, move->m_flForwardMove, move->m_flSideMove,
		               move->m_vecAbsOrigin.x, move->m_vecAbsOrigin.y, move->m_vecAbsOrigin.z);
		g_lastButtons = move->m_nButtons;
		g_lastFwd = move->m_flForwardMove;
		g_lastSide = move->m_flSideMove;
	}
	Vector before = move->m_vecAbsOrigin;
	const Vector velIn = move->m_vecVelocity;
	if (g_provider && McModeActive() && !g_noclip && g_serverMoves > 0) McMechanicsBodyMove(velIn, g_lastServerVel);  // (bridge #37)
	// (bridge #36) the body was moved between two of our moves by something that is not the puppet (setpos, point_teleport, trigger_teleport, SnapPlayer, a
	// level script): that teleport wins. The Minecraft player is sent to the body and the puppet leaves it alone until Minecraft is there; before, the
	// next puppet write put the body back at Minecraft's old spot (a placement "never happened"; a map trigger that teleports the player did nothing).
	if (g_provider && g_lastServerOrigin.Length() > 0.1f && (before - g_lastServerOrigin).Length() > 40.0f) McBodyTeleported(g_lastServerOrigin, before, velIn);
	// (bridge #26: a Portal 2 pusher moving the body is no longer a takeover: Minecraft rides and is pushed by the movers itself)
	BlockWatchTickBegin();
	// (bridge #28) the Minecraft player: Portal 2's own move never carries the body into a portal (Minecraft transits it)
	if (g_provider && McModeActive() && !g_noclip) McTransitGuardMove(move->m_vecAbsOrigin, move->m_vecVelocity, 1.0f / 60.0f);
	const Vector guardedVel = move->m_vecVelocity;
	if (!McModeActive()) ChellShadowFrame(player, move->m_vecAbsOrigin, g_noclip);  // (bridge #44) Chell's physics shadow must not push her out of a dug hole (blockcollide.cpp)
	int r = ServerProcessMovement(thisptr, player, move);
	BlockWatchTickEnd(velIn, move, before, g_lastServerOrigin, g_noclip);
	if (g_provider && McModeActive() && !g_noclip) McMechanicsAfterMove(player, velIn, move->m_vecVelocity);  // (bridge #37) the body's paint powers, and a bounce Portal 2 itself gave it
	// (bridge #28) Portal 2 must never transit the puppet itself: log it if its own move did (a test asserts 0)
	if (g_provider && McModeActive() && !g_noclip && (move->m_vecAbsOrigin - before).Length() > 100.0f && EnteredPortal(before, move->m_vecAbsOrigin) >= 0) {
		// (bridge #69) while Portal 2 carries the body (a sequence's teleport, the hold after it) Minecraft is not leading: the transit is Portal 2's own and Minecraft follows by the teleport
		if (!McLeads()) {
			console->Print("[bridge] mode: Portal 2 transited the body (Minecraft was not leading): (%.0f %.0f %.0f) -> (%.0f %.0f %.0f)\n", before.x, before.y, before.z, move->m_vecAbsOrigin.x, move->m_vecAbsOrigin.y, move->m_vecAbsOrigin.z);
		} else {
		++g_p2PuppetTransits;
		if (g_p2PuppetTransits <= 20)
			console->Print("[bridge] BUG: Portal 2 transited the puppet itself (server): (%.1f %.1f %.1f) -> (%.1f %.1f %.1f), velocity in (%.0f %.0f %.0f) after the guard (%.0f %.0f %.0f)\n",
			               before.x, before.y, before.z, move->m_vecAbsOrigin.x, move->m_vecAbsOrigin.y, move->m_vecAbsOrigin.z, velIn.x, velIn.y, velIn.z, guardedVel.x, guardedVel.y,
			               guardedVel.z);
		}
	}
	if (g_active || g_provider) Apply(move, "server");
	// pcb_launch_to / pcb_stop: written after the game's own move (which reads the player's velocity
	// itself), once noclip is off (a noclip move replaces it with the input's)
	if (g_launchPending && !g_noclip) {
		g_launchPending = false, g_launchPendingClient = true;
		move->m_vecVelocity = g_launchVel;
		console->Print("[bridge] launch applied at (%.1f %.1f %.1f): velocity (%.0f %.0f %.0f)\n", move->m_vecAbsOrigin.x, move->m_vecAbsOrigin.y, move->m_vecAbsOrigin.z, g_launchVel.x,
		               g_launchVel.y, g_launchVel.z);
	}
	// pcb_track: position log for transit tests. A jump of the origin between two movement
	// ticks (a portal teleport) is always logged while tracking.
	if (g_trackTicks > 0) {
		--g_trackTicks;
		bool jump = (before - g_lastServerOrigin).Length() > 64.0f;
		if (jump || (g_trackTicks % g_trackEvery) == 0)
			console->Print("[bridge] track%s origin (%.1f %.1f %.1f) velocity (%.1f %.1f %.1f) speed %.0f\n", jump ? " TELEPORT" : "", move->m_vecAbsOrigin.x,
			               move->m_vecAbsOrigin.y, move->m_vecAbsOrigin.z, move->m_vecVelocity.x, move->m_vecVelocity.y, move->m_vecVelocity.z, move->m_vecVelocity.Length());
	}
	if (g_apexOn && move->m_vecAbsOrigin.z > g_apexMax) g_apexMax = move->m_vecAbsOrigin.z;
	// a teleport between ticks (setpos) or inside this move (a portal transit)
	if ((before - g_lastServerOrigin).Length() > 64.0f && g_lastServerOrigin.Length() > 0.1f) ++g_teleports, g_lastTeleportTo = before;
	else if ((move->m_vecAbsOrigin - before).Length() > 100.0f) ++g_teleports, g_lastTeleportTo = move->m_vecAbsOrigin;
	// (Portal 2 teleports inside the move or between two moves: the jump is from the last move's end, or within this one)
	if (!g_noclip && !McModeActive()) ExitRecentre(g_lastServerOrigin.Length() > 0.1f && (before - g_lastServerOrigin).Length() > 100.0f ? g_lastServerOrigin : before, move);
	// (not while the Minecraft player leads: Minecraft's own collision is the truth, and its player is narrower than
	// Chell's hull, so the block hook found her "stuck" beside every wall it walked along: bridge #13)
	if (g_unstick && !g_noclip && !McModeActive() && !Unstick(move)) {
		float depth = 0;
		g_behindTicks = BehindPortalWhich(move->m_vecAbsOrigin, depth) >= 0 && depth > 48.0f ? g_behindTicks + 1 : 0;
		if (g_behindTicks >= 4) PortalHoleRescue(move, "behind its plane for 4 ticks");
	}
	PuppetTransitAndStuck(before, move);
	g_bodyWatchNoclip = g_noclip;
	BodyWatch(move->m_vecAbsOrigin, player);
	ReadPlayerState(player);
	g_lastServerOrigin = move->m_vecAbsOrigin;
	++g_serverMoves;
	g_lastServerVel = move->m_vecVelocity;
	return r;
}

Vector PlayerOrigin() { return g_lastServerOrigin; }
// (bridge #25) server movement ticks so far: a level's body is known once one has run since its start (mc.cpp)
unsigned PuppetServerMoves() { return g_serverMoves; }
// Floating origin: the shift moved the player by d; until her next move this is where she is.
void PuppetShiftOrigin(const Vector &d) { g_lastServerOrigin += d; }
Vector PlayerVelocity() { return g_lastServerVel; }

// Where the server's movement code last put the player (feet origin).
CON_COMMAND(pcb_where, "pcb_where - print the player's server-side origin and velocity after the last movement tick\n") {
	console->Print("[bridge] player origin (%.2f %.2f %.2f) velocity (%.1f %.1f %.1f)\n", g_lastServerOrigin.x, g_lastServerOrigin.y, g_lastServerOrigin.z,
	               g_lastServerVel.x, g_lastServerVel.y, g_lastServerVel.z);
}
static int __rescall ClientProcessMovement_Hook(void *thisptr, void *player, CMoveData *move) {
	PROF("clmove");  // (bridge #25)
	if (g_provider && McModeActive()) McTransitGuardMove(move->m_vecAbsOrigin, move->m_vecVelocity, 1.0f / 60.0f);  // (bridge #28)
	const Vector clBefore = move->m_vecAbsOrigin;
	int r = ClientProcessMovement(thisptr, player, move);
	if (g_provider && McModeActive() && (move->m_vecAbsOrigin - clBefore).Length() > 100.0f && EnteredPortal(clBefore, move->m_vecAbsOrigin) >= 0) {
		++g_p2PuppetTransits;
		if (g_p2PuppetTransits <= 20)
			console->Print("[bridge] BUG: Portal 2 transited the puppet itself (client prediction): (%.1f %.1f %.1f) -> (%.1f %.1f %.1f)\n", clBefore.x, clBefore.y, clBefore.z,
			               move->m_vecAbsOrigin.x, move->m_vecAbsOrigin.y, move->m_vecAbsOrigin.z);
	}
	if (g_launchPendingClient) g_launchPendingClient = false, move->m_vecVelocity = g_launchVel;
	if (g_active || g_provider) Apply(move, "client");
	return r;
}

bool PuppetInit() {
	g_serverMovement = Interface::Create(MODULE_NAME("server"), "GameMovement001");
	g_clientMovement = Interface::Create(MODULE_NAME("client"), "GameMovement001");
	if (g_serverMovement) g_serverMovement->Hook(ServerProcessMovement_Hook, ServerProcessMovement, Offsets::ProcessMovement);
	if (g_clientMovement) g_clientMovement->Hook(ClientProcessMovement_Hook, ClientProcessMovement, Offsets::ProcessMovement);
	return g_serverMovement && g_clientMovement;
}

void PuppetShutdown() {
	g_active = false;
	Interface::Delete(g_serverMovement);
	Interface::Delete(g_clientMovement);
	g_serverMovement = g_clientMovement = nullptr;
}

void PuppetStart(const Vector &centre, float radius, float period) {
	g_centre = centre;
	g_radius = radius;
	g_period = period > 0.5f ? period : 0.5f;
	g_t0 = Now();
	g_active = true;
	g_logTicks = 5;
	console->Print("[bridge] puppet on: centre (%.1f %.1f %.1f) r=%.1f period=%.1fs\n", centre.x, centre.y, centre.z, radius, g_period);
}

void PuppetStop() {
	g_active = false;
	console->Print("[bridge] puppet off\n");
}

void PuppetFrame() {
	if (!g_active) return;
	Vector pos, vel;
	QAngle ang;
	Target(Now(), pos, vel, ang);
	engine.SetAngles(ang);
}

}  // namespace bridge

namespace bridge {
int McTransits();  // mctransit.cpp
static int g_mcTransitBase = 0;
// (bridge #28) a transit of the Minecraft player is Minecraft's own; the body's jump is seen here too, but held off the planes
// it may not pass the jump test (waituntil transit counts either)
int PuppetTransits() { return std::max(g_transits, McTransits() - g_mcTransitBase); }
CON_COMMAND(pcb_assert_transits, "pcb_assert_transits [min=1] | reset - portal transits (origin carried from one portal of the pair to the other, not setpos) since the reset\n") {
	if (args.ArgC() > 1 && args[1][0] == 'r') return void((g_transits = 0, g_mcTransitBase = McTransits()));
	int mn = args.ArgC() > 1 ? std::atoi(args[1]) : 1;
	char buf[160];
	std::snprintf(buf, sizeof buf, "%d portal transits since the reset (want >= %d), last from portal %d at %.0f u/s", g_transits, mn, g_lastTransitFrom, g_lastTransitSpeed);
	TestCheck(g_transits >= mn, "portal transits", buf);
}
CON_COMMAND(pcb_assert_transit_speed, "pcb_assert_transit_speed [tol_pct=5] [min_in=0] - the last portal transit kept its speed (out within tol% of in)\n") {
	float tol = args.ArgC() > 1 ? (float)std::atof(args[1]) : 5.0f, minIn = args.ArgC() > 2 ? (float)std::atof(args[2]) : 0.0f;
	char buf[200];
	float r = g_lastTransitEntry > 1 ? g_lastTransitSpeed / g_lastTransitEntry : 0;
	std::snprintf(buf, sizeof buf, "last transit from portal %d: %.0f u/s in, %.0f u/s out (%.1f%%), limit %.0f%%, min entry %.0f", g_lastTransitFrom, g_lastTransitEntry, g_lastTransitSpeed, 100 * r, tol, minIn);
	// (Portal 2's own exit rules: the speed out of a portal is capped at 1000 u/s)
	const float want = std::fmin(g_lastTransitEntry, 1000.0f);
	const float rr = want > 1 ? g_lastTransitSpeed / want : 0;
	TestCheck(g_transits > 0 && g_lastTransitEntry >= minIn && std::fabs(rr - 1) * 100 <= tol, "transit kept its speed", buf);
}
CON_COMMAND(pcb_assert_p2_puppet_transits, "pcb_assert_p2_puppet_transits [max=0] - (bridge #28) Portal 2 never transited the Minecraft player's puppet itself\n") {
	const int mx = args.ArgC() > 1 ? std::atoi(args[1]) : 0;
	char buf[120];
	std::snprintf(buf, sizeof buf, "%d of Portal 2's own transits of the puppet (want <= %d)", g_p2PuppetTransits, mx);
	TestCheck(g_p2PuppetTransits <= mx, "Portal 2 left the puppet's transits to Minecraft", buf);
}
CON_COMMAND(pcb_assert_behind_portal, "pcb_assert_behind_portal [max_u=48] | now | reset - Chell's centre never went further behind an open portal's plane (sinking in the hole carve without a transit)\n") {
	if (args.ArgC() > 1 && args[1][0] == 'r') {
		g_behindMax = 0, g_behindLog = 0;
		return;
	}
	char buf[200];
	if (args.ArgC() > 1 && args[1][0] == 'n') {  // now: not behind any portal this tick
		std::snprintf(buf, sizeof buf, "now %.1f u behind a portal plane (deepest since the reset %.1f)", g_behindNow, g_behindMax);
		return TestCheck(g_behindNow <= 0.5f, "not behind a portal now", buf);
	}
	const float mx = args.ArgC() > 1 ? (float)std::atof(args[1]) : 48.0f;
	std::snprintf(buf, sizeof buf, "deepest %.1f u behind a portal plane (limit %.0f) at (%.1f %.1f %.1f)", g_behindMax, mx, g_behindAt.x, g_behindAt.y, g_behindAt.z);
	TestCheck(g_behindMax <= mx, "never sunk behind a portal", buf);
}
CON_COMMAND(pcb_assert_stuck, "pcb_assert_stuck [max_run_ticks=0] | reset - Chell's hull never sat inside a Minecraft block (block collision on) since the reset\n") {
	if (args.ArgC() > 1 && args[1][0] == 'r') return void(g_stuckTicks = g_stuckRun = g_stuckRunMax = 0);
	int mx = args.ArgC() > 1 ? std::atoi(args[1]) : 0;
	char buf[200];
	std::snprintf(buf, sizeof buf, "%d ticks inside a block, longest run %d (limit %d), last at (%.1f %.1f %.1f)", g_stuckTicks, g_stuckRunMax, mx, g_stuckAt.x, g_stuckAt.y, g_stuckAt.z);
	TestCheck(g_stuckRunMax <= mx, "never stuck in a block", buf);
}
}  // namespace bridge
