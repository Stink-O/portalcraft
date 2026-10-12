package dev.skycraft.client.portalcraft;

import com.mojang.blaze3d.ProjectionType;
import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.blaze3d.pipeline.TextureTarget;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.renderpearl.api.GpuFormat;
import com.mojang.renderpearl.api.buffers.GpuBufferSlice;
import com.mojang.renderpearl.backend.opengl.GlTexture;
import dev.skycraft.SkyCraft;
import dev.skycraft.client.mixin.CloudRendererAccessor;
import dev.skycraft.client.mixin.GameRendererAccessor;
import dev.skycraft.client.mixin.LevelRendererPassAccessor;
import dev.skycraft.client.mixin.SectionOcclusionGraphAccessor;
import dev.skycraft.link.SkyLink;
import net.minecraft.client.DeltaTracker;
import net.minecraft.client.Minecraft;
import net.minecraft.client.TextureFilteringMethod;
import net.minecraft.client.renderer.CloudRenderer;
import net.minecraft.client.renderer.GameRenderer;
import net.minecraft.client.renderer.ProjectionMatrixBuffer;
import net.minecraft.client.renderer.fog.FogData;
import net.minecraft.client.renderer.fog.FogRenderer;
import net.minecraft.client.renderer.state.GameRenderState;
import net.minecraft.client.renderer.state.level.CameraRenderState;
import org.joml.Matrix3f;
import org.joml.Matrix4f;
import org.joml.Quaternionf;
import org.joml.Vector4f;

/**
 * Portalcraft (protocol v15, DESIGN-mcportals.md): when both portals of the live pair are in the
 * Minecraft region, Minecraft draws the views through them into its own frame, so the host
 * composites one image whose portal contents are exactly as old as the rest of it.
 *
 * <p>In the primary camera's pass, after the level and before the frame is captured:
 * <ol>
 * <li>the primary's depth goes into its alpha (DepthAlphaPass), so pixels outside openings carry it;
 * <li>for each portal facing the camera and on screen, the level is rendered again from the
 * virtual camera (the primary camera carried through the portal pair: entrance to exit, turned half
 * a turn about the portal's up axis), with an oblique near plane on the exit portal and the opening's
 * screen box as its culling frustum, at the pass render distance, into a target of its own (reused
 * every frame; Minecraft's main target has no stencil). Its depth goes into its alpha;
 * <li>inside that pass the same happens again for the portal seen through it, up to the host's
 * recursion depth (Portal 2: 2); a portal seen at the last level is filled with its flat colour;
 * <li>each pass's target is drawn into its parent through the opening (PortalComposite): Portal 2's
 * ellipse at the current open scale, depth-tested against the parent, so terrain in front of a
 * portal hides it. Colour and alpha are copied, so inside an opening the alpha carries the virtual
 * camera's depth, as the host's occlusion layers for that portal view need.
 * </ol>
 * Every pass has its own fog and cloud buffers (one FogRenderer and one CloudRenderer per pass,
 * moved on once per frame with the main ones): vanilla writes its cloud offsets into a persistently
 * mapped buffer once per frame, and a second level pass in the same frame overwrote what the first
 * pass's draw reads (the clouds twitched in portal views).
 */
public final class PortalPasses {
	/** Pass targets: [i] the view through portal i (depth 1), [2 + i] through portal i again (depth 2). */
	private static final int SLOTS = 4;
	/** The second camera's pass (v13 hosts, mixed pairs) uses buffer set SECOND. */
	public static final int SECOND = SLOTS;
	private static final TextureTarget[] targets = new TextureTarget[SLOTS];
	private static final FogRenderer[] fogs = new FogRenderer[SLOTS + 1];
	private static final CloudRenderer[] clouds = new CloudRenderer[SLOTS + 1];
	private static final ProjectionMatrixBuffer[] projBufs = new ProjectionMatrixBuffer[SLOTS];
	private static final boolean[] fogUsed = new boolean[SLOTS + 1], cloudUsed = new boolean[SLOTS + 1];
	private static RenderTarget override;
	private static boolean alphaDone;
	private static boolean failed;
	private static final float EPS = 0.004F;  // the opening sits this far (blocks) in front of the surface

