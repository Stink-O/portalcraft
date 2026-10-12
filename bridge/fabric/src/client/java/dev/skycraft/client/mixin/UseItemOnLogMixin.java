package dev.skycraft.client.mixin;

import net.minecraft.client.multiplayer.MultiPlayerGameMode;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.InteractionHand;
import net.minecraft.world.InteractionResult;
import net.minecraft.world.phys.BlockHitResult;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Portalcraft (bridge #23): logs the first use-on-block results (why a placement on a host surface did or didn't happen). */
@Mixin(MultiPlayerGameMode.class)
public abstract class UseItemOnLogMixin {
	@Unique private static int logged;

	@Inject(method = "useItemOn", at = @At("RETURN"))
	private void portalcraft$logUseOn(LocalPlayer player, InteractionHand hand, BlockHitResult hit, CallbackInfoReturnable<InteractionResult> cir) {
		if (logged++ < 30) {
			dev.skycraft.SkyCraft.LOG.info("Portalcraft: use {} on {} face {} at {} -> {} (state there {}, held {})", hand, hit.getBlockPos().toShortString(), hit.getDirection(), hit.getLocation(),
				cir.getReturnValue(), player.level().getBlockState(hit.getBlockPos()), player.getItemInHand(hand));
		}
	}
}
