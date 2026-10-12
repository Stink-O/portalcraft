package dev.skycraft.client.portalcraft;

import dev.skycraft.SkyCraft;
import net.minecraft.client.Minecraft;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.block.state.BlockState;

/**
 * Portalcraft (worldgen identity check): with -Dportalcraft.worldHash=<radius_chunks>, once every chunk
 * within that radius of the spot (-Dportalcraft.cliffX/Z, default the origin) is loaded, hash the block states of y 40..160 and log
 * it once. Two scratch worlds of the same seed (-Dskycraft.worldName=...) generated with and without a
 * chunk-generation change (C2ME, JVM flags) must log the same hash; the pinned cliff exit depends on it.
 */
public final class WorldHash {
	private static final int RADIUS = Integer.getInteger("portalcraft.worldHash", 0);
	private static final int CX = Integer.getInteger("portalcraft.cliffX", 0) >> 4, CZ = Integer.getInteger("portalcraft.cliffZ", 0) >> 4;
	private static boolean done;
	private static long lastTry;

	private WorldHash() {
	}

	public static void frame(Minecraft minecraft) {
		if (RADIUS <= 0 || done) {
			return;
		}
		ClientLevel level = minecraft.level;
		long now = System.currentTimeMillis();
		if (level == null || now - lastTry < 2000) {
			return;
		}
		lastTry = now;
		for (int x = CX - RADIUS; x <= CX + RADIUS; x++) {
			for (int z = CZ - RADIUS; z <= CZ + RADIUS; z++) {
				if (!level.getChunkSource().hasChunk(x, z)) {
					return;
				}
			}
		}
		long h = 1469598103934665603L;
		BlockPos.MutableBlockPos p = new BlockPos.MutableBlockPos();
		for (int x = (CX - RADIUS) * 16; x < (CX + RADIUS + 1) * 16; x++) {
			for (int z = (CZ - RADIUS) * 16; z < (CZ + RADIUS + 1) * 16; z++) {
				for (int y = 40; y < 160; y++) {
					BlockState s = level.getBlockState(p.set(x, y, z));
					h = (h ^ net.minecraft.world.level.block.Block.getId(s)) * 1099511628211L;
				}
			}
		}
		done = true;
		SkyCraft.LOG.info("Portalcraft: world hash (chunks {} around the cliff, y 40..160): {}", 2 * RADIUS + 1, Long.toHexString(h));
	}
}
