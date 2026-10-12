// Portalcraft bridge: Chell's own portal gun shoots portals onto Minecraft blocks.
//
// The gun's placement trace (CWeaponPortalgun::TraceFirePortal in server.so, found with
// SourceAutoRecord's Linux signature; research/specs/portal-placement.md) only sees Portal 2's
// world, so in the Minecraft region it ran into the region's invisible sky walls (PASSTHROUGH) and
// Chell could not place a portal. The hook runs the game's own trace, then marches the same shot
// through Minecraft's solid blocks (the streamed kRenSolids bitsets). When a block face comes
// first, the result is replaced by a placement on that face, fitted the way Portal 2 fits a portal
// (portal-placement.md sections 2.2-5):
//   * orientation: the face normal; wall portals upright, floor/ceiling portals with their top
//     towards the shot's horizontal direction;
//   * the 64 x 112 opening must lie on solid block faces with open air in front of every part of
//     it; otherwise it slides to the nearest spot that fits, at most sqrt(2 (32^2 + 56^2)) = 91.2 u
//     away (Portal 2's bump limit), else CANT_FIT;
//   * floor snap: a wall portal whose bottom edge ends within 3.5 u above a block top sits on it;
//   * the other portal of the pair, facing the same way, may not overlap it (OVERLAP_LINKED).
// Portal 2's FirePortal then does the rest from that result: the real prop_portal moves (both
// colours), opening/closing/fizzle effects and sounds, linkage, and the collision carve and the
// composite follow the live portals.
#include "bridge.hpp"
#include "coords.hpp"
#include "dig.hpp"

#include "Hook.hpp"
#include "Modules/Tier1.hpp"
#include "Offsets.hpp"
#include "Utils/Memory.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace bridge {

bool ClipHasRegion();                               // clip.cpp
bool ClipRegionContains(const Vector &p);
bool TerrainSectionKnown(int bx, int by, int bz);   // terrain.cpp
void *BlockWorldEntity();                           // blockcollide.cpp
void ClipOnGunShot(int colour, bool placed, const Vector &pos);  // clip.cpp
bool TerrainNoPortal(int bx, int by, int bz);       // terrain.cpp
int TerrainFluid(int bx, int by, int bz);
void PuppetLaunch(const Vector &vel);               // puppet.cpp

struct PlacementInfo {  // TracePortalPlacementInfo_t (SourceAutoRecord's SDK)
	int result;
	Vector finalPos;
	QAngle finalAngle;
	void *placementHelper;
	CGameTrace tr;
};
enum { kPlaceSuccess = 0, kPlaceBumped = 2, kPlaceCantFit = 3, kPlaceCleanser = 4, kPlaceOverlapLinked = 5, kPlaceInvalidSurface = 8 };

using _TraceFirePortal = bool(__rescall *)(void *gun, const Vector &origin, const Vector &dir, bool portal2, int placedBy, PlacementInfo &info);
static _TraceFirePortal TraceFirePortal = nullptr;
static bool __rescall TraceFirePortal_Hook(void *gun, const Vector &origin, const Vector &dir, bool portal2, int placedBy, PlacementInfo &info);
static Hook g_tfpHook(&TraceFirePortal_Hook);
static bool g_enabled = true;
static int g_log = 400;
static int g_shots = 0, g_onBlocks = 0, g_blockFails = 0;

// ---- the shot through Minecraft's blocks --------------------------------------------------------
struct BlockHit {
	int bx, by, bz;
	Vector normal;  // Source
	Vector point;   // Source
	float dist;     // Source units from the start
	int fluid = 0;  // the shot stopped on a water (1) / lava (2) surface, not a solid block
};

static inline void DirToMc(const Vector &s, double out[3]) { out[0] = s.x, out[1] = s.z, out[2] = -s.y; }

// Grid march (Amanatides-Woo) from `start` along unit `dir`, up to maxDist Source units. The cell
// holding the start is skipped (shooting out of a block you are stuck in). stopAtFluid: a shot from
// the air stops on the surface of Minecraft water or lava (a portal may not be placed there); from
// under water it goes on through the water.
static bool MarchBlocks(const Vector &start, const Vector &dir, float maxDist, BlockHit &hit, bool stopAtFluid = false) {
	double p[3], d[3];
	SrcToMcD(start, p[0], p[1], p[2]);
	DirToMc(dir, d);
	const double maxT = maxDist / kUnitsPerBlock;
	int c[3], step[3];
	double tMax[3], tDelta[3];
	for (int a = 0; a < 3; ++a) {
		c[a] = (int)std::floor(p[a]);
		if (d[a] > 1e-12) step[a] = 1, tMax[a] = (c[a] + 1 - p[a]) / d[a], tDelta[a] = 1 / d[a];
		else if (d[a] < -1e-12) step[a] = -1, tMax[a] = (p[a] - c[a]) / -d[a], tDelta[a] = 1 / -d[a];
		else step[a] = 0, tMax[a] = 1e30, tDelta[a] = 1e30;
	}
	const bool startWet = stopAtFluid && TerrainFluid(c[0], c[1], c[2]) != 0;
	for (int i = 0; i < 4096; ++i) {
		int a = tMax[0] < tMax[1] ? (tMax[0] < tMax[2] ? 0 : 2) : (tMax[1] < tMax[2] ? 1 : 2);
		double t = tMax[a];
		if (t > maxT) return false;
		c[a] += step[a];
		tMax[a] += tDelta[a];
		hit.fluid = 0;
		if (!TerrainIsSolid(c[0], c[1], c[2])) {
			if (!stopAtFluid || startWet) continue;
			hit.fluid = TerrainFluid(c[0], c[1], c[2]);
			if (!hit.fluid) continue;
		}
		hit.bx = c[0], hit.by = c[1], hit.bz = c[2];
		double n[3] = {0, 0, 0};
		n[a] = -step[a];
		hit.normal = Vector((float)n[0], (float)-n[2], (float)n[1]);  // Minecraft (x, y, z) -> Source (x, -z, y)
		hit.dist = (float)(t * kUnitsPerBlock);
		hit.point = start + dir * hit.dist;
		return true;
	}
	return false;
}

// Line of sight from a to b crosses a Minecraft block (b's own cell and the last 2 u excluded).
bool TerrainRayBlocked(const Vector &a, const Vector &b) {
	Vector d = b - a;
	float L = d.Length();
	if (L < 4) return false;
	BlockHit h;
	return MarchBlocks(a, d * (1.0f / L), L - 2.0f, h);
}
// The first Minecraft solid block face along a ray (Source units): the composite's wobble probe tracks such
// points (exact block surfaces, not the image's quantised depth).
bool TerrainRayHit(const Vector &a, const Vector &dir, float maxDist, Vector &point) {
	BlockHit h;
	if (!MarchBlocks(a, dir, maxDist, h)) return false;
	point = h.point;
	return true;
}

// ---- fitting the opening on block faces -----------------------------------------------------------
struct Frame {
	Vector f, r, u;  // normal, right, up (Source)
};

// Source angles for a basis (forward, left = up x forward, up): the public mathlib MatrixAngles.
static QAngle BasisAngles(const Vector &f, const Vector &u) {
	Vector l(u.y * f.z - u.z * f.y, u.z * f.x - u.x * f.z, u.x * f.y - u.y * f.x);
	float xy = std::sqrt(f.x * f.x + f.y * f.y);
	QAngle a;
	if (xy > 0.001f) {
		a.y = std::atan2(f.y, f.x) * 180.0f / (float)M_PI;
		a.x = std::atan2(-f.z, xy) * 180.0f / (float)M_PI;
		a.z = std::atan2(l.z, u.z) * 180.0f / (float)M_PI;
	} else {
		a.y = std::atan2(-l.x, l.y) * 180.0f / (float)M_PI;
		a.x = std::atan2(-f.z, xy) * 180.0f / (float)M_PI;
		a.z = 0;
	}
	return a;
}

static Frame PortalFrame(const Vector &normal, const Vector &shotDir) {
	Frame fr;
	fr.f = normal;
	Vector up(0, 0, 1);
	if (std::fabs(normal.z) > 0.999f) {
		// floor or ceiling: the portal's top towards the shot's horizontal direction
		up = Vector(shotDir.x, shotDir.y, 0);
		float l = up.Length();
		up = l > 1e-4f ? up * (1.0f / l) : Vector(1, 0, 0);
	}
	// up projected onto the face, right = up x normal (Source: right = -(up x forward) reversed)
	float k = Vector::DotProduct(up, normal);
	fr.u = up - normal * k;
	fr.u = fr.u * (1.0f / fr.u.Length());
	fr.r = Vector(fr.u.y * normal.z - fr.u.z * normal.y, fr.u.z * normal.x - fr.u.x * normal.z, fr.u.x * normal.y - fr.u.y * normal.x);
	return fr;
}

static inline void Cell(const Vector &s, int &x, int &y, int &z) {
	double a, b, c;
	SrcToMcD(s, a, b, c);
	x = (int)std::floor(a), y = (int)std::floor(b), z = (int)std::floor(c);
}

// Sample points over the opening (every 8 u inside, every 4 u along the edges, 0.5 u inset).
static std::vector<std::pair<float, float>> g_samples;
static void BuildSamples() {
	if (!g_samples.empty()) return;
	const float hw = 32 - 0.5f, hh = 56 - 0.5f;
	for (float b = -hh; b <= hh + 0.01f; b += 8)
		for (float a = -hw; a <= hw + 0.01f; a += 8) g_samples.push_back({a, b});
	for (float a = -hw; a <= hw; a += 4) g_samples.push_back({a, -hh}), g_samples.push_back({a, hh});
	for (float b = -hh; b <= hh; b += 4) g_samples.push_back({-hw, b}), g_samples.push_back({hw, b});
	g_samples.push_back({hw, hh}), g_samples.push_back({-hw, hh}), g_samples.push_back({hw, -hh}), g_samples.push_back({-hw, -hh});
}

// The opening at centre c lies flat on solid block faces with air in front everywhere.
static bool OpeningFits(const Vector &c, const Frame &fr) {
	for (auto &s : g_samples) {
		Vector p = c + fr.r * s.first + fr.u * s.second;
		int x, y, z;
		Cell(p - fr.f * 1.0f, x, y, z);
		if (!TerrainSectionKnown(x, y, z) || !TerrainIsSolid(x, y, z) || TerrainNoPortal(x, y, z)) return false;
		Cell(p + fr.f * 1.0f, x, y, z);
		if (TerrainIsSolid(x, y, z)) return false;
	}
	return true;
}

