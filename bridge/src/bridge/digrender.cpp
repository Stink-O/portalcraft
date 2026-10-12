// Portalcraft bridge: holes the Minecraft player mined in MOVERS (doors, panels, lifts), made in the picture (bridge #43).
//
// (bridge #75) The dug cells of the world are no longer this file's job: the stencil punch of the main view (#33), the peel that redrew the world
// behind a hole (#44), the union boxes (#41) and the nearest-cells cache (#46) are gone. Portal 2's own world shaders remove the dug cells
// per fragment, in every view (cutrender.cpp, DESIGN-dig-render.md), and cutvis.cpp gives every view and the server what is seen through
// them. What stays here is the mover punch below (a mover is an entity, drawn from its own model: the world's cut does not reach it) and
// the mover pieces' boxes that visclamp.cpp samples for the visibility behind a hole in a closed door.
//
// The stencil state is set through IMatRenderContext::SetStencilState (vtable slot 119 in Portal 2's Linux build; the argument is a pointer to
// a 32-byte {enable, failOp, zFailOp, passOp, compareFunc, ref, testMask, writeMask}; slot and layout read off the client's own call
// sites, with the same enum values as the SDK's StencilOperation_t / StencilComparisonFunction_t). Only the main view is punched.
#include "bridge.hpp"
#include "slots.hpp"
#include "prof.hpp"
#include "coords.hpp"
#include "dig.hpp"

#include "Modules/MaterialSystem.hpp"
#include "Modules/Tier1.hpp"
#include "Utils/SDK/MeshBuilder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace bridge {

QAngle MainViewAngles();  // avatar.cpp
bool IsMainCameraOrigin(const Vector &origin);  // visclamp.cpp (bridge #51)

// (bridge #51) The mover holes belong to the player's MAIN view only. Its angles are the player's, but a portal view of a pair that
// faces each other (opposite normals) has the same angles (the pair's transform is a pure translation), so the angles alone let those views through: the
// punch then ran inside Portal 2's stencil-confined portal view, marked and painted over main-view pixels with the wrong camera (a dark box on a floor or wall
// nowhere near the portal), replaced Portal 2's stencil state with "off" for the rest of that view, and replayed the world draw there. The camera's position
// settles it. `pc_dig_mover_punch 1 0` brings the old, angle-only test back (A/B).
static int g_mainOnly = 1;
static int g_portalViewsSkipped = 0, g_portalViewsLogged = 0;
static bool IsMainView(const CViewSetup *view) {
	const QAngle va = MainViewAngles();
	if (std::fabs(va.x - view->angles.x) > 0.01f || std::fabs(va.y - view->angles.y) > 0.01f) return false;
	if (g_mainOnly && !IsMainCameraOrigin(view->origin)) {
		++g_portalViewsSkipped;
		if (g_portalViewsLogged < 4) ++g_portalViewsLogged, console->Print("[bridge] dig: a view with the player's own angles but its camera elsewhere (origin %.0f %.0f %.0f: a portal view of a facing pair) is not punched\n", view->origin.x, view->origin.y, view->origin.z);
		return false;
	}
	return true;
}

namespace {

enum { kMatView = 0, kMatProjection = 1, kMatModel = 10 };
enum { kKeep = 1, kZero = 2, kReplace = 3, kInvert = 6, kEqual = 3, kAlways = 8 };

struct StencilState {  // ShaderStencilState_t
	int enable;        // (a bool in the low byte)
	int failOp, zFailOp, passOp, compare, ref;
	unsigned testMask, writeMask;
};
static_assert(sizeof(StencilState) == 32, "ShaderStencilState_t is 32 bytes");

// (slots.hpp) a slot held back by the Windows safe-load policy does nothing and returns a zero value
template <typename F, typename... A>
auto RcCall(IMatRenderContext *rc, slots::Id slot, A... a) {
	using R = decltype(F(nullptr)(rc, a...));
	F f = slots::Vtbl<F>(rc, slot);
	if (!f) return R();
	return f(rc, a...);
}
using _RcVoid = void(__rescall *)(IMatRenderContext *);
using _RcInt = void(__rescall *)(IMatRenderContext *, int);
using _RcStencil = void(__rescall *)(IMatRenderContext *, const StencilState *);
using _RcPlane = void(__rescall *)(IMatRenderContext *, const float *);

IMaterial *g_matMark = nullptr;
bool g_matFailed = false;
const unsigned kBit = 0x80;

bool EnsureMaterials() {
	if (g_matMark) return true;
	if (g_matFailed) return false;
	auto kv = new KeyValues("UnlitGeneric");
	kv->SetInt("$nocull", 1);
	kv->SetInt("$vertexcolor", 1);
	kv->SetInt("$vertexalpha", 1);
	kv->SetInt("$translucent", 1);
	g_matMark = materialSystem->CreateMaterial("_pcb_dig_mark", kv);
	if (!g_matMark) {
		g_matFailed = true;
		console->Warn("[bridge] dig: could not create the mover-punch material\n");
		return false;
	}
	return true;
}

}  // namespace

