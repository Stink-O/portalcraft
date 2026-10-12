// Portalcraft bridge (bridge #28): the Minecraft player through Portal 2's portals.
//
// Minecraft transits its own player (the mod's PlayerTransit, as its EntityTransit does for mobs): the tick its eye crosses a
// linked portal's plane inside the opening, the player's position of this tick and of the last one are carried through the pair
// (Immersive Portals' approach: qouteall, iPortalTeam/ImmersivePortalsMod, Apache-2.0), its velocity and look turned, Portal 2's exit
// speed rules applied, and McState reports the transit (transitSeq + McTransit: the pair transform). This file is the host's side:
//   * the look: Portal 2's view angles are turned by the pair once, on the frame the transit arrives (any roll is kept and levelled
//     the way Portal 2 levels Chell's view after a portal: exponential, half-life 0.125 s, at least 10 deg/s); SkyState.transitAck
//     tells the mod its look includes the turn (until then the mod turns the host's look itself);
//   * the fold: a point on the latest tick's line that is behind a linked portal's plane, where that line crossed the plane inside
//     the opening, is drawn through the pair. The camera (the eye) and the puppet body (its hull centre) both follow that rule, so
//     the frames of the transit tick still behind the exit (and an extrapolated frame past the entry before Minecraft's transit
//     arrives) are the same view as the other side: the camera is continuous at every frame, with no blend;
//   * Portal 2 never transits the puppet itself: the body is never behind a portal's plane (the fold), and Portal 2's own move
//     never carries it across one (McTransitGuardMove takes the velocity into a portal out of Portal 2's move; the puppet's real
//     pose and velocity are written after the move). The body jumps to the exit when Minecraft's player goes through, so turrets,
//     lasers and triggers see Portal 2's usual transit position.
// Replaced (bridge #20-#22): the body held in front of the plane for Portal 2's transit, the forced transit, the teleport back to
// Minecraft with softTeleport, TransitTarget, the view blend, the eye-through-portal and g_transitAwait.
#include "bridge.hpp"
#include "growth.hpp"
#include "coords.hpp"
#include "link.hpp"

#include "Modules/Tier1.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace bridge {

using namespace skycraft::proto;

void TestCheck(bool ok, const char *what, const char *detail);                          // testassert.cpp
void PortalThroughPose(int i, int j, const Vector &o, const QAngle &a, Vector &oo, QAngle &oa);  // composite.cpp
Vector PortalRightAxis(int i);                                                            // composite.cpp

static double NowS() { return link::NowNs() * 1e-9; }

// ---- basis <-> angles (Source: pitch down positive, yaw about +Z, roll about the forward axis) --------------------------------
static void Basis(const QAngle &a, Vector &f, Vector &r, Vector &u) {
	const float sp = std::sin(a.x * (float)M_PI / 180), cp = std::cos(a.x * (float)M_PI / 180);
	const float sy = std::sin(a.y * (float)M_PI / 180), cy = std::cos(a.y * (float)M_PI / 180);
	const float sr = std::sin(a.z * (float)M_PI / 180), cr = std::cos(a.z * (float)M_PI / 180);
	f = Vector(cp * cy, cp * sy, -sp);
	r = Vector(-sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp);
	u = Vector(cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp);
}
// Forward + up -> angles. Looking (almost) straight up or down the yaw comes from the up vector, so the roll is 0 there.
static QAngle Angles(const Vector &f, const Vector &u) {
	QAngle a;
	a.x = -std::asin(std::fmax(-1.0f, std::fmin(1.0f, f.z))) * 180.0f / (float)M_PI;
	float hx = f.x, hy = f.y;
	if (hx * hx + hy * hy < 1e-6f) {
		const float s = f.z > 0 ? -1.0f : 1.0f;  // looking up: the camera's up points back; looking down: forward
		hx = u.x * s, hy = u.y * s;
	}
	a.y = std::atan2(hy, hx) * 180.0f / (float)M_PI;
	a.z = 0;
	Vector f0, r0, u0;
	Basis(a, f0, r0, u0);
	a.z = std::atan2(Vector::DotProduct(u, r0), Vector::DotProduct(u, u0)) * 180.0f / (float)M_PI;
	return a;
}
// A Source direction turned by the MC quaternion q (MC (x, y, z) = Source (x, z, -y))
static Vector TurnSrc(const float q[4], const Vector &s) {
	const double vx = s.x, vy = s.z, vz = -s.y;
	const double x = q[0], y = q[1], z = q[2], w = q[3];
	const double cx = y * vz - z * vy, cy = z * vx - x * vz, cz = x * vy - y * vx;
	const double ccx = y * cz - z * cy, ccy = z * cx - x * cz, ccz = x * cy - y * cx;
	const double ox = vx + 2 * (w * cx + ccx), oy = vy + 2 * (w * cy + ccy), oz = vz + 2 * (w * cz + ccz);
	return Vector((float)ox, (float)-oz, (float)oy);
}

