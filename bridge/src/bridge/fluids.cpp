// Portalcraft bridge: Minecraft water and lava are real Source fluids for Chell.
//
// Portal 2's movement code asks the engine trace for point contents (feet, waist, eyes) to decide
// how deep in water the player is; with nothing there, Chell sank to the bottom of Minecraft lakes.
// IEngineTrace::GetPointContents (slot 0, server and client) is hooked: inside the Minecraft region,
// a point in a Minecraft water or lava cell (the fluid bitsets streamed with the solids) reports
// CONTENTS_WATER, so Source's own swimming takes over. Lava also kills (ClipTick).
#include "bridge.hpp"
#include "slots.hpp"
#include "Offsets.hpp"
#include "coords.hpp"

#include "Modules/Tier1.hpp"
#include "Utils/SDK/Trace.hpp"

#include <cmath>

namespace bridge {

int TerrainFluid(int bx, int by, int bz);  // terrain.cpp
bool ClipRegionContains(const Vector &p);  // clip.cpp

static const int kContentsWater = 0x20;
static Interface *g_svTrace = nullptr, *g_clTrace = nullptr;
using _GetPointContents = int(__rescall *)(void *thisptr, const Vector &pos, int mask, void **ent);
static _GetPointContents SvGetPointContents = nullptr, ClGetPointContents = nullptr;
static int g_fluidHits = 0;
static float g_lastSurfaceZ = 0;
float FluidLastSurfaceZ() { return g_lastSurfaceZ; }

int FluidAt(const Vector &p) {
	if (!ClipRegionContains(p)) return 0;
	double x, y, z;
	SrcToMcD(p, x, y, z);
	return TerrainFluid((int)std::floor(x), (int)std::floor(y), (int)std::floor(z));
}

static int Merge(int c, const Vector &p, int mask) {
	if ((mask & kContentsWater) && FluidAt(p)) {
		++g_fluidHits;
		c |= kContentsWater;
	}
	return c;
}
static int __rescall SvGPC_Hook(void *thisptr, const Vector &p, int mask, void **ent) { return Merge(SvGetPointContents(thisptr, p, mask, ent), p, mask); }
static int __rescall ClGPC_Hook(void *thisptr, const Vector &p, int mask, void **ent) { return Merge(ClGetPointContents(thisptr, p, mask, ent), p, mask); }

// Short rays that end inside a Minecraft block (EngineTraceServer TraceRay, slot 5, same interface
// object as the contents hook): a portal placed on blocks re-tests its resting surface 0.1 s after it
// lands, and then periodically, with short rays straight back from inset points of its opening; with
// no Portal 2 surface there they all missed, Portal 2 closed the portal at open ~0.2 and the gun's
// re-apply restarted it 0.75 s later (a first shot in Minecraft popped in instead of opening). A
// pure ray of at most 4 u inside the region that missed (or stopped later) but enters a solid
// Minecraft block reports a hit on that block's face. Removing the block still fizzles the portal.
using _TraceRay = void(__rescall *)(void *thisptr, const Ray_t &ray, unsigned int mask, ITraceFilter *filter, CGameTrace *tr);
static _TraceRay SvTraceRay = nullptr;
static bool g_surfaceTrace = true;
static int g_surfaceHits = 0, g_surfaceLog = 8;
static void *g_worldEnt = nullptr;
void FluidsLevelInit() { g_worldEnt = nullptr; }
static bool SolidAt(const Vector &p) {
	double x, y, z;
	SrcToMcD(p, x, y, z);
	return TerrainIsSolid((int)std::floor(x), (int)std::floor(y), (int)std::floor(z));
}
// (bridge #21) Rays of any length (Portal 2's lasers, turret bullets, projectiles, use rays) stop at the first Minecraft solid
// block on their way (placed blocks and region terrain), with the block face's normal: the beam ends on the block, and
// whatever is behind it (a laser catcher, a relay, a turret) is not reached.
bool TerrainRay(const double s[3], const double d[3], double &tHit, int &axis, int &sign);  // terrain.cpp
extern long long g_rayCasts, g_rayHits;
static bool g_longTrace = true;
static int g_rayDump = 0;
static int g_longLog = 12;
static double g_longMs = 0;
static void LongRayHit(const Ray_t &ray, unsigned int mask, CGameTrace *tr, bool client = false) {
	const Vector d = ray.m_Delta;
	const Vector s0 = ray.m_Start + ray.m_StartOffset;
	double a[3], b[3];
	SrcToMcD(s0, a[0], a[1], a[2]);
	// only as far as Portal 2's own hit (the rest is behind it): long rays cost what the path to the first wall costs
	const float reach = std::min(1.0f, tr->fraction + 1e-4f);
	SrcToMcD(s0 + d * reach, b[0], b[1], b[2]);
	const double dm[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
	double t;
	int axis, sign;
	if (!TerrainRay(a, dm, t, axis, sign)) return;
	const float f = (float)t * reach;
	if (f >= tr->fraction) return;  // Portal 2's own hit is nearer
	// the face normal in Source axes: the block-side step back, through the same mapping as the positions
	double q[3] = {a[0] + dm[0] * t, a[1] + dm[1] * t, a[2] + dm[2] * t}, q2[3] = {q[0], q[1], q[2]};
	q2[axis] -= sign;
	double sx, sy, sz, tx, ty, tz;
	const Vector p1 = McToSrc(q[0], q[1], q[2]), p2 = McToSrc(q2[0], q2[1], q2[2]);
	(void)sx, (void)sy, (void)sz, (void)tx, (void)ty, (void)tz;
	Vector n = p2 - p1;
	if (n.Length() > 1e-6f) n = n * (1.0f / n.Length());
	tr->fraction = f;
	tr->endpos = s0 + d * f;
	tr->plane.normal = n;
	tr->plane.dist = Vector::DotProduct(n, tr->endpos);
	tr->contents = CONTENTS_SOLID;
	tr->startsolid = false;
	tr->allsolid = false;
	tr->fractionleftsolid = 0;
	if (!client) {  // (the client's world entity is its own: left as Portal 2 set it)
		if (!g_worldEnt) {
			auto w = EntFind("worldspawn");
			g_worldEnt = w.empty() ? nullptr : w[0];
		}
		tr->m_pEnt = g_worldEnt;
	}
	if (g_longLog > 0) --g_longLog, console->Print("[bridge] trace: long ray len %.0f mask 0x%x from (%.0f %.0f %.0f) stops at a Minecraft block, fraction %.3f, normal (%.0f %.0f %.0f)\n", d.Length(), mask, s0.x, s0.y, s0.z, f, n.x, n.y, n.z);
}
CON_COMMAND(pcb_ray_dump, "pcb_ray_dump <n> - (tests) log the next n server rays longer than 100 u (mask, extents, result)\n") {
	g_rayDump = args.ArgC() > 1 ? std::atoi(args[1]) : 30;
}
CON_COMMAND(pcb_long_trace, "pcb_long_trace [0|1] - server rays of any length stop at Minecraft solid blocks (lasers, bullets; default 1); prints the counters\n") {
	if (args.ArgC() > 1) g_longTrace = std::atoi(args[1]) != 0, g_longLog = 12;
	console->Print("[bridge] long trace %d: %lld rays walked through the solids, %lld hit a block\n", g_longTrace, g_rayCasts, g_rayHits);
}
static _TraceRay ClTraceRay = nullptr;
// the client's own traces (the beam's drawn length, client-side effects) stop at the same blocks
static void __rescall ClTraceRay_Hook(void *thisptr, const Ray_t &ray, unsigned int mask, ITraceFilter *filter, CGameTrace *tr) {
	ClTraceRay(thisptr, ray, mask, filter, tr);
	if (!g_longTrace || !tr || !ray.m_IsRay || !(mask & CONTENTS_SOLID)) return;
	const float len = ray.m_Delta.Length();
	if (len > 4.0f && len < 100000.0f) LongRayHit(ray, mask, tr, true);
}
static void __rescall SvTraceRay_Hook(void *thisptr, const Ray_t &ray, unsigned int mask, ITraceFilter *filter, CGameTrace *tr) {
	SvTraceRay(thisptr, ray, mask, filter, tr);
	if (g_rayDump > 0 && tr && ray.m_Delta.Length() > 100.0f) {
		--g_rayDump;
		console->Print("[bridge] ray dump: len %.0f isRay %d swept %d ext (%.1f %.1f %.1f) mask 0x%x from (%.0f %.0f %.0f) dir (%.2f %.2f %.2f) -> fraction %.3f ent %p\n", ray.m_Delta.Length(),
		               (int)ray.m_IsRay, (int)ray.m_IsSwept, ray.m_Extents.x, ray.m_Extents.y, ray.m_Extents.z, mask, ray.m_Start.x, ray.m_Start.y, ray.m_Start.z,
		               ray.m_Delta.x / ray.m_Delta.Length(), ray.m_Delta.y / ray.m_Delta.Length(), ray.m_Delta.z / ray.m_Delta.Length(), tr->fraction, tr->m_pEnt);
	}
	if (!g_surfaceTrace || !tr || !ray.m_IsRay || !(mask & CONTENTS_SOLID)) return;
	const Vector d = ray.m_Delta;
	const float len = d.Length();
	if (len > 4.0f && g_longTrace && len < 100000.0f) {
		LongRayHit(ray, mask, tr);
		return;
	}
	if (len < 0.01f || len > 4.0f) return;
	const Vector s = ray.m_Start + ray.m_StartOffset;
	if (!ClipRegionContains(s) || tr->startsolid) return;
	// the first point along the ray inside a block (1/8 u steps), before Portal 2's own hit
	const int n = (int)std::ceil(len * 8.0f);
	for (int k = 0; k <= n; ++k) {
		const float f = (float)k / n;
		if (f >= tr->fraction) return;
		if (!SolidAt(s + d * f)) continue;
		if (k == 0) return;  // starts inside a block: not a surface test (and the hull hooks own those)
		const Vector dir = d * (1.0f / len);
		tr->fraction = f;
		tr->endpos = s + d * f;
		tr->plane.normal = Vector(-dir.x, -dir.y, -dir.z);
		tr->plane.dist = Vector::DotProduct(tr->plane.normal, tr->endpos);
		tr->contents = CONTENTS_SOLID;
		tr->allsolid = false;
		if (!tr->m_pEnt) {
			if (!g_worldEnt) {
				auto w = EntFind("worldspawn");
				g_worldEnt = w.empty() ? nullptr : w[0];
			}
			tr->m_pEnt = g_worldEnt;
		}
		++g_surfaceHits;
		if (g_surfaceLog > 0) --g_surfaceLog, console->Print("[bridge] trace: short ray at (%.1f %.1f %.1f) dir (%.2f %.2f %.2f) len %.2f mask 0x%x hits a Minecraft block at %.3f\n", s.x, s.y, s.z, dir.x, dir.y, dir.z, len, mask, f);
		return;
	}
}
CON_COMMAND(pcb_surface_trace, "pcb_surface_trace [0|1] - short server rays in the region hit Minecraft blocks (portal resting-surface test; default 1)\n") {
	if (args.ArgC() > 1) g_surfaceTrace = std::atoi(args[1]) != 0, g_surfaceLog = 8;
	console->Print("[bridge] surface trace %d: %d short rays hit Minecraft blocks so far\n", g_surfaceTrace, g_surfaceHits);
}

CON_COMMAND(pcb_src_block, "pcb_src_block <x y z> <block> [x2 y2 z2] - (tests) place a Minecraft block (or fill a box) at the Portal 2 point(s)\n") {
	if (args.ArgC() < 5) return;
	double a[3], b[3];
	SrcToMcD(Vector((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3])), a[0], a[1], a[2]);
	b[0] = a[0], b[1] = a[1], b[2] = a[2];
	if (args.ArgC() >= 8) SrcToMcD(Vector((float)std::atof(args[5]), (float)std::atof(args[6]), (float)std::atof(args[7])), b[0], b[1], b[2]);
	char cmd[256];
	std::snprintf(cmd, sizeof cmd, "pcb_mc_cmd fill %d %d %d %d %d %d %s", (int)std::floor(a[0]), (int)std::floor(a[1]), (int)std::floor(a[2]), (int)std::floor(b[0]), (int)std::floor(b[1]), (int)std::floor(b[2]), args[4]);
	engine.Cmd(cmd);
}
CON_COMMAND(pcb_trace_probe, "pcb_trace_probe <x y z> <dx dy dz> <len> - (tests) a pure server ray through the live trace interface: where it stops\n") {
	if (args.ArgC() < 8 || !g_svTrace) return;
	const Vector s((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3]));
	Vector d((float)std::atof(args[4]), (float)std::atof(args[5]), (float)std::atof(args[6]));
	d = d * ((float)std::atof(args[7]) / d.Length());
	struct F : public ITraceFilter {
		virtual bool ShouldHitEntity(void *, int) { return false; }
		virtual TraceType_t GetTraceType() const { return TRACE_WORLD_ONLY; }
	} f;
	Ray_t ray;
	ray.m_Delta = VectorAligned(d.x, d.y, d.z);
	ray.m_IsSwept = true;
	ray.m_Extents = VectorAligned(0, 0, 0);
	ray.m_IsRay = true;
	ray.m_StartOffset = VectorAligned(0, 0, 0);
	ray.m_Start = VectorAligned(s.x, s.y, s.z);
	ray.m_pWorldAxisTransform = nullptr;
	CGameTrace tr;
	std::memset(&tr, 0, sizeof tr);
	void *obj = g_svTrace->ThisPtr();
	auto fn = (_TraceRay)(*(void ***)obj)[Offsets::TraceRay];
	fn(obj, ray, 0x4600400b /*MASK_SHOT-ish*/, &f, &tr);
	console->Print("[bridge] trace probe from (%.0f %.0f %.0f) len %.0f: fraction %.3f end (%.1f %.1f %.1f) normal (%.0f %.0f %.0f) startsolid %d\n", s.x, s.y, s.z, d.Length(), tr.fraction,
	               tr.endpos.x, tr.endpos.y, tr.endpos.z, tr.plane.normal.x, tr.plane.normal.y, tr.plane.normal.z, tr.startsolid);
}

