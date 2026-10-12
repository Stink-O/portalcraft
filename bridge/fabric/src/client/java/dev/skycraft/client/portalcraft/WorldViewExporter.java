package dev.skycraft.client.portalcraft;

import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.renderpearl.api.buffers.GpuBuffer;
import com.mojang.renderpearl.api.buffers.GpuBufferSlice;
import com.mojang.renderpearl.api.textures.GpuTexture;
import dev.skycraft.SkyCraft;
import dev.skycraft.link.Proto;
import dev.skycraft.link.SkyLink;
import java.awt.image.BufferedImage;
import java.io.File;
import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import javax.imageio.ImageIO;
import net.minecraft.client.Minecraft;

/**
 * Portalcraft: copies Minecraft's level render (colour + depth, taken before the hand and HUD are
 * drawn) back from the GPU for the host's camera and publishes it through the overlay triple
 * buffer as a world-view frame. Asynchronous like FrameExporter: a frame is shipped once the GPU
 * copy has finished, typically one frame later.
 */
public final class WorldViewExporter {
	private static final int STAGING = 6;  // up to 3 frames in flight x 2 cameras
	// The host rebuilds the frame as a depth mesh (parallax + occlusion): a grid of linear depths.
	// Per-pixel depth goes into the colour's alpha (DepthAlphaPass); the depth grid readback (a
	// second, separate copy) only with -Dportalcraft.depthGrid=true.
	private static final boolean DEPTH_ALPHA = !Boolean.getBoolean("portalcraft.noDepthAlpha");
	private static final boolean DEPTH = Boolean.getBoolean("portalcraft.depthGrid");
	private static final int GRID_W = Integer.getInteger("portalcraft.depthGridW", 240), GRID_H = Integer.getInteger("portalcraft.depthGridH", 135);
	private static final int FREE = 0, PENDING = 1, READY = 2;
	private static final Staging[] staging = new Staging[STAGING];
	private static long nextFrameId = 1;
	private static int shipped;

	private static final class Staging {
		GpuBuffer color, depth;
		// Portalcraft depth path: our own pixel-pack buffer + fence (Minecraft's copyTextureToBuffer
		// leaves its read framebuffer's read buffer at GL_NONE after a depth copy).
		int depthPbo;
		long depthFence;
		boolean zZeroToOne;
		boolean depthAlpha;
		float far;
		int width, height;
		volatile int pending;  // copies still in flight
		volatile int state = FREE;
		long frameId;
		int requestId;
		int pass;          // 0 primary camera, 1 second camera (v13)
		int cropW, cropH;  // the shipped sub-rectangle (bottom-left corner of the target)
		boolean shipped;
		boolean screenshot;
		float zNear, zFar;
		final org.joml.Matrix4f proj = new org.joml.Matrix4f();
	}

	private WorldViewExporter() {
	}

