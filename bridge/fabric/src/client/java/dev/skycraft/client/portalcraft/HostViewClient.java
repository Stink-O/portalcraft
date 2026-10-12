package dev.skycraft.client.portalcraft;

import dev.skycraft.client.SkyClient;
import dev.skycraft.link.Proto;
import dev.skycraft.link.SkyLink;
import net.minecraft.client.Minecraft;

/**
 * Portalcraft: the host (Portal 2) asks Minecraft to render its own frame from a camera of the
 * host's choosing: the virtual camera of a portal view that looks into the Minecraft region, or
 * the host's camera once the player is in there. While a request is active Minecraft draws its
 * level (normally skipped while linked) from that camera, with no hand or HUD, and
 * {@link WorldViewExporter} ships colour (+ depth in alpha) back through the overlay triple buffer.
 *
 * <p>Protocol v13 hosts add a HostViewExt: a crop window per camera (Minecraft renders and ships
 * only that part of the frustum, one pixel per frame pixel) and an optional second camera, rendered
 * in the same frame as a second level pass ({@code GameRendererViewMixin}) with a shorter render
 * distance. {@link #pass()} says which camera the current pass renders; {@link #view()} and
 * {@link #crop()} follow it.
 */
public final class HostViewClient {
	private static final SkyLink.HostView primary = new SkyLink.HostView();
	private static final SkyLink.HostView scratch = new SkyLink.HostView();
	private static final SkyLink.HostViewExt ext = new SkyLink.HostViewExt();
	private static final SkyLink.HostViewExt extScratch = new SkyLink.HostViewExt();
	private static final float[] FULL = {-1, -1, 1, 1};
	private static final float[] primaryCrop = {-1, -1, 1, 1};
	private static final float[] secondCrop = {-1, -1, 1, 1};
	private static final SkyLink.HostPortals portals = new SkyLink.HostPortals();
	private static final SkyLink.HostPortals portalsScratch = new SkyLink.HostPortals();
	private static boolean active, secondActive, portalsActive;
	private static int secondRd;
	private static int pass;  // 0: primary camera, 1: second camera, 2+: a portal pass (PortalPasses)
	// the camera of the current portal pass (set by PortalPasses before each one)
	private static final SkyLink.HostView portalView = new SkyLink.HostView();
	private static final org.joml.Quaternionf portalRotation = new org.joml.Quaternionf();
	private static final float[] portalCull = {-1, -1, 1, 1};
	private static int portalRd;
	private static double lastX = Double.NaN, lastY, lastZ;
	private static boolean jumped;
	private static boolean logged, loggedSecond, loggedPortals;
	private static int extMismatch;

	private HostViewClient() {
	}

