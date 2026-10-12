package dev.skycraft.mixin;

import dev.skycraft.portalcraft.PlayerFling;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.server.network.ServerGamePacketListenerImpl;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Portalcraft (bridge #28): the server's copy of the player follows the client's own portal transit (PlayerTransit) with the first
 * movement packet from the exit side; packets from before the transit are dropped (PlayerFling.serverMove).
 */
@Mixin(ServerGamePacketListenerImpl.class)
public abstract class PlayerTransitServerMixin {
	@Shadow
	public ServerPlayer player;

	@Inject(method = "handlePlayerPositionChange", at = @At("HEAD"), cancellable = true)
	private void portalcraft$transit(double x, double y, double z, float yRot, float xRot, boolean onGround, boolean horizontalCollision, CallbackInfo ci) {
		ServerGamePacketListenerImpl self = (ServerGamePacketListenerImpl) (Object) this;
		if (!PlayerFling.serverMove(this.player, self::resetPosition, x, y, z)) {
			ci.cancel();
		}
	}
}
