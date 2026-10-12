// Portalcraft bridge: the cut-away world, Direct3D 9 backend (PLAN-windows.md 2.3, PLAN-vulkan.md V3).
//
// Portal 2's world is not drawn inside dug cells, at the fragment level, by Portal 2's own shaders, exactly as in the GL backend
// (render_backend/gl_cut.cpp), but by patching Direct3D 9 shader bytecode instead of togl's GLSL text:
//   * the vertex shader (vs_3_0): every instruction that writes the position output is repeated once more into a new output
//     (dcl_texcoord15), so the pixel shader receives the clip position the engine computed;
//   * the pixel shader (ps_3_0): the first instructions are a prologue that rebuilds the position in dug-cell space (three dp4 of the
//     interpolated clip position with rows that the CPU combined from the view's inverse and the cell map), takes the surface normal
//     from the screen-space derivatives (dsx/dsy/crs), steps eps along it both ways, looks both cells up in a brick index (volume
//     texture, 2 bytes per brick: slot + 1) and a brick atlas (volume texture, one byte per cell), and texkill's the fragment when
//     either cell is dug. Same maths as the GL shader text (cut_logic.cpp supplies the same data).
// The variants are created from the engine's own bytecode (IDirect3DVertexShader9::GetFunction) the first time a shader is bound
// inside the cut window; the window is the one render_backend.hpp describes (ViewOn .. ViewOff around Portal 2's world draws). A
// patched pixel shader is only ever bound with a patched vertex shader (the pair is kept consistent whatever order the engine
// binds in). Shaders that cannot be patched (vs_2_0 / ps_2_x, no free interpolant, sampler 14 or 15 in use ...) are counted by
// reason and drawn uncut. Nothing here comes from Portal 2's shader sources: the token format is the documented one.
#include "bridge/bridge.hpp"
#include "bridge/d3d9/d3d9hook.hpp"
#include "bridge/d3d9/sm_asm.hpp"
#include "bridge/platform/platform.hpp"
#include "bridge/render_backend.hpp"

#include "Modules/Tier1.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bridge {

void CutNoteError(const std::string &e);  // cut_logic.cpp

