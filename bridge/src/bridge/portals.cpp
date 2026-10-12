// Portalcraft bridge: portals on Minecraft blocks (our placement + Portal 2's prop_portal).
//
//   pcb_portal_blocks <0|1> <x> <y> <z> <yaw> [pitch=0]
//
// Casts a ray from x y z along yaw/pitch against the blocks (and the world, to
// know if a wall is closer), then fits the 64 x 112 opening onto the block face
// the way Portal 2 fits portals on walls (research/specs/portal-placement.md
// section 4): the opening shrunk by 2 u per side must lie on coplanar block faces
// with nothing in front; if the aim point does not fit, the centre slides to the
// nearest position that does, at most 91.2 u away, otherwise the shot fails.
// Wall portals keep world-up as their up vector. The fitted portal is then put
// in place with Portal 2's own portal_place (moves the real prop_portal pair).
//
//   pcb_portal_check
//
// Reads the prop_portal entities back (IServerTools FirstEntity / NextEntity /
// GetKeyValue) and compares their origin and angles to what was requested.
#include "bridge.hpp"
#include "slots.hpp"

#include "Modules/Tier1.hpp"
#include "Offsets.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace bridge {

void PlacePortalAt(int which, const Vector &at, const Vector &nrm);  // plugin.cpp

static const float kHW = 32, kHH = 56;
static Vector g_requested[2], g_fitted[2];
static bool g_haveRequest[2] = {false, false};

// ---- server tools readback -------------------------------------------------------
using _FirstEntity = void *(__rescall *)(void *);
using _NextEntity = void *(__rescall *)(void *, void *);
using _GetKeyValue = bool(__rescall *)(void *, void *, const char *, char *, int);
static Interface *g_tools = nullptr;

static bool ToolsReady() {
	if (!g_tools) g_tools = Interface::Create(MODULE_NAME("server"), "VSERVERTOOLS001", false);
	return g_tools != nullptr;
}

// IServerTools: slot numbers per operating system are in slots.hpp.
static std::string KV(void *ent, const char *key) {
	char buf[256] = {0};
	auto get = slots::Orig<_GetKeyValue>(g_tools, slots::kToolsGetKeyValue);
	if (!get) return "";
	if (!get(g_tools->ThisPtr(), ent, key, buf, sizeof buf)) return "";
	return buf;
}

struct PortalInfo {
	void *ent;
	Vector origin;
	QAngle angles;
	std::string name, two, active;
};

static std::vector<PortalInfo> ReadPortals() {
	std::vector<PortalInfo> out;
	if (!ToolsReady()) return out;
	auto first = slots::Orig<_FirstEntity>(g_tools, slots::kToolsFirstEntity);
	auto next = slots::Orig<_NextEntity>(g_tools, slots::kToolsNextEntity);
	if (!first || !next) return out;
	int guard = 0;
	for (void *e = first(g_tools->ThisPtr()); e && guard < 8192; e = next(g_tools->ThisPtr(), e), ++guard) {
		if (KV(e, "classname") != "prop_portal") continue;
		PortalInfo p{e};
		std::string o = KV(e, "origin"), a = KV(e, "angles");
		std::sscanf(o.c_str(), "%f %f %f", &p.origin.x, &p.origin.y, &p.origin.z);
		std::sscanf(a.c_str(), "%f %f %f", &p.angles.x, &p.angles.y, &p.angles.z);
		p.name = KV(e, "targetname");
		p.two = KV(e, "PortalTwo");
		p.active = KV(e, "Activated");
		out.push_back(p);
	}
	return out;
}

CON_COMMAND(pcb_portal_check, "pcb_portal_check - read the prop_portal entities back and compare with the requested placements\n") {
	auto ps = ReadPortals();
	console->Print("[bridge] %d prop_portal entities\n", (int)ps.size());
	for (auto &p : ps) {
		int which = p.two == "1" ? 1 : 0;
		console->Print("[bridge]   prop_portal '%s' PortalTwo=%s Activated=%s origin (%.2f %.2f %.2f) angles (%.1f %.1f %.1f)\n", p.name.c_str(), p.two.c_str(),
		               p.active.c_str(), p.origin.x, p.origin.y, p.origin.z, p.angles.x, p.angles.y, p.angles.z);
		if (g_haveRequest[which]) {
			float err = (p.origin - g_fitted[which]).Length();
			console->Print("[bridge]   portal %d: requested (%.2f %.2f %.2f) fitted (%.2f %.2f %.2f) actual (%.2f %.2f %.2f) |fitted-actual| %.3f -> %s\n", which,
			               g_requested[which].x, g_requested[which].y, g_requested[which].z, g_fitted[which].x, g_fitted[which].y, g_fitted[which].z, p.origin.x,
			               p.origin.y, p.origin.z, err, err < 0.6f ? "PASS" : "FAIL");
		}
	}
}

