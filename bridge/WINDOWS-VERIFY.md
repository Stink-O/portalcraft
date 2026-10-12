# Windows port: what can be checked without the Windows game files

Written 2026-10-11 by the "verify" agent. Source edits: none. Nothing was launched (no game, Steam, Proton or Wine). Updated as the other
agents' code lands; the "as of" line at the end of each section says what was read.

Method for the literal vtable slots (section 2): Valve's public Alien Swarm SDK headers (github.com/Source-SDK-Archives/source-sdk-alien-swarm,
read only, nothing copied into the repo) declare most of the interfaces Portal 2 uses under the same version strings (VClient016,
VEngineServer022, EngineTraceServer004, VEngineModel016, VStudioRender026, VEngineRenderView013, VEngineEffects001, GameMovement001,
ServerGameClients004, VSERVERTOOLS001, VClientEntityList003 ...). A small script read the declaration order and computed the Linux (GCC) slot,
then the Windows (MSVC) slot. The Linux result reproduced the bridge's own Linux numbers (the proof that the header matches Portal 2 for that
interface). The two MSVC layout rules were checked against 14 pairs of Windows/Linux numbers that upstream SourceAutoRecord found by
disassembly, all reproduced:
- a virtual destructor takes 1 slot on MSVC, 2 on GCC (every class with a destructor: Windows number is one lower after it);
- overloaded virtuals of one class sit together on MSVC, at the place of the FIRST declaration of that name in the class body (an override
  redeclared in the class counts), in reverse declaration order. Checked on: IServerTools::SetKeyValue (SAR 13/12 Windows/Linux, 12/13, 11/14),
  IVDebugOverlay::ScreenPosition (12/11), CGameMovement (PlayerMove 17/16, AirAccelerate 24/23, AirMove 25/24, FinishGravity 34/35,
  CheckJumpButton 36/37, TryPlayerMove 39/40, GetPlayerViewOffset 8/9, ProcessMovement 1/2), IClientMode (CreateMove 24/25, OverrideView 18/19).
This is derived, not disassembled. Slots marked "derived" must still be confirmed by the probe on the first run (section 6).

## 0. Summary

| item | count | note |
|---|---:|---|
| SAR table entries the bridge uses (`Offsets::`) | 45 | all 45 are byte-identical to upstream's current table |
| ... of which differ from upstream | 0 | |
| ... of which not in upstream | 0 | |
| ... of which upstream uses differently on Windows than we do | 1 | `GetPaintPower` (finding F3) |
| literal vtable slots (distinct interface+method) | 61 | |
| ... derived unchanged on Windows | 36 | Linux value reproduced by the SDK header, no MSVC reordering reaches them |
| ... derived DIFFERENT on Windows (bridge would call the wrong function) | 14 | listed in F5, plus F4 (IMesh::Draw) |
| ... Portal 2 only (not in any public header), not derivable | 11 | probe on the first run |
| ABI faults that crash on the first call (not slot numbers) | 3 | F1, F2, F4 |
| Java SkyLink Windows backend | missing | F7 |
| DLL: exports / imports | pass | section 4 |
| protocol layout (373 constants) under i686 MinGW | pass | section 3 |

Upstream facts. Upstream SourceAutoRecord master (b977710, 2026-10-10) still has one table for Portal 2 Steam builds, `Portal 2 10090.hpp`
(header comment "Updated from 9568 with no visible changes"); `Games/Portal2.cpp` reports "Portal 2 (10090)" for every Steam build, so
"10090" is a table name, not a version check. Our copy equals upstream's file except that upstream added one signature on 2026-10-08
(`OnGSClientDenyHelper`, commit 6a56592, `sar_prevent_steam_logon_disconnect`), which the bridge does not use. The Steam build here is
buildid 25240193 (`appmanifest_620.acf`, `portal2/steam.inf` PatchVersion=2.0.0.1); the Linux table works against it, so the table is current for
Linux. Windows engine files cannot be compared (not downloaded); upstream runs the same table on Windows today.

## 1. Findings the Windows port agent must fix (precise)

Severity: C = crashes or corrupts on the first call; W = calls the wrong function; L = loses data or a feature.

**F1 (C) `src/bridge/terrain.cpp:68-76`: `Q_AddRef`, `Q_Release`, `Q_Run` are `__attribute__((cdecl))`.** The engine calls a functor's methods as
member functions: `this` arrives in ECX on Windows and the callee pops its arguments. A cdecl function reads `this` from the stack and
returns without popping. Make them `__rescall` (= `__thiscall` on Windows, already defined in `Utils/Platform.hpp`). On Linux `__rescall` is
cdecl, so one spelling serves both. The hand-made vtable shape is fine on MSVC (AddRef, Release, operator() lands in slot 2; the extra Q_Run entries
are harmless).