namespace {

using namespace d3d9;
using namespace d3d9::sm;

// ---- constants the patch uses (pixel shader float constants, samplers) -----------------------------------------------------------------
constexpr int kK = 216;            // c216..c223: the cut's constants (the engine writes below this; checked by the const listener)
constexpr int kSamIdx = 14, kSamAtl = 15;
constexpr int kTexcoordSem = 15;   // the extra interpolant: TEXCOORD15

enum Reason : int {
	R_OK = 0, R_NOT_SM3, R_NO_POSITION, R_TC15_USED, R_NO_OUTPUT_SLOT, R_NO_INPUT_SLOT, R_SAMPLERS_BUSY, R_CONSTS_BUSY, R_PARSE, R_CREATE_FAILED, R_NO_BYTECODE, R_REL_ADDR, R_SM2_UNSUPPORTED, R_TEMPS_BUSY, R_COUNT
};
const char *kReasonName[R_COUNT] = {"patched", "not shader model 3", "no position output", "TEXCOORD15 already used", "no free vertex output (o11 taken)", "no free pixel input (v9 taken)",
                                    "sampler 14/15 in use", "constants c216+ in use", "token stream not understood", "variant creation failed", "bytecode unavailable", "relative addressing on position", "shader model 2 construct not convertible", "temporaries r28+ in use (audit)"};

// ---- shader analysis ------------------------------------------------------------------------------------------------------------------------
struct Info {
	bool ok = false, pixel = false;
	int model = 0;
	size_t firstIns = 0, endTok = 0;   // token index of the first instruction after the declarations; of the end token
	uint32_t outMask = 0, inMask = 0, samplerMask = 0;
	int posReg = -1, maxConst = -1;
	bool tc15 = false;
};

bool Analyze(const std::vector<uint32_t> &code, Info &in) {
	in = Info();
	if (code.size() < 2) return false;
	const uint32_t hi = code[0] >> 16;
	if (hi != 0xFFFE && hi != 0xFFFF) return false;
	in.pixel = hi == 0xFFFF;
	in.model = (int)(code[0] & 0xFFFF);
	size_t i = 1;
	bool header = true;
	in.firstIns = 0;
	for (int guard = 0; guard < (1 << 20); ++guard) {
		if (i >= code.size()) return false;
		const uint32_t t = code[i];
		if (t == 0x0000FFFFu) {
			in.endTok = i;
			if (header) in.firstIns = i;
			return true;
		}
		const uint32_t op = t & 0xFFFF;
		if (op == OP_COMMENT) {
			i += 1 + ((t >> 16) & 0x7FFF);
			continue;
		}
		const size_t len = (t >> 24) & 0xF;
		if (i + 1 + len > code.size()) return false;
		if (op == OP_DCL) {
			if (len < 2) return false;
			const uint32_t u = code[i + 1], d = code[i + 2];
			const Reg ty = TypeOf(d);
			const uint32_t num = NumOf(d);
			if (ty == R_SAMPLER) in.samplerMask |= 1u << (num & 31);
			else {
				const uint32_t usage = u & 0x1F, idx = (u >> 16) & 0xF;
				if (ty == R_OUTPUT) {
					in.outMask |= 1u << (num & 31);
					if (usage == U_POSITION && idx == 0) in.posReg = (int)num;
					if (usage == U_TEXCOORD && idx == (uint32_t)kTexcoordSem) in.tc15 = true;
				} else if (ty == R_INPUT) {
					in.inMask |= 1u << (num & 31);
					if (usage == U_TEXCOORD && idx == (uint32_t)kTexcoordSem) in.tc15 = true;
				}
			}
		} else if (op == OP_DEF) {
			if (len >= 1 && TypeOf(code[i + 1]) == R_CONST) in.maxConst = std::max(in.maxConst, (int)NumOf(code[i + 1]));
		} else if (op == OP_DEFI || op == OP_DEFB) {
		} else {
			if (header) in.firstIns = i, header = false;
			for (size_t k = 1; k <= len; ++k) {
				const uint32_t p = code[i + k];
				if ((p & 0x80000000u) && TypeOf(p) == R_CONST) in.maxConst = std::max(in.maxConst, (int)NumOf(p));
			}
		}
		i += 1 + len;
	}
	return false;
}

int FirstFree(uint32_t mask, int limit) {  // lowest register number below limit not in mask, or -1
	for (int n = 0; n < limit; ++n)
		if (!(mask & (1u << n))) return n;
	return -1;
}

// vs_3_0: repeat every write of the position output into a new TEXCOORD15 output
Reason PatchVs(const std::vector<uint32_t> &code, const Info &in, std::vector<uint32_t> &out) {
	if (in.model != 0x0300) return R_NOT_SM3;
	if (in.posReg < 0) return R_NO_POSITION;
	if (in.tc15) return R_TC15_USED;
	// (Microsoft's d3d9 refuses a vs_3_0 with 12 declared outputs although o0..o11 exist; DXVK took them: at most 10 before ours)
	if (__builtin_popcount(in.outMask) > 10) return R_NO_OUTPUT_SLOT;
	const int n = FirstFree(in.outMask, 12);
	if (n < 0) return R_NO_OUTPUT_SLOT;
	out.clear();
	out.reserve(code.size() + 64);
	out.insert(out.end(), code.begin(), code.begin() + in.firstIns);
	out.push_back(OP_DCL | (2u << 24)), out.push_back(Usage_(U_TEXCOORD, kTexcoordSem)), out.push_back(Dst(R_OUTPUT, (uint32_t)n, kMaskXYZW));
	size_t i = in.firstIns;
	int writes = 0;
	while (i < code.size()) {
		const uint32_t t = code[i];
		if (t == 0x0000FFFFu) break;
		const uint32_t op = t & 0xFFFF;
		size_t len;
		if (op == OP_COMMENT) len = (t >> 16) & 0x7FFF;
		else len = (t >> 24) & 0xF;
		out.insert(out.end(), code.begin() + i, code.begin() + i + 1 + len);
		if (op != OP_COMMENT && len >= 1) {
			const uint32_t p = code[i + 1];
			if ((p & 0x80000000u) && TypeOf(p) == R_OUTPUT && NumOf(p) == (uint32_t)in.posReg) {
				if (p & (1u << 13)) return R_REL_ADDR;
				const size_t start = out.size();
				out.insert(out.end(), code.begin() + i, code.begin() + i + 1 + len);  // the same instruction again, into the new output
				out[start + 1] = (p & ~0x7FFu) | (uint32_t)n;
				++writes;
			}
		}
		i += 1 + len;
	}
	if (!writes) return R_NO_POSITION;
	out.push_back(0x0000FFFFu);
	return R_OK;
}

// ps_3_0 prologue. Inputs: v<m> the interpolated clip position (x, y, z, w); c216..c223 (see CutBackendViewOn).
//   r0 = (x, y, w, 1); r1.xyz = cell position (three dp4); r2 = normal * eps; r4 / r5 = the cell behind / in front (floor);
//   dug(r4) -> r2.x, dug(r5) -> r3.x; texkill when either is dug.
// Emit only writes pixel shader code (prologue, audit ending, kill-only shader). Microsoft's d3d9 refuses a ps_3_0 instruction that reads
// two different constant registers (c1 with c1 is fine, c1 with c2 is not; DXVK took both), so a second and third constant are first copied
// into the scratch temporaries r16 / r17 (above the prologue's r0..r15, below the audit's r28+) and read from there.
struct Emit {
	std::vector<uint32_t> &t;
	static constexpr uint32_t kScratch = 16;
	void ins(uint32_t op, std::initializer_list<uint32_t> p) {
		std::vector<uint32_t> v(p);
		const size_t first = (op == OP_TEXKILL) ? 0 : 1;  // (texkill has no destination)
		int consts[3];
		int nc = 0;
		for (size_t k = first; k < v.size(); ++k) {
			if (TypeOf(v[k]) != R_CONST) continue;
			const int n = (int)NumOf(v[k]);
			int slot = -1;
			for (int j = 0; j < nc; ++j)
				if (consts[j] == n) slot = j;
			if (slot < 0 && nc < 3) consts[slot = nc++] = n;
			if (slot > 0) v[k] = RegBits(R_TEMP, kScratch + (uint32_t)slot - 1) | (v[k] & 0x0FFF0000u);  // keeps swizzle and modifier
		}
		for (int j = 1; j < nc; ++j) {
			t.push_back(OP_MOV | (2u << 24));
			t.push_back(Dst(R_TEMP, kScratch + (uint32_t)j - 1));
			t.push_back(Src(R_CONST, (uint32_t)consts[j]));
		}
		t.push_back(op | ((uint32_t)v.size() << 24));
		for (uint32_t x : v) t.push_back(x);
	}
};
constexpr uint32_t kMaskXZW = 0xD;
uint32_t T(uint32_t n, uint32_t mask = kMaskXYZW) { return Dst(R_TEMP, n, mask); }
uint32_t S(uint32_t n, uint32_t swz = kXYZW, uint32_t mod = M_NONE) { return Src(R_TEMP, n, swz, mod); }
uint32_t C(uint32_t k, uint32_t swz = kXYZW, uint32_t mod = M_NONE) { return Src(R_CONST, (uint32_t)kK + k, swz, mod); }

void EmitDug(Emit &e, uint32_t cell, uint32_t outReg) {
	const uint32_t X = kXXXX, Y = kYYYY, Z = kZZZZ, W = kWWWW;
	e.ins(OP_ADD, {T(8, kMaskXYZ), S(cell), C(3, kXYZW, M_NEG)});            // rel = cell - org
	e.ins(OP_MUL, {T(9, kMaskXYZ), S(8), C(6, Y)});                          // rel / 8
	e.ins(OP_FRC, {T(10, kMaskXYZ), S(9)});
	e.ins(OP_ADD, {T(9, kMaskXYZ), S(9), S(10, kXYZW, M_NEG)});              // bi = floor(rel / 8)
	// (sge / slt are vertex shader instructions: Microsoft's d3d9 refuses a pixel shader that has them, DXVK took them. A test is
	// "difference, cmp to +1 / -1, then x 0.5 + 0.5"; c222.x = 1, c222.z = 0.5. Equality behaves as sge / slt did.)
	e.ins(OP_ADD, {T(10, kMaskXYZ), S(9), C(6, Z)});                         // bi + 0.5 >= 0  (bi >= -0.5)
	e.ins(OP_CMP, {T(10, kMaskXYZ), S(10), C(6, X), C(6, X, M_NEG)});
	e.ins(OP_MAD, {T(10, kMaskXYZ), S(10), C(6, Z), C(6, Z)});
	e.ins(OP_ADD, {T(11, kMaskXYZ), S(9), C(4, kXYZW, M_NEG)});              // bi - dims < 0  (bi < dims)
	e.ins(OP_CMP, {T(11, kMaskXYZ), S(11), C(6, X, M_NEG), C(6, X)});
	e.ins(OP_MAD, {T(11, kMaskXYZ), S(11), C(6, Z), C(6, Z)});
	e.ins(OP_MUL, {T(10, kMaskXYZ), S(10), S(11)});
	e.ins(OP_MUL, {T(10, kMaskX), S(10, X), S(10, Y)});
	e.ins(OP_MUL, {T(10, kMaskX), S(10, X), S(10, Z)});                      // r10.x = inside the index
	e.ins(OP_ADD, {T(11, kMaskXYZ), S(9), C(6, Z)});                         // bi + 0.5
	e.ins(OP_MUL, {T(11, kMaskXYZ), S(11), C(5)});                           // / dims
	e.ins(OP_TEXLD, {T(12), S(11), Src(R_SAMPLER, kSamIdx)});                // (L, L, L, A): low byte, high byte of slot + 1
	e.ins(OP_MAD, {T(13, kMaskXY), S(12, Swz(0, 3, 3, 3)), C(4, W), C(6, Z)});
	e.ins(OP_FRC, {T(14, kMaskXY), S(13)});
	e.ins(OP_ADD, {T(13, kMaskXY), S(13), S(14, kXYZW, M_NEG)});            // the two bytes, exact
	e.ins(OP_MAD, {T(13, kMaskX), S(13, Y), C(7, Y), S(13, X)});             // s = lo + 256 hi
	e.ins(OP_ADD, {T(14, kMaskX), S(13, X), C(6, Z, M_NEG)});                // a brick is there: s - 0.5 >= 0
	e.ins(OP_CMP, {T(14, kMaskX), S(14, X), C(6, X), C(6, X, M_NEG)});
	e.ins(OP_MAD, {T(14, kMaskX), S(14, X), C(6, Z), C(6, Z)});
	e.ins(OP_ADD, {T(13, kMaskX), S(13, X), C(6, X, M_NEG)});                // s - 1 = atlas slot
	e.ins(OP_MUL, {T(15, kMaskX), S(13, X), C(7, Z)});
	e.ins(OP_FRC, {T(15, kMaskY), S(15, X)});
	e.ins(OP_ADD, {T(15, kMaskX), S(15, X), S(15, Y, M_NEG)});               // f1 = floor(slot / 16)
	e.ins(OP_MUL, {T(15, kMaskY), S(15, X), C(7, Z)});
	e.ins(OP_FRC, {T(15, kMaskZ), S(15, Y)});
	e.ins(OP_ADD, {T(15, kMaskY), S(15, Y), S(15, Z, M_NEG)});               // f2 = floor(f1 / 16) = slot / 256
	e.ins(OP_MAD, {T(13, kMaskX), S(15, X), C(7, X, M_NEG), S(13, X)});      // sb.x = slot - 16 f1
	e.ins(OP_MAD, {T(13, kMaskY), S(15, Y), C(7, X, M_NEG), S(15, X)});      // sb.y = f1 - 16 f2
	e.ins(OP_MOV, {T(13, kMaskZ), S(15, Y)});                                // sb.z = f2
	e.ins(OP_MAD, {T(12, kMaskXYZ), S(9), C(6, W, M_NEG), S(8)});            // local cell = rel - 8 bi
	e.ins(OP_MAD, {T(13, kMaskXYZ), S(13), C(6, W), S(12)});                 // sb * 8 + local
	e.ins(OP_ADD, {T(13, kMaskXYZ), S(13), C(6, Z)});
	e.ins(OP_MUL, {T(13, kMaskXYZ), S(13), C(5, W)});                        // / 128
	e.ins(OP_TEXLD, {T(12), S(13), Src(R_SAMPLER, kSamAtl)});
	e.ins(OP_MUL, {T(12, kMaskX), S(12, X), S(10, X)});
	e.ins(OP_MUL, {T(outReg, kMaskX), S(12, X), S(14, X)});
}

void EmitPrologue(std::vector<uint32_t> &t, uint32_t vin, bool audit) {
	Emit e{t};
	const uint32_t X = kXXXX, W = kWWWW;
	e.ins(OP_MOV, {T(0, kMaskXY), Src(R_INPUT, vin)});
	e.ins(OP_MOV, {T(0, kMaskZ), Src(R_INPUT, vin, W)});
	e.ins(OP_MOV, {T(0, kMaskW), C(6, X)});
	e.ins(OP_DP4, {T(1, kMaskX), S(0), C(0)});
	e.ins(OP_DP4, {T(1, kMaskY), S(0), C(1)});
	e.ins(OP_DP4, {T(1, kMaskZ), S(0), C(2)});
	e.ins(OP_DSX, {T(2, kMaskXYZ), S(1)});
	e.ins(OP_DSY, {T(3, kMaskXYZ), S(1)});
	e.ins(OP_CRS, {T(7, kMaskXYZ), S(2), S(3)});
	e.ins(OP_DP3, {T(3, kMaskX), S(7), S(7)});
	e.ins(OP_ADD, {T(3, kMaskX), S(3, X), C(7, W)});
	e.ins(OP_RSQ, {T(3, kMaskX), S(3, X)});
	e.ins(OP_MUL, {T(2, kMaskXYZ), S(7), S(3, X)});
	e.ins(OP_MUL, {T(2, kMaskXYZ), S(2), C(3, W)});                           // normal * eps (cells)
	e.ins(OP_ADD, {T(4, kMaskXYZ), S(1), S(2)});
	e.ins(OP_ADD, {T(5, kMaskXYZ), S(1), S(2, kXYZW, M_NEG)});
	e.ins(OP_FRC, {T(6, kMaskXYZ), S(4)});
	e.ins(OP_ADD, {T(4, kMaskXYZ), S(4), S(6, kXYZW, M_NEG)});
	e.ins(OP_FRC, {T(6, kMaskXYZ), S(5)});
	e.ins(OP_ADD, {T(5, kMaskXYZ), S(5), S(6, kXYZW, M_NEG)});
	EmitDug(e, 4, 2);
	EmitDug(e, 5, 3);
	e.ins(OP_ADD, {T(0), S(2, X), S(3, X)});
	e.ins(OP_ADD, {T(0), C(6, kZZZZ), S(0, kXYZW, M_NEG)});                  // 0.5 - (dug behind + dug in front): negative when either is dug
	if (audit) e.ins(OP_MOV, {T(30), S(0)});                                  // (audit: the verdict is kept in r30 and painted at the end)
	else e.ins(OP_TEXKILL, {T(0)});
}

Reason PatchPs(const std::vector<uint32_t> &code, const Info &in, std::vector<uint32_t> &out, bool audit = false) {
	if (in.model != 0x0300) return R_NOT_SM3;
	if (in.tc15) return R_TC15_USED;
	if (in.samplerMask & ((1u << kSamIdx) | (1u << kSamAtl))) return R_SAMPLERS_BUSY;
	if (in.maxConst >= kK) return R_CONSTS_BUSY;
	const int m = FirstFree(in.inMask, 10);
	if (m < 0) return R_NO_INPUT_SLOT;
	out.clear();
	out.reserve(code.size() + 400);
	out.insert(out.end(), code.begin(), code.begin() + in.firstIns);
	out.push_back(OP_DCL | (2u << 24)), out.push_back(Usage_(U_TEXCOORD, kTexcoordSem)), out.push_back(Dst(R_INPUT, (uint32_t)m, kMaskXYZW));
	out.push_back(OP_DCL | (2u << 24)), out.push_back(0x80000000u | ((uint32_t)TT_VOLUME << 27)), out.push_back(Dst(R_SAMPLER, kSamIdx));
	out.push_back(OP_DCL | (2u << 24)), out.push_back(0x80000000u | ((uint32_t)TT_VOLUME << 27)), out.push_back(Dst(R_SAMPLER, kSamAtl));
	EmitPrologue(out, (uint32_t)m, audit);
	if (!audit) {
		out.insert(out.end(), code.begin() + in.firstIns, code.end());
		return R_OK;
	}
	// audit (pc_cut_audit): the shader's colour output (oC0) is written to r29 instead, r29 is set to black first, and the last instruction paints
	// magenta where the verdict in r30 says the fragment is inside a cut, the shader's own colour elsewhere
	{
		Emit e{out};
		e.ins(OP_MOV, {T(29), C(7, kWWWW)});
		e.ins(OP_MOV, {T(28, kMaskXZW), C(6, kXXXX)});   // magenta from the existing constants: (1, ~0, 1, 1)
		e.ins(OP_MOV, {T(28, kMaskY), C(7, kWWWW)});
	}
	for (size_t i = in.firstIns; i < code.size();) {
		const uint32_t t = code[i];
		if (t == 0x0000FFFFu) break;
		const uint32_t op = t & 0xFFFF;
		if (op == OP_COMMENT) {
			const size_t n = 1 + ((t >> 16) & 0x7FFF);
			out.insert(out.end(), code.begin() + i, code.begin() + i + n);
			i += n;
			continue;
		}
		const size_t len = (t >> 24) & 0xF;
		const size_t at = out.size();
		out.insert(out.end(), code.begin() + i, code.begin() + i + 1 + len);
		for (size_t k = 1; k <= len; ++k) {
			const uint32_t p = out[at + k];
			if (!(p & 0x80000000u)) continue;
			const Reg ty = TypeOf(p);
			const uint32_t n = NumOf(p);
			if (ty == R_TEMP && n >= 28) return R_TEMPS_BUSY;
			if (k == 1 && ty == R_COLOROUT && n == 0) out[at + 1] = (p & ~((7u << 28) | (3u << 11) | 0x7FFu)) | 29u;
		}
		i += 1 + len;
	}
	{
		Emit e{out};
		e.ins(OP_CMP, {Dst(R_COLOROUT, 0), S(30, kXXXX), S(29), S(28)});
	}
	out.push_back(0x0000FFFFu);
	return R_OK;
}


// A pixel shader that only kills: bound in place of "no pixel shader" for the world's depth-only draws (Portal 2 lays the world's depth down first with a
// vertex shader and no pixel shader). Without it that pass writes the depth of the wall inside a cut, and whatever lies behind the hole (the revealed
// blocks, the room beyond) fails the depth test against it: the hole shows the clear colour (white).
void BuildKillPs(std::vector<uint32_t> &out) {
	out.clear();
	out.push_back(kPs30);
	out.push_back(OP_DCL | (2u << 24)), out.push_back(Usage_(U_TEXCOORD, kTexcoordSem)), out.push_back(Dst(R_INPUT, 0, kMaskXYZW));
	out.push_back(OP_DCL | (2u << 24)), out.push_back(0x80000000u | ((uint32_t)TT_VOLUME << 27)), out.push_back(Dst(R_SAMPLER, kSamIdx));
	out.push_back(OP_DCL | (2u << 24)), out.push_back(0x80000000u | ((uint32_t)TT_VOLUME << 27)), out.push_back(Dst(R_SAMPLER, kSamAtl));
	EmitPrologue(out, 0, false);
	Emit e{out};
	e.ins(OP_MOV, {Dst(R_COLOROUT, 0), C(6, kXXXX)});
	out.push_back(0x0000FFFFu);
}

// ---- shader model 2 -> 3 upgrade -----------------------------------------------------------------------------------------------------------------------
// Portal 2 under -vulkan draws most of the world with vs_2_0 + ps_2_x (ps_2_b) pairs, which have no derivatives and few constants. The cut
// needs model 3, so such a pair is rewritten as vs_3_0 + ps_3_0 first (the documented register mapping: the pixel shader's t# become
// texcoord inputs, v0/v1 the colour inputs; the vertex shader's oPos / oD# / oT# / oFog become declared outputs, the colours saturated as the
// model 2 rasterizer did), then patched like a native model 3 pair. A pair is only bound when the pixel shader's inputs are all provided by
// the vertex shader's outputs (the semantic bits below).
constexpr uint32_t kSemFog = 1u << 10;
uint32_t SemColor(uint32_t i) { return 1u << i; }
uint32_t SemTex(uint32_t i) { return 1u << (2 + i); }

// sincos: model 2 takes two more sources (the macro's constant registers), model 3 only the angle; Microsoft's d3d9 refuses the model 2
// form in a model 3 shader (DXVK took it). The instruction at `at` is cut down to dst, src.
constexpr uint32_t kOpSincos = 37;
void SincosToSm3(std::vector<uint32_t> &out, size_t at, uint32_t op, size_t len) {
	if (op != kOpSincos || len != 4) return;
	out.resize(at + 3);
	out[at] = (out[at] & ~(0xFu << 24)) | (2u << 24);
}

int g_upFail = 0;  // why the last UpgradeVs / UpgradePs returned false
// keep: the interpolants to keep (the others' writes are dropped like fog's); a full model 2 vertex shader (position, both colours and
// all eight texture coordinates) leaves no output for the cut, so it is rebuilt per pairing with only what that pixel shader reads.
bool UpgradeVs(const std::vector<uint32_t> &code, std::vector<uint32_t> &out, uint32_t &provides, uint32_t keep = ~0u) {
	Info in;
	if (!Analyze(code, in) || in.pixel || (in.model != 0x0200 && in.model != 0x0201)) return g_upFail = 1, false;
	// which outputs are written
	uint32_t used = 0;
	for (size_t i = in.firstIns; i < code.size();) {
		const uint32_t t = code[i];
		if (t == 0x0000FFFFu) break;
		const uint32_t op = t & 0xFFFF;
		if (op == OP_COMMENT) {
			i += 1 + ((t >> 16) & 0x7FFF);
			continue;
		}
		const size_t len = (t >> 24) & 0xF;
		if (len >= 1 && (code[i + 1] & 0x80000000u)) {
			const uint32_t p = code[i + 1];
			const Reg ty = TypeOf(p);
			const uint32_t n = NumOf(p);
			if (ty == R_RASTOUT) {
				if (n == 1) {
					// (the fog output: a model 2 pixel shader cannot read it; its writes are dropped below)
				} else if (n == 2) return g_upFail = 2, false;  // point size
			} else if (ty == R_ATTROUT) {
				if (n < 2) used |= SemColor(n);
			} else if (ty == R_OUTPUT) {  // (texture coordinate output in model 2)
				if (n < 8) used |= SemTex(n);
				else return g_upFail = 3, false;
			}
		}
		i += 1 + len;
	}
	used &= keep;
	// numbering: o0 position, then colours, texture coordinates, fog
	int num[11];
	int next = 1;
	for (int b = 0; b < 11; ++b) num[b] = (used >> b) & 1 ? next++ : -1;
	if (next > 11) return g_upFail = 4, false;
	out.clear();
	out.push_back(kVs30);
	for (size_t i = 1; i < in.firstIns; ++i) out.push_back(code[i]);
	auto dcl = [&](Usage u, uint32_t idx, int reg, uint32_t mask) { out.push_back(OP_DCL | (2u << 24)), out.push_back(Usage_(u, idx)), out.push_back(Dst(R_OUTPUT, (uint32_t)reg, mask)); };
	dcl(U_POSITION, 0, 0, kMaskXYZW);
	for (uint32_t k = 0; k < 2; ++k)
		if (used & SemColor(k)) dcl(U_COLOR, k, num[k], kMaskXYZW);
	for (uint32_t k = 0; k < 8; ++k)
		if (used & SemTex(k)) dcl(U_TEXCOORD, k, num[2 + k], kMaskXYZW);
	if (used & kSemFog) dcl(U_FOG, 0, num[10], kMaskX);
	for (size_t i = in.firstIns; i < code.size();) {
		const uint32_t t = code[i];
		if (t == 0x0000FFFFu) break;
		const uint32_t op = t & 0xFFFF;
		size_t len;
		if (op == OP_COMMENT) len = (t >> 16) & 0x7FFF;
		else len = (t >> 24) & 0xF;
		const size_t at = out.size();
		out.insert(out.end(), code.begin() + i, code.begin() + i + 1 + len);
		if (op != OP_COMMENT && len >= 1 && (code[i + 1] & 0x80000000u)) {
			uint32_t p = out[at + 1];
			const Reg ty = TypeOf(p);
			const uint32_t n = NumOf(p);
			int reg = -1;
			bool sat = false;
			if (ty == R_RASTOUT && n == 1) {
				out.resize(at);  // drop the fog write
				i += 1 + len;
				continue;
			}
			if ((ty == R_ATTROUT && n < 2 && !(used & SemColor(n))) || (ty == R_OUTPUT && n < 8 && !(used & SemTex(n)))) {
				out.resize(at);  // an interpolant not kept for this pairing
				i += 1 + len;
				continue;
			}
			if (ty == R_RASTOUT) reg = n == 0 ? 0 : -1;
			else if (ty == R_ATTROUT) reg = num[n], sat = true;
			else if (ty == R_OUTPUT) reg = num[2 + n];
			if (reg >= 0) {
				p = (p & ~(0x7u << 28) & ~(0x3u << 11) & ~0x7FFu) | ((uint32_t)R_OUTPUT << 28) | (uint32_t)reg;
				if (sat) p |= (uint32_t)DM_SAT << 20;
				out[at + 1] = p;
			}
		}
		SincosToSm3(out, at, op, len);
		i += 1 + len;
	}
	out.push_back(0x0000FFFFu);
	provides = used;
	return true;
}

bool UpgradePs(const std::vector<uint32_t> &code, std::vector<uint32_t> &out, uint32_t &needs) {
	Info in;
	if (!Analyze(code, in) || !in.pixel || (in.model != 0x0200 && in.model != 0x0201)) return false;
	// the inputs declared (t# and v#), numbered in order of declaration
	int mapT[8], mapV[2];
	for (int &m : mapT) m = -1;
	for (int &m : mapV) m = -1;
	needs = 0;
	int next = 0;
	for (size_t i = 1; i < in.firstIns;) {
		const uint32_t t = code[i];
		const uint32_t op = t & 0xFFFF;
		if (op == OP_COMMENT) {
			i += 1 + ((t >> 16) & 0x7FFF);
			continue;
		}
		const size_t len = (t >> 24) & 0xF;
		if (op == OP_DCL && len >= 2) {
			const uint32_t d = code[i + 2];
			if (TypeOf(d) == R_TEXTURE && NumOf(d) < 8) mapT[NumOf(d)] = next++, needs |= SemTex(NumOf(d));
			else if (TypeOf(d) == R_INPUT && NumOf(d) < 2) mapV[NumOf(d)] = next++, needs |= SemColor(NumOf(d));
		}
		i += 1 + len;
	}
	if (next > 10) return false;
	out.clear();
	out.push_back(kPs30);
	for (size_t i = 1; i < code.size();) {
		const uint32_t t = code[i];
		if (t == 0x0000FFFFu) break;
		const uint32_t op = t & 0xFFFF;
		if (op == OP_COMMENT) {
			const size_t n = 1 + ((t >> 16) & 0x7FFF);
			out.insert(out.end(), code.begin() + i, code.begin() + i + n);
			i += n;
			continue;
		}
		const size_t len = (t >> 24) & 0xF;
		const size_t at = out.size();
		out.insert(out.end(), code.begin() + i, code.begin() + i + 1 + len);
		if (op == OP_DCL && len >= 2) {
			const uint32_t d = code[i + 2];
			const uint32_t mask = (d >> 16) & 0xF;
			if (TypeOf(d) == R_TEXTURE && NumOf(d) < 8) out[at + 1] = Usage_(U_TEXCOORD, NumOf(d)), out[at + 2] = Dst(R_INPUT, (uint32_t)mapT[NumOf(d)], mask ? mask : kMaskXYZW);
			else if (TypeOf(d) == R_INPUT && NumOf(d) < 2) out[at + 1] = Usage_(U_COLOR, NumOf(d)), out[at + 2] = Dst(R_INPUT, (uint32_t)mapV[NumOf(d)], mask ? mask : kMaskXYZW);
		} else if (op == OP_DEF || op == OP_DEFI || op == OP_DEFB) {
		} else {
			if (op == OP_TEXKILL && len >= 1 && TypeOf(code[i + 1]) == R_TEXTURE) return false;
			for (size_t k = 1; k <= len; ++k) {
				uint32_t p = out[at + k];
				if (!(p & 0x80000000u)) continue;
				const Reg ty = TypeOf(p);
				const uint32_t n = NumOf(p);
				int to = -1;
				if (ty == R_TEXTURE && n < 8) to = mapT[n];
				else if (ty == R_INPUT && n < 2) to = mapV[n];
				if (ty == R_TEXTURE || (ty == R_INPUT && n < 2)) {
					if (to < 0) return false;  // used without a declaration
					out[at + k] = (p & ~(0x7u << 28) & ~(0x3u << 11) & ~0x7FFu) | ((uint32_t)R_INPUT << 28) | (uint32_t)to;
				}
			}
			SincosToSm3(out, at, op, len);
		}
		i += 1 + len;
	}
	out.push_back(0x0000FFFFu);
	return true;
}

// ---- a tiny disassembler (pc_cut_dump) ----------------------------------------------------------------------------------------------------------
const char *OpName(uint32_t op) {
	switch (op) {
	case OP_NOP: return "nop"; case OP_MOV: return "mov"; case OP_ADD: return "add"; case OP_SUB: return "sub"; case OP_MAD: return "mad"; case OP_MUL: return "mul";
	case OP_RCP: return "rcp"; case OP_RSQ: return "rsq"; case OP_DP3: return "dp3"; case OP_DP4: return "dp4"; case OP_MIN: return "min"; case OP_MAX: return "max";
	case OP_SLT: return "slt"; case OP_SGE: return "sge"; case OP_EXP: return "exp"; case OP_LOG: return "log"; case OP_LRP: return "lrp"; case OP_FRC: return "frc";
	case OP_CALL: return "call"; case OP_LABEL: return "label"; case OP_DCL: return "dcl"; case OP_POW: return "pow"; case OP_CRS: return "crs"; case OP_ABS: return "abs";
	case OP_NRM: return "nrm"; case OP_REP: return "rep"; case OP_ENDREP: return "endrep"; case OP_IF: return "if"; case OP_IFC: return "ifc"; case OP_ELSE: return "else";
	case OP_ENDIF: return "endif"; case OP_MOVA: return "mova"; case OP_DEFB: return "defb"; case OP_DEFI: return "defi"; case OP_TEXKILL: return "texkill"; case OP_TEXLD: return "texld";
	case OP_DEF: return "def"; case OP_CMP: return "cmp"; case OP_DP2ADD: return "dp2add"; case OP_DSX: return "dsx"; case OP_DSY: return "dsy"; case OP_TEXLDL: return "texldl";
	case 16: return "lit"; case 17: return "dst"; case 27: return "loop"; case 28: return "ret"; case 29: return "endloop"; case 34: return "sgn"; case 37: return "sincos";
	case 44: return "break"; case 45: return "breakc"; case 93: return "texldd"; case 94: return "setp"; case 96: return "breakp";
	default: return nullptr;
	}
}
std::string Param(uint32_t p, bool dst) {
	static const char *tn[] = {"r", "v", "c", "t", "rast", "attr", "o", "i", "oC", "oDepth", "s", "?11", "?12", "?13", "b", "aL", "?16", "misc", "l", "p"};
	const Reg ty = TypeOf(p);
	char b[64];
	std::string s;
	if (!dst && ((p >> 24) & 0xF) == M_NEG) s += "-";
	std::snprintf(b, sizeof b, "%s%u", ty < 20 ? tn[ty] : "?", NumOf(p));
	s += b;
	if (p & (1u << 13)) s += "[rel]";
	const char *cc = "xyzw";
	if (dst) {
		const uint32_t m = (p >> 16) & 0xF;
		if (m != 0xF) {
			s += ".";
			for (int i = 0; i < 4; ++i)
				if (m & (1u << i)) s += cc[i];
		}
	} else {
		const uint32_t sw = (p >> 16) & 0xFF;
		if (sw != kXYZW) {
			s += ".";
			for (int i = 0; i < 4; ++i) s += cc[(sw >> (2 * i)) & 3];
		}
	}
	return s;
}
std::string Disasm(const std::vector<uint32_t> &code) {
	std::string out;
	char b[96];
	std::snprintf(b, sizeof b, "%s_%d_%d  (%zu tokens)\n", (code[0] >> 16) == 0xFFFF ? "ps" : "vs", (code[0] >> 8) & 255, code[0] & 255, code.size());
	out += b;
	size_t i = 1;
	while (i < code.size()) {
		const uint32_t t = code[i];
		if (t == 0xFFFF) {
			out += "end\n";
			break;
		}
		const uint32_t op = t & 0xFFFF;
		if (op == OP_COMMENT) {
			i += 1 + ((t >> 16) & 0x7FFF);
			continue;
		}
		const size_t len = (t >> 24) & 0xF;
		const char *n = OpName(op);
		if (n) out += n;
		else std::snprintf(b, sizeof b, "op%u", op), out += b;
		if (op == OP_DCL && len >= 2) {
			const uint32_t u = code[i + 1];
			const Reg ty = TypeOf(code[i + 2]);
			if (ty == R_SAMPLER) std::snprintf(b, sizeof b, "_%s", ((u >> 27) & 15) == 4 ? "volume" : ((u >> 27) & 15) == 3 ? "cube" : "2d");
			else std::snprintf(b, sizeof b, "_usage%u_%u", u & 0x1F, (u >> 16) & 0xF);
			out += b;
			out += " " + Param(code[i + 2], true);
		} else if (op == OP_DEF && len == 5) {
			float f[4];
			std::memcpy(f, &code[i + 2], 16);
			std::snprintf(b, sizeof b, " %s, %g, %g, %g, %g", Param(code[i + 1], true).c_str(), f[0], f[1], f[2], f[3]);
			out += b;
		} else {
			const bool hasDst = !(op == OP_IF || op == OP_IFC || op == OP_REP || op == OP_CALL || op == 26 || op == OP_LABEL || op == 27 || op == 45 || op == 96);
			for (size_t k = 1; k <= len; ++k) {
				out += k == 1 ? " " : ", ";
				out += Param(code[i + k], k == 1 && hasDst);
			}
		}
		out += "\n";
		i += 1 + len;
	}
	return out;
}

// ---- per-shader records ---------------------------------------------------------------------------------------------------------------------------
struct Sh {
	bool pixel = false;
	int model = 0;
	Reason why = R_OK;
	void *variant = nullptr, *variantAudit = nullptr;
	std::vector<uint32_t> psSrc;   // pixel shader: the (upgraded) tokens the audit variant is made from
	Info psInfo;
	uint32_t tokens = 0, patchedTokens = 0;
	bool upgraded = false;   // model 2 rewritten as model 3
	uint32_t sem = 0;        // upgraded vertex shader: the interpolants it writes; pixel shader: the ones it reads
	bool perPair = false;    // upgraded vertex shader with no output left for the cut: variants per kept interpolant set (VsVariantKeep)
	std::vector<uint32_t> vsCode;
	std::unordered_map<uint32_t, void *> byKeep;
};
std::unordered_map<void *, Sh> g_sh;            // engine shader -> record (we hold a reference on the engine's shader: its address cannot be reused)
std::unordered_map<void *, void *> g_origOf;    // variant -> engine shader
std::mutex g_mx;                                // counters / dump only

std::atomic<long> g_nShaders[2]{{0}, {0}}, g_nPatched[2]{{0}, {0}};
std::atomic<long> g_reasonShaders[R_COUNT];
std::atomic<long> g_drawsPatched{0}, g_drawsUnpatched{0}, g_drawReason[R_COUNT], g_bindsOn{0};
std::atomic<long> g_constClobbers{0}, g_viewOns{0}, g_texUploads{0}, g_texFail{0};
std::atomic<long> g_patchUs{0}, g_patchWorstUs{0};
std::atomic<long> g_modelDraws[3][3];            // [vs class][ps class] draws in the window (class: 0 none/fixed function, 1 sm2, 2 sm3)
std::atomic<long> g_modelDrawsPatched[3][3];
std::string g_sampleVsOrig, g_samplePsOrig, g_sampleVs, g_samplePs;
int g_createFailLogs = 0;

// ---- render-thread state -------------------------------------------------------------------------------------------------------------------------------
struct St {
	void *dev = nullptr;
	bool installed = false, on = false, listeners = false;
	void *engVS = nullptr, *engPS = nullptr, *actVS = nullptr, *actPS = nullptr;
	void *engTex[2] = {nullptr, nullptr};   // the engine's textures at samplers 14 / 15 (we hold a reference while the window is open)
	uint32_t engSamp[2][7] = {};            // its sampler states there
	bool mineBound = false;
	void *idxTex = nullptr, *atlTex = nullptr;
	int idxDims[3] = {0, 0, 0};
	std::shared_ptr<rb::CutBricks> bricks;
	bool dirty = false, haveView = false;
	float K[8][4] = {};   // c216..c223: ps_3_0 has 224 constants in all, so this is the last 8
	bool audit = false;
	bool constsDirty = true;
	bool reasonLogged[R_COUNT] = {};
} g;

constexpr uint32_t kSampStates[7] = {D3DSAMP_ADDRESSU_, D3DSAMP_ADDRESSV_, D3DSAMP_ADDRESSW_, D3DSAMP_MAGFILTER_, D3DSAMP_MINFILTER_, D3DSAMP_MIPFILTER_, D3DSAMP_SRGBTEXTURE_};
constexpr uint32_t kMineSampler[7] = {D3DTADDRESS_CLAMP_, D3DTADDRESS_CLAMP_, D3DTADDRESS_CLAMP_, D3DTEXF_POINT_, D3DTEXF_POINT_, D3DTEXF_NONE_, 0};

using SetShaderT = HRESULT_(PCB_D3DAPI *)(void *, void *);
void DevSetShader(bool pixel, void *s) {
	InternalScope sc;
	Method<SetShaderT>(g.dev, pixel ? kSetPixelShader : kSetVertexShader)(g.dev, s);
}
void DevSetTexture(uint32_t stage, void *tex) {
	InternalScope sc;
	Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, void *)>(g.dev, kSetTexture)(g.dev, stage, tex);
}
void DevSetSampler(uint32_t stage, uint32_t state, uint32_t v) {
	InternalScope sc;
	Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, DWORD_, DWORD_)>(g.dev, kSetSamplerState)(g.dev, stage, state, v);
}
uint32_t DevGetSampler(uint32_t stage, uint32_t state) {
	InternalScope sc;
	DWORD_ v = 0;
	Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, DWORD_, DWORD_ *)>(g.dev, kGetSamplerState)(g.dev, stage, state, &v);
	return v;
}
void *DevGetShader(bool pixel) {  // AddRef'd
	InternalScope sc;
	void *s = nullptr;
	Method<HRESULT_(PCB_D3DAPI *)(void *, void **)>(g.dev, pixel ? kGetPixelShader : kGetVertexShader)(g.dev, &s);
	return s;
}
void *DevGetTexture(uint32_t stage) {  // AddRef'd
	InternalScope sc;
	void *t = nullptr;
	Method<HRESULT_(PCB_D3DAPI *)(void *, DWORD_, void **)>(g.dev, kGetTexture)(g.dev, stage, &t);
	return t;
}
void UploadConsts() {
	InternalScope sc;
	const HRESULT_ hr = Method<HRESULT_(PCB_D3DAPI *)(void *, UINT_, const float *, UINT_)>(g.dev, kSetPixelShaderConstantF)(g.dev, kK, &g.K[0][0], 8);
	static int logged = 0;
	if (Failed(hr) && logged++ < 3) Printf("[bridge] cut(d3d9): SetPixelShaderConstantF c%d..c%d refused (0x%08x): the cut will not work\n", kK, kK + 7, (unsigned)hr);
	g.constsDirty = false;
}