	/** Between the level render and the hand/HUD (GameRenderer.renderLevel). */
	public static void capture(Minecraft minecraft) {
		RenderTarget target = minecraft.gameRenderer.mainRenderTarget();
		GpuTexture color = target.getColorTexture();
		GpuTexture depth = target.getDepthTexture();
		if (color == null) {
			return;
		}
		int width = target.width, height = target.height;
		if (width > Proto.MAX_OVERLAY_W || height > Proto.MAX_OVERLAY_H) {
			return;
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
			return; // all in flight; skip this frame
		}
		long bytes = (long) width * height * 4L;
		var device = RenderSystem.getDevice();
		if (slot.color == null || slot.width != width || slot.height != height) {
			if (slot.color != null) {
				slot.color.close();
			}
			if (slot.depth != null) {
				slot.depth.close();
			}
			slot.color = device.createBuffer(() -> "Portalcraft view colour readback", 9, bytes);
			// Depth readback is off for now: Minecraft's GL copyTextureToBuffer leaves its read
			// framebuffer's read buffer at GL_NONE after a depth copy, so the next colour copy fails
			// (GL_INVALID_OPERATION). Needs our own depth path (full-screen compositing with depth).
			slot.depth = null;
			if (slot.depthPbo != 0) {
				org.lwjgl.opengl.GL15.glDeleteBuffers(slot.depthPbo);
				slot.depthPbo = 0;
			}
			slot.width = width;
			slot.height = height;
		}
		SkyLink.HostView v = HostViewClient.view();
		final Staging s = slot;
		s.state = PENDING;
		s.pass = HostViewClient.pass();
		s.frameId = s.pass == 0 ? nextFrameId++ : nextFrameId - 1;
		s.requestId = v.requestId;
		s.shipped = false;
		float[] crop = HostViewClient.crop();
		s.cropW = HostViewClient.cropW(crop, width);
		s.cropH = HostViewClient.cropH(crop, height);
		s.screenshot = HostViewClient.wantsScreenshot();
		var cam = minecraft.gameRenderer.gameRenderState().levelRenderState.cameraRenderState;
		s.zNear = 0.05F;
		s.zFar = cam.depthFar;
		s.pending = 1;
		Runnable done = () -> {
			if (--s.pending == 0) {
				s.state = READY;
			}
		};
		// Depth first, through plain GL into our own buffer: the colour copy's completion then also
		// means the depth copy (issued before it) is done.
		if (DEPTH && depth instanceof com.mojang.renderpearl.backend.opengl.GlTexture glDepth) {
			if (s.depthPbo == 0) {
				s.depthPbo = org.lwjgl.opengl.GL15.glGenBuffers();
				org.lwjgl.opengl.GL15.glBindBuffer(org.lwjgl.opengl.GL21.GL_PIXEL_PACK_BUFFER, s.depthPbo);
				org.lwjgl.opengl.GL15.glBufferData(org.lwjgl.opengl.GL21.GL_PIXEL_PACK_BUFFER, bytes, org.lwjgl.opengl.GL15.GL_STREAM_READ);
			} else {
				org.lwjgl.opengl.GL15.glBindBuffer(org.lwjgl.opengl.GL21.GL_PIXEL_PACK_BUFFER, s.depthPbo);
			}
			org.lwjgl.opengl.GL11.glPixelStorei(org.lwjgl.opengl.GL11.GL_PACK_ALIGNMENT, 4);
			org.lwjgl.opengl.GL45.glGetTextureImage(glDepth.glId(), 0, org.lwjgl.opengl.GL11.GL_DEPTH_COMPONENT, org.lwjgl.opengl.GL11.GL_FLOAT, (int) bytes, 0L);
			org.lwjgl.opengl.GL15.glBindBuffer(org.lwjgl.opengl.GL21.GL_PIXEL_PACK_BUFFER, 0);
			s.zZeroToOne = device.getDeviceInfo().isZZeroToOne();
			s.far = cam.depthFar;
			s.proj.set(cam.projectionMatrix);
		}
		s.depthAlpha = false;
		if (s.pass == 0 && PortalPasses.consumeAlphaDone()) {
			s.depthAlpha = true;  // encoded before the portal openings were drawn (their own cameras' depths inside)
		} else if (DEPTH_ALPHA && depth instanceof com.mojang.renderpearl.backend.opengl.GlTexture glD && color instanceof com.mojang.renderpearl.backend.opengl.GlTexture glC) {
			s.depthAlpha = DepthAlphaPass.run(glC.glId(), glD.glId(), width, height, s.cropW, s.cropH, cam.projectionMatrix, device.getDeviceInfo().isZZeroToOne(), cam.depthFar);
		}
		var encoder = device.createCommandEncoder();
		if (s.cropW == width && s.cropH == height) {
			encoder.copyTextureToBuffer(color, s.color, 0L, done, 0);
		} else {
			encoder.copyTextureToBuffer(color, s.color, 0L, done, 0, 0, 0, s.cropW, s.cropH);
		}
	}

