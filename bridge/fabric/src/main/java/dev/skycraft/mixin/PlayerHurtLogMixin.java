package dev.skycraft.mixin;

import dev.skycraft.SkyCraft;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.damagesource.DamageSource;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Portalcraft (bridge #20): every damage the player takes is logged with its source, so tests can prove a portal transit
 * never hurts (the host's pcb_mc_hurt reads these lines from mc.log).
 */
@Mixin(ServerPlayer.class)
public abstract class PlayerHurtLogMixin {
	@Inject(method = "hurtServer", at = @At("HEAD"))
	private void portalcraft$logHurt(ServerLevel level, DamageSource source, float damage, CallbackInfoReturnable<Boolean> cir) {
		ServerPlayer self = (ServerPlayer) (Object) this;
		if (damage <= 0.0F || self.isCreative() || self.isSpectator()) {
			return;
		}
		SkyCraft.LOG.info("Portalcraft: player hurt: {} {} at {} {} {} (fall distance {})", source.getMsgId(), String.format("%.2f", damage),
			String.format("%.2f", self.getX()), String.format("%.2f", self.getY()), String.format("%.2f", self.getZ()),
			String.format("%.2f", self.fallDistance));
	}
}