// ---- textures ---------------------------------------------------------------------------------------------------------------------------------------
void ReleaseTextures() {
	if (g.idxTex) Release(g.idxTex), g.idxTex = nullptr;
	if (g.atlTex) Release(g.atlTex), g.atlTex = nullptr;
	g.idxDims[0] = g.idxDims[1] = g.idxDims[2] = 0;
	g.dirty = g.bricks != nullptr;
}
void *MakeVolume(int w, int h, int d, DWORD_ fmt) {
	InternalScope sc;
	void *t = nullptr;
	// (UINT w, h, depth, levels, DWORD usage, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9 **, HANDLE *)
	const HRESULT_ hr = Method<HRESULT_(PCB_D3DAPI *)(void *, UINT_, UINT_, UINT_, UINT_, DWORD_, DWORD_, DWORD_, void **, void *)>(g.dev, kCreateVolumeTexture)(g.dev, (UINT_)w, (UINT_)h, (UINT_)d, 1,
	                                                                                                                                                         D3DUSAGE_DYNAMIC_, fmt, D3DPOOL_DEFAULT_, &t, nullptr);
	if (Failed(hr) || !t) {
		if (g_createFailLogs++ < 4) Printf("[bridge] cut(d3d9): CreateVolumeTexture %dx%dx%d format %u failed (0x%08x)\n", w, h, d, fmt, (unsigned)hr);
		return nullptr;
	}
	return t;
}
bool FillVolume(void *tex, int w, int h, int d, int bytesPerTexel, const uint8_t *src) {
	InternalScope sc;
	D3DLOCKED_BOX_ lb{};
	if (Failed(Method<HRESULT_(PCB_D3DAPI *)(void *, UINT_, D3DLOCKED_BOX_ *, const void *, DWORD_)>(tex, kVolLockBox)(tex, 0, &lb, nullptr, D3DLOCK_DISCARD_))) return false;
	const size_t row = (size_t)w * bytesPerTexel;
	for (int z = 0; z < d; ++z)
		for (int y = 0; y < h; ++y) std::memcpy((uint8_t *)lb.pBits + (size_t)z * lb.SlicePitch + (size_t)y * lb.RowPitch, src + ((size_t)z * h + y) * row, row);
	Method<HRESULT_(PCB_D3DAPI *)(void *, UINT_)>(tex, kVolUnlockBox)(tex, 0);
	return true;
}
bool EnsureTextures() {
	if (!g.bricks) return false;
	if (g.dirty || !g.idxTex || !g.atlTex) {
		const rb::CutBricks &b = *g.bricks;
		if (g.idxTex && (g.idxDims[0] != b.dims[0] || g.idxDims[1] != b.dims[1] || g.idxDims[2] != b.dims[2])) Release(g.idxTex), g.idxTex = nullptr;
		if (!g.idxTex) g.idxTex = MakeVolume(b.dims[0], b.dims[1], b.dims[2], D3DFMT_A8L8_);
		if (!g.atlTex) g.atlTex = MakeVolume(128, 128, 128, D3DFMT_L8_);
		if (!g.idxTex || !g.atlTex) return ++g_texFail, false;
		std::memcpy(g.idxDims, b.dims, sizeof g.idxDims);
		if (!FillVolume(g.idxTex, b.dims[0], b.dims[1], b.dims[2], 2, b.index.data()) || !FillVolume(g.atlTex, 128, 128, 128, 1, b.atlas.data())) return ++g_texFail, false;
		g.dirty = false;
		++g_texUploads;
	}
	return true;
}

