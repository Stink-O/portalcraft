// Portalcraft bridge (bridge #16): F5 third person and the Minecraft player's body.
//
// Camera: Minecraft's F5 cycles its camera (first person -> third person behind -> third person in front). Portal 2's
// view follows it: the eye (mc.cpp's CameraDriver) moved back along the camera's look by the distance Minecraft's own
// camera chose this frame (McState::cameraDistance: its zoom collision against blocks and the exported Portal 2
// collision, ThirdPersonCameraMixin), capped by a short hull trace through Portal 2's world (portal holes are left out of
// the export, and the eye may sit a voxel off a wall). In front of the player the look is mirrored (yaw + 180, pitch
// negated), as Minecraft does. The mouse keeps turning the player, so the camera orbits it.
//
// Body: Minecraft's own player model (AvatarExporter: skin, armour, held items, cape; posed and animated by Minecraft's
// renderer: walk, sneak, swing) arrives every frame as kRenAvatar, positions relative to the feet; its textures as
// kRenTexture, once each. Portal 2 draws it (DESIGN-mcplayer 2 (a)/(c)):
//   * only while the body is outside the region box (in the region Minecraft draws its own player in its host-view
//     passes: kHpAvatarInMc, LocalPlayerHostViewMixin);
//   * in the main view only in third person; in every other view (portal views, any recursion depth) always, so in first
//     person the player sees itself only through portals (Portal 2's rule for Chell);
//   * lit by Portal 2: the engine's light at the body (ComputeLighting for the six axis normals = an ambient cube with the
//     local lights), weighted by each triangle's normal; times Minecraft's vertex colour (tint, hurt flash).
// Chell's model stays hidden in Minecraft mode (mc.cpp HideChell).
#include "bridge.hpp"
#include "growth.hpp"
#include "coords.hpp"
#include "link.hpp"

#include "Modules/MaterialSystem.hpp"
#include "Modules/Tier1.hpp"
#include "Utils/SDK/MeshBuilder.hpp"

#include <chrono>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <map>
#include <vector>