// The other portal of the pair overlaps this one (both facing the same way, Portal 2's 64 u deep
// box in front of the existing portal).
static bool OverlapsPartner(const Vector &c, const Frame &fr, int other) {
	if (other < 0 || !g_portalSet[other]) return false;
	if (Vector::DotProduct(fr.f, g_portalNormal[other]) < 0.95f) return false;
	Vector d = g_portalPos[other] - c;
	float depth = Vector::DotProduct(d, fr.f);
	if (depth < -64.0f || depth > 1.0f) return false;
	// 2D separating axes of the two rectangles in the face plane
	Vector ou = g_portalUp[other], orr(ou.y * fr.f.z - ou.z * fr.f.y, ou.z * fr.f.x - ou.x * fr.f.z, ou.x * fr.f.y - ou.y * fr.f.x);
	const Vector axes[4] = {fr.r, fr.u, orr, ou};
	for (auto &ax : axes) {
		float ra = 32 * std::fabs(Vector::DotProduct(fr.r, ax)) + 56 * std::fabs(Vector::DotProduct(fr.u, ax));
		float rb = 32 * std::fabs(Vector::DotProduct(orr, ax)) + 56 * std::fabs(Vector::DotProduct(ou, ax));
		if (std::fabs(Vector::DotProduct(d, ax)) >= ra + rb) return false;
	}
	return true;
}

static std::vector<std::pair<float, float>> g_offsets;  // in-plane slides, nearest first
static void BuildOffsets() {
	if (!g_offsets.empty()) return;
	const float lim = std::sqrt(2.0f * (32 * 32 + 56 * 56));
	for (float b = -lim; b <= lim; b += 2)
		for (float a = -lim; a <= lim; a += 2)
			if (a * a + b * b <= lim * lim) g_offsets.push_back({a, b});
	std::sort(g_offsets.begin(), g_offsets.end(), [](auto &p, auto &q) { return p.first * p.first + p.second * p.second < q.first * q.first + q.second * q.second; });
}

// Fit the opening around `hit` on its face. Returns the Portal 2 result code.
static int FitOnBlocks(const BlockHit &hit, const Vector &shotDir, int other, Vector &pos, QAngle &ang, Frame &fr, int &tries) {
	BuildSamples();
	BuildOffsets();
	fr = PortalFrame(hit.normal, shotDir);
	// the face plane, exactly on the block boundary
	Vector plane = hit.point;
	bool overlapSeen = false;
	tries = 0;
	for (auto &o : g_offsets) {
		++tries;
		Vector c = plane + fr.r * o.first + fr.u * o.second;
		if (!OpeningFits(c, fr)) continue;
		if (OverlapsPartner(c, fr, other)) {
			overlapSeen = true;
			continue;
		}
		// floor snap: a wall portal that ends just above a block top sits on it
		if (fr.u.z > 0.7f) {
			for (float g = 0.25f; g <= 3.5f; g += 0.25f) {
				Vector lower = c - fr.u * g;
				int x, y, z;
				bool floorBelow = false;
				for (float a = -31.5f; a <= 31.5f && !floorBelow; a += 9) {
					Cell(lower + fr.r * a - fr.u * 56.5f + fr.f * 1.0f, x, y, z);
					floorBelow = TerrainIsSolid(x, y, z);
				}
				if (floorBelow) {
					if (OpeningFits(c - fr.u * (g - 0.25f), fr)) c = c - fr.u * (g - 0.25f);
					break;
				}
			}
		}
		pos = c + fr.f * 0.0f;
		ang = BasisAngles(fr.f, fr.u);
		return (o.first == 0 && o.second == 0) ? kPlaceSuccess : kPlaceBumped;
	}
	pos = plane;
	ang = BasisAngles(fr.f, fr.u);
	return overlapSeen ? kPlaceOverlapLinked : kPlaceCantFit;
}

// Distance along the ray to the first active portal opening it crosses (the game's trace goes
// through portals), or a large number.
static float FirstPortalCrossing(const Vector &o, const Vector &d) {
	float best = 1e30f;
	for (int i = 0; i < 2; ++i) {
		if (!g_portalSet[i]) continue;
		float denom = Vector::DotProduct(d, g_portalNormal[i]);
		if (denom > -1e-4f) continue;
		float t = Vector::DotProduct(g_portalPos[i] - o, g_portalNormal[i]) / denom;
		if (t <= 0) continue;
		Vector q = o + d * t - g_portalPos[i];
		Vector u = g_portalUp[i];
		Vector r(u.y * g_portalNormal[i].z - u.z * g_portalNormal[i].y, u.z * g_portalNormal[i].x - u.x * g_portalNormal[i].z,
		         u.x * g_portalNormal[i].y - u.y * g_portalNormal[i].x);
		float lx = Vector::DotProduct(q, r) / 32, ly = Vector::DotProduct(q, u) / 56;
		if (lx * lx + ly * ly <= 1 && t < best) best = t;
	}
	return best;
}

struct ShotRec {
	Vector pos;
	QAngle ang;
	int result = -1;
	bool blocks = false;
	int seq = 0;            // shots onto blocks so far (tells a new shot from the last one)
	Vector dir;             // the shot's direction where it hit the blocks
	Vector hitPoint, hitNormal;
	int fluid = 0;          // stopped on water (1) / lava (2)
	bool noPortal = false;  // hit a non-portalable block (leaves, glass, partial block)
	bool prevSet = false;   // that colour's portal before the shot: active, where
	Vector prevPos;
	bool collider = false;  // the game's own trace stopped on a prop collider (and was overridden)
};
static ShotRec g_last[2];  // per colour: the last shot that hit Minecraft blocks
// Portal 2 re-tests a gun-placed portal's resting surface right after placing it and finds none
// behind a Minecraft face, so it deactivated the new portal in place (run mask1d). A portal placed
// through the entity's own NewLocation input is not re-tested (the cliff exit has always stayed),
// so each block placement is applied again that way for 1 s, from the server tick.
struct Pending {
	bool on = false;
	Vector pos;
	QAngle ang;
	int ticks = 0;
};
static Pending g_pending[2];
static Vector g_lastShotPos;
static QAngle g_lastShotAng;
static int g_lastShotResult = -1, g_lastShotTwo = -1;