// samplers 14 / 15: ours while a patched pixel shader is bound, the engine's otherwise
void SetMine(bool mine) {
	if (mine == g.mineBound) return;
	if (mine) {
		for (int s = 0; s < 2; ++s)
			for (int k = 0; k < 7; ++k) g.engSamp[s][k] = DevGetSampler(kSamIdx + s, kSampStates[k]);
		for (int s = 0; s < 2; ++s)
			for (int k = 0; k < 7; ++k) DevSetSampler(kSamIdx + s, kSampStates[k], kMineSampler[k]);
		DevSetTexture(kSamIdx, g.idxTex), DevSetTexture(kSamAtl, g.atlTex);
	} else {
		DevSetTexture(kSamIdx, g.engTex[0]), DevSetTexture(kSamAtl, g.engTex[1]);
		for (int s = 0; s < 2; ++s)
			for (int k = 0; k < 7; ++k) DevSetSampler(kSamIdx + s, kSampStates[k], g.engSamp[s][k]);
	}
	g.mineBound = mine;
}

// ---- finding / building the variants -------------------------------------------------------------------------------------------------------
std::vector<uint32_t> GetCode(void *shader) {
	InternalScope sc;
	std::vector<uint32_t> code;
	UINT_ sz = 0;
	auto fn = Method<HRESULT_(PCB_D3DAPI *)(void *, void *, UINT_ *)>(shader, kShaderGetFunction);
	if (Failed(fn(shader, nullptr, &sz)) || sz < 8 || sz > (1u << 24)) return code;
	code.resize((sz + 3) / 4);
	UINT_ sz2 = sz;
	if (Failed(fn(shader, code.data(), &sz2))) code.clear();
	return code;
}

