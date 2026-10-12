package dev.skycraft.client.portalcraft;

import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.renderpearl.api.buffers.GpuBuffer;
import com.mojang.renderpearl.api.buffers.GpuBufferSlice;
import com.mojang.renderpearl.api.textures.GpuTexture;
import dev.skycraft.SkyCraft;
import java.awt.image.BufferedImage;
import java.io.File;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import javax.imageio.ImageIO;
import net.minecraft.client.Minecraft;

/**
 * Portalcraft (bridge #18, parity sheets): Minecraft's own view of the level from the Minecraft player's camera, saved
 * as a PNG, for side-by-side checks against what Portal 2 draws at the same pose (the chamber band, particles, block
 * entities, water). The plugin asks with a request file (cache/bridge/mcnative/request holding a name); the main
 * target is copied right before the hand pass (after the level, before the HUD clears it) and written as
 * cache/bridge/mcnative/mcnative_&lt;name&gt;.png. No protocol change: the file is polled every 10 frames.
 */
public final class NativeShot {
	private static final File DIR = new File(System.getProperty("portalcraft.nativeDir", "../../../cache/bridge/mcnative"));
	private static int frame;
	private static volatile boolean busy;

	private NativeShot() {
	}

	/** Right before the hand pass, on the render thread: the main target holds the player's level image. */
	public static void maybeCapture(Minecraft minecraft) {
		if (busy || ++frame % 10 != 0) {
			return;
		}
		File req = new File(DIR, "request");
		if (!req.isFile()) {
			return;
		}
		String name;
		try {
			name = new String(Files.readAllBytes(req.toPath())).trim().replaceAll("[^A-Za-z0-9_\\-]", "");
			Files.deleteIfExists(req.toPath());
		} catch (Exception e) {
			return;
		}
		if (name.isEmpty()) {
			name = "shot";
		}
		RenderTarget target = minecraft.gameRenderer.mainRenderTarget();
		GpuTexture color = target.getColorTexture();
		if (color == null) {
			return;
		}
		final int w = target.width, h = target.height;
		final GpuBuffer buf = RenderSystem.getDevice().createBuffer(() -> "Portalcraft native shot", 9, (long) w * h * 4L);
		final String n = name;
		busy = true;
		RenderSystem.getDevice().createCommandEncoder().copyTextureToBuffer(color, buf, 0L, () -> {
			try (GpuBufferSlice.MappedView view = buf.map(true, false)) {
				ByteBuffer b = view.data().duplicate().order(ByteOrder.LITTLE_ENDIAN);
				BufferedImage img = new BufferedImage(w, h, BufferedImage.TYPE_INT_RGB);
				for (int y = 0; y < h; y++) {
					int row = (h - 1 - y) * w * 4; // GL rows are bottom-up
					for (int x = 0; x < w; x++) {
						int o = row + x * 4;
						img.setRGB(x, y, ((b.get(o) & 255) << 16) | ((b.get(o + 1) & 255) << 8) | (b.get(o + 2) & 255));
					}
				}
				DIR.mkdirs();
				File out = new File(DIR, "mcnative_" + n + ".png");
				ImageIO.write(img, "png", out);
				SkyCraft.LOG.info("Portalcraft: saved Minecraft's native view {}", out.getCanonicalPath());
			} catch (Exception e) {
				SkyCraft.LOG.error("Portalcraft: native shot failed", e);
			} finally {
				buf.close();
				busy = false;
			}
		}, 0);
	}
}