	/** Start of every Minecraft frame (render thread). */
	public static void poll(Minecraft minecraft) {
		pass = 0;
		if (!SkyClient.linked()) {
			active = false;
			secondActive = false;
			return;
		}
		// HostView, then the v13 block that belongs to it (the host writes the block first, so a
		// block for a newer request means the HostView changed in between: read both again)
		boolean haveExt = false, noExt = false;
		for (int attempt = 0; attempt < 8; attempt++) {
			if (!SkyLink.readHostView(scratch)) {
				break;
			}
			if (!SkyLink.readHostViewExt(extScratch) || !extScratch.valid()) {
				copy(scratch, primary);
				noExt = true;
				break;
			}
			if (extScratch.primaryRequestId == scratch.requestId) {
				// v15: the portal pair block belongs to the same request (written before the ext block)
				boolean havePortals = SkyLink.hostVersion() >= 15 && SkyLink.readHostPortals(portalsScratch);
				if (havePortals && portalsScratch.primaryRequestId != scratch.requestId && attempt < 7) {
					continue;
				}
				copy(scratch, primary);
				copyExt(extScratch, ext);
				haveExt = true;
				portalsActive = havePortals && portalsScratch.primaryRequestId == scratch.requestId && portalsScratch.inMc();
				if (portalsActive) {
					copyPortals(portalsScratch, portals);
				}
				break;
			}
			if (attempt == 7 && extMismatch++ < 5) {
				dev.skycraft.SkyCraft.LOG.warn("Portalcraft: HostViewExt for request {} vs HostView {}: keeping the last consistent pair", extScratch.primaryRequestId,
					scratch.requestId);
			}
		}
		if (haveExt) {
			System.arraycopy(ext.crop, 0, primaryCrop, 0, 4);
			secondActive = ext.second.active();
			System.arraycopy(ext.scrop, 0, secondCrop, 0, 4);
			secondRd = ext.secRenderDistance;
		} else if (noExt) {
			System.arraycopy(FULL, 0, primaryCrop, 0, 4);
			secondActive = false;
			portalsActive = false;
		}
		sanitize(primaryCrop);
		sanitize(secondCrop);
		boolean now = primary.active();
		if (now) {
			double dx = primary.x - lastX, dy = primary.y - lastY, dz = primary.z - lastZ;
			if (Double.isNaN(lastX) || dx * dx + dy * dy + dz * dz > 16 * 16) {
				jumped = true;
			}
			lastX = primary.x;
			lastY = primary.y;
			lastZ = primary.z;
		}
		if (now != active || !logged) {
			logged = true;
			dev.skycraft.SkyCraft.LOG.info("Portalcraft: host view {} (host protocol {})", now ? "on" : "off", SkyLink.hostVersion());
		}
		if (secondActive && !loggedSecond) {
			loggedSecond = true;
			dev.skycraft.SkyCraft.LOG.info("Portalcraft: second camera on (request {}, render distance {})", ext.second.requestId, secondRd);
		}
		if (portalsActive != loggedPortals) {
			loggedPortals = portalsActive;
			dev.skycraft.SkyCraft.LOG.info("Portalcraft: portal pair drawn by Minecraft {} (depth {}, render distance {})", portalsActive ? "on" : "off", portals.maxDepth,
				portals.renderDistance);
		}
		active = now;
		secondActive &= now;
		portalsActive &= now;
		// Occlusion culling starts from the camera's section; a portal's virtual camera sits inside
		// the rock behind the portal, which would cull the landscape it looks at.
		minecraft.smartCull = !active;
	}

	private static void sanitize(float[] c) {
		for (int i = 0; i < 4; i++) {
			if (!(c[i] >= -1.0F && c[i] <= 1.0F)) {
				c[i] = i < 2 ? -1.0F : 1.0F;
			}
		}
		if (c[2] - c[0] < 1e-3F || c[3] - c[1] < 1e-3F) {
			System.arraycopy(FULL, 0, c, 0, 4);
		}
	}

	/** The host camera jumped more than 16 blocks since the last frame (once per jump). */
	public static boolean consumeJump() {
		boolean j = jumped && active && pass == 0;
		if (pass == 0) {
			jumped = false;
		}
		return j;
	}

	public static boolean active() {
		return active;
	}

	private static float playerFov = 70.0F;

	/**
	 * Bridge #14: the player's own vertical FOV (options x sprint / fluid modifiers), as Camera.update computed it before
	 * the host's camera replaced it. McState reports this one: reporting the host's (the camera's field while a host view
	 * is up) closed a loop (host FOV -> McState -> the host's Minecraft-mode camera -> x overscan -> host FOV) that ran
	 * the FOV up to 180 degrees within frames of a J switch (the "stretched, broken" screen).
	 */
	public static void setPlayerFov(float fov) {
		playerFov = fov;
	}

	public static float playerFov() {
		return playerFov;
	}

	/** A second camera is requested this frame (v13 hosts). */
	public static boolean secondActive() {
		return secondActive;
	}

	/** Render distance (chunks) for the second camera; 0 = Minecraft's own. */
	public static int secondRenderDistance() {
		return secondRd;
	}

	/** 0 while the primary camera renders, 1 during the second camera's pass. */
	public static int pass() {
		return pass;
	}

	public static void setPass(int p) {
		pass = p;
	}

	/** The camera of the current pass. */
	public static SkyLink.HostView view() {
		return pass >= 2 ? portalView : pass == 1 ? ext.second : primary;
	}

	/** The primary camera (whatever the current pass). */
	public static SkyLink.HostView primary() {
		return primary;
	}

	/**
	 * The current pass's crop window x0, y0, x1, y1 in NDC of its frustum (-1..1). Portal passes
	 * use the primary's: they land pixel for pixel on the primary's image.
	 */
	public static float[] crop() {
		return pass == 1 ? secondCrop : primaryCrop;
	}

