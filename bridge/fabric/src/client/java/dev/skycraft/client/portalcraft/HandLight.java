package dev.skycraft.client.portalcraft;

import dev.skycraft.SkyCraft;
import dev.skycraft.link.SkyLink;
import net.minecraft.client.Minecraft;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.level.LightLayer;

/**
 * Portalcraft (bridge #27): the Minecraft player's first-person hand and held item in Portal 2's chambers are lit like a
 * block placed at that spot (terrain.cpp's band light), not by Minecraft's open sky over the chamber band.
 *
 * SkyCraft's hand takes Minecraft's own light where the player stands (the lightmap texel of its block and sky light), and
 * there Minecraft's world is the world the player sees. In a chamber the world is Portal 2's: the host sends its light at the
 * hand (Portal 2's lighting state, linear, times its tonemap scale) and the tonemap scale. Here, as terrain.cpp lights a
 * placed block's vertex: L = max(host light x roof, Curve(block) x 0.3 x warm x tone), roof = lerp(0.3, 1, Curve(sky)) from
 * Minecraft's sky light at the eye (its own blocks overhead darken), the hue kept past 1, then display = L^(1/2.2). The hand
 * is still drawn through Minecraft's lightmap: the (block, sky) texel whose colour is nearest that target is used as the
 * hand's light coordinates, so Minecraft's own item shading, emissive items and its shader stay as they are.
 */
public final class HandLight {
	private static final float[] HOST = new float[4];
	private static final float[] MAP = new float[16 * 16 * 3];
	private static final BlockPos.MutableBlockPos BP = new BlockPos.MutableBlockPos();
	private static final float BLOCK_K = 0.3F;  // terrain.cpp g_bandBlockK
	private static final float[] WARM = {1.0F, 0.85F, 0.65F};
	private static long frameNs;
	private static int coords = -1;
	private static float[] target = new float[3];
	private static long lastLog;
	public static volatile boolean enabled = true;

	private HandLight() {
	}

	private static float curve(float l) {
		return l / (4.0F - 3.0F * l);
	}

	/** The hand's light coordinates in a chamber, or -1 (Minecraft's own). Computed once per frame. */
	public static int coords(int vanilla) {
		if (!enabled) {
			return vanilla;
		}
		long now = System.nanoTime();
		if (now - frameNs < 4_000_000L) {
			return coords < 0 ? vanilla : coords;
		}
		frameNs = now;
		coords = compute();
		return coords < 0 ? vanilla : coords;
	}

	private static int compute() {
		Minecraft mc = Minecraft.getInstance();
		ClientLevel level = mc.level;
		Player player = mc.player;
		if (level == null || player == null || !SkyLink.readHostHandLight(HOST)) {
			return -1;
		}
		BP.set(Math.floor(player.getX()), Math.floor(player.getEyeY()), Math.floor(player.getZ()));
		int sky = level.getBrightness(LightLayer.SKY, BP);
		int block = level.getBrightness(LightLayer.BLOCK, BP);
		float roof = 0.3F + 0.7F * curve(sky / 15.0F);
		float bl = curve(block / 15.0F) * BLOCK_K * HOST[3];
		float m = 0;
		for (int i = 0; i < 3; i++) {
			target[i] = Math.max(HOST[i] * roof, bl * WARM[i]);
			m = Math.max(m, target[i]);
		}
		for (int i = 0; i < 3; i++) {
			if (m > 1) {
				target[i] /= m;  // past full scale: keep the hue (terrain.cpp)
			}
			target[i] = (float) Math.pow(Math.max(0, target[i]), 1 / 2.2);
		}
		// the lightmap's 256 colours as Minecraft shows them now; the nearest by a luma-weighted distance
		int best = 0;
		float bestD = Float.MAX_VALUE;
		for (int b = 0; b < 16; b++) {
			for (int s = 0; s < 16; s++) {
				int o = (b * 16 + s) * 3;
				LightProbes.lightmap(b, s, MAP, o);
				float dr = MAP[o] - target[0], dg = MAP[o + 1] - target[1], db = MAP[o + 2] - target[2];
				float dl = 0.3F * dr + 0.59F * dg + 0.11F * db;
				float d = dl * dl * 4 + dr * dr + dg * dg + db * db;
				if (d < bestD) {
					bestD = d;
					best = b * 16 + s;
				}
			}
		}
		int b = best / 16, s = best % 16;
		long t = System.currentTimeMillis();
		if (t - lastLog > 5000) {
			lastLog = t;
			int o = best * 3;
			SkyCraft.LOG.info("Portalcraft: hand light host ({} {} {}) tone {} sky {} block {} -> target ({} {} {}) -> lightmap block {} sky {} = ({} {} {})",
				f(HOST[0]), f(HOST[1]), f(HOST[2]), f(HOST[3]), sky, block, f(target[0]), f(target[1]), f(target[2]), b, s, f(MAP[o]), f(MAP[o + 1]), f(MAP[o + 2]));
		}
		return (b << 4) | (s << 20);  // LightCoordsUtil.pack(block, sky)
	}

	private static String f(float v) {
		return String.format("%.3f", v);
	}

	/** The target colour (display) last chosen, for tests. */
	public static float[] lastTarget() {
		return target.clone();
	}
}