	/** A camera of this frame: the primary (cams[0]) or a pass's virtual camera (cams[1 + slot]). */
	private static final class Cam {
		double x, y, z;
		final Quaternionf rot = new Quaternionf();
		final Matrix4f proj = new Matrix4f();     // final projection: crop translation, oblique plane
		final Matrix4f viewRot = new Matrix4f();
		final Matrix4f boxProj = new Matrix4f();  // plain perspective x view rotation (pre-crop NDC)
		final float[] box = new float[4];         // its pixels: window of the pre-crop NDC
		float far;
		RenderTarget target;
	}

	private static final Cam[] cams = {new Cam(), new Cam(), new Cam(), new Cam(), new Cam()};
	private static final Matrix3f R = new Matrix3f(), Bi = new Matrix3f(), Bj = new Matrix3f();
	private static final Quaternionf Rq = new Quaternionf();
	private static final Vector4f V4 = new Vector4f();
	private static final float[][] boxes = new float[SLOTS][4];  // a pass's window before it is entered
	private static final float[] plane = new float[4], centre = new float[3], axisR = new float[3], axisU = new float[3];
	// statistics
	private static long frames, passCount, nanos, lastLog;
	private static int maxPasses;
	private static int debugLog = Integer.getInteger("portalcraft.portalDebug", 20);

	private PortalPasses() {
	}

	/** GameRendererTargetMixin: the target the current pass renders into (null: Minecraft's own). */
	public static RenderTarget targetOverride() {
		return override;
	}

	/** WorldViewExporter: the primary's depth is already in its alpha (the portal passes did it). */
	public static boolean consumeAlphaDone() {
		boolean d = alphaDone;
		alphaDone = false;
		return d;
	}

	/** Once per frame (LevelRenderer.endFrame): the extra passes' fog and cloud buffers move on. */
	public static void endFrame() {
		for (int i = 0; i <= SLOTS; i++) {
			if (fogUsed[i] && fogs[i] != null) {
				fogs[i].endFrame();
			}
			if (cloudUsed[i] && clouds[i] != null) {
				clouds[i].endFrame();
			}
			fogUsed[i] = cloudUsed[i] = false;
		}
	}

	/** The pass's own CloudRenderer, sharing the main one's cloud texture. */
	private static CloudRenderer cloud(int i, CloudRenderer main) {
		if (clouds[i] == null) {
			clouds[i] = new CloudRenderer();
		}
		CloudRendererAccessor own = (CloudRendererAccessor) clouds[i];
		CloudRenderer.TextureData t = ((CloudRendererAccessor) main).portalcraft$texture();
		if (own.portalcraft$texture() != t) {
			own.portalcraft$setTexture(t);
			clouds[i].markForRebuild();
		}
		cloudUsed[i] = true;
		return clouds[i];
	}

	private static FogRenderer fog(int i) {
		if (fogs[i] == null) {
			fogs[i] = new FogRenderer();
		}
		fogUsed[i] = true;
		return fogs[i];
	}

	/** Runs {@code r} (a level render) with buffer set {@code i}'s clouds (the second camera's pass). */
	public static void withOwnClouds(int i, Runnable r) {
		LevelRendererPassAccessor lr = (LevelRendererPassAccessor) Minecraft.getInstance().levelRenderer;
		CloudRenderer main = lr.portalcraft$cloudRenderer();
		lr.portalcraft$setCloudRenderer(cloud(i, main));
		try {
			r.run();
		} finally {
			lr.portalcraft$setCloudRenderer(main);
		}
	}

