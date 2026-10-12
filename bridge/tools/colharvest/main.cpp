// Offline equivalence tool for the harvest-based collision export (bridge #33).
//
// Harvests a map's collision from the BSP on disk (colharvest.cpp, the same source the plugin's worker uses) and diffs its
// voxels against the static cache the point-query path produced (cache/bridge/colcache/<map>.bin, v4). No game, no
// Minecraft. Build and run: see Makefile (`make -C tools/colharvest run MAP=pc_pit_flings`).
//
//   colharvest --bsp <file.bsp> --cache <map.bin> [--game <dir>]... [--clip 0|1] [--brush-only] [--no-props] [--no-thin]
//              [--regions N] [--samples N] [--map-anchor mcx,mcy,mcz,sx,sy,sz]
//
// The mapping is rebuilt the way clip.cpp does it: MC (-328, 114, 424) at the pc_mc_region entity's x/y and
// regionMin.z + (114 - 40) * 40; the cache header's offset (the MC block at Source 0 0 0) is checked against it.
#include "colharvest.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using colharvest::ColBlock;

struct Region {
	int k[3];
	std::vector<ColBlock> blocks;
};

static bool EntityVec(const std::string &ents, const std::string &targetname, std::vector<std::array<double, 3>> &out) {
	size_t pos = 0;
	while ((pos = ents.find('{', pos)) != std::string::npos) {
		const size_t end = ents.find('}', pos);
		if (end == std::string::npos) break;
		const std::string blk = ents.substr(pos, end - pos);
		pos = end;
		if (blk.find("\"targetname\" \"" + targetname + "\"") == std::string::npos) continue;
		const size_t o = blk.find("\"origin\" \"");
		if (o == std::string::npos) continue;
		double v[3];
		if (std::sscanf(blk.c_str() + o + 10, "%lf %lf %lf", &v[0], &v[1], &v[2]) == 3) out.push_back({v[0], v[1], v[2]});
	}
	return !out.empty();
}

