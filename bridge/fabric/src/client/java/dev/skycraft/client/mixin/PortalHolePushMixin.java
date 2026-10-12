package dev.skycraft.client.mixin;

import dev.skycraft.world.PortalHoles;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.core.BlockPos;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Portalcraft (bridge #28): a player walking into a portal on Minecraft blocks (the cliff portal) is going through it, not stuck
 * in the rock: vanilla's push out of suffocating blocks (LocalPlayer.moveTowardsClosestSpace) tests the whole block cell, which
 * the hole carved for the opening only partly empties, and held the player at the portal's plane, its eye never crossing.
 * Only with a host that has Minecraft transit its player (HP_MC_TRANSIT).
 */
@Mixin(LocalPlayer.class)
public abstract class PortalHolePushMixin {
	@Inject(method = "suffocatesAt", at = @At("HEAD"), cancellable = true)
	private void portalcraft$notInRockInPortalHole(BlockPos pos, CallbackInfoReturnable<Boolean> cir) {
		LocalPlayer self = (LocalPlayer) (Object) this;
		if (dev.skycraft.client.portalcraft.AvatarHost.hostWantsTransit() && PortalHoles.intersects(self.getBoundingBox())) {
			cir.setReturnValue(false);
		}
	}
}
