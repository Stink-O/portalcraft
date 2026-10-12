package dev.skycraft.world;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

/** Bridge #47: the landscape the portal onto Minecraft opens on: cliffs at x = k * 256 facing -X that are flat faces, with a lowland in front. */
class StageTerrainTest {
	@Test
	void everyRunHasAFlatFaceFacingMinusX() {
		for (int k = -3; k <= 3; k++) {
			for (int z = -300; z < 300; z += StageTerrain.RUN) {
				int run = Math.floorDiv(z, StageTerrain.RUN);
				int zs = run * StageTerrain.RUN;
				int face = k * StageTerrain.PERIOD + StageTerrain.offset(zs);
				for (int dz = 0; dz < StageTerrain.RUN; dz++) {
					assertEquals(face, k * StageTerrain.PERIOD + StageTerrain.offset(zs + dz), "the run is flat");
					assertTrue(StageTerrain.plateau(face, zs + dz), "behind the face is the plateau");
					assertFalse(StageTerrain.plateau(face - 1, zs + dz), "in front of the face is the lowland");
					int hi = StageTerrain.surface(face, zs + dz), lo = StageTerrain.surface(face - 1, zs + dz);
					assertTrue(hi - lo >= 18, "the face is at least 18 blocks tall (" + (hi - lo) + " at x " + face + " z " + (zs + dz) + ")");
				}
			}
		}
	}

	@Test
	void lowlandInFrontOfAFaceIsDryAndLevel() {
		int dry = 0, total = 0;
		for (int k = -2; k <= 2; k++) {
			for (int z = -200; z < 200; z++) {
				int face = k * StageTerrain.PERIOD + StageTerrain.offset(z);
				for (int f = 1; f <= 3; f++) {
					total++;
					int s = StageTerrain.surface(face - f, z);
					if (s > StageTerrain.SEA + 1 && Math.abs(s - StageTerrain.LOW) <= 2) {
						dry++;
					}
				}
			}
		}
		assertEquals(total, dry, "the three blocks in front of every face are dry ground within 2 of the lowland level");
	}

	@Test
	void stageStaysInsideItsSections() {
		for (int x = -400; x < 400; x += 7) {
			for (int z = -400; z < 400; z += 5) {
				int s = StageTerrain.surface(x, z);
				assertTrue(s > StageTerrain.Y0 + 20 && s < StageTerrain.TOP - 16, "surface " + s + " at " + x + " " + z);
				assertEquals(StageTerrain.surface(x, z), s, "deterministic");
				assertEquals(1, StageTerrain.code(x, StageTerrain.Y0, z));
				assertEquals(0, StageTerrain.code(x, StageTerrain.TOP, z));
			}
		}
		assertTrue(StageTerrain.sectionInStage(StageTerrain.Y0 >> 4));
		assertTrue(StageTerrain.sectionInStage((StageTerrain.TOP - 1) >> 4));
		assertFalse(StageTerrain.sectionInStage(30));
	}

	@Test
	void hasLakesAndTrees() {
		int water = 0, trees = 0, grass = 0;
		for (int x = -512; x < 512; x += 2) {
			for (int z = -512; z < 512; z += 2) {
				if (StageTerrain.surface(x, z) < StageTerrain.SEA) {
					water++;
				} else if (StageTerrain.treeAt(x, z)) {
					trees++;
				} else {
					grass++;
				}
			}
		}
		assertTrue(water > 1000, "lakes: " + water);
		assertTrue(trees > 100, "oaks: " + trees);
		assertTrue(grass > water, "mostly dry land");
	}

	// ---- the host's exit search (stage.cpp / clip.cpp FindStagePatch), run on the landscape itself ----

	private static boolean solid(int x, int y, int z) {
		int c = StageTerrain.code(x, y, z);
		return c >= 1 && c <= 5;
	}

	/** The same search the host makes in the streamed blocks: a flat, dry 2 x 3 patch of a face (facing -X) with 2 blocks of air in front. */
	private static int[] findPatch(int wallX, int zc) {
		int row = StageTerrain.LOW + 10;
		for (int k = 0; k < 41; k++) {
			int z0 = zc + ((k & 1) != 0 ? -(k + 1) : k);
			if (Math.floorDiv(z0, StageTerrain.RUN) != Math.floorDiv(z0 + 1, StageTerrain.RUN)) {
				continue;
			}
			int[] face = {0, 0}, ground = {-1, -1};
			for (int c = 0; c < 2; c++) {
				for (int x = wallX - 6; x <= wallX + 10; x++) {
					if (solid(x, row, z0 + c) && !solid(x - 1, row, z0 + c)) {
						face[c] = x;
						break;
					}
				}
				if (face[c] == 0) {
					break;
				}
				for (int y = row; y > StageTerrain.LOW - 12; y--) {
					if (solid(face[c] - 1, y, z0 + c)) {
						ground[c] = y;
						break;
					}
				}
			}
			if (face[0] == 0 || face[0] != face[1] || ground[0] < 0 || ground[0] != ground[1]) {
				continue;
			}
			int wy = ground[0] + 1;
			boolean ok = true;
			for (int j = 0; j < 3 && ok; j++) {
				for (int i = 0; i < 2 && ok; i++) {
					ok = solid(face[0], wy + j, z0 + i) && !solid(face[0] - 1, wy + j, z0 + i) && !solid(face[0] - 2, wy + j, z0 + i)
						&& StageTerrain.code(face[0] - 1, wy + j, z0 + i) != 6 && StageTerrain.code(face[0] - 2, wy + j, z0 + i) != 6;
				}
			}
			if (ok) {
				return new int[] {face[0], wy, z0};
			}
		}
		return null;
	}

	@Test
	void everyWindowHasAnExit() {
		int found = 0, total = 0, farthest = 0;
		for (int k = -3; k <= 3; k++) {
			for (int z = -2000; z < 2000; z += 7) {
				int zc = Math.floorDiv(z, StageTerrain.RUN) * StageTerrain.RUN + StageTerrain.RUN / 2 - 1;
				total++;
				int[] p = findPatch(k * StageTerrain.PERIOD, zc);
				if (p != null) {
					found++;
					farthest = Math.max(farthest, Math.abs(p[2] - zc));
					assertTrue(p[0] - k * StageTerrain.PERIOD >= 0 && p[0] - k * StageTerrain.PERIOD <= 3, "the face is at the wall plus its run's offset (" + p[0] + ")");
					assertTrue(p[1] >= StageTerrain.LOW - 1 && p[1] <= StageTerrain.LOW + 3, "the patch stands on the lowland (" + p[1] + ")");
				}
			}
		}
		assertEquals(total, found, "a patch within 40 blocks of the window's z run, in every window (" + found + "/" + total + ")");
		assertTrue(farthest <= 40);
	}
}