// ---- state ---------------------------------------------------------------------------------------------------------------
static bool g_seenInit = false;
static uint32_t g_seen = 0, g_ack = 0;
static int g_transits = 0, g_turnLogs = 0;
static float g_roll = 0;          // the view's roll left from a transit (deg), levelled like Portal 2's
static bool g_turnedNow = false;
static double g_testTurnAt = -10;  // a test script turned the view itself (pcb_setview, pcb_aim_mc): no bug
void McNoteScriptedTurn() { g_testTurnAt = link::NowNs() * 1e-9; }
static bool g_skipTurnCheck = false;  // the look was turned this frame (OverrideView's angles are from before it)
static double g_lastFrameT = 0;
static Vector g_lastVel, g_tickVel;  // Minecraft's velocity of the previous / latest tick (u/s)
static uint64_t g_velTick = 0;
static float g_lastIn = 0, g_lastOut = 0;  // speeds around the latest transit (u/s)
static int g_lastFrom = -1;
static int g_foldFrames = 0, g_bodyFolds = 0, g_guarded = 0;
static int g_debugTicks = 0;  // pcb_mc_transit_debug
static bool g_outPending = false;
static bool g_haveFirst = false;

static double g_foldSticky[2][kPortalSlots] = {};  // [0] the body, [1] the camera:  // (bridge #68) when slot k last folded a point by a real crossing: the fold holds until Minecraft's own transit arrives
bool McScriptedRecently();  // mc.cpp (bridge #63)
uint32_t McTransitAck() { return g_ack; }
int McTransits() { return g_transits; }

