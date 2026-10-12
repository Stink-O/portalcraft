package dev.skycraft.world;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.List;
import org.junit.jupiter.api.Test;

class DigMathTest {
	private static long[] full() {
		return new long[] {-1L, -1L, -1L, -1L, -1L, -1L, -1L, -1L};
	}

	@Test
	void bitIndexMatchesTheProtocol() {
		// bit x + 16 z + 256 y, x y z the block position inside the section (low 4 bits, negatives wrap)
		assertEquals(0, DigMath.bitIndex(0, 0, 0));
		assertEquals(1, DigMath.bitIndex(1, 0, 0));
		assertEquals(16, DigMath.bitIndex(0, 0, 1));
		assertEquals(256, DigMath.bitIndex(0, 1, 0));
		assertEquals(15 + 16 * 15 + 256 * 15, DigMath.bitIndex(15, 15, 15));
		assertEquals(4095, DigMath.bitIndex(-1, -1, -1));
		assertEquals(DigMath.bitIndex(3, 5, 7), DigMath.bitIndex(3 + 16 * 9, 5 - 16 * 4, 7 + 16 * 100));
		assertEquals(DigMath.bitIndex(15, 0, 0), DigMath.bitIndex(-1, 16, 32));
	}

	@Test
	void setTestClearCountAndBytes() {
		long[] bits = new long[DigMath.SECTION_WORDS];
		int b = DigMath.bitIndex(5, 9, 12);
		assertFalse(DigMath.testBit(bits, b));
		DigMath.setBit(bits, b);
		DigMath.setBit(bits, 0);
		DigMath.setBit(bits, 4095);
		assertTrue(DigMath.testBit(bits, b));
		assertEquals(3, DigMath.count(bits));
		byte[] bytes = DigMath.toBytes(bits);
		assertEquals(512, bytes.length);
		assertEquals(1, bytes[0] & 1); // bit 0: byte 0 bit 0
		assertEquals(0x80, bytes[511] & 0x80); // bit 4095: last byte, top bit
		assertEquals(1 << (b & 7), bytes[b >> 3] & (1 << (b & 7)));
		DigMath.clearBit(bits, b);
		assertFalse(DigMath.testBit(bits, b));
		assertEquals(2, DigMath.count(bits));
	}

	@Test
	void whollyInsideNeedsAll512SubVoxels() {
		assertTrue(DigMath.isWhollySolid(full()));
		long[] almost = full();
		almost[3] &= ~(1L << 17);
		assertFalse(DigMath.isWhollySolid(almost));
		assertFalse(DigMath.isWhollySolid(new long[8]));
		assertFalse(DigMath.isWhollySolid(null));
		assertFalse(DigMath.isWhollySolid(new long[4]));
		assertTrue(DigMath.hasContent(almost));
		assertFalse(DigMath.hasContent(new long[8]));
	}

	@Test
	void limitRuleReplacesTheVoidRule() {
		// bridge #38: the void is deep rock; the only cell never opened is one at the dig limit (ColBlock.pad bit 1)
		assertTrue(DigMath.isLimit(2));
		assertTrue(DigMath.isLimit(3));
		assertFalse(DigMath.isLimit(0));
		assertFalse(DigMath.isLimit(1));
		assertTrue(DigMath.canOpen(true, 0));
		assertTrue(DigMath.canOpen(true, DigMath.COL_BLOCK_NO_DIG)); // an old host's no-dig bit is ignored
		assertFalse(DigMath.canOpen(true, DigMath.COL_BLOCK_LIMIT));
		assertFalse(DigMath.canOpen(false, 0));
		assertTrue(DigMath.canReveal(full(), 0, false));
		assertTrue(DigMath.canReveal(full(), DigMath.COL_BLOCK_NO_DIG, false));
		assertFalse(DigMath.canReveal(full(), 0, true));
		long[] partial = full();
		partial[0] = 0;
		partial[1] = 0;
		partial[2] = 0; // the three sample rows 1, 4, 6: only 4 and 6 are left, 18 of 27
		assertFalse(DigMath.canReveal(partial, 0, false));
	}