	/** After the frame: map the newest finished readback and copy it into shared memory. */
	public static void ship() {
		Staging newest = null, second = null;
		for (Staging s : staging) {
			if (s != null && s.state == READY && s.pass == 0 && (newest == null || s.frameId > newest.frameId)) {
				newest = s;
			}
		}
		for (Staging s : staging) {
			if (s != null && s.state == READY && s.pass == 1 && !s.shipped && (second == null || s.frameId > second.frameId)) {
				second = s;
			}
		}
		if (newest == null) {
			return;
		}
		MemorySegment shm = SkyLink.segment();
		if (shm != null) {
			long bytes = (long) newest.cropW * newest.cropH * 4L;
			long base = SkyLink.overlayBackSlotOffset();
			try (GpuBufferSlice.MappedView view = newest.color.map(true, false)) {
				MemorySegment src = MemorySegment.ofBuffer(view.data());
				MemorySegment.copy(src, 0, shm, base, Math.min(bytes, src.byteSize()));
				// One PNG per screenshot request (the host holds the flag for a few frames).
				if (newest.screenshot && !shotLatch) {
					shotLatch = true;
					saveScreenshot(view.data(), newest.cropW, newest.cropH, newest.requestId);
				} else if (!newest.screenshot) {
					shotLatch = false;
				}
			}
			long depthOffset = 0;
			if (newest.depthPbo != 0) {
				org.lwjgl.opengl.GL15.glBindBuffer(org.lwjgl.opengl.GL21.GL_PIXEL_PACK_BUFFER, newest.depthPbo);
				ByteBuffer d = org.lwjgl.opengl.GL30.glMapBufferRange(org.lwjgl.opengl.GL21.GL_PIXEL_PACK_BUFFER, 0, bytes, org.lwjgl.opengl.GL30.GL_MAP_READ_BIT);
				if (d != null) {
					depthOffset = bytes;
					writeDepthGrid(d.order(ByteOrder.LITTLE_ENDIAN), shm, base + depthOffset, newest);
					org.lwjgl.opengl.GL15.glUnmapBuffer(org.lwjgl.opengl.GL21.GL_PIXEL_PACK_BUFFER);
				}
				org.lwjgl.opengl.GL15.glBindBuffer(org.lwjgl.opengl.GL21.GL_PIXEL_PACK_BUFFER, 0);
			}
			// v13: the second camera's image after the first (page aligned), same slot
			long img2 = 0;
			if (second != null) {
				img2 = (bytes + depthBytes(depthOffset) + 4095L) & ~4095L;
				long bytes2 = (long) second.cropW * second.cropH * 4L;
				if (img2 + bytes2 <= Proto.OVERLAY_SLOT_BYTES) {
					try (GpuBufferSlice.MappedView view2 = second.color.map(true, false)) {
						MemorySegment src2 = MemorySegment.ofBuffer(view2.data());
						MemorySegment.copy(src2, 0, shm, base + img2, Math.min(bytes2, src2.byteSize()));
					}
					second.shipped = true;
				} else {
					img2 = 0;
				}
			}
			SkyLink.setWorldViewImage2(img2, img2 != 0 ? second.cropW : 0, img2 != 0 ? second.cropH : 0, img2 != 0 ? second.requestId : 0, img2 != 0 && second.depthAlpha);
			SkyLink.publishWorldView(newest.cropW, newest.cropH, newest.frameId, newest.requestId, depthOffset, newest.zNear, newest.zFar, depthOffset != 0 ? GRID_W : 0,
				depthOffset != 0 ? GRID_H : 0, newest.depthAlpha);
			if (shipped++ < 3) {
				SkyCraft.LOG.info("Portalcraft: world view frame {} for request {} ({}x{}, depth {})", newest.frameId, newest.requestId, newest.cropW, newest.cropH,
					depthOffset != 0);
			}
		}
		for (Staging s : staging) {
			if (s != null && s.state == READY && s.frameId <= newest.frameId && (s.pass == 0 || s.shipped || s.frameId < newest.frameId)) {
				s.state = FREE;
			}
		}
	}

	private static long depthBytes(long depthOffset) {
		return depthOffset != 0 ? (long) GRID_W * GRID_H * 4L : 0L;
	}

	private static boolean shotLatch;