// Each frame, after McState was read (mc.cpp Frame). `active`: the Minecraft player is the player.
void McTransitFrame(const McState &mc, bool active) {
	g_turnedNow = false;
	// Minecraft's velocity per tick (the tick's displacement, in exit space after a transit: the mod carried the last tick through)
	if ((uint64_t)mc.tickQpc != g_velTick) {
		g_velTick = (uint64_t)mc.tickQpc;
		const double s = kUnitsPerBlock * 1000.0 / (mc.tickMs > 0 ? mc.tickMs : 50.0);
		g_lastVel = g_tickVel;
		g_tickVel = Vector((float)((mc.curX - mc.prevX) * s), (float)(-(mc.curZ - mc.prevZ) * s), (float)((mc.curY - mc.prevY) * s));
		// the speed out of a transit: the tick after it (the transit's own tick includes the exit's free-spot step)
		if (g_outPending && mc.transitSeq == g_seen) g_outPending = false, g_lastOut = g_tickVel.Length();
		if (g_debugTicks > 0) {
			--g_debugTicks;
			const Vector c = McToSrc(mc.curX, mc.curY, mc.curZ);
			console->Print("[bridge] mc transit debug: tick feet MC (%.3f %.3f %.3f) src (%.1f %.1f %.1f) vel (%.0f %.0f %.0f) seq %u; portals %d%d p0 (%.1f %.1f %.1f) n (%.2f %.2f %.2f) p1 (%.1f %.1f %.1f) n (%.2f %.2f %.2f)\n",
			               mc.curX, mc.curY, mc.curZ, c.x, c.y, c.z, g_tickVel.x, g_tickVel.y, g_tickVel.z, mc.transitSeq, g_portalSet[0], g_portalSet[1], g_portalPos[0].x, g_portalPos[0].y,
			               g_portalPos[0].z, g_portalNormal[0].x, g_portalNormal[0].y, g_portalNormal[0].z, g_portalPos[1].x, g_portalPos[1].y, g_portalPos[1].z, g_portalNormal[1].x,
			               g_portalNormal[1].y, g_portalNormal[1].z);
		}
	}
	// (diagnostics) the look turned by something other than a Minecraft transit (Portal 2 transiting the puppet would do that)
	{
		static Vector lastF;
		static bool have = false;
		Vector f, r, u;
		Basis(engine.ViewAngles(), f, r, u);
		// (bridge #63) a turn is judged 0.6 s later: Portal 2's own sequences turn the player (the wake-up camera ends facing the room, the cut to the Wheatley
		// room sets new angles a frame BEFORE the teleport and the camera are seen), so it is no bug if a sequence or a body teleport is seen meanwhile
		static double turnAt = 0, turnDeg = 0;
		static QAngle turnAng{};
		const double nowT = link::NowNs() * 1e-9;
		if (active && have && !g_skipTurnCheck && Vector::DotProduct(f, lastF) < 0.7f && mc.transitSeq == g_seen && turnAt == 0) {
			turnAt = nowT, turnDeg = std::acos(std::fmax(-1.0f, Vector::DotProduct(f, lastF))) * 180.0f / (float)M_PI, turnAng = engine.ViewAngles();
			if (McScriptedRecently() || nowT - g_testTurnAt < 1.0) turnAt = -nowT;  // already explained
		}
		if (turnAt != 0 && (turnAt < 0 || nowT - turnAt > 0.6)) {
			static int logs = 0;
			const bool explained = turnAt < 0 || McScriptedRecently() || link::NowNs() * 1e-9 - g_testTurnAt < 1.6;
			if (logs++ < 20)
				console->Print(explained ? "[bridge] mode: Portal 2's sequence turned the view %.0f deg in one frame, now (%.1f %.1f)\n"
				                         : "[bridge] BUG: the view turned %.0f deg in one frame without a Minecraft transit, now (%.1f %.1f)\n", turnDeg, turnAng.x, turnAng.y);
			turnAt = 0;
		}
		lastF = f, have = active, g_skipTurnCheck = false;
	}
	if (!g_seenInit) {
		g_seenInit = true, g_seen = g_ack = mc.transitSeq;
		return;
	}
	if (mc.transitSeq == g_seen) return;
	const McTransit &x = link::LastMcTransit();
	const uint32_t n = mc.transitSeq - g_seen;
	g_seen = mc.transitSeq;
	g_ack = mc.transitSeq;
	if (x.seq != mc.transitSeq) {
		console->Print("[bridge] BUG: mc transit %u: the transform block holds transit %u\n", mc.transitSeq, x.seq);
		return;
	}
	++g_transits;
	for (auto &ch : g_foldSticky) for (double &t : ch) t = 0;  // (bridge #68) Minecraft's transit arrived: the folds end
	// (the speeds of the first transit since pcb_assert_mc_transits reset: a loop between two portals would overwrite them)
	if (!g_haveFirst) {
		g_haveFirst = true;
		g_lastFrom = (int)(mc.transitPortals & 0xff);
		g_lastIn = g_lastVel.Length(), g_lastOut = g_tickVel.Length(), g_outPending = true;
	}
	if (!active) return;
	// Portal 2's view angles turned by the pair (the roll kept apart: Portal 2's own look has none)
	QAngle va = engine.ViewAngles();
	va.z = g_roll;
	Vector f, r, u;
	Basis(va, f, r, u);
	QAngle na = Angles(TurnSrc(x.q, f), TurnSrc(x.q, u));
	g_roll = std::remainder(na.z, 360.0f);
	const QAngle set{std::fmax(-89.0f, std::fmin(89.0f, na.x)), na.y, 0.0f};
	engine.SetAngles(set);
	g_turnedNow = true;
	g_skipTurnCheck = true;
	if (g_turnLogs++ < 200)
		console->Print("[bridge] mc transit %u%s: portal %u -> %u at %.2f of the tick, speed %.0f -> %.0f u/s; look (%.1f %.1f) -> (%.1f %.1f) roll %.1f\n", mc.transitSeq,
		               n > 1 ? " (more than one since the last frame)" : "", mc.transitPortals & 0xff, (mc.transitPortals >> 8) & 0xff, x.frac, g_lastIn, g_lastOut, va.x, va.y, set.x,
		               set.y, g_roll);
}

