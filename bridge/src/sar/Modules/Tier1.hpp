#pragma once
// Portalcraft bridge: trimmed SourceAutoRecord Tier1 module (ConCommand
// registration and the KeyValues allocator), without SAR's console filter.
#include "Interface.hpp"
#include "Utils/SDK.hpp"

#ifdef _WIN32
#	define TIER1 "vstdlib"  // UNVERIFIED (Windows port): upstream SAR's name
#else
#	define TIER1 "libvstdlib"
#endif

class Tier1 {
public:
	Interface *g_pCVar = nullptr;
	_RegisterConCommand RegisterConCommand = nullptr;
	_UnregisterConCommand UnregisterConCommand = nullptr;
	_FindCommandBase FindCommandBase = nullptr;
	void *ConCommand_VTable = nullptr;
	using _KeyValuesSystem = IKeyValuesSystem *(__attribute__((cdecl)) *)();
	_KeyValuesSystem KeyValuesSystem = nullptr;

	bool Init();
	void Shutdown();
};

extern Tier1 *tier1;

// SAR's Command class, reduced to what the bridge needs.
class Command {
	ConCommand *ptr;
	bool isRegistered = false;
public:
	Command(const char *name, _CommandCallback cb, const char *help, int flags = 0);
	ConCommand *ThisPtr() { return ptr; }
	void Register();
	void Unregister();
	static std::vector<Command *> &GetList();
	static int RegisterAll();
	static void UnregisterAll();
};

#define CON_COMMAND(name, description)                        \
	void name##_callback(const CCommand &args);                  \
	Command name(#name, name##_callback, description); \
	void name##_callback(const CCommand &args)
