package dev.skycraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.skycraft.world.SkyCollision;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.item.FireChargeItem;
import net.minecraft.world.item.FlintAndSteelItem;
import net.minecraft.world.item.context.UseOnContext;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * Portalcraft (bridge #20): flint and steel and fire charges light fire on the host's surfaces (Portal 2's floors and
 * walls). The crosshair's hit on a host surface (SkyClip PICK) reports the air cell in front of the surface as the
 * clicked block, so that a block item goes into it; these items light the cell in front of the clicked block, one
 * further out, where nothing holds fire up. On a host hit (the clicked cell is air) the fire goes in the clicked cell.
 */
@Mixin({FlintAndSteelItem.class, FireChargeItem.class})
public abstract class FireOnHostMixin {
	@WrapOperation(
		method = "useOn",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/core/BlockPos;relative(Lnet/minecraft/core/Direction;)Lnet/minecraft/core/BlockPos;")
	)
	private BlockPos portalcraft$fireInHostCell(BlockPos pos, Direction face, Operation<BlockPos> original, UseOnContext context) {
		if (SkyCollision.active() && context.getLevel().getBlockState(pos).isAir()) {
			return pos;
		}
		return original.call(pos, face);
	}
}
