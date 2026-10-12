// Portalcraft bridge: the direct-GL overlay, OpenGL backend (Linux, togl). The renderer-neutral half is hud_logic.cpp; the interface is
// render_backend.hpp (rb::HudBackend*).
//
// The Minecraft player's hand, held item and GUI come from Minecraft's own rendering (SkyCraft's overlay): one image per Minecraft
// frame. Going through the material system would cost ITexture::Download's ~5-6 ms per image, so this draws it in raw OpenGL
// instead, right before the frame is presented: an inline hook on libSDL2's SDL_GL_SwapWindow (called on Portal 2's render thread,
// after togl blitted its frame into the default framebuffer). The image goes through a pixel-unpack buffer (PBO) into our own GL
// texture and is drawn as one full-screen triangle with SkyCraft's overlay shader (premultiplied alpha; Minecraft's crosshair with
// its invert blend in a pass of its own; the screen cursor drawn by the shader). togl keeps a shadow of the GL state and assumes GL
// matches it, so every piece of state touched here is saved first and restored after (and re-read to check, pcb_glhud_stats).
// Also here (GL only): pcb_record, the in-game video recorder.
#include "bridge/bridge.hpp"
#include "bridge/link.hpp"
#include "bridge/render_backend.hpp"
#include "bridge/platform/platform.hpp"

#include "Hook.hpp"
#include "Modules/Tier1.hpp"
#include "Utils/lodepng.hpp"

#include <atomic>
#include <csignal>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <thread>
#include <time.h>
#include <vector>
#include <sys/stat.h>