	/** The window of the frustum the current pass culls with (NDC, before the crop translation). */
	public static float[] cullWindow() {
		return pass >= 2 ? portalCull : crop();
	}

	/** Render distance (chunks) of the current pass; 0 = Minecraft's own. */
	public static int passRenderDistance() {
		return pass >= 2 ? portalRd : pass == 1 ? secondRd : 0;
	}

	/** A portal pass's camera rotation (exact, no Euler round trip); null for host cameras. */
	public static org.joml.Quaternionf passRotation() {
		return pass >= 2 ? portalRotation : null;
	}

	/** v15: the live pair is inside Minecraft and Minecraft draws its views this frame. */
	public static boolean portalsActive() {
		return portalsActive && active;
	}

	public static SkyLink.HostPortals portals() {
		return portals;
	}

	/**
	 * Enter portal pass {@code p} (2+) with this camera: position, rotation, the partner's plane
	 * (oblique near plane), the opening's window to cull with and the pass render distance.
	 */
	public static void beginPortalPass(int p, double x, double y, double z, org.joml.Quaternionfc rot, float[] plane, float[] cull, int rd) {
		pass = p;
		portalView.flags = Proto.VIEW_ACTIVE | Proto.VIEW_CLIP_PLANE;
		portalView.x = x;
		portalView.y = y;
		portalView.z = z;
		portalView.fovY = primary.fovY;
		portalView.requestId = primary.requestId;
		portalRotation.set(rot);
		org.joml.Vector3f e = portalRotation.getEulerAnglesYXZ(new org.joml.Vector3f());
		portalView.yaw = 180.0F - (float) Math.toDegrees(e.y);
		portalView.pitch = -(float) Math.toDegrees(e.x);
		portalView.roll = (float) Math.toDegrees(e.z);
		System.arraycopy(plane, 0, portalView.plane, 0, 4);
		System.arraycopy(cull, 0, portalCull, 0, 4);
		portalRd = rd;
	}

	/** Width and height of the cropped image for a target of w x h pixels. */
	public static int cropW(float[] c, int w) {
		return Math.max(1, Math.min(w, (int) Math.ceil((c[2] - c[0]) * 0.5 * w)));
	}

	public static int cropH(float[] c, int h) {
		return Math.max(1, Math.min(h, (int) Math.ceil((c[3] - c[1]) * 0.5 * h)));
	}

	public static boolean wantsScreenshot() {
		return active && pass == 0 && (primary.flags & Proto.VIEW_SCREENSHOT) != 0;
	}

	private static void copy(SkyLink.HostView a, SkyLink.HostView b) {
		b.seq = a.seq;
		b.flags = a.flags;
		b.x = a.x;
		b.y = a.y;
		b.z = a.z;
		b.yaw = a.yaw;
		b.pitch = a.pitch;
		b.roll = a.roll;
		b.fovY = a.fovY;
		System.arraycopy(a.plane, 0, b.plane, 0, 4);
		b.requestId = a.requestId;
	}

	private static void copyPortals(SkyLink.HostPortals a, SkyLink.HostPortals b) {
		b.seq = a.seq;
		b.flags = a.flags;
		b.primaryRequestId = a.primaryRequestId;
		b.maxDepth = a.maxDepth;
		b.renderDistance = a.renderDistance;
		b.resScale = a.resScale;
		for (int i = 0; i < 2; i++) {
			SkyLink.HostPortal p = a.portal[i], q = b.portal[i];
			q.cx = p.cx;
			q.cy = p.cy;
			q.cz = p.cz;
			q.nx = p.nx;
			q.ny = p.ny;
			q.nz = p.nz;
			q.ux = p.ux;
			q.uy = p.uy;
			q.uz = p.uz;
			q.halfW = p.halfW;
			q.halfH = p.halfH;
			q.open = p.open;
			q.colour = p.colour;
		}
	}

	private static void copyExt(SkyLink.HostViewExt a, SkyLink.HostViewExt b) {
		b.seq = a.seq;
		b.flags = a.flags;
		System.arraycopy(a.crop, 0, b.crop, 0, 4);
		System.arraycopy(a.scrop, 0, b.scrop, 0, 4);
		b.primaryRequestId = a.primaryRequestId;
		b.primaryStream = a.primaryStream;
		copy(a.second, b.second);
		b.secStream = a.secStream;
		b.secRenderDistance = a.secRenderDistance;
	}
}
