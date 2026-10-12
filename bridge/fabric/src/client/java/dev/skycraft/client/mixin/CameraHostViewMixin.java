package dev.skycraft.client.mixin;

import com.mojang.blaze3d.systems.RenderSystem;
import dev.skycraft.client.portalcraft.HostViewClient;
import dev.skycraft.link.Proto;
import dev.skycraft.link.SkyLink;
import net.minecraft.client.Camera;
import net.minecraft.client.DeltaTracker;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.state.level.CameraRenderState;
import net.minecraft.world.phys.Vec3;
import org.joml.Matrix4f;
import org.joml.Matrix4fc;
import org.joml.Quaternionf;
import org.joml.Vector3f;
import org.joml.Vector3fc;
import org.joml.Vector4f;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Portalcraft: while the host has a view request active, Minecraft's camera is the host's camera
 * (position, yaw/pitch/roll, vertical FOV), and an oblique near plane clips everything behind the
 * portal the host is looking through (the virtual camera sits in the rock behind it). Portal passes
 * (PortalPasses: the views Minecraft renders through the live pair itself) set their camera the
 * same way, with the exact rotation and the opening's window as the culling frustum.
 */
@Mixin(Camera.class)
public abstract class CameraHostViewMixin {
	@Shadow private float xRot;
	@Shadow private float yRot;
	@Shadow @Final private Quaternionf rotation;
	@Shadow @Final private Vector3f forwards;
	@Shadow @Final private Vector3f up;
	@Shadow @Final private Vector3f left;
	@Shadow private int matrixPropertiesDirty;
	@Shadow private float fov;
	@Shadow private float depthFar;
	@Shadow private boolean initialized;
	@Shadow @Final private Matrix4f cachedViewRotMatrix;
	@Shadow @Final private static Vector3fc FORWARDS;
	@Shadow @Final private static Vector3fc UP;
	@Shadow @Final private static Vector3fc LEFT;

	@Shadow
	protected abstract void setPosition(double x, double y, double z);

	@Shadow
	public abstract Matrix4f getViewRotationMatrix(Matrix4f dest);

	@Shadow
	private void prepareCullFrustum(final Matrix4fc modelViewMatrix, final Matrix4f projectionMatrixForCulling, final Vec3 cameraPos) {
		throw new AssertionError();
	}

	@Shadow
	private void setupPerspective(final float zNear, final float zFar, final float fov, final float width, final float height) {
		throw new AssertionError();
	}

	@Inject(method = "update", at = @At("TAIL"))
	private void portalcraft$hostCamera(DeltaTracker deltaTracker, CallbackInfo ci) {
		if (!HostViewClient.active() || !this.initialized) {
			return;
		}
		SkyLink.HostView v = HostViewClient.view();
		final float deg = (float) (Math.PI / 180.0);
		this.xRot = v.pitch;
		this.yRot = v.yaw;
		Quaternionf exact = HostViewClient.passRotation();
		if (exact != null) {
			this.rotation.set(exact);  // a portal pass: the virtual camera's rotation as computed
		} else {
			this.rotation.rotationYXZ((float) Math.PI - v.yaw * deg, -v.pitch * deg, v.roll * deg);
		}
		FORWARDS.rotate(this.rotation, this.forwards);
		UP.rotate(this.rotation, this.up);
		LEFT.rotate(this.rotation, this.left);
		this.matrixPropertiesDirty |= 3;
		this.setPosition(v.x, v.y, v.z);
		HostViewClient.setPlayerFov(this.fov);  // the player's own, computed by update() before this
		this.fov = v.fovY;
		Minecraft minecraft = Minecraft.getInstance();
		float w = minecraft.getWindow().getWidth(), h = minecraft.getWindow().getHeight();
		// cull with the crop window's own frustum (v13) and, for the second camera, its render distance
		float far = this.depthFar;
		int rd = HostViewClient.passRenderDistance();
		if (rd > 0) {
			far = Math.min(far, rd * 16.0F);
		}
		Matrix4f cull = new Matrix4f().perspective(this.fov * deg, w / h, 0.05F, far, RenderSystem.getDevice().getDeviceInfo().isZZeroToOne());
		float[] c = HostViewClient.cullWindow();
		if (c[0] > -1.0F || c[1] > -1.0F || c[2] < 1.0F || c[3] < 1.0F) {
			// window [x0,x1] x [y0,y1] -> the whole NDC square; widened to contain the view axis, since
			// Minecraft's section culling backs the frustum's apex off along it (Frustum
			// .offsetToFullyIncludeCameraCube loops forever when the axis lies outside the frustum)
			float x0 = Math.min(c[0], -0.05F), y0 = Math.min(c[1], -0.05F), x1 = Math.max(c[2], 0.05F), y1 = Math.max(c[3], 0.05F);
			float sx = 2.0F / (x1 - x0), sy = 2.0F / (y1 - y0);
			Matrix4f win = new Matrix4f().m00(sx).m11(sy).m30(-(x0 + x1) * 0.5F * sx).m31(-(y0 + y1) * 0.5F * sy);
			cull = win.mul(cull);
		}
		this.prepareCullFrustum(this.getViewRotationMatrix(this.cachedViewRotMatrix), cull, new Vec3(v.x, v.y, v.z));
		this.setupPerspective(0.05F, this.depthFar, this.fov, w, h);
	}