static bool __rescall TraceFirePortal_Hook(void *gun, const Vector &origin, const Vector &dir, bool portal2, int placedBy, PlacementInfo &info) {
	g_tfpHook.Disable();
	bool r = TraceFirePortal(gun, origin, dir, portal2, placedBy, info);
	g_tfpHook.Enable();
	++g_shots;
	struct Notify {  // tells the clip about the shot with its final verdict and spot, on every return path
		bool portal2;
		bool *res;
		PlacementInfo *info;
		~Notify() { ClipOnGunShot(portal2 ? 1 : 0, *res, info->finalPos); }
	};
	bool verdict = r;
	Notify notify{portal2, &verdict, &info};
	// (bridge #38) The eye is inside Portal 2's solid (a tunnel dug through a wall, floor or the void behind them, or a deep hole): the game's trace
	// starts solid and its answer (a hit on the spot, or the wall dug away) is nothing to go by. The shot goes on through the solid it starts in to
	// the first open space (the chamber the tunnel leads back to; the revealed blocks around the tunnel come first, below, as for any shot); the
	// host voxels left in the way (a hole's wall) are hit before that, like in the branch below.
	bool eyeInSolid = false;
	if (g_enabled && DigAny() && engine.GetPointContentsWorld && (engine.GetPointContentsWorld(engine.traceServer->ThisPtr(), origin, MASK_SOLID) & CONTENTS_SOLID) &&
	    DigNear(origin - Vector(2, 2, 2), origin + Vector(2, 2, 2), 2.0f)) {
		Vector dn = dir;
		const float dl0 = dn.Length();
		if (dl0 > 1e-6f) {
			eyeInSolid = true;
			dn = dn * (1.0f / dl0);
			float exitT = -1;
			for (float t = 4.0f; t < 16000.0f; t += 4.0f)
				if (!(engine.GetPointContentsWorld(engine.traceServer->ThisPtr(), origin + dn * t, MASK_SOLID) & CONTENTS_SOLID)) {
					exitT = t;
					break;
				}
			float tv = 0;
			Vector nv;
			if (CollisionFieldVoxelRay(origin, dn, exitT > 0 ? exitT : 16000.0f, tv, nv)) {
				const Vector vp = origin + dn * tv;
				info.result = kPlaceInvalidSurface;
				info.finalPos = vp;
				info.placementHelper = nullptr;
				info.tr.startpos = origin;
				info.tr.endpos = vp;
				info.tr.fraction = std::fmin(1.0f, tv / 56755.84f);
				info.tr.plane.normal = nv;
				info.tr.plane.dist = Vector::DotProduct(vp, nv);
				info.tr.plane.type = std::fabs(nv.x) > 0.5f ? 0 : std::fabs(nv.y) > 0.5f ? 1 : 2;
				info.tr.contents = CONTENTS_SOLID;
				info.tr.startsolid = info.tr.allsolid = false;
				info.tr.surface.name = "**dig**";
				if (void *w = BlockWorldEntity()) info.tr.m_pEnt = w;
				verdict = r = false;
				if (g_log > 0) --g_log, console->Print("[bridge] gun: portal %d shot from inside Portal 2's solid hit a hole wall at %.0f u: no portal there\n", portal2, tv);
			} else if (exitT > 0) {
				g_tfpHook.Disable();
				const bool r2 = TraceFirePortal(gun, origin + dn * exitT, dn, portal2, placedBy, info);
				g_tfpHook.Enable();
				info.tr.startpos = origin;
				if (g_log > 0) --g_log, console->Print("[bridge] gun: portal %d shot from inside Portal 2's solid went on to open space at %.0f u: result %d\n", portal2, exitT, info.result);
				verdict = r = r2;
			} else {
				info.tr.fraction = 1.0f;  // no way out: nothing of the game's to go by; the blocks (below) decide
				info.result = kPlaceInvalidSurface;
				verdict = r = false;
			}
		}
	}
	// (bridge #33) A shot whose first world hit is on a wall the Minecraft player dug a hole in: the engine still has that wall. It goes
	// on through the dug cells to what is behind them: the exported voxels (a hole's wall: no portal there, the shot fizzles), or, if
	// the way is clear to where the game's hit was and beyond, the game's own trace again from past the dug cells.
	if (g_enabled && DigAny() && !eyeInSolid && info.tr.fraction < 1.0f && (info.tr.startpos - origin).Length() < 1.0f) {
		Vector dn = dir;
		const float dl0 = dn.Length();
		if (dl0 > 1e-6f) {
			dn = dn * (1.0f / dl0);
			const Vector hp = info.tr.endpos;
			const float engineDist = (hp - origin).Length();
			if (DigNear(hp - Vector(2, 2, 2), hp + Vector(2, 2, 2), 2.0f)) {
				float tv = 0;
				Vector nv;
				if (CollisionFieldVoxelRay(origin, dn, engineDist + 160.0f, tv, nv)) {
					const Vector vp = origin + dn * tv;
					info.result = kPlaceInvalidSurface;
					info.finalPos = vp;
					info.placementHelper = nullptr;
					info.tr.endpos = vp;
					info.tr.fraction = std::fmin(1.0f, tv / 56755.84f);
					info.tr.plane.normal = nv;
					info.tr.plane.dist = Vector::DotProduct(vp, nv);
					info.tr.plane.type = std::fabs(nv.x) > 0.5f ? 0 : std::fabs(nv.y) > 0.5f ? 1 : 2;
					info.tr.contents = CONTENTS_SOLID;
					info.tr.surface.name = "**dig**";
					if (void *w = BlockWorldEntity()) info.tr.m_pEnt = w;
					verdict = r = false;  // (the revealed blocks, if any come first, are found below as for any shot)
					if (g_log > 0) {
						--g_log;
						console->Print("[bridge] gun: portal %d shot went through a dug cell and hit a hole wall at %.0f u (game: %.0f u): no portal there\n", portal2, tv, engineDist);
					}
				} else {
					// nothing solid in the exported voxels where the game's hit was: the shot goes on from past the dug cells
					const Vector beyond = origin + dn * (engineDist + 72.0f);
					g_tfpHook.Disable();
					const bool r2 = TraceFirePortal(gun, beyond, dn, portal2, placedBy, info);
					g_tfpHook.Enable();
					info.tr.startpos = origin;
					if (g_log > 0) {
						--g_log;
						console->Print("[bridge] gun: portal %d shot went through dug cells (game hit at %.0f u) and was traced on from %.0f u: result %d\n", portal2, engineDist, engineDist + 72.0f, info.result);
					}
					verdict = r = r2;
				}
			}
		}
	}
	if (!g_enabled || !ClipHasRegion() || !TerrainHasSolids()) return r;
	Vector d = dir;
	float dl = d.Length();
	if (dl < 1e-6f) return r;
	d = d * (1.0f / dl);
	const float kRange = 56755.84f;
	// Segment 1: from the eye. The game's result counts only if it is nearer than the blocks.
	bool viaPortal = (info.tr.startpos - origin).Length() > 1.0f;
	float gameDist = viaPortal ? FirstPortalCrossing(origin, d) : (info.tr.fraction < 1.0f ? (info.tr.endpos - origin).Length() : kRange);
	// (bridge #49) in the virtual region (stage.cpp) Portal 2's world is void: the engine's shot starts inside rock and ends at once, so its answer says nothing
	// about what is in the way; only the Minecraft blocks are there
	if (!viaPortal && InStageBox(origin)) gameDist = kRange;
	// The prop collider pool (propcollide.cpp) puts invisible one-block SOLID_BBOX entities on the
	// terrain around Portal 2 props; the game's shot stops on them (a model: not portalable), so a
	// shot at the ground next to a cube failed. They stand exactly where Minecraft blocks are, so the
	// shot goes on to the blocks themselves. (The +2 u: a block face that coincides with the game's
	// hit, a collider face backed off by the trace epsilon, still counts as the blocks.)
	bool colliderHit = false;
	if (!viaPortal && info.tr.fraction < 1.0f && info.tr.m_pEnt) {
		std::string tn = EntKV(info.tr.m_pEnt, "targetname");
		if (tn.rfind("pcb_col_", 0) == 0) colliderHit = true, gameDist = kRange;
	}
	BlockHit hit;
	bool have = MarchBlocks(origin, d, std::fmin(gameDist + 2.0f, kRange), hit, true);
	Vector shotDir = d;
	if (!have && viaPortal) {
		// Segment after the last portal: the shot came out of a portal into the region.
		Vector s = info.tr.startpos, e = info.tr.endpos;
		Vector sd = e - s;
		float sl = sd.Length();
		if (sl > 1.0f) {
			sd = sd * (1.0f / sl);
			float lim = info.tr.fraction < 1.0f && !InStageBox(s + sd * 2.0f) ? sl : kRange;  // (bridge #49: the engine's segment ends at once in the virtual region's void)
			have = MarchBlocks(s, sd, lim, hit, true);
			shotDir = sd;
		}
	}
	if (!have) return r;
	// A fizzler crossed on the way: Portal 2's own verdict stands.
	if (info.result == kPlaceCleanser) return r;
	int other = -1;
	for (int i = 0; i < 2; ++i)
		if (i != (portal2 ? 1 : 0)) other = i;
	Vector pos;
	QAngle ang;
	Frame fr;
	int tries = 0;
	// Portal 2's rules for what a portal may rest on: a liquid surface or a non-portalable block
	// (leaves, glass, slabs/stairs/fences: not a full cube) is an invalid surface; else the fit.
	const bool noPortal = !hit.fluid && TerrainNoPortal(hit.bx, hit.by, hit.bz);
	int res;
	if (hit.fluid || noPortal) {
		res = kPlaceInvalidSurface;
		fr = PortalFrame(hit.normal, shotDir);
		pos = hit.point, ang = BasisAngles(fr.f, fr.u);
	} else {
		res = FitOnBlocks(hit, shotDir, other, pos, ang, fr, tries);
	}
	// snapped to 1/512 u like Portal 2
	for (int a = 0; a < 3; ++a) (&pos.x)[a] = std::round((&pos.x)[a] * 512.0f) / 512.0f;
	int gameResult = info.result;
	info.result = res;
	info.finalPos = pos;
	info.finalAngle = ang;
	info.placementHelper = nullptr;
	info.tr.startpos = viaPortal && shotDir.x != d.x ? info.tr.startpos : origin;
	info.tr.endpos = hit.point;
	info.tr.fraction = std::fmin(1.0f, hit.dist / kRange);
	info.tr.plane.normal = hit.normal;
	info.tr.plane.dist = Vector::DotProduct(hit.point, hit.normal);
	info.tr.plane.type = std::fabs(hit.normal.x) > 0.5f ? 0 : std::fabs(hit.normal.y) > 0.5f ? 1 : 2;
	info.tr.contents = CONTENTS_SOLID;
	info.tr.startsolid = info.tr.allsolid = false;
	info.tr.surface.name = "**blocks**";
	info.tr.surface.flags = 0;
	info.tr.surface.surfaceProps = 0;
	if (void *w = BlockWorldEntity()) info.tr.m_pEnt = w;
	bool ok = res < kPlaceCantFit;
	verdict = ok;
	++g_onBlocks;
	if (!ok) ++g_blockFails;
	g_lastShotPos = pos, g_lastShotAng = ang, g_lastShotResult = res, g_lastShotTwo = portal2;
	{
		ShotRec &sr = g_last[portal2 ? 1 : 0];
		const int c = portal2 ? 1 : 0;
		sr = ShotRec{};
		sr.pos = pos, sr.ang = ang, sr.result = res, sr.blocks = true, sr.seq = g_onBlocks;
		sr.dir = shotDir, sr.hitPoint = hit.point, sr.hitNormal = hit.normal, sr.fluid = hit.fluid, sr.noPortal = noPortal, sr.collider = colliderHit;
		sr.prevSet = g_portalSet[c], sr.prevPos = g_portalPos[c];
	}
	if (res < kPlaceCantFit) {
		Pending &pd = g_pending[portal2 ? 1 : 0];
		pd.on = true, pd.pos = pos, pd.ang = ang, pd.ticks = 0;
	}
	if (g_log > 0) {
		--g_log;
		console->Print("[bridge] gun: portal %d shot hit Minecraft %s %d %d %d face (%.0f %.0f %.0f) at %.0f u (game: result %d, %s at %.0f u%s) -> result %d at (%.2f %.2f %.2f) angles (%.1f %.1f %.1f), %d fits tried\n",
		               portal2, hit.fluid == 1 ? "water" : hit.fluid == 2 ? "lava" : noPortal ? "non-portalable block" : "block", hit.bx, hit.by, hit.bz, hit.normal.x, hit.normal.y,
		               hit.normal.z, hit.dist, gameResult, r ? "ok" : "fail", gameDist, colliderHit ? ", a prop collider: ignored" : "", res, pos.x, pos.y, pos.z, ang.x, ang.y, ang.z, tries);
	}
	return ok;
}


// Floating origin: a placement on blocks still being re-applied moves with the region.
void PortalGunShift(const Vector &d) {
	for (int i = 0; i < 2; ++i)
		if (g_pending[i].on && ClipRegionContains(g_pending[i].pos)) g_pending[i].pos += d;
}

static int g_reapplyForced = 0;
static int g_reapplyMode = 0;
static int g_closedAfter = 45;  // ticks before an inactive portal counts as closed by Portal 2  // 0 SetActivatedState then NewLocation, 1 NewLocation only, 2 NewLocation then SetActivatedState  // ticks after a shot on blocks during which the placement is re-applied unconditionally
CON_COMMAND(pcb_gun_reapply, "pcb_gun_reapply [forced_ticks=0] - after a shot on Minecraft blocks, re-apply the placement unconditionally for this many ticks (then only if Portal 2 closed or moved it)\n") {
	if (args.ArgC() > 1) g_reapplyForced = std::max(0, std::atoi(args[1]));
	if (args.ArgC() > 2) g_reapplyMode = std::atoi(args[2]);
	if (args.ArgC() > 3) g_closedAfter = std::atoi(args[3]);
	console->Print("[bridge] gun re-apply forced for %d ticks\n", g_reapplyForced);
}

void PortalGunTick() {
	for (int i = 0; i < 2; ++i) {
		Pending &pd = g_pending[i];
		if (!pd.on) continue;
		if (++pd.ticks > 60) {
			pd.on = false;
			continue;
		}
		for (void *p : EntFind("prop_portal")) {
			if (std::atoi(EntKV(p, "PortalTwo").c_str()) != i) continue;
			Vector o = EntVec(p, "origin");
			if ((o - pd.pos).Length() > 2.0f) {
				pd.on = false;  // moved elsewhere since (another shot, a fizzler)
				break;
			}
			// re-apply only a portal Portal 2 moved or closed; forcing it every tick (the old first 20 ticks)
			// restarted the open animation and the placement effect on every tick, so the first shot showed
			// a burst of outlines and then popped in fully open (pcb_gun_reapply <forced_ticks>)
			// Portal 2 reports a portal as not activated while it opens (0.5 s): re-applying then reset its
			// open amount every tick, so a first shot on blocks popped in fully open instead of animating
			const bool closed = EntKV(p, "Activated") != "1" && pd.ticks > g_closedAfter;
			if (pd.ticks > g_reapplyForced && !closed && (o - pd.pos).Length() < 0.05f) continue;
			char buf[160];
			std::snprintf(buf, sizeof buf, "%.3f %.3f %.3f %.2f %.2f %.2f", pd.pos.x, pd.pos.y, pd.pos.z, pd.ang.x, pd.ang.y, pd.ang.z);
			bool a = g_reapplyMode == 1 ? true : EntInput(p, "SetActivatedState", "1");
			bool b = EntInput(p, "NewLocation", buf);
			if (g_reapplyMode == 2) a = EntInput(p, "SetActivatedState", "1");
			// (after tick 20 only a portal Portal 2 closed is re-applied: always logged)
			if (pd.ticks <= 2 || pd.ticks > 20) console->Print("[bridge] gun: portal %d re-applied on blocks at (%s) (tick %d, inputs %d/%d, was active %s)\n", i, buf, pd.ticks, a, b, EntKV(p, "Activated").c_str());
			// the partner relinks to the moved portal
			for (void *q : EntFind("prop_portal"))
				if (q != p && EntKV(q, "Activated") == "1") EntInput(q, "SetActivatedState", "1");
		}
	}
}

