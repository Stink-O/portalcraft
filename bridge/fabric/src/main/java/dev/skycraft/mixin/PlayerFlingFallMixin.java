package dev.skycraft.mixin;

import dev.skycraft.SkyCraft;
import dev.skycraft.portalcraft.PlayerFling;
import net.minecraft.core.BlockPos;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.ModifyVariable;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Portalcraft (bridge #28): no fall damage from a portal transit or from the landing that ends the fling after it (Portal 2 never
 * hurts Chell there). The server's copy of the player gathers no fall distance from the moment the client transits until it has
 * landed after that transit; other falls are Minecraft's own.
 */
@Mixin(ServerPlayer.class)
public abstract class PlayerFlingFallMixin {
	private static int portalcraft$logs;

	@ModifyVariable(method = "checkFallDamage", at = @At("HEAD"), argsOnly = true, ordinal = 0)
	private double portalcraft$noFlingFall(double ya) {
		return PlayerFling.serverImmune() ? 0.0 : ya;
	}

	@Inject(method = "checkFallDamage", at = @At("HEAD"))
	private void portalcraft$flingLanding(double ya, boolean onGround, BlockState onState, BlockPos pos, CallbackInfo ci) {
		if (!PlayerFling.serverImmune()) {
			return;
		}
		ServerPlayer self = (ServerPlayer) (Object) this;
		if (self.fallDistance > 0.0 && portalcraft$logs++ < 40) {
			SkyCraft.LOG.info("Portalcraft: fling: fall distance {} cleared (transit {}, Portal 2 has no fall damage there)", String.format("%.2f", self.fallDistance),
				PlayerFling.transitSeq);
		}
		self.resetFallDistance();
		// the client landed after the latest transit and this packet is on the ground: the fling is over for the server too
		if (onGround && PlayerFling.landedSeq == PlayerFling.transitSeq && System.nanoTime() - PlayerFling.transitNs > 300_000_000L) {
			PlayerFling.serverSeq = PlayerFling.transitSeq;
		}
	}
}
