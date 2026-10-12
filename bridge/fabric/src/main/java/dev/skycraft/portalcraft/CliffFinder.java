package dev.skycraft.portalcraft;

import dev.skycraft.SkyCraft;
import java.util.ArrayList;
import java.util.List;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.levelgen.Heightmap;

/**
 * Portalcraft: finds scenic cliffs in a generated world without generating chunks, from the
 * chunk generator's noise heights: a near-vertical drop of 20+ blocks whose low side opens onto
 * lower, dry, varied terrain (the view a flinging player gets out of a portal on the cliff face).
 * -Dskycraft.findCliff=<radius> logs the best candidates at server start.
 */
public final class CliffFinder {
	private CliffFinder() {
	}

	public record Cliff(int x, int top, int z, int dx, int dz, int drop, double score, int lowMean) {
	}

	public static void startIfRequested(MinecraftServer server) {
		int radius = Integer.getInteger("skycraft.findCliff", 0);
		if (radius <= 0) {
			return;
		}
		Thread t = new Thread(() -> {
			try {
				long t0 = System.currentTimeMillis();
				List<Cliff> best = find(server.overworld(), 0, 0, radius, 4);
				SkyCraft.LOG.info("Portalcraft: cliff search (radius {}) took {} ms", radius, System.currentTimeMillis() - t0);
				for (Cliff c : best) {
					SkyCraft.LOG.info("Portalcraft: cliff at {} {} {} facing ({},{}) drop {} low-side mean height {} score {}", c.x(), c.top(), c.z(), c.dx(), c.dz(),
						c.drop(), c.lowMean(), String.format("%.1f", c.score()));
				}
				// Real blocks (carvers, overhangs, trees): generate the chunks and check each face.
				for (Cliff c : best.subList(0, Math.min(60, best.size()))) {
					server.submit(() -> evaluate(server.overworld(), c)).join();
				}
			} catch (Throwable e) {
				SkyCraft.LOG.error("Portalcraft: cliff search failed", e);
			}
		}, "Portalcraft cliff finder");
		t.setDaemon(true);
		t.start();
	}

