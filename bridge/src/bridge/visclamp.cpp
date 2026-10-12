// Portalcraft bridge (bridge #41): the world stays drawn while the camera is in Portal 2's solid / outside its world (a dug tunnel beyond the map).
//
// The problem (the user, 2026-10-04, after tunnelling past the edge of the level): the whole Portal 2 level vanished and the view through the tunnel's
// mouth was solid white. Portal 2 works out what to draw (the potentially visible leaves and the open areas) from the view's origin; an origin
// in the solid void outside the level (or inside a wall brush) is in no valid area, so nothing is marked visible, the world is not drawn and the
// frame clears to white. A tunnel past the map puts the camera there: the Minecraft blocks are drawn (they are ours), the chamber is not.
//
// The fix: the camera, the frustum and the projection stay exactly as they are (the blocks and the hole's punch must line up with them), only the
// ORIGIN THE ENGINE TAKES ITS VISIBILITY FROM is replaced while the camera is bad: the last camera position that was in the open world (the tunnel's
// mouth, in the chamber the player dug out of). From there the engine marks the chamber's leaves and areas visible as it does for a player standing
// at the mouth, and everything seen through the hole is drawn. The same trick Portal 2 itself uses for portal views (a vis origin that is not the eye).
//
// How: the client asks the engine for visibility once per 3D view with IVRenderView::ViewSetupVisEx(novis, n, origins[], flags) (a vtable call on
// the engine's render interface, VEngineRenderView013; slot 43 in this build, read off the client's own call site, one after SetMainView at 42).
// That slot is hooked: an origin that matches the main camera of this frame and was found bad (in solid / outside the world, checked once a frame
// in OverrideView, where the engine's contents queries are safe) is swapped for the last good one. Every other origin (portal views, the sky) is
// passed through untouched.
//
// Generality: it is a property of the camera's position only. No map, no mode (Chell's noclip or a tunnel as Chell works too), no portal is special.
// It changes nothing while the camera is in the open world. `pc_vis_clamp 0` turns it off, `pc_vis_clamp 2` also forces no-vis (everything the
// frustum sees is drawn: the heavy fallback, in case taking the vis from the mouth is not enough).
//
// (bridge #44) Two more jobs on the same interface:
//  * VISIBILITY THROUGH HOLES IN MOVERS. ViewSetupVisEx takes a list of origins and merges their visibility, so the main camera's call also gets sample
//    points in the open air just outside the pieces mined out of movers near the camera (a hole in a closed door): the room behind is visible too.
//    (bridge #75: the world's dug cells are cutvis.cpp's, for every view and the server, under pc_dig_render 2, the default.)
//  * THE WORLD'S DRAW (IVRenderView slot 13) and the translucent world surfaces (slot 19) are hooked so the cut-away shaders see each view's
//    matrices (cutrender.cpp); the #44 replay of the world behind a punched hole is gone with the punch.
#include "bridge.hpp"
#include "slots.hpp"
#include "prof.hpp"
#include "coords.hpp"
#include "dig.hpp"

#include "Modules/Tier1.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "platform/platform.hpp"

