// Portalcraft bridge: Minecraft's own section meshes drawn in Portal 2 (render ring consumer).
//
// The Fabric mod (WorldExporter) meshes every loaded 16^3 section with Minecraft's block
// renderer and streams it over the render ring: kRenAtlas (the block atlas, RGBA8) and
// kRenSection (triangle list of RenVertex, section-relative). Here each section becomes one
// static mesh in Source's material system (IMatRenderContext::CreateStaticMesh), with vertex
// positions already in Portal 2 world space (the region mapping in coords.hpp) and Minecraft's
// light baked into the vertex colour (sky light Curve(l) = l/(4-3l), block light, the fixed
// per-face shade). It is drawn in every 3D view by the render hook, through a dynamic mesh that
// borrows the static vertex/index buffers (GetDynamicMesh vertex/index override + auto-bind).
// kRenSolids gives, per section, which blocks are solid: Chell's movement-trace hook uses it.
#include "bridge.hpp"
#include "slots.hpp"
#include "growth.hpp"
#include "coords.hpp"
#include "link.hpp"
#include "colharvest.hpp"

#include "Modules/MaterialSystem.hpp"
#include "Modules/Tier1.hpp"
#include "Offsets.hpp"
#include "Utils/SDK/MeshBuilder.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace bridge {

void DigOnMessage(const uint8_t *p, uint32_t len);  // dig.cpp (bridge #33)
void MoverDigOnMessage(const uint8_t *p, uint32_t len);  // movers.cpp (bridge #43)

using namespace skycraft::proto;

// IMatRenderContext slots (numbers per operating system in slots.hpp): CreateStaticMesh, DestroyStaticMesh (both forward to
// the shader device; the queued context passes them straight to the real one), GetDynamicMesh 57.
// Portal 2's CreateStaticMesh has a 4th argument (a vertex-stream spec array, NULL = default),
// as in CS:GO's material system; the shader device walks it if non-null.
using _CreateStaticMesh = IMesh *(__rescall *)(IMatRenderContext *, VertexFormat fmt, const char *group, IMaterial *mat, void *streamSpec);
using _DestroyStaticMesh = void(__rescall *)(IMatRenderContext *, IMesh *);

// VertexFormat bits (Source): position, colour, texcoord 0 with two floats.
static const VertexFormat kFmtPos = 0x1, kFmtColor = 0x4;
static VertexFormat TexCoord0Size2() { return (VertexFormat)2 << 48; }

// ---- render-thread work ------------------------------------------------------------------
// Portal 2 runs the material system queued (mat_queue_mode -1 -> a render thread): the main
// thread only records. Static meshes must be created, filled, drawn and destroyed on the render
// thread, so that work is queued as functors on the queued context's call queue
// (IMatRenderContext slot 142 = GetCallQueue; NULL when not queued, then it runs inline).
struct QFunctor;
using QFn = void (*)(QFunctor *);
// Not a C++ polymorphic type: the first word is a hand-made vtable in CFunctor's shape.
struct QFunctor {
	void **vtbl;
	int refs;
	QFn fn;
	QFn destroy;
};
static int __rescall Q_AddRef(QFunctor *f) { return __atomic_add_fetch(&f->refs, 1, __ATOMIC_ACQ_REL); }
static int __rescall Q_Release(QFunctor *f) {
	int r = __atomic_sub_fetch(&f->refs, 1, __ATOMIC_ACQ_REL);
	if (r == 0) f->destroy(f);
	return r;
}
static void __rescall Q_Run(QFunctor *f) {
	if (f->fn) f->fn(f), f->fn = nullptr;  // run once whichever slot the queue calls
}
// CFunctor's layout: IRefCounted (AddRef, Release) then operator(); extra slots cover a build
// that also has a virtual destructor in between.
static void *g_qvtbl[] = {(void *)Q_AddRef, (void *)Q_Release, (void *)Q_Run, (void *)Q_Run, (void *)Q_Run};

using _GetCallQueue = void *(__rescall *)(IMatRenderContext *);
using _QueueFunctorInternal = void(__rescall *)(void *queue, QFunctor *f);

template <typename F> struct QCall : QFunctor {  // QFunctor must stay first (standard layout)
	F body;
	explicit QCall(F &&b) : body(std::move(b)) {}
};

// Run body on the render thread (or now, if the material system is not queued).
template <typename F> static void OnRenderThread(F &&body) {
	auto *f = new QCall<F>(std::forward<F>(body));
	f->vtbl = g_qvtbl;
	f->refs = 1;
	f->fn = [](QFunctor *q) { static_cast<QCall<F> *>(q)->body(); };
	f->destroy = [](QFunctor *q) { delete static_cast<QCall<F> *>(q); };
	// Windows safe-load: without a verified GetCallQueue slot the work is dropped (running it on this thread is only valid when the material
	// system is not queued, and a queued context would crash); the skip is logged by slots::Use.
	if (!slots::Use(slots::kRcGetCallQueue)) {
		Q_Release(f);
		return;
	}
	auto rc = materialSystem->GetRenderContext();
	void *queue = slots::Vtbl<_GetCallQueue>(rc, slots::kRcGetCallQueue)(rc);
	rc->vtable->Release(rc);
	if (!queue) {
		Q_Run(f);
		Q_Release(f);
		return;
	}
	Q_AddRef(f);  // the queue's reference (ICallQueue::QueueFunctor does RetAddRef)
	Memory::VMT<_QueueFunctorInternal>(queue, slots::Index(slots::kCallQueueQueueFunctor))(queue, f);
	Q_Release(f);  // ours
}

// For other files: run fn on the render thread (queued material system) without waiting.
void RunOnRenderThread(void (*fn)(void *), void *arg) {
	OnRenderThread([fn, arg] { fn(arg); });
}

struct GpuVertex {
	float x, y, z, u, v;
	uint8_t bgra[4];
};

struct Section {
	std::vector<GpuVertex> verts;  // freed after upload (render thread)
	std::vector<IMesh *> meshes;   // render thread only
	std::vector<int> counts;
	bool uploaded = false;
	std::vector<GpuVertex> tverts;  // bridge #17: the band's translucent triangles (drawn blended, after the opaque ones)
	bool band = false;  // bridge #17: a chamber-band section (outside the region box): drawn always, lit by Portal 2
	bool animated = false;         // bridge #18: shows an animated sprite (water, lava, fire...)
	std::vector<GpuVertex> averts, atverts;  // bridge #18: its triangles in the animation texture (opaque, translucent)
	std::vector<GpuVertex> skirt;  // bridge #18: contact shadows on Portal 2's surfaces around the placed blocks (quads)
	Vector centre;
};
using SectionPtr = std::shared_ptr<Section>;

static std::map<std::tuple<int, int, int>, SectionPtr> g_sections;
PCB_GAUGE("terrain.sections", g_sections.size());
static std::unordered_map<long long, std::vector<uint64_t>> g_solids;  // section key -> 64 words
PCB_GAUGE("terrain.solids", g_solids.size());
static std::unordered_map<long long, std::vector<uint64_t>> g_fluids;  // section key -> water 64 + lava 64 words
PCB_GAUGE("terrain.fluids", g_fluids.size());
static std::unordered_map<long long, std::vector<uint64_t>> g_noPortal;  // section key -> 64 words: solid but not portalable
static std::unordered_map<long long, int> g_airSections;  // streamed sections with no solid block (known air)
PCB_GAUGE("terrain.air", g_airSections.size());
static IMaterial *g_terrainMat = nullptr, *g_terrainMatTl = nullptr;
static char g_atlasName[64] = "";
static int g_atlasGen = 0;
static ITexture *g_terrainTex = nullptr;
static int g_atlasW = 0, g_atlasH = 0, g_atlasTexH = 0;
static int g_meshesBuilt = 0, g_vertsBuilt = 0, g_msgs = 0;
static std::atomic<int> g_uploaded{0};
static bool g_enabled = true;
static float g_exposure = 1.0f;
static bool g_unlit = false;
static float g_drawDistance = 20000;
static int g_uploadsPerView = 24;

void AvatarOnAtlas(const char *texName, int w, int h, int texH);  // avatar.cpp (bridge #16)
void AvatarOnTexture(const uint8_t *p, uint32_t bytes);
void AvatarOnMesh(const uint8_t *p, uint32_t bytes);
void SceneOnMesh(const uint8_t *p, uint32_t bytes);  // scene.cpp (bridge #19)

// bridge #17: the chamber band. Sections outside the region box hold blocks the Minecraft player placed in Portal 2's
// chambers (MC y 328-463 in pc_pit_flings, above vanilla's build limit; the mod raises the overworld's height). Portal 2
// draws them (zero latency, its depth, portals and recursion), lit SkyCraft's WorldRender way with Portal 2's light. Their
// raw records are kept (placed blocks only: small), so a mesh clear (a floating-origin shift) rebuilds them.
bool ClipHasRegion();                          // clip.cpp
bool ClipRegionContains(const Vector &p);      // clip.cpp
bool TerrainMeshesDebug();                     // render.cpp (pcb_terrain_meshes)
float RenderLightToScale(float linear);        // render.cpp
static std::map<std::tuple<int, int, int>, std::vector<uint8_t>> g_bandRaw;
PCB_GAUGE("terrain.bandRaw", g_bandRaw.size());
static int g_bandBuilt = 0, g_bandLightCalls = 0;
static int g_bandLit = 2;  // pcb_band_light: 0 Minecraft's own baked light, 1 #17's ComputeLighting x5, 2 #18's model lighting state
// bridge #18 (bandlight.cpp): Portal 2's model lighting state per vertex, contact AO, contact shadows on Portal 2
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
static std::unordered_map<long long, P2State> g_bandStateCache;  // sample key -> Portal 2 lighting state (lead)
PCB_GAUGE("terrain.bandStateCache", g_bandStateCache.size());
// (bridge #23) band sections lit with an empty lighting state: rebuilt again a few frames later, up to 10 times
struct BandRelight {
	int tries = 0, frames = 0;
};
static std::map<std::tuple<int, int, int>, BandRelight> g_bandRelight;
PCB_GAUGE("terrain.bandRelight", g_bandRelight.size());
static P2State g_bandLastGood{};
static bool g_bandHaveGood = false;
static int g_bandRelights = 0, g_bandRelightGiveUps = 0, g_bandDeferred = 0;
static void BandRelightQueue(const std::tuple<int, int, int> &key, int stillDark) {
	BandRelight &r = g_bandRelight[key];
	if (r.tries >= 10) {
		++g_bandRelightGiveUps;
		g_bandRelight.erase(key);
		return;
	}
	r.frames = 3 + r.tries * 2;  // (3, 5, 7... frames: the engine's lighting queries work again within a few client frames)
	if (r.tries == 0 || stillDark) console->Print("[bridge] band: section %d %d %d lit with %d black lighting states: relight %d in %d frames\n", std::get<0>(key), std::get<1>(key),
	                                             std::get<2>(key), stillDark, r.tries + 1, r.frames);
}
static std::map<std::tuple<int, int, int>, uint64_t> g_sectionHash;  // lead: skip Minecraft's identical resends
PCB_GAUGE("terrain.sectionHash", g_sectionHash.size());
static bool g_forceSectionBuild = false;
static int g_sectionSkips = 0;
Vector BandLightEval(const P2State &s, const Vector &p, const Vector &n, bool hasNormal);
float BandContactAO(const Vector &p, const Vector &n, int dir);
struct SkirtVert {
	Vector p;
	uint8_t a;
};
void BandSkirts(int bx, int by, int bz, std::vector<SkirtVert> &out);
IMaterial *BandSkirtMaterial();
void BandLightFrame();
void BandLightRampDraw(const Vector &eye);
static void SkirtDirtyAround(int sx, int sy, int sz);
void BlockLightsOnLights(const uint8_t *p, uint32_t bytes);  // blocklights.cpp
void BlockLightsClear();
void BlockLightsFrame(const Vector &eye);
void BlockLightsSuspend();
void BlockLightsResume();
static float g_bandK = 1.0f;     // linear light -> vertex colour (pcb_band_light 2 <k>)
static float g_bandBlockK = 0.3f;  // Minecraft block light (torches) at full level, in the same linear units
static int g_cutFaceRule = 1;  // (bridge #80) pc_cut_light_faces: 1 a face looking into the cut's dug air takes the cut's light, 0 only a face whose sample point is in host solid (#79)
static float g_bandFill = 0.1f;  // a face never darker than this share of its sample's mean ambient (bounce)
static int g_bandSkirts = 1;
static int g_bandStateOk = 0, g_bandStateEmpty = 0;
static long g_bandCutLit = 0;  // (bridge #79) band vertices lit from a cut's mouths
static double g_bandBuildMs = 0, g_bandBuildMax = 0;  // main-thread cost of building band sections (lighting, AO)
static float g_bandGamma = 2.2f;  // vertex colour = linear light ^ (1/gamma): the shader decodes vertex colours (pcb_light_ramp)  // pcb_band_skirts
// The world light at the face (GetLightForPoint) x this is a floor under the ambient cube (the cube alone left block tops
// near black); the sum goes through the test blocks' scale (RenderLightToScale, x5). The user approved this look (b17e).
static float g_bandWorldK = 1.0f;
static bool g_bandRebuild = false;  // rebuild the band from its raw records in the next client frame (TerrainConsume)
void TerrainBandRebuild() { g_bandRebuild = true; }
// (bridge #79, cutlight.cpp) Portal 2's light at the cuts' mouths, carried into the dug cells one level per block
void CutLightFrame();
bool CutLightVertex(double x, double y, double z, int dir, int sky, Vector &out, float *meanOut);
int CutLightCellDist(int x, int y, int z);  // cutlight.cpp: blocks from a mouth, -1 when the cell is not dug air of the cut field
// (bridge #73) the band sections around a cell whose dug state changed: rebuilt from their raw records (their coplanar faces depend on it)
static std::set<std::tuple<int, int, int>> g_bandRebuildKeys;
void TerrainBandCellChanged(int x, int y, int z) {
	for (int dy = -1; dy <= 1; ++dy)
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx) g_bandRebuildKeys.insert(std::make_tuple((x + dx) >> 4, (y + dy) >> 4, (z + dz) >> 4));
}
float BandBlockK() { return g_bandBlockK; }  // (bridge #27) blocklights.cpp: Minecraft's block light on models, as on the blocks
bool ClipInTerrainBox(const Vector &p);  // clip.cpp (bridge #61): the terrain's box, also on a map without a region (campaign maps in a terrain session)
static bool BandAt(const Vector &centre) { return !ClipInTerrainBox(centre); }

static long long SectionKey(int sx, int sy, int sz) { return ((long long)(sx & 0x3FFFFF) << 42) | ((long long)(sz & 0x3FFFFF) << 20) | (long long)(sy & 0xFFFFF); }