	/**
	 * The depth grid the host meshes: GRID_W x GRID_H samples at pixel (i * (w - 1) / (GRID_W - 1),
	 * j * (h - 1) / (GRID_H - 1)) in GL row order, each window depth unprojected with this frame's own
	 * projection matrix (the oblique near plane of portal views included) to the distance along the
	 * view axis in blocks; 0 where nothing was drawn (sky).
	 */
	private static final org.joml.Matrix4f inv = new org.joml.Matrix4f();
	private static final org.joml.Vector4f tmp = new org.joml.Vector4f();
	private static float[] grid;
	private static void writeDepthGrid(ByteBuffer src, MemorySegment shm, long at, Staging st) {
		if (grid == null || grid.length != GRID_W * GRID_H) {
			grid = new float[GRID_W * GRID_H];
		}
		inv.set(st.proj).invert();
		final int w = st.width, h = st.height;
		final float far = st.far;
		for (int j = 0; j < GRID_H; j++) {
			int py = Math.round(j * (h - 1) / (float) (GRID_H - 1));
			float ny = (py + 0.5F) / h * 2 - 1;
			for (int i = 0; i < GRID_W; i++) {
				int px = Math.round(i * (w - 1) / (float) (GRID_W - 1));
				float nx = (px + 0.5F) / w * 2 - 1;
				float wz = src.getFloat((py * w + px) * 4);
				float nz = st.zZeroToOne ? wz : 2 * wz - 1;
				tmp.set(nx, ny, nz, 1.0F);
				inv.transform(tmp);
				float dist = Math.abs(tmp.w) > 1e-12F ? -tmp.z / tmp.w : 0.0F;
				if (!(dist > 0.0F) || dist > far * 0.98F || Float.isNaN(dist)) {
					dist = 0.0F;
				}
				grid[j * GRID_W + i] = dist;
			}
		}
		MemorySegment.copy(MemorySegment.ofArray(grid), 0, shm, at, (long) grid.length * 4);
	}

	/**
	 * Window depth (reversed-Z perspective, near = 0.05, far = depthFar, before any oblique plane)
	 * to linear depth along the view axis in blocks, which is what the host needs. Pixels at the
	 * far plane (sky) become 0. The view with an oblique near plane (portal views) is not exact.
	 */
	private static void writeLinearDepth(ByteBuffer src, MemorySegment shm, long at, Staging st) {
		float n = st.far, f = 0.05F; // JOML setPerspective(near = zFar, far = zNear): reversed
		int count = st.width * st.height;
		float[] row = new float[st.width];
		for (int i = 0; i < count; i += st.width) {
			for (int x = 0; x < st.width; x++) {
				float w = src.getFloat((i + x) * 4);
				float z = st.zZeroToOne ? w : 2 * w - 1;
				float dist;
				if (w <= 0.0F) {
					dist = 0.0F;
				} else if (st.zZeroToOne) {
					// z = (f/(n-f)) ... solved: d = f*n / (f + z*(n - f))... for JOML zZeroToOne with near n, far f
					dist = (f * n) / (f + z * (n - f));
				} else {
					dist = (2 * f * n) / ((f + n) - z * (f - n));
				}
				row[x] = dist;
			}
			MemorySegment.copy(MemorySegment.ofArray(row), 0, shm, at + (long) i * 4, (long) st.width * 4);
		}
	}

	/** Minecraft's own image of this camera, for side-by-side checks against the composite. */
	private static void saveScreenshot(ByteBuffer data, int w, int h, int requestId) {
		try {
			ByteBuffer b = data.duplicate().order(ByteOrder.LITTLE_ENDIAN);
			BufferedImage img = new BufferedImage(w, h, BufferedImage.TYPE_INT_RGB);
			for (int y = 0; y < h; y++) {
				int row = (h - 1 - y) * w * 4; // GL rows are bottom-up
				for (int x = 0; x < w; x++) {
					int o = row + x * 4;
					int r = b.get(o) & 255, g = b.get(o + 1) & 255, bl = b.get(o + 2) & 255;
					img.setRGB(x, y, (r << 16) | (g << 8) | bl);
				}
			}
			File dir = new File(System.getProperty("portalcraft.shotDir", "../../../cache/bridge/mcview"));
			dir.mkdirs();
			File out = new File(dir, "mcview_" + requestId + ".png");
			ImageIO.write(img, "png", out);
			SkyCraft.LOG.info("Portalcraft: saved Minecraft's view {}", out.getCanonicalPath());
		} catch (Exception e) {
			SkyCraft.LOG.error("Portalcraft: view screenshot failed", e);
		}
	}
}