	@Inject(method = "extractRenderState", at = @At("TAIL"))
	private void portalcraft$obliqueNearPlane(CameraRenderState cameraState, DeltaTracker deltaTracker, CallbackInfo ci) {
		if (!HostViewClient.active()) {
			return;
		}
		cameraState.smartCull = false;
		SkyLink.HostView v = HostViewClient.view();
		if ((v.flags & Proto.VIEW_CLIP_PLANE) != 0) {
			obliquePlane(cameraState, v);
		}
		// v13 crop: the window's bottom-left corner moves to the target's bottom-left corner, at the
		// same scale (a translation in NDC), so its pixels land 1:1 in the target's sub-rectangle
		float[] c = HostViewClient.crop();
		if (c[0] > -1.0F || c[1] > -1.0F) {
			Matrix4f t = new Matrix4f().m30(-1.0F - c[0]).m31(-1.0F - c[1]);
			t.mul(cameraState.projectionMatrix, cameraState.projectionMatrix);
		}
	}

	private void obliquePlane(CameraRenderState cameraState, SkyLink.HostView v) {
		// The portal plane in view space: rendering is camera-relative, and the view matrix is the
		// inverse camera rotation.
		Vector3f n = new Vector3f(v.plane[0], v.plane[1], v.plane[2]);
		float d = (float) (v.plane[0] * v.x + v.plane[1] * v.y + v.plane[2] * v.z) + v.plane[3];
		if (d >= -0.01F) {
			return; // camera in front of (or on) the portal: nothing behind it to clip
		}
		n.rotate(new Quaternionf(this.rotation).conjugate());
		obliqueNear(cameraState.projectionMatrix, new Vector4f(n.x, n.y, n.z, d), RenderSystem.getDevice().getDeviceInfo().isZZeroToOne());
	}

	/**
	 * Lengyel's oblique near plane for Minecraft's reversed-Z projection: the near plane
	 * (clip w - z >= 0) becomes {@code c} (view space, visible where c.v >= 0); the far plane is
	 * tilted through the far frustum corner on c's side so nothing in view is lost.
	 */
	private static void obliqueNear(Matrix4f p, Vector4f c, boolean zZeroToOne) {
		Matrix4f inv = new Matrix4f(p).invert();
		Vector4f cClip = inv.transformTranspose(new Vector4f(c));
		Vector4f q = inv.transform(new Vector4f(Math.signum(cClip.x), Math.signum(cClip.y), zZeroToOne ? 0.0F : -1.0F, 1.0F));
		float r3q = p.m03() * q.x + p.m13() * q.y + p.m23() * q.z + p.m33() * q.w;
		float cq = c.x * q.x + c.y * q.y + c.z * q.z + c.w * q.w;
		if (Math.abs(cq) < 1e-9F) {
			return;
		}
		float a = (zZeroToOne ? 1.0F : 2.0F) * r3q / cq;
		if (!(a > 0.0F) || Float.isInfinite(a)) {
			return;
		}
		p.m02(p.m03() - a * c.x);
		p.m12(p.m13() - a * c.y);
		p.m22(p.m23() - a * c.z);
		p.m32(p.m33() - a * c.w);
	}
}
