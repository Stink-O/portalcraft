package dev.skycraft.world;

import java.util.ArrayList;
import java.util.List;

/**
 * Digging, the pure parts (no Minecraft state, unit tested): the section bitsets, "wholly inside the
 * host's solid" from a ColBlock's 512 sub-voxels, the no-dig rule, the ChamberMaterial to block
 * mapping, and the wall faces of a dug cell.
 *
 * Adapted from SkyCraft (MIT, chasmlol/SkyCraft): SkyDig's bit layout (bit x + 16z + 256y of a section),
 * reveal ("cells wholly inside the host's geometry become blocks": SkyDig.classify, 27 sample points, WHOLE = 25), its
 * "what is this deep behind a surface" material ladder (SkyDig.underground) and DigWalls (the hole is closed by
 * Minecraft faces). Here the host solid is the voxel field, not triangles; Portal 2's solid void outside the map is in the
 * field as solid, so it is deep rock like the inside of a Skyrim cliff (bridge #38).
 */
public final class DigMath {
	private DigMath() {
	}

	// Proto.CH_* (kept as literals so this class stays free of the protocol class for tests)
	public static final int CH_NONE = 0, CH_PANEL_WHITE = 1, CH_PANEL_BLACK = 2, CH_METAL = 3, CH_TILE = 4, CH_GLASS = 5, CH_CONCRETE = 6;
	public static final int CH_PORTALABLE_BIT = 1 << 8;
	/** ColBlock.pad bit 0 (bridge #33; no host sets it since #38): the cell is next to Portal 2's void. Ignored: digging goes through the void. */
	public static final int COL_BLOCK_NO_DIG = 1;
	/** ColBlock.pad bit 1 (bridge #38): the cell is at the dig limit (the edge of what Portal 2 can hold): bedrock. */
	public static final int COL_BLOCK_LIMIT = 2;
	/** Words in a section's dug bitset (4096 bits). */
	public static final int SECTION_WORDS = 64;

	// ---- section bitsets ----------------------------------------------------------------------------------

	/** Bit index of a block inside its section: x + 16 z + 256 y (x, y, z are block coordinates; low 4 bits count). */
	public static int bitIndex(int x, int y, int z) {
		return (x & 15) + 16 * (z & 15) + 256 * (y & 15);
	}

	public static boolean testBit(long[] bits, int bit) {
		return ((bits[bit >> 6] >>> (bit & 63)) & 1L) != 0;
	}

	public static void setBit(long[] bits, int bit) {
		bits[bit >> 6] |= 1L << (bit & 63);
	}

	public static void clearBit(long[] bits, int bit) {
		bits[bit >> 6] &= ~(1L << (bit & 63));
	}

	public static int count(long[] bits) {
		int n = 0;
		for (long w : bits) {
			n += Long.bitCount(w);
		}
		return n;
	}

	/** The 512-byte little-endian bitset of a REN_DUG message. */
	public static byte[] toBytes(long[] bits) {
		byte[] out = new byte[SECTION_WORDS * 8];
		for (int i = 0; i < SECTION_WORDS; i++) {
			long w = bits[i];
			for (int k = 0; k < 8; k++) {
				out[i * 8 + k] = (byte) (w >>> (8 * k));
			}
		}
		return out;
	}

	// ---- the host's voxel cell ----------------------------------------------------------------------------

	/** A ColBlock's 512 sub-voxels (eight bits[y] words) are all set: the cell is wholly inside the host's solid. */
	public static boolean isWhollySolid(long[] bits) {
		if (bits == null || bits.length < 8) {
			return false;
		}
		for (int y = 0; y < 8; y++) {
			if (bits[y] != -1L) {
				return false;
			}
		}
		return true;
	}

	public static boolean hasContent(long[] bits) {
		if (bits == null) {
			return false;
		}
		for (int y = 0; y < Math.min(8, bits.length); y++) {
			if (bits[y] != 0L) {
				return true;
			}
		}
		return false;
	}

	/** SkyCraft's rule (SkyDig.WHOLE): a cell becomes a block when this many of its 27 sample points are inside the host's geometry. */
	public static final int WHOLE = 25;
	/** The 27 sample points of a cell, per axis, as sub-voxel indices (SkyCraft: 1/6, 1/2, 5/6 of the cell; of 8 sub-voxels: 1, 4, 6). */
	private static final int[] SAMPLE = {1, 4, 6};

	/** How many of a cell's 27 sample points (SkyCraft's classify grid) lie in a solid sub-voxel. {@code bits} null = nothing there. */
	public static int samples(long[] bits) {
		if (bits == null || bits.length < 8) {
			return 0;
		}
		int n = 0;
		for (int sy : SAMPLE) {
			long w = bits[sy];
			if (w == 0L) {
				continue;
			}
			for (int sz : SAMPLE) {
				for (int sx : SAMPLE) {
					n += (int) ((w >>> (sz * 8 + sx)) & 1L);
				}
			}
		}
		return n;
	}

