package dev.skycraft.mixin;

import dev.skycraft.portalcraft.HostWorld;
import net.minecraft.world.entity.Mob;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Portalcraft (bridge #19): under Portal 2's roof (the chambers are open sky to Minecraft: the chamber band sits high in
 * its air) zombies and skeletons don't burn in daylight (b19_scene3: a zombie in a chamber was on fire at noon).
 */
@Mixin(Mob.class)
public abstract class MobSunBurnMixin {
	@Inject(method = "isSunBurnTick", at = @At("HEAD"), cancellable = true)
	private void portalcraft$roofed(CallbackInfoReturnable<Boolean> cir) {
		if (HostWorld.underHostRoof(((Mob) (Object) this).blockPosition())) {
			cir.setReturnValue(false);
		}
	}
}
