// Portalcraft bridge: shared declarations.
#pragma once

#include "Interface.hpp"
#include "Modules/Console.hpp"
#include "Utils/SDK.hpp"

#include <initializer_list>
#include <string>
#include <vector>

namespace bridge {

// 1 Minecraft block = 40 Source units: Portal 2's 72 u standing player is
// then 1.8 blocks, the Minecraft player's height (lead decision, route B).
constexpr float kUnitsPerBlock = 40.0f;

// The repo root ($PORTALCRAFT_ROOT, else found by the .portalcraft-root marker above the .so).
std::string RepoRoot();
// Directory for the log and screenshots copied out of the game.
std::string CacheDir();
// Directory for Minecraft assets (block textures).
std::string MinecraftAssetsDir();

// ---- engine (engine.so) -----------------------------------------------------
struct Engine {
	Interface *client = nullptr;       // VEngineClient015
	Interface *traceServer = nullptr;  // EngineTraceServer004
	Interface *traceClient = nullptr;  // EngineTraceClient004
#ifdef _WIN32
	// MSVC (the engine) takes `this` in ECX and the hidden struct-return pointer as the first stack argument; GCC does the opposite for a
	// thiscall pointer that returns a struct. Spelling the return pointer out reproduces the MSVC layout (WINDOWS-VERIFY F2).
	using _GetLightForPoint = Vector *(__rescall *)(void *thisptr, Vector *ret, const Vector &pos, bool clamp);
#else
	using _GetLightForPoint = Vector(__rescall *)(void *thisptr, const Vector &pos, bool clamp);
#endif
	using _GetViewAngles = void(__rescall *)(void *thisptr, QAngle &va);
	using _PointOutsideWorld = bool(__rescall *)(void *thisptr, const Vector &pt);
	using _GetPointContents = int(__rescall *)(void *thisptr, const Vector &pt, int mask, void **ent);
	using _ComputeLighting = void(__rescall *)(void *thisptr, const Vector &pt, const Vector *normal, bool clamp, Vector &color, Vector *boxColors);
	_ComputeLighting ComputeLighting = nullptr;
	using _SetViewAngles = void(__rescall *)(void *thisptr, QAngle &va);
	using _ClientCmd = void(__rescall *)(void *thisptr, const char *cmd);
	using _TraceRay = void(__rescall *)(void *thisptr, const Ray_t &ray, unsigned int mask, ITraceFilter *filter, CGameTrace *tr);
	_GetLightForPoint GetLightForPoint = nullptr;
	_GetViewAngles GetViewAngles = nullptr;
	_SetViewAngles SetViewAngles = nullptr;
	_ClientCmd ClientCmd = nullptr;
	_ClientCmd ExecuteClientCmd = nullptr;
	_TraceRay TraceRay = nullptr;
	_PointOutsideWorld PointOutsideWorld = nullptr;
	_GetPointContents GetPointContents = nullptr;

