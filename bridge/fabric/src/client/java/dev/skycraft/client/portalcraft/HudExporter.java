package dev.skycraft.client.portalcraft;

import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.renderpearl.api.buffers.GpuBuffer;
import com.mojang.renderpearl.api.buffers.GpuBufferSlice;
import com.mojang.renderpearl.api.textures.GpuTexture;
import dev.skycraft.SkyCraft;
import dev.skycraft.link.Proto;
import dev.skycraft.link.SkyLink;
import java.lang.foreign.MemorySegment;
import net.minecraft.client.Minecraft;
import org.joml.Vector4f;

/**
 * Portalcraft v17 (DESIGN-mcplayer M2, SkyCraft parity): the Minecraft player's own first-person hand, held item
 * and GUI, shipped every frame to the host through the HUD triple buffer, as SkyCraft's FrameExporter ships its
 * overlay. While the host asks (HostPlayer kHpWantHud), the main target is cleared to transparent right before
 * the hand pass (after the level and any host-view capture), so only the hand, the GUI and screens land in it.
 * Asynchronous readback like FrameExporter: a frame is shipped once the GPU copy finished, typically a frame later.
 */
public final class HudExporter {
	private static final int STAGING = 3;
	private static final int FREE = 0;
	private static final int PENDING = 1;
	private static final int READY = 2;
	private static final Vector4f CLEAR = new Vector4f(0.0F, 0.0F, 0.0F, 0.0F);

	private static final Staging[] staging = new Staging[STAGING];
	private static final int[] hostPlayer = new int[5];
	private static long nextFrameId = 1;
	private static boolean wanted;
	private static boolean handPass;
	private static boolean logged;

	private static final class Staging {
		GpuBuffer buffer;
		int width;
		int height;
		volatile int state = FREE;
		long frameId;
		int guiScale;
		int mcFlags;
		int cameraMode;
	}

	private HudExporter() {
	}

	/** Once per frame, before rendering: does the host want the HUD image? */
	public static void update() {
		boolean w = SkyLink.hudAvailable() && SkyLink.readHostPlayer(hostPlayer) && (hostPlayer[0] & Proto.HP_WANT_HUD) != 0;
		if (w != wanted) {
			SkyCraft.LOG.info("Portalcraft: HUD export {}", w ? "on" : "off");
		}
		wanted = w;
	}

	public static boolean wanted() {
		return wanted;
	}

	/** The hand pass (render3dHud) is running: walk bob applies to the hand even while the host drives the camera. */
	public static boolean handPass() {
		return handPass;
	}

	public static void setHandPass(boolean on) {
		handPass = on;
	}

	/** Right before the hand pass: the level (and the host-view capture) is done; start the HUD on transparent. */
	public static void clearForHud(Minecraft minecraft) {
		NativeShot.maybeCapture(minecraft);  // bridge #18: Minecraft's own view for the parity sheets (tests)
		if (!wanted) {
			return;
		}
		RenderTarget target = minecraft.gameRenderer.mainRenderTarget();
		if (target.getColorTexture() == null || target.getDepthTexture() == null) {
			return;
		}
		RenderSystem.getDevice().createCommandEncoder().clearColorAndDepthTextures(target.getColorTexture(), CLEAR, target.getDepthTexture(), 0.0);
	}

	/** End of the frame: copy the main target (hand + GUI on transparent) back and ship what is ready. */
	public static void capture(Minecraft minecraft, int mcFlags) {
		shipReadyFrames();
		if (!wanted) {
			return;
		}
		RenderTarget target = minecraft.gameRenderer.mainRenderTarget();
		GpuTexture color = target.getColorTexture();
		if (color == null) {
			return;
		}
		int width = target.width;
		int height = target.height;
		if (width > Proto.MAX_HUD_W || height > Proto.MAX_HUD_H) {
			return;
		}
		if (!logged) {
			logged = true;
			SkyCraft.LOG.info("Portalcraft: HUD capture {}x{} format {}", width, height, color.getFormat());
		}
		Staging slot = null;
		for (int i = 0; i < STAGING && slot == null; i++) {
			if (staging[i] == null) {
				staging[i] = new Staging();
			}
			if (staging[i].state == FREE) {
				slot = staging[i];
			}
		}
		if (slot == null) {
			return; // all in flight: skip this frame
		}
		long bytes = (long) width * height * 4L;
		if (slot.buffer == null || slot.width != width || slot.height != height) {
			if (slot.buffer != null) {
				slot.buffer.close();
			}
			slot.buffer = RenderSystem.getDevice().createBuffer(() -> "Portalcraft HUD readback", 9, bytes);
			slot.width = width;
			slot.height = height;
		}
		final Staging captured = slot;
		captured.state = PENDING;
		captured.frameId = nextFrameId++;
		captured.guiScale = minecraft.getWindow().getGuiScale();
		captured.mcFlags = mcFlags;
		captured.cameraMode = minecraft.options.getCameraType().ordinal();
		RenderSystem.getDevice().createCommandEncoder().copyTextureToBuffer(color, captured.buffer, 0L, () -> captured.state = READY, 0);
	}

	private static void shipReadyFrames() {
		Staging newest = null;
		for (Staging s : staging) {
			if (s != null && s.state == READY && (newest == null || s.frameId > newest.frameId)) {
				newest = s;
			}
		}
		if (newest == null) {
			return;
		}
		MemorySegment shm = SkyLink.segment();
		if (shm != null && SkyLink.hudAvailable()) {
			long bytes = (long) newest.width * newest.height * 4L;
			try (GpuBufferSlice.MappedView view = newest.buffer.map(true, false)) {
				MemorySegment src = MemorySegment.ofBuffer(view.data());
				MemorySegment.copy(src, 0, shm, SkyLink.hudBackSlotOffset(), Math.min(bytes, src.byteSize()));
			}
			SkyLink.publishHud(newest.width, newest.height, newest.frameId, newest.guiScale, newest.mcFlags, newest.cameraMode);
		}
		for (Staging s : staging) {
			if (s != null && s.state == READY && s.frameId <= newest.frameId) {
				s.state = FREE;
			}
		}
	}
}
