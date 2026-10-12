package dev.skycraft.client.mixin;

import dev.skycraft.client.portalcraft.HandLight;
import net.minecraft.client.renderer.FirstPersonHandsAndItemsRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.ModifyArg;

/**
 * Portalcraft (bridge #27): the first-person hand and held item take the host's light in Portal 2's chambers, as a placed
 * block there is lit (HandLight), instead of the light of the open-sky chamber band in Minecraft's world. Both hands; only the
 * light coordinates change.
 */
@Mixin(FirstPersonHandsAndItemsRenderer.class)
public abstract class HandLightMixin {
	@ModifyArg(method = "submitHandsWithItems", at = @At(value = "INVOKE", target = "Lnet/minecraft/client/renderer/FirstPersonHandsAndItemsRenderer;submitArmWithItem(Lnet/minecraft/client/renderer/state/level/PlayerRenderState;Lnet/minecraft/client/renderer/state/level/FirstPersonHandsAndItemsRenderState;FFLnet/minecraft/world/InteractionHand;FLnet/minecraft/world/item/ItemStack;FLcom/mojang/blaze3d/vertex/PoseStack;Lnet/minecraft/client/renderer/SubmitNodeCollector;I)V"), index = 10)
	private int portalcraft$handLight(int lightCoords) {
		return HandLight.coords(lightCoords);
	}
}