namespace bridge {

using namespace skycraft::proto;

// ---- the GL subset used here (no GL headers in the build) -----------------------------------------
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef char GLchar;
typedef intptr_t GLintptr;
typedef intptr_t GLsizeiptr;
typedef unsigned int GLbitfield;

enum : GLenum {
	GL_NO_ERROR = 0,
	GL_TRIANGLES = 0x0004,
	GL_ONE = 1, GL_ZERO = 0,
	GL_ONE_MINUS_SRC_COLOR = 0x0301, GL_SRC_ALPHA = 0x0302, GL_ONE_MINUS_SRC_ALPHA = 0x0303, GL_ONE_MINUS_DST_COLOR = 0x0307,
	GL_CULL_FACE = 0x0B44, GL_DEPTH_TEST = 0x0B71, GL_DEPTH_WRITEMASK = 0x0B72, GL_STENCIL_TEST = 0x0B90, GL_VIEWPORT = 0x0BA2,
	GL_ALPHA_TEST = 0x0BC0, GL_BLEND = 0x0BE2, GL_COLOR_LOGIC_OP = 0x0BF2, GL_READ_BUFFER = 0x0C02, GL_SCISSOR_BOX = 0x0C10, GL_SCISSOR_TEST = 0x0C11,
	GL_COLOR_WRITEMASK = 0x0C23,
	GL_UNPACK_SWAP_BYTES = 0x0CF0, GL_UNPACK_LSB_FIRST = 0x0CF1, GL_UNPACK_ROW_LENGTH = 0x0CF2, GL_UNPACK_SKIP_ROWS = 0x0CF3, GL_UNPACK_SKIP_PIXELS = 0x0CF4,
	GL_UNPACK_ALIGNMENT = 0x0CF5, GL_PACK_ROW_LENGTH = 0x0D02, GL_PACK_SKIP_ROWS = 0x0D03, GL_PACK_SKIP_PIXELS = 0x0D04, GL_PACK_ALIGNMENT = 0x0D05,
	GL_TEXTURE_2D = 0x0DE1, GL_UNSIGNED_BYTE = 0x1401, GL_FLOAT = 0x1406, GL_RGBA = 0x1908, GL_BACK = 0x0405,
	GL_VENDOR = 0x1F00, GL_RENDERER = 0x1F01, GL_VERSION = 0x1F02,
	GL_NEAREST = 0x2600, GL_LINEAR = 0x2601, GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803,
	GL_CLIP_DISTANCE0 = 0x3000,
	GL_BLEND_EQUATION_RGB = 0x8009, GL_POLYGON_OFFSET_FILL = 0x8037, GL_RGBA8 = 0x8058, GL_TEXTURE_BINDING_2D = 0x8069,
	GL_UNPACK_SKIP_IMAGES = 0x806D, GL_UNPACK_IMAGE_HEIGHT = 0x806E,
	GL_SAMPLE_ALPHA_TO_COVERAGE = 0x809E, GL_BLEND_DST_RGB = 0x80C8, GL_BLEND_SRC_RGB = 0x80C9, GL_BLEND_DST_ALPHA = 0x80CA, GL_BLEND_SRC_ALPHA = 0x80CB,
	GL_CLAMP_TO_EDGE = 0x812F, GL_TEXTURE0 = 0x84C0, GL_ACTIVE_TEXTURE = 0x84E0, GL_VERTEX_ARRAY_BINDING = 0x85B5,
	GL_BLEND_EQUATION_ALPHA = 0x883D, GL_ARRAY_BUFFER_BINDING = 0x8894, GL_PIXEL_PACK_BUFFER = 0x88EB, GL_PIXEL_UNPACK_BUFFER = 0x88EC,
	GL_PIXEL_PACK_BUFFER_BINDING = 0x88ED, GL_PIXEL_UNPACK_BUFFER_BINDING = 0x88EF, GL_STREAM_DRAW = 0x88E0,
	GL_SAMPLER_BINDING = 0x8919, GL_FRAGMENT_SHADER = 0x8B30, GL_VERTEX_SHADER = 0x8B31, GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82,
	GL_SHADING_LANGUAGE_VERSION = 0x8B8C, GL_CURRENT_PROGRAM = 0x8B8D,
	GL_RASTERIZER_DISCARD = 0x8C89, GL_DRAW_FRAMEBUFFER_BINDING = 0x8CA6, GL_READ_FRAMEBUFFER = 0x8CA8, GL_DRAW_FRAMEBUFFER = 0x8CA9,
	GL_READ_FRAMEBUFFER_BINDING = 0x8CAA, GL_FRAMEBUFFER_SRGB = 0x8DB9, GL_CONTEXT_PROFILE_MASK = 0x9126,
	GL_FUNC_ADD = 0x8006,
};
enum : GLbitfield { GL_MAP_WRITE_BIT = 0x2, GL_MAP_INVALIDATE_BUFFER_BIT = 0x8 };

#define GLFUNCS(X)                                                                                   \
	X(void, GetIntegerv, GLenum, GLint *)                                                            \
	X(GLboolean, IsEnabled, GLenum)                                                                  \
	X(void, Enable, GLenum)                                                                          \
	X(void, Disable, GLenum)                                                                         \
	X(GLenum, GetError, void)                                                                        \
	X(const unsigned char *, GetString, GLenum)                                                      \
	X(void, Viewport, GLint, GLint, GLsizei, GLsizei)                                                \
	X(void, Scissor, GLint, GLint, GLsizei, GLsizei)                                                 \
	X(void, ColorMask, GLboolean, GLboolean, GLboolean, GLboolean)                                   \
	X(void, DepthMask, GLboolean)                                                                    \
	X(void, BlendFuncSeparate, GLenum, GLenum, GLenum, GLenum)                                       \
	X(void, BlendEquationSeparate, GLenum, GLenum)                                                   \
	X(void, BindFramebuffer, GLenum, GLuint)                                                         \
	X(void, ReadBuffer, GLenum)                                                                      \
	X(void, UseProgram, GLuint)                                                                      \
	X(void, ActiveTexture, GLenum)                                                                   \
	X(void, BindTexture, GLenum, GLuint)                                                             \
	X(void, BindSampler, GLuint, GLuint)                                                             \
	X(void, BindBuffer, GLenum, GLuint)                                                              \
	X(void, BindVertexArray, GLuint)                                                                 \
	X(void, GenVertexArrays, GLsizei, GLuint *)                                                      \
	X(void, GenTextures, GLsizei, GLuint *)                                                          \
	X(void, TexImage2D, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *) \
	X(void, TexSubImage2D, GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *) \
	X(void, TexParameteri, GLenum, GLenum, GLint)                                                    \
	X(void, PixelStorei, GLenum, GLint)                                                              \
	X(void, GenBuffers, GLsizei, GLuint *)                                                           \
	X(void, DeleteBuffers, GLsizei, const GLuint *)                                                  \
	X(void, BufferData, GLenum, GLsizeiptr, const void *, GLenum)                                    \
	X(void *, MapBufferRange, GLenum, GLintptr, GLsizeiptr, GLbitfield)                              \
	X(GLboolean, UnmapBuffer, GLenum)                                                                \
	X(GLuint, CreateShader, GLenum)                                                                  \
	X(void, ShaderSource, GLuint, GLsizei, const GLchar *const *, const GLint *)                    \
	X(void, CompileShader, GLuint)                                                                   \
	X(void, GetShaderiv, GLuint, GLenum, GLint *)                                                    \
	X(void, GetShaderInfoLog, GLuint, GLsizei, GLsizei *, GLchar *)                                  \
	X(GLuint, CreateProgram, void)                                                                   \
	X(void, AttachShader, GLuint, GLuint)                                                            \
	X(void, LinkProgram, GLuint)                                                                     \
	X(void, GetProgramiv, GLuint, GLenum, GLint *)                                                   \
	X(void, GetProgramInfoLog, GLuint, GLsizei, GLsizei *, GLchar *)                                 \
	X(GLint, GetUniformLocation, GLuint, const GLchar *)                                             \
	X(void, Uniform1i, GLint, GLint)                                                                 \
	X(void, Uniform1f, GLint, GLfloat)                                                               \
	X(void, Uniform2f, GLint, GLfloat, GLfloat)                                                      \
	X(void, Uniform4f, GLint, GLfloat, GLfloat, GLfloat, GLfloat)                                    \
	X(void, DrawArrays, GLenum, GLint, GLsizei)                                                      \
	X(void, ReadPixels, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)                      \
	X(void, GetFloatv, GLenum, GLfloat *)                                                            \
	X(void, Clear, GLbitfield)                                                                       \
	X(void, ClearColor, GLfloat, GLfloat, GLfloat, GLfloat)                                          \
	X(void, GetBooleanv, GLenum, GLboolean *)

#define GLDECL(ret, name, ...) static ret (*gl##name)(__VA_ARGS__) = nullptr;
GLFUNCS(GLDECL)
#undef GLDECL

// ---- SDL --------------------------------------------------------------------------------------------
using _SDL_GL_SwapWindow = void (*)(void *window);
using _SDL_GL_GetProcAddress = void *(*)(const char *proc);
using _SDL_GL_GetDrawableSize = void (*)(void *window, int *w, int *h);
static _SDL_GL_SwapWindow SDL_GL_SwapWindow_ = nullptr;
static _SDL_GL_GetProcAddress SDL_GL_GetProcAddress_ = nullptr;
static _SDL_GL_GetDrawableSize SDL_GL_GetDrawableSize_ = nullptr;

static void SwapWindow_Hook(void *window);
static Hook g_swapHook(&SwapWindow_Hook);

static double NowMs() { return plat::MonotonicNs() * 1e-6; }

// ---- GL resources (render thread) --------------------------------------------------------------------
static bool g_glTried = false, g_glOk = false;
static GLuint g_prog = 0, g_vao = 0, g_tex = 0, g_pbo[2] = {0, 0};
static int g_pboIdx = 0;
static int g_texW = 0, g_texH = 0;
static GLint g_texFilt = 0;  // the texture's current filter (Draw)
static GLint u_fadeA = -1, u_fadeCol = -1;
static GLint u_tex = -1, u_viewport = -1, u_cursor = -1, u_cursorOn = -1, u_invert = -1, u_mode = -1, u_flipY = -1, u_fbFlip = -1;

static const char *kVs130 =
    "#version 130\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "  uv = p;\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
// SkyCraft's overlay shader (skse/src/Overlay.cpp), in GLSL: premultiplied alpha straight from Minecraft;
// Minecraft's crosshair and attack indicator drawn with its invert blend in a pass of their own (mode 1);
// the screen cursor. Pixel coordinates have a top-left origin, as in SkyCraft.
static const char *kFs130 =
    "#version 130\n"
    "uniform sampler2D tex;\n"
    "uniform vec2 viewport;\n"
    "uniform vec2 cursor;\n"
    "uniform float cursorOn;\n"
    "uniform vec4 invertRect;\n"
    "uniform float mode;\n"
    "uniform float flipY;\n"
    "uniform float fbFlip;\n"
    "uniform float fadeA;\n"
    "uniform vec4 fadeCol;\n"
    "in vec2 uv;\n"
    "out vec4 outColor;\n"
    "bool inInvert(vec2 p) { return all(greaterThanEqual(p, invertRect.xy)) && all(lessThan(p, invertRect.zw)); }\n"
    "vec4 overlay(vec2 t) { if (flipY > 0.5) t.y = 1.0 - t.y; return texture(tex, t); }\n"
    "void main() {\n"
    "  vec2 p = vec2(gl_FragCoord.x, fbFlip > 0.5 ? gl_FragCoord.y : viewport.y - gl_FragCoord.y);\n"
    "  vec2 uvf = vec2(uv.x, fbFlip > 0.5 ? 1.0 - uv.y : uv.y);\n"
    "  if (mode > 0.5) { if (!inInvert(p)) discard; outColor = vec4(overlay(uvf).rgb, 0.0); return; }\n"
    "  if (inInvert(p)) { outColor = vec4(0.0); return; }\n"
    "  vec4 c = overlay(uvf);\n"
    "  if (cursorOn > 0.5) {\n"
    "    vec2 q = p - cursor;\n"
    "    if (q.x >= 0.0 && q.y >= 0.0 && q.y < 18.0 && q.x <= q.y * 0.6) {\n"
    "      bool edge = q.x < 1.5 || q.x > q.y * 0.6 - 1.5 || q.y > 16.5;\n"
    "      c = vec4(edge ? vec3(0.0) : vec3(1.0), 1.0);\n"
    "    }\n"
    "  }\n"
    "  c.rgb = c.rgb * (1.0 - fadeA) + fadeCol.rgb * (fadeA * c.a);\n"  // the screen fade over the hand/HUD (premultiplied: the pixel would have been faded after being drawn)
    "  outColor = c;\n"
    "}\n";

static GLuint Compile(GLenum type, const char *src) {
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, nullptr);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024] = "";
		glGetShaderInfoLog(s, sizeof log, nullptr, log);
		console->Print("[bridge] glhud: shader compile failed: %s\n", log);
		return 0;
	}
	return s;
}

