package dev.skycraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.skycraft.world.SkyCollision;
import net.minecraft.world.entity.item.FallingBlockEntity;
import net.minecraft.world.level.block.FallingBlock;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * Portalcraft (bridge #20): falling sand and gravel land on the host's ground (Portal 2's chamber floors) as blocks.
 * The entity stops on the host's collision (BlockCollisionsMixin), but vanilla then sees air under it ("would continue
 * falling") and breaks it into an item. Host ground under the landing cell counts as not free, as a block would.
 */
@Mixin(FallingBlockEntity.class)
public abstract class FallingBlockLandMixin {
	private static int portalcraft$logs;

	@WrapOperation(
		method = "tick",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/block/FallingBlock;isFree(Lnet/minecraft/world/level/block/state/BlockState;)Z")
	)
	private boolean portalcraft$hostGroundHolds(BlockState below, Operation<Boolean> original) {
		boolean free = original.call(below);
		FallingBlockEntity self = (FallingBlockEntity) (Object) this;
		boolean held = free && SkyCollision.active() && SkyCollision.supportsFromBelow(self.blockPosition());
		if (portalcraft$logs < 40 && SkyCollision.active()) {
			// (bridge #27: gravel dropped as an item on a chamber floor where sand landed; which cell and what the host has there)
			portalcraft$logs++;
			dev.skycraft.SkyCraft.LOG.info("Portalcraft: falling {} at {} (y {}) on ground {}: below {} free {} -> host ground holds {}, ground top here {} below {}",
				self.getBlockState().getBlock(), self.blockPosition().toShortString(), String.format("%.3f", self.getY()), self.onGround(), below.getBlock(), free, held,
				String.format("%.2f", SkyCollision.groundTop(self.blockPosition())), String.format("%.2f", SkyCollision.groundTop(self.blockPosition().below())));
		}
		return held ? false : free;
	}
}
