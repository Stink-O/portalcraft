package dev.skycraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.skycraft.world.HostShapes;
import dev.skycraft.world.SkyCollision;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.BlockCollisions;
import net.minecraft.world.level.CollisionGetter;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.shapes.CollisionContext;
import net.minecraft.world.phys.shapes.EntityCollisionContext;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * Adds Skyrim's geometry to every block-collision query. Vanilla movement, step-up, onGround
 * and fall-damage logic then run unchanged against it.
 */
@Mixin(BlockCollisions.class)
public abstract class BlockCollisionsMixin {
	@WrapOperation(
		method = "computeNext",
		at = @At(
			value = "INVOKE",
			target = "Lnet/minecraft/world/phys/shapes/CollisionContext;getCollisionShape(Lnet/minecraft/world/level/block/state/BlockState;Lnet/minecraft/world/level/CollisionGetter;Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/phys/shapes/VoxelShape;"
		)
	)
	private VoxelShape skycraft$addSkyrimShape(
		CollisionContext context, BlockState state, CollisionGetter level, BlockPos pos, Operation<VoxelShape> original
	) {
		VoxelShape blockShape = original.call(context, state, level, pos);
		net.minecraft.world.entity.Entity entity = context instanceof EntityCollisionContext ec ? ec.getEntity() : null;
		// Portalcraft: players fall / walk into a portal on Minecraft blocks (the hole the host carves for Chell); mobs, items and projectiles go
		// into a linked portal on them too (bridge #22; EntityTransit carries them through)
		blockShape = HostShapes.carve(entity, pos, blockShape, 0.0);
		if (context instanceof EntityCollisionContext entityContext && SkyCollision.usesSmoothCollider(entityContext.getEntity())) {
			return blockShape; // this entity collides with Skyrim's exact triangles instead (SkyCollider)
		}
		VoxelShape sky = SkyCollision.shapeAt(pos);
		if (sky == null) {
			return blockShape;
		}
		// Portalcraft (bridge #45): the portals' holes are carved out of the host's geometry HERE, at query time, as they are out of the blocks above. They
		// used to be baked into the exported field by the host: that field is what Portal 2 has (the dig's reveal, the mining ray and the wall faces read
		// it), and a hole in it removed real wall and the revealed blocks of a hole dug beside it, 128 u deep behind every portal; blocks wholly inside
		// a hole were then re-voxelised as thin geometry, which left a one-voxel skin across the middle of the opening (the Minecraft player could
		// not walk into a gun portal on a campaign chamber's wall).
		sky = HostShapes.carve(entity, pos, sky, 0.0);
		return blockShape.isEmpty() ? sky : Shapes.or(blockShape, sky);
	}
}
