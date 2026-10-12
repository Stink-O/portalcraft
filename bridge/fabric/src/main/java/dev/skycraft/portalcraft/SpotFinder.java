package dev.skycraft.portalcraft;

import dev.skycraft.SkyCraft;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import net.minecraft.core.BlockPos;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.tags.BlockTags;
import net.minecraft.tags.FluidTags;
import net.minecraft.world.level.LevelAccessor;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.levelgen.Heightmap;
import net.minecraft.world.level.storage.LevelResource;

/**
 * Portalcraft (bridge #81): the exit spot of a save, found ONCE per new world (nothing is hand-picked: every player's world has its own random seed).
 * The portal onto Minecraft (pit flings' cliff exit, every chamber's G portal) sits on a 2 x 3 block patch of a natural vertical face (or, as the
 * fallback, on a small stone backing built on flat ground), with a clear, roughly flat, dry landing in front of it. The search is deterministic for a
 * seed (same chunk generation, fixed iteration order) and bounded (about 6 s of the server thread, spawn outwards).
 *
 * <p>The result is stored in the save (portalcraft.json) and reported to the host plugin as cache/bridge/portalcraft_world.env (KEY=VALUE, the format of
 * bridge/world.conf): the plugin's pinned exit, the terrain mapping's anchor and the chamber band's origin all derive from it.
 */
public final class SpotFinder {
	private static final int RADIUS = Integer.getInteger("portalcraft.spotRadius", 200);
	private static final long BUDGET_MS = Long.getLong("portalcraft.spotBudgetMs", 6000L);

	private SpotFinder() {
	}

	/** wx wy wz: the patch's bottom wall block (along-face index 0); dx dz: the way the face looks (the open side). */
	record Spot(int wx, int wy, int wz, int dx, int dz, int faceH, double score, String kind) {
	}

	public static void ensure(MinecraftServer server) {
		try {
			ServerLevel level = server.overworld();
			Path root = server.getWorldPath(LevelResource.ROOT);
			Path json = root.resolve("portalcraft.json");
			Path env = cacheDir().resolve("portalcraft_world.env");
			Files.deleteIfExists(env);  // a stale report of another save must never be read
			Spot s = Files.isRegularFile(json) ? read(json) : null;
			if (s != null) {
				SkyCraft.LOG.info("Portalcraft: spot of this save (portalcraft.json): wall block {} {} {} facing ({},{}) {}", s.wx(), s.wy(), s.wz(), s.dx(), s.dz(), s.kind());
			} else {
				long t0 = System.currentTimeMillis();
				s = find(level);
				SkyCraft.LOG.info("Portalcraft: spot finder took {} ms: {} wall block {} {} {} facing ({},{}) face {} score {}", System.currentTimeMillis() - t0, s.kind(), s.wx(), s.wy(),
					s.wz(), s.dx(), s.dz(), s.faceH(), String.format("%.0f", s.score()));
				write(json, level.getSeed(), s);
			}
			report(env, level.getSeed(), root.toAbsolutePath().normalize().getFileName().toString(), s);
		} catch (Throwable e) {
			SkyCraft.LOG.error("Portalcraft: spot finder failed", e);
		}
	}

	private static Path cacheDir() {
		return Path.of(System.getProperty("portalcraft.cacheDir", "../../../cache/bridge"));
	}

	// ---- storage ---------------------------------------------------------------------------------------------------------------------------------
	private static void write(Path json, long seed, Spot s) throws java.io.IOException {
		String t = String.format("{\"seed\": %d, \"exitWallX\": %d, \"exitWallY\": %d, \"exitWallZ\": %d, \"facingDx\": %d, \"facingDz\": %d, \"faceHeight\": %d, \"kind\": \"%s\"}%n",
			seed, s.wx(), s.wy(), s.wz(), s.dx(), s.dz(), s.faceH(), s.kind());
		Files.writeString(json, t);
	}

	private static long num(String t, String key) {
		int i = t.indexOf("\"" + key + "\"");
		if (i < 0) {
			throw new IllegalArgumentException(key);
		}
		int a = t.indexOf(':', i) + 1;
		int b = a;
		while (b < t.length() && (Character.isDigit(t.charAt(b)) || t.charAt(b) == '-' || t.charAt(b) == ' ')) {
			b++;
		}
		return Long.parseLong(t.substring(a, b).trim());
	}

	private static Spot read(Path json) {
		try {
			String t = Files.readString(json);
			String kind = t.contains("\"floor\"") ? "floor" : t.contains("\"built\"") ? "built" : "natural";
			return new Spot((int) num(t, "exitWallX"), (int) num(t, "exitWallY"), (int) num(t, "exitWallZ"), (int) num(t, "facingDx"), (int) num(t, "facingDz"), (int) num(t, "faceHeight"), 0, kind);
		} catch (Exception e) {
			SkyCraft.LOG.warn("Portalcraft: unreadable portalcraft.json, finding the spot again: {}", e.toString());
			return null;
		}
	}

