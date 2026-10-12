// Portalcraft bridge (bridge #33): harvest-based collision export. See colharvest.hpp.
#include "colharvest.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <strings.h>

namespace colharvest {

namespace {

constexpr int kContentsSolid = 0x1, kContentsWindow = 0x2, kContentsGrate = 0x8, kContentsMoveable = 0x4000, kContentsPlayerClip = 0x10000, kContentsMonster = 0x2000000;
constexpr int kMaskSolid = kContentsSolid | kContentsMoveable | kContentsWindow | kContentsMonster | kContentsGrate;
constexpr int kRegionBlocks = 8;
constexpr int kGrid = kRegionBlocks * 8;  // sub-voxels per region side

struct Plane {
	double n[3];
	double d;  // n . p <= d inside
};

struct Convex {
	std::vector<Plane> planes;
	double mn[3], mx[3];
	int contents = 0;
	bool hasDisp = false;
	bool ghost = false;  // (bridge #46) a brush none of whose sides is drawn (clip, invisible wall, light blocker...): it collides but there is nothing to see or dig
};

// (bridge #46) Is this solid something a player can SEE (so something to mine, and rock to reveal blocks in)? A ghost brush collides but is not drawn.
inline bool VisibleSolid(const Convex &c) { return !c.ghost && (c.contents & (kContentsSolid | kContentsWindow | kContentsGrate | kContentsMoveable | kContentsMonster)) != 0; }

bool ReadFile(const std::string &path, std::vector<uint8_t> &out) {
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	f.seekg(0, std::ios::end);
	const std::streamoff n = f.tellg();
	if (n < 0) return false;
	f.seekg(0);
	out.resize((size_t)n);
	return n == 0 || (bool)f.read((char *)out.data(), n);
}

template <class T> T Get(const std::vector<uint8_t> &d, size_t o) {
	T v{};
	if (o + sizeof(T) <= d.size()) std::memcpy(&v, d.data() + o, sizeof(T));
	return v;
}

// the AABB of a convex polyhedron given by half-spaces: every triple of planes that meets inside the others is a vertex
bool BoundPlanes(Convex &c) {
	const size_t n = c.planes.size();
	c.mn[0] = c.mn[1] = c.mn[2] = 1e30, c.mx[0] = c.mx[1] = c.mx[2] = -1e30;
	bool any = false;
	for (size_t i = 0; i < n; ++i)
		for (size_t j = i + 1; j < n; ++j)
			for (size_t k = j + 1; k < n; ++k) {
				const Plane &a = c.planes[i], &b = c.planes[j], &cc = c.planes[k];
				// n_a x n_b, determinant
				const double cx = b.n[1] * cc.n[2] - b.n[2] * cc.n[1], cy = b.n[2] * cc.n[0] - b.n[0] * cc.n[2], cz = b.n[0] * cc.n[1] - b.n[1] * cc.n[0];
				const double det = a.n[0] * cx + a.n[1] * cy + a.n[2] * cz;
				if (std::fabs(det) < 1e-9) continue;
				// p = (d_a (n_b x n_c) + d_b (n_c x n_a) + d_c (n_a x n_b)) / det
				const double ax = cc.n[1] * a.n[2] - cc.n[2] * a.n[1], ay = cc.n[2] * a.n[0] - cc.n[0] * a.n[2], az = cc.n[0] * a.n[1] - cc.n[1] * a.n[0];
				const double bx = a.n[1] * b.n[2] - a.n[2] * b.n[1], by = a.n[2] * b.n[0] - a.n[0] * b.n[2], bz = a.n[0] * b.n[1] - a.n[1] * b.n[0];
				const double p[3] = {(a.d * cx + b.d * ax + cc.d * bx) / det, (a.d * cy + b.d * ay + cc.d * by) / det, (a.d * cz + b.d * az + cc.d * bz) / det};
				bool ok = true;
				for (size_t m = 0; m < n && ok; ++m) ok = c.planes[m].n[0] * p[0] + c.planes[m].n[1] * p[1] + c.planes[m].n[2] * p[2] - c.planes[m].d <= 0.05;
				if (!ok) continue;
				any = true;
				for (int t = 0; t < 3; ++t) c.mn[t] = std::min(c.mn[t], p[t]), c.mx[t] = std::max(c.mx[t], p[t]);
			}
	return any;
}

// angles (pitch yaw roll, degrees) -> the rotation matrix whose columns are Source's forward, left, up
void AngleMatrix(const double a[3], double R[3][3]) {
	const double d2r = M_PI / 180.0;
	const double sp = std::sin(a[0] * d2r), cp = std::cos(a[0] * d2r), sy = std::sin(a[1] * d2r), cy = std::cos(a[1] * d2r), sr = std::sin(a[2] * d2r), cr = std::cos(a[2] * d2r);
	const double f[3] = {cp * cy, cp * sy, -sp};
	const double r[3] = {-sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp};
	const double u[3] = {cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp};
	for (int i = 0; i < 3; ++i) R[i][0] = f[i], R[i][1] = -r[i], R[i][2] = u[i];
}

}  // namespace

// ---- VPK -----------------------------------------------------------------------------------------------------------
bool Vpk::Mount(const std::string &dirPath) {
	std::vector<uint8_t> d;
	if (!ReadFile(dirPath, d) || d.size() < 12) return false;
	if (Get<uint32_t>(d, 0) != 0x55aa1234u) return false;
	const uint32_t ver = Get<uint32_t>(d, 4), treeSize = Get<uint32_t>(d, 8);
	if (ver != 1 && ver != 2) return false;
	const uint32_t hdr = ver == 2 ? 28 : 12;
	m_dirPath = dirPath;
	m_base = dirPath.size() > 8 && dirPath.compare(dirPath.size() - 8, 8, "_dir.vpk") == 0 ? dirPath.substr(0, dirPath.size() - 8) : dirPath;
	m_dataStart = hdr + treeSize;
	size_t p = hdr;
	const size_t end = std::min<size_t>(d.size(), hdr + (size_t)treeSize);
	auto str = [&](std::string &s) {
		s.clear();
		while (p < end && d[p]) s.push_back((char)d[p++]);
		++p;
		return p <= end;
	};
	std::string ext, path, name;
	while (p < end) {
		if (!str(ext) || ext.empty()) break;
		for (;;) {
			if (!str(path) || path.empty()) break;
			for (;;) {
				if (!str(name) || name.empty()) break;
				if (p + 18 > end) return true;
				Entry e;
				const uint16_t preload = Get<uint16_t>(d, p + 4);
				e.archive = Get<uint16_t>(d, p + 6);
				e.offset = Get<uint32_t>(d, p + 8);
				e.length = Get<uint32_t>(d, p + 12);
				p += 18;
				if (preload && p + preload <= end) e.preload.assign(d.begin() + p, d.begin() + p + preload);
				p += preload;
				std::string full = (path == " " ? "" : path + "/") + name + (ext == " " ? "" : "." + ext);
				for (char &c : full) c = (char)std::tolower((unsigned char)c);
				m_entries.emplace(std::move(full), std::move(e));
			}
		}
	}
	return true;
}

bool Vpk::Read(const std::string &path, std::vector<uint8_t> &out) const {
	auto it = m_entries.find(path);
	if (it == m_entries.end()) return false;
	const Entry &e = it->second;
	out = e.preload;
	if (!e.length) return true;
	std::string file = m_dirPath;
	uint64_t off = e.offset;
	if (e.archive != 0x7fff) {
		char buf[32];
		std::snprintf(buf, sizeof buf, "_%03u.vpk", e.archive);
		file = m_base + buf;
	} else {
		off += m_dataStart;
	}
	std::ifstream f(file, std::ios::binary);
	if (!f) return false;
	f.seekg((std::streamoff)off);
	const size_t at = out.size();
	out.resize(at + e.length);
	return (bool)f.read((char *)out.data() + at, e.length);
}

// ---- the world ----------------------------------------------------------------------------------------------------
struct World::Impl {
	// BSP
	std::vector<Plane> planes;      // raw BSP planes: n . p = dist
	struct Node {
		int plane, child[2];
	};
	struct Leaf {
		int contents;
	};
	std::vector<Node> nodes;
	std::vector<Leaf> leafs;
	int worldHead = 0;
	std::vector<Convex> brushes;  // world brushes (model 0)
	// props
	struct PropModel {
		std::vector<Convex> ledges;  // model space, Source inches
		bool loaded = false;
	};
	struct Prop {
		std::vector<Convex> ledges;  // world space
		double mn[3], mx[3];
	};
	std::vector<Prop> props;
	// displacements: the solid between a brush's flat face and the displaced surface above it
	struct Disp {
		std::vector<double> tris;  // 9 doubles per triangle
		std::vector<Plane> planes; // the brush's planes without the displaced face
		double n[3];
		double mn[3], mx[3];
	};
	std::vector<Disp> disps;
	double wmn[3] = {1e30, 1e30, 1e30}, wmx[3] = {-1e30, -1e30, -1e30};