	bool Init();
	void Shutdown();
	Vector LightAt(const Vector &p);
	// Engine lighting at a point for a surface normal: the leaf ambient cube
	// plus local world lights, as models are lit (IVEngineClient::ComputeLighting).
	Vector LightAt(const Vector &p, const Vector &normal);
	QAngle ViewAngles();
	using _GetScreenSize = void(__rescall *)(void *thisptr, int &w, int &h);
	using _BoolNoArgs = bool(__rescall *)(void *thisptr);
	_GetScreenSize GetScreenSize = nullptr;
	_BoolNoArgs Con_IsVisible = nullptr;
	_BoolNoArgs IsPaused = nullptr;
	using _GetPointContentsWorld = int(__rescall *)(void *thisptr, const Vector &pt, int mask);
	_GetPointContentsWorld GetPointContentsWorld = nullptr;
	void SetAngles(const QAngle &a);
	void Cmd(const char *cmd);
	// Swept ray (halfExtents = 0) or box trace against the server's world.
	bool Trace(const Vector &start, const Vector &end, const Vector &halfExtents, unsigned mask, CGameTrace &tr, bool worldOnly);
	// World-only ray against the client's collision world.
	bool TraceClient(const Vector &start, const Vector &end, unsigned mask, CGameTrace &tr);
};
extern Engine engine;

// ---- a block placed in the Source world --------------------------------------
struct Block {
	Vector mins;  // Source units, Z up; the block spans mins .. mins + kUnitsPerBlock
	int kind;     // 0 grass, 1 dirt, 2 oak planks
};
extern std::vector<Block> g_blocks;

// Portals the bridge placed (linkage group 0): centre, normal, placed yet.
// (bridge #32) portal SLOTS: slot = group * 2 + colour, a group being a LinkageGroupID in order of first sight (clip.cpp PortalSlotOf), group 0 = the
// player's gun pair (slots 0 and 1, what every older user of these arrays means). A portal's partner is slot ^ 1. Portal 2 pairs one portal per
// colour per linkage group, so every active pair of a map has its own two slots.
constexpr int kPortalSlots = 16;
extern Vector g_portalPos[kPortalSlots], g_portalNormal[kPortalSlots], g_portalUp[kPortalSlots];  // up: the opening's long axis
extern bool g_portalSet[kPortalSlots];
extern float g_portalHalfW[kPortalSlots], g_portalHalfH[kPortalSlots];  // (bridge #68) half the opening, u: 32 x 56 for a gun portal; a linked_portal_door has its own size
inline bool PortalLinked(int slot) { return slot >= 0 && slot < kPortalSlots && g_portalSet[slot] && g_portalSet[slot ^ 1]; }

// ---- rendering (render.cpp) --------------------------------------------------
bool RenderInit();       // materials, texture atlas, view hook
void RenderShutdown();
void RenderTraceViews(int n);  // log the next n hooked views
struct ViewRecord {
	Vector origin;
	QAngle angles;
	int frame;
};
const ViewRecord &LastMainView();
void RenderSetLightParams(float scale, float gamma, float floor);
int RenderViewsLastFrame();
void RenderSelectMaterial(int n, bool debugColor);

// ---- player puppet (puppet.cpp) ------------------------------------------------
// Overrides the player's position each movement tick (server and client
// prediction) and the view angles each frame: the PlayerPuppet/CameraDriver
// mechanism Minecraft mode needs.
bool PuppetInit();
void PuppetShutdown();
void PuppetFrame();
void PuppetStart(const Vector &centre, float radius, float period);
void PuppetStop();
void PuppetSetProvider(bool (*fn)(Vector &pos, Vector &vel));  // null: off
Vector PlayerOrigin();    // server-side feet after the last movement tick
Vector PlayerVelocity();

// ---- solid blocks (collider.cpp) ------------------------------------------------
bool ColliderInit();
void *Spawn(const char *cls, std::initializer_list<std::pair<const char *, std::string>> kvs);
void ColliderShutdown();

// ---- player vs blocks (blockcollide.cpp) --------------------------------------------
bool BlockCollideInit();
void BlockCollideShutdown();
void BlockCollideLevelStart();

// ---- Minecraft player (mc.cpp) --------------------------------------------------------
bool McInit();
void McShutdown();
void McLevelInit(const char *map);
void McLevelLive();

// ---- Minecraft terrain (terrain.cpp) ------------------------------------------------------
// bridge #17: which parts: the chamber band's opaque triangles (drawn in the opaque pass, before portals), its translucent
// ones, the region's sections (debug only)
enum { kTerrainBandOpaque = 1, kTerrainBandTranslucent = 2, kTerrainRegion = 4 };
void TerrainDraw(const Vector &eye, int which, const QAngle *ang = nullptr);
void WorldEntsDraw(const Vector &eye);  // worldents.cpp (bridge #17)
void TerrainClear();
bool TerrainIsSolid(int bx, int by, int bz);  // Minecraft block coords
bool TerrainHasSolids();

void CrashLogInstall();
void CrashLogRemove();

// ---- the map's logic_playerproxy outputs (mapindex.cpp, bridge #70) ----
struct PlayerProxyConn {
	std::string output, target, input, param;  // output: OnJump | OnDuck | OnUnDuck
	float delay = 0;
	int times = -1;  // -1: every time
};
const std::vector<PlayerProxyConn> &MapPlayerProxyConns();

// ---- server entities (ents.cpp) --------------------------------------------------------
std::string EntKV(void *ent, const char *key);
Vector EntVec(void *ent, const char *key);
std::vector<void *> EntFind(const char *classname, const char *targetname = nullptr);
bool EntInput(void *ent, const char *input, const char *value = nullptr);
void SnapPlayer(const Vector &pos, const QAngle &ang);
void EntsShutdown();

// ---- Minecraft's own frame composited into Portal 2 (composite.cpp) ---------------------------
bool FluidsInit();
void FluidsLevelInit();  // per-level caches of the trace hook
void FluidsShutdown();
int FluidAt(const Vector &p);  // Minecraft fluid at a Source point: 0 none, 1 water, 2 lava
bool PortalGunInit();
void PortalGunTick();
void PortalGunShutdown();
bool CompositeInit();
bool McLightInit();      // mclight.cpp: Minecraft lighting on Portal 2 models in the region
void McLightShutdown();
void CompositeShutdown();
bool CompositeEnabled();
float CompositeOverscan();
void McFrameSize(int sw, int sh, float overscan, int &w, int &h);  // composite.cpp: the size Minecraft renders at (screen x overscan, fitted to the composite's textures)
void McFrameRequested(int &w, int &h);                              // mc.cpp: the size last sent to Minecraft (0 x 0 before the first)
uint64_t CompositeFramesReceived();
bool CompositeProbesIdle();  // no per-frame probe (measure/dump/watch) is still running
int CompositeHostFramesCounted();
void TransitFrame(CViewSetup *view);  // transit.cpp
void CompositeFrameBegin(const CViewSetup *main);           // once per frame, before rendering
bool CompositeWantsEarly(const CViewSetup *view);  // composite this view at the start of the opaque pass (a lone region portal is opening)
bool CompositeView(const CViewSetup *view, bool isMain);    // per 3D view; true: Minecraft drawn
bool ClipRegionContains(const Vector &p);
int ClipRegionPortal();

// ---- the pit-flings clip (clip.cpp) ------------------------------------------------------
void ClipLevelLive();
void ClipFrame();
void PropCollideTick();
void PropCollideLevelLive();
void FloatOriginTick();       // floatorigin.cpp: endless terrain
void FloatOriginLevelLive();
void ChellProxyLevelLive();
void AudioShutdown();
void ClipTick();

// ---- scripted test runs (script.cpp) ----------------------------------------
void ScriptTick();        // once per server tick
void ScriptLevelInit();
void ScriptShutdown();

}  // namespace bridge
