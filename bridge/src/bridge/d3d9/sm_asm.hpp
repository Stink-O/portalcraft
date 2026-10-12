// Portalcraft bridge: a tiny Direct3D 9 shader-token emitter (vs/ps 2.0 and 3.0), for the backend's own shaders (the HUD) and for the
// instructions the cut-away patch inserts into the game's shaders. The token format is the documented one (d3d9types.h, "Shader Code
// Format"): a version token, instruction tokens with their parameter count in bits 24-27, parameter tokens with bit 31 set, comments, and an
// end token 0x0000FFFF. Nothing here comes from Portal 2.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace bridge::d3d9::sm {

enum Op : uint32_t {
	OP_NOP = 0, OP_MOV = 1, OP_ADD = 2, OP_SUB = 3, OP_MAD = 4, OP_MUL = 5, OP_RCP = 6, OP_RSQ = 7, OP_DP3 = 8, OP_DP4 = 9, OP_MIN = 10, OP_MAX = 11,
	OP_SLT = 12, OP_SGE = 13, OP_EXP = 14, OP_LOG = 15, OP_LRP = 18, OP_FRC = 19, OP_CALL = 25, OP_LABEL = 30, OP_DCL = 31, OP_POW = 32, OP_CRS = 33, OP_ABS = 35,
	OP_NRM = 36, OP_REP = 38, OP_ENDREP = 39, OP_IF = 40, OP_IFC = 41, OP_ELSE = 42, OP_ENDIF = 43, OP_MOVA = 46, OP_DEFB = 47, OP_DEFI = 48,
	OP_TEXKILL = 65, OP_TEXLD = 66, OP_DEF = 81, OP_CMP = 88, OP_DP2ADD = 90, OP_DSX = 91, OP_DSY = 92, OP_TEXLDL = 95,
	OP_COMMENT = 0xFFFE, OP_END = 0xFFFF,
};
enum Reg : uint32_t {
	R_TEMP = 0, R_INPUT = 1, R_CONST = 2, R_TEXTURE = 3, R_RASTOUT = 4, R_ATTROUT = 5, R_OUTPUT = 6, R_CONSTINT = 7, R_COLOROUT = 8, R_DEPTHOUT = 9,
	R_SAMPLER = 10, R_CONSTBOOL = 14, R_LOOP = 15, R_MISC = 17,
};
// swizzles: 2 bits per component, x = 0, y = 1, z = 2, w = 3
constexpr uint32_t Swz(int x, int y, int z, int w) { return (uint32_t)(x | (y << 2) | (z << 4) | (w << 6)); }
constexpr uint32_t kXYZW = Swz(0, 1, 2, 3);
constexpr uint32_t kXXXX = Swz(0, 0, 0, 0), kYYYY = Swz(1, 1, 1, 1), kZZZZ = Swz(2, 2, 2, 2), kWWWW = Swz(3, 3, 3, 3);
constexpr uint32_t kMaskXYZW = 0xF, kMaskXYZ = 0x7, kMaskX = 1, kMaskY = 2, kMaskZ = 4, kMaskW = 8, kMaskXY = 3;
enum SrcMod : uint32_t { M_NONE = 0, M_NEG = 1, M_BIAS = 2, M_BIASNEG = 3, M_SIGN = 4, M_SIGNNEG = 5, M_COMP = 6, M_X2 = 7, M_X2NEG = 8, M_DZ = 9, M_DW = 10, M_ABS = 11, M_ABSNEG = 12, M_NOT = 13 };
enum DstMod : uint32_t { DM_SAT = 1, DM_PP = 2, DM_CENTROID = 4 };

constexpr uint32_t RegBits(Reg t, uint32_t n) { return 0x80000000u | ((t & 7u) << 28) | (((t >> 3) & 3u) << 11) | (n & 0x7FFu); }
constexpr uint32_t Dst(Reg t, uint32_t n, uint32_t mask = kMaskXYZW, uint32_t mod = 0) { return RegBits(t, n) | (mask << 16) | (mod << 20); }
constexpr uint32_t Src(Reg t, uint32_t n, uint32_t swz = kXYZW, uint32_t mod = M_NONE) { return RegBits(t, n) | (swz << 16) | (mod << 24); }
// register type and number of a parameter token
constexpr Reg TypeOf(uint32_t tok) { return (Reg)(((tok >> 28) & 7u) | (((tok >> 11) & 3u) << 3)); }
constexpr uint32_t NumOf(uint32_t tok) { return tok & 0x7FFu; }

constexpr uint32_t kVs20 = 0xFFFE0200, kVs30 = 0xFFFE0300, kPs20 = 0xFFFF0200, kPs20b = 0xFFFF0201, kPs30 = 0xFFFF0300;
// dcl usages (declaration token bits 0-4) and the usage index (bits 16-19)
enum Usage : uint32_t { U_POSITION = 0, U_BLENDWEIGHT = 1, U_BLENDINDICES = 2, U_NORMAL = 3, U_PSIZE = 4, U_TEXCOORD = 5, U_TANGENT = 6, U_BINORMAL = 7, U_TESSFACTOR = 8, U_POSITIONT = 9, U_COLOR = 10, U_FOG = 11, U_DEPTH = 12, U_SAMPLE = 13 };
constexpr uint32_t Usage_(Usage u, uint32_t idx) { return 0x80000000u | (uint32_t)u | (idx << 16); }
enum TexType : uint32_t { TT_2D = 2, TT_CUBE = 3, TT_VOLUME = 4 };

class Builder {
public:
	std::vector<uint32_t> t;
	explicit Builder(uint32_t version) { t.push_back(version); }
	Builder &Ins(uint32_t op, std::initializer_list<uint32_t> params) {
		t.push_back(op | ((uint32_t)params.size() << 24));
		for (uint32_t p : params) t.push_back(p);
		return *this;
	}
	Builder &DclTex(uint32_t texReg) { return Ins(OP_DCL, {0x80000000u, Dst(R_TEXTURE, texReg)}); }  // ps_2_0: dcl t#
	Builder &DclSampler(uint32_t s, TexType type) { return Ins(OP_DCL, {0x80000000u | ((uint32_t)type << 27), Dst(R_SAMPLER, s)}); }
	Builder &DclIn(Usage u, uint32_t usageIdx, uint32_t reg, uint32_t mask = kMaskXYZW) { return Ins(OP_DCL, {Usage_(u, usageIdx), Dst(R_INPUT, reg, mask)}); }  // 3_0: dcl_x v#
	Builder &DclOut(Usage u, uint32_t usageIdx, uint32_t reg, uint32_t mask = kMaskXYZW) { return Ins(OP_DCL, {Usage_(u, usageIdx), Dst(R_OUTPUT, reg, mask)}); }
	Builder &Def(uint32_t reg, float x, float y, float z, float w) {
		uint32_t b[4];
		std::memcpy(&b[0], &x, 4), std::memcpy(&b[1], &y, 4), std::memcpy(&b[2], &z, 4), std::memcpy(&b[3], &w, 4);
		return Ins(OP_DEF, {Dst(R_CONST, reg), b[0], b[1], b[2], b[3]});
	}
	Builder &End() {
		t.push_back(0x0000FFFFu);
		return *this;
	}
};

}  // namespace bridge::d3d9::sm