	bool ParseBsp(const std::vector<uint8_t> &d, World &w, std::string &err, std::string &log);
	bool LoadPhy(const std::vector<uint8_t> &data, PropModel &m);
};

World::World() : m_impl(new Impl) {}
World::~World() {}

namespace {

// Walk the BSP tree from a head node and collect the brushes of the leaves it reaches.
void CollectBrushes(const World::Impl &im, int node, const std::vector<uint16_t> &leafBrushes, const std::vector<uint16_t> &leafFirst, const std::vector<uint16_t> &leafCount,
                    std::set<int> &out) {
	std::vector<int> st{node};
	while (!st.empty()) {
		const int n = st.back();
		st.pop_back();
		if (n < 0) {
			const int l = -1 - n;
			if (l < 0 || l >= (int)leafFirst.size()) continue;
			for (int k = 0; k < leafCount[l]; ++k)
				if ((size_t)leafFirst[l] + k < leafBrushes.size()) out.insert(leafBrushes[leafFirst[l] + k]);
		} else if ((size_t)n < im.nodes.size()) {
			st.push_back(im.nodes[n].child[0]);
			st.push_back(im.nodes[n].child[1]);
		}
	}
}

}  // namespace

bool World::Impl::ParseBsp(const std::vector<uint8_t> &d, World &w, std::string &err, std::string &log) {
	if (d.size() < 8 + 64 * 16 || std::memcmp(d.data(), "VBSP", 4)) return err = "not a VBSP file", false;
	struct Lump {
		int32_t ofs, len, ver, fcc;
	};
	Lump lump[64];
	std::memcpy(lump, d.data() + 8, sizeof lump);
	for (int i = 0; i < 64; ++i)
		if (lump[i].len && lump[i].fcc) return err = "an LZMA-compressed lump (" + std::to_string(i) + ") is not supported", false;
	auto sub = [&](int i) {
		std::vector<uint8_t> v;
		if (lump[i].len > 0 && lump[i].ofs > 0 && (size_t)lump[i].ofs + lump[i].len <= d.size()) v.assign(d.begin() + lump[i].ofs, d.begin() + lump[i].ofs + lump[i].len);
		return v;
	};
	const auto lPlanes = sub(1), lNodes = sub(5), lLeafs = sub(10), lModels = sub(14), lLeafBrushes = sub(17), lBrushes = sub(18), lSides = sub(19), lDisp = sub(26), lEnts = sub(0),
	           lGame = sub(35), lTexInfo = sub(6), lTexData = sub(2), lTexStrTable = sub(44), lTexStrData = sub(43);
	// (bridge #46) is a brush side drawn? Its texinfo flags say (nodraw, skip, hint, trigger) and tool textures are never drawn; the sky's tools are the map's shell (rock)
	auto sideInvisible = [&](int ti) {
		if (ti < 0 || (size_t)(ti + 1) * 72 > lTexInfo.size()) return false;
		int flags = 0, td = 0;
		std::memcpy(&flags, lTexInfo.data() + (size_t)ti * 72 + 64, 4);
		std::memcpy(&td, lTexInfo.data() + (size_t)ti * 72 + 68, 4);
		if (flags & 0x6) return false;  // sky
		if (flags & (0x80 | 0x200 | 0x100 | 0x40)) return true;
		if (td < 0 || (size_t)(td + 1) * 32 > lTexData.size()) return false;
		int nameId = 0;
		std::memcpy(&nameId, lTexData.data() + (size_t)td * 32 + 12, 4);
		int strOfs = 0;
		if (nameId < 0 || (size_t)(nameId + 1) * 4 > lTexStrTable.size()) return false;
		std::memcpy(&strOfs, lTexStrTable.data() + (size_t)nameId * 4, 4);
		if (strOfs < 0 || (size_t)strOfs + 6 > lTexStrData.size()) return false;
		const char *nm = (const char *)lTexStrData.data() + strOfs;
		if (strncasecmp(nm, "tools/", 6) != 0) return false;
		return strncasecmp(nm, "tools/toolsskybox", 17) != 0 && strncasecmp(nm, "tools/toolsblack", 16) != 0;
	};
	w.m_entities.assign(lEnts.begin(), lEnts.end());
	while (!w.m_entities.empty() && w.m_entities.back() == 0) w.m_entities.pop_back();
	if (lPlanes.empty() || lBrushes.empty() || lSides.empty() || lModels.size() < 48) return err = "the BSP has no brushes", false;

	planes.resize(lPlanes.size() / 20);
	for (size_t i = 0; i < planes.size(); ++i) {
		const float *f = (const float *)(lPlanes.data() + i * 20);
		planes[i] = Plane{{f[0], f[1], f[2]}, f[3]};
	}
	nodes.resize(lNodes.size() / 32);
	for (size_t i = 0; i < nodes.size(); ++i) {
		const int32_t *v = (const int32_t *)(lNodes.data() + i * 32);
		nodes[i] = Node{v[0], {v[1], v[2]}};
	}
	const size_t leafSize = lump[10].ver == 0 ? 56 : 32;
	leafs.resize(lLeafs.size() / leafSize);
	std::vector<uint16_t> leafFirst(leafs.size()), leafCount(leafs.size());
	for (size_t i = 0; i < leafs.size(); ++i) {
		const uint8_t *p = lLeafs.data() + i * leafSize;
		leafs[i].contents = *(const int32_t *)p;
		std::memcpy(&leafFirst[i], p + 24, 2);
		std::memcpy(&leafCount[i], p + 26, 2);
	}
	std::vector<uint16_t> leafBrushes(lLeafBrushes.size() / 2);
	std::memcpy(leafBrushes.data(), lLeafBrushes.data(), leafBrushes.size() * 2);
	w.m_nModels = lModels.size() / 48;
	worldHead = *(const int32_t *)(lModels.data() + 36);

	std::set<int> worldSet;
	CollectBrushes(*this, worldHead, leafBrushes, leafFirst, leafCount, worldSet);

	// displacements: counted (their surfaces are not voxelised in this version; see the README / report)
	w.m_dispCount = lDisp.size() / 176;

	const size_t nBrushes = lBrushes.size() / 12, nSides = lSides.size() / 8;
	int degenerate = 0, kept = 0;
	for (int bi : worldSet) {
		if ((size_t)bi >= nBrushes) continue;
		const int32_t *b = (const int32_t *)(lBrushes.data() + (size_t)bi * 12);
		Convex c;
		c.contents = b[2];
		if (!(c.contents & (kMaskSolid | kContentsPlayerClip))) continue;  // water, slime, areaportals, ...
		int drawnSides = 0, realSides = 0;
		for (int s = 0; s < b[1]; ++s) {
			const size_t si = (size_t)b[0] + s;
			if (si >= nSides) break;
			const uint8_t *sp = lSides.data() + si * 8;
			uint16_t plane;
			int16_t texinfo, dispinfo, bevel;
			std::memcpy(&plane, sp, 2);
			std::memcpy(&texinfo, sp + 2, 2);
			std::memcpy(&dispinfo, sp + 4, 2);
			std::memcpy(&bevel, sp + 6, 2);
			if (plane >= planes.size()) continue;
			c.planes.push_back(planes[plane]);
			if (dispinfo >= 0) c.hasDisp = true;
			if ((bevel & 0xFF) == 0) {  // (the high byte is the "thin" flag, not a bevel)
				++realSides;
				if (!sideInvisible(texinfo)) ++drawnSides;
			}
		}
		c.ghost = realSides > 0 && drawnSides == 0;
		if (c.planes.size() < 4 || !BoundPlanes(c)) {
			++degenerate;
			continue;
		}
		for (int t = 0; t < 3; ++t) wmn[t] = std::min(wmn[t], c.mn[t]), wmx[t] = std::max(wmx[t], c.mx[t]);
		brushes.push_back(std::move(c));
		w.m_worldBrushes.push_back((int)brushes.size() - 1);
		++kept;
	}
	// displacements: rebuild each displaced surface (base face corners + vec * dist) and take the brush under it
	{
		const auto lFaces = sub(7), lVerts = sub(3), lEdges = sub(12), lSurf = sub(13), lDV = sub(33);
		int built = 0;
		for (size_t k = 0; k < w.m_dispCount; ++k) {
			const uint8_t *e = lDisp.data() + k * 176;
			float sp[3];
			std::memcpy(sp, e, 12);
			int32_t vstart, power;
			uint16_t mapFace;
			std::memcpy(&vstart, e + 12, 4), std::memcpy(&power, e + 20, 4), std::memcpy(&mapFace, e + 36, 2);
			if (power < 2 || power > 4 || (size_t)(mapFace + 1) * 56 > lFaces.size()) continue;
			const uint8_t *f = lFaces.data() + (size_t)mapFace * 56;
			int32_t firstEdge;
			int16_t numEdges;
			std::memcpy(&firstEdge, f + 4, 4), std::memcpy(&numEdges, f + 8, 2);
			if (numEdges != 4) continue;
			double corner[4][3];
			bool ok = true;
			for (int j = 0; j < 4 && ok; ++j) {
				const size_t si = (size_t)firstEdge + j;
				if ((si + 1) * 4 > lSurf.size()) { ok = false; break; }
				int32_t se;
				std::memcpy(&se, lSurf.data() + si * 4, 4);
				const size_t ei = (size_t)std::abs(se);
				if ((ei + 1) * 4 > lEdges.size()) { ok = false; break; }
				uint16_t v[2];
				std::memcpy(v, lEdges.data() + ei * 4, 4);
				const size_t vi = se >= 0 ? v[0] : v[1];
				if ((vi + 1) * 12 > lVerts.size()) { ok = false; break; }
				float p[3];
				std::memcpy(p, lVerts.data() + vi * 12, 12);
				for (int t = 0; t < 3; ++t) corner[j][t] = p[t];
			}
			if (!ok) continue;
			int start = 0;
			double best = 1e30;
			for (int j = 0; j < 4; ++j) {
				const double dd = (corner[j][0] - sp[0]) * (corner[j][0] - sp[0]) + (corner[j][1] - sp[1]) * (corner[j][1] - sp[1]) + (corner[j][2] - sp[2]) * (corner[j][2] - sp[2]);
				if (dd < best) best = dd, start = j;
			}
			double P[4][3];
			for (int j = 0; j < 4; ++j)
				for (int t = 0; t < 3; ++t) P[j][t] = corner[(start + j) % 4][t];
			const int N = (1 << power) + 1;
			if ((size_t)(vstart + N * N) * 20 > lDV.size()) continue;
			std::vector<double> pos((size_t)N * N * 3);
			for (int i = 0; i < N; ++i) {
				double a[3], b[3];
				for (int t = 0; t < 3; ++t) {
					a[t] = P[0][t] + (P[3][t] - P[0][t]) * i / (N - 1);
					b[t] = P[1][t] + (P[2][t] - P[1][t]) * i / (N - 1);
				}
				for (int j = 0; j < N; ++j) {
					const float *dv = (const float *)(lDV.data() + (size_t)(vstart + (j * N + i)) * 20);
					for (int t = 0; t < 3; ++t) pos[((size_t)i * N + j) * 3 + t] = a[t] + (b[t] - a[t]) * j / (N - 1) + dv[t] * dv[3];
				}
			}
			Disp dsp;
			for (int i = 0; i < N - 1; ++i)
				for (int j = 0; j < N - 1; ++j) {
					const size_t i00 = (size_t)i * N + j, i01 = i00 + 1, i10 = i00 + N, i11 = i10 + 1;
					const std::array<size_t, 3> t0 = ((i + j) & 1) == 0 ? std::array<size_t, 3>{i00, i10, i11} : std::array<size_t, 3>{i00, i10, i01};
					const std::array<size_t, 3> t1 = ((i + j) & 1) == 0 ? std::array<size_t, 3>{i00, i11, i01} : std::array<size_t, 3>{i10, i11, i01};
					for (const auto &tt : {t0, t1})
						for (size_t q : tt)
							for (int t = 0; t < 3; ++t) dsp.tris.push_back(pos[q * 3 + t]);
				}
			// the brush just under the face
			double n[3] = {0, 0, 0};
			{
				const double ux = P[1][0] - P[0][0], uy = P[1][1] - P[0][1], uz = P[1][2] - P[0][2], vx = P[3][0] - P[0][0], vy = P[3][1] - P[0][1], vz = P[3][2] - P[0][2];
				n[0] = uy * vz - uz * vy, n[1] = uz * vx - ux * vz, n[2] = ux * vy - uy * vx;
				const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				if (len < 1e-9) continue;
				for (double &x : n) x /= len;
			}
			double cen[3];
			for (int t = 0; t < 3; ++t) cen[t] = (P[0][t] + P[1][t] + P[2][t] + P[3][t]) / 4;
			const Convex *under = nullptr;
			for (int flip = 0; flip < 2 && !under; ++flip) {
				if (flip) for (double &x : n) x = -x;
				for (const Convex &b : brushes) {
					const double q[3] = {cen[0] - n[0] * 0.5, cen[1] - n[1] * 0.5, cen[2] - n[2] * 0.5};
					if (q[0] < b.mn[0] || q[0] > b.mx[0] || q[1] < b.mn[1] || q[1] > b.mx[1] || q[2] < b.mn[2] || q[2] > b.mx[2]) continue;
					bool in = (b.contents & kMaskSolid) != 0;
					for (const Plane &pl : b.planes)
						if (in && pl.n[0] * q[0] + pl.n[1] * q[1] + pl.n[2] * q[2] > pl.d + 0.01) in = false;
					if (in) { under = &b; break; }
				}
			}
			const double dFace = n[0] * cen[0] + n[1] * cen[1] + n[2] * cen[2];
			if (under) for (const Plane &pl : under->planes) {
				const double dot = pl.n[0] * n[0] + pl.n[1] * n[1] + pl.n[2] * n[2];
				if (dot > 0.999 && std::fabs(pl.d - dFace) < 0.5) continue;  // the displaced face itself
				dsp.planes.push_back(pl);
			}
			for (int t = 0; t < 3; ++t) dsp.n[t] = n[t];
			dsp.mn[0] = dsp.mn[1] = dsp.mn[2] = 1e30, dsp.mx[0] = dsp.mx[1] = dsp.mx[2] = -1e30;
			for (size_t q = 0; q + 2 < dsp.tris.size(); q += 3)
				for (int t = 0; t < 3; ++t) dsp.mn[t] = std::min(dsp.mn[t], dsp.tris[q + t]), dsp.mx[t] = std::max(dsp.mx[t], dsp.tris[q + t]);
			// the solid reaches down to the brush's own bounds
			if (under) for (int t = 0; t < 3; ++t) dsp.mn[t] = std::min(dsp.mn[t], under->mn[t]), dsp.mx[t] = std::max(dsp.mx[t], under->mx[t]);
			disps.push_back(std::move(dsp));
			++built;
		}
		char buf[96];
		std::snprintf(buf, sizeof buf, "bsp: %d of %zu displacements built\n", built, w.m_dispCount);
		log += buf;
	}
	{
		char buf[256];
		std::snprintf(buf, sizeof buf, "bsp: %zu planes, %zu nodes, %zu leafs, %zu models, %d world brushes kept (%d degenerate), %zu displacements\n", planes.size(),
		              nodes.size(), leafs.size(), w.m_nModels, kept, degenerate, w.m_dispCount);
		log += buf;
	}
	return true;
}

bool World::Impl::LoadPhy(const std::vector<uint8_t> &data, PropModel &m) {
	if (data.size() < 16) return false;
	const int32_t headerSize = Get<int32_t>(data, 0), solidCount = Get<int32_t>(data, 8);
	size_t off = (size_t)std::max(headerSize, 16);
	for (int s = 0; s < std::min(std::max(solidCount, 0), 256); ++s) {
		if (off + 4 > data.size()) break;
		const int32_t size = Get<int32_t>(data, off);
		const size_t start = off + 4;
		const size_t end = std::min(data.size(), start + (size_t)std::max(size, 0));
		const bool vphy = start + 4 <= data.size() && !std::memcmp(data.data() + start, "VPHY", 4);
		const size_t surf = vphy ? start + 28 : start;
		// IVP_Compact_Surface: ... offset_ledgetree_root at +32
		const int32_t rootOff = Get<int32_t>(data, surf + 32);
		const int64_t root = (int64_t)surf + rootOff;
		if (s == 0 || true) {
			// only the first solid is the prop's collision model for static props (the rest are for ragdolls and bone followers);
			// the equivalence tool measures whether that matters (it does not for the props these maps use)
		}
		if (root > (int64_t)surf && (size_t)root < end) {
			std::vector<size_t> stack{(size_t)root};
			int guard = 0;
			while (!stack.empty() && ++guard < 200000) {
				const size_t node = stack.back();
				stack.pop_back();
				if (node + 28 > end) break;
				const int32_t right = Get<int32_t>(data, node), ledgeOff = Get<int32_t>(data, node + 4);
				if (right == 0) {
					if (!ledgeOff) continue;
					const size_t ledge = (size_t)((int64_t)node + ledgeOff);
					const int32_t pointOff = Get<int32_t>(data, ledge);
					const int16_t nTris = Get<int16_t>(data, ledge + 12);
					const size_t points = (size_t)((int64_t)ledge + pointOff);
					if (nTris < 1 || ledge + 16 + (size_t)nTris * 16 > data.size()) continue;
					auto pt = [&](uint32_t i, double v[3]) {
						const size_t o = points + (size_t)i * 16;
						const float ix = Get<float>(data, o), iy = Get<float>(data, o + 4), iz = Get<float>(data, o + 8);
						// IVP metres to Source inches: (x, y, z)_source = (ix, iz, -iy) / 0.0254
						v[0] = ix / 0.0254, v[1] = iz / 0.0254, v[2] = -iy / 0.0254;
					};
					struct Tri {
						double a[3], b[3], c[3];
					};
					std::vector<Tri> tris;
					double ctr[3] = {0, 0, 0};
					int nc = 0;
					bool bad = false;
					for (int t = 0; t < nTris; ++t) {
						const size_t tri = ledge + 16 + (size_t)t * 16;
						uint32_t idx[3];
						for (int e = 0; e < 3; ++e) idx[e] = Get<uint32_t>(data, tri + 4 + e * 4) & 0xffff;
						Tri tr;
						pt(idx[0], tr.a), pt(idx[1], tr.b), pt(idx[2], tr.c);
						for (int k = 0; k < 3; ++k)
							if (!std::isfinite(tr.a[k]) || !std::isfinite(tr.b[k]) || !std::isfinite(tr.c[k]) || std::fabs(tr.a[k]) > 40000) bad = true;
						for (int k = 0; k < 3; ++k) ctr[k] += tr.a[k] + tr.b[k] + tr.c[k];
						nc += 3;
						tris.push_back(tr);
					}
					if (bad || tris.empty()) continue;
					for (int k = 0; k < 3; ++k) ctr[k] /= nc;
					Convex c;
					c.contents = kContentsSolid;
					for (const Tri &tr : tris) {
						double u[3] = {tr.b[0] - tr.a[0], tr.b[1] - tr.a[1], tr.b[2] - tr.a[2]}, v[3] = {tr.c[0] - tr.a[0], tr.c[1] - tr.a[1], tr.c[2] - tr.a[2]};
						double n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
						const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
						if (len < 1e-6) continue;
						for (double &x : n) x /= len;
						double dd = n[0] * tr.a[0] + n[1] * tr.a[1] + n[2] * tr.a[2];
						if (n[0] * ctr[0] + n[1] * ctr[1] + n[2] * ctr[2] - dd > 0) {
							for (double &x : n) x = -x;
							dd = -dd;
						}
						bool dup = false;
						for (const Plane &q : c.planes)
							if (std::fabs(q.n[0] - n[0]) < 1e-4 && std::fabs(q.n[1] - n[1]) < 1e-4 && std::fabs(q.n[2] - n[2]) < 1e-4 && std::fabs(q.d - dd) < 1e-2) { dup = true; break; }
						if (!dup) c.planes.push_back(Plane{{n[0], n[1], n[2]}, dd});
					}
					// bounds from the triangle points
					c.mn[0] = c.mn[1] = c.mn[2] = 1e30, c.mx[0] = c.mx[1] = c.mx[2] = -1e30;
					for (const Tri &tr : tris)
						for (const double *p : {tr.a, tr.b, tr.c})
							for (int k = 0; k < 3; ++k) c.mn[k] = std::min(c.mn[k], p[k]), c.mx[k] = std::max(c.mx[k], p[k]);
					if (c.planes.size() >= 4) m.ledges.push_back(std::move(c));
				} else {
					stack.push_back(node + 28);
					const int64_t r = (int64_t)node + right;
					if (r > 0 && (size_t)r < end) stack.push_back((size_t)r);
				}
			}
		}
		off = end;
		break;  // the first solid only (static props collide with solids[0])
	}
	m.loaded = !m.ledges.empty();
	return m.loaded;
}

bool World::Load(const std::string &bspPath, const std::vector<std::string> &gameDirs, std::string &err, std::string *logOut) {
	std::string log;
	std::vector<uint8_t> d;
	if (!ReadFile(bspPath, d)) return err = "cannot read " + bspPath, false;
	m_impl.reset(new Impl);
	m_worldBrushes.clear(), m_props.clear();
	if (!m_impl->ParseBsp(d, *this, err, log)) return false;

	// static props: re-parse the placement (ParseBsp collects them locally; do it again here with the file system)
	struct Lump {
		int32_t ofs, len, ver, fcc;
	};
	Lump lump[64];
	std::memcpy(lump, d.data() + 8, sizeof lump);
	std::vector<std::string> dict;
	struct SP {
		double o[3], a[3];
		int model, solid;
	};
	std::vector<SP> sps;
	if (lump[35].len > 4 && (size_t)lump[35].ofs + lump[35].len <= d.size()) {
		std::vector<uint8_t> g(d.begin() + lump[35].ofs, d.begin() + lump[35].ofs + lump[35].len);
		const int32_t n = Get<int32_t>(g, 0);
		for (int i = 0; i < n && 4 + (size_t)(i + 1) * 16 <= g.size(); ++i) {
			const uint8_t *e = g.data() + 4 + i * 16;
			int32_t id, fofs, flen;
			std::memcpy(&id, e, 4), std::memcpy(&fofs, e + 8, 4), std::memcpy(&flen, e + 12, 4);
			if (id != 0x73707270 || fofs <= 0 || flen <= 0 || (size_t)fofs + flen > d.size()) continue;
			const uint8_t *base = d.data() + fofs;
			size_t p = 0;
			auto i32 = [&](size_t o) { int32_t v = 0; if (o + 4 <= (size_t)flen) std::memcpy(&v, base + o, 4); return v; };
			const int32_t dc = i32(p);
			p += 4;
			for (int k = 0; k < dc && p + 128 <= (size_t)flen; ++k, p += 128) dict.emplace_back((const char *)base + p, strnlen((const char *)base + p, 128));
			const int32_t lc = i32(p);
			p += 4 + (size_t)lc * 2;
			const int32_t pc = i32(p);
			p += 4;
			if (pc <= 0) continue;
			const size_t esz = (size_t)(flen - p) / pc;
			if (esz < 56) continue;
			for (int k = 0; k < pc; ++k) {
				const uint8_t *pe = base + p + (size_t)k * esz;
				float f[6];
				std::memcpy(f, pe, 24);
				uint16_t type;
				std::memcpy(&type, pe + 24, 2);
				SP sp;
				for (int t = 0; t < 3; ++t) sp.o[t] = f[t], sp.a[t] = f[3 + t];
				sp.model = type, sp.solid = pe[30];
				sps.push_back(sp);
			}
		}
	}
	// models -> .phy
	std::vector<Vpk> vpks;
	for (const std::string &dir : gameDirs) {
		Vpk v;
		if (v.Mount(dir + "/pak01_dir.vpk")) vpks.push_back(std::move(v));
	}
	std::map<int, Impl::PropModel> models;
	size_t notSolid = 0, otherSolid = 0, missing = 0, placed = 0;
	std::set<std::string> missingNames;
	for (const SP &sp : sps) {
		if (sp.solid == 0) { ++notSolid; continue; }
		if (sp.solid != 6) { ++otherSolid; continue; }  // bbox/obb static props: no hull to read (none in the maps so far)
		if (sp.model < 0 || sp.model >= (int)dict.size()) continue;
		auto it = models.find(sp.model);
		if (it == models.end()) {
			Impl::PropModel pm;
			std::string name = dict[sp.model];
			for (char &c : name) c = (char)std::tolower((unsigned char)c);
			if (name.size() > 4 && name.compare(name.size() - 4, 4, ".mdl") == 0) name.resize(name.size() - 4);
			const std::string phy = name + ".phy";
			std::vector<uint8_t> data;
			bool got = false;
			for (const Vpk &v : vpks)
				if (v.Read(phy, data)) { got = true; break; }
			if (!got)
				for (const std::string &dir : gameDirs)
					if (ReadFile(dir + "/" + phy, data)) { got = true; break; }
			if (!got || !m_impl->LoadPhy(data, pm)) missingNames.insert(phy);
			it = models.emplace(sp.model, std::move(pm)).first;
		}
		const Impl::PropModel &pm = it->second;
		if (!pm.loaded) { ++missing; continue; }
		double R[3][3];
		AngleMatrix(sp.a, R);
		Impl::Prop prop;
		prop.mn[0] = prop.mn[1] = prop.mn[2] = 1e30, prop.mx[0] = prop.mx[1] = prop.mx[2] = -1e30;
		for (const Convex &l : pm.ledges) {
			Convex w;
			w.contents = kContentsSolid;
			for (const Plane &q : l.planes) {
				Plane nq;
				for (int i = 0; i < 3; ++i) nq.n[i] = R[i][0] * q.n[0] + R[i][1] * q.n[1] + R[i][2] * q.n[2];
				nq.d = q.d + nq.n[0] * sp.o[0] + nq.n[1] * sp.o[1] + nq.n[2] * sp.o[2];
				w.planes.push_back(nq);
			}
			// world bounds: the 8 corners of the local box
			w.mn[0] = w.mn[1] = w.mn[2] = 1e30, w.mx[0] = w.mx[1] = w.mx[2] = -1e30;
			for (int k = 0; k < 8; ++k) {
				const double lp[3] = {(k & 1) ? l.mx[0] : l.mn[0], (k & 2) ? l.mx[1] : l.mn[1], (k & 4) ? l.mx[2] : l.mn[2]};
				for (int i = 0; i < 3; ++i) {
					const double v = sp.o[i] + R[i][0] * lp[0] + R[i][1] * lp[1] + R[i][2] * lp[2];
					w.mn[i] = std::min(w.mn[i], v), w.mx[i] = std::max(w.mx[i], v);
				}
			}
			for (int i = 0; i < 3; ++i) prop.mn[i] = std::min(prop.mn[i], w.mn[i]), prop.mx[i] = std::max(prop.mx[i], w.mx[i]);
			prop.ledges.push_back(std::move(w));
		}
		m_impl->props.push_back(std::move(prop));
		m_props.push_back((int)m_impl->props.size() - 1);
		++placed;
	}
	m_propsMissing = missing;
	{
		char buf[256];
		std::snprintf(buf, sizeof buf, "props: %zu solid hulls placed (%zu not solid, %zu other solid types, %zu without a .phy), %zu VPK dirs mounted\n", placed, notSolid, otherSolid, missing, vpks.size());
		log += buf;
		for (const std::string &n : missingNames) log += "props: no collision model for " + n + "\n";
	}
	m_loaded = true;
	if (logOut) *logOut += log;
	return true;
}

void World::Bounds(double mn[3], double mx[3]) const {
	for (int i = 0; i < 3; ++i) mn[i] = m_impl->wmn[i], mx[i] = m_impl->wmx[i];
}

namespace {
inline bool Inside(const std::vector<Plane> &pl, double x, double y, double z) {
	for (const Plane &q : pl)
		if (q.n[0] * x + q.n[1] * y + q.n[2] * z > q.d) return false;
	return true;
}
inline bool InsideM(const std::vector<Plane> &pl, double x, double y, double z, double m) {
	for (const Plane &q : pl)
		if (q.n[0] * x + q.n[1] * y + q.n[2] * z > q.d + m) return false;
	return true;
}
// Triangle against an axis-aligned box (centre c, half extents h): the separating-axis test.
inline bool TriBox(const double *a, const double *b, const double *cc, const double c[3], double h) {
	const double v[3][3] = {{a[0] - c[0], a[1] - c[1], a[2] - c[2]}, {b[0] - c[0], b[1] - c[1], b[2] - c[2]}, {cc[0] - c[0], cc[1] - c[1], cc[2] - c[2]}};
	for (int ax = 0; ax < 3; ++ax) {  // the box's own axes
		const double mn = std::min({v[0][ax], v[1][ax], v[2][ax]}), mx = std::max({v[0][ax], v[1][ax], v[2][ax]});
		if (mn > h || mx < -h) return false;
	}
	const double e[3][3] = {{v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2]}, {v[2][0] - v[1][0], v[2][1] - v[1][1], v[2][2] - v[1][2]}, {v[0][0] - v[2][0], v[0][1] - v[2][1], v[0][2] - v[2][2]}};
	const double n[3] = {e[0][1] * e[1][2] - e[0][2] * e[1][1], e[0][2] * e[1][0] - e[0][0] * e[1][2], e[0][0] * e[1][1] - e[0][1] * e[1][0]};
	{  // the triangle's plane
		const double d = n[0] * v[0][0] + n[1] * v[0][1] + n[2] * v[0][2];
		const double r = h * (std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]));
		if (std::fabs(d) > r) return false;
	}
	for (int i = 0; i < 3; ++i)  // box axis x edge axes
		for (int ax = 0; ax < 3; ++ax) {
			double u[3] = {0, 0, 0};
			u[ax] = 1;
			const double s[3] = {u[1] * e[i][2] - u[2] * e[i][1], u[2] * e[i][0] - u[0] * e[i][2], u[0] * e[i][1] - u[1] * e[i][0]};
			if (std::fabs(s[0]) + std::fabs(s[1]) + std::fabs(s[2]) < 1e-12) continue;
			const double p0 = s[0] * v[0][0] + s[1] * v[0][1] + s[2] * v[0][2], p1 = s[0] * v[1][0] + s[1] * v[1][1] + s[2] * v[1][2], p2 = s[0] * v[2][0] + s[1] * v[2][1] + s[2] * v[2][2];
			const double r = h * (std::fabs(s[0]) + std::fabs(s[1]) + std::fabs(s[2]));
			if (std::min({p0, p1, p2}) > r || std::max({p0, p1, p2}) < -r) return false;
		}
	return true;
}
// the box test the engine uses against a brush: every plane pushed out by the box's extent along its normal
inline bool BoxInside(const std::vector<Plane> &pl, double x, double y, double z, double h) {
	for (const Plane &q : pl)
		if (q.n[0] * x + q.n[1] * y + q.n[2] * z > q.d + (std::fabs(q.n[0]) + std::fabs(q.n[1]) + std::fabs(q.n[2])) * h) return false;
	return true;
}
}  // namespace