	// ---- portal frames ------------------------------------------------------------------------------
	private static void basis(SkyLink.HostPortal p, Matrix3f b) {
		// columns: right = up x normal, up, normal
		float rx = p.uy * p.nz - p.uz * p.ny, ry = p.uz * p.nx - p.ux * p.nz, rz = p.ux * p.ny - p.uy * p.nx;
		b.set(rx, ry, rz, p.ux, p.uy, p.uz, p.nx, p.ny, p.nz);
	}

	/** The camera {@code from} seen through portal i comes out of portal j: {@code to}. */
	private static void through(SkyLink.HostPortals ps, int i, Cam from, Cam to) {
		SkyLink.HostPortal a = ps.portal[i], b = ps.portal[1 - i];
		basis(a, Bi);
		basis(b, Bj);
		// R = Bj * diag(-1, 1, -1) * Bi^T: normal -> -normal, up -> up, right -> -right
		R.set(Bi).transpose();
		R.m00(-R.m00()).m10(-R.m10()).m20(-R.m20());
		R.m02(-R.m02()).m12(-R.m12()).m22(-R.m22());
		R.mulLocal(Bj);
		double dx = from.x - a.cx, dy = from.y - a.cy, dz = from.z - a.cz;
		to.x = b.cx + R.m00() * dx + R.m10() * dy + R.m20() * dz;
		to.y = b.cy + R.m01() * dx + R.m11() * dy + R.m21() * dz;
		to.z = b.cz + R.m02() * dx + R.m12() * dy + R.m22() * dz;
		Rq.setFromNormalized(R);
		Rq.mul(from.rot, to.rot);
	}

	private static void setBoxProj(Cam c, float fovY, float aspect, float far) {
		c.boxProj.setPerspective(fovY * (float) (Math.PI / 180.0), aspect, 0.05F, far, RenderSystem.getDevice().getDeviceInfo().isZZeroToOne());
		c.boxProj.rotate(V_ROT.set(c.rot).conjugate());
	}

	private static final Quaternionf V_ROT = new Quaternionf();

	/**
	 * Portal i on cam's screen: false if it shows nothing there. {@code out} = its box (pre-crop NDC)
	 * within {@code parent}; returns with FULL set when the camera sits in the opening itself.
	 */
	private static int visible(SkyLink.HostPortal p, Cam cam, float[] parent, float[] out) {
		double qx = cam.x - p.cx, qy = cam.y - p.cy, qz = cam.z - p.cz;
		double d = qx * p.nx + qy * p.ny + qz * p.nz;
		if (p.halfW <= 1e-3F || p.halfH <= 1e-3F) {
			return NONE;
		}
		if (d < 0.1 && d > -1.0) {
			qx -= p.nx * d;
			qy -= p.ny * d;
			qz -= p.nz * d;
			float rx = p.uy * p.nz - p.uz * p.ny, ry = p.uz * p.nx - p.ux * p.nz, rz = p.ux * p.ny - p.uy * p.nx;
			double lx = (qx * rx + qy * ry + qz * rz) / p.halfW, ly = (qx * p.ux + qy * p.uy + qz * p.uz) / p.halfH;
			if (lx * lx + ly * ly <= 1.0) {
				System.arraycopy(parent, 0, out, 0, 4);
				return FULL;
			}
		}
		if (d <= 0.0) {
			return NONE;
		}
		float rx = p.uy * p.nz - p.uz * p.ny, ry = p.uz * p.nx - p.ux * p.nz, rz = p.ux * p.ny - p.uy * p.nx;
		float x0 = 2, y0 = 2, x1 = -2, y1 = -2;
		boolean behind = false;
		for (int k = 0; k < 32; k++) {
			double t = 2 * Math.PI * k / 32;
			float cs = (float) Math.cos(t) * p.halfW * 1.03F, sn = (float) Math.sin(t) * p.halfH * 1.03F;
			V4.set((float) (p.cx - cam.x) + rx * cs + p.ux * sn, (float) (p.cy - cam.y) + ry * cs + p.uy * sn, (float) (p.cz - cam.z) + rz * cs + p.uz * sn, 1.0F);
			cam.boxProj.transform(V4);
			if (V4.w <= 1e-4F) {
				behind = true;
				break;
			}
			float x = V4.x / V4.w, y = V4.y / V4.w;
			x0 = Math.min(x0, x);
			x1 = Math.max(x1, x);
			y0 = Math.min(y0, y);
			y1 = Math.max(y1, y);
		}
		if (behind) {
			System.arraycopy(parent, 0, out, 0, 4);
			return PART;
		}
		out[0] = Math.max(parent[0], x0);
		out[1] = Math.max(parent[1], y0);
		out[2] = Math.min(parent[2], x1);
		out[3] = Math.min(parent[3], y1);
		return out[0] < out[2] && out[1] < out[3] ? PART : NONE;
	}

