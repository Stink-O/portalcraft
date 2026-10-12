package dev.skycraft.client.mixin;

import dev.skycraft.client.portalcraft.PassSectionCache;
import net.minecraft.client.renderer.SectionOcclusionGraph;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** Portalcraft (PassSectionCache): the section graph changed (a full or partial update, a reset). */
@Mixin(SectionOcclusionGraph.class)
public abstract class PassCacheGraphMixin {
	@Inject(method = "runUpdates", at = @At("HEAD"))
	private void portalcraft$updates(CallbackInfo ci) {
		PassSectionCache.invalidate(8);
	}

	@Inject(method = "waitAndReset", at = @At("HEAD"))
	private void portalcraft$reset(CallbackInfo ci) {
		PassSectionCache.invalidate(9);
	}
}
