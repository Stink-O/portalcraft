// Portalcraft bridge (bridge #73): cut-away world, OpenGL backend (Linux, togl). The renderer-neutral half is cut_logic.cpp; the
// interface is render_backend.hpp (rb::CutBackend*). Portal 2's world is not drawn inside dug cells, at the fragment level, by
// Portal 2's own shaders (DESIGN-dig-render.md sections 2 and 3).
//
// Portal 2 on Linux draws through togl, which turns every D3D shader into GLSL text and links each vertex/pixel pair into a GL
// program. The link is intercepted (togl calls GL through a table of function pointers in its own data; the entry for
// glLinkProgramARB is pointed at LinkHook) and a few lines are added to the pair before it links:
//   * vertex shader: the clip position togl computed, captured just before togl flips it into GL's conventions (the line
//     "vTempPos.y = -vTempPos.y;" that every vertex shader togl writes ends with), as a varying pc_clip = (x, y, w);
//   * fragment shader, first thing in main: the world position from pc_clip (three rows of view x projection, which Portal 2's
//     oblique portal clipping never changes: it only rewrites the depth row), a step of e = 0.25 u along the surface's normal each way
//     (bridge #76: the triangle's normal from the screen derivatives of the world position; it was the view ray, which at a grazing
//     angle removed up to 0.25 u of a wall face beside the hole, where nothing is drawn behind: a void hairline at the mouth), both
//     cells looked up in a sparse 3D texture of dug cells, and `discard` if either is dug: the cell behind the fragment is dug (the
//     surface belonged to the dug solid) or the cell in front of it is (a host face on the hole's boundary, which Minecraft's
//     revealed blocks and hole walls cover).
// The parameters live in GL state togl never touches: gl_TextureMatrix[4..7] (set with glMatrixLoadfEXT) and texture units 30 / 31
// (a brick index over the whole +-16384 u map and a brick atlas: no window, no distance limit). They are set by a functor on the
// render thread at the start of every world draw (IVRenderView DrawWorldLists and DrawTranslucentSurfaces, hooked in visclamp.cpp)
// from that view's real matrices, and the enable flag is cleared after it, so only Portal 2's WORLD is cut (brushes,
// displacements, decals, overlays, water, translucent world surfaces, their flashlight passes; in every view: main, portal,
// reflection, shadow maps), never entities, our blocks or the HUD.
//
// pc_dig_render 2 turns it on (and digrender.cpp's punch and peel off). A program whose text has no anchor links unpatched (counted),
// a compile or link failure falls back to togl's own shaders (counted, logged once with the GL log), and nothing is hooked until the
// switch is on. pc_cut_audit 1 paints cut fragments magenta instead of discarding them (the harness's coverage check).
#include "bridge/bridge.hpp"
#include "bridge/render_backend.hpp"
#include "gl_api.hpp"

#include "Modules/MaterialSystem.hpp"
#include "Modules/Tier1.hpp"
#include "Utils/SDK/MeshBuilder.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace bridge {

void RunOnRenderThread(void (*fn)(void *), void *arg);  // terrain.cpp
void CutNoteError(const std::string &e);                 // cut_logic.cpp