// Render thread: create and fill the static meshes of a section.
static void Upload(Section &sec, IMaterial *mat) {
	auto rc = materialSystem->GetRenderContext();
	auto create = slots::Vtbl<_CreateStaticMesh>(rc, slots::kRcCreateStaticMesh);
	const uint32_t maxPer = 65535 - 65535 % 3;
	uint32_t n = (uint32_t)sec.verts.size();
	for (uint32_t first = 0; first < n; first += maxPer) {
		uint32_t cnt = std::min(n - first, maxPer);
		IMesh *mesh = create(rc, kFmtPos | kFmtColor | TexCoord0Size2(), "Portalcraft", mat, nullptr);
		if (!mesh) break;
		MeshDesc desc;
		std::memset(&desc, 0, sizeof desc);
		mesh->LockMesh((int)cnt, (int)cnt, desc);
		for (uint32_t i = 0; i < cnt; ++i) {
			const GpuVertex &gv = sec.verts[first + i];
			float *pp = (float *)((uint8_t *)desc.vertex_data.position + i * desc.vertex_size.position);
			pp[0] = gv.x, pp[1] = gv.y, pp[2] = gv.z;
			float *uv = (float *)((uint8_t *)desc.vertex_data.tex_coord[0] + i * desc.vertex_size.tex_coord[0]);
			uv[0] = gv.u, uv[1] = gv.v;
			std::memcpy(desc.vertex_data.color + i * desc.vertex_size.color, gv.bgra, 4);
			desc.index_data[i] = (unsigned short)(desc.first_vertex + i);
		}
		mesh->UnlockMesh((int)cnt, (int)cnt, desc);
		sec.meshes.push_back(mesh);
		sec.counts.push_back((int)cnt);
	}
	std::vector<GpuVertex>().swap(sec.verts);
	sec.uploaded = true;
	rc->vtable->Release(rc);
	++g_uploaded;
}

static void DestroyOnRenderThread(SectionPtr sec) {
	OnRenderThread([sec]() {
		auto rc = materialSystem->GetRenderContext();
		auto destroy = slots::Vtbl<_DestroyStaticMesh>(rc, slots::kRcDestroyStaticMesh);
		for (auto *m : sec->meshes) destroy(rc, m);
		sec->meshes.clear();
		rc->vtable->Release(rc);
	});
}

void ClipExitRecheck(const char *why);  // clip.cpp
static uint32_t g_solidsGen = 0;  // bumps on every solids message and every clear (the clip re-checks its exit)
uint32_t TerrainSolidsGen() { return g_solidsGen; }
size_t TerrainSectionsKnown() { return g_solids.size() + g_airSections.size(); }

// Meshes are placed with the region mapping in force when they were built; the solids, fluids and
// known-air sets are keyed by Minecraft section and do not depend on it.
static void OnSection(const uint8_t *p, uint32_t bytes);
void BandLightCacheClear();  // bandlight.cpp (bridge #18: its AO cache is keyed by Source position)
void TerrainClearMeshes() {
	BandLightCacheClear();
	for (auto &kv : g_sections) DestroyOnRenderThread(kv.second);
	g_sections.clear();
	// bridge #17: the chamber band's placed blocks are rebuilt through the new mapping (Minecraft sends a section once), in the
	// next client frame (Portal 2's lighting reads 0 outside it)
	g_bandRebuild = !g_bandRaw.empty();
}

// (bridge #61) A new level: everything keyed by a Source position or by Portal 2's lighting of the OLD map goes (the lighting states and the contact-AO
// cache of the placed blocks would light the new map's blocks with the old map's light), and the band's placed blocks are rebuilt once the new
// level is live (its lighting is only readable then). The solids, fluids and sections are Minecraft's, keyed by Minecraft section: they stay.
static bool g_bandLevelPending = false;
bool McLevelIsLive();  // mc.cpp
void CutLightReset();  // cutlight.cpp (bridge #79)
void TerrainLevelReset() {
	CutLightReset();
	g_bandStateCache.clear();
	g_bandRelight.clear();
	BandLightCacheClear();
	g_bandLevelPending = !g_bandRaw.empty();
}

void TerrainClear() {
	g_bandRaw.clear();
	g_sectionHash.clear();
	g_bandStateCache.clear();
	TerrainClearMeshes();
	const bool had = !g_solids.empty() || !g_airSections.empty();
	g_solids.clear();
	g_fluids.clear();
	g_noPortal.clear();
	g_airSections.clear();
	++g_solidsGen;
	if (had) ClipExitRecheck("terrain cleared");
}

