// Portalcraft bridge (bridge #50): the walls of the holes mined in movers, the pure geometry (no engine state; src/bridge/moverwalls.cpp draws them,
// movers.cpp builds them when a mover's cuts change).
//
// A mover (door, panel, lift...) is its shape (boxes, MC blocks, its own frame) minus the cuts the Minecraft player mined. Where a cut goes through the shape the
// picture is punched (digrender.cpp), so the hole shows straight through, thickness and all. A hole needs walls like a world dig's (DigWalls): the faces of the
// cut where the REMAINING shape continues beside it, drawn as Minecraft block faces of the material the cell was mined as. The rule, per cut C:
//   * for each of the six faces of C: the boxes of the remaining shape that touch that plane from outside and overlap the face's rectangle are the neighbours;
//     none = the shape ends there = open (the outside of the door), no wall;
//   * the wall lies on that plane moved `g` OUT into the neighbour (the punch removes the mover's picture `g` beyond the cut's piece, so the visible hole is `g`
//     bigger than the cell all round; the wall sits where the picture really ends) and spans the neighbours' faces ∩ the cell's face, grown `g` along the
//     face where it runs to the cell's edge (so the walls of a hole meet at its corners);
//   * through the shape's THICKNESS the wall spans everything the cut removed (a hollow door has two skins and nothing between them: the gap is closed). The
//     thin axis is the in-plane axis along which the removed material is shortest, when it is shorter than 0.95 block; a thick wall takes the neighbours' own extent;
//   * split at the cut's own lattice lines (1 block, from the cut's low corner) so the block texture repeats per block, like a block's face.
#pragma once

#include "skycraft_protocol.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace bridge {

struct MoverWallQuad {
	float p[4][3];       // Source local units, counter-clockwise seen from the hole
	float fu[4], fv[4];  // position inside the block face, 0..1 (u across, v down for side faces)
	float shade;         // Minecraft's fixed face shading
	unsigned char kind;  // the material kind (ChamberMaterial CH_*) the cell was mined as
	unsigned char face;  // 0 side, 1 top (a floor of the hole), 2 bottom (a ceiling)
	unsigned short group;  // the cut it belongs to (its light is taken once per cut)
};
struct MoverWallSet {
	std::vector<MoverWallQuad> quads;
	std::vector<std::array<float, 3>> centres;  // per cut group: its centre, Source local
};
struct WallCut {
	float lo[3], hi[3];  // MC local, blocks
	int kind;
	std::vector<skycraft::proto::ColMoverBox> removed;  // what this cut took out of the shape
};

