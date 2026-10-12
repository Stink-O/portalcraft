// Portalcraft bridge (bridge #50): the walls of the holes mined in movers, drawn as Minecraft block faces. See moverwalls_geom.hpp for the geometry.
//
// The picture of a mover is punched where a cell was mined (digrender.cpp DigMoverMark: depth, so the engine's own draw of the mover is not made inside its
// holes). Here the hole's walls are drawn first, BEFORE that mark, in the main view: they test against the world drawn so far (the mover is not in it yet);
// then the mark's near faces (which are in front of the walls) take the depth over, so the engine's draw of the mover fails inside the hole and the walls stay,
// and an entity in front of the hole draws over them. The atlas rects of the material (side, top, bottom) come from the mod (kRenMoverDig style messages);
// the colour is Portal 2's light at the cut, like the items in worldents.cpp. pc_mover_walls 0 turns them off.
#include "bridge.hpp"
#include "coords.hpp"
#include "moverwalls.hpp"

#include "Modules/MaterialSystem.hpp"
#include "Modules/Tier1.hpp"
#include "Utils/SDK/MeshBuilder.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace bridge {

QAngle MainViewAngles();                                   // avatar.cpp
bool IsMainCameraOrigin(const Vector &origin);             // visclamp.cpp (bridge #51: a portal view of a facing pair has the player's angles too)
const char *TerrainAtlasTexName(int &gen, float &vScale);  // terrain.cpp
float RenderLightToScale(float linear);                    // render.cpp

namespace {
const int kKinds = 16;
float g_rects[kKinds][12];
bool g_have[kKinds];
bool g_on = true;
IMaterial *g_mat = nullptr;
int g_matGen = -1;
float g_vScale = 1.0f;
int g_lastQuads = 0, g_frames = 0, g_logged = 0, g_styles = 0;
double NowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

bool EnsureMaterial() {
	int gen = 0;
	float vs = 1.0f;
	const char *tex = TerrainAtlasTexName(gen, vs);
	if (!tex) return false;
	g_vScale = vs;
	if (gen == g_matGen && g_mat) return true;
	char name[64];
	auto kv = new KeyValues("UnlitGeneric");
	kv->SetString("$basetexture", tex);
	kv->SetInt("$vertexcolor", 1);
	kv->SetInt("$alphatest", 1);
	kv->SetInt("$nocull", 1);
	std::snprintf(name, sizeof name, "_pcb_mw_solid%d", gen);
	g_mat = materialSystem->CreateMaterial(name, kv);
	g_matGen = gen;
	return g_mat != nullptr;
}

struct LightKey {
	uint32_t id;
	unsigned group;
	bool operator==(const LightKey &o) const { return id == o.id && group == o.group; }
};
struct LightKeyHash {
	size_t operator()(const LightKey &k) const { return (size_t)k.id * 2654435761u ^ (size_t)k.group * 40503u; }
};
struct Lit {
	double at = -1e9;
	float r = 1, g = 1, b = 1;
};
}  // namespace

void MoverWallsSetStyle(int kind, const float rects[12]) {
	if (kind < 0 || kind >= kKinds) return;
	std::memcpy(g_rects[kind], rects, sizeof g_rects[kind]);
	g_have[kind] = true;
	++g_styles;
}

