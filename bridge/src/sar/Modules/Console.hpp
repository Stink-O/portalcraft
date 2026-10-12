#pragma once
// Portalcraft bridge: minimal stand-in for SourceAutoRecord's Console module.
// Resolves tier0's printing functions (same symbols SAR uses) and mirrors
// everything to the bridge log file.
#include "Utils/SDK/Color.hpp"
#include "Utils/Platform.hpp"
#include <cstdio>

#ifdef _WIN32
// (Windows port) the export names are the MSVC-mangled ones from upstream SourceAutoRecord's Console.hpp.
// UNVERIFIED against the Windows tier0.dll until the Windows depot is available (phase 1).
#	define TIER0 "tier0"
#	define CONCOLORMSG_SYMBOL "?ConColorMsg@@YAXABVColor@@PBDZZ"
#	define DEVMSG_SYMBOL "?DevMsg@@YAXPBDZZ"
#	define DEVWARNINGMSG_SYMBOL "?DevWarning@@YAXPBDZZ"
#else
#	define TIER0 "libtier0"
#	define CONCOLORMSG_SYMBOL "_Z11ConColorMsgRK5ColorPKcz"
#	define DEVMSG_SYMBOL "_Z6DevMsgPKcz"
#	define DEVWARNINGMSG_SYMBOL "_Z10DevWarningPKcz"
#endif

class Console {
public:
	using _Msg = void(__attribute__((cdecl)) *)(const char *fmt, ...);
	using _ColorMsg = void(__attribute__((cdecl)) *)(const Color &clr, const char *fmt, ...);
	_Msg Msg = nullptr;
	_Msg Warning = nullptr;
	_Msg DevMsgFn = nullptr;
	_Msg DevWarningFn = nullptr;
	_ColorMsg ColorMsg = nullptr;

	bool Init();
	void Print(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
	void Warn(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
	void DevMsg(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
	void DevWarning(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
};

extern Console *console;
