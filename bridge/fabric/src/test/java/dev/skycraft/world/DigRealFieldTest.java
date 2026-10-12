package dev.skycraft.world;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import org.junit.jupiter.api.Test;

/**
 * The reveal rule on the real geometry of pc_testbed's entry hall (bridge #38): src/test/resources/pc_testbed_hall.field is the harvest's
 * export (tools/colharvest --dump-field; the solid void behind the walls and floor included) of MC blocks -360,426,386 .. -340,452,402.
 * A player digging the hall's floor and the pillar's wall is simulated: the chain "open a cell, reveal around it, break each revealed block,
 * reveal around that" that SkyDig runs, with the same DigMath.classify.
 */
class DigRealFieldTest {
	private static final int X0 = -360, Y0 = 426, Z0 = 386, X1 = -340, Y1 = 452, Z1 = 402;

	private static final class Hall implements DigMath.Field {
		final Map<String, long[]> cells = new HashMap<>();
		final Set<String> dug = new HashSet<>();
		final Set<String> open = new HashSet<>(); // dug cells with no block in them: the opened cells and the revealed blocks that were broken

		static String k(int x, int y, int z) {
			return x + "," + y + "," + z;
		}

		Hall() throws IOException {
			try (BufferedReader r = new BufferedReader(new InputStreamReader(DigRealFieldTest.class.getResourceAsStream("/pc_testbed_hall.field"), StandardCharsets.UTF_8))) {
				String line;
				while ((line = r.readLine()) != null) {
					if (line.startsWith("#")) {
						continue;
					}
					String[] t = line.split(" ");
					long[] bits = new long[8];
					if (t[4].equals("F")) {
						java.util.Arrays.fill(bits, -1L);
					} else {
						for (int i = 0; i < 8; i++) {
							bits[i] = Long.parseUnsignedLong(t[4 + i], 16);
						}
					}
					this.cells.put(k(Integer.parseInt(t[0]), Integer.parseInt(t[1]), Integer.parseInt(t[2])), bits);
				}
			}
		}

		boolean inBox(int x, int y, int z) {
			return x >= X0 && x <= X1 && y >= Y0 && y <= Y1 && z >= Z0 && z <= Z1;
		}

		@Override
		public int samples(int x, int y, int z) {
			if (!inBox(x, y, z)) {
				return -1;
			}
			return DigMath.samples(this.cells.get(k(x, y, z)));
		}

		@Override
		public boolean dug(int x, int y, int z) {
			return this.dug.contains(k(x, y, z));
		}

		@Override
		public boolean limit(int x, int y, int z) {
			return false;
		}
	}

