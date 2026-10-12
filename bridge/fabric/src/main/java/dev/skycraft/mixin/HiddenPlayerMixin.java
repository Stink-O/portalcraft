package dev.skycraft.mixin;

import dev.skycraft.portalcraft.HostWorld;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.damagesource.DamageTypes;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Portalcraft (bridge #19): while Chell is the player (the host's HostPlayer has no kHpMcMode), the hidden Minecraft
 * player that follows her is not there for Minecraft's world: mobs don't see it as an enemy and nothing hurts it.
 * Mobs hunt Chell's own stand-in instead (SkyCombat.huntChell).
 */
@Mixin(ServerPlayer.class)
public abstract class HiddenPlayerMixin {
	@Inject(method = "hurtServer", at = @At("HEAD"), cancellable = true)
	private void portalcraft$hiddenUnhurt(ServerLevel level, DamageSource source, float damage, CallbackInfoReturnable<Boolean> cir) {
		if (HostWorld.playerHidden() && !source.is(DamageTypes.GENERIC_KILL)) {
			cir.setReturnValue(false);
		}
	}
}
