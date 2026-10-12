// Portalcraft bridge: Chell collides with Minecraft blocks (movement-trace hook).
//
// Every player hull trace Portal 2's movement code makes goes through
// CPortalGameMovement::TracePlayerBBox (GameMovement001 vtable slot 14 on
// Linux; slot 15 is PlayerSolidMask, slot 13 SetupMovementBounds). The hook
// runs the game's own trace (world, entities, portal environments), then
// sweeps the same hull against every block AABB and keeps whichever hit comes
// first. A block hit is reported as a world hit (m_pEnt = the world entity),
// so ground detection, stepping, jumping and sliding treat blocks exactly like
// brushes. Server and client (prediction) movement are both hooked.
//
// Portal holes: while the hull overlaps an open portal's hole volume (98% of
// the 64x112 opening, 500 u deep; research/specs/portal-transit.md section 4),
// blocks inside that volume are ignored, so walking into a portal on a block
// wall works. Portals are the ones the bridge placed (pcb_portal*).
#include "bridge.hpp"
#include "slots.hpp"
#include "coords.hpp"
#include "dig.hpp"
#include "stage.hpp"

#include "Modules/Tier1.hpp"
#include "Offsets.hpp"
#include "Utils/Memory.hpp"
#include "Utils/SDK/GameMovement.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace bridge {

static Interface *g_svMove = nullptr, *g_clMove = nullptr;
using _TracePlayerBBox = void(__rescall *)(void *thisptr, const Vector &start, const Vector &end, unsigned mask, int group, CGameTrace &tr);
static _TracePlayerBBox SvTracePlayerBBox = nullptr, ClTracePlayerBBox = nullptr;

static bool g_enabled = true;
static void *g_svWorld = nullptr, *g_clWorld = nullptr;
static int g_blockHits = 0;
static int g_debug = 0;
static int g_traceLog = 0;

// Standing hull. (The ducked hull is 36 u tall; see README known gaps.)
static const Vector kHullMins(-16, -16, 0), kHullMaxs(16, 16, 72);
static const float kVoxelSkin = 3.0f;    // (bridge #35) the export's sub-voxels are 5 u: its surfaces sit up to 2.5 u from the engine's exact ones
void CollisionFieldVoxelMissing(int &rx, int &ry, int &rz);  // collfield.cpp (bridge #35)
bool CollisionFieldHarvestActive();
// (bridge #44) the exported voxels of one hull trace are read once: DigEngineFilter reads them and the sweep that follows in the same Merge reuses them (a trace
// cost them twice, and a stuck player's 50 position tests a tick took 60 ms in the user's log)
static std::vector<std::pair<Vector, Vector>> g_mergeVox;
static Vector g_mergeLo, g_mergeHi;
static bool g_mergeVoxValid = false;
static const float kDigNearPad = 44.0f;  // (bridge #33) a dug cell (40 u) and a little: the hull near one uses the exported voxels

struct Box {
	Vector mn, mx;
	float skin = 0.5f;  // (bridge #35) how deep a start inside the box still counts as contact (SweepBox); the exported voxels get kVoxelSkin
};

static bool Overlap(const Box &a, const Box &b) {
	return a.mn.x < b.mx.x && a.mx.x > b.mn.x && a.mn.y < b.mx.y && a.mx.y > b.mn.y && a.mn.z < b.mx.z && a.mx.z > b.mn.z;
}

// How deep the hole is carved behind a portal's plane. Portal 2 teleports Chell when her centre crosses the plane, so
// she needs her hull's reach behind it plus one tick of travel (a 3500 u/s fall is 58 u a tick): 128 u. It was
// 500 u (the transit spec's figure): in the user's session (bridge #12) Chell, behind a floor portal on blocks with
// no transit, fell 12 blocks down that column into the rock.
static float g_holeDepth = 128.0f;  // pcb_collide_hole_depth

// World-space bounds of a portal's hole volume: `front` u in front of the plane, `depth` u behind it.
static Box HoleBoxEx(int i, float front, float depth) {
	Vector f = g_portalNormal[i];
	// portal "up" is world Z for wall portals, else world X
	Vector up = g_portalUp[i];
	if (std::fabs(Vector::DotProduct(up, f)) > 0.5f) up = std::fabs(f.z) > 0.7f ? Vector(1, 0, 0) : Vector(0, 0, 1);  // not read from a live portal
	Vector r(f.y * up.z - f.z * up.y, f.z * up.x - f.x * up.z, f.x * up.y - f.y * up.x);
	// Portal 2's hole is 0.98 of the opening. For wall portals the bottom edge is
	// carved down to 1 u below the opening instead, so a portal whose bottom sits
	// on the floor (Portal 2 fits them flush) leaves no lip in the block wall.
	const float hw = g_portalHalfW[i] * 0.98f, hhTop = g_portalHalfH[i] * 0.98f, hhBottom = std::fabs(f.z) > 0.7f ? g_portalHalfH[i] * 0.98f : g_portalHalfH[i] + 1.0f;
	Box b{Vector(1e9f, 1e9f, 1e9f), Vector(-1e9f, -1e9f, -1e9f)};
	for (int k = 0; k < 8; ++k) {
		float fd = (k & 1) ? front : -depth;
		Vector p = g_portalPos[i] + f * fd + r * ((k & 2) ? hw : -hw) + up * ((k & 4) ? hhTop : -hhBottom);
		b.mn = Vector(std::fmin(b.mn.x, p.x), std::fmin(b.mn.y, p.y), std::fmin(b.mn.z, p.z));
		b.mx = Vector(std::fmax(b.mx.x, p.x), std::fmax(b.mx.y, p.y), std::fmax(b.mx.z, p.z));
	}
	return b;
}
// Chell's block carve (0.5 u in front of the plane, g_holeDepth behind it)
static Box HoleBox(int i) { return HoleBoxEx(i, 0.5f, g_holeDepth); }

// The hull's centre is over portal i's opening (its axis line inside the 0.98 ellipse): only then is the
// hole carved for Chell. The hole volume is the opening's bounding box, larger than the ellipse at the
// corners; carving it for any overlap let Chell standing beside a floor portal sink a few units into
// the ground there and stay stuck once she moved off it (run mw1s4, 84 ticks).
static bool CentreOverOpening(const Vector &origin, int i) {
	Vector f = g_portalNormal[i], up = g_portalUp[i];
	if (std::fabs(Vector::DotProduct(up, f)) > 0.5f) up = std::fabs(f.z) > 0.7f ? Vector(1, 0, 0) : Vector(0, 0, 1);
	Vector r(f.y * up.z - f.z * up.y, f.z * up.x - f.x * up.z, f.x * up.y - f.y * up.x);
	Vector q = origin + Vector(0, 0, 36) - g_portalPos[i];
	float lx = Vector::DotProduct(q, r) / (g_portalHalfW[i] * 0.98f), ly = Vector::DotProduct(q, up) / (g_portalHalfH[i] * 0.98f);
	// (a wall portal: the hull's height is inside the opening when its bottom is on the opening's bottom)
	if (std::fabs(f.z) <= 0.7f) ly = std::fmax(-1.0f, std::fmin(1.0f, ly * 0.5f));
	return lx * lx + ly * ly <= 1.0f;
}

// This trace's hull sweep reaches portal i's plane moving into it, with the hull's centre over the
// opening where it touches the plane (Portal 2's simulator carves the hole as soon as the player is in
// the portal's environment; a per-tick "already in the hole" test let a fast fall hit the block face
// first: Chell stopped dead on the ground for a tick, then was teleported with her speed lost).
static bool SweptIntoOpening(const Vector &start, const Vector &end, int i) {
	const Vector n = g_portalNormal[i], P = g_portalPos[i];
	const Vector c0 = start + Vector(0, 0, 36), c1 = end + Vector(0, 0, 36);
	if (Vector::DotProduct(c1 - c0, n) >= 0) return false;  // not moving into it
	const float h = 16 * std::fabs(n.x) + 16 * std::fabs(n.y) + 36 * std::fabs(n.z);  // the hull's reach along n
	const float d0 = Vector::DotProduct(c0 - P, n) - h, d1 = Vector::DotProduct(c1 - P, n) - h;
	if (d1 > 0.5f || d0 < -h) return false;  // does not reach the plane this trace / already deep in it
	const float t = d0 > 0 ? d0 / (d0 - d1) : 0.0f;
	const Vector at = start + (end - start) * t;
	return CentreOverOpening(at, i);
}