**F2 (C) struct return through thiscall, two call sites: `src/bridge/bridge.hpp:30` (`_GetLightForPoint`, returns `Vector`) and
`src/bridge/composite.cpp:450` (`_RcGetTM`, returns `Vector`).** Checked by compiling a test in the project's own container
(`localhost/pcb-mingw32`, GCC 10 i686-w64-mingw32): GCC passes the hidden return pointer in ECX and puts `this` on the stack for a thiscall
function pointer that returns a struct. MSVC (the engine) takes `this` in ECX and the return pointer as the first stack argument, callee
pops (`ret 8`). Declare both as `Vector *(__rescall *)(void *thisptr, Vector *ret, ...)` and call with a local `Vector`; the explicit
parameter reproduces the MSVC layout (the same text is valid on Linux cdecl, where the hidden pointer is also the first stack argument, but keep
`#ifdef _WIN32` if that is easier to leave Linux untouched). No other `Vector`/struct-returning thiscall pointers exist (searched `bridge/` and
`sar/`). Member functions defined in our own classes are thiscall by default under MinGW (checked: `ret $8`, `this` in ECX), so the plugin's
`IServerPluginCallbacks` and trace-filter classes are fine.

**F3 (W) `src/bridge/mcmechanics.cpp:702-717` (`ReadPaint`): on Windows `this` for `GetPaintPower` is `player + 0x1250`, not `player`.**
Upstream `src/Features/Tas/TasPlayer.cpp` (~line 410) has `#ifdef _WIN32 paintPowerUser = player + 0x1250 #else player #endif` ("windows being
weird"), then calls `Offsets::GetPaintPower` (2 on Windows, 513 on Linux) on that pointer. With `player` the call lands on slot 2 of the
player's own vtable. The Windows value 0x1250 is from upstream (MIT) and is Portal 2 Steam specific.

**F4 (C) `src/sar/Utils/SDK/MeshBuilder.hpp` (`IMesh`) and `MeshBuilder.cpp:221` (`mesh->Draw()`).** `IMesh` declares `Draw(int,int)` and
`Draw(CPrimList*,int)` as overloads. MSVC (the engine) lays them `[SetPrimitiveType 10, Draw(lists) 11, Draw(first,count) 12, SetColorMesh 13]`;
GCC lays them in declaration order (Draw(first,count) = 11). Upstream compiles this header with MSVC, so it is right there; our MinGW build is
not. Under `#ifdef _WIN32` declare `Draw(void *lists, int nlists)` before `Draw(int first, int count)` in `IMesh` (the other IMesh slots
used, `SetPrimitiveType` 10, `LockMesh` 17, `UnlockMesh` 20, keep their numbers). Without this every block mesh calls the wrong function.

**F5 (W) literal slots whose Windows number differs** (all in the table of section 2; move them into `slots.hpp` with a Windows column):
- `ents.cpp:56,69,70,84,85,265` and `portals.cpp:50,65,66`: IServerTools `GetKeyValue` 11 -> 10, `FirstEntity` 8 -> 7, `NextEntity` 9 -> 8,
  `SnapPlayerToPosition` 3 -> 2 (IBaseInterface's destructor). `collider.cpp` already uses `Offsets::` (table: 14/15/13), fine.
- `blockcollide.cpp:36,662,783`: `TracePlayerBBox` 14 -> 13; `SetGroundEntity` 70 -> 69, `CheckJumpButton` 37 -> 36, `FinishGravity` 35 -> 34
  (these three are SAR table entries; use `Offsets::SetGroundEntity/CheckJumpButton/FinishGravity` instead of literals).
- `visclamp.cpp:58`: `ViewSetupVisEx` 43 -> 44 (IVRenderView overloads `Push3DView`); 13, 19 and 10 stay.
- `mclight.cpp:48`: `SetAmbientLightColors(Vector4D[6])` 19 -> 20 (IStudioRender overloads it with the `Vector[6]` form; 21 and 28 stay).
- `composite.cpp:439`, `digrender.cpp:52`, `render_backend/gl_dig.cpp:66`, `render_backend/cut_logic.cpp:141`: IMatRenderContext
  `GetMatrix(mode, VMatrix*)` 29 -> 30 (overloaded with the 3x4 form). Everything else the bridge calls on the render context up to 75 is unchanged.
- `stageprops.cpp:35,63`, `gl_dig.cpp`, `kServerGetRefEHandle = 3`: `GetRefEHandle` 3 -> 2 and `IServerUnknown::GetNetworkable` 5 -> 4.

**F6 (L) narrow file APIs with UTF-8 paths.** About 45 `std::fopen`/`::stat`/`std::ofstream` calls build paths from `CacheDir()` (plugin.cpp:286
opens `bridge.log`, :374 `progress.txt`, `mc.cpp`, `bugmark.cpp`, `movers.cpp`, `collfield.cpp`, ...). `OwnModulePath()` returns UTF-8 (wide APIs
inside), but msvcrt's `fopen`/`stat` read ANSI, so a data folder under a user name with non-ASCII letters (`%LOCALAPPDATA%\Portalcraft`) cannot be opened:
no `bridge.log`, so the launcher sees nothing. Either route them through a `plat::Fopen`/`plat::Stat` (UTF-8 to `_wfopen`/`_wstat`) or force-include a header
that `#define`s `fopen`/`stat` to such wrappers under `_WIN32`.

**F7 (blocking for linking) the Java side has no Windows `SkyLink`.** `fabric/src/main/java/dev/skycraft/link/SkyLink.java` (1124 lines) maps
`/dev/shm` only; `Proto.java:14` has `MAPPING_NAME = "/Portalcraft_v1"`. The host will create `Local\Portalcraft_v1` and the mod will not open it.
Restore from `~/SkyCraft/fabric/.../SkyLink.java` (`OpenFileMappingW` with `captureCallState("GetLastError")`, `MapViewOfFile`, `CreateMutexW`).
Detail in section 3. Not a plugin bug, listed because the launcher's "linked" state depends on it.

**F8 (minor)**
- `sar/Hook.hpp`: `bool locked` has no initialiser. Fine while every `Hook` is a static object (zero-initialised); a `Hook` on the stack or heap reads garbage.
- `sar/Utils/Memory.cpp`: module enumeration uses `GetModuleFileNameA` then `GetModuleHandleA(path)`; ANSI round trip. Harmless for an ASCII Steam path, fails
  for a Steam library path with characters outside the system code page. Use the `W` pair.
- `platform_win.cpp` `ProcessCommandLine` is empty, so `pc_world auto` cannot read the linked Minecraft's world; the launcher writes `pc_world unified`
  explicitly (`autoexec.rs`), so the shipped flow is unaffected.
- Dev mode only: with `PCB_LINK_FILE` the log line is `link: <path> (PCB_LINK_FILE) created`, which the launcher's matcher (`play.rs`:
  requires `Portalcraft_v1 created`) does not recognise. Irrelevant for real Windows.

Checked and fine: thiscall hooks with `-mstackrealign`, `DrawTranslucentRenderables`/`DrawOpaqueRenderables` argument counts (2 and 3, equal to upstream's
detours, so the callee-pops-arguments difference does not bite), `TraceFirePortal` (5 arguments + `this`, equals upstream), `ITextureRegenerator`/trace filter/plugin
vtables (no destructor, no overloads), `ISERVERPLUGINCALLBACKS002` (same string upstream), `tier0`/`vstdlib` module names and the MSVC-mangled `ConColorMsg`,
`DevMsg`, `DevWarning` names (identical to upstream's `Console.hpp`; `Msg`/`Warning` are plain exports upstream too).

## 2. Engine entries

### 2a. The 45 SAR table entries (all identical to upstream's current Portal 2 10090 table; "usage" = how upstream uses it on Windows)

| entry | Windows | Linux | usage vs ours |
|---|---:|---:|---|
| CreateInterfaceInternal / s_pInterfaceRegs | 5 / 6 | 5 / 11 | same (Interface.cpp identical to upstream) |
| RegisterConCommand, UnregisterConCommand, FindCommandBase | 9, 10, 13 | same | same |
| GetScreenSize, ClientCmd, GetViewAngles, SetViewAngles | 5, 7, 18, 19 | same | same |
| ExecuteClientCmd, IsPaused, Con_IsVisible, GetLightForPoint | 104, 86, 11, 1 | same | same (but see F2 for GetLightForPoint's return) |
| HudProcessInput, GetClientMode, g_pClientMode | 10, 4, 19 | 10, 9, 20 | same chain as `Client.cpp`; `readJmp` is only for Beginner's Guide / Stanley Parable, not Portal 2 |
| CreateMove, OverrideView | 24, 18 | 25, 19 | same |
| ProcessMovement | 1 | 2 | same |
| AcceptInput, S_GetDataDescMap | 40, 11 | 41, 12 | same |
| m_pShadowStand / m_pShadowCrouch | 3160 / 3164 | 3184 / 3188 | same |
| GetPaintPower | 2 | 513 | **differs in use: F3** |
| Paint (CEngineVGui) | 14 | 15 | same |
| TraceRay (+14 = PointOutsideWorld) | 5 | 5 | same |
| CreateEntityByName, DispatchSpawn, SetKeyValueChar | 14, 15, 13 | 15, 16, 12 | same |
| IsCollisionEnabled, EnableGravity | 6, 13 | 7, 14 | same (`EnableGravity - 1` in blockcollide.cpp:860 is the bridge's own, see the check below) |
| CreateMaterial, FindMaterial, CreateProceduralTexture | 81, 82, 91 | same | same (upstream uses `Current()`, we `Original()`, equal because nothing is hooked) |
| RemoveMaterial, DecrementReferenceCount | 156, 13 | 157, 13 | same |
| g_pTextureManager (code offset), RemoveTexture | 8, 30 | 4, 30 | same (`DerefDeref(CreateProceduralTexture + off)`) |
| GetRenderContext | 113 | 113 | same |
| ImageData, ImageFormat (IVTFTexture) | 39, 20 | 38, 21 | same |
| SIGSCAN DrawTranslucentRenderables, DrawOpaqueRenderables, KeyValues_SetString, TraceFirePortal | Windows pattern | Linux pattern | same patterns, same use |

Check on `EnableGravity - 1` (blockcollide.cpp:860): the bridge calls slot `EnableGravity - 1` as `EnableCollisions` (IPhysicsObject). The two are
adjacent in `vphysics_interface.h` (`EnableCollisions` then `EnableGravity`, a destructor before both), so the same subtraction holds on MSVC.
IPhysicsObject has no overloaded virtuals there.

Not table entries but part of the same file and relevant to "how Windows builds are found": the Windows patterns for the three inline hooks begin
`55 8B EC 81 EC 80 00 00 00` (Translucent), `55 8B EC 83 EC 54 83 7D 0C 00` (Opaque), `53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC`
(TraceFirePortal). `Hook.hpp` overwrites 5 bytes at entry and restores them before calling the original, so cutting the 6-byte `sub esp,0x80`
or the 6-byte `53 8B DC 83 EC 08` prologue is safe. Not safe: another thread inside the function while it is un-hooked (existing design on Linux too).

### 2b. Literal vtable slots (61). Source for every row: the Alien Swarm SDK header order plus the two MSVC rules above. "SAR" = documented by upstream.

| interface::method | bridge file | Linux | Windows | status |
|---|---|---:|---:|---|
| CGameMovement::FinishGravity / CheckJumpButton / SetGroundEntity | blockcollide.cpp | 35 / 37 / 70 | 34 / 36 / 69 | SAR documents these three (`Offsets::`) |
| CGameMovement::TracePlayerBBox | blockcollide.cpp | 14 | 13 | derived (header reproduces 14) |
| IServerTools::GetKeyValue / FirstEntity / NextEntity / SnapPlayerToPosition | ents.cpp, portals.cpp | 11 / 8 / 9 / 3 | 10 / 7 / 8 / 2 | derived (header reproduces the Linux numbers; the SetKeyValue/CreateEntityByName table values on Windows agree with the same arithmetic) |
| IServerUnknown::GetRefEHandle / GetNetworkable | stageprops.cpp, gl_dig.cpp | 3 / 5 | 2 / 4 | derived |
| IServerNetworkable::GetEdict / GetClassName | stageprops.cpp | 2 / 3 | 2 / 3 | derived, unchanged |
| IVRenderView::ViewSetupVisEx | visclamp.cpp | 43 | 44 | derived (Push3DView overload pair) |
| IVRenderView::DrawWorldLists / DrawTranslucentSurfaces / GetVisibleFogVolume | visclamp.cpp | 13 / 19 / 10 | 13 / 19 / 10 | derived, unchanged |
| IStudioRender::SetAmbientLightColors(Vector4D[6]) | mclight.cpp | 19 | 20 | derived |
| IStudioRender::SetLocalLights / DrawModel | mclight.cpp | 21 / 28 | 21 / 28 | derived, unchanged |
| IVModelRender::ComputeLightingState | mclight.cpp, bandlight.cpp | 26 | 26 | derived, unchanged |
| IVEfx (CL_AllocDlight / CL_AllocElight / GetActiveDlights) | blocklights.cpp | 4 / 5 / 6 | 4 / 5 / 6 | derived, unchanged |
| IEngineTrace GetPointContents / GetPointContents_WorldOnly / (TraceRay) / PointOutsideWorld | engine.cpp, fluids.cpp | 0 / 1 / 5 / 19 | same | derived, unchanged (TraceRay also SAR) |
| IVEngineServer GetClusterForOrigin / GetPVSForCluster / AddOriginToPVS / ResetPVS / GetArea | cutvis.cpp, gl_dig.cpp, cut_logic.cpp | 12 / 13 / 59 / 58 / 64 | same | derived, unchanged |
| IServerGameClients::ClientSetupVisibility | cutvis.cpp | 9 | 9 | derived, unchanged |
| IClientEntityList::GetClientNetworkable | stageprops.cpp, gl_dig.cpp | 0 | 0 | derived, unchanged |
| IClientRenderable GetRenderOrigin / DrawModel | stageprops.cpp | 1 / 9 | 1 / 9 | derived, unchanged |
| IMaterial::FindVar | composite.cpp:608 | 11 | 11 | derived, unchanged |
| IMatRenderContext AddRef/Release/BeginRender/EndRender/Flush/ReadPixels/MatrixMode/Push/Pop/LoadIdentity | composite.cpp, digrender.cpp, gl_dig.cpp | 0..3, 4, 13, 20, 21, 22, 31 | same | derived, unchanged |
| IMatRenderContext::GetMatrix(VMatrix*) | composite.cpp, digrender.cpp, gl_dig.cpp, cut_logic.cpp | 29 | 30 | derived |
| IMatRenderContext CreateStaticMesh / DestroyStaticMesh / GetDynamicMesh / OverrideDepthEnable | terrain.cpp, MeshBuilder.hpp | 55 / 56 / 57 / 75 | same | derived, unchanged |
| IMesh::Draw(int,int) via the C++ class | MeshBuilder.cpp:221 | 11 | 12 | derived; F4 |
| IMesh SetPrimitiveType / LockMesh / UnlockMesh via the C++ class | MeshBuilder.cpp | 10 / 17 / 20 | same | derived, unchanged |
| VEngineClient015::ComputeLighting | engine.cpp:20 | 69 | probably 69 | Portal 2 only (ASW VEngineClient013 has it at 68); no MSVC reordering in IVEngineClient before it; probe |
| IClientNetworkable IsDormant / entindex | stageprops.cpp, gl_dig.cpp | 9 / 10 | probably 9 / 10 | Portal 2 only (ASW has 8 / 9); probe |
| IMatRenderContext SetStencilState / PushCustomClipPlane / PopCustomClipPlane | digrender.cpp | 119 / 122 / 123 | probably same | Portal 2 only past slot ~100; probe |
| IMatRenderContext::GetCallQueue | terrain.cpp:83 | 142 | probably 142 | Portal 2 only; probe |
| IMatRenderContext Set/GetToneMappingScaleLinear | composite.cpp:441 | 145 / 146 | probably same | Portal 2 only; probe |
| ICallQueue::QueueFunctor, CFunctor shape | terrain.cpp:98-106 | 0 / AddRef,Release,operator() | 0 / same | ASW `callqueue.h`/`functors.h`; MSVC drops the destructor slot only |

Sources of other open-source Source work that would document these: SourceMod's gamedata (alliedmodders/sourcemod, `gamedata/*.txt`) lists
Windows/Linux slots for entities of other games; it does not contain Portal 2's interfaces. SAR upstream is the only open source with
Portal 2 Windows numbers. The CS:GO/TF2 repositories that GitHub search returns for `IMatRenderContext` are leaked Valve source; not used.

### 2c. Windows-specific facts in upstream we do not yet use (so nothing to copy now)
- `RenderContextSize` signature patch (`MaterialSystem.cpp`, 6.45 MB render context): needed by SAR's overlay renderer, not by the bridge.
- Upstream SAR Windows crash-handler change `1165a3d` ("workaround wine bug in CrashHandler"): relevant to the Proton run only if the bridge installs a similar
  handler; the bridge uses its own vectored handler (`platform_win.cpp`), which does not use SAR's code.

## 3. Shared-memory link

Compared: `src/bridge/platform/platform_win.cpp` (ShmCreate, SharedWithThisUser, Elevated, MonotonicNs), `src/bridge/link.cpp`, `protocol/skycraft_protocol.h`,
`fabric/.../link/SkyLink.java` + `Proto.java`, against `~/SkyCraft/skse/src/Link.cpp` and `~/SkyCraft/fabric/.../SkyLink.java`.

| aspect | host (C++) | SkyCraft | Java (Portalcraft) | verdict |
|---|---|---|---|---|
| name | `Local\Portalcraft_v1` (`kMappingName`) | `Local\SkyCraft_v1` | `/Portalcraft_v1` | **mismatch: Java Linux-only (F7)** |
| API | `CreateFileMappingW(INVALID_HANDLE_VALUE, sa, PAGE_READWRITE, hi, lo, name)`, `MapViewOfFile(FILE_MAP_ALL_ACCESS,0,0,0)` | same | `Files`/`FileChannel` on `/dev/shm` | host matches SkyCraft |
| security descriptor | `D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;<user SID>)S:(ML;;NW;;;ME)` | identical | n/a | match |
| existing object | `g_existing = ERROR_ALREADY_EXISTS`; link.cpp clears header + rings and writes the new pid | same | relinks on pid change | match |
| size | `kMappingBytes` ~220 MB, whole view | same idea | maps whole | see below |
| clocks | host `QueryPerformanceCounter` -> ns (integer arithmetic, no overflow) | `GetTickCount64` ms in SkyCraft's `skyrimHeartbeatMs` | `System.nanoTime()` (QPC on Windows), `tickCount = nanoTime/1e6` | **consistent**: both sides QPC; SkyCraft's GetTickCount64 (10-16 ms) must not be copied |
| "Minecraft already running" | none yet | named mutex `<name>_minecraft` (`CreateMutexW` in Java, `OpenMutex` in C++) | flock on `/dev/shm/..._minecraft` | **gap**: no Windows mutex, host or Java; only used for the launcher's "already running" hint, no effect on linking |
| 32/64-bit layout | 373 layout constants compiled as `static_assert` under i686-w64-mingw32 (container `pcb-mingw32`, GCC 10): all equal the `-m32` Linux values that `tools/layout_test.sh` compares to `Proto.java` | n/a | `Proto.java` offsets | **pass** (`double`/`uint64` members are padded explicitly; no `long`, pointers or `bool` in shared structs) |
| 32-bit address space | one 220 MB contiguous view, error text includes `AddressSpaceSummary()` | n/a | 64-bit JVM | design risk, logged on failure; `-Wl,--large-address-aware` is set in the DLL (Characteristics 0x2326) |
| log line | `[bridge] link: Local\Portalcraft_v1 created, <N> MB, host pid <pid>` | | | launcher `play.rs:159-160` matches (`link: ` and `Portalcraft_v1 created`) |

As of: platform_win.cpp 02:05, link.cpp, Java unchanged since the Linux port.

## 4. The DLL (`build/win/portalcraft_bridge.stripped.dll`, built 03:38; PE32, i386)

- Exports: exactly one, `CreateInterface` (undecorated, `--kill-at`). Pass.
- Imports (the whole list): `KERNEL32.dll` (81 functions, includes `K32EnumProcessModules`/`K32GetModuleInformation`/`K32GetProcessMemoryInfo`, Windows 7+),
  `msvcrt.dll` (96), `USER32.dll` (13: window enumeration, `GetAsyncKeyState`, `GetCursorPos`, `CreateWindowExA`), `ADVAPI32.dll` (4: token, SDDL).
  No `libgcc`/`libstdc++`/`libwinpthread`, no `psapi.dll`, no `d3d9.dll`, no tier0/vstdlib (all resolved at run time by name). Pass.
- Flags: `LARGE_ADDRESS_AWARE`, `DYNAMIC_BASE` + `NX_COMPAT` (DllCharacteristics 0x140), `.reloc` present (0x17db8), `.tls` 8 bytes. ImageBase 0x6d400000.
- Compiler flags (`Makefile.win`): `-mstackrealign`, `-mno-ms-bitfields`, `-static -static-libgcc -static-libstdc++`, `-Wl,--kill-at`. Member functions default to thiscall in
  MinGW (verified), free hooks use `__rescall` = `__thiscall` (callee pops; verified `ret $N` in a test).
- Not caught by this check: F1/F2/F4 (they are ABI faults inside correct-looking code).

## 5. Launcher contract (launcher/src vs the plugin)

| launcher expects | plugin does | gap |
|---|---|---|
| `plugin_load "C:/.../plugin/portalcraft_bridge.dll"` (forward slashes, `autoexec.rs:33`), `pc_start_mode mc`, `pc_world unified`, `map ...` | the plugin registers those commands; `plugin_knows_world(&so)` scans the file for the command text, ASCII in `.rdata` | none |
| data root found from the DLL's own path, marker `.portalcraft-root` | `OwnModulePath()` via `GetModuleHandleExW` + `GetModuleFileNameW` | non-ASCII data path: F6 |
| `<data>/cache/bridge/bridge.log` | opened `"a"` with the narrow path | F6; text-mode `\r\n` is fine (launcher splits on `\n`, trims) |
| `[bridge] link: ... Portalcraft_v1 created, ... host pid <pid>` and pid == Portal 2's pid | `plat::ProcessId()` = `GetCurrentProcessId()` | none |
| `[bridge] level init <map>` | `plugin.cpp:371` | none |
| `[bridge] link up (Minecraft pid` | mod connected (`mc.cpp`/`link.cpp`) | needs F7 |
| `progress.txt` = `<map>\n<unix seconds>` | `plugin.cpp:374`, `%lld` with `__USE_MINGW_ANSI_STDIO`, CRLF in text mode | launcher uses `lines()` and `trim()`: fine |
| `[bridge] unloading` | exists | none |
| stale mapping cleanup at launch (`/dev/shm/Portalcraft_v1`, play.rs:259) | Windows mapping dies with its last handle | none |

## 6. First-run checklist (when the Windows depot exists)

Prerequisites: a separate copy of the Windows Portal 2 (app 620) downloaded with SteamCMD into a folder outside your Steam library (for example `~/Portal2-Windows`, with `steam_appid.txt` = 620), and a small script that starts it with Proton (`proton run ./portal2.exe -game portal2 -steam ...`, prefix on a Linux filesystem).
Build with `bridge/tools/build_windows_plugin.sh`. `pcb_probe_slots` does not exist yet (PLAN-windows phase 1); until it does, use the substitutes marked (S).

### 6a. Under Proton (`~/portalcraft-wintest/2-run-windows-portal2-proton.sh -novid -windowed -w 1600 -h 900 -console +plugin_load <path>`)
Steam running first. The prefix must stay on btrfs (the script sets `~/portalcraft-wintest/prefix`). Native Linux Minecraft cannot see a `Local\` mapping: set
`PCB_LINK_FILE=<file>` for a link test. Read `cache/bridge/bridge.log` (the plugin's own copy; also the game console).

1. Load: first line after `plugin_load`: the plugin banner and no "could not find tier0/vstdlib". If `ConColorMsg` or `DevMsg` is not found, the mangled names
   (section 1, "Checked and fine") are wrong for this build: `x86_64-w64-mingw32-objdump -p tier0.dll | rg ConColorMsg` shows the real export.
2. Interfaces: every `Interface::Create` resolves (VEngineClient015, VEngineServer022, EngineTraceServer004/Client004, VStudioRender026, VEngineModel016,
   VEngineRenderView013, VEngineEffects001, VEngineVGui001, GameMovement001 x2, VSERVERTOOLS001, ServerGameClients004, VClientEntityList003, VMaterialSystem080,
   VEngineCvar007). A missing one means a different engine build than the table: compare `portal2/steam.inf` PatchVersion (here 2.0.0.1) and the exe's product version
   (S: `engine.dll` file version in the Windows depot).
3. Signature hooks: three lines `view hook installed at client.dll+0x...`, `opaque-pass hook installed at ...`, and TraceFirePortal's. Each "signature not found" is a
   table/build mismatch; the three Windows patterns in section 2a are SAR's.
4. `ReadPaint` (F3): no "paint power record ... reads state" warnings while standing in a gel map.
5. Crash markers: bridge.log lines starting `EXCEPTION 0x...` followed by `module+offset`. The first thing to try for a crash inside our hook entry is F1/F2.
6. Link (dev): `link: <file> (PCB_LINK_FILE) created, 218 MB, host pid` (launcher string match fails on this variant: expected).
7. Direct3D 9: `pcb_d3d9_init` then `pcb_d3d9_stats`: "device vtable ... (dxvk d3d9?)" and a rising Present counter; `pcb_d3d9_overlay 1` shows the red
   rectangle. Under Proton this is DXVK, so it proves the hook mechanics, not the native driver.
8. Slot probe (S, until `pcb_probe_slots` exists): for each of the 14 "differs" rows in 2b, call the command or hook that uses it and check the effect:
   `pcb_probe 0 0 0` (PointOutsideWorld), a block at the chamber (MeshBuilder.Draw, F4), `pcb_stage` (IServerTools slots), a portal view (ViewSetupVisEx 44),
   F5 tone/ambient lighting (mclight). A wrong slot shows as an immediate crash with the callee address in the `EXCEPTION` line.
9. Run three `map sp_a1_intro1` / quit cycles; `progress.txt` appears after `level init`.

### 6b. Real Windows
Same steps with the launcher: Defender may quarantine the unsigned DLL (check "Protection history" first if `plugin_load` says "Unable to load plugin"); run as the
same user level as Minecraft or accept the elevated-mapping SDDL path (`link: Portal 2 is running as administrator` is logged). Native-only checks:
D3D9 under Microsoft's `d3d9.dll` (`pcb_d3d9_stats`, device vtable address in `d3d9.dll`), `mat_dxlevel` output, fullscreen/alt-tab (Reset), Steam overlay ordering.
What to send back from either run: `bridge.log`, `mc.log`, the first 100 lines of console, `pcb_d3d9_stats`, GPU name.

## 7. Blocked or open

- Everything marked "derived" or "probably" in 2b is blocked on the Windows `engine.dll`, `materialsystem.dll`, `client.dll`, `server.dll`, `shaderapidx9.dll`
  (not downloaded; needs the user's Steam login). `pcb_probe_slots` is not written yet.
- The 11 Portal 2-only slots cannot be derived from any public header; the probe (or a Ghidra pass) is the only way.
- Not examined: the D3D9 backend's shader-bytecode patcher (not written yet); `d3d9hook.cpp` vtable numbers for IDirect3DDevice9 were checked against
  the MinGW-w64 `d3d9.h` (Reset 16, Present 17, SetTexture 65, CreateVertexShader 91, SetVertexShader 92, CreatePixelShader 106, SetPixelShader 107,
  PresentEx 121, ResetEx 132, 119 device methods): all match; surface/texture/state-block/swap-chain numbers match public `d3d9.h` order by hand count.
- Re-check after: F1-F8 land; `slots.hpp` appears; `pcb_probe_slots` appears; the DLL is rebuilt.

## Pass 2 (2026-10-11, after the Windows port agent's F1-F8 fixes; DLL built 03:58)

Read: terrain.cpp, bridge.hpp, composite.cpp, mcmechanics.cpp, MeshBuilder.hpp, Hook.hpp, Makefile.win, slots.hpp (63 rows), platform/input_win.cpp,
fabric `WinMapping.java`/`Proto.java`/`SkyLink.java`, the rebuilt `portalcraft_bridge.stripped.dll`. Not re-done: slots.cpp, probe.cpp and the safe-load policy logic
(not read in this pass), the D3D9 backend. Nothing could be run.

### Fixes against the findings
| finding | state | evidence |
|---|---|---|
| F1 thiscall functor | fixed | terrain.cpp:68-79 `Q_AddRef/Q_Release/Q_Run` are `__rescall` |
| F2 struct return | fixed | `bridge.hpp:33` `Vector *(__rescall *)(void *, Vector *ret, ...)` under `_WIN32`; `composite.cpp:457` `_RcGetTM` takes `Vector *` |
| F3 paint power | fixed | mcmechanics.cpp:707-714 `player + 0x1250` under `_WIN32` only |
| F4 IMesh::Draw order | fixed | MeshBuilder.hpp:112-121 `Draw(lists)` declared first under `_WIN32` |
| F5 literal slots | fixed via `slots.hpp` | grep for remaining numeric slots in `bridge/*.cpp` finds none, except `render_backend/gl_dig.cpp:65 kGetHighestEntityIndex = 6` (GL-only file, not in the Windows build) |
| F6 narrow file APIs | fixed, UNVERIFIED | `Makefile.win` links `-Wl,--wrap=fopen,stat,remove,rename`; the DLL imports `_wfopen`, `_wstat`. `std::ofstream/ifstream` (colharvest, script, worldconf) go through libstdc++'s own `fopen`, which `--wrap` also redirects only if that archive member references plain `fopen`; test with a non-ASCII data folder on the first run |
| F7 Java side | fixed | `WinMapping.java` + `Proto.MAPPING_NAME` per OS (`Local\Portalcraft_v1`), named mutex `announceRunning` |
| F8 `Hook::locked` | fixed (`= false`) |

### slots.hpp Windows column vs this report
All 63 rows: every Windows number equals the value in section 2b (e.g. GetMatrix 30, ToolsSnapPlayer/First/Next/GetKeyValue 2/7/8/10, ServerGetRefEHandle 2,
ServerGetNetworkable 4, RvViewSetupVisEx 44, StudioSetAmbient4 20, MoveTracePlayerBBox 13, MoveFinishGravity/CheckJump/SetGround 34/36/69). Basis labels agree
(Sar for the three CGameMovement entries and the MeshBuilder struct slots; Unverified for the Portal 2-only ones). Differences:
- `RcReadPixels` 13 and `RcFlush` 4 are labelled Derived: correct (Alien Swarm header order, no overload before them).
- The three `RcField*` byte offsets (0x7c, 0xdc, 0x23c into `CMatRenderContext`, ambient cube, local lights, light count) are new to this pass and correctly marked
  Unverified. They are object-layout offsets, not slots; MSVC and GCC lay out the same fields identically only if there is no virtual-destructor/vptr difference
  before them. The class is single inheritance with a vptr first, so they should hold, but nothing public confirms the numbers. Probe.
- `RenderableOffsetFromNet = -4` (Derived): holds if MSVC orders the four bases of `C_BaseEntity` as GCC does (declaration order, same on both ABIs).

### Other overloaded-virtual classes the bridge calls through a C++ header
Computed with the same layout script: `IVertexBuffer`, `IIndexBuffer`, `ITexture`, `IKeyValuesSystem`, `ITextureRegenerator`: no reordering. `ICvar`:
`QueueMaterialThreadSetValue` (slots 34/36 swap), not called. `IVTFTexture`: reorders from slot 12 on (`FileSize` ...), but the bridge reaches it only through
the SAR entries `ImageData`/`ImageFormat` (39/20 Windows), which are upstream's disassembled numbers, so the reorder is already inside them. `IMesh`: only the
`Draw` pair; `Spew`, `ValidateData`, `ModifyBegin/End` are overloads of the base classes' methods (MSVC's placement of a derived overload of an inherited name
is not covered by my rule); the bridge does not call them. Residual risk: `IMesh::LockMesh` (17) and `UnlockMesh` (20) assume those derived overloads do not move.
No other class in `bridge/` or `sar/` is called through a header with overloaded virtuals.

### Input hook (`platform/input_win.cpp`)
`WH_GETMESSAGE` via `SetWindowsHookExW(..., hMod = nullptr, threadId of the game window's thread)`: valid because the procedure is in the calling process and the
thread is ours (MSDN). `GetMsgProc` is `LRESULT CALLBACK` (stdcall; with `--kill-at` fine). It acts only on `code == HC_ACTION && wp == PM_REMOVE` and only for
`m->hwnd == WinGameWindow()`, and swallows by rewriting to `WM_NULL`: correct, and `PM_NOREMOVE` peeks are untouched. Locking: `g_mx` around the filter, atomics for
the filter pointer: no lock held across `CallNextHookEx`. Remaining points:
- The hook is installed once, from the window found at that moment (`InstallHook`). If the engine recreates the window (fullscreen/windowed switch or `vid_` change, `Reset`), the hook stays on the old thread's queue only if the thread is the same; the same thread normally keeps the new window, so likely fine, but it is untested. `WinGameWindow()` is compared per message, so a new HWND is followed.
- No `WM_INPUT` handling. Source's mouse path on Windows may use raw input; Minecraft mouse look in the bridge comes from the shared memory, but swallowing `WM_MOUSEMOVE` for Minecraft screens does not stop raw input reaching the engine. Check on the first run that Chell does not turn while a Minecraft screen is open.
- `WM_CHAR` handling must deal with UTF-16 surrogate pairs (two `WM_CHAR`); I did not read lines 104-126 that far.

### Java `WinMapping`
Signatures are right for a 64-bit JVM: `OpenFileMappingW(DWORD, BOOL, LPCWSTR)` = `(JAVA_INT, JAVA_INT, ADDRESS)` with `captureCallState("GetLastError")`;
`MapViewOfFile(HANDLE, DWORD, DWORD, DWORD, SIZE_T)` = `(ADDRESS, JAVA_INT, JAVA_INT, JAVA_INT, JAVA_LONG)`, called with `0L`; `VirtualQuery(LPCVOID, PMBI, SIZE_T)` returns `SIZE_T`
(`JAVA_LONG`), `RegionSize` read at offset 24 of the 64-bit `MEMORY_BASIC_INFORMATION`, which is correct; `CreateMutexW(ADDRESS, JAVA_INT, ADDRESS)`. The view is
`reinterpret(size)` (page-rounded size, at least `kMappingBytes`) and `SkyLink` slices to `MAPPING_BYTES`. `--enable-native-access=ALL-UNNAMED` is present in
`build.gradle` vmArgs and in `launcher/src/prism.rs:103` (the Prism instance). Remaining: the host side does not open or create the `_minecraft` mutex (no
`CreateMutex/OpenMutex` in `platform_win.cpp`/`link.cpp`), so the Java announcement is unused; harmless.

### New DLL (03:58)
Exports: `CreateInterface` only. Imports still only KERNEL32, msvcrt, USER32, ADVAPI32 (new: `SetWindowsHookExW`, `MapVirtualKeyW`, `_wfopen`, `_wstat`). DllCharacteristics 0x140, Characteristics 0x2326 (large address aware), unchanged. Pass.

### Remaining problems
1. Unverified, not wrong: F6 `--wrap` for stream classes and `stat` naming under msvcrt (test on the first run with a user name containing a non-ASCII letter).
2. `render_backend/gl_dig.cpp:65` literal `kGetHighestEntityIndex = 6`: not built on Windows; add a row if that file ever ships.
3. Host never creates/opens the `Local\Portalcraft_v1_minecraft` mutex (cosmetic).
4. `input_win.cpp`: no `WM_INPUT` handling and unreviewed surrogate-pair handling of `WM_CHAR` (lines 104-126).
5. Everything marked Unverified in slots.hpp (14 rows incl. the 3 `RcField*` offsets) and the "Derived" Windows numbers still need the first-run probe; blocked on the Windows depot.
