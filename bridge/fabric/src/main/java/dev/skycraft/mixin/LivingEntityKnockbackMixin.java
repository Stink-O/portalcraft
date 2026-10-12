package dev.skycraft.mixin;

import dev.skycraft.combat.SkyCombat;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.LivingEntity;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Portalcraft (bridge #57): Portal 2's damage (laser, turret, goo, crush, fall) must not knock the Minecraft player about. SkyCombat.hurtPlayer
 * raises NO_KNOCKBACK around the hurtServer of such a hit; the only motion is then Portal 2's own push, sent by the plugin.
 */
@Mixin(LivingEntity.class)
public abstract class LivingEntityKnockbackMixin {
	@Inject(method = "knockback(DDDLnet/minecraft/world/damagesource/DamageSource;FZ)V", at = @At("HEAD"), cancellable = true)
	private void portalcraft$noHostKnockback(double power, double xd, double zd, DamageSource source, float damage, boolean fromEffect, CallbackInfo ci) {
		if (SkyCombat.NO_KNOCKBACK) {
			ci.cancel();
		}
	}
}