// A box overlaps an open portal's hole volume (props: no terrain collider there, so they pass through).
// Portal i's hole volume (world bounds), for the Minecraft player's collision (collfield.cpp, mc.cpp)
bool PortalHoleBox(int i, Vector &mn, Vector &mx) {
	// (bridge #20) only a linked portal has a hole: an open portal without its partner is a wall in Portal 2. The
	// Minecraft player walked into a lone portal's hole, 128 u into the wall, and the view went into Portal 2's void
	// (the user's "the world goes white")
	if (!PortalLinked(i)) return false;  // (bridge #32: any slot; its partner is i ^ 1, the other colour of its linkage group)
	// (bridge #28) Portal 2's own hole, 0.98 of the opening all round: not Chell's block carve, which reaches 1 u under a wall
	// portal's bottom edge. The export's 1/8-block voxels then cut the chamber floor in the hole (the floor 0.15 block lower
	// in the hole: the Minecraft player stepped down 6 u walking in, and came out 6 u inside the exit's floor)
	// (bridge #45) the hole for the MINECRAFT player, applied to its collision at query time (the mod's BlockCollisionsMixin carves the host's voxels and the
	// blocks), not baked into the exported field any more:
	//  - in front: the export's sub-voxels are 5 u, solid by their centre (or, in thin geometry, by a box that touches), so a wall's front layer can stand up to
	//    5 u in front of the true surface (a portal sits on the true surface); the hole starts 6 u in front of the plane so that layer is inside it. (A
	//    1-voxel lip across the middle of the opening stopped the player's hull in campaign chambers: see HANDOFF #45.)
	//  - behind: only as deep as the transit needs. A wall or ceiling portal: the hull's reach and a margin (the eye crosses the plane 0.3 block into it).
	//    A floor portal: the eye is 1.62 blocks above the feet, so the feet must be able to sink that far (128 u as before).
	const bool floorLike = g_portalNormal[i].z > 0.7f;
	Box b = HoleBoxEx(i, 6.0f, floorLike ? g_holeDepth : std::fmin(g_holeDepth, 56.0f));
	if (std::fabs(g_portalNormal[i].z) <= 0.7f) {
		const Vector up = g_portalUp[i];
		if (up.z > 0.99f) b.mn.z = std::fmax(b.mn.z, g_portalPos[i].z - g_portalHalfH[i] * 0.98f);
	}
	mn = b.mn, mx = b.mx;
	return true;
}
bool BoxInPortalHole(const Vector &mn, const Vector &mx) {
	Box b{mn, mx};
	for (int i = 0; i < kPortalSlots; ++i)
		if (g_portalSet[i] && Overlap(b, HoleBox(i))) return true;
	return false;
}

// Sweep the origin point (hull folded into the box) against one box.
// Returns false on a miss; sets startSolid if the start is deep inside.
//
// A hull resting exactly on a face (the usual state when standing on a block) must register a
// hit at t = 0 when it moves into that face; otherwise gravity sinks it a little every tick until
// it is inside and stuck (run pit6: Chell sank 0.2 u into the ground and could not walk or jump).
// A shallow overlap (up to kSkin, float error at |z| ~ 15000) is treated as contact with the
// nearest face: moving further in is blocked at t = 0, moving out or along is free.
[[maybe_unused]] static const float kSkin = 0.5f;  // (= Box::skin's default)
static bool g_seamFix = true;  // pcb_collide_seams
static bool SweepBox(const Vector &start, const Vector &d, const Box &bb, float &tOut, Vector &nOut, bool &startSolid) {
	Vector mn = bb.mn - kHullMaxs, mx = bb.mx - kHullMins;
	// Resting contact on a top: a hull whose feet are up to the skin below this box's top stands on it. The top
	// is taken at the feet, so the box's sides are no wall for that hull (bridge #12: standing 0.17 u down on a
	// prop collider's top, every seam between two floor blocks stopped Chell dead: "invisible blocks" while
	// carrying a cube). Moving down still hits it at t = 0; moving up or along it is free.
	const float skin = bb.skin;
	if (g_seamFix && start.z < mx.z && start.z > mx.z - skin) mx.z = start.z;
	bool inside = true;
	float minPen = 1e30f, penSign = 0;
	int penAxis = -1;
	for (int a = 0; a < 3; ++a) {
		float s = (&start.x)[a], lo = (&mn.x)[a], hi = (&mx.x)[a];
		if (s <= lo || s >= hi) {
			inside = false;
			break;
		}
		if (s - lo < minPen) minPen = s - lo, penAxis = a, penSign = -1;
		if (hi - s < minPen) minPen = hi - s, penAxis = a, penSign = 1;
	}
	if (inside) {
		if (minPen > skin) {
			startSolid = true;
			return true;
		}
		if ((&d.x)[penAxis] * penSign >= 0) return false;  // moving out of or along the face
		tOut = 0;
		nOut = Vector(0, 0, 0);
		(&nOut.x)[penAxis] = penSign;
		return true;
	}
	float t0 = -1e30f, t1 = 1e30f;
	Vector n(0, 0, 0);
	for (int a = 0; a < 3; ++a) {
		float s = (&start.x)[a], dd = (&d.x)[a], lo = (&mn.x)[a], hi = (&mx.x)[a];
		if (std::fabs(dd) < 1e-7f) {
			if (s <= lo || s >= hi) return false;
			continue;
		}
		float ta = (lo - s) / dd, tb = (hi - s) / dd;
		float nsign = -1;
		if (ta > tb) std::swap(ta, tb), nsign = 1;
		if (ta > t0) {
			t0 = ta;
			n = Vector(0, 0, 0);
			(&n.x)[a] = nsign;
		}
		if (tb < t1) t1 = tb;
	}
	if (t0 >= t1 || t1 <= 0 || t0 > 1 || n.SquaredLength() == 0) return false;
	tOut = std::fmax(t0, 0.0f);
	nOut = n;
	return true;
}

// a minus b as up to 6 boxes (b clipped to a first).
static int Subtract(const Box &a, const Box &b, Box out[6]) {
	if (!Overlap(a, b)) {
		out[0] = a;
		return 1;
	}
	int n = 0;
	Box r = a;
	for (int ax = 0; ax < 3; ++ax) {
		float blo = (&b.mn.x)[ax], bhi = (&b.mx.x)[ax];
		if (blo > (&r.mn.x)[ax]) {  // slab below b
			Box s = r;
			(&s.mx.x)[ax] = blo;
			out[n++] = s;
			(&r.mn.x)[ax] = blo;
		}
		if (bhi < (&r.mx.x)[ax]) {  // slab above b
			Box s = r;
			(&s.mn.x)[ax] = bhi;
			out[n++] = s;
			(&r.mx.x)[ax] = bhi;
		}
	}
	return n;  // r (the part inside b) is dropped
}