static bool InitGl() {
	if (g_glTried) return g_glOk;
	g_glTried = true;
	bool all = true;
#define GLLOAD(ret, name, ...)                                                       \
	gl##name = (ret(*)(__VA_ARGS__))SDL_GL_GetProcAddress_("gl" #name);              \
	if (!gl##name) {                                                                 \
		console->Print("[bridge] glhud: gl" #name " missing\n");                     \
		all = false;                                                                 \
	}
	GLFUNCS(GLLOAD)
#undef GLLOAD
	if (!all) return false;
	GLint profile = 0;
	glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
	glGetError();
	console->Print("[bridge] glhud: GL %s, GLSL %s, %s (profile mask 0x%x)\n", (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION),
	               (const char *)glGetString(GL_RENDERER), profile);
	// The program, the VAO and the texture are ours alone; nothing of togl's is bound while creating them
	// except what the caller saved (the draw path saves and restores around this too).
	GLuint vs = Compile(GL_VERTEX_SHADER, kVs130), fs = Compile(GL_FRAGMENT_SHADER, kFs130);
	if (!vs || !fs) return false;
	g_prog = glCreateProgram();
	glAttachShader(g_prog, vs);
	glAttachShader(g_prog, fs);
	glLinkProgram(g_prog);
	GLint ok = 0;
	glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[1024] = "";
		glGetProgramInfoLog(g_prog, sizeof log, nullptr, log);
		console->Print("[bridge] glhud: link failed: %s\n", log);
		return false;
	}
	u_fadeA = glGetUniformLocation(g_prog, "fadeA");
	u_fadeCol = glGetUniformLocation(g_prog, "fadeCol");
	u_tex = glGetUniformLocation(g_prog, "tex");
	u_viewport = glGetUniformLocation(g_prog, "viewport");
	u_cursor = glGetUniformLocation(g_prog, "cursor");
	u_cursorOn = glGetUniformLocation(g_prog, "cursorOn");
	u_invert = glGetUniformLocation(g_prog, "invertRect");
	u_mode = glGetUniformLocation(g_prog, "mode");
	u_flipY = glGetUniformLocation(g_prog, "flipY");
	u_fbFlip = glGetUniformLocation(g_prog, "fbFlip");
	glGenVertexArrays(1, &g_vao);
	glGenTextures(1, &g_tex);
	glGenBuffers(2, g_pbo);
	g_glOk = true;
	console->Print("[bridge] glhud: ready (program %u, vao %u, texture %u, pbo %u %u)\n", g_prog, g_vao, g_tex, g_pbo[0], g_pbo[1]);
	return true;
}

