package dev.skycraft.client.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.skycraft.client.portalcraft.AvatarHost;
import dev.skycraft.world.SkyCollision;
import net.minecraft.client.Camera;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.ClipContext;
import net.minecraft.world.level.Level;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.Vec3;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * Portalcraft (bridge #16): the F5 camera.
 * <ul>
 * <li>Its zoom collision also stops at the host's exported collision (SkyCollision's voxels: Portal 2's chamber walls,
 * floors and moving solids), which vanilla's {@code Level.clip} never sees (they are collision shapes, not blocks).</li>
 * <li>The distance it chose is recorded for the host ({@link AvatarHost#zoom}): a host view replaces the camera's
 * position afterwards (CameraHostViewMixin), and the host puts its own camera there.</li>
 * </ul>
 */
@Mixin(Camera.class)
public abstract class ThirdPersonCameraMixin {
	@WrapOperation(
		method = "getMaxZoom",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/Level;clip(Lnet/minecraft/world/level/ClipContext;)Lnet/minecraft/world/phys/BlockHitResult;")
	)
	private BlockHitResult portalcraft$zoomAgainstHostVoxels(Level level, ClipContext context, Operation<BlockHitResult> original) {
		BlockHitResult hit = original.call(level, context);
		if (!SkyCollision.active()) {
			return hit;
		}
		Vec3 from = context.getFrom(), to = context.getTo();
		BlockHitResult voxel = BlockGetter.<BlockHitResult, Object>traverseBlocks(from, to, Boolean.TRUE, (ignored, pos) -> {
			VoxelShape shape = SkyCollision.shapeAt(pos);
			return shape == null || shape.isEmpty() ? null : shape.clip(from, to, pos);
		}, ignored -> null);
		if (voxel == null) {
			return hit;
		}
		if (hit != null && hit.getType() != HitResult.Type.MISS && from.distanceToSqr(hit.getLocation()) <= from.distanceToSqr(voxel.getLocation())) {
			return hit;
		}
		return voxel;
	}

	@WrapOperation(method = "alignWithEntity", at = @At(value = "INVOKE", target = "Lnet/minecraft/client/Camera;getMaxZoom(F)F"))
	private float portalcraft$recordZoom(Camera camera, float distance, Operation<Float> original) {
		float zoom = original.call(camera, distance);
		AvatarHost.setZoom(zoom);
		return zoom;
	}
}
