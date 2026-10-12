// Portalcraft bridge: trimmed implementations of SourceAutoRecord's Console,
// Tier1, Command and MaterialSystem modules. Adapted from SourceAutoRecord
// (https://github.com/p2sr/SourceAutoRecord, MIT; see ../LICENSE-SourceAutoRecord).
#include "Modules/Console.hpp"
#include "Modules/MaterialSystem.hpp"
#include "Modules/Tier1.hpp"
#include "Offsets.hpp"
#include "Utils/Memory.hpp"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <cstring>

// Set by the bridge; every console line is mirrored here.
FILE *g_bridgeLogFile = nullptr;
// Recording-clean mode: bridge messages go to the log only, never the game console.
bool g_bridgeQuiet = false;
namespace bridge { void SetQuiet(bool q) { g_bridgeQuiet = q; } }

Console *console = nullptr;
Tier1 *tier1 = nullptr;
MaterialSystem *materialSystem = nullptr;

// ---------------------------------------------------------------- Console

// (bridge #53) the log's size counters (growth.cpp prints them) and a rate limit on Print: a line that repeats every frame (a
// 5-minute session already wrote 70 MB over a week) cost a write + flush each, plus the engine console's own copy.
unsigned long long g_logLines = 0, g_logBytes = 0, g_logDropped = 0;
static std::mutex g_logMx;
static double g_logTokens = 3000.0, g_logT = 0.0;
static unsigned long long g_logPendingDrop = 0;
static const double kLogRate = 300.0, kLogBurst = 3000.0;  // lines per second sustained, burst
static bool LogAdmit(unsigned long long &droppedBefore) {
	std::lock_guard<std::mutex> lk(g_logMx);
	const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	if (g_logT > 0.0) g_logTokens = std::min(kLogBurst, g_logTokens + (now - g_logT) * kLogRate);
	g_logT = now;
	droppedBefore = 0;
	if (g_logTokens < 1.0) {
		++g_logDropped, ++g_logPendingDrop;
		return false;
	}
	g_logTokens -= 1.0;
	droppedBefore = g_logPendingDrop, g_logPendingDrop = 0;
	return true;
}

static void LogLine(const char *prefix, const char *buf) {
	if (!g_bridgeLogFile) return;
	std::fputs(prefix, g_bridgeLogFile);
	std::fputs(buf, g_bridgeLogFile);
	std::fflush(g_bridgeLogFile);
	++g_logLines, g_logBytes += std::strlen(prefix) + std::strlen(buf);
}

bool Console::Init() {
	auto tier0 = Memory::GetModuleHandleByName(MODULE_NAME(TIER0));
	if (!tier0) return false;
	this->Msg = Memory::GetSymbolAddress<_Msg>(tier0, "Msg");
	this->Warning = Memory::GetSymbolAddress<_Msg>(tier0, "Warning");
	this->ColorMsg = Memory::GetSymbolAddress<_ColorMsg>(tier0, CONCOLORMSG_SYMBOL);
	this->DevMsgFn = Memory::GetSymbolAddress<_Msg>(tier0, DEVMSG_SYMBOL);
	this->DevWarningFn = Memory::GetSymbolAddress<_Msg>(tier0, DEVWARNINGMSG_SYMBOL);
	Memory::CloseModuleHandle(tier0);
	return this->Msg && this->ColorMsg;
}

#define FORMAT_BUF()                         \
	char buf[2048];                          \
	va_list ap;                              \
	va_start(ap, fmt);                       \
	std::vsnprintf(buf, sizeof buf, fmt, ap); \
	va_end(ap);