bool PortalGunInit() {
	auto fn = Memory::Scan(MODULE_NAME("server"), Offsets::TraceFirePortal);
	if (!fn) return false;
	TraceFirePortal = (_TraceFirePortal)fn;
	g_tfpHook.SetFunc(fn);
	return true;
}

void PortalGunShutdown() { g_tfpHook.Disable(true); }

CON_COMMAND(pcb_gun_blocks, "pcb_gun_blocks [0|1] - Chell's portal gun places portals on Minecraft blocks (default 1)\n") {
	if (args.ArgC() > 1) g_enabled = std::atoi(args[1]) != 0;
	console->Print("[bridge] gun on blocks %d: %d shots, %d hit Minecraft blocks (%d did not fit); last block shot: portal %d result %d at (%.1f %.1f %.1f) angles (%.1f %.1f %.1f)\n", g_enabled,
	               g_shots, g_onBlocks, g_blockFails, g_lastShotTwo, g_lastShotResult, g_lastShotPos.x, g_lastShotPos.y, g_lastShotPos.z, g_lastShotAng.x, g_lastShotAng.y,
	               g_lastShotAng.z);
}

void TestCheck(bool ok, const char *what, const char *detail);  // testassert.cpp
CON_COMMAND(pcb_assert_gun_block, "pcb_assert_gun_block <portalTwo 0|1> [1 floor | 0 wall | -1 any] - that colour's last shot onto Minecraft blocks fitted, and the portal is there, active, in the region\n") {
	int two = args.ArgC() > 1 ? (std::atoi(args[1]) & 1) : 0;
	int wantFloor = args.ArgC() > 2 ? std::atoi(args[2]) : -1;
	const ShotRec &s = g_last[two];
	char buf[360];
	float dist = -1;
	Vector o;
	std::string active = "?";
	for (void *p : EntFind("prop_portal")) {
		if (std::atoi(EntKV(p, "PortalTwo").c_str()) != two) continue;
		o = EntVec(p, "origin");
		dist = (o - s.pos).Length();
		active = EntKV(p, "Activated");
	}
	bool ok = s.blocks && s.result < kPlaceCantFit && dist >= 0 && dist < 1.0f && active == "1" && ClipRegionContains(o);
	if (wantFloor >= 0) ok = ok && (wantFloor ? std::fabs(g_portalNormal[two].z) > 0.99f : std::fabs(g_portalNormal[two].z) < 0.01f);
	std::snprintf(buf, sizeof buf, "portal %d: last block shot result %d at (%.1f %.1f %.1f) angles (%.1f %.1f %.1f); entity at (%.1f %.1f %.1f), %.2f u away, active %s, normal (%.2f %.2f %.2f)", two,
	              s.result, s.pos.x, s.pos.y, s.pos.z, s.ang.x, s.ang.y, s.ang.z, o.x, o.y, o.z, dist, active.c_str(), g_portalNormal[two].x, g_portalNormal[two].y, g_portalNormal[two].z);
	TestCheck(ok, "portal on Minecraft blocks", buf);
}

CON_COMMAND(pcb_aim, "pcb_aim <x> <y> <z> - turn the view to look at a point (from the current eye)\n") {
	if (args.ArgC() < 4) return console->Print("%s", pcb_aim.ThisPtr()->m_pszHelpString);
	Vector eye = PlayerOrigin() + Vector(0, 0, 64);
	Vector t((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3]));
	Vector d = t - eye;
	float l = d.Length();
	if (l < 1) return;
	float yaw = std::atan2(d.y, d.x) * 180.0f / (float)M_PI, pitch = -std::asin(d.z / l) * 180.0f / (float)M_PI;
	char buf[96];
	std::snprintf(buf, sizeof buf, "setang %.2f %.2f 0", pitch, yaw);
	engine.Cmd(buf);
	console->Print("[bridge] aim from (%.0f %.0f %.0f) at (%.0f %.0f %.0f): %s\n", eye.x, eye.y, eye.z, t.x, t.y, t.z, buf);
}

// Test helper: aim at the nearest flat 2 x 3 (or 3 x 2) patch of block tops with air above, within
// `radius` blocks of the player.
CON_COMMAND(pcb_aim_flat, "pcb_aim_flat [radius_blocks=12] - look at the nearest flat 2x3 patch of Minecraft ground (tests)\n") {
	int rad = args.ArgC() > 1 ? std::atoi(args[1]) : 12;
	Vector feet = PlayerOrigin();
	double fx, fy, fz;
	SrcToMcD(feet, fx, fy, fz);
	int bx = (int)std::floor(fx), by = (int)std::floor(fy), bz = (int)std::floor(fz);
	auto top = [&](int x, int z) -> int {  // highest solid block at or below the player with 3 air above
		for (int y = by + 2; y > by - 40; --y)
			if (TerrainIsSolid(x, y, z)) return (!TerrainIsSolid(x, y + 1, z) && !TerrainIsSolid(x, y + 2, z) && !TerrainIsSolid(x, y + 3, z)) ? y : -1000;
		return -1000;
	};
	double best = 1e30;
	Vector target;
	for (int dz = -rad; dz <= rad; ++dz)
		for (int dx = -rad; dx <= rad; ++dx)
			for (int o = 0; o < 2; ++o) {
				int w = o ? 3 : 2, l = o ? 2 : 3;  // x extent, z extent
				int y0 = top(bx + dx, bz + dz);
				if (y0 == -1000) continue;
				bool flat = true;
				for (int i = 0; i < w && flat; ++i)
					for (int j = 0; j < l && flat; ++j) flat = top(bx + dx + i, bz + dz + j) == y0;
				if (!flat) continue;
				double d = dx * dx + dz * dz + 4.0 * (by - y0) * (by - y0) / 16.0;
				if (d < best) best = d, target = McToSrc(bx + dx + w * 0.5, y0 + 1.0, bz + dz + l * 0.5);
			}
	if (best > 1e29) return console->Print("[bridge] aim_flat: no flat patch within %d blocks\n", rad);
	char buf[128];
	std::snprintf(buf, sizeof buf, "pcb_aim %.1f %.1f %.1f", target.x, target.y, target.z);
	engine.Cmd(buf);
	console->Print("[bridge] aim_flat: patch centre (%.1f %.1f %.1f)\n", target.x, target.y, target.z);
}

// Test helper (occlusion): a camera pose in open air from which the region portal's centre is hidden
// behind Minecraft blocks (the line of sight crosses solid blocks short of the portal's own rock).
int ClipRegionPortal();
CON_COMMAND(pcb_view_occluded, "pcb_view_occluded - noclip pose looking at the region portal from behind a terrain ridge (tests)\n") {
	int pi = ClipRegionPortal();
	if (pi < 0) return console->Print("[bridge] view_occluded: no portal in the region\n");
	Vector c = g_portalPos[pi], n = g_portalNormal[pi];
	Vector side(-n.y, n.x, 0);
	if (side.Length() < 0.1f) side = Vector(1, 0, 0);
	side = side * (1.0f / side.Length());
	Vector flat(n.x, n.y, 0);
	if (flat.Length() < 0.1f) flat = Vector(1, 0, 0);
	flat = flat * (1.0f / flat.Length());
	auto airAt = [](const Vector &p) {
		double x, y, z;
		SrcToMcD(p, x, y, z);
		int bx = (int)std::floor(x), by = (int)std::floor(y), bz = (int)std::floor(z);
		return TerrainSectionKnown(bx, by, bz) && !TerrainIsSolid(bx, by, bz) && !TerrainIsSolid(bx, by - 1, bz) && !TerrainIsSolid(bx, by - 2, bz);
	};
	float bestScore = 1e30f;
	Vector best;
	for (float dist = 240; dist <= 2400; dist += 80)
		for (float ang = -75; ang <= 75; ang += 7.5f)
			for (float h = -200; h <= 200; h += 40) {
				float a = ang * (float)M_PI / 180;
				Vector e = c + (flat * std::cos(a) + side * std::sin(a)) * dist + Vector(0, 0, h);
				if (!airAt(e)) continue;
				Vector d = c - e;
				float L = d.Length();
				d = d * (1.0f / L);
				BlockHit hit;
				if (!MarchBlocks(e, d, L - 80.0f, hit) || hit.dist < 120.0f) continue;
				// prefer a ridge well in front of the portal, a moderate distance, a near-frontal view
				float score = dist + std::fabs(ang) * 4 - (L - hit.dist) * 0.5f;
				if (score < bestScore) bestScore = score, best = e;
			}
	if (bestScore > 1e29f) return console->Print("[bridge] view_occluded: no occluded viewpoint found\n");
	Vector d = c - best;
	float yaw = std::atan2(d.y, d.x) * 180.0f / (float)M_PI, pitch = -std::asin(d.z / d.Length()) * 180.0f / (float)M_PI;
	char buf[160];
	std::snprintf(buf, sizeof buf, "setpos %.2f %.2f %.2f; setang %.2f %.2f 0", best.x, best.y, best.z - 64, pitch, yaw);
	engine.Cmd(buf);
	console->Print("[bridge] view_occluded: eye (%.0f %.0f %.0f), %.0f u from the portal: %s\n", best.x, best.y, best.z, d.Length(), buf);
}

CON_COMMAND(pcb_goto_portal, "pcb_goto_portal <0|1> [distance=40] [along_up_u=0] [along_right_u=0] - setpos the player `distance` u out from portal N's centre along its normal (floor portal: standing above it), offset along the opening's long / short axis\n") {
	int w = args.ArgC() > 1 ? (std::atoi(args[1]) & 1) : 0;
	float d = args.ArgC() > 2 ? (float)std::atof(args[2]) : 40;
	const float ou = args.ArgC() > 3 ? (float)std::atof(args[3]) : 0.0f, orr = args.ArgC() > 4 ? (float)std::atof(args[4]) : 0.0f;
	const Vector &n = g_portalNormal[w], &u = g_portalUp[w];
	const Vector r(u.y * n.z - u.z * n.y, u.z * n.x - u.x * n.z, u.x * n.y - u.y * n.x);
	Vector p = g_portalPos[w] + n * d + u * ou + r * orr;
	if (g_portalNormal[w].z < 0.7f) p.z -= 36;  // wall: centre the 72 u hull on the opening
	char buf[128];
	std::snprintf(buf, sizeof buf, "setpos %.2f %.2f %.2f", p.x, p.y, p.z);
	engine.Cmd(buf);
	console->Print("[bridge] goto portal %d: %s\n", w, buf);
}