// Sweep the hull from start to end against the blocks; true if a block is hit
// before `frac` (updated), with the entry normal.
static bool SweepBlocks(const Vector &start, const Vector &end, float &frac, Vector &normal, bool &startSolid) {
	Box hull{start + kHullMins, start + kHullMaxs};
	Box holes[2];
	bool inHole[2] = {false, false};
	for (int i = 0; i < 2; ++i) {
		if (!g_portalSet[i]) continue;
		holes[i] = HoleBox(i);
		inHole[i] = (Overlap(hull, holes[i]) && CentreOverOpening(start, i)) || SweptIntoOpening(start, end, i);
	}
	Vector d = end - start;
	float len = d.Length();
	bool hit = false;
	startSolid = false;
	// Candidates: the placed test blocks, plus Minecraft terrain blocks (kRenSolids) in the
	// swept hull's bounds.
	std::vector<Box> cands;
	cands.reserve(g_blocks.size() + 32);
	for (auto &b : g_blocks) cands.push_back(Box{b.mins, b.mins + Vector(kUnitsPerBlock, kUnitsPerBlock, kUnitsPerBlock)});
	if (TerrainHasSolids()) {
		Vector lo(std::fmin(start.x, end.x) + kHullMins.x - 1, std::fmin(start.y, end.y) + kHullMins.y - 1, std::fmin(start.z, end.z) + kHullMins.z - 1);
		Vector hi(std::fmax(start.x, end.x) + kHullMaxs.x + 1, std::fmax(start.y, end.y) + kHullMaxs.y + 1, std::fmax(start.z, end.z) + kHullMaxs.z + 1);
		double ax, ay, az, bx, by, bz;
		SrcToMcD(lo, ax, ay, az);
		SrcToMcD(hi, bx, by, bz);
		int x0 = (int)std::floor(std::fmin(ax, bx)), x1 = (int)std::floor(std::fmax(ax, bx));
		int y0 = (int)std::floor(std::fmin(ay, by)), y1 = (int)std::floor(std::fmax(ay, by));
		int z0 = (int)std::floor(std::fmin(az, bz)), z1 = (int)std::floor(std::fmax(az, bz));
		if ((long)(x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) <= 4096)
			for (int y = y0; y <= y1; ++y)
				for (int z = z0; z <= z1; ++z)
					for (int x = x0; x <= x1; ++x)
						if (TerrainIsSolid(x, y, z)) {
							Vector p = McToSrc(x, y, z), q = McToSrc(x + 1, y + 1, z + 1);
							cands.push_back(Box{Vector(std::fmin(p.x, q.x), std::fmin(p.y, q.y), std::fmin(p.z, q.z)),
							                    Vector(std::fmax(p.x, q.x), std::fmax(p.y, q.y), std::fmax(p.z, q.z))});
						}
	}
	// (bridge #47) the virtual region's six invisible walls (Portal 2's world is void there: nothing else keeps Chell inside it)
	{
		const Vector lo = Vector(std::fmin(start.x, end.x), std::fmin(start.y, end.y), std::fmin(start.z, end.z)) + kHullMins - Vector(1, 1, 1);
		const Vector hi = Vector(std::fmax(start.x, end.x), std::fmax(start.y, end.y), std::fmax(start.z, end.z)) + kHullMaxs + Vector(1, 1, 1);
		Vector wmn[6], wmx[6];
		const int nw = StageWallBoxes(lo, hi, wmn, wmx);
		for (int i = 0; i < nw; ++i) cands.push_back(Box{wmn[i], wmx[i]});
	}
	// (bridge #33) Next to a hole the Minecraft player dug, the engine's world still has the wall that was dug away. There the
	// world is the exported voxel field (the host's solid minus the dug cells), swept like blocks; DigEngineFilter has already
	// cleared the engine's own world hit.
	if (DigAny()) {
		const Vector lo = Vector(std::fmin(start.x, end.x), std::fmin(start.y, end.y), std::fmin(start.z, end.z)) + kHullMins - Vector(1, 1, 1);
		const Vector hi = Vector(std::fmax(start.x, end.x), std::fmax(start.y, end.y), std::fmax(start.z, end.z)) + kHullMaxs + Vector(1, 1, 1);
		if (DigNear(lo, hi, kDigNearPad)) {
			static std::vector<std::pair<Vector, Vector>> vox;
			bool complete = true;
			const bool reuse = g_mergeVoxValid && (lo - g_mergeLo).Length() < 0.001f && (hi - g_mergeHi).Length() < 0.001f;
			const auto &list = reuse ? g_mergeVox : vox;
			if (reuse || (CollisionFieldVoxelBoxes(lo, hi, vox, &complete) && complete))
				for (auto &v : list) cands.push_back(Box{v.first, v.second, kVoxelSkin});
		}
	}
	for (auto &cand : cands) {
		// While the hull is in a portal hole, the hole is carved out of the blocks
		// (the rest of each block, the wall around the opening, stays solid).
		Box parts[36];
		int np = 1;
		parts[0] = cand;
		for (int i = 0; i < 2; ++i) {
			if (!inHole[i]) continue;
			Box next[36];
			int nn = 0;
			for (int k = 0; k < np; ++k) nn += Subtract(parts[k], holes[i], next + nn);
			std::memcpy(parts, next, sizeof(Box) * nn);
			np = nn;
		}
		for (int k = 0; k < np; ++k) {
			float t = 1;
			Vector n;
			bool ss = false;
			if (!SweepBox(start, d, parts[k], t, n, ss)) continue;
			if (g_debug > 0) {
				--g_debug;
				console->Print("[bridge] col dbg start (%.2f %.2f %.2f) end (%.2f %.2f %.2f) inHole %d%d part (%.1f %.1f %.1f)-(%.1f %.1f %.1f) t %.3f n (%g %g %g) ss %d\n",
				               start.x, start.y, start.z, end.x, end.y, end.z, inHole[0], inHole[1], parts[k].mn.x, parts[k].mn.y, parts[k].mn.z, parts[k].mx.x,
				               parts[k].mx.y, parts[k].mx.z, t, n.x, n.y, n.z, ss);
			}
			if (ss) {
				startSolid = true;
				frac = 0;
				return true;
			}
			// Back off like the engine does (DIST_EPSILON 1/32 u).
			t = std::fmax(0.0f, t - (len > 0 ? 0.03125f / len : 0));
			if (t < frac) {
				frac = t;
				normal = n;
				hit = true;
			}
		}
	}
	return hit;
}

// How far Chell's centre is behind an open portal's plane while over its opening (the hole carve lets her there;
// Portal 2 teleports her when her centre crosses the plane, so more than a hull's half-height means she is sinking
// in the carve without a transit). 0 when not behind any.
float BehindPortalDepth(const Vector &origin) {
	float worst = 0;
	for (int i = 0; i < kPortalSlots; ++i) {
		if (!g_portalSet[i]) continue;
		const Vector c = origin + Vector(0, 0, 36);
		const float d = Vector::DotProduct(c - g_portalPos[i], g_portalNormal[i]);
		if (d >= 0 || d < -520.0f) continue;
		Vector f = g_portalNormal[i], up = g_portalUp[i];
		if (std::fabs(Vector::DotProduct(up, f)) > 0.5f) up = std::fabs(f.z) > 0.7f ? Vector(1, 0, 0) : Vector(0, 0, 1);
		Vector r(f.y * up.z - f.z * up.y, f.z * up.x - f.x * up.z, f.x * up.y - f.y * up.x);
		const Vector q = c - g_portalPos[i];
		const float lx = Vector::DotProduct(q, r) / 48.0f, ly = Vector::DotProduct(q, up) / 80.0f;  // (the opening + a hull)
		if (lx * lx + ly * ly <= 1.0f) worst = std::fmax(worst, -d);
	}
	return worst;
}

// The standing hull at origin penetrates a block (deeper than the contact skin) outside the open
// portals' holes, with block collision on.
bool BlockHullStuck(const Vector &origin) {
	if (!g_enabled || (g_blocks.empty() && !TerrainHasSolids())) return false;
	float frac = 1;
	Vector n;
	bool ss = false;
	SweepBlocks(origin, origin, frac, n, ss);
	return ss;
}

// The standing hull at origin, moved by d, hits a block on the way (or starts in one): the exit check after a
// portal transit (puppet.cpp ExitRecentre).
bool BlockHullBlocked(const Vector &origin, const Vector &d) {
	if (!g_enabled || (g_blocks.empty() && !TerrainHasSolids())) return false;
	float frac = 1;
	Vector n;
	bool ss = false;
	const bool hit = SweepBlocks(origin, origin + d, frac, n, ss);
	return ss || (hit && frac < 1.0f);
}