// The latest tick's line p0 -> p1 crossed a linked portal's plane inside the opening, and p is behind that plane: the portal to
// draw p through (-1: none). p beyond p1 (the host extrapolates up to half a tick) counts on the same line.
static int FoldPortal(const Vector &p0, const Vector &p1, const Vector &p, int chan) {
	const double now = NowS();
	for (int k = 0; k < kPortalSlots; ++k) {  // (bridge #32: every linked pair, slot k's partner is k ^ 1)
		if (!PortalLinked(k)) continue;
		const Vector &n = g_portalNormal[k], &c = g_portalPos[k];
		const float s = Vector::DotProduct(p - c, n);
		if (s >= 0.0f) {
			g_foldSticky[chan][k] = 0;
			continue;
		}
		const float s0 = Vector::DotProduct(p0 - c, n), s1 = Vector::DotProduct(p1 - c, n);
		Vector a, b;
		float sa, sb;
		bool crossing = true;
		if ((s0 < 0.0f) != (s1 < 0.0f)) a = p0, b = p1, sa = s0, sb = s1;
		else if (s1 >= 0.0f) a = p1, b = p, sa = s1, sb = s;  // extrapolated past p1
		else crossing = false;
		const float limX = g_portalHalfW[k] + 2.0f, limY = g_portalHalfH[k] + 2.0f;
		if (crossing) {
			const Vector hit = a + (b - a) * (sa / (sa - sb)) - c;
			if (std::fabs(Vector::DotProduct(hit, PortalRightAxis(k))) <= limX && std::fabs(Vector::DotProduct(hit, g_portalUp[k])) <= limY) {
				g_foldSticky[chan][k] = now;
				return k;
			}
			continue;
		}
		// (bridge #68) the crossing was some ticks ago and Minecraft's own transit (the eye crosses after the hull centre, a floor portal's 20 u later) has not
		// arrived: still behind the plane, still over the opening, within 0.5 s of the crossing: keep drawing it through the pair. Without it the body flicked
		// to the exit, back to the entry's hold and to the exit again, and everything that watches the body (triggers, the held object) saw three transits.
		if (g_foldSticky[chan][k] > 0 && now - g_foldSticky[chan][k] < 0.5) {
			const Vector q = p - c - n * s;
			if (std::fabs(Vector::DotProduct(q, PortalRightAxis(k))) <= limX && std::fabs(Vector::DotProduct(q, g_portalUp[k])) <= limY) return k;
		}
	}
	return -1;
}

// The puppet body (mc.cpp FeetSrc / PuppetProvider): its hull centre folded by the same rule as the eye.
static int g_bodyHeld = 0;
static Vector McTransitFoldFeetRaw(const Vector &feet, const Vector &prev, const Vector &cur, Vector *vel);
Vector McTransitFoldFeet(const Vector &feet, const Vector &prev, const Vector &cur, Vector *vel) {
	const Vector up(0, 0, 36);
	return McTransitFoldFeetRaw(feet, prev, cur, vel);
}
// The puppet only (mc.cpp PuppetProvider): Portal 2 never takes the puppet itself (t28_transit4: it did from a hull 2-12 u in front
// of the plane, its corner or feet through it): a hull that reaches through a linked portal's plane over the opening is held 1 u in
// front of it. The body is unseen (the avatar is drawn where Minecraft's player is); Minecraft's player is not moved.
Vector McTransitHoldBody(const Vector &feet) {
	const Vector up(0, 0, 36);
	Vector out = feet;
	for (int i = 0; i < kPortalSlots; ++i) {
		if (!PortalLinked(i)) continue;
		const Vector &n = g_portalNormal[i];
		const Vector c = out + up;
		const float s = Vector::DotProduct(c - g_portalPos[i], n), e = 16.0f * (std::fabs(n.x) + std::fabs(n.y)) + 36.0f * std::fabs(n.z);
		if (s - e >= 1.0f || s <= -e) continue;
		const Vector q = c - g_portalPos[i] - n * s;
		if (std::fabs(Vector::DotProduct(q, PortalRightAxis(i))) > g_portalHalfW[i] + 16.0f || std::fabs(Vector::DotProduct(q, g_portalUp[i])) > g_portalHalfH[i] + 36.0f) continue;
		out = out + n * (e + 1.0f - s);
		++g_bodyHeld;
	}
	return out;
}
static Vector McTransitFoldFeetRaw(const Vector &feet, const Vector &prev, const Vector &cur, Vector *vel) {
	const Vector up(0, 0, 36);
	const int k = FoldPortal(prev + up, cur + up, feet + up, 0);
	if (g_debugTicks > 0 && g_portalSet[0] && g_portalSet[1]) {
		static int logs = 0;
		for (int i = 0; i < 2 && logs < 200; ++i) {  // (the gun's pair only: a debug aid)
			const float s = Vector::DotProduct(feet + up - g_portalPos[i], g_portalNormal[i]);
			if (s < 0 && s > -96 && (feet + up - g_portalPos[i] - g_portalNormal[i] * s).Length() < 70)
				++logs, console->Print("[bridge] mc transit debug: body centre %.1f u behind portal %d (%s): prev %.1f cur %.1f\n", -s, i, k == i ? "folded" : "NOT folded",
				                       Vector::DotProduct(prev + up - g_portalPos[i], g_portalNormal[i]), Vector::DotProduct(cur + up - g_portalPos[i], g_portalNormal[i]));
		}
	}
	if (k < 0) return feet;
	Vector o, o0, ov;
	QAngle a{0, 0, 0}, unused;
	PortalThroughPose(k, k ^ 1, feet + up, a, o, unused);
	if (vel) {
		PortalThroughPose(k, k ^ 1, Vector(0, 0, 0), a, o0, unused);
		PortalThroughPose(k, k ^ 1, *vel, a, ov, unused);
		*vel = ov - o0;
	}
	++g_bodyFolds;
	return o - up;
}