// (bridge #49, tests) The player's view turned to portal N's centre (either mode: the Minecraft player's view follows), and the Minecraft player put in
// front of portal N the way pcb_goto_portal puts Chell (pcb_mc_place: the host's placement check, then a short settle), facing it.
CON_COMMAND(pcb_face_portal, "pcb_face_portal <0|1> - (tests) turn the view to portal N's centre from the eye (either mode)\n") {
	const int w = args.ArgC() > 1 ? (std::atoi(args[1]) & 1) : 0;
	if (!g_portalSet[w]) return console->Print("[bridge] face portal %d: not open\n", w);
	char buf[96];
	std::snprintf(buf, sizeof buf, "pcb_face %.2f %.2f %.2f", g_portalPos[w].x, g_portalPos[w].y, g_portalPos[w].z);
	engine.Cmd(buf);
}
CON_COMMAND(pcb_face_floor, "pcb_face_floor <0|1> [distance=140] [drop=70] - (tests) look at the floor `distance` u out from portal N's centre (a point `drop` u below the eye's level: aim a shot at the ground in front of a wall portal)\n") {
	const int w = args.ArgC() > 1 ? (std::atoi(args[1]) & 1) : 0;
	const float d = args.ArgC() > 2 ? (float)std::atof(args[2]) : 140.0f, drop = args.ArgC() > 3 ? (float)std::atof(args[3]) : 70.0f;
	if (!g_portalSet[w]) return console->Print("[bridge] face floor %d: not open\n", w);
	const Vector p = g_portalPos[w] + g_portalNormal[w] * d;
	char buf[96];
	std::snprintf(buf, sizeof buf, "pcb_face %.2f %.2f %.2f", p.x, p.y, p.z - drop);
	engine.Cmd(buf);
}
CON_COMMAND(pcb_mc_place_portal, "pcb_mc_place_portal <0|1> [distance=140] - (tests) the Minecraft player on its feet `distance` u out from portal N's centre (a wall portal), facing it\n") {
	const int w = args.ArgC() > 1 ? (std::atoi(args[1]) & 1) : 0;
	const float d = args.ArgC() > 2 ? (float)std::atof(args[2]) : 140.0f;
	if (!g_portalSet[w]) return console->Print("[bridge] mc place portal %d: not open\n", w);
	const Vector &n = g_portalNormal[w];
	Vector p = g_portalPos[w] + n * d;
	p.z -= 36;  // (a wall portal: its centre is 36 u above the floor of the hull that fits it)
	const float yaw = std::atan2(-n.y, -n.x) * 180.0f / (float)M_PI;
	char buf[128];
	std::snprintf(buf, sizeof buf, "pcb_mc_place %.2f %.2f %.2f %d 24", p.x, p.y, p.z, (int)std::lround(yaw));
	engine.Cmd(buf);
	console->Print("[bridge] mc place portal %d: %s\n", w, buf);
}


