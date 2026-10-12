package dev.skycraft.client.mixin;

import dev.skycraft.client.portalcraft.LightProbes;
import net.minecraft.client.renderer.LightmapRenderStateExtractor;
import net.minecraft.client.renderer.state.LightmapRenderState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Portalcraft: keeps the lightmap's inputs (sky/block factors, colours, brightness) each time
 * Minecraft refreshes them, so {@link LightProbes} can evaluate the lightmap on the CPU exactly as
 * the lightmap shader does, at the host's probe positions.
 */
@Mixin(LightmapRenderStateExtractor.class)
public abstract class LightmapCaptureMixin {
	@Inject(method = "extract", at = @At("TAIL"))
	private void portalcraft$capture(LightmapRenderState state, float partialTicks, CallbackInfo ci) {
		if (state.needsUpdate) {
			LightProbes.capture(state);
		}
	}
}