// ---- togl's state, saved and restored ---------------------------------------------------------------
struct GlState {
	GLint drawFb, readFb, readBuf, program, activeTex, tex2d, sampler, vao, arrayBuf, unpackBuf, packBuf;
	GLint viewport[4], scissor[4];
	GLint blendSrcRgb, blendDstRgb, blendSrcA, blendDstA, blendEqRgb, blendEqA;
	GLboolean colorMask[4], depthMask;
	GLint unpack[8], pack[4];
	GLboolean en[16];
};
static const GLenum kCaps[] = {GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_FRAMEBUFFER_SRGB, GL_ALPHA_TEST, GL_POLYGON_OFFSET_FILL,
                               GL_SAMPLE_ALPHA_TO_COVERAGE, GL_RASTERIZER_DISCARD, GL_COLOR_LOGIC_OP, GL_CLIP_DISTANCE0, GL_CLIP_DISTANCE0 + 1, GL_CLIP_DISTANCE0 + 2,
                               GL_CLIP_DISTANCE0 + 3};
static const int kNCaps = sizeof kCaps / sizeof kCaps[0];
static const GLenum kUnpack[] = {GL_UNPACK_SWAP_BYTES, GL_UNPACK_LSB_FIRST, GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_PIXELS, GL_UNPACK_ALIGNMENT,
                                 GL_UNPACK_IMAGE_HEIGHT, GL_UNPACK_SKIP_IMAGES};
static const GLenum kPack[] = {GL_PACK_ROW_LENGTH, GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS, GL_PACK_ALIGNMENT};

static void SaveState(GlState &s) {
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s.drawFb);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s.readFb);
	glGetIntegerv(GL_READ_BUFFER, &s.readBuf);
	glGetIntegerv(GL_CURRENT_PROGRAM, &s.program);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &s.activeTex);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex2d);
	glGetIntegerv(GL_SAMPLER_BINDING, &s.sampler);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.arrayBuf);
	glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &s.unpackBuf);
	glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &s.packBuf);
	glGetIntegerv(GL_VIEWPORT, s.viewport);
	glGetIntegerv(GL_SCISSOR_BOX, s.scissor);
	glGetIntegerv(GL_BLEND_SRC_RGB, &s.blendSrcRgb);
	glGetIntegerv(GL_BLEND_DST_RGB, &s.blendDstRgb);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.blendSrcA);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &s.blendDstA);
	glGetIntegerv(GL_BLEND_EQUATION_RGB, &s.blendEqRgb);
	glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &s.blendEqA);
	glGetBooleanv(GL_COLOR_WRITEMASK, s.colorMask);
	glGetBooleanv(GL_DEPTH_WRITEMASK, &s.depthMask);
	for (int i = 0; i < 8; ++i) glGetIntegerv(kUnpack[i], &s.unpack[i]);
	for (int i = 0; i < 4; ++i) glGetIntegerv(kPack[i], &s.pack[i]);
	for (int i = 0; i < kNCaps; ++i) s.en[i] = glIsEnabled(kCaps[i]);
	// (glActiveTexture(GL_TEXTURE0) above is undone by RestoreState)
}