void DumpRefused(const char *kind, const std::vector<uint32_t> *orig, const std::vector<uint32_t> &out);
Sh &Lookup(void *shader, bool pixel) {
	auto it = g_sh.find(shader);
	if (it != g_sh.end()) return it->second;
	Sh &r = g_sh[shader];
	r.pixel = pixel;
	AddRef(shader);  // (the address is a key: the engine must not free it and have another shader reuse it)
	const auto t0 = std::chrono::steady_clock::now();
	std::vector<uint32_t> code = GetCode(shader);
	Info in;
	std::vector<uint32_t> out;
	Reason why;
	if (code.empty()) why = R_NO_BYTECODE;
	else if (!Analyze(code, in) || in.pixel != pixel) why = R_PARSE;
	else {
		r.model = in.model;
		r.tokens = (uint32_t)code.size();
		std::vector<uint32_t> up;
		const std::vector<uint32_t> *src = &code;
		Info in3 = in;
		why = R_OK;
		if (in.model == 0x0200 || in.model == 0x0201) {
			uint32_t sem = 0;
			g_upFail = 0;
			if (!(pixel ? UpgradePs(code, up, sem) : UpgradeVs(code, up, sem))) {
				why = R_SM2_UNSUPPORTED;
				static int logged = 0;
				if (logged++ < 3) {
					Printf("[bridge] cut(d3d9): a %s_2 shader (%zu tokens) could not be upgraded (reason %d)\n", pixel ? "ps" : "vs", code.size(), g_upFail);
					std::ofstream(CacheDir() + "/cut_d3d9_fail_" + (pixel ? "ps" : "vs") + std::to_string(logged) + ".txt") << Disasm(code);
				}
			} else if (!Analyze(up, in3)) {
				why = R_SM2_UNSUPPORTED;
				Printf("[bridge] cut(d3d9): the upgraded %s shader does not parse\n", pixel ? "pixel" : "vertex");
				std::ofstream(CacheDir() + "/cut_d3d9_fail_up.txt") << Disasm(up);
			}
			if (why == R_OK) src = &up, r.upgraded = true, r.sem = sem;
			if (why == R_OK && !pixel && __builtin_popcount(sem) > 9) r.perPair = true, r.vsCode = code;
		}
		if (why == R_OK && r.perPair) {
			// (built per pairing: VsVariantKeep)
		} else if (why == R_OK) why = pixel ? PatchPs(*src, in3, out) : PatchVs(*src, in3, out);
		if (why == R_OK && pixel) r.psSrc = *src, r.psInfo = in3;
	}
	if (why == R_OK && !r.perPair) {
		void *v = nullptr;
		HRESULT_ hr;
		{
			InternalScope sc;
			hr = Method<HRESULT_(PCB_D3DAPI *)(void *, const uint32_t *, void **)>(g.dev, pixel ? kCreatePixelShader : kCreateVertexShader)(g.dev, out.data(), &v);
		}
		if (Failed(hr) || !v) {
			why = R_CREATE_FAILED;
			if (g_createFailLogs++ < 4) Printf("[bridge] cut(d3d9): the patched %s shader (%zu tokens) was refused (0x%08x)\n", pixel ? "pixel" : "vertex", out.size(), (unsigned)hr);
			DumpRefused(pixel ? "ps" : "vs", &code, out);
		} else {
			r.variant = v;
			r.patchedTokens = (uint32_t)out.size();
			g_origOf[v] = shader;
			std::lock_guard<std::mutex> lk(g_mx);
			std::string &so = pixel ? g_samplePsOrig : g_sampleVsOrig, &sp = pixel ? g_samplePs : g_sampleVs;
			if (sp.empty() || (code.size() > 120 && code.size() < 400 && sp.size() < 2000)) {
				so = Disasm(code);
				sp = Disasm(out);
			}
		}
	}
	r.why = why;
	++g_nShaders[pixel];
	if (why == R_OK) ++g_nPatched[pixel];
	++g_reasonShaders[why];
	const long us = (long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
	g_patchUs += us;
	if (us > g_patchWorstUs.load()) g_patchWorstUs = us;
	return r;
}

void *VariantOf(void *shader, bool pixel) {
	if (!shader) return nullptr;
	Sh &r = Lookup(shader, pixel);
	if (!pixel || !g.audit || !r.variant) return r.variant;
	if (!r.variantAudit) {  // (pc_cut_audit: the pixel shader that paints magenta instead of killing; made on first use)
		std::vector<uint32_t> out;
		const Reason why = PatchPs(r.psSrc, r.psInfo, out, true);
		void *v = nullptr;
		if (why == R_OK) {
			InternalScope sc;
			const HRESULT_ hr = Method<HRESULT_(PCB_D3DAPI *)(void *, const uint32_t *, void **)>(g.dev, kCreatePixelShader)(g.dev, out.data(), &v);
			if (Failed(hr)) v = nullptr, Printf("[bridge] cut(d3d9): the audit pixel shader (%zu tokens) was refused (0x%08x)\n", out.size(), (unsigned)hr);
		} else Printf("[bridge] cut(d3d9): a pixel shader has no audit variant (%s)\n", kReasonName[why]);
		if (v) g_origOf[v] = shader, r.variantAudit = v;
		else r.variantAudit = r.variant;   // (the plain cut for this one)
	}
	return r.variantAudit;
}
bool IsVariant(void *s) { return s && g_origOf.count(s); }
void *OrigOf(void *s) {
	auto it = g_origOf.find(s);
	return it == g_origOf.end() ? s : it->second;
}

// (Windows, 2026-10-12) Microsoft's d3d9 validates shaders strictly where DXVK does not: a refused stream is saved, with the shader it was made
// from, as cache/bridge/d3d9_refused/<n>_<kind>_{orig,out}.bin (raw tokens, the first 8 refusals), to be taken apart offline.
void DumpRefused(const char *kind, const std::vector<uint32_t> *orig, const std::vector<uint32_t> &out) {
	static int n = 0;
	if (n >= 8) return;
	const std::string dir = CacheDir() + "/d3d9_refused";
	plat::MakeDirs(dir);
	auto put = [&](const char *suffix, const std::vector<uint32_t> &t) {
		const std::string f = dir + "/" + std::to_string(n) + "_" + kind + "_" + suffix + ".bin";
		if (FILE *fp = std::fopen(f.c_str(), "wb")) std::fwrite(t.data(), 4, t.size(), fp), std::fclose(fp);
	};
	if (orig) put("orig", *orig);
	put("out", out);
	++n;
}

void *g_killPs = nullptr;
bool g_killPsTried = false;
long g_killPsDraws = 0;
void *KillPs() {
	if (g_killPsTried) return g_killPs;
	g_killPsTried = true;
	std::vector<uint32_t> code;
	BuildKillPs(code);
	void *v = nullptr;
	HRESULT_ hr;
	{
		InternalScope sc;
		hr = Method<HRESULT_(PCB_D3DAPI *)(void *, const uint32_t *, void **)>(g.dev, kCreatePixelShader)(g.dev, code.data(), &v);
	}
	if (Failed(hr) || !v) {
		Printf("[bridge] cut(d3d9): the kill-only pixel shader (depth-only draws) was refused (0x%08x): the world's depth pass is not cut\n", (unsigned)hr);
		DumpRefused("kill", nullptr, code);
		return nullptr;
	}
	g_origOf[v] = nullptr;  // "no pixel shader" is what it stands in for
	g_killPs = v;
	return v;
}

// a per-pairing variant of a full upgraded vertex shader: only the interpolants in keep (0 = position only, for the depth-only pass)
void *VsVariantKeep(void *vs, uint32_t keep) {
	Sh &r = g_sh[vs];
	auto it = r.byKeep.find(keep);
	if (it != r.byKeep.end()) return it->second;
	void *v = nullptr;
	std::vector<uint32_t> up, out;
	uint32_t sem = 0;
	Info in3;
	if (UpgradeVs(r.vsCode, up, sem, keep) && Analyze(up, in3) && PatchVs(up, in3, out) == R_OK) {
		InternalScope sc;
		const HRESULT_ hr = Method<HRESULT_(PCB_D3DAPI *)(void *, const uint32_t *, void **)>(g.dev, kCreateVertexShader)(g.dev, out.data(), &v);
		if (Failed(hr) || !v) {
			v = nullptr;
			if (g_createFailLogs++ < 4) Printf("[bridge] cut(d3d9): a per-pairing vertex shader (keep 0x%x, %zu tokens) was refused (0x%08x)\n", keep, out.size(), (unsigned)hr);
			DumpRefused("vskeep", &r.vsCode, out);
		} else g_origOf[v] = vs;
	}
	r.byKeep[keep] = v;
	return v;
}

// the pair the device should have for the engine's (vs, ps) while the cut is on
void Wanted(void *vs, void *ps, void *&wvs, void *&wps) {
	wvs = vs, wps = ps;
	if (vs && !ps && !g.audit) {  // a depth-only draw: cut it too (the variant of the vertex shader gives the pixel shader the world position)
		void *vv = VariantOf(vs, false);
		if (!vv && g_sh[vs].perPair) vv = VsVariantKeep(vs, 0);
		void *kp = vv ? KillPs() : nullptr;
		if (kp) wvs = vv, wps = kp, ++g_killPsDraws;
		return;
	}
	if (!vs || !ps) return;
	void *vv = VariantOf(vs, false);
	if (!vv && !g_sh[vs].perPair) return;
	void *pv = VariantOf(ps, true);
	if (!pv) return;  // (the pair stays the engine's: a model 2 pixel shader needs the original vertex shader)
	{
		const Sh &a = g_sh[vs], &b = g_sh[ps];
		if (a.upgraded != b.upgraded || (a.upgraded && (b.sem & ~a.sem))) return;  // (the pixel shader reads an interpolant the vertex shader does not write)
		if (a.perPair) vv = VsVariantKeep(vs, b.sem);
	}
	if (!vv) return;
	wvs = vv, wps = pv;
}

void Reconcile() {  // make the device match the wanted pair (render thread, window open)
	void *wvs, *wps;
	Wanted(g.engVS, g.engPS, wvs, wps);
	if (wvs != g.actVS) DevSetShader(false, wvs), g.actVS = wvs;
	if (wps != g.actPS) DevSetShader(true, wps), g.actPS = wps;
	const bool mine = IsVariant(g.actPS);
	if (mine && g.constsDirty) UploadConsts();
	SetMine(mine);
}

// ---- hook-layer listeners -----------------------------------------------------------------------------------------------------------------------
void *OnBind(void *dev, bool isPixel, void *shader) {
	if (!g.on || dev != g.dev) return nullptr;
	++g_bindsOn;
	if (IsVariant(shader)) return nullptr;  // (our own, bound through the device by someone else)
	(isPixel ? g.engPS : g.engVS) = shader;
	void *wvs, *wps;
	Wanted(g.engVS, g.engPS, wvs, wps);
	// the shader being set goes through the hook's own return value; the other half is reconciled now
	if (isPixel) {
		if (wvs != g.actVS) DevSetShader(false, wvs), g.actVS = wvs;
		g.actPS = wps;
		const bool mine = IsVariant(wps);
		if (mine && g.constsDirty) UploadConsts();
		SetMine(mine);
		return wps == shader ? nullptr : wps;
	}
	if (wps != g.actPS) DevSetShader(true, wps), g.actPS = wps;
	g.actVS = wvs;
	SetMine(IsVariant(g.actPS) && IsVariant(wvs));
	return wvs == shader ? nullptr : wvs;
}

void OnTexture(void *dev, uint32_t stage, void *tex) {
	if (!g.on || dev != g.dev || (stage != (uint32_t)kSamIdx && stage != (uint32_t)kSamAtl)) return;
	const int s = (int)stage - kSamIdx;
	if (g.engTex[s] == tex) {
		if (g.mineBound) DevSetTexture(stage, s ? g.atlTex : g.idxTex);
		return;
	}
	if (tex) AddRef(tex);
	if (g.engTex[s]) Release(g.engTex[s]);
	g.engTex[s] = tex;
	if (g.mineBound) DevSetTexture(stage, s ? g.atlTex : g.idxTex);  // (the engine's call replaced ours)
}

void OnConst(void *dev, bool isPixel, uint32_t start, uint32_t count) {
	if (!isPixel || dev != g.dev) return;
	if (start + count > (uint32_t)kK) g.constsDirty = true, ++g_constClobbers;
}

int ClassOf(void *s, bool pixel) {
	if (!s) return 0;
	const Sh &r = Lookup(OrigOf(s), pixel);
	return r.model == 0x0300 ? 2 : 1;
}
void OnDraw(void *dev) {
	if (!g.on || dev != g.dev) return;
	const int vc = ClassOf(g.engVS, false), pc = ClassOf(g.engPS, true);
	++g_modelDraws[vc][pc];
	if (g.mineBound && IsVariant(g.actPS) && IsVariant(g.actVS)) {
		++g_drawsPatched, ++g_modelDrawsPatched[vc][pc];
		return;
	}
	++g_drawsUnpatched;
	Reason why = R_OK;
	if (g.engPS && Lookup(g.engPS, true).why != R_OK) why = Lookup(g.engPS, true).why;
	else if (g.engVS && Lookup(g.engVS, false).why != R_OK) why = Lookup(g.engVS, false).why;
	++g_drawReason[why];  // (R_OK here: fixed function / a null shader)
}

void OnReset(void *dev) {
	if (dev != g.dev) return;
	if (g.on) {  // (a Reset inside the window cannot happen; be safe)
		g.on = false;
	}
	for (int s = 0; s < 2; ++s)
		if (g.engTex[s]) Release(g.engTex[s]), g.engTex[s] = nullptr;
	g.mineBound = false;
	ReleaseTextures();
}

}  // namespace