// ---- the mcworld suite: shots at chosen kinds of Minecraft surface ------------------------------------
// pcb_find_surface <kind> [radius=24] [like=-1] [cube]
//   Finds the nearest surface of a kind around the player and puts the (noclip) player where a person
//   would shoot it from: eye a few blocks out in open air, clear line of sight, aimed at its centre
//   (never at the pitch clamp). Kinds: ground, wall, ceiling (flat 3 x 4 block patches), slope (a
//   staircase of single steps), edge (ground ending at a drop, aimed 0.25 block from the edge: the
//   opening must bump back), water (a water surface), noportal (leaves / glass / partial blocks).
//   Surfaces within 3.5 blocks of a live portal are skipped. like=N: the same facing as portal N and
//   nearest to it (side-by-side pairs). cube: also spawn a weighted cube (pcb_mwcube) on the ground
//   beside the target (the prop collider pool then surrounds the spot).
static int g_findN = 0;
static char g_placeClear[160] = "";
static bool g_placeWait = false;  // a placed test block is not in the streamed solids yet
static Vector g_findTarget, g_findNormal;
static bool g_findOk = false;
struct SurfCand {
	Vector target, normal, eye;
	double score;
	Vector lateral;  // in the face plane, across the shot
};
// (an all-air section is never sent by Minecraft, so an unknown section counts as air here; the surface itself must have streamed)
static bool McAir(int x, int y, int z) { return !TerrainIsSolid(x, y, z) && TerrainFluid(x, y, z) == 0; }
static bool McPortalable(int x, int y, int z) { return TerrainSectionKnown(x, y, z) && TerrainIsSolid(x, y, z) && !TerrainNoPortal(x, y, z); }
static bool EyeSeesTarget(const Vector &eye, const Vector &target, const Vector &normal, int fx, int fy, int fz) {
	int ex, ey, ez;
	Cell(eye, ex, ey, ez);
	if (!McAir(ex, ey, ez) || !McAir(ex, ey - 1, ez)) return false;
	Vector d = target - eye;
	float L = d.Length();
	BlockHit h;
	if (!MarchBlocks(eye, d * (1.0f / L), L + 20.0f, h, true)) return false;
	return h.bx == fx && h.by == fy && h.bz == fz && Vector::DotProduct(h.normal, normal) > 0.99f && std::fabs(h.dist - L) < 4.0f;
}
CON_COMMAND(pcb_find_surface, "pcb_find_surface <ground|wall|cliff|ceiling|slope|edge|water|noportal> [radius=24] [like=-1] [cube] - put the noclip player where a person would shoot that kind of Minecraft surface from (tests)\n") {
	const std::string kind = args.ArgC() > 1 ? args[1] : "ground";
	const int rad = args.ArgC() > 2 ? std::atoi(args[2]) : 24;
	const int like = args.ArgC() > 3 ? std::atoi(args[3]) : -1;
	const bool cube = args.ArgC() > 4 && !std::strcmp(args[4], "cube");
	// place=<block>: fill the found ground patch's first air layer with a block (Minecraft's own fill
	// command, e.g. oak_leaves[persistent=true] or glass) and aim at its top; pcb_place_clear restores air
	const char *place = args.ArgC() > 4 && !std::strncmp(args[4], "place=", 6) ? args[4] + 6 : nullptr;
	double fx, fy, fz;
	SrcToMcD(PlayerOrigin(), fx, fy, fz);
	const int bx = (int)std::floor(fx), by = (int)std::floor(fy), bz = (int)std::floor(fz);
	const Vector here = PlayerOrigin() + Vector(0, 0, 64);
	std::vector<SurfCand> cands;
	auto nearPortal = [&](const Vector &t) {
		for (int i = 0; i < 2; ++i)
			if (g_portalSet[i] && (g_portalPos[i] - t).Length() < 3.5f * kUnitsPerBlock) return true;
		return false;
	};
	auto addCand = [&](const Vector &target, const Vector &normal, const Vector &eye, const Vector &lateral, int cx, int cy, int cz) {
		if (nearPortal(target)) return;
		if (like >= 0) {
			if (!g_portalSet[like] || Vector::DotProduct(g_portalNormal[like], normal) < 0.99f) return;
		}
		if (!EyeSeesTarget(eye, target, normal, cx, cy, cz)) return;
		double score = like >= 0 ? (g_portalPos[like] - target).Length() : (target - here).Length();
		cands.push_back(SurfCand{target, normal, eye, score, lateral});
	};
	const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};  // Minecraft x, z
	const float B = kUnitsPerBlock;
	if (kind == "ground" || kind == "ceiling" || kind == "edge" || kind == "water" || kind == "noportal") {
		const bool ceil = kind == "ceiling";
		for (int dz = -rad; dz <= rad; ++dz)
			for (int dx = -rad; dx <= rad; ++dx)
				for (int y = by - 24; y <= by + 24; ++y)
					for (auto &dd : dirs) {
						// the patch: `along` the shot (the opening's long axis), `w` across it
						const int ax = dd[0], az = dd[1], lx = -az, lz = ax;  // lateral axis
						const int along = kind == "edge" ? 3 : 4, w = cube ? 5 : 3;
						const int x0 = bx + dx, z0 = bz + dz;
						bool ok = true;
						for (int i = 0; i < along && ok; ++i)
							for (int j = -(w / 2); j <= w / 2 && ok; ++j) {
								int x = x0 + ax * i + lx * j, z = z0 + az * i + lz * j;
								if (kind == "water") {
									ok = TerrainFluid(x, y, z) == 1 && TerrainFluid(x, y - 1, z) == 1 && McAir(x, y + 1, z) && McAir(x, y + 2, z);
								} else if (kind == "noportal") {
									if (i >= 2 || j != 0) continue;  // (a 1 x 2 run of leaves tops is enough)
									ok = TerrainSectionKnown(x, y, z) && TerrainIsSolid(x, y, z) && TerrainNoPortal(x, y, z) && McAir(x, y + 1, z) && McAir(x, y + 2, z);
								} else if (ceil) {
									ok = McPortalable(x, y, z) && McAir(x, y - 1, z) && McAir(x, y - 2, z) && McAir(x, y - 3, z);
								} else {
									ok = McPortalable(x, y, z) && McAir(x, y + 1, z) && McAir(x, y + 2, z) && McAir(x, y + 3, z);
								}
							}
						if (!ok) continue;
						if (kind == "edge") {
							// beyond the far end: a step down (1+ block) across the whole width
							for (int j = -(w / 2); j <= w / 2 && ok; ++j) {
								int x = x0 + ax * along + lx * j, z = z0 + az * along + lz * j;
								ok = McAir(x, y, z) && McAir(x, y + 1, z);
							}
							if (!ok) continue;
						}
						// target: centre of the patch's face (edge: 0.25 block short of the far edge)
						double cx = x0 + 0.5 + ax * (along * 0.5 - 0.5), cz = z0 + 0.5 + az * (along * 0.5 - 0.5);
						if (kind == "edge") cx = x0 + 0.5 + ax * (along - 0.5) - ax * 0.25, cz = z0 + 0.5 + az * (along - 0.5) - az * 0.25;
						if (kind == "noportal") cx = x0 + 0.5, cz = z0 + 0.5;
						double ty = ceil ? y : y + 1.0;
						if (kind == "water") ty = y + 1.0 - 0.01;  // (the water cell's top)
						Vector target = McToSrc(cx, ty, cz), normal(0, 0, ceil ? -1.0f : 1.0f);
						// the shooter: back along the shot axis and up (down for a ceiling), ~5 blocks away
						Vector back = McToSrc(cx - ax, ty, cz - az) - target;  // one block back, Source
						Vector eye = ceil ? target + back * 2.2f - Vector(0, 0, 2.6f * B) : target + back * 4.0f + Vector(0, 0, 3.0f * B);
						Vector lat = McToSrc(cx + lx, ty, cz + lz) - target;
						int hx, hy, hz;
						Cell(target - normal * 1.0f, hx, hy, hz);
						addCand(target, normal, eye, lat, hx, hy, hz);
					}
	} else if (kind == "wall" || kind == "cliff") {
		// wall: a portal a person can walk into (ground in front, level with the opening's bottom, 4
		// blocks out); cliff: any 3 x 4 face with air in front (e.g. the high cliff face)
		const bool walk = kind == "wall";
		for (int dz = -rad; dz <= rad; ++dz)
			for (int dx = -rad; dx <= rad; ++dx)
				for (int y = by - 24; y <= by + 24; ++y)
					for (auto &dd : dirs) {
						// face normal (dd) in Minecraft x/z; the patch is 3 across x 4 tall
						const int nx = dd[0], nz = dd[1], lx = -nz, lz = nx;
						const int x0 = bx + dx, z0 = bz + dz;
						bool ok = true;
						for (int k = 0; k < (walk ? 3 : 4) && ok; ++k)
							for (int j = -1; j <= 1 && ok; ++j) {
								int x = x0 + lx * j, z = z0 + lz * j, yy = y + k;
								ok = McPortalable(x, yy, z) && McAir(x + nx, yy, z + nz) && McAir(x + 2 * nx, yy, z + 2 * nz);
							}
						// ground to stand on in front, level with the patch's bottom, 4 blocks out, with headroom
						for (int k = 1; k <= 3 && ok && walk; ++k)
							for (int j = -1; j <= 1 && ok; ++j) {
								int x = x0 + lx * j + nx * k, z = z0 + lz * j + nz * k;
								ok = TerrainSectionKnown(x, y - 1, z) && TerrainIsSolid(x, y - 1, z) && McAir(x, y, z) && McAir(x, y + 1, z) && McAir(x, y + 2, z);
							}
						if (!ok) continue;
						// (with ground to stand on in front, 1-2 blocks below the patch's bottom: a portal a person can walk into)
						// (aimed low: the opening's bottom 2 u above the ground, so Portal 2's floor snap puts it on it)
						double cx = x0 + 0.5 + nx * 0.5, cz = z0 + 0.5 + nz * 0.5, cy = walk ? y + 1.45 : y + 2.0;
						Vector target = McToSrc(cx, cy, cz);
						Vector normal = McToSrc(cx + nx, cy, cz + nz) - target;
						normal = normal * (1.0f / normal.Length());
						Vector eye = target + normal * (5.0f * B);
						Vector lat = McToSrc(cx + lx, cy, cz + lz) - target;
						addCand(target, normal, eye, lat, x0, walk ? y + 1 : y + 2, z0);
					}
	} else if (kind == "slope") {
		auto top = [&](int x, int z) {
			for (int y = by + 20; y > by - 30; --y)
				if (TerrainIsSolid(x, y, z)) return McAir(x, y + 1, z) && McAir(x, y + 2, z) ? y : -10000;
			return -10000;
		};
		for (int dz = -rad; dz <= rad; ++dz)
			for (int dx = -rad; dx <= rad; ++dx)
				for (auto &dd : dirs) {
					const int ax = dd[0], az = dd[1], lx = -az, lz = ax;
					const int x0 = bx + dx, z0 = bz + dz;
					int y0 = top(x0, z0);
					if (y0 < -9000) continue;
					bool ok = true;
					for (int i = 0; i < 4 && ok; ++i)
						for (int j = -1; j <= 1 && ok; ++j) ok = top(x0 + ax * i + lx * j, z0 + az * i + lz * j) == y0 + i;
					if (!ok) continue;
					double cx = x0 + 0.5 + ax, cz = z0 + 0.5 + az;  // the second step
					Vector target = McToSrc(cx, y0 + 2.0, cz), normal(0, 0, 1);
					Vector back = McToSrc(cx - ax, y0 + 2.0, cz - az) - target;
					Vector eye = target + back * 4.0f + Vector(0, 0, 3.5f * B);
					Vector lat = McToSrc(cx + lx, y0 + 2.0, cz + lz) - target;
					addCand(target, normal, eye, lat, x0 + ax, y0 + 1, z0 + az);
				}
	}
	char buf[400];
	if (cands.empty()) {
		g_findOk = false;
		std::snprintf(buf, sizeof buf, "no %s surface within %d blocks of MC (%d %d %d)%s", kind.c_str(), rad, bx, by, bz, like >= 0 ? " like the other portal" : "");
		return TestCheck(false, "surface found", buf);
	}
	std::sort(cands.begin(), cands.end(), [](const SurfCand &a, const SurfCand &b) { return a.score < b.score; });
	const SurfCand &c = cands[0];
	g_findOk = true, g_findTarget = c.target, g_findNormal = c.normal, ++g_findN;
	Vector d = c.target - c.eye;
	float pitch = -std::asin(d.z / d.Length()) * 180.0f / (float)M_PI, yaw = std::atan2(d.y, d.x) * 180.0f / (float)M_PI;
	char cmd[200];
	std::snprintf(cmd, sizeof cmd, "setpos %.2f %.2f %.2f; setang %.2f %.2f 0", c.eye.x, c.eye.y, c.eye.z - 64, pitch, yaw);
	engine.Cmd(cmd);
	PuppetLaunch(Vector(0, 0, 0));
	if (place) {
		// the patch: 3 across x 4 along; target sits at the patch centre (the shot runs along it)
		double tx, ty, tz;
		SrcToMcD(c.target, tx, ty, tz);
		Vector latMc = c.lateral;  // Source; to Minecraft x / z
		double lmx = latMc.x / kUnitsPerBlock, lmz = -latMc.y / kUnitsPerBlock;
		int ax = std::abs(lmx) > 0.5 ? 0 : 1;  // the shot axis is the other one
		int x0, x1, z0, z1, y = (int)std::lround(ty);
		if (ax == 1) x0 = (int)std::floor(tx) - 1, x1 = x0 + 2, z0 = (int)std::floor(tz - 1.0), z1 = z0 + 3;   // across x, along z
		else z0 = (int)std::floor(tz) - 1, z1 = z0 + 2, x0 = (int)std::floor(tx - 1.0), x1 = x0 + 3;          // across z, along x
		char fc[200];
		std::snprintf(fc, sizeof fc, "pcb_mc_cmd fill %d %d %d %d %d %d %s", x0, y, z0, x1, y, z1, place);
		engine.Cmd(fc);
		std::snprintf(g_placeClear, sizeof g_placeClear, "pcb_mc_cmd fill %d %d %d %d %d %d air", x0, y, z0, x1, y, z1);
		// aim at its top (one block up), from one block higher
		const Vector up(0, 0, kUnitsPerBlock);
		Vector t2 = c.target + up, e2 = c.eye + up;
		g_findTarget = t2, g_placeWait = true;
		Vector d2 = t2 - e2;
		float p2 = -std::asin(d2.z / d2.Length()) * 180.0f / (float)M_PI, y2 = std::atan2(d2.y, d2.x) * 180.0f / (float)M_PI;
		std::snprintf(cmd, sizeof cmd, "setpos %.2f %.2f %.2f; setang %.2f %.2f 0", e2.x, e2.y, e2.z - 64, p2, y2);
		engine.Cmd(cmd);
		console->Print("[bridge] find_surface: placed %s over MC (%d %d %d)-(%d %d %d), aiming at its top\n", place, x0, y, z0, x1, y, z1);
	}
	if (cube) {
		Vector cp = c.target + c.lateral * 1.9f + c.normal * 30.0f;
		char cc[160];
		std::snprintf(cc, sizeof cc, "pcb_spawn_cube %.1f %.1f %.1f pcb_mwcube", cp.x, cp.y, cp.z);
		engine.Cmd(cc);
	}
	double mx, my, mz;
	SrcToMcD(c.target, mx, my, mz);
	std::snprintf(buf, sizeof buf, "%s: %d candidates; target (%.1f %.1f %.1f) = MC (%.2f %.2f %.2f) normal (%.0f %.0f %.0f), eye (%.0f %.0f %.0f) %.1f blocks away, pitch %.1f yaw %.1f%s", kind.c_str(),
	              (int)cands.size(), c.target.x, c.target.y, c.target.z, mx, my, mz, c.normal.x, c.normal.y, c.normal.z, c.eye.x, c.eye.y, c.eye.z, d.Length() / B, pitch, yaw,
	              cube ? ", cube beside it" : "");
	TestCheck(true, "surface found", buf);
}