bool PlayerNoclip();  // puppet.cpp
// (bridge #33) The hull is near a dug cell: the engine's hit on the world (or its start inside the world) is no answer there, because the
// brush it hit is the wall that was dug away. A hit on an entity (a prop, a cube, a mover) stands; the world is swept from the exported
// voxels instead (SweepBlocks). Only when every region the hull can touch has been exported; else the engine's answer stays.
static bool DigEngineFilter(const Vector &start, const Vector &end, CGameTrace &tr, void *world) {
	if (!DigAny()) return false;
	const Vector lo = Vector(std::fmin(start.x, end.x), std::fmin(start.y, end.y), std::fmin(start.z, end.z)) + kHullMins - Vector(1, 1, 1);
	const Vector hi = Vector(std::fmax(start.x, end.x), std::fmax(start.y, end.y), std::fmax(start.z, end.z)) + kHullMaxs + Vector(1, 1, 1);
	if (!DigNear(lo, hi, kDigNearPad)) return false;
	bool complete = true;
	g_mergeVoxValid = false;
	if (!CollisionFieldVoxelBoxes(lo, hi, g_mergeVox, &complete) || !complete) {
		// (bridge #35) the engine's world, which still has the dug floor, stays the answer: say so (dig2: Chell never sank, silently)
		static int said = 0;
		static double lastSay = -100;
		const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		if (said < 12 && now - lastSay > 2.0) {
			++said, lastSay = now;
			int rx, ry, rz;
			CollisionFieldVoxelMissing(rx, ry, rz);
			console->Print("[bridge] dig: Chell's hull at (%.0f %.0f %.0f) is next to a dug cell but the exported voxels around it are not complete (region MC %d %d %d not sent; harvest %s): the engine's world keeps holding her\n", start.x, start.y,
			               start.z, rx, ry, rz, CollisionFieldHarvestActive() ? "ready" : "NOT ready");
		}
		return false;
	}
	g_mergeLo = lo, g_mergeHi = hi, g_mergeVoxValid = true;
	const bool entityHit = (tr.fraction < 1.0f || tr.startsolid) && tr.m_pEnt && tr.m_pEnt != world;
	if (!entityHit) {
		tr.fraction = 1.0f;
		tr.endpos = end;
		tr.startsolid = tr.allsolid = false;
		tr.contents = 0;
		tr.m_pEnt = nullptr;
		std::memset(&tr.plane, 0, sizeof tr.plane);
		tr.surface.name = "";
		tr.surface.flags = 0;
		tr.surface.surfaceProps = 0;
	}
	return true;
}
// (bridge #47) Chell in the virtual region (stage.cpp): Portal 2's own world is the void there (solid everywhere to a trace), and the Minecraft stage
// is the ground. As next to a dug cell, the engine's answer about the world is dropped (an entity's hit stands) and the hull is swept against the
// stage's blocks and the box's walls instead.
static bool StageEngineFilter(const Vector &start, const Vector &end, CGameTrace &tr, void *world) {
	const Vector lo = Vector(std::fmin(start.x, end.x), std::fmin(start.y, end.y), std::fmin(start.z, end.z)) + kHullMins - Vector(1, 1, 1);
	const Vector hi = Vector(std::fmax(start.x, end.x), std::fmax(start.y, end.y), std::fmax(start.z, end.z)) + kHullMaxs + Vector(1, 1, 1);
	if (!StageHullNear(lo, hi, 0.0f)) return false;
	const bool entityHit = (tr.fraction < 1.0f || tr.startsolid) && tr.m_pEnt && tr.m_pEnt != world;
	if (!entityHit) {
		tr.fraction = 1.0f;
		tr.endpos = end;
		tr.startsolid = tr.allsolid = false;
		tr.contents = 0;
		tr.m_pEnt = nullptr;
		std::memset(&tr.plane, 0, sizeof tr.plane);
		tr.surface.name = "";
		tr.surface.flags = 0;
		tr.surface.surfaceProps = 0;
	}
	return true;
}
static void MergeImpl(const Vector &start, const Vector &end, CGameTrace &tr, void *world, bool server) {
	if (g_traceLog > 0 && server) {
		--g_traceLog;
		console->Print("[bridge] trace (%.2f %.2f %.2f)->(%.2f %.2f %.2f) game frac %.3f ss %d n (%.2f %.2f %.2f)\n", start.x, start.y, start.z, end.x, end.y, end.z, tr.fraction,
		               tr.startsolid, tr.plane.normal.x, tr.plane.normal.y, tr.plane.normal.z);
	}
	// noclip flies through blocks like through Portal 2's own world
	if (!g_enabled || PlayerNoclip()) return;
	const bool dug = DigEngineFilter(start, end, tr, world) || StageEngineFilter(start, end, tr, world);
	if (!dug && ((g_blocks.empty() && !TerrainHasSolids()) || tr.startsolid)) return;
	if (tr.startsolid) return;  // (an entity has the hull)
	float frac = tr.fraction;
	Vector n;
	bool ss;
	const bool swept = SweepBlocks(start, end, frac, n, ss);
	g_mergeVoxValid = false;
	if (!swept) return;
	++g_blockHits;
	tr.fraction = frac;
	tr.endpos = start + (end - start) * frac;
	tr.startsolid = ss;
	tr.allsolid = ss;
	if (!ss) {
		tr.plane.normal = n;
		tr.plane.dist = tr.endpos.Dot(n);
		tr.plane.type = std::fabs(n.x) > 0.5f ? 0 : std::fabs(n.y) > 0.5f ? 1 : 2;
	}
	tr.contents = CONTENTS_SOLID;
	tr.surface.name = "**blocks**";
	tr.surface.flags = 0;
	tr.surface.surfaceProps = 0;
	if (world) tr.m_pEnt = world;
}

static void Merge(const Vector &start, const Vector &end, CGameTrace &tr, void *world, bool server) {
	const bool logIt = server && g_traceLog > 0;
	MergeImpl(start, end, tr, world, server);
	g_mergeVoxValid = false;
	if (logIt) console->Print("[bridge]   -> after the blocks: frac %.3f ss %d n (%.2f %.2f %.2f) end z %.2f\n", tr.fraction, tr.startsolid, tr.plane.normal.x, tr.plane.normal.y, tr.plane.normal.z, tr.endpos.z);  // (the voxel list of this trace is not reused by another call)
}

// ---- blocker log (bridge #12: "carrying a cube into Minecraft, it's like there are invisible blocks") ----
// Every hull trace of Chell's movement that ends on a wall (|normal z| < 0.7, or starts solid) is
// attributed: a real Minecraft block (our sweep won), Portal 2's world, one of the prop collider pool's
// entities (propcollide.cpp), or any other entity (logged by class and name). The client's prediction
// traces are attributed the same way (client entities: a collider is recognised by position).
bool PropColliderIs(void *ent);            // propcollide.cpp
bool PropColliderNear(const Vector &pos);  // propcollide.cpp
enum { kHitBlock, kHitWorld, kHitCollider, kHitPortalEnv, kHitOther, kHitN };
static const char *kHitName[kHitN] = {"block", "world", "collider", "portalenv", "other"};
static int g_hitCount[2][kHitN];      // [server, client]
static int g_hitTick[kHitN];          // server hits in the current movement tick
static int g_whoLog = 0;              // pcb_blockers log N [blocks]: log the next N non-block (or all) wall hits
static bool g_whoBlocks = false;
static void Attribute(const Vector &start, const Vector &end, const CGameTrace &tr, void *world, bool server) {
	if (!(tr.fraction < 1.0f) && !tr.startsolid) return;
	if (!tr.startsolid && std::fabs(tr.plane.normal.z) >= 0.7f) return;  // ground / ceiling
	if ((end - start).Length() < 0.01f && !tr.startsolid) return;
	int k;
	const bool block = tr.surface.name && std::strcmp(tr.surface.name, "**blocks**") == 0;
	if (block) k = kHitBlock;
	else if (tr.m_pEnt == world || !tr.m_pEnt) k = kHitWorld;
	else if (server ? PropColliderIs(tr.m_pEnt) : PropColliderNear(tr.endpos)) k = kHitCollider;
	else if (server && EntKV(tr.m_pEnt, "classname") == "portalsimulator_collisionentity") k = kHitPortalEnv;  // Portal 2's carved world around a portal
	else k = kHitOther;
	++g_hitCount[server ? 0 : 1][k];
	if (server) ++g_hitTick[k];
	if ((k != kHitBlock || g_whoBlocks) && g_whoLog > 0) {
		--g_whoLog;
		std::string who = "?";
		if (server && tr.m_pEnt && tr.m_pEnt != world) who = EntKV(tr.m_pEnt, "classname") + " '" + EntKV(tr.m_pEnt, "targetname") + "' at " + EntKV(tr.m_pEnt, "origin");
		console->Print("[bridge] BLOCKED (%s) by %s %s: (%.1f %.1f %.1f)->(%.1f %.1f %.1f) frac %.3f ss %d n (%.2f %.2f %.2f) end (%.1f %.1f %.1f)\n", server ? "server" : "client", kHitName[k],
		               who.c_str(), start.x, start.y, start.z, end.x, end.y, end.z, tr.fraction, tr.startsolid, tr.plane.normal.x, tr.plane.normal.y, tr.plane.normal.z, tr.endpos.x, tr.endpos.y,
		               tr.endpos.z);
	}
}