static void RestoreState(const GlState &s) {
	for (int i = 0; i < kNCaps; ++i) (s.en[i] ? glEnable : glDisable)(kCaps[i]);
	for (int i = 0; i < 8; ++i) glPixelStorei(kUnpack[i], s.unpack[i]);
	for (int i = 0; i < 4; ++i) glPixelStorei(kPack[i], s.pack[i]);
	glDepthMask(s.depthMask);
	glColorMask(s.colorMask[0], s.colorMask[1], s.colorMask[2], s.colorMask[3]);
	glBlendEquationSeparate(s.blendEqRgb, s.blendEqA);
	glBlendFuncSeparate(s.blendSrcRgb, s.blendDstRgb, s.blendSrcA, s.blendDstA);
	glScissor(s.scissor[0], s.scissor[1], s.scissor[2], s.scissor[3]);
	glViewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, s.packBuf);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, s.unpackBuf);
	glBindBuffer(0x8892 /* GL_ARRAY_BUFFER */, s.arrayBuf);
	glBindVertexArray(s.vao);
	glActiveTexture(GL_TEXTURE0);
	glBindSampler(0, s.sampler);
	glBindTexture(GL_TEXTURE_2D, s.tex2d);
	glActiveTexture(s.activeTex);
	glUseProgram(s.program);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, s.readFb);
	glReadBuffer(s.readBuf);  // per framebuffer: set while that framebuffer is bound for reading
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.drawFb);
}

// Re-read and compare (the check behind pcb_assert_glhud). Only the cheap, togl-relevant subset of fields.
static void VerifyState(const GlState &want, rb::HudStats &st) {
	GlState now{};
	SaveState(now);
	glActiveTexture(want.activeTex);  // SaveState switched to unit 0
	const char *bad = nullptr;
	if (now.drawFb != want.drawFb) bad = "draw framebuffer";
	else if (now.readFb != want.readFb) bad = "read framebuffer";
	else if (now.program != want.program) bad = "program";
	else if (now.activeTex != want.activeTex) bad = "active texture";
	else if (now.tex2d != want.tex2d) bad = "texture 2D (unit 0)";
	else if (now.sampler != want.sampler) bad = "sampler (unit 0)";
	else if (now.vao != want.vao) bad = "vertex array";
	else if (now.arrayBuf != want.arrayBuf) bad = "array buffer";
	else if (now.unpackBuf != want.unpackBuf) bad = "unpack buffer";
	else if (std::memcmp(now.viewport, want.viewport, sizeof now.viewport)) bad = "viewport";
	else if (std::memcmp(now.scissor, want.scissor, sizeof now.scissor)) bad = "scissor";
	else if (now.blendSrcRgb != want.blendSrcRgb || now.blendDstRgb != want.blendDstRgb || now.blendSrcA != want.blendSrcA || now.blendDstA != want.blendDstA) bad = "blend func";
	else if (now.blendEqRgb != want.blendEqRgb || now.blendEqA != want.blendEqA) bad = "blend equation";
	else if (std::memcmp(now.colorMask, want.colorMask, sizeof now.colorMask) || now.depthMask != want.depthMask) bad = "write masks";
	else if (std::memcmp(now.unpack, want.unpack, sizeof now.unpack)) bad = "unpack state";
	else if (std::memcmp(now.en, want.en, sizeof(GLboolean) * kNCaps)) bad = "enables";
	if (bad) {
		++st.stateMismatch;
		std::snprintf(st.mismatch, sizeof st.mismatch, "%s", bad);
	}
}


// ---- the image -----------------------------------------------------------------------------------------
static void Upload(const uint8_t *pixels, int w, int h, rb::HudStats &st) {
	const double t0 = NowMs();
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, g_tex);
	glBindSampler(0, 0);
	for (int i = 0; i < 8; ++i) glPixelStorei(kUnpack[i], kUnpack[i] == GL_UNPACK_ALIGNMENT ? 4 : 0);
	if (w != g_texW || h != g_texH) {
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		g_texW = w, g_texH = h, g_texFilt = GL_LINEAR;
		st.texW = w, st.texH = h;
	}
	const GLsizeiptr bytes = (GLsizeiptr)w * h * 4;
	g_pboIdx ^= 1;
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, g_pbo[g_pboIdx]);
	glBufferData(GL_PIXEL_UNPACK_BUFFER, bytes, nullptr, GL_STREAM_DRAW);  // orphan: no wait on the last upload
	void *dst = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, bytes, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
	if (dst) {
		std::memcpy(dst, pixels, (size_t)bytes);
		glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	}
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	const double ms = NowMs() - t0;
	++st.uploads;
	st.upMs += ms;
	if (ms > st.upMax) st.upMax = ms;
}