// ---- block faces -------------------------------------------------------------------

// Inside (or on the boundary of) any block: a point on the seam between two
// neighbouring blocks counts as solid.
static bool InsideBlock(const Vector &p) {
	const float e = 0.001f;
	for (auto &b : g_blocks)
		if (p.x >= b.mins.x - e && p.x <= b.mins.x + kUnitsPerBlock + e && p.y >= b.mins.y - e && p.y <= b.mins.y + kUnitsPerBlock + e &&
		    p.z >= b.mins.z - e && p.z <= b.mins.z + kUnitsPerBlock + e)
			return true;
	return false;
}

// Ray against block AABBs: nearest hit t and face normal.
static bool RayBlocks(const Vector &o, const Vector &d, float &tBest, Vector &nBest) {
	bool hit = false;
	for (auto &b : g_blocks) {
		Vector mn = b.mins, mx = b.mins + Vector(kUnitsPerBlock, kUnitsPerBlock, kUnitsPerBlock);
		float t0 = 0, t1 = 1e9f;
		Vector n(0, 0, 0);
		bool miss = false;
		for (int a = 0; a < 3 && !miss; ++a) {
			float s = (&o.x)[a], dd = (&d.x)[a], lo = (&mn.x)[a], hi = (&mx.x)[a];
			if (std::fabs(dd) < 1e-7f) {
				if (s < lo || s > hi) miss = true;
				continue;
			}
			float ta = (lo - s) / dd, tb = (hi - s) / dd, sg = -1;
			if (ta > tb) std::swap(ta, tb), sg = 1;
			if (ta > t0) t0 = ta, n = Vector(0, 0, 0), (&n.x)[a] = sg;
			t1 = std::fmin(t1, tb);
			if (t0 > t1) miss = true;
		}
		if (miss || n.SquaredLength() == 0 || t0 >= tBest) continue;
		tBest = t0;
		nBest = n;
		hit = true;
	}
	return hit;
}