static void __rescall SvTrace_Hook(void *thisptr, const Vector &start, const Vector &end, unsigned mask, int group, CGameTrace &tr) {
	SvTracePlayerBBox(thisptr, start, end, mask, group, tr);
	Merge(start, end, tr, g_svWorld, true);
	Attribute(start, end, tr, g_svWorld, true);
}
static void __rescall ClTrace_Hook(void *thisptr, const Vector &start, const Vector &end, unsigned mask, int group, CGameTrace &tr) {
	ClTracePlayerBBox(thisptr, start, end, mask, group, tr);
	Merge(start, end, tr, g_clWorld, false);
	Attribute(start, end, tr, g_clWorld, false);
}

// Speed watch: per server movement tick, Chell's horizontal speed in and out and the walls her traces hit.
// A ground tick (no vertical speed in or out) whose speed dropped by > 15 u/s with movement input held along the
// motion (wish velocity within ~45 deg of it) is a "cut", attributed to what was hit that tick (a cut with no wall hit is "unexplained"); a position changed
// between two moves (something outside the movement code pushed her: a physics shadow) is counted too.
// The minimum speed is taken only inside a speed window (pcb_blockers speedwin 1), after 45 ticks of held
// input with no block wall hit for 30 (flat stretches of a set path).
static bool g_watch = false, g_speedWin = false;
static int g_watchTicks = 0, g_cuts[kHitN + 1], g_corr = 0, g_corrLog = 0, g_inputRun = 0, g_sinceBlock = 0;
static float g_minSpeed = 1e9f, g_corrMax = 0;
static Vector g_minAt;
Vector PlayerOrigin();  // puppet.cpp
// (bridge #49) The world entity is what a world-only trace hits, found at level start from the camera: in a campaign chamber (the mirror world) the camera is not
// yet anywhere valid in that first frame and the answer was null for the whole level, which switched off everything that needs it (the ground on blocks, what
// counts as an entity hit). Asked again, from the player's feet, until it is known.
static void ResolveWorldEntity() {
	static int tick = 0;
	if ((g_svWorld && g_clWorld) || ++tick % 30 != 1) return;
	const Vector p = PlayerOrigin() + Vector(0, 0, 36);
	if (p.Length() < 1.0f) return;
	CGameTrace tr;
	engine.Trace(p, p - Vector(0, 0, 8192), Vector(0, 0, 0), MASK_SOLID_BRUSHONLY, tr, true);
	if (!g_svWorld && tr.m_pEnt && EntKV(tr.m_pEnt, "classname") == "worldspawn") g_svWorld = tr.m_pEnt;  // (a platform under her feet is not the world)
	if (g_svWorld && !g_clWorld && engine.traceClient) {
		CGameTrace ct;
		engine.TraceClient(p, p - Vector(0, 0, 8192), MASK_SOLID_BRUSHONLY, ct);
		if (ct.m_pEnt) g_clWorld = ct.m_pEnt;
	}
	if (g_svWorld) console->Print("[bridge] block collision: world entity found from the player's feet: server %p client %p\n", g_svWorld, g_clWorld);
}
void BlockWatchTickBegin() {
	std::memset(g_hitTick, 0, sizeof g_hitTick);
	ResolveWorldEntity();
}
void BlockWatchTickEnd(const Vector &velIn, CMoveData *mv, const Vector &before, const Vector &lastOut, bool noclip) {
	if (!g_watch || noclip) return;
	++g_watchTicks;
	const float in = std::sqrt(velIn.x * velIn.x + velIn.y * velIn.y), out = std::sqrt(mv->m_vecVelocity.x * mv->m_vecVelocity.x + mv->m_vecVelocity.y * mv->m_vecVelocity.y);
	const bool input = std::fabs(mv->m_flForwardMove) > 1 || std::fabs(mv->m_flSideMove) > 1;
	const bool ground = std::fabs(velIn.z) < 1 && std::fabs(mv->m_vecVelocity.z) < 1;
	// the window's run counts full-strength input only: the first usercmds after +forward carry part of the key
	// (fwd 88 of 175); in a throttled run that first long frame covered many ticks at half speed and the window
	// opened while she was still accelerating from 88 (bridge #12's deep1 y 58: 140 u/s, bridge #13)
	const bool fullInput = mv->m_flForwardMove * mv->m_flForwardMove + mv->m_flSideMove * mv->m_flSideMove > 150.0f * 150.0f;
	g_inputRun = fullInput ? g_inputRun + 1 : 0;
	g_sinceBlock = g_hitTick[kHitBlock] ? 0 : g_sinceBlock + 1;
	if (g_speedWin && g_inputRun > 45 && g_sinceBlock > 30 && out < g_minSpeed) g_minSpeed = out, g_minAt = mv->m_vecAbsOrigin;
	// (only while the input pushes along the motion: input against it, e.g. a view turned by a portal transit, brakes)
	const Vector &w = mv->m_outWishVel;
	const float wl = std::sqrt(w.x * w.x + w.y * w.y), along = wl > 1 && in > 1 ? (w.x * velIn.x + w.y * velIn.y) / (wl * in) : 0;
	if (input && ground && along > 0.7f && out < in - 15.0f) {
		int k = kHitN;
		for (int c : {kHitOther, kHitCollider, kHitPortalEnv, kHitWorld, kHitBlock})
			if (g_hitTick[c]) {
				k = c;
				break;
			}
		++g_cuts[k];
		if (k != kHitBlock && g_corrLog < 30)
			++g_corrLog, console->Print("[bridge] SPEED CUT %.0f -> %.0f u/s at (%.1f %.1f %.1f) by %s\n", in, out, mv->m_vecAbsOrigin.x, mv->m_vecAbsOrigin.y, mv->m_vecAbsOrigin.z,
			                            k < kHitN ? kHitName[k] : "nothing hit (no wall this tick)");
	}
	const float dc = (before - lastOut).Length();
	if (dc > 0.05f && dc < 64.0f && lastOut.Length() > 0.1f) {
		++g_corr, g_corrMax = std::fmax(g_corrMax, dc);
		if (g_corrLog < 30)
			++g_corrLog, console->Print("[bridge] MOVED BETWEEN TICKS by %.2f u: (%.1f %.1f %.1f) -> (%.1f %.1f %.1f)\n", dc, lastOut.x, lastOut.y, lastOut.z, before.x, before.y, before.z);
	}
}
static void Summary(char *buf, size_t n, const char *label) {
	std::snprintf(buf, n,
	              "%s: wall hits sv block %d world %d collider %d portalenv %d other %d | cl block %d world %d collider %d other %d; ground speed cuts block %d world %d collider %d portalenv %d "
	              "other %d unexplained %d; moved between ticks %d (max %.2f u); min speed in windows %.0f at (%.0f %.0f %.0f) over %d ticks",
	              label, g_hitCount[0][0], g_hitCount[0][1], g_hitCount[0][2], g_hitCount[0][3], g_hitCount[0][4], g_hitCount[1][0], g_hitCount[1][1], g_hitCount[1][2], g_hitCount[1][4], g_cuts[0],
	              g_cuts[1], g_cuts[2], g_cuts[3], g_cuts[4], g_cuts[5], g_corr, g_corrMax, g_minSpeed > 1e8f ? -1.0f : g_minSpeed, g_minAt.x, g_minAt.y, g_minAt.z, g_watchTicks);
}
CON_COMMAND(pcb_blockers, "pcb_blockers [log N] | reset | report <label> | watch 0/1 | speedwin 0/1 - what Chell's movement traces hit (block / world / collider / portalenv / other), speed cuts\n") {
	const char *a = args.ArgC() > 1 ? args[1] : "report";
	if (!std::strcmp(a, "log")) {
		g_whoLog = args.ArgC() > 2 ? std::atoi(args[2]) : 40;
		g_whoBlocks = args.ArgC() > 3 && std::atoi(args[3]);
		return;
	}
	if (!std::strcmp(a, "watch")) return void(g_watch = args.ArgC() > 2 && std::atoi(args[2]) != 0);
	if (!std::strcmp(a, "speedwin")) return void(g_speedWin = args.ArgC() > 2 && std::atoi(args[2]) != 0);
	if (!std::strcmp(a, "reset")) {
		std::memset(g_hitCount, 0, sizeof g_hitCount), std::memset(g_cuts, 0, sizeof g_cuts);
		g_corr = 0, g_corrMax = 0, g_watchTicks = 0, g_minSpeed = 1e9f, g_corrLog = 0;
		return;
	}
	char buf[600];
	Summary(buf, sizeof buf, args.ArgC() > 2 ? args[2] : "");
	console->Print("[bridge] blockers %s\n", buf);
}
void TestCheck(bool ok, const char *what, const char *detail);  // testassert.cpp
// pcb_assert_blockers <label> [min_speed] [allow]: since the reset nothing but real blocks stopped Chell: no collider /
// other-entity wall hits on either side, no ground speed cut not explained by a block, no between-tick push, and the
// minimum speed inside the speed windows >= min_speed. allow: 1 = Portal 2's world and its portal environment may
// also stop her (a chamber, a portal's rim).
CON_COMMAND(pcb_assert_blockers, "pcb_assert_blockers <label> [min_speed=0] [allow_p2=0] - only real blocks stopped Chell since pcb_blockers reset\n") {
	const float minSpeed = args.ArgC() > 2 ? (float)std::atof(args[2]) : 0.0f;
	const bool allow = args.ArgC() > 3 && std::atoi(args[3]);
	char buf[700];
	Summary(buf, sizeof buf - 40, args.ArgC() > 1 ? args[1] : "");
	std::snprintf(buf + std::strlen(buf), 40, " (want >= %.0f)", minSpeed);
	const int p2 = g_hitCount[0][kHitWorld] + g_hitCount[1][kHitWorld] + g_hitCount[0][kHitPortalEnv] + g_cuts[kHitWorld] + g_cuts[kHitPortalEnv];
	const bool ok = g_hitCount[0][kHitCollider] + g_hitCount[0][kHitOther] + g_hitCount[1][kHitCollider] + g_hitCount[1][kHitOther] == 0 && (allow || p2 == 0) &&
	                g_cuts[kHitCollider] + g_cuts[kHitOther] + g_cuts[kHitN] == 0 && g_corr == 0 && (g_minSpeed > 1e8f ? minSpeed <= 0 : g_minSpeed >= minSpeed);
	TestCheck(ok, "only real blocks stop Chell", buf);
}