int World::PointContents(double x, double y, double z) const {
	int c = 0;
	for (const Convex &b : m_impl->brushes)
		if (x >= b.mn[0] && x <= b.mx[0] && y >= b.mn[1] && y <= b.mx[1] && z >= b.mn[2] && z <= b.mx[2] && Inside(b.planes, x, y, z)) c |= b.contents;
	return c;
}

int World::LeafContents(double x, double y, double z) const {
	const Impl &im = *m_impl;
	int n = im.worldHead;
	int guard = 0;
	while (n >= 0 && (size_t)n < im.nodes.size() && ++guard < 512) {
		const Plane &p = im.planes[im.nodes[n].plane];
		const double dist = p.n[0] * x + p.n[1] * y + p.n[2] * z - p.d;
		n = im.nodes[n].child[dist >= 0 ? 0 : 1];
	}
	const int l = -1 - n;
	return l >= 0 && (size_t)l < im.leafs.size() ? im.leafs[l].contents : 0;
}

bool World::InProp(double x, double y, double z) const {
	for (const Impl::Prop &p : m_impl->props) {
		if (x < p.mn[0] || x > p.mx[0] || y < p.mn[1] || y > p.mx[1] || z < p.mn[2] || z > p.mx[2]) continue;
		for (const Convex &l : p.ledges)
			if (x >= l.mn[0] && x <= l.mx[0] && y >= l.mn[1] && y <= l.mx[1] && z >= l.mn[2] && z <= l.mx[2] && Inside(l.planes, x, y, z)) return true;
	}
	return false;
}