// ---- rb::CutBackend* ----------------------------------------------------------------------------------------------------------------------------------
namespace rb {

bool CutBackendAvailable() { return d3d9::Active(); }

bool CutBackendInstall(std::string &err) {
	if (!d3d9::Active()) {
		err = "no Direct3D 9 device hook in this run (not -vulkan / not Windows): the cut stays off";
		return false;
	}
	void *dev = d3d9::EngineDevice();
	if (!dev) {
		err = "the Direct3D 9 hook has not seen the engine's device yet (no Present): the cut stays off";
		return false;
	}
	g.dev = dev;
	if (!g.listeners) {
		d3d9::AddResetListener(OnReset);
		g.listeners = true;
	}
	d3d9::EnableBindHooks(true);
	d3d9::SetBindListener(OnBind);
	d3d9::SetTextureListener(OnTexture);
	d3d9::SetDrawListener(OnDraw);
	d3d9::SetConstListener(OnConst);
	g.installed = true;
	Printf("[bridge] cut(d3d9): installed on device %p (shader binding, texture, draw and constant hooks on)\n", dev);
	return true;
}

void CutBackendUpload(const CutBricks &d) {
	g.bricks = std::make_shared<CutBricks>(d);
	g.dirty = true;
}

void CutBackendViewOn(const CutView &v) {
	if (!g.installed) return;
	void *dev = d3d9::EngineDevice();
	if (!dev) return;
	g.dev = dev;
	if (!EnsureTextures()) return CutBackendViewOff();
	// the constants: cellFromClip = cellFromWorld * worldFromClip (3x4 each; the clip vector is (x, y, w, 1))
	double M[3][4], W[3][4];
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 4; ++c) M[r][c] = v.cellFromWorld[r][c], W[r][c] = v.worldFromClip[r][c];
	for (int i = 0; i < 3; ++i) {
		for (int k = 0; k < 4; ++k) {
			double s = 0;
			for (int r = 0; r < 3; ++r) s += M[i][r] * W[r][k];
			if (k == 3) s += M[i][3];
			g.K[i][k] = (float)s;
		}
	}
	const double scale = std::sqrt(M[0][0] * M[0][0] + M[0][1] * M[0][1] + M[0][2] * M[0][2]);  // cells per Source unit
	g.K[3][0] = (float)v.org[0], g.K[3][1] = (float)v.org[1], g.K[3][2] = (float)v.org[2], g.K[3][3] = (float)(v.eps * scale);
	g.K[4][0] = (float)v.dims[0], g.K[4][1] = (float)v.dims[1], g.K[4][2] = (float)v.dims[2], g.K[4][3] = 255.0f;
	g.K[5][0] = 1.0f / (float)std::max(1, v.dims[0]), g.K[5][1] = 1.0f / (float)std::max(1, v.dims[1]), g.K[5][2] = 1.0f / (float)std::max(1, v.dims[2]), g.K[5][3] = 1.0f / 128.0f;
	g.K[6][0] = 1.0f, g.K[6][1] = 0.125f, g.K[6][2] = 0.5f, g.K[6][3] = 8.0f;
	g.K[7][0] = 16.0f, g.K[7][1] = 256.0f, g.K[7][2] = 0.0625f, g.K[7][3] = 1e-20f;
	g.audit = v.audit;
	g.constsDirty = true;
	if (g.on) {  // (a window opened inside a window: the constants only)
		Reconcile();
		return;
	}
	++g_viewOns;
	// what the engine has bound now (it caches its shader state and would not set them again for the first draw)
	void *vs = DevGetShader(false), *ps = DevGetShader(true);
	g.engVS = OrigOf(vs), g.engPS = OrigOf(ps);
	g.actVS = vs, g.actPS = ps;
	if (vs) Release(vs);
	if (ps) Release(ps);
	for (int s = 0; s < 2; ++s) {
		if (g.engTex[s]) Release(g.engTex[s]);
		g.engTex[s] = DevGetTexture(kSamIdx + s);  // (AddRef'd)
	}
	g.mineBound = false;
	g.on = true;
	Reconcile();
}