// The camera (mc.cpp OverrideView, Minecraft's view): eye on the latest tick's line eye0 -> eye1.
static int g_foldLogs = 0;
void McTransitCamera(CViewSetup *view, const Vector &eye, const Vector &eye0, const Vector &eye1) {
	const double now = NowS();
	const double dt = g_lastFrameT > 0 ? std::fmin(0.1, now - g_lastFrameT) : 0.0;
	g_lastFrameT = now;
	// the look is Portal 2's view angles as set now: the angles OverrideView got are the client's predicted ones, which lag a
	// SetViewAngles by a frame or two (t28_transit1: single frames turned back 90 deg after a transit)
	{
		const QAngle a = engine.ViewAngles();
		view->angles.x = a.x, view->angles.y = a.y;
	}
	// Portal 2's roll levelling (research/specs/portal-transit.md 1.7)
	if (g_roll != 0.0f && dt > 0) {
		const float ang = std::fabs(g_roll);
		if (ang <= 0.1f) g_roll = 0;
		else {
			float step = ang * (1.0f - (float)std::exp(-5.5451775 * dt));
			if (step / dt < 10.0) step = 10.0f * (float)dt;
			g_roll = ang <= step ? 0.0f : g_roll - std::copysign(step, g_roll);
		}
	}
	view->angles.z = g_roll;
	view->origin = eye;
	const int k = FoldPortal(eye0, eye1, eye, 1);
	if (k < 0) return;
	Vector o;
	QAngle a;
	PortalThroughPose(k, k ^ 1, eye, view->angles, o, a);
	if (g_foldLogs++ < 60)
		console->Print("[bridge] mc transit: eye (%.1f %.1f %.1f) behind portal %d drawn through the pair at (%.1f %.1f %.1f)\n", eye.x, eye.y, eye.z, k, o.x, o.y, o.z);
	++g_foldFrames;
	view->origin = o, view->angles = a;
}

// Portal 2's own move of the puppet (puppet.cpp, before the game's ProcessMovement in Minecraft mode): the velocity into a linked
// portal the hull is about to reach is taken out, so Portal 2's move never carries the hull centre across the plane (its transit
// would run as well as Minecraft's). The puppet's pose and velocity are Minecraft's again after the move.
void McTransitGuardMove(const Vector &origin, Vector &vel, float dt) {
	const Vector c = origin + Vector(0, 0, 36);
	for (int k = 0; k < kPortalSlots; ++k) {
		if (!PortalLinked(k)) continue;
		const Vector &n = g_portalNormal[k];
		const float vn = Vector::DotProduct(vel, n);
		if (vn >= 0.0f) continue;
		const float s = Vector::DotProduct(c - g_portalPos[k], n);
		if (s < -40.0f || s > 72.0f - vn * dt * 3.0f) continue;
		const Vector q = c - g_portalPos[k] - n * s;
		if (std::fabs(Vector::DotProduct(q, PortalRightAxis(k))) > g_portalHalfW[k] + 24.0f || std::fabs(Vector::DotProduct(q, g_portalUp[k])) > g_portalHalfH[k] + 40.0f) continue;
		vel = vel - n * vn;
		++g_guarded;
	}
}