static void Draw(const rb::HudDraw &d) {
	const int W = d.W, H = d.H;
	const bool keepFramebuffer = d.intoEnginesFrame;
	// at the swap: the window's framebuffer (togl has blitted its frame there); in the paint pass (bridge #18): togl's own
	// frame, whatever is bound, so Portal 2's 2D UI (captions, console, menus) is drawn over it afterwards
	if (!keepFramebuffer) glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	for (int i = 0; i < kNCaps; ++i)
		if (kCaps[i] != GL_BLEND) glDisable(kCaps[i]);
	glEnable(GL_BLEND);
	glColorMask(1, 1, 1, 1);
	glViewport(0, 0, W, H);
	glUseProgram(g_prog);
	glBindVertexArray(g_vao);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, g_tex);
	glBindSampler(0, 0);
	{
		// (bridge #15) HUD option A: Minecraft's window is the screen, so the GUI is 1:1: sample texel for texel (nearest);
		// linear only when a size differs (a frame or two around a resize)
		const GLint want = (d.frameW == W && d.frameH == H) ? GL_NEAREST : GL_LINEAR;
		if (want != g_texFilt) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, want), glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, want), g_texFilt = want;
	}
	glUniform1i(u_tex, 0);
	glUniform2f(u_viewport, (float)W, (float)H);
	glUniform1f(u_flipY, d.flipY ? 1.0f : 0.0f);
	glUniform1f(u_fadeA, d.testPattern ? 0.0f : d.fadeA);
	glUniform4f(u_fadeCol, d.fadeR, d.fadeG, d.fadeB, 1.0f);
	// (bridge #21) togl's own frame (the paint pass) is stored upside down relative to the window's framebuffer (it flips at
	// the present): the image and the cursor/invert rectangles are flipped there, the swap path is unchanged
	glUniform1f(u_fbFlip, keepFramebuffer ? 1.0f : 0.0f);
	const float sx = d.frameW > 0 ? (float)W / d.frameW : 1.0f, sy = d.frameH > 0 ? (float)H / d.frameH : 1.0f;
	const bool cursor = d.cursorOn && !d.testPattern;
	glUniform2f(u_cursor, d.cursorX * sx, d.cursorY * sy);
	glUniform1f(u_cursorOn, cursor ? 1.0f : 0.0f);
	// Around the screen centre, where Minecraft puts its crosshair (15 GUI pixels) and the attack indicator
	// under it (16 x 16 from 9 GUI pixels below the centre): SkyCraft's rectangle.
	const bool invert = !d.testPattern && d.guiScale > 0 && d.camera == 0 && !(d.mcFlags & kMcScreenOpen);
	const float g = d.guiScale * sx, cx = W * 0.5f, cy = H * 0.5f;
	if (invert) glUniform4f(u_invert, cx - 12 * g, cy - 12 * g, cx + 12 * g, cy + 28 * g);
	else glUniform4f(u_invert, 0, 0, 0, 0);
	glUniform1f(u_mode, 0.0f);
	glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
	glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	if (invert) {
		// Minecraft's BlendFunction.INVERT; the back buffer's alpha is left alone
		glUniform1f(u_mode, 1.0f);
		glBlendFuncSeparate(GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_ZERO, GL_ONE);
		glDrawArrays(GL_TRIANGLES, 0, 3);
	}
}