// Ground on blocks. Portal 2 finds the ground with its own engine trace (not TracePlayerBBox),
// so it never sees a block under Chell: she was treated as airborne on blocks (air control
// only, no friction, no jumping; run pit7). Every movement tick ends in SetGroundEntity
// (CGameMovement vtable slot per slots.hpp: 70 on Linux, per SourceAutoRecord's offsets), with no trace when
// the game found no ground. The hook turns that into "standing on the world" when a 2 u hull
// sweep down hits a block top, unless the player is moving up faster than a step (a jump).
using _SetGroundEntity = void(__rescall *)(void *thisptr, CGameTrace *pm);
static _SetGroundEntity SvSetGroundEntity = nullptr, ClSetGroundEntity = nullptr;
static int g_groundHits = 0;
static int g_jumpTrace = 0;
// Portal 2's CheckJumpButton leaves the ground (SetGroundEntity(null)) before it adds the jump
// velocity; the block-ground override must not put Chell straight back on the ground then.
static bool g_inCheckJump = false;

static bool BlockGround(void *thisptr, void *world, CGameTrace &out, float reach = 2.0f) {
	if (!g_enabled || !world || PlayerNoclip() || (g_blocks.empty() && !TerrainHasSolids() && !DigAny())) return false;
	auto *mv = *reinterpret_cast<CMoveData **>(reinterpret_cast<char *>(thisptr) + 8);
	if (!mv || mv->m_vecVelocity.z > 140.0f) return false;
	Vector start = mv->m_vecAbsOrigin, end = start - Vector(0, 0, reach);
	// Not while passing through a portal: the lip of the wall under the opening would count as
	// ground for a tick or two and ground friction would eat the fling speed (run pit8: 919 -> 690).
	Box hull{start + kHullMins, start + kHullMaxs};
	for (int i = 0; i < 2; ++i)
		if (g_portalSet[i] && Overlap(hull, HoleBox(i)) && CentreOverOpening(start, i)) return false;
	float frac = 1;
	Vector n;
	bool ss = false;
	if (!SweepBlocks(start, end, frac, n, ss) || ss || n.z < 0.7f) return false;
	// stand on the block (Portal 2 snaps to the ground it finds; without this Chell hovered up to 2 u)
	mv->m_vecAbsOrigin = start + (end - start) * frac;
	std::memset(&out, 0, sizeof out);
	out.startpos = start;
	out.endpos = start + (end - start) * frac;
	out.fraction = frac;
	out.plane.normal = n;
	out.plane.dist = out.endpos.Dot(n);
	out.plane.type = 2;
	out.contents = CONTENTS_SOLID;
	out.surface.name = "**blocks**";
	out.m_pEnt = world;
	return true;
}