// (bridge #66) The held object and a Minecraft transit. Minecraft moves the player and the puppet jumps to the exit without a Portal 2 transit; Portal 2's
// grab only follows a player's own portal transit, so it kept pulling the object toward the player's new place through the world (or, when the object had
// crossed the portal first, still treated it as held through the portal) and let go of it. The object is found (the nearest prop of a held class to the old
// eye, or to the same eye through the pair: it may have crossed first), put through the same pair when it is still on the entry side, and then held again
// in front of the player on the exit side: when Portal 2 lets go (at the jump or in the ticks after), it is put at the hold point and +use is pulsed until it
// is held (g_mcCarryUse, read by mc.cpp CreateMove).
bool PlayerHolding();                                           // propcollide.cpp
bool EntTeleport(void *ent, const Vector &pos, const QAngle &ang);  // floatorigin.cpp
bool g_mcCarryUse = false;
static void *g_carryObj = nullptr;
static int g_carryTicks = 0, g_carryHeldTicks = 0;
static bool g_carryPlaced = false, g_carryDropped = false;
void McTransitCarryHeld(const Vector &before, int i) {
	if (i < 0 || !PlayerHolding()) return;
	const Vector eye = before + Vector(0, 0, 64);
	Vector eyeOut;
	QAngle ua{0, 0, 0}, oa;
	PortalThroughPose(i, i ^ 1, eye, ua, eyeOut, oa);
	void *best = nullptr;
	float bd = 170.0f;
	bool entrySide = true;
	for (const char *cls : {"prop_weighted_cube", "prop_monster_box", "prop_physics", "prop_physics_override", "prop_physics_multiplayer", "prop_glass_futbol", "npc_personality_core",
	                        "npc_portal_turret_floor", "npc_wheatley_boss"})
		for (void *e : EntFind(cls)) {
			const Vector eo = EntVec(e, "origin");
			const float d0 = (eo - eye).Length(), d1 = (eo - eyeOut).Length();
			if (d0 < bd) bd = d0, best = e, entrySide = true;
			if (d1 < bd) bd = d1, best = e, entrySide = false;
		}
	if (!best) return console->Print("[bridge] mc transit: held object not found near the eye (%.0f %.0f %.0f) or its exit\n", eye.x, eye.y, eye.z);
	g_carryObj = best, g_carryTicks = 300, g_carryHeldTicks = 0, g_carryPlaced = false, g_carryDropped = false;
	static int logs = 0;
	if (logs++ < 40)
		console->Print("[bridge] mc transit: the held %s (%s the pair's entry) is let go and held again in front of the player on the exit side (portal %d -> %d)\n",
		               EntKV(best, "classname").c_str(), entrySide ? "at" : "past", i, i ^ 1);
}
// Every server tick (clip.cpp ClipTick).
void McTransitCarryTick() {
	g_mcCarryUse = false;
	if (!g_carryObj || g_carryTicks <= 0) return void(g_carryObj = nullptr);
	--g_carryTicks;
	static int carryLogs = 0;
	if (g_carryTicks % 10 == 0 && carryLogs++ < 120) {
		const Vector co = EntVec(g_carryObj, "origin"), bo = PlayerOrigin();
		console->Print("[bridge] mc carry: tick %d holding %d cube (%.0f %.0f %.0f) body (%.0f %.0f %.0f)\n", g_carryTicks, PlayerHolding(), co.x, co.y, co.z, bo.x, bo.y, bo.z);
	}
	// 1. Portal 2's grab keeps treating the object as held through the pair (it pulls it toward the player's old side): +use lets it go
	if (!g_carryDropped) {
		if (PlayerHolding()) {
			g_mcCarryUse = (g_carryTicks % 8) < 3;
			return;
		}
		g_carryDropped = true;
	}
	// 2. the object goes to the hold point in front of the player (the exit side), 3. and is taken again with +use
	if (PlayerHolding()) {
		if (++g_carryHeldTicks >= 30) g_carryObj = nullptr;  // held steadily: done
		return;
	}
	g_carryHeldTicks = 0;
	const Vector body = PlayerOrigin();
	const QAngle va = engine.ViewAngles();
	const float y = va.y * (float)M_PI / 180;
	const Vector at = body + Vector(0, 0, 50) + Vector(std::cos(y), std::sin(y), 0) * 60.0f;
	if (!g_carryPlaced || (EntVec(g_carryObj, "origin") - at).Length() > 14.0f) {
		const Vector ca = EntVec(g_carryObj, "angles");
		EntTeleport(g_carryObj, at, QAngle{ca.x, ca.y, ca.z});
		g_carryPlaced = true;
	}
	g_mcCarryUse = (g_carryTicks % 6) < 3;  // +use pulses until Portal 2 holds it
}