void Console::Print(const char *fmt, ...) {
	FORMAT_BUF();
	unsigned long long dropped = 0;
	if (!LogAdmit(dropped)) return;  // (bridge #53) over 300 lines/s sustained: dropped, counted, reported with the next line
	if (dropped) {
		char note[128];
		std::snprintf(note, sizeof note, "[bridge] log: %llu lines dropped by the rate limit (300/s sustained, burst 3000)\n", dropped);
		LogLine("", note);
	}
	LogLine("", buf);
	if (!g_bridgeQuiet && this->ColorMsg) this->ColorMsg(Color(120, 200, 255), "%s", buf);
}
void Console::Warn(const char *fmt, ...) {
	FORMAT_BUF();
	LogLine("WARN ", buf);
	if (!g_bridgeQuiet && this->Warning) this->Warning("%s", buf);
}
void Console::DevMsg(const char *fmt, ...) {
	FORMAT_BUF();
	LogLine("dev ", buf);
	if (!g_bridgeQuiet && this->DevMsgFn) this->DevMsgFn("%s", buf);
}
void Console::DevWarning(const char *fmt, ...) {
	FORMAT_BUF();
	LogLine("devwarn ", buf);
	if (!g_bridgeQuiet && this->DevWarningFn) this->DevWarningFn("%s", buf);
}

// ---------------------------------------------------------------- Tier1

bool Tier1::Init() {
	this->g_pCVar = Interface::Create(MODULE_NAME(TIER1), "VEngineCvar007", false);
	if (!this->g_pCVar) return false;
	this->RegisterConCommand = this->g_pCVar->Original<_RegisterConCommand>(Offsets::RegisterConCommand);
	this->UnregisterConCommand = this->g_pCVar->Original<_UnregisterConCommand>(Offsets::UnregisterConCommand);
	this->FindCommandBase = this->g_pCVar->Original<_FindCommandBase>(Offsets::FindCommandBase);

	// SAR steals ConCommand's vtable from an existing engine command.
	auto listdemo = reinterpret_cast<ConCommand *>(this->FindCommandBase(this->g_pCVar->ThisPtr(), "listdemo"));
	if (listdemo) this->ConCommand_VTable = *(void **)listdemo;

	auto h = Memory::GetModuleHandleByName(MODULE_NAME(TIER1));
	if (h) {
		this->KeyValuesSystem = Memory::GetSymbolAddress<_KeyValuesSystem>(h, "KeyValuesSystem");
		Memory::CloseModuleHandle(h);
	}
	return this->ConCommand_VTable && this->KeyValuesSystem;
}

void Tier1::Shutdown() {
	Interface::Delete(this->g_pCVar);
	this->g_pCVar = nullptr;
}

std::vector<Command *> &Command::GetList() {
	static std::vector<Command *> list;
	return list;
}
Command::Command(const char *name, _CommandCallback cb, const char *help, int flags) {
	this->ptr = new ConCommand(name, cb, help, flags, nullptr);
	Command::GetList().push_back(this);
}
void Command::Register() {
	if (this->isRegistered) return;
	*(void **)this->ptr = tier1->ConCommand_VTable;
	tier1->RegisterConCommand(tier1->g_pCVar->ThisPtr(), this->ptr);
	this->isRegistered = true;
}
void Command::Unregister() {
	if (!this->isRegistered) return;
	tier1->UnregisterConCommand(tier1->g_pCVar->ThisPtr(), this->ptr);
	this->isRegistered = false;
}
int Command::RegisterAll() {
	int n = 0;
	for (auto c : GetList()) {
		c->Register();
		++n;
	}
	return n;
}
void Command::UnregisterAll() {
	for (auto c : GetList()) c->Unregister();
}

// ---------------------------------------------------------------- MaterialSystem

// Procedural texture filled from a BGRA buffer (SAR's MemTexRegen).
class MemTexRegen : public ITextureRegenerator {
	uint8_t *bgra;
	int w, h;

public:
	MemTexRegen(uint8_t *src, int w, int h) : w(w), h(h) {
		this->bgra = new uint8_t[w * h * 4];
		std::memcpy(this->bgra, src, w * h * 4);
	}
	~MemTexRegen() { delete[] this->bgra; }