bool BridgeSkip(const char *name);  // plugin.cpp (PCB_SKIP bisect aid)
bool FluidsInit() {
	g_svTrace = Interface::Create(MODULE_NAME("engine"), "EngineTraceServer004");
	g_clTrace = Interface::Create(MODULE_NAME("engine"), "EngineTraceClient004");
	bool ok = g_svTrace && (BridgeSkip("fluids.svgpc") || slots::Hook(g_svTrace, slots::kTraceGetPointContents, SvGPC_Hook, SvGetPointContents));
	if (g_svTrace && !BridgeSkip("fluids.svray")) g_svTrace->Hook(SvTraceRay_Hook, SvTraceRay, Offsets::TraceRay);
	if (g_clTrace && !BridgeSkip("fluids.clgpc")) slots::Hook(g_clTrace, slots::kTraceGetPointContents, ClGPC_Hook, ClGetPointContents);
	if (g_clTrace && !BridgeSkip("fluids.clray")) g_clTrace->Hook(ClTraceRay_Hook, ClTraceRay, Offsets::TraceRay);
	return ok;
}

void FluidsShutdown() {
	Interface::Delete(g_clTrace);
	Interface::Delete(g_svTrace);
	g_svTrace = g_clTrace = nullptr;
}

CON_COMMAND(pcb_fluid, "pcb_fluid [x y z] - Minecraft fluid at a Source point (default: the player's feet + 8 u)\n") {
	Vector p = args.ArgC() > 3 ? Vector((float)std::atof(args[1]), (float)std::atof(args[2]), (float)std::atof(args[3])) : PlayerOrigin() + Vector(0, 0, 8);
	console->Print("[bridge] fluid at (%.1f %.1f %.1f): %d (0 none, 1 water, 2 lava), %d water contents reported so far\n", p.x, p.y, p.z, FluidAt(p), g_fluidHits);
}

