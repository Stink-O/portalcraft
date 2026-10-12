// Portalcraft bridge: the engine's own console output mirrored into bridge.log, for tests (bridge #11).
// Scripted runs only see what the plugin prints; the engine's replies (a refused cheat command, an unknown
// soundscape, a failed input) are invisible. pcb_conlog 1 registers a listener with tier0's logging system
// (the public Alien Swarm SDK interface, tier0/logging.h: one virtual Log(context, message)) inside a pushed
// logging state, so pcb_conlog 0 / unload removes it by popping that state. Off by default.
#include "bridge.hpp"

#include "Modules/Tier1.hpp"

#include <cstdio>
#include <cstring>
#include "platform/platform.hpp"
#include <mutex>

extern FILE *g_bridgeLogFile;

namespace bridge {

void AudioConLine(const char *m);  // audio.cpp

namespace {
struct LoggingListener {
	virtual void Log(const void *context, const char *message) = 0;
};
std::mutex g_mx;
bool g_lineStart = true;
bool g_logOn = false;    // pcb_conlog: mirror into bridge.log
bool g_audioOn = false;  // audio.cpp reads the replies of its sound commands (the ambience fade, asserts)
struct Capture : LoggingListener {
	void Log(const void *, const char *m) override {
		if (!m) return;
		AudioConLine(m);
		if (!g_bridgeLogFile || !g_logOn) return;
		if (g_lineStart && !std::strncmp(m, "[bridge]", 8)) return;  // (the plugin's own lines are in the log already)
		std::lock_guard<std::mutex> lk(g_mx);
		if (g_lineStart) std::fputs("[con] ", g_bridgeLogFile);
		std::fputs(m, g_bridgeLogFile);
		const size_t n = std::strlen(m);
		g_lineStart = n > 0 && m[n - 1] == '\n';
		std::fflush(g_bridgeLogFile);
	}
};
Capture g_capture;
bool g_on = false;
using _Push = void (*)(bool threadLocal, bool clearState);
using _Pop = void (*)(bool threadLocal);
using _Register = void (*)(LoggingListener *);
_Push g_push = nullptr;
_Pop g_pop = nullptr;
_Register g_register = nullptr;
bool Resolve() {
	if (g_push && g_pop && g_register) return true;
	g_push = (_Push)plat::ModuleSymbol("tier0", "LoggingSystem_PushLoggingState");
	g_pop = (_Pop)plat::ModuleSymbol("tier0", "LoggingSystem_PopLoggingState");
	g_register = (_Register)plat::ModuleSymbol("tier0", "LoggingSystem_RegisterLoggingListener");
	return g_push && g_pop && g_register;
}
}  // namespace

// One listener for both users: registered while either wants it.
static void Update() {
	const bool want = g_logOn || g_audioOn;
	if (want == g_on || !Resolve()) return;
	if (want) {
		g_push(false, false);  // a copy of the current state (its listeners kept), ours added on top
		g_register(&g_capture);
	} else {
		g_pop(false);
	}
	g_on = want;
}
void ConLogSet(bool on) {
	g_logOn = on;
	Update();
}
void ConListenAudio(bool on) {
	g_audioOn = on;
	Update();
}
void ConLogShutdown() {
	g_logOn = g_audioOn = false;
	Update();
}

// A console command run now, its output going to the plugin's listener only: not to the console, the notify area
// on screen (developer >= 1) or stdout (bridge #13: the ambience fade's sound commands printed to the console while
// Chell was in Minecraft; a recording must show nothing). The command's own callback is called directly with a
// CCommand built here, inside a thread-local logging state that holds only our listener (other threads keep the
// normal state). Returns false when the command does not exist (the caller may fall back to the console).
bool ConRunSilent(const char *line) {
	if (!Resolve() || !tier1 || !tier1->FindCommandBase) return false;
	CCommand cmd{};
	std::snprintf(cmd.m_pArgSBuffer, sizeof cmd.m_pArgSBuffer, "%s", line);
	std::snprintf(cmd.m_pArgvBuffer, sizeof cmd.m_pArgvBuffer, "%s", line);
	char *p = cmd.m_pArgvBuffer;
	while (*p && cmd.m_nArgc < CCommand::COMMAND_MAX_ARGC) {
		while (*p == ' ') *p++ = 0;
		if (!*p) break;
		cmd.m_ppArgv[cmd.m_nArgc++] = p;
		while (*p && *p != ' ') ++p;
	}
	if (cmd.m_nArgc == 0) return false;
	cmd.m_nArgv0Size = (int)std::strlen(cmd.m_ppArgv[0]);
	// ArgS(): everything after the command name
	const char *rest = line + std::strspn(line, " ");
	rest += cmd.m_nArgv0Size;
	rest += std::strspn(rest, " ");
	std::snprintf(cmd.m_pArgSBuffer, sizeof cmd.m_pArgSBuffer, "%s", rest);
	auto *base = (ConCommandBase *)tier1->FindCommandBase(tier1->g_pCVar->ThisPtr(), cmd.m_ppArgv[0]);
	if (!base || !base->IsCommand()) return false;
	auto *cc = (ConCommand *)base;
	if (!cc->m_bUsingNewCommandCallback || cc->m_bUsingCommandCallbackInterface || !cc->m_fnCommandCallback) return false;
	g_push(true, true);  // this thread only, no listeners...
	g_register(&g_capture);  // ...but ours (AudioConLine parses the replies)
	cc->m_fnCommandCallback(cmd);
	g_pop(true);
	return true;
}

CON_COMMAND(pcb_conlog, "pcb_conlog <0|1> - mirror the engine's console output into bridge.log as [con] lines (tests; off by default)\n") {
	ConLogSet(args.ArgC() > 1 && std::atoi(args[1]) != 0);
	console->Print("[bridge] engine console capture %d\n", g_logOn);
}

}  // namespace bridge
