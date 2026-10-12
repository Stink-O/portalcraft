package dev.skycraft.mixin;

import dev.skycraft.portalcraft.HostWorld;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.level.storage.LevelData;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Portalcraft (bridge #19): the Minecraft player who dies in a Portal 2 chamber respawns where its body is (a forced respawn
 * point at the death spot), not at the world spawn kilometres away: the host then restarts the chamber from its checkpoint
 * and moves the player to Portal 2's spawn, as after any level load (combat.cpp).
 */
@Mixin(ServerPlayer.class)
public abstract class RespawnAtBodyMixin {
	@Inject(method = "die", at = @At("HEAD"))
	private void portalcraft$respawnAtBody(DamageSource source, CallbackInfo ci) {
		ServerPlayer self = (ServerPlayer) (Object) this;
		if (!HostWorld.hostIsMinecraftPlayer()) {
			return;
		}
		self.setRespawnPosition(new ServerPlayer.RespawnConfig(LevelData.RespawnData.of(self.level().dimension(), self.blockPosition(), self.getYRot(), self.getXRot()), true), false);
	}
}
