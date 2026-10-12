package dev.skycraft.client.mixin;

import dev.skycraft.client.portalcraft.PassSectionCache;
import net.minecraft.client.renderer.LevelRenderer;
import net.minecraft.client.renderer.culling.Frustum;
import net.minecraft.client.renderer.extract.LevelExtractor;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** Portalcraft (experiment, PassSectionCache): a pass whose camera did not move reuses its visible-section list. */
@Mixin(LevelExtractor.class)
public abstract class PassCacheExtractorMixin {
	@Shadow @Final private LevelRenderer levelRenderer;

	@Inject(method = "applyFrustum", at = @At("HEAD"), cancellable = true)
	private void portalcraft$reuseList(Frustum frustum, CallbackInfo ci) {
		if (PassSectionCache.restoreFrustum(this.levelRenderer)) {
			ci.cancel();
		}
	}

	@Inject(method = "applyFrustum", at = @At("RETURN"))
	private void portalcraft$keepList(Frustum frustum, CallbackInfo ci) {
		PassSectionCache.storeFrustum(this.levelRenderer);
	}

	@Inject(method = "allChanged", at = @At("HEAD"))
	private void portalcraft$allChanged(CallbackInfo ci) {
		PassSectionCache.invalidate(0);
	}
}
