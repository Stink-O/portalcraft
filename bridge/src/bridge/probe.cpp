// Portalcraft bridge: pcb_probe_slots, the first-run check of everything the bridge takes from the game's binaries.
//   pcb_probe_slots [neighbours=2]
// For every row of slots.hpp (the literal vtable slots) it prints the number used on this system, the address the vtable entry points to as
// module+offset, whether that module is the expected one and the page executable, the first 8 bytes of the target, and the entries around it.
// Then the three inline-hook signatures (how often each matches, where) and the SAR table entries in use. Nothing is called in the game: only
// vtables and code bytes are read, so a wrong number cannot crash the probe. Output goes to bridge.log (like every console line) and to
// cache/bridge/slot_probe.txt. The D3D9 hook has its own census (pcb_d3d9_stats).
#include "bridge.hpp"

#include "Modules/MaterialSystem.hpp"
#include "Modules/Tier1.hpp"
#include "Hook.hpp"
#include "Interface.hpp"
#include "Offsets.hpp"
#include "Utils/Memory.hpp"
#include "platform/platform.hpp"
#include "slots.hpp"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace bridge {
namespace {

FILE *g_out = nullptr;
int g_problems = 0;

void Out(const char *fmt, ...) {
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	console->Print("%s", buf);
	if (g_out) std::fputs(buf, g_out);
}

std::string Stem(std::string n) {  // "libengine.so", "engine.dll" -> "engine"
	for (char &c : n) c = (char)std::tolower((unsigned char)c);
	if (n.compare(0, 3, "lib") == 0) n.erase(0, 3);
	const size_t dot = n.find('.');
	return dot == std::string::npos ? n : n.substr(0, dot);
}

std::string Where(const void *p) {
	plat::AddressInfo ai;
	if (!plat::DescribeAddress(p, ai) || ai.module.empty()) {
		char b[64];
		std::snprintf(b, sizeof b, "%p(no module)", p);
		return b;
	}
	char b[256];
	std::snprintf(b, sizeof b, "%s+0x%x", ai.module.c_str(), (unsigned)ai.offset);
	return b;
}

std::string Bytes(const void *p, int n = 8) {
	uint8_t b[16] = {};
	if (!plat::SafeRead(p, b, (size_t)n)) return "(unreadable)";
	std::string s;
	char t[8];
	for (int i = 0; i < n; ++i) std::snprintf(t, sizeof t, "%s%02x", i ? " " : "", b[i]), s += t;
	return s;
}

// Windows thiscall/stdcall are callee-cleaned: the bytes a method pops with `ret imm16` are 4 x its argument count (this excluded), and a hook or call
// type with another count unbalances the stack (the Proton run's crashes). Scans a function's first 700 bytes (to its int3 padding) for the first
// `ret imm16`, following a forwarding thunk (`mov ecx,[global]; mov eax,[ecx]; mov edx,[eax+d]; jmp edx`) or a `jmp rel32` to its real body.
std::string RetInfo(const void *fn, int depth = 0) {
	uint8_t b[700] = {};
	if (!plat::SafeRead(fn, b, sizeof b)) return "";
	if (b[0] == 0xe9 && depth < 4) return RetInfo((const uint8_t *)fn + 5 + *(const int *)&b[1], depth + 1);
	for (int k = 0; k + 12 < 24; ++k) {
		if (b[k] != 0x8b || b[k + 1] != 0x0d || b[k + 6] != 0x8b || b[k + 7] != 0x01 || b[k + 8] != 0x8b || depth >= 4) continue;
		const uint8_t modrm = b[k + 9];
		int disp = -1;
		if ((modrm & 0xc0) == 0x40) disp = b[k + 10];
		else if ((modrm & 0xc0) == 0x80) disp = *(const int *)&b[k + 10];
		else if ((modrm & 0xc0) == 0x00) disp = 0;
		uintptr_t obj = 0, vt = 0, tgt = 0;
		if (disp < 0 || !plat::SafeRead(*(const void *const *)&b[k + 2], &obj, 4) || !plat::SafeRead((const void *)obj, &vt, 4) || !plat::SafeRead((const void *)(vt + disp), &tgt, 4)) break;
		return RetInfo((const void *)tgt, depth + 1);
	}
	for (int k = 0; k < 698; ++k) {
		if (b[k] == 0xc2 && b[k + 2] == 0) return " ret " + std::to_string(b[k + 1]) + " (" + std::to_string(b[k + 1] / 4) + " args)";
		if (b[k] == 0xcc && b[k + 1] == 0xcc) break;
	}
	return " ret ?";
}

// Our own plugin module (bare file name), for telling "hooked by us" from "wrong module".
std::string SelfModule() {
	plat::AddressInfo ai;
	plat::DescribeAddress((const void *)&Stem, ai);
	return ai.module;
}

struct Obj {
	void *thisptr = nullptr;
	void **vt = nullptr;
	void **orig = nullptr;  // the vtable before our Interface hooks swapped it (hook bookkeeping), nullptr when we did not hook this object
	int size = 0;  // entries that are code pointers, counted from slot 0 (capped)
	std::string note;
};

Obj MakeObj(void *p, const char *note) {
	Obj o;
	o.note = note;
	o.thisptr = p;
	if (!p) return o;
	void **vt = nullptr;
	if (!plat::SafeRead(p, &vt, sizeof vt) || !vt) return o;
	o.vt = vt;
	o.orig = (void **)Interface::OriginalVtable(p);
	for (; o.size < 400; ++o.size) {
		void *e = nullptr;
		if (!plat::SafeRead(vt + o.size, &e, sizeof e) || !e) break;
		plat::AddressInfo ai;
		if (!plat::DescribeAddress(e, ai) || !ai.executable) break;
	}
	return o;
}

std::map<std::string, Obj> g_objs;
std::vector<Interface *> g_made;

void AddIface(const char *key, const char *module, const char *version) {
	Interface *i = Interface::Create(module, version, false);
	if (!i) {
		g_objs[key] = Obj();
		g_objs[key].note = std::string(version) + " not found in " + module;
		return;
	}
	g_made.push_back(i);
	g_objs[key] = MakeObj(i->ThisPtr(), version);
}

void BuildObjects() {
	g_objs.clear();
	AddIface("IServerTools", MODULE_NAME("server"), "VSERVERTOOLS001");
	AddIface("IVEngineServer", MODULE_NAME("engine"), "VEngineServer022");
	AddIface("IVEngineClient", MODULE_NAME("engine"), "VEngineClient015");
	AddIface("IEngineTrace", MODULE_NAME("engine"), "EngineTraceServer004");
	AddIface("IEngineTraceClient", MODULE_NAME("engine"), "EngineTraceClient004");
	AddIface("IStudioRender", MODULE_NAME("studiorender"), "VStudioRender026");
	AddIface("IVModelRender", MODULE_NAME("engine"), "VEngineModel016");
	AddIface("IVRenderView", MODULE_NAME("engine"), "VEngineRenderView013");
	AddIface("IServerGameClients", MODULE_NAME("server"), "ServerGameClients004");
	AddIface("CGameMovement", MODULE_NAME("server"), "GameMovement001");
	AddIface("CGameMovementClient", MODULE_NAME("client"), "GameMovement001");
	AddIface("IVEfx", MODULE_NAME("engine"), "VEngineEffects001");
	AddIface("IClientEntityList", MODULE_NAME("client"), "VClientEntityList003");
	AddIface("IEngineVGui", MODULE_NAME("engine"), "VEngineVGui001");
	if (materialSystem) {
		IMatRenderContext *rc = materialSystem->GetRenderContext();
		g_objs["IMatRenderContext"] = MakeObj(rc, "VMaterialSystem080 GetRenderContext");
		if (rc) rc->vtable->Release(rc);
		IMaterial *m = materialSystem->FindMaterial("models/portals/portalstaticoverlay_1", "Model textures");
		g_objs["IMaterial"] = MakeObj(m, "models/portals/portalstaticoverlay_1");
	}
	// entity objects need a running level; reading only (IServerTools::FirstEntity is a call, so the entity comes from the client list)
	if (g_objs.count("IClientEntityList") && g_objs["IClientEntityList"].vt) {
		// IClientEntityList::GetClientNetworkable(1) is a Sar/Derived call (slot 0); only made when it is not held back
		if (slots::Usable(slots::kEntListGetClientNetworkable)) {
			auto fn = (void *(__rescall *)(void *, int))g_objs["IClientEntityList"].vt[slots::Index(slots::kEntListGetClientNetworkable)];
			void *net = fn ? fn(g_objs["IClientEntityList"].thisptr, 1) : nullptr;
			g_objs["IClientNetworkable"] = MakeObj(net, "entity 1 (networkable)");
			if (net) g_objs["IClientRenderable"] = MakeObj((char *)net + slots::Index(slots::kRenderableOffsetFromNet), "entity 1 (renderable = networkable + offset)");
		}
	}
}

// Which probe object holds the vtable for a table row (by the row's interface name).
const char *ObjKey(const slots::Entry &e) {
	static const struct {
		const char *iface, *key;
	} map[] = {{"IMatRenderContext", "IMatRenderContext"}, {"IMaterial", "IMaterial"}, {"IServerTools", "IServerTools"}, {"IVEngineServer", "IVEngineServer"},
	           {"IVEngineClient", "IVEngineClient"}, {"IEngineTrace", "IEngineTrace"}, {"IStudioRender", "IStudioRender"}, {"IVModelRender", "IVModelRender"},
	           {"IVRenderView", "IVRenderView"}, {"IServerGameClients", "IServerGameClients"}, {"CGameMovement", "CGameMovement"}, {"IVEfx", "IVEfx"},
	           {"IClientEntityList", "IClientEntityList"}, {"IClientNetworkable", "IClientNetworkable"}, {"IClientRenderable", "IClientRenderable"}};
	for (auto &m : map)
		if (!std::strcmp(m.iface, e.iface)) return m.key;
	return nullptr;  // ICallQueue, CMatRenderContext fields, server entity classes: not reachable without a call
}


int g_hooked = 0;

// Checks vtable entry idx of o against the module that should own it. Prints "-> target ..." and returns true when the entry is fine.
// An entry our own Interface hook replaced is HOOKED (ok) when the vtable it was taken from, read from the hook bookkeeping, pointed into
// the expected module and the new target lies in this plugin; a replacement by anything else, or an original in the wrong module, is a PROBLEM.
bool CheckEntry(const Obj &o, int idx, const char *module) {
	void *target = o.vt[idx];
	void *orig = o.orig ? o.orig[idx] : target;
	plat::AddressInfo ai;
	plat::DescribeAddress(target, ai);
	if (orig == target) {
		const bool inMod = Stem(ai.module) == module;
		Out("-> %s  %s%s  bytes %s%s\n", Where(target).c_str(), inMod ? "in expected module" : "NOT in the expected module", ai.executable ? "" : ", NOT EXECUTABLE", Bytes(target).c_str(),
		    RetInfo(target).c_str());
		return inMod && ai.executable;
	}
	plat::AddressInfo oi;
	plat::DescribeAddress(orig, oi);
	const bool origOk = Stem(oi.module) == module && oi.executable;
	const bool ours = !ai.module.empty() && ai.module == SelfModule() && ai.executable;
	Out("-> %s  %s; original %s  %s, bytes %s%s\n", Where(target).c_str(), ours ? "HOOKED by this plugin (ok)" : "REPLACED by something that is not this plugin", Where(orig).c_str(),
	    origOk ? "in expected module" : "NOT in the expected module", Bytes(orig).c_str(), RetInfo(orig).c_str());
	if (ours && origOk) ++g_hooked;
	return ours && origOk;
}

void ProbeSlots(int nb) {
	Out("---- vtable slots (slots.hpp; %d rows) ----\n", (int)slots::kCount);
	for (int i = 0; i < slots::kCount; ++i) {
		const slots::Entry &e = slots::Info((slots::Id)i);
		const char *basis = e.basis == slots::Basis::Sar ? "SAR" : e.basis == slots::Basis::Derived ? "derived" : e.basis == slots::Basis::ProtonVerified ? "proton-verified" : e.basis == slots::Basis::WindowsVerified ? "windows-verified" : "UNVERIFIED";
		const int idx = slots::Index((slots::Id)i);
		if (e.kind == slots::Kind::Offset) {
			Out("%-28s %-18s offset %d (linux %d, windows %d) [%s] %s: a byte offset inside an engine object, checked by the feature that reads it\n", e.name, e.iface, idx, e.linux_, e.win, basis,
			    slots::Usable((slots::Id)i) ? "enabled" : "HELD BACK");
			continue;
		}
		const char *key = ObjKey(e);
		auto it = key ? g_objs.find(key) : g_objs.end();
		Out("%-28s %-18s slot %3d (linux %d, windows %d) [%s] %s: ", e.name, e.iface, idx, e.linux_, e.win, basis, slots::Usable((slots::Id)i) ? "enabled" : "HELD BACK");
		if (it == g_objs.end() || !it->second.vt) {
			Out("%s\n", key ? ("object not available (" + (it == g_objs.end() ? std::string("not created") : it->second.note) + ")").c_str() : "no object to read (needs a call; check by use)");
			continue;
		}
		const Obj &o = it->second;
		if (idx < 0 || idx >= o.size) {
			Out("PROBLEM: slot is outside the vtable (%d code entries)\n", o.size);
			++g_problems;
			continue;
		}
		if (!CheckEntry(o, idx, e.module)) ++g_problems;
		if (nb > 0) {
			std::string line = "      neighbours:";
			for (int k = idx - nb; k <= idx + nb; ++k) {
				if (k < 0 || k >= o.size || k == idx) continue;
				char t[160];
				std::snprintf(t, sizeof t, " %d=%s", k, Where(o.vt[k]).c_str());
				line += t;
			}
			Out("%s\n", line.c_str());
		}
	}
	for (auto &kv : g_objs) {
		if (!kv.second.vt) continue;
		Out("object %-20s %s: this %p, vtable %s, %d code entries\n", kv.first.c_str(), kv.second.note.c_str(), kv.second.thisptr, Where(kv.second.vt).c_str(), kv.second.size);
	}
}

void ProbeSar() {
	Out("---- SourceAutoRecord table entries in use ----\n");
	struct Row {
		const char *name, *obj;
		int slot;
		const char *module;
	};
	const Row rows[] = {{"ProcessMovement", "CGameMovement", Offsets::ProcessMovement, "server"},
	                    {"ProcessMovement (client)", "CGameMovementClient", Offsets::ProcessMovement, "client"},
	                    {"TraceRay", "IEngineTrace", Offsets::TraceRay, "engine"},
	                    {"TraceRay (client)", "IEngineTraceClient", Offsets::TraceRay, "engine"},
	                    {"Paint", "IEngineVGui", Offsets::Paint, "engine"},
	                    {"ClientCmd", "IVEngineClient", Offsets::ClientCmd, "engine"},
	                    {"ExecuteClientCmd", "IVEngineClient", Offsets::ExecuteClientCmd, "engine"},
	                    {"GetScreenSize", "IVEngineClient", Offsets::GetScreenSize, "engine"},
	                    {"GetViewAngles", "IVEngineClient", Offsets::GetViewAngles, "engine"},
	                    {"SetViewAngles", "IVEngineClient", Offsets::SetViewAngles, "engine"},
	                    {"IsPaused", "IVEngineClient", Offsets::IsPaused, "engine"},
	                    {"Con_IsVisible", "IVEngineClient", Offsets::Con_IsVisible, "engine"},
	                    {"GetLightForPoint", "IVEngineClient", Offsets::GetLightForPoint, "engine"},
	                    {"SetGroundEntity", "CGameMovement", Offsets::SetGroundEntity, "server"},
	                    {"CheckJumpButton", "CGameMovement", Offsets::CheckJumpButton, "server"},
	                    {"FinishGravity", "CGameMovement", Offsets::FinishGravity, "server"}};
	for (const Row &r : rows) {
		auto it = g_objs.find(r.obj);
		if (it == g_objs.end() || !it->second.vt || r.slot < 0 || r.slot >= it->second.size) {
			Out("%-26s slot %3d: object not available or slot outside the vtable\n", r.name, r.slot);
			++g_problems;
			continue;
		}
		Out("%-26s slot %3d ", r.name, r.slot);
		if (!CheckEntry(it->second, r.slot, r.module)) {
			Out("      ^ PROBLEM\n");
			++g_problems;
		}
	}
}

// Does the signature ("55 8b ?? ...") match the bytes at `at`, with the first 5 taken from `saved` instead?
bool PatternMatchesRestored(const char *pattern, const uint8_t *at, const uint8_t *saved) {
	int i = 0;
	const char *p = pattern;
	while (*p) {
		while (*p == ' ') ++p;
		if (!*p) break;
		uint8_t b = 0;
		if (i < 5) b = saved[i];
		else if (!plat::SafeRead(at + i, &b, 1)) return false;
		if (*p == '?') {
			while (*p && *p != ' ') ++p;
		} else {
			const unsigned v = (unsigned)std::strtoul(std::string(p, 2).c_str(), nullptr, 16);
			if (v != b) return false;
			p += 2;
		}
		++i;
	}
	return i > 0;
}

void ProbeSignatures() {
	Out("---- inline-hook signatures (each must match exactly once) ----\n");
	struct Sig {
		const char *name, *module, *pattern;
	};
	const Sig sigs[] = {{"DrawTranslucentRenderables", MODULE_NAME("client"), Offsets::DrawTranslucentRenderables},
	                    {"DrawOpaqueRenderables", MODULE_NAME("client"), Offsets::DrawOpaqueRenderables},
	                    {"TraceFirePortal", MODULE_NAME("server"), Offsets::TraceFirePortal}};
	for (const Sig &s : sigs) {
		if (!s.pattern || !*s.pattern) {
			Out("%-28s no pattern for this system\n", s.name);
			++g_problems;
			continue;
		}
		auto hits = Memory::MultiScan(s.module, s.pattern);
		// Our inline hooks overwrite the first 5 bytes, so a hooked function no longer matches in memory: take the bytes the hook saved
		// (Hook::OrigCode) for those 5, the live bytes for the rest, and count the site as a match when that restored image fits.
		Memory::ModuleInfo mi;
		const bool haveMod = Memory::TryGetModule(s.module, &mi);
		int hookedHits = 0;
		for (Hook *h : Hook::GetHooks()) {
			if (!haveMod || !h->Enabled() || !h->Func()) continue;
			const uintptr_t f = (uintptr_t)h->Func();
			if (f < (uintptr_t)mi.base || f >= (uintptr_t)mi.base + mi.size) continue;
			if (PatternMatchesRestored(s.pattern, (const uint8_t *)f, h->OrigCode())) {
				++hookedHits;
				Out("%-28s hooked at %s (saved original bytes %s match the signature)\n", s.name, Where((void *)f).c_str(), Bytes(h->OrigCode(), 5).c_str());
			}
		}
		const size_t total = hits.size() + (size_t)hookedHits;
		Out("%-28s in %s: %zu match%s", s.name, s.module, total, total == 1 ? "" : "es");
		for (size_t i = 0; i < hits.size() && i < 3; ++i) Out("  %s bytes %s", Where((void *)hits[i]).c_str(), Bytes((void *)hits[i]).c_str());
		if (hookedHits) ++g_hooked;
		Out("%s\n", total == 1 ? (hookedHits ? "  ok (HOOKED by this plugin)" : "  ok") : "  PROBLEM");
		if (total != 1) ++g_problems;
	}
}

void ProbeModules() {
	Out("---- modules ----\n");
	for (const char *m : {"engine", "client", "server", "materialsystem", "studiorender", "tier0", "vstdlib", "shaderapidx9", "inputsystem", "launcher"}) {
		Memory::ModuleInfo mi;
		std::string name = std::string(m) + MODULE_EXTENSION;
		if (Memory::TryGetModule(name.c_str(), &mi)) Out("%-16s base %p size 0x%x path %s\n", name.c_str(), (void *)mi.base, (unsigned)mi.size, mi.path);
		else Out("%-16s not loaded\n", name.c_str());
	}
}

}  // namespace

