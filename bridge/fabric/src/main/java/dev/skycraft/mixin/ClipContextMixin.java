package dev.skycraft.mixin;

import dev.skycraft.world.HostShapes;
import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.ClipContext;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.Vec3;
import net.minecraft.world.phys.shapes.CollisionContext;
import net.minecraft.world.phys.shapes.EntityCollisionContext;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Portalcraft (bridge #48): Portal 2's chamber geometry is solid to every block clip with a collider context, as it is to movement
 * (BlockCollisionsMixin). Arrows, tridents, snowballs, pearls, fireballs and fireworks fly along such clips; before this they flew through
 * a chamber's walls and floors (the host's walls are not blocks, so vanilla found only air) and out of the world. Now one rule covers all
 * of them, and mobs' line of sight too: the clip sees the cell's own shape plus the host's voxels, with the open portals' holes cut out
 * for whoever can go through them (HostShapes). Pick (the crosshair, OUTLINE) is SkyClip's, unchanged.
 */
@Mixin(ClipContext.class)
public abstract class ClipContextMixin {
	@Shadow
	@Final
	private ClipContext.Block block;
	@Shadow
	@Final
	private CollisionContext collisionContext;
	@Shadow
	@Final
	private Vec3 from;
	@Shadow
	@Final
	private Vec3 to;

	@Inject(method = "getBlockShape", at = @At("RETURN"), cancellable = true)
	private void portalcraft$hostGeometry(BlockState state, BlockGetter level, BlockPos pos, CallbackInfoReturnable<VoxelShape> cir) {
		if (this.block != ClipContext.Block.COLLIDER) {
			return;
		}
		Entity entity = this.collisionContext instanceof EntityCollisionContext ec ? ec.getEntity() : null;
		cir.setReturnValue(HostShapes.clipShape(entity, pos, cir.getReturnValue(), this.from.distanceTo(this.to)));
	}
}