// (bridge #36) Chell never sank into a dug hole (dig3, dig4): her ground is found by Portal 2's own trace, which sees the engine's world, and the engine's
// world still has the floor that was dug away. DigEngineFilter only cleaned her movement traces, so each tick her ground state said "standing on the
// world" (no gravity), and a jump landed her back on the phantom floor (z 0.0 exactly, velocity 0). Next to a dug cell the engine's world is not the
// answer for the ground either: when it reports the world as the ground, the exported voxels (the world minus the dug cells, as for the movement
// traces) are asked instead: 1 = the ground is there (`t`: the voxel top, the feet snapped to it), 2 = nothing under her (airborne), 0 = not near a hole
// (or the voxels around are not all exported yet): the engine's answer stands.
static bool DigNearComplete(const Vector &start, const Vector &end) {
	const Vector lo = Vector(std::fmin(start.x, end.x), std::fmin(start.y, end.y), std::fmin(start.z, end.z)) + kHullMins - Vector(1, 1, 1);
	const Vector hi = Vector(std::fmax(start.x, end.x), std::fmax(start.y, end.y), std::fmax(start.z, end.z)) + kHullMaxs + Vector(1, 1, 1);
	if (!DigNear(lo, hi, kDigNearPad)) return false;
	static std::vector<std::pair<Vector, Vector>> vox;
	bool complete = true;
	return CollisionFieldVoxelBoxes(lo, hi, vox, &complete) && complete;
}
static int g_digGround = 0, g_digAir = 0;
static int DigWorldGround(void *thisptr, void *world, CGameTrace *pm, CGameTrace &t, bool server) {
	if (!g_enabled || !world || !pm || pm->m_pEnt != world || !DigAny() || PlayerNoclip()) return 0;
	auto *mv = *reinterpret_cast<CMoveData **>(reinterpret_cast<char *>(thisptr) + 8);
	if (!mv || !DigNearComplete(mv->m_vecAbsOrigin, mv->m_vecAbsOrigin - Vector(0, 0, 3.5f))) return 0;
	const Vector at = mv->m_vecAbsOrigin;
	const bool ground = BlockGround(thisptr, world, t, 3.5f);  // (the voxels' tops sit up to 2.5 u from the engine's exact surfaces)
	if (server) {
		++(ground ? g_digGround : g_digAir);
		static int logged = 0;
		static bool lastGround = true;
		if (ground != lastGround && logged < 30)
			++logged, console->Print("[bridge] dig: Chell at (%.1f %.1f %.1f) next to a dug cell: the engine says it stands on its world, the exported voxels say %s\n", at.x, at.y, at.z,
			                         ground ? "there is ground (she stands on it)" : "there is none (she falls)");
		lastGround = ground;
	}
	return ground ? 1 : 2;
}
// (bridge #49) In the virtual region (stage.cpp) the engine's world is void, solid everywhere: its own ground check finds "the world" under Chell (or a plane too
// steep to stand on) wherever she is, so she hovered with no friction and coasted at walking speed for ever. There the ground is the stage's blocks only.
static int StageWorldGround(void *thisptr, void *world, CGameTrace &t) {
	if (!g_map.stage.on || !g_enabled || !world || PlayerNoclip()) return 0;
	auto *mv = *reinterpret_cast<CMoveData **>(reinterpret_cast<char *>(thisptr) + 8);
	if (!mv || !StageHullNear(mv->m_vecAbsOrigin + kHullMins, mv->m_vecAbsOrigin + kHullMaxs, 8.0f)) return 0;
	const bool g = BlockGround(thisptr, world, t, 3.5f);
	return g ? 1 : 2;
}
static void __rescall SvSetGround_Hook(void *thisptr, CGameTrace *pm) {
	CGameTrace t;
	if (!g_inCheckJump) {
		int dg = DigWorldGround(thisptr, g_svWorld, pm, t, true);
		if (!dg) dg = StageWorldGround(thisptr, g_svWorld, t);
		if (g_traceLog > 0) console->Print("[bridge]   SetGroundEntity: engine pm %p ent %p, dig answer %d (0 none, 1 ground, 2 air)\n", pm, pm ? pm->m_pEnt : nullptr, dg);
		if (dg) return SvSetGroundEntity(thisptr, dg == 1 ? &t : nullptr);
	}
	if (g_jumpTrace > 0) {
		--g_jumpTrace;
		auto *mv = *reinterpret_cast<CMoveData **>(reinterpret_cast<char *>(thisptr) + 8);
		console->Print("[bridge] jump trace: SetGroundEntity pm %p ent %p, origin z %.2f vz %.1f\n", pm, pm ? pm->m_pEnt : nullptr, mv ? mv->m_vecAbsOrigin.z : 0.f,
		               mv ? mv->m_vecVelocity.z : 0.f);
	}
	if (!g_inCheckJump && (!pm || !pm->m_pEnt)) {
		const bool bg = BlockGround(thisptr, g_svWorld, t);
		if (g_traceLog > 0) console->Print("[bridge]   BlockGround: %d\n", bg);
		if (bg) {
			++g_groundHits;
			return SvSetGroundEntity(thisptr, &t);
		}
	}
	SvSetGroundEntity(thisptr, pm);
}
static void __rescall ClSetGround_Hook(void *thisptr, CGameTrace *pm) {
	CGameTrace t;
	if (!g_inCheckJump) {
		int dg = DigWorldGround(thisptr, g_clWorld, pm, t, false);
		if (!dg) dg = StageWorldGround(thisptr, g_clWorld, t);
		if (dg) return ClSetGroundEntity(thisptr, dg == 1 ? &t : nullptr);
	}
	if (!g_inCheckJump && (!pm || !pm->m_pEnt) && BlockGround(thisptr, g_clWorld, t)) return ClSetGroundEntity(thisptr, &t);
	ClSetGroundEntity(thisptr, pm);
}

// Jumping off blocks. Portal 2's CheckJumpButton (CGameMovement slot 37 on Linux, SourceAutoRecord)
// refuses to jump when its own ground state says "airborne", which it can for a player standing on
// blocks (run pit9: +jump on terrain did nothing). If it refuses a fresh press while a 2 u sweep
// finds a block top under the feet, the hook performs Portal 2's own jump (research/specs/
// portal-movement.md section 4): v.z += sqrt(sv_gravity * 90), half a gravity step (FinishGravity,
// slot kMoveFinishGravity), mark the button held, leave the ground.
using _CheckJumpButton = bool(__rescall *)(void *thisptr);
using _FinishGravity = void(__rescall *)(void *thisptr);
static _CheckJumpButton SvCheckJump = nullptr, ClCheckJump = nullptr;
static int g_jumpLog = 6, g_blockJumps = 0;

// (bridge #70) the last fresh jump press Portal 2's SERVER move saw and whether the game took it (playerevents.cpp checks that a relayed jump arrived)
static double g_svJumpSeenAt = -1;
static bool g_svJumpTook = false;
bool SvJumpSeen(double &at, bool &took) {
	at = g_svJumpSeenAt, took = g_svJumpTook;
	return g_svJumpSeenAt >= 0;
}
static bool JumpHook(void *thisptr, _CheckJumpButton orig, _SetGroundEntity setGround, void *world, bool server) {
	auto *mv = *reinterpret_cast<CMoveData **>(reinterpret_cast<char *>(thisptr) + 8);
	bool fresh = mv && (mv->m_nButtons & IN_JUMP) && !(mv->m_nOldButtons & IN_JUMP);
	CGameTrace t;
	bool onBlocks = fresh && BlockGround(thisptr, world, t);
	g_inCheckJump = true;
	bool r = orig(thisptr);
	g_inCheckJump = false;
	if (fresh && server) {
		using namespace std::chrono;
		g_svJumpSeenAt = duration<double>(steady_clock::now().time_since_epoch()).count(), g_svJumpTook = r || onBlocks;
	}
	if (fresh && server && r) g_jumpTrace = 4;
	if (fresh && server && g_jumpLog > 0) {
		--g_jumpLog;
		console->Print("[bridge] jump: game %d, on blocks %d, velocity z %.1f\n", r, onBlocks, mv->m_vecVelocity.z);
	}
	if (r || !onBlocks) return r;
	mv->m_vecVelocity.z += 232.379f;  // sqrt(600 * 90)
	auto fg = (*reinterpret_cast<_FinishGravity **>(thisptr))[slots::Index(slots::kMoveFinishGravity)];
	fg(thisptr);
	mv->m_outJumpVel.z += 232.379f;
	mv->m_outStepHeight += 0.15f;
	mv->m_nOldButtons |= IN_JUMP;
	setGround(thisptr, nullptr);
	if (server) ++g_blockJumps;
	return true;
}
static bool __rescall SvCheckJump_Hook(void *thisptr) { return JumpHook(thisptr, SvCheckJump, SvSetGroundEntity, g_svWorld, true); }
static bool __rescall ClCheckJump_Hook(void *thisptr) { return JumpHook(thisptr, ClCheckJump, ClSetGroundEntity, g_clWorld, false); }