	private static final int NONE = 0, PART = 1, FULL = 2;
	private static int hiddenSkips;

	/**
	 * The opening is behind terrain from this camera (in open air): rays to its centre and 8 points
	 * around its edge all stop on blocks short of it. Its pass would be fully hidden by the depth test,
	 * so it is skipped (a cliff portal seen from the far side of a ridge).
	 */
	private static boolean hiddenByTerrain(Minecraft minecraft, SkyLink.HostPortal p, Cam cam) {
		if (minecraft.level == null) {
			return false;
		}
		float rx = p.uy * p.nz - p.uz * p.ny, ry = p.uz * p.nx - p.ux * p.nz, rz = p.ux * p.ny - p.uy * p.nx;
		net.minecraft.world.phys.Vec3 from = new net.minecraft.world.phys.Vec3(cam.x, cam.y, cam.z);
		for (int k = 0; k < 9; k++) {
			double cs = k == 0 ? 0 : Math.cos(2 * Math.PI * k / 8) * 0.85, sn = k == 0 ? 0 : Math.sin(2 * Math.PI * k / 8) * 0.85;
			double tx = p.cx + p.nx * 0.05 + rx * cs * p.halfW + p.ux * sn * p.halfH;
			double ty = p.cy + p.ny * 0.05 + ry * cs * p.halfW + p.uy * sn * p.halfH;
			double tz = p.cz + p.nz * 0.05 + rz * cs * p.halfW + p.uz * sn * p.halfH;
			net.minecraft.world.phys.Vec3 to = new net.minecraft.world.phys.Vec3(tx, ty, tz);
			var hit = minecraft.level.clip(new net.minecraft.world.level.ClipContext(from, to, net.minecraft.world.level.ClipContext.Block.VISUAL,
				net.minecraft.world.level.ClipContext.Fluid.NONE, net.minecraft.world.phys.shapes.CollisionContext.empty()));
			if (hit.getType() == net.minecraft.world.phys.HitResult.Type.MISS || hit.getLocation().distanceToSqr(to) < 0.01) {
				return false;
			}
		}
		hiddenSkips++;
		return true;
	}
	private static final float[] FLAT_BOX = new float[4];