// bridge #18: the atlas keeps its pixels so Minecraft's animated sprites (water, lava, fire, ...; kRenAtlasRegion, one frame
// per game tick) can be patched in and uploaded again (the material system re-reads the whole texture through this).
class AtlasRegen : public ITextureRegenerator {
public:
	std::vector<uint8_t> bgra;
	int w = 0, h = 0;
	void RegenerateTextureBits(ITexture *, IVTFTexture *vtf, Rect_t *) override {
		auto fmtFn = Memory::VMT<ImageFormat(__rescall *)(IVTFTexture *)>(vtf, Offsets::ImageFormat);
		auto dataFn = Memory::VMT<uint8_t *(__rescall *)(IVTFTexture *)>(vtf, Offsets::ImageData);
		if (fmtFn(vtf) != IMAGE_FORMAT_BGRA8888 || bgra.empty()) return;
		if (uint8_t *dst = dataFn(vtf)) std::memcpy(dst, bgra.data(), bgra.size());
	}
	void Release() override {}  // static; textures of older atlases keep pointing here (same size or replaced together)
};
static AtlasRegen g_atlasRegen[2];
static int g_atlasRegenIdx = 0;
// Animated sprites (water, lava, fire, ...) live in a small texture of their own (the block atlas is 2048x4096: re-uploading it
// for every animation frame cost far too much): band triangles that use one get their UVs remapped into it at build time.
struct AnimRect {
	int x, y, w, h;  // in the block atlas
	int ax = -1, ay = -1;  // in the animation texture (-1: not laid out yet)
};
static std::vector<AnimRect> g_animRects;  // animated sprites seen so far
static AtlasRegen g_animRegen[2];
static int g_animRegenIdx = 0, g_animW = 512, g_animH = 0, g_animGen = 0;
static ITexture *g_animTex = nullptr;
static IMaterial *g_animMat = nullptr, *g_animMatTl = nullptr;
static bool g_animLayoutDirty = false, g_animDirty = false;
static double g_animNewAt = 0, g_animDownAt = 0;
static int g_animDownloads = 0, g_atlasRegions = 0;
static bool g_bandAnimated = false;  // a band section shows an animated sprite: only then is the animation texture uploaded
static float g_animHz = 20.0f;       // pc_mc_anim_hz (Minecraft animates per game tick: 20 Hz)
// the atlas is sRGB (0x40, TEXTUREFLAGS_SRGB: decoded to linear in the shader, as Portal 2's own textures): Minecraft's colours
// come out as Minecraft shows them at vertex colour 1 and tonemap 1 (without it they were washed out). pcb_atlas_srgb (next atlas).
static bool g_atlasSrgb = true;
static double NowSec() {
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static void CopyRect(const uint8_t *src, int sw, int sx, int sy, uint8_t *dst, int dw, int dx, int dy, int w, int h, bool swapRB) {
	for (int y = 0; y < h; ++y) {
		const uint8_t *q = src + ((size_t)(sy + y) * sw + sx) * 4;
		uint8_t *d = dst + ((size_t)(dy + y) * dw + dx) * 4;
		if (!swapRB) std::memcpy(d, q, (size_t)w * 4);
		else
			for (int x = 0; x < w; ++x, d += 4, q += 4) d[0] = q[2], d[1] = q[1], d[2] = q[0], d[3] = q[3];
	}
}

static void OnAtlasRegion(const uint8_t *p, uint32_t bytes) {
	if (bytes < sizeof(RenAtlasRegion)) return;
	auto *r = (const RenAtlasRegion *)p;
	AtlasRegen &ag = g_atlasRegen[g_atlasRegenIdx];
	if (ag.bgra.empty() || r->x + r->width > (uint32_t)ag.w || r->y + r->height > (uint32_t)ag.h) return;
	if (bytes < sizeof(RenAtlasRegion) + (uint64_t)r->width * r->height * 4) return;
	++g_atlasRegions;
	AnimRect *ar = nullptr;
	for (auto &a : g_animRects)
		if (a.x == (int)r->x && a.y == (int)r->y) ar = &a;
	if (!ar) {
		g_animRects.push_back({(int)r->x, (int)r->y, (int)r->width, (int)r->height});
		g_animLayoutDirty = true, g_animNewAt = NowSec();
		ar = &g_animRects.back();
	}
	const uint8_t *src = p + sizeof(RenAtlasRegion);
	CopyRect(src, (int)r->width, 0, 0, ag.bgra.data(), ag.w, (int)r->x, (int)r->y, (int)r->width, (int)r->height, true);  // (kept current)
	if (ar->ax >= 0 && !g_animRegen[g_animRegenIdx].bgra.empty()) {
		AtlasRegen &an = g_animRegen[g_animRegenIdx];
		CopyRect(src, (int)r->width, 0, 0, an.bgra.data(), an.w, ar->ax, ar->ay, ar->w, ar->h, true);
		g_animDirty = true;
	}
}

// The animation texture's layout (shelf packing, 512 wide) once no new sprite has come for 0.5 s; the band is rebuilt so
// its triangles point into it.
static void AnimLayout() {
	int x = 0, y = 0, shelf = 0;
	for (auto &a : g_animRects) {
		if (x + a.w > g_animW) x = 0, y += shelf, shelf = 0;
		a.ax = x, a.ay = y;
		x += a.w, shelf = std::max(shelf, a.h);
	}
	int h = 1;
	while (h < y + shelf) h <<= 1;
	g_animH = h;
	g_animRegenIdx ^= 1;
	AtlasRegen &an = g_animRegen[g_animRegenIdx];
	an.w = g_animW, an.h = h;
	an.bgra.assign((size_t)g_animW * h * 4, 0);
	const AtlasRegen &ag = g_atlasRegen[g_atlasRegenIdx];
	for (auto &a : g_animRects) CopyRect(ag.bgra.data(), ag.w, a.x, a.y, an.bgra.data(), an.w, a.ax, a.ay, a.w, a.h, false);
	char tex[64], mat[64];
	++g_animGen;
	std::snprintf(tex, sizeof tex, "_pcb_mc_anim%d", g_animGen);
	auto CreateProceduralTexture = (ITexture * (__rescall *)(void *, const char *, const char *, int, int, ImageFormat, int)) materialSystem->materials->Original(Offsets::CreateProceduralTexture);
	g_animTex = CreateProceduralTexture(materialSystem->materials->ThisPtr(), tex, "Portalcraft textures", g_animW, h, IMAGE_FORMAT_BGRA8888,
	                                    TEXTUREFLAGS_NOMIP | TEXTUREFLAGS_NOLOD | TEXTUREFLAGS_SINGLECOPY | TEXTUREFLAGS_POINTSAMPLE | (g_atlasSrgb ? 0x40 : 0));
	if (!g_animTex) return;
	g_animTex->SetTextureRegenerator(&an, false);
	g_animTex->Download();
	auto kv = new KeyValues("UnlitGeneric");
	kv->SetString("$basetexture", tex);
	kv->SetInt("$vertexcolor", 1);
	kv->SetInt("$alphatest", 1);
	std::snprintf(mat, sizeof mat, "_pcb_mc_anim_mat%d", g_animGen);
	g_animMat = materialSystem->CreateMaterial(mat, kv);
	kv = new KeyValues("UnlitGeneric");
	kv->SetString("$basetexture", tex);
	kv->SetInt("$vertexcolor", 1);
	kv->SetInt("$vertexalpha", 1);
	kv->SetInt("$translucent", 1);
	std::snprintf(mat, sizeof mat, "_pcb_mc_anim_tl%d", g_animGen);
	g_animMatTl = materialSystem->CreateMaterial(mat, kv);
	g_animLayoutDirty = false;
	g_bandRebuild = true;
	console->Print("[bridge] animated sprites: %zu in a %dx%d texture\n", g_animRects.size(), g_animW, h);
}

// Triangles showing an animated sprite move to the animation texture's lists (UVs remapped).
static void SplitAnimated(std::vector<GpuVertex> &vs, std::vector<GpuVertex> &out, float vScale) {
	if (!g_animTex || !g_atlasW || g_animH <= 0) return;
	std::vector<GpuVertex> keep;
	keep.reserve(vs.size());
	for (size_t t = 0; t + 2 < vs.size(); t += 3) {
		const float cu = (vs[t].u + vs[t + 1].u + vs[t + 2].u) / 3 * g_atlasW, cv = (vs[t].v + vs[t + 1].v + vs[t + 2].v) / 3 / vScale * g_atlasH;
		const AnimRect *hit = nullptr;
		for (auto &a : g_animRects)
			if (a.ax >= 0 && cu >= a.x && cu <= a.x + a.w && cv >= a.y && cv <= a.y + a.h) hit = &a;
		if (!hit) {
			keep.insert(keep.end(), vs.begin() + t, vs.begin() + t + 3);
			continue;
		}
		for (int k = 0; k < 3; ++k) {
			GpuVertex g = vs[t + k];
			const float px = g.u * g_atlasW, py = g.v / vScale * g_atlasH;
			g.u = (hit->ax + (px - hit->x)) / (float)g_animW;
			g.v = (hit->ay + (py - hit->y)) / (float)g_animH;
			out.push_back(g);
		}
	}
	vs.swap(keep);
}

// Client frame: lay out new sprites; upload the animation texture (small) at most g_animHz times a second.
static void AtlasAnimate() {
	const double now = NowSec();
	if (g_animLayoutDirty && now - g_animNewAt > 0.5) AnimLayout();
	if (!g_animDirty || !g_animTex || !g_bandAnimated || g_animHz <= 0) return;
	if (now - g_animDownAt < 1.0 / g_animHz) return;
	g_animDownAt = now, g_animDirty = false, ++g_animDownloads;
	ITexture *tex = g_animTex;
	OnRenderThread([tex]() { tex->Download(); });
}

static void OnAtlas(const uint8_t *p, uint32_t bytes) {
	uint32_t w = ((const uint32_t *)p)[0], h = ((const uint32_t *)p)[1];
	if (bytes < 8 + (uint64_t)w * h * 4) return;
	int th = 1;
	while (th < (int)h) th <<= 1;  // power-of-two height; v is rescaled
	std::vector<uint8_t> bgra((size_t)w * th * 4, 0);
	const uint8_t *src = p + 8;
	for (size_t i = 0; i < (size_t)w * h; ++i) {
		bgra[i * 4 + 0] = src[i * 4 + 2];
		bgra[i * 4 + 1] = src[i * 4 + 1];
		bgra[i * 4 + 2] = src[i * 4 + 0];
		bgra[i * 4 + 3] = src[i * 4 + 3];
	}
	TerrainClear();
	// The old material/texture may still be referenced by queued draws: keep them (a new atlas
	// only comes with a new Minecraft session).
	static int gen = 0;
	char texName[64], matName[64];
	std::snprintf(texName, sizeof texName, "_pcb_mc_atlas%d", ++gen);
	std::snprintf(matName, sizeof matName, "_pcb_mc_terrain%d", gen);
	std::snprintf(g_atlasName, sizeof g_atlasName, "%s", texName);
	++g_atlasGen;
	{
		// (both regenerators alternate: a texture of the previous atlas may still be drawn while this one fills)
		g_atlasRegenIdx ^= 1;
		AtlasRegen &ag = g_atlasRegen[g_atlasRegenIdx];
		ag.bgra = bgra, ag.w = (int)w, ag.h = th;
		g_animRects.clear();
		g_animTex = nullptr, g_animMat = g_animMatTl = nullptr, g_animLayoutDirty = false;
		auto CreateProceduralTexture = (ITexture * (__rescall *)(void *, const char *, const char *, int, int, ImageFormat, int)) materialSystem->materials->Original(Offsets::CreateProceduralTexture);
		g_terrainTex = CreateProceduralTexture(materialSystem->materials->ThisPtr(), texName, "Portalcraft textures", (int)w, th, IMAGE_FORMAT_BGRA8888,
		                                       TEXTUREFLAGS_NOMIP | TEXTUREFLAGS_NOLOD | TEXTUREFLAGS_SINGLECOPY | TEXTUREFLAGS_POINTSAMPLE | (g_atlasSrgb ? 0x40 : 0));
		if (g_terrainTex) {
			g_terrainTex->SetTextureRegenerator(&ag, false);
			g_terrainTex->Download();
		}
	}
	auto kv = new KeyValues("UnlitGeneric");
	kv->SetString("$basetexture", texName);
	kv->SetInt("$vertexcolor", 1);
	kv->SetInt("$alphatest", 1);
	if (std::getenv("PCB_BAND_NOCULL")) kv->SetInt("$nocull", 1);  // (test aid) is a missing wall a culled one?
	g_terrainMat = materialSystem->CreateMaterial(matName, kv);
	{  // bridge #17: the band's translucent pass (SkyCraft: blended, after the opaque triangles)
		auto tl = new KeyValues("UnlitGeneric");
		tl->SetString("$basetexture", texName);
		tl->SetInt("$vertexcolor", 1);
		tl->SetInt("$vertexalpha", 1);
		tl->SetInt("$translucent", 1);
		char tlName[64];
		std::snprintf(tlName, sizeof tlName, "_pcb_mc_terrain_tl%d", gen);
		g_terrainMatTl = materialSystem->CreateMaterial(tlName, tl);
	}
	g_atlasW = (int)w, g_atlasH = (int)h, g_atlasTexH = th;
	AvatarOnAtlas(texName, (int)w, (int)h, th);  // bridge #16: held items and armour trims use the atlas
	console->Print("[bridge] terrain: atlas %ux%u (texture %ux%d) -> %s\n", w, h, w, th, g_terrainMat ? "ok" : "FAILED");
}

static float Curve(float l) { return l / (4.0f - 3.0f * l); }

// (bridge #73, DESIGN-dig-render.md 2) Under the cut-away shaders (pc_dig_render 2) a revealed block's face that lies on a visible
// Portal 2 surface is not drawn: the block's own cell is host-solid right behind the face and the cell in front is host-empty and
// not dug (Portal 2's surface is there, at the same depth: the two z-fought, 31), or the cell in front is host-solid itself (the
// face is buried). Faces toward dug cells (the hole's walls) and every face of a block placed in open air are kept.
int DigRenderMode();                                                                                            // cutrender.cpp
void CollisionFieldSentIn(int bx0, int by0, int bz0, int bx1, int by1, int bz1, std::vector<ColBlock> &out);  // collfield.cpp
bool DigIsDugMc(int bx, int by, int bz);                                                                        // dig.cpp
static long g_coplanarDropped = 0, g_coplanarKept = 0;
static bool CullCoplanarFaces(const RenVertex *v, uint32_t n, double ox, double oy, double oz, std::vector<RenVertex> &out) {
	static const int kN[7][3] = {{0, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
	struct Cell {
		bool have;
		ColBlock b;
	};
	std::unordered_map<long long, Cell> cache;
	auto cellAt = [&](int x, int y, int z) -> const Cell & {
		const long long k = ((long long)(x & 0x1FFFFF) << 42) | ((long long)(y & 0x1FFFFF) << 21) | (long long)(z & 0x1FFFFF);
		auto it = cache.find(k);
		if (it != cache.end()) return it->second;
		std::vector<ColBlock> got;
		CollisionFieldSentIn(x, y, z, x, y, z, got);
		Cell c{!got.empty(), {}};
		if (c.have) c.b = got[0];
		return cache.emplace(k, c).first->second;
	};
	// solid sub-voxels (of 64) in the 8 x 8 layer of a cell's export at index `at` along `axis`
	auto layer = [](const ColBlock &b, int axis, int at) {
		int s = 0;
		for (int i = 0; i < 8; ++i)
			for (int j = 0; j < 8; ++j) {
				int x = 0, y = 0, z = 0;
				if (axis == 0) x = at, y = i, z = j;
				else if (axis == 1) y = at, x = i, z = j;
				else z = at, x = i, y = j;
				s += (int)(b.bits[y] >> (z * 8 + x) & 1);
			}
		return s;
	};
	out.clear();
	out.reserve(n);
	bool any = false;
	for (uint32_t i = 0; i < n;) {
		const int d = (v[i].flags >> 4) & 7;
		bool quad = i + 6 <= n && d >= 1 && d <= 6;
		for (uint32_t k = 1; quad && k < 6; ++k) quad = (int)((v[i + k].flags >> 4) & 7) == d;
		quad = quad && v[i].x == v[i + 3].x && v[i].y == v[i + 3].y && v[i].z == v[i + 3].z && v[i + 2].x == v[i + 4].x && v[i + 2].y == v[i + 4].y && v[i + 2].z == v[i + 4].z;
		if (!quad) {
			const uint32_t m = std::min<uint32_t>(3, n - i);
			out.insert(out.end(), v + i, v + i + m);
			i += m;
			continue;
		}
		double c[3] = {0, 0, 0};
		for (uint32_t k = 0; k < 6; ++k) c[0] += v[i + k].x, c[1] += v[i + k].y, c[2] += v[i + k].z;
		c[0] = ox + c[0] / 6.0, c[1] = oy + c[1] / 6.0, c[2] = oz + c[2] / 6.0;
		int axis = 0;
		for (int a = 0; a < 3; ++a)
			if (kN[d][a]) axis = a;
		bool drop = false;
		if (std::fabs(c[axis] - std::round(c[axis])) < 0.01) {
			int in[3], outc[3];
			for (int a = 0; a < 3; ++a) in[a] = (int)std::floor(c[a] - kN[d][a] * 0.5), outc[a] = (int)std::floor(c[a] + kN[d][a] * 0.5);
			// a block in a dug (revealed) cell: the cut-away shaders already remove Portal 2's surface there, its faces are what shows
			if (!DigIsDugMc(outc[0], outc[1], outc[2]) && !DigIsDugMc(in[0], in[1], in[2])) {
				const Cell &ci = cellAt(in[0], in[1], in[2]);
				const Cell &co = cellAt(outc[0], outc[1], outc[2]);
				const int pos = kN[d][axis] > 0;
				const int inL = ci.have ? layer(ci.b, axis, pos ? 7 : 0) : 0, outL = co.have ? layer(co.b, axis, pos ? 0 : 7) : 0;
				drop = outL == 64 || (inL == 64 && outL == 0);
			}
		}
		if (drop) any = true, ++g_coplanarDropped;
		else out.insert(out.end(), v + i, v + i + 6), ++g_coplanarKept;
		i += 6;
	}
	return any;
}

// (bridge #76) CUT CAPS. Where a dug cell D meets a cell N that stays Portal 2's (not dug, no Minecraft block), the part of their shared
// face inside Portal 2's visible solid is the hole's wall. The mod draws it (DigWalls, SkyCraft's sampled walls) from the host's 8x8x8
// sub-voxels: a wall face that is not on the 5 u sub-grid leaves a strip of up to 2.5 u between Portal 2's cut wall face and the
// Minecraft wall (Portal 2's clear colour shows through: the user's "white void at the seam", 38_seam_void.png), or sticks out of it.
// SkyCraft caps exactly along the host's own triangles where it has them (DigWalls landSide); here the host's solid is exactly known
// (the BSP: solid leaves, visible brushes, static-prop hulls; colharvest World::SolidParts), so every such face is replaced by its exact
// clip, textured and lit as the mod's wall (the same atlas rect: the block the cell would be revealed as) and built like any band face.
std::shared_ptr<const colharvest::World> CollisionFieldHarvestWorld();  // collfield.cpp
bool TerrainIsSolid(int bx, int by, int bz);
static int g_capsOn = 1;  // pc_dig_caps
static long g_capsFaces = 0, g_capsPieces = 0, g_capsModDropped = 0, g_capsModKept = 0, g_capsNoTex = 0, g_capsSections = 0;
static double g_capsMs = 0;
static std::set<long long> g_capSections;  // band sections whose mesh holds caps (rebuilt when their Minecraft blocks change)
static std::map<std::pair<long long, int>, int> g_capEmit;  // (bridge #79, pc_dig_caps_at) (dug cell, dir) -> cap pieces in its section's last build (-1: the mod's wall kept)
struct CapRectInfo {
	float u0, v0, u1, v1, span, opaque;
	char src;  // f: this face's wall quad, c: the cell's, s: the section's
};
static bool g_capTraceOn = false;  // (bridge #80) pc_dig_caps_trace: one face's final triangles at its section's next build
static std::tuple<int, int, int> g_capTraceSec;
static int g_capTraceAxis = 0;
static float g_capTracePlane = 0, g_capTraceLo[3] = {0, 0, 0}, g_capTraceHi[3] = {0, 0, 0};
static int g_capRectPick = 1;  // pc_dig_caps_rect: 1 = widest source quad + atlas alpha check (bridge #80), 0 = #76's (last quad seen, first candidate)
static std::map<std::pair<long long, int>, CapRectInfo> g_capRect;  // (bridge #80, pc_dig_caps_at) the atlas rect each cap was textured with
// (bridge #80) the share of an atlas rect's texels (8x8 samples at texel centres) that pass the band's alpha test; 1 when the atlas
// pixels are not known (nothing to judge by)
static float AtlasOpaqueFraction(float u0, float v0, float u1, float v1) {
	const AtlasRegen &ag = g_atlasRegen[g_atlasRegenIdx];
	if (ag.bgra.empty() || !g_atlasW || !g_atlasH) return 1.0f;
	int x0 = (int)std::floor(std::min(u0, u1) * g_atlasW + 0.5f), x1 = (int)std::floor(std::max(u0, u1) * g_atlasW - 0.5f);
	int y0 = (int)std::floor(std::min(v0, v1) * g_atlasH + 0.5f), y1 = (int)std::floor(std::max(v0, v1) * g_atlasH - 0.5f);
	if (x1 < x0) x1 = x0;
	if (y1 < y0) y1 = y0;
	int ok = 0, all = 0;
	for (int j = 0; j < 8; ++j)
		for (int i = 0; i < 8; ++i) {
			const int x = x0 + (x1 - x0) * i / 7, y = y0 + (y1 - y0) * j / 7;
			++all;
			if (x < 0 || y < 0 || x >= ag.w || y >= ag.h) continue;
			if (ag.bgra[((size_t)y * ag.w + x) * 4 + 3] >= 128) ++ok;
		}
	return (float)ok / all;
}
static bool CutCaps(const RenVertex *v, uint32_t n, int sx, int sy, int sz, std::vector<RenVertex> &out) {
	if (!g_capsOn || DigRenderMode() != 2) return false;
	auto world = CollisionFieldHarvestWorld();
	if (!world) return false;
	const auto t0 = std::chrono::steady_clock::now();
	static const int kStep[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};  // Minecraft Direction ordinals
	const int ox = sx * 16, oy = sy * 16, oz = sz * 16;
	std::vector<std::array<int, 3>> dug;
	for (int y = 0; y < 16; ++y)
		for (int z = 0; z < 16; ++z)
			for (int x = 0; x < 16; ++x)
				if (DigIsDugMc(ox + x, oy + y, oz + z)) dug.push_back({ox + x, oy + y, oz + z});
	if (dug.empty()) return false;
	for (const auto &dc : dug)
		for (int dir = 0; dir < 6; ++dir) {
			const long long k = ((long long)(dc[0] & 0x1FFFFF) << 42) | ((long long)(dc[1] & 0x1FFFFF) << 21) | (long long)(dc[2] & 0x1FFFFF);
			g_capEmit.erase({k, dir}), g_capRect.erase({k, dir});
		}
	// The face's texture coordinates as the mod lays them (DigWallQuads.emit): (fu, fv) in 0..1 over the face of D toward N, an atlas
	// rect per kind (side / top for a floor / bottom for a ceiling). A rect is read back from any of the mod's wall quads.
	auto faceUv = [](int dir, const int D[3], const double p[3], double &fu, double &fv) {
		const int axis = dir >> 1 == 0 ? 1 : dir >> 1 == 1 ? 2 : 0;
		if (axis == 1) fu = p[0] - D[0], fv = p[2] - D[2];
		else {
			const int h = axis == 0 ? 2 : 0;
			fu = p[h] - D[h], fv = 1.0 - (p[1] - D[1]);
		}
	};
	struct Rect {
		float u0, v0, u1, v1;
		uint32_t light;
		float span;  // (bridge #80) min(fu, fv) extent of the mod's quad it was read from: 1 = a whole face
	};
	auto rectKind = [](int dir) { return dir == 1 ? 2 : dir == 0 ? 1 : 0; };  // 0 side, 1 floor (N below), 2 ceiling (N above)
	std::map<std::pair<long long, int>, Rect> faceRect;  // (D, dir) -> rect
	std::map<std::pair<long long, int>, Rect> cellRect;  // (D, kind) -> rect
	std::map<int, Rect> anyRect;                          // kind -> rect
	// (bridge #80) keep the rect read from the WIDEST quad (a whole-face quad reads the rect exactly; a sliver of the mod's stepped
	// wall is extrapolated up to 8x), not the last one seen
	auto keepWider = [](auto &m, const auto &k, const Rect &r) {
		auto it = m.find(k);
		if (it == m.end() || r.span > it->second.span || !g_capRectPick) m[k] = r;
	};
	std::map<std::pair<long long, int>, std::vector<uint32_t>> modQuads;  // (D, dir) -> first vertex of each of the mod's wall quads
	auto cellKey = [](int x, int y, int z) { return ((long long)(x & 0x1FFFFF) << 42) | ((long long)(y & 0x1FFFFF) << 21) | (long long)(z & 0x1FFFFF); };
	std::vector<char> isWall(n, 0);
	static const int kN[7][3] = {{0, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
	for (uint32_t i = 0; i + 6 <= n; i += 6) {
		const int d = (v[i].flags >> 4) & 7;
		if (d < 1 || d > 6 || (v[i].flags & 3) != 1 || v[i].color != 0xFFFFFFFFu) continue;
		bool quad = true;
		for (uint32_t k = 1; quad && k < 6; ++k) quad = (int)((v[i + k].flags >> 4) & 7) == d && v[i + k].color == 0xFFFFFFFFu;
		quad = quad && v[i].x == v[i + 3].x && v[i].y == v[i + 3].y && v[i].z == v[i + 3].z && v[i + 2].x == v[i + 4].x && v[i + 2].y == v[i + 4].y && v[i + 2].z == v[i + 4].z;
		if (!quad) continue;
		double c[3] = {0, 0, 0};
		for (uint32_t k = 0; k < 6; ++k) c[0] += v[i + k].x, c[1] += v[i + k].y, c[2] += v[i + k].z;
		c[0] = ox + c[0] / 6.0, c[1] = oy + c[1] / 6.0, c[2] = oz + c[2] / 6.0;
		const int axis = kN[d][0] ? 0 : kN[d][1] ? 1 : 2;
		if (std::fabs(c[axis] - std::round(c[axis])) > 0.01) continue;
		int D[3], N[3];
		for (int a = 0; a < 3; ++a) D[a] = (int)std::floor(c[a] + kN[d][a] * 0.5), N[a] = (int)std::floor(c[a] - kN[d][a] * 0.5);
		if (!DigIsDugMc(D[0], D[1], D[2]) || DigIsDugMc(N[0], N[1], N[2]) || TerrainIsSolid(N[0], N[1], N[2])) continue;
		const int dir = (d - 1) ^ 1;  // from D toward N
		// the rect: u and v are affine in (fu, fv); two corners with different fu (fv) give the slope
		double fu[4], fv[4];
		const uint32_t cs[4] = {i, i + 1, i + 2, i + 5};
		for (int k = 0; k < 4; ++k) {
			const double p[3] = {ox + v[cs[k]].x, oy + v[cs[k]].y, oz + v[cs[k]].z};
			faceUv(dir, D, p, fu[k], fv[k]);
		}
		int ua = 0, ub = 0, va = 0, vb = 0;
		for (int a = 0; a < 4; ++a)
			for (int b = 0; b < 4; ++b) {
				if (std::fabs(fu[a] - fu[b]) > std::fabs(fu[ua] - fu[ub])) ua = a, ub = b;
				if (std::fabs(fv[a] - fv[b]) > std::fabs(fv[va] - fv[vb])) va = a, vb = b;
			}
		if (std::fabs(fu[ua] - fu[ub]) < 0.05 || std::fabs(fv[va] - fv[vb]) < 0.05) continue;  // too thin to read a rect from: left to the mod
		const double su = (v[cs[ua]].u - v[cs[ub]].u) / (fu[ua] - fu[ub]), sv = (v[cs[va]].v - v[cs[vb]].v) / (fv[va] - fv[vb]);
		Rect r;
		r.u0 = (float)(v[cs[ua]].u - fu[ua] * su), r.u1 = (float)(r.u0 + su);
		r.v0 = (float)(v[cs[va]].v - fv[va] * sv), r.v1 = (float)(r.v0 + sv);
		r.light = v[i].light;
		r.span = (float)std::min(std::fabs(fu[ua] - fu[ub]), std::fabs(fv[va] - fv[vb]));
		const long long dk = cellKey(D[0], D[1], D[2]);
		keepWider(faceRect, std::make_pair(dk, dir), r);
		keepWider(cellRect, std::make_pair(dk, rectKind(dir)), r);
		keepWider(anyRect, rectKind(dir), r);
		modQuads[{dk, dir}].push_back(i);
		for (uint32_t k = 0; k < 6; ++k) isWall[i + k] = 1;
	}
	// the caps
	std::vector<RenVertex> caps;
	std::vector<char> keepMod(n, 0);
	long faces = 0, pieces = 0;
	for (const auto &dc : dug) {
		const int D[3] = {dc[0], dc[1], dc[2]};
		const long long dk = cellKey(D[0], D[1], D[2]);
		if (TerrainIsSolid(D[0], D[1], D[2])) {  // a revealed block fills it: its own faces show (the mod's walls, if any, stay)
			for (int dir = 0; dir < 6; ++dir) {
				auto mq = modQuads.find({dk, dir});
				if (mq != modQuads.end())
					for (uint32_t i0 : mq->second) keepMod[i0] = 1;
			}
			continue;
		}
		for (int dir = 0; dir < 6; ++dir) {
			const int N[3] = {D[0] + kStep[dir][0], D[1] + kStep[dir][1], D[2] + kStep[dir][2]};
			if (DigIsDugMc(N[0], N[1], N[2]) || TerrainIsSolid(N[0], N[1], N[2])) continue;
			const int axis = dir >> 1 == 0 ? 1 : dir >> 1 == 1 ? 2 : 0;
			const double plane = D[axis] + (kStep[dir][axis] > 0 ? 1.0 : 0.0);
			const int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
			colharvest::World::Poly poly;
			static const double kSq[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
			for (const auto &q : kSq) {
				double m[3];
				m[axis] = plane, m[a1] = D[a1] + q[0], m[a2] = D[a2] + q[1];
				const Vector s = McToSrc(m[0], m[1], m[2]);
				poly.push_back({s.x, s.y, s.z});
			}
			const Vector into = McToSrc(N[0] + 0.5, N[1] + 0.5, N[2] + 0.5) - McToSrc(D[0] + 0.5, D[1] + 0.5, D[2] + 0.5);
			const double in3[3] = {into.x / kUnitsPerBlock, into.y / kUnitsPerBlock, into.z / kUnitsPerBlock};
			std::vector<colharvest::World::Poly> parts;
			world->SolidParts(poly, in3, parts);
			auto mq = modQuads.find({dk, dir});
			if (parts.empty()) {  // nothing of the BSP's here (a displacement, clip): the mod's wall stays as it was
				g_capEmit[{dk, dir}] = -1;
				if (mq != modQuads.end())
					for (uint32_t i0 : mq->second) keepMod[i0] = 1, ++g_capsModKept;
				continue;
			}
			// (bridge #80) the candidates in order of locality: this face's own wall quad, the cell's (same kind, any kind), the
			// section's (same kind, any). The first that reads the rect from a wide quad AND is opaque in the atlas wins; else the
			// first opaque one; else the first. The band material is alpha-tested: a rect on transparent texels draws nothing.
			std::vector<std::pair<const Rect *, char>> cand;
			{
				auto fr = faceRect.find({dk, dir});
				if (fr != faceRect.end()) cand.push_back({&fr->second, 'f'});
				auto cr = cellRect.find({dk, rectKind(dir)});
				if (cr != cellRect.end()) cand.push_back({&cr->second, 'c'});
				for (int k = 0; k < 3; ++k) {
					auto ck = cellRect.find({dk, k});
					if (k != rectKind(dir) && ck != cellRect.end()) cand.push_back({&ck->second, 'c'});
				}
				auto ar = anyRect.find(rectKind(dir));
				if (ar != anyRect.end()) cand.push_back({&ar->second, 's'});
				for (auto &a : anyRect)
					if (a.first != rectKind(dir)) cand.push_back({&a.second, 's'});
			}
			const Rect *r = nullptr;
			char src = '-';
			float opq = -1;
			for (int pass = g_capRectPick ? 0 : 2; pass < 3 && !r; ++pass)
				for (auto &c : cand) {
					const float o = AtlasOpaqueFraction(c.first->u0, c.first->v0, c.first->u1, c.first->v1);
					if (pass == 0 && !(c.first->span >= 0.49f && o >= 0.999f)) continue;
					if (pass == 1 && !(o >= 0.999f)) continue;
					r = c.first, src = c.second, opq = o;
					break;
				}
			if (r) g_capRect[{dk, dir}] = {r->u0, r->v0, r->u1, r->v1, r->span, opq, src};
			if (!r) {  // no wall of the mod's anywhere in this section to take a texture from
				++g_capsNoTex;
				if (mq != modQuads.end())
					for (uint32_t i0 : mq->second) keepMod[i0] = 1;
				continue;
			}
			++faces;
			g_capEmit[{dk, dir}] = (int)parts.size();
			const int look = dir ^ 1;  // the face looks into D
			const uint32_t flags = 1u | ((uint32_t)(look + 1) << 4);
			for (const auto &pp : parts) {
				std::vector<std::array<double, 3>> mc;
				for (const auto &q : pp) {
					double x, y, z;
					SrcToMcD(Vector((float)q[0], (float)q[1], (float)q[2]), x, y, z);
					double m[3] = {x, y, z};
					m[axis] = plane;  // exactly on the face's plane
					for (int a = 0; a < 3; ++a)
						if (a != axis) m[a] = std::min((double)D[a] + 1.0, std::max((double)D[a], m[a]));
					mc.push_back({m[0], m[1], m[2]});
				}
				if (mc.size() < 3) continue;
				// counter-clockwise seen from the front (Minecraft's winding; OnSection reverses it for Source)
				double nrm[3] = {0, 0, 0};
				for (size_t k = 1; k + 1 < mc.size(); ++k) {
					const double u[3] = {mc[k][0] - mc[0][0], mc[k][1] - mc[0][1], mc[k][2] - mc[0][2]}, w[3] = {mc[k + 1][0] - mc[0][0], mc[k + 1][1] - mc[0][1], mc[k + 1][2] - mc[0][2]};
					nrm[0] += u[1] * w[2] - u[2] * w[1], nrm[1] += u[2] * w[0] - u[0] * w[2], nrm[2] += u[0] * w[1] - u[1] * w[0];
				}
				if (nrm[0] * kN[look + 1][0] + nrm[1] * kN[look + 1][1] + nrm[2] * kN[look + 1][2] < 0) std::reverse(mc.begin(), mc.end());
				auto vert = [&](const std::array<double, 3> &m) {
					RenVertex rv{};
					rv.x = (float)(m[0] - ox), rv.y = (float)(m[1] - oy), rv.z = (float)(m[2] - oz);
					double fu, fv;
					const double p[3] = {m[0], m[1], m[2]};
					faceUv(dir, D, p, fu, fv);
					rv.u = (float)(r->u0 + fu * (r->u1 - r->u0)), rv.v = (float)(r->v0 + fv * (r->v1 - r->v0));
					rv.color = 0xFFFFFFFFu, rv.light = r->light, rv.flags = flags;
					return rv;
				};
				// a fan of quads (0 1 2 0 2 3 per quad, the last one doubled for a triangle): the band treats them as the mod's quads
				for (size_t k = 1; k + 1 < mc.size(); k += 2) {
					const size_t a = 0, b = k, c = k + 1, d = std::min(k + 2, mc.size() - 1);
					for (size_t idx : {a, b, c, a, c, d}) caps.push_back(vert(mc[idx]));
				}
				++pieces;
			}
		}
	}
	if (faces == 0 && modQuads.empty()) return false;
	out.clear();
	out.reserve(n + caps.size());
	long dropped = 0;
	for (uint32_t i = 0; i < n;) {
		if (isWall[i] && !keepMod[i]) {
			i += 6, ++dropped;
			continue;
		}
		if (isWall[i]) {
			out.insert(out.end(), v + i, v + i + 6);
			i += 6;
			continue;
		}
		out.push_back(v[i]);
		++i;
	}
	out.insert(out.end(), caps.begin(), caps.end());
	g_capsFaces += faces, g_capsPieces += pieces, g_capsModDropped += dropped, ++g_capsSections;
	g_capSections.insert(SectionKey(sx, sy, sz));
	g_capsMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	return true;
}

static void OnSection(const uint8_t *p, uint32_t bytes) {
	auto *hdr = (const RenSection *)p;
	auto key = std::make_tuple(hdr->sx, hdr->sy, hdr->sz);
	{
		// Minecraft sends a section again whenever it recompiles it (walking near it, light updates); an identical one
		// was rebuilt and relit on the main thread every time: the hitches walking around placed blocks (lead, 2026-10-02)
		uint64_t h = 1469598103934665603ull;
		for (uint32_t i = 0; i < bytes; ++i) h = (h ^ p[i]) * 1099511628211ull;
		auto hit = g_sectionHash.find(key);
		if (!g_forceSectionBuild && hit != g_sectionHash.end() && hit->second == h && g_sections.count(key)) {
			++g_sectionSkips;
			return;
		}
		g_sectionHash[key] = h;
	}
	// (bridge #27) the old mesh stays until the new one is built: a band section whose lighting is not known yet keeps it
	auto dropOld = [&] {
		auto it = g_sections.find(key);
		if (it != g_sections.end()) {
			DestroyOnRenderThread(it->second);
			g_sections.erase(it);
		}
	};
	uint32_t n = hdr->vertexCount;
	if (n == 0 || bytes < sizeof(RenSection) + (uint64_t)n * sizeof(RenVertex)) {
		dropOld();
		g_bandRaw.erase(key);  // (the last block of a chamber section was broken)
		return;
	}
	auto *v = (const RenVertex *)(p + sizeof(RenSection));
	const double ox0 = hdr->sx * 16.0, oy0 = hdr->sy * 16.0, oz0 = hdr->sz * 16.0;
	const bool band = BandAt(McToSrc(ox0 + 8, oy0 + 8, oz0 + 8));
	// above vanilla's build limit only placed blocks exist: kept whatever the mapping (a vertical shift moves the box)
	if (band || hdr->sy >= 20) {
		if (g_bandRaw[key].data() != p) g_bandRaw[key].assign(p, p + sizeof(RenSection) + (size_t)n * sizeof(RenVertex));
	} else {
		g_bandRaw.erase(key);
	}
	if (!g_terrainMat) return dropOld();
	// the region's sections are Minecraft's own frame (the composite); their meshes are built only for the debug switch
	if (!band && !TerrainMeshesDebug()) return dropOld();
	std::vector<RenVertex> culled;  // (bridge #73) no Minecraft face on a visible Portal 2 surface
	if (band && DigRenderMode() == 2 && CullCoplanarFaces(v, n, ox0, oy0, oz0, culled)) {
		if (culled.empty()) return dropOld();
		v = culled.data();
		n = (uint32_t)culled.size();
	}
	std::vector<RenVertex> capped;  // (bridge #76) the hole walls clipped exactly to Portal 2's solid
	if (band && CutCaps(v, n, hdr->sx, hdr->sy, hdr->sz, capped)) {
		if (capped.empty()) return dropOld();
		v = capped.data();
		n = (uint32_t)capped.size();
	}
	auto sec = std::make_shared<Section>();
	sec->band = band;
	static const float kShade[7] = {1.0f, 0.5f, 1.0f, 0.8f, 0.8f, 0.6f, 0.6f};  // none, down, up, N, S, W, E
	const float vScale = g_atlasTexH ? (float)g_atlasH / g_atlasTexH : 1.0f;
	const double ox = hdr->sx * 16.0, oy = hdr->sy * 16.0, oz = hdr->sz * 16.0;
	sec->centre = McToSrc(ox + 8, oy + 8, oz + 8);
	sec->verts.resize(n);
	// bridge #17, band lighting: SkyCraft WorldRender.cpp's PSMain/Lighting(), evaluated per vertex on the CPU (the
	// material system has no shader of ours), with Portal 2 as the host light source:
	//   amb(n) + lights      = ComputeLighting(point, n): Portal 2's ambient cube by the face normal + its local lights
	//                          (SkyCraft: Ambient(n) + the 16-light loop), floored by the world light at the point
	//                          (GetLightForPoint x pcb_band_light k; the ambient cube alone left block tops near black)
	//   lit                  = that x lerp(0.3, 1, Curve(sky)) (Minecraft roofs darken), scaled like the test blocks
	//   colour               = texture x vertex colour (tint x AO) x max(lit, Curve(block) x (1, 0.85, 0.65))
	// No Minecraft face shade (SkyCraft leaves it out: the normal does that). Sample point: the vertex, 2 u out along the
	// normal and 10% towards its triangle's centre (corners touch Portal 2 walls).
	static const Vector kDirN[7] = {{0, 0, 1}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, -1, 0}, {-1, 0, 0}, {1, 0, 0}};
	std::unordered_map<long long, Vector> lightCache;
	auto bandLight = [&](uint32_t i) -> Vector {
		int dir = (v[i].flags >> 4) & 7;
		if (dir > 6) dir = 0;
		const Vector &nn = kDirN[dir];
		const uint32_t t0 = i - i % 3;
		const double cx = (v[t0].x + v[t0 + 1].x + v[t0 + 2].x) / 3.0, cy = (v[t0].y + v[t0 + 1].y + v[t0 + 2].y) / 3.0, cz = (v[t0].z + v[t0 + 1].z + v[t0 + 2].z) / 3.0;
		const double px = v[i].x + (cx - v[i].x) * 0.1, py = v[i].y + (cy - v[i].y) * 0.1, pz = v[i].z + (cz - v[i].z) * 0.1;
		const Vector c = McToSrc(ox0 + px, oy0 + py, oz0 + pz) + nn * 2.0f;
		const long long key = ((long long)std::lround(c.x) & 0xFFFFF) << 40 | ((long long)std::lround(c.y) & 0xFFFFF) << 20 | ((long long)std::lround(c.z) & 0xFFFFF);
		auto it = lightCache.find(key * 8 + dir);
		if (it != lightCache.end()) return it->second;
		Vector l = engine.LightAt(c, nn);
		const Vector w = engine.LightAt(c) * g_bandWorldK;
		l.x = std::fmax(l.x, w.x), l.y = std::fmax(l.y, w.y), l.z = std::fmax(l.z, w.z);
		if (dir == 0) l = l * 0.8f;  // no normal (plants): SkyCraft's averaged ambient, roughly
		if (++g_bandLightCalls <= 8)
			console->Print("[bridge] band light: dir %d at (%.1f %.1f %.1f) -> linear (%.3f %.3f %.3f), world (%.3f %.3f %.3f)\n", dir, c.x, c.y, c.z, l.x, l.y, l.z, w.x, w.y, w.z);
		const Vector out(RenderLightToScale(l.x), RenderLightToScale(l.y), RenderLightToScale(l.z));
		lightCache.emplace(key * 8 + dir, out);
		return out;
	};
	// bridge #18: Portal 2's model lighting state per vertex (one batched query for the section's distinct sample points),
	// evaluated at the vertex like Portal 2's vertex-lit shader, times the contact AO from Portal 2's geometry
	std::vector<Vector> v2light;
	std::vector<float> v2ao;
	std::vector<char> v2cut;  // (bridge #79) lit from the cut's mouths
	const auto tBuild0 = std::chrono::steady_clock::now();
	if (band && g_bandLit == 2) {
		std::unordered_map<long long, int> idx;
		std::vector<Vector> pts;
		std::vector<long long> ptKeys;
		std::vector<int> vidx(n);
		for (uint32_t i = 0; i < n; ++i) {
			int dir = (v[i].flags >> 4) & 7;
			if (dir > 6) dir = 0;
			const Vector &nn = kDirN[dir];
			const uint32_t t0 = i - i % 3;
			const Vector pos = McToSrc(ox0 + v[i].x, oy0 + v[i].y, oz0 + v[i].z);
			const Vector ctr = McToSrc(ox0 + (v[t0].x + v[t0 + 1].x + v[t0 + 2].x) / 3.0, oy0 + (v[t0].y + v[t0 + 1].y + v[t0 + 2].y) / 3.0, oz0 + (v[t0].z + v[t0 + 1].z + v[t0 + 2].z) / 3.0);
			// where a model lit like this face would stand: half a block out, pulled in from the corner (corners touch walls)
			// (no pull towards the face's centre: blocks sharing a corner then sample the same point, no seam between them;
			// a corner inside a Portal 2 wall moves below)
			Vector sp = pos + nn * (dir ? 16.0f : 8.0f);
			if (engine.GetPointContentsWorld && (engine.GetPointContentsWorld(engine.traceServer->ThisPtr(), sp, MASK_SOLID) & CONTENTS_SOLID)) {
				const Vector alt[3] = {pos + nn * 16.0f + (ctr - pos) * 0.3f, ctr + nn * 16.0f, pos + nn * 30.0f};
				for (auto &a : alt)
					if (!(engine.GetPointContentsWorld(engine.traceServer->ThisPtr(), a, MASK_SOLID) & CONTENTS_SOLID)) {
						sp = a;
						break;
					}
			}
			const long long key = (((long long)std::lround(sp.x * 0.5f) & 0xFFFFF) << 40 | ((long long)std::lround(sp.y * 0.5f) & 0xFFFFF) << 20 | ((long long)std::lround(sp.z * 0.5f) & 0xFFFFF)) * 8 + dir;
			auto it = idx.find(key);
			if (it == idx.end()) it = idx.emplace(key, (int)pts.size()).first, pts.push_back(sp), ptKeys.push_back(key);
			vidx[i] = it->second;
		}
		std::vector<P2State> states(pts.size());
		{
			// (lead: Portal 2's static lighting at a sample point never changes: query only points not seen before)
			std::vector<Vector> miss;
			std::vector<size_t> missAt;
			for (size_t k = 0; k < pts.size(); ++k) {
				auto c = g_bandStateCache.find(ptKeys[k]);
				if (c != g_bandStateCache.end()) states[k] = c->second;
				else miss.push_back(pts[k]), missAt.push_back(k);
			}
			if (!miss.empty()) {
				std::vector<P2State> got;
				BlockLightsSuspend();  // (Portal 2 folds dynamic lights into the ambient cube; the blocks have Minecraft's block light)
				BandLightQuery(miss, got);
				BlockLightsResume();
				for (size_t j = 0; j < missAt.size() && j < got.size(); ++j) {
					states[missAt[j]] = got[j];
					float sum = 0;
					for (int a = 0; a < 6; ++a) sum += got[j].cube[a][0] + got[j].cube[a][1] + got[j].cube[a][2];
					if (g_bandStateCache.size() > 250000) g_bandStateCache.clear();  // (bridge #53) bounded (a miss re-queries the engine)
					if (sum > 0 || got[j].n > 0) g_bandStateCache[ptKeys[missAt[j]]] = got[j];  // (empty ones are retried)
				}
			}
		}
		// an empty state (no cube, no lights: a query the engine could not place) falls back to ComputeLighting per axis
		static const Vector kAx[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
		auto stateSum = [](const P2State &st) {
			float sum = 0;
			for (int a = 0; a < 6; ++a) sum += st.cube[a][0] + st.cube[a][1] + st.cube[a][2];
			return sum;
		};
		int emptyHere = 0;
		P2State okMean{};
		int okN = 0;
		for (size_t k = 0; k < states.size(); ++k) {
			P2State &st = states[k];
			if (stateSum(st) > 0 || st.n > 0) {
				++g_bandStateOk;
				for (int a = 0; a < 6; ++a)
					for (int c = 0; c < 3; ++c) okMean.cube[a][c] += st.cube[a][c];
				++okN;
				continue;
			}
			// (bridge #27) a sample point inside Portal 2's solid (a face against the floor or a wall) is never lit: no relight for it
			// (r27_c: 4 such states relit a section 10 times); it takes the section's mean below
			const bool inSolid = engine.GetPointContentsWorld && (engine.GetPointContentsWorld(engine.traceServer->ThisPtr(), pts[k], MASK_SOLID) & CONTENTS_SOLID);
			++g_bandStateEmpty;
			if (!inSolid) ++emptyHere;
			for (int a = 0; a < 6; ++a) {
				const Vector l = engine.LightAt(pts[k], kAx[a]);
				st.cube[a][0] = l.x, st.cube[a][1] = l.y, st.cube[a][2] = l.z;
			}
		}
		// (bridge #23, the user's "a block placed on its own is black until more are placed") a state still black after the
		// fallback: the section's other states, or the last good state seen anywhere, meanwhile; the section is lit again
		// a few frames later (BandRelightFrame), up to 10 times, instead of staying black until its next rebuild
		if (okN > 0) {
			for (int a = 0; a < 6; ++a)
				for (int c = 0; c < 3; ++c) okMean.cube[a][c] /= (float)okN;
			g_bandLastGood = okMean, g_bandHaveGood = true;
		}
		int stillDark = 0, unlit = 0;
		for (size_t k = 0; k < states.size(); ++k) {
			P2State &st = states[k];
			if (stateSum(st) > 0 || st.n > 0) continue;
			++stillDark;
			if (okN > 0) st = okMean;
			else if (g_bandHaveGood) st = g_bandLastGood;
			else ++unlit;
		}
		if (emptyHere > 0) BandRelightQueue(std::make_tuple(hdr->sx, hdr->sy, hdr->sz), stillDark);
		// (bridge #27) no light known anywhere yet (the session's first placed block, whose first queries came back empty): it
		// would be baked black (r27_b, bgra 0 0 0 for two builds). Keep what was drawn and build it on the relight instead.
		if (unlit > 0 && unlit == (int)states.size() && g_bandRelight.count(key)) {
			++g_bandDeferred;
			console->Print("[bridge] band: section %d %d %d: no Portal 2 light known yet, not drawn black: built on its relight\n", hdr->sx, hdr->sy, hdr->sz);
			return;
		}
		v2light.resize(n);
		v2ao.assign(n, 1.0f);
		v2cut.assign(n, 0);
		// (bridge #79) a sample point inside Portal 2's solid in front of a dug cell: the cut's light (cutlight.cpp) instead
		std::vector<char> ptSolid(pts.size(), 0);
		if (engine.GetPointContentsWorld)
			for (size_t k = 0; k < pts.size(); ++k) ptSolid[k] = (engine.GetPointContentsWorld(engine.traceServer->ThisPtr(), pts[k], MASK_SOLID) & CONTENTS_SOLID) != 0;
		for (uint32_t i = 0; i < n; ++i) {
			int dir = (v[i].flags >> 4) & 7;
			if (dir > 6) dir = 0;
			const Vector &nn = kDirN[dir];
			const Vector pos = McToSrc(ox0 + v[i].x, oy0 + v[i].y, oz0 + v[i].z);
			// (bridge #80) or the face looks into a dug air cell of the cut's field (the cell in front of its triangle's centre): along the
			// mouth's edge a cap's sample point lies out in the room's air, and it took the room's light with Portal 2's contact AO
			// tracing into the hole's walls (0.44 of the wall's light right at the edge on sp_a1_intro2)
			bool intoCut = false;
			if (dir && !ptSolid[vidx[i]] && g_cutFaceRule) {
				static const int kMcN[7][3] = {{0, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
				const uint32_t t0 = i - i % 3;
				const double cx = (v[t0].x + v[t0 + 1].x + v[t0 + 2].x) / 3.0 + kMcN[dir][0] * 0.5, cy = (v[t0].y + v[t0 + 1].y + v[t0 + 2].y) / 3.0 + kMcN[dir][1] * 0.5,
				             cz = (v[t0].z + v[t0 + 1].z + v[t0 + 2].z) / 3.0 + kMcN[dir][2] * 0.5;
				intoCut = CutLightCellDist((int)std::floor(ox0 + cx), (int)std::floor(oy0 + cy), (int)std::floor(oz0 + cz)) >= 0;
			}
			if (dir && (ptSolid[vidx[i]] || intoCut)) {
				Vector tl;
				float tmean = 0;
				if (CutLightVertex(ox0 + v[i].x, oy0 + v[i].y, oz0 + v[i].z, dir - 1, (v[i].light >> 8) & 0xF, tl, &tmean)) {
					tl.x = std::fmax(tl.x, tmean * g_bandFill), tl.y = std::fmax(tl.y, tmean * g_bandFill), tl.z = std::fmax(tl.z, tmean * g_bandFill);
					v2light[i] = tl * g_bandK;
					v2cut[i] = 1;  // no contact AO (its traces start in the host's solid) and no roof term (the cut's falloff and Minecraft's sky are in it)
					if (++g_bandCutLit <= 40 || g_bandCutLit % 400 == 0) {
						const Vector old = BandLightEval(states[vidx[i]], pos + nn * 1.0f, nn, true);
						console->Print("[bridge] band light (cut): dir %d at (%.1f %.1f %.1f) sky %d block %d -> (%.4f %.4f %.4f), mean %.4f; the in-solid sample gave (%.4f %.4f %.4f)\n", dir, pos.x, pos.y, pos.z,
						               (v[i].light >> 8) & 0xF, v[i].light & 0xF, tl.x, tl.y, tl.z, tmean, old.x, old.y, old.z);
					}
					continue;
				}
			}
			const P2State &st = states[vidx[i]];
			Vector L = BandLightEval(st, pos + nn * 1.0f, nn, dir != 0);
			float mean[3] = {0, 0, 0};
			for (int a = 0; a < 6; ++a)
				for (int c = 0; c < 3; ++c) mean[c] += st.cube[a][c] / 6.0f;
			L.x = std::fmax(L.x, mean[0] * g_bandFill), L.y = std::fmax(L.y, mean[1] * g_bandFill), L.z = std::fmax(L.z, mean[2] * g_bandFill);
			v2light[i] = L * g_bandK;
			if (dir) v2ao[i] = BandContactAO(pos, nn, dir);
			if (++g_bandLightCalls <= 6 || g_bandLightCalls % 500 == 0)
				console->Print("[bridge] band light (state): dir %d at (%.1f %.1f %.1f) -> (%.3f %.3f %.3f), %d local lights, ao %.2f\n", dir, pos.x, pos.y, pos.z, L.x, L.y, L.z, st.n, v2ao[i]);
		}
	}
	Vector quadCentre;  // (lead) the centre of the block quad vertex i belongs to, for the edge grow below
	bool quadOk = false;
	for (uint32_t i = 0; i < n; ++i) {
		const RenVertex &rv = v[i];
		GpuVertex &gv = sec->verts[i];
		Vector pos = McToSrc(ox + rv.x, oy + rv.y, oz + rv.z);
		if (band) {
			// faces 0.06 u back along their normal: a Portal 2 portal fitted flush on a block face (portalgun.cpp) is drawn in
			// the opaque pass, these in the translucent one at the same depth, and covered it (b17_place6 c_portal)
			static const Vector kN[7] = {{0, 0, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, -1, 0}, {-1, 0, 0}, {1, 0, 0}};
			const int d = (rv.flags >> 4) & 7;
			// (lead, 2026-10-03) pushed back alone, two faces meeting at an inside corner (a dug tunnel's wall and floor) part
			// by 0.06 u along the edge and the clear colour shows through as a white hairline. So each face also grows 0.06 u
			// past its own edges, in its plane, and corners close again. The mod sends a block quad as 6 vertices (0 1 2 0 2 3,
			// WorldExporter / DigWalls); a group that doesn't look like one (fluids, plants) is only pushed back.
			if (i % 6 == 0) {
				quadOk = i + 6 <= n;
				for (uint32_t k = 1; quadOk && k < 6; ++k) quadOk = (int)((v[i + k].flags >> 4) & 7) == d;
				quadOk = quadOk && d >= 1 && d <= 6 && v[i].x == v[i + 3].x && v[i].y == v[i + 3].y && v[i].z == v[i + 3].z && v[i + 2].x == v[i + 4].x &&
				         v[i + 2].y == v[i + 4].y && v[i + 2].z == v[i + 4].z;
				if (quadOk) {
					quadCentre = Vector(0, 0, 0);
					for (uint32_t k = 0; k < 6; ++k) quadCentre += McToSrc(ox + v[i + k].x, oy + v[i + k].y, oz + v[i + k].z);
					quadCentre = quadCentre * (1.0f / 6.0f);  // (2 v0 + v1 + 2 v2 + v3) / 6 = the centre of a parallelogram
				}
			}
			if (d >= 1 && d <= 6) {
				if (quadOk)
					for (int a = 0; a < 3; ++a) {
						if ((&kN[d].x)[a] != 0) continue;  // the normal axis: pushed back below
						const float off = (&pos.x)[a] - (&quadCentre.x)[a];
						if (std::fabs(off) > 0.01f) (&pos.x)[a] += off > 0 ? 0.06f : -0.06f;
					}
				pos = pos - kN[d] * 0.06f;
			}
		}
		gv.x = pos.x, gv.y = pos.y, gv.z = pos.z;
		gv.u = rv.u, gv.v = rv.v * vScale;
		float block = (rv.light & 0xF) / 15.0f, sky = ((rv.light >> 8) & 0xF) / 15.0f;
		int dir = (rv.flags >> 4) & 7;
		float shade = kShade[dir < 7 ? dir : 0];
		float skyL = Curve(sky), blockL = Curve(block);
		// daylight neutral white, block light warm (torches); a little ambient everywhere
		float lr = std::fmax(std::fmax(skyL, blockL * 1.0f), 0.05f), lg = std::fmax(std::fmax(skyL, blockL * 0.85f), 0.05f),
		      lb = std::fmax(std::fmax(skyL, blockL * 0.6f), 0.05f);
		if (band && g_bandLit == 2) {
			const Vector hl = v2light[i] * v2ao[i];
			const float roof = v2cut[i] ? 1.0f : 0.3f + 0.7f * skyL;
			lr = std::fmax(hl.x * roof, blockL * g_bandBlockK), lg = std::fmax(hl.y * roof, blockL * 0.85f * g_bandBlockK), lb = std::fmax(hl.z * roof, blockL * 0.65f * g_bandBlockK);
			// past full scale: keep the hue (a per-channel clip turned lit planks orange)
			// the vertex colour is decoded to linear by the shader (pcb_light_ramp, a18_light1): encode the light; Minecraft's
			// tint x AO byte is a display value, decoded with it
			const float m = std::fmax(lr, std::fmax(lg, lb));
			if (m > 1.0f) lr /= m, lg /= m, lb /= m;
			const float ig = 1.0f / g_bandGamma;
			lr = std::pow(std::fmax(lr, 0.0f), ig), lg = std::pow(std::fmax(lg, 0.0f), ig), lb = std::pow(std::fmax(lb, 0.0f), ig);
			shade = 1.0f;
		} else if (band && g_bandLit) {
			const Vector hl = bandLight(i);
			const float roof = 0.3f + 0.7f * skyL;
			lr = std::fmax(hl.x * roof, blockL * 1.0f), lg = std::fmax(hl.y * roof, blockL * 0.85f), lb = std::fmax(hl.z * roof, blockL * 0.65f);
			shade = 1.0f;
		}
		float k = shade * g_exposure;
		if (g_unlit) lr = lg = lb = 1.0f, k = 1.0f;
		auto ch = [&](uint32_t c, float l) { return (uint8_t)std::fmin(255.0f, c * l * k); };
		gv.bgra[0] = ch((rv.color >> 16) & 0xFF, lb);
		gv.bgra[1] = ch((rv.color >> 8) & 0xFF, lg);
		gv.bgra[2] = ch(rv.color & 0xFF, lr);
		gv.bgra[3] = (rv.color >> 24) & 0xFF;
	}
	if (g_meshesBuilt < 3) {
		for (uint32_t i = 0; i < n && i < 4; ++i)
			console->Print("[bridge] terrain: section %d %d %d v%u pos (%.2f %.2f %.2f) uv (%.4f %.4f) colour 0x%08x light 0x%x flags 0x%x -> bgra %u %u %u %u\n", hdr->sx, hdr->sy,
			               hdr->sz, i, v[i].x, v[i].y, v[i].z, v[i].u, v[i].v, v[i].color, v[i].light, v[i].flags, sec->verts[i].bgra[0], sec->verts[i].bgra[1],
			               sec->verts[i].bgra[2], sec->verts[i].bgra[3]);
	}
	if (band) {
		// SkyCraft's OnSection: opaque/cutout triangles first, translucent (water, stained glass) after, drawn blended
		std::vector<GpuVertex> opaque;
		opaque.reserve(n);
		const bool trace = g_capTraceOn && g_capTraceSec == key;
		if (trace) console->Print("[bridge] caps trace: section %d %d %d build, %u vertices; triangles within 1.5 u of the face plane:\n", hdr->sx, hdr->sy, hdr->sz, n);
		for (uint32_t t = 0; t + 2 < n; t += 3) {
			auto &dst = (v[t].flags & 2) ? sec->tverts : opaque;
			if (trace) {  // (bridge #80) pc_dig_caps_trace: what the build really emits on one face of a dug cell
				const GpuVertex *g[3] = {&sec->verts[t], &sec->verts[t + 2], &sec->verts[t + 1]};
				bool on = true;
				for (const GpuVertex *q : g) {
					const float c[3] = {q->x, q->y, q->z};
					on = on && std::fabs(c[g_capTraceAxis] - g_capTracePlane) < 1.5f;
					for (int a2 = 0; a2 < 3 && on; ++a2)
						if (a2 != g_capTraceAxis) on = c[a2] >= g_capTraceLo[a2] - 2 && c[a2] <= g_capTraceHi[a2] + 2;
				}
				if (on) {
					const Vector e1(g[1]->x - g[0]->x, g[1]->y - g[0]->y, g[1]->z - g[0]->z), e2(g[2]->x - g[0]->x, g[2]->y - g[0]->y, g[2]->z - g[0]->z);
					const Vector nr(e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x);
					console->Print("[bridge]   tri %u %s flags 0x%x dir %d: (%.2f %.2f %.2f) (%.2f %.2f %.2f) (%.2f %.2f %.2f) normal (%.1f %.1f %.1f) uv (%.4f %.4f) bgra %u %u %u %u\n", t, (v[t].flags & 2) ? "translucent" : "opaque",
					               v[t].flags, (v[t].flags >> 4) & 7, g[0]->x, g[0]->y, g[0]->z, g[1]->x, g[1]->y, g[1]->z, g[2]->x, g[2]->y, g[2]->z, nr.x, nr.y, nr.z, g[0]->u, g[0]->v, g[0]->bgra[0], g[0]->bgra[1],
					               g[0]->bgra[2], g[0]->bgra[3]);
				}
			}
			// Minecraft's front faces are counter-clockwise, Source's (D3D) clockwise: reversed, or only the back faces show
			// (b17_place1-4: the inside of the block box, its bottom face black)
			dst.push_back(sec->verts[t]);
			dst.push_back(sec->verts[t + 2]);
			dst.push_back(sec->verts[t + 1]);
		}
		sec->verts.swap(opaque);
		if (trace) g_capTraceOn = false;
	}
	g_vertsBuilt += (int)n;
	++g_meshesBuilt;
	if (band) {
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tBuild0).count();
		g_bandBuildMs += ms, g_bandBuildMax = std::max(g_bandBuildMax, ms);
	}
	if (band && ++g_bandBuilt <= 20) console->Print("[bridge] band: section %d %d %d, %u vertices, %d light samples so far\n", hdr->sx, hdr->sy, hdr->sz, n, g_bandLightCalls);
	if (band) {
		SplitAnimated(sec->verts, sec->averts, vScale);
		SplitAnimated(sec->tverts, sec->atverts, vScale);
		sec->animated = !sec->averts.empty() || !sec->atverts.empty();
	}
	dropOld();
	g_sections[key] = sec;
	if (band) {
		SkirtDirtyAround(hdr->sx, hdr->sy, hdr->sz);
		g_bandAnimated = false;
		for (auto &kv : g_sections) g_bandAnimated |= kv.second->band && kv.second->animated;
	}
}

// bridge #18: contact shadows on Portal 2 around the band's blocks. They depend on the solids (sent after the section) and
// on the neighbouring sections' solids, so they are rebuilt after either changes, a few sections per frame.
static std::map<std::tuple<int, int, int>, int> g_skirtDirty;
PCB_GAUGE("terrain.skirtDirty", g_skirtDirty.size());
static int g_skirtQuads = 0;
static void SkirtDirtyAround(int sx, int sy, int sz) {
	for (int d = 0; d < 7; ++d) {
		static const int o[7][3] = {{0, 0, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
		auto key = std::make_tuple(sx + o[d][0], sy + o[d][1], sz + o[d][2]);
		auto it = g_sections.find(key);
		if (it != g_sections.end() && it->second->band) g_skirtDirty[key] = 1;
	}
}
static double g_skirtMs = 0, g_skirtMax = 0;
static void BuildSkirtsImpl(int sx, int sy, int sz, Section &sec);
static void BuildSkirts(int sx, int sy, int sz, Section &sec) {
	const auto t0 = std::chrono::steady_clock::now();
	BuildSkirtsImpl(sx, sy, sz, sec);
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	g_skirtMs += ms, g_skirtMax = std::max(g_skirtMax, ms);
}
static void BuildSkirtsImpl(int sx, int sy, int sz, Section &sec) {
	sec.skirt.clear();
	if (!g_bandSkirts || g_bandLit == 0) return;
	std::vector<SkirtVert> sv;
	for (int y = 0; y < 16; ++y)
		for (int z = 0; z < 16; ++z)
			for (int x = 0; x < 16; ++x) {
				const int bx = sx * 16 + x, by = sy * 16 + y, bz = sz * 16 + z;
				if (TerrainIsSolid(bx, by, bz)) BandSkirts(bx, by, bz, sv);
			}
	sec.skirt.reserve(sv.size());
	for (auto &q : sv) {
		GpuVertex g{};
		g.x = q.p.x, g.y = q.p.y, g.z = q.p.z;
		g.bgra[3] = q.a;
		sec.skirt.push_back(g);
	}
	g_skirtQuads += (int)sv.size() / 4;
}

static double g_lastSolidsT = 0;  // wall clock of the last section record (streaming activity)
double TerrainIdleS() {
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count() - g_lastSolidsT;
}
static void OnSolids(const uint8_t *p, uint32_t bytes) {
	auto *h = (const RenSolids *)p;
	++g_solidsGen;
	{
		using namespace std::chrono;
		g_lastSolidsT = duration<double>(steady_clock::now().time_since_epoch()).count();
	}
	long long key = SectionKey(h->sx, h->sy, h->sz);
	SkirtDirtyAround(h->sx, h->sy, h->sz);  // bridge #18 (after the bits below are in: built in TerrainConsume)
	if (h->count == 0 || bytes < sizeof(RenSolids) + 512) {
		if (g_solids.count(key) && g_capSections.count(key)) g_bandRebuildKeys.insert(std::make_tuple(h->sx, h->sy, h->sz));  // (bridge #76)
		g_solids.erase(key);
		g_fluids.erase(key);
		g_noPortal.erase(key);
		g_airSections[key] = 1;
		return;
	}
	g_airSections.erase(key);
	std::vector<uint64_t> bits(64);
	std::memcpy(bits.data(), p + sizeof(RenSolids), 512);
	{  // (bridge #76) cut caps depend on which cells hold a Minecraft block (sent after the section): rebuild the capped sections around a change
		auto old = g_solids.find(key);
		if ((old == g_solids.end() || old->second != bits) && !g_capSections.empty()) {
			static const int o[7][3] = {{0, 0, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
			for (const auto &d : o)
				if (g_capSections.count(SectionKey(h->sx + d[0], h->sy + d[1], h->sz + d[2]))) g_bandRebuildKeys.insert(std::make_tuple(h->sx + d[0], h->sy + d[1], h->sz + d[2]));
		}
	}
	g_solids[key] = std::move(bits);
	// Portalcraft: water then lava bitsets follow the solids (Minecraft's fluids for Chell)
	if (bytes >= sizeof(RenSolids) + 1536) {
		std::vector<uint64_t> fl(128);
		std::memcpy(fl.data(), p + sizeof(RenSolids) + 512, 1024);
		bool any = false;
		for (uint64_t w : fl) any |= w != 0;
		if (any) g_fluids[key] = std::move(fl);
		else g_fluids.erase(key);
	}
	// then the blocks a portal may not be placed on (leaves, glass, partial blocks)
	if (bytes >= sizeof(RenSolids) + 2048) {
		std::vector<uint64_t> np(64);
		std::memcpy(np.data(), p + sizeof(RenSolids) + 1536, 512);
		bool any = false;
		for (uint64_t w : np) any |= w != 0;
		if (any) g_noPortal[key] = std::move(np);
		else g_noPortal.erase(key);
	} else {
		g_noPortal.erase(key);
	}
}

// A solid block a portal may not be placed on (leaves, glass, slabs, stairs...; Minecraft block coords)
bool TerrainNoPortal(int bx, int by, int bz) {
	auto it = g_noPortal.find(SectionKey(bx >> 4, by >> 4, bz >> 4));
	if (it == g_noPortal.end()) return false;
	int bit = (bx & 15) + 16 * (bz & 15) + 256 * (by & 15);
	return (it->second[bit >> 6] >> (bit & 63)) & 1;
}
int TerrainNoPortalSections() { return (int)g_noPortal.size(); }

// 0 none, 1 water, 2 lava (Minecraft block coords)
int TerrainFluid(int bx, int by, int bz) {
	auto it = g_fluids.find(SectionKey(bx >> 4, by >> 4, bz >> 4));
	if (it == g_fluids.end()) return 0;
	int bit = (bx & 15) + 16 * (bz & 15) + 256 * (by & 15);
	if ((it->second[bit >> 6] >> (bit & 63)) & 1) return 1;
	if ((it->second[64 + (bit >> 6)] >> (bit & 63)) & 1) return 2;
	return 0;
}

// Sections are placed with the region mapping in force when they are built, so nothing is
// consumed until a mapping is set (the ring holds them; Minecraft retries when it is full).
static bool g_mapReady = false;
void TerrainSetMapReady(bool ready) {
	if (ready && !g_mapReady) console->Print("[bridge] terrain: region mapping set, streaming sections\n");
	g_mapReady = ready;
}
bool TerrainMapReady() { return g_mapReady; }

// Drain the render ring for up to budgetMs.
void TerrainConsume(double budgetMs) {
	uint8_t *base = link::Base();
	BandLightFrame();  // bridge #18: diagnostics queued from commands
	BlockLightsFrame(LastMainView().origin);  // bridge #18: Minecraft's emitters as Portal 2 dynamic lights
	AtlasAnimate();
	if (!base || !g_mapReady) return;
	for (int built = 0; !g_skirtDirty.empty() && built < 4; ++built) {
		auto key = g_skirtDirty.begin()->first;
		g_skirtDirty.erase(g_skirtDirty.begin());
		auto it = g_sections.find(key);
		if (it != g_sections.end() && it->second->band) BuildSkirts(std::get<0>(key), std::get<1>(key), std::get<2>(key), *it->second);
	}
	CutLightFrame();  // (bridge #79) the cuts' light field, before the band sections it changed are rebuilt below
	// (bridge #23) relight the sections whose lighting came back empty
	for (auto it = g_bandRelight.begin(); it != g_bandRelight.end();) {
		if (--it->second.frames > 0) {
			++it;
			continue;
		}
		const auto key = it->first;
		const int tries = ++it->second.tries;
		it->second.frames = 1 << 20;  // (OnSection requeues it if still empty)
		auto raw = g_bandRaw.find(key);
		if (raw == g_bandRaw.end()) {
			it = g_bandRelight.erase(it);
			continue;
		}
		const std::vector<uint8_t> copy = raw->second;
		const size_t before = g_bandRelight.size();
		g_forceSectionBuild = true;
		OnSection(copy.data(), (uint32_t)copy.size());
		g_forceSectionBuild = false;
		++g_bandRelights;
		it = g_bandRelight.find(key);
		if (it != g_bandRelight.end() && it->second.frames == (1 << 20)) {
			console->Print("[bridge] band: section %d %d %d relit (try %d): lighting states all found\n", std::get<0>(key), std::get<1>(key), std::get<2>(key), tries);
			it = g_bandRelight.erase(it);
		} else
			it = g_bandRelight.upper_bound(key);
		(void)before;
		break;  // (one section per frame)
	}
	if (g_bandLevelPending && McLevelIsLive()) g_bandLevelPending = false, g_bandRebuild = !g_bandRaw.empty();  // (bridge #61)
	if (g_bandRebuild) {
		g_bandRebuild = false;
		g_bandRebuildKeys.clear();
		auto raw = g_bandRaw;
		g_forceSectionBuild = true;
		for (auto &kv : raw) OnSection(kv.second.data(), (uint32_t)kv.second.size());
		g_forceSectionBuild = false;
	} else if (!g_bandRebuildKeys.empty()) {
		std::vector<std::vector<uint8_t>> raws;
		for (const auto &k : g_bandRebuildKeys) {
			auto it = g_bandRaw.find(k);
			if (it != g_bandRaw.end()) raws.push_back(it->second);
		}
		g_bandRebuildKeys.clear();
		g_forceSectionBuild = true;
		for (auto &r : raws) OnSection(r.data(), (uint32_t)r.size());
		g_forceSectionBuild = false;
	}
	uint8_t *ring = base + kOffRenderRing;
	auto *headp = (uint64_t *)(ring + kRenRingHeadOff);
	auto *tailp = (uint64_t *)(ring + kRenRingTailOff);
	uint8_t *data = ring + kRenRingDataOff;
	auto t0 = std::chrono::steady_clock::now();
	uint64_t head = __atomic_load_n(headp, __ATOMIC_ACQUIRE);
	uint64_t tail = __atomic_load_n(tailp, __ATOMIC_RELAXED);
	while (tail < head) {
		uint64_t pos = tail % kRenRingDataBytes;
		uint32_t type = *(uint32_t *)(data + pos), payload = *(uint32_t *)(data + pos + 4);
		if (type == kRenPad) {
			tail += kRenRingDataBytes - pos;
			continue;
		}
		const uint8_t *p = data + pos + 8;
		++g_msgs;
		if (g_enabled) switch (type) {
			case kRenAtlas: OnAtlas(p, payload); break;
			case kRenSection: OnSection(p, payload); break;
			case kRenClearAll:
				console->Print("[bridge] terrain: Minecraft is resending its world (%zu sections were held)\n", TerrainSectionsKnown());
				TerrainClear();
				BlockLightsClear();
				ClipExitRecheck("Minecraft resent its world");
				break;
			case kRenSolids: OnSolids(p, payload); break;
			case kRenMoverDig: MoverDigOnMessage(p, payload); break;  // bridge #43: a cell the Minecraft player mined out of a mover (movers.cpp)
			case kRenDug: DigOnMessage(p, payload); break;  // bridge #33: the cells the Minecraft player dug out of the chamber (dig.cpp)
			case kRenLights: BlockLightsOnLights(p, payload); break;  // bridge #18: emitters -> Portal 2 dynamic lights
			case kRenAtlasRegion: OnAtlasRegion(p, payload); break;  // bridge #18: animated sprites (water, lava, fire)
			case kRenTexture: AvatarOnTexture(p, payload); break;  // bridge #16: the player's body (avatar.cpp)
			case kRenAvatar: AvatarOnMesh(p, payload); break;
			case kRenScene: SceneOnMesh(p, payload); break;  // bridge #19: entities, block entities, particles (scene.cpp)
			default: break;  // textures, avatar, scene, lights, animated atlas regions: later
		}
		tail += (8 + payload + 7) & ~7ull;
		__atomic_store_n(tailp, tail, __ATOMIC_RELEASE);
		if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > budgetMs) break;
	}
}

// ---- drawing ------------------------------------------------------------------------------
// Main thread, inside a view: queue the draw of every section in range for this view.
// (bridge #27) Portal 2's tonemap scale as the frame actually uses it, read on the render thread (the main thread's queued
// context reports 1.0): the hand light (bandlight.cpp) multiplies by it, as the shader does for the placed blocks.
Vector RenderToneScaleGet();  // composite.cpp
static std::atomic<float> g_renderTone{1.0f};
static double g_toneReadAt = 0;
float TerrainRenderTone() { return g_renderTone.load(); }
void TerrainDraw(const Vector &eye, int which, const QAngle *ang) {
	if ((which & kTerrainBandOpaque) && NowSec() - g_toneReadAt > 0.05) {
		g_toneReadAt = NowSec();
		OnRenderThread([] {
			const Vector t = RenderToneScaleGet();
			g_renderTone.store((t.x + t.y + t.z) / 3.0f);
		});
	}
	if (!g_terrainMat || g_sections.empty()) return;
	auto list = std::make_shared<std::vector<SectionPtr>>();
	float maxD2 = g_drawDistance * g_drawDistance;
	std::vector<SectionPtr> bandTl;
	auto drawList = [](IMaterial *m, const std::vector<GpuVertex> &vs) {
		const uint32_t n = (uint32_t)vs.size();
		// (lead) at most 2000 triangles per dynamic mesh: 8000 (24000 vertices) overflowed the engine's dynamic buffer on a
		// full section of Minecraft terrain and the lock returned no memory (crash, sp_a4_tb_catch in the terrain world)
		for (uint32_t first = 0; first + 2 < n; first += 3 * 2000) {
			const uint32_t cnt = std::min<uint32_t>(n - first, 3 * 2000) / 3 * 3;
			MeshBuilder mb(m, PrimitiveType::TRIANGLES, (int)(cnt / 3));
			for (uint32_t i = first; i < first + cnt; ++i) {
				const GpuVertex &gv = vs[i];
				mb.Position(Vector(gv.x, gv.y, gv.z));
				mb.Color(Color(gv.bgra[2], gv.bgra[1], gv.bgra[0], gv.bgra[3]));
				mb.TexCoord(0, gv.u, gv.v);
				mb.AdvanceVertex();
			}
			mb.Draw();
		}
	};
	// (bridge #52) The band is drawn through dynamic meshes, every view, from the CPU: it was meant for the few blocks a player
	// places in a chamber, but the mirror world can hold any amount of exported Minecraft (a saved world's old structures: 180
	// sections, 545000 vertices, three views a frame: 16 ms of drawing, then the engine's dynamic buffers gave no memory and the
	// game crashed). So: nearest sections first, none wholly behind the camera, and a vertex budget per call.
	std::vector<Section *> bandSel;
	{
		Vector fwd(0, 0, 0);
		if (ang) {
			const float pr = ang->x * 3.14159265f / 180.0f, yr = ang->y * 3.14159265f / 180.0f;
			fwd = Vector(std::cos(pr) * std::cos(yr), std::cos(pr) * std::sin(yr), -std::sin(pr));
		}
		std::vector<std::pair<float, Section *>> cand;
		for (auto &kv : g_sections) {
			Section &sec = *kv.second;
			if (!sec.band) continue;
			const Vector d = sec.centre - eye;
			const float d2 = d.SquaredLength();
			if (d2 > maxD2) continue;
			if (ang && d2 > 700.0f * 700.0f && d.x * fwd.x + d.y * fwd.y + d.z * fwd.z < -700.0f) continue;  // wholly behind the camera (a section's radius is ~560 u)
			cand.push_back({d2, &sec});
		}
		std::sort(cand.begin(), cand.end(), [](const std::pair<float, Section *> &a, const std::pair<float, Section *> &b) { return a.first < b.first; });
		const bool tl = (which & kTerrainBandTranslucent) != 0;
		size_t budget = tl ? 12000 : 40000;  // vertices per call (a view has two calls)
		for (auto &c : cand) {
			const size_t n = tl ? c.second->tverts.size() + c.second->atverts.size() + c.second->skirt.size() : c.second->verts.size() + c.second->averts.size();
			if (n == 0) continue;
			if (n > budget && !bandSel.empty()) break;
			budget = n >= budget ? 0 : budget - n;
			bandSel.push_back(c.second);
			if (budget == 0) break;
		}
		static double lastLog = 0;
		if (cand.size() > bandSel.size() && NowSec() - lastLog > 10) {
			lastLog = NowSec();
			console->Print("[bridge] band: %zu sections in range, %zu drawn (vertex budget)\n", cand.size(), bandSel.size());
		}
	}
	for (auto &kv : g_sections) {
		Section &sec = *kv.second;
		if ((sec.centre - eye).SquaredLength() > maxD2) continue;
		if (sec.band) {
			if (std::find(bandSel.begin(), bandSel.end(), &sec) == bandSel.end()) continue;
			if ((which & kTerrainBandTranslucent) && (!sec.tverts.empty() || !sec.atverts.empty()) && g_terrainMatTl) bandTl.push_back(kv.second);
			if (!(which & kTerrainBandOpaque)) continue;
			// bridge #17: the band's few placed blocks go through a dynamic mesh every view (the vertices stay on the CPU), as
			// avatar.cpp and worldents.cpp draw (SkyCraft keeps an immutable vertex buffer per section; the static-mesh path here
			// lost its texcoords: flat colour boxes in b17_place1)
			drawList(g_terrainMat, sec.verts);
			if (!sec.averts.empty() && g_animMat) drawList(g_animMat, sec.averts);
			continue;
		}
		if (which & kTerrainRegion) list->push_back(kv.second);
	}
	if (which & kTerrainBandTranslucent) {
		BandLightRampDraw(eye);
		IMaterial *sm = BandSkirtMaterial();
		for (auto &kv : g_sections) {
			const Section &sec = *kv.second;
			if (!sec.band || sec.skirt.empty() || !sm || (sec.centre - eye).SquaredLength() > maxD2) continue;
			if (std::find(bandSel.begin(), bandSel.end(), &sec) == bandSel.end()) continue;
			const uint32_t nq = (uint32_t)sec.skirt.size() / 4;
			for (uint32_t first = 0; first < nq; first += 4000) {
				const uint32_t cnt = std::min<uint32_t>(nq - first, 4000);
				MeshBuilder mb(sm, PrimitiveType::QUADS, (int)cnt);
				for (uint32_t i = first * 4; i < (first + cnt) * 4; ++i) {
					const GpuVertex &gv = sec.skirt[i];
					mb.Position(Vector(gv.x, gv.y, gv.z));
					mb.Color(Color(0, 0, 0, gv.bgra[3]));
					mb.AdvanceVertex();
				}
				mb.Draw();
			}
		}
	}
	// translucent triangles (water, stained glass, ice) back to front from this view, as Minecraft sorts them
	if (!bandTl.empty()) {
		struct Tri {
			float d;
			const GpuVertex *g;
			IMaterial *m;
		};
		std::vector<Tri> tris;
		auto add = [&](const std::vector<GpuVertex> &vs, IMaterial *m) {
			if (!m) return;
			for (size_t t = 0; t + 2 < vs.size(); t += 3) {
				const GpuVertex *g = &vs[t];
				const float cx = (g[0].x + g[1].x + g[2].x) / 3 - eye.x, cy = (g[0].y + g[1].y + g[2].y) / 3 - eye.y, cz = (g[0].z + g[1].z + g[2].z) / 3 - eye.z;
				tris.push_back({cx * cx + cy * cy + cz * cz, g, m});
			}
		};
		for (auto &sec : bandTl) add(sec->tverts, g_terrainMatTl), add(sec->atverts, g_animMatTl);
		std::sort(tris.begin(), tris.end(), [](const Tri &a, const Tri &b) { return a.d > b.d; });
		std::vector<GpuVertex> run;
		IMaterial *cur = nullptr;
		for (auto &t : tris) {
			if (t.m != cur && !run.empty()) drawList(cur, run), run.clear();
			cur = t.m;
			run.insert(run.end(), t.g, t.g + 3);
		}
		if (!run.empty()) drawList(cur, run);
	}
	if (list->empty()) return;
	IMaterial *mat = g_terrainMat;
	int uploads = g_uploadsPerView;
	OnRenderThread([list, mat, uploads]() mutable {
		auto rc = materialSystem->GetRenderContext();
		for (auto &sec : *list) {
			if (!sec->uploaded) {
				if (uploads <= 0) continue;
				--uploads;
				Upload(*sec, mat);
			}
			for (auto *m : sec->meshes) {
				IMesh *dyn = rc->vtable->GetDynamicMesh(rc, true, m, m, mat);
				if (dyn) dyn->Draw();
			}
		}
		rc->vtable->Release(rc);
	});
}

bool TerrainIsSolid(int bx, int by, int bz) {
	int sx = bx >> 4, sy = by >> 4, sz = bz >> 4;
	auto it = g_solids.find(SectionKey(sx, sy, sz));
	if (it == g_solids.end()) return false;
	int bit = (bx & 15) + 16 * (bz & 15) + 256 * (by & 15);
	return (it->second[bit >> 6] >> (bit & 63)) & 1;
}
bool TerrainHasSolids() { return !g_solids.empty(); }
// (bridge #21) Voxel ray cast through the Minecraft solids (placed blocks and region terrain): from s to s + d (Minecraft block
// coordinates), the first solid voxel the ray enters. Returns the parameter t in [0,1], the axis (0 x, 1 y, 2 z) and the step sign
// of the face it entered through. Sections that hold no solid are skipped whole (a 16-block box at a time), so a long ray
// through a chamber's empty space costs a few hash lookups. A ray that starts inside a solid does not hit (not a surface test).
long long g_rayCasts = 0, g_rayHits = 0;
bool TerrainRay(const double s[3], const double d[3], double &tHit, int &axis, int &sign) {
	if (g_solids.empty()) return false;
	++g_rayCasts;
	int v[3], step[3];
	double tMax[3], tDelta[3];
	for (int i = 0; i < 3; ++i) {
		v[i] = (int)std::floor(s[i]);
		step[i] = d[i] > 0 ? 1 : d[i] < 0 ? -1 : 0;
		tDelta[i] = step[i] ? std::fabs(1.0 / d[i]) : 1e30;
		tMax[i] = step[i] ? ((step[i] > 0 ? v[i] + 1 - s[i] : s[i] - v[i]) * tDelta[i]) : 1e30;
	}
	double t = 0;
	int enterAxis = -1;
	bool first = true;
	for (int guard = 0; guard < 4096 && t < 1.0; ++guard) {
		auto it = g_solids.find(SectionKey(v[0] >> 4, v[1] >> 4, v[2] >> 4));
		if (it == g_solids.end()) {
			// no solid in this section: leave it in one go (the slab exit time over its 16-block box)
			double texit = 1e30;
			for (int i = 0; i < 3; ++i) {
				if (!step[i]) continue;
				const int lo = (v[i] >> 4) << 4, hi = lo + 16;
				const double edge = step[i] > 0 ? hi : lo;
				texit = std::min(texit, (edge - s[i]) / d[i]);
			}
			if (texit <= t) texit = t + 1e-9;
			t = texit;
			for (int i = 0; i < 3; ++i) {
				v[i] = (int)std::floor(s[i] + d[i] * (t + 1e-7));  // just inside the next section
				tMax[i] = step[i] ? ((step[i] > 0 ? v[i] + 1 - s[i] : s[i] - v[i]) * tDelta[i]) : 1e30;
			}
			enterAxis = -1;
			first = false;
			continue;
		}
		const int bit = (v[0] & 15) + 16 * (v[2] & 15) + 256 * (v[1] & 15);
		if ((it->second[bit >> 6] >> (bit & 63)) & 1) {
			if (first) return false;  // starts inside a block
			if (enterAxis < 0) { // entered from a skipped section: the face is the dominant crossing
				enterAxis = 0;
				for (int i = 1; i < 3; ++i) if (std::fabs(d[i]) > std::fabs(d[enterAxis])) enterAxis = i;
			}
			tHit = t, axis = enterAxis, sign = step[enterAxis] ? step[enterAxis] : 1;
			++g_rayHits;
			return true;
		}
		first = false;
		int a = tMax[0] < tMax[1] ? (tMax[0] < tMax[2] ? 0 : 2) : (tMax[1] < tMax[2] ? 1 : 2);
		if (!step[a]) return false;
		t = tMax[a];
		if (t >= 1.0) return false;
		v[a] += step[a];
		tMax[a] += tDelta[a];
		enterAxis = a;
	}
	return false;
}
// The section holding this block has streamed (solid bits, or known to be all air).
bool TerrainSectionKnown(int bx, int by, int bz) {
	long long k = SectionKey(bx >> 4, by >> 4, bz >> 4);
	return g_solids.count(k) || g_airSections.count(k);
}

CON_COMMAND(pcb_column, "pcb_column <mcX> <mcZ> - terrain solidity of one Minecraft column, y 40..127 ('?' = section not held)\n") {
	if (args.ArgC() < 3) return console->Print("%s", pcb_column.ThisPtr()->m_pszHelpString);
	int x = std::atoi(args[1]), z = std::atoi(args[2]);
	char row[96];
	int n = 0;
	for (int y = 40; y < 128; ++y)
		row[n++] = g_solids.count(SectionKey(x >> 4, y >> 4, z >> 4)) ? (TerrainIsSolid(x, y, z) ? '#' : '.') : '?';
	row[n] = 0;
	console->Print("[bridge] column %d %d, y 40 up: %s\n", x, z, row);
}

CON_COMMAND(pcb_terrain_stats, "pcb_terrain_stats - Minecraft terrain streaming statistics\n") {
	console->Print("[bridge] terrain: %zu sections held, %d uploaded to the GPU, %d built, %d vertices built, %zu solid sections, %d ring messages, atlas %dx%d\n",
	               g_sections.size(), g_uploaded.load(), g_meshesBuilt, g_vertsBuilt, g_solids.size(), g_msgs, g_atlasW, g_atlasH);
	console->Print("[bridge] terrain: chamber sections built %d (%.1f ms total, max %.1f ms), identical resends skipped %d, lighting states cached %zu\n", g_bandBuilt, g_bandBuildMs, g_bandBuildMax, g_sectionSkips, g_bandStateCache.size());
}

// For worldents.cpp (bridge #17): the combined Minecraft atlas (block textures, item icons, crack stages).
const char *TerrainAtlasTexName(int &gen, float &vScale) {
	if (!g_terrainTex || !g_atlasName[0]) return nullptr;
	vScale = g_atlasTexH ? (float)g_atlasH / g_atlasTexH : 1.0f;
	gen = g_atlasGen;
	return g_atlasName;
}

// Solid blocks above vanilla's build limit (MC y >= 320): what was placed in the chamber band (tests).
int TerrainBandSolidCount() {
	int n = 0;
	for (auto &kv : g_solids) {
		const long long sy = kv.first & 0xFFFFF;
		if (sy < 20 || sy >= 0x80000) continue;
		for (uint64_t w : kv.second) n += __builtin_popcountll(w);
	}
	return n;
}

CON_COMMAND(pc_dig_caps, "pc_dig_caps [0|1] - (bridge #76) cut caps: the walls of dug holes clipped exactly to Portal 2's solid (1, default) or the mod's 5 u sub-voxel walls (0); rebuilds the band\n") {
	if (args.ArgC() > 1) {
		g_capsOn = std::atoi(args[1]) != 0;
		g_capSections.clear();
		TerrainBandRebuild();
	}
	console->Print("[bridge] dig caps %s: %ld faces capped in %ld section builds (%ld pieces), %ld of the mod's wall quads replaced, %ld kept (no BSP solid there), %ld faces with no texture to take, %.1f ms in all\n",
	               g_capsOn ? "ON" : "off", g_capsFaces, g_capsSections, g_capsPieces, g_capsModDropped, g_capsModKept, g_capsNoTex, g_capsMs);
}

// (bridge #79) one dug cell's caps: what the band build saw and did for each face (the u_westend cap that the seam probe found missing)
int CutLightCellDist(int x, int y, int z);  // cutlight.cpp
CON_COMMAND(pc_dig_caps_trace, "pc_dig_caps_trace <x> <y> <z> <dir 0-5> - (bridge #80) at the next build of that dug cell's section, print every final band triangle on its face toward dir (Source positions after the winding swap, normal, uv, colour, opaque/translucent); rebuilds the section\n") {
	if (args.ArgC() < 5) return console->Print("usage: pc_dig_caps_trace x y z dir (0 down 1 up 2 north 3 south 4 west 5 east)\n");
	const int D[3] = {std::atoi(args[1]), std::atoi(args[2]), std::atoi(args[3])}, dir = std::atoi(args[4]) % 6;
	static const int kStep[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
	const int axis = dir >> 1 == 0 ? 1 : dir >> 1 == 1 ? 2 : 0;
	const double plane = D[axis] + (kStep[dir][axis] > 0 ? 1.0 : 0.0);
	float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
	for (int k = 0; k < 4; ++k) {
		double m[3];
		m[axis] = plane, m[(axis + 1) % 3] = D[(axis + 1) % 3] + (k & 1), m[(axis + 2) % 3] = D[(axis + 2) % 3] + (k >> 1);
		const Vector s = McToSrc(m[0], m[1], m[2]);
		const float c[3] = {s.x, s.y, s.z};
		for (int a = 0; a < 3; ++a) lo[a] = std::min(lo[a], c[a]), hi[a] = std::max(hi[a], c[a]);
	}
	int sa = 0;
	for (int a = 1; a < 3; ++a)
		if (hi[a] - lo[a] < hi[sa] - lo[sa]) sa = a;
	g_capTraceAxis = sa, g_capTracePlane = 0.5f * (lo[sa] + hi[sa]);
	for (int a = 0; a < 3; ++a) g_capTraceLo[a] = lo[a], g_capTraceHi[a] = hi[a];
	g_capTraceSec = std::make_tuple(D[0] >> 4, D[1] >> 4, D[2] >> 4);
	g_capTraceOn = true;
	console->Print("[bridge] caps trace armed: section %d %d %d, Source plane axis %d at %.1f, [%.1f %.1f %.1f .. %.1f %.1f %.1f]\n", D[0] >> 4, D[1] >> 4, D[2] >> 4, sa, g_capTracePlane, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
	TerrainBandRebuild();
}

CON_COMMAND(pc_cut_light_faces, "pc_cut_light_faces [0|1] - (bridge #80) which band faces take the cut's light: 1 (default) every face looking into a dug air cell of the cut, 0 only faces whose light sample point is inside Portal 2's solid (#79); rebuilds the band\n") {
	if (args.ArgC() >= 2) {
		g_cutFaceRule = std::atoi(args[1]) ? 1 : 0;
		TerrainBandRebuild();
	}
	console->Print("[bridge] pc_cut_light_faces %d\n", g_cutFaceRule);
}

CON_COMMAND(pc_dig_caps_rect, "pc_dig_caps_rect [0|1] - (bridge #80) how a cut cap's atlas rect is chosen: 1 = from the widest of the mod's wall quads and opaque in the atlas (default), 0 = bridge #76's (the last quad seen); rebuilds the band\n") {
	if (args.ArgC() >= 2) {
		g_capRectPick = std::atoi(args[1]) ? 1 : 0;
		g_bandRebuild = true;
	}
	console->Print("[bridge] pc_dig_caps_rect %d\n", g_capRectPick);
}

CON_COMMAND(pc_dig_caps_at, "pc_dig_caps_at <x> <y> <z> - (bridge #79) a dug cell's faces (Minecraft block coordinates): neighbour dug/block, the exact cap's pieces now, and what its section's last band build emitted\n") {
	if (args.ArgC() < 4) return console->Print("usage: pc_dig_caps_at x y z\n");
	const int D[3] = {std::atoi(args[1]), std::atoi(args[2]), std::atoi(args[3])};
	static const int kStep[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
	static const char *kName[6] = {"down", "up", "north", "south", "west", "east"};
	const long long dk = ((long long)(D[0] & 0x1FFFFF) << 42) | ((long long)(D[1] & 0x1FFFFF) << 21) | (long long)(D[2] & 0x1FFFFF);
	const auto sk = std::make_tuple(D[0] >> 4, D[1] >> 4, D[2] >> 4);
	console->Print("[bridge] caps at %d %d %d: dug %d, block %d, cut light d %d; section %d %d %d raw %s, capped %s, drawn %s\n", D[0], D[1], D[2], DigIsDugMc(D[0], D[1], D[2]), TerrainIsSolid(D[0], D[1], D[2]),
	               CutLightCellDist(D[0], D[1], D[2]), std::get<0>(sk), std::get<1>(sk), std::get<2>(sk), g_bandRaw.count(sk) ? "yes" : "NO", g_capSections.count(SectionKey(D[0] >> 4, D[1] >> 4, D[2] >> 4)) ? "yes" : "no",
	               g_sections.count(sk) ? "yes" : "NO");
	auto world = CollisionFieldHarvestWorld();
	for (int dir = 0; dir < 6; ++dir) {
		const int N[3] = {D[0] + kStep[dir][0], D[1] + kStep[dir][1], D[2] + kStep[dir][2]};
		const int axis = dir >> 1 == 0 ? 1 : dir >> 1 == 1 ? 2 : 0, a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
		const double plane = D[axis] + (kStep[dir][axis] > 0 ? 1.0 : 0.0);
		std::string pieces;
		if (world) {
			colharvest::World::Poly poly;
			static const double kSq[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
			for (const auto &q : kSq) {
				double m[3];
				m[axis] = plane, m[a1] = D[a1] + q[0], m[a2] = D[a2] + q[1];
				const Vector s = McToSrc(m[0], m[1], m[2]);
				poly.push_back({s.x, s.y, s.z});
			}
			const Vector into = McToSrc(N[0] + 0.5, N[1] + 0.5, N[2] + 0.5) - McToSrc(D[0] + 0.5, D[1] + 0.5, D[2] + 0.5);
			const double in3[3] = {into.x / kUnitsPerBlock, into.y / kUnitsPerBlock, into.z / kUnitsPerBlock};
			std::vector<colharvest::World::Poly> parts;
			const double area = world->SolidParts(poly, in3, parts);
			char b[160];
			std::snprintf(b, sizeof b, "%zu pieces %.1f u2", parts.size(), area);
			pieces = b;
			for (const auto &pp : parts) {
				double lo[3] = {1e9, 1e9, 1e9}, hi[3] = {-1e9, -1e9, -1e9};
				for (const auto &q : pp)
					for (int t = 0; t < 3; ++t) lo[t] = std::min(lo[t], q[t]), hi[t] = std::max(hi[t], q[t]);
				std::snprintf(b, sizeof b, " [%.1f %.1f %.1f .. %.1f %.1f %.1f]", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
				pieces += b;
			}
		} else
			pieces = "no harvest world";
		auto em = g_capEmit.find({dk, dir});
		const std::string last = em == g_capEmit.end() ? "no cap" : em->second < 0 ? "mod wall kept (no solid)" : std::to_string(em->second) + " cap pieces";
		console->Print("[bridge]   %-5s -> %d %d %d: dug %d block %d; exact %s; last build %s\n", kName[dir], N[0], N[1], N[2], DigIsDugMc(N[0], N[1], N[2]), TerrainIsSolid(N[0], N[1], N[2]), pieces.c_str(), last.c_str());
		auto cr = g_capRect.find({dk, dir});
		if (cr != g_capRect.end()) {
			const CapRectInfo &ri = cr->second;
			console->Print("[bridge]         rect from %c (span %.2f): texels u %.2f..%.2f v %.2f..%.2f, opaque %.2f\n", ri.src, ri.span, ri.u0 * g_atlasW, ri.u1 * g_atlasW, ri.v0 * g_atlasH, ri.v1 * g_atlasH, ri.opaque);
		}
	}
}

CON_COMMAND(pcb_band_stats, "pcb_band_stats - the chamber band (blocks placed in Portal 2's chambers): sections, raw records, light samples\n") {
	int n = 0, up = 0;
	for (auto &kv : g_sections)
		if (kv.second->band) ++n, up += kv.second->uploaded;
	console->Print("[bridge] band: %d sections drawn (%d uploaded), %zu raw kept, %d built, %d light samples, lit %d; lighting states %d ok, %d empty (ComputeLighting fallback); build %.1f ms total, %.2f ms max per section; relit %d, pending %zu, gave up %d, deferred %d\n", n, up, g_bandRaw.size(), g_bandBuilt, g_bandLightCalls, g_bandLit, g_bandStateOk, g_bandStateEmpty, g_bandBuildMs, g_bandBuildMax, g_bandRelights, g_bandRelight.size(), g_bandRelightGiveUps, g_bandDeferred);
}

CON_COMMAND(pcb_band_light, "pcb_band_light 0|1|2 [k] [block_k] [fill] - chamber-band blocks: 2 Portal 2's model lighting state per vertex (default; k scales it), 1 #17's ComputeLighting x5 (k: world-light floor), 0 Minecraft's baked light; rebuilds\n") {
	if (args.ArgC() > 1) g_bandLit = std::atoi(args[1]);
	if (args.ArgC() > 2) (g_bandLit == 1 ? g_bandWorldK : g_bandK) = (float)std::atof(args[2]);
	if (args.ArgC() > 3) g_bandBlockK = (float)std::atof(args[3]);
	if (args.ArgC() > 4) g_bandFill = (float)std::atof(args[4]);
	if (args.ArgC() > 5) g_bandGamma = std::fmax(0.5f, (float)std::atof(args[5]));
	g_bandRebuild = true;  // (in the client frame: ComputeLighting from a console/script command returned 0, all black, b17_place5)
	console->Print("[bridge] band light %d, k %.2f, block k %.2f, fill %.2f, world k %.2f, %zu sections to rebuild\n", g_bandLit, g_bandK, g_bandBlockK, g_bandFill, g_bandWorldK, g_bandRaw.size());
}

CON_COMMAND(pcb_band_skirts, "pcb_band_skirts 0|1 - contact shadows on Portal 2's surfaces around placed blocks (default 1)\n") {
	if (args.ArgC() > 1) g_bandSkirts = std::atoi(args[1]);
	for (auto &kv : g_sections)
		if (kv.second->band) g_skirtDirty[kv.first] = 1;
	console->Print("[bridge] band skirts %d (%d quads built so far; %.1f ms total, %.2f ms max per section)\n", g_bandSkirts, g_skirtQuads, g_skirtMs, g_skirtMax);
}

CON_COMMAND(pc_mc_anim_hz, "pc_mc_anim_hz [hz=10] - how often the animation texture is uploaded for animated sprites on placed blocks (water, lava, fire); 0 freezes them\n") {
	if (args.ArgC() > 1) g_animHz = (float)std::atof(args[1]);
	console->Print("[bridge] animated sprites: %.1f Hz, %zu animated sprites known (texture %dx%d), %d region frames received, %d uploads, band animated %d\n", g_animHz, g_animRects.size(),
	               g_animW, g_animH, g_atlasRegions, g_animDownloads, g_bandAnimated);
}

CON_COMMAND(pcb_atlas_srgb, "pcb_atlas_srgb 0|1 - the Minecraft atlas is an sRGB texture (default 1; applies to the next atlas Minecraft sends)\n") {
	if (args.ArgC() > 1) g_atlasSrgb = std::atoi(args[1]) != 0;
	console->Print("[bridge] atlas sRGB %d\n", g_atlasSrgb);
}

CON_COMMAND(pcb_terrain_exposure, "pcb_terrain_exposure <k> - brightness of newly built sections (default 1)\n") {
	if (args.ArgC() > 1) g_exposure = (float)std::atof(args[1]);
	if (args.ArgC() > 2) g_unlit = std::atoi(args[2]) != 0;
	console->Print("[bridge] terrain exposure %.2f (applies to sections built from now on)\n", g_exposure);
}

}  // namespace bridge