// ---- in-game video recorder (lead, 2026-10-02: "get a video of the Minecraft player walking...") -----------------
// pcb_record start <name> [fps] / stop: the presented frame (Portal 2 + the Minecraft overlay) is read back through two
// pixel-pack buffers (asynchronous: a frame's pixels are taken one frame later), handed to a writer thread and piped
// to ffmpeg (NVENC H.264) at a constant rate: a frame is repeated when Portal 2 is slower than the video rate and
// skipped when faster. Output: cache/bridge/videos/<name>.mp4.
static std::atomic<int> g_recOn{0};
static std::atomic<int> g_recStopReq{0};
static int g_recFps = 60, g_recW = 0, g_recH = 0, g_recPboIdx = 0, g_recPending = 0;
static GLuint g_recPbo[2] = {0, 0};
static double g_recT0 = 0, g_recNext = 0;
static uint64_t g_recFrames = 0, g_recDups = 0, g_recDrops = 0;
static FILE *g_recPipe = nullptr;
static std::string g_recPath;
static std::mutex g_recMx;
static std::condition_variable g_recCv;
static std::deque<std::pair<std::vector<uint8_t>, int>> g_recQ;  // frame, how many times to write it
static std::thread g_recThread;
static bool g_recThreadStop = false;
static double RecNow() { return plat::MonotonicNs() * 1e-9; }
static void RecWriter() {
	for (;;) {
		std::pair<std::vector<uint8_t>, int> job;
		{
			std::unique_lock<std::mutex> lk(g_recMx);
			g_recCv.wait(lk, [] { return g_recThreadStop || !g_recQ.empty(); });
			if (g_recQ.empty() && g_recThreadStop) return;
			job = std::move(g_recQ.front());
			g_recQ.pop_front();
		}
		for (int i = 0; i < job.second && g_recPipe; ++i)
			if (std::fwrite(job.first.data(), 1, job.first.size(), g_recPipe) != job.first.size()) break;  // (the encoder went away)
	}
}
static void RecStopRender() {  // render thread
	if (g_recPbo[0]) glDeleteBuffers(2, g_recPbo), g_recPbo[0] = g_recPbo[1] = 0;
	{
		std::lock_guard<std::mutex> lk(g_recMx);
		g_recThreadStop = true;
	}
	g_recCv.notify_all();
	if (g_recThread.joinable()) g_recThread.join();
	if (g_recPipe) std::fclose(g_recPipe), g_recPipe = nullptr;
	console->Print("[bridge] record: stopped, %llu video frames (%llu repeated, %llu dropped) -> %s\n", (unsigned long long)g_recFrames, (unsigned long long)g_recDups,
	               (unsigned long long)g_recDrops, g_recPath.c_str());
	g_recOn = 0, g_recStopReq = 0;
}
static void RecFrame(int W, int H) {  // render thread, after the overlay, before the swap
	if (!g_recOn.load()) return;
	if (g_recStopReq.load()) return RecStopRender();
	const size_t bytes = (size_t)W * H * 4;
	if (!g_recPipe) {
		g_recW = W, g_recH = H;
		// (ffmpeg isn't inside Steam's runtime container: an encoder outside reads the frames from a named pipe,
		// tools/record_video.sh; opening the pipe waits for it)
		signal(SIGPIPE, SIG_IGN);
		g_recPipe = std::fopen(g_recPath.c_str(), "wb");
		if (!g_recPipe) {
			console->Print("[bridge] record: can't open %s (run tools/record_video.sh first)\n", g_recPath.c_str());
			g_recOn = 0;
			return;
		}
		glGenBuffers(2, g_recPbo);
		for (int i = 0; i < 2; ++i) glBindBuffer(GL_PIXEL_PACK_BUFFER, g_recPbo[i]), glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, 0x88E1 /* GL_STREAM_READ */);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		g_recThreadStop = false;
		g_recThread = std::thread(RecWriter);
		g_recT0 = RecNow(), g_recNext = 0, g_recPending = 0, g_recFrames = g_recDups = g_recDrops = 0;
		console->Print("[bridge] record: %dx%d at %d fps -> %s\n", W, H, g_recFps, g_recPath.c_str());
	}
	if (W != g_recW || H != g_recH) return;  // (the window was resized: those frames are skipped)
	const double t = RecNow() - g_recT0;
	if (t < g_recNext) return;  // faster than the video rate: this frame isn't needed
	int copies = 1 + (int)((t - g_recNext) * g_recFps);  // slower: the frame stands for the video frames missed
	if (copies > g_recFps) copies = g_recFps;
	g_recNext += copies / (double)g_recFps;
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glReadBuffer(GL_BACK);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	glPixelStorei(GL_PACK_SKIP_ROWS, 0);
	glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
	const int cur = g_recPboIdx, prev = cur ^ 1;
	glBindBuffer(GL_PIXEL_PACK_BUFFER, g_recPbo[cur]);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);  // async into this PBO
	if (g_recPending) {  // the previous frame's pixels have arrived by now
		glBindBuffer(GL_PIXEL_PACK_BUFFER, g_recPbo[prev]);
		if (const void *src = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes, 0x0001 /* GL_MAP_READ_BIT */)) {
			std::vector<uint8_t> f((const uint8_t *)src, (const uint8_t *)src + bytes);
			glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
			std::lock_guard<std::mutex> lk(g_recMx);
			if (g_recQ.size() < 24) {
				g_recQ.emplace_back(std::move(f), g_recPending);
				g_recFrames += g_recPending, g_recDups += g_recPending - 1;
			} else {
				++g_recDrops;
			}
		}
		g_recCv.notify_one();
	}
	g_recPending = copies;
	g_recPboIdx = prev;
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

// (bridge #24) the bug-mark toast (bugmark.cpp): drawn after the readbacks and the recorder's, so no screenshot or video has