	// ---- the frame ------------------------------------------------------------------------------------
	/**
	 * In the primary camera's pass, after its level render (GameRendererViewMixin). True when the
	 * pair is drawn by Minecraft this frame (the primary's alpha then already holds its depth).
	 */
	public static boolean frame(Minecraft minecraft, GameRenderer renderer) {
		if (failed || !HostViewClient.portalsActive() || HostViewClient.pass() != 0) {
			return false;
		}
		long t0 = System.nanoTime();
		SkyLink.HostPortals ps = HostViewClient.portals();
		GameRenderState st = renderer.gameRenderState();
		CameraRenderState cs = st.levelRenderState.cameraRenderState;
		RenderTarget main = renderer.mainRenderTarget();
		if (!(main.getColorTexture() instanceof GlTexture) || !(main.getDepthTexture() instanceof GlTexture)) {
			failed = true;
			SkyCraft.LOG.error("Portalcraft: portal passes need the OpenGL backend; off");
			return false;
		}
		Cam p = cams[0];
		p.x = cs.pos.x;
		p.y = cs.pos.y;
		p.z = cs.pos.z;
		p.rot.set(cs.orientation);
		p.proj.set(cs.projectionMatrix);
		p.viewRot.set(cs.viewRotationMatrix);
		p.far = cs.depthFar;
		p.target = main;
		final float fovY = HostViewClient.primary().fovY, aspect = main.width / (float) main.height;
		setBoxProj(p, fovY, aspect, p.far);
		System.arraycopy(HostViewClient.crop(), 0, p.box, 0, 4);
		boolean z01 = RenderSystem.getDevice().getDeviceInfo().isZZeroToOne();
		float[] crop = HostViewClient.crop();
		// 1) the primary's depth into its alpha (the openings then carry their own cameras' depths)
		DepthAlphaPass.run(((GlTexture) main.getColorTexture()).glId(), ((GlTexture) main.getDepthTexture()).glId(), main.width, main.height,
			HostViewClient.cropW(crop, main.width), HostViewClient.cropH(crop, main.height), p.proj, z01, p.far);
		alphaDone = true;
		int passes = 0;
		final int maxDepth = Math.max(1, Math.min(2, ps.maxDepth));
		DeltaTracker dt = minecraft.getDeltaTracker();
		float partial = st.levelRenderState.worldPartialTicks;
		try {
			for (int i = 0; i < 2; i++) {
				int vis = visible(ps.portal[i], p, p.box, boxes[i]);
				if (vis == NONE || (vis == PART && hiddenByTerrain(minecraft, ps.portal[i], p))) {
					continue;
				}
				if (ps.maxDepth == 0) {
					// (measurements: the host asks for the opening as a flat colour, where Minecraft's opening ends)
					opening(ps.portal[i], p, main, -1, 1.0F, vis == FULL, ps.portal[i].colour & 0xFFFFFF);
					continue;
				}
				passes += pass(minecraft, renderer, ps, i, i, 1, maxDepth, p, boxes[i], vis == FULL, fovY, aspect, z01);
			}
		} catch (RuntimeException e) {
			failed = true;
			SkyCraft.LOG.error("Portalcraft: portal pass failed; portal passes off", e);
		} finally {
			override = null;
			if (passes > 0) {
				// the primary camera again (its render state: the hand pass, the capture)
				HostViewClient.setPass(0);
				renderer.mainCamera().update(dt);
				((GameRendererAccessor) renderer).portalcraft$extractCamera(dt, partial);
				((GameRendererAccessor) renderer).portalcraft$globalSettings().update(st.windowRenderState.width, st.windowRenderState.height, st.optionsRenderState.glintStrength,
					st.levelRenderState.gameTime, partial, st.optionsRenderState.menuBackgroundBlurriness, st.levelRenderState.cameraRenderState.pos,
					st.optionsRenderState.textureFiltering == TextureFilteringMethod.RGSS);
				// the next frame's primary rebuilds its visible sections for its own frustum
				((SectionOcclusionGraphAccessor) minecraft.levelRenderer.sectionOcclusionGraph()).portalcraft$needsFrustumUpdate().set(true);
			}
			HostViewClient.setPass(0);
		}
		frames++;
		passCount += passes;
		maxPasses = Math.max(maxPasses, passes);
		nanos += System.nanoTime() - t0;
		if (frames - lastLog >= 1200 || frames == 1) {
			SkyCraft.LOG.info("Portalcraft: portal passes: {} frames, {} passes/frame (max {}), {} ms/frame (CPU), {} portals skipped behind terrain", frames - lastLog,
				String.format("%.2f", passCount / (double) (frames - lastLog)), maxPasses, String.format("%.2f", nanos / 1e6 / (frames - lastLog)), hiddenSkips);
			hiddenSkips = 0;
			lastLog = frames;
			passCount = 0;
			nanos = 0;
			maxPasses = 0;
		}
		return true;
	}

