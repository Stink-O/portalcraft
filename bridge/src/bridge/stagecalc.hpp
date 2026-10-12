// Portalcraft bridge #47: the pure part of the virtual region (no engine types, so tools/stagecalc_test.cpp can run it over every map).
//
// A campaign chamber has no hidden Minecraft box in its BSP (the maps are Valve's, nothing is added). The portal onto Minecraft
// opens onto the Minecraft stage instead, drawn and walked on in an EMPTY box of Portal 2's world: a box of the +-16384 u cube that
// nothing of the map touches. Which box is worked out from the map itself: the union of everything the BSP holds (the world's model,
// every brush entity's model at its origin, every entity's origin), and the one of the cube's 27 anchor positions that is farthest
// from it.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace stagecalc {

struct Box {
	float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f};
	bool valid() const { return mn[0] <= mx[0]; }
	void add(const float p[3]) {
		for (int i = 0; i < 3; ++i) mn[i] = std::fmin(mn[i], p[i]), mx[i] = std::fmax(mx[i], p[i]);
	}
	void add(const float a[3], const float b[3]) {
		add(a);
		add(b);
	}
};

// Everything the map's BSP file holds, as one box: the world model (lump 14, model 0), each brush entity's model placed at its
// origin (a rotated one is taken at its worst: its largest extent either way), and every entity's origin. Returns false when the
// file is not a readable VBSP.
inline bool BspExtent(const std::string &path, Box &out) {
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f) return false;
	char hdr[8 + 64 * 16];
	int lumps[64][2] = {};
	const bool ok = std::fread(hdr, 1, sizeof hdr, f) == sizeof hdr && !std::memcmp(hdr, "VBSP", 4);
	if (!ok) return std::fclose(f), false;
	for (int i = 0; i < 64; ++i) std::memcpy(lumps[i], hdr + 8 + i * 16, 8);
	auto lump = [&](int i, std::string &s) {
		if (lumps[i][0] <= 0 || lumps[i][1] <= 0 || lumps[i][1] > 64 * 1024 * 1024) return false;
		s.resize((size_t)lumps[i][1]);
		return std::fseek(f, lumps[i][0], SEEK_SET) == 0 && std::fread(&s[0], 1, s.size(), f) == s.size();
	};
	std::string ents, models;
	const bool got = lump(0, ents) && lump(14, models);
	std::fclose(f);
	if (!got || models.size() < 48) return false;
	const size_t nModels = models.size() / 48;
	auto model = [&](size_t m, float mn[3], float mx[3]) {
		float v[6];
		std::memcpy(v, models.data() + m * 48, sizeof v);
		for (int i = 0; i < 3; ++i) mn[i] = v[i], mx[i] = v[3 + i];
	};
	out = Box();
	{
		float mn[3], mx[3];
		model(0, mn, mx);
		out.add(mn, mx);
	}
	// the entity text: { "key" "value" ... }
	size_t i = 0;
	const size_t n = ents.size();
	auto quoted = [&](std::string &s) {
		while (i < n && ents[i] != '"' && ents[i] != '}' && ents[i] != '{') ++i;
		if (i >= n || ents[i] != '"') return false;
		const size_t b = ++i;
		while (i < n && ents[i] != '"') ++i;
		s.assign(ents, b, i - b);
		if (i < n) ++i;
		return true;
	};
	while (i < n) {
		while (i < n && ents[i] != '{') ++i;
		if (i >= n) break;
		++i;
		std::string k, v, origin, modelStr, cls, angles;
		for (;;) {
			while (i < n && (ents[i] == ' ' || ents[i] == '\n' || ents[i] == '\r' || ents[i] == '\t')) ++i;
			if (i >= n || ents[i] == '}') break;
			if (!quoted(k) || !quoted(v)) break;
			if (k == "origin") origin = v;
			else if (k == "model") modelStr = v;
			else if (k == "classname") cls = v;
			else if (k == "angles") angles = v;
		}
		if (i < n) ++i;
		float o[3] = {0, 0, 0};
		const bool hasOrigin = !origin.empty() && std::sscanf(origin.c_str(), "%f %f %f", &o[0], &o[1], &o[2]) == 3;
		if (modelStr.size() > 1 && modelStr[0] == '*') {
			const size_t m = (size_t)std::atoi(modelStr.c_str() + 1);
			if (m > 0 && m < nModels) {
				float mn[3], mx[3];
				model(m, mn, mx);
				float a3[3] = {0, 0, 0};
				const bool rotated = !angles.empty() && std::sscanf(angles.c_str(), "%f %f %f", &a3[0], &a3[1], &a3[2]) == 3 && (a3[0] != 0 || a3[1] != 0 || a3[2] != 0);
				if (!rotated) {
					// at its own origin, where it was compiled
					float a[3], b[3];
					for (int q = 0; q < 3; ++q) a[q] = o[q] + mn[q], b[q] = o[q] + mx[q];
					out.add(a, b);
				} else {
					// turned about its origin: anywhere within the distance of its farthest corner
					float r2 = 0;
					for (int c = 0; c < 8; ++c) {
						float d2 = 0;
						for (int q = 0; q < 3; ++q) {
							const float v = (c >> q & 1) ? mx[q] : mn[q];
							d2 += v * v;
						}
						r2 = std::fmax(r2, d2);
					}
					const float r = std::sqrt(r2);
					float a[3] = {o[0] - r, o[1] - r, o[2] - r}, b[3] = {o[0] + r, o[1] + r, o[2] + r};
					out.add(a, b);
				}
			}
		} else if (hasOrigin && cls != "worldspawn") {
			out.add(o);
		}
	}
	return out.valid();
}