// World-space AABBs of the pieces mined out of movers (oriented boxes at the mover's drawn pose), the nearest first.
static void MoverPieceBoxes(const Vector &eye, std::vector<std::pair<Vector, Vector>> &out, uint32_t *key) {
	static thread_local std::vector<MoverHoles> holes;
	MoverHolesSnapshot(holes);
	uint32_t k = (uint32_t)holes.size() * 2654435761u;
	for (const MoverHoles &h : holes) {
		if ((h.origin - eye).Length() > 3000.0f) continue;
		for (int a = 0; a < 3; ++a) k = (k ^ (uint32_t)(int)std::floor((&h.origin.x)[a] / 12.0f)) * 16777619u;  // a mover that moved 12 u is a different set
		k = (k ^ (uint32_t)h.boxes.size()) * 16777619u;
		for (const auto &b : h.boxes) {
			Vector mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
			for (int c = 0; c < 8; ++c) {
				const Vector l((c & 1) ? b.second.x : b.first.x, (c & 2) ? b.second.y : b.first.y, (c & 4) ? b.second.z : b.first.z);
				for (int a = 0; a < 3; ++a) {
					const float v = (&h.origin.x)[a] + h.rot[a][0] * l.x + h.rot[a][1] * l.y + h.rot[a][2] * l.z;
					(&mn.x)[a] = std::fmin((&mn.x)[a], v), (&mx.x)[a] = std::fmax((&mx.x)[a], v);
				}
			}
			out.push_back({mn, mx});
		}
	}
	if (key) *key = k;
}

// (bridge #44, #46) The boxes whose far sides the world's visibility must reach: the pieces mined out of movers (a hole in a closed door opens onto the room
// behind it, which the engine's visibility, taken from the camera's own area, shuts out while the door's area portal is closed). visclamp.cpp samples the
// open air around them. (bridge #75: the world's dug cells are cutvis.cpp's.)
void DigPunchBoxes(const Vector &eye, std::vector<std::pair<Vector, Vector>> &out, uint32_t *moverKey) {
	out.clear();
	if (moverKey) *moverKey = 0;
	if (!DigEnabled()) return;
	if (MoverHolesAny()) MoverPieceBoxes(eye, out, moverKey);
}

// A key that changes when the mover pieces move or change (visclamp.cpp rebuilds its samples then).
uint32_t MoverPiecesKey(const Vector &eye) {
	if (!DigEnabled() || !MoverHolesAny()) return 0;
	uint32_t k = 0;
	std::vector<std::pair<Vector, Vector>> tmp;
	MoverPieceBoxes(eye, tmp, &k);
	return k | 1u;
}