	/**
	 * The view through portal i from camera {@code parent} (into slot): rendered, recursed, and drawn
	 * into the parent's target through the opening. Returns the number of level passes rendered.
	 */
	private static int pass(Minecraft minecraft, GameRenderer renderer, SkyLink.HostPortals ps, int i, int slot, int depth, int maxDepth, Cam parent, float[] box,
		boolean full, float fovY, float aspect, boolean z01) {
		final int j = 1 - i;
		Cam c = cams[1 + slot];
		through(ps, i, parent, c);
		RenderTarget main = cams[0].target;
		TextureTarget t = target(slot, main, ps.resScale);
		c.target = t;
		final int rd = ps.renderDistance > 0 ? ps.renderDistance : 0;
		SkyLink.HostPortal b = ps.portal[j];
		plane[0] = b.nx;
		plane[1] = b.ny;
		plane[2] = b.nz;
		plane[3] = (float) -(b.nx * b.cx + b.ny * b.cy + b.nz * b.cz);
		System.arraycopy(box, 0, c.box, 0, 4);
		if (debugLog > 0 && (frames % 300) == 0) {
			debugLog--;
			SkyCraft.LOG.info("Portalcraft: portal pass slot {} depth {} through {}: camera ({} {} {}) from ({} {} {}), plane ({} {} {} {}) camera side {}, box ({} {} {} {}) full {}",
				slot, depth, i, c.x, c.y, c.z, parent.x, parent.y, parent.z, plane[0], plane[1], plane[2], plane[3], plane[0] * c.x + plane[1] * c.y + plane[2] * c.z + plane[3], box[0],
				box[1], box[2], box[3], full);
		}
		renderLevel(minecraft, renderer, slot, c, t, rd);
		setBoxProj(c, fovY, aspect, c.far);
		int passes = 1;
		float[] crop = HostViewClient.crop();
		DepthAlphaPass.run(((GlTexture) t.getColorTexture()).glId(), ((GlTexture) t.getDepthTexture()).glId(), t.width, t.height, HostViewClient.cropW(crop, t.width),
			HostViewClient.cropH(crop, t.height), c.proj, z01, c.far);
		// what this view sees of the pair: the entrance again (the exit is its near plane)
		if (depth < maxDepth && slot + 2 < SLOTS) {
			float[] sub = boxes[slot + 2];
			int vis = visible(ps.portal[i], c, c.box, sub);
			if (vis != NONE) {
				passes += pass(minecraft, renderer, ps, i, slot + 2, depth + 1, maxDepth, c, sub, vis == FULL, fovY, aspect, z01);
			}
		} else {
			// past the recursion limit: the portal's flat colour, as Portal 2 draws it
			int vis = visible(ps.portal[i], c, c.box, FLAT_BOX);
			if (vis != NONE) {
				opening(ps.portal[i], c, t, -1, 1.0F, vis == FULL, ps.portal[i].colour & 0xFFFFFF);
			}
		}
		// into the parent through portal i's opening
		float scale = t.width / (float) parent.target.width;
		opening(ps.portal[i], parent, parent.target, ((GlTexture) t.getColorTexture()).glId(), scale, full, -1);
		return passes;
	}

	private static void opening(SkyLink.HostPortal p, Cam cam, RenderTarget dst, int srcTex, float srcScale, boolean full, int flat) {
		float rx = p.uy * p.nz - p.uz * p.ny, ry = p.uz * p.nx - p.ux * p.nz, rz = p.ux * p.ny - p.uy * p.nx;
		centre[0] = (float) (p.cx - cam.x) + p.nx * EPS;
		centre[1] = (float) (p.cy - cam.y) + p.ny * EPS;
		centre[2] = (float) (p.cz - cam.z) + p.nz * EPS;
		axisR[0] = rx * p.halfW;
		axisR[1] = ry * p.halfW;
		axisR[2] = rz * p.halfW;
		axisU[0] = p.ux * p.halfH;
		axisU[1] = p.uy * p.halfH;
		axisU[2] = p.uz * p.halfH;
		PortalComposite.draw(((GlTexture) dst.getColorTexture()).glId(), ((GlTexture) dst.getDepthTexture()).glId(), dst.width, dst.height, srcTex, srcScale, cam.proj, cam.viewRot,
			centre, axisR, axisU, full, flat);
	}