	@Test
	void revealNeedsTwentyFiveOfTwentySevenSamples() {
		assertEquals(27, DigMath.samples(full()));
		assertEquals(0, DigMath.samples(new long[8]));
		assertEquals(0, DigMath.samples(null));
		// the samples are the sub-voxels 1, 4, 6 on each axis: clearing sub-voxel (x 1, y 1, z 1) loses one, (x 0, y 0, z 0) loses none
		long[] a = full();
		a[0] &= ~1L; // (0, 0, 0): not a sample
		assertEquals(27, DigMath.samples(a));
		assertTrue(DigMath.isInside(a));
		a[1] &= ~(1L << (1 * 8 + 1)); // (x 1, y 1, z 1): a sample
		assertEquals(26, DigMath.samples(a));
		a[4] &= ~(1L << (4 * 8 + 4)); // the centre
		assertEquals(25, DigMath.samples(a));
		assertTrue(DigMath.isInside(a), "25 of 27 is a block (SkyCraft's WHOLE)");
		a[6] &= ~(1L << (6 * 8 + 6));
		assertEquals(24, DigMath.samples(a));
		assertFalse(DigMath.isInside(a));
		// a floor whose surface is at the middle of the cell (top half empty): 18 of 27 stay the host's
		long[] half = new long[8];
		for (int y = 0; y < 4; y++) {
			half[y] = -1L;
		}
		assertEquals(9, DigMath.samples(half)); // y samples 1 only... 4 and 6 are in the empty half: 9 of 27
		assertFalse(DigMath.isInside(half));
	}

	/** A field of a few solid cells, for the reveal rule. Everything unset is air; cells with y < 0 are void (wholly solid). */
	private static final class Fake implements DigMath.Field {
		final java.util.Set<String> dug = new java.util.HashSet<>();
		final java.util.Set<String> air = new java.util.HashSet<>();
		final java.util.Set<String> partial = new java.util.HashSet<>();
		final java.util.Set<String> limit = new java.util.HashSet<>();
		final java.util.Set<String> unknown = new java.util.HashSet<>();

		static String k(int x, int y, int z) {
			return x + "," + y + "," + z;
		}

		@Override
		public int samples(int x, int y, int z) {
			String k = k(x, y, z);
			if (this.unknown.contains(k)) {
				return -1;
			}
			if (this.air.contains(k)) {
				return 0;
			}
			if (this.partial.contains(k)) {
				return 13;
			}
			return y < 0 ? 27 : 0; // the floor is y = 0 (a partial layer), below it solid void, above it air
		}

		@Override
		public boolean dug(int x, int y, int z) {
			return this.dug.contains(k(x, y, z));
		}

		@Override
		public boolean limit(int x, int y, int z) {
			return this.limit.contains(k(x, y, z));
		}
	}

	@Test
	void theVoidBehindAFloorIsDeepRockWithALadderOfMaterials() {
		Fake f = new Fake();
		// the floor layer: cell (0, 0, 0) is partly solid (the surface), (0, -1, 0) and below are void; open the floor cell
		f.partial.add(Fake.k(0, 0, 0));
		f.dug.add(Fake.k(0, 0, 0));
		// not-inside: the partial neighbours of the floor cell
		f.partial.add(Fake.k(1, 0, 0));
		assertEquals(DigMath.AIR, DigMath.classify(f, 1, 0, 0, DigMath.CH_PANEL_WHITE));
		// (0, -1, 0): void directly under the opened floor: its up neighbour is the dug floor cell (rock), then the hall's air at y 1: depth 1
		assertEquals(1, DigMath.depth(f, 0, -1, 0));
		assertEquals(DigMath.CH_PANEL_BLACK, DigMath.classify(f, 0, -1, 0, DigMath.CH_PANEL_BLACK));
		// a tunnel straight down: each cell dug as the player goes, the one under it classified; the cell n under the hall's floor is n deep
		// (the dug cells above it are rock, the hall's air above them is the open): the chamber's block, concrete, then stone
		for (int n = 1; n <= 8; n++) {
			int want = n <= 1 ? DigMath.CH_METAL : n < 4 ? DigMath.CH_CONCRETE : DigMath.BK_STONE;
			assertEquals(Math.min(n, DigMath.DEPTH_CAP), DigMath.depth(f, 0, -n, 0), "depth of cell " + (-n));
			assertEquals(want, DigMath.classify(f, 0, -n, 0, DigMath.CH_METAL), "cell " + (-n));
			f.dug.add(Fake.k(0, -n, 0));
		}
		// and far down it is stone with no air in reach
		assertEquals(DigMath.DEPTH_CAP, DigMath.depth(f, 5, -40, 5));
		assertEquals(DigMath.BK_STONE, DigMath.classify(f, 5, -40, 5, DigMath.CH_PANEL_WHITE));
	}