void CutBackendViewOff() {
	if (!g.installed || !g.on) return;
	g.on = false;
	// the engine's own pair, its textures and sampler states back at 14 / 15
	if (g.engVS != g.actVS) DevSetShader(false, g.engVS), g.actVS = g.engVS;
	if (g.engPS != g.actPS) DevSetShader(true, g.engPS), g.actPS = g.engPS;
	SetMine(false);
	for (int s = 0; s < 2; ++s)
		if (g.engTex[s]) Release(g.engTex[s]), g.engTex[s] = nullptr;
}

void CutBackendUninstall() {
	if (!g.installed) return;
	d3d9::SetBindListener(nullptr);
	d3d9::SetTextureListener(nullptr);
	d3d9::SetDrawListener(nullptr);
	d3d9::SetConstListener(nullptr);
	if (g.on) CutBackendViewOff();
	g.installed = false;
}

std::string CutBackendCounters() {
	char buf[600];
	std::snprintf(buf, sizeof buf,
	              "d3d9: %ld vertex / %ld pixel shaders met in the window, %ld / %ld patched; %ld windows, window draws %ld patched / %ld not; volumes uploaded %ld (failed %ld); engine writes over c216+ %ld",
	              g_nShaders[0].load(), g_nShaders[1].load(), g_nPatched[0].load(), g_nPatched[1].load(), g_viewOns.load(), g_drawsPatched.load(), g_drawsUnpatched.load(), g_texUploads.load(), g_texFail.load(),
	              g_constClobbers.load());
	return buf;
}
std::string CutBackendPatchTiming() {
	char buf[200];
	std::snprintf(buf, sizeof buf, "[bridge] cut: patching at first use took %.1f ms in all, worst shader %.2f ms\n", g_patchUs.load() / 1000.0, g_patchWorstUs.load() / 1000.0);
	return buf;
}
void CutBackendDump() {
	std::lock_guard<std::mutex> lk(g_mx);
	std::ofstream(CacheDir() + "/cut_d3d9_sample_vs_orig.txt") << g_sampleVsOrig;
	std::ofstream(CacheDir() + "/cut_d3d9_sample_vs.txt") << g_sampleVs;
	std::ofstream(CacheDir() + "/cut_d3d9_sample_ps_orig.txt") << g_samplePsOrig;
	std::ofstream(CacheDir() + "/cut_d3d9_sample_ps.txt") << g_samplePs;
	console->Print("[bridge] cut(d3d9): wrote the sample vertex / pixel shader (original and patched) to cache/bridge/cut_d3d9_sample_*.txt\n");
}
void CutBackendPerfBegin() {}
void CutBackendPerfEnd() {}
void CutBackendPerfPoll(double &gpuNs, long &draws, long &lost) { gpuNs = 0, draws = 0, lost = 0; }
void CutBackendPerfReset() {}
bool CutBackendHasPerf() { return false; }

}  // namespace rb