	private static final int[][] DIRS = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};

	/** The chain: dig a cell, reveal its neighbours; every revealed block is then broken too (as a player tunnelling would), up to a budget. */
	private static Map<String, Integer> dig(Hall h, int x, int y, int z, int family, int breakBudget, boolean breakRevealed) {
		Map<String, Integer> kinds = new HashMap<>();
		ArrayDeque<int[]> queue = new ArrayDeque<>();
		h.dug.add(Hall.k(x, y, z));
		h.open.add(Hall.k(x, y, z));
		queue.add(new int[] {x, y, z});
		int broken = 0;
		while (!queue.isEmpty()) {
			int[] c = queue.poll();
			for (int dx = -1; dx <= 1; dx++) {
				for (int dy = -1; dy <= 1; dy++) {
					for (int dz = -1; dz <= 1; dz++) {
						int nonZero = (dx != 0 ? 1 : 0) + (dy != 0 ? 1 : 0) + (dz != 0 ? 1 : 0);
						if (nonZero == 0 || nonZero == 3) {
							continue; // the 6 face and 12 edge neighbours, as SkyDig.revealAround
						}
						int nx = c[0] + dx, ny = c[1] + dy, nz = c[2] + dz;
						if (h.dug(nx, ny, nz)) {
							continue;
						}
						int kind = DigMath.classify(h, nx, ny, nz, family);
						if (kind == DigMath.PENDING || kind == DigMath.AIR) {
							continue;
						}
						h.dug.add(Hall.k(nx, ny, nz));
						kinds.put(Hall.k(nx, ny, nz), kind);
						if (breakRevealed && broken < breakBudget && nonZero == 1) {
							broken++;
							h.open.add(Hall.k(nx, ny, nz));
							queue.add(new int[] {nx, ny, nz});
						}
					}
				}
			}
		}
		return kinds;
	}

	@Test
	void theHallFloorIsAPartialLayerOverSolidVoid() throws IOException {
		Hall h = new Hall();
		// the floor layer (y 443) is partial (about a third solid), everything under it solid, the air above it nothing
		assertEquals(0, h.samples(-352, 446, 398));
		int floor = h.samples(-352, 443, 398);
		assertTrue(floor > 0 && floor < DigMath.WHOLE, "floor layer " + floor);
		assertEquals(27, h.samples(-352, 442, 398));
		assertEquals(27, h.samples(-352, 430, 398));
	}

	@Test
	void openingTheHallFloorRevealsTheVoidBelowAndDigsOnThroughIt() throws IOException {
		Hall h = new Hall();
		int family = DigMath.CH_PANEL_BLACK;
		// open the floor cell (the player's mining): the neighbours around it
		Map<String, Integer> first = dig(h, -352, 443, 398, family, 0, false);
		// directly under it: the void, one deep: the chamber's own block. The four floor-layer neighbours are partial: the host's surface stays.
		// (and the four edge cells under the floor cell's edges: they close the crack the plugin's 0.06 u face pull-back leaves where two walls meet)
		assertEquals(DigMath.CH_PANEL_BLACK, first.get(Hall.k(-352, 442, 398)));
		assertEquals(5, first.size());
		// the old rule (all 512 sub-voxels) would have revealed the same cell here, but the NoDig refusal kept anything from being opened next to the void;
		// the real change is going on: the player breaks the revealed block and the next ring appears, down to the bottom of the box
		Hall d = new Hall();
		Map<String, Integer> all = dig(d, -352, 443, 398, family, 1000000, true);
		int[] byKind = new int[9];
		for (int kind : all.values()) {
			byKind[kind]++;
		}
		// every kind of the ladder shows up: family, concrete, stone; nothing is bedrock (no limit here)
		assertTrue(byKind[DigMath.CH_PANEL_BLACK] > 0, "family blocks");
		assertTrue(byKind[DigMath.CH_CONCRETE] > 0, "concrete");
		assertTrue(byKind[DigMath.BK_STONE] > 0, "stone");
		assertEquals(0, byKind[DigMath.BK_BEDROCK]);
		// closure: no open cell (the opened one, a broken block) has a face into rock that is not dug (the tunnel is closed by blocks), unless
		// the cell is outside the box (unknown)
		for (String key : d.open) {
			String[] t = key.split(",");
			int x = Integer.parseInt(t[0]), y = Integer.parseInt(t[1]), z = Integer.parseInt(t[2]);
			for (int[] dir : DIRS) {
				int nx = x + dir[0], ny = y + dir[1], nz = z + dir[2];
				if (d.dug(nx, ny, nz) || !d.inBox(nx, ny, nz)) {
					continue;
				}
				assertFalse(DigMath.isInside(d.cells.get(Hall.k(nx, ny, nz))), "an open face into rock at " + nx + " " + ny + " " + nz);
			}
		}
		// the material under the floor goes family (depth 1) -> concrete (2, 3) -> stone (4+): spot-check the shaft's own column
		assertEquals(Integer.valueOf(DigMath.CH_PANEL_BLACK), all.get(Hall.k(-352, 442, 398)));
		assertEquals(Integer.valueOf(DigMath.CH_CONCRETE), all.get(Hall.k(-352, 441, 398)));
		assertEquals(Integer.valueOf(DigMath.CH_CONCRETE), all.get(Hall.k(-352, 440, 398)));
		assertEquals(Integer.valueOf(DigMath.BK_STONE), all.get(Hall.k(-352, 439, 398)));
	}

	@Test
	void aTunnelThroughThePillarsThinWallNeedsNoRevealAndIsClosedByWalls() throws IOException {
		Hall h = new Hall();
		// dig6's tunnel: cells -348 444 395, 444 394 ... through the pillar's wall (two partial cells thick, air on both sides)
		int n = 0;
		for (int[] c : new int[][] {{-348, 444, 395}, {-348, 444, 394}, {-348, 443, 394}, {-348, 443, 393}, {-348, 443, 392}}) {
			Map<String, Integer> r = dig(h, c[0], c[1], c[2], DigMath.CH_METAL, 0, false);
			n += r.size();
			for (Map.Entry<String, Integer> e : r.entrySet()) {
				assertTrue(e.getValue() == DigMath.CH_METAL || e.getValue() == DigMath.CH_CONCRETE || e.getValue() == DigMath.BK_STONE, e.getKey());
			}
		}
		// whatever was revealed, no face of an open cell opens into rock
		for (String key : h.open) {
			String[] t = key.split(",");
			int x = Integer.parseInt(t[0]), y = Integer.parseInt(t[1]), z = Integer.parseInt(t[2]);
			for (int[] dir : DIRS) {
				int nx = x + dir[0], ny = y + dir[1], nz = z + dir[2];
				if (h.dug(nx, ny, nz) || !h.inBox(nx, ny, nz)) {
					continue;
				}
				assertFalse(DigMath.isInside(h.cells.get(Hall.k(nx, ny, nz))), "an open face into rock at " + nx + " " + ny + " " + nz);
			}
		}
		System.out.println("pillar tunnel: " + h.dug.size() + " dug cells, " + n + " revealed");
		assertTrue(h.dug.size() >= 5);
	}

	@Test
	void aTunnelWestThroughTheHallsOuterWallLeavesTheMapAndStaysClosed() throws IOException {
		Hall h = new Hall();
		// the hall's west wall: a brush 30 u thick (Source x -1120 .. -1090) in the cell x -356, which is partial (about 2/3); behind it the solid void
		int wall = h.samples(-356, 444, 391);
		assertTrue(wall > 0 && wall < DigMath.WHOLE, "wall cell " + wall);
		assertEquals(27, h.samples(-357, 444, 391));
		// mine west along y 444 (one cell high is enough to check the ladder; the player digs 2 high): open the wall cell, then every revealed block ahead
		h.dug.add(Hall.k(-356, 444, 391));
		h.open.add(Hall.k(-356, 444, 391));
		int[] expected = {DigMath.CH_METAL, DigMath.CH_CONCRETE}; // x -357, -358: depth 1, 2 (the dumped box ends at x -360: beyond it nothing is known, which counts as open)
		for (int i = 0; i < expected.length; i++) {
			int x = -357 - i;
			assertEquals(expected[i], DigMath.classify(h, x, 444, 391, DigMath.CH_METAL), "cell x " + x);
			assertTrue(h.cells.containsKey(Hall.k(x, 444, 391)));
			h.dug.add(Hall.k(x, 444, 391));
			h.open.add(Hall.k(x, 444, 391));
			// the four cells around the tunnel at this x are inside the void too: revealed (a block each), none left open
			for (int[] side : new int[][] {{0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}) {
				int kind = DigMath.classify(h, x + side[0], 444 + side[1], 391 + side[2], DigMath.CH_METAL);
				assertTrue(kind != DigMath.AIR && kind != DigMath.PENDING, "side cell " + side[1] + "," + side[2] + " of x " + x + " is " + kind);
			}
		}
	}
}