int main(int argc, char **argv) {
	std::string bsp, cachePath;
	std::vector<std::string> games;
	colharvest::Options opt;
	int maxRegions = 1 << 30, samples = 12, skip = 0, stride = 1;
	bool haveAnchor = false;
	std::vector<std::string> probes, columns;
	std::string dumpBox, dumpOut;
	bool capsMode = false;
	std::vector<std::array<int, 3>> only;
	double anchor[6] = {0};
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
		if (a == "--bsp") bsp = next();
		else if (a == "--cache") cachePath = next();
		else if (a == "--game") games.push_back(next());
		else if (a == "--clip") opt.clipMode = std::atoi(next().c_str());
		else if (a == "--leaf") opt.leafPoint = true;
		else if (a == "--brush-only") opt.leafPoint = false;
		else if (a == "--no-props") opt.staticProps = false;
		else if (a == "--prop-margin") opt.propMargin = std::atof(next().c_str());
		else if (a == "--no-thin") opt.thinFallback = false;
		else if (a == "--regions") maxRegions = std::atoi(next().c_str());
		else if (a == "--skip") skip = std::atoi(next().c_str());
		else if (a == "--stride") stride = std::atoi(next().c_str());
		else if (a == "--probe") { probes.push_back(next()); }
		else if (a == "--column") columns.push_back(next());
		else if (a == "--only") { int k[3]; if (std::sscanf(next().c_str(), "%d,%d,%d", &k[0], &k[1], &k[2]) == 3) only.push_back({k[0], k[1], k[2]}); }
		else if (a == "--dug") { int k[3]; if (std::sscanf(next().c_str(), "%d,%d,%d", &k[0], &k[1], &k[2]) == 3) { if (!opt.dug) opt.dug = std::make_shared<colharvest::DugSet>(); const_cast<colharvest::DugSet &>(*opt.dug).insert(colharvest::BlockKey(k[0], k[1], k[2])); } }
		else if (a == "--dump-field") dumpBox = next();
		else if (a == "--caps") capsMode = true;
		else if (a == "--out") dumpOut = next();
		else if (a == "--samples") samples = std::atoi(next().c_str());
		else if (a == "--map-anchor") haveAnchor = std::sscanf(next().c_str(), "%lf,%lf,%lf,%lf,%lf,%lf", &anchor[0], &anchor[1], &anchor[2], &anchor[3], &anchor[4], &anchor[5]) == 6;
	}
	if (bsp.empty() || (cachePath.empty() && dumpBox.empty() && !capsMode)) {
		std::fprintf(stderr, "usage: colharvest --bsp <file.bsp> --cache <map.bin> [--game <dir>]... [--clip 0|1] [--brush-only] [--no-props] [--no-thin] [--regions N] [--samples N]\n");
		return 2;
	}
	if (games.empty()) {
		const char *p2 = std::getenv("PORTAL2_DIR");  // set by tools/portal2_dir.sh (source it), or pass --game <dir> yourself
		if (!p2 || !*p2) return std::fprintf(stderr, "colharvest: no --game given and PORTAL2_DIR is not set (run tools/portal2_dir.sh to find Portal 2)\n"), 2;
		const std::string root = std::string(p2) + "/";
		for (const char *g : {"update", "portal2_dlc2", "portal2_dlc1", "portal2"}) games.push_back(root + g);
	}
	auto t0 = std::chrono::steady_clock::now();
	colharvest::World world;
	std::string err, log;
	if (!world.Load(bsp, games, err, &log)) return std::fprintf(stderr, "load failed: %s\n", err.c_str()), 1;
	std::printf("%s", log.c_str());
	std::printf("harvest load: %.2f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());

	// the mapping
	colharvest::Mapping m;
	std::vector<std::array<double, 3>> reg, bounds;
	EntityVec(world.entities(), "pc_mc_region", reg);
	EntityVec(world.entities(), "pc_mc_region_bounds", bounds);
	if (haveAnchor) m.mcX = anchor[0], m.mcY = anchor[1], m.mcZ = anchor[2], m.sx = anchor[3], m.sy = anchor[4], m.sz = anchor[5];
	else if (!reg.empty() && bounds.size() >= 2) {
		const double minz = std::min(bounds[0][2], bounds[1][2]);
		m.mcX = -328, m.mcY = 114, m.mcZ = 424;
		m.sx = reg[0][0], m.sy = reg[0][1], m.sz = minz + (114.0 - 40.0) * 40.0;
		opt.hasExclude = true;
		for (int i = 0; i < 3; ++i) opt.exclude.mn[i] = std::min(bounds[0][i], bounds[1][i]), opt.exclude.mx[i] = std::max(bounds[0][i], bounds[1][i]);
	} else {
		std::fprintf(stderr, "no pc_mc_region in the map: give --map-anchor\n");
		return 1;
	}
	std::printf("mapping: MC (%.0f %.0f %.0f) at Source (%.0f %.0f %.0f), exclude box %s\n", m.mcX, m.mcY, m.mcZ, m.sx, m.sy, m.sz, opt.hasExclude ? "on" : "off");

	// --dump-field x0,y0,z0,x1,y1,z1 --out file: the harvest's blocks in a box of Minecraft block coordinates, as text (bridge #38: the Fabric mod's reveal
	// rule is tested on it, src/test/resources). One line per block: "x y z pad F" for a wholly solid block, else "x y z pad w0 .. w7" (hex layers, bottom to top).
	if (!dumpBox.empty()) {
		int b[6];
		if (std::sscanf(dumpBox.c_str(), "%d,%d,%d,%d,%d,%d", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6 || dumpOut.empty()) return std::fprintf(stderr, "--dump-field x0,y0,z0,x1,y1,z1 --out file\n"), 2;
		FILE *f = std::fopen(dumpOut.c_str(), "w");
		if (!f) return std::fprintf(stderr, "cannot write %s\n", dumpOut.c_str()), 1;
		std::fprintf(f, "# harvest of %s, MC blocks %d,%d,%d .. %d,%d,%d (mapping MC %.0f %.0f %.0f at Source %.0f %.0f %.0f, exclude box %s); every block with a solid sub-voxel, bits[y] bit z * 8 + x\n", bsp.c_str(), b[0], b[1], b[2], b[3], b[4], b[5], m.mcX, m.mcY, m.mcZ, m.sx, m.sy, m.sz,
		             opt.hasExclude ? "on" : "off");
		long long n = 0;
		for (int rx = (int)std::floor(b[0] / 8.0); rx <= (int)std::floor(b[3] / 8.0); ++rx)
			for (int ry = (int)std::floor(b[1] / 8.0); ry <= (int)std::floor(b[4] / 8.0); ++ry)
				for (int rz = (int)std::floor(b[2] / 8.0); rz <= (int)std::floor(b[5] / 8.0); ++rz) {
					std::vector<ColBlock> nb;
					world.VoxeliseRegion(m, opt, rx, ry, rz, nb);
					for (const ColBlock &cb : nb) {
						if (cb.x < b[0] || cb.x > b[3] || cb.y < b[1] || cb.y > b[4] || cb.z < b[2] || cb.z > b[5]) continue;
						bool whole = true;
						for (int y = 0; y < 8; ++y) whole &= cb.bits[y] == ~0ull;
						std::fprintf(f, "%d %d %d %u", cb.x, cb.y, cb.z, cb.pad);
						if (whole) std::fprintf(f, " F\n");
						else {
							for (int y = 0; y < 8; ++y) std::fprintf(f, " %016llx", (unsigned long long)cb.bits[y]);
							std::fprintf(f, "\n");
						}
						++n;
					}
				}
		std::fclose(f);
		std::printf("dumped %lld blocks to %s\n", n, dumpOut.c_str());
		return 0;
	}
	// (bridge #76) --caps with --dug cells: for every face between a dug cell and a non-dug neighbour, the exact cap (World::SolidParts:
	// the face clipped to the visible solid) against the mod's hole wall (DigWallQuads: the neighbour's solid sub-voxels next to the dug
	// cell, 5 u squares). Sampled at 0.5 u over the face: "gap" = solid in the BSP but no wall (the clear colour shows there), "over" =
	// wall where there is no solid (a lip into open air).
	if (capsMode) {
		if (!opt.dug || opt.dug->empty()) return std::fprintf(stderr, "--caps needs --dug cells\n"), 2;
		auto toSrc = [&](double x, double y, double z) { return std::array<double, 3>{m.sx + (x - m.mcX) * m.upb, m.sy - (z - m.mcZ) * m.upb, m.sz + (y - m.mcY) * m.upb}; };
		std::map<std::tuple<int, int, int>, ColBlock> blocks;
		std::set<std::tuple<int, int, int>> done;
		auto blockAt = [&](int x, int y, int z) -> const ColBlock * {
			auto rk = std::make_tuple((int)std::floor(x / 8.0), (int)std::floor(y / 8.0), (int)std::floor(z / 8.0));
			if (done.insert(rk).second) {
				std::vector<ColBlock> nb;
				world.VoxeliseRegion(m, opt, std::get<0>(rk), std::get<1>(rk), std::get<2>(rk), nb);
				for (const ColBlock &cb : nb) blocks[std::make_tuple(cb.x, cb.y, cb.z)] = cb;
			}
			auto it = blocks.find(std::make_tuple(x, y, z));
			return it == blocks.end() ? nullptr : &it->second;
		};
		static const int step[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
		static const char *dname[6] = {"down", "up", "north", "south", "west", "east"};
		double totGap = 0, totOver = 0, totExact = 0, totVox = 0;
		int faces = 0, facesGap = 0;
		std::vector<std::array<int, 3>> cells;
		for (uint64_t k : *opt.dug) {
			const int x = (int)((k >> 38) & 0x3FFFFFF) - 33554432, z = (int)((k >> 12) & 0x3FFFFFF) - 33554432, y = (int)(k & 0xFFF) - 2048;
			cells.push_back({x, y, z});
		}
		std::sort(cells.begin(), cells.end());
		for (const auto &c : cells)
			for (int d = 0; d < 6; ++d) {
				const int nx = c[0] + step[d][0], ny = c[1] + step[d][1], nz = c[2] + step[d][2];
				if (opt.dug->count(colharvest::BlockKey(nx, ny, nz))) continue;
				// the face's corners (MC), on the plane between the two cells
				const int ax = step[d][0] ? 0 : step[d][1] ? 1 : 2;
				const double plane = (step[d][ax] > 0 ? 1.0 : 0.0) + c[ax];
				const int a1 = (ax + 1) % 3, a2 = (ax + 2) % 3;
				colharvest::World::Poly poly;
				const double uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
				for (auto &q : uv) {
					double p[3];
					p[ax] = plane, p[a1] = c[a1] + q[0], p[a2] = c[a2] + q[1];
					poly.push_back(toSrc(p[0], p[1], p[2]));
				}
				const auto s0 = toSrc(c[0] + 0.5, c[1] + 0.5, c[2] + 0.5), s1 = toSrc(nx + 0.5, ny + 0.5, nz + 0.5);
				double into[3] = {(s1[0] - s0[0]) / m.upb, (s1[1] - s0[1]) / m.upb, (s1[2] - s0[2]) / m.upb};
				std::vector<colharvest::World::Poly> parts;
				const double area = world.SolidParts(poly, into, parts);
				const ColBlock *nb = blockAt(nx, ny, nz);
				const bool ghost = nb && (nb->pad & skycraft::proto::kColBlockVisKnown) && !(nb->pad & skycraft::proto::kColBlockVisAny);
				// sample the face at 0.5 u (80 x 80)
				int gap = 0, over = 0, ex = 0, vx = 0;
				for (int i = 0; i < 80; ++i)
					for (int j = 0; j < 80; ++j) {
						double p[3];
						p[ax] = plane, p[a1] = c[a1] + (i + 0.5) / 80.0, p[a2] = c[a2] + (j + 0.5) / 80.0;
						const auto s = toSrc(p[0], p[1], p[2]);
						bool inExact = false;
						for (const auto &pp : parts) {
							// point in a convex planar polygon: same side of every edge (around the face normal `into`)
							bool in = true;
							for (size_t e = 0; e < pp.size() && in; ++e) {
								const auto &A = pp[e], &B = pp[(e + 1) % pp.size()];
								const double ex1[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]}, w[3] = {s[0] - A[0], s[1] - A[1], s[2] - A[2]};
								const double cr[3] = {ex1[1] * w[2] - ex1[2] * w[1], ex1[2] * w[0] - ex1[0] * w[2], ex1[0] * w[1] - ex1[1] * w[0]};
								const double sd = cr[0] * into[0] + cr[1] * into[1] + cr[2] * into[2];
								static thread_local double sign0;
								if (e == 0) sign0 = sd;
								else if ((sd > 1e-9 && sign0 < -1e-9) || (sd < -1e-9 && sign0 > 1e-9)) in = false;
								if (std::fabs(sign0) <= 1e-9) sign0 = sd;
							}
							if (in) { inExact = true; break; }
						}
						// the neighbour's sub-voxel just behind this point
						bool inVox = false;
						if (nb && !ghost) {
							double q[3] = {p[0], p[1], p[2]};
							q[ax] += step[d][ax] * 0.001;
							const int sx = std::min(7, std::max(0, (int)std::floor((q[0] - nx) * 8))), sy = std::min(7, std::max(0, (int)std::floor((q[1] - ny) * 8))),
							          sz = std::min(7, std::max(0, (int)std::floor((q[2] - nz) * 8)));
							inVox = (nb->bits[sy] >> (sz * 8 + sx)) & 1;
						}
						ex += inExact, vx += inVox, gap += inExact && !inVox, over += inVox && !inExact;
					}
				if (!ex && !vx) continue;
				++faces;
				const double px = 1600.0 / 6400.0;  // u^2 per sample
				totGap += gap * px, totOver += over * px, totExact += ex * px, totVox += vx * px;
				if (gap) ++facesGap;
				std::printf("cap %d %d %d %-5s -> %d %d %d: exact %6.1f u2 (%zu pieces, area %.1f), wall %6.1f u2, GAP %6.1f u2, over %6.1f u2%s\n", c[0], c[1], c[2], dname[d], nx, ny, nz, ex * px, parts.size(), area, vx * px, gap * px,
				            over * px, ghost ? " (neighbour ghost-only: no wall)" : nb ? "" : " (neighbour not exported)");
				if (std::getenv("CAPS_PIECES"))
					for (const auto &pp : parts) {
						double lo[3] = {1e9, 1e9, 1e9}, hi[3] = {-1e9, -1e9, -1e9};
						for (const auto &q : pp)
							for (int t = 0; t < 3; ++t) lo[t] = std::min(lo[t], q[t]), hi[t] = std::max(hi[t], q[t]);
						std::printf("    piece (%.1f %.1f %.1f)..(%.1f %.1f %.1f)\n", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
					}
				if (over > 0.5 * vx && vx > 0) {  // a wall where the BSP has no visible solid: what is there?
					const auto q = toSrc(nx + 0.5, ny + 0.5, nz + 0.5);
					const auto cl = world.Classify(q[0], q[1], q[2]);
					std::printf("    neighbour centre (%.0f %.0f %.0f): brush contents 0x%x, leaf 0x%x, in a prop %d, pad 0x%x\n", q[0], q[1], q[2], cl.contents, cl.leaf, cl.prop ? 1 : 0, nb ? nb->pad : 0);
					if (std::getenv("CAPS_DESCRIBE")) std::printf("%s", world.Describe(q[0], q[1], q[2], 20).c_str());
				}
			}
		std::printf("caps: %d faces, %d with a gap; exact %.0f u2, wall %.0f u2, gap %.1f u2, over %.1f u2\n", faces, facesGap, totExact, totVox, totGap, totOver);
		return 0;
	}
	// the cache
	std::vector<Region> regions;
	int off[3] = {0};
	{
		std::ifstream f(cachePath, std::ios::binary);
		if (!f) return std::fprintf(stderr, "cannot read %s\n", cachePath.c_str()), 1;
		uint32_t hdr[2], n;
		f.read((char *)hdr, 8);
		f.read((char *)off, 12);
		f.read((char *)&n, 4);
		if (hdr[0] != 0x43435043 || hdr[1] != 4) return std::fprintf(stderr, "not a v4 colcache\n"), 1;
		for (uint32_t i = 0; i < n; ++i) {
			Region r;
			int32_t k[3];
			uint32_t c;
			f.read((char *)k, 12);
			f.read((char *)&c, 4);
			r.blocks.resize(c);
			if (c) f.read((char *)r.blocks.data(), (std::streamsize)c * sizeof(ColBlock));
			if (!f) break;
			r.k[0] = k[0], r.k[1] = k[1], r.k[2] = k[2];
			regions.push_back(std::move(r));
		}
	}
	const double ox = std::floor(m.mcX + (0 - m.sx) / 40), oy = std::floor(m.mcY + (0 - m.sz) / 40), oz = std::floor(m.mcZ - (0 - m.sy) / 40);
	std::printf("cache: %zu regions, offset (%d %d %d); mapping gives (%.0f %.0f %.0f) -> %s\n", regions.size(), off[0], off[1], off[2], ox, oy, oz,
	            (ox == off[0] && oy == off[1] && oz == off[2]) ? "match" : "MISMATCH (regions compared at the cache's own keys; the delta below is applied)");
	// cache keys are at the cache's mapping; ours may differ by whole regions
	const int d[3] = {(int)ox - off[0], (int)oy - off[1], (int)oz - off[2]};
	if (d[0] % 8 || d[1] % 8 || d[2] % 8) return std::fprintf(stderr, "the mappings differ by a non-whole number of regions\n"), 1;

	auto voxelAt = [&](bool wantOld, double x, double y, double z) {  // is the sub-voxel containing a Source point solid?
		const double mcx = m.mcX + (x - m.sx) / 40, mcy = m.mcY + (z - m.sz) / 40, mcz = m.mcZ - (y - m.sy) / 40;
		const int bx = (int)std::floor(mcx), by = (int)std::floor(mcy), bz = (int)std::floor(mcz);
		const int sx = (int)std::floor((mcx - bx) * 8), sy = (int)std::floor((mcy - by) * 8), sz = (int)std::floor((mcz - bz) * 8);
		const int rx = (int)std::floor(bx / 8.0), ry = (int)std::floor(by / 8.0), rz = (int)std::floor(bz / 8.0);
		static std::map<std::tuple<int, int, int>, std::vector<ColBlock>> cacheNew;
		const std::vector<ColBlock> *blocks = nullptr;
		if (wantOld) {
			for (const Region &r : regions)
				if (r.k[0] + d[0] / 8 == rx && r.k[1] + d[1] / 8 == ry && r.k[2] + d[2] / 8 == rz) { blocks = &r.blocks; break; }
		} else {
			auto it = cacheNew.find({rx, ry, rz});
			if (it == cacheNew.end()) {
				std::vector<ColBlock> nb;
				world.VoxeliseRegion(m, opt, rx, ry, rz, nb);
				it = cacheNew.emplace(std::make_tuple(rx, ry, rz), std::move(nb)).first;
			}
			blocks = &it->second;
		}
		if (!blocks) return '?';
		for (const ColBlock &b : *blocks)
			if (b.x + (wantOld ? d[0] : 0) == bx && b.y + (wantOld ? d[1] : 0) == by && b.z + (wantOld ? d[2] : 0) == bz) return (b.bits[sy] >> (sz * 8 + sx) & 1) ? '#' : '.';
		return '.';
	};
	for (const std::string &cl : columns) {  // --column "x y zlo zhi": the voxels of a vertical column, old and new
		double c[4];
		if (std::sscanf(cl.c_str(), "%lf %lf %lf %lf", &c[0], &c[1], &c[2], &c[3]) != 4) continue;
		std::string o, n;
		for (double z = c[2]; z <= c[3]; z += 5) o += voxelAt(true, c[0], c[1], z), n += voxelAt(false, c[0], c[1], z);
		std::printf("column (%.1f %.1f) z %.0f..%.0f step 5 (bottom->top)\n  old %s\n  new %s\n", c[0], c[1], c[2], c[3], o.c_str(), n.c_str());
	}
	for (const std::string &pr : probes) {  // --probe "x y z": what is at a Source point
		double p[3];
		if (std::sscanf(pr.c_str(), "%lf %lf %lf", &p[0], &p[1], &p[2]) != 3) continue;
		const double mcx = m.mcX + (p[0] - m.sx) / 40, mcy = m.mcY + (p[2] - m.sz) / 40, mcz = m.mcZ - (p[1] - m.sy) / 40;
		const int bx = (int)std::floor(mcx), by = (int)std::floor(mcy), bz = (int)std::floor(mcz);
		std::printf("probe (%.1f %.1f %.1f): block %d %d %d, sub %d %d %d\n", p[0], p[1], p[2], bx, by, bz, (int)std::floor((mcx - bx) * 8), (int)std::floor((mcy - by) * 8), (int)std::floor((mcz - bz) * 8));
		const auto c = world.Classify(p[0], p[1], p[2]);
		std::printf("  contents 0x%x leaf 0x%x prop %d surface %.2f\n%s", c.contents, c.leaf, c.prop, c.surface, world.Describe(p[0], p[1], p[2], 8).c_str());
		// the block: old (cache) and new
		const int rx = (int)std::floor(bx / 8.0), ry = (int)std::floor(by / 8.0), rz = (int)std::floor(bz / 8.0);
		std::vector<ColBlock> nb;
		world.VoxeliseRegion(m, opt, rx, ry, rz, nb);
		const ColBlock *oldB = nullptr, *newB = nullptr;
		for (const Region &r : regions)
			if (r.k[0] + (int)ox - off[0] == rx * 1 * 0 + r.k[0] + ((int)ox - off[0]) && r.k[0] + ((int)ox - off[0]) / 8 == rx && r.k[1] + ((int)oy - off[1]) / 8 == ry && r.k[2] + ((int)oz - off[2]) / 8 == rz)
				for (const ColBlock &b : r.blocks)
					if (b.x + ((int)ox - off[0]) == bx && b.y + ((int)oy - off[1]) == by && b.z + ((int)oz - off[2]) == bz) oldB = &b;
		for (const ColBlock &b : nb)
			if (b.x == bx && b.y == by && b.z == bz) newB = &b;
		std::printf("  old block %s, new block %s; layers y=0..7 (z rows top->bottom as x bits), old | new:\n", oldB ? "present" : "absent", newB ? "present" : "absent");
		for (int y = 0; y < 8; ++y) {
			std::printf("   y%d ", y);
			for (int pass = 0; pass < 2; ++pass) {
				const ColBlock *b = pass ? newB : oldB;
				for (int z = 0; z < 8; ++z) {
					for (int x = 0; x < 8; ++x) std::printf("%c", b && (b->bits[y] >> (z * 8 + x) & 1) ? '#' : '.');
					std::printf(" ");
				}
				std::printf(pass ? "\n" : "| ");
			}
		}
	}
	// the diff
	long long oldVox = 0, newVox = 0, both = 0, oldOnly = 0, newOnly = 0, oldBlocks = 0, newBlocks = 0, bothBlocks = 0, identBlocks = 0, onlyOldBlocks = 0, onlyNewBlocks = 0;
	long long regionsDone = 0, regionsIdentical = 0, noDigBlocks = 0, wholeBlocks = 0, wholeDiggable = 0, partialDiggable = 0;
	double harvestMs = 0;
	struct Diff {
		double p[3];
		int blk[3];
		int sub[3];
		bool oldSolid;
	};
	std::vector<Diff> diffs;
	std::map<std::string, long long> cls;  // class -> voxels
	std::map<std::string, std::vector<Diff>> clsSamples;
	long long ri = -1;
	for (const Region &r : regions) {
		if (++ri < skip || (ri - skip) % stride) continue;
		if (!only.empty()) {
			bool hit = false;
			for (auto &k : only) hit |= (k[0] == r.k[0] + d[0] / 8 && k[1] == r.k[1] + d[1] / 8 && k[2] == r.k[2] + d[2] / 8);
			if (!hit) continue;
		}
		if (regionsDone >= maxRegions) break;
		if (regionsDone % 200 == 0) std::fprintf(stderr, "region %lld/%zu (%.1f s harvest)\n", ri, regions.size(), harvestMs / 1000);
		++regionsDone;
		const int rx = r.k[0] + d[0] / 8, ry = r.k[1] + d[1] / 8, rz = r.k[2] + d[2] / 8;
		std::vector<ColBlock> nb;
		auto ts = std::chrono::steady_clock::now();
		colharvest::RegionStats st;
		world.VoxeliseRegion(m, opt, rx, ry, rz, nb, &st);
		harvestMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ts).count();
		std::map<std::tuple<int, int, int>, const ColBlock *> om, nm;
		for (const ColBlock &b : r.blocks) om[{b.x + d[0], b.y + d[1], b.z + d[2]}] = &b;
		for (const ColBlock &b : nb) nm[{b.x, b.y, b.z}] = &b;
		bool ident = true;
		auto cmpBlock = [&](const std::tuple<int, int, int> &k, const ColBlock *o, const ColBlock *n) {
			for (int y = 0; y < 8; ++y) {
				const uint64_t ob = o ? o->bits[y] : 0, nbits = n ? n->bits[y] : 0;
				oldVox += __builtin_popcountll(ob), newVox += __builtin_popcountll(nbits);
				both += __builtin_popcountll(ob & nbits);
				oldOnly += __builtin_popcountll(ob & ~nbits), newOnly += __builtin_popcountll(nbits & ~ob);
				uint64_t df = ob ^ nbits;
				if (df) ident = false;
				while (df) {
					const int bit = __builtin_ctzll(df);
					df &= df - 1;
					const int sx = bit & 7, sz = bit >> 3;
					const double mcx = std::get<0>(k) + (sx + 0.5) / 8, mcy = std::get<1>(k) + (y + 0.5) / 8, mcz = std::get<2>(k) + (sz + 0.5) / 8;
					Diff dd;
					dd.p[0] = m.sx + (mcx - m.mcX) * 40, dd.p[1] = m.sy - (mcz - m.mcZ) * 40, dd.p[2] = m.sz + (mcy - m.mcY) * 40;
					dd.blk[0] = std::get<0>(k), dd.blk[1] = std::get<1>(k), dd.blk[2] = std::get<2>(k);
					dd.sub[0] = sx, dd.sub[1] = y, dd.sub[2] = sz;
					dd.oldSolid = (ob >> bit) & 1;
					// classify with the harvest's own knowledge
					const auto pr = world.Classify(dd.p[0], dd.p[1], dd.p[2]);
					std::string c = dd.oldSolid ? "old-only: " : "new-only: ";
					if (dd.oldSolid) {
						if (pr.contents & 0x200400b) c += (pr.surface < 3 ? "brush, within 3u of a surface" : "brush, deeper");
						else if (pr.contents & 0x10000) c += "playerclip";
						else if (pr.prop) c += "static prop hull";
						else if (pr.leaf & 0x200400b) c += "empty by brush but solid by tree leaf";
						else c += pr.surface < 6 ? "no brush, within 6u of a surface (box/edge rounding)" : "no brush, no prop (old data unexplained)";
					} else {
						if (pr.contents & 0x200400b) c += (pr.leaf & 0x200400b) ? "inside a brush (tree agrees)" : "inside a brush, but the tree leaf says empty";
						else if (pr.contents & 0x10000) c += "playerclip kept (floor over nothing)";
						else if (pr.prop) c += "static prop hull";
						else if (pr.leaf & 0x200400b) c += "solid void by tree leaf, > 20 u from any brush (the old box gate skipped the block)";
						else c += "thin-geometry fallback";
					}
					cls[c]++;
					auto &v = clsSamples[c];
					if ((int)v.size() < samples) v.push_back(dd);
				}
			}
		};
		for (auto &e : om) {
			auto it = nm.find(e.first);
			if (it == nm.end()) ++onlyOldBlocks;
			else ++bothBlocks;
			cmpBlock(e.first, e.second, it == nm.end() ? nullptr : it->second);
		}
		for (const ColBlock &b : nb) {
			bool whole = true;
			for (int y = 0; y < 8; ++y) whole &= b.bits[y] == ~0ull;
			noDigBlocks += (b.pad & 1) ? 1 : 0;
			wholeBlocks += whole ? 1 : 0;
			if (!(b.pad & 1)) (whole ? wholeDiggable : partialDiggable)++;
		}
		for (auto &e : nm)
			if (!om.count(e.first)) ++onlyNewBlocks, cmpBlock(e.first, nullptr, e.second);
		oldBlocks += (long long)om.size(), newBlocks += (long long)nm.size();
		long long idb = 0;
		for (auto &e : om) {
			auto it = nm.find(e.first);
			if (it != nm.end() && !std::memcmp(e.second->bits, it->second->bits, sizeof e.second->bits)) ++idb;
		}
		identBlocks += idb;
		if (ident) ++regionsIdentical;
	}
	auto pct = [](long long a, long long b) { return b ? 100.0 * (double)a / (double)b : 100.0; };
	std::printf("\n== %lld regions compared in %.1f s of harvest time (%.1f ms/region) ==\n", regionsDone, harvestMs / 1000, regionsDone ? harvestMs / regionsDone : 0.0);
	std::printf("regions identical: %lld (%.2f%%)\n", regionsIdentical, pct(regionsIdentical, regionsDone));
	std::printf("blocks: old %lld, new %lld, in both %lld, old only %lld, new only %lld; bit-identical %lld (%.3f%% of old, %.3f%% of new)\n", oldBlocks, newBlocks, bothBlocks, onlyOldBlocks,
	            onlyNewBlocks, identBlocks, pct(identBlocks, oldBlocks), pct(identBlocks, newBlocks));
	std::printf("voxels: old %lld, new %lld, both %lld, old only %lld, new only %lld\n", oldVox, newVox, both, oldOnly, newOnly);
	std::printf("voxel agreement: %.4f%% of old voxels are in new; %.4f%% of new voxels are in old; Jaccard %.4f%%\n", pct(both, oldVox), pct(both, newVox), pct(both, oldVox + newOnly));
	std::printf("dig: of the harvest's %lld blocks, %lld are next to the solid void (NoDig), %lld are wholly solid (%lld of those diggable: reveal as blocks), %lld partial surface blocks are diggable\n", newBlocks, noDigBlocks,
	            wholeBlocks, wholeDiggable, partialDiggable);
	std::printf("\nclasses of difference (voxels):\n");
	std::vector<std::pair<long long, std::string>> sorted;
	for (auto &e : cls) sorted.push_back({e.second, e.first});
	std::sort(sorted.rbegin(), sorted.rend());
	for (auto &e : sorted) {
		std::printf("  %9lld  %s\n", e.first, e.second.c_str());
		for (const Diff &dd : clsSamples[e.second])
			std::printf("             at Source (%.1f %.1f %.1f) block %d %d %d sub %d %d %d\n", dd.p[0], dd.p[1], dd.p[2], dd.blk[0], dd.blk[1], dd.blk[2], dd.sub[0], dd.sub[1], dd.sub[2]);
	}
	return 0;
}