	/** The plugin's view (bridge/world.conf's keys). The cliff anchor of the terrain mapping sits 3 blocks behind the patch and 20 above it, as the original cliff did. */
	private static void report(Path env, long seed, String save, Spot s) throws java.io.IOException {
		int cx = s.wx() - s.dx() * 3, cz = s.wz() - s.dz() * 3, cy = s.wy() + 20;
		String t = String.format("# reported by the mod (SpotFinder) for the save below; the plugin and the tools read it, never edit%nSEED=%d%nSAVE=%s%nCLIFF_X=%d%nCLIFF_Y=%d%nCLIFF_Z=%d%nCLIFF_DX=%d%nCLIFF_DZ=%d%n"
			+ "EXIT_WX=%d%nEXIT_WY=%d%nEXIT_WZ=%d%nCHAMBER_X=%d%nCHAMBER_Y=752%nCHAMBER_Z=%d%nSPOT_KIND=%s%nSPOT_FACE_H=%d%n", seed, save, cx, cy, cz, s.dx(), s.dz(), s.wx(), s.wy(), s.wz(),
			Math.ceilDiv(s.wx(), 16) * 16, Math.ceilDiv(s.wz(), 16) * 16, s.kind(), s.faceH());
		Files.createDirectories(env.getParent());
		Path tmp = env.resolveSibling(env.getFileName() + ".tmp");
		Files.writeString(tmp, t);
		Files.move(tmp, env, java.nio.file.StandardCopyOption.REPLACE_EXISTING);
	}

	// ---- the search ------------------------------------------------------------------------------------------------------------------------------
	private record Cand(int x, int top, int z, int dx, int dz, double pre) {
	}

	static Spot find(ServerLevel level) {
		long t0 = System.currentTimeMillis();
		var source = level.getChunkSource();
		var gen = source.getGenerator();
		var rs = source.randomState();
		final int step = 4, n = RADIUS * 2 / step + 1, sea = gen.getSeaLevel();
		int[] h = new int[n * n];
		boolean[] wet = new boolean[n * n];
		for (int j = 0; j < n; j++) {
			for (int i = 0; i < n; i++) {
				int x = -RADIUS + i * step, z = -RADIUS + j * step;
				int y = gen.getBaseHeight(x, z, Heightmap.Types.WORLD_SURFACE_WG, level, rs);
				h[j * n + i] = y;
				wet[j * n + i] = y <= sea + 1;
			}
		}
		// candidate faces: a step in the ground (a drop of 3..40 within one grid step), dry, with dry, level land ahead
		int[][] dirs = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
		List<Cand> cands = new ArrayList<>();
		for (int j = 8; j < n - 8; j++) {
			for (int i = 8; i < n - 8; i++) {
				int top = h[j * n + i];
				if (wet[j * n + i] || top > 125) {
					continue;
				}
				for (int[] d : dirs) {
					int low = h[(j + d[1]) * n + (i + d[0])];
					int drop = top - low;
					if (drop < 3 || drop > 40) {
						continue;
					}
					double dev = 0;
					boolean ok = true;
					for (int k = 2; k <= 7 && ok; k++) {
						int idx = (j + d[1] * k) * n + (i + d[0] * k);
						ok = !wet[idx];
						dev += Math.abs(h[idx] - low);
					}
					if (!ok || dev / 6 > 4) {
						continue;
					}
					double dist = Math.hypot(-RADIUS + i * step, -RADIUS + j * step);
					cands.add(new Cand(-RADIUS + i * step, top, -RADIUS + j * step, d[0], d[1], Math.min(drop, 12) * 2 - dev / 6 * 3 - dist * 0.04));
				}
			}
		}
		cands.sort((a, b) -> {
			int c = Double.compare(b.pre(), a.pre());
			if (c != 0) {
				return c;
			}
			c = Integer.compare(a.x(), b.x());
			return c != 0 ? c : Integer.compare(a.z(), b.z()) != 0 ? Integer.compare(a.z(), b.z()) : Integer.compare(a.dx(), b.dx());
		});
		Spot best = null;
		int tried = 0;
		for (Cand c : cands) {
			if (tried >= 60 || (tried >= 8 && System.currentTimeMillis() - t0 > BUDGET_MS)) {
				break;
			}
			tried++;
			Spot s = evaluate(level, c);
			if (s != null && (best == null || s.score() > best.score())) {
				best = s;
			}
		}
		SkyCraft.LOG.info("Portalcraft: spot finder: {} candidate faces, {} checked on the real blocks", cands.size(), tried);
		return best != null ? best : build(level, h, wet, n);
	}