// pcb_assert_gun_shot <colour 0|1> <placed|rejected|valid> [floor|wall|ceiling|any] [min_bump_u=0]
static int g_assertedSeq[2] = {0, 0};
CON_COMMAND(pcb_assert_gun_shot, "pcb_assert_gun_shot <colour 0|1> <placed|rejected|valid> [floor|wall|ceiling|any] [min_bump_u] - the colour's newest shot onto Minecraft blocks: placement, up vector, fit, linking\n") {
	if (args.ArgC() < 3) return console->Print("%s", pcb_assert_gun_shot.ThisPtr()->m_pszHelpString);
	const int c = std::atoi(args[1]) & 1;
	const char want = args[2][0];
	const std::string kind = args.ArgC() > 3 ? args[3] : "any";
	const float minBump = args.ArgC() > 4 ? (float)std::atof(args[4]) : 0;
	const ShotRec &s = g_last[c];
	const bool fresh = s.blocks && s.seq > g_assertedSeq[c];
	g_assertedSeq[c] = s.seq;
	Vector o;
	bool found = false, active = false;
	for (void *p : EntFind("prop_portal")) {
		if (std::atoi(EntKV(p, "PortalTwo").c_str()) != c) continue;
		o = EntVec(p, "origin"), found = true, active = EntKV(p, "Activated") == "1";
	}
	const Vector n = g_portalNormal[c], u = g_portalUp[c];
	const bool placedOk = s.result < kPlaceCantFit;
	char buf[600];
	int len = std::snprintf(buf, sizeof buf, "portal %d: shot %s, result %d%s%s%s, hit (%.1f %.1f %.1f) n (%.0f %.0f %.0f); entity %s at (%.1f %.1f %.1f) active %d, normal (%.2f %.2f %.2f) up (%.2f %.2f %.2f)",
	                        c, fresh ? "new" : "NOT NEW", s.result, s.fluid == 1 ? " [water]" : s.fluid == 2 ? " [lava]" : "", s.noPortal ? " [non-portalable]" : "",
	                        s.collider ? " [prop collider overridden]" : "", s.hitPoint.x, s.hitPoint.y, s.hitPoint.z, s.hitNormal.x, s.hitNormal.y, s.hitNormal.z, found ? "found" : "MISSING", o.x,
	                        o.y, o.z, active, n.x, n.y, n.z, u.x, u.y, u.z);
	bool ok = fresh && found;
	bool placed = placedOk && active && (o - s.pos).Length() < 1.0f && ClipRegionContains(o);
	if (want == 'p' || (want == 'v' && placedOk)) {
		ok = ok && placed;
		// orientation: the face's normal; a wall portal upright, a floor/ceiling portal's top along the shot
		if (kind == "floor") ok = ok && n.z > 0.99f;
		else if (kind == "ceiling") ok = ok && n.z < -0.99f;
		else if (kind == "wall") ok = ok && std::fabs(n.z) < 0.01f;
		bool upOk;
		if (std::fabs(n.z) > 0.99f) {
			Vector h(s.dir.x, s.dir.y, 0);
			float hl = h.Length();
			upOk = hl > 1e-3f && Vector::DotProduct(u, h * (1.0f / hl)) > 0.98f;
		} else {
			upOk = u.z > 0.99f;
		}
		ok = ok && upOk && Vector::DotProduct(n, s.hitNormal) > 0.99f;
		// the opening really rests on portalable block faces with air in front (the live pose)
		Frame fr;
		fr.f = n, fr.u = u, fr.r = Vector(u.y * n.z - u.z * n.y, u.z * n.x - u.x * n.z, u.x * n.y - u.y * n.x);
		BuildSamples();
		bool fits = OpeningFits(o, fr);
		Vector moved = s.pos - s.hitPoint;
		moved = moved - n * Vector::DotProduct(moved, n);
		float bump = moved.Length();
		ok = ok && fits && bump >= minBump && bump <= 91.3f;
		// linking: the other colour exists and is open, so the pair is linked
		int other = 1 - c;
		bool linked = g_portalSet[other];
		len += std::snprintf(buf + len, sizeof buf - len, "; up %s, fit on blocks %s, bumped %.1f u (min %.1f), partner %s", upOk ? "ok" : "WRONG", fits ? "ok" : "NO", bump, minBump,
		                     linked ? "open (linked)" : "not placed");
		TestCheck(ok, want == 'v' ? "gun shot valid (placed by Portal 2's rules)" : "gun shot placed", buf);
		return;
	}
	if (want == 'r' || want == 'v') {
		// rejected: the portal stayed where it was (or stayed closed)
		bool unchanged = s.prevSet ? (active && (o - s.prevPos).Length() < 1.0f) : !(active && (o - s.pos).Length() < 1.0f);
		ok = ok && !placedOk && unchanged;
		std::snprintf(buf + len, sizeof buf - len, "; before the shot %s (%.1f %.1f %.1f): %s", s.prevSet ? "open at" : "closed", s.prevPos.x, s.prevPos.y, s.prevPos.z,
		              unchanged ? "unchanged" : "CHANGED");
		TestCheck(ok, want == 'v' ? "gun shot valid (rejected by Portal 2's rules)" : "gun shot rejected", buf);
	}
}

// pcb_view_from <portal> <distance_blocks>: a noclip pose `distance` blocks from portal N's centre in
// open air with a clear line of sight to it, facing it (wall portals: straight out first, then up to
// 40 deg aside / 20 deg up; floor portals: 55 deg up first, any azimuth). Chamber portals: straight out.
CON_COMMAND(pcb_view_from, "pcb_view_from <0|1> <distance_blocks> - noclip pose in open air looking at portal N from that distance (tests)\n") {
	if (args.ArgC() < 3) return console->Print("%s", pcb_view_from.ThisPtr()->m_pszHelpString);
	const int i = std::atoi(args[1]) & 1;
	const float dist = (float)std::atof(args[2]) * kUnitsPerBlock;
	if (!g_portalSet[i]) return TestCheck(false, "view pose", "portal not open");
	const Vector c = g_portalPos[i], n = g_portalNormal[i];
	const bool floorLike = std::fabs(n.z) > 0.7f, region = ClipRegionContains(c);
	Vector side = std::fabs(n.z) > 0.7f ? Vector(1, 0, 0) : Vector(-n.y, n.x, 0);
	side = side * (1.0f / side.Length());
	Vector up = floorLike ? Vector(0, 1, 0) : Vector(0, 0, 1);
	auto ok = [&](const Vector &eye) {
		if (!region) return true;
		int x, y, z;
		Cell(eye, x, y, z);
		if (!McAir(x, y, z) || !McAir(x, y - 1, z) || !ClipRegionContains(eye)) return false;
		return !TerrainRayBlocked(eye, c + n * 2.0f);
	};
	Vector best;
	bool found = false;
	if (floorLike) {
		const float els[4] = {55, 65, 45, 75};
		for (float el : els)
			for (int az = 0; az < 360 && !found; az += 30) {
				float e = el * (float)M_PI / 180, a = az * (float)M_PI / 180;
				Vector h = side * std::cos(a) + up * std::sin(a);
				Vector eye = c + (n * std::sin(e) + h * std::cos(e)) * dist;
				if (ok(eye)) best = eye, found = true;
			}
	} else {
		const float as[5] = {0, 20, -20, 40, -40}, es[4] = {0, 10, 20, -10};
		for (float ev : es)
			for (float av : as) {
				if (found) break;
				float e = ev * (float)M_PI / 180, a = av * (float)M_PI / 180;
				Vector eye = c + (n * (std::cos(a) * std::cos(e)) + side * (std::sin(a) * std::cos(e)) + up * std::sin(e)) * dist;
				if (ok(eye)) best = eye, found = true;
			}
	}
	char buf[240];
	if (!found) {
		std::snprintf(buf, sizeof buf, "portal %d: no open-air pose %.0f blocks out with a clear view", i, dist / kUnitsPerBlock);
		return TestCheck(false, "view pose", buf);
	}
	Vector d = c - best;
	float pitch = -std::asin(d.z / d.Length()) * 180.0f / (float)M_PI, yaw = std::atan2(d.y, d.x) * 180.0f / (float)M_PI;
	char cmd[160];
	std::snprintf(cmd, sizeof cmd, "setpos %.2f %.2f %.2f; setang %.2f %.2f 0", best.x, best.y, best.z - 64, pitch, yaw);
	engine.Cmd(cmd);
	PuppetLaunch(Vector(0, 0, 0));
	std::snprintf(buf, sizeof buf, "portal %d from %.0f blocks: eye (%.0f %.0f %.0f), pitch %.1f yaw %.1f", i, dist / kUnitsPerBlock, best.x, best.y, best.z, pitch, yaw);
	TestCheck(true, "view pose", buf);
}
CON_COMMAND(pcb_place_clear, "pcb_place_clear - restore the air that pcb_find_surface ... place=<block> filled\n") {
	if (!g_placeClear[0]) return;
	engine.Cmd(g_placeClear);
	console->Print("[bridge] find_surface: restored (%s)\n", g_placeClear);
	g_placeClear[0] = 0;
}
// waituntil placed: the block placed by pcb_find_surface ... place=<block> has streamed (non-portalable)
// ---- test placements must be real ones ---------------------------------------------------------------
// pcb_assert_portal_face [n=-1 both]: a placed portal lies flush on an exposed Minecraft block face. At 9
// points across its opening (centre, +-0.75 of the half extents on each axis, the 4 diagonals at 0.5):
// the cell 3 u behind the plane is a streamed solid, and the cells 3 u, 1 block and 2 blocks in front
// are not solid. A section that has not streamed counts as air in front only once streaming has
// settled (all-air sections are never sent); before that it is a failure (a portal "on" the boundary of
// terrain that has not arrived yet sits inside the rock).
double TerrainIdleS();  // terrain.cpp
static bool PortalOnFace(int i, char *out, size_t n) {
	if (!g_portalSet[i]) return std::snprintf(out, n, "portal %d not placed", i), false;
	const Vector c = g_portalPos[i], nrm = g_portalNormal[i], up = g_portalUp[i];
	const Vector r(up.y * nrm.z - up.z * nrm.y, up.z * nrm.x - up.x * nrm.z, up.x * nrm.y - up.y * nrm.x);
	const float pts[9][2] = {{0, 0}, {0.75f, 0}, {-0.75f, 0}, {0, 0.75f}, {0, -0.75f}, {0.5f, 0.5f}, {-0.5f, 0.5f}, {0.5f, -0.5f}, {-0.5f, -0.5f}};
	int behindBad = 0, behindUnknown = 0, frontSolid = 0, frontUnknown = 0;
	for (auto &q : pts) {
		Vector p = c + r * (q[0] * 32.0f) + up * (q[1] * 56.0f);
		int x, y, z;
		Cell(p - nrm * 3.0f, x, y, z);
		if (!TerrainSectionKnown(x, y, z)) ++behindUnknown, ++behindBad;
		else if (!TerrainIsSolid(x, y, z)) ++behindBad;
		for (float d : {3.0f, 40.0f, 80.0f}) {
			Cell(p + nrm * d, x, y, z);
			if (!TerrainSectionKnown(x, y, z)) ++frontUnknown;
			else if (TerrainIsSolid(x, y, z)) ++frontSolid;
		}
	}
	const double idle = TerrainIdleS();
	const bool ok = behindBad == 0 && frontSolid == 0 && (frontUnknown == 0 || idle > 1.0);
	double mx, my, mz;
	SrcToMcD(c, mx, my, mz);
	std::snprintf(out, n, "portal %d at (%.1f %.1f %.1f) = MC (%.2f %.2f %.2f) normal (%.2f %.2f %.2f): behind not solid %d/9 (unstreamed %d), in front solid %d/27, unstreamed %d/27 (streaming idle %.1f s)",
	              i, c.x, c.y, c.z, mx, my, mz, nrm.x, nrm.y, nrm.z, behindBad, behindUnknown, frontSolid, frontUnknown, idle);
	return ok;
}
CON_COMMAND(pcb_assert_portal_face, "pcb_assert_portal_face [n=-1] - portal n (or both) lies flush on an exposed Minecraft block face: solid behind, air in front, across the opening\n") {
	int which = args.ArgC() > 1 ? std::atoi(args[1]) : -1;
	for (int i = 0; i < 2; ++i) {
		if (which >= 0 && i != which) continue;
		char buf[400];
		bool ok = PortalOnFace(i, buf, sizeof buf);
		TestCheck(ok, "portal on an exposed block face", buf);
	}
}

