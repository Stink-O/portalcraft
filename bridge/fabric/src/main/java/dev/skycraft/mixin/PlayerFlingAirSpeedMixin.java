package dev.skycraft.mixin;

import dev.skycraft.portalcraft.PlayerFling;
import net.minecraft.world.entity.player.Player;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Portalcraft (bridge #28): while the player flings, Minecraft's own air control is off (Portal 2's is applied instead,
 * PlayerFlingMixin): with no air drag Minecraft's 0.02 a tick would add up without a limit.
 */
@Mixin(Player.class)
public abstract class PlayerFlingAirSpeedMixin {
	@Inject(method = "getFlyingSpeed", at = @At("HEAD"), cancellable = true)
	private void portalcraft$noMcAirControl(CallbackInfoReturnable<Float> cir) {
		if (PlayerFling.flinging(this)) {
			cir.setReturnValue(0.0F);
		}
	}
}