namespace bridge {

void CutWorldDrawBegin();  // cutrender.cpp (bridge #73)
int CutVisExpand(const Vector &o, Vector *out, int max, bool *replaced);  // cutvis.cpp (bridge #73)
bool CutVisInit();
void CutVisShutdown();
void CutWorldDrawEnd();
void CutFrame();
void CutPerfDrawBegin();  // cutrender.cpp (bridge #75)
void CutPerfDrawEnd();
void CutPerfFrame();
int DigRenderMode();
bool CutVisFogOrigin(const Vector &eye, Vector &out);  // cutvis.cpp (bridge #75)

namespace {

using _ViewSetupVisEx = void(__rescall *)(void *thisptr, bool novis, int numorigins, const Vector *origins, unsigned int &returnFlags);

// IVRenderView::DrawWorldLists (slot 13, the same place as in the public SDK; read off the client's own call site: this, an extra first word, the world
// list, the flags built from the view's draw flags, and the water height word)
using _DrawWorldLists = void(__rescall *)(void *thisptr, int a1, void *list, unsigned long flags, int a4);

Interface *g_render = nullptr;
_ViewSetupVisEx g_orig = nullptr;
_DrawWorldLists g_origWL = nullptr;
long g_wlCalls = 0, g_frames = 0;

int g_mode = 1;                // pc_vis_clamp: 0 off, 1 vis origin from the last good camera, 2 plus no-vis
int g_sampling = 1;            // (bridge #44) sample origins around the dug boxes near the camera
std::vector<Vector> g_samples;  // this frame's extra vis origins
Vector g_cam(0, 0, 0);         // this frame's main camera
long g_augmented = 0, g_sampleBuilds = 0;
int g_lastBoxes = 0, g_lastCandidates = 0;
uint32_t g_sampleGen = 0xFFFFFFFFu, g_sampleMoverKey = 0;
int g_sampleCell[3] = {1 << 30, 0, 0};
const int kMaxSamples = 20;
bool g_active = false;         // this frame's main camera is bad and a good origin is known
bool g_haveGood = false;
Vector g_good(0, 0, 0), g_bad(0, 0, 0);
long g_framesBad = 0, g_swaps = 0, g_noGood = 0;
int g_logged = 0;
bool g_wasBad = false;
Vector g_lastMainOrigins[32];
int g_lastMainN = 0;

constexpr int kMaskSolidContents = 0x1;   // CONTENTS_SOLID
constexpr float kMaxFallbackDistance = 6000.0f;  // a "last good" position further than this is another place (a teleport): not used

void __rescall ViewSetupVisEx_Hook(void *thisptr, bool novis, int numorigins, const Vector *origins, unsigned int &returnFlags) {
	// (bridge #73) pc_dig_render 2: every view's origins go through the cut rule (cutvis.cpp): an origin inside a cut becomes the
	// cut's mouths, any other origin gains the mouths of the cuts its cluster can see. The old main-view samples are not used.
	if (DigRenderMode() == 2 && origins && numorigins >= 1 && numorigins <= 8) {
		Vector tmp[32];
		int n = 0;
		bool changed = false, main = false;
		for (int i = 0; i < numorigins; ++i) {
			const bool isMain = (origins[i] - g_cam).Length() < 8.0f;
			main |= isMain;
			Vector e[24];
			bool rep = false;
			int k = CutVisExpand(origins[i], e, 24, &rep);
			if (!rep && isMain && g_active) e[0] = g_good, rep = true, ++g_swaps;  // the camera in solid with no cut there (noclip): the last open position, as before
			changed |= rep || k != 1;
			for (int j = 0; j < k && n < 30; ++j) tmp[n++] = e[j];
		}
		// holes mined in movers keep #46's samples (only those: digrender.cpp leaves the dug cells to the cut rule in this mode)
		if (main && g_sampling && g_mode)
			for (const Vector &sp : g_samples)
				if (n < 30) tmp[n++] = sp, changed = true;
		if (main) {
			g_lastMainN = n;
			for (int i = 0; i < n && i < 32; ++i) g_lastMainOrigins[i] = tmp[i];
		}
		if (changed) {
			++g_augmented;
			g_orig(thisptr, novis, n, tmp, returnFlags);
		} else {
			g_orig(thisptr, novis, numorigins, origins, returnFlags);
		}
		return;
	}
	if (g_mode && origins && numorigins >= 1 && numorigins <= 8 && (g_active || !g_samples.empty())) {
		bool main = false;
		for (int i = 0; i < numorigins; ++i) main |= (origins[i] - g_cam).Length() < 8.0f;
		if (main) {
			Vector tmp[32];
			int n = 0;
			bool replaced = false;
			for (int i = 0; i < numorigins; ++i) {
				tmp[n] = origins[i];
				if (g_active && (origins[i] - g_cam).Length() < 8.0f) tmp[n] = g_good, replaced = true;
				++n;
			}
			// (bridge #44) the open air around the holes near the camera: their leaves and areas are visible as well
			if (g_sampling && g_mode)
				for (const Vector &sp : g_samples)
					if (n < 30) tmp[n++] = sp;
			if (replaced) ++g_swaps;
			if (n > numorigins) ++g_augmented;
			g_lastMainN = n;  // (bridge #73: the harness reports what the main view handed the engine)
			for (int i = 0; i < n && i < 32; ++i) g_lastMainOrigins[i] = tmp[i];
			g_orig(thisptr, novis || (replaced && g_mode >= 2), n, tmp, returnFlags);
			return;
		}
	}
	if (origins && numorigins >= 1 && numorigins <= 8)
		for (int i = 0; i < numorigins; ++i)
			if ((origins[i] - g_cam).Length() < 8.0f) {
				g_lastMainN = std::min(numorigins, 32);
				for (int k = 0; k < g_lastMainN; ++k) g_lastMainOrigins[k] = origins[k];
				break;
			}
	g_orig(thisptr, novis, numorigins, origins, returnFlags);
}

void __rescall DrawWorldLists_Hook(void *thisptr, int a1, void *list, unsigned long flags, int a4) {
	++g_wlCalls;
	CutPerfDrawBegin();   // (bridge #75) pc_cut_perf: GPU timestamps around the world draw
	CutWorldDrawBegin();  // (bridge #73) the cut-away world shaders see this view's matrices, and only the world's draws (cutrender.cpp)
	g_origWL(thisptr, a1, list, flags, a4);
	CutWorldDrawEnd();
	CutPerfDrawEnd();
}

// (bridge #73) IVRenderView slot 19: the translucent world surfaces of a leaf (read off the client's two call sites in the translucent
// pass: this, an extra first word, the world list, the sort index, the flags, the shadow-depth flag). Cut like the opaque world.
using _DrawTranslucentSurfaces = void(__rescall *)(void *thisptr, int a1, void *list, int sortIndex, unsigned long flags, int shadowDepth);
_DrawTranslucentSurfaces g_origTS = nullptr;
void __rescall DrawTranslucentSurfaces_Hook(void *thisptr, int a1, void *list, int sortIndex, unsigned long flags, int shadowDepth) {
	CutPerfDrawBegin();
	CutWorldDrawBegin();
	g_origTS(thisptr, a1, list, sortIndex, flags, shadowDepth);
	CutWorldDrawEnd();
	CutPerfDrawEnd();
}

// (bridge #75) IVRenderView slot 10: the fog volume / water the view sees, from an eye point (the public SDK's order: the slot before
// the world-list pair, 11 CreateWorldList and 13 DrawWorldLists, which this build keeps). The client asks it once per scene view with
// the view's origin and decides from the answer whether to draw water at all. An eye inside a dug cut is in a solid leaf (no water
// data): the cut's mouth answers for it (cutvis.cpp). The arguments are forwarded untouched otherwise; the first is only read when
// it is a readable pointer to a point within 8 u of this frame's main camera (checked with process_vm_readv: no fault on a bad guess).
#ifdef _WIN32
// Windows: thiscall is callee-cleaned, so the argument count must be exact. The method behind slot 10 ends in `ret 12`: three arguments (Proton run 2026-10-11;
// with four, the hook popped 4 bytes too many and the game returned into the stack).
using _FogVolume = void *(__rescall *)(void *thisptr, void *a1, void *a2, void *a3);
#else
using _FogVolume = void *(__rescall *)(void *thisptr, void *a1, void *a2, void *a3, void *a4);
#endif
_FogVolume g_origFog = nullptr;
long g_fogCalls = 0, g_fogMain = 0, g_fogSwapped = 0;
bool ReadableVec(const void *p, Vector &v) {
	return plat::SafeRead(p, &v, sizeof v);
}
#ifdef _WIN32
void *__rescall FogVolume_Hook(void *thisptr, void *a1, void *a2, void *a3) {
#else
void *__rescall FogVolume_Hook(void *thisptr, void *a1, void *a2, void *a3, void *a4) {
#endif
	++g_fogCalls;
	Vector eye;
	if (DigRenderMode() == 2 && DigEnabled() && DigAny() && ReadableVec(a1, eye) && std::isfinite(eye.x) && (eye - g_cam).Length() < 8.0f) {
		++g_fogMain;
		Vector m;
		if (CutVisFogOrigin(eye, m)) {
			++g_fogSwapped;
			static Vector s_origin;  // (a stable address for the call; the render thread is the only caller)
			s_origin = m;
#ifdef _WIN32
			return g_origFog(thisptr, &s_origin, a2, a3);
#else
			return g_origFog(thisptr, &s_origin, a2, a3, a4);
#endif
		}
	}
#ifdef _WIN32
	return g_origFog(thisptr, a1, a2, a3);
#else
	return g_origFog(thisptr, a1, a2, a3, a4);
#endif
}

}  // namespace

bool VisClampInit() {
	g_render = Interface::Create(MODULE_NAME("engine"), "VEngineRenderView013");
	if (!g_render) {
		console->Warn("[bridge] vis clamp: the engine's render interface was not found (continuing without it)\n");
		return false;
	}
	if (g_render->vtableSize <= slots::Index(slots::kRvViewSetupVisEx) + 1 || !slots::Hook(g_render, slots::kRvViewSetupVisEx, ViewSetupVisEx_Hook, g_orig)) {
		console->Warn("[bridge] vis clamp: the render interface has %d slots: not hooked (continuing without it)\n", g_render->vtableSize);
		Interface::Delete(g_render);
		g_render = nullptr;
		return false;
	}
	if (!slots::Hook(g_render, slots::kRvDrawWorldLists, DrawWorldLists_Hook, g_origWL)) {
		g_origWL = nullptr;
		console->Warn("[bridge] vis clamp: the world draw (slot %d) was not hooked: dug cells are not cut out of the world\n", slots::Index(slots::kRvDrawWorldLists));
	}
	CutVisInit();  // (bridge #73) the server's PVS through dug cells
	if (!slots::Hook(g_render, slots::kRvDrawTranslucentSurfaces, DrawTranslucentSurfaces_Hook, g_origTS)) {
		g_origTS = nullptr;
		console->Warn("[bridge] vis clamp: the translucent world draw (slot %d) was not hooked\n", slots::Index(slots::kRvDrawTranslucentSurfaces));
	}
	if (!slots::Hook(g_render, slots::kRvFogVolume, FogVolume_Hook, g_origFog)) {
		g_origFog = nullptr;
		console->Warn("[bridge] vis clamp: the fog-volume query (slot %d) was not hooked: no water seen from inside a cut\n", slots::Index(slots::kRvFogVolume));
	}
	return true;
}

// (bridge #73) the vis origins the main camera's last ViewSetupVisEx passed on to the engine (digharness.cpp)
int VisLastMainOrigins(Vector *out, int max) {
	const int n = std::min(g_lastMainN, max);
	for (int i = 0; i < n; ++i) out[i] = g_lastMainOrigins[i];
	return n;
}

// (bridge #51) Is a 3D view's origin the player's main camera (this frame's, set in OverrideView)? The view's ANGLES cannot tell a portal view from the main
// view: two portals that face each other (opposite normals: a corridor's side walls, a floor and a ceiling) have the identity as their transform, so the
// virtual camera looks along the same angles as the player and only its position differs. digrender.cpp and render.cpp ask this.
Vector VisMainCamera() { return g_cam; }  // (bridge #73) cutvis.cpp's server hook
bool IsMainCameraOrigin(const Vector &origin) { return g_frames == 0 || (origin - g_cam).Length() < 8.0f; }

void CutShutdown();  // cutrender.cpp
void VisClampShutdown() {
	CutShutdown();  // (bridge #73) togl's link entry back to its own before this code goes
	CutVisShutdown();
	if (g_render) Interface::Delete(g_render);  // (puts the engine's own vtable back)
	g_render = nullptr;
	g_active = false;
}

void DigPunchBoxes(const Vector &eye, std::vector<std::pair<Vector, Vector>> &out, uint32_t *moverKey);  // digrender.cpp
uint32_t MoverPiecesKey(const Vector &eye);                                                       // digrender.cpp

namespace {
bool OpenAir(const Vector &p) {
	void *tr = engine.traceServer->ThisPtr();
	if (engine.PointOutsideWorld(tr, p)) return false;
	return (engine.GetPointContentsWorld(tr, p, 0x1) & kMaskSolidContents) == 0;
}

// (bridge #44) Points in the open air just outside the faces of the dug boxes near the camera, for the visibility merge. The boxes are the punch's
// (disjoint, grown 0.5 u): a point 6 u outside a face is in the air that touches the hole there (the chamber's, or what lies on the other side of a thin
// wall), unless it is inside solid or beyond the world, or inside another dug box. The ones on faces that turn away from the camera come first (their
// leaves are the ones the camera's own leaf does not see), then the nearest; points closer than 24 u to one already taken are dropped.
void RebuildSamples(const Vector &eye) {
	++g_sampleBuilds;
	g_samples.clear();
	std::vector<std::pair<Vector, Vector>> boxes;
	DigPunchBoxes(eye, boxes, nullptr);
	g_lastBoxes = (int)boxes.size();
	if (boxes.empty()) return;
	auto boxDist = [&](const std::pair<Vector, Vector> &b) {
		Vector d(0, 0, 0);
		for (int a = 0; a < 3; ++a) (&d.x)[a] = std::fmax(std::fmax((&b.first.x)[a] - (&eye.x)[a], (&eye.x)[a] - (&b.second.x)[a]), 0.0f);
		return d.Length();
	};
	std::sort(boxes.begin(), boxes.end(), [&](const std::pair<Vector, Vector> &a, const std::pair<Vector, Vector> &b) { return boxDist(a) < boxDist(b); });
	if (boxes.size() > 24) boxes.resize(24);
	struct Cand {
		Vector p;
		int prio;
		float dist;
	};
	std::vector<Cand> cands;
	for (size_t bi = 0; bi < boxes.size(); ++bi) {
		const Vector mn = boxes[bi].first, mx = boxes[bi].second;
		for (int a = 0; a < 3; ++a)
			for (int sgn = -1; sgn <= 1; sgn += 2) {
				const int u = (a + 1) % 3, v = (a + 2) % 3;
				const float eu = (&mx.x)[u] - (&mn.x)[u], ev = (&mx.x)[v] - (&mn.x)[v];
				const int nu = std::max(1, std::min(3, (int)std::ceil(eu / 40.0f))), nv = std::max(1, std::min(3, (int)std::ceil(ev / 40.0f)));
				for (int iu = 0; iu < nu; ++iu)
					for (int iv = 0; iv < nv; ++iv) {
						Vector p(0, 0, 0);
						(&p.x)[a] = sgn > 0 ? (&mx.x)[a] + 6.0f : (&mn.x)[a] - 6.0f;
						(&p.x)[u] = (&mn.x)[u] + eu * (iu + 0.5f) / nu;
						(&p.x)[v] = (&mn.x)[v] + ev * (iv + 0.5f) / nv;
						bool inside = false;  // in another dug box: not open air of the host's, its neighbour box has its own faces
						for (size_t bj = 0; bj < boxes.size() && !inside; ++bj)
							inside = p.x > boxes[bj].first.x && p.x < boxes[bj].second.x && p.y > boxes[bj].first.y && p.y < boxes[bj].second.y && p.z > boxes[bj].first.z && p.z < boxes[bj].second.z;
						if (inside || !OpenAir(p)) continue;
						const float away = (&p.x)[a] * sgn - (&eye.x)[a] * sgn;  // > 0: the point is beyond the face as the camera sees it
						cands.push_back({p, away > 0 ? 0 : 1, (p - eye).Length()});
					}
			}
	}
	g_lastCandidates = (int)cands.size();
	std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.prio != b.prio ? a.prio < b.prio : a.dist < b.dist; });
	for (const Cand &c : cands) {
		if ((int)g_samples.size() >= kMaxSamples) break;
		bool near = false;
		for (const Vector &q : g_samples) near |= (q - c.p).Length() < 24.0f;
		if (!near) g_samples.push_back(c.p);
	}
}
}  // namespace

