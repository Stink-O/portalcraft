package dev.skycraft.client.mixin;

import dev.skycraft.client.portalcraft.PortalPasses;
import net.minecraft.client.renderer.LevelRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** Portalcraft: the extra passes' cloud and fog buffers move on once per frame, with the main ones. */
@Mixin(LevelRenderer.class)
public abstract class PortalPassFrameMixin {
	@Inject(method = "endFrame", at = @At("TAIL"))
	private void portalcraft$endFrame(CallbackInfo ci) {
		PortalPasses.endFrame();
	}
}
