// Portalcraft bridge (bridge #73): the OpenGL subset that cutrender.cpp and digharness.cpp call on Portal 2's render thread.
//
// No GL headers are in the build (glhud.cpp keeps its own copy for the same reason). Everything here runs on the render thread,
// inside togl's GL context, between togl's own calls: togl shadows the GL state and assumes GL matches it, so whoever calls
// these saves and restores every piece of state it touches (bindings, pack state, matrix mode, active texture).
#pragma once

#include <cstdint>

namespace bridge {
namespace gl {

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef unsigned int GLhandleARB;  // (Linux: handles are unsigned ints)
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef unsigned char GLubyte;
typedef float GLfloat;
typedef char GLchar;
typedef unsigned int GLbitfield;

enum : GLenum {
	NO_ERROR = 0,
	DEPTH_BUFFER_BIT = 0x0100, COLOR_BUFFER_BIT = 0x4000,
	VIEWPORT = 0x0BA2, MATRIX_MODE = 0x0BA0, PACK_ROW_LENGTH = 0x0D02, PACK_SKIP_ROWS = 0x0D03, PACK_SKIP_PIXELS = 0x0D04, PACK_ALIGNMENT = 0x0D05,
	UNPACK_ROW_LENGTH = 0x0CF2, UNPACK_SKIP_ROWS = 0x0CF3, UNPACK_SKIP_PIXELS = 0x0CF4, UNPACK_ALIGNMENT = 0x0CF5, UNPACK_SKIP_IMAGES = 0x806D, UNPACK_IMAGE_HEIGHT = 0x806E,
	READ_BUFFER = 0x0C02, TEXTURE_2D = 0x0DE1, TEXTURE_3D = 0x806F, UNSIGNED_BYTE = 0x1401, FLOAT = 0x1406, UNSIGNED_SHORT = 0x1403,
	RGBA = 0x1908, RED = 0x1903, RG = 0x8227, R8 = 0x8229, RG8 = 0x822B, DEPTH_COMPONENT = 0x1902, RGBA8 = 0x8058, DEPTH24_STENCIL8 = 0x88F0, DEPTH_STENCIL = 0x84F9,
	VENDOR = 0x1F00, RENDERER = 0x1F01, VERSION = 0x1F02, SHADING_LANGUAGE_VERSION = 0x8B8C,
	NEAREST = 0x2600, TEXTURE_MAG_FILTER = 0x2800, TEXTURE_MIN_FILTER = 0x2801, TEXTURE_WRAP_S = 0x2802, TEXTURE_WRAP_T = 0x2803, TEXTURE_WRAP_R = 0x8072,
	CLAMP_TO_EDGE = 0x812F, TEXTURE_BASE_LEVEL = 0x813C, TEXTURE_MAX_LEVEL = 0x813D, TEXTURE0 = 0x84C0, ACTIVE_TEXTURE = 0x84E0,
	TEXTURE = 0x1702, MODELVIEW = 0x1700, PROJECTION = 0x1701,
	PIXEL_PACK_BUFFER = 0x88EB, PIXEL_UNPACK_BUFFER = 0x88EC, PIXEL_PACK_BUFFER_BINDING = 0x88ED, PIXEL_UNPACK_BUFFER_BINDING = 0x88EF,
	FRAGMENT_SHADER = 0x8B30, VERTEX_SHADER = 0x8B31, COMPILE_STATUS = 0x8B81, LINK_STATUS = 0x8B82, CURRENT_PROGRAM = 0x8B8D,
	OBJECT_TYPE_ARB = 0x8B4E, OBJECT_SUBTYPE_ARB = 0x8B4F, OBJECT_COMPILE_STATUS_ARB = 0x8B81, OBJECT_LINK_STATUS_ARB = 0x8B82, OBJECT_INFO_LOG_LENGTH_ARB = 0x8B84,
	OBJECT_ATTACHED_OBJECTS_ARB = 0x8B85, OBJECT_ACTIVE_UNIFORMS_ARB = 0x8B86, OBJECT_SHADER_SOURCE_LENGTH_ARB = 0x8B88, SHADER_OBJECT_ARB = 0x8B48, PROGRAM_OBJECT_ARB = 0x8B40,
	FRAMEBUFFER = 0x8D40, READ_FRAMEBUFFER = 0x8CA8, DRAW_FRAMEBUFFER = 0x8CA9, READ_FRAMEBUFFER_BINDING = 0x8CAA, DRAW_FRAMEBUFFER_BINDING = 0x8CA6,
	RENDERBUFFER = 0x8D41, RENDERBUFFER_BINDING = 0x8CA7, COLOR_ATTACHMENT0 = 0x8CE0, DEPTH_STENCIL_ATTACHMENT = 0x821A, FRAMEBUFFER_COMPLETE = 0x8CD5,
	TIMESTAMP = 0x8E28, QUERY_RESULT = 0x8866, QUERY_RESULT_AVAILABLE = 0x8867,
	SAMPLE_BUFFERS = 0x80A8, SAMPLES = 0x80A9, MAX_TEXTURE_IMAGE_UNITS = 0x8872, MAX_TEXTURE_COORDS = 0x8871, MAX_3D_TEXTURE_SIZE = 0x8073,
};

#define PCB_GLFUNCS(X)                                                                                                         \
	X(GLenum, GetError, void)                                                                                                   \
	X(const GLubyte *, GetString, GLenum)                                                                                       \
	X(void, GetIntegerv, GLenum, GLint *)                                                                                       \
	X(void, ReadPixels, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)                                                 \
	X(void, PixelStorei, GLenum, GLint)                                                                                         \
	X(void, ReadBuffer, GLenum)                                                                                                 \
	X(void, BindBuffer, GLenum, GLuint)                                                                                         \
	X(void, BindFramebuffer, GLenum, GLuint)                                                                                    \
	X(void, GenFramebuffers, GLsizei, GLuint *)                                                                                 \
	X(void, DeleteFramebuffers, GLsizei, const GLuint *)                                                                        \
	X(void, GenRenderbuffers, GLsizei, GLuint *)                                                                                \
	X(void, DeleteRenderbuffers, GLsizei, const GLuint *)                                                                       \
	X(void, BindRenderbuffer, GLenum, GLuint)                                                                                   \
	X(void, RenderbufferStorage, GLenum, GLenum, GLsizei, GLsizei)                                                              \
	X(void, FramebufferRenderbuffer, GLenum, GLenum, GLenum, GLuint)                                                            \
	X(GLenum, CheckFramebufferStatus, GLenum)                                                                                   \
	X(void, BlitFramebuffer, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)                        \
	X(void, GetAttachedObjectsARB, GLhandleARB, GLsizei, GLsizei *, GLhandleARB *)                                              \
	X(void, GetObjectParameterivARB, GLhandleARB, GLenum, GLint *)                                                              \
	X(void, GetShaderSourceARB, GLhandleARB, GLsizei, GLsizei *, GLchar *)                                                      \
	X(GLhandleARB, CreateShaderObjectARB, GLenum)                                                                               \
	X(void, ShaderSourceARB, GLhandleARB, GLsizei, const GLchar **, const GLint *)                                              \
	X(void, CompileShaderARB, GLhandleARB)                                                                                      \
	X(void, AttachObjectARB, GLhandleARB, GLhandleARB)                                                                          \
	X(void, DetachObjectARB, GLhandleARB, GLhandleARB)                                                                          \
	X(void, LinkProgramARB, GLhandleARB)                                                                                        \
	X(void, GetInfoLogARB, GLhandleARB, GLsizei, GLsizei *, GLchar *)                                                           \
	X(void, DeleteObjectARB, GLhandleARB)                                                                                       \
	X(GLint, GetUniformLocationARB, GLhandleARB, const GLchar *)                                                                \
	X(void, ProgramUniform1iEXT, GLuint, GLint, GLint)                                                                          \
	X(void, GenTextures, GLsizei, GLuint *)                                                                                     \
	X(void, TextureImage3DEXT, GLuint, GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)    \
	X(void, TextureSubImage3DEXT, GLuint, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void *) \
	X(void, TextureParameteriEXT, GLuint, GLenum, GLenum, GLint)                                                                \
	X(void, BindMultiTextureEXT, GLenum, GLenum, GLuint)                                                                        \
	X(void, MatrixLoadfEXT, GLenum, const GLfloat *)                                                                            \
	X(void, ActiveTexture, GLenum)                                                                                              \
	X(void, MatrixMode, GLenum)                                                                                                 \
	X(void, LoadMatrixf, const GLfloat *)                                                                                       \
	X(void, GetActiveUniformARB, GLhandleARB, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *)                          \
	X(void, GetUniformfvARB, GLhandleARB, GLint, GLfloat *)                                                                     \
	X(void, GetUniformivARB, GLhandleARB, GLint, GLint *)                                                                       \
	X(void, Uniform1fv, GLint, GLsizei, const GLfloat *)                                                                        \
	X(void, Uniform2fv, GLint, GLsizei, const GLfloat *)                                                                        \
	X(void, Uniform3fv, GLint, GLsizei, const GLfloat *)                                                                        \
	X(void, Uniform4fv, GLint, GLsizei, const GLfloat *)                                                                        \
	X(void, Uniform1iv, GLint, GLsizei, const GLint *)                                                                          \
	X(void, Uniform2iv, GLint, GLsizei, const GLint *)                                                                          \
	X(void, Uniform3iv, GLint, GLsizei, const GLint *)                                                                          \
	X(void, Uniform4iv, GLint, GLsizei, const GLint *)                                                                          \
	X(void, UniformMatrix2fv, GLint, GLsizei, GLboolean, const GLfloat *)                                                       \
	X(void, UniformMatrix3fv, GLint, GLsizei, GLboolean, const GLfloat *)                                                       \
	X(void, UniformMatrix4fv, GLint, GLsizei, GLboolean, const GLfloat *)                                                       \
	X(void, GenQueries, GLsizei, GLuint *)                                                                                      \
	X(void, QueryCounter, GLuint, GLenum)                                                                                       \
	X(void, GetQueryObjectiv, GLuint, GLenum, GLint *)                                                                          \
	X(void, GetQueryObjectui64v, GLuint, GLenum, uint64_t *)

#define PCB_GLDECL(ret, name, ...) extern ret (*name)(__VA_ARGS__);
PCB_GLFUNCS(PCB_GLDECL)
#undef PCB_GLDECL

// Resolve every entry through SDL_GL_GetProcAddress (needs a current context only for some drivers; NVIDIA's are global).
// Returns false and logs the missing names if any is missing. Safe to call more than once.
bool Load();
// SDL_GL_GetProcAddress itself (null if SDL2 is not loaded)
void *ProcAddress(const char *name);

}  // namespace gl
}  // namespace bridge