inline void BuildMoverWalls(const std::vector<skycraft::proto::ColMoverBox> &finalBoxes, const std::vector<WallCut> &cuts, float growUnits, float unit, MoverWallSet &out,
                            size_t maxQuads = 3000) {
	using skycraft::proto::ColMoverBox;
	out.quads.clear();
	out.centres.clear();
	const float eps = 2e-3f, g = growUnits / unit;
	auto mn = [](const ColMoverBox &b, int a) { return a == 0 ? b.minX : a == 1 ? b.minY : b.minZ; };
	auto mx = [](const ColMoverBox &b, int a) { return a == 0 ? b.maxX : a == 1 ? b.maxY : b.maxZ; };
	for (size_t ci = 0; ci < cuts.size(); ++ci) {
		const WallCut &C = cuts[ci];
		if (C.removed.empty()) continue;
		float rLo[3] = {1e30f, 1e30f, 1e30f}, rHi[3] = {-1e30f, -1e30f, -1e30f};
		for (const ColMoverBox &r : C.removed)
			for (int a = 0; a < 3; ++a) rLo[a] = std::fmin(rLo[a], mn(r, a)), rHi[a] = std::fmax(rHi[a], mx(r, a));
		const unsigned short group = (unsigned short)out.centres.size();
		out.centres.push_back({(C.lo[0] + C.hi[0]) * 0.5f * unit, -(C.lo[2] + C.hi[2]) * 0.5f * unit, (C.lo[1] + C.hi[1]) * 0.5f * unit});
		for (int a = 0; a < 3; ++a)
			for (int s = -1; s <= 1; s += 2) {
				const float plane = s < 0 ? C.lo[a] : C.hi[a];
				const int b = a == 0 ? 1 : 0, c = a == 2 ? 1 : 2;  // the in-plane axes
				float bbLo[2] = {1e30f, 1e30f}, bbHi[2] = {-1e30f, -1e30f}, depth = g;
				bool any = false;
				for (const ColMoverBox &B : finalBoxes) {
					if (std::fabs((s < 0 ? mx(B, a) : mn(B, a)) - plane) > eps) continue;
					const float l0 = std::fmax(mn(B, b), C.lo[b]), h0 = std::fmin(mx(B, b), C.hi[b]), l1 = std::fmax(mn(B, c), C.lo[c]), h1 = std::fmin(mx(B, c), C.hi[c]);
					if (h0 - l0 <= eps || h1 - l1 <= eps) continue;
					any = true;
					bbLo[0] = std::fmin(bbLo[0], l0), bbHi[0] = std::fmax(bbHi[0], h0), bbLo[1] = std::fmin(bbLo[1], l1), bbHi[1] = std::fmax(bbHi[1], h1);
					depth = std::fmin(depth, mx(B, a) - mn(B, a));
				}
				if (!any) continue;
				// the in-plane ranges: the thin axis spans what the cut removed, the other the neighbours' extent grown g where it runs to the cell's edge
				const int ax[2] = {b, c};
				float lo[2], hi[2];
				const float ext[2] = {rHi[b] - rLo[b], rHi[c] - rLo[c]};
				const int thin = ext[0] <= ext[1] ? 0 : 1;
				const bool isThin = ext[thin] < 0.95f;
				for (int k = 0; k < 2; ++k) {
					lo[k] = bbLo[k], hi[k] = bbHi[k];
					if (isThin && k == thin) {
						lo[k] = std::fmin(lo[k], rLo[ax[k]]), hi[k] = std::fmax(hi[k], rHi[ax[k]]);
					} else {
						if (lo[k] <= C.lo[ax[k]] + eps) lo[k] -= g;
						if (hi[k] >= C.hi[ax[k]] - eps) hi[k] += g;
					}
				}
				const float at = plane + (float)s * depth;  // moved out into the neighbour
				const int face = a == 1 ? (s < 0 ? 1 : 2) : 0;
				const float shade = a == 1 ? (s < 0 ? 1.0f : 0.5f) : a == 2 ? 0.8f : 0.6f;
				// split at the cut's block lattice
				auto cuts1 = [&](int k, std::vector<float> &v) {
					v.push_back(lo[k]);
					for (float t = std::floor(lo[k] - C.lo[ax[k]] + eps) + 1.0f; C.lo[ax[k]] + t < hi[k] - eps; t += 1.0f)
						if (C.lo[ax[k]] + t > lo[k] + eps) v.push_back(C.lo[ax[k]] + t);
					v.push_back(hi[k]);
				};
				std::vector<float> sb, sc;
				cuts1(0, sb), cuts1(1, sc);
				for (size_t i = 0; i + 1 < sb.size(); ++i)
					for (size_t j = 0; j + 1 < sc.size(); ++j) {
						if (out.quads.size() >= maxQuads) return;
						MoverWallQuad q;
						q.shade = shade, q.kind = (unsigned char)C.kind, q.face = (unsigned char)face, q.group = group;
						// corners in (b, c): (0,0) (1,0) (1,1) (0,1)
						const float cb[4] = {sb[i], sb[i + 1], sb[i + 1], sb[i]}, cc[4] = {sc[j], sc[j], sc[j + 1], sc[j + 1]};
						const float ob = C.lo[b] + std::floor(sb[i] - C.lo[b] + eps), oc = C.lo[c] + std::floor(sc[j] - C.lo[c] + eps);  // the lattice cell's low corner
						for (int k = 0; k < 4; ++k) {
							float m[3];
							m[a] = at, m[b] = cb[k], m[c] = cc[k];
							q.p[k][0] = m[0] * unit, q.p[k][1] = -m[2] * unit, q.p[k][2] = m[1] * unit;  // MC local (x, y, z) -> Source local (x, -z, y)
							const float db = std::min(1.0f, std::max(0.0f, cb[k] - ob)), dc = std::min(1.0f, std::max(0.0f, cc[k] - oc));
							if (a == 1) q.fu[k] = (b == 0 ? db : dc), q.fv[k] = (b == 0 ? dc : db);  // floors and ceilings: u along x, v along z
							else {
								// side faces: u along the horizontal in-plane axis (x on z faces, z on x faces), v down from the top
								const float h = (b == 1) ? dc : db, v = (b == 1) ? db : dc;  // b or c is the vertical axis (1)
								q.fu[k] = h, q.fv[k] = 1.0f - v;
							}
						}
						out.quads.push_back(q);
					}
			}
	}
}

}  // namespace bridge