CON_COMMAND(pcb_portal_blocks, "pcb_portal_blocks <0|1> <x> <y> <z> <yaw> [pitch=0] - shoot a portal at the blocks from x y z (fits it onto the block face)\n") {
	if (args.ArgC() < 6) return console->Print("%s", pcb_portal_blocks.ThisPtr()->m_pszHelpString);
	int which = std::atoi(args[1]) & 1;
	Vector o((float)std::atof(args[2]), (float)std::atof(args[3]), (float)std::atof(args[4]));
	float yaw = (float)std::atof(args[5]) * (float)M_PI / 180, pitch = (args.ArgC() > 6 ? (float)std::atof(args[6]) : 0) * (float)M_PI / 180;
	Vector d(std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), -std::sin(pitch));

	float t = 1e9f;
	Vector n;
	if (!RayBlocks(o, d, t, n)) return console->Print("[bridge] portal_blocks: no block hit\n");
	CGameTrace wt;
	engine.Trace(o, o + d * t, Vector(0, 0, 0), MASK_SHOT_PORTAL, wt, true);
	if (wt.fraction < 0.999f) return console->Print("[bridge] portal_blocks: the world is in the way at (%.1f %.1f %.1f)\n", wt.endpos.x, wt.endpos.y, wt.endpos.z);
	Vector aim = o + d * t;

	// Portal frame on this face: F = n, U = world up for walls (else world +X), R = U x F.
	Vector F = n;
	Vector U = std::fabs(F.z) > 0.7f ? Vector(1, 0, 0) : Vector(0, 0, 1);
	Vector R(U.y * F.z - U.z * F.y, U.z * F.x - U.x * F.z, U.x * F.y - U.y * F.x);
	int fa = std::fabs(F.x) > 0.5f ? 0 : std::fabs(F.y) > 0.5f ? 1 : 2;  // face axis
	float plane = (&aim.x)[fa];                                          // face plane coordinate

	// A point of the face plane is portalable surface if there is block just
	// behind it and none just in front.
	auto onSurface = [&](Vector p) {
		(&p.x)[fa] = plane - (&F.x)[fa] * 0.5f;
		if (!InsideBlock(p)) return false;
		(&p.x)[fa] = plane + (&F.x)[fa] * 0.5f;
		return !InsideBlock(p);
	};
	// Nothing may stick out in front of the opening (world solid or another block).
	auto obstructed = [&](Vector p) {
		(&p.x)[fa] = plane + (&F.x)[fa] * 1.0f;
		if (InsideBlock(p)) return true;
		return (engine.GetPointContents(engine.traceServer->ThisPtr(), p, MASK_SOLID_BRUSHONLY, nullptr) & MASK_SOLID_BRUSHONLY) != 0;
	};
	// Portal 2's fit: the opening shrunk by 2 u must lie on the surface, and the
	// full opening must be clear in front (so it ends up flush against a floor or
	// ledge, like a real wall portal). Samples ~4 u apart.
	auto fits = [&](const Vector &c) {
		const float si = kHW - 2 + 0.01f, su = kHH - 2 + 0.01f;
		for (int i = 0; i <= 15; ++i)
			for (int j = 0; j <= 28; ++j) {
				float r = -si + 2 * si * i / 15, u = -su + 2 * su * j / 28;
				if (!onSurface(c + R * r + U * u)) return false;
			}
		const float fi = kHW - 0.05f, fu = kHH - 0.05f;
		for (int i = 0; i <= 16; ++i)
			for (int j = 0; j <= 28; ++j) {
				float r = -fi + 2 * fi * i / 16, u = -fu + 2 * fu * j / 28;
				if (obstructed(c + R * r + U * u)) return false;
			}
		return true;
	};
	Vector centre = aim;
	(&centre.x)[fa] = plane;
	Vector best;
	float bestD = 1e9f;
	const float maxMove = std::sqrt(2 * (kHW * kHW + kHH * kHH));  // 91.2 u
	// Nearest fitting centre: 1 u grid around the aim point, then refine to 1/32 u.
	auto search = [&](const Vector &around, float range, float step) {
		int n = (int)(range / step);
		for (int i = -n; i <= n; ++i)
			for (int j = -n; j <= n; ++j) {
				Vector c = around + R * (i * step) + U * (j * step);
				Vector off = c - centre;
				float dd = off.SquaredLength();
				if (dd >= bestD || dd > maxMove * maxMove) continue;
				if (fits(c)) bestD = dd, best = c;
			}
	};
	search(centre, maxMove, 1.0f);
	if (bestD < 1e8f) search(best, 1.0f, 1.0f / 32);
	if (bestD > 1e8f) return console->Print("[bridge] portal_blocks: CANT_FIT (block face too small near (%.1f %.1f %.1f))\n", aim.x, aim.y, aim.z);
	// Floor snap for wall portals (placement spec step 6): if the floor, world or a
	// block top, is within hh + 1.5 below the centre but not closer than hh - 0.1,
	// the bottom edge lands flush on it.
	bool snapped = false;
	if (std::fabs(U.z) > 0.7f) {
		Vector from = best + F * 0.05f;
		float dist = kHH + 1.5f;
		CGameTrace ft;
		engine.Trace(from, from - U * dist, Vector(0, 0, 0), MASK_SOLID_BRUSHONLY, ft, true);
		float hitD = ft.fraction < 1 ? ft.fraction * dist : 1e9f;
		for (auto &b : g_blocks) {  // block tops under the opening's centre line
			float top = b.mins.z + kUnitsPerBlock;
			if (from.x > b.mins.x && from.x < b.mins.x + kUnitsPerBlock && from.y > b.mins.y && from.y < b.mins.y + kUnitsPerBlock && top <= from.z)
				hitD = std::fmin(hitD, from.z - top);
		}
		if (hitD <= kHH + 1.5f && hitD >= kHH - 0.1f) {
			best = best - U * (hitD - kHH);
			snapped = true;
		}
	}
	Vector at = best + F * 0.5f;
	g_requested[which] = aim;
	g_fitted[which] = at;
	g_haveRequest[which] = true;
	console->Print("[bridge] portal_blocks %d: aim (%.2f %.2f %.2f) normal (%g %g %g) -> fitted centre (%.2f %.2f %.2f), moved %.2f u%s; bottom edge z %.2f\n", which, aim.x,
	               aim.y, aim.z, F.x, F.y, F.z, at.x, at.y, at.z, std::sqrt(bestD), snapped ? ", floor-snapped" : "", at.z - kHH);
	PlacePortalAt(which, at, F);
}

}  // namespace bridge