	private static boolean solid(LevelAccessor level, int x, int y, int z) {
		var p = new BlockPos(x, y, z);
		return !level.getBlockState(p).getCollisionShape(level, p).isEmpty();
	}

	/** The real blocks of a candidate: the best 2 x 3 patch of the face (wall at one depth, 2 air blocks in front), and the landing. Null if unfit. */
	private static Spot evaluate(ServerLevel level, Cand c) {
		final int dx = c.dx(), dz = c.dz(), rx = dz, rz = -dx;
		final int yTop = c.top() + 3, yBot = c.top() - 46;
		int[][] dep = new int[13][yTop - yBot + 1];
		for (int r = -6; r <= 6; r++) {
			for (int y = yBot; y <= yTop; y++) {
				int last = -1000;
				int res = -1000;
				for (int k = -6; k < 12; k++) {
					if (solid(level, c.x() + rx * r + dx * k, y, c.z() + rz * r + dz * k)) {
						last = k;
					} else if (last > -1000 && !solid(level, c.x() + rx * r + dx * (k + 1), y, c.z() + rz * r + dz * (k + 1)) && !solid(level, c.x() + rx * r + dx * (k + 2), y, c.z() + rz * r + dz * (k + 2))) {
						res = last;
						break;
					}
				}
				dep[r + 6][y - yBot] = res;
			}
		}
		int bestLen = 0, bestR = 0, bestK = 0, bestLo = 0;
		for (int r = -6; r <= 5; r++) {
			int runLen = 0, runK = 0;
			for (int y = yTop; y >= yBot - 1; y--) {
				int k = y >= yBot ? dep[r + 6][y - yBot] : -1000;
				boolean ok = k != -1000 && dep[r + 7][y - yBot] == k;
				if (ok && runLen > 0 && k == runK) {
					runLen++;
				} else {
					if (runLen > bestLen || (runLen == bestLen && runLen > 0 && Math.abs(r) < Math.abs(bestR))) {
						bestLen = runLen;
						bestR = r;
						bestK = runK;
						bestLo = y + 1;
					}
					runLen = ok ? 1 : 0;
					runK = k;
				}
			}
		}
		if (bestLen < 3) {
			return null;
		}
		final int r = bestR, k = bestK;
		// the patch: a third of the way up a tall face, on the face's lowest rows of a short one; the ground just in front must be reachable
		int py = bestLo + Math.min(6, (bestLen - 3) / 3);
		for (int j = 0; j < 3; j++) {
			for (int i = 0; i < 2; i++) {
				if (!solid(level, c.x() + rx * (r + i) + dx * k, py + j, c.z() + rz * (r + i) + dz * k) || solid(level, c.x() + rx * (r + i) + dx * (k + 1), py + j, c.z() + rz * (r + i) + dz * (k + 1))
					|| solid(level, c.x() + rx * (r + i) + dx * (k + 2), py + j, c.z() + rz * (r + i) + dz * (k + 2))) {
					return null;
				}
			}
		}
		for (int up = 3; up <= 10; up++) {  // nothing hanging over the opening (a cave mouth, a canopy of stone)
			for (int i = 0; i < 2; i++) {
				for (int f = 1; f <= 4; f++) {
					if (solid(level, c.x() + rx * (r + i) + dx * (k + f), py + up, c.z() + rz * (r + i) + dz * (k + f))) {
						return null;
					}
				}
			}
		}
		// the landing: ground 3..28 blocks out, 13 wide
		HashMap<Integer, Integer> hist = new HashMap<>();
		int tot = 0, wetCols = 0;
		for (int a = r - 6; a <= r + 7; a++) {
			for (int f = k + 3; f <= k + 28; f++) {
				int x = c.x() + rx * a + dx * f, z = c.z() + rz * a + dz * f;
				int y = py + 8;
				while (y > py - 50 && !solid(level, x, y, z)) {
					y--;
				}
				if (!level.getFluidState(new BlockPos(x, y + 1, z)).isEmpty() || level.getFluidState(new BlockPos(x, y, z)).is(FluidTags.WATER)) {
					wetCols++;
				}
				hist.merge(y, 1, Integer::sum);
				tot++;
			}
		}
		int mode = 0, modeN = 0, near = 0;
		for (var e : hist.entrySet()) {
			if (e.getValue() > modeN || (e.getValue() == modeN && e.getKey() > mode)) {
				modeN = e.getValue();
				mode = e.getKey();
			}
		}
		for (var e : hist.entrySet()) {
			if (Math.abs(e.getKey() - mode) <= 1) {
				near += e.getValue();
			}
		}
		int flat = 100 * near / tot, wetPct = 100 * wetCols / tot, below = py - mode;
		if (flat < 35 || wetPct > 12 || below < -1 || below > 45) {
			return null;
		}
		// the land ahead 6..40 blocks: trees are a plus, a deep or broken surface (ravine, cliff country) is not
		int leaves = 0, cols = 0, highL = 0, totL = 0, highR = 0, totR = 0;  // highL/R: columns to the sides that rise 8+ above the patch (a gorge)
		double sum = 0, sum2 = 0;
		for (int a = r - 16; a <= r + 17; a += 3) {
			for (int f = k + 6; f <= k + 40; f += 3) {
				int x = c.x() + rx * a + dx * f, z = c.z() + rz * a + dz * f;
				int y = py + 25;
				while (y > py - 45) {
					var st = level.getBlockState(new BlockPos(x, y, z));
					if (!st.isAir()) {
						if (st.is(BlockTags.LEAVES) || st.is(BlockTags.LOGS)) {
							leaves++;
						}
						sum += y;
						sum2 += (double) y * y;
						cols++;
						if (a < r - 5) {
							totL++;
							highL += y > py + 8 ? 1 : 0;
						} else if (a > r + 6) {
							totR++;
							highR += y > py + 8 ? 1 : 0;
						}
						break;
					}
					y--;
				}
			}
		}
		double mean = cols > 0 ? sum / cols : 0, sd = cols > 0 ? Math.sqrt(Math.max(0, sum2 / cols - mean * mean)) : 99;
		if (sd > 11 || py > 128) {
			return null;
		}
		if (totL > 0 && totR > 0 && highL * 10 >= totL * 3 && highR * 10 >= totR * 3) {  // walled in on both sides: a ravine, not open land
			return null;
		}
		double leafPct = cols > 0 ? 100.0 * leaves / cols : 0;
		double dist = Math.hypot(c.x(), c.z());
		double score = flat + Math.min(bestLen, 12) * 2 + Math.min(leafPct, 35) * 0.8 - sd * 1.5 - dist * 0.05 - Math.max(0, py - 100) * 0.5;
		return new Spot(c.x() + rx * r + dx * k, py, c.z() + rz * r + dz * k, dx, dz, bestLen, score, "natural");
	}