	/**
	 * (bridge #81) Looks at the actual blocks of a candidate and logs "CLIFFEVAL" lines for choosing a cliff for filming: the tallest VERTICAL face
	 * (rows where the wall sits at one depth over two neighbouring columns, with 2 blocks of air in front of it, which is what the exit patch check
	 * needs), where the 2 x 3 patch goes (a third of the way up, as the original cliff), nothing overhanging it, the landing in front (how flat
	 * the ground is 4..30 blocks out, and how far below the patch), and what the land 6..54 blocks ahead is made of (leaves, logs, water).
	 */
	static void evaluate(ServerLevel level, Cliff c) {
		final int dx = c.dx(), dz = c.dz(), rx = dz, rz = -dx;
		java.util.function.Function<int[], Boolean> solid = a -> {
			var p = new net.minecraft.core.BlockPos(c.x() + rx * a[0] + dx * a[1], a[2], c.z() + rz * a[0] + dz * a[1]);
			return !level.getBlockState(p).getCollisionShape(level, p).isEmpty();
		};
		java.util.function.IntBinaryOperator depth = (r, y) -> {
			int last = -1000;
			for (int k = -20; k < 30; k++) {
				if (solid.apply(new int[] {r, k, y})) {
					last = k;
				} else if (last > -1000) {
					boolean open = true;
					for (int f = 1; f <= 3; f++) {
						open &= !solid.apply(new int[] {r, k + f, y});
					}
					if (open) {
						return last;
					}
				}
			}
			return -1000;
		};
		int bestLen = 0, bestR = 0, bestK = 0, bestLo = 0;
		int yTop = c.top() + 2, yBot = c.top() - 64;
		int[][] dep = new int[23][yTop - yBot + 1];
		for (int r = -11; r <= 11; r++) {
			for (int y = yBot; y <= yTop; y++) {
				dep[r + 11][y - yBot] = depth.applyAsInt(r, y);
			}
		}
		for (int r = -11; r <= 10; r++) {
			int runLen = 0, runK = 0;
			for (int y = yTop; y >= yBot - 1; y--) {
				int k = y >= yBot ? dep[r + 11][y - yBot] : -1000;
				boolean ok = k != -1000 && dep[r + 12][y - yBot] == k;
				if (ok && runLen > 0 && k == runK) {
					runLen++;
				} else {
					if (runLen > bestLen) {
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
			SkyCraft.LOG.info("Portalcraft: CLIFFEVAL cliff {} {} {}: no vertical face", c.x(), c.top(), c.z());
			return;
		}
		final int r = bestR, k = bestK;
		int py = bestLo + Math.max(0, (bestLen - 3) / 3);  // the patch's bottom row: a third of the way up
		// the exit check (like the plugin's): 3 rows x 2 columns solid at depth k, 2 air blocks in front of each
		boolean patchOk = true;
		for (int j = 0; j < 3 && patchOk; j++) {
			for (int i = 0; i < 2 && patchOk; i++) {
				patchOk = solid.apply(new int[] {r + i, k, py + j}) && !solid.apply(new int[] {r + i, k + 1, py + j}) && !solid.apply(new int[] {r + i, k + 2, py + j});
			}
		}
		boolean overhang = false;
		for (int up = 3; up <= 14 && !overhang; up++) {
			for (int i = 0; i < 2 && !overhang; i++) {
				for (int f = 1; f <= 4 && !overhang; f++) {
					overhang = solid.apply(new int[] {r + i, k + f, py + up});
				}
			}
		}
		// the landing: the ground 4..30 blocks out, 13 wide, from the patch down
		int gMin = Integer.MAX_VALUE, gMax = Integer.MIN_VALUE;
		java.util.HashMap<Integer, Integer> hist = new java.util.HashMap<>();
		int tot = 0;
		for (int a = r - 6; a <= r + 7; a++) {
			for (int f = k + 4; f <= k + 30; f++) {
				int y = py + 8;
				while (y > py - 60 && !solid.apply(new int[] {a, f, y})) {
					y--;
				}
				hist.merge(y, 1, Integer::sum);
				tot++;
			}
		}
		int mode = 0, modeN = 0;
		for (var e : hist.entrySet()) {
			if (e.getValue() > modeN) {
				modeN = e.getValue();
				mode = e.getKey();
			}
		}
		int near = 0;
		for (var e : hist.entrySet()) {
			if (Math.abs(e.getKey() - mode) <= 1) {
				near += e.getValue();
			}
		}
		int logs = 0, leaves = 0, water = 0, land = 0, n = 0;
		double sum = 0, sum2 = 0;
		for (int a = r - 24; a <= r + 24; a += 2) {
			for (int f = k + 6; f <= k + 54; f += 2) {
				var p = new net.minecraft.core.BlockPos(c.x() + rx * a + dx * f, 0, c.z() + rz * a + dz * f);
				int y = py + 20;
				while (y > py - 70) {
					var st = level.getBlockState(p.atY(y));
					if (!st.isAir()) {
						if (st.is(net.minecraft.tags.BlockTags.LOGS)) {
							logs++;
						} else if (st.is(net.minecraft.tags.BlockTags.LEAVES)) {
							leaves++;
						} else if (st.getFluidState().is(net.minecraft.tags.FluidTags.WATER)) {
							water++;
						} else {
							land++;
							sum += y;
							sum2 += (double) y * y;
							n++;
						}
						break;
					}
					y--;
				}
			}
		}
		double mean = n > 0 ? sum / n : 0, sd = n > 0 ? Math.sqrt(Math.max(0, sum2 / n - mean * mean)) : 0;
		int cols = logs + leaves + water + land;
		var biome = level.getBiome(new net.minecraft.core.BlockPos(c.x() + rx * r + dx * (k + 20), py, c.z() + rz * r + dz * (k + 20))).unwrapKey().map(kk -> kk.identifier().toString()).orElse("?");
		SkyCraft.LOG.info("Portalcraft: CLIFFEVAL cliff {} {} {} facing ({},{}): vertical face {} tall (y {}..{}), patch wall block {} {} {} patchOk {} overhang {}; landing ground y {} ({} below the patch), flat {}% within 1 (y {}..{}); land ahead: {} columns, leaves {}%, logs {}, water {}%, mean y {} sd {}; biome {}",
			c.x(), c.top(), c.z(), dx, dz, bestLen, bestLo, bestLo + bestLen - 1, c.x() + rx * r + dx * k, py, c.z() + rz * r + dz * k, patchOk, overhang, mode, py - mode, tot > 0 ? 100 * near / tot : 0,
			hist.isEmpty() ? 0 : java.util.Collections.min(hist.keySet()), hist.isEmpty() ? 0 : java.util.Collections.max(hist.keySet()), cols, cols > 0 ? 100 * leaves / cols : 0, logs,
			cols > 0 ? 100 * water / cols : 0, String.format("%.0f", mean), String.format("%.1f", sd), biome);
	}

	public static List<Cliff> find(ServerLevel level, int cx, int cz, int radius, int step) {
		var source = level.getChunkSource();
		var gen = source.getGenerator();
		var rs = source.randomState();
		int n = radius * 2 / step + 1;
		int[] h = new int[n * n];
		boolean[] wet = new boolean[n * n];
		int sea = gen.getSeaLevel();
		for (int j = 0; j < n; j++) {
			for (int i = 0; i < n; i++) {
				int x = cx - radius + i * step, z = cz - radius + j * step;
				int y = gen.getBaseHeight(x, z, Heightmap.Types.WORLD_SURFACE_WG, level, rs);
				h[j * n + i] = y;
				wet[j * n + i] = y <= sea + 1 && gen.getBaseColumn(x, z, level, rs).getBlock(y - 1).is(Blocks.WATER);
			}
		}
		int[][] dirs = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
		List<Cliff> out = new ArrayList<>();
		int near = Math.max(1, 4 / step), far = Math.max(2, 96 / step), mid = Math.max(1, 24 / step);
		for (int j = far; j < n - far; j++) {
			for (int i = far; i < n - far; i++) {
				int top = h[j * n + i];
				if (wet[j * n + i]) {
					continue;
				}
				for (int[] d : dirs) {
					int low = h[(j + d[1] * near) * n + (i + d[0] * near)];
					int drop = top - low;
					if (drop < 16) {
						continue;
					}
					// open view: everything ahead 24..96 blocks is well below the top, mostly dry
					boolean open = true;
					double sum = 0, sum2 = 0;
					int dry = 0, cnt = 0;
					for (int k = mid; k <= far; k++) {
						int idx = (j + d[1] * k) * n + (i + d[0] * k);
						int y = h[idx];
						if (y > top - 12) {
							open = false;
							break;
						}
						sum += y;
						sum2 += (double) y * y;
						cnt++;
						if (!wet[idx]) {
							dry++;
						}
					}
					if (!open || cnt == 0) {
						continue;
					}
					double mean = sum / cnt, var = sum2 / cnt - mean * mean;
					double score = drop + Math.sqrt(Math.max(0, var)) * 2 + 20.0 * dry / cnt;
					out.add(new Cliff(cx - radius + i * step, top, cz - radius + j * step, d[0], d[1], drop, score, (int) mean));
				}
			}
		}
		out.sort((a, b) -> Double.compare(b.score(), a.score()));
		// keep candidates at least 64 blocks apart
		List<Cliff> best = new ArrayList<>();
		for (Cliff c : out) {
			boolean far2 = true;
			for (Cliff b : best) {
				if (Math.abs(b.x() - c.x()) < 40 && Math.abs(b.z() - c.z()) < 40) {
					far2 = false;
					break;
				}
			}
			if (far2) {
				best.add(c);
				if (best.size() >= 60) {
					break;
				}
			}
		}
		return best;
	}
}