// Once a frame (mc.cpp OverrideView, after every adjustment of the camera): is the main camera in the open world? `live` = a level is running.
void VisClampFrame(const Vector &eye, bool live) {
	PROF("visclamp");
	++g_frames;
	g_cam = eye;
	CutPerfFrame();
	if (live) CutFrame();  // (bridge #73) cutrender.cpp: install when switched on, keep its textures in step with the dug cells
	if (!g_render || !g_mode || !live || !engine.traceServer || !engine.PointOutsideWorld || !engine.GetPointContentsWorld) {
		g_active = false, g_haveGood = g_haveGood && live;  // (a new level forgets the last good camera)
		g_wasBad = false;
		g_samples.clear();
		return;
	}
	// (bridge #44) the open air around the holes near the camera (rebuilt when the dug set or the camera's 20 u cell changes)
	if (g_sampling && DigEnabled() && MoverHolesAny()) {
		const int cell[3] = {(int)std::floor(eye.x / 20.0f), (int)std::floor(eye.y / 20.0f), (int)std::floor(eye.z / 20.0f)};
		const uint32_t gen = DigGen();
		const uint32_t mkey = MoverPiecesKey(eye);  // (bridge #46) holes in movers: rebuilt when a door moves
		if (gen != g_sampleGen || mkey != g_sampleMoverKey || cell[0] != g_sampleCell[0] || cell[1] != g_sampleCell[1] || cell[2] != g_sampleCell[2]) {
			g_sampleGen = gen, g_sampleMoverKey = mkey, g_sampleCell[0] = cell[0], g_sampleCell[1] = cell[1], g_sampleCell[2] = cell[2];
			RebuildSamples(eye);
			static int logged = 0;
			if (logged < 6 && !g_samples.empty()) ++logged, console->Print("[bridge] vis clamp: %zu sample points in the open air around %d dug boxes (of %d candidates) are merged into the camera's visibility, so what lies beyond a hole is drawn\n", g_samples.size(), g_lastBoxes, g_lastCandidates);
		}
	} else {
		g_samples.clear();
		g_sampleGen = 0xFFFFFFFFu;
	}
	// (bridge #49) the virtual region (stage.cpp) is void on purpose: Minecraft's picture is all there is to see there, and the chamber must not be drawn from a "last open position"
	if (InStageBox(eye)) {
		g_active = false, g_wasBad = false;
		return;
	}
	void *tr = engine.traceServer->ThisPtr();
	const bool out = engine.PointOutsideWorld(tr, eye);
	const bool solid = (engine.GetPointContentsWorld(tr, eye, 0x1 /* CONTENTS_SOLID */) & kMaskSolidContents) != 0;
	if (!out && !solid) {
		g_good = eye, g_haveGood = true, g_active = false;
		if (g_wasBad && g_logged < 30) ++g_logged, console->Print("[bridge] vis clamp: the camera is back in the open world (%ld frames out, %ld views taken from the last good position)\n", g_framesBad, g_swaps);
		g_wasBad = false;
		return;
	}
	++g_framesBad;
	g_bad = eye;
	g_active = g_haveGood && (eye - g_good).Length() < kMaxFallbackDistance;
	if (!g_haveGood || !g_active) ++g_noGood;
	if (!g_wasBad && g_logged < 30) {
		++g_logged;
		if (g_active)
			console->Print("[bridge] vis clamp: the camera (%.0f %.0f %.0f) is %s Portal 2's solid: the world's visibility is taken from the last open position (%.0f %.0f %.0f) so the chamber stays drawn\n", eye.x, eye.y, eye.z,
			               out ? "outside the world, in" : "in", g_good.x, g_good.y, g_good.z);
		else
			console->Print("[bridge] vis clamp: the camera (%.0f %.0f %.0f) is in Portal 2's solid and no open position is known (a teleport?): nothing to take the visibility from\n", eye.x, eye.y, eye.z);
	}
	g_wasBad = true;
}

