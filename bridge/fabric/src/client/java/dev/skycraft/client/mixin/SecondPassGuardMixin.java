package dev.skycraft.client.mixin;

import dev.skycraft.client.portalcraft.HostViewClient;
import net.minecraft.client.renderer.LevelRenderer;
import net.minecraft.client.renderer.state.level.CameraRenderState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Portalcraft: during the second camera's level pass the view area (loaded sections around the
 * camera) stays centred on the primary camera, so two cameras never make it recentre back and
 * forth every frame (which would drop and recompile the sections at its edge).
 */
@Mixin(LevelRenderer.class)
public abstract class SecondPassGuardMixin {
	@Inject(method = "repositionCamera", at = @At("HEAD"), cancellable = true)
	private void portalcraft$keepViewArea(CameraRenderState camera, CallbackInfo ci) {
		if (HostViewClient.pass() != 0) {
			ci.cancel();
		}
	}
}
