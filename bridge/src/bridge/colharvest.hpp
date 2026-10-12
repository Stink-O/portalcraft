// Portalcraft bridge (bridge #33): harvest-based collision export (REVIEW-freshEyes 3.2).
//
// Portal 2's static collision (world brushes, displacements, static-prop .phy hulls) is read once per map from the
// level file on disk and from the game's VPK archives, then voxelised on a worker thread into the same ColBlock
// regions the point-query path (collfield.cpp) produces. This file is plain level-file parsing: no engine, no game
// code, no decompiled code, and no dependency on the bridge's other headers, so the same source builds in the plugin
// (-m32) and in the offline equivalence tool (tools/colharvest, a host build).
//
// Formats: the Source BSP (VBSP v21) lumps, the VPK v1/v2 directory format, and the .phy collision files (a compact
// ledge tree of convex pieces). The convex-piece reading follows the format notes already used by
// crates/source_assets (collision.rs, phy.rs).
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include "skycraft_protocol.h"

namespace colharvest {

using ColBlock = skycraft::proto::ColBlock;

// Minecraft block <-> Source mapping, as coords.hpp's RegionMap: src = s + ((x - mcX) * 40, -(z - mcZ) * 40, (y - mcY) * 40).
struct Mapping {
	double mcX = 0, mcY = 0, mcZ = 0;
	double sx = 0, sy = 0, sz = 0;
	double upb = 40.0;  // Source units per block
};

struct Box {
	double mn[3] = {0, 0, 0}, mx[3] = {0, 0, 0};
	bool Contains(double x, double y, double z) const { return x > mn[0] && x < mx[0] && y > mn[1] && y < mx[1] && z > mn[2] && z < mx[2]; }
};

// A Minecraft block position packed into one key (the dug-cell sets).
inline uint64_t BlockKey(int x, int y, int z) {
	return (((uint64_t)(x + 33554432) & 0x3FFFFFFull) << 38) | (((uint64_t)(z + 33554432) & 0x3FFFFFFull) << 12) | ((uint64_t)(y + 2048) & 0xFFFull);
}
using DugSet = std::unordered_set<uint64_t>;

struct Options {
	// Playerclip (CONTENTS_PLAYERCLIP, Valve's invisible brushes): 1 = every playerclip voxel solid (what the point-query export
	// does in the game today, whatever its comments say: its entity trace also hits the world's playerclip), 0 = dropped except
	// as a floor over nothing (the intent of bridge #23, ClipFloorOverVoid).
	int clipMode = 1;
	bool staticProps = true;
	double propMargin = 0.0;   // static-prop hulls are tested this many units fatter (vphysics hulls carry a small skin)
	bool thinFallback = true;  // a block with no solid sub-voxel centre but a brush thinner than 5 u: box-test each sub-voxel
	bool leafPoint = true;     // point contents = the BSP tree's leaf contents (which hold the solid outside the map) plus the exact brushes, as the engine's world query; false: brushes only
	bool hasExclude = false;   // blocks whose centre lies inside `exclude` (Source) are not exported (the Minecraft region's own terrain)
	Box exclude;
	std::shared_ptr<const DugSet> dug;  // cells the Minecraft player dug out (BlockKey): nothing is exported there
	bool markNoDig = false;             // set kColBlockNoDig on blocks next to the solid void outside the map (bridge #38: off; digging goes through the void)
	double limit = 15800.0;             // (bridge #38) a block with any part beyond +-limit on a Source axis gets kColBlockLimit (the mod reveals it as bedrock): Portal 2 kills what leaves +-16384 u
	int nHoles = 0;  // open portals' hole boxes (Source): nothing is exported inside them
	Box holes[16];  // (bridge #32: one per portal slot, kPortalSlots)
};

// What a region's voxelisation counted (for logs and the equivalence tool).
struct RegionStats {
	int brushes = 0, props = 0, blocks = 0, thinBlocks = 0;
	int clipVoxels = 0, clipKept = 0, dugSkipped = 0, noDig = 0, limit = 0;
};

class World {
public:
	World();
	~World();
	// Parse the BSP at bspPath; gameDirs are searched in order for `<dir>/pak01_dir.vpk` (static-prop models). Returns false (and
	// sets err) if the BSP cannot be read. Log lines (what was parsed, what was missing) are appended to `log`.
	bool Load(const std::string &bspPath, const std::vector<std::string> &gameDirs, std::string &err, std::string *log = nullptr);