namespace {
// ---- (bridge #43) holes in movers ------------------------------------------------------------------------------------------
// A door, a wall panel, a lift: the engine draws it as an entity AFTER the punch above, and from its own model, so no cell cut can take a piece out of it. The
// holes the Minecraft player mined in a mover (movers.cpp keeps them, in the mover's own frame) are made around the engine's draw of the entities instead, with
// the depth buffer, using the pieces of the mover that were mined out (oriented boxes at the mover's drawn pose):
//   A. before the engine draws its opaque entities (and after the world, the placed blocks and our entities): every piece's faces are drawn with depth test and
//      depth write on and the colour untouched (alpha 0). Where a piece is nearer than what is drawn so far, the depth buffer now holds the piece's near face.
//      The mover's own fragments there lie at or behind that face (the piece is grown 2 u, so the mover's surface is behind the face), so they fail the depth
//      test: the mover is not drawn inside its holes, and whatever the world drew behind it shows through. Entities in front of the face are not affected.
//   (bridge #43, 2nd) the same A / B again around the engine's TRANSLUCENT pass (render.cpp), so a door part the engine draws with alpha is made around the
//   hole as well (it was the "ghosted rectangles" over a mined door, by analysis); ours (the Minecraft body, blocks, particles of ours) draw between the two.
//   B. after the engine's opaque entities: the pieces are drawn again (depth test only, same depth function: the faces that are now in the buffer pass) and mark
//      the stencil where they do; a full-screen quad at the far depth then clears the depth where the mark is (and the mark), so what is drawn after
//      (translucent parts, the Minecraft player's body, particles, our later blocks) is not hidden behind the hole's near face.
// Gaps (README): inside the hole's silhouette things drawn in the opaque pass BEHIND the near face are hidden (another part of a hollow mover, an entity behind
// it); only the main view is punched; the hole has no walls (the picture behind it shows).
bool g_moverPunch = true, g_moverMarked = false;
const CViewSetup *g_moverView = nullptr;
int g_moverFrames = 0, g_moverLogged = 0, g_moverLastBoxes = 0;
using Corners = std::array<Vector, 8>;
std::vector<Corners> g_moverBoxes;

void OrientedBoxQuads(MeshBuilder &mb, const Corners &v) {
	const Color c(0, 0, 0, 0);
	static const int q[6][4] = {{0, 1, 2, 3}, {4, 7, 6, 5}, {0, 4, 5, 1}, {1, 5, 6, 2}, {2, 6, 7, 3}, {3, 7, 4, 0}};
	for (auto &f : q)
		for (int k = 0; k < 4; ++k) mb.Position(v[f[k]]), mb.Color(c), mb.TexCoord(0, 0, 0), mb.AdvanceVertex();
}
void DrawMoverBoxes(bool write) {
	for (size_t i = 0; i < g_moverBoxes.size(); i += 64) {
		const size_t n = std::min<size_t>(64, g_moverBoxes.size() - i);
		MeshBuilder mb(g_matMark, PrimitiveType::QUADS, (int)n * 6);
		mb.ForceDepth(write, true);
		for (size_t k = 0; k < n; ++k) OrientedBoxQuads(mb, g_moverBoxes[i + k]);
		mb.Draw();
	}
}
}  // namespace

// A: before the engine's opaque entities (render.cpp)
void DigMoverMark(const CViewSetup *view) {
	// (bridge #51) a nested view (a portal view is drawn INSIDE the main view's translucent pass) must not clear the main view's mark: its Restore would never run
	// and the hole pieces' depth would stay in the buffer for the rest of the frame
	if (!g_moverPunch || !MoverHolesAny()) return;
	if (!IsMainView(view)) return;
	// the stencil state and clip planes are Portal 2-only slots: held back on the first Windows run
	if (!slots::Use(slots::kRcSetStencilState)) return;
	g_moverMarked = false;
	g_moverView = nullptr;
	if (!EnsureMaterials()) return;
	static std::vector<MoverHoles> holes;
	MoverHolesSnapshot(holes);
	g_moverBoxes.clear();
	const float g = 3.0f;  // grown: the mover's surface is behind the face, and a piece of collision a little smaller than the drawn model still covers it
	for (const MoverHoles &h : holes) {
		if ((h.origin - view->origin).Length() > 3000.0f) continue;
		for (const auto &b : h.boxes) {
			if (g_moverBoxes.size() >= 1024) break;
			const Vector mn = b.first - Vector(g, g, g), mx = b.second + Vector(g, g, g);
			Corners c;
			for (int k = 0; k < 8; ++k) {
				const Vector l((k & 1) ? mx.x : mn.x, (k & 2) ? mx.y : mn.y, (k & 4) ? mx.z : mn.z);
				c[k] = h.origin + Vector(h.rot[0][0] * l.x + h.rot[0][1] * l.y + h.rot[0][2] * l.z, h.rot[1][0] * l.x + h.rot[1][1] * l.y + h.rot[1][2] * l.z, h.rot[2][0] * l.x + h.rot[2][1] * l.y + h.rot[2][2] * l.z);
			}
			// BoxQuads' corner order: (mn,mn,mn) (mx,mn,mn) (mx,mx,mn) (mn,mx,mn), then the same at mx.z
			g_moverBoxes.push_back({c[0], c[1], c[3], c[2], c[4], c[5], c[7], c[6]});
		}
	}
	if (g_moverBoxes.empty()) return;
	DrawMoverBoxes(true);
	g_moverMarked = true;
	g_moverView = view;
	g_moverLastBoxes = (int)g_moverBoxes.size();
	++g_moverFrames;
	if (g_moverLogged < 3) {
		++g_moverLogged;
		console->Print("[bridge] dig: punched %zu mover-hole pieces out of the main view (frame %d)\n", g_moverBoxes.size(), g_moverFrames);
	}
}

