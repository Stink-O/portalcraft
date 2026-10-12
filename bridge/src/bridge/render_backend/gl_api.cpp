// Portalcraft bridge (bridge #73): see glapi.hpp.
#include "gl_api.hpp"

#include "Modules/Console.hpp"

#include <dlfcn.h>
#include <string>

namespace bridge {
namespace gl {

#define PCB_GLDEF(ret, name, ...) ret (*name)(__VA_ARGS__) = nullptr;
PCB_GLFUNCS(PCB_GLDEF)
#undef PCB_GLDEF

namespace {
using _GetProc = void *(*)(const char *);
_GetProc g_getProc = nullptr;
int g_state = 0;  // 0 not tried, 1 ok, -1 failed
}  // namespace

void *ProcAddress(const char *name) {
	if (!g_getProc) {
		void *h = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_NOLOAD);
		g_getProc = (_GetProc)dlsym(h ? h : RTLD_DEFAULT, "SDL_GL_GetProcAddress");
		if (!g_getProc) return nullptr;
	}
	return g_getProc(name);
}

bool Load() {
	if (g_state) return g_state > 0;
	std::string missing;
#define PCB_GLLOAD(ret, name, ...)                                     \
	name = (ret(*)(__VA_ARGS__))ProcAddress("gl" #name);               \
	if (!name) missing += " gl" #name;
	PCB_GLFUNCS(PCB_GLLOAD)
#undef PCB_GLLOAD
	g_state = missing.empty() ? 1 : -1;
	if (!missing.empty()) console->Warn("[bridge] gl: missing entry points:%s\n", missing.c_str());
	return g_state > 0;
}

}  // namespace gl
}  // namespace bridge