CON_COMMAND(pcb_d3d9_cutstats, "pcb_d3d9_cutstats - (tests) the Direct3D 9 cut: shaders met inside the cut window, patched or why not, and what the window's draws used\n") {
	static const char *cls[3] = {"none/FF", "sm2", "sm3"};
	console->Print("[bridge] cut(d3d9): %s\n", rb::CutBackendCounters().c_str());
	for (int r = 0; r < R_COUNT; ++r) {
		const long n = g_reasonShaders[r].load();
		if (n) console->Print("[bridge] cut(d3d9): shaders (vertex + pixel): %ld %s\n", n, kReasonName[r]);
	}
	for (int r = 0; r < R_COUNT; ++r) {
		const long n = g_drawReason[r].load();
		if (n) console->Print("[bridge] cut(d3d9): window draws left uncut: %ld (%s)\n", n, r == R_OK ? "fixed function / no shader" : kReasonName[r]);
	}
	for (int v = 0; v < 3; ++v)
		for (int p = 0; p < 3; ++p)
			if (g_modelDraws[v][p].load()) console->Print("[bridge] cut(d3d9): window draws vs %s + ps %s: %ld, cut %ld\n", cls[v], cls[p], g_modelDraws[v][p].load(), g_modelDrawsPatched[v][p].load());
	console->Print("[bridge] cut(d3d9): depth-only draws given the kill-only pixel shader: %s, pairs chosen %ld\n", g_killPs ? "created" : "NOT created", g_killPsDraws);
	console->Print("[bridge] cut(d3d9): %zu shaders recorded, %ld binds in the window\n", g_sh.size(), g_bindsOn.load());
	for (const auto &kv : g_sh)
		console->Print("[bridge] cut(d3d9): shader %p %s %d_%d, %u tokens, %s\n", kv.first, kv.second.pixel ? "ps" : "vs", kv.second.model >> 8, kv.second.model & 255, kv.second.tokens, kReasonName[kv.second.why]);
	console->Print("%s", rb::CutBackendPatchTiming().c_str());
}

}  // namespace bridge