// ---- (bridge #76) cut caps: a polygon clipped exactly to the visible solid --------------------------------------------------
namespace {
using CPoly = World::Poly;
double SideOf(const Plane &pl, const std::array<double, 3> &p) { return pl.n[0] * p[0] + pl.n[1] * p[1] + pl.n[2] * p[2] - pl.d; }
// Split a convex polygon by a plane: front = n.p - d > 0, back = the rest (a polygon on the plane goes to `onFront` side).
void SplitPoly(const CPoly &p, const Plane &pl, CPoly &front, CPoly &back, bool onFront) {
	front.clear(), back.clear();
	const size_t n = p.size();
	if (n < 3) return;
	const double eps = 1e-7;
	std::vector<double> d(n);
	bool anyF = false, anyB = false;
	for (size_t i = 0; i < n; ++i) {
		d[i] = SideOf(pl, p[i]);
		if (std::fabs(d[i]) <= eps) d[i] = 0;
		anyF |= d[i] > 0, anyB |= d[i] < 0;
	}
	if (!anyB && (anyF || onFront)) return void(front = p);
	if (!anyF) return void(back = p);
	for (size_t i = 0; i < n; ++i) {
		const auto &a = p[i], &b = p[(i + 1) % n];
		const double da = d[i], db = d[(i + 1) % n];
		if (da >= 0) front.push_back(a);
		if (da <= 0) back.push_back(a);
		if ((da > 0 && db < 0) || (da < 0 && db > 0)) {
			const double t = da / (da - db);
			const std::array<double, 3> m{a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
			front.push_back(m), back.push_back(m);
		}
	}
	if (front.size() < 3) front.clear();
	if (back.size() < 3) back.clear();
}
double PolyArea(const CPoly &p) {
	double s[3] = {0, 0, 0};
	for (size_t i = 1; i + 1 < p.size(); ++i) {
		const double u[3] = {p[i][0] - p[0][0], p[i][1] - p[0][1], p[i][2] - p[0][2]}, v[3] = {p[i + 1][0] - p[0][0], p[i + 1][1] - p[0][1], p[i + 1][2] - p[0][2]};
		s[0] += u[1] * v[2] - u[2] * v[1], s[1] += u[2] * v[0] - u[0] * v[2], s[2] += u[0] * v[1] - u[1] * v[0];
	}
	return 0.5 * std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
}
}  // namespace

double World::SolidParts(const Poly &poly0, const double into[3], std::vector<Poly> &out) const {
	const Impl &im = *m_impl;
	out.clear();
	if (poly0.size() < 3) return 0;
	const double h = 0.01;
	Poly poly = poly0;
	for (auto &p : poly)
		for (int t = 0; t < 3; ++t) p[t] += into[t] * h;
	std::vector<Poly> solid, rest;
	// 1. down the BSP tree: pieces in solid leaves are solid (the structural brushes and the sealed outside)
	struct Item {
		int node;
		Poly p;
	};
	std::vector<Item> st;
	st.push_back({im.worldHead, poly});
	Poly f, b;
	int guard = 0;
	while (!st.empty() && ++guard < 200000) {
		Item it = std::move(st.back());
		st.pop_back();
		if (it.node < 0) {
			const int l = -1 - it.node;
			const int c = l >= 0 && (size_t)l < im.leafs.size() ? im.leafs[l].contents : 0;
			((c & (kContentsSolid | kContentsWindow)) ? solid : rest).push_back(std::move(it.p));
			continue;
		}
		if ((size_t)it.node >= im.nodes.size()) {
			rest.push_back(std::move(it.p));
			continue;
		}
		const Plane &pl = im.planes[im.nodes[it.node].plane];
		SplitPoly(it.p, pl, f, b, true);
		if (!f.empty()) st.push_back({im.nodes[it.node].child[0], f});
		if (!b.empty()) st.push_back({im.nodes[it.node].child[1], b});
	}
	// 2. the rest against the visible brushes (detail brushes live in empty leaves): inside parts are solid, outside parts go on
	double mn[3] = {1e30, 1e30, 1e30}, mx[3] = {-1e30, -1e30, -1e30};
	for (const auto &p : poly)
		for (int t = 0; t < 3; ++t) mn[t] = std::min(mn[t], p[t]), mx[t] = std::max(mx[t], p[t]);
	std::vector<const Convex *> cand;  // the visible brushes, then the static props' convex pieces (visible, not cut by the shaders)
	for (const Convex &br : im.brushes)
		if (VisibleSolid(br)) cand.push_back(&br);
	for (const Impl::Prop &pr : im.props)
		if (!(pr.mn[0] > mx[0] + 0.1 || pr.mx[0] < mn[0] - 0.1 || pr.mn[1] > mx[1] + 0.1 || pr.mx[1] < mn[1] - 0.1 || pr.mn[2] > mx[2] + 0.1 || pr.mx[2] < mn[2] - 0.1))
			for (const Convex &l : pr.ledges) cand.push_back(&l);
	for (const Convex *pbr : cand) {
		const Convex &br = *pbr;
		if (rest.empty()) break;
		if (br.mn[0] > mx[0] + 0.1 || br.mx[0] < mn[0] - 0.1 || br.mn[1] > mx[1] + 0.1 || br.mx[1] < mn[1] - 0.1 || br.mn[2] > mx[2] + 0.1 || br.mx[2] < mn[2] - 0.1) continue;
		std::vector<Poly> next;
		for (Poly &r : rest) {
			Poly cur = std::move(r);
			bool inside = true;
			for (const Plane &q : br.planes) {
				SplitPoly(cur, q, f, b, false);
				if (!f.empty()) next.push_back(f);  // outside this plane: not in the brush
				if (b.empty()) {
					inside = false;
					break;
				}
				cur.swap(b);
			}
			if (inside && cur.size() >= 3) solid.push_back(std::move(cur));
		}
		rest.swap(next);
	}
	double area = 0;
	for (Poly &p : solid) {
		const double a = PolyArea(p);
		if (a < 1e-4) continue;
		for (auto &q : p)
			for (int t = 0; t < 3; ++t) q[t] -= into[t] * h;
		area += a;
		out.push_back(std::move(p));
	}
	return area;
}

std::string World::Describe(double x, double y, double z, double r) const {
	std::string out;
	char buf[400];
	int bi = 0;
	for (const Convex &b : m_impl->brushes) {
		++bi;
		if (x < b.mn[0] - r || x > b.mx[0] + r || y < b.mn[1] - r || y > b.mx[1] + r || z < b.mn[2] - r || z > b.mx[2] + r) continue;
		double worst = -1e30;
		for (const Plane &q : b.planes) worst = std::max(worst, q.n[0] * x + q.n[1] * y + q.n[2] * z - q.d);
		std::snprintf(buf, sizeof buf, "  brush #%d contents 0x%x box (%.1f %.1f %.1f)..(%.1f %.1f %.1f) planes %zu signed-dist %.2f%s\n", bi - 1, b.contents, b.mn[0], b.mn[1], b.mn[2], b.mx[0], b.mx[1], b.mx[2],
		              b.planes.size(), worst, b.hasDisp ? " (disp sides)" : "");
		out += buf;
	}
	int pi = 0;
	for (const Impl::Prop &pr : m_impl->props) {
		++pi;
		if (x < pr.mn[0] - r || x > pr.mx[0] + r || y < pr.mn[1] - r || y > pr.mx[1] + r || z < pr.mn[2] - r || z > pr.mx[2] + r) continue;
		std::snprintf(buf, sizeof buf, "  prop #%d box (%.1f %.1f %.1f)..(%.1f %.1f %.1f) ledges %zu\n", pi - 1, pr.mn[0], pr.mn[1], pr.mn[2], pr.mx[0], pr.mx[1], pr.mx[2], pr.ledges.size());
		out += buf;
	}
	return out;
}

World::Probe World::Classify(double x, double y, double z) const {
	Probe p;
	p.contents = PointContents(x, y, z);
	p.leaf = LeafContents(x, y, z);
	p.prop = InProp(x, y, z);
	for (const Convex &b : m_impl->brushes) {
		if (x < b.mn[0] - 40 || x > b.mx[0] + 40 || y < b.mn[1] - 40 || y > b.mx[1] + 40 || z < b.mn[2] - 40 || z > b.mx[2] + 40) continue;
		double worst = -1e30;
		for (const Plane &q : b.planes) worst = std::max(worst, q.n[0] * x + q.n[1] * y + q.n[2] * z - q.d);
		p.surface = std::min(p.surface, std::fabs(worst));
	}
	for (const Impl::Prop &pr : m_impl->props)
		for (const Convex &l : pr.ledges) {
			if (x < l.mn[0] - 40 || x > l.mx[0] + 40 || y < l.mn[1] - 40 || y > l.mx[1] + 40 || z < l.mn[2] - 40 || z > l.mx[2] + 40) continue;
			double worst = -1e30;
			for (const Plane &q : l.planes) worst = std::max(worst, q.n[0] * x + q.n[1] * y + q.n[2] * z - q.d);
			p.surface = std::min(p.surface, std::fabs(worst));
		}
	return p;
}

void World::VoxeliseRegion(const Mapping &m, const Options &o, int rx, int ry, int rz, std::vector<ColBlock> &out, RegionStats *st) const {
	out.clear();
	const Impl &im = *m_impl;
	const double upb = m.upb, sub = 1.0 / 8.0;
	// Source coordinates of the sub-voxel centres, per axis (MC axes: ix east, iy up, iz south)
	std::vector<double> X(kGrid), Y(kGrid), Z(kGrid);  // X[ix] Source x, Y[iz] Source y, Z[iy] Source z
	for (int i = 0; i < kGrid; ++i) {
		const double mcc = (i + 0.5) * sub;
		X[i] = m.sx + (rx * kRegionBlocks + mcc - m.mcX) * upb;
		Y[i] = m.sy - (rz * kRegionBlocks + mcc - m.mcZ) * upb;
		Z[i] = m.sz + (ry * kRegionBlocks + mcc - m.mcY) * upb;
	}
	// the region's Source box
	double rmn[3] = {std::min(X[0], X[kGrid - 1]) - upb, std::min(Y[0], Y[kGrid - 1]) - upb, std::min(Z[0], Z[kGrid - 1]) - upb};
	double rmx[3] = {std::max(X[0], X[kGrid - 1]) + upb, std::max(Y[0], Y[kGrid - 1]) + upb, std::max(Z[0], Z[kGrid - 1]) + upb};
	auto overlaps = [&](const double mn[3], const double mx[3], double pad) {
		return mn[0] <= rmx[0] + pad && mx[0] >= rmn[0] - pad && mn[1] <= rmx[1] + pad && mx[1] >= rmn[1] - pad && mn[2] <= rmx[2] + pad && mx[2] >= rmn[2] - pad;
	};
	std::vector<const Convex *> bs, clipFloorCand;
	std::vector<const Convex *> ps;
	for (const Convex &b : im.brushes) {
		if (overlaps(b.mn, b.mx, 3.0)) bs.push_back(&b);
		if ((b.contents & kMaskSolid) && b.mn[0] <= rmx[0] && b.mx[0] >= rmn[0] && b.mn[1] <= rmx[1] && b.mx[1] >= rmn[1] && b.mx[2] >= rmn[2] - 170.0 && b.mn[2] <= rmx[2])
			clipFloorCand.push_back(&b);
	}
	if (o.staticProps)
		for (const Impl::Prop &p : im.props)
			if (overlaps(p.mn, p.mx, 3.0))
				for (const Convex &l : p.ledges)
					if (overlaps(l.mn, l.mx, 3.0)) ps.push_back(&l);
	if (bs.empty() && ps.empty() && !o.leafPoint) return;
	if (st) st->brushes = (int)bs.size(), st->props = (int)ps.size();

	std::vector<uint8_t> grid((size_t)kGrid * kGrid * kGrid, 0);  // (iy * 64 + iz) * 64 + ix; bit0 solid, bit1 playerclip
	auto idx = [](int ix, int iy, int iz) { return ((size_t)iy * kGrid + iz) * kGrid + ix; };
	// index ranges of a Source box
	auto range = [&](const double mn[3], const double mx[3], int lo[3], int hi[3]) {
		// Source x -> ix
		auto fi = [&](double mcc) { return mcc; };
		(void)fi;
		const double mcx0 = m.mcX + (mn[0] - m.sx) / upb, mcx1 = m.mcX + (mx[0] - m.sx) / upb;
		const double mcz0 = m.mcZ - (mx[1] - m.sy) / upb, mcz1 = m.mcZ - (mn[1] - m.sy) / upb;
		const double mcy0 = m.mcY + (mn[2] - m.sz) / upb, mcy1 = m.mcY + (mx[2] - m.sz) / upb;
		auto lo1 = [&](double mc, int r) { return std::max(0, (int)std::floor((mc - r * kRegionBlocks) * 8 - 0.5) - 1); };
		auto hi1 = [&](double mc, int r) { return std::min(kGrid - 1, (int)std::ceil((mc - r * kRegionBlocks) * 8 - 0.5) + 1); };
		lo[0] = lo1(mcx0, rx), hi[0] = hi1(mcx1, rx);
		lo[1] = lo1(mcy0, ry), hi[1] = hi1(mcy1, ry);
		lo[2] = lo1(mcz0, rz), hi[2] = hi1(mcz1, rz);
	};
	auto raster = [&](const Convex &c, uint8_t flag, double margin = 0.0) {
		const double pad = margin + 0.01;
		const double bmn[3] = {c.mn[0] - pad, c.mn[1] - pad, c.mn[2] - pad}, bmx[3] = {c.mx[0] + pad, c.mx[1] + pad, c.mx[2] + pad};
		int lo[3], hi[3];
		range(bmn, bmx, lo, hi);
		for (int iy = lo[1]; iy <= hi[1]; ++iy) {
			const double z = Z[iy];
			if (z < bmn[2] || z > bmx[2]) continue;
			for (int iz = lo[2]; iz <= hi[2]; ++iz) {
				const double y = Y[iz];
				if (y < bmn[1] || y > bmx[1]) continue;
				for (int ix = lo[0]; ix <= hi[0]; ++ix) {
					const double x = X[ix];
					if (x < bmn[0] || x > bmx[0]) continue;
					if (margin == 0.0 ? Inside(c.planes, x, y, z) : InsideM(c.planes, x, y, z, margin)) grid[idx(ix, iy, iz)] |= flag;
				}
			}
		}
	};
	if (o.leafPoint) {  // the tree's leaf contents (the solid outside the map included), plus the exact brushes below
		// Descend the BSP once over the whole index box: an axis-aligned node splits the box exactly, anything else is
		// answered voxel by voxel from that node; a leaf fills its box.
		struct Rec {
			const Impl &im;
			const std::vector<double> &X, &Y, &Z;
			std::vector<uint8_t> &grid;
			int leafBoxes = 0;
			void Fill(int leaf, const int lo[3], const int hi[3]) {
				const int c = leaf >= 0 && (size_t)leaf < im.leafs.size() ? im.leafs[leaf].contents : 0;
				const uint8_t f = ((c & kMaskSolid) ? 1 : 0) | ((c & kContentsPlayerClip) ? 2 : 0) | ((c & (kContentsSolid | kContentsWindow)) ? 4 : 0);
				if (!f) return;
				++leafBoxes;
				for (int iy = lo[1]; iy <= hi[1]; ++iy)
					for (int iz = lo[2]; iz <= hi[2]; ++iz)
						for (int ix = lo[0]; ix <= hi[0]; ++ix) grid[((size_t)iy * kGrid + iz) * kGrid + ix] |= f;
			}
			int Walk(int n, double x, double y, double z) const {
				int guard = 0;
				while (n >= 0 && (size_t)n < im.nodes.size() && ++guard < 512) {
					const Plane &p = im.planes[im.nodes[n].plane];
					n = im.nodes[n].child[p.n[0] * x + p.n[1] * y + p.n[2] * z - p.d >= 0 ? 0 : 1];
				}
				return -1 - n;
			}
			void Go(int n, const int lo[3], const int hi[3]) {
				if (n < 0) return Fill(-1 - n, lo, hi);
				if ((size_t)n >= im.nodes.size()) return;
				const Plane &p = im.planes[im.nodes[n].plane];
				int axis = -1;
				double sgn = 1;
				for (int a = 0; a < 3; ++a)
					if (std::fabs(p.n[a]) == 1.0 && p.n[(a + 1) % 3] == 0.0 && p.n[(a + 2) % 3] == 0.0) axis = a, sgn = p.n[a];
				if (axis < 0) {
					for (int iy = lo[1]; iy <= hi[1]; ++iy)
						for (int iz = lo[2]; iz <= hi[2]; ++iz)
							for (int ix = lo[0]; ix <= hi[0]; ++ix) {
								const int l = Walk(n, X[ix], Y[iz], Z[iy]);
								const int a[3] = {ix, iy, iz};
								Fill(l, a, a);
							}
					return;
				}
				// index dimension of the Source axis: x -> ix (0), y -> iz (2), z -> iy (1)
				const int dim = axis == 0 ? 0 : axis == 1 ? 2 : 1;
				const std::vector<double> &C = axis == 0 ? X : axis == 1 ? Y : Z;
				int firstFront = hi[dim] + 1, lastFront = lo[dim] - 1;  // front voxels form one run of the interval
				for (int i = lo[dim]; i <= hi[dim]; ++i)
					if (sgn * C[i] - p.d >= 0) {
						firstFront = std::min(firstFront, i);
						lastFront = std::max(lastFront, i);
					}
				int a[3] = {lo[0], lo[1], lo[2]}, b[3] = {hi[0], hi[1], hi[2]};
				if (firstFront <= lastFront) {
					a[dim] = firstFront, b[dim] = lastFront;
					Go(im.nodes[n].child[0], a, b);
				}
				// the back voxels: below firstFront and above lastFront (one of them, as the front run touches an end of the interval)
				if (firstFront > lastFront) {
					Go(im.nodes[n].child[1], lo, hi);
				} else {
					if (firstFront > lo[dim]) {
						a[dim] = lo[dim], b[dim] = firstFront - 1;
						Go(im.nodes[n].child[1], a, b);
					}
					if (lastFront < hi[dim]) {
						a[dim] = lastFront + 1, b[dim] = hi[dim];
						Go(im.nodes[n].child[1], a, b);
					}
				}
			}
		} rec{im, X, Y, Z, grid};
		const int lo[3] = {0, 0, 0}, hi[3] = {kGrid - 1, kGrid - 1, kGrid - 1};
		rec.Go(im.worldHead, lo, hi);
	}
	{
		for (const Convex *b : bs) {
			uint8_t flag = ((b->contents & kMaskSolid) ? 1 : 0) | ((b->contents & kContentsPlayerClip) ? 2 : 0) | (VisibleSolid(*b) ? 4 : 0);
			if (flag & 3) raster(*b, flag);
		}
	}
	for (const Convex *l : ps) raster(*l, 1 | 4, o.propMargin);
	for (const Impl::Disp &dsp : im.disps) {
		if (!overlaps(dsp.mn, dsp.mx, 3.0)) continue;
		int lo[3], hi[3];
		range(dsp.mn, dsp.mx, lo, hi);
		for (int iy = lo[1]; iy <= hi[1]; ++iy)
			for (int iz = lo[2]; iz <= hi[2]; ++iz)
				for (int ix = lo[0]; ix <= hi[0]; ++ix) {
					uint8_t &g = grid[idx(ix, iy, iz)];
					if ((g & 5) == 5) continue;  // (bridge #46: a solid of a ghost brush still takes the displacement's visibility)
					const double x = X[ix], y = Y[iz], z = Z[iy];
					if (dsp.planes.empty()) {  // a displacement with no brush under it: a thin sheet; the 5 u cell it passes through (the engine's box against the triangles)
						const double ctr[3] = {x, y, z};
						bool touch = false;
						for (size_t q = 0; q + 8 < dsp.tris.size() && !touch; q += 9) touch = TriBox(&dsp.tris[q], &dsp.tris[q + 3], &dsp.tris[q + 6], ctr, 2.45);
						if (touch) {
							g |= 1 | 4;
							continue;
						}
					}
					if (dsp.planes.empty() || !Inside(dsp.planes, x, y, z)) continue;
					// The engine's displacement collision reports a sub-voxel whose 5 u cell the displaced surface touches (a box
					// against the triangles), not only one whose centre is under it: test the cell's bottom centre and bottom corners.
					const double h = 2.45, sp[5][2] = {{0, 0}, {-h, -h}, {h, -h}, {-h, h}, {h, h}};
					bool below = false;
					for (int s = 0; s < 5 && !below; ++s) {
						const double px = x + sp[s][0], py = y + sp[s][1], pz = z - h;
						if (s && !Inside(dsp.planes, px, py, pz)) continue;
						// below the displaced surface: a ray along the face normal crosses it an odd number of times
						int hits = 0;
						for (size_t q = 0; q + 8 < dsp.tris.size(); q += 9) {
							const double *a = &dsp.tris[q], *b = a + 3, *c = a + 6;
							const double e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
							const double hh[3] = {dsp.n[1] * e2[2] - dsp.n[2] * e2[1], dsp.n[2] * e2[0] - dsp.n[0] * e2[2], dsp.n[0] * e2[1] - dsp.n[1] * e2[0]};
							const double det = e1[0] * hh[0] + e1[1] * hh[1] + e1[2] * hh[2];
							if (std::fabs(det) < 1e-9) continue;
							const double s3[3] = {px - a[0], py - a[1], pz - a[2]};
							const double u = (s3[0] * hh[0] + s3[1] * hh[1] + s3[2] * hh[2]) / det;
							if (u < 0 || u > 1) continue;
							const double qv[3] = {s3[1] * e1[2] - s3[2] * e1[1], s3[2] * e1[0] - s3[0] * e1[2], s3[0] * e1[1] - s3[1] * e1[0]};
							const double v = (dsp.n[0] * qv[0] + dsp.n[1] * qv[1] + dsp.n[2] * qv[2]) / det;
							if (v < 0 || u + v > 1) continue;
							const double t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) / det;
							if (t > 0) ++hits;
						}
						below = (hits & 1) != 0;
					}
					if (below) g |= 1 | 4;
				}
	}