// B: after them
void DigMoverRestore(const CViewSetup *view) {
	if (!g_moverMarked || view != g_moverView) return;
	g_moverMarked = false;
	g_moverView = nullptr;
	IMatRenderContext *rc = materialSystem->GetRenderContext();
	if (!rc) return;
	StencilState mark{1, kKeep, kKeep, kReplace, kAlways, (int)kBit, kBit, kBit};
	RcCall<_RcStencil>(rc, slots::kRcSetStencilState, &mark);
	DrawMoverBoxes(false);
	StencilState paint{1, kKeep, kKeep, kZero, kEqual, (int)kBit, kBit, kBit};
	RcCall<_RcStencil>(rc, slots::kRcSetStencilState, &paint);
	const int modes[3] = {kMatView, kMatProjection, kMatModel};
	for (int m : modes) RcCall<_RcInt>(rc, slots::kRcMatrixMode, m), RcCall<_RcVoid>(rc, slots::kRcPushMatrix), RcCall<_RcVoid>(rc, slots::kRcLoadIdentity);
	{
		MeshBuilder mb(g_matMark, PrimitiveType::QUADS, 1);
		mb.ForceDepth(true, false);
		const Color none(0, 0, 0, 0);  // the colour is kept: only the depth goes back
		const float q[4][2] = {{-1.01f, 1.01f}, {1.01f, 1.01f}, {1.01f, -1.01f}, {-1.01f, -1.01f}};
		for (auto &c : q) mb.Position(Vector(c[0], c[1], 0.9999f)), mb.Color(none), mb.TexCoord(0, 0, 0), mb.AdvanceVertex();
		mb.Draw();
	}
	for (int m : modes) RcCall<_RcInt>(rc, slots::kRcMatrixMode, m), RcCall<_RcVoid>(rc, slots::kRcPopMatrix);
	StencilState off{0, kKeep, kKeep, kKeep, kAlways, 0, 0xFFFFFFFFu, 0xFFFFFFFFu};
	RcCall<_RcStencil>(rc, slots::kRcSetStencilState, &off);
	RcCall<_RcVoid>(rc, slots::kRcRelease);
}

CON_COMMAND(pc_dig_mover_punch, "pc_dig_mover_punch [0|1] [mainonly 0|1] - (bridge #43) punch the holes mined in movers (doors, panels, lifts...) out of the picture (default 1; the main view only: mainonly 0 = judge the main view by its angles alone, the old test)\n") {
	if (args.ArgC() > 1) g_moverPunch = std::atoi(args[1]) != 0;
	if (args.ArgC() > 2) g_mainOnly = std::atoi(args[2]) != 0;
	console->Print("[bridge] mover-hole punch %s: %d pieces in the last frame, %d frames, %s; %d portal views skipped\n", g_moverPunch ? "on" : "off", g_moverLastBoxes, g_moverFrames, MoverHolesAny() ? "holes exist" : "no holes", g_portalViewsSkipped);
}


void DigRenderShutdown() {
	if (g_matMark) materialSystem->DestroyMaterial(g_matMark), g_matMark = nullptr;
}

}  // namespace bridge