// ---- tests -----------------------------------------------------------------------------------------------------------------
CON_COMMAND(pcb_mc_transit_debug, "pcb_mc_transit_debug <ticks> - (bridge #28) log Minecraft's feet, the portals and unfolded bodies for that many ticks\n") {
	g_debugTicks = args.ArgC() > 1 ? std::atoi(args[1]) : 100;
}
CON_COMMAND(pcb_assert_mc_transits, "pcb_assert_mc_transits <min> | reset - Minecraft's own transits of its player through the pair since the reset (bridge #28)\n") {
	static int base = 0;
	if (args.ArgC() > 1 && args[1][0] == 'r') return void((base = g_transits, g_haveFirst = false));
	const int want = args.ArgC() > 1 ? std::atoi(args[1]) : 1;
	char buf[220];
	std::snprintf(buf, sizeof buf, "%d since the reset (want >= %d); last from portal %d at %.0f u/s in, %.0f u/s out; %d frames drawn through the pair, %d body folds, %d guarded moves, %d body holds",
	              g_transits - base, want, g_lastFrom, g_lastIn, g_lastOut, g_foldFrames, g_bodyFolds, g_guarded, g_bodyHeld);
	TestCheck(g_transits - base >= want, "Minecraft player transits", buf);
}
// The speed out of the latest Minecraft transit against the speed in: Portal 2's rules (out = in, capped at 1000 u/s, floor exits
// at least ~300 u/s along the exit's normal) within tol %.
CON_COMMAND(pcb_assert_mc_transit_speed, "pcb_assert_mc_transit_speed [tol_pct=10] [min_in=0] - the first Minecraft transit since pcb_assert_mc_transits reset kept its speed (bridge #28)\n") {
	const float tol = args.ArgC() > 1 ? (float)std::atof(args[1]) : 10.0f, minIn = args.ArgC() > 2 ? (float)std::atof(args[2]) : 0.0f;
	const float want = std::fmin(g_lastIn, 1000.0f);
	const float r = want > 1 ? g_lastOut / want : 0;
	char buf[200];
	std::snprintf(buf, sizeof buf, "from portal %d: %.0f u/s in, %.0f u/s out (%.0f%% of %.0f), tol %.0f%%, min entry %.0f", g_lastFrom, g_lastIn, g_lastOut, 100 * r, want, tol, minIn);
	TestCheck(g_transits > 0 && g_lastIn >= minIn && std::fabs(r - 1) * 100 <= tol, "Minecraft transit kept its speed", buf);
}