void MoverWallsDraw(const CViewSetup *view) {
	if (!g_on || !MoverWallsAny()) return;
	// the main view only (the mover punch is: a hole is not punched in a portal view, and walls without the hole would float in front of the mover)
	const QAngle va = MainViewAngles();
	if (std::fabs(va.x - view->angles.x) > 0.01f || std::fabs(va.y - view->angles.y) > 0.01f || !IsMainCameraOrigin(view->origin)) return;
	if (!EnsureMaterial()) return;
	static std::vector<MoverWalls> walls;
	MoverWallsSnapshot(walls);
	if (walls.empty()) return;
	static std::unordered_map<LightKey, Lit, LightKeyHash> lights;
	const double now = NowS();
	int fallback = -1;
	for (int k = 1; k < kKinds && fallback < 0; ++k)
		if (g_have[k]) fallback = k;
	if (fallback < 0) return;  // the mod has sent no styles yet
	int total = 0;
	for (const MoverWalls &w : walls)
		if (w.set && (w.origin - view->origin).Length() < 3000.0f) total += (int)w.set->quads.size();
	if (!total) return;
	total = std::min(total, 6000);
	// (the engine's dynamic mesh holds only so many vertices: at most 1000 quads per mesh, as terrain.cpp's lists)
	const int kChunk = 1000;
	std::unique_ptr<MeshBuilder> mbp;
	int drawn = 0;
	for (const MoverWalls &w : walls) {
		if (!w.set || (w.origin - view->origin).Length() > 3000.0f) continue;
		for (const MoverWallQuad &q : w.set->quads) {
			if (drawn >= total) break;
			if (drawn % kChunk == 0) {
				if (mbp) mbp->Draw();
				mbp.reset(new MeshBuilder(g_mat, PrimitiveType::QUADS, std::min(kChunk, total - drawn)));
			}
			MeshBuilder &mb = *mbp;
			const int kind = q.kind < kKinds && g_have[q.kind] ? q.kind : fallback;
			// the mod's order (cubeFaces): side 0..3, top 4..7, bottom 8..11; a floor of the hole shows a block's top, a ceiling its bottom
			const float *rr = g_rects[kind] + (q.face == 1 ? 4 : q.face == 2 ? 8 : 0);
			Lit &l = lights[{w.id, q.group}];
			if (now - l.at > 0.3 && q.group < w.set->centres.size()) {
				const auto &c = w.set->centres[q.group];
				const Vector local(c[0], c[1], c[2]);
				const Vector world = w.origin + Vector(w.rot[0][0] * local.x + w.rot[0][1] * local.y + w.rot[0][2] * local.z, w.rot[1][0] * local.x + w.rot[1][1] * local.y + w.rot[1][2] * local.z,
				                                       w.rot[2][0] * local.x + w.rot[2][1] * local.y + w.rot[2][2] * local.z);
				const Vector li = engine.LightAt(world, Vector(0, 0, 1));
				l.r = RenderLightToScale(li.x), l.g = RenderLightToScale(li.y), l.b = RenderLightToScale(li.z), l.at = now;
			}
			auto ch = [&](float v) { return (uint8_t)std::fmin(255.0f, std::fmax(0.0f, v * q.shade * 255.0f)); };
			const Color col(ch(l.r), ch(l.g), ch(l.b), 255);
			for (int k = 0; k < 4; ++k) {
				const Vector local(q.p[k][0], q.p[k][1], q.p[k][2]);
				const Vector world = w.origin + Vector(w.rot[0][0] * local.x + w.rot[0][1] * local.y + w.rot[0][2] * local.z, w.rot[1][0] * local.x + w.rot[1][1] * local.y + w.rot[1][2] * local.z,
				                                       w.rot[2][0] * local.x + w.rot[2][1] * local.y + w.rot[2][2] * local.z);
				mb.Position(world);
				mb.Color(col);
				mb.TexCoord(0, rr[0] + q.fu[k] * (rr[2] - rr[0]), (rr[1] + q.fv[k] * (rr[3] - rr[1])) * g_vScale);
				mb.AdvanceVertex();
			}
			++drawn;
		}
	}
	if (mbp) mbp->Draw();
	g_lastQuads = drawn;
	++g_frames;
	if (lights.size() > 4000) lights.clear();
	if (g_logged < 3) {
		++g_logged;
		console->Print("[bridge] mover walls: drew %d wall quads of mined movers (frame %d)\n", drawn, g_frames);
	}
}

CON_COMMAND(pc_mover_walls, "pc_mover_walls [0|1] - (bridge #50) draw Minecraft block faces as the walls of holes mined in movers (default 1; the main view only)\n") {
	if (args.ArgC() > 1) g_on = std::atoi(args[1]) != 0;
	int have = 0;
	for (int k = 0; k < kKinds; ++k) have += g_have[k];
	console->Print("[bridge] mover walls %s: %d quads in the last frame, %d frames, %d style messages (%d kinds), %s\n", g_on ? "on" : "off", g_lastQuads, g_frames, g_styles, have,
	               MoverWallsAny() ? "walls exist" : "no walls");
}

}  // namespace bridge