namespace bridge {

using namespace skycraft::proto;

bool McModeActive();                // mc.cpp
bool McLeads();                     // mc.cpp
Vector McFeetSrc();                 // mc.cpp: Minecraft's interpolated feet (Source)
const McState &McStateNow();        // mc.cpp
bool ClipRegionContains(const Vector &p);  // clip.cpp
void TestCheck(bool ok, const char *what, const char *detail);  // testassert.cpp

static double NowS() {
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ---- textures ---------------------------------------------------------------------------------
struct AvTex {
	IMaterial *mat = nullptr, *matT = nullptr;  // cutout, translucent
	float uScale = 1, vScale = 1;               // padding to a power of two
	int w = 0, h = 0;
	bool binaryAlpha = true;  // (bridge #54) no texel is partly transparent: cutout looks as blending would
};
static std::map<uint32_t, AvTex> g_tex;  // 0 = the block/item atlas (terrain.cpp's texture)
PCB_GAUGE("avatar.tex", g_tex.size());
static int g_texGen = 0;

static IMaterial *MakeMat(const char *tex, bool translucent) {
	char name[64];
	std::snprintf(name, sizeof name, "_pcb_av_mat%d%s", ++g_texGen, translucent ? "t" : "");
	auto kv = new KeyValues("UnlitGeneric");
	kv->SetString("$basetexture", tex);
	kv->SetInt("$vertexcolor", 1);
	kv->SetInt("$nocull", 1);
	if (translucent) {
		kv->SetInt("$translucent", 1);
		kv->SetInt("$vertexalpha", 1);
	} else {
		kv->SetInt("$alphatest", 1);
	}
	return materialSystem->CreateMaterial(name, kv);
}

// terrain.cpp: the combined block/item atlas arrived (texture 0 of the avatar's batches)
void AvatarOnAtlas(const char *texName, int w, int h, int texH) {
	AvTex t;
	t.binaryAlpha = false;  // the atlas has translucent texels (glass, ice): its blended batches stay blended
	t.mat = MakeMat(texName, false), t.matT = MakeMat(texName, true);
	t.vScale = texH ? (float)h / texH : 1.0f;
	t.w = w, t.h = h;
	g_tex[0] = t;
}

// scene.cpp (bridge #19): the same textures serve the scene mesh (entities, block entities, particles)
bool AvatarTexture(uint32_t id, IMaterial *&mat, IMaterial *&matT, float &uScale, float &vScale) {
	auto it = g_tex.find(id);
	if (it == g_tex.end() || !it->second.mat) return false;
	mat = it->second.mat, matT = it->second.matT ? it->second.matT : it->second.mat;
	uScale = it->second.uScale, vScale = it->second.vScale;
	return true;
}

static int Pow2(int v) {
	int p = 1;
	while (p < v) p <<= 1;
	return p;
}
// terrain.cpp: kRenTexture (RenTexture + RGBA8, top row first). The same id again (a new link) replaces it; the old
// material may still be referenced by queued draws, so it is kept (as terrain.cpp does with atlases).
void AvatarOnTexture(const uint8_t *p, uint32_t bytes) {
	if (bytes < sizeof(RenTexture)) return;
	const auto *h = (const RenTexture *)p;
	if (!h->id || h->width == 0 || h->height == 0 || h->width > 4096 || h->height > 4096) return;
	if (bytes < sizeof(RenTexture) + (uint64_t)h->width * h->height * 4) return;
	const int w = Pow2((int)h->width), th = Pow2((int)h->height);
	std::vector<uint8_t> bgra((size_t)w * th * 4, 0);
	const uint8_t *src = p + sizeof(RenTexture);
	for (uint32_t y = 0; y < h->height; ++y)
		for (uint32_t x = 0; x < h->width; ++x) {
			const uint8_t *s = src + ((size_t)y * h->width + x) * 4;
			uint8_t *d = &bgra[((size_t)y * w + x) * 4];
			d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = s[3];
		}
	char texName[64];
	std::snprintf(texName, sizeof texName, "_pcb_av_tex%u_%d", h->id, ++g_texGen);
	if (!materialSystem->CreateTexture(texName, w, th, bgra.data(), TEXTUREFLAGS_NOMIP | TEXTUREFLAGS_NOLOD | TEXTUREFLAGS_SINGLECOPY | TEXTUREFLAGS_POINTSAMPLE | TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT)) {
		console->Warn("[bridge] avatar: texture %u (%ux%u) could not be created\n", h->id, h->width, h->height);
		return;
	}
	AvTex t;
	for (size_t i = 3; i < bgra.size(); i += 4)
		if (bgra[i] > 24 && bgra[i] < 232) { t.binaryAlpha = false; break; }
	t.mat = MakeMat(texName, false), t.matT = MakeMat(texName, true);
	t.uScale = (float)h->width / w, t.vScale = (float)h->height / th;
	t.w = (int)h->width, t.h = (int)h->height;
	g_tex[h->id] = t;
	console->Print("[bridge] avatar: texture %u %ux%u uploaded\n", h->id, h->width, h->height);
}

// ---- the body's mesh -------------------------------------------------------------------------------
static std::vector<RenBatch> g_batches;
static std::vector<RenVertex> g_verts;
static double g_meshAt = 0;
static uint64_t g_meshes = 0;

// terrain.cpp: kRenAvatar (RenAvatar + RenBatch[] + RenVertex[]); 0 batches = not shown
void AvatarOnMesh(const uint8_t *p, uint32_t bytes) {
	if (bytes < sizeof(RenAvatar)) return;
	const auto *h = (const RenAvatar *)p;
	const uint64_t need = sizeof(RenAvatar) + (uint64_t)h->batchCount * sizeof(RenBatch) + (uint64_t)h->vertexCount * sizeof(RenVertex);
	if (bytes < need || h->batchCount > 256 || h->vertexCount > 200000) return;
	const auto *b = (const RenBatch *)(p + sizeof(RenAvatar));
	g_batches.assign(b, b + h->batchCount);
	const auto *v = (const RenVertex *)(b + h->batchCount);
	g_verts.assign(v, v + h->vertexCount);
	g_meshAt = NowS();
	++g_meshes;
}

// ---- the camera ------------------------------------------------------------------------------------
static bool g_tpOn = true;       // pc_mc_thirdperson
static bool g_tpTrace = true;    // pc_mc_thirdperson_trace: Portal 2's own hull trace on top of Minecraft's zoom
static int g_tpMode = 0;         // this frame: 0 first person, 1 behind, 2 in front
static float g_tpMcDist = 0, g_tpDist = 0, g_tpTraceDist = 0;
static QAngle g_mainAng;
static Vector g_mainOrg;
static bool g_mainValid = false;
static uint64_t g_frameNo = 0;
static float zoom = 0;  // the eased camera distance (u), its mode, the last frame's time
static int zoomMode = 0;
static double lastT = 0;

static Vector Forward(const QAngle &a) {
	const float p = a.x * (float)M_PI / 180.0f, y = a.y * (float)M_PI / 180.0f;
	return Vector(std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), -std::sin(p));
}

// mc.cpp, OverrideView: after the CameraDriver put the eye in, before the composite predicts its cameras.
void AvatarCameraFrame(CViewSetup *view) {
	++g_frameNo;
	g_tpMode = 0;
	if (!McModeActive() || !g_tpOn || (McStateNow().cameraMode != 1 && McStateNow().cameraMode != 2)) zoomMode = 0, lastT = 0;
	if (McModeActive() && g_tpOn) {
		const McState &m = McStateNow();
		const int mode = m.cameraMode == 1 || m.cameraMode == 2 ? (int)m.cameraMode : 0;
		if (mode) {
			QAngle a = view->angles;
			if (mode == 2) a.y = std::remainder(a.y + 180.0f, 360.0f), a.x = -a.x;
			const Vector eye = view->origin, back = Forward(a) * -1.0f;
			g_tpMcDist = std::fmax(0.0f, std::fmin(m.cameraDistance, 16.0f)) * kUnitsPerBlock;
			// SkyCraft's zoom (skse Game.cpp, MIT): Minecraft's distance pulls in at once (never through a wall) and
			// eases back out over ~0.2 s, so a ray grazing the ground can't shake the camera.
			const double now = NowS(), dt = lastT > 0 ? now - lastT : 0;
			lastT = now;
			if (zoomMode != mode || g_tpMcDist < zoom) zoom = g_tpMcDist;
			else zoom += (g_tpMcDist - zoom) * (1.0f - (float)std::exp(-std::fmax(dt, 0.0) / 0.2));
			zoomMode = mode;
			float d = zoom;
			g_tpTraceDist = d;
			if (g_tpTrace && d > 0.5f) {
				CGameTrace tr;
				engine.Trace(eye, eye + back * d, Vector(4, 4, 4), MASK_SOLID_BRUSHONLY, tr, true);
				if (!tr.startsolid && tr.fraction < 1.0f) g_tpTraceDist = d * tr.fraction;
				d = std::fmin(d, g_tpTraceDist);
			}
			g_tpDist = d;
			g_tpMode = mode;
			view->origin = eye + back * d;
			view->angles = a;
		}
	}
	g_mainAng = view->angles, g_mainOrg = view->origin, g_mainValid = true;
}

// render.cpp: the main view's angles (in front of the player, Portal 2's own view angles are the player's, not the view's)
QAngle MainViewAngles() { return g_tpMode == 2 && g_mainValid ? g_mainAng : engine.ViewAngles(); }

// mc.cpp: HostPlayer flags. The body's mesh every frame in Minecraft mode; Minecraft draws its own player in host-view
// passes while the body is in the region.
static bool g_bodyOn = true;  // pc_mc_body
Vector McFeetRawSrc();  // mc.cpp
// (bridge #28) Minecraft's player where it is: a body half through a portal is drawn where it is (not jumped to the other side early)
static Vector BodyFeet() { return McLeads() ? McFeetRawSrc() : PlayerOrigin(); }
static bool BodyInRegion() { return ClipRegionContains(BodyFeet() + Vector(0, 0, 36)); }
uint32_t AvatarHostFlags() {
	if (!McModeActive() || !g_bodyOn) return 0;
	return kHpWantAvatar | (BodyInRegion() ? kHpAvatarInMc : 0u);
}

// ---- drawing ----------------------------------------------------------------------------------------
static float g_lightScale = 5.0f;  // pc_mc_body_light: as the blocks (render.cpp), linear light x 5
static Vector g_cube[6];           // +X -X +Y -Y +Z -Z (Source)
static uint64_t g_cubeFrame = ~0ull;
static int g_drawsMain = 0, g_drawsPortal = 0, g_skipRegion = 0, g_skipFirst = 0;
static int g_outward = 0, g_inward = 0;  // triangle normals vs the body's axis (a sanity check of the winding)
static bool g_debugLog = false;
static float g_uvInset = 0.03f;  // pc_mc_body_uvinset: texels

// bridge #19: #18's Portal 2 model lighting (bandlight.cpp), as Chell or a cube standing there is lit (same declarations as
// terrain.cpp / scene.cpp)
struct P2Light {
	int type;
	Vector col, pos, dir;
	float range, falloff, a0, a1, a2, cosTheta, cosPhi, oneOver;
};
struct P2State {
	float cube[6][3];
	int n;
	P2Light l[4];
};
bool BandLightQuery(const std::vector<Vector> &pts, std::vector<P2State> &out);
Vector BandLightEval(const P2State &s, const Vector &p, const Vector &n, bool hasNormal);
static int g_bodyLightMode = 2;  // pc_mc_body_light_mode: 2 the model lighting state (default), 1 ComputeLighting x scale
static P2State g_bodyState;
static bool g_bodyStateOk = false;
float BandLinearToVertex(float l);  // bandlight.cpp (#18)
void BlockLightsSuspend();          // blocklights.cpp (#18): our own dlights out of the query
void BlockLightsResume();
static Vector Encode(const Vector &l) { return Vector(BandLinearToVertex(l.x), BandLinearToVertex(l.y), BandLinearToVertex(l.z)); }

// (bridge #62) Minecraft's block lights (torches...) at the body: the ComputeLightingState hook (mclight.cpp) never sees this
// draw (BandLightQuery calls the engine's original), so they are added here with the same rule, BlockLightsAt (level less the
// Manhattan distance, the fade), as constant-strength point lights: colour x N.L. They are queried while NOT suspended;
// the dlights stay out of the engine's query (Suspend), so nothing counts twice.
int BlockLightsAt(const Vector &p, Vector *pos, Vector *col, int max);  // blocklights.cpp
static Vector g_mcLpos[8], g_mcLcol[8];
static int g_mcLn = 0;
static bool g_mcBlockLight = true;  // pc_mc_body_blocklight

// (bridge #70) while the view is redirected (a point_viewproxy: the vault) what is drawn at the Minecraft player's place goes where the camera is: p' = R p + t
// (mc.cpp McViewRedirect). `g_xfOn` is set per draw; Xf is the identity otherwise.
bool McViewRedirect(Vector &t, float r[3][3]);  // mc.cpp
static bool g_xfOn = false;
static Vector g_xfT(0, 0, 0);
static float g_xfR[3][3];
static Vector Xf(const Vector &p) {
	if (!g_xfOn) return p;
	return Vector(g_xfR[0][0] * p.x + g_xfR[0][1] * p.y + g_xfR[0][2] * p.z + g_xfT.x, g_xfR[1][0] * p.x + g_xfR[1][1] * p.y + g_xfR[1][2] * p.z + g_xfT.y,
	              g_xfR[2][0] * p.x + g_xfR[2][1] * p.y + g_xfR[2][2] * p.z + g_xfT.z);
}
static Vector XfInv(const Vector &p) {  // transpose(R) (p - t)
	if (!g_xfOn) return p;
	const Vector q = p - g_xfT;
	return Vector(g_xfR[0][0] * q.x + g_xfR[1][0] * q.y + g_xfR[2][0] * q.z, g_xfR[0][1] * q.x + g_xfR[1][1] * q.y + g_xfR[2][1] * q.z,
	              g_xfR[0][2] * q.x + g_xfR[1][2] * q.y + g_xfR[2][2] * q.z);
}

static void LightCube(const Vector &centre) {
	// centre: where the body is in Minecraft's frame (its torches); the lighting of Portal 2's world is read where the body is DRAWN
	const Vector drawn = Xf(centre);
	g_mcLn = g_mcBlockLight ? BlockLightsAt(centre, g_mcLpos, g_mcLcol, 8) : 0;
	for (int j = 0; j < g_mcLn; ++j) g_mcLpos[j] = Xf(g_mcLpos[j]);
	if (g_bodyLightMode == 2) {
		std::vector<P2State> st;
		BlockLightsSuspend();
		g_bodyStateOk = BandLightQuery({drawn}, st) && !st.empty();
		BlockLightsResume();
		if (g_bodyStateOk) g_bodyState = st[0];
	}
	static const Vector n[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
	for (int i = 0; i < 6; ++i) {
		Vector l = engine.LightAt(drawn + n[i] * 20.0f, n[i]);
		g_cube[i] = Vector(std::fmin(1.0f, l.x * g_lightScale), std::fmin(1.0f, l.y * g_lightScale), std::fmin(1.0f, l.z * g_lightScale));
	}
}

bool AvatarReady() { return McModeActive() && g_bodyOn && !g_batches.empty() && !g_verts.empty() && NowS() - g_meshAt < 0.5; }

// render.cpp, translucent pass of every 3D view (not the skybox or shadow depth)
void AvatarDraw(const CViewSetup *view) {
	if (!AvatarReady()) return;
	g_xfOn = McViewRedirect(g_xfT, g_xfR);
	const bool isMain = g_mainValid && (view->origin - g_mainOrg).Length() < 1.0f && std::fabs(std::remainder(view->angles.y - g_mainAng.y, 360.0f)) < 0.01f &&
	                    std::fabs(view->angles.x - g_mainAng.x) < 0.01f;
	if (isMain && (g_tpMode == 0 || g_tpDist < 24.0f)) return void(++g_skipFirst);  // (or the F5 camera pulled into the head)
	const Vector feet = BodyFeet();
	if (ClipRegionContains(feet + Vector(0, 0, 36))) return void(++g_skipRegion);
	// (bridge #28) never around a camera inside the body: a portal view's camera is the eye carried through the pair, inside the
	// body on the far side while the player goes through (t28_transit10: two frames of the inside of the own head)
	const Vector camInBody = XfInv(view->origin);  // the camera in the body's own frame (the redirect undone)
	if (std::fabs(camInBody.x - feet.x) < 24.0f && std::fabs(camInBody.y - feet.y) < 24.0f && camInBody.z > feet.z - 8.0f && camInBody.z < feet.z + 84.0f)
		return void(++g_skipFirst);
	if (g_cubeFrame != g_frameNo) g_cubeFrame = g_frameNo, LightCube(feet + Vector(0, 0, 36));
	const Vector axisLo = feet, axisHi = feet + Vector(0, 0, 72);
	for (int pass = 0; pass < 2; ++pass)
	for (const RenBatch &b : g_batches) {
		auto it = g_tex.find(b.texture);
		if (it == g_tex.end() || !it->second.mat) continue;
		// (bridge #54) a "blended" batch (Minecraft's entity_translucent: the player's skin) whose texture has only fully opaque or fully
		// clear texels and whose vertices are opaque is a cutout model: alpha-tested WITH depth writes, so lasers, glass and the
		// background drawn later are depth-tested against the body. Only really translucent ones stay blended.
		bool blended = (b.flags & 1) != 0, cutoutOfBlended = false;
		if (blended && it->second.binaryAlpha && b.first + (uint64_t)b.count <= g_verts.size()) {
			bool opaque = true;
			for (uint32_t i = 0; i < b.count && opaque; ++i) opaque = (g_verts[b.first + i].color >> 24) == 255;
			if (opaque) blended = false, cutoutOfBlended = true;
		}
		if ((int)blended != pass) continue;  // solid, then blended (SkyCraft's two passes)
		const AvTex &t = it->second;
		const uint32_t n = b.count - b.count % 3;
		if (n == 0 || b.first + (uint64_t)n > g_verts.size()) continue;
		IMaterial *mat = blended && t.matT ? t.matT : t.mat;
		// (bridge #54) The player model is Minecraft's entity_translucent: a blended batch, drawn without depth writes. Both faces of every
		// cube were then painted in quad order (the far inner faces over the near outer ones: the body looked inside out). So a blended
		// batch is drawn as a translucent surface should be: back faces out (the winding is outward, tools/avatar_winding_check.py),
		// nearest triangle last. The solid batches keep depth writes and need neither.
		std::vector<uint32_t> order;  // first vertex of each triangle, in drawing order
		order.reserve(n / 3);
		for (uint32_t i = 0; i < n; i += 3) order.push_back(b.first + i);
		if (pass == 1 || cutoutOfBlended) {
			std::vector<std::pair<float, uint32_t>> keyed;
			keyed.reserve(order.size());
			for (uint32_t tv : order) {
				Vector p[3];
				for (int k = 0; k < 3; ++k) {
					const RenVertex &rv = g_verts[tv + k];
					p[k] = Xf(feet + Vector(rv.x * kUnitsPerBlock, -rv.z * kUnitsPerBlock, rv.y * kUnitsPerBlock));
				}
				const Vector c = (p[0] + p[1] + p[2]) * (1.0f / 3.0f);
				const Vector nrm = (p[1] - p[0]).Cross(p[2] - p[0]);
				if (nrm.Dot(view->origin - c) <= 0.0f) continue;  // facing away from this view
				const Vector d = view->origin - c;
				keyed.push_back({d.x * d.x + d.y * d.y + d.z * d.z, tv});
			}
			if (pass == 1) std::stable_sort(keyed.begin(), keyed.end(), [](const auto &a, const auto &b2) { return a.first > b2.first; });
			order.clear();
			for (auto &k : keyed) order.push_back(k.second);
		}
		const uint32_t total = (uint32_t)order.size() * 3;
		for (uint32_t done = 0; done < total;) {
			const uint32_t cnt = std::min<uint32_t>(total - done, 9000);
			MeshBuilder mb(mat, PrimitiveType::TRIANGLES, (int)(cnt / 3));
			for (uint32_t i = 0; i < cnt; i += 3) {
				const uint32_t base = order[(done + i) / 3];
				Vector p[3];
				for (int k = 0; k < 3; ++k) {
					const RenVertex &rv = g_verts[base + k];
					p[k] = Xf(feet + Vector(rv.x * kUnitsPerBlock, -rv.z * kUnitsPerBlock, rv.y * kUnitsPerBlock));
				}
				Vector nrm = (p[1] - p[0]).Cross(p[2] - p[0]);
				const float len = nrm.Length();
				nrm = len > 1e-6f ? nrm * (1.0f / len) : Vector(0, 0, 1);
				if (g_debugLog && !g_xfOn) {
					const Vector c = (p[0] + p[1] + p[2]) * (1.0f / 3.0f);
					const float z = std::fmax(axisLo.z, std::fmin(axisHi.z, c.z));
					const Vector r = c - Vector(feet.x, feet.y, z);
					(r.x * nrm.x + r.y * nrm.y + r.z * nrm.z > 0 ? g_outward : g_inward)++;
				}
				// the ambient cube, Source's way: squared normal components pick each axis' side
				const Vector &cx = g_cube[nrm.x >= 0 ? 0 : 1], &cy = g_cube[nrm.y >= 0 ? 2 : 3], &cz = g_cube[nrm.z >= 0 ? 4 : 5];
				const float wx = nrm.x * nrm.x, wy = nrm.y * nrm.y, wz = nrm.z * nrm.z;
				Vector light = cx * wx + cy * wy + cz * wz;
				const Vector tc = (p[0] + p[1] + p[2]) * (1.0f / 3.0f);
				if (g_bodyLightMode == 2 && g_bodyStateOk) light = BandLightEval(g_bodyState, tc + nrm, nrm, true);
				for (int j = 0; j < g_mcLn; ++j) {  // (bridge #62) Minecraft's torches, as mclight.cpp adds them to a model
					Vector L = g_mcLpos[j] - tc;
					const float ll = L.Length();
					L = ll > 1e-3f ? L * (1.0f / ll) : nrm;
					const float w = std::fmax(0.0f, (nrm.Dot(L) + 0.25f) / 1.25f);
					light += g_mcLcol[j] * w;
				}
				if (g_bodyLightMode == 2 && g_bodyStateOk) light = Encode(light);
				else if (g_mcLn) light = Vector(std::fmin(1.0f, light.x), std::fmin(1.0f, light.y), std::fmin(1.0f, light.z));
				// (bridge #62) point sampling lands exactly on a texel border at a face's UV edge and can pick the neighbouring island
				// (transparent: the alpha test then leaves a seam): sample a hair (1/32 texel) inside the triangle's UV extent
				float cu = 0, cv = 0;
				for (int k = 0; k < 3; ++k) cu += g_verts[base + k].u, cv += g_verts[base + k].v;
				cu /= 3, cv /= 3;
				const float insU = g_uvInset / (float)std::max(1, t.w), insV = g_uvInset / (float)std::max(1, t.h);
				for (int k = 0; k < 3; ++k) {
					const RenVertex &rv = g_verts[base + k];
					const uint32_t c = rv.color;
					auto ch = [](uint32_t v, float l) { return (uint8_t)std::fmin(255.0f, v * l); };
					mb.Position(p[k]);
					mb.Color(Color(ch(c & 0xFF, light.x), ch((c >> 8) & 0xFF, light.y), ch((c >> 16) & 0xFF, light.z), (uint8_t)(c >> 24)));
					const float du = cu - rv.u, dv = cv - rv.v;
					const float u = rv.u + (std::fabs(du) > insU ? (du > 0 ? insU : -insU) : du);
					const float v = rv.v + (std::fabs(dv) > insV ? (dv > 0 ? insV : -insV) : dv);
					mb.TexCoord(0, u * t.uScale, v * t.vScale);
					mb.AdvanceVertex();
				}
			}
			mb.Draw();
			done += cnt;
		}
	}
	(isMain ? g_drawsMain : g_drawsPortal)++;
}


// ---- commands -------------------------------------------------------------------------------------------
CON_COMMAND(pc_mc_thirdperson, "pc_mc_thirdperson 0|1 - Portal 2's view follows Minecraft's F5 camera (default 1)\n") {
	if (args.ArgC() > 1) g_tpOn = std::atoi(args[1]) != 0;
	console->Print("[bridge] third person (F5) %s\n", g_tpOn ? "on" : "off");
}
CON_COMMAND(pc_mc_thirdperson_trace, "pc_mc_thirdperson_trace 0|1 - also trace Portal 2's world from the eye to the F5 camera (default 1)\n") {
	if (args.ArgC() > 1) g_tpTrace = std::atoi(args[1]) != 0;
	console->Print("[bridge] third-person camera trace %s\n", g_tpTrace ? "on" : "off");
}
CON_COMMAND(pc_mc_body, "pc_mc_body 0|1 - draw the Minecraft player's body (default 1)\n") {
	if (args.ArgC() > 1) g_bodyOn = std::atoi(args[1]) != 0;
	console->Print("[bridge] Minecraft player's body %s\n", g_bodyOn ? "on" : "off");
}
CON_COMMAND(pc_mc_body_uvinset, "pc_mc_body_uvinset <texels> - (bridge #62) how far inside its UV extent each body triangle samples (default 0.03; 0 = off)\n") {
	if (args.ArgC() > 1) g_uvInset = std::fmax(0.0f, (float)std::atof(args[1]));
	console->Print("[bridge] body uv inset %.3f texel\n", g_uvInset);
}
CON_COMMAND(pc_mc_body_blocklight, "pc_mc_body_blocklight 0|1 - (bridge #62) Minecraft's torches light the body (default 1)\n") {
	if (args.ArgC() > 1) g_mcBlockLight = std::atoi(args[1]) != 0;
	console->Print("[bridge] body block light %d (%d lights now)\n", g_mcBlockLight, g_mcLn);
}
CON_COMMAND(pc_mc_body_light_mode, "pc_mc_body_light_mode 2|1 - the Minecraft body's light: 2 Portal 2's model lighting state (default), 1 ComputeLighting x pc_mc_body_light\n") {
	if (args.ArgC() > 1) g_bodyLightMode = std::atoi(args[1]) == 1 ? 1 : 2;
	console->Print("[bridge] body light mode %d\n", g_bodyLightMode);
}
CON_COMMAND(pc_mc_body_light, "pc_mc_body_light <scale> - Portal 2's light on the Minecraft player's body (default 5, as the blocks)\n") {
	if (args.ArgC() > 1) g_lightScale = (float)std::atof(args[1]);
	console->Print("[bridge] body light scale %.2f\n", g_lightScale);
}
CON_COMMAND(pcb_avatar_status, "pcb_avatar_status [debug 0|1] - the body and the F5 camera\n") {
	if (args.ArgC() > 1) g_debugLog = std::atoi(args[1]) != 0;
	const McState &m = McStateNow();
	size_t tris = 0;
	for (auto &b : g_batches) tris += b.count / 3;
	const Vector f = BodyFeet();
	console->Print("[bridge] avatar: mode %s, Minecraft camera %u (distance %.2f blocks), view %d at %.1f u (Minecraft %.1f, trace %.1f), mesh %zu batches %zu triangles %.2f s old (%llu meshes), "
	               "%zu textures, feet (%.1f %.1f %.1f) in region %d, draws main %d portal %d, skipped first-person main %d region %d, normals out %d in %d\n",
	               McModeActive() ? "minecraft" : "chell", m.cameraMode, m.cameraDistance, g_tpMode, g_tpDist, g_tpMcDist, g_tpTraceDist, g_batches.size(), tris, NowS() - g_meshAt,
	               (unsigned long long)g_meshes, g_tex.size(), f.x, f.y, f.z, BodyInRegion() ? 1 : 0, g_drawsMain, g_drawsPortal, g_skipFirst, g_skipRegion, g_outward, g_inward);
	for (auto &b : g_batches) console->Print("[bridge] avatar batch: texture %u first %u count %u flags 0x%x (%s)\n", b.texture, b.first, b.count, b.flags, g_tex.count(b.texture) ? "have" : "MISSING");
}
// pcb_assert_tp <mode> [min_u] [max_u]: Minecraft's camera mode and Portal 2's camera distance from the eye this frame
CON_COMMAND(pcb_assert_tp, "pcb_assert_tp <0|1|2> [min_u] [max_u] - the F5 camera mode and Portal 2's camera distance from the eye\n") {
	const int want = args.ArgC() > 1 ? std::atoi(args[1]) : 1;
	const float lo = args.ArgC() > 2 ? (float)std::atof(args[2]) : 0, hi = args.ArgC() > 3 ? (float)std::atof(args[3]) : 1e9f;
	const float d = g_tpMode ? g_tpDist : 0;
	char buf[220];
	std::snprintf(buf, sizeof buf, "Minecraft camera %u, view mode %d, distance %.1f u (Minecraft's zoom %.1f u, Portal 2 trace %.1f u), want mode %d and %.0f..%.0f u",
	              McStateNow().cameraMode, g_tpMode, d, g_tpMcDist, g_tpTraceDist, want, lo, hi);
	TestCheck(g_tpMode == want && (int)McStateNow().cameraMode == want && d >= lo && d <= hi, "F5 camera", buf);
}
// The third-person camera is not inside Portal 2's world (the view would render from solid: white or black)
CON_COMMAND(pcb_assert_tp_clear, "pcb_assert_tp_clear - Portal 2's (third-person) camera is in open space, not in a wall\n") {
	CGameTrace tr;
	engine.Trace(g_mainOrg, g_mainOrg, Vector(2, 2, 2), MASK_SOLID_BRUSHONLY, tr, true);
	const bool out = engine.PointOutsideWorld(engine.traceServer->ThisPtr(), g_mainOrg);
	char buf[160];
	std::snprintf(buf, sizeof buf, "camera at (%.1f %.1f %.1f): in solid %d, outside the map %d", g_mainOrg.x, g_mainOrg.y, g_mainOrg.z, tr.startsolid ? 1 : 0, out ? 1 : 0);
	TestCheck(!tr.startsolid && !out, "camera clear of walls", buf);
}
// pcb_assert_body <main|portal|none_main|region> [min] | reset
CON_COMMAND(pcb_assert_body, "pcb_assert_body reset | main <min> | portal <min> | nomain | region <min> - the body's draws since the reset\n") {
	static int bm = 0, bp = 0, br = 0, bf = 0;
	if (args.ArgC() < 2 || !std::strcmp(args[1], "reset")) return void((bm = g_drawsMain, bp = g_drawsPortal, br = g_skipRegion, bf = g_skipFirst));
	const int m = g_drawsMain - bm, p = g_drawsPortal - bp, r = g_skipRegion - br, f = g_skipFirst - bf;
	const int want = args.ArgC() > 2 ? std::atoi(args[2]) : 1;
	char buf[200];
	std::snprintf(buf, sizeof buf, "since the reset: main-view draws %d, portal-view draws %d, first-person main views skipped %d, views skipped in the region %d (%s, want %d)", m, p, f, r, args[1],
	              want);
	bool ok = false;
	if (!std::strcmp(args[1], "main")) ok = m >= want;
	else if (!std::strcmp(args[1], "portal")) ok = p >= want;
	else if (!std::strcmp(args[1], "nomain")) ok = m == 0;
	else if (!std::strcmp(args[1], "region")) ok = r >= want && m == 0 && p == 0;
	TestCheck(ok, "Minecraft player's body", buf);
}


// ---- test helpers (Chell mode, before the switch) -----------------------------------------------------------
void SnapPlayer(const Vector &pos, const QAngle &ang);  // ents.cpp
bool TeleportTo(const std::string &target, const Vector &o, const QAngle &a);  // floatorigin.cpp
static Vector g_wallHit, g_wallN;
static bool g_wallOk = false;
// pcb_av_wall <gap_u> <face|away>: the nearest wall around the player (72 horizontal traces at chest height, within
// 1500 u); the player is put gap_u in front of it on the same floor height, facing it or with its back to it.
CON_COMMAND(pcb_av_wall, "pcb_av_wall <gap_u> <face|away> - (tests) put the player gap_u in front of the nearest wall, facing it or back to it\n") {
	const float gap = args.ArgC() > 1 ? (float)std::atof(args[1]) : 100;
	const bool away = args.ArgC() > 2 && !std::strcmp(args[2], "away");
	const Vector feet = PlayerOrigin(), from = feet + Vector(0, 0, 48);
	// rays that hit a wall square on (normal within ~25 deg of the ray); the nearest whose spot gap_u in front of the
	// wall, back along the same (open) ray, is clear for the player's hull, inside the map and over a floor
	float best = 1e9f;
	Vector bestAt, bestDir;
	for (int i = 0; i < 72; ++i) {
		const float a = i * 5.0f * (float)M_PI / 180.0f;
		const Vector dir(std::cos(a), std::sin(a), 0);
		CGameTrace tr;
		engine.Trace(from, from + dir * 1500.0f, Vector(0, 0, 0), MASK_SOLID_BRUSHONLY, tr, true);
		if (tr.startsolid || tr.fraction >= 1.0f || std::fabs(tr.plane.normal.z) > 0.2f) continue;
		if (-(tr.plane.normal.x * dir.x + tr.plane.normal.y * dir.y) < 0.9f) continue;
		const float d = tr.fraction * 1500.0f;
		if (d < gap + 24.0f || d >= best) continue;
		const Vector at = feet + dir * (d - gap - 16.0f);
		CGameTrace hull, down;
		engine.Trace(at + Vector(0, 0, 37), at + Vector(0, 0, 37), Vector(16, 16, 36), MASK_SOLID_BRUSHONLY, hull, true);
		engine.Trace(at + Vector(0, 0, 4), at - Vector(0, 0, 64), Vector(0, 0, 0), MASK_SOLID_BRUSHONLY, down, true);
		if (hull.startsolid || down.fraction >= 1.0f || engine.PointOutsideWorld(engine.traceServer->ThisPtr(), at + Vector(0, 0, 36))) continue;
		// (#14: and clear of the chamber's entities: pc_pit_flings's floor has animated arm panels, solid props; a spot
		// inside one dropped the player through the floor to z -192 in a16_avatar1c)
		CGameTrace ents;
		engine.Trace(at + Vector(0, 0, 37), at + Vector(0, 0, 37), Vector(16, 16, 36), MASK_PLAYERSOLID, ents, false);
		if (ents.startsolid && (at - feet).Length() > 40.0f) continue;
		best = d, bestAt = at, bestDir = dir, g_wallHit = tr.endpos, g_wallN = Vector(tr.plane.normal.x, tr.plane.normal.y, 0);
	}
	g_wallOk = best < 1e8f;
	if (!g_wallOk) return void(console->Print("[bridge] pcb_av_wall: no usable wall within 1500 u\n"));
	g_wallN = g_wallN * (1.0f / g_wallN.Length());
	const float yaw = std::atan2(bestDir.y, bestDir.x) * 180.0f / (float)M_PI;
	// (#14: SnapPlayerToPosition takes the EYE position: given the feet it put the player 64 u into the floor)
	const QAngle ang{away ? 10.0f : 0.0f, away ? std::remainder(yaw + 180.0f, 360.0f) : yaw, 0};
	if (!TeleportTo("!player", bestAt, ang)) SnapPlayer(bestAt + Vector(0, 0, 64), ang);
	console->Print("[bridge] pcb_av_wall: wall at (%.1f %.1f %.1f) normal (%.2f %.2f), %.0f u away; player put at (%.1f %.1f %.1f) %s it\n", g_wallHit.x, g_wallHit.y, g_wallHit.z,
	               g_wallN.x, g_wallN.y, best, bestAt.x, bestAt.y, bestAt.z, away ? "with its back to" : "facing");
}
// pcb_av_portals <side_u>: the shared pair on the wall pcb_av_wall found: portal 0 straight ahead of the player, portal 1
// side_u along the wall; looking into portal 0 shows the player from portal 1 (you see yourself).
CON_COMMAND(pcb_av_portals, "pcb_av_portals <side_u> - (tests) both portals on the wall pcb_av_wall found, side by side\n") {
	if (!g_wallOk) return void(console->Print("[bridge] pcb_av_portals: run pcb_av_wall first\n"));
	const float side = args.ArgC() > 1 ? (float)std::atof(args[1]) : 120;
	const Vector feet = PlayerOrigin(), t(-g_wallN.y, g_wallN.x, 0);
	const float yaw = std::atan2(g_wallN.y, g_wallN.x) * 180.0f / (float)M_PI;
	const Vector base = Vector(g_wallHit.x, g_wallHit.y, feet.z + 56.0f) + g_wallN * 1.0f;
	for (int i = 0; i < 2; ++i) {
		const Vector c = base + t * (i ? side : 0.0f);
		char buf[200];
		std::snprintf(buf, sizeof buf, "pcb_portal %d %.1f %.1f %.1f 0 %.1f 0", i, c.x, c.y, c.z, yaw);
		engine.Cmd(buf);
	}
}

}  // namespace bridge