	virtual void RegenerateTextureBits(ITexture *tex, IVTFTexture *vtf_tex, Rect_t *rect) override {
		auto ImageFormat_fn = Memory::VMT<ImageFormat(__rescall *)(IVTFTexture *)>(vtf_tex, Offsets::ImageFormat);
		ImageFormat fmt = ImageFormat_fn(vtf_tex);
		if (fmt != IMAGE_FORMAT_BGRA8888) {
			console->Warn("MemTexRegen: unexpected image format %d\n", (int)fmt);
			return;
		}
		auto ImageData = Memory::VMT<uint8_t *(__rescall *)(IVTFTexture *)>(vtf_tex, Offsets::ImageData);
		std::memcpy(ImageData(vtf_tex), this->bgra, this->w * this->h * 4);
	}
	virtual void Release() override { delete this; }
};

bool MaterialSystem::Init() {
	// No vtable copy: the bridge hooks nothing here, it only calls.
	this->materials = Interface::Create(MODULE_NAME("materialsystem"), "VMaterialSystem080", false);
	if (!this->materials) return false;
	this->RemoveMaterial = this->materials->Original<_RemoveMaterial>(Offsets::RemoveMaterial);
	this->KeyValues_SetString = (_KeyValues_SetString)Memory::Scan(MODULE_NAME("materialsystem"), Offsets::KeyValues_SetString);
	return this->KeyValues_SetString != nullptr;
}

void MaterialSystem::Shutdown() {
	Interface::Delete(this->materials);
	this->materials = nullptr;
}

IMaterial *MaterialSystem::CreateMaterial(const char *name, KeyValues *kv) {
	auto fn = this->materials->Original<IMaterial *(__rescall *)(void *, const char *, void *)>(Offsets::CreateMaterial);
	return fn(this->materials->ThisPtr(), name, kv);
}

void MaterialSystem::DestroyMaterial(IMaterial *mat) {
	if (!mat) return;
	auto material = reinterpret_cast<CMaterial_QueueFriendly *>(mat)->m_pRealTimeVersion;
	auto DecrementReferenceCount = Memory::VMT<void(__rescall *)(IMaterialInternal *)>(material, Offsets::DecrementReferenceCount);
	DecrementReferenceCount(material);
	this->RemoveMaterial(this->materials->ThisPtr(), material);
}

IMaterial *MaterialSystem::FindMaterial(const char *materialName, const char *textureGroupName) {
	auto func = (IMaterial * (__rescall *)(void *, const char *, const char *, bool, const char *)) this->materials->Original(Offsets::FindMaterial);
	return func(this->materials->ThisPtr(), materialName, textureGroupName, true, nullptr);
}

ITexture *MaterialSystem::CreateTexture(const char *name, int w, int h, uint8_t *bgra, int extraFlags) {
	auto CreateProceduralTexture = (ITexture * (__rescall *)(void *, const char *, const char *, int, int, ImageFormat, int)) this->materials->Original(Offsets::CreateProceduralTexture);
	ITexture *tex = CreateProceduralTexture(this->materials->ThisPtr(), name, "Portalcraft textures", w, h, IMAGE_FORMAT_BGRA8888,
	                                        TEXTUREFLAGS_NOMIP | TEXTUREFLAGS_NOLOD | TEXTUREFLAGS_SINGLECOPY | extraFlags);
	if (!tex) return nullptr;
	tex->SetTextureRegenerator(new MemTexRegen(bgra, w, h));
	tex->Download();
	return tex;
}

void MaterialSystem::DestroyTexture(ITexture *tex) {
	if (!tex) return;
	auto CreateProceduralTexture = this->materials->Original(Offsets::CreateProceduralTexture);
	auto g_pTextureManager = Memory::DerefDeref(CreateProceduralTexture + Offsets::g_pTextureManager);
	auto RemoveTexture = Memory::VMT<void(__rescall *)(void *, ITexture *)>(g_pTextureManager, Offsets::RemoveTexture);
	RemoveTexture(g_pTextureManager, tex);
}

IMatRenderContext *MaterialSystem::GetRenderContext() {
	auto func = (IMatRenderContext * (__rescall *)(void *)) this->materials->Original(Offsets::GetRenderContext);
	return func(this->materials->ThisPtr());
}
