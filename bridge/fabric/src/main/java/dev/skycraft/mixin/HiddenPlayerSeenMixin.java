package dev.skycraft.mixin;

import dev.skycraft.portalcraft.HostWorld;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.player.Player;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Portalcraft (bridge #19): mobs don't target the hidden Minecraft player while Chell is the player (HiddenPlayerMixin). */
@Mixin(Player.class)
public abstract class HiddenPlayerSeenMixin {
	@Inject(method = "canBeSeenAsEnemy", at = @At("HEAD"), cancellable = true)
	private void portalcraft$hiddenUnseen(CallbackInfoReturnable<Boolean> cir) {
		if ((Object) this instanceof ServerPlayer && HostWorld.playerHidden()) {
			cir.setReturnValue(false);
		}
	}
}