	/** The cell is inside the host's geometry as far as reveal is concerned (SkyCraft: 25 of 27 sample points). */
	public static boolean isInside(long[] bits) {
		return samples(bits) >= WHOLE;
	}

	/** Old rule (bridge #33): never open or reveal a cell whose ColBlock pad has COL_BLOCK_NO_DIG. No host sets it since bridge #38. */
	public static boolean isNoDig(int pad) {
		return (pad & COL_BLOCK_NO_DIG) != 0;
	}

	/** The cell is at the dig limit (the edge of what Portal 2 can hold): bedrock, never opened. */
	public static boolean isLimit(int pad) {
		return (pad & COL_BLOCK_LIMIT) != 0;
	}

	/** Whether a cell may be opened (mined by hand): it has host content and is not at the dig limit. */
	public static boolean canOpen(boolean hasContent, int pad) {
		return hasContent && !isLimit(pad);
	}

	/** Whether a cell becomes a revealed block: inside the host's geometry (25 of 27 sample points) and not already dug. */
	public static boolean canReveal(long[] bits, int pad, boolean alreadyDug) {
		return !alreadyDug && isInside(bits);
	}

	// ---- reveal: what is inside the host's geometry, and what it is made of (SkyDig.classify / underground) ----------------

	/** classify: the cell's region has not arrived (or its chunk is not loaded): try again later. */
	public static final int PENDING = -1;
	/** classify: not inside the host's geometry (open air, or only partly inside): the host's surface stays, the walls are drawn. */
	public static final int AIR = 0;
	/** What a revealed cell is made of beyond the chamber family CH_* (1..6): vanilla stone with ores, and bedrock at the limit. */
	public static final int BK_STONE = 7, BK_BEDROCK = 8;
	/** Depth (cells behind the host's surface) at which the material stops changing: stone from here on. */
	public static final int DEPTH_CAP = 8;
	/** Cells up to this depth are the chamber's own block family; then bare concrete down to {@link #STONE_DEPTH}; then stone. */
	public static final int FAMILY_DEPTH = 1, STONE_DEPTH = 4;

	/** The host's field as the reveal rule sees it (the mod's SkyCollision plus the dug set; unit tests bring their own). */
	public interface Field {
		/** Solid sample points of 27 (see {@link #samples}) in this cell, or -1 if the host has not sent its region yet. */
		int samples(int x, int y, int z);

		boolean dug(int x, int y, int z);

		boolean limit(int x, int y, int z);
	}