namespace {

using namespace gl;
constexpr int kUnitIdx = rb::kCutUnitIdx, kUnitAtl = rb::kCutUnitAtl;

// ---- counters (any thread) -----------------------------------------------------------------------------------------------------
std::atomic<long> g_links{0}, g_patched{0}, g_noAnchor{0}, g_compileFail{0}, g_linkFail{0}, g_already{0};
void NoteError(const std::string &e) { CutNoteError(e); }
// ---- the shader text ---------------------------------------------------------------------------------------------------------
const char *kMarker = "pc_cut_v1";
const char *kVsDecl = "\n// pc_cut_v1 (Portalcraft: dug cells)\nvarying vec3 pc_clip;\n";
const char *kVsCapture = "pc_clip = vTempPos.xyw;\n";
const char *kFsDecl =
    "\n// pc_cut_v1 (Portalcraft: dug cells)\n"
    "varying vec3 pc_clip;\n"
    "uniform sampler3D zzzz_pcCutIdx;\n"
    "uniform sampler3D zzzz_pcCutAtl;\n"
    "float pc_dug(vec3 c) {\n"
    "  vec4 o = gl_TextureMatrix[5][1];\n"
    "  vec4 n = gl_TextureMatrix[5][2];\n"
    "  vec3 r = c - o.xyz;\n"
    "  vec3 b = floor(r / 8.0);\n"
    "  if (any(lessThan(b, vec3(0.0))) || any(greaterThanEqual(b, n.xyz))) return 0.0;\n"
    "  vec2 rg = texture3D(zzzz_pcCutIdx, (b + 0.5) / n.xyz).rg;\n"
    "  float s = floor(rg.r * 255.0 + 0.5) + 256.0 * floor(rg.g * 255.0 + 0.5);\n"
    "  if (s < 0.5) return 0.0;\n"
    "  s -= 1.0;\n"
    "  vec3 sb = vec3(mod(s, 16.0), mod(floor(s / 16.0), 16.0), floor(s / 256.0));\n"
    "  vec3 l = r - b * 8.0;\n"
    "  return texture3D(zzzz_pcCutAtl, (sb * 8.0 + floor(l) + 0.5) / 128.0).r;\n"
    "}\n"
    "bool pc_cut() {\n"
    "  vec4 p = gl_TextureMatrix[5][0];\n"
    "  vec3 w = (gl_TextureMatrix[7] * vec4(pc_clip, 1.0)).xyz;\n"
    "  vec3 nn = cross(dFdx(w), dFdy(w));\n"
    "  if (p.x < 0.5) return false;\n"
    "  float nl = length(nn);\n"
    "  vec3 d;\n"
    "  if (nl > 1e-12 && p.w < 0.5) d = nn * (p.y / nl);\n"
    "  else {\n"
    "    d = w - gl_TextureMatrix[5][3].xyz;\n"
    "    float dl = length(d);\n"
    "    if (dl < 0.001) return false;\n"
    "    d *= p.y / dl;\n"
    "  }\n"
    "  vec3 cb = floor((gl_TextureMatrix[6] * vec4(w + d, 1.0)).xyz);\n"
    "  vec3 cf = floor((gl_TextureMatrix[6] * vec4(w - d, 1.0)).xyz);\n"
    "  return pc_dug(cb) > 0.5 || pc_dug(cf) > 0.5;\n"
    "}\n";

bool Find(const std::string &s, const char *what, size_t &at, size_t from = 0) {
	at = s.find(what, from);
	return at != std::string::npos;
}

bool PatchVs(const std::string &src, std::string &out) {
	size_t a, m;
	if (!Find(src, "vTempPos.y = -vTempPos.y;", a) || !Find(src, "void main()", m) || m > a) return false;
	out = src.substr(0, m) + kVsDecl + src.substr(m, a - m) + kVsCapture + src.substr(a);
	return true;
}
bool PatchFs(const std::string &src, std::string &out) {
	size_t m, b;
	if (!Find(src, "void main()", m) || !Find(src, "{", b, m)) return false;
	const bool fragColor = src.find("gl_FragColor") != std::string::npos;
	const std::string call = std::string("\n  if (pc_cut()) { if (gl_TextureMatrix[5][0].z > 0.5) { ") + (fragColor ? "gl_FragColor" : "gl_FragData[0]") + " = vec4(1.0, 0.0, 1.0, 1.0); return; } discard; }\n";
	out = src.substr(0, m) + kFsDecl + src.substr(m, b + 1 - m) + call + src.substr(b + 1);
	return true;
}

// ---- the link hook (render thread, inside togl) ------------------------------------------------------------------------------
using _Link = void (*)(GLhandleARB);
_Link g_origLink = nullptr;
std::vector<void **> g_patchedSlots;
struct Cached {
	uint64_t hash;
	GLhandleARB patched;
};
std::unordered_map<GLhandleARB, Cached> g_shaderCache;  // original shader object -> its patched copy (render thread only)
int g_failLogs = 0;
std::string g_sampleVs, g_sampleFs;  // one patched pair, written to the cache for reference (generated text, not shipped)

uint64_t Hash(const std::string &s) {
	uint64_t h = 1469598103934665603ull;
	for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
	return h;
}
std::string Source(GLhandleARB sh) {
	GLint len = 0;
	GetObjectParameterivARB(sh, OBJECT_SHADER_SOURCE_LENGTH_ARB, &len);
	if (len <= 0 || len > (1 << 22)) return std::string();
	std::string s((size_t)len, '\0');
	GLsizei got = 0;
	GetShaderSourceARB(sh, len, &got, &s[0]);
	s.resize((size_t)std::max(0, got));
	return s;
}
GLhandleARB Compile(GLenum type, const std::string &src) {
	GLhandleARB sh = CreateShaderObjectARB(type);
	const GLchar *p = src.c_str();
	const GLint l = (GLint)src.size();
	ShaderSourceARB(sh, 1, &p, &l);
	CompileShaderARB(sh);
	GLint ok = 0;
	GetObjectParameterivARB(sh, OBJECT_COMPILE_STATUS_ARB, &ok);
	if (!ok) {
		if (g_failLogs < 4) {
			++g_failLogs;
			char log[1200] = "";
			GLsizei n = 0;
			GetInfoLogARB(sh, sizeof log - 1, &n, log);
			NoteError(std::string("a patched ") + (type == VERTEX_SHADER ? "vertex" : "fragment") + " shader did not compile: " + log);
		}
		DeleteObjectARB(sh);
		return 0;
	}
	return sh;
}
// The patched copy of an original shader object (compiled once per original text).
GLhandleARB PatchedOf(GLhandleARB orig, GLenum type, bool &noAnchor) {
	noAnchor = false;
	const std::string src = Source(orig);
	if (src.empty()) return noAnchor = true, 0;
	if (src.find(kMarker) != std::string::npos) return orig;  // (already ours)
	const uint64_t h = Hash(src);
	auto it = g_shaderCache.find(orig);
	if (it != g_shaderCache.end() && it->second.hash == h) return it->second.patched;
	std::string out;
	if (!(type == VERTEX_SHADER ? PatchVs(src, out) : PatchFs(src, out))) return noAnchor = true, 0;
	const GLhandleARB sh = Compile(type, out);
	if (!sh) return 0;
	g_shaderCache[orig] = {h, sh};
	if (type == VERTEX_SHADER && g_sampleVs.empty()) g_sampleVs = out;
	if (type == FRAGMENT_SHADER && g_sampleFs.empty()) g_sampleFs = out;
	return sh;
}

std::unordered_map<GLhandleARB, int> &ProgState();
void LinkHook(GLhandleARB prog) {
	++g_links;
	ProgState().erase(prog);  // (a new link of this handle: judged again)
	if (DigRenderMode() != 2) return g_origLink(prog);
	GLhandleARB objs[8];
	GLsizei n = 0;
	GetAttachedObjectsARB(prog, 8, &n, objs);
	GLhandleARB vs = 0, fs = 0;
	for (int i = 0; i < n; ++i) {
		GLint t = 0;
		GetObjectParameterivARB(objs[i], OBJECT_SUBTYPE_ARB, &t);
		if (t == (GLint)VERTEX_SHADER) vs = objs[i];
		else if (t == (GLint)FRAGMENT_SHADER) fs = objs[i];
	}
	if (!vs || !fs) return ++g_noAnchor, g_origLink(prog);
	bool na1 = false, na2 = false;
	const GLhandleARB pvs = PatchedOf(vs, VERTEX_SHADER, na1);
	if (pvs == vs) return ++g_already, g_origLink(prog);
	const GLhandleARB pfs = pvs ? PatchedOf(fs, FRAGMENT_SHADER, na2) : 0;
	if (!pvs || !pfs || pfs == fs) {
		if (na1 || na2) ++g_noAnchor;
		else ++g_compileFail;
		return g_origLink(prog);
	}
	DetachObjectARB(prog, vs), DetachObjectARB(prog, fs);
	AttachObjectARB(prog, pvs), AttachObjectARB(prog, pfs);
	g_origLink(prog);
	GLint ok = 0;
	GetObjectParameterivARB(prog, OBJECT_LINK_STATUS_ARB, &ok);
	if (!ok) {
		++g_linkFail;
		if (g_failLogs < 6) {
			++g_failLogs;
			char log[1200] = "";
			GLsizei ln = 0;
			GetInfoLogARB(prog, sizeof log - 1, &ln, log);
			NoteError(std::string("a patched program did not link (togl's own is used): ") + log);
		}
		DetachObjectARB(prog, pvs), DetachObjectARB(prog, pfs);
		AttachObjectARB(prog, vs), AttachObjectARB(prog, fs);
		g_origLink(prog);
		return;
	}
	const GLint li = GetUniformLocationARB(prog, "zzzz_pcCutIdx"), la = GetUniformLocationARB(prog, "zzzz_pcCutAtl");
	if (li >= 0) ProgramUniform1iEXT(prog, li, kUnitIdx);
	if (la >= 0) ProgramUniform1iEXT(prog, la, kUnitAtl);
	ProgState()[prog] = 1;
	++g_patched;
}

// ---- programs linked before the hook: patched at their first use -------------------------------------------------------------
// The program is linked again with the patched pair. Linking resets every uniform, and togl set its own (sampler units, constant
// arrays) once after its link and remembers them, so all active uniforms are read before and written back after, by name. If the
// relink fails, togl's own shaders are put back and linked again (values restored the same way).
using _Use = void (*)(GLhandleARB);
_Use g_origUse = nullptr;
void **g_useSlot = nullptr;
std::unordered_map<GLhandleARB, int> g_progState;  // 1 patched, 2 left alone
std::unordered_map<GLhandleARB, int> &ProgState() { return g_progState; }
std::atomic<long> g_relinked{0}, g_relinkFail{0};

struct UniVal {
	std::string name;
	GLenum type;
	GLint loc;
	std::vector<float> f;
	std::vector<GLint> i;
};
int Comps(GLenum t) {
	switch (t) {
	case 0x1406: case 0x1404: case 0x8B56: return 1;
	case 0x8B50: case 0x8B53: case 0x8B57: return 2;
	case 0x8B51: case 0x8B54: case 0x8B58: return 3;
	case 0x8B52: case 0x8B55: case 0x8B59: case 0x8B5A: return 4;
	case 0x8B5B: return 9;
	case 0x8B5C: return 16;
	default: return 1;  // samplers
	}
}
bool IsFloat(GLenum t) { return t == 0x1406 || (t >= 0x8B50 && t <= 0x8B52) || (t >= 0x8B5A && t <= 0x8B5C); }
void SaveUniforms(GLhandleARB prog, std::vector<UniVal> &out) {
	out.clear();
	GLint n = 0;
	GetObjectParameterivARB(prog, OBJECT_ACTIVE_UNIFORMS_ARB, &n);
	for (GLint k = 0; k < n && k < 512; ++k) {
		char name[256] = "";
		GLsizei len = 0;
		GLint size = 0;
		GLenum type = 0;
		GetActiveUniformARB(prog, (GLuint)k, sizeof name - 1, &len, &size, &type, name);
		std::string base(name);
		if (base.rfind("gl_", 0) == 0) continue;
		const size_t br = base.find('[');
		if (br != std::string::npos) base = base.substr(0, br);
		for (GLint e = 0; e < std::max(1, size); ++e) {
			UniVal v;
			v.name = size > 1 ? base + "[" + std::to_string(e) + "]" : (br != std::string::npos ? base + "[0]" : base);
			v.type = type;
			v.loc = GetUniformLocationARB(prog, v.name.c_str());
			if (v.loc < 0) continue;
			if (IsFloat(type)) v.f.resize(Comps(type)), GetUniformfvARB(prog, v.loc, v.f.data());
			else v.i.resize(Comps(type)), GetUniformivARB(prog, v.loc, v.i.data());
			out.push_back(std::move(v));
		}
	}
}
void RestoreUniforms(GLhandleARB prog, const std::vector<UniVal> &vals) {  // (prog is bound)
	for (const UniVal &v : vals) {
		const GLint loc = GetUniformLocationARB(prog, v.name.c_str());
		if (loc < 0) continue;
		const GLenum t = v.type;
		if (IsFloat(t)) {
			if (t == 0x8B5A) UniformMatrix2fv(loc, 1, 0, v.f.data());
			else if (t == 0x8B5B) UniformMatrix3fv(loc, 1, 0, v.f.data());
			else if (t == 0x8B5C) UniformMatrix4fv(loc, 1, 0, v.f.data());
			else if (v.f.size() == 1) Uniform1fv(loc, 1, v.f.data());
			else if (v.f.size() == 2) Uniform2fv(loc, 1, v.f.data());
			else if (v.f.size() == 3) Uniform3fv(loc, 1, v.f.data());
			else Uniform4fv(loc, 1, v.f.data());
		} else {
			if (v.i.size() == 1) Uniform1iv(loc, 1, v.i.data());
			else if (v.i.size() == 2) Uniform2iv(loc, 1, v.i.data());
			else if (v.i.size() == 3) Uniform3iv(loc, 1, v.i.data());
			else Uniform4iv(loc, 1, v.i.data());
		}
	}
}

std::atomic<long> g_relinkUs{0}, g_relinkWorstUs{0};
void UseHook(GLhandleARB prog) {
	g_origUse(prog);
	if (!prog || DigRenderMode() != 2 || !g_origLink) return;
	auto st = g_progState.find(prog);
	if (st != g_progState.end()) return;
	g_progState[prog] = 2;
	GLhandleARB objs[8];
	GLsizei n = 0;
	GetAttachedObjectsARB(prog, 8, &n, objs);
	GLhandleARB vs = 0, fs = 0;
	for (int i = 0; i < n; ++i) {
		GLint t = 0;
		GetObjectParameterivARB(objs[i], OBJECT_SUBTYPE_ARB, &t);
		if (t == (GLint)VERTEX_SHADER) vs = objs[i];
		else if (t == (GLint)FRAGMENT_SHADER) fs = objs[i];
	}
	if (!vs || !fs) return;
	// (bridge #75) the cost of patching at first use (compiles + relink + uniform save/restore), on the render thread
	struct Timer {
		std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
		~Timer() {
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			g_relinkUs += (long)(ms * 1000), g_relinkWorstUs = std::max(g_relinkWorstUs.load(), (long)(ms * 1000));
		}
	} timer;
	bool na1 = false, na2 = false;
	const GLhandleARB pvs = PatchedOf(vs, VERTEX_SHADER, na1);
	if (pvs == vs) return void(g_progState[prog] = 1);
	const GLhandleARB pfs = pvs ? PatchedOf(fs, FRAGMENT_SHADER, na2) : 0;
	if (!pvs || !pfs || pfs == fs) {
		if (na1 || na2) ++g_noAnchor;
		else ++g_compileFail;
		return;
	}
	std::vector<UniVal> vals;
	SaveUniforms(prog, vals);
	DetachObjectARB(prog, vs), DetachObjectARB(prog, fs);
	AttachObjectARB(prog, pvs), AttachObjectARB(prog, pfs);
	g_origLink(prog);
	GLint ok = 0;
	GetObjectParameterivARB(prog, OBJECT_LINK_STATUS_ARB, &ok);
	if (!ok) {
		++g_relinkFail;
		DetachObjectARB(prog, pvs), DetachObjectARB(prog, pfs);
		AttachObjectARB(prog, vs), AttachObjectARB(prog, fs);
		g_origLink(prog);
	}
	g_origUse(prog);
	// togl keeps the uniform locations it read after its own link: they must not move
	int moved = 0;
	if (ok)
		for (const UniVal &v : vals)
			if (GetUniformLocationARB(prog, v.name.c_str()) != v.loc) ++moved;
	if (ok && moved) {
		++g_relinkFail;
		ok = 0;
		DetachObjectARB(prog, pvs), DetachObjectARB(prog, pfs);
		AttachObjectARB(prog, vs), AttachObjectARB(prog, fs);
		g_origLink(prog);
		g_origUse(prog);
	}
	RestoreUniforms(prog, vals);
	static int logged = 0;
	if (logged < 6) {
		++logged;
		const GLenum e = GetError();
		console->Print("[bridge] cut: program %u relinked at first use: %s, %zu uniform values kept, %d locations moved, GL error 0x%x\n", prog, ok ? "patched" : "togl's own again", vals.size(), moved, e);
	}
	if (ok) {
		const GLint li = GetUniformLocationARB(prog, "zzzz_pcCutIdx"), la = GetUniformLocationARB(prog, "zzzz_pcCutAtl");
		GLint one[1] = {kUnitIdx};
		if (li >= 0) Uniform1iv(li, 1, one);
		one[0] = kUnitAtl;
		if (la >= 0) Uniform1iv(la, 1, one);
		g_progState[prog] = 1;
		++g_relinked;
	}
}

// ---- installing: point togl's table entry for glLinkProgramARB at LinkHook --------------------------------------------------
bool PatchTogl() {
	void *cands[3] = {ProcAddress("glLinkProgramARB"), dlsym(RTLD_DEFAULT, "glLinkProgramARB"), nullptr};
	using _GetProc = void *(*)(const unsigned char *);
	if (auto gxp = (_GetProc)dlsym(RTLD_DEFAULT, "glXGetProcAddressARB")) cands[2] = gxp((const unsigned char *)"glLinkProgramARB");
	int found = 0;
	// togl keeps its GL entry points in one object on the heap, which it hands out through its exported GetOpenGLEntryPoints
	void *htogl = dlopen("libtogl.so", RTLD_NOW | RTLD_NOLOAD);
	using _GetEP = void *(*)(void *);
	auto getEP = htogl ? (_GetEP)dlsym(htogl, "GetOpenGLEntryPoints") : nullptr;
	void *ep = getEP ? getEP(nullptr) : nullptr;
	if (ep) {
		// glUseProgram's entry, for programs linked before the hook (togl links its pairs early, from its program cache)
		{
			void **us = (void **)((char *)ep + 0x19c);
			void *want = ProcAddress("glUseProgram");
			if (*us && (*us == want || *us == dlsym(RTLD_DEFAULT, "glUseProgram"))) g_useSlot = us, g_origUse = (_Use)*us;
		}
		for (int i = 0; i < 0x398 / 4; ++i) {
			void **slot = (void **)ep + i;
			for (void *c : cands)
				if (c && *slot == c && (!g_origLink || (void *)g_origLink == c)) {
					g_origLink = (_Link)c;
					g_patchedSlots.push_back(slot);
					++found;
					break;
				}
		}
		if (!found) {
			std::string w;
			char t[32];
			for (int i = 0; i < 12; ++i) std::snprintf(t, sizeof t, " %p", ((void **)ep)[i]), w += t;
			console->Print("[bridge] cut: togl's entry-point object %p does not hold glLinkProgramARB as %p / %p / %p; its first words:%s\n", ep, cands[0], cands[1], cands[2], w.c_str());
		}
	}
	std::ifstream maps("/proc/self/maps");
	std::string line;
	while (!found && std::getline(maps, line)) {
		if (line.find("libtogl") == std::string::npos) continue;
		unsigned long a = 0, b = 0;
		char perm[8] = "";
		if (std::sscanf(line.c_str(), "%lx-%lx %7s", &a, &b, perm) != 3 || perm[0] != 'r' || perm[1] != 'w') continue;
		for (unsigned long p = a; p + 4 <= b; p += 4) {
			void **slot = (void **)p;
			void *v = *slot;
			if (!v) continue;
			for (void *c : cands)
				if (c && v == c) {
					if (!g_origLink) g_origLink = (_Link)v;
					if ((void *)g_origLink != v) continue;  // (two different pointers: keep the first kind)
					g_patchedSlots.push_back(slot);
					++found;
				}
		}
	}
	if (!found || !g_origLink) return NoteError("togl's glLinkProgramARB entry was not found in its data (another renderer?): the cut stays off"), false;
	for (void **s : g_patchedSlots) __atomic_store_n(s, (void *)&LinkHook, __ATOMIC_SEQ_CST);
	if (g_useSlot) __atomic_store_n(g_useSlot, (void *)&UseHook, __ATOMIC_SEQ_CST);
	else NoteError("togl's glUseProgram entry not recognised: programs linked before the hook stay unpatched");
	console->Print("[bridge] cut: togl's link entry hooked (%d table slots)\n", found);
	return true;
}
void UnpatchTogl() {
	if (g_useSlot) __atomic_store_n(g_useSlot, (void *)g_origUse, __ATOMIC_SEQ_CST), g_useSlot = nullptr;
	for (void **s : g_patchedSlots) __atomic_store_n(s, (void *)g_origLink, __ATOMIC_SEQ_CST);
	g_patchedSlots.clear();
}

// ---- the dug-cell textures ---------------------------------------------------------------------------------------------------
GLuint g_texIdx = 0, g_texAtl = 0;
int g_texDims[3] = {0, 0, 0};


void UploadBricks(const rb::CutBricks &d) {
	GLint align = 4, rowLen = 0, skipR = 0, skipP = 0, skipI = 0, imgH = 0, ubuf = 0;
	GetIntegerv(UNPACK_ALIGNMENT, &align), GetIntegerv(UNPACK_ROW_LENGTH, &rowLen), GetIntegerv(UNPACK_SKIP_ROWS, &skipR), GetIntegerv(UNPACK_SKIP_PIXELS, &skipP);
	GetIntegerv(UNPACK_SKIP_IMAGES, &skipI), GetIntegerv(UNPACK_IMAGE_HEIGHT, &imgH), GetIntegerv(PIXEL_UNPACK_BUFFER_BINDING, &ubuf);
	BindBuffer(PIXEL_UNPACK_BUFFER, 0);
	PixelStorei(UNPACK_ALIGNMENT, 1), PixelStorei(UNPACK_ROW_LENGTH, 0), PixelStorei(UNPACK_SKIP_ROWS, 0), PixelStorei(UNPACK_SKIP_PIXELS, 0);
	PixelStorei(UNPACK_SKIP_IMAGES, 0), PixelStorei(UNPACK_IMAGE_HEIGHT, 0);
	auto params = [](GLuint t) {
		TextureParameteriEXT(t, TEXTURE_3D, TEXTURE_MIN_FILTER, NEAREST), TextureParameteriEXT(t, TEXTURE_3D, TEXTURE_MAG_FILTER, NEAREST);
		TextureParameteriEXT(t, TEXTURE_3D, TEXTURE_WRAP_S, CLAMP_TO_EDGE), TextureParameteriEXT(t, TEXTURE_3D, TEXTURE_WRAP_T, CLAMP_TO_EDGE), TextureParameteriEXT(t, TEXTURE_3D, TEXTURE_WRAP_R, CLAMP_TO_EDGE);
		TextureParameteriEXT(t, TEXTURE_3D, TEXTURE_BASE_LEVEL, 0), TextureParameteriEXT(t, TEXTURE_3D, TEXTURE_MAX_LEVEL, 0);
	};
	if (!g_texIdx) GenTextures(1, &g_texIdx), params(g_texIdx);
	if (!g_texAtl) {
		GenTextures(1, &g_texAtl), params(g_texAtl);
		TextureImage3DEXT(g_texAtl, TEXTURE_3D, 0, R8, 128, 128, 128, 0, RED, UNSIGNED_BYTE, nullptr);
	}
	if (g_texDims[0] != d.dims[0] || g_texDims[1] != d.dims[1] || g_texDims[2] != d.dims[2]) {
		TextureImage3DEXT(g_texIdx, TEXTURE_3D, 0, RG8, d.dims[0], d.dims[1], d.dims[2], 0, RG, UNSIGNED_BYTE, d.index.data());
		std::memcpy(g_texDims, d.dims, sizeof g_texDims);
	} else {
		TextureSubImage3DEXT(g_texIdx, TEXTURE_3D, 0, 0, 0, 0, d.dims[0], d.dims[1], d.dims[2], RG, UNSIGNED_BYTE, d.index.data());
	}
	TextureSubImage3DEXT(g_texAtl, TEXTURE_3D, 0, 0, 0, 0, 128, 128, 128, RED, UNSIGNED_BYTE, d.atlas.data());
	BindBuffer(PIXEL_UNPACK_BUFFER, (GLuint)ubuf);
	PixelStorei(UNPACK_ALIGNMENT, align), PixelStorei(UNPACK_ROW_LENGTH, rowLen), PixelStorei(UNPACK_SKIP_ROWS, skipR), PixelStorei(UNPACK_SKIP_PIXELS, skipP);
	PixelStorei(UNPACK_SKIP_IMAGES, skipI), PixelStorei(UNPACK_IMAGE_HEIGHT, imgH);
}

// gl_TextureMatrix[unit] = m (column-major). togl never uses the fixed-function matrices; whichever way they are written, the
// active texture unit and the matrix mode are left as they were.
float g_off[16] = {0};  // enable = 0
int g_tmPath = 0;       // 0 glMatrixLoadfEXT(TEXTUREi), 1 active texture + matrix mode + glLoadMatrixf (put back after)
void LoadTexMatrix(int unit, const float *m) {
	if (g_tmPath == 0) return MatrixLoadfEXT(TEXTURE0 + unit, m);
	GLint act = 0, mode = 0;
	GetIntegerv(ACTIVE_TEXTURE, &act), GetIntegerv(MATRIX_MODE, &mode);
	ActiveTexture(TEXTURE0 + unit);
	MatrixMode(TEXTURE);
	LoadMatrixf(m);
	MatrixMode((GLenum)mode);
	ActiveTexture((GLenum)act);
}

// ---- (bridge #75) the GPU cost of Portal 2's world draws: GL timestamps around every world draw call ---------------------------------
struct PerfQ {
	GLuint a = 0, b = 0;
	long frame = 0;
};
std::vector<GLuint> g_qFree;
std::vector<PerfQ> g_qPending;
GLuint g_qOpen = 0;
std::mutex g_perfMu;
double g_perfGpuNs = 0;
long g_perfDraws = 0, g_perfLost = 0;
GLuint TakeQuery() {
	if (g_qFree.empty()) {
		GLuint q[64];
		GenQueries(64, q);
		g_qFree.assign(q, q + 64);
	}
	const GLuint q = g_qFree.back();
	g_qFree.pop_back();
	return q;
}
void PollQueries() {
	for (size_t i = 0; i < g_qPending.size();) {
		GLint ok = 0;
		GetQueryObjectiv(g_qPending[i].b, QUERY_RESULT_AVAILABLE, &ok);
		if (!ok) {
			++i;
			continue;
		}
		uint64_t ta = 0, tb = 0;
		GetQueryObjectui64v(g_qPending[i].a, QUERY_RESULT, &ta);
		GetQueryObjectui64v(g_qPending[i].b, QUERY_RESULT, &tb);
		{
			std::lock_guard<std::mutex> lk(g_perfMu);
			if (tb > ta) g_perfGpuNs += (double)(tb - ta);
			++g_perfDraws;
		}
		g_qFree.push_back(g_qPending[i].a), g_qFree.push_back(g_qPending[i].b);
		g_qPending[i] = g_qPending.back();
		g_qPending.pop_back();
	}
}

}  // namespace