	private static TextureTarget target(int slot, RenderTarget main, float resScale) {
		float s = resScale > 0.05F && resScale <= 1.0F ? resScale : 1.0F;
		int w = Math.max(16, Math.round(main.width * s)), h = Math.max(16, Math.round(main.height * s));
		TextureTarget t = targets[slot];
		if (t == null) {
			t = targets[slot] = new TextureTarget("Portalcraft portal pass " + slot, w, h, GpuFormat.RGBA8_UNORM, GpuFormat.D32_FLOAT);
			SkyCraft.LOG.info("Portalcraft: portal pass target {} {}x{}", slot, w, h);
		} else if (t.width != w || t.height != h) {
			t.resize(w, h);  // only when Minecraft's frame size changes
		}
		return t;
	}

	/** One level pass from the virtual camera c into target t (the second-camera machinery, own buffers). */
	private static void renderLevel(Minecraft minecraft, GameRenderer renderer, int slot, Cam c, TextureTarget t, int rd) {
		GameRendererAccessor acc = (GameRendererAccessor) renderer;
		DeltaTracker dt = minecraft.getDeltaTracker();
		GameRenderState st = renderer.gameRenderState();
		float partial = st.levelRenderState.worldPartialTicks;
		HostViewClient.beginPortalPass(2 + slot, c.x, c.y, c.z, c.rot, plane, c.box, rd);
		renderer.mainCamera().update(dt);
		acc.portalcraft$extractCamera(dt, partial);
		CameraRenderState cs = st.levelRenderState.cameraRenderState;
		if (rd > 0) {
			// fog where this pass's terrain ends, as Minecraft does at that render distance
			FogData f = cs.fogData;
			float end = rd * 16.0F, span = Math.max(4.0F, Math.min(64.0F, end / 10.0F));
			if (end < f.renderDistanceEnd) {
				f.renderDistanceEnd = end;
				f.renderDistanceStart = end - span;
			}
		}
		((SectionOcclusionGraphAccessor) minecraft.levelRenderer.sectionOcclusionGraph()).portalcraft$needsFrustumUpdate().set(true);
		minecraft.levelExtractor.extract(dt, renderer.mainCamera(), partial);
		acc.portalcraft$globalSettings().update(t.width, t.height, st.optionsRenderState.glintStrength, st.levelRenderState.gameTime, partial,
			st.optionsRenderState.menuBackgroundBlurriness, cs.pos, st.optionsRenderState.textureFiltering == TextureFilteringMethod.RGSS);
		c.proj.set(cs.projectionMatrix);
		c.viewRot.set(cs.viewRotationMatrix);
		c.far = cs.depthFar;
		if (projBufs[slot] == null) {
			projBufs[slot] = new ProjectionMatrixBuffer("portalcraft portal pass " + slot);
		}
		RenderSystem.setProjectionMatrix(projBufs[slot].getBuffer(cs.projectionMatrix), ProjectionType.PERSPECTIVE);
		FogRenderer fog = fog(slot);
		fog.updateBuffer(cs.fogData);
		GpuBufferSlice terrainFog = fog.getBuffer(FogRenderer.FogMode.WORLD);
		LevelRendererPassAccessor lr = (LevelRendererPassAccessor) minecraft.levelRenderer;
		CloudRenderer mainClouds = lr.portalcraft$cloudRenderer();
		RenderTarget prev = override;
		override = t;
		lr.portalcraft$setCloudRenderer(cloud(slot, mainClouds));
		try {
			minecraft.levelRenderer.render(acc.portalcraft$resourcePool(), false, cs, terrainFog, cs.fogData.color, true, false);
		} finally {
			lr.portalcraft$setCloudRenderer(mainClouds);
			override = prev;
		}
	}
}
