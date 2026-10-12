// Portalcraft bridge: every literal virtual-function slot number (and the few engine-object byte offsets) in one table, with a Linux and a
// Windows column.
//   Linux column   : exactly the numbers the bridge has always used.
//   Windows column : Portal 2 on Windows is built with MSVC, whose vtable layout differs from GCC's (WINDOWS-VERIFY.md). Each entry says how
//                    sure we are:
//     Sar        : upstream SourceAutoRecord documents this method's Windows number (or uses the same struct layout on Windows).
//     Derived    : computed from Valve's public SDK header plus the two MSVC rules (one destructor slot; overloads grouped, reversed). The
//                  rules reproduced 14 numbers SAR found by disassembly. Used by default, confirmed by pcb_probe_slots on the first run.
//     ProtonVerified : checked by running the Windows game under Proton (2026-10-11); treated like Sar for the policy. The note says how.
//     WindowsVerified: checked on real Windows (2026-10-12, pcb_probe_slots after a map load, and every test run there with it on); like Sar.
//     Unverified : Portal 2 only (no public header documents it). NOT used on Windows unless the first-run "safe-load" policy enables it:
//                  environment variable PCB_WIN_UNVERIFIED, or the file cache/bridge/win_unverified.txt, holding "all" or a list of group
//                  names or slot names (separated by commas, spaces or newlines). The plugin logs every use it skipped.
//   A wrong number found by the probe is fixed without rebuilding: PCB_SLOTS="RcFlush=5,EngComputeLighting=70" or lines "Name=number" in
//   cache/bridge/slot_overrides.txt. An overridden entry counts as enabled.
// Linux builds ignore the policy (every entry is usable) so Linux behaviour is unchanged.
#pragma once

#include "Interface.hpp"

#include <cstdint>

