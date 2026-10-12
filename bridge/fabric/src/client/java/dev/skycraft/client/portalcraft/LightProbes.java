package dev.skycraft.client.portalcraft;

import dev.skycraft.SkyCraft;
import dev.skycraft.link.Proto;
import dev.skycraft.link.SkyLink;
import net.minecraft.client.Camera;
import net.minecraft.client.Minecraft;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.client.renderer.state.LightmapRenderState;
import net.minecraft.core.BlockPos;
import net.minecraft.world.attribute.EnvironmentAttributes;
import net.minecraft.world.level.LightLayer;
import org.joml.Vector3fc;

/**
 * Portalcraft (protocol v16): Minecraft's light at the host's probe points, so Portal 2 can light its
 * own objects in the Minecraft region (the portal gun, Chell, cubes) the way Minecraft lights an
 * entity or a held item there: the lightmap colour at the probe's block (sky and block light levels
 * through the lightmap, evaluated on the CPU with the lightmap shader's formula and the inputs
 * Minecraft last gave it), plus the sun's direction and the time of day for a directional term.
 */
public final class LightProbes {
	private static final double[] POS = new double[Proto.MAX_LIGHT_PROBES * 3];
	private static final float[] SAMPLES = new float[Proto.MAX_LIGHT_PROBES * 6];
	private static final int[] ID = new int[1];
	private static final float[] SUN = new float[3], SKY = new float[3], SKY_LIGHT = new float[3];
	private static final BlockPos.MutableBlockPos BP = new BlockPos.MutableBlockPos();
	// the lightmap shader's inputs (LightmapRenderState), as last refreshed
	private static boolean haveState;
	private static float skyFactor = 1, blockFactor = 1.4F, nightVision, darkness, bossDarken, brightness = 0.5F;
	private static final float[] blockTint = {1, 1, 1}, skyLightColor = {1, 1, 1}, ambient = {0, 0, 0}, nightVisionColor = {1, 1, 1};
	private static long lastLog;

	private LightProbes() {
	}

	private static void copy(Vector3fc v, float[] out) {
		out[0] = v.x();
		out[1] = v.y();
		out[2] = v.z();
	}

	/** LightmapCaptureMixin, whenever Minecraft refreshes its lightmap (render thread). */
	public static void capture(LightmapRenderState s) {
		skyFactor = s.skyFactor;
		blockFactor = s.blockFactor;
		nightVision = s.nightVisionEffectIntensity;
		darkness = s.darknessEffectScale;
		bossDarken = s.bossOverlayWorldDarkening;
		brightness = s.brightness;
		copy(s.blockLightTint, blockTint);
		copy(s.skyLightColor, skyLightColor);
		copy(s.ambientColor, ambient);
		copy(s.nightVisionColor, nightVisionColor);
		haveState = true;
	}

	private static float curve(float level) {
		return level / (4.0F - 3.0F * level);
	}

	/** lightmap.fsh for (block, sky) light levels 0..15, into out[o..o+2]. */
	static void lightmap(int blockLevel, int skyLevel, float[] out, int o) {
		float bl = blockLevel / 15.0F, sl = skyLevel / 15.0F;
		float bb = curve(bl) * blockFactor, sb = curve(sl) * skyFactor;
		float mix = 0.9F * (2 * bl - 1) * (2 * bl - 1);
		float max = 0;
		float[] c = new float[3];
		for (int i = 0; i < 3; i++) {
			float v = Math.max(ambient[i], nightVisionColor[i] * nightVision);
			v += skyLightColor[i] * sb;
			v += (blockTint[i] + (1 - blockTint[i]) * mix) * bb;
			float dark = i == 0 ? 0.7F : 0.6F;
			v = v + (v * dark - v) * bossDarken;
			v -= darkness;
			v = Math.max(0, Math.min(1, v));
			c[i] = v;
			max = Math.max(max, v);
		}
		float inv = 1 - max, scaled = 1 - inv * inv * inv * inv;
		for (int i = 0; i < 3; i++) {
			float ng = max > 0 ? c[i] * (scaled / max) : 0;
			out[o + i] = c[i] + (ng - c[i]) * brightness;
		}
	}

	/** Start of every linked Minecraft frame (render thread), after HostViewClient.poll. */
	public static void frame(Minecraft minecraft) {
		ClientLevel level = minecraft.level;
		if (level == null || SkyLink.hostVersion() < 16) {
			return;
		}
		int n = SkyLink.readLightProbes(POS, ID);
		if (n < 0) {
			return;
		}
		for (int i = 0; i < n; i++) {
			BP.set(Math.floor(POS[i * 3]), Math.floor(POS[i * 3 + 1]), Math.floor(POS[i * 3 + 2]));
			int sky = level.getBrightness(LightLayer.SKY, BP);
			int block = level.getBrightness(LightLayer.BLOCK, BP);
			lightmap(block, sky, SAMPLES, i * 6);
			SAMPLES[i * 6 + 3] = sky;
			SAMPLES[i * 6 + 4] = block;
			SAMPLES[i * 6 + 5] = level.canSeeSky(BP) ? 1 : 0;
		}
		Camera camera = minecraft.gameRenderer.mainCamera();
		float sunDeg = 0;
		try {
			sunDeg = camera.attributeProbe().getValue(EnvironmentAttributes.SUN_ANGLE, 1.0F);
			copy(camera.attributeProbe().getValue(EnvironmentAttributes.SKY_COLOR, 1.0F), SKY);
		} catch (Throwable t) {
			// the probe is not ready before the first frame: keep the defaults
		}
		// SkyRenderer: the sun sits at +Y rotated by the sun angle about X, then -90 deg about Y
		double a = Math.toRadians(sunDeg);
		SUN[0] = (float) -Math.sin(a);
		SUN[1] = (float) Math.cos(a);
		SUN[2] = 0;
		System.arraycopy(skyLightColor, 0, SKY_LIGHT, 0, 3);
		float dayTime = (float) Math.floorMod(level.getOverworldClockTime(), 24000L);
		SkyLink.writeMcLight(ID[0], n, SUN, haveState ? skyFactor : 1, SKY, dayTime, SKY_LIGHT, SAMPLES);
		long now = System.currentTimeMillis();
		if (n > 0 && now - lastLog > 10000) {
			lastLog = now;
			SkyCraft.LOG.info("Portalcraft: light probe 0 at {} {} {}: sky {} block {} see-sky {} -> lightmap {} {} {} (sky factor {}, time {})", String.format("%.1f", POS[0]),
				String.format("%.1f", POS[1]), String.format("%.1f", POS[2]), (int) SAMPLES[3], (int) SAMPLES[4], (int) SAMPLES[5], String.format("%.3f", SAMPLES[0]),
				String.format("%.3f", SAMPLES[1]), String.format("%.3f", SAMPLES[2]), String.format("%.3f", skyFactor), (int) dayTime);
		}
	}
}