CON_COMMAND(pc_vis_clamp, "pc_vis_clamp [0|1|2] [samples 0|1] - while the camera is in Portal 2's solid or outside the level (a tunnel beyond the map), take the world's visibility from the last open camera position so the chamber stays drawn (default 1; 2 = also no-vis, heavy); samples (default 1): also merge the visibility of the open air around nearby dug holes, so what lies beyond a hole is drawn\n") {
	if (args.ArgC() > 1) g_mode = std::atoi(args[1]);
	if (args.ArgC() > 2) g_sampling = std::atoi(args[2]) != 0;
	console->Print("[bridge] vis clamp %d (%s): hook %s, %ld frames, %ld bad-camera frames (%ld without a good position), %ld view setups swapped; now %s\n", g_mode, g_mode == 0 ? "off" : g_mode == 1 ? "vis origin" : "vis origin + no-vis",
	               g_render ? "installed" : "NOT installed", g_frames, g_framesBad, g_noGood, g_swaps, g_active ? "swapping" : "passing through");
	console->Print("[bridge] fog-volume query (slot 10) %s: %ld calls, %ld at the main camera, %ld answered from a cut's mouth\n", g_origFog ? "hooked" : "NOT hooked", g_fogCalls, g_fogMain, g_fogSwapped);
	console->Print("[bridge] vis samples %s: %zu points now (%d mover-hole boxes near the camera, %d candidates), %ld view setups merged them, %ld rebuilds; world draw hook %s (%ld calls)\n", g_sampling ? "on" : "off", g_samples.size(),
	               g_lastBoxes, g_lastCandidates, g_augmented, g_sampleBuilds, g_origWL ? "installed" : "NOT installed", g_wlCalls);
}

}  // namespace bridge
