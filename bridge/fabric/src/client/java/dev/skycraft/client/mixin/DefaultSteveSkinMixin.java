package dev.skycraft.client.mixin;

import java.util.UUID;
import net.minecraft.client.resources.DefaultPlayerSkin;
import net.minecraft.world.entity.player.PlayerSkin;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

// Portalcraft (lead): the offline test account (and any other player without a downloaded skin) is drawn as classic
// Steve (wide arms) instead of the default skin picked from its UUID. Set PORTALCRAFT_DEFAULT_SKIN=uuid to keep vanilla.
@Mixin(DefaultPlayerSkin.class)
public abstract class DefaultSteveSkinMixin {
	@Shadow @Final private static PlayerSkin[] DEFAULT_SKINS;
	@Unique private static PlayerSkin skycraft$steve;

	@Inject(method = "get(Ljava/util/UUID;)Lnet/minecraft/world/entity/player/PlayerSkin;", at = @At("HEAD"), cancellable = true)
	private static void skycraft$steve(UUID uuid, CallbackInfoReturnable<PlayerSkin> cir) {
		if ("uuid".equals(System.getenv("PORTALCRAFT_DEFAULT_SKIN"))) return;
		if (skycraft$steve == null)
			for (PlayerSkin s : DEFAULT_SKINS)
				if (String.valueOf(s).contains("wide/steve")) { skycraft$steve = s; break; }
		if (skycraft$steve != null) cir.setReturnValue(skycraft$steve);
	}
}