	/** Fallback: nothing qualified within the radius. A 3-wide, 4-high, 2-deep stone backing on the flattest dry ground near the spawn, facing -X, the patch on the ground. */
	private static Spot build(ServerLevel level, int[] h, boolean[] wet, int n) {
		int bi = -1;
		double bs = Double.MAX_VALUE;
		for (int j = 6; j < n - 6; j++) {
			for (int i = 6; i < n - 6; i++) {
				if (wet[j * n + i]) {
					continue;
				}
				int min = Integer.MAX_VALUE, max = Integer.MIN_VALUE;
				boolean dry = true;
				for (int b = -5; b <= 1 && dry; b++) {
					for (int a = -2; a <= 2; a++) {
						int idx = (j + a) * n + (i + b);
						dry = !wet[idx];
						min = Math.min(min, h[idx]);
						max = Math.max(max, h[idx]);
					}
				}
				double sc = dry ? (max - min) * 100 + Math.hypot(-RADIUS + i * 4, -RADIUS + j * 4) : Double.MAX_VALUE;
				if (sc < bs) {
					bs = sc;
					bi = j * n + i;
				}
			}
		}
		int cx = -RADIUS + (bi % n) * 4, cz = -RADIUS + (bi / n) * 4;
		int g = level.getChunkSource().getGenerator().getBaseHeight(cx, cz, Heightmap.Types.WORLD_SURFACE_WG, level, level.getChunkSource().randomState()) - 1;
		int wx = cx, wz = cz - 1;  // the patch's wall column wx, along-face z wz..wz+1
		for (int x = wx - 6; x <= wx + 1; x++) {
			for (int z = wz - 2; z <= wz + 3; z++) {
				for (int y = g + 1; y <= g + 6; y++) {
					if (x < wx - 0 || x > wx + 1 || z < wz - 1 || z > wz + 2) {
						level.setBlock(new BlockPos(x, y, z), Blocks.AIR.defaultBlockState(), 2);
					}
				}
				if (x < wx) {
					level.setBlock(new BlockPos(x, g, z), level.getBlockState(new BlockPos(x, g, z)).isAir() ? Blocks.STONE.defaultBlockState() : level.getBlockState(new BlockPos(x, g, z)), 2);
				}
			}
		}
		for (int x = wx; x <= wx + 1; x++) {
			for (int z = wz - 1; z <= wz + 2; z++) {
				for (int y = g; y <= g + 4; y++) {
					level.setBlock(new BlockPos(x, y, z), Blocks.STONE.defaultBlockState(), 2);
				}
			}
		}
		return new Spot(wx, g + 1, wz, -1, 0, 4, 0, "built");
	}
}