// (bridge #44) Why Chell could not walk through a hole dug in a wall. Her movement was already free there (the hull traces above read the exported voxels
// next to a dug cell), and she walked into the hole; then, between two movement ticks, she was put back at the wall face, every tick (the log's "MOVED
// BETWEEN TICKS": x -524.8 -> -528.4, z +0.5). That is Portal 2's physics shadow of the player (a vphysics hull that follows her): it collides with the
// ENGINE's world, which still has the wall that was dug away, so it stays at the wall face; when it differs from the player's position the game checks the
// player's own position against the engine's world (not our hooks), finds it inside the wall, and moves the player to the shadow. With the shadow's
// collisions off (what the game does itself in noclip) the shadow just follows her and nothing moves her back; sv_turbophysics 1, which skips that update,
// let her through in a test. So: while her hull is near a dug cell the active shadow's collisions are off, and they are put back when she leaves.
// Only the shadow that is active is touched (the game keeps the standing hull's and the crouching hull's shadows, one of them disabled): the one that is
// enabled at the start of a tick outside the zone is remembered as the active one, and the one enabled at the start of a tick inside the zone (the game
// toggles them when she ducks) is switched off.
static bool g_shadowOff = false;
static void *g_shadowSeen[2] = {nullptr, nullptr};
static int g_shadowActive = -1, g_shadowToggles = 0;
using _PhysIsCollisionEnabled = bool(__rescall *)(void *thisptr);
using _PhysEnableCollisions = void(__rescall *)(void *thisptr, bool enable);
void ChellShadowFrame(void *player, const Vector &origin, bool noclip) {
	if (!player || !g_enabled || Offsets::m_pShadowStand <= 0) return;
	void *obj[2] = {*reinterpret_cast<void **>(reinterpret_cast<char *>(player) + Offsets::m_pShadowStand), *reinterpret_cast<void **>(reinterpret_cast<char *>(player) + Offsets::m_pShadowCrouch)};
	if (obj[0] != g_shadowSeen[0] || obj[1] != g_shadowSeen[1]) {  // new shadows (a new level, a respawn): the game made them
		g_shadowSeen[0] = obj[0], g_shadowSeen[1] = obj[1];
		g_shadowOff = false, g_shadowActive = -1;
	}
	if (!obj[0] || !obj[1]) return;
	if (noclip) {  // the game turns the shadows off itself and puts them back when noclip ends
		g_shadowOff = false;
		return;
	}
	bool want = false;
	if (DigAny()) {
		const Vector lo = origin + kHullMins - Vector(1, 1, 1), hi = origin + kHullMaxs + Vector(1, 1, 1);
		want = DigNear(lo, hi, kDigNearPad);
	}
	// (bridge #49) the virtual region (stage.cpp) is void in the engine's world, solid everywhere to the shadow: the same rule (the stage's blocks are the ground)
	if (!want && g_map.stage.on) want = StageHullNear(origin + kHullMins, origin + kHullMaxs, 24.0f);
	bool en[2];
	for (int i = 0; i < 2; ++i) en[i] = Memory::VMT<_PhysIsCollisionEnabled>(obj[i], Offsets::IsCollisionEnabled)(obj[i]);
	auto enable = [&](int i, bool on) { Memory::VMT<_PhysEnableCollisions>(obj[i], Offsets::EnableGravity - 1)(obj[i], on); };
	if (want) {
		for (int i = 0; i < 2; ++i)
			if (en[i]) {
				g_shadowActive = i;
				enable(i, false);
				if (!g_shadowOff) {
					++g_shadowToggles;
					static int logged = 0;
					if (logged < 12) ++logged, console->Print("[bridge] dig: Chell is next to a dug cell or in the stage at (%.0f %.0f %.0f): her physics shadow's collisions with the engine's world (which still has the wall) are switched off, so it cannot push her back out of the hole\n", origin.x, origin.y, origin.z);
				}
				g_shadowOff = true;
			}
	} else {
		if (g_shadowOff && g_shadowActive >= 0 && !en[g_shadowActive]) {
			enable(g_shadowActive, true);
			static int logged = 0;
			if (logged < 12) ++logged, console->Print("[bridge] dig: Chell is away from the dug cells at (%.0f %.0f %.0f): her physics shadow collides again\n", origin.x, origin.y, origin.z);
		}
		g_shadowOff = false;
		for (int i = 0; i < 2; ++i)
			if (en[i]) g_shadowActive = i;
	}
}

void *BlockWorldEntity() { return g_svWorld; }

// The world entity on each side = what a world-only trace hits.
void BlockCollideLevelStart() {
	CGameTrace tr;
	Vector p = LastMainView().origin;
	engine.Trace(p, p - Vector(0, 0, 8192), Vector(0, 0, 0), MASK_SOLID_BRUSHONLY, tr, true);
	g_svWorld = tr.m_pEnt;
	if (engine.traceClient) {
		CGameTrace ct;
		engine.TraceClient(p, p - Vector(0, 0, 8192), MASK_SOLID_BRUSHONLY, ct);
		g_clWorld = ct.m_pEnt;
	}
	console->Print("[bridge] block collision: world entity server %p client %p\n", g_svWorld, g_clWorld);
}

bool BlockCollideInit() {
	g_svMove = Interface::Create(MODULE_NAME("server"), "GameMovement001");
	g_clMove = Interface::Create(MODULE_NAME("client"), "GameMovement001");
	if (!g_svMove || !g_clMove) return false;
	// Each hook is independent; one held back by the Windows safe-load policy leaves the others installed (slots.hpp).
	slots::Hook(g_svMove, slots::kMoveTracePlayerBBox, SvTrace_Hook, SvTracePlayerBBox);
	slots::Hook(g_clMove, slots::kMoveTracePlayerBBox, ClTrace_Hook, ClTracePlayerBBox);
	slots::Hook(g_svMove, slots::kMoveSetGroundEntity, SvSetGround_Hook, SvSetGroundEntity);
	slots::Hook(g_clMove, slots::kMoveSetGroundEntity, ClSetGround_Hook, ClSetGroundEntity);
	slots::Hook(g_svMove, slots::kMoveCheckJumpButton, SvCheckJump_Hook, SvCheckJump);
	slots::Hook(g_clMove, slots::kMoveCheckJumpButton, ClCheckJump_Hook, ClCheckJump);
	return true;
}

void BlockCollideShutdown() {
	Interface::Delete(g_svMove);
	Interface::Delete(g_clMove);
	g_svMove = g_clMove = nullptr;
}

CON_COMMAND(pcb_collide_hole_depth, "pcb_collide_hole_depth [u=128] - how deep the portal hole is carved out of Minecraft blocks behind a portal's plane (was 500)\n") {
	if (args.ArgC() > 1) g_holeDepth = std::fmax(40.0f, (float)std::atof(args[1]));
	console->Print("[bridge] portal hole depth %.0f u\n", g_holeDepth);
}
// Chell's hull is behind an open portal's plane over its opening: the portal it is in and her centre's depth behind it.
int BehindPortalWhich(const Vector &origin, float &depth) {
	int which = -1;
	depth = 0;
	for (int i = 0; i < kPortalSlots; ++i) {
		if (!g_portalSet[i]) continue;
		const Vector c = origin + Vector(0, 0, 36);
		const float d = Vector::DotProduct(c - g_portalPos[i], g_portalNormal[i]);
		if (d >= 0 || d < -560.0f) continue;  // (as deep as the old 500 u hole let her sink)
		if (!CentreOverOpening(origin, i) && !CentreOverOpening(origin - g_portalNormal[i] * d, i)) continue;
		if (-d > depth) depth = -d, which = i;
	}
	return which;
}
CON_COMMAND(pcb_collide_seams, "pcb_collide_seams [0|1] - a hull resting up to 0.5 u below a block top is on it: neighbouring blocks' sides are no wall (default 1; 0 = the old behaviour)\n") {
	if (args.ArgC() > 1) g_seamFix = std::atoi(args[1]) != 0;
	console->Print("[bridge] block seam fix %s\n", g_seamFix ? "on" : "off");
}
CON_COMMAND(pcb_collide, "pcb_collide <0|1> [debug_hits] [debug_traces] - player collision with blocks via the movement-trace hook (default 1)\n") {
	if (args.ArgC() > 1) g_enabled = std::atoi(args[1]) != 0;
	if (args.ArgC() > 2) g_debug = std::atoi(args[2]);
	if (args.ArgC() > 3) g_traceLog = std::atoi(args[3]);
	if (!g_svWorld) BlockCollideLevelStart();
	console->Print("[bridge] block collision %s, %d block hits, %d block-ground ticks, %d block jumps so far\n", g_enabled ? "on" : "off", g_blockHits, g_groundHits, g_blockJumps);
}

// (bridge #45) Chell's own hole volumes (the block carve's box, linked portals only) for the exported-voxel rules near a dug cell (collfield.cpp)
int ChellHoleBoxes(Vector *mn, Vector *mx, int max) {
	int n = 0;
	for (int i = 0; i < kPortalSlots && n < max; ++i) {
		if (!PortalLinked(i)) continue;
		const Box b = HoleBox(i);
		mn[n] = b.mn, mx[n] = b.mx, ++n;
	}
	return n;
}

}  // namespace bridge