// ---- rb::CutBackend* (the interface in render_backend.hpp) ---------------------------------------------------------------------------
namespace rb {

bool CutBackendInstall(std::string &err) {
	if (!gl::Load()) {
		err = "GL entry points missing: the cut stays off";
		return false;
	}
	GLint units = 0;
	GetIntegerv(MAX_TEXTURE_IMAGE_UNITS, &units);
	if (units < 32) {
		err = "fewer than 32 fragment texture units: the cut stays off";
		return false;
	}
	if (!PatchTogl()) return false;  // (it noted its own error)
	// (programs linked before the hook are patched at their first use: UseHook)
	GetError();
	MatrixLoadfEXT(TEXTURE0 + 5, g_off);
	if (GetError() != NO_ERROR) {
		g_tmPath = 1;
		console->Print("[bridge] cut: glMatrixLoadfEXT refused a texture unit: the matrices go through the active texture unit instead\n");
	}
	LoadTexMatrix(5, g_off);
	GetError();
	return true;
}

void CutBackendUpload(const CutBricks &d) { UploadBricks(d); }

void CutBackendViewOn(const CutView &v) {
	float m7[16] = {0}, m6[16] = {0}, m5[16] = {0};
	// column-major: m[col * 4 + row]
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 4; ++c) m7[c * 4 + r] = v.worldFromClip[r][c];
	m7[15] = 1;
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 4; ++c) m6[c * 4 + r] = v.cellFromWorld[r][c];
	m6[15] = 1;
	m5[0] = 1.0f, m5[1] = v.eps, m5[2] = v.audit ? 1.0f : 0.0f, m5[3] = v.viewRay ? 1.0f : 0.0f;
	m5[4] = (float)v.org[0], m5[5] = (float)v.org[1], m5[6] = (float)v.org[2];
	m5[8] = (float)v.dims[0], m5[9] = (float)v.dims[1], m5[10] = (float)v.dims[2], m5[11] = (float)v.brick;
	m5[12] = v.eye[0], m5[13] = v.eye[1], m5[14] = v.eye[2], m5[15] = 1.0f;
	LoadTexMatrix(7, m7);
	LoadTexMatrix(6, m6);
	LoadTexMatrix(5, m5);
	BindMultiTextureEXT(TEXTURE0 + kUnitIdx, TEXTURE_3D, g_texIdx);
	BindMultiTextureEXT(TEXTURE0 + kUnitAtl, TEXTURE_3D, g_texAtl);
}

