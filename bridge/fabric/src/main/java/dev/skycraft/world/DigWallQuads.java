package dev.skycraft.world;

import java.util.List;

/**
 * The quads of the walls of holes dug into the host's chamber, the pure part (no Minecraft state; unit tested). Adapted from SkyCraft's
 * DigWalls (MIT, chasmlol/SkyCraft; see LICENSE-SkyCraft): for every dug cell D and every neighbour N across a face of D that is not
 * dug and has no occluding block, one quad per merged run of solid sub-voxel faces of N's boundary layer next to D (8x8 sub-voxels per
 * block, the host's voxel field), lying on the plane between D and N and facing into D. A wholly solid N is a single full-face quad.
 * The client's DigWalls feeds it Minecraft's blocks and light and puts the quads in the section mesh.
 *
 * Directions are Minecraft's Direction ordinals: 0 down, 1 up, 2 north (-z), 3 south (+z), 4 west (-x), 5 east (+x).
 */
public final class DigWallQuads {
	private DigWallQuads() {
	}

	public interface Out {
		/** One quad: four corners (x, y, z, section-relative blocks) in counter-clockwise order seen from the front, their atlas UVs, light, and the normal (Direction ordinal). */
		void quad(float[] xyz, float[] uv, int light, int normal);
	}

	public interface Faces {
		/** Side, top and bottom atlas rects (u0, v0, u1, v1) of a full-cube block of this reveal kind (12 floats), or null. */
		float[] of(int kind);
	}

	/** What the walls need to know about the world. Cells are block coordinates. */
	public interface Source {
		boolean dug(int x, int y, int z);

		/** A real block fills the cell (nothing to see through it, so no wall next to it). */
		boolean occludes(int x, int y, int z);

		/** The host's raw voxels in the cell (eight words, bits[y] bit z * 8 + x), or null. */
		long[] bits(int x, int y, int z);

		/** Minecraft's light in the (dug) cell, packed. */
		int light(int x, int y, int z);

		/** The block kind (DigMath: CH_* 1..6, BK_STONE) of the wall of the neighbour (nx, ny, nz) of the dug cell (x, y, z). */
		int kind(int x, int y, int z, int nx, int ny, int nz);
	}

	private static final int[][] STEP = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};

	/** The quads of the walls around the dug cells {@code dug} (bits x + 16z + 256y) of section (sx, sy, sz). Returns the number of quads. */
	public static int collect(Source src, int sx, int sy, int sz, long[] dug, Faces faces, Out out) {
		int quads = 0;
		float[] xyz = new float[12];
		float[] uv = new float[8];
		for (int word = 0; word < 64; word++) {
			long bits = dug[word];
			while (bits != 0) {
				int bit = word * 64 + Long.numberOfTrailingZeros(bits);
				bits &= bits - 1;
				int x = sx * 16 + (bit & 15), y = sy * 16 + (bit >> 8), z = sz * 16 + ((bit >> 4) & 15);
				if (src.occludes(x, y, z)) {
					continue; // a solid block fills the dug cell: nothing to see from in it
				}
				int light = src.light(x, y, z);
				for (int dir = 0; dir < 6; dir++) {
					int nx = x + STEP[dir][0], ny = y + STEP[dir][1], nz = z + STEP[dir][2];
					if (src.dug(nx, ny, nz) || src.occludes(nx, ny, nz)) {
						continue;
					}
					long[] hostBits = src.bits(nx, ny, nz);
					if (hostBits == null) {
						continue;
					}
					long mask = DigMath.boundaryMask(hostBits, dir);
					if (mask == 0L) {
						continue;
					}
					float[] rects = faces.of(src.kind(x, y, z, nx, ny, nz));
					if (rects == null) {
						continue;
					}
					int rect = dir == 1 ? 8 : dir == 0 ? 4 : 0; // a ceiling shows the block's bottom
					List<int[]> merged = DigMath.mergeRects(mask);
					for (int[] r : merged) {
						emit(out, dir, x, y, z, sx, sy, sz, r, rects, rect, light, xyz, uv);
						quads++;
					}
				}
			}
		}
		return quads;
	}

	private static void emit(Out out, int dir, int x, int y, int z, int sx, int sy, int sz, int[] r, float[] rects, int rect, int light, float[] xyz, float[] uv) {
		int axis = DigMath.axisOf(dir);
		int[] ab = DigMath.faceAxes(dir);
		int[] origin = {x, y, z};
		double plane = origin[axis] + (DigMath.stepOf(dir) > 0 ? 1.0 : 0.0);
		int[][] corner = {{r[0], r[1]}, {r[0] + r[2], r[1]}, {r[0] + r[2], r[1] + r[3]}, {r[0], r[1] + r[3]}};
		int[] section = {sx * 16, sy * 16, sz * 16};
		double[] p = new double[3];
		for (int k = 0; k < 4; k++) {
			p[axis] = plane;
			p[ab[0]] = origin[ab[0]] + corner[k][0] / 8.0;
			p[ab[1]] = origin[ab[1]] + corner[k][1] / 8.0;
			xyz[k * 3] = (float) (p[0] - section[0]);
			xyz[k * 3 + 1] = (float) (p[1] - section[1]);
			xyz[k * 3 + 2] = (float) (p[2] - section[2]);
			// texture: u across the face's horizontal axis (x, or z on x faces), v down from the top (side faces) or along z (floors, ceilings)
			double fu, fv;
			if (axis == 1) {
				fu = p[0] - origin[0];
				fv = p[2] - origin[2];
			} else {
				int h = axis == 0 ? 2 : 0;
				fu = p[h] - origin[h];
				fv = 1.0 - (p[1] - origin[1]);
			}
			uv[k * 2] = rects[rect] + (float) fu * (rects[rect + 2] - rects[rect]);
			uv[k * 2 + 1] = rects[rect + 1] + (float) fv * (rects[rect + 3] - rects[rect + 1]);
		}
		out.quad(xyz, uv, light, dir ^ 1); // the opposite direction (0/1, 2/3, 4/5 pair up)
	}
}
