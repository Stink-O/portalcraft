// Portalcraft bridge: the transition test (crossings chamber <-> Minecraft, recorded frame by frame).
//
//   pcb_aim_track <0|1|off> [min_area_pct=0.2] [trace_crossings=0]
//       Until Chell passes through it, the camera turns towards portal N's centre like a person's
//       (critically damped, at most 180 deg/s, no snapping); released on the tick she teleports, so
//       the frames after the crossing are Portal 2's own view handling.
//   pcb_aim_check <0|1> [rate]
//       Count `portal_in_view` from now (the approach window): the portal's projected centre within
//       the central 40% of the screen and its opening at least min_area_pct of the screen.
//   pcb_goto_aim, pcb_stop, pcb_launch_to
//       Placement in front of a portal (velocity zeroed) and a ballistic launch into it.
// The per-frame image checks are pcb_frame_watch ... transit (composite.cpp).
#include "bridge.hpp"
#include "coords.hpp"

#include "Modules/Tier1.hpp"

#include <cmath>
#include <ctime>
#include "platform/platform.hpp"
#include <cstdlib>
#include <cstring>

namespace bridge {

void TestCheck(bool ok, const char *what, const char *detail);  // testassert.cpp
void PuppetLaunch(const Vector &vel);                           // puppet.cpp

static int g_aimPortal = -1;
static bool g_aimActive = false;
static Vector g_aimLastEye;
static float g_minAreaPct = 0.2f;
static int g_inViewOk = 0, g_inViewFail = 0, g_aimFrames = 0;
static float g_lastAreaPct = 0, g_lastCentre = 0;
static int g_aimLog = 0;
static int g_traceCrossings = 0;
void CompositeTraceViews(int n);  // composite.cpp

// Before the composite's frame setup (mc.cpp OverrideView hook): may turn the camera. The camera
// turns like a person's: towards the portal's centre on a critically damped spring, at most
// g_aimRate deg/s, never snapping; control is released on the tick Chell teleports, so the frames
// after the crossing are Portal 2's own view handling (exit orientation, roll recovery).
static double g_aimT = 0;
static QAngle g_aimCur;
static float g_aimVel = 0;  // deg/s along the great circle towards the target
static float g_aimRate = 180.0f, g_aimOmega = 9.0f;
static bool g_aimCheck = false;  // count portal_in_view (the approach window)
static bool g_aimCheckOnce = false;
static int g_aimSkipped = 0;
static bool g_inWindow = false;  // this frame is in the approach window (portal_in_view counted)  // frames whose target was out of reach (past the pitch clamp or behind)
static double NowSec() {
	return (double)plat::MonotonicNs() * 1e-9;
}
static Vector Fwd(const QAngle &a) {
	float sp = std::sin(a.x * (float)M_PI / 180), cp = std::cos(a.x * (float)M_PI / 180);
	float sy = std::sin(a.y * (float)M_PI / 180), cy = std::cos(a.y * (float)M_PI / 180);
	return Vector(cp * cy, cp * sy, -sp);
}
// pcb_cam_turn: a person's turn: yaw eased in and out (cosine velocity profile), peak rate as given.
static bool g_turnOn = false;
static double g_turnT0 = 0, g_turnDur = 1;
static float g_turnYaw0 = 0, g_turnDeg = 0;
// pcb_fly_to: the noclip player carried to a point at a steady speed (long trips: persistence tests).
static bool g_flyOn = false;
static Vector g_flyTo;
static float g_flySpeed = 1000;
static double g_flyT = 0;
static void CamTurnFrame(CViewSetup *view) {
	if (!g_turnOn) return;
	double t = NowSec() - g_turnT0;
	if (g_turnT0 == 0) g_turnT0 = NowSec(), g_turnYaw0 = view->angles.y, t = 0;
	float k = (float)std::fmin(1.0, t / g_turnDur);
	float done = g_turnDeg * (k - std::sin(2 * (float)M_PI * k) / (2 * (float)M_PI));
	QAngle a = view->angles;
	a.y = g_turnYaw0 + done;
	view->angles = a;
	engine.SetAngles(a);
	if (k >= 1.0f) g_turnOn = false, console->Print("[bridge] turn: %.0f deg done in %.2f s\n", g_turnDeg, t);
}
static void FlyFrame(CViewSetup *view) {
	if (!g_flyOn) return;
	double now = NowSec();
	float dt = g_flyT > 0 ? (float)std::fmin(0.05, now - g_flyT) : 0.0f;
	g_flyT = now;
	Vector p = PlayerOrigin(), d = g_flyTo - p;
	float L = d.Length(), step = g_flySpeed * dt;
	Vector q = L <= step ? g_flyTo : p + d * (step / L);
	char buf[128];
	std::snprintf(buf, sizeof buf, "setpos %.2f %.2f %.2f", q.x, q.y, q.z);
	engine.Cmd(buf);
	if (L <= step) g_flyOn = false, console->Print("[bridge] fly: arrived at (%.0f %.0f %.0f)\n", q.x, q.y, q.z);
}
// pcb_steer: the camera held every frame (long scripted trips; the user's mouse cannot turn it): towards a
// Minecraft point (converted through the current mapping, so floating-origin shifts do not matter) or at
// fixed angles.
static int g_steer = 0;  // 0 off, 1 towards g_steerMc, 2 fixed g_steerAng
static double g_steerMc[3];
static QAngle g_steerAng;
static void SteerFrame(CViewSetup *view) {
	if (!g_steer) return;
	QAngle a = g_steerAng;
	if (g_steer == 1) {
		Vector d = McToSrc(g_steerMc[0], g_steerMc[1], g_steerMc[2]) - view->origin;
		float L = d.Length();
		if (L < 1.0f) return;
		a.x = std::fmax(-89.0f, std::fmin(89.0f, -std::asin(d.z / L) * 180.0f / (float)M_PI));
		a.y = std::atan2(d.y, d.x) * 180.0f / (float)M_PI, a.z = 0;
	}
	view->angles = a;
	engine.SetAngles(a);
}
CON_COMMAND(pcb_steer, "pcb_steer <mcX> <mcY> <mcZ> | rel <dx> <dy> <dz> | angles <pitch> <yaw> | off - hold the camera every frame towards a Minecraft point or at fixed angles (tests)\n") {
	if (args.ArgC() < 2 || !std::strcmp(args[1], "off")) return void(g_steer = 0);
	if (!std::strcmp(args[1], "angles") && args.ArgC() > 3) {
		g_steer = 2, g_steerAng = QAngle{(float)std::atof(args[2]), (float)std::atof(args[3]), 0};
	} else if (!std::strcmp(args[1], "rel") && args.ArgC() > 4) {
		// towards Chell's eye (Minecraft coordinates) plus an offset in blocks
		double e[3];
		SrcToMcD(PlayerOrigin() + Vector(0, 0, 64), e[0], e[1], e[2]);
		g_steer = 1;
		for (int i = 0; i < 3; ++i) g_steerMc[i] = e[i] + std::atof(args[2 + i]);
	} else if (args.ArgC() > 3) {
		g_steer = 1;
		for (int i = 0; i < 3; ++i) g_steerMc[i] = std::atof(args[1 + i]);
	}
	console->Print("[bridge] steer: %s\n", g_steer == 1 ? "towards a Minecraft point" : "fixed angles");
}
void TransitFrame(CViewSetup *view) {
	SteerFrame(view);
	CamTurnFrame(view);
	FlyFrame(view);
	if (!g_aimActive || g_aimPortal < 0 || !g_portalSet[g_aimPortal]) return;
	const Vector c = g_portalPos[g_aimPortal], n = g_portalNormal[g_aimPortal];
	float d = Vector::DotProduct(view->origin - c, n);
	// a teleport: a jump of the eye from right at the portal (a setpos before the approach is not one)
	bool jumped = g_aimLastEye.Length() > 0.1f && (view->origin - g_aimLastEye).Length() > 200.0f &&
	              std::fabs(Vector::DotProduct(g_aimLastEye - c, n)) < 120.0f;
	g_aimLastEye = view->origin;
	if (jumped || d < 0.0f) {
		g_aimActive = false, g_aimCheck = false;
		if (g_traceCrossings > 0) --g_traceCrossings, CompositeTraceViews(24);  // the views of the crossing's frames
		console->Print("[bridge] aim: through portal %d (%s), released after %d frames (%d with the target out of reach, not aimed)\n", g_aimPortal, jumped ? "teleported" : "past its plane",
		               g_aimFrames, g_aimSkipped);
		return;
	}
	double now = NowSec();
	float dt = g_aimT > 0 ? (float)std::fmin(0.05, std::fmax(0.001, now - g_aimT)) : 1.0f / 120;
	g_aimT = now;
	if (g_aimFrames == 0) g_aimCur = view->angles, g_aimVel = 0;
	Vector dir = c - view->origin;
	float len = dir.Length();
	Vector want = dir * (1.0f / len), cur = Fwd(g_aimCur);
	// never drive the camera into the pitch clamp, or round to a target behind it: such a target is
	// not aimed at (the camera is left as it is)
	const float needPitch = -std::asin(std::fmax(-1.0f, std::fmin(1.0f, want.z))) * 180.0f / (float)M_PI;
	const bool reachable = std::fabs(needPitch) <= 89.0f && Vector::DotProduct(Fwd(view->angles), want) > 0.0f;
	QAngle a = view->angles;
	if (reachable) {
		float err = std::acos(std::fmax(-1.0f, std::fmin(1.0f, Vector::DotProduct(cur, want)))) * 180.0f / (float)M_PI;
		// critically damped towards the target, rate limited
		g_aimVel += (g_aimOmega * g_aimOmega * err - 2 * g_aimOmega * g_aimVel) * dt;
		g_aimVel = std::fmax(0.0f, std::fmin(g_aimRate, g_aimVel));
		float step = std::fmin(err, g_aimVel * dt);
		Vector f = want;
		if (err > 1e-3f && step < err) {
			// rotate cur towards want by step degrees (slerp on the unit sphere)
			float t = step / err, th = err * (float)M_PI / 180;
			float sa = std::sin((1 - t) * th) / std::sin(th), sb = std::sin(t * th) / std::sin(th);
			f = cur * sa + want * sb;
			f = f * (1.0f / f.Length());
		}
		a = QAngle{-std::asin(std::fmax(-1.0f, std::fmin(1.0f, f.z))) * 180.0f / (float)M_PI, std::atan2(f.y, f.x) * 180.0f / (float)M_PI, 0.0f};
		g_aimCur = a;
	} else {
		g_aimCur = view->angles, g_aimVel = 0, ++g_aimSkipped;
	}
	if (reachable) {
		view->angles = a;
		engine.SetAngles(a);
	}
	++g_aimFrames;
	// portal_in_view (counted in the approach window): the portal's centre within the central 40% of
	// the screen, the opening's share of the screen from its solid angle
	float tx = std::tan(view->fov * (float)M_PI / 360.0f), ty = tx / (view->m_flAspectRatio > 0.1f ? view->m_flAspectRatio : 16.0f / 9);
	Vector fa = Fwd(a);
	float cosOff = Vector::DotProduct(fa, want);
	g_lastCentre = cosOff > 1e-3f ? std::sqrt(std::fmax(0.0f, 1 - cosOff * cosOff)) / cosOff / tx : 99.0f;  // NDC radius of the centre
	g_lastAreaPct = 100.0f * ((float)M_PI * 32 * 56 * std::fabs(Vector::DotProduct(n, want)) / (len * len)) / (4 * tx * ty);
	// the approach window ends a block from the plane: closer, the portal covers a third of the screen or
	// more and its centre swings faster than a person's turn (the crossing itself is not an approach)
	g_inWindow = g_aimCheck && (d >= 40.0f || g_aimCheckOnce);
	if (!g_inWindow) return;
	if (g_aimCheckOnce) g_aimCheck = g_aimCheckOnce = false;  // a single check (the moment before a fall)
	// (right at the plane the portal fills most of the screen and its centre is no longer a meaningful
	// target: in view while it covers >= 40%)
	bool ok = (g_lastCentre < 0.4f && g_lastAreaPct >= g_minAreaPct) || g_lastAreaPct >= 40.0f;
	ok ? ++g_inViewOk : ++g_inViewFail;
	if (g_aimLog > 0 || !ok) {
		if (g_aimLog > 0) --g_aimLog;
		console->Print("[bridge] aim f%d: eye (%.1f %.1f %.1f) pitch %.1f yaw %.1f, %.0f u to portal %d, centre at %.2f, opening %.2f%% of the screen%s\n", g_aimFrames, view->origin.x,
		               view->origin.y, view->origin.z, a.x, a.y, len, g_aimPortal, g_lastCentre, g_lastAreaPct, ok ? "" : "  PORTAL NOT IN VIEW");
	}
}

// For the per-frame transit log: -1 when not aiming, else 0/1 = portal_in_view this frame.
int TransitApproach(float &pitch, float &yaw, float &areaPct) {
	QAngle a = engine.ViewAngles();
	pitch = a.x, yaw = a.y, areaPct = g_lastAreaPct;
	if (!g_aimActive || !g_aimCheck || !g_inWindow) return -1;
	return (g_lastCentre < 0.4f && g_lastAreaPct >= g_minAreaPct) || g_lastAreaPct >= 40.0f ? 1 : 0;
}

CON_COMMAND(pcb_aim_track, "pcb_aim_track <0|1|off> [min_area_pct=0.2] - keep the camera on portal N's centre until the eye passes through it; checks portal_in_view\n") {
	if (args.ArgC() < 2 || args[1][0] == 'o') {
		g_aimActive = false;
		return;
	}
	g_aimPortal = std::atoi(args[1]) & 1;
	if (args.ArgC() > 2) g_minAreaPct = (float)std::atof(args[2]);
	if (args.ArgC() > 3) g_traceCrossings = std::atoi(args[3]);
	g_aimActive = true, g_aimFrames = 0, g_aimLastEye = Vector(0, 0, 0), g_aimLog = 6, g_aimT = 0, g_aimSkipped = 0;
}

CON_COMMAND(pcb_aim_check, "pcb_aim_check <0|1|once> [rate_deg_s=180] - count portal_in_view from now (the approach window), or for the next frame only / aim turn rate\n") {
	g_aimCheckOnce = args.ArgC() > 1 && args[1][0] == 'o';  // "once": just the next frame
	g_aimCheck = g_aimCheckOnce || (args.ArgC() > 1 && std::atoi(args[1]) != 0);
	if (args.ArgC() > 2) g_aimRate = (float)std::atof(args[2]);
}

CON_COMMAND(pcb_assert_in_view, "pcb_assert_in_view [reset] - every aimed approach frame since the reset had the portal in view (portal_in_view)\n") {
	if (args.ArgC() > 1) return void(g_inViewOk = g_inViewFail = 0);
	char buf[160];
	std::snprintf(buf, sizeof buf, "%d approach frames in view, %d not (centre within the central 40%%, opening >= %.2f%% of the screen)", g_inViewOk, g_inViewFail, g_minAreaPct);
	TestCheck(g_inViewOk > 0 && g_inViewFail == 0, "portal_in_view", buf);
}

bool TerrainSectionKnown(int bx, int by, int bz);  // terrain.cpp: the section holding this block has streamed
// Precondition for the clip's tests: the Minecraft sections around a block (the cliff exit) have
// arrived from Minecraft, by section id, before anything relies on them.
CON_COMMAND(pcb_assert_sections_mc, "pcb_assert_sections_mc <x> <y> <z> [radius_blocks=2] - the sections holding every block within the radius of this Minecraft block have streamed\n") {
	if (args.ArgC() < 4) return console->Print("%s", pcb_assert_sections_mc.ThisPtr()->m_pszHelpString);
	int x = std::atoi(args[1]), y = std::atoi(args[2]), z = std::atoi(args[3]), r = args.ArgC() > 4 ? std::atoi(args[4]) : 2;
	char buf[400];
	int n = 0, missing = 0, len = 0;
	buf[0] = 0;
	for (int sx = (x - r) >> 4; sx <= (x + r) >> 4; ++sx)
		for (int sy = (y - r) >> 4; sy <= (y + r) >> 4; ++sy)
			for (int sz = (z - r) >> 4; sz <= (z + r) >> 4; ++sz) {
				bool ok = TerrainSectionKnown(sx * 16, sy * 16, sz * 16);
				++n, missing += !ok;
				if (len < 300) len += std::snprintf(buf + len, sizeof buf - len, "[%d %d %d %s] ", sx, sy, sz, ok ? "ok" : "MISSING");
			}
	TestCheck(missing == 0 && n > 0, "Minecraft sections streamed", buf);
}

CON_COMMAND(pcb_stop, "pcb_stop - zero the player's velocity on the next move (after a setpos)\n") { PuppetLaunch(Vector(0, 0, 0)); }

// pcb_goto_aim <0|1> <height> <lateral> [azimuth_deg=0]: setpos so the eye is `height` u out from
// portal N's plane and `lateral` u off its axis (azimuth around the normal), velocity zeroed. The
// approach then starts there (gravity, or pcb_launch_to).
CON_COMMAND(pcb_goto_aim, "pcb_goto_aim <0|1> <height> <lateral> [azimuth=0] - place the eye `height` u out from portal N's plane, `lateral` u off its axis; velocity zeroed\n") {
	if (args.ArgC() < 4) return console->Print("%s", pcb_goto_aim.ThisPtr()->m_pszHelpString);
	int i = std::atoi(args[1]) & 1;
	float h = (float)std::atof(args[2]), lat = (float)std::atof(args[3]), az = args.ArgC() > 4 ? (float)std::atof(args[4]) * (float)M_PI / 180 : 0;
	// lateral axes in world terms: a floor/ceiling portal's are world x and y (azimuth 0 = +x, 90 = +y);
	// a wall portal's are its horizontal right and world up
	Vector n = g_portalNormal[i], r, u;
	if (std::fabs(n.z) > 0.7f) r = Vector(1, 0, 0), u = Vector(0, 1, 0);
	else {
		r = Vector(n.y, -n.x, 0);
		r = r * (1.0f / std::fmax(1e-3f, r.Length()));
		u = Vector(0, 0, 1);
	}
	Vector eye = g_portalPos[i] + n * h + (r * std::cos(az) + u * std::sin(az)) * lat;
	Vector feet = eye - Vector(0, 0, 64);
	char buf[128];
	std::snprintf(buf, sizeof buf, "setpos %.2f %.2f %.2f", feet.x, feet.y, feet.z);
	engine.Cmd(buf);
	PuppetLaunch(Vector(0, 0, 0));
	// facing the portal from the start (pitch within the clamp), as a player standing there would
	Vector d = g_portalPos[i] - eye;
	float pitch = std::fmax(-88.0f, std::fmin(88.0f, -std::asin(d.z / d.Length()) * 180.0f / (float)M_PI)), yaw = std::atan2(d.y, d.x) * 180.0f / (float)M_PI;
	QAngle face{pitch, yaw, 0};
	engine.SetAngles(face);
	console->Print("[bridge] goto_aim portal %d: eye (%.1f %.1f %.1f): %s, facing pitch %.1f yaw %.1f\n", i, eye.x, eye.y, eye.z, buf, pitch, yaw);
}

CON_COMMAND(pcb_launch_to, "pcb_launch_to <x> <y> <z> <speed> | p<0|1> <speed> - ballistic velocity that carries the player's centre to the point (horizontal speed; straight down if below)\n") {
	if (args.ArgC() < 3) return console->Print("%s", pcb_launch_to.ThisPtr()->m_pszHelpString);
	Vector t;
	float speed;
	if (args[1][0] == 'p') {  // pcb_launch_to p<0|1> <speed>: portal N's centre
		int i = (args[1][1] - '0') & 1;
		t = g_portalPos[i], speed = (float)std::atof(args[2]);
	} else {
		if (args.ArgC() < 5) return console->Print("%s", pcb_launch_to.ThisPtr()->m_pszHelpString);
		t = Vector((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3])), speed = (float)std::atof(args[4]);
	}
	Vector o = PlayerOrigin() + Vector(0, 0, 36);
	Vector d = t - o;
	float h = std::sqrt(d.x * d.x + d.y * d.y);
	Vector v;
	if (h < 8.0f) {
		v = Vector(d.x * 2, d.y * 2, -speed);
	} else {
		float time = h / speed;
		v = Vector(d.x / h * speed, d.y / h * speed, d.z / time + 0.5f * 600.0f * time);
	}
	PuppetLaunch(v);
	console->Print("[bridge] launch from (%.0f %.0f %.0f) to (%.0f %.0f %.0f): velocity (%.0f %.0f %.0f)\n", o.x, o.y, o.z, t.x, t.y, t.z, v.x, v.y, v.z);
}