CON_COMMAND(pcb_find_water, "pcb_find_water [radius_blocks=64] - nearest Minecraft water surface (2+ deep) to the player; prints a Source point above it\n") {
	int rad = args.ArgC() > 1 ? std::atoi(args[1]) : 64;
	double px, py, pz;
	SrcToMcD(PlayerOrigin(), px, py, pz);
	int bx = (int)std::floor(px), bz = (int)std::floor(pz);
	double best = 1e18;
	int bxx = 0, byy = 0, bzz = 0;
	for (int dz = -rad; dz <= rad; ++dz)
		for (int dx = -rad; dx <= rad; ++dx)
			for (int y = 100; y >= 48; --y) {
				int f = TerrainFluid(bx + dx, y, bz + dz);
				if (!f) continue;
				if (f == 1 && TerrainFluid(bx + dx, y - 1, bz + dz) == 1 && TerrainFluid(bx + dx, y - 2, bz + dz) == 1) {
					double d = dx * dx + dz * dz;
					if (d < best) best = d, bxx = bx + dx, byy = y, bzz = bz + dz;
				}
				break;
			}
	if (best > 1e17) return console->Print("[bridge] no water within %d blocks\n", rad);
	Vector s = McToSrc(bxx + 0.5, byy + 1.0, bzz + 0.5);
	g_lastSurfaceZ = s.z;
	console->Print("[bridge] water: surface block MC (%d %d %d), %.0f blocks away; above it Source (%.1f %.1f %.1f)\n", bxx, byy, bzz, std::sqrt(best), s.x, s.y, s.z);
	char cmd[128];
	std::snprintf(cmd, sizeof cmd, "setpos %.1f %.1f %.1f", s.x, s.y, s.z + 40);
	if (args.ArgC() > 2) engine.Cmd(cmd), console->Print("[bridge] %s\n", cmd);
}

}  // namespace bridge