// The distance between two boxes (0 when they overlap).
inline float BoxGap(const float amn[3], const float amx[3], const float bmn[3], const float bmx[3]) {
	float s = 0;
	for (int i = 0; i < 3; ++i) {
		const float d = std::fmax(0.0f, std::fmax(bmn[i] - amx[i], amn[i] - bmx[i]));
		s += d * d;
	}
	return std::sqrt(s);
}

// The virtual region's box: half-extent `half` (Source units), centre among the 27 anchors of the +-limit cube (the cube's centre, face
// centres, edge centres and corners, each kept `half` inside the limit), the one farthest from `world`. gap = its distance from it.
inline bool ChooseRegion(const Box &world, const float half[3], float limit, float centre[3], float &gap) {
	gap = -1;
	for (int sx = -1; sx <= 1; ++sx)
		for (int sy = -1; sy <= 1; ++sy)
			for (int sz = -1; sz <= 1; ++sz) {
				const float c[3] = {sx * (limit - half[0]), sy * (limit - half[1]), sz * (limit - half[2])};
				const float mn[3] = {c[0] - half[0], c[1] - half[1], c[2] - half[2]}, mx[3] = {c[0] + half[0], c[1] + half[1], c[2] + half[2]};
				const float g = BoxGap(mn, mx, world.mn, world.mx);
				if (g > gap + 1e-3f) {
					gap = g;
					std::memcpy(centre, c, sizeof c);
				}
			}
	return gap >= 0;
}

// (bridge #78) The unified world's window onto the terrain: the slab of the +-limit cube under the map (or over it when there is no room
// under it), as wide and deep as the cube, its height from the cube's floor (ceiling) to the map's lowest (highest) point less a margin, at
// most maxH. Its sides reach the cube's sides, so the terrain outside the window maps outside Portal 2's world. False when neither slab is
// minH high.
inline bool ChooseSlab(const Box &world, float limit, float margin, float minH, float maxH, float mn[3], float mx[3], bool &above) {
	const float under = (world.mn[2] - margin) + limit, over = limit - (world.mx[2] + margin);
	if (under < minH && over < minH) return false;
	above = under < minH;
	mn[0] = mn[1] = -limit, mx[0] = mx[1] = limit;
	if (!above) mn[2] = -limit, mx[2] = std::fmin(world.mn[2] - margin, -limit + maxH);
	else mn[2] = world.mx[2] + margin, mx[2] = std::fmin(limit, mn[2] + maxH);
	return true;
}

}  // namespace stagecalc