// pcb_build_corridor [mcX mcY mcZ] [gap=12] | remove: a test site with two portalable walls facing each other,
// built with Minecraft's own fill commands IN OPEN AIR (nothing of the world is replaced, so `remove`, a fill
// with air over the same box, restores it exactly): a stone floor whose top is at mcY, centred on (mcX, mcZ),
// and two 7-wide x 5-tall walls `gap` blocks apart along Minecraft x. It refuses a box that is not all air.
// pcb_corridor_aim <0|1> then puts the noclip player in the middle aiming at wall 0 (west, face +x) or 1.
static int g_corr[4] = {0, 0, 0, 0};  // x0 (west wall block), x1 (east wall block), y (floor top = first air), z (centre)
static bool g_corrOk = false;
static int g_corrBox[6];
CON_COMMAND(pcb_build_corridor, "pcb_build_corridor [mcX=-376 mcY=89 mcZ=442] [gap=12] | remove - build (or remove) two facing stone walls in open air (tests)\n") {
	char c[200];
	if (args.ArgC() > 1 && !std::strcmp(args[1], "remove")) {
		if (!g_corrOk) return console->Print("[bridge] build_corridor: nothing built\n");
		std::snprintf(c, sizeof c, "pcb_mc_cmd fill %d %d %d %d %d %d air", g_corrBox[0], g_corrBox[1], g_corrBox[2], g_corrBox[3], g_corrBox[4], g_corrBox[5]);
		engine.Cmd(c);
		g_corrOk = false;
		return console->Print("[bridge] build_corridor: removed\n");
	}
	const int cx = args.ArgC() > 3 ? std::atoi(args[1]) : -376, y = args.ArgC() > 3 ? std::atoi(args[2]) : 89, cz = args.ArgC() > 3 ? std::atoi(args[3]) : 442;
	const int gi = args.ArgC() > 4 ? 4 : (args.ArgC() == 2 ? 1 : 0);
	const int gap = gi ? std::max(6, std::min(30, std::atoi(args[gi]))) : 12;
	const int x0 = cx - gap / 2, x1 = x0 + gap;
	const int box[6] = {x0, y - 1, cz - 3, x1, y + 4, cz + 3};
	int solid = 0, cells = 0;
	for (int x = box[0] - 1; x <= box[3] + 1; ++x)
		for (int yy = box[1]; yy <= box[4] + 2; ++yy)
			for (int z = box[2] - 1; z <= box[5] + 1; ++z) ++cells, solid += TerrainIsSolid(x, yy, z) || TerrainFluid(x, yy, z) != 0;
	if (solid) {
		std::snprintf(c, sizeof c, "box MC (%d %d %d)-(%d %d %d) has %d of %d cells not air", box[0], box[1], box[2], box[3], box[4], box[5], solid, cells);
		TestCheck(false, "test corridor built in open air", c);
		return;
	}
	std::memcpy(g_corrBox, box, sizeof box);
	std::snprintf(c, sizeof c, "pcb_mc_cmd fill %d %d %d %d %d %d stone", x0, y - 1, cz - 3, x1, y - 1, cz + 3);  // floor
	engine.Cmd(c);
	std::snprintf(c, sizeof c, "pcb_mc_cmd fill %d %d %d %d %d %d stone", x0, y, cz - 3, x0, y + 4, cz + 3);  // west wall
	engine.Cmd(c);
	std::snprintf(c, sizeof c, "pcb_mc_cmd fill %d %d %d %d %d %d stone", x1, y, cz - 3, x1, y + 4, cz + 3);  // east wall
	engine.Cmd(c);
	g_corr[0] = x0, g_corr[1] = x1, g_corr[2] = y, g_corr[3] = cz, g_corrOk = true;
	std::snprintf(c, sizeof c, "walls at MC x %d and %d, y %d..%d, z %d..%d, floor top y %d (gap %d), in open air", x0, x1, y, y + 4, cz - 3, cz + 3, y, gap);
	TestCheck(true, "test corridor built in open air", c);
}
// pcb_spawn_cube_before <portal> [distance_u=80] [name=pcb_cube]: a weighted cube on the ground in front of a
// wall portal (dropped from just above the opening's bottom; the prop collider pool lands it on the terrain)
CON_COMMAND(pcb_spawn_cube_before, "pcb_spawn_cube_before <portal> [distance_u=80] [name=pcb_cube] - spawn a weighted cube in front of a wall portal (tests)\n") {
	const int i = args.ArgC() > 1 ? (std::atoi(args[1]) & 1) : 1;
	const float d = args.ArgC() > 2 ? (float)std::atof(args[2]) : 80.0f;
	if (!g_portalSet[i]) return console->Print("[bridge] spawn_cube_before: portal %d not placed\n", i);
	Vector p = g_portalPos[i] + g_portalNormal[i] * d;
	p.z = g_portalPos[i].z - 56.0f + 30.0f;
	char c[160];
	std::snprintf(c, sizeof c, "pcb_spawn_cube %.1f %.1f %.1f %s", p.x, p.y, p.z, args.ArgC() > 3 ? args[3] : "pcb_cube");
	engine.Cmd(c);
}
// pcb_view_through <portal> <chell|targetname> [distance_blocks=3]: a noclip pose looking through the portal at
// that object: the eye on the line from the object's image behind the portal (the object carried from the
// partner's side) through the opening's centre, `distance` blocks out in open air (the nearest such
// distance from 2 to 8 blocks whose eye is not inside Minecraft blocks)
CON_COMMAND(pcb_view_through, "pcb_view_through <portal> <chell|targetname> [distance_blocks=3] - noclip pose looking through the portal at that object (tests)\n") {
	const int i = args.ArgC() > 1 ? (std::atoi(args[1]) & 1) : 0, j = 1 - i;
	if (!g_portalSet[i] || !g_portalSet[j]) return TestCheck(false, "view through the portal", "no pair");
	Vector obj;
	if (args.ArgC() < 3 || !std::strcmp(args[2], "chell")) obj = PlayerOrigin() + Vector(0, 0, 36);
	else {
		auto v = EntFind(nullptr, args[2]);
		if (v.empty()) return TestCheck(false, "view through the portal", "no such entity");
		obj = EntVec(v[0], "origin");
	}
	// the image Y behind portal i of obj near portal j: portal j's frame -> portal i's frame (turned half a turn)
	const Vector &ni = g_portalNormal[i], &ui = g_portalUp[i], &nj = g_portalNormal[j], &uj = g_portalUp[j];
	auto right = [](const Vector &u, const Vector &n) { return Vector(u.y * n.z - u.z * n.y, u.z * n.x - u.x * n.z, u.x * n.y - u.y * n.x); };
	Vector ri = right(ui, ni), rj = right(uj, nj), d = obj - g_portalPos[j];
	Vector Y = g_portalPos[i] + ni * -Vector::DotProduct(d, nj) + ri * -Vector::DotProduct(d, rj) + ui * Vector::DotProduct(d, uj);
	Vector dir = g_portalPos[i] - Y;
	dir = dir * (1.0f / dir.Length());
	if (Vector::DotProduct(dir, ni) < 0.15f) return TestCheck(false, "view through the portal", "the object is not behind the opening from any pose");
	const float want = args.ArgC() > 3 ? (float)std::atof(args[3]) : 3.0f;
	for (float k = 0; k <= 6; k += 0.5f) {
		for (int sgn = 1; sgn >= -1; sgn -= 2) {
			float dist = want + sgn * k;
			if (dist < 2 || dist > 8) continue;
			Vector eye = g_portalPos[i] + dir * (dist * kUnitsPerBlock);
			int x, y, z;
			Cell(eye, x, y, z);
			if (TerrainIsSolid(x, y, z) || TerrainIsSolid(x, y - 1, z) || TerrainRayBlocked(eye, g_portalPos[i] + ni * 2.0f)) continue;
			float pitch = -std::asin(std::fmax(-1.0f, std::fmin(1.0f, -dir.z))) * 180.0f / (float)M_PI, yaw = std::atan2(-dir.y, -dir.x) * 180.0f / (float)M_PI;
			char cmd[200];
			std::snprintf(cmd, sizeof cmd, "setpos %.2f %.2f %.2f; setang %.2f %.2f 0", eye.x, eye.y, eye.z - 64, pitch, yaw);
			engine.Cmd(cmd);
			PuppetLaunch(Vector(0, 0, 0));
			char buf[200];
			std::snprintf(buf, sizeof buf, "portal %d at %s, eye %.1f blocks out (%.0f %.0f %.0f), pitch %.1f yaw %.1f", i, args.ArgC() > 2 ? args[2] : "chell", dist, eye.x, eye.y, eye.z, pitch, yaw);
			return TestCheck(true, "view through the portal", buf);
		}
	}
	TestCheck(false, "view through the portal", "no open-air eye on the line through the opening");
}
// `waituntil corridor`: the built walls (and the air between them) have streamed
bool CorridorReady() {
	if (!g_corrOk) return true;
	const int mid = (g_corr[0] + g_corr[1]) / 2;
	return TerrainSectionKnown(g_corr[0], g_corr[2] + 2, g_corr[3]) && TerrainIsSolid(g_corr[0], g_corr[2] + 2, g_corr[3]) && TerrainIsSolid(g_corr[1], g_corr[2] + 2, g_corr[3]) &&
	       TerrainIsSolid(g_corr[1], g_corr[2] + 4, g_corr[3] + 2) && !TerrainIsSolid(mid, g_corr[2] + 1, g_corr[3]) && !TerrainIsSolid(g_corr[0] + 1, g_corr[2] + 3, g_corr[3]) &&
	       !TerrainIsSolid(g_corr[1] - 1, g_corr[2] + 3, g_corr[3]);
}
CON_COMMAND(pcb_corridor_aim, "pcb_corridor_aim <0|1> [dz_blocks=0] - noclip player in the corridor's middle aiming at wall 0 (west) or 1 (east), dz blocks off its centre\n") {
	if (!g_corrOk) return console->Print("[bridge] corridor_aim: no corridor\n");
	const int w = args.ArgC() > 1 ? (std::atoi(args[1]) & 1) : 0;
	const double dz = args.ArgC() > 2 ? std::max(-1.6, std::min(1.6, std::atof(args[2]))) : 0.0;
	const double mid = (g_corr[0] + g_corr[1] + 1) * 0.5;
	// the face: west wall's +x face at x0 + 1, east wall's -x face at x1; aimed low (Portal 2's floor snap)
	const double fx = w ? g_corr[1] : g_corr[0] + 1;
	Vector target = McToSrc(fx, g_corr[2] + 1.45, g_corr[3] + 0.5 + dz);
	Vector eye = McToSrc(w ? mid + 2.0 : mid - 2.0, g_corr[2] + 1.62, g_corr[3] + 0.5 + dz);
	Vector d = target - eye;
	float pitch = -std::asin(d.z / d.Length()) * 180.0f / (float)M_PI, yaw = std::atan2(d.y, d.x) * 180.0f / (float)M_PI;
	char cmd[200];
	std::snprintf(cmd, sizeof cmd, "setpos %.2f %.2f %.2f; setang %.2f %.2f 0", eye.x, eye.y, eye.z - 64, pitch, yaw);
	engine.Cmd(cmd);
	PuppetLaunch(Vector(0, 0, 0));
	console->Print("[bridge] corridor_aim %d: %s\n", w, cmd);
}

bool FindPlacedReady() {
	if (!g_placeWait) return true;
	int x, y, z;
	Cell(g_findTarget - Vector(0, 0, 2.0f), x, y, z);
	return TerrainIsSolid(x, y, z) && TerrainNoPortal(x, y, z);
}
}  // namespace bridge
