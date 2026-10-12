package dev.skycraft.client.mixin;

import dev.skycraft.world.SkyMovers;
import net.minecraft.client.player.LocalPlayer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Portalcraft (bridge #26): at the start of the local player's tick (after the level stored its old position, so a carry
 * is interpolated like any movement) the host's movers move on to their latest poses; the player's move then rides,
 * collides and is pushed (SkyMovers, via EntityMoverMixin). Replaces bridge #23's LocalPlayerPlatformMixin.
 */
@Mixin(LocalPlayer.class)
public abstract class LocalPlayerMoverMixin {
	@Inject(method = "tick", at = @At("HEAD"))
	private void portalcraft$movers(CallbackInfo ci) {
		SkyMovers.refresh();
		SkyMovers.setRideEntity((LocalPlayer) (Object) this);
		dev.skycraft.portalcraft.HostMechanics.tickStart((LocalPlayer) (Object) this);  // bridge #32: faith plate launch, funnel carry
	}
}