	// a playerclip voxel is kept only as a floor over nothing (clipMode 0): a ray 160 u straight down meets no solid brush
	auto floorOverVoid = [&](double px, double py, double pz) {
		for (const Convex *b : clipFloorCand) {
			if (px < b->mn[0] || px > b->mx[0] || py < b->mn[1] || py > b->mx[1]) continue;
			double lo = 0, hi = 1;
			bool miss = false;
			for (const Plane &q : b->planes) {
				const double d0 = q.n[0] * px + q.n[1] * py + q.n[2] * pz - q.d, den = -160.0 * q.n[2];
				if (std::fabs(den) < 1e-12) {
					if (d0 > 0) { miss = true; break; }
					continue;
				}
				const double t = -d0 / den;
				if (den > 0) hi = std::min(hi, t);
				else lo = std::max(lo, t);
				if (lo > hi) { miss = true; break; }
			}
			if (!miss && lo < 1.0 && lo <= hi) return false;
		}
		return true;
	};

	const Box *ex = o.hasExclude ? &o.exclude : nullptr;
	auto inHole = [&](double x, double y, double z) {
		for (int i = 0; i < o.nHoles; ++i)
			if (o.holes[i].Contains(x, y, z)) return true;
		return false;
	};
	// "void": solid by the tree's leaf (the sealed map's outside is filled with solid leaves) but in no brush and no prop
	std::vector<const Convex *> bsWide, psWide;
	if (o.markNoDig) {
		const double pad = 2 * upb + 5;
		for (const Convex &b : im.brushes)
			if ((b.contents & kMaskSolid) && b.mn[0] <= rmx[0] + pad && b.mx[0] >= rmn[0] - pad && b.mn[1] <= rmx[1] + pad && b.mx[1] >= rmn[1] - pad && b.mn[2] <= rmx[2] + pad && b.mx[2] >= rmn[2] - pad)
				bsWide.push_back(&b);
		for (const Impl::Prop &p : im.props)
			if (p.mn[0] <= rmx[0] + pad && p.mx[0] >= rmn[0] - pad && p.mn[1] <= rmx[1] + pad && p.mx[1] >= rmn[1] - pad && p.mn[2] <= rmx[2] + pad && p.mx[2] >= rmn[2] - pad)
				for (const Convex &l : p.ledges) psWide.push_back(&l);
	}
	auto voidAt = [&](double x, double y, double z) {
		if (!(LeafContents(x, y, z) & kMaskSolid)) return false;
		for (const Convex *b : bsWide)
			if (x >= b->mn[0] && x <= b->mx[0] && y >= b->mn[1] && y <= b->mx[1] && z >= b->mn[2] && z <= b->mx[2] && Inside(b->planes, x, y, z)) return false;
		for (const Convex *l : psWide)
			if (x >= l->mn[0] && x <= l->mx[0] && y >= l->mn[1] && y <= l->mx[1] && z >= l->mn[2] && z <= l->mx[2] && Inside(l->planes, x, y, z)) return false;
		return true;
	};
	const DugSet *dug = o.dug && !o.dug->empty() ? o.dug.get() : nullptr;
	for (int by = 0; by < kRegionBlocks; ++by)
		for (int bz = 0; bz < kRegionBlocks; ++bz)
			for (int bx = 0; bx < kRegionBlocks; ++bx) {
				if (dug && dug->count(BlockKey(rx * kRegionBlocks + bx, ry * kRegionBlocks + by, rz * kRegionBlocks + bz))) {
					if (st) ++st->dugSkipped;
					continue;  // dug out by the Minecraft player: its host geometry is gone
				}
				// block centre in Source
				const double cx = m.sx + (rx * kRegionBlocks + bx + 0.5 - m.mcX) * upb, cy = m.sy - (rz * kRegionBlocks + bz + 0.5 - m.mcZ) * upb,
				             cz = m.sz + (ry * kRegionBlocks + by + 0.5 - m.mcY) * upb;
				if (ex && ex->Contains(cx, cy, cz)) continue;
				ColBlock cb{};
				cb.x = rx * kRegionBlocks + bx, cb.y = ry * kRegionBlocks + by, cb.z = rz * kRegionBlocks + bz;
				bool any = false;
				bool touches = false;  // some non-air voxel (clip included) in the block
				uint64_t vis[8] = {0, 0, 0, 0, 0, 0, 0, 0};  // (bridge #46) the solid sub-voxels that are visible geometry
				for (int sy = 0; sy < 8; ++sy) {
					uint64_t layer = 0;
					for (int sz = 0; sz < 8; ++sz)
						for (int sx = 0; sx < 8; ++sx) {
							const int ix = bx * 8 + sx, iy = by * 8 + sy, iz = bz * 8 + sz;
							const uint8_t g = grid[idx(ix, iy, iz)];
							if (!g) continue;
							touches = true;
							bool solid = (g & 1) != 0;
							if (!solid && (g & 2)) {
								if (st) ++st->clipVoxels;
								solid = o.clipMode == 1 || floorOverVoid(X[ix], Y[iz], Z[iy]);
								if (solid && st) ++st->clipKept;
							}
							if (solid && inHole(X[ix], Y[iz], Z[iy])) solid = false;
							if (solid) layer |= 1ull << (sz * 8 + sx);
							if (solid && (g & 4)) vis[sy] |= 1ull << (sz * 8 + sx);
						}
					cb.bits[sy] = layer;
					any |= layer != 0;
				}
				if (!any && o.thinFallback) {
					// thin geometry: a brush thinner than a sub-voxel can miss every centre; box-test each sub-voxel (half 2.45 u)
					const double h = upb / 16 - 0.05, bh = upb / 2 - 0.05;
					const int mask = o.clipMode == 1 ? (kMaskSolid | kContentsPlayerClip) : kMaskSolid;
					std::vector<const Convex *> cand;
					for (const Convex *b : bs)
						if ((b->contents & mask) && cx + bh + h >= b->mn[0] && cx - bh - h <= b->mx[0] && cy + bh + h >= b->mn[1] && cy - bh - h <= b->mx[1] && cz + bh + h >= b->mn[2] &&
						    cz - bh - h <= b->mx[2])
							cand.push_back(b);
					for (const Convex *l : ps)
						if (cx + bh + h >= l->mn[0] && cx - bh - h <= l->mx[0] && cy + bh + h >= l->mn[1] && cy - bh - h <= l->mx[1] && cz + bh + h >= l->mn[2] && cz - bh - h <= l->mx[2]) cand.push_back(l);
					if (!cand.empty()) {
						bool thin = false;
						for (int sy = 0; sy < 8; ++sy) {
							uint64_t layer = 0;
							for (int sz = 0; sz < 8; ++sz)
								for (int sx = 0; sx < 8; ++sx) {
									const int ix = bx * 8 + sx, iy = by * 8 + sy, iz = bz * 8 + sz;
									const double x = X[ix], y = Y[iz], z = Z[iy];
									if (inHole(x, y, z)) continue;
									bool seen = false;
									for (const Convex *b : cand)
										if (x + h >= b->mn[0] && x - h <= b->mx[0] && y + h >= b->mn[1] && y - h <= b->mx[1] && z + h >= b->mn[2] && z - h <= b->mx[2] && BoxInside(b->planes, x, y, z, h)) {
											layer |= 1ull << (sz * 8 + sx);
											seen |= VisibleSolid(*b);
										}
									if (seen) vis[sy] |= 1ull << (sz * 8 + sx);
								}
							cb.bits[sy] = layer;
							any |= layer != 0;
							thin |= layer != 0;
						}
						if (thin && st) ++st->thinBlocks;
					}
				}
				(void)touches;
				if (any) {
					// (bridge #46) how much of the cell is geometry a player can see (see the protocol header): the 27 reveal sample points and "any"
					static const int kSample[3] = {1, 4, 6};
					int samples = 0;
					uint64_t anyVis = 0;
					for (int sy = 0; sy < 8; ++sy) anyVis |= vis[sy];
					for (int a : kSample)
						for (int b : kSample)
							for (int c : kSample) samples += (int)((vis[a] >> (b * 8 + c)) & 1ull);
					// "wholly inside the host's geometry" (the mod reveals a cell as a block at 25 of 27) means a cell that is solid from face to face: a cell whose
					// surface layer lies inside it (a wall face 5 u past the cell's edge: 27 of 27 sample points, 448 of 512 voxels) would become a full block that
					// sticks 5 u out of the wall around every hole. Only a wholly solid, wholly visible cell reports 27; anything less is capped under the threshold.
					bool whole = true;
					for (int sy = 0; sy < 8; ++sy) whole = whole && vis[sy] == ~0ull;
					if (!whole) samples = std::min(samples, 24);
					cb.pad |= skycraft::proto::kColBlockVisKnown | (anyVis ? skycraft::proto::kColBlockVisAny : 0u) | ((uint32_t)samples << skycraft::proto::kColBlockVisSamplesShift);
				}
				if (any && o.markNoDig) {
					bool nearVoid = voidAt(cx, cy, cz);
					for (int a = 0; a < 6 && !nearVoid; ++a) {
						const double s = (a & 1) ? upb : -upb;
						nearVoid = voidAt(cx + (a >> 1 == 0 ? s : 0), cy + (a >> 1 == 1 ? s : 0), cz + (a >> 1 == 2 ? s : 0));
					}
					if (nearVoid) cb.pad |= skycraft::proto::kColBlockNoDig, st ? (void)++st->noDig : (void)0;
				}
				if (any && o.limit > 0 && (std::fabs(cx) + upb * 0.5 > o.limit || std::fabs(cy) + upb * 0.5 > o.limit || std::fabs(cz) + upb * 0.5 > o.limit)) {
					cb.pad |= skycraft::proto::kColBlockLimit;  // (bridge #38) at the edge of what Portal 2 can hold: bedrock
					if (st) ++st->limit;
				}
				if (any) out.push_back(cb);
			}
	if (st) st->blocks = (int)out.size();
}

}  // namespace colharvest
