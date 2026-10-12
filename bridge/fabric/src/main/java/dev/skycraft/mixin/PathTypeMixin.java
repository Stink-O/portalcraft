package dev.skycraft.mixin;

import dev.skycraft.world.SkyCollision;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.pathfinder.PathType;
import net.minecraft.world.level.pathfinder.WalkNodeEvaluator;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Portalcraft (bridge #19): mobs path over the host's world (Portal 2's chamber floors, walls, stairs), which Minecraft
 * only knows as collision (SkyCollision), not as blocks. A cell that is mostly host geometry is BLOCKED (a wall, or the
 * ground a mob walks on: the cell above it becomes WALKABLE by vanilla's own rule), and the floor height above host
 * ground is the host geometry's top. Movement itself already collides with it (BlockCollisionsMixin).
 */
@Mixin(WalkNodeEvaluator.class)
public abstract class PathTypeMixin {
	@Inject(method = "getPathTypeFromState(Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/level/pathfinder/PathType;", at = @At("HEAD"), cancellable = true)
	private static void portalcraft$hostBlocked(BlockGetter level, BlockPos pos, CallbackInfoReturnable<PathType> cir) {
		if (SkyCollision.solidFraction(pos) >= 0.5F && level.getBlockState(pos).isAir()) {
			cir.setReturnValue(PathType.BLOCKED);
		}
	}

	@Inject(method = "getFloorLevel(Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;)D", at = @At("HEAD"), cancellable = true)
	private static void portalcraft$hostFloor(BlockGetter level, BlockPos pos, CallbackInfoReturnable<Double> cir) {
		BlockPos below = pos.below();
		VoxelShape host = SkyCollision.shapeAt(below);
		if (host == null || host.isEmpty() || !level.getBlockState(below).getCollisionShape(level, below).isEmpty()) {
			return;
		}
		cir.setReturnValue(below.getY() + host.max(Direction.Axis.Y));
	}
}