CON_COMMAND(pcb_probe_slots, "pcb_probe_slots [neighbours=2] - check every vtable slot, signature and SAR offset the bridge uses against the loaded game (reads only; log + cache/bridge/slot_probe.txt)\n") {
	const int nb = args.ArgC() > 1 ? std::atoi(args[1]) : 2;
	g_out = std::fopen((CacheDir() + "/slot_probe.txt").c_str(), "w");
	g_problems = 0;
	g_hooked = 0;
	const std::string area = plat::AddressSpaceSummary();
	Out("==== pcb_probe_slots: %s, pid %u%s%s\n", plat::ExecutablePath().c_str(), plat::ProcessId(), area.empty() ? "" : ", ", area.c_str());
	ProbeModules();
	BuildObjects();
	ProbeSlots(nb);
	ProbeSar();
	ProbeSignatures();
	for (Interface *i : g_made) Interface::Delete(i);
	g_made.clear();
	g_objs.clear();
	Out("==== probe finished: %d problem%s (%d entries/signatures hooked by this plugin, counted as ok). A slot that is NOT in the expected module is wrong for this build: fix it without rebuilding with PCB_SLOTS=Name=number "
	    "(or cache/bridge/slot_overrides.txt), then enable held-back groups with PCB_WIN_UNVERIFIED=<group>\n", g_problems, g_problems == 1 ? "" : "s", g_hooked);
	if (g_out) std::fclose(g_out), g_out = nullptr;
}

}  // namespace bridge
