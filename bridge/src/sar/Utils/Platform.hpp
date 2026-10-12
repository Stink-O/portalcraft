#pragma once

#ifdef _WIN32
#	define __rescalll __thiscall
#else
#	define __rescalll __attribute__((__cdecl__))
#	define __stdcall
#endif

#ifdef _WIN32
#	define MODULE_EXTENSION ".dll"
#	define __rescall __thiscall
#	define PCB_DLL_EXPORT extern "C" __declspec(dllexport)
#	define SEEK_DIR_CUR std::ios_base::cur
#	define STDCALL_NAME(base, param_bytes) "_" base "@" #param_bytes

// (Windows port) the bridge writes its hooks as free functions called with __thiscall (GCC/MinGW supports the attribute on
// free functions), patched in with Hook.hpp's 5-byte jump: no MinHook. SAR's __fastcall-with-edx detour macros and the
// MinHook ones are not used by the bridge and were dropped; these three mirror the Linux ones.
#	define DECL_DETOUR(name, ...)                                   \
		using _##name = int(__rescall *)(void *thisptr, ##__VA_ARGS__); \
		static _##name name;                                            \
		static int __rescall name##_Hook(void *thisptr, ##__VA_ARGS__)
#	define DECL_DETOUR_T(type, name, ...)                            \
		using _##name = type(__rescall *)(void *thisptr, ##__VA_ARGS__); \
		static _##name name;                                             \
		static type __rescall name##_Hook(void *thisptr, ##__VA_ARGS__)
#	define DECL_DETOUR_B(name, ...)                                 \
		using _##name = int(__rescall *)(void *thisptr, ##__VA_ARGS__); \
		static _##name name;                                            \
		static _##name name##Base;                                      \
		static int __rescall name##_Hook(void *thisptr, ##__VA_ARGS__)

#	define DETOUR(name, ...) \
		int __rescall name##_Hook(void *thisptr, ##__VA_ARGS__)
#	define DETOUR_T(type, name, ...) \
		type __rescall name##_Hook(void *thisptr, ##__VA_ARGS__)
#	define DETOUR_B(name, ...) \
		int __rescall name##_Hook(void *thisptr, ##__VA_ARGS__)
#else
#	define MODULE_EXTENSION ".so"
#	define __rescall __attribute__((__cdecl__))
#	define __cdecl __attribute__((__cdecl__))
#	define __fastcall __attribute__((__fastcall__))
#	define PCB_DLL_EXPORT extern "C" __attribute__((visibility("default")))
#	define SEEK_DIR_CUR std::ios_base::cur
#	define STDCALL_NAME(base, param_bytes) base

#	define DECL_DETOUR(name, ...)                                   \
		using _##name = int(__rescall *)(void *thisptr, ##__VA_ARGS__); \
		static _##name name;                                            \
		static int __rescall name##_Hook(void *thisptr, ##__VA_ARGS__)
#	define DECL_DETOUR_T(type, name, ...)                            \
		using _##name = type(__rescall *)(void *thisptr, ##__VA_ARGS__); \
		static _##name name;                                             \
		static type __rescall name##_Hook(void *thisptr, ##__VA_ARGS__)
#	define DECL_DETOUR_B(name, ...)                                 \
		using _##name = int(__rescall *)(void *thisptr, ##__VA_ARGS__); \
		static _##name name;                                            \
		static _##name name##Base;                                      \
		static int __rescall name##_Hook(void *thisptr, ##__VA_ARGS__)

#	define DETOUR(name, ...) \
		int __rescall name##_Hook(void *thisptr, ##__VA_ARGS__)
#	define DETOUR_T(type, name, ...) \
		type __rescall name##_Hook(void *thisptr, ##__VA_ARGS__)
#	define DETOUR_B(name, ...) \
		int __rescall name##_Hook(void *thisptr, ##__VA_ARGS__)
#endif

#define MODULE_NAME(name) name MODULE_EXTENSION
