package dev.skycraft.client.mixin;

import com.mojang.blaze3d.pipeline.RenderTarget;
import dev.skycraft.client.portalcraft.PortalPasses;
import net.minecraft.client.renderer.GameRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Portalcraft: during a portal pass (PortalPasses) the level renders into that pass's own target.
 * LevelRenderer (its clear, sky, main, transparency and outline passes, and the size of its
 * internal targets) reads the target through this getter.
 */
@Mixin(GameRenderer.class)
public abstract class GameRendererTargetMixin {
	@Inject(method = "mainRenderTarget", at = @At("HEAD"), cancellable = true)
	private void portalcraft$passTarget(CallbackInfoReturnable<RenderTarget> cir) {
		RenderTarget t = PortalPasses.targetOverride();
		if (t != null) {
			cir.setReturnValue(t);
		}
	}
}
