// Portalcraft bridge: server entity helpers (no sv_cheats needed).
//   IServerTools (VSERVERTOOLS001, Linux slots): SnapPlayerToPosition 3, FirstEntity 8,
//   NextEntity 9, GetKeyValue 11 (SourceAutoRecord pins the neighbouring SetKeyValue 12,
//   CreateEntityByName 15, DispatchSpawn 16).
//   CBaseEntity::AcceptInput: vtable slot 41 on Linux (SourceAutoRecord).
#include "bridge.hpp"
#include "slots.hpp"

#include "Hook.hpp"
#include "Offsets.hpp"
#include "Modules/Tier1.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <unordered_set>

namespace bridge {

static Interface *g_tools = nullptr;
using _FirstEntity = void *(__rescall *)(void *);
using _NextEntity = void *(__rescall *)(void *, void *);
using _GetKeyValue = bool(__rescall *)(void *, void *, const char *, char *, int);
using _SnapPlayer = void(__rescall *)(void *, const Vector &org, const QAngle &ang, void *clientPlayer);

// variant_t as the engine passes it by value (union, handle, field type).
struct Variant {
	union {
		const char *str;
		int i;
		float f;
		float vec[3];
	};
	unsigned int ehandle;
	int fieldType;  // 2 = FIELD_STRING, 0 = FIELD_VOID
};
// variant_t has a non-trivial copy constructor, so the Linux (Itanium) ABI passes it by
// invisible reference: the fifth argument is a pointer.
#ifdef _WIN32
// MSVC passes variant_t by value (20 bytes on the stack; the method ends in `ret 0x24`): the callee pops it, so a pointer-sized stand-in unbalances the stack.
using _AcceptInput = bool(__rescall *)(void *self, const char *input, void *activator, void *caller, Variant value, int outputId);
#define PCB_AI_ARG(v) (v)
#define PCB_AI_PTR(v) (&(v))
#else
using _AcceptInput = bool(__rescall *)(void *self, const char *input, void *activator, void *caller, Variant *value, int outputId);
#define PCB_AI_ARG(v) (&(v))
#define PCB_AI_PTR(v) (v)
#endif

static bool Tools() {
	if (!g_tools) g_tools = Interface::Create(MODULE_NAME("server"), "VSERVERTOOLS001", false);
	return g_tools != nullptr;
}

void EntsShutdown() {
	Interface::Delete(g_tools);
	g_tools = nullptr;
}

std::string EntKV(void *ent, const char *key) {
	if (!Tools() || !ent) return "";
	char buf[256] = {0};
	auto get = slots::Orig<_GetKeyValue>(g_tools, slots::kToolsGetKeyValue);
	if (!get || !get(g_tools->ThisPtr(), ent, key, buf, sizeof buf)) return "";
	return buf;
}

Vector EntVec(void *ent, const char *key) {
	Vector v(0, 0, 0);
	std::sscanf(EntKV(ent, key).c_str(), "%f %f %f", &v.x, &v.y, &v.z);
	return v;
}

std::vector<void *> EntFind(const char *classname, const char *targetname) {
	std::vector<void *> out;
	if (!Tools()) return out;
	auto first = slots::Orig<_FirstEntity>(g_tools, slots::kToolsFirstEntity);
	auto next = slots::Orig<_NextEntity>(g_tools, slots::kToolsNextEntity);
	if (!first || !next) return out;
	int guard = 0;
	for (void *e = first(g_tools->ThisPtr()); e && guard < 16384; e = next(g_tools->ThisPtr(), e), ++guard) {
		if (classname && EntKV(e, "classname") != classname) continue;
		if (targetname && EntKV(e, "targetname") != targetname) continue;
		out.push_back(e);
	}
	return out;
}

// Every live server entity (pointers only: cheap enough for every tick).
void EntLiveSet(std::unordered_set<void *> &out) {
	out.clear();
	if (!Tools()) return;
	auto first = slots::Orig<_FirstEntity>(g_tools, slots::kToolsFirstEntity);
	auto next = slots::Orig<_NextEntity>(g_tools, slots::kToolsNextEntity);
	if (!first || !next) return;
	int guard = 0;
	for (void *e = first(g_tools->ThisPtr()); e && guard < 16384; e = next(g_tools->ThisPtr(), e), ++guard) out.insert(e);
}

bool EntInput(void *ent, const char *input, const char *value) {
	if (!ent) return false;
	Variant v;
	std::memset(&v, 0, sizeof v);
	v.ehandle = 0xFFFFFFFF;
	if (value) v.str = value, v.fieldType = 2;
	auto fn = Memory::VMT<_AcceptInput>(ent, Offsets::AcceptInput);
	return fn(ent, input, nullptr, nullptr, PCB_AI_ARG(v), 0);
}

// ---- entity fields by name, through the server datamaps (layout as in SourceAutoRecord's
// Datamap.hpp, MIT; Linux: 64-byte records, the embedded map pointer at +32). The map comes from
// CBaseEntity::GetDataDescMap (Offsets::S_GetDataDescMap).
namespace {
struct TypeDesc {
	int fieldType;
	const char *fieldName;
	int fieldOffset;
	unsigned short fieldSize;
	short flags;
	const char *externalName;
	void *saveRestoreOps;
#ifndef _WIN32
	void *unk1;  // Linux only (upstream SAR's Datamap.hpp: no such field in the Windows layout; found by the Proton run 2026-10-11)
#endif
	void *inputFunc;
	struct DataMap *td;
	int fieldSizeInBytes;
	void *overrideField;
	int overrideCount;
	float fieldTolerance;
	int flatOffset[2];
	unsigned short flatGroup;
};
#ifdef _WIN32
static_assert(sizeof(TypeDesc) == 60, "typedescription_t is 60 bytes on Windows");
#else
static_assert(sizeof(TypeDesc) == 64, "typedescription_t is 64 bytes on Linux");
#endif
struct DataMap {
	TypeDesc *dataDesc;
	int dataNumFields;
	const char *dataClassName;
	DataMap *baseMap;
};
int FindField(DataMap *dm, const char *name, int base, int depth) {
	for (; dm && depth < 32; dm = dm->baseMap, ++depth) {
		for (int i = 0; i < dm->dataNumFields; ++i) {
			const TypeDesc &f = dm->dataDesc[i];
			if (f.fieldName && !std::strcmp(f.fieldName, name)) return base + f.fieldOffset;
			if (f.fieldType == 10 /* FIELD_EMBEDDED */ && f.td) {
				int r = FindField(f.td, name, base + f.fieldOffset, depth + 1);
				if (r >= 0) return r;
			}
		}
	}
	return -1;
}
}  // namespace

static void DumpFields(DataMap *dm, const char *sub, int base, int depth, int &n) {
	for (; dm && depth < 32 && n < 200; dm = dm->baseMap, ++depth)
		for (int i = 0; i < dm->dataNumFields && n < 200; ++i) {
			const TypeDesc &f = dm->dataDesc[i];
			if (f.fieldName && (!sub || std::strstr(f.fieldName, sub)))
				++n, console->Print("[bridge]   %s.%s +%d type %d\n", dm->dataClassName ? dm->dataClassName : "?", f.fieldName, base + f.fieldOffset, f.fieldType);
			if (f.fieldType == 10 && f.td) DumpFields(f.td, sub, base + f.fieldOffset, depth + 1, n);
		}
}
int EntFieldOffset(void *ent, const char *field);
CON_COMMAND(pcb_ent_field, "pcb_ent_field <classname> <targetname|*> <field> <i|f|s|b> - one server datamap field of the first matching entity (debug)\n") {
	if (args.ArgC() < 5) return;
	std::vector<void *> v = EntFind(args[1], std::strcmp(args[2], "*") ? args[2] : nullptr);
	if (v.empty()) return console->Print("[bridge] field: no %s %s\n", args[1], args[2]);
	int off = EntFieldOffset(v[0], args[3]);
	if (off < 0) return console->Print("[bridge] field: %s has no %s\n", args[1], args[3]);
	const char *p = (const char *)v[0] + off;
	switch (args[4][0]) {
	case 'f': console->Print("[bridge] field %s.%s = %f\n", args[1], args[3], *(const float *)p); break;
	case 's': console->Print("[bridge] field %s.%s = '%s'\n", args[1], args[3], *(const char *const *)p ? *(const char *const *)p : "(null)"); break;
	case 'b': console->Print("[bridge] field %s.%s = %d\n", args[1], args[3], (int)*(const unsigned char *)p); break;
	default: console->Print("[bridge] field %s.%s = %d\n", args[1], args[3], *(const int *)p); break;
	}
}
void TestCheck(bool ok, const char *what, const char *detail);  // testassert.cpp
CON_COMMAND(pcb_assert_ent_field, "pcb_assert_ent_field <classname> <targetname|*> <field> <int value> - a server datamap field (int or bool) has that value (tests: a floor button pressed)\n") {
	if (args.ArgC() < 5) return;
	std::vector<void *> v = EntFind(args[1], std::strcmp(args[2], "*") ? args[2] : nullptr);
	char buf[200];
	if (v.empty()) {
		std::snprintf(buf, sizeof buf, "no %s %s", args[1], args[2]);
		return TestCheck(false, "entity field", buf);
	}
	const int off = EntFieldOffset(v[0], args[3]);
	const int want = std::atoi(args[4]);
	const int val = off < 0 ? -9999 : (want == 0 || want == 1) ? (int)*((const unsigned char *)v[0] + off) : *(const int *)((const char *)v[0] + off);
	std::snprintf(buf, sizeof buf, "%s %s.%s = %d (want %d)%s", args[1], args[2], args[3], val, want, off < 0 ? " (no such field)" : "");
	TestCheck(off >= 0 && val == want, "entity field", buf);
}
// (bridge #31) a float field (a math_counter's value, an output counter) is within min..max
CON_COMMAND(pcb_assert_ent_float, "pcb_assert_ent_float <classname> <targetname|*> <field> <min> [max=min] - (bridge #31) a server datamap float field (math_counter m_OutValue) is in range\n") {
	if (args.ArgC() < 5) return;
	std::vector<void *> v = EntFind(args[1], std::strcmp(args[2], "*") ? args[2] : nullptr);
	char buf[200];
	if (v.empty()) {
		std::snprintf(buf, sizeof buf, "no %s %s", args[1], args[2]);
		return TestCheck(false, "entity float field", buf);
	}
	const int off = EntFieldOffset(v[0], args[3]);
	const float val = off < 0 ? -9999.0f : *(const float *)((const char *)v[0] + off), mn = (float)std::atof(args[4]), mx = args.ArgC() > 5 ? (float)std::atof(args[5]) : mn;
	std::snprintf(buf, sizeof buf, "%s %s.%s = %.2f (want %.2f..%.2f)%s", args[1], args[2], args[3], val, mn, mx, off < 0 ? " (no such field)" : "");
	TestCheck(off >= 0 && val >= mn - 1e-3f && val <= mx + 1e-3f, "entity float field", buf);
}
CON_COMMAND(pcb_dump_fields, "pcb_dump_fields <classname> [substring] - the server datamap fields of the first such entity (debug)\n") {
	if (args.ArgC() < 2) return;
	std::vector<void *> v = EntFind(args[1]);
	console->Print("[bridge] %d %s entities\n", (int)v.size(), args[1]);
	if (v.empty()) return;
	auto getMap = Memory::VMT<DataMap *(__rescall *)(void *)>(v[0], Offsets::S_GetDataDescMap);
	int n = 0;
	DumpFields(getMap(v[0]), args.ArgC() > 2 ? args[2] : nullptr, 0, 0, n);
}

int EntFieldOffset(void *ent, const char *field) {
	if (!ent) return -1;
	auto getMap = Memory::VMT<DataMap *(__rescall *)(void *)>(ent, Offsets::S_GetDataDescMap);
	return FindField(getMap(ent), field, 0, 0);
}

// (bridge #37) a keyvalue straight from the entity's datamap, by its external (keyvalue) name or its field name: the route for a key that
// IServerTools::GetKeyValue does not give (an empty string for a string key, a key the entity resolved and cleared at spawn). `out` is set for
// string, float, int, bool and vector fields; false: no such field or a type that is not read.
bool EntDatamapRead(void *ent, const char *name, std::string &out) {
	if (!ent || !name) return false;
	auto getMap = Memory::VMT<DataMap *(__rescall *)(void *)>(ent, Offsets::S_GetDataDescMap);
	int depth = 0;
	for (DataMap *dm = getMap(ent); dm && depth < 32; dm = dm->baseMap, ++depth)
		for (int i = 0; i < dm->dataNumFields; ++i) {
			const TypeDesc &f = dm->dataDesc[i];
			const bool hit = (f.externalName && !strcasecmp(f.externalName, name)) || (f.fieldName && !std::strcmp(f.fieldName, name));
			if (!hit) continue;
			const char *p = (const char *)ent + f.fieldOffset;
			char b[96];
			switch (f.fieldType) {
			case 2: {  // FIELD_STRING: a pointer into the engine's string pool
				const char *str = *(const char *const *)p;
				out.clear();
				for (int k = 0; str && k < 200 && str[k]; ++k) {
					if ((unsigned char)str[k] < 32 || (unsigned char)str[k] > 126) return out.clear(), false;
					out += str[k];
				}
				return true;
			}
			case 1: case 16: std::snprintf(b, sizeof b, "%g", *(const float *)p), out = b; return true;
			case 5: case 17: std::snprintf(b, sizeof b, "%d", *(const int *)p), out = b; return true;
			case 6: std::snprintf(b, sizeof b, "%d", (int)*(const unsigned char *)p), out = b; return true;
			case 3: std::snprintf(b, sizeof b, "%g %g %g", ((const float *)p)[0], ((const float *)p)[1], ((const float *)p)[2]), out = b; return true;
			default: break;
			}
		}
	return false;
}
// EntKV, then the datamap when the key comes back empty (not for loops over every entity: a datamap walk per call)
std::string EntKVx(void *ent, const char *key) {
	std::string s = EntKV(ent, key);
	if (s.empty() && !EntDatamapRead(ent, key, s)) s.clear();
	return s;
}

// MOVETYPE_* of a server player (or other entity); -1 if the field is not found.
int EntMoveType(void *ent) {
	static int off = -2;
	if (off == -2) {
		off = EntFieldOffset(ent, "m_MoveType");
		console->Print("[bridge] m_MoveType at +%d in the server datamap\n", off);
	}
	return off < 0 ? -1 : (int)*reinterpret_cast<unsigned char *>((char *)ent + off);
}

void SnapPlayer(const Vector &pos, const QAngle &ang) {
	if (!Tools()) return;
	if (auto snap = slots::Orig<_SnapPlayer>(g_tools, slots::kToolsSnapPlayer)) snap(g_tools->ThisPtr(), pos, ang, nullptr);
}


// ---- (bridge #63) watched inputs: env_fade "Fade" (the screen fade the Minecraft hand/HUD must be under) and player_speedmod "ModifySpeed"
// (Portal 2 holds the player still with speed 0). CBaseEntity::AcceptInput (vtable slot Offsets::AcceptInput, not overridden by these classes) gets
// a jump patch like the SDL swap hook (glhud.cpp): off while the original runs, on again after. Everything else passes untouched.
static void *g_aiFunc = nullptr;
static int g_portalInputLogs = 0;  // (a cap per level: the log is for finding who closes a portal, not a stream)
#ifdef _WIN32
static bool __rescall AcceptInput_Hook(void *self, const char *input, void *activator, void *caller, Variant value, int outputId);
#else
static bool __rescall AcceptInput_Hook(void *self, const char *input, void *activator, void *caller, Variant *value, int outputId);
#endif
static Hook g_aiHook(&AcceptInput_Hook);
struct FadeRec {
	double t0;
	float dur, hold, rev, a;
	unsigned flags;
	float rgb[3];
};
static std::vector<FadeRec> g_fades;
static float g_speedmod = 1.0f;
static bool g_vmGrab = false;  // (bridge #68) logic_playerproxy ForceVMGrabController: what the player picks up is carried by the viewmodel (intro7's Wheatley, bts4)
static double NowSecE() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static void OnWatchedInput(void *self, const char *input, Variant *value) {
	const std::string cls = EntKV(self, "classname");
	if (!strcasecmp(input, "ForceVMGrabController") || !strcasecmp(input, "ForcePhysicsGrabController")) {
		g_vmGrab = !strcasecmp(input, "ForceVMGrabController");
		console->Print("[bridge] watch: %s %s: the player's grab is now %s\n", cls.c_str(), input, g_vmGrab ? "the viewmodel's" : "the physics one");
	} else if (!strcasecmp(input, "ModifySpeed") && cls == "player_speedmod") {
		float v = 1.0f;
		if (value) v = value->fieldType == 2 && value->str ? (float)std::atof(value->str) : value->fieldType == 5 ? (float)value->i : value->f;
		g_speedmod = v;
		console->Print("[bridge] watch: player_speedmod %s ModifySpeed %.2f\n", EntKV(self, "targetname").c_str(), v);
	} else if (!strcasecmp(input, "Fade") && cls == "env_fade") {
		FadeRec f{};
		f.t0 = NowSecE();
		f.dur = (float)std::atof(EntKVx(self, "duration").c_str());
		f.hold = (float)std::atof(EntKVx(self, "holdtime").c_str());
		f.rev = (float)std::atof(EntKVx(self, "ReverseFadeDuration").c_str());
		f.flags = (unsigned)std::atoi(EntKVx(self, "spawnflags").c_str());
		int r = 255, g = 255, b = 255, a = 255;
		std::sscanf(EntKVx(self, "rendercolor").c_str(), "%d %d %d", &r, &g, &b);
		const std::string am = EntKVx(self, "renderamt");
		if (!am.empty()) a = std::atoi(am.c_str());
		f.rgb[0] = r / 255.0f, f.rgb[1] = g / 255.0f, f.rgb[2] = b / 255.0f, f.a = a / 255.0f;
		g_fades.push_back(f);
		if (g_fades.size() > 16) g_fades.erase(g_fades.begin());
		console->Print("[bridge] watch: env_fade %s Fade: %s, colour (%d %d %d) alpha %d, %.1f s + hold %.1f s, back over %.1f s\n", EntKV(self, "targetname").c_str(),
		               (f.flags & 1) ? "from the colour" : "to the colour", r, g, b, a, f.dur, f.hold, f.rev);
	}
}
#ifdef _WIN32
static bool __rescall AcceptInput_Hook(void *self, const char *input, void *activator, void *caller, Variant value, int outputId) {
#else
static bool __rescall AcceptInput_Hook(void *self, const char *input, void *activator, void *caller, Variant *value, int outputId) {
#endif
	g_aiHook.Disable();
	const bool r = ((_AcceptInput)g_aiFunc)(self, input, activator, caller, value, outputId);
	g_aiHook.Enable();
	if (input && (input[0] == 'F' || input[0] == 'f' || input[0] == 'M' || input[0] == 'm')) OnWatchedInput(self, input, PCB_AI_PTR(value));
	// (2026-10-12) every input a portal gets, with who sent it: the Minecraft exit portal was closed when Chell went through it and the bridge's
	// re-open replayed its animation and sound; this says whether a map relay or the engine does it (no input logged = the engine itself)
	if (input && self && g_portalInputLogs < 400 && EntKV(self, "classname") == "prop_portal") {
		++g_portalInputLogs;
		const Variant *v = PCB_AI_PTR(value);
		const char *val = v && v->fieldType == 2 && v->str ? v->str : "";
		console->Print("[bridge] watch: prop_portal %p (%s, PortalTwo %s) input %s \"%s\" from %s %s -> %s\n", self, EntKV(self, "targetname").c_str(), EntKV(self, "PortalTwo").c_str(), input, val,
		               caller ? EntKV(caller, "classname").c_str() : "(no caller: the plugin or the engine)", caller ? EntKV(caller, "targetname").c_str() : "", r ? "taken" : "refused");
	}
	return r;
}
void InputWatchInstall() {
	if (g_aiFunc || !Tools()) return;
	std::vector<void *> v = EntFind("env_fade", nullptr);
	if (v.empty()) v = EntFind("player_speedmod", nullptr);
	if (v.empty()) v = EntFind("logic_playerproxy", nullptr);
	if (v.empty()) return;
	g_aiFunc = (void *)Memory::VMT<_AcceptInput>(v[0], Offsets::AcceptInput);
	g_aiHook.SetFunc(g_aiFunc);
	console->Print("[bridge] watch: AcceptInput hooked at %p (env_fade Fade, player_speedmod ModifySpeed)\n", g_aiFunc);
}
void InputWatchLevelInit() {
	g_portalInputLogs = 0;
	g_fades.clear();
	g_speedmod = 1.0f;
	g_vmGrab = false;
}
bool PlayerVmGrab() { return g_vmGrab; }
void InputWatchShutdown() {
	g_aiHook.Disable(true);
}
bool PlayerSpeedmodZero() { return g_speedmod < 0.05f; }
// The screen fade now: the strongest of the live fades (colour, alpha 0..1), modelled like the engine: to the colour over `duration`, held `holdtime`,
// back over ReverseFadeDuration (spawnflag 4 stays out); "from the colour" (spawnflag 1) starts at the colour, held, then clears over `duration`.
bool FadeNow(float &alpha, float rgb[3]) {
	const double now = NowSecE();
	alpha = 0;
	rgb[0] = rgb[1] = rgb[2] = 1.0f;
	for (size_t i = 0; i < g_fades.size();) {
		const FadeRec &f = g_fades[i];
		double t = now - f.t0;
		float k = 0;
		bool done = false;
		if (f.flags & 1) {
			if (t < f.hold) k = 1;
			else if (t - f.hold < f.dur) k = 1.0f - (float)((t - f.hold) / std::max(0.001f, f.dur));
			else done = true;
		} else if (t < f.dur) k = (float)(t / std::max(0.001f, f.dur));
		else if (t - f.dur < f.hold) k = 1;
		else if (f.flags & 4) k = 1;
		else if (f.rev > 0 && t - f.dur - f.hold < f.rev) k = 1.0f - (float)((t - f.dur - f.hold) / f.rev);
		else done = true;
		if (done) {
			g_fades.erase(g_fades.begin() + i);
			continue;
		}
		if (k * f.a > alpha) alpha = k * f.a, rgb[0] = f.rgb[0], rgb[1] = f.rgb[1], rgb[2] = f.rgb[2];
		++i;
	}
	return alpha > 0.002f;
}

}  // namespace bridge
