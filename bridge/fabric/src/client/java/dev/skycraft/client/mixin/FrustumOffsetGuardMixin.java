package dev.skycraft.client.mixin;

import net.minecraft.client.renderer.culling.Frustum;
import org.joml.FrustumIntersection;
import org.joml.Vector4f;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Portalcraft: Frustum.offsetToFullyIncludeCameraCube steps the frustum back along the view vector until
 * the camera's section cube is inside it. A host view's frustum can never contain it: a portal view's
 * oblique near plane sits at the exit portal, in front of the camera, and stepping back only moves the
 * cube further behind that plane. Vanilla loops forever (the hidden Minecraft froze mid-session in a
 * second-camera pass, 2026-10-01 07:22, and stopped quitting with its host). Same steps, bounded: after
 * 256 (1024 blocks) the frustum is left where it started.
 */
@Mixin(Frustum.class)
public abstract class FrustumOffsetGuardMixin {
	@Shadow @Final private FrustumIntersection intersection;
	@Shadow private Vector4f viewVector;
	@Shadow private double camX;
	@Shadow private double camY;
	@Shadow private double camZ;
	private static int portalcraft$gaveUp;

	@Inject(method = "offsetToFullyIncludeCameraCube", at = @At("HEAD"), cancellable = true)
	private void portalcraft$boundedOffset(int cubeSize, CallbackInfoReturnable<Frustum> cir) {
		final double x0 = this.camX, y0 = this.camY, z0 = this.camZ;
		final double ax = Math.floor(x0 / cubeSize) * cubeSize, ay = Math.floor(y0 / cubeSize) * cubeSize, az = Math.floor(z0 / cubeSize) * cubeSize;
		final double bx = Math.ceil(x0 / cubeSize) * cubeSize, by = Math.ceil(y0 / cubeSize) * cubeSize, bz = Math.ceil(z0 / cubeSize) * cubeSize;
		for (int i = 0; i < 256; i++) {
			if (this.intersection.intersectAab((float) (ax - this.camX), (float) (ay - this.camY), (float) (az - this.camZ), (float) (bx - this.camX), (float) (by - this.camY),
				(float) (bz - this.camZ)) == FrustumIntersection.INSIDE) {
				cir.setReturnValue((Frustum) (Object) this);
				return;
			}
			this.camX -= this.viewVector.x() * 4.0F;
			this.camY -= this.viewVector.y() * 4.0F;
			this.camZ -= this.viewVector.z() * 4.0F;
		}
		this.camX = x0;
		this.camY = y0;
		this.camZ = z0;
		if (portalcraft$gaveUp++ < 5) {
			dev.skycraft.SkyCraft.LOG.warn("Portalcraft: frustum cannot include the camera's section (oblique host view); left unshifted");
		}
		cir.setReturnValue((Frustum) (Object) this);
	}
}