void CutBackendViewOff() { LoadTexMatrix(5, g_off); }

void CutBackendUninstall() {
	if (!g_patchedSlots.empty()) UnpatchTogl();
}

std::string CutBackendCounters() {
	char buf[400];
	std::snprintf(buf, sizeof buf, "%ld links seen, %ld patched, %ld without the anchor, %ld already patched, %ld compile / %ld link failures, %ld relinked at first use (%ld failed)", g_links.load(), g_patched.load(),
	              g_noAnchor.load(), g_already.load(), g_compileFail.load(), g_linkFail.load(), g_relinked.load(), g_relinkFail.load());
	return buf;
}
std::string CutBackendPatchTiming() {
	char buf[200];
	std::snprintf(buf, sizeof buf, "[bridge] cut: patching at first use took %.1f ms in all, worst program %.1f ms\n", g_relinkUs.load() / 1000.0, g_relinkWorstUs.load() / 1000.0);
	return buf;
}
void CutBackendDump() {
	std::ofstream(CacheDir() + "/cut_sample_vs.glsl") << g_sampleVs;
	std::ofstream(CacheDir() + "/cut_sample_fs.glsl") << g_sampleFs;
	console->Print("[bridge] cut: wrote %zu + %zu bytes\n", g_sampleVs.size(), g_sampleFs.size());
}

