package dev.skycraft.client.mixin;

import com.mojang.blaze3d.vertex.UberGpuBuffer;
import dev.skycraft.client.portalcraft.PassSectionCache;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Portalcraft (PassSectionCache): terrain buffer allocations added, removed, or the buffer resized (slices moved). */
@Mixin(UberGpuBuffer.class)
public abstract class PassCacheUberBufferMixin {
	@Inject(method = "addAllocation", at = @At("HEAD"))
	private void portalcraft$add(CallbackInfoReturnable<Boolean> cir) {
		PassSectionCache.invalidate(5);
	}

	@Inject(method = "removeAllocation", at = @At("HEAD"))
	private void portalcraft$remove(CallbackInfo ci) {
		PassSectionCache.invalidate(6);
	}

	@Inject(method = "uploadStagedAllocations", at = @At("RETURN"))
	private void portalcraft$resized(CallbackInfoReturnable<Boolean> cir) {
		if (cir.getReturnValueZ()) {
			PassSectionCache.invalidate(7);
		}
	}
}