// ---- pcb_view_track (bridge #20, moved here and extended by #28): the camera through a portal, frame by frame ---------------
// Records the rendered view (origin and forward) every frame; the report unfolds the frames after the crossing back through the
// pair (exit -> entry), so the path is one continuous line through the portal and any hitch shows: the camera speed before and
// after (u/s, wall clock), stalled frames (< 30% of the speed before), the largest step against the expected one, and the largest
// turn of the view between two frames (deg) against the frames before the crossing.
struct ViewSample {
	double t;
	Vector o, f;
};
static std::vector<ViewSample> g_vt;
PCB_GAUGE("mctransit.viewSamples", g_vt.size());
static bool g_vtOn = false;
void ViewTrackFrame(const CViewSetup *view) {
	if (!g_vtOn || g_vt.size() >= 6000) return;
	Vector f, r, u;
	Basis(view->angles, f, r, u);
	g_vt.push_back({NowS(), view->origin, f});
}
static float g_vtLastRetained = 0, g_vtLastJump = 0, g_vtLastTurn = 0;
static int g_vtLastStallFrames = 0;
CON_COMMAND(pcb_view_track, "pcb_view_track start | report <label> | assert <max_stall_frames> <min_retained_pct> <max_jump_ratio> [max_turn_deg] - camera smoothness through a portal\n") {
	if (args.ArgC() < 2) return;
	if (!std::strcmp(args[1], "start")) {
		g_vt.clear(), g_vtOn = true;
		return;
	}
	if (!std::strcmp(args[1], "assert")) {
		const int maxStall = args.ArgC() > 2 ? std::atoi(args[2]) : 0;
		const float minRet = args.ArgC() > 3 ? (float)std::atof(args[3]) : 80.0f, maxJump = args.ArgC() > 4 ? (float)std::atof(args[4]) : 3.0f;
		const float maxTurn = args.ArgC() > 5 ? (float)std::atof(args[5]) : 1e9f;
		char b[240];
		std::snprintf(b, sizeof b, "%d stalled frames (want <= %d), speed retained %.0f%% (want >= %.0f%%), worst jump %.2fx the expected step (want <= %.1f), worst turn %.1f deg (want <= %.1f)",
		              g_vtLastStallFrames, maxStall, g_vtLastRetained, minRet, g_vtLastJump, maxJump, g_vtLastTurn, maxTurn);
		return TestCheck(g_vtLastStallFrames <= maxStall && g_vtLastRetained >= minRet && g_vtLastJump <= maxJump && g_vtLastTurn <= maxTurn, "camera through the portal", b);
	}
	if (std::strcmp(args[1], "report")) return;
	g_vtOn = false;
	const char *label = args.ArgC() > 2 ? args[2] : "";
	const size_t n = g_vt.size();
	if (n < 10 || !g_portalSet[0] || !g_portalSet[1]) return console->Print("[bridge] view track %s: %zu frames, portals %d %d: nothing to report\n", label, n, g_portalSet[0], g_portalSet[1]);
	// the crossing: the first frame much nearer the other portal than the previous one was
	size_t cross = 0;
	int from = -1;
	for (size_t k = 1; k < n && !cross; ++k)
		for (int i = 0; i < 2; ++i) {
			const int j = 1 - i;
			const float pi = (g_vt[k - 1].o - g_portalPos[i]).Length(), pj = (g_vt[k - 1].o - g_portalPos[j]).Length(), ci = (g_vt[k].o - g_portalPos[i]).Length(),
			            cj = (g_vt[k].o - g_portalPos[j]).Length();
			if (pi < 200.0f && cj < 200.0f && pi < pj && cj < ci && (g_vt[k].o - g_vt[k - 1].o).Length() > 60.0f) {
				cross = k, from = i;
				break;
			}
		}
	if (!cross) return console->Print("[bridge] view track %s: %zu frames, no crossing found\n", label, n);
	std::vector<Vector> u(n), uf(n);
	for (size_t k = 0; k < n; ++k) {
		if (k < cross) u[k] = g_vt[k].o, uf[k] = g_vt[k].f;
		else {
			QAngle a{0, 0, 0}, oa;
			Vector o0, of;
			PortalThroughPose(1 - from, from, g_vt[k].o, a, u[k], oa);  // exit side -> entry side
			PortalThroughPose(1 - from, from, Vector(0, 0, 0), a, o0, oa);
			PortalThroughPose(1 - from, from, g_vt[k].f, a, of, oa);
			uf[k] = of - o0;
		}
	}
	auto speed = [&](size_t k) { const double dt = g_vt[k].t - g_vt[k - 1].t; return dt > 1e-4 ? (u[k] - u[k - 1]).Length() / dt : 0.0; };
	auto turn = [&](size_t k) { return std::acos(std::fmax(-1.0f, std::fmin(1.0f, Vector::DotProduct(uf[k], uf[k - 1]) / std::fmax(1e-6f, uf[k].Length() * uf[k - 1].Length())))) * 180.0 / M_PI; };
	double before = 0, after = 0, turnBefore = 0;
	int nb = 0, na = 0;
	// (bridge #28) by time, not frames (an unfocused run draws 40 frames a second, a focused one 200): 150 ms before the crossing,
	// and 30-150 ms after it (a fling that hits something later is not the transit's)
	const double tc = g_vt[cross].t;
	for (size_t k = 1; k < cross; ++k)
		if (g_vt[k].t >= tc - 0.15 && g_vt[k].t <= tc - 0.02) before += speed(k), turnBefore = std::max(turnBefore, turn(k)), ++nb;
	for (size_t k = cross + 1; k < n; ++k)
		if (g_vt[k - 1].t >= tc + 0.03 && g_vt[k].t <= tc + 0.15) after += speed(k), ++na;
	before = nb ? before / nb : 0, after = na ? after / na : 0;
	double slowest = 1e9, worstJump = 0, worstTurn = 0;
	int stall = 0;
	for (size_t k = cross > 6 ? cross - 6 : 1; k < n && k < cross + 8; ++k) {
		if (g_vt[k].t < tc - 0.1 || g_vt[k - 1].t > tc + 0.15) continue;  // (the transit's frames, by time)
		const double sp = speed(k);
		slowest = std::min(slowest, sp);
		if (before > 1 && sp < 0.3 * before) ++stall;
		const double dt = g_vt[k].t - g_vt[k - 1].t, expect = std::max(1.0, before * dt);
		worstJump = std::max(worstJump, (double)(u[k] - u[k - 1]).Length() / expect);
		worstTurn = std::max(worstTurn, turn(k));
	}
	g_vtLastStallFrames = stall, g_vtLastRetained = before > 1 ? (float)(100.0 * after / before) : 0, g_vtLastJump = (float)worstJump, g_vtLastTurn = (float)worstTurn;
	console->Print("[bridge] VIEW TRACK %s: %zu frames, crossing at frame %zu (portal %d -> %d); camera %.0f u/s before, %.0f u/s after (%.0f%%), slowest frame %.0f u/s, %d stalled frames, worst jump %.2fx, worst turn %.1f deg (before the crossing %.1f)\n",
	               label, n, cross, from, 1 - from, before, after, g_vtLastRetained, slowest, stall, worstJump, worstTurn, turnBefore);
	for (size_t k = cross > 6 ? cross - 6 : 1; k < n && k < cross + 8; ++k)
		console->Print("[bridge]   frame %zu%s dt %.1f ms, unfolded (%.1f %.1f %.1f), %.0f u/s, turn %.2f deg\n", k, k == cross ? " CROSS" : "", 1000.0 * (g_vt[k].t - g_vt[k - 1].t), u[k].x, u[k].y,
		               u[k].z, speed(k), turn(k));
}

}  // namespace bridge