	@Test
	void sideCellsOfATunnelAreNotShallowBecauseTheTunnelIsDug() {
		Fake f = new Fake();
		// a tunnel 5 cells deep going down from the hall: the wall cell beside its bottom is deep, not touching the tunnel's open
		for (int y = 0; y >= -5; y--) {
			f.dug.add(Fake.k(0, y, 0));
		}
		f.partial.add(Fake.k(0, 0, 0));
		assertEquals(DigMath.BK_STONE, DigMath.classify(f, 1, -5, 0, DigMath.CH_PANEL_WHITE), "beside the tunnel's bottom, 6 cells below the hall");
		assertEquals(DigMath.CH_PANEL_WHITE, DigMath.classify(f, 1, -1, 0, DigMath.CH_PANEL_WHITE) , "beside the tunnel just under the floor");
	}

	@Test
	void limitCellsAreBedrockAndUnknownCellsWait() {
		Fake f = new Fake();
		f.limit.add(Fake.k(0, -3, 0));
		assertEquals(DigMath.BK_BEDROCK, DigMath.classify(f, 0, -3, 0, DigMath.CH_PANEL_WHITE));
		f.unknown.add(Fake.k(0, -4, 0));
		assertEquals(DigMath.PENDING, DigMath.classify(f, 0, -4, 0, DigMath.CH_PANEL_WHITE));
		// air stays air (the host's surface stays)
		assertEquals(DigMath.AIR, DigMath.classify(f, 0, 3, 0, DigMath.CH_PANEL_WHITE));
		// a cell next to an unknown region still gets a depth (unknown counts as open: the family, not stone)
		assertEquals(DigMath.CH_PANEL_WHITE, DigMath.kindAtDepth(DigMath.CH_NONE, 0));
		assertEquals(DigMath.CH_CONCRETE, DigMath.kindAtDepth(DigMath.CH_GLASS, 0), "glass is no rock: it digs into concrete");
	}

	@Test
	void materialToBlockMapping() {
		assertEquals("chamber_panel_white", DigMath.blockName(0));
		assertEquals("chamber_panel_white", DigMath.blockName(1));
		assertEquals("chamber_panel_black", DigMath.blockName(2));
		assertEquals("chamber_metal", DigMath.blockName(3));
		assertEquals("chamber_tile", DigMath.blockName(4));
		assertEquals("chamber_glass", DigMath.blockName(5));
		assertEquals("chamber_concrete", DigMath.blockName(6));
		// only the low byte is the material; bit 8 is the portalable flag
		assertEquals(1, DigMath.materialOf(1 | DigMath.CH_PORTALABLE_BIT));
		assertEquals("chamber_metal", DigMath.blockName(3 | DigMath.CH_PORTALABLE_BIT));
		assertEquals(0, DigMath.materialOf(7));
		assertEquals(0, DigMath.materialOf(200));
		assertTrue(DigMath.portalable(1));
		assertTrue(DigMath.portalable(4));
		assertTrue(DigMath.portalable(0));
		assertFalse(DigMath.portalable(2));
		assertFalse(DigMath.portalable(3));
		assertFalse(DigMath.portalable(5));
		assertFalse(DigMath.portalable(6));
		assertEquals(3.0F, DigMath.hardness(3));
		assertEquals(0.3F, DigMath.hardness(5));
		assertEquals(1.5F, DigMath.hardness(2));
	}

