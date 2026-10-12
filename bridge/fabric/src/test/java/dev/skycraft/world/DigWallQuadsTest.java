package dev.skycraft.world;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Random;
import java.util.Set;
import org.junit.jupiter.api.Test;

/** The geometry of the hole walls (bridge #38: looking for the "grey wedges" of the lead's dig6 screenshots). */
class DigWallQuadsTest {
	private static final int[][] STEP = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};

	private static final class World implements DigWallQuads.Source {
		final Set<String> dug = new HashSet<>();
		final Set<String> blocks = new HashSet<>();
		final Map<String, long[]> host = new HashMap<>();

		static String k(int x, int y, int z) {
			return x + "," + y + "," + z;
		}

		@Override
		public boolean dug(int x, int y, int z) {
			return this.dug.contains(k(x, y, z));
		}

		@Override
		public boolean occludes(int x, int y, int z) {
			return this.blocks.contains(k(x, y, z));
		}

		@Override
		public long[] bits(int x, int y, int z) {
			return this.host.get(k(x, y, z));
		}

		@Override
		public int light(int x, int y, int z) {
			return 7;
		}

		@Override
		public int kind(int x, int y, int z, int nx, int ny, int nz) {
			return DigMath.CH_CONCRETE;
		}
	}

	private record Quad(float[] xyz, float[] uv, int light, int normal) {
	}

	private static List<Quad> collect(World w, int sx, int sy, int sz, long[] dug) {
		List<Quad> out = new ArrayList<>();
		DigWallQuads.collect(w, sx, sy, sz, dug, kind -> new float[] {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1}, (xyz, uv, light, normal) -> out.add(new Quad(xyz.clone(), uv.clone(), light, normal)));
		return out;
	}

	private static long[] randomBits(Random r, int density) {
		long[] b = new long[8];
		for (int y = 0; y < 8; y++) {
			for (int i = 0; i < 64; i++) {
				if (r.nextInt(100) < density) {
					b[y] |= 1L << i;
				}
			}
		}
		return b;
	}

	/** A quad in a section's coordinates, the dug cell's face to the neighbour, facing into the dug cell, counter-clockwise from the front. */
	@Test
	void everyQuadLiesOnItsFaceInsideTheCellFacingIntoTheHole() {
		Random r = new Random(7);
		for (int round = 0; round < 200; round++) {
			World w = new World();
			// the dug cell somewhere in section (-3, 27, 5) (negative section coordinates, a section far from the origin)
			int sx = -3, sy = 27, sz = 5;
			int cx = r.nextInt(16), cy = r.nextInt(16), cz = r.nextInt(16);
			int x = sx * 16 + cx, y = sy * 16 + cy, z = sz * 16 + cz;
			w.dug.add(World.k(x, y, z));
			for (int[] s : STEP) {
				if (r.nextInt(4) > 0) {
					w.host.put(World.k(x + s[0], y + s[1], z + s[2]), randomBits(r, 10 + r.nextInt(90)));
				}
			}
			long[] dug = new long[64];
			DigMath.setBit(dug, DigMath.bitIndex(cx, cy, cz));
			for (Quad q : collect(w, sx, sy, sz, dug)) {
				int normal = q.normal;
				int dir = normal ^ 1; // from the dug cell to the neighbour
				int axis = DigMath.axisOf(dir);
				double[][] v = new double[4][3];
				double[] origin = {x - sx * 16, y - sy * 16, z - sz * 16};
				for (int k = 0; k < 4; k++) {
					for (int a = 0; a < 3; a++) {
						v[k][a] = q.xyz[k * 3 + a];
						// inside the dug cell's own box (section-relative), on its face plane along the axis
						assertTrue(v[k][a] >= origin[a] - 1e-6 && v[k][a] <= origin[a] + 1 + 1e-6, "vertex " + k + " axis " + a + " leaves the cell: " + v[k][a] + " vs " + origin[a]);
					}
					assertEquals(origin[axis] + (DigMath.stepOf(dir) > 0 ? 1 : 0), v[k][axis], 1e-6, "vertex off the face plane");
				}
				// a rectangle with sides along the two other axes
				double[] e1 = {v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2]};
				double[] e2 = {v[3][0] - v[0][0], v[3][1] - v[0][1], v[3][2] - v[0][2]};
				double[] diag = {v[2][0] - v[0][0], v[2][1] - v[0][1], v[2][2] - v[0][2]};
				for (int a = 0; a < 3; a++) {
					assertEquals(e1[a] + e2[a], diag[a], 1e-6, "not a parallelogram");
				}
				// counter-clockwise seen from the front: (v1 - v0) x (v3 - v0) = the normal
				double[] c = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
				double len = Math.sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
				assertTrue(len > 1e-9, "degenerate quad");
				for (int a = 0; a < 3; a++) {
					assertEquals(STEP[normal][a], c[a] / len, 1e-6, "winding: the front is not the normal's side");
				}
			}
		}
	}

	@Test
	void aWhollySolidNeighbourIsOneFullFaceAndPartialOnesCoverExactlyTheirSolidSubVoxels() {
		World w = new World();
		w.dug.add(World.k(0, 0, 0));
		long[] full = {-1L, -1L, -1L, -1L, -1L, -1L, -1L, -1L};
		w.host.put(World.k(1, 0, 0), full);
		// below: only the top two sub-voxel layers of the cell below are solid (a floor surface 2/8 under the cell's top)... as seen from above
		long[] shelf = new long[8];
		shelf[7] = -1L;
		shelf[6] = -1L;
		w.host.put(World.k(0, -1, 0), shelf);
		// north neighbour: only the bottom layer: not next to the dug cell... north face layer z = 7 of that cell: nothing there
		long[] nope = new long[8];
		nope[0] = 0x00FFFFFFFFFFFFFFL; // z layers 0..6 only
		w.host.put(World.k(0, 0, -1), nope);
		long[] dug = new long[64];
		DigMath.setBit(dug, 0);
		List<Quad> quads = collect(w, 0, 0, 0, dug);
		int east = 0, down = 0, north = 0;
		double areaEast = 0, areaDown = 0;
		for (Quad q : quads) {
			double minA = 9, maxA = -9, minB = 9, maxB = -9;
			if (q.normal == 4) { // facing west: the east neighbour's wall
				east++;
				for (int k = 0; k < 4; k++) {
					minA = Math.min(minA, q.xyz[k * 3 + 1]);
					maxA = Math.max(maxA, q.xyz[k * 3 + 1]);
					minB = Math.min(minB, q.xyz[k * 3 + 2]);
					maxB = Math.max(maxB, q.xyz[k * 3 + 2]);
				}
				areaEast += (maxA - minA) * (maxB - minB);
			} else if (q.normal == 1) { // facing up: the floor of the cell below
				down++;
				for (int k = 0; k < 4; k++) {
					minA = Math.min(minA, q.xyz[k * 3]);
					maxA = Math.max(maxA, q.xyz[k * 3]);
					minB = Math.min(minB, q.xyz[k * 3 + 2]);
					maxB = Math.max(maxB, q.xyz[k * 3 + 2]);
				}
				areaDown += (maxA - minA) * (maxB - minB);
				for (int k = 0; k < 4; k++) {
					assertEquals(0.0, q.xyz[k * 3 + 1], 1e-9, "the floor face is the dug cell's bottom plane");
				}
			} else if (q.normal == 3) {
				north++;
			}
		}
		assertEquals(1, east, "one merged quad for the full face");
		assertEquals(1.0, areaEast, 1e-9);
		assertEquals(1, down);
		assertEquals(1.0, areaDown, 1e-9, "the boundary layer of the cell below (its top layer, y = 7) is solid across the whole face");
		assertEquals(0, north, "the layer of the north cell next to the dug cell is empty");
		// a real block in the neighbour or in the dug cell itself: no wall
		w.blocks.add(World.k(1, 0, 0));
		assertEquals(quads.size() - 1, collect(w, 0, 0, 0, dug).size());
		w.blocks.add(World.k(0, 0, 0));
		assertEquals(0, collect(w, 0, 0, 0, dug).size());
	}

	@Test
	void twoDugCellsShareNoWall() {
		World w = new World();
		w.dug.add(World.k(0, 0, 0));
		w.dug.add(World.k(1, 0, 0));
		long[] dug = new long[64];
		DigMath.setBit(dug, DigMath.bitIndex(0, 0, 0));
		DigMath.setBit(dug, DigMath.bitIndex(1, 0, 0));
		long[] full = {-1L, -1L, -1L, -1L, -1L, -1L, -1L, -1L};
		for (int[] s : STEP) {
			w.host.put(World.k(0 + s[0], s[1], s[2]), full);
			w.host.put(World.k(1 + s[0], s[1], s[2]), full);
		}
		// the cells (0,0,0) and (1,0,0) are dug and have host content "inside" each other: no wall between them
		assertEquals(5 + 5, collect(w, 0, 0, 0, dug).size());
	}
}