namespace bridge::slots {

enum class Basis : uint8_t { Sar, Derived, Unverified, ProtonVerified, WindowsVerified };
enum class Kind : uint8_t { Slot, Offset };

//   X(Name, interface, module, group, linux, windows, basis, kind, note)
#define PCB_SLOT_TABLE(X)                                                                                                                              \
	/* IMatRenderContext (the material system's render context; vtable lives in materialsystem) */                                                    \
	X(RcRelease, "IMatRenderContext", "materialsystem", "rc", 1, 1, Sar, Slot, "SAR MeshBuilder struct layout, same on both systems")             \
	X(RcBeginRender, "IMatRenderContext", "materialsystem", "rc", 2, 2, Sar, Slot, "SAR MeshBuilder struct layout")                                \
	X(RcEndRender, "IMatRenderContext", "materialsystem", "rc", 3, 3, Sar, Slot, "SAR MeshBuilder struct layout")                                  \
	X(RcFlush, "IMatRenderContext", "materialsystem", "rc", 4, 4, Derived, Slot, "before any overload group")                                       \
	X(RcReadPixels, "IMatRenderContext", "materialsystem", "rc", 13, 13, Derived, Slot, "x,y,w,h form; header says unchanged on MSVC")             \
	X(RcMatrixMode, "IMatRenderContext", "materialsystem", "rc", 20, 20, Derived, Slot, "")                                                        \
	X(RcPushMatrix, "IMatRenderContext", "materialsystem", "rc", 21, 21, Derived, Slot, "")                                                        \
	X(RcPopMatrix, "IMatRenderContext", "materialsystem", "rc", 22, 22, Derived, Slot, "")                                                         \
	X(RcGetMatrix, "IMatRenderContext", "materialsystem", "rc", 29, 30, Derived, Slot, "GetMatrix(mode, VMatrix*) is overloaded with the 3x4 form: MSVC lists them reversed") \
	X(RcLoadIdentity, "IMatRenderContext", "materialsystem", "rc", 31, 31, Derived, Slot, "")                                                      \
	X(RcCreateStaticMesh, "IMatRenderContext", "materialsystem", "rc", 55, 55, Derived, Slot, "")                                                  \
	X(RcDestroyStaticMesh, "IMatRenderContext", "materialsystem", "rc", 56, 56, Derived, Slot, "")                                                 \
	X(RcGetDynamicMesh, "IMatRenderContext", "materialsystem", "rc", 57, 57, Sar, Slot, "SAR MeshBuilder struct layout")                           \
	X(RcSetStencilState, "IMatRenderContext", "materialsystem", "rc_stencil", 119, 119, WindowsVerified, Slot, "Portal 2 only; Windows 2026-10-12: the probe after a map load shows a function taking the one reference argument the declaration has (before a map load the queued context answers instead)") \
	X(RcPushCustomClipPlane, "IMatRenderContext", "materialsystem", "rc_stencil", 122, 122, WindowsVerified, Slot, "Portal 2 only; Windows 2026-10-12: the probe after a map load shows one pointer argument, as declared")                    \
	X(RcPopCustomClipPlane, "IMatRenderContext", "materialsystem", "rc_stencil", 123, 123, WindowsVerified, Slot, "Portal 2 only (follows PushCustomClipPlane); Windows 2026-10-12: the probe after a map load shows no arguments, as declared") \
	X(RcGetCallQueue, "IMatRenderContext", "materialsystem", "rc_callqueue", 142, 142, ProtonVerified, Slot, "Portal 2 only: the queued context returns the render thread's call queue; the probe checks the answer is non-null on a queued context; Proton 2026-10-11: non-null on the queued context, QueueFunctor ran the cut and terrain functors on the render thread (pr_dig15: 21056 world windows with the dig on)") \
	X(RcSetToneMappingScaleLinear, "IMatRenderContext", "materialsystem", "rc_tone", 145, 145, WindowsVerified, Slot, "Portal 2 only; Windows 2026-10-12: the probe after a map load shows one reference argument, as declared, next to the verified getter at 146")                 \
	X(RcGetToneMappingScaleLinear, "IMatRenderContext", "materialsystem", "rc_tone", 146, 146, ProtonVerified, Slot, "Portal 2 only (returns a Vector: hidden pointer argument on Windows); Proton 2026-10-11: the tone scale read each frame varies sensibly (1.08 to 1.06), hand light uses it (pr_dig15)") \
	X(CallQueueQueueFunctor, "ICallQueue", "materialsystem", "rc_callqueue", 0, 0, Derived, Slot, "first method of the queue interface")             \
	/* fields inside the material system's render context object (not vtable slots) */                                                                   \
	X(RcFieldAmbient, "CMatRenderContext", "materialsystem", "rc_fields", 0x7c, 0x7c, ProtonVerified, Offset, "byte offset of the ambient cube; assumes MSVC lays the object out like GCC; Proton 2026-10-11: band light state (ambient cube + local lights) gave sensible colours for the terrain blocks (pr_dig15, pr_dig6 e2)") \
	X(RcFieldLights, "CMatRenderContext", "materialsystem", "rc_fields", 0xdc, 0xdc, ProtonVerified, Offset, "byte offset of the local light array; Proton 2026-10-11: as RcFieldAmbient (4 local lights counted in the bedroom)")  \
	X(RcFieldLightCount, "CMatRenderContext", "materialsystem", "rc_fields", 0x23c, 0x23c, ProtonVerified, Offset, "byte offset of the light count; Proton 2026-10-11: as RcFieldAmbient")  \
	/* IMaterial */                                                                                                                                     \
	X(MaterialFindVar, "IMaterial", "materialsystem", "material", 11, 11, Derived, Slot, "")                                                       \
	/* IServerTools (server): MSVC has no extra destructor slot here, so the plain methods are one lower */                                          \
	X(ToolsSnapPlayer, "IServerTools", "server", "tools", 3, 2, Derived, Slot, "SnapPlayerToPosition")                                               \
	X(ToolsFirstEntity, "IServerTools", "server", "tools", 8, 7, Derived, Slot, "")                                                                \
	X(ToolsNextEntity, "IServerTools", "server", "tools", 9, 8, Derived, Slot, "")                                                                 \
	X(ToolsGetKeyValue, "IServerTools", "server", "tools", 11, 10, Derived, Slot, "")                                                              \
	/* IVEngineServer (engine) */                                                                                                                       \
	X(EngClusterForOrigin, "IVEngineServer", "engine", "engsrv", 12, 12, Derived, Slot, "")                                                        \
	X(EngPVSForCluster, "IVEngineServer", "engine", "engsrv", 13, 13, Derived, Slot, "")                                                           \
	X(EngResetPVS, "IVEngineServer", "engine", "engsrv", 58, 58, Derived, Slot, "documented only in a comment")                                    \
	X(EngAddOriginToPVS, "IVEngineServer", "engine", "engsrv", 59, 59, Derived, Slot, "")                                                          \
	X(EngGetArea, "IVEngineServer", "engine", "engsrv", 64, 64, Derived, Slot, "")                                                                 \
	/* IVEngineClient */                                                                                                                                \
	X(ClientComputeLighting, "IVEngineClient", "engine", "engine_light", 69, 69, ProtonVerified, Slot, "Portal 2 only; no MSVC reordering before it, so probably unchanged; Proton 2026-10-11: returns real light (hand light 0.11 0.10 0.07 in the bedroom, band lighting states ok, pr_dig15)") \
	/* IEngineTrace */                                                                                                                                  \
	X(TraceGetPointContents, "IEngineTrace", "engine", "trace", 0, 0, Derived, Slot, "")                                                           \
	X(TraceGetPointContentsWorld, "IEngineTrace", "engine", "trace", 1, 1, Derived, Slot, "")                                                      \
	X(TracePointOutsideWorld, "IEngineTrace", "engine", "trace", 19, 19, ProtonVerified, Slot, "SAR's TraceRay (5) + 14; called with no map loaded it prints the engine's leaf-out-of-range warning (its own text), so it is the right function")                                  \
	/* IStudioRender / IVModelRender */                                                                                                                 \
	X(StudioSetAmbient4, "IStudioRender", "studiorender", "studio", 19, 20, Derived, Slot, "SetAmbientLightColors(Vector4D[6]) is overloaded with the Vector[6] form: MSVC lists them reversed") \
	X(StudioSetLocalLights, "IStudioRender", "studiorender", "studio", 21, 21, Derived, Slot, "")                                                  \
	X(StudioDrawModel, "IStudioRender", "studiorender", "studio", 28, 28, Derived, Slot, "")                                                       \
	X(ModelComputeLightingState, "IVModelRender", "engine", "modelrender", 26, 26, Derived, Slot, "")                                              \
	/* IVRenderView */                                                                                                                                  \
	X(RvFogVolume, "IVRenderView", "engine", "renderview", 10, 10, Derived, Slot, "GetVisibleFogVolume")                                           \
	X(RvDrawWorldLists, "IVRenderView", "engine", "renderview", 13, 13, Derived, Slot, "")                                                         \
	X(RvDrawTranslucentSurfaces, "IVRenderView", "engine", "renderview", 19, 19, Derived, Slot, "")                                                \
	X(RvViewSetupVisEx, "IVRenderView", "engine", "renderview", 43, 44, Derived, Slot, "Push3DView is overloaded earlier in the interface")      \
	/* IServerGameClients */                                                                                                                            \
	X(GcClientSetupVisibility, "IServerGameClients", "server", "gameclients", 9, 9, Derived, Slot, "")                                              \
	/* CGameMovement */                                                                                                                                 \
	X(MoveTracePlayerBBox, "CGameMovement", "server", "movement", 14, 15, ProtonVerified, Slot, "Windows 15, not the derived 13: found with pcb_vtdump (the only slot near there with ret 20, 5 args, 201 bytes; 12-14 are getters); same on client.dll; the hook at 15 runs the movement tick under Proton (13 crashed the game)") \
	X(MoveFinishGravity, "CGameMovement", "server", "movement", 35, 34, Sar, Slot, "SAR Offsets::FinishGravity")                                  \
	X(MoveCheckJumpButton, "CGameMovement", "server", "movement", 37, 36, Sar, Slot, "SAR Offsets::CheckJumpButton")                               \
	X(MoveSetGroundEntity, "CGameMovement", "server", "movement", 70, 69, Sar, Slot, "SAR Offsets::SetGroundEntity")                               \
	/* IVEngineEffects */                                                                                                                               \
	X(EfxAllocDlight, "IVEfx", "engine", "efx", 4, 4, Derived, Slot, "")                                                                           \
	X(EfxAllocElight, "IVEfx", "engine", "efx", 5, 5, Derived, Slot, "")                                                                           \
	X(EfxGetActiveDLights, "IVEfx", "engine", "efx", 6, 6, Derived, Slot, "")                                                                      \
	/* entities, client and server */                                                                                                                   \
	X(EntListGetClientNetworkable, "IClientEntityList", "client", "entlist", 0, 0, Derived, Slot, "")                                              \
	X(NetIsDormant, "IClientNetworkable", "client", "net_dormant", 9, 9, WindowsVerified, Slot, "Portal 2 only (the Alien Swarm header has it at 8); Windows 2026-10-12: the probe after a map load shows it tests the entity index field and returns a flag")  \
	X(NetEntIndex, "IClientNetworkable", "client", "net_dormant", 10, 10, WindowsVerified, Slot, "Portal 2 only (the Alien Swarm header has it at 9); Windows 2026-10-12: the probe after a map load shows it returns that same index field") \
	X(RenderableGetRenderOrigin, "IClientRenderable", "client", "renderable", 1, 1, Derived, Slot, "")                                             \
	X(RenderableDrawModel, "IClientRenderable", "client", "renderable", 9, 9, Derived, Slot, "follows GetModel (SAR 8 on both systems)")           \
	X(RenderableOffsetFromNet, "C_BaseEntity", "client", "renderable", -4, -4, Derived, Offset, "bytes from the IClientNetworkable sub-object to IClientRenderable: same inheritance order under MSVC and GCC") \
	X(ServerGetRefEHandle, "IServerUnknown", "server", "serverent", 3, 2, Derived, Slot, "IHandleEntity has a virtual destructor: one slot on MSVC")  \
	X(ServerGetNetworkable, "IServerUnknown", "server", "serverent", 5, 4, Derived, Slot, "")                                                      \
	X(ServerNetGetEdict, "IServerNetworkable", "server", "serverent", 2, 2, Derived, Slot, "")                                                     \
	X(ServerNetGetClassName, "IServerNetworkable", "server", "serverent", 3, 3, Derived, Slot, "")

enum Id : int {
#define PCB_X(Name, iface, module, group, lin, win, basis, kind, note) k##Name,
	PCB_SLOT_TABLE(PCB_X)
#undef PCB_X
	kCount
};

struct Entry {
	const char *name, *iface, *module, *group;
	int linux_, win;
	Basis basis;
	Kind kind;
	const char *note;
};

const Entry &Info(Id id);
// The number for this operating system with any user override applied. Does not check the policy.
int Index(Id id);
// True when the entry may be used: always on Linux; on Windows when it is Sar/Derived, overridden, or enabled by the safe-load policy.
bool Usable(Id id);
// Same as Usable but also counts the skip for the log (once per entry): call it where the code would otherwise use the slot.
bool Use(Id id);
// Windows: one summary of what is held back (logged at load). Linux: nothing.
void LogPolicy();
// true when PCB_WIN_UNVERIFIED / win_unverified.txt asked for everything.
bool EnabledAll();
// Number of entries by basis (probe summary).
int Count(Basis b);

// ---- call helpers ---------------------------------------------------------------------------------------------------------------
// Original function pointer of a slot, nullptr when the entry is not usable on this system.
template <typename T>
T Orig(Interface *iface, Id id) {
	return (iface && Use(id)) ? iface->Original<T>(Index(id)) : (T) nullptr;
}
// Interface::Hook that respects the policy: false (and a log line) when the slot is held back.
template <typename D, typename O>
bool Hook(Interface *iface, Id id, D detour, O &original) {
	if (!iface || !Use(id)) return false;
	return iface->Hook(detour, original, Index(id));
}
// Function pointer from an object's vtable, nullptr when held back.
template <typename T>
T Vtbl(void *obj, Id id) {
	return (obj && Use(id)) ? reinterpret_cast<T>((*reinterpret_cast<void ***>(obj))[Index(id)]) : (T) nullptr;
}
// Calls virtual method `id` of obj. A held-back slot returns a zero value and does nothing.
template <typename R, typename... A>
R VCall(void *obj, Id id, A... a) {
	auto f = Vtbl<R(__rescall *)(void *, A...)>(obj, id);
	if (!f) return R();
	return f(obj, a...);
}
}  // namespace bridge::slots