	@Test
	void boundaryMaskOfAFullCellIsTheFullFace() {
		for (int dir = 0; dir < 6; dir++) {
			assertEquals(-1L, DigMath.boundaryMask(full(), dir), "dir " + dir);
		}
		assertEquals(0L, DigMath.boundaryMask(new long[8], 4));
		assertEquals(0L, DigMath.boundaryMask(null, 4));
	}

	@Test
	void boundaryMaskTakesTheLayerNextToTheDugCell() {
		// A single sub-voxel at (x 0, y 3, z 5): bits[3] bit 5 * 8 + 0.
		long[] bits = new long[8];
		bits[3] |= 1L << (5 * 8 + 0);
		// the dug cell is west of this cell: dir (from the dug cell to this one) EAST (5): the x = 0 layer matters
		long east = DigMath.boundaryMask(bits, 5);
		// faceAxes(EAST) = {z, y}: a = z (5), b = y (3)
		assertEquals(1L << (5 + 8 * 3), east);
		// seen from the east (dir WEST = 4) the layer is x = 7: empty
		assertEquals(0L, DigMath.boundaryMask(bits, 4));
		// seen from below (dir UP = 1: this cell is above the dug cell): the y = 0 layer: empty; from above (DOWN = 0): y = 7: empty
		assertEquals(0L, DigMath.boundaryMask(bits, 1));
		assertEquals(0L, DigMath.boundaryMask(bits, 0));
		// north neighbour (dir NORTH = 2): this cell is north of the dug one; the layer next to it is z = 7: empty; SOUTH (3): z = 0: empty
		assertEquals(0L, DigMath.boundaryMask(bits, 2));
		assertEquals(0L, DigMath.boundaryMask(bits, 3));
		// a voxel at the y = 0 layer is seen from below
		long[] floor = new long[8];
		floor[0] |= 1L << (2 * 8 + 6); // z 2, x 6
		long up = DigMath.boundaryMask(floor, 1); // faceAxes(UP) = {x, z}
		assertEquals(1L << (6 + 8 * 2), up);
		// the same voxel seen from a dug cell below... a dug cell ABOVE a floor cell: dir DOWN: layer y = 7 of the floor cell
		long[] top = new long[8];
		top[7] |= 1L << (4 * 8 + 1); // z 4, x 1
		long down = DigMath.boundaryMask(top, 0); // faceAxes(DOWN) = {z, x}: a = z 4, b = x 1
		assertEquals(1L << (4 + 8 * 1), down);
	}

	@Test
	void faceAxesPointIntoTheDugCell() {
		// A x B must equal the normal into the dug cell, i.e. minus the step of dir
		for (int dir = 0; dir < 6; dir++) {
			int[] ab = DigMath.faceAxes(dir);
			int axis = DigMath.axisOf(dir);
			int third = 3 - ab[0] - ab[1];
			assertEquals(axis, third, "dir " + dir);
			boolean cyclic = (ab[1] - ab[0] + 3) % 3 == 1; // x->y->z->x: A x B = +third axis
			int normalSign = cyclic ? 1 : -1;
			assertEquals(-DigMath.stepOf(dir), normalSign, "dir " + dir);
		}
	}

	@Test
	void mergeRectsCoversTheMaskExactly() {
		assertEquals(List.of(), DigMath.mergeRects(0L).stream().map(r -> r).toList());
		List<int[]> whole = DigMath.mergeRects(-1L);
		assertEquals(1, whole.size());
		assertArrayEquals(new int[] {0, 0, 8, 8}, whole.get(0));
		// an L shape: row 0 full, column 0 full
		long l = 0xFFL;
		for (int b = 1; b < 8; b++) {
			l |= 1L << (8 * b);
		}
		List<int[]> rects = DigMath.mergeRects(l);
		long covered = 0;
		for (int[] r : rects) {
			for (int j = 0; j < r[3]; j++) {
				for (int i = 0; i < r[2]; i++) {
					long bit = 1L << (r[0] + i + 8 * (r[1] + j));
					assertEquals(0L, covered & bit, "overlap");
					covered |= bit;
				}
			}
		}
		assertEquals(l, covered);
		assertEquals(2, rects.size());
		// a checkerboard cannot merge
		long checker = 0xAA55_AA55_AA55_AA55L;
		assertEquals(32, DigMath.mergeRects(checker).size());
	}
}