void CutBackendPerfBegin() {
	if (!gl::Load()) return;
	PollQueries();
	if (g_qOpen) return;  // (nested: the outer pair times it)
	g_qOpen = TakeQuery();
	QueryCounter(g_qOpen, TIMESTAMP);
}
void CutBackendPerfEnd() {
	if (!g_qOpen) return;
	const GLuint b = TakeQuery();
	QueryCounter(b, TIMESTAMP);
	g_qPending.push_back({g_qOpen, b, 0});
	g_qOpen = 0;
	if (g_qPending.size() > 4096) {  // never read back (a driver without timestamps): drop them
		std::lock_guard<std::mutex> lk(g_perfMu);
		g_perfLost += (long)g_qPending.size();
		g_qPending.clear();
	}
}
void CutBackendPerfPoll(double &gpuNs, long &draws, long &lost) {
	std::lock_guard<std::mutex> lk(g_perfMu);
	gpuNs = g_perfGpuNs, draws = g_perfDraws, lost = g_perfLost;
}
void CutBackendPerfReset() {
	std::lock_guard<std::mutex> lk(g_perfMu);
	g_perfGpuNs = 0, g_perfDraws = 0;
}
bool CutBackendHasPerf() { return true; }
bool CutBackendAvailable() { return true; }

}  // namespace rb

}  // namespace bridge