	private static final int[][] DIRS = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};

	/** A cell counts as solid rock for depth: dug (it was rock once, or the surface layer) or inside the host's geometry. */
	private static boolean rock(Field f, int x, int y, int z) {
		return f.dug(x, y, z) || f.samples(x, y, z) >= WHOLE;
	}

	/**
	 * How many solid cells lie between this cell and the nearest open space (a cell not inside the host's geometry and not dug), over the
	 * six axes, at most {@link #DEPTH_CAP}: 0 = touching the open, like the layer under a surface. SkyCraft measures how far behind a
	 * surface a point is; this is the same on the voxel field. Dug cells are rock for this (a tunnel does not make its walls shallow), the
	 * open of the chamber the tunnel leads back to does.
	 */
	public static int depth(Field f, int x, int y, int z) {
		int best = DEPTH_CAP;
		for (int[] d : DIRS) {
			for (int k = 1; k <= best; k++) {
				if (!rock(f, x + d[0] * k, y + d[1] * k, z + d[2] * k)) {
					best = k - 1;
					break;
				}
			}
			if (best == 0) {
				break;
			}
		}
		return best;
	}

	/** The block family a surface layer is made of: CH_NONE is the white panel; glass is not a rock, so it digs into concrete. */
	public static int layerFamily(int family) {
		int ch = materialOf(family);
		return ch == CH_NONE ? CH_PANEL_WHITE : ch == CH_GLASS ? CH_CONCRETE : ch;
	}

	/** What a cell this deep behind the surface is made of: the chamber's own block for the first layers, concrete, then stone (SkyCraft: grass has dirt under it, then stone). */
	public static int kindAtDepth(int family, int depth) {
		if (depth <= FAMILY_DEPTH) {
			return layerFamily(family);
		}
		return depth < STONE_DEPTH ? CH_CONCRETE : BK_STONE;
	}

	/**
	 * SkyCraft's classify on the voxel field: PENDING (the cell's region is not here yet), AIR (not wholly inside the host's geometry:
	 * 25 of its 27 sample points are not solid), else the kind of block it becomes: BK_BEDROCK at the dig limit, else by depth
	 * ({@link #kindAtDepth}) from the chamber family {@code family} of the surface being dug.
	 */
	public static int classify(Field f, int x, int y, int z, int family) {
		int n = f.samples(x, y, z);
		if (n < 0) {
			return PENDING;
		}
		if (n < WHOLE) {
			return AIR;
		}
		if (f.limit(x, y, z)) {
			return BK_BEDROCK;
		}
		return kindAtDepth(family, depth(f, x, y, z));
	}

	// ---- ChamberMaterial -> block family ------------------------------------------------------------------

	public static final String[] BLOCK_NAMES = {
		"chamber_panel_white", // 0 (not known) and 1
		"chamber_panel_white",
		"chamber_panel_black",
		"chamber_metal",
		"chamber_tile",
		"chamber_glass",
		"chamber_concrete",
	};

	/** The CH_* value (0..6) a HostPlayer.digMaterial stands for: the low byte; unknown values are 0 (white panel). */
	public static int materialOf(int digMaterial) {
		int ch = digMaterial & 0xFF;
		return ch >= 0 && ch <= CH_CONCRETE ? ch : CH_NONE;
	}

	public static String blockName(int material) {
		return BLOCK_NAMES[materialOf(material)];
	}

	/** Name of a reveal kind (CH_* 1..6, BK_STONE, BK_BEDROCK) for logs. */
	public static String kindName(int kind) {
		return kind == BK_STONE ? "stone" : kind == BK_BEDROCK ? "bedrock" : blockName(kind);
	}

	/** Portalable block families (portals stick to them): the white panel and the tile. */
	public static boolean portalable(int material) {
		int ch = materialOf(material);
		return ch == CH_NONE || ch == CH_PANEL_WHITE || ch == CH_TILE;
	}

	/** Hardness (strength) of a family, roughly stone. */
	public static float hardness(int material) {
		return switch (materialOf(material)) {
			case CH_METAL -> 3.0F;
			case CH_GLASS -> 0.3F;
			default -> 1.5F;
		};
	}

	// ---- wall faces of a dug cell -------------------------------------------------------------------------

	/**
	 * The in-plane axes (0 = x, 1 = y, 2 = z) of the face between a dug cell and its neighbour in direction
	 * {@code dir} (ordinal of Direction: 0 down, 1 up, 2 north, 3 south, 4 west, 5 east), ordered so that
	 * A x B points into the dug cell (counter-clockwise seen from inside it): {A, B}.
	 */
	public static int[] faceAxes(int dir) {
		return switch (dir) {
			case 4 -> new int[] {1, 2}; // west neighbour: normal +x
			case 5 -> new int[] {2, 1}; // east: -x
			case 0 -> new int[] {2, 0}; // down: +y
			case 1 -> new int[] {0, 2}; // up: -y
			case 2 -> new int[] {0, 1}; // north: +z
			default -> new int[] {1, 0}; // south: -z
		};
	}

	/** Axis (0 x, 1 y, 2 z) of a Direction ordinal. */
	public static int axisOf(int dir) {
		return dir == 0 || dir == 1 ? 1 : dir == 2 || dir == 3 ? 2 : 0;
	}

	/** Step sign (+1 or -1) of a Direction ordinal. */
	public static int stepOf(int dir) {
		return dir == 1 || dir == 3 || dir == 5 ? 1 : -1;
	}

	/**
	 * Which sub-voxel faces of the neighbour cell's boundary layer next to the dug cell are solid. The neighbour
	 * (cell + dir) has its eight bits[y] words; the layer next to the dug cell is its first one in the step
	 * direction (x = 0 for east) or last (x = 7 for west). Result: bit a + 8 b of a long, a along faceAxes[0],
	 * b along faceAxes[1], each 0..7 in block-coordinate order.
	 */
	public static long boundaryMask(long[] bits, int dir) {
		if (bits == null || bits.length < 8) {
			return 0L;
		}
		int[] ab = faceAxes(dir);
		int axis = axisOf(dir);
		int layer = stepOf(dir) > 0 ? 0 : 7;
		long mask = 0L;
		int[] c = new int[3];
		for (int b = 0; b < 8; b++) {
			for (int a = 0; a < 8; a++) {
				c[axis] = layer;
				c[ab[0]] = a;
				c[ab[1]] = b;
				// bits[y] bit (z * 8 + x)
				if (((bits[c[1]] >>> (c[2] * 8 + c[0])) & 1L) != 0) {
					mask |= 1L << (a + 8 * b);
				}
			}
		}
		return mask;
	}

	/** Greedy rectangles over an 8x8 mask (bit a + 8 b): each int[] is {a0, b0, width, height}. */
	public static List<int[]> mergeRects(long mask) {
		List<int[]> out = new ArrayList<>();
		long left = mask;
		for (int b = 0; b < 8; b++) {
			for (int a = 0; a < 8; a++) {
				if (((left >>> (a + 8 * b)) & 1L) == 0) {
					continue;
				}
				int w = 1;
				while (a + w < 8 && ((left >>> (a + w + 8 * b)) & 1L) != 0) {
					w++;
				}
				int h = 1;
				outer:
				while (b + h < 8) {
					for (int i = 0; i < w; i++) {
						if (((left >>> (a + i + 8 * (b + h))) & 1L) == 0) {
							break outer;
						}
					}
					h++;
				}
				for (int j = 0; j < h; j++) {
					for (int i = 0; i < w; i++) {
						left &= ~(1L << (a + i + 8 * (b + j)));
					}
				}
				out.add(new int[] {a, b, w, h});
			}
		}
		return out;
	}
}