	bool loaded() const { return m_loaded; }
	const std::string &entities() const { return m_entities; }  // the entity lump text
	// Counts for logs.
	size_t brushCount() const { return m_worldBrushes.size(); }
	size_t propCount() const { return m_props.size(); }
	size_t dispCount() const { return m_dispCount; }
	size_t propsMissing() const { return m_propsMissing; }
	size_t modelCount() const { return m_nModels; }
	// World AABB of every harvested solid (Source).
	void Bounds(double mn[3], double mx[3]) const;

	// Voxelise region (rx, ry, rz) (8x8x8 blocks, Minecraft block coordinates) into ColBlocks in the order the point-query path
	// produces (y, then z, then x). Re-entrant: any number of threads may call it at once.
	void VoxeliseRegion(const Mapping &m, const Options &o, int rx, int ry, int rz, std::vector<ColBlock> &out, RegionStats *st = nullptr) const;

	// The world's contents bits (OR of every brush covering the point; props excluded) at a Source point; exact brush containment.
	int PointContents(double x, double y, double z) const;
	// Same through the BSP tree's leaf (the engine's own world query works that way).
	int LeafContents(double x, double y, double z) const;
	// Does the point lie inside a static prop's collision hull?
	bool InProp(double x, double y, double z) const;

	// (bridge #76) Cut caps: the parts of a convex planar polygon (Source points, in order) that lie inside VISIBLE host solid, exactly:
	// the BSP tree's solid leaves (the sealed map's outside included), every visible solid world brush (detail too) and the static props'
	// collision hulls. Ghost brushes (clip, invisible walls) and displacements are not counted. The polygon is classified a hair (0.01 u) along `into` (a unit
	// vector), so a polygon lying on a brush face takes the side `into` points to. The pieces are convex, disjoint, in the polygon's
	// plane and winding. Returns the summed area of the pieces.
	using Poly = std::vector<std::array<double, 3>>;
	double SolidParts(const Poly &poly, const double into[3], std::vector<Poly> &out) const;

	// Debug probe for the equivalence tool: everything this harvest knows about one Source point.
	struct Probe {
		int contents = 0;      // OR of the brush contents covering it
		int leaf = 0;          // the tree leaf's contents
		bool prop = false;     // inside a static prop hull
		double surface = 1e9;  // distance to the nearest brush surface (units, by the largest plane distance), any brush within 40 u
	};
	Probe Classify(double x, double y, double z) const;
	// Everything within `r` units of a point, one line each (brush index, contents, bounds, nearest plane distance; props with their ledge count).
	std::string Describe(double x, double y, double z, double r) const;

	struct Impl;

private:
	friend struct Impl;
	bool m_loaded = false;
	std::string m_entities;
	size_t m_dispCount = 0, m_propsMissing = 0, m_nModels = 0;
	std::vector<int> m_worldBrushes;
	std::vector<int> m_props;
	std::unique_ptr<Impl> m_impl;
};

// ---- VPK (read-only) ----------------------------------------------------------------------------------------------
class Vpk {
public:
	// Mounts <dir>/pak01_dir.vpk if present; returns false if it cannot be read.
	bool Mount(const std::string &dirVpkPath);
	bool Read(const std::string &path, std::vector<uint8_t> &out) const;  // path: "models/props/x.phy", lowercase, forward slashes
	size_t size() const { return m_entries.size(); }

private:
	struct Entry {
		uint32_t archive;  // 0x7fff: in the dir file
		uint32_t offset, length;
		std::vector<uint8_t> preload;
	};
	std::string m_base;  // path without "_dir.vpk"
	std::string m_dirPath;
	uint32_t m_dataStart = 0;
	std::unordered_map<std::string, Entry> m_entries;
};

}  // namespace colharvest