CON_COMMAND(pcb_launch_vel, "pcb_launch_vel <vx> <vy> <vz> - set the player's velocity on the next move (tests: entry speed and angle into a portal)\n") {
	if (args.ArgC() < 4) return console->Print("%s", pcb_launch_vel.ThisPtr()->m_pszHelpString);
	PuppetLaunch(Vector((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3])));
}

CON_COMMAND(pcb_cam_turn, "pcb_cam_turn <degrees> [peak_deg_s=180] - turn the camera's yaw like a person (eased in and out)\n") {
	if (args.ArgC() < 2) return;
	g_turnDeg = (float)std::atof(args[1]);
	float peak = args.ArgC() > 2 ? (float)std::atof(args[2]) : 180.0f;
	g_turnDur = std::fmax(0.1, 2.0 * std::fabs(g_turnDeg) / peak);  // the cosine profile peaks at twice the mean rate
	g_turnT0 = 0, g_turnOn = true, g_aimActive = false;
}
CON_COMMAND(pcb_fly_to, "pcb_fly_to <x> <y> <z> [speed=1000] | p<0|1> <out_u> [speed] - carry the (noclip) player to a point at a steady speed\n") {
	if (args.ArgC() < 3) return;
	if (args[1][0] == 'p') {
		int i = (args[1][1] - '0') & 1;
		float out = (float)std::atof(args[2]);
		g_flyTo = g_portalPos[i] + g_portalNormal[i] * out - Vector(0, 0, 64);
		g_flySpeed = args.ArgC() > 3 ? (float)std::atof(args[3]) : 1000;
	} else {
		if (args.ArgC() < 4) return;
		g_flyTo = Vector((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3]));
		g_flySpeed = args.ArgC() > 4 ? (float)std::atof(args[4]) : 1000;
	}
	g_flyOn = true, g_flyT = 0;
	console->Print("[bridge] fly to (%.0f %.0f %.0f) at %.0f u/s\n", g_flyTo.x, g_flyTo.y, g_flyTo.z, g_flySpeed);
}
// pcb_stand_before <0|1> <distance> [azimuth_deg=0]: Chell standing on the ground in front of a wall
// portal (feet at the opening's bottom edge, `distance` u out) or beside a floor portal (`distance` u
// from its centre, azimuth around it), facing its centre; velocity zeroed.
CON_COMMAND(pcb_stand_before, "pcb_stand_before <0|1> <distance> [azimuth=0] - stand on the ground in front of / beside portal N, facing it\n") {
	if (args.ArgC() < 3) return console->Print("%s", pcb_stand_before.ThisPtr()->m_pszHelpString);
	int i = std::atoi(args[1]) & 1;
	float dist = (float)std::atof(args[2]), az = args.ArgC() > 3 ? (float)std::atof(args[3]) * (float)M_PI / 180 : 0;
	const Vector c = g_portalPos[i], n = g_portalNormal[i];
	Vector feet;
	if (std::fabs(n.z) > 0.7f) {
		feet = c + Vector(std::cos(az), std::sin(az), 0) * dist + Vector(0, 0, 1);
	} else {
		Vector side(-n.y, n.x, 0);
		feet = c + n * (dist * std::cos(az)) + side * (dist * std::sin(az));
		feet.z = c.z - 56 + 1;
	}
	char buf[128];
	std::snprintf(buf, sizeof buf, "setpos %.2f %.2f %.2f", feet.x, feet.y, feet.z);
	engine.Cmd(buf);
	PuppetLaunch(Vector(0, 0, 0));
	Vector d = c - (feet + Vector(0, 0, 64));
	QAngle face{std::fmax(-80.0f, std::fmin(80.0f, -std::asin(d.z / d.Length()) * 180.0f / (float)M_PI)), std::atan2(d.y, d.x) * 180.0f / (float)M_PI, 0};
	engine.SetAngles(face);
	console->Print("[bridge] stand before portal %d: %s, facing pitch %.1f yaw %.1f\n", i, buf, face.x, face.y);
}
}  // namespace bridge