// ---- the rb::HudBackend interface ------------------------------------------------------------------------------------------------
namespace rb {

bool HudBackendInstall() {
	void *h = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_NOLOAD);
	if (!h) h = RTLD_DEFAULT;
	SDL_GL_SwapWindow_ = (_SDL_GL_SwapWindow)dlsym(h, "SDL_GL_SwapWindow");
	SDL_GL_GetProcAddress_ = (_SDL_GL_GetProcAddress)dlsym(h, "SDL_GL_GetProcAddress");
	SDL_GL_GetDrawableSize_ = (_SDL_GL_GetDrawableSize)dlsym(h, "SDL_GL_GetDrawableSize");
	console->Print("[bridge] glhud: SDL_GL_SwapWindow %p, GetProcAddress %p, GetDrawableSize %p\n", (void *)SDL_GL_SwapWindow_, (void *)SDL_GL_GetProcAddress_,
	               (void *)SDL_GL_GetDrawableSize_);
	if (!SDL_GL_SwapWindow_ || !SDL_GL_GetProcAddress_) return false;
	g_swapHook.SetFunc(SDL_GL_SwapWindow_);
	return true;
}
void HudBackendUninstall() { g_swapHook.Disable(true); }

bool HudBackendReady() { return SDL_GL_GetProcAddress_ && InitGl(); }
bool HudBackendWindowSize(void *window, int *w, int *h) {
	if (!SDL_GL_GetDrawableSize_) return false;
	SDL_GL_GetDrawableSize_(window, w, h);
	return true;
}

static GlState g_saved;
void HudBackendBegin(HudStats &st) {
	while (glGetError() != GL_NO_ERROR) ++st.preErrors;  // togl's own, so ours are counted alone
	g_saved = GlState{};
	SaveState(g_saved);
}
void HudBackendEnd(HudStats &st, bool verify) {
	while (glGetError() != GL_NO_ERROR) ++st.glErrors;
	RestoreState(g_saved);
	if (verify) VerifyState(g_saved, st);
}
void HudBackendUpload(const uint8_t *rgba, int w, int h, HudStats &st) { Upload(rgba, w, h, st); }
void HudBackendDraw(const HudDraw &d) { Draw(d); }

static void ReadSetup() {
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glReadBuffer(GL_BACK);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	glPixelStorei(GL_PACK_SKIP_ROWS, 0);
	glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
}
bool HudBackendReadPixel(int, int H, int x, int y, uint8_t rgba[4]) {
	ReadSetup();
	glReadPixels(x, H - 1 - y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	return true;
}
bool HudBackendReadFrame(int W, int H, std::vector<uint8_t> &raw, bool &bottomUp) {
	ReadSetup();
	raw.resize((size_t)W * H * 4);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
	bottomUp = true;
	return true;
}
bool HudBackendWantsFrame() { return g_recOn.load() != 0; }
void HudBackendAfterOverlay(int W, int H) { RecFrame(W, H); }

// (bridge #24) the bug-mark toast (bugmark.cpp): drawn after the readbacks and the recorder's, so no screenshot or video has
// it; plain scissored clears of the window's framebuffer
void HudBackendToast(int W, int H, const int (*r)[4], const uint32_t *c, int n) {
	GLfloat cc[4] = {0, 0, 0, 0};
	glGetFloatv(0x0C22 /* GL_COLOR_CLEAR_VALUE */, cc);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	for (int i = 0; i < kNCaps; ++i) glDisable(kCaps[i]);
	glEnable(GL_SCISSOR_TEST);
	glColorMask(1, 1, 1, 1);
	for (int i = 0; i < n; ++i) {
		if (r[i][2] <= 0 || r[i][3] <= 0) continue;
		glScissor(r[i][0], H - r[i][1] - r[i][3], r[i][2], r[i][3]);  // (top-left origin -> GL's bottom-left)
		glClearColor((c[i] & 255) / 255.0f, ((c[i] >> 8) & 255) / 255.0f, ((c[i] >> 16) & 255) / 255.0f, 1.0f);
		glClear(0x4000 /* GL_COLOR_BUFFER_BIT */);
	}
	glClearColor(cc[0], cc[1], cc[2], cc[3]);
}

}  // namespace rb

static void SwapWindow_Hook(void *window) {
	const bool run = rb::HudPresentBegin();
	if (run) rb::HudOnPresent(window);
	g_swapHook.Disable();
	SDL_GL_SwapWindow_(window);
	g_swapHook.Enable();
	rb::HudPresentEnd();
}

CON_COMMAND(pcb_record, "pcb_record start <name> [fps=60] | stop - stream the presented frames (Portal 2 + the Minecraft overlay) to cache/bridge/videos/<name>.fifo (tools/record_video.sh encodes it)\n") {
	if (args.ArgC() >= 3 && !std::strcmp(args[1], "start")) {
		if (g_recOn.load()) return console->Print("[bridge] record: already recording\n");
		std::string dir = CacheDir() + "/videos";
		plat::MakeDir(dir);
		g_recPath = dir + "/" + args[2] + ".fifo";
		g_recFps = args.ArgC() > 3 ? std::max(10, std::min(120, std::atoi(args[3]))) : 60;
		g_recStopReq = 0;
		g_recOn = 1;
		return;
	}
	if (args.ArgC() >= 2 && !std::strcmp(args[1], "stop")) {
		if (g_recOn.load()) g_recStopReq = 1;
		return;
	}
	console->Print("%s", pcb_record.ThisPtr()->m_pszHelpString);
}

}  // namespace bridge
